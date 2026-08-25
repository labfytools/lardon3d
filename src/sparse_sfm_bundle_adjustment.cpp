#include <lardon3d/sparse_sfm_bundle_adjustment.h>

#include "incremental_reconstruction_internal.h"

#include <ceres/ceres.h>
#include <ceres/rotation.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <utility>
#include <vector>

namespace lardon3d::sparse_bundle_adjustment {

constexpr size_t maximum_images = 4096;
constexpr size_t maximum_landmarks = 250000;
constexpr size_t maximum_observations = 1000000;
constexpr double depth_epsilon = 1e-9;
constexpr double gauge_epsilon = 1e-9;

enum class PreparationStatus {
  prepared,
  invalid_argument,
  out_of_memory,
  internal_error
};
enum class PrivateTermination { converged, no_convergence, failure };

struct ResolvedObservation {
  Lardon3DSparseIncrementalObservation source;
  size_t image_index;
};

struct Preparation {
  std::vector<Lardon3DSparseIncrementalImage> images;
  std::vector<Lardon3DSparseIncrementalComponent> components;
  std::vector<Lardon3DSparseIncrementalCamera> cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> landmarks;
  std::vector<Lardon3DSparseIncrementalLandmarkObservation> observations;
  std::vector<ResolvedObservation> resolved_observations;
  std::vector<Lardon3DSparseBundleAdjustmentComponentDiagnostic> diagnostics;
};

struct ImageIndex {
  uint64_t image_id;
  size_t index;
};

struct ObservationIndex {
  uint64_t feature_set_id;
  uint32_t feature_index;
  size_t index;
};

struct ComponentView {
  std::vector<size_t> cameras;
  std::vector<size_t> landmarks;
  std::vector<size_t> observations;
};

struct SolverCamera {
  size_t source_index;
  size_t image_index;
  double quaternion[4];
  double center[3];
  double initial_center[3];
};

struct SolverLandmark {
  size_t source_index;
  double point[3];
};

struct SolverObservation {
  size_t camera_index;
  size_t landmark_index;
  size_t resolved_index;
};

struct UnderconstraintResult {
  bool valid;
  uint32_t mask;
};
enum class UnderconstraintAction { proceed, reject, internal_error };

struct CandidateDecision {
  bool accepted;
  bool has_metrics;
  Lardon3DSparseBundleAdjustmentRejectionReason rejection_reason;
};

enum class ParameterKind : int { landmark = 0, camera_quaternion = 1, camera_center = 2 };
struct ParameterRecord {
  ParameterKind kind;
  uint64_t identity;
  int group;
  bool constant;
  int subset_axis;
};

struct DisjointSet {
  std::vector<size_t> parent;

  explicit DisjointSet(size_t count) : parent(count) {
    std::iota(parent.begin(), parent.end(), 0);
  }

  size_t find(size_t value) {
    while (parent[value] != value) {
      parent[value] = parent[parent[value]];
      value = parent[value];
    }
    return value;
  }

  void join(size_t a, size_t b) {
    a = find(a);
    b = find(b);
    if (a != b) parent[std::max(a, b)] = std::min(a, b);
  }
};

bool finite_calibration(const Lardon3DSparseGeometryCalibration &value) {
  return value.width > 0 && value.height > 0 && std::isfinite(value.fx) &&
         std::isfinite(value.fy) && value.fx > 0.0 && value.fy > 0.0 &&
         std::isfinite(value.cx) && std::isfinite(value.cy) && value.cx >= 0.0 &&
         value.cy >= 0.0 && value.cx < value.width && value.cy < value.height &&
         std::isfinite(value.k1) && std::isfinite(value.k2) &&
         std::isfinite(value.p1) && std::isfinite(value.p2);
}

bool finite_pose(const Lardon3DSparseGeometryPose &value) {
  for (double item : value.rotation_cw)
    if (!std::isfinite(item)) return false;
  for (double item : value.translation_cw)
    if (!std::isfinite(item)) return false;
  return true;
}

bool valid_rotation(const Lardon3DSparseGeometryPose &value) {
  const double *r = value.rotation_cw;
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 3; ++column) {
      double dot = 0.0;
      for (size_t item = 0; item < 3; ++item)
        dot += r[row * 3 + item] * r[column * 3 + item];
      if (!std::isfinite(dot) ||
          std::abs(dot - (row == column ? 1.0 : 0.0)) >= 1e-6)
        return false;
    }
  }
  const double determinant =
      r[0] * (r[4] * r[8] - r[5] * r[7]) -
      r[1] * (r[3] * r[8] - r[5] * r[6]) +
      r[2] * (r[3] * r[7] - r[4] * r[6]);
  return std::isfinite(determinant) && std::abs(determinant - 1.0) < 1e-6;
}

