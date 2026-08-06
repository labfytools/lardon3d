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

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
