#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <lardon3d/project_db.h>

struct Lardon3DProjectDb {
  sqlite3 *connection;
  pthread_mutex_t mutex;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
};

static const char schema_v5[] =
    "CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
    "INSERT INTO metadata(key,value) VALUES('schema_version',5);"
    "INSERT INTO metadata(key,value) VALUES('next_task_id',1);"
    "INSERT INTO metadata(key,value) VALUES('legacy_image_catalog_pending',0);"
    "CREATE TABLE project(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
    "stable_id TEXT NOT NULL UNIQUE,name TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at "
    "INTEGER NOT NULL);"
    "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY CHECK(task_id>0),name TEXT NOT NULL,"
    "task_kind TEXT,task_kind_version INTEGER,"
    "saved_state INTEGER NOT NULL CHECK(saved_state BETWEEN 0 AND 5),"
    "recovery_state INTEGER NOT NULL CHECK(recovery_state BETWEEN 0 AND 5),"
    "progress INTEGER NOT NULL CHECK(progress BETWEEN 0 AND 100),sequence_count INTEGER NOT NULL "
    "CHECK(sequence_count>=0),"
    "started_sec INTEGER NOT NULL,started_nsec INTEGER NOT NULL CHECK(started_nsec BETWEEN 0 AND "
    "999999999),"
    "finished_sec INTEGER NOT NULL,finished_nsec INTEGER NOT NULL CHECK(finished_nsec BETWEEN 0 "
    "AND 999999999),updated_at INTEGER NOT NULL,"
    "CHECK((task_kind IS NULL AND task_kind_version IS NULL) OR (task_kind IS NOT NULL AND "
    "task_kind_version>0)));"
    "CREATE INDEX tasks_recovery_state_idx ON tasks(recovery_state,task_id);"
    "CREATE TABLE checkpoints(task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE "
    "CASCADE,"
    "path TEXT NOT NULL,format_version INTEGER NOT NULL CHECK(format_version>0),"
    "durability INTEGER NOT NULL CHECK(durability BETWEEN 0 AND 1),updated_at INTEGER NOT NULL);"
    "CREATE TABLE artifacts(artifact_id TEXT PRIMARY KEY,kind TEXT NOT NULL,path TEXT NOT NULL,"
    "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 1),size_bytes INTEGER NOT NULL "
    "CHECK(size_bytes>=0),"
    "producer_task_id INTEGER REFERENCES tasks(task_id),created_at INTEGER NOT NULL,updated_at "
    "INTEGER NOT NULL);"
    "CREATE INDEX artifacts_state_idx ON artifacts(state,artifact_id);"
    "CREATE INDEX artifacts_producer_idx ON artifacts(producer_task_id);"
    "CREATE TABLE scansets(scanset_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(scanset_id>0),"
    "name TEXT NOT NULL CHECK(length(name)>0 AND length(name)<256),created_at INTEGER NOT NULL "
    "CHECK(created_at>=0),"
    "updated_at INTEGER NOT NULL CHECK(updated_at>=created_at));"
    "CREATE TABLE image_assets(asset_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(asset_id>0),sha256 "
    "BLOB NOT NULL UNIQUE CHECK(length(sha256)=32),"
    "path TEXT NOT NULL UNIQUE CHECK(length(path)>0 AND length(path)<4096),size_bytes INTEGER NOT "
    "NULL CHECK(size_bytes>=0),"
    "state INTEGER NOT NULL CHECK(state=1),created_at INTEGER NOT NULL CHECK(created_at>=0));"
    "CREATE TABLE images(image_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(image_id>0),scanset_id "
    "INTEGER NOT NULL REFERENCES scansets(scanset_id),"
    "asset_id INTEGER NOT NULL REFERENCES image_assets(asset_id),original_name TEXT NOT NULL "
    "CHECK(length(original_name)>0 AND length(original_name)<256),"
    "source_path TEXT NOT NULL CHECK(length(source_path)>0 AND length(source_path)<4096),"
    "producer_task_id INTEGER REFERENCES tasks(task_id),imported_at INTEGER NOT NULL "
    "CHECK(imported_at>=0),"
    "UNIQUE(scanset_id,asset_id));"
    "CREATE INDEX images_scanset_idx ON images(scanset_id,image_id);"
    "CREATE INDEX images_producer_idx ON images(producer_task_id,image_id);"
    "CREATE TABLE image_import_tasks(task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON "
    "DELETE CASCADE,"
    "source_path TEXT NOT NULL,scanset_id INTEGER NOT NULL REFERENCES scansets(scanset_id));";

static const char schema_feature_v5[] =
    "CREATE TABLE feature_assets(feature_asset_id INTEGER PRIMARY KEY AUTOINCREMENT "
    "CHECK(feature_asset_id>0),sha256 BLOB NOT NULL UNIQUE CHECK(length(sha256)=32),path TEXT NOT "
    "NULL UNIQUE CHECK(length(path)>0 AND length(path)<4096),size_bytes INTEGER NOT NULL "
    "CHECK(size_bytes>=0),durability INTEGER NOT NULL CHECK(durability BETWEEN 0 AND 1),created_at "
    "INTEGER NOT NULL CHECK(created_at>=0));"
    "CREATE TABLE feature_sets(feature_set_id INTEGER PRIMARY KEY AUTOINCREMENT "
    "CHECK(feature_set_id>0),image_id INTEGER NOT NULL REFERENCES "
    "images(image_id),feature_asset_id INTEGER NOT NULL REFERENCES "
    "feature_assets(feature_asset_id),extractor_kind TEXT NOT NULL CHECK(length(extractor_kind)>0 "
    "AND length(extractor_kind)<65),extractor_version INTEGER NOT NULL "
    "CHECK(extractor_version>0),parameter_fingerprint BLOB NOT NULL "
    "CHECK(length(parameter_fingerprint)=32),source_image_sha256 BLOB NOT NULL "
    "CHECK(length(source_image_sha256)=32),feature_count INTEGER NOT NULL "
    "CHECK(feature_count BETWEEN 0 AND 8192),descriptor_type INTEGER NOT NULL "
    "CHECK(descriptor_type "
    "IN(1,2)),descriptor_dimension INTEGER NOT NULL "
    "CHECK(descriptor_dimension BETWEEN 1 AND 4096),producer_task_id "
    "INTEGER REFERENCES tasks(task_id),created_at INTEGER NOT NULL "
    "CHECK(created_at>=0),UNIQUE(image_id,extractor_kind,extractor_version,parameter_fingerprint));"
    "CREATE INDEX feature_sets_image_idx ON feature_sets(image_id,feature_set_id);"
    "CREATE INDEX feature_sets_producer_idx ON feature_sets(producer_task_id,feature_set_id);"
    "CREATE TABLE feature_extract_tasks(task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON "
    "DELETE CASCADE,image_id INTEGER NOT NULL REFERENCES images(image_id),extractor_kind TEXT NOT "
    "NULL,extractor_version INTEGER NOT NULL CHECK(extractor_version>0),max_features INTEGER NOT "
    "NULL CHECK(max_features BETWEEN 1 AND 8192),pyramid_levels INTEGER NOT NULL "
    "CHECK(pyramid_levels BETWEEN 1 AND 16),fast_threshold INTEGER NOT NULL "
    "CHECK(fast_threshold BETWEEN 1 AND 255),parameter_fingerprint BLOB NOT NULL "
    "CHECK(length(parameter_fingerprint)=32));";

static const char schema_visual_v6[] =
    "CREATE TABLE visual_indexes(visual_index_id INTEGER PRIMARY KEY AUTOINCREMENT "
    "CHECK(visual_index_id>0),index_kind TEXT NOT NULL CHECK(length(index_kind)>0 AND "
    "length(index_kind)<65),index_version INTEGER NOT NULL CHECK(index_version>0),"
    "descriptor_type INTEGER NOT NULL CHECK(descriptor_type IN(1,2)),descriptor_dimension "
    "INTEGER NOT NULL CHECK(descriptor_dimension BETWEEN 1 AND 4096),extractor_kind TEXT NOT "
    "NULL CHECK(length(extractor_kind)>0 AND length(extractor_kind)<65),extractor_version "
    "INTEGER NOT NULL CHECK(extractor_version>0),feature_parameter_fingerprint BLOB NOT NULL "
    "CHECK(length(feature_parameter_fingerprint)=32),index_parameter_fingerprint BLOB NOT NULL "
    "CHECK(length(index_parameter_fingerprint)=32),table_count INTEGER NOT NULL CHECK(table_count "
    "BETWEEN 1 AND 32),key_bits INTEGER NOT NULL CHECK(key_bits BETWEEN 8 AND 32),"
    "max_features_per_set INTEGER NOT NULL CHECK(max_features_per_set BETWEEN 1 AND 1024),"
    "max_bucket_postings INTEGER NOT NULL CHECK(max_bucket_postings BETWEEN 1 AND 4096),"
    "created_at INTEGER NOT NULL CHECK(created_at>=0),UNIQUE(index_kind,index_version,"
    "descriptor_type,descriptor_dimension,extractor_kind,extractor_version,"
    "feature_parameter_fingerprint,index_parameter_fingerprint));"
    "CREATE TABLE visual_index_segments(visual_index_segment_id INTEGER PRIMARY KEY "
    "AUTOINCREMENT CHECK(visual_index_segment_id>0),visual_index_id INTEGER NOT NULL REFERENCES "
    "visual_indexes(visual_index_id),generation INTEGER NOT NULL CHECK(generation>0),sha256 BLOB "
    "NOT NULL CHECK(length(sha256)=32),path TEXT NOT NULL UNIQUE CHECK(length(path)>0 AND "
    "length(path)<4096),size_bytes INTEGER NOT NULL CHECK(size_bytes>=128),posting_count INTEGER "
    "NOT NULL CHECK(posting_count>=0),feature_set_count INTEGER NOT NULL CHECK(feature_set_count "
    "BETWEEN 1 AND 16),durability INTEGER NOT NULL CHECK(durability BETWEEN 0 AND 1),"
    "producer_task_id INTEGER REFERENCES tasks(task_id),created_at INTEGER NOT NULL "
    "CHECK(created_at>=0),UNIQUE(visual_index_id,generation),UNIQUE(visual_index_id,sha256));"
    "CREATE INDEX visual_index_segments_index_idx ON visual_index_segments(visual_index_id,"
    "generation);"
    "CREATE TABLE visual_index_memberships(visual_index_id INTEGER NOT NULL REFERENCES "
    "visual_indexes(visual_index_id),feature_set_id INTEGER NOT NULL REFERENCES "
    "feature_sets(feature_set_id),visual_index_segment_id INTEGER NOT NULL REFERENCES "
    "visual_index_segments(visual_index_segment_id),PRIMARY KEY(visual_index_id,feature_set_id));"
    "CREATE INDEX visual_index_memberships_segment_idx ON visual_index_memberships("
    "visual_index_segment_id,feature_set_id);"
    "CREATE TABLE visual_index_update_tasks(task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) "
    "ON DELETE CASCADE,visual_index_id INTEGER NOT NULL REFERENCES visual_indexes(visual_index_id),"
    "after_feature_set_id INTEGER NOT NULL CHECK(after_feature_set_id>=0));";

static void copy_error(char destination[LARDON3D_PROJECT_DB_ERROR_CAPACITY], const char *text) {
  if (destination) {
    (void)snprintf(destination, LARDON3D_PROJECT_DB_ERROR_CAPACITY, "%s", text ? text : "");
  }
}

static Lardon3DProjectDbResult sqlite_result(Lardon3DProjectDb *database, int code,
                                             const char *context) {
  (void)snprintf(database->error, sizeof(database->error), "%s: %s", context,
                 sqlite3_errmsg(database->connection));
  if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
    return LARDON3D_PROJECT_DB_BUSY;
  }
  if (code == SQLITE_CONSTRAINT) {
    return LARDON3D_PROJECT_DB_CONSTRAINT;
  }
  if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB) {
    return LARDON3D_PROJECT_DB_CORRUPT;
  }
  return LARDON3D_PROJECT_DB_IO_ERROR;
}

static bool bounded_text(const char *text, size_t capacity, bool allow_empty) {
  if (!text) {
    return false;
  }
  size_t length = strnlen(text, capacity);
  return length < capacity && (allow_empty || length > 0);
}

static bool valid_task_id(uint64_t id) { return id > 0 && id <= INT64_MAX; }

static bool valid_state(Lardon3DTaskState state) {
  return state >= TASK_PENDING && state <= TASK_COMPLETED;
}

static bool database_time(sqlite3_int64 value, time_t *output) {
  if (value < 0) {
    return false;
  }
  time_t converted = (time_t)value;
  if (converted < 0 || (uint64_t)converted != (uint64_t)value) {
    return false;
  }
  *output = converted;
  return true;
}

