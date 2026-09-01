#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/task_queue.h>

#include "../src/resource_governor_internal.h"
#include "../src/task_internal.h"
#include "../src/task_queue_internal.h"
#include "resource_snapshot_test_utils.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

enum {
    TASK_COUNT = 400,
};

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    Lardon3DTaskQueue *queue;
    bool block_registered;
    bool release_registered;
    bool registered;
    bool producer_waiting;
    bool closing;
} QueueIngressProbe;

static _Atomic(QueueIngressProbe *) active_queue_ingress_probe;

/* Strong definition for the private seam compiled into this test binary.
 * Events acknowledge exact Queue-internal linearization points; no elapsed-
 * time assumption is permitted in lifetime/destruction tests. */
void
lardon3d_task_queue_internal_test_event(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskQueueTestEvent event
)
{
    QueueIngressProbe *probe = atomic_load_explicit(
        &active_queue_ingress_probe, memory_order_acquire);
    if (!probe || probe->queue != queue) {
        return;
    }
    (void)pthread_mutex_lock(&probe->mutex);
    if (event == LARDON3D_TASK_QUEUE_TEST_CALL_REGISTERED) {
        probe->registered = true;
        (void)pthread_cond_broadcast(&probe->condition);
        while (probe->block_registered && !probe->release_registered) {
            (void)pthread_cond_wait(&probe->condition, &probe->mutex);
        }
    } else if (event == LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING) {
        probe->producer_waiting = true;
        (void)pthread_cond_broadcast(&probe->condition);
    } else if (event == LARDON3D_TASK_QUEUE_TEST_CLOSING) {
        probe->closing = true;
        (void)pthread_cond_broadcast(&probe->condition);
    }
    (void)pthread_mutex_unlock(&probe->mutex);
}

static bool
queue_ingress_probe_init(
    QueueIngressProbe *probe,
    Lardon3DTaskQueue *queue,
    bool block_registered
)
{
    if (pthread_mutex_init(&probe->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&probe->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&probe->mutex);
        return false;
    }
    probe->queue = queue;
    probe->block_registered = block_registered;
    atomic_store_explicit(
        &active_queue_ingress_probe, probe, memory_order_release);
    return true;
}

static void
queue_ingress_probe_disable(QueueIngressProbe *probe)
{
    atomic_store_explicit(
        &active_queue_ingress_probe, NULL, memory_order_release);
    (void)pthread_cond_destroy(&probe->condition);
    (void)pthread_mutex_destroy(&probe->mutex);
}

static bool
queue_ingress_probe_wait(
    QueueIngressProbe *probe,
    Lardon3DTaskQueueTestEvent event
)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += 10;
    (void)pthread_mutex_lock(&probe->mutex);
    bool *observed = &probe->closing;
    if (event == LARDON3D_TASK_QUEUE_TEST_CALL_REGISTERED) {
        observed = &probe->registered;
    } else if (event == LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING) {
        observed = &probe->producer_waiting;
    }
    int result = 0;
    while (!*observed && result == 0) {
        result = pthread_cond_timedwait(
            &probe->condition, &probe->mutex, &deadline);
    }
    bool reached = *observed;
    (void)pthread_mutex_unlock(&probe->mutex);
    return reached && result == 0;
}

static void
queue_ingress_probe_release(QueueIngressProbe *probe)
{
    (void)pthread_mutex_lock(&probe->mutex);
    probe->release_registered = true;
    (void)pthread_cond_broadcast(&probe->condition);
    (void)pthread_mutex_unlock(&probe->mutex);
}

typedef struct {
    pthread_mutex_t mutex;
    size_t order[TASK_COUNT];
    size_t count;
} OrderLog;

typedef struct {
    OrderLog *log;
    size_t value;
    size_t steps;
    Lardon3DTaskExecutionContract contract;
    bool contract_seen;
    bool fail;
} QueueWork;

static void
short_pause(void)
{
    const struct timespec duration = {.tv_sec = 0, .tv_nsec = 1000000};
    (void)nanosleep(&duration, NULL);
}

static bool
queue_callback(Lardon3DTask *task, void *userdata)
{
    QueueWork *work = userdata;
    if (!lardon3d_task_execution_contract(task, &work->contract)) {
        return false;
    }
    work->contract_seen = true;
    (void)pthread_mutex_lock(&work->log->mutex);
    work->log->order[work->log->count++] = work->value;
    (void)pthread_mutex_unlock(&work->log->mutex);
    for (size_t step = 0; step < work->steps; ++step) {
        if (!lardon3d_task_checkpoint(task)) {
            return false;
        }
        unsigned int progress = (unsigned int)(((step + 1) * 100) / work->steps);
        (void)lardon3d_task_set_progress(task, progress, "File en cours.");
        if (work->steps > 1) {
            short_pause();
        }
    }
    return !work->fail;
}

static bool
immediate_callback(Lardon3DTask *task, void *userdata)
{
    (void)task;
    (void)userdata;
    return true;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t started;
    size_t finished;
    size_t destroyed;
    bool release;
    bool release_finished;
} LifetimeTracker;

typedef struct {
    LifetimeTracker *tracker;
    bool block;
    bool fail;
} OwnedQueueWork;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    Lardon3DTaskQueue *queue;
    uint64_t task_id;
    bool identity_ready;
    bool observed;
    bool observation_ok;
} FinishedQueueObservation;

static bool
lifetime_tracker_init(LifetimeTracker *tracker)
{
    if (pthread_mutex_init(&tracker->mutex, NULL) != 0) {
        return false;
    }
    if (pthread_cond_init(&tracker->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&tracker->mutex);
        return false;
    }
    return true;
}

static void
lifetime_tracker_destroy(LifetimeTracker *tracker)
{
    (void)pthread_cond_destroy(&tracker->condition);
    (void)pthread_mutex_destroy(&tracker->mutex);
}

static bool
wait_tracker_count(
    LifetimeTracker *tracker,
    bool wait_for_destroyed,
    size_t wanted
)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += 10;
    (void)pthread_mutex_lock(&tracker->mutex);
    size_t *value = wait_for_destroyed ? &tracker->destroyed : &tracker->started;
    int result = 0;
    while (*value < wanted && result == 0) {
        result = pthread_cond_timedwait(
            &tracker->condition, &tracker->mutex, &deadline);
    }
    bool reached = *value >= wanted;
    (void)pthread_mutex_unlock(&tracker->mutex);
    return reached && result == 0;
}

static size_t
tracker_destroyed(LifetimeTracker *tracker)
{
    (void)pthread_mutex_lock(&tracker->mutex);
    size_t destroyed = tracker->destroyed;
    (void)pthread_mutex_unlock(&tracker->mutex);
    return destroyed;
}

static size_t
tracker_started(LifetimeTracker *tracker)
{
    (void)pthread_mutex_lock(&tracker->mutex);
    size_t started = tracker->started;
    (void)pthread_mutex_unlock(&tracker->mutex);
    return started;
}

static bool
wait_tracker_finished(LifetimeTracker *tracker, size_t wanted)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += 10;
    (void)pthread_mutex_lock(&tracker->mutex);
    int result = 0;
    while (tracker->finished < wanted && result == 0) {
        result = pthread_cond_timedwait(
            &tracker->condition, &tracker->mutex, &deadline);
    }
    bool reached = tracker->finished >= wanted;
    (void)pthread_mutex_unlock(&tracker->mutex);
    return reached && result == 0;
}

static void
release_tracker_callbacks(LifetimeTracker *tracker)
{
    (void)pthread_mutex_lock(&tracker->mutex);
    tracker->release = true;
    (void)pthread_cond_broadcast(&tracker->condition);
    (void)pthread_mutex_unlock(&tracker->mutex);
}

static void
release_tracker_finished(LifetimeTracker *tracker)
{
    (void)pthread_mutex_lock(&tracker->mutex);
    tracker->release_finished = true;
    (void)pthread_cond_broadcast(&tracker->condition);
    (void)pthread_mutex_unlock(&tracker->mutex);
}

static bool
owned_queue_callback(Lardon3DTask *task, void *userdata)
{
    OwnedQueueWork *work = userdata;
    (void)pthread_mutex_lock(&work->tracker->mutex);
    ++work->tracker->started;
    (void)pthread_cond_broadcast(&work->tracker->condition);
    while (work->block && !work->tracker->release) {
        (void)pthread_cond_wait(
            &work->tracker->condition, &work->tracker->mutex);
    }
    (void)pthread_mutex_unlock(&work->tracker->mutex);
    return lardon3d_task_checkpoint(task) && !work->fail;
}

