#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sched.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/app_state.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/incremental_reconstruction.h>
#include <lardon3d/incremental_reconstruction_task.h>
#include <lardon3d/project_db.h>
#include <lardon3d/resource_governor.h>
#include <lardon3d/sparse_sfm_model.h>
#include <lardon3d/sparse_sfm_task.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_kind_registry.h>
#include <lardon3d/task_queue.h>
}
#include "../src/sparse_sfm_gate_f_internal.h"

extern "C" int lardon3d_sparse_sfm_task_test_observation_lookup(
    const Lardon3DSparseIncrementalObservation *observations, size_t count,
    uint64_t track_id, uint64_t image_id, uint64_t feature_set_id,
    uint32_t feature_index, size_t *resolved_index);
extern "C" bool lardon3d_sparse_sfm_task_test_project_landmarks(
    const Lardon3DSparseIncrementalLandmark *landmarks, size_t landmark_count,
    const uint64_t *component_keys, size_t component_count,
    Lardon3DSparseLandmark *output, size_t output_capacity, size_t *output_count);

namespace {
#define CHECK(value)                                                            \
  do {                                                                          \
    if (!(value)) {                                                             \
      std::fprintf(stderr, "sparse-sfm-task:%d: %s\n", __LINE__, #value);      \
      return false;                                                             \
    }                                                                           \
  } while (0)

constexpr size_t kBaseImageCount = 3;
constexpr size_t kImageCount = 4;
constexpr size_t kTrackCount = 12;

struct Fixture {
  std::string root;
  std::string database_path;
  Lardon3DAppState state{};
  Lardon3DProjectDbScanSet scanset{};
  Lardon3DProjectDbImage images[kImageCount]{};
  Lardon3DProjectDbFeatureSet feature_sets[kImageCount]{};
  Lardon3DProjectDbTrackSet base_track_set{};
  Lardon3DProjectDbTrackSet track_set{};
  Lardon3DSparseCalibrationScope scope{};
  Lardon3DSparseIncrementalParameters parameters{};
};

Lardon3DResourcePolicy policy() {
  return {0, 0, 0, 0, 1.0, 100.0, 100.0, 100.0, 0, 1};
}

bool wait_state(Lardon3DTaskQueue *queue, uint64_t id, Lardon3DTaskState wanted) {
  timespec start{};
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return false;
  const int64_t deadline = start.tv_sec * INT64_C(1000000000) + start.tv_nsec +
                           INT64_C(10000000000);
  for (;;) {
    Lardon3DTaskSnapshot snapshot{};
    if (lardon3d_task_queue_get(queue, id, &snapshot) && snapshot.state == wanted) return true;
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec * INT64_C(1000000000) + now.tv_nsec >= deadline)
      return false;
    sched_yield();
  }
}

bool wait_reservations_released(Lardon3DResourceGovernor *governor) {
  timespec start{};
  if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return false;
  const int64_t deadline = start.tv_sec * INT64_C(1000000000) + start.tv_nsec +
                           INT64_C(10000000000);
  while (lardon3d_resource_governor_reservation_count(governor) != 0) {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec * INT64_C(1000000000) + now.tv_nsec >= deadline)
      return false;
    sched_yield();
  }
  return true;
}

Lardon3DSparseGeometryPoint2 project(const Lardon3DSparseGeometryCalibration &calibration,
                                     size_t camera,
                                     const Lardon3DSparseGeometryPoint3 &point) {
  double x = point.x + static_cast<double>(camera);
  return {calibration.fx * x / point.z + calibration.cx,
          calibration.fy * point.y / point.z + calibration.cy};
}

std::string asset_path(const unsigned char hash[32], const char *kind) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex(64, '0');
  for (size_t index = 0; index < 32; ++index) {
    hex[2 * index] = digits[hash[index] >> 4];
    hex[2 * index + 1] = digits[hash[index] & 15U];
  }
  return std::string("assets/") + kind + "/" + hex.substr(0, 2) + "/" + hex;
}

