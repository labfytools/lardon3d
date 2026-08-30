#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>

static const unsigned char feature_magic[8] = {'L', '3', 'D', 'F', 'E', 'A', 'T', 0};
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "Feature File v1/v2 exige IEEE-754 binary32.");

/* L3DFEAT v1/v2 uses explicit fixed-width little-endian headers, never native
 * structs.  Counts, offsets, dimensions, source SHA-256, and parameter
 * fingerprint bind descriptors to their input; malformed/truncated or unknown
 * versions are rejected before feature data is consumed. */

struct Lardon3DFeatureReader {
  int descriptor;
  uint64_t keypoint_offset;
  uint64_t descriptor_offset;
  Lardon3DFeatureFileMetadata metadata;
};

static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}
static void put_u64(unsigned char *p, uint64_t v) {
  for (unsigned int i = 0; i < 8; ++i) {
    p[i] = (unsigned char)(v >> (8U * i));
  }
}
static uint32_t get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const unsigned char *p) {
  uint64_t v = 0;
  for (unsigned int i = 0; i < 8; ++i) {
    v |= (uint64_t)p[i] << (8U * i);
  }
  return v;
}
static void put_float(unsigned char *p, float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  put_u32(p, bits);
}
static float get_float(const unsigned char *p) {
  uint32_t bits = get_u32(p);
  float value = 0;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static bool write_all(int fd, const void *data, size_t size) {
  const unsigned char *bytes = data;
  size_t used = 0;
  while (used < size) {
    ssize_t n = write(fd, bytes + used, size - used);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    used += (size_t)n;
  }
  return true;
}

static bool read_exact(int fd, void *data, size_t size, off_t offset) {
  unsigned char *bytes = data;
  size_t used = 0;
  while (used < size) {
    ssize_t n = pread(fd, bytes + used, size - used, offset + (off_t)used);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    used += (size_t)n;
  }
  return true;
}

static bool sha256_fd(int fd, unsigned char output[32]) {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (!context) {
    return false;
  }
  bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
  unsigned char buffer[65536];
  off_t offset = 0;
  while (ok) {
    ssize_t n = pread(fd, buffer, sizeof(buffer), offset);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    ok = EVP_DigestUpdate(context, buffer, (size_t)n) == 1;
    offset += (off_t)n;
  }
  unsigned int length = 0;
  ok = ok && EVP_DigestFinal_ex(context, output, &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  return ok;
}

static void hex_sha(const unsigned char hash[32], char text[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    text[2 * i] = digits[hash[i] >> 4];
    text[2 * i + 1] = digits[hash[i] & 15];
  }
  text[64] = '\0';
}

static bool canonical_feature_path(const unsigned char hash[32], const char *path) {
  if (!hash || !path) {
    return false;
  }
  char hex[65];
  hex_sha(hash, hex);
  char expected[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  int written = snprintf(expected, sizeof(expected), "assets/features/%.2s/%s", hex, hex);
  return written > 0 && (size_t)written < sizeof(expected) && strcmp(path, expected) == 0;
}

static bool join(char output[PATH_MAX], const char *a, const char *b) {
  int n = snprintf(output, PATH_MAX, "%s/%s", a, b);
  return n > 0 && (size_t)n < PATH_MAX;
}

static bool ensure_directory(const char *path) {
  if (mkdir(path, 0755) == 0) {
    return true;
  }
  if (errno != EEXIST) {
    return false;
  }
  struct stat info;
  return lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode);
}

static bool sync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  bool ok = fsync(fd) == 0;
  if (close(fd) != 0) {
    ok = false;
  }
  return ok;
}

static bool sync_feature_asset_directory(const char *project_path,
                                         const Lardon3DProjectDbFeatureSet *feature_set) {
  char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  int written = snprintf(relative, sizeof(relative), "%s", feature_set->asset.path);
  if (written <= 0 || (size_t)written >= sizeof(relative)) {
    return false;
  }
  char *separator = strrchr(relative, '/');
  if (!separator) {
    return false;
  }
  *separator = '\0';
  char directory[PATH_MAX];
  return join(directory, project_path, relative) && sync_directory(directory);
}

static bool valid_features(const Lardon3DExtractedFeatures *features) {
  if (!features || features->feature_count > LARDON3D_FEATURE_MAX_FEATURES ||
      features->image_width == 0 || features->image_height == 0 ||
      (features->feature_count > 0 && (!features->keypoints || !features->descriptors))) {
    return false;
  }
  for (uint32_t i = 0; i < features->feature_count; ++i) {
    const Lardon3DFeatureKeypoint *p = &features->keypoints[i];
    if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->size) || !isfinite(p->angle_degrees) ||
        !isfinite(p->response) || p->x < 0 || p->y < 0 || p->x >= (float)features->image_width ||
        p->y >= (float)features->image_height || p->size <= 0 || p->angle_degrees < 0 ||
        p->angle_degrees >= 360 || p->octave < -128 || p->octave > 127) {
      return false;
    }
  }
  return true;
}