static void
owned_queue_work_destroy(void *userdata)
{
    OwnedQueueWork *work = userdata;
    LifetimeTracker *tracker = work->tracker;
    (void)pthread_mutex_lock(&tracker->mutex);
    ++tracker->destroyed;
    (void)pthread_cond_broadcast(&tracker->condition);
    (void)pthread_mutex_unlock(&tracker->mutex);
    free(work);
}

static void
owned_finished_callback(const Lardon3DTask *task, void *userdata)
{
    (void)task;
    LifetimeTracker *tracker = userdata;
    (void)pthread_mutex_lock(&tracker->mutex);
    ++tracker->finished;
    (void)pthread_cond_broadcast(&tracker->condition);
    while (!tracker->release_finished) {
        (void)pthread_cond_wait(&tracker->condition, &tracker->mutex);
    }
    (void)pthread_mutex_unlock(&tracker->mutex);
}

static bool
finished_observation_work(Lardon3DTask *task, void *userdata)
{
    FinishedQueueObservation *observation = userdata;
    (void)pthread_mutex_lock(&observation->mutex);
    while (!observation->identity_ready) {
        (void)pthread_cond_wait(
            &observation->condition, &observation->mutex);
    }
    (void)pthread_mutex_unlock(&observation->mutex);
    return lardon3d_task_checkpoint(task);
}

static void
observe_queue_from_finished(const Lardon3DTask *task, void *userdata)
{
    (void)task;
    FinishedQueueObservation *observation = userdata;
    Lardon3DTaskSnapshot by_id;
    Lardon3DTaskSnapshot newest;
    Lardon3DTaskQueueSummary summary;
    bool ok = lardon3d_task_queue_get(
            observation->queue, observation->task_id, &by_id)
        && by_id.id == observation->task_id
        && by_id.state == TASK_COMPLETED
        && lardon3d_task_queue_snapshot(
            observation->queue, &newest, 1, &summary) == 1
        && newest.id == observation->task_id
        && summary.completed == 1 && summary.total == 1;
    (void)pthread_mutex_lock(&observation->mutex);
    observation->observation_ok = ok;
    observation->observed = true;
    (void)pthread_cond_broadcast(&observation->condition);
    (void)pthread_mutex_unlock(&observation->mutex);
}

static bool
wait_finished_observation(FinishedQueueObservation *observation)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += 10;
    (void)pthread_mutex_lock(&observation->mutex);
    int result = 0;
    while (!observation->observed && result == 0) {
        result = pthread_cond_timedwait(
            &observation->condition, &observation->mutex, &deadline);
    }
    bool ok = observation->observed && observation->observation_ok;
    (void)pthread_mutex_unlock(&observation->mutex);
    return result == 0 && ok;
}

static Lardon3DTask *
create_owned_task(
    const char *name,
    const Lardon3DResourceEstimate *estimate,
    LifetimeTracker *tracker,
    bool block,
    bool fail
)
{
    OwnedQueueWork *work = calloc(1, sizeof(*work));
    if (!work) {
        return NULL;
    }
    work->tracker = tracker;
    work->block = block;
    work->fail = fail;
    Lardon3DTask *task = lardon3d_task_create_typed(
        name,
        estimate,
        "test.queue_lifetime",
        1,
        owned_queue_callback,
        work,
        owned_queue_work_destroy
    );
    if (!task) {
        free(work);
    }
    return task;
}

static bool
wait_terminal(Lardon3DTaskQueue *queue, uint64_t id, Lardon3DTaskSnapshot *result)
{
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        if (!lardon3d_task_queue_get(queue, id, result)) {
            return false;
        }
        if (result->state == TASK_CANCELLED || result->state == TASK_FAILED
            || result->state == TASK_COMPLETED) {
            return true;
        }
        short_pause();
    }
    return false;
}

static bool
wait_full_terminal_history(
    Lardon3DTaskQueue *queue,
    size_t expected_completed
)
{
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        Lardon3DTaskSnapshot records[
            LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1];
        Lardon3DTaskQueueSummary summary;
        size_t count = lardon3d_task_queue_snapshot(
            queue,
            records,
            LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1,
            &summary
        );
        /* A just-finished Task is terminal before the worker retires it. With
         * full history that valid transition exposes 65 records; exactly 64
         * proves the live Task has been destroyed and inserted into history. */
        if (count == LARDON3D_TASK_QUEUE_HISTORY_CAPACITY
            && summary.running == 0 && summary.pending == 0
            && summary.completed == expected_completed
            && summary.total == expected_completed) {
            return true;
        }
        short_pause();
    }
    return false;
}

typedef struct {
    Lardon3DTaskQueue *queue;
    Lardon3DResourceEstimate estimate;
    QueueWork *work;
    uint64_t id;
    bool added;
} ProducerThread;

typedef struct {
    Lardon3DTaskQueue *queue;
    size_t count;
} QueueCountThread;

typedef struct {
    Lardon3DTaskQueue *queue;
    atomic_bool returned;
} QueueDestroyThread;

static void *
producer_thread(void *context)
{
    ProducerThread *producer = context;
    Lardon3DTask *task = lardon3d_task_create(
        "Producteur",
        &producer->estimate,
        queue_callback,
        producer->work
    );
    if (!task) {
        producer->added = false;
        return NULL;
    }
    producer->added = lardon3d_task_queue_add(
        producer->queue,
        task,
        &producer->id
    );
    if (!producer->added) {
        lardon3d_task_destroy(task);
    }
    return NULL;
}

static void *
queue_count_thread(void *context)
{
    QueueCountThread *call = context;
    call->count = lardon3d_task_queue_count(call->queue);
    return NULL;
}

static void *
queue_destroy_thread(void *context)
{
    QueueDestroyThread *call = context;
    lardon3d_task_queue_destroy(call->queue);
    atomic_store_explicit(&call->returned, true, memory_order_release);
    return NULL;
}

/* Réserve toutes les ressources CPU pour que le worker ne puisse plus
   consommer de tâche : la file d'attente reste pleine et les producteurs
   bloquent de façon déterministe. */
static bool
hold_resources(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceReservation **reservation
)
{
    Lardon3DResourceSnapshot blocking_snapshot = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    const Lardon3DResourceEstimate blocking_estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1024,
    };
    Lardon3DResourceDecision decision;
    return lardon3d_test_resource_snapshot_make_fresh(&blocking_snapshot)
        && lardon3d_resource_governor_reserve(
        governor,
        &blocking_snapshot,
        &blocking_estimate,
        &decision,
        reservation
    );
}

static bool
test_registered_ingress_blocks_destroy(Lardon3DResourceGovernor *governor)
{
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    QueueIngressProbe probe = {0};
    CHECK(queue_ingress_probe_init(&probe, queue, true));

    QueueCountThread count_call = {.queue = queue, .count = SIZE_MAX};
    pthread_t count_thread;
    CHECK(pthread_create(
        &count_thread, NULL, queue_count_thread, &count_call) == 0);
    CHECK(queue_ingress_probe_wait(
        &probe, LARDON3D_TASK_QUEUE_TEST_CALL_REGISTERED));

    QueueDestroyThread destroy_call = {.queue = queue};
    atomic_init(&destroy_call.returned, false);
    pthread_t destroy_thread;
    CHECK(pthread_create(
        &destroy_thread, NULL, queue_destroy_thread, &destroy_call) == 0);
    CHECK(queue_ingress_probe_wait(
        &probe, LARDON3D_TASK_QUEUE_TEST_CLOSING));
    CHECK(!atomic_load_explicit(&destroy_call.returned, memory_order_acquire));

    /* The registered call is still before queue->mutex. Destroy has closed
     * ingress and must remain alive until this exact reference can observe
     * stopping, release its pin, and return. */
    queue_ingress_probe_release(&probe);
    CHECK(pthread_join(count_thread, NULL) == 0);
    CHECK(count_call.count == 0);
    CHECK(pthread_join(destroy_thread, NULL) == 0);
    CHECK(atomic_load_explicit(&destroy_call.returned, memory_order_acquire));
    queue_ingress_probe_disable(&probe);
    return true;
}

