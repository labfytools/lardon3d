#ifndef LARDON3D_SPARSE_SFM_INCREMENTAL_H
#define LARDON3D_SPARSE_SFM_INCREMENTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/sparse_sfm_geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LARDON3D_SPARSE_INCREMENTAL_COMPLETE = 0,
  LARDON3D_SPARSE_INCREMENTAL_PARTIAL,
  LARDON3D_SPARSE_INCREMENTAL_FAILED,
  LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT,
  LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY
} Lardon3DSparseIncrementalStatus;

typedef struct {
  uint64_t image_id;
  Lardon3DSparseGeometryCalibration calibration;
} Lardon3DSparseIncrementalImage;

typedef struct {
  uint64_t track_id;
  uint64_t image_id;
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint32_t feature_count;
  double x;
  double y;
} Lardon3DSparseIncrementalObservation;

typedef struct {
  uint64_t track_set_id;
  uint64_t calibration_scope_id;
  const Lardon3DSparseIncrementalImage *images;
  size_t image_count;
  const Lardon3DSparseIncrementalObservation *observations;
  size_t observation_count;
} Lardon3DSparseIncrementalInput;

typedef struct {
  uint32_t minimum_seed_tracks;
  uint32_t minimum_seed_landmarks;
  uint32_t minimum_pnp_correspondences;
  uint32_t maximum_seed_candidates;
  uint32_t maximum_registration_rounds;
  uint32_t maximum_landmarks_per_round;
  uint32_t maximum_images;
  uint64_t maximum_observations;
  uint64_t maximum_tracks;
  double reprojection_threshold_px;
  double minimum_track_parallax_rad;
  Lardon3DSparseGeometryRelativePoseParameters relative_pose;
  Lardon3DSparseGeometryPnPParameters pnp;
  Lardon3DSparseGeometryPointRefinementParameters refinement;
} Lardon3DSparseIncrementalParameters;

typedef struct {
  uint64_t image_id;
  uint64_t component_key;
  Lardon3DSparseGeometryPose pose_cw;
} Lardon3DSparseIncrementalCamera;

typedef struct {
  uint64_t landmark_id;
  uint64_t track_id;
  uint64_t component_key;
  Lardon3DSparseGeometryPoint3 point;
  double reprojection_rmse_px;
  double reprojection_median_px;
  uint64_t observation_count;
} Lardon3DSparseIncrementalLandmark;

typedef struct {
  uint64_t landmark_id;
  uint64_t track_id;
  uint64_t image_id;
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint32_t position_in_track;
} Lardon3DSparseIncrementalLandmarkObservation;

typedef struct {
  uint64_t component_key;
  uint64_t image_count;
  uint64_t registered_image_count;
  uint64_t landmark_count;
} Lardon3DSparseIncrementalComponent;

typedef struct {
  uint64_t image_id;
  uint64_t component_key;
} Lardon3DSparseIncrementalUnregisteredImage;

typedef struct {
  Lardon3DSparseIncrementalStatus status;
  uint64_t track_set_id;
  uint64_t calibration_scope_id;
  Lardon3DSparseIncrementalComponent *components;
  size_t component_count;
  Lardon3DSparseIncrementalCamera *cameras;
  size_t camera_count;
  Lardon3DSparseIncrementalLandmark *landmarks;
  size_t landmark_count;
  Lardon3DSparseIncrementalLandmarkObservation *observations;
  size_t observation_count;
  Lardon3DSparseIncrementalUnregisteredImage *unregistered_images;
  size_t unregistered_image_count;
  uint64_t seed_candidates_considered;
  uint64_t seed_candidates_available;
  uint64_t seed_image_a;
  uint64_t seed_image_b;
  int32_t last_seed_geometry_status;
  double last_seed_parallax_rad;
  uint64_t registration_rounds;
  uint64_t registration_attempts;
  uint64_t registration_successes;
  uint64_t registration_failures;
  uint32_t last_pnp_inlier_count;
  uint64_t triangulation_attempts;
  uint64_t triangulation_failures;
  uint64_t rejected_behind_camera;
  uint64_t rejected_reprojection;
  uint64_t rejected_landmarks;
  int32_t last_triangulation_status;
  uint64_t landmark_update_attempts;
  uint64_t landmark_update_successes;
  uint64_t landmark_update_failures;
  uint64_t no_growth_terminations;
  uint64_t round_limit_terminations;
  uint64_t point_refinement_attempts;
  uint64_t point_refinement_successes;
} Lardon3DSparseIncrementalResult;

bool lardon3d_sparse_incremental_parameters_default(
    Lardon3DSparseIncrementalParameters *parameters);

Lardon3DSparseIncrementalStatus lardon3d_sparse_incremental_run(
    const Lardon3DSparseIncrementalInput *input,
    const Lardon3DSparseIncrementalParameters *parameters,
    Lardon3DSparseIncrementalResult *result);

void lardon3d_sparse_incremental_result_destroy(
    Lardon3DSparseIncrementalResult *result);

#ifdef __cplusplus
}
#endif

#endif
