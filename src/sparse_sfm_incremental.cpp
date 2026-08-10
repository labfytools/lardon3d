#include <lardon3d/sparse_sfm_incremental.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <new>
#include <set>
#include <utility>
#include <vector>

namespace {

struct Observation {
  uint64_t track_id;
  uint64_t image_id;
  uint64_t feature_set_id;
  uint32_t feature_index;
  Lardon3DSparseGeometryPoint2 pixel;
};

struct Track {
  uint64_t id;
  std::vector<Observation> observations;
};

struct Image {
  uint64_t id;
  Lardon3DSparseGeometryCalibration calibration;
  size_t component;
};

struct Pose {
  uint64_t image_id;
  Lardon3DSparseGeometryPose value;
};

struct Landmark {
  uint64_t track_id;
  uint64_t component_key;
  Lardon3DSparseGeometryPoint3 point;
  std::vector<Observation> observations;
  double rmse;
  double median;
};

struct Component {
  uint64_t key;
  std::vector<size_t> image_indices;
};

struct UnionFind {
  std::vector<size_t> parent;

  explicit UnionFind(size_t count) : parent(count) {
    for (size_t index = 0; index < count; ++index) parent[index] = index;
  }

  size_t find(size_t value) {
    size_t root = value;
    while (parent[root] != root) root = parent[root];
    while (parent[value] != value) {
      size_t next = parent[value];
      parent[value] = root;
      value = next;
    }
    return root;
  }

