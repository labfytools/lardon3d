#include <math.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lardon3d/calibration_tooling_v2.h>

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "calibration publication v2 failure at line %d: %s\n", \
          __LINE__, #condition); \
  return false; \
} } while (0)

enum {
  ARTIFACT_SIZE = 780,
  ARTIFACT_HEADER_SIZE = 28,
  GROUP_RECORD_SIZE = 376,
  GROUP_MEMBER_OFFSET = 232,
  MEMBER_SELECTED_INDEX_OFFSET = 0,
  MEMBER_IMAGE_ID_OFFSET = 4,
  MEMBER_FX_OFFSET = 52,
};

static bool sql(const char *path, const char *text) {
  sqlite3 *database = NULL;
  if (sqlite3_open(path, &database) != SQLITE_OK) return false;
  int result = sqlite3_exec(database, text, NULL, NULL, NULL);
  return sqlite3_close(database) == SQLITE_OK && result == SQLITE_OK;
}

static bool scalar(const char *path, const char *text, int *value) {
  sqlite3 *database = NULL;
  sqlite3_stmt *statement = NULL;
  if (sqlite3_open(path, &database) != SQLITE_OK ||
      sqlite3_prepare_v2(database, text, -1, &statement, NULL) != SQLITE_OK) {
    if (statement) sqlite3_finalize(statement);
    if (database) sqlite3_close(database);
    return false;
  }
  bool ok = sqlite3_step(statement) == SQLITE_ROW;
  if (ok) *value = sqlite3_column_int(statement, 0);
  return sqlite3_finalize(statement) == SQLITE_OK &&
         sqlite3_close(database) == SQLITE_OK && ok;
}

static bool digest(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(bytes, size, output, &output_size, EVP_sha256(), NULL) == 1 &&
         output_size == 32;
}

static void fill_digest(unsigned char output[32], unsigned char seed) {
  memset(output, seed, 32);
}

static void write_u32_le(unsigned char output[4], uint32_t value) {
  for (size_t index = 0; index < 4; ++index)
    output[index] = (unsigned char)(value >> (8u * index));
}

static void write_u64_le(unsigned char output[8], uint64_t value) {
  for (size_t index = 0; index < 8; ++index)
    output[index] = (unsigned char)(value >> (8u * index));
}

/* Synthetic evidence deliberately represents two independent optical groups;
 * it tests publication mechanics and does not claim a physical calibration. */
static void evidence_fixture(Lardon3DCalibrationToolingV2Evidence *evidence,
                             Lardon3DCalibrationToolingV2Group groups[2],
                             Lardon3DCalibrationToolingV2Entry entries[2]) {
  memset(evidence, 0, sizeof(*evidence));
  memset(groups, 0, 2 * sizeof(*groups));
  memset(entries, 0, 2 * sizeof(*entries));
  for (size_t index = 0; index < 2; ++index) {
    Lardon3DCalibrationToolingV2Group *group = &groups[index];
    Lardon3DCalibrationToolingV2Entry *entry = &entries[index];
    fill_digest(group->group_identity_sha256, (unsigned char)(1 + index));
    group->group_version = 1;
    fill_digest(group->optical_state_sha256, (unsigned char)(10 + index));
    fill_digest(group->target_sha256, (unsigned char)(20 + index));
    fill_digest(group->solver_executable_sha256, (unsigned char)(30 + index));
    fill_digest(group->solver_configuration_sha256, (unsigned char)(40 + index));
    fill_digest(group->initialization_evidence_sha256, (unsigned char)(50 + index));
    fill_digest(group->validation_evidence_sha256, (unsigned char)(60 + index));
    group->entries = entry;
    group->entry_count = 1;

    entry->selected_item_index = (uint32_t)index;
    entry->image_id = index + 1;
    fill_digest(entry->representation_sha256, (unsigned char)(0x11 * (index + 1)));
    entry->width = index == 0 ? 4000 : 6000;
    entry->height = index == 0 ? 2250 : 4000;
    entry->fx = index == 0 ? 3000.0 : 4500.0;
    entry->fy = index == 0 ? 3001.0 : 4502.0;
    entry->cx = index == 0 ? 2000.0 : 3000.0;
    entry->cy = index == 0 ? 1100.0 : 2000.0;
    entry->k1 = index == 0 ? 0.01 : 0.02;
    entry->k2 = index == 0 ? -0.01 : -0.02;
    entry->p1 = 0.001;
    entry->p2 = -0.001;
    entry->support_images = 50;
    entry->support_observations = 2000;
    entry->reprojection_rmse_px = 0.4;
    entry->maximum_parameter_delta = 0.01;
    entry->validation_flags = LARDON3D_CALIBRATION_TOOLING_V2_VALIDATION_FLAGS;
  }
  evidence->groups = groups;
  evidence->group_count = 2;
  evidence->entry_count = 2;
}

