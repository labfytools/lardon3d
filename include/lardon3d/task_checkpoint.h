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
