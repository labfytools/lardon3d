#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/matcher_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);    \
      return false;                                                            \
    }                                                                          \
  } while (0)

enum {
  IMAGE_COUNT = 42,
  PAIR_COUNT = IMAGE_COUNT - 1,
  PERSISTED_PAIR_COUNT = PAIR_COUNT - 1,
};

typedef struct {
  char root[PATH_MAX];
  Lardon3DAppState state;
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbImage images[IMAGE_COUNT];
  Lardon3DProjectDbCandidatePair pairs[PAIR_COUNT];
  unsigned char feature_fingerprint[32];
} Fixture;

static Lardon3DResourcePolicy interactive_policy(void) {
  return (Lardon3DResourcePolicy){
      .system_memory_reserve_bytes = 4ULL * 1024 * 1024 * 1024,
      .emergency_memory_floor_bytes = 2ULL * 1024 * 1024 * 1024,
      .system_cpu_reserve = 4,
      .maximum_cpu_load_ratio = 1.0,
      .maximum_cpu_pressure_avg10 = 100.0,
      .maximum_memory_pressure_avg10 = 100.0,
      .maximum_io_pressure_avg10 = 100.0,
      .io_slot_capacity = 1,
      .gpu_slot_capacity = 1,
  };
}

static bool join_path(char output[PATH_MAX], const char *left,
                      const char *right) {
  int written = snprintf(output, PATH_MAX, "%s/%s", left, right);
  return written > 0 && (size_t)written < PATH_MAX;
}

static bool query_integer(const char *path, const char *sql,
                          sqlite3_int64 expected) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  bool success = sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY,
                                 NULL) == SQLITE_OK &&
                 sqlite3_prepare_v2(connection, sql, -1, &statement, NULL) ==
                     SQLITE_OK &&
                 sqlite3_step(statement) == SQLITE_ROW &&
                 sqlite3_column_int64(statement, 0) == expected;
  if (statement) {
    (void)sqlite3_finalize(statement);
  }
  if (connection) {
    (void)sqlite3_close(connection);
  }
  return success;
}

static bool downgrade_project_to_historical_v10(const char *database_path) {
  sqlite3 *connection = NULL;
  if (sqlite3_open_v2(database_path, &connection, SQLITE_OPEN_READWRITE,
                      NULL) != SQLITE_OK) {
    if (connection) {
      (void)sqlite3_close(connection);
    }
    return false;
  }
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE matcher_tasks;"
      "UPDATE metadata SET value=10 WHERE key='schema_version';"
      "COMMIT;PRAGMA foreign_keys=ON;";
  bool success = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && success;
}

static bool remove_tree(const char *path) {
  struct stat information;
  if (lstat(path, &information) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(information.st_mode)) {
    return unlink(path) == 0;
  }
  DIR *directory = opendir(path);
  if (!directory) {
    return false;
  }
  bool success = true;
  for (struct dirent *entry = readdir(directory); entry;
       entry = readdir(directory)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[PATH_MAX];
    if (!join_path(child, path, entry->d_name) || !remove_tree(child)) {
      success = false;
    }
  }
  if (closedir(directory) != 0 || rmdir(path) != 0) {
    success = false;
  }
  return success;
}

static bool create_runtime(Lardon3DAppState *state) {
  state->hardware_profile = (Lardon3DHardwareProfile){
      .logical_cpu_count = 16,
      .page_size_bytes = 4096,
      .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
      .gpu_available = true,
      .gpu_uses_shared_memory = true,
      .cpu_architecture = "test",
  };
  Lardon3DResourcePolicy policy = interactive_policy();
  state->resource_governor =
      lardon3d_resource_governor_create(&state->hardware_profile, &policy);
  state->orb_vulkan_backend = lardon3d_orb_vulkan_backend_create();
  state->task_queue =
      state->resource_governor && state->orb_vulkan_backend
          ? lardon3d_task_queue_create(state->resource_governor, 16)
          : NULL;
  return state->task_queue != NULL;
}

static bool wait_state(Lardon3DTaskQueue *queue, uint64_t task_id,
                       Lardon3DTaskState wanted,
                       Lardon3DTaskSnapshot *snapshot) {
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (lardon3d_task_queue_get(queue, task_id, snapshot) &&
        snapshot->state == wanted) {
      return true;
    }
    sched_yield();
  }
  return false;
}

