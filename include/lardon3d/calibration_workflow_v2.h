#ifndef LARDON3D_CALIBRATION_WORKFLOW_V2_H
#define LARDON3D_CALIBRATION_WORKFLOW_V2_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/calibration_bootstrap_v2.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LARDON3D_CALIBRATION_WORKFLOW_V2_AUTOMATIC = 1,
  LARDON3D_CALIBRATION_WORKFLOW_V2_EXISTING_EXPLICIT = 2,
  LARDON3D_CALIBRATION_WORKFLOW_V2_PUBLISH_EXPLICIT = 3,
} Lardon3DCalibrationWorkflowV2BindingKind;

typedef struct {
  uint32_t selected_item_index;
  /* Capture identity must be copied from the selected execution's durable item
   * mapping. Workflow rejects any mismatch and never derives it from image_id,
   * artifact group number, path, filename, or SHA-256. */
  uint64_t capture_id;
  Lardon3DCalibrationWorkflowV2BindingKind kind;
  /* EXISTING_EXPLICIT requires an existing exact v26 applicability. */
  uint64_t applicability_id;
  /* PUBLISH_EXPLICIT uses this Capture's complete observed tuple as the exact
   * applicability exemplar. It may equal capture_id. Other modes require 0. */
  uint64_t exemplar_capture_id;
} Lardon3DCalibrationWorkflowV2Binding;

typedef enum {
  LARDON3D_CALIBRATION_WORKFLOW_V2_READY = 0,
  LARDON3D_CALIBRATION_WORKFLOW_V2_CALIBRATION_REQUIRED,
  LARDON3D_CALIBRATION_WORKFLOW_V2_SELECTION_REQUIRED,
  LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_WORKFLOW_V2_INVALID_EVIDENCE,
  LARDON3D_CALIBRATION_WORKFLOW_V2_ASSIGNMENT_CONFLICT,
  LARDON3D_CALIBRATION_WORKFLOW_V2_DB_ERROR,
  LARDON3D_CALIBRATION_WORKFLOW_V2_OUT_OF_MEMORY,
} Lardon3DCalibrationWorkflowV2Result;

typedef struct {
  Lardon3DCalibrationBootstrapV2Output publication;
  uint32_t selected_item_count;
} Lardon3DCalibrationWorkflowV2Output;

/* Compose heterogeneous L3DCALB2 publication with exact Project DB v26
 * applicability and the final selected-execution READY transition.
 *
 * All pointers are borrowed for the call. `bindings` must cover every selected
 * item exactly once and must repeat the durable selected item -> Capture
 * mapping. AUTOMATIC accepts exactly one compatible applicability and reports
 * CALIBRATION_REQUIRED/SELECTION_REQUIRED truthfully for zero/multiple choices.
 * EXISTING_EXPLICIT verifies and durably selects the requested exact
 * applicability. PUBLISH_EXPLICIT creates/reuses deterministic optical profile
 * metadata for the artifact calibration, creates/reuses exact exemplar
 * applicability, and selects it explicitly.
 *
 * READY is returned only when every durable Capture resolves to the same
 * calibration_id as its complete scope member and that exact scope is attached.
 * Invalid evidence or an assignment mismatch is a distinct non-ready error.
 * Pre-attachment failure never attaches a scope; immutable publication and
 * optical evidence created by an earlier phase may remain and exact retry
 * converges. No schema change, solver execution, or Capture inference occurs.
 */
Lardon3DCalibrationWorkflowV2Result lardon3d_calibration_workflow_v2_complete(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const unsigned char *artifact, size_t artifact_size,
    const unsigned char
        expected_artifact_sha256[LARDON3D_PROJECT_DB_SHA256_SIZE],
    const Lardon3DCalibrationWorkflowV2Binding *bindings, size_t binding_count,
    Lardon3DCalibrationWorkflowV2Output *output);

#ifdef __cplusplus
}
#endif

#endif
