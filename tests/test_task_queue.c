#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/task_queue.h>

#include "../src/resource_governor_internal.h"
#include "../src/task_internal.h"
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

typedef struct {
    Lardon3DTaskQueue *queue;
    Lardon3DResourceEstimate estimate;
    QueueWork *work;
    uint64_t id;
    bool added;
} ProducerThread;

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
    CHECK(log.count == TASK_COUNT);
    for (size_t index = 0; index < TASK_COUNT; ++index) {
        CHECK(log.order[index] == index);
        CHECK(work[index].contract_seen);
        CHECK(work[index].contract.batch_size == 1);
    }
    Lardon3DTaskQueueSummary summary;
    Lardon3DTaskSnapshot listed[8];
    CHECK(lardon3d_task_queue_snapshot(queue, listed, 8, &summary) == 8);
    CHECK(summary.total == TASK_COUNT);
    CHECK(summary.running == 0);
    CHECK(summary.pending == 0);
    CHECK(summary.completed == TASK_COUNT);
    CHECK(lardon3d_task_queue_get_at(queue, 0, &snapshot));
    CHECK(snapshot.id == ids[0]);
    CHECK(!lardon3d_task_queue_get_at(queue, TASK_COUNT, &snapshot));
    CHECK(lardon3d_task_queue_remove(queue, ids[0]));
    CHECK(!lardon3d_task_queue_get(queue, ids[0], &snapshot));
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
    CHECK(lardon3d_task_pause(slow_task));
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
    CHECK(lardon3d_task_resume(slow_task));
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
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer_thread, &producer) == 0);
    short_pause();
    CHECK(!producer.added);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_task_queue_resources_changed(queue);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(producer.added);
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
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer_thread, &producer) == 0);
    short_pause();
    CHECK(!producer.added);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_task_queue_resources_changed(queue);
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, first_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(lardon3d_task_queue_remove(queue, first_id));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(producer.added);
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
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, producer_thread, &producer) == 0);
    short_pause();
    CHECK(!producer.added);
    // destroy réveille le producteur via broadcast(&not_full)
    lardon3d_task_queue_destroy(queue);
    // Le producteur peut maintenant sortir de add et terminer
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(!producer.added);
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
    bool ok = test_enqueue_under_capacity(governor)
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
