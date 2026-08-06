#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/task.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition); \
            return false; \
        } \
    } while (0)

typedef struct {
    size_t steps;
    long pause_ns;
} Work;

static void
short_pause(long nanoseconds)
{
    struct timespec duration = {.tv_sec = 0, .tv_nsec = nanoseconds};
    (void)nanosleep(&duration, NULL);
}

static bool
work_callback(Lardon3DTask *task, void *userdata)
{
    Work *work = userdata;
    for (size_t step = 0; step < work->steps; ++step) {
        if (!lardon3d_task_checkpoint(task)) {
            return false;
        }
        unsigned int progress = (unsigned int)(((step + 1) * 100) / work->steps);
        if (!lardon3d_task_set_progress(task, progress, "Traitement.")) {
            return false;
        }
        short_pause(work->pause_ns);
    }
    return true;
}

static bool
failure_callback(Lardon3DTask *task, void *userdata)
{
    (void)userdata;
    return lardon3d_task_fail(task, "Erreur contrôlée.") && false;
}

static void *
start_task(void *context)
{
    Lardon3DTask *task = context;
    return (void *)(uintptr_t)(lardon3d_task_start(task) ? 1 : 0);
}

static bool
wait_for_state(Lardon3DTask *task, Lardon3DTaskState expected)
{
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        Lardon3DTaskSnapshot snapshot;
        if (!lardon3d_task_snapshot(task, &snapshot)) {
            return false;
        }
        if (snapshot.state == expected) {
            return true;
        }
        short_pause(1000000);
    }
    return false;
}

static bool
run_test(void)
{
    CHECK(!lardon3d_task_create(NULL, work_callback, NULL));
    CHECK(!lardon3d_task_create("", work_callback, NULL));
    CHECK(!lardon3d_task_create("invalide", NULL, NULL));
    lardon3d_task_destroy(NULL);
    CHECK(!lardon3d_task_join(NULL));

    Work work = {.steps = 100, .pause_ns = 1000000};
    Lardon3DTask *task = lardon3d_task_create("Tâche de test", work_callback, &work);
    CHECK(task);
    CHECK(lardon3d_task_assign_id(task, 42));
    CHECK(!lardon3d_task_assign_id(task, 43));
    CHECK(lardon3d_task_id(task) == 42);
    Lardon3DTaskSnapshot snapshot;
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_PENDING);
    CHECK(snapshot.progress == 0);
    CHECK(strcmp(snapshot.name, "Tâche de test") == 0);

    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, start_task, task) == 0);
    CHECK(wait_for_state(task, TASK_RUNNING));
    CHECK(lardon3d_task_pause(task));
    CHECK(wait_for_state(task, TASK_PAUSED));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    unsigned int paused_progress = snapshot.progress;
    short_pause(5000000);
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.progress == paused_progress);
    CHECK(lardon3d_task_resume(task));
    CHECK(wait_for_state(task, TASK_RUNNING));
    CHECK(lardon3d_task_join(task));
    void *thread_result;
    CHECK(pthread_join(thread, &thread_result) == 0);
    CHECK((uintptr_t)thread_result == 1);
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_COMPLETED);
    CHECK(snapshot.progress == 100);
    CHECK(snapshot.started_at.tv_sec > 0);
    CHECK(snapshot.finished_at.tv_sec > 0);
    CHECK(!lardon3d_task_start(task));
    lardon3d_task_destroy(task);

    work = (Work) {.steps = 1000, .pause_ns = 1000000};
    task = lardon3d_task_create("Annulation", work_callback, &work);
    CHECK(task && pthread_create(&thread, NULL, start_task, task) == 0);
    CHECK(wait_for_state(task, TASK_RUNNING));
    lardon3d_task_request_cancel(task);
    CHECK(lardon3d_task_join(task));
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_CANCELLED);
    CHECK(snapshot.progress < 100);
    lardon3d_task_destroy(task);

    task = lardon3d_task_create("Échec", failure_callback, NULL);
    CHECK(task && lardon3d_task_start(task));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_FAILED);
    CHECK(strcmp(snapshot.message, "Erreur contrôlée.") == 0);
    lardon3d_task_destroy(task);

    task = lardon3d_task_create("Pause avant départ", work_callback, &work);
    CHECK(task && lardon3d_task_pause(task));
    CHECK(lardon3d_task_snapshot(task, &snapshot));
    CHECK(snapshot.state == TASK_PAUSED);
    CHECK(lardon3d_task_resume(task));
    lardon3d_task_request_cancel(task);
    CHECK(lardon3d_task_join(task));
    lardon3d_task_destroy(task);
    return true;
}

int
main(void)
{
    return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
