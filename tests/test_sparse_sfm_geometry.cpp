#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <lardon3d/sparse_sfm_geometry.h>

#define CHECK(value)                                                            \
  do {                                                                          \
    if (!(value)) {                                                             \
      std::fprintf(stderr, "geometry failure line %d: %s\n", __LINE__, #value); \
      return 1;                                                                 \
    }                                                                            \
  } while (0)

static Lardon3DSparseGeometryPoint2 project(
    const Lardon3DSparseGeometryCalibration &calibration,
    const Lardon3DSparseGeometryPoint3 &point,
    const Lardon3DSparseGeometryPose &pose) {
  double x = pose.rotation_cw[0] * point.x + pose.rotation_cw[1] * point.y +
             pose.rotation_cw[2] * point.z + pose.translation_cw[0];
  double y = pose.rotation_cw[3] * point.x + pose.rotation_cw[4] * point.y +
             pose.rotation_cw[5] * point.z + pose.translation_cw[1];
  double z = pose.rotation_cw[6] * point.x + pose.rotation_cw[7] * point.y +
             pose.rotation_cw[8] * point.z + pose.translation_cw[2];
  Lardon3DSparseGeometryPoint2 projected = {calibration.fx * x / z + calibration.cx,
                                            calibration.fy * y / z + calibration.cy};
  return projected;
}

static double matrix_noise(size_t index, double amplitude) {
  int value = static_cast<int>((index * 37U + 11U) % 17U) - 8;
  return amplitude * static_cast<double>(value) / 8.0;
}

static double rotation_error(const Lardon3DSparseGeometryPose &pose) {
  double trace = pose.rotation_cw[0] + pose.rotation_cw[4] +
                 pose.rotation_cw[8];
  return std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0));
}

static double translation_direction_error(
    const Lardon3DSparseGeometryPose &pose, double x, double y, double z) {
  double norm = std::sqrt(pose.translation_cw[0] * pose.translation_cw[0] +
                          pose.translation_cw[1] * pose.translation_cw[1] +
                          pose.translation_cw[2] * pose.translation_cw[2]);
  double target_norm = std::sqrt(x * x + y * y + z * z);
  double dot = (pose.translation_cw[0] * x + pose.translation_cw[1] * y +
                pose.translation_cw[2] * z) /
               (norm * target_norm);
  return std::acos(std::clamp(dot, -1.0, 1.0));
}

