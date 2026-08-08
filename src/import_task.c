#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/import.h>
#include <lardon3d/import_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

enum {
    IMAGE_IMPORT_MINIMUM_BATCH = 1,
    IMAGE_IMPORT_MAXIMUM_BATCH = 32,
    IMAGE_IMPORT_FIXED_MEMORY = 128 * 1024,
    IMAGE_IMPORT_MEMORY_PER_ITEM = NAME_MAX + 64,
};

typedef struct {
    char project_path[PATH_MAX];
    char source_path[PATH_MAX];
    Lardon3DProjectDb *project_db;
    Lardon3DResourceGovernor *governor;
} Lardon3DImageImportContext;

struct Lardon3DImportTask {
    Lardon3DAppState *state;
    uint64_t task_id;
};

static bool
canonical_source(const char *source, char output[PATH_MAX])
{
    if (!source || !source[0] || strnlen(source, PATH_MAX) >= PATH_MAX) return false;
    char absolute[PATH_MAX];
    int written;
    if (source[0] == '/') {
        written = snprintf(absolute, sizeof(absolute), "%s", source);
    } else {
        char current[PATH_MAX];
        if (!getcwd(current, sizeof(current))) return false;
        written = snprintf(absolute, sizeof(absolute), "%s/%s", current, source);
    }
    if (written <= 0 || (size_t)written >= sizeof(absolute)) return false;
    struct stat information;
    if (lstat(absolute, &information) != 0 || S_ISLNK(information.st_mode)
        || !S_ISDIR(information.st_mode)) return false;
    written = snprintf(output, PATH_MAX, "%s", absolute);
    return written > 0 && (size_t)written < PATH_MAX;
}

static void
destroy_context(void *userdata)
{
    free(userdata);
}

static void
runtime_project(const Lardon3DImageImportContext *context, Lardon3DAppState *state)
{
    lardon3d_app_state_init(state);
    state->project_loaded = true;
    state->project_db = context->project_db;
    state->resource_governor = context->governor;
    (void)snprintf(state->project_path, sizeof(state->project_path), "%s",
        context->project_path);
}

static bool
cooperative_continue(void *userdata)
{
    return !lardon3d_task_checkpoint(userdata);
}

static void
import_finished(const Lardon3DTask *task, void *userdata)
{
#ifdef LARDON3D_IMPORT_TASK_TESTING
    const char *skip = getenv("LARDON3D_TEST_IMPORT_SKIP_FINISHED_CHECKPOINT");
    if (skip && strcmp(skip, "1") == 0) return;
#endif
    Lardon3DImageImportContext *context = userdata;
    Lardon3DAppState state;
    runtime_project(context, &state);
    (void)lardon3d_project_checkpoint_task(&state, task);
}

static uint64_t
elapsed_ns(struct timespec begin, struct timespec end)
{
    uint64_t seconds = end.tv_sec >= begin.tv_sec
        ? (uint64_t)(end.tv_sec - begin.tv_sec) : 0;
    long nanoseconds = end.tv_nsec - begin.tv_nsec;
    if (nanoseconds < 0 && seconds > 0) {
        --seconds;
        nanoseconds += 1000000000L;
    }
    return seconds <= UINT64_MAX / UINT64_C(1000000000)
        ? seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds : UINT64_MAX;
}

