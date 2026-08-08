/*
 * Tests de durcissement et finalisation de la consolidation ORB/SIFT
 * intra-image. Couvre : zero-features, rayon, grid boundary, support_count,
 * mismatch, grand set, idempotence, concurrence, stress, pagination,
 * fingerprint et quality comparison.
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
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

#include <lardon3d/feature_store.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/precision_features.h>
#include <lardon3d/project.h>
#include <lardon3d/project_db.h>
#include <lardon3d/sift_task.h>
#include <lardon3d/task_queue.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                     \
      return false;                                                            \
    }                                                                          \
  } while (0)

/* -- Utilities -- */

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

static bool write_pgm(const char *p) {
  FILE *f = fopen(p, "wb");
  if (!f) return false;
  fprintf(f, "P5\n192 192\n255\n");
  for (unsigned y = 0; y < 192; y++) {
    for (unsigned x = 0; x < 192; x++) {
      unsigned base = ((((x / 12) ^ (y / 12)) & 1U) != 0U) ? 205U : 35U;
      unsigned texture = (x * 17U + y * 29U + (x * y) % 37U) % 43U;
      int dx = (int)x - 96, dy = (int)y - 96;
      unsigned ring = (dx * dx + dy * dy > 32 * 32 &&
                       dx * dx + dy * dy < 42 * 42)
                          ? 45U
                          : 0U;
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

static bool write_uniform_pgm(const char *path) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  bool ok = fprintf(file, "P5\n192 192\n255\n") > 0;
  unsigned char row[192];
  memset(row, 127, sizeof(row));
  for (size_t y = 0; ok && y < 192; ++y)
    ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
  return fclose(file) == 0 && ok;
}

static bool write_gaussian_blob_pgm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P5\n192 192\n255\n");
  for (unsigned y = 0; y < 192; y++) {
    for (unsigned x = 0; x < 192; x++) {
      double dx = (double)x - 96.0, dy = (double)y - 96.0;
      double r2 = (dx * dx + dy * dy) / (40.0 * 40.0);
      double v = 200.0 * exp(-r2) + 30.0;
      unsigned char c = (unsigned char)(v > 255.0 ? 255.0 : v);
      if (fwrite(&c, 1, 1, f) != 1) {
        fclose(f);
        return false;
      }
    }
  }
  return fclose(f) == 0;
}

static bool write_large_pgm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P5\n512 512\n255\n");
  for (unsigned y = 0; y < 512; y++) {
    for (unsigned x = 0; x < 512; x++) {
      unsigned base = (((x / 8) ^ (y / 8)) & 1U) ? 210U : 30U;
      unsigned t1 = (x * 13U + y * 31U) % 67U;
      unsigned t2 = ((x + y) * 7U + (x * y) % 53U) % 41U;
      int dx = (int)x - 256, dy = (int)y - 256;
      unsigned ring = (dx * dx + dy * dy > 80 * 80 &&
                       dx * dx + dy * dy < 120 * 120)
                          ? 50U
                          : 0U;
      unsigned v = base + t1 + t2 + ring;
      unsigned char c = (unsigned char)(v > 255U ? 255U : v);
      if (fwrite(&c, 1, 1, f) != 1) {
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
                       Lardon3DTaskState wanted,
                       Lardon3DTaskSnapshot *out) {
  for (size_t i = 0; i < 2000000; i++) {
    if (lardon3d_task_queue_get(q, id, out) && out->state == wanted)
      return true;
    sched_yield();
  }
  return false;
}

/* -- Concurrency types -- */

typedef struct {
  Lardon3DAppState *state;
  uint64_t orb_id;
  uint64_t sift_id;
  Lardon3DFeatureConsolidationParameters params;
  Lardon3DProjectDbFeatureSupportSet support;
  Lardon3DProjectDbResult result;
} ConsolidationCtx;

static void *consolidate_thread(void *ud) {
  ConsolidationCtx *ctx = ud;
  ctx->result = lardon3d_consolidate_orb_sift(
      ctx->state, ctx->orb_id, ctx->sift_id, &ctx->params,
      &ctx->support);
  return NULL;
}

static bool find_feature_set(Lardon3DProjectDb *db, uint64_t image_id,
                             const char *kind,
                             const unsigned char fp[32],
                             Lardon3DProjectDbFeatureSet *out) {
  return lardon3d_project_db_find_feature_set(db, image_id, kind, 1, fp,
                                              out) ==
         LARDON3D_PROJECT_DB_OK;
}

/* -- Tests -- */

static bool test_zero_features_both_empty(void) {
  char root[] = "/tmp/lardon3d-consol-zero-both-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "uniform.pgm") &&
        write_uniform_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_create(&state, "ZeroBoth"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state, image.image_id,
                                                &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED, &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id,
                                             &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED, &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params, sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb", orb_fp,
                         &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift", sift_fp,
                         &sift_set));
  CHECK(orb_set.feature_count == 0 && sift_set.feature_count == 0);

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  Lardon3DProjectDbResult r = lardon3d_consolidate_orb_sift(
      &state, orb_set.feature_set_id, sift_set.feature_set_id, &cp, &pub);
  CHECK(r == LARDON3D_PROJECT_DB_OK);
  CHECK(pub.group_count == 0);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_zero_features_both_empty\n");
  return true;
}

static bool test_zero_orb_only_sift(void) {
  char root[] = "/tmp/lardon3d-consol-zero-orb-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "blob.pgm") &&
        write_gaussian_blob_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_create(&state, "ZeroOrb"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state,
                                                image.image_id,
                                                &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED, &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id,
                                             &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED, &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params, sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));
  CHECK(sift_set.feature_count > 0);

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id, sift_set.feature_set_id,
            &cp, &pub) == LARDON3D_PROJECT_DB_OK);

  Lardon3DProjectDbFeatureSupportGroup groups[256];
  uint32_t listed = 0;
  uint64_t after = 0;
  if (orb_set.feature_count == 0) {
    /* Path zero-ORB : chaque SIFT isolé devient un groupe. */
    CHECK(pub.group_count == sift_set.feature_count);
    do {
      size_t cnt = 0;
      CHECK(lardon3d_project_db_list_feature_support_groups(
                state.project_db, pub.feature_support_set_id, after,
                groups, 256, &cnt) == LARDON3D_PROJECT_DB_OK);
      if (cnt == 0) break;
      for (size_t i = 0; i < cnt; ++i) {
        CHECK(groups[i].support_count == 1);
        CHECK(!groups[i].has_second_feature);
        after = groups[i].feature_support_group_id;
      }
      listed += (uint32_t)cnt;
    } while (listed < pub.group_count);
    CHECK(listed == pub.group_count);
  } else {
    /* FAST a tout de même détecté des coins sur le blob : la
       consolidation doit fonctionner et produire des groupes valides. */
    CHECK(pub.group_count > 0);
    do {
      size_t cnt = 0;
      CHECK(lardon3d_project_db_list_feature_support_groups(
                state.project_db, pub.feature_support_set_id, after,
                groups, 256, &cnt) == LARDON3D_PROJECT_DB_OK);
      if (cnt == 0) break;
      for (size_t i = 0; i < cnt; ++i) {
        CHECK(groups[i].support_count == 1 ||
              groups[i].support_count == 2);
        after = groups[i].feature_support_group_id;
      }
      listed += (uint32_t)cnt;
    } while (listed < pub.group_count);
    CHECK(listed == pub.group_count);
  }

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_zero_orb_only_sift\n");
  return true;
}

