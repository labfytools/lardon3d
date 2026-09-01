#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/runtime_observer.h>

#include "runtime_observer_internal.h"

enum {
    RUNTIME_OBSERVER_MINIMUM_INTERVAL_NS = 1000000000ULL,
};

_Static_assert(
    LARDON3D_TUI_TASK_CAPACITY
        >= LARDON3D_TASK_QUEUE_PRODUCTION_CAPACITY + 1
            + LARDON3D_TASK_QUEUE_HISTORY_CAPACITY,
    "runtime observation must cover pending + active + terminal history"
);

typedef struct {
    Lardon3DHardwareProfile profile;
    Lardon3DTaskQueue *queue;
    Lardon3DResourceGovernor *governor;
} ProductionProvider;

struct Lardon3DRuntimeObserver {
    Lardon3DHardwareProfile profile;
    Lardon3DRuntimeObserverProvider provider;
    uint64_t minimum_refresh_interval_ns;
    uint64_t last_attempt_ns;
    bool last_attempt_known;
    bool cached_project_loaded;
    bool cached_project_known;
    bool cache_known;
    bool swap_baseline_known;
    uint64_t swap_pages_in;
    uint64_t swap_pages_out;
    Lardon3DTuiProgressTracker progress_tracker;
    Lardon3DRuntimeSnapshot cache;
};

static void
copy_text(char *destination, size_t capacity, const char *source)
{
    if (destination && capacity > 0) {
        (void)snprintf(destination, capacity, "%s", source ? source : "");
    }
}

static bool
production_now(void *context, uint64_t *now_ns)
{
    (void)context;
    struct timespec now;
    if (!now_ns || clock_gettime(CLOCK_MONOTONIC, &now) != 0
        || now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L
        || (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000)) {
        return false;
    }
    uint64_t seconds = (uint64_t)now.tv_sec * UINT64_C(1000000000);
    if (seconds > UINT64_MAX - (uint64_t)now.tv_nsec) {
        return false;
    }
    *now_ns = seconds + (uint64_t)now.tv_nsec;
    return true;
}

static size_t
active_task_index(
    const Lardon3DTaskObservation *tasks,
    size_t count,
    bool *known
)
{
    static const Lardon3DTaskState priority[] = {
        TASK_RUNNING,
        TASK_PAUSED,
        TASK_PENDING,
    };
    *known = false;
    for (size_t state_index = 0;
         state_index < sizeof(priority) / sizeof(priority[0]);
         ++state_index) {
        for (size_t index = 0; index < count; ++index) {
            if (tasks[index].state == priority[state_index]) {
                *known = true;
                return index;
            }
        }
    }
    return 0;
}

static bool
production_capture(
    void *context,
    Lardon3DRuntimeObserverSample *sample,
    char reason[LARDON3D_TUI_TEXT_CAPACITY]
)
{
    if (!context || !sample || !reason) {
        return false;
    }
    ProductionProvider *provider = context;
    *sample = (Lardon3DRuntimeObserverSample) {0};
    reason[0] = '\0';
    if (clock_gettime(CLOCK_REALTIME, &sample->realtime_now) != 0) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "Cannot read realtime clock for elapsed Task time");
        return false;
    }

    sample->task_count = lardon3d_task_queue_observe(
        provider->queue, sample->tasks, LARDON3D_TUI_TASK_CAPACITY,
        &sample->task_summary);
    if (sample->task_count > LARDON3D_TUI_TASK_CAPACITY) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "Queue returned an invalid bounded snapshot count");
        return false;
    }

    char resource_reason[LARDON3D_TUI_TEXT_CAPACITY] = "";
    sample->resource_valid = lardon3d_resource_observation_capture(
            &provider->profile, &sample->resource_observation,
            resource_reason, sizeof(resource_reason))
        && lardon3d_resource_governor_availability(provider->governor,
            &sample->resource_observation.snapshot, &sample->availability)
        && lardon3d_resource_governor_get_policy(provider->governor,
            &sample->policy)
        && lardon3d_resource_governor_internal_cpu_policy(
            provider->governor, &sample->cpu_policy);
    sample->pressure = lardon3d_resource_governor_pressure(
        provider->governor);
    sample->external_storage_registered =
        lardon3d_resource_governor_get_external_storage(
            provider->governor, &sample->external_storage);

    if (!sample->resource_valid) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            resource_reason[0] ? resource_reason
                               : "Resource observation unavailable");
    }
    return true;
}

static void
production_destroy(void *context)
{
    free(context);
}

static const Lardon3DRuntimeObserverProviderOps production_ops = {
    .monotonic_now_ns = production_now,
    .capture = production_capture,
    .destroy = production_destroy,
};

