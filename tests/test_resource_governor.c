#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    Lardon3DResourceGovernor *governor;
    Lardon3DResourceSnapshot snapshot;
    Lardon3DResourceRequest request;
    atomic_bool *failed;
} ThreadContext;

typedef struct {
    Lardon3DResourceGovernor *governor;
    uint64_t observed_generation;
    uint64_t timeout_ns;
    atomic_bool *result;
} WaitContext;

static void *
wait_for_change_thread(void *argument)
{
    WaitContext *context = argument;
    bool changed = lardon3d_resource_governor_wait_for_change(
        context->governor,
        context->observed_generation,
        context->timeout_ns
    );
    atomic_store(context->result, changed);
    return NULL;
}

static void *
decide_repeatedly(void *argument)
{
    ThreadContext *context = argument;
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        Lardon3DResourceDecision decision;
        if (!lardon3d_resource_governor_decide(
                context->governor,
                &context->snapshot,
                &context->request,
                &decision
            )
            || decision.kind != LARDON3D_RESOURCE_START
            || decision.batch_size != context->request.preferred_batch_size) {
            (void)fprintf(
                stderr,
                "Décision concurrente : kind=%d batch=%zu cpu=%u\n",
                (int)decision.kind,
                decision.batch_size,
                decision.cpu_threads
            );
            atomic_store(context->failed, true);
            break;
        }
    }
    return NULL;
}

static bool
run_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy;
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    CHECK(policy.system_memory_reserve_bytes == GIBIBYTES(2));
    CHECK(policy.system_cpu_reserve == 1);
    policy = (Lardon3DResourcePolicy) {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 8,
    };
    CHECK(!lardon3d_resource_governor_create(NULL, &policy));
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);
    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(10),
        .cpu_load_1m = 2.0,
    };
    Lardon3DResourceRequest request = {
        .memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 16,
    };
    Lardon3DResourceDecision decision;
    CHECK(lardon3d_resource_governor_decide(
        governor,
        &snapshot,
        &request,
        &decision
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 8);
    CHECK(decision.cpu_threads == 15);
    CHECK(decision.reason[0]);
    Lardon3DResourceSnapshot invalid_snapshot = snapshot;
    invalid_snapshot.cpu_load_1m = -1.0;
    CHECK(!lardon3d_resource_governor_decide(
        governor,
        &invalid_snapshot,
        &request,
        &decision
    ));

    snapshot.memory_available_bytes = GIBIBYTES(6);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 4);
    snapshot.memory_available_bytes = GIBIBYTES(3);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);

    snapshot.memory_available_bytes = GIBIBYTES(10);
    snapshot.cpu_load_1m = 15.0;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    snapshot.cpu_load_1m = 2.0;
    snapshot.io_pressure_known = true;
    snapshot.io_pressure_avg10 = 90.0;
    request.io_intensive = true;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);

    request.io_intensive = false;
    request.memory_bytes_per_item = GIBIBYTES(8);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    request.memory_bytes_per_item = GIBIBYTES(1);
    request.gpu_memory_bytes_per_item = MEBIBYTES(512);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    lardon3d_resource_governor_destroy(governor);

    profile.gpu_available = true;
    profile.gpu_uses_shared_memory = true;
    policy.gpu_memory_reserve_bytes = 0;
    policy.gpu_slot_capacity = 2;
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor);
    request = (Lardon3DResourceRequest) {
        .memory_bytes_per_item = GIBIBYTES(1),
        .gpu_memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 1,
        .preferred_batch_size = 4,
        .requested_cpu_threads = 4,
    };
    snapshot.memory_available_bytes = GIBIBYTES(6);
    snapshot.cpu_load_1m = 2.0;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 2);
    lardon3d_resource_governor_destroy(governor);

    profile.gpu_available = true;
    profile.gpu_uses_shared_memory = false;
    profile.gpu_memory_known = true;
    profile.gpu_memory_total_bytes = GIBIBYTES(4);
    policy.gpu_memory_reserve_bytes = MEBIBYTES(512);
    governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor);
    request.minimum_batch_size = 1;
    request.preferred_batch_size = 3;
    request.gpu_memory_bytes_per_item = GIBIBYTES(1);
    snapshot.gpu_memory_available_known = false;
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    snapshot.gpu_memory_available_known = true;
    snapshot.gpu_memory_available_bytes = GIBIBYTES(2) + MEBIBYTES(512);
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &request, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(decision.batch_size == 2);

    request.gpu_memory_bytes_per_item = 0;
    request.preferred_batch_size = 2;
    request.memory_bytes_per_item = GIBIBYTES(1);
    atomic_bool failed = false;
    ThreadContext context = {
        .governor = governor,
        .snapshot = snapshot,
        .request = request,
        .failed = &failed,
    };
    pthread_t threads[8];
    for (size_t index = 0; index < 8; ++index) {
        CHECK(pthread_create(&threads[index], NULL, decide_repeatedly, &context) == 0);
    }
    for (size_t index = 0; index < 8; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    CHECK(!atomic_load(&failed));

    Lardon3DResourcePolicy invalid = policy;
    invalid.system_cpu_reserve = profile.logical_cpu_count;
    CHECK(!lardon3d_resource_governor_set_policy(governor, &invalid));
    CHECK(lardon3d_resource_governor_set_policy(governor, &policy));
    CHECK(strcmp(lardon3d_resource_decision_name(LARDON3D_RESOURCE_START), "Démarrer") == 0);
    lardon3d_resource_governor_destroy(governor);
    lardon3d_resource_governor_destroy(NULL);
    return true;
}

