#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/project_db.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);                             \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

static bool query_integer(const char *path, const char *sql, sqlite3_int64 expected) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  if (sqlite3_open_v2(path, &connection, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
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
  return sqlite3_finalize(statement) == SQLITE_OK && sqlite3_close(connection) == SQLITE_OK &&
         matches;
}

static bool create_v9_database(const char *path) {
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(path, &database, error) != LARDON3D_PROJECT_DB_OK) return false;
  lardon3d_project_db_close(database);
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) return false;
  static const char sql[] =
      "PRAGMA foreign_keys=OFF;BEGIN IMMEDIATE;"
      "DROP TABLE matcher_tasks;"
      "DROP TABLE match_results;"
      "UPDATE metadata SET value=9 WHERE key='schema_version';COMMIT;PRAGMA foreign_keys=ON;";
  bool ok = sqlite3_exec(connection, sql, NULL, NULL, NULL) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static void asset_path_for_hash(const unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE],
                                char path[LARDON3D_PROJECT_DB_PATH_CAPACITY]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < LARDON3D_PROJECT_DB_SHA256_SIZE; ++i) {
    hex[i * 2] = digits[hash[i] >> 4];
    hex[i * 2 + 1] = digits[hash[i] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, LARDON3D_PROJECT_DB_PATH_CAPACITY, "assets/images/%c%c/%s", hex[0], hex[1],
                 hex);
}

static void feature_asset_path_for_hash(
    const unsigned char hash[LARDON3D_PROJECT_DB_SHA256_SIZE],
    char path[LARDON3D_PROJECT_DB_PATH_CAPACITY]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < LARDON3D_PROJECT_DB_SHA256_SIZE; ++i) {
    hex[i * 2] = digits[hash[i] >> 4];
    hex[i * 2 + 1] = digits[hash[i] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, LARDON3D_PROJECT_DB_PATH_CAPACITY, "assets/features/%c%c/%s", hex[0], hex[1],
                 hex);
}

