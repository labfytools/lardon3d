#include "track_builder_internal.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <openssl/evp.h>
#include <stdexcept>
#include <vector>

namespace tb = lardon3d::track_builder_internal;
using Observation = Lardon3DTrackBuilderObservation;

static_assert(sizeof(tb::Node) == 16);
static_assert(sizeof(tb::Edge) == 8);
static_assert(sizeof(tb::IdentitySlot) == 16);
static_assert(sizeof(tb::Membership) == 16);

namespace {
bool meta_equal(const tb::FeatureMetadata &a, const tb::FeatureMetadata &b,
                bool include_identity) {
  return (!include_identity || (a.feature_set_id == b.feature_set_id &&
                                 a.image_id == b.image_id)) &&
         a.extractor_version == b.extractor_version &&
         a.descriptor_type == b.descriptor_type &&
         a.descriptor_dimension == b.descriptor_dimension &&
         std::strncmp(a.extractor_kind, b.extractor_kind,
                      LARDON3D_TRACK_BUILDER_KIND_CAPACITY) == 0 &&
         std::memcmp(a.parameter_fingerprint, b.parameter_fingerprint, 32) == 0;
}
bool meta_valid(const tb::FeatureMetadata &m) {
  return m.feature_set_id != 0 && m.image_id != 0 &&
         std::memchr(m.extractor_kind, '\0', sizeof(m.extractor_kind));
}
uint64_t hash_key(uint64_t set, uint32_t index) {
  uint64_t v = set ^ (static_cast<uint64_t>(index) + 0x9e3779b97f4a7c15ULL +
                      (set << 6U) + (set >> 2U));
  v ^= v >> 30U; v *= 0xbf58476d1ce4e5b9ULL;
  v ^= v >> 27U; v *= 0x94d049bb133111ebULL;
  return v ^ (v >> 31U);
}
size_t table_capacity(uint64_t nodes) {
  if (nodes == 0) return 0;
  if (nodes > std::numeric_limits<size_t>::max() / 2U) throw std::bad_alloc();
  const size_t required = static_cast<size_t>(nodes + nodes / 2U + 1U);
  size_t capacity = 8;
  while (capacity < required) {
    if (capacity > std::numeric_limits<size_t>::max() / 2U) throw std::bad_alloc();
    capacity *= 2U;
  }
  return capacity;
}
struct Dsu {
  std::vector<uint32_t> parent;
  std::vector<unsigned char> rank;
  explicit Dsu(size_t n) : parent(n), rank(n, 0) {
    for (size_t i = 0; i < n; ++i) parent[i] = static_cast<uint32_t>(i);
  }
  uint32_t root(uint32_t v) {
    while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
    return v;
  }
  void unite(uint32_t a, uint32_t b) {
    a = root(a); b = root(b); if (a == b) return;
    if (rank[a] < rank[b]) std::swap(a, b);
    parent[b] = a; if (rank[a] == rank[b]) ++rank[a];
  }
};
tb::FeatureMetadata project(const Observation &o) {
  tb::FeatureMetadata m{};
  m.feature_set_id = o.feature_set_id; m.image_id = o.image_id;
  std::memcpy(m.extractor_kind, o.extractor_kind, sizeof(m.extractor_kind));
  m.extractor_version = o.extractor_version;
  std::memcpy(m.parameter_fingerprint, o.parameter_fingerprint, 32);
  m.descriptor_type = o.descriptor_type; m.descriptor_dimension = o.descriptor_dimension;
  return m;
}
bool digest(const unsigned char *input, size_t size, unsigned char output[32]) {
  unsigned int length = 0;
  return EVP_Digest(input, size, output, &length, EVP_sha256(), nullptr) == 1 &&
         length == 32;
}
}  // namespace

tb::CompactGraph::CompactGraph(uint64_t edge_hint) {
  if (edge_hint > UINT32_MAX / 2U || edge_hint > std::numeric_limits<size_t>::max())
    throw std::bad_alloc();
  const uint64_t node_hint = edge_hint * 2U;
  nodes_.reserve(static_cast<size_t>(node_hint));
  edges_.reserve(static_cast<size_t>(edge_hint));
  identity_.resize(table_capacity(node_hint));
}