static bool
run_generation_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 8,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);

    /* Test 1 : génération initiale = 0. */
    CHECK(lardon3d_resource_governor_generation(governor) == 0);

    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(10),
        .cpu_load_1m = 2.0,
    };
    Lardon3DResourceEstimate estimate = {
        .memory_bytes_per_item = GIBIBYTES(1),
        .minimum_batch_size = 2,
        .maximum_batch_size = 8,
        .desired_cpu_threads = 4,
        .task_class = LARDON3D_RESOURCE_TASK_GENERAL,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;

    /* Test 2 : WAIT ne change pas la génération. */
    snapshot.cpu_load_1m = 15.0;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(reservation == NULL);
    CHECK(lardon3d_resource_governor_generation(governor) == 0);
    snapshot.cpu_load_1m = 2.0;

    /* Test 3 : REJECT ne change pas la génération. */
    estimate.memory_bytes_per_item = GIBIBYTES(8);
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_REJECT);
    CHECK(reservation == NULL);
    CHECK(lardon3d_resource_governor_generation(governor) == 0);
    estimate.memory_bytes_per_item = GIBIBYTES(1);

    /* Test 4 : création d'une réservation change la génération (0 -> 1). */
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_START
        || decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH);
    CHECK(reservation != NULL);
    CHECK(lardon3d_resource_governor_generation(governor) == 1);

    /* Test 5 : libération change la génération (1 -> 2). */
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_generation(governor) == 2);

    /* Test 6 : double libération refusée ne change pas la génération. */
    CHECK(!lardon3d_resource_governor_release(governor, reservation));
    CHECK(lardon3d_resource_governor_generation(governor) == 2);

    /* Test 7 : un thread attendant est réveillé après une libération. */
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(reservation != NULL);
    uint64_t generation = lardon3d_resource_governor_generation(governor);
    atomic_bool wait_result = false;
    WaitContext wait_context = {
        .governor = governor,
        .observed_generation = generation,
        .timeout_ns = 5000000000ULL,
        .result = &wait_result,
    };
    pthread_t waiter;
    CHECK(pthread_create(&waiter, NULL, wait_for_change_thread, &wait_context) == 0);
    CHECK(lardon3d_resource_governor_release(governor, reservation));
    CHECK(pthread_join(waiter, NULL) == 0);
    CHECK(atomic_load(&wait_result) == true);

    /* Test 8 : l'attente expire proprement (timeout court). */
    generation = lardon3d_resource_governor_generation(governor);
    CHECK(!lardon3d_resource_governor_wait_for_change(
        governor,
        generation,
        1000000ULL
    ));

    /* Test 9 : une décision WAIT ne modifie pas la génération et
     * wait_for_change() expire sans faux changement. */
    generation = lardon3d_resource_governor_generation(governor);
    snapshot.cpu_load_1m = 15.0;
    CHECK(lardon3d_resource_governor_reserve(
        governor,
        &snapshot,
        &estimate,
        &decision,
        &reservation
    ));
    CHECK(decision.kind == LARDON3D_RESOURCE_WAIT);
    CHECK(reservation == NULL);
    snapshot.cpu_load_1m = 2.0;
    CHECK(lardon3d_resource_governor_generation(governor) == generation);
    CHECK(!lardon3d_resource_governor_wait_for_change(
        governor,
        generation,
        1000000ULL
    ));

    /* Test 10 : plusieurs attentes successives fonctionnent. */
    for (size_t index = 0; index < 3; ++index) {
        CHECK(lardon3d_resource_governor_reserve(
            governor,
            &snapshot,
            &estimate,
            &decision,
            &reservation
        ));
        CHECK(reservation != NULL);
        generation = lardon3d_resource_governor_generation(governor);
        CHECK(lardon3d_resource_governor_release(governor, reservation));
        CHECK(lardon3d_resource_governor_wait_for_change(
            governor,
            generation,
            5000000000ULL
        ));
    }

    lardon3d_resource_governor_destroy(governor);
    return true;
}

