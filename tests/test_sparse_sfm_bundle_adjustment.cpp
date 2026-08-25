#include <lardon3d/sparse_sfm_bundle_adjustment.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

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
extern "C" uint32_t lardon3d_sparse_bundle_adjustment_test_structural_mask(
    size_t camera_count, size_t landmark_count, size_t observation_count,
    size_t pose_anchor, const size_t *camera_indices,
    const size_t *landmark_indices, size_t edge_count);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_candidate_decision(
    int termination, bool final_metrics_valid, bool gauge_valid,
    double initial_cost, double final_cost, bool *has_metrics,
    Lardon3DSparseBundleAdjustmentRejectionReason *reason);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_counts_valid(
    size_t image_count, size_t camera_count, size_t landmark_count,
    size_t result_observation_count, size_t input_observation_count);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_candidate_publication(
    bool final_metrics_valid, double initial_cost, double final_cost,
    double original_x, double candidate_x, double *published_x,
    bool *has_metrics, Lardon3DSparseBundleAdjustmentRejectionReason *reason);
extern "C" int lardon3d_sparse_bundle_adjustment_test_internal_underconstraint();
extern "C" bool lardon3d_sparse_bundle_adjustment_test_parameter_ordering(
    const Lardon3DSparseBundleAdjustmentInput *input, int *kinds,
    uint64_t *identities, int *groups, bool *constants, int *subset_axes,
    size_t capacity, size_t *count);
extern "C" bool lardon3d_sparse_bundle_adjustment_test_invalid_candidate(
    const Lardon3DSparseBundleAdjustmentInput *input, double *published_z,
    bool *has_metrics, Lardon3DSparseBundleAdjustmentRejectionReason *reason);

static Lardon3DSparseGeometryCalibration calibration() {
  return {1280, 960, 800.0, 810.0, 640.0, 480.0, 0.0, 0.0, 0.0, 0.0};
}

static double camera_center_x(const Lardon3DSparseGeometryPose &pose) {
  return -(pose.rotation_cw[0] * pose.translation_cw[0] +
           pose.rotation_cw[3] * pose.translation_cw[1] +
           pose.rotation_cw[6] * pose.translation_cw[2]);
}

static bool close_scalar(double a, double b) {
  return std::isfinite(a) && std::isfinite(b) &&
         std::abs(a - b) <=
             1e-12 * std::max({1.0, std::abs(a), std::abs(b)});
}

static bool close_rotation(const double a[9], const double b[9]) {
  bool identical = true;
  for (size_t index = 0; index < 9; ++index)
    identical = identical && a[index] == b[index];
  if (identical) return true;
  double trace = 0.0;
  for (size_t row = 0; row < 3; ++row)
    for (size_t column = 0; column < 3; ++column)
      trace += a[row * 3 + column] * b[row * 3 + column];
  const double cosine = std::max(-1.0, std::min(1.0, (trace - 1.0) * 0.5));
  return std::acos(cosine) <= 1e-12;
}

