/**
 * Feature File v2 robustness tests.
 *
 * Tests deterministically generated binary fixtures for:
 * - header validation (magic, version, sizes, fields)
 * - payload truncation and corruption
 * - typed reader (U8 / F32)
 * - v1 / v2 compatibility
 * - size safety and overflow prevention
 *
 * No database, no OpenCV, no network, no allocation above a few KiB.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>

#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                         \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                   */
/* ------------------------------------------------------------------ */

static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}
static void put_u64(unsigned char *p, uint64_t v) {
  for (unsigned int i = 0; i < 8; ++i)
    p[i] = (unsigned char)(v >> (8U * i));
}
static void put_float_le(unsigned char *p, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  put_u32(p, bits);
}

/* ------------------------------------------------------------------ */
/* Feature File v1 layout constants                                    */
/* ------------------------------------------------------------------ */

enum {
  V1_HEADER_SIZE = 160,
  V1_KP_RECORD_SIZE = 24,
  V1_DESC_DIM = 32,
  V2_HEADER_SIZE = 176,
  KP_RECORD_SIZE = 24,
};

static const unsigned char feature_magic[8] = {'L', '3', 'D', 'F', 'E', 'A', 'T', 0};

/* ------------------------------------------------------------------ */
/* SHA-256 computation                                                  */
/* ------------------------------------------------------------------ */

static bool sha256_file(const char *path, unsigned char out[32]) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    close(fd);
    return false;
  }
  bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
  unsigned char buf[65536];
  ssize_t n;
  while (ok && (n = read(fd, buf, sizeof(buf))) > 0)
    ok = EVP_DigestUpdate(ctx, buf, (size_t)n) == 1;
  unsigned int len = 0;
  ok = ok && EVP_DigestFinal_ex(ctx, out, &len) == 1 && len == 32;
  EVP_MD_CTX_free(ctx);
  close(fd);
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

/* ------------------------------------------------------------------ */
/* Build canonical asset path from SHA-256                             */
/* ------------------------------------------------------------------ */

static void canonical_path_from_sha(const unsigned char sha[32], char path[128]) {
  char hex[65];
  hex_sha(sha, hex);
  snprintf(path, 128, "assets/features/%.2s/%s", hex, hex);
}

/* ------------------------------------------------------------------ */
/* Write buffer to a temp file                                         */
/* ------------------------------------------------------------------ */

static bool write_tmp(const char *path, const unsigned char *data, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return false;
  size_t written = 0;
  while (written < size) {
    ssize_t n = write(fd, data + written, size - written);
    if (n <= 0) {
      close(fd);
      return false;
    }
    written += (size_t)n;
  }
  return close(fd) == 0;
}

/* ------------------------------------------------------------------ */
/* Feature set builders                                                */
/* ------------------------------------------------------------------ */

static Lardon3DProjectDbFeatureSet make_set_v1(const unsigned char sha256[32],
                                               uint32_t count, uint64_t size) {
  Lardon3DProjectDbFeatureSet s;
  memset(&s, 0, sizeof(s));
  memcpy(s.asset.sha256, sha256, 32);
  canonical_path_from_sha(sha256, s.asset.path);
  s.asset.size_bytes = size;
  s.feature_count = count;
  s.descriptor_type = 1; /* U8 */
  s.descriptor_dimension = V1_DESC_DIM;
  snprintf(s.extractor_kind, sizeof(s.extractor_kind), "orb");
  s.extractor_version = 1;
  /* source_image_sha256 and parameter_fingerprint: zeros (reader checks
   * them against the header; the header also has zeros for these in our
   * fixture) */
  return s;
}

static Lardon3DProjectDbFeatureSet make_set_v2(const unsigned char sha256[32],
                                               uint32_t count, uint32_t dim,
                                               uint32_t type, const char *kind,
                                               uint64_t size) {
  Lardon3DProjectDbFeatureSet s;
  memset(&s, 0, sizeof(s));
  memcpy(s.asset.sha256, sha256, 32);
  canonical_path_from_sha(sha256, s.asset.path);
  s.asset.size_bytes = size;
  s.feature_count = count;
  s.descriptor_type = type;
  s.descriptor_dimension = dim;
  snprintf(s.extractor_kind, sizeof(s.extractor_kind), "%s", kind);
  s.extractor_version = 1;
  return s;
}

/* ------------------------------------------------------------------ */
/* Link a file into the root at its canonical path                     */
/* ------------------------------------------------------------------ */

