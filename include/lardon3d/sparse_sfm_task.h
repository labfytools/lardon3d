#ifndef LARDON3D_SPARSE_SFM_TASK_H
#define LARDON3D_SPARSE_SFM_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/sparse_sfm_incremental.h>
#include <lardon3d/task_kind_registry.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_SPARSE_SFM_TASK_KIND "sparse_sfm.run"

enum { LARDON3D_SPARSE_SFM_TASK_KIND_VERSION = 1 };

typedef struct {
  uint64_t track_set_id;
  uint64_t calibration_scope_id;
  Lardon3DSparseIncrementalParameters parameters;
} Lardon3DSparseSfmTaskConfiguration;

Lardon3DTask *lardon3d_project_create_sparse_sfm_task(
    Lardon3DAppState *state, const Lardon3DSparseSfmTaskConfiguration *configuration,
    uint64_t *task_id);
bool lardon3d_project_enqueue_sparse_sfm_task(
    Lardon3DAppState *state, const Lardon3DSparseSfmTaskConfiguration *configuration,
    uint64_t *task_id);
bool lardon3d_sparse_sfm_task_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
                                          void *context, Lardon3DTaskKindBinding *binding);

#ifdef __cplusplus
}
#endif

#endif
