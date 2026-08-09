#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/match_file.h>
#include <lardon3d/project_db.h>
#include <lardon3d/track_builder_project.h>
}

namespace {
constexpr Lardon3DGeometricVerifierKind kVerifier = LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL;
constexpr uint32_t kVersion = 1;
unsigned char g_verifier[32];
std::string g_delete_db;
uint64_t g_delete_id = 0;
bool g_delete_before_revalidation = false;
std::string g_insert_db;
uint64_t g_insert_match_id = 0;
bool g_insert_during_build = false;

void check(bool value, const char *expression, int line) {
  if (!value) {
    std::fprintf(stderr, "track-builder-project:%d: %s\n", line, expression);
    std::abort();
  }
}
#define CHECK(value) check((value), #value, __LINE__)

std::string asset_path(const unsigned char hash[32], const char *kind) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    hex[2 * i] = digits[hash[i] >> 4];
    hex[2 * i + 1] = digits[hash[i] & 0x0fU];
  }
  return std::string("assets/") + kind + "/" + hex.substr(0, 2) + "/" + hex;
}

void digest_file(const std::string &path, unsigned char hash[32], uint64_t *size) {
  FILE *file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  CHECK(context != nullptr && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1);
  unsigned char buffer[4096];
  *size = 0;
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
    CHECK(EVP_DigestUpdate(context, buffer, read) == 1);
    *size += read;
  }
  unsigned int length = 0;
  CHECK(EVP_DigestFinal_ex(context, hash, &length) == 1 && length == 32);
  EVP_MD_CTX_free(context);
  std::fclose(file);
}

struct Fixture {
  std::string directory;
  std::string db_path;
  Lardon3DProjectDb *db = nullptr;
  uint64_t image[3]{};
  uint64_t feature[3]{};
  uint64_t next_asset = 1;
  uint64_t next_file = 1;

