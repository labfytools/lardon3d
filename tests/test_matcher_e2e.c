#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <lardon3d/app_state.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/matcher.h>
#include <lardon3d/project_db.h>

#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "Échec ligne %d: %s\n", __LINE__, #x);                                    \
      return false;                                                                                \
    }                                                                                              \
  } while (0)

typedef struct {
  char root[PATH_MAX];
  char database_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DAppState state;
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbImage image_a;
  Lardon3DProjectDbImage image_b;
  Lardon3DProjectDbCandidatePair pair;
} Fixture;

static bool join_path(char output[PATH_MAX], const char *a, const char *b) {
  int n = snprintf(output, PATH_MAX, "%s/%s", a, b);
  return n > 0 && (size_t)n < PATH_MAX;
}

static bool remove_tree(const char *path) {
  struct stat info;
  if (lstat(path, &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return unlink(path) == 0;
  DIR *directory = opendir(path);
  if (!directory) return false;
  bool ok = true;
  for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char child[PATH_MAX];
    if (!join_path(child, path, entry->d_name) || !remove_tree(child)) ok = false;
  }
  if (closedir(directory) != 0 || rmdir(path) != 0) ok = false;
  return ok;
}

static void image_asset_path(const unsigned char hash[32], char path[4096]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < 32; ++i) {
    hex[2 * i] = digits[hash[i] >> 4];
    hex[2 * i + 1] = digits[hash[i] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, 4096, "assets/images/%c%c/%s", hex[0], hex[1], hex);
}

static bool register_image(Fixture *fixture, unsigned char seed,
                           Lardon3DProjectDbImage *image) {
  unsigned char hash[32];
  memset(hash, seed, sizeof(hash));
  char path[4096];
  image_asset_path(hash, path);
  Lardon3DProjectDbImageRegisterStatus status;
  return lardon3d_project_db_register_image(
             fixture->database, fixture->scanset.scanset_id, hash, path, 1, "fixture.bin",
             "/synthetic/fixture.bin", 0, seed, &status, image) == LARDON3D_PROJECT_DB_OK;
}

static bool fixture_open(Fixture *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  char root[] = "/tmp/lardon3d-matcher-e2e-XXXXXX";
  char *created = mkdtemp(root);
  if (!created || snprintf(fixture->root, sizeof(fixture->root), "%s", created) <= 0 ||
      !join_path(fixture->database_path, fixture->root, "project.db"))
    return false;
  char error[256];
  if (lardon3d_project_db_open(fixture->database_path, &fixture->database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  lardon3d_app_state_init(&fixture->state);
  fixture->state.project_loaded = true;
  fixture->state.project_db = fixture->database;
  (void)snprintf(fixture->state.project_path, sizeof(fixture->state.project_path), "%s",
                 fixture->root);
  return lardon3d_project_db_create_scanset(fixture->database, "matcher-e2e",
                                             &fixture->scanset) == LARDON3D_PROJECT_DB_OK &&
         register_image(fixture, 1, &fixture->image_a) &&
         register_image(fixture, 2, &fixture->image_b) &&
         lardon3d_project_db_create_candidate_pair(
             fixture->database, fixture->image_a.image_id, fixture->image_b.image_id, 1,
             &fixture->pair) == LARDON3D_PROJECT_DB_OK;
}

static bool fixture_close(Fixture *fixture) {
  char matches_path[PATH_MAX];
  bool no_temps = true;
  if (join_path(matches_path, fixture->root, "assets/matches")) {
    DIR *directory = opendir(matches_path);
    if (directory) {
      for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        if (strncmp(entry->d_name, ".match-", 7) == 0) no_temps = false;
      }
      if (closedir(directory) != 0) no_temps = false;
    } else if (errno != ENOENT) {
      no_temps = false;
    }
  } else {
    no_temps = false;
  }
  if (fixture->database) lardon3d_project_db_close(fixture->database);
  fixture->database = NULL;
  fixture->state.project_db = NULL;
  bool removed = remove_tree(fixture->root);
  return no_temps && removed;
}

static bool publish_features(Fixture *fixture, uint64_t image_id, const char *kind,
                             Lardon3DFeatureDescriptorType type, const void *descriptors,
                             uint32_t count, unsigned char fingerprint_seed,
                             Lardon3DProjectDbFeatureSet *set) {
  Lardon3DExtractedFeatures features;
  memset(&features, 0, sizeof(features));
  features.image_width = 64;
  features.image_height = 64;
  features.feature_count = count;
  if (count > 0) {
    features.keypoints = calloc(count, sizeof(*features.keypoints));
    if (!features.keypoints) return false;
    for (uint32_t i = 0; i < count; ++i) features.keypoints[i].size = 1.0F;
    size_t scalar = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 1 : sizeof(float);
    size_t dimension = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 32 : 128;
    features.descriptor_bytes = (size_t)count * dimension * scalar;
    features.descriptors = malloc(features.descriptor_bytes);
    if (!features.descriptors) {
      free(features.keypoints);
      return false;
    }
    memcpy(features.descriptors, descriptors, features.descriptor_bytes);
  }
  unsigned char fingerprint[32];
  memset(fingerprint, fingerprint_seed, sizeof(fingerprint));
  uint32_t dimension = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 32 : 128;
  Lardon3DFeatureStoreResult result = lardon3d_feature_store_publish_v2(
      &fixture->state, image_id, 0, kind, 1, fingerprint, type, dimension, 0, &features, set);
  free(features.keypoints);
  free(features.descriptors);
  return result == LARDON3D_FEATURE_STORE_OK;
}

static bool read_result_asset(const Fixture *fixture, const Lardon3DProjectDbFeatureSet *set_a,
                              const Lardon3DProjectDbFeatureSet *set_b,
                              const Lardon3DProjectDbMatchResult *result,
                              Lardon3DMatchFileEntry *entries, size_t capacity,
                              uint32_t *count) {
  char path[PATH_MAX];
  if (!join_path(path, fixture->root, result->match_asset_path)) return false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  Lardon3DMatchFileHeader header;
  Lardon3DMatchFileResult read_result = lardon3d_match_file_read(
      fd, &header, entries, capacity, count, set_a->feature_set_id, set_b->feature_set_id,
      set_a->feature_count, set_b->feature_count);
  (void)close(fd);
  return read_result == LARDON3D_MATCH_FILE_OK;
}

#ifdef LARDON3D_MATCHER_E2E_VULKAN
static bool files_equal(const char *path_a, const char *path_b) {
  int fd_a = open(path_a, O_RDONLY | O_CLOEXEC);
  int fd_b = open(path_b, O_RDONLY | O_CLOEXEC);
  if (fd_a < 0 || fd_b < 0) {
    if (fd_a >= 0) (void)close(fd_a);
    if (fd_b >= 0) (void)close(fd_b);
    return false;
  }
  unsigned char a[4096];
  unsigned char b[4096];
  bool equal = true;
  for (;;) {
    ssize_t read_a = read(fd_a, a, sizeof(a));
    ssize_t read_b = read(fd_b, b, sizeof(b));
    if (read_a < 0 || read_b < 0 || read_a != read_b ||
        (read_a > 0 && memcmp(a, b, (size_t)read_a) != 0)) {
      equal = false;
      break;
    }
    if (read_a == 0) break;
  }
  if (close(fd_a) != 0 || close(fd_b) != 0) equal = false;
  return equal;
}

static bool file_sha256(const char *path, unsigned char output[32]) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (fd < 0 || !context) {
    if (fd >= 0) (void)close(fd);
    EVP_MD_CTX_free(context);
    return false;
  }
  bool ok = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
  unsigned char buffer[4096];
  while (ok) {
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      ok = false;
    } else if (count == 0) {
      break;
    } else {
      ok = EVP_DigestUpdate(context, buffer, (size_t)count) == 1;
    }
  }
  unsigned int length = 0;
  ok = ok && EVP_DigestFinal_ex(context, output, &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  return close(fd) == 0 && ok;
}

static uint32_t deterministic_random(uint32_t *state) {
  *state = *state * 1664525U + 1013904223U;
  return *state;
}

static bool test_cpu_vulkan_match_file_parity(void) {
  const uint32_t feature_count = 768;
  const size_t bytes = (size_t)feature_count * 32;
  Fixture fixture;
  CHECK(fixture_open(&fixture));
  unsigned char *descriptors = malloc(bytes);
  CHECK(descriptors != NULL);
  uint32_t random_state = 0x12345678U;
  for (size_t index = 0; index < bytes; ++index) {
    descriptors[index] = (unsigned char)(deterministic_random(&random_state) >> 24);
  }
  Lardon3DProjectDbFeatureSet set_a, set_b;
  CHECK(publish_features(&fixture, fixture.image_a.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, descriptors, feature_count, 31,
                         &set_a));
  CHECK(publish_features(&fixture, fixture.image_b.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, descriptors, feature_count, 32,
                         &set_b));
  free(descriptors);

  char cpu_path[PATH_MAX];
  char vulkan_path[PATH_MAX];
  char fallback_path[PATH_MAX];
  CHECK(join_path(cpu_path, fixture.root, "cpu.match"));
  CHECK(join_path(vulkan_path, fixture.root, "vulkan.match"));
  CHECK(join_path(fallback_path, fixture.root, "fallback.match"));
  Lardon3DMatcherParams params = {LARDON3D_MATCHER_ORB_BF, 0.75F};
  Lardon3DMatcherStats cpu_stats;
  Lardon3DMatcherStats vulkan_stats;
  CHECK(lardon3d_matcher_run(fixture.root, &set_a, &set_b, &params, cpu_path,
                             &cpu_stats) == LARDON3D_MATCHER_OK);
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  CHECK(backend != NULL);
  CHECK(lardon3d_matcher_run_with_backend(fixture.root, &set_a, &set_b, &params,
                                          vulkan_path, backend, &vulkan_stats) ==
        LARDON3D_MATCHER_OK);
  CHECK(vulkan_stats.used_vulkan && !vulkan_stats.vulkan_fallback);
  CHECK(cpu_stats.match_count == vulkan_stats.match_count);
  CHECK(files_equal(cpu_path, vulkan_path));
  unsigned char cpu_sha[32];
  unsigned char vulkan_sha[32];
  CHECK(file_sha256(cpu_path, cpu_sha));
  CHECK(file_sha256(vulkan_path, vulkan_sha));
  CHECK(memcmp(cpu_sha, vulkan_sha, sizeof(cpu_sha)) == 0);
  lardon3d_orb_vulkan_backend_destroy(backend);

  CHECK(setenv("LARDON3D_VULKAN_DISABLE", "1", 1) == 0);
  backend = lardon3d_orb_vulkan_backend_create();
  CHECK(backend != NULL);
  Lardon3DMatcherStats fallback_stats;
  CHECK(lardon3d_matcher_run_with_backend(fixture.root, &set_a, &set_b, &params,
                                          fallback_path, backend, &fallback_stats) ==
        LARDON3D_MATCHER_OK);
  CHECK(fallback_stats.vulkan_fallback && !fallback_stats.used_vulkan);
  CHECK(files_equal(cpu_path, fallback_path));
  lardon3d_orb_vulkan_backend_destroy(backend);
  CHECK(unsetenv("LARDON3D_VULKAN_DISABLE") == 0);
  CHECK(unlink(cpu_path) == 0);
  CHECK(unlink(vulkan_path) == 0);
  CHECK(unlink(fallback_path) == 0);
  CHECK(fixture_close(&fixture));
  return true;
}
#endif

static bool run_kind_e2e(const char *kind, Lardon3DMatcherKind matcher_kind,
                         Lardon3DFeatureDescriptorType type) {
  Fixture fixture;
  CHECK(fixture_open(&fixture));
  uint32_t dimension = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 32 : 128;
  size_t scalar = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 1 : sizeof(float);
  size_t row_bytes = (size_t)dimension * scalar;
  unsigned char *a = calloc(3, row_bytes);
  unsigned char *b = calloc(3, row_bytes);
  CHECK(a && b);
  if (type == LARDON3D_FEATURE_DESCRIPTOR_U8) {
    memset(a + row_bytes, 0xFF, row_bytes);
    memset(b + row_bytes, 0xF0, row_bytes);
    memset(b + 2 * row_bytes, 0xFF, row_bytes);
  } else {
    float *af = (float *)a;
    float *bf = (float *)b;
    for (uint32_t i = 0; i < dimension; ++i) {
      af[dimension + i] = 2.0F;
      bf[dimension + i] = 1.0F;
      bf[2 * dimension + i] = 2.0F;
    }
  }
  Lardon3DProjectDbFeatureSet set_a, set_b;
  CHECK(publish_features(&fixture, fixture.image_a.image_id, kind, type, a, 3, 3, &set_a));
  CHECK(publish_features(&fixture, fixture.image_b.image_id, kind, type, b, 3, 4, &set_b));
  free(a);
  free(b);

  Lardon3DMatcherParams params = {matcher_kind, lardon3d_matcher_default_ratio(matcher_kind)};
  Lardon3DProjectDbMatchResult result;
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.result_status == LARDON3D_MATCH_RESULT_STATUS_MATCHED &&
        result.match_count == 3 && result.has_match_asset);
  uint64_t result_id = result.match_result_id;
  unsigned char asset_hash[32];
  memcpy(asset_hash, result.match_asset_sha256, 32);
  Lardon3DMatchFileEntry entries[3];
  uint32_t count = 0;
  CHECK(read_result_asset(&fixture, &set_a, &set_b, &result, entries, 3, &count));
  CHECK(count == 3 && entries[0].feature_index_a == 0 && entries[0].feature_index_b == 0 &&
        entries[1].feature_index_a == 1 && entries[1].feature_index_b == 2 &&
        entries[2].feature_index_a == 2 && entries[2].feature_index_b == 0);

  lardon3d_project_db_close(fixture.database);
  fixture.database = NULL;
  fixture.state.project_db = NULL;
  char error[256];
  CHECK(lardon3d_project_db_open(fixture.database_path, &fixture.database, error) ==
        LARDON3D_PROJECT_DB_OK);
  fixture.state.project_db = fixture.database;
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.match_result_id == result_id &&
        memcmp(result.match_asset_sha256, asset_hash, 32) == 0);

  char asset_path[PATH_MAX];
  CHECK(join_path(asset_path, fixture.root, result.match_asset_path));
  int fd = open(asset_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  CHECK(fd >= 0 && write(fd, "corrupt", 7) == 7 && close(fd) == 0);
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.match_result_id == result_id && result.match_count == 3 &&
        read_result_asset(&fixture, &set_a, &set_b, &result, entries, 3, &count));

  fd = open(asset_path, O_WRONLY | O_CLOEXEC);
  unsigned char wrong_feature_set[8] = {99, 0, 0, 0, 0, 0, 0, 0};
  CHECK(fd >= 0 && pwrite(fd, wrong_feature_set, sizeof(wrong_feature_set), 16) ==
                       (ssize_t)sizeof(wrong_feature_set) &&
        close(fd) == 0);
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.match_result_id == result_id &&
        read_result_asset(&fixture, &set_a, &set_b, &result, entries, 3, &count));

  CHECK(unlink(asset_path) == 0);
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.match_result_id == result_id &&
        read_result_asset(&fixture, &set_a, &set_b, &result, entries, 3, &count));

  Lardon3DMatcherParams changed = params;
  changed.ratio_threshold -= 0.05F;
  Lardon3DProjectDbMatchResult distinct;
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &changed, &distinct) ==
        LARDON3D_MATCHER_OK);
  CHECK(distinct.match_result_id != result_id &&
        memcmp(distinct.match_asset_sha256, result.match_asset_sha256, 32) == 0);
  for (int iteration = 0; iteration < 100; ++iteration) {
    CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                             &set_a, &set_b, &params, &result) ==
          LARDON3D_MATCHER_OK);
    CHECK(result.match_result_id == result_id && result.match_count == 3);
  }
  CHECK(fixture_close(&fixture));
  return true;
}

