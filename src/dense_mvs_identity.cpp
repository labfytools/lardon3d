#include <lardon3d/dense_mvs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <openssl/evp.h>
#include <vector>

namespace lardon3d::dense_mvs_detail {
bool source_image_set_identity(const Lardon3DDenseMvsSourceImage *images,
                               size_t count, unsigned char digest[32]);
bool calibration_binding_identity(const Lardon3DDenseMvsSourceImage *images,
                                  size_t count, unsigned char digest[32]);
} // namespace lardon3d::dense_mvs_detail

namespace {
bool put_u32(unsigned char *out, size_t cap, size_t *at, uint32_t value) {
  if (!out || !at || *at > cap - 4) return false;
  for (unsigned int i = 0; i < 4; ++i) out[(*at)++] = (unsigned char)(value >> (8U * i));
  return true;
}
bool put_u64(unsigned char *out, size_t cap, size_t *at, uint64_t value) {
  if (!out || !at || *at > cap - 8) return false;
  for (unsigned int i = 0; i < 8; ++i) out[(*at)++] = (unsigned char)(value >> (8U * i));
  return true;
}
bool put_f64(unsigned char *out, size_t cap, size_t *at, double value) {
  if (!std::isfinite(value)) return false;
  if (value == 0.0) value = 0.0;
  uint64_t bits = 0;
  static_assert(sizeof bits == sizeof value);
  std::memcpy(&bits, &value, sizeof bits);
  return put_u64(out, cap, at, bits);
}
bool sha(const unsigned char *data, size_t size, unsigned char out[32]) {
  unsigned int length = 0;
  return data && out && EVP_Digest(data, size, out, &length, EVP_sha256(), nullptr) == 1 && length == 32;
}
bool nonzero(const unsigned char value[32]) {
  for (size_t i = 0; i < 32; ++i) if (value[i] != 0) return true;
  return false;
}
bool manifest_valid(const Lardon3DDenseMvsBackendManifest *manifest) {
  return manifest && nonzero(manifest->interface_colmap_version_identity) &&
         nonzero(manifest->interface_colmap_binary_sha256) &&
         nonzero(manifest->densify_point_cloud_version_identity) &&
         nonzero(manifest->densify_point_cloud_binary_sha256);
}
}

