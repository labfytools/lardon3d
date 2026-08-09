#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

extern "C" {
#include <lardon3d/app_state.h>
#include <lardon3d/feature_store.h>
#include <lardon3d/matcher.h>
#include <lardon3d/project_db.h>
}

struct Sample {
  uint64_t open_ns;
  uint64_t read_ns;
  uint64_t knn_ns;
  uint64_t filter_ns;
  uint64_t canonicalize_ns;
  uint64_t serialize_ns;
  uint64_t sha_ns;
  uint64_t publish_ns;
  uint64_t database_ns;
  uint64_t total_ns;
};

static uint64_t median(std::vector<uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

static void image_path(const unsigned char hash[32], char path[4096]) {
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (size_t i = 0; i < 32; ++i) {
    hex[2 * i] = digits[hash[i] >> 4];
    hex[2 * i + 1] = digits[hash[i] & 15U];
  }
  hex[64] = '\0';
  (void)snprintf(path, 4096, "assets/images/%c%c/%s", hex[0], hex[1], hex);
}

static bool register_image(Lardon3DProjectDb *db, uint64_t scanset_id, unsigned char seed,
                           Lardon3DProjectDbImage *image) {
  unsigned char hash[32];
  memset(hash, seed, sizeof(hash));
  char path[4096];
  image_path(hash, path);
  Lardon3DProjectDbImageRegisterStatus status;
  return lardon3d_project_db_register_image(db, scanset_id, hash, path, 1, "fixture.bin",
                                             "/synthetic/fixture.bin", 0, seed, &status,
                                             image) == LARDON3D_PROJECT_DB_OK;
}

static void fill_features(Lardon3DExtractedFeatures *features, uint32_t count,
                          Lardon3DFeatureDescriptorType type, unsigned char salt) {
  (void)salt;
  memset(features, 0, sizeof(*features));
  features->image_width = 4096;
  features->image_height = 4096;
  features->feature_count = count;
  features->keypoints = static_cast<Lardon3DFeatureKeypoint *>(
      calloc(count == 0 ? 1 : count, sizeof(*features->keypoints)));
  for (uint32_t i = 0; i < count; ++i) {
    features->keypoints[i].x = (float)(i % 4096U);
    features->keypoints[i].y = (float)(i % 4096U);
    features->keypoints[i].size = 1.0F;
  }
  size_t scalar = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 1 : sizeof(float);
  features->descriptor_bytes = (size_t)count * 128 * scalar;
  features->descriptors = static_cast<unsigned char *>(malloc(features->descriptor_bytes));
  if (type == LARDON3D_FEATURE_DESCRIPTOR_U8) {
    features->descriptor_bytes = (size_t)count * 32;
    features->descriptors = static_cast<unsigned char *>(
        realloc(features->descriptors, features->descriptor_bytes));
    for (uint32_t row = 0; row < count; ++row) {
      uint32_t value = row + 1U;
      for (uint32_t column = 0; column < 32; ++column) {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        features->descriptors[(size_t)row * 32 + column] = (unsigned char)value;
      }
    }
  } else {
    float *values = reinterpret_cast<float *>(features->descriptors);
    size_t value_count = (size_t)count * 128;
    for (size_t i = 0; i < value_count; ++i)
      values[i] = (float)(((i / 128U) * 37U + (i % 128U) * 11U) % 65521U) / 65521.0F;
  }
}

static bool publish(Lardon3DAppState *state, uint64_t image_id, const char *kind,
                    Lardon3DFeatureDescriptorType type, uint32_t count, unsigned char salt,
                    Lardon3DProjectDbFeatureSet *set) {
  Lardon3DExtractedFeatures features;
  fill_features(&features, count, type, salt);
  unsigned char fingerprint[32];
  memset(fingerprint, salt, sizeof(fingerprint));
  uint32_t dimension = type == LARDON3D_FEATURE_DESCRIPTOR_U8 ? 32 : 128;
  Lardon3DFeatureStoreResult result = lardon3d_feature_store_publish_v2(
      state, image_id, 0, kind, 1, fingerprint, type, dimension, 0, &features, set);
  free(features.keypoints);
  free(features.descriptors);
  return result == LARDON3D_FEATURE_STORE_OK;
}

static bool benchmark_case(const char *kind, Lardon3DMatcherKind matcher_kind,
                           Lardon3DFeatureDescriptorType type, uint32_t count,
                           int repetitions) {
  char root_template[] = "/tmp/lardon3d-matcher-bench-XXXXXX";
  char *root = mkdtemp(root_template);
  if (!root) return false;
  std::string db_path = std::string(root) + "/project.db";
  Lardon3DProjectDb *db = nullptr;
  char error[256];
  if (lardon3d_project_db_open(db_path.c_str(), &db, error) != LARDON3D_PROJECT_DB_OK)
    return false;
  Lardon3DProjectDbScanSet scanset;
  Lardon3DProjectDbImage image_a, image_b;
  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = db;
  (void)snprintf(state.project_path, sizeof(state.project_path), "%s", root);
  bool ok = lardon3d_project_db_create_scanset(db, "benchmark", &scanset) ==
                LARDON3D_PROJECT_DB_OK &&
            register_image(db, scanset.scanset_id, 1, &image_a) &&
            register_image(db, scanset.scanset_id, 2, &image_b);
  Lardon3DProjectDbFeatureSet set_a, set_b;
  ok = ok && publish(&state, image_a.image_id, kind, type, count, 3, &set_a) &&
       publish(&state, image_b.image_id, kind, type, count, 4, &set_b);
  Lardon3DProjectDbCandidatePair pair;
  ok = ok && lardon3d_project_db_create_candidate_pair(
                 db, image_a.image_id, image_b.image_id, 1, &pair) == LARDON3D_PROJECT_DB_OK;
  std::vector<Sample> samples;
  Lardon3DMatcherParams params = {matcher_kind, lardon3d_matcher_default_ratio(matcher_kind)};
  for (int repetition = 0; ok && repetition <= repetitions; ++repetition) {
    Lardon3DMatcherStats stats;
    params.ratio_threshold = lardon3d_matcher_default_ratio(matcher_kind) -
                             (float)repetition * 0.001F;
    Lardon3DProjectDbMatchResult result;
    ok = lardon3d_matcher_match_and_publish_profiled(
             root, db, &pair, &set_a, &set_b, &params, &result, &stats) == LARDON3D_MATCHER_OK;
    if (repetition > 0)
      samples.push_back({stats.feature_open_ns, stats.descriptor_read_ns, stats.knn_ns,
                         stats.filter_ns, stats.canonicalize_ns, stats.serialize_ns,
                         stats.sha256_ns, stats.publication_ns, stats.database_ns,
                         stats.total_ns});
  }
  if (ok) {
    std::vector<uint64_t> values;
    auto field = [&](auto getter) {
      values.clear();
      for (const Sample &sample : samples) values.push_back(getter(sample));
      return (double)median(values) / 1000000.0;
    };
    printf("%s,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", kind, count,
           field([](const Sample &s) { return s.open_ns; }),
           field([](const Sample &s) { return s.read_ns; }),
           field([](const Sample &s) { return s.knn_ns; }),
           field([](const Sample &s) { return s.filter_ns; }),
           field([](const Sample &s) { return s.canonicalize_ns; }),
           field([](const Sample &s) { return s.serialize_ns; }),
           field([](const Sample &s) { return s.sha_ns; }),
           field([](const Sample &s) { return s.publish_ns; }),
           field([](const Sample &s) { return s.database_ns; }),
           field([](const Sample &s) { return s.total_ns; }));
    std::vector<uint64_t> reuse_totals;
    for (int repetition = 0; repetition < 5; ++repetition) {
      Lardon3DMatcherStats reuse_stats;
      Lardon3DProjectDbMatchResult reused;
      ok = ok && lardon3d_matcher_match_and_publish_profiled(
                     root, db, &pair, &set_a, &set_b, &params, &reused, &reuse_stats) ==
                     LARDON3D_MATCHER_OK;
      reuse_totals.push_back(reuse_stats.total_ns);
    }
    printf("%s-reuse,%u,0,0,0,0,0,0,0,0,0,%.3f\n", kind, count,
           (double)median(reuse_totals) / 1000000.0);
  }
  lardon3d_project_db_close(db);
  std::filesystem::remove_all(root);
  return ok;
}

int main(int argc, char **argv) {
  int repetitions = argc > 3 ? atoi(argv[3]) : 7;
  int threads = argc > 4 ? atoi(argv[4]) : cv::getNumThreads();
  if (repetitions < 1 || threads < 1) return EXIT_FAILURE;
  cv::setNumThreads(threads);
  printf("kind,count,open_ms,read_ms,knn_ms,filter_ms,canonicalize_ms,serialize_ms,"
         "sha_ms,publish_ms,db_ms,total_ms\n");
  if (argc > 2) {
    uint32_t count = (uint32_t)strtoul(argv[2], nullptr, 10);
    if (strcmp(argv[1], "orb") == 0)
      return benchmark_case("orb", LARDON3D_MATCHER_ORB_BF,
                            LARDON3D_FEATURE_DESCRIPTOR_U8, count, repetitions)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    if (strcmp(argv[1], "sift") == 0 || strcmp(argv[1], "rootsift") == 0) {
      bool rootsift = strcmp(argv[1], "rootsift") == 0;
      return benchmark_case(argv[1], rootsift ? LARDON3D_MATCHER_ROOTSIFT_BF
                                              : LARDON3D_MATCHER_SIFT_BF,
                            LARDON3D_FEATURE_DESCRIPTOR_F32, count, repetitions)
                 ? EXIT_SUCCESS
                 : EXIT_FAILURE;
    }
    return EXIT_FAILURE;
  }
  const uint32_t sizes[] = {64, 256, 1024, 4096, 8192};
  for (uint32_t count : sizes) {
    if (!benchmark_case("orb", LARDON3D_MATCHER_ORB_BF,
                        LARDON3D_FEATURE_DESCRIPTOR_U8, count, repetitions) ||
        !benchmark_case("sift", LARDON3D_MATCHER_SIFT_BF,
                        LARDON3D_FEATURE_DESCRIPTOR_F32, count, repetitions) ||
        !benchmark_case("rootsift", LARDON3D_MATCHER_ROOTSIFT_BF,
                        LARDON3D_FEATURE_DESCRIPTOR_F32, count, repetitions))
      return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