bool finite_point(const Lardon3DSparseGeometryPoint3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool camera_center(const Lardon3DSparseGeometryPose &pose, double center[3]) {
  if (!finite_pose(pose) || !valid_rotation(pose)) return false;
  for (size_t column = 0; column < 3; ++column) {
    center[column] = -(pose.rotation_cw[column] * pose.translation_cw[0] +
                       pose.rotation_cw[3 + column] * pose.translation_cw[1] +
                       pose.rotation_cw[6 + column] * pose.translation_cw[2]);
    if (!std::isfinite(center[column])) return false;
  }
  return true;
}

bool rotation_to_quaternion(const double rotation[9], double quaternion[4]) {
  const double trace = rotation[0] + rotation[4] + rotation[8];
  if (trace > 0.0) {
    const double scale = 2.0 * std::sqrt(trace + 1.0);
    if (!std::isfinite(scale) || scale == 0.0) return false;
    quaternion[0] = 0.25 * scale;
    quaternion[1] = (rotation[7] - rotation[5]) / scale;
    quaternion[2] = (rotation[2] - rotation[6]) / scale;
    quaternion[3] = (rotation[3] - rotation[1]) / scale;
  } else if (rotation[0] > rotation[4] && rotation[0] > rotation[8]) {
    const double scale = 2.0 * std::sqrt(1.0 + rotation[0] - rotation[4] - rotation[8]);
    if (!std::isfinite(scale) || scale == 0.0) return false;
    quaternion[0] = (rotation[7] - rotation[5]) / scale;
    quaternion[1] = 0.25 * scale;
    quaternion[2] = (rotation[1] + rotation[3]) / scale;
    quaternion[3] = (rotation[2] + rotation[6]) / scale;
  } else if (rotation[4] > rotation[8]) {
    const double scale = 2.0 * std::sqrt(1.0 + rotation[4] - rotation[0] - rotation[8]);
    if (!std::isfinite(scale) || scale == 0.0) return false;
    quaternion[0] = (rotation[2] - rotation[6]) / scale;
    quaternion[1] = (rotation[1] + rotation[3]) / scale;
    quaternion[2] = 0.25 * scale;
    quaternion[3] = (rotation[5] + rotation[7]) / scale;
  } else {
    const double scale = 2.0 * std::sqrt(1.0 + rotation[8] - rotation[0] - rotation[4]);
    if (!std::isfinite(scale) || scale == 0.0) return false;
    quaternion[0] = (rotation[3] - rotation[1]) / scale;
    quaternion[1] = (rotation[2] + rotation[6]) / scale;
    quaternion[2] = (rotation[5] + rotation[7]) / scale;
    quaternion[3] = 0.25 * scale;
  }
  double norm = 0.0;
  for (size_t index = 0; index < 4; ++index)
    norm += quaternion[index] * quaternion[index];
  norm = std::sqrt(norm);
  if (!std::isfinite(norm) || norm == 0.0) return false;
  for (size_t index = 0; index < 4; ++index) quaternion[index] /= norm;
  if (quaternion[0] < 0.0 ||
      (quaternion[0] == 0.0 &&
       (quaternion[1] < 0.0 ||
        (quaternion[1] == 0.0 &&
         (quaternion[2] < 0.0 ||
          (quaternion[2] == 0.0 && quaternion[3] < 0.0))))))
    for (size_t index = 0; index < 4; ++index) quaternion[index] = -quaternion[index];
  return true;
}

bool quaternion_to_pose(const double quaternion_source[4], const double center[3],
                        Lardon3DSparseGeometryPose *pose) {
  if (!pose) return false;
  double quaternion[4] = {quaternion_source[0], quaternion_source[1],
                          quaternion_source[2], quaternion_source[3]};
  double norm = 0.0;
  for (double value : quaternion) norm += value * value;
  norm = std::sqrt(norm);
  if (!std::isfinite(norm) || norm == 0.0) return false;
  for (double &value : quaternion) value /= norm;
  if (quaternion[0] < 0.0) for (double &value : quaternion) value = -value;
  const double w = quaternion[0];
  const double x = quaternion[1];
  const double y = quaternion[2];
  const double z = quaternion[3];
  double *r = pose->rotation_cw;
  r[0] = 1.0 - 2.0 * (y * y + z * z);
  r[1] = 2.0 * (x * y - z * w);
  r[2] = 2.0 * (x * z + y * w);
  r[3] = 2.0 * (x * y + z * w);
  r[4] = 1.0 - 2.0 * (x * x + z * z);
  r[5] = 2.0 * (y * z - x * w);
  r[6] = 2.0 * (x * z - y * w);
  r[7] = 2.0 * (y * z + x * w);
  r[8] = 1.0 - 2.0 * (x * x + y * y);
  for (size_t row = 0; row < 3; ++row) {
    pose->translation_cw[row] =
        -(r[row * 3] * center[0] + r[row * 3 + 1] * center[1] +
          r[row * 3 + 2] * center[2]);
  }
  return finite_pose(*pose) && valid_rotation(*pose);
}

struct ReprojectionResidual {
  Lardon3DSparseGeometryCalibration calibration;
  double observed_x;
  double observed_y;

  template <typename T>
  bool operator()(const T *quaternion, const T *center, const T *point,
                  T *residual) const {
    const T relative[3] = {point[0] - center[0], point[1] - center[1],
                           point[2] - center[2]};
    T camera[3];
    ceres::QuaternionRotatePoint(quaternion, relative, camera);
    if (camera[2] <= T(depth_epsilon)) return false;
    const T xn = camera[0] / camera[2];
    const T yn = camera[1] / camera[2];
    const T r2 = xn * xn + yn * yn;
    const T radial = T(1.0) + T(calibration.k1) * r2 +
                     T(calibration.k2) * r2 * r2;
    const T xd = xn * radial + T(2.0 * calibration.p1) * xn * yn +
                 T(calibration.p2) * (r2 + T(2.0) * xn * xn);
    const T yd = yn * radial + T(calibration.p1) * (r2 + T(2.0) * yn * yn) +
                 T(2.0 * calibration.p2) * xn * yn;
    residual[0] = T(calibration.fx) * xd + T(calibration.cx - observed_x);
    residual[1] = T(calibration.fy) * yd + T(calibration.cy - observed_y);
    return true;
  }
};

bool select_anchors(const std::vector<Lardon3DSparseIncrementalCamera> &cameras,
                    uint64_t component_key,
                    Lardon3DSparseBundleAdjustmentComponentDiagnostic *diagnostic) {
  const Lardon3DSparseIncrementalCamera *pose_anchor = nullptr;
  for (const auto &camera : cameras) {
    if (camera.component_key == component_key &&
        (!pose_anchor || camera.image_id < pose_anchor->image_id))
      pose_anchor = &camera;
  }
  if (!pose_anchor) return false;
  double anchor_center[3];
  if (!camera_center(pose_anchor->pose_cw, anchor_center)) return false;
  const Lardon3DSparseIncrementalCamera *scale_anchor = nullptr;
  double best_distance = -1.0;
  double best_delta[3] = {};
  for (const auto &camera : cameras) {
    if (camera.component_key != component_key || camera.image_id == pose_anchor->image_id)
      continue;
    double center[3];
    if (!camera_center(camera.pose_cw, center)) return false;
    double delta[3] = {center[0] - anchor_center[0], center[1] - anchor_center[1],
                       center[2] - anchor_center[2]};
    const double distance = std::hypot(delta[0], delta[1], delta[2]);
    if (!std::isfinite(distance)) return false;
    if (!scale_anchor || distance > best_distance ||
        (distance == best_distance && camera.image_id < scale_anchor->image_id)) {
      scale_anchor = &camera;
      best_distance = distance;
      std::copy(delta, delta + 3, best_delta);
    }
  }
  if (!scale_anchor) return false;
  size_t axis = 0;
  if (std::abs(best_delta[1]) > std::abs(best_delta[axis])) axis = 1;
  if (std::abs(best_delta[2]) > std::abs(best_delta[axis])) axis = 2;
  diagnostic->pose_anchor_image_id = pose_anchor->image_id;
  diagnostic->scale_anchor_image_id = scale_anchor->image_id;
  diagnostic->scale_axis =
      static_cast<Lardon3DSparseBundleAdjustmentScaleAxis>(axis + 1);
  diagnostic->has_anchors = std::abs(best_delta[axis]) > gauge_epsilon;
  return diagnostic->has_anchors;
}

bool termination_accepted(PrivateTermination value) {
  return value == PrivateTermination::converged;
}

bool project(const Lardon3DSparseGeometryCalibration &calibration,
             const Lardon3DSparseGeometryPose &pose,
             const Lardon3DSparseGeometryPoint3 &point,
             Lardon3DSparseGeometryPoint2 *pixel) {
  if (!pixel || !finite_calibration(calibration) || !finite_pose(pose) ||
      !valid_rotation(pose) || !finite_point(point))
    return false;
  const double *r = pose.rotation_cw;
  const double *t = pose.translation_cw;
  const double x = r[0] * point.x + r[1] * point.y + r[2] * point.z + t[0];
  const double y = r[3] * point.x + r[4] * point.y + r[5] * point.z + t[1];
  const double z = r[6] * point.x + r[7] * point.y + r[8] * point.z + t[2];
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
      z <= depth_epsilon)
    return false;
  const double xn = x / z;
  const double yn = y / z;
  const double r2 = xn * xn + yn * yn;
  const double radial = 1.0 + calibration.k1 * r2 + calibration.k2 * r2 * r2;
  const double xd = xn * radial + 2.0 * calibration.p1 * xn * yn +
                    calibration.p2 * (r2 + 2.0 * xn * xn);
  const double yd = yn * radial + calibration.p1 * (r2 + 2.0 * yn * yn) +
                    2.0 * calibration.p2 * xn * yn;
  pixel->x = calibration.fx * xd + calibration.cx;
  pixel->y = calibration.fy * yd + calibration.cy;
  return std::isfinite(pixel->x) && std::isfinite(pixel->y);
}

bool residual_metrics(const double *residuals, size_t count, double *rmse,
                      double *huber_cost) {
  if (!residuals || count == 0 || !rmse || !huber_cost || count > SIZE_MAX / 2)
    return false;
  double squared_sum = 0.0;
  double rho_sum = 0.0;
  for (size_t index = 0; index < count; ++index) {
    const double dx = residuals[index * 2];
    const double dy = residuals[index * 2 + 1];
    const double squared = dx * dx + dy * dy;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(squared))
      return false;
    const double rho = squared <= 4.0 ? squared : 4.0 * std::sqrt(squared) - 4.0;
    squared_sum += squared;
    rho_sum += rho;
    if (!std::isfinite(rho) || !std::isfinite(squared_sum) || !std::isfinite(rho_sum))
      return false;
  }
  *rmse = std::sqrt(squared_sum / static_cast<double>(count));
  *huber_cost = 0.5 * rho_sum;
  return std::isfinite(*rmse) && std::isfinite(*huber_cost);
}

