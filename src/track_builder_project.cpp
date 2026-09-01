#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <new>
#include <openssl/evp.h>
#include <string>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "track_builder_internal.hpp"

extern "C" {
#include <lardon3d/match_file.h>
#include <lardon3d/track_builder.h>
#include <lardon3d/track_builder_project.h>
}

namespace {
namespace internal = lardon3d::track_builder_internal;

bool profile_enabled() {
  const char *value = std::getenv("LARDON3D_TRACK_BUILDER_PROFILE");
  return value != nullptr && std::strcmp(value, "1") == 0;
}

double elapsed_seconds(const std::chrono::steady_clock::time_point &start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/* CONTRACT: profile output is opt-in diagnostic stderr only.  It carries no
 * task identity or scientific state, so a restarted task remains exactly the
 * same durable operation whether profiling is enabled or absent. */
void report_profile(bool enabled, uint64_t gvr_count, uint64_t raw_edge_hint,
                    const internal::CompactGraph &graph, double hint_seconds,
                    double graph_seconds, double resolve_seconds, double core_seconds,
                    double serialize_seconds, double revalidate_seconds) {
  if (!enabled) return;
  const internal::Profile &profile = graph.profile();
  const uint64_t identity_operations = profile.identity_lookups + profile.identity_inserts;
  const double mean_probe = identity_operations == 0
                                ? 0.0
                                : static_cast<double>(profile.identity_probes) /
                                      static_cast<double>(identity_operations);
  std::fprintf(stderr,
               "track_builder_profile gvr=%llu raw_edge_hint=%llu nodes=%zu raw_edges=%llu "
               "hint_s=%.3f graph_s=%.3f resolve_s=%.3f core_s=%.3f serialize_s=%.3f "
               "revalidate_s=%.3f identity_lookups=%llu identity_inserts=%llu "
               "identity_mean_probe=%.3f identity_max_probe=%llu\n",
               static_cast<unsigned long long>(gvr_count),
               static_cast<unsigned long long>(raw_edge_hint), graph.node_count(),
               static_cast<unsigned long long>(graph.raw_edge_count()), hint_seconds,
               graph_seconds, resolve_seconds, core_seconds, serialize_seconds,
               revalidate_seconds,
               static_cast<unsigned long long>(profile.identity_lookups),
               static_cast<unsigned long long>(profile.identity_inserts), mean_probe,
               static_cast<unsigned long long>(profile.identity_max_probe));
}

#ifdef LARDON3D_TRACK_BUILDER_PROJECT_TESTING
extern "C" void lardon3d_track_builder_project_test_publication_capacities(
    uint64_t node_count, uint64_t serialization_capacity_bytes);
#endif

struct FeatureCacheEntry {
  uint64_t feature_set_id = 0;
  uint64_t image_id = 0;
  uint32_t feature_count = 0;
  uint32_t metadata_index = 0;
};

internal::FeatureMetadata project_metadata(const Lardon3DProjectDbFeatureSet &set) {
  internal::FeatureMetadata value{};
  value.feature_set_id = set.feature_set_id;
  value.image_id = set.image_id;
  std::memcpy(value.extractor_kind, set.extractor_kind, sizeof(value.extractor_kind));
  value.extractor_version = set.extractor_version;
  std::memcpy(value.parameter_fingerprint, set.parameter_fingerprint, 32);
  value.descriptor_type = set.descriptor_type;
  value.descriptor_dimension = set.descriptor_dimension;
  return value;
}

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
    internal::CompactGraph &graph) {
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
  auto load_set = [&](uint64_t id, FeatureCacheEntry **out) {
    auto found = cache.find(id);
    if (found != cache.end()) {
      *out = &found->second;
      return LARDON3D_PROJECT_DB_OK;
    }
    FeatureCacheEntry entry{};
    Lardon3DProjectDbFeatureSet loaded{};
    Lardon3DProjectDbResult value = lardon3d_project_db_load_feature_set(
        request.database, id, &loaded);
    if (value == LARDON3D_PROJECT_DB_OK) {
      entry.feature_set_id = loaded.feature_set_id;
      entry.image_id = loaded.image_id;
      entry.feature_count = loaded.feature_count;
      entry.metadata_index = graph.register_feature(project_metadata(loaded));
      auto inserted = cache.emplace(id, entry);
      *out = &inserted.first->second;
    }
    return value;
  };
  FeatureCacheEntry *set_a = nullptr;
  FeatureCacheEntry *set_b = nullptr;
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
    const auto cache_a = cache.find(set_a->feature_set_id);
    const auto cache_b = cache.find(set_b->feature_set_id);
    if (cache_a == cache.end() || cache_b == cache.end() ||
        !graph.add_edge(cache_a->second.metadata_index, entry.feature_index_a,
                        cache_b->second.metadata_index, entry.feature_index_b))
      return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  }
  if (selected != gvr.inlier_count) return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  return LARDON3D_TRACK_BUILDER_PROJECT_OK;
}

} // namespace

