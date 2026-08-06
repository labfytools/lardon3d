#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <lardon3d/task_queue.h>

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
    CHECK(!lardon3d_task_queue_create(NULL));
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor);
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
    CHECK(lardon3d_task_queue_count(queue) == TASK_COUNT);
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
    CHECK(lardon3d_task_queue_count(queue) == TASK_COUNT - 1);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);

    queue = lardon3d_task_queue_create(governor);
    CHECK(queue);
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

    queue = lardon3d_task_queue_create(governor);
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
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &blocking_snapshot,
        &blocking_estimate,
        &decision,
        &blocking_reservation
    ));
    CHECK(blocking_reservation);
    queue = lardon3d_task_queue_create(governor);
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
    lardon3d_task_queue_resources_changed(queue);
    CHECK(wait_terminal(queue, awakened_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(awakened.contract_seen);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
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
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &io_blocking_snapshot,
        &io_blocking_estimate,
        &io_decision,
        &io_blocking_reservation
    ));
    CHECK(io_blocking_reservation);
    queue = lardon3d_task_queue_create(governor);
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
    lardon3d_task_queue_resources_changed(queue);
    CHECK(wait_terminal(queue, blocked_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(blocked.contract_seen);
    CHECK(bypass_log.count == 2);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&bypass_log.mutex) == 0);

    queue = lardon3d_task_queue_create(governor);
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
    CHECK(reduced_task);
    CHECK(lardon3d_task_queue_add(queue, reduced_task, &reduced_id));
    CHECK(wait_terminal(queue, reduced_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(reduced.contract.cpu_threads == 1024);
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

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
