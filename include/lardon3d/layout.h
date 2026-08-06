#ifndef LARDON3D_LAYOUT_H
#define LARDON3D_LAYOUT_H

#include <lardon3d/app_state.h>
#include <lardon3d/import_task.h>
#include <lardon3d/task_queue.h>

void lardon3d_layout_draw(
    const Lardon3DAppState *state,
    const char *input_text,
    const char *input_label,
    const Lardon3DImportTaskSnapshot *import_snapshot,
    const Lardon3DTaskSnapshot *task_snapshots,
    size_t task_count,
    const Lardon3DTaskQueueSummary *task_summary,
    int rows,
    int cols
);

#endif
