#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <lardon3d/resource_governor.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

#define GIBIBYTES(value) ((uint64_t)(value) * 1024 * 1024 * 1024)
#define MEBIBYTES(value) ((uint64_t)(value) * 1024 * 1024)

enum {
    THREAD_COUNT = 16,
    RESERVATIONS_PER_THREAD = 25,
    RESERVATION_COUNT = THREAD_COUNT * RESERVATIONS_PER_THREAD,
};

typedef struct {
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceSnapshot snapshot;
    Lardon3DResourceEstimate estimate;
    Lardon3DResourceReservation **reservations;
    size_t first;
    pthread_barrier_t *created;
    pthread_barrier_t *release;
    atomic_bool *failed;
} ReservationThread;

static Lardon3DResourcePolicy
test_policy(unsigned int io_slots)
{
    return (Lardon3DResourcePolicy) {
        .system_memory_reserve_bytes = 0,
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 0,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .gpu_slot_capacity = 0,
        .io_slot_capacity = io_slots,
    };
}

static void *
reserve_concurrently(void *argument)
{
    ReservationThread *thread = argument;
    for (size_t offset = 0; offset < RESERVATIONS_PER_THREAD; ++offset) {
        Lardon3DResourceDecision decision;
        size_t index = thread->first + offset;
        if (!lardon3d_resource_governor_reserve(
                thread->governor,
                &thread->snapshot,
                &thread->estimate,
                &decision,
                &thread->reservations[index]
            )
            || !thread->reservations[index]
            || decision.kind != LARDON3D_RESOURCE_START) {
            atomic_store(thread->failed, true);
        }
    }
    (void)pthread_barrier_wait(thread->created);
    (void)pthread_barrier_wait(thread->release);
    for (size_t offset = 0; offset < RESERVATIONS_PER_THREAD; ++offset) {
        Lardon3DResourceReservation *reservation =
            thread->reservations[thread->first + offset];
        if (!reservation
            || !lardon3d_resource_governor_release(
                thread->governor,
                reservation
            )
            || lardon3d_resource_governor_release(
                thread->governor,
                reservation
            )) {
            atomic_store(thread->failed, true);
        }
    }
    return NULL;
}

