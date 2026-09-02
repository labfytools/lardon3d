#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
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
#include <lardon3d/geometric_verifier_task.h>
#include <lardon3d/match_file.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);    \
      return false;                                                            \
    }                                                                          \
  } while (0)

static int fixture_parent_count(void) {
  const char *value = getenv("LARDON3D_TEST_GEOMETRIC_PARENT_COUNT");
  if (!value || !value[0]) {
    return 12;
  }
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  return end && *end == '\0' && parsed >= 12 && parsed <= 10000 ? (int)parsed
                                                                : 12;
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
  bool ok = true;
  for (struct dirent *entry = readdir(directory); entry;
       entry = readdir(directory)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[4096];
    int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child) ||
        !remove_tree(child)) {
      ok = false;
    }
  }
  return closedir(directory) == 0 && rmdir(path) == 0 && ok;
}

static bool fixture_join_path(char *output, size_t capacity, const char *left,
                              const char *right) {
  if (!output || capacity == 0 || !left || !right) {
    return false;
  }
  size_t left_size = strlen(left);
  size_t right_size = strlen(right);
  if (left_size == 0 || right_size == 0 || right_size > SIZE_MAX - 2 ||
      left_size > SIZE_MAX - right_size - 2 ||
      left_size + right_size + 2 > capacity) {
    return false;
  }
  memcpy(output, left, left_size);
  output[left_size] = '/';
  memcpy(output + left_size + 1, right, right_size + 1);
  return true;
}

static Lardon3DResourcePolicy policy(void) {
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

static bool wait_state(Lardon3DTaskQueue *queue, uint64_t id,
                       Lardon3DTaskState wanted,
                       Lardon3DTaskSnapshot *snapshot) {
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    if (lardon3d_task_queue_get(queue, id, snapshot) &&
        snapshot->state == wanted) {
      return true;
    }
    sched_yield();
  }
  return false;
}

static bool wait_durable(Lardon3DProjectDb *database, uint64_t id,
                         Lardon3DTaskState wanted) {
  for (size_t attempt = 0; attempt < 2000000; ++attempt) {
    Lardon3DProjectDbTask task;
    if (lardon3d_project_db_load_task(database, id, &task) ==
            LARDON3D_PROJECT_DB_OK &&
        task.saved_state == wanted) {
      return true;
    }
    sched_yield();
  }
  return false;
}

static bool exec_sql(const char *path, const char *sql) {
  sqlite3 *db = NULL;
  bool ok =
      sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
      sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(db) == SQLITE_OK && ok;
}

static bool query_integer(const char *path, const char *sql,
                          sqlite3_int64 expected) {
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  bool ok =
      sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
      sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW &&
      sqlite3_column_int64(statement, 0) == expected;
  if (statement) {
    (void)sqlite3_finalize(statement);
  }
  return sqlite3_close(db) == SQLITE_OK && ok;
}

typedef struct {
  uint64_t first_match_result_id;
  uint64_t last_match_result_id;
  char second_match_asset_path[PATH_MAX];
} FreshParentFixture;

static void fixture_image_asset_path(const unsigned char hash[32],
                                     char path[PATH_MAX]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < 32; ++index) {
    hex[2 * index] = digits[hash[index] >> 4U];
    hex[2 * index + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, PATH_MAX, "assets/images/%c%c/%s", hex[0], hex[1],
                 hex);
}

static bool fixture_register_image(Lardon3DAppState *state, uint64_t scanset_id,
                                   unsigned char seed,
                                   Lardon3DProjectDbImage *image) {
  unsigned char hash[32];
  memset(hash, seed, sizeof(hash));
  char relative[PATH_MAX];
  fixture_image_asset_path(hash, relative);
  Lardon3DProjectDbImageRegisterStatus status;
  return lardon3d_project_db_register_image(
             state->project_db, scanset_id, hash, relative, 1, "fixture.bin",
             "/fixture.bin", 0, (int64_t)seed, &status, image) ==
         LARDON3D_PROJECT_DB_OK;
}

