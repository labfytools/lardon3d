#include "sparse_sfm_gate_f_internal.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <openssl/evp.h>
#include <vector>
#include <algorithm>

namespace {
bool put_u32(unsigned char *record, size_t *offset, uint32_t value) {
  if (*offset > 372U - 4U) return false;
  for (unsigned int byte = 0; byte < 4; ++byte)
    record[(*offset)++] = static_cast<unsigned char>(value >> (byte * 8U));
  return true;
}

bool put_u64(unsigned char *record, size_t *offset, uint64_t value) {
  if (*offset > 372U - 8U) return false;
  for (unsigned int byte = 0; byte < 8; ++byte)
    record[(*offset)++] = static_cast<unsigned char>(value >> (byte * 8U));
  return true;
}

bool put_f64(unsigned char *record, size_t *offset, double value) {
  if (!std::isfinite(value)) return false;
  if (value == 0.0) value = 0.0;
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  return put_u64(record, offset, bits);
}

bool checked_add(uint64_t left, uint64_t right, uint64_t *result) {
  if (right > std::numeric_limits<uint64_t>::max() - left) return false;
  *result = left + right;
  return true;
}

bool checked_mul(uint64_t left, uint64_t right, uint64_t *result) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
  *result = left * right;
  return true;
}
} // namespace

bool lardon3d_sparse_sfm_fingerprint_record(
    const Lardon3DSparseIncrementalParameters *p, unsigned char record[372]) {
  if (!p || !record) return false;
  std::memset(record, 0, 372);
  std::memcpy(record, "L3DSFMFP", 8);
  size_t offset = 8;
  bool ok = put_u32(record, &offset, 1) &&
            put_u32(record, &offset, p->minimum_seed_tracks) &&
            put_u32(record, &offset, p->minimum_seed_landmarks) &&
            put_u32(record, &offset, p->minimum_pnp_correspondences) &&
            put_u32(record, &offset, p->maximum_seed_candidates) &&
            put_u32(record, &offset, p->maximum_registration_rounds) &&
            put_u32(record, &offset, p->maximum_landmarks_per_round) &&
            put_u32(record, &offset, p->maximum_images) &&
            put_u64(record, &offset, p->maximum_observations) &&
            put_u64(record, &offset, p->maximum_tracks) &&
            put_f64(record, &offset, p->reprojection_threshold_px) &&
            put_f64(record, &offset, p->minimum_track_parallax_rad) &&
            put_f64(record, &offset, p->relative_pose.robust_threshold_px) &&
            put_f64(record, &offset, p->relative_pose.confidence) &&
            put_u32(record, &offset, p->relative_pose.max_iterations) &&
            put_u32(record, &offset, p->relative_pose.minimum_inliers) &&
            put_f64(record, &offset, p->relative_pose.minimum_inlier_ratio) &&
            put_f64(record, &offset, p->relative_pose.minimum_parallax_rad) &&
            put_f64(record, &offset, p->relative_pose.minimum_cheirality_ratio) &&
            put_u64(record, &offset, p->relative_pose.deterministic_seed) &&
            put_f64(record, &offset, p->pnp.reprojection_threshold_px) &&
            put_f64(record, &offset, p->pnp.confidence) &&
            put_u32(record, &offset, p->pnp.max_iterations) &&
            put_u32(record, &offset, p->pnp.minimum_inliers) &&
            put_f64(record, &offset, p->pnp.minimum_inlier_ratio) &&
            put_u64(record, &offset, p->pnp.deterministic_seed) &&
            put_u32(record, &offset, p->refinement.max_iterations) &&
            put_f64(record, &offset, p->refinement.convergence_tolerance);
  for (unsigned int policy = 0; ok && policy < 19; ++policy)
    ok = put_u32(record, &offset, 1);
  ok = ok && put_f64(record, &offset, 1e-9);
  for (unsigned int policy = 0; ok && policy < 2; ++policy)
    ok = put_u32(record, &offset, 1);
  ok = ok && put_f64(record, &offset, 1e-9) && put_u32(record, &offset, 1) &&
       put_f64(record, &offset, 2.0);
  for (unsigned int policy = 0; ok && policy < 6; ++policy)
    ok = put_u32(record, &offset, 1);
  ok = ok && put_u32(record, &offset, 1) && put_u32(record, &offset, 50) &&
       put_f64(record, &offset, 1e-6) && put_f64(record, &offset, 1e-10) &&
       put_f64(record, &offset, 1e-8) && put_u32(record, &offset, 0) &&
       put_u32(record, &offset, 1) && put_f64(record, &offset, 1e-12) &&
       put_u32(record, &offset, 1) && put_u32(record, &offset, 1);
  return ok && offset == 372;
}

bool lardon3d_sparse_sfm_parameter_fingerprint(
    const Lardon3DSparseIncrementalParameters *parameters, unsigned char digest[32]) {
  if (!digest) return false;
  unsigned char record[372];
  unsigned int length = 0;
  return lardon3d_sparse_sfm_fingerprint_record(parameters, record) &&
         EVP_Digest(record, sizeof(record), digest, &length, EVP_sha256(), nullptr) == 1 &&
         length == 32;
}

bool lardon3d_sparse_sfm_resource_estimate(uint64_t image_count, uint64_t track_count,
                                           uint64_t observation_count,
                                           Lardon3DResourceEstimate *estimate) {
  if (!estimate) return false;
  uint64_t images = 0, tracks = 0, observations = 0, raw = 134217728ULL;
  if (!checked_mul(image_count, 65536ULL, &images) ||
      !checked_mul(track_count, 2048ULL, &tracks) ||
      !checked_mul(observation_count, 512ULL, &observations) ||
      !checked_add(raw, images, &raw) || !checked_add(raw, tracks, &raw) ||
      !checked_add(raw, observations, &raw)) return false;
  constexpr uint64_t mib = 1024ULL * 1024ULL;
  uint64_t remainder = raw % mib;
  if (remainder != 0 && !checked_add(raw, mib - remainder, &raw)) return false;
  *estimate = {raw, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
  return true;
}

bool lardon3d_sparse_sfm_component_persistable(uint64_t registered_image_count,
                                                uint64_t landmark_count) {
  return registered_image_count > 0 && landmark_count > 0;
}

bool lardon3d_sparse_sfm_publication_metrics(const double *squared_errors, size_t count,
                                              double *rmse, double *median) {
  if (!squared_errors || count == 0 || !rmse || !median) return false;
  try {
    std::vector<double> errors;
    errors.reserve(count);
    double sum = 0.0;
    for (size_t index = 0; index < count; ++index) {
      double squared = squared_errors[index];
      if (!std::isfinite(squared) || squared < 0.0 || !std::isfinite(sum + squared)) return false;
      sum += squared;
      errors.push_back(std::sqrt(squared));
    }
    std::sort(errors.begin(), errors.end());
    *rmse = std::sqrt(sum / static_cast<double>(count));
    size_t middle = count / 2;
    *median = count % 2 ? errors[middle]
                        : errors[middle - 1] + (errors[middle] - errors[middle - 1]) / 2.0;
    return std::isfinite(*rmse) && std::isfinite(*median);
  } catch (...) {
    return false;
  }
}
