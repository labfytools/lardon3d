#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
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

#include "candidate_pair_gen_internal.h"
#include "task_internal.h"

enum {
    CANDIDATE_PAIR_MINIMUM_BATCH = 1,
    CANDIDATE_PAIR_MAXIMUM_BATCH = 64,
    /* ALGORITHMIC / RESOURCE: a participant needs one independent source, so
     * the existing 64-item batch bound is also the portable CPU safety bound.
     * Governor admission may select fewer CPUs without changing Candidate
     * identity, ordering, or the number of items admitted for this sequence. */
    CANDIDATE_PAIR_CPU_MAX_SAFE = CANDIDATE_PAIR_MAXIMUM_BATCH,
    CANDIDATE_PAIR_WINDOW_PER_THREAD = 2,
    CANDIDATE_PAIR_WINDOW_MAX = CANDIDATE_PAIR_MAXIMUM_BATCH,
    CANDIDATE_PAIR_CHILD_STACK_BYTES = 4 * 1024 * 1024,
    CANDIDATE_PAIR_FIXED_MEMORY = 256 * 1024,
    /* RESOURCE: one admitted item can own one participant. Eight MiB covers
     * its bounded 4 MiB pthread stack plus the Visual Index query buffers and
     * private SQLite reader/cache allowance. This is sequence memory, not a
     * dataset-size limit; Governor may reduce the batch on smaller hosts. */
    CANDIDATE_PAIR_MEMORY_PER_ITEM = 8 * 1024 * 1024,
};

typedef struct {
    char project_path[PATH_MAX];
    char database_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
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

typedef struct {
    const char *project_path;
    Lardon3DProjectDb *database;
    uint64_t visual_index_id;
    const Lardon3DVisualIndexQueryOptions *query_options;
    const uint64_t *source_ids;
    Lardon3DCandidatePairComputation *computations;
    Lardon3DVisualIndexResult *results;
    size_t source_count;
    size_t first;
    size_t stride;
} CandidateComputeWorker;

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
static atomic_size_t test_started_participants;
static atomic_size_t test_computed_work_items;
static atomic_size_t test_active_private_databases;
static atomic_size_t test_fail_thread_create_after = SIZE_MAX;

void lardon3d_candidate_pair_task_test_reset_parallel_counters(void) {
    atomic_store(&test_started_participants, 0);
    atomic_store(&test_computed_work_items, 0);
}

size_t lardon3d_candidate_pair_task_test_started_participants(void) {
    return atomic_load(&test_started_participants);
}

size_t lardon3d_candidate_pair_task_test_computed_work_items(void) {
    return atomic_load(&test_computed_work_items);
}

size_t lardon3d_candidate_pair_task_test_active_private_databases(void) {
    return atomic_load(&test_active_private_databases);
}

void lardon3d_candidate_pair_task_test_fail_thread_create_after(
    size_t successful_children) {
    atomic_store(&test_fail_thread_create_after, successful_children);
}
#endif

static void close_worker_database(CandidateComputeWorker *worker) {
    if (!worker || !worker->database) return;
    lardon3d_project_db_close(worker->database);
    worker->database = NULL;
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
    atomic_fetch_sub(&test_active_private_databases, 1);
#endif
}

static void *compute_worker(void *userdata) {
    CandidateComputeWorker *worker = userdata;
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
    atomic_fetch_add(&test_started_participants, 1);
#endif
    for (size_t i = worker->first; i < worker->source_count; i += worker->stride) {
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
        atomic_fetch_add(&test_computed_work_items, 1);
#endif
        worker->results[i] = lardon3d_candidate_pair_compute(
            worker->project_path, worker->database, worker->visual_index_id,
            worker->source_ids[i], worker->query_options, &worker->computations[i]);
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
        const char *failed_id = getenv("LARDON3D_TEST_CANDIDATE_PAIR_FAIL_SOURCE_ID");
        if (failed_id && strtoull(failed_id, NULL, 10) == worker->source_ids[i]) {
            worker->results[i] = LARDON3D_VISUAL_INDEX_IO_ERROR;
        }
#endif
    }
    close_worker_database(worker);
    return NULL;
}

static int create_compute_thread(pthread_t *thread,
                                 const pthread_attr_t *attributes,
                                 CandidateComputeWorker *worker,
                                 size_t successful_children) {
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
    /* This private seam fails before pthread_create, after an exact number of
     * successful children. It deterministically exercises the production
     * join/handle cleanup path without creating an unowned thread. */
    if (atomic_load(&test_fail_thread_create_after) == successful_children) {
        return -1;
    }
#else
    (void)successful_children;
#endif
    return pthread_create(thread, attributes, compute_worker, worker);
}

static unsigned int membership_progress(uint64_t completed, uint64_t total) {
    if (completed >= total && total != 0) return 99U;
    if (total == 0) return 0U;
    /* Find floor(completed * 99 / total) without overflowing uint64_t.
     * Progress has only 99 incomplete buckets, so threshold comparison is
     * both exact and independent of floating-point behavior. */
    unsigned int progress = 0;
    uint64_t quotient = total / 99U;
    uint64_t remainder = total % 99U;
    for (unsigned int candidate = 1; candidate <= 99U; ++candidate) {
        uint64_t threshold = quotient * candidate +
            (remainder * candidate + 98U) / 99U;
        if (completed < threshold) break;
        progress = candidate;
    }
    return progress;
}

static bool set_membership_progress(Lardon3DTask *task, uint64_t completed,
                                    uint64_t total, uint64_t generated) {
    char message[LARDON3D_TASK_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message), "FS:%lu/%lu générées:%lu",
                   (unsigned long)completed, (unsigned long)total,
                   (unsigned long)generated);
    return lardon3d_task_set_progress(
        task, membership_progress(completed, total), message);
}

