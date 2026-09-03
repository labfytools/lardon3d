#include <lardon3d/calibration_af_study_workflow.h>

#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool nonzero_digest(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t index = 0; index < 32; ++index) any |= value[index];
  return any != 0;
}

static bool token_copy(
    const char *input,
    char output[LARDON3D_CALIBRATION_AF_STUDY_FOCUS_TOKEN_CAPACITY]) {
  if (!input) return false;
  size_t length = 0;
  while (length < LARDON3D_CALIBRATION_AF_STUDY_FOCUS_TOKEN_CAPACITY &&
         input[length] != '\0')
    ++length;
  if (length == 0 ||
      length >= LARDON3D_CALIBRATION_AF_STUDY_FOCUS_TOKEN_CAPACITY)
    return false;
  memset(output, 0, LARDON3D_CALIBRATION_AF_STUDY_FOCUS_TOKEN_CAPACITY);
  memcpy(output, input, length);
  return true;
}

static bool digest_begin(EVP_MD_CTX **ctx, const char *domain) {
  *ctx = EVP_MD_CTX_new();
  return *ctx &&
         EVP_DigestInit_ex(*ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(*ctx, domain, strlen(domain)) == 1;
}

static bool digest_add(EVP_MD_CTX *ctx, const void *bytes, size_t size) {
  return EVP_DigestUpdate(ctx, bytes, size) == 1;
}

static bool digest_finish(EVP_MD_CTX *ctx, unsigned char output[32]) {
  unsigned int output_size = 0;
  const bool ok =
      EVP_DigestFinal_ex(ctx, output, &output_size) == 1 && output_size == 32;
  EVP_MD_CTX_free(ctx);
  return ok;
}

static void encode_u32_le(uint32_t value, unsigned char output[4]) {
  for (size_t index = 0; index < 4; ++index)
    output[index] = (unsigned char)(value >> (8u * index));
}

static void encode_f64_le(double value, unsigned char output[8]) {
  uint64_t bits = 0;
  if (value == 0.0) value = 0.0;
  memcpy(&bits, &value, sizeof(bits));
  for (size_t index = 0; index < 8; ++index)
    output[index] = (unsigned char)(bits >> (8u * index));
}

static bool validation_binding(
    const Lardon3DCalibrationWorkflowInputBoundary *boundary,
    unsigned char output[32]) {
  static const char domain[] = "L3DCAL_WORKFLOW_VALIDATION_V1\n";
  EVP_MD_CTX *ctx = NULL;
  if (!digest_begin(&ctx, domain)) {
    if (ctx) EVP_MD_CTX_free(ctx);
    return false;
  }
  const bool ok =
      digest_add(ctx, boundary->detection_sha256, 32) &&
      digest_add(ctx, boundary->solve_sha256, 32) &&
      digest_add(ctx, boundary->evidence_sha256, 32) &&
      digest_add(ctx, boundary->producer_sha256, 32);
  if (!ok) {
    EVP_MD_CTX_free(ctx);
    return false;
  }
  return digest_finish(ctx, output);
}

static bool sample_identity(
    const Lardon3DCalibrationWorkflowExternalEvidence *external,
    unsigned char output[32]) {
  static const char domain[] = "L3DAF_CALIBRATION_SAMPLE_V1\n";
  EVP_MD_CTX *ctx = NULL;
  if (!digest_begin(&ctx, domain)) {
    if (ctx) EVP_MD_CTX_free(ctx);
    return false;
  }

  bool ok =
      digest_add(ctx, external->target_sha256, 32) &&
      digest_add(ctx, external->optical_state_sha256, 32) &&
      digest_add(ctx, external->solver_executable_sha256, 32) &&
      digest_add(ctx, external->solver_configuration_sha256, 32) &&
      digest_add(ctx, external->initialization_evidence_sha256, 32) &&
      digest_add(ctx, external->validation_evidence_sha256, 32);

  unsigned char encoded_u32[4];
  encode_u32_le(external->oriented_width, encoded_u32);
  ok = ok && digest_add(ctx, encoded_u32, sizeof(encoded_u32));
  encode_u32_le(external->oriented_height, encoded_u32);
  ok = ok && digest_add(ctx, encoded_u32, sizeof(encoded_u32));

  for (size_t parameter = 0; parameter < 8 && ok; ++parameter) {
    unsigned char encoded_f64[8];
    encode_f64_le(external->repeated_parameters[0][parameter], encoded_f64);
    ok = digest_add(ctx, encoded_f64, sizeof(encoded_f64));
  }

  if (!ok) {
    EVP_MD_CTX_free(ctx);
    return false;
  }
  return digest_finish(ctx, output);
}

static bool boundary_provenance_valid(
    const Lardon3DCalibrationWorkflowExternalEvidence *external) {
  const Lardon3DCalibrationWorkflowInputBoundary *boundary =
      &external->boundary;

  if (!nonzero_digest(boundary->session_sha256) ||
      !nonzero_digest(boundary->detection_sha256) ||
      !nonzero_digest(boundary->solve_sha256) ||
      !nonzero_digest(boundary->evidence_sha256) ||
      !nonzero_digest(boundary->producer_sha256) ||
      !nonzero_digest(boundary->optical_state_sha256) ||
      !nonzero_digest(boundary->solver_executable_sha256) ||
      !nonzero_digest(boundary->solver_configuration_sha256))
    return false;

  return memcmp(external->optical_state_sha256,
                boundary->optical_state_sha256, 32) == 0 &&
         memcmp(external->solver_executable_sha256,
                boundary->solver_executable_sha256, 32) == 0 &&
         memcmp(external->solver_configuration_sha256,
                boundary->solver_configuration_sha256, 32) == 0 &&
         memcmp(external->initialization_evidence_sha256,
                boundary->session_sha256, 32) == 0;
}

static bool published_parameters_valid(
    const Lardon3DCalibrationWorkflowExternalEvidence *external) {
  const double *published = external->repeated_parameters[0];
  for (size_t parameter = 0; parameter < 8; ++parameter) {
    if (!isfinite(published[parameter])) return false;
    if (external->repeated_parameters[1][parameter] != published[parameter] ||
        external->repeated_parameters[2][parameter] != published[parameter])
      return false;
  }

  return published[0] > 0.0 && published[1] > 0.0 &&
         published[2] >= 0.0 && published[3] >= 0.0 &&
         published[2] < (double)external->oriented_width &&
         published[3] < (double)external->oriented_height;
}

Lardon3DCalibrationAfStudyWorkflowResult
lardon3d_calibration_af_study_sample_from_materialized_evidence(
    const Lardon3DCalibrationWorkflowExternalEvidence *external,
    Lardon3DCalibrationAfStudySampleRole role, const char *focus_token,
    Lardon3DCalibrationAfStudySample *output) {
  if (output) memset(output, 0, sizeof(*output));

  if (!external || !focus_token || !output ||
      (role != LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT &&
       role != LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT))
    return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT;

  Lardon3DCalibrationAfStudySample sample = {0};
  if (!token_copy(focus_token, sample.focus_token))
    return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_ARGUMENT;

  if (external->oriented_width == 0 || external->oriented_height == 0 ||
      !nonzero_digest(external->target_sha256) ||
      !nonzero_digest(external->optical_state_sha256) ||
      !nonzero_digest(external->solver_executable_sha256) ||
      !nonzero_digest(external->solver_configuration_sha256) ||
      !nonzero_digest(external->initialization_evidence_sha256) ||
      !nonzero_digest(external->validation_evidence_sha256) ||
      !boundary_provenance_valid(external) ||
      !published_parameters_valid(external))
    return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE;

  unsigned char expected_validation[32] = {0};
  if (!validation_binding(&external->boundary, expected_validation))
    return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_CRYPTO_ERROR;
  if (memcmp(external->validation_evidence_sha256,
             expected_validation, 32) != 0)
    return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_INVALID_EVIDENCE;

  sample.role = role;
  if (!sample_identity(external, sample.calibration_evidence_sha256))
    return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_CRYPTO_ERROR;

  sample.fx = external->repeated_parameters[0][0];
  sample.fy = external->repeated_parameters[0][1];
  sample.cx = external->repeated_parameters[0][2];
  sample.cy = external->repeated_parameters[0][3];
  sample.k1 = external->repeated_parameters[0][4];
  sample.k2 = external->repeated_parameters[0][5];
  sample.p1 = external->repeated_parameters[0][6];
  sample.p2 = external->repeated_parameters[0][7];

  *output = sample;
  return LARDON3D_CALIBRATION_AF_STUDY_WORKFLOW_OK;
}
