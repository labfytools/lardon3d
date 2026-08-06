#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lardon3d/import.h>
#include <lardon3d/import_task.h>

struct Lardon3DImportTask {
    pthread_mutex_t mutex;
    pthread_t thread;
    bool thread_started;
    bool joined;
    bool cancel_requested;
    Lardon3DAppState worker_state;
    char source_directory[PATH_MAX];
    Lardon3DImportTaskSnapshot snapshot;
};

static void
copy_message(char destination[256], const char *message)
{
    (void)snprintf(destination, 256, "%s", message ? message : "");
}

static bool
task_is_cancelled(void *context)
{
    Lardon3DImportTask *task = context;
    bool cancelled;
    (void)pthread_mutex_lock(&task->mutex);
    cancelled = task->cancel_requested;
    (void)pthread_mutex_unlock(&task->mutex);
    return cancelled;
}

static void
task_progressed(void *context, const Lardon3DImportProgress *progress)
{
    Lardon3DImportTask *task = context;
    (void)pthread_mutex_lock(&task->mutex);
    task->snapshot.total = progress->total;
    task->snapshot.processed = progress->processed;
    task->snapshot.copied = progress->copied;
    task->snapshot.already_present = progress->already_present;
    task->snapshot.ignored = progress->ignored;
    copy_message(task->snapshot.message, progress->message);
    (void)pthread_mutex_unlock(&task->mutex);
}

static void *
run_import(void *argument)
{
    Lardon3DImportTask *task = argument;
    Lardon3DImportResult result;
    const Lardon3DImportControl control = {
        .context = task,
        .is_cancelled = task_is_cancelled,
        .progressed = task_progressed,
    };
    Lardon3DImportOutcome outcome = lardon3d_import_directory_controlled(
        &task->worker_state,
        task->source_directory,
        &result,
        &control
    );

    (void)pthread_mutex_lock(&task->mutex);
    task->snapshot.status = outcome == LARDON3D_IMPORT_SUCCEEDED
        ? LARDON3D_IMPORT_TASK_SUCCEEDED
        : outcome == LARDON3D_IMPORT_CANCELLED
            ? LARDON3D_IMPORT_TASK_CANCELLED
            : LARDON3D_IMPORT_TASK_FAILED;
    task->snapshot.total = result.admissible_found;
    task->snapshot.copied = result.copied;
    task->snapshot.already_present = result.already_present;
    copy_message(task->snapshot.message, task->worker_state.status_message);
    (void)pthread_mutex_unlock(&task->mutex);
    return NULL;
}

Lardon3DImportTask *
lardon3d_import_task_create(void)
{
    Lardon3DImportTask *task = calloc(1, sizeof(*task));
    if (!task) {
        return NULL;
    }
    if (pthread_mutex_init(&task->mutex, NULL) != 0) {
        free(task);
        return NULL;
    }
    task->snapshot.status = LARDON3D_IMPORT_TASK_IDLE;
    return task;
}

bool
lardon3d_import_task_start(
    Lardon3DImportTask *task,
    const Lardon3DAppState *state,
    const char *source_directory
)
{
    if (!task || !state || !source_directory) {
        return false;
    }

    (void)pthread_mutex_lock(&task->mutex);
    if (task->thread_started) {
        (void)pthread_mutex_unlock(&task->mutex);
        return false;
    }
    int written = snprintf(
        task->source_directory,
        sizeof(task->source_directory),
        "%s",
        source_directory
    );
    if (written < 0 || (size_t)written >= sizeof(task->source_directory)) {
        copy_message(task->snapshot.message, "Erreur : chemin source trop long.");
        task->snapshot.status = LARDON3D_IMPORT_TASK_FAILED;
        (void)pthread_mutex_unlock(&task->mutex);
        return false;
    }
    task->worker_state = *state;
    task->cancel_requested = false;
    task->joined = false;
    task->snapshot = (Lardon3DImportTaskSnapshot) {
        .status = LARDON3D_IMPORT_TASK_RUNNING,
        .message = "Analyse du dossier source...",
    };
    task->thread_started = true;
    int error = pthread_create(&task->thread, NULL, run_import, task);
    if (error != 0) {
        task->thread_started = false;
        task->snapshot.status = LARDON3D_IMPORT_TASK_FAILED;
        copy_message(
            task->snapshot.message,
            "Erreur : impossible de lancer la tâche d'import."
        );
        (void)pthread_mutex_unlock(&task->mutex);
        return false;
    }
    (void)pthread_mutex_unlock(&task->mutex);
    return true;
}

void
lardon3d_import_task_request_cancel(Lardon3DImportTask *task)
{
    if (!task) {
        return;
    }
    (void)pthread_mutex_lock(&task->mutex);
    if (task->snapshot.status == LARDON3D_IMPORT_TASK_RUNNING) {
        task->cancel_requested = true;
        copy_message(task->snapshot.message, "Annulation demandée...");
    }
    (void)pthread_mutex_unlock(&task->mutex);
}

bool
lardon3d_import_task_snapshot(
    Lardon3DImportTask *task,
    Lardon3DImportTaskSnapshot *snapshot
)
{
    if (!task || !snapshot) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    *snapshot = task->snapshot;
    (void)pthread_mutex_unlock(&task->mutex);
    return true;
}

bool
lardon3d_import_task_is_finished(Lardon3DImportTask *task)
{
    Lardon3DImportTaskSnapshot snapshot;
    if (!lardon3d_import_task_snapshot(task, &snapshot)) {
        return false;
    }
    return snapshot.status == LARDON3D_IMPORT_TASK_SUCCEEDED
        || snapshot.status == LARDON3D_IMPORT_TASK_CANCELLED
        || snapshot.status == LARDON3D_IMPORT_TASK_FAILED;
}

bool
lardon3d_import_task_join(Lardon3DImportTask *task)
{
    if (!task) {
        return false;
    }

    (void)pthread_mutex_lock(&task->mutex);
    bool started = task->thread_started;
    bool joined = task->joined;
    bool should_join = started && !joined;
    pthread_t thread = task->thread;
    (void)pthread_mutex_unlock(&task->mutex);
    if (!should_join) {
        return started && joined;
    }
    if (pthread_join(thread, NULL) != 0) {
        return false;
    }
    (void)pthread_mutex_lock(&task->mutex);
    task->joined = true;
    (void)pthread_mutex_unlock(&task->mutex);
    return true;
}

void
lardon3d_import_task_destroy(Lardon3DImportTask *task)
{
    if (!task) {
        return;
    }
    if (task->thread_started && !task->joined) {
        lardon3d_import_task_request_cancel(task);
        if (!lardon3d_import_task_join(task)) {
            return;
        }
    }
    (void)pthread_mutex_destroy(&task->mutex);
    free(task);
}