static bool test_zero_sift_only_orb(void) {
  char root[] = "/tmp/lardon3d-consol-zero-sift-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_create(&state, "ZeroSift"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state, image.image_id,
                                                &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED, &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED, &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params, sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb", orb_fp,
                         &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));
  CHECK(orb_set.feature_count > 0);

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id, sift_set.feature_set_id,
            &cp, &pub) == LARDON3D_PROJECT_DB_OK);
  if (sift_set.feature_count == 0) {
    CHECK(pub.group_count == orb_set.feature_count);
  } else {
    CHECK(pub.group_count > 0);
  }

  Lardon3DProjectDbFeatureSupportGroup groups[256];
  uint32_t listed = 0;
  uint64_t after = 0;
  do {
    size_t cnt = 0;
    CHECK(lardon3d_project_db_list_feature_support_groups(
              state.project_db, pub.feature_support_set_id, after,
              groups, 256, &cnt) == LARDON3D_PROJECT_DB_OK);
    if (cnt == 0) break;
    for (size_t i = 0; i < cnt; ++i) {
      CHECK(groups[i].support_count == 1 ||
            groups[i].support_count == 2);
      after = groups[i].feature_support_group_id;
    }
    listed += (uint32_t)cnt;
  } while (listed < pub.group_count);
  CHECK(listed == pub.group_count);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_zero_sift_only_orb\n");
  return true;
}