static bool link_into_root(const char *root, const char *tmp_path,
                           const unsigned char sha[32]) {
  char rel[128];
  canonical_path_from_sha(sha, rel);
  /* Derive the two-char prefix from the hex SHA, not from rel. */
  char hex[65];
  hex_sha(sha, hex);
  /* Create parent directory: root/assets/features/<xx>/ */
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s/assets", root);
  mkdir(dir, 0755);
  snprintf(dir, sizeof(dir), "%s/assets/features", root);
  mkdir(dir, 0755);
  char prefix[PATH_MAX];
  snprintf(prefix, sizeof(prefix), "%s/assets/features/%.2s", root, hex);
  mkdir(prefix, 0755);
  /* Create full path */
  char full[PATH_MAX];
  snprintf(full, sizeof(full), "%s/%s", root, rel);
  unlink(full);
  /* Use hard link (not symlink) to match production publish behavior.
   * The reader opens with O_NOFOLLOW which rejects symlinks. */
  return link(tmp_path, full) == 0;
}

/* ------------------------------------------------------------------ */
/* Build a minimal valid v1 Feature File in buffer                     */
/* ------------------------------------------------------------------ */

static size_t build_v1(unsigned char *buf, size_t capacity, uint32_t count,
                       uint32_t width, uint32_t height) {
  uint64_t kp_bytes = (uint64_t)count * V1_KP_RECORD_SIZE;
  uint64_t desc_bytes = (uint64_t)count * V1_DESC_DIM;
  uint64_t total = V1_HEADER_SIZE + kp_bytes + desc_bytes;
  CHECK(total <= capacity);
  memset(buf, 0, (size_t)total);
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 1);                 /* version */
  put_u32(buf + 12, V1_HEADER_SIZE);   /* header size */
  put_u32(buf + 16, count);            /* feature count */
  put_u32(buf + 20, V1_DESC_DIM);      /* descriptor dimension */
  put_u32(buf + 24, 1);                /* descriptor type U8 */
  put_u32(buf + 28, V1_KP_RECORD_SIZE); /* keypoint record size */
  put_u32(buf + 32, width);            /* image width */
  put_u32(buf + 36, height);           /* image height */
  put_u64(buf + 40, V1_HEADER_SIZE);   /* keypoint offset */
  put_u64(buf + 48, V1_HEADER_SIZE + kp_bytes); /* descriptor offset */
  put_u64(buf + 56, total);            /* total size */
  memcpy(buf + 128, "orb\0", 4);       /* extractor kind */
  put_u32(buf + 144, 1);              /* extractor version */
  /* keypoints */
  for (uint32_t i = 0; i < count; ++i) {
    size_t off = V1_HEADER_SIZE + (size_t)i * V1_KP_RECORD_SIZE;
    put_float_le(buf + off + 0, (float)(i % width));
    put_float_le(buf + off + 4, (float)((i / width) % height));
    put_float_le(buf + off + 8, 5.0f);
    put_float_le(buf + off + 12, (float)(i % 360));
    put_float_le(buf + off + 16, 0.5f);
    put_u32(buf + off + 20, 0);
  }
  /* descriptors */
  size_t desc_off = V1_HEADER_SIZE + kp_bytes;
  for (uint32_t i = 0; i < count; ++i)
    memset(buf + desc_off + (size_t)i * V1_DESC_DIM, (int)(i & 0xFF), V1_DESC_DIM);
  return (size_t)total;
}

/* ------------------------------------------------------------------ */
/* Build a minimal valid v2 Feature File (U8) in buffer                */
/* ------------------------------------------------------------------ */

static size_t build_v2_u8(unsigned char *buf, size_t capacity, uint32_t count,
                          uint32_t dim, uint32_t width, uint32_t height) {
  uint32_t scalar = 1;
  uint64_t kp_bytes = (uint64_t)count * KP_RECORD_SIZE;
  uint64_t desc_bytes = (uint64_t)count * dim * scalar;
  uint64_t total = V2_HEADER_SIZE + kp_bytes + desc_bytes;
  CHECK(total <= capacity);
  memset(buf, 0, (size_t)total);
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 2);                 /* version */
  put_u32(buf + 12, V2_HEADER_SIZE);   /* header size */
  put_u32(buf + 16, count);            /* feature count */
  put_u32(buf + 20, dim);              /* descriptor dimension */
  put_u32(buf + 24, 1);                /* descriptor type U8 */
  put_u32(buf + 28, scalar);           /* scalar size */
  put_u32(buf + 32, KP_RECORD_SIZE);   /* keypoint record size */
  put_u32(buf + 36, width);
  put_u32(buf + 40, height);
  put_u32(buf + 44, 0xF);              /* capabilities: all bits */
  put_u64(buf + 48, V2_HEADER_SIZE);   /* keypoint offset */
  put_u64(buf + 56, V2_HEADER_SIZE + kp_bytes); /* descriptor offset */
  put_u64(buf + 64, total);            /* total size */
  memcpy(buf + 136, "orb\0", 4);       /* extractor kind */
  put_u32(buf + 152, 1);              /* extractor version */
  /* keypoints */
  for (uint32_t i = 0; i < count; ++i) {
    size_t off = V2_HEADER_SIZE + (size_t)i * KP_RECORD_SIZE;
    put_float_le(buf + off + 0, (float)(i % width));
    put_float_le(buf + off + 4, (float)((i / width) % height));
    put_float_le(buf + off + 8, 5.0f);
    put_float_le(buf + off + 12, (float)(i % 360));
    put_float_le(buf + off + 16, 0.5f);
    put_u32(buf + off + 20, 0);
  }
  /* descriptors */
  size_t desc_off = V2_HEADER_SIZE + kp_bytes;
  for (uint32_t i = 0; i < count; ++i)
    memset(buf + desc_off + (size_t)i * dim, (int)(i & 0xFF), dim);
  return (size_t)total;
}

