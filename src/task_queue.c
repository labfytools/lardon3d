#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/task_queue.h>

#include "task_internal.h"
#define LARDON3D_TASK_QUEUE_TEST_WEAK_REFERENCE
#include "task_queue_internal.h"
#undef LARDON3D_TASK_QUEUE_TEST_WEAK_REFERENCE

typedef struct TaskNode {
    Lardon3DTask *task;
    struct TaskNode *previous_all;
    struct TaskNode *next_all;
    struct TaskNode *next_pending;
    /* Pins Task across Queue-unlocked control callbacks; only Queue mutex
     * mutates this count. */
    size_t control_users;
} TaskNode;

enum {
    LARDON3D_PENDING_RESOURCE_WAIT_MILLISECONDS = 500,
};

#define LARDON3D_TASK_QUEUE_CALL_GATE_CLOSING \
    ((size_t)1 << (sizeof(size_t) * CHAR_BIT - 1))
#define LARDON3D_TASK_QUEUE_CALL_GATE_REFERENCES \
    (LARDON3D_TASK_QUEUE_CALL_GATE_CLOSING - (size_t)1)

#if defined(LARDON3D_TASK_QUEUE_TESTING)
static void
notify_internal_test_event(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskQueueTestEvent event
)
{
#if defined(__GNUC__) || defined(__clang__)
    if (lardon3d_task_queue_internal_test_event) {
        lardon3d_task_queue_internal_test_event(queue, event);
    }
#else
    lardon3d_task_queue_internal_test_event(queue, event);
#endif
}
#endif

/* The Queue owns Task lifetime, serialized dispatch, and pending backpressure.
 * It does not own resource policy; every admission decision comes from the
 * Governor. Terminal Task objects are deliberately replaced with fixed-size
 * snapshots so task-owned working sets do not become application-lifetime
 * allocations.
 */
struct Lardon3DTaskQueue {
    /* One atomic word linearizes call registration against irreversible close.
     * The high bit is monotonic CLOSING; low bits are calls that registered
     * before it. Registered calls pin every following field, including mutex,
     * until their release is observed by destroy. */
    atomic_size_t call_gate;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_t worker;
    bool worker_started;
    bool worker_ready;
    bool stopping;
    Lardon3DResourceGovernor *governor;
    uint64_t next_id;
    /* INVARIANT: generation exhaustion is Queue-lifetime monotonic state, not
     * an overloaded numeric cursor. Restored/preassigned IDs may still enter
     * after exhaustion, but neither they nor history removal can re-arm a
     * generated ID that could wrap and collide with an earlier lifetime ID. */
    bool generated_ids_exhausted;
    TaskNode *all_head;
    TaskNode *all_tail;
    TaskNode *pending_head;
    TaskNode *pending_tail;
    TaskNode *active;
    Lardon3DTaskObservation history[LARDON3D_TASK_QUEUE_HISTORY_CAPACITY];
    size_t history_count;
    /* Lifetime telemetry is saturating and never decremented by history
     * eviction/removal; it owns no Task or userdata. */
    size_t terminal_count;
    size_t capacity;
    size_t pending_count;
    size_t active_producers;
};

static bool
terminal_state(Lardon3DTaskState state)
{
    return state == TASK_CANCELLED || state == TASK_FAILED
        || state == TASK_COMPLETED;
}

static void
observation_to_snapshot(
    const Lardon3DTaskObservation *observation,
    Lardon3DTaskSnapshot *snapshot
)
{
    *snapshot = (Lardon3DTaskSnapshot) {
        .id = observation->id,
        .progress = observation->progress,
        .state = observation->state,
        .started_at = observation->started_at,
        .finished_at = observation->finished_at,
    };
    (void)snprintf(snapshot->name, sizeof(snapshot->name), "%s",
        observation->name);
    (void)snprintf(snapshot->message, sizeof(snapshot->message), "%s",
        observation->message);
}

static size_t
saturating_add(size_t left, size_t right)
{
    return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}

static size_t
call_gate_references(size_t state)
{
    return state & LARDON3D_TASK_QUEUE_CALL_GATE_REFERENCES;
}

