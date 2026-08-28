#include <math.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/project_db.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);          \
      return false;                                                            \
    }                                                                          \
  } while (0)

typedef struct {
  uint64_t matched_id;
  uint64_t no_match_id;
  uint64_t second_matched_id;
  uint64_t candidate_pair_id;
  uint64_t feature_set_id_a;
  uint64_t feature_set_id_b;
} Parents;

static bool execute_sql(const char *path, const char *sql) {
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) {
    return false;
  }
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
    if (statement)
      (void)sqlite3_finalize(statement);
    if (connection)
      (void)sqlite3_close(connection);
    return false;
  }
  bool matches = sqlite3_step(statement) == SQLITE_ROW &&
                 sqlite3_column_int64(statement, 0) == expected &&
                 sqlite3_step(statement) == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK &&
         sqlite3_close(connection) == SQLITE_OK && matches;
}

static void asset_path(const unsigned char hash[32], const char *kind,
                       char path[LARDON3D_PROJECT_DB_PATH_CAPACITY]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < 32; ++index) {
    hex[index * 2] = digits[hash[index] >> 4];
    hex[index * 2 + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, LARDON3D_PROJECT_DB_PATH_CAPACITY, "assets/%s/%c%c/%s",
                 kind, hex[0], hex[1], hex);
}

