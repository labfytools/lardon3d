#ifndef LARDON3D_TASK_QUEUE_H
#define LARDON3D_TASK_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/task.h>

typedef struct Lardon3DTaskQueue Lardon3DTaskQueue;

typedef struct {
    size_t running;
    size_t pending;
    size_t completed;
    size_t total;
} Lardon3DTaskQueueSummary;

Lardon3DTaskQueue *lardon3d_task_queue_create(
    Lardon3DResourceGovernor *governor
);
void lardon3d_task_queue_destroy(Lardon3DTaskQueue *queue);
/* La file devient propriétaire de task uniquement en cas de succès. */
bool lardon3d_task_queue_add(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
);
bool lardon3d_task_queue_remove(Lardon3DTaskQueue *queue, uint64_t task_id);
bool lardon3d_task_queue_cancel(Lardon3DTaskQueue *queue, uint64_t task_id);
void lardon3d_task_queue_resources_changed(Lardon3DTaskQueue *queue);
size_t lardon3d_task_queue_count(Lardon3DTaskQueue *queue);
bool lardon3d_task_queue_get(
    Lardon3DTaskQueue *queue,
    uint64_t task_id,
    Lardon3DTaskSnapshot *snapshot
);
bool lardon3d_task_queue_get_at(
    Lardon3DTaskQueue *queue,
    size_t index,
    Lardon3DTaskSnapshot *snapshot
);
size_t lardon3d_task_queue_snapshot(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskSnapshot *snapshots,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
);

#endif