/* CONTRACT: successful CAS is the public-call lifetime linearization point.
 * The acq_rel RMW and destroy's acq_rel close occupy one modification order:
 * registration either precedes close and must be awaited, or observes the
 * monotonic CLOSING bit and touches no mutex or Queue-owned Task. */
static bool
register_call(Lardon3DTaskQueue *queue)
{
    size_t observed = atomic_load_explicit(
        &queue->call_gate, memory_order_acquire);
    for (;;) {
        if ((observed & LARDON3D_TASK_QUEUE_CALL_GATE_CLOSING) != 0
            || call_gate_references(observed)
                == LARDON3D_TASK_QUEUE_CALL_GATE_REFERENCES) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &queue->call_gate,
                &observed,
                observed + (size_t)1,
                memory_order_acq_rel,
                memory_order_acquire
            )) {
#if defined(LARDON3D_TASK_QUEUE_TESTING)
            notify_internal_test_event(
                queue, LARDON3D_TASK_QUEUE_TEST_CALL_REGISTERED);
#endif
            return true;
        }
    }
}

/* Caller owns queue->mutex. A release that removes the final reference pairs
 * with destroy's acquire load; the condition signal supplies the blocking
 * handoff while the atomic remains the authoritative lifetime count. */
static void
release_call_locked(Lardon3DTaskQueue *queue)
{
    size_t previous = atomic_fetch_sub_explicit(
        &queue->call_gate, (size_t)1, memory_order_release);
    if (call_gate_references(previous) == 1) {
        (void)pthread_cond_broadcast(&queue->not_empty);
    }
}

static bool
begin_call(Lardon3DTaskQueue *queue)
{
    if (!register_call(queue)) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    if (queue->stopping) {
        release_call_locked(queue);
        (void)pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    return true;
}

/* Caller still owns queue->mutex. */
static void
end_call_locked(Lardon3DTaskQueue *queue)
{
    release_call_locked(queue);
    (void)pthread_mutex_unlock(&queue->mutex);
}

static TaskNode *
find_live_locked(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        if (lardon3d_task_id(node->task) == task_id) {
            return node;
        }
    }
    return NULL;
}

static bool
find_history_locked(
    const Lardon3DTaskQueue *queue,
    uint64_t task_id,
    size_t *index
)
{
    for (size_t current = 0; current < queue->history_count; ++current) {
        if (queue->history[current].id == task_id) {
            if (index) {
                *index = current;
            }
            return true;
        }
    }
    return false;
}

static void
remove_history_locked(Lardon3DTaskQueue *queue, size_t index)
{
    if (index + 1 < queue->history_count) {
        memmove(
            &queue->history[index],
            &queue->history[index + 1],
            (queue->history_count - index - 1) * sizeof(queue->history[0])
        );
    }
    --queue->history_count;
}

static void
append_history_locked(
    Lardon3DTaskQueue *queue,
    const Lardon3DTaskObservation *observation
)
{
    /* INVARIANT: history is oldest-to-newest internally. At the fixed TUI
     * bound, deterministic left eviction avoids a second allocator/failure
     * path exactly when the real Task must be released promptly. */
    if (queue->history_count == LARDON3D_TASK_QUEUE_HISTORY_CAPACITY) {
        memmove(
            &queue->history[0],
            &queue->history[1],
            (LARDON3D_TASK_QUEUE_HISTORY_CAPACITY - 1)
                * sizeof(queue->history[0])
        );
        --queue->history_count;
    }
    queue->history[queue->history_count++] = *observation;
}

static void
unlink_all_locked(Lardon3DTaskQueue *queue, TaskNode *node)
{
    if (node->previous_all) {
        node->previous_all->next_all = node->next_all;
    } else {
        queue->all_head = node->next_all;
    }
    if (node->next_all) {
        node->next_all->previous_all = node->previous_all;
    } else {
        queue->all_tail = node->previous_all;
    }
    node->previous_all = NULL;
    node->next_all = NULL;
}

