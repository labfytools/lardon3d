#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <dirent.h>
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
#include <lardon3d/hardware_profile.h>
#include <lardon3d/resource_governor.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/track_builder_task.h>
#include <lardon3d/track_builder_project.h>
}

namespace lardon3d::track_builder_internal {
using Checkpoint = bool (*)(void *userdata);
Lardon3DTrackBuilderProjectStatus build_project(
    const Lardon3DTrackBuilderProjectRequest *request,
    Lardon3DTrackBuilderProjectResult *result, Checkpoint checkpoint,
    void *checkpoint_userdata);
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
bool g_publication_race = false;
uint64_t g_publication_nodes = 0;
uint64_t g_publication_capacity_bytes = 0;

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
    CHECK(mkdir((directory + "/.lardon3d").c_str(), 0700) == 0);
    CHECK(mkdir((directory + "/.lardon3d/checkpoints").c_str(), 0700) == 0);
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
    auto v1 = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    auto v2 = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
    char sql[512];
    std::snprintf(
        sql, sizeof(sql),
        "UPDATE geometric_verification_results SET verifier_version=2,"
        "parameter_fingerprint=X'4343434343434343434343434343434343434343434343434343434343434343' "
        "WHERE geometric_verification_result_id=%llu",
        static_cast<unsigned long long>(v2));
    execute_sql(fixture.db_path, sql);
    uint64_t mixed_ids[] = {v1, v2};
    auto request = fixture.request(mixed_ids, 2);
    Lardon3DTrackBuilderProjectResult result{};
    CHECK(lardon3d_track_builder_build_project(&request, &result) !=
              LARDON3D_TRACK_BUILDER_PROJECT_OK &&
          track_set_count(fixture.db_path) == 0);
    std::puts("GV-V1/V2 SELECTOR: PASS (mixed lineage rejected)");
  }

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

void run_resource_case(size_t gvr_count = 13) {
  Fixture fixture(8192);
  const bool profile = std::getenv("LARDON3D_RESOURCE_PROFILE") != nullptr;
  auto report_rss = [profile](const char *stage) {
    if (!profile) return;
    struct rusage usage{};
    CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
    std::printf("RESOURCE_STAGE=%s RSS_KIB=%ld\n", stage, usage.ru_maxrss);
  };
  report_rss("fixture-open");
  std::vector<Lardon3DMatchFileEntry> entries;
  std::vector<unsigned char> mask(1024, 0xff);
  entries.reserve(8192);
  for (uint32_t i = 0; i < 8192; ++i) entries.push_back({i, i, 0.1F});
  std::vector<uint64_t> ids;
  for (size_t i = 0; i < gvr_count; ++i) ids.push_back(fixture.add_gvr(0, 1, entries, mask));
  report_rss("fixture-built");
  auto request = fixture.request(ids.data(), ids.size());
  Lardon3DTrackBuilderProjectResult result{};
  auto started = std::chrono::steady_clock::now();
  report_rss("before-build");
  uint64_t expected_raw = static_cast<uint64_t>(gvr_count) * 8192ULL;
  const auto build_status = lardon3d_track_builder_build_project(&request, &result);
  if (build_status != LARDON3D_TRACK_BUILDER_PROJECT_OK ||
      result.raw_inlier_edge_count != expected_raw ||
      result.core_observation_count != 16384 || result.track_count != 8192)
    std::fprintf(stderr, "resource build status=%d raw=%llu nodes=%llu tracks=%llu\n",
                 static_cast<int>(build_status),
                 static_cast<unsigned long long>(result.raw_inlier_edge_count),
                 static_cast<unsigned long long>(result.core_observation_count),
                 static_cast<unsigned long long>(result.track_count));
  CHECK(build_status == LARDON3D_TRACK_BUILDER_PROJECT_OK &&
        result.raw_inlier_edge_count == expected_raw &&
        result.core_observation_count == 16384 && result.track_count == 8192);
  report_rss("after-build");
  auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
  struct rusage usage{};
  CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
  uint64_t predicted = 0;
  CHECK(lardon3d_track_builder_task_memory_estimate(
      result.raw_inlier_edge_count, 2, &predicted));
  std::printf("RESOURCE GVR=%zu RAW=%llu INLIERS=%llu FEATURES=3 OBS=%llu TRACKS=%llu "
              "PREDICTED_RESERVATION=%llu\n",
              ids.size(), static_cast<unsigned long long>(result.raw_inlier_edge_count),
              static_cast<unsigned long long>(result.raw_inlier_edge_count),
              static_cast<unsigned long long>(result.core_observation_count),
              static_cast<unsigned long long>(result.track_count),
              static_cast<unsigned long long>(predicted));
  std::printf("RESOURCE DURATION_SECONDS=%.3f PEAK_RSS_KIB=%ld MAX_MATCH_FILES_LIVE=1\n",
              elapsed.count(), usage.ru_maxrss);
}