/* ------------------------------------------------------------------ */
/* Build a minimal valid v2 Feature File (F32) in buffer               */
/* ------------------------------------------------------------------ */

static size_t build_v2_f32(unsigned char *buf, size_t capacity, uint32_t count,
                           uint32_t dim, uint32_t width, uint32_t height) {
  uint32_t scalar = 4;
  uint64_t kp_bytes = (uint64_t)count * KP_RECORD_SIZE;
  uint64_t desc_bytes = (uint64_t)count * dim * scalar;
  uint64_t total = V2_HEADER_SIZE + kp_bytes + desc_bytes;
  CHECK(total <= capacity);
  memset(buf, 0, (size_t)total);
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 2);
  put_u32(buf + 12, V2_HEADER_SIZE);
  put_u32(buf + 16, count);
  put_u32(buf + 20, dim);
  put_u32(buf + 24, 2);                /* descriptor type F32 */
  put_u32(buf + 28, scalar);
  put_u32(buf + 32, KP_RECORD_SIZE);
  put_u32(buf + 36, width);
  put_u32(buf + 40, height);
  put_u32(buf + 44, 0xF);
  put_u64(buf + 48, V2_HEADER_SIZE);
  put_u64(buf + 56, V2_HEADER_SIZE + kp_bytes);
  put_u64(buf + 64, total);
  memcpy(buf + 136, "sift\0", 5);
  put_u32(buf + 152, 1);
  /* keypoints */
  for (uint32_t i = 0; i < count; ++i) {
    size_t off = V2_HEADER_SIZE + (size_t)i * KP_RECORD_SIZE;
    put_float_le(buf + off + 0, (float)(i % width));
    put_float_le(buf + off + 4, (float)((i / width) % height));
    put_float_le(buf + off + 8, 5.0f);
    put_float_le(buf + off + 12, (float)(i % 360));
    put_float_le(buf + off + 16, 0.5f);
    put_u32(buf + off + 20, 0);
  }
  /* F32 descriptors */
  size_t desc_off = V2_HEADER_SIZE + kp_bytes;
  for (uint32_t i = 0; i < count * dim; ++i) {
    float v = (float)(i % 256) / 256.0f;
    put_float_le(buf + desc_off + (size_t)i * 4, v);
  }
  return (size_t)total;
}

/* ------------------------------------------------------------------ */
/* Helper: write fixture to temp file, link into root, return set      */
/* ------------------------------------------------------------------ */

static Lardon3DProjectDbFeatureSet fixture_v1(const char *root, const char *tmp_path,
                                              unsigned char *buf, size_t capacity,
                                              uint32_t count, uint32_t w, uint32_t h) {
  size_t total = build_v1(buf, capacity, count, w, h);
  Lardon3DProjectDbFeatureSet empty;
  memset(&empty, 0, sizeof(empty));
  if (!write_tmp(tmp_path, buf, total))
    return empty;
  unsigned char sha[32];
  if (!sha256_file(tmp_path, sha))
    return empty;
  if (!link_into_root(root, tmp_path, sha))
    return empty;
  return make_set_v1(sha, count, total);
}

static Lardon3DProjectDbFeatureSet fixture_v2_u8(const char *root, const char *tmp_path,
                                                  unsigned char *buf, size_t capacity,
                                                  uint32_t count, uint32_t dim,
                                                  uint32_t w, uint32_t h) {
  size_t total = build_v2_u8(buf, capacity, count, dim, w, h);
  Lardon3DProjectDbFeatureSet empty;
  memset(&empty, 0, sizeof(empty));
  if (!write_tmp(tmp_path, buf, total))
    return empty;
  unsigned char sha[32];
  if (!sha256_file(tmp_path, sha))
    return empty;
  if (!link_into_root(root, tmp_path, sha))
    return empty;
  return make_set_v2(sha, count, dim, 1, "orb", total);
}

