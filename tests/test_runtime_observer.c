#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_observer_internal.h"

#define GIB (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "runtime observer failure line %d: %s\n",\
                __LINE__, #condition);                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    uint64_t now_ns;
    size_t capture_calls;
    bool fail_capture;
    bool destroyed;
    Lardon3DRuntimeObserverSample sample;
} FakeObserver;

static bool
fake_now(void *context, uint64_t *now_ns)
{
    FakeObserver *fake = context;
    *now_ns = fake->now_ns;
    return true;
}

static bool
fake_capture(
    void *context,
    Lardon3DRuntimeObserverSample *sample,
    char reason[LARDON3D_TUI_TEXT_CAPACITY]
)
{
    FakeObserver *fake = context;
    ++fake->capture_calls;
    if (fake->fail_capture) {
        (void)snprintf(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "injected bounded capture failure");
        return false;
    }
    *sample = fake->sample;
    reason[0] = '\0';
    return true;
}

static void
fake_destroy(void *context)
{
    ((FakeObserver *)context)->destroyed = true;
}

static const Lardon3DRuntimeObserverProviderOps fake_ops = {
    .monotonic_now_ns = fake_now,
    .capture = fake_capture,
    .destroy = fake_destroy,
};

static bool production_stub_enabled;
static unsigned int last_diagnostic_calls;
static Lardon3DTaskObservation production_stub_task;

/* The production-owner stubs are enabled only for the final association
 * regression. They supply owners, not model results, so the same production
 * observer builder remains under test. */
size_t
lardon3d_task_queue_observe(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskObservation *observations,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
)
{
    (void)queue;
    if (summary) {
        *summary = (Lardon3DTaskQueueSummary) {0};
    }
    if (!production_stub_enabled || !observations || capacity == 0) {
        return 0;
    }
    observations[0] = production_stub_task;
    if (summary) {
        summary->running = 1;
        summary->total = 1;
    }
    return 1;
}

bool
lardon3d_resource_observation_capture(
    const Lardon3DHardwareProfile *profile,
    Lardon3DResourceObservation *observation,
    char *error_message,
    size_t error_message_size
)
{
    (void)profile;
    if (error_message && error_message_size) {
        error_message[0] = '\0';
    }
    if (!production_stub_enabled || !observation) {
        return false;
    }
    *observation = (Lardon3DResourceObservation) {
        .snapshot = {
            .memory_available_bytes = 8 * GIB,
            .swap_available_bytes = 3 * GIB,
        },
        .swap_total_known = true,
        .swap_total_bytes = 4 * GIB,
    };
    return true;
}

bool
lardon3d_resource_governor_availability(
    Lardon3DResourceGovernor *governor,
    const Lardon3DResourceSnapshot *snapshot,
    Lardon3DResourceAvailability *availability
)
{
    (void)governor;
    (void)snapshot;
    if (!production_stub_enabled || !availability) {
        return false;
    }
    *availability = (Lardon3DResourceAvailability) {
        .memory_budget_bytes = 13 * GIB,
        .cpu_budget = 4,
        .cpu_reserved = 2,
        .cpu_available = 2,
    };
    return true;
}

bool
lardon3d_resource_governor_get_policy(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourcePolicy *policy
)
{
    (void)governor;
    if (!production_stub_enabled || !policy) {
        return false;
    }
    *policy = (Lardon3DResourcePolicy) {
        .system_memory_reserve_bytes = 3 * GIB,
    };
    return true;
}

bool
lardon3d_resource_governor_get_external_storage(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceExternalStorage *storage
)
{
    (void)governor;
    if (storage) {
        *storage = (Lardon3DResourceExternalStorage) {0};
    }
    if (!production_stub_enabled || !storage) {
        return false;
    }
    *storage = (Lardon3DResourceExternalStorage) {
        .generation = 9,
        .status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_AVAILABLE,
        .new_scratch_allocations_allowed = true,
    };
    (void)snprintf(storage->stable_identity,
        sizeof(storage->stable_identity), "drive-production");
    (void)snprintf(storage->reason, sizeof(storage->reason),
        "registered production snapshot");
    return true;
}

Lardon3DResourcePressure
lardon3d_resource_governor_pressure(Lardon3DResourceGovernor *governor)
{
    (void)governor;
    return LARDON3D_RESOURCE_PRESSURE_YELLOW;
}

