#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
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
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceReservation *initial_reservation;
    unsigned int expected_breaks;
    unsigned int breaks_done;
    size_t batch_sizes[8];
    Lardon3DTaskExecutionContract initial_contract;
    bool got_initial_contract;
} SeqContext;

typedef struct {
    Lardon3DResourceGovernor *governor;
} WaitContext;

typedef struct {
    Lardon3DResourceGovernor *governor;
    pthread_mutex_t sync_mutex;
    pthread_cond_t sync_cond;
    bool about_to_break;
    bool cancel_ready;
} CancelContext;

typedef struct {
    Lardon3DTask *task;
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceReservation *reservation;
} StartContext;

static void
short_pause(long nanoseconds)
{
    struct timespec duration = {.tv_sec = 0, .tv_nsec = nanoseconds};
    (void)nanosleep(&duration, NULL);
}

static bool
seq_callback(Lardon3DTask *task, void *userdata)
{
    SeqContext *ctx = userdata;
    if (!lardon3d_task_execution_contract(task, &ctx->initial_contract)) {
        return false;
    }
    ctx->got_initial_contract = true;
    Lardon3DResourceReservation *previous = ctx->initial_reservation;
    for (unsigned int index = 0; index < ctx->expected_breaks; ++index) {
        if (!lardon3d_task_checkpoint(task)) {
            return false;
        }
        /* Vérifier que la réservation précédente est encore valide AVANT le break */
        if (previous && !lardon3d_resource_governor_reservation_is_valid(
                ctx->governor,
                previous
            )) {
            return false;
        }
        Lardon3DResourceReservation *reservation = NULL;
        Lardon3DTaskExecutionContract contract;
        if (!lardon3d_task_sequence_break(
                task,
                ctx->governor,
                &reservation,
                &contract
            )) {
            return false;
        }
        if (!lardon3d_resource_governor_reservation_is_valid(
                ctx->governor,
                reservation
            )) {
            return false;
        }
        ctx->batch_sizes[index] = contract.batch_size;
        previous = reservation;
        ++ctx->breaks_done;
        unsigned int progress = (unsigned int)(
            ((index + 1) * 100) / ctx->expected_breaks
        );
        if (!lardon3d_task_set_progress(task, progress, "Séquence.")) {
            return false;
        }
        short_pause(2000000);
    }
    return true;
}

static bool
wait_callback(Lardon3DTask *task, void *userdata)
{
    WaitContext *ctx = userdata;
    Lardon3DResourceReservation *reservation = NULL;
    Lardon3DTaskExecutionContract contract;
    return lardon3d_task_sequence_break(
        task,
        ctx->governor,
        &reservation,
        &contract
    );
}

static bool
cancel_callback(Lardon3DTask *task, void *userdata)
{
    CancelContext *ctx = userdata;
    if (!lardon3d_task_checkpoint(task)) {
        return false;
    }
    (void)pthread_mutex_lock(&ctx->sync_mutex);
    ctx->about_to_break = true;
    (void)pthread_cond_broadcast(&ctx->sync_cond);
    while (!ctx->cancel_ready) {
        (void)pthread_cond_wait(&ctx->sync_cond, &ctx->sync_mutex);
    }
    (void)pthread_mutex_unlock(&ctx->sync_mutex);
    Lardon3DResourceReservation *reservation = NULL;
    Lardon3DTaskExecutionContract contract;
    return lardon3d_task_sequence_break(
        task,
        ctx->governor,
        &reservation,
        &contract
    );
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
run_sequential_test(void)
{
    const Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = 1000,
        .minimum_batch_size = 1,
        .maximum_batch_size = 1000,
        .desired_cpu_threads = 1,
    };
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
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
    CHECK(governor);
    Lardon3DResourceSnapshot resource_snapshot = {
        .memory_available_bytes = 20000,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &resource_snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_START
        || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 20);

    SeqContext ctx = {
        .governor = governor,
        .initial_reservation = reservation,
        .expected_breaks = 3,
    };
    Lardon3DTask *task = lardon3d_task_create(
        "Séquences",
        &estimate,
        seq_callback,
        &ctx
    );
    CHECK(task);
    StartContext start = {task, governor, reservation};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, start_task, &start) == 0);
    CHECK(wait_for_state(task, TASK_RUNNING));
    CHECK(lardon3d_task_join(task));
    void *thread_result;
    CHECK(pthread_join(thread, &thread_result) == 0);
    CHECK((uintptr_t)thread_result == 1);
    CHECK(ctx.got_initial_contract);
    CHECK(ctx.breaks_done == 3);
    CHECK(ctx.initial_contract.batch_size == 20);
    CHECK(ctx.batch_sizes[0] == 1000);
    CHECK(ctx.batch_sizes[1] == 1000);
    CHECK(ctx.batch_sizes[2] == 1000);
    CHECK(ctx.batch_sizes[0] != ctx.initial_contract.batch_size);
    CHECK(lardon3d_task_sequence_count(task) == 3);
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(snapshot.progress == 100);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    lardon3d_task_destroy(task);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_wait_test(void)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 100,
        .desired_cpu_threads = 1,
        .desired_io_slots = 1,
    };
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 0.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    Lardon3DResourceSnapshot resource_snapshot = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
        .io_pressure_known = false,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &resource_snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    WaitContext ctx = {.governor = governor};
    Lardon3DTask *task = lardon3d_task_create(
        "Attente",
        &estimate,
        wait_callback,
        &ctx
    );
    CHECK(task);
    CHECK(lardon3d_task_start(task, governor, reservation));
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_FAILED);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    lardon3d_task_destroy(task);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_cancel_test(void)
{
    const Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 100,
        .desired_cpu_threads = 1,
    };
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
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
    CHECK(governor);
    Lardon3DResourceSnapshot resource_snapshot = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &resource_snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CancelContext ctx = {.governor = governor};
    CHECK(pthread_mutex_init(&ctx.sync_mutex, NULL) == 0);
    CHECK(pthread_cond_init(&ctx.sync_cond, NULL) == 0);
    Lardon3DTask *task = lardon3d_task_create(
        "Annulation",
        &estimate,
        cancel_callback,
        &ctx
    );
    CHECK(task);
    StartContext start = {task, governor, reservation};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, start_task, &start) == 0);
    (void)pthread_mutex_lock(&ctx.sync_mutex);
    while (!ctx.about_to_break) {
        (void)pthread_cond_wait(&ctx.sync_cond, &ctx.sync_mutex);
    }
    (void)pthread_mutex_unlock(&ctx.sync_mutex);
    lardon3d_task_request_cancel(task);
    (void)pthread_mutex_lock(&ctx.sync_mutex);
    ctx.cancel_ready = true;
    (void)pthread_cond_broadcast(&ctx.sync_cond);
    (void)pthread_mutex_unlock(&ctx.sync_mutex);
    CHECK(lardon3d_task_join(task));
    CHECK(pthread_join(thread, NULL) == 0);
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    lardon3d_task_destroy(task);
    (void)pthread_cond_destroy(&ctx.sync_cond);
    (void)pthread_mutex_destroy(&ctx.sync_mutex);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

int
main(void)
{
    if (!run_sequential_test()) {
        return EXIT_FAILURE;
    }
    if (!run_wait_test()) {
        return EXIT_FAILURE;
    }
    if (!run_cancel_test()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}