static Lardon3DProjectDbFeatureSet fixture_v2_f32(const char *root, const char *tmp_path,
                                                   unsigned char *buf, size_t capacity,
                                                   uint32_t count, uint32_t dim,
                                                   uint32_t w, uint32_t h) {
  size_t total = build_v2_f32(buf, capacity, count, dim, w, h);
  Lardon3DProjectDbFeatureSet empty;
  memset(&empty, 0, sizeof(empty));
  if (!write_tmp(tmp_path, buf, total))
    return empty;
  unsigned char sha[32];
  if (!sha256_file(tmp_path, sha))
    return empty;
  if (!link_into_root(root, tmp_path, sha))
    return empty;
  return make_set_v2(sha, count, dim, 2, "sift", total);
}

/* ------------------------------------------------------------------ */
/* Tests: HEADER                                                       */
/* ------------------------------------------------------------------ */

static bool test_empty_file(const char *root) {
  const char *tmp = "/tmp/ff-robust-empty.bin";
  CHECK(write_tmp(tmp, (const unsigned char *)"", 0));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 0, 0);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  return true;
}

static bool test_too_small(const char *root) {
  unsigned char buf[100] = {0};
  memcpy(buf, feature_magic, 8);
  const char *tmp = "/tmp/ff-robust-small.bin";
  CHECK(write_tmp(tmp, buf, sizeof(buf)));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 0, sizeof(buf));
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  return true;
}

static bool test_bad_magic(const char *root) {
  unsigned char buf[V1_HEADER_SIZE] = {0};
  memcpy(buf, "BADMAGC", 8);
  put_u32(buf + 8, 1);
  put_u32(buf + 12, V1_HEADER_SIZE);
  const char *tmp = "/tmp/ff-robust-magic.bin";
  CHECK(write_tmp(tmp, buf, sizeof(buf)));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 0, sizeof(buf));
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_INVALID);
  return true;
}

static bool test_version_zero(const char *root) {
  unsigned char buf[V1_HEADER_SIZE] = {0};
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 0); /* version 0 */
  put_u32(buf + 12, V1_HEADER_SIZE);
  const char *tmp = "/tmp/ff-robust-ver0.bin";
  CHECK(write_tmp(tmp, buf, sizeof(buf)));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 0, sizeof(buf));
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_INVALID);
  return true;
}

static bool test_future_version(const char *root) {
  unsigned char buf[V1_HEADER_SIZE] = {0};
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 3); /* future version */
  put_u32(buf + 12, V1_HEADER_SIZE);
  const char *tmp = "/tmp/ff-robust-future.bin";
  CHECK(write_tmp(tmp, buf, sizeof(buf)));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 0, sizeof(buf));
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION);
  return true;
}

static bool test_header_truncated(const char *root) {
  /* build_v1(5,64,64) needs 160+5*24+5*32 = 440 bytes */
  unsigned char full[V1_HEADER_SIZE + 400];
  (void)build_v1(full, sizeof(full), 5, 64, 64);
  const char *tmp = "/tmp/ff-robust-trunc.bin";
  const off_t truncations[] = {8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 144, 148, 160};
  for (size_t i = 0; i < sizeof(truncations) / sizeof(truncations[0]); ++i) {
    CHECK(write_tmp(tmp, full, (size_t)truncations[i]));
    unsigned char sha[32];
    CHECK(sha256_file(tmp, sha));
    CHECK(link_into_root(root, tmp, sha));
    Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 5, (uint64_t)truncations[i]);
    Lardon3DFeatureReader *reader = NULL;
    Lardon3DFeatureFileMetadata meta;
    Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
    lardon3d_feature_reader_close(reader);
    CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  }
  return true;
}

static bool test_wrong_header_size(const char *root) {
  /* build_v1(3,32,32) needs 160+3*24+3*32 = 328 bytes */
  unsigned char buf[V1_HEADER_SIZE + 200];
  (void)build_v1(buf, sizeof(buf), 3, 32, 32);
  put_u32(buf + 12, 999); /* wrong header size */
  const char *tmp = "/tmp/ff-robust-hdrsz.bin";
  CHECK(write_tmp(tmp, buf, sizeof(buf)));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 3, sizeof(buf));
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  return true;
}

/* ------------------------------------------------------------------ */
/* Tests: PAYLOAD                                                      */
/* ------------------------------------------------------------------ */

