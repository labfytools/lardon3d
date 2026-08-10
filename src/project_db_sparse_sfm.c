#include <limits.h>
#include <math.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "project_db_internal.h"
#include <lardon3d/sparse_sfm_model.h>

static bool sparse_finite(double value) { return isfinite(value) != 0; }

static bool sparse_fits_sqlite(uint64_t value) {
  return value <= (uint64_t)INT64_MAX;
}

static Lardon3DProjectDbResult sparse_step_reuse(Lardon3DProjectDb *db,
                                                 sqlite3_stmt *statement,
                                                 const char *context) {
  int code = sqlite3_step(statement);
  if (code != SQLITE_DONE)
    return sqlite_result(db, code, context);
  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);
  return LARDON3D_PROJECT_DB_OK;
}

static void sparse_put_u32(unsigned char *data, size_t *offset,
                           uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte)
    data[(*offset)++] = (unsigned char)(value >> (byte * 8));
}

static void sparse_put_u64(unsigned char *data, size_t *offset,
                           uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte)
    data[(*offset)++] = (unsigned char)(value >> (byte * 8));
}

static void sparse_put_double(unsigned char *data, size_t *offset,
                              double value) {
  uint64_t bits = 0;
  if (value == 0.0)
    value = 0.0;
  memcpy(&bits, &value, sizeof(bits));
  sparse_put_u64(data, offset, bits);
}

static bool sparse_hash(const unsigned char *data, size_t size,
                        unsigned char hash[32]) {
  unsigned int length = 0;
  return EVP_Digest(data, size, hash, &length, EVP_sha256(), NULL) == 1 &&
         length == 32;
}

static bool sparse_calibration_valid(const Lardon3DSparseCalibration *input) {
  if (!input ||
      input->model_kind != LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE ||
      input->model_version != 1 || input->width == 0 || input->height == 0 ||
      input->provenance_kind < LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT ||
      input->provenance_kind >
          LARDON3D_SPARSE_SFM_PROVENANCE_IMPORTED_TRUSTED ||
      !(input->fx > 0.0) || !(input->fy > 0.0) || input->cx < 0.0 ||
      input->cy < 0.0 || input->cx >= input->width ||
      input->cy >= input->height)
    return false;
  const double values[] = {input->fx, input->fy, input->cx, input->cy,
                           input->k1, input->k2, input->p1, input->p2};
  for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
    if (!sparse_finite(values[index]))
      return false;
  return true;
}

static bool sparse_calibration_hash(const Lardon3DSparseCalibration *input,
                                    unsigned char hash[32]) {
  if (!sparse_calibration_valid(input))
    return false;
  unsigned char bytes[8 + 4 * 6 + 8 * 8 + 4 + 32];
  size_t offset = 0;
  memcpy(bytes + offset, "L3D3DCP1", 8);
  offset += 8;
  sparse_put_u32(bytes, &offset, 1);
  sparse_put_u32(bytes, &offset, input->model_kind);
  sparse_put_u32(bytes, &offset, input->model_version);
  sparse_put_u32(bytes, &offset, input->width);
  sparse_put_u32(bytes, &offset, input->height);
  const double values[] = {input->fx, input->fy, input->cx, input->cy,
                           input->k1, input->k2, input->p1, input->p2};
  for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index)
    sparse_put_double(bytes, &offset, values[index]);
  sparse_put_u32(bytes, &offset, input->provenance_kind);
  memcpy(bytes + offset, input->provenance_fingerprint, 32);
  offset += 32;
  return sparse_hash(bytes, offset, hash);
}

