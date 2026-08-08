#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lardon3d/task_queue.h>

typedef struct TaskNode {
    Lardon3DTask *task;
    struct TaskNode *next_all;
    struct TaskNode *next_pending;
} TaskNode;

struct Lardon3DTaskQueue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_t worker;
    bool worker_started;
    bool stopping;
    Lardon3DResourceGovernor *governor;
    uint64_t next_id;
    TaskNode *all_head;
    TaskNode *all_tail;
    TaskNode *pending_head;
    TaskNode *pending_tail;
    Lardon3DTask *active;
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
unlink_pending(Lardon3DTaskQueue *queue, TaskNode *previous, TaskNode *node)
{
    bool was_full = queue->pending_count >= queue->capacity;
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
    if (was_full) {
        (void)pthread_cond_signal(&queue->not_full);
    }
}

/* Parcourt la file d'attente et sélectionne la première tâche admissible.
 * Les tâches terminales ou refusées sont retirées de la file d'attente.
 * Une tâche en attente de ressources reste en file et sera réévaluée.
 * Retourne NULL si aucune tâche ne peut démarrer immédiatement. */
static Lardon3DTask *
select_admissible(
    Lardon3DTaskQueue *queue,
    Lardon3DResourceReservation **reservation
)
{
    TaskNode *previous = NULL;
    TaskNode *node = queue->pending_head;
    while (node) {
        TaskNode *next = node->next_pending;
        Lardon3DTaskSnapshot task_snapshot;
        if (!lardon3d_task_snapshot(node->task, &task_snapshot)) {
            unlink_pending(queue, previous, node);
            node = next;
            continue;
        }
        if (terminal_state(task_snapshot.state)) {
            unlink_pending(queue, previous, node);
            node = next;
            continue;
        }
        Lardon3DResourceEstimate estimate;
        Lardon3DResourceDecision decision;
        Lardon3DResourceReservation *candidate = NULL;
        bool evaluated = lardon3d_task_resource_estimate(node->task, &estimate)
            && lardon3d_resource_governor_reserve_available(
                queue->governor,
                &estimate,
                &decision,
                &candidate
            );
        if (!evaluated) {
            (void)lardon3d_task_reject(
                node->task,
                "Impossible d'évaluer les ressources disponibles."
            );
            unlink_pending(queue, previous, node);
            node = next;
            continue;
        }
        if (decision.kind == LARDON3D_RESOURCE_WAIT) {
            previous = node;
            node = next;
            continue;
        }
        if (decision.kind == LARDON3D_RESOURCE_REJECT || !candidate) {
            (void)lardon3d_task_reject(node->task, decision.reason);
            unlink_pending(queue, previous, node);
            node = next;
            continue;
        }
        unlink_pending(queue, previous, node);
        *reservation = candidate;
        return node->task;
    }
    return NULL;
}

