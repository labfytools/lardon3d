#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "Échec ligne %d: %s\n", __LINE__, #x);                                       \
      return false;                                                                                \
    }                                                                                              \
  } while (0)
static bool join_path(char out[PATH_MAX], const char *a, const char *b) {
  int n = snprintf(out, PATH_MAX, "%s/%s", a, b);
  return n > 0 && (size_t)n < PATH_MAX;
}
static bool remove_tree(const char *p) {
  struct stat s;
  if (lstat(p, &s) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(s.st_mode)) {
    return unlink(p) == 0;
  }
  DIR *d = opendir(p);
  if (!d) {
    return false;
  }
  bool ok = true;
  for (struct dirent *e = readdir(d); e; e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    char c[PATH_MAX];
    if (!join_path(c, p, e->d_name) || !remove_tree(c)) {
      ok = false;
    }
  }
  if (closedir(d) || rmdir(p)) {
    ok = false;
  }
  return ok;
}
static bool write_pgm(const char *p) {
  FILE *f = fopen(p, "wb");
  if (!f) {
    return false;
  }
  fprintf(f, "P5\n192 192\n255\n");
  for (unsigned y = 0; y < 192; y++) {
    for (unsigned x = 0; x < 192; x++) {
      unsigned char v = (unsigned char)((((x / 6) ^ (y / 6)) & 1) ? 245 : 10);
      if (fwrite(&v, 1, 1, f) != 1) {
        fclose(f);
        return false;
      }
    }
  }
  return fclose(f) == 0;
}
static bool runtime(Lardon3DAppState *s) {
  s->hardware_profile = (Lardon3DHardwareProfile){.logical_cpu_count = 64,
                                                  .page_size_bytes = 4096,
                                                  .memory_total_bytes = UINT64_MAX,
                                                  .cpu_architecture = "test"};
  Lardon3DResourcePolicy p = {
      .maximum_cpu_load_ratio = 1, .maximum_io_pressure_avg10 = 100, .io_slot_capacity = 2};
  s->resource_governor = lardon3d_resource_governor_create(&s->hardware_profile, &p);
  s->task_queue = s->resource_governor ? lardon3d_task_queue_create(s->resource_governor, 4) : NULL;
  return s->task_queue != NULL;
}
static bool wait_state(Lardon3DTaskQueue *q, uint64_t id, Lardon3DTaskState wanted,
                       Lardon3DTaskSnapshot *out) {
  for (size_t i = 0; i < 2000000; i++) {
    if (lardon3d_task_queue_get(q, id, out) && out->state == wanted) {
      return true;
    }
    sched_yield();
  }
  return false;
}

static bool run_test(void) {
  char root[] = "/tmp/lardon3d-feature-task-XXXXXX";
  CHECK(mkdtemp(root) && setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "source.pgm") && write_pgm(source));
  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_create(&state, "Features"));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, source, 0, &image, &asset) ==
        LARDON3D_IMAGE_CATALOG_IMPORTED);
  Lardon3DFeatureExtractorParameters parameters = {512, 4, 10};
  CHECK(setenv("LARDON3D_TEST_FEATURE_PAUSE_BEFORE_PUBLISH", "1", 1) == 0 &&
        setenv("LARDON3D_TEST_FEATURE_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state, image.image_id, &parameters, &task_id));
  Lardon3DTaskSnapshot snapshot;
  CHECK(wait_state(state.task_queue, task_id, TASK_PAUSED, &snapshot));
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  CHECK(unsetenv("LARDON3D_TEST_FEATURE_PAUSE_BEFORE_PUBLISH") == 0 &&
        unsetenv("LARDON3D_TEST_FEATURE_SKIP_FINISHED_CHECKPOINT") == 0);
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  Lardon3DProjectRecoverySummary summary;
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 1);
  CHECK(wait_state(state.task_queue, task_id, TASK_COMPLETED, &snapshot) &&
        snapshot.progress == 100);
  unsigned char fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&parameters, fp);
  Lardon3DProjectDbFeatureSet set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id,
                                             LARDON3D_FEATURE_EXTRACTOR_KIND, 1, fp,
                                             &set) == LARDON3D_PROJECT_DB_OK);
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 0);
  CHECK(lardon3d_project_db_load_feature_set(state.project_db, set.feature_set_id, &set) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(unlink(source) == 0);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  CHECK(lardon3d_feature_reader_open(state.project_path, &set, &reader, &metadata) ==
        LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);

  Lardon3DVisualIndexConfiguration index_configuration = {1, 256, 128};
  uint64_t visual_index_id = 0;
  CHECK(lardon3d_visual_index_create(state.project_db, &set, &index_configuration,
                                     &visual_index_id) == LARDON3D_VISUAL_INDEX_OK);
  CHECK(setenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT", "1", 1) == 0 &&
        setenv("LARDON3D_TEST_VISUAL_INDEX_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t visual_task_id = 0;
  CHECK(lardon3d_project_enqueue_visual_index_update(&state, visual_index_id, &visual_task_id));
  CHECK(wait_state(state.task_queue, visual_task_id, TASK_PAUSED, &snapshot));
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  CHECK(unsetenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT") == 0 &&
        unsetenv("LARDON3D_TEST_VISUAL_INDEX_SKIP_FINISHED_CHECKPOINT") == 0);
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 1);
  CHECK(wait_state(state.task_queue, visual_task_id, TASK_COMPLETED, &snapshot));
  Lardon3DProjectDbVisualIndexSegment visual_segments[2];
  size_t visual_segment_count = 0;
  CHECK(lardon3d_project_db_list_visual_index_segments(
            state.project_db, visual_index_id, 0, visual_segments, 2, &visual_segment_count) ==
            LARDON3D_PROJECT_DB_OK &&
        visual_segment_count == 1);

  CHECK(write_pgm(source));
  Lardon3DProjectDbImage missing_image;
  Lardon3DProjectDbImageAsset missing_asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, source, 0, &missing_image,
                                           &missing_asset) ==
        LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT);
  char managed_path[PATH_MAX];
  CHECK(join_path(managed_path, state.project_path, missing_asset.path));
  CHECK(unlink(managed_path) == 0);
  uint64_t missing_task_id = 0;
  Lardon3DFeatureExtractorParameters missing_parameters = {511, 4, 10};
  CHECK(lardon3d_project_enqueue_feature_extract(&state, missing_image.image_id,
                                                 &missing_parameters, &missing_task_id));
  CHECK(wait_state(state.task_queue, missing_task_id, TASK_FAILED, &snapshot));

  char invalid_source[PATH_MAX];
  CHECK(join_path(invalid_source, root, "invalid.pgm"));
  FILE *invalid = fopen(invalid_source, "wb");
  CHECK(invalid && fwrite("not an image", 1, 12, invalid) == 12 && fclose(invalid) == 0);
  Lardon3DProjectDbImage invalid_image;
  Lardon3DProjectDbImageAsset invalid_asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, invalid_source, 0,
                                           &invalid_image,
                                           &invalid_asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
  uint64_t invalid_task_id = 0;
  Lardon3DFeatureExtractorParameters invalid_parameters = {510, 4, 10};
  CHECK(lardon3d_project_enqueue_feature_extract(&state, invalid_image.image_id,
                                                 &invalid_parameters, &invalid_task_id));
  CHECK(wait_state(state.task_queue, invalid_task_id, TASK_FAILED, &snapshot));
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  return true;
}
int main(void) { return run_test() ? 0 : 1; }