static Lardon3DProjectDbResult
sparse_load_calibration_locked(Lardon3DProjectDb *db, uint64_t id,
                               Lardon3DSparseCalibration *output) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT "
              "scientific_hash,model_kind,model_version,width,height,fx,fy,cx,"
              "cy,k1,k2,p1,p2,"
              "provenance_kind,provenance_fingerprint FROM sparse_calibrations "
              "WHERE calibration_id=?1",
              &statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    return result;
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)id);
  int code = sqlite3_step(statement);
  if (code == SQLITE_DONE)
    result = LARDON3D_PROJECT_DB_NOT_FOUND;
  else if (code != SQLITE_ROW || sqlite3_column_bytes(statement, 0) != 32 ||
           sqlite3_column_bytes(statement, 14) != 32)
    result = LARDON3D_PROJECT_DB_CORRUPT;
  else {
    memset(output, 0, sizeof(*output));
    output->calibration_id = id;
    memcpy(output->scientific_hash, sqlite3_column_blob(statement, 0), 32);
    output->model_kind = (uint32_t)sqlite3_column_int64(statement, 1);
    output->model_version = (uint32_t)sqlite3_column_int64(statement, 2);
    output->width = (uint32_t)sqlite3_column_int64(statement, 3);
    output->height = (uint32_t)sqlite3_column_int64(statement, 4);
    output->fx = sqlite3_column_double(statement, 5);
    output->fy = sqlite3_column_double(statement, 6);
    output->cx = sqlite3_column_double(statement, 7);
    output->cy = sqlite3_column_double(statement, 8);
    output->k1 = sqlite3_column_double(statement, 9);
    output->k2 = sqlite3_column_double(statement, 10);
    output->p1 = sqlite3_column_double(statement, 11);
    output->p2 = sqlite3_column_double(statement, 12);
    output->provenance_kind = (uint32_t)sqlite3_column_int64(statement, 13);
    memcpy(output->provenance_fingerprint, sqlite3_column_blob(statement, 14),
           32);
    unsigned char expected[32];
    if (!sparse_calibration_hash(output, expected) ||
        memcmp(expected, output->scientific_hash, 32) != 0)
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  sqlite3_finalize(statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_calibration_create(Lardon3DProjectDb *db,
                                   const Lardon3DSparseCalibration *input,
                                   Lardon3DSparseCalibration *output) {
  if (!db || !sparse_calibration_valid(input) || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  unsigned char hash[32];
  if (!sparse_calibration_hash(input, hash))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result =
      execute(db, "BEGIN IMMEDIATE", "begin calibration");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "INSERT OR IGNORE INTO "
        "sparse_calibrations(scientific_hash,model_kind,model_version,width,"
        "height,"
        "fx,fy,cx,cy,k1,k2,p1,p2,provenance_kind,provenance_fingerprint) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, input->model_kind);
    sqlite3_bind_int64(statement, 3, input->model_version);
    sqlite3_bind_int64(statement, 4, input->width);
    sqlite3_bind_int64(statement, 5, input->height);
    sqlite3_bind_double(statement, 6, input->fx);
    sqlite3_bind_double(statement, 7, input->fy);
    sqlite3_bind_double(statement, 8, input->cx);
    sqlite3_bind_double(statement, 9, input->cy);
    sqlite3_bind_double(statement, 10, input->k1);
    sqlite3_bind_double(statement, 11, input->k2);
    sqlite3_bind_double(statement, 12, input->p1);
    sqlite3_bind_double(statement, 13, input->p2);
    sqlite3_bind_int64(statement, 14, input->provenance_kind);
    sqlite3_bind_blob(statement, 15, input->provenance_fingerprint, 32,
                      SQLITE_TRANSIENT);
    result = step_done(db, statement, "insert calibration");
    statement = NULL;
  }
  if (statement)
    sqlite3_finalize(statement);
  uint64_t id = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(db,
                     "SELECT calibration_id FROM sparse_calibrations WHERE "
                     "scientific_hash=?1",
                     &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
      if (sqlite3_step(statement) != SQLITE_ROW)
        result = LARDON3D_PROJECT_DB_CORRUPT;
      else
        id = (uint64_t)sqlite3_column_int64(statement, 0);
      sqlite3_finalize(statement);
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = execute(db, "COMMIT", "commit calibration");
  if (result != LARDON3D_PROJECT_DB_OK)
    (void)execute(db, "ROLLBACK", "rollback calibration");
  if (result == LARDON3D_PROJECT_DB_OK)
    result = sparse_load_calibration_locked(db, id, output);
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_calibration_load(Lardon3DProjectDb *db, uint64_t id,
                                 Lardon3DSparseCalibration *output) {
  if (!db || !id || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result =
      sparse_load_calibration_locked(db, id, output);
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_calibration_find_by_hash(Lardon3DProjectDb *db,
                                         const unsigned char hash[32],
                                         Lardon3DSparseCalibration *output) {
  if (!db || !hash || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db,
      "SELECT calibration_id FROM sparse_calibrations WHERE scientific_hash=?1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else
      result = sparse_load_calibration_locked(
          db, (uint64_t)sqlite3_column_int64(statement, 0), output);
    sqlite3_finalize(statement);
  }
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

static int sparse_scope_member_compare(const void *left, const void *right) {
  const Lardon3DSparseCalibrationMember *a = left;
  const Lardon3DSparseCalibrationMember *b = right;
  if (a->image_id < b->image_id)
    return -1;
  if (a->image_id > b->image_id)
    return 1;
  return 0;
}

static bool sparse_scope_hash(const Lardon3DSparseCalibrationMember *members,
                              size_t count, unsigned char hash[32]) {
  if (!members || count == 0 || count > (SIZE_MAX - 20U) / 40U)
    return false;
  size_t size = 20U + count * 40U;
  unsigned char *bytes = malloc(size);
  if (!bytes)
    return false;
  size_t offset = 0;
  memcpy(bytes, "L3D3DSC1", 8);
  offset = 8;
  sparse_put_u32(bytes, &offset, 1);
  sparse_put_u64(bytes, &offset, count);
  for (size_t index = 0; index < count; ++index) {
    sparse_put_u64(bytes, &offset, members[index].image_id);
    memcpy(bytes + offset, members[index].calibration_hash, 32);
    offset += 32;
  }
  bool ok = sparse_hash(bytes, offset, hash);
  free(bytes);
  return ok;
}

static bool sparse_scope_hash_locked(Lardon3DProjectDb *db, uint64_t scope_id,
                                     uint64_t member_count,
                                     unsigned char hash[32]) {
  if (member_count > (uint64_t)INT64_MAX)
    return false;
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (!context || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(context);
    return false;
  }
  unsigned char header[20];
  size_t offset = 0;
  memcpy(header, "L3D3DSC1", 8);
  offset = 8;
  sparse_put_u32(header, &offset, 1);
  sparse_put_u64(header, &offset, member_count);
  bool ok = EVP_DigestUpdate(context, header, sizeof(header)) == 1;
  sqlite3_stmt *statement = NULL;
  if (ok)
    ok = prepare(
             db,
             "SELECT m.image_id,c.scientific_hash FROM "
             "sparse_calibration_scope_images AS m JOIN sparse_calibrations AS c "
             "ON c.calibration_id=m.calibration_id WHERE m.scope_id=?1 "
             "ORDER BY m.image_id",
             &statement) == LARDON3D_PROJECT_DB_OK;
  if (ok) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)scope_id);
    uint64_t count = 0;
    int code = SQLITE_ROW;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
      if (sqlite3_column_bytes(statement, 1) != 32 || count == UINT64_MAX) {
        ok = false;
        break;
      }
      unsigned char member[40];
      offset = 0;
      sparse_put_u64(member, &offset,
                     (uint64_t)sqlite3_column_int64(statement, 0));
      memcpy(member + offset, sqlite3_column_blob(statement, 1), 32);
      if (EVP_DigestUpdate(context, member, sizeof(member)) != 1) {
        ok = false;
        break;
      }
      ++count;
    }
    if (code != SQLITE_DONE || count != member_count)
      ok = false;
    sqlite3_finalize(statement);
  }
  unsigned int length = 0;
  if (ok)
    ok = EVP_DigestFinal_ex(context, hash, &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  return ok;
}

Lardon3DProjectDbResult lardon3d_sparse_calibration_scope_create(
    Lardon3DProjectDb *db, const Lardon3DSparseCalibrationMember *members,
    size_t member_count, Lardon3DSparseCalibrationScope *output) {
  if (!db || !members || !output || member_count == 0 ||
      member_count > INT64_MAX)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  if (member_count > SIZE_MAX / sizeof(*members))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;

  Lardon3DSparseCalibrationMember *canonical =
      malloc(member_count * sizeof(*canonical));
  if (!canonical)
    return LARDON3D_PROJECT_DB_IO_ERROR;
  memcpy(canonical, members, member_count * sizeof(*canonical));
  qsort(canonical, member_count, sizeof(*canonical),
        sparse_scope_member_compare);
  for (size_t index = 0; index < member_count; ++index) {
    if (!sparse_fits_sqlite(canonical[index].image_id) ||
        canonical[index].image_id == 0 ||
        (index && canonical[index - 1].image_id == canonical[index].image_id)) {
      free(canonical);
      return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
    }
  }
  unsigned char scope_hash[32];
  if (!sparse_scope_hash(canonical, member_count, scope_hash)) {
    free(canonical);
    return LARDON3D_PROJECT_DB_IO_ERROR;
  }

  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result =
      execute(db, "BEGIN IMMEDIATE", "begin calibration scope");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(db,
                     "SELECT scientific_hash FROM sparse_calibrations "
                     "WHERE calibration_id=?1",
                     &statement);
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK && index < member_count; ++index) {
    sqlite3_bind_int64(statement, 1,
                       (sqlite3_int64)canonical[index].calibration_id);
    if (sqlite3_step(statement) != SQLITE_ROW ||
        sqlite3_column_bytes(statement, 0) != 32 ||
        memcmp(sqlite3_column_blob(statement, 0), canonical[index].calibration_hash,
               32) != 0)
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
  }
  if (statement)
    sqlite3_finalize(statement);
  statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "INSERT OR IGNORE INTO sparse_calibration_scopes(scientific_hash,"
        "member_count) VALUES(?1,?2)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_blob(statement, 1, scope_hash, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)member_count);
    result = step_done(db, statement, "insert calibration scope");
    statement = NULL;
  }
  if (statement)
    sqlite3_finalize(statement);

  uint64_t scope_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(db,
                     "SELECT scope_id FROM sparse_calibration_scopes "
                     "WHERE scientific_hash=?1",
                     &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_blob(statement, 1, scope_hash, 32, SQLITE_TRANSIENT);
      if (sqlite3_step(statement) != SQLITE_ROW)
        result = LARDON3D_PROJECT_DB_CORRUPT;
      else
        scope_id = (uint64_t)sqlite3_column_int64(statement, 0);
      sqlite3_finalize(statement);
      statement = NULL;
    }
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "INSERT OR IGNORE INTO sparse_calibration_scope_images(scope_id,"
        "image_id,calibration_id) VALUES(?1,?2,?3)",
        &statement);
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK && index < member_count; ++index) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)scope_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)canonical[index].image_id);
    sqlite3_bind_int64(statement, 3,
                       (sqlite3_int64)canonical[index].calibration_id);
    result =
        sparse_step_reuse(db, statement, "insert calibration scope member");
  }
  if (statement)
    sqlite3_finalize(statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = execute(db, "COMMIT", "commit calibration scope");
  if (result != LARDON3D_PROJECT_DB_OK)
    (void)execute(db, "ROLLBACK", "rollback calibration scope");
  if (result == LARDON3D_PROJECT_DB_OK) {
    memset(output, 0, sizeof(*output));
    output->scope_id = scope_id;
    memcpy(output->scientific_hash, scope_hash, 32);
    output->member_count = member_count;
  }
  free(canonical);
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_calibration_scope_load(Lardon3DProjectDb *db, uint64_t scope_id,
                                       Lardon3DSparseCalibrationScope *output) {
  if (!db || !scope_id || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT scientific_hash,member_count FROM "
              "sparse_calibration_scopes WHERE scope_id=?1",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)(sqlite3_int64)scope_id);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW || sqlite3_column_bytes(statement, 0) != 32 ||
             sqlite3_column_int64(statement, 1) <= 0)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else {
      memset(output, 0, sizeof(*output));
       output->scope_id = scope_id;
       memcpy(output->scientific_hash, sqlite3_column_blob(statement, 0), 32);
       output->member_count = (uint64_t)sqlite3_column_int64(statement, 1);
       unsigned char expected[32];
       if (!sparse_scope_hash_locked(db, scope_id, output->member_count,
                                     expected) ||
           memcmp(expected, output->scientific_hash, 32) != 0)
         result = LARDON3D_PROJECT_DB_CORRUPT;
    }
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_sparse_calibration_scope_find_by_hash(
    Lardon3DProjectDb *db, const unsigned char hash[32],
    Lardon3DSparseCalibrationScope *output) {
  if (!db || !hash || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT scope_id FROM sparse_calibration_scopes "
              "WHERE scientific_hash=?1",
              &statement);
  uint64_t scope_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_blob(statement, 1, hash, 32, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else
      scope_id = (uint64_t)sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
  }
  if (result == LARDON3D_PROJECT_DB_OK) {
    result = prepare(db,
                     "SELECT scientific_hash,member_count FROM "
                     "sparse_calibration_scopes WHERE scope_id=?1",
                     &statement);
    if (result == LARDON3D_PROJECT_DB_OK) {
      sqlite3_bind_int64(statement, 1, (sqlite3_int64)scope_id);
      if (sqlite3_step(statement) != SQLITE_ROW ||
          sqlite3_column_bytes(statement, 0) != 32 ||
          sqlite3_column_int64(statement, 1) <= 0) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
      } else {
        memset(output, 0, sizeof(*output));
         output->scope_id = scope_id;
         memcpy(output->scientific_hash, sqlite3_column_blob(statement, 0), 32);
         output->member_count = (uint64_t)sqlite3_column_int64(statement, 1);
         unsigned char expected[32];
         if (!sparse_scope_hash_locked(db, scope_id, output->member_count,
                                       expected) ||
             memcmp(expected, output->scientific_hash, 32) != 0)
           result = LARDON3D_PROJECT_DB_CORRUPT;
      }
      sqlite3_finalize(statement);
    }
  }
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_sparse_calibration_scope_list_members(
    Lardon3DProjectDb *db, uint64_t scope_id, uint64_t after_image_id,
    Lardon3DSparseCalibrationMember *items, size_t capacity, size_t *count,
    uint64_t *next_after_image_id) {
  if (!db || !scope_id || !items || !count || !next_after_image_id ||
      capacity > LARDON3D_SPARSE_SFM_PAGE_MAX)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  *count = 0;
  *next_after_image_id = after_image_id;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db,
      "SELECT image_id,calibration_id FROM sparse_calibration_scope_images "
      "WHERE scope_id=?1 AND image_id>?2 ORDER BY image_id LIMIT ?3",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)(sqlite3_int64)scope_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_image_id);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)(sqlite3_int64)capacity);
    while (*count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
      items[*count].image_id = (uint64_t)sqlite3_column_int64(statement, 0);
      items[*count].calibration_id =
          (uint64_t)sqlite3_column_int64(statement, 1);
      Lardon3DSparseCalibration calibration;
      result = sparse_load_calibration_locked(db, items[*count].calibration_id,
                                              &calibration);
      if (result != LARDON3D_PROJECT_DB_OK)
        break;
      memcpy(items[*count].calibration_hash, calibration.scientific_hash, 32);
      *next_after_image_id = items[*count].image_id;
      ++*count;
    }
    sqlite3_finalize(statement);
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    *count = 0;
    *next_after_image_id = after_image_id;
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

