#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lardon3d/task_queue.h>

typedef struct TaskNode {
    Lardon3DTask *task;
    struct TaskNode *next_all;
    struct TaskNode *next_pending;
} TaskNode;

struct Lardon3DTaskQueue {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t worker;
    bool worker_started;
    bool stopping;
    Lardon3DResourceGovernor *governor;
    uint64_t next_id;
    TaskNode *all_head;
    TaskNode *all_tail;
    TaskNode *pending_head;
    TaskNode *pending_tail;
    Lardon3DTask *active;
    size_t count;
};

static bool
terminal_state(Lardon3DTaskState state)
{
    return state == TASK_CANCELLED || state == TASK_FAILED
        || state == TASK_COMPLETED;
}

static void *
queue_worker(void *context)
{
    Lardon3DTaskQueue *queue = context;
    for (;;) {
        (void)pthread_mutex_lock(&queue->mutex);
        while (!queue->stopping && !queue->pending_head) {
            (void)pthread_cond_wait(&queue->condition, &queue->mutex);
        }
        if (queue->stopping) {
            (void)pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        TaskNode *node = queue->pending_head;
        Lardon3DTaskSnapshot task_snapshot;
        if (!lardon3d_task_snapshot(node->task, &task_snapshot)) {
            (void)pthread_mutex_unlock(&queue->mutex);
            continue;
        }
        if (terminal_state(task_snapshot.state)) {
            queue->pending_head = node->next_pending;
            if (!queue->pending_head) {
                queue->pending_tail = NULL;
            }
            node->next_pending = NULL;
            (void)pthread_mutex_unlock(&queue->mutex);
            continue;
        }
        Lardon3DResourceEstimate estimate;
        Lardon3DResourceDecision decision;
        Lardon3DResourceReservation *reservation = NULL;
        bool evaluated = lardon3d_task_resource_estimate(node->task, &estimate)
            && lardon3d_resource_governor_reserve_available(
                queue->governor,
                &estimate,
                &decision,
                &reservation
            );
        if (!evaluated) {
            (void)lardon3d_task_reject(
                node->task,
                "Impossible d'évaluer les ressources disponibles."
            );
            (void)pthread_mutex_unlock(&queue->mutex);
            continue;
        }
        if (decision.kind == LARDON3D_RESOURCE_WAIT) {
            (void)pthread_cond_wait(&queue->condition, &queue->mutex);
            (void)pthread_mutex_unlock(&queue->mutex);
            continue;
        }
        if (decision.kind == LARDON3D_RESOURCE_REJECT || !reservation) {
            (void)lardon3d_task_reject(node->task, decision.reason);
            (void)pthread_mutex_unlock(&queue->mutex);
            continue;
        }
        queue->pending_head = node->next_pending;
        if (!queue->pending_head) {
            queue->pending_tail = NULL;
        }
        node->next_pending = NULL;
        queue->active = node->task;
        (void)pthread_mutex_unlock(&queue->mutex);

        if (!lardon3d_task_start(
                node->task,
                queue->governor,
                reservation
            )) {
            (void)lardon3d_task_reject(
                node->task,
                "Réservation de ressources invalide."
            );
        }
        (void)lardon3d_resource_governor_release(
            queue->governor,
            reservation
        );

        (void)pthread_mutex_lock(&queue->mutex);
        queue->active = NULL;
        (void)pthread_cond_broadcast(&queue->condition);
        (void)pthread_mutex_unlock(&queue->mutex);
    }
}

Lardon3DTaskQueue *
lardon3d_task_queue_create(Lardon3DResourceGovernor *governor)
{
    if (!governor) {
        return NULL;
    }
    Lardon3DTaskQueue *queue = calloc(1, sizeof(*queue));
    if (!queue) {
        return NULL;
    }
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue);
        return NULL;
    }
    if (pthread_cond_init(&queue->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->next_id = 1;
    queue->governor = governor;
    if (pthread_create(&queue->worker, NULL, queue_worker, queue) != 0) {
        (void)pthread_cond_destroy(&queue->condition);
        (void)pthread_mutex_destroy(&queue->mutex);
        free(queue);
        return NULL;
    }
    queue->worker_started = true;
    return queue;
}

bool
lardon3d_task_queue_cancel(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    while (node && lardon3d_task_id(node->task) != task_id) {
        node = node->next_all;
    }
    if (node) {
        lardon3d_task_request_cancel(node->task);
        (void)pthread_cond_broadcast(&queue->condition);
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    return node != NULL;
}

void
lardon3d_task_queue_resources_changed(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    (void)pthread_cond_broadcast(&queue->condition);
    (void)pthread_mutex_unlock(&queue->mutex);
}

void
lardon3d_task_queue_destroy(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    queue->stopping = true;
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        lardon3d_task_request_cancel(node->task);
    }
    (void)pthread_cond_broadcast(&queue->condition);
    (void)pthread_mutex_unlock(&queue->mutex);
    if (queue->worker_started) {
        (void)pthread_join(queue->worker, NULL);
    }
    TaskNode *node = queue->all_head;
    while (node) {
        TaskNode *next = node->next_all;
        lardon3d_task_destroy(node->task);
        free(node);
        node = next;
    }
    (void)pthread_cond_destroy(&queue->condition);
    (void)pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

bool
lardon3d_task_queue_add(
    Lardon3DTaskQueue *queue,
    Lardon3DTask *task,
    uint64_t *task_id
)
{
    if (!queue || !task) {
        return false;
    }
    TaskNode *node = calloc(1, sizeof(*node));
    if (!node) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    if (queue->stopping || queue->next_id == 0
        || !lardon3d_task_assign_id(task, queue->next_id)) {
        (void)pthread_mutex_unlock(&queue->mutex);
        free(node);
        return false;
    }
    uint64_t id = queue->next_id++;
    node->task = task;
    if (queue->all_tail) {
        queue->all_tail->next_all = node;
    } else {
        queue->all_head = node;
    }
    queue->all_tail = node;
    if (queue->pending_tail) {
        queue->pending_tail->next_pending = node;
    } else {
        queue->pending_head = node;
    }
    queue->pending_tail = node;
    ++queue->count;
    if (task_id) {
        *task_id = id;
    }
    (void)pthread_cond_signal(&queue->condition);
    (void)pthread_mutex_unlock(&queue->mutex);
    return true;
}

bool
lardon3d_task_queue_remove(Lardon3DTaskQueue *queue, uint64_t task_id)
{
    if (!queue || task_id == 0) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *previous = NULL;
    TaskNode *node = queue->all_head;
    while (node && lardon3d_task_id(node->task) != task_id) {
        previous = node;
        node = node->next_all;
    }
    Lardon3DTaskSnapshot snapshot;
    if (!node || !lardon3d_task_snapshot(node->task, &snapshot)
        || !terminal_state(snapshot.state)) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    while (node->task == queue->active) {
        (void)pthread_cond_wait(&queue->condition, &queue->mutex);
    }
    if (previous) {
        previous->next_all = node->next_all;
    } else {
        queue->all_head = node->next_all;
    }
    if (queue->all_tail == node) {
        queue->all_tail = previous;
    }
    TaskNode *pending_previous = NULL;
    TaskNode *pending = queue->pending_head;
    while (pending && pending != node) {
        pending_previous = pending;
        pending = pending->next_pending;
    }
    if (pending) {
        if (pending_previous) {
            pending_previous->next_pending = pending->next_pending;
        } else {
            queue->pending_head = pending->next_pending;
        }
        if (queue->pending_tail == pending) {
            queue->pending_tail = pending_previous;
        }
    }
    --queue->count;
    (void)pthread_mutex_unlock(&queue->mutex);
    lardon3d_task_destroy(node->task);
    free(node);
    return true;
}

size_t
lardon3d_task_queue_count(Lardon3DTaskQueue *queue)
{
    if (!queue) {
        return 0;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    size_t count = queue->count;
    (void)pthread_mutex_unlock(&queue->mutex);
    return count;
}

bool
lardon3d_task_queue_get(
    Lardon3DTaskQueue *queue,
    uint64_t task_id,
    Lardon3DTaskSnapshot *snapshot
)
{
    if (!queue || task_id == 0 || !snapshot) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    while (node && lardon3d_task_id(node->task) != task_id) {
        node = node->next_all;
    }
    bool found = node && lardon3d_task_snapshot(node->task, snapshot);
    (void)pthread_mutex_unlock(&queue->mutex);
    return found;
}

bool
lardon3d_task_queue_get_at(
    Lardon3DTaskQueue *queue,
    size_t index,
    Lardon3DTaskSnapshot *snapshot
)
{
    if (!queue || !snapshot) {
        return false;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    TaskNode *node = queue->all_head;
    while (node && index > 0) {
        node = node->next_all;
        --index;
    }
    bool found = node && lardon3d_task_snapshot(node->task, snapshot);
    (void)pthread_mutex_unlock(&queue->mutex);
    return found;
}

size_t
lardon3d_task_queue_snapshot(
    Lardon3DTaskQueue *queue,
    Lardon3DTaskSnapshot *snapshots,
    size_t capacity,
    Lardon3DTaskQueueSummary *summary
)
{
    if (summary) {
        *summary = (Lardon3DTaskQueueSummary) {0};
    }
    if (!queue || (!snapshots && capacity > 0)) {
        return 0;
    }
    (void)pthread_mutex_lock(&queue->mutex);
    size_t copied = 0;
    for (TaskNode *node = queue->all_head; node; node = node->next_all) {
        Lardon3DTaskSnapshot snapshot;
        if (!lardon3d_task_snapshot(node->task, &snapshot)) {
            continue;
        }
        if (summary) {
            ++summary->total;
            if (snapshot.state == TASK_RUNNING || snapshot.state == TASK_PAUSED) {
                ++summary->running;
            } else if (snapshot.state == TASK_PENDING) {
                ++summary->pending;
            } else {
                ++summary->completed;
            }
        }
        if (copied < capacity) {
            snapshots[copied++] = snapshot;
        }
    }
    (void)pthread_mutex_unlock(&queue->mutex);
    return copied;
}