static bool wait_durable_state(Lardon3DProjectDb *database, uint64_t task_id,
                               Lardon3DTaskState wanted,
                               Lardon3DProjectDbTask *durable_task) {
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (lardon3d_project_db_load_task(database, task_id, durable_task) ==
            LARDON3D_PROJECT_DB_OK &&
        durable_task->saved_state == wanted) {
      return true;
    }
    sched_yield();
  }
  return false;
}

static void image_asset_path(const unsigned char hash[32], char path[4096]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < 32; ++index) {
    hex[2 * index] = digits[hash[index] >> 4];
    hex[2 * index + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, 4096, "assets/images/%c%c/%s", hex[0], hex[1], hex);
}

static bool register_image(Fixture *fixture, unsigned char seed, size_t index) {
  unsigned char hash[32];
  memset(hash, seed, sizeof(hash));
  char path[4096];
  image_asset_path(hash, path);
  Lardon3DProjectDbImageRegisterStatus status;
  return lardon3d_project_db_register_image(
             fixture->state.project_db, fixture->scanset.scanset_id, hash, path,
             1, "fixture.bin", "/synthetic/fixture.bin", 0, seed, &status,
             &fixture->images[index]) == LARDON3D_PROJECT_DB_OK;
}

static bool publish_features(Fixture *fixture, size_t image_index) {
  unsigned char descriptor[32];
  memset(descriptor, (int)image_index, sizeof(descriptor));
  Lardon3DFeatureKeypoint keypoint = {
      .size = 1.0F,
  };
  Lardon3DExtractedFeatures features = {
      .image_width = 64,
      .image_height = 64,
      .feature_count = 1,
      .keypoints = &keypoint,
      .descriptors = descriptor,
      .descriptor_bytes = sizeof(descriptor),
  };
  Lardon3DProjectDbFeatureSet feature_set;
  return lardon3d_feature_store_publish_v2(
             &fixture->state, fixture->images[image_index].image_id, 0, "orb",
             1, fixture->feature_fingerprint, LARDON3D_FEATURE_DESCRIPTOR_U8,
             32, 0, &features, &feature_set) == LARDON3D_FEATURE_STORE_OK;
}