static bool sparse_rotation_valid(const double rotation[9]) {
  if (!rotation)
    return false;
  for (size_t index = 0; index < 9; ++index)
    if (!sparse_finite(rotation[index]))
      return false;
  double determinant =
      rotation[0] * (rotation[4] * rotation[8] - rotation[5] * rotation[7]) -
      rotation[1] * (rotation[3] * rotation[8] - rotation[5] * rotation[6]) +
      rotation[2] * (rotation[3] * rotation[7] - rotation[4] * rotation[6]);
  if (fabs(determinant - 1.0) > 1e-6)
    return false;
  for (size_t row = 0; row < 3; ++row)
    for (size_t column = 0; column < 3; ++column) {
      double dot = 0.0;
      for (size_t axis = 0; axis < 3; ++axis)
        dot += rotation[row * 3 + axis] * rotation[column * 3 + axis];
      if (fabs(dot - (row == column ? 1.0 : 0.0)) > 1e-6)
        return false;
    }
  return true;
}

static bool
sparse_publication_valid(const Lardon3DSparsePublication *publication) {
  if (!publication || !sparse_fits_sqlite(publication->track_set_id) ||
      !sparse_fits_sqlite(publication->calibration_scope_id) ||
      !publication->track_set_id || !publication->calibration_scope_id ||
      publication->sfm_kind != LARDON3D_SPARSE_SFM_KIND_INCREMENTAL ||
      publication->sfm_version != 1 || !publication->components ||
      publication->component_count == 0 || !publication->registered_images ||
      publication->registered_image_count < 2 || !publication->landmarks ||
      publication->landmark_count == 0 ||
      (!publication->observations && publication->observation_count) ||
      !sparse_finite(publication->reprojection_rmse_px) ||
      !sparse_finite(publication->reprojection_median_px))
    return false;
  for (size_t index = 0; index < publication->component_count; ++index) {
    const Lardon3DSparseComponent *component = &publication->components[index];
    if (!sparse_fits_sqlite(component->component_key) ||
        !sparse_fits_sqlite(component->registered_image_count) ||
        !sparse_fits_sqlite(component->landmark_count) ||
        !component->component_key || !component->registered_image_count ||
        !component->landmark_count ||
        (index && publication->components[index - 1].component_key >=
                      component->component_key))
      return false;
  }
  for (size_t index = 0; index < publication->registered_image_count; ++index) {
    const Lardon3DSparseRegisteredImage *pose =
        &publication->registered_images[index];
    if (!sparse_fits_sqlite(pose->image_id) ||
        !sparse_fits_sqlite(pose->component_key) || !pose->image_id ||
        !pose->component_key || !sparse_rotation_valid(pose->rotation_cw) ||
        !sparse_finite(pose->translation_cw[0]) ||
        !sparse_finite(pose->translation_cw[1]) ||
        !sparse_finite(pose->translation_cw[2]) ||
        (index &&
         publication->registered_images[index - 1].image_id >= pose->image_id))
       return false;
  }
  if (publication->component_count > SIZE_MAX / sizeof(size_t) / 3U ||
      publication->component_count > SIZE_MAX / sizeof(uint64_t))
    return false;
  size_t *component_image_counts =
      calloc(publication->component_count * 3U, sizeof(size_t));
  if (!component_image_counts)
    return false;
  uint64_t *minimum_image_ids =
      malloc(publication->component_count * sizeof(*minimum_image_ids));
  if (!minimum_image_ids) {
    free(component_image_counts);
    return false;
  }
  for (size_t index = 0; index < publication->component_count; ++index)
    minimum_image_ids[index] = UINT64_MAX;
  bool components_valid = true;
  for (size_t index = 0;
       components_valid && index < publication->registered_image_count;
       ++index) {
    size_t low = 0;
    size_t high = publication->component_count;
    uint64_t key = publication->registered_images[index].component_key;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      if (publication->components[middle].component_key < key)
        low = middle + 1;
      else
        high = middle;
    }
    if (low == publication->component_count ||
        publication->components[low].component_key != key)
      components_valid = false;
    else {
      ++component_image_counts[low];
      if (publication->registered_images[index].image_id <
          minimum_image_ids[low])
        minimum_image_ids[low] = publication->registered_images[index].image_id;
    }
  }
  for (size_t index = 0;
       components_valid && index < publication->landmark_count; ++index) {
    size_t low = 0;
    size_t high = publication->component_count;
    uint64_t key = publication->landmarks[index].component_key;
    while (low < high) {
      size_t middle = low + (high - low) / 2;
      if (publication->components[middle].component_key < key)
        low = middle + 1;
      else
        high = middle;
    }
    if (low == publication->component_count ||
        publication->components[low].component_key != key)
      components_valid = false;
    else
      ++component_image_counts[publication->component_count + low];
  }
  for (size_t index = 0;
       components_valid && index < publication->component_count; ++index)
    if (component_image_counts[index] !=
            publication->components[index].registered_image_count ||
        component_image_counts[publication->component_count + index] !=
            publication->components[index].landmark_count ||
        minimum_image_ids[index] != publication->components[index].component_key)
      components_valid = false;
  free(minimum_image_ids);
  free(component_image_counts);
  if (!components_valid)
    return false;
  for (size_t index = 0; index < publication->landmark_count; ++index) {
    const Lardon3DSparseLandmark *landmark = &publication->landmarks[index];
    if (!sparse_fits_sqlite(landmark->track_id) ||
        !sparse_fits_sqlite(landmark->component_key) ||
        !sparse_fits_sqlite(landmark->observation_count) ||
        !landmark->track_id || !landmark->component_key ||
        !landmark->observation_count || !sparse_finite(landmark->x) ||
        !sparse_finite(landmark->y) || !sparse_finite(landmark->z) ||
        !sparse_finite(landmark->reprojection_rmse_px) ||
        !sparse_finite(landmark->reprojection_median_px) ||
        (index &&
         publication->landmarks[index - 1].track_id >= landmark->track_id))
      return false;
  }
  return true;
}

