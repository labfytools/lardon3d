#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/types.h>
#endif
#include <unistd.h>

#include <lardon3d/feature_extractor.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/hardware_profile.h>
#include <lardon3d/matcher_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>

#include "../src/matcher_task_benchmark_internal.h"
#include "../src/orb_vulkan_backend_internal.h"
#include "../src/resource_governor_internal.h"

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
  /* The fixture makes twelve adjacent pairs eligible, then deliberately
   * deletes eligible pair index two to preserve sparse Candidate Pair IDs. */
  PIPELINE_ELIGIBLE_PAIR_COUNT = 11,
};

void lardon3d_matcher_task_test_reset_backend_counters(void);
size_t lardon3d_matcher_task_test_vulkan_uses(void);
size_t lardon3d_matcher_task_test_forced_fallbacks(void);
size_t lardon3d_matcher_task_test_overlap_publications(void);
uint64_t lardon3d_matcher_task_test_max_retained_vulkan_payload(void);
size_t lardon3d_matcher_task_test_event_count(void);
bool lardon3d_matcher_task_test_event(size_t index, int *kind,
                                     uint64_t *candidate_pair_id,
                                     size_t *order);
bool lardon3d_matcher_task_test_auto_capability_envelope(
    size_t benchmark_inflight_override, size_t benchmark_batch_override,
    Lardon3DTaskCapabilityEnvelope *envelope);

