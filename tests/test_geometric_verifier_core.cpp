#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/app_state.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/geometric_verifier.h>
#include <lardon3d/match_file.h>
}

#include "../src/geometric_verifier_internal.h"

static int failures;

struct Fixture {
  char root[PATH_MAX];
  char database_path[PATH_MAX];
  Lardon3DAppState state;
  Lardon3DProjectDbFeatureSet set_a;
  Lardon3DProjectDbFeatureSet set_b;
  Lardon3DProjectDbCandidatePair pair;
  uint32_t parent_sequence;
};

static void to_hex(const unsigned char *bytes, size_t size, char *output) {
  static const char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < size; ++index) {
    output[2 * index] = digits[bytes[index] >> 4U];
    output[2 * index + 1] = digits[bytes[index] & 15U];
  }
  output[2 * size] = '\0';
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                   #expression);                                               \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static bool join_path(char output[PATH_MAX], const char *left,
                      const char *right) {
  int length = std::snprintf(output, PATH_MAX, "%s/%s", left, right);
  return length > 0 && length < PATH_MAX;
}

static bool remove_tree(const char *path) {
  struct stat information;
  if (lstat(path, &information) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(information.st_mode))
    return unlink(path) == 0;
  DIR *directory = opendir(path);
  if (!directory)
    return false;
  bool ok = true;
  for (dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0)
      continue;
    char child[PATH_MAX];
    if (!join_path(child, path, entry->d_name) || !remove_tree(child))
      ok = false;
  }
  if (closedir(directory) != 0 || rmdir(path) != 0)
    ok = false;
  return ok;
}

static bool execute_sql(const char *path, const char *sql) {
  sqlite3 *connection = nullptr;
  if (sqlite3_open(path, &connection) != SQLITE_OK)
    return false;
  bool ok =
      sqlite3_exec(connection, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  return sqlite3_close(connection) == SQLITE_OK && ok;
}

static void image_asset_path(const unsigned char hash[32], char path[4096]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t index = 0; index < 32; ++index) {
    hex[2 * index] = digits[hash[index] >> 4U];
    hex[2 * index + 1] = digits[hash[index] & 15U];
  }
  hex[64] = '\0';
  (void)std::snprintf(path, 4096, "assets/images/%c%c/%s", hex[0], hex[1], hex);
}

static bool register_image(Fixture *fixture, unsigned char seed,
                           Lardon3DProjectDbImage *image) {
  unsigned char hash[32];
  std::memset(hash, seed, sizeof(hash));
  char path[4096];
  image_asset_path(hash, path);
  Lardon3DProjectDbImageRegisterStatus status;
  return lardon3d_project_db_register_image(
             fixture->state.project_db, 1, hash, path, 1, "fixture.bin",
             "/fixture.bin", 0, seed, &status, image) == LARDON3D_PROJECT_DB_OK;
}

static bool publish_features(Fixture *fixture,
                             const Lardon3DProjectDbImage *image,
                             unsigned char salt,
                             Lardon3DProjectDbFeatureSet *set) {
  constexpr uint32_t feature_count = LARDON3D_MATCH_FILE_MAX_MATCHES;
  std::vector<Lardon3DFeatureKeypoint> keypoints(feature_count);
  std::vector<unsigned char> descriptors(
      static_cast<size_t>(feature_count) * 32, salt);
  for (uint32_t index = 0; index < feature_count; ++index) {
    uint32_t source = salt == 4 ? (index * 3511U) % feature_count : index;
    keypoints[index].x = 20.0F + static_cast<float>(source % 128U) * 7.0F;
    keypoints[index].y = 20.0F + static_cast<float>(source / 128U) * 7.0F;
    if (salt == 4)
      keypoints[index].x -= 5.0F;
    keypoints[index].size = 1.0F;
  }
  // Distinct observation IDs may legitimately share coordinates; v2 support
  // must count identities rather than unique Point2d values.
  keypoints[1].x = keypoints[0].x;
  keypoints[1].y = keypoints[0].y;
  Lardon3DExtractedFeatures features = {};
  features.image_width = 1024;
  features.image_height = 768;
  features.feature_count = feature_count;
  features.keypoints = keypoints.data();
  features.descriptors = descriptors.data();
  features.descriptor_bytes = descriptors.size();
  unsigned char fingerprint[32];
  std::memset(fingerprint, salt, sizeof(fingerprint));
  return lardon3d_feature_store_publish_v2(
             &fixture->state, image->image_id, 0, "orb", 1, fingerprint,
             LARDON3D_FEATURE_DESCRIPTOR_U8, 32, 0, &features,
             set) == LARDON3D_FEATURE_STORE_OK;
}

static bool fixture_create(Fixture *fixture) {
  std::memset(fixture, 0, sizeof(*fixture));
  char root[] = "/tmp/lardon3d-geometric-core-XXXXXX";
  char *created = mkdtemp(root);
  if (!created ||
      std::snprintf(fixture->root, sizeof(fixture->root), "%s", created) <= 0 ||
      !join_path(fixture->database_path, fixture->root, "project.db"))
    return false;
  lardon3d_app_state_init(&fixture->state);
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  if (lardon3d_project_db_open(fixture->database_path,
                               &fixture->state.project_db,
                               error) != LARDON3D_PROJECT_DB_OK)
    return false;
  fixture->state.project_loaded = true;
  (void)std::snprintf(fixture->state.project_path,
                      sizeof(fixture->state.project_path), "%s", fixture->root);
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbImage image_a, image_b;
  return lardon3d_project_db_create_scanset(fixture->state.project_db,
                                            "geometry", &scanset) ==
             LARDON3D_PROJECT_DB_OK &&
         scanset.scanset_id == 1 && register_image(fixture, 1, &image_a) &&
         register_image(fixture, 2, &image_b) &&
         publish_features(fixture, &image_a, 3, &fixture->set_a) &&
         publish_features(fixture, &image_b, 4, &fixture->set_b) &&
         lardon3d_project_db_create_candidate_pair(
             fixture->state.project_db, image_a.image_id, image_b.image_id, 1,
             &fixture->pair) == LARDON3D_PROJECT_DB_OK;
}

