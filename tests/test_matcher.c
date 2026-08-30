#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lardon3d/match_file.h>
#include <lardon3d/matcher.h>
#include <lardon3d/orb_vulkan_backend.h>

#include "../src/matcher_internal.h"

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "Échec ligne %d : %s\n", __LINE__, #condition);                             \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

/* --- Match File tests --- */

static bool test_match_file_write_read(void) {
  char dir[] = "/tmp/lardon3d-mf-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/test.mf", dir) > 0);

  Lardon3DMatchFileEntry entries[3];
  entries[0].feature_index_a = 0;
  entries[0].feature_index_b = 0;
  entries[0].distance = 0.0F;
  entries[1].feature_index_a = 5;
  entries[1].feature_index_b = 3;
  entries[1].distance = 3.0F;
  entries[2].feature_index_a = 10;
  entries[2].feature_index_b = 20;
  entries[2].distance = 12.5F;

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(lardon3d_match_file_write(fd, 1, 32, 1, 2, entries, 3) ==
        LARDON3D_MATCH_FILE_OK);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_OK);
  CHECK(memcmp(header.magic, LARDON3D_MATCH_FILE_MAGIC, 4) == 0);
  CHECK(header.format_version == 1);
  CHECK(header.descriptor_type == 1);
  CHECK(header.descriptor_dimension == 32);
  CHECK(header.match_count == 3);
  CHECK(header.feature_set_id_a == 1 && header.feature_set_id_b == 2);

  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0);
  unsigned char physical_magic[4];
  CHECK(read(fd, physical_magic, sizeof(physical_magic)) == (ssize_t)sizeof(physical_magic));
  CHECK(memcmp(physical_magic, "L3DM", 4) == 0);
  CHECK(lseek(fd, 0, SEEK_SET) == 0);
  Lardon3DMatchFileEntry read_entries[10];
  uint32_t count = 0;
  CHECK(lardon3d_match_file_read(fd, &header, read_entries, 10, &count, 1, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_OK);
  (void)close(fd);
  CHECK(count == 3);
  CHECK(read_entries[0].feature_index_a == 0);
  CHECK(read_entries[2].feature_index_a == 10);
  CHECK(read_entries[2].feature_index_b == 20);
  CHECK(fabsf(read_entries[2].distance - 12.5F) < 0.001F);
  CHECK(lardon3d_match_file_validate(path, &header, 9, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_CORRUPT);
  CHECK(lardon3d_match_file_validate(path, &header, 1, 9, 8192, 8192) ==
        LARDON3D_MATCH_FILE_CORRUPT);
  CHECK(lardon3d_match_file_validate(path, &header, 2, 1, 8192, 8192) ==
        LARDON3D_MATCH_FILE_CORRUPT);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_empty(void) {
  char dir[] = "/tmp/lardon3d-me-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/empty.mf", dir) > 0);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(lardon3d_match_file_write(fd, 1, 32, 1, 2, NULL, 0) ==
        LARDON3D_MATCH_FILE_OK);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 0, 0) ==
        LARDON3D_MATCH_FILE_OK);
  CHECK(header.match_count == 0);

  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0);
  Lardon3DMatchFileEntry entries[1];
  uint32_t count = 0;
  CHECK(lardon3d_match_file_read(fd, &header, entries, 1, &count, 1, 2, 0, 0) ==
        LARDON3D_MATCH_FILE_OK);
  (void)close(fd);
  CHECK(count == 0);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_bad_magic(void) {
  char dir[] = "/tmp/lardon3d-mbm-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/bad.mf", dir) > 0);

  unsigned char buf[32] = {0};
  memcpy(buf, "MD3L", 4);
  buf[4] = 1;
  buf[5] = 1;
  buf[12] = 32;
  buf[16] = 1;
  buf[24] = 2;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(write(fd, buf, 32) == 32);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 0, 0, 0, 0) ==
        LARDON3D_MATCH_FILE_BAD_MAGIC);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_bad_version(void) {
  unsigned char buf[32] = {0};
  memcpy(buf, "L3DM", 4);
  buf[4] = 42;

  char dir[] = "/tmp/lardon3d-mbv-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/badv.mf", dir) > 0);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(write(fd, buf, 32) == 32);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 0, 0, 0, 0) ==
        LARDON3D_MATCH_FILE_BAD_VERSION);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_truncated(void) {
  unsigned char buf[32] = {0};
  memcpy(buf, "L3DM", 4);
  buf[4] = 1;
  buf[5] = 1;
  buf[8] = 1;
  buf[12] = 32;
  buf[16] = 1;
  buf[24] = 2;

  char dir[] = "/tmp/lardon3d-mt-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/trunc.mf", dir) > 0);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(write(fd, buf, 32) == 32);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 0, 0, 0, 0) ==
        LARDON3D_MATCH_FILE_BAD_SIZE);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_invalid_entry(void) {
  char dir[] = "/tmp/lardon3d-mie-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/bade.mf", dir) > 0);

  unsigned char buf[44];
  memset(buf, 0, 44);
  memcpy(buf, "L3DM", 4);
  buf[4] = 1;
  buf[5] = 1;
  buf[8] = 1;
  buf[12] = 32;
  buf[16] = 1;
  buf[24] = 2;
  buf[32] = 0x0F;
  buf[33] = 0x27;
  buf[34] = 0;
  buf[35] = 0;

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(write(fd, buf, 44) == 44);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_BAD_ENTRY);
  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0);
  Lardon3DMatchFileEntry entries[1];
  uint32_t count = 0;
  CHECK(lardon3d_match_file_read(fd, &header, entries, 1, &count, 1, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_BAD_ENTRY);
  (void)close(fd);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_nan_distance(void) {
  char dir[] = "/tmp/lardon3d-mnd-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/nan.mf", dir) > 0);

  unsigned char buf[44];
  memset(buf, 0, 44);
  memcpy(buf, "L3DM", 4);
  buf[4] = 1;
  buf[5] = 1;
  buf[8] = 1;
  buf[12] = 32;
  buf[16] = 1;
  buf[24] = 2;
  buf[40] = 0;
  buf[41] = 0;
  buf[42] = 0xC0;
  buf[43] = 0x7F;

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(write(fd, buf, 44) == 44);
  (void)close(fd);

  fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  CHECK(fd >= 0);
  Lardon3DMatchFileHeader header;
  Lardon3DMatchFileEntry entries[1];
  uint32_t count = 0;
  CHECK(lardon3d_match_file_read(fd, &header, entries, 1, &count, 1, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_BAD_ENTRY);
  (void)close(fd);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_f32(void) {
  char dir[] = "/tmp/lardon3d-mf32-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/sift.mf", dir) > 0);

  Lardon3DMatchFileEntry entries[1];
  entries[0].feature_index_a = 0;
  entries[0].feature_index_b = 0;
  entries[0].distance = 100.0F;

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(lardon3d_match_file_write(fd, 2, 128, 1, 2, entries, 1) ==
        LARDON3D_MATCH_FILE_OK);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 8192, 8192) ==
        LARDON3D_MATCH_FILE_OK);
  CHECK(header.descriptor_type == 2);
  CHECK(header.descriptor_dimension == 128);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_bad_type(void) {
  unsigned char buf[32] = {0};
  memcpy(buf, "L3DM", 4);
  buf[4] = 1;
  buf[5] = 3;

  char dir[] = "/tmp/lardon3d-mbt-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/badtype.mf", dir) > 0);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(write(fd, buf, 32) == 32);
  (void)close(fd);

  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 0, 0, 0, 0) ==
        LARDON3D_MATCH_FILE_BAD_TYPE);

  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool test_match_file_maximum_and_multiplicity(void) {
  char dir[] = "/tmp/lardon3d-mmax-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/max.mf", dir) > 0);
  Lardon3DMatchFileEntry *entries = calloc(LARDON3D_MATCH_FILE_MAX_MATCHES,
                                           sizeof(*entries));
  CHECK(entries);
  for (uint32_t i = 0; i < LARDON3D_MATCH_FILE_MAX_MATCHES; ++i) {
    entries[i].feature_index_a = i;
    entries[i].feature_index_b = 0;
    entries[i].distance = (float)i;
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK(lardon3d_match_file_write(fd, 1, 32, 11, 12, entries,
                                  LARDON3D_MATCH_FILE_MAX_MATCHES) ==
        LARDON3D_MATCH_FILE_OK);
  CHECK(lardon3d_match_file_write(fd, 1, 32, 11, 12, entries,
                                  LARDON3D_MATCH_FILE_MAX_MATCHES + 1) ==
        LARDON3D_MATCH_FILE_INVALID_ARGUMENT);
  CHECK(close(fd) == 0);
  struct stat info;
  CHECK(stat(path, &info) == 0 && info.st_size == LARDON3D_MATCH_FILE_MAX_SIZE);
  Lardon3DMatchFileHeader header;
  CHECK(lardon3d_match_file_validate(path, &header, 11, 12, 8192, 1) ==
        LARDON3D_MATCH_FILE_OK);
  free(entries);
  CHECK(unlink(path) == 0);
  CHECK(rmdir(dir) == 0);
  return true;
}

