#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>

#include <lardon3d/sparse_sfm_geometry.h>

namespace {

using Result = Lardon3DSparseGeometryResult;

struct RngGuard {
  uint64_t saved;
  explicit RngGuard(uint64_t seed) : saved(cv::theRNG().state) {
    cv::theRNG().state = seed;
  }
  ~RngGuard() { cv::theRNG().state = saved; }
};

bool finite_value(double value) { return std::isfinite(value); }

bool calibration_valid(const Lardon3DSparseGeometryCalibration &calibration) {
  return calibration.width > 0 && calibration.height > 0 &&
         calibration.fx > 0.0 && calibration.fy > 0.0 &&
         calibration.cx >= 0.0 && calibration.cx < calibration.width &&
         calibration.cy >= 0.0 && calibration.cy < calibration.height &&
         finite_value(calibration.fx) && finite_value(calibration.fy) &&
         finite_value(calibration.cx) && finite_value(calibration.cy) &&
         finite_value(calibration.k1) && finite_value(calibration.k2) &&
         finite_value(calibration.p1) && finite_value(calibration.p2);
}

bool points_finite(const Lardon3DSparseGeometryPoint2 *points, size_t count) {
  if (!points)
    return false;
  for (size_t index = 0; index < count; ++index)
    if (!finite_value(points[index].x) || !finite_value(points[index].y))
      return false;
  return true;
}

cv::Mat camera_matrix(const Lardon3DSparseGeometryCalibration &calibration) {
  cv::Mat matrix = cv::Mat::zeros(3, 3, CV_64F);
  matrix.at<double>(0, 0) = calibration.fx;
  matrix.at<double>(0, 2) = calibration.cx;
  matrix.at<double>(1, 1) = calibration.fy;
  matrix.at<double>(1, 2) = calibration.cy;
  matrix.at<double>(2, 2) = 1.0;
  return matrix;
}

cv::Mat distortion(const Lardon3DSparseGeometryCalibration &calibration) {
  cv::Mat matrix = cv::Mat::zeros(1, 4, CV_64F);
  matrix.at<double>(0, 0) = calibration.k1;
  matrix.at<double>(0, 1) = calibration.k2;
  matrix.at<double>(0, 2) = calibration.p1;
  matrix.at<double>(0, 3) = calibration.p2;
  return matrix;
}

bool rotation_valid(const cv::Mat &rotation) {
  if (rotation.rows != 3 || rotation.cols != 3)
    return false;
  cv::Mat error = rotation.t() * rotation - cv::Mat::eye(3, 3, CV_64F);
  double determinant = cv::determinant(rotation);
  return cv::norm(error, cv::NORM_INF) < 1e-6 &&
         std::abs(determinant - 1.0) < 1e-6;
}

void copy_pose(const cv::Mat &rotation, const cv::Mat &translation,
               Lardon3DSparseGeometryPose *pose) {
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      pose->rotation_cw[row * 3 + column] = rotation.at<double>(row, column);
  for (int index = 0; index < 3; ++index)
    pose->translation_cw[index] = translation.at<double>(index, 0);
}

bool pose_finite(const Lardon3DSparseGeometryPose &pose) {
  for (double value : pose.rotation_cw)
    if (!finite_value(value))
      return false;
  for (double value : pose.translation_cw)
    if (!finite_value(value))
      return false;
  return true;
}

cv::Mat pose_rotation(const Lardon3DSparseGeometryPose &pose) {
  cv::Mat matrix(3, 3, CV_64F);
  for (int row = 0; row < 3; ++row)
    for (int column = 0; column < 3; ++column)
      matrix.at<double>(row, column) = pose.rotation_cw[row * 3 + column];
  return matrix;
}

cv::Mat pose_projection(const Lardon3DSparseGeometryPose &pose) {
  cv::Mat projection = cv::Mat::zeros(3, 4, CV_64F);
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      projection.at<double>(row, column) = pose.rotation_cw[row * 3 + column];
    projection.at<double>(row, 3) = pose.translation_cw[row];
  }
  return projection;
}

