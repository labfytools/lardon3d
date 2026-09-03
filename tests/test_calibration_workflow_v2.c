#include <lardon3d/calibration_tooling_v2.h>
#include <lardon3d/calibration_workflow_v2.h>
#include <lardon3d/optical_profiles.h>

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,       \
              #condition);                                                     \
      return false;                                                            \
    }                                                                          \
  } while (0)

static bool sql(const char *path, const char *text) {
  sqlite3 *database = NULL;
  char *error = NULL;
  if (sqlite3_open(path, &database) != SQLITE_OK)
    return false;
  int code = sqlite3_exec(database, text, NULL, NULL, &error);
  if (code != SQLITE_OK)
    fprintf(stderr, "SQL: %s\n", error ? error : "error");
  sqlite3_free(error);
  sqlite3_close(database);
  return code == SQLITE_OK;
}

static void digest(unsigned char output[32], unsigned char seed) {
  for (size_t index = 0; index < 32; ++index)
    output[index] = (unsigned char)(seed + index);
}

static void evidence(Lardon3DCalibrationToolingV2Evidence *value,
                     Lardon3DCalibrationToolingV2Group groups[2],
                     Lardon3DCalibrationToolingV2Entry entries[2]) {
  memset(value, 0, sizeof(*value));
  memset(groups, 0, 2 * sizeof(*groups));
  memset(entries, 0, 2 * sizeof(*entries));
  for (size_t index = 0; index < 2; ++index) {
    digest(groups[index].group_identity_sha256, (unsigned char)(1 + index));
    groups[index].group_version = 1;
    digest(groups[index].optical_state_sha256, (unsigned char)(11 + index));
    digest(groups[index].target_sha256, (unsigned char)(21 + index));
    digest(groups[index].solver_executable_sha256, (unsigned char)(31 + index));
    digest(groups[index].solver_configuration_sha256,
           (unsigned char)(41 + index));
    digest(groups[index].initialization_evidence_sha256,
           (unsigned char)(51 + index));
    digest(groups[index].validation_evidence_sha256,
           (unsigned char)(61 + index));
    groups[index].entries = &entries[index];
    groups[index].entry_count = 1;
    entries[index].selected_item_index = (uint32_t)index;
    entries[index].image_id = index + 1;
    memset(entries[index].representation_sha256, (int)(0x11 * (index + 1)), 32);
    entries[index].width = index == 0 ? 4000 : 6000;
    entries[index].height = index == 0 ? 3000 : 4000;
    entries[index].fx = index == 0 ? 3000.0 : 4500.0;
    entries[index].fy = entries[index].fx + 1.0;
    entries[index].cx = entries[index].width / 2.0;
    entries[index].cy = entries[index].height / 2.0;
    entries[index].support_images = 20;
    entries[index].support_observations = 1000;
    entries[index].reprojection_rmse_px = 0.4;
    entries[index].maximum_parameter_delta = 0.01;
    entries[index].validation_flags =
        LARDON3D_CALIBRATION_TOOLING_V2_VALIDATION_FLAGS;
  }
  value->groups = groups;
  value->group_count = 2;
  value->entry_count = 2;
}