static void
governor_reason(
    const Lardon3DRuntimeObserverSample *sample,
    const Lardon3DTuiResourceView *resources,
    char reason[LARDON3D_TUI_TEXT_CAPACITY]
)
{
    if (resources->swap_delta_known
        && (resources->swap_pages_in_delta > 0
            || resources->swap_pages_out_delta > 0)) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "Active swap delta observed");
    } else if (resources->ram_available_bytes
            <= resources->ram_reserve_bytes) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "MemAvailable is at or below the hard host reserve");
    } else if (sample->resource_observation.snapshot.memory_pressure_known
        && sample->resource_observation.snapshot.memory_pressure_avg10 > 0.0) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "Memory PSI is active");
    } else if (sample->pressure == LARDON3D_RESOURCE_PRESSURE_GREEN) {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "No active pressure signal observed");
    } else {
        copy_text(reason, LARDON3D_TUI_TEXT_CAPACITY,
            "Governor recovery or conservative slow-start");
    }
}

static void
build_resources(
    Lardon3DRuntimeObserver *observer,
    const Lardon3DRuntimeObserverSample *sample,
    uint64_t now_ns,
    const Lardon3DTaskObservation *active,
    Lardon3DTuiResourceView *resources
)
{
    *resources = (Lardon3DTuiResourceView) {0};
    if (sample->external_storage_registered) {
        const Lardon3DResourceExternalStorage *external =
            &sample->external_storage;
        resources->external_storage_registered = true;
        resources->external_storage_status = external->status;
        resources->scratch_new_allocations_allowed =
            external->new_scratch_allocations_allowed;
        resources->scratch_known = true;
        resources->scratch_total_known = external->scratch_total_known;
        resources->scratch_free_known = external->scratch_free_known;
        resources->scratch_total_bytes = external->scratch_total_bytes;
        resources->scratch_free_bytes = external->scratch_free_bytes;
        resources->scratch_leases = external->active_scratch_leases;
        resources->external_swap_total_known = external->swap_total_known;
        resources->external_swap_used_known = external->swap_used_known;
        resources->external_swap_total_bytes = external->swap_total_bytes;
        resources->external_swap_used_bytes = external->swap_used_bytes;
        copy_text(resources->external_storage_identity,
            sizeof(resources->external_storage_identity),
            external->stable_identity);
        copy_text(resources->external_storage_reason,
            sizeof(resources->external_storage_reason), external->reason);
    }
    if (!sample->resource_valid) {
        copy_text(resources->governor_reason,
            sizeof(resources->governor_reason),
            "Resource snapshot unavailable");
        return;
    }
    resources->valid = true;
    resources->captured_monotonic_ns = now_ns;
    resources->governor_pressure = sample->pressure;
    resources->cpu_logical_total = observer->profile.logical_cpu_count;
    resources->cpu_active = sample->availability.cpu_reserved;
    resources->cpu_available = sample->availability.cpu_available;
    resources->cpu_admitted_known = active && active->has_execution_contract;
    resources->cpu_admitted = resources->cpu_admitted_known
        ? active->execution_contract.cpu_threads : 0;
    copy_text(resources->cpu_reason, sizeof(resources->cpu_reason),
        sample->cpu_policy.reason[0] ? sample->cpu_policy.reason
                                     : "CPU topology policy unavailable");

    resources->gpu_present = observer->profile.gpu_available;
    resources->gpu_uses_shared_memory =
        observer->profile.gpu_uses_shared_memory;
    resources->gpu_memory_known = sample->availability.gpu_memory_known;
    resources->gpu_memory_reserved_bytes =
        sample->availability.gpu_memory_reserved_bytes;
    resources->gpu_memory_available_bytes =
        sample->availability.gpu_memory_available_bytes;
    resources->gpu_slots_active = sample->availability.gpu_slots_reserved;
    resources->gpu_slots_available = sample->availability.gpu_slots_available;

    resources->ram_total_bytes = observer->profile.memory_total_bytes;
    resources->ram_available_bytes =
        sample->resource_observation.snapshot.memory_available_bytes;
    resources->ram_reserve_bytes =
        sample->policy.system_memory_reserve_bytes;
    resources->ram_reserved_bytes = sample->availability.memory_reserved_bytes;
    resources->swap_total_known = sample->resource_observation.swap_total_known;
    resources->swap_total_bytes = sample->resource_observation.swap_total_bytes;
    if (sample->resource_observation.swap_total_known
        && sample->resource_observation.snapshot.swap_available_bytes
            <= sample->resource_observation.swap_total_bytes) {
        resources->swap_used_known = true;
        resources->swap_used_bytes = sample->resource_observation.swap_total_bytes
            - sample->resource_observation.snapshot.swap_available_bytes;
    }
    if (sample->resource_observation.snapshot.swap_activity_known) {
        if (observer->swap_baseline_known
            && sample->resource_observation.snapshot.swap_pages_in
                >= observer->swap_pages_in
            && sample->resource_observation.snapshot.swap_pages_out
                >= observer->swap_pages_out) {
            resources->swap_delta_known = true;
            resources->swap_pages_in_delta =
                sample->resource_observation.snapshot.swap_pages_in
                - observer->swap_pages_in;
            resources->swap_pages_out_delta =
                sample->resource_observation.snapshot.swap_pages_out
                - observer->swap_pages_out;
        }
        observer->swap_baseline_known = true;
        observer->swap_pages_in =
            sample->resource_observation.snapshot.swap_pages_in;
        observer->swap_pages_out =
            sample->resource_observation.snapshot.swap_pages_out;
    } else {
        observer->swap_baseline_known = false;
    }

    resources->io_active = sample->availability.io_slots_reserved;
    resources->io_available = sample->availability.io_slots_available;
    if (active && active->has_execution_contract) {
        resources->batch_known = true;
        resources->batch_size = active->execution_contract.batch_size;
    }
    /* A Governor's last diagnostic is keyed only by kind/version and can
     * belong to an earlier Task or sequence. It must never override this
     * Task's installed contract. Until an exact Task+sequence observation
     * exists, private inflight/helper/backend and utilization values remain
     * explicitly unknown. */
    resources->gpu_backend = LARDON3D_TUI_GPU_BACKEND_UNKNOWN;
    copy_text(resources->gpu_backend_reason,
        sizeof(resources->gpu_backend_reason),
        "Exact active Task backend association UNKNOWN");

    governor_reason(sample, resources, resources->governor_reason);
}