static const Lardon3DSparseLandmark *
sparse_find_landmark(const Lardon3DSparsePublication *publication,
                     uint64_t track_id) {
  size_t low = 0;
  size_t high = publication->landmark_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    uint64_t candidate = publication->landmarks[middle].track_id;
    if (candidate == track_id)
      return &publication->landmarks[middle];
    if (candidate < track_id)
      low = middle + 1;
    else
      high = middle;
  }
  return NULL;
}

static const Lardon3DSparseComponent *sparse_find_component(
    const Lardon3DSparsePublication *publication, uint64_t component_key) {
  size_t low = 0;
  size_t high = publication->component_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    uint64_t candidate = publication->components[middle].component_key;
    if (candidate == component_key)
      return &publication->components[middle];
    if (candidate < component_key)
      low = middle + 1;
    else
      high = middle;
  }
  return NULL;
}

static const Lardon3DSparseRegisteredImage *sparse_find_registered_image(
    const Lardon3DSparsePublication *publication, uint64_t image_id) {
  size_t low = 0;
  size_t high = publication->registered_image_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    uint64_t candidate = publication->registered_images[middle].image_id;
    if (candidate == image_id)
      return &publication->registered_images[middle];
    if (candidate < image_id)
      low = middle + 1;
    else
      high = middle;
  }
  return NULL;
}

