#ifndef LARDON3D_TASK_CHECKPOINT_H
#define LARDON3D_TASK_CHECKPOINT_H

#include <stdint.h>

#include <lardon3d/task.h>

enum {
    LARDON3D_TASK_CHECKPOINT_VERSION = 1,
};

typedef enum {
    LARDON3D_TASK_CHECKPOINT_OK = 0,
    LARDON3D_TASK_CHECKPOINT_NOT_FOUND,
    LARDON3D_TASK_CHECKPOINT_INVALID,
    LARDON3D_TASK_CHECKPOINT_UNSUPPORTED_VERSION,
    LARDON3D_TASK_CHECKPOINT_IO_ERROR,
    LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE
} Lardon3DTaskCheckpointResult;

/* Writes <path>.next with the same file and parent-directory durability as a
 * canonical checkpoint.  The caller retains ownership of path; this API never
 * replaces its canonical representation. */
Lardon3DTaskCheckpointResult lardon3d_task_checkpoint_stage(
    const char *path,
    const Lardon3DTaskDurableSnapshot *snapshot
);
/* Loads the staged <path>.next representation.  It is optional so NOT_FOUND
 * is a normal result for legacy v22 projects containing only <path>. */
Lardon3DTaskCheckpointResult lardon3d_task_checkpoint_load_staged(
    const char *path,
    Lardon3DTaskDurableSnapshot *snapshot,
    uint32_t *format_version
);
/* Publishes a validated staged representation as canonical without removing
 * .next until canonical file and directory durability have succeeded.  A
 * PUBLISHED_NOT_DURABLE result means the namespace changed but its directory
 * sync failed; recovery must still validate the codec/version and the
 * DB-stored task-summary fields before accepting either representation. */
Lardon3DTaskCheckpointResult lardon3d_task_checkpoint_promote_staged(const char *path);
/* Removes stale staged state only after canonical publication is durable. */
Lardon3DTaskCheckpointResult lardon3d_task_checkpoint_discard_staged(const char *path);

Lardon3DTaskCheckpointResult lardon3d_task_checkpoint_save(
    const char *path,
    const Lardon3DTaskDurableSnapshot *snapshot
);
Lardon3DTaskCheckpointResult lardon3d_task_checkpoint_load(
    const char *path,
    Lardon3DTaskDurableSnapshot *snapshot,
    uint32_t *format_version
);

#endif
