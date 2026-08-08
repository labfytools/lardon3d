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

bool lardon3d_project_create(
    Lardon3DAppState *state,
    const char *name
);

bool lardon3d_project_open(
    Lardon3DAppState *state,
    const char *directory_name
);

void lardon3d_project_close(Lardon3DAppState *state);
Lardon3DProjectTaskCheckpointResult lardon3d_project_checkpoint_task(
    Lardon3DAppState *state,
    const Lardon3DTask *task
);
Lardon3DProjectTaskCheckpointResult
lardon3d_project_checkpoint_image_import_task(
    Lardon3DAppState *state,
    const Lardon3DTask *task,
    const char *source_path
);
Lardon3DProjectDbResult lardon3d_project_list_recoverable(
    Lardon3DAppState *state,
    const Lardon3DTaskKindRegistry *registry,
    uint64_t after_task_id,
    Lardon3DProjectRecoveryEntry *entries,
    size_t capacity,
    size_t *count
);

#endif
