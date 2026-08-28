#include <lardon3d/raw_development.h>
extern "C" {
#include <lardon3d/image_catalog.h>
}

#include <libraw/libraw.h>
#include <libdeflate.h>
#include <opencv2/core/version.hpp>
#include <opencv2/imgcodecs.hpp>
#include <openssl/evp.h>
#include <png.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr uint32_t kMaxDimension = 16384;
constexpr uint64_t kMaxPixels = 40000000;
constexpr uint64_t kMaxSourceBytes = UINT64_C(1024) * 1024U * 1024U;
constexpr int kPngCompression = 6;
constexpr int kPngStrategy = cv::IMWRITE_PNG_STRATEGY_DEFAULT;
constexpr uint32_t kBackendKindLibRaw = 1;
constexpr uint32_t kEncoderKindOpenCvPng = 1;
constexpr size_t kRawFingerprintPayloadSize = 200;

// RAW Policy v1 is part of derived scientific identity.  Its fixed
// little-endian L3DRAWD1 payload is not a pathname-derived source identity.
bool valid_wb(const float wb[4]) {
  for (size_t i = 0; i < 4; ++i) if (!std::isfinite(wb[i]) || wb[i] <= 0.0F) return false;
  return true;
}
void put_u32(std::vector<unsigned char> *bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes->push_back((unsigned char)(value >> (8U * i)));
}
void put_float(std::vector<unsigned char> *bytes, float value) {
  uint32_t bits = 0; static_assert(sizeof(bits) == sizeof(value), "float width");
  std::memcpy(&bits, &value, sizeof(bits)); put_u32(bytes, bits);
}
bool sha256(const std::vector<unsigned char> &bytes, unsigned char out[32]) {
  unsigned int size = 0;
  return EVP_Digest(bytes.data(), bytes.size(), out, &size, EVP_sha256(), nullptr) == 1 && size == 32;
}
bool checked_rgb_size(uint32_t width, uint32_t height, size_t *bytes) {
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) return false;
  uint64_t pixels = (uint64_t)width * height;
  if (pixels > kMaxPixels || pixels > SIZE_MAX / 3U) return false;
  *bytes = (size_t)pixels * 3U; return true;
}
Lardon3DRawDevelopmentResult catalog_result(Lardon3DImageCatalogAssetPublishResult result) {
  switch (result) {
    case LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED: return LARDON3D_RAW_DEVELOPMENT_OK;
    case LARDON3D_IMAGE_CATALOG_ASSET_INVALID_ARGUMENT: return LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT;
    case LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_ERROR: return LARDON3D_RAW_DEVELOPMENT_SOURCE_NOT_FOUND;
    case LARDON3D_IMAGE_CATALOG_ASSET_SOURCE_CHANGED: return LARDON3D_RAW_DEVELOPMENT_SOURCE_CHANGED;
    case LARDON3D_IMAGE_CATALOG_ASSET_TOO_LARGE: return LARDON3D_RAW_DEVELOPMENT_OUTPUT_LIMIT_EXCEEDED;
    case LARDON3D_IMAGE_CATALOG_ASSET_PUBLICATION_ERROR: return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    case LARDON3D_IMAGE_CATALOG_ASSET_DB_ERROR: return LARDON3D_RAW_DEVELOPMENT_DB_ERROR;
  }
  return LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR;
}
Lardon3DRawDevelopmentResult db_result(Lardon3DProjectDbResult result) {
  if (result == LARDON3D_PROJECT_DB_OK) return LARDON3D_RAW_DEVELOPMENT_OK;
  if (result == LARDON3D_PROJECT_DB_CONSTRAINT) return LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;
  if (result == LARDON3D_PROJECT_DB_NOT_FOUND) return LARDON3D_RAW_DEVELOPMENT_SOURCE_NOT_FOUND;
  return LARDON3D_RAW_DEVELOPMENT_DB_ERROR;
}
bool same_scientific_derivation(const Lardon3DProjectDbAssetDerivation &stored,
                                const Lardon3DProjectDbAssetDerivation &expected) {
  return stored.parent_asset_id == expected.parent_asset_id
      && stored.child_asset_id == expected.child_asset_id && stored.kind == expected.kind
      && stored.version == expected.version
      && std::memcmp(stored.parameter_fingerprint, expected.parameter_fingerprint,
                     sizeof(expected.parameter_fingerprint)) == 0;
}
Lardon3DRawDevelopmentResult ensure_derivation(Lardon3DProjectDb *database,
                                                const Lardon3DProjectDbAssetDerivation &expected) {
  Lardon3DProjectDbAssetDerivation stored = {};
  Lardon3DProjectDbResult load = lardon3d_project_db_load_asset_derivation(
      database, expected.child_asset_id, &stored);
  if (load == LARDON3D_PROJECT_DB_NOT_FOUND)
    return db_result(lardon3d_project_db_record_asset_derivation(database, &expected));
  if (load != LARDON3D_PROJECT_DB_OK) return LARDON3D_RAW_DEVELOPMENT_DB_ERROR;
  return same_scientific_derivation(stored, expected) ? LARDON3D_RAW_DEVELOPMENT_OK
                                                       : LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;
}
Lardon3DRawDevelopmentResult ensure_capture_asset_role(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t asset_id,
    Lardon3DProjectDbCaptureAssetRole expected_role) {
  Lardon3DProjectDbResult attach = lardon3d_project_db_attach_capture_asset(
      database, capture_id, asset_id, expected_role);
  if (attach == LARDON3D_PROJECT_DB_OK) return LARDON3D_RAW_DEVELOPMENT_OK;
  if (attach != LARDON3D_PROJECT_DB_CONSTRAINT) return db_result(attach);

  Lardon3DProjectDbCaptureAsset page[LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX];
  uint64_t after_asset_id = 0;
  for (;;) {
    size_t count = 0;
    Lardon3DProjectDbResult listed = lardon3d_project_db_list_capture_assets(
        database, capture_id, after_asset_id, page,
        LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX, &count);
    if (listed != LARDON3D_PROJECT_DB_OK) return LARDON3D_RAW_DEVELOPMENT_DB_ERROR;
    for (size_t index = 0; index < count; ++index) {
      if (page[index].asset_id == asset_id)
        return page[index].role == expected_role ? LARDON3D_RAW_DEVELOPMENT_OK
                                                 : LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;
      if (page[index].asset_id > asset_id) return LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;
    }
    if (count < LARDON3D_PROJECT_DB_CATALOG_PAGE_MAX) return LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;
    after_asset_id = page[count - 1].asset_id;
  }
}
Lardon3DRawDevelopmentResult libraw_result(int code) {
  if (code == LIBRAW_FILE_UNSUPPORTED) return LARDON3D_RAW_DEVELOPMENT_UNSUPPORTED_RAW;
  if (code == LIBRAW_UNSUFFICIENT_MEMORY) return LARDON3D_RAW_DEVELOPMENT_OUT_OF_MEMORY;
  if (code == LIBRAW_DATA_ERROR || code == LIBRAW_IO_ERROR) return LARDON3D_RAW_DEVELOPMENT_CORRUPT_RAW;
  return LARDON3D_RAW_DEVELOPMENT_DECODER_FAILURE;
}
bool write_all(int fd, const unsigned char *data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    ssize_t written = write(fd, data + offset, size - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += (size_t)written;
  }
  return true;
}
bool asset_file_path(const Lardon3DAppState *state, const Lardon3DProjectDbImageAsset &asset,
                     char path[LARDON3D_APP_STATE_PATH_CAPACITY]) {
  int count = std::snprintf(path, LARDON3D_APP_STATE_PATH_CAPACITY, "%s/%s",
                            state->project_path, asset.path);
  return count > 0 && count < LARDON3D_APP_STATE_PATH_CAPACITY;
}
bool verify_managed_asset(const char *path, const unsigned char expected[32]) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat info; EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && context
      && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  unsigned char buffer[65536];
  while (ok) {
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) { ok = false; break; }
    if (count == 0) break;
    ok = EVP_DigestUpdate(context, buffer, (size_t)count) == 1;
  }
  unsigned char actual[32]; unsigned int length = 0;
  if (ok) ok = EVP_DigestFinal_ex(context, actual, &length) == 1 && length == sizeof(actual)
      && std::memcmp(actual, expected, sizeof(actual)) == 0;
  EVP_MD_CTX_free(context); (void)close(fd); return ok;
}
bool valid_name(const char *path, char name[LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY]) {
  const char *base = std::strrchr(path, '/'); base = base ? base + 1 : path;
  size_t size = strnlen(base, LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY);
  if (size == 0 || size >= LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY || std::strchr(base, '\\')
      || std::strchr(base, '\t') || std::strchr(base, '\r') || std::strchr(base, '\n')) return false;
  std::memcpy(name, base, size + 1); return true;
}
}