bool triangulated_point_valid(const cv::Mat &homogeneous,
                              const cv::Mat &rotation_b,
                              const cv::Mat &translation_b,
                              cv::Mat *point) {
  double scale = homogeneous.at<double>(3, 0);
  if (!finite_value(scale) || std::abs(scale) < 1e-12)
    return false;
  cv::Mat candidate = homogeneous.rowRange(0, 3) / scale;
  double depth_a = candidate.at<double>(2, 0);
  cv::Mat point_b = rotation_b * candidate + translation_b;
  double depth_b = point_b.at<double>(2, 0);
  if (!finite_value(depth_a) || !finite_value(depth_b) || depth_a <= 1e-9 ||
      depth_b <= 1e-9 || !cv::checkRange(candidate))
    return false;
  *point = candidate;
  return true;
}

bool parallax_valid(const Lardon3DSparseGeometryPoint2 &point_a,
                    const Lardon3DSparseGeometryPoint2 &point_b,
                    const cv::Mat &rotation_b) {
  cv::Mat ray_a(3, 1, CV_64F);
  cv::Mat ray_b(3, 1, CV_64F);
  ray_a.at<double>(0, 0) = point_a.x;
  ray_a.at<double>(1, 0) = point_a.y;
  ray_a.at<double>(2, 0) = 1.0;
  ray_b.at<double>(0, 0) = point_b.x;
  ray_b.at<double>(1, 0) = point_b.y;
  ray_b.at<double>(2, 0) = 1.0;
  ray_a /= cv::norm(ray_a);
  ray_b = rotation_b.t() * ray_b;
  ray_b /= cv::norm(ray_b);
  double cosine = ray_a.dot(ray_b);
  return std::acos(std::clamp(cosine, -1.0, 1.0)) >= 1e-4;
}

} // namespace

extern "C" Lardon3DSparseGeometryResult lardon3d_sparse_geometry_normalize(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPoint2 *pixels, size_t count,
    Lardon3DSparseGeometryPoint2 *normalized) {
  if (!calibration || !pixels || !normalized || count == 0)
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (!calibration_valid(*calibration))
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (!points_finite(pixels, count))
    return LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT;
  try {
    std::vector<cv::Point2d> source;
    source.reserve(count);
    for (size_t index = 0; index < count; ++index)
      source.emplace_back(pixels[index].x, pixels[index].y);
    std::vector<cv::Point2d> undistorted;
    cv::undistortPoints(source, undistorted, camera_matrix(*calibration),
                        distortion(*calibration));
    if (undistorted.size() != count)
      return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
    for (size_t index = 0; index < count; ++index) {
      normalized[index].x = undistorted[index].x;
      normalized[index].y = undistorted[index].y;
      if (!finite_value(normalized[index].x) ||
          !finite_value(normalized[index].y))
        return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
    }
    return LARDON3D_SPARSE_GEOMETRY_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  }
}