bool initialize_runtime(Fixture *fixture) {
  lardon3d_app_state_init(&fixture->state);
  fixture->state.hardware_profile.logical_cpu_count = 16;
  fixture->state.hardware_profile.page_size_bytes = 4096;
  fixture->state.hardware_profile.memory_total_bytes = 64ULL * 1024 * 1024 * 1024;
  std::snprintf(fixture->state.hardware_profile.cpu_architecture,
                sizeof(fixture->state.hardware_profile.cpu_architecture), "test");
  auto resource_policy = policy();
  fixture->state.resource_governor =
      lardon3d_resource_governor_create(&fixture->state.hardware_profile, &resource_policy);
  fixture->state.task_queue = fixture->state.resource_governor
                                  ? lardon3d_task_queue_create(
                                        fixture->state.resource_governor, 8)
                                  : nullptr;
  return fixture->state.task_queue != nullptr;
}

bool create_fixture(Fixture *fixture) {
  char root[] = "/tmp/lardon3d-sparse-task-XXXXXX";
  CHECK(mkdtemp(root) != nullptr);
  fixture->root = root;
  fixture->database_path = fixture->root + "/project.db";
  CHECK(mkdir((fixture->root + "/.lardon3d").c_str(), 0700) == 0);
  CHECK(mkdir((fixture->root + "/.lardon3d/checkpoints").c_str(), 0700) == 0);
  CHECK(initialize_runtime(fixture));
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  CHECK(lardon3d_project_db_open(fixture->database_path.c_str(),
                                 &fixture->state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  fixture->state.project_loaded = true;
  CHECK(std::snprintf(fixture->state.project_path, sizeof(fixture->state.project_path), "%s",
                      fixture->root.c_str()) > 0);
  CHECK(lardon3d_project_db_create_scanset(fixture->state.project_db, "sfm-task",
                                           &fixture->scanset) == LARDON3D_PROJECT_DB_OK);

  const Lardon3DSparseGeometryCalibration geometry = {
      4000, 3000, 2000.0, 2000.0, 2000.0, 1500.0, 0.0, 0.0, 0.0, 0.0};
  std::vector<Lardon3DSparseGeometryPoint3> points;
  for (size_t track = 0; track < kTrackCount; ++track)
    points.push_back({-1.0 + static_cast<double>(track % 4) * 0.5,
                      -0.8 + static_cast<double>(track / 4) * 0.4,
                      5.0 + static_cast<double>(track % 3) * 0.25});

  unsigned char feature_fingerprint[32]{};
  for (size_t camera = 0; camera < kImageCount; ++camera) {
    unsigned char image_hash[32]{};
    image_hash[0] = static_cast<unsigned char>(camera + 1);
    std::string relative = asset_path(image_hash, "images");
    Lardon3DProjectDbImageRegisterStatus status{};
    CHECK(lardon3d_project_db_register_image(
              fixture->state.project_db, fixture->scanset.scanset_id, image_hash,
              relative.c_str(), 1, "synthetic", relative.c_str(), 0, 1, &status,
              &fixture->images[camera]) ==
          LARDON3D_PROJECT_DB_OK);
    std::vector<Lardon3DFeatureKeypoint> keypoints(kTrackCount);
    std::vector<unsigned char> descriptors(kTrackCount * 32);
    for (size_t track = 0; track < kTrackCount; ++track) {
      auto pixel = project(geometry, camera, points[track]);
      keypoints[track].x = static_cast<float>(pixel.x);
      keypoints[track].y = static_cast<float>(pixel.y);
      if (camera == kImageCount - 1 && track == 0)
        keypoints[track].x += 0.25F;
      keypoints[track].size = 1.0F;
      std::memset(descriptors.data() + track * 32, static_cast<int>(track + camera), 32);
    }
    Lardon3DExtractedFeatures features{};
    features.image_width = 4000;
    features.image_height = 3000;
    features.feature_count = static_cast<uint32_t>(kTrackCount);
    features.keypoints = keypoints.data();
    features.descriptors = descriptors.data();
    features.descriptor_bytes = descriptors.size();
    CHECK(lardon3d_feature_store_publish_v2(
              &fixture->state, fixture->images[camera].image_id, 0, "test", 1,
              feature_fingerprint, LARDON3D_FEATURE_DESCRIPTOR_U8, 32, 0, &features,
              &fixture->feature_sets[camera]) == LARDON3D_FEATURE_STORE_OK);
  }

  Lardon3DSparseCalibration calibration{};
  calibration.model_kind = LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE;
  calibration.model_version = LARDON3D_SPARSE_SFM_CALIBRATION_VERSION;
  calibration.width = 4000;
  calibration.height = 3000;
  calibration.fx = 2000.0;
  calibration.fy = 2000.0;
  calibration.cx = 2000.0;
  calibration.cy = 1500.0;
  calibration.provenance_kind = LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT;
  Lardon3DSparseCalibration stored{};
  CHECK(lardon3d_sparse_calibration_create(fixture->state.project_db, &calibration, &stored) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationMember members[kImageCount]{};
  for (size_t camera = 0; camera < kImageCount; ++camera) {
    members[camera].image_id = fixture->images[camera].image_id;
    members[camera].calibration_id = stored.calibration_id;
    std::memcpy(members[camera].calibration_hash, stored.scientific_hash, 32);
  }
  CHECK(lardon3d_sparse_calibration_scope_create(fixture->state.project_db, members, kImageCount,
                                                 &fixture->scope) == LARDON3D_PROJECT_DB_OK);

  std::vector<Lardon3DProjectDbTrackObservation> observations(kTrackCount * kImageCount);
  std::vector<Lardon3DProjectDbTrack> tracks(kTrackCount);
  for (size_t track = 0; track < kTrackCount; ++track) {
    tracks[track].observation_count = kImageCount;
    tracks[track].observations = observations.data() + track * kImageCount;
    for (size_t camera = 0; camera < kImageCount; ++camera)
      tracks[track].observations[camera] = {
          fixture->feature_sets[camera].feature_set_id, static_cast<uint32_t>(track),
          static_cast<uint32_t>(camera)};
  }
  Lardon3DProjectDbTrackSet configuration{};
  std::snprintf(configuration.builder_kind, sizeof(configuration.builder_kind), "test");
  configuration.builder_version = 1;
  configuration.verifier_kind = 1;
  configuration.verifier_version = 1;
  configuration.gvr_count = 1;
  configuration.track_count = kTrackCount;
  CHECK(lardon3d_project_db_create_track_set(fixture->state.project_db, &configuration,
                                             tracks.data(), tracks.size(),
                                             &fixture->track_set) == LARDON3D_PROJECT_DB_OK);
  std::vector<Lardon3DProjectDbTrackObservation> base_observations(
      kTrackCount * kBaseImageCount);
  std::vector<Lardon3DProjectDbTrack> base_tracks(kTrackCount);
  for (size_t track = 0; track < kTrackCount; ++track) {
    base_tracks[track].observation_count = kBaseImageCount;
    base_tracks[track].observations =
        base_observations.data() + track * kBaseImageCount;
    for (size_t camera = 0; camera < kBaseImageCount; ++camera)
      base_tracks[track].observations[camera] = {
          fixture->feature_sets[camera].feature_set_id,
          static_cast<uint32_t>(track), static_cast<uint32_t>(camera)};
  }
  configuration.parameter_fingerprint[0] = 1;
  CHECK(lardon3d_project_db_create_track_set(
            fixture->state.project_db, &configuration, base_tracks.data(),
            base_tracks.size(), &fixture->base_track_set) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_sparse_incremental_parameters_default(&fixture->parameters));
  fixture->parameters.minimum_seed_tracks = 6;
  fixture->parameters.minimum_seed_landmarks = 6;
  fixture->parameters.minimum_pnp_correspondences = 6;
  fixture->parameters.maximum_registration_rounds = 8;
  return true;
}

void destroy_runtime(Fixture *fixture) {
  if (fixture->state.task_queue) lardon3d_task_queue_destroy(fixture->state.task_queue);
  if (fixture->state.resource_governor)
    lardon3d_resource_governor_destroy(fixture->state.resource_governor);
  if (fixture->state.project_db) lardon3d_project_db_close(fixture->state.project_db);
  fixture->state = {};
}

Lardon3DSparseSfmTaskConfiguration configuration(Fixture *fixture) {
  return {fixture->base_track_set.track_set_id, fixture->scope.scope_id,
          fixture->parameters};
}

bool recompute_metrics(Fixture *fixture,
                       const Lardon3DSparseReconstruction &reconstruction,
                       double *rmse, double *median) {
  std::map<uint64_t, Lardon3DSparseRegisteredImage> cameras;
  std::map<uint64_t, Lardon3DSparseLandmark> landmarks;
  Lardon3DSparseRegisteredImage camera_items[64]{};
  Lardon3DSparseRegisteredImagePage camera_page{0, 64, 0, 0, camera_items};
  CHECK(lardon3d_sparse_registered_image_list(
            fixture->state.project_db, reconstruction.reconstruction_id, 0,
            64, &camera_page) == LARDON3D_PROJECT_DB_OK);
  for (size_t index = 0; index < camera_page.count; ++index)
    cameras.emplace(camera_items[index].image_id, camera_items[index]);
  Lardon3DSparseLandmark landmark_items[64]{};
  Lardon3DSparseLandmarkPage landmark_page{0, 64, 0, 0, landmark_items};
  CHECK(lardon3d_sparse_landmark_list(
            fixture->state.project_db, reconstruction.reconstruction_id, 0,
            64, &landmark_page) == LARDON3D_PROJECT_DB_OK);
  for (size_t index = 0; index < landmark_page.count; ++index)
    landmarks.emplace(landmark_items[index].track_id, landmark_items[index]);
  Lardon3DSparseLandmarkObservation observation_items[64]{};
  Lardon3DSparseObservationPage observation_page{0, 0, 64, 0, 0, 0,
                                                  observation_items};
  CHECK(lardon3d_sparse_observation_list(
            fixture->state.project_db, reconstruction.reconstruction_id, 0, 0,
            64, &observation_page) == LARDON3D_PROJECT_DB_OK);
  std::vector<double> errors;
  double sum = 0.0;
  const auto geometry = Lardon3DSparseGeometryCalibration{
      4000, 3000, 2000.0, 2000.0, 2000.0, 1500.0, 0.0, 0.0, 0.0, 0.0};
  for (size_t index = 0; index < observation_page.count; ++index) {
    const auto &observation = observation_items[index];
    auto landmark = landmarks.find(observation.track_id);
    const Lardon3DProjectDbFeatureSet *feature_set = nullptr;
    size_t camera_index = 0;
    for (; camera_index < kImageCount; ++camera_index)
      if (fixture->feature_sets[camera_index].feature_set_id ==
          observation.feature_set_id) {
        feature_set = &fixture->feature_sets[camera_index];
        break;
      }
    CHECK(landmark != landmarks.end() && feature_set != nullptr);
    auto camera = cameras.find(fixture->images[camera_index].image_id);
    CHECK(camera != cameras.end());
    Lardon3DFeatureReader *reader = nullptr;
    Lardon3DFeatureFileMetadata metadata{};
    CHECK(lardon3d_feature_reader_open(fixture->root.c_str(), feature_set,
                                       &reader, &metadata) ==
          LARDON3D_FEATURE_STORE_OK);
    Lardon3DFeatureKeypoint keypoint{};
    const auto read_status = lardon3d_feature_reader_keypoints(
        reader, observation.feature_index, &keypoint, 1);
    lardon3d_feature_reader_close(reader);
    CHECK(read_status == LARDON3D_FEATURE_STORE_OK);
    const double *r = camera->second.rotation_cw;
    const double *t = camera->second.translation_cw;
    const double x = r[0] * landmark->second.x + r[1] * landmark->second.y +
                     r[2] * landmark->second.z + t[0];
    const double y = r[3] * landmark->second.x + r[4] * landmark->second.y +
                     r[5] * landmark->second.z + t[1];
    const double z = r[6] * landmark->second.x + r[7] * landmark->second.y +
                     r[8] * landmark->second.z + t[2];
    CHECK(std::isfinite(z) && z > 1e-9);
    const double dx = geometry.fx * x / z + geometry.cx - keypoint.x;
    const double dy = geometry.fy * y / z + geometry.cy - keypoint.y;
    const double squared = dx * dx + dy * dy;
    CHECK(std::isfinite(squared) && std::isfinite(sum + squared));
    sum += squared;
    errors.push_back(std::sqrt(squared));
  }
  CHECK(!errors.empty());
  std::sort(errors.begin(), errors.end());
  *rmse = std::sqrt(sum / static_cast<double>(errors.size()));
  const size_t middle = errors.size() / 2;
  *median = errors.size() % 2
                ? errors[middle]
                : errors[middle - 1] +
                      (errors[middle] - errors[middle - 1]) / 2.0;
  return std::isfinite(*rmse) && std::isfinite(*median);
}

bool test_lookup() {
  Lardon3DSparseIncrementalObservation values[3] = {
      {1, 10, 100, 1, 8, 0, 0}, {2, 20, 100, 2, 8, 0, 0},
      {3, 30, 200, 1, 8, 0, 0}};
  size_t index = 99;
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 1, 10, 100, 1,
                                                         &index) == 1 && index == 0);
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 2, 20, 100, 2,
                                                         &index) == 1 && index == 1);
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 3, 30, 200, 1,
                                                         &index) == 1 && index == 2);
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 1, 10, 999, 1,
                                                         &index) == 0);
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 99, 10, 100, 1,
                                                         &index) == 0);
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 1, 99, 100, 1,
                                                         &index) == 0);
  values[2].feature_set_id = 100;
  CHECK(lardon3d_sparse_sfm_task_test_observation_lookup(values, 3, 1, 10, 100, 1,
                                                         &index) == -1);
  return true;
}