static Lardon3DProjectDbResult execute(Lardon3DProjectDb *database, const char *sql,
                                       const char *context) {
  char *message = NULL;
  int code = sqlite3_exec(database->connection, sql, NULL, NULL, &message);
  if (code == SQLITE_OK) {
    return LARDON3D_PROJECT_DB_OK;
  }
  (void)snprintf(database->error, sizeof(database->error), "%s: %s", context,
                 message ? message : sqlite3_errmsg(database->connection));
  sqlite3_free(message);
  return sqlite_result(database, code, context);
}

static Lardon3DProjectDbResult prepare(Lardon3DProjectDb *database, const char *sql,
                                       sqlite3_stmt **statement) {
  int code = sqlite3_prepare_v2(database->connection, sql, -1, statement, NULL);
  return code == SQLITE_OK ? LARDON3D_PROJECT_DB_OK : sqlite_result(database, code, "prepare");
}

static bool table_exists(sqlite3 *connection, const char *name) {
  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(connection, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                         -1, &statement, NULL) != SQLITE_OK) {
    return false;
  }
  (void)sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  bool exists = sqlite3_step(statement) == SQLITE_ROW;
  (void)sqlite3_finalize(statement);
  return exists;
}

static Lardon3DProjectDbResult migrate(Lardon3DProjectDb *database, unsigned int from_version) {
  if (from_version > LARDON3D_PROJECT_DB_SCHEMA_VERSION) {
    copy_error(database->error, "Version de schéma future non supportée.");
    return LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA;
  }
  if (from_version == LARDON3D_PROJECT_DB_SCHEMA_VERSION) {
    return LARDON3D_PROJECT_DB_OK;
  }
  if (from_version != 0 && from_version != 1 && from_version != 2 && from_version != 3 &&
      from_version != 4 && from_version != 5) {
    return LARDON3D_PROJECT_DB_CORRUPT;
  }
  Lardon3DProjectDbResult result = execute(database, "BEGIN IMMEDIATE", "begin migration");
  if (result == LARDON3D_PROJECT_DB_OK && from_version == 0) {
    result = execute(database, schema_v5, "create schema v5");
    if (result == LARDON3D_PROJECT_DB_OK) {
      result = execute(database, schema_feature_v5, "create feature schema v5");
    }
    if (result == LARDON3D_PROJECT_DB_OK) {
      result = execute(database, schema_visual_v6, "create visual index schema v6");
    }
    if (result == LARDON3D_PROJECT_DB_OK) {
      result = execute(database,
                       "UPDATE metadata SET value=6 WHERE key='schema_version' AND value=5",
                       "finish new schema v6");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && from_version == 1) {
    result = execute(database,
                     "ALTER TABLE tasks ADD COLUMN task_kind TEXT;"
                     "ALTER TABLE tasks ADD COLUMN task_kind_version INTEGER "
                     "CHECK(task_kind_version IS NULL OR task_kind_version>0)",
                     "migrate schema v1 to v2");
#ifdef LARDON3D_PROJECT_DB_TESTING
    const char *forced_failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V2");
    if (result == LARDON3D_PROJECT_DB_OK && forced_failure && strcmp(forced_failure, "1") == 0) {
      result =
          execute(database, "INSERT INTO missing_test_table VALUES(1)", "forced migration failure");
    }
#endif
    if (result == LARDON3D_PROJECT_DB_OK) {
      result =
          execute(database, "UPDATE metadata SET value=2 WHERE key='schema_version' AND value=1",
                  "finish schema v2 migration");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && from_version != 0 && from_version < 3) {
    result = execute(database,
                     "CREATE TABLE image_import_tasks(task_id INTEGER PRIMARY KEY REFERENCES "
                     "tasks(task_id) ON DELETE CASCADE,source_path TEXT NOT NULL);"
                     "INSERT INTO metadata(key,value) VALUES('next_task_id',(SELECT CASE WHEN "
                     "COALESCE(MAX(task_id),0)>=9223372036854775807 THEN 0 ELSE "
                     "COALESCE(MAX(task_id),0)+1 END FROM tasks))",
                     "migrate schema v2 to v3");
#ifdef LARDON3D_PROJECT_DB_TESTING
    const char *forced_failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V3");
    if (result == LARDON3D_PROJECT_DB_OK && forced_failure && strcmp(forced_failure, "1") == 0) {
      result =
          execute(database, "INSERT INTO missing_test_table VALUES(1)", "forced migration failure");
    }
#endif
    if (result == LARDON3D_PROJECT_DB_OK) {
      result =
          execute(database, "UPDATE metadata SET value=3 WHERE key='schema_version' AND value=2",
                  "finish schema v3 migration");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && from_version != 0 && from_version < 4) {
    result = execute(
        database,
        "CREATE TABLE scansets(scanset_id INTEGER PRIMARY KEY AUTOINCREMENT "
        "CHECK(scanset_id>0),name TEXT NOT NULL CHECK(length(name)>0 AND "
        "length(name)<256),created_at INTEGER NOT NULL CHECK(created_at>=0),updated_at INTEGER "
        "NOT NULL CHECK(updated_at>=created_at));"
        "CREATE TABLE image_assets(asset_id INTEGER PRIMARY KEY AUTOINCREMENT "
        "CHECK(asset_id>0),sha256 BLOB NOT NULL UNIQUE CHECK(length(sha256)=32),path TEXT NOT "
        "NULL UNIQUE CHECK(length(path)>0 AND length(path)<4096),size_bytes INTEGER NOT NULL "
        "CHECK(size_bytes>=0),state INTEGER NOT NULL CHECK(state=1),created_at INTEGER NOT "
        "NULL CHECK(created_at>=0));"
        "CREATE TABLE images(image_id INTEGER PRIMARY KEY AUTOINCREMENT "
        "CHECK(image_id>0),scanset_id INTEGER NOT NULL REFERENCES "
        "scansets(scanset_id),asset_id INTEGER NOT NULL REFERENCES "
        "image_assets(asset_id),original_name TEXT NOT NULL CHECK(length(original_name)>0 AND "
        "length(original_name)<256),source_path TEXT NOT NULL CHECK(length(source_path)>0 AND "
        "length(source_path)<4096),producer_task_id INTEGER REFERENCES "
        "tasks(task_id),imported_at INTEGER NOT NULL "
        "CHECK(imported_at>=0),UNIQUE(scanset_id,asset_id));"
        "CREATE INDEX images_scanset_idx ON images(scanset_id,image_id);"
        "CREATE INDEX images_producer_idx ON images(producer_task_id,image_id);"
        "ALTER TABLE image_import_tasks ADD COLUMN scanset_id INTEGER REFERENCES "
        "scansets(scanset_id);"
        "INSERT INTO scansets(name,created_at,updated_at) SELECT 'Imports antérieurs à ScanSet "
        "v1',0,0 WHERE EXISTS(SELECT 1 FROM image_import_tasks);"
        "UPDATE image_import_tasks SET scanset_id=(SELECT scanset_id FROM scansets WHERE "
        "name='Imports antérieurs à ScanSet v1' ORDER BY scanset_id LIMIT 1) WHERE scanset_id "
        "IS NULL;"
        "INSERT INTO metadata(key,value) VALUES('legacy_image_catalog_pending',CASE WHEN "
        "EXISTS(SELECT 1 FROM image_import_tasks) THEN 1 ELSE 0 END)",
        "migrate schema v3 to v4");
#ifdef LARDON3D_PROJECT_DB_TESTING
    const char *forced_failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V4");
    if (result == LARDON3D_PROJECT_DB_OK && forced_failure && strcmp(forced_failure, "1") == 0) {
      result =
          execute(database, "INSERT INTO missing_test_table VALUES(1)", "forced migration failure");
    }
#endif
    if (result == LARDON3D_PROJECT_DB_OK) {
      result =
          execute(database, "UPDATE metadata SET value=4 WHERE key='schema_version' AND value=3",
                  "finish schema v4 migration");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && from_version != 0 && from_version < 5) {
    result = execute(
        database,
        "CREATE TABLE feature_assets(feature_asset_id INTEGER PRIMARY KEY AUTOINCREMENT "
        "CHECK(feature_asset_id>0),sha256 BLOB NOT NULL UNIQUE CHECK(length(sha256)=32),path "
        "TEXT NOT NULL UNIQUE CHECK(length(path)>0 AND length(path)<4096),size_bytes INTEGER "
        "NOT NULL CHECK(size_bytes>=0),durability INTEGER NOT NULL CHECK(durability BETWEEN 0 "
        "AND 1),created_at INTEGER NOT NULL CHECK(created_at>=0));"
        "CREATE TABLE feature_sets(feature_set_id INTEGER PRIMARY KEY AUTOINCREMENT "
        "CHECK(feature_set_id>0),image_id INTEGER NOT NULL REFERENCES "
        "images(image_id),feature_asset_id INTEGER NOT NULL REFERENCES "
        "feature_assets(feature_asset_id),extractor_kind TEXT NOT NULL "
        "CHECK(length(extractor_kind)>0 AND length(extractor_kind)<65),extractor_version "
        "INTEGER NOT NULL CHECK(extractor_version>0),parameter_fingerprint BLOB NOT NULL "
        "CHECK(length(parameter_fingerprint)=32),source_image_sha256 BLOB NOT NULL "
        "CHECK(length(source_image_sha256)=32),feature_count INTEGER NOT NULL "
        "CHECK(feature_count BETWEEN 0 AND 8192),descriptor_type INTEGER NOT NULL "
        "CHECK(descriptor_type "
        "IN(1,2)),descriptor_dimension INTEGER NOT NULL "
        "CHECK(descriptor_dimension BETWEEN 1 AND 4096),producer_task_id INTEGER REFERENCES "
        "tasks(task_id),created_at INTEGER NOT NULL "
        "CHECK(created_at>=0),UNIQUE(image_id,extractor_kind,extractor_version,parameter_"
        "fingerprint));"
        "CREATE INDEX feature_sets_image_idx ON feature_sets(image_id,feature_set_id);"
        "CREATE INDEX feature_sets_producer_idx ON "
        "feature_sets(producer_task_id,feature_set_id);"
        "CREATE TABLE feature_extract_tasks(task_id INTEGER PRIMARY KEY REFERENCES "
        "tasks(task_id) ON DELETE CASCADE,image_id INTEGER NOT NULL REFERENCES "
        "images(image_id),extractor_kind TEXT NOT NULL,extractor_version INTEGER NOT NULL "
        "CHECK(extractor_version>0),max_features INTEGER NOT NULL "
        "CHECK(max_features BETWEEN 1 AND 8192),pyramid_levels INTEGER NOT NULL "
        "CHECK(pyramid_levels BETWEEN 1 AND 16),fast_threshold INTEGER NOT NULL "
        "CHECK(fast_threshold BETWEEN 1 AND 255),parameter_fingerprint BLOB NOT NULL "
        "CHECK(length(parameter_fingerprint)=32));",
        "migrate schema v4 to v5");
#ifdef LARDON3D_PROJECT_DB_TESTING
    const char *forced_failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V5");
    if (result == LARDON3D_PROJECT_DB_OK && forced_failure && strcmp(forced_failure, "1") == 0) {
      result = execute(database, "INSERT INTO missing_test_table VALUES(1)",
                       "forced migration v5 failure");
    }
#endif
    if (result == LARDON3D_PROJECT_DB_OK) {
      result =
          execute(database, "UPDATE metadata SET value=5 WHERE key='schema_version' AND value=4",
                  "finish schema v5 migration");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && from_version != 0 && from_version < 6) {
    result = execute(database, schema_visual_v6, "migrate schema v5 to v6");
#ifdef LARDON3D_PROJECT_DB_TESTING
    const char *forced_failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V6");
    if (result == LARDON3D_PROJECT_DB_OK && forced_failure && strcmp(forced_failure, "1") == 0) {
      result = execute(database, "INSERT INTO missing_test_table VALUES(1)",
                       "forced migration v6 failure");
    }
#endif
    if (result == LARDON3D_PROJECT_DB_OK) {
      result = execute(database,
                       "UPDATE metadata SET value=6 WHERE key='schema_version' AND value=5",
                       "finish schema v6 migration");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(database, "COMMIT", "commit migration");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(database, "ROLLBACK", "rollback migration");
  }
  return result;
}

static Lardon3DProjectDbResult read_schema_version(Lardon3DProjectDb *database,
                                                   unsigned int *version) {
  if (!table_exists(database->connection, "metadata")) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(database->connection,
                                  "SELECT 1 FROM sqlite_master WHERE type='table' LIMIT 1", -1,
                                  &statement, NULL);
    if (code != SQLITE_OK) {
      return sqlite_result(database, code, "inspect schema");
    }
    code = sqlite3_step(statement);
    bool empty = code == SQLITE_DONE;
    (void)sqlite3_finalize(statement);
    if (!empty) {
      copy_error(database->error, "Base incohérente sans metadata.");
      return LARDON3D_PROJECT_DB_CORRUPT;
    }
    *version = 0;
    return LARDON3D_PROJECT_DB_OK;
  }
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(database, "SELECT value FROM metadata WHERE key='schema_version'", &statement);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result;
  }
  int code = sqlite3_step(statement);
  if (code != SQLITE_ROW || sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
    (void)sqlite3_finalize(statement);
    copy_error(database->error, "schema_version absente ou invalide.");
    return LARDON3D_PROJECT_DB_CORRUPT;
  }
  sqlite3_int64 value = sqlite3_column_int64(statement, 0);
  code = sqlite3_step(statement);
  (void)sqlite3_finalize(statement);
  if (code != SQLITE_DONE || value < 1 || value > UINT_MAX) {
    return LARDON3D_PROJECT_DB_CORRUPT;
  }
  *version = (unsigned int)value;
  return LARDON3D_PROJECT_DB_OK;
}

Lardon3DProjectDbResult lardon3d_project_db_open(const char *path, Lardon3DProjectDb **output,
                                                 char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]) {
  if (output) {
    *output = NULL;
  }
  if (!path || !path[0] || !output ||
      strnlen(path, LARDON3D_PROJECT_DB_PATH_CAPACITY) >= LARDON3D_PROJECT_DB_PATH_CAPACITY) {
    copy_error(error, "Chemin de base invalide.");
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  Lardon3DProjectDb *database = calloc(1, sizeof(*database));
  if (!database) {
    copy_error(error, "Allocation impossible.");
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }
  if (pthread_mutex_init(&database->mutex, NULL) != 0) {
    free(database);
    copy_error(error, "Mutex impossible.");
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }
  int code =
      sqlite3_open_v2(path, &database->connection,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  if (code != SQLITE_OK) {
    copy_error(error, database->connection ? sqlite3_errmsg(database->connection)
                                           : "Ouverture SQLite impossible.");
    if (database->connection) {
      (void)sqlite3_close(database->connection);
    }
    (void)pthread_mutex_destroy(&database->mutex);
    free(database);
    return code == SQLITE_CANTOPEN ? LARDON3D_PROJECT_DB_IO_ERROR : LARDON3D_PROJECT_DB_CORRUPT;
  }
  Lardon3DProjectDbResult result =
      execute(database,
              "PRAGMA foreign_keys=ON;PRAGMA journal_mode=DELETE;PRAGMA synchronous=FULL;PRAGMA "
              "busy_timeout=5000",
              "configure SQLite");
  unsigned int version = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = read_schema_version(database, &version);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = migrate(database, version);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    const char *required[] = {"project",
                              "tasks",
                              "checkpoints",
                              "artifacts",
                              "image_import_tasks",
                              "scansets",
                              "image_assets",
                              "images",
                              "feature_assets",
                              "feature_sets",
                              "feature_extract_tasks"};
    for (size_t index = 0; index < 11 && result == LARDON3D_PROJECT_DB_OK; ++index) {
      if (!table_exists(database->connection, required[index])) {
        copy_error(database->error, "Schéma v1 incomplet.");
        result = LARDON3D_PROJECT_DB_CORRUPT;
      }
    }
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    copy_error(error, database->error);
    (void)sqlite3_close(database->connection);
    (void)pthread_mutex_destroy(&database->mutex);
    free(database);
    return result;
  }
  database->error[0] = '\0';
  copy_error(error, "");
  *output = database;
  return LARDON3D_PROJECT_DB_OK;
}

void lardon3d_project_db_close(Lardon3DProjectDb *database) {
  if (!database) {
    return;
  }
  (void)pthread_mutex_lock(&database->mutex);
  (void)sqlite3_close(database->connection);
  (void)pthread_mutex_unlock(&database->mutex);
  (void)pthread_mutex_destroy(&database->mutex);
  free(database);
}

bool lardon3d_project_db_last_error(Lardon3DProjectDb *database,
                                    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]) {
  if (!database || !error) {
    return false;
  }
  (void)pthread_mutex_lock(&database->mutex);
  copy_error(error, database->error);
  (void)pthread_mutex_unlock(&database->mutex);
  return true;
}
unsigned int lardon3d_project_db_schema_version(Lardon3DProjectDb *database) {
  return database ? LARDON3D_PROJECT_DB_SCHEMA_VERSION : 0U;
}

Lardon3DProjectDbResult lardon3d_project_db_legacy_catalog_pending(Lardon3DProjectDb *database,
                                                                   bool *pending) {
  if (pending) {
    *pending = false;
  }
  if (!database || !pending) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database, "SELECT value FROM metadata WHERE key='legacy_image_catalog_pending'", &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    int code = sqlite3_step(statement);
    sqlite3_int64 value = sqlite3_column_int64(statement, 0);
    if (code != SQLITE_ROW || (value != 0 && value != 1) ||
        sqlite3_step(statement) != SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      *pending = value == 1;
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static Lardon3DProjectDbResult step_done(Lardon3DProjectDb *database, sqlite3_stmt *statement,
                                         const char *context) {
  int code = sqlite3_step(statement);
  if (code != SQLITE_DONE) {
    Lardon3DProjectDbResult result = sqlite_result(database, code, context);
    (void)sqlite3_finalize(statement);
    return result;
  }
  code = sqlite3_finalize(statement);
  return code == SQLITE_OK ? LARDON3D_PROJECT_DB_OK : sqlite_result(database, code, context);
}

static bool copy_column(sqlite3_stmt *statement, int column, char *destination, size_t capacity) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
    return false;
  }
  int bytes = sqlite3_column_bytes(statement, column);
  const unsigned char *text = sqlite3_column_text(statement, column);
  if (!text || bytes < 0 || (size_t)bytes >= capacity) {
    return false;
  }
  memcpy(destination, text, (size_t)bytes);
  destination[bytes] = '\0';
  return true;
}

Lardon3DProjectDbResult lardon3d_project_db_set_project(Lardon3DProjectDb *database,
                                                        const Lardon3DProjectDbProject *project) {
  if (!database || !project ||
      !bounded_text(project->stable_id, sizeof(project->stable_id), false) ||
      !bounded_text(project->name, sizeof(project->name), false) || project->created_at < 0 ||
      project->updated_at < project->created_at) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "INSERT INTO project(singleton,stable_id,name,created_at,updated_at) VALUES(1,?1,?2,?3,?4) "
      "ON CONFLICT(singleton) DO UPDATE SET name=excluded.name,updated_at=excluded.updated_at "
      "WHERE project.stable_id=excluded.stable_id AND project.created_at=excluded.created_at",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, project->stable_id, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, project->name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 3, project->created_at);
    (void)sqlite3_bind_int64(statement, 4, project->updated_at);
    result = step_done(database, statement, "set project");
    if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
      copy_error(database->error, "Identité de projet contradictoire.");
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    }
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_get_project(Lardon3DProjectDb *database,
                                                        Lardon3DProjectDbProject *project) {
  if (!database || !project) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database, "SELECT stable_id,name,created_at,updated_at FROM project WHERE singleton=1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW ||
               !copy_column(statement, 0, project->stable_id, sizeof(project->stable_id)) ||
               !copy_column(statement, 1, project->name, sizeof(project->name))) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      project->created_at = sqlite3_column_int64(statement, 2);
      project->updated_at = sqlite3_column_int64(statement, 3);
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static bool valid_checkpoint(const Lardon3DProjectDbCheckpoint *checkpoint) {
  return checkpoint && bounded_text(checkpoint->path, sizeof(checkpoint->path), false) &&
         checkpoint->format_version > 0 && checkpoint->updated_at >= 0 &&
         checkpoint->durability >= LARDON3D_DB_CHECKPOINT_DURABLE &&
         checkpoint->durability <= LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE;
}

static bool valid_durable_task(const Lardon3DTaskDurableSnapshot *snapshot, int64_t updated_at) {
  return snapshot && valid_task_id(snapshot->id) &&
         bounded_text(snapshot->name, sizeof(snapshot->name), false) &&
         valid_state(snapshot->saved_state) && valid_state(snapshot->recovery_state) &&
         snapshot->recovery_state ==
             ((snapshot->saved_state == TASK_RUNNING || snapshot->saved_state == TASK_PAUSED)
                  ? TASK_PENDING
                  : snapshot->saved_state) &&
         snapshot->progress <= 100 && snapshot->sequence_count <= INT_MAX &&
         snapshot->started_at.tv_sec >= 0 && snapshot->started_at.tv_sec <= INT64_MAX &&
         snapshot->started_at.tv_nsec >= 0 && snapshot->started_at.tv_nsec < 1000000000L &&
         snapshot->finished_at.tv_sec >= 0 && snapshot->finished_at.tv_sec <= INT64_MAX &&
         snapshot->finished_at.tv_nsec >= 0 && snapshot->finished_at.tv_nsec < 1000000000L &&
         updated_at >= 0;
}

static Lardon3DProjectDbResult
record_task_internal(Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot,
                     const char *task_kind, uint32_t task_kind_version,
                     const Lardon3DProjectDbCheckpoint *checkpoint, const char *source_path,
                     uint64_t scanset_id, const Lardon3DProjectDbFeatureExtractTask *feature,
                     const Lardon3DProjectDbVisualIndexUpdateTask *visual,
                     int64_t updated_at) {
  bool typed = task_kind != NULL;
  if (!database || !valid_durable_task(snapshot, updated_at) ||
      (typed && (!lardon3d_task_kind_is_valid(task_kind) || task_kind_version == 0)) ||
      (!typed && task_kind_version != 0) ||
      (source_path && !bounded_text(source_path, LARDON3D_PROJECT_DB_PATH_CAPACITY, false)) ||
      (source_path && !valid_task_id(scanset_id)) ||
      (feature &&
       (!valid_task_id(feature->task_id) || feature->task_id != snapshot->id ||
        !valid_task_id(feature->image_id) ||
        !lardon3d_task_kind_is_valid(feature->extractor_kind) || feature->extractor_version == 0 ||
        feature->max_features == 0 || feature->max_features > 8192 ||
        feature->pyramid_levels == 0 || feature->pyramid_levels > 16 ||
        feature->fast_threshold == 0 || feature->fast_threshold > 255)) ||
      (visual &&
       (!valid_task_id(visual->task_id) || visual->task_id != snapshot->id ||
        !valid_task_id(visual->visual_index_id) || visual->after_feature_set_id > INT64_MAX)) ||
      (checkpoint && !valid_checkpoint(checkpoint))) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
#ifdef LARDON3D_PROJECT_DB_TESTING
  const char *forced_busy = getenv("LARDON3D_TEST_PROJECT_DB_BUSY_CHECKPOINT");
  if (forced_busy && checkpoint && strcmp(forced_busy, "1") == 0) {
    copy_error(database->error, "Verrou DB injecté pour test.");
    return LARDON3D_PROJECT_DB_BUSY;
  }
#endif
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result = execute(database, "BEGIN IMMEDIATE", "begin task record");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(
        database,
        "INSERT INTO "
        "tasks(task_id,name,task_kind,task_kind_version,saved_state,recovery_state,progress,"
        "sequence_count,started_sec,started_nsec,finished_sec,finished_nsec,updated_at)"
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) ON CONFLICT(task_id) DO UPDATE SET "
        "name=excluded.name,saved_state=excluded.saved_state,"
        "recovery_state=excluded.recovery_state,progress=excluded.progress,sequence_count="
        "excluded.sequence_count,started_sec=excluded.started_sec,"
        "started_nsec=excluded.started_nsec,finished_sec=excluded.finished_sec,finished_nsec="
        "excluded.finished_nsec,updated_at=excluded.updated_at "
        "WHERE (tasks.task_kind IS NULL AND excluded.task_kind IS NULL) OR "
        "(tasks.task_kind=excluded.task_kind AND "
        "tasks.task_kind_version=excluded.task_kind_version)",
        &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)snapshot->id);
    (void)sqlite3_bind_text(statement, 2, snapshot->name, -1, SQLITE_TRANSIENT);
    if (typed) {
      (void)sqlite3_bind_text(statement, 3, task_kind, -1, SQLITE_TRANSIENT);
      (void)sqlite3_bind_int64(statement, 4, task_kind_version);
    } else {
      (void)sqlite3_bind_null(statement, 3);
      (void)sqlite3_bind_null(statement, 4);
    }
    (void)sqlite3_bind_int(statement, 5, (int)snapshot->saved_state);
    (void)sqlite3_bind_int(statement, 6, (int)snapshot->recovery_state);
    (void)sqlite3_bind_int(statement, 7, (int)snapshot->progress);
    (void)sqlite3_bind_int(statement, 8, (int)snapshot->sequence_count);
    (void)sqlite3_bind_int64(statement, 9, snapshot->started_at.tv_sec);
    (void)sqlite3_bind_int64(statement, 10, snapshot->started_at.tv_nsec);
    (void)sqlite3_bind_int64(statement, 11, snapshot->finished_at.tv_sec);
    (void)sqlite3_bind_int64(statement, 12, snapshot->finished_at.tv_nsec);
    (void)sqlite3_bind_int64(statement, 13, updated_at);
    result = step_done(database, statement, "upsert task");
    if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
      copy_error(database->error, "Type métier de tâche immuable.");
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && checkpoint) {
#ifdef LARDON3D_PROJECT_DB_TESTING
    const char *forced_failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT");
    if (forced_failure && strcmp(forced_failure, "1") == 0) {
      result = execute(database, "INSERT INTO missing_test_table VALUES(1)",
                       "forced checkpoint failure");
    }
#endif
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result =
        prepare(database,
                "UPDATE metadata SET value=CASE WHEN ?1=9223372036854775807 THEN 0 ELSE ?1+1 END "
                "WHERE key='next_task_id' AND value>0 AND value<=?1",
                &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)snapshot->id);
      result = step_done(database, statement, "advance recorded task id");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && source_path) {
    result =
        prepare(database,
                "INSERT INTO image_import_tasks(task_id,source_path,scanset_id) VALUES(?1,?2,?3) "
                "ON CONFLICT(task_id) DO UPDATE SET source_path=excluded.source_path "
                "WHERE image_import_tasks.source_path=excluded.source_path AND "
                "image_import_tasks.scanset_id=excluded.scanset_id",
                &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)snapshot->id);
      (void)sqlite3_bind_text(statement, 2, source_path, -1, SQLITE_TRANSIENT);
      (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)scanset_id);
      result = step_done(database, statement, "upsert image import");
      if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
        copy_error(database->error, "Source d'import immuable.");
        result = LARDON3D_PROJECT_DB_CONSTRAINT;
      }
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && feature) {
    result = prepare(
        database,
        "INSERT INTO "
        "feature_extract_tasks(task_id,image_id,extractor_kind,extractor_version,max_features,"
        "pyramid_levels,fast_threshold,parameter_fingerprint) VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
        "ON CONFLICT(task_id) DO UPDATE SET image_id=excluded.image_id WHERE "
        "feature_extract_tasks.image_id=excluded.image_id AND "
        "feature_extract_tasks.extractor_kind=excluded.extractor_kind AND "
        "feature_extract_tasks.extractor_version=excluded.extractor_version AND "
        "feature_extract_tasks.max_features=excluded.max_features AND "
        "feature_extract_tasks.pyramid_levels=excluded.pyramid_levels AND "
        "feature_extract_tasks.fast_threshold=excluded.fast_threshold AND "
        "feature_extract_tasks.parameter_fingerprint=excluded.parameter_fingerprint",
        &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)feature->task_id);
      (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)feature->image_id);
      (void)sqlite3_bind_text(statement, 3, feature->extractor_kind, -1, SQLITE_TRANSIENT);
      (void)sqlite3_bind_int64(statement, 4, feature->extractor_version);
      (void)sqlite3_bind_int64(statement, 5, feature->max_features);
      (void)sqlite3_bind_int64(statement, 6, feature->pyramid_levels);
      (void)sqlite3_bind_int64(statement, 7, feature->fast_threshold);
      (void)sqlite3_bind_blob(statement, 8, feature->parameter_fingerprint, 32, SQLITE_TRANSIENT);
      result = step_done(database, statement, "upsert feature extract");
      statement = NULL;
      if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
        copy_error(database->error, "Paramètres feature immuables.");
        result = LARDON3D_PROJECT_DB_CONSTRAINT;
      }
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && visual) {
    result = prepare(database,
                     "INSERT INTO visual_index_update_tasks(task_id,visual_index_id,"
                     "after_feature_set_id) VALUES(?1,?2,?3) ON CONFLICT(task_id) DO UPDATE SET "
                     "after_feature_set_id=excluded.after_feature_set_id WHERE "
                     "visual_index_update_tasks.visual_index_id=excluded.visual_index_id",
                     &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_int64(statement, 1, (sqlite3_int64)visual->task_id);
      sqlite3_bind_int64(statement, 2, (sqlite3_int64)visual->visual_index_id);
      sqlite3_bind_int64(statement, 3, (sqlite3_int64)visual->after_feature_set_id);
      result = step_done(database, statement, "upsert visual index update");
      if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
        copy_error(database->error, "Index cible de tâche immuable.");
        result = LARDON3D_PROJECT_DB_CONSTRAINT;
      }
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK && checkpoint) {
    result =
        prepare(database,
                "INSERT INTO checkpoints(task_id,path,format_version,durability,updated_at) "
                "VALUES(?1,?2,?3,?4,?5) "
                "ON CONFLICT(task_id) DO UPDATE SET "
                "path=excluded.path,format_version=excluded.format_version,durability=excluded."
                "durability,updated_at=excluded.updated_at",
                &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)snapshot->id);
      (void)sqlite3_bind_text(statement, 2, checkpoint->path, -1, SQLITE_TRANSIENT);
      (void)sqlite3_bind_int64(statement, 3, checkpoint->format_version);
      (void)sqlite3_bind_int(statement, 4, (int)checkpoint->durability);
      (void)sqlite3_bind_int64(statement, 5, checkpoint->updated_at);
      result = step_done(database, statement, "upsert checkpoint");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(database, "COMMIT", "commit task record");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(database, "ROLLBACK", "rollback task record");
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_record_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot, const char *task_kind,
    uint32_t task_kind_version, const Lardon3DProjectDbCheckpoint *checkpoint, int64_t updated_at) {
  return record_task_internal(database, snapshot, task_kind, task_kind_version, checkpoint, NULL, 0,
                              NULL, NULL, updated_at);
}

