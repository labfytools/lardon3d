#ifndef LARDON3D_SPARSE_SFM_GEOMETRY_H
#define LARDON3D_SPARSE_SFM_GEOMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LARDON3D_SPARSE_GEOMETRY_OK = 0,
  LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT,
  LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT,
  LARDON3D_SPARSE_GEOMETRY_INSUFFICIENT_CORRESPONDENCES,
  LARDON3D_SPARSE_GEOMETRY_ESTIMATION_FAILED,
  LARDON3D_SPARSE_GEOMETRY_LOW_PARALLAX,
  LARDON3D_SPARSE_GEOMETRY_DEGENERATE,
  LARDON3D_SPARSE_GEOMETRY_CHEIRALITY_FAILED,
  LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE
} Lardon3DSparseGeometryResult;

typedef struct {
  uint32_t width;
  uint32_t height;
  double fx;
  double fy;
  double cx;
  double cy;
  double k1;
  double k2;
  double p1;
  double p2;
} Lardon3DSparseGeometryCalibration;

typedef struct {
  double x;
  double y;
} Lardon3DSparseGeometryPoint2;

typedef struct {
  double x;
  double y;
  double z;
} Lardon3DSparseGeometryPoint3;

typedef struct {
  double rotation_cw[9];
  double translation_cw[3];
} Lardon3DSparseGeometryPose;

typedef struct {
  double robust_threshold_px;
  double confidence;
  uint32_t max_iterations;
  uint32_t minimum_inliers;
  double minimum_inlier_ratio;
  double minimum_parallax_rad;
  double minimum_cheirality_ratio;
  uint64_t deterministic_seed;
} Lardon3DSparseGeometryRelativePoseParameters;

typedef struct {
  Lardon3DSparseGeometryPose pose_ba;
  uint32_t inlier_count;
  double inlier_ratio;
  double median_parallax_rad;
  uint8_t *inlier_mask;
  size_t inlier_mask_capacity;
} Lardon3DSparseGeometryRelativePoseResult;

typedef struct {
  double reprojection_threshold_px;
  double confidence;
  uint32_t max_iterations;
  uint32_t minimum_inliers;
  double minimum_inlier_ratio;
  uint64_t deterministic_seed;
} Lardon3DSparseGeometryPnPParameters;

typedef struct {
  Lardon3DSparseGeometryPose pose_cw;
  uint32_t inlier_count;
  double inlier_ratio;
  uint8_t *inlier_mask;
  size_t inlier_mask_capacity;
} Lardon3DSparseGeometryPnPResult;

typedef struct {
  uint32_t max_iterations;
  double convergence_tolerance;
} Lardon3DSparseGeometryPointRefinementParameters;

Lardon3DSparseGeometryResult lardon3d_sparse_geometry_normalize(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPoint2 *pixels, size_t count,
    Lardon3DSparseGeometryPoint2 *normalized);

Lardon3DSparseGeometryResult lardon3d_sparse_geometry_relative_pose(
    const Lardon3DSparseGeometryCalibration *calibration_a,
    const Lardon3DSparseGeometryCalibration *calibration_b,
    const Lardon3DSparseGeometryPoint2 *pixels_a,
    const Lardon3DSparseGeometryPoint2 *pixels_b, size_t count,
    const Lardon3DSparseGeometryRelativePoseParameters *parameters,
    Lardon3DSparseGeometryRelativePoseResult *result);

Lardon3DSparseGeometryResult lardon3d_sparse_geometry_triangulate_two_view(
    const Lardon3DSparseGeometryPoint2 *normalized_a,
    const Lardon3DSparseGeometryPoint2 *normalized_b,
    const Lardon3DSparseGeometryPose *pose_a,
    const Lardon3DSparseGeometryPose *pose_b,
    Lardon3DSparseGeometryPoint3 *point);

Lardon3DSparseGeometryResult lardon3d_sparse_geometry_triangulate_multi_view(
    const Lardon3DSparseGeometryPoint2 *normalized_points,
    const Lardon3DSparseGeometryPose *poses, size_t view_count,
    Lardon3DSparseGeometryPoint3 *point);

Lardon3DSparseGeometryResult lardon3d_sparse_geometry_refine_point(
    const Lardon3DSparseGeometryPoint2 *normalized_points,
    const Lardon3DSparseGeometryPose *poses, size_t view_count,
    const Lardon3DSparseGeometryPoint3 *initial_point,
    const Lardon3DSparseGeometryPointRefinementParameters *parameters,
    Lardon3DSparseGeometryPoint3 *refined_point);

Lardon3DSparseGeometryResult lardon3d_sparse_geometry_pnp(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPoint3 *points,
    const Lardon3DSparseGeometryPoint2 *pixels, size_t count,
    const Lardon3DSparseGeometryPnPParameters *parameters,
    Lardon3DSparseGeometryPnPResult *result);

#ifdef __cplusplus
}
#endif

#endif
