#include <lardon3d/calibration_workflow_v2.h>

#include <lardon3d/optical_profiles.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool complete_state(const Lardon3DOpticalCaptureGeometricState *state) {
  return state->focus_state == LARDON3D_OPTICAL_OBSERVATION_OBSERVED &&
         state->aperture_state == LARDON3D_OPTICAL_OBSERVATION_OBSERVED &&
         state->stabilization != LARDON3D_OPTICAL_STABILIZATION_UNKNOWN &&
         state->crop_state == LARDON3D_OPTICAL_OBSERVATION_OBSERVED &&
         state->pipeline_state == LARDON3D_OPTICAL_OBSERVATION_OBSERVED &&
         state->representation_state == LARDON3D_OPTICAL_OBSERVATION_OBSERVED &&
         state->decoded_geometry_state ==
             LARDON3D_OPTICAL_OBSERVATION_OBSERVED &&
         state->decoded_width != 0 && state->decoded_height != 0;
}

static bool same_state(const Lardon3DOpticalCaptureGeometricState *left,
                       const Lardon3DOpticalCaptureGeometricState *right) {
  return left->optical_configuration_id == right->optical_configuration_id &&
         left->state_version == right->state_version &&
         left->provenance == right->provenance &&
         left->focus_state == right->focus_state &&
         strcmp(left->focus_observation, right->focus_observation) == 0 &&
         left->aperture_state == right->aperture_state &&
         left->aperture_x1000 == right->aperture_x1000 &&
         left->stabilization == right->stabilization &&
         left->crop_state == right->crop_state &&
         strcmp(left->crop_observation, right->crop_observation) == 0 &&
         left->pipeline_state == right->pipeline_state &&
         strcmp(left->pipeline_observation, right->pipeline_observation) == 0 &&
         left->representation_state == right->representation_state &&
         strcmp(left->representation_observation,
                right->representation_observation) == 0 &&
         left->decoded_geometry_state == right->decoded_geometry_state &&
         left->decoded_width == right->decoded_width &&
         left->decoded_height == right->decoded_height;
}

static Lardon3DCalibrationWorkflowV2Result
bootstrap_result(Lardon3DCalibrationBootstrapV2Result result) {
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_MALFORMED_ARTIFACT ||
      result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_PROVENANCE_MISMATCH)
    return LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_EVIDENCE;
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_SELECTION_CONFLICT)
    return LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT;
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_OUT_OF_MEMORY)
    return LARDON3D_CALIBRATION_WORKFLOW_V2_OUT_OF_MEMORY;
  if (result == LARDON3D_CALIBRATION_BOOTSTRAP_V2_INVALID_ARGUMENT)
    return LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_ARGUMENT;
  return LARDON3D_CALIBRATION_WORKFLOW_V2_DB_ERROR;
}

static Lardon3DCalibrationWorkflowV2Result
db_error(Lardon3DProjectDbResult result) {
  return result == LARDON3D_PROJECT_DB_CONSTRAINT ||
                 result == LARDON3D_PROJECT_DB_NOT_FOUND
             ? LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT
             : LARDON3D_CALIBRATION_WORKFLOW_V2_DB_ERROR;
}

static void hex_digest(const unsigned char digest[32], char output[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < 32; ++index) {
    output[2 * index] = digits[digest[index] >> 4];
    output[2 * index + 1] = digits[digest[index] & 15];
  }
  output[64] = '\0';
}

static Lardon3DCalibrationWorkflowV2Result
publish_applicability(Lardon3DProjectDb *database,
                      const Lardon3DCalibrationWorkflowV2Binding *binding,
                      uint64_t calibration_id, uint64_t configuration_id,
                      uint64_t *applicability_id) {
  Lardon3DSparseCalibration calibration;
  Lardon3DProjectDbResult status =
      lardon3d_sparse_calibration_load(database, calibration_id, &calibration);
  if (status != LARDON3D_PROJECT_DB_OK)
    return db_error(status);
  char scientific_hash[65];
  char provenance_hash[65];
  hex_digest(calibration.scientific_hash, scientific_hash);
  hex_digest(calibration.provenance_fingerprint, provenance_hash);
  Lardon3DOpticalCalibrationProfile requested = {0};
  requested.optical_configuration_id = configuration_id;
  requested.sparse_calibration_id = calibration_id;
  requested.profile_version =
      LARDON3D_CALIBRATION_BOOTSTRAP_V2_ARTIFACT_VERSION;
  requested.applicability = LARDON3D_OPTICAL_CALIBRATION_EXACT_CONFIGURATION;
  requested.created_at = 0;
  (void)snprintf(requested.name, sizeof(requested.name), "L3DCALB2-%s",
                 scientific_hash);
  (void)snprintf(requested.provenance, sizeof(requested.provenance),
                 "L3DCALB2 group-sha256=%s", provenance_hash);
  Lardon3DOpticalCalibrationProfile profile;
  status = lardon3d_optical_calibration_profile_create(database, &requested,
                                                       &profile);
  if (status != LARDON3D_PROJECT_DB_OK)
    return db_error(status);
  Lardon3DOpticalCalibrationApplicabilityV2 applicability;
  status = lardon3d_optical_calibration_applicability_v2_create(
      database, profile.calibration_profile_id, binding->exemplar_capture_id,
      &applicability);
  if (status != LARDON3D_PROJECT_DB_OK)
    return db_error(status);
  *applicability_id = applicability.applicability_id;
  return LARDON3D_CALIBRATION_WORKFLOW_V2_READY;
}

