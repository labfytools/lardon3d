#ifndef LARDON3D_PROJECT_DB_H
#define LARDON3D_PROJECT_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <lardon3d/task.h>

enum {
    LARDON3D_PROJECT_DB_SCHEMA_VERSION = 4,
    LARDON3D_PROJECT_DB_ID_CAPACITY = 65,
    LARDON3D_PROJECT_DB_KIND_CAPACITY = 65,
    LARDON3D_PROJECT_DB_PATH_CAPACITY = 4096,
    LARDON3D_PROJECT_DB_ERROR_CAPACITY = 256,
    LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX = 256,
    LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX = 256,
    LARDON3D_PROJECT_DB_SCANSET_NAME_CAPACITY = 256,
    LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY = 256,
    LARDON3D_PROJECT_DB_SHA256_SIZE = 32,
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
    bool has_task_kind;
    char task_kind[LARDON3D_TASK_KIND_CAPACITY];
    uint32_t task_kind_version;
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

typedef struct {
    uint64_t task_id;
    char source_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    uint64_t scanset_id;
} Lardon3DProjectDbImageImport;

typedef struct {
    uint64_t scanset_id;
    char name[LARDON3D_PROJECT_DB_SCANSET_NAME_CAPACITY];
    int64_t created_at;
    int64_t updated_at;
} Lardon3DProjectDbScanSet;

typedef enum {
    LARDON3D_DB_IMAGE_ASSET_READY = 1
} Lardon3DProjectDbImageAssetState;

typedef struct {
    uint64_t asset_id;
    unsigned char sha256[LARDON3D_PROJECT_DB_SHA256_SIZE];
    char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    uint64_t size_bytes;
    Lardon3DProjectDbImageAssetState state;
    int64_t created_at;
} Lardon3DProjectDbImageAsset;

typedef struct {
    uint64_t image_id;
    uint64_t scanset_id;
    uint64_t asset_id;
    char original_name[LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY];
    char source_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    bool has_producer_task;
    uint64_t producer_task_id;
    int64_t imported_at;
} Lardon3DProjectDbImage;

typedef enum {
    LARDON3D_PROJECT_DB_IMAGE_REGISTERED = 0,
    LARDON3D_PROJECT_DB_IMAGE_ALREADY_PRESENT
} Lardon3DProjectDbImageRegisterStatus;

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
Lardon3DProjectDbResult lardon3d_project_db_legacy_catalog_pending(
    Lardon3DProjectDb *database, bool *pending
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
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    int64_t updated_at
);
Lardon3DProjectDbResult lardon3d_project_db_record_image_import_task(
    Lardon3DProjectDb *database,
    const Lardon3DTaskDurableSnapshot *snapshot,
    const char *task_kind,
    uint32_t task_kind_version,
    const Lardon3DProjectDbCheckpoint *checkpoint,
    const char *source_path,
    uint64_t scanset_id,
    int64_t updated_at
);
Lardon3DProjectDbResult lardon3d_project_db_create_scanset(
    Lardon3DProjectDb *database, const char *name,
    Lardon3DProjectDbScanSet *scanset
);
Lardon3DProjectDbResult lardon3d_project_db_load_scanset(
    Lardon3DProjectDb *database, uint64_t scanset_id,
    Lardon3DProjectDbScanSet *scanset
);
Lardon3DProjectDbResult lardon3d_project_db_list_scansets(
    Lardon3DProjectDb *database, uint64_t after_scanset_id,
    Lardon3DProjectDbScanSet *scansets, size_t capacity, size_t *count
);
Lardon3DProjectDbResult lardon3d_project_db_register_image(
    Lardon3DProjectDb *database, uint64_t scanset_id,
    const unsigned char sha256[LARDON3D_PROJECT_DB_SHA256_SIZE],
    const char *asset_path, uint64_t size_bytes, const char *original_name,
    const char *source_path, uint64_t producer_task_id, int64_t imported_at,
    Lardon3DProjectDbImageRegisterStatus *status,
    Lardon3DProjectDbImage *image
);
Lardon3DProjectDbResult lardon3d_project_db_load_image(
    Lardon3DProjectDb *database, uint64_t image_id,
    Lardon3DProjectDbImage *image, Lardon3DProjectDbImageAsset *asset
);
Lardon3DProjectDbResult lardon3d_project_db_list_images(
    Lardon3DProjectDb *database, uint64_t scanset_id, uint64_t after_image_id,
    Lardon3DProjectDbImage *images, Lardon3DProjectDbImageAsset *assets,
    size_t capacity, size_t *count
);
Lardon3DProjectDbResult lardon3d_project_db_count_images(
    Lardon3DProjectDb *database, uint64_t scanset_id, uint64_t *count
);
Lardon3DProjectDbResult lardon3d_project_db_load_image_import(
    Lardon3DProjectDb *database,
    uint64_t task_id,
    Lardon3DProjectDbImageImport *parameters
);
Lardon3DProjectDbResult lardon3d_project_db_allocate_task_id(
    Lardon3DProjectDb *database,
    uint64_t *task_id
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