extern "C" Lardon3DSparseGeometryResult lardon3d_sparse_geometry_relative_pose(
    const Lardon3DSparseGeometryCalibration *calibration_a,
    const Lardon3DSparseGeometryCalibration *calibration_b,
    const Lardon3DSparseGeometryPoint2 *pixels_a,
    const Lardon3DSparseGeometryPoint2 *pixels_b, size_t count,
    const Lardon3DSparseGeometryRelativePoseParameters *parameters,
    Lardon3DSparseGeometryRelativePoseResult *result) {
  if (!calibration_a || !calibration_b || !pixels_a || !pixels_b ||
      !parameters || !result || count > static_cast<size_t>(INT_MAX) ||
      parameters->max_iterations == 0 || parameters->confidence <= 0.0 ||
      parameters->confidence >= 1.0 || parameters->robust_threshold_px <= 0.0 ||
      parameters->minimum_parallax_rad < 0.0 ||
      parameters->minimum_cheirality_ratio <= 0.0 ||
      parameters->minimum_cheirality_ratio > 1.0)
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (count < 5)
    return LARDON3D_SPARSE_GEOMETRY_INSUFFICIENT_CORRESPONDENCES;
  if (!calibration_valid(*calibration_a) || !calibration_valid(*calibration_b))
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (!points_finite(pixels_a, count) || !points_finite(pixels_b, count))
    return LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT;
  result->inlier_count = 0;
  result->inlier_ratio = 0.0;
  result->median_parallax_rad = 0.0;
  if (result->inlier_mask && result->inlier_mask_capacity < count)
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  try {
    std::vector<cv::Point2d> normalized_a(count), normalized_b(count);
    Lardon3DSparseGeometryResult status =
        lardon3d_sparse_geometry_normalize(calibration_a, pixels_a, count,
                                           reinterpret_cast<
                                               Lardon3DSparseGeometryPoint2 *>(
                                               normalized_a.data()));
    if (status != LARDON3D_SPARSE_GEOMETRY_OK)
      return status;
    status = lardon3d_sparse_geometry_normalize(
        calibration_b, pixels_b, count,
        reinterpret_cast<Lardon3DSparseGeometryPoint2 *>(normalized_b.data()));
    if (status != LARDON3D_SPARSE_GEOMETRY_OK)
      return status;
    double mean_ax = 0.0;
    double mean_ay = 0.0;
    double mean_bx = 0.0;
    double mean_by = 0.0;
    for (size_t index = 0; index < count; ++index) {
      mean_ax += normalized_a[index].x;
      mean_ay += normalized_a[index].y;
      mean_bx += normalized_b[index].x;
      mean_by += normalized_b[index].y;
    }
    mean_ax /= static_cast<double>(count);
    mean_ay /= static_cast<double>(count);
    mean_bx /= static_cast<double>(count);
    mean_by /= static_cast<double>(count);
    double covariance_axx = 0.0;
    double covariance_ayy = 0.0;
    double covariance_axy = 0.0;
    double covariance_bxx = 0.0;
    double covariance_byy = 0.0;
    double covariance_bxy = 0.0;
    for (size_t index = 0; index < count; ++index) {
      double ax = normalized_a[index].x - mean_ax;
      double ay = normalized_a[index].y - mean_ay;
      double bx = normalized_b[index].x - mean_bx;
      double by = normalized_b[index].y - mean_by;
      covariance_axx += ax * ax;
      covariance_ayy += ay * ay;
      covariance_axy += ax * ay;
      covariance_bxx += bx * bx;
      covariance_byy += by * by;
      covariance_bxy += bx * by;
    }
    if (covariance_axx * covariance_ayy - covariance_axy * covariance_axy <
            1e-10 ||
        covariance_bxx * covariance_byy - covariance_bxy * covariance_bxy <
            1e-10)
      return LARDON3D_SPARSE_GEOMETRY_DEGENERATE;
    cv::Mat points_a(static_cast<int>(count), 2, CV_64F, normalized_a.data());
    cv::Mat points_b(static_cast<int>(count), 2, CV_64F, normalized_b.data());
    cv::Mat mask;
    RngGuard rng(parameters->deterministic_seed);
    double normalized_threshold = parameters->robust_threshold_px /
                                  std::max(calibration_a->fx,
                                           calibration_a->fy);
    cv::Mat essential = cv::findEssentialMat(
        points_a, points_b, 1.0, cv::Point2d(0, 0), cv::RANSAC,
        parameters->confidence, normalized_threshold,
        static_cast<int>(parameters->max_iterations), mask);
    if (essential.empty())
      return LARDON3D_SPARSE_GEOMETRY_ESTIMATION_FAILED;
    cv::Mat rotation, translation;
    int inliers = cv::recoverPose(essential, points_a, points_b, rotation,
                                  translation, 1.0, cv::Point2d(0, 0), mask);
    double count_value = static_cast<double>(count);
    if (inliers < static_cast<int>(parameters->minimum_inliers) ||
        static_cast<double>(inliers) / count_value <
            parameters->minimum_inlier_ratio ||
        !rotation_valid(rotation) || cv::norm(translation) < 1e-12)
      return LARDON3D_SPARSE_GEOMETRY_ESTIMATION_FAILED;
    std::vector<double> parallaxes;
    cv::Mat projection_a = cv::Mat::zeros(3, 4, CV_64F);
    projection_a.at<double>(0, 0) = projection_a.at<double>(1, 1) =
        projection_a.at<double>(2, 2) = 1.0;
    cv::Mat projection_b = cv::Mat::zeros(3, 4, CV_64F);
    rotation.copyTo(projection_b.colRange(0, 3));
    translation.copyTo(projection_b.col(3));
    cv::Mat points_4d;
    cv::triangulatePoints(projection_a, projection_b, points_a.t(), points_b.t(),
                          points_4d);
    for (int index = 0; index < points_4d.cols; ++index) {
      if (!mask.at<unsigned char>(index))
        continue;
      cv::Mat point;
      if (!triangulated_point_valid(points_4d.col(index), rotation, translation,
                                    &point))
        continue;
      cv::Vec3d ray_a(normalized_a[index].x, normalized_a[index].y, 1.0);
      cv::Vec3d ray_b(normalized_b[index].x, normalized_b[index].y, 1.0);
      ray_a = ray_a / cv::norm(ray_a);
      ray_b = ray_b / cv::norm(ray_b);
      cv::Mat ray_b_input(3, 1, CV_64F);
      ray_b_input.at<double>(0, 0) = ray_b[0];
      ray_b_input.at<double>(1, 0) = ray_b[1];
      ray_b_input.at<double>(2, 0) = ray_b[2];
      cv::Mat ray_b_mat = rotation.t() * ray_b_input;
      ray_b = cv::Vec3d(ray_b_mat.at<double>(0, 0),
                        ray_b_mat.at<double>(1, 0),
                        ray_b_mat.at<double>(2, 0));
      double cosine = std::clamp(ray_a.dot(ray_b), -1.0, 1.0);
      parallaxes.push_back(std::acos(cosine));
    }
    if (parallaxes.empty() ||
        parallaxes.size() < static_cast<size_t>(
                                static_cast<double>(inliers) *
                                parameters->minimum_cheirality_ratio))
      return LARDON3D_SPARSE_GEOMETRY_CHEIRALITY_FAILED;
    std::sort(parallaxes.begin(), parallaxes.end());
    double median = parallaxes[parallaxes.size() / 2];
    result->median_parallax_rad = median;
    if (median < parameters->minimum_parallax_rad)
      return LARDON3D_SPARSE_GEOMETRY_LOW_PARALLAX;
    copy_pose(rotation, translation, &result->pose_ba);
    result->inlier_count = static_cast<uint32_t>(inliers);
    result->inlier_ratio = static_cast<double>(inliers) / count_value;
    if (result->inlier_mask)
      for (size_t index = 0; index < count; ++index)
        result->inlier_mask[index] = mask.at<unsigned char>(static_cast<int>(index));
    return LARDON3D_SPARSE_GEOMETRY_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  }
}