bool cost_acceptable(double initial_cost, double final_cost) {
  if (!std::isfinite(initial_cost) || !std::isfinite(final_cost)) return false;
  const double tolerance = 1e-12 * std::max(1.0, std::abs(initial_cost));
  return std::isfinite(tolerance) && final_cost <= initial_cost + tolerance;
}

bool build_component_views(const Preparation &preparation,
                           std::vector<ComponentView> *views) {
  if (!views) return false;
  views->clear();
  views->resize(preparation.components.size());
  for (size_t index = 0; index < preparation.components.size(); ++index) {
    (*views)[index].cameras.reserve(
        static_cast<size_t>(preparation.components[index].registered_image_count));
    (*views)[index].landmarks.reserve(
        static_cast<size_t>(preparation.components[index].landmark_count));
    (*views)[index].observations.reserve(
        static_cast<size_t>(preparation.diagnostics[index].observation_count));
  }
  for (size_t index = 0; index < preparation.cameras.size(); ++index) {
    auto component = std::lower_bound(
        preparation.components.begin(), preparation.components.end(),
        preparation.cameras[index].component_key,
        [](const auto &value, uint64_t key) { return value.component_key < key; });
    if (component == preparation.components.end() ||
        component->component_key != preparation.cameras[index].component_key)
      return false;
    (*views)[static_cast<size_t>(component - preparation.components.begin())]
        .cameras.push_back(index);
  }
  for (size_t index = 0; index < preparation.landmarks.size(); ++index) {
    auto component = std::lower_bound(
        preparation.components.begin(), preparation.components.end(),
        preparation.landmarks[index].component_key,
        [](const auto &value, uint64_t key) { return value.component_key < key; });
    if (component == preparation.components.end() ||
        component->component_key != preparation.landmarks[index].component_key)
      return false;
    (*views)[static_cast<size_t>(component - preparation.components.begin())]
        .landmarks.push_back(index);
  }
  for (size_t index = 0; index < preparation.observations.size(); ++index) {
    auto landmark = std::lower_bound(
        preparation.landmarks.begin(), preparation.landmarks.end(),
        preparation.observations[index].track_id,
        [](const auto &value, uint64_t key) { return value.track_id < key; });
    if (landmark == preparation.landmarks.end() ||
        landmark->track_id != preparation.observations[index].track_id)
      return false;
    auto component = std::lower_bound(
        preparation.components.begin(), preparation.components.end(),
        landmark->component_key,
        [](const auto &value, uint64_t key) { return value.component_key < key; });
    if (component == preparation.components.end() ||
        component->component_key != landmark->component_key)
      return false;
    (*views)[static_cast<size_t>(component - preparation.components.begin())]
        .observations.push_back(index);
  }
  return true;
}