enum {
  TEST_EVENT_GPU_SUBMIT = 1,
  TEST_EVENT_GPU_FINISH = 2,
  TEST_EVENT_PUBLICATION_START = 3,
  TEST_EVENT_PUBLICATION_FINISH = 4,
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

#ifdef LARDON3D_MATCHER_TASK_VULKAN
static bool matcher_capability_boundary_case(
    const Lardon3DTaskCapabilityEnvelope *envelope, uint64_t total_bytes,
    uint64_t available_bytes, Lardon3DResourceDecisionKind expected_decision,
    Lardon3DResourceBackend expected_backend, size_t expected_batch,
    uint64_t expected_memory_bytes, uint64_t expected_gpu_memory_bytes) {
  const uint64_t gib = UINT64_C(1024) * 1024 * 1024;
  Lardon3DHardwareProfile profile = {
      .logical_cpu_count = 16,
      .page_size_bytes = 4096,
      .memory_total_bytes = total_bytes,
      .gpu_available = true,
      .gpu_uses_shared_memory = true,
      .cpu_architecture = "boundary-test",
  };
  Lardon3DResourcePolicy policy = {
      .system_memory_reserve_bytes = 3 * gib,
      .emergency_memory_floor_bytes = 2 * gib,
      .system_cpu_reserve = 0,
      .maximum_cpu_load_ratio = 1.0,
      .maximum_cpu_pressure_avg10 = 100.0,
      .maximum_memory_pressure_avg10 = 100.0,
      .maximum_io_pressure_avg10 = 100.0,
      .gpu_slot_capacity = 1,
      .io_slot_capacity = 1,
  };
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  if (!governor ||
      !lardon3d_resource_governor_internal_set_backend_available(
          governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true)) {
    lardon3d_resource_governor_destroy(governor);
    return false;
  }
  Lardon3DResourceSnapshot snapshot = {
      .memory_available_bytes = available_bytes,
      .swap_activity_known = true,
  };
  if (clock_gettime(CLOCK_MONOTONIC, &snapshot.captured_at) != 0) {
    lardon3d_resource_governor_destroy(governor);
    return false;
  }
  Lardon3DResourceCapabilitySelection selection = {0};
  Lardon3DResourceReservation *reservation = NULL;
  bool success = lardon3d_resource_governor_internal_reserve_capability(
      governor, &snapshot, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, envelope, &selection, &reservation);
  bool admitting = expected_decision == LARDON3D_RESOURCE_START ||
                   expected_decision == LARDON3D_RESOURCE_REDUCE_BATCH;
  success = success && selection.decision.kind == expected_decision &&
            ((admitting && reservation != NULL &&
              selection.capability.backend == expected_backend &&
              selection.decision.batch_size == expected_batch) ||
             (!admitting && reservation == NULL));
  if (!success) {
    fprintf(stderr,
            "boundary actual decision=%d backend=%d batch=%zu reservation=%s; "
            "expected decision=%d backend=%d batch=%zu\n",
            (int)selection.decision.kind, (int)selection.capability.backend,
            selection.decision.batch_size, reservation ? "yes" : "no",
            (int)expected_decision, (int)expected_backend, expected_batch);
  }
  if (success && reservation) {
    Lardon3DResourceReservationInfo information;
    success = lardon3d_resource_reservation_get_active(
                  governor, reservation, &information) &&
              information.memory_bytes == expected_memory_bytes &&
              information.gpu_memory_bytes == expected_gpu_memory_bytes &&
              lardon3d_resource_governor_release(governor, reservation);
    reservation = NULL;
  }
  if (reservation) {
    (void)lardon3d_resource_governor_release(governor, reservation);
  }
  lardon3d_resource_governor_destroy(governor);
  return success;
}
#endif

static bool matcher_exact_memory_boundary_test(void) {
#ifdef LARDON3D_MATCHER_TASK_VULKAN
  const uint64_t gib = UINT64_C(1024) * 1024 * 1024;
  const uint64_t per_pair = UINT64_C(10) * 1024 * 1024;
  const uint64_t per_slot = UINT64_C(640) * 1024;
  const uint64_t normal_window = 8 * per_pair + per_slot;
  const uint64_t normal_total = 3 * gib + normal_window;
  const uint64_t normal_gpu_minimum = 3 * gib + per_pair + per_slot;
  const uint64_t normal_cpu_minimum = 3 * gib + per_pair;
  Lardon3DTaskCapabilityEnvelope normal;
  CHECK(lardon3d_matcher_task_test_auto_capability_envelope(0, 0, &normal));
  CHECK(normal.count == 2 &&
        normal.capabilities[0].backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        normal.capabilities[0].estimate.maximum_batch_size == 8 &&
        normal.capabilities[0].sustained_gpu_batch_feedback &&
        normal.capabilities[0].inflight_limit == 1 &&
        normal.capabilities[0].minimum_inflight_limit == 1 &&
        normal.capabilities[0].gpu_memory_bytes_per_inflight == per_slot &&
        normal_total < 3 * gib + 12 * per_pair + 2 * per_slot);
  /* The exact maximum proves that the old depth-two/batch-twelve prefilter is
   * gone. Falling below that maximum must not reject an adaptive capability:
   * AUTO can still admit the immutable batch-one contract. Only the exact
   * minimum boundaries select CPU fallback and finally reject both arms. */
  CHECK(matcher_capability_boundary_case(
      &normal, normal_total, normal_total, LARDON3D_RESOURCE_START,
      LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, 1, per_pair, per_slot));
  CHECK(matcher_capability_boundary_case(
      &normal, normal_total - 1, normal_total - 1,
      LARDON3D_RESOURCE_START, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, 1,
      per_pair, per_slot));
  CHECK(matcher_capability_boundary_case(
      &normal, normal_gpu_minimum - 1, normal_gpu_minimum - 1,
      LARDON3D_RESOURCE_START, LARDON3D_RESOURCE_BACKEND_CPU, 1, per_pair, 0));
  CHECK(matcher_capability_boundary_case(
      &normal, normal_cpu_minimum - 1, normal_cpu_minimum - 1,
      LARDON3D_RESOURCE_REJECT, LARDON3D_RESOURCE_BACKEND_FIXED, 0, 0, 0));
#if defined(LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE)
  const uint64_t forced_window = 2 * per_pair + per_slot;
  const uint64_t forced_total = 3 * gib + forced_window;
  Lardon3DTaskCapabilityEnvelope forced;
  CHECK(lardon3d_matcher_task_test_auto_capability_envelope(1, 2, &forced));
  CHECK(forced.count == 1 &&
        forced.capabilities[0].backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        forced.capabilities[0].estimate.minimum_batch_size == 2 &&
        forced.capabilities[0].estimate.maximum_batch_size == 2 &&
        !forced.capabilities[0].sustained_gpu_batch_feedback &&
        forced.capabilities[0].inflight_limit == 1 &&
        forced.capabilities[0].minimum_inflight_limit == 1 &&
        forced_total < 3 * gib + 12 * per_pair + 2 * per_slot);
  CHECK(matcher_capability_boundary_case(
      &forced, forced_total, forced_total, LARDON3D_RESOURCE_START,
      LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, 2, 2 * per_pair, per_slot));
  CHECK(matcher_capability_boundary_case(
      &forced, forced_total - 1, forced_total - 1,
      LARDON3D_RESOURCE_REJECT, LARDON3D_RESOURCE_BACKEND_FIXED, 0, 0, 0));
  /* Exact sizing never weakens the 2 GiB hard floor: an otherwise valid
   * forced capability waits and owns no reservation at that current snapshot. */
  CHECK(matcher_capability_boundary_case(
      &forced, forced_total, 2 * gib, LARDON3D_RESOURCE_WAIT,
      LARDON3D_RESOURCE_BACKEND_FIXED, 0, 0, 0));
  Lardon3DTaskCapabilityEnvelope forced_twelve;
  CHECK(lardon3d_matcher_task_test_auto_capability_envelope(
            1, 12, &forced_twelve) &&
        forced_twelve.count == 1 &&
        forced_twelve.capabilities[0].estimate.minimum_batch_size == 12 &&
        forced_twelve.capabilities[0].estimate.maximum_batch_size == 12 &&
        !forced_twelve.capabilities[0].batch_adaptive &&
        !forced_twelve.capabilities[0].sustained_gpu_batch_feedback);
#endif
#endif
  return true;
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

#ifdef LARDON3D_MATCHER_TASK_VULKAN
static bool wait_candidate_results_have_one_evidence(
    const char *path, uint64_t candidate_pair_id) {
  /* Queue state may become terminal immediately before its finished callback
   * releases the DB transaction. Retry the observable read boundary without
   * timing sleeps; a real duplicate never converges to one scientific value. */
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (candidate_results_have_one_evidence(path, candidate_pair_id)) {
      return true;
    }
    sched_yield();
  }
  return false;
}

static bool find_test_event(int wanted_kind, uint64_t wanted_pair,
                            size_t *order) {
  if (!order) return false;
  size_t count = lardon3d_matcher_task_test_event_count();
  for (size_t index = 0; index < count; ++index) {
    int kind = 0;
    uint64_t pair = 0;
    size_t candidate_order = 0;
    if (lardon3d_matcher_task_test_event(index, &kind, &pair,
                                        &candidate_order) &&
        kind == wanted_kind && pair == wanted_pair) {
      *order = candidate_order;
      return true;
    }
  }
  return false;
}

static bool staged_match_temporaries_absent_once(const Fixture *fixture) {
  char assets[PATH_MAX];
  char matches[PATH_MAX];
  if (!join_path(assets, fixture->state.project_path, "assets") ||
      !join_path(matches, assets, "matches")) {
    return false;
  }
  DIR *directory = opendir(matches);
  if (!directory) return false;
  bool clean = true;
  for (struct dirent *entry = readdir(directory); entry;
       entry = readdir(directory)) {
    if (strncmp(entry->d_name, ".match-", 7) == 0) {
      clean = false;
      break;
    }
  }
  return closedir(directory) == 0 && clean;
}

static bool no_staged_match_temporaries(const Fixture *fixture) {
  /* Task state may become FAILED before the callback unwinds its private stage
   * cleanup. Poll the observable filesystem boundary without sleeps; Queue's
   * single owner must converge before another Task can consume the slot. */
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (staged_match_temporaries_absent_once(fixture)) return true;
    sched_yield();
  }
  return false;
}
#endif

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
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return false;
  /* ASan/UBSan intentionally makes the real Vulkan fixture much slower. A
   * wall-clock bound tests eventual Queue state without turning main-thread
   * polling speed into an accidental timeout contract. */
  deadline.tv_sec += 30;
  for (;;) {
    if (lardon3d_task_queue_get(queue, task_id, snapshot) &&
        snapshot->state == wanted) {
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

static bool wait_durable_state(Lardon3DProjectDb *database, uint64_t task_id,
                               Lardon3DTaskState wanted,
                               Lardon3DProjectDbTask *durable_task) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return false;
  deadline.tv_sec += 30;
  for (;;) {
    if (lardon3d_project_db_load_task(database, task_id, durable_task) ==
            LARDON3D_PROJECT_DB_OK &&
        durable_task->saved_state == wanted) {
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
  const char *pipeline_fixture = getenv("LARDON3D_TEST_MATCHER_PIPELINE_FIXTURE");
  const size_t eligible_image_count =
      pipeline_fixture && strcmp(pipeline_fixture, "1") == 0 ? 13U : 3U;
  uint32_t feature_count = image_index < eligible_image_count ? 769U : 1U;
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
    /* The first two pairs have more than 768 descriptors per side, forcing
     * consecutive audited Vulkan submissions while retaining bounded fixture
     * memory and deterministic CPU/file parity. */
    uint32_t value = image_index < eligible_image_count
        ? index : (uint32_t)image_index;
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
  bool success = lardon3d_project_enqueue_matcher_task_with_mode(
                     &fixture->state, &settings,
                     LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL, &task_id) &&
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
  bool failed = lardon3d_project_enqueue_matcher_task_with_mode(
                    &fixture->state, &settings,
                    LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL, &task_id) &&
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

#if defined(LARDON3D_MATCHER_TASK_VULKAN) && \
    defined(LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE)
typedef struct {
  size_t count;
  Lardon3DProjectDbMatchResult results[PAIR_COUNT];
  Lardon3DResourceSequenceDiagnostic diagnostic;
  Lardon3DResourceSequenceAggregate aggregate;
  size_t overlap_publications;
} PipelineEvidence;

#ifdef __linux__
enum { PROCESS_THREAD_SNAPSHOT_CAPACITY = 1024 };

typedef struct {
  size_t count;
  pid_t tids[PROCESS_THREAD_SNAPSHOT_CAPACITY];
} ProcessThreadSnapshot;

static bool parse_task_tid(const char *text, pid_t *tid) {
  if (!text || !tid || text[0] < '0' || text[0] > '9') return false;
  unsigned long value = 0;
  const char *cursor = text;
  do {
    unsigned int digit = (unsigned int)(*cursor - '0');
    if (value > (unsigned long)(INT_MAX - (int)digit) / 10) return false;
    value = value * 10 + digit;
    ++cursor;
  } while (*cursor >= '0' && *cursor <= '9');
  if (*cursor != '\0' || value == 0) return false;
  *tid = (pid_t)value;
  return true;
}

static bool capture_process_thread_snapshot(ProcessThreadSnapshot *snapshot) {
  if (!snapshot) return false;
  *snapshot = (ProcessThreadSnapshot){0};
  DIR *directory = opendir("/proc/self/task");
  if (!directory) return false;
  bool valid = true;
  for (struct dirent *entry = readdir(directory); entry;
       entry = readdir(directory)) {
    pid_t tid = 0;
    if (!parse_task_tid(entry->d_name, &tid)) continue;
    if (snapshot->count == PROCESS_THREAD_SNAPSHOT_CAPACITY) {
      valid = false;
      break;
    }
    snapshot->tids[snapshot->count++] = tid;
  }
  return closedir(directory) == 0 && valid;
}

static bool thread_snapshot_contains(
    const ProcessThreadSnapshot *snapshot, pid_t tid) {
  for (size_t index = 0; index < snapshot->count; ++index) {
    if (snapshot->tids[index] == tid) return true;
  }
  return false;
}

static bool read_thread_comm(pid_t tid, char comm[64]) {
  char path[128];
  int written = snprintf(path, sizeof(path), "/proc/self/task/%ld/comm",
                         (long)tid);
  if (written <= 0 || (size_t)written >= sizeof(path)) return false;
  int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  ssize_t count = read(descriptor, comm, 63);
  bool valid = count > 0 && count < 63 && read(descriptor, comm + count, 1) == 0;
  if (close(descriptor) != 0) valid = false;
  if (!valid) return false;
  size_t length = (size_t)count;
  while (length > 0 &&
         (comm[length - 1] == '\n' || comm[length - 1] == '\r')) {
    --length;
  }
  comm[length] = '\0';
  return true;
}

static bool read_thread_cpu_list(pid_t tid, char output[256]) {
  char path[128];
  int written = snprintf(path, sizeof(path), "/proc/self/task/%ld/status",
                         (long)tid);
  if (written <= 0 || (size_t)written >= sizeof(path)) return false;
  int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  char status[16384];
  ssize_t count = read(descriptor, status, sizeof(status) - 1);
  bool valid = count > 0 && count < (ssize_t)(sizeof(status) - 1) &&
               read(descriptor, status + count, 1) == 0;
  int saved_error = errno;
  if (close(descriptor) != 0) valid = false;
  if (!valid) {
    errno = saved_error;
    return false;
  }
  status[count] = '\0';
  static const char prefix[] = "Cpus_allowed_list:";
  char *line = strstr(status, prefix);
  if (!line || (line != status && line[-1] != '\n')) return false;
  line += sizeof(prefix) - 1;
  while (*line == ' ' || *line == '\t') ++line;
  char *end = strchr(line, '\n');
  if (!end) return false;
  while (end > line && (end[-1] == ' ' || end[-1] == '\t' ||
                        end[-1] == '\r')) {
    --end;
  }
  size_t length = (size_t)(end - line);
  if (length == 0 || length >= 256) return false;
  memcpy(output, line, length);
  output[length] = '\0';
  return true;
}

static bool format_cpu_list(
    const uint64_t mask[LARDON3D_RESOURCE_CPU_MASK_WORDS], char output[256]) {
  size_t used = 0;
  bool first = true;
  for (unsigned int cpu = 0; cpu < LARDON3D_RESOURCE_CPU_MAX;) {
    if ((mask[cpu / 64] & (UINT64_C(1) << (cpu % 64))) == 0) {
      ++cpu;
      continue;
    }
    unsigned int begin = cpu;
    while (cpu + 1 < LARDON3D_RESOURCE_CPU_MAX &&
           (mask[(cpu + 1) / 64] &
            (UINT64_C(1) << ((cpu + 1) % 64))) != 0) {
      ++cpu;
    }
    unsigned int end = cpu;
    int written = snprintf(output + used, 256 - used,
                           first ? begin == end ? "%u" : "%u-%u"
                                 : begin == end ? ",%u" : ",%u-%u",
                           begin, end);
    if (written <= 0 || (size_t)written >= 256 - used) return false;
    used += (size_t)written;
    first = false;
    ++cpu;
  }
  return !first;
}

static bool verify_process_heavy_affinity(
    Lardon3DResourceGovernor *governor,
    const ProcessThreadSnapshot *before_threads,
    const cpu_set_t *main_before) {
  Lardon3DResourceCpuPolicyDiagnostic diagnostic;
  if (!governor || !before_threads || !main_before
      || !lardon3d_resource_governor_internal_cpu_policy(
          governor, &diagnostic)
      || !diagnostic.affinity_active
      || !diagnostic.runtime_thread_policy_active
      || !diagnostic.mesa_shader_cache_disabled) {
    return false;
  }
  char expected_cpu_list[256];
  if (!format_cpu_list(diagnostic.compute_mask, expected_cpu_list)) {
    return false;
  }
  DIR *directory = opendir("/proc/self/task");
  if (!directory) return false;
  bool valid = true;
  size_t non_main = 0;
  size_t mesa_disk_threads = 0;
  pid_t main_tid = getpid();
  char main_comm[64];
  char main_cpu_list[256];
  if (!read_thread_comm(main_tid, main_comm)
      || !read_thread_cpu_list(main_tid, main_cpu_list)) {
    valid = false;
  } else {
    (void)fprintf(stdout,
                  "thread_affinity_evidence tid=%ld comm=%s cpus=%s "
                  "role=main\n",
                  (long)main_tid, main_comm, main_cpu_list);
  }
  for (struct dirent *entry = readdir(directory); entry;
       entry = readdir(directory)) {
    pid_t tid = 0;
    if (!parse_task_tid(entry->d_name, &tid) || tid == main_tid) continue;
    char comm[64];
    char cpu_list[256];
    errno = 0;
    if (!read_thread_comm(tid, comm) || !read_thread_cpu_list(tid, cpu_list)) {
      if (errno == ESRCH || errno == ENOENT) continue;
      valid = false;
      break;
    }
    ++non_main;
    bool created_after_snapshot = !thread_snapshot_contains(before_threads, tid);
    (void)fprintf(stdout,
                  "thread_affinity_evidence tid=%ld comm=%s cpus=%s "
                  "role=runtime new=%s\n",
                  (long)tid, comm, cpu_list,
                  created_after_snapshot ? "true" : "false");
    if (strcmp(cpu_list, expected_cpu_list) != 0) valid = false;
    if (strstr(comm, ":disk$") != NULL) ++mesa_disk_threads;
    if (!valid) break;
  }
  if (closedir(directory) != 0) valid = false;
  cpu_set_t main_after;
  CPU_ZERO(&main_after);
  if (sched_getaffinity(0, sizeof(main_after), &main_after) != 0
      || !CPU_EQUAL(main_before, &main_after)) {
    valid = false;
  }
  /* The Queue worker is always present. On the verified 780M host, disabling
   * Mesa's disk cache must eliminate the known affinity-widening disk helpers;
   * every remaining live runtime thread must retain the worker compute pool. */
  Lardon3DHardwareProfile host;
  bool current_780m = lardon3d_hardware_profile_detect(&host, NULL, 0)
      && host.gpu_drm_card_index == 1 && host.gpu_uses_shared_memory
      && host.gpu_memory_known
      && host.gpu_memory_total_bytes == UINT64_C(536870912);
  if (current_780m && mesa_disk_threads != 0) {
    valid = false;
  }
  return valid && non_main >= 1;
}
#else
typedef struct {
  bool unused;
} ProcessThreadSnapshot;

static bool capture_process_thread_snapshot(ProcessThreadSnapshot *snapshot) {
  if (!snapshot) return false;
  snapshot->unused = false;
  return true;
}

static bool verify_process_heavy_affinity(
    Lardon3DResourceGovernor *governor,
    const ProcessThreadSnapshot *before_threads,
    const cpu_set_t *main_before) {
  (void)governor;
  (void)before_threads;
  (void)main_before;
  return true;
}
#endif

static bool capture_pipeline_evidence(bool synchronous,
                                      unsigned int inflight_override,
                                      unsigned int batch_override,
                                      PipelineEvidence *evidence) {
  if (!evidence || (inflight_override != 1 && inflight_override != 2) ||
      (batch_override != 0 && batch_override != 2 && batch_override != 4 &&
       batch_override != 8 && batch_override != 12) ||
      (synchronous && (inflight_override != 1 || batch_override != 0)))
    return false;
  Fixture fixture;
  cpu_set_t main_before;
  CPU_ZERO(&main_before);
  if (sched_getaffinity(0, sizeof(main_before), &main_before) != 0) return false;
  /* Keep enough consecutive eligible pairs for AUTO slow-start to reach a
   * rolling batch greater than one without making the main CPU fixture heavy. */
  if (setenv("LARDON3D_TEST_MATCHER_PIPELINE_FIXTURE", "1", 1) != 0 ||
      !fixture_create(&fixture) ||
      unsetenv("LARDON3D_TEST_MATCHER_PIPELINE_FIXTURE") != 0)
    return false;
  ProcessThreadSnapshot before_threads;
  if (!capture_process_thread_snapshot(&before_threads)) {
    stop_runtime(&fixture);
    (void)remove_tree(fixture.root);
    return false;
  }
  const char *inflight = inflight_override == 1 ? "1" : "2";
  char batch[3];
  int batch_length = snprintf(batch, sizeof(batch), "%u", batch_override);
  bool configured =
      (synchronous
           ? setenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV, "1", 1)
           : unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV)) == 0 &&
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV, inflight, 1) == 0 &&
      (batch_override != 0
           ? batch_length > 0 && (size_t)batch_length < sizeof(batch) &&
                 setenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV, batch, 1) == 0
           : unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0) &&
      unsetenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT") == 0;
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration settings = configuration(&fixture);
  uint64_t task_id = 0;
  Lardon3DTaskSnapshot snapshot;
  bool completed = configured && lardon3d_project_enqueue_matcher_task(
                       &fixture.state, &settings, &task_id) &&
                   wait_state(fixture.state.task_queue, task_id,
                              TASK_COMPLETED, &snapshot);
  bool environment_restored =
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV) == 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0;
  completed = environment_restored && completed;
  uint64_t cursor = 0;
  while (completed && evidence->count < PAIR_COUNT) {
    size_t count = 0;
    completed = lardon3d_project_db_list_match_results(
                    fixture.state.project_db, cursor,
                    evidence->results + evidence->count,
                    PAIR_COUNT - evidence->count, &count) ==
                LARDON3D_PROJECT_DB_OK;
    if (!completed || count == 0) break;
    evidence->count += count;
    cursor = evidence->results[evidence->count - 1].match_result_id;
  }
  evidence->overlap_publications =
      lardon3d_matcher_task_test_overlap_publications();
  completed = completed && evidence->count == PERSISTED_PAIR_COUNT &&
      lardon3d_matcher_task_test_vulkan_uses() >= 2 &&
      lardon3d_resource_governor_internal_last_diagnostic(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &evidence->diagnostic) &&
      lardon3d_resource_governor_internal_sequence_aggregate(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &evidence->aggregate) &&
      evidence->diagnostic.backend ==
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
      evidence->diagnostic.cpu_threads == 1 &&
      evidence->diagnostic.gpu_slots == 1 &&
      evidence->diagnostic.helper_limit == 0 &&
      evidence->diagnostic.io_slots == 1 &&
      evidence->diagnostic.batch_size ==
          (batch_override != 0 ? batch_override : 2) &&
      evidence->diagnostic.memory_bytes ==
          (uint64_t)(batch_override != 0 ? batch_override : 2) *
              10U * 1024U * 1024U &&
      evidence->diagnostic.inflight_limit == inflight_override &&
      evidence->diagnostic.gpu_memory_bytes ==
          inflight_override * 640U * 1024U &&
      evidence->aggregate.admission_count > 0 &&
      evidence->aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_CPU] == 0 &&
      evidence->aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] ==
          evidence->aggregate.admission_count &&
      evidence->aggregate.contract_change_count == 0 &&
      evidence->aggregate.backend_failure_fallback_sequences == 0 &&
      evidence->aggregate.backend_other_fallback_sequences == 0 &&
      evidence->aggregate.local_ineligible_fallback_items ==
          PERSISTED_PAIR_COUNT - PIPELINE_ELIGIBLE_PAIR_COUNT &&
      evidence->aggregate.backend_failure_fallback_items == 0 &&
      evidence->aggregate.backend_other_fallback_items == 0 &&
      verify_process_heavy_affinity(
          fixture.state.resource_governor, &before_threads, &main_before);
  stop_runtime(&fixture);
  return remove_tree(fixture.root) && completed;
}