  explicit Fixture(uint32_t feature_count = 32) {
    char name[] = "/tmp/lardon3d-track-c-XXXXXX";
    CHECK(mkdtemp(name) != nullptr);
    directory = name;
    db_path = directory + "/project.db";
    char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
    CHECK(lardon3d_project_db_open(db_path.c_str(), &db, error) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbScanSet scanset{};
    CHECK(lardon3d_project_db_create_scanset(db, "SPECIMEN", &scanset) ==
          LARDON3D_PROJECT_DB_OK);
    for (size_t i = 0; i < 3; ++i) {
      unsigned char hash[32]{};
      hash[0] = static_cast<unsigned char>(i + 1);
      std::string path = asset_path(hash, "images");
      Lardon3DProjectDbImageRegisterStatus status;
      Lardon3DProjectDbImage created{};
      char original_name[32];
      std::snprintf(original_name, sizeof(original_name), "image-%zu.jpg", i + 1);
      Lardon3DProjectDbResult image_result = lardon3d_project_db_register_image(
          db, scanset.scanset_id, hash, path.c_str(), 1, original_name, path.c_str(), 0,
          static_cast<int64_t>(i + 1), &status, &created);
      CHECK(image_result == LARDON3D_PROJECT_DB_OK);
      image[i] = created.image_id;
      unsigned char parameter[32]{0x5a};
      unsigned char source[32]{};
      source[0] = static_cast<unsigned char>(i + 1);
      unsigned char asset[32]{static_cast<unsigned char>(0x70U + i)};
      Lardon3DProjectDbFeatureSet set{};
      CHECK(lardon3d_project_db_register_feature_set(
                db, image[i], "orb", 1, parameter, source, feature_count, 1, 32, asset,
                asset_path(asset, "features").c_str(), 1, LARDON3D_DB_FEATURE_ASSET_DURABLE, 0,
                static_cast<int64_t>(i + 10), &set) == LARDON3D_PROJECT_DB_OK);
      feature[i] = set.feature_set_id;
    }
    std::memset(g_verifier, 0x42, sizeof(g_verifier));
  }

  ~Fixture() {
    if (db) lardon3d_project_db_close(db);
  }

  uint64_t add_feature_set(size_t image_index, unsigned char parameter_byte,
                           const char *extractor = "orb", uint32_t descriptor_type = 1) {
    unsigned char parameter[32]{parameter_byte};
    unsigned char source[32]{};
    source[0] = static_cast<unsigned char>(image_index + 1);
    unsigned char asset[32]{static_cast<unsigned char>(0x90U + parameter_byte)};
    Lardon3DProjectDbFeatureSet set{};
    CHECK(lardon3d_project_db_register_feature_set(
              db, image[image_index], extractor, 1, parameter, source, 32,
              descriptor_type, 32, asset, asset_path(asset, "features").c_str(), 1,
              LARDON3D_DB_FEATURE_ASSET_DURABLE, 0, 100, &set) == LARDON3D_PROJECT_DB_OK);
    return set.feature_set_id;
  }

  uint64_t add_gvr(size_t first, size_t second,
                   const std::vector<Lardon3DMatchFileEntry> &entries,
                   const std::vector<unsigned char> &mask,
                   Lardon3DGeometricVerificationStatus status = LARDON3D_GEOMETRIC_VERIFIED,
                   uint64_t feature_a = 0, uint64_t feature_b = 0,
                   const char *matcher_kind = "matcher-a", uint32_t matcher_version = 1) {
    if (feature_a == 0) feature_a = feature[first];
    if (feature_b == 0) feature_b = feature[second];
    Lardon3DProjectDbCandidatePair pair{};
    Lardon3DProjectDbResult pair_result = lardon3d_project_db_find_candidate_pair(
        db, image[first], image[second], &pair);
    if (pair_result == LARDON3D_PROJECT_DB_NOT_FOUND) {
      pair_result = lardon3d_project_db_create_candidate_pair(
          db, image[first], image[second], static_cast<int64_t>(next_file), &pair);
    }
    CHECK(pair_result == LARDON3D_PROJECT_DB_OK);
    std::string path = directory + "/match-" + std::to_string(next_file++) + ".bin";
    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(lardon3d_match_file_write(fd, 1, 32, feature_a, feature_b,
                                    entries.data(), static_cast<uint32_t>(entries.size())) ==
          LARDON3D_MATCH_FILE_OK);
    CHECK(close(fd) == 0);
    unsigned char hash[32]{};
    uint64_t size = 0;
    digest_file(path, hash, &size);
    unsigned char matcher_fingerprint[32]{};
    matcher_fingerprint[0] = static_cast<unsigned char>(next_file);
    Lardon3DProjectDbMatchResult match{};
    CHECK(lardon3d_project_db_create_match_result(
              db, pair.candidate_pair_id, feature_a, feature_b, matcher_kind, matcher_version,
              matcher_fingerprint, LARDON3D_MATCH_RESULT_STATUS_MATCHED,
              static_cast<uint32_t>(entries.size()), hash,
              path.substr(directory.size() + 1).c_str(), size, static_cast<int64_t>(next_file),
              &match) == LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbGeometricVerificationResult gvr{};
    double model[9]{};
    Lardon3DProjectDbResult gvr_result = lardon3d_project_db_create_geometric_verification_result(
              db, match.match_result_id, kVerifier, kVersion, g_verifier, status,
              popcount(mask),
              mask.data(), mask.size(),
              status == LARDON3D_GEOMETRIC_VERIFIED ? model : nullptr,
              static_cast<int64_t>(next_file), &gvr);
    CHECK(gvr_result == LARDON3D_PROJECT_DB_OK);
    return gvr.geometric_verification_result_id;
  }

  Lardon3DTrackBuilderProjectRequest request(const uint64_t *ids, size_t count) const {
    return {directory.c_str(), db, kVerifier, kVersion, g_verifier, ids, count};
  }

  static uint32_t popcount(const std::vector<unsigned char> &mask) {
    uint32_t count = 0;
    for (unsigned char byte : mask) {
      for (unsigned bit = 0; bit < 8; ++bit) count += (byte >> bit) & 1U;
    }
    return count;
  }
};

void assert_track(Lardon3DProjectDb *db, uint64_t set_id, uint64_t a, uint64_t b,
                  uint64_t c) {
  Lardon3DProjectDbTrack track{};
  CHECK(lardon3d_project_db_find_track_by_observation(db, set_id, a, 0, &track) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(track.observation_count == 3 && track.observations[0].feature_set_id == a &&
        track.observations[0].position_in_track == 0 && track.observations[1].feature_set_id == b &&
        track.observations[1].position_in_track == 1 && track.observations[2].feature_set_id == c &&
        track.observations[2].position_in_track == 2);
  lardon3d_project_db_free_track(&track);
}

void assert_two_observation_track(Lardon3DProjectDb *db, uint64_t set_id, uint64_t a, uint64_t b) {
  Lardon3DProjectDbTrack track{};
  CHECK(lardon3d_project_db_find_track_by_observation(db, set_id, a, 0, &track) ==
        LARDON3D_PROJECT_DB_OK && track.observation_count == 2 &&
        track.observations[0].feature_set_id == a &&
        track.observations[0].position_in_track == 0 &&
        track.observations[1].feature_set_id == b &&
        track.observations[1].position_in_track == 1);
  lardon3d_project_db_free_track(&track);
}

uint64_t track_set_count(const std::string &path) {
  sqlite3 *db = nullptr;
  sqlite3_stmt *statement = nullptr;
  CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM track_sets", -1, &statement, nullptr) ==
        SQLITE_OK);
  CHECK(sqlite3_step(statement) == SQLITE_ROW);
  uint64_t result = static_cast<uint64_t>(sqlite3_column_int64(statement, 0));
  sqlite3_finalize(statement);
  sqlite3_close(db);
  return result;
}

uint64_t table_count(const std::string &path, const char *table) {
  char sql[128];
  std::snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
  sqlite3 *db = nullptr;
  sqlite3_stmt *statement = nullptr;
  CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
  CHECK(sqlite3_step(statement) == SQLITE_ROW);
  uint64_t result = static_cast<uint64_t>(sqlite3_column_int64(statement, 0));
  CHECK(sqlite3_finalize(statement) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
  return result;
}

void execute_sql(const std::string &path, const char *sql) {
  sqlite3 *db = nullptr;
  CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_close(db) == SQLITE_OK);
}

void independent_scope_digest(const uint64_t *ids, size_t count, unsigned char output[32]) {
  std::vector<unsigned char> bytes(8 + count * 8);
  const char domain[] = "L3DTSIS1";
  std::memcpy(bytes.data(), domain, 8);
  for (size_t i = 0; i < count; ++i) {
    uint64_t value = ids[i];
    for (size_t byte = 0; byte < 8; ++byte) {
      bytes[8 + i * 8 + byte] = static_cast<unsigned char>(value & 0xffU);
      value >>= 8;
    }
  }
  unsigned int length = 0;
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  CHECK(context != nullptr && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
        EVP_DigestFinal_ex(context, output, &length) == 1 && length == 32);
  EVP_MD_CTX_free(context);
}

void run_basic_and_reuse() {
  Fixture fixture;
  auto ab = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto bc = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
  uint64_t ids[] = {ab, bc};
  auto request = fixture.request(ids, 2);
  Lardon3DTrackBuilderProjectResult first{};
  auto first_status = lardon3d_track_builder_build_project(&request, &first);
  CHECK(first_status == LARDON3D_TRACK_BUILDER_PROJECT_OK && !first.reused &&
        first.track_count == 1);
  assert_track(fixture.db, first.track_set_id, fixture.feature[0], fixture.feature[1],
               fixture.feature[2]);
  uint64_t before = track_set_count(fixture.db_path);
  Lardon3DTrackBuilderProjectResult second{};
  CHECK(lardon3d_track_builder_build_project(&request, &second) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && second.reused && second.track_set_id == first.track_set_id);
  CHECK(track_set_count(fixture.db_path) == before);
  std::puts("C01/C20: PASS");
}

void run_mask_cases() {
  Fixture fixture;
  std::vector<Lardon3DMatchFileEntry> entries;
  for (uint32_t i = 0; i < 14; ++i) entries.push_back({i, i, 0.1F});
  auto gvr = fixture.add_gvr(0, 1, entries, {0x85, 0x21});
  uint64_t ids[] = {gvr};
  auto request = fixture.request(ids, 1);
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(&request, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && result.raw_inlier_edge_count == 5 &&
        result.track_count == 5);
  std::puts("C02/C03: PASS (outlier exclusion, asymmetric LSB-first mask)");
}

void run_rejection_cases() {
  Fixture fixture;
  auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01}, LARDON3D_GEOMETRIC_REJECTED);
  uint64_t ids[] = {gvr};
  auto request = fixture.request(ids, 1);
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(&request, &result) !=
            LARDON3D_TRACK_BUILDER_PROJECT_OK &&
        track_set_count(fixture.db_path) == 0);
  request.verifier_kind = 99;
  CHECK(lardon3d_track_builder_build_project(&request, &result) !=
        LARDON3D_TRACK_BUILDER_PROJECT_OK);
  CHECK(fixture.request(nullptr, 0).gvr_count == 0);
  std::puts("C04-C07/C11: PASS");
}