static bool fixture_publish_features(Lardon3DAppState *state,
                                     const Lardon3DProjectDbImage *image,
                                     unsigned char salt,
                                     Lardon3DProjectDbFeatureSet *set) {
  enum { FEATURE_COUNT = 16, DESCRIPTOR_DIMENSION = 32 };
  Lardon3DFeatureKeypoint keypoints[FEATURE_COUNT];
  unsigned char descriptors[FEATURE_COUNT * DESCRIPTOR_DIMENSION];
  memset(keypoints, 0, sizeof(keypoints));
  memset(descriptors, salt, sizeof(descriptors));
  for (uint32_t index = 0; index < FEATURE_COUNT; ++index) {
    keypoints[index].x = 10.0F + (float)index;
    keypoints[index].y = 20.0F + (float)index;
    keypoints[index].size = 1.0F;
  }
  Lardon3DExtractedFeatures features = {
      .image_width = 256,
      .image_height = 256,
      .feature_count = FEATURE_COUNT,
      .keypoints = keypoints,
      .descriptors = descriptors,
      .descriptor_bytes = sizeof(descriptors),
  };
  unsigned char fingerprint[32];
  memset(fingerprint, salt, sizeof(fingerprint));
  return lardon3d_feature_store_publish_v2(
             state, image->image_id, 0, "orb", 1, fingerprint,
             LARDON3D_FEATURE_DESCRIPTOR_U8, DESCRIPTOR_DIMENSION, 0, &features,
             set) == LARDON3D_FEATURE_STORE_OK;
}

static bool fixture_sha256_file(const char *path, unsigned char output[32],
                                uint64_t *size) {
  int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
  unsigned char bytes[4096];
  uint64_t total = 0;
  for (;;) {
    ssize_t count = read(descriptor, bytes, sizeof(bytes));
    if (count < 0) {
      ok = false;
      break;
    }
    if (count == 0) {
      break;
    }
    if (total > UINT64_MAX - (uint64_t)count ||
        !context ||
        EVP_DigestUpdate(context, bytes, (size_t)count) != 1) {
      ok = false;
      break;
    }
    total += (uint64_t)count;
  }
  unsigned int digest_size = 0;
  ok = ok && context &&
       EVP_DigestFinal_ex(context, output, &digest_size) == 1 &&
       digest_size == 32;
  EVP_MD_CTX_free(context);
  if (close(descriptor) != 0) {
    ok = false;
  }
  if (ok) {
    *size = total;
  }
  return ok;
}

static bool seed_fresh_parents(Lardon3DAppState *state, size_t count,
                               FreshParentFixture *fixture) {
  if (!state || !state->project_db || !fixture || count < 2 || count > 64) {
    return false;
  }
  memset(fixture, 0, sizeof(*fixture));
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbImage image_a;
  Lardon3DProjectDbImage image_b;
  Lardon3DProjectDbFeatureSet set_a;
  Lardon3DProjectDbFeatureSet set_b;
  Lardon3DProjectDbCandidatePair pair;
  if (lardon3d_project_db_create_scanset(state->project_db, "fresh", &scanset) !=
          LARDON3D_PROJECT_DB_OK ||
      !fixture_register_image(state, scanset.scanset_id, 1, &image_a) ||
      !fixture_register_image(state, scanset.scanset_id, 2, &image_b) ||
      !fixture_publish_features(state, &image_a, 3, &set_a) ||
      !fixture_publish_features(state, &image_b, 4, &set_b) ||
      lardon3d_project_db_create_candidate_pair(
          state->project_db, image_a.image_id, image_b.image_id, 1, &pair) !=
          LARDON3D_PROJECT_DB_OK) {
    return false;
  }

  Lardon3DMatchFileEntry entries[6];
  for (uint32_t index = 0; index < 6; ++index) {
    entries[index] = (Lardon3DMatchFileEntry){
        .feature_index_a = index,
        .feature_index_b = index,
        .distance = (float)(index + 1U),
    };
  }
  for (size_t index = 0; index < count; ++index) {
    char relative[64];
    char full[PATH_MAX];
    int relative_length =
        snprintf(relative, sizeof(relative), "matches-%zu.bin", index + 1);
    int full_length = snprintf(full, sizeof(full), "%s/%s", state->project_path,
                               relative);
    if (relative_length <= 0 || (size_t)relative_length >= sizeof(relative) ||
        full_length <= 0 || (size_t)full_length >= sizeof(full)) {
      return false;
    }
    int descriptor =
        open(full, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
      return false;
    }
    bool written = lardon3d_match_file_write(
                       descriptor, LARDON3D_FEATURE_DESCRIPTOR_U8, 32,
                       set_a.feature_set_id, set_b.feature_set_id, entries, 6) ==
                   LARDON3D_MATCH_FILE_OK;
    if (close(descriptor) != 0) {
      written = false;
    }
    unsigned char asset_sha256[32];
    uint64_t asset_size = 0;
    if (!written ||
        !fixture_sha256_file(full, asset_sha256, &asset_size)) {
      return false;
    }
    unsigned char matcher_fingerprint[32];
    memset(matcher_fingerprint, (int)(index + 1),
           sizeof(matcher_fingerprint));
    Lardon3DProjectDbMatchResult parent;
    if (lardon3d_project_db_create_match_result(
            state->project_db, pair.candidate_pair_id, set_a.feature_set_id,
            set_b.feature_set_id, "fixture", 1, matcher_fingerprint,
            LARDON3D_MATCH_RESULT_STATUS_MATCHED, 6, asset_sha256, relative,
            asset_size, (int64_t)(index + 1), &parent) !=
        LARDON3D_PROJECT_DB_OK) {
      return false;
    }
    if (index == 0) {
      fixture->first_match_result_id = parent.match_result_id;
    }
    fixture->last_match_result_id = parent.match_result_id;
    if (index == 1) {
      (void)snprintf(fixture->second_match_asset_path,
                     sizeof(fixture->second_match_asset_path), "%s", full);
    }
  }
  return fixture->first_match_result_id != 0 &&
         fixture->last_match_result_id >= fixture->first_match_result_id &&
         fixture->second_match_asset_path[0] != '\0';
}