Lardon3DTrackBuilderProjectStatus internal::build_project(
    const Lardon3DTrackBuilderProjectRequest *request,
    Lardon3DTrackBuilderProjectResult *result, Checkpoint checkpoint,
    void *checkpoint_userdata) {
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
    const bool profiling = profile_enabled();
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

    const auto hint_start = std::chrono::steady_clock::now();
    uint64_t raw_edge_hint = 0;
    for (size_t i = 0; i < request->gvr_count; ++i) {
      Lardon3DProjectDbGeometricVerificationResult gvr{};
      db = lardon3d_project_db_load_geometric_verification_result(
          request->database, request->gvr_ids[i], &gvr);
      if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
      if (gvr.inlier_count > UINT64_MAX - raw_edge_hint)
        return LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY;
      raw_edge_hint += gvr.inlier_count;
    }
    const double hint_seconds = elapsed_seconds(hint_start);
    const auto graph_start = std::chrono::steady_clock::now();
    internal::CompactGraph graph(raw_edge_hint);
    const double graph_seconds = elapsed_seconds(graph_start);
    std::unordered_map<uint64_t, FeatureCacheEntry> cache;
    const auto resolve_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < request->gvr_count; ++i) {
      auto status = resolve_gvr(*request, request->gvr_ids[i], cache, graph);
      if (status != LARDON3D_TRACK_BUILDER_PROJECT_OK) return status;
      /* CONTRACT: Task pause/cancel is observed only between complete GVR
       * reads. DSU, canonicalization and atomic publication are never
       * preempted, so interruption cannot expose a partial Track Set. */
      if (checkpoint && !checkpoint(checkpoint_userdata))
        return LARDON3D_TRACK_BUILDER_PROJECT_INTERRUPTED;
    }
    const double resolve_seconds = elapsed_seconds(resolve_start);
    const auto core_start = std::chrono::steady_clock::now();
    internal::Output core;
    auto core_status = graph.build(&core);
    if (core_status != LARDON3D_TRACK_BUILDER_OK) {
      return core_status == LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY
                 ? LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY
                 : LARDON3D_TRACK_BUILDER_PROJECT_CORE_ERROR;
    }
    const double core_seconds = elapsed_seconds(core_start);
    /* GATE D CONTRACT: Gate B (DSU/canonicalization) is deliberately
     * non-preemptible, but a pause/cancel raised during it must be observed
     * before any publication preparation can become durable. */
    if (checkpoint && !checkpoint(checkpoint_userdata))
      return LARDON3D_TRACK_BUILDER_PROJECT_INTERRUPTED;
    /* CONTRACT/SERIALIZATION: this is the sole transient conversion from
     * canonical compact membership to Track Model rows. position_in_track is
     * the already-canonical flat order; DB publication below remains the sole
     * atomic, owner-only persistence point. */
    const auto serialize_start = std::chrono::steady_clock::now();
    std::vector<std::vector<Lardon3DProjectDbTrackObservation>> stored(core.tracks.size());
    std::vector<Lardon3DProjectDbTrack> publish(core.tracks.size());
    for (size_t i = 0; i < core.tracks.size(); ++i) {
      const internal::TrackRange &range = core.tracks[i];
      stored[i].resize(range.count);
      for (size_t j = 0; j < stored[i].size(); ++j) {
        const auto &source = core.memberships[range.begin + j];
        stored[i][j] = {source.feature_set_id, source.feature_index,
                        static_cast<uint32_t>(j)};
      }
      publish[i] = {0, 0, static_cast<uint32_t>(stored[i].size()), stored[i].data()};
    }
    const double serialize_seconds = elapsed_seconds(serialize_start);