static void
unlink_pending(Lardon3DTaskQueue *queue, TaskNode *previous, TaskNode *node)
{
    if (previous) {
        previous->next_pending = node->next_pending;
    } else {
        queue->pending_head = node->next_pending;
    }
    if (queue->pending_tail == node) {
        queue->pending_tail = previous;
    }
    node->next_pending = NULL;
    --queue->pending_count;
    /* Every removal frees one slot. Multiple producers may be asleep while
     * the worker removes several Tasks before they reacquire the mutex, so
     * every removal must signal one waiter. */
    (void)pthread_cond_signal(&queue->not_full);
}

static void
unlink_pending_if_present_locked(Lardon3DTaskQueue *queue, TaskNode *node)
{
    TaskNode *previous = NULL;
    TaskNode *current = queue->pending_head;
    while (current && current != node) {
        previous = current;
        current = current->next_pending;
    }
    if (current) {
        unlink_pending(queue, previous, current);
    }
}

/* CONTRACT: this is the sole live-Task -> terminal-record transition. The
 * caller proves the finished callback has returned and later destroys the
 * detached node without queue->mutex. active/control users prevent detachment
 * while another thread can still dereference the Task. */
static TaskNode *
retire_terminal_locked(
    Lardon3DTaskQueue *queue,
    TaskNode *node,
    bool retain_history
)
{
    Lardon3DTaskObservation observation;
    if (!node || node == queue->active || node->control_users != 0
        || !lardon3d_task_observation(node->task, &observation)
        || !terminal_state(observation.state)) {
        return NULL;
    }
    unlink_pending_if_present_locked(queue, node);
    unlink_all_locked(queue, node);
    queue->terminal_count = saturating_add(queue->terminal_count, 1);
    if (retain_history) {
        append_history_locked(queue, &observation);
    }
    (void)pthread_cond_broadcast(&queue->not_empty);
    return node;
}

static void
push_retired(TaskNode **retired, TaskNode *node)
{
    if (!node) {
        return;
    }
    node->next_all = *retired;
    *retired = node;
}

static void
destroy_retired(TaskNode *retired)
{
    while (retired) {
        TaskNode *next = retired->next_all;
        /* WHY: Task destruction may join execution and invokes userdata
         * destruction. It must never run while Queue observers/producers are
         * excluded by queue->mutex. */
        lardon3d_task_destroy(retired->task);
        free(retired);
        retired = next;
    }
}

/* Scans pending Tasks in FIFO order and selects the first admissible one.
 * A resource WAIT may therefore let a later Task run without removing the
 * waiting Task from its position. Terminal or rejected Tasks are removed.
 * A resource-waiting Task remains queued for later re-evaluation.
 * Returns NULL when no Task can start immediately. */
static TaskNode *
select_admissible(
    Lardon3DTaskQueue *queue,
    Lardon3DResourceReservation **reservation,
    bool *resource_wait_pending,
    bool *reject_selected,
    char reject_message[LARDON3D_TASK_MESSAGE_CAPACITY],
    TaskNode **retired
)
{
    *resource_wait_pending = false;
    *reject_selected = false;
    TaskNode *previous = NULL;
    TaskNode *node = queue->pending_head;
    while (node) {
        TaskNode *next = node->next_pending;
        Lardon3DTaskSnapshot task_snapshot;
        if (!lardon3d_task_snapshot(node->task, &task_snapshot)) {
            unlink_pending(queue, previous, node);
            *reject_selected = true;
            (void)snprintf(
                reject_message,
                LARDON3D_TASK_MESSAGE_CAPACITY,
                "%s",
                "Impossible d'observer l'état de la tâche."
            );
            return node;
        }
        if (terminal_state(task_snapshot.state)) {
            unlink_pending(queue, previous, node);
            push_retired(retired, retire_terminal_locked(queue, node, true));
            node = next;
            continue;
        }
        Lardon3DResourceDecision decision;
        Lardon3DResourceReservation *candidate = NULL;
        bool evaluated = lardon3d_task_internal_reserve_available(
            node->task,
            queue->governor,
            &decision,
            &candidate
        );
        if (!evaluated) {
            unlink_pending(queue, previous, node);
            *reject_selected = true;
            (void)snprintf(
                reject_message,
                LARDON3D_TASK_MESSAGE_CAPACITY,
                "%s",
                "Impossible d'évaluer les ressources disponibles."
            );
            return node;
        }
        if (decision.kind == LARDON3D_RESOURCE_WAIT) {
            *resource_wait_pending = true;
            previous = node;
            node = next;
            continue;
        }
        if (decision.kind == LARDON3D_RESOURCE_REJECT || !candidate) {
            unlink_pending(queue, previous, node);
            *reservation = candidate;
            *reject_selected = true;
            (void)snprintf(
                reject_message,
                LARDON3D_TASK_MESSAGE_CAPACITY,
                "%s",
                decision.reason
            );
            return node;
        }
        unlink_pending(queue, previous, node);
        *reservation = candidate;
        return node;
    }
    return NULL;
}