static bool equal_results(const Lardon3DSparseBundleAdjustmentResult &a,
                          const Lardon3DSparseBundleAdjustmentResult &b) {
  if (a.status != b.status || a.component_count != b.component_count ||
      a.camera_count != b.camera_count || a.landmark_count != b.landmark_count ||
      a.observation_count != b.observation_count)
    return false;
  for (size_t index = 0; index < a.component_count; ++index) {
    const auto &ac = a.components[index];
    const auto &bc = b.components[index];
    if (ac.component_key != bc.component_key || ac.image_count != bc.image_count ||
        ac.registered_image_count != bc.registered_image_count ||
        ac.landmark_count != bc.landmark_count)
      return false;
    const auto &ad = a.diagnostics[index];
    const auto &bd = b.diagnostics[index];
    if (ad.component_key != bd.component_key || ad.camera_count != bd.camera_count ||
        ad.landmark_count != bd.landmark_count ||
        ad.observation_count != bd.observation_count || ad.eligible != bd.eligible ||
        ad.has_anchors != bd.has_anchors ||
        ad.pose_anchor_image_id != bd.pose_anchor_image_id ||
        ad.scale_anchor_image_id != bd.scale_anchor_image_id ||
        ad.scale_axis != bd.scale_axis || ad.has_costs != bd.has_costs ||
        ad.has_rmse != bd.has_rmse || ad.iteration_count != bd.iteration_count ||
        ad.termination != bd.termination || ad.accepted != bd.accepted ||
        ad.rejection_reason != bd.rejection_reason)
      return false;
    if (ad.has_costs &&
        (!close_scalar(ad.initial_robust_cost, bd.initial_robust_cost) ||
         !close_scalar(ad.final_robust_cost, bd.final_robust_cost)))
      return false;
    if (ad.has_rmse &&
        (!close_scalar(ad.initial_reprojection_rmse_px,
                       bd.initial_reprojection_rmse_px) ||
         !close_scalar(ad.final_reprojection_rmse_px,
                       bd.final_reprojection_rmse_px)))
      return false;
  }
  for (size_t index = 0; index < a.camera_count; ++index) {
    if (a.cameras[index].image_id != b.cameras[index].image_id ||
        a.cameras[index].component_key != b.cameras[index].component_key ||
        !close_rotation(a.cameras[index].pose_cw.rotation_cw,
                        b.cameras[index].pose_cw.rotation_cw))
      return false;
    for (size_t item = 0; item < 3; ++item)
      if (!close_scalar(a.cameras[index].pose_cw.translation_cw[item],
                        b.cameras[index].pose_cw.translation_cw[item]))
        return false;
  }
  for (size_t index = 0; index < a.landmark_count; ++index) {
    const auto &al = a.landmarks[index];
    const auto &bl = b.landmarks[index];
    if (al.landmark_id != bl.landmark_id || al.track_id != bl.track_id ||
        al.component_key != bl.component_key ||
        al.observation_count != bl.observation_count ||
        !close_scalar(al.point.x, bl.point.x) ||
        !close_scalar(al.point.y, bl.point.y) ||
        !close_scalar(al.point.z, bl.point.z) ||
        !close_scalar(al.reprojection_rmse_px, bl.reprojection_rmse_px) ||
        !close_scalar(al.reprojection_median_px, bl.reprojection_median_px))
      return false;
  }
  for (size_t index = 0; index < a.observation_count; ++index) {
    const auto &ao = a.observations[index];
    const auto &bo = b.observations[index];
    if (ao.landmark_id != bo.landmark_id || ao.track_id != bo.track_id ||
        ao.image_id != bo.image_id || ao.feature_set_id != bo.feature_set_id ||
        ao.feature_index != bo.feature_index ||
        ao.position_in_track != bo.position_in_track)
      return false;
  }
  return true;
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

  bool has_metrics = false;
  Lardon3DSparseBundleAdjustmentRejectionReason reason =
      LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONE;
  CHECK(lardon3d_sparse_bundle_adjustment_test_candidate_decision(
      0, true, true, 10.0, 9.0, &has_metrics, &reason));
  CHECK(has_metrics && reason == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONE);
  CHECK(!lardon3d_sparse_bundle_adjustment_test_candidate_decision(
      1, false, true, 10.0, 0.0, &has_metrics, &reason));
  CHECK(!has_metrics && reason ==
                            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NO_CONVERGENCE);
  CHECK(!lardon3d_sparse_bundle_adjustment_test_candidate_decision(
      2, false, true, 10.0, 0.0, &has_metrics, &reason));
  CHECK(!has_metrics && reason ==
                            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_SOLVER_FAILURE);
  CHECK(!lardon3d_sparse_bundle_adjustment_test_candidate_decision(
      0, false, true, 10.0, 0.0, &has_metrics, &reason));
  CHECK(!has_metrics && reason ==
                            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE);
  CHECK(!lardon3d_sparse_bundle_adjustment_test_candidate_decision(
      0, true, true, 10.0, 11.0, &has_metrics, &reason));
  CHECK(has_metrics && reason ==
                           LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_COST_REGRESSION);
  double published_x = 0.0;
  CHECK(!lardon3d_sparse_bundle_adjustment_test_candidate_publication(
      false, 10.0, 0.0, 3.0, 9.0, &published_x, &has_metrics, &reason));
  CHECK(published_x == 3.0 && !has_metrics &&
        reason == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE);
  CHECK(!lardon3d_sparse_bundle_adjustment_test_candidate_publication(
      true, 10.0, 11.0, 3.0, 9.0, &published_x, &has_metrics, &reason));
  CHECK(published_x == 3.0 && has_metrics &&
        reason == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_COST_REGRESSION);

  CHECK(lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4095, 4095, 249999, 999999, 999999));
  CHECK(lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4096, 4096, 250000, 1000000, 1000000));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4097, 4096, 250000, 1000000, 1000000));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4096, 4097, 250000, 1000000, 1000000));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4096, 4096, 250001, 1000000, 1000000));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4096, 4096, 250000, 1000001, 1000000));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      4096, 4096, 250000, 1000000, 1000001));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      0, 0, 0, std::numeric_limits<size_t>::max(), 0));
  CHECK(!lardon3d_sparse_bundle_adjustment_test_counts_valid(
      std::numeric_limits<size_t>::max(), 0, 0, 0, 0));
  CHECK(lardon3d_sparse_bundle_adjustment_test_internal_underconstraint() ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INTERNAL_ERROR);
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

  value = fixture();
  bind_fixture(&value);
  value.cameras[1].pose_cw.translation_cw[0] =
      -std::nextafter(1e-9, 1.0);
  value.cameras[1].pose_cw.translation_cw[1] = 0.0;
  value.cameras[2].pose_cw.translation_cw[0] =
      std::nextafter(1e-9, 1.0);
  value.cameras[2].pose_cw.translation_cw[1] = 0.0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_prepare(
            &value.input, diagnostics, 1, resolved, 3, &diagnostic_count,
            &resolved_count) == 0);
  CHECK(diagnostics[0].eligible);
  CHECK(diagnostics[0].scale_anchor_image_id == 20);
  CHECK(diagnostics[0].scale_axis ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_AXIS_X);
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