Lardon3DProjectDbResult lardon3d_project_db_record_image_import_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot, const char *task_kind,
    uint32_t task_kind_version, const Lardon3DProjectDbCheckpoint *checkpoint,
    const char *source_path, uint64_t scanset_id, int64_t updated_at) {
  if (!source_path) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  return record_task_internal(database, snapshot, task_kind, task_kind_version, checkpoint,
                              source_path, scanset_id, NULL, NULL, updated_at);
}

Lardon3DProjectDbResult lardon3d_project_db_record_feature_extract_task(
    Lardon3DProjectDb *database, const Lardon3DTaskDurableSnapshot *snapshot, const char *task_kind,
    uint32_t task_kind_version, const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbFeatureExtractTask *parameters, int64_t updated_at) {
  if (!parameters) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  return record_task_internal(database, snapshot, task_kind, task_kind_version, checkpoint, NULL, 0,
                              parameters, NULL, updated_at);
}

static bool read_task(sqlite3_stmt *statement, Lardon3DProjectDbTask *task) {
  memset(task, 0, sizeof(*task));
  sqlite3_int64 id = sqlite3_column_int64(statement, 0);
  bool has_kind = sqlite3_column_type(statement, 2) != SQLITE_NULL;
  bool has_kind_version = sqlite3_column_type(statement, 3) != SQLITE_NULL;
  int progress = sqlite3_column_int(statement, 6);
  sqlite3_int64 sequence_count = sqlite3_column_int64(statement, 7);
  sqlite3_int64 started_nsec = sqlite3_column_int64(statement, 9);
  sqlite3_int64 finished_nsec = sqlite3_column_int64(statement, 11);
  if (id <= 0 || !copy_column(statement, 1, task->name, sizeof(task->name))) {
    return false;
  }
  if (has_kind != has_kind_version) {
    return false;
  }
  if (has_kind) {
    sqlite3_int64 version = sqlite3_column_int64(statement, 3);
    if (!copy_column(statement, 2, task->task_kind, sizeof(task->task_kind)) ||
        !lardon3d_task_kind_is_valid(task->task_kind) || version <= 0 || version > UINT32_MAX) {
      return false;
    }
    task->has_task_kind = true;
    task->task_kind_version = (uint32_t)version;
  }
  task->task_id = (uint64_t)id;
  task->saved_state = (Lardon3DTaskState)sqlite3_column_int(statement, 4);
  task->recovery_state = (Lardon3DTaskState)sqlite3_column_int(statement, 5);
  if (progress < 0 || progress > 100 || sequence_count < 0 || sequence_count > UINT_MAX ||
      started_nsec < 0 || started_nsec >= 1000000000 || finished_nsec < 0 ||
      finished_nsec >= 1000000000 ||
      !database_time(sqlite3_column_int64(statement, 8), &task->started_at.tv_sec) ||
      !database_time(sqlite3_column_int64(statement, 10), &task->finished_at.tv_sec)) {
    return false;
  }
  task->progress = (unsigned int)progress;
  task->sequence_count = (unsigned int)sequence_count;
  task->started_at.tv_nsec = (long)started_nsec;
  task->finished_at.tv_nsec = (long)finished_nsec;
  task->updated_at = sqlite3_column_int64(statement, 12);
  if (sqlite3_column_type(statement, 13) != SQLITE_NULL) {
    task->has_checkpoint = true;
    if (!copy_column(statement, 13, task->checkpoint.path, sizeof(task->checkpoint.path))) {
      return false;
    }
    sqlite3_int64 format_version = sqlite3_column_int64(statement, 14);
    if (format_version <= 0 || format_version > UINT32_MAX) {
      return false;
    }
    task->checkpoint.format_version = (uint32_t)format_version;
    task->checkpoint.durability =
        (Lardon3DProjectDbCheckpointDurability)sqlite3_column_int(statement, 15);
    task->checkpoint.updated_at = sqlite3_column_int64(statement, 16);
    if (task->checkpoint.durability < LARDON3D_DB_CHECKPOINT_DURABLE ||
        task->checkpoint.durability > LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE) {
      return false;
    }
  }
  return valid_state(task->saved_state) && valid_state(task->recovery_state) &&
         task->progress <= 100;
}