static bool
test_generated_id_exhaustion_is_sticky(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 2);
    CHECK(queue);
    Lardon3DTaskSnapshot snapshot;

    /* A restored penultimate ID advances the generator to its final value.
     * Removing both terminal records proves exhaustion is lifetime state, not
     * an accidental consequence of retained collision records. */
    Lardon3DTask *penultimate = lardon3d_task_create(
        "Restored penultimate ID", &estimate, immediate_callback, NULL);
    CHECK(penultimate && lardon3d_task_assign_id(penultimate, UINT64_MAX - 1));
    CHECK(lardon3d_task_queue_add(queue, penultimate, NULL));
    CHECK(wait_terminal(queue, UINT64_MAX - 1, &snapshot));
    CHECK(lardon3d_task_queue_remove(queue, UINT64_MAX - 1));

    Lardon3DTask *last = lardon3d_task_create(
        "Generated final ID", &estimate, immediate_callback, NULL);
    uint64_t last_id = 0;
    CHECK(last && lardon3d_task_queue_add(queue, last, &last_id));
    CHECK(last_id == UINT64_MAX);
    CHECK(wait_terminal(queue, UINT64_MAX, &snapshot));
    CHECK(lardon3d_task_queue_remove(queue, UINT64_MAX));

    Lardon3DTask *rejected = lardon3d_task_create(
        "Generation exhausted", &estimate, immediate_callback, NULL);
    CHECK(rejected);
    CHECK(lardon3d_task_queue_try_add_ex(queue, rejected, NULL)
        == LARDON3D_TASK_QUEUE_ADD_ERROR);
    lardon3d_task_destroy(rejected);

    Lardon3DTask *lower = lardon3d_task_create(
        "Lower restored ID", &estimate, immediate_callback, NULL);
    CHECK(lower && lardon3d_task_assign_id(lower, 7));
    CHECK(lardon3d_task_queue_add(queue, lower, NULL));
    CHECK(wait_terminal(queue, 7, &snapshot));
    CHECK(lardon3d_task_queue_remove(queue, 7));

    rejected = lardon3d_task_create(
        "Generation remains exhausted", &estimate, immediate_callback, NULL);
    CHECK(rejected);
    CHECK(lardon3d_task_queue_try_add_ex(queue, rejected, NULL)
        == LARDON3D_TASK_QUEUE_ADD_ERROR);
    lardon3d_task_destroy(rejected);
    lardon3d_task_queue_destroy(queue);
    return true;
}

