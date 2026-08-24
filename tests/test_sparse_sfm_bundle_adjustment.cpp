#include <lardon3d/sparse_sfm_bundle_adjustment.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#define CHECK(value)                                                            \
  do {                                                                          \
    if (!(value)) {                                                             \
      std::fprintf(stderr, "bundle adjustment check failed at line %d: %s\n", \
                   __LINE__, #value);                                           \
      return 1;                                                                 \
    }                                                                           \
  } while (0)

extern "C" bool lardon3d_sparse_bundle_adjustment_test_termination_accepted(
    int value);
extern "C" int lardon3d_sparse_bundle_adjustment_test_prepare(
    const Lardon3DSparseBundleAdjustmentInput *input,
    Lardon3DSparseBundleAdjustmentComponentDiagnostic *diagnostics,
    size_t diagnostic_capacity, Lardon3DSparseIncrementalObservation *resolved,
    size_t resolved_capacity, size_t *diagnostic_count, size_t *resolved_count);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_project(
    const Lardon3DSparseGeometryCalibration *calibration,
    const Lardon3DSparseGeometryPose *pose,
    const Lardon3DSparseGeometryPoint3 *point,
    Lardon3DSparseGeometryPoint2 *pixel);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_metrics(
    const double *residuals, size_t count, double *rmse, double *huber_cost);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_cost_acceptable(
    double initial_cost, double final_cost);

static Lardon3DSparseGeometryCalibration calibration() {
  return {1280, 960, 800.0, 810.0, 640.0, 480.0, 0.0, 0.0, 0.0, 0.0};
}

static int test_helpers() {
  const Lardon3DSparseGeometryPose pose = {
      {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}};
  const Lardon3DSparseGeometryPoint3 point = {0.2, -0.1, 2.0};
  auto camera = calibration();
  Lardon3DSparseGeometryPoint2 pixel = {};
  CHECK(lardon3d_sparse_bundle_adjustment_test_project(&camera, &pose, &point, &pixel));
  CHECK(std::abs(pixel.x - 720.0) < 1e-12);
  CHECK(std::abs(pixel.y - 439.5) < 1e-12);

  camera.k1 = 0.1;
  camera.k2 = -0.02;
  camera.p1 = 0.003;
  camera.p2 = -0.004;
  CHECK(lardon3d_sparse_bundle_adjustment_test_project(&camera, &pose, &point, &pixel));
  const double xn = 0.1;
  const double yn = -0.05;
  const double r2 = xn * xn + yn * yn;
  const double radial = 1.0 + camera.k1 * r2 + camera.k2 * r2 * r2;
  const double xd = xn * radial + 2.0 * camera.p1 * xn * yn +
                    camera.p2 * (r2 + 2.0 * xn * xn);
  const double yd = yn * radial + camera.p1 * (r2 + 2.0 * yn * yn) +
                    2.0 * camera.p2 * xn * yn;
  CHECK(std::abs(pixel.x - (camera.fx * xd + camera.cx)) < 1e-12);
  CHECK(std::abs(pixel.y - (camera.fy * yd + camera.cy)) < 1e-12);

  double residuals[4] = {3.0, 4.0, 0.0, 0.0};
  double rmse = 0.0;
  double cost = 0.0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_metrics(residuals, 2, &rmse, &cost));
  CHECK(std::abs(rmse - std::sqrt(12.5)) < 1e-12);
  CHECK(cost == 8.0);
  double boundary[2] = {2.0, 0.0};
  CHECK(lardon3d_sparse_bundle_adjustment_test_metrics(boundary, 1, &rmse, &cost));
  CHECK(rmse == 2.0 && cost == 2.0);
  CHECK(lardon3d_sparse_bundle_adjustment_test_cost_acceptable(10.0, 10.0 + 1e-11));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_cost_acceptable(10.0,
                                                                 10.0 + 2e-11));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_cost_acceptable(NAN, 0.0));

  residuals[0] = std::numeric_limits<double>::infinity();
  CHECK(!lardon3d_sparse_bundle_adjustment_test_metrics(residuals, 2, &rmse, &cost));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_project(nullptr, &pose, &point, &pixel));
  Lardon3DSparseGeometryPoint3 behind = {0.0, 0.0, -1.0};
  CHECK(!lardon3d_sparse_bundle_adjustment_test_project(&camera, &pose, &behind,
                                                         &pixel));
  Lardon3DSparseGeometryPoint3 depth_boundary = {0.0, 0.0, 1e-9};
  CHECK(!lardon3d_sparse_bundle_adjustment_test_project(
      &camera, &pose, &depth_boundary, &pixel));
  depth_boundary.z = std::nextafter(1e-9, 1.0);
  CHECK(lardon3d_sparse_bundle_adjustment_test_project(
      &camera, &pose, &depth_boundary, &pixel));
  auto invalid_pose = pose;
  invalid_pose.rotation_cw[0] += 1e-6;
  CHECK(!lardon3d_sparse_bundle_adjustment_test_project(
      &camera, &invalid_pose, &point, &pixel));
  auto invalid_calibration = camera;
  invalid_calibration.cx = -1.0;
  CHECK(!lardon3d_sparse_bundle_adjustment_test_project(
      &invalid_calibration, &pose, &point, &pixel));

  CHECK(lardon3d_sparse_bundle_adjustment_test_termination_accepted(0));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_termination_accepted(1));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_termination_accepted(2));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_termination_accepted(3));
  return 0;
}

