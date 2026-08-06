#ifndef LARDON3D_IMPORT_TASK_H
#define LARDON3D_IMPORT_TASK_H

#include <stdbool.h>
#include <stddef.h>

#include <lardon3d/app_state.h>

typedef enum {
    LARDON3D_IMPORT_TASK_IDLE = 0,
    LARDON3D_IMPORT_TASK_RUNNING,
    LARDON3D_IMPORT_TASK_SUCCEEDED,
    LARDON3D_IMPORT_TASK_CANCELLED,
    LARDON3D_IMPORT_TASK_FAILED
} Lardon3DImportTaskStatus;

typedef struct {
    Lardon3DImportTaskStatus status;
    size_t total;
    size_t processed;
    size_t copied;
    size_t already_present;
    size_t ignored;
    char message[256];
} Lardon3DImportTaskSnapshot;

typedef struct Lardon3DImportTask Lardon3DImportTask;

Lardon3DImportTask *lardon3d_import_task_create(void);
bool lardon3d_import_task_start(
    Lardon3DImportTask *task,
    const Lardon3DAppState *state,
    const char *source_directory
);
void lardon3d_import_task_request_cancel(Lardon3DImportTask *task);
bool lardon3d_import_task_snapshot(
    Lardon3DImportTask *task,
    Lardon3DImportTaskSnapshot *snapshot
);
bool lardon3d_import_task_is_finished(Lardon3DImportTask *task);
bool lardon3d_import_task_join(Lardon3DImportTask *task);
void lardon3d_import_task_destroy(Lardon3DImportTask *task);

#endif
