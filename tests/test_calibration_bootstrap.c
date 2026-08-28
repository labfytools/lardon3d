#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/calibration_bootstrap.h>

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "calibration bootstrap failure at line %d: %s\n", __LINE__, #condition); \
  return false; } } while (0)

enum { ARTIFACT_SIZE = 292 };

static bool sql(const char *path, const char *text) {
  sqlite3 *connection = NULL;
  if (sqlite3_open(path, &connection) != SQLITE_OK) return false;
  int code = sqlite3_exec(connection, text, NULL, NULL, NULL);
  return sqlite3_close(connection) == SQLITE_OK && code == SQLITE_OK;
}

static void put_u32(unsigned char *bytes, size_t *offset, uint32_t value) {
  for (size_t index = 0; index < 4; ++index) bytes[(*offset)++] = (unsigned char)(value >> (8 * index));
}

static void put_u64(unsigned char *bytes, size_t *offset, uint64_t value) {
  for (size_t index = 0; index < 8; ++index) bytes[(*offset)++] = (unsigned char)(value >> (8 * index));
}

static void put_f64(unsigned char *bytes, size_t *offset, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  put_u64(bytes, offset, bits);
}

static bool digest(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(bytes, size, output, &output_size, EVP_sha256(), NULL) == 1 &&
         output_size == 32;
}

static void artifact(unsigned char bytes[ARTIFACT_SIZE]) {
  memset(bytes, 0, ARTIFACT_SIZE);
  size_t offset = 0;
  memcpy(bytes + offset, "L3DCALB1", 8); offset += 8;
  put_u32(bytes, &offset, 1); put_u32(bytes, &offset, 1);
  put_u32(bytes, &offset, 1); put_u32(bytes, &offset, 1);
  /* Four distinct nonzero evidence digests stand for executable,
   * configuration, initialization and validation evidence. */
  for (unsigned int evidence = 1; evidence <= 4; ++evidence) {
    memset(bytes + offset, (int)evidence, 32);
    offset += 32;
  }
  put_u64(bytes, &offset, 1);
  memset(bytes + offset, 0x11, 32); offset += 32;
  put_u32(bytes, &offset, 4000); put_u32(bytes, &offset, 2250);
  put_f64(bytes, &offset, 3072.6); put_f64(bytes, &offset, 3053.54);
  put_f64(bytes, &offset, 1969.56); put_f64(bytes, &offset, 1118.52);
  put_f64(bytes, &offset, 0.0452928); put_f64(bytes, &offset, -0.0377292);
  put_f64(bytes, &offset, 0.00479862); put_f64(bytes, &offset, 0.00672575);
  put_u32(bytes, &offset, 56); put_u32(bytes, &offset, 1000);
  put_f64(bytes, &offset, 0.4); put_f64(bytes, &offset, 0.000001);
  put_u32(bytes, &offset, 15);
}