static bool write_feature_file(int fd, const Lardon3DProjectDbImageAsset *image_asset,
                               const unsigned char fingerprint[32],
                               const Lardon3DExtractedFeatures *features, uint64_t *file_size) {
  uint64_t kp_bytes = (uint64_t)features->feature_count * LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE;
  uint64_t descriptor_bytes =
      (uint64_t)features->feature_count * LARDON3D_FEATURE_DESCRIPTOR_DIMENSION;
  uint64_t descriptor_offset = LARDON3D_FEATURE_FILE_HEADER_SIZE + kp_bytes;
  uint64_t total = descriptor_offset + descriptor_bytes;
  if (total > LARDON3D_FEATURE_FILE_MAX_SIZE) {
    return false;
  }
  unsigned char header[LARDON3D_FEATURE_FILE_HEADER_SIZE] = {0};
  memcpy(header, feature_magic, 8);
  put_u32(header + 8, LARDON3D_FEATURE_FILE_VERSION);
  put_u32(header + 12, LARDON3D_FEATURE_FILE_HEADER_SIZE);
  put_u32(header + 16, features->feature_count);
  put_u32(header + 20, LARDON3D_FEATURE_DESCRIPTOR_DIMENSION);
  put_u32(header + 24, LARDON3D_FEATURE_DESCRIPTOR_U8);
  put_u32(header + 28, LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE);
  put_u32(header + 32, features->image_width);
  put_u32(header + 36, features->image_height);
  put_u64(header + 40, LARDON3D_FEATURE_FILE_HEADER_SIZE);
  put_u64(header + 48, descriptor_offset);
  put_u64(header + 56, total);
  memcpy(header + 64, image_asset->sha256, 32);
  memcpy(header + 96, fingerprint, 32);
  memcpy(header + 128, LARDON3D_FEATURE_EXTRACTOR_KIND, 4);
  put_u32(header + 144, LARDON3D_FEATURE_EXTRACTOR_VERSION);
  if (!write_all(fd, header, sizeof(header))) {
    return false;
  }
  unsigned char record[LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE];
  for (uint32_t i = 0; i < features->feature_count; ++i) {
    const Lardon3DFeatureKeypoint *p = &features->keypoints[i];
    put_float(record, p->x);
    put_float(record + 4, p->y);
    put_float(record + 8, p->size);
    put_float(record + 12, p->angle_degrees);
    put_float(record + 16, p->response);
    put_u32(record + 20, (uint32_t)p->octave);
    if (!write_all(fd, record, sizeof(record))) {
      return false;
    }
  }
  if (descriptor_bytes > 0 && !write_all(fd, features->descriptors, (size_t)descriptor_bytes)) {
    return false;
  }
  *file_size = total;
  return true;
}

static bool valid_f32_descriptors(const Lardon3DExtractedFeatures *features, uint32_t dimension) {
  if (dimension != 128 ||
      features->descriptor_bytes != (size_t)features->feature_count * dimension * sizeof(float))
    return false;
  size_t count = (size_t)features->feature_count * dimension;
  for (size_t i = 0; i < count; ++i) {
    float value;
    memcpy(&value, features->descriptors + i * sizeof(value), sizeof(value));
    if (!isfinite(value)) return false;
  }
  return true;
}