Lardon3DCalibrationWorkflowV2Result lardon3d_calibration_workflow_v2_complete(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char expected_artifact_sha256[32],
    const Lardon3DCalibrationWorkflowV2Binding *bindings, size_t binding_count,
    Lardon3DCalibrationWorkflowV2Output *output) {
  if (output)
    memset(output, 0, sizeof(*output));
  if (!database || execution_id == 0 || !artifact ||
      !expected_artifact_sha256 || !bindings || !output || binding_count == 0 ||
      binding_count > LARDON3D_CALIBRATION_BOOTSTRAP_V2_MAX_ENTRIES)
    return LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_ARGUMENT;

  Lardon3DProjectDbSelectedExecution execution;
  Lardon3DProjectDbResult status = lardon3d_project_db_load_selected_execution(
      database, execution_id, &execution);
  if (status != LARDON3D_PROJECT_DB_OK)
    return db_error(status);
  if (execution.item_count != binding_count ||
      (execution.stage != LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
       execution.stage != LARDON3D_SELECTED_EXECUTION_READY))
    return LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT;

  const Lardon3DCalibrationWorkflowV2Binding **ordered =
      calloc(binding_count, sizeof(*ordered));
  Lardon3DOpticalCaptureGeometricState *states =
      calloc(binding_count, sizeof(*states));
  Lardon3DCalibrationBootstrapV2Member *members =
      calloc(binding_count, sizeof(*members));
  if (!ordered || !states || !members) {
    free(ordered);
    free(states);
    free(members);
    return LARDON3D_CALIBRATION_WORKFLOW_V2_OUT_OF_MEMORY;
  }

  Lardon3DCalibrationWorkflowV2Result result =
      LARDON3D_CALIBRATION_WORKFLOW_V2_READY;
  for (size_t index = 0; index < binding_count; ++index) {
    const Lardon3DCalibrationWorkflowV2Binding *binding = &bindings[index];
    if (binding->selected_item_index >= binding_count ||
        ordered[binding->selected_item_index] || binding->capture_id == 0 ||
        (binding->kind == LARDON3D_CALIBRATION_WORKFLOW_V2_AUTOMATIC &&
         (binding->applicability_id != 0 ||
          binding->exemplar_capture_id != 0)) ||
        (binding->kind == LARDON3D_CALIBRATION_WORKFLOW_V2_EXISTING_EXPLICIT &&
         (binding->applicability_id == 0 ||
          binding->exemplar_capture_id != 0)) ||
        (binding->kind == LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT &&
         (binding->applicability_id != 0 ||
          binding->exemplar_capture_id == 0)) ||
        binding->kind < LARDON3D_CALIBRATION_WORKFLOW_V2_AUTOMATIC ||
        binding->kind > LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT) {
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_ARGUMENT;
      break;
    }
    ordered[binding->selected_item_index] = binding;
  }

  /* Preflight all scientific Capture bindings before immutable publication.
   * A missing/incomplete tuple is a truthful semantic state, while a caller
   * mapping that disagrees with selected_execution_items is an error. */
  for (size_t index = 0; index < binding_count &&
                         result == LARDON3D_CALIBRATION_WORKFLOW_V2_READY;
       ++index) {
    const Lardon3DCalibrationWorkflowV2Binding *binding = ordered[index];
    Lardon3DProjectDbSelectedExecutionItem selected;
    status = lardon3d_project_db_load_selected_execution_item(
        database, execution_id, (uint32_t)index, &selected);
    if (status != LARDON3D_PROJECT_DB_OK || !selected.has_image ||
        selected.capture_id != binding->capture_id) {
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT;
      break;
    }
    status = lardon3d_optical_capture_geometric_state_load(
        database, binding->capture_id, &states[index]);
    if (status == LARDON3D_PROJECT_DB_NOT_FOUND ||
        (status == LARDON3D_PROJECT_DB_OK && !complete_state(&states[index]))) {
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_CALIBRATION_REQUIRED;
      break;
    }
    if (status != LARDON3D_PROJECT_DB_OK) {
      result = db_error(status);
      break;
    }
    if (binding->kind == LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT) {
      Lardon3DOpticalCaptureGeometricState exemplar;
      status = lardon3d_optical_capture_geometric_state_load(
          database, binding->exemplar_capture_id, &exemplar);
      if (status == LARDON3D_PROJECT_DB_NOT_FOUND ||
          (status == LARDON3D_PROJECT_DB_OK && !complete_state(&exemplar))) {
        result = LARDON3D_CALIBRATION_WORKFLOW_V2_CALIBRATION_REQUIRED;
        break;
      }
      if (status != LARDON3D_PROJECT_DB_OK) {
        result = db_error(status);
        break;
      }
      if (!same_state(&states[index], &exemplar)) {
        result = LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT;
        break;
      }
    }
    if (binding->kind == LARDON3D_CALIBRATION_WORKFLOW_V2_AUTOMATIC) {
      Lardon3DOpticalCalibrationResolutionV2 resolution;
      status = lardon3d_optical_capture_calibration_resolve_v2(
          database, binding->capture_id, &resolution);
      if (status != LARDON3D_PROJECT_DB_OK) {
        result = db_error(status);
        break;
      }
      if (resolution.kind == LARDON3D_OPTICAL_CALIBRATION_REQUIRED)
        result = LARDON3D_CALIBRATION_WORKFLOW_V2_CALIBRATION_REQUIRED;
      else if (resolution.kind ==
               LARDON3D_OPTICAL_CALIBRATION_SELECTION_REQUIRED)
        result = LARDON3D_CALIBRATION_WORKFLOW_V2_SELECTION_REQUIRED;
    }
  }

  Lardon3DCalibrationBootstrapV2Output publication;
  if (result == LARDON3D_CALIBRATION_WORKFLOW_V2_READY) {
    Lardon3DCalibrationBootstrapV2Result published =
        lardon3d_calibration_bootstrap_v2_publish_unattached(
            database, execution_id, artifact, artifact_size,
            expected_artifact_sha256, members, binding_count, &publication);
    if (published != LARDON3D_CALIBRATION_BOOTSTRAP_V2_OK)
      result = bootstrap_result(published);
  }

  for (size_t index = 0; index < binding_count &&
                         result == LARDON3D_CALIBRATION_WORKFLOW_V2_READY;
       ++index) {
    const Lardon3DCalibrationWorkflowV2Binding *binding = ordered[index];
    uint64_t applicability_id = binding->applicability_id;
    if (binding->kind == LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT)
      result = publish_applicability(
          database, binding, members[index].calibration_id,
          states[index].optical_configuration_id, &applicability_id);
    if (result != LARDON3D_CALIBRATION_WORKFLOW_V2_READY)
      break;
    if (binding->kind != LARDON3D_CALIBRATION_WORKFLOW_V2_AUTOMATIC) {
      status = lardon3d_optical_capture_calibration_select_v2(
          database, binding->capture_id, applicability_id);
      if (status != LARDON3D_PROJECT_DB_OK) {
        result = db_error(status);
        break;
      }
    }
    Lardon3DOpticalCalibrationResolutionV2 resolution;
    status = lardon3d_optical_capture_calibration_resolve_v2(
        database, binding->capture_id, &resolution);
    if (status != LARDON3D_PROJECT_DB_OK) {
      result = db_error(status);
      break;
    }
    if (resolution.kind == LARDON3D_OPTICAL_CALIBRATION_REQUIRED)
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_CALIBRATION_REQUIRED;
    else if (resolution.kind == LARDON3D_OPTICAL_CALIBRATION_SELECTION_REQUIRED)
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_SELECTION_REQUIRED;
    else if (resolution.sparse_calibration_id != members[index].calibration_id)
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT;
  }

  if (result == LARDON3D_CALIBRATION_WORKFLOW_V2_READY) {
    status = lardon3d_project_db_assign_selected_calibration_scope(
        database, execution_id, publication.scope.scope_id);
    if (status != LARDON3D_PROJECT_DB_OK)
      result = db_error(status);
  }
  if (result == LARDON3D_CALIBRATION_WORKFLOW_V2_READY) {
    status = lardon3d_project_db_load_selected_execution(database, execution_id,
                                                         &execution);
    if (status != LARDON3D_PROJECT_DB_OK ||
        execution.stage != LARDON3D_SELECTED_EXECUTION_READY ||
        !execution.has_calibration_scope ||
        execution.calibration_scope_id != publication.scope.scope_id)
      result = LARDON3D_CALIBRATION_WORKFLOW_V2_DB_ERROR;
  }
  if (result == LARDON3D_CALIBRATION_WORKFLOW_V2_READY) {
    output->publication = publication;
    output->selected_item_count = (uint32_t)binding_count;
  }
  free(ordered);
  free(states);
  free(members);
  return result;
}
