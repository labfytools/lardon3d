#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/geometric_verifier_task.h>
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
            "VALUES(?1,1,1,?2,1,0,zeroblob(2),1)",
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
          "SELECT match_result_id,1,1,?1,1,0,zeroblob(2),2 FROM match_results",
          -1, &statement, NULL) == SQLITE_OK);
  sqlite3_bind_blob(statement, 1, fingerprint, 32, SQLITE_TRANSIENT);
  int code = sqlite3_step(statement);
  if (code != SQLITE_DONE) {
    (void)fprintf(stderr, "Insertion reuse: %s\n", sqlite3_errmsg(db));
  }
  CHECK(code == SQLITE_DONE);
  CHECK(sqlite3_finalize(statement) == SQLITE_OK);
  return sqlite3_close(db) == SQLITE_OK;
}

static bool run_task_test(void) {
  char root[] = "/tmp/lardon3d-geometric-task-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char internal[4096];
  char checkpoints[4096];
  char database_path[4096];
  (void)snprintf(internal, sizeof(internal), "%s/.lardon3d", root);
  (void)snprintf(checkpoints, sizeof(checkpoints), "%s/checkpoints", internal);
  (void)snprintf(database_path, sizeof(database_path), "%s/project.sqlite3",
                 internal);
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

  configuration.verifier.threshold_pixels = 1.6;
  lardon3d_geometric_verifier_fingerprint(&configuration.verifier, fingerprint);
  CHECK(add_reusable_results(database_path, fingerprint));
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
  lardon3d_geometric_verifier_fingerprint(&configuration.verifier, fingerprint);
  CHECK(add_reusable_results(database_path, fingerprint));
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
  CHECK(lardon3d_project_db_schema_version(database) == 15);
  lardon3d_project_db_close(database);
  CHECK(remove_tree(root));
  return true;
}

int main(void) { return run_task_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