static bool same_pipeline_scientific_output(
    const PipelineEvidence *rolling, const PipelineEvidence *synchronous) {
  if (rolling->count != synchronous->count) return false;
  for (size_t index = 0; index < rolling->count; ++index) {
    const Lardon3DProjectDbMatchResult *left = &rolling->results[index];
    const Lardon3DProjectDbMatchResult *right = &synchronous->results[index];
    if (left->candidate_pair_id != right->candidate_pair_id ||
        left->feature_set_id_a != right->feature_set_id_a ||
        left->feature_set_id_b != right->feature_set_id_b ||
        strcmp(left->matcher_kind, right->matcher_kind) != 0 ||
        left->matcher_version != right->matcher_version ||
        memcmp(left->parameter_fingerprint, right->parameter_fingerprint,
               sizeof(left->parameter_fingerprint)) != 0 ||
        left->result_status != right->result_status ||
        left->match_count != right->match_count ||
        left->has_match_asset != right->has_match_asset ||
        memcmp(left->match_asset_sha256, right->match_asset_sha256,
               sizeof(left->match_asset_sha256)) != 0 ||
        left->match_asset_size_bytes != right->match_asset_size_bytes)
      return false;
  }
  return true;
}

static bool same_forced_contract_except_depth(
    const PipelineEvidence *depth_one, const PipelineEvidence *depth_two) {
  const Lardon3DResourceSequenceDiagnostic *one = &depth_one->diagnostic;
  const Lardon3DResourceSequenceDiagnostic *two = &depth_two->diagnostic;
  return one->backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
         two->backend == one->backend && one->cpu_threads == 1 &&
         two->cpu_threads == one->cpu_threads && one->gpu_slots == 1 &&
         two->gpu_slots == one->gpu_slots && one->batch_size == 2 &&
         two->batch_size == one->batch_size && one->helper_limit == 0 &&
         two->helper_limit == one->helper_limit &&
         two->io_slots == one->io_slots && two->memory_bytes == one->memory_bytes &&
         one->inflight_limit == 1 && two->inflight_limit == 2 &&
         one->gpu_memory_bytes == 640U * 1024U &&
         two->gpu_memory_bytes == 2U * 640U * 1024U;
}

static bool same_forced_contract_except_batch(
    const PipelineEvidence *batch_two, const PipelineEvidence *other,
    size_t expected_batch) {
  const Lardon3DResourceSequenceDiagnostic *two = &batch_two->diagnostic;
  const Lardon3DResourceSequenceDiagnostic *candidate = &other->diagnostic;
  return two->backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
         candidate->backend == two->backend && two->cpu_threads == 1 &&
         candidate->cpu_threads == two->cpu_threads && two->gpu_slots == 1 &&
         candidate->gpu_slots == two->gpu_slots && two->batch_size == 2 &&
         candidate->batch_size == expected_batch && two->helper_limit == 0 &&
         candidate->helper_limit == two->helper_limit &&
         candidate->io_slots == two->io_slots && two->inflight_limit == 1 &&
         candidate->inflight_limit == two->inflight_limit &&
         two->gpu_memory_bytes == 640U * 1024U &&
         candidate->gpu_memory_bytes == two->gpu_memory_bytes &&
         two->memory_bytes == 2U * 10U * 1024U * 1024U &&
         candidate->memory_bytes == expected_batch * 10U * 1024U * 1024U;
}

static bool forced_benchmark_fail_closed(void) {
  if (unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) != 0 ||
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV, "2", 1) != 0)
    return false;
  Fixture fixture;
  if (!fixture_create(&fixture)) {
    (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV);
    return false;
  }
  Lardon3DMatcherTaskConfiguration settings = configuration(&fixture);
  uint64_t rejected_id = 0;
  /* Build/GPU/backend are the only creation metadata. Memory failure belongs
   * to the exact Governor boundary test above, not a divergent AppState copy. */
  fixture.state.hardware_profile.gpu_available = false;
  Lardon3DTask *gpu_rejected = lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &rejected_id);
  fixture.state.hardware_profile.gpu_available = true;
  bool ok = !gpu_rejected;
  if (!ok) fprintf(stderr, "forced benchmark pre-admission rejection failed\n");
  if (gpu_rejected) lardon3d_task_destroy(gpu_rejected);

  /* Backend availability can disappear after creation but before admission.
   * The one-capability forced envelope must be rejected, never admitted CPU. */
  uint64_t unavailable_id = 0;
  Lardon3DTask *unavailable = ok ? lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &unavailable_id) : NULL;
  Lardon3DTaskSnapshot snapshot;
  bool unavailable_added = unavailable &&
      lardon3d_resource_governor_internal_set_backend_available(
          fixture.state.resource_governor,
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, false) &&
      lardon3d_task_queue_add(fixture.state.task_queue, unavailable, NULL);
  if (unavailable_added) unavailable = NULL;
  ok = ok && unavailable_added &&
       wait_state(fixture.state.task_queue, unavailable_id, TASK_FAILED,
                  &snapshot);
  if (!ok) fprintf(stderr, "forced benchmark unavailable backend was not rejected\n");
  if (unavailable) lardon3d_task_destroy(unavailable);

  /* A failure after GPU admission may leave exact whole-pair CPU publications
   * durable, but the benchmark Task itself must fail and classify the cohort
   * as backend failure rather than local ineligibility. */
  char failed_pair[32];
  Lardon3DMatcherTaskConfiguration failing = settings;
  failing.matcher.ratio_threshold = 0.74F;
  uint64_t failing_id = 0;
  ok = ok &&
       lardon3d_resource_governor_internal_set_backend_available(
           fixture.state.resource_governor,
           LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true) &&
       snprintf(failed_pair, sizeof(failed_pair), "%lu",
                (unsigned long)fixture.pairs[0].candidate_pair_id) > 0 &&
       setenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
              failed_pair, 1) == 0 &&
       lardon3d_project_enqueue_matcher_task(
           &fixture.state, &failing, &failing_id) &&
       wait_state(fixture.state.task_queue, failing_id, TASK_FAILED, &snapshot);
  (void)unsetenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID");
  Lardon3DResourceSequenceAggregate aggregate;
  Lardon3DResourceSequenceDiagnostic failing_diagnostic;
  size_t failed_results = 0;
  ok = ok && count_results(&fixture, &failed_results) && failed_results == 2 &&
       lardon3d_resource_governor_internal_sequence_aggregate(
           fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
           LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate) &&
       lardon3d_resource_governor_internal_last_diagnostic(
           fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
           LARDON3D_MATCHER_TASK_KIND_VERSION, &failing_diagnostic) &&
       aggregate.admission_count == 1 && aggregate.sequence_count == 1 &&
       aggregate.selected_backend_admissions[
           LARDON3D_RESOURCE_BACKEND_CPU] == 0 &&
       aggregate.selected_backend_admissions[
           LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] == 1 &&
       aggregate.backend_failure_fallback_sequences == 1 &&
       aggregate.backend_ineligible_fallback_sequences == 0 &&
       aggregate.backend_other_fallback_sequences == 0 &&
       aggregate.local_ineligible_fallback_items == 0 &&
       aggregate.backend_failure_fallback_items == failed_results &&
       aggregate.backend_other_fallback_items == 0 &&
       failing_diagnostic.execution.backend_failure_fallback_items ==
           failed_results &&
       failing_diagnostic.execution.local_ineligible_fallback_items == 0 &&
       failing_diagnostic.execution.backend_other_fallback_items == 0;
  if (!ok) {
    fprintf(stderr,
            "forced benchmark failure classification failed results=%zu "
            "admissions=%lu sequences=%lu cpu=%lu vk=%lu actual_cpu=%lu "
            "actual_vk=%lu actual_mixed=%lu fallbacks=%lu local=%lu "
            "failure=%lu other=%lu local_items=%lu failure_items=%lu "
            "other_items=%lu reason=%s\n",
            failed_results, (unsigned long)aggregate.admission_count,
            (unsigned long)aggregate.sequence_count,
            (unsigned long)aggregate.selected_backend_admissions[
                LARDON3D_RESOURCE_BACKEND_CPU],
            (unsigned long)aggregate.selected_backend_admissions[
                LARDON3D_RESOURCE_BACKEND_ORB_VULKAN],
            (unsigned long)aggregate.actual_backend_sequences[
                LARDON3D_RESOURCE_BACKEND_CPU],
            (unsigned long)aggregate.actual_backend_sequences[
                LARDON3D_RESOURCE_BACKEND_ORB_VULKAN],
            (unsigned long)aggregate.actual_backend_sequences[
                LARDON3D_RESOURCE_BACKEND_MIXED],
            (unsigned long)aggregate.backend_fallback_sequences,
            (unsigned long)aggregate.backend_ineligible_fallback_sequences,
            (unsigned long)aggregate.backend_failure_fallback_sequences,
            (unsigned long)aggregate.backend_other_fallback_sequences,
            (unsigned long)aggregate.local_ineligible_fallback_items,
            (unsigned long)aggregate.backend_failure_fallback_items,
            (unsigned long)aggregate.backend_other_fallback_items,
            failing_diagnostic.backend_reason);
  }

  Lardon3DMatcherTaskConfiguration following = settings;
  following.matcher.ratio_threshold = 0.73F;
  uint64_t following_id = 0;
  bool availability_restored =
      lardon3d_resource_governor_internal_set_backend_available(
          fixture.state.resource_governor,
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true);
  bool following_enqueued = availability_restored &&
      lardon3d_project_enqueue_matcher_task(
          &fixture.state, &following, &following_id);
  bool following_completed = following_enqueued &&
      wait_state(fixture.state.task_queue, following_id, TASK_COMPLETED,
                 &snapshot);
  bool following_aggregate = following_completed &&
      lardon3d_resource_governor_internal_sequence_aggregate(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate);
  ok = ok && following_aggregate && aggregate.admission_count > 1 &&
       aggregate.selected_backend_admissions[
           LARDON3D_RESOURCE_BACKEND_CPU] == 0 &&
       aggregate.selected_backend_admissions[
           LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] ==
           aggregate.admission_count &&
       aggregate.backend_failure_fallback_sequences == 1 &&
       aggregate.backend_other_fallback_sequences == 0 &&
       aggregate.backend_failure_fallback_items == failed_results &&
       aggregate.backend_other_fallback_items == 0;
  if (!ok) {
    fprintf(stderr,
            "forced benchmark backend reuse failed available=%d enqueue=%d "
            "completed=%d aggregate=%d state=%d admissions=%lu cpu=%lu vk=%lu\n",
            availability_restored, following_enqueued, following_completed,
            following_aggregate, (int)snapshot.state,
            (unsigned long)aggregate.admission_count,
            (unsigned long)aggregate.selected_backend_admissions[
                LARDON3D_RESOURCE_BACKEND_CPU],
            (unsigned long)aggregate.selected_backend_admissions[
                LARDON3D_RESOURCE_BACKEND_ORB_VULKAN]);
  }
  bool environment_restored =
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0;
  stop_runtime(&fixture);
  return remove_tree(fixture.root) && environment_restored && ok;
}