void run_durable_task_case() {
  Fixture direct_fixture;
  auto direct_first = direct_fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto direct_second = direct_fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
  uint64_t direct_ids[] = {direct_first, direct_second};
  auto direct_request = direct_fixture.request(direct_ids, 2);
  Lardon3DTrackBuilderProjectResult direct_result{};
  CHECK(lardon3d_track_builder_build_project(&direct_request, &direct_result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && direct_result.track_count == 1);
  assert_track(direct_fixture.db, direct_result.track_set_id, direct_fixture.feature[0],
               direct_fixture.feature[1], direct_fixture.feature[2]);

  Fixture fixture;
  auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto second = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
  uint64_t ids[] = {first, second};
  Lardon3DHardwareProfile profile{};
  char hardware_error[128]{};
  CHECK(lardon3d_hardware_profile_detect(&profile, hardware_error, sizeof(hardware_error)));
  Lardon3DResourcePolicy policy{};
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 2);
  CHECK(queue != nullptr);
  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = fixture.db;
  state.resource_governor = governor;
  state.task_queue = queue;
  std::snprintf(state.project_path, sizeof(state.project_path), "%s", fixture.directory.c_str());
  Lardon3DTrackBuilderTaskConfiguration configuration = {
      state.project_path, fixture.db, kVerifier, kVersion, g_verifier, ids, 2};
  Lardon3DTaskKindDescriptor descriptor = {
      LARDON3D_TRACK_BUILDER_TASK_KIND, LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION,
      lardon3d_track_builder_task_reconstruct};
  Lardon3DTaskKindRegistry registry{};
  const Lardon3DTaskKindDescriptor *found_descriptor = nullptr;
  CHECK(lardon3d_task_kind_registry_init(&registry, &descriptor, 1) &&
        lardon3d_task_kind_registry_lookup(
            &registry, LARDON3D_TRACK_BUILDER_TASK_KIND,
            LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION, &found_descriptor) ==
            LARDON3D_TASK_KIND_OK && found_descriptor == &descriptor);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_track_builder_task(&state, &configuration, &task_id));
  Lardon3DTaskSnapshot snapshot{};
  for (size_t attempt = 0; attempt < 200; ++attempt) {
    CHECK(lardon3d_task_queue_get(queue, task_id, &snapshot));
    if (snapshot.state == TASK_COMPLETED || snapshot.state == TASK_FAILED ||
        snapshot.state == TASK_CANCELLED)
      break;
    usleep(10000);
  }
  CHECK(snapshot.state == TASK_COMPLETED && track_set_count(fixture.db_path) == 1);
  Lardon3DProjectDbTrackSet task_sets[1]{};
  size_t task_set_count_value = 0;
  CHECK(lardon3d_project_db_list_track_sets(fixture.db, 0, task_sets, 1,
                                            &task_set_count_value) ==
            LARDON3D_PROJECT_DB_OK &&
        task_set_count_value == 1 && task_sets[0].track_count == direct_result.track_count);
  assert_track(fixture.db, task_sets[0].track_set_id, fixture.feature[0], fixture.feature[1],
               fixture.feature[2]);
  CHECK(lardon3d_task_queue_remove(queue, task_id));
  lardon3d_task_queue_destroy(queue);
  lardon3d_resource_governor_destroy(governor);
  Lardon3DProjectDbTrackBuilderTask durable{};
  CHECK(lardon3d_project_db_load_track_builder_task(fixture.db, task_id, &durable) ==
        LARDON3D_PROJECT_DB_OK && durable.gvr_count == 2 && durable.scope_format_version == 1);
  Lardon3DTaskDurableSnapshot durable_snapshot{};
  uint32_t checkpoint_version = 0;
  CHECK(lardon3d_task_checkpoint_load(
            (fixture.directory + "/.lardon3d/checkpoints/" + std::to_string(task_id) + ".chk")
                .c_str(),
            &durable_snapshot, &checkpoint_version) == LARDON3D_TASK_CHECKPOINT_OK &&
        checkpoint_version == 1 && durable_snapshot.id == task_id);
  Lardon3DHardwareProfile reopened_profile{};
  CHECK(lardon3d_hardware_profile_detect(&reopened_profile, hardware_error,
                                         sizeof(hardware_error)));
  CHECK(lardon3d_resource_policy_default(&reopened_profile, &policy));
  Lardon3DResourceGovernor *reopened_governor =
      lardon3d_resource_governor_create(&reopened_profile, &policy);
  CHECK(reopened_governor != nullptr);
  Lardon3DTaskReconstructionContext runtime = {
      fixture.directory.c_str(), fixture.db, reopened_governor, nullptr};
  Lardon3DTaskKindBinding binding{};
  CHECK(lardon3d_track_builder_task_reconstruct(&durable_snapshot, &runtime, &binding) &&
        binding.callback != nullptr && binding.userdata_destroy != nullptr);
  Lardon3DTask *restored = lardon3d_task_restore_typed(
      &durable_snapshot, LARDON3D_TRACK_BUILDER_TASK_KIND,
      LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION, binding.callback, binding.userdata,
      binding.userdata_destroy);
  CHECK(restored != nullptr);
  lardon3d_task_destroy(restored);
  lardon3d_resource_governor_destroy(reopened_governor);
  std::puts("GATE D TASK: PASS (durable enqueue, execution, publication)");
}

