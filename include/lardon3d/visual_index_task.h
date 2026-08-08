#ifndef LARDON3D_VISUAL_INDEX_TASK_H
#define LARDON3D_VISUAL_INDEX_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND "visual_index.update"
enum { LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND_VERSION = 1 };

Lardon3DTask *lardon3d_project_create_visual_index_update_task(
    Lardon3DAppState *state, uint64_t visual_index_id, uint64_t *task_id);
bool lardon3d_project_enqueue_visual_index_update(Lardon3DAppState *state,
                                                  uint64_t visual_index_id,
                                                  uint64_t *task_id);
bool lardon3d_visual_index_update_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
                                              void *context,
                                              Lardon3DTaskKindBinding *binding);

#endif