static int matrix_test() {
  Lardon3DSparseGeometryCalibration calibration =
      {1280, 960, 800, 800, 640, 480, 0, 0, 0, 0};
  Lardon3DSparseGeometryPose pose_a = {{1, 0, 0, 0, 1, 0, 0, 0, 1},
                                       {0, 0, 0}};
  Lardon3DSparseGeometryPose pose_b = pose_a;
  pose_b.translation_cw[0] = 1.0;
  double pnp_worst_rotation_error = 0.0;
  double pnp_worst_translation_error = 0.0;
  double pnp_worst_precision = 1.0;
  double pnp_worst_recall = 1.0;
  constexpr size_t count = 64;
  Lardon3DSparseGeometryPoint2 pixels_a[count];
  Lardon3DSparseGeometryPoint2 clean_b[count];
  for (size_t index = 0; index < count; ++index) {
    Lardon3DSparseGeometryPoint3 point = {
        -1.0 + static_cast<double>(index % 8) * 0.28,
        -0.7 + static_cast<double>(index / 8) * 0.18,
        4.0 + static_cast<double>(index % 11) * 0.2};
    pixels_a[index] = project(calibration, point, pose_a);
    clean_b[index] = project(calibration, point, pose_b);
  }
  const double noises[] = {0.0, 0.25, 0.5, 1.5};
  const double outlier_rates[] = {0.0, 0.1, 0.25, 0.4};
  for (double noise : noises) {
    for (double outlier_rate : outlier_rates) {
      Lardon3DSparseGeometryPoint2 pixels_b[count];
      size_t outliers = static_cast<size_t>(count * outlier_rate);
      for (size_t index = 0; index < count; ++index) {
        pixels_b[index] = clean_b[index];
        pixels_b[index].x += matrix_noise(index, noise);
        pixels_b[index].y += matrix_noise(index + 19, noise);
        if (index >= count - outliers) {
          pixels_b[index].x = 80.0 + static_cast<double>(index * 31U % 1100U);
          pixels_b[index].y = 60.0 + static_cast<double>(index * 17U % 800U);
        }
      }
      uint8_t mask[count] = {0};
      Lardon3DSparseGeometryRelativePoseParameters parameters =
          {1.5, 0.999, 1500, 24, 0.5, 1e-4, 0.5, 100 +
                                                   static_cast<uint64_t>(noise * 10)};
      Lardon3DSparseGeometryRelativePoseResult result = {};
      result.inlier_mask = mask;
      result.inlier_mask_capacity = count;
      Lardon3DSparseGeometryResult status =
          lardon3d_sparse_geometry_relative_pose(
              &calibration, &calibration, pixels_a, pixels_b, count,
              &parameters, &result);
      if (noise == 0.0 && outlier_rate == 0.0) {
        CHECK(status == LARDON3D_SPARSE_GEOMETRY_OK);
        CHECK(rotation_error(result.pose_ba) < 0.02);
        CHECK(translation_direction_error(result.pose_ba, 1, 0, 0) < 0.05);
      }
      if (status == LARDON3D_SPARSE_GEOMETRY_OK && noise <= 0.75 &&
          outlier_rate <= 0.25) {
        CHECK(rotation_error(result.pose_ba) < 0.25);
        CHECK(translation_direction_error(result.pose_ba, 1, 0, 0) < 0.35);
        CHECK(result.inlier_count >= 24);
      }
      CHECK(status != LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT);
    }
  }
  Lardon3DSparseGeometryPoint2 collinear_a[count];
  Lardon3DSparseGeometryPoint2 collinear_b[count];
  for (size_t index = 0; index < count; ++index) {
    collinear_a[index] = {500.0 + static_cast<double>(index), 480.0};
    collinear_b[index] = {500.0 + static_cast<double>(index), 480.0};
  }
  uint8_t mask[count] = {0};
  Lardon3DSparseGeometryRelativePoseParameters parameters =
      {1.0, 0.999, 500, 8, 0.5, 1e-4, 0.5, 77};
  Lardon3DSparseGeometryRelativePoseResult result = {};
  result.inlier_mask = mask;
  result.inlier_mask_capacity = count;
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, collinear_a, collinear_b, count,
            &parameters, &result) != LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, pixels_a, clean_b, 4, &parameters,
            &result) == LARDON3D_SPARSE_GEOMETRY_INSUFFICIENT_CORRESPONDENCES);
  Lardon3DSparseGeometryPoint2 nan_point = {NAN, 0};
  CHECK(lardon3d_sparse_geometry_normalize(&calibration, &nan_point, 1,
                                           &nan_point) ==
        LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT);
  Lardon3DSparseGeometryPose forward = pose_b;
  forward.translation_cw[0] = 0;
  forward.translation_cw[2] = 0.01;
  for (size_t index = 0; index < count; ++index)
    clean_b[index] = project(calibration,
                             {-1.0 + static_cast<double>(index % 8) * 0.28,
                              -0.7 + static_cast<double>(index / 8) * 0.18,
                              4.0 + static_cast<double>(index % 11) * 0.2},
                             forward);
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, pixels_a, clean_b, count, &parameters,
            &result) != LARDON3D_SPARSE_GEOMETRY_OK);
  const size_t pnp_count = 32;
  Lardon3DSparseGeometryPoint3 pnp_points[pnp_count];
  Lardon3DSparseGeometryPoint2 pnp_pixels[pnp_count];
  for (size_t index = 0; index < pnp_count; ++index) {
    pnp_points[index] = {-1.0 + static_cast<double>(index % 8) * 0.3,
                         -0.7 + static_cast<double>(index / 8) * 0.2,
                         4.0 + static_cast<double>(index % 5) * 0.3};
    pnp_pixels[index] = project(calibration, pnp_points[index], pose_b);
  }
  uint8_t pnp_mask[pnp_count] = {0};
  Lardon3DSparseGeometryPnPParameters pnp_parameters =
      {1.5, 0.999, 1000, 12, 0.5, 404};
  Lardon3DSparseGeometryPnPResult pnp_result = {};
  pnp_result.inlier_mask = pnp_mask;
  pnp_result.inlier_mask_capacity = pnp_count;
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, pnp_points, pnp_pixels, pnp_count,
            &pnp_parameters, &pnp_result) == LARDON3D_SPARSE_GEOMETRY_OK);
  const double pnp_noises[] = {0.0, 0.25, 0.75, 2.0};
  const double pnp_outliers[] = {0.0, 0.125, 0.25, 0.4};
  for (double noise : pnp_noises) {
    for (double outlier_rate : pnp_outliers) {
      Lardon3DSparseGeometryPoint2 altered[pnp_count];
      size_t outlier_count = static_cast<size_t>(pnp_count * outlier_rate);
      for (size_t index = 0; index < pnp_count; ++index) {
        altered[index] = pnp_pixels[index];
        altered[index].x += matrix_noise(index, noise);
        altered[index].y += matrix_noise(index + 31, noise);
        if (index >= pnp_count - outlier_count) {
          altered[index].x = 100.0 + static_cast<double>(index * 41U % 1000U);
          altered[index].y = 100.0 + static_cast<double>(index * 23U % 700U);
        }
      }
      Lardon3DSparseGeometryResult status = lardon3d_sparse_geometry_pnp(
          &calibration, pnp_points, altered, pnp_count, &pnp_parameters,
          &pnp_result);
      CHECK(status != LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT);
      if (status == LARDON3D_SPARSE_GEOMETRY_OK) {
        size_t true_positive = 0;
        size_t false_positive = 0;
        size_t false_negative = 0;
        for (size_t index = 0; index < pnp_count; ++index) {
          bool expected_inlier = index < pnp_count - outlier_count;
          bool actual_inlier = pnp_mask[index] != 0;
          if (expected_inlier && actual_inlier)
            ++true_positive;
          else if (!expected_inlier && actual_inlier)
            ++false_positive;
          else if (expected_inlier)
            ++false_negative;
        }
        double precision = true_positive == 0
                               ? 0.0
                               : static_cast<double>(true_positive) /
                                     static_cast<double>(true_positive + false_positive);
        double recall = static_cast<double>(true_positive) /
                        static_cast<double>(true_positive + false_negative);
        pnp_worst_precision = std::min(pnp_worst_precision, precision);
        pnp_worst_recall = std::min(pnp_worst_recall, recall);
        pnp_worst_rotation_error =
            std::max(pnp_worst_rotation_error, rotation_error(pnp_result.pose_cw));
        pnp_worst_translation_error = std::max(
            pnp_worst_translation_error,
            std::sqrt((pnp_result.pose_cw.translation_cw[0] - 1.0) *
                          (pnp_result.pose_cw.translation_cw[0] - 1.0) +
                      pnp_result.pose_cw.translation_cw[1] *
                          pnp_result.pose_cw.translation_cw[1] +
                      pnp_result.pose_cw.translation_cw[2] *
                          pnp_result.pose_cw.translation_cw[2]));
      }
    }
  }
  Lardon3DSparseGeometryPoint3 collinear_points[pnp_count];
  Lardon3DSparseGeometryPoint2 collinear_pixels[pnp_count];
  for (size_t index = 0; index < pnp_count; ++index) {
    collinear_points[index] = {static_cast<double>(index) * 0.1, 0, 4};
    collinear_pixels[index] = project(calibration, collinear_points[index], pose_b);
  }
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, collinear_points, collinear_pixels, pnp_count,
            &pnp_parameters, &pnp_result) ==
        LARDON3D_SPARSE_GEOMETRY_DEGENERATE);
  Lardon3DSparseGeometryPoint3 duplicate_points[pnp_count];
  Lardon3DSparseGeometryPoint2 duplicate_pixels[pnp_count];
  for (size_t index = 0; index < pnp_count; ++index) {
    duplicate_points[index] = pnp_points[0];
    duplicate_pixels[index] = pnp_pixels[0];
  }
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, duplicate_points, duplicate_pixels, pnp_count,
            &pnp_parameters, &pnp_result) ==
        LARDON3D_SPARSE_GEOMETRY_DEGENERATE);
  Lardon3DSparseGeometryPoint3 planar_points[pnp_count];
  Lardon3DSparseGeometryPoint3 near_planar_points[pnp_count];
  Lardon3DSparseGeometryPoint3 far_points[pnp_count];
  Lardon3DSparseGeometryPoint2 planar_pixels[pnp_count];
  Lardon3DSparseGeometryPoint2 near_planar_pixels[pnp_count];
  Lardon3DSparseGeometryPoint2 far_pixels[pnp_count];
  for (size_t index = 0; index < pnp_count; ++index) {
    double x = -1.0 + static_cast<double>(index % 8) * 0.3;
    double y = -0.7 + static_cast<double>(index / 8) * 0.2;
    planar_points[index] = {x, y, 4.0};
    near_planar_points[index] = {x, y, 4.0 + static_cast<double>(index % 3) * 1e-4};
    far_points[index] = {x, y, 1000.0 + static_cast<double>(index % 3)};
    planar_pixels[index] = project(calibration, planar_points[index], pose_b);
    near_planar_pixels[index] =
        project(calibration, near_planar_points[index], pose_b);
    far_pixels[index] = project(calibration, far_points[index], pose_b);
  }
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, planar_points, planar_pixels, pnp_count,
            &pnp_parameters, &pnp_result) == LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, near_planar_points, near_planar_pixels, pnp_count,
            &pnp_parameters, &pnp_result) !=
        LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT);
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, far_points, far_pixels, pnp_count, &pnp_parameters,
            &pnp_result) != LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT);
  for (size_t view_count : {size_t(2), size_t(3), size_t(5), size_t(10)}) {
    Lardon3DSparseGeometryPoint2 observations[10];
    Lardon3DSparseGeometryPose poses[10];
    Lardon3DSparseGeometryPoint3 truth_point = {0.2, -0.1, 4.0};
    for (size_t view = 0; view < view_count; ++view) {
      poses[view] = pose_a;
      poses[view].translation_cw[0] = static_cast<double>(view) * 0.5;
      observations[view] = project(calibration, truth_point, poses[view]);
      CHECK(lardon3d_sparse_geometry_normalize(
                &calibration, &observations[view], 1, &observations[view]) ==
            LARDON3D_SPARSE_GEOMETRY_OK);
    }
    Lardon3DSparseGeometryPoint3 multi;
    CHECK(lardon3d_sparse_geometry_triangulate_multi_view(
              observations, poses, view_count, &multi) ==
          LARDON3D_SPARSE_GEOMETRY_OK);
    CHECK(std::abs(multi.z - truth_point.z) < 1e-7);
  }
  Lardon3DSparseGeometryPoint2 planar_a[count];
  Lardon3DSparseGeometryPoint2 planar_b[count];
  Lardon3DSparseGeometryPoint2 far_a[count];
  Lardon3DSparseGeometryPoint2 far_b[count];
  Lardon3DSparseGeometryPose far_pose = pose_b;
  for (size_t index = 0; index < count; ++index) {
    Lardon3DSparseGeometryPoint3 planar_point = {
        -1.0 + static_cast<double>(index % 8) * 0.28,
        -0.7 + static_cast<double>(index / 8) * 0.18, 4.0};
    Lardon3DSparseGeometryPoint3 far_point = {planar_point.x, planar_point.y,
                                              10000.0};
    planar_a[index] = project(calibration, planar_point, pose_a);
    planar_b[index] = project(calibration, planar_point, pose_b);
    far_a[index] = project(calibration, far_point, pose_a);
    far_b[index] = project(calibration, far_point, far_pose);
  }
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, planar_a, planar_b, count, &parameters,
            &result) != LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT);
  parameters.minimum_parallax_rad = 1e-4;
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, far_a, far_b, count, &parameters,
            &result) != LARDON3D_SPARSE_GEOMETRY_OK);
  std::printf("pnp_worst_rotation=%.17g pnp_worst_translation=%.17g "
              "pnp_worst_precision=%.17g pnp_worst_recall=%.17g\n",
              pnp_worst_rotation_error, pnp_worst_translation_error,
              pnp_worst_precision, pnp_worst_recall);
  return 0;
}