static bool
realtime_elapsed(
    const struct timespec *now,
    const struct timespec *started,
    uint64_t *seconds
)
{
    if (!now || !started || !seconds || now->tv_sec < 0 || started->tv_sec <= 0
        || now->tv_nsec < 0 || now->tv_nsec >= 1000000000L
        || started->tv_nsec < 0 || started->tv_nsec >= 1000000000L
        || now->tv_sec < started->tv_sec) {
        return false;
    }
    time_t elapsed = now->tv_sec - started->tv_sec;
    if (now->tv_nsec < started->tv_nsec) {
        if (elapsed == 0) {
            return false;
        }
        --elapsed;
    }
    if (elapsed < 0) {
        return false;
    }
    *seconds = (uint64_t)elapsed;
    return true;
}

static bool
build_snapshot(
    Lardon3DRuntimeObserver *observer,
    bool project_loaded,
    uint64_t now_ns,
    const Lardon3DRuntimeObserverSample *sample,
    const char *capture_reason,
    Lardon3DRuntimeSnapshot *result
)
{
    if (sample->task_count > LARDON3D_TUI_TASK_CAPACITY) {
        return false;
    }
    *result = (Lardon3DRuntimeSnapshot) {0};
    result->generation = observer->cache.generation == UINT64_MAX
        ? UINT64_MAX
        : observer->cache.generation + 1;
    result->captured_monotonic_ns = now_ns;
    result->task_count = project_loaded ? sample->task_count : 0;
    result->task_summary = project_loaded
        ? sample->task_summary : (Lardon3DTaskQueueSummary) {0};
    if (result->task_count > 0) {
        memcpy(result->tasks, sample->tasks,
            result->task_count * sizeof(sample->tasks[0]));
    }
    result->active_task_index = active_task_index(
        result->tasks, result->task_count, &result->active_task_known);
    const Lardon3DTaskObservation *active = result->active_task_known
        ? &result->tasks[result->active_task_index]
        : NULL;

    Lardon3DResourcePressure pressure = sample->resource_valid
        ? sample->pressure
        : LARDON3D_RESOURCE_PRESSURE_YELLOW;
    lardon3d_tui_stage_views_build(project_loaded, result->tasks,
        result->task_count, pressure, result->stages);
    if (active) {
        Lardon3DTuiProgressSample progress = {
            .task_id = active->id,
            .monotonic_ns = now_ns,
            .task_state = active->state,
            .typed_task = active->has_task_kind,
            .progress_percent = active->progress,
            .durable_counts_known = active->durable_progress_known,
            .durable_completed = active->durable_completed,
            .durable_total = active->durable_total,
            .pressure_limited =
                pressure != LARDON3D_RESOURCE_PRESSURE_GREEN,
        };
        if (!lardon3d_tui_progress_update(&observer->progress_tracker,
                &progress, &result->active_progress)) {
            return false;
        }
        result->active_progress.elapsed_known = realtime_elapsed(
            &sample->realtime_now, &active->started_at,
            &result->active_progress.elapsed_seconds);
    } else {
        observer->progress_tracker = (Lardon3DTuiProgressTracker) {0};
    }

    build_resources(observer, sample, now_ns, active, &result->resources);
    result->ssd_controller_available = sample->ssd_controller_available;
    result->ssd = sample->ssd;
    copy_text(result->status, sizeof(result->status),
        capture_reason && capture_reason[0]
            ? capture_reason
            : (sample->resource_valid
                ? "Runtime observation updated"
                : "Runtime updated; resource telemetry unavailable"));
    return true;
}