static bool write_minimal_match_file(const char *path) {
  Lardon3DMatchFileEntry entry = {0, 0, 1.0F};
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = lardon3d_match_file_write(fd, 1, 32, 1, 2, &entry, 1) ==
            LARDON3D_MATCH_FILE_OK;
  return close(fd) == 0 && ok;
}

static bool mutate_bytes(const char *path, off_t offset, const void *bytes, size_t size) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = pwrite(fd, bytes, size, offset) == (ssize_t)size;
  return close(fd) == 0 && ok;
}

static bool expect_mutation(const char *path, off_t offset, const void *bytes, size_t size,
                            Lardon3DMatchFileResult expected) {
  if (!write_minimal_match_file(path) || !mutate_bytes(path, offset, bytes, size)) return false;
  Lardon3DMatchFileHeader header;
  return lardon3d_match_file_validate(path, &header, 1, 2, 1, 1) == expected;
}

static bool test_match_file_corruption_matrix(void) {
  char dir[] = "/tmp/lardon3d-matrix-XXXXXX";
  CHECK(mkdtemp(dir));
  char path[512];
  CHECK(snprintf(path, sizeof(path), "%s/match", dir) > 0);
  Lardon3DMatchFileHeader header;

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0 && close(fd) == 0);
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 1, 1) ==
        LARDON3D_MATCH_FILE_BAD_SIZE);
  unsigned char one = 0;
  CHECK(mutate_bytes(path, 0, &one, 1));
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 1, 1) ==
        LARDON3D_MATCH_FILE_BAD_SIZE);

  unsigned char version_zero = 0;
  unsigned char version_future = 2;
  unsigned char type_zero = 0;
  unsigned char type_unknown = 99;
  unsigned char reserved[2] = {1, 0};
  unsigned char dimension[4] = {31, 0, 0, 0};
  unsigned char zero_id[8] = {0};
  unsigned char count_8193[4] = {1, 32, 0, 0};
  unsigned char count_max[4] = {255, 255, 255, 255};
  unsigned char bad_magic[4] = {'B', 'A', 'D', '!'};
  CHECK(expect_mutation(path, 0, bad_magic, 4, LARDON3D_MATCH_FILE_BAD_MAGIC));
  CHECK(expect_mutation(path, 4, &version_zero, 1, LARDON3D_MATCH_FILE_BAD_VERSION));
  CHECK(expect_mutation(path, 4, &version_future, 1, LARDON3D_MATCH_FILE_BAD_VERSION));
  CHECK(expect_mutation(path, 5, &type_zero, 1, LARDON3D_MATCH_FILE_BAD_TYPE));
  CHECK(expect_mutation(path, 5, &type_unknown, 1, LARDON3D_MATCH_FILE_BAD_TYPE));
  CHECK(expect_mutation(path, 6, reserved, 2, LARDON3D_MATCH_FILE_CORRUPT));
  CHECK(expect_mutation(path, 12, dimension, 4, LARDON3D_MATCH_FILE_BAD_TYPE));
  CHECK(expect_mutation(path, 16, zero_id, 8, LARDON3D_MATCH_FILE_CORRUPT));
  CHECK(expect_mutation(path, 24, zero_id, 8, LARDON3D_MATCH_FILE_CORRUPT));
  CHECK(expect_mutation(path, 8, count_8193, 4, LARDON3D_MATCH_FILE_BAD_COUNT));
  CHECK(expect_mutation(path, 8, count_max, 4, LARDON3D_MATCH_FILE_BAD_COUNT));

  unsigned char index_one[4] = {1, 0, 0, 0};
  CHECK(expect_mutation(path, 32, index_one, 4, LARDON3D_MATCH_FILE_BAD_ENTRY));
  CHECK(expect_mutation(path, 36, index_one, 4, LARDON3D_MATCH_FILE_BAD_ENTRY));
  unsigned char negative[4] = {0, 0, 128, 191};
  unsigned char negative_zero[4] = {0, 0, 0, 128};
  unsigned char positive_inf[4] = {0, 0, 128, 127};
  unsigned char negative_inf[4] = {0, 0, 128, 255};
  unsigned char nan[4] = {0, 0, 192, 127};
  CHECK(expect_mutation(path, 40, negative, 4, LARDON3D_MATCH_FILE_BAD_ENTRY));
  CHECK(expect_mutation(path, 40, positive_inf, 4, LARDON3D_MATCH_FILE_BAD_ENTRY));
  CHECK(expect_mutation(path, 40, negative_inf, 4, LARDON3D_MATCH_FILE_BAD_ENTRY));
  CHECK(expect_mutation(path, 40, nan, 4, LARDON3D_MATCH_FILE_BAD_ENTRY));
  CHECK(expect_mutation(path, 40, negative_zero, 4, LARDON3D_MATCH_FILE_OK));

  CHECK(write_minimal_match_file(path));
  fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
  CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 1, 1) ==
        LARDON3D_MATCH_FILE_BAD_SIZE);

  Lardon3DMatchFileEntry ordered[2] = {{0, 0, 0.0F}, {1, 0, 1.0F}};
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0 && lardon3d_match_file_write(fd, 1, 32, 1, 2, ordered, 2) ==
                       LARDON3D_MATCH_FILE_OK &&
        close(fd) == 0);
  unsigned char duplicate_a[4] = {0, 0, 0, 0};
  CHECK(mutate_bytes(path, 44, duplicate_a, sizeof(duplicate_a)));
  CHECK(lardon3d_match_file_validate(path, &header, 1, 2, 2, 1) ==
        LARDON3D_MATCH_FILE_BAD_ENTRY);
  ordered[1].feature_index_a = 0;
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  CHECK(fd >= 0 && lardon3d_match_file_write(fd, 1, 32, 1, 2, ordered, 2) ==
                       LARDON3D_MATCH_FILE_BAD_ENTRY &&
        close(fd) == 0);
  CHECK(unlink(path) == 0 && rmdir(dir) == 0);
  return true;
}