static bool compute_window(const Lardon3DCandidatePairTaskContext *context,
                           uint64_t visual_index_id,
                           const Lardon3DVisualIndexQueryOptions *query_options,
                           const uint64_t *source_ids, size_t source_count,
                           unsigned int admitted_threads,
                           Lardon3DCandidatePairComputation *computations,
                           Lardon3DVisualIndexResult *results) {
    if (!context || !query_options || !source_ids || !computations || !results ||
        source_count == 0 || source_count > CANDIDATE_PAIR_WINDOW_MAX ||
        admitted_threads == 0 ||
        admitted_threads > CANDIDATE_PAIR_CPU_MAX_SAFE) {
        return false;
    }
    unsigned int worker_count = admitted_threads;
    /* source_count is validated against the 64-item bound before narrowing. */
    if (worker_count > source_count) worker_count = (unsigned int)source_count;
    CandidateComputeWorker workers[CANDIDATE_PAIR_CPU_MAX_SAFE];
    pthread_t threads[CANDIDATE_PAIR_CPU_MAX_SAFE - 1];
    size_t created = 0;
    bool started = true;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    /* INVARIANT: child stack reservation is explicitly bounded and included
     * in memory_bytes_per_item; relying on the host pthread default would make
     * a 64-participant admission unaccountable and non-portable. */
    if (pthread_attr_setstacksize(&attributes,
                                  CANDIDATE_PAIR_CHILD_STACK_BYTES) != 0) {
        (void)pthread_attr_destroy(&attributes);
        return false;
    }
    for (size_t i = 0; i < source_count; ++i) {
        results[i] = LARDON3D_VISUAL_INDEX_IO_ERROR;
    }
    for (unsigned int i = 0; i < worker_count; ++i) {
        workers[i] = (CandidateComputeWorker){
            .project_path = context->project_path,
            .visual_index_id = visual_index_id,
            .query_options = query_options,
            .source_ids = source_ids,
            .computations = computations,
            .results = results,
            .source_count = source_count,
            .first = i,
            .stride = worker_count,
        };
        char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
        if (lardon3d_project_db_open(context->database_path,
                                    &workers[i].database, error) !=
            LARDON3D_PROJECT_DB_OK) {
            for (unsigned int opened = 0; opened < i; ++opened) {
                close_worker_database(&workers[opened]);
            }
            (void)pthread_attr_destroy(&attributes);
            return false;
        }
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
        atomic_fetch_add(&test_active_private_databases, 1);
#endif
    }
    for (unsigned int i = 1; i < worker_count; ++i) {
        if (create_compute_thread(&threads[created], &attributes, &workers[i],
                                  created) != 0) {
            started = false;
            break;
        }
        ++created;
    }
    if (pthread_attr_destroy(&attributes) != 0) started = false;
    if (worker_count > 0) (void)compute_worker(&workers[0]);
    for (size_t i = 0; i < created; ++i) {
        if (pthread_join(threads[i], NULL) != 0) started = false;
    }
    for (unsigned int i = (unsigned int)created + 1; i < worker_count; ++i) {
        close_worker_database(&workers[i]);
    }
    return started;
}

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
bool lardon3d_candidate_pair_task_test_compute_window(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    const uint64_t *source_ids, size_t source_count,
    unsigned int admitted_threads,
    Lardon3DCandidatePairComputation *computations,
    Lardon3DVisualIndexResult *results) {
    if (!project_path || !database) return false;
    Lardon3DCandidatePairTaskContext context = {0};
    int written = snprintf(context.project_path, sizeof(context.project_path),
                           "%s", project_path);
    if (written <= 0 || (size_t)written >= sizeof(context.project_path) ||
        !lardon3d_project_db_copy_path(database, context.database_path)) {
        return false;
    }
    /* The seam delegates to the production fan-out and owns no publication.
     * Tests can therefore exercise all 64 participants without inventing Task
     * progress, Candidate identities, or a second scheduling implementation. */
    return compute_window(&context, visual_index_id, query_options, source_ids,
                          source_count, admitted_threads, computations, results);
}
#endif

