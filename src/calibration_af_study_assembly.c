#include <lardon3d/calibration_af_study_assembly.h>

#include <stdbool.h>
#include <string.h>

static bool nonzero_digest(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t index = 0; index < 32; ++index) any |= value[index];
  return any != 0;
}

static Lardon3DCalibrationAfStudyAssemblyResult map_bridge_result(
    Lardon3DCalibrationAfStudyWorkflowResult result) {
  switch (result) {
    case LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_ARGUMENT;
    case LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE;
    case LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_CRYPTO_ERROR:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_CRYPTO_ERROR;
    case LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK:
      break;
  }
  return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE;
}

static Lardon3DCalibrationAfStudyAssemblyResult map_study_result(
    Lardon3DCalibrationAfStudyResult result) {
  switch (result) {
    case LARDON3D_CALIBRATION_AF_STUDY_INVALID_ARGUMENT:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_ARGUMENT;
    case LARDON3D_CALIBRATION_AF_STUDY_CAPACITY:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_CAPACITY;
    case LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE;
    case LARDON3D_CALIBRATION_AF_STUDY_ENCODING_ERROR:
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_ENCODING_ERROR;
    case LARDON3D_CALIBRATION_AF_STUDY_OK:
      break;
  }
  return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE;
}

Lardon3DCalibrationAfStudyAssemblyResult
lardon3d_calibration_af_study_assemble_materialized(
    const Lardon3DCalibrationAfStudyAssemblyInput *input,
    unsigned char *artifact, size_t artifact_capacity, size_t *written,
    unsigned char artifact_sha256[32],
    Lardon3DCalibrationAfStudySummary *summary) {
  if (written) *written = 0;
  if (artifact_sha256) memset(artifact_sha256, 0, 32);
  if (summary) memset(summary, 0, sizeof(*summary));

  if (!input || !artifact || !written || !artifact_sha256 || !summary ||
      !input->entries || input->entry_count < 2 ||
      input->entry_count > LARDON3D_CALIBRATION_AF_STUDY_MAX_SAMPLES ||
      !nonzero_digest(input->study_context_sha256))
    return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_ARGUMENT;

  Lardon3DCalibrationAfStudySample
      samples[LARDON3D_CALIBRATION_AF_STUDY_MAX_SAMPLES];
  memset(samples, 0, sizeof(samples));

  uint32_t width = 0;
  uint32_t height = 0;

  for (size_t index = 0; index < input->entry_count; ++index) {
    const Lardon3DCalibrationAfStudyAssemblyEntry *entry =
        &input->entries[index];
    if (!entry->external || !entry->focus_token)
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_ARGUMENT;

    Lardon3DCalibrationAfStudyWorkflowResult bridge =
        lardon3d_calibration_af_study_sample_from_materialized_evidence(
            entry->external, entry->role, entry->focus_token, &samples[index]);
    if (bridge != LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK)
      return map_bridge_result(bridge);

    if (index == 0) {
      width = entry->external->oriented_width;
      height = entry->external->oriented_height;
    } else if (entry->external->oriented_width != width ||
               entry->external->oriented_height != height) {
      return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE;
    }

    /* Stronger than the raw L3DAFST1 producer's (token,digest) duplicate rule:
     * an already-materialized calibration result is independent evidence only
     * once, regardless of how the caller labels focus or study role. */
    for (size_t previous = 0; previous < index; ++previous) {
      if (memcmp(samples[previous].calibration_evidence_sha256,
                 samples[index].calibration_evidence_sha256, 32) == 0)
        return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE;
    }
  }

  Lardon3DCalibrationAfStudyInput study = {0};
  memcpy(study.study_context_sha256, input->study_context_sha256, 32);
  study.width = width;
  study.height = height;
  study.samples = samples;
  study.sample_count = input->entry_count;

  Lardon3DCalibrationAfStudyResult result =
      lardon3d_calibration_af_study_produce(
          &study, artifact, artifact_capacity, written, artifact_sha256,
          summary);
  if (result != LARDON3D_CALIBRATION_AF_STUDY_OK)
    return map_study_result(result);

  return LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_OK;
}
