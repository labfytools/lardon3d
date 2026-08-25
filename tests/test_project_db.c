#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/project_db.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      (void)fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);    \
      return false;                                                            \
    }                                                                          \
  } while (0)

Lardon3DProjectDbResult
lardon3d_project_db_test_orphan_checkpoint(Lardon3DProjectDb *database);
Lardon3DProjectDbResult lardon3d_project_db_test_delete_catalog_identity(
    Lardon3DProjectDb *database, uint64_t scanset_id, uint64_t image_id,
    uint64_t asset_id);

static void
asset_path_for_hash(const unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE],
                    char path[LARDON3D_PROJECT_DB_PATH_CAPACITY]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < sizeof(hex) / 2; ++index) {
    hex[index * 2] = digits[hash[index] >> 4];
    hex[index * 2 + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, LARDON3D_PROJECT_DB_PATH_CAPACITY,
                 "assets/images/%c%c/%s", hex[0], hex[1], hex);
}

static void feature_asset_path_for_hash(
    const unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE],
    char path[LARDON3D_PROJECT_DB_PATH_CAPACITY]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < LARDON3D_PROJECT_DB_SHA256_SIZE; ++index) {
    hex[index * 2] = digits[hash[index] >> 4];
    hex[index * 2 + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, LARDON3D_PROJECT_DB_PATH_CAPACITY,
                 "assets/features/%c%c/%s", hex[0], hex[1], hex);
}

static Lardon3DTaskDurableSnapshot task_snapshot(uint64_t id,
                                                 Lardon3DTaskState saved) {
  Lardon3DTaskDurableSnapshot snapshot = {
      .id = id,
      .estimate = {.minimum_batch_size = 1,
                   .maximum_batch_size = 1,
                   .desired_cpu_threads = 1},
      .progress = saved == TASK_COMPLETED ? 100 : 25,
      .saved_state = saved,
      .recovery_state =
          saved == TASK_RUNNING || saved == TASK_PAUSED ? TASK_PENDING : saved,
      .started_at = {.tv_sec = 10, .tv_nsec = 20},
      .finished_at = {.tv_sec = 30, .tv_nsec = 40},
      .sequence_count = 2,
  };
  (void)snprintf(snapshot.name, sizeof(snapshot.name), "Tâche %llu",
                 (unsigned long long)id);
  return snapshot;
}

typedef struct {
  Lardon3DProjectDb *database;
  bool success;
} ThreadContext;
typedef struct {
  Lardon3DProjectDb *database;
  uint64_t id;
  bool success;
} IdThreadContext;

static void *read_thread(void *userdata) {
  ThreadContext *context = userdata;
  context->success = true;
  for (size_t index = 0; index < 100; ++index) {
    Lardon3DProjectDbTask task;
    if (lardon3d_project_db_load_task(context->database, 1, &task) !=
        LARDON3D_PROJECT_DB_OK) {
      context->success = false;
      break;
    }
  }
  return NULL;
}

static void *allocate_id_thread(void *userdata) {
  IdThreadContext *context = userdata;
  context->success =
      lardon3d_project_db_allocate_task_id(context->database, &context->id) ==
      LARDON3D_PROJECT_DB_OK;
  return NULL;
}

static bool create_future_database(const char *path) {
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
  bool ok =
      sqlite3_exec(
          connection,
          "CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
          "INSERT INTO metadata VALUES('schema_version',18);",
          NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v7_database(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  lardon3d_project_db_close(database);
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE sparse_sfm_tasks;"
      "DROP TABLE sparse_landmark_observations;"
      "DROP TABLE sparse_landmarks;"
      "DROP TABLE sparse_registered_images;"
      "DROP TABLE sparse_reconstruction_components;"
      "DROP TABLE sparse_reconstructions;"
      "DROP TABLE sparse_calibration_scope_images;"
      "DROP TABLE sparse_calibration_scopes;"
      "DROP TABLE sparse_calibrations;"
      "DROP TABLE geometric_verifier_tasks;"
      "DROP TABLE geometric_verification_results;"
      "DROP TABLE matcher_tasks;"
      "DROP TABLE match_results;"
      "DROP TABLE candidate_pair_generate_tasks;"
      "DROP TABLE candidate_pairs;"
      "DROP TABLE track_observations;"
      "DROP TABLE track_builder_tasks;"
      "DROP TABLE tracks;"
      "DROP TABLE track_sets;"
      "UPDATE metadata SET value=7 WHERE key='schema_version';COMMIT;PRAGMA "
      "foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v6_database(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  lardon3d_project_db_close(database);
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE sparse_sfm_tasks;"
      "DROP TABLE sparse_landmark_observations;"
      "DROP TABLE sparse_landmarks;"
      "DROP TABLE sparse_registered_images;"
      "DROP TABLE sparse_reconstruction_components;"
      "DROP TABLE sparse_reconstructions;"
      "DROP TABLE sparse_calibration_scope_images;"
      "DROP TABLE sparse_calibration_scopes;"
      "DROP TABLE sparse_calibrations;"
      "DROP TABLE geometric_verifier_tasks;"
      "DROP TABLE geometric_verification_results;"
      "DROP TABLE matcher_tasks;"
      "DROP TABLE match_results;"
      "DROP TABLE candidate_pair_generate_tasks;"
      "DROP TABLE candidate_pairs;"
      "DROP TABLE feature_support_members;DROP TABLE feature_support_groups;"
      "DROP TABLE feature_support_sets;DROP TABLE sift_extract_tasks;"
      "ALTER TABLE feature_sets DROP COLUMN feature_density_per_megapixel;"
      "ALTER TABLE feature_sets DROP COLUMN coverage_ratio;"
      "ALTER TABLE feature_sets DROP COLUMN total_cells;"
      "ALTER TABLE feature_sets DROP COLUMN occupied_cells;"
      "DROP TABLE track_observations;"
      "DROP TABLE track_builder_tasks;"
      "DROP TABLE tracks;"
      "DROP TABLE track_sets;"
      "UPDATE metadata SET value=6 WHERE key='schema_version';COMMIT;PRAGMA "
      "foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v10_database(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  lardon3d_project_db_close(database);
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE sparse_sfm_tasks;"
      "DROP TABLE sparse_landmark_observations;"
      "DROP TABLE sparse_landmarks;"
      "DROP TABLE sparse_registered_images;"
      "DROP TABLE sparse_reconstruction_components;"
      "DROP TABLE sparse_reconstructions;"
      "DROP TABLE sparse_calibration_scope_images;"
      "DROP TABLE sparse_calibration_scopes;"
      "DROP TABLE sparse_calibrations;"
      "DROP TABLE geometric_verifier_tasks;"
      "DROP TABLE geometric_verification_results;"
      "DROP TABLE matcher_tasks;"
      "DROP TABLE track_observations;"
      "DROP TABLE track_builder_tasks;"
      "DROP TABLE tracks;"
      "DROP TABLE track_sets;"
      "UPDATE metadata SET value=10 WHERE key='schema_version';"
      "COMMIT;PRAGMA foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v13_database(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  lardon3d_project_db_close(database);
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE sparse_sfm_tasks;"
      "DROP TABLE sparse_landmark_observations;"
      "DROP TABLE sparse_landmarks;"
      "DROP TABLE sparse_registered_images;"
      "DROP TABLE sparse_reconstruction_components;"
      "DROP TABLE sparse_reconstructions;"
      "DROP TABLE sparse_calibration_scope_images;"
      "DROP TABLE sparse_calibration_scopes;"
      "DROP TABLE sparse_calibrations;"
      "DROP TABLE track_observations;"
      "DROP TABLE track_builder_tasks;"
      "DROP TABLE tracks;"
      "DROP TABLE track_sets;"
      "UPDATE metadata SET value=13 WHERE key='schema_version';COMMIT;PRAGMA "
      "foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v14_database(const char *path) {
  if (!create_v13_database(path))
    return false;
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "CREATE TABLE track_sets(track_set_id INTEGER PRIMARY KEY AUTOINCREMENT "
      "CHECK(track_set_id>0),builder_kind TEXT NOT NULL "
      "CHECK(length(builder_kind)>0 AND "
      "length(builder_kind)<=64),builder_version INTEGER NOT NULL "
      "CHECK(builder_version>0),"
      "parameter_fingerprint BLOB NOT NULL "
      "CHECK(length(parameter_fingerprint)=32),"
      "verifier_kind INTEGER NOT NULL CHECK(verifier_kind>0),verifier_version "
      "INTEGER NOT NULL "
      "CHECK(verifier_version>0),verifier_fingerprint BLOB NOT NULL "
      "CHECK(length(verifier_fingerprint)=32),"
      "input_scope_hash BLOB NOT NULL "
      "CHECK(length(input_scope_hash)=32),gvr_count INTEGER NOT NULL "
      "CHECK(gvr_count>=1),track_count INTEGER NOT NULL "
      "CHECK(track_count>=0),created_at INTEGER NOT NULL "
      "CHECK(created_at>=0),UNIQUE(builder_kind,builder_version,parameter_"
      "fingerprint,verifier_kind,"
      "verifier_version,verifier_fingerprint,input_scope_hash));"
      "CREATE TABLE tracks(track_id INTEGER PRIMARY KEY AUTOINCREMENT "
      "CHECK(track_id>0),"
      "track_set_id INTEGER NOT NULL REFERENCES track_sets(track_set_id) ON "
      "DELETE CASCADE,"
      "observation_count INTEGER NOT NULL CHECK(observation_count>=2));"
      "CREATE INDEX tracks_set_idx ON tracks(track_set_id,track_id);"
      "CREATE TABLE track_observations(track_set_id INTEGER NOT NULL,track_id "
      "INTEGER NOT NULL "
      "REFERENCES tracks(track_id) ON DELETE CASCADE,feature_set_id INTEGER "
      "NOT NULL "
      "REFERENCES feature_sets(feature_set_id),feature_index INTEGER NOT NULL "
      "CHECK(feature_index>=0),"
      "position_in_track INTEGER NOT NULL CHECK(position_in_track>=0),PRIMARY "
      "KEY(track_set_id,feature_set_id,"
      "feature_index),UNIQUE(track_id,position_in_track));"
      "CREATE INDEX track_observations_lookup_idx ON "
      "track_observations(feature_set_id,feature_index,"
      "track_set_id);UPDATE metadata SET value=14 WHERE "
      "key='schema_version';COMMIT;"
      "PRAGMA foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_true_v15_database(const char *path) {
  if (!create_v14_database(path))
    return false;
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "CREATE TABLE track_builder_tasks("
      "task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,"
      "builder_kind TEXT NOT NULL CHECK(builder_kind='track_builder'),"
      "builder_version INTEGER NOT NULL CHECK(builder_version>0),"
      "builder_fingerprint BLOB NOT NULL CHECK(length(builder_fingerprint)=32),"
      "verifier_kind INTEGER NOT NULL CHECK(verifier_kind>0),"
      "verifier_version INTEGER NOT NULL CHECK(verifier_version>0),"
      "verifier_fingerprint BLOB NOT NULL "
      "CHECK(length(verifier_fingerprint)=32),"
      "input_scope_hash BLOB NOT NULL CHECK(length(input_scope_hash)=32),"
      "gvr_count INTEGER NOT NULL CHECK(gvr_count>=1),"
      "scope_path TEXT NOT NULL CHECK(length(scope_path)>0 AND "
      "length(scope_path)<=4096),"
      "scope_size_bytes INTEGER NOT NULL CHECK(scope_size_bytes>0),"
      "scope_sha256 BLOB NOT NULL CHECK(length(scope_sha256)=32),"
      "scope_format_version INTEGER NOT NULL CHECK(scope_format_version=1));"
      "UPDATE metadata SET value=15 WHERE key='schema_version';"
      "COMMIT;PRAGMA foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool execute_test_sql(const char *path, const char *sql) {
  sqlite3 *connection = NULL;
  bool ok = sqlite3_open(path, &connection) == SQLITE_OK &&
            sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  if (connection)
    ok = sqlite3_close(connection) == SQLITE_OK && ok;
  return ok;
}

static bool pragma_is_clean(const char *path, const char *pragma,
                            const char *expected) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  bool clean = sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) ==
                   SQLITE_OK &&
               sqlite3_prepare_v2(connection, pragma, -1, &statement, NULL) ==
                   SQLITE_OK;
  if (clean) {
    int code = sqlite3_step(statement);
    if (strcmp(pragma, "PRAGMA foreign_key_check") == 0)
      clean = code == SQLITE_DONE;
    else
      clean = code == SQLITE_ROW &&
              strcmp((const char *)sqlite3_column_text(statement, 0), expected) ==
                  0 &&
              sqlite3_step(statement) == SQLITE_DONE;
  }
  if (statement)
    sqlite3_finalize(statement);
  if (connection)
    sqlite3_close(connection);
  return clean;
}

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} SchemaText;

