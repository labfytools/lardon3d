#ifndef LARDON3D_IMPORT_TASK_H
#define LARDON3D_IMPORT_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_IMAGE_IMPORT_TASK_KIND "import.images"
enum { LARDON3D_IMAGE_IMPORT_TASK_KIND_VERSION = 1 };

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

typedef Lardon3DTaskReconstructionContext Lardon3DImageImportReconstructionContext;

Lardon3DTask *lardon3d_project_create_image_import_task(Lardon3DAppState *state,
                                                        uint64_t scanset_id,
                                                        const char *source_directory,
                                                        uint64_t *task_id);
bool lardon3d_project_enqueue_image_import(Lardon3DAppState *state, uint64_t scanset_id,
                                           const char *source_directory, uint64_t *task_id);
bool lardon3d_image_import_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot, void *context,
                                       Lardon3DTaskKindBinding *binding);

/* TUI compatibility: lightweight handle over a Queue Task, with no private thread. */
Lardon3DImportTask *lardon3d_import_task_create(void);
bool lardon3d_import_task_start(Lardon3DImportTask *task, Lardon3DAppState *state,
                                const char *source_directory);
void lardon3d_import_task_request_cancel(Lardon3DImportTask *task);
bool lardon3d_import_task_snapshot(Lardon3DImportTask *task, Lardon3DImportTaskSnapshot *snapshot);
bool lardon3d_import_task_is_finished(Lardon3DImportTask *task);
bool lardon3d_import_task_join(Lardon3DImportTask *task);
void lardon3d_import_task_destroy(Lardon3DImportTask *task);

#endif