static bool run(void) {
  char directory[] = "/tmp/lardon3d-calibration-bootstrap-XXXXXX";
  CHECK(mkdtemp(directory) != NULL);
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/project.db", directory) > 0);
  Lardon3DProjectDb *database = NULL;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(database);
  /* This fixture establishes the already-frozen operational mappings. The
   * importer may validate them but must never infer them from equal IDs. */
  CHECK(sql(path,
      "INSERT INTO scansets(scanset_id,name,created_at,updated_at) VALUES(1,'scope',1,1);"
      "INSERT INTO tasks VALUES(1,'quality','photo_quality.triage',1,5,5,100,1,0,0,0,0,1);"
      "INSERT INTO tasks VALUES(2,'campaign','acquisition_campaign.run',1,5,5,100,1,0,0,0,0,1);"
      "INSERT INTO photo_quality_triage_tasks VALUES(1,1,2,1,X'01');"
      "INSERT INTO photo_quality_triage_results VALUES(1,1,0,1,0,1,0,100,100,100,100,"
      "1.0,1.0,0.0,0.0,1.0,1.0,0.0,'GOOD');"
      "INSERT INTO acquisition_campaign_tasks VALUES(2,1,2,2,X'02');"
      "INSERT INTO captures(capture_id,scanset_id,created_at) VALUES(1,1,1);"
      "INSERT INTO acquisition_campaign_captures VALUES(2,2,1);"
      "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,created_at) VALUES"
      "(1,X'1111111111111111111111111111111111111111111111111111111111111111',"
      "'assets/images/11/1111111111111111111111111111111111111111111111111111111111111111',"
      "1,1,1);"
      "INSERT INTO images(image_id,scanset_id,asset_id,original_name,source_path,imported_at) "
      "VALUES(1,1,1,'selected.jpg','/source/selected.jpg',1);"
      "INSERT INTO capture_images VALUES(1,1);"));
  CHECK(lardon3d_project_db_open(path, &database, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbSelectedExecutionItem item = {
      .item_index = 0,
      .quality_group_id = 1,
      .campaign_group_id = 2,
      .capture_id = 1,
      .representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE};
  Lardon3DProjectDbSelectedExecution execution;
  CHECK(lardon3d_project_db_create_selected_execution(database, 1, 2, &item, 1, 10,
                                                       &execution) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_record_selected_representation(database, execution.execution_id,
                                                            0, 1, 1) == LARDON3D_PROJECT_DB_OK);

  unsigned char bytes[ARTIFACT_SIZE], hash[32], wrong_hash[32] = {0};
  artifact(bytes);
  CHECK(digest(bytes, sizeof(bytes), hash));
  Lardon3DCalibrationBootstrapOutput output;
  CHECK(lardon3d_calibration_bootstrap_import(database, execution.execution_id, bytes,
          sizeof(bytes), wrong_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_PROVENANCE_MISMATCH);

  bytes[160] ^= 1u;
  CHECK(digest(bytes, sizeof(bytes), wrong_hash));
  CHECK(lardon3d_calibration_bootstrap_import(database, execution.execution_id, bytes,
          sizeof(bytes), wrong_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_SELECTION_CONFLICT);
  artifact(bytes);
  bytes[8] = 2;
  CHECK(digest(bytes, sizeof(bytes), wrong_hash));
  CHECK(lardon3d_calibration_bootstrap_import(database, execution.execution_id, bytes,
          sizeof(bytes), wrong_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT);
  artifact(bytes);
  bytes[ARTIFACT_SIZE - 4] = 14;
  CHECK(digest(bytes, sizeof(bytes), wrong_hash));
  CHECK(lardon3d_calibration_bootstrap_import(database, execution.execution_id, bytes,
          sizeof(bytes), wrong_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_MALFORMED_ARTIFACT);

  artifact(bytes);
  Lardon3DCalibrationBootstrapResult imported = lardon3d_calibration_bootstrap_import(
      database, execution.execution_id, bytes, sizeof(bytes), hash, &output);
  if (imported != LARDON3D_CALIBRATION_BOOTSTRAP_OK)
    fprintf(stderr, "valid artifact import result=%d\n", (int)imported);
  CHECK(imported == LARDON3D_CALIBRATION_BOOTSTRAP_OK);
  CHECK(output.calibration_count == 1 && output.scope.scope_id > 0 &&
        memcmp(output.artifact_sha256, hash, 32) == 0);
  Lardon3DSparseCalibrationMember member;
  size_t count = 0;
  uint64_t next = 0;
  CHECK(lardon3d_sparse_calibration_scope_list_members(database, output.scope.scope_id, 0,
          &member, 1, &count, &next) == LARDON3D_PROJECT_DB_OK && count == 1 &&
        member.image_id == 1);
  Lardon3DSparseCalibration calibration;
  CHECK(lardon3d_sparse_calibration_load(database, member.calibration_id, &calibration) ==
        LARDON3D_PROJECT_DB_OK &&
        calibration.provenance_kind == LARDON3D_SPARSE_SFM_PROVENANCE_IMPORTED_TRUSTED &&
        memcmp(calibration.provenance_fingerprint, hash, 32) == 0);
  CHECK(lardon3d_calibration_bootstrap_import(database, execution.execution_id, bytes,
          sizeof(bytes), hash, &output) == LARDON3D_CALIBRATION_BOOTSTRAP_OK);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution.execution_id,
          &execution) == LARDON3D_PROJECT_DB_OK &&
        execution.stage == LARDON3D_SELECTED_EXECUTION_READY &&
        execution.calibration_scope_id == output.scope.scope_id);

  lardon3d_project_db_close(database);
  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) { return run() ? EXIT_SUCCESS : EXIT_FAILURE; }