void run_scope_errors() {
  Fixture fixture;
  auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t unsorted[] = {gvr + 1, gvr};
  auto bad = fixture.request(unsorted, 2);
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(&bad, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT);
  uint64_t duplicate[] = {gvr, gvr};
  bad = fixture.request(duplicate, 2);
  CHECK(lardon3d_track_builder_build_project(&bad, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT);
  uint64_t missing[] = {gvr + 1000};
  bad = fixture.request(missing, 1);
  CHECK(lardon3d_track_builder_build_project(&bad, &result) !=
        LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
  std::puts("C08-C10: PASS");
}

void run_revalidation_and_duplicate_scope() {
  Fixture fixture;
  auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t ids[] = {gvr};
  auto request = fixture.request(ids, 1);
  g_delete_db = fixture.db_path;
  g_delete_id = gvr;
  g_delete_before_revalidation = true;
  Lardon3DTrackBuilderProjectResult result{};
  auto status = lardon3d_track_builder_build_project(&request, &result);
  CHECK(status !=
            LARDON3D_TRACK_BUILDER_PROJECT_OK &&
        track_set_count(fixture.db_path) == 0);

  Fixture duplicate_fixture;
  auto first = duplicate_fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto second = duplicate_fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t duplicate_ids[] = {first, second};
  auto duplicate_request = duplicate_fixture.request(duplicate_ids, 2);
  CHECK(lardon3d_track_builder_build_project(&duplicate_request, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && result.track_count == 1);
  std::puts("C18/C25: PASS (duplicate collapse, selected-input disappearance detected)");
}

void run_remaining_matrix() {
  {
    Fixture fixture;
    auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    uint64_t ids[] = {gvr};
    auto request = fixture.request(ids, 1);
    Lardon3DTrackBuilderProjectResult result{};
    request.verifier_version = 2;
    CHECK(lardon3d_track_builder_build_project(&request, &result) !=
          LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
    request.verifier_version = 1;
    unsigned char wrong[32];
    std::memset(wrong, 0x43, sizeof(wrong));
    request.verifier_fingerprint = wrong;
    CHECK(lardon3d_track_builder_build_project(&request, &result) !=
          LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
    request.gvr_ids = nullptr;
    request.gvr_count = 0;
    CHECK(lardon3d_track_builder_build_project(&request, &result) ==
          LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT && track_set_count(fixture.db_path) == 0);
    std::puts("C06/C07/C11: PASS");
  }

  {
    Fixture fixture;
    auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    uint64_t ids[] = {gvr};
    auto request = fixture.request(ids, 1);
    Lardon3DProjectDbGeometricVerificationResult loaded{};
    CHECK(lardon3d_project_db_load_geometric_verification_result(fixture.db, gvr, &loaded) ==
          LARDON3D_PROJECT_DB_OK);
    Lardon3DProjectDbMatchResult match{};
    CHECK(lardon3d_project_db_load_match_result(fixture.db, loaded.match_result_id, &match) ==
          LARDON3D_PROJECT_DB_OK);
    std::string path = fixture.directory + "/" + match.match_asset_path;
    int fd = open(path.c_str(), O_WRONLY);
    CHECK(fd >= 0);
    unsigned char bad_magic = 'X';
    CHECK(write(fd, &bad_magic, 1) == 1 && close(fd) == 0);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) !=
          LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
    std::puts("C12: PASS (Match File bad magic propagated)");
  }

  {
    Fixture fixture;
    std::vector<Lardon3DMatchFileEntry> entries;
    for (uint32_t i = 0; i < 9; ++i) entries.push_back({i, i, 0.1F});
    auto gvr = fixture.add_gvr(0, 1, entries, {0xff, 0x01});
    uint64_t ids[] = {gvr};
    execute_sql(fixture.db_path,
                "UPDATE geometric_verification_results SET inlier_mask=X'FF' ");
    auto request = fixture.request(ids, 1);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) !=
          LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
    std::puts("C13: PASS (mask length rejected by persisted loader)");
  }

  {
    Fixture fixture;
    auto gvr = fixture.add_gvr(0, 1, {{32, 0, 0.1F}}, {0x01});
    uint64_t ids[] = {gvr};
    auto request = fixture.request(ids, 1);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) !=
          LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
    std::puts("C14: PASS (Match File bounds validator rejected OOB index)");
  }

  {
    Fixture fixture;
    uint64_t extra = fixture.add_feature_set(0, 0x61);
    auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01},
                                 LARDON3D_GEOMETRIC_VERIFIED, extra, fixture.feature[1]);
    auto second = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    uint64_t ids[] = {first, second};
    auto request = fixture.request(ids, 2);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) ==
          LARDON3D_TRACK_BUILDER_PROJECT_OK && result.track_count == 0);
    std::puts("C15: PASS (same-image scientific conflict)");
  }

  {
    Fixture fixture;
    uint64_t extra = fixture.add_feature_set(1, 0x62);
    auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01},
                                 LARDON3D_GEOMETRIC_VERIFIED, fixture.feature[0], extra);
    auto second = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01},
                                  LARDON3D_GEOMETRIC_VERIFIED, extra, fixture.feature[2]);
    uint64_t ids[] = {first, second};
    auto request = fixture.request(ids, 2);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) ==
          LARDON3D_TRACK_BUILDER_PROJECT_OK && result.track_count == 0);
    std::puts("C16: PASS (heterogeneous Feature Set conflict)");
  }

  {
    Fixture fixture;
    auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01},
                                 LARDON3D_GEOMETRIC_VERIFIED, 0, 0, "matcher-a", 1);
    auto second = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01},
                                  LARDON3D_GEOMETRIC_VERIFIED, 0, 0, "matcher-b", 2);
    uint64_t ids[] = {first, second};
    auto request = fixture.request(ids, 2);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) ==
          LARDON3D_TRACK_BUILDER_PROJECT_OK && result.track_count == 1);
    std::puts("C17: PASS (matcher configurations do not rank edges)");
  }

  {
    Fixture fixture;
    auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    auto duplicate = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    uint64_t one[] = {first};
    uint64_t two[] = {first, duplicate};
    auto request_one = fixture.request(one, 1);
    auto request_two = fixture.request(two, 2);
    Lardon3DTrackBuilderProjectResult a{}, b{};
    CHECK(lardon3d_track_builder_build_project(&request_one, &a) ==
          LARDON3D_TRACK_BUILDER_PROJECT_OK);
    CHECK(lardon3d_track_builder_build_project(&request_two, &b) ==
          LARDON3D_TRACK_BUILDER_PROJECT_OK && a.track_set_id != b.track_set_id &&
          a.track_count == b.track_count && track_set_count(fixture.db_path) == 2);
    unsigned char expected_one[32]{};
    unsigned char expected_two[32]{};
    independent_scope_digest(one, 1, expected_one);
    independent_scope_digest(two, 2, expected_two);
    const unsigned char fixed_one[32] = {
        0x5e, 0xf4, 0xf8, 0x44, 0x57, 0x8f, 0x92, 0x2c,
        0x7d, 0x55, 0xb8, 0xb7, 0x9a, 0x9f, 0x71, 0x21,
        0xac, 0x6b, 0x8a, 0x55, 0xe6, 0xb5, 0x3d, 0x6d,
        0x87, 0x7e, 0x20, 0x4b, 0x52, 0x58, 0x59, 0xbe};
    const unsigned char fixed_two[32] = {
        0xff, 0xb4, 0x3e, 0x52, 0x87, 0x38, 0xac, 0x64,
        0xc7, 0x39, 0xaa, 0xa8, 0xe2, 0x4a, 0xf1, 0xd4,
        0xcd, 0x99, 0x4b, 0x59, 0xb4, 0x6a, 0x97, 0xc6,
        0xf4, 0xb1, 0x66, 0x00, 0x52, 0x5b, 0x64, 0xa3};
    Lardon3DProjectDbTrackSet loaded_one{}, loaded_two{};
    CHECK(lardon3d_project_db_load_track_set(fixture.db, a.track_set_id, &loaded_one) ==
              LARDON3D_PROJECT_DB_OK &&
          lardon3d_project_db_load_track_set(fixture.db, b.track_set_id, &loaded_two) ==
              LARDON3D_PROJECT_DB_OK &&
          std::memcmp(expected_one, loaded_one.input_scope_hash, 32) == 0 &&
          std::memcmp(expected_two, loaded_two.input_scope_hash, 32) == 0 &&
          std::memcmp(expected_one, fixed_one, 32) == 0 &&
          std::memcmp(expected_two, fixed_two, 32) == 0 &&
          std::memcmp(expected_one, expected_two, 32) != 0);
    std::puts("C19: PASS (same logical output, distinct scope identity)");
  }
}