static bool seed_reusable_parents(const char *path,
                                  const unsigned char fingerprint[32]) {
  sqlite3 *db = NULL;
  CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
  static const char setup[] =
      "PRAGMA foreign_keys=ON;BEGIN IMMEDIATE;"
      "INSERT INTO scansets(name,created_at,updated_at) VALUES('task',1,1);"
      "INSERT INTO image_assets(sha256,path,size_bytes,state,created_at) "
      "VALUES(zeroblob(32),'assets/images/00/a',1,1,1),"
      "(randomblob(32),'assets/images/00/b',1,1,1);"
      "INSERT INTO "
      "images(scanset_id,asset_id,original_name,source_path,imported_at) "
      "VALUES(1,1,'a','a',1),(1,2,'b','b',1);"
      "INSERT INTO "
      "feature_assets(sha256,path,size_bytes,durability,created_at) "
      "VALUES(randomblob(32),'assets/features/a',1,0,1),"
      "(randomblob(32),'assets/features/b',1,0,1);"
      "INSERT INTO "
      "feature_sets(image_id,feature_asset_id,extractor_kind,extractor_version,"
      "parameter_fingerprint,source_image_sha256,feature_count,descriptor_type,"
      "descriptor_dimension,created_at) VALUES"
      "(1,1,'orb',1,zeroblob(32),zeroblob(32),16,1,32,1),"
      "(2,2,'orb',1,zeroblob(32),zeroblob(32),16,1,32,1);"
      "INSERT INTO candidate_pairs(image_id_a,image_id_b,created_at) "
      "VALUES(1,2,1);"
      "COMMIT;";
  CHECK(sqlite3_exec(db, setup, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_stmt *parent = NULL;
  sqlite3_stmt *result = NULL;
  CHECK(sqlite3_prepare_v2(
            db,
            "INSERT INTO "
            "match_results(candidate_pair_id,feature_set_id_a,feature_set_id_b,"
            "matcher_kind,matcher_version,parameter_fingerprint,result_status,"
            "match_count,"
            "match_asset_sha256,match_asset_path,match_asset_size_bytes,"
            "created_at) "
            "VALUES(1,1,2,?1,1,?2,1,16,?3,?4,128,1)",
            -1, &parent, NULL) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(
            db,
            "INSERT INTO "
            "geometric_verification_results(match_result_id,verifier_kind,"
            "verifier_version,parameter_fingerprint,status,inlier_count,inlier_"
            "mask,created_at) "
            "VALUES(?1,1,3,?2,1,0,zeroblob(2),1)",
            -1, &result, NULL) == SQLITE_OK);
  for (int index = 0; index < fixture_parent_count(); ++index) {
    char kind[32];
    char asset[64];
    unsigned char identity[32];
    unsigned char match_hash[32];
    memset(identity, index + 1, sizeof(identity));
    memset(match_hash, index + 33, sizeof(match_hash));
    (void)snprintf(kind, sizeof(kind), "fixture-%d", index);
    (void)snprintf(asset, sizeof(asset), "assets/matches/%d", index);
    sqlite3_bind_text(parent, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(parent, 2, identity, 32, SQLITE_TRANSIENT);
    sqlite3_bind_blob(parent, 3, match_hash, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(parent, 4, asset, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(parent) == SQLITE_DONE);
    CHECK(sqlite3_reset(parent) == SQLITE_OK);
    sqlite3_bind_int64(result, 1, sqlite3_last_insert_rowid(db));
    sqlite3_bind_blob(result, 2, fingerprint, 32, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(result) == SQLITE_DONE);
    CHECK(sqlite3_reset(result) == SQLITE_OK);
  }
  CHECK(sqlite3_finalize(parent) == SQLITE_OK);
  CHECK(sqlite3_finalize(result) == SQLITE_OK);
  return sqlite3_close(db) == SQLITE_OK;
}

static bool add_reusable_results(const char *path,
                                 uint32_t verifier_version,
                                 const unsigned char fingerprint[32]) {
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
  CHECK(
      sqlite3_prepare_v2(
          db,
          "INSERT INTO "
          "geometric_verification_results(match_result_id,verifier_kind,"
          "verifier_version,parameter_fingerprint,status,inlier_count,inlier_"
          "mask,created_at) "
          "SELECT match_result_id,1,?1,?2,1,0,zeroblob(2),2 FROM match_results",
          -1, &statement, NULL) == SQLITE_OK);
  sqlite3_bind_int64(statement, 1, verifier_version);
  sqlite3_bind_blob(statement, 2, fingerprint, 32, SQLITE_TRANSIENT);
  int code = sqlite3_step(statement);
  if (code != SQLITE_DONE) {
    (void)fprintf(stderr, "Insertion reuse: %s\n", sqlite3_errmsg(db));
  }
  CHECK(code == SQLITE_DONE);
  CHECK(sqlite3_finalize(statement) == SQLITE_OK);
  return sqlite3_close(db) == SQLITE_OK;
}

static bool update_task_fingerprint(const char *path, uint64_t task_id,
                                    const unsigned char fingerprint[32]) {
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  bool ok = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
            sqlite3_prepare_v2(
                db,
                "UPDATE geometric_verifier_tasks SET parameter_fingerprint=?1 "
                "WHERE task_id=?2",
                -1, &statement, NULL) == SQLITE_OK;
  if (ok) {
    sqlite3_bind_blob(statement, 1, fingerprint, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)task_id);
    ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1;
  }
  if (statement) {
    ok = sqlite3_finalize(statement) == SQLITE_OK && ok;
  }
  return sqlite3_close(db) == SQLITE_OK && ok;
}

static bool run_task_mid_batch_failure_regression_test(void) {
  char root[] = "/tmp/lardon3d-geometric-task-fail-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char internal[4096];
  char checkpoints[4096];
  char database_path[4096];
  CHECK(fixture_join_path(internal, sizeof(internal), root, ".lardon3d") &&
        fixture_join_path(checkpoints, sizeof(checkpoints), internal,
                          "checkpoints") &&
        fixture_join_path(database_path, sizeof(database_path), internal,
                          "project.sqlite3"));
  CHECK(mkdir(internal, 0700) == 0 && mkdir(checkpoints, 0700) == 0);

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(database_path, &state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  state.project_loaded = true;
  (void)snprintf(state.project_path, sizeof(state.project_path), "%s", root);
  state.hardware_profile = (Lardon3DHardwareProfile){
      .logical_cpu_count = 16,
      .page_size_bytes = 4096,
      .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
      .cpu_architecture = "test",
  };
  Lardon3DResourcePolicy resource_policy = policy();
  state.resource_governor = lardon3d_resource_governor_create(
      &state.hardware_profile, &resource_policy);
  state.task_queue = lardon3d_task_queue_create(state.resource_governor, 16);
  CHECK(state.resource_governor && state.task_queue);

  Lardon3DGeometricVerifierTaskConfiguration configuration = {
      .verifier = lardon3d_geometric_verifier_default_parameters(),
  };
  uint64_t invalid_task_id = 99;
  CHECK(!lardon3d_project_enqueue_geometric_verifier_task(
            NULL, &configuration, &invalid_task_id) &&
        invalid_task_id == 0);
  FreshParentFixture fresh;
  CHECK(seed_fresh_parents(&state, 16, &fresh));
  CHECK(fresh.first_match_result_id == 1 && fresh.last_match_result_id == 16);
  CHECK(query_integer(database_path,
                      "SELECT COUNT(*) FROM geometric_verification_results",
                      0));

  /* Fail after fourteen children at the independently safe CPU16 bound. Every
   * created participant must be joined and every fresh opaque stage destroyed
   * before callback-owned batch storage is released; owner publication and
   * cursor mutation stay at zero. ASan supplies the exact lifetime oracle. */
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_FORCE_PARTICIPANTS", "16", 1) == 0);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_THREAD_FAIL_AFTER", "14", 1) == 0);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                         &task_id));
  Lardon3DTaskSnapshot snapshot;
  CHECK(wait_state(state.task_queue, task_id, TASK_FAILED, &snapshot));
  CHECK(wait_durable(state.project_db, task_id, TASK_FAILED));
  CHECK(query_integer(database_path,
                      "SELECT after_match_result_id FROM "
                      "geometric_verifier_tasks WHERE task_id=(SELECT "
                      "MAX(task_id) FROM geometric_verifier_tasks)",
                      0));
  CHECK(query_integer(database_path,
                      "SELECT COUNT(*) FROM geometric_verification_results",
                      0));
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_THREAD_FAIL_AFTER") == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_FORCE_PARTICIPANTS") == 0);

  /* Index zero is a genuinely fresh, valid preparation. Removing only index
   * one's immutable Match asset makes a later slot CORRUPT; the all-slot
   * preflight must discard every successful stage before publishing index zero. */
  CHECK(unlink(fresh.second_match_asset_path) == 0);

  task_id = 0;
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                        &task_id));
  CHECK(wait_state(state.task_queue, task_id, TASK_FAILED, &snapshot));
  CHECK(wait_durable(state.project_db, task_id, TASK_FAILED));
  CHECK(snapshot.message[0] != '\0' &&
        strstr(snapshot.message, "Contrat de lot") == NULL);

  CHECK(query_integer(database_path,
                     "SELECT after_match_result_id FROM geometric_verifier_tasks "
                     "WHERE task_id=(SELECT MAX(task_id) FROM "
                     "geometric_verifier_tasks)",
                     0));

  CHECK(query_integer(database_path,
                     "SELECT COUNT(*) FROM geometric_verification_results", 0));

  lardon3d_task_queue_destroy(state.task_queue);
  lardon3d_resource_governor_destroy(state.resource_governor);
  lardon3d_project_db_close(state.project_db);
  CHECK(remove_tree(root));
  return true;
}