static bool schema_append(SchemaText *text, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  va_list copy;
  va_copy(copy, arguments);
  int required = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (required < 0) {
    va_end(arguments);
    return false;
  }
  size_t needed = text->length + (size_t)required + 1;
  if (needed > text->capacity) {
    size_t capacity = text->capacity ? text->capacity : 1024;
    while (capacity < needed)
      capacity *= 2;
    char *grown = realloc(text->data, capacity);
    if (!grown) {
      va_end(arguments);
      return false;
    }
    text->data = grown;
    text->capacity = capacity;
  }
  (void)vsnprintf(text->data + text->length, text->capacity - text->length,
                  format, arguments);
  text->length += (size_t)required;
  va_end(arguments);
  return true;
}

static bool schema_append_normalized_sql(SchemaText *text, const char *sql) {
  bool whitespace = false;
  if (!schema_append(text, ""))
    return false;
  for (const unsigned char *cursor = (const unsigned char *)sql;
       cursor && *cursor; ++cursor) {
    if (isspace(*cursor)) {
      whitespace = true;
      continue;
    }
    if (whitespace && text->length && text->data[text->length - 1] != ' ') {
      if (!schema_append(text, " "))
        return false;
    }
    whitespace = false;
    if (!schema_append(text, "%c", *cursor))
      return false;
  }
  return true;
}

static char *schema_format(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  va_list copy;
  va_copy(copy, arguments);
  int required = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (required < 0) {
    va_end(arguments);
    return NULL;
  }
  char *result = malloc((size_t)required + 1);
  if (result)
    (void)vsnprintf(result, (size_t)required + 1, format, arguments);
  va_end(arguments);
  return result;
}

static int schema_string_compare(const void *left, const void *right) {
  const char *const *a = left;
  const char *const *b = right;
  return strcmp(*a, *b);
}

static void schema_string_array_free(char **items, size_t count) {
  for (size_t index = 0; index < count; ++index)
    free(items[index]);
  free(items);
}

static bool schema_append_pragma_lines(sqlite3 *connection, SchemaText *text,
                                       const char *table, const char *pragma) {
  char *query = sqlite3_mprintf("PRAGMA %s(%w)", pragma, table);
  if (!query)
    return false;
  sqlite3_stmt *statement = NULL;
  bool ok = sqlite3_prepare_v2(connection, query, -1, &statement, NULL) ==
            SQLITE_OK;
  sqlite3_free(query);
  char **lines = NULL;
  size_t count = 0;
  int code = SQLITE_ROW;
  while (ok && (code = sqlite3_step(statement)) == SQLITE_ROW) {
    char *line = NULL;
    if (strcmp(pragma, "table_info") == 0) {
      const unsigned char *name = sqlite3_column_text(statement, 1);
      const unsigned char *type = sqlite3_column_text(statement, 2);
      const unsigned char *default_value = sqlite3_column_text(statement, 4);
      line = schema_format("COLUMN|%d|%s|%s|%d|%s|%d",
                           sqlite3_column_int(statement, 0),
                           name ? (const char *)name : "",
                           type ? (const char *)type : "",
                           sqlite3_column_int(statement, 3),
                           default_value ? (const char *)default_value : "<NULL>",
                           sqlite3_column_int(statement, 5));
      if (!line)
        ok = false;
    } else if (strcmp(pragma, "foreign_key_list") == 0) {
      const unsigned char *target = sqlite3_column_text(statement, 2);
      const unsigned char *from = sqlite3_column_text(statement, 3);
      const unsigned char *to = sqlite3_column_text(statement, 4);
      const unsigned char *update = sqlite3_column_text(statement, 5);
      const unsigned char *delete_action = sqlite3_column_text(statement, 6);
      const unsigned char *match = sqlite3_column_text(statement, 7);
      line = schema_format("FK|%d|%d|%s|%s|%s|%s|%s|%s",
                           sqlite3_column_int(statement, 0),
                           sqlite3_column_int(statement, 1),
                           target ? (const char *)target : "",
                           from ? (const char *)from : "",
                           to ? (const char *)to : "",
                           update ? (const char *)update : "",
                           delete_action ? (const char *)delete_action : "",
                           match ? (const char *)match : "");
      if (!line)
        ok = false;
    } else {
      const unsigned char *name = sqlite3_column_text(statement, 1);
      const unsigned char *origin = sqlite3_column_text(statement, 3);
      const char *display_name = origin &&
                                         (strcmp((const char *)origin, "pk") == 0 ||
                                          strcmp((const char *)origin, "u") == 0)
                                     ? "<generated>"
                                     : (name ? (const char *)name : "");
      line = schema_format(
          "INDEX|%s|%s|%d|%d|%s|%d", display_name,
          name ? (const char *)name : "", sqlite3_column_int(statement, 0),
          sqlite3_column_int(statement, 2), origin ? (const char *)origin : "",
          sqlite3_column_int(statement, 4));
      if (!line)
        ok = false;
    }
    if (!ok || !line)
      break;
    char **grown = realloc(lines, (count + 1) * sizeof(*lines));
    if (!grown) {
      free(line);
      ok = false;
      break;
    }
    lines = grown;
    lines[count++] = line;
  }
  if (code != SQLITE_DONE)
    ok = false;
  if (statement)
    sqlite3_finalize(statement);
  if (ok && count > 1)
    qsort(lines, count, sizeof(*lines), schema_string_compare);
  for (size_t index = 0; ok && index < count; ++index) {
    if (strcmp(pragma, "index_list") == 0) {
      char display_name[256];
      char actual_name[256];
      char origin[8];
      int sequence = 0;
      int unique = 0;
      int partial = 0;
      if (sscanf(lines[index], "INDEX|%255[^|]|%255[^|]|%d|%d|%7[^|]|%d",
                 display_name, actual_name, &sequence, &unique, origin,
                 &partial) != 7 ||
          !schema_append(text, "index_list|INDEX|%s|%d|%d|%s|%d\n",
                         display_name, sequence, unique, origin, partial))
        ok = false;
      const char *name_start = strchr(lines[index], '|');
      if (name_start)
        ++name_start;
      const char *display_end = name_start ? strchr(name_start, '|') : NULL;
      const char *actual_start = display_end ? display_end + 1 : NULL;
      const char *actual_end = actual_start ? strchr(actual_start, '|') : NULL;
      size_t name_length = actual_end && actual_start
                               ? (size_t)(actual_end - actual_start)
                               : 0;
      char index_name[256];
      if (name_length >= sizeof(index_name))
        ok = false;
      else {
        memcpy(index_name, actual_start, name_length);
        index_name[name_length] = '\0';
        char *index_query = sqlite3_mprintf("PRAGMA index_info(%w)",
                                            index_name);
        sqlite3_stmt *index_statement = NULL;
        ok = index_query &&
             sqlite3_prepare_v2(connection, index_query, -1,
                                &index_statement, NULL) == SQLITE_OK;
        sqlite3_free(index_query);
        while (ok && sqlite3_step(index_statement) == SQLITE_ROW) {
          const char *index_label = strstr(lines[index], "|<generated>|")
                                        ? "<generated>"
                                        : index_name;
          if (!schema_append(text, "INDEX_COLUMN|%s|%d|%d|%s\n", index_label,
                             sqlite3_column_int(index_statement, 0),
                             sqlite3_column_int(index_statement, 1),
                             sqlite3_column_text(index_statement, 2)
                                 ? (const char *)sqlite3_column_text(
                                       index_statement, 2)
                                 : ""))
            ok = false;
        }
        if (index_statement)
          sqlite3_finalize(index_statement);
      }
    } else if (!schema_append(text, "%s|%s\n", pragma, lines[index]))
      ok = false;
  }
  schema_string_array_free(lines, count);
  return ok;
}

static bool schema_dump(const char *path, char **output) {
  sqlite3 *connection = NULL;
  if (sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) !=
      SQLITE_OK)
    return false;
  SchemaText text = {0};
  sqlite3_stmt *statement = NULL;
  bool ok = sqlite3_prepare_v2(
                connection,
                "SELECT type,name,sql FROM sqlite_master WHERE name NOT LIKE "
                "'sqlite_%' AND type IN ('table','index','trigger','view') "
                "ORDER BY type,name",
                -1, &statement, NULL) == SQLITE_OK;
  int code = SQLITE_ROW;
  while (ok && (code = sqlite3_step(statement)) == SQLITE_ROW) {
    const char *type = (const char *)sqlite3_column_text(statement, 0);
    const char *name = (const char *)sqlite3_column_text(statement, 1);
    const char *sql = (const char *)sqlite3_column_text(statement, 2);
    if (!schema_append(&text, "%s|%s|", type, name) ||
        !schema_append_normalized_sql(&text, sql ? sql : "") ||
        !schema_append(&text, "\n")) {
      ok = false;
      break;
    }
    if (strcmp(type, "table") == 0) {
      ok = schema_append_pragma_lines(connection, &text, name, "table_info") &&
           schema_append_pragma_lines(connection, &text, name,
                                       "foreign_key_list") &&
           schema_append_pragma_lines(connection, &text, name, "index_list");
    }
  }
  if (code != SQLITE_DONE)
    ok = false;
  if (statement)
    sqlite3_finalize(statement);
  if (sqlite3_close(connection) != SQLITE_OK)
    ok = false;
  if (!ok) {
    free(text.data);
    return false;
  }
  *output = text.data;
  return true;
}

static bool schema_compare(const char *fresh_path, const char *migrated_path,
                           bool report_difference) {
  char *fresh = NULL;
  char *migrated = NULL;
  bool fresh_ok = schema_dump(fresh_path, &fresh);
  bool migrated_ok = schema_dump(migrated_path, &migrated);
  bool ok = fresh_ok && migrated_ok;
  if (!ok && report_difference)
    fprintf(stderr, "schema comparator metadata collection failed (%d,%d)\n",
            fresh_ok, migrated_ok);
  if (ok && strcmp(fresh, migrated) != 0) {
    ok = false;
    if (report_difference)
      fprintf(stderr, "schema comparator mismatch: structural metadata differs\n");
  }
  free(fresh);
  free(migrated);
  return ok;
}