static Lardon3DProjectDbResult
sparse_validate_relations(Lardon3DProjectDb *db,
                          const Lardon3DSparsePublication *publication) {
  sqlite3_stmt *scope_statement = NULL;
  sqlite3_stmt *track_statement = NULL;
  sqlite3_stmt *observation_statement = NULL;
  sqlite3_stmt *track_component_statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT 1 FROM sparse_calibration_scope_images "
              "WHERE scope_id=?1 AND image_id=?2",
              &scope_statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(db,
                     "SELECT 1 FROM tracks WHERE track_set_id=?1 "
                     "AND track_id=?2",
                     &track_statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    result =
        prepare(db,
                "SELECT 1 FROM track_observations WHERE track_set_id=?1 "
                "AND track_id=?2 AND feature_set_id=?3 AND feature_index=?4 "
                "AND position_in_track=?5",
                &observation_statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "SELECT f.image_id FROM track_observations AS t JOIN feature_sets AS f "
        "ON f.feature_set_id=t.feature_set_id WHERE t.track_set_id=?1 AND "
        "t.track_id=?2 ORDER BY t.position_in_track",
        &track_component_statement);
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK &&
       index < publication->registered_image_count;
       ++index) {
    const Lardon3DSparseRegisteredImage *pose =
        &publication->registered_images[index];
    if (!sparse_find_component(publication, pose->component_key)) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
      break;
    }
  }
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK && index < publication->landmark_count;
       ++index) {
    const Lardon3DSparseLandmark *landmark = &publication->landmarks[index];
    if (!sparse_find_component(publication, landmark->component_key)) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
      break;
    }
    sqlite3_bind_int64(track_component_statement, 1,
                       (sqlite3_int64)publication->track_set_id);
    sqlite3_bind_int64(track_component_statement, 2,
                       (sqlite3_int64)landmark->track_id);
    while (sqlite3_step(track_component_statement) == SQLITE_ROW) {
      uint64_t image_id = (uint64_t)sqlite3_column_int64(
          track_component_statement, 0);
      const Lardon3DSparseRegisteredImage *pose =
          sparse_find_registered_image(publication, image_id);
      if (!pose || pose->component_key != landmark->component_key) {
        result = LARDON3D_PROJECT_DB_CONSTRAINT;
        break;
      }
    }
    sqlite3_reset(track_component_statement);
    sqlite3_clear_bindings(track_component_statement);
  }
  for (size_t index = 0; result == LARDON3D_PROJECT_DB_OK &&
                         index < publication->registered_image_count;
       ++index) {
    sqlite3_bind_int64(scope_statement, 1,
                       (sqlite3_int64)publication->calibration_scope_id);
    sqlite3_bind_int64(
        scope_statement, 2,
        (sqlite3_int64)publication->registered_images[index].image_id);
    int scope_code = sqlite3_step(scope_statement);
    if (scope_code != SQLITE_ROW)
    if (scope_code != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    sqlite3_reset(scope_statement);
    sqlite3_clear_bindings(scope_statement);
  }
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK && index < publication->landmark_count;
       ++index) {
    sqlite3_bind_int64(track_statement, 1,
                       (sqlite3_int64)publication->track_set_id);
    sqlite3_bind_int64(track_statement, 2,
                       (sqlite3_int64)publication->landmarks[index].track_id);
    int track_code = sqlite3_step(track_statement);
    if (track_code != SQLITE_ROW)
    if (track_code != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    sqlite3_reset(track_statement);
    sqlite3_clear_bindings(track_statement);
  }
  for (size_t index = 0; result == LARDON3D_PROJECT_DB_OK &&
                         index < publication->observation_count;
       ++index) {
    const Lardon3DSparseLandmarkObservation *ob =
        &publication->observations[index];
    if (!sparse_find_landmark(publication, ob->track_id)) {
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
      break;
    }
    sqlite3_bind_int64(observation_statement, 1,
                       (sqlite3_int64)publication->track_set_id);
    sqlite3_bind_int64(observation_statement, 2, (sqlite3_int64)ob->track_id);
    sqlite3_bind_int64(observation_statement, 3,
                       (sqlite3_int64)ob->feature_set_id);
    sqlite3_bind_int64(observation_statement, 4, ob->feature_index);
    sqlite3_bind_int64(observation_statement, 5, ob->position_in_track);
    int observation_code = sqlite3_step(observation_statement);
    if (observation_code != SQLITE_ROW)
    if (observation_code != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CONSTRAINT;
    sqlite3_reset(observation_statement);
    sqlite3_clear_bindings(observation_statement);
  }
  if (scope_statement)
    sqlite3_finalize(scope_statement);
  if (track_statement)
    sqlite3_finalize(track_statement);
  if (observation_statement)
    sqlite3_finalize(observation_statement);
  if (track_component_statement)
    sqlite3_finalize(track_component_statement);
  return result;
}

static Lardon3DProjectDbResult
sparse_load_reconstruction_locked(Lardon3DProjectDb *db, uint64_t id,
                                  Lardon3DSparseReconstruction *output) {
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT "
              "track_set_id,calibration_scope_id,sfm_kind,sfm_version,"
              "parameter_fingerprint,component_count,registered_image_count,"
              "landmark_count,reprojection_rmse_px,reprojection_median_px FROM "
              "sparse_reconstructions WHERE reconstruction_id=?1",
              &statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    return result;
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)id);
  int code = sqlite3_step(statement);
  if (code == SQLITE_DONE)
    result = LARDON3D_PROJECT_DB_NOT_FOUND;
  else if (code != SQLITE_ROW || sqlite3_column_bytes(statement, 4) != 32)
    result = LARDON3D_PROJECT_DB_CORRUPT;
  else {
    memset(output, 0, sizeof(*output));
    output->reconstruction_id = id;
    output->track_set_id = (uint64_t)sqlite3_column_int64(statement, 0);
    output->calibration_scope_id = (uint64_t)sqlite3_column_int64(statement, 1);
    output->sfm_kind = (uint32_t)sqlite3_column_int64(statement, 2);
    output->sfm_version = (uint32_t)sqlite3_column_int64(statement, 3);
    memcpy(output->parameter_fingerprint, sqlite3_column_blob(statement, 4),
           32);
    output->component_count = (uint64_t)sqlite3_column_int64(statement, 5);
    output->registered_image_count =
        (uint64_t)sqlite3_column_int64(statement, 6);
    output->landmark_count = (uint64_t)sqlite3_column_int64(statement, 7);
    output->reprojection_rmse_px = sqlite3_column_double(statement, 8);
    output->reprojection_median_px = sqlite3_column_double(statement, 9);
    if (!output->track_set_id || !output->calibration_scope_id ||
        output->sfm_kind != LARDON3D_SPARSE_SFM_KIND_INCREMENTAL ||
        output->sfm_version != LARDON3D_SPARSE_SFM_VERSION ||
        output->component_count == 0 || output->registered_image_count < 2 ||
        output->landmark_count == 0 ||
        !sparse_finite(output->reprojection_rmse_px) ||
        !sparse_finite(output->reprojection_median_px))
      result = LARDON3D_PROJECT_DB_CORRUPT;
  }
  sqlite3_finalize(statement);
  if (result != LARDON3D_PROJECT_DB_OK)
    memset(output, 0, sizeof(*output));
  return result;
}

