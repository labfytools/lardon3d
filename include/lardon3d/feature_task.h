#ifndef LARDON3D_FEATURE_TASK_H
#define LARDON3D_FEATURE_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/feature_extractor.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_FEATURE_EXTRACT_TASK_KIND "features.extract"
enum { LARDON3D_FEATURE_EXTRACT_TASK_KIND_VERSION = 1 };

Lardon3DTask *
lardon3d_project_create_feature_extract_task(Lardon3DAppState *state, uint64_t image_id,
                                             const Lardon3DFeatureExtractorParameters *parameters,
                                             uint64_t *task_id);
bool lardon3d_project_enqueue_feature_extract(Lardon3DAppState *state, uint64_t image_id,
                                              const Lardon3DFeatureExtractorParameters *parameters,
                                              uint64_t *task_id);
bool lardon3d_feature_extract_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
                                          void *context, Lardon3DTaskKindBinding *binding);

#endif