static bool run_test(void) {
  char directory[] = "/tmp/lardon3d-match-result-XXXXXX";
  CHECK(mkdtemp(directory));
  char database_path[512], v9_path[512], failed_v10_path[512];
  CHECK(snprintf(database_path, sizeof(database_path), "%s/project.db", directory) > 0);
  CHECK(snprintf(v9_path, sizeof(v9_path), "%s/v9.db", directory) > 0);
  CHECK(snprintf(failed_v10_path, sizeof(failed_v10_path), "%s/failed-v10.db", directory) > 0);

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  Lardon3DProjectDb *database = NULL;
  CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(database && lardon3d_project_db_schema_version(database) == 11);

  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_project_db_create_scanset(database, "Match-test", &scanset) ==
        LARDON3D_PROJECT_DB_OK);

  unsigned char hash_a[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0xAA};
  char path_a[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(hash_a, path_a);
  Lardon3DProjectDbImageRegisterStatus identity_status;
  Lardon3DProjectDbImage image_a;
  CHECK(lardon3d_project_db_register_image(database, scanset.scanset_id, hash_a, path_a, 1,
                                           "a.jpg", "/src/a.jpg", 0, 1, &identity_status,
                                           &image_a) == LARDON3D_PROJECT_DB_OK);

  unsigned char hash_b[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0xBB};
  char path_b[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(hash_b, path_b);
  Lardon3DProjectDbImage image_b;
  CHECK(lardon3d_project_db_register_image(database, scanset.scanset_id, hash_b, path_b, 1,
                                           "b.jpg", "/src/b.jpg", 0, 2, &identity_status,
                                           &image_b) == LARDON3D_PROJECT_DB_OK);

  Lardon3DProjectDbCandidatePair pair;
  CHECK(lardon3d_project_db_create_candidate_pair(database, image_a.image_id, image_b.image_id, 10,
                                                   &pair) == LARDON3D_PROJECT_DB_OK);
  CHECK(pair.candidate_pair_id > 0);

  /* Create feature sets for foreign key references */
  unsigned char feature_hash_a[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0x11};
  char feature_path_a[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  feature_asset_path_for_hash(feature_hash_a, feature_path_a);
  Lardon3DProjectDbFeatureSet fs_a;
  CHECK(lardon3d_project_db_register_feature_set(
            database, image_a.image_id, "orb", 1, feature_hash_a, hash_a, 100, 1, 32,
            feature_hash_a, feature_path_a, 128, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 10,
            &fs_a) == LARDON3D_PROJECT_DB_OK);
  CHECK(fs_a.feature_set_id > 0 && fs_a.image_id == image_a.image_id);

  unsigned char feature_hash_b[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0x22};
  char feature_path_b[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  feature_asset_path_for_hash(feature_hash_b, feature_path_b);
  Lardon3DProjectDbFeatureSet fs_b;
  CHECK(lardon3d_project_db_register_feature_set(
            database, image_b.image_id, "sift", 1, feature_hash_b, hash_b, 100, 2, 128,
            feature_hash_b, feature_path_b, 256,
            LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE, 0, 20, &fs_b) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(fs_b.feature_set_id > 0 && fs_b.image_id == image_b.image_id);

  /* Create MATCHED result with inliers */
  unsigned char fp1[32];
  memset(fp1, 0x01, 32);
  Lardon3DProjectDbMatchResult result;
  unsigned char match_hash[32];
  memset(match_hash, 0x5A, sizeof(match_hash));
  const char *match_path = "assets/matches/5a/asset";
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100,
            &result) == LARDON3D_PROJECT_DB_OK);
  CHECK(result.match_result_id > 0 && result.candidate_pair_id == pair.candidate_pair_id &&
        result.feature_set_id_a == fs_a.feature_set_id &&
        result.feature_set_id_b == fs_b.feature_set_id &&
        strcmp(result.matcher_kind, "orb_bf") == 0 && result.matcher_version == 1 &&
        memcmp(result.parameter_fingerprint, fp1, 32) == 0 &&
        result.result_status == LARDON3D_MATCH_RESULT_STATUS_MATCHED &&
        result.match_count == 30 && result.created_at == 100);
  uint64_t first_id = result.match_result_id;

  /* Create NO_MATCH result (0 inliers) */
  unsigned char fp2[32];
  memset(fp2, 0x02, 32);
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 2, fp2, LARDON3D_MATCH_RESULT_STATUS_NO_MATCH, 0,
            NULL, NULL, 0, 200,
            &result) == LARDON3D_PROJECT_DB_OK);
  CHECK(result.result_status == LARDON3D_MATCH_RESULT_STATUS_NO_MATCH &&
        result.match_count == 0 && !result.has_match_asset);
  uint64_t second_id = result.match_result_id;

  /* Duplicate: same CP + same matcher + same params + same FS → CONSTRAINT */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 10,
            match_hash, match_path, 152, 500,
            &result) == LARDON3D_PROJECT_DB_CONSTRAINT);

  /* Create additional Feature Sets for distinct-FS and inverted-ID tests */
  unsigned char feature_hash_a2[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0x33};
  char feature_path_a2[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  feature_asset_path_for_hash(feature_hash_a2, feature_path_a2);
  Lardon3DProjectDbFeatureSet fs_a2;
  unsigned char fs_a2_fp[32];
  memset(fs_a2_fp, 0xAA, 32);
  CHECK(lardon3d_project_db_register_feature_set(
            database, image_a.image_id, "orb", 2, fs_a2_fp, hash_a, 200, 1, 32,
            feature_hash_a2, feature_path_a2, 256, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 30,
            &fs_a2) == LARDON3D_PROJECT_DB_OK);
  CHECK(fs_a2.feature_set_id > 0 && fs_a2.image_id == image_a.image_id);

  unsigned char feature_hash_b2[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0x44};
  char feature_path_b2[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  feature_asset_path_for_hash(feature_hash_b2, feature_path_b2);
  Lardon3DProjectDbFeatureSet fs_b2;
  unsigned char fs_b2_fp[32];
  memset(fs_b2_fp, 0xBB, 32);
  CHECK(lardon3d_project_db_register_feature_set(
            database, image_b.image_id, "sift", 2, fs_b2_fp, hash_b, 200, 2, 128,
            feature_hash_b2, feature_path_b2, 512,
            LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE, 0, 40, &fs_b2) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(fs_b2.feature_set_id > 0 && fs_b2.image_id == image_b.image_id);

  /* Same CP + same matcher + same params + different FS → distinct */
  unsigned char fp5[32];
  memset(fp5, 0x05, 32);
  Lardon3DProjectDbMatchResult distinct_fs_result;
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a2.feature_set_id, fs_b2.feature_set_id,
            "orb_bf", 1, fp5, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 20,
            match_hash, match_path, 272, 400,
            &distinct_fs_result) == LARDON3D_PROJECT_DB_OK);
  CHECK(distinct_fs_result.feature_set_id_a == fs_a2.feature_set_id &&
        distinct_fs_result.feature_set_id_b == fs_b2.feature_set_id);
  uint64_t distinct_id = distinct_fs_result.match_result_id;
  CHECK(distinct_id != first_id);

  /* feature_set_id_a > feature_set_id_b but correct ownership → valid */
  /* Register image_c, image_d; create FS for d first, then c → fs_c.id > fs_d.id */
  unsigned char hash_c[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0xCC};
  char path_c[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(hash_c, path_c);
  Lardon3DProjectDbImage image_c;
  CHECK(lardon3d_project_db_register_image(database, scanset.scanset_id, hash_c, path_c, 1,
                                            "c.jpg", "/src/c.jpg", 0, 3, &identity_status,
                                            &image_c) == LARDON3D_PROJECT_DB_OK);

  unsigned char hash_d[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0xDD};
  char path_d[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  asset_path_for_hash(hash_d, path_d);
  Lardon3DProjectDbImage image_d;
  CHECK(lardon3d_project_db_register_image(database, scanset.scanset_id, hash_d, path_d, 1,
                                            "d.jpg", "/src/d.jpg", 0, 4, &identity_status,
                                            &image_d) == LARDON3D_PROJECT_DB_OK);

  /* Create feature set for image_d first (lower ID), then image_c (higher ID) */
  unsigned char feature_hash_d[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0x55};
  char feature_path_d[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  feature_asset_path_for_hash(feature_hash_d, feature_path_d);
  Lardon3DProjectDbFeatureSet fs_d;
  CHECK(lardon3d_project_db_register_feature_set(
            database, image_d.image_id, "orb", 1, feature_hash_d, hash_d, 100, 1, 32,
            feature_hash_d, feature_path_d, 128, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 50,
            &fs_d) == LARDON3D_PROJECT_DB_OK);

  unsigned char feature_hash_c[LARDON3D_PROJECT_DB_SHA256_SIZE] = {0x66};
  char feature_path_c[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  feature_asset_path_for_hash(feature_hash_c, feature_path_c);
  Lardon3DProjectDbFeatureSet fs_c;
  CHECK(lardon3d_project_db_register_feature_set(
            database, image_c.image_id, "sift", 1, feature_hash_c, hash_c, 100, 2, 128,
            feature_hash_c, feature_path_c, 256,
            LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE, 0, 60, &fs_c) ==
        LARDON3D_PROJECT_DB_OK);

  /* fs_c was created after fs_d → fs_c.feature_set_id > fs_d.feature_set_id */
  CHECK(fs_c.feature_set_id > fs_d.feature_set_id);

  /* CP (image_c, image_d): image_c should come first since registered first */
  Lardon3DProjectDbCandidatePair pair_cd;
  CHECK(lardon3d_project_db_create_candidate_pair(database, image_c.image_id, image_d.image_id, 20,
                                                    &pair_cd) == LARDON3D_PROJECT_DB_OK);
  unsigned char fp6[32];
  memset(fp6, 0x06, 32);
  Lardon3DProjectDbMatchResult inverted_fs_result;
  CHECK(lardon3d_project_db_create_match_result(
            database, pair_cd.candidate_pair_id, fs_c.feature_set_id, fs_d.feature_set_id,
            "orb_bf", 1, fp6, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 15,
            match_hash, match_path, 212, 500,
            &inverted_fs_result) == LARDON3D_PROJECT_DB_OK);
  CHECK(inverted_fs_result.feature_set_id_a == fs_c.feature_set_id &&
        inverted_fs_result.feature_set_id_b == fs_d.feature_set_id);
  CHECK(fs_c.feature_set_id > fs_d.feature_set_id);

  /* FeatureSet A belonging to image B → rejected (INVALID_ARGUMENT) */
  unsigned char fp7[32];
  memset(fp7, 0x07, 32);
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_b.feature_set_id, fs_a.feature_set_id,
            "orb_bf", 1, fp7, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 5,
            match_hash, match_path, 92, 600,
            &result) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* FeatureSet B belonging to image A (not image B) → rejected */
  unsigned char fp8[32];
  memset(fp8, 0x08, 32);
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_a2.feature_set_id,
            "orb_bf", 1, fp8, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 5,
            match_hash, match_path, 92, 700,
            &result) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Load by ID */
  Lardon3DProjectDbMatchResult loaded;
  CHECK(lardon3d_project_db_load_match_result(database, first_id, &loaded) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded.match_result_id == first_id && loaded.result_status ==
        LARDON3D_MATCH_RESULT_STATUS_MATCHED && loaded.match_count == 30);

  /* Load not found */
  CHECK(lardon3d_project_db_load_match_result(database, 999999, &loaded) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);

  /* Find by full 6-part identity */
  Lardon3DProjectDbMatchResult found;
  CHECK(lardon3d_project_db_find_match_result(database, pair.candidate_pair_id,
                                               fs_a.feature_set_id, fs_b.feature_set_id,
                                               "orb_bf", 1, fp1,
                                               &found) == LARDON3D_PROJECT_DB_OK);
  CHECK(found.match_result_id == first_id && found.match_count == 30);

  /* Find with wrong feature_set_id → NOT_FOUND */
  CHECK(lardon3d_project_db_find_match_result(database, pair.candidate_pair_id,
                                               fs_a2.feature_set_id, fs_b.feature_set_id,
                                               "orb_bf", 1, fp1,
                                               &found) == LARDON3D_PROJECT_DB_NOT_FOUND);

  /* Find not found */
  unsigned char unknown_fp[32];
  memset(unknown_fp, 0xFF, 32);
  CHECK(lardon3d_project_db_find_match_result(database, pair.candidate_pair_id,
                                               fs_a.feature_set_id, fs_b.feature_set_id,
                                               "orb_bf", 1, unknown_fp,
                                               &found) == LARDON3D_PROJECT_DB_NOT_FOUND);

  /* Find with wrong candidate_pair */
  CHECK(lardon3d_project_db_find_match_result(database, 999999,
                                               fs_a.feature_set_id, fs_b.feature_set_id,
                                               "orb_bf", 1, fp1,
                                               &found) == LARDON3D_PROJECT_DB_NOT_FOUND);

  /* Pagination */
  Lardon3DProjectDbMatchResult page[LARDON3D_PROJECT_DB_MATCH_RESULT_PAGE_MAX];
  size_t page_count = 0;
  CHECK(lardon3d_project_db_list_match_results(database, 0, page, 4, &page_count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(page_count == 4 && page[0].match_result_id == first_id &&
        page[1].match_result_id == second_id && page[2].match_result_id == distinct_id);

  /* Cursor pagination */
  page_count = 0;
  CHECK(lardon3d_project_db_list_match_results(database, first_id, page, 2, &page_count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(page_count == 2 && page[0].match_result_id == second_id &&
        page[1].match_result_id == distinct_id);

  /* End of list */
  page_count = 0;
  CHECK(lardon3d_project_db_list_match_results(database, distinct_id, page, 1, &page_count) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(page_count == 1 && page[0].match_result_id > distinct_id);

  /* Capacity bound (PAGE_MAX) */
  CHECK(lardon3d_project_db_list_match_results(
            database, 0, page, LARDON3D_PROJECT_DB_MATCH_RESULT_PAGE_MAX + 1,
            &page_count) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: NULL database */
  CHECK(lardon3d_project_db_create_match_result(NULL, pair.candidate_pair_id,
            fs_a.feature_set_id, fs_b.feature_set_id, "orb_bf", 1, fp1,
            LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100,
            &result) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: 0 ids */
  CHECK(lardon3d_project_db_create_match_result(
            database, 0, fs_a.feature_set_id, fs_b.feature_set_id, "orb_bf", 1, fp1,
            LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, 0, fs_b.feature_set_id, "orb_bf", 1, fp1,
            LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: NULL output */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100, NULL) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: empty matcher_kind */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: 0 matcher_version */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 0, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: NULL fingerprint */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, NULL, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: bad status */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, fp1, 2, 30,
            match_hash, match_path, 392, 100, &result) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid result/asset combinations */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 10,
            NULL, NULL, 0, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "bad-no-match", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_NO_MATCH, 0,
            match_hash, match_path, 32, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "too-many", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 8193,
            match_hash, match_path, 98348, 100, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Invalid arguments: negative created_at */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "orb_bf", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_MATCHED, 30,
            match_hash, match_path, 392, -1, &result) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Status boundary values */
  CHECK(lardon3d_project_db_create_match_result(
            database, pair.candidate_pair_id, fs_a.feature_set_id, fs_b.feature_set_id,
            "boundary", 1, fp1, LARDON3D_MATCH_RESULT_STATUS_NO_MATCH, 0,
            NULL, NULL, 0, 500, &result) ==
        LARDON3D_PROJECT_DB_OK);

  /* Close/reopen persistence */
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(lardon3d_project_db_open(database_path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 11);

  /* Verify persistence: load previously created results */
  CHECK(lardon3d_project_db_load_match_result(database, first_id, &loaded) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded.match_result_id == first_id && loaded.result_status ==
        LARDON3D_MATCH_RESULT_STATUS_MATCHED &&
        strcmp(loaded.matcher_kind, "orb_bf") == 0 && loaded.match_count == 30);

  /* Verify persistence: find by key */
  CHECK(lardon3d_project_db_find_match_result(database, pair.candidate_pair_id,
                                               fs_a.feature_set_id, fs_b.feature_set_id,
                                               "orb_bf", 1, fp1,
                                               &found) == LARDON3D_PROJECT_DB_OK);
  CHECK(found.match_result_id == first_id);

  /* Verify persistence of distinct result */
  Lardon3DProjectDbMatchResult distinct_loaded;
  CHECK(lardon3d_project_db_load_match_result(database, distinct_id, &distinct_loaded) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(distinct_loaded.match_result_id == distinct_id &&
        distinct_loaded.feature_set_id_a == fs_a2.feature_set_id &&
        distinct_loaded.feature_set_id_b == fs_b2.feature_set_id);
  CHECK(distinct_id != first_id);

  /* Verify inverted result persisted */
  Lardon3DProjectDbMatchResult inverted_loaded;
  CHECK(lardon3d_project_db_load_match_result(database, inverted_fs_result.match_result_id,
                                               &inverted_loaded) == LARDON3D_PROJECT_DB_OK);
  CHECK(inverted_loaded.feature_set_id_a == fs_c.feature_set_id &&
        inverted_loaded.feature_set_id_b == fs_d.feature_set_id);
  CHECK(inverted_loaded.feature_set_id_a > inverted_loaded.feature_set_id_b);

  lardon3d_project_db_close(database);
  database = NULL;

  /* Migration from v9 */
  CHECK(create_v9_database(v9_path));
  CHECK(query_integer(v9_path, "SELECT value FROM metadata WHERE key='schema_version'", 9));
  CHECK(lardon3d_project_db_open(v9_path, &database, error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_schema_version(database) == 11);
  lardon3d_project_db_close(database);
  database = NULL;
  CHECK(query_integer(v9_path, "SELECT value FROM metadata WHERE key='schema_version'", 11));
  CHECK(query_integer(v9_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                      "name='match_results'", 1));

  /* Forced migration failure v10 */
  CHECK(create_v9_database(failed_v10_path));
  CHECK(setenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V10", "1", 1) == 0);
  CHECK(lardon3d_project_db_open(failed_v10_path, &database, error) ==
        LARDON3D_PROJECT_DB_IO_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_PROJECT_DB_FAIL_MIGRATION_V10") == 0);
  CHECK(query_integer(failed_v10_path, "SELECT value FROM metadata WHERE key='schema_version'", 9));
  CHECK(query_integer(failed_v10_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                      "name='match_results'", 0));
  CHECK(query_integer(failed_v10_path,
                      "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                      "name='matcher_tasks'", 0));
  CHECK(lardon3d_project_db_open(failed_v10_path, &database, error) == LARDON3D_PROJECT_DB_OK &&
        lardon3d_project_db_schema_version(database) == 11);
  lardon3d_project_db_close(database);
  database = NULL;

  CHECK(unlink(database_path) == 0);
  CHECK(unlink(v9_path) == 0);
  CHECK(unlink(failed_v10_path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) {
  return run_test() ? EXIT_SUCCESS : EXIT_FAILURE;
}