Lardon3DProjectDbResult lardon3d_sparse_reconstruction_publish(
    Lardon3DProjectDb *db, const Lardon3DSparsePublication *publication,
    Lardon3DSparseReconstruction *output) {
  if (!db || !output || !sparse_publication_valid(publication))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result =
      execute(db, "BEGIN IMMEDIATE", "begin sparse reconstruction");
  sqlite3_stmt *statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(db, "SELECT 1 FROM track_sets WHERE track_set_id=?1",
                     &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1,
                       (sqlite3_int64)(sqlite3_int64)publication->track_set_id);
    if (sqlite3_step(statement) != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result =
        prepare(db, "SELECT 1 FROM sparse_calibration_scopes WHERE scope_id=?1",
                &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(
        statement, 1,
        (sqlite3_int64)(sqlite3_int64)publication->calibration_scope_id);
    if (sqlite3_step(statement) != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = sparse_validate_relations(db, publication);
  uint64_t reconstruction_id = 0;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "INSERT INTO "
        "sparse_reconstructions(track_set_id,calibration_scope_id,sfm_kind,sfm_"
        "version,parameter_fingerprint,component_count,registered_image_count,"
        "landmark_count,reprojection_rmse_px,reprojection_median_px,created_at)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1,
                       (sqlite3_int64)(sqlite3_int64)publication->track_set_id);
    sqlite3_bind_int64(
        statement, 2,
        (sqlite3_int64)(sqlite3_int64)publication->calibration_scope_id);
    sqlite3_bind_int64(statement, 3, publication->sfm_kind);
    sqlite3_bind_int64(statement, 4, publication->sfm_version);
    sqlite3_bind_blob(statement, 5, publication->parameter_fingerprint, 32,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 6,
                       (sqlite3_int64)publication->component_count);
    sqlite3_bind_int64(statement, 7,
                       (sqlite3_int64)publication->registered_image_count);
    sqlite3_bind_int64(statement, 8,
                       (sqlite3_int64)publication->landmark_count);
    sqlite3_bind_double(statement, 9, publication->reprojection_rmse_px);
    sqlite3_bind_double(statement, 10, publication->reprojection_median_px);
    sqlite3_bind_int64(statement, 11, publication->created_at);
    result = step_done(db, statement, "insert sparse reconstruction");
    statement = NULL;
  }
  if (statement)
    sqlite3_finalize(statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    reconstruction_id = (uint64_t)sqlite3_last_insert_rowid(db->connection);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "INSERT INTO "
        "sparse_reconstruction_components(reconstruction_id,component_key,"
        "registered_image_count,landmark_count) VALUES(?1,?2,?3,?4)",
        &statement);
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK && index < publication->component_count;
       ++index) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(
        statement, 2,
        (sqlite3_int64)publication->components[index].component_key);
    sqlite3_bind_int64(
        statement, 3,
        (sqlite3_int64)publication->components[index].registered_image_count);
    sqlite3_bind_int64(
        statement, 4,
        (sqlite3_int64)publication->components[index].landmark_count);
    result = sparse_step_reuse(db, statement, "insert sparse component");
  }
  if (statement)
    sqlite3_finalize(statement);
  statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(db,
                     "INSERT INTO "
                     "sparse_registered_images(reconstruction_id,component_id,"
                     "image_id,rotation_00,rotation_01,rotation_02,rotation_10,"
                     "rotation_11,rotation_12,rotation_20,rotation_21,rotation_"
                     "22,translation_0,translation_1,translation_2) SELECT "
                     "?1,component_id,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?"
                     "14 FROM sparse_reconstruction_components WHERE "
                     "reconstruction_id=?1 AND component_key=?15",
                     &statement);
  for (size_t index = 0; result == LARDON3D_PROJECT_DB_OK &&
                         index < publication->registered_image_count;
       ++index) {
    const Lardon3DSparseRegisteredImage *pose =
        &publication->registered_images[index];
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)pose->image_id);
    for (int bind = 0; bind < 9; ++bind)
      sqlite3_bind_double(statement, 3 + bind, pose->rotation_cw[bind]);
    for (int bind = 0; bind < 3; ++bind)
      sqlite3_bind_double(statement, 12 + bind, pose->translation_cw[bind]);
    sqlite3_bind_int64(statement, 15, (sqlite3_int64)pose->component_key);
    result = sparse_step_reuse(db, statement, "insert sparse pose");
  }
  if (statement)
    sqlite3_finalize(statement);
  statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result =
        prepare(db,
                "INSERT INTO "
                "sparse_landmarks(reconstruction_id,component_id,track_id,x,y,"
                "z,reprojection_rmse_px,reprojection_median_px,observation_"
                "count) SELECT ?1,component_id,?2,?3,?4,?5,?6,?7,?8 FROM "
                "sparse_reconstruction_components WHERE reconstruction_id=?1 "
                "AND component_key=?9",
                &statement);
  for (size_t index = 0;
       result == LARDON3D_PROJECT_DB_OK && index < publication->landmark_count;
       ++index) {
    const Lardon3DSparseLandmark *landmark = &publication->landmarks[index];
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)landmark->track_id);
    sqlite3_bind_double(statement, 3, landmark->x);
    sqlite3_bind_double(statement, 4, landmark->y);
    sqlite3_bind_double(statement, 5, landmark->z);
    sqlite3_bind_double(statement, 6, landmark->reprojection_rmse_px);
    sqlite3_bind_double(statement, 7, landmark->reprojection_median_px);
    sqlite3_bind_int64(statement, 8,
                       (sqlite3_int64)landmark->observation_count);
    sqlite3_bind_int64(statement, 9, (sqlite3_int64)landmark->component_key);
    result = sparse_step_reuse(db, statement, "insert sparse landmark");
  }
  if (statement)
    sqlite3_finalize(statement);
  statement = NULL;
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "INSERT INTO "
        "sparse_landmark_observations(landmark_id,feature_set_id,feature_index,"
        "position_in_track) SELECT landmark_id,?2,?3,?4 FROM sparse_landmarks "
        "WHERE reconstruction_id=?1 AND track_id=?5",
        &statement);
  for (size_t index = 0; result == LARDON3D_PROJECT_DB_OK &&
                         index < publication->observation_count;
       ++index) {
    const Lardon3DSparseLandmarkObservation *ob =
        &publication->observations[index];
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)ob->feature_set_id);
    sqlite3_bind_int64(statement, 3, ob->feature_index);
    sqlite3_bind_int64(statement, 4, ob->position_in_track);
    sqlite3_bind_int64(statement, 5, (sqlite3_int64)ob->track_id);
    result = sparse_step_reuse(db, statement, "insert sparse observation");
  }
  if (statement)
    sqlite3_finalize(statement);
  if (result == LARDON3D_PROJECT_DB_OK)
    result = execute(db, "COMMIT", "commit sparse reconstruction");
  if (result != LARDON3D_PROJECT_DB_OK)
    (void)execute(db, "ROLLBACK", "rollback sparse reconstruction");
  if (result == LARDON3D_PROJECT_DB_OK)
    result = sparse_load_reconstruction_locked(db, reconstruction_id, output);
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_sparse_reconstruction_find_exact(
    Lardon3DProjectDb *db, uint64_t track_set_id, uint64_t scope_id,
    uint32_t kind, uint32_t version, const unsigned char fingerprint[32],
    Lardon3DSparseReconstruction *output) {
  if (!db || !track_set_id || !scope_id || !fingerprint || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT reconstruction_id FROM sparse_reconstructions WHERE "
              "track_set_id=?1 AND calibration_scope_id=?2 AND sfm_kind=?3 AND "
              "sfm_version=?4 AND parameter_fingerprint=?5",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)track_set_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)scope_id);
    sqlite3_bind_int64(statement, 3, kind);
    sqlite3_bind_int64(statement, 4, version);
    sqlite3_bind_blob(statement, 5, fingerprint, 32, SQLITE_TRANSIENT);
    int code = sqlite3_step(statement);
    if (code == SQLITE_DONE)
      result = LARDON3D_PROJECT_DB_NOT_FOUND;
    else if (code != SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    else
      result = sparse_load_reconstruction_locked(
          db, (uint64_t)sqlite3_column_int64(statement, 0), output);
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_reconstruction_load(Lardon3DProjectDb *db, uint64_t id,
                                    Lardon3DSparseReconstruction *output) {
  if (!db || !id || !output)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  (void)pthread_mutex_lock(&db->mutex);
  Lardon3DProjectDbResult result =
      sparse_load_reconstruction_locked(db, id, output);
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_reconstruction_list(Lardon3DProjectDb *db, uint64_t after_id,
                                    size_t capacity,
                                    Lardon3DSparseReconstructionPage *page) {
  if (!db || !page || !page->items || capacity > 64)
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  page->after_id = after_id;
  page->capacity = capacity;
  page->count = 0;
  page->next_after_id = after_id;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT reconstruction_id FROM sparse_reconstructions WHERE "
              "reconstruction_id>?1 ORDER BY reconstruction_id LIMIT ?2",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)after_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)capacity);
    while (page->count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
      uint64_t id = (uint64_t)sqlite3_column_int64(statement, 0);
      result =
          sparse_load_reconstruction_locked(db, id, &page->items[page->count]);
      if (result != LARDON3D_PROJECT_DB_OK)
        break;
      page->next_after_id = id;
      ++page->count;
    }
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