void run_late_identity_race_case() {
  Fixture fixture;
  auto gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t ids[] = {gvr};
  auto request = fixture.request(ids, 1);
  g_publication_race = true;
  Lardon3DTrackBuilderProjectResult result{};
  auto status = lardon3d_track_builder_build_project(&request, &result);
  std::fprintf(stderr, "C27 status=%d reused=%d sets=%llu\n", status, result.reused,
               static_cast<unsigned long long>(track_set_count(fixture.db_path)));
  CHECK(status ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && result.reused && result.track_count == 1 &&
        track_set_count(fixture.db_path) == 1);
  std::puts("C27: PASS (deterministic exact-identity collision/reuse)");
}

void run_crash_recovery_case() {
  Fixture fixture;
  auto first = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  auto second = fixture.add_gvr(1, 2, {{0, 0, 0.1F}}, {0x01});
  uint64_t ids[] = {first, second};
  Lardon3DHardwareProfile profile{};
  char error[128]{};
  CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
  Lardon3DResourcePolicy policy{};
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  Lardon3DTaskQueue *queue = lardon3d_task_queue_create(governor, 2);
  CHECK(queue != nullptr);
  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = fixture.db;
  state.resource_governor = governor;
  state.task_queue = queue;
  std::snprintf(state.project_path, sizeof(state.project_path), "%s", fixture.directory.c_str());
  Lardon3DTrackBuilderTaskConfiguration configuration = {
      state.project_path, fixture.db, kVerifier, kVersion, g_verifier, ids, 2};
  uint64_t task_id = 0;
  setenv("LARDON3D_TRACK_BUILDER_TEST_SKIP_FINISHED", "1", 1);
  CHECK(lardon3d_project_enqueue_track_builder_task(&state, &configuration, &task_id));
  Lardon3DTaskSnapshot snapshot{};
  for (size_t attempt = 0; attempt < 200; ++attempt) {
    CHECK(lardon3d_task_queue_get(queue, task_id, &snapshot));
    if (snapshot.state == TASK_COMPLETED) break;
    usleep(10000);
  }
  CHECK(snapshot.state == TASK_COMPLETED && track_set_count(fixture.db_path) == 1);
  CHECK(lardon3d_task_queue_remove(queue, task_id));
  lardon3d_task_queue_destroy(queue);
  lardon3d_resource_governor_destroy(governor);
  CHECK(unsetenv("LARDON3D_TRACK_BUILDER_TEST_SKIP_FINISHED") == 0);
  lardon3d_project_db_close(fixture.db);
  fixture.db = nullptr;
  CHECK(lardon3d_project_db_open(fixture.db_path.c_str(), &fixture.db, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DTaskDurableSnapshot durable_snapshot{};
  uint32_t version = 0;
  CHECK(lardon3d_task_checkpoint_load(
            (fixture.directory + "/.lardon3d/checkpoints/" + std::to_string(task_id) + ".chk")
                .c_str(),
            &durable_snapshot, &version) == LARDON3D_TASK_CHECKPOINT_OK &&
        durable_snapshot.id == task_id && durable_snapshot.saved_state == TASK_PENDING);
  CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  governor = lardon3d_resource_governor_create(&profile, &policy);
  queue = lardon3d_task_queue_create(governor, 2);
  CHECK(governor != nullptr && queue != nullptr);
  Lardon3DTaskReconstructionContext runtime = {
      fixture.directory.c_str(), fixture.db, governor, nullptr};
  Lardon3DTaskKindBinding binding{};
  CHECK(lardon3d_track_builder_task_reconstruct(&durable_snapshot, &runtime, &binding));
  Lardon3DTask *restored = lardon3d_task_restore_typed(
      &durable_snapshot, LARDON3D_TRACK_BUILDER_TASK_KIND,
      LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION, binding.callback, binding.userdata,
      binding.userdata_destroy);
  CHECK(restored != nullptr && lardon3d_task_queue_add(queue, restored, nullptr));
  for (size_t attempt = 0; attempt < 200; ++attempt) {
    CHECK(lardon3d_task_queue_get(queue, task_id, &snapshot));
    if (snapshot.state == TASK_COMPLETED) break;
    usleep(10000);
  }
  CHECK(snapshot.state == TASK_COMPLETED && track_set_count(fixture.db_path) == 1);
  CHECK(lardon3d_task_queue_remove(queue, task_id));
  lardon3d_task_queue_destroy(queue);
  lardon3d_resource_governor_destroy(governor);
  execute_sql(fixture.db_path,
              ("UPDATE track_builder_tasks SET scope_size_bytes=1 WHERE task_id=" +
               std::to_string(task_id))
                  .c_str());
  Lardon3DProjectDbTrackBuilderTask corrupt_payload{};
  CHECK(lardon3d_project_db_load_track_builder_task(fixture.db, task_id,
                                                     &corrupt_payload) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DTaskKindBinding corrupt_binding{};
  Lardon3DResourceGovernor *corrupt_governor =
      lardon3d_resource_governor_create(&profile, &policy);
  CHECK(corrupt_governor != nullptr);
  Lardon3DTaskReconstructionContext corrupt_runtime = {
      fixture.directory.c_str(), fixture.db, corrupt_governor, nullptr};
  CHECK(!lardon3d_track_builder_task_reconstruct(&durable_snapshot, &corrupt_runtime,
                                                 &corrupt_binding));
  lardon3d_resource_governor_destroy(corrupt_governor);
  std::puts("CRASH AFTER PUBLICATION: PASS (replay exact reuse, no duplicate set)");
}

void run_pause_cancel_case() {
  auto make_state = [](Fixture &fixture, Lardon3DResourceGovernor **governor,
                       Lardon3DTaskQueue **queue, Lardon3DAppState *state) {
    Lardon3DHardwareProfile profile{};
    char error[128]{};
    CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
    Lardon3DResourcePolicy policy{};
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    *governor = lardon3d_resource_governor_create(&profile, &policy);
    CHECK(*governor != nullptr);
    *queue = lardon3d_task_queue_create(*governor, 2);
    CHECK(*queue != nullptr);
    lardon3d_app_state_init(state);
    state->project_loaded = true;
    state->project_db = fixture.db;
    state->resource_governor = *governor;
    state->task_queue = *queue;
    std::snprintf(state->project_path, sizeof(state->project_path), "%s",
                  fixture.directory.c_str());
  };

  Fixture paused_fixture;
  auto paused_gvr = paused_fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t paused_ids[] = {paused_gvr};
  Lardon3DResourceGovernor *paused_governor = nullptr;
  Lardon3DTaskQueue *paused_queue = nullptr;
  Lardon3DAppState paused_state{};
  make_state(paused_fixture, &paused_governor, &paused_queue, &paused_state);
  Lardon3DTrackBuilderTaskConfiguration paused_configuration = {
      paused_state.project_path, paused_fixture.db, kVerifier, kVersion, g_verifier,
      paused_ids, 1};
  uint64_t paused_id = 0;
  Lardon3DTask *paused_task = lardon3d_project_create_track_builder_task(
      &paused_state, &paused_configuration, &paused_id);
  CHECK(paused_task != nullptr && lardon3d_task_pause(paused_task) &&
        lardon3d_task_queue_add(paused_queue, paused_task, nullptr));
  usleep(20000);
  Lardon3DTaskSnapshot paused_snapshot{};
  CHECK(lardon3d_task_queue_get(paused_queue, paused_id, &paused_snapshot) &&
        paused_snapshot.state == TASK_PAUSED && track_set_count(paused_fixture.db_path) == 0);
  CHECK(lardon3d_task_queue_resume(paused_queue, paused_id));
  for (size_t attempt = 0; attempt < 200; ++attempt) {
    CHECK(lardon3d_task_queue_get(paused_queue, paused_id, &paused_snapshot));
    if (paused_snapshot.state == TASK_COMPLETED) break;
    usleep(10000);
  }
  CHECK(paused_snapshot.state == TASK_COMPLETED && track_set_count(paused_fixture.db_path) == 1);
  CHECK(lardon3d_task_queue_remove(paused_queue, paused_id));
  lardon3d_task_queue_destroy(paused_queue);
  lardon3d_resource_governor_destroy(paused_governor);

  Fixture cancelled_fixture;
  auto cancelled_gvr = cancelled_fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  uint64_t cancelled_ids[] = {cancelled_gvr};
  Lardon3DResourceGovernor *cancelled_governor = nullptr;
  Lardon3DTaskQueue *cancelled_queue = nullptr;
  Lardon3DAppState cancelled_state{};
  make_state(cancelled_fixture, &cancelled_governor, &cancelled_queue, &cancelled_state);
  Lardon3DTrackBuilderTaskConfiguration cancelled_configuration = {
      cancelled_state.project_path, cancelled_fixture.db, kVerifier, kVersion, g_verifier,
      cancelled_ids, 1};
  uint64_t cancelled_id = 0;
  Lardon3DTask *cancelled_task = lardon3d_project_create_track_builder_task(
      &cancelled_state, &cancelled_configuration, &cancelled_id);
  CHECK(cancelled_task != nullptr);
  lardon3d_task_request_cancel(cancelled_task);
  CHECK(lardon3d_task_queue_add(cancelled_queue, cancelled_task, nullptr));
  Lardon3DTaskSnapshot cancelled_snapshot{};
  for (size_t attempt = 0; attempt < 200; ++attempt) {
    CHECK(lardon3d_task_queue_get(cancelled_queue, cancelled_id, &cancelled_snapshot));
    if (cancelled_snapshot.state == TASK_CANCELLED) break;
    usleep(10000);
  }
  CHECK(cancelled_snapshot.state == TASK_CANCELLED &&
        track_set_count(cancelled_fixture.db_path) == 0);
  CHECK(lardon3d_task_queue_remove(cancelled_queue, cancelled_id));
  lardon3d_task_queue_destroy(cancelled_queue);
  lardon3d_resource_governor_destroy(cancelled_governor);
  std::puts("PAUSE/CANCEL: PASS");
}

struct LateRefusal {
  size_t calls = 0;
  size_t refuse_at = 0;
};

bool refuse_checkpoint(void *userdata) {
  auto *state = static_cast<LateRefusal *>(userdata);
  ++state->calls;
  return state->calls != state->refuse_at;
}

void run_late_refusal_boundaries_case() {
  Fixture fixture;
  const uint64_t gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
  const uint64_t ids[] = {gvr};
  const auto request = fixture.request(ids, 1);

  for (size_t refusal : {2U, 3U}) {
    LateRefusal checkpoint{0, refusal};
    Lardon3DTrackBuilderProjectResult interrupted{};
    CHECK(lardon3d::track_builder_internal::build_project(
              &request, &interrupted, refuse_checkpoint, &checkpoint) ==
              LARDON3D_TRACK_BUILDER_PROJECT_INTERRUPTED &&
          checkpoint.calls == refusal && track_set_count(fixture.db_path) == 0 &&
          table_count(fixture.db_path, "tracks") == 0 &&
          table_count(fixture.db_path, "track_observations") == 0);
  }

  Lardon3DTrackBuilderProjectResult completed{};
  CHECK(lardon3d_track_builder_build_project(&request, &completed) ==
            LARDON3D_TRACK_BUILDER_PROJECT_OK &&
        !completed.reused && completed.track_count == 1 &&
        track_set_count(fixture.db_path) == 1 &&
        table_count(fixture.db_path, "tracks") == 1 &&
        table_count(fixture.db_path, "track_observations") == 2);
  assert_two_observation_track(fixture.db, completed.track_set_id,
                               fixture.feature[0], fixture.feature[1]);
  std::puts("LATE REFUSAL: PASS (post-DSU and pre-publication remain zero, retry exact)");
}

void run_s21_admission_estimate_case() {
  constexpr uint64_t edges = 6628174ULL;
  constexpr uint64_t capacity_after_canonical_reserve = 12750811136ULL;
  constexpr uint64_t historical_envelope = 19546898688ULL;
  uint64_t compact = 0;
  CHECK(lardon3d_track_builder_task_memory_estimate(edges, 0, &compact));
  CHECK(compact == 1932981756ULL && compact < capacity_after_canonical_reserve &&
        historical_envelope > capacity_after_canonical_reserve);
  CHECK(!lardon3d_track_builder_task_memory_estimate(
      static_cast<uint64_t>(UINT32_MAX) / 2U + 1U, 0, &compact));
  CHECK(!lardon3d_track_builder_task_memory_estimate(edges, UINT64_MAX, &compact));
  uint64_t with_match_peak = 0;
  CHECK(lardon3d_track_builder_task_memory_estimate_with_match_peak(
      edges, 0, LARDON3D_MATCH_FILE_MAX_MATCHES, &with_match_peak));
  CHECK(with_match_peak == compact +
        LARDON3D_MATCH_FILE_MAX_MATCHES * sizeof(Lardon3DMatchFileEntry));
  CHECK(!lardon3d_track_builder_task_memory_estimate_with_match_peak(
      0, 0, UINT64_MAX, &with_match_peak));
  uint64_t adversarial_compact = 0, adversarial_peak = 0;
  CHECK(lardon3d_track_builder_task_memory_estimate(1, 2, &adversarial_compact));
  CHECK(lardon3d_track_builder_task_memory_estimate_with_match_peak(
      1, 2, LARDON3D_MATCH_FILE_MAX_MATCHES, &adversarial_peak));
  CHECK(adversarial_peak == adversarial_compact +
        LARDON3D_MATCH_FILE_MAX_MATCHES * sizeof(Lardon3DMatchFileEntry));
  Lardon3DHardwareProfile profile{};
  char error[128]{};
  CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
  profile.memory_total_bytes = capacity_after_canonical_reserve;
  Lardon3DResourcePolicy policy{};
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  policy.system_memory_reserve_bytes = 0;
  policy.emergency_memory_floor_bytes = 0;
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  Lardon3DResourceSnapshot snapshot{};
  CHECK(clock_gettime(CLOCK_MONOTONIC, &snapshot.captured_at) == 0);
  snapshot.memory_available_bytes = capacity_after_canonical_reserve;
  snapshot.memory_free_bytes = capacity_after_canonical_reserve;
  Lardon3DResourceEstimate estimate = {
      compact, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DResourceDecision decision{};
  Lardon3DResourceReservation *reservation = nullptr;
  CHECK(lardon3d_resource_governor_reserve(
            governor, &snapshot, &estimate, &decision, &reservation) &&
        decision.kind == LARDON3D_RESOURCE_START && reservation != nullptr &&
        lardon3d_resource_governor_release(governor, reservation));
  lardon3d_resource_governor_destroy(governor);

  /* WHY/CONTRACT: matches can greatly exceed inliers. At the exact boundary
   * the legacy E/F subtotal would start, while truthful peak accounting must
   * make the sole Governor refuse admission. */
  profile.memory_total_bytes = adversarial_peak - 1U;
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  policy.system_memory_reserve_bytes = 0;
  policy.emergency_memory_floor_bytes = 0;
  governor = lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  snapshot.memory_available_bytes = adversarial_peak - 1U;
  snapshot.memory_free_bytes = adversarial_peak - 1U;
  estimate.memory_fixed_bytes = adversarial_peak;
  reservation = nullptr;
  CHECK(lardon3d_resource_governor_reserve(
            governor, &snapshot, &estimate, &decision, &reservation) &&
        decision.kind != LARDON3D_RESOURCE_START && reservation == nullptr);
  lardon3d_resource_governor_destroy(governor);
  std::puts("S21 ADMISSION: PASS (truthful checked envelope fits canonical reserve)");
}

void run_adversarial_match_peak_task_case() {
  Fixture fixture(LARDON3D_MATCH_FILE_MAX_MATCHES);
  std::vector<Lardon3DMatchFileEntry> entries(LARDON3D_MATCH_FILE_MAX_MATCHES);
  for (uint32_t i = 0; i < LARDON3D_MATCH_FILE_MAX_MATCHES; ++i)
    entries[i] = {i, i, 0.1F};
  std::vector<unsigned char> mask(LARDON3D_MATCH_FILE_MAX_MATCHES / 8U, 0);
  mask[0] = 0x01;
  const uint64_t gvr = fixture.add_gvr(0, 1, entries, mask);
  const uint64_t ids[] = {gvr};
  Lardon3DHardwareProfile profile{};
  char error[128]{};
  CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
  Lardon3DResourcePolicy policy{};
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = fixture.db;
  state.resource_governor = governor;
  std::snprintf(state.project_path, sizeof(state.project_path), "%s",
                fixture.directory.c_str());
  Lardon3DTrackBuilderTaskConfiguration configuration = {
      state.project_path, fixture.db, kVerifier, kVersion, g_verifier, ids, 1};
  uint64_t task_id = 0;
  Lardon3DTask *task = lardon3d_project_create_track_builder_task(
      &state, &configuration, &task_id);
  uint64_t expected = 0;
  Lardon3DResourceEstimate estimate{};
  CHECK(task != nullptr && task_id != 0 &&
        lardon3d_track_builder_task_memory_estimate_with_match_peak(
            1, 2, LARDON3D_MATCH_FILE_MAX_MATCHES, &expected) &&
        lardon3d_task_resource_estimate(task, &estimate) &&
        estimate.memory_fixed_bytes == expected);
  lardon3d_task_destroy(task);
  lardon3d_resource_governor_destroy(governor);
  std::puts("MATCH PEAK TASK: PASS (8192 matches, 1 inlier, exact creation estimate)");
}

void run_disjoint_publication_capacity_case() {
  constexpr uint32_t track_count = 256;
  Fixture fixture(track_count);
  std::vector<Lardon3DMatchFileEntry> entries(track_count);
  for (uint32_t i = 0; i < track_count; ++i) entries[i] = {i, i, 0.1F};
  std::vector<unsigned char> mask(track_count / 8U, 0xffU);
  const uint64_t gvr = fixture.add_gvr(0, 1, entries, mask);
  const uint64_t ids[] = {gvr};
  auto request = fixture.request(ids, 1);
  g_publication_nodes = 0;
  g_publication_capacity_bytes = 0;
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(&request, &result) ==
            LARDON3D_TRACK_BUILDER_PROJECT_OK &&
        result.track_count == track_count && result.core_observation_count == 2U * track_count);
  CHECK(g_publication_nodes == 2U * track_count &&
        g_publication_capacity_bytes == 44U * g_publication_nodes);
  uint64_t estimate = 0;
  CHECK(lardon3d_track_builder_task_memory_estimate(track_count, 2, &estimate));
  uint64_t required_slots = g_publication_nodes + g_publication_nodes / 2U + 1U;
  uint64_t slots = 8;
  while (slots < required_slots) slots *= 2U;
  const uint64_t legacy_undercharge = 4ULL * 1024ULL * 1024ULL +
      85ULL * g_publication_nodes + 8ULL * track_count + 16ULL * slots + 640ULL * 2U;
  CHECK(estimate == legacy_undercharge + 16ULL * g_publication_nodes);

  Lardon3DHardwareProfile profile{};
  char error[128]{};
  CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
  profile.memory_total_bytes = estimate - 1U;
  Lardon3DResourcePolicy policy{};
  CHECK(lardon3d_resource_policy_default(&profile, &policy));
  policy.system_memory_reserve_bytes = 0;
  policy.emergency_memory_floor_bytes = 0;
  Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  Lardon3DResourceSnapshot snapshot{};
  CHECK(clock_gettime(CLOCK_MONOTONIC, &snapshot.captured_at) == 0);
  snapshot.memory_available_bytes = estimate - 1U;
  snapshot.memory_free_bytes = estimate - 1U;
  Lardon3DResourceEstimate resource = {
      estimate, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DResourceDecision decision{};
  Lardon3DResourceReservation *reservation = nullptr;
  CHECK(lardon3d_resource_governor_reserve(
            governor, &snapshot, &resource, &decision, &reservation) &&
        decision.kind != LARDON3D_RESOURCE_START && reservation == nullptr);
  lardon3d_resource_governor_destroy(governor);
  std::puts("PUBLICATION CAPACITY: PASS (256 disjoint tracks, truthful 44 B/node peak)");
}

bool checkpoints_directory_empty(const Fixture &fixture) {
  DIR *directory = opendir((fixture.directory + "/.lardon3d/checkpoints").c_str());
  CHECK(directory != nullptr);
  bool empty = true;
  while (dirent *entry = readdir(directory))
    if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
      empty = false;
  CHECK(closedir(directory) == 0);
  return empty;
}

void run_creation_ownership_failures_case() {
  for (const char *fault : {"LARDON3D_TRACK_BUILDER_TEST_FAIL_SCOPE_DIR_FSYNC",
                            "LARDON3D_TRACK_BUILDER_TEST_FAIL_INITIAL_PERSIST"}) {
    Fixture fixture;
    const uint64_t gvr = fixture.add_gvr(0, 1, {{0, 0, 0.1F}}, {0x01});
    const uint64_t ids[] = {gvr};
    Lardon3DHardwareProfile profile{};
    char error[128]{};
    CHECK(lardon3d_hardware_profile_detect(&profile, error, sizeof(error)));
    Lardon3DResourcePolicy policy{};
    CHECK(lardon3d_resource_policy_default(&profile, &policy));
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    CHECK(governor != nullptr);
    Lardon3DAppState state{};
    lardon3d_app_state_init(&state);
    state.project_loaded = true;
    state.project_db = fixture.db;
    state.resource_governor = governor;
    std::snprintf(state.project_path, sizeof(state.project_path), "%s", fixture.directory.c_str());
    Lardon3DTrackBuilderTaskConfiguration configuration = {
        state.project_path, fixture.db, kVerifier, kVersion, g_verifier, ids, 1};
    uint64_t task_id = 99;
    CHECK(setenv(fault, "1", 1) == 0);
    CHECK(lardon3d_project_create_track_builder_task(&state, &configuration, &task_id) == nullptr);
    CHECK(unsetenv(fault) == 0 && task_id == 0 && checkpoints_directory_empty(fixture) &&
          table_count(fixture.db_path, "tasks") == 0 &&
          table_count(fixture.db_path, "track_builder_tasks") == 0);
    lardon3d_resource_governor_destroy(governor);
  }
  std::puts("CREATION OWNERSHIP: PASS (post-rename and post-checkpoint rollback clean)");
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

extern "C" void lardon3d_track_builder_project_test_before_publication(
    const Lardon3DTrackBuilderProjectRequest *request) {
  if (!g_publication_race) return;
  g_publication_race = false;
  Lardon3DTrackBuilderProjectResult result{};
  CHECK(lardon3d_track_builder_build_project(request, &result) ==
        LARDON3D_TRACK_BUILDER_PROJECT_OK && !result.reused);
}

extern "C" void lardon3d_track_builder_project_test_publication_capacities(
    uint64_t node_count, uint64_t serialization_capacity_bytes) {
  g_publication_nodes = node_count;
  g_publication_capacity_bytes = serialization_capacity_bytes;
}
#endif

int main() {
  if (std::getenv("LARDON3D_RESOURCE_CALIBRATION_ONLY")) {
    const char *scale = std::getenv("LARDON3D_RESOURCE_SCALE");
    if (scale) {
      run_resource_case(static_cast<size_t>(std::strtoull(scale, nullptr, 10)));
    } else {
      run_resource_case(13);
      run_resource_case(31);
      run_resource_case(62);
    }
    return 0;
  }
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
  run_s21_admission_estimate_case();
  run_adversarial_match_peak_task_case();
  run_disjoint_publication_capacity_case();
  run_creation_ownership_failures_case();
  run_durable_task_case();
  run_late_identity_race_case();
  run_pause_cancel_case();
  run_late_refusal_boundaries_case();
  run_crash_recovery_case();
  std::puts("C01-C27 integration harness: PASS (C26 N/A; C27 Gate D closed)");
  return 0;
}