static bool run_engaged_control_case(bool cancel, size_t parent_count) {
  char root[] = "/tmp/lardon3d-geometric-task-control-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char internal[PATH_MAX];
  char checkpoints[PATH_MAX];
  char database_path[PATH_MAX];
  CHECK(fixture_join_path(internal, sizeof(internal), root, ".lardon3d") &&
        fixture_join_path(checkpoints, sizeof(checkpoints), internal,
                          "checkpoints") &&
        fixture_join_path(database_path, sizeof(database_path), internal,
                          "project.sqlite3"));
  CHECK(mkdir(internal, 0700) == 0 && mkdir(checkpoints, 0700) == 0);

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(database_path, &state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  state.project_loaded = true;
  (void)snprintf(state.project_path, sizeof(state.project_path), "%s", root);
  state.hardware_profile = (Lardon3DHardwareProfile){
      .logical_cpu_count = 16,
      .page_size_bytes = 4096,
      .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
      .cpu_architecture = "test",
  };
  Lardon3DResourcePolicy resource_policy = policy();
  state.resource_governor = lardon3d_resource_governor_create(
      &state.hardware_profile, &resource_policy);
  state.task_queue = lardon3d_task_queue_create(state.resource_governor, 16);
  CHECK(state.resource_governor && state.task_queue);

  FreshParentFixture fresh;
  CHECK(seed_fresh_parents(&state, parent_count, &fresh));
  CHECK(fresh.first_match_result_id == 1 &&
        fresh.last_match_result_id == parent_count);
  Lardon3DGeometricVerifierTaskConfiguration configuration = {
      .verifier = lardon3d_geometric_verifier_default_parameters(),
  };
  lardon3d_geometric_verifier_task_test_arm_prepublication_barrier();
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                         &task_id));
  bool reached =
      lardon3d_geometric_verifier_task_test_wait_prepublication_barrier();
  if (!reached) {
    lardon3d_geometric_verifier_task_test_release_prepublication_barrier();
  }
  CHECK(reached);
  bool requested = cancel
                       ? lardon3d_task_queue_cancel(state.task_queue, task_id)
                       : lardon3d_task_queue_pause(state.task_queue, task_id);
  lardon3d_geometric_verifier_task_test_release_prepublication_barrier();
  CHECK(requested);

  Lardon3DTaskSnapshot snapshot;
  Lardon3DTaskState expected_state = cancel ? TASK_CANCELLED : TASK_PAUSED;
  CHECK(wait_state(state.task_queue, task_id, expected_state, &snapshot));
  if (cancel) {
    CHECK(wait_durable(state.project_db, task_id, TASK_CANCELLED));
  }
  const uint64_t engaged_cursor = fresh.first_match_result_id + 15;
  Lardon3DProjectDbGeometricVerifierTask durable;
  CHECK(lardon3d_project_db_load_geometric_verifier_task(
            state.project_db, task_id, &durable) == LARDON3D_PROJECT_DB_OK &&
        durable.after_match_result_id == engaged_cursor);
  CHECK(query_integer(database_path,
                      "SELECT COUNT(*) FROM geometric_verification_results",
                      16));
  char later_results_sql[256];
  int sql_length = snprintf(
      later_results_sql, sizeof(later_results_sql),
      "SELECT COUNT(*) FROM geometric_verification_results WHERE "
      "match_result_id>%llu",
      (unsigned long long)engaged_cursor);
  CHECK(sql_length > 0 && (size_t)sql_length < sizeof(later_results_sql) &&
        query_integer(database_path, later_results_sql, 0));

  Lardon3DProjectDbTask generic;
  CHECK(lardon3d_project_db_load_task(state.project_db, task_id, &generic) ==
        LARDON3D_PROJECT_DB_OK);
  unsigned int engaged_progress = parent_count == 16 ? 100U : 99U;
  CHECK(generic.progress == engaged_progress);
  if (cancel) {
    CHECK(generic.saved_state == TASK_CANCELLED);
  } else {
    /* The durable RUNNING snapshot is the post-batch checkpoint. Only after it
     * commits does the in-memory Task honor the pending pause. */
    CHECK(generic.saved_state == TASK_RUNNING);
    CHECK(lardon3d_task_queue_resume(state.task_queue, task_id));
    CHECK(wait_durable(state.project_db, task_id, TASK_COMPLETED));
    CHECK(lardon3d_project_db_load_geometric_verifier_task(
              state.project_db, task_id, &durable) == LARDON3D_PROJECT_DB_OK &&
          durable.after_match_result_id == fresh.last_match_result_id);
    CHECK(query_integer(database_path,
                        "SELECT COUNT(*) FROM geometric_verification_results",
                        (sqlite3_int64)parent_count));
  }

  lardon3d_task_queue_destroy(state.task_queue);
  lardon3d_resource_governor_destroy(state.resource_governor);
  lardon3d_project_db_close(state.project_db);
  CHECK(remove_tree(root));
  return true;
}