static void fixture_destroy(Fixture *fixture) {
  if (fixture->state.project_db)
    lardon3d_project_db_close(fixture->state.project_db);
  fixture->state.project_db = nullptr;
  CHECK(remove_tree(fixture->root));
}

static bool sha256_file(const char *path, unsigned char output[32],
                        uint64_t *size) {
  int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    return false;
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  unsigned char bytes[4096];
  uint64_t total = 0;
  for (;;) {
    ssize_t count = read(descriptor, bytes, sizeof(bytes));
    if (count < 0) {
      ok = false;
      break;
    }
    if (count == 0)
      break;
    total += static_cast<uint64_t>(count);
    if (EVP_DigestUpdate(context, bytes, static_cast<size_t>(count)) != 1)
      ok = false;
  }
  unsigned int digest_size = 0;
  ok = ok && EVP_DigestFinal_ex(context, output, &digest_size) == 1 &&
       digest_size == 32;
  EVP_MD_CTX_free(context);
  if (close(descriptor) != 0)
    ok = false;
  *size = total;
  return ok;
}

static void put_u32(unsigned char bytes[4], uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index)
    bytes[index] = static_cast<unsigned char>(value >> (8U * index));
}

static bool create_parent_with_entries(
    Fixture *fixture, const std::vector<Lardon3DMatchFileEntry> &entries,
    uint64_t *parent_id) {
  ++fixture->parent_sequence;
  char relative[64];
  char full[PATH_MAX];
  int length = std::snprintf(relative, sizeof(relative), "matches-%u.bin",
                             fixture->parent_sequence);
  if (length <= 0 || length >= static_cast<int>(sizeof(relative)) ||
      !join_path(full, fixture->root, relative))
    return false;
  const uint32_t count = static_cast<uint32_t>(entries.size());
  int descriptor = open(full, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return false;
  bool ok = lardon3d_match_file_write(
                descriptor, LARDON3D_FEATURE_DESCRIPTOR_U8, 32,
                fixture->set_a.feature_set_id, fixture->set_b.feature_set_id,
                entries.data(), count) == LARDON3D_MATCH_FILE_OK;
  if (close(descriptor) != 0)
    ok = false;
  unsigned char sha256[32];
  uint64_t size = 0;
  if (!ok || !sha256_file(full, sha256, &size))
    return false;
  unsigned char fingerprint[32];
  std::memset(fingerprint, static_cast<int>(fixture->parent_sequence),
              sizeof(fingerprint));
  Lardon3DProjectDbMatchResult parent;
  if (lardon3d_project_db_create_match_result(
          fixture->state.project_db, fixture->pair.candidate_pair_id,
          fixture->set_a.feature_set_id, fixture->set_b.feature_set_id,
          "fixture", 1, fingerprint, LARDON3D_MATCH_RESULT_STATUS_MATCHED,
          count, sha256, relative, size, fixture->parent_sequence,
          &parent) != LARDON3D_PROJECT_DB_OK)
    return false;
  *parent_id = parent.match_result_id;
  return true;
}

static bool create_parent(Fixture *fixture, uint32_t count,
                          uint64_t *parent_id) {
  std::vector<Lardon3DMatchFileEntry> entries(count);
  for (uint32_t index = 0; index < count; ++index) {
    entries[index].feature_index_a = index;
    entries[index].feature_index_b =
        count == 0 ? 0 : (index * (count == 7 ? 1U : 7U)) % count;
    entries[index].distance = static_cast<float>(index + 1U);
  }
  return create_parent_with_entries(fixture, entries, parent_id);
}

static std::string mask_bits(uint32_t count, uint32_t inliers,
                             uint32_t stride = 1) {
  std::string bits(count, '0');
  uint32_t position = 0;
  for (uint32_t selected = 0; selected < inliers; ++selected) {
    while (bits[position] == '1')
      position = (position + 1U) % count;
    bits[position] = '1';
    position = (position + stride) % count;
  }
  return bits;
}

static bool same_scientific_result(
    const Lardon3DProjectDbGeometricVerificationResult &left,
    const Lardon3DProjectDbGeometricVerificationResult &right) {
  return left.match_result_id == right.match_result_id &&
         left.verifier_kind == right.verifier_kind &&
         left.verifier_version == right.verifier_version &&
         std::memcmp(left.parameter_fingerprint, right.parameter_fingerprint,
                     sizeof(left.parameter_fingerprint)) == 0 &&
         left.status == right.status &&
         left.inlier_count == right.inlier_count &&
         left.inlier_mask_size == right.inlier_mask_size &&
         std::memcmp(left.inlier_mask, right.inlier_mask,
                     left.inlier_mask_size) == 0 &&
         left.has_model == right.has_model &&
         (!left.has_model ||
          std::memcmp(left.model, right.model, sizeof(left.model)) == 0);
}

static void test_parallel_prepare_and_owner_order(Fixture *fixture) {
  constexpr size_t parent_count = 16;
  std::vector<uint64_t> parent_ids(parent_count);
  for (size_t index = 0; index < parent_count; ++index)
    CHECK(create_parent(fixture, 32, &parent_ids[index]));
  const Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  std::vector<Lardon3DProjectDbGeometricVerificationResult> baseline;
  for (size_t width : {1U, 2U, 4U, 8U, 12U, 16U}) {
    char sql[256];
    int length = std::snprintf(
        sql, sizeof(sql),
        "DELETE FROM geometric_verification_results WHERE match_result_id "
        "BETWEEN %llu AND %llu",
        static_cast<unsigned long long>(parent_ids.front()),
        static_cast<unsigned long long>(parent_ids.back()));
    CHECK(length > 0 && length < static_cast<int>(sizeof(sql)) &&
          execute_sql(fixture->database_path, sql));
    std::vector<Lardon3DGeometricVerifierPrepared *> prepared(parent_count,
                                                               nullptr);
    std::vector<Lardon3DProjectDbGeometricVerificationResult> results(
        parent_count);
    std::vector<Lardon3DGeometricVerifierResult> statuses(
        parent_count, LARDON3D_GEOMETRIC_VERIFIER_INVALID_ARGUMENT);
    std::vector<unsigned char> reused(parent_count, 0);
    std::atomic<size_t> next{0};
    lardon3d_geometric_verifier_test_reset_estimator_calls();
    auto worker = [&]() {
      for (;;) {
        size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= parent_count)
          return;
        bool was_reused = false;
        statuses[index] =
            lardon3d_geometric_verifier_internal_prepare_version(
                fixture->root, fixture->state.project_db, parent_ids[index],
                &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
                &prepared[index], &results[index], &was_reused);
        reused[index] = was_reused ? 1U : 0U;
      }
    };
    std::vector<std::thread> children;
    for (size_t index = 1; index < width; ++index)
      children.emplace_back(worker);
    worker();
    for (std::thread &child : children)
      child.join();
    CHECK(lardon3d_geometric_verifier_test_estimator_calls() == parent_count);
    uint64_t previous_result_id = 0;
    for (size_t index = 0; index < parent_count; ++index) {
      CHECK(statuses[index] == LARDON3D_GEOMETRIC_VERIFIER_OK &&
            prepared[index] && reused[index] == 0);
      bool was_reused = false;
      statuses[index] =
          lardon3d_geometric_verifier_internal_publish_prepared(
              fixture->state.project_db, prepared[index], &results[index],
              &was_reused);
      lardon3d_geometric_verifier_internal_prepared_destroy(prepared[index]);
      prepared[index] = nullptr;
      CHECK(statuses[index] == LARDON3D_GEOMETRIC_VERIFIER_OK && !was_reused &&
            results[index].geometric_verification_result_id >
                previous_result_id);
      previous_result_id = results[index].geometric_verification_result_id;
    }
    if (baseline.empty()) {
      baseline = results;
    } else {
      for (size_t index = 0; index < parent_count; ++index)
        CHECK(same_scientific_result(baseline[index], results[index]));
    }
  }
}