Lardon3DRuntimeObserver *
lardon3d_runtime_observer_create_with_provider(
    const Lardon3DHardwareProfile *profile,
    Lardon3DRuntimeObserverProvider provider,
    uint64_t minimum_refresh_interval_ns
)
{
    if (!profile || profile->logical_cpu_count == 0
        || profile->memory_total_bytes == 0 || !provider.ops
        || !provider.ops->monotonic_now_ns || !provider.ops->capture
        || minimum_refresh_interval_ns < RUNTIME_OBSERVER_MINIMUM_INTERVAL_NS) {
        return NULL;
    }
    Lardon3DRuntimeObserver *observer = calloc(1, sizeof(*observer));
    if (!observer) {
        return NULL;
    }
    observer->profile = *profile;
    observer->provider = provider;
    observer->minimum_refresh_interval_ns = minimum_refresh_interval_ns;
    return observer;
}

Lardon3DRuntimeObserver *
lardon3d_runtime_observer_create(
    const Lardon3DHardwareProfile *profile,
    Lardon3DTaskQueue *queue,
    Lardon3DResourceGovernor *governor
)
{
    if (!profile || !queue || !governor) {
        return NULL;
    }
    ProductionProvider *context = calloc(1, sizeof(*context));
    if (!context) {
        return NULL;
    }
    *context = (ProductionProvider) {
        .profile = *profile,
        .queue = queue,
        .governor = governor,
    };
    Lardon3DRuntimeObserverProvider provider = {
        .ops = &production_ops,
        .context = context,
    };
    Lardon3DRuntimeObserver *observer =
        lardon3d_runtime_observer_create_with_provider(
            profile, provider, RUNTIME_OBSERVER_MINIMUM_INTERVAL_NS);
    if (!observer) {
        free(context);
    }
    return observer;
}

void
lardon3d_runtime_observer_destroy(Lardon3DRuntimeObserver *observer)
{
    if (!observer) {
        return;
    }
    if (observer->provider.ops && observer->provider.ops->destroy) {
        observer->provider.ops->destroy(observer->provider.context);
    }
    free(observer);
}

bool
lardon3d_runtime_observer_refresh(
    Lardon3DRuntimeObserver *observer,
    bool project_loaded,
    bool force,
    Lardon3DRuntimeSnapshot *snapshot
)
{
    if (snapshot) {
        *snapshot = (Lardon3DRuntimeSnapshot) {0};
    }
    if (!observer || !snapshot) {
        return false;
    }
    uint64_t now_ns;
    if (!observer->provider.ops->monotonic_now_ns(
            observer->provider.context, &now_ns)) {
        if (observer->cache_known) {
            observer->cache.stale = true;
            copy_text(observer->cache.status, sizeof(observer->cache.status),
                "Runtime clock unavailable; showing stale observation");
            *snapshot = observer->cache;
        }
        return false;
    }
    bool project_changed = !observer->cached_project_known
        || observer->cached_project_loaded != project_loaded;
    if (!force && !project_changed && observer->cache_known
        && observer->last_attempt_known && now_ns >= observer->last_attempt_ns
        && now_ns - observer->last_attempt_ns
            < observer->minimum_refresh_interval_ns) {
        *snapshot = observer->cache;
        return true;
    }
    observer->last_attempt_known = true;
    observer->last_attempt_ns = now_ns;

    Lardon3DRuntimeObserverSample sample;
    char reason[LARDON3D_TUI_TEXT_CAPACITY] = "";
    if (!observer->provider.ops->capture(
            observer->provider.context, &sample, reason)) {
        if (observer->cache_known) {
            observer->cache.stale = true;
            copy_text(observer->cache.status, sizeof(observer->cache.status),
                reason[0] ? reason
                          : "Runtime refresh failed; showing stale observation");
            *snapshot = observer->cache;
        }
        return false;
    }
    Lardon3DRuntimeSnapshot next;
    if (!build_snapshot(observer, project_loaded, now_ns, &sample, reason,
            &next)) {
        if (observer->cache_known) {
            observer->cache.stale = true;
            copy_text(observer->cache.status, sizeof(observer->cache.status),
                "Invalid runtime sample; showing stale observation");
            *snapshot = observer->cache;
        }
        return false;
    }
    observer->cache = next;
    observer->cache_known = true;
    observer->cached_project_known = true;
    observer->cached_project_loaded = project_loaded;
    *snapshot = observer->cache;
    return true;
}
