extern "C" {
#include <lardon3d/app_state.h>
#include <lardon3d/incremental_reconstruction_task.h>
}

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

#include "sparse_sfm_gate_f_internal.h"

extern "C" {
#include <lardon3d/feature_store.h>
#include <lardon3d/incremental_reconstruction.h>
#include <lardon3d/project.h>
#include <lardon3d/sparse_sfm_model.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>
}

namespace {
struct Context {
  std::string project_path;
  Lardon3DProjectDb *database = nullptr;
  Lardon3DProjectDbIncrementalReconstructionTask durable{};
  Lardon3DIncrementalReconstructionShape shape{};
};

struct Reader {
  Lardon3DProjectDbFeatureSet feature_set{};
  Lardon3DFeatureReader *reader = nullptr;
  Lardon3DFeatureFileMetadata metadata{};
};

struct Resolved {
  Lardon3DSparseReconstruction base_record{};
  Lardon3DSparseIncrementalResult base{};
  std::vector<Lardon3DSparseIncrementalComponent> base_components;
  std::vector<Lardon3DSparseIncrementalCamera> base_cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> base_landmarks;
  std::vector<Lardon3DSparseIncrementalLandmarkObservation> base_observations;
  std::vector<Lardon3DSparseIncrementalObservation> base_track_observations;
  std::vector<Lardon3DSparseIncrementalImage> extension_images;
  std::vector<Lardon3DSparseIncrementalObservation> extension_observations;
  std::map<uint64_t, Lardon3DSparseGeometryCalibration> calibrations;
};

void destroy_context(void *userdata) { delete static_cast<Context *>(userdata); }

bool valid_durable(const Lardon3DProjectDbIncrementalReconstructionTask &durable) {
  unsigned char expected[32];
  return durable.task_id != 0 && durable.base_reconstruction_id != 0 &&
         durable.extension_track_set_id != 0 && durable.calibration_scope_id != 0 &&
         durable.incremental_kind == LARDON3D_INCREMENTAL_RECONSTRUCTION_KIND &&
         durable.incremental_version == LARDON3D_INCREMENTAL_RECONSTRUCTION_VERSION &&
         lardon3d_incremental_reconstruction_parameter_fingerprint(expected) &&
         std::memcmp(expected, durable.parameter_fingerprint, 32) == 0;
}

bool checked_add(uint64_t left, uint64_t right, uint64_t *output) {
  if (!output || right > UINT64_MAX - left) return false;
  *output = left + right;
  return true;
}

bool load_scope(Lardon3DProjectDb *database, uint64_t scope_id,
                std::map<uint64_t, Lardon3DSparseCalibrationMember> *members,
                std::map<uint64_t, Lardon3DSparseGeometryCalibration> *calibrations) {
  Lardon3DSparseCalibrationScope scope{};
  if (!database || !members ||
      lardon3d_sparse_calibration_scope_load(database, scope_id, &scope) !=
          LARDON3D_PROJECT_DB_OK)
    return false;
  uint64_t after = 0;
  for (;;) {
    Lardon3DSparseCalibrationMember page[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    uint64_t next = 0;
    if (lardon3d_sparse_calibration_scope_list_members(
            database, scope_id, after, page, LARDON3D_SPARSE_SFM_PAGE_MAX,
            &count, &next) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < count; ++index) {
      if (!members->emplace(page[index].image_id, page[index]).second) return false;
      if (calibrations) {
        Lardon3DSparseCalibration calibration{};
        if (lardon3d_sparse_calibration_load(database, page[index].calibration_id,
                                              &calibration) !=
                LARDON3D_PROJECT_DB_OK ||
            std::memcmp(calibration.scientific_hash,
                        page[index].calibration_hash, 32) != 0)
          return false;
        calibrations->emplace(
            page[index].image_id,
            Lardon3DSparseGeometryCalibration{
                calibration.width, calibration.height, calibration.fx,
                calibration.fy, calibration.cx, calibration.cy, calibration.k1,
                calibration.k2, calibration.p1, calibration.p2});
      }
    }
    if (count == 0) break;
    if (next <= after) return false;
    after = next;
  }
  return members->size() == scope.member_count;
}

bool derive_shape(Context *context) {
  if (!context) return false;
  Lardon3DSparseReconstruction base{};
  if (lardon3d_sparse_reconstruction_load(
          context->database, context->durable.base_reconstruction_id, &base) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  Lardon3DProjectDbTrackSet track_set{};
  if (lardon3d_project_db_load_track_set(
          context->database, context->durable.extension_track_set_id,
          &track_set) != LARDON3D_PROJECT_DB_OK)
    return false;
  std::map<uint64_t, Lardon3DSparseCalibrationMember> members;
  if (!load_scope(context->database, context->durable.calibration_scope_id,
                  &members, nullptr))
    return false;
  uint64_t observations = 0;
  uint64_t tracks_seen = 0;
  uint64_t after = 0;
  for (;;) {
    Lardon3DProjectDbTrack tracks[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    if (lardon3d_project_db_list_tracks(
            context->database, track_set.track_set_id, after, tracks,
            LARDON3D_SPARSE_SFM_PAGE_MAX, &count) != LARDON3D_PROJECT_DB_OK)
      return false;
    bool valid = true;
    for (size_t index = 0; index < count; ++index) {
      if (tracks[index].track_id <= after ||
          !checked_add(observations, tracks[index].observation_count,
                       &observations))
        valid = false;
      after = tracks[index].track_id;
      ++tracks_seen;
      lardon3d_project_db_free_track(&tracks[index]);
    }
    if (!valid) return false;
    if (count == 0) break;
  }
  if (tracks_seen != track_set.track_count || observations == 0) return false;
  context->shape = {base.registered_image_count, base.landmark_count,
                    0, members.size(), tracks_seen, observations};
  uint64_t landmark_after = 0;
  for (;;) {
    Lardon3DSparseLandmark page[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    Lardon3DSparseLandmarkPage list{landmark_after,
                                    LARDON3D_SPARSE_SFM_PAGE_MAX, 0, 0, page};
    if (lardon3d_sparse_landmark_list(
            context->database, base.reconstruction_id, landmark_after,
            LARDON3D_SPARSE_SFM_PAGE_MAX, &list) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < list.count; ++index)
      if (!checked_add(context->shape.base_observation_count,
                       page[index].observation_count,
                       &context->shape.base_observation_count))
        return false;
    if (list.count == 0) break;
    if (list.next_track_id <= landmark_after) return false;
    landmark_after = list.next_track_id;
  }
  return true;
}

void close_readers(std::map<uint64_t, Reader> *readers) {
  for (auto &[id, reader] : *readers) {
    (void)id;
    if (reader.reader) lardon3d_feature_reader_close(reader.reader);
  }
  readers->clear();
}

bool resolve_observation(Context *context,
                         const Lardon3DProjectDbTrackObservation &source,
                         std::map<uint64_t, Reader> *readers, uint64_t track_id,
                         Lardon3DSparseIncrementalObservation *output) {
  auto found = readers->find(source.feature_set_id);
  if (found == readers->end()) {
    Reader reader;
    if (lardon3d_project_db_load_feature_set(
            context->database, source.feature_set_id, &reader.feature_set) !=
            LARDON3D_PROJECT_DB_OK ||
        lardon3d_feature_reader_open(context->project_path.c_str(),
                                     &reader.feature_set, &reader.reader,
                                     &reader.metadata) != LARDON3D_FEATURE_STORE_OK ||
        reader.metadata.feature_count != reader.feature_set.feature_count) {
      if (reader.reader) lardon3d_feature_reader_close(reader.reader);
      return false;
    }
    found = readers->emplace(source.feature_set_id, reader).first;
  }
  Lardon3DFeatureKeypoint keypoint{};
  if (source.feature_index >= found->second.metadata.feature_count ||
      lardon3d_feature_reader_keypoints(found->second.reader, source.feature_index,
                                        &keypoint, 1) != LARDON3D_FEATURE_STORE_OK ||
      !std::isfinite(keypoint.x) || !std::isfinite(keypoint.y))
    return false;
  *output = {track_id, found->second.feature_set.image_id, source.feature_set_id,
             source.feature_index, found->second.metadata.feature_count,
             static_cast<double>(keypoint.x), static_cast<double>(keypoint.y)};
  return true;
}

bool load_base_snapshot(Context *context, Resolved *resolved,
                        std::map<uint64_t, Reader> *readers) {
  if (lardon3d_sparse_reconstruction_load(
          context->database, context->durable.base_reconstruction_id,
          &resolved->base_record) != LARDON3D_PROJECT_DB_OK)
    return false;
  uint64_t after = 0;
  for (;;) {
    Lardon3DSparseComponent page[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    Lardon3DSparseComponentPage list{after, LARDON3D_SPARSE_SFM_PAGE_MAX,
                                     0, 0, page};
    if (lardon3d_sparse_component_list(
            context->database, resolved->base_record.reconstruction_id, after,
            LARDON3D_SPARSE_SFM_PAGE_MAX, &list) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < list.count; ++index)
      resolved->base_components.push_back(
          {page[index].component_key, page[index].registered_image_count,
           page[index].registered_image_count, page[index].landmark_count});
    if (list.count == 0) break;
    if (list.next_component_key <= after) return false;
    after = list.next_component_key;
  }
  after = 0;
  for (;;) {
    Lardon3DSparseRegisteredImage page[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    Lardon3DSparseRegisteredImagePage list{after, LARDON3D_SPARSE_SFM_PAGE_MAX,
                                           0, 0, page};
    if (lardon3d_sparse_registered_image_list(
            context->database, resolved->base_record.reconstruction_id, after,
            LARDON3D_SPARSE_SFM_PAGE_MAX, &list) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < list.count; ++index) {
      Lardon3DSparseGeometryPose pose{};
      std::memcpy(pose.rotation_cw, page[index].rotation_cw,
                  sizeof(pose.rotation_cw));
      std::memcpy(pose.translation_cw, page[index].translation_cw,
                  sizeof(pose.translation_cw));
      resolved->base_cameras.push_back(
          {page[index].image_id, page[index].component_key, pose});
    }
    if (list.count == 0) break;
    if (list.next_image_id <= after) return false;
    after = list.next_image_id;
  }
  after = 0;
  for (;;) {
    Lardon3DSparseLandmark page[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    Lardon3DSparseLandmarkPage list{after, LARDON3D_SPARSE_SFM_PAGE_MAX,
                                    0, 0, page};
    if (lardon3d_sparse_landmark_list(
            context->database, resolved->base_record.reconstruction_id, after,
            LARDON3D_SPARSE_SFM_PAGE_MAX, &list) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < list.count; ++index) {
      resolved->base_landmarks.push_back(
          {page[index].landmark_id, page[index].track_id,
           page[index].component_key, {page[index].x, page[index].y, page[index].z},
           page[index].reprojection_rmse_px,
           page[index].reprojection_median_px, page[index].observation_count});
      Lardon3DProjectDbTrack track{};
      if (lardon3d_project_db_load_track(context->database, page[index].track_id,
                                         &track) != LARDON3D_PROJECT_DB_OK)
        return false;
      bool valid = true;
      for (uint32_t position = 0; position < track.observation_count; ++position) {
        Lardon3DSparseIncrementalObservation observation{};
        if (!resolve_observation(context, track.observations[position], readers,
                                 track.track_id, &observation)) {
          valid = false;
          break;
        }
        resolved->base_track_observations.push_back(observation);
      }
      lardon3d_project_db_free_track(&track);
      if (!valid) return false;
    }
    if (list.count == 0) break;
    if (list.next_track_id <= after) return false;
    after = list.next_track_id;
  }
  uint64_t after_landmark = 0;
  uint32_t after_position = 0;
  for (;;) {
    Lardon3DSparseLandmarkObservation page[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    Lardon3DSparseObservationPage list{after_landmark, after_position,
                                       LARDON3D_SPARSE_SFM_PAGE_MAX, 0, 0, 0,
                                       page};
    if (lardon3d_sparse_observation_list(
            context->database, resolved->base_record.reconstruction_id,
            after_landmark, after_position, LARDON3D_SPARSE_SFM_PAGE_MAX,
            &list) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < list.count; ++index) {
      auto reader = readers->find(page[index].feature_set_id);
      if (reader == readers->end()) {
        Reader loaded;
        if (lardon3d_project_db_load_feature_set(
                context->database, page[index].feature_set_id,
                &loaded.feature_set) != LARDON3D_PROJECT_DB_OK)
          return false;
        reader = readers->emplace(page[index].feature_set_id, loaded).first;
      }
      resolved->base_observations.push_back(
          {page[index].landmark_id, page[index].track_id,
           reader->second.feature_set.image_id,
           page[index].feature_set_id, page[index].feature_index,
           page[index].position_in_track});
    }
    if (list.count == 0) break;
    if (list.next_landmark_id < after_landmark ||
        (list.next_landmark_id == after_landmark &&
         list.next_position_in_track <= after_position))
      return false;
    after_landmark = list.next_landmark_id;
    after_position = list.next_position_in_track;
  }
  resolved->base.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
  resolved->base.track_set_id = resolved->base_record.track_set_id;
  resolved->base.calibration_scope_id = resolved->base_record.calibration_scope_id;
  resolved->base.components = resolved->base_components.data();
  resolved->base.component_count = resolved->base_components.size();
  resolved->base.cameras = resolved->base_cameras.data();
  resolved->base.camera_count = resolved->base_cameras.size();
  resolved->base.landmarks = resolved->base_landmarks.data();
  resolved->base.landmark_count = resolved->base_landmarks.size();
  resolved->base.observations = resolved->base_observations.data();
  resolved->base.observation_count = resolved->base_observations.size();
  return resolved->base.component_count == resolved->base_record.component_count &&
         resolved->base.camera_count == resolved->base_record.registered_image_count &&
         resolved->base.landmark_count == resolved->base_record.landmark_count;
}

bool resolve_all(Context *context, Resolved *resolved) {
  std::map<uint64_t, Reader> readers;
  bool valid = load_base_snapshot(context, resolved, &readers);
  std::map<uint64_t, Lardon3DSparseCalibrationMember> base_scope;
  std::map<uint64_t, Lardon3DSparseCalibrationMember> extension_scope;
  if (valid)
    valid = load_scope(context->database, resolved->base_record.calibration_scope_id,
                       &base_scope, nullptr) &&
            load_scope(context->database, context->durable.calibration_scope_id,
                       &extension_scope, &resolved->calibrations);
  for (const auto &camera : resolved->base_cameras) {
    auto historical = base_scope.find(camera.image_id);
    auto extension = extension_scope.find(camera.image_id);
    if (!valid || historical == base_scope.end() || extension == extension_scope.end() ||
        std::memcmp(historical->second.calibration_hash,
                    extension->second.calibration_hash, 32) != 0) {
      valid = false;
      break;
    }
  }
  Lardon3DProjectDbTrackSet track_set{};
  if (valid)
    valid = lardon3d_project_db_load_track_set(
                context->database, context->durable.extension_track_set_id,
                &track_set) == LARDON3D_PROJECT_DB_OK;
  uint64_t after = 0;
  uint64_t tracks_seen = 0;
  std::set<uint64_t> participating_images;
  while (valid) {
    Lardon3DProjectDbTrack tracks[LARDON3D_SPARSE_SFM_PAGE_MAX]{};
    size_t count = 0;
    if (lardon3d_project_db_list_tracks(
            context->database, track_set.track_set_id, after, tracks,
            LARDON3D_SPARSE_SFM_PAGE_MAX, &count) != LARDON3D_PROJECT_DB_OK) {
      valid = false;
      break;
    }
    for (size_t index = 0; valid && index < count; ++index) {
      if (tracks[index].track_id <= after) {
        valid = false;
        break;
      }
      for (uint32_t position = 0;
           valid && position < tracks[index].observation_count; ++position) {
        Lardon3DSparseIncrementalObservation observation{};
        valid = resolve_observation(context, tracks[index].observations[position],
                                    &readers, tracks[index].track_id, &observation) &&
                resolved->calibrations.count(observation.image_id) != 0;
        if (valid) {
          participating_images.insert(observation.image_id);
          resolved->extension_observations.push_back(observation);
        }
      }
      after = tracks[index].track_id;
      ++tracks_seen;
    }
    for (size_t index = 0; index < count; ++index)
      lardon3d_project_db_free_track(&tracks[index]);
    if (!valid || count == 0) break;
  }
  for (const auto &camera : resolved->base_cameras)
    participating_images.insert(camera.image_id);
  for (uint64_t image_id : participating_images) {
    auto calibration = resolved->calibrations.find(image_id);
    if (calibration == resolved->calibrations.end()) {
      valid = false;
      break;
    }
    resolved->extension_images.push_back({image_id, calibration->second});
  }
  close_readers(&readers);
  return valid && tracks_seen == track_set.track_count &&
         !resolved->extension_observations.empty();
}

bool persist(Context *context, const Lardon3DTask *task) {
  Lardon3DTaskDurableSnapshot snapshot{};
  if (!lardon3d_task_durable_snapshot(task, &snapshot)) return false;
  Lardon3DProjectDbCheckpoint checkpoint_record{};
  std::snprintf(checkpoint_record.path, sizeof(checkpoint_record.path),
                ".lardon3d/checkpoints/%llu.chk",
                static_cast<unsigned long long>(snapshot.id));
  checkpoint_record.format_version = LARDON3D_TASK_CHECKPOINT_VERSION;
  std::string path = context->project_path + "/" + checkpoint_record.path;
  auto saved = lardon3d_task_checkpoint_save(path.c_str(), &snapshot);
  if (saved != LARDON3D_TASK_CHECKPOINT_OK &&
      saved != LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE)
    return false;
  checkpoint_record.durability =
      saved == LARDON3D_TASK_CHECKPOINT_OK
          ? LARDON3D_DB_CHECKPOINT_DURABLE
          : LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE;
  return lardon3d_project_db_record_incremental_reconstruction_task(
             context->database, &snapshot,
             LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND,
             LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND_VERSION,
             &checkpoint_record, &context->durable, 0) == LARDON3D_PROJECT_DB_OK;
}

bool task_checkpoint(void *userdata) {
  return lardon3d_task_checkpoint(static_cast<Lardon3DTask *>(userdata));
}

bool build_publication(const Resolved &resolved,
                       const Lardon3DSparseIncrementalResult &snapshot,
                       const unsigned char parameter_fingerprint[32],
                       std::vector<Lardon3DSparseComponent> *components,
                       std::vector<Lardon3DSparseRegisteredImage> *cameras,
                       std::vector<Lardon3DSparseLandmark> *landmarks,
                       std::vector<Lardon3DSparseLandmarkObservation> *observations,
                       Lardon3DSparsePublication *publication) {
  std::map<uint64_t, const Lardon3DSparseIncrementalCamera *> cameras_by_image;
  std::map<uint64_t, const Lardon3DSparseIncrementalLandmark *> landmarks_by_track;
  std::map<std::pair<uint64_t, uint32_t>,
           const Lardon3DSparseIncrementalObservation *>
      source_observations;
  std::vector<double> squared_errors;
  for (size_t index = 0; index < snapshot.camera_count; ++index)
    cameras_by_image.emplace(snapshot.cameras[index].image_id,
                             &snapshot.cameras[index]);
  for (size_t index = 0; index < snapshot.landmark_count; ++index)
    landmarks_by_track.emplace(snapshot.landmarks[index].track_id,
                               &snapshot.landmarks[index]);
  for (const auto &source : resolved.extension_observations)
    if (!source_observations
             .emplace(std::make_pair(source.feature_set_id,
                                     source.feature_index),
                      &source)
             .second)
      return false;
  for (size_t index = 0; index < snapshot.component_count; ++index)
    components->push_back({0, snapshot.components[index].component_key,
                           snapshot.components[index].registered_image_count,
                           snapshot.components[index].landmark_count});
  for (size_t index = 0; index < snapshot.camera_count; ++index) {
    Lardon3DSparseRegisteredImage camera{};
    camera.image_id = snapshot.cameras[index].image_id;
    camera.component_key = snapshot.cameras[index].component_key;
    std::memcpy(camera.rotation_cw, snapshot.cameras[index].pose_cw.rotation_cw,
                sizeof(camera.rotation_cw));
    std::memcpy(camera.translation_cw,
                snapshot.cameras[index].pose_cw.translation_cw,
                sizeof(camera.translation_cw));
    cameras->push_back(camera);
  }
  for (size_t index = 0; index < snapshot.landmark_count; ++index) {
    const auto &source = snapshot.landmarks[index];
    landmarks->push_back({0, source.track_id, source.component_key, source.point.x,
                          source.point.y, source.point.z,
                          source.reprojection_rmse_px,
                          source.reprojection_median_px,
                          source.observation_count});
  }
  for (size_t index = 0; index < snapshot.observation_count; ++index) {
    const auto &source = snapshot.observations[index];
    auto camera = cameras_by_image.find(source.image_id);
    auto landmark = landmarks_by_track.find(source.track_id);
    auto calibration = resolved.calibrations.find(source.image_id);
    auto observed = source_observations.find(
        std::make_pair(source.feature_set_id, source.feature_index));
    if (camera == cameras_by_image.end() ||
        landmark == landmarks_by_track.end() ||
        calibration == resolved.calibrations.end() ||
        observed == source_observations.end() ||
        observed->second->track_id != source.track_id ||
        observed->second->image_id != source.image_id)
      return false;
    Lardon3DSparseGeometryPoint2 pixel{observed->second->x,
                                       observed->second->y};
    double squared_error = 0.0;
    if (!lardon3d_sparse_sfm_squared_reprojection_error(
            &calibration->second, &camera->second->pose_cw,
            &landmark->second->point, &pixel, &squared_error))
      return false;
    squared_errors.push_back(squared_error);
    observations->push_back({0, source.track_id, source.feature_set_id,
                             source.feature_index, source.position_in_track});
  }
  *publication = {resolved.base.track_set_id, 0, 0, 0, {}, nullptr, 0,
                  nullptr, 0, nullptr, 0, nullptr, 0, 0.0, 0.0, 0};
  publication->track_set_id = snapshot.track_set_id;
  publication->calibration_scope_id = snapshot.calibration_scope_id;
  publication->sfm_kind = LARDON3D_SPARSE_SFM_KIND_INCREMENTAL;
  publication->sfm_version = LARDON3D_SPARSE_SFM_VERSION;
  std::memcpy(publication->parameter_fingerprint, parameter_fingerprint, 32);
  publication->components = components->data();
  publication->component_count = components->size();
  publication->registered_images = cameras->data();
  publication->registered_image_count = cameras->size();
  publication->landmarks = landmarks->data();
  publication->landmark_count = landmarks->size();
  publication->observations = observations->data();
  publication->observation_count = observations->size();
  if (!lardon3d_sparse_sfm_publication_metrics(
          squared_errors.data(), squared_errors.size(),
          &publication->reprojection_rmse_px,
          &publication->reprojection_median_px))
    return false;
  publication->created_at = static_cast<int64_t>(std::time(nullptr));
  return true;
}

bool execute(Context *context, Lardon3DTask *task, bool *reused, bool *no_change) {
  Lardon3DIncrementalReconstructionIdentity identity{
      context->durable.base_reconstruction_id,
      context->durable.extension_track_set_id,
      context->durable.calibration_scope_id,
      context->durable.incremental_kind,
      context->durable.incremental_version,
      {}};
  std::memcpy(identity.parameter_fingerprint,
              context->durable.parameter_fingerprint, 32);
  unsigned char digest[32];
  if (!lardon3d_incremental_reconstruction_identity_digest(&identity, digest))
    return false;
  Lardon3DIncrementalReconstructionMetadata existing_metadata{};
  Lardon3DSparseReconstruction existing{};
  auto lookup = lardon3d_incremental_reconstruction_find_exact(
      context->database, digest, &existing_metadata, &existing);
  if (lookup == LARDON3D_PROJECT_DB_OK) {
    *reused = true;
    return true;
  }
  if (lookup != LARDON3D_PROJECT_DB_NOT_FOUND || !lardon3d_task_checkpoint(task))
    return false;
  Resolved resolved;
  if (!resolve_all(context, &resolved)) return false;
  Lardon3DSparseIncrementalParameters parameters{};
  if (!lardon3d_sparse_incremental_parameters_default(&parameters)) return false;
  Lardon3DSparseIncrementalInput extension{
      context->durable.extension_track_set_id,
      context->durable.calibration_scope_id,
      resolved.extension_images.data(), resolved.extension_images.size(),
      resolved.extension_observations.data(), resolved.extension_observations.size()};
  Lardon3DIncrementalReconstructionInput input{
      context->durable.base_reconstruction_id, &resolved.base,
      resolved.base_track_observations.data(),
      resolved.base_track_observations.size(), &extension, &parameters,
      task_checkpoint, task};
  Lardon3DIncrementalReconstructionResult result{};
  auto status = lardon3d_incremental_reconstruction_run(&input, &result);
  if (status == LARDON3D_INCREMENTAL_RECONSTRUCTION_NO_CHANGE) {
    *no_change = true;
    return true;
  }
  if (status != LARDON3D_INCREMENTAL_RECONSTRUCTION_OK) {
    lardon3d_incremental_reconstruction_result_destroy(&result);
    return false;
  }
  std::vector<Lardon3DSparseComponent> components;
  std::vector<Lardon3DSparseRegisteredImage> cameras;
  std::vector<Lardon3DSparseLandmark> landmarks;
  std::vector<Lardon3DSparseLandmarkObservation> observations;
  Lardon3DSparsePublication publication{};
  if (!build_publication(resolved, result.snapshot,
                         context->durable.parameter_fingerprint,
                         &components, &cameras,
                         &landmarks, &observations, &publication)) {
    lardon3d_incremental_reconstruction_result_destroy(&result);
    return false;
  }
  Lardon3DIncrementalReconstructionMetadata metadata{
      0, context->durable.base_reconstruction_id,
      context->durable.extension_track_set_id,
      context->durable.calibration_scope_id,
      context->durable.incremental_kind,
      context->durable.incremental_version,
      {}, {}};
  std::memcpy(metadata.parameter_fingerprint,
              context->durable.parameter_fingerprint, 32);
  std::memcpy(metadata.scientific_identity, digest, 32);
  Lardon3DSparseReconstruction published{};
  auto published_status = lardon3d_incremental_reconstruction_publish(
      context->database, &publication, &metadata, &published);
  lardon3d_incremental_reconstruction_result_destroy(&result);
  if (published_status == LARDON3D_PROJECT_DB_OK) return true;
  if (published_status != LARDON3D_PROJECT_DB_CONSTRAINT) return false;
  lookup = lardon3d_incremental_reconstruction_find_exact(
      context->database, digest, &existing_metadata, &existing);
  *reused = lookup == LARDON3D_PROJECT_DB_OK;
  return *reused;
}

bool run(Lardon3DTask *task, void *userdata) {
  try {
    bool reused = false;
    bool no_change = false;
    if (!execute(static_cast<Context *>(userdata), task, &reused, &no_change))
      return lardon3d_task_fail(task,
                                "Reconstruction incrémentale non publiable.");
    const char *message = reused ? "Reconstruction incrémentale réutilisée."
                          : no_change ? "Extension sans changement scientifique."
                                      : "Reconstruction incrémentale publiée.";
    return lardon3d_task_set_progress(task, 100, message);
  } catch (const std::bad_alloc &) {
    return lardon3d_task_fail(task,
                              "Mémoire insuffisante pour la phase H.");
  } catch (...) {
    return lardon3d_task_fail(task, "Erreur interne de la phase H.");
  }
}

void finished(const Lardon3DTask *task, void *userdata) {
  try {
    (void)persist(static_cast<Context *>(userdata), task);
  } catch (...) {
  }
}
} // namespace

extern "C" bool lardon3d_incremental_reconstruction_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  try {
    if (!snapshot || !userdata || !binding) return false;
    auto *runtime = static_cast<Lardon3DTaskReconstructionContext *>(userdata);
    Lardon3DProjectDbIncrementalReconstructionTask durable{};
    if (lardon3d_project_db_load_incremental_reconstruction_task(
            runtime->project_db, snapshot->id, &durable) != LARDON3D_PROJECT_DB_OK)
      return false;
    if (!valid_durable(durable)) return false;
    auto *context = new (std::nothrow) Context;
    if (!context) return false;
    context->project_path = runtime->project_path;
    context->database = runtime->project_db;
    context->durable = durable;
    if (!derive_shape(context)) {
      delete context;
      return false;
    }
    *binding = {run, context, destroy_context, finished, context};
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" Lardon3DTask *lardon3d_project_create_incremental_reconstruction_task(
    Lardon3DAppState *state,
    const Lardon3DIncrementalReconstructionTaskConfiguration *configuration,
    uint64_t *task_id) {
  try {
    if (task_id) *task_id = 0;
    if (!state || !state->project_loaded || !state->project_db ||
        !state->resource_governor || !configuration ||
        configuration->base_reconstruction_id == 0 ||
        configuration->extension_track_set_id == 0 ||
        configuration->calibration_scope_id == 0)
      return nullptr;
    auto *context = new (std::nothrow) Context;
    if (!context) return nullptr;
    context->project_path = state->project_path;
    context->database = state->project_db;
    context->durable.base_reconstruction_id =
        configuration->base_reconstruction_id;
    context->durable.extension_track_set_id =
        configuration->extension_track_set_id;
    context->durable.calibration_scope_id = configuration->calibration_scope_id;
    context->durable.incremental_kind =
        LARDON3D_INCREMENTAL_RECONSTRUCTION_KIND;
    context->durable.incremental_version =
        LARDON3D_INCREMENTAL_RECONSTRUCTION_VERSION;
    if (!lardon3d_incremental_reconstruction_parameter_fingerprint(
            context->durable.parameter_fingerprint) || !derive_shape(context)) {
      delete context;
      return nullptr;
    }
    Lardon3DResourceEstimate estimate{};
    if (!lardon3d_incremental_reconstruction_resource_estimate(&context->shape,
                                                               &estimate)) {
      delete context;
      return nullptr;
    }
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id) !=
        LARDON3D_PROJECT_DB_OK) {
      delete context;
      return nullptr;
    }
    context->durable.task_id = id;
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Reconstruction incrémentale", &estimate,
        LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND,
        LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND_VERSION, run, context,
        destroy_context);
    if (!task || !lardon3d_task_assign_id(task, id) ||
        !lardon3d_task_set_finished_callback(task, finished, context) ||
        !persist(context, task)) {
      if (task)
        lardon3d_task_destroy(task);
      else
        delete context;
      return nullptr;
    }
    if (task_id) *task_id = id;
    return task;
  } catch (...) {
    if (task_id) *task_id = 0;
    return nullptr;
  }
}

extern "C" bool lardon3d_project_enqueue_incremental_reconstruction_task(
    Lardon3DAppState *state,
    const Lardon3DIncrementalReconstructionTaskConfiguration *configuration,
    uint64_t *task_id) {
  Lardon3DTask *task = lardon3d_project_create_incremental_reconstruction_task(
      state, configuration, task_id);
  if (!task || !lardon3d_task_queue_add(state->task_queue, task, nullptr)) {
    if (task) lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
