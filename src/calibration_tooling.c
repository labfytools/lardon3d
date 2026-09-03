#include <lardon3d/calibration_tooling.h>

#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <string.h>

enum { kHeaderSize = 152, kEntrySize = 140, kMinimumViews = 40, kMinimumCorners = 1600 };

static bool nonzero(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t i = 0; i < 32; ++i) any |= value[i];
  return any != 0;
}
static bool finite_value(double value) { return isfinite(value); }
static void put_u32(unsigned char *p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8u * i));
}
static void put_u64(unsigned char *p, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (8u * i));
}
static void put_f64(unsigned char *p, double value) {
  uint64_t bits = 0; memcpy(&bits, &value, sizeof(bits)); put_u64(p, bits);
}
static bool digest(const unsigned char *p, size_t n, unsigned char out[32]) {
  unsigned int length = 0;
  return EVP_Digest(p, n, out, &length, EVP_sha256(), NULL) == 1 && length == 32;
}
static bool parameters_valid(const Lardon3DCalibrationToolingEntry *v, bool fit) {
  const double *p = fit ? &v->fit_fx : &v->fx;
  return p[0] > 0.0 && p[1] > 0.0 && p[2] >= 0.0 && p[3] >= 0.0 &&
         p[2] < (double)v->width && p[3] < (double)v->height &&
         finite_value(p[0]) && finite_value(p[1]) && finite_value(p[2]) && finite_value(p[3]) &&
         finite_value(p[4]) && finite_value(p[5]) && finite_value(p[6]) && finite_value(p[7]);
}
static int angle_class(double degrees) {
  if (degrees < 20.0 || degrees > 60.0) return -1;
  return degrees <= 35.0 ? 0 : (degrees <= 50.0 ? 1 : 2);
}
static unsigned int popcount4(uint32_t value) {
  unsigned int count = 0;
  for (unsigned int bit = 0; bit < 4; ++bit) count += (value >> bit) & 1u;
  return count;
}
static bool projected_delta_ok(const Lardon3DCalibrationToolingEntry *v) {
  static const double rays[5][2] = {{0, 0}, {-0.7, -0.7}, {0.7, -0.7},
                                    {-0.7, 0.7}, {0.7, 0.7}};
  const double *a = &v->fx, *b = &v->fit_fx;
  double maximum = 0.0;
  for (size_t i = 0; i < 5; ++i) {
    double x = rays[i][0], y = rays[i][1], r2 = x*x + y*y;
    double ax = x*(1+a[4]*r2+a[5]*r2*r2)+2*a[6]*x*y+a[7]*(r2+2*x*x);
    double ay = y*(1+a[4]*r2+a[5]*r2*r2)+a[6]*(r2+2*y*y)+2*a[7]*x*y;
    double bx = x*(1+b[4]*r2+b[5]*r2*r2)+2*b[6]*x*y+b[7]*(r2+2*x*x);
    double by = y*(1+b[4]*r2+b[5]*r2*r2)+b[6]*(r2+2*y*y)+2*b[7]*x*y;
    double delta = hypot(a[0]*ax+a[2] - (b[0]*bx+b[2]),
                         a[1]*ay+a[3] - (b[1]*by+b[3]));
    if (!finite_value(delta)) return false;
    if (delta > maximum) maximum = delta;
  }
  return finite_value(v->maximum_parameter_delta) &&
         fabs(v->maximum_parameter_delta - maximum) <= 1e-12 && maximum <= 0.10;
}

