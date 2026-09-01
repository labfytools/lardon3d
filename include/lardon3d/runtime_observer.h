#ifndef LARDON3D_RUNTIME_OBSERVER_H
#define LARDON3D_RUNTIME_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_governor.h>
#include <lardon3d/ssd_controller.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/tui_model.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lardon3DRuntimeObserver Lardon3DRuntimeObserver;

typedef struct {
    uint64_t generation;
    uint64_t captured_monotonic_ns;
    bool stale;
    char status[LARDON3D_TUI_TEXT_CAPACITY];

    Lardon3DTaskObservation tasks[LARDON3D_TUI_TASK_CAPACITY];
    size_t task_count;
    Lardon3DTaskQueueSummary task_summary;
    bool active_task_known;
    size_t active_task_index;
    Lardon3DTuiProgressView active_progress;
    Lardon3DTuiStageView stages[LARDON3D_TUI_STAGE_COUNT];

    Lardon3DTuiResourceView resources;
    bool ssd_controller_available;
    Lardon3DSsdSnapshot ssd;
} Lardon3DRuntimeSnapshot;

/* Creates a passive observer over borrowed runtime owners. Queue and Governor
 * must outlive it; Hardware Profile is required only for this call and is
 * copied by value. Observation owns no Task/reservation and never changes
 * admission. Governor-registered SSD usage is copied here under the Governor
 * mutex; physical D-Bus identity/action observation still belongs to the
 * TUI's sole bounded SSD worker and is merged only into the separate physical
 * view. */
Lardon3DRuntimeObserver *lardon3d_runtime_observer_create(
    const Lardon3DHardwareProfile *profile,
    Lardon3DTaskQueue *queue,
    Lardon3DResourceGovernor *governor
);

void lardon3d_runtime_observer_destroy(Lardon3DRuntimeObserver *observer);

/* Copies a caller-owned coherent view. Ordinary calls are coalesced for at
 * least one monotonic second, bounding Queue/Governor and /proc work. The SSD
 * worker independently applies the same minimum telemetry cadence. `force` is
 * reserved for explicit user refresh/testing, never per-frame use.
 * A failed refresh preserves the last bounded view, marks it stale, and
 * returns false; no internal pointer escapes. Thread-safe only through the
 * borrowed owners—call this object from its single TUI/main owner. */
bool lardon3d_runtime_observer_refresh(
    Lardon3DRuntimeObserver *observer,
    bool project_loaded,
    bool force,
    Lardon3DRuntimeSnapshot *snapshot
);

#ifdef __cplusplus
}
#endif

#endif