static bool validation_cases(void) {
  Lardon3DCalibrationToolingV2Evidence evidence;
  Lardon3DCalibrationToolingV2Group groups[2];
  Lardon3DCalibrationToolingV2Entry entries[2];
  evidence_fixture(&evidence, groups, entries);
  CHECK(lardon3d_calibration_tooling_v2_validate(&evidence) ==
        LARDON3D_CALIBRATION_TOOLING_V2_OK);

  evidence.group_count = 1;
  CHECK(lardon3d_calibration_tooling_v2_validate(&evidence) ==
        LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED);
  evidence.group_count = 2;
  entries[1].image_id = entries[0].image_id;
  CHECK(lardon3d_calibration_tooling_v2_validate(&evidence) ==
        LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED);
  entries[1].image_id = 2;
  Lardon3DCalibrationToolingV2Group swapped[2] = {groups[1], groups[0]};
  evidence.groups = swapped;
  CHECK(lardon3d_calibration_tooling_v2_validate(&evidence) ==
        LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED);
  evidence.groups = groups;
  entries[1].fx = NAN;
  CHECK(lardon3d_calibration_tooling_v2_validate(&evidence) ==
        LARDON3D_CALIBRATION_TOOLING_V2_EVIDENCE_REJECTED);
  return true;
}

static bool create_execution(const char *path, Lardon3DProjectDb **database,
                             Lardon3DProjectDbSelectedExecution *execution) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, database, error) == LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(*database);
  *database = NULL;
  CHECK(sql(path,
      "INSERT INTO scansets(scanset_id,name,created_at,updated_at) VALUES(1,'v2',1,1);"
      "INSERT INTO tasks VALUES(1,'quality','photo_quality.triage',1,5,5,100,1,0,0,0,0,1);"
      "INSERT INTO tasks VALUES(2,'campaign','acquisition_campaign.run',1,5,5,100,1,0,0,0,0,1);"
      "INSERT INTO photo_quality_triage_tasks VALUES(1,1,2,1,X'01');"
      "INSERT INTO photo_quality_triage_results VALUES"
      "(1,1,0,1,0,1,0,100,100,100,100,1,1,0,0,1,1,0,'GOOD'),"
      "(1,2,1,1,0,1,0,100,100,100,100,1,1,0,0,1,1,0,'GOOD');"
      "INSERT INTO acquisition_campaign_tasks VALUES(2,1,2,2,X'02');"
      "INSERT INTO captures VALUES(1,1,1),(2,1,2);"
      "INSERT INTO acquisition_campaign_captures VALUES(2,11,1),(2,12,2);"
      "INSERT INTO image_assets VALUES"
      "(1,X'1111111111111111111111111111111111111111111111111111111111111111',"
      "'assets/images/11/1111111111111111111111111111111111111111111111111111111111111111',"
      "1,1,1),"
      "(2,X'2222222222222222222222222222222222222222222222222222222222222222',"
      "'assets/images/22/2222222222222222222222222222222222222222222222222222222222222222',"
      "1,1,2);"
      "INSERT INTO images VALUES"
      "(1,1,1,'one.jpg','/one.jpg',NULL,1),"
      "(2,1,2,'two.jpg','/two.jpg',NULL,2);"
      "INSERT INTO capture_images VALUES(1,1),(2,2);"));
  CHECK(lardon3d_project_db_open(path, database, error) == LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbSelectedExecutionItem items[2] = {
      {.item_index = 0, .quality_group_id = 1, .campaign_group_id = 11,
       .capture_id = 1,
       .representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE},
      {.item_index = 1, .quality_group_id = 2, .campaign_group_id = 12,
       .capture_id = 2,
       .representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE}};
  CHECK(lardon3d_project_db_create_selected_execution(*database, 1, 2, items, 2, 10,
                                                       execution) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_record_selected_representation(
            *database, execution->execution_id, 0, 1, 1) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_record_selected_representation(
            *database, execution->execution_id, 1, 2, 2) == LARDON3D_PROJECT_DB_OK);
  return true;
}

