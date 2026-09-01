#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/optical_profiles.h>
#include <lardon3d/sparse_sfm_model.h>

#include "project_db_internal.h"

static bool optical_id(uint64_t value) {
  return value > 0 && value <= (uint64_t)INT64_MAX;
}

static bool optical_cursor_id(uint64_t value) {
  return value <= (uint64_t)INT64_MAX;
}

static bool optical_text(const char *value, size_t capacity, bool allow_empty) {
  if (!value)
    return false;
  size_t length = strnlen(value, capacity);
  return length < capacity && (allow_empty || length > 0);
}

static bool optical_copy_text(sqlite3_stmt *statement, int column,
                              char *output, size_t capacity,
                              bool allow_empty) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return false;
  int bytes = sqlite3_column_bytes(statement, column);
  const unsigned char *text = sqlite3_column_text(statement, column);
  if (!text || bytes < 0 || (size_t)bytes >= capacity ||
      (!allow_empty && bytes == 0) || memchr(text, '\0', (size_t)bytes))
    return false;
  memcpy(output, text, (size_t)bytes);
  output[bytes] = '\0';
  return true;
}

static bool optical_column_equals(sqlite3_stmt *statement, int column,
                                  const char *expected) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return false;
  int bytes = sqlite3_column_bytes(statement, column);
  size_t expected_size = strlen(expected);
  const void *text = sqlite3_column_text(statement, column);
  return bytes >= 0 && (size_t)bytes == expected_size && text &&
         memcmp(text, expected, expected_size) == 0;
}

static Lardon3DProjectDbResult optical_commit_or_rollback(
    Lardon3DProjectDb *database, Lardon3DProjectDbResult result,
    const char *commit_context, const char *rollback_context) {
  if (result == LARDON3D_PROJECT_DB_OK)
    result = execute(database, "COMMIT", commit_context);
  if (result != LARDON3D_PROJECT_DB_OK)
    (void)execute(database, "ROLLBACK", rollback_context);
  return result;
}

static bool optical_page_arguments(uint64_t after_id, const void *items,
                                   size_t capacity, const size_t *count,
                                   const uint64_t *next_after_id) {
  return optical_cursor_id(after_id) && items && capacity > 0 &&
         capacity <= LARDON3D_OPTICAL_PAGE_MAX && count && next_after_id;
}

static bool read_camera_body(sqlite3_stmt *statement,
                             Lardon3DOpticalCameraBodyProfile *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_int64 id = sqlite3_column_int64(statement, 0);
  if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || id <= 0 ||
      !optical_copy_text(statement, 1, output->manufacturer,
                         sizeof(output->manufacturer), false) ||
      !optical_copy_text(statement, 2, output->model, sizeof(output->model),
                         false) ||
      !optical_copy_text(statement, 3, output->name, sizeof(output->name),
                         false)) {
    memset(output, 0, sizeof(*output));
    return false;
  }
  output->camera_body_profile_id = (uint64_t)id;
  return true;
}