static bool fixture_create(Fixture *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  char root[] = "/tmp/lardon3d-matcher-task-XXXXXX";
  char *created = mkdtemp(root);
  if (!created ||
      snprintf(fixture->root, sizeof(fixture->root), "%s", created) <= 0 ||
      setenv("LARDON3D_PROJECTS_ROOT", fixture->root, 1) != 0) {
    return false;
  }
  memset(fixture->feature_fingerprint, 0x5A,
         sizeof(fixture->feature_fingerprint));
  lardon3d_app_state_init(&fixture->state);
  if (!create_runtime(&fixture->state) ||
      !lardon3d_project_create(&fixture->state, "MatcherTask") ||
      lardon3d_project_db_create_scanset(fixture->state.project_db,
                                         "matcher-task", &fixture->scanset) !=
          LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  for (size_t index = 0; index < IMAGE_COUNT; ++index) {
    if (!register_image(fixture, (unsigned char)(index + 1), index) ||
        !publish_features(fixture, index)) {
      return false;
    }
  }
  for (size_t index = 0; index < PAIR_COUNT; ++index) {
    if (lardon3d_project_db_create_candidate_pair(
            fixture->state.project_db, fixture->images[index].image_id,
            fixture->images[index + 1].image_id, (int64_t)index,
            &fixture->pairs[index]) != LARDON3D_PROJECT_DB_OK) {
      return false;
    }
  }
  char database_path[PATH_MAX];
  if (!join_path(database_path, fixture->state.project_path, "project.db")) {
    return false;
  }
  sqlite3 *connection = NULL;
  if (sqlite3_open_v2(database_path, &connection, SQLITE_OPEN_READWRITE,
                      NULL) != SQLITE_OK) {
    if (connection) {
      sqlite3_close(connection);
    }
    return false;
  }
  char sql[128];
  int written =
      snprintf(sql, sizeof(sql),
               "DELETE FROM candidate_pairs WHERE candidate_pair_id=%lu",
               (unsigned long)fixture->pairs[2].candidate_pair_id);
  bool deleted = written > 0 && (size_t)written < sizeof(sql) &&
                 sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK &&
                 sqlite3_changes(connection) == 1;
  (void)sqlite3_close(connection);
  if (!deleted) {
    return false;
  }
  return true;
}

static void stop_runtime(Fixture *fixture) {
  if (fixture->state.task_queue) {
    lardon3d_task_queue_destroy(fixture->state.task_queue);
    fixture->state.task_queue = NULL;
  }
  lardon3d_project_close(&fixture->state);
  if (fixture->state.resource_governor) {
    lardon3d_resource_governor_destroy(fixture->state.resource_governor);
    fixture->state.resource_governor = NULL;
  }
  lardon3d_orb_vulkan_backend_destroy(fixture->state.orb_vulkan_backend);
  fixture->state.orb_vulkan_backend = NULL;
}

static bool reopen_runtime(Fixture *fixture) {
  lardon3d_app_state_init(&fixture->state);
  return create_runtime(&fixture->state) &&
         lardon3d_project_open(&fixture->state, "MatcherTask");
}

static Lardon3DMatcherTaskConfiguration configuration(const Fixture *fixture) {
  Lardon3DMatcherTaskConfiguration result = {
      .feature_extractor_version = 1,
      .matcher =
          {
              .kind = LARDON3D_MATCHER_ORB_BF,
              .ratio_threshold = 0.75F,
          },
  };
  (void)snprintf(result.feature_extractor_kind,
                 sizeof(result.feature_extractor_kind), "orb");
  memcpy(result.feature_parameter_fingerprint, fixture->feature_fingerprint,
         sizeof(result.feature_parameter_fingerprint));
  return result;
}

static bool count_results(Fixture *fixture, size_t *count) {
  Lardon3DProjectDbMatchResult results[16];
  uint64_t cursor = 0;
  *count = 0;
  for (;;) {
    size_t page_count = 0;
    if (lardon3d_project_db_list_match_results(
            fixture->state.project_db, cursor, results, 16, &page_count) !=
        LARDON3D_PROJECT_DB_OK) {
      return false;
    }
    *count += page_count;
    if (page_count < 16) {
      return true;
    }
    cursor = results[page_count - 1].match_result_id;
  }
}

static bool run_test(void) {
  Fixture fixture;
  CHECK(fixture_create(&fixture));
  char database_path[PATH_MAX];
  CHECK(join_path(database_path, fixture.state.project_path, "project.db"));
  stop_runtime(&fixture);
  CHECK(downgrade_project_to_historical_v10(database_path));
  CHECK(query_integer(database_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      10));
  CHECK(query_integer(database_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                      "name='match_results'",
                      1));
  CHECK(query_integer(database_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                      "name='matcher_tasks'",
                      0));
  CHECK(reopen_runtime(&fixture));
  CHECK(lardon3d_project_db_schema_version(fixture.state.project_db) == 11);
  CHECK(query_integer(database_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                      "name='matcher_tasks'",
                      1));
  const Lardon3DTaskKindRegistry *registry =
      lardon3d_task_kind_registry_production();
  const Lardon3DTaskKindDescriptor *descriptor = NULL;
  CHECK(lardon3d_task_kind_registry_lookup(registry, LARDON3D_MATCHER_TASK_KIND,
                                           LARDON3D_MATCHER_TASK_KIND_VERSION,
                                           &descriptor) ==
            LARDON3D_TASK_KIND_OK &&
        descriptor != NULL);

  Lardon3DMatcherTaskConfiguration settings = configuration(&fixture);
  Lardon3DMatcherTaskConfiguration invalid = settings;
  invalid.matcher.kind = LARDON3D_MATCHER_SIFT_BF;
  uint64_t invalid_task_id = 0;
  CHECK(!lardon3d_project_create_matcher_task(&fixture.state, &invalid,
                                              &invalid_task_id));
  CHECK(invalid_task_id == 0);
  CHECK(setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  CHECK(setenv("LARDON3D_TEST_MATCHER_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(&fixture.state, &settings,
                                              &task_id));
  Lardon3DTaskSnapshot snapshot;
  CHECK(wait_state(fixture.state.task_queue, task_id, TASK_PAUSED, &snapshot));

  size_t result_count = 0;
  CHECK(count_results(&fixture, &result_count) && result_count == 1);
  Lardon3DProjectDbMatcherTask saved;
  CHECK(lardon3d_project_db_load_matcher_task(fixture.state.project_db, task_id,
                                              &saved) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(saved.after_candidate_pair_id == 0);
  CHECK(saved.matcher_kind == LARDON3D_MATCHER_ORB_BF);
  CHECK(saved.ratio_threshold == 0.75F);

  stop_runtime(&fixture);
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION") == 0);
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_SKIP_FINISHED_CHECKPOINT") == 0);
  CHECK(reopen_runtime(&fixture));
  Lardon3DProjectRecoverySummary recovery;
  CHECK(lardon3d_project_last_recovery_summary(&fixture.state, &recovery));
  CHECK(recovery.resumed == 1);
  CHECK(
      wait_state(fixture.state.task_queue, task_id, TASK_COMPLETED, &snapshot));
  CHECK(snapshot.progress == 100);
  Lardon3DProjectDbTask durable_task;
  CHECK(wait_durable_state(fixture.state.project_db, task_id, TASK_COMPLETED,
                           &durable_task));
  CHECK(durable_task.sequence_count >= 1);
  CHECK(count_results(&fixture, &result_count) &&
        result_count == PERSISTED_PAIR_COUNT);
  CHECK(lardon3d_project_db_load_matcher_task(fixture.state.project_db, task_id,
                                              &saved) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(saved.after_candidate_pair_id ==
        fixture.pairs[PAIR_COUNT - 1].candidate_pair_id);

  CHECK(setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  uint64_t cancelled_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(&fixture.state, &settings,
                                              &cancelled_id));
  CHECK(wait_state(fixture.state.task_queue, cancelled_id, TASK_PAUSED,
                   &snapshot));
  CHECK(lardon3d_task_queue_cancel(fixture.state.task_queue, cancelled_id));
  CHECK(wait_state(fixture.state.task_queue, cancelled_id, TASK_CANCELLED,
                   &snapshot));
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION") == 0);

  CHECK(setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  uint64_t resumed_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(&fixture.state, &settings,
                                              &resumed_id));
  CHECK(
      wait_state(fixture.state.task_queue, resumed_id, TASK_PAUSED, &snapshot));
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION") == 0);
  CHECK(lardon3d_task_queue_resume(fixture.state.task_queue, resumed_id));
  CHECK(wait_state(fixture.state.task_queue, resumed_id, TASK_COMPLETED,
                   &snapshot));

  CHECK(setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH", "1", 1) == 0);
  uint64_t pressure_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(&fixture.state, &settings,
                                              &pressure_id));
  CHECK(wait_state(fixture.state.task_queue, pressure_id, TASK_PAUSED,
                   &snapshot));
  Lardon3DResourcePolicy pressure_policy = interactive_policy();
  pressure_policy.system_memory_reserve_bytes =
      fixture.state.hardware_profile.memory_total_bytes - 16ULL * 1024 * 1024;
  pressure_policy.emergency_memory_floor_bytes =
      pressure_policy.system_memory_reserve_bytes;
  CHECK(lardon3d_resource_governor_set_policy(fixture.state.resource_governor,
                                              &pressure_policy));
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH") == 0);
  CHECK(lardon3d_task_queue_resume(fixture.state.task_queue, pressure_id));
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (lardon3d_resource_governor_pressure(fixture.state.resource_governor) ==
        LARDON3D_RESOURCE_PRESSURE_RED) {
      break;
    }
    sched_yield();
  }
  CHECK(lardon3d_resource_governor_pressure(fixture.state.resource_governor) ==
        LARDON3D_RESOURCE_PRESSURE_RED);
  Lardon3DResourcePolicy normal_policy = interactive_policy();
  CHECK(lardon3d_resource_governor_set_policy(fixture.state.resource_governor,
                                              &normal_policy));
  CHECK(wait_state(fixture.state.task_queue, pressure_id, TASK_COMPLETED,
                   &snapshot));

  uint64_t estimate_task_id = 0;
  Lardon3DTask *estimate_task = lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &estimate_task_id);
  Lardon3DTaskDurableSnapshot estimate_snapshot;
  CHECK(estimate_task != NULL && estimate_task_id != 0);
  CHECK(lardon3d_task_durable_snapshot(estimate_task, &estimate_snapshot));
  CHECK(estimate_snapshot.estimate.gpu_memory_fixed_bytes ==
        LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES);
  CHECK(estimate_snapshot.estimate.desired_gpu_slots == 1);
  lardon3d_task_destroy(estimate_task);

  stop_runtime(&fixture);
  CHECK(remove_tree(fixture.root));
  return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