static bool test_ratio_single_neighbor_and_no_match(void) {
  Fixture fixture;
  CHECK(fixture_open(&fixture));
  unsigned char a[32], b[64];
  memset(a, 0x0F, sizeof(a));
  memset(b, 0, 32);
  memset(b + 32, 0xFF, 32);
  Lardon3DProjectDbFeatureSet set_a, set_b;
  CHECK(publish_features(&fixture, fixture.image_a.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, a, 1, 10, &set_a));
  CHECK(publish_features(&fixture, fixture.image_b.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, b, 2, 11, &set_b));
  Lardon3DMatcherParams params = {LARDON3D_MATCHER_ORB_BF, 0.75F};
  Lardon3DProjectDbMatchResult result;
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.result_status == LARDON3D_MATCH_RESULT_STATUS_NO_MATCH &&
        result.match_count == 0 && !result.has_match_asset);
  uint64_t no_match_id = result.match_result_id;
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK && result.match_result_id == no_match_id);
  for (int iteration = 0; iteration < 100; ++iteration) {
    CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                             &set_a, &set_b, &params, &result) ==
          LARDON3D_MATCHER_OK);
    CHECK(result.match_result_id == no_match_id && !result.has_match_asset);
  }
  CHECK(fixture_close(&fixture));

  CHECK(fixture_open(&fixture));
  memset(a, 0, sizeof(a));
  memset(b, 0, 32);
  CHECK(publish_features(&fixture, fixture.image_a.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, a, 1, 12, &set_a));
  CHECK(publish_features(&fixture, fixture.image_b.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, b, 1, 13, &set_b));
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.result_status == LARDON3D_MATCH_RESULT_STATUS_MATCHED && result.match_count == 1);
  CHECK(fixture_close(&fixture));

  CHECK(fixture_open(&fixture));
  CHECK(publish_features(&fixture, fixture.image_a.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, a, 1, 14, &set_a));
  CHECK(publish_features(&fixture, fixture.image_b.image_id, "orb",
                         LARDON3D_FEATURE_DESCRIPTOR_U8, NULL, 0, 15, &set_b));
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &set_a, &set_b, &params, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.result_status == LARDON3D_MATCH_RESULT_STATUS_NO_MATCH && result.match_count == 0);
  CHECK(fixture_close(&fixture));
  return true;
}

