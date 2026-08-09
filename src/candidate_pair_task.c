#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/candidate_pair_gen.h>
#include <lardon3d/candidate_pair_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

enum {
    CANDIDATE_PAIR_MINIMUM_BATCH = 1,
    CANDIDATE_PAIR_MAXIMUM_BATCH = 64,
    CANDIDATE_PAIR_FIXED_MEMORY = 128 * 1024,
    CANDIDATE_PAIR_MEMORY_PER_ITEM = 256 * 256,
};

typedef struct {
    char project_path[PATH_MAX];
    Lardon3DProjectDb *database;
    Lardon3DResourceGovernor *governor;
    Lardon3DProjectDbCandidatePairGenerateTask parameters;
} Lardon3DCandidatePairTaskContext;

static void destroy_context(void *userdata) { free(userdata); }

static void runtime_state(const Lardon3DCandidatePairTaskContext *context,
                          Lardon3DAppState *state) {
    lardon3d_app_state_init(state);
    state->project_loaded = true;
    state->project_db = context->database;
    state->resource_governor = context->governor;
    (void)snprintf(state->project_path, sizeof(state->project_path), "%s",
                   context->project_path);
}

static void finished_callback(const Lardon3DTask *task, void *userdata) {
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
    const char *skip = getenv("LARDON3D_TEST_CANDIDATE_PAIR_SKIP_FINISHED_CHECKPOINT");
    if (skip && strcmp(skip, "1") == 0) return;
#endif
    Lardon3DCandidatePairTaskContext *context = userdata;
    Lardon3DAppState state;
    runtime_state(context, &state);
    (void)lardon3d_project_checkpoint_candidate_pair_generate_task(
        &state, task, &context->parameters);
}

static uint64_t elapsed_ns(struct timespec begin, struct timespec end) {
    uint64_t s = end.tv_sec >= begin.tv_sec ? (uint64_t)(end.tv_sec - begin.tv_sec) : 0;
    long n = end.tv_nsec - begin.tv_nsec;
    if (n < 0 && s) {
        --s;
        n += 1000000000L;
    }
    return s <= UINT64_MAX / 1000000000ULL
               ? s * 1000000000ULL + (uint64_t)n
               : UINT64_MAX;
}

enum { FEATURE_SET_PAGE = 64 };

