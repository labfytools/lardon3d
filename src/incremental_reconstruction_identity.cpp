#include <lardon3d/incremental_reconstruction.h>

#include <cstring>
#include <limits>
#include <openssl/evp.h>

namespace {
constexpr size_t fingerprint_size =
    LARDON3D_INCREMENTAL_RECONSTRUCTION_FINGERPRINT_RECORD_SIZE;
constexpr size_t identity_size =
    LARDON3D_INCREMENTAL_RECONSTRUCTION_IDENTITY_RECORD_SIZE;

bool put_u32(unsigned char *record, size_t capacity, size_t *offset,
             uint32_t value) {
  if (!record || !offset || *offset > capacity - 4) return false;
  for (unsigned int byte = 0; byte < 4; ++byte)
    record[(*offset)++] = static_cast<unsigned char>(value >> (byte * 8U));
  return true;
}

bool put_u64(unsigned char *record, size_t capacity, size_t *offset,
             uint64_t value) {
  if (!record || !offset || *offset > capacity - 8) return false;
  for (unsigned int byte = 0; byte < 8; ++byte)
    record[(*offset)++] = static_cast<unsigned char>(value >> (byte * 8U));
  return true;
}

bool digest_record(const unsigned char *record, size_t size,
                   unsigned char digest[32]) {
  unsigned int length = 0;
  return record && digest &&
         EVP_Digest(record, size, digest, &length, EVP_sha256(), nullptr) == 1 &&
         length == 32;
}

bool checked_add(uint64_t left, uint64_t right, uint64_t *output) {
  if (!output || right > std::numeric_limits<uint64_t>::max() - left) return false;
  *output = left + right;
  return true;
}

bool checked_term(uint64_t count, uint64_t coefficient, uint64_t *raw) {
  if (count != 0 && coefficient > std::numeric_limits<uint64_t>::max() / count)
    return false;
  return checked_add(*raw, count * coefficient, raw);
}
} // namespace

extern "C" bool lardon3d_incremental_reconstruction_parameter_fingerprint_record(
    unsigned char record[fingerprint_size]) {
  if (!record) return false;
  std::memset(record, 0, fingerprint_size);
  std::memcpy(record, "L3DHPRM1", 8);
  size_t offset = 8;
  if (!put_u32(record, fingerprint_size, &offset, 1) ||
      !put_u32(record, fingerprint_size, &offset,
               LARDON3D_INCREMENTAL_RECONSTRUCTION_VERSION))
    return false;
  /* Field position is the policy identifier; every v1 policy is version 1. */
  for (unsigned int policy = 0; policy < 16; ++policy)
    if (!put_u32(record, fingerprint_size, &offset, 1)) return false;
  return offset == fingerprint_size;
}

extern "C" bool lardon3d_incremental_reconstruction_parameter_fingerprint(
    unsigned char digest[32]) {
  unsigned char record[fingerprint_size];
  return digest &&
         lardon3d_incremental_reconstruction_parameter_fingerprint_record(record) &&
         digest_record(record, sizeof(record), digest);
}

extern "C" bool lardon3d_incremental_reconstruction_identity_record(
    const Lardon3DIncrementalReconstructionIdentity *identity,
    unsigned char record[identity_size]) {
  if (!identity || !record || identity->base_reconstruction_id == 0 ||
      identity->extension_track_set_id == 0 || identity->calibration_scope_id == 0 ||
      identity->incremental_kind != LARDON3D_INCREMENTAL_RECONSTRUCTION_KIND ||
      identity->incremental_version != LARDON3D_INCREMENTAL_RECONSTRUCTION_VERSION)
    return false;
  std::memset(record, 0, identity_size);
  std::memcpy(record, "L3DHIDV1", 8);
  size_t offset = 8;
  if (!put_u32(record, identity_size, &offset, 1) ||
      !put_u64(record, identity_size, &offset, identity->base_reconstruction_id) ||
      !put_u64(record, identity_size, &offset, identity->extension_track_set_id) ||
      !put_u64(record, identity_size, &offset, identity->calibration_scope_id) ||
      !put_u32(record, identity_size, &offset, identity->incremental_kind) ||
      !put_u32(record, identity_size, &offset, identity->incremental_version))
    return false;
  std::memcpy(record + offset, identity->parameter_fingerprint, 32);
  offset += 32;
  return offset == identity_size;
}

extern "C" bool lardon3d_incremental_reconstruction_identity_digest(
    const Lardon3DIncrementalReconstructionIdentity *identity,
    unsigned char digest[32]) {
  unsigned char record[identity_size];
  return digest && lardon3d_incremental_reconstruction_identity_record(identity, record) &&
         digest_record(record, sizeof(record), digest);
}

extern "C" bool lardon3d_incremental_reconstruction_resource_estimate(
    const Lardon3DIncrementalReconstructionShape *shape,
    Lardon3DResourceEstimate *estimate) {
  if (!shape || !estimate) return false;
  uint64_t raw = 268435456ULL;
  if (!checked_term(shape->base_camera_count, 131072ULL, &raw) ||
      !checked_term(shape->base_landmark_count, 4096ULL, &raw) ||
      !checked_term(shape->base_observation_count, 1024ULL, &raw) ||
      !checked_term(shape->extension_image_count, 131072ULL, &raw) ||
      !checked_term(shape->extension_track_count, 4096ULL, &raw) ||
      !checked_term(shape->extension_observation_count, 1024ULL, &raw))
    return false;
  constexpr uint64_t mib = 1024ULL * 1024ULL;
  const uint64_t remainder = raw % mib;
  if (remainder != 0 && !checked_add(raw, mib - remainder, &raw)) return false;
  *estimate = {raw, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
  return true;
}
