// This translation unit calls the C Project DB and frozen Tooling APIs from
// C++. Their declarations must have C linkage before the workflow include path.
extern "C" {
#include <lardon3d/project_db.h>
}

#include <lardon3d/calibration_workflow.h>

#include <cstring>
#include <new>

namespace {

Lardon3DCalibrationWorkflowResult tooling_result(
    Lardon3DCalibrationToolingResult result) {
  if (result == LARDON3D_CALIBRATION_TOOLING_OK)
    return LARDON3D_CALIBRATION_WORKFLOW_OK;
  if (result == LARDON3D_CALIBRATION_TOOLING_CAPACITY)
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
  if (result == LARDON3D_CALIBRATION_TOOLING_IMPORT_ERROR)
    return LARDON3D_CALIBRATION_WORKFLOW_PROJECT_DB_ERROR;
  return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;
}

Lardon3DCalibrationWorkflowResult complete_impl(
    Lardon3DProjectDb *database, const char *project_path,
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationToolingView *views, size_t view_capacity,
    Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks,
    size_t coordinate_check_capacity, Lardon3DCalibrationToolingEntry *entries,
    size_t entry_capacity, unsigned char *artifact, size_t artifact_capacity,
    size_t *artifact_size, Lardon3DCalibrationBootstrapOutput *output) {
  if (artifact_size) *artifact_size = 0;
  if (output) std::memset(output, 0, sizeof(*output));
  if (!database || !project_path || !*project_path || !files || !views ||
      !coordinate_checks || !entries || !artifact || !artifact_size || !output)
    return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;

  Lardon3DCalibrationWorkflowExternalEvidence external{};
  Lardon3DCalibrationWorkflowResult result =
      lardon3d_calibration_workflow_materialize_external_evidence(
          files, views, view_capacity, coordinate_checks, coordinate_check_capacity,
          &external);
  if (result != LARDON3D_CALIBRATION_WORKFLOW_OK) return result;

  Lardon3DCalibrationToolingEvidence tooling{};
  result = lardon3d_calibration_workflow_bind_selected_execution(
      database, project_path, &external, entries, entry_capacity, &tooling);
  if (result != LARDON3D_CALIBRATION_WORKFLOW_OK) return result;

  result = tooling_result(lardon3d_calibration_tooling_import(
      database, external.boundary.selected_execution_id, &tooling, artifact,
      artifact_capacity, artifact_size, output));
  if (result != LARDON3D_CALIBRATION_WORKFLOW_OK) return result;

  // Tooling owns immutable import publication. The workflow's success contract
  // is stricter: only the exact returned scope attached to READY is success.
  Lardon3DProjectDbSelectedExecution execution{};
  const Lardon3DProjectDbResult loaded =
      lardon3d_project_db_load_selected_execution(
          database, external.boundary.selected_execution_id, &execution);
  if (loaded != LARDON3D_PROJECT_DB_OK ||
      execution.stage != LARDON3D_SELECTED_EXECUTION_READY ||
      !execution.has_calibration_scope || execution.calibration_scope_id == 0 ||
      execution.calibration_scope_id != output->scope.scope_id)
    return LARDON3D_CALIBRATION_WORKFLOW_PROJECT_DB_ERROR;
  return LARDON3D_CALIBRATION_WORKFLOW_OK;
}

}  // namespace

extern "C" Lardon3DCalibrationWorkflowResult
lardon3d_calibration_workflow_complete(
    Lardon3DProjectDb *database, const char *project_path,
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationToolingView *views, size_t view_capacity,
    Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks,
    size_t coordinate_check_capacity, Lardon3DCalibrationToolingEntry *entries,
    size_t entry_capacity, unsigned char *artifact, size_t artifact_capacity,
    size_t *artifact_size, Lardon3DCalibrationBootstrapOutput *output) {
  try {
    return complete_impl(database, project_path, files, views, view_capacity,
                         coordinate_checks, coordinate_check_capacity, entries,
                         entry_capacity, artifact, artifact_capacity,
                         artifact_size, output);
  } catch (const std::bad_alloc&) {
    if (artifact_size) *artifact_size = 0;
    if (output) std::memset(output, 0, sizeof(*output));
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
  } catch (...) {
    if (artifact_size) *artifact_size = 0;
    if (output) std::memset(output, 0, sizeof(*output));
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  }
}