static bool run_engaged_control_regression_tests(void) {
  /* Sixteen exercises the explicit exhausted-batch checkpoint. Seventeen
   * leaves one undispatched parent, proving pause/cancel cannot start a second
   * batch before the engaged prefix becomes the control boundary. */
  return run_engaged_control_case(false, 16) &&
         run_engaged_control_case(true, 16) &&
         run_engaged_control_case(false, 17) &&
         run_engaged_control_case(true, 17);
}

static bool run_task_test(void) {
  char root[] = "/tmp/lardon3d-geometric-task-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char internal[4096];
  char checkpoints[4096];
  char database_path[4096];
  CHECK(fixture_join_path(internal, sizeof(internal), root, ".lardon3d") &&
        fixture_join_path(checkpoints, sizeof(checkpoints), internal,
                          "checkpoints") &&
        fixture_join_path(database_path, sizeof(database_path), internal,
                          "project.sqlite3"));
  CHECK(mkdir(internal, 0700) == 0 && mkdir(checkpoints, 0700) == 0);

  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(database_path, &state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  state.project_loaded = true;
  (void)snprintf(state.project_path, sizeof(state.project_path), "%s", root);
  state.hardware_profile = (Lardon3DHardwareProfile){
      .logical_cpu_count = 16,
      .page_size_bytes = 4096,
      .memory_total_bytes = 16ULL * 1024 * 1024 * 1024,
      .cpu_architecture = "test",
  };
  Lardon3DResourcePolicy resource_policy = policy();
  state.resource_governor = lardon3d_resource_governor_create(
      &state.hardware_profile, &resource_policy);
  state.task_queue = lardon3d_task_queue_create(state.resource_governor, 16);
  CHECK(state.resource_governor && state.task_queue);

  Lardon3DGeometricVerifierTaskConfiguration configuration = {
      .verifier = lardon3d_geometric_verifier_default_parameters(),
  };
  unsigned char fingerprint[32];
  lardon3d_geometric_verifier_fingerprint(&configuration.verifier, fingerprint);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_PARENT_COUNT", "32", 1) == 0);
  CHECK(seed_reusable_parents(database_path, fingerprint));

  const Lardon3DTaskKindRegistry *registry =
      lardon3d_task_kind_registry_production();
  const Lardon3DTaskKindDescriptor *descriptor = NULL;
  CHECK(lardon3d_task_kind_registry_lookup(
            registry, LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND, 1, &descriptor) ==
        LARDON3D_TASK_KIND_OK);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                         &task_id));
  Lardon3DTaskSnapshot snapshot;
  CHECK(wait_durable(state.project_db, task_id, TASK_COMPLETED));
  Lardon3DProjectDbGeometricVerifierTask durable;
  CHECK(lardon3d_project_db_load_geometric_verifier_task(
            state.project_db, task_id, &durable) == LARDON3D_PROJECT_DB_OK &&
        durable.after_match_result_id == (uint64_t)fixture_parent_count() &&
        memcmp(durable.parameter_fingerprint, fingerprint, 32) == 0);

  /* Start with only one compute CPU, acknowledge the first durable sequence,
   * then enlarge the host pool before the real sequence_break. Exact reuse
   * intentionally records zero scientific work, so Governor slow-start stays
   * at CPU1; the sequence count proves fresh admission without manufacturing
   * throughput evidence. */
  resource_policy.system_cpu_reserve = 15;
  CHECK(lardon3d_resource_governor_set_policy(state.resource_governor,
                                              &resource_policy));
  lardon3d_geometric_verifier_task_test_reset_cpu_contracts();
  lardon3d_geometric_verifier_task_test_arm_sequence_barrier();
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(
      &state, &configuration, &task_id));
  CHECK(lardon3d_geometric_verifier_task_test_wait_sequence_barrier());
  CHECK(lardon3d_project_db_load_geometric_verifier_task(
            state.project_db, task_id, &durable) == LARDON3D_PROJECT_DB_OK &&
        durable.after_match_result_id > 0 &&
        durable.after_match_result_id < (uint64_t)fixture_parent_count());
  resource_policy.system_cpu_reserve = 8;
  CHECK(lardon3d_resource_governor_set_policy(state.resource_governor,
                                              &resource_policy));
  lardon3d_geometric_verifier_task_test_release_sequence_barrier();
  CHECK(wait_durable(state.project_db, task_id, TASK_COMPLETED));
  CHECK(lardon3d_geometric_verifier_task_test_cpu_contracts() == (1U << 1));
  Lardon3DProjectDbTask readmitted_task;
  CHECK(lardon3d_project_db_load_task(state.project_db, task_id,
                                      &readmitted_task) ==
            LARDON3D_PROJECT_DB_OK &&
        readmitted_task.sequence_count > 0);
  resource_policy.system_cpu_reserve = 4;
  CHECK(lardon3d_resource_governor_set_policy(state.resource_governor,
                                              &resource_policy));

  configuration.verifier.threshold_pixels = 1.6;
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &configuration.verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2,
      fingerprint));
  CHECK(add_reusable_results(database_path,
                             LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2,
                             fingerprint));
  unsigned char current_fingerprint[32];
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &configuration.verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
      current_fingerprint));
  CHECK(add_reusable_results(database_path,
                             LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
                             current_fingerprint));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                         &task_id));
  CHECK(wait_state(state.task_queue, task_id, TASK_PAUSED, &snapshot));
  CHECK(lardon3d_project_db_load_geometric_verifier_task(
            state.project_db, task_id, &durable) == LARDON3D_PROJECT_DB_OK &&
        durable.after_match_result_id == 0);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_SKIP_FINISHED_CHECKPOINT", "1", 1) ==
        0);
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_db_close(state.project_db);
  state.project_db = NULL;
  // Simulate a historical v1 durable row at the crash boundary. Recovery must
  // derive v1 from this exact fingerprint without a schema-version column.
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &configuration.verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1,
      fingerprint));
  CHECK(update_task_fingerprint(database_path, task_id, fingerprint));
  CHECK(add_reusable_results(database_path,
                             LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1,
                             fingerprint));
  CHECK(lardon3d_project_db_open(database_path, &state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  state.task_queue = lardon3d_task_queue_create(state.resource_governor, 16);
  CHECK(state.task_queue != NULL);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION") == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_SKIP_FINISHED_CHECKPOINT") == 0);
  Lardon3DProjectRecoverySummary recovery;
  Lardon3DProjectDbResult recovery_result =
      lardon3d_project_resume_recoverable_tasks(
          &state, lardon3d_task_kind_registry_production(), &recovery);
  if (recovery_result != LARDON3D_PROJECT_DB_OK || recovery.resumed != 1) {
    (void)fprintf(
        stderr,
        "Recovery result=%d inspected=%zu resumed=%zu skipped=%zu failed=%zu\n",
        recovery_result, recovery.inspected, recovery.resumed, recovery.skipped,
        recovery.failed);
  }
  CHECK(recovery_result == LARDON3D_PROJECT_DB_OK && recovery.resumed == 1);
  CHECK(wait_durable(state.project_db, task_id, TASK_COMPLETED));
  CHECK(lardon3d_project_db_load_geometric_verifier_task(
            state.project_db, task_id, &durable) == LARDON3D_PROJECT_DB_OK &&
        durable.after_match_result_id == (uint64_t)fixture_parent_count());

  configuration.verifier.threshold_pixels = 1.7;
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &configuration.verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2,
      fingerprint));
  CHECK(add_reusable_results(database_path,
                             LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2,
                             fingerprint));
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &configuration.verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
      current_fingerprint));
  CHECK(add_reusable_results(database_path,
                             LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
                             current_fingerprint));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                         &task_id));
  CHECK(wait_state(state.task_queue, task_id, TASK_PAUSED, &snapshot));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_SKIP_FINISHED_CHECKPOINT", "1", 1) ==
        0);
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_db_close(state.project_db);
  state.project_db = NULL;
  // Historical v2 reconstruction uses its exact immutable fingerprint; the
  // current v3 task identity must never relabel this durable row.
  CHECK(update_task_fingerprint(database_path, task_id, fingerprint));
  CHECK(lardon3d_project_db_open(database_path, &state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  state.task_queue = lardon3d_task_queue_create(state.resource_governor, 16);
  CHECK(state.task_queue != NULL);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION") == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_SKIP_FINISHED_CHECKPOINT") == 0);
  recovery_result = lardon3d_project_resume_recoverable_tasks(
      &state, lardon3d_task_kind_registry_production(), &recovery);
  CHECK(recovery_result == LARDON3D_PROJECT_DB_OK && recovery.resumed == 1);
  CHECK(wait_durable(state.project_db, task_id, TASK_COMPLETED));

  configuration.verifier.threshold_pixels = 1.8;
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &configuration.verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
      fingerprint));
  CHECK(add_reusable_results(database_path,
                             LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
                             fingerprint));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION", "1", 1) == 0);
  CHECK(lardon3d_project_enqueue_geometric_verifier_task(&state, &configuration,
                                                         &task_id));
  CHECK(wait_state(state.task_queue, task_id, TASK_PAUSED, &snapshot));
  CHECK(lardon3d_task_queue_cancel(state.task_queue, task_id));
  CHECK(wait_state(state.task_queue, task_id, TASK_CANCELLED, &snapshot));
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION") == 0);

  lardon3d_task_queue_destroy(state.task_queue);
  lardon3d_resource_governor_destroy(state.resource_governor);
  lardon3d_project_db_close(state.project_db);
  CHECK(exec_sql(
      database_path,
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE IF EXISTS feature_extract_batch_tasks;"
      "DROP TABLE IF EXISTS raw_development_batch_tasks;"
      "DROP TABLE IF EXISTS capture_calibration_selections;"
      "DROP TABLE IF EXISTS optical_calibration_profiles;"
      "DROP TABLE IF EXISTS capture_optical_configurations;"
      "DROP INDEX IF EXISTS acquisition_campaign_capture_identity_v23;"
      "DROP TABLE IF EXISTS acquisition_campaign_group_optics;"
      "DROP TABLE IF EXISTS optical_configurations;"
      "DROP TABLE IF EXISTS lens_profile_aliases;DROP TABLE IF EXISTS lens_profiles;"
      "DROP TABLE IF EXISTS camera_body_aliases;DROP TABLE IF EXISTS camera_body_profiles;"
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
      "DROP TABLE track_observations;"
      "DROP TABLE tracks;"
      "DROP TABLE track_sets;"
      "DROP TABLE track_builder_tasks;"
      "UPDATE metadata SET value=12 WHERE key='schema_version';COMMIT;"));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V13", "1", 1) == 0);
  Lardon3DProjectDb *database = NULL;
  CHECK(lardon3d_project_db_open(database_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V13") == 0);
  CHECK(query_integer(database_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      12));
  CHECK(
      query_integer(database_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='geometric_verifier_tasks'",
                    0));
  CHECK(lardon3d_project_db_open(database_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == LARDON3D_PROJECT_DB_SCHEMA_VERSION);
  lardon3d_project_db_close(database);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_PARENT_COUNT") == 0);
  CHECK(remove_tree(root));
  return true;
}

int main(void) {
  return (run_task_test() && run_task_mid_batch_failure_regression_test() &&
          run_engaged_control_regression_tests())
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
