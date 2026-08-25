#include <lardon3d/incremental_reconstruction.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#define CHECK(value)                                                            \
  do {                                                                          \
    if (!(value)) {                                                             \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #value);                                                     \
      return false;                                                             \
    }                                                                           \
  } while (0)

namespace {
Lardon3DSparseGeometryCalibration calibration() {
  return {640, 480, 800.0, 800.0, 320.0, 240.0, 0.0, 0.0, 0.0, 0.0};
}

Lardon3DSparseGeometryPose pose(double center_x) {
  Lardon3DSparseGeometryPose result{};
  result.rotation_cw[0] = result.rotation_cw[4] = result.rotation_cw[8] = 1.0;
  result.translation_cw[0] = -center_x;
  return result;
}

Lardon3DSparseGeometryPoint2 project(double center_x,
                                    const Lardon3DSparseGeometryPoint3 &point) {
  return {800.0 * (point.x - center_x) / point.z + 320.0,
          800.0 * point.y / point.z + 240.0};
}

struct Fixture {
  Lardon3DSparseIncrementalParameters parameters{};
  std::vector<Lardon3DSparseIncrementalComponent> components;
  std::vector<Lardon3DSparseIncrementalCamera> cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> landmarks;
  std::vector<Lardon3DSparseIncrementalLandmarkObservation> base_observations;
  std::vector<Lardon3DSparseIncrementalObservation> base_tracks;
  std::vector<Lardon3DSparseIncrementalImage> images;
  std::vector<Lardon3DSparseIncrementalObservation> extension_observations;
  Lardon3DSparseIncrementalResult base{};
  Lardon3DSparseIncrementalInput extension{};

  void refresh() {
    base.components = components.data();
    base.component_count = components.size();
    base.cameras = cameras.data();
    base.camera_count = cameras.size();
    base.landmarks = landmarks.data();
    base.landmark_count = landmarks.size();
    base.observations = base_observations.data();
    base.observation_count = base_observations.size();
    extension.images = images.data();
    extension.image_count = images.size();
    extension.observations = extension_observations.data();
    extension.observation_count = extension_observations.size();
  }

  Fixture(uint64_t new_image_id, bool descendant_new_observations,
          bool new_only_observations) {
    (void)lardon3d_sparse_incremental_parameters_default(&parameters);
    parameters.minimum_pnp_correspondences = 6;
    parameters.pnp.minimum_inliers = 6;
    components.push_back({100, 2, 2, 8});
    cameras.push_back({100, 100, pose(0.0)});
    cameras.push_back({101, 100, pose(1.0)});
    images.push_back({100, calibration()});
    images.push_back({101, calibration()});
    images.push_back({new_image_id, calibration()});
    for (uint32_t index = 0; index < 8; ++index) {
      Lardon3DSparseGeometryPoint3 point{
          -0.8 + 0.25 * index, -0.35 + 0.1 * (index % 4),
          4.0 + 0.3 * (index % 3)};
      const uint64_t base_track = 10 + index;
      const uint64_t extension_track = 1000 + index;
      landmarks.push_back({500 + index, base_track, 100, point, 0.0, 0.0, 2});
      for (uint32_t camera_index = 0; camera_index < 2; ++camera_index) {
        const uint64_t image_id = 100 + camera_index;
        const uint64_t feature_set = 10000 + image_id;
        const auto pixel = project(static_cast<double>(camera_index), point);
        base_tracks.push_back({base_track, image_id, feature_set, index, 64,
                               pixel.x, pixel.y});
        extension_observations.push_back(
            {extension_track, image_id, feature_set, index, 64, pixel.x, pixel.y});
        base_observations.push_back({500 + index, base_track, image_id,
                                     feature_set, index, camera_index});
      }
      if (descendant_new_observations) {
        const auto pixel = project(0.5, point);
        extension_observations.push_back(
            {extension_track, new_image_id, 10000 + new_image_id, index, 64,
             pixel.x, pixel.y});
      }
    }
    if (new_only_observations)
      extension_observations.push_back(
          {9000, new_image_id, 20000 + new_image_id, 0, 1, 320.0, 240.0});
    base.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
    base.track_set_id = 1;
    base.calibration_scope_id = 1;
    extension = {2, 2, images.data(), images.size(), extension_observations.data(),
                 extension_observations.size()};
    refresh();
  }