struct Fixture {
  Lardon3DSparseIncrementalImage images[3];
  Lardon3DSparseIncrementalObservation input_observations[3];
  Lardon3DSparseIncrementalComponent components[1];
  Lardon3DSparseIncrementalCamera cameras[3];
  Lardon3DSparseIncrementalLandmark landmarks[1];
  Lardon3DSparseIncrementalLandmarkObservation result_observations[3];
  Lardon3DSparseIncrementalUnregisteredImage unregistered_images[1];
  Lardon3DSparseIncrementalResult result;
  Lardon3DSparseBundleAdjustmentInput input;
};

static Fixture fixture() {
  Fixture value = {};
  const auto intrinsic = calibration();
  value.images[0] = {10, intrinsic};
  value.images[1] = {20, intrinsic};
  value.images[2] = {30, intrinsic};
  value.input_observations[0] = {7, 10, 100, 1, 2, 650.0, 480.0};
  value.input_observations[1] = {7, 20, 200, 1, 2, 500.0, 480.0};
  value.input_observations[2] = {7, 30, 300, 1, 2, 700.0, 480.0};
  value.components[0] = {10, 3, 3, 1};
  const double identity[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  std::memcpy(value.cameras[0].pose_cw.rotation_cw, identity, sizeof(identity));
  std::memcpy(value.cameras[1].pose_cw.rotation_cw, identity, sizeof(identity));
  std::memcpy(value.cameras[2].pose_cw.rotation_cw, identity, sizeof(identity));
  value.cameras[0].image_id = 10;
  value.cameras[1].image_id = 20;
  value.cameras[2].image_id = 30;
  for (auto &camera : value.cameras) camera.component_key = 10;
  value.cameras[1].pose_cw.translation_cw[0] = -2.0;
  value.cameras[1].pose_cw.translation_cw[1] = -2.0;
  value.cameras[2].pose_cw.translation_cw[0] = 2.0;
  value.cameras[2].pose_cw.translation_cw[1] = 2.0;
  value.landmarks[0] = {1, 7, 10, {0.0, 0.0, 5.0}, 0.5, 0.4, 3};
  for (size_t index = 0; index < 3; ++index) {
    value.result_observations[index] = {
        1, 7, value.images[index].image_id,
        value.input_observations[index].feature_set_id,
        value.input_observations[index].feature_index,
        static_cast<uint32_t>(index)};
  }
  value.result.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
  value.result.track_set_id = 77;
  value.result.calibration_scope_id = 88;
  value.result.component_count = 1;
  value.result.camera_count = 3;
  value.result.landmark_count = 1;
  value.result.observation_count = 3;
  value.unregistered_images[0] = {99, 10};
  value.result.unregistered_image_count = 1;
  value.result.seed_candidates_considered = 3;
  value.result.seed_candidates_available = 4;
  value.result.seed_image_a = 10;
  value.result.seed_image_b = 20;
  value.result.last_seed_geometry_status = 2;
  value.result.last_seed_parallax_rad = 0.25;
  value.result.registration_rounds = 5;
  value.result.registration_attempts = 6;
  value.result.registration_successes = 3;
  value.result.registration_failures = 3;
  value.result.last_pnp_inlier_count = 8;
  value.result.triangulation_attempts = 9;
  value.result.triangulation_failures = 1;
  value.result.rejected_behind_camera = 2;
  value.result.rejected_reprojection = 3;
  value.result.rejected_landmarks = 4;
  value.result.last_triangulation_status = 5;
  value.result.landmark_update_attempts = 6;
  value.result.landmark_update_successes = 7;
  value.result.landmark_update_failures = 8;
  value.result.no_growth_terminations = 9;
  value.result.round_limit_terminations = 10;
  value.result.point_refinement_attempts = 11;
  value.result.point_refinement_successes = 12;
  return value;
}

static void bind_fixture(Fixture *value) {
  value->result.components = value->components;
  value->result.cameras = value->cameras;
  value->result.landmarks = value->landmarks;
  value->result.observations = value->result_observations;
  value->result.unregistered_images = value->unregistered_images;
  value->input = {&value->result, value->images, 3, value->input_observations, 3};
}

static int test_preparation() {
  Fixture value = fixture();
  bind_fixture(&value);
  const Fixture original = value;
  Lardon3DSparseBundleAdjustmentComponentDiagnostic diagnostics[1] = {};
  Lardon3DSparseIncrementalObservation resolved[3] = {};
  size_t diagnostic_count = 0;
  size_t resolved_count = 0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 0);
  CHECK(diagnostic_count == 1 && resolved_count == 3);
  CHECK(resolved[1].image_id == 20 && resolved[1].x == 500.0);
  CHECK(diagnostics[0].eligible);
  CHECK(diagnostics[0].pose_anchor_image_id == 10);
  CHECK(diagnostics[0].scale_anchor_image_id == 20);
  CHECK(diagnostics[0].scale_axis ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_X);
  CHECK(diagnostics[0].termination ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_NOT_RUN);
  CHECK(!diagnostics[0].has_costs && !diagnostics[0].has_rmse);
  CHECK(!diagnostics[0].accepted);
  CHECK(std::memcmp(&value.result, &original.result, sizeof(value.result)) == 0);
  CHECK(std::memcmp(value.images, original.images, sizeof(value.images)) == 0);
  CHECK(std::memcmp(value.input_observations, original.input_observations,
                    sizeof(value.input_observations)) == 0);
  CHECK(std::memcmp(value.cameras, original.cameras, sizeof(value.cameras)) == 0);
  CHECK(std::memcmp(value.landmarks, original.landmarks, sizeof(value.landmarks)) == 0);
  CHECK(std::memcmp(value.components, original.components, sizeof(value.components)) == 0);
  CHECK(std::memcmp(value.result_observations, original.result_observations,
                    sizeof(value.result_observations)) == 0);
  CHECK(std::memcmp(value.unregistered_images, original.unregistered_images,
                    sizeof(value.unregistered_images)) == 0);

  value = fixture();
  bind_fixture(&value);
  value.cameras[1].pose_cw.translation_cw[0] = -1e-9;
  value.cameras[1].pose_cw.translation_cw[1] = 0.0;
  value.cameras[2].pose_cw.translation_cw[0] = 1e-9;
  value.cameras[2].pose_cw.translation_cw[1] = 0.0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 0);
  CHECK(!diagnostics[0].eligible);
  CHECK(diagnostics[0].rejection_reason ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_GAUGE_DEGENERATE);
  return 0;
}