static void test_parameters_and_fingerprint() {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  CHECK(lardon3d_geometric_verifier_parameters_valid(&parameters));
  unsigned char first[32], second[32], changed[32], v1[32], v2[32], v3[32];
  lardon3d_geometric_verifier_fingerprint(&parameters, first);
  lardon3d_geometric_verifier_fingerprint(&parameters, second);
  CHECK(std::memcmp(first, second, sizeof(first)) == 0);
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1, v1));
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2, v2));
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, v3));
  CHECK(std::memcmp(first, v1, sizeof(first)) != 0);
  auto differs = [&](Lardon3DGeometricVerifierParameters changed_parameters) {
    lardon3d_geometric_verifier_fingerprint(&changed_parameters, changed);
    return std::memcmp(first, changed, sizeof(first)) != 0;
  };
  parameters.threshold_pixels = 2.0;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.confidence = 0.99;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.max_iterations = 4000;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.min_inlier_count = 17;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.min_inlier_ratio = 0.21;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.seed_policy_version = 2;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.canonicalization_version = 2;
  CHECK(differs(parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  unsigned char default_bytes[LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE];
  unsigned char changed_bytes[LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE];
  CHECK(lardon3d_geometric_verifier_fingerprint_bytes(
      &parameters, LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC, 1, default_bytes));
  CHECK(lardon3d_geometric_verifier_fingerprint_bytes(
      &parameters, LARDON3D_GEOMETRIC_ALGORITHM_USAC_DEFAULT, 1,
      changed_bytes));
  CHECK(std::memcmp(default_bytes, changed_bytes, sizeof(default_bytes)) != 0);
  CHECK(lardon3d_geometric_verifier_fingerprint_bytes(
      &parameters, LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC, 2, changed_bytes));
  CHECK(std::memcmp(default_bytes, changed_bytes, sizeof(default_bytes)) != 0);
  CHECK(default_bytes[83] == 0);
  parameters.min_inlier_ratio = 0.0;
  CHECK(lardon3d_geometric_verifier_parameters_valid(&parameters));
  CHECK(lardon3d_geometric_verifier_fingerprint_bytes(
      &parameters, LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC, 1, default_bytes));
  parameters.min_inlier_ratio = -0.0;
  CHECK(lardon3d_geometric_verifier_fingerprint_bytes(
      &parameters, LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC, 1, changed_bytes));
  CHECK(std::memcmp(default_bytes, changed_bytes, sizeof(default_bytes)) == 0);
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.confidence = 1.0;
  CHECK(!lardon3d_geometric_verifier_parameters_valid(&parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.threshold_pixels = std::numeric_limits<double>::quiet_NaN();
  CHECK(!lardon3d_geometric_verifier_parameters_valid(&parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.max_iterations = std::numeric_limits<uint32_t>::max();
  CHECK(!lardon3d_geometric_verifier_parameters_valid(&parameters));
  parameters = lardon3d_geometric_verifier_default_parameters();
  parameters.min_inlier_count = 8193;
  CHECK(!lardon3d_geometric_verifier_parameters_valid(&parameters));

  parameters = lardon3d_geometric_verifier_default_parameters();
  CHECK(lardon3d_geometric_verifier_fingerprint_bytes(
      &parameters, LARDON3D_GEOMETRIC_ALGORITHM_USAC_MAGSAC, 1, default_bytes));
  char encoded_hex[2 * LARDON3D_GEOMETRIC_VERIFIER_FINGERPRINT_SIZE + 1];
  char fingerprint_hex[65];
  to_hex(default_bytes, sizeof(default_bytes), encoded_hex);
  to_hex(v1, sizeof(v1), fingerprint_hex);
  CHECK(std::strcmp(
            encoded_hex,
            "4c3344475646503101000000010000000100000001000000000000000000f83f"
            "2b8716d9cef7ef3f88130000100000009a9999999999c93f0100000001000000"
            "0200020001050000000e00000001030300000000") == 0);
  CHECK(
      std::strcmp(
          fingerprint_hex,
          "ddb44bb070c62be66c405946e89cbb49c084f8f30a21d6f408dc239225b7bbd0") ==
      0);
  to_hex(v2, sizeof(v2), fingerprint_hex);
  CHECK(std::strcmp(
            fingerprint_hex,
            "7868a893437ee611a10008a093286997212fa8bd80b2afd2bb1d11f04f01c5ae") ==
        0);
  to_hex(v3, sizeof(v3), fingerprint_hex);
  CHECK(std::strcmp(
            fingerprint_hex,
            "6944a471d611d8ffc59dac7cf15a5b79b97e2371d4c51785c477d68c1577f74c") ==
        0);
  CHECK(std::memcmp(first, v3, sizeof(first)) == 0);
  CHECK(LARDON3D_GEOMETRIC_VERIFIER_VERSION == 3);
}

static void test_seed() {
  unsigned char match[32] = {};
  unsigned char fingerprint[32] = {};
  uint32_t first = lardon3d_geometric_verifier_seed(match, fingerprint);
  CHECK(first == lardon3d_geometric_verifier_seed(match, fingerprint));
  match[31] = 1;
  CHECK(first != lardon3d_geometric_verifier_seed(match, fingerprint));
  CHECK(lardon3d_geometric_verifier_seed(nullptr, fingerprint) == 0);
  CHECK(first == 1803944746U);
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  unsigned char production_fingerprint[32];
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1,
      production_fingerprint));
  CHECK(lardon3d_geometric_verifier_seed(match, production_fingerprint) ==
        1910542150U);
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2,
      production_fingerprint));
  CHECK(lardon3d_geometric_verifier_seed(match, production_fingerprint) ==
        1528046088U);
  CHECK(lardon3d_geometric_verifier_fingerprint_for_version(
      &parameters, LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3,
      production_fingerprint));
  CHECK(lardon3d_geometric_verifier_seed(match, production_fingerprint) ==
        188721673U);
}

static void test_canonicalization() {
  const double source[9] = {-2.0, 1.0, 0.0, 0.5, -0.25, 0.0, 0.0, 0.0, 0.0};
  double positive[9], negative[9], scaled[9], half[9];
  for (size_t index = 0; index < 9; ++index) {
    positive[index] = source[index];
    negative[index] = -source[index];
    scaled[index] = 2.0 * source[index];
    half[index] = 0.5 * source[index];
  }
  CHECK(lardon3d_geometric_verifier_canonicalize(positive));
  CHECK(lardon3d_geometric_verifier_canonicalize(negative));
  CHECK(lardon3d_geometric_verifier_canonicalize(scaled));
  CHECK(lardon3d_geometric_verifier_canonicalize(half));
  CHECK(std::memcmp(positive, negative, sizeof(positive)) == 0);
  for (size_t index = 0; index < 9; ++index)
    CHECK(positive[index] == scaled[index]);
  for (size_t index = 0; index < 9; ++index)
    CHECK(positive[index] == half[index]);
  CHECK(positive[0] > 0.0);
  double zero[9] = {};
  CHECK(!lardon3d_geometric_verifier_canonicalize(zero));
  double invalid[9] = {std::numeric_limits<double>::infinity()};
  CHECK(!lardon3d_geometric_verifier_canonicalize(invalid));
  invalid[0] = -std::numeric_limits<double>::infinity();
  CHECK(!lardon3d_geometric_verifier_canonicalize(invalid));
  invalid[0] = std::numeric_limits<double>::quiet_NaN();
  CHECK(!lardon3d_geometric_verifier_canonicalize(invalid));
  double tie[9] = {-1.0, 1.0};
  CHECK(lardon3d_geometric_verifier_canonicalize(tie));
  CHECK(tie[0] > 0.0 && tie[1] < 0.0);
  double tiny[9] = {std::numeric_limits<double>::denorm_min()};
  CHECK(lardon3d_geometric_verifier_canonicalize(tiny));
  CHECK(tiny[0] == 1.0);
}

static bool
mask_matches(const Lardon3DProjectDbGeometricVerificationResult &result,
             const std::string &bits) {
  if (result.inlier_mask_size != (bits.size() + 7U) / 8U)
    return false;
  for (size_t index = 0; index < bits.size(); ++index) {
    bool stored = (result.inlier_mask[index / 8U] & (1U << (index % 8U))) != 0;
    if (stored != (bits[index] == '1'))
      return false;
  }
  return true;
}

static bool
run_controlled(Fixture *fixture, uint64_t parent_id, const std::string &bits,
               const Lardon3DGeometricVerifierParameters *parameters,
               Lardon3DProjectDbGeometricVerificationResult *result,
               bool *reused) {
  if (setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "controlled", 1) != 0 ||
      setenv("LARDON3D_TEST_GEOMETRIC_MASK", bits.c_str(), 1) != 0)
    return false;
  Lardon3DGeometricVerifierResult status =
      lardon3d_geometric_verifier_verify_and_publish(
          fixture->root, fixture->state.project_db, parent_id, parameters,
          result, reused);
  (void)unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR");
  (void)unsetenv("LARDON3D_TEST_GEOMETRIC_MASK");
  return status == LARDON3D_GEOMETRIC_VERIFIER_OK;
}