extern "C" Lardon3DRawDevelopmentResult lardon3d_raw_development_policy_fingerprint(
    const float wb[4], unsigned char fingerprint[32]) {
  if (!wb || !fingerprint || !valid_wb(wb)) return LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT;
  try {
    if (std::strcmp(png_get_libpng_ver(nullptr), PNG_LIBPNG_VER_STRING) != 0)
      return LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR;
    std::vector<unsigned char> bytes;
    bytes.reserve(kRawFingerprintPayloadSize);
    // Record all policy choices and relevant backend versions; decoder
    // defaults must never create an untracked representation.
    const char magic[] = "L3DRAWD1";
    bytes.insert(bytes.end(), magic, magic + sizeof(magic) - 1);
    put_u32(&bytes, 1); put_u32(&bytes, kBackendKindLibRaw);
    put_u32(&bytes, LIBRAW_VERSION); put_u32(&bytes, libraw_versionNumber());
    put_u32(&bytes, 3); put_u32(&bytes, 0); put_u32(&bytes, 0);
    for (size_t i = 0; i < 4; ++i) put_float(&bytes, wb[i]);
    put_u32(&bytes, 0); put_u32(&bytes, 0); put_u32(&bytes, 0); put_u32(&bytes, 1);
    put_float(&bytes, 1.0F / 2.4F); put_float(&bytes, 12.92F);
    put_u32(&bytes, 1); put_float(&bytes, 1.0F); put_float(&bytes, 0.0F);
    put_u32(&bytes, 0); put_float(&bytes, 1.0F); put_float(&bytes, 0.0F);
    put_u32(&bytes, 0); put_float(&bytes, 0.0F); put_u32(&bytes, 0); put_u32(&bytes, 0);
    put_u32(&bytes, 0); put_u32(&bytes, 0); put_u32(&bytes, 0); put_u32(&bytes, 0);
    put_u32(&bytes, 8); put_u32(&bytes, 3); put_u32(&bytes, kEncoderKindOpenCvPng);
    put_u32(&bytes, kPngCompression);
    put_u32(&bytes, kPngStrategy); put_u32(&bytes, 0); put_u32(&bytes, kMaxDimension);
    put_u32(&bytes, (uint32_t)kMaxPixels);
    put_u32(&bytes, (uint32_t)(kMaxSourceBytes >> 20)); put_u32(&bytes, CV_VERSION_MAJOR);
    put_u32(&bytes, CV_VERSION_MINOR); put_u32(&bytes, CV_VERSION_REVISION);
    put_u32(&bytes, PNG_LIBPNG_VER_MAJOR); put_u32(&bytes, PNG_LIBPNG_VER_MINOR);
    put_u32(&bytes, PNG_LIBPNG_VER_RELEASE);
    put_u32(&bytes, LIBDEFLATE_VERSION_MAJOR); put_u32(&bytes, LIBDEFLATE_VERSION_MINOR);
    if (bytes.size() != kRawFingerprintPayloadSize)
      return LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR;
    return sha256(bytes, fingerprint) ? LARDON3D_RAW_DEVELOPMENT_OK
                                      : LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR;
  } catch (const std::bad_alloc &) {
    return LARDON3D_RAW_DEVELOPMENT_OUT_OF_MEMORY;
  } catch (...) {
    return LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR;
  }
}