/* --- Matcher utility function tests --- */

static bool test_matcher_kind_string(void) {
  CHECK(strcmp(lardon3d_matcher_kind_string(LARDON3D_MATCHER_ORB_BF), "orb_bf") == 0);
  CHECK(strcmp(lardon3d_matcher_kind_string(LARDON3D_MATCHER_SIFT_BF), "sift_bf") == 0);
  CHECK(strcmp(lardon3d_matcher_kind_string(LARDON3D_MATCHER_ROOTSIFT_BF), "rootsift_bf") == 0);
  return true;
}

static bool test_matcher_fingerprint(void) {
  unsigned char fp1[32], fp2[32];
  Lardon3DMatcherParams p1 = {LARDON3D_MATCHER_ORB_BF, 0.75F};
  Lardon3DMatcherParams p2 = {LARDON3D_MATCHER_ORB_BF, 0.80F};
  lardon3d_matcher_fingerprint(&p1, fp1);
  lardon3d_matcher_fingerprint(&p2, fp2);
  CHECK(memcmp(fp1, fp2, 32) != 0);

  unsigned char fp3[32];
  lardon3d_matcher_fingerprint(&p1, fp3);
  CHECK(memcmp(fp1, fp3, 32) == 0);

  Lardon3DMatcherParams p3 = {LARDON3D_MATCHER_SIFT_BF, 0.75F};
  unsigned char fp4[32];
  lardon3d_matcher_fingerprint(&p3, fp4);
  CHECK(memcmp(fp1, fp4, 32) != 0);

  return true;
}