uint32_t structural_underconstraint_mask(
    size_t camera_count, size_t landmark_count, size_t observation_count,
    size_t pose_anchor, std::vector<std::pair<size_t, size_t>> edges) {
  constexpr uint32_t uc1 = 1U << 0;
  constexpr uint32_t uc2 = 1U << 1;
  constexpr uint32_t uc3 = 1U << 2;
  constexpr uint32_t uc4 = 1U << 3;
  uint32_t mask = 0;
  const uint64_t cameras = camera_count;
  const uint64_t landmarks = landmark_count;
  const uint64_t observations = observation_count;
  if (cameras > (UINT64_MAX - 7) / 6 || landmarks > UINT64_MAX / 3 ||
      observations > UINT64_MAX / 2) {
    mask |= uc1;
  } else {
    const uint64_t camera_dof = 6 * cameras;
    const uint64_t landmark_dof = 3 * landmarks;
    if (camera_dof > UINT64_MAX - landmark_dof ||
        camera_dof + landmark_dof < 7 ||
        2 * observations < camera_dof + landmark_dof - 7)
      mask |= uc1;
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  std::vector<size_t> camera_support(camera_count, 0);
  std::vector<size_t> landmark_support(landmark_count, 0);
  DisjointSet graph(camera_count + landmark_count);
  for (const auto &[camera, landmark] : edges) {
    if (camera >= camera_count || landmark >= landmark_count)
      return uc1 | uc2 | uc3 | uc4;
    ++camera_support[camera];
    ++landmark_support[landmark];
    graph.join(camera, camera_count + landmark);
  }
  for (size_t support : landmark_support)
    if (support < 2) mask |= uc2;
  if (pose_anchor >= camera_count) return uc1 | uc2 | uc3 | uc4;
  for (size_t index = 0; index < camera_support.size(); ++index)
    if (index != pose_anchor && camera_support[index] < 3) mask |= uc3;
  const size_t root = graph.find(pose_anchor);
  for (size_t index = 0; index < graph.parent.size(); ++index)
    if (graph.find(index) != root) mask |= uc4;
  return mask;
}

UnderconstraintResult underconstraint_result(
    const Preparation &preparation, const ComponentView &view,
    const Lardon3DSparseBundleAdjustmentComponentDiagnostic &diagnostic) {
  std::vector<std::pair<size_t, size_t>> edges;
  edges.reserve(view.observations.size());
  for (size_t observation_index : view.observations) {
    const auto &observation = preparation.observations[observation_index];
    auto camera = std::lower_bound(
        view.cameras.begin(), view.cameras.end(), observation.image_id,
        [&](size_t index, uint64_t key) {
          return preparation.cameras[index].image_id < key;
        });
    auto landmark = std::lower_bound(
        view.landmarks.begin(), view.landmarks.end(), observation.track_id,
        [&](size_t index, uint64_t key) {
          return preparation.landmarks[index].track_id < key;
        });
    if (camera == view.cameras.end() || landmark == view.landmarks.end() ||
        preparation.cameras[*camera].image_id != observation.image_id ||
        preparation.landmarks[*landmark].track_id != observation.track_id)
      return {false, 0};
    edges.emplace_back(static_cast<size_t>(camera - view.cameras.begin()),
                       static_cast<size_t>(landmark - view.landmarks.begin()));
  }
  size_t pose_anchor = view.cameras.size();
  for (size_t index = 0; index < view.cameras.size(); ++index)
    if (preparation.cameras[view.cameras[index]].image_id ==
        diagnostic.pose_anchor_image_id)
      pose_anchor = index;
  if (pose_anchor == view.cameras.size()) return {false, 0};
  return {true, structural_underconstraint_mask(
                    view.cameras.size(), view.landmarks.size(),
                    view.observations.size(), pose_anchor, std::move(edges))};
}

UnderconstraintAction classify_underconstraint(const UnderconstraintResult &result) {
  if (!result.valid) return UnderconstraintAction::internal_error;
  return result.mask == 0 ? UnderconstraintAction::proceed
                          : UnderconstraintAction::reject;
}

bool public_counts_valid(size_t image_count, size_t camera_count,
                         size_t landmark_count, size_t result_observation_count,
                         size_t input_observation_count) {
  if (image_count > maximum_images || camera_count > maximum_images ||
      landmark_count > maximum_landmarks ||
      result_observation_count > maximum_observations ||
      input_observation_count > maximum_observations)
    return false;
  return result_observation_count <= SIZE_MAX / 2 &&
         result_observation_count <= SIZE_MAX / sizeof(double) / 2;
}

template <typename T>
void copy_view(std::vector<T> *destination, const T *source, size_t count) {
  destination->clear();
  if (count != 0) destination->assign(source, source + count);
}

PreparationStatus prepare(
    const Lardon3DSparseBundleAdjustmentInput &input, Preparation *preparation,
    const Lardon3DSparseBundleAdjustmentAnchor *anchors = nullptr,
    size_t anchor_count = 0) {
  if (!preparation || !input.incremental_result)
    return PreparationStatus::invalid_argument;
  const auto &result = *input.incremental_result;
  if (result.status < LARDON3D_SPARSE_INCREMENTAL_COMPLETE ||
      result.status > LARDON3D_SPARSE_INCREMENTAL_FAILED ||
      !public_counts_valid(input.image_count, result.camera_count,
                           result.landmark_count, result.observation_count,
                           input.observation_count) ||
      (input.image_count && !input.images) ||
      (input.observation_count && !input.observations) ||
      (result.component_count && !result.components) ||
      (result.camera_count && !result.cameras) ||
      (result.landmark_count && !result.landmarks) ||
      (result.observation_count && !result.observations))
    return PreparationStatus::invalid_argument;
  try {
    Preparation candidate;
    copy_view(&candidate.images, input.images, input.image_count);
    copy_view(&candidate.components, result.components, result.component_count);
    copy_view(&candidate.cameras, result.cameras, result.camera_count);
    copy_view(&candidate.landmarks, result.landmarks, result.landmark_count);
    copy_view(&candidate.observations, result.observations, result.observation_count);
    std::sort(candidate.images.begin(), candidate.images.end(),
              [](const auto &a, const auto &b) { return a.image_id < b.image_id; });

    std::vector<ImageIndex> images;
    images.reserve(input.image_count);
    for (size_t index = 0; index < input.image_count; ++index) {
      const auto &image = candidate.images[index];
      if (image.image_id == 0 || !finite_calibration(image.calibration) ||
          (index && candidate.images[index - 1].image_id == image.image_id))
        return PreparationStatus::invalid_argument;
      images.push_back({image.image_id, index});
    }

    std::vector<ObservationIndex> observation_index;
    observation_index.reserve(input.observation_count);
    for (size_t index = 0; index < input.observation_count; ++index) {
      const auto &observation = input.observations[index];
      if (observation.track_id == 0 || observation.image_id == 0 ||
          observation.feature_set_id == 0 || observation.feature_count == 0 ||
          observation.feature_index >= observation.feature_count ||
          !std::isfinite(observation.x) || !std::isfinite(observation.y))
        return PreparationStatus::invalid_argument;
      auto image = std::lower_bound(
          images.begin(), images.end(), observation.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      if (image == images.end() || image->image_id != observation.image_id)
        return PreparationStatus::invalid_argument;
      observation_index.push_back(
          {observation.feature_set_id, observation.feature_index, index});
    }
    std::sort(observation_index.begin(), observation_index.end(),
              [](const auto &a, const auto &b) {
                return std::pair{a.feature_set_id, a.feature_index} <
                       std::pair{b.feature_set_id, b.feature_index};
              });
    for (size_t index = 1; index < observation_index.size(); ++index) {
      if (observation_index[index - 1].feature_set_id ==
              observation_index[index].feature_set_id &&
          observation_index[index - 1].feature_index ==
              observation_index[index].feature_index)
        return PreparationStatus::invalid_argument;
    }

    std::vector<uint64_t> component_camera_counts(result.component_count, 0);
    std::vector<uint64_t> component_landmark_counts(result.component_count, 0);
    for (size_t index = 0; index < result.component_count; ++index) {
      const auto &component = candidate.components[index];
      if (component.component_key == 0 ||
          (index && candidate.components[index - 1].component_key >=
                        component.component_key) ||
          component.registered_image_count > component.image_count)
        return PreparationStatus::invalid_argument;
    }
    for (size_t index = 0; index < result.camera_count; ++index) {
      const auto &camera = candidate.cameras[index];
      if (camera.image_id == 0 ||
          (index && candidate.cameras[index - 1].image_id >= camera.image_id) ||
          !finite_pose(camera.pose_cw) || !valid_rotation(camera.pose_cw))
        return PreparationStatus::invalid_argument;
      auto component = std::lower_bound(
          candidate.components.begin(), candidate.components.end(), camera.component_key,
          [](const auto &item, uint64_t key) { return item.component_key < key; });
      auto image = std::lower_bound(
          images.begin(), images.end(), camera.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      if (component == candidate.components.end() ||
          component->component_key != camera.component_key || image == images.end() ||
          image->image_id != camera.image_id)
        return PreparationStatus::invalid_argument;
      ++component_camera_counts[static_cast<size_t>(component - candidate.components.begin())];
    }
    for (size_t index = 0; index < result.landmark_count; ++index) {
      const auto &landmark = candidate.landmarks[index];
      if (landmark.landmark_id == 0 || landmark.track_id == 0 ||
          landmark.observation_count < 2 || !finite_point(landmark.point) ||
          (index && candidate.landmarks[index - 1].track_id >= landmark.track_id))
        return PreparationStatus::invalid_argument;
      auto component = std::lower_bound(
          candidate.components.begin(), candidate.components.end(), landmark.component_key,
          [](const auto &item, uint64_t key) { return item.component_key < key; });
      if (component == candidate.components.end() ||
          component->component_key != landmark.component_key)
        return PreparationStatus::invalid_argument;
      ++component_landmark_counts[static_cast<size_t>(component - candidate.components.begin())];
    }
    for (size_t index = 0; index < result.component_count; ++index) {
      if (candidate.components[index].registered_image_count !=
              component_camera_counts[index] ||
          candidate.components[index].landmark_count != component_landmark_counts[index])
        return PreparationStatus::invalid_argument;
    }

    candidate.diagnostics.resize(result.component_count);
    std::vector<uint64_t> landmark_observation_counts(result.landmark_count, 0);
    std::vector<uint8_t> published_observations(input.observation_count, 0);
    candidate.resolved_observations.reserve(result.observation_count);
    uint64_t previous_landmark = 0;
    uint32_t previous_position = 0;
    for (size_t index = 0; index < result.observation_count; ++index) {
      const auto &published = candidate.observations[index];
      const bool ordered = index == 0 || published.landmark_id > previous_landmark ||
                           (published.landmark_id == previous_landmark &&
                            published.position_in_track > previous_position);
      auto landmark = std::lower_bound(
          candidate.landmarks.begin(), candidate.landmarks.end(), published.track_id,
          [](const auto &item, uint64_t key) { return item.track_id < key; });
      auto observation = std::lower_bound(
          observation_index.begin(), observation_index.end(),
          std::pair{published.feature_set_id, published.feature_index},
          [](const auto &item, const auto &key) {
            return std::pair{item.feature_set_id, item.feature_index} < key;
          });
      auto image = std::lower_bound(
          images.begin(), images.end(), published.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      if (!ordered || landmark == candidate.landmarks.end() ||
          landmark->landmark_id != published.landmark_id ||
          landmark->track_id != published.track_id || observation == observation_index.end() ||
          observation->feature_set_id != published.feature_set_id ||
          observation->feature_index != published.feature_index || image == images.end() ||
          image->image_id != published.image_id)
        return PreparationStatus::invalid_argument;
      const auto &source = input.observations[observation->index];
      if (published_observations[observation->index])
        return PreparationStatus::invalid_argument;
      published_observations[observation->index] = 1;
      auto camera = std::lower_bound(
          candidate.cameras.begin(), candidate.cameras.end(), published.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      if (source.track_id != published.track_id || source.image_id != published.image_id ||
          camera == candidate.cameras.end() || camera->image_id != published.image_id ||
          camera->component_key != landmark->component_key)
        return PreparationStatus::invalid_argument;
      const size_t landmark_index = static_cast<size_t>(landmark - candidate.landmarks.begin());
      auto component = std::lower_bound(
          candidate.components.begin(), candidate.components.end(), landmark->component_key,
          [](const auto &item, uint64_t key) { return item.component_key < key; });
      const size_t component_index =
          static_cast<size_t>(component - candidate.components.begin());
      ++landmark_observation_counts[landmark_index];
      ++candidate.diagnostics[component_index].observation_count;
      candidate.resolved_observations.push_back({source, image->index});
      previous_landmark = published.landmark_id;
      previous_position = published.position_in_track;
    }
    for (size_t index = 0; index < candidate.landmarks.size(); ++index) {
      if (candidate.landmarks[index].observation_count !=
          landmark_observation_counts[index])
        return PreparationStatus::invalid_argument;
    }
    for (size_t index = 0; index < candidate.components.size(); ++index) {
      auto &diagnostic = candidate.diagnostics[index];
      const auto &component = candidate.components[index];
      diagnostic.component_key = component.component_key;
      diagnostic.camera_count = component.registered_image_count;
      diagnostic.landmark_count = component.landmark_count;
      diagnostic.termination = LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_NOT_RUN;
      diagnostic.rejection_reason =
          LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_INELIGIBLE;
      const Lardon3DSparseBundleAdjustmentAnchor *override = nullptr;
      for (size_t anchor_index = 0; anchor_index < anchor_count; ++anchor_index) {
        if (anchors[anchor_index].component_key == component.component_key) {
          if (override) return PreparationStatus::invalid_argument;
          override = &anchors[anchor_index];
        }
      }
      bool anchors_valid = false;
      if (override) {
        const auto pose = std::find_if(candidate.cameras.begin(), candidate.cameras.end(),
                                      [&](const auto &camera) {
                                        return camera.component_key == component.component_key &&
                                               camera.image_id ==
                                                   override->pose_anchor_image_id;
                                      });
        const auto scale = std::find_if(candidate.cameras.begin(), candidate.cameras.end(),
                                       [&](const auto &camera) {
                                         return camera.component_key == component.component_key &&
                                                camera.image_id ==
                                                    override->scale_anchor_image_id;
                                       });
        anchors_valid = pose != candidate.cameras.end() &&
                        scale != candidate.cameras.end() && pose != scale &&
                        override->scale_axis >=
                            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_X &&
                        override->scale_axis <=
                            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_Z;
        if (anchors_valid) {
          diagnostic.pose_anchor_image_id = override->pose_anchor_image_id;
          diagnostic.scale_anchor_image_id = override->scale_anchor_image_id;
          diagnostic.scale_axis = override->scale_axis;
          diagnostic.has_anchors = true;
        }
      } else {
        anchors_valid = select_anchors(candidate.cameras, component.component_key,
                                       &diagnostic);
      }
      diagnostic.eligible = component.registered_image_count >= 2 &&
                            component.landmark_count >= 1 && anchors_valid;
      if (diagnostic.eligible)
        diagnostic.rejection_reason = LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONE;
      else if (!diagnostic.has_anchors && component.registered_image_count >= 2)
        diagnostic.rejection_reason =
            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_GAUGE_DEGENERATE;
    }
    *preparation = std::move(candidate);
    return PreparationStatus::prepared;
  } catch (const std::bad_alloc &) {
    return PreparationStatus::out_of_memory;
  } catch (...) {
    return PreparationStatus::internal_error;
  }
}

bool component_metrics(
    const Preparation &preparation, const ComponentView &view,
    const std::vector<Lardon3DSparseIncrementalCamera> &cameras,
    const std::vector<Lardon3DSparseIncrementalLandmark> &landmarks,
    double *rmse, double *cost) {
  std::vector<double> residuals;
  if (view.observations.size() > SIZE_MAX / 2) return false;
  residuals.resize(view.observations.size() * 2);
  for (size_t local = 0; local < view.observations.size(); ++local) {
    const size_t observation_index = view.observations[local];
    const auto &observation = preparation.observations[observation_index];
    auto camera = std::lower_bound(
        cameras.begin(), cameras.end(), observation.image_id,
        [](const auto &value, uint64_t key) { return value.image_id < key; });
    auto landmark = std::lower_bound(
        landmarks.begin(), landmarks.end(), observation.track_id,
        [](const auto &value, uint64_t key) { return value.track_id < key; });
    if (camera == cameras.end() || camera->image_id != observation.image_id ||
        landmark == landmarks.end() || landmark->track_id != observation.track_id)
      return false;
    const auto &resolved = preparation.resolved_observations[observation_index];
    Lardon3DSparseGeometryPoint2 predicted = {};
    if (!project(preparation.images[resolved.image_index].calibration,
                 camera->pose_cw, landmark->point, &predicted))
      return false;
    residuals[local * 2] = predicted.x - resolved.source.x;
    residuals[local * 2 + 1] = predicted.y - resolved.source.y;
  }
  return residual_metrics(residuals.data(), view.observations.size(), rmse, cost);
}

bool build_public_candidate(
    const Preparation &preparation,
    const Lardon3DSparseBundleAdjustmentComponentDiagnostic &diagnostic,
    const std::vector<SolverCamera> &cameras,
    const std::vector<SolverLandmark> &landmarks,
    std::vector<Lardon3DSparseIncrementalCamera> *candidate_cameras,
    std::vector<Lardon3DSparseIncrementalLandmark> *candidate_landmarks) {
  if (!candidate_cameras || !candidate_landmarks) return false;
  *candidate_cameras = preparation.cameras;
  *candidate_landmarks = preparation.landmarks;
  for (size_t index = 0; index < cameras.size(); ++index) {
    const size_t source_index = cameras[index].source_index;
    if (preparation.cameras[source_index].image_id ==
        diagnostic.pose_anchor_image_id)
      continue;
    if (!quaternion_to_pose(
            cameras[index].quaternion, cameras[index].center,
            &(*candidate_cameras)[source_index].pose_cw))
      return false;
  }
  for (const auto &landmark : landmarks) {
    const Lardon3DSparseGeometryPoint3 point = {
        landmark.point[0], landmark.point[1], landmark.point[2]};
    if (!finite_point(point)) return false;
    (*candidate_landmarks)[landmark.source_index].point = point;
  }
  return true;
}

PrivateTermination translate_termination(ceres::TerminationType value) {
  if (value == ceres::CONVERGENCE) return PrivateTermination::converged;
  if (value == ceres::NO_CONVERGENCE) return PrivateTermination::no_convergence;
  return PrivateTermination::failure;
}

Lardon3DSparseBundleAdjustmentTermination public_termination(
    PrivateTermination value) {
  if (value == PrivateTermination::converged)
    return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_CONVERGED;
  if (value == PrivateTermination::no_convergence)
    return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_NO_CONVERGENCE;
  return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_FAILURE;
}

CandidateDecision validate_candidate(PrivateTermination termination,
                                     bool final_metrics_valid,
                                     bool gauge_valid,
                                     double initial_cost,
                                     double final_cost) {
  if (!termination_accepted(termination)) {
    return {false, false,
            termination == PrivateTermination::no_convergence
                ? LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NO_CONVERGENCE
                : LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_SOLVER_FAILURE};
  }
  if (!final_metrics_valid || !gauge_valid)
    return {false, false,
            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE};
  if (!cost_acceptable(initial_cost, final_cost))
    return {false, true,
            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_COST_REGRESSION};
  return {true, true, LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONE};
}

void apply_component_decision(
    const CandidateDecision &decision, const ComponentView &view,
    const std::vector<Lardon3DSparseIncrementalCamera> &candidate_cameras,
    const std::vector<Lardon3DSparseIncrementalLandmark> &candidate_landmarks,
    std::vector<Lardon3DSparseIncrementalCamera> *published_cameras,
    std::vector<Lardon3DSparseIncrementalLandmark> *published_landmarks) {
  if (!decision.accepted) return;
  for (size_t source_index : view.cameras)
    (*published_cameras)[source_index] = candidate_cameras[source_index];
  for (size_t source_index : view.landmarks)
    (*published_landmarks)[source_index] = candidate_landmarks[source_index];
}

bool configure_parameter_blocks(
    ceres::Problem *problem, ceres::ParameterBlockOrdering *ordering,
    const Preparation &preparation,
    const Lardon3DSparseBundleAdjustmentComponentDiagnostic &diagnostic,
    std::vector<SolverCamera> *cameras,
    std::vector<SolverLandmark> *landmarks,
    std::vector<ParameterRecord> *records) {
  if (!problem || !ordering || !cameras || !landmarks) return false;
  if (records) records->clear();
  for (auto &landmark : *landmarks) {
    problem->AddParameterBlock(landmark.point, 3);
    ordering->AddElementToGroup(landmark.point, 0);
    if (records)
      records->push_back({ParameterKind::landmark,
                          preparation.landmarks[landmark.source_index].track_id,
                          0, false, -1});
  }
  for (auto &camera : *cameras) {
    const uint64_t image_id = preparation.cameras[camera.source_index].image_id;
    problem->AddParameterBlock(camera.quaternion, 4,
                               new ceres::QuaternionManifold);
    problem->AddParameterBlock(camera.center, 3);
    ordering->AddElementToGroup(camera.quaternion, 1);
    ordering->AddElementToGroup(camera.center, 1);
    if (records) {
      records->push_back({ParameterKind::camera_quaternion, image_id, 1,
                          image_id == diagnostic.pose_anchor_image_id, -1});
      records->push_back({ParameterKind::camera_center, image_id, 1,
                          image_id == diagnostic.pose_anchor_image_id,
                          image_id == diagnostic.scale_anchor_image_id
                              ? static_cast<int>(diagnostic.scale_axis) - 1
                              : -1});
    }
  }
  auto pose_anchor = std::find_if(cameras->begin(), cameras->end(), [&](const auto &camera) {
    return preparation.cameras[camera.source_index].image_id ==
           diagnostic.pose_anchor_image_id;
  });
  auto scale_anchor = std::find_if(cameras->begin(), cameras->end(), [&](const auto &camera) {
    return preparation.cameras[camera.source_index].image_id ==
           diagnostic.scale_anchor_image_id;
  });
  if (pose_anchor == cameras->end() || scale_anchor == cameras->end()) return false;
  problem->SetParameterBlockConstant(pose_anchor->quaternion);
  problem->SetParameterBlockConstant(pose_anchor->center);
  const int scale_axis = static_cast<int>(diagnostic.scale_axis) - 1;
  problem->SetManifold(scale_anchor->center,
                       new ceres::SubsetManifold(3, {scale_axis}));
  return true;
}

bool solve_component(
    const Preparation &preparation, const ComponentView &view,
    Lardon3DSparseBundleAdjustmentComponentDiagnostic *diagnostic,
    std::vector<Lardon3DSparseIncrementalCamera> *published_cameras,
    std::vector<Lardon3DSparseIncrementalLandmark> *published_landmarks) {
  if (!diagnostic || !published_cameras || !published_landmarks) return false;
  std::vector<SolverCamera> cameras;
  std::vector<SolverLandmark> landmarks;
  std::vector<SolverObservation> observations;
  cameras.reserve(view.cameras.size());
  landmarks.reserve(view.landmarks.size());
  observations.reserve(view.observations.size());

  for (size_t source_index : view.cameras) {
    const auto &source = preparation.cameras[source_index];
    auto image = std::lower_bound(
        preparation.images.begin(), preparation.images.end(), source.image_id,
        [](const auto &value, uint64_t key) { return value.image_id < key; });
    SolverCamera camera = {};
    camera.source_index = source_index;
    camera.image_index = static_cast<size_t>(image - preparation.images.begin());
    if (image == preparation.images.end() || image->image_id != source.image_id ||
        !rotation_to_quaternion(source.pose_cw.rotation_cw, camera.quaternion) ||
        !camera_center(source.pose_cw, camera.center))
      return false;
    std::copy(camera.center, camera.center + 3, camera.initial_center);
    cameras.push_back(camera);
  }
  for (size_t source_index : view.landmarks) {
    const auto &source = preparation.landmarks[source_index];
    landmarks.push_back(
        {source_index, {source.point.x, source.point.y, source.point.z}});
  }
  for (size_t resolved_index : view.observations) {
    const auto &source = preparation.observations[resolved_index];
    auto camera = std::lower_bound(
        cameras.begin(), cameras.end(), source.image_id,
        [&](const auto &value, uint64_t key) {
          return preparation.cameras[value.source_index].image_id < key;
        });
    auto landmark = std::lower_bound(
        landmarks.begin(), landmarks.end(), source.track_id,
        [&](const auto &value, uint64_t key) {
          return preparation.landmarks[value.source_index].track_id < key;
        });
    if (camera == cameras.end() || landmark == landmarks.end()) return false;
    observations.push_back({static_cast<size_t>(camera - cameras.begin()),
                            static_cast<size_t>(landmark - landmarks.begin()),
                            resolved_index});
  }

  if (!component_metrics(preparation, view, preparation.cameras,
                         preparation.landmarks,
                         &diagnostic->initial_reprojection_rmse_px,
                         &diagnostic->initial_robust_cost)) {
    diagnostic->rejection_reason =
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE;
    return true;
  }
  ceres::Problem problem;
  auto ordering = std::make_shared<ceres::ParameterBlockOrdering>();
  if (!configure_parameter_blocks(&problem, ordering.get(), preparation,
                                  *diagnostic, &cameras, &landmarks, nullptr))
    return false;

  auto *loss = new ceres::HuberLoss(2.0);
  for (const auto &observation : observations) {
    const auto &resolved = preparation.resolved_observations[observation.resolved_index];
    const auto &calibration = preparation.images[cameras[observation.camera_index]
                                                      .image_index].calibration;
    auto *cost = new ceres::AutoDiffCostFunction<ReprojectionResidual, 2, 4, 3, 3>(
        new ReprojectionResidual{calibration, resolved.source.x, resolved.source.y});
    problem.AddResidualBlock(cost, loss,
                             cameras[observation.camera_index].quaternion,
                             cameras[observation.camera_index].center,
                             landmarks[observation.landmark_index].point);
  }

  auto pose_anchor = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) {
    return preparation.cameras[camera.source_index].image_id ==
           diagnostic->pose_anchor_image_id;
  });
  auto scale_anchor = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) {
    return preparation.cameras[camera.source_index].image_id ==
           diagnostic->scale_anchor_image_id;
  });
  if (pose_anchor == cameras.end() || scale_anchor == cameras.end()) return false;
  const int scale_axis = static_cast<int>(diagnostic->scale_axis) - 1;

  ceres::Solver::Options options;
  options.minimizer_type = ceres::TRUST_REGION;
  options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT;
  options.linear_solver_type = ceres::ITERATIVE_SCHUR;
  options.preconditioner_type = ceres::SCHUR_JACOBI;
  options.num_threads = 1;
  options.max_num_iterations = 50;
  options.function_tolerance = 1e-6;
  options.gradient_tolerance = 1e-10;
  options.parameter_tolerance = 1e-8;
  options.linear_solver_ordering = std::move(ordering);
  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);

  const PrivateTermination termination = translate_termination(summary.termination_type);
  diagnostic->termination = public_termination(termination);
  diagnostic->iteration_count = summary.iterations.size() > UINT32_MAX
                                    ? UINT32_MAX
                                    : static_cast<uint32_t>(summary.iterations.size());
  std::vector<Lardon3DSparseIncrementalCamera> candidate_cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> candidate_landmarks;
  bool candidate_valid = false;
  if (termination_accepted(termination) &&
      build_public_candidate(preparation, *diagnostic, cameras, landmarks,
                             &candidate_cameras, &candidate_landmarks)) {
    candidate_valid = component_metrics(
        preparation, view, candidate_cameras, candidate_landmarks,
        &diagnostic->final_reprojection_rmse_px,
        &diagnostic->final_robust_cost);
  }
  const bool gauge_valid =
      scale_anchor->center[scale_axis] == scale_anchor->initial_center[scale_axis];
  const CandidateDecision decision = validate_candidate(
      termination, candidate_valid, gauge_valid, diagnostic->initial_robust_cost,
      diagnostic->final_robust_cost);
  diagnostic->has_costs = decision.has_metrics;
  diagnostic->has_rmse = decision.has_metrics;
  diagnostic->accepted = decision.accepted;
  diagnostic->rejection_reason = decision.rejection_reason;
  apply_component_decision(decision, view, candidate_cameras,
                           candidate_landmarks, published_cameras,
                           published_landmarks);
  return true;
}

