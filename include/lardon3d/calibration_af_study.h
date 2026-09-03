#ifndef LARDON3D_CALIBRATION_AF_STUDY_H
#define LARDON3D_CALIBRATION_AF_STUDY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_AF_STUDY_VERSION = 1,
  LARDON3D_CALIBRATION_AF_STUDY_PROBE_MODEL_VERSION = 1,
  LARDON3D_CALIBRATION_AF_STUDY_SHA256_SIZE = 32,
  LARDON3D_CALIBRATION_AF_STUDY_FOCUS_TOKEN_CAPACITY = 128,
  LARDON3D_CALIBRATION_AF_STUDY_MAX_SAMPLES = 64,
  LARDON3D_CALIBRATION_AF_STUDY_MAX_PAIRS = 2016,
  LARDON3D_CALIBRATION_AF_STUDY_MAX_ARTIFACT_BYTES = 131072,
};

typedef enum {
  LARDON3D_CALIBRATION_AF_STUDY_OK = 0,
  LARDON3D_CALIBRATION_AF_STUDY_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_AF_STUDY_CAPACITY,
  LARDON3D_CALIBRATION_AF_STUDY_INVALID_EVIDENCE,
  LARDON3D_CALIBRATION_AF_STUDY_ENCODING_ERROR,
} Lardon3DCalibrationAfStudyResult;

typedef enum {
  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_FIT = 1,
  LARDON3D_CALIBRATION_AF_STUDY_SAMPLE_HOLDOUT = 2,
} Lardon3DCalibrationAfStudySampleRole;

typedef struct {
  Lardon3DCalibrationAfStudySampleRole role;
  char focus_token[LARDON3D_CALIBRATION_AF_STUDY_FOCUS_TOKEN_CAPACITY];
  unsigned char calibration_evidence_sha256[LARDON3D_CALIBRATION_AF_STUDY_SHA256_SIZE];
  double fx;
  double fy;
  double cx;
  double cy;
  double k1;
  double k2;
  double p1;
  double p2;
} Lardon3DCalibrationAfStudySample;

typedef struct {
  unsigned char study_context_sha256[LARDON3D_CALIBRATION_AF_STUDY_SHA256_SIZE];
  uint32_t width;
  uint32_t height;
  const Lardon3DCalibrationAfStudySample *samples;
  size_t sample_count;
} Lardon3DCalibrationAfStudyInput;

typedef struct {
  uint32_t sample_count;
  uint32_t fit_count;
  uint32_t holdout_count;
  uint32_t pair_count;
  uint32_t same_focus_pair_count;
  uint32_t cross_focus_pair_count;
  uint32_t fit_holdout_pair_count;
  double all_center_max_px;
  double all_edge_probe_max_px;
  double all_corner_probe_max_px;
  double all_global_probe_max_px;
  double same_focus_global_probe_max_px;
  double cross_focus_global_probe_max_px;
  double fit_holdout_global_probe_max_px;
} Lardon3DCalibrationAfStudySummary;

/* Produce deterministic AF-study evidence from already acquired calibration
 * results. This API performs no calibration solve, no Project DB access and no
 * scientific PASS/FAIL decision. `study_context_sha256` is the caller-retained
 * identity of the exact body/lens/focal/non-focus geometric study context.
 * Focus tokens are opaque exact observations and may repeat across independent
 * calibration samples. A repeated exact (focus token, calibration evidence
 * SHA-256) pair is rejected because it is not independent evidence.
 *
 * Projection deltas use the frozen pinhole + k1/k2/p1/p2 forward model on nine
 * canonical normalized probes: centre, four edge probes and four corner probes
 * at +/-0.7. Metrics are measurements only; this v1 API freezes no acceptance
 * threshold. The binary L3DAFST1 artifact canonicalizes samples independent of
 * caller order, includes every pairwise metric, and is suitable for hashing as
 * retained evidence. */
Lardon3DCalibrationAfStudyResult lardon3d_calibration_af_study_produce(
    const Lardon3DCalibrationAfStudyInput *input,
    unsigned char *artifact, size_t artifact_capacity, size_t *written,
    unsigned char artifact_sha256[LARDON3D_CALIBRATION_AF_STUDY_SHA256_SIZE],
    Lardon3DCalibrationAfStudySummary *summary);

#ifdef __cplusplus
}
#endif

#endif