static bool
run_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 1024,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = 0,
        .system_cpu_reserve = 0,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    CHECK(governor);
    lardon3d_task_queue_destroy(NULL);
    CHECK(lardon3d_task_queue_count(NULL) == 0);
    CHECK(!lardon3d_task_queue_create(NULL, 0));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1024);
    CHECK(queue);
    short_pause();

    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[TASK_COUNT];
    Lardon3DTask *tasks[TASK_COUNT];
    uint64_t ids[TASK_COUNT];
    for (size_t index = 0; index < TASK_COUNT; ++index) {
        work[index] = (QueueWork) {
            .log = &log,
            .value = index,
            .steps = 1,
        };
        tasks[index] = lardon3d_task_create(
            "FIFO",
            &estimate,
            queue_callback,
            &work[index]
        );
        CHECK(tasks[index]);
        CHECK(lardon3d_task_queue_add(queue, tasks[index], &ids[index]));
        CHECK(ids[index] == index + 1);
    }
    CHECK(lardon3d_task_queue_count(queue) <= TASK_COUNT);
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, ids[TASK_COUNT - 1], &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(wait_full_terminal_history(queue, TASK_COUNT));
    CHECK(log.count == TASK_COUNT);
    for (size_t index = 0; index < TASK_COUNT; ++index) {
        CHECK(log.order[index] == index);
        CHECK(work[index].contract_seen);
        CHECK(work[index].contract.batch_size == 1);
    }
    Lardon3DTaskQueueSummary summary;
    Lardon3DTaskSnapshot listed[LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1];
    CHECK(lardon3d_task_queue_snapshot(
        queue,
        listed,
        LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1,
        &summary
    ) == LARDON3D_TASK_QUEUE_HISTORY_CAPACITY);
    CHECK(summary.total == TASK_COUNT);
    CHECK(summary.running == 0);
    CHECK(summary.pending == 0);
    CHECK(summary.completed == TASK_COUNT);
    CHECK(lardon3d_task_queue_get_at(queue, 0, &snapshot));
    CHECK(snapshot.id == ids[TASK_COUNT - 1]);
    CHECK(listed[0].id == ids[TASK_COUNT - 1]);
    CHECK(listed[LARDON3D_TASK_QUEUE_HISTORY_CAPACITY - 1].id
        == ids[TASK_COUNT - LARDON3D_TASK_QUEUE_HISTORY_CAPACITY]);
    CHECK(!lardon3d_task_queue_get_at(
        queue, LARDON3D_TASK_QUEUE_HISTORY_CAPACITY, &snapshot));
    CHECK(!lardon3d_task_queue_get(queue, ids[0], &snapshot));
    CHECK(!lardon3d_task_queue_remove(queue, ids[0]));
    Lardon3DTask *after_eviction = lardon3d_task_create(
        "Après éviction", &estimate, immediate_callback, NULL);
    uint64_t after_eviction_id = 0;
    CHECK(after_eviction && lardon3d_task_queue_add(
        queue, after_eviction, &after_eviction_id));
    CHECK(after_eviction_id == TASK_COUNT + 1);
    CHECK(wait_terminal(queue, after_eviction_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(wait_full_terminal_history(queue, TASK_COUNT + 1));
    CHECK(lardon3d_task_queue_get_at(queue, 0, &snapshot)
        && snapshot.id == after_eviction_id);
    CHECK(lardon3d_task_queue_remove(queue, ids[TASK_COUNT - 1]));
    CHECK(!lardon3d_task_queue_get(queue, ids[TASK_COUNT - 1], &snapshot));
    CHECK(lardon3d_task_queue_snapshot(queue, NULL, 0, &summary) == 0);
    CHECK(summary.total == TASK_COUNT + 1
        && summary.completed == TASK_COUNT + 1);
    CHECK(lardon3d_task_queue_count(queue) == 0);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);

    queue = lardon3d_task_queue_create(governor, 1024);
    CHECK(queue);
    OrderLog restored_log = {0};
    CHECK(pthread_mutex_init(&restored_log.mutex, NULL) == 0);
    QueueWork restored_work = {.log = &restored_log, .value = 0, .steps = 1};
    Lardon3DTask *restored = lardon3d_task_create(
        "Restaurée", &estimate, queue_callback, &restored_work);
    CHECK(restored && lardon3d_task_assign_id(restored, 100));
    CHECK(lardon3d_task_queue_add(queue, restored, NULL));
    Lardon3DTask *duplicate = lardon3d_task_create(
        "Doublon", &estimate, queue_callback, &restored_work);
    CHECK(duplicate && lardon3d_task_assign_id(duplicate, 100));
    CHECK(lardon3d_task_queue_try_add_ex(queue, duplicate, NULL)
        == LARDON3D_TASK_QUEUE_ADD_DUPLICATE_ID);
    lardon3d_task_destroy(duplicate);
    CHECK(wait_terminal(queue, 100, &snapshot));
    CHECK(lardon3d_task_queue_remove(queue, 100));
    CHECK(pthread_mutex_destroy(&restored_log.mutex) == 0);
    OrderLog control_log = {0};
    CHECK(pthread_mutex_init(&control_log.mutex, NULL) == 0);
    QueueWork slow = {.log = &control_log, .value = 1, .steps = 500};
    QueueWork cancelled = {.log = &control_log, .value = 2, .steps = 1};
    Lardon3DTask *slow_task = lardon3d_task_create(
        "Longue",
        &estimate,
        queue_callback,
        &slow
    );
    Lardon3DTask *cancelled_task = lardon3d_task_create(
        "Annulée en attente",
        &estimate,
        queue_callback,
        &cancelled
    );
    uint64_t slow_id;
    uint64_t cancelled_id;
    CHECK(slow_task && cancelled_task);
    CHECK(lardon3d_task_queue_add(queue, slow_task, &slow_id));
    CHECK(lardon3d_task_queue_add(queue, cancelled_task, &cancelled_id));
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        CHECK(lardon3d_task_queue_get(queue, slow_id, &snapshot));
        if (snapshot.state == TASK_RUNNING) {
            break;
        }
        short_pause();
    }
    CHECK(lardon3d_task_queue_pause(queue, slow_id));
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        CHECK(lardon3d_task_queue_get(queue, slow_id, &snapshot));
        if (snapshot.state == TASK_PAUSED) {
            break;
        }
        short_pause();
    }
    CHECK(snapshot.state == TASK_PAUSED);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 1);
    CHECK(lardon3d_task_queue_cancel(queue, cancelled_id));
    CHECK(lardon3d_task_queue_resume(queue, slow_id));
    CHECK(wait_terminal(queue, slow_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(wait_terminal(queue, cancelled_id, &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(control_log.count == 1);
    CHECK(lardon3d_task_queue_remove(queue, cancelled_id));
    CHECK(lardon3d_task_queue_remove(queue, slow_id));
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&control_log.mutex) == 0);

    queue = lardon3d_task_queue_create(governor, 1024);
    CHECK(queue);
    OrderLog destruction_log = {0};
    CHECK(pthread_mutex_init(&destruction_log.mutex, NULL) == 0);
    QueueWork destruction_work = {
        .log = &destruction_log,
        .value = 0,
        .steps = 10000,
    };
    Lardon3DTask *destruction_task = lardon3d_task_create(
        "Destruction sûre",
        &estimate,
        queue_callback,
        &destruction_work
    );
    CHECK(destruction_task);
    CHECK(lardon3d_task_queue_add(queue, destruction_task, NULL));
    short_pause();
    lardon3d_task_queue_destroy(queue);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    CHECK(pthread_mutex_destroy(&destruction_log.mutex) == 0);

    Lardon3DResourceSnapshot blocking_snapshot = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate blocking_estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1024,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *blocking_reservation;
    CHECK(lardon3d_test_resource_snapshot_make_fresh(&blocking_snapshot));
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &blocking_snapshot,
        &blocking_estimate,
        &decision,
        &blocking_reservation
    ));
    CHECK(blocking_reservation);
    queue = lardon3d_task_queue_create(governor, 1024);
    CHECK(queue);
    OrderLog wait_log = {0};
    CHECK(pthread_mutex_init(&wait_log.mutex, NULL) == 0);
    QueueWork waiting = {.log = &wait_log, .value = 1, .steps = 1};
    Lardon3DTask *waiting_task = lardon3d_task_create(
        "Attente ressources",
        &estimate,
        queue_callback,
        &waiting
    );
    uint64_t waiting_id;
    CHECK(waiting_task);
    CHECK(lardon3d_task_queue_add(queue, waiting_task, &waiting_id));
    short_pause();
    CHECK(lardon3d_task_queue_get(queue, waiting_id, &snapshot));
    CHECK(snapshot.state == TASK_PENDING);
    CHECK(!waiting.contract_seen);
    CHECK(lardon3d_task_queue_cancel(queue, waiting_id));
    CHECK(wait_terminal(queue, waiting_id, &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(!waiting.contract_seen);
    QueueWork awakened = {.log = &wait_log, .value = 2, .steps = 1};
    Lardon3DTask *awakened_task = lardon3d_task_create(
        "Réveil ressources",
        &estimate,
        queue_callback,
        &awakened
    );
    uint64_t awakened_id;
    CHECK(awakened_task);
    CHECK(lardon3d_task_queue_add(queue, awakened_task, &awakened_id));
    short_pause();
    CHECK(!awakened.contract_seen);
    CHECK(lardon3d_resource_governor_release(governor, blocking_reservation));
    CHECK(wait_terminal(queue, awakened_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(awakened.contract_seen);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);

    QueueWork capture_failed = {.log = &wait_log, .value = 3, .steps = 1};
    Lardon3DTask *capture_failed_task = lardon3d_task_create(
        "Échec capture",
        &estimate,
        queue_callback,
        &capture_failed
    );
    uint64_t capture_failed_id;
    CHECK(capture_failed_task);
    lardon3d_resource_governor_internal_force_capture_failure(governor, true);
    CHECK(lardon3d_task_queue_add(
        queue,
        capture_failed_task,
        &capture_failed_id
    ));
    CHECK(wait_terminal(queue, capture_failed_id, &snapshot));
    CHECK(snapshot.state == TASK_FAILED);
    CHECK(!capture_failed.contract_seen);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    lardon3d_resource_governor_internal_force_capture_failure(governor, false);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&wait_log.mutex) == 0);

    /* Une tâche de tête bloquée en WAIT ne doit pas empêcher une tâche
       admissible située derrière elle de démarrer. */
    Lardon3DResourceSnapshot io_blocking_snapshot = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate io_blocking_estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .desired_io_slots = 1,
    };
    Lardon3DResourceDecision io_decision;
    Lardon3DResourceReservation *io_blocking_reservation;
    CHECK(lardon3d_test_resource_snapshot_make_fresh(&io_blocking_snapshot));
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &io_blocking_snapshot,
        &io_blocking_estimate,
        &io_decision,
        &io_blocking_reservation
    ));
    CHECK(io_blocking_reservation);
    queue = lardon3d_task_queue_create(governor, 1024);
    CHECK(queue);
    OrderLog bypass_log = {0};
    CHECK(pthread_mutex_init(&bypass_log.mutex, NULL) == 0);
    Lardon3DResourceEstimate io_estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .desired_io_slots = 1,
    };
    QueueWork blocked = {.log = &bypass_log, .value = 1, .steps = 1};
    Lardon3DTask *blocked_task = lardon3d_task_create(
        "Bloquée en tête",
        &io_estimate,
        queue_callback,
        &blocked
    );
    uint64_t blocked_id;
    CHECK(blocked_task);
    CHECK(lardon3d_task_queue_add(queue, blocked_task, &blocked_id));
    QueueWork bypass = {.log = &bypass_log, .value = 2, .steps = 1};
    Lardon3DTask *bypass_task = lardon3d_task_create(
        "Admissible derrière",
        &estimate,
        queue_callback,
        &bypass
    );
    uint64_t bypass_id;
    CHECK(bypass_task);
    CHECK(lardon3d_task_queue_add(queue, bypass_task, &bypass_id));
    CHECK(wait_terminal(queue, bypass_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(bypass.contract_seen);
    CHECK(lardon3d_task_queue_get(queue, blocked_id, &snapshot));
    CHECK(snapshot.state == TASK_PENDING);
    CHECK(!blocked.contract_seen);
    CHECK(lardon3d_resource_governor_release(governor, io_blocking_reservation));
    CHECK(wait_terminal(queue, blocked_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(blocked.contract_seen);
    CHECK(bypass_log.count == 2);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    Lardon3DTaskQueueSummary bypass_summary;
    lardon3d_task_queue_snapshot(queue, NULL, 0, &bypass_summary);
    CHECK(bypass_summary.running == 0);
    CHECK(bypass_summary.pending == 0);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&bypass_log.mutex) == 0);

    queue = lardon3d_task_queue_create(governor, 1024);
    CHECK(queue);
    OrderLog contract_log = {0};
    CHECK(pthread_mutex_init(&contract_log.mutex, NULL) == 0);
    Lardon3DResourceEstimate reduced_estimate = estimate;
    reduced_estimate.desired_cpu_threads = 2048;
    QueueWork reduced = {.log = &contract_log, .value = 1, .steps = 1};
    Lardon3DTask *reduced_task = lardon3d_task_create(
        "Contrat réduit",
        &reduced_estimate,
        queue_callback,
        &reduced
    );
    uint64_t reduced_id;
    Lardon3DTaskCapabilityEnvelope reduced_envelope = {
        .count = 1,
        .capabilities = {{
            .estimate = reduced_estimate,
            .backend = LARDON3D_RESOURCE_BACKEND_CPU,
            .inflight_limit = 1,
            .cpu_reducible = true,
        }},
    };
    /* This synthetic callback explicitly consumes the contract; unlike a
     * production kind, it has no registry entry from which to reconstruct the
     * otherwise private adaptive envelope. */
    CHECK(reduced_task && lardon3d_task_internal_set_capability_envelope(
        reduced_task, &reduced_envelope));
    CHECK(lardon3d_task_queue_add(queue, reduced_task, &reduced_id));
    CHECK(wait_terminal(queue, reduced_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    /* CPU-reducible capabilities slow-start at one participant. A later
     * sequence may grow only after two baseline and two improving samples. */
    CHECK(reduced.contract.cpu_threads == 1);
    Lardon3DResourceEstimate rejected_estimate = {
        .memory_fixed_bytes = UINT64_MAX,
        .memory_bytes_per_item = 1,
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    QueueWork rejected = {.log = &contract_log, .value = 2, .steps = 1};
    Lardon3DTask *rejected_task = lardon3d_task_create(
        "Impossible",
        &rejected_estimate,
        queue_callback,
        &rejected
    );
    uint64_t rejected_id;
    CHECK(rejected_task);
    CHECK(lardon3d_task_queue_add(queue, rejected_task, &rejected_id));
    CHECK(wait_terminal(queue, rejected_id, &snapshot));
    CHECK(snapshot.state == TASK_FAILED);
    CHECK(!rejected.contract_seen);
    QueueWork failure = {
        .log = &contract_log,
        .value = 3,
        .steps = 1,
        .fail = true,
    };
    Lardon3DTask *failure_task = lardon3d_task_create(
        "Échec callback",
        &estimate,
        queue_callback,
        &failure
    );
    uint64_t failure_id;
    CHECK(failure_task);
    CHECK(lardon3d_task_queue_add(queue, failure_task, &failure_id));
    CHECK(wait_terminal(queue, failure_id, &snapshot));
    CHECK(snapshot.state == TASK_FAILED);
    CHECK(failure.contract_seen);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&contract_log.mutex) == 0);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
test_finished_read_only_reentrancy(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    FinishedQueueObservation observation = {0};
    CHECK(pthread_mutex_init(&observation.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&observation.condition, NULL) == 0);
    observation.queue = lardon3d_task_queue_create(governor, 1);
    CHECK(observation.queue);
    Lardon3DTask *task = lardon3d_task_create(
        "Observation de fin",
        &estimate,
        finished_observation_work,
        &observation
    );
    CHECK(task && lardon3d_task_set_finished_callback(
        task, observe_queue_from_finished, &observation));
    CHECK(lardon3d_task_queue_add(
        observation.queue, task, &observation.task_id));
    (void)pthread_mutex_lock(&observation.mutex);
    observation.identity_ready = true;
    (void)pthread_cond_broadcast(&observation.condition);
    (void)pthread_mutex_unlock(&observation.mutex);
    CHECK(wait_finished_observation(&observation));
    lardon3d_task_queue_destroy(observation.queue);
    CHECK(pthread_cond_destroy(&observation.condition) == 0);
    CHECK(pthread_mutex_destroy(&observation.mutex) == 0);
    return true;
}

static bool
test_terminal_lifetime(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    LifetimeTracker tracker = {0};
    CHECK(lifetime_tracker_init(&tracker));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 4);
    CHECK(queue);
    Lardon3DTaskObservation absent_observation = {.id = UINT64_MAX};
    CHECK(!lardon3d_task_queue_get_observation(
        queue, UINT64_MAX, &absent_observation));
    CHECK(absent_observation.id == 0
        && !absent_observation.has_execution_contract);

    Lardon3DTask *active = create_owned_task(
        "Durée active", &estimate, &tracker, true, false);
    uint64_t active_id = 0;
    CHECK(active && lardon3d_task_queue_add(queue, active, &active_id));
    CHECK(wait_tracker_count(&tracker, false, 1));

    Lardon3DTask *pending = create_owned_task(
        "Durée attente", &estimate, &tracker, false, false);
    uint64_t pending_id = 0;
    CHECK(pending && lardon3d_task_queue_add(queue, pending, &pending_id));
    CHECK(tracker_destroyed(&tracker) == 0);
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_queue_get(queue, active_id, &snapshot)
        && snapshot.state == TASK_RUNNING);
    CHECK(lardon3d_task_queue_get(queue, pending_id, &snapshot)
        && snapshot.state == TASK_PENDING);
    CHECK(!lardon3d_task_queue_remove(queue, active_id));
    CHECK(!lardon3d_task_queue_remove(queue, pending_id));

    /* Pending cancellation completes its finished callback synchronously; the
     * userdata must be gone before Queue destruction while its snapshot stays
     * observable. The active Task remains owned until its callback exits. */
    CHECK(lardon3d_task_queue_cancel(queue, pending_id));
    CHECK(wait_tracker_count(&tracker, true, 1));
    CHECK(lardon3d_task_queue_get(queue, pending_id, &snapshot)
        && snapshot.state == TASK_CANCELLED);
    CHECK(tracker_destroyed(&tracker) == 1);
    Lardon3DTaskSnapshot live_first[2];
    Lardon3DTaskQueueSummary live_summary;
    CHECK(lardon3d_task_queue_snapshot(
        queue, live_first, 2, &live_summary) == 2);
    CHECK(live_first[0].id == active_id
        && live_first[0].state == TASK_RUNNING);
    CHECK(live_first[1].id == pending_id
        && live_first[1].state == TASK_CANCELLED);
    CHECK(live_summary.running == 1 && live_summary.pending == 0
        && live_summary.completed == 1 && live_summary.total == 2);

    release_tracker_callbacks(&tracker);
    CHECK(wait_tracker_count(&tracker, true, 2));
    CHECK(lardon3d_task_queue_get(queue, active_id, &snapshot)
        && snapshot.state == TASK_COMPLETED);

    Lardon3DTask *failed = create_owned_task(
        "Durée échec", &estimate, &tracker, false, true);
    uint64_t failed_id = 0;
    CHECK(failed && lardon3d_task_queue_add(queue, failed, &failed_id));
    CHECK(wait_tracker_count(&tracker, true, 3));
    CHECK(lardon3d_task_queue_get(queue, failed_id, &snapshot)
        && snapshot.state == TASK_FAILED);

    Lardon3DTask *finished = create_owned_task(
        "Durée finalisation", &estimate, &tracker, false, false);
    uint64_t finished_id = 0;
    CHECK(finished && lardon3d_task_set_finished_callback(
        finished, owned_finished_callback, &tracker));
    CHECK(lardon3d_task_queue_add(queue, finished, &finished_id));
    CHECK(wait_tracker_finished(&tracker, 1));
    CHECK(tracker_destroyed(&tracker) == 3);
    CHECK(lardon3d_task_queue_get(queue, finished_id, &snapshot)
        && snapshot.state == TASK_COMPLETED);
    release_tracker_finished(&tracker);
    CHECK(wait_tracker_count(&tracker, true, 4));
    CHECK(lardon3d_task_queue_get(queue, finished_id, &snapshot)
        && snapshot.state == TASK_COMPLETED);

    CHECK(lardon3d_task_queue_cancel(queue, active_id));
    CHECK(!lardon3d_task_queue_pause(queue, active_id));
    CHECK(!lardon3d_task_queue_resume(queue, active_id));

    Lardon3DTaskSnapshot ordered[4];
    Lardon3DTaskQueueSummary summary;
    CHECK(lardon3d_task_queue_snapshot(queue, ordered, 4, &summary) == 4);
    CHECK(ordered[0].id == finished_id
        && ordered[0].state == TASK_COMPLETED);
    CHECK(ordered[1].id == failed_id && ordered[1].state == TASK_FAILED);
    CHECK(ordered[2].id == active_id && ordered[2].state == TASK_COMPLETED);
    CHECK(ordered[3].id == pending_id && ordered[3].state == TASK_CANCELLED);
    CHECK(summary.running == 0 && summary.pending == 0
        && summary.completed == 4 && summary.total == 4);
    CHECK(lardon3d_task_queue_remove(queue, failed_id));
    CHECK(!lardon3d_task_queue_get(queue, failed_id, &snapshot));
    CHECK(!lardon3d_task_queue_remove(queue, failed_id));
    CHECK(lardon3d_task_queue_snapshot(queue, NULL, 0, &summary) == 0);
    CHECK(summary.completed == 4 && summary.total == 4);

    lardon3d_task_queue_destroy(queue);
    CHECK(tracker_destroyed(&tracker) == 4);
    lifetime_tracker_destroy(&tracker);
    return true;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t ready;
    bool go;
} AccessGate;

typedef struct {
    Lardon3DTaskQueue *queue;
    AccessGate *gate;
    size_t task_count;
    bool remove;
    bool ok;
} QueueAccessStress;

static void *
queue_access_stress(void *userdata)
{
    QueueAccessStress *stress = userdata;
    (void)pthread_mutex_lock(&stress->gate->mutex);
    ++stress->gate->ready;
    (void)pthread_cond_broadcast(&stress->gate->condition);
    while (!stress->gate->go) {
        (void)pthread_cond_wait(&stress->gate->condition, &stress->gate->mutex);
    }
    (void)pthread_mutex_unlock(&stress->gate->mutex);

    stress->ok = true;
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        Lardon3DTaskSnapshot snapshots[LARDON3D_TASK_QUEUE_HISTORY_CAPACITY];
        Lardon3DTaskQueueSummary summary;
        size_t count = lardon3d_task_queue_snapshot(
            stress->queue,
            snapshots,
            LARDON3D_TASK_QUEUE_HISTORY_CAPACITY,
            &summary
        );
        if (count > LARDON3D_TASK_QUEUE_HISTORY_CAPACITY
            || summary.total < summary.completed) {
            stress->ok = false;
            break;
        }
        uint64_t id = (uint64_t)(iteration % stress->task_count) + 1;
        Lardon3DTaskSnapshot snapshot;
        if (lardon3d_task_queue_get(stress->queue, id, &snapshot)
            && snapshot.id != id) {
            stress->ok = false;
            break;
        }
        if (stress->remove) {
            (void)lardon3d_task_queue_remove(stress->queue, id);
        }
    }
    return NULL;
}