#ifdef LARDON3D_RAW_DEVELOPMENT_TESTING
extern "C" size_t lardon3d_raw_development_test_policy_payload_size(void) {
  return kRawFingerprintPayloadSize;
}
extern "C" Lardon3DRawDevelopmentResult
lardon3d_raw_development_test_ensure_derivation(
    Lardon3DProjectDb *database, const Lardon3DProjectDbAssetDerivation *expected) {
  return !database || !expected ? LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT
                                : ensure_derivation(database, *expected);
}
extern "C" Lardon3DRawDevelopmentResult
lardon3d_raw_development_test_ensure_capture_asset_role(
    Lardon3DProjectDb *database, uint64_t capture_id, uint64_t asset_id,
    Lardon3DProjectDbCaptureAssetRole expected_role) {
  return ensure_capture_asset_role(database, capture_id, asset_id, expected_role);
}
#endif

extern "C" Lardon3DRawDevelopmentResult lardon3d_raw_develop_to_capture(
    Lardon3DAppState *state, uint64_t capture_id, const char *source_arw_path,
    uint64_t producer_task_id, int64_t created_at, Lardon3DRawDevelopmentOutput *output) {
  if (!state || !state->project_loaded || !state->project_db || capture_id == 0 || !source_arw_path
      || !source_arw_path[0] || created_at < 0 || !output) return LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT;
  std::memset(output, 0, sizeof(*output));
  try {
    Lardon3DProjectDbCapture capture;
    Lardon3DRawDevelopmentResult result = db_result(lardon3d_project_db_load_capture(
        state->project_db, capture_id, &capture));
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    result = catalog_result(lardon3d_image_catalog_publish_asset_file(
        state, source_arw_path, created_at, kMaxSourceBytes, &output->source_asset));
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    result = ensure_capture_asset_role(state->project_db, capture_id,
        output->source_asset.asset_id, LARDON3D_DB_CAPTURE_ASSET_SOURCE);
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    char managed_source[LARDON3D_APP_STATE_PATH_CAPACITY];
    if (!asset_file_path(state, output->source_asset, managed_source)) return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    if (!verify_managed_asset(managed_source, output->source_asset.sha256)) return LARDON3D_RAW_DEVELOPMENT_SOURCE_CHANGED;
    LibRaw raw;
    raw.imgdata.params.user_qual = 3; raw.imgdata.params.half_size = 0;
    // The resolved WB is explicit.  Automatic WB, brightness, and color
    // choices are disabled so policy rather than hidden decoder state defines
    // the derived asset.
    raw.imgdata.params.user_flip = 0; raw.imgdata.params.use_auto_wb = 0;
    raw.imgdata.params.use_camera_wb = 0; raw.imgdata.params.use_camera_matrix = 0;
    raw.imgdata.params.output_profile = nullptr; raw.imgdata.params.camera_profile = nullptr;
    raw.imgdata.params.output_color = 1; raw.imgdata.params.gamm[0] = 1.0 / 2.4;
    raw.imgdata.params.gamm[1] = 12.92; raw.imgdata.params.no_auto_bright = 1;
    raw.imgdata.params.bright = 1.0F; raw.imgdata.params.adjust_maximum_thr = 0.0F;
    raw.imgdata.params.exp_correc = 0; raw.imgdata.params.exp_shift = 1.0F;
    raw.imgdata.params.exp_preser = 0.0F; raw.imgdata.params.highlight = 0;
    raw.imgdata.params.threshold = 0.0F; raw.imgdata.params.fbdd_noiserd = 0;
    raw.imgdata.params.med_passes = 0; raw.imgdata.params.output_bps = 8;
    raw.imgdata.params.output_tiff = 0; raw.imgdata.params.user_black = 0;
    raw.imgdata.params.user_sat = 0; raw.imgdata.params.no_auto_scale = 0;
    raw.imgdata.params.no_interpolation = 0;
    for (size_t i = 0; i < 4; ++i) raw.imgdata.params.aber[i] = 1.0;
    int code = raw.open_file(managed_source);
    if (code != LIBRAW_SUCCESS) return libraw_result(code);
    size_t decoded_size = 0;
    if (!checked_rgb_size(raw.imgdata.sizes.iwidth, raw.imgdata.sizes.iheight, &decoded_size))
      return LARDON3D_RAW_DEVELOPMENT_OUTPUT_LIMIT_EXCEEDED;
    code = raw.unpack(); if (code != LIBRAW_SUCCESS) return libraw_result(code);
    for (size_t i = 0; i < 4; ++i) output->resolved_camera_wb[i] = raw.imgdata.color.cam_mul[i];
    if (!valid_wb(output->resolved_camera_wb)) return LARDON3D_RAW_DEVELOPMENT_UNSUPPORTED_RAW;
    for (size_t i = 0; i < 4; ++i) raw.imgdata.params.user_mul[i] = output->resolved_camera_wb[i];
    result = lardon3d_raw_development_policy_fingerprint(output->resolved_camera_wb,
        output->parameter_fingerprint);
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    code = raw.dcraw_process(); if (code != LIBRAW_SUCCESS) return libraw_result(code);
    int image_error = LIBRAW_SUCCESS;
    libraw_processed_image_t *processed = raw.dcraw_make_mem_image(&image_error);
    if (!processed) return libraw_result(image_error);
    output->width = processed->width; output->height = processed->height;
    size_t rgb_size = 0;
    if (processed->type != LIBRAW_IMAGE_BITMAP || processed->colors != 3
        || !checked_rgb_size(output->width, output->height, &rgb_size)
        || processed->data_size != rgb_size) { LibRaw::dcraw_clear_mem(processed); return LARDON3D_RAW_DEVELOPMENT_OUTPUT_LIMIT_EXCEEDED; }
    std::vector<unsigned char> bgr(rgb_size);
    for (size_t i = 0; i < rgb_size; i += 3) { bgr[i] = processed->data[i + 2]; bgr[i + 1] = processed->data[i + 1]; bgr[i + 2] = processed->data[i]; }
    LibRaw::dcraw_clear_mem(processed);
    cv::Mat image((int)output->height, (int)output->width, CV_8UC3, bgr.data());
    std::vector<unsigned char> png;
    std::vector<int> parameters = {cv::IMWRITE_PNG_COMPRESSION, kPngCompression,
        cv::IMWRITE_PNG_STRATEGY, kPngStrategy};
    if (!cv::imencode(".png", image, png, parameters) || png.empty()) return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    cv::Mat validated = cv::imdecode(png, cv::IMREAD_UNCHANGED);
    if (validated.empty() || validated.cols != (int)output->width
        || validated.rows != (int)output->height || validated.type() != CV_8UC3)
      return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    char staging[LARDON3D_APP_STATE_PATH_CAPACITY];
    int written = std::snprintf(staging, sizeof(staging), "%s/assets/images/.raw-png.tmp.XXXXXX", state->project_path);
    if (written <= 0 || written >= (int)sizeof(staging)) return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    int fd = mkstemp(staging); if (fd < 0) return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    bool write_ok = write_all(fd, png.data(), png.size());
    bool sync_ok = write_ok && fsync(fd) == 0;
    int close_result = close(fd);
    if (!write_ok || !sync_ok || close_result != 0) {
      (void)unlink(staging);
      return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    }
    result = catalog_result(lardon3d_image_catalog_publish_asset_file(state, staging, created_at,
        kMaxSourceBytes, &output->derived_asset));
    (void)unlink(staging);
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    Lardon3DProjectDbAssetDerivation derivation = {};
    derivation.parent_asset_id = output->source_asset.asset_id; derivation.child_asset_id = output->derived_asset.asset_id;
    derivation.kind = LARDON3D_DB_ASSET_DERIVATION_GENERIC_VERSIONED; derivation.version = 1;
    std::memcpy(derivation.parameter_fingerprint, output->parameter_fingerprint, sizeof(derivation.parameter_fingerprint));
    derivation.has_producer_task = producer_task_id != 0; derivation.producer_task_id = producer_task_id;
    derivation.created_at = created_at;
    result = ensure_derivation(state->project_db, derivation);
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    char name[LARDON3D_PROJECT_DB_IMAGE_NAME_CAPACITY];
    if (!valid_name(source_arw_path, name)) return LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT;
    Lardon3DProjectDbImageRegisterStatus status;
    result = db_result(lardon3d_project_db_publish_derived_capture_image(state->project_db, capture_id,
        output->derived_asset.asset_id, name, source_arw_path, producer_task_id, created_at, &status, &output->image));
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    std::snprintf(output->libraw_version, sizeof(output->libraw_version), "%s", libraw_version());
    std::snprintf(output->png_encoder_version, sizeof(output->png_encoder_version), "%s", CV_VERSION);
    return LARDON3D_RAW_DEVELOPMENT_OK;
  } catch (const std::bad_alloc &) { return LARDON3D_RAW_DEVELOPMENT_OUT_OF_MEMORY; }
  catch (...) { return LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR; }
}

