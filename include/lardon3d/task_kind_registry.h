#ifndef LARDON3D_TASK_KIND_REGISTRY_H
#define LARDON3D_TASK_KIND_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/task.h>

typedef struct Lardon3DProjectDb Lardon3DProjectDb;
typedef struct Lardon3DResourceGovernor Lardon3DResourceGovernor;
typedef struct Lardon3DOrbVulkanBackend Lardon3DOrbVulkanBackend;

enum {
  LARDON3D_TASK_KIND_REGISTRY_MAX = 64,
};

typedef struct {
  Lardon3DTaskCallback callback;
  void *userdata;
  Lardon3DTaskUserdataDestroy userdata_destroy;
  Lardon3DTaskFinishedCallback finished_callback;
  void *finished_userdata;
} Lardon3DTaskKindBinding;

typedef bool (*Lardon3DTaskKindReconstruct)(const Lardon3DTaskDurableSnapshot *snapshot,
                                            void *context, Lardon3DTaskKindBinding *binding);

typedef struct {
  const char *kind;
  uint32_t kind_version;
  Lardon3DTaskKindReconstruct reconstruct;
} Lardon3DTaskKindDescriptor;

typedef struct {
  const Lardon3DTaskKindDescriptor *descriptors;
  size_t count;
} Lardon3DTaskKindRegistry;

typedef struct {
  const char *project_path;
  Lardon3DProjectDb *project_db;
  Lardon3DResourceGovernor *resource_governor;
  Lardon3DOrbVulkanBackend *orb_vulkan_backend;
} Lardon3DTaskReconstructionContext;

typedef enum {
  LARDON3D_TASK_KIND_OK = 0,
  LARDON3D_TASK_KIND_INVALID_ARGUMENT,
  LARDON3D_TASK_KIND_UNKNOWN,
  LARDON3D_TASK_KIND_UNSUPPORTED_VERSION,
  LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED,
  LARDON3D_TASK_KIND_RESTORE_FAILED
} Lardon3DTaskKindResult;

bool lardon3d_task_kind_registry_init(Lardon3DTaskKindRegistry *registry,
                                      const Lardon3DTaskKindDescriptor *descriptors, size_t count);
Lardon3DTaskKindResult
lardon3d_task_kind_registry_lookup(const Lardon3DTaskKindRegistry *registry, const char *kind,
                                   uint32_t kind_version,
                                   const Lardon3DTaskKindDescriptor **descriptor);
Lardon3DTaskKindResult lardon3d_task_kind_registry_restore(
    const Lardon3DTaskKindRegistry *registry, const char *kind, uint32_t kind_version,
    const Lardon3DTaskDurableSnapshot *snapshot, void *context, Lardon3DTask **task);
const Lardon3DTaskKindRegistry *lardon3d_task_kind_registry_production(void);

#endif