static bool test_count_zero(const char *root) {
  unsigned char buf[V1_HEADER_SIZE + 48];
  const char *tmp = "/tmp/ff-robust-zero.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v1(root, tmp, buf, sizeof(buf), 0, 32, 32);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  CHECK(meta.feature_count == 0);
  Lardon3DFeatureKeypoint kp;
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, &kp, 1) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_count_mismatch(const char *root) {
  unsigned char buf[512];
  const char *tmp = "/tmp/ff-robust-mismatch.bin";
  size_t total = build_v1(buf, sizeof(buf), 5, 32, 32);
  CHECK(write_tmp(tmp, buf, total));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  /* Tell the set it has 10 features but file has 5 */
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 10, total);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_payload_truncated(const char *root) {
  unsigned char buf[512];
  const char *tmp = "/tmp/ff-robust-ptrunc.bin";
  (void)build_v1(buf, sizeof(buf), 5, 32, 32);
  /* Truncate: keep header but cut last keypoint */
  size_t cut = V1_HEADER_SIZE + 5 * V1_KP_RECORD_SIZE - 1;
  CHECK(write_tmp(tmp, buf, cut));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 5, cut);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_descriptor_truncated(const char *root) {
  unsigned char buf[512];
  const char *tmp = "/tmp/ff-robust-dtrunc.bin";
  (void)build_v1(buf, sizeof(buf), 5, 32, 32);
  size_t cut = V1_HEADER_SIZE + 5 * V1_KP_RECORD_SIZE + 32 * 4;
  CHECK(write_tmp(tmp, buf, cut));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 5, cut);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_range_read(const char *root) {
  unsigned char buf[1024];
  const char *tmp = "/tmp/ff-robust-range.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v1(root, tmp, buf, sizeof(buf), 10, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  /* Read exactly at last feature */
  Lardon3DFeatureKeypoint kp;
  unsigned char desc[32];
  CHECK(lardon3d_feature_reader_keypoints(reader, 9, &kp, 1) == LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_descriptors(reader, 9, desc, 1, sizeof(desc)) ==
        LARDON3D_FEATURE_STORE_OK);
  /* Start after last feature */
  CHECK(lardon3d_feature_reader_keypoints(reader, 10, &kp, 1) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  CHECK(lardon3d_feature_reader_descriptors(reader, 10, desc, 1, sizeof(desc)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  /* Range exceeding count */
  CHECK(lardon3d_feature_reader_keypoints(reader, 8, &kp, 5) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  /* Capacity 0 with null pointer */
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, NULL, 0) == LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_descriptors(reader, 0, NULL, 0, 0) == LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_range_max(const char *root) {
  /* build_v1(260,64,64) needs 160+260*24+260*32 = 14720 bytes */
  unsigned char buf[16384];
  const char *tmp = "/tmp/ff-robust-maxrange.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v1(root, tmp, buf, sizeof(buf), 260, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  /* Read 256 (max) */
  Lardon3DFeatureKeypoint kp[256];
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, kp, 256) == LARDON3D_FEATURE_STORE_OK);
  /* Try 257 (exceeds max) */
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, kp, 257) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  lardon3d_feature_reader_close(reader);
  return true;
}

/* ------------------------------------------------------------------ */
/* Tests: TYPES (U8 / F32)                                             */
/* ------------------------------------------------------------------ */

static bool test_u8_read(const char *root) {
  unsigned char buf[1024];
  const char *tmp = "/tmp/ff-robust-u8.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v2_u8(root, tmp, buf, sizeof(buf), 8, 32, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  CHECK(meta.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8);
  unsigned char desc[8 * 32];
  CHECK(lardon3d_feature_reader_descriptors_u8(reader, 0, desc, 8, sizeof(desc)) ==
        LARDON3D_FEATURE_STORE_OK);
  for (uint32_t i = 0; i < 8; ++i)
    CHECK(desc[i * 32] == (unsigned char)(i & 0xFF));
  /* Wrong typed read */
  float fdesc[32];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, fdesc, 1, sizeof(fdesc)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_f32_read(const char *root) {
  unsigned char buf[8192];
  const char *tmp = "/tmp/ff-robust-f32.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v2_f32(root, tmp, buf, sizeof(buf), 8, 128, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  CHECK(meta.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32);
  CHECK(meta.descriptor_dimension == 128);
  CHECK(meta.descriptor_scalar_size_bytes == 4);
  float fdesc[128];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, fdesc, 1, sizeof(fdesc)) ==
        LARDON3D_FEATURE_STORE_OK);
  CHECK(isfinite(fdesc[0]));
  CHECK(fdesc[0] == (float)(0 % 256) / 256.0f);
  /* Wrong typed read */
  unsigned char udesc[128];
  CHECK(lardon3d_feature_reader_descriptors_u8(reader, 0, udesc, 1, sizeof(udesc)) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  /* Read chunk at boundary */
  float chunk[256 * 128];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, chunk, 8, sizeof(float) * 8 * 128) ==
        LARDON3D_FEATURE_STORE_OK);
  /* Try reading more than max range */
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, chunk, 257,
                                                sizeof(float) * 257 * 128) ==
        LARDON3D_FEATURE_STORE_INVALID_ARGUMENT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_f32_nan_rejected(const char *root) {
  unsigned char buf[8192];
  const char *tmp = "/tmp/ff-robust-f32nan.bin";
  size_t total = build_v2_f32(buf, sizeof(buf), 4, 128, 32, 32);
  /* Inject NaN in the first descriptor value */
  size_t desc_off = V2_HEADER_SIZE + 4 * KP_RECORD_SIZE;
  unsigned char nan_bytes[4] = {0, 0, 0xC0, 0x7F}; /* quiet NaN in LE */
  memcpy(buf + desc_off, nan_bytes, 4);
  CHECK(write_tmp(tmp, buf, total));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v2(sha, 4, 128, 2, "sift", total);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_f32_inf_rejected(const char *root) {
  unsigned char buf[8192];
  const char *tmp = "/tmp/ff-robust-f32inf.bin";
  size_t total = build_v2_f32(buf, sizeof(buf), 4, 128, 32, 32);
  size_t desc_off = V2_HEADER_SIZE + 4 * KP_RECORD_SIZE;
  unsigned char inf_bytes[4] = {0, 0, 0x80, 0x7F}; /* +Inf in LE */
  memcpy(buf + desc_off, inf_bytes, 4);
  CHECK(write_tmp(tmp, buf, total));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v2(sha, 4, 128, 2, "sift", total);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_f32_wrong_dimension(const char *root) {
  unsigned char buf[4096];
  const char *tmp = "/tmp/ff-robust-f32dim.bin";
  uint32_t dim = 64;
  uint32_t count = 4;
  uint32_t scalar = 4;
  uint64_t kp_bytes = (uint64_t)count * KP_RECORD_SIZE;
  uint64_t desc_bytes = (uint64_t)count * dim * scalar;
  uint64_t total = V2_HEADER_SIZE + kp_bytes + desc_bytes;
  memset(buf, 0, (size_t)total);
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 2);
  put_u32(buf + 12, V2_HEADER_SIZE);
  put_u32(buf + 16, count);
  put_u32(buf + 20, dim);
  put_u32(buf + 24, 2); /* F32 */
  put_u32(buf + 28, scalar);
  put_u32(buf + 32, KP_RECORD_SIZE);
  put_u32(buf + 36, 32);
  put_u32(buf + 40, 32);
  put_u32(buf + 44, 0xF);
  put_u64(buf + 48, V2_HEADER_SIZE);
  put_u64(buf + 56, V2_HEADER_SIZE + kp_bytes);
  put_u64(buf + 64, total);
  memcpy(buf + 136, "sift\0", 5);
  put_u32(buf + 152, 1);
  for (uint32_t i = 0; i < count; ++i) {
    size_t off = V2_HEADER_SIZE + (size_t)i * KP_RECORD_SIZE;
    put_float_le(buf + off + 0, (float)(i % 32));
    put_float_le(buf + off + 4, 1.0f);
    put_float_le(buf + off + 8, 5.0f);
    put_float_le(buf + off + 12, 0.0f);
    put_float_le(buf + off + 16, 0.5f);
    put_u32(buf + off + 20, 0);
  }
  for (uint32_t i = 0; i < count * dim; ++i)
    put_float_le(buf + V2_HEADER_SIZE + kp_bytes + (size_t)i * 4, 0.1f);
  CHECK(write_tmp(tmp, buf, (size_t)total));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v2(sha, count, dim, 2, "sift", total);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

/* ------------------------------------------------------------------ */
/* Tests: V1/V2 COMPATIBILITY                                          */
/* ------------------------------------------------------------------ */

static bool test_v1_still_readable(const char *root) {
  unsigned char buf[1024];
  const char *tmp = "/tmp/ff-robust-v1compat.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v1(root, tmp, buf, sizeof(buf), 5, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  CHECK(meta.format_version == 1);
  CHECK(meta.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8);
  CHECK(meta.descriptor_dimension == 32);
  CHECK(meta.descriptor_scalar_size_bytes == 1);
  CHECK(meta.feature_count == 5);
  Lardon3DFeatureKeypoint kp;
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, &kp, 1) == LARDON3D_FEATURE_STORE_OK);
  CHECK(isfinite(kp.x) && isfinite(kp.y));
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_v2_u8_still_readable(const char *root) {
  unsigned char buf[1024];
  const char *tmp = "/tmp/ff-robust-v2u8.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v2_u8(root, tmp, buf, sizeof(buf), 5, 32, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  CHECK(meta.format_version == 2);
  CHECK(meta.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_U8);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_v2_f32_still_readable(const char *root) {
  unsigned char buf[8192];
  const char *tmp = "/tmp/ff-robust-v2f32.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v2_f32(root, tmp, buf, sizeof(buf), 5, 128, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  CHECK(meta.format_version == 2);
  CHECK(meta.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32);
  CHECK(meta.descriptor_dimension == 128);
  float fdesc[128];
  CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, fdesc, 1, sizeof(fdesc)) ==
        LARDON3D_FEATURE_STORE_OK);
  CHECK(isfinite(fdesc[0]));
  lardon3d_feature_reader_close(reader);
  return true;
}