extern "C" Lardon3DSparseGeometryResult
lardon3d_sparse_geometry_triangulate_two_view(
    const Lardon3DSparseGeometryPoint2 *normalized_a,
    const Lardon3DSparseGeometryPoint2 *normalized_b,
    const Lardon3DSparseGeometryPose *pose_a,
    const Lardon3DSparseGeometryPose *pose_b,
    Lardon3DSparseGeometryPoint3 *point) {
  if (!normalized_a || !normalized_b || !pose_a || !pose_b || !point ||
      !pose_finite(*pose_a) || !pose_finite(*pose_b) ||
      !rotation_valid(pose_rotation(*pose_a)) ||
      !rotation_valid(pose_rotation(*pose_b)) ||
      !finite_value(normalized_a->x) || !finite_value(normalized_a->y) ||
      !finite_value(normalized_b->x) || !finite_value(normalized_b->y))
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  try {
    cv::Mat points_a(2, 1, CV_64F);
    cv::Mat points_b(2, 1, CV_64F);
    points_a.at<double>(0, 0) = normalized_a->x;
    points_a.at<double>(1, 0) = normalized_a->y;
    points_b.at<double>(0, 0) = normalized_b->x;
    points_b.at<double>(1, 0) = normalized_b->y;
    cv::Mat homogeneous;
    cv::triangulatePoints(pose_projection(*pose_a), pose_projection(*pose_b),
                          points_a, points_b, homogeneous);
    cv::Mat candidate;
    cv::Mat rotation_b = pose_rotation(*pose_b);
    cv::Mat translation_b(3, 1, CV_64F);
    for (int index = 0; index < 3; ++index)
      translation_b.at<double>(index, 0) = pose_b->translation_cw[index];
    if (!parallax_valid(*normalized_a, *normalized_b, rotation_b))
      return LARDON3D_SPARSE_GEOMETRY_LOW_PARALLAX;
    if (!triangulated_point_valid(homogeneous.col(0), rotation_b,
                                  translation_b, &candidate))
      return LARDON3D_SPARSE_GEOMETRY_DEGENERATE;
    point->x = candidate.at<double>(0, 0);
    point->y = candidate.at<double>(1, 0);
    point->z = candidate.at<double>(2, 0);
    return LARDON3D_SPARSE_GEOMETRY_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  }
}