template <typename T>
bool allocate_copy(const std::vector<T> &source, T **destination) {
  *destination = nullptr;
  if (source.empty()) return true;
  if (source.size() > SIZE_MAX / sizeof(T)) return false;
  *destination = static_cast<T *>(std::malloc(source.size() * sizeof(T)));
  if (!*destination) return false;
  std::copy(source.begin(), source.end(), *destination);
  return true;
}

void destroy_result(Lardon3DSparseBundleAdjustmentResult *result) {
  if (!result) return;
  std::free(result->components);
  std::free(result->cameras);
  std::free(result->landmarks);
  std::free(result->observations);
  std::free(result->diagnostics);
  *result = {};
}

} // namespace lardon3d::sparse_bundle_adjustment

static Lardon3DSparseBundleAdjustmentExecutionStatus run_bundle_adjustment(
    const Lardon3DSparseBundleAdjustmentInput *input,
    const Lardon3DSparseBundleAdjustmentAnchor *anchors, size_t anchor_count,
    Lardon3DSparseBundleAdjustmentResult *result) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (!result) return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INVALID_ARGUMENT;
  destroy_result(result);
  if (!input || (anchor_count != 0 && !anchors))
    return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INVALID_ARGUMENT;
  try {
    Preparation preparation;
    const PreparationStatus preparation_status =
        prepare(*input, &preparation, anchors, anchor_count);
    if (preparation_status == PreparationStatus::invalid_argument)
      return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INVALID_ARGUMENT;
    if (preparation_status == PreparationStatus::out_of_memory)
      return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OUT_OF_MEMORY;
    if (preparation_status == PreparationStatus::internal_error)
      return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR;

    std::vector<ComponentView> views;
    if (!build_component_views(preparation, &views))
      return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR;
    std::vector<Lardon3DSparseIncrementalCamera> published_cameras =
        preparation.cameras;
    std::vector<Lardon3DSparseIncrementalLandmark> published_landmarks =
        preparation.landmarks;
    size_t accepted = 0;
    size_t rejected_eligible = 0;
    for (size_t index = 0; index < views.size(); ++index) {
      auto &diagnostic = preparation.diagnostics[index];
      if (!diagnostic.eligible) continue;
      const UnderconstraintResult underconstraint =
          underconstraint_result(preparation, views[index], diagnostic);
      const UnderconstraintAction underconstraint_action =
          classify_underconstraint(underconstraint);
      if (underconstraint_action == UnderconstraintAction::internal_error)
        return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR;
      if (underconstraint_action == UnderconstraintAction::reject) {
        diagnostic.eligible = false;
        diagnostic.rejection_reason =
            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_UNDERCONSTRAINED;
        continue;
      }
      if (!solve_component(preparation, views[index], &diagnostic,
                           &published_cameras, &published_landmarks))
        return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR;
      if (diagnostic.accepted)
        ++accepted;
      else
        ++rejected_eligible;
    }

    Lardon3DSparseBundleAdjustmentResult candidate = {};
    candidate.status = accepted == 0
                           ? LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_FAILED
                           : rejected_eligible == 0
                                 ? LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE
                                 : LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_PARTIAL;
    candidate.component_count = preparation.components.size();
    candidate.camera_count = published_cameras.size();
    candidate.landmark_count = published_landmarks.size();
    candidate.observation_count = preparation.observations.size();
    if (!allocate_copy(preparation.components, &candidate.components) ||
        !allocate_copy(published_cameras, &candidate.cameras) ||
        !allocate_copy(published_landmarks, &candidate.landmarks) ||
        !allocate_copy(preparation.observations, &candidate.observations) ||
        !allocate_copy(preparation.diagnostics, &candidate.diagnostics)) {
      destroy_result(&candidate);
      return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OUT_OF_MEMORY;
    }
    *result = candidate;
    return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK;
  } catch (const std::bad_alloc &) {
    destroy_result(result);
    return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OUT_OF_MEMORY;
  } catch (...) {
    destroy_result(result);
    return LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR;
  }
}

