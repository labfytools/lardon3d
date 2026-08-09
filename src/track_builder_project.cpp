#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <new>
#include <openssl/evp.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <lardon3d/match_file.h>
#include <lardon3d/track_builder.h>
#include <lardon3d/track_builder_project.h>
}

namespace {
using Observation = Lardon3DTrackBuilderObservation;
using Edge = Lardon3DTrackBuilderEdge;

struct Key {
  uint64_t set;
  uint32_t index;
  bool operator==(const Key &other) const { return set == other.set && index == other.index; }
};

struct KeyHash {
  size_t operator()(const Key &key) const noexcept {
    return static_cast<size_t>(key.set ^ (static_cast<uint64_t>(key.index) * 0x9e3779b97f4a7c15ULL));
  }
};

struct FeatureCacheEntry {
  Lardon3DProjectDbFeatureSet value{};
};

Lardon3DTrackBuilderProjectStatus map_db(Lardon3DProjectDbResult value) {
  if (value == LARDON3D_PROJECT_DB_INVALID_ARGUMENT || value == LARDON3D_PROJECT_DB_CONSTRAINT)
    return LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT;
  if (value == LARDON3D_PROJECT_DB_NOT_FOUND)
    return LARDON3D_TRACK_BUILDER_PROJECT_NOT_FOUND;
  if (value == LARDON3D_PROJECT_DB_CORRUPT)
    return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  return LARDON3D_TRACK_BUILDER_PROJECT_DATABASE_ERROR;
}

bool join_path(char *output, size_t capacity, const char *root, const char *relative) {
  if (!output || !root || !relative || relative[0] == '\0') return false;
  int written = std::snprintf(output, capacity, "%s/%s", root, relative);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool scope_hash(const uint64_t *ids, size_t count, unsigned char output[32]) {
  if (count > (std::numeric_limits<size_t>::max() - 8U) / sizeof(uint64_t)) return false;
  std::vector<unsigned char> bytes(8U + count * sizeof(uint64_t));
  std::memcpy(bytes.data(), "L3DTSIS1", 8);
  for (size_t i = 0; i < count; ++i) {
    uint64_t value = ids[i];
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
      bytes[8U + i * sizeof(value) + byte] = static_cast<unsigned char>(value & 0xffU);
      value >>= 8U;
    }
  }
  unsigned int length = 0;
  return EVP_Digest(bytes.data(), bytes.size(), output, &length, EVP_sha256(), nullptr) == 1 &&
         length == 32;
}

bool selector_matches(const Lardon3DProjectDbGeometricVerificationResult &gvr,
                      const Lardon3DTrackBuilderProjectRequest &request) {
  return static_cast<int>(gvr.verifier_kind) == request.verifier_kind &&
         gvr.verifier_version == request.verifier_version &&
         std::memcmp(gvr.parameter_fingerprint, request.verifier_fingerprint, 32) == 0;
}

Lardon3DProjectDbResult validate_gvr_snapshot(
    const Lardon3DTrackBuilderProjectRequest &request, uint64_t gvr_id) {
  Lardon3DProjectDbGeometricVerificationResult gvr{};
  Lardon3DProjectDbResult db = lardon3d_project_db_load_geometric_verification_result(
      request.database, gvr_id, &gvr);
  if (db != LARDON3D_PROJECT_DB_OK) return db;
  if (gvr.status != LARDON3D_GEOMETRIC_VERIFIED || !selector_matches(gvr, request))
    return LARDON3D_PROJECT_DB_CORRUPT;
  Lardon3DProjectDbMatchResult match{};
  db = lardon3d_project_db_load_match_result(request.database, gvr.match_result_id, &match);
  if (db != LARDON3D_PROJECT_DB_OK || match.result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED ||
      !match.has_match_asset) return db == LARDON3D_PROJECT_DB_OK
                                     ? LARDON3D_PROJECT_DB_CORRUPT : db;
  Lardon3DProjectDbCandidatePair pair{};
  db = lardon3d_project_db_load_candidate_pair(request.database, match.candidate_pair_id, &pair);
  if (db != LARDON3D_PROJECT_DB_OK) return db;
  Lardon3DProjectDbFeatureSet set_a{};
  Lardon3DProjectDbFeatureSet set_b{};
  db = lardon3d_project_db_load_feature_set(request.database, match.feature_set_id_a, &set_a);
  if (db != LARDON3D_PROJECT_DB_OK) return db;
  db = lardon3d_project_db_load_feature_set(request.database, match.feature_set_id_b, &set_b);
  if (db != LARDON3D_PROJECT_DB_OK) return db;
  if (set_a.image_id != pair.image_id_a || set_b.image_id != pair.image_id_b ||
      match.feature_set_id_a == match.feature_set_id_b)
    return LARDON3D_PROJECT_DB_CORRUPT;
  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  if (!join_path(path, sizeof(path), request.project_path, match.match_asset_path))
    return LARDON3D_PROJECT_DB_CORRUPT;
  Lardon3DMatchFileHeader header{};
  if (lardon3d_match_file_validate_asset(path, match.match_asset_sha256,
                                         match.match_asset_size_bytes, &header,
                                         set_a.feature_set_id, set_b.feature_set_id,
                                         set_a.feature_count, set_b.feature_count) !=
      LARDON3D_MATCH_FILE_OK || header.match_count != match.match_count)
    return LARDON3D_PROJECT_DB_CORRUPT;
  return LARDON3D_PROJECT_DB_OK;
}

Lardon3DTrackBuilderProjectStatus resolve_gvr(
    const Lardon3DTrackBuilderProjectRequest &request, uint64_t gvr_id,
    std::unordered_map<uint64_t, FeatureCacheEntry> &cache,
    std::deque<Observation> &observations,
    std::unordered_map<Key, Observation *, KeyHash> &observation_map,
    std::vector<Edge> &edges) {
  Lardon3DProjectDbGeometricVerificationResult gvr{};
  Lardon3DProjectDbResult db = lardon3d_project_db_load_geometric_verification_result(
      request.database, gvr_id, &gvr);
  if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
  if (gvr.status != LARDON3D_GEOMETRIC_VERIFIED || !selector_matches(gvr, request))
    return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  Lardon3DProjectDbMatchResult match{};
  db = lardon3d_project_db_load_match_result(request.database, gvr.match_result_id, &match);
  if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
  if (match.result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED || !match.has_match_asset ||
      match.match_count == 0) return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  Lardon3DProjectDbCandidatePair pair{};
  db = lardon3d_project_db_load_candidate_pair(request.database, match.candidate_pair_id, &pair);
  if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
  auto load_set = [&](uint64_t id, Lardon3DProjectDbFeatureSet **out) {
    auto found = cache.find(id);
    if (found != cache.end()) {
      *out = &found->second.value;
      return LARDON3D_PROJECT_DB_OK;
    }
    FeatureCacheEntry entry{};
    Lardon3DProjectDbResult value = lardon3d_project_db_load_feature_set(
        request.database, id, &entry.value);
    if (value == LARDON3D_PROJECT_DB_OK) {
      auto inserted = cache.emplace(id, entry);
      *out = &inserted.first->second.value;
    }
    return value;
  };
  Lardon3DProjectDbFeatureSet *set_a = nullptr;
  Lardon3DProjectDbFeatureSet *set_b = nullptr;
  db = load_set(match.feature_set_id_a, &set_a);
  if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
  db = load_set(match.feature_set_id_b, &set_b);
  if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
  if (set_a->image_id != pair.image_id_a || set_b->image_id != pair.image_id_b ||
      match.feature_set_id_a == match.feature_set_id_b) return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;

  char path[LARDON3D_PROJECT_DB_PATH_CAPACITY];
  if (!join_path(path, sizeof(path), request.project_path, match.match_asset_path))
    return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  Lardon3DMatchFileHeader header{};
  if (lardon3d_match_file_validate_asset(path, match.match_asset_sha256,
                                         match.match_asset_size_bytes, &header,
                                         set_a->feature_set_id, set_b->feature_set_id,
                                         set_a->feature_count, set_b->feature_count) !=
      LARDON3D_MATCH_FILE_OK || header.match_count != match.match_count)
    return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  std::vector<Lardon3DMatchFileEntry> entries(match.match_count);
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  uint32_t count = 0;
  bool read_ok = fd >= 0 && lardon3d_match_file_read(
      fd, &header, entries.data(), entries.size(), &count, set_a->feature_set_id,
      set_b->feature_set_id, set_a->feature_count, set_b->feature_count) == LARDON3D_MATCH_FILE_OK;
  if (fd >= 0) (void)close(fd);
  if (!read_ok || count != match.match_count || gvr.inlier_mask_size != (count + 7U) / 8U)
    return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  size_t selected = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if ((gvr.inlier_mask[i / 8U] & static_cast<unsigned char>(1U << (i % 8U))) == 0) continue;
    ++selected;
    const auto &entry = entries[i];
    Key keys[2] = {{set_a->feature_set_id, entry.feature_index_a},
                   {set_b->feature_set_id, entry.feature_index_b}};
    Observation *resolved[2] = {};
    Lardon3DProjectDbFeatureSet *sets[2] = {set_a, set_b};
    for (size_t endpoint = 0; endpoint < 2; ++endpoint) {
      auto found = observation_map.find(keys[endpoint]);
      if (found == observation_map.end()) {
        Observation value{};
        value.feature_set_id = keys[endpoint].set;
        value.feature_index = keys[endpoint].index;
        value.image_id = sets[endpoint]->image_id;
        std::memcpy(value.extractor_kind, sets[endpoint]->extractor_kind,
                    sizeof(value.extractor_kind));
        value.extractor_version = sets[endpoint]->extractor_version;
        std::memcpy(value.parameter_fingerprint, sets[endpoint]->parameter_fingerprint, 32);
        value.descriptor_type = sets[endpoint]->descriptor_type;
        value.descriptor_dimension = sets[endpoint]->descriptor_dimension;
        observations.push_back(value);
        resolved[endpoint] = &observations.back();
        observation_map.emplace(keys[endpoint], resolved[endpoint]);
      } else {
        resolved[endpoint] = found->second;
      }
    }
    edges.push_back({resolved[0], resolved[1]});
  }
  if (selected != gvr.inlier_count) return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  return LARDON3D_TRACK_BUILDER_PROJECT_OK;
}

} // namespace