static int multiview_matrix_test() {
  const Lardon3DSparseGeometryCalibration calibration =
      {1280, 960, 800, 800, 640, 480, 0, 0, 0, 0};
  const Lardon3DSparseGeometryPoint3 truth = {0.35, -0.2, 4.0};
  const size_t view_counts[] = {2, 3, 5, 10};
  double worst_error = 0.0;
  double worst_mean_reprojection = 0.0;
  double worst_max_reprojection = 0.0;
  double lowest_accepted_parallax = std::numeric_limits<double>::max();
  double highest_rejected_parallax = 0.0;
  for (size_t view_count : view_counts) {
    const double noise_levels[] = {0.0, 0.25, 0.75, 1.5};
    for (size_t noise_index = 0; noise_index < 4; ++noise_index) {
      Lardon3DSparseGeometryPoint2 observations[10];
      Lardon3DSparseGeometryPose poses[10];
      for (size_t view = 0; view < view_count; ++view) {
        poses[view] = {{1, 0, 0, 0, 1, 0, 0, 0, 1},
                       {static_cast<double>(view) * 0.5,
                        static_cast<double>(view % 2) * 0.2, 0}};
        observations[view] = project(calibration, truth, poses[view]);
        observations[view].x +=
            matrix_noise(view + noise_index * 17, noise_levels[noise_index]);
        observations[view].y +=
            matrix_noise(view + noise_index * 23, noise_levels[noise_index]);
        CHECK(lardon3d_sparse_geometry_normalize(
                  &calibration, &observations[view], 1, &observations[view]) ==
              LARDON3D_SPARSE_GEOMETRY_OK);
      }
      Lardon3DSparseGeometryPoint3 output;
      Lardon3DSparseGeometryResult status =
          lardon3d_sparse_geometry_triangulate_multi_view(
              observations, poses, view_count, &output);
      CHECK(status == LARDON3D_SPARSE_GEOMETRY_OK);
      CHECK(std::isfinite(output.x) && std::isfinite(output.y) &&
            std::isfinite(output.z));
      double error = std::sqrt((output.x - truth.x) * (output.x - truth.x) +
                               (output.y - truth.y) * (output.y - truth.y) +
                               (output.z - truth.z) * (output.z - truth.z));
      double mean_reprojection = 0.0;
      double max_reprojection = 0.0;
      for (size_t view = 0; view < view_count; ++view) {
        Lardon3DSparseGeometryPoint3 camera_point = output;
        camera_point.x += poses[view].translation_cw[0];
        camera_point.y += poses[view].translation_cw[1];
        camera_point.z += poses[view].translation_cw[2];
        double dx = camera_point.x / camera_point.z - observations[view].x;
        double dy = camera_point.y / camera_point.z - observations[view].y;
        double reprojection = std::hypot(dx, dy);
        mean_reprojection += reprojection;
        max_reprojection = std::max(max_reprojection, reprojection);
      }
      mean_reprojection /= static_cast<double>(view_count);
      worst_error = std::max(worst_error, error);
      worst_mean_reprojection =
          std::max(worst_mean_reprojection, mean_reprojection);
      worst_max_reprojection = std::max(worst_max_reprojection, max_reprojection);
      Lardon3DSparseGeometryPoint3 repeated;
      CHECK(lardon3d_sparse_geometry_triangulate_multi_view(
                observations, poses, view_count, &repeated) ==
            LARDON3D_SPARSE_GEOMETRY_OK);
      CHECK(std::memcmp(&output, &repeated, sizeof(output)) == 0);
    }
    const double boundary_baselines[] = {0.001, 0.00045, 0.00035, 0.00005};
    for (double baseline : boundary_baselines) {
      Lardon3DSparseGeometryPoint2 observations[10];
      Lardon3DSparseGeometryPose poses[10];
      for (size_t view = 0; view < view_count; ++view) {
        poses[view] = {{1, 0, 0, 0, 1, 0, 0, 0, 1},
                       {static_cast<double>(view) * baseline, 0, 0}};
        observations[view] = project(calibration, truth, poses[view]);
        CHECK(lardon3d_sparse_geometry_normalize(
                  &calibration, &observations[view], 1, &observations[view]) ==
              LARDON3D_SPARSE_GEOMETRY_OK);
      }
      Lardon3DSparseGeometryPoint3 output;
      Lardon3DSparseGeometryResult status =
          lardon3d_sparse_geometry_triangulate_multi_view(
              observations, poses, view_count, &output);
      double effective_parallax = baseline / truth.z;
      if (effective_parallax >= 1e-4) {
        CHECK(status == LARDON3D_SPARSE_GEOMETRY_OK);
        lowest_accepted_parallax =
            std::min(lowest_accepted_parallax, effective_parallax);
      } else {
        CHECK(status == LARDON3D_SPARSE_GEOMETRY_LOW_PARALLAX);
        highest_rejected_parallax =
            std::max(highest_rejected_parallax, effective_parallax);
      }
    }
    Lardon3DSparseGeometryPoint2 observations[10];
    Lardon3DSparseGeometryPose poses[10];
    Lardon3DSparseGeometryPoint3 far_truth = {0.35, -0.2, 10000.0};
    for (size_t view = 0; view < view_count; ++view) {
      poses[view] = {{1, 0, 0, 0, 1, 0, 0, 0, 1},
                     {static_cast<double>(view) * 2.0, 0, 0}};
      observations[view] = project(calibration, far_truth, poses[view]);
      CHECK(lardon3d_sparse_geometry_normalize(
                &calibration, &observations[view], 1, &observations[view]) ==
            LARDON3D_SPARSE_GEOMETRY_OK);
    }
    Lardon3DSparseGeometryPoint3 output;
    CHECK(lardon3d_sparse_geometry_triangulate_multi_view(
              observations, poses, view_count, &output) ==
          LARDON3D_SPARSE_GEOMETRY_OK);
    CHECK(std::isfinite(output.z));
  }
  std::printf("multi_view_worst_error=%.17g mean_reprojection=%.17g "
              "max_reprojection=%.17g lowest_parallax=%.17g "
              "highest_rejected_parallax=%.17g\n",
              worst_error, worst_mean_reprojection, worst_max_reprojection,
              lowest_accepted_parallax, highest_rejected_parallax);
  return 0;
}