  Lardon3DIncrementalReconstructionStatus run(
      Lardon3DIncrementalReconstructionResult *result) {
    Lardon3DIncrementalReconstructionInput input{
        7, &base, base_tracks.data(), base_tracks.size(), &extension,
        &parameters, nullptr, nullptr};
    return lardon3d_incremental_reconstruction_run(&input, result);
  }
};

bool test_identity_and_estimate() {
  unsigned char fingerprint[32]{};
  CHECK(lardon3d_incremental_reconstruction_parameter_fingerprint(fingerprint));
  const unsigned char golden[32] = {
      0xf4, 0x4a, 0x89, 0xb2, 0x3b, 0x52, 0x04, 0x81,
      0x70, 0x18, 0x48, 0xec, 0xf1, 0x75, 0x63, 0x8e,
      0x82, 0xf2, 0xc9, 0xe2, 0x5b, 0x70, 0xa0, 0x1f,
      0x3e, 0xb7, 0x67, 0xbf, 0x28, 0x44, 0x6c, 0xd8};
  CHECK(std::memcmp(fingerprint, golden, 32) == 0);
  const unsigned char zero[32]{};
  CHECK(std::memcmp(fingerprint, zero, 32) != 0);
  Lardon3DIncrementalReconstructionIdentity identity{7, 8, 9, 1, 1, {}};
  std::memcpy(identity.parameter_fingerprint, fingerprint, 32);
  unsigned char first[32]{};
  unsigned char second[32]{};
  CHECK(lardon3d_incremental_reconstruction_identity_digest(&identity, first));
  CHECK(lardon3d_incremental_reconstruction_identity_digest(&identity, second));
  CHECK(std::memcmp(first, second, 32) == 0);
  ++identity.base_reconstruction_id;
  CHECK(lardon3d_incremental_reconstruction_identity_digest(&identity, second));
  CHECK(std::memcmp(first, second, 32) != 0);
  --identity.base_reconstruction_id;
  ++identity.extension_track_set_id;
  CHECK(lardon3d_incremental_reconstruction_identity_digest(&identity, second));
  CHECK(std::memcmp(first, second, 32) != 0);
  --identity.extension_track_set_id;
  ++identity.calibration_scope_id;
  CHECK(lardon3d_incremental_reconstruction_identity_digest(&identity, second));
  CHECK(std::memcmp(first, second, 32) != 0);

  Lardon3DIncrementalReconstructionShape shape{2, 8, 16, 3, 8, 24};
  Lardon3DResourceEstimate estimate{};
  CHECK(lardon3d_incremental_reconstruction_resource_estimate(&shape, &estimate));
  CHECK(estimate.memory_fixed_bytes == 269484032);
  CHECK(estimate.minimum_batch_size == 1 && estimate.maximum_batch_size == 1);
  CHECK(estimate.desired_cpu_threads == 1 && estimate.desired_gpu_slots == 0 &&
        estimate.desired_io_slots == 1 &&
        estimate.task_class == LARDON3D_RESOURCE_TASK_CPU);
  shape.base_camera_count = std::numeric_limits<uint64_t>::max();
  CHECK(!lardon3d_incremental_reconstruction_resource_estimate(&shape, &estimate));
  return true;
}

bool test_lineage_failures() {
  Fixture missing(150, false, false);
  missing.extension_observations.erase(missing.extension_observations.begin());
  missing.extension.observations = missing.extension_observations.data();
  missing.extension.observation_count = missing.extension_observations.size();
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(missing.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MISSING);

  Fixture split(150, false, false);
  split.extension_observations[1].track_id = 7000;
  CHECK(split.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_SPLIT);

  Fixture merge(150, false, false);
  for (auto &observation : merge.extension_observations)
    if (observation.track_id == 1001) observation.track_id = 1000;
  CHECK(merge.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_MERGE);

  Fixture duplicate(150, false, false);
  duplicate.extension_observations.push_back(duplicate.extension_observations.front());
  duplicate.extension.observations = duplicate.extension_observations.data();
  duplicate.extension.observation_count = duplicate.extension_observations.size();
  CHECK(duplicate.run(&result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_LINEAGE_DUPLICATE);
  return true;
}

bool test_h_b01() {
  Fixture fixture(150, true, false);
  fixture.parameters.maximum_registration_rounds = 0;
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture.run(&result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_DESCENDANT_UNREGISTERED_OBSERVATION);

  Fixture new_only(150, false, true);
  new_only.parameters.maximum_registration_rounds = 0;
  CHECK(new_only.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_NO_CHANGE);
  CHECK(!result.changed && result.snapshot.component_count == 0);
  return true;
}

bool test_h_b02() {
  Fixture fixture(99, true, false);
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture.run(&result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_COMPONENT_KEY_VIOLATION);
  CHECK(!result.changed && result.snapshot.component_count == 0);
  return true;
}

bool reject_checkpoint(void *) { return false; }

bool test_cancellation() {
  Fixture fixture(150, true, false);
  Lardon3DIncrementalReconstructionInput input{
      7, &fixture.base, fixture.base_tracks.data(), fixture.base_tracks.size(),
      &fixture.extension, &fixture.parameters, reject_checkpoint, nullptr};
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(lardon3d_incremental_reconstruction_run(&input, &result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_CANCELLED);
  CHECK(!result.changed && result.snapshot.component_count == 0);
  return true;
}

bool test_successful_enrichment() {
  Fixture fixture(150, true, false);
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_OK);
  CHECK(result.changed && result.snapshot.component_count == 1 &&
        result.snapshot.camera_count == 3 && result.snapshot.landmark_count == 8 &&
        result.snapshot.observation_count == 24);
  CHECK(result.snapshot.components[0].component_key == 100);
  CHECK(result.snapshot.cameras[0].image_id == 100 &&
        result.snapshot.cameras[2].image_id == 150);
  lardon3d_incremental_reconstruction_result_destroy(&result);
  return true;
}

void add_second_component(Fixture *fixture) {
  fixture->components.push_back({200, 2, 2, 8});
  fixture->cameras.push_back({200, 200, pose(10.0)});
  fixture->cameras.push_back({201, 200, pose(11.0)});
  fixture->images.push_back({200, calibration()});
  fixture->images.push_back({201, calibration()});
  for (uint32_t index = 0; index < 8; ++index) {
    Lardon3DSparseGeometryPoint3 point{
        9.2 + 0.25 * index, -0.35 + 0.1 * (index % 4),
        4.0 + 0.3 * (index % 3)};
    const uint64_t base_track = 30 + index;
    const uint64_t extension_track = 3000 + index;
    fixture->landmarks.push_back(
        {700 + index, base_track, 200, point, 0.0, 0.0, 2});
    for (uint32_t camera_index = 0; camera_index < 2; ++camera_index) {
      const uint64_t image_id = 200 + camera_index;
      const uint64_t feature_set = 10000 + image_id;
      const auto pixel = project(10.0 + camera_index, point);
      fixture->base_tracks.push_back({base_track, image_id, feature_set, index,
                                      64, pixel.x, pixel.y});
      fixture->extension_observations.push_back(
          {extension_track, image_id, feature_set, index, 64, pixel.x, pixel.y});
      fixture->base_observations.push_back(
          {700 + index, base_track, image_id, feature_set, index, camera_index});
    }
  }
  fixture->refresh();
}

bool test_component_isolation_and_bridge() {
  Fixture fixture(150, true, false);
  add_second_component(&fixture);
  const auto camera_b0 = fixture.cameras[2];
  const auto landmark_b0 = fixture.landmarks[8];
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_OK);
  CHECK(result.snapshot.component_count == 2 &&
        result.snapshot.components[0].component_key == 100 &&
        result.snapshot.components[1].component_key == 200);
  auto camera = std::find_if(result.snapshot.cameras,
                             result.snapshot.cameras + result.snapshot.camera_count,
                             [](const auto &item) { return item.image_id == 200; });
  auto landmark = std::find_if(
      result.snapshot.landmarks,
      result.snapshot.landmarks + result.snapshot.landmark_count,
      [](const auto &item) { return item.track_id == 3000; });
  CHECK(camera != result.snapshot.cameras + result.snapshot.camera_count &&
        landmark != result.snapshot.landmarks + result.snapshot.landmark_count);
  CHECK(std::memcmp(&camera->pose_cw, &camera_b0.pose_cw,
                    sizeof(camera->pose_cw)) == 0);
  CHECK(std::memcmp(&landmark->point, &landmark_b0.point,
                    sizeof(landmark->point)) == 0);
  lardon3d_incremental_reconstruction_result_destroy(&result);

  Fixture bridge(150, true, false);
  add_second_component(&bridge);
  for (auto &observation : bridge.extension_observations)
    if (observation.track_id == 3000) observation.track_id = 1000;
  bridge.refresh();
  CHECK(bridge.run(&result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_CROSS_COMPONENT_BRIDGE);
  return true;
}

bool expect_invalid_base(Fixture *fixture) {
  fixture->refresh();
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture->run(&result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE);
  CHECK(!result.changed && result.snapshot.components == nullptr &&
        result.snapshot.component_count == 0);
  lardon3d_incremental_reconstruction_result_destroy(&result);
  return true;
}

bool test_base_observation_validation() {
  Fixture wrong_landmark(150, false, false);
  ++wrong_landmark.base_observations[0].landmark_id;
  CHECK(expect_invalid_base(&wrong_landmark));

  Fixture wrong_track(150, false, false);
  ++wrong_track.base_observations[0].track_id;
  CHECK(expect_invalid_base(&wrong_track));

  Fixture duplicate_position(150, false, false);
  duplicate_position.base_observations[1].position_in_track = 0;
  CHECK(expect_invalid_base(&duplicate_position));

  Fixture invalid_position(150, false, false);
  invalid_position.base_observations[0].position_in_track = 2;
  CHECK(expect_invalid_base(&invalid_position));

  Fixture wrong_count(150, false, false);
  ++wrong_count.landmarks[0].observation_count;
  CHECK(expect_invalid_base(&wrong_count));

  Fixture cross_component(150, false, false);
  add_second_component(&cross_component);
  cross_component.base_observations[0].image_id = 200;
  CHECK(expect_invalid_base(&cross_component));

  Fixture missing_source(150, false, false);
  missing_source.base_tracks.pop_back();
  CHECK(expect_invalid_base(&missing_source));

  Fixture extra_source(150, false, false);
  extra_source.base_tracks.push_back(extra_source.base_tracks.front());
  extra_source.base_tracks.back().feature_index = 63;
  CHECK(expect_invalid_base(&extra_source));

  Fixture wrong_source_image(150, false, false);
  wrong_source_image.base_tracks[0].image_id = 101;
  CHECK(expect_invalid_base(&wrong_source_image));

  Fixture wrong_source_feature(150, false, false);
  ++wrong_source_feature.base_tracks[0].feature_index;
  CHECK(expect_invalid_base(&wrong_source_feature));

  Fixture duplicate_source(150, false, false);
  duplicate_source.base_tracks.push_back(duplicate_source.base_tracks.front());
  CHECK(expect_invalid_base(&duplicate_source));
  return true;
}

bool test_landmark_id_exhaustion() {
  Fixture fixture(150, true, false);
  fixture.images.push_back({151, calibration()});
  for (uint32_t index = 0; index < 8; ++index) {
    const auto pixel = project(0.75, fixture.landmarks[index].point);
    fixture.extension_observations.push_back(
        {1000 + index, 151, 10151, index, 64, pixel.x, pixel.y});
  }
  const Lardon3DSparseGeometryPoint3 new_point{0.2, -0.1, 5.5};
  const auto first = project(0.5, new_point);
  const auto second = project(0.75, new_point);
  fixture.extension_observations.push_back(
      {9000, 150, 20150, 0, 1, first.x, first.y});
  fixture.extension_observations.push_back(
      {9000, 151, 20151, 0, 1, second.x, second.y});
  fixture.landmarks.back().landmark_id = UINT64_MAX - 1;
  for (auto &observation : fixture.base_observations)
    if (observation.track_id == fixture.landmarks.back().track_id)
      observation.landmark_id = UINT64_MAX - 1;
  fixture.refresh();
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture.run(&result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_INVALID_BASE);
  CHECK(!result.changed && result.snapshot.components == nullptr);
  lardon3d_incremental_reconstruction_result_destroy(&result);
  return true;
}

bool snapshots_equal(const Lardon3DSparseIncrementalResult &left,
                     const Lardon3DSparseIncrementalResult &right) {
  return left.component_count == right.component_count &&
         left.camera_count == right.camera_count &&
         left.landmark_count == right.landmark_count &&
         left.observation_count == right.observation_count &&
         std::memcmp(left.components, right.components,
                     left.component_count * sizeof(*left.components)) == 0 &&
         std::memcmp(left.cameras, right.cameras,
                     left.camera_count * sizeof(*left.cameras)) == 0 &&
         std::memcmp(left.landmarks, right.landmarks,
                     left.landmark_count * sizeof(*left.landmarks)) == 0 &&
         std::memcmp(left.observations, right.observations,
                     left.observation_count * sizeof(*left.observations)) == 0;
}

bool test_new_only_triangulation_and_determinism() {
  Fixture fixture(150, true, false);
  fixture.images.push_back({151, calibration()});
  for (uint32_t index = 0; index < 8; ++index) {
    const auto pixel = project(0.75, fixture.landmarks[index].point);
    fixture.extension_observations.push_back(
        {1000 + index, 151, 10151, index, 64, pixel.x, pixel.y});
  }
  const Lardon3DSparseGeometryPoint3 new_point{0.2, -0.1, 5.5};
  auto first = project(0.5, new_point);
  auto second = project(0.75, new_point);
  fixture.extension_observations.push_back(
      {9000, 150, 20150, 0, 1, first.x, first.y});
  fixture.extension_observations.push_back(
      {9000, 151, 20151, 0, 1, second.x, second.y});
  fixture.refresh();

  Lardon3DIncrementalReconstructionResult first_result{};
  Lardon3DIncrementalReconstructionResult second_result{};
  CHECK(fixture.run(&first_result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_OK);
  CHECK(fixture.run(&second_result) == LARDON3D_INCREMENTAL_RECONSTRUCTION_OK);
  CHECK(first_result.snapshot.camera_count == 4 &&
        first_result.snapshot.landmark_count == 9 &&
        first_result.snapshot.components[0].component_key == 100);
  auto landmark = std::lower_bound(
      first_result.snapshot.landmarks,
      first_result.snapshot.landmarks + first_result.snapshot.landmark_count,
      UINT64_C(9000), [](const auto &item, uint64_t track_id) {
        return item.track_id < track_id;
      });
  CHECK(landmark != first_result.snapshot.landmarks +
                        first_result.snapshot.landmark_count &&
        landmark->track_id == 9000 && landmark->component_key == 100 &&
        landmark->observation_count == 2);
  CHECK(snapshots_equal(first_result.snapshot, second_result.snapshot));
  lardon3d_incremental_reconstruction_result_destroy(&first_result);
  lardon3d_incremental_reconstruction_result_destroy(&second_result);
  return true;
}

bool test_bundle_adjustment_failure() {
  Fixture fixture(150, true, false);
  fixture.cameras[1].pose_cw = fixture.cameras[0].pose_cw;
  fixture.refresh();
  Lardon3DIncrementalReconstructionResult result{};
  CHECK(fixture.run(&result) ==
        LARDON3D_INCREMENTAL_RECONSTRUCTION_BUNDLE_ADJUSTMENT_FAILED);
  CHECK(!result.changed && result.snapshot.components == nullptr &&
        result.snapshot.component_count == 0);
  lardon3d_incremental_reconstruction_result_destroy(&result);
  return true;
}
} // namespace

int main() {
  if (!test_identity_and_estimate() || !test_lineage_failures() ||
      !test_h_b01() || !test_h_b02() || !test_cancellation() ||
      !test_successful_enrichment() ||
      !test_component_isolation_and_bridge() ||
      !test_base_observation_validation() ||
      !test_landmark_id_exhaustion() ||
      !test_new_only_triangulation_and_determinism() ||
      !test_bundle_adjustment_failure())
    return 1;
  return 0;
}
