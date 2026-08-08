#ifndef LARDON3D_SIFT_TASK_H
#define LARDON3D_SIFT_TASK_H

#include <lardon3d/app_state.h>
#include <lardon3d/feature_extractor.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_SIFT_EXTRACT_TASK_KIND "features.extract.sift"
#define LARDON3D_ROOTSIFT_EXTRACT_TASK_KIND "features.extract.rootsift"
enum { LARDON3D_SIFT_EXTRACT_TASK_KIND_VERSION = 1 };

Lardon3DTask *lardon3d_project_create_sift_extract_task(
    Lardon3DAppState *state, uint64_t image_id, const Lardon3DSiftExtractorParameters *parameters,
    uint64_t *task_id);
bool lardon3d_project_enqueue_sift_extract(
    Lardon3DAppState *state, uint64_t image_id, const Lardon3DSiftExtractorParameters *parameters,
    uint64_t *task_id);
bool lardon3d_sift_extract_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot, void *context,
                                       Lardon3DTaskKindBinding *binding);

#endif
