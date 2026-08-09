#ifndef LARDON3D_GEOMETRIC_VERIFIER_TASK_H
#define LARDON3D_GEOMETRIC_VERIFIER_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/geometric_verifier.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND "geometric_verifier.run"

enum {
  LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION = 1,
  LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH = 1,
  LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH = 8,
};

typedef struct {
  Lardon3DGeometricVerifierParameters verifier;
} Lardon3DGeometricVerifierTaskConfiguration;

Lardon3DTask *lardon3d_project_create_geometric_verifier_task(
    Lardon3DAppState *state,
    const Lardon3DGeometricVerifierTaskConfiguration *configuration,
    uint64_t *task_id);
bool lardon3d_project_enqueue_geometric_verifier_task(
    Lardon3DAppState *state,
    const Lardon3DGeometricVerifierTaskConfiguration *configuration,
    uint64_t *task_id);
bool lardon3d_geometric_verifier_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

#endif