extern "C" Lardon3DSparseGeometryResult
lardon3d_sparse_geometry_triangulate_multi_view(
    const Lardon3DSparseGeometryPoint2 *normalized_points,
    const Lardon3DSparseGeometryPose *poses, size_t view_count,
    Lardon3DSparseGeometryPoint3 *point) {
  if (!normalized_points || !poses || !point || view_count < 2 ||
      view_count > static_cast<size_t>(INT_MAX / 2))
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  try {
    cv::Mat design = cv::Mat::zeros(static_cast<int>(view_count * 2), 4,
                                    CV_64F);
    for (size_t view = 0; view < view_count; ++view) {
      if (!pose_finite(poses[view]) || !rotation_valid(pose_rotation(poses[view])) ||
          !finite_value(normalized_points[view].x) ||
          !finite_value(normalized_points[view].y))
        return LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT;
      cv::Mat projection = pose_projection(poses[view]);
      int row = static_cast<int>(view * 2);
      design.row(row) = normalized_points[view].x * projection.row(2) -
                        projection.row(0);
      design.row(row + 1) = normalized_points[view].y * projection.row(2) -
                            projection.row(1);
    }
    cv::SVD decomposition(design, cv::SVD::MODIFY_A | cv::SVD::FULL_UV);
    cv::Mat homogeneous = decomposition.vt.row(3).t();
    cv::Mat candidate;
    cv::Mat identity_rotation = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat zero_translation = cv::Mat::zeros(3, 1, CV_64F);
    if (!triangulated_point_valid(homogeneous, identity_rotation,
                                  zero_translation, &candidate))
      return LARDON3D_SPARSE_GEOMETRY_DEGENERATE;
    for (size_t view = 0; view < view_count; ++view) {
      cv::Mat camera_point = pose_rotation(poses[view]) * candidate;
      for (int index = 0; index < 3; ++index)
        camera_point.at<double>(index, 0) += poses[view].translation_cw[index];
      if (!finite_value(camera_point.at<double>(2, 0)) ||
          camera_point.at<double>(2, 0) <= 1e-9)
        return LARDON3D_SPARSE_GEOMETRY_CHEIRALITY_FAILED;
    }
    if (!parallax_valid(normalized_points[0], normalized_points[1],
                        pose_rotation(poses[1])))
      return LARDON3D_SPARSE_GEOMETRY_LOW_PARALLAX;
    point->x = candidate.at<double>(0, 0);
    point->y = candidate.at<double>(1, 0);
    point->z = candidate.at<double>(2, 0);
    return LARDON3D_SPARSE_GEOMETRY_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  }
}