static void test_e2e_and_reuse(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  uint64_t verified_parent = 0;
  CHECK(create_parent(fixture, 32, &verified_parent));
  std::string verified_bits = mask_bits(32, 20, 7);
  Lardon3DProjectDbGeometricVerificationResult verified;
  bool reused = false;
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  CHECK(run_controlled(fixture, verified_parent, verified_bits, &parameters,
                       &verified, &reused));
  CHECK(!reused && lardon3d_geometric_verifier_test_estimator_calls() == 1);
  CHECK(verified.status == LARDON3D_GEOMETRIC_VERIFIED && verified.has_model &&
        verified.inlier_count == 20 && mask_matches(verified, verified_bits));
  double norm = 0.0;
  for (double value : verified.model) {
    CHECK(std::isfinite(value));
    norm = std::hypot(norm, value);
  }
  CHECK(std::fabs(norm - 1.0) < 1e-15);
  Lardon3DProjectDbGeometricVerificationResult reused_verified;
  CHECK(run_controlled(fixture, verified_parent, verified_bits, &parameters,
                       &reused_verified, &reused));
  CHECK(reused && lardon3d_geometric_verifier_test_estimator_calls() == 1 &&
        reused_verified.geometric_verification_result_id ==
            verified.geometric_verification_result_id);

  uint64_t rejected_parent = 0;
  CHECK(create_parent(fixture, 32, &rejected_parent));
  std::string rejected_bits = mask_bits(32, 15, 5);
  Lardon3DProjectDbGeometricVerificationResult rejected;
  CHECK(run_controlled(fixture, rejected_parent, rejected_bits, &parameters,
                       &rejected, &reused));
  CHECK(!reused && rejected.status == LARDON3D_GEOMETRIC_REJECTED &&
        !rejected.has_model && rejected.inlier_count == 15 &&
        mask_matches(rejected, rejected_bits));
  uint32_t calls_after_rejected =
      lardon3d_geometric_verifier_test_estimator_calls();
  CHECK(run_controlled(fixture, rejected_parent, rejected_bits, &parameters,
                       &rejected, &reused));
  CHECK(reused && lardon3d_geometric_verifier_test_estimator_calls() ==
                      calls_after_rejected);

  parameters.threshold_pixels = 2.0;
  Lardon3DProjectDbGeometricVerificationResult different;
  CHECK(run_controlled(fixture, verified_parent, verified_bits, &parameters,
                       &different, &reused));
  CHECK(!reused && different.geometric_verification_result_id !=
                       verified.geometric_verification_result_id);
}