static bool
test_concurrent_terminal_access(Lardon3DResourceGovernor *governor)
{
    enum { STRESS_TASKS = 128, ACCESS_THREADS = 2 };
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    LifetimeTracker tracker = {0};
    CHECK(lifetime_tracker_init(&tracker));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, STRESS_TASKS);
    CHECK(queue);
    for (size_t index = 0; index < STRESS_TASKS; ++index) {
        Lardon3DTask *task = create_owned_task(
            "Accès concurrent", &estimate, &tracker, true, false);
        uint64_t id = 0;
        CHECK(task && lardon3d_task_queue_add(queue, task, &id));
        CHECK(id == index + 1);
    }
    CHECK(wait_tracker_count(&tracker, false, 1));

    AccessGate gate = {0};
    CHECK(pthread_mutex_init(&gate.mutex, NULL) == 0);
    CHECK(pthread_cond_init(&gate.condition, NULL) == 0);
    QueueAccessStress stress[ACCESS_THREADS] = {
        {.queue = queue, .gate = &gate, .task_count = STRESS_TASKS},
        {.queue = queue, .gate = &gate, .task_count = STRESS_TASKS, .remove = true},
    };
    pthread_t threads[ACCESS_THREADS];
    for (size_t index = 0; index < ACCESS_THREADS; ++index) {
        CHECK(pthread_create(
            &threads[index], NULL, queue_access_stress, &stress[index]) == 0);
    }
    (void)pthread_mutex_lock(&gate.mutex);
    while (gate.ready < ACCESS_THREADS) {
        (void)pthread_cond_wait(&gate.condition, &gate.mutex);
    }
    gate.go = true;
    (void)pthread_cond_broadcast(&gate.condition);
    (void)pthread_mutex_unlock(&gate.mutex);
    release_tracker_callbacks(&tracker);
    for (size_t index = 0; index < ACCESS_THREADS; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0 && stress[index].ok);
    }
    CHECK(wait_tracker_count(&tracker, true, STRESS_TASKS));

    Lardon3DTaskSnapshot retained[LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1];
    Lardon3DTaskQueueSummary summary;
    size_t retained_count = lardon3d_task_queue_snapshot(
        queue,
        retained,
        LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1,
        &summary
    );
    CHECK(retained_count <= LARDON3D_TASK_QUEUE_HISTORY_CAPACITY);
    for (size_t index = 1; index < retained_count; ++index) {
        CHECK(retained[index - 1].id > retained[index].id);
    }
    CHECK(summary.running == 0 && summary.pending == 0);
    CHECK(summary.completed == STRESS_TASKS && summary.total == STRESS_TASKS);
    lardon3d_task_queue_destroy(queue);
    CHECK(tracker_destroyed(&tracker) == STRESS_TASKS);
    CHECK(pthread_cond_destroy(&gate.condition) == 0);
    CHECK(pthread_mutex_destroy(&gate.mutex) == 0);
    lifetime_tracker_destroy(&tracker);
    return true;
}