static Lardon3DProjectDbResult camera_body_load_locked(
    Lardon3DProjectDb *database, uint64_t profile_id,
    Lardon3DOpticalCameraBodyProfile *output) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT camera_body_profile_id,manufacturer,model,name FROM "
      "camera_body_profiles WHERE camera_body_profile_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)profile_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load camera body profile");
    else if (!read_camera_body(statement, output) ||
             output->camera_body_profile_id != profile_id ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_camera_body_create(
    Lardon3DProjectDb *database, const Lardon3DOpticalCameraBodyProfile *input,
    Lardon3DOpticalCameraBodyProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !input || !output || input == output ||
      input->camera_body_profile_id != 0 ||
      !optical_text(input->manufacturer, sizeof(input->manufacturer), false) ||
      !optical_text(input->model, sizeof(input->model), false) ||
      !optical_text(input->name, sizeof(input->name), false))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;

  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin camera body profile");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "INSERT OR IGNORE INTO camera_body_profiles(manufacturer,model,name) "
        "VALUES(?1,?2,?3)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, input->manufacturer, -1,
                           SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, input->model, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, input->name, -1, SQLITE_TRANSIENT);
    result = step_done(database, statement, "insert camera body profile");
    statement = NULL;
  }
  uint64_t profile_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT camera_body_profile_id FROM camera_body_profiles WHERE "
        "manufacturer=?1 AND model=?2 AND name=?3",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, input->manufacturer, -1,
                           SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, input->model, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, input->name, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else if (code != SQLITE_ROW) {
      result = sqlite_result(database, code, "find camera body profile");
    } else if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
               sqlite3_column_int64(statement, 0) <= 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      sqlite3_int64 stored_id = sqlite3_column_int64(statement, 0);
      profile_id = (uint64_t)stored_id;
      if (sqlite3_step(statement) != SQLITE_DONE)
        result = LARDON3D_PROJECT_DB_CORRUPT;
    }
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result,
                                      "commit camera body profile",
                                      "rollback camera body profile");
  if (result == LARDON3D_PROJECT_DB_OK)
    result = camera_body_load_locked(database, profile_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_camera_body_load(
    Lardon3DProjectDb *database, uint64_t camera_body_profile_id,
    Lardon3DOpticalCameraBodyProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(camera_body_profile_id) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      camera_body_load_locked(database, camera_body_profile_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_camera_body_list(
    Lardon3DProjectDb *database, uint64_t after_profile_id,
    Lardon3DOpticalCameraBodyProfile *items, size_t capacity, size_t *count,
    uint64_t *next_after_profile_id) {
  if (count)
    *count = 0;
  if (next_after_profile_id)
    *next_after_profile_id = after_profile_id;
  if (!database || !optical_page_arguments(after_profile_id, items, capacity,
                                             count, next_after_profile_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  memset(items, 0, capacity * sizeof(*items));
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT camera_body_profile_id,manufacturer,model,name FROM "
      "camera_body_profiles WHERE camera_body_profile_id>?1 ORDER BY "
      "camera_body_profile_id LIMIT ?2",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)after_profile_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_camera_body(statement, &items[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      *next_after_profile_id = items[*count].camera_body_profile_id;
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE &&
        *count < capacity)
      result = sqlite_result(database, code, "list camera body profiles");
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    memset(items, 0, capacity * sizeof(*items));
    *count = 0;
    *next_after_profile_id = after_profile_id;
  }
  return result;
}

static bool read_camera_alias(sqlite3_stmt *statement,
                              Lardon3DOpticalCameraBodyAlias *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_int64 alias_id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 profile_id = sqlite3_column_int64(statement, 1);
  if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || alias_id <= 0 ||
      sqlite3_column_type(statement, 1) != SQLITE_INTEGER || profile_id <= 0 ||
      !optical_copy_text(statement, 2, output->metadata_make,
                         sizeof(output->metadata_make), false) ||
      !optical_copy_text(statement, 3, output->metadata_model,
                         sizeof(output->metadata_model), false)) {
    memset(output, 0, sizeof(*output));
    return false;
  }
  output->alias_id = (uint64_t)alias_id;
  output->camera_body_profile_id = (uint64_t)profile_id;
  return true;
}

Lardon3DProjectDbResult lardon3d_optical_camera_body_alias_add(
    Lardon3DProjectDb *database, uint64_t camera_body_profile_id,
    const char *metadata_make, const char *metadata_model,
    Lardon3DOpticalCameraBodyAlias *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(camera_body_profile_id) ||
      !optical_text(metadata_make, LARDON3D_OPTICAL_TEXT_CAPACITY, false) ||
      !optical_text(metadata_model, LARDON3D_OPTICAL_TEXT_CAPACITY, false) ||
      !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin camera body alias");
  Lardon3DOpticalCameraBodyProfile body;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = camera_body_load_locked(database, camera_body_profile_id, &body);
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "INSERT OR IGNORE INTO camera_body_aliases(camera_body_profile_id,"
        "metadata_make,metadata_model) VALUES(?1,?2,?3)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)camera_body_profile_id);
    (void)sqlite3_bind_text(statement, 2, metadata_make, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, metadata_model, -1, SQLITE_TRANSIENT);
    result = step_done(database, statement, "insert camera body alias");
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT alias_id,camera_body_profile_id,metadata_make,metadata_model "
        "FROM camera_body_aliases WHERE metadata_make=?1 AND metadata_model=?2",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, metadata_make, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, metadata_model, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "find camera body alias");
    else if (!read_camera_alias(statement, output))
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else if (output->camera_body_profile_id != camera_body_profile_id)
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    else if (sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result,
                                      "commit camera body alias",
                                      "rollback camera body alias");
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_camera_body_find_exact_alias(
    Lardon3DProjectDb *database, const char *metadata_make,
    const char *metadata_model, Lardon3DOpticalCameraBodyProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database ||
      !optical_text(metadata_make, LARDON3D_OPTICAL_TEXT_CAPACITY, false) ||
      !optical_text(metadata_model, LARDON3D_OPTICAL_TEXT_CAPACITY, false) ||
      !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT b.camera_body_profile_id,b.manufacturer,b.model,b.name,a.alias_id "
      "FROM camera_body_aliases a LEFT JOIN camera_body_profiles b ON "
      "b.camera_body_profile_id=a.camera_body_profile_id WHERE "
      "a.metadata_make=?1 AND a.metadata_model=?2",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, metadata_make, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, metadata_model, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "find exact camera body alias");
    else if (!read_camera_body(statement, output) ||
             sqlite3_column_type(statement, 4) != SQLITE_INTEGER ||
             sqlite3_column_int64(statement, 4) <= 0 ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_camera_body_alias_list(
    Lardon3DProjectDb *database, uint64_t camera_body_profile_id,
    uint64_t after_alias_id, Lardon3DOpticalCameraBodyAlias *items,
    size_t capacity, size_t *count, uint64_t *next_after_alias_id) {
  if (count)
    *count = 0;
  if (next_after_alias_id)
    *next_after_alias_id = after_alias_id;
  if (!database || !optical_id(camera_body_profile_id) ||
      !optical_page_arguments(after_alias_id, items, capacity, count,
                              next_after_alias_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  memset(items, 0, capacity * sizeof(*items));
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DOpticalCameraBodyProfile body;
  Lardon3DProjectDbResult result =
      camera_body_load_locked(database, camera_body_profile_id, &body);
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT alias_id,camera_body_profile_id,metadata_make,metadata_model "
        "FROM camera_body_aliases WHERE camera_body_profile_id=?1 AND "
        "alias_id>?2 ORDER BY alias_id LIMIT ?3",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)camera_body_profile_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_alias_id);
    (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_camera_alias(statement, &items[*count]) ||
          items[*count].camera_body_profile_id != camera_body_profile_id) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      *next_after_alias_id = items[*count].alias_id;
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE &&
        *count < capacity)
      result = sqlite_result(database, code, "list camera body aliases");
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    memset(items, 0, capacity * sizeof(*items));
    *count = 0;
    *next_after_alias_id = after_alias_id;
  }
  return result;
}

static bool lens_values_valid(Lardon3DOpticalLensInterface interface_kind,
                              Lardon3DOpticalFocalRangeKind range_kind,
                              uint32_t minimum_focal_um,
                              uint32_t maximum_focal_um) {
  if (interface_kind < LARDON3D_OPTICAL_LENS_MANUAL ||
      interface_kind > LARDON3D_OPTICAL_LENS_INTEGRATED)
    return false;
  if (range_kind == LARDON3D_OPTICAL_FOCAL_RANGE_UNKNOWN)
    return minimum_focal_um == 0 && maximum_focal_um == 0;
  if (range_kind == LARDON3D_OPTICAL_FOCAL_RANGE_PRIME)
    return minimum_focal_um > 0 && minimum_focal_um == maximum_focal_um;
  if (range_kind == LARDON3D_OPTICAL_FOCAL_RANGE_ZOOM)
    return minimum_focal_um > 0 && minimum_focal_um < maximum_focal_um;
  return false;
}

static bool read_lens(sqlite3_stmt *statement,
                      Lardon3DOpticalLensProfile *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_int64 id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 interface_kind = sqlite3_column_int64(statement, 4);
  sqlite3_int64 range_kind = sqlite3_column_int64(statement, 5);
  sqlite3_int64 minimum_focal = sqlite3_column_int64(statement, 6);
  sqlite3_int64 maximum_focal = sqlite3_column_int64(statement, 7);
  if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || id <= 0 ||
      !optical_copy_text(statement, 1, output->manufacturer,
                         sizeof(output->manufacturer), true) ||
      !optical_copy_text(statement, 2, output->model, sizeof(output->model),
                         true) ||
      !optical_copy_text(statement, 3, output->name, sizeof(output->name),
                         false) ||
      sqlite3_column_type(statement, 4) != SQLITE_INTEGER ||
      sqlite3_column_type(statement, 5) != SQLITE_INTEGER ||
      sqlite3_column_type(statement, 6) != SQLITE_INTEGER ||
      sqlite3_column_type(statement, 7) != SQLITE_INTEGER ||
      interface_kind < 1 || interface_kind > 3 || range_kind < 1 ||
      range_kind > 3 || minimum_focal < 0 || minimum_focal > UINT32_MAX ||
      maximum_focal < 0 || maximum_focal > UINT32_MAX ||
      !lens_values_valid((Lardon3DOpticalLensInterface)interface_kind,
                         (Lardon3DOpticalFocalRangeKind)range_kind,
                         (uint32_t)minimum_focal,
                         (uint32_t)maximum_focal)) {
    memset(output, 0, sizeof(*output));
    return false;
  }
  output->lens_profile_id = (uint64_t)id;
  output->interface_kind = (Lardon3DOpticalLensInterface)interface_kind;
  output->focal_range_kind = (Lardon3DOpticalFocalRangeKind)range_kind;
  output->minimum_focal_um = (uint32_t)minimum_focal;
  output->maximum_focal_um = (uint32_t)maximum_focal;
  return true;
}

static Lardon3DProjectDbResult lens_load_locked(
    Lardon3DProjectDb *database, uint64_t profile_id,
    Lardon3DOpticalLensProfile *output) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT lens_profile_id,manufacturer,model,name,interface_kind,"
      "focal_range_kind,minimum_focal_um,maximum_focal_um FROM lens_profiles "
      "WHERE lens_profile_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)profile_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load lens profile");
    else if (!read_lens(statement, output) ||
             output->lens_profile_id != profile_id ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_lens_create(
    Lardon3DProjectDb *database, const Lardon3DOpticalLensProfile *input,
    Lardon3DOpticalLensProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !input || !output || input == output ||
      input->lens_profile_id != 0 ||
      !optical_text(input->manufacturer, sizeof(input->manufacturer), true) ||
      !optical_text(input->model, sizeof(input->model), true) ||
      !optical_text(input->name, sizeof(input->name), false) ||
      !lens_values_valid(input->interface_kind, input->focal_range_kind,
                         input->minimum_focal_um, input->maximum_focal_um))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin lens profile");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "INSERT OR IGNORE INTO lens_profiles(manufacturer,model,name,"
        "interface_kind,focal_range_kind,minimum_focal_um,maximum_focal_um) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, input->manufacturer, -1,
                           SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, input->model, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, input->name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 4, input->interface_kind);
    (void)sqlite3_bind_int64(statement, 5, input->focal_range_kind);
    (void)sqlite3_bind_int64(statement, 6, input->minimum_focal_um);
    (void)sqlite3_bind_int64(statement, 7, input->maximum_focal_um);
    result = step_done(database, statement, "insert lens profile");
    statement = NULL;
  }
  uint64_t profile_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT lens_profile_id,interface_kind,focal_range_kind,minimum_focal_um,"
        "maximum_focal_um FROM lens_profiles WHERE manufacturer=?1 AND model=?2 "
        "AND name=?3",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, input->manufacturer, -1,
                           SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, input->model, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, input->name, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else if (code != SQLITE_ROW) {
      result = sqlite_result(database, code, "find lens profile");
    } else if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
               sqlite3_column_int64(statement, 0) <= 0 ||
               sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
               sqlite3_column_type(statement, 2) != SQLITE_INTEGER ||
               sqlite3_column_type(statement, 3) != SQLITE_INTEGER ||
               sqlite3_column_type(statement, 4) != SQLITE_INTEGER) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else if (sqlite3_column_int64(statement, 1) != input->interface_kind ||
               sqlite3_column_int64(statement, 2) != input->focal_range_kind ||
               sqlite3_column_int64(statement, 3) != input->minimum_focal_um ||
               sqlite3_column_int64(statement, 4) != input->maximum_focal_um) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    } else {
      profile_id = (uint64_t)sqlite3_column_int64(statement, 0);
      if (sqlite3_step(statement) != SQLITE_DONE)
        result = LARDON3D_PROJECT_DB_CORRUPT;
    }
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result, "commit lens profile",
                                      "rollback lens profile");
  if (result == LARDON3D_PROJECT_DB_OK)
    result = lens_load_locked(database, profile_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_lens_load(
    Lardon3DProjectDb *database, uint64_t lens_profile_id,
    Lardon3DOpticalLensProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(lens_profile_id) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      lens_load_locked(database, lens_profile_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_lens_list(
    Lardon3DProjectDb *database, uint64_t after_profile_id,
    Lardon3DOpticalLensProfile *items, size_t capacity, size_t *count,
    uint64_t *next_after_profile_id) {
  if (count)
    *count = 0;
  if (next_after_profile_id)
    *next_after_profile_id = after_profile_id;
  if (!database || !optical_page_arguments(after_profile_id, items, capacity,
                                             count, next_after_profile_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  memset(items, 0, capacity * sizeof(*items));
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT lens_profile_id,manufacturer,model,name,interface_kind,"
      "focal_range_kind,minimum_focal_um,maximum_focal_um FROM lens_profiles "
      "WHERE lens_profile_id>?1 ORDER BY lens_profile_id LIMIT ?2",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)after_profile_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_lens(statement, &items[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      *next_after_profile_id = items[*count].lens_profile_id;
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE &&
        *count < capacity)
      result = sqlite_result(database, code, "list lens profiles");
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    memset(items, 0, capacity * sizeof(*items));
    *count = 0;
    *next_after_profile_id = after_profile_id;
  }
  return result;
}

static bool read_lens_alias(sqlite3_stmt *statement,
                            Lardon3DOpticalLensAlias *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_int64 alias_id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 profile_id = sqlite3_column_int64(statement, 1);
  if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || alias_id <= 0 ||
      sqlite3_column_type(statement, 1) != SQLITE_INTEGER || profile_id <= 0 ||
      !optical_copy_text(statement, 2, output->metadata_make,
                         sizeof(output->metadata_make), true) ||
      !optical_copy_text(statement, 3, output->metadata_model,
                         sizeof(output->metadata_model), false)) {
    memset(output, 0, sizeof(*output));
    return false;
  }
  output->alias_id = (uint64_t)alias_id;
  output->lens_profile_id = (uint64_t)profile_id;
  return true;
}

Lardon3DProjectDbResult lardon3d_optical_lens_alias_add(
    Lardon3DProjectDb *database, uint64_t lens_profile_id,
    const char *metadata_make, const char *metadata_model,
    Lardon3DOpticalLensAlias *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(lens_profile_id) ||
      !optical_text(metadata_make, LARDON3D_OPTICAL_TEXT_CAPACITY, true) ||
      !optical_text(metadata_model, LARDON3D_OPTICAL_TEXT_CAPACITY, false) ||
      !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin lens alias");
  Lardon3DOpticalLensProfile lens;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = lens_load_locked(database, lens_profile_id, &lens);
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "INSERT OR IGNORE INTO lens_profile_aliases(lens_profile_id,"
        "metadata_make,metadata_model) VALUES(?1,?2,?3)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)lens_profile_id);
    (void)sqlite3_bind_text(statement, 2, metadata_make, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 3, metadata_model, -1, SQLITE_TRANSIENT);
    result = step_done(database, statement, "insert lens alias");
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT alias_id,lens_profile_id,metadata_make,metadata_model FROM "
        "lens_profile_aliases WHERE metadata_make=?1 AND metadata_model=?2",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, metadata_make, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, metadata_model, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "find lens alias");
    else if (!read_lens_alias(statement, output))
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else if (output->lens_profile_id != lens_profile_id)
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    else if (sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result, "commit lens alias",
                                      "rollback lens alias");
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_lens_find_exact_alias(
    Lardon3DProjectDb *database, const char *metadata_make,
    const char *metadata_model, Lardon3DOpticalLensProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database ||
      !optical_text(metadata_make, LARDON3D_OPTICAL_TEXT_CAPACITY, true) ||
      !optical_text(metadata_model, LARDON3D_OPTICAL_TEXT_CAPACITY, false) ||
      !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT l.lens_profile_id,l.manufacturer,l.model,l.name,l.interface_kind,"
      "l.focal_range_kind,l.minimum_focal_um,l.maximum_focal_um,a.alias_id FROM "
      "lens_profile_aliases a LEFT JOIN lens_profiles l ON l.lens_profile_id="
      "a.lens_profile_id WHERE a.metadata_make=?1 AND a.metadata_model=?2",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_text(statement, 1, metadata_make, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_text(statement, 2, metadata_model, -1, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "find exact lens alias");
    else if (!read_lens(statement, output) ||
             sqlite3_column_type(statement, 8) != SQLITE_INTEGER ||
             sqlite3_column_int64(statement, 8) <= 0 ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_lens_alias_list(
    Lardon3DProjectDb *database, uint64_t lens_profile_id,
    uint64_t after_alias_id, Lardon3DOpticalLensAlias *items, size_t capacity,
    size_t *count, uint64_t *next_after_alias_id) {
  if (count)
    *count = 0;
  if (next_after_alias_id)
    *next_after_alias_id = after_alias_id;
  if (!database || !optical_id(lens_profile_id) ||
      !optical_page_arguments(after_alias_id, items, capacity, count,
                              next_after_alias_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  memset(items, 0, capacity * sizeof(*items));
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DOpticalLensProfile lens;
  Lardon3DProjectDbResult result =
      lens_load_locked(database, lens_profile_id, &lens);
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT alias_id,lens_profile_id,metadata_make,metadata_model FROM "
        "lens_profile_aliases WHERE lens_profile_id=?1 AND alias_id>?2 ORDER BY "
        "alias_id LIMIT ?3",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)lens_profile_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_alias_id);
    (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_lens_alias(statement, &items[*count]) ||
          items[*count].lens_profile_id != lens_profile_id) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      *next_after_alias_id = items[*count].alias_id;
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE &&
        *count < capacity)
      result = sqlite_result(database, code, "list lens aliases");
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    memset(items, 0, capacity * sizeof(*items));
    *count = 0;
    *next_after_alias_id = after_alias_id;
  }
  return result;
}

static bool focal_valid_for_lens(const Lardon3DOpticalLensProfile *lens,
                                 uint32_t focal_length_um) {
  if (focal_length_um == 0)
    return true;
  if (lens->focal_range_kind == LARDON3D_OPTICAL_FOCAL_RANGE_UNKNOWN)
    return true;
  if (lens->focal_range_kind == LARDON3D_OPTICAL_FOCAL_RANGE_PRIME)
    return focal_length_um == lens->minimum_focal_um;
  return focal_length_um >= lens->minimum_focal_um &&
         focal_length_um <= lens->maximum_focal_um;
}

static bool read_configuration(sqlite3_stmt *statement,
                               Lardon3DOpticalConfiguration *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_int64 configuration_id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 body_id = sqlite3_column_int64(statement, 1);
  sqlite3_int64 lens_id = sqlite3_column_int64(statement, 2);
  sqlite3_int64 focal = sqlite3_column_int64(statement, 3);
  sqlite3_int64 joined_body_id = sqlite3_column_int64(statement, 4);
  sqlite3_int64 joined_lens_id = sqlite3_column_int64(statement, 5);
  sqlite3_int64 interface_kind = sqlite3_column_int64(statement, 6);
  sqlite3_int64 range_kind = sqlite3_column_int64(statement, 7);
  sqlite3_int64 minimum_focal = sqlite3_column_int64(statement, 8);
  sqlite3_int64 maximum_focal = sqlite3_column_int64(statement, 9);
  for (int column = 0; column < 10; ++column)
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER)
      return false;
  if (configuration_id <= 0 || body_id <= 0 || lens_id <= 0 ||
      joined_body_id != body_id || joined_lens_id != lens_id || focal < 0 ||
      focal > UINT32_MAX || interface_kind < 1 || interface_kind > 3 ||
      range_kind < 1 || range_kind > 3 || minimum_focal < 0 ||
      minimum_focal > UINT32_MAX || maximum_focal < 0 ||
      maximum_focal > UINT32_MAX)
    return false;
  Lardon3DOpticalLensProfile lens = {
      .interface_kind = (Lardon3DOpticalLensInterface)interface_kind,
      .focal_range_kind = (Lardon3DOpticalFocalRangeKind)range_kind,
      .minimum_focal_um = (uint32_t)minimum_focal,
      .maximum_focal_um = (uint32_t)maximum_focal,
  };
  if (!lens_values_valid(lens.interface_kind, lens.focal_range_kind,
                         lens.minimum_focal_um, lens.maximum_focal_um) ||
      !focal_valid_for_lens(&lens, (uint32_t)focal))
    return false;
  output->optical_configuration_id = (uint64_t)configuration_id;
  output->camera_body_profile_id = (uint64_t)body_id;
  output->lens_profile_id = (uint64_t)lens_id;
  output->has_focal_length = focal != 0;
  output->focal_length_um = (uint32_t)focal;
  return true;
}

static const char configuration_select[] =
    "SELECT o.optical_configuration_id,o.camera_body_profile_id,"
    "o.lens_profile_id,o.focal_length_um,b.camera_body_profile_id,"
    "l.lens_profile_id,l.interface_kind,l.focal_range_kind,"
    "l.minimum_focal_um,l.maximum_focal_um FROM optical_configurations o "
    "LEFT JOIN camera_body_profiles b ON b.camera_body_profile_id="
    "o.camera_body_profile_id LEFT JOIN lens_profiles l ON l.lens_profile_id="
    "o.lens_profile_id ";

static Lardon3DProjectDbResult configuration_dependencies_locked(
    Lardon3DProjectDb *database,
    const Lardon3DOpticalConfiguration *configuration) {
  Lardon3DOpticalCameraBodyProfile body = {0};
  Lardon3DOpticalLensProfile lens = {0};
  Lardon3DProjectDbResult result = camera_body_load_locked(
      database, configuration->camera_body_profile_id, &body);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = lens_load_locked(database, configuration->lens_profile_id, &lens);
  /* Foreign keys prevent ordinary orphaning, but loaders must still reject a
     database altered with constraint enforcement disabled. A surviving ID is
     not enough when the referenced typed profile itself is malformed. */
  if (result == LARDON3D_PROJECT_DB_NOT_FOUND)
    result = LARDON3D_PROJECT_DB_CORRUPT;
  return result;
}

static Lardon3DProjectDbResult configuration_load_locked(
    Lardon3DProjectDb *database, uint64_t configuration_id,
    Lardon3DOpticalConfiguration *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_stmt *statement = NULL;
  char query[1024];
  int length = snprintf(query, sizeof(query), "%s WHERE o.optical_configuration_id=?1",
                        configuration_select);
  if (length < 0 || (size_t)length >= sizeof(query))
    return LARDON3D_PROJECT_DB_IO_ERROR;
  Lardon3DProjectDbResult result = prepare(database, query, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)configuration_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load optical configuration");
    else if (!read_configuration(statement, output) ||
             output->optical_configuration_id != configuration_id ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = configuration_dependencies_locked(database, output);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_configuration_create(
    Lardon3DProjectDb *database, const Lardon3DOpticalConfiguration *input,
    Lardon3DOpticalConfiguration *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !input || !output || input == output ||
      input->optical_configuration_id != 0 ||
      !optical_id(input->camera_body_profile_id) ||
      !optical_id(input->lens_profile_id) ||
      (input->has_focal_length && input->focal_length_um == 0) ||
      (!input->has_focal_length && input->focal_length_um != 0))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin optical configuration");
  Lardon3DOpticalCameraBodyProfile body = {0};
  Lardon3DOpticalLensProfile lens = {0};
  if (result == LARDON3D_PROJECT_DB_OK)
    result = camera_body_load_locked(database, input->camera_body_profile_id,
                                     &body);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = lens_load_locked(database, input->lens_profile_id, &lens);
  if (result == LARDON3D_PROJECT_DB_OK &&
      !focal_valid_for_lens(&lens, input->focal_length_um))
    result = LARDON3D_PROJECT_DB_CONSTRAINT;
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "INSERT OR IGNORE INTO optical_configurations(camera_body_profile_id,"
        "lens_profile_id,focal_length_um) VALUES(?1,?2,?3)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)input->camera_body_profile_id);
    (void)sqlite3_bind_int64(statement, 2,
                             (sqlite3_int64)input->lens_profile_id);
    (void)sqlite3_bind_int64(statement, 3, input->focal_length_um);
    result = step_done(database, statement, "insert optical configuration");
    statement = NULL;
  }
  uint64_t configuration_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT optical_configuration_id FROM optical_configurations WHERE "
        "camera_body_profile_id=?1 AND lens_profile_id=?2 AND focal_length_um=?3",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)input->camera_body_profile_id);
    (void)sqlite3_bind_int64(statement, 2,
                             (sqlite3_int64)input->lens_profile_id);
    (void)sqlite3_bind_int64(statement, 3, input->focal_length_um);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else if (code != SQLITE_ROW) {
      result = sqlite_result(database, code, "find optical configuration");
    } else if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
               sqlite3_column_int64(statement, 0) <= 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else {
      configuration_id = (uint64_t)sqlite3_column_int64(statement, 0);
      if (sqlite3_step(statement) != SQLITE_DONE)
        result = LARDON3D_PROJECT_DB_CORRUPT;
    }
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result,
                                      "commit optical configuration",
                                      "rollback optical configuration");
  if (result == LARDON3D_PROJECT_DB_OK)
    result = configuration_load_locked(database, configuration_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_configuration_load(
    Lardon3DProjectDb *database, uint64_t optical_configuration_id,
    Lardon3DOpticalConfiguration *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(optical_configuration_id) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result = configuration_load_locked(
      database, optical_configuration_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_configuration_list(
    Lardon3DProjectDb *database, uint64_t after_configuration_id,
    Lardon3DOpticalConfiguration *items, size_t capacity, size_t *count,
    uint64_t *next_after_configuration_id) {
  if (count)
    *count = 0;
  if (next_after_configuration_id)
    *next_after_configuration_id = after_configuration_id;
  if (!database ||
      !optical_page_arguments(after_configuration_id, items, capacity, count,
                              next_after_configuration_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  memset(items, 0, capacity * sizeof(*items));
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  char query[1024];
  int length = snprintf(query, sizeof(query),
                        "%s WHERE o.optical_configuration_id>?1 ORDER BY "
                        "o.optical_configuration_id LIMIT ?2",
                        configuration_select);
  Lardon3DProjectDbResult result =
      length < 0 || (size_t)length >= sizeof(query)
          ? LARDON3D_PROJECT_DB_IO_ERROR
          : prepare(database, query, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)after_configuration_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_configuration(statement, &items[*count])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      result = configuration_dependencies_locked(database, &items[*count]);
      if (result != LARDON3D_PROJECT_DB_OK)
        break;
      *next_after_configuration_id =
          items[*count].optical_configuration_id;
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE &&
        *count < capacity)
      result = sqlite_result(database, code, "list optical configurations");
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK) {
    memset(items, 0, capacity * sizeof(*items));
    *count = 0;
    *next_after_configuration_id = after_configuration_id;
  }
  return result;
}

static Lardon3DProjectDbResult validate_campaign_for_optics_locked(
    Lardon3DProjectDb *database, uint64_t task_id, uint32_t group_id,
    sqlite3_int64 *cursor, sqlite3_int64 *mapping_count) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT c.next_group_id,c.group_count,t.task_kind,t.task_kind_version,"
      "c.scanset_id,s.scanset_id,"
      "(SELECT COUNT(*) FROM acquisition_campaign_captures m WHERE "
      "m.task_id=c.task_id),"
      "(SELECT COUNT(CASE WHEN typeof(m.task_id)!='integer' OR "
      "typeof(m.group_id)!='integer' OR m.group_id<1 OR "
      "m.group_id>c.next_group_id OR m.group_id>c.group_count OR "
      "typeof(m.capture_id)!='integer' OR m.capture_id<=0 OR p.capture_id IS "
      "NULL OR typeof(p.scanset_id)!='integer' OR p.scanset_id<=0 OR "
      "p.scanset_id!=c.scanset_id THEN 1 END) FROM "
      "acquisition_campaign_captures m LEFT JOIN captures p ON "
      "p.capture_id=m.capture_id WHERE m.task_id=c.task_id),"
      "(SELECT MIN(m.group_id) FROM acquisition_campaign_captures m WHERE "
      "m.task_id=c.task_id),(SELECT MAX(m.group_id) FROM "
      "acquisition_campaign_captures m WHERE m.task_id=c.task_id),c.request FROM "
      "acquisition_campaign_tasks c LEFT JOIN tasks t ON t.task_id=c.task_id "
      "LEFT JOIN scansets s ON s.scanset_id=c.scanset_id WHERE c.task_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)task_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load campaign for optics");
    else {
      sqlite3_int64 local_cursor = sqlite3_column_int64(statement, 0);
      sqlite3_int64 group_count = sqlite3_column_int64(statement, 1);
      sqlite3_int64 task_version = sqlite3_column_int64(statement, 3);
      sqlite3_int64 scanset_id = sqlite3_column_int64(statement, 4);
      sqlite3_int64 joined_scanset_id = sqlite3_column_int64(statement, 5);
      sqlite3_int64 local_mapping_count = sqlite3_column_int64(statement, 6);
      sqlite3_int64 invalid_mapping_count = sqlite3_column_int64(statement, 7);
      sqlite3_int64 minimum_group_id = sqlite3_column_int64(statement, 8);
      sqlite3_int64 maximum_group_id = sqlite3_column_int64(statement, 9);
      int minimum_type = sqlite3_column_type(statement, 8);
      int maximum_type = sqlite3_column_type(statement, 9);
      int request_bytes = sqlite3_column_bytes(statement, 10);
      /* INVARIANT: optical assignment may observe only the same exact
         one-based retained prefix as campaign recovery. Count/min/max prove
         contiguity under the mapping primary key; the invalid aggregate also
         rejects ahead rows, bad storage classes and cross-ScanSet Captures.
         The immutable request is validated too: optics must never make a
         campaign mutable/visible when canonical recovery rejects its payload. */
      bool empty_prefix = local_cursor == 0 && local_mapping_count == 0 &&
                          minimum_type == SQLITE_NULL &&
                          maximum_type == SQLITE_NULL;
      bool complete_prefix =
          local_cursor > 0 && local_mapping_count == local_cursor &&
          minimum_type == SQLITE_INTEGER && maximum_type == SQLITE_INTEGER &&
          minimum_group_id == 1 && maximum_group_id == local_cursor;
      if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 3) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 4) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 5) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 6) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 7) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 10) != SQLITE_BLOB ||
          local_cursor < 0 || group_count < 1 || group_count > 4096 ||
          local_cursor > group_count || group_id == 0 || group_id > group_count ||
          scanset_id <= 0 || joined_scanset_id != scanset_id ||
          local_mapping_count < 0 || local_mapping_count > 4096 ||
          invalid_mapping_count != 0 || (!empty_prefix && !complete_prefix) ||
          request_bytes <= 0 ||
          (size_t)request_bytes >
              LARDON3D_ACQUISITION_CAMPAIGN_TASK_REQUEST_MAX_BYTES ||
          sqlite3_column_blob(statement, 10) == NULL ||
          !optical_column_equals(statement, 2,
                                 LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND) ||
          task_version != LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION ||
          sqlite3_step(statement) != SQLITE_DONE) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else {
        *cursor = local_cursor;
        *mapping_count = local_mapping_count;
      }
    }
  }
  (void)sqlite3_finalize(statement);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_campaign_group_assign(
    Lardon3DProjectDb *database, uint64_t campaign_task_id, uint32_t group_id,
    uint64_t optical_configuration_id) {
  if (!database || !optical_id(campaign_task_id) || group_id == 0 ||
      group_id > 4096 || !optical_id(optical_configuration_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin campaign group optics");
  Lardon3DOpticalConfiguration configuration;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = configuration_load_locked(database, optical_configuration_id,
                                       &configuration);
  sqlite3_int64 cursor = 0;
  sqlite3_int64 mapping_count = 0;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = validate_campaign_for_optics_locked(
        database, campaign_task_id, group_id, &cursor, &mapping_count);

  sqlite3_stmt *statement = NULL;
  bool assignment_absent = false;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT optical_configuration_id FROM acquisition_campaign_group_optics "
        "WHERE task_id=?1 AND group_id=?2",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)campaign_task_id);
    (void)sqlite3_bind_int64(statement, 2, group_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      assignment_absent = true;
    } else if (code != SQLITE_ROW) {
      result = sqlite_result(database, code, "load campaign group optics");
    } else if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
               sqlite3_column_int64(statement, 0) <= 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else if ((uint64_t)sqlite3_column_int64(statement, 0) !=
               optical_configuration_id) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    } else if (sqlite3_step(statement) != SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
  }
  (void)sqlite3_finalize(statement);
  statement = NULL;

  if (result == LARDON3D_PROJECT_DB_OK && assignment_absent && cursor == 0 &&
      mapping_count != 0)
    result = LARDON3D_PROJECT_DB_CORRUPT;
  if (result == LARDON3D_PROJECT_DB_OK && assignment_absent && cursor != 0)
    result = LARDON3D_PROJECT_DB_CONSTRAINT;
  if (result == LARDON3D_PROJECT_DB_OK && assignment_absent)
    result = prepare(
        database,
        "INSERT INTO acquisition_campaign_group_optics(task_id,group_id,"
        "optical_configuration_id) VALUES(?1,?2,?3)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK && assignment_absent) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)campaign_task_id);
    (void)sqlite3_bind_int64(statement, 2, group_id);
    (void)sqlite3_bind_int64(statement, 3,
                             (sqlite3_int64)optical_configuration_id);
    result = step_done(database, statement, "insert campaign group optics");
    statement = NULL;
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result,
                                      "commit campaign group optics",
                                      "rollback campaign group optics");
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_campaign_group_load(
    Lardon3DProjectDb *database, uint64_t campaign_task_id, uint32_t group_id,
    Lardon3DOpticalCampaignGroupAssignment *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(campaign_task_id) || group_id == 0 ||
      group_id > 4096 || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT g.task_id,g.group_id,g.optical_configuration_id,c.group_count,"
      "t.task_kind,t.task_kind_version,o.optical_configuration_id FROM "
      "acquisition_campaign_group_optics g LEFT JOIN acquisition_campaign_tasks c "
      "ON c.task_id=g.task_id LEFT JOIN tasks t ON t.task_id=g.task_id LEFT JOIN "
      "optical_configurations o ON o.optical_configuration_id="
      "g.optical_configuration_id WHERE g.task_id=?1 AND g.group_id=?2",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)campaign_task_id);
    (void)sqlite3_bind_int64(statement, 2, group_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load campaign group assignment");
    else {
      sqlite3_int64 task = sqlite3_column_int64(statement, 0);
      sqlite3_int64 group = sqlite3_column_int64(statement, 1);
      sqlite3_int64 configuration = sqlite3_column_int64(statement, 2);
      sqlite3_int64 group_count = sqlite3_column_int64(statement, 3);
      sqlite3_int64 version = sqlite3_column_int64(statement, 5);
      sqlite3_int64 joined_configuration = sqlite3_column_int64(statement, 6);
      if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 2) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 3) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 5) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 6) != SQLITE_INTEGER ||
          task != (sqlite3_int64)campaign_task_id ||
          group != (sqlite3_int64)group_id || configuration <= 0 ||
          group_count < 1 || group_count > 4096 || group > group_count ||
          joined_configuration != configuration ||
          !optical_column_equals(statement, 4,
                                 LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND) ||
          version != LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION ||
          sqlite3_step(statement) != SQLITE_DONE) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else {
        output->campaign_task_id = (uint64_t)task;
        output->group_id = (uint32_t)group;
        output->optical_configuration_id = (uint64_t)configuration;
      }
    }
  }
  (void)sqlite3_finalize(statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_int64 cursor = 0;
    sqlite3_int64 mapping_count = 0;
    Lardon3DProjectDbResult dependency = validate_campaign_for_optics_locked(
        database, campaign_task_id, group_id, &cursor, &mapping_count);
    if (dependency != LARDON3D_PROJECT_DB_OK)
      result = dependency == LARDON3D_PROJECT_DB_NOT_FOUND
                   ? LARDON3D_PROJECT_DB_CORRUPT
                   : dependency;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    Lardon3DOpticalConfiguration configuration = {0};
    Lardon3DProjectDbResult dependency = configuration_load_locked(
        database, output->optical_configuration_id, &configuration);
    if (dependency != LARDON3D_PROJECT_DB_OK)
      result = dependency == LARDON3D_PROJECT_DB_NOT_FOUND
                   ? LARDON3D_PROJECT_DB_CORRUPT
                   : dependency;
  }
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