static void test_scientific_boundaries(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  uint64_t insufficient_parent = 0;
  CHECK(create_parent(fixture, 6, &insufficient_parent));
  Lardon3DProjectDbGeometricVerificationResult insufficient;
  bool reused = false;
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, insufficient_parent,
            &parameters, &insufficient,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(!reused && insufficient.status == LARDON3D_GEOMETRIC_REJECTED &&
        insufficient.inlier_count == 0 && !insufficient.has_model &&
        insufficient.inlier_mask_size == 1 &&
        insufficient.inlier_mask[0] == 0 &&
        lardon3d_geometric_verifier_test_estimator_calls() == 0);

  uint64_t empty_parent = 0;
  CHECK(create_parent(fixture, 16, &empty_parent));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "empty", 1) == 0);
  Lardon3DProjectDbGeometricVerificationResult empty;
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, empty_parent, &parameters,
            &empty, &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);
  CHECK(empty.status == LARDON3D_GEOMETRIC_REJECTED &&
        empty.inlier_count == 0 && !empty.has_model);

  for (uint32_t inliers : {15U, 16U, 17U}) {
    uint64_t parent = 0;
    CHECK(create_parent(fixture, 80, &parent));
    std::string bits = mask_bits(80, inliers, 9);
    Lardon3DProjectDbGeometricVerificationResult result;
    CHECK(run_controlled(fixture, parent, bits, &parameters, &result, &reused));
    CHECK(result.inlier_count == inliers && mask_matches(result, bits));
    CHECK(result.status == (inliers >= 16 ? LARDON3D_GEOMETRIC_VERIFIED
                                          : LARDON3D_GEOMETRIC_REJECTED));
  }
}