static int test_invalid() {
  Lardon3DSparseBundleAdjustmentComponentDiagnostic diagnostics[1] = {};
  Lardon3DSparseIncrementalObservation resolved[3] = {};
  size_t diagnostic_count = 0;
  size_t resolved_count = 0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            nullptr, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);
  Fixture value = fixture();
  bind_fixture(&value);
  value.input_observations[1].feature_set_id = 999;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);
  value = fixture();
  bind_fixture(&value);
  value.input_observations[1].image_id = 30;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);
  value = fixture();
  bind_fixture(&value);
  value.input_observations[1].x = NAN;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);
  value = fixture();
  bind_fixture(&value);
  value.input_observations[1].feature_set_id = value.input_observations[0].feature_set_id;
  value.input_observations[1].feature_index = value.input_observations[0].feature_index;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);
  value = fixture();
  bind_fixture(&value);
  std::swap(value.cameras[0], value.cameras[1]);
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);
  value = fixture();
  bind_fixture(&value);
  value.input.image_count = 4097;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 1);

  Lardon3DSparseIncrementalResult empty = {};
  empty.status = LARDON3D_SPARSE_INCREMENTAL_FAILED;
  Lardon3DSparseBundleAdjustmentInput empty_input = {&empty, nullptr, 0, nullptr, 0};
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &empty_input, nullptr, 0, nullptr, 0, &diagnostic_count,
            &resolved_count) == 0);
  CHECK(diagnostic_count == 0 && resolved_count == 0);
  return 0;
}

