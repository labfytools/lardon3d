#ifndef LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_H
#define LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_H

#include <lardon3d/calibration_af_study.h>
#include <lardon3d/calibration_workflow.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK = 0,
  LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE,
  LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_CRYPTO_ERROR,
} Lardon3DCalibrationAfStudyWorkflowResult;

/* Convert one already validated/materialized Calibration Workflow result into
 * one AF-study sample without re-parsing any solver file.
 *
 * `focus_token` and FIT/HOLDOUT role are study annotations. They are
 * deliberately excluded from calibration_evidence_sha256, so relabelling the
 * same calibration result cannot manufacture a new calibration-evidence
 * identity.
 *
 * calibration_evidence_sha256 is domain-separated and binds:
 * - target identity;
 * - optical-state identity;
 * - solver executable/configuration identity;
 * - initialization and validation evidence;
 * - exact oriented dimensions;
 * - exact published binary64 fx,fy,cx,cy,k1,k2,p1,p2.
 *
 * The bridge checks the materialization invariants it consumes, including
 * deterministic validation-evidence binding and equality of all three repeated
 * full solves. It publishes repeated_parameters[0].
 *
 * No Project DB access, metadata interpretation, physical AF applicability
 * decision, interpolation, extrapolation, solver execution or thresholding
 * occurs here. */
Lardon3DCalibrationAfStudyWorkflowResult
lardon3d_calibration_af_study_sample_from_materialized_evidence(
    const Lardon3DCalibrationWorkflowExternalEvidence *external,
    Lardon3DCalibrationAfStudySampleRole role, const char *focus_token,
    Lardon3DCalibrationAfStudySample *output);

#ifdef __cplusplus
}
#endif

#endif
