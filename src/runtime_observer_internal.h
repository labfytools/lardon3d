#ifndef LARDON3D_RUNTIME_OBSERVER_INTERNAL_H
#define LARDON3D_RUNTIME_OBSERVER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/runtime_observer.h>

#include "resource_governor_internal.h"

typedef struct {
    struct timespec realtime_now;
    Lardon3DTaskObservation tasks[LARDON3D_TUI_TASK_CAPACITY];
    size_t task_count;
    Lardon3DTaskQueueSummary task_summary;

    bool resource_valid;
    Lardon3DResourceObservation resource_observation;
    Lardon3DResourceAvailability availability;
    Lardon3DResourcePolicy policy;
    Lardon3DResourceCpuPolicyDiagnostic cpu_policy;
    Lardon3DResourcePressure pressure;
    bool external_storage_registered;
    Lardon3DResourceExternalStorage external_storage;
    bool ssd_controller_available;
    bool ssd_snapshot_valid;
    Lardon3DSsdSnapshot ssd;
} Lardon3DRuntimeObserverSample;

typedef struct {
    bool (*monotonic_now_ns)(void *context, uint64_t *now_ns);
    bool (*capture)(
        void *context,
        Lardon3DRuntimeObserverSample *sample,
        char reason[LARDON3D_TUI_TEXT_CAPACITY]
    );
    void (*destroy)(void *context);
} Lardon3DRuntimeObserverProviderOps;

typedef struct {
    const Lardon3DRuntimeObserverProviderOps *ops;
    void *context;
} Lardon3DRuntimeObserverProvider;

/* Private deterministic seam: the observer owns provider.context only after a
 * successful create. Tests inject one complete bounded sample per refresh;
 * production uses the same pure model builder after collecting real owners. */
Lardon3DRuntimeObserver *lardon3d_runtime_observer_create_with_provider(
    const Lardon3DHardwareProfile *profile,
    Lardon3DRuntimeObserverProvider provider,
    uint64_t minimum_refresh_interval_ns
);

#endif
