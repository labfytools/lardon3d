#include <lardon3d/calibration_af_study.h>

#include <math.h>
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

static Lardon3DCalibrationAfStudySample make_sample(
    const char *focus_token, unsigned char digest_value, double focal_px,
    Lardon3DCalibrationAfStudySampleRole role) {
  Lardon3DCalibrationAfStudySample sample = {0};
  sample.role = role;
  (void)snprintf(sample.focus_token, sizeof(sample.focus_token), "%s",
                 focus_token);
  fill_digest(sample.calibration_evidence_sha256, digest_value);
  sample.fx = focal_px;
  sample.fy = focal_px + 2.0;
  sample.cx = 3000.0;
  sample.cy = 2000.0;
  sample.k1 = -0.1;
  sample.k2 = 0.01;
  sample.p1 = 0.001;
  sample.p2 = -0.001;
  return sample;
}

static Lardon3DCalibrationAfStudyInput make_input(
    Lardon3DCalibrationAfStudySample *samples, size_t sample_count) {
  Lardon3DCalibrationAfStudyInput input = {0};
  fill_digest(input.study_context_sha256, 0x91);
  input.width = 6000;
  input.height = 4000;
  input.samples = samples;
  input.sample_count = sample_count;
  return input;
}

static int test_deterministic_and_summary(void) {
  Lardon3DCalibrationAfStudySample samples[3] = {
      make_sample("sony-focus:137", 0x11, 4000.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT),
      make_sample("sony-focus:137", 0x22, 4001.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT),
      make_sample("sony-focus:165", 0x33, 4010.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT),
  };
  Lardon3DCalibrationAfStudyInput input = make_input(samples, 3);

  unsigned char artifact_a[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char artifact_b[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha_a[32];
  unsigned char sha_b[32];
  size_t written_a = 0;
  size_t written_b = 0;
  Lardon3DCalibrationAfStudySummary summary_a;
  Lardon3DCalibrationAfStudySummary summary_b;

  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact_a, sizeof(artifact_a), &written_a, sha_a,
            &summary_a) == LARDON3D_CALIBRATION_AF_STUDY_OK);
  CHECK(written_a == 900);
  CHECK(memcmp(artifact_a, "L3DAFST1", 8) == 0);
  CHECK(summary_a.sample_count == 3);
  CHECK(summary_a.fit_count == 2);
  CHECK(summary_a.holdout_count == 1);
  CHECK(summary_a.pair_count == 3);
  CHECK(summary_a.same_focus_pair_count == 1);
  CHECK(summary_a.cross_focus_pair_count == 2);
  CHECK(summary_a.fit_holdout_pair_count == 2);
  CHECK(summary_a.all_global_probe_max_px > 0.0);
  CHECK(summary_a.same_focus_global_probe_max_px > 0.0);
  CHECK(summary_a.cross_focus_global_probe_max_px >
        summary_a.same_focus_global_probe_max_px);
  CHECK(summary_a.fit_holdout_global_probe_max_px > 0.0);

  Lardon3DCalibrationAfStudySample reordered[3] = {
      samples[2], samples[0], samples[1],
  };
  input.samples = reordered;
  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact_b, sizeof(artifact_b), &written_b, sha_b,
            &summary_b) == LARDON3D_CALIBRATION_AF_STUDY_OK);
  CHECK(written_a == written_b);
  CHECK(memcmp(artifact_a, artifact_b, written_a) == 0);
  CHECK(memcmp(sha_a, sha_b, 32) == 0);
  CHECK(memcmp(&summary_a, &summary_b, sizeof(summary_a)) == 0);
  return 0;
}

static int test_capacity_is_failure_atomic_for_outputs(void) {
  Lardon3DCalibrationAfStudySample samples[2] = {
      make_sample("focus:a", 0x11, 4000.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT),
      make_sample("focus:b", 0x22, 4005.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT),
  };
  Lardon3DCalibrationAfStudyInput input = make_input(samples, 2);
  unsigned char artifact[16] = {0};
  unsigned char sha[32];
  memset(sha, 0xA5, sizeof(sha));
  size_t written = 999;
  Lardon3DCalibrationAfStudySummary summary;
  memset(&summary, 0xA5, sizeof(summary));

  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_CAPACITY);
  CHECK(written == 0);
  unsigned char zero[32] = {0};
  CHECK(memcmp(sha, zero, 32) == 0);
  Lardon3DCalibrationAfStudySummary zero_summary = {0};
  CHECK(memcmp(&summary, &zero_summary, sizeof(summary)) == 0);
  return 0;
}

static int test_duplicate_evidence_rejected(void) {
  Lardon3DCalibrationAfStudySample samples[2] = {
      make_sample("focus:repeat", 0x11, 4000.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT),
      make_sample("focus:repeat", 0x11, 4001.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT),
  };
  Lardon3DCalibrationAfStudyInput input = make_input(samples, 2);
  unsigned char artifact[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha[32];
  size_t written = 0;
  Lardon3DCalibrationAfStudySummary summary;
  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE);
  return 0;
}

static int test_invalid_values_rejected(void) {
  Lardon3DCalibrationAfStudySample samples[2] = {
      make_sample("focus:a", 0x11, 4000.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT),
      make_sample("focus:b", 0x22, 4005.0,
                  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT),
  };
  Lardon3DCalibrationAfStudyInput input = make_input(samples, 2);
  unsigned char artifact[LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES];
  unsigned char sha[32];
  size_t written = 0;
  Lardon3DCalibrationAfStudySummary summary;

  samples[0].fx = NAN;
  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE);
  samples[0] = make_sample("focus:a", 0x11, 4000.0,
                           LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT);

  memset(samples[0].focus_token, 'x', sizeof(samples[0].focus_token));
  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE);
  samples[0] = make_sample("focus:a", 0x11, 4000.0,
                           LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT);

  memset(input.study_context_sha256, 0, sizeof(input.study_context_sha256));
  CHECK(lardon3d_calibration_af_study_produce(
            &input, artifact, sizeof(artifact), &written, sha, &summary) ==
        LARDON3D_CALIBRATION_AF_STUDY_INVALID_ARGUMENT);
  return 0;
}

int main(void) {
  CHECK(test_deterministic_and_summary() == 0);
  CHECK(test_capacity_is_failure_atomic_for_outputs() == 0);
  CHECK(test_duplicate_evidence_rejected() == 0);
  CHECK(test_invalid_values_rejected() == 0);
  puts("CALIBRATION_AF_STUDY_EVIDENCE_V1=PASS");
  return 0;
}