static void test_v2_observation_support(Fixture *fixture) {
  const uint32_t a_insufficient[] = {0, 1, 2, 3, 4, 5, 0, 1};
  const uint32_t b_sufficient[] = {0, 1, 2, 3, 4, 5, 6, 7};
  const uint32_t both_a[] = {0, 1, 2, 3, 4, 5};
  const uint32_t both_b[] = {0, 1, 2, 3, 4, 5};
  CHECK(!lardon3d_geometric_verifier_test_has_minimal_support(
      a_insufficient, b_sufficient, 8, 8, 8));
  CHECK(!lardon3d_geometric_verifier_test_has_minimal_support(
      both_a, both_b, 6, 8, 8));

  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  std::vector<Lardon3DMatchFileEntry> repeated_b(10);
  for (uint32_t index = 0; index < repeated_b.size(); ++index) {
    repeated_b[index] = {index, index % 3U, static_cast<float>(index)};
  }
  uint64_t parent = 0;
  CHECK(create_parent_with_entries(fixture, repeated_b, &parent));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "error", 1) == 0);
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  Lardon3DProjectDbGeometricVerificationResult v2;
  bool reused = false;
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, parent, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2, &v2,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(!reused && v2.verifier_version == 2 &&
        v2.status == LARDON3D_GEOMETRIC_REJECTED && v2.inlier_count == 0 &&
        !v2.has_model && v2.inlier_mask_size == 2 && v2.inlier_mask[0] == 0 &&
        v2.inlier_mask[1] == 0 &&
        lardon3d_geometric_verifier_test_estimator_calls() == 0);

  // Historical v1 deliberately uses raw row count. Publishing it on the same
  // parent proves that v2 did not reinterpret or overwrite the v1 identity.
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, parent, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1, &v2,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR);
  CHECK(lardon3d_geometric_verifier_test_estimator_calls() == 1);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);

  std::vector<Lardon3DMatchFileEntry> exact(7);
  for (uint32_t index = 0; index < exact.size(); ++index)
    exact[index] = {index, index, static_cast<float>(index)};
  CHECK(create_parent_with_entries(fixture, exact, &parent));
  parameters.min_inlier_count = 1;
  parameters.min_inlier_ratio = 0.0;
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "controlled", 1) == 0);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_MASK", "1111111", 1) == 0);
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, parent, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V2, &v2,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(!reused && v2.status == LARDON3D_GEOMETRIC_VERIFIED &&
        v2.inlier_count == 7 &&
        lardon3d_geometric_verifier_test_estimator_calls() == 1);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_MASK") == 0);
}

static void test_v3_acceptance_feasibility(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused = false;

  // V3's raw-count preflight is exactly the acceptance proof N<min_inliers.
  // The neighboring N==min_inliers case must remain estimator-eligible.
  uint64_t below = 0;
  uint64_t exact = 0;
  CHECK(create_parent(fixture, parameters.min_inlier_count - 1U, &below));
  CHECK(create_parent(fixture, parameters.min_inlier_count, &exact));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "error", 1) == 0);
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, below, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(!reused && result.status == LARDON3D_GEOMETRIC_REJECTED &&
        result.inlier_count == 0 && !result.has_model &&
        lardon3d_geometric_verifier_test_estimator_calls() == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, exact, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR);
  CHECK(lardon3d_geometric_verifier_test_estimator_calls() == 1);

  parameters.min_inlier_count = 9;
  CHECK(create_parent(fixture, 8, &below));
  CHECK(create_parent(fixture, 9, &exact));
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, below, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(lardon3d_geometric_verifier_test_estimator_calls() == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, exact, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR);
  CHECK(lardon3d_geometric_verifier_test_estimator_calls() == 1);

  parameters = lardon3d_geometric_verifier_default_parameters();
  std::vector<Lardon3DMatchFileEntry> repeated_b(20);
  for (uint32_t index = 0; index < repeated_b.size(); ++index)
    repeated_b[index] = {index, index % 3U, static_cast<float>(index)};
  CHECK(create_parent_with_entries(fixture, repeated_b, &below));
  lardon3d_geometric_verifier_test_reset_estimator_calls();
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, below, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(result.status == LARDON3D_GEOMETRIC_REJECTED &&
        lardon3d_geometric_verifier_test_estimator_calls() == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);

  // Acceptance impossibility never bypasses Match asset validation. This
  // deliberately corrupt N<min_inliers parent must fail, not publish REJECTED.
  uint64_t corrupt = 0;
  CHECK(create_parent(fixture, parameters.min_inlier_count - 1U, &corrupt));
  char corrupt_path[PATH_MAX];
  char corrupt_relative[64];
  CHECK(std::snprintf(corrupt_relative, sizeof(corrupt_relative),
                      "matches-%u.bin", fixture->parent_sequence) > 0);
  CHECK(join_path(corrupt_path, fixture->root, corrupt_relative));
  int corrupt_fd = open(corrupt_path, O_WRONLY | O_CLOEXEC);
  CHECK(corrupt_fd >= 0);
  const unsigned char changed = 0xff;
  CHECK(pwrite(corrupt_fd, &changed, 1, 0) == 1);
  CHECK(close(corrupt_fd) == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish_version(
            fixture->root, fixture->state.project_db, corrupt, &parameters,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_CORRUPT);
}