static bool run(Lardon3DTask *task, void *userdata) {
    Lardon3DCandidatePairTaskContext *context = userdata;

    Lardon3DVisualIndexQueryOptions query_options = {
        .top_k = context->parameters.top_k,
        .minimum_evidence_count = context->parameters.minimum_evidence_count,
        .scanset_filter = (Lardon3DVisualIndexScanSetFilter)context->parameters.scanset_filter,
        .exclude_same_asset = context->parameters.exclude_same_asset,
    };

    uint64_t total_generated = 0;
    uint64_t completed_memberships = 0;
    uint64_t total_memberships = 0;
    if (lardon3d_project_db_visual_index_membership_progress(
            context->database, context->parameters.visual_index_id,
            context->parameters.after_feature_set_id, &completed_memberships,
            &total_memberships) != LARDON3D_PROJECT_DB_OK) {
        return lardon3d_task_fail(task, "Comptage des memberships impossible.");
    }
    if (!set_membership_progress(task, completed_memberships, total_memberships, 0)) {
        return false;
    }

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
    bool test_pause_before_batch = false;
#endif
    for (;;) {
        Lardon3DTaskExecutionContract contract;
        if (!lardon3d_task_execution_contract(task, &contract) ||
            contract.batch_size < CANDIDATE_PAIR_MINIMUM_BATCH ||
            contract.batch_size > CANDIDATE_PAIR_MAXIMUM_BATCH ||
            contract.cpu_threads == 0 ||
            contract.cpu_threads > CANDIDATE_PAIR_CPU_MAX_SAFE) {
            return lardon3d_task_fail(task, "Contrat de lot Candidate Pair invalide.");
        }

        size_t processed_in_sequence = 0;
        uint64_t cursor = context->parameters.after_feature_set_id;

        struct timespec begin, end;
        (void)clock_gettime(CLOCK_MONOTONIC, &begin);
        uint64_t seq_generated = 0;

        bool exhausted = false;
        while (processed_in_sequence < contract.batch_size) {
            if (!lardon3d_task_checkpoint(task)) return false;
            size_t remaining = contract.batch_size - processed_in_sequence;
            size_t window_capacity = contract.cpu_threads >
                                             CANDIDATE_PAIR_WINDOW_MAX /
                                                 CANDIDATE_PAIR_WINDOW_PER_THREAD
                                         ? CANDIDATE_PAIR_WINDOW_MAX
                                         : (size_t)contract.cpu_threads *
                                               CANDIDATE_PAIR_WINDOW_PER_THREAD;
            if (window_capacity > remaining) window_capacity = remaining;
            uint64_t source_ids[CANDIDATE_PAIR_WINDOW_MAX];
            size_t source_count = 0;
            Lardon3DProjectDbResult listed =
                lardon3d_project_db_list_visual_index_memberships(
                    context->database, context->parameters.visual_index_id,
                    cursor, source_ids, window_capacity, &source_count);
            if (listed != LARDON3D_PROJECT_DB_OK) {
                return lardon3d_task_fail(task, "Liste des memberships impossible.");
            }
            if (source_count == 0) {
                exhausted = true;
                break;
            }

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
            const char *pause_before = getenv(
                "LARDON3D_TEST_CANDIDATE_PAIR_PAUSE_BEFORE_BATCH");
            if (pause_before && strcmp(pause_before, "1") == 0) {
                /* Keep the typed cursor at the durable prefix before this
                 * window. This test-only interruption models a process death
                 * before participant dispatch, so recovery must exercise the
                 * normal Registry -> Queue -> Governor callback path. */
                if (!lardon3d_task_checkpoint(task)) return false;
                (void)lardon3d_task_pause(task);
                test_pause_before_batch = true;
                exhausted = true;
                break;
            }
#endif

            Lardon3DCandidatePairComputation computations[CANDIDATE_PAIR_WINDOW_MAX];
            Lardon3DVisualIndexResult results[CANDIDATE_PAIR_WINDOW_MAX];
            /* CONTRACT: the Queue callback is one admitted CPU participant.
             * At most cpu_threads-1 child threads are created, every useful
             * participant owns one private SQLite handle, and all are joined
             * and closed before reservation release or sequence_break. The
             * per-item charge covers this bounded participant state and stack;
             * batch=1 therefore cannot allocate idle CPU workers. */
            if (!compute_window(context, context->parameters.visual_index_id,
                                &query_options, source_ids, source_count,
                                contract.cpu_threads, computations, results)) {
                return lardon3d_task_fail(task, "Workers Candidate Pair impossibles.");
            }
            for (size_t i = 0; i < source_count; ++i) {
                if (!lardon3d_task_checkpoint(task)) return false;
                if (results[i] != LARDON3D_VISUAL_INDEX_OK) {
                    return lardon3d_task_fail(task,
                                              "Calcul Candidate Pair impossible.");
                }
                Lardon3DCandidatePairGenStats stats;
                Lardon3DVisualIndexResult published = lardon3d_candidate_pair_publish(
                    context->database, &computations[i], &stats);
                if (published != LARDON3D_VISUAL_INDEX_OK) {
                    return lardon3d_task_fail(task,
                                              "Publication Candidate Pair impossible.");
                }
                seq_generated += stats.generated_count;
                ++processed_in_sequence;
                ++completed_memberships;
                cursor = source_ids[i];
                context->parameters.after_feature_set_id = cursor;
                if (!set_membership_progress(task, completed_memberships,
                                             total_memberships,
                                             total_generated + seq_generated)) {
                    return false;
                }
            }
        }

        (void)clock_gettime(CLOCK_MONOTONIC, &end);
        total_generated += seq_generated;
        context->parameters.after_feature_set_id = cursor;

        (void)lardon3d_resource_governor_record_batch(
            context->governor, LARDON3D_RESOURCE_TASK_CPU,
            (size_t)seq_generated, elapsed_ns(begin, end), 0);
        (void)lardon3d_task_internal_record_sequence(
            task, elapsed_ns(begin, end), processed_in_sequence);

        Lardon3DAppState state;
        runtime_state(context, &state);
        if (lardon3d_project_checkpoint_candidate_pair_generate_task(
                &state, task, &context->parameters) !=
            LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
            return lardon3d_task_fail(
                task, "Checkpoint Candidate Pair impossible.");
        }

#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
        bool test_pause_requested = test_pause_before_batch;
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
            /* 100 is reserved for an observed empty suffix, never merely for
             * publishing the membership that had the highest initial rank. */
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
    /* Private compute readers must reopen the same durable database selected
     * by the runtime. Project path identifies the asset root, not the database
     * basename, and publication remains owned by the Queue callback handle. */
    if (!lardon3d_project_db_copy_path(runtime->project_db,
                                      context->database_path)) {
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
    unsigned int desired_cpu_threads = CANDIDATE_PAIR_CPU_MAX_SAFE;
#ifdef LARDON3D_CANDIDATE_PAIR_TASK_TESTING
    const char *test_threads = getenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS");
    if (test_threads) {
        unsigned long parsed = strtoul(test_threads, NULL, 10);
        if (parsed >= 1 && parsed <= CANDIDATE_PAIR_CPU_MAX_SAFE) {
            desired_cpu_threads = (unsigned int)parsed;
        }
    }
    /* Test-only sequence shaping proves that a healthy Governor re-admits
     * parallel Candidate work after several durable boundaries. Production
     * keeps its canonical 1..64 resource estimate. */
    size_t test_maximum_batch = CANDIDATE_PAIR_MAXIMUM_BATCH;
    const char *test_batch = getenv("LARDON3D_TEST_CANDIDATE_PAIR_MAXIMUM_BATCH");
    if (test_batch) {
        unsigned long parsed = strtoul(test_batch, NULL, 10);
        if (parsed >= CANDIDATE_PAIR_MINIMUM_BATCH &&
            parsed <= CANDIDATE_PAIR_MAXIMUM_BATCH) {
            test_maximum_batch = (size_t)parsed;
        }
    }
#else
    const size_t test_maximum_batch = CANDIDATE_PAIR_MAXIMUM_BATCH;
#endif
    const Lardon3DResourceEstimate estimate = {
        .memory_fixed_bytes = CANDIDATE_PAIR_FIXED_MEMORY,
        .memory_bytes_per_item = CANDIDATE_PAIR_MEMORY_PER_ITEM,
        .minimum_batch_size = CANDIDATE_PAIR_MINIMUM_BATCH,
        .maximum_batch_size = test_maximum_batch,
        .desired_cpu_threads = desired_cpu_threads,
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