bool
lardon3d_resource_governor_internal_cpu_policy(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceCpuPolicyDiagnostic *diagnostic
)
{
    (void)governor;
    if (!production_stub_enabled || !diagnostic) {
        return false;
    }
    *diagnostic = (Lardon3DResourceCpuPolicyDiagnostic) {
        .compute_cpu_count = 4,
    };
    (void)snprintf(diagnostic->reason, sizeof(diagnostic->reason),
        "whole-core compute pool");
    return true;
}

bool
lardon3d_resource_governor_internal_last_diagnostic(
    Lardon3DResourceGovernor *governor,
    const char *task_kind,
    uint32_t task_kind_version,
    Lardon3DResourceSequenceDiagnostic *diagnostic
)
{
    (void)governor;
    (void)task_kind;
    (void)task_kind_version;
    (void)diagnostic;
    ++last_diagnostic_calls;
    return false;
}

const char *
lardon3d_task_state_name(Lardon3DTaskState state)
{
    static const char *const names[] = {
        "PENDING", "RUNNING", "PAUSED", "CANCELLED", "FAILED", "COMPLETED",
    };
    return state >= TASK_PENDING && state <= TASK_COMPLETED
        ? names[state] : "UNKNOWN";
}

static void
prepare_sample(FakeObserver *fake)
{
    fake->sample = (Lardon3DRuntimeObserverSample) {
        .realtime_now = {.tv_sec = 100, .tv_nsec = 0},
        .task_count = 1,
        .task_summary = {.running = 1, .total = 1},
        .resource_valid = true,
        .resource_observation = {
            .snapshot = {
                .memory_available_bytes = 8 * GIB,
                .swap_available_bytes = 3 * GIB,
                .memory_pressure_known = true,
                .memory_pressure_avg10 = 0.0,
                .swap_activity_known = true,
                .swap_pages_in = 10,
                .swap_pages_out = 20,
            },
            .swap_total_known = true,
            .swap_total_bytes = 4 * GIB,
        },
        .availability = {
            .memory_budget_bytes = 13 * GIB,
            .memory_reserved_bytes = GIB,
            .cpu_budget = 4,
            .cpu_reserved = 2,
            .cpu_available = 2,
            .gpu_memory_known = true,
            .gpu_memory_reserved_bytes = 128 * UINT64_C(1024) * 1024,
            .gpu_memory_available_bytes = 512 * UINT64_C(1024) * 1024,
            .gpu_slot_budget = 1,
            .gpu_slots_reserved = 1,
            .io_slot_budget = 2,
            .io_slots_reserved = 1,
            .io_slots_available = 1,
        },
        .policy = {.system_memory_reserve_bytes = 3 * GIB},
        .cpu_policy = {.compute_cpu_count = 4},
        .pressure = LARDON3D_RESOURCE_PRESSURE_GREEN,
        .external_storage_registered = true,
        .external_storage = {
            .generation = 7,
            .status = LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE,
            .new_scratch_allocations_allowed = true,
            .scratch_total_known = true,
            .scratch_free_known = true,
            .scratch_total_bytes = 8 * GIB,
            .scratch_free_bytes = 6 * GIB,
            .swap_total_known = true,
            .swap_used_known = true,
            .swap_total_bytes = 4 * GIB,
            .swap_used_bytes = GIB,
            .active_scratch_leases = 2,
            .stable_identity = "drive-1",
            .reason = "registered and in use",
        },
        .ssd_controller_available = true,
        .ssd_snapshot_valid = true,
        .ssd = {
            .state = LARDON3D_SSD_IN_USE,
            .scratch_mounted = true,
            .scratch_lease_count = 2,
        },
    };
    (void)snprintf(fake->sample.cpu_policy.reason,
        sizeof(fake->sample.cpu_policy.reason), "whole-core compute pool");
    fake->sample.tasks[0] = (Lardon3DTaskObservation) {
        .id = 91,
        .has_task_kind = true,
        .task_kind_version = 1,
        .progress = 25,
        .durable_progress_known = true,
        .durable_completed = 25,
        .durable_total = 100,
        .state = TASK_RUNNING,
        .started_at = {.tv_sec = 90, .tv_nsec = 0},
        .has_execution_contract = true,
        .execution_contract = {
            .batch_size = 8,
            .cpu_threads = 2,
            .gpu_slots = 1,
            .io_slots = 1,
        },
    };
    (void)snprintf(fake->sample.tasks[0].name,
        sizeof(fake->sample.tasks[0].name), "matcher");
    (void)snprintf(fake->sample.tasks[0].task_kind,
        sizeof(fake->sample.tasks[0].task_kind), "matcher.run");
    (void)snprintf(fake->sample.tasks[0].message,
        sizeof(fake->sample.tasks[0].message), "matching");
}

