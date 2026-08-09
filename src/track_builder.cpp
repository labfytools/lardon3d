#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <openssl/evp.h>
#include <unordered_map>
#include <vector>

extern "C" {
#include <lardon3d/track_builder.h>
}

namespace {
using Observation = Lardon3DTrackBuilderObservation;
using Edge = Lardon3DTrackBuilderEdge;

struct CompactEdge {
  size_t low;
  size_t high;
};

bool identity_less(const Observation &a, const Observation &b) {
  return a.feature_set_id < b.feature_set_id ||
         (a.feature_set_id == b.feature_set_id &&
          a.feature_index < b.feature_index);
}

bool identity_equal(const Observation &a, const Observation &b) {
  return a.feature_set_id == b.feature_set_id &&
         a.feature_index == b.feature_index;
}

bool metadata_equal(const Observation &a, const Observation &b) {
  return a.image_id == b.image_id &&
         a.extractor_version == b.extractor_version &&
         a.descriptor_type == b.descriptor_type &&
         a.descriptor_dimension == b.descriptor_dimension &&
         std::strncmp(a.extractor_kind, b.extractor_kind,
                      LARDON3D_TRACK_BUILDER_KIND_CAPACITY) == 0 &&
         std::memcmp(a.parameter_fingerprint, b.parameter_fingerprint, 32) == 0;
}

bool observation_valid(const Observation &observation) {
  return observation.feature_set_id != 0 && observation.image_id != 0 &&
         std::memchr(observation.extractor_kind, '\0',
                     LARDON3D_TRACK_BUILDER_KIND_CAPACITY) != nullptr;
}

struct Dsu {
  std::vector<size_t> parent;
  std::vector<unsigned char> rank;
  explicit Dsu(size_t size) : parent(size), rank(size, 0) {
    for (size_t i = 0; i < size; ++i) parent[i] = i;
  }
  size_t root(size_t value) {
    while (parent[value] != value) {
      parent[value] = parent[parent[value]];
      value = parent[value];
    }
    return value;
  }
  void unite(size_t a, size_t b) {
    a = root(a); b = root(b);
    if (a == b) return;
    if (rank[a] < rank[b]) std::swap(a, b);
    parent[b] = a;
    if (rank[a] == rank[b]) ++rank[a];
  }
};

struct IdentityHash {
  size_t operator()(const std::pair<uint64_t, uint32_t> &key) const noexcept {
    uint64_t value = key.first ^ (static_cast<uint64_t>(key.second) +
                                  0x9e3779b97f4a7c15ULL + (key.first << 6U) +
                                  (key.first >> 2U));
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    return static_cast<size_t>(value ^ (value >> 31U));
  }
};

std::pair<uint64_t, uint32_t> identity_key(const Observation &observation) {
  return {observation.feature_set_id, observation.feature_index};
}

bool digest(const unsigned char *input, size_t size, unsigned char output[32]) {
  unsigned int output_size = 0;
  return EVP_Digest(input, size, output, &output_size, EVP_sha256(), nullptr) == 1 &&
         output_size == 32;
}

} // namespace

extern "C" bool lardon3d_track_builder_fingerprint_bytes(unsigned char bytes[48]) {
  if (!bytes) return false;
  std::memset(bytes, 0, 48);
  std::memcpy(bytes, "L3DTBFP1", 8);
  const uint32_t fields[] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 0};
  for (size_t field = 0; field < 10; ++field)
    for (size_t byte = 0; byte < 4; ++byte)
      bytes[8 + field * 4 + byte] =
          static_cast<unsigned char>(fields[field] >> (byte * 8));
  return true;
}

extern "C" bool lardon3d_track_builder_fingerprint(unsigned char fingerprint[32]) {
  if (!fingerprint) return false;
  unsigned char bytes[48];
  return lardon3d_track_builder_fingerprint_bytes(bytes) && digest(bytes, 48, fingerprint);
}

extern "C" void lardon3d_track_builder_result_free(Lardon3DTrackBuilderResultSet *result) {
  if (!result) return;
  if (result->tracks) {
    for (size_t i = 0; i < result->track_count; ++i)
      delete[] result->tracks[i].observations;
    delete[] result->tracks;
  }
  result->tracks = nullptr;
  result->track_count = 0;
}