static bool write_feature_file_v2(
    int fd, const Lardon3DProjectDbImageAsset *image_asset, const char *extractor_kind,
    uint32_t extractor_version, const unsigned char fingerprint[32],
    Lardon3DFeatureDescriptorType descriptor_type, uint32_t descriptor_dimension,
    uint32_t capabilities, const Lardon3DExtractedFeatures *features, uint64_t *file_size) {
  uint32_t scalar = descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 1U : 4U;
  size_t kind_length = strnlen(extractor_kind, 16);
  if (kind_length == 0 || kind_length >= 16 || extractor_version == 0 ||
      descriptor_dimension == 0 || descriptor_dimension > 4096 ||
      (descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_U8 &&
       descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_F32) ||
      (descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32 &&
       !valid_f32_descriptors(features, descriptor_dimension))) {
    return false;
  }
  uint64_t keypoint_bytes =
      (uint64_t)features->feature_count * LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE;
  uint64_t descriptor_bytes =
      (uint64_t)features->feature_count * descriptor_dimension * scalar;
  uint64_t descriptor_offset = LARDON3D_FEATURE_FILE_V2_HEADER_SIZE + keypoint_bytes;
  uint64_t total = descriptor_offset + descriptor_bytes;
  if (total > LARDON3D_FEATURE_FILE_MAX_SIZE) return false;
  unsigned char header[LARDON3D_FEATURE_FILE_V2_HEADER_SIZE] = {0};
  memcpy(header, feature_magic, 8);
  put_u32(header + 8, LARDON3D_FEATURE_FILE_VERSION_V2);
  put_u32(header + 12, LARDON3D_FEATURE_FILE_V2_HEADER_SIZE);
  put_u32(header + 16, features->feature_count);
  put_u32(header + 20, descriptor_dimension);
  put_u32(header + 24, (uint32_t)descriptor_type);
  put_u32(header + 28, scalar);
  put_u32(header + 32, LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE);
  put_u32(header + 36, features->image_width);
  put_u32(header + 40, features->image_height);
  put_u32(header + 44, capabilities);
  put_u64(header + 48, LARDON3D_FEATURE_FILE_V2_HEADER_SIZE);
  put_u64(header + 56, descriptor_offset);
  put_u64(header + 64, total);
  memcpy(header + 72, image_asset->sha256, 32);
  memcpy(header + 104, fingerprint, 32);
  memcpy(header + 136, extractor_kind, kind_length);
  put_u32(header + 152, extractor_version);
  if (!write_all(fd, header, sizeof(header))) return false;
  unsigned char record[LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE];
  for (uint32_t i = 0; i < features->feature_count; ++i) {
    const Lardon3DFeatureKeypoint *point = &features->keypoints[i];
    put_float(record, point->x);
    put_float(record + 4, point->y);
    put_float(record + 8, point->size);
    put_float(record + 12, point->angle_degrees);
    put_float(record + 16, point->response);
    put_u32(record + 20, (uint32_t)point->octave);
    if (!write_all(fd, record, sizeof(record))) return false;
  }
  if (descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8) {
    if (descriptor_bytes && !write_all(fd, features->descriptors, (size_t)descriptor_bytes))
      return false;
  } else {
    unsigned char encoded[4];
    size_t value_count = (size_t)features->feature_count * descriptor_dimension;
    for (size_t i = 0; i < value_count; ++i) {
      float value;
      memcpy(&value, features->descriptors + i * sizeof(value), sizeof(value));
      put_float(encoded, value);
      if (!write_all(fd, encoded, sizeof(encoded))) return false;
    }
  }
  *file_size = total;
  return true;
}

static Lardon3DFeatureStoreResult
reuse_existing_feature_set(Lardon3DAppState *state, uint64_t image_id, uint64_t producer_task_id,
                           const char *kind, uint32_t version,
                           const unsigned char fingerprint[32],
                           Lardon3DProjectDbFeatureSet *feature_set) {
  if (feature_set->asset.durability == LARDON3D_DB_FEATURE_ASSET_DURABLE) {
    return LARDON3D_FEATURE_STORE_ALREADY_PRESENT;
  }
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  Lardon3DFeatureStoreResult validation =
      lardon3d_feature_reader_open(state->project_path, feature_set, &reader, &metadata);
  lardon3d_feature_reader_close(reader);
  if (validation != LARDON3D_FEATURE_STORE_OK ||
      !sync_feature_asset_directory(state->project_path, feature_set)) {
    return LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
  }
  int64_t now = (int64_t)time(NULL);
  if (now < 0) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  Lardon3DProjectDbResult promoted = lardon3d_project_db_register_feature_set_quality(
      state->project_db, image_id, kind, version, fingerprint, feature_set->source_image_sha256,
      feature_set->feature_count, feature_set->descriptor_type, feature_set->descriptor_dimension,
      feature_set->occupied_cells, feature_set->total_cells, feature_set->coverage_ratio,
      feature_set->feature_density_per_megapixel,
      feature_set->asset.sha256, feature_set->asset.path, feature_set->asset.size_bytes,
      LARDON3D_DB_FEATURE_ASSET_DURABLE, producer_task_id, now, feature_set);
  return promoted == LARDON3D_PROJECT_DB_OK ? LARDON3D_FEATURE_STORE_ALREADY_PRESENT
                                            : LARDON3D_FEATURE_STORE_DB_ERROR;
}

static Lardon3DFeatureStoreResult register_published_feature_set(
    Lardon3DAppState *state, uint64_t image_id, uint64_t producer_task_id,
    const Lardon3DProjectDbImageAsset *image_asset, const char *kind, uint32_t version,
    const unsigned char fingerprint[32], Lardon3DFeatureDescriptorType descriptor_type,
    uint32_t descriptor_dimension,
    const Lardon3DExtractedFeatures *features, const unsigned char file_hash[32],
    const char *relative_path, uint64_t file_size, bool durable,
    Lardon3DProjectDbFeatureSet *feature_set) {
  int64_t now = (int64_t)time(NULL);
  if (now < 0) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  Lardon3DProjectDbResult result = lardon3d_project_db_register_feature_set_quality(
      state->project_db, image_id, kind, version, fingerprint, image_asset->sha256,
      features->feature_count, descriptor_type, descriptor_dimension, features->quality.occupied_cells,
      features->quality.total_cells, features->quality.coverage_ratio,
      features->quality.feature_density_per_megapixel, file_hash, relative_path, file_size,
      durable ? LARDON3D_DB_FEATURE_ASSET_DURABLE : LARDON3D_DB_FEATURE_ASSET_PUBLISHED_NOT_DURABLE,
      producer_task_id, now, feature_set);
  if (result != LARDON3D_PROJECT_DB_OK) {
    return result == LARDON3D_PROJECT_DB_BUSY ? LARDON3D_FEATURE_STORE_DB_BUSY
                                              : LARDON3D_FEATURE_STORE_DB_ERROR;
  }
  return durable ? LARDON3D_FEATURE_STORE_OK : LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE;
}