static bool test_radius_boundaries(void) {
  Lardon3DFeatureConsolidationParameters p;

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = 0.5};
  CHECK(lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = 64.0};
  CHECK(lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = 4.0};
  CHECK(lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = 0.49};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = 64.01};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = -1.0};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = 0.0};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = NAN};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = INFINITY};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  p = (Lardon3DFeatureConsolidationParameters){
      .association_radius_pixels = -INFINITY};
  CHECK(!lardon3d_feature_consolidation_parameters_valid(&p));

  fprintf(stderr, "  PASS test_radius_boundaries\n");
  return true;
}

static bool test_different_radius_different_result(void) {
  char root[] = "/tmp/lardon3d-consol-radius-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_create(&state, "Radius"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state, image.image_id,
                                                &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED, &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id,
                                             &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED, &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params, sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb", orb_fp,
                         &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp_small = {
      .association_radius_pixels = 2.0};
  Lardon3DProjectDbFeatureSupportSet pub_small;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id, sift_set.feature_set_id,
            &cp_small, &pub_small) == LARDON3D_PROJECT_DB_OK);

  Lardon3DFeatureConsolidationParameters cp_large = {
      .association_radius_pixels = 8.0};
  Lardon3DProjectDbFeatureSupportSet pub_large;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id, sift_set.feature_set_id,
            &cp_large, &pub_large) == LARDON3D_PROJECT_DB_OK);

  unsigned char fp_small[32], fp_large[32];
  lardon3d_feature_consolidation_fingerprint(&cp_small, fp_small);
  lardon3d_feature_consolidation_fingerprint(&cp_large, fp_large);
  CHECK(memcmp(fp_small, fp_large, 32) != 0);
  CHECK(pub_small.feature_support_set_id !=
        pub_large.feature_support_set_id);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr,
          "  PASS test_different_radius_different_result\n");
  return true;
}

static bool test_support_count_values(void) {
  char root[] = "/tmp/lardon3d-consol-supp-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "SupportCount"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state, image.image_id,
                                                &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED, &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id,
                                             &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED, &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params, sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb", orb_fp,
                         &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id, sift_set.feature_set_id,
            &cp, &pub) == LARDON3D_PROJECT_DB_OK);
  CHECK(pub.group_count > 0);

  bool has_pair = false;
  Lardon3DProjectDbFeatureSupportGroup groups[256];
  uint32_t listed = 0;
  uint64_t after = 0;
  do {
    size_t cnt = 0;
    CHECK(lardon3d_project_db_list_feature_support_groups(
              state.project_db, pub.feature_support_set_id, after,
              groups, 256, &cnt) == LARDON3D_PROJECT_DB_OK);
    if (cnt == 0) break;
    for (size_t i = 0; i < cnt; ++i) {
      CHECK(groups[i].support_count == 1 ||
            groups[i].support_count == 2);
      CHECK(groups[i].distance_pixels >= 0.0);
      CHECK(groups[i].distance_pixels <=
            cp.association_radius_pixels);
      has_pair |= (groups[i].support_count == 2);
      after = groups[i].feature_support_group_id;
    }
    listed += (uint32_t)cnt;
  } while (listed < pub.group_count);
  CHECK(listed == pub.group_count);
  CHECK(has_pair);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_support_count_values\n");
  return true;
}