static int resource_test() {
  Lardon3DSparseGeometryCalibration calibration =
      {1280, 960, 800, 800, 640, 480, 0, 0, 0, 0};
  Lardon3DSparseGeometryPose pose_a = {{1, 0, 0, 0, 1, 0, 0, 0, 1},
                                       {0, 0, 0}};
  Lardon3DSparseGeometryPose pose_b = pose_a;
  pose_b.translation_cw[0] = 1.0;
  const size_t relative_count = 8192;
  std::vector<Lardon3DSparseGeometryPoint2> pixels_a(relative_count);
  std::vector<Lardon3DSparseGeometryPoint2> pixels_b(relative_count);
  for (size_t index = 0; index < relative_count; ++index) {
    Lardon3DSparseGeometryPoint3 point = {
        -1.0 + static_cast<double>(index % 128) * 0.015,
        -0.8 + static_cast<double>((index / 128) % 64) * 0.025,
        4.0 + static_cast<double>(index % 17) * 0.1};
    pixels_a[index] = project(calibration, point, pose_a);
    pixels_b[index] = project(calibration, point, pose_b);
  }
  std::vector<uint8_t> mask(relative_count);
  Lardon3DSparseGeometryRelativePoseParameters relative_parameters =
      {1.0, 0.999, 1000, 32, 0.1, 1e-5, 0.5, 99};
  Lardon3DSparseGeometryRelativePoseResult relative = {};
  relative.inlier_mask = mask.data();
  relative.inlier_mask_capacity = mask.size();
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, pixels_a.data(), pixels_b.data(),
            relative_count, &relative_parameters, &relative) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  const size_t pnp_count = 2000;
  std::vector<Lardon3DSparseGeometryPoint3> points(pnp_count);
  std::vector<Lardon3DSparseGeometryPoint2> pnp_pixels(pnp_count);
  for (size_t index = 0; index < pnp_count; ++index) {
    points[index] = {-1.0 + static_cast<double>(index % 100) * 0.02,
                     -0.8 + static_cast<double>((index / 100) % 20) * 0.03,
                     4.0 + static_cast<double>(index % 13) * 0.1};
    pnp_pixels[index] = project(calibration, points[index], pose_b);
  }
  std::vector<uint8_t> pnp_mask(pnp_count);
  Lardon3DSparseGeometryPnPParameters pnp_parameters =
      {1.0, 0.999, 1000, 32, 0.1, 99};
  Lardon3DSparseGeometryPnPResult pnp = {};
  pnp.inlier_mask = pnp_mask.data();
  pnp.inlier_mask_capacity = pnp_mask.size();
  CHECK(lardon3d_sparse_geometry_pnp(
            &calibration, points.data(), pnp_pixels.data(), pnp_count,
            &pnp_parameters, &pnp) == LARDON3D_SPARSE_GEOMETRY_OK);
  Lardon3DSparseGeometryPoint2 normalized_a;
  Lardon3DSparseGeometryPoint2 normalized_b;
  CHECK(lardon3d_sparse_geometry_normalize(&calibration, &pixels_a[0], 1,
                                           &normalized_a) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(lardon3d_sparse_geometry_normalize(&calibration, &pixels_b[0], 1,
                                           &normalized_b) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  Lardon3DSparseGeometryPoint3 output;
  for (size_t index = 0; index < 100000; ++index)
    CHECK(lardon3d_sparse_geometry_triangulate_two_view(
              &normalized_a, &normalized_b, &pose_a, &pose_b, &output) ==
          LARDON3D_SPARSE_GEOMETRY_OK);
  std::printf("relative=%zu inliers=%u pose=%.17g,%.17g,%.17g pnp=%zu "
              "pnp_inliers=%u pnp_pose=%.17g,%.17g,%.17g triangulations=%d\n",
              relative_count, relative.inlier_count,
              relative.pose_ba.translation_cw[0],
              relative.pose_ba.translation_cw[1],
              relative.pose_ba.translation_cw[2], pnp_count,
              pnp.inlier_count, pnp.pose_cw.translation_cw[0],
              pnp.pose_cw.translation_cw[1], pnp.pose_cw.translation_cw[2],
              100000);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1)
    if (std::strcmp(argv[1], "matrix") == 0)
      return matrix_test();
  if (argc > 1)
    if (std::strcmp(argv[1], "multiview-matrix") == 0)
      return multiview_matrix_test();
  if (argc > 1)
    return resource_test();
  Lardon3DSparseGeometryCalibration calibration =
      {1280, 960, 800, 800, 640, 480, 0, 0, 0, 0};
  Lardon3DSparseGeometryPoint2 pixel = {720, 520};
  Lardon3DSparseGeometryPoint2 normalized;
  CHECK(lardon3d_sparse_geometry_normalize(&calibration, &pixel, 1,
                                           &normalized) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(std::abs(normalized.x - 0.1) < 1e-12);
  CHECK(std::abs(normalized.y - 0.05) < 1e-12);
  Lardon3DSparseGeometryCalibration distorted_calibration = calibration;
  distorted_calibration.k1 = 0.08;
  distorted_calibration.k2 = -0.01;
  distorted_calibration.p1 = 0.001;
  distorted_calibration.p2 = -0.002;
  const double ideal_x = 0.2;
  const double ideal_y = -0.15;
  const double radius_squared = ideal_x * ideal_x + ideal_y * ideal_y;
  const double radial = 1.0 + distorted_calibration.k1 * radius_squared +
                        distorted_calibration.k2 * radius_squared * radius_squared;
  const double distorted_x = ideal_x * radial +
                              2.0 * distorted_calibration.p1 * ideal_x * ideal_y +
                              distorted_calibration.p2 *
                                  (radius_squared + 2.0 * ideal_x * ideal_x);
  const double distorted_y = ideal_y * radial +
                              distorted_calibration.p1 *
                                  (radius_squared + 2.0 * ideal_y * ideal_y) +
                              2.0 * distorted_calibration.p2 * ideal_x * ideal_y;
  Lardon3DSparseGeometryPoint2 distorted_pixel = {
      distorted_calibration.fx * distorted_x + distorted_calibration.cx,
      distorted_calibration.fy * distorted_y + distorted_calibration.cy};
  CHECK(lardon3d_sparse_geometry_normalize(
            &distorted_calibration, &distorted_pixel, 1, &normalized) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(std::abs(normalized.x - ideal_x) < 1e-8);
  CHECK(std::abs(normalized.y - ideal_y) < 1e-8);

  Lardon3DSparseGeometryPose pose_a = {{1, 0, 0, 0, 1, 0, 0, 0, 1},
                                       {0, 0, 0}};
  Lardon3DSparseGeometryPose pose_b = pose_a;
  pose_b.translation_cw[0] = 1.0;
  Lardon3DSparseGeometryPoint3 truth = {0.2, -0.1, 4.0};
  Lardon3DSparseGeometryPoint2 pixels_a[8];
  Lardon3DSparseGeometryPoint2 pixels_b[8];
  for (size_t index = 0; index < 8; ++index) {
    Lardon3DSparseGeometryPoint3 point = {
        truth.x + static_cast<double>(index) * 0.17,
        truth.y + static_cast<double>(index % 3) * 0.13,
        truth.z + static_cast<double>(index) * 0.21};
    pixels_a[index] = project(calibration, point, pose_a);
    pixels_b[index] = project(calibration, point, pose_b);
  }
  uint8_t mask[8] = {0};
  Lardon3DSparseGeometryRelativePoseParameters relative_parameters =
      {1.0, 0.999, 1000, 6, 0.75, 1e-4, 0.5, 1234};
  Lardon3DSparseGeometryRelativePoseResult relative = {};
  relative.inlier_mask = mask;
  relative.inlier_mask_capacity = 8;
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, pixels_a, pixels_b, 8,
            &relative_parameters, &relative) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(relative.inlier_count >= 6);
  CHECK(relative.median_parallax_rad > 1e-4);
  Lardon3DSparseGeometryRelativePoseResult repeated = {};
  repeated.inlier_mask = mask;
  repeated.inlier_mask_capacity = 8;
  for (int run = 0; run < 20; ++run) {
    CHECK(lardon3d_sparse_geometry_relative_pose(
              &calibration, &calibration, pixels_a, pixels_b, 8,
              &relative_parameters, &repeated) ==
          LARDON3D_SPARSE_GEOMETRY_OK);
    CHECK(repeated.inlier_count == relative.inlier_count);
    CHECK(std::memcmp(mask, relative.inlier_mask, sizeof(mask)) == 0);
  }
  Lardon3DSparseGeometryPoint2 nonfinite_pixel = {NAN, 0};
  CHECK(lardon3d_sparse_geometry_normalize(
            &calibration, &nonfinite_pixel, 1, &normalized) ==
        LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT);
  Lardon3DSparseGeometryPoint2 pure_pixels_b[8];
  for (size_t index = 0; index < 8; ++index)
    pure_pixels_b[index] = pixels_a[index];
  CHECK(lardon3d_sparse_geometry_relative_pose(
            &calibration, &calibration, pixels_a, pure_pixels_b, 8,
            &relative_parameters, &repeated) !=
        LARDON3D_SPARSE_GEOMETRY_OK);

  Lardon3DSparseGeometryPoint2 normalized_a;
  Lardon3DSparseGeometryPoint2 normalized_b;
  CHECK(lardon3d_sparse_geometry_normalize(&calibration, &pixels_a[0], 1,
                                           &normalized_a) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(lardon3d_sparse_geometry_normalize(&calibration, &pixels_b[0], 1,
                                           &normalized_b) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  Lardon3DSparseGeometryPoint3 triangulated;
  CHECK(lardon3d_sparse_geometry_triangulate_two_view(
            &normalized_a, &normalized_b, &pose_a, &pose_b, &triangulated) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(std::abs(triangulated.x - truth.x) < 1e-8);
  CHECK(std::abs(triangulated.y - truth.y) < 1e-8);
  CHECK(std::abs(triangulated.z - truth.z) < 1e-8);
  Lardon3DSparseGeometryPoint2 multi_points[3] = {
      normalized_a, normalized_b, normalized_b};
  Lardon3DSparseGeometryPose multi_poses[3] = {pose_a, pose_b, pose_b};
  Lardon3DSparseGeometryPoint3 multi_point;
  CHECK(lardon3d_sparse_geometry_triangulate_multi_view(
            multi_points, multi_poses, 3, &multi_point) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(std::abs(multi_point.z - truth.z) < 1e-8);
  Lardon3DSparseGeometryPointRefinementParameters refinement_parameters =
      {30, 1e-12};
  Lardon3DSparseGeometryPoint3 initial = {truth.x + 0.1, truth.y - 0.1,
                                          truth.z + 0.2};
  Lardon3DSparseGeometryPoint3 refined;
  CHECK(lardon3d_sparse_geometry_refine_point(
            multi_points, multi_poses, 3, &initial, &refinement_parameters,
            &refined) == LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(std::abs(refined.x - truth.x) < 1e-7);
  CHECK(std::abs(refined.y - truth.y) < 1e-7);
  CHECK(std::abs(refined.z - truth.z) < 1e-7);

  Lardon3DSparseGeometryPnPParameters pnp_parameters =
      {1.0, 0.999, 1000, 6, 0.75, 1234};
  Lardon3DSparseGeometryPoint3 points[8];
  for (size_t index = 0; index < 8; ++index)
    points[index] = {truth.x + static_cast<double>(index) * 0.17,
                     truth.y + static_cast<double>(index % 3) * 0.13,
                     truth.z + static_cast<double>(index) * 0.21};
  Lardon3DSparseGeometryPnPResult pnp = {};
  pnp.inlier_mask = mask;
  pnp.inlier_mask_capacity = 8;
  CHECK(lardon3d_sparse_geometry_pnp(&calibration, points, pixels_b, 8,
                                     &pnp_parameters, &pnp) ==
        LARDON3D_SPARSE_GEOMETRY_OK);
  CHECK(pnp.inlier_count >= 6);
  return 0;
}