static bool create_project(const char *path, Lardon3DProjectDb **database,
                           uint64_t *execution_id) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(path, database, error) ==
        LARDON3D_PROJECT_DB_OK);
  lardon3d_project_db_close(*database);
  *database = NULL;
  CHECK(sql(
      path,
      "INSERT INTO scansets VALUES(1,'workflow-v2',1,1);"
      "INSERT INTO tasks "
      "VALUES(1,'quality','photo_quality.triage',1,5,5,1,1,0,0,0,0,1);"
      "INSERT INTO tasks "
      "VALUES(2,'campaign','acquisition_campaign.run',1,5,5,1,1,0,0,0,0,1);"
      "INSERT INTO photo_quality_triage_tasks VALUES(1,1,2,1,X'01');"
      "INSERT INTO photo_quality_triage_results VALUES"
      "(1,1,0,1,0,1,0,1,1,1,1,1,1,0,0,1,1,0,'GOOD'),"
      "(1,2,1,1,0,1,0,1,1,1,1,1,1,0,0,1,1,0,'GOOD');"
      "INSERT INTO acquisition_campaign_tasks VALUES(2,1,2,2,X'02');"
      "INSERT INTO captures VALUES(1,1,1),(2,1,2);"
      "INSERT INTO acquisition_campaign_captures VALUES(2,11,1),(2,12,2);"
      "INSERT INTO image_assets VALUES"
      "(1,X'1111111111111111111111111111111111111111111111111111111111111111',"
      "'assets/images/11/"
      "1111111111111111111111111111111111111111111111111111111111111111',1,1,1)"
      ","
      "(2,X'2222222222222222222222222222222222222222222222222222222222222222',"
      "'assets/images/22/"
      "2222222222222222222222222222222222222222222222222222222222222222',1,1,2)"
      ";"
      "INSERT INTO images VALUES(1,1,1,'a','a',NULL,1),(2,1,2,'b','b',NULL,2);"
      "INSERT INTO capture_images VALUES(1,1),(2,2);"
      "INSERT INTO camera_body_profiles VALUES(1,'Maker','Body','Body');"
      "INSERT INTO lens_profiles VALUES(1,'Maker','L1','L1',2,2,24000,24000);"
      "INSERT INTO lens_profiles VALUES(2,'Maker','L2','L2',2,2,50000,50000);"
      "INSERT INTO optical_configurations VALUES(1,1,1,24000),(2,1,2,50000);"
      "INSERT INTO capture_optical_configurations "
      "VALUES(1,1,2,NULL,NULL),(2,2,2,NULL,NULL);"));
  CHECK(lardon3d_project_db_open(path, database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbSelectedExecutionItem items[2] = {
      {.item_index = 0,
       .quality_group_id = 1,
       .campaign_group_id = 11,
       .capture_id = 1,
       .representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE},
      {.item_index = 1,
       .quality_group_id = 2,
       .campaign_group_id = 12,
       .capture_id = 2,
       .representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE}};
  Lardon3DProjectDbSelectedExecution execution;
  CHECK(lardon3d_project_db_create_selected_execution(*database, 1, 2, items, 2,
                                                      1, &execution) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_record_selected_representation(
            *database, execution.execution_id, 0, 1, 1) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_record_selected_representation(
            *database, execution.execution_id, 1, 2, 2) ==
        LARDON3D_PROJECT_DB_OK);
  *execution_id = execution.execution_id;
  return true;
}

static bool add_state(Lardon3DProjectDb *database, uint64_t capture_id,
                      uint64_t configuration_id, uint32_t width,
                      uint32_t height) {
  Lardon3DOpticalCaptureGeometricState input = {
      .capture_id = capture_id,
      .optical_configuration_id = configuration_id,
      .state_version = 1,
      .provenance = LARDON3D_OPTICAL_GEOMETRIC_STATE_CALLER_EXPLICIT,
      .focus_state = LARDON3D_OPTICAL_OBSERVATION_OBSERVED,
      .aperture_state = LARDON3D_OPTICAL_OBSERVATION_OBSERVED,
      .aperture_x1000 = 8000,
      .stabilization = LARDON3D_OPTICAL_STABILIZATION_OFF,
      .crop_state = LARDON3D_OPTICAL_OBSERVATION_OBSERVED,
      .pipeline_state = LARDON3D_OPTICAL_OBSERVATION_OBSERVED,
      .representation_state = LARDON3D_OPTICAL_OBSERVATION_OBSERVED,
      .decoded_geometry_state = LARDON3D_OPTICAL_OBSERVATION_OBSERVED,
      .decoded_width = width,
      .decoded_height = height};
  strcpy(input.focus_observation, "fixed");
  strcpy(input.crop_observation, "full");
  strcpy(input.pipeline_observation, "raw-v1");
  strcpy(input.representation_observation, "png-v1");
  Lardon3DOpticalCaptureGeometricState output;
  return lardon3d_optical_capture_geometric_state_create(
             database, &input, &output) == LARDON3D_PROJECT_DB_OK;
}

static bool is_unattached(Lardon3DProjectDb *database, uint64_t execution_id) {
  Lardon3DProjectDbSelectedExecution execution;
  return lardon3d_project_db_load_selected_execution(
             database, execution_id, &execution) == LARDON3D_PROJECT_DB_OK &&
         execution.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
         !execution.has_calibration_scope;
}

static bool run_cases(void) {
  Lardon3DCalibrationToolingV2Evidence source;
  Lardon3DCalibrationToolingV2Group groups[2];
  Lardon3DCalibrationToolingV2Entry entries[2];
  evidence(&source, groups, entries);
  unsigned char artifact[1024], artifact_hash[32];
  size_t artifact_size = 0;
  CHECK(lardon3d_calibration_tooling_v2_produce(
            &source, artifact, sizeof(artifact), &artifact_size,
            artifact_hash) == LARDON3D_CALIBRATION_TOOLING_V2_OK);

  char directory[] = "/tmp/lardon3d-workflow-v2-XXXXXX";
  CHECK(mkdtemp(directory) != NULL);
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/project.db", directory) > 0);
  Lardon3DProjectDb *database = NULL;
  uint64_t execution_id = 0;
  CHECK(create_project(path, &database, &execution_id));
  Lardon3DCalibrationWorkflowV2Binding bindings[2] = {
      {.selected_item_index = 0,
       .capture_id = 1,
       .kind = LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT,
       .exemplar_capture_id = 1},
      {.selected_item_index = 1,
       .capture_id = 2,
       .kind = LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT,
       .exemplar_capture_id = 2}};
  Lardon3DCalibrationWorkflowV2Output output;

  /* No complete observed tuple is a semantic non-ready state and must not
   * trigger the unattached publication primitive. */
  CHECK(lardon3d_calibration_workflow_v2_complete(
            database, execution_id, artifact, artifact_size, artifact_hash,
            bindings, 2,
            &output) == LARDON3D_CALIBRATION_WORKFLOW_V2_CALIBRATION_REQUIRED);
  CHECK(is_unattached(database, execution_id));
  CHECK(add_state(database, 1, 1, 4000, 3000));
  CHECK(add_state(database, 2, 2, 6000, 4000));

  unsigned char wrong_hash[32] = {0};
  memset(&output, 0xa5, sizeof(output));
  CHECK(lardon3d_calibration_workflow_v2_complete(
            database, execution_id, artifact, artifact_size, wrong_hash,
            bindings, 2,
            &output) == LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_EVIDENCE);
  const Lardon3DCalibrationWorkflowV2Output empty_output = {0};
  CHECK(memcmp(&output, &empty_output, sizeof(output)) == 0);
  CHECK(is_unattached(database, execution_id));

  Lardon3DCalibrationBootstrapV2Member probe_members[2];
  Lardon3DCalibrationBootstrapV2Output probe_publication;
  Lardon3DCalibrationBootstrapV2Result probe =
      lardon3d_calibration_bootstrap_v2_publish_unattached(
          database, execution_id, artifact, artifact_size, artifact_hash,
          probe_members, 2, &probe_publication);
  if (probe != LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK)
    fprintf(stderr, "unattached probe result=%d\n", (int)probe);
  CHECK(probe == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK);

  Lardon3DCalibrationWorkflowV2Result completed =
      lardon3d_calibration_workflow_v2_complete(
          database, execution_id, artifact, artifact_size, artifact_hash,
          bindings, 2, &output);
  if (completed != LARDON3D_CALIBRATION_WORKFLOW_V2_READY)
    fprintf(stderr, "happy workflow result=%d\n", (int)completed);
  CHECK(completed == LARDON3D_CALIBRATION_WORKFLOW_V2_READY);
  uint64_t scope_id = output.publication.scope.scope_id;
  Lardon3DSparseCalibrationMember members[2];
  size_t count = 0;
  uint64_t next = 0;
  CHECK(lardon3d_sparse_calibration_scope_list_members(
            database, scope_id, 0, members, 2, &count, &next) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(count == 2 && members[0].calibration_id != members[1].calibration_id);
  CHECK(lardon3d_calibration_workflow_v2_complete(
            database, execution_id, artifact, artifact_size, artifact_hash,
            bindings, 2, &output) == LARDON3D_CALIBRATION_WORKFLOW_V2_READY &&
        output.publication.scope.scope_id == scope_id);
  lardon3d_project_db_close(database);

  /* A requested applicability for the other heterogeneous member is exact-
   * state incompatible and cannot attach the otherwise complete scope. */
  char wrong_path[512];
  CHECK(snprintf(wrong_path, sizeof(wrong_path), "%s/wrong.db", directory) > 0);
  CHECK(create_project(wrong_path, &database, &execution_id));
  CHECK(add_state(database, 1, 1, 4000, 3000));
  CHECK(add_state(database, 2, 2, 6000, 4000));
  Lardon3DCalibrationBootstrapV2Member published[2];
  Lardon3DCalibrationBootstrapV2Output publication;
  CHECK(lardon3d_calibration_bootstrap_v2_publish_unattached(
            database, execution_id, artifact, artifact_size, artifact_hash,
            published, 2,
            &publication) == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK);
  Lardon3DOpticalCalibrationProfile profile = {
      .optical_configuration_id = 2,
      .sparse_calibration_id = published[1].calibration_id,
      .profile_version = 1,
      .applicability = LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION};
  strcpy(profile.name, "wrong-member");
  strcpy(profile.provenance, "test");
  Lardon3DOpticalCalibrationProfile stored_profile;
  CHECK(lardon3d_optical_calibration_profile_create(
            database, &profile, &stored_profile) == LARDON3D_PROJECT_DB_OK);
  Lardon3DOpticalCalibrationApplicabilityV2 applicability;
  CHECK(lardon3d_optical_calibration_applicability_v2_create(
            database, stored_profile.calibration_profile_id, 2,
            &applicability) == LARDON3D_PROJECT_DB_OK);
  bindings[0].kind = LARDON3D_CALIBRATION_WORKFLOW_V2_EXISTING_EXPLICIT;
  bindings[0].applicability_id = applicability.applicability_id;
  bindings[0].exemplar_capture_id = 0;
  CHECK(lardon3d_calibration_workflow_v2_complete(
            database, execution_id, artifact, artifact_size, artifact_hash,
            bindings, 2,
            &output) == LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT);
  CHECK(is_unattached(database, execution_id));
  lardon3d_project_db_close(database);

  /* Two exact candidates remain visibly ambiguous in AUTOMATIC mode even
   * when both happen to reference the artifact's immutable calibration. */
  char ambiguous_path[512];
  CHECK(snprintf(ambiguous_path, sizeof(ambiguous_path), "%s/ambiguous.db",
                 directory) > 0);
  CHECK(create_project(ambiguous_path, &database, &execution_id));
  CHECK(add_state(database, 1, 1, 4000, 3000));
  CHECK(add_state(database, 2, 2, 6000, 4000));
  CHECK(lardon3d_calibration_bootstrap_v2_publish_unattached(
            database, execution_id, artifact, artifact_size, artifact_hash,
            published, 2,
            &publication) == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK);
  for (size_t index = 0; index < 2; ++index) {
    memset(&profile, 0, sizeof(profile));
    profile.optical_configuration_id = 1;
    profile.sparse_calibration_id = published[0].calibration_id;
    profile.profile_version = 1;
    profile.applicability = LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION;
    profile.created_at = 0;
    CHECK(snprintf(profile.name, sizeof(profile.name), "ambiguous-%zu", index) >
          0);
    strcpy(profile.provenance, "test ambiguity");
    CHECK(lardon3d_optical_calibration_profile_create(
              database, &profile, &stored_profile) == LARDON3D_PROJECT_DB_OK);
    CHECK(lardon3d_optical_calibration_applicability_v2_create(
              database, stored_profile.calibration_profile_id, 1,
              &applicability) == LARDON3D_PROJECT_DB_OK);
  }
  bindings[0].kind = LARDON3D_CALIBRATION_WORKFLOW_V2_AUTOMATIC;
  bindings[0].applicability_id = 0;
  bindings[0].exemplar_capture_id = 0;
  CHECK(lardon3d_calibration_workflow_v2_complete(
            database, execution_id, artifact, artifact_size, artifact_hash,
            bindings, 2,
            &output) == LARDON3D_CALIBRATION_WORKFLOW_V2_SELECTION_REQUIRED);
  CHECK(is_unattached(database, execution_id));
  lardon3d_project_db_close(database);

  CHECK(unlink(path) == 0);
  CHECK(unlink(wrong_path) == 0);
  CHECK(unlink(ambiguous_path) == 0);
  CHECK(rmdir(directory) == 0);
  return true;
}

int main(void) { return run_cases() ? EXIT_SUCCESS : EXIT_FAILURE; }