static bool
run_adaptive_batch_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 16,
        .page_size_bytes = 4096,
        .memory_total_bytes = GIBIBYTES(16),
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_memory_reserve_bytes = GIBIBYTES(2),
        .gpu_memory_reserve_bytes = 0,
        .system_cpu_reserve = 1,
        .maximum_cpu_load_ratio = 0.90,
        .maximum_io_pressure_avg10 = 80.0,
        .io_slot_capacity = 8,
    };
    Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(
        &profile,
        &policy
    );
    CHECK(governor);

    Lardon3DResourceSnapshot snapshot = {
        .memory_available_bytes = GIBIBYTES(10),
        .cpu_load_1m = 2.0,
    };
    Lardon3DResourceDecision decision;

    /* Test 1: Without metrics, batch is unchanged */
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 2: Record batch matching estimate → no reduction */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        8,
        1000000000ULL,
        MEBIBYTES(800)
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 3: Record batch with higher memory usage → batch reduced */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        8,
        1000000000ULL,
        MEBIBYTES(1600)
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 4);

    /* Test 4: Buffer overflow (9 writes) → oldest overwritten */
    for (size_t i = 0; i < 9; ++i) {
        CHECK(lardon3d_resource_governor_record_batch(
            governor,
            LARDON3D_RESOURCE_TASK_GENERAL,
            4,
            1000000000ULL,
            MEBIBYTES(400)
        ));
    }
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 5: Different task class not affected */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_IO,
        8,
        1000000000ULL,
        MEBIBYTES(1600)
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 2,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 8);

    /* Test 6: record_batch increments generation */
    uint64_t gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        4,
        1000000000ULL,
        MEBIBYTES(400)
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before + 1);

    /* Test 7: NULL governor returns false */
    CHECK(!lardon3d_resource_governor_record_batch(NULL, LARDON3D_RESOURCE_TASK_GENERAL, 4, 0, 0));

    /* Test 8: Zero batch_size is ignored */
    CHECK(lardon3d_resource_governor_record_batch(governor, LARDON3D_RESOURCE_TASK_GENERAL, 0, 0, 0));

    /* T1: Invalid task class → false, generation unchanged */
    gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(!lardon3d_resource_governor_record_batch(
        governor,
        (Lardon3DResourceTaskClass)999,
        4,
        1000000000ULL,
        MEBIBYTES(400)
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before);

    /* T2: batch_size == 0 → true, generation unchanged */
    gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        0,
        0,
        0
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before);

    /* T3: minimum_batch_size guaranteed */
    for (size_t i = 0; i < 8; ++i) {
        CHECK(lardon3d_resource_governor_record_batch(
            governor,
            LARDON3D_RESOURCE_TASK_GENERAL,
            8,
            1000000000ULL,
            GIBIBYTES(8)
        ));
    }
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = MEBIBYTES(100),
        .minimum_batch_size = 4,
        .preferred_batch_size = 8,
        .requested_cpu_threads = 4,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size == 4);

    /* T4: Overflow of static_batch * memory_bytes_per_item in uint64_t.
     * With memory_bytes_per_item = 2 and preferred_batch_size = SIZE_MAX,
     * the multiplication SIZE_MAX * 2 would overflow uint64_t. The overflow
     * guard in adaptive_batch_limit returns 1 (most conservative). The result
     * is never SIZE_MAX, the adaptive limit never increases the batch, and
     * the final result is clamped to the minimum expected value. */
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_GENERAL,
        1,
        1000000000ULL,
        100
    ));
    CHECK(lardon3d_resource_governor_decide(governor, &snapshot, &(Lardon3DResourceRequest) {
        .memory_bytes_per_item = 2,
        .minimum_batch_size = 1,
        .preferred_batch_size = SIZE_MAX,
        .requested_cpu_threads = 1,
    }, &decision));
    CHECK(decision.kind == LARDON3D_RESOURCE_START);
    CHECK(decision.batch_size != SIZE_MAX);
    CHECK(decision.batch_size >= 1);
    CHECK(decision.batch_size <= 1);

    /* T5: generation increment only on real record */
    gen_before = lardon3d_resource_governor_generation(governor);
    CHECK(lardon3d_resource_governor_record_batch(
        governor,
        LARDON3D_RESOURCE_TASK_IO,
        4,
        1000000000ULL,
        MEBIBYTES(400)
    ));
    CHECK(lardon3d_resource_governor_generation(governor) == gen_before + 1);

    lardon3d_resource_governor_destroy(governor);
    return true;
}

int
main(void)
{
    return (run_test() && run_generation_test() && run_adaptive_batch_test())
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