extern "C" Lardon3DTrackBuilderResult lardon3d_track_builder_build(
    const Observation *input_observations, size_t observation_count, const Edge *input_edges,
    size_t edge_count, Lardon3DTrackBuilderResultSet *result) {
  if (!result || (observation_count != 0 && !input_observations) ||
      (edge_count != 0 && !input_edges))
    return LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT;
  lardon3d_track_builder_result_free(result);
  try {
    if (edge_count > std::numeric_limits<size_t>::max() / 2)
      return LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT;
    std::vector<Observation> table;
    if (observation_count != 0)
      table.assign(input_observations, input_observations + observation_count);
    for (const Observation &observation : table)
      if (!observation_valid(observation)) return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
    std::sort(table.begin(), table.end(), identity_less);
    for (size_t i = 1; i < table.size(); ++i)
      if (identity_equal(table[i - 1], table[i]) &&
          !metadata_equal(table[i - 1], table[i]))
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
    table.erase(std::unique(table.begin(), table.end(), identity_equal), table.end());
    std::unordered_map<std::pair<uint64_t, uint32_t>, size_t, IdentityHash> table_indices;
    table_indices.reserve(table.size());
    for (size_t i = 0; i < table.size(); ++i) table_indices.emplace(identity_key(table[i]), i);

    std::vector<CompactEdge> normalized;
    normalized.reserve(edge_count);
    for (size_t i = 0; i < edge_count; ++i) {
      const Edge &edge = input_edges[i];
      if (!edge.first || !edge.second || !observation_valid(*edge.first) ||
          !observation_valid(*edge.second) || identity_equal(*edge.first, *edge.second))
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
      auto first_it = table_indices.find(identity_key(*edge.first));
      auto second_it = table_indices.find(identity_key(*edge.second));
      if (first_it == table_indices.end() || second_it == table_indices.end() ||
          first_it->second >= table.size() || second_it->second >= table.size() ||
          !metadata_equal(table[first_it->second], *edge.first) ||
          !metadata_equal(table[second_it->second], *edge.second))
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
      if (first_it->second < second_it->second)
        normalized.push_back({first_it->second, second_it->second});
      else
        normalized.push_back({second_it->second, first_it->second});
    }
    std::sort(normalized.begin(), normalized.end(), [](const CompactEdge &a,
                                                       const CompactEdge &b) {
      return a.low < b.low || (a.low == b.low && a.high < b.high);
    });
    normalized.erase(std::unique(normalized.begin(), normalized.end(),
                                 [](const CompactEdge &a, const CompactEdge &b) {
                                   return a.low == b.low && a.high == b.high;
                                 }),
                     normalized.end());
    Dsu dsu(table.size());
    for (const CompactEdge &edge : normalized) dsu.unite(edge.low, edge.high);
    std::vector<std::vector<size_t>> components(table.size());
    for (size_t i = 0; i < table.size(); ++i)
      if (!normalized.empty()) components[dsu.root(i)].push_back(i);
    std::vector<std::vector<size_t>> accepted;
    for (auto &component : components) {
      if (component.size() < 2) continue;
      std::sort(component.begin(), component.end());
      bool valid = true;
      std::vector<uint64_t> images;
      images.reserve(component.size());
      const Observation &first = table[component.front()];
      for (size_t node : component) {
        const Observation &observation = table[node];
        if (observation.image_id == 0 ||
            (observation.extractor_version != first.extractor_version) ||
            (observation.descriptor_type != first.descriptor_type) ||
            (observation.descriptor_dimension != first.descriptor_dimension) ||
            std::strncmp(observation.extractor_kind, first.extractor_kind,
                         LARDON3D_TRACK_BUILDER_KIND_CAPACITY) != 0 ||
            std::memcmp(observation.parameter_fingerprint, first.parameter_fingerprint, 32) != 0)
          valid = false;
        images.push_back(observation.image_id);
      }
      std::sort(images.begin(), images.end());
      for (size_t i = 1; i < images.size(); ++i)
        if (images[i - 1] == images[i]) valid = false;
      if (valid) accepted.push_back(component);
    }
    auto component_less = [&table](const std::vector<size_t> &a,
                                   const std::vector<size_t> &b) {
      size_t count = std::min(a.size(), b.size());
      for (size_t i = 0; i < count; ++i) {
        if (identity_less(table[a[i]], table[b[i]])) return true;
        if (identity_less(table[b[i]], table[a[i]])) return false;
      }
      return a.size() < b.size();
    };
    std::sort(accepted.begin(), accepted.end(), component_less);
    if (!accepted.empty()) {
      result->tracks = new Lardon3DTrackBuilderTrack[accepted.size()]();
      result->track_count = accepted.size();
      for (size_t i = 0; i < accepted.size(); ++i) {
        result->tracks[i].observation_count = accepted[i].size();
        result->tracks[i].observations =
            new Lardon3DTrackBuilderTrackObservation[accepted[i].size()];
        for (size_t j = 0; j < accepted[i].size(); ++j) {
          const Observation &source = table[accepted[i][j]];
          result->tracks[i].observations[j] = {
              source.feature_set_id, source.feature_index, source.image_id};
        }
      }
    }
    return LARDON3D_TRACK_BUILDER_OK;
  } catch (const std::bad_alloc &) {
    lardon3d_track_builder_result_free(result);
    return LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY;
  } catch (...) {
    lardon3d_track_builder_result_free(result);
    return LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY;
  }
}