static bool
test_shutdown_userdata_exact_once(Lardon3DResourceGovernor *governor)
{
    enum { SHUTDOWN_TASKS = 3 };
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(hold_resources(governor, &reservation));
    LifetimeTracker tracker = {0};
    CHECK(lifetime_tracker_init(&tracker));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, SHUTDOWN_TASKS);
    CHECK(queue);
    for (size_t index = 0; index < SHUTDOWN_TASKS; ++index) {
        Lardon3DTask *task = create_owned_task(
            "Arrêt exact", &estimate, &tracker, false, false);
        CHECK(task && lardon3d_task_queue_add(queue, task, NULL));
    }
    CHECK(tracker_destroyed(&tracker) == 0);
    lardon3d_task_queue_destroy(queue);
    CHECK(tracker_started(&tracker) == 0);
    CHECK(tracker_destroyed(&tracker) == SHUTDOWN_TASKS);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lifetime_tracker_destroy(&tracker);
    return true;
}

static bool
test_saturation_observes_active(Lardon3DResourceGovernor *governor)
{
    enum {
        PENDING_TASKS = 64,
        OBSERVATION_CAPACITY =
            2 * LARDON3D_TASK_QUEUE_HISTORY_CAPACITY + 1,
    };
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    LifetimeTracker tracker = {0};
    CHECK(lifetime_tracker_init(&tracker));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(
        governor, PENDING_TASKS);
    CHECK(queue);
    Lardon3DTask *active = create_owned_task(
        "Saturation active", &estimate, &tracker, true, false);
    uint64_t active_id = 0;
    CHECK(active && lardon3d_task_queue_add(queue, active, &active_id));
    CHECK(wait_tracker_count(&tracker, false, 1));
    for (size_t index = 0; index < PENDING_TASKS; ++index) {
        Lardon3DTask *pending = create_owned_task(
            "Saturation pending", &estimate, &tracker, false, false);
        CHECK(pending && lardon3d_task_queue_add(queue, pending, NULL));
    }

    Lardon3DTaskObservation observations[OBSERVATION_CAPACITY];
    Lardon3DTaskQueueSummary summary;
    size_t count = lardon3d_task_queue_observe(
        queue, observations, OBSERVATION_CAPACITY, &summary);
    CHECK(count == PENDING_TASKS + 1);
    CHECK(summary.running == 1 && summary.pending == PENDING_TASKS);
    bool active_found = false;
    for (size_t index = 0; index < count; ++index) {
        if (observations[index].id == active_id) {
            active_found = observations[index].state == TASK_RUNNING
                && observations[index].has_execution_contract
                && observations[index].execution_contract.cpu_threads > 0;
        }
    }
    CHECK(active_found);

    release_tracker_callbacks(&tracker);
    CHECK(wait_tracker_count(
        &tracker, true, (size_t)PENDING_TASKS + 1));
    lardon3d_task_queue_destroy(queue);
    lifetime_tracker_destroy(&tracker);
    return true;
}