extern "C" Lardon3DSparseGeometryResult lardon3d_sparse_geometry_refine_point(
    const Lardon3DSparseGeometryPoint2 *normalized_points,
    const Lardon3DSparseGeometryPose *poses, size_t view_count,
    const Lardon3DSparseGeometryPoint3 *initial_point,
    const Lardon3DSparseGeometryPointRefinementParameters *parameters,
    Lardon3DSparseGeometryPoint3 *refined_point) {
  if (!normalized_points || !poses || !initial_point || !parameters ||
      !refined_point || view_count < 2 || parameters->max_iterations == 0 ||
      parameters->convergence_tolerance <= 0.0)
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (!finite_value(initial_point->x) || !finite_value(initial_point->y) ||
      !finite_value(initial_point->z))
    return LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT;
  cv::Mat point(3, 1, CV_64F);
  point.at<double>(0, 0) = initial_point->x;
  point.at<double>(1, 0) = initial_point->y;
  point.at<double>(2, 0) = initial_point->z;
  try {
    auto objective = [&](const cv::Mat &candidate, cv::Mat *residual) {
      cv::Mat values = cv::Mat::zeros(static_cast<int>(view_count * 2), 1,
                                      CV_64F);
      double sum = 0.0;
      for (size_t view = 0; view < view_count; ++view) {
        cv::Mat camera_point = pose_rotation(poses[view]) * candidate;
        for (int index = 0; index < 3; ++index)
          camera_point.at<double>(index, 0) += poses[view].translation_cw[index];
        double z = camera_point.at<double>(2, 0);
        if (!finite_value(z) || z <= 1e-9)
          return std::numeric_limits<double>::infinity();
        double x = camera_point.at<double>(0, 0) / z;
        double y = camera_point.at<double>(1, 0) / z;
        values.at<double>(static_cast<int>(view * 2), 0) =
            x - normalized_points[view].x;
        values.at<double>(static_cast<int>(view * 2 + 1), 0) =
            y - normalized_points[view].y;
        sum += values.at<double>(static_cast<int>(view * 2), 0) *
                   values.at<double>(static_cast<int>(view * 2), 0) +
               values.at<double>(static_cast<int>(view * 2 + 1), 0) *
                   values.at<double>(static_cast<int>(view * 2 + 1), 0);
      }
      *residual = values;
      return sum;
    };
    cv::Mat residual;
    double current = objective(point, &residual);
    if (!std::isfinite(current))
      return LARDON3D_SPARSE_GEOMETRY_DEGENERATE;
    for (uint32_t iteration = 0; iteration < parameters->max_iterations;
         ++iteration) {
      cv::Mat jacobian(static_cast<int>(view_count * 2), 3, CV_64F);
      const double step = 1e-7;
      for (int axis = 0; axis < 3; ++axis) {
        cv::Mat perturbed = point.clone();
        perturbed.at<double>(axis, 0) += step;
        cv::Mat shifted;
        if (!std::isfinite(objective(perturbed, &shifted)))
          return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
        jacobian.col(axis) = (shifted - residual) / step;
      }
      cv::Mat normal = jacobian.t() * jacobian;
      cv::Mat rhs = -jacobian.t() * residual;
      cv::Mat delta;
      if (!cv::solve(normal, rhs, delta, cv::DECOMP_SVD))
        return LARDON3D_SPARSE_GEOMETRY_DEGENERATE;
      cv::Mat candidate = point + delta;
      cv::Mat candidate_residual;
      double next = objective(candidate, &candidate_residual);
      if (!std::isfinite(next) || next > current)
        return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
      point = candidate;
      residual = candidate_residual;
      if (cv::norm(delta) <= parameters->convergence_tolerance ||
          std::abs(current - next) <= parameters->convergence_tolerance) {
        refined_point->x = point.at<double>(0, 0);
        refined_point->y = point.at<double>(1, 0);
        refined_point->z = point.at<double>(2, 0);
        return LARDON3D_SPARSE_GEOMETRY_OK;
      }
      current = next;
    }
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  } catch (const cv::Exception &) {
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  }
}

