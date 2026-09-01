#ifndef LARDON3D_LAYOUT_H
#define LARDON3D_LAYOUT_H

#include <lardon3d/app_state.h>
#include <lardon3d/import_task.h>
#include <lardon3d/runtime_observer.h>
#include <lardon3d/tui_model.h>
#include <lardon3d/tui_optics.h>
#include <lardon3d/tui_ssd_async.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frozen legacy renderer ABI. It accepts only the historical bounded Task and
 * resource views; the implementation adapts them without reading beyond any
 * caller object compiled against the original declarations. Main thread only.
 */
void lardon3d_layout_draw(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    const Lardon3DTaskSnapshot *task_snapshots,
    size_t task_count,
    const Lardon3DTaskQueueSummary *task_summary,
    const Lardon3DResourceAvailability *resource_availability,
    int rows,
    int cols
);

/* Main-thread-only extended renderer. `state` and `runtime` are required;
 * import/SSD/optics/palette views are nullable. Every supplied view is a
 * borrowed, bounded caller-owned copy for this call; rendering performs no
 * Queue, Governor, Project DB, controller, or scientific mutation.
 * interaction_mode is the actual input owner and is the sole source of footer
 * capabilities. */
void lardon3d_layout_draw_runtime(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    const Lardon3DRuntimeSnapshot *runtime,
    const Lardon3DTuiSsdAsyncSnapshot *ssd_operation,
    const Lardon3DTuiOpticsSnapshot *optics,
    size_t selected_task,
    const Lardon3DTuiPalette *palette,
    Lardon3DTuiInteractionMode interaction_mode,
    int rows,
    int cols
);

#ifdef __cplusplus
}
#endif

#endif