Lardon3DCalibrationToolingResult lardon3d_calibration_tooling_validate(
    const Lardon3DCalibrationToolingEvidence *e) {
  if (!e || !e->views || !e->entries || !e->coordinate_checks || e->view_count > LARDON3D_CALIBRATION_TOOLING_MAX_VIEWS ||
      e->entry_count == 0 || e->entry_count > 4096 || e->coordinate_check_count > LARDON3D_CALIBRATION_TOOLING_MAX_COORDINATE_CHECKS)
    return LARDON3D_CALIBRATION_TOOLING_INVALID_ARGUMENT;
  if (!nonzero(e->target_sha256) || !nonzero(e->optical_state_sha256) ||
      !nonzero(e->solver_executable_sha256) || !nonzero(e->solver_configuration_sha256) ||
      !nonzero(e->initialization_evidence_sha256) || !nonzero(e->validation_evidence_sha256) ||
      e->target_family != LARDON3D_CALIBRATION_TOOLING_TARGET_CHARUCO_9X7_DICT_5X5_100 ||
      e->target_squares_x != 9 || e->target_squares_y != 7 ||
      !finite_value(e->target_square_length_mm) || e->target_square_length_mm != 30.0 ||
      !finite_value(e->target_marker_length_mm) || e->target_marker_length_mm != 21.0 ||
      !finite_value(e->target_active_width_mm) || e->target_active_width_mm != 270.0 ||
      !finite_value(e->target_active_height_mm) || e->target_active_height_mm != 210.0 ||
      !finite_value(e->target_white_border_mm) || e->target_white_border_mm < 30.0 ||
      e->extra_distortion_coefficient_count != 0 || !finite_value(e->measurement_resolution_mm) ||
      e->measurement_resolution_mm <= 0 || e->measurement_resolution_mm > 0.1 ||
      !isnan(e->target_flatness_mm))
    return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  double lo = 30.0, hi = 30.0;
  for (size_t i = 0; i < LARDON3D_CALIBRATION_TOOLING_TARGET_MEASUREMENTS; ++i) {
    if (!finite_value(e->target_measurements_mm[i]) || fabs(e->target_measurements_mm[i] - 30.0) > 0.30)
      return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
    if (e->target_measurements_mm[i] < lo) lo = e->target_measurements_mm[i];
    if (e->target_measurements_mm[i] > hi) hi = e->target_measurements_mm[i];
  }
  if (hi - lo > 0.20 || !finite_value(e->holdout_rmse_px) || !finite_value(e->holdout_maximum_residual_px) ||
      e->holdout_rmse_px < 0 || e->holdout_maximum_residual_px < 0 ||
      e->holdout_rmse_px > 0.75 || e->holdout_maximum_residual_px > 1.50)
    return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  uint64_t corners = 0, residuals = 0, high = 0;
  size_t accepted_views = 0;
  uint32_t quadrant[5] = {0}, distance[3] = {0}, angles[3] = {0}, inclined = 0;
  size_t fit_views = 0, holdout_views = 0, fit_quadrant[4] = {0};
  double min_distance = INFINITY, max_distance = 0.0;
  for (size_t i = 0; i < e->view_count; ++i) {
    const Lardon3DCalibrationToolingView *v = &e->views[i];
    if (!nonzero(v->source_sha256) || v->accepted > 1 || v->holdout > 1 ||
        (!v->accepted && v->rejection_reason == 0) ||
        (v->accepted && v->rejection_reason != 0))
      return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
    if (!v->accepted) {
      if (v->holdout != 0) return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
      continue;
    }
    if (v->quadrant > 4 || v->distance_band > 2 ||
        (v->orientation_degrees != 0 && v->orientation_degrees != 90 &&
         v->orientation_degrees != 180 && v->orientation_degrees != 270) ||
        popcount4(v->target_corner_quadrant_mask) < 3 ||
        v->corner_count < 16 || !finite_value(v->target_occupancy) || v->target_occupancy < .20 || v->target_occupancy > .80 ||
        !finite_value(v->normal_angle_degrees) || !finite_value(v->distance_metres) || v->distance_metres <= 0 ||
        !finite_value(v->corner_rms_px) || v->corner_rms_px < 0 || v->corner_rms_px > .25 || !finite_value(v->clipped_fraction) || v->clipped_fraction < 0 || v->clipped_fraction > .01 ||
        !finite_value(v->reprojection_rmse_px) || v->reprojection_rmse_px < 0 || v->reprojection_rmse_px > .75 ||
        !finite_value(v->maximum_residual_px) || v->maximum_residual_px < 0 || v->maximum_residual_px > 1.50)
      return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
    if (v->high_residual_count > v->residual_count || UINT64_MAX - corners < v->corner_count || UINT64_MAX - residuals < v->residual_count || UINT64_MAX - high < v->high_residual_count)
      return LARDON3D_CALIBRATION_TOOLING_CAPACITY;
    ++accepted_views;
    corners += v->corner_count; residuals += v->residual_count; high += v->high_residual_count;
    ++quadrant[v->quadrant]; ++distance[v->distance_band];
    int class_index = angle_class(v->normal_angle_degrees);
    if (class_index >= 0) { ++angles[class_index]; ++inclined; }
    if (v->holdout) ++holdout_views;
    else { ++fit_views; if (v->quadrant < 4) ++fit_quadrant[v->quadrant]; }
    if (v->distance_metres < min_distance) min_distance = v->distance_metres;
    if (v->distance_metres > max_distance) max_distance = v->distance_metres;
  }
  if (accepted_views < kMinimumViews || corners < kMinimumCorners || quadrant[0] < 6 || quadrant[1] < 6 || quadrant[2] < 6 || quadrant[3] < 6 || quadrant[4] < 8 ||
      distance[0] < 8 || distance[1] < 8 || distance[2] < 8 || max_distance / min_distance < 1.5 ||
      inclined < 24 || angles[0] < 6 || angles[1] < 6 || angles[2] < 6 || fit_views < 32 || holdout_views < 8 ||
      fit_quadrant[0] < 4 || fit_quadrant[1] < 4 || fit_quadrant[2] < 4 || fit_quadrant[3] < 4 ||
      residuals == 0 || high * 100 > residuals)
    return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  for (size_t i = 0; i < e->view_count; ++i) {
    const Lardon3DCalibrationToolingView *v = &e->views[i];
    if (!v->accepted) continue;
    size_t rank = 0;
    int class_index = angle_class(v->normal_angle_degrees);
    for (size_t other = 0; other < e->view_count; ++other) {
      const Lardon3DCalibrationToolingView *candidate = &e->views[other];
      if (!candidate->accepted || candidate->quadrant != v->quadrant ||
          candidate->distance_band != v->distance_band ||
          angle_class(candidate->normal_angle_degrees) != class_index) continue;
      int compare = memcmp(candidate->source_sha256, v->source_sha256, 32);
      if (compare == 0 && other != i) return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
      if (compare < 0) ++rank;
    }
    if (v->holdout != (rank % 5 == 4)) return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  }
  for (size_t i = 0; i < e->coordinate_check_count; ++i) {
    const Lardon3DCalibrationToolingCoordinateCheck *c = &e->coordinate_checks[i];
    if (!nonzero(c->source_sha256) || (c->orientation_degrees != 0 && c->orientation_degrees != 90 &&
         c->orientation_degrees != 180 && c->orientation_degrees != 270) ||
        !finite_value(c->dx_px) || !finite_value(c->dy_px) || fabs(c->dx_px) > .01 || fabs(c->dy_px) > .01)
      return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  }
  for (size_t view_index = 0; view_index < e->view_count; ++view_index) {
    const Lardon3DCalibrationToolingView *v = &e->views[view_index];
    if (!v->accepted) continue;
    size_t matches = 0;
    for (size_t check_index = 0; check_index < e->coordinate_check_count; ++check_index) {
      const Lardon3DCalibrationToolingCoordinateCheck *c = &e->coordinate_checks[check_index];
      if (memcmp(c->source_sha256, v->source_sha256, 32) == 0) {
        if (c->orientation_degrees != v->orientation_degrees)
          return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
        ++matches;
      }
    }
    if (matches < 20) return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  }
  for (size_t i = 0; i < e->entry_count; ++i) {
    const Lardon3DCalibrationToolingEntry *v = &e->entries[i];
    if (!v->image_id || !nonzero(v->representation_sha256) ||
        memcmp(v->optical_state_sha256, e->optical_state_sha256, 32) != 0 ||
        !v->width || !v->height ||
        !parameters_valid(v, false) || !parameters_valid(v, true) || !v->support_images || !v->support_observations ||
        !finite_value(v->reprojection_rmse_px) || v->reprojection_rmse_px < 0 || v->reprojection_rmse_px > .50 ||
        v->validation_flags != LARDON3D_CALIBRATION_TOOLING_VALIDATION_FLAGS || !projected_delta_ok(v))
      return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
    for (size_t previous = 0; previous < i; ++previous)
      if (v->image_id == e->entries[previous].image_id)
        return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
    const double published[8] = {v->fx,v->fy,v->cx,v->cy,v->k1,v->k2,v->p1,v->p2};
    for (size_t repeat = 0; repeat < 3; ++repeat)
      for (size_t parameter = 0; parameter < 8; ++parameter)
        if (!finite_value(v->repeated_parameters[repeat][parameter]) ||
            v->repeated_parameters[repeat][parameter] != published[parameter])
          return LARDON3D_CALIBRATION_TOOLING_SCIENCE_REJECTED;
  }
  return LARDON3D_CALIBRATION_TOOLING_OK;
}

