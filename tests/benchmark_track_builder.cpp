#include <chrono>
#include <cstdio>
#include <sys/resource.h>
#include <vector>

extern "C" {
#include <lardon3d/track_builder.h>
}

static Lardon3DTrackBuilderObservation make_observation(uint64_t id) {
  Lardon3DTrackBuilderObservation value{};
  value.feature_set_id = id + 1;
  value.image_id = id + 1;
  value.extractor_version = 1;
  value.descriptor_type = 1;
  value.descriptor_dimension = 32;
  std::snprintf(value.extractor_kind, sizeof(value.extractor_kind), "orb");
  return value;
}

int main() {
  for (size_t edge_count : {10000U, 100000U, 1000000U}) {
    std::vector<Lardon3DTrackBuilderObservation> observations;
    std::vector<Lardon3DTrackBuilderEdge> edges;
    observations.reserve(edge_count + 1);
    edges.reserve(edge_count);
    for (size_t i = 0; i <= edge_count; ++i)
      observations.push_back(make_observation(i));
    for (size_t i = 1; i <= edge_count; ++i)
      edges.push_back({&observations[i - 1], &observations[i]});
    Lardon3DTrackBuilderResultSet result{};
    auto start = std::chrono::steady_clock::now();
    auto status = lardon3d_track_builder_build(
        observations.data(), observations.size(), edges.data(), edges.size(), &result);
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
    struct rusage usage{};
    (void)getrusage(RUSAGE_SELF, &usage);
    std::printf("edges=%zu status=%d seconds=%.3f rss_kib=%ld observations=%zu tracks=%zu\n",
                edge_count, status, elapsed.count(), usage.ru_maxrss,
                observations.size(), result.track_count);
    lardon3d_track_builder_result_free(&result);
  }
}