extern "C" bool lardon3d_dense_mvs_parameter_fingerprint_record(
    const Lardon3DDenseMvsParameters *parameters, unsigned char record[40]) {
  if (!parameters || !record || parameters->number_views == 0) return false;
  std::memset(record, 0, 40); std::memcpy(record, "L3DMPRM1", 8);
  size_t at = 8;
  return put_u32(record, 40, &at, 1) && put_u32(record, 40, &at, 1) &&
         put_u32(record, 40, &at, parameters->resolution_level) &&
         put_u32(record, 40, &at, parameters->minimum_resolution) &&
         put_u32(record, 40, &at, parameters->number_views) &&
         put_u32(record, 40, &at, parameters->fusion_mode) &&
         put_u32(record, 40, &at, 1) && /* FIXED_K_OPENCV_UNDISTORT_V1 */
         put_u32(record, 40, &at, 1) && /* OPENMVS_2_4_CPU_ONLY_V1 */
         at == 40;
}
extern "C" bool lardon3d_dense_mvs_parameter_fingerprint(
    const Lardon3DDenseMvsParameters *parameters, unsigned char digest[32]) {
  unsigned char record[40];
  return digest &&
         lardon3d_dense_mvs_parameter_fingerprint_record(parameters, record) &&
         sha(record, sizeof(record), digest);
}
extern "C" bool lardon3d_dense_mvs_backend_manifest_record(
    const Lardon3DDenseMvsBackendManifest *manifest, unsigned char record[148]) {
  if (!manifest_valid(manifest) || !record) return false;
  std::memset(record, 0, 148);
  std::memcpy(record, "L3DMBKD1", 8);
  size_t at = 8;
  if (!put_u32(record, 148, &at, 1) || !put_u32(record, 148, &at, 1)) return false;
  std::memcpy(record + at, manifest->interface_colmap_version_identity, 32); at += 32;
  std::memcpy(record + at, manifest->interface_colmap_binary_sha256, 32); at += 32;
  if (!put_u32(record, 148, &at, 2)) return false;
  std::memcpy(record + at, manifest->densify_point_cloud_version_identity, 32); at += 32;
  std::memcpy(record + at, manifest->densify_point_cloud_binary_sha256, 32); at += 32;
  return at == 148;
}
extern "C" bool lardon3d_dense_mvs_backend_manifest_digest(
    const Lardon3DDenseMvsBackendManifest *manifest, unsigned char digest[32]) {
  unsigned char record[148];
  return digest && lardon3d_dense_mvs_backend_manifest_record(manifest, record) &&
         sha(record, sizeof(record), digest);
}
extern "C" bool lardon3d_dense_mvs_identity_record(
    const Lardon3DDenseMvsIdentity *id, unsigned char record[220]) {
  if (!id || !record || id->dense_kind != 1 || id->dense_version != 1 ||
      id->backend_kind != 1 || id->backend_version == 0 || !nonzero(id->base_reconstruction_identity) ||
      !nonzero(id->source_image_set_identity) || !nonzero(id->calibration_scope_identity) ||
      !nonzero(id->calibration_binding_identity) ||
      !nonzero(id->backend_binary_sha256) || !nonzero(id->parameter_fingerprint)) return false;
  std::memset(record, 0, 220); std::memcpy(record, "L3DMDID2", 8); size_t at = 8;
  if (!put_u32(record, 220, &at, 2) || !put_u32(record, 220, &at, id->dense_kind) ||
      !put_u32(record, 220, &at, id->dense_version) || !put_u32(record, 220, &at, id->backend_kind) ||
      !put_u32(record, 220, &at, id->backend_version)) return false;
  for (const auto *field : {id->base_reconstruction_identity, id->source_image_set_identity,
                            id->calibration_scope_identity, id->calibration_binding_identity,
                            id->backend_binary_sha256,
                            id->parameter_fingerprint}) { std::memcpy(record + at, field, 32); at += 32; }
  return at == 220;
}
extern "C" bool lardon3d_dense_mvs_identity_digest(
    const Lardon3DDenseMvsIdentity *id, unsigned char digest[32]) {
  unsigned char record[220];
  return digest && lardon3d_dense_mvs_identity_record(id, record) &&
         sha(record, sizeof(record), digest);
}
bool lardon3d::dense_mvs_detail::calibration_binding_identity(
    const Lardon3DDenseMvsSourceImage *images, size_t count,
    unsigned char digest[32]) {
  constexpr size_t kRecordSize = 80;
  if (!images || !digest || count == 0 ||
      count > (SIZE_MAX - 20) / kRecordSize)
    return false;
  std::vector<const Lardon3DDenseMvsSourceImage *> ordered;
  ordered.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (images[i].image_id == 0) return false;
    ordered.push_back(&images[i]);
  }
  std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) {
    return a->image_id < b->image_id;
  });
  for (size_t i = 1; i < count; ++i)
    if (ordered[i - 1]->image_id == ordered[i]->image_id) return false;
  std::vector<unsigned char> record(20 + count * kRecordSize, 0);
  std::memcpy(record.data(), "L3DMCAL1", 8);
  size_t at = 8;
  if (!put_u32(record.data(), record.size(), &at, 1) ||
      !put_u64(record.data(), record.size(), &at, count))
    return false;
  for (const auto *image : ordered) {
    const auto &cal = image->calibration;
    if (!put_u64(record.data(), record.size(), &at, image->image_id) ||
        !put_u32(record.data(), record.size(), &at, cal.width) ||
        !put_u32(record.data(), record.size(), &at, cal.height) ||
        !put_f64(record.data(), record.size(), &at, cal.fx) ||
        !put_f64(record.data(), record.size(), &at, cal.fy) ||
        !put_f64(record.data(), record.size(), &at, cal.cx) ||
        !put_f64(record.data(), record.size(), &at, cal.cy) ||
        !put_f64(record.data(), record.size(), &at, cal.k1) ||
        !put_f64(record.data(), record.size(), &at, cal.k2) ||
        !put_f64(record.data(), record.size(), &at, cal.p1) ||
        !put_f64(record.data(), record.size(), &at, cal.p2))
      return false;
  }
  return at == record.size() && sha(record.data(), record.size(), digest);
}

extern "C" bool lardon3d_dense_mvs_calibration_binding_identity(
    const Lardon3DDenseMvsSourceImage *images, size_t count,
    unsigned char digest[32]) {
  try {
    return lardon3d::dense_mvs_detail::calibration_binding_identity(
        images, count, digest);
  } catch (const std::bad_alloc &) {
    return false;
  } catch (...) {
    return false;
  }
}
bool lardon3d::dense_mvs_detail::source_image_set_identity(
    const Lardon3DDenseMvsSourceImage *images, size_t count,
    unsigned char digest[32]) {
  if (!images || !digest || count == 0 || count > (SIZE_MAX - 20) / 40)
    return false;
  std::vector<const Lardon3DDenseMvsSourceImage *> ordered;
  ordered.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (images[i].image_id == 0 || !images[i].source_path ||
        !nonzero(images[i].immutable_sha256))
      return false;
    ordered.push_back(&images[i]);
  }
  std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) {
    return a->image_id < b->image_id;
  });
  for (size_t i = 1; i < count; ++i)
    if (ordered[i - 1]->image_id == ordered[i]->image_id) return false;
  std::vector<unsigned char> record(20 + count * 40, 0);
  std::memcpy(record.data(), "L3DMSRC1", 8);
  size_t at = 8;
  if (!put_u32(record.data(), record.size(), &at, 1) ||
      !put_u64(record.data(), record.size(), &at, count))
    return false;
  for (const auto *image : ordered) {
    if (!put_u64(record.data(), record.size(), &at, image->image_id))
      return false;
    std::memcpy(record.data() + at, image->immutable_sha256, 32);
    at += 32;
  }
  return at == record.size() && sha(record.data(), record.size(), digest);
}

extern "C" bool lardon3d_dense_mvs_source_image_set_identity(
    const Lardon3DDenseMvsSourceImage *images, size_t count,
    unsigned char digest[32]) {
  try {
    return lardon3d::dense_mvs_detail::source_image_set_identity(
        images, count, digest);
  } catch (const std::bad_alloc &) {
    return false;
  } catch (...) {
    return false;
  }
}