extern "C" Lardon3DTrackBuilderProjectStatus lardon3d_track_builder_build_project(
    const Lardon3DTrackBuilderProjectRequest *request,
    Lardon3DTrackBuilderProjectResult *result) {
  if (!request || !result || !request->project_path || !request->database ||
      !request->verifier_fingerprint || !request->gvr_ids || request->gvr_count == 0 ||
      request->verifier_kind <= 0 || request->verifier_version == 0) {
    return LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT;
  }
  *result = {};
  for (size_t i = 0; i < request->gvr_count; ++i) {
    if (request->gvr_ids[i] == 0 || (i != 0 && request->gvr_ids[i - 1] >= request->gvr_ids[i]))
      return LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT;
  }
  try {
    std::vector<uint64_t> owned_ids(request->gvr_ids,
                                    request->gvr_ids + request->gvr_count);
    Lardon3DTrackBuilderProjectRequest owned_request = *request;
    owned_request.gvr_ids = owned_ids.data();
    request = &owned_request;
    unsigned char input_hash[32];
    if (!scope_hash(request->gvr_ids, request->gvr_count, input_hash))
      return LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY;
    unsigned char builder_fingerprint[32];
    if (!lardon3d_track_builder_fingerprint(builder_fingerprint))
      return LARDON3D_TRACK_BUILDER_PROJECT_CORE_ERROR;
    Lardon3DProjectDbTrackSet identity{};
    std::snprintf(identity.builder_kind, sizeof(identity.builder_kind), "track_builder");
    identity.builder_version = LARDON3D_TRACK_BUILDER_VERSION;
    std::memcpy(identity.parameter_fingerprint, builder_fingerprint, 32);
    identity.verifier_kind = request->verifier_kind;
    identity.verifier_version = request->verifier_version;
    std::memcpy(identity.verifier_fingerprint, request->verifier_fingerprint, 32);
    std::memcpy(identity.input_scope_hash, input_hash, 32);
    identity.gvr_count = request->gvr_count;
    Lardon3DProjectDbTrackSet found{};
    Lardon3DProjectDbResult db = lardon3d_project_db_find_track_set(
        request->database, &identity, &found);
    if (db == LARDON3D_PROJECT_DB_OK) {
      if (found.gvr_count != request->gvr_count)
        return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
      result->track_set_id = found.track_set_id;
      result->gvr_count = found.gvr_count;
      result->track_count = found.track_count;
      result->reused = true;
      return LARDON3D_TRACK_BUILDER_PROJECT_OK;
    }
    if (db != LARDON3D_PROJECT_DB_NOT_FOUND) return map_db(db);

    std::unordered_map<uint64_t, FeatureCacheEntry> cache;
    std::deque<Observation> observations;
    std::unordered_map<Key, Observation *, KeyHash> observation_map;
    std::vector<Edge> edges;
    for (size_t i = 0; i < request->gvr_count; ++i) {
      auto status = resolve_gvr(*request, request->gvr_ids[i], cache, observations,
                                observation_map, edges);
      if (status != LARDON3D_TRACK_BUILDER_PROJECT_OK) return status;
    }
    std::vector<Observation> core_observations(observations.begin(), observations.end());
    std::unordered_map<Key, Observation *, KeyHash> core_map;
    core_map.reserve(core_observations.size());
    for (auto &observation : core_observations)
      core_map.emplace(Key{observation.feature_set_id, observation.feature_index}, &observation);
    std::vector<Edge> core_edges;
    core_edges.reserve(edges.size());
    for (const Edge &edge : edges) {
      Key first{edge.first->feature_set_id, edge.first->feature_index};
      Key second{edge.second->feature_set_id, edge.second->feature_index};
      core_edges.push_back({core_map.at(first), core_map.at(second)});
    }
    Lardon3DTrackBuilderResultSet core{};
    auto core_status = lardon3d_track_builder_build(
        core_observations.empty() ? nullptr : core_observations.data(), core_observations.size(),
        core_edges.empty() ? nullptr : core_edges.data(), core_edges.size(), &core);
    if (core_status != LARDON3D_TRACK_BUILDER_OK) {
      lardon3d_track_builder_result_free(&core);
      return core_status == LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY
                 ? LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY
                 : LARDON3D_TRACK_BUILDER_PROJECT_CORE_ERROR;
    }
    std::vector<std::vector<Lardon3DProjectDbTrackObservation>> stored(core.track_count);
    std::vector<Lardon3DProjectDbTrack> publish(core.track_count);
    for (size_t i = 0; i < core.track_count; ++i) {
      stored[i].resize(core.tracks[i].observation_count);
      for (size_t j = 0; j < stored[i].size(); ++j) {
        const auto &source = core.tracks[i].observations[j];
        stored[i][j] = {source.feature_set_id, source.feature_index,
                        static_cast<uint32_t>(j)};
      }
      publish[i] = {0, 0, static_cast<uint32_t>(stored[i].size()), stored[i].data()};
    }
#ifdef LARDON3D_TRACK_BUILDER_PROJECT_TESTING
    lardon3d_track_builder_project_test_before_revalidation(
        request->database, request->gvr_ids, request->gvr_count);
#endif
    for (size_t i = 0; i < request->gvr_count; ++i) {
      db = validate_gvr_snapshot(*request, request->gvr_ids[i]);
      if (db != LARDON3D_PROJECT_DB_OK) {
        lardon3d_track_builder_result_free(&core);
        return map_db(db);
      }
    }
    Lardon3DProjectDbTrackSet published{};
    identity.track_count = core.track_count;
    db = lardon3d_project_db_create_track_set(request->database, &identity,
                                              publish.data(), publish.size(), &published);
    lardon3d_track_builder_result_free(&core);
    if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
    result->track_set_id = published.track_set_id;
    result->gvr_count = published.gvr_count;
    result->track_count = published.track_count;
    result->raw_inlier_edge_count = edges.size();
    result->core_observation_count = observations.size();
    result->reused = false;
    return LARDON3D_TRACK_BUILDER_PROJECT_OK;
  } catch (const std::bad_alloc &) {
    return LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY;
  } catch (...) {
    return LARDON3D_TRACK_BUILDER_PROJECT_DATABASE_ERROR;
  }
}