static const char task_select[] =
    "SELECT "
    "t.task_id,t.name,t.task_kind,t.task_kind_version,t.saved_state,t.recovery_state,t.progress,t."
    "sequence_count,t.started_sec,t.started_nsec,"
    "t.finished_sec,t.finished_nsec,t.updated_at,c.path,c.format_version,c.durability,c.updated_at "
    "FROM tasks t LEFT JOIN checkpoints c ON c.task_id=t.task_id ";

Lardon3DProjectDbResult lardon3d_project_db_load_task(Lardon3DProjectDb *database, uint64_t task_id,
                                                      Lardon3DProjectDbTask *task) {
  if (!database || !valid_task_id(task_id) || !task) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  char sql[768];
  (void)snprintf(sql, sizeof(sql), "%s WHERE t.task_id=?1", task_select);
  Lardon3DProjectDbResult result = prepare(database, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)task_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || !read_task(statement, task)) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_list_recoverable(Lardon3DProjectDb *database,
                                                             uint64_t after_task_id,
                                                             Lardon3DProjectDbTask *tasks,
                                                             size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!database || !tasks || !count || after_task_id > INT64_MAX || capacity == 0 ||
      capacity > LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
#ifdef LARDON3D_PROJECT_DB_TESTING
  const char *forced_busy = getenv("LARDON3D_TEST_PROJECT_DB_BUSY_RECOVERY");
  if (forced_busy && strcmp(forced_busy, "1") == 0) {
    return LARDON3D_PROJECT_DB_BUSY;
  }
#endif
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  char sql[896];
  (void)snprintf(sql, sizeof(sql),
                 "%s WHERE t.recovery_state=?1 AND c.task_id IS NOT NULL AND t.task_id>?2 ORDER "
                 "BY t.task_id LIMIT ?3",
                 task_select);
  Lardon3DProjectDbResult result = prepare(database, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int(statement, 1, TASK_PENDING);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_task_id);
    (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_task(statement, &tasks[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && *count < capacity && code != SQLITE_DONE) {
      result = sqlite_result(database, code, "list recoverable");
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_allocate_task_id(Lardon3DProjectDb *database,
                                                             uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!database || !task_id) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result = execute(database, "BEGIN IMMEDIATE", "begin task id allocation");
  sqlite3_stmt *statement = NULL;
  sqlite3_int64 next = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(database, "SELECT value FROM metadata WHERE key='next_task_id'", &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    int code = sqlite3_step(statement);
    if (code != SQLITE_ROW || sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      next = sqlite3_column_int64(statement, 0);
      if (next <= 0 || next > INT64_MAX) {
        result = LARDON3D_PROJECT_DB_CONSTRAINT;
      }
    }
    (void)sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(database,
                     "UPDATE metadata SET value=CASE WHEN value=9223372036854775807 THEN 0 "
                     "ELSE value+1 END WHERE key='next_task_id' AND value=?1",
                     &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, next);
    result = step_done(database, statement, "advance task id");
    statement = NULL;
    if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(database, "COMMIT", "commit task id allocation");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(database, "ROLLBACK", "rollback task id allocation");
  } else {
    *task_id = (uint64_t)next;
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_project_db_load_image_import(Lardon3DProjectDb *database, uint64_t task_id,
                                      Lardon3DProjectDbImageImport *parameters) {
  if (!database || !valid_task_id(task_id) || !parameters) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  memset(parameters, 0, sizeof(*parameters));
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(database, "SELECT source_path,scanset_id FROM image_import_tasks WHERE task_id=?1",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)task_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || !copy_column(statement, 0, parameters->source_path,
                                                  sizeof(parameters->source_path))) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      sqlite3_int64 scanset = sqlite3_column_int64(statement, 1);
      if (scanset <= 0) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else {
        parameters->task_id = task_id;
        parameters->scanset_id = (uint64_t)scanset;
      }
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static bool valid_catalog_id(uint64_t value) { return value > 0 && value <= INT64_MAX; }

static bool valid_original_name(const char *name) {
  return bounded_text(name, LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY, false) && !strchr(name, '/') &&
         !strchr(name, '\\') && !strchr(name, '\t') && !strchr(name, '\r') && !strchr(name, '\n');
}

static bool valid_relative_asset_path(const char *path) {
  return bounded_text(path, LARDON3D_PROJECT_DB_PATH_CAPACITY, false) && path[0] != '/' &&
         !strchr(path, '\\') && strncmp(path, "../", 3) != 0 && !strstr(path, "/../") &&
         strcmp(path, "..") != 0;
}

static bool canonical_asset_path(const unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE],
                                 const char *path) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < LARDON3D_PROJECT_DB_SHA256_SIZE; ++index) {
    hex[index * 2] = digits[hash[index] >> 4];
    hex[index * 2 + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  char expected[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  int written = snprintf(expected, sizeof(expected), "assets/images/%c%c/%s", hex[0], hex[1], hex);
  return written > 0 && (size_t)written < sizeof(expected) && strcmp(path, expected) == 0;
}

Lardon3DProjectDbResult lardon3d_project_db_create_scanset(Lardon3DProjectDb *database,
                                                           const char *name,
                                                           Lardon3DProjectDbScanSet *scanset) {
  if (!database || !scanset ||
      !bounded_text(name, LARDON3D_PROJECT_DB_SCANSET_NAME_CAPACITY, false)) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  int64_t now = (int64_t)time(NULL);
  if (now < 0) {
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }
  memset(scanset, 0, sizeof(*scanset));
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database, "INSERT INTO scansets(name,created_at,updated_at) VALUES(?1,?2,?2)", &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 2, now);
    result = step_done(database, statement, "create scanset");
  }
  sqlite3_int64 id = sqlite3_last_insert_rowid(database->connection);
  if (result == LARDON3D_PROJECT_DB_OK && id <= 0) {
    result = LARDON3D_PROJECT_DB_CONSTRAINT;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    scanset->scanset_id = (uint64_t)id;
    scanset->created_at = now;
    scanset->updated_at = now;
    (void)snprintf(scanset->name, sizeof(scanset->name), "%s", name);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static bool read_scanset(sqlite3_stmt *statement, Lardon3DProjectDbScanSet *scanset) {
  sqlite3_int64 id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 created = sqlite3_column_int64(statement, 2);
  sqlite3_int64 updated = sqlite3_column_int64(statement, 3);
  if (id <= 0 || created < 0 || updated < created ||
      !copy_column(statement, 1, scanset->name, sizeof(scanset->name))) {
    return false;
  }
  scanset->scanset_id = (uint64_t)id;
  scanset->created_at = created;
  scanset->updated_at = updated;
  return true;
}

Lardon3DProjectDbResult lardon3d_project_db_load_scanset(Lardon3DProjectDb *database,
                                                         uint64_t scanset_id,
                                                         Lardon3DProjectDbScanSet *scanset) {
  if (!database || !valid_catalog_id(scanset_id) || !scanset) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  memset(scanset, 0, sizeof(*scanset));
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database, "SELECT scanset_id,name,created_at,updated_at FROM scansets WHERE scanset_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)scanset_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || !read_scanset(statement, scanset)) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_list_scansets(Lardon3DProjectDb *database,
                                                          uint64_t after_scanset_id,
                                                          Lardon3DProjectDbScanSet *scansets,
                                                          size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!database || !scansets || !count || after_scanset_id > INT64_MAX || capacity == 0 ||
      capacity > LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(database,
              "SELECT scanset_id,name,created_at,updated_at FROM scansets WHERE scanset_id>?1 "
              "ORDER BY scanset_id LIMIT ?2",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)after_scanset_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_scanset(statement, &scansets[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && *count < capacity && code != SQLITE_DONE) {
      result = sqlite_result(database, code, "list scansets");
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static bool read_image(sqlite3_stmt *statement, Lardon3DProjectDbImage *image,
                       Lardon3DProjectDbImageAsset *asset) {
  sqlite3_int64 image_id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 scanset_id = sqlite3_column_int64(statement, 1);
  sqlite3_int64 asset_id = sqlite3_column_int64(statement, 2);
  sqlite3_int64 size = sqlite3_column_int64(statement, 7);
  int hash_size = sqlite3_column_bytes(statement, 5);
  const void *hash = sqlite3_column_blob(statement, 5);
  if (image_id <= 0 || scanset_id <= 0 || asset_id <= 0 || size < 0 ||
      hash_size != LARDON3D_PROJECT_DB_SHA256_SIZE || !hash ||
      !copy_column(statement, 3, image->original_name, sizeof(image->original_name)) ||
      !copy_column(statement, 4, image->source_path, sizeof(image->source_path)) ||
      !copy_column(statement, 6, asset->path, sizeof(asset->path)) ||
      !valid_original_name(image->original_name) || !valid_relative_asset_path(asset->path)) {
    return false;
  }
  image->image_id = (uint64_t)image_id;
  image->scanset_id = (uint64_t)scanset_id;
  image->asset_id = (uint64_t)asset_id;
  asset->asset_id = (uint64_t)asset_id;
  memcpy(asset->sha256, hash, LARDON3D_PROJECT_DB_SHA256_SIZE);
  if (!canonical_asset_path(asset->sha256, asset->path)) {
    return false;
  }
  asset->size_bytes = (uint64_t)size;
  asset->state = (Lardon3DProjectDbImageAssetState)sqlite3_column_int(statement, 8);
  asset->created_at = sqlite3_column_int64(statement, 9);
  image->has_producer_task = sqlite3_column_type(statement, 10) != SQLITE_NULL;
  sqlite3_int64 producer = sqlite3_column_int64(statement, 10);
  if (image->has_producer_task && producer <= 0) {
    return false;
  }
  image->producer_task_id = image->has_producer_task ? (uint64_t)producer : 0;
  image->imported_at = sqlite3_column_int64(statement, 11);
  return asset->state == LARDON3D_DB_IMAGE_ASSET_READY && asset->created_at >= 0 &&
         image->imported_at >= 0;
}

static const char image_select[] =
    "SELECT "
    "i.image_id,i.scanset_id,i.asset_id,i.original_name,i.source_path,a.sha256,a.path,a.size_bytes,"
    "a.state,a.created_at,i.producer_task_id,i.imported_at FROM images i JOIN image_assets a ON "
    "a.asset_id=i.asset_id ";

Lardon3DProjectDbResult lardon3d_project_db_register_image(
    Lardon3DProjectDb *database, uint64_t scanset_id,
    const unsigned char sha256[LARDON3D_PROJECT_DB_SHA256_SIZE], const char *asset_path,
    uint64_t size_bytes, const char *original_name, const char *source_path,
    uint64_t producer_task_id, int64_t imported_at, Lardon3DProjectDbImageRegisterStatus *status,
    Lardon3DProjectDbImage *image) {
  if (!database || !valid_catalog_id(scanset_id) || !sha256 ||
      !valid_relative_asset_path(asset_path) || !canonical_asset_path(sha256, asset_path) ||
      size_bytes > INT64_MAX || !valid_original_name(original_name) ||
      !bounded_text(source_path, LARDON3D_PROJECT_DB_PATH_CAPACITY, false) ||
      (producer_task_id != 0 && !valid_task_id(producer_task_id)) || imported_at < 0 || !status ||
      !image) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
#ifdef LARDON3D_PROJECT_DB_TESTING
  const char *failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_IMAGE_REGISTER");
  if (failure && strcmp(failure, "1") == 0) {
    return LARDON3D_PROJECT_DB_BUSY;
  }
#endif
  memset(image, 0, sizeof(*image));
  *status = LARDON3D_PROJECT_DB_IMAGE_REGISTERED;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result = execute(database, "BEGIN IMMEDIATE", "begin image register");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(database,
                     "INSERT INTO image_assets(sha256,path,size_bytes,state,created_at) "
                     "VALUES(?1,?2,?3,1,?4) ON CONFLICT(sha256) DO NOTHING",
                     &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_blob(statement, 1, sha256, LARDON3D_PROJECT_DB_SHA256_SIZE,
                            SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, asset_path, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)size_bytes);
    (void)sqlite3_bind_int64(statement, 4, imported_at);
    result = step_done(database, statement, "insert image asset");
    statement = NULL;
  }
  sqlite3_int64 asset_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result =
        prepare(database, "SELECT asset_id,path,size_bytes,state FROM image_assets WHERE sha256=?1",
                &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_blob(statement, 1, sha256, LARDON3D_PROJECT_DB_SHA256_SIZE,
                            SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    char stored_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
    if (code != SQLITE_ROW || !copy_column(statement, 1, stored_path, sizeof(stored_path)) ||
        strcmp(stored_path, asset_path) != 0 ||
        sqlite3_column_int64(statement, 2) != (sqlite3_int64)size_bytes ||
        sqlite3_column_int(statement, 3) != LARDON3D_DB_IMAGE_ASSET_READY) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      asset_id = sqlite3_column_int64(statement, 0);
    }
    (void)sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(
        database,
        "INSERT INTO "
        "images(scanset_id,asset_id,original_name,source_path,producer_task_id,imported_at) "
        "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(scanset_id,asset_id) DO NOTHING",
        &statement);
  }
  int inserted = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)scanset_id);
    (void)sqlite3_bind_int64(statement, 2, asset_id);
    (void)sqlite3_bind_text(statement, 3, original_name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 4, source_path, -1, SQLITE_TRANSIENT);
    if (producer_task_id) {
      (void)sqlite3_bind_int64(statement, 5, (sqlite3_int64)producer_task_id);
    } else {
      (void)sqlite3_bind_null(statement, 5);
    }
    (void)sqlite3_bind_int64(statement, 6, imported_at);
    result = step_done(database, statement, "insert logical image");
    statement = NULL;
    inserted = sqlite3_changes(database->connection);
  }
  sqlite3_int64 image_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(database, "SELECT image_id FROM images WHERE scanset_id=?1 AND asset_id=?2",
                     &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)scanset_id);
    (void)sqlite3_bind_int64(statement, 2, asset_id);
    int code = sqlite3_step(statement);
    if (code != SQLITE_ROW || (image_id = sqlite3_column_int64(statement, 0)) <= 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    (void)sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(database, "COMMIT", "commit image register");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(database, "ROLLBACK", "rollback image register");
  }
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result;
  }
  *status = inserted == 1 ? LARDON3D_PROJECT_DB_IMAGE_REGISTERED
                          : LARDON3D_PROJECT_DB_IMAGE_ALREADY_PRESENT;
  return lardon3d_project_db_load_image(database, (uint64_t)image_id, image,
                                        &(Lardon3DProjectDbImageAsset){0});
}

Lardon3DProjectDbResult lardon3d_project_db_load_image(Lardon3DProjectDb *database,
                                                       uint64_t image_id,
                                                       Lardon3DProjectDbImage *image,
                                                       Lardon3DProjectDbImageAsset *asset) {
  if (!database || !valid_catalog_id(image_id) || !image || !asset) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  memset(image, 0, sizeof(*image));
  memset(asset, 0, sizeof(*asset));
  (void)pthread_mutex_lock(&database->mutex);
  char sql[768];
  (void)snprintf(sql, sizeof(sql), "%s WHERE i.image_id=?1", image_select);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(database, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)image_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || !read_image(statement, image, asset)) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_list_images(Lardon3DProjectDb *database,
                                                        uint64_t scanset_id,
                                                        uint64_t after_image_id,
                                                        Lardon3DProjectDbImage *images,
                                                        Lardon3DProjectDbImageAsset *assets,
                                                        size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!database || !valid_catalog_id(scanset_id) || after_image_id > INT64_MAX || !images ||
      !assets || !count || capacity == 0 || capacity > LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbResult exists = lardon3d_project_db_load_scanset(database, scanset_id, &scanset);
  if (exists != LARDON3D_PROJECT_DB_OK) {
    return exists;
  }
  (void)pthread_mutex_lock(&database->mutex);
  char sql[896];
  (void)snprintf(sql, sizeof(sql),
                 "%s WHERE i.scanset_id=?1 AND i.image_id>?2 ORDER BY i.image_id LIMIT ?3",
                 image_select);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(database, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)scanset_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_image_id);
    (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      memset(&images[*count], 0, sizeof(images[*count]));
      memset(&assets[*count], 0, sizeof(assets[*count]));
      if (!read_image(statement, &images[*count], &assets[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && *count < capacity && code != SQLITE_DONE) {
      result = sqlite_result(database, code, "list images");
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_count_images(Lardon3DProjectDb *database,
                                                         uint64_t scanset_id, uint64_t *count) {
  if (count) {
    *count = 0;
  }
  if (!database || !valid_catalog_id(scanset_id) || !count) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbResult exists = lardon3d_project_db_load_scanset(database, scanset_id, &scanset);
  if (exists != LARDON3D_PROJECT_DB_OK) {
    return exists;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(database, "SELECT count(*) FROM images WHERE scanset_id=?1", &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)scanset_id);
    int code = sqlite3_step(statement);
    sqlite3_int64 value = sqlite3_column_int64(statement, 0);
    if (code != SQLITE_ROW || value < 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      *count = (uint64_t)value;
    }
    (void)sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_project_db_create_artifact(Lardon3DProjectDb *database,
                                    const Lardon3DProjectDbArtifact *artifact) {
  if (!database || !artifact ||
      !bounded_text(artifact->artifact_id, sizeof(artifact->artifact_id), false) ||
      !bounded_text(artifact->kind, sizeof(artifact->kind), false) ||
      !bounded_text(artifact->path, sizeof(artifact->path), false) ||
      artifact->state != LARDON3D_DB_ARTIFACT_STAGED || artifact->size_bytes != 0 ||
      artifact->created_at < 0 || artifact->updated_at < artifact->created_at ||
      (artifact->has_producer_task && !valid_task_id(artifact->producer_task_id))) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(database,
              "INSERT INTO "
              "artifacts(artifact_id,kind,path,state,size_bytes,producer_task_id,created_at,"
              "updated_at) VALUES(?1,?2,?3,0,0,?4,?5,?6)",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, artifact->artifact_id, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, artifact->kind, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, artifact->path, -1, SQLITE_TRANSIENT);
    if (artifact->has_producer_task) {
      (void)sqlite3_bind_int64(statement, 4, (sqlite3_int64)artifact->producer_task_id);
    } else {
      (void)sqlite3_bind_null(statement, 4);
    }
    (void)sqlite3_bind_int64(statement, 5, artifact->created_at);
    (void)sqlite3_bind_int64(statement, 6, artifact->updated_at);
    result = step_done(database, statement, "create artifact");
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_mark_artifact_ready(Lardon3DProjectDb *database,
                                                                const char *artifact_id,
                                                                int64_t updated_at) {
  if (!database || !bounded_text(artifact_id, LARDON3D_PROJECT_DB_ID_CAPACITY, false) ||
      updated_at < 0) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  Lardon3DProjectDbArtifact artifact;
  Lardon3DProjectDbResult result =
      lardon3d_project_db_load_artifact(database, artifact_id, &artifact);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result;
  }
  (void)snprintf(path, sizeof(path), "%s", artifact.path);
  struct stat information;
  if (stat(path, &information) != 0 || !S_ISREG(information.st_mode) || information.st_size < 0) {
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  result = prepare(
      database,
      "UPDATE artifacts SET state=1,size_bytes=?1,updated_at=?2 WHERE artifact_id=?3 AND path=?4",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, information.st_size);
    (void)sqlite3_bind_int64(statement, 2, updated_at);
    (void)sqlite3_bind_text(statement, 3, artifact_id, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 4, path, -1, SQLITE_TRANSIENT);
    result = step_done(database, statement, "mark artifact ready");
    if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    }
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_load_artifact(Lardon3DProjectDb *database,
                                                          const char *artifact_id,
                                                          Lardon3DProjectDbArtifact *artifact) {
  if (!database || !artifact ||
      !bounded_text(artifact_id, LARDON3D_PROJECT_DB_ID_CAPACITY, false)) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT artifact_id,kind,path,state,size_bytes,producer_task_id,created_at,updated_at FROM "
      "artifacts WHERE artifact_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, artifact_id, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW ||
               !copy_column(statement, 0, artifact->artifact_id, sizeof(artifact->artifact_id)) ||
               !copy_column(statement, 1, artifact->kind, sizeof(artifact->kind)) ||
               !copy_column(statement, 2, artifact->path, sizeof(artifact->path))) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      sqlite3_int64 size = sqlite3_column_int64(statement, 4);
      sqlite3_int64 producer = sqlite3_column_int64(statement, 5);
      artifact->state = (Lardon3DProjectDbArtifactState)sqlite3_column_int(statement, 3);
      artifact->size_bytes = size >= 0 ? (uint64_t)size : 0;
      artifact->has_producer_task = sqlite3_column_type(statement, 5) != SQLITE_NULL;
      artifact->producer_task_id =
          artifact->has_producer_task && producer > 0 ? (uint64_t)producer : 0;
      artifact->created_at = sqlite3_column_int64(statement, 6);
      artifact->updated_at = sqlite3_column_int64(statement, 7);
    }
    (void)sqlite3_finalize(statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK &&
      (artifact->state < LARDON3D_DB_ARTIFACT_STAGED ||
       artifact->state > LARDON3D_DB_ARTIFACT_READY ||
       (artifact->has_producer_task && artifact->producer_task_id == 0))) {
    result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static bool canonical_feature_path(const unsigned char hash[32], const char *path) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < 32; ++i) {
    hex[2 * i] = digits[hash[i] >> 4];
    hex[2 * i + 1] = digits[hash[i] & 15];
  }
  hex[64] = '\0';
  char expected[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  int n = snprintf(expected, sizeof(expected), "assets/features/%c%c/%s", hex[0], hex[1], hex);
  return n > 0 && (size_t)n < sizeof(expected) && strcmp(expected, path) == 0;
}

static bool read_feature_set(sqlite3_stmt *s, Lardon3DProjectDbFeatureSet *set) {
  memset(set, 0, sizeof(*set));
  sqlite3_int64 id = sqlite3_column_int64(s, 0), image = sqlite3_column_int64(s, 1),
                asset = sqlite3_column_int64(s, 2);
  sqlite3_int64 version = sqlite3_column_int64(s, 4), count = sqlite3_column_int64(s, 7),
                type = sqlite3_column_int64(s, 8), dim = sqlite3_column_int64(s, 9);
  const void *fp = sqlite3_column_blob(s, 5), *source = sqlite3_column_blob(s, 6),
             *hash = sqlite3_column_blob(s, 12);
  sqlite3_int64 size = sqlite3_column_int64(s, 14), durability = sqlite3_column_int64(s, 15),
                producer = sqlite3_column_int64(s, 10);
  if (id <= 0 || image <= 0 || asset <= 0 || version <= 0 || version > UINT32_MAX || count < 0 ||
      count > 8192 || type < 1 || type > 2 || dim <= 0 || dim > 4096 ||
      sqlite3_column_bytes(s, 5) != 32 || sqlite3_column_bytes(s, 6) != 32 ||
      sqlite3_column_bytes(s, 12) != 32 || !fp || !source || !hash || size < 0 || durability < 0 ||
      durability > 1 || !copy_column(s, 3, set->extractor_kind, sizeof(set->extractor_kind)) ||
      !lardon3d_task_kind_is_valid(set->extractor_kind) ||
      !copy_column(s, 13, set->asset.path, sizeof(set->asset.path))) {
    return false;
  }
  set->feature_set_id = (uint64_t)id;
  set->image_id = (uint64_t)image;
  set->feature_asset_id = (uint64_t)asset;
  set->extractor_version = (uint32_t)version;
  memcpy(set->parameter_fingerprint, fp, 32);
  memcpy(set->source_image_sha256, source, 32);
  set->feature_count = (uint32_t)count;
  set->descriptor_type = (uint32_t)type;
  set->descriptor_dimension = (uint32_t)dim;
  set->has_producer_task = sqlite3_column_type(s, 10) != SQLITE_NULL;
  if (set->has_producer_task && producer <= 0) {
    return false;
  }
  set->producer_task_id = set->has_producer_task ? (uint64_t)producer : 0;
  set->created_at = sqlite3_column_int64(s, 11);
  set->asset.feature_asset_id = (uint64_t)asset;
  memcpy(set->asset.sha256, hash, 32);
  set->asset.size_bytes = (uint64_t)size;
  set->asset.durability = (Lardon3DProjectDbFeatureDurability)durability;
  set->asset.created_at = sqlite3_column_int64(s, 16);
  return set->created_at >= 0 && set->asset.created_at >= 0 &&
         canonical_feature_path(set->asset.sha256, set->asset.path);
}

static const char feature_select[] =
    "SELECT "
    "f.feature_set_id,f.image_id,f.feature_asset_id,f.extractor_kind,f.extractor_version,f."
    "parameter_fingerprint,f.source_image_sha256,f.feature_count,f.descriptor_type,f.descriptor_"
    "dimension,f.producer_task_id,f.created_at,a.sha256,a.path,a.size_bytes,a.durability,a.created_"
    "at FROM feature_sets f JOIN feature_assets a ON a.feature_asset_id=f.feature_asset_id ";

static Lardon3DProjectDbResult verify_feature_source_locked(Lardon3DProjectDb *db,
                                                            uint64_t image_id,
                                                            const unsigned char source[32]) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(db,
                                           "SELECT a.sha256 FROM images i JOIN image_assets a ON "
                                           "a.asset_id=i.asset_id WHERE i.image_id=?1",
                                           &statement);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result;
  }
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)image_id);
  int code = sqlite3_step(statement);
  if (code != SQLITE_ROW || sqlite3_column_bytes(statement, 0) != 32 ||
      memcmp(sqlite3_column_blob(statement, 0), source, 32) != 0) {
    result = code == SQLITE_DONE ? LARDON3D_PROJECT_DB_NOT_FOUND : LARDON3D_PROJECT_DB_CONSTRAINT;
  }
  sqlite3_finalize(statement);
  return result;
}

static Lardon3DProjectDbResult find_or_insert_feature_asset_locked(
    Lardon3DProjectDb *db, const unsigned char hash[32], const char *path, uint64_t size,
    Lardon3DProjectDbFeatureDurability durability, int64_t created, sqlite3_int64 *asset_id) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "INSERT INTO feature_assets(sha256,path,size_bytes,durability,created_at) "
              "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(sha256) DO NOTHING",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)size);
    sqlite3_bind_int(statement, 4, (int)durability);
    sqlite3_bind_int64(statement, 5, created);
    result = step_done(db, statement, "insert feature asset");
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(db,
                     "SELECT feature_asset_id,path,size_bytes,durability "
                     "FROM feature_assets WHERE sha256=?1",
                     &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    char stored[4096];
    sqlite3_int64 stored_durability = sqlite3_column_int64(statement, 3);
    if (code != SQLITE_ROW || !copy_column(statement, 1, stored, sizeof(stored)) ||
        strcmp(stored, path) != 0 || sqlite3_column_int64(statement, 2) != (sqlite3_int64)size ||
        stored_durability < 0 || stored_durability > 1) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      *asset_id = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK && durability == LARDON3D_DB_FEATURE_ASSET_DURABLE) {
    result = prepare(db,
                     "UPDATE feature_assets SET durability=?1 "
                     "WHERE feature_asset_id=?2 AND durability=?3",
                     &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK && durability == LARDON3D_DB_FEATURE_ASSET_DURABLE) {
    sqlite3_bind_int(statement, 1, LARDON3D_DB_FEATURE_ASSET_DURABLE);
    sqlite3_bind_int64(statement, 2, *asset_id);
    sqlite3_bind_int(statement, 3, LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE);
    result = step_done(db, statement, "promote feature asset durability");
  }
  return result;
}

static Lardon3DProjectDbResult
insert_feature_set_locked(Lardon3DProjectDb *db, uint64_t image_id, sqlite3_int64 asset_id,
                          const char *kind, uint32_t version, const unsigned char fp[32],
                          const unsigned char source[32], uint32_t count, uint32_t type,
                          uint32_t dim, uint64_t producer, int64_t created) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "INSERT INTO feature_sets(image_id,feature_asset_id,extractor_kind,extractor_version,"
              "parameter_fingerprint,source_image_sha256,feature_count,descriptor_type,"
              "descriptor_dimension,producer_task_id,created_at) "
              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) ON CONFLICT(image_id,extractor_kind,"
              "extractor_version,parameter_fingerprint) DO NOTHING",
              &statement);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result;
  }
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)image_id);
  sqlite3_bind_int64(statement, 2, asset_id);
  sqlite3_bind_text(statement, 3, kind, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 4, version);
  sqlite3_bind_blob(statement, 5, fp, 32, SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 6, source, 32, SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 7, count);
  sqlite3_bind_int64(statement, 8, type);
  sqlite3_bind_int64(statement, 9, dim);
  if (producer) {
    sqlite3_bind_int64(statement, 10, (sqlite3_int64)producer);
  } else {
    sqlite3_bind_null(statement, 10);
  }
  sqlite3_bind_int64(statement, 11, created);
  return step_done(db, statement, "insert feature set");
}

