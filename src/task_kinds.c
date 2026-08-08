#include <lardon3d/feature_task.h>
#include <lardon3d/import_task.h>
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
      }};
  static const Lardon3DTaskKindRegistry registry = {
      .descriptors = descriptors,
      .count = sizeof(descriptors) / sizeof(descriptors[0]),
  };
  return &registry;
}
