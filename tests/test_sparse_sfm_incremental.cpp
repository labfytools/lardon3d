#include <lardon3d/sparse_sfm_incremental.h>

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#define CHECK(value)                                                            \
  do {                                                                          \
    if (!(value)) {                                                             \
      std::fprintf(stderr, "incremental check failed at line %d: %s\n",       \
                   __LINE__, #value);                                          \
      return false;                                                             \
    }                                                                           \
  } while (0)

static Lardon3DSparseGeometryPoint2 project(
    const Lardon3DSparseGeometryCalibration &calibration,
    const Lardon3DSparseGeometryPose &pose,
    const Lardon3DSparseGeometryPoint3 &point) {
  const double x = pose.rotation_cw[0] * point.x + pose.rotation_cw[1] * point.y +
                   pose.rotation_cw[2] * point.z + pose.translation_cw[0];
  const double y = pose.rotation_cw[3] * point.x + pose.rotation_cw[4] * point.y +
                   pose.rotation_cw[5] * point.z + pose.translation_cw[1];
  const double z = pose.rotation_cw[6] * point.x + pose.rotation_cw[7] * point.y +
                   pose.rotation_cw[8] * point.z + pose.translation_cw[2];
  return {calibration.fx * x / z + calibration.cx,
          calibration.fy * y / z + calibration.cy};
}

static bool run_test() {
  Lardon3DSparseIncrementalParameters parameters;
  CHECK(lardon3d_sparse_incremental_parameters_default(&parameters));
  parameters.minimum_seed_tracks = 6;
  parameters.minimum_seed_landmarks = 6;
  parameters.minimum_pnp_correspondences = 6;
  parameters.maximum_registration_rounds = 8;

  const Lardon3DSparseGeometryCalibration calibration = {
      4000, 3000, 2000.0, 2000.0, 2000.0, 1500.0, 0.0, 0.0, 0.0, 0.0};
  const Lardon3DSparseGeometryPose poses[3] = {
      {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}},
      {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0}},
      {{1, 0, 0, 0, 1, 0, 0, 0, 1}, {2, 0, 0}},
  };
  Lardon3DSparseIncrementalImage images[3] = {
      {10, calibration}, {20, calibration}, {30, calibration}};
  std::vector<Lardon3DSparseIncrementalObservation> observations;
  for (uint64_t track = 1; track <= 12; ++track) {
    Lardon3DSparseGeometryPoint3 point = {
        -1.0 + static_cast<double>(track % 4) * 0.5,
        -0.8 + static_cast<double>(track / 4) * 0.4,
        5.0 + static_cast<double>(track % 3) * 0.25};
    for (size_t camera = 0; camera < 3; ++camera) {
      Lardon3DSparseGeometryPoint2 pixel = project(calibration, poses[camera], point);
      observations.push_back({track, images[camera].image_id, 100 + camera,
                              static_cast<uint32_t>(track), 64, pixel.x, pixel.y});
    }
  }
  Lardon3DSparseIncrementalInput input = {
      77, 88, images, 3, observations.data(), observations.size()};
  Lardon3DSparseIncrementalResult result = {};
  CHECK(lardon3d_sparse_incremental_run(&input, &parameters, &result) ==
        LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
  CHECK(result.component_count == 1);
  CHECK(result.camera_count == 3);
  CHECK(result.landmark_count >= 6);
  CHECK(result.unregistered_image_count == 0);
  CHECK(result.components[0].component_key == 10);
  for (size_t index = 0; index < result.camera_count; ++index)
    CHECK(result.cameras[index].component_key == 10);
  for (size_t index = 1; index < result.camera_count; ++index)
    CHECK(result.cameras[index - 1].image_id < result.cameras[index].image_id);
  for (size_t index = 1; index < result.landmark_count; ++index)
    CHECK(result.landmarks[index - 1].track_id < result.landmarks[index].track_id);

  Lardon3DSparseIncrementalResult repeated = {};
  CHECK(lardon3d_sparse_incremental_run(&input, &parameters, &repeated) ==
        result.status);
  CHECK(repeated.camera_count == result.camera_count);
  CHECK(repeated.landmark_count == result.landmark_count);
  for (size_t index = 0; index < result.camera_count; ++index) {
    CHECK(repeated.cameras[index].image_id == result.cameras[index].image_id);
    for (size_t value = 0; value < 9; ++value)
      CHECK(repeated.cameras[index].pose_cw.rotation_cw[value] ==
            result.cameras[index].pose_cw.rotation_cw[value]);
  }
  lardon3d_sparse_incremental_result_destroy(&repeated);

  Lardon3DSparseIncrementalInput disconnected = input;
  Lardon3DSparseIncrementalImage singleton = {99, calibration};
  std::vector<Lardon3DSparseIncrementalImage> more_images = {images[0], images[1],
                                                              images[2], singleton};
  disconnected.images = more_images.data();
  disconnected.image_count = more_images.size();
  Lardon3DSparseIncrementalResult partial = {};
  CHECK(lardon3d_sparse_incremental_run(&disconnected, &parameters, &partial) ==
        LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
  CHECK(partial.unregistered_image_count == 1);
  CHECK(partial.unregistered_images[0].image_id == 99);
  lardon3d_sparse_incremental_result_destroy(&partial);

  Lardon3DSparseIncrementalResult invalid = {};
  Lardon3DSparseIncrementalInput invalid_input = input;
  invalid_input.observations = nullptr;
  CHECK(lardon3d_sparse_incremental_run(&invalid_input, &parameters, &invalid) ==
        LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
  CHECK(invalid.camera_count == 0 && invalid.landmark_count == 0);

  auto expect_invalid = [&](const Lardon3DSparseIncrementalInput &candidate,
                            const Lardon3DSparseIncrementalParameters &limits) {
    Lardon3DSparseIncrementalResult output = {};
    const auto status = lardon3d_sparse_incremental_run(
        &candidate, &limits, &output);
    const bool valid = status == LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT &&
                       output.camera_count == 0 && output.landmark_count == 0;
    lardon3d_sparse_incremental_result_destroy(&output);
    return valid;
  };
  Lardon3DSparseIncrementalInput malformed = input;
  malformed.images = nullptr;
  CHECK(expect_invalid(malformed, parameters));
  malformed = input;
  malformed.observations = observations.data();
  std::vector<Lardon3DSparseIncrementalObservation> bad_observations = observations;
  bad_observations[0].feature_count = 1;
  malformed.observations = bad_observations.data();
  CHECK(expect_invalid(malformed, parameters));
  bad_observations = observations;
  bad_observations[0].x = NAN;
  malformed.observations = bad_observations.data();
  CHECK(expect_invalid(malformed, parameters));
  bad_observations = observations;
  bad_observations.back().track_id = bad_observations.front().track_id;
  bad_observations.back().image_id = bad_observations.front().image_id;
  malformed.observations = bad_observations.data();
  CHECK(expect_invalid(malformed, parameters));
  Lardon3DSparseIncrementalParameters bounded = parameters;
  bounded.maximum_images = 2;
  CHECK(expect_invalid(input, bounded));
  bounded = parameters;
  bounded.maximum_observations = observations.size() - 1;
  CHECK(expect_invalid(input, bounded));
  bounded = parameters;
  bounded.maximum_tracks = 1;
  Lardon3DSparseIncrementalResult bounded_result = {};
  CHECK(lardon3d_sparse_incremental_run(&input, &bounded, &bounded_result) ==
        LARDON3D_SPARSE_INCREMENTAL_FAILED);
  lardon3d_sparse_incremental_result_destroy(&bounded_result);
  Lardon3DSparseIncrementalParameters invalid_limits = parameters;
  invalid_limits.maximum_registration_rounds = 0;
  CHECK(expect_invalid(input, invalid_limits));

  std::vector<Lardon3DSparseIncrementalObservation> flat = observations;
  for (auto &observation : flat) {
    observation.x = 2100.0;
    observation.y = 1500.0;
  }
  malformed = input;
  malformed.observations = flat.data();
  Lardon3DSparseIncrementalResult exhausted = {};
  CHECK(lardon3d_sparse_incremental_run(&malformed, &parameters, &exhausted) ==
        LARDON3D_SPARSE_INCREMENTAL_FAILED);
  CHECK(exhausted.seed_candidates_considered > 0);
  lardon3d_sparse_incremental_result_destroy(&exhausted);

  lardon3d_sparse_incremental_result_destroy(&result);
  return true;
}

struct BatchFixture {
  Lardon3DSparseGeometryCalibration calibration;
  std::vector<Lardon3DSparseIncrementalImage> images;
  std::vector<Lardon3DSparseIncrementalObservation> observations;
};

static BatchFixture make_batch_fixture(size_t camera_count, size_t track_count,
                                       bool reject_first_seed, bool noisy,
                                       bool outliers, bool fail_pnp) {
  BatchFixture fixture;
  fixture.calibration = {4000, 3000, 2000.0, 2000.0, 2000.0, 1500.0,
                         0.0, 0.0, 0.0, 0.0};
  for (size_t camera = 0; camera < camera_count; ++camera)
    fixture.images.push_back({10 + camera, fixture.calibration});
  for (size_t track = 0; track < track_count; ++track) {
    const Lardon3DSparseGeometryPoint3 point = {
        -1.0 + static_cast<double>(track % 4) * 0.5,
        -0.8 + static_cast<double>(track / 4) * 0.4,
        5.0 + static_cast<double>(track % 3) * 0.25};
    for (size_t camera = 0; camera < camera_count; ++camera) {
      double translation = static_cast<double>(camera);
      if (reject_first_seed && camera == 1) translation = 1e-3;
      const Lardon3DSparseGeometryPose pose = {
          {1, 0, 0, 0, 1, 0, 0, 0, 1}, {translation, 0, 0}};
      Lardon3DSparseGeometryPoint2 pixel =
          project(fixture.calibration, pose, point);
      if (noisy && camera == 2) {
        pixel.x += static_cast<double>((track * 17) % 5) * 0.12 - 0.24;
        pixel.y += static_cast<double>((track * 11) % 5) * 0.10 - 0.20;
      }
      if (outliers && camera == 2 && track < 3) {
        pixel.x += 900.0 + static_cast<double>(track) * 30.0;
        pixel.y -= 700.0;
      }
      if (fail_pnp && camera == 2) {
        pixel.x = 300.0 + static_cast<double>((track * 613) % 3300);
        pixel.y = 200.0 + static_cast<double>((track * 977) % 2300);
      }
      fixture.observations.push_back(
          {track + 1, 10 + camera, 100 + camera,
           static_cast<uint32_t>(track),
           static_cast<uint32_t>(track_count + 1), pixel.x, pixel.y});
    }
  }
  return fixture;
}

static BatchFixture make_growth_fixture(int kind) {
  BatchFixture fixture = make_batch_fixture(3, 12, false, false, false, false);
  fixture.images[2].image_id = 13;
  for (auto &observation : fixture.observations)
    if (observation.image_id == 12) observation.image_id = 13;
  fixture.observations.erase(
      std::remove_if(fixture.observations.begin(), fixture.observations.end(),
                     [](const auto &observation) {
                       return observation.image_id == 13 && observation.track_id > 6;
                     }),
      fixture.observations.end());
  const Lardon3DSparseGeometryPose growth_pose = {
      {1, 0, 0, 0, 1, 0, 0, 0, 1},
      {2, 0, kind == 19 ? -4.5 : 0}};
  if (kind == 19) {
    for (auto &observation : fixture.observations) {
      if (observation.image_id != 13) continue;
      const size_t track = static_cast<size_t>(observation.track_id - 1);
      const Lardon3DSparseGeometryPoint3 support = {
          -1.0 + static_cast<double>(track % 4) * 0.5,
          -0.8 + static_cast<double>(track / 4) * 0.4,
          5.0 + static_cast<double>(track % 3) * 0.25};
      const auto pixel = project(fixture.calibration, growth_pose, support);
      observation.x = pixel.x;
      observation.y = pixel.y;
    }
  }
  const Lardon3DSparseGeometryPoint3 point = {
      0.25, 0.15, kind == 19 ? 4.0 : 5.5};
  const Lardon3DSparseGeometryPose pose_a = {
      {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
  const auto pixel_a = project(fixture.calibration, pose_a, point);
  auto pixel_b = project(fixture.calibration, growth_pose, point);
  if (kind == 21) pixel_b = pixel_a;
  if (kind == 19 || kind == 20 || kind == 21 || kind == 25) {
    const Lardon3DSparseGeometryPose pose_mid = {
        {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0}};
    const auto pixel_mid = project(fixture.calibration, pose_mid, point);
    fixture.observations.push_back({
        13, 11, 101, 12, 64,
        kind == 21 ? pixel_a.x
                   : kind == 20 ? pixel_mid.x + 700.0 : pixel_mid.x,
        kind == 21 ? pixel_a.y
                   : kind == 20 ? pixel_mid.y - 500.0 : pixel_mid.y});
  }
  fixture.observations.push_back({13, 10, 100, 12, 64, pixel_a.x, pixel_a.y});
  fixture.observations.push_back({13, 13, 102, 12, 64, pixel_b.x, pixel_b.y});
  return fixture;
}

static bool run_batch_cases() {
  Lardon3DSparseIncrementalParameters parameters;
  CHECK(lardon3d_sparse_incremental_parameters_default(&parameters));
  parameters.minimum_seed_tracks = 6;
  parameters.minimum_seed_landmarks = 6;
  parameters.minimum_pnp_correspondences = 6;
  parameters.maximum_registration_rounds = 8;

  /* CASE 01: minimal valid two-view reconstruction. */
  {
    const BatchFixture minimal =
        make_batch_fixture(2, 12, false, false, false, false);
    const Lardon3DSparseIncrementalInput candidate = {
        301, 302, minimal.images.data(), minimal.images.size(),
        minimal.observations.data(), minimal.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.seed_candidates_considered == 1);
    CHECK(output.camera_count == 2);
    CHECK(output.landmark_count >= parameters.minimum_seed_landmarks);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 02: deterministic seed selection. */
  {
    const BatchFixture deterministic =
        make_batch_fixture(3, 12, false, false, false, false);
    const Lardon3DSparseIncrementalInput candidate = {
        303, 304, deterministic.images.data(), deterministic.images.size(),
        deterministic.observations.data(), deterministic.observations.size()};
    Lardon3DSparseIncrementalResult first = {}, second = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &first) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &second) ==
          first.status);
    CHECK(first.seed_image_a == 10 && first.seed_image_b == 11);
    CHECK(second.seed_image_a == first.seed_image_a);
    CHECK(second.seed_image_b == first.seed_image_b);
    CHECK(std::memcmp(first.cameras, second.cameras,
                      first.camera_count * sizeof(*first.cameras)) == 0);
    lardon3d_sparse_incremental_result_destroy(&second);
    lardon3d_sparse_incremental_result_destroy(&first);
  }

  /* CASE 03: multiple eligible candidates use canonical ordering. */
  {
    const BatchFixture multiple =
        make_batch_fixture(4, 12, false, false, false, false);
    const Lardon3DSparseIncrementalInput candidate = {
        305, 306, multiple.images.data(), multiple.images.size(),
        multiple.observations.data(), multiple.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.seed_candidates_available == 6);
    CHECK(output.seed_candidates_considered == 1);
    CHECK(output.seed_image_a == 10 && output.seed_image_b == 11);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 04: first seed rejected, later seed accepted. */
  const BatchFixture fixture = make_batch_fixture(4, 12, true, false, false, false);
  const Lardon3DSparseIncrementalInput input = {
      401, 402, fixture.images.data(), fixture.images.size(),
      fixture.observations.data(), fixture.observations.size()};
  Lardon3DSparseIncrementalResult result = {};
  CHECK(lardon3d_sparse_incremental_run(&input, &parameters, &result) ==
        LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
  CHECK(result.seed_candidates_considered >= 2);
  CHECK(result.seed_image_a == 10 && result.seed_image_b == 12);
  CHECK(result.camera_count == 4);
  lardon3d_sparse_incremental_result_destroy(&result);

  /* CASE 05: equal-support cameras are added by image ID, one per round. */
  {
    const BatchFixture ordered =
        make_batch_fixture(5, 12, false, false, false, false);
    Lardon3DSparseIncrementalParameters one_round = parameters;
    one_round.maximum_registration_rounds = 1;
    const Lardon3DSparseIncrementalInput candidate = {
        307, 308, ordered.images.data(), ordered.images.size(),
        ordered.observations.data(), ordered.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &one_round, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(output.registration_rounds == 1);
    CHECK(output.registration_successes == 1);
    CHECK(output.camera_count == 3);
    CHECK(output.cameras[2].image_id == 12);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 06: clean calibrated PnP registers one camera. */
  {
    const BatchFixture clean =
        make_batch_fixture(3, 12, false, false, false, false);
    const Lardon3DSparseIncrementalInput candidate = {
        309, 310, clean.images.data(), clean.images.size(),
        clean.observations.data(), clean.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.registration_attempts == 1);
    CHECK(output.registration_successes == 1);
    CHECK(output.last_pnp_inlier_count == 12);
    CHECK(output.camera_count == 3);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 07: deterministic noisy PnP registration. */
  {
    const BatchFixture noisy = make_batch_fixture(3, 12, false, true, false, false);
    const Lardon3DSparseIncrementalInput noisy_input = {
        403, 404, noisy.images.data(), noisy.images.size(),
        noisy.observations.data(), noisy.observations.size()};
    Lardon3DSparseIncrementalResult noisy_result = {};
    CHECK(lardon3d_sparse_incremental_run(&noisy_input, &parameters,
                                          &noisy_result) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(noisy_result.registration_attempts >= 1);
    CHECK(noisy_result.registration_successes >= 1);
    CHECK(noisy_result.last_pnp_inlier_count >=
          parameters.minimum_pnp_correspondences);
    CHECK(noisy_result.camera_count == 3);
    for (size_t index = 0; index < noisy_result.camera_count; ++index)
      for (double value : noisy_result.cameras[index].pose_cw.rotation_cw)
        CHECK(std::isfinite(value));
    lardon3d_sparse_incremental_result_destroy(&noisy_result);
  }

  /* CASE 08: deterministic PnP outliers. */
  {
    const BatchFixture outliers = make_batch_fixture(3, 12, false, false, true, false);
    const Lardon3DSparseIncrementalInput outlier_input = {
        405, 406, outliers.images.data(), outliers.images.size(),
        outliers.observations.data(), outliers.observations.size()};
    Lardon3DSparseIncrementalResult outlier_result = {};
    CHECK(lardon3d_sparse_incremental_run(&outlier_input, &parameters,
                                          &outlier_result) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(outlier_result.registration_attempts >= 1);
    CHECK(outlier_result.registration_successes == 1);
    CHECK(outlier_result.last_pnp_inlier_count >= 9);
    CHECK(outlier_result.camera_count == 3);
    lardon3d_sparse_incremental_result_destroy(&outlier_result);
  }

  /* CASE 09: PnP-stage registration failure. */
  {
    const BatchFixture failed = make_batch_fixture(3, 12, false, false, false, true);
    const Lardon3DSparseIncrementalInput failed_input = {
        407, 408, failed.images.data(), failed.images.size(),
        failed.observations.data(), failed.observations.size()};
    Lardon3DSparseIncrementalResult failed_result = {};
    CHECK(lardon3d_sparse_incremental_run(&failed_input, &parameters,
                                          &failed_result) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(failed_result.registration_attempts >= 1);
    CHECK(failed_result.registration_failures >= 1);
    CHECK(failed_result.registration_successes == 0);
    CHECK(failed_result.camera_count == 2);
    CHECK(failed_result.unregistered_image_count == 1);
    CHECK(failed_result.unregistered_images[0].image_id == 12);
    lardon3d_sparse_incremental_result_destroy(&failed_result);
  }

  /* CASE 10: insufficient PnP support skips the solver. */
  {
    const BatchFixture fixture = make_batch_fixture(3, 12, false, false, false, false);
    Lardon3DSparseIncrementalParameters insufficient = parameters;
    insufficient.minimum_pnp_correspondences = 20;
    insufficient.pnp.minimum_inliers = 20;
    const Lardon3DSparseIncrementalInput candidate = {
        409, 410, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &insufficient, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(output.camera_count == 2);
    CHECK(output.unregistered_image_count == 1);
    CHECK(output.registration_attempts == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 11: valid low-parallax seed reaches Gate C and is rejected. */
  {
    const BatchFixture fixture = make_batch_fixture(2, 12, false, false, false, false);
    Lardon3DSparseIncrementalParameters low_parallax = parameters;
    low_parallax.relative_pose.minimum_parallax_rad = 1.0;
    const Lardon3DSparseIncrementalInput candidate = {
        411, 412, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &low_parallax, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_FAILED);
    CHECK(output.seed_candidates_considered == 1);
    CHECK(output.last_seed_geometry_status == LARDON3D_SPARSE_GEOMETRY_LOW_PARALLAX ||
          output.last_seed_geometry_status == LARDON3D_SPARSE_GEOMETRY_DEGENERATE);
    CHECK(output.camera_count == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 12: pure rotation reaches relative pose handling and rejects. */
  {
    BatchFixture fixture = make_batch_fixture(2, 12, true, false, false, false);
    const double c = std::cos(0.25), s = std::sin(0.25);
    for (size_t track = 0; track < 12; ++track) {
      const Lardon3DSparseGeometryPoint3 point = {
          -1.0 + static_cast<double>(track % 4) * 0.5,
          -0.8 + static_cast<double>(track / 4) * 0.4,
          5.0 + static_cast<double>(track % 3) * 0.25};
      const Lardon3DSparseGeometryPose rotation = {
          {c, -s, 0, s, c, 0, 0, 0, 1}, {0, 0, 0}};
      const auto pixel = project(fixture.calibration, rotation, point);
      fixture.observations[track * 2 + 1].x = pixel.x;
      fixture.observations[track * 2 + 1].y = pixel.y;
    }
    const Lardon3DSparseIncrementalInput candidate = {
        413, 414, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_FAILED);
    CHECK(output.seed_candidates_considered == 1);
    CHECK(output.last_seed_geometry_status != LARDON3D_SPARSE_GEOMETRY_OK ||
          output.landmark_count == 0);
    CHECK(output.camera_count == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 13: planar seed is consumed as a Gate C degeneracy rejection. */
  {
    BatchFixture fixture = make_batch_fixture(2, 12, false, false, false, false);
    for (size_t track = 0; track < 12; ++track) {
      const Lardon3DSparseGeometryPoint3 point = {
          -1.0 + static_cast<double>(track % 4) * 0.5, 0.0, 5.0};
      const Lardon3DSparseGeometryPose shifted = {
          {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0}};
      const auto pixel = project(fixture.calibration, shifted, point);
      fixture.observations[track * 2 + 1].x = pixel.x;
      fixture.observations[track * 2 + 1].y = pixel.y;
    }
    const Lardon3DSparseIncrementalInput candidate = {
        415, 416, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_FAILED);
    CHECK(output.seed_candidates_considered == 1);
    CHECK(output.last_seed_geometry_status != LARDON3D_SPARSE_GEOMETRY_OK);
    CHECK(output.camera_count == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 14: far but recoverable scene remains finite and bounded. */
  {
    BatchFixture fixture = make_batch_fixture(2, 12, false, false, false, false);
    for (size_t track = 0; track < 12; ++track) {
      const Lardon3DSparseGeometryPoint3 point = {
          -1.0 + static_cast<double>(track % 4) * 0.5,
          -0.8 + static_cast<double>(track / 4) * 0.4, 100.0};
      const Lardon3DSparseGeometryPose shifted = {
          {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0}};
      const auto pixel = project(fixture.calibration, shifted, point);
      fixture.observations[track * 2 + 1].x = pixel.x;
      fixture.observations[track * 2 + 1].y = pixel.y;
    }
    const Lardon3DSparseIncrementalInput candidate = {
        417, 418, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.seed_image_a == 10 && output.seed_image_b == 11);
    CHECK(output.camera_count == 2);
    CHECK(output.landmark_count >= 6);
    for (size_t index = 0; index < output.landmark_count; ++index) {
      CHECK(std::isfinite(output.landmarks[index].point.x));
      CHECK(std::isfinite(output.landmarks[index].point.y));
      CHECK(std::isfinite(output.landmarks[index].point.z));
    }
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 15: disconnected graph discovery retains a singleton component. */
  {
    BatchFixture disconnected =
        make_batch_fixture(3, 12, false, false, false, false);
    disconnected.images.push_back({99, disconnected.calibration});
    const Lardon3DSparseIncrementalInput candidate = {
        4171, 4172, disconnected.images.data(), disconnected.images.size(),
        disconnected.observations.data(), disconnected.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(output.component_count == 2);
    CHECK(output.components[1].component_key == 99);
    CHECK(output.components[1].registered_image_count == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 16: two independently reconstructable components. */
  {
    BatchFixture fixture = make_batch_fixture(4, 24, false, false, false, false);
    std::vector<Lardon3DSparseIncrementalObservation> split;
    for (const auto &observation : fixture.observations) {
      const bool first = observation.track_id <= 12;
      const bool keep = first ? observation.image_id <= 11 : observation.image_id >= 12;
      if (!keep) continue;
      auto copy = observation;
      if (!first) {
        copy.image_id += 10;
        copy.feature_set_id += 10;
      }
      split.push_back(copy);
    }
    fixture.images = {{10, fixture.calibration}, {11, fixture.calibration},
                      {22, fixture.calibration}, {23, fixture.calibration}};
    fixture.observations = split;
    const Lardon3DSparseIncrementalInput candidate = {
        419, 420, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.component_count == 2);
    CHECK(output.components[0].component_key == 10);
    CHECK(output.components[1].component_key == 22);
    CHECK(output.components[0].registered_image_count == 2);
    CHECK(output.components[1].registered_image_count == 2);
    CHECK(output.components[0].landmark_count >= 6);
    CHECK(output.components[1].landmark_count >= 6);
    /* CASE 17: both independent component seed cameras fix identity gauge. */
    size_t identity_gauges = 0;
    for (size_t index = 0; index < output.camera_count; ++index) {
      if (output.cameras[index].image_id != output.cameras[index].component_key)
        continue;
      const Lardon3DSparseGeometryPose identity = {
          {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
      CHECK(std::memcmp(&output.cameras[index].pose_cw, &identity,
                        sizeof(identity)) == 0);
      ++identity_gauges;
    }
    CHECK(identity_gauges == 2);
    /* CASE 31: component output is canonical. */
    for (size_t index = 1; index < output.component_count; ++index)
      CHECK(output.components[index - 1].component_key <
            output.components[index].component_key);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 18: every unreconstructed image is explicit and component-keyed. */
  {
    BatchFixture incomplete =
        make_batch_fixture(3, 12, false, false, false, false);
    incomplete.images.push_back({99, incomplete.calibration});
    const Lardon3DSparseIncrementalInput candidate = {
        4191, 4192, incomplete.images.data(), incomplete.images.size(),
        incomplete.observations.data(), incomplete.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(output.unregistered_image_count == 1);
    CHECK(output.unregistered_images[0].image_id == 99);
    CHECK(output.unregistered_images[0].component_key == 99);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 19: Gate C returns exact cheirality failure for a finite point that
   * is in front of the seed cameras and behind the newly registered camera. */
  {
    const BatchFixture fixture = make_growth_fixture(19);
    const Lardon3DSparseIncrementalInput candidate = {
        421, 422, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.registration_successes >= 1);
    CHECK(output.triangulation_attempts > 0);
    CHECK(output.last_triangulation_status ==
          LARDON3D_SPARSE_GEOMETRY_CHEIRALITY_FAILED);
    CHECK(output.triangulation_failures == 0);
    CHECK(output.rejected_behind_camera > 0);
    CHECK(output.rejected_reprojection == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 20: a finite growth candidate is rejected by reprojection error. */
  {
    const BatchFixture fixture = make_growth_fixture(20);
    const Lardon3DSparseIncrementalInput candidate = {
        423, 424, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.registration_successes >= 1);
    CHECK(output.triangulation_attempts > 0);
    CHECK(output.rejected_reprojection > 0);
    CHECK(output.rejected_behind_camera == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 21: a growth Track reaches and fails triangulation. */
  {
    const BatchFixture fixture = make_growth_fixture(21);
    const Lardon3DSparseIncrementalInput candidate = {
        425, 426, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.registration_successes >= 1);
    CHECK(output.triangulation_attempts > 0);
    CHECK(output.triangulation_failures > 0);
    CHECK(output.rejected_behind_camera == 0);
    CHECK(output.rejected_reprojection == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 22: repeated image observations and feature references are rejected. */
  {
    const BatchFixture valid =
        make_batch_fixture(2, 12, false, false, false, false);
    std::vector<Lardon3DSparseIncrementalObservation> duplicate_image =
        valid.observations;
    duplicate_image.push_back(duplicate_image.front());
    duplicate_image.back().feature_index += 1;
    Lardon3DSparseIncrementalInput candidate = {
        4251, 4252, valid.images.data(), valid.images.size(),
        duplicate_image.data(), duplicate_image.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
    lardon3d_sparse_incremental_result_destroy(&output);

    std::vector<Lardon3DSparseIncrementalObservation> duplicate_feature =
        valid.observations;
    duplicate_feature[2].feature_set_id = duplicate_feature[0].feature_set_id;
    duplicate_feature[2].feature_index = duplicate_feature[0].feature_index;
    candidate.observations = duplicate_feature.data();
    candidate.observation_count = duplicate_feature.size();
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 23: one seed Track is atomically updated to all six cameras. */
  {
    const BatchFixture fixture = make_batch_fixture(6, 12, false, false, false, false);
    const Lardon3DSparseIncrementalInput candidate = {
        425, 426, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    size_t found = 0;
    for (size_t index = 0; index < output.landmark_count; ++index)
      if (output.landmarks[index].track_id == 1) {
        ++found;
        CHECK(output.landmarks[index].landmark_id == 1);
        CHECK(output.landmarks[index].observation_count == 6);
        CHECK(std::isfinite(output.landmarks[index].point.x));
        CHECK(std::isfinite(output.landmarks[index].point.y));
        CHECK(std::isfinite(output.landmarks[index].point.z));
      }
    CHECK(found == 1);
    size_t observation_count = 0;
    uint64_t previous_image_id = 0;
    for (size_t index = 0; index < output.observation_count; ++index) {
      if (output.observations[index].track_id != 1) continue;
      CHECK(output.observations[index].image_id > previous_image_id);
      CHECK(output.observations[index].position_in_track == observation_count);
      previous_image_id = output.observations[index].image_id;
      ++observation_count;
    }
    CHECK(observation_count == 6);
    CHECK(output.landmark_update_attempts > 0);
    CHECK(output.landmark_update_successes > 0);

    Lardon3DSparseIncrementalResult repeated = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &repeated) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(repeated.landmark_count == output.landmark_count);
    CHECK(repeated.observation_count == output.observation_count);
    CHECK(std::memcmp(repeated.landmarks, output.landmarks,
                      output.landmark_count * sizeof(*output.landmarks)) == 0);
    CHECK(std::memcmp(repeated.observations, output.observations,
                      output.observation_count * sizeof(*output.observations)) == 0);
    lardon3d_sparse_incremental_result_destroy(&repeated);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 23 rollback: one bad newly registered observation cannot replace the
   * valid two-view landmark. */
  {
    BatchFixture rollback =
        make_batch_fixture(3, 12, false, false, false, false);
    BatchFixture reference = rollback;
    reference.images.pop_back();
    reference.observations.erase(
        std::remove_if(reference.observations.begin(),
                       reference.observations.end(),
                       [](const auto &observation) {
                         return observation.image_id == 12;
                       }),
        reference.observations.end());
    const Lardon3DSparseIncrementalInput reference_input = {
        427, 428, reference.images.data(), reference.images.size(),
        reference.observations.data(), reference.observations.size()};
    Lardon3DSparseIncrementalResult reference_output = {};
    CHECK(lardon3d_sparse_incremental_run(
              &reference_input, &parameters, &reference_output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    Lardon3DSparseGeometryPoint3 reference_point = {};
    for (size_t index = 0; index < reference_output.landmark_count; ++index)
      if (reference_output.landmarks[index].track_id == 1)
        reference_point = reference_output.landmarks[index].point;
    for (auto &observation : rollback.observations)
      if (observation.track_id == 1 && observation.image_id == 12) {
        observation.x += 900.0;
        observation.y -= 700.0;
      }
    const Lardon3DSparseIncrementalInput candidate = {
        427, 428, rollback.images.data(), rollback.images.size(),
        rollback.observations.data(), rollback.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.landmark_update_failures > 0);
    size_t found = 0;
    for (size_t index = 0; index < output.landmark_count; ++index)
      if (output.landmarks[index].track_id == 1) {
        ++found;
        CHECK(output.landmarks[index].observation_count == 2);
        CHECK(std::memcmp(&output.landmarks[index].point, &reference_point,
                          sizeof(reference_point)) == 0);
      }
    CHECK(found == 1);
    lardon3d_sparse_incremental_result_destroy(&reference_output);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 24: a Track with no seed landmark becomes eligible after PnP. */
  {
    const BatchFixture growth = make_growth_fixture(0);
    const Lardon3DSparseIncrementalInput candidate = {
        4281, 4282, growth.images.data(), growth.images.size(),
        growth.observations.data(), growth.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.registration_successes == 1);
    size_t found = 0;
    for (size_t index = 0; index < output.landmark_count; ++index)
      if (output.landmarks[index].track_id == 13) ++found;
    CHECK(found == 1);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 25: a new growth landmark consumes all three registered views. */
  {
    const BatchFixture growth = make_growth_fixture(25);
    const Lardon3DSparseIncrementalInput candidate = {
        4283, 4284, growth.images.data(), growth.images.size(),
        growth.observations.data(), growth.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    size_t found = 0;
    for (size_t index = 0; index < output.landmark_count; ++index)
      if (output.landmarks[index].track_id == 13) {
        ++found;
        CHECK(output.landmarks[index].observation_count == 3);
      }
    CHECK(found == 1);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 26: point-only refinement is attempted and accepted. */
  {
    const BatchFixture growth = make_growth_fixture(25);
    const Lardon3DSparseIncrementalInput candidate = {
        4285, 4286, growth.images.data(), growth.images.size(),
        growth.observations.data(), growth.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.point_refinement_attempts > 0);
    CHECK(output.point_refinement_successes > 0);
    for (size_t index = 0; index < output.landmark_count; ++index) {
      CHECK(std::isfinite(output.landmarks[index].point.x));
      CHECK(std::isfinite(output.landmarks[index].point.y));
      CHECK(std::isfinite(output.landmarks[index].point.z));
    }
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 27: a valid seed remains intact when a full registration attempt
   * makes no progress. */
  {
    const BatchFixture blocked =
        make_batch_fixture(3, 12, false, false, false, true);
    const Lardon3DSparseIncrementalInput candidate = {
        429, 430, blocked.images.data(), blocked.images.size(),
        blocked.observations.data(), blocked.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(output.camera_count == 2);
    CHECK(output.landmark_count >= parameters.minimum_seed_landmarks);
    CHECK(output.registration_rounds == 1);
    CHECK(output.registration_attempts > 0);
    CHECK(output.registration_successes == 0);
    CHECK(output.no_growth_terminations == 1);
    CHECK(output.round_limit_terminations == 0);
    CHECK(output.unregistered_image_count == 1);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 28: natural completion registers every image without stop reason. */
  {
    const BatchFixture complete =
        make_batch_fixture(5, 12, false, false, false, false);
    const Lardon3DSparseIncrementalInput candidate = {
        4301, 4302, complete.images.data(), complete.images.size(),
        complete.observations.data(), complete.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(output.camera_count == 5);
    CHECK(output.registration_rounds == 3);
    CHECK(output.unregistered_image_count == 0);
    CHECK(output.no_growth_terminations == 0);
    CHECK(output.round_limit_terminations == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 29: the only available seed is attempted and exhausted. */
  {
    BatchFixture exhausted =
        make_batch_fixture(2, 12, false, false, false, false);
    for (auto &observation : exhausted.observations) {
      observation.x = 2100.0;
      observation.y = 1500.0;
    }
    const Lardon3DSparseIncrementalInput candidate = {
        4303, 4304, exhausted.images.data(), exhausted.images.size(),
        exhausted.observations.data(), exhausted.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_FAILED);
    CHECK(output.seed_candidates_available == 1);
    CHECK(output.seed_candidates_considered == 1);
    CHECK(output.camera_count == 0);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 30: one permitted round registers exactly one of three remaining
   * cameras and stops at the configured bound. */
  {
    const BatchFixture bounded =
        make_batch_fixture(5, 12, false, false, false, false);
    Lardon3DSparseIncrementalParameters one_round = parameters;
    one_round.maximum_registration_rounds = 1;
    const Lardon3DSparseIncrementalInput candidate = {
        431, 432, bounded.images.data(), bounded.images.size(),
        bounded.observations.data(), bounded.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &one_round, &output) ==
          LARDON3D_SPARSE_INCREMENTAL_PARTIAL);
    CHECK(output.registration_rounds == 1);
    CHECK(output.registration_successes == 1);
    CHECK(output.camera_count == 3);
    CHECK(output.unregistered_image_count == 2);
    CHECK(output.no_growth_terminations == 0);
    CHECK(output.round_limit_terminations == 1);

    Lardon3DSparseIncrementalResult repeated = {};
    CHECK(lardon3d_sparse_incremental_run(
              &candidate, &one_round, &repeated) == output.status);
    CHECK(repeated.registration_rounds == output.registration_rounds);
    CHECK(repeated.camera_count == output.camera_count);
    CHECK(repeated.unregistered_image_count == output.unregistered_image_count);
    CHECK(std::memcmp(repeated.cameras, output.cameras,
                      output.camera_count * sizeof(*output.cameras)) == 0);
    CHECK(std::memcmp(repeated.unregistered_images,
                      output.unregistered_images,
                      output.unregistered_image_count *
                          sizeof(*output.unregistered_images)) == 0);
    lardon3d_sparse_incremental_result_destroy(&repeated);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* CASE 32: camera output is strictly ordered by image identity. */
  /* CASE 33: landmark output is strictly ordered by Track identity. */
  /* CASE 34: a complete in-process rerun has identical scientific arrays and
   * deterministic diagnostics. */
  {
    BatchFixture canonical =
        make_batch_fixture(5, 12, false, false, false, false);
    std::reverse(canonical.images.begin(), canonical.images.end());
    std::reverse(canonical.observations.begin(), canonical.observations.end());
    const Lardon3DSparseIncrementalInput candidate = {
        433, 434, canonical.images.data(), canonical.images.size(),
        canonical.observations.data(), canonical.observations.size()};
    Lardon3DSparseIncrementalResult first = {}, second = {};
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &first) ==
          LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
    CHECK(lardon3d_sparse_incremental_run(&candidate, &parameters, &second) ==
          first.status);
    for (size_t index = 1; index < first.camera_count; ++index)
      CHECK(first.cameras[index - 1].image_id < first.cameras[index].image_id);
    for (size_t index = 1; index < first.landmark_count; ++index)
      CHECK(first.landmarks[index - 1].component_key <
                first.landmarks[index].component_key ||
            (first.landmarks[index - 1].component_key ==
                 first.landmarks[index].component_key &&
             first.landmarks[index - 1].track_id <
                 first.landmarks[index].track_id));
    CHECK(first.component_count == second.component_count);
    CHECK(first.camera_count == second.camera_count);
    CHECK(first.landmark_count == second.landmark_count);
    CHECK(first.observation_count == second.observation_count);
    CHECK(first.unregistered_image_count == second.unregistered_image_count);
    CHECK(std::memcmp(first.components, second.components,
                      first.component_count * sizeof(*first.components)) == 0);
    CHECK(std::memcmp(first.cameras, second.cameras,
                      first.camera_count * sizeof(*first.cameras)) == 0);
    CHECK(std::memcmp(first.landmarks, second.landmarks,
                      first.landmark_count * sizeof(*first.landmarks)) == 0);
    CHECK(std::memcmp(first.observations, second.observations,
                      first.observation_count * sizeof(*first.observations)) == 0);
    CHECK(first.seed_candidates_considered ==
          second.seed_candidates_considered);
    CHECK(first.registration_rounds == second.registration_rounds);
    CHECK(first.triangulation_attempts == second.triangulation_attempts);
    CHECK(first.landmark_update_successes ==
          second.landmark_update_successes);
    lardon3d_sparse_incremental_result_destroy(&second);
    lardon3d_sparse_incremental_result_destroy(&first);
  }
  return true;
}

static bool run_boundary_cases() {
  Lardon3DSparseIncrementalParameters parameters;
  CHECK(lardon3d_sparse_incremental_parameters_default(&parameters));
  parameters.minimum_seed_tracks = 6;
  parameters.minimum_seed_landmarks = 6;
  parameters.minimum_pnp_correspondences = 6;

  /* Input count validation: maximum_images LIMIT-1, LIMIT, LIMIT+1. */
  for (size_t camera_count : {size_t{2}, size_t{3}, size_t{4}}) {
    const BatchFixture fixture = make_batch_fixture(
        camera_count, 12, false, false, false, false);
    Lardon3DSparseIncrementalParameters bounded = parameters;
    bounded.maximum_images = 3;
    const Lardon3DSparseIncrementalInput input = {
        501, 502, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    const auto status =
        lardon3d_sparse_incremental_run(&input, &bounded, &output);
    CHECK(camera_count <= 3 ? status == LARDON3D_SPARSE_INCREMENTAL_COMPLETE
                            : status ==
                                  LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* Input count validation: maximum_tracks LIMIT-1, LIMIT, LIMIT+1. */
  for (size_t track_count : {size_t{11}, size_t{12}, size_t{13}}) {
    const BatchFixture fixture = make_batch_fixture(
        2, track_count, false, false, false, false);
    Lardon3DSparseIncrementalParameters bounded = parameters;
    bounded.maximum_tracks = 12;
    const Lardon3DSparseIncrementalInput input = {
        503, 504, fixture.images.data(), fixture.images.size(),
        fixture.observations.data(), fixture.observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    const auto status =
        lardon3d_sparse_incremental_run(&input, &bounded, &output);
    CHECK(track_count <= 12 ? status == LARDON3D_SPARSE_INCREMENTAL_COMPLETE
                            : status == LARDON3D_SPARSE_INCREMENTAL_FAILED);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* Input count validation: maximum_observations LIMIT-1, LIMIT, LIMIT+1. */
  const BatchFixture observation_fixture =
      make_batch_fixture(3, 12, false, false, false, false);
  for (size_t observation_count : {size_t{34}, size_t{35}, size_t{36}}) {
    std::vector<Lardon3DSparseIncrementalObservation> observations =
        observation_fixture.observations;
    if (observation_count <= 35) observations.erase(observations.begin() + 35);
    if (observation_count == 34) observations.erase(observations.begin() + 32);
    Lardon3DSparseIncrementalParameters bounded = parameters;
    bounded.maximum_observations = 35;
    bounded.maximum_tracks = 12;
    const Lardon3DSparseIncrementalInput input = {
        505, 506, observation_fixture.images.data(),
        observation_fixture.images.size(), observations.data(),
        observations.size()};
    Lardon3DSparseIncrementalResult output = {};
    const auto status =
        lardon3d_sparse_incremental_run(&input, &bounded, &output);
    CHECK(observation_count <= 35
              ? status == LARDON3D_SPARSE_INCREMENTAL_COMPLETE
              : status == LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
    lardon3d_sparse_incremental_result_destroy(&output);
  }

  /* Policy boundary: one seed candidate fails, two reach the valid fallback. */
  const BatchFixture seeds =
      make_batch_fixture(4, 12, true, false, false, false);
  const Lardon3DSparseIncrementalInput seed_input = {
      507, 508, seeds.images.data(), seeds.images.size(),
      seeds.observations.data(), seeds.observations.size()};
  Lardon3DSparseIncrementalParameters one_seed = parameters;
  one_seed.maximum_seed_candidates = 1;
  Lardon3DSparseIncrementalResult output = {};
  CHECK(lardon3d_sparse_incremental_run(&seed_input, &one_seed, &output) ==
        LARDON3D_SPARSE_INCREMENTAL_FAILED);
  CHECK(output.seed_candidates_considered == 1);
  lardon3d_sparse_incremental_result_destroy(&output);
  one_seed.maximum_seed_candidates = 2;
  CHECK(lardon3d_sparse_incremental_run(&seed_input, &one_seed, &output) ==
        LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
  CHECK(output.seed_candidates_considered == 2);
  lardon3d_sparse_incremental_result_destroy(&output);

  /* Policy boundary: at most two new landmarks are admitted in one round. */
  BatchFixture growth =
      make_batch_fixture(3, 12, false, false, false, false);
  growth.observations.erase(
      std::remove_if(
          growth.observations.begin(), growth.observations.end(),
          [](const auto &observation) {
            return observation.image_id == 12 && observation.track_id > 6;
          }),
      growth.observations.end());
  for (uint64_t track = 13; track <= 15; ++track) {
    const Lardon3DSparseGeometryPoint3 point = {
        -0.5 + static_cast<double>(track - 13) * 0.3, 0.2, 6.0};
    for (size_t camera : {size_t{0}, size_t{2}}) {
      const Lardon3DSparseGeometryPose pose = {
          {1, 0, 0, 0, 1, 0, 0, 0, 1},
          {static_cast<double>(camera), 0, 0}};
      const auto pixel = project(growth.calibration, pose, point);
      growth.observations.push_back(
          {track, 10 + camera, 100 + camera,
           static_cast<uint32_t>(track), 16, pixel.x, pixel.y});
    }
  }
  Lardon3DSparseIncrementalParameters two_landmarks = parameters;
  two_landmarks.maximum_landmarks_per_round = 2;
  const Lardon3DSparseIncrementalInput growth_input = {
      509, 510, growth.images.data(), growth.images.size(),
      growth.observations.data(), growth.observations.size()};
  CHECK(lardon3d_sparse_incremental_run(
            &growth_input, &two_landmarks, &output) ==
        LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
  CHECK(output.landmark_count == 14);
  lardon3d_sparse_incremental_result_destroy(&output);
  return true;
}

static bool run_failure_cases() {
  Lardon3DSparseIncrementalParameters parameters;
  CHECK(lardon3d_sparse_incremental_parameters_default(&parameters));
  parameters.minimum_seed_tracks = 6;
  parameters.minimum_seed_landmarks = 6;
  parameters.minimum_pnp_correspondences = 6;
  const BatchFixture base =
      make_batch_fixture(2, 12, false, false, false, false);
  const Lardon3DSparseIncrementalInput valid = {
      601, 602, base.images.data(), base.images.size(),
      base.observations.data(), base.observations.size()};

  auto expect_invalid = [&](const Lardon3DSparseIncrementalInput &input) {
    Lardon3DSparseIncrementalResult output = {};
    const bool accepted = lardon3d_sparse_incremental_run(
                              &input, &parameters, &output) ==
                          LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT &&
                          output.camera_count == 0 && output.landmark_count == 0;
    lardon3d_sparse_incremental_result_destroy(&output);
    return accepted;
  };

  Lardon3DSparseIncrementalResult output = {};
  CHECK(lardon3d_sparse_incremental_run(nullptr, &parameters, &output) ==
        LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
  CHECK(lardon3d_sparse_incremental_run(&valid, nullptr, &output) ==
        LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);
  CHECK(lardon3d_sparse_incremental_run(&valid, &parameters, nullptr) ==
        LARDON3D_SPARSE_INCREMENTAL_INVALID_ARGUMENT);

  Lardon3DSparseIncrementalInput invalid = valid;
  invalid.images = nullptr;
  CHECK(expect_invalid(invalid));
  invalid = valid;
  invalid.observations = nullptr;
  CHECK(expect_invalid(invalid));
  invalid = valid;
  invalid.observation_count = 0;
  CHECK(expect_invalid(invalid));
  invalid = valid;
  invalid.track_set_id = 0;
  CHECK(expect_invalid(invalid));
  invalid = valid;
  invalid.calibration_scope_id = 0;
  CHECK(expect_invalid(invalid));

  for (int kind = 0; kind < 5; ++kind) {
    std::vector<Lardon3DSparseIncrementalImage> images = base.images;
    if (kind == 0) images[0].calibration.fx = 0.0;
    if (kind == 1) images[0].calibration.fx = NAN;
    if (kind == 2) images[0].calibration.k1 = INFINITY;
    if (kind == 3) images[0].calibration.cx = -1.0;
    if (kind == 4) images[0].image_id = 0;
    invalid = valid;
    invalid.images = images.data();
    CHECK(expect_invalid(invalid));
  }

  for (int kind = 0; kind < 6; ++kind) {
    std::vector<Lardon3DSparseIncrementalObservation> observations =
        base.observations;
    if (kind == 0) observations[0].feature_set_id = 0;
    if (kind == 1)
      observations[0].feature_index = observations[0].feature_count;
    if (kind == 2) observations[0].x = NAN;
    if (kind == 3) observations[0].y = INFINITY;
    if (kind == 4) observations[0].image_id = 999;
    if (kind == 5) observations[0].track_id = 0;
    invalid = valid;
    invalid.observations = observations.data();
    CHECK(expect_invalid(invalid));
  }

  std::vector<Lardon3DSparseIncrementalObservation> singleton = {
      base.observations.front()};
  invalid = valid;
  invalid.observations = singleton.data();
  invalid.observation_count = singleton.size();
  CHECK(expect_invalid(invalid));

  /* Repeated failure, safe destruction, and a valid call after failure. */
  invalid = valid;
  invalid.calibration_scope_id = 0;
  CHECK(expect_invalid(invalid));
  CHECK(expect_invalid(invalid));
  CHECK(lardon3d_sparse_incremental_run(&valid, &parameters, &output) ==
        LARDON3D_SPARSE_INCREMENTAL_COMPLETE);
  lardon3d_sparse_incremental_result_destroy(&output);
  lardon3d_sparse_incremental_result_destroy(&output);
  lardon3d_sparse_incremental_result_destroy(nullptr);
  return true;
}

static void signature_mix(uint64_t *hash, const void *data, size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (size_t index = 0; index < size; ++index) {
    *hash ^= bytes[index];
    *hash *= UINT64_C(1099511628211);
  }
}

static bool scientific_signature(uint64_t *signature) {
  const BatchFixture fixture =
      make_batch_fixture(5, 12, false, false, false, false);
  Lardon3DSparseIncrementalParameters parameters;
  if (!lardon3d_sparse_incremental_parameters_default(&parameters)) return false;
  parameters.minimum_seed_tracks = 6;
  parameters.minimum_seed_landmarks = 6;
  parameters.minimum_pnp_correspondences = 6;
  const Lardon3DSparseIncrementalInput input = {
      701, 702, fixture.images.data(), fixture.images.size(),
      fixture.observations.data(), fixture.observations.size()};
  Lardon3DSparseIncrementalResult result = {};
  if (lardon3d_sparse_incremental_run(&input, &parameters, &result) !=
      LARDON3D_SPARSE_INCREMENTAL_COMPLETE)
    return false;

  uint64_t hash = UINT64_C(1469598103934665603);
#define MIX(value) signature_mix(&hash, &(value), sizeof(value))
  MIX(result.status);
  MIX(result.track_set_id);
  MIX(result.calibration_scope_id);
  for (size_t index = 0; index < result.component_count; ++index) {
    MIX(result.components[index].component_key);
    MIX(result.components[index].image_count);
    MIX(result.components[index].registered_image_count);
    MIX(result.components[index].landmark_count);
  }
  for (size_t index = 0; index < result.camera_count; ++index) {
    MIX(result.cameras[index].image_id);
    MIX(result.cameras[index].component_key);
    signature_mix(&hash, result.cameras[index].pose_cw.rotation_cw,
                  sizeof(result.cameras[index].pose_cw.rotation_cw));
    signature_mix(&hash, result.cameras[index].pose_cw.translation_cw,
                  sizeof(result.cameras[index].pose_cw.translation_cw));
  }
  for (size_t index = 0; index < result.landmark_count; ++index) {
    MIX(result.landmarks[index].landmark_id);
    MIX(result.landmarks[index].track_id);
    MIX(result.landmarks[index].component_key);
    MIX(result.landmarks[index].point.x);
    MIX(result.landmarks[index].point.y);
    MIX(result.landmarks[index].point.z);
    MIX(result.landmarks[index].reprojection_rmse_px);
    MIX(result.landmarks[index].reprojection_median_px);
    MIX(result.landmarks[index].observation_count);
  }
  for (size_t index = 0; index < result.observation_count; ++index) {
    MIX(result.observations[index].landmark_id);
    MIX(result.observations[index].track_id);
    MIX(result.observations[index].image_id);
    MIX(result.observations[index].feature_set_id);
    MIX(result.observations[index].feature_index);
    MIX(result.observations[index].position_in_track);
  }
  MIX(result.seed_candidates_available);
  MIX(result.seed_candidates_considered);
  MIX(result.seed_image_a);
  MIX(result.seed_image_b);
  MIX(result.registration_rounds);
  MIX(result.registration_attempts);
  MIX(result.registration_successes);
  MIX(result.triangulation_attempts);
  MIX(result.landmark_update_attempts);
  MIX(result.landmark_update_successes);
  MIX(result.point_refinement_attempts);
  MIX(result.point_refinement_successes);
#undef MIX
  lardon3d_sparse_incremental_result_destroy(&result);
  *signature = hash;
  return true;
}

static bool run_case_35(const char *executable) {
  /* CASE 35: twenty fresh executable images emit one scientific signature. */
  uint64_t expected = 0;
  for (size_t run = 0; run < 20; ++run) {
    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      close(descriptors[0]);
      CHECK(dup2(descriptors[1], STDOUT_FILENO) >= 0);
      close(descriptors[1]);
      execl(executable, executable, "--scientific-signature", nullptr);
      _exit(127);
    }
    close(descriptors[1]);
    uint64_t actual = 0;
    size_t received = 0;
    while (received < sizeof(actual)) {
      const ssize_t count = read(
          descriptors[0], reinterpret_cast<unsigned char *>(&actual) + received,
          sizeof(actual) - received);
      if (count <= 0) break;
      received += static_cast<size_t>(count);
    }
    close(descriptors[0]);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(received == sizeof(actual));
    if (run == 0)
      expected = actual;
    else
      CHECK(actual == expected);
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--scientific-signature") == 0) {
    uint64_t signature = 0;
    if (!scientific_signature(&signature)) return EXIT_FAILURE;
    return write(STDOUT_FILENO, &signature, sizeof(signature)) ==
                   static_cast<ssize_t>(sizeof(signature))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }
  return run_test() && run_batch_cases() && run_boundary_cases() &&
                 run_failure_cases() && run_case_35(argv[0])
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