  void unite(size_t left, size_t right) {
    left = find(left);
    right = find(right);
    if (left == right) return;
    if (left < right)
      parent[right] = left;
    else
      parent[left] = right;
  }
};

bool finite_calibration(const Lardon3DSparseGeometryCalibration &calibration) {
  return calibration.width > 0 && calibration.height > 0 &&
         std::isfinite(calibration.fx) && std::isfinite(calibration.fy) &&
         std::isfinite(calibration.cx) && std::isfinite(calibration.cy) &&
         std::isfinite(calibration.k1) && std::isfinite(calibration.k2) &&
         std::isfinite(calibration.p1) && std::isfinite(calibration.p2) &&
         calibration.fx > 0.0 && calibration.fy > 0.0 &&
         calibration.cx >= 0.0 && calibration.cx < calibration.width &&
         calibration.cy >= 0.0 && calibration.cy < calibration.height;
}

bool finite_pose(const Lardon3DSparseGeometryPose &pose) {
  for (double value : pose.rotation_cw)
    if (!std::isfinite(value)) return false;
  for (double value : pose.translation_cw)
    if (!std::isfinite(value)) return false;
  return true;
}

Lardon3DSparseGeometryPose identity_pose() {
  return {{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0},
          {0.0, 0.0, 0.0}};
}

bool project(const Image &image, const Pose &pose,
             const Lardon3DSparseGeometryPoint3 &point,
             Lardon3DSparseGeometryPoint2 *pixel) {
  const double *r = pose.value.rotation_cw;
  const double *t = pose.value.translation_cw;
  const double x = r[0] * point.x + r[1] * point.y + r[2] * point.z + t[0];
  const double y = r[3] * point.x + r[4] * point.y + r[5] * point.z + t[1];
  const double z = r[6] * point.x + r[7] * point.y + r[8] * point.z + t[2];
  if (!pixel || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
      z <= 1e-9)
    return false;
  pixel->x = image.calibration.fx * x / z + image.calibration.cx;
  pixel->y = image.calibration.fy * y / z + image.calibration.cy;
  return std::isfinite(pixel->x) && std::isfinite(pixel->y);
}

double reprojection_error(const Image &image, const Pose &pose,
                          const Landmark &landmark,
                          const Observation &observation) {
  Lardon3DSparseGeometryPoint2 projected;
  if (!project(image, pose, landmark.point, &projected)) return INFINITY;
  const double dx = projected.x - observation.pixel.x;
  const double dy = projected.y - observation.pixel.y;
  return std::sqrt(dx * dx + dy * dy);
}

bool validate_parameters(const Lardon3DSparseIncrementalParameters &p) {
  return p.minimum_seed_tracks >= 2 && p.minimum_seed_landmarks >= 1 &&
         p.minimum_pnp_correspondences >= 4 && p.maximum_seed_candidates > 0 &&
         p.maximum_registration_rounds > 0 && p.maximum_landmarks_per_round > 0 &&
         p.maximum_images >= 2 && p.maximum_observations >= p.maximum_tracks &&
         std::isfinite(p.reprojection_threshold_px) &&
         p.reprojection_threshold_px > 0.0 &&
         std::isfinite(p.minimum_track_parallax_rad) &&
         p.minimum_track_parallax_rad >= 0.0 &&
         p.relative_pose.minimum_inliers >= p.minimum_seed_tracks &&
         p.pnp.minimum_inliers >= p.minimum_pnp_correspondences &&
         p.relative_pose.minimum_inlier_ratio > 0.0 &&
         p.relative_pose.minimum_inlier_ratio <= 1.0 &&
         p.pnp.minimum_inlier_ratio > 0.0 && p.pnp.minimum_inlier_ratio <= 1.0;
}

void destroy_result(Lardon3DSparseIncrementalResult *result) {
  if (!result) return;
  std::free(result->components);
  std::free(result->cameras);
  std::free(result->landmarks);
  std::free(result->observations);
  std::free(result->unregistered_images);
  *result = {};
}

bool append_component(const Component &component,
                      const std::vector<Image> &images,
                      std::vector<Lardon3DSparseIncrementalComponent> *out) {
  Lardon3DSparseIncrementalComponent value = {};
  value.component_key = component.key;
  value.image_count = component.image_indices.size();
  for (size_t index : component.image_indices)
    if (images[index].id == component.key) value.component_key = images[index].id;
  out->push_back(value);
  return true;
}

enum class LandmarkCandidateStatus {
  accepted,
  insufficient,
  triangulation_failed,
  behind_camera,
  reprojection_failed,
};

LandmarkCandidateStatus build_landmark_candidate(
    const Track &track, uint64_t component_key,
    const std::vector<Image> &images,
    const std::map<uint64_t, size_t> &image_index,
    const std::vector<Pose> &cameras, const std::set<uint64_t> &registered,
    const Lardon3DSparseIncrementalParameters &parameters,
    Landmark *candidate, Lardon3DSparseGeometryResult *geometry_status,
    Lardon3DSparseIncrementalResult *result) {
  std::vector<Lardon3DSparseGeometryPoint2> normalized;
  std::vector<Lardon3DSparseGeometryPose> poses;
  std::vector<Observation> used;
  for (const Observation &observation : track.observations) {
    if (!registered.count(observation.image_id)) continue;
    const auto pose_it = std::find_if(
        cameras.begin(), cameras.end(), [&](const Pose &pose) {
          return pose.image_id == observation.image_id;
        });
    if (pose_it == cameras.end()) continue;
    const Image &image = images[image_index.at(observation.image_id)];
    Lardon3DSparseGeometryPoint2 point;
    if (lardon3d_sparse_geometry_normalize(
            &image.calibration, &observation.pixel, 1, &point) !=
        LARDON3D_SPARSE_GEOMETRY_OK)
      return LandmarkCandidateStatus::triangulation_failed;
    normalized.push_back(point);
    poses.push_back(pose_it->value);
    used.push_back(observation);
  }
  if (used.size() < 2) return LandmarkCandidateStatus::insufficient;

  Lardon3DSparseGeometryPoint3 point;
  *geometry_status = lardon3d_sparse_geometry_triangulate_multi_view(
      normalized.data(), poses.data(), normalized.size(), &point);
  if (*geometry_status != LARDON3D_SPARSE_GEOMETRY_OK) {
    return *geometry_status == LARDON3D_SPARSE_GEOMETRY_CHEIRALITY_FAILED
               ? LandmarkCandidateStatus::behind_camera
               : LandmarkCandidateStatus::triangulation_failed;
  }
  Lardon3DSparseGeometryPoint3 refined;
  ++result->point_refinement_attempts;
  const Lardon3DSparseGeometryResult refinement_status =
      lardon3d_sparse_geometry_refine_point(
          normalized.data(), poses.data(), normalized.size(), &point,
          &parameters.refinement, &refined);
  if (refinement_status == LARDON3D_SPARSE_GEOMETRY_OK) {
    point = refined;
    ++result->point_refinement_successes;
  }

  Landmark replacement = {
      track.id, component_key, point, std::move(used), 0.0, 0.0};
  double squared = 0.0;
  double maximum = 0.0;
  for (const Observation &observation : replacement.observations) {
    const Image &image = images[image_index.at(observation.image_id)];
    const Pose &pose = *std::find_if(
        cameras.begin(), cameras.end(), [&](const Pose &item) {
          return item.image_id == observation.image_id;
        });
    const double error = reprojection_error(
        image, pose, replacement, observation);
    if (!std::isfinite(error)) return LandmarkCandidateStatus::behind_camera;
    if (error > parameters.reprojection_threshold_px)
      return LandmarkCandidateStatus::reprojection_failed;
    squared += error * error;
    maximum = std::max(maximum, error);
  }
  replacement.rmse = std::sqrt(
      squared / static_cast<double>(replacement.observations.size()));
  replacement.median = maximum;
  *candidate = std::move(replacement);
  return LandmarkCandidateStatus::accepted;
}

void record_landmark_rejection(LandmarkCandidateStatus status,
                               Lardon3DSparseIncrementalResult *result) {
  if (status == LandmarkCandidateStatus::behind_camera) {
    ++result->rejected_landmarks;
    ++result->rejected_behind_camera;
  } else if (status == LandmarkCandidateStatus::reprojection_failed) {
    ++result->rejected_landmarks;
    ++result->rejected_reprojection;
  } else if (status == LandmarkCandidateStatus::triangulation_failed) {
    ++result->triangulation_failures;
  }
}

}  // namespace