static bool
test_enqueue_under_capacity(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 4);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[3];
    Lardon3DTask *tasks[3];
    uint64_t ids[3];
    for (size_t index = 0; index < 3; ++index) {
        work[index] = (QueueWork) {
            .log = &log,
            .value = index,
            .steps = 1,
        };
        tasks[index] = lardon3d_task_create(
            "Sous capacité",
            &estimate,
            queue_callback,
            &work[index]
        );
        CHECK(tasks[index]);
        CHECK(lardon3d_task_queue_add(queue, tasks[index], &ids[index]));
    }
    CHECK(lardon3d_task_queue_count(queue) <= 3);
    Lardon3DTaskSnapshot snapshot;
    for (size_t index = 0; index < 3; ++index) {
        CHECK(wait_terminal(queue, ids[index], &snapshot));
        CHECK(snapshot.state == TASK_COMPLETED);
    }
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_capacity_reached_blocks(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DResourceReservation *reservation;
    CHECK(hold_resources(governor, &reservation));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 2);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[3];
    Lardon3DTask *tasks[2];
    uint64_t ids[2];
    for (size_t index = 0; index < 2; ++index) {
        work[index] = (QueueWork) {
            .log = &log,
            .value = index,
            .steps = 1,
        };
        tasks[index] = lardon3d_task_create(
            "File pleine",
            &estimate,
            queue_callback,
            &work[index]
        );
        CHECK(tasks[index]);
        CHECK(lardon3d_task_queue_add(queue, tasks[index], &ids[index]));
    }
    CHECK(lardon3d_task_queue_count(queue) == 2);
    work[2] = (QueueWork) {.log = &log, .value = 2, .steps = 1};
    ProducerThread producer = {
        .queue = queue,
        .estimate = estimate,
        .work = &work[2],
    };
    QueueIngressProbe probe = {0};
    CHECK(queue_ingress_probe_init(&probe, queue, false));
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer_thread, &producer) == 0);
    CHECK(queue_ingress_probe_wait(
        &probe, LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING));
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_task_queue_resources_changed(queue);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(producer.added);
    queue_ingress_probe_disable(&probe);
    Lardon3DTaskSnapshot snapshot;
    for (size_t index = 0; index < 2; ++index) {
        CHECK(wait_terminal(queue, ids[index], &snapshot));
        CHECK(snapshot.state == TASK_COMPLETED);
    }
    CHECK(wait_terminal(queue, producer.id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_release_unblocks_producer(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DResourceReservation *reservation;
    CHECK(hold_resources(governor, &reservation));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[2];
    work[0] = (QueueWork) {.log = &log, .value = 0, .steps = 1};
    Lardon3DTask *first = lardon3d_task_create(
        "Première",
        &estimate,
        queue_callback,
        &work[0]
    );
    CHECK(first);
    uint64_t first_id;
    CHECK(lardon3d_task_queue_add(queue, first, &first_id));
    CHECK(lardon3d_task_queue_count(queue) == 1);
    work[1] = (QueueWork) {.log = &log, .value = 1, .steps = 1};
    ProducerThread producer = {
        .queue = queue,
        .estimate = estimate,
        .work = &work[1],
    };
    QueueIngressProbe probe = {0};
    CHECK(queue_ingress_probe_init(&probe, queue, false));
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer_thread, &producer) == 0);
    CHECK(queue_ingress_probe_wait(
        &probe, LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING));
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_task_queue_resources_changed(queue);
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, first_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(lardon3d_task_queue_remove(queue, first_id));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(producer.added);
    queue_ingress_probe_disable(&probe);
    CHECK(wait_terminal(queue, producer.id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_multiple_producers(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 4);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[4];
    ProducerThread producers[4];
    pthread_t threads[4];
    for (size_t index = 0; index < 4; ++index) {
        work[index] = (QueueWork) {
            .log = &log,
            .value = index,
            .steps = 1,
        };
        producers[index] = (ProducerThread) {
            .queue = queue,
            .estimate = estimate,
            .work = &work[index],
        };
        CHECK(pthread_create(
            &threads[index],
            NULL,
            producer_thread,
            &producers[index]
        ) == 0);
    }
    for (size_t index = 0; index < 4; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(producers[index].added);
    }
    CHECK(lardon3d_task_queue_count(queue) <= 4);
    Lardon3DTaskSnapshot snapshot;
    for (size_t index = 0; index < 4; ++index) {
        CHECK(wait_terminal(queue, producers[index].id, &snapshot));
        CHECK(snapshot.state == TASK_COMPLETED);
    }
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_shutdown_unblocks_producer(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DResourceReservation *reservation;
    CHECK(hold_resources(governor, &reservation));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[2];
    work[0] = (QueueWork) {.log = &log, .value = 0, .steps = 1};
    Lardon3DTask *first = lardon3d_task_create(
        "Première",
        &estimate,
        queue_callback,
        &work[0]
    );
    CHECK(first);
    uint64_t first_id;
    CHECK(lardon3d_task_queue_add(queue, first, &first_id));
    CHECK(lardon3d_task_queue_count(queue) == 1);
    work[1] = (QueueWork) {.log = &log, .value = 1, .steps = 1};
    ProducerThread producer = {
        .queue = queue,
        .estimate = estimate,
        .work = &work[1],
    };
    QueueIngressProbe probe = {0};
    CHECK(queue_ingress_probe_init(&probe, queue, false));
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer_thread, &producer) == 0);
    CHECK(queue_ingress_probe_wait(
        &probe, LARDON3D_TASK_QUEUE_TEST_PRODUCER_WAITING));
    /* The hook fires with the Queue mutex held after producer registration and
     * immediately before not_full wait. Destroy must wake and await that exact
     * in-progress add call, not a thread presumed to have run after a sleep. */
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(!producer.added);
    queue_ingress_probe_disable(&probe);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_shutdown_empty_queue(Lardon3DResourceGovernor *governor)
{
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    lardon3d_task_queue_destroy(queue);
    return true;
}

static bool
test_pending_count_correct(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DResourceReservation *reservation;
    CHECK(hold_resources(governor, &reservation));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 4);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[2];
    Lardon3DTask *tasks[2];
    uint64_t ids[2];
    for (size_t index = 0; index < 2; ++index) {
        work[index] = (QueueWork) {
            .log = &log,
            .value = index,
            .steps = 1,
        };
        tasks[index] = lardon3d_task_create(
            "Comptage",
            &estimate,
            queue_callback,
            &work[index]
        );
        CHECK(tasks[index]);
        CHECK(lardon3d_task_queue_add(queue, tasks[index], &ids[index]));
    }
    CHECK(lardon3d_task_queue_count(queue) == 2);
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_queue_cancel(queue, ids[0]));
    CHECK(wait_terminal(queue, ids[0], &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(lardon3d_task_queue_remove(queue, ids[0]));
    CHECK(lardon3d_task_queue_count(queue) == 1);
    CHECK(lardon3d_task_queue_cancel(queue, ids[1]));
    CHECK(wait_terminal(queue, ids[1], &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(lardon3d_task_queue_remove(queue, ids[1]));
    CHECK(lardon3d_task_queue_count(queue) == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_capacity_one(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DResourceReservation *reservation;
    CHECK(hold_resources(governor, &reservation));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work = {.log = &log, .value = 0, .steps = 1};
    Lardon3DTask *task = lardon3d_task_create(
        "Capacité un",
        &estimate,
        queue_callback,
        &work
    );
    CHECK(task);
    uint64_t id;
    CHECK(lardon3d_task_queue_add(queue, task, &id));
    CHECK(lardon3d_task_queue_count(queue) == 1);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_task_queue_resources_changed(queue);
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

static bool
test_stress_concurrent(Lardon3DResourceGovernor *governor)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 8);
    CHECK(queue);
    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    enum { PRODUCERS = 4, PER_PRODUCER = 4 };
    QueueWork work[PRODUCERS * PER_PRODUCER];
    ProducerThread producers[PRODUCERS * PER_PRODUCER];
    pthread_t threads[PRODUCERS * PER_PRODUCER];
    size_t index = 0;
    for (size_t producer = 0; producer < PRODUCERS; ++producer) {
        for (size_t item = 0; item < PER_PRODUCER; ++item) {
            work[index] = (QueueWork) {
                .log = &log,
                .value = index,
                .steps = 1,
            };
            producers[index] = (ProducerThread) {
                .queue = queue,
                .estimate = estimate,
                .work = &work[index],
            };
            CHECK(pthread_create(
                &threads[index],
                NULL,
                producer_thread,
                &producers[index]
            ) == 0);
            ++index;
        }
    }
    for (size_t i = 0; i < index; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(producers[i].added);
    }
    Lardon3DTaskSnapshot snapshot;
    for (size_t i = 0; i < index; ++i) {
        CHECK(wait_terminal(queue, producers[i].id, &snapshot));
        CHECK(snapshot.state == TASK_COMPLETED);
    }
    CHECK(log.count == index);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);
    return true;
}

typedef struct {
    Lardon3DResourceGovernor *governor;
    Lardon3DTaskExecutionContract first;
    Lardon3DTaskExecutionContract unchanged;
    Lardon3DTaskExecutionContract second;
    Lardon3DResourceCapabilitySelection first_selection;
    Lardon3DResourceCapabilitySelection unchanged_selection;
} ImmutableSequenceWork;

#ifdef __linux__
typedef struct {
    cpu_set_t worker_mask;
    cpu_set_t child_mask;
    bool child_started;
} AffinityWork;

static void *
capture_child_affinity(void *userdata)
{
    AffinityWork *work = userdata;
    work->child_started = sched_getaffinity(
        0, sizeof(work->child_mask), &work->child_mask) == 0;
    return NULL;
}

static bool
affinity_callback(Lardon3DTask *task, void *userdata)
{
    AffinityWork *work = userdata;
    pthread_t child;
    if (sched_getaffinity(0, sizeof(work->worker_mask), &work->worker_mask) != 0
        || pthread_create(&child, NULL, capture_child_affinity, work) != 0
        || pthread_join(child, NULL) != 0 || !work->child_started) {
        return false;
    }
    return lardon3d_task_set_progress(task, 100, "Affinité observée.");
}

static bool
test_worker_only_affinity(void)
{
    cpu_set_t main_before;
    CPU_ZERO(&main_before);
    if (sched_getaffinity(0, sizeof(main_before), &main_before) != 0) {
        return true;
    }
    Lardon3DResourceCpuTopologyInput topology = {
        .affinity_available = true,
        .topology_available = true,
    };
    for (unsigned int cpu = 0; cpu < CPU_SETSIZE
         && cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
        if (!CPU_ISSET((size_t)cpu, &main_before)) {
            continue;
        }
        size_t index = topology.allowed_cpu_count++;
        topology.allowed_cpu_ids[index] = cpu;
        topology.topology_entries[index] =
            (Lardon3DResourceCpuTopologyEntry) {
                .cpu_id = cpu,
                .package_id = 0,
                .core_id = (unsigned int)index,
            };
    }
    topology.topology_entry_count = topology.allowed_cpu_count;
    if (topology.allowed_cpu_count < 2
        || topology.allowed_cpu_count > UINT_MAX) {
        return true;
    }
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = (unsigned int)topology.allowed_cpu_count,
        .page_size_bytes = 4096,
        .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };

    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor
        && lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &topology));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    Lardon3DResourceCpuPolicyDiagnostic diagnostic;
    CHECK(lardon3d_resource_governor_internal_cpu_policy(
        governor, &diagnostic));
    CHECK(diagnostic.affinity_attempted && diagnostic.affinity_active
        && diagnostic.compute_cpu_count == topology.allowed_cpu_count - 1);
    cpu_set_t expected;
    CPU_ZERO(&expected);
    for (unsigned int cpu = 0; cpu < CPU_SETSIZE
         && cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
        if ((diagnostic.compute_mask[cpu / 64]
                & (UINT64_C(1) << (cpu % 64))) != 0) {
            CPU_SET((size_t)cpu, &expected);
        }
    }
    AffinityWork work = {0};
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Affinité worker", &estimate, "test.affinity", 1,
        affinity_callback, &work, NULL);
    uint64_t id = 0;
    CHECK(task && lardon3d_task_queue_add(queue, task, &id));
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, id, &snapshot)
        && snapshot.state == TASK_COMPLETED
        && CPU_EQUAL(&work.worker_mask, &expected)
        && CPU_EQUAL(&work.child_mask, &expected));
    cpu_set_t main_after;
    CPU_ZERO(&main_after);
    CHECK(sched_getaffinity(0, sizeof(main_after), &main_after) == 0
        && CPU_EQUAL(&main_before, &main_after));
    lardon3d_task_queue_destroy(queue);
    lardon3d_resource_governor_destroy(governor);

    /* An injected application failure must restore/retain the inherited mask,
     * remain observable, and still permit Queue-owned Task cleanup. */
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor
        && lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &topology));
    lardon3d_resource_governor_internal_force_worker_affinity_failure(
        governor, true);
    queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue && lardon3d_resource_governor_internal_cpu_policy(
        governor, &diagnostic));
    CHECK(diagnostic.affinity_attempted && !diagnostic.affinity_active
        && strcmp(diagnostic.reason, "worker-affinity-apply-failed") == 0);
    work = (AffinityWork) {0};
    task = lardon3d_task_create_typed(
        "Échec affinité worker", &estimate, "test.affinity.failure", 1,
        affinity_callback, &work, NULL);
    id = 0;
    CHECK(task && lardon3d_task_queue_add(queue, task, &id)
        && wait_terminal(queue, id, &snapshot)
        && snapshot.state == TASK_COMPLETED
        && CPU_EQUAL(&work.worker_mask, &main_before)
        && CPU_EQUAL(&work.child_mask, &main_before));
    lardon3d_task_queue_destroy(queue);
    lardon3d_resource_governor_destroy(governor);
    return true;
}
#else
static bool test_worker_only_affinity(void) { return true; }
#endif