static bool sparse_page_capacity_valid(size_t capacity) {
  return capacity <= LARDON3D_SPARSE_SFM_PAGE_MAX;
}

Lardon3DProjectDbResult
lardon3d_sparse_component_list(Lardon3DProjectDb *db,
                               uint64_t reconstruction_id,
                               uint64_t after_component_key, size_t capacity,
                               Lardon3DSparseComponentPage *page) {
  if (!db || !page || !page->items || !reconstruction_id ||
      !sparse_page_capacity_valid(capacity))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  page->after_component_key = after_component_key;
  page->capacity = capacity;
  page->count = 0;
  page->next_component_key = after_component_key;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result =
      prepare(db,
              "SELECT 1 FROM sparse_reconstruction_components AS c WHERE "
              "c.reconstruction_id=?1 AND (c.component_key!=(SELECT "
              "MIN(r.image_id) FROM sparse_registered_images AS r WHERE "
              "r.reconstruction_id=c.reconstruction_id AND "
              "r.component_id=c.component_id) OR c.registered_image_count!=("
              "SELECT COUNT(*) FROM sparse_registered_images AS r2 WHERE "
              "r2.reconstruction_id=c.reconstruction_id AND "
              "r2.component_id=c.component_id) OR c.landmark_count!=(SELECT "
              "COUNT(*) FROM sparse_landmarks AS l WHERE "
              "l.reconstruction_id=c.reconstruction_id AND "
              "l.component_id=c.component_id)) LIMIT 1",
              &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    if (sqlite3_step(statement) == SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(db,
                     "SELECT component_id,component_key,registered_image_count,"
                     "landmark_count FROM sparse_reconstruction_components "
                     "WHERE reconstruction_id=?1 AND component_key>?2 "
                     "ORDER BY component_key LIMIT ?3",
                     &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_component_key);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    while (page->count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
      Lardon3DSparseComponent *item = &page->items[page->count];
      item->component_id = (uint64_t)sqlite3_column_int64(statement, 0);
      item->component_key = (uint64_t)sqlite3_column_int64(statement, 1);
      item->registered_image_count =
          (uint64_t)sqlite3_column_int64(statement, 2);
      item->landmark_count = (uint64_t)sqlite3_column_int64(statement, 3);
      page->next_component_key = item->component_key;
      ++page->count;
    }
    sqlite3_finalize(statement);
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    page->count = 0;
    page->next_component_key = after_component_key;
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_sparse_registered_image_list(
    Lardon3DProjectDb *db, uint64_t reconstruction_id, uint64_t after_image_id,
    size_t capacity, Lardon3DSparseRegisteredImagePage *page) {
  if (!db || !page || !page->items || !reconstruction_id ||
      !sparse_page_capacity_valid(capacity))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  page->after_image_id = after_image_id;
  page->capacity = capacity;
  page->count = 0;
  page->next_image_id = after_image_id;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db,
      "SELECT 1 FROM sparse_reconstruction_components AS c WHERE "
      "c.reconstruction_id=?1 AND c.component_key!=(SELECT MIN(r.image_id) "
      "FROM sparse_registered_images AS r WHERE r.reconstruction_id=?1 AND "
      "r.component_id=c.component_id) LIMIT 1",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    if (sqlite3_step(statement) == SQLITE_ROW)
      result = LARDON3D_PROJECT_DB_CORRUPT;
    sqlite3_finalize(statement);
    statement = NULL;
  }
  if (result == LARDON3D_PROJECT_DB_OK)
    result = prepare(
        db,
        "SELECT "
        "r.image_id,r.component_id,r.rotation_00,r.rotation_01,r.rotation_02,"
        "r.rotation_10,r.rotation_11,r.rotation_12,r.rotation_20,r.rotation_21,"
        "r.rotation_22,"
        "translation_0,translation_1,translation_2,c.component_key "
        "FROM sparse_registered_images AS r JOIN "
        "sparse_reconstruction_components AS c ON c.reconstruction_id="
        "r.reconstruction_id AND c.component_id=r.component_id "
        "WHERE r.reconstruction_id=?1 AND r.image_id>?2 ORDER BY r.image_id "
        "LIMIT ?3",
        &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_image_id);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    while (page->count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
      Lardon3DSparseRegisteredImage *item = &page->items[page->count];
      item->image_id = (uint64_t)sqlite3_column_int64(statement, 0);
      item->component_key = 0;
      for (int index = 0; index < 9; ++index)
        item->rotation_cw[index] = sqlite3_column_double(statement, index + 2);
      for (int index = 0; index < 3; ++index)
        item->translation_cw[index] =
            sqlite3_column_double(statement, index + 11);
      item->component_key = (uint64_t)sqlite3_column_int64(statement, 14);
      if (!sparse_rotation_valid(item->rotation_cw) ||
          !sparse_finite(item->translation_cw[0]) ||
          !sparse_finite(item->translation_cw[1]) ||
          !sparse_finite(item->translation_cw[2])) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      page->next_image_id = item->image_id;
      ++page->count;
    }
    sqlite3_finalize(statement);
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    page->count = 0;
    page->next_image_id = after_image_id;
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult
lardon3d_sparse_landmark_list(Lardon3DProjectDb *db, uint64_t reconstruction_id,
                              uint64_t after_track_id, size_t capacity,
                              Lardon3DSparseLandmarkPage *page) {
  if (!db || !page || !page->items || !reconstruction_id ||
      !sparse_page_capacity_valid(capacity))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  page->after_track_id = after_track_id;
  page->capacity = capacity;
  page->count = 0;
  page->next_track_id = after_track_id;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db,
      "SELECT l.landmark_id,l.track_id,l.component_id,l.x,l.y,l.z,"
      "l.reprojection_rmse_px,l.reprojection_median_px,l.observation_count,"
      "c.component_key,CASE WHEN c.component_key=(SELECT MIN(ri.image_id) "
      "FROM sparse_registered_images AS ri WHERE ri.reconstruction_id="
      "l.reconstruction_id AND ri.component_id=l.component_id) AND "
      "EXISTS (SELECT 1 FROM tracks AS tr WHERE tr.track_set_id=r.track_set_id "
      "AND tr.track_id=l.track_id) AND NOT EXISTS (SELECT 1 FROM "
      "track_observations AS t JOIN feature_sets AS f ON "
      "f.feature_set_id=t.feature_set_id WHERE t.track_set_id=r.track_set_id "
      "AND t.track_id=l.track_id AND NOT EXISTS (SELECT 1 FROM "
      "sparse_registered_images AS ri2 WHERE ri2.reconstruction_id="
      "l.reconstruction_id AND ri2.image_id=f.image_id AND "
      "ri2.component_id=l.component_id)) THEN 1 ELSE 0 END "
      "FROM sparse_landmarks AS l JOIN sparse_reconstructions AS r ON "
      "r.reconstruction_id=l.reconstruction_id JOIN "
      "sparse_reconstruction_components AS c ON "
      "c.reconstruction_id=l.reconstruction_id AND c.component_id=l.component_id "
      "WHERE l.reconstruction_id=?1 AND l.track_id>?2 ORDER BY l.track_id "
      "LIMIT ?3",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_track_id);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)capacity);
    while (page->count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
      Lardon3DSparseLandmark *item = &page->items[page->count];
      item->landmark_id = (uint64_t)sqlite3_column_int64(statement, 0);
      item->track_id = (uint64_t)sqlite3_column_int64(statement, 1);
      item->component_key = (uint64_t)sqlite3_column_int64(statement, 9);
      item->x = sqlite3_column_double(statement, 3);
      item->y = sqlite3_column_double(statement, 4);
      item->z = sqlite3_column_double(statement, 5);
      item->reprojection_rmse_px = sqlite3_column_double(statement, 6);
       item->reprojection_median_px = sqlite3_column_double(statement, 7);
       item->observation_count = (uint64_t)sqlite3_column_int64(statement, 8);
       if (sqlite3_column_int(statement, 10) == 0) {
         result = LARDON3D_PROJECT_DB_CORRUPT;
         break;
       }
      if (!sparse_finite(item->x) || !sparse_finite(item->y) ||
          !sparse_finite(item->z) ||
          !sparse_finite(item->reprojection_rmse_px) ||
          !sparse_finite(item->reprojection_median_px)) {
        result = LARDON3D_PROJECT_DB_CORRUPT;
        break;
      }
      page->next_track_id = item->track_id;
      ++page->count;
    }
    sqlite3_finalize(statement);
  }
  if (result != LARDON3D_PROJECT_DB_OK) {
    page->count = 0;
    page->next_track_id = after_track_id;
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}

