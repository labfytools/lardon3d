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
#include <unistd.h>

#include <lardon3d/candidate_pair_gen.h>
#include <lardon3d/candidate_pair_task.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/project.h>
#include <lardon3d/sift_task.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>

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
    for (size_t i = 0; i < 2000000; i++) {
        if (lardon3d_task_queue_get(q, id, out) &&
            out->state == wanted) {
            return true;
        }
        sched_yield();
    }
    return false;
}

static bool run_test(void) {
    char root[] = "/tmp/lardon3d-candidate-pair-task-XXXXXX";
    CHECK(mkdtemp(root) &&
          setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);

    /* --- Créer 3 images distinctes --- */
    char source_a[PATH_MAX], source_b[PATH_MAX], source_c[PATH_MAX];
    CHECK(join_path(source_a, root, "a.pgm") && write_pgm(source_a, 1));
    CHECK(join_path(source_b, root, "b.pgm") && write_pgm(source_b, 2));
    CHECK(join_path(source_c, root, "c.pgm") && write_pgm(source_c, 3));

    Lardon3DAppState state;
    lardon3d_app_state_init(&state);
    CHECK(runtime(&state) && lardon3d_project_create(&state, "CPTask"));

    Lardon3DProjectDbScanSet scanset;
    CHECK(lardon3d_image_catalog_create_scanset(&state, "S", &scanset));

    Lardon3DProjectDbImage img_a, img_b, img_c;
    Lardon3DProjectDbImageAsset asset_a, asset_b, asset_c;
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
              LARDON3D_IMAGE_CATALOG_IMPORTED);

    /* Extraction feature ORB */
    Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
    uint64_t orb_a_id = 0, orb_b_id = 0, orb_c_id = 0;
    CHECK(lardon3d_project_enqueue_feature_extract(
              &state, img_a.image_id, &orb_params, &orb_a_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_b.image_id, &orb_params, &orb_b_id) &&
          lardon3d_project_enqueue_feature_extract(
              &state, img_c.image_id, &orb_params, &orb_c_id));
    Lardon3DTaskSnapshot snap;
    CHECK(wait_state(state.task_queue, orb_a_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_b_id, TASK_COMPLETED, &snap));
    CHECK(wait_state(state.task_queue, orb_c_id, TASK_COMPLETED, &snap));

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

    /* --- Test 1: Création task Candidate Pair --- */
    Lardon3DVisualIndexQueryOptions qopts = {
        .top_k = 16,
        .minimum_evidence_count = 10,
        .scanset_filter = LARDON3D_VISUAL_INDEX_ANY_SCANSET,
        .exclude_same_asset = false,
    };
    uint64_t cp_task_id = 0;
    CHECK(lardon3d_project_enqueue_candidate_pair_generate(
        &state, visual_index_id, &qopts, &cp_task_id));
    CHECK(cp_task_id != 0);
    CHECK(wait_state(state.task_queue, cp_task_id, TASK_COMPLETED, &snap));
    CHECK(snap.progress == 100);

    /* Vérification : des paires ont été générées */
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

    /* --- Test 3: Checkpoint / reprise --- */
    /* Pause après premier batch */
    CHECK(setenv("LARDON3D_TEST_CANDIDATE_PAIR_PAUSE_AFTER_BATCH", "1",
                 1) == 0 &&
          setenv("LARDON3D_TEST_CANDIDATE_PAIR_SKIP_FINISHED_CHECKPOINT",
                 "1", 1) == 0);

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
    CHECK(unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_PAUSE_AFTER_BATCH") == 0 &&
          unsetenv("LARDON3D_TEST_CANDIDATE_PAIR_SKIP_FINISHED_CHECKPOINT") ==
              0);

    lardon3d_app_state_init(&state);
    CHECK(runtime(&state) && lardon3d_project_open(&state, "CPTask"));

    Lardon3DProjectRecoverySummary summary;
    CHECK(lardon3d_project_last_recovery_summary(&state, &summary) &&
          summary.resumed == 1);
    CHECK(wait_state(state.task_queue, cp_task3_id, TASK_COMPLETED, &snap) &&
          snap.progress == 100);

    /* --- Test 4: Reconstruction --- */
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

    /* --- Test 5: Ordre canonique vérifié --- */
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