extern "C" Lardon3DSparseGeometryResult lardon3d_sparse_geometry_pnp(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPoint3 *points,
    const Lardon3DSparseGeometryPoint2 *pixels, size_t count,
    const Lardon3DSparseGeometryPnPParameters *parameters,
    Lardon3DSparseGeometryPnPResult *result) {
  if (!calibration || !points || !pixels || !parameters || !result ||
      count < 4 || count > static_cast<size_t>(INT_MAX) ||
      parameters->max_iterations == 0 || parameters->confidence <= 0.0 ||
      parameters->confidence >= 1.0 || parameters->reprojection_threshold_px <= 0.0 ||
      parameters->minimum_inlier_ratio <= 0.0 ||
      parameters->minimum_inlier_ratio > 1.0)
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (!calibration_valid(*calibration))
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  if (result->inlier_mask && result->inlier_mask_capacity < count)
    return LARDON3D_SPARSE_GEOMETRY_INVALID_ARGUMENT;
  for (size_t index = 0; index < count; ++index)
    if (!finite_value(points[index].x) || !finite_value(points[index].y) ||
        !finite_value(points[index].z) || !finite_value(pixels[index].x) ||
        !finite_value(pixels[index].y))
      return LARDON3D_SPARSE_GEOMETRY_NONFINITE_INPUT;
  try {
    std::vector<cv::Point3d> object_points;
    std::vector<cv::Point2d> image_points;
    object_points.reserve(count);
    image_points.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      object_points.emplace_back(points[index].x, points[index].y,
                                 points[index].z);
      image_points.emplace_back(pixels[index].x, pixels[index].y);
    }
    cv::Mat object_matrix(static_cast<int>(count), 3, CV_64F);
    cv::Scalar object_mean = cv::mean(object_points);
    for (size_t index = 0; index < count; ++index) {
      object_matrix.at<double>(static_cast<int>(index), 0) =
          points[index].x - object_mean[0];
      object_matrix.at<double>(static_cast<int>(index), 1) =
          points[index].y - object_mean[1];
      object_matrix.at<double>(static_cast<int>(index), 2) =
          points[index].z - object_mean[2];
    }
    cv::SVD object_svd(object_matrix, cv::SVD::NO_UV);
    if (object_svd.w.at<double>(1, 0) < object_svd.w.at<double>(0, 0) * 1e-8)
      return LARDON3D_SPARSE_GEOMETRY_DEGENERATE;
    cv::Mat rvec, tvec, inliers;
    RngGuard rng(parameters->deterministic_seed);
    bool solved = cv::solvePnPRansac(
        object_points, image_points, camera_matrix(*calibration),
        distortion(*calibration), rvec, tvec, false, parameters->max_iterations,
        static_cast<float>(parameters->reprojection_threshold_px),
        parameters->confidence, inliers, cv::SOLVEPNP_EPNP);
    uint32_t minimum = std::max<uint32_t>(parameters->minimum_inliers, 4);
    double count_value = static_cast<double>(count);
    if (!solved || inliers.rows < static_cast<int>(minimum) ||
        static_cast<double>(inliers.rows) / count_value <
            parameters->minimum_inlier_ratio)
      return LARDON3D_SPARSE_GEOMETRY_ESTIMATION_FAILED;
    cv::Mat rotation;
    cv::Rodrigues(rvec, rotation);
    if (!rotation_valid(rotation) || !cv::checkRange(tvec))
      return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
    uint32_t positive_depth = 0;
    for (int row = 0; row < inliers.rows; ++row) {
      int point_index = inliers.at<int>(row, 0);
      cv::Mat object(3, 1, CV_64F);
      object.at<double>(0, 0) = points[point_index].x;
      object.at<double>(1, 0) = points[point_index].y;
      object.at<double>(2, 0) = points[point_index].z;
      cv::Mat camera = rotation * object + tvec;
      if (camera.at<double>(2, 0) > 1e-9)
        ++positive_depth;
    }
    if (static_cast<double>(positive_depth) /
            static_cast<double>(inliers.rows) <
        parameters->minimum_inlier_ratio)
      return LARDON3D_SPARSE_GEOMETRY_CHEIRALITY_FAILED;
    copy_pose(rotation, tvec, &result->pose_cw);
    result->inlier_count = static_cast<uint32_t>(inliers.rows);
    result->inlier_ratio = static_cast<double>(inliers.rows) / count_value;
    if (result->inlier_mask) {
      std::fill(result->inlier_mask, result->inlier_mask + count, 0);
      for (int row = 0; row < inliers.rows; ++row)
        result->inlier_mask[inliers.at<int>(row, 0)] = 1;
    }
    return LARDON3D_SPARSE_GEOMETRY_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_SPARSE_GEOMETRY_NUMERIC_FAILURE;
  }
}