static void *
queue_worker(void *context)
{
    Lardon3DTaskQueue *queue = context;
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
        Lardon3DTask *selected = select_admissible(queue, &reservation);
        if (!selected) {
            (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
            (void)pthread_mutex_unlock(&queue->mutex);
            continue;
        }
        queue->active = selected;
        (void)pthread_mutex_unlock(&queue->mutex);

        if (!lardon3d_task_start(selected, queue->governor, reservation)) {
            (void)lardon3d_task_reject(
                selected,
                "Réservation de ressources invalide."
            );
        }
        /* La tâche peut avoir libéré et re-réservé via sequence_break pendant
         * son callback. Dans ce cas la réservation d'origine est déjà libérée
         * et cet appel est sans effet ; la réservation courante de la tâche a
         * été libérée par lardon3d_task_start. */
        (void)lardon3d_resource_governor_release(
            queue->governor,
            reservation
        );

        (void)pthread_mutex_lock(&queue->mutex);
        queue->active = NULL;
        (void)pthread_cond_broadcast(&queue->not_empty);
        (void)pthread_mutex_unlock(&queue->mutex);
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
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
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
    return queue;
}

bool
lardon3d_task_queue_cancel(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    while (node && lardon3d_task_id(node->task) != task_id) {
        node = node->next_all;
    }
    if (node) {
        Lardon3DTaskSnapshot task_snapshot;
        bool was_pending = lardon3d_task_snapshot(node->task, &task_snapshot)
            && task_snapshot.state != TASK_RUNNING
            && task_snapshot.state != TASK_PAUSED;
        lardon3d_task_request_cancel(node->task);
        if (was_pending && queue->pending_count == queue->capacity) {
            (void)pthread_cond_signal(&queue->not_full);
        }
        (void)pthread_cond_broadcast(&queue->not_empty);
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    return node != NULL;
}

void
lardon3d_task_queue_resources_changed(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    (void)pthread_cond_broadcast(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->mutex);
}

void
lardon3d_task_queue_destroy(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    queue->stopping = true;
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        lardon3d_task_request_cancel(node->task);
    }
    (void)pthread_cond_broadcast(&queue->not_empty);
    (void)pthread_cond_broadcast(&queue->not_full);
    while (queue->active_producers > 0) {
        (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    if (queue->worker_started) {
        (void)pthread_join(queue->worker, NULL);
    }
    TaskNode *node = queue->all_head;
    while (node) {
        TaskNode *next = node->next_all;
        lardon3d_task_destroy(node->task);
        free(node);
        node = next;
    }
    (void)pthread_cond_destroy(&queue->not_full);
    (void)pthread_cond_destroy(&queue->not_empty);
    (void)pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

/* Appelée sous le mutex queue. Ne signale pas not_empty sur échec. */
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
    if (id == 0) {
        if (queue->next_id == 0
            || !lardon3d_task_assign_id(task, queue->next_id)) {
            return false;
        }
        id = queue->next_id++;
    } else if (id >= queue->next_id) {
        queue->next_id = id == UINT64_MAX ? 0 : id + 1;
    }
    node->task = task;
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
    (void)pthread_mutex_lock(&queue->mutex);
    ++queue->active_producers;
    while (!queue->stopping && queue->pending_count >= queue->capacity) {
        (void)pthread_cond_wait(&queue->not_full, &queue->mutex);
    }
    --queue->active_producers;
    (void)pthread_cond_broadcast(&queue->not_empty);
    bool accepted = enqueue_locked(queue, node, task, task_id);
    (void)pthread_mutex_unlock(&queue->mutex);
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
    if (!queue || !task) {
        return false;
    }
    TaskNode *node = calloc(1, sizeof(*node));
    if (!node) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    bool accepted = queue->pending_count < queue->capacity
        && enqueue_locked(queue, node, task, task_id);
    (void)pthread_mutex_unlock(&queue->mutex);
    if (!accepted) {
        free(node);
    }
    return accepted;
}

bool
lardon3d_task_queue_remove(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *previous = NULL;
    TaskNode *node = queue->all_head;
    while (node && lardon3d_task_id(node->task) != task_id) {
        previous = node;
        node = node->next_all;
    }
    Lardon3DTaskSnapshot snapshot;
    if (!node || !lardon3d_task_snapshot(node->task, &snapshot)
        || !terminal_state(snapshot.state)) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    while (node->task == queue->active) {
        (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    if (previous) {
        previous->next_all = node->next_all;
    } else {
        queue->all_head = node->next_all;
    }
    if (queue->all_tail == node) {
        queue->all_tail = previous;
    }
    TaskNode *pending_previous = NULL;
    TaskNode *pending = queue->pending_head;
    while (pending && pending != node) {
        pending_previous = pending;
        pending = pending->next_pending;
    }
    if (pending) {
        unlink_pending(queue, pending_previous, pending);
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    lardon3d_task_destroy(node->task);
    free(node);
    return true;
}

size_t
lardon3d_task_queue_count(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return 0;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    size_t count = queue->pending_count;
    (void)pthread_mutex_unlock(&queue->mutex);
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
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    while (node && lardon3d_task_id(node->task) != task_id) {
        node = node->next_all;
    }
    bool found = node && lardon3d_task_snapshot(node->task, snapshot);
    (void)pthread_mutex_unlock(&queue->mutex);
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
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    while (node && index > 0) {
        node = node->next_all;
        --index;
    }
    bool found = node && lardon3d_task_snapshot(node->task, snapshot);
    (void)pthread_mutex_unlock(&queue->mutex);
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
    (void)pthread_mutex_lock(&queue->mutex);
    size_t copied = 0;
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        Lardon3DTaskSnapshot snapshot;
        if (!lardon3d_task_snapshot(node->task, &snapshot)) {
            continue;
        }
        if (summary) {
            ++summary->total;
            if (snapshot.state == TASK_RUNNING || snapshot.state == TASK_PAUSED) {
                ++summary->running;
            } else if (snapshot.state == TASK_PENDING) {
                ++summary->pending;
            } else {
                ++summary->completed;
            }
        }
        if (copied < capacity) {
            snapshots[copied++] = snapshot;
        }
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    return copied;
}