static int test_multiple_components() {
  const auto intrinsic = calibration();
  Lardon3DSparseIncrementalImage images[4] = {
      {10, intrinsic}, {20, intrinsic}, {30, intrinsic}, {40, intrinsic}};
  Lardon3DSparseIncrementalObservation source_observations[4] = {
      {7, 10, 100, 1, 2, 640.0, 480.0},
      {7, 20, 200, 1, 2, 600.0, 480.0},
      {8, 30, 300, 1, 2, 640.0, 480.0},
      {8, 40, 400, 1, 2, 600.0, 480.0}};
  Lardon3DSparseIncrementalComponent components[2] = {
      {10, 2, 2, 1}, {30, 2, 2, 1}};
  Lardon3DSparseIncrementalCamera cameras[4] = {};
  const double identity[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  for (size_t index = 0; index < 4; ++index) {
    cameras[index].image_id = images[index].image_id;
    cameras[index].component_key = index < 2 ? 10 : 30;
    std::memcpy(cameras[index].pose_cw.rotation_cw, identity, sizeof(identity));
  }
  cameras[1].pose_cw.translation_cw[0] = -1.0;
  cameras[3].pose_cw.translation_cw[1] = -1.0;
  Lardon3DSparseIncrementalLandmark landmarks[2] = {
      {1, 7, 10, {0.0, 0.0, 5.0}, 0.5, 0.4, 2},
      {2, 8, 30, {0.0, 0.0, 6.0}, 0.5, 0.4, 2}};
  Lardon3DSparseIncrementalLandmarkObservation observations[4] = {
      {1, 7, 10, 100, 1, 0}, {1, 7, 20, 200, 1, 1},
      {2, 8, 30, 300, 1, 0}, {2, 8, 40, 400, 1, 1}};
  Lardon3DSparseIncrementalResult result = {};
  result.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
  result.components = components;
  result.component_count = 2;
  result.cameras = cameras;
  result.camera_count = 4;
  result.landmarks = landmarks;
  result.landmark_count = 2;
  result.observations = observations;
  result.observation_count = 4;
  Lardon3DSparseBundleAdjustmentInput input = {
      &result, images, 4, source_observations, 4};
  Lardon3DSparseBundleAdjustmentComponentDiagnostic diagnostics[2] = {};
  Lardon3DSparseIncrementalObservation resolved[4] = {};
  size_t diagnostic_count = 0;
  size_t resolved_count = 0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &input, diagnostics, 2, resolved, 4, &diagnostic_count,
            &resolved_count) == 0);
  CHECK(diagnostic_count == 2 && resolved_count == 4);
  CHECK(diagnostics[0].component_key == 10 && diagnostics[0].observation_count == 2);
  CHECK(diagnostics[1].component_key == 30 && diagnostics[1].observation_count == 2);
  CHECK(diagnostics[0].scale_axis == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_X);
  CHECK(diagnostics[1].scale_axis == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_Y);
  return 0;
}

int main() {
  if (test_helpers() != 0) return 1;
  if (test_preparation() != 0) return 1;
  if (test_invalid() != 0) return 1;
  return test_multiple_components();
}