static bool
run_test(void)
{
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = 8,
        .memory_total_bytes = 16 * GIB,
        .gpu_available = true,
        .gpu_uses_shared_memory = true,
    };
    FakeObserver fake = {.now_ns = UINT64_C(2000000000)};
    prepare_sample(&fake);
    Lardon3DRuntimeObserver *observer =
        lardon3d_runtime_observer_create_with_provider(
            &profile,
            (Lardon3DRuntimeObserverProvider) {
                .ops = &fake_ops,
                .context = &fake,
            },
            UINT64_C(1000000000));
    CHECK(observer);

    Lardon3DRuntimeSnapshot view;
    CHECK(lardon3d_runtime_observer_refresh(observer, true, true, &view));
    CHECK(fake.capture_calls == 1);
    CHECK(view.generation == 1);
    CHECK(view.active_task_known && view.active_task_index == 0);
    CHECK(view.stages[LARDON3D_TUI_STAGE_MATCHER].state
        == LARDON3D_TUI_STAGE_RUNNING);
    CHECK(view.stages[LARDON3D_TUI_STAGE_DENSE].state
        == LARDON3D_TUI_STAGE_NOT_APPLICABLE);
    CHECK(view.active_progress.percentage == 25);
    CHECK(view.active_progress.durable_counts_known);
    CHECK(view.active_progress.completed == 25);
    CHECK(view.active_progress.total == 100);
    CHECK(view.active_progress.elapsed_known);
    CHECK(view.active_progress.elapsed_seconds == 10);
    CHECK(view.resources.valid);
    CHECK(view.resources.cpu_logical_total == 8);
    CHECK(view.resources.cpu_active == 2);
    CHECK(view.resources.cpu_admitted_known);
    CHECK(view.resources.cpu_admitted == 2);
    CHECK(view.resources.cpu_available == 2);
    CHECK(view.resources.gpu_slots_active == 1);
    CHECK(!view.resources.gpu_busy_known);
    CHECK(view.resources.ram_reserve_bytes == 3 * GIB);
    CHECK(view.resources.swap_total_bytes == 4 * GIB);
    CHECK(view.resources.swap_used_bytes == GIB);
    CHECK(!view.resources.swap_delta_known);
    CHECK(view.resources.gpu_uses_shared_memory);
    CHECK(view.resources.batch_known);
    CHECK(view.resources.batch_size == 8);
    CHECK(!view.resources.inflight_known);
    CHECK(!view.resources.helpers_known);
    CHECK(view.resources.gpu_backend == LARDON3D_TUI_GPU_BACKEND_UNKNOWN);
    CHECK(view.resources.scratch_known && view.resources.scratch_leases == 2);
    CHECK(view.resources.external_storage_registered);
    CHECK(view.resources.external_storage_status
        == LARDON3D_RESOURCE_EXTERNAL_STORAGE_IN_USE);
    CHECK(view.resources.scratch_new_allocations_allowed);
    CHECK(view.resources.external_swap_used_bytes == GIB);
    CHECK(strcmp(view.resources.external_storage_identity, "drive-1") == 0);
    CHECK(view.ssd.state == LARDON3D_SSD_IN_USE);

    fake.now_ns += UINT64_C(500000000);
    CHECK(lardon3d_runtime_observer_refresh(observer, true, false, &view));
    CHECK(fake.capture_calls == 1);
    CHECK(view.generation == 1);

    fake.now_ns += UINT64_C(500000000);
    fake.sample.resource_observation.snapshot.swap_pages_in = 12;
    fake.sample.resource_observation.snapshot.swap_pages_out = 25;
    CHECK(lardon3d_runtime_observer_refresh(observer, true, false, &view));
    CHECK(fake.capture_calls == 2);
    CHECK(view.resources.swap_delta_known);
    CHECK(view.resources.swap_pages_in_delta == 2);
    CHECK(view.resources.swap_pages_out_delta == 5);

    /* A project identity transition invalidates coalescing immediately. */
    fake.now_ns += 1;
    CHECK(lardon3d_runtime_observer_refresh(observer, false, false, &view));
    CHECK(fake.capture_calls == 3);
    CHECK(view.task_count == 0);
    CHECK(!view.active_task_known);
    CHECK(view.task_summary.total == 0);

    fake.now_ns += UINT64_C(1000000000);
    fake.fail_capture = true;
    uint64_t generation = view.generation;
    CHECK(!lardon3d_runtime_observer_refresh(observer, false, true, &view));
    CHECK(view.stale);
    CHECK(view.generation == generation);
    CHECK(strstr(view.status, "injected") != NULL);

    fake.fail_capture = false;
    fake.now_ns += UINT64_C(1000000000);
    fake.sample.resource_observation.snapshot.swap_pages_in = 1;
    fake.sample.resource_observation.snapshot.swap_pages_out = 2;
    CHECK(lardon3d_runtime_observer_refresh(observer, false, true, &view));
    CHECK(!view.resources.swap_delta_known);
    CHECK(!view.stale);

    /* The admitted Task contract knows batch, but cannot fabricate private
     * inflight/helper dimensions when no Governor diagnostic exists. */
    fake.now_ns += UINT64_C(1000000000);
    CHECK(lardon3d_runtime_observer_refresh(observer, true, true, &view));
    CHECK(view.resources.batch_known && view.resources.batch_size == 8);
    CHECK(!view.resources.inflight_known && !view.resources.helpers_known);
    CHECK(view.resources.gpu_backend
        == LARDON3D_TUI_GPU_BACKEND_UNKNOWN);

    /* Aggregate CPU reservations cannot fill in the admitted count for a Task
     * whose exact installed contract is absent. */
    fake.sample.tasks[0].has_execution_contract = false;
    fake.sample.tasks[0].execution_contract =
        (Lardon3DTaskExecutionContract) {0};
    fake.now_ns += UINT64_C(1000000000);
    CHECK(lardon3d_runtime_observer_refresh(observer, true, true, &view));
    CHECK(!view.resources.cpu_admitted_known);
    CHECK(view.resources.cpu_admitted == 0);
    CHECK(!view.resources.batch_known);
    fake.sample.tasks[0].has_execution_contract = true;
    fake.sample.tasks[0].execution_contract =
        (Lardon3DTaskExecutionContract) {
            .batch_size = 8,
            .cpu_threads = 2,
            .gpu_slots = 1,
            .io_slots = 1,
        };

    /* A fully saturated production Queue can expose 64 pending records before
     * the running record in its newest-first order. The 129-entry observation
     * bound and active scan must still select the actual running Task. */
    Lardon3DTaskObservation active = fake.sample.tasks[0];
    fake.sample.task_count = 65;
    fake.sample.task_summary = (Lardon3DTaskQueueSummary) {
        .running = 1,
        .pending = 64,
        .total = 65,
    };
    for (size_t index = 0; index < 64; ++index) {
        fake.sample.tasks[index] = (Lardon3DTaskObservation) {
            .id = UINT64_C(1000) + (uint64_t)index,
            .state = TASK_PENDING,
        };
        (void)snprintf(fake.sample.tasks[index].name,
            sizeof(fake.sample.tasks[index].name), "pending-%zu", index);
    }
    fake.sample.tasks[64] = active;
    fake.now_ns += UINT64_C(1000000000);
    CHECK(lardon3d_runtime_observer_refresh(observer, true, true, &view));
    CHECK(view.task_count == 65);
    CHECK(view.active_task_known && view.active_task_index == 64);
    CHECK(view.tasks[view.active_task_index].id == active.id);

    lardon3d_runtime_observer_destroy(observer);
    CHECK(fake.destroyed);

    /* Production capture must not query a kind/version-wide last diagnostic:
     * such a row may belong to a prior Task with the same kind. */
    production_stub_enabled = true;
    production_stub_task = active;
    last_diagnostic_calls = 0;
    int queue_owner;
    int governor_owner;
    observer = lardon3d_runtime_observer_create(&profile,
        (Lardon3DTaskQueue *)&queue_owner,
        (Lardon3DResourceGovernor *)&governor_owner);
    CHECK(observer);
    CHECK(lardon3d_runtime_observer_refresh(observer, true, true, &view));
    CHECK(last_diagnostic_calls == 0);
    CHECK(view.resources.batch_known && view.resources.batch_size == 8);
    CHECK(!view.resources.inflight_known && !view.resources.helpers_known);
    CHECK(view.resources.gpu_backend == LARDON3D_TUI_GPU_BACKEND_UNKNOWN);
    lardon3d_runtime_observer_destroy(observer);
    production_stub_enabled = false;
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