static Lardon3DProjectDbResult capture_assignment_load_locked(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureAssignment *output) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT a.capture_id,a.optical_configuration_id,a.assignment_provenance,"
      "a.campaign_task_id,a.campaign_group_id,c.capture_id,"
      "o.optical_configuration_id,g.optical_configuration_id,m.capture_id,"
      "ac.group_count,t.task_kind,t.task_kind_version,c.scanset_id,"
      "cs.scanset_id FROM "
      "capture_optical_configurations a LEFT JOIN captures c ON c.capture_id="
      "a.capture_id LEFT JOIN optical_configurations o ON o.optical_configuration_id="
      "a.optical_configuration_id LEFT JOIN acquisition_campaign_group_optics g ON "
      "g.task_id=a.campaign_task_id AND g.group_id=a.campaign_group_id LEFT JOIN "
      "acquisition_campaign_captures m ON m.task_id=a.campaign_task_id AND "
      "m.group_id=a.campaign_group_id LEFT JOIN acquisition_campaign_tasks ac ON "
      "ac.task_id=a.campaign_task_id LEFT JOIN tasks t ON t.task_id="
      "a.campaign_task_id LEFT JOIN scansets cs ON cs.scanset_id=c.scanset_id "
      "WHERE a.capture_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)capture_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load Capture optical assignment");
    else {
      sqlite3_int64 stored_capture = sqlite3_column_int64(statement, 0);
      sqlite3_int64 configuration = sqlite3_column_int64(statement, 1);
      sqlite3_int64 provenance = sqlite3_column_int64(statement, 2);
      sqlite3_int64 campaign_task = sqlite3_column_int64(statement, 3);
      sqlite3_int64 campaign_group = sqlite3_column_int64(statement, 4);
      sqlite3_int64 joined_capture = sqlite3_column_int64(statement, 5);
      sqlite3_int64 joined_configuration = sqlite3_column_int64(statement, 6);
      /* A surviving capture_id alone is insufficient: an orphaned or
         dynamically mistyped ScanSet relation is durable corruption, not a
         valid optical owner. No path/metadata fallback may repair it. */
      sqlite3_int64 capture_scanset = sqlite3_column_int64(statement, 12);
      sqlite3_int64 joined_scanset = sqlite3_column_int64(statement, 13);
      if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 2) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 5) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 6) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 12) != SQLITE_INTEGER ||
          sqlite3_column_type(statement, 13) != SQLITE_INTEGER ||
          stored_capture != (sqlite3_int64)capture_id || stored_capture <= 0 ||
          configuration <= 0 || joined_capture != stored_capture ||
          joined_configuration != configuration || capture_scanset <= 0 ||
          joined_scanset != capture_scanset ||
          (provenance != LARDON3D_OPTICAL_ASSIGNMENT_CAMPAIGN &&
           provenance != LARDON3D_OPTICAL_ASSIGNMENT_CALLER_EXPLICIT)) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else if (provenance == LARDON3D_OPTICAL_ASSIGNMENT_CAMPAIGN) {
        sqlite3_int64 group_count = sqlite3_column_int64(statement, 9);
        sqlite3_int64 task_version = sqlite3_column_int64(statement, 11);
        if (sqlite3_column_type(statement, 3) != SQLITE_INTEGER ||
            sqlite3_column_type(statement, 4) != SQLITE_INTEGER ||
            sqlite3_column_type(statement, 7) != SQLITE_INTEGER ||
            sqlite3_column_type(statement, 8) != SQLITE_INTEGER ||
            sqlite3_column_type(statement, 9) != SQLITE_INTEGER ||
            sqlite3_column_type(statement, 11) != SQLITE_INTEGER ||
            campaign_task <= 0 || campaign_group <= 0 || campaign_group > 4096 ||
            group_count < 1 || group_count > 4096 || campaign_group > group_count ||
            sqlite3_column_int64(statement, 7) != configuration ||
            sqlite3_column_int64(statement, 8) != stored_capture ||
            !optical_column_equals(statement, 10,
                                   LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND) ||
            task_version != LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION)
          result = LARDON3D_PROJECT_DB_CORRUPT;
      } else if (sqlite3_column_type(statement, 3) != SQLITE_NULL ||
                 sqlite3_column_type(statement, 4) != SQLITE_NULL ||
                 sqlite3_column_type(statement, 7) != SQLITE_NULL ||
                 sqlite3_column_type(statement, 8) != SQLITE_NULL ||
                 sqlite3_column_type(statement, 9) != SQLITE_NULL ||
                 sqlite3_column_type(statement, 10) != SQLITE_NULL ||
                 sqlite3_column_type(statement, 11) != SQLITE_NULL) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      }
      if (result == LARDON3D_PROJECT_DB_OK &&
          sqlite3_step(statement) != SQLITE_DONE)
        result = LARDON3D_PROJECT_DB_CORRUPT;
      if (result == LARDON3D_PROJECT_DB_OK) {
        output->capture_id = (uint64_t)stored_capture;
        output->optical_configuration_id = (uint64_t)configuration;
        output->provenance = (Lardon3DOpticalAssignmentProvenance)provenance;
        output->has_campaign_origin =
            provenance == LARDON3D_OPTICAL_ASSIGNMENT_CAMPAIGN;
        output->campaign_task_id =
            output->has_campaign_origin ? (uint64_t)campaign_task : 0;
        output->campaign_group_id =
            output->has_campaign_origin ? (uint32_t)campaign_group : 0;
      }
    }
  }
  (void)sqlite3_finalize(statement);
  if (result == LARDON3D_PROJECT_DB_OK && output->has_campaign_origin) {
    sqlite3_int64 cursor = 0;
    sqlite3_int64 mapping_count = 0;
    Lardon3DProjectDbResult dependency = validate_campaign_for_optics_locked(
        database, output->campaign_task_id, output->campaign_group_id, &cursor,
        &mapping_count);
    if (dependency != LARDON3D_PROJECT_DB_OK)
      result = dependency == LARDON3D_PROJECT_DB_NOT_FOUND
                   ? LARDON3D_PROJECT_DB_CORRUPT
                   : dependency;
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    Lardon3DOpticalConfiguration configuration = {0};
    Lardon3DProjectDbResult dependency = configuration_load_locked(
        database, output->optical_configuration_id, &configuration);
    if (dependency != LARDON3D_PROJECT_DB_OK)
      result = dependency == LARDON3D_PROJECT_DB_NOT_FOUND
                   ? LARDON3D_PROJECT_DB_CORRUPT
                   : dependency;
  }
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_capture_assign_explicit(
    Lardon3DProjectDb *database, uint64_t capture_id,
    uint64_t optical_configuration_id) {
  if (!database || !optical_id(capture_id) ||
      !optical_id(optical_configuration_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      execute(database, "BEGIN IMMEDIATE", "begin explicit Capture optics");
  Lardon3DOpticalConfiguration configuration;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = configuration_load_locked(database, optical_configuration_id,
                                       &configuration);
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT c.capture_id,c.scanset_id,s.scanset_id FROM captures c LEFT "
        "JOIN scansets s ON s.scanset_id=c.scanset_id WHERE c.capture_id=?1",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)capture_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load Capture for explicit optics");
    else if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
             sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
             sqlite3_column_type(statement, 2) != SQLITE_INTEGER ||
             sqlite3_column_int64(statement, 0) != (sqlite3_int64)capture_id ||
             sqlite3_column_int64(statement, 1) <= 0 ||
             sqlite3_column_int64(statement, 2) !=
                 sqlite3_column_int64(statement, 1) ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  statement = NULL;

  Lardon3DOpticalCaptureAssignment existing = {0};
  if (result == LARDON3D_PROJECT_DB_OK) {
    Lardon3DProjectDbResult load =
        capture_assignment_load_locked(database, capture_id, &existing);
    if (load == LARDON3D_PROJECT_DB_OK) {
      result = existing.provenance ==
                       LARDON3D_OPTICAL_ASSIGNMENT_CALLER_EXPLICIT &&
                   existing.optical_configuration_id == optical_configuration_id
                   ? LARDON3D_PROJECT_DB_OK
                   : LARDON3D_PROJECT_DB_CONSTRAINT;
    } else if (load == LARDON3D_PROJECT_DB_NOT_FOUND) {
      result = prepare(
          database,
          "INSERT INTO capture_optical_configurations("
          "capture_id,optical_configuration_id,assignment_provenance,"
          "campaign_task_id,campaign_group_id) VALUES(?1,?2,2,NULL,NULL)",
          &statement);
      if (result == LARDON3D_PROJECT_DB_OK) {
        (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)capture_id);
        (void)sqlite3_bind_int64(statement, 2,
                                 (sqlite3_int64)optical_configuration_id);
        result = step_done(database, statement, "insert explicit Capture optics");
        statement = NULL;
      }
    } else {
      result = load;
    }
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result,
                                      "commit explicit Capture optics",
                                      "rollback explicit Capture optics");
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_capture_assignment_load(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureAssignment *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(capture_id) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result =
      capture_assignment_load_locked(database, capture_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

static bool read_calibration_profile(
    sqlite3_stmt *statement, Lardon3DOpticalCalibrationProfile *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_int64 profile_id = sqlite3_column_int64(statement, 0);
  sqlite3_int64 configuration_id = sqlite3_column_int64(statement, 1);
  sqlite3_int64 calibration_id = sqlite3_column_int64(statement, 2);
  sqlite3_int64 profile_version = sqlite3_column_int64(statement, 4);
  sqlite3_int64 applicability = sqlite3_column_int64(statement, 6);
  sqlite3_int64 created_at = sqlite3_column_int64(statement, 7);
  sqlite3_int64 joined_configuration = sqlite3_column_int64(statement, 8);
  sqlite3_int64 joined_calibration = sqlite3_column_int64(statement, 9);
  if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || profile_id <= 0 ||
      sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
      configuration_id <= 0 ||
      sqlite3_column_type(statement, 2) != SQLITE_INTEGER || calibration_id <= 0 ||
      !optical_copy_text(statement, 3, output->name, sizeof(output->name),
                         false) ||
      sqlite3_column_type(statement, 4) != SQLITE_INTEGER ||
      profile_version <= 0 || profile_version > UINT32_MAX ||
      !optical_copy_text(statement, 5, output->provenance,
                         sizeof(output->provenance), false) ||
      sqlite3_column_type(statement, 6) != SQLITE_INTEGER ||
      applicability != LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION ||
      sqlite3_column_type(statement, 7) != SQLITE_INTEGER || created_at < 0 ||
      sqlite3_column_type(statement, 8) != SQLITE_INTEGER ||
      joined_configuration != configuration_id ||
      sqlite3_column_type(statement, 9) != SQLITE_INTEGER ||
      joined_calibration != calibration_id) {
    memset(output, 0, sizeof(*output));
    return false;
  }
  output->calibration_profile_id = (uint64_t)profile_id;
  output->optical_configuration_id = (uint64_t)configuration_id;
  output->sparse_calibration_id = (uint64_t)calibration_id;
  output->profile_version = (uint32_t)profile_version;
  output->applicability =
      (Lardon3DOpticalCalibrationApplicability)applicability;
  output->created_at = created_at;
  return true;
}

static const char calibration_profile_select[] =
    "SELECT p.calibration_profile_id,p.optical_configuration_id,"
    "p.sparse_calibration_id,p.name,p.profile_version,p.provenance,"
    "p.applicability,p.created_at,o.optical_configuration_id,s.calibration_id "
    "FROM optical_calibration_profiles p LEFT JOIN optical_configurations o ON "
    "o.optical_configuration_id=p.optical_configuration_id LEFT JOIN "
    "sparse_calibrations s ON s.calibration_id=p.sparse_calibration_id ";

static Lardon3DProjectDbResult calibration_profile_load_locked(
    Lardon3DProjectDb *database, uint64_t profile_id,
    Lardon3DOpticalCalibrationProfile *output) {
  memset(output, 0, sizeof(*output));
  sqlite3_stmt *statement = NULL;
  char query[1024];
  int length = snprintf(query, sizeof(query),
                        "%s WHERE p.calibration_profile_id=?1",
                        calibration_profile_select);
  if (length < 0 || (size_t)length >= sizeof(query))
    return LARDON3D_PROJECT_DB_IO_ERROR;
  Lardon3DProjectDbResult result = prepare(database, query, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)profile_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code, "load optical calibration profile");
    else if (!read_calibration_profile(statement, output) ||
             output->calibration_profile_id != profile_id ||
             sqlite3_step(statement) != SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  (void)sqlite3_finalize(statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

static Lardon3DProjectDbResult calibration_profile_find_natural_key_locked(
    Lardon3DProjectDb *database,
    const Lardon3DOpticalCalibrationProfile *input, uint64_t *profile_id) {
  *profile_id = 0;
  sqlite3_stmt *statement = NULL;
  char query[1200];
  int length = snprintf(
      query, sizeof(query),
      "%s WHERE p.optical_configuration_id=?1 AND p.name=?2 AND "
      "p.profile_version=?3",
      calibration_profile_select);
  Lardon3DProjectDbResult result =
      length < 0 || (size_t)length >= sizeof(query)
          ? LARDON3D_PROJECT_DB_IO_ERROR
          : prepare(database, query, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)input->optical_configuration_id);
    (void)sqlite3_bind_text(statement, 2, input->name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 3, input->profile_version);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    } else if (code != SQLITE_ROW) {
      result = sqlite_result(database, code, "find optical calibration profile");
    } else {
      Lardon3DOpticalCalibrationProfile stored;
      if (!read_calibration_profile(statement, &stored) ||
          stored.optical_configuration_id != input->optical_configuration_id ||
          strcmp(stored.name, input->name) != 0 ||
          stored.profile_version != input->profile_version ||
          sqlite3_step(statement) != SQLITE_DONE) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else {
        *profile_id = stored.calibration_profile_id;
      }
    }
  }
  (void)sqlite3_finalize(statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    *profile_id = 0;
  return result;
}

static Lardon3DProjectDbResult calibration_profile_reconcile_existing(
    Lardon3DProjectDb *database,
    const Lardon3DOpticalCalibrationProfile *input, uint64_t profile_id,
    Lardon3DOpticalCalibrationProfile *output) {
  Lardon3DOpticalCalibrationProfile stored;
  Lardon3DProjectDbResult result =
      lardon3d_optical_calibration_profile_load(database, profile_id, &stored);
  if (result == LARDON3D_PROJECT_DB_NOT_FOUND)
    return LARDON3D_PROJECT_DB_CORRUPT;
  if (result != LARDON3D_PROJECT_DB_OK)
    return result;

  /* CONTRACT: durable state wins result classification. Validate the complete
     stored profile and its immutable sparse-calibration dependency before
     treating different caller properties as a normal identity conflict. */
  if (stored.optical_configuration_id != input->optical_configuration_id ||
      stored.sparse_calibration_id != input->sparse_calibration_id ||
      strcmp(stored.name, input->name) != 0 ||
      stored.profile_version != input->profile_version ||
      strcmp(stored.provenance, input->provenance) != 0 ||
      stored.applicability != input->applicability ||
      stored.created_at != input->created_at)
    return LARDON3D_PROJECT_DB_CONSTRAINT;
  *output = stored;
  return LARDON3D_PROJECT_DB_OK;
}

Lardon3DProjectDbResult lardon3d_optical_calibration_profile_create(
    Lardon3DProjectDb *database,
    const Lardon3DOpticalCalibrationProfile *input,
    Lardon3DOpticalCalibrationProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !input || !output || input == output ||
      input->calibration_profile_id != 0 ||
      !optical_id(input->optical_configuration_id) ||
      !optical_id(input->sparse_calibration_id) ||
      !optical_text(input->name, sizeof(input->name), false) ||
      input->profile_version == 0 ||
      !optical_text(input->provenance, sizeof(input->provenance), false) ||
      input->applicability !=
          LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION ||
      input->created_at < 0)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;

  /* WHY: reconcile the durable natural identity before validating a possibly
     different caller dependency. Otherwise an orphaned stored profile could be
     disguised as NOT_FOUND (exact retry) or CONSTRAINT (property conflict). */
  uint64_t profile_id = 0;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result = calibration_profile_find_natural_key_locked(
      database, input, &profile_id);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result == LARDON3D_PROJECT_DB_OK)
    return calibration_profile_reconcile_existing(database, input, profile_id,
                                                  output);
  if (result != LARDON3D_PROJECT_DB_NOT_FOUND)
    return result;

  /* With no durable natural-key row, the caller's v22 sparse object is
     validated through its canonical loader before binding. This module stores
     only context/provenance and never duplicates its numerical identity. */
  Lardon3DSparseCalibration sparse_calibration;
  result = lardon3d_sparse_calibration_load(
      database, input->sparse_calibration_id, &sparse_calibration);
  if (result != LARDON3D_PROJECT_DB_OK)
    return result;

  (void)pthread_mutex_lock(&database->mutex);
  result = execute(database, "BEGIN IMMEDIATE", "begin optical calibration profile");
  Lardon3DOpticalConfiguration configuration;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = configuration_load_locked(database, input->optical_configuration_id,
                                       &configuration);
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "INSERT OR IGNORE INTO optical_calibration_profiles("
        "optical_configuration_id,sparse_calibration_id,name,profile_version,"
        "provenance,applicability,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)input->optical_configuration_id);
    (void)sqlite3_bind_int64(statement, 2,
                             (sqlite3_int64)input->sparse_calibration_id);
    (void)sqlite3_bind_text(statement, 3, input->name, -1, SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 4, input->profile_version);
    (void)sqlite3_bind_text(statement, 5, input->provenance, -1,
                           SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(statement, 6, input->applicability);
    (void)sqlite3_bind_int64(statement, 7, input->created_at);
    result = step_done(database, statement, "insert optical calibration profile");
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = calibration_profile_find_natural_key_locked(database, input,
                                                         &profile_id);
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(database, result,
                                      "commit optical calibration profile",
                                      "rollback optical calibration profile");
  (void)pthread_mutex_unlock(&database->mutex);
  if (result != LARDON3D_PROJECT_DB_OK)
    return result;
  return calibration_profile_reconcile_existing(database, input, profile_id,
                                                output);
}

Lardon3DProjectDbResult lardon3d_optical_calibration_profile_load(
    Lardon3DProjectDb *database, uint64_t calibration_profile_id,
    Lardon3DOpticalCalibrationProfile *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(calibration_profile_id) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DProjectDbResult result = calibration_profile_load_locked(
      database, calibration_profile_id, output);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result == LARDON3D_PROJECT_DB_OK) {
    Lardon3DOpticalConfiguration configuration;
    Lardon3DSparseCalibration calibration;
    Lardon3DProjectDbResult configuration_result =
        lardon3d_optical_configuration_load(
            database, output->optical_configuration_id, &configuration);
    Lardon3DProjectDbResult calibration_result =
        configuration_result == LARDON3D_PROJECT_DB_OK
            ? lardon3d_sparse_calibration_load(
                  database, output->sparse_calibration_id, &calibration)
            : configuration_result;
    if (calibration_result != LARDON3D_PROJECT_DB_OK) {
      result = calibration_result == LARDON3D_PROJECT_DB_NOT_FOUND
                   ? LARDON3D_PROJECT_DB_CORRUPT
                   : calibration_result;
      memset(output, 0, sizeof(*output));
    }
  }
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_calibration_profile_list_compatible(
    Lardon3DProjectDb *database, uint64_t optical_configuration_id,
    uint64_t after_profile_id, Lardon3DOpticalCalibrationProfile *items,
    size_t capacity, size_t *count, uint64_t *next_after_profile_id) {
  if (count)
    *count = 0;
  if (next_after_profile_id)
    *next_after_profile_id = after_profile_id;
  if (!database || !optical_id(optical_configuration_id) ||
      !optical_page_arguments(after_profile_id, items, capacity, count,
                              next_after_profile_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  memset(items, 0, capacity * sizeof(*items));
  (void)pthread_mutex_lock(&database->mutex);
  Lardon3DOpticalConfiguration configuration;
  Lardon3DProjectDbResult result = configuration_load_locked(
      database, optical_configuration_id, &configuration);
  sqlite3_stmt *statement = NULL;
  char query[1200];
  int length = snprintf(
      query, sizeof(query),
      "%s WHERE p.optical_configuration_id=?1 AND p.calibration_profile_id>?2 "
      "ORDER BY p.calibration_profile_id LIMIT ?3",
      calibration_profile_select);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = length < 0 || (size_t)length >= sizeof(query)
                 ? LARDON3D_PROJECT_DB_IO_ERROR
                 : prepare(database, query, &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1,
                             (sqlite3_int64)optical_configuration_id);
    (void)sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_profile_id);
    (void)sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    int code = SQLITE_DONE;
    while (*count < capacity && (code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (!read_calibration_profile(statement, &items[*count]) ||
          items[*count].optical_configuration_id != optical_configuration_id) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      *next_after_profile_id = items[*count].calibration_profile_id;
      ++*count;
    }
    if (result == LARDON3D_PROJECT_DB_OK && code != SQLITE_DONE &&
        *count < capacity)
      result = sqlite_result(database, code,
                             "list compatible optical calibrations");
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result == LARDON3D_PROJECT_DB_OK) {
    for (size_t index = 0; index < *count; ++index) {
      Lardon3DSparseCalibration calibration;
      Lardon3DProjectDbResult dependency = lardon3d_sparse_calibration_load(
          database, items[index].sparse_calibration_id, &calibration);
      if (dependency != LARDON3D_PROJECT_DB_OK) {
        result = dependency == LARDON3D_PROJECT_DB_NOT_FOUND
                     ? LARDON3D_PROJECT_DB_CORRUPT
                     : dependency;
        break;
      }
    }
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    memset(items, 0, capacity * sizeof(*items));
    *count = 0;
    *next_after_profile_id = after_profile_id;
  }
  return result;
}

Lardon3DProjectDbResult lardon3d_optical_capture_calibration_select(
    Lardon3DProjectDb *database, uint64_t capture_id,
    uint64_t calibration_profile_id) {
  if (!database || !optical_id(capture_id) ||
      !optical_id(calibration_profile_id))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  Lardon3DOpticalCalibrationProfile validated_profile;
  Lardon3DProjectDbResult result = lardon3d_optical_calibration_profile_load(
      database, calibration_profile_id, &validated_profile);
  if (result != LARDON3D_PROJECT_DB_OK)
    return result;
  (void)pthread_mutex_lock(&database->mutex);
  result = execute(
      database, "BEGIN IMMEDIATE", "begin Capture calibration selection");
  Lardon3DOpticalCaptureAssignment assignment = {0};
  Lardon3DOpticalCalibrationProfile profile = {0};
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = capture_assignment_load_locked(database, capture_id, &assignment);
    if (result == LARDON3D_PROJECT_DB_NOT_FOUND) {
      sqlite3_stmt *capture_statement = NULL;
      result = prepare(
          database,
          "SELECT c.capture_id,c.scanset_id,s.scanset_id FROM captures c LEFT "
          "JOIN scansets s ON s.scanset_id=c.scanset_id WHERE c.capture_id=?1",
          &capture_statement);
      if (result == LARDON3D_PROJECT_DB_OK) {
        (void)sqlite3_bind_int64(capture_statement, 1,
                                 (sqlite3_int64)capture_id);
        int code = sqlite3_step(capture_statement);
        if (code == SQLITE_DONE)
          result = LARDON3D_PROJECT_DB_NOT_FOUND;
        else if (code != SQLITE_ROW)
          result = sqlite_result(database, code,
                                 "load Capture calibration owner");
        else if (sqlite3_column_type(capture_statement, 0) != SQLITE_INTEGER ||
                 sqlite3_column_type(capture_statement, 1) != SQLITE_INTEGER ||
                 sqlite3_column_type(capture_statement, 2) != SQLITE_INTEGER ||
                 sqlite3_column_int64(capture_statement, 0) !=
                     (sqlite3_int64)capture_id ||
                 sqlite3_column_int64(capture_statement, 1) <= 0 ||
                 sqlite3_column_int64(capture_statement, 2) !=
                     sqlite3_column_int64(capture_statement, 1) ||
                 sqlite3_step(capture_statement) != SQLITE_DONE)
          result = LARDON3D_PROJECT_DB_CORRUPT;
        else
          result = LARDON3D_PROJECT_DB_CONSTRAINT;
      }
      (void)sqlite3_finalize(capture_statement);
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = calibration_profile_load_locked(database, calibration_profile_id,
                                             &profile);
  if (result == LARDON3D_PROJECT_DB_OK &&
      (profile.optical_configuration_id !=
           validated_profile.optical_configuration_id ||
       profile.sparse_calibration_id != validated_profile.sparse_calibration_id))
    result = LARDON3D_PROJECT_DB_CORRUPT;
  if (result == LARDON3D_PROJECT_DB_OK &&
      assignment.optical_configuration_id != profile.optical_configuration_id)
    result = LARDON3D_PROJECT_DB_CONSTRAINT;

  sqlite3_stmt *statement = NULL;
  bool selection_absent = false;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        database,
        "SELECT calibration_profile_id,optical_configuration_id FROM "
        "capture_calibration_selections WHERE capture_id=?1",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)capture_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE) {
      selection_absent = true;
    } else if (code != SQLITE_ROW) {
      result = sqlite_result(database, code, "load Capture calibration selection");
    } else if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
               sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
               sqlite3_column_int64(statement, 0) <= 0 ||
               sqlite3_column_int64(statement, 1) <= 0) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    } else if ((uint64_t)sqlite3_column_int64(statement, 0) !=
                   calibration_profile_id ||
               (uint64_t)sqlite3_column_int64(statement, 1) !=
                   assignment.optical_configuration_id) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    } else if (sqlite3_step(statement) != SQLITE_DONE) {
      result = LARDON3D_PROJECT_DB_CORRUPT;
    }
  }
  (void)sqlite3_finalize(statement);
  statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK && selection_absent)
    result = prepare(
        database,
        "INSERT INTO capture_calibration_selections(capture_id,"
        "calibration_profile_id,optical_configuration_id) VALUES(?1,?2,?3)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK && selection_absent) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)capture_id);
    (void)sqlite3_bind_int64(statement, 2,
                             (sqlite3_int64)calibration_profile_id);
    (void)sqlite3_bind_int64(statement, 3,
                             (sqlite3_int64)assignment.optical_configuration_id);
    result = step_done(database, statement, "insert Capture calibration selection");
    statement = NULL;
  }
  (void)sqlite3_finalize(statement);
  result = optical_commit_or_rollback(
      database, result, "commit Capture calibration selection",
      "rollback Capture calibration selection");
  (void)pthread_mutex_unlock(&database->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_optical_capture_calibration_selection_load(
    Lardon3DProjectDb *database, uint64_t capture_id,
    Lardon3DOpticalCaptureCalibrationSelection *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || !optical_id(capture_id) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&database->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      database,
      "SELECT x.capture_id,x.calibration_profile_id,x.optical_configuration_id,"
      "a.capture_id,a.optical_configuration_id,p.calibration_profile_id,"
      "p.optical_configuration_id,p.sparse_calibration_id,s.calibration_id FROM "
      "capture_calibration_selections x LEFT JOIN capture_optical_configurations a "
      "ON a.capture_id=x.capture_id LEFT JOIN optical_calibration_profiles p ON "
      "p.calibration_profile_id=x.calibration_profile_id LEFT JOIN "
      "sparse_calibrations s ON s.calibration_id=p.sparse_calibration_id WHERE "
      "x.capture_id=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    (void)sqlite3_bind_int64(statement, 1, (sqlite3_int64)capture_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = sqlite_result(database, code,
                             "load Capture calibration selection");
    else {
      sqlite3_int64 stored_capture = sqlite3_column_int64(statement, 0);
      sqlite3_int64 profile_id = sqlite3_column_int64(statement, 1);
      sqlite3_int64 configuration_id = sqlite3_column_int64(statement, 2);
      sqlite3_int64 calibration_id = sqlite3_column_int64(statement, 7);
      bool types_valid = true;
      for (int column = 0; column < 9; ++column)
        if (sqlite3_column_type(statement, column) != SQLITE_INTEGER)
          types_valid = false;
      if (!types_valid || stored_capture != (sqlite3_int64)capture_id ||
          stored_capture <= 0 || profile_id <= 0 || configuration_id <= 0 ||
          calibration_id <= 0 || sqlite3_column_int64(statement, 3) != stored_capture ||
          sqlite3_column_int64(statement, 4) != configuration_id ||
          sqlite3_column_int64(statement, 5) != profile_id ||
          sqlite3_column_int64(statement, 6) != configuration_id ||
          sqlite3_column_int64(statement, 8) != calibration_id ||
          sqlite3_step(statement) != SQLITE_DONE) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else {
        output->capture_id = (uint64_t)stored_capture;
        output->calibration_profile_id = (uint64_t)profile_id;
        output->optical_configuration_id = (uint64_t)configuration_id;
        output->sparse_calibration_id = (uint64_t)calibration_id;
      }
    }
  }
  (void)sqlite3_finalize(statement);
  (void)pthread_mutex_unlock(&database->mutex);
  if (result == LARDON3D_PROJECT_DB_OK) {
    Lardon3DOpticalCaptureAssignment assignment = {0};
    Lardon3DOpticalCalibrationProfile profile = {0};
    Lardon3DProjectDbResult assignment_result =
        lardon3d_optical_capture_assignment_load(database, capture_id,
                                                 &assignment);
    Lardon3DProjectDbResult profile_result =
        assignment_result == LARDON3D_PROJECT_DB_OK
            ? lardon3d_optical_calibration_profile_load(
                  database, output->calibration_profile_id, &profile)
            : assignment_result;
    if (profile_result != LARDON3D_PROJECT_DB_OK ||
        assignment.optical_configuration_id != output->optical_configuration_id ||
        profile.optical_configuration_id != output->optical_configuration_id ||
        profile.sparse_calibration_id != output->sparse_calibration_id) {
      result = profile_result == LARDON3D_PROJECT_DB_NOT_FOUND
                   ? LARDON3D_PROJECT_DB_CORRUPT
                   : profile_result;
      if (result == LARDON3D_PROJECT_DB_OK)
        result = LARDON3D_PROJECT_DB_CORRUPT;
    }
  }
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}
