#ifndef LARDON3D_CALIBRATION_WORKFLOW_H
#define LARDON3D_CALIBRATION_WORKFLOW_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/calibration_tooling.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_WORKFLOW_VERSION = 1,
  LARDON3D_CALIBRATION_WORKFLOW_MAX_FILE_BYTES = 128 * 1024 * 1024,
  LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS = 4096,
  LARDON3D_CALIBRATION_WORKFLOW_OPTICAL_STATE_TOKEN_CAPACITY = 1025,
};

typedef enum {
  LARDON3D_CALIBRATION_WORKFLOW_OK = 0,
  LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR,
  LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE,
  LARDON3D_CALIBRATION_WORKFLOW_CAPACITY,
  LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE,
  LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH,
  LARDON3D_CALIBRATION_WORKFLOW_PROJECT_DB_ERROR,
} Lardon3DCalibrationWorkflowResult;

/* `campaign_state_path` is a canonical external acquisition manifest:
 *
 * L3DCAL_CAMPAIGN_STATE_V1
 * execution <selected-execution-id>
 * optical_configuration <explicit-v23-configuration-id>
 * optical_state <sha256> <complete-Science-v1-state-token>
 * capture <zero-based-selected-item-index> <capture-id>
 *
 * Capture rows are contiguous and ordered. The later DB-binding stage proves
 * that these IDs are exactly the selected execution and that each Capture owns
 * the declared explicit optical configuration; this input boundary performs no
 * DB access and therefore never treats the manifest alone as that proof. */
typedef struct {
  const char *session_path;
  const char *detection_path;
  const char *solve_path;
  const char *evidence_path;
  const char *producer_path;
  const char *campaign_state_path;
} Lardon3DCalibrationWorkflowInputFiles;

typedef struct {
  unsigned char session_sha256[32];
  unsigned char detection_sha256[32];
  unsigned char solve_sha256[32];
  unsigned char evidence_sha256[32];
  unsigned char producer_sha256[32];
  unsigned char campaign_state_sha256[32];
  unsigned char optical_state_sha256[32];
  unsigned char solver_executable_sha256[32];
  unsigned char solver_configuration_sha256[32];
  uint64_t selected_execution_id;
  uint64_t optical_configuration_id;
  uint32_t capture_count;
  uint64_t capture_ids[LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS];
  char optical_state_token[LARDON3D_CALIBRATION_WORKFLOW_OPTICAL_STATE_TOKEN_CAPACITY];
} Lardon3DCalibrationWorkflowInputBoundary;

/* Validate only the immutable external input boundary. This function performs
 * no Project DB access, no calibration solve, no Tooling import and no
 * selected-execution mutation. Every path is opened O_NONBLOCK/O_NOFOLLOW,
 * must resolve to a bounded regular file, and is SHA-256 checked before its
 * syntax/provenance is consumed. */
Lardon3DCalibrationWorkflowResult lardon3d_calibration_workflow_validate_input_boundary(
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationWorkflowInputBoundary *boundary);


typedef enum {
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_SOURCE_SIZE = 1,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_DECODE = 2,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_DECODED_DIMENSIONS = 3,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_INSUFFICIENT_CHARUCO = 4,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_INVALID_CHARUCO_ID = 5,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_OCCUPANCY = 6,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_TARGET_PHYSICAL_QUADRANTS = 7,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_CLIPPING = 8,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_PRE_SOLVE_CORNER_RMS = 9,
  LARDON3D_CALIBRATION_WORKFLOW_REJECTION_COORDINATE_EQUIVALENCE = 10,
} Lardon3DCalibrationWorkflowRejectionReason;

typedef struct {
  Lardon3DCalibrationWorkflowInputBoundary boundary;
  unsigned char target_sha256[32];
  unsigned char optical_state_sha256[32];
  unsigned char solver_executable_sha256[32];
  unsigned char solver_configuration_sha256[32];
  unsigned char initialization_evidence_sha256[32];
  unsigned char validation_evidence_sha256[32];
  uint32_t target_family;
  uint32_t target_squares_x;
  uint32_t target_squares_y;
  double target_square_length_mm;
  double target_marker_length_mm;
  double target_active_width_mm;
  double target_active_height_mm;
  double target_white_border_mm;
  double target_measurements_mm[LARDON3D_CALIBRATION_TOOLING_TARGET_MEASUREMENTS];
  double measurement_resolution_mm;
  double target_flatness_mm;
  double holdout_rmse_px;
  double holdout_maximum_residual_px;
  uint32_t extra_distortion_coefficient_count;
  /* Exact decoded/oriented geometry shared by every accepted calibration view.
   * It is retained from detection.json + coordinate evidence and is never
   * inferred from camera parameters or campaign images. */
  uint32_t oriented_width;
  uint32_t oriented_height;
  const Lardon3DCalibrationToolingView *views;
  size_t view_count;
  const Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks;
  size_t coordinate_check_count;
  double repeated_parameters[3][8];
  double fit_parameters[8];
  uint32_t support_images;
  uint32_t support_observations;
  double reprojection_rmse_px;
  double maximum_residual_px;
  double high_residual_fraction;
  double maximum_parameter_delta;
  uint32_t validation_flags;
} Lardon3DCalibrationWorkflowExternalEvidence;

/* Materialize the already validated external session and solver bundle into
 * bounded Science-v1 evidence. Caller owns `views` and `coordinate_checks`;
 * output borrows those arrays on success. This stage performs no Project DB
 * access, no Tooling import and no selected-execution mutation. */
Lardon3DCalibrationWorkflowResult
lardon3d_calibration_workflow_materialize_external_evidence(
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationToolingView *views, size_t view_capacity,
    Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks,
    size_t coordinate_check_capacity,
    Lardon3DCalibrationWorkflowExternalEvidence *output);

/* Bind already materialized external evidence to the exact durable selected
 * execution. This stage is read-only: it verifies selected item order/Capture
 * identity, explicit v23 optical assignment, managed representation
 * size/SHA-256, safe project-relative regular-file access and the exact
 * OpenCV-decoded oriented dimensions. Caller owns `entries`; `output` borrows
 * them on success. No Tooling import or selected-execution mutation occurs. */
Lardon3DCalibrationWorkflowResult
lardon3d_calibration_workflow_bind_selected_execution(
    Lardon3DProjectDb *database, const char *project_path,
    const Lardon3DCalibrationWorkflowExternalEvidence *external,
    Lardon3DCalibrationToolingEntry *entries, size_t entry_capacity,
    Lardon3DCalibrationToolingEvidence *output);

/* Complete the bounded calibration workflow through the frozen Tooling and
 * Bootstrap importer. All supplied arrays and artifact storage remain caller
 * owned; `output` is cleared unless import reaches the exact READY transition.
 * Input validation, materialization and selected-execution binding finish
 * before this function invokes the only mutating operation. Exact retries use
 * Bootstrap's immutable import semantics. */
Lardon3DCalibrationWorkflowResult
lardon3d_calibration_workflow_complete(
    Lardon3DProjectDb *database, const char *project_path,
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationToolingView *views, size_t view_capacity,
    Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks,
    size_t coordinate_check_capacity, Lardon3DCalibrationToolingEntry *entries,
    size_t entry_capacity, unsigned char *artifact, size_t artifact_capacity,
    size_t *artifact_size, Lardon3DCalibrationBootstrapOutput *output);

#ifdef __cplusplus
}
#endif

#endif
