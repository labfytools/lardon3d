#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <lardon3d/task_queue.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

enum {
    TASK_COUNT = 400,
};

typedef struct {
    pthread_mutex_t mutex;
    size_t order[TASK_COUNT];
    size_t count;
} OrderLog;

typedef struct {
    OrderLog *log;
    size_t value;
    size_t steps;
} QueueWork;

static void
short_pause(void)
{
    const struct timespec duration = {.tv_sec = 0, .tv_nsec = 1000000};
    (void)nanosleep(&duration, NULL);
}

static bool
queue_callback(Lardon3DTask *task, void *userdata)
{
    QueueWork *work = userdata;
    (void)pthread_mutex_lock(&work->log->mutex);
    work->log->order[work->log->count++] = work->value;
    (void)pthread_mutex_unlock(&work->log->mutex);
    for (size_t step = 0; step < work->steps; ++step) {
        if (!lardon3d_task_checkpoint(task)) {
            return false;
        }
        unsigned int progress = (unsigned int)(((step + 1) * 100) / work->steps);
        (void)lardon3d_task_set_progress(task, progress, "File en cours.");
        if (work->steps > 1) {
            short_pause();
        }
    }
    return true;
}

static bool
wait_terminal(Lardon3DTaskQueue *queue, uint64_t id, Lardon3DTaskSnapshot *result)
{
    for (size_t attempt = 0; attempt < 10000; ++attempt) {
        if (!lardon3d_task_queue_get(queue, id, result)) {
            return false;
        }
        if (result->state == TASK_CANCELLED || result->state == TASK_FAILED
            || result->state == TASK_COMPLETED) {
            return true;
        }
        short_pause();
    }
    return false;
}

static bool
run_test(void)
{
    lardon3d_task_queue_destroy(NULL);
    CHECK(lardon3d_task_queue_count(NULL) == 0);
    Lardon3DTaskQueue *queue = lardon3d_task_queue_create();
    CHECK(queue);
    short_pause();

    OrderLog log = {0};
    CHECK(pthread_mutex_init(&log.mutex, NULL) == 0);
    QueueWork work[TASK_COUNT];
    Lardon3DTask *tasks[TASK_COUNT];
    uint64_t ids[TASK_COUNT];
    for (size_t index = 0; index < TASK_COUNT; ++index) {
        work[index] = (QueueWork) {
            .log = &log,
            .value = index,
            .steps = 1,
        };
        tasks[index] = lardon3d_task_create("FIFO", queue_callback, &work[index]);
        CHECK(tasks[index]);
        CHECK(lardon3d_task_queue_add(queue, tasks[index], &ids[index]));
        CHECK(ids[index] == index + 1);
    }
    CHECK(lardon3d_task_queue_count(queue) == TASK_COUNT);
    Lardon3DTaskSnapshot snapshot;
    CHECK(wait_terminal(queue, ids[TASK_COUNT - 1], &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(log.count == TASK_COUNT);
    for (size_t index = 0; index < TASK_COUNT; ++index) {
        CHECK(log.order[index] == index);
    }
    Lardon3DTaskQueueSummary summary;
    Lardon3DTaskSnapshot listed[8];
    CHECK(lardon3d_task_queue_snapshot(queue, listed, 8, &summary) == 8);
    CHECK(summary.total == TASK_COUNT);
    CHECK(summary.running == 0);
    CHECK(summary.pending == 0);
    CHECK(summary.completed == TASK_COUNT);
    CHECK(lardon3d_task_queue_get_at(queue, 0, &snapshot));
    CHECK(snapshot.id == ids[0]);
    CHECK(!lardon3d_task_queue_get_at(queue, TASK_COUNT, &snapshot));
    CHECK(lardon3d_task_queue_remove(queue, ids[0]));
    CHECK(!lardon3d_task_queue_get(queue, ids[0], &snapshot));
    CHECK(lardon3d_task_queue_count(queue) == TASK_COUNT - 1);
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&log.mutex) == 0);

    queue = lardon3d_task_queue_create();
    CHECK(queue);
    OrderLog control_log = {0};
    CHECK(pthread_mutex_init(&control_log.mutex, NULL) == 0);
    QueueWork slow = {.log = &control_log, .value = 1, .steps = 500};
    QueueWork cancelled = {.log = &control_log, .value = 2, .steps = 1};
    Lardon3DTask *slow_task = lardon3d_task_create("Longue", queue_callback, &slow);
    Lardon3DTask *cancelled_task = lardon3d_task_create(
        "Annulée en attente",
        queue_callback,
        &cancelled
    );
    uint64_t slow_id;
    uint64_t cancelled_id;
    CHECK(slow_task && cancelled_task);
    CHECK(lardon3d_task_queue_add(queue, slow_task, &slow_id));
    CHECK(lardon3d_task_queue_add(queue, cancelled_task, &cancelled_id));
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        CHECK(lardon3d_task_queue_get(queue, slow_id, &snapshot));
        if (snapshot.state == TASK_RUNNING) {
            break;
        }
        short_pause();
    }
    CHECK(lardon3d_task_pause(slow_task));
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        CHECK(lardon3d_task_queue_get(queue, slow_id, &snapshot));
        if (snapshot.state == TASK_PAUSED) {
            break;
        }
        short_pause();
    }
    CHECK(snapshot.state == TASK_PAUSED);
    lardon3d_task_request_cancel(cancelled_task);
    CHECK(lardon3d_task_resume(slow_task));
    CHECK(wait_terminal(queue, slow_id, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(wait_terminal(queue, cancelled_id, &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(control_log.count == 1);
    CHECK(lardon3d_task_queue_remove(queue, cancelled_id));
    CHECK(lardon3d_task_queue_remove(queue, slow_id));
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&control_log.mutex) == 0);

    queue = lardon3d_task_queue_create();
    CHECK(queue);
    OrderLog destruction_log = {0};
    CHECK(pthread_mutex_init(&destruction_log.mutex, NULL) == 0);
    QueueWork destruction_work = {
        .log = &destruction_log,
        .value = 0,
        .steps = 10000,
    };
    Lardon3DTask *destruction_task = lardon3d_task_create(
        "Destruction sûre",
        queue_callback,
        &destruction_work
    );
    CHECK(destruction_task);
    CHECK(lardon3d_task_queue_add(queue, destruction_task, NULL));
    short_pause();
    lardon3d_task_queue_destroy(queue);
    CHECK(pthread_mutex_destroy(&destruction_log.mutex) == 0);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