/* ------------------------------------------------------------------ */
/* Tests: CORRUPTION TARGETED                                           */
/* ------------------------------------------------------------------ */

static bool test_corrupt_single_field_v1(const char *root) {
  unsigned char orig[1024];
  size_t total = build_v1(orig, sizeof(orig), 5, 64, 64);
  const char *tmp = "/tmp/ff-robust-corrupt1.bin";
  /* Offsets that trigger CORRUPT when mutated:
   * 12=version, 16=header_size, 20=count, 24=dim, 25=type,
   * 28=kp_record_size, 40=kp_offset_lo, 48=desc_offset_lo,
   * 56=total_size, 128=extractor_kind, 144=extractor_version.
   * Width(32)/height(36) are NOT validated for range by the reader. */
  const size_t offsets[] = {
      12, 16, 20, 24, 28, 40, 48, 56, 128, 144,
  };
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    unsigned char mutated[1024];
    memcpy(mutated, orig, total);
    mutated[offsets[i]] ^= 0xFF;
    CHECK(write_tmp(tmp, mutated, total));
    unsigned char sha[32];
    CHECK(sha256_file(tmp, sha));
    CHECK(link_into_root(root, tmp, sha));
    Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 5, total);
    Lardon3DFeatureReader *reader = NULL;
    Lardon3DFeatureFileMetadata meta;
    Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
    lardon3d_feature_reader_close(reader);
    CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  }
  return true;
}

