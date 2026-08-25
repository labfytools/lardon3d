#include "sparse_sfm_gate_f_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <new>
#include <set>
#include <string>
#include <tuple>
#include <vector>

extern "C" {
#include <lardon3d/feature_store.h>
#include <lardon3d/project.h>
#include <lardon3d/sparse_sfm_bundle_adjustment.h>
#include <lardon3d/sparse_sfm_model.h>
#include <lardon3d/sparse_sfm_task.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>
}

namespace {
struct Context {
  std::string project_path;
  Lardon3DProjectDb *database = nullptr;
  Lardon3DProjectDbSparseSfmTask durable{};
  struct WorkloadShape {
    uint64_t image_count = 0;
    uint64_t track_count = 0;
    uint64_t observation_count = 0;
  } shape;
};

struct ObservationKey {
  uint64_t feature_set_id;
  uint32_t feature_index;

  bool operator<(const ObservationKey &other) const {
    return std::tie(feature_set_id, feature_index) <
           std::tie(other.feature_set_id, other.feature_index);
  }
};

struct Reader {
  Lardon3DProjectDbFeatureSet feature_set{};
  Lardon3DFeatureReader *reader = nullptr;
  Lardon3DFeatureFileMetadata metadata{};
};

struct Resolved {
  std::vector<Lardon3DSparseIncrementalImage> images;
  std::vector<Lardon3DSparseIncrementalObservation> observations;
  std::map<uint64_t, Lardon3DSparseGeometryCalibration> calibrations;
  std::map<ObservationKey, size_t> observation_index;
};

struct PublicationStorage {
  std::vector<Lardon3DSparseComponent> components;
  std::vector<Lardon3DSparseRegisteredImage> cameras;
  std::vector<Lardon3DSparseLandmark> landmarks;
  std::vector<Lardon3DSparseLandmarkObservation> observations;
  double rmse = 0.0;
  double median = 0.0;
};

void destroy_context(void *userdata) { delete static_cast<Context *>(userdata); }

bool checked_add(uint64_t left, uint64_t right, uint64_t *output) {
  if (right > UINT64_MAX - left) return false;
  *output = left + right;
  return true;
}

bool load_calibration_membership(const Context &context, std::set<uint64_t> *images) {
  Lardon3DSparseCalibrationScope scope{};
  if (lardon3d_sparse_calibration_scope_load(context.database,
                                              context.durable.calibration_scope_id, &scope) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  uint64_t after = 0;
  for (;;) {
    Lardon3DSparseCalibrationMember members[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    uint64_t next = 0;
    if (lardon3d_sparse_calibration_scope_list_members(
            context.database, scope.scope_id, after, members, LARDON3D_SPARSE_SFM_PAGE_MAX,
            &count, &next) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < count; ++index)
      if (!images->insert(members[index].image_id).second) return false;
    if (count == 0) break;
    if (next <= after) return false;
    after = next;
  }
  return images->size() == scope.member_count;
}

bool derive_workload_shape(const Context &context, Context::WorkloadShape *shape) {
  Lardon3DProjectDbTrackSet track_set{};
  if (!shape ||
      lardon3d_project_db_load_track_set(context.database, context.durable.track_set_id,
                                         &track_set) != LARDON3D_PROJECT_DB_OK)
    return false;
  std::set<uint64_t> calibration_images;
  if (!load_calibration_membership(context, &calibration_images)) return false;
  std::map<uint64_t, Lardon3DProjectDbFeatureSet> feature_sets;
  std::set<uint64_t> participating_images;
  Context::WorkloadShape result{};
  uint64_t after = 0;
  for (;;) {
    Lardon3DProjectDbTrack tracks[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    if (lardon3d_project_db_list_tracks(context.database, track_set.track_set_id, after, tracks,
                                        LARDON3D_SPARSE_SFM_PAGE_MAX, &count) !=
        LARDON3D_PROJECT_DB_OK)
      return false;
    bool ok = true;
    for (size_t index = 0; ok && index < count; ++index) {
      const auto &track = tracks[index];
      if (track.track_id <= after || track.track_set_id != track_set.track_set_id ||
          !checked_add(result.observation_count, track.observation_count,
                       &result.observation_count)) {
        ok = false;
        break;
      }
      for (uint32_t position = 0; position < track.observation_count; ++position) {
        const auto &observation = track.observations[position];
        auto feature = feature_sets.find(observation.feature_set_id);
        if (feature == feature_sets.end()) {
          Lardon3DProjectDbFeatureSet feature_set{};
          if (lardon3d_project_db_load_feature_set(context.database,
                                                   observation.feature_set_id,
                                                   &feature_set) != LARDON3D_PROJECT_DB_OK) {
            ok = false;
            break;
          }
          feature = feature_sets.emplace(observation.feature_set_id, feature_set).first;
        }
        if (observation.feature_index >= feature->second.feature_count ||
            !calibration_images.count(feature->second.image_id)) {
          ok = false;
          break;
        }
        participating_images.insert(feature->second.image_id);
      }
      after = track.track_id;
      ++result.track_count;
    }
    for (size_t index = 0; index < count; ++index)
      lardon3d_project_db_free_track(&tracks[index]);
    if (!ok) return false;
    if (count == 0) break;
  }
  result.image_count = participating_images.size();
  if (result.track_count != track_set.track_count || result.image_count == 0 ||
      result.observation_count == 0)
    return false;
  *shape = result;
  return true;
}

bool build_observation_index(Resolved *resolved) {
  resolved->observation_index.clear();
  for (size_t index = 0; index < resolved->observations.size(); ++index) {
    const auto &observation = resolved->observations[index];
    if (!resolved->observation_index
             .emplace(ObservationKey{observation.feature_set_id, observation.feature_index}, index)
             .second)
      return false;
  }
  return true;
}

void project_landmarks(const Lardon3DSparseIncrementalLandmark *landmarks,
                       size_t landmark_count, const std::set<uint64_t> &component_keys,
                       std::vector<Lardon3DSparseLandmark> *output) {
  for (size_t index = 0; index < landmark_count; ++index) {
    const auto &landmark = landmarks[index];
    if (!component_keys.count(landmark.component_key)) continue;
    output->push_back(
        {0, landmark.track_id, landmark.component_key, landmark.point.x, landmark.point.y,
         landmark.point.z, landmark.reprojection_rmse_px, landmark.reprojection_median_px,
         landmark.observation_count});
  }
  std::sort(output->begin(), output->end(), [](const auto &a, const auto &b) {
    return std::tie(a.component_key, a.track_id) <
           std::tie(b.component_key, b.track_id);
  });
}

void close_readers(std::map<uint64_t, Reader> *readers) {
  for (auto &item : *readers)
    if (item.second.reader) lardon3d_feature_reader_close(item.second.reader);
  readers->clear();
}

bool load_reader(const Context &context, uint64_t feature_set_id,
                 std::map<uint64_t, Reader> *readers, Reader **output) {
  auto existing = readers->find(feature_set_id);
  if (existing != readers->end()) {
    *output = &existing->second;
    return true;
  }
  Reader candidate;
  if (lardon3d_project_db_load_feature_set(context.database, feature_set_id,
                                           &candidate.feature_set) != LARDON3D_PROJECT_DB_OK ||
      lardon3d_feature_reader_open(context.project_path.c_str(), &candidate.feature_set,
                                   &candidate.reader, &candidate.metadata) !=
          LARDON3D_FEATURE_STORE_OK ||
      candidate.metadata.feature_count != candidate.feature_set.feature_count) {
    if (candidate.reader) lardon3d_feature_reader_close(candidate.reader);
    return false;
  }
  auto inserted = readers->emplace(feature_set_id, candidate);
  *output = &inserted.first->second;
  return true;
}

bool load_calibrations(const Context &context, Resolved *resolved) {
  Lardon3DSparseCalibrationScope scope{};
  if (lardon3d_sparse_calibration_scope_load(context.database,
                                              context.durable.calibration_scope_id, &scope) !=
      LARDON3D_PROJECT_DB_OK) return false;
  uint64_t after = 0;
  for (;;) {
    Lardon3DSparseCalibrationMember members[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    uint64_t next = 0;
    if (lardon3d_sparse_calibration_scope_list_members(
            context.database, scope.scope_id, after, members, LARDON3D_SPARSE_SFM_PAGE_MAX,
            &count, &next) != LARDON3D_PROJECT_DB_OK) return false;
    for (size_t index = 0; index < count; ++index) {
      Lardon3DSparseCalibration calibration{};
      if (lardon3d_sparse_calibration_load(context.database, members[index].calibration_id,
                                           &calibration) != LARDON3D_PROJECT_DB_OK ||
          std::memcmp(calibration.scientific_hash, members[index].calibration_hash, 32) != 0)
        return false;
      resolved->calibrations.emplace(
          members[index].image_id,
          Lardon3DSparseGeometryCalibration{calibration.width, calibration.height, calibration.fx,
                                            calibration.fy, calibration.cx, calibration.cy,
                                            calibration.k1, calibration.k2, calibration.p1,
                                            calibration.p2});
    }
    if (count == 0) break;
    if (next <= after) return false;
    after = next;
  }
  return resolved->calibrations.size() == scope.member_count;
}

bool resolve_input(const Context &context, Resolved *resolved, uint64_t *track_count) {
  Lardon3DProjectDbTrackSet track_set{};
  if (lardon3d_project_db_load_track_set(context.database, context.durable.track_set_id,
                                         &track_set) != LARDON3D_PROJECT_DB_OK ||
      !load_calibrations(context, resolved)) return false;
  std::map<uint64_t, Reader> readers;
  std::set<uint64_t> participating_images;
  uint64_t after = 0;
  uint64_t tracks_seen = 0;
  bool ok = true;
  while (ok) {
    Lardon3DProjectDbTrack tracks[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    if (lardon3d_project_db_list_tracks(context.database, track_set.track_set_id, after, tracks,
                                        LARDON3D_SPARSE_SFM_PAGE_MAX, &count) !=
        LARDON3D_PROJECT_DB_OK) {
      ok = false;
      break;
    }
    for (size_t index = 0; ok && index < count; ++index) {
      const Lardon3DProjectDbTrack &track = tracks[index];
      if (track.track_id <= after || track.track_set_id != track_set.track_set_id) {
        ok = false;
        break;
      }
      for (uint32_t position = 0; ok && position < track.observation_count; ++position) {
        const Lardon3DProjectDbTrackObservation &observation = track.observations[position];
        Reader *reader = nullptr;
        Lardon3DFeatureKeypoint keypoint{};
        if (!load_reader(context, observation.feature_set_id, &readers, &reader) ||
            observation.feature_index >= reader->metadata.feature_count ||
            lardon3d_feature_reader_keypoints(reader->reader, observation.feature_index, &keypoint,
                                              1) != LARDON3D_FEATURE_STORE_OK ||
            !std::isfinite(keypoint.x) || !std::isfinite(keypoint.y) ||
            resolved->calibrations.find(reader->feature_set.image_id) ==
                resolved->calibrations.end()) {
          ok = false;
          break;
        }
        participating_images.insert(reader->feature_set.image_id);
        resolved->observations.push_back(
            {track.track_id, reader->feature_set.image_id, observation.feature_set_id,
             observation.feature_index, reader->metadata.feature_count,
             static_cast<double>(keypoint.x), static_cast<double>(keypoint.y)});
      }
      after = track.track_id;
      ++tracks_seen;
    }
    for (size_t index = 0; index < count; ++index)
      lardon3d_project_db_free_track(&tracks[index]);
    if (!ok || count == 0) break;
  }
  close_readers(&readers);
  if (!ok || tracks_seen != track_set.track_count) return false;
  for (uint64_t image_id : participating_images)
    resolved->images.push_back({image_id, resolved->calibrations.at(image_id)});
  *track_count = tracks_seen;
  return !resolved->images.empty() && !resolved->observations.empty() &&
         build_observation_index(resolved);
}

bool persist(Context *context, const Lardon3DTask *task) {
  Lardon3DTaskDurableSnapshot snapshot{};
  if (!lardon3d_task_durable_snapshot(task, &snapshot)) return false;
  Lardon3DProjectDbCheckpoint checkpoint{};
  std::snprintf(checkpoint.path, sizeof(checkpoint.path), ".lardon3d/checkpoints/%llu.chk",
                static_cast<unsigned long long>(snapshot.id));
  checkpoint.format_version = LARDON3D_TASK_CHECKPOINT_VERSION;
  checkpoint.updated_at = 0;
  std::string path = context->project_path + "/" + checkpoint.path;
  Lardon3DTaskCheckpointResult saved = lardon3d_task_checkpoint_save(path.c_str(), &snapshot);
  if (saved != LARDON3D_TASK_CHECKPOINT_OK &&
      saved != LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE) return false;
  checkpoint.durability = saved == LARDON3D_TASK_CHECKPOINT_OK
                              ? LARDON3D_DB_CHECKPOINT_DURABLE
                              : LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE;
  return lardon3d_project_db_record_sparse_sfm_task(
             context->database, &snapshot, LARDON3D_SPARSE_SFM_TASK_KIND,
             LARDON3D_SPARSE_SFM_TASK_KIND_VERSION, &checkpoint, &context->durable, 0) ==
         LARDON3D_PROJECT_DB_OK;
}

const Lardon3DSparseIncrementalObservation *find_input_observation(
    const Resolved &resolved, uint64_t track_id, uint64_t image_id, uint64_t feature_set_id,
    uint32_t feature_index) {
  auto found = resolved.observation_index.find(ObservationKey{feature_set_id, feature_index});
  if (found == resolved.observation_index.end()) return nullptr;
  const auto &item = resolved.observations[found->second];
  return item.track_id == track_id && item.image_id == image_id ? &item : nullptr;
}

bool build_publication(const Resolved &resolved,
                       const Lardon3DSparseBundleAdjustmentResult &result,
                       PublicationStorage *storage) {
  std::set<uint64_t> component_keys;
  for (size_t index = 0; index < result.component_count; ++index) {
    const auto &component = result.components[index];
    if (lardon3d_sparse_sfm_component_persistable(component.registered_image_count,
                                                   component.landmark_count)) {
      component_keys.insert(component.component_key);
      storage->components.push_back({0, component.component_key, component.registered_image_count,
                                     component.landmark_count});
    }
  }
  if (storage->components.empty()) return false;
  for (size_t index = 0; index < result.camera_count; ++index) {
    const auto &camera = result.cameras[index];
    if (!component_keys.count(camera.component_key)) continue;
    Lardon3DSparseRegisteredImage output{};
    output.image_id = camera.image_id;
    output.component_key = camera.component_key;
    std::memcpy(output.rotation_cw, camera.pose_cw.rotation_cw, sizeof(output.rotation_cw));
    std::memcpy(output.translation_cw, camera.pose_cw.translation_cw,
                sizeof(output.translation_cw));
    storage->cameras.push_back(output);
  }
  project_landmarks(result.landmarks, result.landmark_count, component_keys,
                    &storage->landmarks);
  std::sort(storage->cameras.begin(), storage->cameras.end(),
            [](const auto &a, const auto &b) { return a.image_id < b.image_id; });
  std::map<uint64_t, const Lardon3DSparseRegisteredImage *> cameras;
  for (const auto &camera : storage->cameras) cameras.emplace(camera.image_id, &camera);
  std::map<uint64_t, const Lardon3DSparseLandmark *> landmarks;
  for (const auto &landmark : storage->landmarks) landmarks.emplace(landmark.track_id, &landmark);
  std::vector<double> squared_errors;
  for (size_t index = 0; index < result.observation_count; ++index) {
    const auto &observation = result.observations[index];
    auto landmark = landmarks.find(observation.track_id);
    if (landmark == landmarks.end()) continue;
    auto camera = cameras.find(observation.image_id);
    auto calibration = resolved.calibrations.find(observation.image_id);
    const auto *source = find_input_observation(resolved, observation.track_id,
                                                observation.image_id,
                                                observation.feature_set_id,
                                                observation.feature_index);
    if (camera == cameras.end() || calibration == resolved.calibrations.end() || !source)
      return false;
    const auto &pose = *camera->second;
    const auto &point = *landmark->second;
    double xc = pose.rotation_cw[0] * point.x + pose.rotation_cw[1] * point.y +
                    pose.rotation_cw[2] * point.z + pose.translation_cw[0];
    double yc = pose.rotation_cw[3] * point.x + pose.rotation_cw[4] * point.y +
                    pose.rotation_cw[5] * point.z + pose.translation_cw[1];
    double zc = pose.rotation_cw[6] * point.x + pose.rotation_cw[7] * point.y +
                    pose.rotation_cw[8] * point.z + pose.translation_cw[2];
    if (!std::isfinite(xc) || !std::isfinite(yc) || !std::isfinite(zc) || zc <= 1e-9)
      return false;
    double xn = xc / zc, yn = yc / zc, r2 = xn * xn + yn * yn;
    const auto &cal = calibration->second;
    double radial = 1.0 + cal.k1 * r2 + cal.k2 * r2 * r2;
    double xd = xn * radial + 2.0 * cal.p1 * xn * yn + cal.p2 * (r2 + 2.0 * xn * xn);
    double yd = yn * radial + cal.p1 * (r2 + 2.0 * yn * yn) + 2.0 * cal.p2 * xn * yn;
    double dx = cal.fx * xd + cal.cx - source->x;
    double dy = cal.fy * yd + cal.cy - source->y;
    double squared = dx * dx + dy * dy;
    if (!std::isfinite(squared)) return false;
    squared_errors.push_back(squared);
    storage->observations.push_back(
        {0, observation.track_id, observation.feature_set_id, observation.feature_index,
         observation.position_in_track});
  }
  if (storage->cameras.size() < 2 || storage->landmarks.empty() || squared_errors.empty())
    return false;
  std::sort(storage->observations.begin(), storage->observations.end(), [](const auto &a,
                                                                          const auto &b) {
    return std::tie(a.track_id, a.position_in_track) <
           std::tie(b.track_id, b.position_in_track);
  });
  return lardon3d_sparse_sfm_publication_metrics(squared_errors.data(), squared_errors.size(),
                                                  &storage->rmse, &storage->median);
}

bool execute(Context *context, Lardon3DTask *task, bool *reused) {
  unsigned char fingerprint[32]{};
  if (!lardon3d_sparse_sfm_parameter_fingerprint(&context->durable.parameters, fingerprint))
    return false;
  Lardon3DSparseReconstruction existing{};
  Lardon3DProjectDbResult lookup = lardon3d_sparse_reconstruction_find_exact(
      context->database, context->durable.track_set_id, context->durable.calibration_scope_id,
      context->durable.sfm_kind, context->durable.sfm_version, fingerprint, &existing);
  if (lookup == LARDON3D_PROJECT_DB_OK) {
    *reused = true;
    return true;
  }
  if (lookup != LARDON3D_PROJECT_DB_NOT_FOUND || !lardon3d_task_checkpoint(task)) return false;
  Resolved resolved;
  uint64_t track_count = 0;
  if (!resolve_input(*context, &resolved, &track_count)) return false;
  if (track_count != context->shape.track_count ||
      resolved.observations.size() != context->shape.observation_count ||
      resolved.images.size() != context->shape.image_count)
    return false;
  Lardon3DSparseIncrementalInput input{context->durable.track_set_id,
                                       context->durable.calibration_scope_id,
                                       resolved.images.data(), resolved.images.size(),
                                       resolved.observations.data(), resolved.observations.size()};
  Lardon3DSparseIncrementalResult incremental{};
  Lardon3DSparseIncrementalStatus d_status = lardon3d_sparse_incremental_run(
      &input, &context->durable.parameters, &incremental);
  if (d_status != LARDON3D_SPARSE_INCREMENTAL_COMPLETE &&
      d_status != LARDON3D_SPARSE_INCREMENTAL_PARTIAL) {
    lardon3d_sparse_incremental_result_destroy(&incremental);
    return false;
  }
  if (!lardon3d_task_checkpoint(task)) {
    lardon3d_sparse_incremental_result_destroy(&incremental);
    return false;
  }
  Lardon3DSparseBundleAdjustmentInput e_input{&incremental, resolved.images.data(),
                                              resolved.images.size(),
                                              resolved.observations.data(),
                                              resolved.observations.size()};
  Lardon3DSparseBundleAdjustmentResult adjusted{};
  auto e_status = lardon3d_sparse_bundle_adjustment_run(&e_input, &adjusted);
  lardon3d_sparse_incremental_result_destroy(&incremental);
  if (e_status != LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK ||
      !lardon3d_task_checkpoint(task)) {
    lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
    return false;
  }
  PublicationStorage storage;
  bool valid = build_publication(resolved, adjusted, &storage);
  if (!valid) {
    lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
    return false;
  }
  Lardon3DSparsePublication publication{
      context->durable.track_set_id,
      context->durable.calibration_scope_id,
      context->durable.sfm_kind,
      context->durable.sfm_version,
      {},
      storage.components.data(),
      storage.components.size(),
      storage.cameras.data(),
      storage.cameras.size(),
      storage.landmarks.data(),
      storage.landmarks.size(),
      storage.observations.data(),
      storage.observations.size(),
      storage.rmse,
      storage.median,
      static_cast<int64_t>(std::time(nullptr))};
  std::memcpy(publication.parameter_fingerprint, fingerprint, 32);
  Lardon3DSparseReconstruction published{};
  Lardon3DProjectDbResult publish_result =
      lardon3d_sparse_reconstruction_publish(context->database, &publication, &published);
  lardon3d_sparse_bundle_adjustment_result_destroy(&adjusted);
  if (publish_result == LARDON3D_PROJECT_DB_OK) return true;
  if (publish_result != LARDON3D_PROJECT_DB_CONSTRAINT) return false;
  lookup = lardon3d_sparse_reconstruction_find_exact(
      context->database, context->durable.track_set_id, context->durable.calibration_scope_id,
      context->durable.sfm_kind, context->durable.sfm_version, fingerprint, &existing);
  *reused = lookup == LARDON3D_PROJECT_DB_OK;
  return *reused;
}

bool run(Lardon3DTask *task, void *userdata) {
  try {
    bool reused = false;
    if (!execute(static_cast<Context *>(userdata), task, &reused))
      return lardon3d_task_fail(task, "Sparse SfM non publiable.");
    return lardon3d_task_set_progress(task, 100,
                                      reused ? "Reconstruction réutilisée."
                                             : "Reconstruction publiée.");
  } catch (const std::bad_alloc &) {
    return lardon3d_task_fail(task, "Mémoire insuffisante pour Sparse SfM.");
  } catch (...) {
    return lardon3d_task_fail(task, "Erreur interne Sparse SfM.");
  }
}

void finished(const Lardon3DTask *task, void *userdata) {
  try {
    (void)persist(static_cast<Context *>(userdata), task);
  } catch (...) {
  }
}
} // namespace

#ifdef LARDON3D_SPARSE_SFM_TASK_TESTING
extern "C" int lardon3d_sparse_sfm_task_test_observation_lookup(
    const Lardon3DSparseIncrementalObservation *observations, size_t count,
    uint64_t track_id, uint64_t image_id, uint64_t feature_set_id,
    uint32_t feature_index, size_t *resolved_index) {
  try {
    if ((!observations && count != 0) || !resolved_index) return -1;
    Resolved resolved;
    if (count != 0) resolved.observations.assign(observations, observations + count);
    if (!build_observation_index(&resolved)) return -1;
    const auto *found = find_input_observation(resolved, track_id, image_id,
                                               feature_set_id, feature_index);
    if (!found) return 0;
    *resolved_index = static_cast<size_t>(found - resolved.observations.data());
    return 1;
  } catch (...) {
    return -1;
  }
}

extern "C" bool lardon3d_sparse_sfm_task_test_project_landmarks(
    const Lardon3DSparseIncrementalLandmark *landmarks, size_t landmark_count,
    const uint64_t *component_keys, size_t component_count,
    Lardon3DSparseLandmark *output, size_t output_capacity, size_t *output_count) {
  try {
    if ((!landmarks && landmark_count != 0) ||
        (!component_keys && component_count != 0) || !output_count ||
        (!output && output_capacity != 0))
      return false;
    std::set<uint64_t> persistable_components;
    for (size_t index = 0; index < component_count; ++index)
      if (!persistable_components.insert(component_keys[index]).second) return false;
    std::vector<Lardon3DSparseLandmark> projected;
    project_landmarks(landmarks, landmark_count, persistable_components, &projected);
    if (projected.size() > output_capacity || (!output && !projected.empty())) return false;
    std::copy(projected.begin(), projected.end(), output);
    *output_count = projected.size();
    return true;
  } catch (...) {
    return false;
  }
}
#endif

extern "C" bool lardon3d_sparse_sfm_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  try {
    if (!snapshot || !userdata || !binding) return false;
    auto *runtime = static_cast<Lardon3DTaskReconstructionContext *>(userdata);
    Lardon3DProjectDbSparseSfmTask durable{};
    if (lardon3d_project_db_load_sparse_sfm_task(runtime->project_db, snapshot->id, &durable) !=
        LARDON3D_PROJECT_DB_OK) return false;
    auto *context = new (std::nothrow) Context;
    if (!context) return false;
    context->project_path = runtime->project_path;
    context->database = runtime->project_db;
    context->durable = durable;
    unsigned char fingerprint[32];
    if (!lardon3d_sparse_sfm_parameter_fingerprint(&durable.parameters, fingerprint) ||
        !derive_workload_shape(*context, &context->shape)) {
      delete context;
      return false;
    }
    *binding = {run, context, destroy_context, finished, context};
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" Lardon3DTask *lardon3d_project_create_sparse_sfm_task(
    Lardon3DAppState *state, const Lardon3DSparseSfmTaskConfiguration *configuration,
    uint64_t *task_id) {
  try {
    if (task_id) *task_id = 0;
    if (!state || !state->project_loaded || !state->project_db || !state->resource_governor ||
        !configuration)
      return nullptr;
    Context temporary;
    temporary.project_path = state->project_path;
    temporary.database = state->project_db;
    temporary.durable.track_set_id = configuration->track_set_id;
    temporary.durable.calibration_scope_id = configuration->calibration_scope_id;
    if (!derive_workload_shape(temporary, &temporary.shape)) return nullptr;
    Lardon3DResourceEstimate estimate{};
    if (!lardon3d_sparse_sfm_resource_estimate(
            temporary.shape.image_count, temporary.shape.track_count,
            temporary.shape.observation_count, &estimate))
      return nullptr;
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK)
      return nullptr;
    auto *context = new (std::nothrow) Context;
    if (!context) return nullptr;
    context->project_path = state->project_path;
    context->database = state->project_db;
    context->shape = temporary.shape;
    context->durable = {id, configuration->track_set_id, configuration->calibration_scope_id,
                        LARDON3D_SPARSE_SFM_KIND_INCREMENTAL, LARDON3D_SPARSE_SFM_VERSION,
                        configuration->parameters};
    unsigned char fingerprint[32];
    if (!lardon3d_sparse_sfm_parameter_fingerprint(&context->durable.parameters, fingerprint)) {
      delete context;
      return nullptr;
    }
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Sparse SfM", &estimate, LARDON3D_SPARSE_SFM_TASK_KIND,
        LARDON3D_SPARSE_SFM_TASK_KIND_VERSION, run, context, destroy_context);
    if (!task || !lardon3d_task_assign_id(task, id) ||
        !lardon3d_task_set_finished_callback(task, finished, context) || !persist(context, task)) {
      if (task) lardon3d_task_destroy(task);
      else delete context;
      return nullptr;
    }
    if (task_id) *task_id = id;
    return task;
  } catch (...) {
    if (task_id) *task_id = 0;
    return nullptr;
  }
}

extern "C" bool lardon3d_project_enqueue_sparse_sfm_task(
    Lardon3DAppState *state, const Lardon3DSparseSfmTaskConfiguration *configuration,
    uint64_t *task_id) {
  Lardon3DTask *task = lardon3d_project_create_sparse_sfm_task(state, configuration, task_id);
  if (!task || !lardon3d_task_queue_add(state->task_queue, task, nullptr)) {
    if (task) lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
