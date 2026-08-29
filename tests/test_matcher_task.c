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
#include <lardon3d/task_checkpoint.h>
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

void lardon3d_matcher_task_test_reset_backend_counters(void);
size_t lardon3d_matcher_task_test_vulkan_uses(void);
size_t lardon3d_matcher_task_test_forced_fallbacks(void);

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
  bool success =
      sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) ==
          SQLITE_OK &&
      sqlite3_prepare_v2(connection, sql, -1, &statement, NULL) == SQLITE_OK &&
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

static bool candidate_results_have_one_evidence(const char *path,
                                                uint64_t candidate_pair_id) {
  char sql[512];
  int written = snprintf(
      sql, sizeof(sql),
      "SELECT count(*) FROM (SELECT candidate_pair_id FROM match_results "
      "WHERE candidate_pair_id=%lu GROUP BY candidate_pair_id HAVING "
      "count(DISTINCT result_status||':'||match_count||':'||"
      "ifnull(hex(match_asset_sha256),''))<>1)",
      (unsigned long)candidate_pair_id);
  return written > 0 && (size_t)written < sizeof(sql) &&
         query_integer(path, sql, 0);
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
      "DROP TABLE IF EXISTS asset_derivations;"
      "DROP TABLE IF EXISTS capture_selections;"
      "DROP TABLE IF EXISTS capture_assets;"
      "DROP TABLE IF EXISTS capture_images;"
      "DROP TABLE IF EXISTS captures;"
      "DROP TABLE IF EXISTS incremental_reconstruction_tasks;"
      "DROP TABLE IF EXISTS incremental_reconstructions;"
      "DROP TABLE IF EXISTS sparse_sfm_tasks;"
      "DROP TABLE IF EXISTS sparse_landmark_observations;"
      "DROP TABLE IF EXISTS sparse_landmarks;"
      "DROP TABLE IF EXISTS sparse_registered_images;"
      "DROP TABLE IF EXISTS sparse_reconstruction_components;"
      "DROP TABLE IF EXISTS sparse_reconstructions;"
      "DROP TABLE IF EXISTS sparse_calibration_scope_images;"
      "DROP TABLE IF EXISTS sparse_calibration_scopes;"
      "DROP TABLE IF EXISTS sparse_calibrations;"
      "DROP TABLE geometric_verifier_tasks;"
      "DROP TABLE geometric_verification_results;"
      "DROP TABLE matcher_tasks;"
      "DROP TABLE track_observations;"
      "DROP TABLE tracks;"
      "DROP TABLE track_sets;DROP TABLE track_builder_tasks;"
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
  uint32_t feature_count = image_index < 2 ? 769U : 1U;
  Lardon3DFeatureKeypoint *keypoints =
      calloc(feature_count, sizeof(*keypoints));
  unsigned char *descriptors = calloc(feature_count, 32);
  if (!keypoints || !descriptors) {
    free(keypoints);
    free(descriptors);
    return false;
  }
  for (uint32_t index = 0; index < feature_count; ++index) {
    keypoints[index].size = 1.0F;
    /* The first pair is identical and has more than 768 descriptors per side,
     * forcing the audited Vulkan dispatch boundary while retaining a unique
     * zero-distance nearest neighbor for exact CPU/file parity. */
    uint32_t value = image_index < 2 ? index : (uint32_t)image_index;
    for (size_t byte = 0; byte < 32; ++byte) {
      descriptors[(size_t)index * 32 + byte] =
          (unsigned char)((value >> (8U * (byte % 4))) ^ (uint32_t)(byte * 29));
    }
  }
  Lardon3DExtractedFeatures features = {
      .image_width = 64,
      .image_height = 64,
      .feature_count = feature_count,
      .keypoints = keypoints,
      .descriptors = descriptors,
      .descriptor_bytes = (size_t)feature_count * 32,
  };
  Lardon3DProjectDbFeatureSet feature_set;
  bool success = lardon3d_feature_store_publish_v2(
             &fixture->state, fixture->images[image_index].image_id, 0, "orb",
             1, fixture->feature_fingerprint, LARDON3D_FEATURE_DESCRIPTOR_U8,
             32, 0, &features, &feature_set) == LARDON3D_FEATURE_STORE_OK;
  free(keypoints);
  free(descriptors);
  return success;
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

static bool same_estimate(const Lardon3DResourceEstimate *a,
                          const Lardon3DResourceEstimate *b) {
  return a->memory_fixed_bytes == b->memory_fixed_bytes &&
         a->gpu_memory_fixed_bytes == b->gpu_memory_fixed_bytes &&
         a->memory_bytes_per_item == b->memory_bytes_per_item &&
         a->gpu_memory_bytes_per_item == b->gpu_memory_bytes_per_item &&
         a->minimum_batch_size == b->minimum_batch_size &&
         a->maximum_batch_size == b->maximum_batch_size &&
         a->desired_cpu_threads == b->desired_cpu_threads &&
         a->desired_gpu_slots == b->desired_gpu_slots &&
         a->desired_io_slots == b->desired_io_slots &&
         a->task_class == b->task_class;
}

static bool run_completed_with_threads(Fixture *fixture,
                                       Lardon3DMatcherTaskConfiguration settings,
                                       unsigned int threads) {
  char value[16];
  (void)snprintf(value, sizeof(value), "%u", threads);
  if (setenv("LARDON3D_TEST_MATCHER_CPU_THREADS", value, 1) != 0) return false;
  uint64_t task_id = 0;
  Lardon3DTaskSnapshot snapshot;
  Lardon3DProjectDbTask durable;
  bool success = lardon3d_project_enqueue_matcher_task(&fixture->state, &settings,
                                                       &task_id) &&
                 wait_state(fixture->state.task_queue, task_id, TASK_COMPLETED,
                            &snapshot) &&
                 snapshot.progress == 100 &&
                 wait_durable_state(fixture->state.project_db, task_id,
                                    TASK_COMPLETED, &durable);
  return unsetenv("LARDON3D_TEST_MATCHER_CPU_THREADS") == 0 && success;
}

static bool run_failed_at_pair(Fixture *fixture,
                               Lardon3DMatcherTaskConfiguration settings,
                               const char *failure_variable,
                               uint64_t failed_pair_id, size_t expected_prefix,
                               uint64_t expected_cursor) {
  size_t before = 0;
  if (!count_results(fixture, &before)) return false;
  char value[32];
  (void)snprintf(value, sizeof(value), "%lu", (unsigned long)failed_pair_id);
  if (setenv(failure_variable, value, 1) != 0 ||
      setenv("LARDON3D_TEST_MATCHER_CPU_THREADS", "4", 1) != 0) {
    return false;
  }
  uint64_t task_id = 0;
  Lardon3DTaskSnapshot snapshot;
  bool failed = lardon3d_project_enqueue_matcher_task(&fixture->state, &settings,
                                                      &task_id) &&
                wait_state(fixture->state.task_queue, task_id, TASK_FAILED,
                           &snapshot);
  (void)unsetenv(failure_variable);
  (void)unsetenv("LARDON3D_TEST_MATCHER_CPU_THREADS");
  size_t after = 0;
  Lardon3DProjectDbMatcherTask saved;
  bool cursor_saved = false;
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (lardon3d_project_db_load_matcher_task(fixture->state.project_db, task_id,
                                              &saved) == LARDON3D_PROJECT_DB_OK &&
        saved.after_candidate_pair_id == expected_cursor) {
      cursor_saved = true;
      break;
    }
    sched_yield();
  }
  return failed && cursor_saved && count_results(fixture, &after) &&
         after == before + expected_prefix &&
         saved.after_candidate_pair_id == expected_cursor;
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
  CHECK(
      query_integer(database_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='match_results'",
                    1));
  CHECK(
      query_integer(database_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='matcher_tasks'",
                    0));
  CHECK(reopen_runtime(&fixture));
  CHECK(lardon3d_project_db_schema_version(fixture.state.project_db) ==
        LARDON3D_PROJECT_DB_SCHEMA_VERSION);
  CHECK(
      query_integer(database_path,
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
      fixture.state.hardware_profile.memory_total_bytes - 96ULL * 1024 * 1024;
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

  /* Thread-count changes are operational only: all three runs must publish
   * the same Candidate Pair cardinality and identical raw Match evidence. */
  size_t equivalence_before = 0;
  CHECK(count_results(&fixture, &equivalence_before));
  Lardon3DMatcherTaskConfiguration one = settings;
  one.matcher.ratio_threshold = 0.80F;
  CHECK(run_completed_with_threads(&fixture, one, 1));
  Lardon3DMatcherTaskConfiguration two = settings;
  two.matcher.ratio_threshold = 0.81F;
  CHECK(run_completed_with_threads(&fixture, two, 2));
  Lardon3DMatcherTaskConfiguration four = settings;
  four.matcher.ratio_threshold = 0.82F;
  CHECK(run_completed_with_threads(&fixture, four, 4));
  Lardon3DMatcherTaskConfiguration eight = settings;
  eight.matcher.ratio_threshold = 0.84F;
  CHECK(run_completed_with_threads(&fixture, eight, 8));
  size_t equivalence_after = 0;
  CHECK(count_results(&fixture, &equivalence_after));
  CHECK(equivalence_after == equivalence_before + 4 * PERSISTED_PAIR_COUNT);
  CHECK(candidate_results_have_one_evidence(
      database_path, fixture.pairs[0].candidate_pair_id));

  /* Explicit GPU selection is operational only. A real Queue admission must
   * reserve the exact GPU shape and dispatch the first 769x769 ORB pair to
   * Vulkan; all Match evidence remains byte-identical to the CPU runs above. */
#ifdef LARDON3D_MATCHER_TASK_VULKAN
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration gpu = settings;
  gpu.matcher.ratio_threshold = 0.68F;
  uint64_t gpu_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
      &fixture.state, &gpu, LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &gpu_id));
  CHECK(wait_state(fixture.state.task_queue, gpu_id, TASK_COMPLETED, &snapshot));
  Lardon3DProjectDbTask gpu_durable;
  CHECK(wait_durable_state(fixture.state.project_db, gpu_id, TASK_COMPLETED,
                           &gpu_durable));
  CHECK(lardon3d_matcher_task_test_vulkan_uses() >= 1);
  CHECK(candidate_results_have_one_evidence(
      database_path, fixture.pairs[0].candidate_pair_id));
  /* Runtime backend failure after valid GPU admission falls back through the
   * exact CPU implementation; it never changes the immutable estimate or the
   * scientific Match identity. */
  CHECK(setenv("LARDON3D_TEST_MATCHER_FORCE_FALLBACK", "1", 1) == 0);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration fallback = settings;
  fallback.matcher.ratio_threshold = 0.67F;
  uint64_t fallback_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
      &fixture.state, &fallback, LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN,
      &fallback_id));
  CHECK(wait_state(fixture.state.task_queue, fallback_id, TASK_COMPLETED,
                   &snapshot));
  Lardon3DProjectDbTask fallback_durable;
  CHECK(wait_durable_state(fixture.state.project_db, fallback_id,
                           TASK_COMPLETED, &fallback_durable));
  CHECK(lardon3d_matcher_task_test_forced_fallbacks() >= 1);
  CHECK(lardon3d_matcher_task_test_vulkan_uses() == 0);
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_FORCE_FALLBACK") == 0);
  CHECK(candidate_results_have_one_evidence(
      database_path, fixture.pairs[0].candidate_pair_id));