static bool
run_image_import(Lardon3DTask *task, void *userdata)
{
    Lardon3DImageImportContext *context = userdata;
    Lardon3DAppState state;
    runtime_project(context, &state);
    for (;;) {
        Lardon3DTaskExecutionContract contract;
        if (!lardon3d_task_execution_contract(task, &contract)
            || contract.batch_size < IMAGE_IMPORT_MINIMUM_BATCH
            || contract.batch_size > IMAGE_IMPORT_MAXIMUM_BATCH) {
            return lardon3d_task_fail(task, "Contrat de lot import invalide.");
        }
        Lardon3DImportControl control = {
            .context = task,
            .is_cancelled = cooperative_continue,
        };
        Lardon3DImportResult result;
        bool complete = false;
        struct timespec begin, end;
        (void)clock_gettime(CLOCK_MONOTONIC, &begin);
        Lardon3DImportOutcome outcome = lardon3d_import_directory_batch(
            &state, context->source_path, contract.batch_size, &result,
            &control, &complete);
        (void)clock_gettime(CLOCK_MONOTONIC, &end);
        if (outcome == LARDON3D_IMPORT_CANCELLED) return false;
        if (outcome != LARDON3D_IMPORT_SUCCEEDED) {
            return lardon3d_task_fail(task, state.status_message);
        }
        unsigned int progress = complete ? 100U : result.admissible_found == 0
            ? 99U : (unsigned int)((result.processed * 99U)
                / result.admissible_found);
        if (!lardon3d_task_set_progress(task, progress, state.status_message)) {
            return false;
        }
        (void)lardon3d_resource_governor_record_batch(
            context->governor, LARDON3D_RESOURCE_TASK_IMPORT,
            result.newly_manifested,
            elapsed_ns(begin, end), 0);
        if (lardon3d_project_checkpoint_image_import_task(
                &state, task, context->source_path)
            != LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
            return lardon3d_task_fail(task, "Checkpoint import impossible.");
        }
        if (complete) return true;
#ifdef LARDON3D_IMPORT_TASK_TESTING
        const char *pause = getenv("LARDON3D_TEST_IMPORT_PAUSE_AFTER_BATCH");
        if (pause && strcmp(pause, "1") == 0) {
            (void)lardon3d_task_pause(task);
        }
#endif
        Lardon3DResourceReservation *reservation = NULL;
        if (!lardon3d_task_sequence_break(task, context->governor,
                &reservation, &contract)) return false;
    }
}

static Lardon3DImageImportContext *
create_context(const char *project_path, const char *source_path,
    Lardon3DProjectDb *database, Lardon3DResourceGovernor *governor)
{
    if (!project_path || !source_path || !database || !governor) return NULL;
    Lardon3DImageImportContext *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    int project_written = snprintf(context->project_path,
        sizeof(context->project_path), "%s", project_path);
    int source_written = snprintf(context->source_path,
        sizeof(context->source_path), "%s", source_path);
    if (project_written <= 0 || (size_t)project_written >= sizeof(context->project_path)
        || source_written <= 0 || (size_t)source_written >= sizeof(context->source_path)) {
        free(context); return NULL;
    }
    context->project_db = database;
    context->governor = governor;
    return context;
}

bool
lardon3d_image_import_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot,
    void *userdata,
    Lardon3DTaskKindBinding *binding
)
{
    Lardon3DImageImportReconstructionContext *runtime = userdata;
    if (!snapshot || !runtime || !runtime->project_path || !runtime->project_db
        || !runtime->resource_governor || !binding) return false;
    Lardon3DProjectDbImageImport parameters;
    if (lardon3d_project_db_load_image_import(runtime->project_db, snapshot->id,
            &parameters) != LARDON3D_PROJECT_DB_OK) return false;
    char source[PATH_MAX];
    if (!canonical_source(parameters.source_path, source)) return false;
    Lardon3DImageImportContext *context = create_context(runtime->project_path,
        source, runtime->project_db, runtime->resource_governor);
    if (!context) return false;
    *binding = (Lardon3DTaskKindBinding) {
        .callback = run_image_import,
        .userdata = context,
        .userdata_destroy = destroy_context,
        .finished_callback = import_finished,
        .finished_userdata = context,
    };
    return true;
}

