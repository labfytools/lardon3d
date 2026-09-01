#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

extern "C" {
#include <lardon3d/track_builder.h>
}

namespace {
using Observation = Lardon3DTrackBuilderObservation;
using Edge = Lardon3DTrackBuilderEdge;
using Track = std::vector<std::pair<uint64_t, uint32_t>>;

Observation observation(uint64_t set, uint32_t feature, uint64_t image,
                        uint32_t extractor_version = 1,
                        uint32_t descriptor_type = 1,
                        uint32_t dimension = 32, unsigned char fingerprint = 7) {
  Observation value{};
  value.feature_set_id = set;
  value.feature_index = feature;
  value.image_id = image;
  std::snprintf(value.extractor_kind, sizeof(value.extractor_kind), "orb");
  value.extractor_version = extractor_version;
  value.parameter_fingerprint[0] = fingerprint;
  value.descriptor_type = descriptor_type;
  value.descriptor_dimension = dimension;
  return value;
}

Edge edge(const Observation &a, const Observation &b) { return {&a, &b}; }

std::vector<Track> read_result(const Lardon3DTrackBuilderResultSet &result) {
  std::vector<Track> tracks;
  for (size_t i = 0; i < result.track_count; ++i) {
    Track track;
    for (size_t j = 0; j < result.tracks[i].observation_count; ++j) {
      const Lardon3DTrackBuilderTrackObservation &value =
          result.tracks[i].observations[j];
      track.emplace_back(value.feature_set_id, value.feature_index);
      assert(j == 0 || track[j - 1] < track[j]);
    }
    for (size_t j = 0; j < track.size(); ++j)
      for (size_t k = j + 1; k < track.size(); ++k)
        assert(result.tracks[i].observations[j].image_id !=
               result.tracks[i].observations[k].image_id);
    assert(track.size() >= 2);
    tracks.push_back(track);
  }
  assert(std::is_sorted(tracks.begin(), tracks.end()));
  return tracks;
}

std::vector<Track> build(const std::vector<Observation> &observations,
                         const std::vector<Edge> &edges) {
  Lardon3DTrackBuilderResultSet result{};
  assert(lardon3d_track_builder_build(observations.data(), observations.size(),
                                      edges.data(), edges.size(), &result) ==
         LARDON3D_TRACK_BUILDER_OK);
  std::vector<Track> tracks = read_result(result);
  lardon3d_track_builder_result_free(&result);
  lardon3d_track_builder_result_free(&result);
  return tracks;
}

void expect(const std::vector<Observation> &observations,
            const std::vector<Edge> &edges, const std::vector<Track> &expected) {
  const auto actual = build(observations, edges);
  if (actual != expected) {
    std::fprintf(stderr, "mismatch expected=%zu actual=%zu edges=%zu\n",
                 expected.size(), actual.size(), edges.size());
    for (const auto &track : actual) {
      std::fprintf(stderr, "actual:");
      for (const auto &key : track) std::fprintf(stderr, " %llu:%u",
                                                  (unsigned long long)key.first,
                                                  key.second);
      std::fputc('\n', stderr);
    }
    assert(false);
  }
}

void test_fingerprint() {
  unsigned char bytes[48];
  unsigned char fingerprint[32];
  assert(lardon3d_track_builder_fingerprint_bytes(bytes));
  assert(lardon3d_track_builder_fingerprint(fingerprint));
  const char expected_bytes[] =
      "4c3344544246503101000000010000000100000001000000010000000100000001"
      "000000010000000200000000000000";
  const char expected_hash[] =
      "e1f1fae479bcf82001a5b33dda331195617b8751668e46a6cf1eecf2d125df31";
  for (size_t i = 0; i < 48; ++i) {
    unsigned int value = 0;
    std::sscanf(expected_bytes + 2 * i, "%2x", &value);
    assert(bytes[i] == value);
  }
  for (size_t i = 0; i < 32; ++i) {
    unsigned int high = 0;
    unsigned int low = 0;
    std::sscanf(expected_hash + 2 * i, "%1x%1x", &high, &low);
    assert(fingerprint[i] == (high * 16U + low));
  }
}

void test_adversarial() {
  const Observation a = observation(1, 0, 10);
  const Observation b = observation(2, 0, 11);
  const Observation c = observation(3, 0, 12);
  const Observation d = observation(4, 0, 13);
  const Observation conflict = observation(5, 0, 12);
  std::vector<Observation> all{a, b, c, d, conflict};
  expect(all, {edge(a, b), edge(b, c)}, {{{1, 0}, {2, 0}, {3, 0}}});
  expect(all, {edge(a, b), edge(b, c), edge(c, a)},
         {{{1, 0}, {2, 0}, {3, 0}}});
  expect(all, {edge(b, a), edge(a, b), edge(a, b)}, {{{1, 0}, {2, 0}}});
  expect(all, {edge(a, b), edge(b, c), edge(a, conflict)}, {});
  expect(all, {edge(a, b), edge(c, d)},
         {{{1, 0}, {2, 0}}, {{3, 0}, {4, 0}}});
  expect(all, {edge(a, b), edge(c, d), edge(b, conflict), edge(c, conflict)}, {});
  expect(all, {edge(a, b)}, {{{1, 0}, {2, 0}}});

  Observation heterogeneous = observation(6, 0, 14, 2);
  expect({a, b, heterogeneous}, {edge(a, b)}, {{{1, 0}, {2, 0}}});
  expect({a, b, heterogeneous}, {edge(a, heterogeneous)}, {});
  Observation same_image = observation(7, 0, 10);
  expect({a, b, same_image}, {edge(a, b), edge(b, same_image)}, {});
  expect({a, b}, {}, {});
  std::vector<Edge> duplicates;
  for (size_t i = 0; i < 100; ++i) duplicates.push_back(edge((i % 2) == 0 ? a : b,
                                                               (i % 2) == 0 ? b : a));
  expect({a, b}, duplicates, {{{1, 0}, {2, 0}}});
  Edge self = edge(a, a);
  Lardon3DTrackBuilderResultSet invalid{};
  assert(lardon3d_track_builder_build(&a, 1, &self, 1, &invalid) ==
         LARDON3D_TRACK_BUILDER_CORRUPT_INPUT);
  lardon3d_track_builder_result_free(&invalid);

  for (uint32_t dimension : {31U, 33U})
    expect({a, b, observation(8, 0, 12, 1, 1, dimension)},
           {edge(a, observation(8, 0, 12, 1, 1, dimension))}, {});
  for (uint32_t version : {2U, 3U})
    expect({a, b, observation(9, 0, 12, version)},
           {edge(a, observation(9, 0, 12, version))}, {});
  Observation different_fingerprint = observation(10, 0, 12, 1, 1, 32, 8);
  expect({a, b, different_fingerprint}, {edge(a, different_fingerprint)}, {});

  std::vector<Edge> permuted{edge(a, b), edge(b, c), edge(c, d)};
  auto canonical = build({a, b, c, d}, permuted);
  std::reverse(permuted.begin(), permuted.end());
  assert(build({a, b, c, d}, permuted) == canonical);
}

void test_corruption_and_immutability() {
  Observation a = observation(1, 0, 10);
  Observation b = observation(2, 0, 11);
  Observation contradictory = a;
  contradictory.image_id = 99;
  std::vector<Observation> inputs{a, b, contradictory};
  std::vector<Edge> edges{edge(a, b)};
  const auto before = inputs;
  Lardon3DTrackBuilderResultSet result{};
  assert(lardon3d_track_builder_build(inputs.data(), inputs.size(), edges.data(),
                                      edges.size(), &result) ==
         LARDON3D_TRACK_BUILDER_CORRUPT_INPUT);
  assert(result.track_count == 0 && result.tracks == nullptr);
  assert(inputs.size() == before.size());
  assert(std::memcmp(inputs.data(), before.data(),
                     inputs.size() * sizeof(Observation)) == 0);
  assert(lardon3d_track_builder_build(nullptr, 1, nullptr, 0, &result) ==
         LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT);
  assert(lardon3d_track_builder_build(nullptr, 0, nullptr, 0, &result) ==
         LARDON3D_TRACK_BUILDER_OK);
  lardon3d_track_builder_result_free(&result);
  assert(lardon3d_track_builder_build(nullptr, 0, nullptr, 0, nullptr) ==
         LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT);
}

void test_isolated_divergent_metadata_regression() {
  const Observation o1 = observation(1, 0, 10, 1);
  const Observation o2 = observation(1, 1, 10, 2);
  const Observation o3 = observation(2, 0, 11, 1);
  /* Public ABI contract: metadata is tied to an observation identity and is
   * checked per connected component.  Divergent metadata on isolated O2 must
   * not poison the valid O1--O3 component. */
  expect({o1, o2, o3}, {edge(o1, o3)}, {{{1, 0}, {2, 0}}});
}

void test_additional_adversarial() {
  const Observation a = observation(1, 0, 10);
  const Observation b = observation(2, 0, 11);
  const Observation c = observation(3, 0, 12);
  const Observation d = observation(4, 0, 13);
  const Observation c_again = observation(5, 0, 12);
  expect({a, b, c, d, c_again},
         {edge(a, b), edge(b, c), edge(c, d), edge(a, c_again)}, {});
  expect({a, b, c, d}, {edge(a, b), edge(a, c), edge(a, d)},
         {{{1, 0}, {2, 0}, {3, 0}, {4, 0}}});

  std::vector<Observation> disjoint;
  std::vector<Edge> disjoint_edges;
  std::vector<Track> expected;
  for (uint64_t i = 0; i < 32; ++i)
    disjoint.push_back(observation(i + 1, 0, i + 1));
  for (size_t i = 0; i < disjoint.size(); i += 2) {
    disjoint_edges.push_back(edge(disjoint[i], disjoint[i + 1]));
    expected.push_back({{i + 1, 0}, {i + 2, 0}});
  }
  assert(build(disjoint, disjoint_edges) == expected);
  std::reverse(disjoint_edges.begin(), disjoint_edges.end());
  assert(build(disjoint, disjoint_edges) == expected);
}

std::vector<Track> oracle(const std::vector<Observation> &nodes,
                          const std::vector<std::pair<size_t, size_t>> &edges,
                          uint32_t mask) {
  std::vector<std::vector<size_t>> adjacency(nodes.size());
  for (size_t i = 0; i < edges.size(); ++i) {
    if ((mask & (1U << i)) == 0) continue;
    adjacency[edges[i].first].push_back(edges[i].second);
    adjacency[edges[i].second].push_back(edges[i].first);
  }
  std::vector<bool> seen(nodes.size(), false);
  std::vector<Track> answer;
  for (size_t start = 0; start < nodes.size(); ++start) {
    if (seen[start] || adjacency[start].empty()) continue;
    std::vector<size_t> pending{start};
    seen[start] = true;
    std::vector<size_t> component;
    while (!pending.empty()) {
      size_t current = pending.back();
      pending.pop_back();
      component.push_back(current);
      for (size_t next : adjacency[current])
        if (!seen[next]) {
          seen[next] = true;
          pending.push_back(next);
        }
    }
    std::sort(component.begin(), component.end());
    std::vector<uint64_t> images;
    bool valid = component.size() >= 2;
    Track track;
    for (size_t index : component) {
      for (uint64_t image : images)
        if (image == nodes[index].image_id) valid = false;
      images.push_back(nodes[index].image_id);
      track.emplace_back(nodes[index].feature_set_id, nodes[index].feature_index);
    }
    if (valid) answer.push_back(track);
  }
  std::sort(answer.begin(), answer.end());
  return answer;
}

void test_exhaustive() {
  std::vector<Observation> nodes;
  for (uint64_t image = 1; image <= 3; ++image) {
    nodes.push_back(observation(image * 10, 0, image));
    nodes.push_back(observation(image * 10 + 1, 0, image));
  }
  std::vector<std::pair<size_t, size_t>> pairs;
  std::vector<Edge> all_edges;
  for (size_t i = 0; i < nodes.size(); ++i)
    for (size_t j = i + 1; j < nodes.size(); ++j) {
      pairs.emplace_back(i, j);
      all_edges.push_back(edge(nodes[i], nodes[j]));
    }
  for (uint32_t mask = 0; mask < (1U << pairs.size()); ++mask) {
    std::vector<Edge> selected;
    for (size_t i = 0; i < all_edges.size(); ++i)
      if (mask & (1U << i)) selected.push_back(all_edges[i]);
    assert(build(nodes, selected) == oracle(nodes, pairs, mask));
    if (mask % 257 == 0) {
      std::reverse(selected.begin(), selected.end());
      assert(build(nodes, selected) == oracle(nodes, pairs, mask));
    }
  }
}

void test_large_and_repeatable() {
  std::vector<Observation> nodes;
  std::vector<Edge> edges;
  for (uint64_t i = 0; i < 301; ++i) nodes.push_back(observation(i + 1, 0, i + 1));
  for (size_t i = 1; i < nodes.size(); ++i) edges.push_back(edge(nodes[i - 1], nodes[i]));
  auto expected = build(nodes, edges);
  assert(expected.size() == 1 && expected[0].size() == 301);
  std::mt19937 generator(12345);
  for (int run = 0; run < 100; ++run) {
    std::shuffle(edges.begin(), edges.end(), generator);
    assert(build(nodes, edges) == expected);
  }
}
} // namespace

int main() {
  test_fingerprint();
  test_adversarial();
  test_corruption_and_immutability();
  test_isolated_divergent_metadata_regression();
  test_additional_adversarial();
  test_exhaustive();
  test_large_and_repeatable();
  std::puts("track-builder-core: PASS");
  return 0;
}