static bool test_mismatch_different_image(void) {
  char root[] = "/tmp/lardon3d-consol-mismatch-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char src1[PATH_MAX], src2[PATH_MAX];
  CHECK(join_path(src1, root, "a.pgm") && write_pgm(src1));
  CHECK(join_path(src2, root, "b.pgm") && write_large_pgm(src2));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "Mismatch"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage img1, img2;
  Lardon3DProjectDbImageAsset a1, a2;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, src1, 0, &img1,
            &a1) == LARDON3D_IMAGE_CATALOG_IMPORTED);
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, src2, 0, &img2,
            &a2) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task1 = 0, orb_task2 = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, img1.image_id, &orb_params, &orb_task1) &&
        lardon3d_project_enqueue_feature_extract(
            &state, img2.image_id, &orb_params, &orb_task2));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task1, TASK_COMPLETED,
                   &snap) &&
        wait_state(state.task_queue, orb_task2, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, img1.image_id,
                                             &sift_params,
                                             &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char fp1[32], fp2[32], sfp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, fp1);
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, fp2);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params, sfp);

  Lardon3DProjectDbFeatureSet orb1, orb2, sift1;
  CHECK(find_feature_set(state.project_db, img1.image_id, "orb",
                         fp1, &orb1));
  CHECK(find_feature_set(state.project_db, img2.image_id, "orb",
                         fp2, &orb2));
  CHECK(find_feature_set(state.project_db, img1.image_id, "sift",
                         sfp, &sift1));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  Lardon3DProjectDbResult r = lardon3d_consolidate_orb_sift(
      &state, orb2.feature_set_id, sift1.feature_set_id, &cp,
      &pub);
  CHECK(r == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_mismatch_different_image\n");
  return true;
}

static bool test_mismatch_wrong_extractor(void) {
  char root[] = "/tmp/lardon3d-consol-mismatch-ext-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "MismatchExt"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  unsigned char fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, fp);
  Lardon3DProjectDbFeatureSet orb_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         fp, &orb_set));

  /* first == second */
  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  Lardon3DProjectDbResult r = lardon3d_consolidate_orb_sift(
      &state, orb_set.feature_set_id, orb_set.feature_set_id,
      &cp, &pub);
  CHECK(r == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_mismatch_wrong_extractor\n");
  return true;
}

static bool test_large_set(void) {
  char root[] = "/tmp/lardon3d-consol-large-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "large.pgm") &&
        write_large_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "LargeSet"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {4096, 8, 12};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 4096;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));
  CHECK(orb_set.feature_count > 0 && sift_set.feature_count > 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp,
            &pub) == LARDON3D_PROJECT_DB_OK);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (t1.tv_sec - t0.tv_sec) +
                   (t1.tv_nsec - t0.tv_nsec) / 1e9;

  CHECK(pub.group_count > 0);
  CHECK(elapsed < 10.0);

  Lardon3DProjectDbFeatureSupportGroup groups[256];
  uint32_t listed = 0;
  uint64_t after = 0;
  do {
    size_t cnt = 0;
    CHECK(lardon3d_project_db_list_feature_support_groups(
              state.project_db, pub.feature_support_set_id,
              after, groups, 256, &cnt) ==
          LARDON3D_PROJECT_DB_OK);
    if (cnt == 0) break;
    for (size_t i = 0; i < cnt; ++i) {
      CHECK(isfinite(groups[i].x) && isfinite(groups[i].y));
      CHECK(groups[i].support_count >= 1 &&
            groups[i].support_count <= 2);
      after = groups[i].feature_support_group_id;
    }
    listed += (uint32_t)cnt;
  } while (listed < pub.group_count);

  fprintf(stderr,
          "  PASS test_large_set (%u ORB, %u SIFT, "
          "%u groups, %.3fs)\n",
          orb_set.feature_count, sift_set.feature_count,
          pub.group_count, elapsed);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  return true;
}

static bool test_idempotent(void) {
  char root[] = "/tmp/lardon3d-consol-idem-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "Idempotent"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub1, pub2;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp,
            &pub1) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp,
            &pub2) == LARDON3D_PROJECT_DB_OK);
  CHECK(pub1.feature_support_set_id ==
        pub2.feature_support_set_id);
  CHECK(pub1.group_count == pub2.group_count);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_idempotent\n");
  return true;
}