static bool create_match_result(Lardon3DProjectDb *database, uint64_t pair_id,
                                uint64_t fs_a, uint64_t fs_b, uint32_t version,
                                unsigned char fingerprint_byte, int status,
                                uint32_t match_count, uint64_t *id) {
  unsigned char fingerprint[32] = {0};
  unsigned char hash[32] = {0};
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  memset(fingerprint, fingerprint_byte, sizeof(fingerprint));
  memset(hash, (unsigned char)(fingerprint_byte + 64U), sizeof(hash));
  asset_path(hash, "matches", path);
  Lardon3DProjectDbMatchResult result;
  Lardon3DProjectDbResult db_result = lardon3d_project_db_create_match_result(
      database, pair_id, fs_a, fs_b, "model-test", version, fingerprint, status,
      match_count, status == LARDON3D_MATCH_RESULT_STATUS_MATCHED ? hash : NULL,
      status == LARDON3D_MATCH_RESULT_STATUS_MATCHED ? path : NULL,
      status == LARDON3D_MATCH_RESULT_STATUS_MATCHED
          ? 32U + (uint64_t)match_count * 12U
          : 0,
      version, &result);
  if (db_result != LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  *id = result.match_result_id;
  return true;
}

static bool create_parents(Lardon3DProjectDb *database, Parents *parents) {
  Lardon3DProjectDbScanSet scanset;
  if (lardon3d_project_db_create_scanset(database, "Geometry-model",
                                         &scanset) != LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  unsigned char image_hash_a[32] = {0x11};
  unsigned char image_hash_b[32] = {0x22};
  char image_path_a[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  char image_path_b[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path(image_hash_a, "images", image_path_a);
  asset_path(image_hash_b, "images", image_path_b);
  Lardon3DProjectDbImageRegisterStatus register_status;
  Lardon3DProjectDbImage image_a;
  Lardon3DProjectDbImage image_b;
  if (lardon3d_project_db_register_image(
          database, scanset.scanset_id, image_hash_a, image_path_a, 10, "a.jpg",
          "/a.jpg", 0, 1, &register_status,
          &image_a) != LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_register_image(
          database, scanset.scanset_id, image_hash_b, image_path_b, 10, "b.jpg",
          "/b.jpg", 0, 2, &register_status,
          &image_b) != LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  Lardon3DProjectDbCandidatePair pair;
  if (lardon3d_project_db_create_candidate_pair(database, image_a.image_id,
                                                image_b.image_id, 3, &pair) !=
      LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  unsigned char feature_hash_a[32] = {0x33};
  unsigned char feature_hash_b[32] = {0x44};
  char feature_path_a[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  char feature_path_b[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path(feature_hash_a, "features", feature_path_a);
  asset_path(feature_hash_b, "features", feature_path_b);
  Lardon3DProjectDbFeatureSet feature_a;
  Lardon3DProjectDbFeatureSet feature_b;
  if (lardon3d_project_db_register_feature_set(
          database, image_a.image_id, "orb", 1, feature_hash_a, image_hash_a,
          8192, 1, 32, feature_hash_a, feature_path_a, 100,
          LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 4,
          &feature_a) != LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_register_feature_set(
          database, image_b.image_id, "orb", 1, feature_hash_b, image_hash_b,
          8192, 1, 32, feature_hash_b, feature_path_b, 100,
          LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 5,
          &feature_b) != LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  parents->candidate_pair_id = pair.candidate_pair_id;
  parents->feature_set_id_a = feature_a.feature_set_id;
  parents->feature_set_id_b = feature_b.feature_set_id;
  return create_match_result(database, pair.candidate_pair_id,
                             feature_a.feature_set_id, feature_b.feature_set_id,
                             1, 0x51, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 9,
                             &parents->matched_id) &&
         create_match_result(database, pair.candidate_pair_id,
                             feature_a.feature_set_id, feature_b.feature_set_id,
                             2, 0x52, LARDON3D_MATCH_RESULT_STATUS_NO_MATCH, 0,
                             &parents->no_match_id) &&
         create_match_result(database, pair.candidate_pair_id,
                             feature_a.feature_set_id, feature_b.feature_set_id,
                             3, 0x53, LARDON3D_MATCH_RESULT_STATUS_MATCHED,
                             8192, &parents->second_matched_id);
}

static bool create_v11_database(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) !=
      LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  lardon3d_project_db_close(database);
  return execute_sql(path,
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
                     "DROP TABLE track_observations;"
                     "DROP TABLE tracks;"
                     "DROP TABLE track_sets;DROP TABLE track_builder_tasks;"
                     "UPDATE metadata SET value=11 WHERE key='schema_version';"
                     "COMMIT;PRAGMA foreign_keys=ON;");
}

static bool test_model_api(const char *path) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *database = NULL;
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 20);
  Parents parents;
  CHECK(create_parents(database, &parents));

  unsigned char fingerprint_a[32];
  unsigned char fingerprint_b[32];
  memset(fingerprint_a, 0xa1, sizeof(fingerprint_a));
  memset(fingerprint_b, 0xb2, sizeof(fingerprint_b));
  unsigned char mask_a[2] = {0x01, 0x00};
  double model[9] = {0.0, -1.0, 1e-300, 1e300, 4.0, 5.0, 6.0, 7.0, 8.0};
  Lardon3DProjectDbGeometricVerificationResult verified;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint_a,
            LARDON3D_GEOMETRIC_VERIFIED, 1, mask_a, sizeof(mask_a), model, 10,
            &verified) == LARDON3D_PROJECT_DB_OK);
  CHECK(verified.has_model && verified.inlier_mask_size == 2 &&
        memcmp(verified.model, model, sizeof(model)) == 0);

  unsigned char rejected_mask[2] = {0x03, 0x00};
  Lardon3DProjectDbGeometricVerificationResult rejected;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 2, fingerprint_b,
            LARDON3D_GEOMETRIC_REJECTED, 2, rejected_mask,
            sizeof(rejected_mask), NULL, 11,
            &rejected) == LARDON3D_PROJECT_DB_OK);
  CHECK(!rejected.has_model && rejected.inlier_count == 2);

  unsigned char zero_mask[2] = {0};
  Lardon3DProjectDbGeometricVerificationResult zero_inliers;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_b,
            LARDON3D_GEOMETRIC_REJECTED, 0, zero_mask, sizeof(zero_mask), NULL,
            11, &zero_inliers) == LARDON3D_PROJECT_DB_OK);

  Lardon3DProjectDbGeometricVerificationResult loaded;
  CHECK(lardon3d_project_db_load_geometric_verification_result(
            database, verified.geometric_verification_result_id, &loaded) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded.match_result_id == parents.matched_id && loaded.has_model &&
        memcmp(loaded.model, model, sizeof(model)) == 0 &&
        memcmp(loaded.inlier_mask, mask_a, sizeof(mask_a)) == 0);
  CHECK(lardon3d_project_db_find_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint_a,
            &loaded) == LARDON3D_PROJECT_DB_OK &&
        loaded.geometric_verification_result_id ==
            verified.geometric_verification_result_id);

  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 2, rejected_mask,
            sizeof(rejected_mask), NULL, 12,
            &loaded) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.no_match_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 0, mask_a, 1, NULL, 12,
            &loaded) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, UINT64_C(9223372036854775807),
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 0, mask_a, 1, NULL, 12,
            &loaded) == LARDON3D_PROJECT_DB_NOT_FOUND);

  unsigned char bad_padding[2] = {0x01, 0x80};
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 1, bad_padding, sizeof(bad_padding),
            NULL, 12, &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 2, mask_a, sizeof(mask_a), NULL, 12,
            &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  unsigned char all_inliers[2] = {0xff, 0x01};
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 4, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 9, all_inliers, sizeof(all_inliers),
            NULL, 12, &loaded) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 5, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 10, all_inliers, sizeof(all_inliers),
            NULL, 12, &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_a,
            LARDON3D_GEOMETRIC_VERIFIED, 1, mask_a, sizeof(mask_a), NULL, 12,
            &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 1, mask_a, sizeof(mask_a), model, 12,
            &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  double bad_model[9] = {0};
  bad_model[4] = NAN;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_a,
            LARDON3D_GEOMETRIC_VERIFIED, 1, mask_a, sizeof(mask_a), bad_model,
            12, &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  bad_model[4] = INFINITY;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 3, fingerprint_a,
            LARDON3D_GEOMETRIC_VERIFIED, 1, mask_a, sizeof(mask_a), bad_model,
            12, &loaded) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  unsigned char max_mask[LARDON3D_PROJECT_DB_INLIER_MASK_MAX] = {0};
  max_mask[0] = 0x01;
  max_mask[1023] = 0x80;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.second_matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint_a,
            LARDON3D_GEOMETRIC_REJECTED, 2, max_mask, sizeof(max_mask), NULL,
            13, &loaded) == LARDON3D_PROJECT_DB_OK);
  CHECK(loaded.inlier_mask_size == 1024 && loaded.inlier_count == 2);

  static const uint32_t boundary_counts[] = {1, 2, 7, 8, 9, 63, 64, 65, 8191};
  for (size_t boundary_index = 0;
       boundary_index < sizeof(boundary_counts) / sizeof(boundary_counts[0]);
       ++boundary_index) {
    uint32_t match_count = boundary_counts[boundary_index];
    uint64_t parent_id = 0;
    CHECK(create_match_result(
        database, parents.candidate_pair_id, parents.feature_set_id_a,
        parents.feature_set_id_b, (uint32_t)(10U + boundary_index),
        (unsigned char)(0x60U + boundary_index),
        LARDON3D_MATCH_RESULT_STATUS_MATCHED, match_count, &parent_id));
    unsigned char boundary_mask[LARDON3D_PROJECT_DB_INLIER_MASK_MAX] = {0};
    size_t boundary_size = ((size_t)match_count + 7U) / 8U;
    uint32_t last_index = match_count - 1U;
    boundary_mask[last_index / 8U] = (unsigned char)(1U << (last_index % 8U));
    unsigned char boundary_fingerprint[32];
    memset(boundary_fingerprint, (int)(0xc0U + boundary_index),
           sizeof(boundary_fingerprint));
    CHECK(lardon3d_project_db_create_geometric_verification_result(
              database, parent_id, LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1,
              boundary_fingerprint, LARDON3D_GEOMETRIC_REJECTED, 1,
              boundary_mask, boundary_size, NULL, 20 + (int64_t)boundary_index,
              &loaded) == LARDON3D_PROJECT_DB_OK);
    CHECK(loaded.inlier_mask_size == boundary_size &&
          loaded.inlier_mask[last_index / 8U] ==
              (unsigned char)(1U << (last_index % 8U)));
  }

  Lardon3DProjectDbGeometricVerificationResult page[2];
  size_t count = 0;
  CHECK(lardon3d_project_db_list_geometric_verification_results(
            database, parents.matched_id, 0, page, 1, &count) ==
            LARDON3D_PROJECT_DB_OK &&
        count == 1);
  uint64_t cursor = page[0].geometric_verification_result_id;
  CHECK(lardon3d_project_db_list_geometric_verification_results(
            database, parents.matched_id, cursor, page, 2, &count) ==
            LARDON3D_PROJECT_DB_OK &&
        count == 2 && page[0].geometric_verification_result_id > cursor);
  CHECK(lardon3d_project_db_list_geometric_verification_results(
            database, parents.matched_id, 0, page,
            LARDON3D_PROJECT_DB_GEOMETRIC_RESULT_PAGE_MAX + 1U,
            &count) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  uint64_t verified_id = verified.geometric_verification_result_id;
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_geometric_verification_result(
            database, verified_id, &loaded) == LARDON3D_PROJECT_DB_OK &&
        memcmp(loaded.model, model, sizeof(model)) == 0);
  lardon3d_project_db_close(database);
  return true;
}