static bool
test_adaptive_batches(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 64,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(64),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = test_policy(64);
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(37),
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 1,
        .maximum_batch_size = 128,
        .desired_cpu_threads = 8,
        .desired_io_slots = 2,
        .task_class = LARDON3D_RESOURCE_TASK_MIXED,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(reservation);
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 37);
    CHECK(lardon3d_resource_governor_reservation_is_valid(
        governor,
        reservation
    ));
    Lardon3DResourceReservationInfo information;
    CHECK(lardon3d_resource_reservation_get(
        governor,
        reservation,
        &information
    ));
    CHECK(information.id == 1);
    CHECK(information.memory_bytes == GIBIBYTES(37));
    CHECK(information.batch_size == 37);
    CHECK(information.cpu_threads == 8);
    CHECK(information.io_slots == 2);
    CHECK(information.state == LARDON3D_RESERVATION_ACTIVE);
    CHECK(information.created_at.tv_sec > 0);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 1);

    Lardon3DResourceAvailability availability;
    CHECK(lardon3d_resource_governor_availability(
        governor,
        &snapshot,
        &availability
    ));
    CHECK(availability.memory_reserved_bytes == GIBIBYTES(37));
    CHECK(availability.memory_available_bytes == 0);
    CHECK(availability.cpu_reserved == 8);
    CHECK(availability.io_slots_reserved == 2);
    CHECK(availability.active_reservations == 1);
    Lardon3DResourcePolicy reduced_policy = policy;
    reduced_policy.system_memory_reserve_bytes = GIBIBYTES(28);
    CHECK(!lardon3d_resource_governor_set_policy(
        governor,
        &reduced_policy
    ));

    Lardon3DResourceReservation *second = NULL;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &second
    ));
    CHECK(!second);
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(!lardon3d_resource_governor_release(governor, reservation));
    CHECK(!lardon3d_resource_governor_reservation_is_valid(
        governor,
        reservation
    ));
    CHECK(lardon3d_resource_reservation_get(
        governor,
        reservation,
        &information
    ));
    CHECK(information.state == LARDON3D_RESERVATION_RELEASED);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    CHECK(lardon3d_resource_governor_availability(
        governor,
        &snapshot,
        &availability
    ));
    CHECK(availability.memory_available_bytes == GIBIBYTES(37));
    CHECK(lardon3d_resource_governor_set_policy(
        governor,
        &reduced_policy
    ));

    estimate = (Lardon3DResourceEstimate) {
        .memory_fixed_bytes = GIBIBYTES(65),
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
    };
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &second
    ));
    CHECK(!second);
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
test_multiple_reservations(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = test_policy(16);
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(8),
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate fixed = {
        .memory_fixed_bytes = GIBIBYTES(3),
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 2,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_IO,
    };
    Lardon3DResourceEstimate variable = {
        .memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 1,
        .maximum_batch_size = 6,
        .desired_cpu_threads = 2,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_IO,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *first;
    Lardon3DResourceReservation *second;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &fixed,
        &decision,
        &first
    ));
    CHECK(first && decision.kind == LARDON3D_RESOURCE_START);
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &variable,
        &decision,
        &second
    ));
    CHECK(second && decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 5);
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 2);
    Lardon3DResourceReservationInfo listed[2];
    CHECK(lardon3d_resource_governor_list_reservations(
        governor,
        listed,
        2
    ) == 2);
    CHECK(listed[0].id != listed[1].id);
    CHECK(lardon3d_resource_governor_release(governor, first));
    CHECK(lardon3d_resource_governor_release(governor, second));
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
test_gpu_reservation(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .gpu_available = true,
        .gpu_memory_known = true,
        .gpu_memory_total_bytes = GIBIBYTES(4),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = test_policy(4);
    policy.gpu_memory_reserve_bytes = MEBIBYTES(512);
    policy.gpu_slot_capacity = 2;
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    Lardon3DResourceGovernor *other = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor && other);
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(16),
        .gpu_memory_available_known = true,
        .gpu_memory_available_bytes = GIBIBYTES(3) + MEBIBYTES(512),
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate estimate = {
        .gpu_memory_fixed_bytes = MEBIBYTES(512),
        .gpu_memory_bytes_per_item = MEBIBYTES(512),
        .minimum_batch_size = 1,
        .maximum_batch_size = 6,
        .desired_cpu_threads = 2,
        .desired_gpu_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_GPU,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(reservation && decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 5);
    Lardon3DResourceReservationInfo information;
    CHECK(lardon3d_resource_reservation_get(
        governor,
        reservation,
        &information
    ));
    CHECK(information.gpu_memory_bytes == GIBIBYTES(3));
    CHECK(information.gpu_slots == 1);
    Lardon3DResourceAvailability availability;
    CHECK(lardon3d_resource_governor_availability(
        governor,
        &snapshot,
        &availability
    ));
    CHECK(availability.gpu_memory_reserved_bytes == GIBIBYTES(3));
    CHECK(availability.gpu_memory_available_bytes == 0);
    CHECK(availability.gpu_slots_available == 1);
    CHECK(!lardon3d_resource_governor_reservation_is_valid(
        other,
        reservation
    ));
    CHECK(!lardon3d_resource_governor_release(other, reservation));
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    lardon3d_resource_governor_destroy(other);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
test_concurrency(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 512,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(4),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = test_policy(512);
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    Lardon3DResourceReservation *reservations[RESERVATION_COUNT] = {0};
    pthread_barrier_t created;
    pthread_barrier_t release;
    CHECK(pthread_barrier_init(&created, NULL, THREAD_COUNT + 1) == 0);
    CHECK(pthread_barrier_init(&release, NULL, THREAD_COUNT + 1) == 0);
    atomic_bool failed = false;
    ReservationThread contexts[THREAD_COUNT];
    pthread_t threads[THREAD_COUNT];
    for (size_t index = 0; index < THREAD_COUNT; ++index) {
        contexts[index] = (ReservationThread) {
            .governor = governor,
            .snapshot = {
                .memory_available_bytes = GIBIBYTES(4),
                .cpu_load_1m = 0.0,
            },
            .estimate = {
                .memory_fixed_bytes = MEBIBYTES(1),
                .minimum_batch_size = 1,
                .maximum_batch_size = 1,
                .desired_cpu_threads = 1,
                .desired_io_slots = 1,
                .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
            },
            .reservations = reservations,
            .first = index * RESERVATIONS_PER_THREAD,
            .created = &created,
            .release = &release,
            .failed = &failed,
        };
        CHECK(pthread_create(
            &threads[index],
            NULL,
            reserve_concurrently,
            &contexts[index]
        ) == 0);
    }
    (void)pthread_barrier_wait(&created);
    CHECK(!atomic_load(&failed));
    CHECK(lardon3d_resource_governor_reservation_count(governor)
        == RESERVATION_COUNT);
    Lardon3DResourceAvailability availability;
    CHECK(lardon3d_resource_governor_availability(
        governor,
        &contexts[0].snapshot,
        &availability
    ));
    CHECK(availability.memory_reserved_bytes
        == MEBIBYTES(RESERVATION_COUNT));
    CHECK(availability.cpu_reserved == RESERVATION_COUNT);
    CHECK(availability.io_slots_reserved == RESERVATION_COUNT);
    (void)pthread_barrier_wait(&release);
    for (size_t index = 0; index < THREAD_COUNT; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    CHECK(!atomic_load(&failed));
    CHECK(lardon3d_resource_governor_reservation_count(governor) == 0);
    CHECK(pthread_barrier_destroy(&created) == 0);
    CHECK(pthread_barrier_destroy(&release) == 0);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
test_destroy_with_active_reservations(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 4,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(4),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = test_policy(4);
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(4),
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceEstimate estimate = {
        .memory_fixed_bytes = MEBIBYTES(1),
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = 1,
        .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(reservation);
    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_test(void)
{
    CHECK(test_adaptive_batches());
    CHECK(test_multiple_reservations());
    CHECK(test_gpu_reservation());
    CHECK(test_concurrency());
    CHECK(test_destroy_with_active_reservations());
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