static void
wait_for_pending_change(Lardon3DTaskQueue *queue, bool resource_wait_pending)
{
    if (!resource_wait_pending) {
        (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
        return;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        return;
    }
    deadline.tv_nsec +=
        LARDON3D_PENDING_RESOURCE_WAIT_MILLISECONDS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(
        &queue->not_empty,
        &queue->mutex,
        &deadline
    );
}

static void *
queue_worker(void *context)
{
    Lardon3DTaskQueue *queue = context;
    /* Only this thread owns heavy Task callbacks. Applying the Governor mask
     * here keeps the caller/main/TUI thread unconstrained; children created by
     * OpenCV, Matcher, or the Vulkan driver inherit the bounded worker mask. */
    (void)lardon3d_resource_governor_internal_apply_worker_affinity(
        queue->governor);
    (void)pthread_mutex_lock(&queue->mutex);
    queue->worker_ready = true;
    (void)pthread_cond_broadcast(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->mutex);
    /* Single worker only; execution is serialized by design. Queue preserves
     * pending FIFO order with adaptive dispatch/backpressure; Governor decides
     * admission.
     */
    for (;;) {
        (void)pthread_mutex_lock(&queue->mutex);
        while (!queue->stopping && !queue->pending_head) {
            (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
        }
        if (queue->stopping) {
            (void)pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        Lardon3DResourceReservation *reservation = NULL;
        bool resource_wait_pending;
        bool reject_selected;
        char reject_message[LARDON3D_TASK_MESSAGE_CAPACITY] = {0};
        TaskNode *retired = NULL;
        TaskNode *selected = select_admissible(
            queue,
            &reservation,
            &resource_wait_pending,
            &reject_selected,
            reject_message,
            &retired
        );
        if (!selected) {
            if (!retired) {
                wait_for_pending_change(queue, resource_wait_pending);
            }
            (void)pthread_mutex_unlock(&queue->mutex);
            destroy_retired(retired);
            continue;
        }
        queue->active = selected;
        (void)pthread_mutex_unlock(&queue->mutex);
        destroy_retired(retired);

        if (reject_selected) {
            /* Admission rejection can invoke the durable finished callback.
             * It runs without Queue mutex held and is therefore fully complete
             * before the Task is converted to lightweight history below.
             * Unlocked permits observation, not self-removal/destruction: those
             * operations depend on this callback returning. */
            (void)lardon3d_task_reject(selected->task, reject_message);
        } else {
            /* Policy may change between Tasks. Reapply and verify only on this
             * worker; failure is recorded by the Governor and execution
             * continues under conservative compute-count admission. */
            (void)lardon3d_resource_governor_internal_apply_worker_affinity(
                queue->governor);
            if (!lardon3d_task_start(
                    selected->task, queue->governor, reservation)) {
                (void)lardon3d_task_reject(
                    selected->task,
                    "Réservation de ressources invalide."
                );
            }
        }
        /* The Task may have released and re-reserved through sequence_break
         * during its callback. In that case the original reservation is
         * already released and this call is a no-op; lardon3d_task_start
         * released the Task's current reservation. */
        if (reservation) {
            (void)lardon3d_resource_governor_release(
                queue->governor,
                reservation
            );
        }

        (void)pthread_mutex_lock(&queue->mutex);
        queue->active = NULL;
        retired = retire_terminal_locked(queue, selected, true);
        (void)pthread_cond_broadcast(&queue->not_empty);
        (void)pthread_mutex_unlock(&queue->mutex);
        destroy_retired(retired);
    }
}

Lardon3DTaskQueue *
lardon3d_task_queue_create(Lardon3DResourceGovernor *governor, size_t capacity)
{
    if (!governor || capacity < 1) {
        return NULL;
    }
    Lardon3DTaskQueue *queue = calloc(1, sizeof(*queue));
    if (!queue) {
        return NULL;
    }
    atomic_init(&queue->call_gate, 0);
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue);
        return NULL;
    }
    pthread_condattr_t not_empty_attributes;
    if (pthread_condattr_init(&not_empty_attributes) != 0) {
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    if (pthread_condattr_setclock(
            &not_empty_attributes,
            CLOCK_MONOTONIC
        ) != 0
        || pthread_cond_init(
            &queue->not_empty,
            &not_empty_attributes
        ) != 0) {
        (void)pthread_condattr_destroy(&not_empty_attributes);
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    (void)pthread_condattr_destroy(&not_empty_attributes);
    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->next_id = 1;
    queue->governor = governor;
    queue->capacity = capacity;
    if (pthread_create(&queue->worker, NULL, queue_worker, queue) != 0) {
        (void)pthread_cond_destroy(&queue->not_full);
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->worker_started = true;
    (void)pthread_mutex_lock(&queue->mutex);
    while (!queue->worker_ready) {
        (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    return queue;
}

bool
lardon3d_task_queue_cancel(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) {
        return false;
    }
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *node = find_live_locked(queue, task_id);
    if (!node) {
        bool retained = find_history_locked(queue, task_id, NULL);
        end_call_locked(queue);
        return retained;
    }
    /* request_cancel may synchronously execute the finished callback. Pin the
     * node, release Queue mutex for that potentially blocking durability work,
     * then retire only after the callback has returned. */
    ++node->control_users;
    (void)pthread_mutex_unlock(&queue->mutex);
    lardon3d_task_request_cancel(node->task);
    (void)pthread_mutex_lock(&queue->mutex);
    --node->control_users;
    TaskNode *retired = retire_terminal_locked(queue, node, true);
    (void)pthread_cond_broadcast(&queue->not_empty);
    if (retired) {
        /* Keep this public call registered while destruction runs unlocked, so
         * Queue destroy cannot free the Queue before this caller and its
         * userdata destructor have fully returned. */
        (void)pthread_mutex_unlock(&queue->mutex);
        destroy_retired(retired);
        (void)pthread_mutex_lock(&queue->mutex);
    }
    end_call_locked(queue);
    return true;
}

bool
lardon3d_task_queue_pause(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) return false;
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *node = find_live_locked(queue, task_id);
    bool paused = node && lardon3d_task_pause(node->task);
    end_call_locked(queue);
    return paused;
}

bool
lardon3d_task_queue_resume(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) return false;
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *node = find_live_locked(queue, task_id);
    bool resumed = node && lardon3d_task_resume(node->task);
    if (resumed) (void)pthread_cond_broadcast(&queue->not_empty);
    end_call_locked(queue);
    return resumed;
}

void
lardon3d_task_queue_resources_changed(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return;
    }
    if (!begin_call(queue)) {
        return;
    }
    (void)pthread_cond_broadcast(&queue->not_empty);
    end_call_locked(queue);
}

void
lardon3d_task_queue_destroy(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return;
    }
    /* Closing is monotonic and shares the registration modification order.
     * The application must already have prevented new API invocations; this
     * gate resolves only calls whose registration races with this close. */
    size_t previous_gate = atomic_fetch_or_explicit(
        &queue->call_gate,
        LARDON3D_TASK_QUEUE_CALL_GATE_CLOSING,
        memory_order_acq_rel
    );
    if ((previous_gate & LARDON3D_TASK_QUEUE_CALL_GATE_CLOSING) != 0) {
        /* Concurrent/repeated destruction is outside the raw-pointer lifetime
         * contract. If it overlaps while memory is still live, only the call
         * that linearized CLOSING owns cancellation and physical destruction. */
        return;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    queue->stopping = true;
    (void)pthread_cond_broadcast(&queue->not_empty);
    (void)pthread_cond_broadcast(&queue->not_full);
#if defined(LARDON3D_TASK_QUEUE_TESTING)
    notify_internal_test_event(queue, LARDON3D_TASK_QUEUE_TEST_CLOSING);
#endif
    while (call_gate_references(atomic_load_explicit(
               &queue->call_gate, memory_order_acquire)) != 0
        || queue->active_producers > 0) {
        (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    /* Pin every remaining node while cancellation runs without Queue mutex.
     * Pending cancellation may execute a durable finished callback; active
     * execution may finish concurrently, but the worker cannot retire a pin. */
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        ++node->control_users;
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        lardon3d_task_request_cancel(node->task);
    }
    (void)pthread_mutex_lock(&queue->mutex);
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        --node->control_users;
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    if (queue->worker_started) {
        (void)pthread_join(queue->worker, NULL);
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    queue->all_head = NULL;
    queue->all_tail = NULL;
    queue->pending_head = NULL;
    queue->pending_tail = NULL;
    queue->pending_count = 0;
    (void)pthread_mutex_unlock(&queue->mutex);
    destroy_retired(node);
    (void)pthread_cond_destroy(&queue->not_full);
    (void)pthread_cond_destroy(&queue->not_empty);
    (void)pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

/* Called with the Queue mutex held. Does not signal not_empty on failure. */
static bool
enqueue_locked(
    Lardon3DTaskQueue *queue,
    TaskNode *node,
    Lardon3DTask *task,
    uint64_t *task_id
)
{
    uint64_t id = lardon3d_task_id(task);
    if (queue->stopping) {
        return false;
    }
    for (TaskNode *existing = queue->all_head; existing;
         existing = existing->next_all) {
        if (id != 0 && lardon3d_task_id(existing->task) == id) {
            return false;
        }
    }
    if (id != 0 && find_history_locked(queue, id, NULL)) {
        return false;
    }
    if (id == 0) {
        if (queue->generated_ids_exhausted
            || !lardon3d_task_assign_id(task, queue->next_id)) {
            return false;
        }
        id = queue->next_id;
        if (id == UINT64_MAX) {
            queue->generated_ids_exhausted = true;
        } else {
            queue->next_id = id + 1;
        }
    } else if (!queue->generated_ids_exhausted && id >= queue->next_id) {
        if (id == UINT64_MAX) {
            queue->generated_ids_exhausted = true;
        } else {
            queue->next_id = id + 1;
        }
    }
    node->task = task;
    node->previous_all = queue->all_tail;
    if (queue->all_tail) {
        queue->all_tail->next_all = node;
    } else {
        queue->all_head = node;
    }
    queue->all_tail = node;
    if (queue->pending_tail) {
        queue->pending_tail->next_pending = node;
    } else {
        queue->pending_head = node;
    }
    queue->pending_tail = node;
    ++queue->pending_count;
    if (task_id) {
        *task_id = id;
    }
    (void)pthread_cond_signal(&queue->not_empty);
    return true;
}

bool
lardon3d_task_queue_add(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
)
{
    if (!queue || !task) {
        return false;
    }
    TaskNode *node = calloc(1, sizeof(*node));
    if (!node) {
        return false;
    }
    if (!begin_call(queue)) {
        free(node);
        return false;
    }
    ++queue->active_producers;
    while (!queue->stopping && queue->pending_count >= queue->capacity) {
#if defined(LARDON3D_TASK_QUEUE_TESTING)
        notify_internal_test_event(
            queue, LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING);
#endif
        (void)pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
    --queue->active_producers;
    (void)pthread_cond_broadcast(&queue->not_empty);
    bool accepted = enqueue_locked(queue, node, task, task_id);
    end_call_locked(queue);
    if (!accepted) {
        free(node);
    }
    return accepted;
}

bool
lardon3d_task_queue_try_add(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
)
{
    return lardon3d_task_queue_try_add_ex(queue, task, task_id)
        == LARDON3D_TASK_QUEUE_ADD_OK;
}

Lardon3DTaskQueueAddResult
lardon3d_task_queue_try_add_ex(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
)
{
    if (!queue || !task) {
        return LARDON3D_TASK_QUEUE_ADD_ERROR;
    }
    TaskNode *node = calloc(1, sizeof(*node));
    if (!node) {
        return LARDON3D_TASK_QUEUE_ADD_ERROR;
    }
    if (!begin_call(queue)) {
        free(node);
        return LARDON3D_TASK_QUEUE_ADD_STOPPING;
    }
    Lardon3DTaskQueueAddResult result = LARDON3D_TASK_QUEUE_ADD_OK;
    if (queue->stopping) {
        result = LARDON3D_TASK_QUEUE_ADD_STOPPING;
    } else if (queue->pending_count >= queue->capacity) {
        result = LARDON3D_TASK_QUEUE_ADD_FULL;
    } else {
        uint64_t id = lardon3d_task_id(task);
        for (TaskNode *existing = queue->all_head; existing;
             existing = existing->next_all) {
            if (id != 0 && lardon3d_task_id(existing->task) == id) {
                result = LARDON3D_TASK_QUEUE_ADD_DUPLICATE_ID;
                break;
            }
        }
        if (result == LARDON3D_TASK_QUEUE_ADD_OK && id != 0
            && find_history_locked(queue, id, NULL)) {
            result = LARDON3D_TASK_QUEUE_ADD_DUPLICATE_ID;
        }
        if (result == LARDON3D_TASK_QUEUE_ADD_OK
            && !enqueue_locked(queue, node, task, task_id)) {
            result = LARDON3D_TASK_QUEUE_ADD_ERROR;
        }
    }
    end_call_locked(queue);
    if (result != LARDON3D_TASK_QUEUE_ADD_OK) {
        free(node);
    }
    return result;
}

bool
lardon3d_task_queue_remove(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) {
        return false;
    }
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *retired = NULL;
    for (;;) {
        TaskNode *node = find_live_locked(queue, task_id);
        if (!node) {
            size_t history_index;
            bool retained = find_history_locked(queue, task_id, &history_index);
            if (retained) {
                remove_history_locked(queue, history_index);
            }
            end_call_locked(queue);
            return retained;
        }
        Lardon3DTaskSnapshot snapshot;
        if (!lardon3d_task_snapshot(node->task, &snapshot)
            || !terminal_state(snapshot.state)) {
            end_call_locked(queue);
            return false;
        }
        if (node == queue->active || node->control_users != 0) {
            (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
            if (queue->stopping) {
                end_call_locked(queue);
                return false;
            }
            continue;
        }
        /* Explicit removal still contributes to lifetime summary, but skips
         * insertion into recent history. */
        retired = retire_terminal_locked(queue, node, false);
        bool removed = retired != NULL;
        if (retired) {
            (void)pthread_mutex_unlock(&queue->mutex);
            destroy_retired(retired);
            (void)pthread_mutex_lock(&queue->mutex);
        }
        end_call_locked(queue);
        return removed;
    }
}

size_t
lardon3d_task_queue_count(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return 0;
    }
    if (!begin_call(queue)) {
        return 0;
    }
    size_t count = queue->pending_count;
    end_call_locked(queue);
    return count;
}

bool
lardon3d_task_queue_get(
    Lardon3DTaskQueue *queue,
    uint64_t task_id,
    Lardon3DTaskSnapshot *snapshot
)
{
    if (!queue || task_id == 0 || !snapshot) {
        return false;
    }
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *node = find_live_locked(queue, task_id);
    bool found = node && lardon3d_task_snapshot(node->task, snapshot);
    if (!found) {
        size_t history_index;
        found = find_history_locked(queue, task_id, &history_index);
        if (found) {
            observation_to_snapshot(&queue->history[history_index], snapshot);
        }
    }
    end_call_locked(queue);
    return found;
}

bool
lardon3d_task_queue_get_observation(
    Lardon3DTaskQueue *queue,
    uint64_t task_id,
    Lardon3DTaskObservation *observation
)
{
    if (observation) {
        *observation = (Lardon3DTaskObservation) {0};
    }
    if (!queue || task_id == 0 || !observation) {
        return false;
    }
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *node = find_live_locked(queue, task_id);
    bool found = node && lardon3d_task_observation(node->task, observation);
    if (!found) {
        size_t history_index;
        found = find_history_locked(queue, task_id, &history_index);
        if (found) {
            *observation = queue->history[history_index];
        }
    }
    end_call_locked(queue);
    return found;
}

bool
lardon3d_task_queue_get_at(
    Lardon3DTaskQueue *queue,
    size_t index,
    Lardon3DTaskSnapshot *snapshot
)
{
    if (!queue || !snapshot) {
        return false;
    }
    if (!begin_call(queue)) {
        return false;
    }
    TaskNode *node = queue->all_tail;
    while (node && index > 0) {
        node = node->previous_all;
        --index;
    }
    bool found = node && lardon3d_task_snapshot(node->task, snapshot);
    if (!node && index < queue->history_count) {
        observation_to_snapshot(
            &queue->history[queue->history_count - index - 1], snapshot);
        found = true;
    }
    end_call_locked(queue);
    return found;
}

size_t
lardon3d_task_queue_snapshot(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskSnapshot *snapshots,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
)
{
    if (summary) {
        *summary = (Lardon3DTaskQueueSummary) {0};
    }
    if (!queue || (!snapshots && capacity > 0)) {
        return 0;
    }
    if (!begin_call(queue)) {
        return 0;
    }
    size_t copied = 0;
    for (TaskNode *node = queue->all_tail; node; node = node->previous_all) {
        Lardon3DTaskSnapshot snapshot;
        if (!lardon3d_task_snapshot(node->task, &snapshot)) {
            continue;
        }
        if (copied < capacity) {
            snapshots[copied++] = snapshot;
        }
    }
    for (size_t offset = 0; offset < queue->history_count; ++offset) {
        if (copied < capacity) {
            snapshots[copied++] =
                (Lardon3DTaskSnapshot) {0};
            observation_to_snapshot(
                &queue->history[queue->history_count - offset - 1],
                &snapshots[copied - 1]);
        }
    }
    if (summary) {
        size_t live_terminal = 0;
        for (TaskNode *node = queue->all_head; node; node = node->next_all) {
            Lardon3DTaskSnapshot snapshot;
            if (!lardon3d_task_snapshot(node->task, &snapshot)) {
                continue;
            }
            if (snapshot.state == TASK_RUNNING || snapshot.state == TASK_PAUSED) {
                summary->running = saturating_add(summary->running, 1);
            } else if (snapshot.state == TASK_PENDING) {
                summary->pending = saturating_add(summary->pending, 1);
            } else {
                live_terminal = saturating_add(live_terminal, 1);
            }
        }
        summary->completed = saturating_add(
            queue->terminal_count, live_terminal);
        summary->total = saturating_add(
            summary->completed,
            saturating_add(summary->running, summary->pending)
        );
    }
    end_call_locked(queue);
    return copied;
}

size_t
lardon3d_task_queue_observe(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskObservation *observations,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
)
{
    if (summary) {
        *summary = (Lardon3DTaskQueueSummary) {0};
    }
    if (!queue || (!observations && capacity > 0)) {
        return 0;
    }
    if (!begin_call(queue)) {
        return 0;
    }
    size_t copied = 0;
    for (TaskNode *node = queue->all_tail; node; node = node->previous_all) {
        Lardon3DTaskObservation observation;
        if (!lardon3d_task_observation(node->task, &observation)) {
            continue;
        }
        if (copied < capacity) {
            observations[copied++] = observation;
        }
    }
    for (size_t offset = 0; offset < queue->history_count; ++offset) {
        if (copied < capacity) {
            observations[copied++] =
                queue->history[queue->history_count - offset - 1];
        }
    }
    if (summary) {
        size_t live_terminal = 0;
        for (TaskNode *node = queue->all_head; node; node = node->next_all) {
            Lardon3DTaskSnapshot snapshot;
            if (!lardon3d_task_snapshot(node->task, &snapshot)) {
                continue;
            }
            if (snapshot.state == TASK_RUNNING || snapshot.state == TASK_PAUSED) {
                summary->running = saturating_add(summary->running, 1);
            } else if (snapshot.state == TASK_PENDING) {
                summary->pending = saturating_add(summary->pending, 1);
            } else {
                live_terminal = saturating_add(live_terminal, 1);
            }
        }
        summary->completed = saturating_add(
            queue->terminal_count, live_terminal);
        summary->total = saturating_add(
            summary->completed,
            saturating_add(summary->running, summary->pending)
        );
    }
    end_call_locked(queue);
    return copied;
}