Lardon3DFeatureStoreResult lardon3d_feature_store_publish(
    Lardon3DAppState *state, uint64_t image_id, uint64_t producer_task_id,
    const Lardon3DFeatureExtractorParameters *parameters, const Lardon3DExtractedFeatures *features,
    Lardon3DProjectDbFeatureSet *feature_set) {
  if (feature_set) {
    memset(feature_set, 0, sizeof(*feature_set));
  }
  if (!state || !state->project_loaded || !state->project_db || !feature_set || image_id == 0 ||
      !lardon3d_feature_extractor_parameters_valid(parameters) || !valid_features(features) ||
      features->descriptor_bytes !=
          (size_t)features->feature_count * LARDON3D_FEATURE_DESCRIPTOR_DIMENSION) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  unsigned char fingerprint[32];
  lardon3d_feature_extractor_parameter_fingerprint(parameters, fingerprint);
  Lardon3DProjectDbResult found = lardon3d_project_db_find_feature_set(
      state->project_db, image_id, LARDON3D_FEATURE_EXTRACTOR_KIND,
      LARDON3D_FEATURE_EXTRACTOR_VERSION, fingerprint, feature_set);
  if (found == LARDON3D_PROJECT_DB_OK) {
    return reuse_existing_feature_set(state, image_id, producer_task_id,
                                      LARDON3D_FEATURE_EXTRACTOR_KIND,
                                      LARDON3D_FEATURE_EXTRACTOR_VERSION, fingerprint, feature_set);
  }
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND) {
    return found == LARDON3D_PROJECT_DB_BUSY ? LARDON3D_FEATURE_STORE_DB_BUSY
                                             : LARDON3D_FEATURE_STORE_DB_ERROR;
  }
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset image_asset;
  if (lardon3d_project_db_load_image(state->project_db, image_id, &image, &image_asset) !=
      LARDON3D_PROJECT_DB_OK) {
    return LARDON3D_FEATURE_STORE_NOT_FOUND;
  }
  char assets[PATH_MAX], features_dir[PATH_MAX];
  if (!join(assets, state->project_path, "assets") || !ensure_directory(assets) ||
      !join(features_dir, assets, "features") || !ensure_directory(features_dir)) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  char temporary[PATH_MAX];
  int n = snprintf(temporary, sizeof(temporary), "%s/.feature-XXXXXX", features_dir);
  if (n <= 0 || (size_t)n >= sizeof(temporary)) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  int fd = mkstemp(temporary);
  if (fd < 0) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  uint64_t file_size = 0;
  bool written =
      write_feature_file(fd, &image_asset, fingerprint, features, &file_size) && fsync(fd) == 0;
  unsigned char file_hash[32];
  if (written) {
    written = sha256_fd(fd, file_hash);
  }
  if (close(fd) != 0) {
    written = false;
  }
  if (!written) {
    unlink(temporary);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  char hex[65];
  hex_sha(file_hash, hex);
  char prefix[PATH_MAX];
  n = snprintf(prefix, sizeof(prefix), "%s/%.2s", features_dir, hex);
  if (n <= 0 || (size_t)n >= sizeof(prefix) || !ensure_directory(prefix)) {
    unlink(temporary);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  n = snprintf(relative, sizeof(relative), "assets/features/%.2s/%s", hex, hex);
  char final[PATH_MAX];
  if (n <= 0 || (size_t)n >= sizeof(relative) || !join(final, state->project_path, relative)) {
    unlink(temporary);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  if (link(temporary, final) != 0) {
    if (errno != EEXIST) {
      unlink(temporary);
      return LARDON3D_FEATURE_STORE_IO_ERROR;
    }
    int existing = open(final, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    unsigned char existing_hash[32];
    bool same = existing >= 0 && fstat(existing, &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_size >= 0 && (uint64_t)info.st_size == file_size &&
                sha256_fd(existing, existing_hash) && memcmp(existing_hash, file_hash, 32) == 0;
    if (existing >= 0) {
      (void)close(existing);
    }
    if (!same) {
      unlink(temporary);
      return LARDON3D_FEATURE_STORE_CORRUPT;
    }
  }
  unlink(temporary);
  bool durable = sync_directory(prefix);
#ifdef LARDON3D_FEATURE_STORE_TESTING
  const char *fail_sync = getenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC");
  if (fail_sync && strcmp(fail_sync, "1") == 0) {
    durable = false;
  }
#endif
  return register_published_feature_set(
      state, image_id, producer_task_id, &image_asset, LARDON3D_FEATURE_EXTRACTOR_KIND,
      LARDON3D_FEATURE_EXTRACTOR_VERSION, fingerprint, LARDON3D_FEATURE_DESCRIPTOR_U8,
      LARDON3D_FEATURE_DESCRIPTOR_DIMENSION, features, file_hash, relative, file_size, durable,
      feature_set);
}

Lardon3DFeatureStoreResult lardon3d_feature_store_publish_v2(
    Lardon3DAppState *state, uint64_t image_id, uint64_t producer_task_id, const char *kind,
    uint32_t version, const unsigned char fingerprint[32],
    Lardon3DFeatureDescriptorType descriptor_type, uint32_t descriptor_dimension,
    uint32_t capabilities, const Lardon3DExtractedFeatures *features,
    Lardon3DProjectDbFeatureSet *feature_set) {
  if (feature_set) memset(feature_set, 0, sizeof(*feature_set));
  uint32_t known_capabilities = LARDON3D_FEATURE_HAS_SCALE |
                                LARDON3D_FEATURE_HAS_ORIENTATION |
                                LARDON3D_FEATURE_HAS_RESPONSE | LARDON3D_FEATURE_HAS_OCTAVE;
  if (!state || !state->project_loaded || !state->project_db || !feature_set || image_id == 0 ||
      !kind || !lardon3d_task_kind_is_valid(kind) || version == 0 || !fingerprint ||
      strnlen(kind, 16) >= 16 || descriptor_dimension == 0 || descriptor_dimension > 4096 ||
      ((strcmp(kind, "sift") == 0 || strcmp(kind, "rootsift") == 0) && version != 1) ||
      (capabilities & ~known_capabilities) != 0 ||
      (descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_U8 &&
       descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_F32) || !valid_features(features) ||
      (descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8 &&
       features->descriptor_bytes != (size_t)features->feature_count * descriptor_dimension) ||
      (descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32 &&
       !valid_f32_descriptors(features, descriptor_dimension))) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  Lardon3DProjectDbResult found = lardon3d_project_db_find_feature_set(
      state->project_db, image_id, kind, version, fingerprint, feature_set);
  if (found == LARDON3D_PROJECT_DB_OK) {
    return reuse_existing_feature_set(state, image_id, producer_task_id, kind, version, fingerprint,
                                      feature_set);
  }
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND) {
    return found == LARDON3D_PROJECT_DB_BUSY ? LARDON3D_FEATURE_STORE_DB_BUSY
                                             : LARDON3D_FEATURE_STORE_DB_ERROR;
  }
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset image_asset;
  if (lardon3d_project_db_load_image(state->project_db, image_id, &image, &image_asset) !=
      LARDON3D_PROJECT_DB_OK) {
    return LARDON3D_FEATURE_STORE_NOT_FOUND;
  }
  char assets[PATH_MAX], features_dir[PATH_MAX];
  if (!join(assets, state->project_path, "assets") || !ensure_directory(assets) ||
      !join(features_dir, assets, "features") || !ensure_directory(features_dir)) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  char temporary[PATH_MAX];
  int n = snprintf(temporary, sizeof(temporary), "%s/.feature-XXXXXX", features_dir);
  if (n <= 0 || (size_t)n >= sizeof(temporary)) return LARDON3D_FEATURE_STORE_IO_ERROR;
  int fd = mkstemp(temporary);
  if (fd < 0) return LARDON3D_FEATURE_STORE_IO_ERROR;
  uint64_t file_size = 0;
  bool written = write_feature_file_v2(fd, &image_asset, kind, version, fingerprint,
                                       descriptor_type, descriptor_dimension, capabilities,
                                       features, &file_size) &&
                 fsync(fd) == 0;
  unsigned char file_hash[32];
  if (written) written = sha256_fd(fd, file_hash);
  if (close(fd) != 0) written = false;
  if (!written) {
    unlink(temporary);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  char hex[65];
  hex_sha(file_hash, hex);
  char prefix[PATH_MAX];
  n = snprintf(prefix, sizeof(prefix), "%s/%.2s", features_dir, hex);
  if (n <= 0 || (size_t)n >= sizeof(prefix) || !ensure_directory(prefix)) {
    unlink(temporary);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  char relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  n = snprintf(relative, sizeof(relative), "assets/features/%.2s/%s", hex, hex);
  char final[PATH_MAX];
  if (n <= 0 || (size_t)n >= sizeof(relative) || !join(final, state->project_path, relative)) {
    unlink(temporary);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  if (link(temporary, final) != 0) {
    if (errno != EEXIST) {
      unlink(temporary);
      return LARDON3D_FEATURE_STORE_IO_ERROR;
    }
    int existing = open(final, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    unsigned char existing_hash[32];
    bool same = existing >= 0 && fstat(existing, &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_size >= 0 && (uint64_t)info.st_size == file_size &&
                sha256_fd(existing, existing_hash) && memcmp(existing_hash, file_hash, 32) == 0;
    if (existing >= 0) (void)close(existing);
    if (!same) {
      unlink(temporary);
      return LARDON3D_FEATURE_STORE_CORRUPT;
    }
  }
  unlink(temporary);
  bool durable = sync_directory(prefix);
#ifdef LARDON3D_FEATURE_STORE_TESTING
  const char *fail_sync = getenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC");
  if (fail_sync && strcmp(fail_sync, "1") == 0) {
    durable = false;
  }
#endif
  return register_published_feature_set(
      state, image_id, producer_task_id, &image_asset, kind, version, fingerprint, descriptor_type,
      descriptor_dimension, features, file_hash, relative, file_size, durable, feature_set);
}

static Lardon3DFeatureStoreResult validate_header_v1(const unsigned char h[160], uint64_t actual,
                                                  Lardon3DFeatureFileMetadata *m, uint64_t *ko,
                                                  uint64_t *doff) {
  if (memcmp(h, feature_magic, 8) != 0) {
    return LARDON3D_FEATURE_STORE_INVALID;
  }
  uint32_t version = get_u32(h + 8);
  if (version != 1) {
    return version > 1 ? LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION
                       : LARDON3D_FEATURE_STORE_INVALID;
  }
  uint32_t count = get_u32(h + 16), dim = get_u32(h + 20), type = get_u32(h + 24),
           record = get_u32(h + 28), width = get_u32(h + 32), height = get_u32(h + 36);
  uint64_t key = get_u64(h + 40), desc = get_u64(h + 48), declared = get_u64(h + 56);
  uint64_t kb = (uint64_t)count * record, db = (uint64_t)count * dim;
  if (get_u32(h + 12) != 160 || count > LARDON3D_FEATURE_MAX_FEATURES || dim != 32 ||
      type != LARDON3D_FEATURE_DESCRIPTOR_U8 || record != 24 || width == 0 || height == 0 ||
      key != 160 || desc != key + kb || declared != desc + db || declared != actual ||
      actual > LARDON3D_FEATURE_FILE_V1_MAX_SIZE || memcmp(h + 128, "orb\0", 4) != 0 ||
      get_u32(h + 144) != 1) {
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  for (size_t i = 132; i < 144; ++i) {
    if (h[i] != 0) {
      return LARDON3D_FEATURE_STORE_CORRUPT;
    }
  }
  for (size_t i = 148; i < 160; ++i) {
    if (h[i] != 0) {
      return LARDON3D_FEATURE_STORE_CORRUPT;
    }
  }
  *m = (Lardon3DFeatureFileMetadata){.format_version = version,
                                     .extractor_version = get_u32(h + 144),
                                     .feature_count = count,
                                     .descriptor_dimension = dim,
                                     .descriptor_type = (Lardon3DFeatureDescriptorType)type,
                                     .descriptor_scalar_size_bytes = 1,
                                     .capabilities = LARDON3D_FEATURE_HAS_SCALE |
                                                     LARDON3D_FEATURE_HAS_ORIENTATION |
                                                     LARDON3D_FEATURE_HAS_RESPONSE |
                                                     LARDON3D_FEATURE_HAS_OCTAVE,
                                     .image_width = width,
                                     .image_height = height};
  memcpy(m->source_image_sha256, h + 64, 32);
  memcpy(m->parameter_fingerprint, h + 96, 32);
  memcpy(m->extractor_kind, h + 128, 4);
  *ko = key;
  *doff = desc;
  return LARDON3D_FEATURE_STORE_OK;
}

static Lardon3DFeatureStoreResult validate_header_v2(const unsigned char h[176], uint64_t actual,
                                                     Lardon3DFeatureFileMetadata *m, uint64_t *ko,
                                                     uint64_t *doff) {
  uint32_t count = get_u32(h + 16), dim = get_u32(h + 20), type = get_u32(h + 24);
  uint32_t scalar = get_u32(h + 28), record = get_u32(h + 32), width = get_u32(h + 36);
  uint32_t height = get_u32(h + 40), capabilities = get_u32(h + 44);
  uint64_t key = get_u64(h + 48), desc = get_u64(h + 56), declared = get_u64(h + 64);
  bool known_type = (type == LARDON3D_FEATURE_DESCRIPTOR_U8 && scalar == 1) ||
                    (type == LARDON3D_FEATURE_DESCRIPTOR_F32 && scalar == 4);
  uint64_t key_bytes = (uint64_t)count * record;
  uint64_t descriptor_bytes = (uint64_t)count * dim * scalar;
  uint32_t known_capabilities = LARDON3D_FEATURE_HAS_SCALE |
                                LARDON3D_FEATURE_HAS_ORIENTATION |
                                LARDON3D_FEATURE_HAS_RESPONSE | LARDON3D_FEATURE_HAS_OCTAVE;
  if (memcmp(h, feature_magic, 8) != 0 || get_u32(h + 8) != 2 || get_u32(h + 12) != 176 ||
      count > LARDON3D_FEATURE_MAX_FEATURES || !known_type || dim == 0 || dim > 4096 ||
      record != 24 || width == 0 || height == 0 || (capabilities & ~known_capabilities) != 0 ||
      key != 176 || desc != key + key_bytes || declared != desc + descriptor_bytes ||
      declared != actual || actual > LARDON3D_FEATURE_FILE_MAX_SIZE || h[136] == 0 ||
      get_u32(h + 152) == 0) {
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  for (size_t i = 156; i < 176; ++i) {
    if (h[i] != 0) return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  if (!memchr(h + 136, 0, 16)) return LARDON3D_FEATURE_STORE_CORRUPT;
  *m = (Lardon3DFeatureFileMetadata){.format_version = 2,
                                     .extractor_version = get_u32(h + 152),
                                     .feature_count = count,
                                     .descriptor_dimension = dim,
                                     .descriptor_type = (Lardon3DFeatureDescriptorType)type,
                                     .descriptor_scalar_size_bytes = scalar,
                                     .capabilities = capabilities,
                                     .image_width = width,
                                     .image_height = height};
  memcpy(m->source_image_sha256, h + 72, 32);
  memcpy(m->parameter_fingerprint, h + 104, 32);
  memcpy(m->extractor_kind, h + 136, 16);
  m->extractor_kind[15] = '\0';
  *ko = key;
  *doff = desc;
  return LARDON3D_FEATURE_STORE_OK;
}

static bool validate_f32_payload(int fd, uint64_t offset, uint32_t count, uint32_t dimension) {
  unsigned char encoded[LARDON3D_FEATURE_READER_RANGE_MAX * 128 * 4];
  if (dimension != 128) return false;
  for (uint32_t start = 0; start < count;) {
    uint32_t features = count - start;
    if (features > LARDON3D_FEATURE_READER_RANGE_MAX)
      features = LARDON3D_FEATURE_READER_RANGE_MAX;
    size_t values = (size_t)features * dimension;
    size_t bytes = values * 4;
    uint64_t position = offset + (uint64_t)start * dimension * 4;
    if (!read_exact(fd, encoded, bytes, (off_t)position)) return false;
    for (size_t i = 0; i < values; ++i)
      if (!isfinite(get_float(encoded + i * 4))) return false;
    start += features;
  }
  return true;
}

Lardon3DFeatureStoreResult lardon3d_feature_reader_open(const char *project_path,
                                                        const Lardon3DProjectDbFeatureSet *set,
                                                        Lardon3DFeatureReader **out,
                                                        Lardon3DFeatureFileMetadata *metadata) {
  if (out) {
    *out = NULL;
  }
  if (!project_path || !set || !out || !metadata ||
      !canonical_feature_path(set->asset.sha256, set->asset.path)) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  char path[PATH_MAX];
  if (!join(path, project_path, set->asset.path)) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return errno == ENOENT ? LARDON3D_FEATURE_STORE_NOT_FOUND : LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 160 ||
      (uint64_t)st.st_size > LARDON3D_FEATURE_FILE_MAX_SIZE ||
      (uint64_t)st.st_size != set->asset.size_bytes) {
    close(fd);
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  unsigned char header[176] = {0};
  if (!read_exact(fd, header, 160, 0)) {
    close(fd);
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  Lardon3DFeatureReader *reader = calloc(1, sizeof(*reader));
  if (!reader) {
    close(fd);
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  uint32_t version = get_u32(header + 8);
  if (version == 2 && !read_exact(fd, header + 160, 16, 160)) {
    close(fd);
    free(reader);
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  Lardon3DFeatureStoreResult result;
  if (version == 1) {
    result = validate_header_v1(header, (uint64_t)st.st_size, &reader->metadata,
                                &reader->keypoint_offset, &reader->descriptor_offset);
  } else if (version == 2) {
    result = validate_header_v2(header, (uint64_t)st.st_size, &reader->metadata,
                                &reader->keypoint_offset, &reader->descriptor_offset);
  } else {
    result = version > 2 ? LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION
                         : LARDON3D_FEATURE_STORE_INVALID;
  }
  if (result != LARDON3D_FEATURE_STORE_OK) {
    close(fd);
    free(reader);
    return result;
  }
  unsigned char hash[32];
  if (!sha256_fd(fd, hash) || memcmp(hash, set->asset.sha256, 32) != 0) {
    close(fd);
    free(reader);
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  if (reader->metadata.feature_count != set->feature_count ||
      strcmp(reader->metadata.extractor_kind, set->extractor_kind) != 0 ||
      reader->metadata.extractor_version != set->extractor_version ||
      reader->metadata.descriptor_dimension != set->descriptor_dimension ||
      reader->metadata.descriptor_type != (Lardon3DFeatureDescriptorType)set->descriptor_type ||
      memcmp(reader->metadata.source_image_sha256, set->source_image_sha256, 32) != 0 ||
      memcmp(reader->metadata.parameter_fingerprint, set->parameter_fingerprint, 32) != 0) {
    close(fd);
    free(reader);
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  if (reader->metadata.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32 &&
      !validate_f32_payload(fd, reader->descriptor_offset, reader->metadata.feature_count,
                            reader->metadata.descriptor_dimension)) {
    close(fd);
    free(reader);
    return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  reader->descriptor = fd;
  *metadata = reader->metadata;
  *out = reader;
  return LARDON3D_FEATURE_STORE_OK;
}

void lardon3d_feature_reader_close(Lardon3DFeatureReader *r) {
  if (!r) {
    return;
  }
  (void)close(r->descriptor);
  free(r);
}

Lardon3DFeatureStoreResult lardon3d_feature_reader_keypoints(Lardon3DFeatureReader *r,
                                                             uint32_t start,
                                                             Lardon3DFeatureKeypoint *out,
                                                             size_t capacity) {
  if (!r || (!out && capacity > 0) || capacity > LARDON3D_FEATURE_READER_RANGE_MAX ||
      start > r->metadata.feature_count || capacity > (size_t)(r->metadata.feature_count - start)) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  unsigned char record[24];
  for (size_t i = 0; i < capacity; ++i) {
    uint64_t index = (uint64_t)start + i;
    if (!read_exact(r->descriptor, record, 24, (off_t)(r->keypoint_offset + index * 24))) {
      return LARDON3D_FEATURE_STORE_IO_ERROR;
    }
    out[i] = (Lardon3DFeatureKeypoint){get_float(record),      get_float(record + 4),
                                       get_float(record + 8),  get_float(record + 12),
                                       get_float(record + 16), (int32_t)get_u32(record + 20)};
    if (!isfinite(out[i].x) || !isfinite(out[i].y) || !isfinite(out[i].size) ||
        !isfinite(out[i].angle_degrees) || !isfinite(out[i].response) || out[i].x < 0.0F ||
        out[i].y < 0.0F || out[i].x >= (float)r->metadata.image_width ||
        out[i].y >= (float)r->metadata.image_height || out[i].size <= 0.0F ||
        out[i].angle_degrees < 0.0F || out[i].angle_degrees >= 360.0F ||
        out[i].octave < -128 || out[i].octave > 127) {
      return LARDON3D_FEATURE_STORE_CORRUPT;
    }
  }
  return LARDON3D_FEATURE_STORE_OK;
}

Lardon3DFeatureStoreResult lardon3d_feature_reader_descriptors(Lardon3DFeatureReader *r,
                                                               uint32_t start, unsigned char *out,
                                                               size_t capacity, size_t bytes) {
  return lardon3d_feature_reader_descriptors_u8(r, start, out, capacity, bytes);
}

static bool descriptor_range_valid(const Lardon3DFeatureReader *r, uint32_t start,
                                   size_t capacity, size_t scalar, size_t bytes) {
  return r && capacity <= LARDON3D_FEATURE_READER_RANGE_MAX &&
         start <= r->metadata.feature_count &&
         capacity <= (size_t)(r->metadata.feature_count - start) &&
         r->metadata.descriptor_dimension <= SIZE_MAX / scalar &&
         capacity <= SIZE_MAX / (r->metadata.descriptor_dimension * scalar) &&
         bytes >= capacity * r->metadata.descriptor_dimension * scalar;
}

Lardon3DFeatureStoreResult lardon3d_feature_reader_descriptors_u8(
    Lardon3DFeatureReader *r, uint32_t start, unsigned char *out, size_t capacity, size_t bytes) {
  if (!r || r->metadata.descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_U8 ||
      (!out && capacity > 0) || !descriptor_range_valid(r, start, capacity, 1, bytes)) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  size_t amount = capacity * r->metadata.descriptor_dimension;
  uint64_t offset = r->descriptor_offset + (uint64_t)start * r->metadata.descriptor_dimension;
  return amount && !read_exact(r->descriptor, out, amount, (off_t)offset)
             ? LARDON3D_FEATURE_STORE_IO_ERROR
             : LARDON3D_FEATURE_STORE_OK;
}

Lardon3DFeatureStoreResult lardon3d_feature_reader_descriptors_f32(
    Lardon3DFeatureReader *r, uint32_t start, float *out, size_t capacity, size_t bytes) {
  if (!r || (!out && capacity > 0) || capacity > LARDON3D_FEATURE_READER_RANGE_MAX ||
      r->metadata.descriptor_type != LARDON3D_FEATURE_DESCRIPTOR_F32 ||
      !descriptor_range_valid(r, start, capacity, 4, bytes)) {
    return LARDON3D_FEATURE_STORE_INVALID_ARGUMENT;
  }
  unsigned char encoded[256 * 128 * 4];
  if (r->metadata.descriptor_dimension > 128) return LARDON3D_FEATURE_STORE_CORRUPT;
  size_t values = capacity * r->metadata.descriptor_dimension;
  size_t amount = values * 4;
  uint64_t offset = r->descriptor_offset + (uint64_t)start * r->metadata.descriptor_dimension * 4;
  if (amount && !read_exact(r->descriptor, encoded, amount, (off_t)offset)) {
    return LARDON3D_FEATURE_STORE_IO_ERROR;
  }
  for (size_t i = 0; i < values; ++i) {
    out[i] = get_float(encoded + i * 4);
    if (!isfinite(out[i])) return LARDON3D_FEATURE_STORE_CORRUPT;
  }
  return LARDON3D_FEATURE_STORE_OK;
}