Lardon3DProjectDbResult lardon3d_project_db_register_feature_set(
    Lardon3DProjectDb *db, uint64_t image_id, const char *kind, uint32_t version,
    const unsigned char fp[32], const unsigned char source[32], uint32_t count, uint32_t type,
    uint32_t dim, const unsigned char hash[32], const char *path, uint64_t size,
    Lardon3DProjectDbFeatureDurability durability, uint64_t producer, int64_t created,
    Lardon3DProjectDbFeatureSet *out) {
  if (!db || !valid_catalog_id(image_id) || !kind || !lardon3d_task_kind_is_valid(kind) ||
      version == 0 || !fp || !source || count > 8192 || type < 1 || type > 2 || dim == 0 ||
      dim > 4096 || !hash || !canonical_feature_path(hash, path) || size > INT64_MAX ||
      durability < 0 || durability > 1 || (producer && !valid_task_id(producer)) || created < 0 ||
      !out) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
#ifdef LARDON3D_PROJECT_DB_TESTING
  const char *failure = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_FEATURE_REGISTER");
  if (failure && strcmp(failure, "1") == 0) {
    return LARDON3D_PROJECT_DB_BUSY;
  }
#endif
  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result = execute(db, "BEGIN IMMEDIATE", "begin feature register");
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = verify_feature_source_locked(db, image_id, source);
  }
  sqlite3_int64 asset_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result =
        find_or_insert_feature_asset_locked(db, hash, path, size, durability, created, &asset_id);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = insert_feature_set_locked(db, image_id, asset_id, kind, version, fp, source, count,
                                       type, dim, producer, created);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(db, "COMMIT", "commit feature register");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(db, "ROLLBACK", "rollback feature register");
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result == LARDON3D_PROJECT_DB_OK
             ? lardon3d_project_db_find_feature_set(db, image_id, kind, version, fp, out)
             : result;
}