uint32_t tb::CompactGraph::register_feature(const FeatureMetadata &m) {
  if (!meta_valid(m)) throw std::invalid_argument("invalid feature metadata");
  /* CONTRACT: CompactGraph validates metadata inside each connected component;
   * it must not globally bind a feature_set_id.  The public C ABI historically
   * permits unrelated observations from that set to carry divergent metadata,
   * while the Project adapter enforces immutable Feature Set metadata in its
   * own cache before registering exactly one entry per set. */
  if (metadata_.size() == UINT32_MAX) throw std::bad_alloc();
  metadata_.push_back(m);
  return static_cast<uint32_t>(metadata_.size() - 1U);
}

void tb::CompactGraph::insert_identity(const Node &node, uint32_t index) {
  const size_t mask = identity_.size() - 1U;
  size_t slot = static_cast<size_t>(hash_key(node.feature_set_id, node.feature_index)) & mask;
  uint64_t probes = 1;
  while (identity_[slot].node_plus_one) {
    slot = (slot + 1U) & mask;
    ++probes;
  }
  identity_[slot] = {node.feature_set_id, node.feature_index, index + 1U};
  ++identity_size_;
  ++profile_.identity_inserts;
  profile_.identity_probes += probes;
  profile_.identity_max_probe = std::max(profile_.identity_max_probe, probes);
}

uint32_t tb::CompactGraph::resolve_node(uint32_t metadata_index,
                                        uint32_t feature_index) {
  if (metadata_index >= metadata_.size() || identity_.empty())
    throw std::invalid_argument("invalid metadata index");
  const uint64_t set = metadata_[metadata_index].feature_set_id;
  const size_t mask = identity_.size() - 1U;
  size_t slot = static_cast<size_t>(hash_key(set, feature_index)) & mask;
  uint64_t probes = 1;
  while (identity_[slot].node_plus_one) {
    const IdentitySlot &found = identity_[slot];
    if (found.feature_set_id == set && found.feature_index == feature_index) {
      const uint32_t index = found.node_plus_one - 1U;
      if (nodes_[index].metadata_index != metadata_index)
        throw std::invalid_argument("contradictory observation");
      ++profile_.identity_lookups;
      profile_.identity_probes += probes;
      profile_.identity_max_probe = std::max(profile_.identity_max_probe, probes);
      return index;
    }
    slot = (slot + 1U) & mask;
    ++probes;
  }
  if (nodes_.size() == UINT32_MAX || identity_size_ == identity_.size())
    throw std::bad_alloc();
  nodes_.push_back({set, feature_index, metadata_index});
  const uint32_t index = static_cast<uint32_t>(nodes_.size() - 1U);
  insert_identity(nodes_.back(), index);
  ++profile_.identity_lookups;
  profile_.identity_probes += probes;
  profile_.identity_max_probe = std::max(profile_.identity_max_probe, probes);
  return index;
}

bool tb::CompactGraph::add_edge(uint32_t am, uint32_t ai, uint32_t bm,
                                uint32_t bi) {
  const uint32_t a = resolve_node(am, ai), b = resolve_node(bm, bi);
  if (a == b) return false;
  edges_.push_back(a < b ? Edge{a, b} : Edge{b, a});
  ++raw_edge_count_;
  return true;
}

const tb::FeatureMetadata &tb::CompactGraph::metadata(uint32_t i) const {
  return metadata_.at(i);
}
uint64_t tb::CompactGraph::node_image(uint32_t i) const {
  return metadata_.at(nodes_.at(i).metadata_index).image_id;
}

