#include <lardon3d/calibration_af_study_assembly.h>

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
      return 1;                                                               \
    }                                                                         \
  } while (0)

static void fill_digest(unsigned char output[32], unsigned char value) {
  memset(output, value, 32);
}

static int validation_sha(
    const Lardon3DCalibrationWorkflowInputBoundary *boundary,
    unsigned char output[32]) {
  static const char domain[] = "L3DCAL_WORKFLOW_VALIDATION_V1\n";
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) return 0;
  int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
           EVP_DigestUpdate(ctx, domain, sizeof(domain) - 1) == 1 &&
           EVP_DigestUpdate(ctx, boundary->detection_sha256, 32) == 1 &&
           EVP_DigestUpdate(ctx, boundary->solve_sha256, 32) == 1 &&
           EVP_DigestUpdate(ctx, boundary->evidence_sha256, 32) == 1 &&
           EVP_DigestUpdate(ctx, boundary->producer_sha256, 32) == 1;
  unsigned int length = 0;
  ok = ok && EVP_DigestFinal_ex(ctx, output, &length) == 1 && length == 32;
  EVP_MD_CTX_free(ctx);
  return ok;
}

static Lardon3DCalibrationWorkflowExternalEvidence fixture(
    unsigned char identity, double focal_offset) {
  Lardon3DCalibrationWorkflowExternalEvidence value = {0};

  fill_digest(value.boundary.session_sha256, identity);
  fill_digest(value.boundary.detection_sha256, (unsigned char)(identity + 1));
  fill_digest(value.boundary.solve_sha256, (unsigned char)(identity + 2));
  fill_digest(value.boundary.evidence_sha256, (unsigned char)(identity + 3));
  fill_digest(value.boundary.producer_sha256, 0x55);
  fill_digest(value.boundary.campaign_state_sha256, 0x66);
  fill_digest(value.boundary.optical_state_sha256,
              (unsigned char)(identity + 4));
  fill_digest(value.boundary.solver_executable_sha256, 0x88);
  fill_digest(value.boundary.solver_configuration_sha256, 0x99);

  fill_digest(value.target_sha256, 0xA1);
  memcpy(value.optical_state_sha256, value.boundary.optical_state_sha256, 32);
  memcpy(value.solver_executable_sha256,
         value.boundary.solver_executable_sha256, 32);
  memcpy(value.solver_configuration_sha256,
         value.boundary.solver_configuration_sha256, 32);
  memcpy(value.initialization_evidence_sha256,
         value.boundary.session_sha256, 32);
  (void)validation_sha(&value.boundary, value.validation_evidence_sha256);

  value.oriented_width = 6000;
  value.oriented_height = 4000;
  const double params[8] = {
      4000.0 + focal_offset, 4002.0 + focal_offset,
      3000.0, 2000.0, -0.1, 0.01, 0.001, -0.001,
  };
  for (size_t run = 0; run < 3; ++run)
    memcpy(value.repeated_parameters[run], params, sizeof(params));
  return value;
}

static Lardon3DCalibrationAfStudyAssemblyInput make_input(
    Lardon3DCalibrationAfStudyAssemblyEntry *entries, size_t count) {
  Lardon3DCalibrationAfStudyAssemblyInput input = {0};
  fill_digest(input.study_context_sha256, 0xD1);
  input.entries = entries;
  input.entry_count = count;
  return input;
}

static int test_happy_path_and_order_independence(void) {
  Lardon3DCalibrationWorkflowExternalEvidence first = fixture(0x11, 0.0);
  Lardon3DCalibrationWorkflowExternalEvidence second = fixture(0x21, 8.0);
  Lardon3DCalibrationWorkflowExternalEvidence third = fixture(0x31, 14.0);

  Lardon3DCalibrationAfStudyAssemblyEntry entries[3] = {
      {&first, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:137"},
      {&second, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:151"},
      {&third, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT, "focus:165"},
  };
  Lardon3DCalibrationAfStudyAssemblyInput input = make_input(entries, 3);

  unsigned char artifact_a[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char artifact_b[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha_a[32];
  unsigned char sha_b[32];
  size_t written_a = 0;
  size_t written_b = 0;
  Lardon3DCalibrationAfStudySummary summary_a;
  Lardon3DCalibrationAfStudySummary summary_b;

  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact_a, sizeof(artifact_a), &written_a, sha_a,
            &summary_a) == LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_OK);
  CHECK(written_a > 0);
  CHECK(memcmp(artifact_a, "L3DAFST1", 8) == 0);
  CHECK(summary_a.sample_count == 3);
  CHECK(summary_a.fit_count == 2);
  CHECK(summary_a.holdout_count == 1);
  CHECK(summary_a.pair_count == 3);

  Lardon3DCalibrationAfStudyAssemblyEntry reordered[3] = {
      entries[2], entries[0], entries[1],
  };
  input.entries = reordered;
  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact_b, sizeof(artifact_b), &written_b, sha_b,
            &summary_b) == LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_OK);
  CHECK(written_a == written_b);
  CHECK(memcmp(artifact_a, artifact_b, written_a) == 0);
  CHECK(memcmp(sha_a, sha_b, 32) == 0);
  CHECK(memcmp(&summary_a, &summary_b, sizeof(summary_a)) == 0);
  return 0;
}