Lardon3DCalibrationToolingResult lardon3d_calibration_tooling_produce(
    const Lardon3DCalibrationToolingEvidence *e, unsigned char *artifact,
    size_t capacity, size_t *written, unsigned char hash[32]) {
  if (written) *written = 0;
  if (!artifact || !hash) return LARDON3D_CALIBRATION_TOOLING_INVALID_ARGUMENT;
  Lardon3DCalibrationToolingResult valid = lardon3d_calibration_tooling_validate(e);
  if (valid != LARDON3D_CALIBRATION_TOOLING_OK) return valid;
  if (e->entry_count > (SIZE_MAX - kHeaderSize) / kEntrySize) return LARDON3D_CALIBRATION_TOOLING_CAPACITY;
  size_t size = kHeaderSize + e->entry_count * kEntrySize;
  if (size > LARDON3D_CALIBRATION_BOOTSTRAP_MAX_BYTES || capacity < size) return LARDON3D_CALIBRATION_TOOLING_CAPACITY;
  size_t at = 0; memcpy(artifact + at, "L3DCALB1", 8); at += 8;
  put_u32(artifact + at, 1); at += 4; put_u32(artifact + at, 1); at += 4; put_u32(artifact + at, 1); at += 4; put_u32(artifact + at, (uint32_t)e->entry_count); at += 4;
  memcpy(artifact + at, e->solver_executable_sha256, 32); at += 32; memcpy(artifact + at, e->solver_configuration_sha256, 32); at += 32;
  memcpy(artifact + at, e->initialization_evidence_sha256, 32); at += 32; memcpy(artifact + at, e->validation_evidence_sha256, 32); at += 32;
  for (size_t i = 0; i < e->entry_count; ++i) {
    const Lardon3DCalibrationToolingEntry *v = &e->entries[i];
    put_u64(artifact + at, v->image_id); at += 8; memcpy(artifact + at, v->representation_sha256, 32); at += 32;
    put_u32(artifact + at, v->width); at += 4; put_u32(artifact + at, v->height); at += 4;
    const double values[8] = {v->fx,v->fy,v->cx,v->cy,v->k1,v->k2,v->p1,v->p2};
    for (size_t j = 0; j < 8; ++j) { put_f64(artifact + at, values[j]); at += 8; }
    put_u32(artifact + at, v->support_images); at += 4; put_u32(artifact + at, v->support_observations); at += 4;
    put_f64(artifact + at, v->reprojection_rmse_px); at += 8; put_f64(artifact + at, v->maximum_parameter_delta); at += 8;
    put_u32(artifact + at, v->validation_flags); at += 4;
  }
  if (at != size || !digest(artifact, size, hash)) return LARDON3D_CALIBRATION_TOOLING_ENCODING_ERROR;
  if (written) *written = size;
  return LARDON3D_CALIBRATION_TOOLING_OK;
}

Lardon3DCalibrationToolingResult lardon3d_calibration_tooling_import(
    Lardon3DProjectDb *db, uint64_t execution_id, const Lardon3DCalibrationToolingEvidence *e,
    unsigned char *artifact, size_t capacity, size_t *written, Lardon3DCalibrationBootstrapOutput *out) {
  unsigned char hash[32];
  Lardon3DCalibrationToolingResult result = lardon3d_calibration_tooling_produce(e, artifact, capacity, written, hash);
  if (result != LARDON3D_CALIBRATION_TOOLING_OK || !db || !execution_id || !out) return result == LARDON3D_CALIBRATION_TOOLING_OK ? LARDON3D_CALIBRATION_TOOLING_INVALID_ARGUMENT : result;
  return lardon3d_calibration_bootstrap_import(db, execution_id, artifact, *written, hash, out) == LARDON3D_CALIBRATION_BOOTSTRAP_OK ?
             LARDON3D_CALIBRATION_TOOLING_OK : LARDON3D_CALIBRATION_TOOLING_IMPORT_ERROR;
}