static bool pair_local_neighbor_failure_case(const char *failure_variable) {
  Fixture fixture;
  if (!failure_variable || !fixture_create(&fixture)) return false;
  char local_pair[32];
  char failed_pair[32];
  bool configured =
      snprintf(local_pair, sizeof(local_pair), "%lu",
               (unsigned long)fixture.pairs[0].candidate_pair_id) > 0 &&
      snprintf(failed_pair, sizeof(failed_pair), "%lu",
               (unsigned long)fixture.pairs[1].candidate_pair_id) > 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV) == 0 &&
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV, "2", 1) == 0 &&
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV, "2", 1) == 0 &&
      setenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID",
             local_pair, 1) == 0 &&
      setenv(failure_variable, failed_pair, 1) == 0;
  Lardon3DMatcherTaskConfiguration settings = configuration(&fixture);
  uint64_t task_id = 0;
  Lardon3DTaskSnapshot snapshot;
  bool failed = configured && lardon3d_project_enqueue_matcher_task(
      &fixture.state, &settings, &task_id) &&
      wait_state(fixture.state.task_queue, task_id, TASK_FAILED, &snapshot);
  Lardon3DResourceSequenceAggregate aggregate = {0};
  Lardon3DResourceSequenceDiagnostic diagnostic = {0};
  size_t result_count = 0;
  bool exact = failed && count_results(&fixture, &result_count) &&
      result_count == 2 &&
      lardon3d_resource_governor_internal_sequence_aggregate(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate) &&
      lardon3d_resource_governor_internal_last_diagnostic(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &diagnostic) &&
      aggregate.sequence_count == 1 && aggregate.durable_items == 2 &&
      aggregate.local_ineligible_fallback_items == 1 &&
      aggregate.backend_failure_fallback_items == 1 &&
      aggregate.backend_other_fallback_items == 0 &&
      diagnostic.execution.local_ineligible_fallback_items == 1 &&
      diagnostic.execution.backend_failure_fallback_items == 1 &&
      diagnostic.execution.backend_other_fallback_items == 0;
  (void)unsetenv(failure_variable);
  (void)unsetenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID");
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV);
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV);
  stop_runtime(&fixture);
  return remove_tree(fixture.root) && exact;
}

static bool local_vulkan_failure_preserves_backend_case(
    const char *failure_variable, bool pre_submit) {
  Fixture fixture;
  if (!failure_variable || !fixture_create(&fixture)) return false;
  char first_pair[32];
  bool configured =
      snprintf(first_pair, sizeof(first_pair), "%lu",
               (unsigned long)fixture.pairs[0].candidate_pair_id) > 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV) == 0 &&
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV, "2", 1) == 0 &&
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV, "2", 1) == 0 &&
      setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH", "1", 1) == 0 &&
      setenv(failure_variable, first_pair, 1) == 0;
  Lardon3DOrbVulkanTelemetry telemetry_before = {0};
  Lardon3DOrbVulkanTelemetry telemetry_after = {0};
  configured = configured && lardon3d_orb_vulkan_internal_telemetry(
      fixture.state.orb_vulkan_backend, &telemetry_before);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration settings = configuration(&fixture);
  settings.matcher.ratio_threshold = pre_submit ? 0.611F : 0.612F;
  uint64_t task_id = 0;
  Lardon3DTaskSnapshot snapshot;
  bool paused = configured && lardon3d_project_enqueue_matcher_task(
      &fixture.state, &settings, &task_id) &&
      wait_state(fixture.state.task_queue, task_id, TASK_PAUSED, &snapshot);
  Lardon3DResourceSequenceAggregate aggregate = {0};
  Lardon3DResourceSequenceDiagnostic diagnostic = {0};
  Lardon3DOrbVulkanInfo backend_info = {0};
  size_t first_submit = 0;
  size_t first_finish = 0;
  size_t successor_submit = 0;
  size_t successor_finish = 0;
  bool first_submitted = find_test_event(
      TEST_EVENT_GPU_SUBMIT, fixture.pairs[0].candidate_pair_id,
      &first_submit);
  bool first_finished = find_test_event(
      TEST_EVENT_GPU_FINISH, fixture.pairs[0].candidate_pair_id,
      &first_finish);
  bool successor_submitted = find_test_event(
      TEST_EVENT_GPU_SUBMIT, fixture.pairs[1].candidate_pair_id,
      &successor_submit);
  bool successor_finished = find_test_event(
      TEST_EVENT_GPU_FINISH, fixture.pairs[1].candidate_pair_id,
      &successor_finish);
  /* The injected fault is deliberately outside Vulkan. The first durable
   * sequence must use a complete CPU fallback, retain an unrelated submitted
   * successor, and classify exactly one OTHER item without poisoning shared
   * backend health or leaking a request-bound slot. */
  bool exact = paused &&
      lardon3d_resource_governor_internal_sequence_aggregate(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate) &&
      lardon3d_resource_governor_internal_last_diagnostic(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &diagnostic) &&
      lardon3d_orb_vulkan_internal_telemetry(
          fixture.state.orb_vulkan_backend, &telemetry_after) &&
      lardon3d_orb_vulkan_backend_info(
          fixture.state.orb_vulkan_backend, &backend_info) &&
      aggregate.sequence_count == 1 && aggregate.durable_items == 2 &&
      aggregate.backend_failure_fallback_sequences == 0 &&
      aggregate.backend_other_fallback_sequences == 1 &&
      aggregate.local_ineligible_fallback_items == 0 &&
      aggregate.backend_failure_fallback_items == 0 &&
      aggregate.backend_other_fallback_items == 1 &&
      diagnostic.backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
      diagnostic.actual_backend == LARDON3D_RESOURCE_BACKEND_MIXED &&
      diagnostic.backend_fallback &&
      diagnostic.execution.local_ineligible_fallback_items == 0 &&
      diagnostic.execution.backend_failure_fallback_items == 0 &&
      diagnostic.execution.backend_other_fallback_items == 1 &&
      strcmp(diagnostic.backend_reason,
             "vulkan-and-local-failure-cpu-fallback") == 0 &&
      successor_submitted && successor_finished &&
      (pre_submit ? !first_submitted && !first_finished
                  : first_submitted && first_finished) &&
      telemetry_after.submits == telemetry_before.submits +
          (pre_submit ? 1U : 2U) &&
      telemetry_after.completions == telemetry_before.completions +
          (pre_submit ? 1U : 2U) &&
      telemetry_after.failures == telemetry_before.failures &&
      telemetry_after.discards == telemetry_before.discards &&
      telemetry_after.pending_slots == 0 && !telemetry_after.slot_pending &&
      backend_info.available &&
      lardon3d_matcher_task_test_overlap_publications() >= 1 &&
      no_staged_match_temporaries(&fixture);

  bool resumed = unsetenv(failure_variable) == 0 &&
      unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH") == 0 &&
      lardon3d_task_queue_resume(fixture.state.task_queue, task_id) &&
      wait_state(fixture.state.task_queue, task_id, TASK_COMPLETED, &snapshot);
  size_t first_task_results = 0;
  exact = exact && resumed && count_results(&fixture, &first_task_results) &&
      first_task_results == PERSISTED_PAIR_COUNT;

  /* A fresh AUTO Task is the Governor-level health proof. It must still
   * select the Vulkan arm and reuse the exact slots after the local fault. */
  bool overrides_cleared =
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0;
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration following = configuration(&fixture);
  following.matcher.ratio_threshold = pre_submit ? 0.613F : 0.614F;
  uint64_t following_id = 0;
  size_t final_results = 0;
  exact = exact && overrides_cleared &&
      lardon3d_project_enqueue_matcher_task(
          &fixture.state, &following, &following_id) &&
      wait_state(fixture.state.task_queue, following_id,
                 TASK_COMPLETED, &snapshot) &&
      lardon3d_matcher_task_test_vulkan_uses() >= 2 &&
      lardon3d_resource_governor_internal_last_diagnostic(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &diagnostic) &&
      diagnostic.backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
      diagnostic.gpu_slots == 1 &&
      lardon3d_resource_governor_internal_sequence_aggregate(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate) &&
      aggregate.backend_failure_fallback_sequences == 0 &&
      aggregate.backend_failure_fallback_items == 0 &&
      aggregate.backend_other_fallback_items == 1 &&
      count_results(&fixture, &final_results) &&
      final_results == 2 * PERSISTED_PAIR_COUNT &&
      lardon3d_orb_vulkan_backend_info(
          fixture.state.orb_vulkan_backend, &backend_info) &&
      backend_info.available && no_staged_match_temporaries(&fixture);

  (void)unsetenv(failure_variable);
  (void)unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH");
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV);
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV);
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV);
  stop_runtime(&fixture);
  return remove_tree(fixture.root) && exact;
}

