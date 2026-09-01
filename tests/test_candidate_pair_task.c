#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include <lardon3d/candidate_pair_gen.h>
#include <lardon3d/candidate_pair_task.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/project.h>
#include <lardon3d/sift_task.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>

#include "src/candidate_pair_gen_internal.h"
#include "resource_snapshot_test_utils.h"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            fprintf(stderr, "Echec ligne %d: %s\n", __LINE__, #x);               \
            return false;                                                         \
        }                                                                         \
    } while (0)

static bool join_path(char out[PATH_MAX], const char *a, const char *b) {
    int n = snprintf(out, PATH_MAX, "%s/%s", a, b);
    return n > 0 && (size_t)n < PATH_MAX;
}

static bool remove_tree(const char *p) {
    struct stat s;
    if (lstat(p, &s) != 0) return errno == ENOENT;
    if (!S_ISDIR(s.st_mode)) return unlink(p) == 0;
    DIR *d = opendir(p);
    if (!d) return false;
    bool ok = true;
    for (struct dirent *e = readdir(d); e; e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[PATH_MAX];
        if (!join_path(c, p, e->d_name) || !remove_tree(c)) ok = false;
    }
    if (closedir(d) || rmdir(p)) ok = false;
    return ok;
}

static bool write_pgm(const char *p, unsigned seed) {
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    fprintf(f, "P5\n192 192\n255\n");
    for (unsigned y = 0; y < 192; y++) {
        for (unsigned x = 0; x < 192; x++) {
            unsigned base = ((((x / 12) ^ (y / 12)) & 1U) != 0U) ? 205U : 35U;
            unsigned texture =
                ((x * 17U + y * 29U + seed * 31U + (x * y) % 37U) % 43U);
            int dx = (int)x - 96, dy = (int)y - 96;
            unsigned ring =
                (dx * dx + dy * dy > 32 * 32 &&
                 dx * dx + dy * dy < 42 * 42) ? 45U : 0U;
            unsigned char v = (unsigned char)(base + texture + ring > 255U
                                                  ? 255U
                                                  : base + texture + ring);
            if (fwrite(&v, 1, 1, f) != 1) {
                fclose(f);
                return false;
            }
        }
    }
    return fclose(f) == 0;
}

static bool runtime(Lardon3DAppState *s) {
    s->hardware_profile = (Lardon3DHardwareProfile){
        .logical_cpu_count = 64,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test"};
    Lardon3DResourcePolicy p = {.maximum_cpu_load_ratio = 1,
                                .maximum_io_pressure_avg10 = 100,
                                .io_slot_capacity = 2};
    s->resource_governor =
        lardon3d_resource_governor_create(&s->hardware_profile, &p);
    s->task_queue = s->resource_governor
                        ? lardon3d_task_queue_create(s->resource_governor, 4)
                        : NULL;
    return s->task_queue != NULL;
}

static bool wait_state(Lardon3DTaskQueue *q, uint64_t id,
                       Lardon3DTaskState wanted, Lardon3DTaskSnapshot *out) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return false;
    deadline.tv_sec += 5;
    for (;;) {
        if (lardon3d_task_queue_get(q, id, out) &&
            out->state == wanted) {
            return true;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec)) {
            return false;
        }
        sched_yield();
    }
}

static bool reset_candidate_pairs(Lardon3DProjectDb *project_database) {
    char database_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    if (!lardon3d_project_db_copy_path(project_database, database_path)) return false;
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    (void)sqlite3_busy_timeout(database, 5000);
    char *error = NULL;
    bool ok = sqlite3_exec(
        database,
        "BEGIN; DELETE FROM candidate_pairs; "
        "DELETE FROM sqlite_sequence WHERE name='candidate_pairs'; COMMIT;",
        NULL, NULL, &error) == SQLITE_OK;
    if (!ok) fprintf(stderr, "reset Candidate Pair: %s\n", error ? error : "unknown");
    sqlite3_free(error);
    sqlite3_close(database);
    return ok;
}

static bool load_pairs(Lardon3DProjectDb *database,
                       Lardon3DProjectDbCandidatePair pairs[32], size_t *count) {
    return lardon3d_project_db_list_candidate_pairs(database, 0, pairs, 32, count) ==
           LARDON3D_PROJECT_DB_OK;
}

static bool same_pair_order(const Lardon3DProjectDbCandidatePair *a, size_t a_count,
                            const Lardon3DProjectDbCandidatePair *b, size_t b_count) {
    if (a_count != b_count) return false;
    for (size_t i = 0; i < a_count; ++i) {
        if (a[i].image_id_a != b[i].image_id_a || a[i].image_id_b != b[i].image_id_b) {
            return false;
        }
    }
    return true;
}

