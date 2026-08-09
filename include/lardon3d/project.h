#ifndef LARDON3D_PROJECT_H
#define LARDON3D_PROJECT_H

#include <stdbool.h>

#include <lardon3d/app_state.h>
#include <lardon3d/project_db.h>
#include <lardon3d/task_kind_registry.h>

typedef enum {
  LARDON3D_PROJECT_TASK_CHECKPOINT_OK = 0,
  LARDON3D_PROJECT_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE,
  LARDON3D_PROJECT_TASK_CHECKPOINT_NO_PROJECT,
  LARDON3D_PROJECT_TASK_CHECKPOINT_INVALID_TASK,
  LARDON3D_PROJECT_TASK_CHECKPOINT_IO_ERROR,
  LARDON3D_PROJECT_TASK_CHECKPOINT_DB_BUSY,
  LARDON3D_PROJECT_TASK_CHECKPOINT_DB_ERROR
} Lardon3DProjectTaskCheckpointResult;

typedef enum {
  LARDON3D_PROJECT_RECOVERABLE = 0,
  LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE,
  LARDON3D_PROJECT_RECOVERY_MISSING_CHECKPOINT,
  LARDON3D_PROJECT_RECOVERY_INVALID_CHECKPOINT,
  LARDON3D_PROJECT_RECOVERY_UNSUPPORTED_CHECKPOINT,
  LARDON3D_PROJECT_RECOVERY_CHECKPOINT_IO_ERROR,
  LARDON3D_PROJECT_RECOVERY_LEGACY_UNTYPED,
  LARDON3D_PROJECT_RECOVERY_UNKNOWN_TASK_KIND,
  LARDON3D_PROJECT_RECOVERY_UNSUPPORTED_TASK_KIND_VERSION
} Lardon3DProjectRecoveryStatus;

typedef struct {
  uint64_t task_id;
  char name[LARDON3D_TASK_NAME_CAPACITY];
  char task_kind[LARDON3D_TASK_KIND_CAPACITY];
  uint32_t task_kind_version;
  Lardon3DProjectRecoveryStatus status;
  Lardon3DProjectDbCheckpointDurability durability;
  Lardon3DTaskDurableSnapshot snapshot;
} Lardon3DProjectRecoveryEntry;

typedef struct {
  size_t inspected;
  size_t resumed;
  size_t skipped;
  size_t failed;
  size_t published_not_durable;
  bool queue_full;
} Lardon3DProjectRecoverySummary;

bool lardon3d_project_create(Lardon3DAppState *state, const char *name);

bool lardon3d_project_open(Lardon3DAppState *state, const char *directory_name);

void lardon3d_project_close(Lardon3DAppState *state);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_task(Lardon3DAppState *state,
                                                                     const Lardon3DTask *task);
Lardon3DProjectTaskCheckpointResult
lardon3d_project_checkpoint_image_import_task(Lardon3DAppState *state, const Lardon3DTask *task,
                                              const char *source_path, uint64_t scanset_id);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_feature_extract_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbFeatureExtractTask *parameters);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_sift_extract_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbSiftExtractTask *parameters);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_visual_index_update_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbVisualIndexUpdateTask *parameters);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_candidate_pair_generate_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbCandidatePairGenerateTask *parameters);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_matcher_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbMatcherTask *parameters);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_geometric_verifier_task(
    Lardon3DAppState *state, const Lardon3DTask *task,
    const Lardon3DProjectDbGeometricVerifierTask *parameters);
Lardon3DProjectDbResult lardon3d_project_list_recoverable(Lardon3DAppState *state,
                                                          const Lardon3DTaskKindRegistry *registry,
                                                          uint64_t after_task_id,
                                                          Lardon3DProjectRecoveryEntry *entries,
                                                          size_t capacity, size_t *count);
Lardon3DProjectDbResult
lardon3d_project_resume_recoverable_tasks(Lardon3DAppState *state,
                                          const Lardon3DTaskKindRegistry *registry,
                                          Lardon3DProjectRecoverySummary *summary);
bool lardon3d_project_last_recovery_summary(const Lardon3DAppState *state,
                                            Lardon3DProjectRecoverySummary *summary);

#endif
