#include <lardon3d/calibration_af_study_workflow.h>

#include <math.h>
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

static int sha256_validation(
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

static Lardon3DCalibrationWorkflowExternalEvidence fixture(void) {
  Lardon3DCalibrationWorkflowExternalEvidence value = {0};

  fill_digest(value.boundary.session_sha256, 0x11);
  fill_digest(value.boundary.detection_sha256, 0x22);
  fill_digest(value.boundary.solve_sha256, 0x33);
  fill_digest(value.boundary.evidence_sha256, 0x44);
  fill_digest(value.boundary.producer_sha256, 0x55);
  fill_digest(value.boundary.campaign_state_sha256, 0x66);
  fill_digest(value.boundary.optical_state_sha256, 0x77);
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

  (void)sha256_validation(&value.boundary, value.validation_evidence_sha256);

  value.oriented_width = 6000;
  value.oriented_height = 4000;
  const double params[8] = {
      4000.0, 4002.0, 3000.0, 2000.0, -0.1, 0.01, 0.001, -0.001,
  };
  for (size_t run = 0; run < 3; ++run)
    memcpy(value.repeated_parameters[run], params, sizeof(params));

  return value;
}

static int test_happy_path_and_identity_stability(void) {
  Lardon3DCalibrationWorkflowExternalEvidence external = fixture();
  Lardon3DCalibrationAfStudySample fit = {0};
  Lardon3DCalibrationAfStudySample holdout = {0};

  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT,
            "sony-focus:137", &fit) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK);
  CHECK(fit.role == LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT);
  CHECK(strcmp(fit.focus_token, "sony-focus:137") == 0);
  CHECK(fit.fx == 4000.0 && fit.fy == 4002.0);
  CHECK(fit.cx == 3000.0 && fit.cy == 2000.0);

  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT,
            "different-focus-label", &holdout) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK);

  CHECK(memcmp(fit.calibration_evidence_sha256,
               holdout.calibration_evidence_sha256, 32) == 0);
  unsigned char zero[32] = {0};
  CHECK(memcmp(fit.calibration_evidence_sha256, zero, 32) != 0);
  return 0;
}

static int test_identity_binds_provenance_and_parameters(void) {
  Lardon3DCalibrationWorkflowExternalEvidence a = fixture();
  Lardon3DCalibrationWorkflowExternalEvidence b = fixture();
  Lardon3DCalibrationAfStudySample sample_a = {0};
  Lardon3DCalibrationAfStudySample sample_b = {0};

  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &a, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample_a) == LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK);

  b.target_sha256[0] ^= 1;
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &b, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample_b) == LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK);
  CHECK(memcmp(sample_a.calibration_evidence_sha256,
               sample_b.calibration_evidence_sha256, 32) != 0);

  b = fixture();
  for (size_t run = 0; run < 3; ++run)
    b.repeated_parameters[run][0] += 1.0;
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &b, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample_b) == LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK);
  CHECK(memcmp(sample_a.calibration_evidence_sha256,
               sample_b.calibration_evidence_sha256, 32) != 0);
  return 0;
}

static int test_invalid_materialization_rejected(void) {
  Lardon3DCalibrationWorkflowExternalEvidence external = fixture();
  Lardon3DCalibrationAfStudySample sample;
  memset(&sample, 0xA5, sizeof(sample));

  external.validation_evidence_sha256[0] ^= 1;
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE);
  Lardon3DCalibrationAfStudySample zero = {0};
  CHECK(memcmp(&sample, &zero, sizeof(sample)) == 0);

  external = fixture();
  external.repeated_parameters[1][0] += 1.0;
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE);

  external = fixture();
  external.repeated_parameters[0][0] = NAN;
  external.repeated_parameters[1][0] = NAN;
  external.repeated_parameters[2][0] = NAN;
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE);

  external = fixture();
  external.optical_state_sha256[0] ^= 1;
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE);
  return 0;
}

static int test_invalid_arguments(void) {
  Lardon3DCalibrationWorkflowExternalEvidence external = fixture();
  Lardon3DCalibrationAfStudySample sample = {0};

  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            NULL, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "focus:a",
            &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT);
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, 0, "focus:a", &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT);
  CHECK(lardon3d_calibration_af_study_sample_from_materialized_evidence(
            &external, LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT, "",
            &sample) ==
        LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT);
  return 0;
}

int main(void) {
  CHECK(test_happy_path_and_identity_stability() == 0);
  CHECK(test_identity_binds_provenance_and_parameters() == 0);
  CHECK(test_invalid_materialization_rejected() == 0);
  CHECK(test_invalid_arguments() == 0);
  puts("CALIBRATION_AF_STUDY_WORKFLOW_BRIDGE_V1=PASS");
  return 0;
}