static void test_mask_boundaries(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  parameters.min_inlier_count = 1;
  parameters.min_inlier_ratio = 0.0;
  for (uint32_t count : {1U, 2U}) {
    uint64_t parent = 0;
    CHECK(create_parent(fixture, count, &parent));
    Lardon3DProjectDbGeometricVerificationResult result;
    bool reused = false;
    CHECK(lardon3d_geometric_verifier_verify_and_publish(
              fixture->root, fixture->state.project_db, parent, &parameters,
              &result, &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
    CHECK(result.status == LARDON3D_GEOMETRIC_REJECTED &&
          result.inlier_count == 0 && result.inlier_mask_size == 1 &&
          result.inlier_mask[0] == 0);
  }
  for (uint32_t count : {7U, 8U, 9U, 63U, 64U, 65U, 8191U, 8192U}) {
    uint64_t parent = 0;
    CHECK(create_parent(fixture, count, &parent));
    std::string bits = mask_bits(count, (count + 1U) / 2U, 11);
    Lardon3DProjectDbGeometricVerificationResult result;
    bool reused = false;
    CHECK(run_controlled(fixture, parent, bits, &parameters, &result, &reused));
    CHECK(!reused && result.status == LARDON3D_GEOMETRIC_VERIFIED &&
          result.inlier_count == (count + 1U) / 2U &&
          mask_matches(result, bits));
    unsigned int remainder = count % 8U;
    if (remainder != 0)
      CHECK((result.inlier_mask[result.inlier_mask_size - 1] &
             static_cast<unsigned char>(0xffU << remainder)) == 0);
  }
}

static void test_failures(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  for (const char *behavior :
       {"error", "malformed-mask", "malformed-model", "nan-model"}) {
    uint64_t parent = 0;
    CHECK(create_parent(fixture, 32, &parent));
    CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", behavior, 1) == 0);
    Lardon3DProjectDbGeometricVerificationResult result;
    bool reused = false;
    CHECK(lardon3d_geometric_verifier_verify_and_publish(
              fixture->root, fixture->state.project_db, parent, &parameters,
              &result, &reused) == LARDON3D_GEOMETRIC_VERIFIER_ESTIMATOR_ERROR);
    CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);
    unsigned char fingerprint[32];
    lardon3d_geometric_verifier_fingerprint(&parameters, fingerprint);
    CHECK(lardon3d_project_db_find_geometric_verification_result(
              fixture->state.project_db, parent,
              LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL,
              LARDON3D_GEOMETRIC_VERIFIER_VERSION, fingerprint,
              &result) == LARDON3D_PROJECT_DB_NOT_FOUND);
  }

  uint64_t publication_parent = 0;
  CHECK(create_parent(fixture, 32, &publication_parent));
  std::string bits = mask_bits(32, 20, 3);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_PUBLICATION_FAILURE", "1", 1) == 0);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "controlled", 1) == 0);
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_MASK", bits.c_str(), 1) == 0);
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused = false;
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, publication_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_DATABASE_ERROR);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_PUBLICATION_FAILURE") == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_MASK") == 0);
  unsigned char fingerprint[32];
  lardon3d_geometric_verifier_fingerprint(&parameters, fingerprint);
  CHECK(lardon3d_project_db_find_geometric_verification_result(
            fixture->state.project_db, publication_parent,
            LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL,
            LARDON3D_GEOMETRIC_VERIFIER_VERSION, fingerprint,
            &result) == LARDON3D_PROJECT_DB_NOT_FOUND);
  CHECK(run_controlled(fixture, publication_parent, bits, &parameters, &result,
                       &reused));
  CHECK(!reused && result.status == LARDON3D_GEOMETRIC_VERIFIED);

  uint64_t allocation_parent = 0;
  CHECK(create_parent(fixture, 32, &allocation_parent));
  CHECK(setenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR", "bad-alloc", 1) == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, allocation_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_OUT_OF_MEMORY);
  CHECK(unsetenv("LARDON3D_TEST_GEOMETRIC_ESTIMATOR") == 0);
}