static bool malformed_artifact_is_prepublication(
    const char *path, Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char artifact[ARTIFACT_SIZE]) {
  unsigned char artifact_sha256[32];
  Lardon3DCalibrationBootstrapV2Output output;
  Lardon3DProjectDbSelectedExecution execution;
  int count = -1;
  memset(&output, 0xa5, sizeof(output));
  CHECK(digest(artifact, ARTIFACT_SIZE, artifact_sha256));
  CHECK(lardon3d_calibration_bootstrap_v2_import(
            database, execution_id, artifact, ARTIFACT_SIZE, artifact_sha256, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT);
  const unsigned char zero_output[sizeof(output)] = {0};
  CHECK(memcmp(&output, zero_output, sizeof(output)) == 0);
  CHECK(scalar(path, "SELECT COUNT(*) FROM sparse_calibrations", &count) && count == 0);
  CHECK(scalar(path, "SELECT COUNT(*) FROM sparse_calibration_scopes", &count) && count == 0);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution_id, &execution) ==
            LARDON3D_PROJECT_DB_OK &&
        execution.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
        !execution.has_calibration_scope);
  return true;
}

static bool production_import(void) {
  char directory[] = "/tmp/lardon3d-calibration-publication-v2-XXXXXX";
  CHECK(mkdtemp(directory) != NULL);
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/project.db", directory) > 0);
  Lardon3DProjectDb *database = NULL;
  Lardon3DProjectDbSelectedExecution execution;
  CHECK(create_execution(path, &database, &execution));

  Lardon3DCalibrationToolingV2Evidence evidence;
  Lardon3DCalibrationToolingV2Group groups[2];
  Lardon3DCalibrationToolingV2Entry entries[2];
  evidence_fixture(&evidence, groups, entries);
  unsigned char artifact_a[ARTIFACT_SIZE + 1], artifact_b[ARTIFACT_SIZE];
  unsigned char hash_a[32], hash_b[32];
  size_t size_a = 0, size_b = 0;
  CHECK(lardon3d_calibration_tooling_v2_produce(
            &evidence, artifact_a, ARTIFACT_SIZE, &size_a, hash_a) ==
        LARDON3D_CALIBRATION_TOOLING_V2_OK);
  CHECK(lardon3d_calibration_tooling_v2_produce(
            &evidence, artifact_b, sizeof(artifact_b), &size_b, hash_b) ==
        LARDON3D_CALIBRATION_TOOLING_V2_OK);
  CHECK(size_a == ARTIFACT_SIZE && size_b == size_a &&
        memcmp(artifact_a, artifact_b, size_a) == 0 && memcmp(hash_a, hash_b, 32) == 0);

  /* These checksum-valid corruptions cross the production bootstrap parser
   * directly. Offsets select fields in the two canonical single-member group
   * records; they do not duplicate the parser's validation decisions. */
  unsigned char malformed[ARTIFACT_SIZE];
  const size_t first_member = ARTIFACT_HEADER_SIZE + GROUP_MEMBER_OFFSET;
  const size_t second_member =
      ARTIFACT_HEADER_SIZE + GROUP_RECORD_SIZE + GROUP_MEMBER_OFFSET;
  memcpy(malformed, artifact_b, sizeof(malformed));
  write_u64_le(malformed + first_member + MEMBER_FX_OFFSET, UINT64_C(0x7ff8000000000000));
  CHECK(malformed_artifact_is_prepublication(
      path, database, execution.execution_id, malformed));

  memcpy(malformed, artifact_b, sizeof(malformed));
  write_u32_le(malformed + second_member + MEMBER_SELECTED_INDEX_OFFSET, 0);
  CHECK(malformed_artifact_is_prepublication(
      path, database, execution.execution_id, malformed));

  memcpy(malformed, artifact_b, sizeof(malformed));
  write_u64_le(malformed + second_member + MEMBER_IMAGE_ID_OFFSET, 1);
  CHECK(malformed_artifact_is_prepublication(
      path, database, execution.execution_id, malformed));

  memcpy(malformed, artifact_b, sizeof(malformed));
  write_u32_le(malformed + second_member + MEMBER_SELECTED_INDEX_OFFSET, 2);
  CHECK(malformed_artifact_is_prepublication(
      path, database, execution.execution_id, malformed));

  Lardon3DCalibrationBootstrapV2Output output;
  unsigned char wrong_hash[32] = {0};
  CHECK(lardon3d_calibration_bootstrap_v2_import(
            database, execution.execution_id, artifact_a, size_a, wrong_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_V2_PROVENANCE_MISMATCH);
  unsigned char trailing_hash[32];
  artifact_a[size_a] = 0;
  CHECK(digest(artifact_a, size_a + 1, trailing_hash));
  CHECK(lardon3d_calibration_bootstrap_v2_import(
            database, execution.execution_id, artifact_a, size_a + 1, trailing_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT);

  unsigned char reversed[ARTIFACT_SIZE], temporary[GROUP_RECORD_SIZE], reversed_hash[32];
  memcpy(reversed, artifact_b, sizeof(reversed));
  memcpy(temporary, reversed + 28, sizeof(temporary));
  memcpy(reversed + 28, reversed + 28 + GROUP_RECORD_SIZE, GROUP_RECORD_SIZE);
  memcpy(reversed + 28 + GROUP_RECORD_SIZE, temporary, GROUP_RECORD_SIZE);
  CHECK(digest(reversed, sizeof(reversed), reversed_hash));
  CHECK(lardon3d_calibration_bootstrap_v2_import(
            database, execution.execution_id, reversed, sizeof(reversed), reversed_hash, &output) ==
        LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT);

  fill_digest(entries[1].representation_sha256, 0x33);
  CHECK(lardon3d_calibration_tooling_v2_import(
            database, execution.execution_id, &evidence, artifact_a, ARTIFACT_SIZE,
            &size_a, &output) == LARDON3D_CALIBRATION_TOOLING_V2_IMPORT_ERROR);
  fill_digest(entries[1].representation_sha256, 0x22);
  int count = -1;
  CHECK(scalar(path, "SELECT COUNT(*) FROM sparse_calibrations", &count) && count == 0);
  CHECK(scalar(path, "SELECT COUNT(*) FROM sparse_calibration_scopes", &count) && count == 0);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution.execution_id,
                                                     &execution) == LARDON3D_PROJECT_DB_OK &&
        execution.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
        !execution.has_calibration_scope);

  entries[1].image_id = 99;
  CHECK(lardon3d_calibration_tooling_v2_import(
            database, execution.execution_id, &evidence, artifact_a, ARTIFACT_SIZE,
            &size_a, &output) == LARDON3D_CALIBRATION_TOOLING_V2_IMPORT_ERROR);
  entries[1].image_id = 2;
  CHECK(scalar(path, "SELECT COUNT(*) FROM sparse_calibrations", &count) && count == 0);

  CHECK(lardon3d_calibration_tooling_v2_produce(
            &evidence, artifact_a, ARTIFACT_SIZE, &size_a, hash_a) ==
        LARDON3D_CALIBRATION_TOOLING_V2_OK);
  Lardon3DCalibrationBootstrapV2Result imported = lardon3d_calibration_bootstrap_v2_import(
      database, execution.execution_id, artifact_a, size_a, hash_a, &output);
  if (imported != LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK)
    fprintf(stderr, "valid v2 bootstrap result=%d\n", (int)imported);
  CHECK(imported == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK);
  uint64_t scope_id = output.scope.scope_id;
  CHECK(output.group_count == 2 && output.calibration_count == 2 && scope_id != 0);
  CHECK(lardon3d_calibration_tooling_v2_import(
            database, execution.execution_id, &evidence, artifact_a, ARTIFACT_SIZE,
            NULL, &output) == LARDON3D_CALIBRATION_TOOLING_V2_OK &&
        output.scope.scope_id == scope_id);

  Lardon3DSparseCalibrationMember members[2];
  size_t member_count = 0;
  uint64_t next_image_id = 0;
  CHECK(lardon3d_sparse_calibration_scope_list_members(
            database, scope_id, 0, members, 2, &member_count, &next_image_id) ==
            LARDON3D_PROJECT_DB_OK &&
        member_count == 2 && next_image_id == 2 && members[0].image_id == 1 &&
        members[1].image_id == 2);
  Lardon3DSparseCalibration calibrations[2];
  CHECK(lardon3d_sparse_calibration_load(database, members[0].calibration_id,
                                          &calibrations[0]) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_sparse_calibration_load(database, members[1].calibration_id,
                                          &calibrations[1]) == LARDON3D_PROJECT_DB_OK);
  CHECK(memcmp(calibrations[0].provenance_fingerprint,
               calibrations[1].provenance_fingerprint, 32) != 0);
  CHECK(lardon3d_project_db_load_selected_execution(database, execution.execution_id,
                                                     &execution) == LARDON3D_PROJECT_DB_OK &&
        execution.stage == LARDON3D_SELECTED_EXECUTION_READY &&
        execution.has_calibration_scope && execution.calibration_scope_id == scope_id);

  lardon3d_project_db_close(database);
  CHECK(unlink(path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) {
  return validation_cases() && production_import() ? EXIT_SUCCESS : EXIT_FAILURE;
}
