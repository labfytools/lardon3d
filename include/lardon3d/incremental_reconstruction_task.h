#ifndef LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_H
#define LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/task_kind_registry.h>

typedef struct Lardon3DAppState Lardon3DAppState;

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND \
  "incremental_reconstruction.run"

enum { LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND_VERSION = 1 };

typedef struct {
  uint64_t base_reconstruction_id;
  uint64_t extension_track_set_id;
  uint64_t calibration_scope_id;
} Lardon3DIncrementalReconstructionTaskConfiguration;

Lardon3DTask *lardon3d_project_create_incremental_reconstruction_task(
    Lardon3DAppState *state,
    const Lardon3DIncrementalReconstructionTaskConfiguration *configuration,
    uint64_t *task_id);
bool lardon3d_project_enqueue_incremental_reconstruction_task(
    Lardon3DAppState *state,
    const Lardon3DIncrementalReconstructionTaskConfiguration *configuration,
    uint64_t *task_id);
bool lardon3d_incremental_reconstruction_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

#ifdef __cplusplus
}
#endif

#endif