static bool test_kind_and_ownership_rejection(void) {
  Fixture fixture;
  CHECK(fixture_open(&fixture));
  float descriptors[128] = {0};
  Lardon3DProjectDbFeatureSet sift_a, sift_b;
  CHECK(publish_features(&fixture, fixture.image_b.image_id, "sift",
                         LARDON3D_FEATURE_DESCRIPTOR_F32, descriptors, 1, 21, &sift_b));
  CHECK(publish_features(&fixture, fixture.image_a.image_id, "sift",
                         LARDON3D_FEATURE_DESCRIPTOR_F32, descriptors, 1, 20, &sift_a));
  CHECK(sift_a.feature_set_id > sift_b.feature_set_id);
  Lardon3DMatcherParams rootsift = {LARDON3D_MATCHER_ROOTSIFT_BF, 0.7F};
  Lardon3DMatcherStats stats;
  char output[PATH_MAX];
  CHECK(join_path(output, fixture.root, "wrong.match"));
  CHECK(lardon3d_matcher_run(fixture.root, &sift_a, &sift_b, &rootsift, output, &stats) ==
        LARDON3D_MATCHER_TYPE_MISMATCH);
  Lardon3DMatcherParams sift = {LARDON3D_MATCHER_SIFT_BF, 0.7F};
  Lardon3DProjectDbMatchResult result;
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &sift_a, &sift_b, &sift, &result) ==
        LARDON3D_MATCHER_OK);
  CHECK(result.result_status == LARDON3D_MATCH_RESULT_STATUS_MATCHED);
  CHECK(lardon3d_matcher_match_and_publish(fixture.root, fixture.database, &fixture.pair,
                                           &sift_b, &sift_a, &sift, &result) ==
        LARDON3D_MATCHER_FAILED);
  CHECK(fixture_close(&fixture));
  return true;
}

static bool run_tests(void) {
  CHECK(run_kind_e2e("orb", LARDON3D_MATCHER_ORB_BF,
                     LARDON3D_FEATURE_DESCRIPTOR_U8));
  CHECK(run_kind_e2e("sift", LARDON3D_MATCHER_SIFT_BF,
                     LARDON3D_FEATURE_DESCRIPTOR_F32));
  CHECK(run_kind_e2e("rootsift", LARDON3D_MATCHER_ROOTSIFT_BF,
                     LARDON3D_FEATURE_DESCRIPTOR_F32));
  CHECK(test_ratio_single_neighbor_and_no_match());
  CHECK(test_kind_and_ownership_rejection());
#ifdef LARDON3D_MATCHER_E2E_VULKAN
  CHECK(test_cpu_vulkan_match_file_parity());
#endif
  return true;
}

int main(void) {
  return run_tests() ? EXIT_SUCCESS : EXIT_FAILURE;
}