bool test_landmark_projection_order() {
  Lardon3DSparseIncrementalLandmark landmarks[5]{};
  const uint64_t component_keys[2] = {5, 10};
  const uint64_t input_components[5] = {10, 5, 99, 5, 10};
  const uint64_t input_tracks[5] = {2, 100, 3, 7, 1};
  for (size_t index = 0; index < 5; ++index) {
    landmarks[index].component_key = input_components[index];
    landmarks[index].track_id = input_tracks[index];
    landmarks[index].point.z = 1.0;
    landmarks[index].observation_count = 2;
  }
  Lardon3DSparseLandmark projected[5]{};
  size_t count = 0;
  CHECK(lardon3d_sparse_sfm_task_test_project_landmarks(
      landmarks, 5, component_keys, 2, projected, 5, &count));
  CHECK(count == 4);
  CHECK(projected[0].component_key == 5 && projected[0].track_id == 7);
  CHECK(projected[1].component_key == 5 && projected[1].track_id == 100);
  CHECK(projected[2].component_key == 10 && projected[2].track_id == 1);
  CHECK(projected[3].component_key == 10 && projected[3].track_id == 2);
  for (size_t index = 0; index < count; ++index)
    CHECK(projected[index].component_key != 99);
  return true;
}

bool test_task() {
  Fixture fixture;
  CHECK(create_fixture(&fixture));
  auto config = configuration(&fixture);
  uint64_t task_id = 0;
  Lardon3DTask *task = lardon3d_project_create_sparse_sfm_task(&fixture.state, &config, &task_id);
  CHECK(task && task_id != 0);
  char kind[LARDON3D_TASK_KIND_CAPACITY]{};
  uint32_t kind_version = 0;
  CHECK(lardon3d_task_kind(task, kind, &kind_version));
  CHECK(std::strcmp(kind, LARDON3D_SPARSE_SFM_TASK_KIND) == 0 && kind_version == 1);
  Lardon3DResourceEstimate estimate{};
  CHECK(lardon3d_task_resource_estimate(task, &estimate));
  CHECK(estimate.memory_fixed_bytes == 135266304 && estimate.desired_cpu_threads == 1 &&
        estimate.desired_io_slots == 1 && estimate.minimum_batch_size == 1 &&
        estimate.maximum_batch_size == 1);
  Lardon3DProjectDbSparseSfmTask durable{};
  CHECK(lardon3d_project_db_load_sparse_sfm_task(fixture.state.project_db, task_id, &durable) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_task_queue_add(fixture.state.task_queue, task, nullptr));
  CHECK(wait_state(fixture.state.task_queue, task_id, TASK_COMPLETED));

  unsigned char fingerprint[32]{};
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&fixture.parameters, fingerprint));
  Lardon3DSparseReconstruction reconstruction{};
  CHECK(lardon3d_sparse_reconstruction_find_exact(
            fixture.state.project_db, fixture.base_track_set.track_set_id,
            fixture.scope.scope_id, 1, 1,
            fingerprint, &reconstruction) == LARDON3D_PROJECT_DB_OK);
  CHECK(reconstruction.component_count == 1 && reconstruction.registered_image_count == 3 &&
        reconstruction.landmark_count >= 6 &&
        std::isfinite(reconstruction.reprojection_rmse_px) &&
        std::isfinite(reconstruction.reprojection_median_px));
  Lardon3DSparseLandmarkObservation persisted_observations[64]{};
  Lardon3DSparseObservationPage observation_page{};
  observation_page.items = persisted_observations;
  CHECK(lardon3d_sparse_observation_list(fixture.state.project_db,
                                         reconstruction.reconstruction_id, 0, 0, 64,
                                         &observation_page) == LARDON3D_PROJECT_DB_OK);
  CHECK(observation_page.count >= 18);

  Lardon3DIncrementalReconstructionTaskConfiguration h_configuration{
      reconstruction.reconstruction_id, fixture.track_set.track_set_id,
      fixture.scope.scope_id};
  uint64_t h_task_id = 0;
  Lardon3DTask *h_task = lardon3d_project_create_incremental_reconstruction_task(
      &fixture.state, &h_configuration, &h_task_id);
  CHECK(h_task && h_task_id != 0);
  CHECK(lardon3d_task_kind(h_task, kind, &kind_version));
  CHECK(std::strcmp(kind, LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND) == 0 &&
        kind_version == 1);
  Lardon3DResourceEstimate h_estimate{};
  CHECK(lardon3d_task_resource_estimate(h_task, &h_estimate));
  CHECK(h_estimate.minimum_batch_size == 1 && h_estimate.maximum_batch_size == 1 &&
        h_estimate.desired_cpu_threads == 1 && h_estimate.desired_gpu_slots == 0 &&
        h_estimate.desired_io_slots == 1 &&
        h_estimate.task_class == LARDON3D_RESOURCE_TASK_CPU);
  Lardon3DProjectDbIncrementalReconstructionTask h_payload{};
  CHECK(lardon3d_project_db_load_incremental_reconstruction_task(
            fixture.state.project_db, h_task_id, &h_payload) == LARDON3D_PROJECT_DB_OK);
  unsigned char h_fingerprint[32]{};
  CHECK(lardon3d_incremental_reconstruction_parameter_fingerprint(h_fingerprint));
  CHECK(std::memcmp(h_payload.parameter_fingerprint, h_fingerprint, 32) == 0);
  CHECK(lardon3d_task_queue_add(fixture.state.task_queue, h_task, nullptr));
  CHECK(wait_state(fixture.state.task_queue, h_task_id, TASK_COMPLETED));
  CHECK(wait_reservations_released(fixture.state.resource_governor));
  Lardon3DIncrementalReconstructionIdentity h_identity{
      reconstruction.reconstruction_id, fixture.track_set.track_set_id,
      fixture.scope.scope_id, 1, 1, {}};
  std::memcpy(h_identity.parameter_fingerprint, h_fingerprint, 32);
  unsigned char h_digest[32]{};
  CHECK(lardon3d_incremental_reconstruction_identity_digest(&h_identity,
                                                             h_digest));
  Lardon3DIncrementalReconstructionMetadata h_metadata{};
  Lardon3DSparseReconstruction h_reconstruction{};
  CHECK(lardon3d_incremental_reconstruction_find_exact(
            fixture.state.project_db, h_digest, &h_metadata,
            &h_reconstruction) == LARDON3D_PROJECT_DB_OK);
  CHECK(h_metadata.base_reconstruction_id == reconstruction.reconstruction_id);
  CHECK(h_reconstruction.reconstruction_id != reconstruction.reconstruction_id &&
        h_reconstruction.registered_image_count == kImageCount &&
        h_reconstruction.landmark_count == reconstruction.landmark_count &&
        h_reconstruction.reprojection_rmse_px > 0.0 &&
        h_reconstruction.reprojection_median_px >= 0.0);
  double expected_h_rmse = 0.0;
  double expected_h_median = 0.0;
  CHECK(recompute_metrics(&fixture, h_reconstruction, &expected_h_rmse,
                          &expected_h_median));
  CHECK(std::abs(h_reconstruction.reprojection_rmse_px - expected_h_rmse) <
            1e-12 &&
        std::abs(h_reconstruction.reprojection_median_px - expected_h_median) <
            1e-12);
  CHECK(std::abs(h_reconstruction.reprojection_rmse_px -
                 reconstruction.reprojection_rmse_px) > 1e-12);

  Lardon3DSparseCalibration incompatible_calibration{};
  incompatible_calibration.model_kind =
      LARDON3D_SPARSE_SFM_CALIBRATION_KIND_PINHOLE;
  incompatible_calibration.model_version =
      LARDON3D_SPARSE_SFM_CALIBRATION_VERSION;
  incompatible_calibration.width = 4000;
  incompatible_calibration.height = 3000;
  incompatible_calibration.fx = 2100.0;
  incompatible_calibration.fy = 2100.0;
  incompatible_calibration.cx = 2000.0;
  incompatible_calibration.cy = 1500.0;
  incompatible_calibration.provenance_kind =
      LARDON3D_SPARSE_SFM_PROVENANCE_USER_EXPLICIT;
  Lardon3DSparseCalibration incompatible_stored{};
  CHECK(lardon3d_sparse_calibration_create(
            fixture.state.project_db, &incompatible_calibration,
            &incompatible_stored) == LARDON3D_PROJECT_DB_OK);
  Lardon3DSparseCalibrationMember incompatible_members[kImageCount]{};
  for (size_t index = 0; index < kImageCount; ++index) {
    incompatible_members[index].image_id = fixture.images[index].image_id;
    incompatible_members[index].calibration_id = incompatible_stored.calibration_id;
    std::memcpy(incompatible_members[index].calibration_hash,
                incompatible_stored.scientific_hash, 32);
  }
  Lardon3DSparseCalibrationScope incompatible_scope{};
  CHECK(lardon3d_sparse_calibration_scope_create(
            fixture.state.project_db, incompatible_members, kImageCount,
            &incompatible_scope) == LARDON3D_PROJECT_DB_OK);
  Lardon3DIncrementalReconstructionTaskConfiguration incompatible_h{
      reconstruction.reconstruction_id, fixture.track_set.track_set_id,
      incompatible_scope.scope_id};
  uint64_t incompatible_task_id = 0;
  CHECK(lardon3d_project_enqueue_incremental_reconstruction_task(
      &fixture.state, &incompatible_h, &incompatible_task_id));
  CHECK(wait_state(fixture.state.task_queue, incompatible_task_id, TASK_FAILED));
  CHECK(wait_reservations_released(fixture.state.resource_governor));
  Lardon3DIncrementalReconstructionIdentity incompatible_identity{
      reconstruction.reconstruction_id, fixture.track_set.track_set_id,
      incompatible_scope.scope_id, 1, 1, {}};
  std::memcpy(incompatible_identity.parameter_fingerprint, h_fingerprint, 32);
  unsigned char incompatible_digest[32]{};
  CHECK(lardon3d_incremental_reconstruction_identity_digest(
      &incompatible_identity, incompatible_digest));
  Lardon3DIncrementalReconstructionMetadata missing_metadata{};
  Lardon3DSparseReconstruction missing_reconstruction{};
  CHECK(lardon3d_incremental_reconstruction_find_exact(
            fixture.state.project_db, incompatible_digest, &missing_metadata,
            &missing_reconstruction) == LARDON3D_PROJECT_DB_NOT_FOUND);

  uint64_t h_restart_id = 0;
  Lardon3DTask *h_pending =
      lardon3d_project_create_incremental_reconstruction_task(
          &fixture.state, &h_configuration, &h_restart_id);
  CHECK(h_pending != nullptr);
  Lardon3DTaskDurableSnapshot h_before{};
  CHECK(lardon3d_task_durable_snapshot(h_pending, &h_before));
  lardon3d_task_destroy(h_pending);
  Lardon3DTaskKindDescriptor h_descriptor{
      LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND, 1,
      lardon3d_incremental_reconstruction_task_reconstruct};
  Lardon3DTaskKindRegistry h_registry{};
  CHECK(lardon3d_task_kind_registry_init(&h_registry, &h_descriptor, 1));
  Lardon3DTaskReconstructionContext h_runtime{
      fixture.root.c_str(), fixture.state.project_db,
      fixture.state.resource_governor, nullptr};
  Lardon3DTask *h_restored = nullptr;
  CHECK(lardon3d_task_kind_registry_restore(
            &h_registry, LARDON3D_INCREMENTAL_RECONSTRUCTION_TASK_KIND, 1,
            &h_before, &h_runtime, &h_restored) == LARDON3D_TASK_KIND_OK);
  Lardon3DResourceEstimate h_restored_estimate{};
  CHECK(lardon3d_task_resource_estimate(h_restored, &h_restored_estimate));
  CHECK(std::memcmp(&h_estimate, &h_restored_estimate,
                    sizeof(h_estimate)) == 0);
  CHECK(lardon3d_task_queue_add(fixture.state.task_queue, h_restored, nullptr));
  CHECK(wait_state(fixture.state.task_queue, h_restart_id, TASK_COMPLETED));
  CHECK(wait_reservations_released(fixture.state.resource_governor));
  Lardon3DIncrementalReconstructionMetadata reused_h_metadata{};
  Lardon3DSparseReconstruction reused_h{};
  CHECK(lardon3d_incremental_reconstruction_find_exact(
            fixture.state.project_db, h_digest, &reused_h_metadata, &reused_h) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(reused_h.reconstruction_id == h_reconstruction.reconstruction_id);

  uint64_t reuse_id = 0;
  CHECK(lardon3d_project_enqueue_sparse_sfm_task(&fixture.state, &config, &reuse_id));
  CHECK(wait_state(fixture.state.task_queue, reuse_id, TASK_COMPLETED));
  Lardon3DSparseReconstruction reused{};
  CHECK(lardon3d_sparse_reconstruction_find_exact(
            fixture.state.project_db, fixture.base_track_set.track_set_id,
            fixture.scope.scope_id, 1, 1,
            fingerprint, &reused) == LARDON3D_PROJECT_DB_OK);
  CHECK(reused.reconstruction_id == reconstruction.reconstruction_id);

  uint64_t restart_id = 0;
  Lardon3DTask *pending =
      lardon3d_project_create_sparse_sfm_task(&fixture.state, &config, &restart_id);
  CHECK(pending != nullptr);
  Lardon3DTaskDurableSnapshot before{};
  CHECK(lardon3d_task_durable_snapshot(pending, &before));
  lardon3d_task_destroy(pending);
  lardon3d_project_db_close(fixture.state.project_db);
  fixture.state.project_db = nullptr;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  CHECK(lardon3d_project_db_open(fixture.database_path.c_str(), &fixture.state.project_db, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DIncrementalReconstructionMetadata reopened_h_metadata{};
  Lardon3DSparseReconstruction reopened_h{};
  CHECK(lardon3d_incremental_reconstruction_find_exact(
            fixture.state.project_db, h_digest, &reopened_h_metadata,
            &reopened_h) == LARDON3D_PROJECT_DB_OK);
  CHECK(reopened_h.reconstruction_id == h_reconstruction.reconstruction_id &&
        reopened_h_metadata.base_reconstruction_id ==
            reconstruction.reconstruction_id &&
        reopened_h.registered_image_count == kImageCount &&
        reopened_h.reprojection_rmse_px ==
            h_reconstruction.reprojection_rmse_px);
  Lardon3DTaskKindDescriptor descriptor{LARDON3D_SPARSE_SFM_TASK_KIND, 1,
                                        lardon3d_sparse_sfm_task_reconstruct};
  Lardon3DTaskKindRegistry registry{};
  CHECK(lardon3d_task_kind_registry_init(&registry, &descriptor, 1));
  Lardon3DTaskReconstructionContext runtime{fixture.root.c_str(), fixture.state.project_db,
                                             fixture.state.resource_governor, nullptr};
  Lardon3DTask *restored = nullptr;
  CHECK(lardon3d_task_kind_registry_restore(&registry, LARDON3D_SPARSE_SFM_TASK_KIND, 1,
                                            &before, &runtime, &restored) ==
        LARDON3D_TASK_KIND_OK);
  Lardon3DResourceEstimate restored_estimate{};
  CHECK(lardon3d_task_resource_estimate(restored, &restored_estimate));
  CHECK(std::memcmp(&estimate, &restored_estimate, sizeof(estimate)) == 0);
  Lardon3DProjectDbSparseSfmTask restored_payload{};
  CHECK(lardon3d_project_db_load_sparse_sfm_task(fixture.state.project_db, restart_id,
                                                 &restored_payload) == LARDON3D_PROJECT_DB_OK);
  unsigned char restored_fingerprint[32]{};
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&restored_payload.parameters,
                                                  restored_fingerprint));
  CHECK(std::memcmp(fingerprint, restored_fingerprint, 32) == 0);
  unsigned char record_before[372]{};
  unsigned char record_after[372]{};
  CHECK(lardon3d_sparse_sfm_fingerprint_record(&fixture.parameters, record_before));
  CHECK(lardon3d_sparse_sfm_fingerprint_record(&restored_payload.parameters, record_after));
  CHECK(std::memcmp(record_before, record_after, sizeof(record_before)) == 0);
  CHECK(lardon3d_task_queue_add(fixture.state.task_queue, restored, nullptr));
  CHECK(wait_state(fixture.state.task_queue, restart_id, TASK_COMPLETED));
  destroy_runtime(&fixture);
  return true;
}
} // namespace

int main() {
  return test_lookup() && test_landmark_projection_order() && test_task() ? 0 : 1;
}
