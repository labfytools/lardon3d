#include "incremental_reconstruction_internal.h"

#include <lardon3d/incremental_reconstruction.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <new>
#include <set>
#include <tuple>
#include <vector>

namespace {
struct ObservationKey {
  uint64_t feature_set_id;
  uint32_t feature_index;

  bool operator<(const ObservationKey &other) const {
    return std::tie(feature_set_id, feature_index) <
           std::tie(other.feature_set_id, other.feature_index);
  }
};

struct HistoricalObservationKey {
  uint64_t track_id;
  uint64_t image_id;
  uint64_t feature_set_id;
  uint32_t feature_index;

  bool operator<(const HistoricalObservationKey &other) const {
    return std::tie(track_id, image_id, feature_set_id, feature_index) <
           std::tie(other.track_id, other.image_id, other.feature_set_id,
                    other.feature_index);
  }
};

struct Lineage {
  uint64_t base_track_id;
  uint64_t extension_track_id;
  uint64_t component_key;
  uint64_t landmark_id;
  std::set<ObservationKey> historical;
};

bool finite_calibration(const Lardon3DSparseGeometryCalibration &value) {
  return value.width > 0 && value.height > 0 && std::isfinite(value.fx) &&
         std::isfinite(value.fy) && std::isfinite(value.cx) &&
         std::isfinite(value.cy) && std::isfinite(value.k1) &&
         std::isfinite(value.k2) && std::isfinite(value.p1) &&
         std::isfinite(value.p2) && value.fx > 0.0 && value.fy > 0.0;
}

bool finite_pose(const Lardon3DSparseGeometryPose &value) {
  for (double item : value.rotation_cw)
    if (!std::isfinite(item)) return false;
  for (double item : value.translation_cw)
    if (!std::isfinite(item)) return false;
  return true;
}

bool finite_point(const Lardon3DSparseGeometryPoint3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool checkpoint(const Lardon3DIncrementalReconstructionInput &input) {
  return !input.checkpoint || input.checkpoint(input.checkpoint_context);
}

template <typename T>
bool copy_array(const std::vector<T> &source, T **output) {
  *output = nullptr;
  if (source.empty()) return true;
  if (source.size() > SIZE_MAX / sizeof(T)) return false;
  *output = static_cast<T *>(std::malloc(source.size() * sizeof(T)));
  if (!*output) return false;
  std::copy(source.begin(), source.end(), *output);
  return true;
}

void destroy_snapshot(Lardon3DSparseIncrementalResult *snapshot) {
  if (!snapshot) return;
  std::free(snapshot->components);
  std::free(snapshot->cameras);
  std::free(snapshot->landmarks);
  std::free(snapshot->observations);
  std::free(snapshot->unregistered_images);
  *snapshot = {};
}

bool valid_base(const Lardon3DSparseIncrementalResult &base) {
  if ((base.status != LARDON3D_SPARSE_INCREMENTAL_COMPLETE &&
       base.status != LARDON3D_SPARSE_INCREMENTAL_PARTIAL) ||
      base.component_count == 0 || base.camera_count < 2 ||
      base.landmark_count == 0 || base.observation_count == 0 ||
      !base.components || !base.cameras || !base.landmarks || !base.observations)
    return false;
  std::map<uint64_t, uint64_t> component_min_image;
  std::map<uint64_t, uint64_t> component_camera_count;
  std::map<uint64_t, uint64_t> component_landmark_count;
  std::map<uint64_t, uint64_t> camera_components;
  std::map<uint64_t, const Lardon3DSparseIncrementalLandmark *> landmarks_by_track;
  std::set<uint64_t> landmark_ids;
  for (size_t index = 0; index < base.component_count; ++index) {
    const auto &component = base.components[index];
    if (component.component_key == 0 || component.registered_image_count == 0 ||
        component.landmark_count == 0 ||
        (index && base.components[index - 1].component_key >= component.component_key))
      return false;
    component_min_image.emplace(component.component_key, UINT64_MAX);
  }
  for (size_t index = 0; index < base.camera_count; ++index) {
    const auto &camera = base.cameras[index];
    auto minimum = component_min_image.find(camera.component_key);
    if (camera.image_id == 0 || minimum == component_min_image.end() ||
        !finite_pose(camera.pose_cw) ||
        !camera_components.emplace(camera.image_id, camera.component_key).second)
      return false;
    minimum->second = std::min(minimum->second, camera.image_id);
    ++component_camera_count[camera.component_key];
  }
  for (size_t index = 0; index < base.landmark_count; ++index) {
    const auto &landmark = base.landmarks[index];
    if (landmark.landmark_id == 0 || landmark.track_id == 0 ||
        component_min_image.find(landmark.component_key) == component_min_image.end() ||
        !finite_point(landmark.point) || landmark.observation_count < 2 ||
        !landmark_ids.insert(landmark.landmark_id).second ||
        !landmarks_by_track.emplace(landmark.track_id, &landmark).second)
      return false;
    ++component_landmark_count[landmark.component_key];
  }
  for (size_t index = 0; index < base.component_count; ++index) {
    const auto &component = base.components[index];
    if (component_min_image[component.component_key] != component.component_key ||
        component_camera_count[component.component_key] !=
            component.registered_image_count ||
        component_landmark_count[component.component_key] != component.landmark_count)
      return false;
  }
  std::map<uint64_t, uint64_t> observation_counts;
  std::map<uint64_t, std::set<uint32_t>> positions;
  std::set<ObservationKey> keys;
  for (size_t index = 0; index < base.observation_count; ++index) {
    const auto &observation = base.observations[index];
    auto landmark = landmarks_by_track.find(observation.track_id);
    auto camera = camera_components.find(observation.image_id);
    if (landmark == landmarks_by_track.end() ||
        observation.landmark_id != landmark->second->landmark_id ||
        camera == camera_components.end() ||
        camera->second != landmark->second->component_key ||
        observation.feature_set_id == 0 ||
        observation.position_in_track >= landmark->second->observation_count ||
        !positions[observation.track_id]
             .insert(observation.position_in_track)
             .second ||
        !keys.insert({observation.feature_set_id, observation.feature_index}).second)
      return false;
    ++observation_counts[observation.track_id];
  }
  for (size_t index = 0; index < base.landmark_count; ++index)
    if (observation_counts[base.landmarks[index].track_id] !=
            base.landmarks[index].observation_count ||
        positions[base.landmarks[index].track_id].size() !=
            base.landmarks[index].observation_count)
      return false;
  return true;
}

Lardon3DIncrementalReconstructionStatus build_lineage(
    const Lardon3DIncrementalReconstructionInput &input,
    std::vector<Lineage> *lineages,
    std::map<uint64_t, size_t> *extension_lineage,
    std::map<uint64_t,
             std::vector<const Lardon3DSparseIncrementalObservation *>>
        *extension_tracks,
    std::map<ObservationKey, const Lardon3DSparseIncrementalObservation *> *extension_keys) {
  const auto &base = *input.base;
  std::map<uint64_t, const Lardon3DSparseIncrementalLandmark *> base_landmarks;
  for (size_t index = 0; index < base.landmark_count; ++index)
    base_landmarks.emplace(base.landmarks[index].track_id, &base.landmarks[index]);

  std::map<uint64_t, std::vector<const Lardon3DSparseIncrementalObservation *>> base_tracks;
  std::map<HistoricalObservationKey,
           const Lardon3DSparseIncrementalLandmarkObservation *>
      snapshot_historical;
  std::map<HistoricalObservationKey,
           const Lardon3DSparseIncrementalObservation *>
      supplied_historical;
  for (size_t index = 0; index < base.observation_count; ++index) {
    const auto &observation = base.observations[index];
    const HistoricalObservationKey key{observation.track_id, observation.image_id,
                                       observation.feature_set_id,
                                       observation.feature_index};
    if (!snapshot_historical.emplace(key, &observation).second)
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
  }
  std::set<ObservationKey> historical_keys;
  for (size_t index = 0; index < input.base_track_observation_count; ++index) {
    const auto &observation = input.base_track_observations[index];
    const HistoricalObservationKey key{observation.track_id, observation.image_id,
                                       observation.feature_set_id,
                                       observation.feature_index};
    if (!base_landmarks.count(observation.track_id) || observation.image_id == 0 ||
        observation.feature_set_id == 0 || observation.feature_count == 0 ||
        observation.feature_index >= observation.feature_count ||
        !std::isfinite(observation.x) || !std::isfinite(observation.y) ||
        !supplied_historical.emplace(key, &observation).second ||
        !historical_keys.insert({observation.feature_set_id,
                                 observation.feature_index}).second)
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
    base_tracks[observation.track_id].push_back(&observation);
  }
  if (base_tracks.size() != base.landmark_count ||
      supplied_historical.size() != snapshot_historical.size())
    return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
  for (const auto &[key, observation] : snapshot_historical) {
    (void)observation;
    if (!supplied_historical.count(key))
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
  }

  for (size_t index = 0; index < input.extension->observation_count; ++index) {
    const auto &observation = input.extension->observations[index];
    if (observation.track_id == 0 || observation.image_id == 0 ||
        observation.feature_set_id == 0 || observation.feature_count == 0 ||
        observation.feature_index >= observation.feature_count ||
        !std::isfinite(observation.x) || !std::isfinite(observation.y) ||
        !extension_keys->emplace(ObservationKey{observation.feature_set_id,
                                                observation.feature_index},
                                 &observation).second)
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_DUPLICATE;
    (*extension_tracks)[observation.track_id].push_back(&observation);
  }

  lineages->reserve(base.landmark_count);
  for (const auto &[base_track_id, historical] : base_tracks) {
    uint64_t descendant = 0;
    std::set<ObservationKey> base_keys;
    for (const auto *observation : historical) {
      ObservationKey key{observation->feature_set_id, observation->feature_index};
      base_keys.insert(key);
      auto found = extension_keys->find(key);
      if (found == extension_keys->end())
        return LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MISSING;
      if (descendant == 0)
        descendant = found->second->track_id;
      else if (descendant != found->second->track_id)
        return LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_SPLIT;
    }
    if (descendant == 0)
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MISSING;
    const auto *landmark = base_landmarks.at(base_track_id);
    Lineage lineage{base_track_id, descendant, landmark->component_key,
                    landmark->landmark_id, std::move(base_keys)};
    auto inserted = extension_lineage->emplace(descendant, lineages->size());
    if (!inserted.second) {
      const auto &other = (*lineages)[inserted.first->second];
      return other.component_key == lineage.component_key
                 ? LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MERGE
                 : LARDON3D_INCREMENTAL_RECONSTRUCTION_CROSS_COMPONENT_BRIDGE;
    }
    lineages->push_back(std::move(lineage));
  }
  return LARDON3D_INCREMENTAL_RECONSTRUCTION_OK;
}

struct Candidate {
  uint64_t image_id;
  uint64_t component_key;
  size_t support;
  std::vector<Lardon3DSparseGeometryPoint3> points;
  std::vector<Lardon3DSparseGeometryPoint2> pixels;
};

bool register_cameras(
    const Lardon3DIncrementalReconstructionInput &input,
    const std::vector<Lineage> &lineages,
    const std::map<uint64_t, size_t> &extension_lineage,
    std::vector<Lardon3DSparseIncrementalCamera> *cameras,
    std::set<uint64_t> *affected,
    Lardon3DIncrementalReconstructionStatus *failure) {
  std::map<uint64_t, const Lardon3DSparseIncrementalLandmark *> landmarks;
  for (size_t index = 0; index < input.base->landmark_count; ++index) {
    const auto &landmark = input.base->landmarks[index];
    landmarks.emplace(landmark.track_id, &landmark);
  }
  std::set<uint64_t> registered;
  std::map<uint64_t, uint64_t> component_keys;
  for (const auto &camera : *cameras) {
    registered.insert(camera.image_id);
    component_keys.emplace(camera.component_key, camera.component_key);
  }
  std::set<uint64_t> remaining;
  std::map<uint64_t, const Lardon3DSparseIncrementalImage *> images_by_id;
  std::map<uint64_t, std::vector<const Lardon3DSparseIncrementalObservation *>>
      observations_by_image;
  for (size_t index = 0; index < input.extension->observation_count; ++index) {
    const auto &observation = input.extension->observations[index];
    observations_by_image[observation.image_id].push_back(&observation);
  }
  for (size_t index = 0; index < input.extension->image_count; ++index) {
    const auto &image = input.extension->images[index];
    if (image.image_id == 0 || !finite_calibration(image.calibration) ||
        !images_by_id.emplace(image.image_id, &image).second)
      return false;
    if (!registered.count(image.image_id)) remaining.insert(image.image_id);
  }

  for (uint32_t round = 0;
       round < input.parameters->maximum_registration_rounds && !remaining.empty();
       ++round) {
    std::vector<Candidate> candidates;
    for (uint64_t image_id : remaining) {
      std::map<uint64_t, Candidate> by_component;
      const auto image_observations = observations_by_image.find(image_id);
      if (image_observations == observations_by_image.end()) continue;
      for (const auto *observation_pointer : image_observations->second) {
        const auto &observation = *observation_pointer;
        auto lineage_index = extension_lineage.find(observation.track_id);
        if (lineage_index == extension_lineage.end()) continue;
        const auto &lineage = lineages[lineage_index->second];
        const auto *landmark = landmarks.at(lineage.base_track_id);
        auto &candidate = by_component[lineage.component_key];
        candidate.image_id = image_id;
        candidate.component_key = lineage.component_key;
        candidate.points.push_back(landmark->point);
        candidate.pixels.push_back({observation.x, observation.y});
      }
      size_t eligible_components = 0;
      for (auto &[component, candidate] : by_component) {
        candidate.support = candidate.points.size();
        if (candidate.support >= input.parameters->minimum_pnp_correspondences) {
          ++eligible_components;
          candidates.push_back(std::move(candidate));
        }
      }
      if (eligible_components > 1) {
        *failure = LARDON3D_INCREMENTAL_RECONSTRUCTION_CROSS_COMPONENT_BRIDGE;
        return false;
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
      return left.support != right.support ? left.support > right.support
                                           : left.image_id < right.image_id;
    });
    bool grew = false;
    for (const auto &candidate : candidates) {
      if (!remaining.count(candidate.image_id)) continue;
      auto image_entry = images_by_id.find(candidate.image_id);
      const auto *image = image_entry == images_by_id.end()
                              ? nullptr
                              : image_entry->second;
      std::vector<uint8_t> mask(candidate.support);
      Lardon3DSparseGeometryPnPResult pnp{};
      pnp.inlier_mask = mask.data();
      pnp.inlier_mask_capacity = mask.size();
      if (!image || lardon3d_sparse_geometry_pnp(
                        &image->calibration, candidate.points.data(),
                        candidate.pixels.data(), candidate.support,
                        &input.parameters->pnp, &pnp) != LARDON3D_SPARSE_GEOMETRY_OK ||
          !finite_pose(pnp.pose_cw))
        continue;
      if (candidate.image_id < candidate.component_key) {
        *failure = LARDON3D_INCREMENTAL_RECONSTRUCTION_COMPONENT_KEY_VIOLATION;
        return false;
      }
      cameras->push_back({candidate.image_id, candidate.component_key, pnp.pose_cw});
      remaining.erase(candidate.image_id);
      affected->insert(candidate.component_key);
      grew = true;
    }
    if (!grew) break;
  }
  std::sort(cameras->begin(), cameras->end(),
            [](const auto &left, const auto &right) {
              return left.image_id < right.image_id;
            });
  return true;
}

bool project_ok(const Lardon3DSparseGeometryCalibration &calibration,
                const Lardon3DSparseGeometryPose &pose,
                const Lardon3DSparseGeometryPoint3 &point,
                const Lardon3DSparseGeometryPoint2 &observed, double threshold) {
  const double *r = pose.rotation_cw;
  const double *t = pose.translation_cw;
  const double x = r[0] * point.x + r[1] * point.y + r[2] * point.z + t[0];
  const double y = r[3] * point.x + r[4] * point.y + r[5] * point.z + t[1];
  const double z = r[6] * point.x + r[7] * point.y + r[8] * point.z + t[2];
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z <= 1e-9)
    return false;
  const double xn = x / z;
  const double yn = y / z;
  const double r2 = xn * xn + yn * yn;
  const double radial = 1.0 + calibration.k1 * r2 + calibration.k2 * r2 * r2;
  const double px = calibration.fx *
                        (xn * radial + 2.0 * calibration.p1 * xn * yn +
                         calibration.p2 * (r2 + 2.0 * xn * xn)) +
                    calibration.cx;
  const double py = calibration.fy *
                        (yn * radial + calibration.p1 * (r2 + 2.0 * yn * yn) +
                         2.0 * calibration.p2 * xn * yn) +
                    calibration.cy;
  return std::isfinite(px) && std::isfinite(py) &&
         std::hypot(px - observed.x, py - observed.y) <= threshold;
}

bool select_base_anchor(const Lardon3DSparseIncrementalResult &base,
                        uint64_t component_key,
                        Lardon3DSparseBundleAdjustmentAnchor *anchor) {
  std::vector<const Lardon3DSparseIncrementalCamera *> cameras;
  for (size_t index = 0; index < base.camera_count; ++index)
    if (base.cameras[index].component_key == component_key)
      cameras.push_back(&base.cameras[index]);
  if (cameras.size() < 2) return false;
  std::sort(cameras.begin(), cameras.end(), [](const auto *left, const auto *right) {
    return left->image_id < right->image_id;
  });
  const auto *pose = cameras.front();
  auto center = [](const auto &camera, double output[3]) {
    const double *r = camera.pose_cw.rotation_cw;
    const double *t = camera.pose_cw.translation_cw;
    for (size_t column = 0; column < 3; ++column)
      output[column] = -(r[column] * t[0] + r[3 + column] * t[1] +
                         r[6 + column] * t[2]);
  };
  double pose_center[3];
  center(*pose, pose_center);
  const Lardon3DSparseIncrementalCamera *scale = nullptr;
  double best = -1.0;
  double best_delta[3]{};
  for (size_t index = 1; index < cameras.size(); ++index) {
    double candidate_center[3];
    center(*cameras[index], candidate_center);
    double delta[3] = {candidate_center[0] - pose_center[0],
                       candidate_center[1] - pose_center[1],
                       candidate_center[2] - pose_center[2]};
    const double distance = std::hypot(delta[0], delta[1], delta[2]);
    if (!scale || distance > best ||
        (distance == best && cameras[index]->image_id < scale->image_id)) {
      scale = cameras[index];
      best = distance;
      std::copy(delta, delta + 3, best_delta);
    }
  }
  size_t axis = 0;
  if (std::abs(best_delta[1]) > std::abs(best_delta[axis])) axis = 1;
  if (std::abs(best_delta[2]) > std::abs(best_delta[axis])) axis = 2;
  if (!scale || std::abs(best_delta[axis]) <= 1e-9) return false;
  *anchor = {component_key, pose->image_id, scale->image_id,
             static_cast<Lardon3DSparseBundleAdjustmentScaleAxis>(axis + 1)};
  return true;
}

bool full_ba_affected(
    const Lardon3DIncrementalReconstructionInput &input,
    const std::set<uint64_t> &affected,
    std::vector<Lardon3DSparseIncrementalComponent> *components,
    std::vector<Lardon3DSparseIncrementalCamera> *cameras,
    std::vector<Lardon3DSparseIncrementalLandmark> *landmarks,
    std::vector<Lardon3DSparseIncrementalLandmarkObservation> *observations) {
  std::map<uint64_t, size_t> component_indexes;
  std::map<uint64_t, size_t> camera_indexes;
  std::map<uint64_t, size_t> landmark_indexes;
  std::map<uint64_t, std::vector<size_t>> cameras_by_component;
  std::map<uint64_t, std::vector<size_t>> landmarks_by_component;
  std::map<uint64_t, std::vector<size_t>> observations_by_component;
  std::map<uint64_t, uint64_t> track_components;
  for (size_t index = 0; index < components->size(); ++index)
    component_indexes.emplace((*components)[index].component_key, index);
  for (size_t index = 0; index < cameras->size(); ++index) {
    camera_indexes.emplace((*cameras)[index].image_id, index);
    cameras_by_component[(*cameras)[index].component_key].push_back(index);
  }
  for (size_t index = 0; index < landmarks->size(); ++index) {
    landmark_indexes.emplace((*landmarks)[index].track_id, index);
    landmarks_by_component[(*landmarks)[index].component_key].push_back(index);
    track_components.emplace((*landmarks)[index].track_id,
                             (*landmarks)[index].component_key);
  }
  for (size_t index = 0; index < observations->size(); ++index) {
    auto component = track_components.find((*observations)[index].track_id);
    if (component == track_components.end()) return false;
    observations_by_component[component->second].push_back(index);
  }
  for (uint64_t key : affected) {
    std::vector<Lardon3DSparseIncrementalComponent> sub_components;
    std::vector<Lardon3DSparseIncrementalCamera> sub_cameras;
    std::vector<Lardon3DSparseIncrementalLandmark> sub_landmarks;
    std::vector<Lardon3DSparseIncrementalLandmarkObservation> sub_observations;
    auto component = component_indexes.find(key);
    if (component == component_indexes.end()) return false;
    sub_components.push_back((*components)[component->second]);
    for (size_t index : cameras_by_component[key])
      sub_cameras.push_back((*cameras)[index]);
    for (size_t index : landmarks_by_component[key])
      sub_landmarks.push_back((*landmarks)[index]);
    for (size_t index : observations_by_component[key])
      sub_observations.push_back((*observations)[index]);
    Lardon3DSparseIncrementalResult sub{};
    sub.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
    sub.components = sub_components.data();
    sub.component_count = sub_components.size();
    sub.cameras = sub_cameras.data();
    sub.camera_count = sub_cameras.size();
    sub.landmarks = sub_landmarks.data();
    sub.landmark_count = sub_landmarks.size();
    sub.observations = sub_observations.data();
    sub.observation_count = sub_observations.size();
    Lardon3DSparseBundleAdjustmentInput ba_input{
        &sub, input.extension->images, input.extension->image_count,
        input.extension->observations, input.extension->observation_count};
    Lardon3DSparseBundleAdjustmentAnchor anchor{};
    if (!select_base_anchor(*input.base, key, &anchor)) return false;
    Lardon3DSparseBundleAdjustmentResult adjusted{};
    auto status = lardon3d_sparse_bundle_adjustment_run_with_anchors(
        &ba_input, &anchor, 1, &adjusted);
    if (status != LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK ||
        adjusted.status != LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE ||
        adjusted.component_count != 1 || !adjusted.diagnostics[0].accepted) {
      lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
      return false;
    }
    for (size_t index = 0; index < adjusted.camera_count; ++index) {
      auto target = camera_indexes.find(adjusted.cameras[index].image_id);
      if (target == camera_indexes.end()) {
        lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
        return false;
      }
      (*cameras)[target->second] = adjusted.cameras[index];
    }
    for (size_t index = 0; index < adjusted.landmark_count; ++index) {
      auto target = landmark_indexes.find(adjusted.landmarks[index].track_id);
      if (target == landmark_indexes.end()) {
        lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
        return false;
      }
      (*landmarks)[target->second] = adjusted.landmarks[index];
    }
    lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
  }
  return true;
}

Lardon3DIncrementalReconstructionStatus execute(
    const Lardon3DIncrementalReconstructionInput &input,
    Lardon3DIncrementalReconstructionResult *result) {
  if (!input.base || !input.extension || !input.parameters ||
      input.base_reconstruction_id == 0 ||
      (input.base_track_observation_count && !input.base_track_observations) ||
      (input.extension->image_count && !input.extension->images) ||
      (input.extension->observation_count && !input.extension->observations))
    return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_ARGUMENT;
  if (!valid_base(*input.base))
    return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
  if (!checkpoint(input)) return LARDON3D_INCREMENTAL_RECONSTRUCTION_CANCELLED;

  std::vector<Lineage> lineages;
  std::map<uint64_t, size_t> extension_lineage;
  std::map<uint64_t,
           std::vector<const Lardon3DSparseIncrementalObservation *>>
      extension_tracks;
  std::map<ObservationKey, const Lardon3DSparseIncrementalObservation *> extension_keys;
  auto lineage_status = build_lineage(input, &lineages, &extension_lineage,
                                      &extension_tracks, &extension_keys);
  if (lineage_status != LARDON3D_INCREMENTAL_RECONSTRUCTION_OK) return lineage_status;

  std::vector<Lardon3DSparseIncrementalComponent> components(
      input.base->components, input.base->components + input.base->component_count);
  std::vector<Lardon3DSparseIncrementalCamera> cameras(
      input.base->cameras, input.base->cameras + input.base->camera_count);
  std::vector<Lardon3DSparseIncrementalLandmark> landmarks(
      input.base->landmarks, input.base->landmarks + input.base->landmark_count);
  std::set<uint64_t> affected;
  Lardon3DIncrementalReconstructionStatus registration_failure =
      LARDON3D_INCREMENTAL_RECONSTRUCTION_GEOMETRY_FAILED;
  if (!register_cameras(input, lineages, extension_lineage, &cameras, &affected,
                        &registration_failure))
    return registration_failure;
  if (!checkpoint(input)) return LARDON3D_INCREMENTAL_RECONSTRUCTION_CANCELLED;

  std::map<uint64_t, uint64_t> registered_component;
  for (const auto &camera : cameras)
    registered_component.emplace(camera.image_id, camera.component_key);
  std::vector<Lardon3DSparseIncrementalLandmarkObservation> observations;
  observations.reserve(input.extension->observation_count);
  std::map<uint64_t, size_t> landmark_by_track;
  std::map<uint64_t, size_t> lineage_by_base_track;
  for (size_t index = 0; index < lineages.size(); ++index)
    lineage_by_base_track.emplace(lineages[index].base_track_id, index);
  for (size_t index = 0; index < landmarks.size(); ++index) {
    auto lineage = lineage_by_base_track.find(landmarks[index].track_id);
    if (lineage == lineage_by_base_track.end())
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
    const auto &lineage_record = lineages[lineage->second];
    landmarks[index].track_id = lineage_record.extension_track_id;
    landmarks[index].observation_count = 0;
    landmark_by_track.emplace(lineage_record.extension_track_id, index);
  }

  for (const auto &lineage : lineages) {
    uint32_t position = 0;
    bool added = false;
    auto descendant = extension_tracks.find(lineage.extension_track_id);
    if (descendant == extension_tracks.end())
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MISSING;
    for (const auto *source_pointer : descendant->second) {
      const auto &source = *source_pointer;
      const bool historical = lineage.historical.count(
          {source.feature_set_id, source.feature_index}) != 0;
      auto registered = registered_component.find(source.image_id);
      if (registered == registered_component.end() ||
          registered->second != lineage.component_key) {
        if (!historical)
          return LARDON3D_INCREMENTAL_RECONSTRUCTION_DESCENDANT_UNREGISTERED_OBSERVATION;
        return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
      }
      const size_t landmark_index = landmark_by_track.at(lineage.extension_track_id);
      observations.push_back({landmarks[landmark_index].landmark_id,
                              lineage.extension_track_id, source.image_id,
                              source.feature_set_id, source.feature_index, position++});
      ++landmarks[landmark_index].observation_count;
      if (!historical) added = true;
    }
    if (added) affected.insert(lineage.component_key);
  }

  uint64_t next_landmark_id = 1;
  for (const auto &landmark : landmarks) {
    if (landmark.landmark_id >= next_landmark_id) {
      if (landmark.landmark_id >= UINT64_MAX - 1)
        next_landmark_id = UINT64_MAX;
      else
        next_landmark_id = landmark.landmark_id + 1;
    }
  }
  std::map<uint64_t, const Lardon3DSparseIncrementalImage *> images_by_id;
  for (size_t index = 0; index < input.extension->image_count; ++index)
    images_by_id.emplace(input.extension->images[index].image_id,
                         &input.extension->images[index]);
  for (const auto &[track_id, track] : extension_tracks) {
    if (extension_lineage.count(track_id)) continue;
    if (track.size() < 2) continue;
    uint64_t component_key = 0;
    bool admissible = true;
    std::vector<Lardon3DSparseGeometryPoint2> normalized;
    std::vector<Lardon3DSparseGeometryPose> poses;
    normalized.reserve(track.size());
    poses.reserve(track.size());
    for (const auto *source : track) {
      auto registered = registered_component.find(source->image_id);
      auto image_entry = images_by_id.find(source->image_id);
      const auto *image = image_entry == images_by_id.end() ? nullptr
                                                            : image_entry->second;
      auto camera = std::lower_bound(cameras.begin(), cameras.end(), source->image_id,
                                     [](const auto &item, uint64_t id) {
                                       return item.image_id < id;
                                     });
      if (registered == registered_component.end() || !image || camera == cameras.end() ||
          camera->image_id != source->image_id ||
          (component_key != 0 && component_key != registered->second)) {
        admissible = false;
        break;
      }
      component_key = registered->second;
      Lardon3DSparseGeometryPoint2 pixel{source->x, source->y};
      Lardon3DSparseGeometryPoint2 point{};
      if (lardon3d_sparse_geometry_normalize(&image->calibration, &pixel, 1, &point) !=
          LARDON3D_SPARSE_GEOMETRY_OK) {
        admissible = false;
        break;
      }
      normalized.push_back(point);
      poses.push_back(camera->pose_cw);
    }
    if (!admissible) continue;
    Lardon3DSparseGeometryPoint3 point{};
    if (lardon3d_sparse_geometry_triangulate_multi_view(
            normalized.data(), poses.data(), poses.size(), &point) !=
        LARDON3D_SPARSE_GEOMETRY_OK)
      continue;
    Lardon3DSparseGeometryPoint3 refined{};
    if (lardon3d_sparse_geometry_refine_point(
            normalized.data(), poses.data(), poses.size(), &point,
            &input.parameters->refinement, &refined) == LARDON3D_SPARSE_GEOMETRY_OK)
      point = refined;
    bool accepted = finite_point(point);
    for (size_t index = 0; accepted && index < track.size(); ++index) {
      auto image_entry = images_by_id.find(track[index]->image_id);
      const auto *image = image_entry == images_by_id.end() ? nullptr
                                                            : image_entry->second;
      accepted = image && project_ok(image->calibration, poses[index], point,
                                     {track[index]->x, track[index]->y},
                                     input.parameters->reprojection_threshold_px);
    }
    if (!accepted) continue;
    if (next_landmark_id == 0 || next_landmark_id == UINT64_MAX)
      return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE;
    const uint64_t landmark_id = next_landmark_id++;
    landmarks.push_back({landmark_id, track_id, component_key, point, 0.0, 0.0,
                         static_cast<uint64_t>(track.size())});
    uint32_t position = 0;
    for (const auto *source : track)
      observations.push_back({landmark_id, track_id, source->image_id,
                              source->feature_set_id, source->feature_index, position++});
    affected.insert(component_key);
  }

  if (affected.empty()) {
    result->status = LARDON3D_INCREMENTAL_RECONSTRUCTION_NO_CHANGE;
    return result->status;
  }
  std::sort(landmarks.begin(), landmarks.end(), [](const auto &left, const auto &right) {
    return left.track_id < right.track_id;
  });
  std::sort(observations.begin(), observations.end(), [](const auto &left, const auto &right) {
    return std::tie(left.landmark_id, left.position_in_track) <
           std::tie(right.landmark_id, right.position_in_track);
  });
  std::map<uint64_t, uint64_t> component_camera_counts;
  std::map<uint64_t, uint64_t> component_landmark_counts;
  for (const auto &camera : cameras)
    ++component_camera_counts[camera.component_key];
  for (const auto &landmark : landmarks)
    ++component_landmark_counts[landmark.component_key];
  for (auto &component : components) {
    component.registered_image_count = component_camera_counts[component.component_key];
    component.image_count = component.registered_image_count;
    component.landmark_count = component_landmark_counts[component.component_key];
  }
  if (!checkpoint(input)) return LARDON3D_INCREMENTAL_RECONSTRUCTION_CANCELLED;
  if (!full_ba_affected(input, affected, &components, &cameras, &landmarks,
                        &observations))
    return LARDON3D_INCREMENTAL_RECONSTRUCTION_BUNDLE_ADJUSTMENT_FAILED;
  if (!checkpoint(input)) return LARDON3D_INCREMENTAL_RECONSTRUCTION_CANCELLED;

  Lardon3DSparseIncrementalResult snapshot{};
  snapshot.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
  snapshot.track_set_id = input.extension->track_set_id;
  snapshot.calibration_scope_id = input.extension->calibration_scope_id;
  snapshot.component_count = components.size();
  snapshot.camera_count = cameras.size();
  snapshot.landmark_count = landmarks.size();
  snapshot.observation_count = observations.size();
  if (!copy_array(components, &snapshot.components) ||
      !copy_array(cameras, &snapshot.cameras) ||
      !copy_array(landmarks, &snapshot.landmarks) ||
      !copy_array(observations, &snapshot.observations)) {
    destroy_snapshot(&snapshot);
    return LARDON3D_INCREMENTAL_RECONSTRUCTION_OUT_OF_MEMORY;
  }
  result->snapshot = snapshot;
  result->changed = true;
  result->status = LARDON3D_INCREMENTAL_RECONSTRUCTION_OK;
  return result->status;
}
} // namespace

extern "C" Lardon3DIncrementalReconstructionStatus
lardon3d_incremental_reconstruction_run(
    const Lardon3DIncrementalReconstructionInput *input,
    Lardon3DIncrementalReconstructionResult *result) {
  if (!result) return LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_ARGUMENT;
  lardon3d_incremental_reconstruction_result_destroy(result);
  if (!input) {
    result->status = LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_ARGUMENT;
    return result->status;
  }
  try {
    return execute(*input, result);
  } catch (const std::bad_alloc &) {
    result->status = LARDON3D_INCREMENTAL_RECONSTRUCTION_OUT_OF_MEMORY;
    return result->status;
  } catch (...) {
    result->status = LARDON3D_INCREMENTAL_RECONSTRUCTION_GEOMETRY_FAILED;
    return result->status;
  }
}

extern "C" void lardon3d_incremental_reconstruction_result_destroy(
    Lardon3DIncrementalReconstructionResult *result) {
  if (!result) return;
  destroy_snapshot(&result->snapshot);
  *result = {};
}