#else
  /* The supported Vulkan-disabled build exposes no backend. An explicit GPU
   * request must fail before Task-ID allocation instead of becoming CPU work. */
  Lardon3DMatcherTaskConfiguration gpu = settings;
  gpu.matcher.ratio_threshold = 0.68F;
  uint64_t gpu_id = 99;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
            &fixture.state, &gpu, LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN,
            &gpu_id) == false &&
        gpu_id == 0);
#endif

  /* The request remains eight, but only two CPUs are available after host
   * headroom. Successful completion exercises the reduced Governor contract. */
  Lardon3DResourcePolicy reduced_policy = interactive_policy();
  reduced_policy.system_cpu_reserve = 14;
  CHECK(lardon3d_resource_governor_set_policy(fixture.state.resource_governor,
                                              &reduced_policy));
  Lardon3DMatcherTaskConfiguration reduced = settings;
  reduced.matcher.ratio_threshold = 0.83F;
  uint64_t reduced_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(&fixture.state, &reduced,
                                              &reduced_id));
  CHECK(wait_state(fixture.state.task_queue, reduced_id, TASK_COMPLETED,
                   &snapshot));
  Lardon3DResourcePolicy restored_policy = interactive_policy();
  CHECK(lardon3d_resource_governor_set_policy(fixture.state.resource_governor,
                                              &restored_policy));

  /* A failed computation seals the ordered suffix. The finished checkpoint
   * may retain only the already durable contiguous prefix. */
  Lardon3DMatcherTaskConfiguration fail_first = settings;
  fail_first.matcher.ratio_threshold = 0.70F;
  CHECK(run_failed_at_pair(&fixture, fail_first,
                           "LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
                           fixture.pairs[0].candidate_pair_id, 0, 0));
  Lardon3DMatcherTaskConfiguration fail_middle = settings;
  fail_middle.matcher.ratio_threshold = 0.71F;
  CHECK(run_failed_at_pair(&fixture, fail_middle,
                           "LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
                           fixture.pairs[5].candidate_pair_id, 4,
                           fixture.pairs[4].candidate_pair_id));
  Lardon3DMatcherTaskConfiguration fail_last = settings;
  fail_last.matcher.ratio_threshold = 0.72F;
  CHECK(run_failed_at_pair(&fixture, fail_last,
                           "LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
                           fixture.pairs[PAIR_COUNT - 1].candidate_pair_id,
                           PERSISTED_PAIR_COUNT - 1,
                           fixture.pairs[PAIR_COUNT - 2].candidate_pair_id));
  Lardon3DMatcherTaskConfiguration publish_first = settings;
  publish_first.matcher.ratio_threshold = 0.73F;
  CHECK(run_failed_at_pair(&fixture, publish_first,
                           "LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
                           fixture.pairs[0].candidate_pair_id, 0, 0));
  Lardon3DMatcherTaskConfiguration publish_middle = settings;
  publish_middle.matcher.ratio_threshold = 0.74F;
  CHECK(run_failed_at_pair(&fixture, publish_middle,
                           "LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
                           fixture.pairs[5].candidate_pair_id, 4,
                           fixture.pairs[4].candidate_pair_id));
  Lardon3DMatcherTaskConfiguration publish_last = settings;
  publish_last.matcher.ratio_threshold = 0.76F;
  CHECK(run_failed_at_pair(&fixture, publish_last,
                           "LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
                           fixture.pairs[PAIR_COUNT - 1].candidate_pair_id,
                           PERSISTED_PAIR_COUNT - 1,
                           fixture.pairs[PAIR_COUNT - 2].candidate_pair_id));

  uint64_t estimate_task_id = 0;
  Lardon3DTask *estimate_task = lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &estimate_task_id);
  Lardon3DTaskDurableSnapshot estimate_snapshot;
  CHECK(estimate_task != NULL && estimate_task_id != 0);
  CHECK(lardon3d_task_durable_snapshot(estimate_task, &estimate_snapshot));
  /* Parallel Matcher execution is permanently CPU-only even if admission
   * later grants one thread; its immutable estimate must reserve no GPU. */
  CHECK(estimate_snapshot.estimate.desired_cpu_threads > 1);
  CHECK(estimate_snapshot.estimate.gpu_memory_fixed_bytes == 0);
  CHECK(estimate_snapshot.estimate.desired_gpu_slots == 0);
  Lardon3DTaskDurableSnapshot current_cpu = estimate_snapshot;
  Lardon3DTaskDurableSnapshot current_gpu = current_cpu;
  current_gpu.estimate.gpu_memory_fixed_bytes =
      LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES;
  current_gpu.estimate.desired_cpu_threads = 1;
  current_gpu.estimate.desired_gpu_slots = 1;
  lardon3d_task_destroy(estimate_task);
  estimate_task = NULL;