static int test_underconstraint_rules() {
  std::vector<size_t> cameras;
  std::vector<size_t> landmarks;
  auto complete_edges = [&](size_t camera_count, size_t landmark_count) {
    cameras.clear();
    landmarks.clear();
    for (size_t camera = 0; camera < camera_count; ++camera) {
      for (size_t landmark = 0; landmark < landmark_count; ++landmark) {
        cameras.push_back(camera);
        landmarks.push_back(landmark);
      }
    }
  };

  complete_edges(4, 3);
  CHECK(lardon3d_sparse_bundle_adjustment_test_structural_mask(
            4, 3, cameras.size(), 0, cameras.data(), landmarks.data(),
            cameras.size()) == 1U);

  complete_edges(3, 6);
  for (size_t index = cameras.size(); index-- > 0;) {
    if (landmarks[index] == 0 && cameras[index] != 0) {
      cameras.erase(cameras.begin() + static_cast<ptrdiff_t>(index));
      landmarks.erase(landmarks.begin() + static_cast<ptrdiff_t>(index));
    }
  }
  CHECK((lardon3d_sparse_bundle_adjustment_test_structural_mask(
             3, 6, cameras.size(), 0, cameras.data(), landmarks.data(),
             cameras.size()) & 2U) != 0);

  complete_edges(3, 10);
  for (size_t index = cameras.size(); index-- > 0;) {
    if (cameras[index] == 1 && landmarks[index] >= 2) {
      cameras.erase(cameras.begin() + static_cast<ptrdiff_t>(index));
      landmarks.erase(landmarks.begin() + static_cast<ptrdiff_t>(index));
    }
  }
  CHECK(lardon3d_sparse_bundle_adjustment_test_structural_mask(
            3, 10, cameras.size(), 0, cameras.data(), landmarks.data(),
            cameras.size()) == 4U);

  cameras.clear();
  landmarks.clear();
  for (size_t group = 0; group < 2; ++group) {
    for (size_t camera = group * 3; camera < group * 3 + 3; ++camera) {
      for (size_t landmark = group * 6; landmark < group * 6 + 6; ++landmark) {
        cameras.push_back(camera);
        landmarks.push_back(landmark);
      }
    }
  }
  CHECK(lardon3d_sparse_bundle_adjustment_test_structural_mask(
            6, 12, cameras.size(), 0, cameras.data(), landmarks.data(),
            cameras.size()) == 8U);
  return 0;
}

struct ScientificFixture {
  std::vector<Lardon3DSparseIncrementalImage> images;
  std::vector<Lardon3DSparseIncrementalObservation> input_observations;
  std::vector<Lardon3DSparseIncrementalComponent> components;
  std::vector<Lardon3DSparseIncrementalCamera> cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> landmarks;
  std::vector<Lardon3DSparseIncrementalLandmarkObservation> observations;
  Lardon3DSparseIncrementalResult incremental = {};
  Lardon3DSparseBundleAdjustmentInput input = {};
};