static bool same_computation(const Lardon3DCandidatePairComputation *left,
                             const Lardon3DCandidatePairComputation *right) {
    if (left->source_feature_set_id != right->source_feature_set_id ||
        left->queried_count != right->queried_count ||
        left->proposal_count != right->proposal_count) {
        return false;
    }
    for (size_t i = 0; i < left->proposal_count; ++i) {
        if (left->proposals[i].image_id_a != right->proposals[i].image_id_a ||
            left->proposals[i].image_id_b != right->proposals[i].image_id_b) {
            return false;
        }
    }
    return true;
}

static bool run_portable_compute_contracts(
    Lardon3DAppState *state, uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *options,
    const uint64_t fixture_source_ids[3]) {
    enum { SOURCE_COUNT = 64 };
    uint64_t sources[SOURCE_COUNT];
    Lardon3DCandidatePairComputation baseline[SOURCE_COUNT];
    Lardon3DCandidatePairComputation parallel[SOURCE_COUNT];
    Lardon3DVisualIndexResult baseline_results[SOURCE_COUNT];
    Lardon3DVisualIndexResult parallel_results[SOURCE_COUNT];
    for (size_t i = 0; i < SOURCE_COUNT; ++i) {
        sources[i] = fixture_source_ids[i % 3];
    }

    lardon3d_candidate_pair_task_test_fail_thread_create_after(SIZE_MAX);
    lardon3d_candidate_pair_task_test_reset_parallel_counters();
    CHECK(lardon3d_candidate_pair_task_test_compute_window(
              state->project_path, state->project_db, visual_index_id, options,
              sources, SOURCE_COUNT, 1, baseline, baseline_results) &&
          lardon3d_candidate_pair_task_test_started_participants() == 1 &&
          lardon3d_candidate_pair_task_test_computed_work_items() == SOURCE_COUNT &&
          lardon3d_candidate_pair_task_test_active_private_databases() == 0);

    const unsigned int portable_counts[] = {16, 32, 64};
    for (size_t run = 0;
         run < sizeof(portable_counts) / sizeof(portable_counts[0]); ++run) {
        lardon3d_candidate_pair_task_test_reset_parallel_counters();
        CHECK(lardon3d_candidate_pair_task_test_compute_window(
                  state->project_path, state->project_db, visual_index_id,
                  options, sources, SOURCE_COUNT, portable_counts[run],
                  parallel, parallel_results) &&
              lardon3d_candidate_pair_task_test_started_participants() ==
                  portable_counts[run] &&
              lardon3d_candidate_pair_task_test_computed_work_items() ==
                  SOURCE_COUNT &&
              lardon3d_candidate_pair_task_test_active_private_databases() == 0);
        for (size_t i = 0; i < SOURCE_COUNT; ++i) {
            CHECK(baseline_results[i] == LARDON3D_VISUAL_INDEX_OK &&
                  parallel_results[i] == baseline_results[i] &&
                  same_computation(&parallel[i], &baseline[i]));
        }
    }

    /* Fail after two children have started. The Queue-owner participant and
     * those children may compute, but every child is joined and every one of
     * the 16 preopened private DB handles is closed before failure returns. */
    lardon3d_candidate_pair_task_test_reset_parallel_counters();
    lardon3d_candidate_pair_task_test_fail_thread_create_after(2);
    CHECK(!lardon3d_candidate_pair_task_test_compute_window(
              state->project_path, state->project_db, visual_index_id, options,
              sources, SOURCE_COUNT, 16, parallel, parallel_results) &&
          lardon3d_candidate_pair_task_test_started_participants() == 3 &&
          lardon3d_candidate_pair_task_test_active_private_databases() == 0);
    lardon3d_candidate_pair_task_test_fail_thread_create_after(SIZE_MAX);
    return true;
}

static bool wait_saved_cursor(Lardon3DProjectDb *database, uint64_t task_id,
                              uint64_t expected) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return false;
    deadline.tv_sec += 5;
    for (;;) {
        Lardon3DProjectDbCandidatePairGenerateTask saved;
        if (lardon3d_project_db_load_candidate_pair_generate_task(
                database, task_id, &saved) == LARDON3D_PROJECT_DB_OK &&
            saved.after_feature_set_id == expected) {
            return true;
        }
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            return false;
        }
        sched_yield();
    }
}

static bool same_estimate(const Lardon3DResourceEstimate *left,
                          const Lardon3DResourceEstimate *right) {
    return left->memory_fixed_bytes == right->memory_fixed_bytes &&
           left->gpu_memory_fixed_bytes == right->gpu_memory_fixed_bytes &&
           left->memory_bytes_per_item == right->memory_bytes_per_item &&
           left->gpu_memory_bytes_per_item == right->gpu_memory_bytes_per_item &&
           left->minimum_batch_size == right->minimum_batch_size &&
           left->maximum_batch_size == right->maximum_batch_size &&
           left->desired_cpu_threads == right->desired_cpu_threads &&
           left->desired_gpu_slots == right->desired_gpu_slots &&
           left->desired_io_slots == right->desired_io_slots &&
           left->task_class == right->task_class;
}