Lardon3DTask *
lardon3d_project_create_image_import_task(
    Lardon3DAppState *state,
    const char *source_directory,
    uint64_t *task_id
)
{
    if (task_id) *task_id = 0;
    if (!state || !state->project_loaded || !state->project_db
        || !state->resource_governor || !task_id) return NULL;
    char source[PATH_MAX];
    if (!canonical_source(source_directory, source)) return NULL;
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id)
        != LARDON3D_PROJECT_DB_OK) return NULL;
    Lardon3DImageImportContext *context = create_context(state->project_path,
        source, state->project_db, state->resource_governor);
    if (!context) return NULL;
    const Lardon3DResourceEstimate estimate = {
        .memory_fixed_bytes = IMAGE_IMPORT_FIXED_MEMORY,
        .memory_bytes_per_item = IMAGE_IMPORT_MEMORY_PER_ITEM,
        .minimum_batch_size = IMAGE_IMPORT_MINIMUM_BATCH,
        .maximum_batch_size = IMAGE_IMPORT_MAXIMUM_BATCH,
        .desired_cpu_threads = 1,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_IMPORT,
    };
    Lardon3DTask *task = lardon3d_task_create_typed("Import d'images",
        &estimate, LARDON3D_IMAGE_IMPORT_TASK_KIND,
        LARDON3D_IMAGE_IMPORT_TASK_KIND_VERSION, run_image_import, context,
        destroy_context);
    if (!task || !lardon3d_task_assign_id(task, id)
        || !lardon3d_task_set_finished_callback(task, import_finished, context)
        || lardon3d_project_checkpoint_image_import_task(state, task, source)
            != LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
        lardon3d_task_destroy(task);
        return NULL;
    }
    *task_id = id;
    return task;
}

bool
lardon3d_project_enqueue_image_import(Lardon3DAppState *state,
    const char *source_directory, uint64_t *task_id)
{
    if (!state || !state->task_queue) return false;
    Lardon3DTask *task = lardon3d_project_create_image_import_task(
        state, source_directory, task_id);
    if (!task) return false;
    if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
        lardon3d_task_destroy(task);
        return false;
    }
    return true;
}

Lardon3DImportTask *lardon3d_import_task_create(void) { return calloc(1, sizeof(Lardon3DImportTask)); }

bool
lardon3d_import_task_start(Lardon3DImportTask *task, Lardon3DAppState *state,
    const char *source_directory)
{
    if (!task || task->task_id != 0 || !state) return false;
    task->state = state;
    return lardon3d_project_enqueue_image_import(state, source_directory,
        &task->task_id);
}

void lardon3d_import_task_request_cancel(Lardon3DImportTask *task)
{ if (task && task->state) (void)lardon3d_task_queue_cancel(task->state->task_queue, task->task_id); }

bool
lardon3d_import_task_snapshot(Lardon3DImportTask *task,
    Lardon3DImportTaskSnapshot *snapshot)
{
    if (!task || !task->state || !snapshot) return false;
    Lardon3DTaskSnapshot generic;
    if (!lardon3d_task_queue_get(task->state->task_queue, task->task_id,
            &generic)) return false;
    *snapshot = (Lardon3DImportTaskSnapshot) {
        .status = generic.state == TASK_COMPLETED ? LARDON3D_IMPORT_TASK_SUCCEEDED
            : generic.state == TASK_CANCELLED ? LARDON3D_IMPORT_TASK_CANCELLED
            : generic.state == TASK_FAILED ? LARDON3D_IMPORT_TASK_FAILED
            : LARDON3D_IMPORT_TASK_RUNNING,
        .total = 100,
        .processed = generic.progress,
    };
    (void)snprintf(snapshot->message, sizeof(snapshot->message), "%s",
        generic.message);
    return true;
}

bool lardon3d_import_task_is_finished(Lardon3DImportTask *task)
{ Lardon3DImportTaskSnapshot snapshot; return lardon3d_import_task_snapshot(task, &snapshot)
    && snapshot.status != LARDON3D_IMPORT_TASK_RUNNING; }
bool lardon3d_import_task_join(Lardon3DImportTask *task)
{ return task && lardon3d_import_task_is_finished(task); }
void
lardon3d_import_task_destroy(Lardon3DImportTask *task)
{
    if (task && lardon3d_import_task_is_finished(task)) {
        (void)lardon3d_task_queue_remove(task->state->task_queue, task->task_id);
    }
    free(task);
}
