#include <lardon3d/sparse_sfm_bundle_adjustment.h>

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>
#include <vector>

namespace lardon3d::sparse_bundle_adjustment {

constexpr size_t maximum_images = 4096;
constexpr size_t maximum_landmarks = 250000;
constexpr size_t maximum_observations = 1000000;
constexpr double depth_epsilon = 1e-9;
constexpr double gauge_epsilon = 1e-9;

enum class PreparationStatus { prepared, invalid_argument, out_of_memory };
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

template <typename T>
void copy_view(std::vector<T> *destination, const T *source, size_t count) {
  destination->clear();
  if (count != 0) destination->assign(source, source + count);
}

PreparationStatus prepare(const Lardon3DSparseBundleAdjustmentInput &input,
                          Preparation *preparation) {
  if (!preparation || !input.incremental_result)
    return PreparationStatus::invalid_argument;
  const auto &result = *input.incremental_result;
  if (result.status < LARDON3D_SPARSE_INCREMENTAL_COMPLETE ||
      result.status > LARDON3D_SPARSE_INCREMENTAL_FAILED ||
      input.image_count > maximum_images || result.camera_count > maximum_images ||
      result.landmark_count > maximum_landmarks ||
      result.observation_count > maximum_observations ||
      input.observation_count > maximum_observations ||
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
      diagnostic.eligible = component.registered_image_count >= 2 &&
                            component.landmark_count >= 1 &&
                            select_anchors(candidate.cameras, component.component_key,
                                           &diagnostic);
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
    return PreparationStatus::invalid_argument;
  }
}

} // namespace lardon3d::sparse_bundle_adjustment

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
#endif