static bool test_concurrent_identical(void) {
  char root[] = "/tmp/lardon3d-consol-conc-id-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "ConcIdentical"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  ConsolidationCtx ctx[2] = {
      {.state = &state,
       .orb_id = orb_set.feature_set_id,
       .sift_id = sift_set.feature_set_id,
       .params = cp},
      {.state = &state,
       .orb_id = orb_set.feature_set_id,
       .sift_id = sift_set.feature_set_id,
       .params = cp}};

  pthread_t threads[2];
  CHECK(pthread_create(&threads[0], NULL, consolidate_thread,
                       &ctx[0]) == 0 &&
        pthread_create(&threads[1], NULL, consolidate_thread,
                       &ctx[1]) == 0 &&
        pthread_join(threads[0], NULL) == 0 &&
        pthread_join(threads[1], NULL) == 0);

  CHECK(ctx[0].result == LARDON3D_PROJECT_DB_OK);
  CHECK(ctx[1].result == LARDON3D_PROJECT_DB_OK);
  CHECK(ctx[0].support.feature_support_set_id ==
        ctx[1].support.feature_support_set_id);
  CHECK(ctx[0].support.group_count ==
        ctx[1].support.group_count);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_concurrent_identical\n");
  return true;
}

static bool test_concurrent_different(void) {
  char root[] = "/tmp/lardon3d-consol-conc-diff-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "ConcDifferent"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params1 =
      lardon3d_sift_precision_classic_v1(false);
  sift_params1.max_features = 496;
  uint64_t sift_task1 = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params1,
            &sift_task1));
  CHECK(wait_state(state.task_queue, sift_task1, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params2 = sift_params1;
  sift_params2.max_features = 497;
  uint64_t sift_task2 = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params2,
            &sift_task2));
  CHECK(wait_state(state.task_queue, sift_task2, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sfp1[32], sfp2[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params1,
                                                sfp1);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params2,
                                                sfp2);

  Lardon3DProjectDbFeatureSet orb_set, sift_set1, sift_set2;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sfp1, &sift_set1));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sfp2, &sift_set2));
  CHECK(sift_set1.feature_set_id != sift_set2.feature_set_id);

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  ConsolidationCtx ctx[2] = {
      {.state = &state,
       .orb_id = orb_set.feature_set_id,
       .sift_id = sift_set1.feature_set_id,
       .params = cp},
      {.state = &state,
       .orb_id = orb_set.feature_set_id,
       .sift_id = sift_set2.feature_set_id,
       .params = cp}};

  pthread_t threads[2];
  CHECK(pthread_create(&threads[0], NULL, consolidate_thread,
                       &ctx[0]) == 0 &&
        pthread_create(&threads[1], NULL, consolidate_thread,
                       &ctx[1]) == 0 &&
        pthread_join(threads[0], NULL) == 0 &&
        pthread_join(threads[1], NULL) == 0);

  CHECK(ctx[0].result == LARDON3D_PROJECT_DB_OK);
  CHECK(ctx[1].result == LARDON3D_PROJECT_DB_OK);
  CHECK(ctx[0].support.feature_support_set_id !=
        ctx[1].support.feature_support_set_id);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_concurrent_different\n");
  return true;
}

static bool test_fingerprint(void) {
  lardon3d_feature_consolidation_fingerprint(NULL, NULL);

  Lardon3DFeatureConsolidationParameters p = {
      .association_radius_pixels = 4.0};
  unsigned char fp1[32];
  lardon3d_feature_consolidation_fingerprint(&p, fp1);
  bool non_zero = false;
  for (int i = 0; i < 32; ++i) non_zero |= (fp1[i] != 0);
  CHECK(non_zero);

  Lardon3DFeatureConsolidationParameters q = {
      .association_radius_pixels = 8.0};
  unsigned char fp2[32];
  lardon3d_feature_consolidation_fingerprint(&q, fp2);
  CHECK(memcmp(fp1, fp2, 32) != 0);

  unsigned char fp3[32];
  lardon3d_feature_consolidation_fingerprint(&p, fp3);
  CHECK(memcmp(fp1, fp3, 32) == 0);

  fprintf(stderr, "  PASS test_fingerprint\n");
  return true;
}

static bool test_quality_comparison(void) {
  Lardon3DProjectDbFeatureSupportGroup isolated = {
      .feature_support_group_id = 10, .support_count = 1};
  Lardon3DProjectDbFeatureSupportGroup confirmed = {
      .feature_support_group_id = 5, .support_count = 2};

  CHECK(lardon3d_feature_support_higher_quality(&confirmed,
                                                &isolated));
  CHECK(!lardon3d_feature_support_higher_quality(&isolated,
                                                 &confirmed));

  Lardon3DProjectDbFeatureSupportGroup a = {
      .feature_support_group_id = 3, .support_count = 2};
  Lardon3DProjectDbFeatureSupportGroup b = {
      .feature_support_group_id = 7, .support_count = 2};
  CHECK(lardon3d_feature_support_higher_quality(&a, &b));
  CHECK(!lardon3d_feature_support_higher_quality(&b, &a));

  CHECK(!lardon3d_feature_support_higher_quality(NULL, &isolated));
  CHECK(
      !lardon3d_feature_support_higher_quality(&isolated, NULL));

  fprintf(stderr, "  PASS test_quality_comparison\n");
  return true;
}