static bool run(Lardon3DTask *task, void *userdata) {
    Lardon3DCandidatePairTaskContext *context = userdata;

    Lardon3DVisualIndexQueryOptions query_options = {
        .top_k = context->parameters.top_k,
        .minimum_evidence_count = context->parameters.minimum_evidence_count,
        .scanset_filter = (Lardon3DVisualIndexScanSetFilter)context->parameters.scanset_filter,
        .exclude_same_asset = context->parameters.exclude_same_asset,
    };

    uint64_t total_generated = 0;
    uint64_t total_feature_sets = 0;

    for (;;) {
        Lardon3DTaskExecutionContract contract;
        if (!lardon3d_task_execution_contract(task, &contract) ||
            contract.batch_size < CANDIDATE_PAIR_MINIMUM_BATCH ||
            contract.batch_size > CANDIDATE_PAIR_MAXIMUM_BATCH) {
            return lardon3d_task_fail(task, "Contrat de lot Candidate Pair invalide.");
        }

        size_t processed_in_sequence = 0;
        uint64_t cursor = context->parameters.after_feature_set_id;
        Lardon3DProjectDbFeatureSet page[FEATURE_SET_PAGE];
        size_t count = 0;
        Lardon3DProjectDbResult listed =
            lardon3d_project_db_list_feature_sets(context->database, cursor, page,
                                                  FEATURE_SET_PAGE, &count);
        if (listed != LARDON3D_PROJECT_DB_OK) {
            return lardon3d_task_fail(task,
                                      "Liste des Feature Sets impossible.");
        }

        struct timespec begin, end;
        (void)clock_gettime(CLOCK_MONOTONIC, &begin);
        uint64_t seq_generated = 0;

        while (count > 0 &&
               processed_in_sequence < contract.batch_size) {
            for (size_t i = 0; i < count &&
                 processed_in_sequence < contract.batch_size;
                 ++i) {
                if (!lardon3d_task_checkpoint(task)) {
                    return false;
                }
                Lardon3DCandidatePairGenStats stats;
                Lardon3DVisualIndexResult result =
                    lardon3d_candidate_pair_generate(
                        context->project_path, context->database,
                        context->parameters.visual_index_id,
                        page[i].feature_set_id, &query_options, &stats);
                if (result != LARDON3D_VISUAL_INDEX_OK) {
                    return lardon3d_task_fail(
                        task, "Génération Candidate Pair impossible.");
                }
                seq_generated += stats.generated_count;
                cursor = page[i].feature_set_id;
                ++processed_in_sequence;
            }
            if (processed_in_sequence < contract.batch_size) {
                listed = lardon3d_project_db_list_feature_sets(
                    context->database, cursor, page, FEATURE_SET_PAGE,
                    &count);
                if (listed != LARDON3D_PROJECT_DB_OK) {
                    return lardon3d_task_fail(
                        task, "Liste des Feature Sets impossible.");
                }
            }
        }

        (void)clock_gettime(CLOCK_MONOTONIC, &end);
        total_generated += seq_generated;
        total_feature_sets += processed_in_sequence;

        context->parameters.after_feature_set_id = cursor;

        bool exhausted = (count == 0);
        unsigned int progress = exhausted ? 100U
            : (total_feature_sets == 0 ? 0U
                   : (unsigned int)((total_feature_sets * 99U) /
                                    (total_feature_sets + 1U)));

        char msg[LARDON3D_TASK_MESSAGE_CAPACITY];
        (void)snprintf(msg, sizeof(msg), "FS:%lu générées:%lu",
                       (unsigned long)total_feature_sets,
                       (unsigned long)total_generated);
        if (!lardon3d_task_set_progress(task, progress, msg)) {
            return false;
        }

        (void)lardon3d_resource_governor_record_batch(
            context->governor, LARDON3D_RESOURCE_TASK_CPU,
            (size_t)seq_generated, elapsed_ns(begin, end), 0);

        Lardon3DAppState state;
        runtime_state(context, &state);
        if (lardon3d_project_checkpoint_candidate_pair_generate_task(
                &state, task, &context->parameters) !=
            LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
            return lardon3d_task_fail(
                task, "Checkpoint Candidate Pair impossible.");
        }

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
        bool test_pause_requested = false;
        const char *pause = getenv(
            "LARDON3D_TEST_CANDIDATE_PAIR_PAUSE_AFTER_BATCH");
        if (pause && strcmp(pause, "1") == 0) {
            (void)lardon3d_task_pause(task);
            test_pause_requested = true;
        }
#endif

        if (exhausted
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
            && !test_pause_requested
#endif
        ) {
            return lardon3d_task_set_progress(
                task, 100, "Génération Candidate Pair terminée.");
        }

        Lardon3DResourceReservation *reservation = NULL;
        if (!lardon3d_task_sequence_break(task, context->governor,
                                          &reservation, &contract)) {
            return false;
        }
    }
}

static Lardon3DCandidatePairTaskContext *
make_context(const Lardon3DTaskReconstructionContext *runtime,
             const Lardon3DProjectDbCandidatePairGenerateTask *parameters) {
    if (!runtime || !runtime->project_path || !runtime->project_db ||
        !runtime->resource_governor || !parameters) {
        return NULL;
    }
    Lardon3DCandidatePairTaskContext *context =
        calloc(1, sizeof(*context));
    if (!context) return NULL;
    int n = snprintf(context->project_path, sizeof(context->project_path),
                     "%s", runtime->project_path);
    if (n <= 0 || (size_t)n >= sizeof(context->project_path)) {
        free(context);
        return NULL;
    }
    context->database = runtime->project_db;
    context->governor = runtime->resource_governor;
    context->parameters = *parameters;
    return context;
}