Lardon3DTrackBuilderResult tb::CompactGraph::build(Output *output) {
  if (!output) return LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT;
  output->memberships.clear(); output->tracks.clear();
  try {
    std::sort(edges_.begin(), edges_.end(), [](const Edge &a, const Edge &b) {
      return a.low < b.low || (a.low == b.low && a.high < b.high);
    });
    edges_.erase(std::unique(edges_.begin(), edges_.end(), [](const Edge &a, const Edge &b) {
      return a.low == b.low && a.high == b.high;
    }), edges_.end());
    Dsu dsu(nodes_.size());
    for (const Edge &edge : edges_) dsu.unite(edge.low, edge.high);

    /* INVARIANT: one flat grouping owns every node exactly once. DSU roots
     * delimit complete components but never determine persisted order. */
    std::vector<uint32_t> grouped(nodes_.size());
    for (size_t i = 0; i < grouped.size(); ++i) grouped[i] = static_cast<uint32_t>(i);
    std::sort(grouped.begin(), grouped.end(), [&](uint32_t a, uint32_t b) {
      const uint32_t ra = dsu.root(a), rb = dsu.root(b);
      if (ra != rb) return ra < rb;
      const Node &x = nodes_[a], &y = nodes_[b];
      return x.feature_set_id < y.feature_set_id ||
             (x.feature_set_id == y.feature_set_id && x.feature_index < y.feature_index);
    });
    std::vector<TrackRange> accepted;
    for (size_t begin = 0; begin < grouped.size();) {
      size_t end = begin + 1U; const uint32_t root = dsu.root(grouped[begin]);
      while (end < grouped.size() && dsu.root(grouped[end]) == root) ++end;
      bool valid = end - begin >= 2U;
      const FeatureMetadata &first = metadata_[nodes_[grouped[begin]].metadata_index];
      std::vector<uint64_t> images; images.reserve(end - begin);
      for (size_t i = begin; i < end; ++i) {
        const FeatureMetadata &m = metadata_[nodes_[grouped[i]].metadata_index];
        valid = valid && meta_equal(first, m, false); images.push_back(m.image_id);
      }
      std::sort(images.begin(), images.end());
      valid = valid && std::adjacent_find(images.begin(), images.end()) == images.end();
      if (valid) accepted.push_back({begin, end - begin});
      begin = end;
    }
    auto less = [&](const TrackRange &a, const TrackRange &b) {
      const size_t count = std::min(a.count, b.count);
      for (size_t i = 0; i < count; ++i) {
        const Node &x = nodes_[grouped[a.begin + i]], &y = nodes_[grouped[b.begin + i]];
        if (x.feature_set_id != y.feature_set_id) return x.feature_set_id < y.feature_set_id;
        if (x.feature_index != y.feature_index) return x.feature_index < y.feature_index;
      }
      return a.count < b.count;
    };
    std::sort(accepted.begin(), accepted.end(), less);
    size_t count = 0; for (const TrackRange &range : accepted) count += range.count;
    output->memberships.reserve(count); output->tracks.reserve(accepted.size());
    for (const TrackRange &range : accepted) {
      const size_t begin = output->memberships.size();
      for (size_t i = 0; i < range.count; ++i) {
        const uint32_t ni = grouped[range.begin + i]; const Node &n = nodes_[ni];
        output->memberships.push_back({n.feature_set_id, n.feature_index, ni});
      }
      output->tracks.push_back({begin, range.count});
    }
    return LARDON3D_TRACK_BUILDER_OK;
  } catch (const std::bad_alloc &) {
    output->memberships.clear(); output->tracks.clear();
    return LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY;
  }
}

extern "C" bool lardon3d_track_builder_fingerprint_bytes(unsigned char bytes[48]) {
  if (!bytes) return false;
  std::memset(bytes, 0, 48);
  std::memcpy(bytes, "L3DTBFP1", 8);
  const uint32_t fields[] = {1,1,1,1,1,1,1,1,2,0};
  for (size_t f = 0; f < 10; ++f) for (size_t b = 0; b < 4; ++b)
    bytes[8 + f * 4 + b] = static_cast<unsigned char>(fields[f] >> (b * 8));
  return true;
}
extern "C" bool lardon3d_track_builder_fingerprint(unsigned char output[32]) {
  unsigned char bytes[48]; return output && lardon3d_track_builder_fingerprint_bytes(bytes) &&
                                   digest(bytes, sizeof(bytes), output);
}
extern "C" void lardon3d_track_builder_result_free(Lardon3DTrackBuilderResultSet *result) {
  if (!result) return;
  if (result->tracks) {
    for (size_t i = 0; i < result->track_count; ++i)
      delete[] result->tracks[i].observations;
    delete[] result->tracks;
  }
  result->tracks = nullptr; result->track_count = 0;
}