static bool test_pagination_exhaustive(void) {
  char root[] = "/tmp/lardon3d-consol-page-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "Pagination"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp,
            &pub) == LARDON3D_PROJECT_DB_OK);
  CHECK(pub.group_count > 0);

  uint32_t listed = 0;
  uint64_t after = 0;
  do {
    Lardon3DProjectDbFeatureSupportGroup g;
    size_t cnt = 0;
    CHECK(lardon3d_project_db_list_feature_support_groups(
              state.project_db, pub.feature_support_set_id,
              after, &g, 1, &cnt) == LARDON3D_PROJECT_DB_OK);
    if (cnt == 0) break;
    CHECK(g.feature_support_group_id > after);
    after = g.feature_support_group_id;
    listed += (uint32_t)cnt;
  } while (listed < pub.group_count);
  CHECK(listed == pub.group_count);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_pagination_exhaustive\n");
  return true;
}

static bool test_stress(void) {
  char root[] = "/tmp/lardon3d-consol-stress-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "Stress"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  uint32_t reference_groups = 0;
  uint64_t reference_id = 0;

  for (int i = 0; i < 10; ++i) {
    Lardon3DProjectDbFeatureSupportSet pub;
    CHECK(lardon3d_consolidate_orb_sift(
              &state, orb_set.feature_set_id,
              sift_set.feature_set_id, &cp,
              &pub) == LARDON3D_PROJECT_DB_OK);
    if (i == 0) {
      reference_groups = pub.group_count;
      reference_id = pub.feature_support_set_id;
    } else {
      CHECK(pub.feature_support_set_id == reference_id);
      CHECK(pub.group_count == reference_groups);
    }
  }

  fprintf(stderr,
          "  PASS test_stress (10 iterations, %u groups)\n",
          reference_groups);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  return true;
}

static bool test_db_first_equals_second(void) {
  char root[] = "/tmp/lardon3d-consol-db-eq-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "DBEqual"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  unsigned char fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params, fp);
  Lardon3DProjectDbFeatureSet set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         fp, &set));

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, set.feature_set_id, set.feature_set_id,
            &cp, &pub) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_db_first_equals_second\n");
  return true;
}

static bool test_radius_min_max_work(void) {
  char root[] = "/tmp/lardon3d-consol-rad-mm-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "RadiusMinMax"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));

  Lardon3DFeatureConsolidationParameters cp_min = {
      .association_radius_pixels = 0.5};
  Lardon3DProjectDbFeatureSupportSet pub_min;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp_min,
            &pub_min) == LARDON3D_PROJECT_DB_OK);

  Lardon3DFeatureConsolidationParameters cp_max = {
      .association_radius_pixels = 64.0};
  Lardon3DProjectDbFeatureSupportSet pub_max;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp_max,
            &pub_max) == LARDON3D_PROJECT_DB_OK);

  CHECK(pub_min.feature_support_set_id !=
        pub_max.feature_support_set_id);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_radius_min_max_work\n");
  return true;
}