static bool loader_rejects_after_sql(const char *path, const char *sql) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *database = NULL;
  if (!execute_sql(path, sql) ||
      lardon3d_project_db_open(path, &database, error) !=
          LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  Lardon3DProjectDbGeometricVerificationResult result;
  bool rejected = lardon3d_project_db_load_geometric_verification_result(
                      database, 1, &result) == LARDON3D_PROJECT_DB_CORRUPT;
  lardon3d_project_db_close(database);
  return rejected;
}

static bool test_corruption(const char *path) {
  static const char reset[] =
      "PRAGMA ignore_check_constraints=ON;"
      "UPDATE geometric_verification_results SET "
      "verifier_kind=1,verifier_version=1,"
      "parameter_fingerprint=zeroblob(32),status=2,inlier_count=1,inlier_mask="
      "x'0100',"
      "model_m00=0,model_m01=0,model_m02=0,model_m10=0,model_m11=0,model_m12=0,"
      "model_m20=0,model_m21=0,model_m22=0 WHERE "
      "geometric_verification_result_id=1;";
  CHECK(execute_sql(path, reset));

  static const char *corruptions[] = {
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET verifier_kind=9 WHERE geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET verifier_kind=1,verifier_version=0 WHERE "
      "geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET verifier_version=1,parameter_fingerprint=x'01' "
      "WHERE geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET parameter_fingerprint=zeroblob(32),status=9 "
      "WHERE geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET status=2,inlier_count=9000 WHERE "
      "geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET inlier_count=1,inlier_mask=x'03' WHERE "
      "geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET inlier_mask=x'0180' WHERE geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET inlier_mask=x'0300' WHERE geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET inlier_mask=x'0100',model_m00=NULL WHERE "
      "geometric_verification_result_id=1;",
      "PRAGMA ignore_check_constraints=ON;UPDATE "
      "geometric_verification_results "
      "SET model_m00=1e999 WHERE geometric_verification_result_id=1;",
  };
  for (size_t index = 0; index < sizeof(corruptions) / sizeof(corruptions[0]);
       ++index) {
    CHECK(execute_sql(path, reset));
    CHECK(loader_rejects_after_sql(path, corruptions[index]));
  }

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *database = NULL;
  CHECK(execute_sql(path, reset));
  CHECK(lardon3d_project_db_open(path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbGeometricVerificationResult result;
  CHECK(lardon3d_project_db_load_geometric_verification_result(
            database, 1, &result) == LARDON3D_PROJECT_DB_OK);
  uint64_t parent_id = result.match_result_id;
  lardon3d_project_db_close(database);
  char parent_corruption[512];
  CHECK(snprintf(parent_corruption, sizeof(parent_corruption),
                 "PRAGMA ignore_check_constraints=ON;UPDATE match_results SET "
                 "result_status=0 "
                 "WHERE match_result_id=%llu;",
                 (unsigned long long)parent_id) > 0);
  CHECK(loader_rejects_after_sql(path, parent_corruption));
  return true;
}

static bool test_migration(const char *v11_path, const char *failed_path) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *database = NULL;
  CHECK(create_v11_database(v11_path));
  CHECK(query_integer(
      v11_path, "SELECT value FROM metadata WHERE key='schema_version'", 11));
  CHECK(
      query_integer(v11_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='geometric_verification_results'",
                    0));
  CHECK(lardon3d_project_db_open(v11_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 20);
  Parents parents;
  CHECK(create_parents(database, &parents));
  unsigned char fingerprint[32] = {0x91};
  unsigned char mask[2] = {0x01, 0x00};
  Lardon3DProjectDbGeometricVerificationResult migrated_result;
  CHECK(lardon3d_project_db_create_geometric_verification_result(
            database, parents.matched_id,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, 1, fingerprint,
            LARDON3D_GEOMETRIC_REJECTED, 1, mask, sizeof(mask), NULL, 30,
            &migrated_result) == LARDON3D_PROJECT_DB_OK);
  uint64_t migrated_result_id =
      migrated_result.geometric_verification_result_id;
  lardon3d_project_db_close(database);
  CHECK(query_integer(
      v11_path, "SELECT value FROM metadata WHERE key='schema_version'", 20));
  CHECK(
      query_integer(v11_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='index' AND "
                    "name='geometric_verification_results_parent_idx'",
                    1));
  CHECK(lardon3d_project_db_open(v11_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_geometric_verification_result(
            database, migrated_result_id, &migrated_result) ==
            LARDON3D_PROJECT_DB_OK &&
        migrated_result.match_result_id == parents.matched_id);
  lardon3d_project_db_close(database);

  CHECK(create_v11_database(failed_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V12", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V12") == 0);
  CHECK(query_integer(failed_path,
                      "SELECT value FROM metadata WHERE key='schema_version'",
                      11));
  CHECK(
      query_integer(failed_path,
                    "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                    "name='geometric_verification_results'",
                    0));
  CHECK(lardon3d_project_db_open(failed_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 20);
  lardon3d_project_db_close(database);
  return true;
}

static bool run_test(void) {
  char directory[] = "/tmp/lardon3d-geometric-model-XXXXXX";
  CHECK(mkdtemp(directory));
  char database_path[512];
  char v11_path[512];
  char failed_path[512];
  CHECK(snprintf(database_path, sizeof(database_path), "%s/project.db",
                 directory) > 0);
  CHECK(snprintf(v11_path, sizeof(v11_path), "%s/v11.db", directory) > 0);
  CHECK(snprintf(failed_path, sizeof(failed_path), "%s/failed-v12.db",
                 directory) > 0);
  CHECK(test_model_api(database_path));
  CHECK(test_corruption(database_path));
  CHECK(test_migration(v11_path, failed_path));
  CHECK(unlink(database_path) == 0);
  CHECK(unlink(v11_path) == 0);
  CHECK(unlink(failed_path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) { return run_test() ? EXIT_SUCCESS : EXIT_FAILURE; }
