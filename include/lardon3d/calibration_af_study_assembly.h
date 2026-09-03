#ifndef LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_H
#define LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_H

#include <lardon3d/calibration_af_study.h>
#include <lardon3d/calibration_af_study_workflow.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_OK = 0,
  LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE,
  LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_CAPACITY,
  LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_CRYPTO_ERROR,
  LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_ENCODING_ERROR,
} Lardon3DCalibrationAfStudyAssemblyResult;

typedef struct {
  const Lardon3DCalibrationWorkflowExternalEvidence *external;
  Lardon3DCalibrationAfStudySampleRole role;
  const char *focus_token;
} Lardon3DCalibrationAfStudyAssemblyEntry;

typedef struct {
  /* Exact caller-retained identity of the common body/lens/focal/non-focus
   * geometric study context. The assembly does not derive or reinterpret it. */
  unsigned char study_context_sha256[
      LARDON3D_CALIBRATION_AF_STUDY_SHA256_SIZE];
  const Lardon3DCalibrationAfStudyAssemblyEntry *entries;
  size_t entry_count;
} Lardon3DCalibrationAfStudyAssemblyInput;

/* Assemble 2..MAX_SAMPLES already-materialized Calibration Workflow results
 * into one deterministic L3DAFST1 artifact.
 *
 * Each entry is converted through the frozen Workflow bridge; no solver file
 * is parsed here. All samples must share exact oriented dimensions because one
 * L3DAFST1 study has one image geometry. The common optical/non-focus identity
 * remains the explicit study_context_sha256 supplied by the caller.
 *
 * Duplicate calibration_evidence_sha256 values are rejected regardless of
 * focus token or FIT/HOLDOUT role. A single physical calibration result cannot
 * therefore be relabelled to masquerade as independent AF evidence.
 *
 * This boundary performs no Project DB access, no metadata interpretation, no
 * physical applicability decision and no acceptance thresholding. */
Lardon3DCalibrationAfStudyAssemblyResult
lardon3d_calibration_af_study_assemble_materialized(
    const Lardon3DCalibrationAfStudyAssemblyInput *input,
    unsigned char *artifact, size_t artifact_capacity, size_t *written,
    unsigned char artifact_sha256[
        LARDON3D_CALIBRATION_AF_STUDY_SHA256_SIZE],
    Lardon3DCalibrationAfStudySummary *summary);

#ifdef __cplusplus
}
#endif

#endif