extern "C" Lardon3DSparseBundleAdjustmentExecutionStatus
lardon3d_sparse_bundle_adjustment_run(
    const Lardon3DSparseBundleAdjustmentInput *input,
    Lardon3DSparseBundleAdjustmentResult *result) {
  return run_bundle_adjustment(input, nullptr, 0, result);
}

Lardon3DSparseBundleAdjustmentExecutionStatus
lardon3d_sparse_bundle_adjustment_run_with_anchors(
    const Lardon3DSparseBundleAdjustmentInput *input,
    const Lardon3DSparseBundleAdjustmentAnchor *anchors, size_t anchor_count,
    Lardon3DSparseBundleAdjustmentResult *result) {
  return run_bundle_adjustment(input, anchors, anchor_count, result);
}

extern "C" void lardon3d_sparse_bundle_adjustment_result_destroy(
    Lardon3DSparseBundleAdjustmentResult *result) {
  lardon3d::sparse_bundle_adjustment::destroy_result(result);
}

#ifdef LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TESTING
extern "C" int lardon3d_sparse_bundle_adjustment_test_prepare(
    const Lardon3DSparseBundleAdjustmentInput *input,
    Lardon3DSparseBundleAdjustmentComponentDiagnostic *diagnostics,
    size_t diagnostic_capacity, Lardon3DSparseIncrementalObservation *resolved,
    size_t resolved_capacity, size_t *diagnostic_count, size_t *resolved_count) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (!input || !diagnostic_count || !resolved_count) return 1;
  Preparation preparation;
  const auto status = prepare(*input, &preparation);
  if (status != PreparationStatus::prepared)
    return status == PreparationStatus::out_of_memory ? 2 : 1;
  *diagnostic_count = preparation.diagnostics.size();
  *resolved_count = preparation.resolved_observations.size();
  if ((preparation.diagnostics.size() > diagnostic_capacity) ||
      (preparation.resolved_observations.size() > resolved_capacity) ||
      (!preparation.diagnostics.empty() && !diagnostics) ||
      (!preparation.resolved_observations.empty() && !resolved))
    return 1;
  std::copy(preparation.diagnostics.begin(), preparation.diagnostics.end(), diagnostics);
  for (size_t index = 0; index < preparation.resolved_observations.size(); ++index)
    resolved[index] = preparation.resolved_observations[index].source;
  return 0;
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_project(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPose *pose,
    const Lardon3DSparseGeometryPoint3 *point,
    Lardon3DSparseGeometryPoint2 *pixel) {
  return calibration && pose && point &&
         lardon3d::sparse_bundle_adjustment::project(*calibration, *pose, *point, pixel);
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_metrics(
    const double *residuals, size_t count, double *rmse, double *huber_cost) {
  return lardon3d::sparse_bundle_adjustment::residual_metrics(
      residuals, count, rmse, huber_cost);
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_cost_acceptable(
    double initial_cost, double final_cost) {
  return lardon3d::sparse_bundle_adjustment::cost_acceptable(initial_cost, final_cost);
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_termination_accepted(
    int value) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (value < 0 || value > 2) return false;
  return termination_accepted(static_cast<PrivateTermination>(value));
}

extern "C" uint32_t lardon3d_sparse_bundle_adjustment_test_underconstraint(
    const Lardon3DSparseBundleAdjustmentInput *input, size_t component_index) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (!input) return UINT32_MAX;
  Preparation preparation;
  if (prepare(*input, &preparation) != PreparationStatus::prepared ||
      component_index >= preparation.components.size())
    return UINT32_MAX;
  std::vector<ComponentView> views;
  if (!build_component_views(preparation, &views)) return UINT32_MAX;
  const UnderconstraintResult result = underconstraint_result(
      preparation, views[component_index], preparation.diagnostics[component_index]);
  return result.valid ? result.mask : UINT32_MAX;
}

extern "C" uint32_t lardon3d_sparse_bundle_adjustment_test_structural_mask(
    size_t camera_count, size_t landmark_count, size_t observation_count,
    size_t pose_anchor, const size_t *camera_indices,
    const size_t *landmark_indices, size_t edge_count) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (edge_count != 0 && (!camera_indices || !landmark_indices)) return UINT32_MAX;
  std::vector<std::pair<size_t, size_t>> edges;
  edges.reserve(edge_count);
  for (size_t index = 0; index < edge_count; ++index)
    edges.emplace_back(camera_indices[index], landmark_indices[index]);
  return structural_underconstraint_mask(camera_count, landmark_count,
                                         observation_count, pose_anchor,
                                         std::move(edges));
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_candidate_decision(
    int termination, bool final_metrics_valid, bool gauge_valid,
    double initial_cost, double final_cost, bool *has_metrics,
    Lardon3DSparseBundleAdjustmentRejectionReason *reason) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (termination < 0 || termination > 2 || !has_metrics || !reason) return false;
  const CandidateDecision decision = validate_candidate(
      static_cast<PrivateTermination>(termination), final_metrics_valid,
      gauge_valid, initial_cost, final_cost);
  *has_metrics = decision.has_metrics;
  *reason = decision.rejection_reason;
  return decision.accepted;
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_counts_valid(
    size_t image_count, size_t camera_count, size_t landmark_count,
    size_t result_observation_count, size_t input_observation_count) {
  return lardon3d::sparse_bundle_adjustment::public_counts_valid(
      image_count, camera_count, landmark_count, result_observation_count,
      input_observation_count);
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_candidate_publication(
    bool final_metrics_valid, double initial_cost, double final_cost,
    double original_x, double candidate_x, double *published_x,
    bool *has_metrics, Lardon3DSparseBundleAdjustmentRejectionReason *reason) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (!published_x || !has_metrics || !reason) return false;
  const CandidateDecision decision = validate_candidate(
      PrivateTermination::converged, final_metrics_valid, true, initial_cost,
      final_cost);
  ComponentView view;
  view.landmarks.push_back(0);
  std::vector<Lardon3DSparseIncrementalCamera> cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> published(1);
  std::vector<Lardon3DSparseIncrementalLandmark> candidate(1);
  published[0].point.x = original_x;
  candidate[0].point.x = candidate_x;
  apply_component_decision(decision, view, cameras, candidate, &cameras,
                           &published);
  *published_x = published[0].point.x;
  *has_metrics = decision.has_metrics;
  *reason = decision.rejection_reason;
  return decision.accepted;
}

extern "C" int lardon3d_sparse_bundle_adjustment_test_internal_underconstraint() {
  using namespace lardon3d::sparse_bundle_adjustment;
  Preparation preparation;
  preparation.cameras.resize(1);
  preparation.cameras[0].image_id = 10;
  preparation.landmarks.resize(1);
  preparation.landmarks[0].track_id = 20;
  preparation.observations.resize(1);
  preparation.observations[0].image_id = 99;
  preparation.observations[0].track_id = 20;
  ComponentView view;
  view.cameras.push_back(0);
  view.landmarks.push_back(0);
  view.observations.push_back(0);
  Lardon3DSparseBundleAdjustmentComponentDiagnostic diagnostic = {};
  diagnostic.pose_anchor_image_id = 10;
  const UnderconstraintAction action = classify_underconstraint(
      underconstraint_result(preparation, view, diagnostic));
  return action == UnderconstraintAction::internal_error
             ? LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR
             : LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK;
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_parameter_ordering(
    const Lardon3DSparseBundleAdjustmentInput *input, int *kinds,
    uint64_t *identities, int *groups, bool *constants, int *subset_axes,
    size_t capacity, size_t *count) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (!input || !count) return false;
  Preparation preparation;
  if (prepare(*input, &preparation) != PreparationStatus::prepared ||
      preparation.components.size() != 1)
    return false;
  std::vector<ComponentView> views;
  if (!build_component_views(preparation, &views)) return false;
  std::vector<SolverCamera> cameras;
  std::vector<SolverLandmark> landmarks;
  for (size_t source_index : views[0].cameras) {
    SolverCamera camera = {};
    camera.source_index = source_index;
    if (!rotation_to_quaternion(preparation.cameras[source_index].pose_cw.rotation_cw,
                                camera.quaternion) ||
        !camera_center(preparation.cameras[source_index].pose_cw, camera.center))
      return false;
    cameras.push_back(camera);
  }
  for (size_t source_index : views[0].landmarks) {
    const auto &point = preparation.landmarks[source_index].point;
    landmarks.push_back({source_index, {point.x, point.y, point.z}});
  }
  ceres::Problem problem;
  ceres::ParameterBlockOrdering ordering;
  std::vector<ParameterRecord> records;
  if (!configure_parameter_blocks(&problem, &ordering, preparation,
                                  preparation.diagnostics[0], &cameras,
                                  &landmarks, &records))
    return false;
  *count = records.size();
  if (records.size() > capacity ||
      (!records.empty() &&
       (!kinds || !identities || !groups || !constants || !subset_axes)))
    return false;
  for (size_t index = 0; index < records.size(); ++index) {
    kinds[index] = static_cast<int>(records[index].kind);
    identities[index] = records[index].identity;
    groups[index] = records[index].group;
    constants[index] = records[index].constant;
    subset_axes[index] = records[index].subset_axis;
  }
  return true;
}

extern "C" bool lardon3d_sparse_bundle_adjustment_test_invalid_candidate(
    const Lardon3DSparseBundleAdjustmentInput *input, double *published_z,
    bool *has_metrics, Lardon3DSparseBundleAdjustmentRejectionReason *reason) {
  using namespace lardon3d::sparse_bundle_adjustment;
  if (!input || !published_z || !has_metrics || !reason) return false;
  Preparation preparation;
  if (prepare(*input, &preparation) != PreparationStatus::prepared ||
      preparation.components.size() != 1 || preparation.landmarks.empty())
    return false;
  std::vector<ComponentView> views;
  if (!build_component_views(preparation, &views)) return false;
  std::vector<Lardon3DSparseIncrementalCamera> candidate_cameras =
      preparation.cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> candidate_landmarks =
      preparation.landmarks;
  candidate_landmarks[views[0].landmarks[0]].point.z = -1.0;
  double initial_rmse = 0.0;
  double initial_cost = 0.0;
  double final_rmse = 0.0;
  double final_cost = 0.0;
  if (!component_metrics(preparation, views[0], preparation.cameras,
                         preparation.landmarks, &initial_rmse, &initial_cost))
    return false;
  const bool final_valid = component_metrics(
      preparation, views[0], candidate_cameras, candidate_landmarks,
      &final_rmse, &final_cost);
  const CandidateDecision decision = validate_candidate(
      PrivateTermination::converged, final_valid, true, initial_cost, final_cost);
  std::vector<Lardon3DSparseIncrementalCamera> published_cameras =
      preparation.cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> published_landmarks =
      preparation.landmarks;
  apply_component_decision(decision, views[0], candidate_cameras,
                           candidate_landmarks, &published_cameras,
                           &published_landmarks);
  *published_z = published_landmarks[views[0].landmarks[0]].point.z;
  *has_metrics = decision.has_metrics;
  *reason = decision.rejection_reason;
  return !decision.accepted && !final_valid;
}
#endif