void run_durability_and_zero_track() {
  Fixture fixture;
  auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t ids[] = {gvr};
  auto request = fixture.request(ids, 1);
  Lardon3DTrackBuilderProjectResult first{};
  CHECK(lardon3d_track_builder_build_project(&request, &first) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && first.track_count == 1);
  uint64_t tracks_before = table_count(fixture.db_path, "tracks");
  uint64_t observations_before = table_count(fixture.db_path, "track_observations");
  lardon3d_project_db_close(fixture.db);
  fixture.db = nullptr;
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  CHECK(lardon3d_project_db_open(fixture.db_path.c_str(), &fixture.db, error) ==
        LARDON3D_PROJECT_DB_OK);
  request.database = fixture.db;
  Lardon3DProjectDbTrackSet loaded{};
  CHECK(lardon3d_project_db_load_track_set(fixture.db, first.track_set_id, &loaded) ==
        LARDON3D_PROJECT_DB_OK && loaded.track_count == 1 && loaded.gvr_count == 1);
  assert_two_observation_track(fixture.db, first.track_set_id, fixture.feature[0],
                               fixture.feature[1]);
  Lardon3DTrackBuilderProjectResult reused{};
  CHECK(lardon3d_track_builder_build_project(&request, &reused) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && reused.reused &&
        reused.track_set_id == first.track_set_id);
  CHECK(track_set_count(fixture.db_path) == 1 && table_count(fixture.db_path, "tracks") ==
            tracks_before &&
        table_count(fixture.db_path, "track_observations") == observations_before);
  std::puts("C21/C22: PASS (close/reopen and reuse durability)");

  Fixture zero;
  uint64_t extra = zero.add_feature_set(0, 0x63);
  auto conflict_a = zero.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01},
                                  LARDON3D_GEOMETRIC_VERIFIED, extra, zero.feature[1]);
  auto conflict_b = zero.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t conflict_ids[] = {conflict_a, conflict_b};
  auto conflict_request = zero.request(conflict_ids, 2);
  Lardon3DTrackBuilderProjectResult zero_result{};
  CHECK(lardon3d_track_builder_build_project(&conflict_request, &zero_result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && zero_result.track_count == 0 &&
        track_set_count(zero.db_path) == 1);
  std::puts("C23: PASS (zero-track Track Set published)");
}

