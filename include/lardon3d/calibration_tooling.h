#ifndef LARDON3D_CALIBRATION_TOOLING_H
#define LARDON3D_CALIBRATION_TOOLING_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/calibration_bootstrap.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_CALIBRATION_TOOLING_VERSION = 1,
  LARDON3D_CALIBRATION_TOOLING_MAX_VIEWS = 4096,
  LARDON3D_CALIBRATION_TOOLING_MAX_COORDINATE_CHECKS = 81920,
  LARDON3D_CALIBRATION_TOOLING_TARGET_MEASUREMENTS = 10,
  LARDON3D_CALIBRATION_TOOLING_VALIDATION_FLAGS = 15,
  LARDON3D_CALIBRATION_TOOLING_TARGET_CHARUCO_9X7_DICT_5X5_100 = 1,
};

typedef enum {
  LARDON3D_CALIBRATION_TOOLING_OK = 0,
  LARDON3D_CALIBRATION_TOOLING_INVALID_ARGUMENT,
  LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED,
  LARDON3D_CALIBRATION_TOOLING_CAPACITY,
  LARDON3D_CALIBRATION_TOOLING_ENCODING_ERROR,
  LARDON3D_CALIBRATION_TOOLING_IMPORT_ERROR,
} Lardon3DCalibrationToolingResult;

/* One solver-reported view. `quadrant` is 0..3 for frame quadrants and 4 for
 * the centre. `distance_band` is the declared near/mid/far band 0..2.
 * Rejected views retain their source identity and a nonzero rejection reason;
 * only accepted views contribute to the frozen acceptance statistics. The
 * manifest is caller-owned and borrowed only during validation/production. */
typedef struct {
  unsigned char source_sha256[32];
  uint32_t accepted;
  uint32_t rejection_reason;
  uint32_t holdout;
  uint32_t quadrant;
  uint32_t distance_band;
  uint32_t orientation_degrees;
  uint32_t target_corner_quadrant_mask;
  uint32_t corner_count;
  uint32_t residual_count;
  uint32_t high_residual_count;
  double target_occupancy;
  double normal_angle_degrees;
  double distance_metres;
  double corner_rms_px;
  double clipped_fraction;
  double reprojection_rmse_px;
  double maximum_residual_px;
} Lardon3DCalibrationToolingView;

/* Every selected image has exactly one artifact entry in selected-item order.
 * `fit_parameters` are the deterministic 80% fit result used to verify `maximum_parameter_delta`; the
 * published parameters are the complete-set result. */
typedef struct {
  uint64_t image_id;
  unsigned char representation_sha256[32];
  unsigned char optical_state_sha256[32];
  uint32_t width;
  uint32_t height;
  double fx, fy, cx, cy, k1, k2, p1, p2;
  double fit_fx, fit_fy, fit_cx, fit_cy, fit_k1, fit_k2, fit_p1, fit_p2;
  double repeated_parameters[3][8];
  uint32_t support_images;
  uint32_t support_observations;
  double reprojection_rmse_px;
  double maximum_parameter_delta;
  uint32_t validation_flags;
} Lardon3DCalibrationToolingEntry;

typedef struct {
  unsigned char source_sha256[32];
  uint32_t orientation_degrees;
  double dx_px;
  double dy_px;
} Lardon3DCalibrationToolingCoordinateCheck;

/* This is an operational, bounded view of a completed Science v1 bundle. WHY:
 * the solver remains external, so this API validates its immutable evidence
 * rather than importing a solver-private format. CONTRACT: all pointers are
 * borrowed; each digest is SHA-256; no field is persisted by this API except
 * through the frozen bootstrap importer. INVARIANT: entries are in selected
 * item order and never include Tracks, poses, or reconstruction results. */
typedef struct {
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
  /* Calibration Science v1 defines planarity as a categorical physical
   * attestation, not a numeric flatness tolerance. This legacy ABI field is
   * therefore a required NAN sentinel and MUST NOT carry an invented physical
   * measurement. The future workflow coordinator binds the canonical session
   * manifest containing `planarity PASS <sha256>` through
   * initialization_evidence_sha256. */
  double target_flatness_mm;
  double holdout_rmse_px;
  double holdout_maximum_residual_px;
  uint32_t extra_distortion_coefficient_count;
  const Lardon3DCalibrationToolingView *views;
  size_t view_count;
  const Lardon3DCalibrationToolingEntry *entries;
  size_t entry_count;
  const Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks;
  size_t coordinate_check_count;
} Lardon3DCalibrationToolingEvidence;

/* Validate all CALIBRATION_SCIENCE_V1 hard rejects. No allocations, DB access,
 * solver execution, or persistent writes occur. */
Lardon3DCalibrationToolingResult lardon3d_calibration_tooling_validate(
    const Lardon3DCalibrationToolingEvidence *evidence);

/* Encode exactly L3DCALB1 v1 into caller storage and return its SHA-256. The
 * output is deterministic for identical evidence. `written` may be NULL. */
Lardon3DCalibrationToolingResult lardon3d_calibration_tooling_produce(
    const Lardon3DCalibrationToolingEvidence *evidence, unsigned char *artifact,
    size_t artifact_capacity, size_t *written, unsigned char artifact_sha256[32]);

/* Validate, produce, then invoke only the frozen production importer. A failed
 * validation/encoding never reaches the DB. Import semantics, including its
 * permitted immutable-row recovery behavior, remain owned by that importer. */
Lardon3DCalibrationToolingResult lardon3d_calibration_tooling_import(
    Lardon3DProjectDb *database, uint64_t execution_id,
    const Lardon3DCalibrationToolingEvidence *evidence, unsigned char *artifact,
    size_t artifact_capacity, size_t *written, Lardon3DCalibrationBootstrapOutput *output);

#ifdef __cplusplus
}
#endif

#endif