static bool test_matcher_default_ratio(void) {
  float r = lardon3d_matcher_default_ratio(LARDON3D_MATCHER_ORB_BF);
  CHECK(r == 0.75F);
  r = lardon3d_matcher_default_ratio(LARDON3D_MATCHER_SIFT_BF);
  CHECK(r == 0.7F);
  r = lardon3d_matcher_default_ratio(LARDON3D_MATCHER_ROOTSIFT_BF);
  CHECK(r == 0.7F);
  return true;
}

static bool test_matcher_private_path_bound(void) {
  char project_path[4097];
  memset(project_path, 'x', sizeof(project_path) - 1);
  project_path[sizeof(project_path) - 1] = '\0';
  Lardon3DProjectDbFeatureSet feature_set = {
      .feature_set_id = 1,
      .feature_count = 769,
      .descriptor_type = LARDON3D_FEATURE_DESCRIPTOR_U8,
      .descriptor_dimension = 32,
  };
  Lardon3DMatcherParams parameters = {
      .kind = LARDON3D_MATCHER_ORB_BF,
      .ratio_threshold = 0.75F,
  };
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  Lardon3DMatcherPendingVulkanStage *pending =
      (Lardon3DMatcherPendingVulkanStage *)(uintptr_t)1;
  bool backend_fault = true;
  CHECK(backend);
  Lardon3DMatcherResult expected = lardon3d_orb_vulkan_should_use(769, 769)
      ? LARDON3D_MATCHER_IO_ERROR : LARDON3D_MATCHER_INVALID_ARGUMENT;
  CHECK(lardon3d_matcher_begin_vulkan_stage(
            project_path, &feature_set, &feature_set, &parameters, backend,
            &pending, &backend_fault) == expected &&
        pending == NULL && !backend_fault);
  lardon3d_orb_vulkan_backend_destroy(backend);
  return true;
}

static bool run_tests(void) {
  CHECK(test_match_file_write_read());
  CHECK(test_match_file_empty());
  CHECK(test_match_file_bad_magic());
  CHECK(test_match_file_bad_version());
  CHECK(test_match_file_truncated());
  CHECK(test_match_file_invalid_entry());
  CHECK(test_match_file_nan_distance());
  CHECK(test_match_file_f32());
  CHECK(test_match_file_bad_type());
  CHECK(test_match_file_maximum_and_multiplicity());
  CHECK(test_match_file_corruption_matrix());
  CHECK(test_matcher_kind_string());
  CHECK(test_matcher_fingerprint());
  CHECK(test_matcher_default_ratio());
  CHECK(test_matcher_private_path_bound());
  return true;
}

int main(void) {
  return run_tests() ? EXIT_SUCCESS : EXIT_FAILURE;
}