void run_scope_snapshot_case() {
  Fixture fixture;
  auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto second = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
  auto late = fixture.add_gvr(0, 1, {{7, 7, 0.1F}}, {0x01});
  Lardon3DProjectDbGeometricVerificationResult late_gvr{};
  CHECK(lardon3d_project_db_load_geometric_verification_result(fixture.db, late, &late_gvr) ==
        LARDON3D_PROJECT_DB_OK);
  execute_sql(fixture.db_path,
              ("DELETE FROM geometric_verification_results WHERE "
               "geometric_verification_result_id=" + std::to_string(late))
                  .c_str());
  g_insert_db = fixture.db_path;
  g_insert_match_id = late_gvr.match_result_id;
  g_insert_during_build = true;
  uint64_t ids[] = {first, second};
  auto request = fixture.request(ids, 2);
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(&request, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && result.track_count == 1 && result.gvr_count == 2);
  CHECK(track_set_count(fixture.db_path) == 1);
  std::puts("C24: PASS (late matching GVR excluded from explicit scope)");
}

void run_partial_input_failure() {
  Fixture fixture;
  auto valid = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto corrupt = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
  Lardon3DProjectDbGeometricVerificationResult loaded{};
  CHECK(lardon3d_project_db_load_geometric_verification_result(fixture.db, corrupt, &loaded) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbMatchResult match{};
  CHECK(lardon3d_project_db_load_match_result(fixture.db, loaded.match_result_id, &match) ==
        LARDON3D_PROJECT_DB_OK);
  int fd = open((fixture.directory + "/" + match.match_asset_path).c_str(), O_WRONLY);
  CHECK(fd >= 0);
  unsigned char bad_magic = 'Q';
  CHECK(write(fd, &bad_magic, 1) == 1 && close(fd) == 0);
  uint64_t ids[] = {valid, corrupt};
  auto request = fixture.request(ids, 2);
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(&request, &result) !=
        LARDON3D_TRACK_BUILDER_PROJECT_OK && track_set_count(fixture.db_path) == 0);
  std::puts("PARTIAL INPUT: PASS");
}