static bool test_corrupt_single_field_v2(const char *root) {
  unsigned char orig[2048];
  size_t total = build_v2_u8(orig, sizeof(orig), 5, 32, 64, 64);
  const char *tmp = "/tmp/ff-robust-corrupt2.bin";
  /* Offsets that trigger CORRUPT when mutated:
   * 12=version, 16=header_size, 20=count, 24=dim, 25=type,
   * 28=scalar, 32=kp_record_size, 44=capabilities, 48=kp_offset_lo,
   * 56=desc_offset_lo, 64=total_size_lo, 136=extractor_kind,
   * 152=extractor_version.
   * Width(36)/height(40) are NOT validated for range by the reader. */
  const size_t offsets[] = {
      12, 16, 20, 24, 28, 32, 44, 48, 56, 64, 136, 152,
  };
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    unsigned char mutated[2048];
    memcpy(mutated, orig, total);
    mutated[offsets[i]] ^= 0xFF;
    CHECK(write_tmp(tmp, mutated, total));
    unsigned char sha[32];
    CHECK(sha256_file(tmp, sha));
    CHECK(link_into_root(root, tmp, sha));
    Lardon3DProjectDbFeatureSet set = make_set_v2(sha, 5, 32, 1, "orb", total);
    Lardon3DFeatureReader *reader = NULL;
    Lardon3DFeatureFileMetadata meta;
    Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
    lardon3d_feature_reader_close(reader);
    CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  }
  return true;
}

static bool test_corrupt_keypoint(const char *root) {
  unsigned char orig[1024];
  const char *tmp = "/tmp/ff-robust-cpkp.bin";
  size_t total = build_v1(orig, sizeof(orig), 5, 64, 64);
  /* Corrupt the x of keypoint #2 with NaN */
  size_t kp_offset = V1_HEADER_SIZE + 2 * V1_KP_RECORD_SIZE;
  unsigned char mutated[1024];
  memcpy(mutated, orig, total);
  unsigned char nan[4] = {0, 0, 0xC0, 0x7F};
  memcpy(mutated + kp_offset, nan, 4);
  CHECK(write_tmp(tmp, mutated, total));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 5, total);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  Lardon3DFeatureKeypoint kp[3];
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, kp, 2) == LARDON3D_FEATURE_STORE_OK);
  CHECK(lardon3d_feature_reader_keypoints(reader, 0, kp, 3) == LARDON3D_FEATURE_STORE_CORRUPT);
  lardon3d_feature_reader_close(reader);
  return true;
}

/* ------------------------------------------------------------------ */
/* Tests: SIZE SAFETY                                                  */
/* ------------------------------------------------------------------ */

static bool test_offset_overflow_v2(const char *root) {
  unsigned char buf[176 + 32];
  memset(buf, 0, sizeof(buf));
  memcpy(buf, feature_magic, 8);
  put_u32(buf + 8, 2);
  put_u32(buf + 12, V2_HEADER_SIZE);
  put_u32(buf + 16, 0xFFFFFFFF);
  put_u32(buf + 20, 0xFFFFFFFF);
  put_u32(buf + 24, 1);
  put_u32(buf + 28, 1);
  put_u32(buf + 32, KP_RECORD_SIZE);
  put_u32(buf + 36, 1);
  put_u32(buf + 40, 1);
  put_u32(buf + 44, 0);
  put_u64(buf + 48, V2_HEADER_SIZE);
  put_u64(buf + 56, V2_HEADER_SIZE + (uint64_t)0xFFFFFFFF * KP_RECORD_SIZE);
  put_u64(buf + 64, V2_HEADER_SIZE + (uint64_t)0xFFFFFFFF * KP_RECORD_SIZE +
                        (uint64_t)0xFFFFFFFF * 0xFFFFFFFF);
  memcpy(buf + 136, "orb\0", 4);
  put_u32(buf + 152, 1);
  const char *tmp = "/tmp/ff-robust-overflow.bin";
  CHECK(write_tmp(tmp, buf, sizeof(buf)));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  Lardon3DProjectDbFeatureSet set = make_set_v2(sha, 0xFFFFFFFF, 0xFFFFFFFF, 1, "orb", sizeof(buf));
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  Lardon3DFeatureStoreResult r = lardon3d_feature_reader_open(root, &set, &reader, &meta);
  lardon3d_feature_reader_close(reader);
  CHECK(r == LARDON3D_FEATURE_STORE_CORRUPT);
  return true;
}

