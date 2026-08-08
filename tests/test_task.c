#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/task.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

typedef struct {
    size_t steps;
    long pause_ns;
} Work;

typedef struct {
    Lardon3DTask *task;
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceReservation *reservation;
} StartContext;

typedef struct {
    Lardon3DResourceGovernor *governor;
    size_t count;
    Lardon3DTaskState state;
    bool snapshot_succeeded;
    bool reservation_released;
} FinishProbe;

static void
finished_callback(const Lardon3DTask *task, void *userdata)
{
    FinishProbe *probe = userdata;
    Lardon3DTaskSnapshot snapshot;
    ++probe->count;
    probe->snapshot_succeeded = lardon3d_task_snapshot(task, &snapshot);
    if (probe->snapshot_succeeded) probe->state = snapshot.state;
    probe->reservation_released = !probe->governor
        || lardon3d_resource_governor_reservation_count(probe->governor) == 0;
}

static void
short_pause(long nanoseconds)
{
    struct timespec duration = {.tv_sec = 0, .tv_nsec = nanoseconds};
    (void)nanosleep(&duration, NULL);
}

static bool
work_callback(Lardon3DTask *task, void *userdata)
{
    Work *work = userdata;
    for (size_t step = 0; step < work->steps; ++step) {
        if (!lardon3d_task_checkpoint(task)) {
            return false;
        }
        unsigned int progress = (unsigned int)(((step + 1) * 100) / work->steps);
        if (!lardon3d_task_set_progress(task, progress, "Traitement.")) {
            return false;
        }
        short_pause(work->pause_ns);
    }
    return true;
}

static bool
failure_callback(Lardon3DTask *task, void *userdata)
{
    (void)userdata;
    return lardon3d_task_fail(task, "Erreur contrôlée.") && false;
}

static void *
start_task(void *context)
{
    StartContext *start = context;
    return (void *)(uintptr_t)(lardon3d_task_start(
        start->task,
        start->governor,
        start->reservation
    ) ? 1 : 0);
}

static bool
wait_for_state(Lardon3DTask *task, Lardon3DTaskState expected)
{
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        Lardon3DTaskSnapshot snapshot;
        if (!lardon3d_task_snapshot(task, &snapshot)) {
            return false;
        }
        if (snapshot.state == expected) {
            return true;
        }
        short_pause(1000000);
    }
    return false;
}

static bool
run_test(void)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
    };
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 4,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    Lardon3DResourceSnapshot resource_snapshot = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(governor);
    CHECK(!lardon3d_task_create(NULL, &estimate, work_callback, NULL));
    CHECK(!lardon3d_task_create("", &estimate, work_callback, NULL));
    CHECK(!lardon3d_task_create("invalide", &estimate, NULL, NULL));
    lardon3d_task_destroy(NULL);
    CHECK(!lardon3d_task_join(NULL));

    Work work = {.steps = 100, .pause_ns = 1000000};
    Lardon3DTask *task = lardon3d_task_create(
        "Tâche de test",
        &estimate,
        work_callback,
        &work
    );
    CHECK(task);
    FinishProbe completed_probe = {.governor = governor};
    CHECK(lardon3d_task_set_finished_callback(task, finished_callback,
        &completed_probe));
    CHECK(lardon3d_task_assign_id(task, 42));
    CHECK(!lardon3d_task_assign_id(task, 43));
    CHECK(lardon3d_task_id(task) == 42);
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_PENDING);
    CHECK(snapshot.progress == 0);
    CHECK(strcmp(snapshot.name, "Tâche de test") == 0);

    pthread_t thread;
    CHECK(lardon3d_resource_governor_reserve(
        governor, &resource_snapshot, &estimate, &decision, &reservation
    ));
    StartContext start = {task, governor, reservation};
    CHECK(pthread_create(&thread, NULL, start_task, &start) == 0);
    CHECK(wait_for_state(task, TASK_RUNNING));
    CHECK(lardon3d_task_pause(task));
    CHECK(wait_for_state(task, TASK_PAUSED));
    CHECK(completed_probe.count == 0);
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    unsigned int paused_progress = snapshot.progress;
    short_pause(5000000);
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.progress == paused_progress);
    CHECK(lardon3d_task_resume(task));
    CHECK(wait_for_state(task, TASK_RUNNING));
    CHECK(lardon3d_task_join(task));
    void *thread_result;
    CHECK(pthread_join(thread, &thread_result) == 0);
    CHECK((uintptr_t)thread_result == 1);
    CHECK(!lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(snapshot.progress == 100);
    CHECK(snapshot.started_at.tv_sec > 0);
    CHECK(snapshot.finished_at.tv_sec > 0);
    CHECK(completed_probe.count == 1 && completed_probe.snapshot_succeeded
        && completed_probe.state == TASK_COMPLETED
        && completed_probe.reservation_released);
    CHECK(!lardon3d_task_start(task, NULL, NULL));
    lardon3d_task_destroy(task);

    work = (Work) {.steps = 1000, .pause_ns = 1000000};
    task = lardon3d_task_create("Annulation", &estimate, work_callback, &work);
    CHECK(task);
    FinishProbe cancelled_probe = {.governor = governor};
    CHECK(lardon3d_task_set_finished_callback(task, finished_callback,
        &cancelled_probe));
    CHECK(lardon3d_resource_governor_reserve(
        governor, &resource_snapshot, &estimate, &decision, &reservation
    ));
    start = (StartContext) {task, governor, reservation};
    CHECK(pthread_create(&thread, NULL, start_task, &start) == 0);
    CHECK(wait_for_state(task, TASK_RUNNING));
    lardon3d_task_request_cancel(task);
    CHECK(lardon3d_task_join(task));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(!lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(snapshot.progress < 100);
    CHECK(cancelled_probe.count == 1 && cancelled_probe.snapshot_succeeded
        && cancelled_probe.state == TASK_CANCELLED
        && cancelled_probe.reservation_released);
    lardon3d_task_destroy(task);

    task = lardon3d_task_create("Échec", &estimate, failure_callback, NULL);
    CHECK(task);
    FinishProbe failed_probe = {.governor = governor};
    CHECK(lardon3d_task_set_finished_callback(task, finished_callback,
        &failed_probe));
    CHECK(lardon3d_resource_governor_reserve(
        governor, &resource_snapshot, &estimate, &decision, &reservation
    ));
    CHECK(lardon3d_task_start(task, governor, reservation));
    CHECK(!lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_FAILED);
    CHECK(strcmp(snapshot.message, "Erreur contrôlée.") == 0);
    CHECK(failed_probe.count == 1 && failed_probe.snapshot_succeeded
        && failed_probe.state == TASK_FAILED
        && failed_probe.reservation_released);
    lardon3d_task_destroy(task);

    task = lardon3d_task_create(
        "Pause avant départ",
        &estimate,
        work_callback,
        &work
    );
    CHECK(task && lardon3d_task_pause(task));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_PAUSED);
    CHECK(lardon3d_task_resume(task));
    FinishProbe pending_cancel_probe = {0};
    CHECK(lardon3d_task_set_finished_callback(task, finished_callback,
        &pending_cancel_probe));
    lardon3d_task_request_cancel(task);
    lardon3d_task_request_cancel(task);
    CHECK(lardon3d_task_join(task));
    CHECK(pending_cancel_probe.count == 1
        && pending_cancel_probe.state == TASK_CANCELLED);
    lardon3d_task_destroy(task);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