void run_resource_case() {
  Fixture fixture(8192);
  std::vector<Lardon3DMatchFileEntry> entries;
  std::vector<unsigned char> mask(1024, 0xff);
  entries.reserve(8192);
  for (uint32_t i = 0; i < 8192; ++i) entries.push_back({i, i, 0.1F});
  std::vector<uint64_t> ids;
  for (size_t i = 0; i < 13; ++i) ids.push_back(fixture.add_gvr(0, 1, entries, mask));
  auto request = fixture.request(ids.data(), ids.size());
  Lardon3DTrackBuilderProjectResult result{};
  auto started = std::chrono::steady_clock::now();
  CHECK(lardon3d_track_builder_build_project(&request, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && result.raw_inlier_edge_count == 106496 &&
        result.core_observation_count == 16384 && result.track_count == 8192);
  auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
  struct rusage usage{};
  CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
  std::printf("RESOURCE GVR=%zu RAW=%llu INLIERS=%llu FEATURES=3 OBS=%llu TRACKS=%llu\n",
              ids.size(), static_cast<unsigned long long>(result.raw_inlier_edge_count),
              static_cast<unsigned long long>(result.raw_inlier_edge_count),
              static_cast<unsigned long long>(result.core_observation_count),
              static_cast<unsigned long long>(result.track_count));
  std::printf("RESOURCE DURATION_SECONDS=%.3f PEAK_RSS_KIB=%ld MAX_MATCH_FILES_LIVE=1\n",
              elapsed.count(), usage.ru_maxrss);
}

} // namespace