typedef enum {
  PREFIX_EXIT_CANCEL = 0,
  PREFIX_EXIT_COMPUTE_FAILURE,
  PREFIX_EXIT_PUBLICATION_FAILURE,
} PrefixExit;

static bool durable_fallback_prefix_case(bool synchronous, PrefixExit exit) {
  Fixture fixture;
  if (!fixture_create(&fixture)) return false;
  char first_pair[32];
  char second_pair[32];
  bool configured =
      snprintf(first_pair, sizeof(first_pair), "%lu",
               (unsigned long)fixture.pairs[0].candidate_pair_id) > 0 &&
      snprintf(second_pair, sizeof(second_pair), "%lu",
               (unsigned long)fixture.pairs[1].candidate_pair_id) > 0 &&
      (synchronous
           ? setenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV, "1", 1)
           : unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV)) == 0 &&
      setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV,
             synchronous ? "1" : "2", 1) == 0 &&
      unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0 &&
      setenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID",
             first_pair, 1) == 0;
  if (configured && exit == PREFIX_EXIT_CANCEL) {
    configured = setenv(
        "LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION", "1", 1) == 0;
  } else if (configured && exit == PREFIX_EXIT_COMPUTE_FAILURE) {
    configured =
        setenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
               second_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
               second_pair, 1) == 0;
  } else if (configured) {
    configured =
        setenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
               second_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
               second_pair, 1) == 0;
  }
  Lardon3DMatcherTaskConfiguration settings = configuration(&fixture);
  uint64_t task_id = 0;
  Lardon3DTaskSnapshot snapshot;
  bool ended = configured && lardon3d_project_enqueue_matcher_task(
      &fixture.state, &settings, &task_id);
  if (ended && exit == PREFIX_EXIT_CANCEL) {
    ended = wait_state(fixture.state.task_queue, task_id, TASK_PAUSED,
                       &snapshot) &&
        lardon3d_task_queue_cancel(fixture.state.task_queue, task_id) &&
        wait_state(fixture.state.task_queue, task_id, TASK_CANCELLED,
                   &snapshot);
  } else if (ended) {
    ended = wait_state(fixture.state.task_queue, task_id, TASK_FAILED,
                       &snapshot);
  }
  Lardon3DResourceSequenceAggregate aggregate = {0};
  size_t result_count = 0;
  /* The first fallback is durable; the second pair never reaches that
   * boundary. Failed/cancelled work must retain one item without manufacturing
   * a successful throughput sequence or durable-item rate observation. */
  bool exact = ended && count_results(&fixture, &result_count) &&
      result_count == 1 &&
      lardon3d_resource_governor_internal_sequence_aggregate(
          fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate) &&
      aggregate.local_ineligible_fallback_items == 1 &&
      aggregate.backend_failure_fallback_items == 0 &&
      aggregate.backend_other_fallback_items == 0 &&
      aggregate.sequence_count == 0 && aggregate.durable_items == 0;
  (void)unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION");
  (void)unsetenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID");
  (void)unsetenv("LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID");
  (void)unsetenv("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID");
  (void)unsetenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID");
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV);
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV);
  (void)unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV);
  stop_runtime(&fixture);
  return remove_tree(fixture.root) && exact;
}
#endif

static bool run_test(void) {
  CHECK(matcher_exact_memory_boundary_test());
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
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  /* Malformed runner-private controls must fail Task construction rather than
   * silently selecting an adaptive or partially tagged capability. */
  uint64_t invalid_benchmark_task_id = 99;
  CHECK(setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV, "01", 1) == 0);
  Lardon3DTask *invalid_benchmark_task =
      lardon3d_project_create_matcher_task(
          &fixture.state, &settings, &invalid_benchmark_task_id);
  CHECK(invalid_benchmark_task == NULL && invalid_benchmark_task_id == 0 &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == 0);
  CHECK(setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV, "1", 1) == 0 &&
        setenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV, "3", 1) == 0);
  invalid_benchmark_task_id = 99;
  invalid_benchmark_task = lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &invalid_benchmark_task_id);
  CHECK(invalid_benchmark_task == NULL && invalid_benchmark_task_id == 0 &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == 0 &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0);
  CHECK(setenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV, "4", 1) == 0);
  invalid_benchmark_task_id = 99;
  invalid_benchmark_task = lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &invalid_benchmark_task_id);
  CHECK(invalid_benchmark_task == NULL && invalid_benchmark_task_id == 0 &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0);