Lardon3DProjectDbResult lardon3d_project_db_find_feature_set(Lardon3DProjectDb *db,
                                                             uint64_t image_id, const char *kind,
                                                             uint32_t version,
                                                             const unsigned char fp[32],
                                                             Lardon3DProjectDbFeatureSet *out) {
  if (!db || !valid_catalog_id(image_id) || !lardon3d_task_kind_is_valid(kind) || version == 0 ||
      !fp || !out) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  char sql[1024];
  snprintf(sql, sizeof(sql),
           "%s WHERE f.image_id=?1 AND f.extractor_kind=?2 AND f.extractor_version=?3 AND "
           "f.parameter_fingerprint=?4",
           feature_select);
  sqlite3_stmt *s = NULL;
  Lardon3DProjectDbResult result = prepare(db, sql, &s);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(s, 1, (sqlite3_int64)image_id);
    sqlite3_bind_text(s, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, version);
    sqlite3_bind_blob(s, 4, fp, 32, SQLITE_TRANSIENT);
    int code = sqlite3_step(s);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || !read_feature_set(s, out)) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    sqlite3_finalize(s);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_load_feature_set(Lardon3DProjectDb *db, uint64_t id,
                                                             Lardon3DProjectDbFeatureSet *out) {
  if (!db || !valid_catalog_id(id) || !out) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  char sql[1024];
  snprintf(sql, sizeof(sql), "%s WHERE f.feature_set_id=?1", feature_select);
  sqlite3_stmt *s = NULL;
  Lardon3DProjectDbResult result = prepare(db, sql, &s);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(s, 1, (sqlite3_int64)id);
    int code = sqlite3_step(s);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || !read_feature_set(s, out)) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    sqlite3_finalize(s);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_list_feature_sets(Lardon3DProjectDb *db, uint64_t after,
                                                              Lardon3DProjectDbFeatureSet *sets,
                                                              size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!db || after > INT64_MAX || !sets || !count || capacity == 0 || capacity > 256) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  char sql[1024];
  snprintf(sql, sizeof(sql), "%s WHERE f.feature_set_id>?1 ORDER BY f.feature_set_id LIMIT ?2",
           feature_select);
  sqlite3_stmt *s = NULL;
  Lardon3DProjectDbResult result = prepare(db, sql, &s);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(s, 1, (sqlite3_int64)after);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(s)) == SQLITE_ROW) {
      if (!read_feature_set(s, &sets[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && *count < capacity && code != SQLITE_DONE) {
      result = sqlite_result(db, code, "list feature sets");
    }
    sqlite3_finalize(s);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_project_db_load_feature_extract_task(Lardon3DProjectDb *db, uint64_t task_id,
                                              Lardon3DProjectDbFeatureExtractTask *p) {
  if (!db || !valid_task_id(task_id) || !p) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  memset(p, 0, sizeof(*p));
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *s = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT "
              "image_id,extractor_kind,extractor_version,max_features,pyramid_levels,fast_"
              "threshold,parameter_fingerprint FROM feature_extract_tasks WHERE task_id=?1",
              &s);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(s, 1, (sqlite3_int64)task_id);
    int code = sqlite3_step(s);
    sqlite3_int64 image = sqlite3_column_int64(s, 0), version = sqlite3_column_int64(s, 2),
                  max = sqlite3_column_int64(s, 3), levels = sqlite3_column_int64(s, 4),
                  threshold = sqlite3_column_int64(s, 5);
    const void *fp = sqlite3_column_blob(s, 6);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || image <= 0 || version <= 0 || version > UINT32_MAX ||
               max <= 0 || max > 8192 || levels <= 0 || levels > 16 || threshold <= 0 ||
               threshold > 255 ||
               !copy_column(s, 1, p->extractor_kind, sizeof(p->extractor_kind)) ||
               !lardon3d_task_kind_is_valid(p->extractor_kind) ||
               sqlite3_column_bytes(s, 6) != 32 || !fp) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      p->task_id = task_id;
      p->image_id = (uint64_t)image;
      p->extractor_version = (uint32_t)version;
      p->max_features = (uint32_t)max;
      p->pyramid_levels = (uint32_t)levels;
      p->fast_threshold = (uint32_t)threshold;
      memcpy(p->parameter_fingerprint, fp, 32);
    }
    sqlite3_finalize(s);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

static bool valid_visual_index(const Lardon3DProjectDbVisualIndex *index) {
  return index && lardon3d_task_kind_is_valid(index->index_kind) && index->index_version > 0 &&
         index->descriptor_type >= 1 && index->descriptor_type <= 2 &&
         index->descriptor_dimension > 0 && index->descriptor_dimension <= 4096 &&
         lardon3d_task_kind_is_valid(index->extractor_kind) && index->extractor_version > 0 &&
         index->table_count > 0 && index->table_count <= 32 && index->key_bits >= 8 &&
         index->key_bits <= 32 && index->max_features_per_set > 0 &&
         index->max_features_per_set <= 1024 && index->max_bucket_postings > 0 &&
         index->max_bucket_postings <= 4096 && index->created_at >= 0;
}

static bool read_visual_index(sqlite3_stmt *statement, Lardon3DProjectDbVisualIndex *index) {
  memset(index, 0, sizeof(*index));
  sqlite3_int64 id = sqlite3_column_int64(statement, 0);
  const void *feature_fp = sqlite3_column_blob(statement, 7);
  const void *index_fp = sqlite3_column_blob(statement, 8);
  if (id <= 0 || !copy_column(statement, 1, index->index_kind, sizeof(index->index_kind)) ||
      !copy_column(statement, 5, index->extractor_kind, sizeof(index->extractor_kind)) ||
      sqlite3_column_bytes(statement, 7) != 32 || sqlite3_column_bytes(statement, 8) != 32 ||
      !feature_fp || !index_fp) {
    return false;
  }
  index->visual_index_id = (uint64_t)id;
  index->index_version = (uint32_t)sqlite3_column_int64(statement, 2);
  index->descriptor_type = (uint32_t)sqlite3_column_int64(statement, 3);
  index->descriptor_dimension = (uint32_t)sqlite3_column_int64(statement, 4);
  index->extractor_version = (uint32_t)sqlite3_column_int64(statement, 6);
  memcpy(index->feature_parameter_fingerprint, feature_fp, 32);
  memcpy(index->index_parameter_fingerprint, index_fp, 32);
  index->table_count = (uint32_t)sqlite3_column_int64(statement, 9);
  index->key_bits = (uint32_t)sqlite3_column_int64(statement, 10);
  index->max_features_per_set = (uint32_t)sqlite3_column_int64(statement, 11);
  index->max_bucket_postings = (uint32_t)sqlite3_column_int64(statement, 12);
  index->created_at = sqlite3_column_int64(statement, 13);
  return valid_visual_index(index);
}

static const char visual_index_select[] =
    "SELECT visual_index_id,index_kind,index_version,descriptor_type,descriptor_dimension,"
    "extractor_kind,extractor_version,feature_parameter_fingerprint,"
    "index_parameter_fingerprint,table_count,key_bits,max_features_per_set,"
    "max_bucket_postings,created_at FROM visual_indexes";

Lardon3DProjectDbResult lardon3d_project_db_create_visual_index(
    Lardon3DProjectDb *db, const Lardon3DProjectDbVisualIndex *configuration,
    Lardon3DProjectDbVisualIndex *out) {
  if (!db || !valid_visual_index(configuration) || !out) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db,
      "INSERT INTO visual_indexes(index_kind,index_version,descriptor_type,descriptor_dimension,"
      "extractor_kind,extractor_version,feature_parameter_fingerprint,"
      "index_parameter_fingerprint,table_count,key_bits,max_features_per_set,"
      "max_bucket_postings,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) "
      "ON CONFLICT(index_kind,index_version,descriptor_type,descriptor_dimension,extractor_kind,"
      "extractor_version,feature_parameter_fingerprint,index_parameter_fingerprint) DO NOTHING",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_text(statement, 1, configuration->index_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, configuration->index_version);
    sqlite3_bind_int64(statement, 3, configuration->descriptor_type);
    sqlite3_bind_int64(statement, 4, configuration->descriptor_dimension);
    sqlite3_bind_text(statement, 5, configuration->extractor_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 6, configuration->extractor_version);
    sqlite3_bind_blob(statement, 7, configuration->feature_parameter_fingerprint, 32,
                      SQLITE_TRANSIENT);
    sqlite3_bind_blob(statement, 8, configuration->index_parameter_fingerprint, 32,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 9, configuration->table_count);
    sqlite3_bind_int64(statement, 10, configuration->key_bits);
    sqlite3_bind_int64(statement, 11, configuration->max_features_per_set);
    sqlite3_bind_int64(statement, 12, configuration->max_bucket_postings);
    sqlite3_bind_int64(statement, 13, configuration->created_at);
    result = step_done(db, statement, "create visual index");
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "%s WHERE index_kind=?1 AND index_version=?2 AND descriptor_type=?3 AND "
             "descriptor_dimension=?4 AND extractor_kind=?5 AND extractor_version=?6 AND "
             "feature_parameter_fingerprint=?7 AND index_parameter_fingerprint=?8",
             visual_index_select);
    result = prepare(db, sql, &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_text(statement, 1, configuration->index_kind, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(statement, 2, configuration->index_version);
      sqlite3_bind_int64(statement, 3, configuration->descriptor_type);
      sqlite3_bind_int64(statement, 4, configuration->descriptor_dimension);
      sqlite3_bind_text(statement, 5, configuration->extractor_kind, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(statement, 6, configuration->extractor_version);
      sqlite3_bind_blob(statement, 7, configuration->feature_parameter_fingerprint, 32,
                        SQLITE_TRANSIENT);
      sqlite3_bind_blob(statement, 8, configuration->index_parameter_fingerprint, 32,
                        SQLITE_TRANSIENT);
      int code = sqlite3_step(statement);
      if (code != SQLITE_ROW || !read_visual_index(statement, out)) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      }
      sqlite3_finalize(statement);
    }
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_load_visual_index(
    Lardon3DProjectDb *db, uint64_t id, Lardon3DProjectDbVisualIndex *out) {
  if (!db || !valid_catalog_id(id) || !out) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  char sql[768];
  snprintf(sql, sizeof(sql), "%s WHERE visual_index_id=?1", visual_index_select);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(db, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)id);
    int code = sqlite3_step(statement);
    result = code == SQLITE_DONE ? LARDON3D_PROJECT_DB_NOT_FOUND
                                 : code == SQLITE_ROW && read_visual_index(statement, out)
                                       ? LARDON3D_PROJECT_DB_OK
                                       : LARDON3D_PROJECT_DB_CORRUPT;
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

static bool canonical_visual_path(const unsigned char hash[32], const char *path) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < 32; ++i) {
    hex[i * 2] = digits[hash[i] >> 4];
    hex[i * 2 + 1] = digits[hash[i] & 15];
  }
  hex[64] = '\0';
  char expected[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  int written = snprintf(expected, sizeof(expected), "assets/visual-index/%c%c/%s", hex[0],
                         hex[1], hex);
  return written > 0 && (size_t)written < sizeof(expected) && strcmp(expected, path) == 0;
}

static bool read_visual_segment(sqlite3_stmt *statement,
                                Lardon3DProjectDbVisualIndexSegment *segment) {
  memset(segment, 0, sizeof(*segment));
  sqlite3_int64 id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 index_id = sqlite3_column_int64(statement, 1);
  sqlite3_int64 generation = sqlite3_column_int64(statement, 2);
  const void *hash = sqlite3_column_blob(statement, 3);
  sqlite3_int64 size = sqlite3_column_int64(statement, 5);
  sqlite3_int64 postings = sqlite3_column_int64(statement, 6);
  sqlite3_int64 members = sqlite3_column_int64(statement, 7);
  sqlite3_int64 durability = sqlite3_column_int64(statement, 8);
  if (id <= 0 || index_id <= 0 || generation <= 0 || !hash ||
      sqlite3_column_bytes(statement, 3) != 32 || size < 128 || postings < 0 || members < 1 ||
      members > 16 || durability < 0 || durability > 1 ||
      !copy_column(statement, 4, segment->path, sizeof(segment->path))) {
    return false;
  }
  segment->visual_index_segment_id = (uint64_t)id;
  segment->visual_index_id = (uint64_t)index_id;
  segment->generation = (uint64_t)generation;
  memcpy(segment->sha256, hash, 32);
  segment->size_bytes = (uint64_t)size;
  segment->posting_count = (uint64_t)postings;
  segment->feature_set_count = (uint32_t)members;
  segment->durability = (Lardon3DProjectDbVisualIndexDurability)durability;
  segment->has_producer_task = sqlite3_column_type(statement, 9) != SQLITE_NULL;
  segment->producer_task_id = segment->has_producer_task
                                  ? (uint64_t)sqlite3_column_int64(statement, 9)
                                  : 0;
  segment->created_at = sqlite3_column_int64(statement, 10);
  return segment->created_at >= 0 && canonical_visual_path(segment->sha256, segment->path);
}

static const char visual_segment_select[] =
    "SELECT visual_index_segment_id,visual_index_id,generation,sha256,path,size_bytes,"
    "posting_count,feature_set_count,durability,producer_task_id,created_at "
    "FROM visual_index_segments";

Lardon3DProjectDbResult lardon3d_project_db_list_visual_index_segments(
    Lardon3DProjectDb *db, uint64_t index_id, uint64_t after,
    Lardon3DProjectDbVisualIndexSegment *segments, size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!db || !valid_catalog_id(index_id) || after > INT64_MAX || !segments || !count ||
      capacity == 0 || capacity > 256) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  char sql[768];
  snprintf(sql, sizeof(sql), "%s WHERE visual_index_id=?1 AND generation>?2 ORDER BY generation "
                             "LIMIT ?3", visual_segment_select);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(db, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)index_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_visual_segment(statement, &segments[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE && *count < capacity) {
      result = sqlite_result(db, code, "list visual index segments");
    }
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_list_visual_index_pending(
    Lardon3DProjectDb *db, uint64_t index_id, uint64_t after,
    Lardon3DProjectDbFeatureSet *sets, size_t capacity, size_t *count) {
  if (count) {
    *count = 0;
  }
  if (!db || !valid_catalog_id(index_id) || after > INT64_MAX || !sets || !count ||
      capacity == 0 || capacity > 16) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&db->mutex);
  char sql[2048];
  snprintf(sql, sizeof(sql),
           "%s JOIN visual_indexes v ON v.visual_index_id=?1 LEFT JOIN "
           "visual_index_memberships m ON m.visual_index_id=v.visual_index_id AND "
           "m.feature_set_id=f.feature_set_id WHERE f.feature_set_id>?2 AND m.feature_set_id IS "
           "NULL AND f.descriptor_type=v.descriptor_type AND f.descriptor_dimension="
           "v.descriptor_dimension AND f.extractor_kind=v.extractor_kind AND "
           "f.extractor_version=v.extractor_version AND f.parameter_fingerprint="
           "v.feature_parameter_fingerprint ORDER BY f.feature_set_id LIMIT ?3",
           feature_select);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(db, sql, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)index_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_feature_set(statement, &sets[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE && *count < capacity) {
      result = sqlite_result(db, code, "list pending visual index features");
    }
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_publish_visual_index_segment(
    Lardon3DProjectDb *db, const Lardon3DProjectDbVisualIndexSegment *segment,
    const uint64_t *ids, size_t count, Lardon3DProjectDbVisualIndexSegment *published) {
  if (!db || !segment || !valid_catalog_id(segment->visual_index_id) || segment->generation == 0 ||
      !canonical_visual_path(segment->sha256, segment->path) || segment->size_bytes < 128 ||
      segment->size_bytes > INT64_MAX || segment->posting_count > INT64_MAX || !ids || count == 0 ||
      count > 16 || segment->feature_set_count != count || segment->durability < 0 ||
      segment->durability > 1 || (segment->producer_task_id &&
                                  !valid_task_id(segment->producer_task_id)) ||
      segment->created_at < 0 || !published) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
#ifdef LARDON3D_PROJECT_DB_TESTING
  const char *fail_publish = getenv("LARDON3D_TEST_PROJECT_DB_FAIL_VISUAL_SEGMENT");
  if (fail_publish && strcmp(fail_publish, "1") == 0) {
    return LARDON3D_PROJECT_DB_BUSY;
  }
#endif
  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result = execute(db, "BEGIN IMMEDIATE", "begin visual segment publish");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(db, "INSERT INTO visual_index_segments(visual_index_id,generation,sha256,"
                         "path,size_bytes,posting_count,feature_set_count,durability,"
                         "producer_task_id,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
                     &statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)segment->visual_index_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)segment->generation);
    sqlite3_bind_blob(statement, 3, segment->sha256, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, segment->path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 5, (sqlite3_int64)segment->size_bytes);
    sqlite3_bind_int64(statement, 6, (sqlite3_int64)segment->posting_count);
    sqlite3_bind_int64(statement, 7, (sqlite3_int64)count);
    sqlite3_bind_int(statement, 8, (int)segment->durability);
    if (segment->producer_task_id) {
      sqlite3_bind_int64(statement, 9, (sqlite3_int64)segment->producer_task_id);
    } else {
      sqlite3_bind_null(statement, 9);
    }
    sqlite3_bind_int64(statement, 10, segment->created_at);
    result = step_done(db, statement, "insert visual segment");
  }
  sqlite3_int64 segment_id = sqlite3_last_insert_rowid(db->connection);
  for (size_t i = 0; i < count && result == LARDON3D_PROJECT_DB_OK; ++i) {
    if (!valid_catalog_id(ids[i])) {
      result = LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
      break;
    }
    result = prepare(db, "INSERT INTO visual_index_memberships(visual_index_id,feature_set_id,"
                         "visual_index_segment_id) VALUES(?1,?2,?3)", &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_int64(statement, 1, (sqlite3_int64)segment->visual_index_id);
      sqlite3_bind_int64(statement, 2, (sqlite3_int64)ids[i]);
      sqlite3_bind_int64(statement, 3, segment_id);
      result = step_done(db, statement, "insert visual membership");
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(db, "COMMIT", "commit visual segment publish");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(db, "ROLLBACK", "rollback visual segment publish");
  }
  (void)pthread_mutex_unlock(&db->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result;
  }
  Lardon3DProjectDbVisualIndexSegment page[1];
  size_t page_count = 0;
  result = lardon3d_project_db_list_visual_index_segments(db, segment->visual_index_id,
                                                          segment->generation - 1, page, 1,
                                                          &page_count);
  if (result == LARDON3D_PROJECT_DB_OK && page_count == 1 &&
      page[0].generation == segment->generation) {
    *published = page[0];
    return LARDON3D_PROJECT_DB_OK;
  }
  return result == LARDON3D_PROJECT_DB_OK ? LARDON3D_PROJECT_DB_CORRUPT : result;
}

Lardon3DProjectDbResult lardon3d_project_db_record_visual_index_update_task(
    Lardon3DProjectDb *db, const Lardon3DTaskDurableSnapshot *snapshot, const char *kind,
    uint32_t version, const Lardon3DProjectDbCheckpoint *checkpoint,
    const Lardon3DProjectDbVisualIndexUpdateTask *parameters, int64_t updated_at) {
  if (!snapshot || !parameters || parameters->task_id != snapshot->id ||
      !valid_catalog_id(parameters->visual_index_id) ||
      parameters->after_feature_set_id > INT64_MAX) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  return record_task_internal(db, snapshot, kind, version, checkpoint, NULL, 0, NULL,
                              parameters, updated_at);
}

Lardon3DProjectDbResult lardon3d_project_db_load_visual_index_update_task(
    Lardon3DProjectDb *db, uint64_t task_id,
    Lardon3DProjectDbVisualIndexUpdateTask *parameters) {
  if (!db || !valid_task_id(task_id) || !parameters) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  memset(parameters, 0, sizeof(*parameters));
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db, "SELECT visual_index_id,after_feature_set_id FROM visual_index_update_tasks WHERE "
          "task_id=?1", &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)task_id);
    int code = sqlite3_step(statement);
    sqlite3_int64 index_id = sqlite3_column_int64(statement, 0);
    sqlite3_int64 after = sqlite3_column_int64(statement, 1);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW || index_id <= 0 || after < 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      parameters->task_id = task_id;
      parameters->visual_index_id = (uint64_t)index_id;
      parameters->after_feature_set_id = (uint64_t)after;
    }
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

