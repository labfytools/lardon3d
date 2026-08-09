#include <lardon3d/candidate_pair_task.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/geometric_verifier_task.h>
#include <lardon3d/import_task.h>
#include <lardon3d/matcher_task.h>
#include <lardon3d/sift_task.h>
#include <lardon3d/visual_index_task.h>
#include <lardon3d/task_kind_registry.h>

const Lardon3DTaskKindRegistry *lardon3d_task_kind_registry_production(void) {
  static const Lardon3DTaskKindDescriptor descriptors[] = {
      {
          .kind = LARDON3D_IMAGE_IMPORT_TASK_KIND,
          .kind_version = LARDON3D_IMAGE_IMPORT_TASK_KIND_VERSION,
          .reconstruct = lardon3d_image_import_reconstruct,
      },
      {
          .kind = LARDON3D_FEATURE_EXTRACT_TASK_KIND,
          .kind_version = LARDON3D_FEATURE_EXTRACT_TASK_KIND_VERSION,
          .reconstruct = lardon3d_feature_extract_reconstruct,
      },
      {
          .kind = LARDON3D_SIFT_EXTRACT_TASK_KIND,
          .kind_version = LARDON3D_SIFT_EXTRACT_TASK_KIND_VERSION,
          .reconstruct = lardon3d_sift_extract_reconstruct,
      },
      {
          .kind = LARDON3D_ROOTSIFT_EXTRACT_TASK_KIND,
          .kind_version = LARDON3D_SIFT_EXTRACT_TASK_KIND_VERSION,
          .reconstruct = lardon3d_sift_extract_reconstruct,
      },
      {
          .kind = LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND,
          .kind_version = LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND_VERSION,
          .reconstruct = lardon3d_visual_index_update_reconstruct,
      },
      {
          .kind = LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND,
          .kind_version = LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND_VERSION,
          .reconstruct = lardon3d_candidate_pair_generate_reconstruct,
      },
      {
          .kind = LARDON3D_MATCHER_TASK_KIND,
          .kind_version = LARDON3D_MATCHER_TASK_KIND_VERSION,
          .reconstruct = lardon3d_matcher_task_reconstruct,
      },
      {
          .kind = LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND,
          .kind_version = LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION,
          .reconstruct = lardon3d_geometric_verifier_task_reconstruct,
      }};
  static const Lardon3DTaskKindRegistry registry = {
      .descriptors = descriptors,
      .count = sizeof(descriptors) / sizeof(descriptors[0]),
  };
  return &registry;
}