static bool
immutable_sequence_callback(Lardon3DTask *task, void *userdata)
{
    ImmutableSequenceWork *work = userdata;
    if (!lardon3d_task_execution_contract(task, &work->first)
        || !lardon3d_task_internal_execution_selection(
            task, &work->first_selection)
        || work->first.gpu_slots != 1
        || work->first_selection.inflight_limit != 1
        || !lardon3d_resource_governor_internal_set_backend_available(
            work->governor,
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN,
            false
        )
        || !lardon3d_task_execution_contract(task, &work->unchanged)
        || !lardon3d_task_internal_execution_selection(
            task, &work->unchanged_selection)
        || work->first.batch_size != work->unchanged.batch_size
        || work->first.memory_bytes != work->unchanged.memory_bytes
        || work->first.gpu_memory_bytes != work->unchanged.gpu_memory_bytes
        || work->first.cpu_threads != work->unchanged.cpu_threads
        || work->first.gpu_slots != work->unchanged.gpu_slots
        || work->first.io_slots != work->unchanged.io_slots
        || work->first_selection.inflight_limit
            != work->unchanged_selection.inflight_limit) {
        return false;
    }
    Lardon3DResourceReservation *reservation = NULL;
    return lardon3d_task_sequence_break(
            task,
            work->governor,
            &reservation,
            &work->second
        )
        && work->second.gpu_slots == 0 && work->second.cpu_threads == 1;
}

static bool
test_sequence_contract_immutability(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 8,
        .page_size_bytes = 4096,
        .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
        .gpu_available = true,
        .gpu_uses_shared_memory = true,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .gpu_slot_capacity = 1,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor);
    CHECK(lardon3d_resource_governor_internal_set_backend_available(
        governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 1);
    CHECK(queue);
    Lardon3DResourceEstimate cpu = {
        .memory_bytes_per_item = 1024 * 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 4,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    Lardon3DResourceEstimate gpu = cpu;
    gpu.gpu_memory_fixed_bytes = 0;
    gpu.desired_cpu_threads = 1;
    gpu.desired_gpu_slots = 1;
    Lardon3DTaskCapabilityEnvelope envelope = {
        .count = 2,
        .capabilities = {
            {
                .estimate = gpu,
                .backend = LARDON3D_RESOURCE_BACKEND_ORB_VULKAN,
                .inflight_limit = 2,
                .minimum_inflight_limit = 1,
                .gpu_memory_bytes_per_inflight = 640 * 1024,
                .preferred = true,
                .inflight_adaptive = true,
                .requires_runtime_backend = true,
            },
            {
                .estimate = cpu,
                .backend = LARDON3D_RESOURCE_BACKEND_CPU,
                .inflight_limit = 1,
                .cpu_reducible = true,
            },
        },
    };
    ImmutableSequenceWork work = {.governor = governor};
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Contrat immuable",
        &cpu,
        "test.sequence",
        1,
        immutable_sequence_callback,
        &work,
        NULL
    );
    CHECK(task && lardon3d_task_internal_set_capability_envelope(task, &envelope));
    uint64_t id = 0;
    CHECK(lardon3d_task_queue_add(queue, task, &id));
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED && work.first.gpu_slots == 1
        && work.second.gpu_slots == 0);
    lardon3d_task_queue_destroy(queue);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

typedef struct {
    bool called;
    unsigned int cpu_threads;
} CpuEnvelopeWork;

static bool
cpu_envelope_callback(Lardon3DTask *task, void *userdata)
{
    CpuEnvelopeWork *work = userdata;
    Lardon3DTaskExecutionContract contract;
    if (!work || !lardon3d_task_execution_contract(task, &contract)) {
        return false;
    }
    work->called = true;
    work->cpu_threads = contract.cpu_threads;
    return true;
}

static bool
test_fixed_default_and_validated_cpu_range(void)
{
    /* An unknown kind must not acquire CPU adaptation merely because its
     * durable estimate asks for several threads. Only registered callbacks
     * whose output was validated across counts may consume a reduced count. */
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 1024,
        .page_size_bytes = 4096,
        .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_cpu_reserve = 1022,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor);
    Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = 1024,
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 4,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    CpuEnvelopeWork fixed_work = {0};
    Lardon3DTask *fixed = lardon3d_task_create_typed(
        "Enveloppe fixe", &estimate, "test.fixed", 1,
        cpu_envelope_callback, &fixed_work, NULL);
    CHECK(fixed);
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(lardon3d_task_internal_reserve_available(
        fixed, governor, &decision, &reservation));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT && !reservation
        && !fixed_work.called);
    lardon3d_task_destroy(fixed);

    CpuEnvelopeWork adaptive_work = {0};
    Lardon3DTask *adaptive = lardon3d_task_create_typed(
        "Enveloppe validée", &estimate, "features.extract", 1,
        cpu_envelope_callback, &adaptive_work, NULL);
    CHECK(adaptive);
    CHECK(lardon3d_task_internal_reserve_available(
        adaptive, governor, &decision, &reservation));
    CHECK((decision.kind == LARDON3D_RESOURCE_START
            || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH)
        && reservation);
    CHECK(lardon3d_task_start(adaptive, governor, reservation));
    CHECK(adaptive_work.called && adaptive_work.cpu_threads == 1);
    (void)lardon3d_resource_governor_release(governor, reservation);
    lardon3d_task_destroy(adaptive);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

int
main(void)
{
    if (!run_test() || !test_worker_only_affinity()
        || !test_sequence_contract_immutability()
        || !test_fixed_default_and_validated_cpu_range()) {
        return EXIT_FAILURE;
    }
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 1024,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = 0,
        .system_cpu_reserve = 0,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    if (!governor) {
        return EXIT_FAILURE;
    }
    bool ok = test_registered_ingress_blocks_destroy(governor)
        && test_generated_id_exhaustion_is_sticky(governor)
        && test_finished_read_only_reentrancy(governor)
        && test_terminal_lifetime(governor)
        && test_concurrent_terminal_access(governor)
        && test_shutdown_userdata_exact_once(governor)
        && test_saturation_observes_active(governor)
        && test_enqueue_under_capacity(governor)
        && test_capacity_reached_blocks(governor)
        && test_release_unblocks_producer(governor)
        && test_multiple_producers(governor)
        && test_shutdown_unblocks_producer(governor)
        && test_shutdown_empty_queue(governor)
        && test_pending_count_correct(governor)
        && test_capacity_one(governor)
        && test_stress_concurrent(governor);
    lardon3d_resource_governor_destroy(governor);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