static ScientificFixture scientific_fixture(bool perturb_camera,
                                             bool perturb_landmarks,
                                             size_t landmark_count = 5,
                                             bool nontrivial_anchor = false) {
  ScientificFixture value;
  const auto intrinsic = calibration();
  const double identity[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  const double anchor_angle = 0.2;
  const double anchor_rotation[9] = {
      std::cos(anchor_angle), -std::sin(anchor_angle), 0.0,
      std::sin(anchor_angle), std::cos(anchor_angle), 0.0,
      0.0, 0.0, 1.0};
  for (size_t camera = 0; camera < 4; ++camera) {
    const uint64_t image_id = 10 + camera * 10;
    value.images.push_back({image_id, intrinsic});
    Lardon3DSparseIncrementalCamera output = {};
    output.image_id = image_id;
    output.component_key = 10;
    std::memcpy(output.pose_cw.rotation_cw,
                camera == 0 && nontrivial_anchor ? anchor_rotation : identity,
                sizeof(identity));
    output.pose_cw.translation_cw[0] = -static_cast<double>(camera);
    value.cameras.push_back(output);
  }
  if (perturb_camera) value.cameras[1].pose_cw.translation_cw[1] = 0.08;

  for (size_t landmark = 0; landmark < landmark_count; ++landmark) {
    const uint64_t track_id = landmark + 1;
    const Lardon3DSparseGeometryPoint3 truth = {
        -0.6 + 0.25 * static_cast<double>(landmark),
        -0.3 + 0.12 * static_cast<double>(landmark % 3),
        4.5 + 0.3 * static_cast<double>(landmark)};
    auto initial = truth;
    if (perturb_landmarks) {
      initial.x += 0.06;
      initial.y -= 0.04;
    }
    value.landmarks.push_back(
        {track_id, track_id, 10, initial, 0.0, 0.0, 4});
    for (size_t camera = 0; camera < 4; ++camera) {
      Lardon3DSparseGeometryPose truth_pose = {};
      std::memcpy(truth_pose.rotation_cw,
                  camera == 0 && nontrivial_anchor ? anchor_rotation : identity,
                  sizeof(identity));
      truth_pose.translation_cw[0] = -static_cast<double>(camera);
      Lardon3DSparseGeometryPoint2 pixel = {};
      if (!lardon3d_sparse_bundle_adjustment_test_project(
              &intrinsic, &truth_pose, &truth, &pixel))
        std::abort();
      const uint64_t image_id = 10 + camera * 10;
      const uint64_t feature_set_id = 100 + camera;
      value.input_observations.push_back(
          {track_id, image_id, feature_set_id,
           static_cast<uint32_t>(landmark), 64, pixel.x, pixel.y});
      value.observations.push_back(
          {track_id, track_id, image_id, feature_set_id,
           static_cast<uint32_t>(landmark), static_cast<uint32_t>(camera)});
    }
  }
  value.components.push_back({10, 4, 4, landmark_count});
  value.incremental.status = LARDON3D_SPARSE_INCREMENTAL_COMPLETE;
  value.incremental.track_set_id = 77;
  value.incremental.calibration_scope_id = 88;
  return value;
}

static void bind_scientific_fixture(ScientificFixture *value) {
  value->incremental.components = value->components.data();
  value->incremental.component_count = value->components.size();
  value->incremental.cameras = value->cameras.data();
  value->incremental.camera_count = value->cameras.size();
  value->incremental.landmarks = value->landmarks.data();
  value->incremental.landmark_count = value->landmarks.size();
  value->incremental.observations = value->observations.data();
  value->incremental.observation_count = value->observations.size();
  value->input = {&value->incremental, value->images.data(), value->images.size(),
                  value->input_observations.data(),
                  value->input_observations.size()};
}

static bool published_metrics(const ScientificFixture &fixture,
                              const Lardon3DSparseBundleAdjustmentResult &result,
                              double *rmse, double *cost) {
  std::vector<double> residuals(result.observation_count * 2);
  for (size_t index = 0; index < result.observation_count; ++index) {
    const auto &association = result.observations[index];
    auto camera = std::lower_bound(
        result.cameras, result.cameras + result.camera_count, association.image_id,
        [](const auto &item, uint64_t key) { return item.image_id < key; });
    auto landmark = std::lower_bound(
        result.landmarks, result.landmarks + result.landmark_count,
        association.track_id,
        [](const auto &item, uint64_t key) { return item.track_id < key; });
    auto source = std::find_if(
        fixture.input_observations.begin(), fixture.input_observations.end(),
        [&](const auto &item) {
          return item.feature_set_id == association.feature_set_id &&
                 item.feature_index == association.feature_index;
        });
    auto image = std::lower_bound(
        fixture.images.begin(), fixture.images.end(), association.image_id,
        [](const auto &item, uint64_t key) { return item.image_id < key; });
    if (camera == result.cameras + result.camera_count ||
        landmark == result.landmarks + result.landmark_count ||
        source == fixture.input_observations.end() || image == fixture.images.end())
      return false;
    Lardon3DSparseGeometryPoint2 pixel = {};
    if (!lardon3d_sparse_bundle_adjustment_test_project(
            &image->calibration, &camera->pose_cw, &landmark->point, &pixel))
      return false;
    residuals[index * 2] = pixel.x - source->x;
    residuals[index * 2 + 1] = pixel.y - source->y;
  }
  return lardon3d_sparse_bundle_adjustment_test_metrics(
      residuals.data(), result.observation_count, rmse, cost);
}

static ScientificFixture two_component_fixture(bool reject_second) {
  ScientificFixture first = scientific_fixture(true, true);
  ScientificFixture second = scientific_fixture(true, true);
  for (auto &image : second.images) image.image_id += 100;
  for (auto &observation : second.input_observations) {
    observation.track_id += 100;
    observation.image_id += 100;
    observation.feature_set_id += 1000;
  }
  for (auto &component : second.components) component.component_key += 100;
  for (auto &camera : second.cameras) {
    camera.image_id += 100;
    camera.component_key += 100;
  }
  for (auto &landmark : second.landmarks) {
    landmark.landmark_id += 100;
    landmark.track_id += 100;
    landmark.component_key += 100;
  }
  if (reject_second) second.landmarks[0].point.z = -1.0;
  for (auto &observation : second.observations) {
    observation.landmark_id += 100;
    observation.track_id += 100;
    observation.image_id += 100;
    observation.feature_set_id += 1000;
  }
  first.images.insert(first.images.end(), second.images.begin(), second.images.end());
  first.input_observations.insert(first.input_observations.end(),
                                  second.input_observations.begin(),
                                  second.input_observations.end());
  first.components.insert(first.components.end(), second.components.begin(),
                          second.components.end());
  first.cameras.insert(first.cameras.end(), second.cameras.begin(),
                       second.cameras.end());
  first.landmarks.insert(first.landmarks.end(), second.landmarks.begin(),
                         second.landmarks.end());
  first.observations.insert(first.observations.end(), second.observations.begin(),
                            second.observations.end());
  return first;
}

static int run_scientific_case(bool perturb_camera, bool perturb_landmarks,
                               double noise_px, size_t outlier_percent) {
  auto value = scientific_fixture(perturb_camera, perturb_landmarks,
                                  noise_px == 0.0 && outlier_percent == 0 ? 5 : 10);
  if (noise_px != 0.0) {
    for (auto &landmark : value.landmarks) landmark.point.z = 5.0;
    for (auto &observation : value.input_observations) {
      const auto camera = std::lower_bound(
          value.cameras.begin(), value.cameras.end(), observation.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      const auto landmark = std::lower_bound(
          value.landmarks.begin(), value.landmarks.end(), observation.track_id,
          [](const auto &item, uint64_t key) { return item.track_id < key; });
      const auto image = std::lower_bound(
          value.images.begin(), value.images.end(), observation.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      CHECK(camera != value.cameras.end());
      CHECK(landmark != value.landmarks.end());
      CHECK(image != value.images.end());
      Lardon3DSparseGeometryPoint2 noiseless = {};
      CHECK(lardon3d_sparse_bundle_adjustment_test_project(
          &image->calibration, &camera->pose_cw, &landmark->point, &noiseless));
      observation.x = noiseless.x;
      observation.y = noiseless.y;
    }
    for (size_t index = 0; index < value.input_observations.size(); ++index) {
      auto &observation = value.input_observations[index];
      const auto camera = std::lower_bound(
          value.cameras.begin(), value.cameras.end(), observation.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      const auto landmark = std::lower_bound(
          value.landmarks.begin(), value.landmarks.end(), observation.track_id,
          [](const auto &item, uint64_t key) { return item.track_id < key; });
      const auto image = std::lower_bound(
          value.images.begin(), value.images.end(), observation.image_id,
          [](const auto &item, uint64_t key) { return item.image_id < key; });
      CHECK(camera != value.cameras.end());
      CHECK(landmark != value.landmarks.end());
      CHECK(image != value.images.end());
      Lardon3DSparseGeometryPoint2 noiseless = {};
      CHECK(lardon3d_sparse_bundle_adjustment_test_project(
          &image->calibration, &camera->pose_cw, &landmark->point, &noiseless));
      CHECK(close_scalar(observation.x, noiseless.x));
      CHECK(close_scalar(observation.y, noiseless.y));
      observation.y += observation.track_id % 2 == 0 ? noise_px : -noise_px;
      CHECK(std::hypot(observation.x - noiseless.x,
                       observation.y - noiseless.y) == noise_px);
    }
  }
  const size_t outlier_count =
      value.input_observations.size() * outlier_percent / 100;
  CHECK(outlier_count * 100 ==
        value.input_observations.size() * outlier_percent);
  if (outlier_percent == 10) CHECK(outlier_count == 4);
  if (outlier_percent == 20) CHECK(outlier_count == 8);
  if (outlier_percent == 40) CHECK(outlier_count == 16);
  for (size_t index = 0; index < outlier_count; ++index) {
    value.input_observations[index].x += index % 2 == 0 ? 2.000001 : -2.000001;
  }
  bind_scientific_fixture(&value);
  Lardon3DSparseBundleAdjustmentResult result = {};
  CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  if (outlier_percent != 0 &&
      result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_FAILED) {
    CHECK(!result.diagnostics[0].accepted);
    CHECK(result.diagnostics[0].termination ==
          LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_NO_CONVERGENCE);
    CHECK(result.diagnostics[0].rejection_reason ==
          LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NO_CONVERGENCE);
    CHECK(!result.diagnostics[0].has_costs && !result.diagnostics[0].has_rmse);
    CHECK(std::memcmp(result.cameras, value.cameras.data(),
                      value.cameras.size() * sizeof(value.cameras[0])) == 0);
    CHECK(std::memcmp(result.landmarks, value.landmarks.data(),
                      value.landmarks.size() * sizeof(value.landmarks[0])) == 0);
    lardon3d_sparse_bundle_adjustment_result_destroy(&result);
    return 0;
  }
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE);
  CHECK(result.diagnostics[0].accepted);
  CHECK(result.diagnostics[0].termination ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_CONVERGED);
  CHECK(result.diagnostics[0].has_costs && result.diagnostics[0].has_rmse);
  CHECK(std::isfinite(result.diagnostics[0].initial_robust_cost));
  CHECK(std::isfinite(result.diagnostics[0].final_robust_cost));
  CHECK(result.diagnostics[0].final_robust_cost <=
        result.diagnostics[0].initial_robust_cost +
            1e-12 * std::max(1.0,
                             std::abs(result.diagnostics[0].initial_robust_cost)));
  if (perturb_camera || perturb_landmarks)
    CHECK(result.diagnostics[0].final_robust_cost <
          result.diagnostics[0].initial_robust_cost);
  CHECK(std::memcmp(&result.cameras[0], &value.cameras[0],
                    sizeof(value.cameras[0])) == 0);
  double published_rmse = 0.0;
  double published_cost = 0.0;
  CHECK(published_metrics(value, result, &published_rmse, &published_cost));
  CHECK(close_scalar(published_rmse,
                     result.diagnostics[0].final_reprojection_rmse_px));
  CHECK(close_scalar(published_cost, result.diagnostics[0].final_robust_cost));
  const size_t scale_index = value.cameras.size() - 1;
  const double scale_before = camera_center_x(value.cameras[scale_index].pose_cw);
  const double scale_after = camera_center_x(result.cameras[scale_index].pose_cw);
  CHECK(std::abs(scale_after - scale_before) <=
        1e-12 * std::max({1.0, std::abs(scale_before), std::abs(scale_after)}));
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);
  return 0;
}

static int test_execution_and_solver() {
  Lardon3DSparseBundleAdjustmentResult result = {};
  CHECK(lardon3d_sparse_bundle_adjustment_run(nullptr, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_INVALID_ARGUMENT);
  CHECK(result.components == nullptr && result.component_count == 0);

  auto value = scientific_fixture(false, false);
  bind_scientific_fixture(&value);
  const auto original_cameras = value.cameras;
  const auto original_landmarks = value.landmarks;
  const auto original_images = value.images;
  const auto original_observations = value.input_observations;
  const auto original_components = value.components;
  const auto original_result_observations = value.observations;
  const auto original_incremental = value.incremental;
  CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE);
  CHECK(result.diagnostics[0].eligible && result.diagnostics[0].accepted);
  CHECK(result.diagnostics[0].has_costs && result.diagnostics[0].has_rmse);
  CHECK(result.diagnostics[0].termination ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_TERMINATION_CONVERGED);
  CHECK(std::memcmp(value.cameras.data(), original_cameras.data(),
                    value.cameras.size() * sizeof(value.cameras[0])) == 0);
  CHECK(std::memcmp(value.landmarks.data(), original_landmarks.data(),
                    value.landmarks.size() * sizeof(value.landmarks[0])) == 0);
  CHECK(std::memcmp(value.input_observations.data(), original_observations.data(),
                    value.input_observations.size() *
                        sizeof(value.input_observations[0])) == 0);
  CHECK(std::memcmp(value.images.data(), original_images.data(),
                    value.images.size() * sizeof(value.images[0])) == 0);
  CHECK(std::memcmp(value.components.data(), original_components.data(),
                    value.components.size() * sizeof(value.components[0])) == 0);
  CHECK(std::memcmp(value.observations.data(), original_result_observations.data(),
                    value.observations.size() * sizeof(value.observations[0])) == 0);
  CHECK(std::memcmp(&value.incremental, &original_incremental,
                    sizeof(value.incremental)) == 0);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);
  lardon3d_sparse_bundle_adjustment_result_destroy(nullptr);

  auto anchor_value = scientific_fixture(false, false, 5, true);
  bind_scientific_fixture(&anchor_value);
  CHECK(lardon3d_sparse_bundle_adjustment_run(&anchor_value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE);
  CHECK(std::memcmp(&result.cameras[0], &anchor_value.cameras[0],
                    sizeof(result.cameras[0])) == 0);
  double anchor_rmse = 0.0;
  double anchor_cost = 0.0;
  CHECK(published_metrics(anchor_value, result, &anchor_rmse, &anchor_cost));
  CHECK(close_scalar(anchor_rmse,
                     result.diagnostics[0].final_reprojection_rmse_px));
  CHECK(close_scalar(anchor_cost, result.diagnostics[0].final_robust_cost));
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  Lardon3DSparseIncrementalImage lone_image = {10, calibration()};
  Lardon3DSparseIncrementalComponent lone_component = {10, 1, 1, 0};
  Lardon3DSparseIncrementalCamera lone_camera = {};
  lone_camera.image_id = 10;
  lone_camera.component_key = 10;
  const double identity[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  std::memcpy(lone_camera.pose_cw.rotation_cw, identity, sizeof(identity));
  Lardon3DSparseIncrementalResult lone_incremental = {};
  lone_incremental.status = LARDON3D_SPARSE_INCREMENTAL_FAILED;
  lone_incremental.components = &lone_component;
  lone_incremental.component_count = 1;
  lone_incremental.cameras = &lone_camera;
  lone_incremental.camera_count = 1;
  Lardon3DSparseBundleAdjustmentInput lone_input = {
      &lone_incremental, &lone_image, 1, nullptr, 0};
  CHECK(lardon3d_sparse_bundle_adjustment_run(&lone_input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_FAILED);
  CHECK(!result.diagnostics[0].eligible);
  CHECK(result.diagnostics[0].rejection_reason ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_INELIGIBLE);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  Lardon3DSparseIncrementalResult empty_incremental = {};
  empty_incremental.status = LARDON3D_SPARSE_INCREMENTAL_FAILED;
  Lardon3DSparseBundleAdjustmentInput empty_input = {
      &empty_incremental, nullptr, 0, nullptr, 0};
  CHECK(lardon3d_sparse_bundle_adjustment_run(&empty_input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_FAILED);
  CHECK(result.component_count == 0 && result.camera_count == 0 &&
        result.landmark_count == 0 && result.observation_count == 0);
  CHECK(result.components == nullptr && result.cameras == nullptr &&
        result.landmarks == nullptr && result.observations == nullptr &&
        result.diagnostics == nullptr);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  value = scientific_fixture(true, true);
  bind_scientific_fixture(&value);
  int kinds[13] = {};
  uint64_t identities[13] = {};
  int groups[13] = {};
  bool constants[13] = {};
  int subset_axes[13] = {};
  size_t parameter_count = 0;
  CHECK(lardon3d_sparse_bundle_adjustment_test_parameter_ordering(
      &value.input, kinds, identities, groups, constants, subset_axes, 13,
      &parameter_count));
  CHECK(parameter_count == 13);
  for (size_t index = 0; index < 5; ++index)
    CHECK(kinds[index] == 0 && identities[index] == index + 1 && groups[index] == 0);
  for (size_t camera = 0; camera < 4; ++camera) {
    const size_t quaternion = 5 + camera * 2;
    const size_t center = quaternion + 1;
    CHECK(kinds[quaternion] == 1 && kinds[center] == 2);
    CHECK(identities[quaternion] == 10 + camera * 10 &&
          identities[center] == identities[quaternion]);
    CHECK(groups[quaternion] == 1 && groups[center] == 1);
    CHECK(constants[quaternion] == (camera == 0));
    CHECK(constants[center] == (camera == 0));
    CHECK(subset_axes[center] == (camera == 3 ? 0 : -1));
  }
  double published_z = 0.0;
  bool candidate_has_metrics = true;
  Lardon3DSparseBundleAdjustmentRejectionReason candidate_reason =
      LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONE;
  CHECK(lardon3d_sparse_bundle_adjustment_test_invalid_candidate(
      &value.input, &published_z, &candidate_has_metrics, &candidate_reason));
  CHECK(published_z == value.landmarks[0].point.z);
  CHECK(!candidate_has_metrics &&
        candidate_reason ==
            LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE);
  CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE);
  CHECK(result.diagnostics[0].accepted);
  CHECK(result.diagnostics[0].final_robust_cost <=
        result.diagnostics[0].initial_robust_cost);
  CHECK(std::memcmp(&result.cameras[0], &value.cameras[0],
                    sizeof(value.cameras[0])) == 0);
  CHECK(std::abs(camera_center_x(result.cameras[3].pose_cw) -
                 camera_center_x(value.cameras[3].pose_cw)) < 1e-12);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  value = two_component_fixture(false);
  bind_scientific_fixture(&value);
  CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_COMPLETE);
  CHECK(result.component_count == 2 && result.diagnostics[0].accepted &&
        result.diagnostics[1].accepted);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  value = two_component_fixture(true);
  bind_scientific_fixture(&value);
  const auto rejected_cameras = value.cameras;
  const auto rejected_landmarks = value.landmarks;
  CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_PARTIAL);
  CHECK(result.diagnostics[0].accepted && !result.diagnostics[1].accepted);
  CHECK(result.diagnostics[1].rejection_reason ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_NONFINITE);
  CHECK(std::memcmp(result.cameras + 4, rejected_cameras.data() + 4,
                    4 * sizeof(result.cameras[0])) == 0);
  CHECK(std::memcmp(result.landmarks + 5, rejected_landmarks.data() + 5,
                    5 * sizeof(result.landmarks[0])) == 0);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  CHECK(run_scientific_case(true, false, 0.0, 0) == 0);
  CHECK(run_scientific_case(false, true, 0.0, 0) == 0);
  CHECK(run_scientific_case(true, true, 0.0, 0) == 0);
  CHECK(run_scientific_case(false, false, 0.5, 0) == 0);
  CHECK(run_scientific_case(false, false, 1.0, 0) == 0);
  CHECK(run_scientific_case(false, false, 2.0, 0) == 0);
  CHECK(run_scientific_case(false, false, 0.0, 10) == 0);
  CHECK(run_scientific_case(false, false, 0.0, 20) == 0);
  CHECK(run_scientific_case(false, false, 0.0, 40) == 0);

  value = scientific_fixture(true, true);
  bind_scientific_fixture(&value);
  CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  for (size_t run = 1; run < 5; ++run) {
    Lardon3DSparseBundleAdjustmentResult repeated = {};
    CHECK(lardon3d_sparse_bundle_adjustment_run(&value.input, &repeated) ==
          LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
    CHECK(equal_results(result, repeated));
    lardon3d_sparse_bundle_adjustment_result_destroy(&repeated);
  }
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);

  Fixture sparse = fixture();
  bind_fixture(&sparse);
  CHECK(lardon3d_sparse_bundle_adjustment_run(&sparse.input, &result) ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK);
  CHECK(result.status == LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_FAILED);
  CHECK(!result.diagnostics[0].eligible);
  CHECK(result.diagnostics[0].rejection_reason ==
        LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_REJECTION_UNDERCONSTRAINED);
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);
  return 0;
}

template <typename T>
static bool write_items(FILE *file, const T *items, size_t count) {
  return count == 0 || std::fwrite(items, sizeof(T), count, file) == count;
}

template <typename T>
static bool read_items(FILE *file, std::vector<T> *items, size_t count) {
  items->resize(count);
  return count == 0 || std::fread(items->data(), sizeof(T), count, file) == count;
}

struct SnapshotStorage {
  Lardon3DSparseBundleAdjustmentResult result = {};
  std::vector<Lardon3DSparseIncrementalComponent> components;
  std::vector<Lardon3DSparseIncrementalCamera> cameras;
  std::vector<Lardon3DSparseIncrementalLandmark> landmarks;
  std::vector<Lardon3DSparseIncrementalLandmarkObservation> observations;
  std::vector<Lardon3DSparseBundleAdjustmentComponentDiagnostic> diagnostics;
};

static bool write_snapshot(const char *path,
                           const Lardon3DSparseBundleAdjustmentResult &result) {
  FILE *file = std::fopen(path, "wb");
  if (!file) return false;
  const char header[8] = {'L', '3', 'D', 'B', 'A', 'E', '2', '7'};
  const bool ok = write_items(file, header, sizeof(header)) &&
                  write_items(file, &result.status, 1) &&
                  write_items(file, &result.component_count, 1) &&
                  write_items(file, &result.camera_count, 1) &&
                  write_items(file, &result.landmark_count, 1) &&
                  write_items(file, &result.observation_count, 1) &&
                  write_items(file, result.components, result.component_count) &&
                  write_items(file, result.cameras, result.camera_count) &&
                  write_items(file, result.landmarks, result.landmark_count) &&
                  write_items(file, result.observations, result.observation_count) &&
                  write_items(file, result.diagnostics, result.component_count);
  return std::fclose(file) == 0 && ok;
}

static bool read_snapshot(const char *path, SnapshotStorage *snapshot) {
  FILE *file = std::fopen(path, "rb");
  if (!file) return false;
  char header[8] = {};
  const char expected[8] = {'L', '3', 'D', 'B', 'A', 'E', '2', '7'};
  bool ok = read_items(file, &snapshot->components, 0) &&
            std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
            std::memcmp(header, expected, sizeof(header)) == 0 &&
            std::fread(&snapshot->result.status, sizeof(snapshot->result.status), 1,
                       file) == 1 &&
            std::fread(&snapshot->result.component_count,
                       sizeof(snapshot->result.component_count), 1, file) == 1 &&
            std::fread(&snapshot->result.camera_count,
                       sizeof(snapshot->result.camera_count), 1, file) == 1 &&
            std::fread(&snapshot->result.landmark_count,
                       sizeof(snapshot->result.landmark_count), 1, file) == 1 &&
            std::fread(&snapshot->result.observation_count,
                       sizeof(snapshot->result.observation_count), 1, file) == 1;
  if (ok && (snapshot->result.component_count > 4096 ||
             snapshot->result.camera_count > 4096 ||
             snapshot->result.landmark_count > 250000 ||
             snapshot->result.observation_count > 1000000))
    ok = false;
  if (ok)
    ok = read_items(file, &snapshot->components, snapshot->result.component_count) &&
         read_items(file, &snapshot->cameras, snapshot->result.camera_count) &&
         read_items(file, &snapshot->landmarks, snapshot->result.landmark_count) &&
         read_items(file, &snapshot->observations,
                    snapshot->result.observation_count) &&
         read_items(file, &snapshot->diagnostics, snapshot->result.component_count);
  if (ok) {
    snapshot->result.components = snapshot->components.data();
    snapshot->result.cameras = snapshot->cameras.data();
    snapshot->result.landmarks = snapshot->landmarks.data();
    snapshot->result.observations = snapshot->observations.data();
    snapshot->result.diagnostics = snapshot->diagnostics.data();
    ok = std::fgetc(file) == EOF;
  }
  return std::fclose(file) == 0 && ok;
}

static int snapshot_mode(const char *write_path, const char *compare_path) {
  auto value = scientific_fixture(true, true);
  bind_scientific_fixture(&value);
  Lardon3DSparseBundleAdjustmentResult result = {};
  if (lardon3d_sparse_bundle_adjustment_run(&value.input, &result) !=
      LARDON3D_SPARSE_BUNDLE_ADJUSTMENT_EXECUTION_OK)
    return 1;
  bool ok = false;
  if (write_path) {
    ok = write_snapshot(write_path, result);
  } else {
    SnapshotStorage reference;
    ok = read_snapshot(compare_path, &reference) &&
         equal_results(reference.result, result);
  }
  lardon3d_sparse_bundle_adjustment_result_destroy(&result);
  return ok ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc == 3 && std::strcmp(argv[1], "--write-snapshot") == 0)
    return snapshot_mode(argv[2], nullptr);
  if (argc == 3 && std::strcmp(argv[1], "--compare-snapshot") == 0)
    return snapshot_mode(nullptr, argv[2]);
  if (test_helpers() != 0) return 1;
  if (test_preparation() != 0) return 1;
  if (test_invalid() != 0) return 1;
  if (test_multiple_components() != 0) return 1;
  if (test_underconstraint_rules() != 0) return 1;
  return test_execution_and_solver();
}