static bool test_wrong_sha256(const char *root) {
  unsigned char buf[512];
  const char *tmp = "/tmp/ff-robust-sha.bin";
  size_t total = build_v1(buf, sizeof(buf), 3, 32, 32);
  CHECK(write_tmp(tmp, buf, total));
  unsigned char sha[32];
  CHECK(sha256_file(tmp, sha));
  CHECK(link_into_root(root, tmp, sha));
  /* Tamper the SHA in the set: the reader derives the file path from the
   * SHA, so a wrong SHA means the file is not found at the derived path. */
  sha[0] ^= 0xFF;
  Lardon3DProjectDbFeatureSet set = make_set_v1(sha, 3, total);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) ==
        LARDON3D_FEATURE_STORE_NOT_FOUND);
  lardon3d_feature_reader_close(reader);
  return true;
}

static bool test_repeated_reads(const char *root) {
  unsigned char buf[1024];
  const char *tmp = "/tmp/ff-robust-repeat.bin";
  Lardon3DProjectDbFeatureSet set = fixture_v2_u8(root, tmp, buf, sizeof(buf), 10, 32, 64, 64);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata meta;
  CHECK(lardon3d_feature_reader_open(root, &set, &reader, &meta) == LARDON3D_FEATURE_STORE_OK);
  unsigned char desc[32];
  for (int i = 0; i < 10; ++i) {
    CHECK(lardon3d_feature_reader_descriptors_u8(reader, 5, desc, 1, sizeof(desc)) ==
          LARDON3D_FEATURE_STORE_OK);
    CHECK(desc[0] == 5);
  }
  lardon3d_feature_reader_close(reader);
  return true;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

#define RUN(fn)                                                                                    \
  do {                                                                                             \
    fprintf(stderr, "  %-50s ", #fn);                                                              \
    if (fn(root)) {                                                                                \
      passed++;                                                                                    \
      fprintf(stderr, "PASS\n");                                                                   \
    } else {                                                                                       \
      failed++;                                                                                    \
    }                                                                                              \
  } while (0)

int main(void) {
  char root[] = "/tmp/lardon3d-ff-robust-XXXXXX";
  if (!mkdtemp(root)) {
    perror("mkdtemp");
    return 1;
  }
  int passed = 0, failed = 0;
  fprintf(stderr, "=== Feature File v2 Robustness Tests ===\n");

  fprintf(stderr, "\n--- HEADER ---\n");
  RUN(test_empty_file);
  RUN(test_too_small);
  RUN(test_bad_magic);
  RUN(test_version_zero);
  RUN(test_future_version);
  RUN(test_header_truncated);
  RUN(test_wrong_header_size);

  fprintf(stderr, "\n--- PAYLOAD ---\n");
  RUN(test_count_zero);
  RUN(test_count_mismatch);
  RUN(test_payload_truncated);
  RUN(test_descriptor_truncated);
  RUN(test_range_read);
  RUN(test_range_max);

  fprintf(stderr, "\n--- TYPES ---\n");
  RUN(test_u8_read);
  RUN(test_f32_read);
  RUN(test_f32_nan_rejected);
  RUN(test_f32_inf_rejected);
  RUN(test_f32_wrong_dimension);

  fprintf(stderr, "\n--- COMPATIBILITY ---\n");
  RUN(test_v1_still_readable);
  RUN(test_v2_u8_still_readable);
  RUN(test_v2_f32_still_readable);

  fprintf(stderr, "\n--- CORRUPTION ---\n");
  RUN(test_corrupt_single_field_v1);
  RUN(test_corrupt_single_field_v2);
  RUN(test_corrupt_keypoint);

  fprintf(stderr, "\n--- SIZE SAFETY ---\n");
  RUN(test_offset_overflow_v2);
  RUN(test_wrong_sha256);
  RUN(test_repeated_reads);

  fprintf(stderr, "\n=== RESULTS: %d passed, %d failed ===\n", passed, failed);

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
  system(cmd);

  return failed > 0 ? 1 : 0;
}