#ifdef LARDON3D_MATCHER_TASK_VULKAN
  estimate_task = lardon3d_project_create_matcher_task_with_mode(
      &fixture.state, &settings, LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN,
      &estimate_task_id);
  CHECK(estimate_task != NULL &&
        lardon3d_task_durable_snapshot(estimate_task, &estimate_snapshot));
  CHECK(same_estimate(&estimate_snapshot.estimate, &current_gpu.estimate));
#endif

  /* Selection failures occur before Task-ID allocation and cannot silently
   * become CPU work. The existing create API remains CPU-parallel. */
  Lardon3DMatcherTaskConfiguration wrong_kind = settings;
  wrong_kind.matcher.kind = LARDON3D_MATCHER_SIFT_BF;
  (void)snprintf(wrong_kind.feature_extractor_kind,
                 sizeof(wrong_kind.feature_extractor_kind), "sift");
  uint64_t rejected_id = 99;
  CHECK(lardon3d_project_create_matcher_task_with_mode(
            &fixture.state, &wrong_kind,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &rejected_id) == NULL &&
        rejected_id == 0);
  rejected_id = 99;
  CHECK(lardon3d_project_create_matcher_task_with_mode(
            &fixture.state, &settings, (Lardon3DMatcherTaskMode)99,
            &rejected_id) == NULL &&
        rejected_id == 0);
  bool gpu_available = fixture.state.hardware_profile.gpu_available;
  fixture.state.hardware_profile.gpu_available = false;
  rejected_id = 99;
  CHECK(lardon3d_project_create_matcher_task_with_mode(
            &fixture.state, &settings,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &rejected_id) == NULL &&
        rejected_id == 0);
  fixture.state.hardware_profile.gpu_available = gpu_available;
  Lardon3DOrbVulkanBackend *backend = fixture.state.orb_vulkan_backend;
  fixture.state.orb_vulkan_backend = NULL;
  rejected_id = 99;
  CHECK(lardon3d_project_create_matcher_task_with_mode(
            &fixture.state, &settings,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &rejected_id) == NULL &&
        rejected_id == 0);
  fixture.state.orb_vulkan_backend = backend;

  /* Reconstruction accepts only the two current and two historical complete
   * shapes. CPU12 signatures are normalized ephemerally for Task admission;
   * neighboring shapes are corruption, not mode inference. */
  Lardon3DTaskReconstructionContext reconstruction = {
      .project_path = fixture.state.project_path,
      .project_db = fixture.state.project_db,
      .resource_governor = fixture.state.resource_governor,
      .orb_vulkan_backend = fixture.state.orb_vulkan_backend,
  };
  Lardon3DTaskDurableSnapshot historical_cpu = current_cpu;
  historical_cpu.estimate.memory_fixed_bytes = 10U * 1024U * 1024U;
  historical_cpu.estimate.memory_bytes_per_item = 0;
  historical_cpu.estimate.desired_cpu_threads = 12;
  Lardon3DTaskDurableSnapshot historical_gpu = current_gpu;
  historical_gpu.estimate.memory_fixed_bytes = 10U * 1024U * 1024U;
  historical_gpu.estimate.memory_bytes_per_item = 0;
  historical_gpu.estimate.desired_cpu_threads = 12;
  char reconciled_path[PATH_MAX];
  CHECK(snprintf(reconciled_path, sizeof(reconciled_path),
                 "%s/.lardon3d/checkpoints/%lu.chk",
                 fixture.state.project_path,
                 (unsigned long)estimate_task_id) > 0 &&
        lardon3d_task_checkpoint_save(reconciled_path, &historical_gpu) ==
            LARDON3D_TASK_CHECKPOINT_OK);
  const Lardon3DTaskDurableSnapshot *accepted[] = {
      &current_cpu, &current_gpu, &historical_cpu, &historical_gpu};
  for (size_t index = 0; index < sizeof(accepted) / sizeof(accepted[0]); ++index) {
    Lardon3DTask *restored = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              LARDON3D_MATCHER_TASK_KIND, LARDON3D_MATCHER_TASK_KIND_VERSION,
              accepted[index], &reconstruction, &restored) ==
              LARDON3D_TASK_KIND_OK &&
          restored);
    Lardon3DResourceEstimate restored_estimate;
    CHECK(lardon3d_task_resource_estimate(restored, &restored_estimate));
    const Lardon3DResourceEstimate *expected =
        index == 2 ? &current_cpu.estimate
                   : index == 3 ? &current_gpu.estimate
                                : &accepted[index]->estimate;
    CHECK(same_estimate(&restored_estimate, expected));
    lardon3d_task_destroy(restored);
  }
  Lardon3DTaskDurableSnapshot reconciled_snapshot;
  uint32_t reconciled_version = 0;
  CHECK(lardon3d_task_checkpoint_load(reconciled_path, &reconciled_snapshot,
                                      &reconciled_version) ==
            LARDON3D_TASK_CHECKPOINT_OK &&
        reconciled_version == LARDON3D_TASK_CHECKPOINT_VERSION &&
        same_estimate(&reconciled_snapshot.estimate, &historical_gpu.estimate));
  char staged_path[PATH_MAX];
  CHECK(snprintf(staged_path, sizeof(staged_path), "%s.next",
                 reconciled_path) > 0 &&
        access(staged_path, F_OK) != 0 && errno == ENOENT);
  Lardon3DTaskDurableSnapshot malformed[4] = {
      historical_cpu, historical_cpu, historical_gpu, historical_gpu};
  malformed[0].estimate.memory_fixed_bytes--;
  malformed[1].estimate.memory_bytes_per_item = 1;
  malformed[2].estimate.gpu_memory_fixed_bytes++;
  malformed[3].estimate.desired_cpu_threads = 11;
  for (size_t index = 0; index < sizeof(malformed) / sizeof(malformed[0]); ++index) {
    Lardon3DTask *restored = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              LARDON3D_MATCHER_TASK_KIND, LARDON3D_MATCHER_TASK_KIND_VERSION,
              &malformed[index], &reconstruction, &restored) ==
              LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED &&
          restored == NULL);
  }
  lardon3d_task_destroy(estimate_task);

  stop_runtime(&fixture);
  CHECK(remove_tree(fixture.root));
  return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
