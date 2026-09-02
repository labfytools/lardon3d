#ifndef LARDON3D_TASK_QUEUE_H
#define LARDON3D_TASK_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/task.h>

typedef struct Lardon3DTaskQueue Lardon3DTaskQueue;

enum {
    /* Application Queue ingress is intentionally bounded independently from
     * terminal history. The runtime observer can therefore size one finite
     * copy for every possible pending/active/history record. */
    LARDON3D_TASK_QUEUE_PRODUCTION_CAPACITY = 64,
    /* The TUI consumes at most 64 rows. Retaining the same bounded number of
     * terminal snapshots keeps recent work observable without retaining Task
     * userdata for the Queue lifetime. */
    LARDON3D_TASK_QUEUE_HISTORY_CAPACITY = 64,
};

typedef enum {
    LARDON3D_TASK_QUEUE_ADD_OK = 0,
    LARDON3D_TASK_QUEUE_ADD_FULL,
    LARDON3D_TASK_QUEUE_ADD_STOPPING,
    LARDON3D_TASK_QUEUE_ADD_DUPLICATE_ID,
    LARDON3D_TASK_QUEUE_ADD_ERROR
} Lardon3DTaskQueueAddResult;

typedef struct {
    size_t running;
    size_t pending;
    /* Saturating count of every Task that became terminal since Queue
     * creation, including terminal records later removed or aged out. */
    size_t completed;
    /* Saturating completed + currently non-terminal Tasks. */
    size_t total;
} Lardon3DTaskQueueSummary;

/* Creates a bounded FIFO Queue with adaptive admissible-work selection and one
 * serialized worker. capacity bounds pending Tasks, not terminal history; the
 * Governor retains resource-admission ownership and must outlive the Queue.
 *
 * CONTRACT: after a successful add, the Queue owns Task and its userdata. Once
 * the Task is terminal and its finished callback has returned, the Queue keeps
 * only a snapshot and promptly destroys the real Task outside the Queue lock.
 * The oldest terminal snapshot is evicted above
 * LARDON3D_TASK_QUEUE_HISTORY_CAPACITY.
 *
 * Finished callbacks execute without the Queue mutex. While the Queue owner
 * keeps it alive, callbacks may use read-only get/get_at/count/snapshot APIs.
 * They must not synchronously remove their own still-active record, destroy the
 * same Queue, or invoke another operation whose completion depends on that
 * callback returning; defer such work until after the callback completes.
 */
Lardon3DTaskQueue *lardon3d_task_queue_create(
    Lardon3DResourceGovernor *governor,
    size_t capacity
);
/* Atomically closes call ingress, cancels live work, waits for the worker and
 * every call whose registration preceded closing (including blocked
 * producers), and destroys all remaining Queue-owned objects exactly once.
 *
 * The owner must prevent new API invocations before calling destroy and must
 * invoke destroy only once. This is the standard external lifetime rule for an
 * object addressed through a raw C pointer: the internal gate resolves calls
 * already registered at the close race, but cannot make an invocation that
 * starts after destruction safe. NULL is accepted. Never call destroy
 * synchronously from a Task finished callback running on this Queue. */
void lardon3d_task_queue_destroy(Lardon3DTaskQueue *queue);
/* The Queue takes ownership of task only on success.
 * Blocking: waits for a free slot when the Queue is full. A zero Task ID is
 * assigned from a nonzero monotonic sequence and is never generated twice
 * during this Queue lifetime, including after terminal-history eviction or
 * removal. Once UINT64_MAX has been generated (or consumed by a restored
 * Task), automatic generation remains exhausted for this Queue lifetime;
 * adding a lower preassigned ID cannot re-arm it. */
bool lardon3d_task_queue_add(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
);
/* Non-blocking: returns false if the Queue is full or stopping.
   The Queue takes ownership of task only on success. */
bool lardon3d_task_queue_try_add(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
);
Lardon3DTaskQueueAddResult lardon3d_task_queue_try_add_ex(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
);
/* Removes a retained terminal snapshot. Active/pending IDs are not removable;
 * an evicted/unknown ID returns false. A finished callback must not remove its
 * own record synchronously because retirement awaits that callback. */
bool lardon3d_task_queue_remove(Lardon3DTaskQueue *queue, uint64_t task_id);
/* Requests cancellation of a live Task. A retained terminal ID returns true
 * as the request is already satisfied; an evicted/unknown ID returns false. */
bool lardon3d_task_queue_cancel(Lardon3DTaskQueue *queue, uint64_t task_id);
/* Pause/resume act only on retained live, non-terminal Tasks. Terminal-history
 * and evicted/unknown IDs return false. */
bool lardon3d_task_queue_pause(Lardon3DTaskQueue *queue, uint64_t task_id);
bool lardon3d_task_queue_resume(Lardon3DTaskQueue *queue, uint64_t task_id);
void lardon3d_task_queue_resources_changed(Lardon3DTaskQueue *queue);
/* Number of Tasks still awaiting dispatch; active and history are excluded. */
size_t lardon3d_task_queue_count(Lardon3DTaskQueue *queue);
/* Returns a copy for live or retained terminal work. IDs that aged out of the
 * bounded terminal history return false. */
bool lardon3d_task_queue_get(
    Lardon3DTaskQueue *queue,
    uint64_t task_id,
    Lardon3DTaskSnapshot *snapshot
);
/* Extended counterpart of get(); retained terminal observations preserve
 * exact typed/durable/admission state after the real Task is destroyed. A
 * non-NULL output is zeroed before lookup, including for unknown/aged-out IDs
 * and closing/error returns. */
bool lardon3d_task_queue_get_observation(
    Lardon3DTaskQueue *queue,
    uint64_t task_id,
    Lardon3DTaskObservation *observation
);
/* Uses the same ordering as lardon3d_task_queue_snapshot(). */
bool lardon3d_task_queue_get_at(
    Lardon3DTaskQueue *queue,
    size_t index,
    Lardon3DTaskSnapshot *snapshot
);
/* Copies the most operationally relevant records first: live Tasks in reverse
 * submission order, followed by terminal snapshots in reverse completion
 * order. Thus a bounded caller sees current and newest work instead of an old
 * prefix. The returned count never exceeds capacity; summary, when non-NULL,
 * describes all current live Tasks plus the saturating lifetime terminal
 * count, independently of snapshot eviction or explicit removal. snapshots may
 * be NULL only when capacity is zero. */
size_t lardon3d_task_queue_snapshot(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskSnapshot *snapshots,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
);

/* Additive extended observation. Ordering and lifetime-summary semantics are
 * identical to snapshot(), but each record carries exact typed/durable/
 * admitted Task data. With the production capacity of 64, a capacity of 129
 * covers the maximum 64 pending + one active + 64 recent terminal records, so
 * live work cannot be hidden by history. observations may be NULL only when
 * capacity is zero. */
size_t lardon3d_task_queue_observe(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskObservation *observations,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
);

#endif