bool lardon3d_candidate_pair_generate_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
    Lardon3DTaskReconstructionContext *runtime = userdata;
    if (!snapshot || !runtime || !binding) return false;
    Lardon3DProjectDbCandidatePairGenerateTask parameters;
    if (lardon3d_project_db_load_candidate_pair_generate_task(
            runtime->project_db, snapshot->id,
            &parameters) != LARDON3D_PROJECT_DB_OK) {
        return false;
    }
    Lardon3DCandidatePairTaskContext *context =
        make_context(runtime, &parameters);
    if (!context) return false;
    *binding = (Lardon3DTaskKindBinding){
        .callback = run,
        .userdata = context,
        .userdata_destroy = destroy_context,
        .finished_callback = finished_callback,
        .finished_userdata = context,
    };
    return true;
}

Lardon3DTask *lardon3d_project_create_candidate_pair_generate_task(
    Lardon3DAppState *state, uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    uint64_t *task_id) {
    if (task_id) *task_id = 0;
    if (!state || !state->project_loaded || !state->project_db ||
        !state->resource_governor || !task_id || visual_index_id == 0 ||
        !query_options || query_options->top_k == 0 ||
        query_options->top_k > LARDON3D_VISUAL_INDEX_TOP_K_MAX ||
        query_options->minimum_evidence_count > 1024 ||
        (int)query_options->scanset_filter < 0 ||
        (int)query_options->scanset_filter > 2) {
        return NULL;
    }
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id) !=
        LARDON3D_PROJECT_DB_OK) {
        return NULL;
    }
    Lardon3DProjectDbCandidatePairGenerateTask parameters = {
        .task_id = id,
        .visual_index_id = visual_index_id,
        .after_feature_set_id = 0,
        .top_k = query_options->top_k,
        .minimum_evidence_count = query_options->minimum_evidence_count,
        .scanset_filter = (int)query_options->scanset_filter,
        .exclude_same_asset = query_options->exclude_same_asset,
    };
    Lardon3DTaskReconstructionContext runtime = {
        .project_path = state->project_path,
        .project_db = state->project_db,
        .resource_governor = state->resource_governor,
        .orb_vulkan_backend = state->orb_vulkan_backend,
    };
    Lardon3DCandidatePairTaskContext *context =
        make_context(&runtime, &parameters);
    if (!context) return NULL;
    const Lardon3DResourceEstimate estimate = {
        .memory_fixed_bytes = CANDIDATE_PAIR_FIXED_MEMORY,
        .memory_bytes_per_item = CANDIDATE_PAIR_MEMORY_PER_ITEM,
        .minimum_batch_size = CANDIDATE_PAIR_MINIMUM_BATCH,
        .maximum_batch_size = CANDIDATE_PAIR_MAXIMUM_BATCH,
        .desired_cpu_threads = 1,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Génération Candidate Pair", &estimate,
        LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND,
        LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND_VERSION, run, context,
        destroy_context);
    if (!task || !lardon3d_task_assign_id(task, id) ||
        !lardon3d_task_set_finished_callback(task, finished_callback,
                                             context) ||
        lardon3d_project_checkpoint_candidate_pair_generate_task(
            state, task, &parameters) !=
            LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
        lardon3d_task_destroy(task);
        return NULL;
    }
    *task_id = id;
    return task;
}

bool lardon3d_project_enqueue_candidate_pair_generate(
    Lardon3DAppState *state, uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    uint64_t *task_id) {
    if (!state || !state->task_queue) return false;
    Lardon3DTask *task =
        lardon3d_project_create_candidate_pair_generate_task(
            state, visual_index_id, query_options, task_id);
    if (!task) return false;
    if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
        lardon3d_task_destroy(task);
        return false;
    }
    return true;
}