static bool run_test(void) {
    char root[] = "/tmp/lardon3d-candidate-pair-task-XXXXXX";
    CHECK(mkdtemp(root) &&
          setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);

    /* Six FeatureSets let the test cross three real Task/Queue/Governor
     * sequence boundaries when its test-only admitted maximum is two. */
    char source_a[PATH_MAX], source_b[PATH_MAX], source_c[PATH_MAX];
    char source_d[PATH_MAX], source_e[PATH_MAX], source_f[PATH_MAX];
    CHECK(join_path(source_a, root, "a.pgm") && write_pgm(source_a, 1));
    CHECK(join_path(source_b, root, "b.pgm") && write_pgm(source_b, 2));
    CHECK(join_path(source_c, root, "c.pgm") && write_pgm(source_c, 3));
    CHECK(join_path(source_d, root, "d.pgm") && write_pgm(source_d, 4));
    CHECK(join_path(source_e, root, "e.pgm") && write_pgm(source_e, 5));
    CHECK(join_path(source_f, root, "f.pgm") && write_pgm(source_f, 6));

    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    CHECK(runtime(&state) && lardon3d_project_create(&state, "CPTask"));

    Lardon3DProjectDbScanSet scanset;
    CHECK(lardon3d_image_catalog_create_scanset(&state, "S", &scanset));

    Lardon3DProjectDbImage img_a, img_b, img_c, img_d, img_e, img_f;
    Lardon3DProjectDbImageAsset asset_a, asset_b, asset_c, asset_d, asset_e, asset_f;
    CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id,
                                             source_a, 0, &img_a,
                                             &asset_a) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED &&
          lardon3d_image_catalog_import_file(&state, scanset.scanset_id,
                                             source_b, 0, &img_b,
                                             &asset_b) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED &&
          lardon3d_image_catalog_import_file(&state, scanset.scanset_id,
                                             source_c, 0, &img_c,
                                             &asset_c) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED &&
          lardon3d_image_catalog_import_file(&state, scanset.scanset_id,
                                             source_d, 0, &img_d,
                                             &asset_d) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED &&
          lardon3d_image_catalog_import_file(&state, scanset.scanset_id,
                                             source_e, 0, &img_e,
                                             &asset_e) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED &&
          lardon3d_image_catalog_import_file(&state, scanset.scanset_id,
                                             source_f, 0, &img_f,
                                             &asset_f) ==
              LARDON3D_IMAGE_CATALOG_IMPORTED);

    /* Extraction feature ORB */
    Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
    uint64_t orb_a_id = 0, orb_b_id = 0, orb_c_id = 0, orb_d_id = 0, orb_e_id = 0, orb_f_id = 0;
    CHECK(lardon3d_project_enqueue_feature_extract(
              &state, img_a.image_id, &orb_params, &orb_a_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_b.image_id, &orb_params, &orb_b_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_c.image_id, &orb_params, &orb_c_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_d.image_id, &orb_params, &orb_d_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_e.image_id, &orb_params, &orb_e_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_f.image_id, &orb_params, &orb_f_id));
    Lardon3DTaskSnapshot snap;
    CHECK(wait_state(state.task_queue, orb_a_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_b_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_c_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_d_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_e_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_f_id, TASK_COMPLETED, &snap));

    unsigned char fp[32];
    lardon3d_feature_extractor_parameter_fingerprint(&orb_params, fp);
    Lardon3DProjectDbFeatureSet fs_a, fs_b, fs_c;
    CHECK(lardon3d_project_db_find_feature_set(
              state.project_db, img_a.image_id,
              LARDON3D_FEATURE_EXTRACTOR_KIND, 1, fp, &fs_a) ==
              LARDON3D_PROJECT_DB_OK &&
          lardon3d_project_db_find_feature_set(
              state.project_db, img_b.image_id,
              LARDON3D_FEATURE_EXTRACTOR_KIND, 1, fp, &fs_b) ==
              LARDON3D_PROJECT_DB_OK &&
          lardon3d_project_db_find_feature_set(
              state.project_db, img_c.image_id,
              LARDON3D_FEATURE_EXTRACTOR_KIND, 1, fp, &fs_c) ==
              LARDON3D_PROJECT_DB_OK);

    /* Visual Index */
    Lardon3DVisualIndexConfiguration index_cfg = {1, 256, 128};
    uint64_t visual_index_id = 0;
    CHECK(lardon3d_visual_index_create(state.project_db, &fs_a, &index_cfg,
                                       &visual_index_id) ==
          LARDON3D_VISUAL_INDEX_OK);
    uint64_t vi_task_id = 0;
    CHECK(lardon3d_project_enqueue_visual_index_update(
              &state, visual_index_id, &vi_task_id));
    CHECK(wait_state(state.task_queue, vi_task_id, TASK_COMPLETED, &snap));

    /* Candidate workers must follow the runtime-selected durable DB filename;
     * project_path alone intentionally cannot identify that database. */
    char project_path[PATH_MAX], default_database_path[PATH_MAX];
    char selected_database_path[PATH_MAX];
    CHECK(snprintf(project_path, sizeof(project_path), "%s", state.project_path) > 0 &&
          join_path(default_database_path, project_path, "project.db") &&
          join_path(selected_database_path, project_path, "selected.lardon3d"));
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    CHECK(rename(default_database_path, selected_database_path) == 0);
    char database_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
    CHECK(lardon3d_project_db_open(selected_database_path, &state.project_db,
                                  database_error) == LARDON3D_PROJECT_DB_OK);
    state.project_loaded = true;
    CHECK(snprintf(state.project_path, sizeof(state.project_path), "%s",
                   project_path) > 0);
    state.task_queue = lardon3d_task_queue_create(state.resource_governor, 4);
    CHECK(state.task_queue != NULL);

    /* --- Test 1: déterminisme exact jusqu'au plafond portable de 64 --- */
    Lardon3DVisualIndexQueryOptions qopts = {
        .top_k = 16,
        .minimum_evidence_count = 10,
        .scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET,
        .exclude_same_asset = false,
    };
    const uint64_t portable_fixture_sources[3] = {
        fs_a.feature_set_id, fs_b.feature_set_id, fs_c.feature_set_id};
    CHECK(run_portable_compute_contracts(&state, visual_index_id, &qopts,
                                         portable_fixture_sources));

    /* A healthy Governor admits two bounded sources per sequence. The six
     * durable memberships create three productive windows: initial admission
     * plus two productive re-admissions after sequence_break. Completion then
     * re-admits once more only to observe the empty suffix. This proves that
     * parallel fan-out is not a one-window startup effect. The production
     * estimate remains 1..64; this test-only cap exercises the boundary. */
    CHECK(reset_candidate_pairs(state.project_db) &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS", "2", 1) == 0 &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_MAXIMUM_BATCH", "2", 1) == 0);
    lardon3d_candidate_pair_task_test_reset_parallel_counters();
    uint64_t multi_sequence_task_id = 0;
    CHECK(lardon3d_project_enqueue_candidate_pair_generate(
              &state, visual_index_id, &qopts, &multi_sequence_task_id) &&
          wait_state(state.task_queue, multi_sequence_task_id, TASK_COMPLETED, &snap) &&
          snap.progress == 100 &&
          /* A fresh kind history begins at CPU1. This small fixture proves
           * every durable item executes; synthetic throughput is not allowed
           * to assume that CPU2 was already demonstrated beneficial. */
          lardon3d_candidate_pair_task_test_started_participants() >= 1 &&
          lardon3d_candidate_pair_task_test_computed_work_items() >= 6);
    Lardon3DProjectDbTask multi_sequence_task;
    CHECK(lardon3d_project_db_load_task(state.project_db, multi_sequence_task_id,
                                        &multi_sequence_task) == LARDON3D_PROJECT_DB_OK &&
          multi_sequence_task.sequence_count >= 3);
    Lardon3DProjectDbCandidatePairGenerateTask multi_sequence_cursor;
    CHECK(lardon3d_project_db_load_candidate_pair_generate_task(
              state.project_db, multi_sequence_task_id, &multi_sequence_cursor) ==
              LARDON3D_PROJECT_DB_OK &&
          multi_sequence_cursor.after_feature_set_id != 0);
    CHECK(unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_MAXIMUM_BATCH") == 0 &&
          unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS") == 0);

    /* CPU admission does not authorize extra work. With 64 CPUs requested but
     * an admitted one-item batch, each of the six sequence breaks
     * has exactly one independent source and therefore one useful worker. */
    CHECK(reset_candidate_pairs(state.project_db) &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS", "64", 1) == 0 &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_MAXIMUM_BATCH", "1", 1) == 0);
    lardon3d_candidate_pair_task_test_reset_parallel_counters();
    uint64_t pressure_task_id = 0;
    CHECK(lardon3d_project_enqueue_candidate_pair_generate(
              &state, visual_index_id, &qopts, &pressure_task_id) &&
          wait_state(state.task_queue, pressure_task_id, TASK_COMPLETED, &snap) &&
          lardon3d_candidate_pair_task_test_started_participants() == 6 &&
          lardon3d_candidate_pair_task_test_computed_work_items() == 6);
    Lardon3DProjectDbTask pressure_task;
    CHECK(lardon3d_project_db_load_task(state.project_db, pressure_task_id,
                                        &pressure_task) == LARDON3D_PROJECT_DB_OK &&
          pressure_task.sequence_count >= 6);
    CHECK(unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_MAXIMUM_BATCH") == 0 &&
          unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS") == 0);

    const char *thread_counts[] = {"1", "2", "4", "16", "32", "64"};
    uint64_t cp_task_id = 0;
    Lardon3DProjectDbCandidatePair baseline[32];
    size_t baseline_count = 0;
    for (size_t run = 0;
         run < sizeof(thread_counts) / sizeof(thread_counts[0]); ++run) {
        CHECK(reset_candidate_pairs(state.project_db) &&
              setenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS", thread_counts[run], 1) == 0);
        uint64_t current_task_id = 0;
        CHECK(lardon3d_project_enqueue_candidate_pair_generate(
            &state, visual_index_id, &qopts, &current_task_id));
        if (run == 0) cp_task_id = current_task_id;
        CHECK(wait_state(state.task_queue, current_task_id, TASK_COMPLETED, &snap) &&
              snap.progress == 100);
        Lardon3DProjectDbCandidatePair current[32];
        size_t current_count = 0;
        CHECK(load_pairs(state.project_db, current, &current_count) && current_count > 0);
        if (run == 0) {
            memcpy(baseline, current, current_count * sizeof(*current));
            baseline_count = current_count;
        } else {
            CHECK(same_pair_order(baseline, baseline_count, current, current_count));
        }
    }
    CHECK(unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS") == 0);

    Lardon3DProjectDbCandidatePair pairs[16];
    size_t pair_count = 0;
    CHECK(lardon3d_project_db_list_candidate_pairs(
              state.project_db, 0, pairs, 16, &pair_count) ==
              LARDON3D_PROJECT_DB_OK &&
          pair_count > 0);

    /* --- Test 2: Idempotence --- */
    uint64_t cp_task2_id = 0;
    CHECK(lardon3d_project_enqueue_candidate_pair_generate(
        &state, visual_index_id, &qopts, &cp_task2_id));
    CHECK(cp_task2_id != cp_task_id);
    CHECK(wait_state(state.task_queue, cp_task2_id, TASK_COMPLETED, &snap));
    CHECK(snap.progress == 100);

    size_t pair_count2 = 0;
    CHECK(lardon3d_project_db_list_candidate_pairs(
              state.project_db, 0, pairs, 16, &pair_count2) ==
              LARDON3D_PROJECT_DB_OK &&
          pair_count2 == pair_count);

    /* --- Test 3: échec first/middle/last et frontière contiguë --- */
    uint64_t ordered_sources[] = {fs_a.feature_set_id, fs_b.feature_set_id,
                                  fs_c.feature_set_id};
    for (size_t failed = 0; failed < 3; ++failed) {
        CHECK(reset_candidate_pairs(state.project_db));
        for (size_t prefix = 0; prefix < failed; ++prefix) {
            Lardon3DCandidatePairGenStats prefix_stats;
            CHECK(lardon3d_candidate_pair_generate(
                      state.project_path, state.project_db, visual_index_id,
                      ordered_sources[prefix], &qopts, &prefix_stats) ==
                  LARDON3D_VISUAL_INDEX_OK);
        }
        Lardon3DProjectDbCandidatePair expected[32];
        size_t expected_count = 0;
        CHECK(load_pairs(state.project_db, expected, &expected_count) &&
              reset_candidate_pairs(state.project_db));

        char failed_id[32];
        (void)snprintf(failed_id, sizeof(failed_id), "%lu",
                       (unsigned long)ordered_sources[failed]);
        CHECK(setenv("LARDON3D_TEST_CANDIDATE_PAIR_FAIL_SOURCE_ID", failed_id, 1) == 0 &&
              setenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS", "4", 1) == 0);
        uint64_t failed_task_id = 0;
        CHECK(lardon3d_project_enqueue_candidate_pair_generate(
                  &state, visual_index_id, &qopts, &failed_task_id) &&
              wait_state(state.task_queue, failed_task_id, TASK_FAILED, &snap));
        uint64_t expected_cursor = failed == 0 ? 0 : ordered_sources[failed - 1];
        CHECK(wait_saved_cursor(state.project_db, failed_task_id, expected_cursor));
        Lardon3DProjectDbCandidatePair actual[32];
        size_t actual_count = 0;
        CHECK(load_pairs(state.project_db, actual, &actual_count) &&
              same_pair_order(expected, expected_count, actual, actual_count));
    }
    CHECK(unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_FAIL_SOURCE_ID") == 0 &&
          unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS") == 0 &&
          reset_candidate_pairs(state.project_db));

    /* --- Test 4: Checkpoint / reprise --- */
    /* Pause before the first window so recovery has enough independent
     * sources to prove that its production callback actually fans out. */
    CHECK(setenv("LARDON3D_TEST_CANDIDATE_PAIR_PAUSE_BEFORE_BATCH", "1",
                 1) == 0 &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_SKIP_FINISHED_CHECKPOINT",
                 "1", 1) == 0 &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS", "1", 1) == 0);

    Lardon3DVisualIndexQueryOptions qopts_b = {
        .top_k = 8, .minimum_evidence_count = 0,
        .scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET,
        .exclude_same_asset = false,
    };
    uint64_t cp_task3_id = 0;
    CHECK(lardon3d_project_enqueue_candidate_pair_generate(
        &state, visual_index_id, &qopts_b, &cp_task3_id));
    CHECK(wait_state(state.task_queue, cp_task3_id, TASK_PAUSED, &snap));

    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    state.resource_governor = NULL;
    CHECK(rename(selected_database_path, default_database_path) == 0);
    CHECK(unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_PAUSE_BEFORE_BATCH") == 0 &&
          unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_SKIP_FINISHED_CHECKPOINT") ==
              0 &&
          unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_THREADS") == 0);

    /* Persist the real Candidate v1 resource shape. A current 256 KiB task
     * forced to one thread is a near miss, not a historical checkpoint. */
    char candidate_checkpoint_path[PATH_MAX];
    CHECK(snprintf(candidate_checkpoint_path, sizeof(candidate_checkpoint_path),
                   "%s/.lardon3d/checkpoints/%lu.chk", project_path,
                   (unsigned long)cp_task3_id) > 0);
    Lardon3DTaskDurableSnapshot legacy_snapshot;
    uint32_t checkpoint_version = 0;
    CHECK(lardon3d_task_checkpoint_load(candidate_checkpoint_path, &legacy_snapshot,
                                        &checkpoint_version) ==
              LARDON3D_TASK_CHECKPOINT_OK &&
          checkpoint_version == LARDON3D_TASK_CHECKPOINT_VERSION);
    legacy_snapshot.estimate = (Lardon3DResourceEstimate){
        .memory_fixed_bytes = 128 * 1024,
        .gpu_memory_fixed_bytes = 0,
        .memory_bytes_per_item = 64 * 1024,
        .gpu_memory_bytes_per_item = 0,
        .minimum_batch_size = 1,
        .maximum_batch_size = 64,
        .desired_cpu_threads = 1,
        .desired_gpu_slots = 0,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    CHECK(lardon3d_task_checkpoint_save(candidate_checkpoint_path, &legacy_snapshot) ==
          LARDON3D_TASK_CHECKPOINT_OK);

    /* Candidate v1 recovery recognizes the exact historical serial estimate,
     * then normalizes it privately before Queue admission. This operational
     * estimate is not checkpointed, so Task identity and the typed cursor stay
     * durable and unchanged. */
    lardon3d_app_state_init(&state);
    CHECK(runtime(&state) &&
          lardon3d_project_db_open(default_database_path, &state.project_db,
                                   database_error) == LARDON3D_PROJECT_DB_OK);
    state.project_loaded = true;
    CHECK(snprintf(state.project_path, sizeof(state.project_path), "%s",
                   project_path) > 0);
    Lardon3DProjectRecoveryEntry recovery_entries[8];
    size_t recovery_count = 0;
    CHECK(lardon3d_project_list_recoverable(
              &state, lardon3d_task_kind_registry_production(), 0,
              recovery_entries, 8, &recovery_count) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectRecoveryEntry *candidate_recovery = NULL;
    for (size_t i = 0; i < recovery_count; ++i) {
        if (recovery_entries[i].task_id == cp_task3_id) {
            candidate_recovery = &recovery_entries[i];
        }
    }
    CHECK(candidate_recovery &&
          same_estimate(&candidate_recovery->snapshot.estimate,
                        &legacy_snapshot.estimate));
    Lardon3DProjectDbCandidatePairGenerateTask cursor_before;
    CHECK(lardon3d_project_db_load_candidate_pair_generate_task(
              state.project_db, cp_task3_id, &cursor_before) ==
          LARDON3D_PROJECT_DB_OK);

    Lardon3DTaskReconstructionContext reconstruction = {
        .project_path = state.project_path,
        .project_db = state.project_db,
        .resource_governor = state.resource_governor,
        .orb_vulkan_backend = state.orb_vulkan_backend,
    };
    Lardon3DTask *recovered = NULL;

    /* The immediately preceding parallel form used fixed256/per-item64KiB
     * with CPU12. It is independent from the oldest fixed128/CPU1 signature
     * below and both normalize to the portable, fully accounted estimate. */
    Lardon3DTaskDurableSnapshot historical_parallel =
        candidate_recovery->snapshot;
    historical_parallel.estimate.memory_fixed_bytes = 256 * 1024;
    historical_parallel.estimate.memory_bytes_per_item = 64 * 1024;
    historical_parallel.estimate.desired_cpu_threads = 12;
    Lardon3DTask *historical_parallel_task = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              candidate_recovery->task_kind,
              candidate_recovery->task_kind_version, &historical_parallel,
              &reconstruction, &historical_parallel_task) ==
              LARDON3D_TASK_KIND_OK &&
          historical_parallel_task);
    Lardon3DResourceEstimate historical_parallel_effective;
    CHECK(lardon3d_task_resource_estimate(
              historical_parallel_task, &historical_parallel_effective) &&
          historical_parallel_effective.memory_bytes_per_item ==
              8 * 1024 * 1024 &&
          historical_parallel_effective.desired_cpu_threads == 64);
    lardon3d_task_destroy(historical_parallel_task);

    /* Every field belongs to the historical signature. Neighboring estimates
     * are malformed rather than evidence for legacy operational policy. */
    Lardon3DTaskDurableSnapshot near_snapshots[4];
    for (size_t i = 0; i < 4; ++i) near_snapshots[i] = legacy_snapshot;
    near_snapshots[0].estimate.memory_fixed_bytes = 256 * 1024;
    near_snapshots[1].estimate.memory_bytes_per_item = 64 * 1024 + 1;
    near_snapshots[2].estimate.desired_cpu_threads = 2;
    near_snapshots[3].estimate.desired_io_slots = 2;
    for (size_t i = 0; i < 4; ++i) {
        Lardon3DTask *near_task = NULL;
        CHECK(lardon3d_task_kind_registry_restore(
                  lardon3d_task_kind_registry_production(),
                  candidate_recovery->task_kind,
                  candidate_recovery->task_kind_version, &near_snapshots[i],
                  &reconstruction, &near_task) ==
                  LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED &&
              near_task == NULL);
    }
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              candidate_recovery->task_kind,
              candidate_recovery->task_kind_version,
              &candidate_recovery->snapshot, &reconstruction, &recovered) ==
              LARDON3D_TASK_KIND_OK &&
          recovered && lardon3d_task_id(recovered) == cp_task3_id);
    Lardon3DResourceEstimate recovered_estimate;
    const Lardon3DResourceEstimate current_estimate = {
        .memory_fixed_bytes = 256 * 1024,
        .gpu_memory_fixed_bytes = 0,
        .memory_bytes_per_item = 8 * 1024 * 1024,
        .gpu_memory_bytes_per_item = 0,
        .minimum_batch_size = 1,
        .maximum_batch_size = 64,
        .desired_cpu_threads = 64,
        .desired_gpu_slots = 0,
        .desired_io_slots = 1,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    CHECK(lardon3d_task_resource_estimate(recovered, &recovered_estimate) &&
          same_estimate(&recovered_estimate, &current_estimate));

    Lardon3DResourceSnapshot resources = {
        .memory_available_bytes = UINT64_MAX,
        .cpu_load_1m = 0.0,
    };
    Lardon3DResourceDecision decision;
    Lardon3DResourceReservation *reservation = NULL;
    CHECK(lardon3d_test_resource_snapshot_make_fresh(&resources) &&
          lardon3d_resource_governor_reserve(
              state.resource_governor, &resources, &recovered_estimate,
              &decision, &reservation) &&
          decision.cpu_threads >= 1 && decision.cpu_threads <= 64 &&
          decision.batch_size <= 64 && reservation);
    lardon3d_resource_governor_release(state.resource_governor, reservation);

    Lardon3DHardwareProfile reduced_profile = {
        .logical_cpu_count = 4,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_MAX,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy reduced_policy = {
        .system_cpu_reserve = 2,
        .maximum_cpu_load_ratio = 1,
        .maximum_io_pressure_avg10 = 100,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *reduced_governor =
        lardon3d_resource_governor_create(&reduced_profile, &reduced_policy);
    reservation = NULL;
    CHECK(reduced_governor &&
          lardon3d_resource_governor_reserve(
              reduced_governor, &resources, &recovered_estimate,
              &decision, &reservation) &&
          decision.kind == LARDON3D_RESOURCE_REDUCE_BATCH &&
          decision.cpu_threads == 2 && decision.batch_size >= 1 &&
          decision.batch_size <= 64 && reservation);
    lardon3d_resource_governor_release(reduced_governor, reservation);
    lardon3d_resource_governor_destroy(reduced_governor);
    lardon3d_task_destroy(recovered);

    recovery_count = 0;
    CHECK(lardon3d_project_list_recoverable(
              &state, lardon3d_task_kind_registry_production(), 0,
              recovery_entries, 8, &recovery_count) == LARDON3D_PROJECT_DB_OK);
    candidate_recovery = NULL;
    for (size_t i = 0; i < recovery_count; ++i) {
        if (recovery_entries[i].task_id == cp_task3_id) {
            candidate_recovery = &recovery_entries[i];
        }
    }
    Lardon3DProjectDbCandidatePairGenerateTask cursor_after;
    CHECK(candidate_recovery &&
          candidate_recovery->snapshot.id == cp_task3_id &&
          same_estimate(&candidate_recovery->snapshot.estimate,
                        &legacy_snapshot.estimate) &&
          lardon3d_project_db_load_candidate_pair_generate_task(
              state.project_db, cp_task3_id, &cursor_after) ==
              LARDON3D_PROJECT_DB_OK &&
          cursor_after.after_feature_set_id ==
              cursor_before.after_feature_set_id);

    /* A pre-terminal crash repeats exact ephemeral normalization. Recovery
     * does not publish an estimate-only .chk or .chk.next with the same Task
     * summary, so the durable historical snapshot remains unambiguous. */
    char candidate_staged_path[PATH_MAX];
    CHECK(snprintf(candidate_staged_path, sizeof(candidate_staged_path),
                   "%s.next", candidate_checkpoint_path) > 0 &&
          access(candidate_staged_path, F_OK) != 0 && errno == ENOENT);
    Lardon3DTask *current_again = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              candidate_recovery->task_kind,
              candidate_recovery->task_kind_version,
              &candidate_recovery->snapshot, &reconstruction,
              &current_again) == LARDON3D_TASK_KIND_OK &&
          current_again);
    Lardon3DResourceEstimate current_again_estimate;
    CHECK(lardon3d_task_resource_estimate(current_again,
                                          &current_again_estimate) &&
          same_estimate(&current_again_estimate, &current_estimate));
    lardon3d_task_destroy(current_again);
    Lardon3DTaskDurableSnapshot still_durable;
    CHECK(lardon3d_task_checkpoint_load(candidate_checkpoint_path,
                                        &still_durable, NULL) ==
              LARDON3D_TASK_CHECKPOINT_OK &&
          same_estimate(&still_durable.estimate, &legacy_snapshot.estimate) &&
          access(candidate_staged_path, F_OK) != 0 && errno == ENOENT);
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    state.resource_governor = NULL;

    /* This project_open follows the real runner's durable recovery route.
     * The paused task has not dispatched this window, so the counters below
     * prove participant threads performed recovered production work. */
    lardon3d_candidate_pair_task_test_reset_parallel_counters();
    lardon3d_app_state_init(&state);
    CHECK(runtime(&state) && lardon3d_project_open(&state, "CPTask"));

    Lardon3DProjectRecoverySummary summary;
    CHECK(lardon3d_project_last_recovery_summary(&state, &summary) &&
          summary.resumed == 1);
    CHECK(wait_state(state.task_queue, cp_task3_id, TASK_COMPLETED, &snap) &&
          snap.progress == 100 &&
          lardon3d_candidate_pair_task_test_started_participants() > 1 &&
          lardon3d_candidate_pair_task_test_computed_work_items() > 1);

    /* --- Test 5: Reconstruction --- */
    /* Attendre que la tâche se termine puis ré-ouvrir sans reprise */
    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    state.resource_governor = NULL;

    lardon3d_app_state_init(&state);
    CHECK(runtime(&state) && lardon3d_project_open(&state, "CPTask"));
    CHECK(lardon3d_project_last_recovery_summary(&state, &summary) &&
          summary.resumed == 0);

    /* Vérifier que la DB task est sauvegardée */
    Lardon3DProjectDbCandidatePairGenerateTask saved;
    CHECK(lardon3d_project_db_load_candidate_pair_generate_task(
              state.project_db, cp_task_id, &saved) ==
              LARDON3D_PROJECT_DB_OK &&
          saved.top_k == 16);

    /* --- Test 6: Ordre canonique vérifié --- */
    size_t final_count = 0;
    Lardon3DProjectDbCandidatePair final_pairs[32];
    CHECK(lardon3d_project_db_list_candidate_pairs(
              state.project_db, 0, final_pairs, 32, &final_count) ==
              LARDON3D_PROJECT_DB_OK &&
          final_count > 0);

    for (size_t i = 0; i < final_count; ++i) {
        CHECK(final_pairs[i].image_id_a < final_pairs[i].image_id_b);
    }

    lardon3d_task_queue_destroy(state.task_queue);
    state.task_queue = NULL;
    lardon3d_project_close(&state);
    lardon3d_resource_governor_destroy(state.resource_governor);
    CHECK(unlink(source_a) == 0 && unlink(source_b) == 0 &&
          unlink(source_c) == 0 && remove_tree(root));
    return true;
}

int main(void) { return run_test() ? 0 : 1; }