extern "C" Lardon3DRawDevelopmentResult lardon3d_raw_develop_asset_to_capture(
    Lardon3DAppState *state, uint64_t capture_id, uint64_t source_asset_id,
    uint64_t producer_task_id, int64_t created_at, Lardon3DRawDevelopmentOutput *output) {
  if (!state || !state->project_loaded || !state->project_db || capture_id == 0 ||
      source_asset_id == 0 || created_at < 0 || !output)
    return LARDON3D_RAW_DEVELOPMENT_INVALID_ARGUMENT;
  std::memset(output, 0, sizeof(*output));
  try {
    bool explicit_raw = false;
    uint64_t after_asset_id = source_asset_id - 1u;
    Lardon3DProjectDbCaptureSourceAsset relation{};
    size_t count = 0;
    Lardon3DProjectDbResult listed = lardon3d_project_db_list_capture_source_assets(
        state->project_db, capture_id, after_asset_id, &relation, 1, &count);
    if (listed != LARDON3D_PROJECT_DB_OK) return db_result(listed);
    explicit_raw = count == 1 && relation.asset_id == source_asset_id &&
                   relation.source_kind == LARDON3D_DB_CAPTURE_SOURCE_RAW;
    if (!explicit_raw) return LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;

    Lardon3DProjectDbImageAsset asset{};
    Lardon3DProjectDbResult loaded = lardon3d_project_db_load_image_asset(
        state->project_db, source_asset_id, &asset);
    if (loaded != LARDON3D_PROJECT_DB_OK) return db_result(loaded);
    char managed_source[LARDON3D_APP_STATE_PATH_CAPACITY];
    if (!asset_file_path(state, asset, managed_source)) return LARDON3D_RAW_DEVELOPMENT_IO_ERROR;
    if (!verify_managed_asset(managed_source, asset.sha256))
      return LARDON3D_RAW_DEVELOPMENT_SOURCE_CHANGED;

    // The pathname passed to the frozen developer is resolved only after the
    // explicit durable ID and RAW relation are validated. Republishing these
    // immutable managed bytes must converge to that same asset ID.
    Lardon3DRawDevelopmentResult result = lardon3d_raw_develop_to_capture(
        state, capture_id, managed_source, producer_task_id, created_at, output);
    if (result != LARDON3D_RAW_DEVELOPMENT_OK) return result;
    return output->source_asset.asset_id == source_asset_id
               ? LARDON3D_RAW_DEVELOPMENT_OK
               : LARDON3D_RAW_DEVELOPMENT_CONSTRAINT;
  } catch (const std::bad_alloc &) {
    return LARDON3D_RAW_DEVELOPMENT_OUT_OF_MEMORY;
  } catch (...) {
    return LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR;
  }
}