#ifdef LARDON3D_TRACK_BUILDER_PROJECT_TESTING
    uint64_t stored_observation_capacity = 0;
    for (const auto &observations : stored)
      stored_observation_capacity += observations.capacity();
    const uint64_t serialization_capacity_bytes =
        stored_observation_capacity * sizeof(Lardon3DProjectDbTrackObservation) +
        stored.capacity() * sizeof(std::vector<Lardon3DProjectDbTrackObservation>) +
        publish.capacity() * sizeof(Lardon3DProjectDbTrack);
    lardon3d_track_builder_project_test_publication_capacities(
        graph.node_count(), serialization_capacity_bytes);
    lardon3d_track_builder_project_test_before_revalidation(
        request->database, request->gvr_ids, request->gvr_count);
#endif
    const auto revalidate_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < request->gvr_count; ++i) {
      db = validate_gvr_snapshot(*request, request->gvr_ids[i]);
      if (db != LARDON3D_PROJECT_DB_OK) {
        return map_db(db);
      }
    }
    const double revalidate_seconds = elapsed_seconds(revalidate_start);
    report_profile(profiling, request->gvr_count, raw_edge_hint, graph, hint_seconds,
                   graph_seconds, resolve_seconds, core_seconds, serialize_seconds,
                   revalidate_seconds);
    /* GATE D CONTRACT: this is the final interruptible boundary.  Refusal
     * guarantees zero publication; create_track_set below remains one
     * non-preemptible atomic publication. */
    if (checkpoint && !checkpoint(checkpoint_userdata))
      return LARDON3D_TRACK_BUILDER_PROJECT_INTERRUPTED;
    Lardon3DProjectDbTrackSet published{};
    identity.track_count = core.tracks.size();
#ifdef LARDON3D_TRACK_BUILDER_PROJECT_TESTING
    lardon3d_track_builder_project_test_before_publication(request);
    Lardon3DProjectDbTrackSet raced_before_publish{};
    if (lardon3d_project_db_find_track_set(request->database, &identity,
                                           &raced_before_publish) == LARDON3D_PROJECT_DB_OK) {
      result->track_set_id = raced_before_publish.track_set_id;
      result->gvr_count = raced_before_publish.gvr_count;
      result->track_count = raced_before_publish.track_count;
      result->raw_inlier_edge_count = graph.raw_edge_count();
      result->core_observation_count = graph.node_count();
      result->reused = true;
      return LARDON3D_TRACK_BUILDER_PROJECT_OK;
    }
#endif
    db = lardon3d_project_db_create_track_set(request->database, &identity,
                                              publish.data(), publish.size(), &published);
    if (db == LARDON3D_PROJECT_DB_CONSTRAINT) {
      Lardon3DProjectDbTrackSet raced{};
      db = lardon3d_project_db_find_track_set(request->database, &identity, &raced);
      if (db == LARDON3D_PROJECT_DB_OK) {
        result->track_set_id = raced.track_set_id;
        result->gvr_count = raced.gvr_count;
        result->track_count = raced.track_count;
        result->raw_inlier_edge_count = graph.raw_edge_count();
        result->core_observation_count = graph.node_count();
        result->reused = true;
        return LARDON3D_TRACK_BUILDER_PROJECT_OK;
      }
    }
    if (db != LARDON3D_PROJECT_DB_OK) return map_db(db);
    result->track_set_id = published.track_set_id;
    result->gvr_count = published.gvr_count;
    result->track_count = published.track_count;
    result->raw_inlier_edge_count = graph.raw_edge_count();
    result->core_observation_count = graph.node_count();
    result->reused = false;
    return LARDON3D_TRACK_BUILDER_PROJECT_OK;
  } catch (const std::bad_alloc &) {
    return LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY;
  } catch (const std::invalid_argument &) {
    return LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT;
  } catch (...) {
    return LARDON3D_TRACK_BUILDER_PROJECT_DATABASE_ERROR;
  }
}

extern "C" Lardon3DTrackBuilderProjectStatus lardon3d_track_builder_build_project(
    const Lardon3DTrackBuilderProjectRequest *request,
    Lardon3DTrackBuilderProjectResult *result) {
  return internal::build_project(request, result, nullptr, nullptr);
}