#endif
  Lardon3DMatcherTaskConfiguration invalid = settings;
  invalid.matcher.kind = LARDON3D_MATCHER_SIFT_BF;
  uint64_t invalid_task_id = 0;
  CHECK(!lardon3d_project_create_matcher_task(&fixture.state, &invalid,
                                              &invalid_task_id));
  CHECK(invalid_task_id == 0);
  CHECK(setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  CHECK(setenv("LARDON3D_TEST_MATCHER_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t task_id = 0;
  const uint64_t original_memory_total =
      fixture.state.hardware_profile.memory_total_bytes;
  const uint64_t depth_one_maximum_total =
      UINT64_C(3) * 1024 * 1024 * 1024 +
      UINT64_C(8) * 10 * 1024 * 1024 + UINT64_C(640) * 1024;
  fixture.state.hardware_profile.memory_total_bytes = depth_one_maximum_total;
  Lardon3DTask *first_auto = lardon3d_project_create_matcher_task(
      &fixture.state, &settings, &task_id);
  fixture.state.hardware_profile.memory_total_bytes = original_memory_total;
  Lardon3DOrbVulkanInfo before_auto;
  CHECK(first_auto && lardon3d_orb_vulkan_backend_info(
          fixture.state.orb_vulkan_backend, &before_auto)
      && !before_auto.initialized);
  /* Normal creation at the exact depth-1/batch-8 UMA boundary has exposed
   * metadata only. The removed depth-2 caller guess would suppress it here;
   * the first possible initialization remains begin() on the Queue worker. */
  CHECK(lardon3d_task_queue_add(
      fixture.state.task_queue, first_auto, NULL));
  Lardon3DTaskSnapshot snapshot;
  CHECK(wait_state(fixture.state.task_queue, task_id, TASK_PAUSED, &snapshot));
  Lardon3DOrbVulkanInfo after_auto;
  CHECK(lardon3d_orb_vulkan_backend_info(
      fixture.state.orb_vulkan_backend, &after_auto));
#ifdef LARDON3D_MATCHER_TASK_VULKAN
  CHECK(after_auto.initialized);
#else
  CHECK(!after_auto.initialized);
#endif

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
  CHECK(setenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT", "2", 1) == 0);
  uint64_t cancelled_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(&fixture.state, &settings,
                                              &cancelled_id));
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT") == 0);
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
      /* CPU12 reserves at most twelve 10 MiB staged-pair working sets; leave
       * less than that available so the paused task must wait for a safe
       * re-admission rather than silently exceeding its Governor contract. */
      fixture.state.hardware_profile.memory_total_bytes - 128ULL * 1024 * 1024;
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

  /* Thread-count changes are operational only: all five runs must publish
   * the same Candidate Pair cardinality and identical raw Match evidence. */
  CHECK(lardon3d_feature_opencv_configure_threads(3));
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
  Lardon3DMatcherTaskConfiguration twelve = settings;
  twelve.matcher.ratio_threshold = 0.85F;
  CHECK(run_completed_with_threads(&fixture, twelve, 12));
  size_t equivalence_after = 0;
  CHECK(count_results(&fixture, &equivalence_after));
  CHECK(equivalence_after == equivalence_before + 5 * PERSISTED_PAIR_COUNT);
  CHECK(lardon3d_feature_opencv_thread_count() == 3);
  Lardon3DResourceSequenceDiagnostic cpu_item_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
            LARDON3D_MATCHER_TASK_KIND_VERSION, &cpu_item_diagnostic) &&
        cpu_item_diagnostic.backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        cpu_item_diagnostic.actual_backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        cpu_item_diagnostic.execution.local_ineligible_fallback_items == 0 &&
        cpu_item_diagnostic.execution.backend_failure_fallback_items == 0 &&
        cpu_item_diagnostic.execution.backend_other_fallback_items == 0);
  CHECK(candidate_results_have_one_evidence(
      database_path, fixture.pairs[0].candidate_pair_id));

  /* Explicit GPU selection is operational only. A real Queue admission must
   * reserve the exact GPU shape and dispatch the first 769x769 ORB pair to
   * Vulkan; all Match evidence remains byte-identical to the CPU runs above. */
#ifdef LARDON3D_MATCHER_TASK_VULKAN
  /* Normal ORB is AUTO. The validated runtime must select GPU first, while
   * retaining the canonical CPU durable estimate and identical evidence. */
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration automatic = settings;
  automatic.matcher.ratio_threshold = 0.69F;
  uint64_t automatic_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(
      &fixture.state, &automatic, &automatic_id));
  CHECK(wait_state(fixture.state.task_queue, automatic_id, TASK_COMPLETED,
                   &snapshot));
  CHECK(lardon3d_matcher_task_test_vulkan_uses() >= 1);
  Lardon3DResourceSequenceDiagnostic automatic_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
      fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, &automatic_diagnostic));
  CHECK(automatic_diagnostic.backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        automatic_diagnostic.helper_limit == 0 &&
        automatic_diagnostic.gpu_slots == 1 &&
        automatic_diagnostic.inflight_limit == 1 &&
        automatic_diagnostic.gpu_memory_bytes == 640U * 1024U &&
        automatic_diagnostic.reason[0] != '\0' &&
        automatic_diagnostic.backend_reason[0] != '\0');
  if (automatic_diagnostic.actual_backend ==
      LARDON3D_RESOURCE_BACKEND_ORB_VULKAN) {
    CHECK(!automatic_diagnostic.backend_fallback &&
          automatic_diagnostic.execution.vulkan_submits >= 1 &&
          automatic_diagnostic.execution.vulkan_completions >= 1 &&
          automatic_diagnostic.execution.publication_ns > 0 &&
          strcmp(automatic_diagnostic.backend_reason,
                 "vulkan-completed") == 0);
  } else {
    /* Tiny fixture pairs are below the validated Vulkan threshold. They are
     * complete CPU pairs inside a GPU-selected sequence, not backend failure. */
    CHECK(automatic_diagnostic.backend_fallback &&
          (automatic_diagnostic.actual_backend ==
               LARDON3D_RESOURCE_BACKEND_CPU ||
           automatic_diagnostic.actual_backend ==
               LARDON3D_RESOURCE_BACKEND_MIXED));
  }

  /* A successor rejected as locally ineligible has no pending handle. The
   * first sequence must finish that complete pair on CPU, retain backend
   * health, and leave an already-created AUTO Task eligible for GPU admission. */
  char ineligible_pair[32];
  CHECK(snprintf(ineligible_pair, sizeof(ineligible_pair), "%lu",
                 (unsigned long)fixture.pairs[1].candidate_pair_id) > 0 &&
        setenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID",
               ineligible_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH", "1", 1) == 0);
  Lardon3DMatcherTaskConfiguration local_ineligible = settings;
  local_ineligible.matcher.ratio_threshold = 0.687F;
  uint64_t local_ineligible_id = 0;
  Lardon3DTask *local_ineligible_task =
      lardon3d_project_create_matcher_task_with_mode(
          &fixture.state, &local_ineligible,
          LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &local_ineligible_id);
  Lardon3DMatcherTaskConfiguration auto_after_ineligible = settings;
  auto_after_ineligible.matcher.ratio_threshold = 0.686F;
  uint64_t auto_after_ineligible_id = 0;
  Lardon3DTask *auto_after_ineligible_task =
      lardon3d_project_create_matcher_task(
          &fixture.state, &auto_after_ineligible,
          &auto_after_ineligible_id);
  CHECK(local_ineligible_task && auto_after_ineligible_task &&
        lardon3d_task_queue_add(fixture.state.task_queue,
                                local_ineligible_task, NULL) &&
        wait_state(fixture.state.task_queue, local_ineligible_id,
                   TASK_PAUSED, &snapshot));
  Lardon3DResourceSequenceDiagnostic ineligible_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
            LARDON3D_MATCHER_TASK_KIND_VERSION, &ineligible_diagnostic) &&
        ineligible_diagnostic.backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        ineligible_diagnostic.actual_backend ==
            LARDON3D_RESOURCE_BACKEND_MIXED &&
        ineligible_diagnostic.backend_fallback &&
        ineligible_diagnostic.execution.local_ineligible_fallback_items > 0 &&
        ineligible_diagnostic.execution.backend_failure_fallback_items == 0 &&
        ineligible_diagnostic.execution.backend_other_fallback_items == 0 &&
        strcmp(ineligible_diagnostic.backend_reason,
               "vulkan-and-ineligible-pair-cpu-fallback") == 0);
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID") == 0 &&
        unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH") == 0 &&
        lardon3d_task_queue_resume(fixture.state.task_queue,
                                   local_ineligible_id) &&
        wait_state(fixture.state.task_queue, local_ineligible_id,
                   TASK_COMPLETED, &snapshot) &&
        lardon3d_task_queue_add(fixture.state.task_queue,
                                auto_after_ineligible_task, NULL) &&
        wait_state(fixture.state.task_queue, auto_after_ineligible_id,
                   TASK_COMPLETED, &snapshot));
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
            LARDON3D_MATCHER_TASK_KIND_VERSION, &ineligible_diagnostic) &&
        ineligible_diagnostic.backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        ineligible_diagnostic.gpu_slots == 1);

  /* Genuine begin failure is learned before publication. Even when that
   * publication then fails, shared AUTO admission must already be disabled;
   * the pre-created probe therefore selects its complete CPU capability. */
  char backend_failure_pair[32];
  char early_publication_pair[32];
  CHECK(snprintf(backend_failure_pair, sizeof(backend_failure_pair), "%lu",
                 (unsigned long)fixture.pairs[1].candidate_pair_id) > 0 &&
        snprintf(early_publication_pair, sizeof(early_publication_pair), "%lu",
                 (unsigned long)fixture.pairs[0].candidate_pair_id) > 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
               backend_failure_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
               early_publication_pair, 1) == 0);
  Lardon3DMatcherTaskConfiguration early_failure = settings;
  early_failure.matcher.ratio_threshold = 0.685F;
  uint64_t early_failure_id = 0;
  Lardon3DTask *early_failure_task =
      lardon3d_project_create_matcher_task_with_mode(
          &fixture.state, &early_failure,
          LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &early_failure_id);
  Lardon3DMatcherTaskConfiguration auto_after_failure = settings;
  auto_after_failure.matcher.ratio_threshold = 0.684F;
  uint64_t auto_after_failure_id = 0;
  Lardon3DTask *auto_after_failure_task =
      lardon3d_project_create_matcher_task(
          &fixture.state, &auto_after_failure, &auto_after_failure_id);
  size_t early_failure_results_before = 0;
  size_t early_failure_results_after = 0;
  CHECK(early_failure_task && auto_after_failure_task &&
        count_results(&fixture, &early_failure_results_before) &&
        lardon3d_task_queue_add(fixture.state.task_queue,
                                early_failure_task, NULL) &&
        wait_state(fixture.state.task_queue, early_failure_id,
                   TASK_FAILED, &snapshot) &&
        unsetenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID") == 0 &&
        unsetenv("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID") == 0 &&
        count_results(&fixture, &early_failure_results_after) &&
        early_failure_results_after == early_failure_results_before &&
        no_staged_match_temporaries(&fixture) &&
        lardon3d_task_queue_add(fixture.state.task_queue,
                                auto_after_failure_task, NULL) &&
        wait_state(fixture.state.task_queue, auto_after_failure_id,
                   TASK_COMPLETED, &snapshot));
  Lardon3DResourceSequenceDiagnostic after_failure_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
            LARDON3D_MATCHER_TASK_KIND_VERSION, &after_failure_diagnostic) &&
        after_failure_diagnostic.backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        after_failure_diagnostic.gpu_slots == 0 &&
        lardon3d_resource_governor_internal_set_backend_available(
            fixture.state.resource_governor,
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, true));

  /* Force only the private admission value, not the execution algorithm. The
   * production rolling owner must submit two exact requests before finishing
   * the oldest, then preserve ordered one-evidence publication and output
   * cardinality without timing sleeps. */
  CHECK(setenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT", "2", 1) == 0);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration depth_two = settings;
  depth_two.matcher.ratio_threshold = 0.683F;
  uint64_t depth_two_id = 0;
  size_t depth_two_before = 0;
  size_t depth_two_after = 0;
  CHECK(count_results(&fixture, &depth_two_before) &&
        lardon3d_project_enqueue_matcher_task(
            &fixture.state, &depth_two, &depth_two_id) &&
        wait_state(fixture.state.task_queue, depth_two_id, TASK_COMPLETED,
                   &snapshot) &&
        unsetenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT") == 0 &&
        count_results(&fixture, &depth_two_after) &&
        depth_two_after == depth_two_before + PERSISTED_PAIR_COUNT);
  size_t depth_submit_first = 0;
  size_t depth_submit_second = 0;
  size_t depth_finish_first = 0;
  size_t depth_publication_first = 0;
  CHECK(find_test_event(TEST_EVENT_GPU_SUBMIT,
                        fixture.pairs[0].candidate_pair_id,
                        &depth_submit_first) &&
        find_test_event(TEST_EVENT_GPU_SUBMIT,
                        fixture.pairs[1].candidate_pair_id,
                        &depth_submit_second) &&
        find_test_event(TEST_EVENT_GPU_FINISH,
                        fixture.pairs[0].candidate_pair_id,
                        &depth_finish_first) &&
        find_test_event(TEST_EVENT_PUBLICATION_START,
                        fixture.pairs[0].candidate_pair_id,
                        &depth_publication_first));
  Lardon3DResourceSequenceDiagnostic depth_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
            LARDON3D_MATCHER_TASK_KIND_VERSION, &depth_diagnostic) &&
        depth_diagnostic.backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        depth_diagnostic.inflight_limit == 2 &&
        depth_diagnostic.gpu_memory_bytes == 2 * 640 * 1024);
  Lardon3DOrbVulkanInfo depth_backend_info;
  CHECK(lardon3d_matcher_task_test_max_retained_vulkan_payload() ==
            2 * 640 * 1024
        && lardon3d_orb_vulkan_backend_info(
            fixture.state.orb_vulkan_backend, &depth_backend_info)
        && depth_backend_info.permanent_payload_bytes == 640 * 1024);
  CHECK(depth_submit_first < depth_submit_second);
  CHECK(depth_submit_second < depth_finish_first);
  CHECK(depth_finish_first < depth_publication_first);
  CHECK(wait_candidate_results_have_one_evidence(
      database_path, fixture.pairs[0].candidate_pair_id));

  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration gpu = settings;
  gpu.matcher.ratio_threshold = 0.68F;
  uint64_t gpu_id = 0;
  size_t gpu_results_before = 0;
  CHECK(count_results(&fixture, &gpu_results_before));
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
      &fixture.state, &gpu, LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &gpu_id));
  CHECK(wait_state(fixture.state.task_queue, gpu_id, TASK_COMPLETED, &snapshot));
  Lardon3DProjectDbTask gpu_durable;
  CHECK(wait_durable_state(fixture.state.project_db, gpu_id, TASK_COMPLETED,
                           &gpu_durable));
  CHECK(lardon3d_matcher_task_test_vulkan_uses() >= 1);
  Lardon3DOrbVulkanInfo fixed_backend_info;
  CHECK(lardon3d_matcher_task_test_max_retained_vulkan_payload() ==
            640 * 1024
        && lardon3d_orb_vulkan_backend_info(
            fixture.state.orb_vulkan_backend, &fixed_backend_info)
        && fixed_backend_info.permanent_payload_bytes == 640 * 1024);
  size_t gpu_results_after = 0;
  CHECK(count_results(&fixture, &gpu_results_after) &&
        gpu_results_after == gpu_results_before + PERSISTED_PAIR_COUNT);
  size_t submit_first = 0, finish_first = 0, submit_second = 0;
  size_t publication_start = 0, publication_finish = 0;
  CHECK(lardon3d_matcher_task_test_overlap_publications() >= 1 &&
        find_test_event(TEST_EVENT_GPU_SUBMIT,
                        fixture.pairs[0].candidate_pair_id, &submit_first) &&
        find_test_event(TEST_EVENT_GPU_FINISH,
                        fixture.pairs[0].candidate_pair_id, &finish_first) &&
        find_test_event(TEST_EVENT_GPU_SUBMIT,
                        fixture.pairs[1].candidate_pair_id, &submit_second) &&
        find_test_event(TEST_EVENT_PUBLICATION_START,
                        fixture.pairs[0].candidate_pair_id,
                        &publication_start) &&
        find_test_event(TEST_EVENT_PUBLICATION_FINISH,
                        fixture.pairs[0].candidate_pair_id,
                        &publication_finish));
  CHECK(submit_first < finish_first && finish_first < submit_second &&
        submit_second < publication_start &&
        publication_start < publication_finish);
  CHECK(candidate_results_have_one_evidence(
      database_path, fixture.pairs[0].candidate_pair_id));

  /* A rejected successor begin has no GPU event or partial evidence; that
   * pair runs wholly on CPU and every exact backend slot remains reusable. */
  char failed_begin_pair[32];
  CHECK(snprintf(failed_begin_pair, sizeof(failed_begin_pair), "%lu",
                 (unsigned long)fixture.pairs[1].candidate_pair_id) > 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
               failed_begin_pair, 1) == 0);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration begin_fallback = settings;
  begin_fallback.matcher.ratio_threshold = 0.665F;
  uint64_t begin_fallback_id = 0;
  CHECK(count_results(&fixture, &gpu_results_before) &&
        lardon3d_project_enqueue_matcher_task_with_mode(
            &fixture.state, &begin_fallback,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &begin_fallback_id) &&
        wait_state(fixture.state.task_queue, begin_fallback_id,
                   TASK_COMPLETED, &snapshot) &&
        unsetenv("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID") == 0 &&
        count_results(&fixture, &gpu_results_after) &&
        gpu_results_after == gpu_results_before + PERSISTED_PAIR_COUNT);
  CHECK(!find_test_event(TEST_EVENT_GPU_SUBMIT,
                         fixture.pairs[1].candidate_pair_id, &submit_second));
  CHECK(lardon3d_matcher_task_test_overlap_publications() == 0);

  /* Publication failure occurs after successor submission. Failure cleanup
   * must discard that private request so a following Task can reuse the slot. */
  char failed_publish_pair[32];
  CHECK(snprintf(failed_publish_pair, sizeof(failed_publish_pair), "%lu",
                 (unsigned long)fixture.pairs[0].candidate_pair_id) > 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
               failed_publish_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT", "2", 1) == 0);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration failed_publication = settings;
  failed_publication.matcher.ratio_threshold = 0.655F;
  uint64_t failed_publication_id = 0;
  CHECK(count_results(&fixture, &gpu_results_before));
  CHECK(lardon3d_project_enqueue_matcher_task(
      &fixture.state, &failed_publication, &failed_publication_id));
  CHECK(wait_state(fixture.state.task_queue, failed_publication_id,
                   TASK_FAILED, &snapshot));
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID") == 0);
  CHECK(unsetenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT") == 0);
  CHECK(count_results(&fixture, &gpu_results_after));
  CHECK(gpu_results_after == gpu_results_before);
  CHECK(lardon3d_matcher_task_test_overlap_publications() == 1);
  CHECK(no_staged_match_temporaries(&fixture));
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration after_publish_failure = settings;
  after_publish_failure.matcher.ratio_threshold = 0.645F;
  uint64_t after_publish_failure_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
            &fixture.state, &after_publish_failure,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN,
            &after_publish_failure_id) &&
        wait_state(fixture.state.task_queue, after_publish_failure_id,
                   TASK_COMPLETED, &snapshot) &&
        lardon3d_matcher_task_test_vulkan_uses() >= 2);

  /* At depth two, local ineligibility of the oldest pair consumes no slot, so
   * the successor is already pending when its whole-pair CPU fallback fails.
   * The Task owner must discard that successor before sequence cleanup can
   * shrink the mapped payload and release the Governor reservation. */
  char failed_cpu_fallback_pair[32];
  CHECK(snprintf(failed_cpu_fallback_pair,
                 sizeof(failed_cpu_fallback_pair), "%lu",
                 (unsigned long)fixture.pairs[0].candidate_pair_id) > 0);
  Lardon3DOrbVulkanTelemetry cleanup_before = {0};
  Lardon3DOrbVulkanTelemetry cleanup_after = {0};
  CHECK(lardon3d_orb_vulkan_internal_telemetry(
            fixture.state.orb_vulkan_backend, &cleanup_before) &&
        setenv("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID",
               failed_cpu_fallback_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
               failed_cpu_fallback_pair, 1) == 0 &&
        setenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT", "2", 1) == 0);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration failed_cpu_fallback = settings;
  failed_cpu_fallback.matcher.ratio_threshold = 0.643F;
  uint64_t failed_cpu_fallback_id = 0;
  size_t failed_cpu_fallback_before = 0;
  size_t failed_cpu_fallback_after = 0;
  CHECK(count_results(&fixture, &failed_cpu_fallback_before) &&
        lardon3d_project_enqueue_matcher_task(
            &fixture.state, &failed_cpu_fallback,
            &failed_cpu_fallback_id) &&
        wait_state(fixture.state.task_queue, failed_cpu_fallback_id,
                   TASK_FAILED, &snapshot) &&
        unsetenv(
            "LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID") == 0 &&
        unsetenv("LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID") == 0 &&
        unsetenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT") == 0 &&
        count_results(&fixture, &failed_cpu_fallback_after) &&
        failed_cpu_fallback_after == failed_cpu_fallback_before &&
        no_staged_match_temporaries(&fixture));
  size_t failed_cpu_successor_submit = 0;
  Lardon3DOrbVulkanInfo cleaned_backend_info = {0};
  CHECK(find_test_event(TEST_EVENT_GPU_SUBMIT,
                        fixture.pairs[1].candidate_pair_id,
                        &failed_cpu_successor_submit) &&
        lardon3d_matcher_task_test_max_retained_vulkan_payload() ==
            2 * 640 * 1024 &&
        lardon3d_orb_vulkan_internal_telemetry(
            fixture.state.orb_vulkan_backend, &cleanup_after) &&
        cleanup_after.discards == cleanup_before.discards + 1 &&
        cleanup_after.pending_slots == 0 &&
        !cleanup_after.slot_pending &&
        cleanup_after.retained_capacity == 1 &&
        cleanup_after.retained_payload_bytes == 640 * 1024 &&
        !cleanup_after.sequence_capacity_active &&
        lardon3d_orb_vulkan_backend_info(
            fixture.state.orb_vulkan_backend, &cleaned_backend_info) &&
        cleaned_backend_info.available &&
        cleaned_backend_info.permanent_payload_bytes == 640 * 1024);

  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration after_cpu_fallback_failure = settings;
  after_cpu_fallback_failure.matcher.ratio_threshold = 0.642F;
  uint64_t after_cpu_fallback_failure_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
            &fixture.state, &after_cpu_fallback_failure,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN,
            &after_cpu_fallback_failure_id) &&
        wait_state(fixture.state.task_queue,
                   after_cpu_fallback_failure_id, TASK_COMPLETED, &snapshot) &&
        lardon3d_matcher_task_test_vulkan_uses() >= 2 &&
        count_results(&fixture, &gpu_results_after) &&
        gpu_results_after ==
            failed_cpu_fallback_before + PERSISTED_PAIR_COUNT &&
        wait_candidate_results_have_one_evidence(
            database_path, fixture.pairs[0].candidate_pair_id) &&
        lardon3d_orb_vulkan_internal_telemetry(
            fixture.state.orb_vulkan_backend, &cleanup_after) &&
        cleanup_after.pending_slots == 0 &&
        cleanup_after.retained_capacity == 1 &&
        !cleanup_after.sequence_capacity_active);

  /* Cancellation while predecessor publication is paused owns a submitted
   * successor. The sequence-exit path discards it, and reuse is immediate. */
  CHECK(setenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration cancelled_gpu = settings;
  cancelled_gpu.matcher.ratio_threshold = 0.635F;
  uint64_t cancelled_gpu_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
            &fixture.state, &cancelled_gpu,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &cancelled_gpu_id) &&
        wait_state(fixture.state.task_queue, cancelled_gpu_id, TASK_PAUSED,
                   &snapshot) &&
        lardon3d_task_queue_cancel(fixture.state.task_queue,
                                   cancelled_gpu_id) &&
        wait_state(fixture.state.task_queue, cancelled_gpu_id,
                   TASK_CANCELLED, &snapshot) &&
        unsetenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION") == 0 &&
        lardon3d_matcher_task_test_overlap_publications() == 1 &&
        no_staged_match_temporaries(&fixture));
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration after_cancel = settings;
  after_cancel.matcher.ratio_threshold = 0.625F;
  uint64_t after_cancel_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
            &fixture.state, &after_cancel,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &after_cancel_id) &&
        wait_state(fixture.state.task_queue, after_cancel_id, TASK_COMPLETED,
                   &snapshot) &&
        lardon3d_matcher_task_test_vulkan_uses() >= 2);
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
  Lardon3DResourceSequenceDiagnostic fallback_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
            fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
            LARDON3D_MATCHER_TASK_KIND_VERSION, &fallback_diagnostic) &&
        fallback_diagnostic.backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        fallback_diagnostic.actual_backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        fallback_diagnostic.backend_fallback &&
        fallback_diagnostic.helper_limit == 0 &&
        strcmp(fallback_diagnostic.backend_reason,
               "vulkan-failed-whole-pair-cpu-fallback") == 0);
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
  CHECK(lardon3d_project_enqueue_matcher_task_with_mode(
      &fixture.state, &reduced, LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL,
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
  /* AUTO backend choice is private runtime state. MIXED truthfully records
   * that the operation may consume CPU or Vulkan without persisting either
   * backend choice. Explicit CPU retains the exact CPU-class signature. */
  CHECK(estimate_snapshot.estimate.desired_cpu_threads > 1);
  CHECK(estimate_snapshot.estimate.gpu_memory_fixed_bytes == 0);
  CHECK(estimate_snapshot.estimate.desired_gpu_slots == 0);
  CHECK(estimate_snapshot.estimate.task_class ==
        LARDON3D_RESOURCE_TASK_MIXED);
  Lardon3DTaskDurableSnapshot current_auto = estimate_snapshot;
  Lardon3DTaskDurableSnapshot current_cpu = current_auto;
  current_cpu.estimate.task_class = LARDON3D_RESOURCE_TASK_CPU;
  Lardon3DTaskDurableSnapshot current_gpu = current_cpu;
  current_gpu.estimate.gpu_memory_fixed_bytes =
      LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES;
  current_gpu.estimate.desired_cpu_threads = 1;
  current_gpu.estimate.desired_gpu_slots = 1;
  lardon3d_task_destroy(estimate_task);
  estimate_task = NULL;

  uint64_t explicit_cpu_id = 0;
  Lardon3DTask *explicit_cpu_task =
      lardon3d_project_create_matcher_task_with_mode(
          &fixture.state, &settings, LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL,
          &explicit_cpu_id);
  Lardon3DTaskDurableSnapshot explicit_cpu_snapshot;
  CHECK(explicit_cpu_task && explicit_cpu_id != current_auto.id &&
        lardon3d_task_durable_snapshot(explicit_cpu_task,
                                       &explicit_cpu_snapshot));
  CHECK(same_estimate(&explicit_cpu_snapshot.estimate,
                      &current_cpu.estimate));
  current_cpu = explicit_cpu_snapshot;
  current_gpu = current_cpu;
  current_gpu.estimate.gpu_memory_fixed_bytes =
      LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES;
  current_gpu.estimate.desired_cpu_threads = 1;
  current_gpu.estimate.desired_gpu_slots = 1;
  lardon3d_task_destroy(explicit_cpu_task);

#ifdef LARDON3D_MATCHER_TASK_VULKAN
  estimate_task = lardon3d_project_create_matcher_task_with_mode(
      &fixture.state, &settings, LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN,
      &estimate_task_id);
  CHECK(estimate_task != NULL &&
        lardon3d_task_durable_snapshot(estimate_task, &estimate_snapshot));
  CHECK(same_estimate(&estimate_snapshot.estimate, &current_gpu.estimate));
  current_gpu = estimate_snapshot;
#endif

  /* Explicit selection failures occur before Task-ID allocation and cannot
   * silently become CPU work. Normal create remains AUTO. */
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
  lardon3d_matcher_task_test_reset_backend_counters();
  Lardon3DMatcherTaskConfiguration no_gpu_auto = settings;
  no_gpu_auto.matcher.ratio_threshold = 0.63F;
  uint64_t no_gpu_id = 0;
  CHECK(lardon3d_project_enqueue_matcher_task(
      &fixture.state, &no_gpu_auto, &no_gpu_id));
  CHECK(wait_state(fixture.state.task_queue, no_gpu_id, TASK_COMPLETED,
                   &snapshot));
  CHECK(lardon3d_matcher_task_test_vulkan_uses() == 0);
  Lardon3DResourceSequenceDiagnostic cpu_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
      fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, &cpu_diagnostic));
  CHECK(cpu_diagnostic.backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        cpu_diagnostic.gpu_slots == 0);
  fixture.state.hardware_profile.gpu_available = gpu_available;
  Lardon3DOrbVulkanBackend *backend = fixture.state.orb_vulkan_backend;
  fixture.state.orb_vulkan_backend = NULL;
  rejected_id = 99;
  CHECK(lardon3d_project_create_matcher_task_with_mode(
            &fixture.state, &settings,
            LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN, &rejected_id) == NULL &&
        rejected_id == 0);
  fixture.state.orb_vulkan_backend = backend;

  /* Reconstruction accepts exact current CPU12 forms plus the preceding
   * CPU8-per-pair and oldest fixed-memory CPU12 forms. All historic resource
   * policies normalize only in memory; neighboring shapes stay corruption. */
  Lardon3DTaskReconstructionContext reconstruction = {
      .project_path = fixture.state.project_path,
      .project_db = fixture.state.project_db,
      .resource_governor = fixture.state.resource_governor,
      .orb_vulkan_backend = fixture.state.orb_vulkan_backend,
  };

  /* Restart preserves the two normal/override policies without a schema or
   * codec field. On a Vulkan build the restored AUTO signature selects GPU,
   * while the restored explicit CPU signature remains fixed CPU. */
  Lardon3DTask *restarted_auto = NULL;
  CHECK(lardon3d_task_kind_registry_restore(
            lardon3d_task_kind_registry_production(),
            LARDON3D_MATCHER_TASK_KIND, LARDON3D_MATCHER_TASK_KIND_VERSION,
            &current_auto, &reconstruction, &restarted_auto) ==
            LARDON3D_TASK_KIND_OK &&
        restarted_auto);
  Lardon3DTaskDurableSnapshot order_historical_cpu = current_cpu;
  order_historical_cpu.estimate.memory_fixed_bytes = 10U * 1024U * 1024U;
  order_historical_cpu.estimate.memory_bytes_per_item = 0;
  order_historical_cpu.estimate.desired_cpu_threads = 12;
  order_historical_cpu.estimate.maximum_batch_size = 8;
  Lardon3DTaskDurableSnapshot order_historical_gpu = current_gpu;
  order_historical_gpu.estimate.memory_fixed_bytes = 10U * 1024U * 1024U;
  order_historical_gpu.estimate.memory_bytes_per_item = 0;
  order_historical_gpu.estimate.desired_cpu_threads = 12;
  order_historical_gpu.estimate.maximum_batch_size = 8;
  const Lardon3DTaskDurableSnapshot *fixed_restore_order[] = {
      &current_cpu,
      &current_gpu,
      &order_historical_cpu,
      &order_historical_gpu,
  };
  /* AUTO establishes shared eligibility. Later co-restoration of fixed CPU,
   * fixed Vulkan, and historical forms is order-independent and must not
   * clear it before the already-restored AUTO Task reaches admission. */
  for (size_t index = 0;
       index < sizeof(fixed_restore_order) / sizeof(fixed_restore_order[0]);
       ++index) {
    Lardon3DTask *fixed_restored = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              LARDON3D_MATCHER_TASK_KIND,
              LARDON3D_MATCHER_TASK_KIND_VERSION, fixed_restore_order[index],
              &reconstruction, &fixed_restored) == LARDON3D_TASK_KIND_OK &&
          fixed_restored);
    lardon3d_task_destroy(fixed_restored);
  }
  CHECK(lardon3d_task_queue_add(fixture.state.task_queue, restarted_auto, NULL));
  CHECK(wait_state(fixture.state.task_queue, current_auto.id, TASK_COMPLETED,
                   &snapshot));
  Lardon3DResourceSequenceDiagnostic restart_diagnostic;
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
      fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, &restart_diagnostic));
