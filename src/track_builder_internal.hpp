#ifndef LARDON3D_TRACK_BUILDER_INTERNAL_HPP
#define LARDON3D_TRACK_BUILDER_INTERNAL_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include <lardon3d/track_builder.h>
#include <lardon3d/track_builder_project.h>
}

namespace lardon3d::track_builder_internal {

struct FeatureMetadata {
  uint64_t feature_set_id;
  uint64_t image_id;
  char extractor_kind[LARDON3D_TRACK_BUILDER_KIND_CAPACITY];
  uint32_t extractor_version;
  unsigned char parameter_fingerprint[32];
  uint32_t descriptor_type;
  uint32_t descriptor_dimension;
};

struct Node {
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint32_t metadata_index;
};

struct Edge {
  uint32_t low;
  uint32_t high;
};

struct IdentitySlot {
  uint64_t feature_set_id = 0;
  uint32_t feature_index = 0;
  uint32_t node_plus_one = 0;
};

struct Membership {
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint32_t node_index;
};

struct TrackRange {
  size_t begin;
  size_t count;
};

struct Output {
  std::vector<Membership> memberships;
  std::vector<TrackRange> tracks;
};

/* WHY: Project construction must have one owner for nodes, identity lookup and
 * indexed edges.  Transporting pointer edges through the public ABI recreated
 * the complete graph twice before DSU.  This private type is deliberately not
 * persistent and carries no scientific policy. */
class CompactGraph {
 public:
  explicit CompactGraph(uint64_t raw_edge_hint);
  uint32_t register_feature(const FeatureMetadata &metadata);
  bool add_edge(uint32_t first_metadata, uint32_t first_feature_index,
                uint32_t second_metadata, uint32_t second_feature_index);
  Lardon3DTrackBuilderResult build(Output *output);
  const FeatureMetadata &metadata(uint32_t index) const;
  uint64_t node_image(uint32_t node_index) const;
  size_t node_count() const { return nodes_.size(); }
  uint64_t raw_edge_count() const { return raw_edge_count_; }

 private:
  uint32_t resolve_node(uint32_t metadata_index, uint32_t feature_index);
  void insert_identity(const Node &node, uint32_t node_index);

  std::vector<FeatureMetadata> metadata_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<IdentitySlot> identity_;
  size_t identity_size_ = 0;
  uint64_t raw_edge_count_ = 0;
};

using Checkpoint = bool (*)(void *userdata);

Lardon3DTrackBuilderProjectStatus build_project(
    const Lardon3DTrackBuilderProjectRequest *request,
    Lardon3DTrackBuilderProjectResult *result, Checkpoint checkpoint,
    void *checkpoint_userdata);

}  // namespace lardon3d::track_builder_internal

#endif
