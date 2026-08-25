#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <openssl/evp.h>

#include <lardon3d/sparse_sfm_incremental.h>
#include "../src/sparse_sfm_gate_f_internal.h"

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      return 1;                                                                           \
    }                                                                                     \
  } while (false)

static bool decode_hex(const char *hex, unsigned char *output, size_t size) {
  if (std::strlen(hex) != size * 2) return false;
  for (size_t i = 0; i < size; ++i) {
    unsigned int value = 0;
    if (std::sscanf(hex + i * 2, "%2x", &value) != 1) return false;
    output[i] = static_cast<unsigned char>(value);
  }
  return true;
}

static bool hash(const unsigned char *bytes, size_t size, unsigned char digest[32]) {
  unsigned int length = 0;
  return EVP_Digest(bytes, size, digest, &length, EVP_sha256(), nullptr) == 1 && length == 32;
}

int main() {
  static const char golden_record_hex[] =
      "4c334453464d46500100000006000000060000000600000020000000200000000010000000100000"
      "40420f000000000090d003000000000000000000000000402d431cebe2361a3f000000000000f83f"
      "2b8716d9cef7ef3fdc05000006000000000000000000e03f2d431cebe2361a3f000000000000e03f"
      "0000000000000000000000000000f83f2b8716d9cef7ef3fe803000006000000000000000000e03f"
      "00000000000000001e00000011ea2d819997713d0100000001000000010000000100000001000000"
      "01000000010000000100000001000000010000000100000001000000010000000100000001000000"
      "0100000001000000010000000100000095d626e80b2e113e010000000100000095d626e80b2e113e"
      "01000000000000000000004001000000010000000100000001000000010000000100000001000000"
      "320000008dedb5a0f7c6b03ebbbdd7d9df7cdb3d3a8c30e28e79453e000000000100000011ea2d81"
      "9997713d0100000001000000";
  static const char golden_digest_hex[] =
      "e1c83e5b2036e49254a9426ddbace42b7831373bc896f27abdd2f61e302f9e8c";
  unsigned char expected[372]{};
  unsigned char expected_digest[32]{};
  CHECK(decode_hex(golden_record_hex, expected, sizeof(expected)));
  CHECK(decode_hex(golden_digest_hex, expected_digest, sizeof(expected_digest)));
  Lardon3DSparseIncrementalParameters parameters{};
  CHECK(lardon3d_sparse_incremental_parameters_default(&parameters));
  unsigned char record[372]{};
  unsigned char digest[32]{};
  CHECK(lardon3d_sparse_sfm_fingerprint_record(&parameters, record));
  CHECK(std::memcmp(record, expected, sizeof(record)) == 0);
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&parameters, digest));
  CHECK(std::memcmp(digest, expected_digest, sizeof(digest)) == 0);
  unsigned char repeated[32]{};
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&parameters, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) == 0);

  Lardon3DSparseIncrementalParameters mutated = parameters;
  mutated.minimum_seed_tracks++;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.maximum_tracks++;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.reprojection_threshold_px += 0.25;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.relative_pose.confidence -= 0.01;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.relative_pose.deterministic_seed++;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.pnp.confidence -= 0.01;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.pnp.deterministic_seed++;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  mutated = parameters;
  mutated.refinement.convergence_tolerance *= 2.0;
  CHECK(lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);

  unsigned char policy_record[372];
  std::memcpy(policy_record, record, sizeof(record));
  policy_record[180] ^= 1;
  CHECK(hash(policy_record, sizeof(policy_record), repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  std::memcpy(policy_record, record, sizeof(record));
  policy_record[256] ^= 1;
  CHECK(hash(policy_record, sizeof(policy_record), repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);
  std::memcpy(policy_record, record, sizeof(record));
  policy_record[300] ^= 1;
  CHECK(hash(policy_record, sizeof(policy_record), repeated));
  CHECK(std::memcmp(digest, repeated, sizeof(digest)) != 0);

  mutated = parameters;
  mutated.minimum_track_parallax_rad = 0.0;
  CHECK(lardon3d_sparse_sfm_fingerprint_record(&mutated, record));
  mutated.minimum_track_parallax_rad = -0.0;
  CHECK(lardon3d_sparse_sfm_fingerprint_record(&mutated, policy_record));
  CHECK(std::memcmp(record, policy_record, sizeof(record)) == 0);
  mutated = parameters;
  mutated.pnp.confidence = std::numeric_limits<double>::quiet_NaN();
  CHECK(!lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));
  mutated.pnp.confidence = std::numeric_limits<double>::infinity();
  CHECK(!lardon3d_sparse_sfm_parameter_fingerprint(&mutated, repeated));

  Lardon3DResourceEstimate estimate{};
  CHECK(lardon3d_sparse_sfm_resource_estimate(10, 100, 1000, &estimate));
  CHECK(estimate.memory_fixed_bytes == 136314880ULL);
  CHECK(estimate.memory_bytes_per_item == 0 && estimate.gpu_memory_fixed_bytes == 0 &&
        estimate.gpu_memory_bytes_per_item == 0 && estimate.minimum_batch_size == 1 &&
        estimate.maximum_batch_size == 1 && estimate.desired_cpu_threads == 1 &&
        estimate.desired_gpu_slots == 0 && estimate.desired_io_slots == 1 &&
        estimate.task_class == LARDON3D_RESOURCE_TASK_CPU);
  CHECK(!lardon3d_sparse_sfm_resource_estimate(UINT64_MAX, 1, 1, &estimate));
  CHECK(!lardon3d_sparse_sfm_resource_estimate(1, UINT64_MAX, 1, &estimate));
  CHECK(!lardon3d_sparse_sfm_resource_estimate(1, 1, UINT64_MAX, &estimate));
  CHECK(lardon3d_sparse_sfm_component_persistable(2, 3));
  CHECK(!lardon3d_sparse_sfm_component_persistable(0, 3));
  CHECK(!lardon3d_sparse_sfm_component_persistable(2, 0));
  double rmse = 0.0;
  double median = 0.0;
  const double zero[] = {0.0};
  CHECK(lardon3d_sparse_sfm_publication_metrics(zero, 1, &rmse, &median));
  CHECK(rmse == 0.0 && median == 0.0);
  const double odd[] = {1.0, 9.0, 4.0};
  CHECK(lardon3d_sparse_sfm_publication_metrics(odd, 3, &rmse, &median));
  CHECK(std::abs(rmse - std::sqrt(14.0 / 3.0)) < 1e-15 && median == 2.0);
  const double even[] = {1.0, 16.0, 4.0, 9.0};
  CHECK(lardon3d_sparse_sfm_publication_metrics(even, 4, &rmse, &median));
  CHECK(std::abs(rmse - std::sqrt(7.5)) < 1e-15 && median == 2.5);
  const double weighted[] = {1.0, 1.0, 100.0};
  CHECK(lardon3d_sparse_sfm_publication_metrics(weighted, 3, &rmse, &median));
  CHECK(std::abs(rmse - std::sqrt(34.0)) < 1e-15 && median == 1.0);
  const double invalid[] = {1.0, std::numeric_limits<double>::infinity()};
  CHECK(!lardon3d_sparse_sfm_publication_metrics(invalid, 2, &rmse, &median));
  return 0;
}