extern "C" bool lardon3d_sparse_incremental_parameters_default(
    Lardon3DSparseIncrementalParameters *parameters) {
  if (!parameters) return false;
  *parameters = {};
  parameters->minimum_seed_tracks = 6;
  parameters->minimum_seed_landmarks = 6;
  parameters->minimum_pnp_correspondences = 6;
  parameters->maximum_seed_candidates = 32;
  parameters->maximum_registration_rounds = 32;
  parameters->maximum_landmarks_per_round = 4096;
  parameters->maximum_images = 4096;
  parameters->maximum_observations = 1000000;
  parameters->maximum_tracks = 250000;
  parameters->reprojection_threshold_px = 2.0;
  parameters->minimum_track_parallax_rad = 1e-4;
  parameters->relative_pose = {1.5, 0.999, 1500, 6, 0.5, 1e-4, 0.5, 0};
  parameters->pnp = {1.5, 0.999, 1000, 6, 0.5, 0};
  parameters->refinement = {30, 1e-12};
  return true;
}

extern "C" Lardon3DSparseIncrementalStatus lardon3d_sparse_incremental_run(
    const Lardon3DSparseIncrementalInput *input,
    const Lardon3DSparseIncrementalParameters *parameters,
    Lardon3DSparseIncrementalResult *result) {
  if (!result) return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;
  destroy_result(result);
  try {
  if (!input || !parameters || !validate_parameters(*parameters) ||
      !input->images || input->image_count == 0 || !input->observations ||
      input->observation_count == 0 || input->image_count > parameters->maximum_images ||
      input->observation_count > parameters->maximum_observations ||
      input->track_set_id == 0 ||
      input->calibration_scope_id == 0)
    return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;

  std::vector<Image> images;
  images.reserve(input->image_count);
  for (size_t index = 0; index < input->image_count; ++index) {
    const auto &source = input->images[index];
    if (source.image_id == 0 || !finite_calibration(source.calibration))
      return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;
    images.push_back({source.image_id, source.calibration, 0});
  }
  std::sort(images.begin(), images.end(),
            [](const Image &a, const Image &b) { return a.id < b.id; });
  for (size_t index = 1; index < images.size(); ++index)
    if (images[index - 1].id == images[index].id)
      return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;

  std::map<uint64_t, size_t> image_index;
  for (size_t index = 0; index < images.size(); ++index)
    image_index[images[index].id] = index;

  std::map<uint64_t, Track> tracks_by_id;
  for (size_t index = 0; index < input->observation_count; ++index) {
    const auto &source = input->observations[index];
    if (source.track_id == 0 || source.image_id == 0 ||
        source.feature_set_id == 0 || source.feature_index >= source.feature_count ||
        image_index.find(source.image_id) == image_index.end() ||
        !std::isfinite(source.x) || !std::isfinite(source.y))
      return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;
    Track &track = tracks_by_id[source.track_id];
    track.id = source.track_id;
    for (const Observation &old : track.observations)
      if (old.image_id == source.image_id)
        return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;
    track.observations.push_back(
        {source.track_id, source.image_id, source.feature_set_id,
         source.feature_index, {source.x, source.y}});
  }
  if (tracks_by_id.empty() || tracks_by_id.size() > parameters->maximum_tracks)
    return LARDON3D_SPARSE_INCREMENTAL_FAILED;
  std::set<std::pair<uint64_t, uint32_t>> feature_observations;
  for (const auto &entry : tracks_by_id) {
    if (entry.second.observations.size() < 2)
      return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;
    for (const Observation &observation : entry.second.observations)
      if (!feature_observations.insert(
              {observation.feature_set_id, observation.feature_index})
               .second)
        return LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT;
  }
  for (auto &entry : tracks_by_id) {
    std::sort(entry.second.observations.begin(),
              entry.second.observations.end(),
              [](const Observation &left, const Observation &right) {
                return left.image_id < right.image_id;
              });
  }

  UnionFind union_find(images.size());
  for (const auto &entry : tracks_by_id) {
    const auto &observations = entry.second.observations;
    for (size_t index = 1; index < observations.size(); ++index)
      union_find.unite(image_index[observations[0].image_id],
                       image_index[observations[index].image_id]);
  }
  std::map<size_t, Component> components_by_root;
  for (size_t index = 0; index < images.size(); ++index)
    components_by_root[union_find.find(index)].image_indices.push_back(index);
  std::vector<Component> components;
  for (auto &entry : components_by_root) {
    auto &items = entry.second.image_indices;
    std::sort(items.begin(), items.end(), [&](size_t a, size_t b) {
      return images[a].id < images[b].id;
    });
    entry.second.key = images[items.front()].id;
    components.push_back(entry.second);
    for (size_t item : items) images[item].component = components.size() - 1;
  }
  std::sort(components.begin(), components.end(),
            [](const Component &a, const Component &b) { return a.key < b.key; });
  std::map<uint64_t, size_t> component_by_image;
  for (size_t component = 0; component < components.size(); ++component)
    for (size_t image : components[component].image_indices) {
      images[image].component = component;
      component_by_image[images[image].id] = component;
    }

  std::vector<Lardon3DSparseIncrementalComponent> output_components;
  for (const Component &component : components)
    append_component(component, images, &output_components);
  std::vector<Pose> cameras;
  std::vector<Landmark> landmarks;
  std::set<uint64_t> registered;
  std::set<uint64_t> accepted_tracks;
  std::map<uint64_t, uint64_t> component_keys;
  for (const Component &component : components) component_keys[component.key] = component.key;

  for (const Component &component : components) {
    if (component.image_indices.size() < 2) continue;
    struct Candidate { uint64_t a; uint64_t b; uint32_t shared; };
    std::vector<Candidate> candidates;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> shared_by_pair;
    for (const auto &entry : tracks_by_id) {
      std::vector<uint64_t> track_images;
      for (const Observation &observation : entry.second.observations)
        if (std::find(track_images.begin(), track_images.end(), observation.image_id) ==
            track_images.end())
          track_images.push_back(observation.image_id);
      std::sort(track_images.begin(), track_images.end());
      for (size_t left = 0; left < track_images.size(); ++left)
        for (size_t right = left + 1; right < track_images.size(); ++right)
          if (component_by_image[track_images[left]] ==
                  component_by_image[track_images[right]] &&
              component_by_image[track_images[left]] ==
                  component_by_image[component.key])
            ++shared_by_pair[{track_images[left], track_images[right]}];
    }
    for (const auto &entry : shared_by_pair)
      if (entry.second >= parameters->minimum_seed_tracks)
        candidates.push_back({entry.first.first, entry.first.second, entry.second});
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                if (a.shared != b.shared) return a.shared > b.shared;
                if (a.a != b.a) return a.a < b.a;
                return a.b < b.b;
              });
    const size_t candidate_limit = std::min<size_t>(
        candidates.size(), parameters->maximum_seed_candidates);
    result->seed_candidates_available += candidates.size();
    bool seeded = false;
    for (size_t candidate_index = 0; candidate_index < candidate_limit;
         ++candidate_index) {
      ++result->seed_candidates_considered;
      const Candidate &candidate = candidates[candidate_index];
      const Image &image_a = images[image_index[candidate.a]];
      const Image &image_b = images[image_index[candidate.b]];
      std::vector<Observation> pair_a, pair_b;
      std::vector<uint64_t> track_ids;
      for (const auto &entry : tracks_by_id) {
        const Observation *a = nullptr, *b = nullptr;
        for (const Observation &observation : entry.second.observations) {
          if (observation.image_id == candidate.a) a = &observation;
          if (observation.image_id == candidate.b) b = &observation;
        }
        if (a && b) {
          track_ids.push_back(entry.first);
          pair_a.push_back(*a);
          pair_b.push_back(*b);
        }
      }
      std::vector<Lardon3DSparseGeometryPoint2> pixels_a(pair_a.size());
      std::vector<Lardon3DSparseGeometryPoint2> pixels_b(pair_b.size());
      for (size_t i = 0; i < pair_a.size(); ++i) {
        pixels_a[i] = pair_a[i].pixel;
        pixels_b[i] = pair_b[i].pixel;
      }
      std::vector<uint8_t> mask(pair_a.size());
      Lardon3DSparseGeometryRelativePoseResult pose_result = {};
      pose_result.inlier_mask = mask.data();
      pose_result.inlier_mask_capacity = mask.size();
      auto relative = parameters->relative_pose;
      relative.minimum_inliers = std::max<uint32_t>(
          relative.minimum_inliers, parameters->minimum_seed_tracks);
      const Lardon3DSparseGeometryResult relative_status =
          lardon3d_sparse_geometry_relative_pose(
              &image_a.calibration, &image_b.calibration, pixels_a.data(),
              pixels_b.data(), pixels_a.size(), &relative, &pose_result);
      result->last_seed_geometry_status = relative_status;
      result->last_seed_parallax_rad = pose_result.median_parallax_rad;
      if (relative_status != LARDON3D_SPARSE_GEOMETRY_OK ||
          pose_result.inlier_count < parameters->minimum_seed_landmarks ||
         pose_result.median_parallax_rad < parameters->minimum_track_parallax_rad ||
          !finite_pose(pose_result.pose_ba))
        continue;
      Pose pose_a = {candidate.a, identity_pose()};
      Pose pose_b = {candidate.b, pose_result.pose_ba};
      std::vector<Landmark> seed_landmarks;
      for (size_t i = 0; i < pair_a.size(); ++i) {
        if (!mask[i]) continue;
        ++result->triangulation_attempts;
        Lardon3DSparseGeometryPoint2 normalized[2];
        if (lardon3d_sparse_geometry_normalize(&image_a.calibration,
                                                &pair_a[i].pixel, 1,
                                                &normalized[0]) !=
                LARDON3D_SPARSE_GEOMETRY_OK ||
            lardon3d_sparse_geometry_normalize(&image_b.calibration,
                                                &pair_b[i].pixel, 1,
                                                &normalized[1]) !=
                LARDON3D_SPARSE_GEOMETRY_OK)
          continue;
        Lardon3DSparseGeometryPoint3 point;
        if (lardon3d_sparse_geometry_triangulate_two_view(
                &normalized[0], &normalized[1], &pose_a.value, &pose_b.value,
                &point) != LARDON3D_SPARSE_GEOMETRY_OK)
        {
          ++result->triangulation_failures;
          continue;
        }
        Lardon3DSparseGeometryPoint3 refined;
        Lardon3DSparseGeometryPose seed_poses[2] = {pose_a.value, pose_b.value};
        if (lardon3d_sparse_geometry_refine_point(
                normalized, seed_poses, 2, &point,
                &parameters->refinement, &refined) ==
            LARDON3D_SPARSE_GEOMETRY_OK)
          point = refined;
        Landmark landmark = {track_ids[i], component.key, point, {pair_a[i], pair_b[i]},
                             0.0, 0.0};
        const double error_a = reprojection_error(image_a, pose_a, landmark, pair_a[i]);
        const double error_b = reprojection_error(image_b, pose_b, landmark, pair_b[i]);
        if (!std::isfinite(error_a) || !std::isfinite(error_b) ||
            std::max(error_a, error_b) > parameters->reprojection_threshold_px)
        {
          if (!std::isfinite(error_a) || !std::isfinite(error_b))
            ++result->rejected_behind_camera;
          else
            ++result->rejected_reprojection;
          continue;
        }
        landmark.rmse = std::sqrt((error_a * error_a + error_b * error_b) / 2.0);
        landmark.median = std::max(error_a, error_b);
        seed_landmarks.push_back(landmark);
      }
      if (seed_landmarks.size() < parameters->minimum_seed_landmarks) continue;
      cameras.push_back(pose_a);
      cameras.push_back(pose_b);
      registered.insert(candidate.a);
      registered.insert(candidate.b);
      for (Landmark &landmark : seed_landmarks) {
        accepted_tracks.insert(landmark.track_id);
        landmarks.push_back(landmark);
      }
      seeded = true;
      result->seed_image_a = candidate.a;
      result->seed_image_b = candidate.b;
      break;
    }
    if (!seeded) continue;

    bool stopped_without_growth = false;
    for (uint32_t round = 0;
         round < parameters->maximum_registration_rounds; ++round) {
      bool all_component_images_registered = true;
      for (size_t image_position : component.image_indices)
        if (!registered.count(images[image_position].id)) {
          all_component_images_registered = false;
          break;
        }
      if (all_component_images_registered) break;
      ++result->registration_rounds;
      struct CandidateImage { uint64_t id; uint32_t support; };
      std::vector<CandidateImage> image_candidates;
      for (size_t image_position : component.image_indices) {
        const uint64_t image_id = images[image_position].id;
        if (registered.count(image_id)) continue;
        uint32_t support = 0;
        for (const Landmark &landmark : landmarks) {
          for (const Observation &observation : tracks_by_id[landmark.track_id].observations)
            if (observation.image_id == image_id) { ++support; break; }
        }
        if (support >= parameters->minimum_pnp_correspondences)
          image_candidates.push_back({image_id, support});
      }
      std::sort(image_candidates.begin(), image_candidates.end(),
                [](const CandidateImage &a, const CandidateImage &b) {
                  if (a.support != b.support) return a.support > b.support;
                  return a.id < b.id;
                });
      if (image_candidates.empty()) {
        stopped_without_growth = true;
        break;
      }
      bool registered_one = false;
      for (const CandidateImage &candidate : image_candidates) {
        const Image &image = images[image_index[candidate.id]];
        std::vector<Lardon3DSparseGeometryPoint3> points;
        std::vector<Lardon3DSparseGeometryPoint2> pixels;
        std::vector<uint64_t> track_ids;
        for (const Landmark &landmark : landmarks) {
          const Track &track = tracks_by_id[landmark.track_id];
          for (const Observation &observation : track.observations)
            if (observation.image_id == candidate.id) {
              points.push_back(landmark.point);
              pixels.push_back(observation.pixel);
              track_ids.push_back(landmark.track_id);
              break;
            }
        }
        std::vector<uint8_t> mask(points.size());
        Lardon3DSparseGeometryPnPResult pnp_result = {};
        pnp_result.inlier_mask = mask.data();
        pnp_result.inlier_mask_capacity = mask.size();
        ++result->registration_attempts;
        if (lardon3d_sparse_geometry_pnp(
                &image.calibration, points.data(), pixels.data(), points.size(),
                &parameters->pnp, &pnp_result) != LARDON3D_SPARSE_GEOMETRY_OK ||
            !finite_pose(pnp_result.pose_cw))
        {
          ++result->registration_failures;
          continue;
        }
        ++result->registration_successes;
        result->last_pnp_inlier_count = pnp_result.inlier_count;
        Pose pose = {candidate.id, pnp_result.pose_cw};
        cameras.push_back(pose);
        registered.insert(candidate.id);
        registered_one = true;
        break;
      }
      if (!registered_one) {
        stopped_without_growth = true;
        break;
      }

      for (Landmark &landmark : landmarks) {
        if (landmark.component_key != component.key) continue;
        const Track &track = tracks_by_id[landmark.track_id];
        size_t eligible_count = 0;
        for (const Observation &observation : track.observations)
          if (registered.count(observation.image_id)) ++eligible_count;
        if (eligible_count <= landmark.observations.size()) continue;
        ++result->landmark_update_attempts;
        ++result->triangulation_attempts;
        Landmark replacement;
        Lardon3DSparseGeometryResult geometry_status =
            LARDON3D_SPARSE_GEOMETRY_OK;
        const LandmarkCandidateStatus update_status =
            build_landmark_candidate(
                track, component.key, images, image_index, cameras,
                registered, *parameters, &replacement, &geometry_status,
                result);
        result->last_triangulation_status = geometry_status;
        if (update_status == LandmarkCandidateStatus::accepted) {
          landmark = std::move(replacement);
          ++result->landmark_update_successes;
        } else {
          ++result->landmark_update_failures;
          record_landmark_rejection(update_status, result);
        }
      }

      size_t added = 0;
      for (const auto &entry : tracks_by_id) {
        if (accepted_tracks.count(entry.first) || added >= parameters->maximum_landmarks_per_round)
          continue;
        size_t eligible_count = 0;
        for (const Observation &observation : entry.second.observations)
          if (registered.count(observation.image_id)) ++eligible_count;
        if (eligible_count < 2) continue;
        ++result->triangulation_attempts;
        Landmark landmark;
        Lardon3DSparseGeometryResult geometry_status =
            LARDON3D_SPARSE_GEOMETRY_OK;
        const LandmarkCandidateStatus candidate_status =
            build_landmark_candidate(
                entry.second, component.key, images, image_index, cameras,
                registered, *parameters, &landmark, &geometry_status, result);
        result->last_triangulation_status = geometry_status;
        if (candidate_status != LandmarkCandidateStatus::accepted) {
          record_landmark_rejection(candidate_status, result);
          continue;
        }
        landmarks.push_back(landmark);
        accepted_tracks.insert(entry.first);
        ++added;
      }
    }
    size_t component_registered = 0;
    for (size_t image_position : component.image_indices)
      if (registered.count(images[image_position].id)) ++component_registered;
    if (component_registered < component.image_indices.size()) {
      if (stopped_without_growth)
        ++result->no_growth_terminations;
      else
        ++result->round_limit_terminations;
    }
  }

  std::sort(cameras.begin(), cameras.end(),
            [](const Pose &a, const Pose &b) { return a.image_id < b.image_id; });
  std::sort(landmarks.begin(), landmarks.end(),
            [](const Landmark &a, const Landmark &b) {
              if (a.component_key != b.component_key)
                return a.component_key < b.component_key;
              return a.track_id < b.track_id;
            });
  for (const Component &component : components) {
    for (auto &output : output_components)
      if (output.component_key == component.key) {
        for (const Pose &camera : cameras)
          if (component_by_image[camera.image_id] ==
              component_by_image[component.key]) ++output.registered_image_count;
        for (const Landmark &landmark : landmarks)
          if (landmark.component_key == component.key) ++output.landmark_count;
      }
    for (size_t image_index_value : component.image_indices) {
      const uint64_t image_id = images[image_index_value].id;
      if (!registered.count(image_id))
        result->unregistered_image_count++;
    }
  }

  result->component_count = output_components.size();
  result->camera_count = cameras.size();
  result->landmark_count = landmarks.size();
  if (result->component_count) {
    result->components = static_cast<Lardon3DSparseIncrementalComponent *>(
        std::malloc(result->component_count * sizeof(*result->components)));
    if (!result->components) { destroy_result(result); return LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY; }
    std::copy(output_components.begin(), output_components.end(), result->components);
  }
  if (result->camera_count) {
    result->cameras = static_cast<Lardon3DSparseIncrementalCamera *>(
        std::malloc(result->camera_count * sizeof(*result->cameras)));
    if (!result->cameras) { destroy_result(result); return LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY; }
    for (size_t i = 0; i < cameras.size(); ++i)
      result->cameras[i] = {
          cameras[i].image_id,
          components[component_by_image[cameras[i].image_id]].key,
          cameras[i].value};
  }
  if (result->landmark_count) {
    result->landmarks = static_cast<Lardon3DSparseIncrementalLandmark *>(
        std::malloc(result->landmark_count * sizeof(*result->landmarks)));
    if (!result->landmarks) { destroy_result(result); return LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY; }
    size_t observation_count = 0;
    for (const Landmark &landmark : landmarks) observation_count += landmark.observations.size();
    result->observation_count = observation_count;
    result->observations = static_cast<Lardon3DSparseIncrementalLandmarkObservation *>(
        std::malloc(observation_count * sizeof(*result->observations)));
    if (!result->observations) { destroy_result(result); return LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY; }
    size_t observation_offset = 0;
    for (size_t i = 0; i < landmarks.size(); ++i) {
      const Landmark &landmark = landmarks[i];
      result->landmarks[i] = {landmark.track_id, landmark.track_id, landmark.component_key,
                              landmark.point, landmark.rmse, landmark.median,
                              landmark.observations.size()};
      for (size_t position = 0; position < landmark.observations.size(); ++position) {
        const Observation &observation = landmark.observations[position];
        result->observations[observation_offset++] = {
            landmark.track_id, landmark.track_id, observation.image_id,
            observation.feature_set_id, observation.feature_index,
            static_cast<uint32_t>(position)};
      }
    }
  }
  if (result->unregistered_image_count) {
    result->unregistered_images = static_cast<Lardon3DSparseIncrementalUnregisteredImage *>(
        std::malloc(result->unregistered_image_count * sizeof(*result->unregistered_images)));
    if (!result->unregistered_images) { destroy_result(result); return LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY; }
    size_t offset = 0;
    for (const Image &image : images)
      if (!registered.count(image.id))
        result->unregistered_images[offset++] = {
            image.id, components[component_by_image[image.id]].key};
  }
  result->track_set_id = input->track_set_id;
  result->calibration_scope_id = input->calibration_scope_id;
  result->status = result->camera_count == 0
                       ? LARDON3D_SPARSE_INCREMENTAL_FAILED
                       : result->unregistered_image_count == 0
                             ? LARDON3D_SPARSE_INCREMENTAL_COMPLETE
                             : LARDON3D_SPARSE_INCREMENTAL_PARTIAL;
  return result->status;
  } catch (const std::bad_alloc &) {
    destroy_result(result);
    return LARDON3D_SPARSE_INCREMENTAL_OUT_OF_MEMORY;
  } catch (...) {
    destroy_result(result);
    return LARDON3D_SPARSE_INCREMENTAL_FAILED;
  }
}

extern "C" void lardon3d_sparse_incremental_result_destroy(
    Lardon3DSparseIncrementalResult *result) {
  destroy_result(result);
}