static bool create_v5_database(const char *path) {
  if (!create_v6_database(path))
    return false;
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE visual_index_update_tasks;DROP TABLE "
      "visual_index_memberships;"
      "DROP TABLE visual_index_segments;DROP TABLE visual_indexes;"
      "UPDATE metadata SET value=5 WHERE key='schema_version';COMMIT;PRAGMA "
      "foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v1_database(const char *path) {
  static const char sql[] =
      "PRAGMA foreign_keys=ON;"
      "CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
      "INSERT INTO metadata VALUES('schema_version',1);"
      "CREATE TABLE project(singleton INTEGER PRIMARY KEY "
      "CHECK(singleton=1),stable_id TEXT NOT "
      "NULL UNIQUE,name TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at "
      "INTEGER NOT NULL);"
      "CREATE TABLE tasks(task_id INTEGER PRIMARY KEY CHECK(task_id>0),name "
      "TEXT NOT "
      "NULL,saved_state INTEGER NOT NULL CHECK(saved_state BETWEEN 0 AND "
      "5),recovery_state "
      "INTEGER NOT NULL CHECK(recovery_state BETWEEN 0 AND 5),progress INTEGER "
      "NOT NULL "
      "CHECK(progress BETWEEN 0 AND 100),sequence_count INTEGER NOT NULL "
      "CHECK(sequence_count>=0),started_sec INTEGER NOT NULL,started_nsec "
      "INTEGER NOT NULL "
      "CHECK(started_nsec BETWEEN 0 AND 999999999),finished_sec INTEGER NOT "
      "NULL,finished_nsec "
      "INTEGER NOT NULL CHECK(finished_nsec BETWEEN 0 AND "
      "999999999),updated_at INTEGER NOT "
      "NULL);"
      "CREATE INDEX tasks_recovery_state_idx ON tasks(recovery_state,task_id);"
      "CREATE TABLE checkpoints(task_id INTEGER PRIMARY KEY REFERENCES "
      "tasks(task_id) ON DELETE "
      "CASCADE,path TEXT NOT NULL,format_version INTEGER NOT NULL "
      "CHECK(format_version>0),durability INTEGER NOT NULL CHECK(durability "
      "BETWEEN 0 AND "
      "1),updated_at INTEGER NOT NULL);"
      "CREATE TABLE artifacts(artifact_id TEXT PRIMARY KEY,kind TEXT NOT "
      "NULL,path TEXT NOT "
      "NULL,state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 1),size_bytes "
      "INTEGER NOT NULL "
      "CHECK(size_bytes>=0),producer_task_id INTEGER REFERENCES "
      "tasks(task_id),created_at "
      "INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
      "CREATE INDEX artifacts_state_idx ON artifacts(state,artifact_id);"
      "CREATE INDEX artifacts_producer_idx ON artifacts(producer_task_id);"
      "INSERT INTO project VALUES(1,'legacy-project','Legacy',1,1);"
      "INSERT INTO tasks VALUES(9,'Legacy task',1,0,12,3,1,0,0,0,2);"
      "INSERT INTO checkpoints VALUES(9,'legacy.chk',1,0,2);"
      "INSERT INTO artifacts "
      "VALUES('legacy-artifact','legacy','legacy.bin',0,0,9,2,2);";
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v2_database(const char *path) {
  if (!create_v1_database(path)) {
    return false;
  }
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
  bool ok =
      sqlite3_exec(connection,
                   "ALTER TABLE tasks ADD COLUMN task_kind TEXT;"
                   "ALTER TABLE tasks ADD COLUMN task_kind_version INTEGER "
                   "CHECK(task_kind_version IS NULL OR task_kind_version>0);"
                   "UPDATE tasks SET task_kind='test.work',task_kind_version=1 "
                   "WHERE task_id=9;"
                   "UPDATE metadata SET value=2 WHERE key='schema_version'",
                   NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v3_database(const char *path) {
  if (!create_v2_database(path)) {
    return false;
  }
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
  bool ok = sqlite3_exec(
                connection,
                "CREATE TABLE image_import_tasks(task_id INTEGER PRIMARY KEY "
                "REFERENCES "
                "tasks(task_id) ON DELETE CASCADE,source_path TEXT NOT NULL);"
                "INSERT INTO image_import_tasks VALUES(9,'/legacy/source');"
                "INSERT INTO metadata(key,value) VALUES('next_task_id',10);"
                "UPDATE metadata SET value=3 WHERE key='schema_version'",
                NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool create_v4_database(const char *path) {
  if (!create_v3_database(path)) {
    return false;
  }
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
  const char *sql =
      "CREATE TABLE scansets(scanset_id INTEGER PRIMARY KEY AUTOINCREMENT "
      "CHECK(scanset_id>0),name TEXT NOT NULL,created_at INTEGER NOT "
      "NULL,updated_at INTEGER NOT "
      "NULL);"
      "CREATE TABLE image_assets(asset_id INTEGER PRIMARY KEY AUTOINCREMENT "
      "CHECK(asset_id>0),sha256 BLOB NOT NULL UNIQUE,path TEXT NOT NULL "
      "UNIQUE,size_bytes "
      "INTEGER NOT NULL,state INTEGER NOT NULL,created_at INTEGER NOT NULL);"
      "CREATE TABLE images(image_id INTEGER PRIMARY KEY AUTOINCREMENT "
      "CHECK(image_id>0),scanset_id INTEGER NOT NULL REFERENCES "
      "scansets(scanset_id),asset_id "
      "INTEGER NOT NULL REFERENCES image_assets(asset_id),original_name TEXT "
      "NOT "
      "NULL,source_path TEXT NOT NULL,producer_task_id INTEGER REFERENCES "
      "tasks(task_id),imported_at INTEGER NOT "
      "NULL,UNIQUE(scanset_id,asset_id));"
      "CREATE INDEX images_scanset_idx ON images(scanset_id,image_id);"
      "CREATE INDEX images_producer_idx ON images(producer_task_id,image_id);"
      "ALTER TABLE image_import_tasks ADD COLUMN scanset_id INTEGER REFERENCES "
      "scansets(scanset_id);"
      "INSERT INTO scansets(name,created_at,updated_at) VALUES('Legacy',0,0);"
      "UPDATE image_import_tasks SET scanset_id=1;"
      "INSERT INTO metadata VALUES('legacy_image_catalog_pending',1);"
      "UPDATE metadata SET value=4 WHERE key='schema_version';";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static bool query_integer(const char *path, const char *sql,
                          sqlite3_int64 expected) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  if (sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) !=
          SQLITE_OK ||
      sqlite3_prepare_v2(connection, sql, -1, &statement, NULL) != SQLITE_OK) {
    if (statement) {
      (void)sqlite3_finalize(statement);
    }
    if (connection) {
      (void)sqlite3_close(connection);
    }
    return false;
  }
  bool matches = sqlite3_step(statement) == SQLITE_ROW &&
                 sqlite3_column_int64(statement, 0) == expected &&
                 sqlite3_step(statement) == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK &&
         sqlite3_close(connection) == SQLITE_OK && matches;
}

static void init_track_configuration(Lardon3DProjectDbTrackSet *configuration,
                                     unsigned char scope_byte,
                                     uint64_t gvr_count, size_t track_count) {
  memset(configuration, 0, sizeof(*configuration));
  (void)snprintf(configuration->builder_kind,
                 sizeof(configuration->builder_kind), "gate-c-track-builder");
  configuration->builder_version = 1;
  configuration->parameter_fingerprint[0] = 0x10;
  configuration->verifier_kind = 1;
  configuration->verifier_version = 1;
  configuration->verifier_fingerprint[0] = 0x20;
  memset(configuration->input_scope_hash, scope_byte,
         sizeof(configuration->input_scope_hash));
  configuration->gvr_count = gvr_count;
  configuration->track_count = track_count;
  configuration->created_at = 1000;
}

static bool run_test(void) {
  char directory[] = "/tmp/lardon3d-project-db-XXXXXX";
  CHECK(mkdtemp(directory));
  char database_path[512], artifact_path[512], future_path[512],
      corrupt_path[512];
  char legacy_path[512], failed_migration_path[512], v2_path[512];
  char failed_v3_migration_path[512], v3_path[512], failed_v4_path[512];
  char v4_path[512], failed_v5_path[512], failed_v6_path[512],
      failed_v7_path[512];
  char direct_v5_path[512], v8_path[512], failed_v8_path[512];
  char v10_path[512], failed_v11_path[512];
  char v13_path[512], true_v14_path[512], failed_v14_path[512],
      failed_v15_path[512], true_v15_path[512];
  CHECK(snprintf(database_path, sizeof(database_path), "%s/project.db",
                 directory) > 0);
  CHECK(snprintf(artifact_path, sizeof(artifact_path), "%s/artifact.bin",
                 directory) > 0);
  CHECK(snprintf(future_path, sizeof(future_path), "%s/future.db", directory) >
        0);
  CHECK(snprintf(corrupt_path, sizeof(corrupt_path), "%s/corrupt.db",
                 directory) > 0);
  CHECK(snprintf(legacy_path, sizeof(legacy_path), "%s/legacy.db", directory) >
        0);
  CHECK(snprintf(failed_migration_path, sizeof(failed_migration_path),
                 "%s/failed-migration.db", directory) > 0);
  CHECK(snprintf(v2_path, sizeof(v2_path), "%s/v2.db", directory) > 0);
  CHECK(snprintf(failed_v3_migration_path, sizeof(failed_v3_migration_path),
                 "%s/failed-v3-migration.db", directory) > 0);
  CHECK(snprintf(v3_path, sizeof(v3_path), "%s/v3.db", directory) > 0);
  CHECK(snprintf(failed_v4_path, sizeof(failed_v4_path),
                 "%s/failed-v4-migration.db", directory) > 0);
  CHECK(snprintf(v4_path, sizeof(v4_path), "%s/v4.db", directory) > 0);
  CHECK(snprintf(failed_v5_path, sizeof(failed_v5_path),
                 "%s/failed-v5-migration.db", directory) > 0);
  CHECK(snprintf(failed_v6_path, sizeof(failed_v6_path),
                 "%s/failed-v6-migration.db", directory) > 0);
  CHECK(snprintf(failed_v7_path, sizeof(failed_v7_path),
                 "%s/failed-v7-migration.db", directory) > 0);
  CHECK(snprintf(direct_v5_path, sizeof(direct_v5_path), "%s/direct-v5.db",
                 directory) > 0);
  CHECK(snprintf(v8_path, sizeof(v8_path), "%s/v8.db", directory) > 0);
  CHECK(snprintf(failed_v8_path, sizeof(failed_v8_path),
                 "%s/failed-v8-migration.db", directory) > 0);
  CHECK(snprintf(v10_path, sizeof(v10_path), "%s/v10.db", directory) > 0);
  CHECK(snprintf(failed_v11_path, sizeof(failed_v11_path), "%s/failed-v11.db",
                 directory) > 0);
  CHECK(snprintf(v13_path, sizeof(v13_path), "%s/v13.db", directory) > 0);
  CHECK(snprintf(true_v14_path, sizeof(true_v14_path), "%s/true-v14.db",
                 directory) > 0);
  CHECK(snprintf(failed_v14_path, sizeof(failed_v14_path), "%s/failed-v14.db",
                 directory) > 0);
  CHECK(snprintf(failed_v15_path, sizeof(failed_v15_path), "%s/failed-v15.db",
                 directory) > 0);
  CHECK(snprintf(true_v15_path, sizeof(true_v15_path), "%s/true-v15.db",
                 directory) > 0);

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *database = NULL;
  CHECK(lardon3d_project_db_open(database_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(database && lardon3d_project_db_schema_version(database) == 17);
  bool legacy_pending = true;
  CHECK(lardon3d_project_db_legacy_catalog_pending(database, &legacy_pending) ==
            LARDON3D_PROJECT_DB_OK &&
        !legacy_pending);

  Lardon3DProjectDbScanSet deleted_scanset;
  CHECK(lardon3d_project_db_create_scanset(database, "Deleted identity",
                                           &deleted_scanset) ==
        LARDON3D_PROJECT_DB_OK);
  unsigned char first_hash[LARDON3D_PROJECT_DB_SHA256_SIZE] = {1};
  char first_asset_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(first_hash, first_asset_path);
  Lardon3DProjectDbImageRegisterStatus identity_status;
  Lardon3DProjectDbImage deleted_image;
  CHECK(lardon3d_project_db_register_image(
            database, deleted_scanset.scanset_id, first_hash, first_asset_path,
            1, "deleted.jpg", "/source/deleted.jpg", 0, 1, &identity_status,
            &deleted_image) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_test_delete_catalog_identity(
            database, deleted_scanset.scanset_id, deleted_image.image_id,
            deleted_image.asset_id) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet replacement_scanset;
  CHECK(lardon3d_project_db_create_scanset(database, "Replacement identity",
                                           &replacement_scanset) ==
            LARDON3D_PROJECT_DB_OK &&
        replacement_scanset.scanset_id > deleted_scanset.scanset_id);
  unsigned char second_hash[LARDON3D_PROJECT_DB_SHA256_SIZE] = {2};
  char second_asset_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(second_hash, second_asset_path);
  Lardon3DProjectDbImage replacement_image;
  CHECK(lardon3d_project_db_register_image(
            database, replacement_scanset.scanset_id, second_hash,
            second_asset_path, 1, "replacement.jpg", "/source/replacement.jpg",
            0, 2, &identity_status,
            &replacement_image) == LARDON3D_PROJECT_DB_OK &&
        replacement_image.image_id > deleted_image.image_id &&
        replacement_image.asset_id > deleted_image.asset_id);

  unsigned char third_hash[LARDON3D_PROJECT_DB_SHA256_SIZE] = {3};
  char third_asset_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(third_hash, third_asset_path);
  Lardon3DProjectDbImage pair_image;
  CHECK(lardon3d_project_db_register_image(
            database, replacement_scanset.scanset_id, third_hash,
            third_asset_path, 1, "pair-a.jpg", "/source/pair-a.jpg", 0, 3,
            &identity_status, &pair_image) == LARDON3D_PROJECT_DB_OK);
  unsigned char fourth_hash[LARDON3D_PROJECT_DB_SHA256_SIZE] = {4};
  char fourth_asset_path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(fourth_hash, fourth_asset_path);
  Lardon3DProjectDbImage pair_image_b;
  CHECK(lardon3d_project_db_register_image(
            database, replacement_scanset.scanset_id, fourth_hash,
            fourth_asset_path, 1, "pair-b.jpg", "/source/pair-b.jpg", 0, 4,
            &identity_status, &pair_image_b) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbCandidatePair pair;
  CHECK(lardon3d_project_db_create_candidate_pair(
            database, replacement_image.image_id, pair_image.image_id, 10,
            &pair) == LARDON3D_PROJECT_DB_OK);
  CHECK(pair.candidate_pair_id > 0 &&
        pair.image_id_a == replacement_image.image_id &&
        pair.image_id_b == pair_image.image_id && pair.created_at == 10);
  Lardon3DProjectDbCandidatePair first_pair = pair;
  Lardon3DProjectDbCandidatePair second_pair;
  CHECK(lardon3d_project_db_create_candidate_pair(
            database, replacement_image.image_id, pair_image_b.image_id, 11,
            &second_pair) == LARDON3D_PROJECT_DB_OK);
  CHECK(second_pair.candidate_pair_id > first_pair.candidate_pair_id);
  CHECK(lardon3d_project_db_create_candidate_pair(
            database, pair_image.image_id, replacement_image.image_id, 12,
            &pair) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_create_candidate_pair(
            database, replacement_image.image_id, replacement_image.image_id,
            13, &pair) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_create_candidate_pair(
            database, replacement_image.image_id, pair_image.image_id, -1,
            &pair) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_candidate_pair(
            database, 0, pair_image.image_id, 14, &pair) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  Lardon3DProjectDbCandidatePair found;
  CHECK(lardon3d_project_db_find_candidate_pair(
            database, pair_image.image_id, replacement_image.image_id,
            &found) == LARDON3D_PROJECT_DB_OK);
  CHECK(found.candidate_pair_id == first_pair.candidate_pair_id &&
        found.image_id_a == replacement_image.image_id &&
        found.image_id_b == pair_image.image_id && found.created_at == 10);
  CHECK(lardon3d_project_db_find_candidate_pair(
            database, replacement_image.image_id, pair_image.image_id,
            &found) == LARDON3D_PROJECT_DB_OK);
  CHECK(found.candidate_pair_id == first_pair.candidate_pair_id);
  CHECK(lardon3d_project_db_find_candidate_pair(
            database, pair_image.image_id, pair_image_b.image_id, &found) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  Lardon3DProjectDbCandidatePair loaded;
  CHECK(lardon3d_project_db_load_candidate_pair(
            database, first_pair.candidate_pair_id, &loaded) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded.image_id_a == first_pair.image_id_a &&
        loaded.image_id_b == first_pair.image_id_b && loaded.created_at == 10);
  CHECK(lardon3d_project_db_load_candidate_pair(database, 999999, &loaded) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  Lardon3DProjectDbCandidatePair
      pair_page[LARDON3D_PROJECT_DB_CANDIDATE_PAIR_PAGE_MAX];
  size_t pair_count = 0;
  CHECK(lardon3d_project_db_list_candidate_pairs(
            database, 0, pair_page, 2, &pair_count) == LARDON3D_PROJECT_DB_OK);
  CHECK(pair_count == 2 &&
        pair_page[0].candidate_pair_id == first_pair.candidate_pair_id &&
        pair_page[1].candidate_pair_id == second_pair.candidate_pair_id);
  CHECK(lardon3d_project_db_list_candidate_pairs(
            database, first_pair.candidate_pair_id, pair_page, 2,
            &pair_count) == LARDON3D_PROJECT_DB_OK);
  CHECK(pair_count == 1 &&
        pair_page[0].candidate_pair_id == second_pair.candidate_pair_id);
  CHECK(lardon3d_project_db_list_candidate_pairs(
            database, second_pair.candidate_pair_id, pair_page, 1,
            &pair_count) == LARDON3D_PROJECT_DB_OK);
  CHECK(pair_count == 0);
  CHECK(lardon3d_project_db_list_candidate_pairs(
            database, 0, pair_page,
            LARDON3D_PROJECT_DB_CANDIDATE_PAIR_PAGE_MAX + 1,
            &pair_count) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  Lardon3DProjectDbProject project = {.created_at = 100, .updated_at = 100};
  (void)snprintf(project.stable_id, sizeof(project.stable_id), "project-0001");
  (void)snprintf(project.name, sizeof(project.name), "Projet test");
  CHECK(lardon3d_project_db_set_project(database, &project) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbProject loaded_project;
  CHECK(lardon3d_project_db_get_project(database, &loaded_project) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(strcmp(loaded_project.stable_id, project.stable_id) == 0);
  project.updated_at = 101;
  (void)snprintf(project.name, sizeof(project.name), "Projet renommé");
  CHECK(lardon3d_project_db_set_project(database, &project) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbProject conflict = project;
  (void)snprintf(conflict.stable_id, sizeof(conflict.stable_id), "other");
  CHECK(lardon3d_project_db_set_project(database, &conflict) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  char copied_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_last_error(database, copied_error));
  CHECK(copied_error[0] != '\0');
  project = loaded_project;
  memset(project.name, 'x', sizeof(project.name));
  CHECK(lardon3d_project_db_set_project(database, &project) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  Lardon3DTaskDurableSnapshot running = task_snapshot(1, TASK_RUNNING);
  Lardon3DProjectDbCheckpoint checkpoint = {.format_version = 1,
                                            .durability =
                                                LARDON3D_DB_CHECKPOINT_DURABLE,
                                            .updated_at = 200};
  (void)snprintf(checkpoint.path, sizeof(checkpoint.path),
                 "%s/checkpoints/task-1.chk", directory);
  CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 1,
                                        &checkpoint,
                                        200) == LARDON3D_PROJECT_DB_OK);
  running.progress = 30;
  running.sequence_count = 3;
  CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 1,
                                        &checkpoint,
                                        201) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet import_scanset;
  CHECK(lardon3d_project_db_create_scanset(database, "Legacy import",
                                           &import_scanset) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbTask task;
  CHECK(lardon3d_project_db_load_task(database, 1, &task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(task.recovery_state == TASK_PENDING && task.progress == 30 &&
        task.sequence_count == 3);
  CHECK(task.has_task_kind && strcmp(task.task_kind, "test.work") == 0 &&
        task.task_kind_version == 1);
  CHECK(task.has_checkpoint &&
        strcmp(task.checkpoint.path, checkpoint.path) == 0);
  checkpoint.durability = LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE;
  CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 1,
                                        &checkpoint,
                                        201) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_task(database, 1, &task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(task.checkpoint.durability ==
        LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE);
  CHECK(lardon3d_project_db_record_task(database, &running, "test.other", 1,
                                        &checkpoint,
                                        202) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_record_task(database, &running, "Test.invalid", 1,
                                        &checkpoint, 202) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_record_task(database, &running, "test.work", 0,
                                        &checkpoint, 202) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_record_image_import_task(
            database, &running, "import.images", 1, &checkpoint,
            "/tmp/source-a", import_scanset.scanset_id,
            202) == LARDON3D_PROJECT_DB_CONSTRAINT);

  Lardon3DTaskDurableSnapshot completed = task_snapshot(2, TASK_COMPLETED);
  CHECK(lardon3d_project_db_record_task(database, &completed, "test.work", 1,
                                        NULL, 202) == LARDON3D_PROJECT_DB_OK);
  Lardon3DTaskDurableSnapshot no_checkpoint = task_snapshot(4, TASK_PENDING);
  CHECK(lardon3d_project_db_record_task(database, &no_checkpoint, NULL, 0, NULL,
                                        202) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbTask page[1];
  size_t count = 0;
  CHECK(lardon3d_project_db_list_recoverable(database, 0, page, 1, &count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(count == 1 && page[0].task_id == 1);
  CHECK(lardon3d_project_db_list_recoverable(database, 1, page, 1, &count) ==
            LARDON3D_PROJECT_DB_OK &&
        count == 0);
  CHECK(lardon3d_project_db_list_recoverable(
            database, 0, page, LARDON3D_PROJECT_DB_RECOVERY_PAGE_MAX + 1,
            &count) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  Lardon3DTaskDurableSnapshot image_import = task_snapshot(5, TASK_PENDING);
  CHECK(lardon3d_project_db_record_image_import_task(
            database, &image_import, "import.images", 1, &checkpoint,
            "/tmp/source-a", import_scanset.scanset_id,
            202) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbImageImport import_parameters;
  CHECK(lardon3d_project_db_load_image_import(
            database, 5, &import_parameters) == LARDON3D_PROJECT_DB_OK);
  CHECK(import_parameters.task_id == 5 &&
        strcmp(import_parameters.source_path, "/tmp/source-a") == 0 &&
        import_parameters.scanset_id == import_scanset.scanset_id);
  CHECK(lardon3d_project_db_record_image_import_task(
            database, &image_import, "import.images", 1, &checkpoint,
            "/tmp/source-b", import_scanset.scanset_id,
            203) == LARDON3D_PROJECT_DB_CONSTRAINT);

  Lardon3DTaskDurableSnapshot rollback_task = task_snapshot(3, TASK_PENDING);
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT", "1", 1) == 0);
  CHECK(lardon3d_project_db_record_task(database, &rollback_task, "test.work",
                                        1, &checkpoint,
                                        203) == LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_CHECKPOINT") == 0);
  CHECK(lardon3d_project_db_load_task(database, 3, &task) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  CHECK(lardon3d_project_db_test_orphan_checkpoint(database) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);

  Lardon3DProjectDbArtifact artifact = {.state = LARDON3D_DB_ARTIFACT_STAGED,
                                        .has_producer_task = true,
                                        .producer_task_id = 1,
                                        .created_at = 300,
                                        .updated_at = 300};
  (void)snprintf(artifact.artifact_id, sizeof(artifact.artifact_id),
                 "artifact-1");
  (void)snprintf(artifact.kind, sizeof(artifact.kind), "generic-test");
  (void)snprintf(artifact.path, sizeof(artifact.path), "%s", artifact_path);
  CHECK(lardon3d_project_db_create_artifact(database, &artifact) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbArtifact loaded_artifact;
  CHECK(lardon3d_project_db_load_artifact(database, artifact.artifact_id,
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_artifact.state == LARDON3D_DB_ARTIFACT_STAGED);
  CHECK(lardon3d_project_db_mark_artifact_ready(database, artifact.artifact_id,
                                                301) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  int descriptor = open(artifact_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(descriptor >= 0);
  const char payload[] = "validated";
  CHECK(write(descriptor, payload, sizeof(payload)) ==
        (ssize_t)sizeof(payload));
  CHECK(close(descriptor) == 0);
  CHECK(lardon3d_project_db_mark_artifact_ready(database, artifact.artifact_id,
                                                301) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_artifact(database, artifact.artifact_id,
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_artifact.state == LARDON3D_DB_ARTIFACT_READY &&
        loaded_artifact.size_bytes == sizeof(payload));
  CHECK(lardon3d_project_db_create_artifact(database, &artifact) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  artifact.producer_task_id = 999;
  (void)snprintf(artifact.artifact_id, sizeof(artifact.artifact_id),
                 "orphan-artifact");
  CHECK(lardon3d_project_db_create_artifact(database, &artifact) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  artifact = loaded_artifact;
  artifact.state = LARDON3D_DB_ARTIFACT_STAGED;
  artifact.size_bytes = 0;
  memset(artifact.path, 'x', sizeof(artifact.path));
  CHECK(lardon3d_project_db_create_artifact(database, &artifact) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  ThreadContext contexts[2] = {{.database = database}, {.database = database}};
  pthread_t threads[2];
  CHECK(pthread_create(&threads[0], NULL, read_thread, &contexts[0]) == 0);
  CHECK(pthread_create(&threads[1], NULL, read_thread, &contexts[1]) == 0);
  CHECK(pthread_join(threads[0], NULL) == 0 &&
        pthread_join(threads[1], NULL) == 0);
  CHECK(contexts[0].success && contexts[1].success);
  IdThreadContext id_contexts[2] = {{.database = database},
                                    {.database = database}};
  CHECK(pthread_create(&threads[0], NULL, allocate_id_thread,
                       &id_contexts[0]) == 0);
  CHECK(pthread_create(&threads[1], NULL, allocate_id_thread,
                       &id_contexts[1]) == 0);
  CHECK(pthread_join(threads[0], NULL) == 0 &&
        pthread_join(threads[1], NULL) == 0);
  CHECK(id_contexts[0].success && id_contexts[1].success &&
        id_contexts[0].id != id_contexts[1].id && id_contexts[0].id > 5 &&
        id_contexts[1].id > 5);

  char too_long[LARDON3D_PROJECT_DB_PATH_CAPACITY + 1];
  memset(too_long, 'x', sizeof(too_long));
  too_long[sizeof(too_long) - 1] = '\0';
  CHECK(lardon3d_project_db_open(too_long, &database, error) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  lardon3d_project_db_close(contexts[0].database);
  database = NULL;
  CHECK(query_integer(database_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      17));
  CHECK(query_integer(database_path,
                      "SELECT count(*) FROM tasks WHERE task_id=1", 1));
  CHECK(lardon3d_project_db_open(database_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_task(database, 1, &task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_artifact(database, "artifact-1",
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_find_candidate_pair(
            database, replacement_image.image_id, pair_image.image_id,
            &found) == LARDON3D_PROJECT_DB_OK);
  CHECK(found.candidate_pair_id == first_pair.candidate_pair_id);
  lardon3d_project_db_close(database);

  CHECK(create_future_database(future_path));
  CHECK(lardon3d_project_db_open(future_path, &database, error) ==
        LARDON3D_PROJECT_DB_UNSUPPORTED_SCHEMA);
  descriptor = open(corrupt_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(descriptor >= 0);
  CHECK(write(descriptor, "not sqlite", 10) == 10 && close(descriptor) == 0);
  CHECK(lardon3d_project_db_open(corrupt_path, &database, error) ==
        LARDON3D_PROJECT_DB_CORRUPT);

  CHECK(create_v1_database(legacy_path));
  CHECK(lardon3d_project_db_open(legacy_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  CHECK(lardon3d_project_db_get_project(database, &loaded_project) ==
            LARDON3D_PROJECT_DB_OK &&
        strcmp(loaded_project.stable_id, "legacy-project") == 0);
  CHECK(lardon3d_project_db_load_task(database, 9, &task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(!task.has_task_kind && task.has_checkpoint &&
        strcmp(task.checkpoint.path, "legacy.chk") == 0);
  CHECK(lardon3d_project_db_load_artifact(database, "legacy-artifact",
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(legacy_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      17));

  CHECK(create_v1_database(failed_migration_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V2", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_migration_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V2") == 0);
  CHECK(query_integer(failed_migration_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      1));
  CHECK(lardon3d_project_db_open(failed_migration_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_task(database, 9, &task) ==
            LARDON3D_PROJECT_DB_OK &&
        !task.has_task_kind);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v2_database(v2_path));
  CHECK(lardon3d_project_db_open(v2_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_task(database, 9, &task) ==
            LARDON3D_PROJECT_DB_OK &&
        task.has_task_kind && strcmp(task.task_kind, "test.work") == 0);
  CHECK(task.has_checkpoint && strcmp(task.checkpoint.path, "legacy.chk") == 0);
  CHECK(lardon3d_project_db_load_artifact(database, "legacy-artifact",
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(
      v2_path, "SELECT value FROM metadata WHERE key='schema_version'", 17));

  CHECK(create_v2_database(failed_v3_migration_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V3", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v3_migration_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V3") == 0);
  CHECK(query_integer(failed_v3_migration_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      2));
  CHECK(query_integer(failed_v3_migration_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' "
                      "AND name='image_import_tasks'",
                      0));

  CHECK(create_v3_database(v3_path));
  CHECK(lardon3d_project_db_open(v3_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_image_import(
            database, 9, &import_parameters) == LARDON3D_PROJECT_DB_OK);
  CHECK(strcmp(import_parameters.source_path, "/legacy/source") == 0 &&
        import_parameters.scanset_id > 0);
  CHECK(lardon3d_project_db_legacy_catalog_pending(database, &legacy_pending) ==
            LARDON3D_PROJECT_DB_OK &&
        legacy_pending);
  CHECK(lardon3d_project_db_load_task(database, 9, &task) ==
            LARDON3D_PROJECT_DB_OK &&
        task.has_task_kind);
  CHECK(lardon3d_project_db_load_artifact(database, "legacy-artifact",
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(
      v3_path, "SELECT value FROM metadata WHERE key='schema_version'", 17));

  CHECK(create_v3_database(failed_v4_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V4", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v4_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V4") == 0);
  CHECK(query_integer(failed_v4_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      3));
  CHECK(query_integer(failed_v4_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' "
                      "AND name='scansets'",
                      0));

  CHECK(create_v4_database(v4_path));
  Lardon3DProjectDbResult v4_result =
      lardon3d_project_db_open(v4_path, &database, error);
  if (v4_result != LARDON3D_PROJECT_DB_OK) {
    fprintf(stderr, "Migration v4 (%d): %s\n", (int)v4_result, error);
  }
  CHECK(v4_result == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  CHECK(lardon3d_project_db_load_task(database, 9, &task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_artifact(database, "legacy-artifact",
                                          &loaded_artifact) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v4_database(failed_v5_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V5", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v5_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V5") == 0);
  CHECK(query_integer(failed_v5_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      4));
  CHECK(query_integer(failed_v5_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' "
                      "AND name='feature_sets'",
                      0));

  CHECK(create_v4_database(failed_v6_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V6", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v6_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V6") == 0);
  CHECK(query_integer(failed_v6_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      4));
  CHECK(query_integer(failed_v6_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' "
                      "AND name='visual_indexes'",
                      0));

  CHECK(create_v6_database(failed_v7_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V7", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v7_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V7") == 0);
  CHECK(query_integer(failed_v7_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      6));
  CHECK(
      query_integer(failed_v7_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='sift_extract_tasks'",
                    0));
  Lardon3DProjectDbResult retry_v7 =
      lardon3d_project_db_open(failed_v7_path, &database, error);
  if (retry_v7 != LARDON3D_PROJECT_DB_OK) {
    fprintf(stderr, "Nouvelle tentative migration v7 (%d): %s\n", (int)retry_v7,
            error);
  }
  CHECK(retry_v7 == LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v5_database(direct_v5_path));
  CHECK(query_integer(direct_v5_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      5));
  CHECK(lardon3d_project_db_open(direct_v5_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v7_database(v8_path));
  CHECK(query_integer(
      v8_path, "SELECT value FROM metadata WHERE key='schema_version'", 7));
  CHECK(lardon3d_project_db_open(v8_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(
      v8_path, "SELECT value FROM metadata WHERE key='schema_version'", 17));

  CHECK(create_v7_database(failed_v8_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V8", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v8_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V8") == 0);
  CHECK(query_integer(failed_v8_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      7));
  CHECK(
      query_integer(failed_v8_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='candidate_pairs'",
                    0));
  CHECK(lardon3d_project_db_open(failed_v8_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v10_database(v10_path));
  CHECK(query_integer(
      v10_path, "SELECT value FROM metadata WHERE key='schema_version'", 10));
  CHECK(
      query_integer(v10_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='match_results'",
                    1));
  CHECK(
      query_integer(v10_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='matcher_tasks'",
                    0));
  CHECK(lardon3d_project_db_open(v10_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(
      v10_path, "SELECT value FROM metadata WHERE key='schema_version'", 17));
  CHECK(
      query_integer(v10_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='matcher_tasks'",
                    1));

  CHECK(create_v10_database(failed_v11_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V11", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v11_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V11") == 0);
  CHECK(query_integer(failed_v11_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      10));
  CHECK(
      query_integer(failed_v11_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='matcher_tasks'",
                    0));
  CHECK(
      query_integer(failed_v11_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='match_results'",
                    1));
  CHECK(lardon3d_project_db_open(failed_v11_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v13_database(v13_path));
  CHECK(query_integer(
      v13_path, "SELECT value FROM metadata WHERE key='schema_version'", 13));
  CHECK(
      query_integer(v13_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='track_sets'",
                    0));
  CHECK(lardon3d_project_db_open(v13_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(
      v13_path, "SELECT value FROM metadata WHERE key='schema_version'", 17));
  CHECK(
      query_integer(v13_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='track_sets'",
                    1));

  CHECK(create_v14_database(true_v14_path));
  CHECK(query_integer(true_v14_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      14));
  CHECK(
      query_integer(true_v14_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='track_builder_tasks'",
                    0));
  Lardon3DProjectDbResult true_v14_result =
      lardon3d_project_db_open(true_v14_path, &database, error);
  if (true_v14_result != LARDON3D_PROJECT_DB_OK) {
    fprintf(stderr, "true v14 upgrade: %d %s path=%s\n", true_v14_result, error,
            true_v14_path);
  }
  CHECK(true_v14_result == LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(
      query_integer(true_v14_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='track_builder_tasks'",
                    1));

  CHECK(create_true_v15_database(true_v15_path));
  CHECK(query_integer(true_v15_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      15));
  const char *v16_tables[] = {
      "sparse_calibrations", "sparse_calibration_scopes",
      "sparse_calibration_scope_images", "sparse_reconstructions",
      "sparse_reconstruction_components", "sparse_registered_images",
      "sparse_landmarks", "sparse_landmark_observations"};
  const char *v16_indexes[] = {
      "sparse_scope_images_calibration_idx",
      "sparse_scope_images_calibration_lookup_idx",
      "sparse_reconstructions_scope_idx", "sparse_components_page_idx",
      "sparse_registered_page_idx", "sparse_registered_image_lookup_idx",
      "sparse_landmarks_page_idx", "sparse_landmark_track_lookup_idx",
      "sparse_landmark_observations_page_idx"};
  for (size_t index = 0; index < sizeof(v16_tables) / sizeof(v16_tables[0]);
       ++index) {
    char schema_query[256];
    (void)snprintf(schema_query, sizeof(schema_query),
                   "SELECT count(*) FROM sqlite_master WHERE type='table' "
                   "AND name='%s'",
                   v16_tables[index]);
    CHECK(query_integer(true_v15_path, schema_query, 0));
  }
  for (size_t index = 0;
       index < sizeof(v16_indexes) / sizeof(v16_indexes[0]); ++index) {
    char schema_query[256];
    (void)snprintf(schema_query, sizeof(schema_query),
                   "SELECT count(*) FROM sqlite_master WHERE type='index' "
                   "AND name='%s'",
                   v16_indexes[index]);
    CHECK(query_integer(true_v15_path, schema_query, 0));
  }
  CHECK(query_integer(true_v15_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' "
                      "AND name='track_builder_tasks'",
                      1));
  CHECK(execute_test_sql(
            true_v15_path,
            "PRAGMA foreign_keys=ON;"
            "INSERT INTO project(singleton,stable_id,name,created_at,updated_at) "
            "VALUES(1,'legacy-project','Legacy',1,1);"
            "INSERT INTO tasks(task_id,name,saved_state,recovery_state,progress,"
            "sequence_count,started_sec,started_nsec,finished_sec,finished_nsec,"
            "updated_at,task_kind,task_kind_version) VALUES(9,'Legacy task',1,0,"
            "100,1,10,20,30,40,2,'track_builder',1);"
            "INSERT INTO checkpoints(task_id,path,format_version,durability,"
            "updated_at) VALUES(9,'legacy.chk',1,0,2);"
            "INSERT INTO artifacts(artifact_id,kind,path,state,size_bytes,"
            "producer_task_id,created_at,updated_at) VALUES('legacy-artifact',"
            "'legacy','legacy.bin',0,0,9,2,2);"
            "INSERT INTO scansets(scanset_id,name,created_at,updated_at) "
            "VALUES(1,'legacy-images',1,1);"
            "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,"
            "created_at) VALUES(1,zeroblob(32),'legacy/image-a',1,1,1);"
            "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,"
            "created_at) VALUES(2,randomblob(32),'legacy/image-b',1,1,1);"
            "INSERT INTO images(image_id,scanset_id,asset_id,original_name,"
            "source_path,producer_task_id,imported_at) VALUES(1,1,1,'a.jpg',"
            "'legacy/a.jpg',9,1);"
            "INSERT INTO images(image_id,scanset_id,asset_id,original_name,"
            "source_path,producer_task_id,imported_at) VALUES(2,1,2,'b.jpg',"
            "'legacy/b.jpg',9,1);"
            "INSERT INTO feature_assets(feature_asset_id,sha256,path,size_bytes,"
            "durability,created_at) VALUES(1,zeroblob(32),'legacy/feature-a',"
            "1,0,1);"
            "INSERT INTO feature_assets(feature_asset_id,sha256,path,size_bytes,"
            "durability,created_at) VALUES(2,randomblob(32),'legacy/feature-b',"
            "1,0,1);"
            "INSERT INTO feature_sets(feature_set_id,image_id,feature_asset_id,"
            "extractor_kind,extractor_version,parameter_fingerprint,"
            "source_image_sha256,feature_count,descriptor_type,descriptor_dimension,"
            "producer_task_id,created_at) VALUES(1,1,1,'orb',1,zeroblob(32),"
            "zeroblob(32),2,1,32,9,1);"
            "INSERT INTO feature_sets(feature_set_id,image_id,feature_asset_id,"
            "extractor_kind,extractor_version,parameter_fingerprint,"
            "source_image_sha256,feature_count,descriptor_type,descriptor_dimension,"
            "producer_task_id,created_at) VALUES(2,2,2,'orb',1,randomblob(32),"
            "randomblob(32),2,1,32,9,1);"
            "INSERT INTO track_sets(track_set_id,builder_kind,builder_version,"
            "parameter_fingerprint,verifier_kind,verifier_version,"
            "verifier_fingerprint,input_scope_hash,gvr_count,track_count,"
            "created_at) VALUES(1,'track_builder',1,zeroblob(32),1,1,"
            "zeroblob(32),zeroblob(32),1,1,1);"
            "INSERT INTO tracks(track_id,track_set_id,observation_count) "
            "VALUES(1,1,2);"
            "INSERT INTO track_observations(track_set_id,track_id,feature_set_id,"
            "feature_index,position_in_track) VALUES(1,1,1,0,0);"
            "INSERT INTO track_observations(track_set_id,track_id,feature_set_id,"
            "feature_index,position_in_track) VALUES(1,1,2,0,1);"
            "INSERT INTO track_builder_tasks(task_id,builder_kind,"
            "builder_version,builder_fingerprint,verifier_kind,"
            "verifier_version,verifier_fingerprint,input_scope_hash,gvr_count,"
            "scope_path,scope_size_bytes,scope_sha256,scope_format_version) "
            "VALUES(9,'track_builder',1,zeroblob(32),1,1,zeroblob(32),"
            "zeroblob(32),1,'.lardon3d/checkpoints/legacy.scope',128,"
            "zeroblob(32),1)"));
  CHECK(query_integer(true_v15_path,
                      "SELECT count(*) FROM track_builder_tasks WHERE task_id=9",
                      1));
  Lardon3DProjectDbResult true_v15_result =
      lardon3d_project_db_open(true_v15_path, &database, error);
  if (true_v15_result != LARDON3D_PROJECT_DB_OK)
    fprintf(stderr, "true v15 upgrade: %d %s\n", true_v15_result, error);
  CHECK(true_v15_result == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  Lardon3DProjectDbProject migrated_project;
  CHECK(lardon3d_project_db_get_project(database, &migrated_project) ==
            LARDON3D_PROJECT_DB_OK &&
        strcmp(migrated_project.stable_id, "legacy-project") == 0);
  Lardon3DProjectDbTask migrated_task;
  CHECK(lardon3d_project_db_load_task(database, 9, &migrated_task) ==
            LARDON3D_PROJECT_DB_OK && migrated_task.has_checkpoint &&
        strcmp(migrated_task.checkpoint.path, "legacy.chk") == 0);
  Lardon3DProjectDbTrackBuilderTask migrated_builder;
  CHECK(lardon3d_project_db_load_track_builder_task(database, 9,
                                                    &migrated_builder) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(migrated_builder.gvr_count == 1 &&
        strcmp(migrated_builder.scope_path,
               ".lardon3d/checkpoints/legacy.scope") == 0);
  CHECK(query_integer(true_v15_path, "SELECT count(*) FROM scansets", 1));
  CHECK(query_integer(true_v15_path, "SELECT count(*) FROM images", 2));
  CHECK(query_integer(true_v15_path, "SELECT count(*) FROM feature_sets", 2));
  CHECK(query_integer(true_v15_path, "SELECT count(*) FROM track_sets", 1));
  CHECK(query_integer(true_v15_path, "SELECT count(*) FROM tracks", 1));
  CHECK(query_integer(true_v15_path, "SELECT count(*) FROM track_observations", 2));
  CHECK(query_integer(
            true_v15_path,
            "SELECT count(*) FROM track_observations AS o JOIN tracks AS t "
            "ON t.track_id=o.track_id WHERE o.track_set_id=t.track_set_id AND "
            "t.track_id=1",
            2));
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(lardon3d_project_db_open(database_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(schema_compare(database_path, true_v15_path, true));
  CHECK(pragma_is_clean(database_path, "PRAGMA foreign_key_check", ""));
  CHECK(pragma_is_clean(true_v15_path, "PRAGMA foreign_key_check", ""));
  CHECK(pragma_is_clean(database_path, "PRAGMA quick_check", "ok"));
  CHECK(pragma_is_clean(true_v15_path, "PRAGMA quick_check", "ok"));
  CHECK(query_integer(database_path,
                     "SELECT count(*) FROM sqlite_master WHERE type IN "
                     "('trigger','view')",
                     0));
  CHECK(query_integer(true_v15_path,
                     "SELECT count(*) FROM sqlite_master WHERE type IN "
                     "('trigger','view')",
                     0));
  CHECK(execute_test_sql(true_v15_path,
                         "DROP INDEX sparse_reconstructions_scope_idx"));
  CHECK(!schema_compare(database_path, true_v15_path, true));

  CHECK(unlink(true_v15_path) == 0);
  CHECK(create_true_v15_database(true_v15_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V16", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(true_v15_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V16") == 0);
  CHECK(query_integer(true_v15_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      15));
  CHECK(query_integer(true_v15_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' "
                      "AND name='sparse_calibrations'",
                      0));
  CHECK(lardon3d_project_db_open(true_v15_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v13_database(failed_v14_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V14", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v14_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V14") == 0);
  CHECK(query_integer(failed_v14_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      13));
  CHECK(
      query_integer(failed_v14_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='track_sets'",
                    0));
  CHECK(lardon3d_project_db_open(failed_v14_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(create_v13_database(failed_v15_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V15", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v15_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V15") == 0);
  CHECK(query_integer(failed_v15_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      13));
  CHECK(
      query_integer(failed_v15_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='track_builder_tasks'",
                    0));
  CHECK(lardon3d_project_db_open(failed_v15_path, &database, error) ==
            LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 17);
  lardon3d_project_db_close(database);
  database = NULL;

  /* Gate C: exercise every public Track API through the Project DB contract. */
  char track_api_path[512];
  CHECK(snprintf(track_api_path, sizeof(track_api_path), "%s/track-api.db",
                 directory) > 0);
  Lardon3DProjectDb *track_database = NULL;
  CHECK(lardon3d_project_db_open(track_api_path, &track_database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet track_scanset;
  CHECK(lardon3d_project_db_create_scanset(track_database, "Track API",
                                           &track_scanset) ==
        LARDON3D_PROJECT_DB_OK);
  unsigned char track_image_hash_a[32] = {31};
  unsigned char track_image_hash_b[32] = {32};
  char track_image_path_a[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  char track_image_path_b[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(track_image_hash_a, track_image_path_a);
  asset_path_for_hash(track_image_hash_b, track_image_path_b);
  Lardon3DProjectDbImageRegisterStatus track_image_status;
  Lardon3DProjectDbImage track_image_a, track_image_b;
  CHECK(lardon3d_project_db_register_image(
            track_database, track_scanset.scanset_id, track_image_hash_a,
            track_image_path_a, 1, "track-a.jpg", "/source/track-a.jpg", 0, 1,
            &track_image_status, &track_image_a) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_register_image(
            track_database, track_scanset.scanset_id, track_image_hash_b,
            track_image_path_b, 1, "track-b.jpg", "/source/track-b.jpg", 0, 2,
            &track_image_status, &track_image_b) == LARDON3D_PROJECT_DB_OK);

  unsigned char feature_parameter[32] = {41};
  unsigned char source_hash_a[32];
  unsigned char source_hash_b[32];
  unsigned char feature_asset_hash_a[32] = {44};
  unsigned char feature_asset_hash_b[32] = {45};
  char feature_asset_path_a[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  char feature_asset_path_b[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  memcpy(source_hash_a, track_image_hash_a, sizeof(source_hash_a));
  memcpy(source_hash_b, track_image_hash_b, sizeof(source_hash_b));
  feature_asset_path_for_hash(feature_asset_hash_a, feature_asset_path_a);
  feature_asset_path_for_hash(feature_asset_hash_b, feature_asset_path_b);
  Lardon3DProjectDbFeatureSet track_features_a, track_features_b;
  CHECK(lardon3d_project_db_register_feature_set(
            track_database, track_image_a.image_id, "orb", 1, feature_parameter,
            source_hash_a, 128, 1, 32, feature_asset_hash_a,
            feature_asset_path_a, 128, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 10,
            &track_features_a) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_register_feature_set(
            track_database, track_image_b.image_id, "orb", 1, feature_parameter,
            source_hash_b, 128, 1, 32, feature_asset_hash_b,
            feature_asset_path_b, 128, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 11,
            &track_features_b) == LARDON3D_PROJECT_DB_OK);

  enum { gate_track_count = 65 };
  Lardon3DProjectDbTrack tracks[gate_track_count];
  Lardon3DProjectDbTrackObservation observations[gate_track_count][2];
  memset(tracks, 0, sizeof(tracks));
  for (size_t index = 0; index < gate_track_count; ++index) {
    observations[index][0] = (Lardon3DProjectDbTrackObservation){
        .feature_set_id = track_features_a.feature_set_id,
        .feature_index = (uint32_t)index,
        .position_in_track = 0};
    observations[index][1] = (Lardon3DProjectDbTrackObservation){
        .feature_set_id = track_features_b.feature_set_id,
        .feature_index = (uint32_t)index,
        .position_in_track = 1};
    tracks[index] = (Lardon3DProjectDbTrack){
        .observation_count = 2, .observations = observations[index]};
  }
  Lardon3DProjectDbTrackSet track_configuration;
  init_track_configuration(&track_configuration, 0x51, 1, gate_track_count);
  Lardon3DProjectDbTrackSet published_tracks;
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &track_configuration, tracks, gate_track_count,
            &published_tracks) == LARDON3D_PROJECT_DB_OK);
  CHECK(published_tracks.track_set_id > 0 &&
        published_tracks.track_count == gate_track_count &&
        published_tracks.gvr_count == 1 &&
        published_tracks.created_at > 1000000000);
  Lardon3DProjectDbTrackSet loaded_track_set, found_track_set;
  CHECK(lardon3d_project_db_load_track_set(
            track_database, published_tracks.track_set_id, &loaded_track_set) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_track_set.track_set_id == published_tracks.track_set_id);
  CHECK(lardon3d_project_db_find_track_set(track_database, &track_configuration,
                                           &found_track_set) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(found_track_set.track_set_id == published_tracks.track_set_id);
  Lardon3DProjectDbTrackSet track_sets_page[64];
  size_t track_set_count = 0;
  CHECK(lardon3d_project_db_list_track_sets(track_database, 0, track_sets_page,
                                            64, &track_set_count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(track_set_count == 1 &&
        track_sets_page[0].track_set_id == published_tracks.track_set_id);

  Lardon3DProjectDbTrack loaded_track, found_track;
  CHECK(lardon3d_project_db_list_tracks(
            track_database, published_tracks.track_set_id, 0, tracks, 64,
            &track_set_count) == LARDON3D_PROJECT_DB_OK);
  CHECK(track_set_count == 64 && tracks[0].track_id > 0);
  uint64_t first_track_id = tracks[0].track_id;
  uint64_t last_first_page_track_id = tracks[track_set_count - 1].track_id;
  for (size_t index = 0; index < track_set_count; ++index) {
    lardon3d_project_db_free_track(&tracks[index]);
  }
  CHECK(lardon3d_project_db_list_tracks(
            track_database, published_tracks.track_set_id,
            last_first_page_track_id, tracks, 64,
            &track_set_count) == LARDON3D_PROJECT_DB_OK);
  CHECK(track_set_count == 1);
  uint64_t second_track_id = tracks[0].track_id;
  CHECK(lardon3d_project_db_load_track(track_database, second_track_id,
                                       &loaded_track) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_track.observation_count == 2);
  lardon3d_project_db_free_track(&loaded_track);
  CHECK(lardon3d_project_db_find_track_by_observation(
            track_database, published_tracks.track_set_id,
            track_features_a.feature_set_id, 1,
            &found_track) == LARDON3D_PROJECT_DB_OK);
  CHECK(found_track.track_id > 0 &&
        found_track.track_set_id == published_tracks.track_set_id);
  lardon3d_project_db_free_track(&found_track);
  lardon3d_project_db_free_track(&tracks[0]);

  Lardon3DProjectDbTrackSet reused_tracks;
  Lardon3DProjectDbTrackSet reuse_configuration = track_configuration;
  reuse_configuration.track_count = 0;
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &reuse_configuration, NULL, 0, &reused_tracks) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(reused_tracks.track_set_id == published_tracks.track_set_id);
  Lardon3DProjectDbTrackSet mismatched_configuration = reuse_configuration;
  mismatched_configuration.gvr_count = 2;
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &mismatched_configuration, NULL, 0,
            &reused_tracks) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(query_integer(track_api_path, "SELECT count(*) FROM track_sets", 1));

  Lardon3DProjectDbTrack invalid_track = {.observation_count = 1,
                                          .observations = observations[0]};
  Lardon3DProjectDbTrackSet invalid_configuration;
  init_track_configuration(&invalid_configuration, 0x52, 1, 1);
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &invalid_configuration, &invalid_track, 1,
            &reused_tracks) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  invalid_track.observation_count = 2;
  observations[0][1].feature_set_id = 999999;
  init_track_configuration(&invalid_configuration, 0x53, 1, 1);
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &invalid_configuration, &invalid_track, 1,
            &reused_tracks) == LARDON3D_PROJECT_DB_NOT_FOUND);
  observations[0][1].feature_set_id = track_features_b.feature_set_id;
  observations[0][1].feature_index = 128;
  init_track_configuration(&invalid_configuration, 0x54, 1, 1);
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &invalid_configuration, &invalid_track, 1,
            &reused_tracks) == LARDON3D_PROJECT_DB_CONSTRAINT);
  observations[0][1].feature_index = 0;
  observations[0][1].feature_set_id = track_features_a.feature_set_id;
  init_track_configuration(&invalid_configuration, 0x55, 1, 1);
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &invalid_configuration, &invalid_track, 1,
            &reused_tracks) == LARDON3D_PROJECT_DB_CONSTRAINT);
  observations[0][1].feature_set_id = track_features_a.feature_set_id;
  observations[0][1].feature_index = 1;
  init_track_configuration(&invalid_configuration, 0x56, 1, 1);
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &invalid_configuration, &invalid_track, 1,
            &reused_tracks) == LARDON3D_PROJECT_DB_CONSTRAINT);
  observations[0][1].feature_set_id = track_features_b.feature_set_id;
  observations[0][1].feature_index = 0;
  observations[0][0].position_in_track = 1;
  init_track_configuration(&invalid_configuration, 0x57, 1, 1);
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &invalid_configuration, &invalid_track, 1,
            &reused_tracks) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  observations[0][0].position_in_track = 0;
  CHECK(query_integer(track_api_path, "SELECT count(*) FROM track_sets", 1));

  Lardon3DProjectDbTrackSet cascade_configuration;
  init_track_configuration(&cascade_configuration, 0x58, 1, 1);
  Lardon3DProjectDbTrack cascade_track = {
      .observation_count = 2,
      .observations = observations[0],
  };
  Lardon3DProjectDbTrackSet cascade_published;
  CHECK(lardon3d_project_db_create_track_set(
            track_database, &cascade_configuration, &cascade_track, 1,
            &cascade_published) == LARDON3D_PROJECT_DB_OK);
  CHECK(query_integer(track_api_path, "SELECT count(*) FROM track_sets", 2));
  sqlite3 *cascade_raw = NULL;
  CHECK(sqlite3_open(track_api_path, &cascade_raw) == SQLITE_OK);
  CHECK(sqlite3_exec(cascade_raw, "PRAGMA foreign_keys=ON;", NULL, NULL,
                     NULL) == SQLITE_OK);
  char delete_cascade_sql[256];
  (void)snprintf(delete_cascade_sql, sizeof(delete_cascade_sql),
                 "DELETE FROM track_sets WHERE track_set_id=%llu;",
                 (unsigned long long)cascade_published.track_set_id);
  CHECK(sqlite3_exec(cascade_raw, delete_cascade_sql, NULL, NULL, NULL) ==
        SQLITE_OK);
  CHECK(sqlite3_close(cascade_raw) == SQLITE_OK);
  char cascade_count_sql[256];
  (void)snprintf(cascade_count_sql, sizeof(cascade_count_sql),
                 "SELECT count(*) FROM track_sets WHERE track_set_id=%llu",
                 (unsigned long long)cascade_published.track_set_id);
  CHECK(query_integer(track_api_path, cascade_count_sql, 0));
  (void)snprintf(cascade_count_sql, sizeof(cascade_count_sql),
                 "SELECT count(*) FROM tracks WHERE track_set_id=%llu",
                 (unsigned long long)cascade_published.track_set_id);
  CHECK(query_integer(track_api_path, cascade_count_sql, 0));
  (void)snprintf(
      cascade_count_sql, sizeof(cascade_count_sql),
      "SELECT count(*) FROM track_observations WHERE track_set_id=%llu",
      (unsigned long long)cascade_published.track_set_id);
  CHECK(query_integer(track_api_path, cascade_count_sql, 0));

  sqlite3 *track_raw = NULL;
  CHECK(sqlite3_open(track_api_path, &track_raw) == SQLITE_OK);
  CHECK(sqlite3_exec(track_raw, "PRAGMA foreign_keys=OFF;", NULL, NULL, NULL) ==
        SQLITE_OK);
  char corruption_sql[512];
  (void)snprintf(corruption_sql, sizeof(corruption_sql),
                 "UPDATE tracks SET track_set_id=999999 WHERE track_id=%llu;",
                 (unsigned long long)first_track_id);
  CHECK(sqlite3_exec(track_raw, corruption_sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(lardon3d_project_db_load_track(track_database, first_track_id,
                                       &loaded_track) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  (void)snprintf(corruption_sql, sizeof(corruption_sql),
                 "UPDATE tracks SET track_set_id=%llu WHERE track_id=%llu;",
                 (unsigned long long)published_tracks.track_set_id,
                 (unsigned long long)first_track_id);
  CHECK(sqlite3_exec(track_raw, corruption_sql, NULL, NULL, NULL) == SQLITE_OK);
  (void)snprintf(
      corruption_sql, sizeof(corruption_sql),
      "UPDATE track_observations SET track_id=999999 WHERE track_id=%llu;",
      (unsigned long long)first_track_id);
  CHECK(sqlite3_exec(track_raw, corruption_sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(lardon3d_project_db_find_track_by_observation(
            track_database, published_tracks.track_set_id,
            track_features_a.feature_set_id, 0,
            &found_track) == LARDON3D_PROJECT_DB_CORRUPT);
  (void)snprintf(
      corruption_sql, sizeof(corruption_sql),
      "UPDATE track_observations SET track_id=%llu WHERE track_id=999999;",
      (unsigned long long)first_track_id);
  CHECK(sqlite3_exec(track_raw, corruption_sql, NULL, NULL, NULL) == SQLITE_OK);
  (void)snprintf(
      corruption_sql, sizeof(corruption_sql),
      "UPDATE track_observations SET feature_set_id=%llu,feature_index=127 "
      "WHERE track_id=%llu AND "
      "position_in_track=1;",
      (unsigned long long)track_features_a.feature_set_id,
      (unsigned long long)first_track_id);
  CHECK(sqlite3_exec(track_raw, corruption_sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(lardon3d_project_db_load_track(track_database, first_track_id,
                                       &loaded_track) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  CHECK(sqlite3_close(track_raw) == SQLITE_OK);
  lardon3d_project_db_close(track_database);
  CHECK(unlink(track_api_path) == 0);

  sqlite3 *raw_db = NULL;
  CHECK(sqlite3_open(database_path, &raw_db) == SQLITE_OK);
  CHECK(sqlite3_exec(raw_db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) ==
        SQLITE_OK);

  sqlite3_stmt *img_check = NULL;
  CHECK(sqlite3_prepare_v2(
            raw_db, "SELECT image_id FROM images ORDER BY image_id LIMIT 1", -1,
            &img_check, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(img_check) == SQLITE_ROW);
  sqlite3_int64 img_id = sqlite3_column_int64(img_check, 0);
  CHECK(img_id > 0);
  (void)sqlite3_finalize(img_check);

  CHECK(sqlite3_exec(
            raw_db,
            "INSERT INTO "
            "feature_assets(sha256,path,size_bytes,durability,created_at) "
            "VALUES(X'"
            "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFE',"
            "'assets/features/ca/fecafeca',128,0,500);",
            NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_int64 fa_id = sqlite3_last_insert_rowid(raw_db);
  CHECK(fa_id > 0);

  char insert_fs[1024];
  (void)snprintf(
      insert_fs, sizeof(insert_fs),
      "INSERT INTO feature_sets(image_id,feature_asset_id,extractor_kind,"
      "extractor_version,parameter_fingerprint,source_image_sha256,feature_"
      "count,"
      "descriptor_type,descriptor_dimension,created_at) "
      "VALUES(%lld,%lld,'orb',1,"
      "X'CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFE',"
      "X'CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFE',"
      "64,1,32,500);",
      (long long)img_id, (long long)fa_id);
  CHECK(sqlite3_exec(raw_db, insert_fs, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_int64 fs_id = sqlite3_last_insert_rowid(raw_db);
  CHECK(fs_id > 0);

  CHECK(
      sqlite3_exec(
          raw_db,
          "INSERT INTO "
          "track_sets(builder_kind,builder_version,parameter_fingerprint,"
          "verifier_kind,verifier_version,verifier_fingerprint,input_scope_"
          "hash,"
          "gvr_count,track_count,created_at) "
          "VALUES('test-builder',1,"
          "X'000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F',"
          "1,1,"
          "X'101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F',"
          "X'202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F',"
          "1,0,200);",
          NULL, NULL, NULL) == SQLITE_OK);

  CHECK(
      sqlite3_exec(
          raw_db,
          "INSERT INTO "
          "track_sets(builder_kind,builder_version,parameter_fingerprint,"
          "verifier_kind,verifier_version,verifier_fingerprint,input_scope_"
          "hash,"
          "gvr_count,track_count,created_at) "
          "VALUES('test-builder',1,"
          "X'000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F',"
          "1,1,"
          "X'101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F',"
          "X'202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F',"
          "1,0,200);",
          NULL, NULL, NULL) == SQLITE_CONSTRAINT);

  CHECK(
      sqlite3_exec(
          raw_db,
          "INSERT INTO "
          "track_sets(builder_kind,builder_version,parameter_fingerprint,"
          "verifier_kind,verifier_version,verifier_fingerprint,input_scope_"
          "hash,"
          "gvr_count,track_count,created_at) "
          "VALUES('inv-builder',0,"
          "X'000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F',"
          "1,1,"
          "X'101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F',"
          "X'202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F',"
          "1,0,200);",
          NULL, NULL, NULL) == SQLITE_CONSTRAINT);

  CHECK(
      sqlite3_exec(
          raw_db,
          "INSERT INTO "
          "track_sets(builder_kind,builder_version,parameter_fingerprint,"
          "verifier_kind,verifier_version,verifier_fingerprint,input_scope_"
          "hash,"
          "gvr_count,track_count,created_at) "
          "VALUES('other-builder',1,"
          "X'000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F',"
          "1,1,"
          "X'101112131415161718191A1B1C1D1E1F202122232425262728292A2B2C2D2E2F',"
          "X'202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F',"
          "1,0,200);",
          NULL, NULL, NULL) == SQLITE_OK);

  CHECK(
      sqlite3_exec(
          raw_db,
          "INSERT INTO "
          "track_sets(builder_kind,builder_version,parameter_fingerprint,"
          "verifier_kind,verifier_version,verifier_fingerprint,input_scope_"
          "hash,"
          "gvr_count,track_count,created_at) "
          "VALUES('gvr-fail',1,"
          "X'303132333435363738393A3B3C3D3E3F404142434445464748494A4B4C4D4E4F',"
          "1,1,"
          "X'404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F',"
          "X'505152535455565758595A5B5C5D5E5F606162636465666768696A6B6C6D6E6F',"
          "0,0,200);",
          NULL, NULL, NULL) == SQLITE_CONSTRAINT);

  CHECK(sqlite3_exec(raw_db,
                     "INSERT INTO tracks(track_set_id,observation_count) "
                     "VALUES(99999,2);",
                     NULL, NULL, NULL) == SQLITE_CONSTRAINT);

  CHECK(sqlite3_exec(raw_db,
                     "INSERT INTO tracks(track_set_id,observation_count) "
                     "VALUES(1,1);",
                     NULL, NULL, NULL) == SQLITE_CONSTRAINT);

  CHECK(sqlite3_exec(raw_db,
                     "INSERT INTO tracks(track_set_id,observation_count) "
                     "VALUES(1,2);",
                     NULL, NULL, NULL) == SQLITE_OK);

  char insert_obs[512];
  (void)snprintf(
      insert_obs, sizeof(insert_obs),
      "INSERT INTO track_observations(track_set_id,track_id,feature_set_id,"
      "feature_index,position_in_track) "
      "VALUES(1,1,%lld,0,0);",
      (long long)(fs_id + 999));
  CHECK(sqlite3_exec(raw_db, insert_obs, NULL, NULL, NULL) ==
        SQLITE_CONSTRAINT);

  (void)snprintf(
      insert_obs, sizeof(insert_obs),
      "INSERT INTO track_observations(track_set_id,track_id,feature_set_id,"
      "feature_index,position_in_track) "
      "VALUES(1,1,%lld,0,0);",
      (long long)fs_id);
  CHECK(sqlite3_exec(raw_db, insert_obs, NULL, NULL, NULL) == SQLITE_OK);

  CHECK(sqlite3_exec(raw_db, insert_obs, NULL, NULL, NULL) ==
        SQLITE_CONSTRAINT);

  CHECK(sqlite3_exec(raw_db, "DELETE FROM track_sets WHERE track_set_id=2;",
                     NULL, NULL, NULL) == SQLITE_OK);

  sqlite3_stmt *cascade_check = NULL;
  CHECK(sqlite3_prepare_v2(raw_db,
                           "SELECT count(*) FROM tracks WHERE track_set_id=2",
                           -1, &cascade_check, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(cascade_check) == SQLITE_ROW &&
        sqlite3_column_int(cascade_check, 0) == 0);
  (void)sqlite3_finalize(cascade_check);
  cascade_check = NULL;
  CHECK(sqlite3_prepare_v2(
            raw_db,
            "SELECT count(*) FROM track_observations WHERE track_set_id=2", -1,
            &cascade_check, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(cascade_check) == SQLITE_ROW &&
        sqlite3_column_int(cascade_check, 0) == 0);
  (void)sqlite3_finalize(cascade_check);
  cascade_check = NULL;

  CHECK(sqlite3_close(raw_db) == SQLITE_OK);

  CHECK(unlink(artifact_path) == 0);
  CHECK(unlink(database_path) == 0);
  CHECK(unlink(future_path) == 0);
  CHECK(unlink(corrupt_path) == 0);
  CHECK(unlink(legacy_path) == 0);
  CHECK(unlink(failed_migration_path) == 0);
  CHECK(unlink(v2_path) == 0);
  CHECK(unlink(failed_v3_migration_path) == 0);
  CHECK(unlink(v3_path) == 0);
  CHECK(unlink(failed_v4_path) == 0);
  CHECK(unlink(v4_path) == 0);
  CHECK(unlink(failed_v5_path) == 0);
  CHECK(unlink(failed_v6_path) == 0);
  CHECK(unlink(failed_v7_path) == 0);
  CHECK(unlink(direct_v5_path) == 0);
  CHECK(unlink(v8_path) == 0);
  CHECK(unlink(failed_v8_path) == 0);
  CHECK(unlink(v10_path) == 0);
  CHECK(unlink(failed_v11_path) == 0);
  CHECK(unlink(v13_path) == 0);
  CHECK(unlink(failed_v14_path) == 0);
  CHECK(unlink(true_v14_path) == 0);
  CHECK(unlink(true_v15_path) == 0);
  CHECK(unlink(failed_v15_path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