static int test_relabelled_duplicate_calibration_rejected(void) {
  Lardon3DCalibrationWorkflowExternalEvidence same = fixture(0x11, 0.0);
  Lardon3DCalibrationAfStudyAssemblyEntry entries[2] = {
      {&same, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:137"},
      {&same, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT, "focus:165"},
  };
  Lardon3DCalibrationAfStudyAssemblyInput input = make_input(entries, 2);
  unsigned char artifact[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha[32];
  size_t written = 999;
  Lardon3DCalibrationAfStudySummary summary;
  memset(sha, 0xA5, sizeof(sha));
  memset(&summary, 0xA5, sizeof(summary));

  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE);
  CHECK(written == 0);
  unsigned char zero_sha[32] = {0};
  CHECK(memcmp(sha, zero_sha, 32) == 0);
  Lardon3DCalibrationAfStudySummary zero_summary = {0};
  CHECK(memcmp(&summary, &zero_summary, sizeof(summary)) == 0);
  return 0;
}

static int test_dimension_mismatch_rejected(void) {
  Lardon3DCalibrationWorkflowExternalEvidence first = fixture(0x11, 0.0);
  Lardon3DCalibrationWorkflowExternalEvidence second = fixture(0x21, 8.0);
  second.oriented_width = 5999;

  Lardon3DCalibrationAfStudyAssemblyEntry entries[2] = {
      {&first, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:137"},
      {&second, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT, "focus:165"},
  };
  Lardon3DCalibrationAfStudyAssemblyInput input = make_input(entries, 2);
  unsigned char artifact[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha[32];
  size_t written = 0;
  Lardon3DCalibrationAfStudySummary summary;

  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE);
  return 0;
}

static int test_invalid_bridge_evidence_rejected(void) {
  Lardon3DCalibrationWorkflowExternalEvidence first = fixture(0x11, 0.0);
  Lardon3DCalibrationWorkflowExternalEvidence second = fixture(0x21, 8.0);
  second.validation_evidence_sha256[0] ^= 1;

  Lardon3DCalibrationAfStudyAssemblyEntry entries[2] = {
      {&first, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:137"},
      {&second, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT, "focus:165"},
  };
  Lardon3DCalibrationAfStudyAssemblyInput input = make_input(entries, 2);
  unsigned char artifact[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha[32];
  size_t written = 0;
  Lardon3DCalibrationAfStudySummary summary;

  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_EVIDENCE);
  return 0;
}

static int test_invalid_arguments_and_capacity(void) {
  Lardon3DCalibrationWorkflowExternalEvidence first = fixture(0x11, 0.0);
  Lardon3DCalibrationWorkflowExternalEvidence second = fixture(0x21, 8.0);
  Lardon3DCalibrationAfStudyAssemblyEntry entries[2] = {
      {&first, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:137"},
      {&second, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT, "focus:165"},
  };
  Lardon3DCalibrationAfStudyAssemblyInput input = make_input(entries, 1);
  unsigned char artifact[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha[32];
  size_t written = 0;
  Lardon3DCalibrationAfStudySummary summary;

  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_INVALID_ARGUMENT);

  input.entry_count = 2;
  CHECK(lardon3d_calibration_af_study_assemble_materialized(
            &input, artifact, 8, &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_ASSEMBLY_CAPACITY);
  return 0;
}

int main(void) {
  CHECK(test_happy_path_and_order_independence() == 0);
  CHECK(test_relabelled_duplicate_calibration_rejected() == 0);
  CHECK(test_dimension_mismatch_rejected() == 0);
  CHECK(test_invalid_bridge_evidence_rejected() == 0);
  CHECK(test_invalid_arguments_and_capacity() == 0);
  puts("CALIBRATION_AF_STUDY_ASSEMBLY_V1=PASS");
  return 0;
}