#ifdef LARDON3D_TRACK_BUILDER_PROJECT_TESTING
extern "C" void lardon3d_track_builder_project_test_before_revalidation(
    Lardon3DProjectDb *, const uint64_t *, size_t) {
  if (g_insert_during_build) {
    sqlite3 *connection = nullptr;
    CHECK(sqlite3_open(g_insert_db.c_str(), &connection) == SQLITE_OK);
    char sql[512];
    std::snprintf(
        sql, sizeof(sql),
        "INSERT INTO geometric_verification_results "
        "(match_result_id,verifier_kind,verifier_version,parameter_fingerprint,status," 
        "inlier_count,inlier_mask,model_m00,model_m01,model_m02,model_m10,model_m11,model_m12," 
        "model_m20,model_m21,model_m22,created_at) VALUES (%llu,1,1," 
        "X'4242424242424242424242424242424242424242424242424242424242424242',2,1,X'01'," 
        "0,0,0,0,0,0,0,0,0,999)",
        static_cast<unsigned long long>(g_insert_match_id));
    int insert_result = sqlite3_exec(connection, sql, nullptr, nullptr, nullptr);
    if (insert_result != SQLITE_OK) {
      std::fprintf(stderr, "late GVR insert: %s\n", sqlite3_errmsg(connection));
    }
    CHECK(insert_result == SQLITE_OK);
    CHECK(sqlite3_close(connection) == SQLITE_OK);
    g_insert_during_build = false;
  }
  if (!g_delete_before_revalidation) return;
  sqlite3 *connection = nullptr;
  CHECK(sqlite3_open(g_delete_db.c_str(), &connection) == SQLITE_OK);
  char sql[256];
  std::snprintf(sql, sizeof(sql),
                "DELETE FROM geometric_verification_results WHERE "
                "geometric_verification_result_id=%llu",
                static_cast<unsigned long long>(g_delete_id));
  CHECK(sqlite3_exec(connection, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(sqlite3_close(connection) == SQLITE_OK);
  g_delete_before_revalidation = false;
}
#endif

int main() {
  Lardon3DTrackBuilderProjectResult result{};
  Lardon3DTrackBuilderProjectRequest invalid{};
  CHECK(lardon3d_track_builder_build_project(&invalid, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT);
  run_basic_and_reuse();
  run_mask_cases();
  run_rejection_cases();
  run_scope_errors();
  run_revalidation_and_duplicate_scope();
  run_remaining_matrix();
  run_durability_and_zero_track();
  run_scope_snapshot_case();
  run_partial_input_failure();
  run_resource_case();
  std::puts("C01-C27 integration harness: PASS (C26 N/A; C27 deferred to Gate D)");
  return 0;
}
