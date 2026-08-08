#ifndef LARDON3D_PROJECT_DB_H
#define LARDON3D_PROJECT_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/task.h>

enum {
    LARDON3D_PROJECT_DB_SCHEMA_VERSION = 1,
    LARDON3D_PROJECT_DB_ID_CAPACITY = 65,
    LARDON3D_PROJECT_DB_KIND_CAPACITY = 65,
    LARDON3D_PROJECT_DB_PATH_CAPACITY = 4096,
    LARDON3D_PROJECT_DB_ERROR_CAPACITY = 256,
    LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX = 256,
};

typedef struct Lardon3DProjectDb Lardon3DProjectDb;

typedef enum {
    LARDON3D_PROJECT_DB_OK = 0,
    LARDON3D_PROJECT_DB_INVALID_ARGUMENT,
    LARDON3D_PROJECT_DB_NOT_FOUND,
    LARDON3D_PROJECT_DB_BUSY,
    LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA,
    LARDON3D_PROJECT_DB_CORRUPT,
    LARDON3D_PROJECT_DB_CONSTRAINT,
    LARDON3D_PROJECT_DB_IO_ERROR
} Lardon3DProjectDbResult;

typedef enum {
    LARDON3D_DB_CHECKPOINT_DURABLE = 0,
    LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE
} Lardon3DProjectDbCheckpointDurability;

typedef enum {
    LARDON3D_DB_ARTIFACT_STAGED = 0,
    LARDON3D_DB_ARTIFACT_READY
} Lardon3DProjectDbArtifactState;

typedef struct {
    char stable_id[LARDON3D_PROJECT_DB_ID_CAPACITY];
    char name[LARDON3D_TASK_NAME_CAPACITY];
    int64_t created_at;
    int64_t updated_at;
} Lardon3DProjectDbProject;

typedef struct {
    char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    uint32_t format_version;
    Lardon3DProjectDbCheckpointDurability durability;
    int64_t updated_at;
} Lardon3DProjectDbCheckpoint;

typedef struct {
    uint64_t task_id;
    char name[LARDON3D_TASK_NAME_CAPACITY];
    Lardon3DTaskState saved_state;
    Lardon3DTaskState recovery_state;
    unsigned int progress;
    unsigned int sequence_count;
    struct timespec started_at;
    struct timespec finished_at;
    int64_t updated_at;
    bool has_checkpoint;
    Lardon3DProjectDbCheckpoint checkpoint;
} Lardon3DProjectDbTask;

typedef struct {
    char artifact_id[LARDON3D_PROJECT_DB_ID_CAPACITY];
    char kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
    char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    Lardon3DProjectDbArtifactState state;
    uint64_t size_bytes;
    bool has_producer_task;
    uint64_t producer_task_id;
    int64_t created_at;
    int64_t updated_at;
} Lardon3DProjectDbArtifact;

Lardon3DProjectDbResult lardon3d_project_db_open(
    const char *path,
    Lardon3DProjectDb **database,
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]
);
void lardon3d_project_db_close(Lardon3DProjectDb *database);
bool lardon3d_project_db_last_error(
    Lardon3DProjectDb *database,
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]
);
unsigned int lardon3d_project_db_schema_version(Lardon3DProjectDb *database);

Lardon3DProjectDbResult lardon3d_project_db_set_project(
    Lardon3DProjectDb *database,
    const Lardon3DProjectDbProject *project
);
Lardon3DProjectDbResult lardon3d_project_db_get_project(
    Lardon3DProjectDb *database,
    Lardon3DProjectDbProject *project
);
Lardon3DProjectDbResult lardon3d_project_db_record_task(
    Lardon3DProjectDb *database,
    const Lardon3DTaskDurableSnapshot *snapshot,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    int64_t updated_at
);
Lardon3DProjectDbResult lardon3d_project_db_load_task(
    Lardon3DProjectDb *database,
    uint64_t task_id,
    Lardon3DProjectDbTask *task
);
Lardon3DProjectDbResult lardon3d_project_db_list_recoverable(
    Lardon3DProjectDb *database,
    uint64_t after_task_id,
    Lardon3DProjectDbTask *tasks,
    size_t capacity,
    size_t *count
);
Lardon3DProjectDbResult lardon3d_project_db_create_artifact(
    Lardon3DProjectDb *database,
    const Lardon3DProjectDbArtifact *artifact
);
Lardon3DProjectDbResult lardon3d_project_db_mark_artifact_ready(
    Lardon3DProjectDb *database,
    const char *artifact_id,
    int64_t updated_at
);
Lardon3DProjectDbResult lardon3d_project_db_load_artifact(
    Lardon3DProjectDb *database,
    const char *artifact_id,
    Lardon3DProjectDbArtifact *artifact
);

#endif