#ifdef LARDON3D_MATCHER_TASK_VULKAN
  CHECK(restart_diagnostic.backend ==
            LARDON3D_RESOURCE_BACKEND_ORB_VULKAN &&
        restart_diagnostic.gpu_slots == 1);
#else
  CHECK(restart_diagnostic.backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        restart_diagnostic.gpu_slots == 0);
#endif
  Lardon3DTask *restarted_cpu = NULL;
  CHECK(lardon3d_task_kind_registry_restore(
            lardon3d_task_kind_registry_production(),
            LARDON3D_MATCHER_TASK_KIND, LARDON3D_MATCHER_TASK_KIND_VERSION,
            &current_cpu, &reconstruction, &restarted_cpu) ==
            LARDON3D_TASK_KIND_OK &&
        restarted_cpu);
  CHECK(lardon3d_task_queue_add(fixture.state.task_queue, restarted_cpu, NULL));
  CHECK(wait_state(fixture.state.task_queue, current_cpu.id, TASK_COMPLETED,
                   &snapshot));
  CHECK(lardon3d_resource_governor_internal_last_diagnostic(
      fixture.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, &restart_diagnostic));
  CHECK(restart_diagnostic.backend == LARDON3D_RESOURCE_BACKEND_CPU &&
        restart_diagnostic.gpu_slots == 0);
  Lardon3DTaskDurableSnapshot historical_cpu = order_historical_cpu;
  Lardon3DTaskDurableSnapshot historical_gpu = order_historical_gpu;
  Lardon3DTaskDurableSnapshot previous_cpu = current_cpu;
  previous_cpu.estimate.maximum_batch_size = 8;
  previous_cpu.estimate.desired_cpu_threads = 8;
  Lardon3DTaskDurableSnapshot previous_gpu = current_gpu;
  previous_gpu.estimate.maximum_batch_size = 8;
  char reconciled_path[PATH_MAX];
  CHECK(snprintf(reconciled_path, sizeof(reconciled_path),
                 "%s/.lardon3d/checkpoints/%lu.chk",
                 fixture.state.project_path,
                 (unsigned long)estimate_task_id) > 0 &&
        lardon3d_task_checkpoint_save(reconciled_path, &historical_gpu) ==
            LARDON3D_TASK_CHECKPOINT_OK);
  const Lardon3DTaskDurableSnapshot *accepted[] = {
      &current_auto, &current_cpu, &current_gpu, &previous_cpu, &previous_gpu,
      &historical_cpu, &historical_gpu};
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
        index == 3 || index == 5 ? &current_cpu.estimate
                   : index == 4 || index == 6 ? &current_gpu.estimate
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
  Lardon3DTaskDurableSnapshot malformed[5] = {
      historical_cpu, historical_cpu, historical_gpu, historical_gpu,
      current_gpu};
  malformed[0].estimate.memory_fixed_bytes--;
  malformed[1].estimate.memory_bytes_per_item = 1;
  malformed[2].estimate.gpu_memory_fixed_bytes++;
  malformed[3].estimate.desired_cpu_threads = 11;
  /* MIXED is truthful only for the CPU-shaped durable AUTO policy. A Vulkan
   * resource shape cannot use the class as a synthetic backend tag. */
  malformed[4].estimate.task_class = LARDON3D_RESOURCE_TASK_MIXED;
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
#if defined(LARDON3D_MATCHER_TASK_VULKAN) && \
    defined(LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE)
  /* Distinct fresh Projects prevent identity reuse from hiding either control.
   * Runner-private tokens fix one normal Governor-admitted Vulkan capability
   * for the depth and batch matrices. Synchronous remains the historical
   * depth-one/batch-two fence baseline; all scientific fields stay exact. */
  PipelineEvidence rolling_one = {0};
  PipelineEvidence rolling_two = {0};
  PipelineEvidence batch_four = {0};
  PipelineEvidence batch_eight = {0};
  PipelineEvidence batch_twelve = {0};
  PipelineEvidence synchronous = {0};
  CHECK(capture_pipeline_evidence(false, 1, 2, &rolling_one));
  CHECK(capture_pipeline_evidence(false, 2, 2, &rolling_two));
  CHECK(capture_pipeline_evidence(false, 1, 4, &batch_four));
  CHECK(capture_pipeline_evidence(false, 1, 8, &batch_eight));
  CHECK(capture_pipeline_evidence(false, 1, 12, &batch_twelve));
  CHECK(capture_pipeline_evidence(true, 1, 0, &synchronous));
  CHECK(rolling_one.overlap_publications >= 1);
  CHECK(rolling_two.overlap_publications >= 1);
  CHECK(batch_four.overlap_publications >= 1);
  CHECK(batch_eight.overlap_publications >= 1);
  CHECK(batch_twelve.overlap_publications >= 1);
  CHECK(synchronous.overlap_publications == 0);
  CHECK(same_forced_contract_except_depth(&rolling_one, &rolling_two));
  CHECK(same_forced_contract_except_batch(&rolling_one, &batch_four, 4));
  CHECK(same_forced_contract_except_batch(&rolling_one, &batch_eight, 8));
  CHECK(same_forced_contract_except_batch(&rolling_one, &batch_twelve, 12));
  /* Sequence grouping is deliberately different at each fixed batch. The
   * production Matcher-owned item comparator must nevertheless count the same
   * 29 locally ineligible pairs exactly once in every cohort. */
  CHECK(rolling_one.aggregate.local_ineligible_fallback_items ==
        PERSISTED_PAIR_COUNT - PIPELINE_ELIGIBLE_PAIR_COUNT);
  CHECK(rolling_two.aggregate.local_ineligible_fallback_items ==
        rolling_one.aggregate.local_ineligible_fallback_items);
  CHECK(batch_four.aggregate.local_ineligible_fallback_items ==
        rolling_one.aggregate.local_ineligible_fallback_items);
  CHECK(batch_eight.aggregate.local_ineligible_fallback_items ==
        rolling_one.aggregate.local_ineligible_fallback_items);
  CHECK(batch_twelve.aggregate.local_ineligible_fallback_items ==
        rolling_one.aggregate.local_ineligible_fallback_items);
  CHECK(synchronous.aggregate.local_ineligible_fallback_items ==
        rolling_one.aggregate.local_ineligible_fallback_items);
  CHECK(rolling_one.aggregate.backend_ineligible_fallback_sequences !=
        batch_four.aggregate.backend_ineligible_fallback_sequences);
  CHECK(batch_four.aggregate.backend_ineligible_fallback_sequences !=
        batch_eight.aggregate.backend_ineligible_fallback_sequences);
  CHECK(same_pipeline_scientific_output(&rolling_one, &rolling_two));
  CHECK(same_pipeline_scientific_output(&rolling_one, &batch_four));
  CHECK(same_pipeline_scientific_output(&rolling_one, &batch_eight));
  CHECK(same_pipeline_scientific_output(&rolling_one, &batch_twelve));
  CHECK(same_pipeline_scientific_output(&rolling_two, &synchronous));
  CHECK(forced_benchmark_fail_closed());
  CHECK(pair_local_neighbor_failure_case(
      "LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID"));
  CHECK(pair_local_neighbor_failure_case(
      "LARDON3D_TEST_MATCHER_FAIL_VULKAN_FINISH_PAIR_ID"));
  CHECK(local_vulkan_failure_preserves_backend_case(
      "LARDON3D_TEST_MATCHER_FAIL_LOCAL_VULKAN_BEGIN_PAIR_ID", true));
  CHECK(local_vulkan_failure_preserves_backend_case(
      "LARDON3D_TEST_MATCHER_FAIL_LOCAL_VULKAN_FINISH_PAIR_ID", false));
  CHECK(durable_fallback_prefix_case(false, PREFIX_EXIT_CANCEL));
  CHECK(durable_fallback_prefix_case(
      false, PREFIX_EXIT_COMPUTE_FAILURE));
  CHECK(durable_fallback_prefix_case(
      false, PREFIX_EXIT_PUBLICATION_FAILURE));
  CHECK(durable_fallback_prefix_case(true, PREFIX_EXIT_CANCEL));
  CHECK(durable_fallback_prefix_case(
      true, PREFIX_EXIT_COMPUTE_FAILURE));
  CHECK(durable_fallback_prefix_case(
      true, PREFIX_EXIT_PUBLICATION_FAILURE));
#endif
  return true;
}

int main(void) {
  Lardon3DResourceDriverPolicyResult driver_policy =
      lardon3d_resource_governor_internal_configure_driver_policy();
  return driver_policy != LARDON3D_RESOURCE_DRIVER_POLICY_FAILED &&
                 driver_policy !=
                     LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE &&
                 run_test()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