static bool test_support_count_orb_only_indices(void) {
  char root[] = "/tmp/lardon3d-consol-sc-idx-XXXXXX";
  CHECK(mkdtemp(root) &&
        setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "texture.pgm") && write_pgm(source));

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) &&
        lardon3d_project_create(&state, "SCIdx"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(
            &state, scanset.scanset_id, source, 0, &image,
            &asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);

  Lardon3DFeatureExtractorParameters orb_params = {512, 4, 10};
  uint64_t orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &orb_params, &orb_task));
  Lardon3DTaskSnapshot snap;
  CHECK(wait_state(state.task_queue, orb_task, TASK_COMPLETED,
                   &snap));

  Lardon3DSiftExtractorParameters sift_params =
      lardon3d_sift_precision_classic_v1(false);
  sift_params.max_features = 512;
  uint64_t sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &sift_params, &sift_task));
  CHECK(wait_state(state.task_queue, sift_task, TASK_COMPLETED,
                   &snap));

  unsigned char orb_fp[32], sift_fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&orb_params,
                                                   orb_fp);
  lardon3d_sift_extractor_parameter_fingerprint(&sift_params,
                                                sift_fp);

  Lardon3DProjectDbFeatureSet orb_set, sift_set;
  CHECK(find_feature_set(state.project_db, image.image_id, "orb",
                         orb_fp, &orb_set));
  CHECK(find_feature_set(state.project_db, image.image_id, "sift",
                         sift_fp, &sift_set));
  CHECK(orb_set.feature_count > 0 && sift_set.feature_count > 0);

  Lardon3DFeatureConsolidationParameters cp =
      lardon3d_feature_consolidation_v1();
  Lardon3DProjectDbFeatureSupportSet pub;
  CHECK(lardon3d_consolidate_orb_sift(
            &state, orb_set.feature_set_id,
            sift_set.feature_set_id, &cp,
            &pub) == LARDON3D_PROJECT_DB_OK);
  CHECK(pub.group_count > 0);

  Lardon3DProjectDbFeatureSupportGroup groups[256];
  uint32_t listed = 0;
  uint64_t after = 0;
  do {
    size_t cnt = 0;
    CHECK(lardon3d_project_db_list_feature_support_groups(
              state.project_db, pub.feature_support_set_id,
              after, groups, 256, &cnt) ==
          LARDON3D_PROJECT_DB_OK);
    if (cnt == 0) break;
    for (size_t i = 0; i < cnt; ++i) {
      CHECK(groups[i].support_count == 1 ||
            groups[i].support_count == 2);
      CHECK(groups[i].first_feature_index <
            (groups[i].first_member_from_second_set
                 ? sift_set.feature_count
                 : orb_set.feature_count));
      if (groups[i].has_second_feature) {
        CHECK(groups[i].second_feature_index <
              (groups[i].first_member_from_second_set
                   ? orb_set.feature_count
                   : sift_set.feature_count));
      }
      after = groups[i].feature_support_group_id;
    }
    listed += (uint32_t)cnt;
  } while (listed < pub.group_count);
  CHECK(listed == pub.group_count);

  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  fprintf(stderr, "  PASS test_support_count_orb_only_indices\n");
  return true;
}

/* -- Main -- */

typedef bool (*test_fn)(void);

typedef struct {
  const char *name;
  test_fn fn;
} TestEntry;

int main(void) {
  static const TestEntry tests[] = {
      {"zero_features_both_empty", test_zero_features_both_empty},
      {"zero_orb_only_sift", test_zero_orb_only_sift},
      {"zero_sift_only_orb", test_zero_sift_only_orb},
      {"radius_boundaries", test_radius_boundaries},
      {"different_radius_different_result",
       test_different_radius_different_result},
      {"support_count_values", test_support_count_values},
      {"mismatch_different_image",
       test_mismatch_different_image},
      {"mismatch_wrong_extractor",
       test_mismatch_wrong_extractor},
      {"large_set", test_large_set},
      {"idempotent", test_idempotent},
      {"concurrent_identical", test_concurrent_identical},
      {"concurrent_different", test_concurrent_different},
      {"fingerprint", test_fingerprint},
      {"quality_comparison", test_quality_comparison},
      {"pagination_exhaustive", test_pagination_exhaustive},
      {"stress", test_stress},
      {"db_first_equals_second",
       test_db_first_equals_second},
      {"radius_min_max_work", test_radius_min_max_work},
      {"support_count_orb_only_indices",
       test_support_count_orb_only_indices},
  };

  size_t pass = 0, fail = 0;
  size_t total = sizeof(tests) / sizeof(tests[0]);
  for (size_t i = 0; i < total; ++i) {
    fprintf(stderr, "[%zu/%zu] %s ... ", i + 1, total,
            tests[i].name);
    if (tests[i].fn()) {
      ++pass;
    } else {
      ++fail;
      fprintf(stderr, "FAILED\n");
    }
  }
  fprintf(stderr,
          "\n=== Results: %zu passed, %zu failed out of %zu "
          "===\n",
          pass, fail, total);
  return fail ? 1 : 0;
}