#ifdef LARDON3D_PROJECT_DB_TESTING
Lardon3DProjectDbResult lardon3d_project_db_test_delete_feature_identity(
    Lardon3DProjectDb *database, uint64_t feature_set_id, uint64_t feature_asset_id) {
  if (!database || !valid_catalog_id(feature_set_id) || !valid_catalog_id(feature_asset_id)) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin delete feature identity");
  sqlite3_stmt *s = NULL;
  const char *sql[2] = {"DELETE FROM feature_sets WHERE feature_set_id=?1",
                        "DELETE FROM feature_assets WHERE feature_asset_id=?1"};
  uint64_t ids[2] = {feature_set_id, feature_asset_id};
  for (size_t i = 0; i < 2 && result == LARDON3D_PROJECT_DB_OK; ++i) {
    result = prepare(database, sql[i], &s);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_int64(s, 1, (sqlite3_int64)ids[i]);
      result = step_done(database, s, "delete feature identity");
      s = NULL;
      if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
        result = LARDON3D_PROJECT_DB_NOT_FOUND;
      }
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(database, "COMMIT", "commit delete feature identity");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(database, "ROLLBACK", "rollback delete feature identity");
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_project_db_test_orphan_checkpoint(Lardon3DProjectDb *database) {
  if (!database) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(database,
              "INSERT INTO checkpoints(task_id,path,format_version,durability,updated_at) "
              "VALUES(9223372036854775807,'orphan',1,0,0)",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = step_done(database, statement, "orphan checkpoint");
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_project_db_test_delete_catalog_identity(Lardon3DProjectDb *database, uint64_t scanset_id,
                                                 uint64_t image_id, uint64_t asset_id) {
  if (!database || !valid_catalog_id(scanset_id) || !valid_catalog_id(image_id) ||
      !valid_catalog_id(asset_id)) {
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  }
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin catalog identity deletion test");
  const char *sql[] = {
      "DELETE FROM images WHERE image_id=?1",
      "DELETE FROM image_assets WHERE asset_id=?1",
      "DELETE FROM scansets WHERE scanset_id=?1",
  };
  const uint64_t ids[] = {image_id, asset_id, scanset_id};
  for (size_t index = 0; index < 3 && result == LARDON3D_PROJECT_DB_OK; ++index) {
    sqlite3_stmt *statement = NULL;
    result = prepare(database, sql[index], &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)ids[index]);
      result = step_done(database, statement, "delete catalog identity test");
      if (result == LARDON3D_PROJECT_DB_OK && sqlite3_changes(database->connection) != 1) {
        result = LARDON3D_PROJECT_DB_NOT_FOUND;
      }
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = execute(database, "COMMIT", "commit catalog identity deletion test");
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    (void)execute(database, "ROLLBACK", "rollback catalog identity deletion test");
  }
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}
#endif