Lardon3DProjectDbResult lardon3d_sparse_observation_list(
    Lardon3DProjectDb *db, uint64_t reconstruction_id,
    uint64_t after_landmark_id, uint32_t after_position_in_track,
    size_t capacity, Lardon3DSparseObservationPage *page) {
  if (!db || !page || !page->items || !reconstruction_id ||
      !sparse_page_capacity_valid(capacity))
    return LARDON3D_PROJECT_DB_INVALID_ARGUMENT;
  page->after_landmark_id = after_landmark_id;
  page->after_position_in_track = after_position_in_track;
  page->capacity = capacity;
  page->count = 0;
  page->next_landmark_id = after_landmark_id;
  page->next_position_in_track = after_position_in_track;
  (void)pthread_mutex_lock(&db->mutex);
  sqlite3_stmt *statement = NULL;
  Lardon3DProjectDbResult result = prepare(
      db,
      "SELECT o.landmark_id,l.track_id,o.feature_set_id,o.feature_index,"
      "o.position_in_track,CASE WHEN EXISTS (SELECT 1 FROM "
      "track_observations AS t WHERE t.track_set_id=r.track_set_id AND "
      "t.track_id=l.track_id AND t.feature_set_id=o.feature_set_id AND "
      "t.feature_index=o.feature_index AND "
      "t.position_in_track=o.position_in_track) THEN 1 ELSE 0 END "
      "FROM sparse_landmark_observations AS o JOIN sparse_landmarks AS l ON "
      "l.landmark_id=o.landmark_id JOIN sparse_reconstructions AS r ON "
      "r.reconstruction_id=l.reconstruction_id WHERE l.reconstruction_id=?1 "
      "AND (o.landmark_id>?2 OR (o.landmark_id=?2 AND "
      "o.position_in_track>?3)) ORDER BY o.landmark_id,o.position_in_track "
      "LIMIT ?4",
      &statement);
  if (result == LARDON3D_PROJECT_DB_OK) {
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)reconstruction_id);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)after_landmark_id);
    sqlite3_bind_int64(statement, 3, (sqlite3_int64)after_position_in_track);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)capacity);
    while (page->count < capacity && sqlite3_step(statement) == SQLITE_ROW) {
      Lardon3DSparseLandmarkObservation *item = &page->items[page->count];
      item->landmark_id = (uint64_t)sqlite3_column_int64(statement, 0);
      item->track_id = (uint64_t)sqlite3_column_int64(statement, 1);
       item->feature_set_id = (uint64_t)sqlite3_column_int64(statement, 2);
       item->feature_index = (uint32_t)sqlite3_column_int64(statement, 3);
       item->position_in_track = (uint32_t)sqlite3_column_int64(statement, 4);
       if (sqlite3_column_int(statement, 5) == 0) {
         result = LARDON3D_PROJECT_DB_CORRUPT;
         break;
       }
      page->next_landmark_id = item->landmark_id;
      page->next_position_in_track = item->position_in_track;
      ++page->count;
    }
    sqlite3_finalize(statement);
  }
  (void)pthread_mutex_unlock(&db->mutex);
  return result;
}