static void test_parent_and_asset_failures(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused = false;
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, UINT64_C(999999),
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_NOT_FOUND);

  unsigned char fingerprint[32] = {};
  Lardon3DProjectDbMatchResult no_match;
  CHECK(lardon3d_project_db_create_match_result(
            fixture->state.project_db, fixture->pair.candidate_pair_id,
            fixture->set_a.feature_set_id, fixture->set_b.feature_set_id,
            "no-match", 1, fingerprint, LARDON3D_MATCH_RESULT_STATUS_NO_MATCH,
            0, nullptr, nullptr, 0, 1, &no_match) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, no_match.match_result_id,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_NOT_FOUND);

  uint64_t missing_asset_parent = 0;
  CHECK(create_parent(fixture, 32, &missing_asset_parent));
  Lardon3DProjectDbMatchResult parent;
  CHECK(lardon3d_project_db_load_match_result(fixture->state.project_db,
                                              missing_asset_parent, &parent) ==
        LARDON3D_PROJECT_DB_OK);
  char full[PATH_MAX];
  CHECK(join_path(full, fixture->root, parent.match_asset_path));
  CHECK(unlink(full) == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, missing_asset_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_CORRUPT);

  uint64_t truncated_parent = 0;
  CHECK(create_parent(fixture, 32, &truncated_parent));
  CHECK(lardon3d_project_db_load_match_result(fixture->state.project_db,
                                              truncated_parent, &parent) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(join_path(full, fixture->root, parent.match_asset_path));
  CHECK(truncate(full, LARDON3D_MATCH_FILE_HEADER_SIZE) == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, truncated_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_CORRUPT);

  uint64_t index_parent = 0;
  CHECK(create_parent(fixture, 32, &index_parent));
  CHECK(lardon3d_project_db_load_match_result(fixture->state.project_db,
                                              index_parent, &parent) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(join_path(full, fixture->root, parent.match_asset_path));
  int descriptor = open(full, O_WRONLY | O_CLOEXEC);
  CHECK(descriptor >= 0);
  unsigned char invalid_index[4];
  put_u32(invalid_index, LARDON3D_MATCH_FILE_MAX_MATCHES);
  CHECK(pwrite(descriptor, invalid_index, sizeof(invalid_index),
               LARDON3D_MATCH_FILE_HEADER_SIZE) ==
        static_cast<ssize_t>(sizeof(invalid_index)));
  CHECK(close(descriptor) == 0);
  unsigned char corrupt_sha[32];
  uint64_t corrupt_size = 0;
  CHECK(sha256_file(full, corrupt_sha, &corrupt_size));
  char corrupt_relative[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  (void)std::snprintf(corrupt_relative, sizeof(corrupt_relative), "%s",
                      parent.match_asset_path);
  uint32_t corrupt_count = parent.match_count;
  Lardon3DProjectDbMatchResult repaired;
  CHECK(lardon3d_project_db_repair_match_result(
            fixture->state.project_db, index_parent,
            LARDON3D_MATCH_RESULT_STATUS_MATCHED, corrupt_count, corrupt_sha,
            corrupt_relative, corrupt_size,
            &repaired) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, index_parent, &parameters,
            &result, &reused) == LARDON3D_GEOMETRIC_VERIFIER_CORRUPT);
}

static void test_real_estimator_and_reopen(Fixture *fixture) {
  uint64_t parent = 0;
  CHECK(create_parent(fixture, 8192, &parent));
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused = false;
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, parent, &parameters,
            &result, &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(!reused && result.status == LARDON3D_GEOMETRIC_VERIFIED &&
        result.has_model &&
        result.inlier_count >= parameters.min_inlier_count &&
        result.inlier_mask_size == 1024);
  Lardon3DProjectDbGeometricVerificationResult before_reopen = result;
  uint64_t result_id = result.geometric_verification_result_id;
  lardon3d_project_db_close(fixture->state.project_db);
  fixture->state.project_db = nullptr;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY];
  CHECK(lardon3d_project_db_open(fixture->database_path,
                                 &fixture->state.project_db,
                                 error) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, parent, &parameters,
            &result, &reused) == LARDON3D_GEOMETRIC_VERIFIER_OK);
  CHECK(reused && result.geometric_verification_result_id == result_id &&
        result.status == before_reopen.status &&
        result.inlier_count == before_reopen.inlier_count &&
        result.inlier_mask_size == before_reopen.inlier_mask_size &&
        std::memcmp(result.inlier_mask, before_reopen.inlier_mask,
                    before_reopen.inlier_mask_size) == 0 &&
        std::memcmp(result.model, before_reopen.model, sizeof(result.model)) ==
            0);
}

static void test_feature_failures(Fixture *fixture) {
  Lardon3DGeometricVerifierParameters parameters =
      lardon3d_geometric_verifier_default_parameters();
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused = false;

  uint64_t ownership_parent = 0;
  CHECK(create_parent(fixture, 32, &ownership_parent));
  char sql[256];
  int length = std::snprintf(
      sql, sizeof(sql),
      "UPDATE match_results SET feature_set_id_a=%llu WHERE "
      "match_result_id=%llu",
      static_cast<unsigned long long>(fixture->set_b.feature_set_id),
      static_cast<unsigned long long>(ownership_parent));
  CHECK(length > 0 && length < static_cast<int>(sizeof(sql)) &&
        execute_sql(fixture->database_path, sql));
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, ownership_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_CORRUPT);

  uint64_t missing_asset_parent = 0;
  CHECK(create_parent(fixture, 32, &missing_asset_parent));
  char feature_path[PATH_MAX];
  CHECK(join_path(feature_path, fixture->root, fixture->set_a.asset.path));
  CHECK(unlink(feature_path) == 0);
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, missing_asset_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_CORRUPT);

  uint64_t missing_set_parent = 0;
  CHECK(create_parent(fixture, 32, &missing_set_parent));
  length = std::snprintf(
      sql, sizeof(sql),
      "PRAGMA foreign_keys=OFF;DELETE FROM feature_sets "
      "WHERE feature_set_id=%llu;PRAGMA foreign_keys=ON",
      static_cast<unsigned long long>(fixture->set_a.feature_set_id));
  CHECK(length > 0 && length < static_cast<int>(sizeof(sql)) &&
        execute_sql(fixture->database_path, sql));
  CHECK(lardon3d_geometric_verifier_verify_and_publish(
            fixture->root, fixture->state.project_db, missing_set_parent,
            &parameters, &result,
            &reused) == LARDON3D_GEOMETRIC_VERIFIER_NOT_FOUND);
}

int main() {
  test_parameters_and_fingerprint();
  test_seed();
  test_canonicalization();
  Fixture fixture;
  CHECK(fixture_create(&fixture));
  if (fixture.state.project_db) {
    test_e2e_and_reuse(&fixture);
    test_scientific_boundaries(&fixture);
  test_v2_observation_support(&fixture);
  test_v3_acceptance_feasibility(&fixture);
    test_mask_boundaries(&fixture);
    test_failures(&fixture);
    test_parent_and_asset_failures(&fixture);
    test_real_estimator_and_reopen(&fixture);
    test_parallel_prepare_and_owner_order(&fixture);
    test_feature_failures(&fixture);
  }
  fixture_destroy(&fixture);
  if (failures == 0)
    std::puts("geometric verifier core unit tests: PASS");
  return failures == 0 ? 0 : 1;
}