extern "C" Lardon3DTrackBuilderResult lardon3d_track_builder_build(
    const Observation *observations, size_t observation_count,
    const Lardon3DTrackBuilderEdge *edges, size_t edge_count,
    Lardon3DTrackBuilderResultSet *result) {
  if (!result || (observation_count && !observations) || (edge_count && !edges))
    return LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT;
  lardon3d_track_builder_result_free(result);
  try {
    tb::CompactGraph graph(edge_count);
    struct PublicIdentity {
      uint64_t feature_set_id;
      uint32_t feature_index;
      tb::FeatureMetadata metadata;
      uint32_t metadata_index = 0;
      bool registered = false;
    };
    std::vector<PublicIdentity> input_identities;
    input_identities.reserve(observation_count);
    for (size_t i = 0; i < observation_count; ++i) {
      const tb::FeatureMetadata metadata = project(observations[i]);
      if (!meta_valid(metadata)) return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
      input_identities.push_back({observations[i].feature_set_id,
                                  observations[i].feature_index, metadata});
    }
    std::sort(input_identities.begin(), input_identities.end(),
              [](const PublicIdentity &a, const PublicIdentity &b) {
                return std::make_pair(a.feature_set_id, a.feature_index) <
                       std::make_pair(b.feature_set_id, b.feature_index);
              });
    for (size_t i = 1; i < input_identities.size(); ++i) {
      if (input_identities[i - 1].feature_set_id == input_identities[i].feature_set_id &&
          input_identities[i - 1].feature_index == input_identities[i].feature_index &&
          !meta_equal(input_identities[i - 1].metadata, input_identities[i].metadata, true))
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
    }
    auto find_identity = [&](uint64_t set, uint32_t index) -> PublicIdentity * {
      auto found = std::lower_bound(
          input_identities.begin(), input_identities.end(), std::make_pair(set, index),
          [](const PublicIdentity &value, const std::pair<uint64_t, uint32_t> &key) {
            return std::make_pair(value.feature_set_id, value.feature_index) < key;
          });
      return found != input_identities.end() && found->feature_set_id == set &&
                     found->feature_index == index ? &*found : nullptr;
    };
    for (size_t i = 0; i < edge_count; ++i) {
      if (!edges[i].first || !edges[i].second)
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
      PublicIdentity *first = find_identity(edges[i].first->feature_set_id,
                                            edges[i].first->feature_index);
      PublicIdentity *second = find_identity(edges[i].second->feature_set_id,
                                             edges[i].second->feature_index);
      if (!first || !second || !meta_equal(first->metadata, project(*edges[i].first), true) ||
          !meta_equal(second->metadata, project(*edges[i].second), true))
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
      /* WHY: only observations participating in an edge enter CompactGraph.
       * Isolated public inputs cannot invalidate an unrelated component. */
      if (!first->registered) {
        first->metadata_index = graph.register_feature(first->metadata);
        first->registered = true;
      }
      if (!second->registered) {
        second->metadata_index = graph.register_feature(second->metadata);
        second->registered = true;
      }
      if (!graph.add_edge(first->metadata_index, first->feature_index,
                          second->metadata_index, second->feature_index))
        return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
    }
    tb::Output output; const auto status = graph.build(&output); if (status) return status;
    if (!output.tracks.empty()) {
      result->tracks = new Lardon3DTrackBuilderTrack[output.tracks.size()]();
      result->track_count = output.tracks.size();
      for (size_t i = 0; i < output.tracks.size(); ++i) {
        const tb::TrackRange &range = output.tracks[i];
        result->tracks[i].observation_count = range.count;
        result->tracks[i].observations = new Lardon3DTrackBuilderTrackObservation[range.count];
        for (size_t j = 0; j < range.count; ++j) {
          const tb::Membership &m = output.memberships[range.begin + j];
          result->tracks[i].observations[j] =
              {m.feature_set_id, m.feature_index, graph.node_image(m.node_index)};
        }
      }
    }
    return LARDON3D_TRACK_BUILDER_OK;
  } catch (const std::invalid_argument &) {
    lardon3d_track_builder_result_free(result); return LARDON3D_TRACK_BUILDER_CORRUPT_INPUT;
  } catch (...) {
    lardon3d_track_builder_result_free(result); return LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY;
  }
}
