#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {
#include <lardon3d/feature_extractor.h>
}

#define CHECK(x)                                                                                 \
  do {                                                                                           \
    if (!(x)) {                                                                                  \
      std::fprintf(stderr, "precision benchmark failure line %d: %s\n", __LINE__, #x);           \
      return 1;                                                                                  \
    }                                                                                            \
  } while (0)

struct Metrics {
  size_t count{};
  double coverage{};
  double repeatability{};
  double median{};
  double p90{};
  double milliseconds{};
  double correct_matches{};
};

static cv::Mat fixture(bool low_texture = false) {
  cv::Mat image(384, 512, CV_8U, cv::Scalar(low_texture ? 118 : 28));
  cv::rectangle(image, {25, 25}, {230, 180}, cv::Scalar(220), 3);
  cv::circle(image, {145, 110}, 62, cv::Scalar(55), 5);
  cv::line(image, {18, 330}, {470, 205}, cv::Scalar(238), 4);
  cv::putText(image, "L3D", {275, 105}, cv::FONT_HERSHEY_SIMPLEX, 1.8, cv::Scalar(210), 4);
  for (int y = 220; y < 360; y += 12) for (int x = 45; x < 245; x += 12) {
    int value = ((x / 12 + y / 12) & 1) ? 195 : 58;
    cv::rectangle(image, {x, y}, {x + 7, y + 7}, cv::Scalar(value), cv::FILLED);
  }
  if (!low_texture) {
    cv::RNG rng(0x4c3344);
    cv::Mat noise(image.size(), CV_8U);
    rng.fill(noise, cv::RNG::UNIFORM, 0, 25);
    cv::add(image, noise, image);
  }
  return image;
}

static cv::Mat imbalanced_fixture() {
  cv::Mat image(384, 512, CV_8U, cv::Scalar(105));
  cv::RNG rng(0x53504154);
  cv::Mat textured(384, 300, CV_8U);
  rng.fill(textured, cv::RNG::UNIFORM, 10, 245);
  textured.copyTo(image(cv::Rect(0, 0, 300, 384)));
  cv::rectangle(image, {350, 55}, {470, 160}, cv::Scalar(225), 4);
  cv::circle(image, {410, 280}, 45, cv::Scalar(35), 5);
  return image;
}

static double fixed_coverage(const Lardon3DExtractedFeatures &features) {
  bool occupied[64]{};
  for (uint32_t i = 0; i < features.feature_count; ++i) {
    float width = static_cast<float>(features.image_width);
    float height = static_cast<float>(features.image_height);
    uint32_t column =
        std::min(7U, static_cast<uint32_t>(features.keypoints[i].x / width * 8.0F));
    uint32_t row = std::min(7U, static_cast<uint32_t>(features.keypoints[i].y / height * 8.0F));
    occupied[row * 8U + column] = true;
  }
  return static_cast<double>(std::count(std::begin(occupied), std::end(occupied), true)) / 64.0;
}

static std::string temporary_path(const char *suffix) {
  char pattern[] = "/tmp/lardon3d-precision-XXXXXX";
  int fd = mkstemp(pattern);
  if (fd >= 0) close(fd);
  std::string path = pattern;
  std::remove(path.c_str());
  return path + suffix;
}

static bool extract(const cv::Mat &image, bool sift, bool rootsift,
                    Lardon3DExtractedFeatures *features, double *milliseconds,
                    Lardon3DSiftExtractorParameters override_parameters = {}) {
  std::string path = temporary_path(".png");
  if (!cv::imwrite(path, image)) return false;
  auto begin = std::chrono::steady_clock::now();
  Lardon3DFeatureExtractResult result;
  if (sift) {
    Lardon3DSiftExtractorParameters parameters = override_parameters.max_features
                                                      ? override_parameters
                                                      : lardon3d_sift_precision_classic_v1(rootsift);
    parameters.rootsift = rootsift;
    result = lardon3d_feature_extract_sift(path.c_str(), &parameters, features);
  } else {
    Lardon3DFeatureExtractorParameters parameters{4096, 8, 12};
    result = lardon3d_feature_extract_orb(path.c_str(), &parameters, features);
  }
  auto end = std::chrono::steady_clock::now();
  *milliseconds = std::chrono::duration<double, std::milli>(end - begin).count();
  std::remove(path.c_str());
  return result == LARDON3D_FEATURE_EXTRACT_OK;
}

static Metrics compare(const Lardon3DExtractedFeatures &a, const Lardon3DExtractedFeatures &b,
                       const cv::Mat &homography, double elapsed) {
  std::vector<double> errors;
  for (uint32_t i = 0; i < a.feature_count; ++i) {
    cv::Matx31d source(a.keypoints[i].x, a.keypoints[i].y, 1.0);
    cv::Matx31d projected = cv::Matx33d(homography) * source;
    double px = projected(0) / projected(2), py = projected(1) / projected(2);
    double best = 3.0;
    for (uint32_t j = 0; j < b.feature_count; ++j) {
      double dx = b.keypoints[j].x - px, dy = b.keypoints[j].y - py;
      best = std::min(best, std::sqrt(dx * dx + dy * dy));
    }
    if (best < 3.0) errors.push_back(best);
  }
  std::sort(errors.begin(), errors.end());
  Metrics metrics;
  metrics.count = b.feature_count;
  metrics.coverage = b.quality.coverage_ratio;
  metrics.repeatability = a.feature_count ? static_cast<double>(errors.size()) / a.feature_count : 0;
  metrics.median = errors.empty() ? 0 : errors[errors.size() / 2];
  metrics.p90 = errors.empty() ? 0 : errors[(errors.size() * 9) / 10];
  metrics.milliseconds = elapsed;
  return metrics;
}

static double matching_correctness(const Lardon3DExtractedFeatures &a,
                                   const Lardon3DExtractedFeatures &b, const cv::Mat &homography,
                                   int norm) {
  if (!a.feature_count || !b.feature_count) return 0;
  int type = norm == cv::NORM_HAMMING ? CV_8U : CV_32F;
  int dimension = norm == cv::NORM_HAMMING ? 32 : 128;
  cv::Mat da(static_cast<int>(a.feature_count), dimension, type, a.descriptors);
  cv::Mat db(static_cast<int>(b.feature_count), dimension, type, b.descriptors);
  std::vector<std::vector<cv::DMatch>> matches;
  cv::BFMatcher(norm).knnMatch(da, db, matches, 2);
  size_t accepted = 0, correct = 0;
  for (const auto &pair : matches) if (pair.size() == 2 && pair[0].distance < 0.8F * pair[1].distance) {
    ++accepted;
    const auto &p = a.keypoints[static_cast<size_t>(pair[0].queryIdx)];
    const auto &q = b.keypoints[static_cast<size_t>(pair[0].trainIdx)];
    cv::Matx31d expected = cv::Matx33d(homography) * cv::Matx31d(p.x, p.y, 1.0);
    double dx = q.x - expected(0) / expected(2), dy = q.y - expected(1) / expected(2);
    correct += dx * dx + dy * dy <= 9.0;
  }
  return accepted ? static_cast<double>(correct) / static_cast<double>(accepted) : 0;
}

static cv::Mat affine_homography(const cv::Mat &affine) {
  cv::Mat h = cv::Mat::eye(3, 3, CV_64F);
  affine.copyTo(h(cv::Rect(0, 0, 3, 2)));
  return h;
}

int main() {
  Lardon3DSiftExtractorParameters valid_parameters = lardon3d_sift_precision_classic_v1(false);
  CHECK(lardon3d_sift_extractor_parameters_valid(&valid_parameters));
  Lardon3DSiftExtractorParameters invalid_parameters = valid_parameters;
  invalid_parameters.max_features = 0;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.octave_layers = 9;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.contrast_threshold = std::numeric_limits<double>::quiet_NaN();
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.edge_threshold = 101.0;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.sigma = 0.49;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.grid_rows = 0;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.grid_cols = 33;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  invalid_parameters = valid_parameters;
  invalid_parameters.max_features_per_cell = 0;
  CHECK(!lardon3d_sift_extractor_parameters_valid(&invalid_parameters));
  cv::Mat base = fixture();
  struct Transform { const char *name; cv::Mat image; cv::Mat h; };
  std::vector<Transform> transforms;
  for (double angle : {5.0, 15.0, 30.0}) {
    cv::Point2f center(static_cast<float>(base.cols) / 2.0F,
                       static_cast<float>(base.rows) / 2.0F);
    cv::Mat a = cv::getRotationMatrix2D(center, angle, 1.0);
    cv::Mat changed;
    cv::warpAffine(base, changed, a, base.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT);
    transforms.push_back({angle == 5 ? "rot5" : angle == 15 ? "rot15" : "rot30", changed,
                          affine_homography(a)});
  }
  for (double scale : {0.75, 1.25, 1.5}) {
    cv::Point2f center(static_cast<float>(base.cols) / 2.0F,
                       static_cast<float>(base.rows) / 2.0F);
    cv::Mat a = cv::getRotationMatrix2D(center, 0, scale);
    cv::Mat changed;
    cv::warpAffine(base, changed, a, base.size(), cv::INTER_LINEAR, cv::BORDER_REFLECT);
    transforms.push_back({scale < 1 ? "scale075" : scale < 1.4 ? "scale125" : "scale150",
                          changed, affine_homography(a)});
  }
  cv::Mat crop = base(cv::Rect(48, 36, 400, 300)).clone();
  cv::Mat crop_h = cv::Mat::eye(3, 3, CV_64F);
  crop_h.at<double>(0, 2) = -48.0;
  crop_h.at<double>(1, 2) = -36.0;
  transforms.push_back({"crop", crop, crop_h});
  cv::Mat illumination;
  base.convertTo(illumination, -1, 0.72, 32);
  transforms.push_back({"illumination", illumination, cv::Mat::eye(3, 3, CV_64F)});
  cv::Mat gamma_lut(1, 256, CV_8U);
  for (int value = 0; value < 256; ++value) {
    double normalized = static_cast<double>(value) / 255.0;
    gamma_lut.at<unsigned char>(value) =
        cv::saturate_cast<unsigned char>(std::pow(normalized, 0.8) * 255.0);
  }
  cv::Mat gamma;
  cv::LUT(base, gamma_lut, gamma);
  transforms.push_back({"gamma", gamma, cv::Mat::eye(3, 3, CV_64F)});
  cv::Mat blur;
  cv::GaussianBlur(base, blur, {5, 5}, 1.1);
  transforms.push_back({"blur", blur, cv::Mat::eye(3, 3, CV_64F)});
  transforms.push_back({"low_texture", fixture(true), cv::Mat::eye(3, 3, CV_64F)});

  for (int engine = 0; engine < 3; ++engine) {
    bool sift = engine != 0, rootsift = engine == 2;
    Lardon3DExtractedFeatures reference{};
    double reference_time = 0;
    CHECK(extract(base, sift, rootsift, &reference, &reference_time));
    CHECK(reference.feature_count > 0);
    for (const Transform &transform : transforms) {
      Lardon3DExtractedFeatures changed{};
      double elapsed = 0;
      CHECK(extract(transform.image, sift, rootsift, &changed, &elapsed));
      Metrics metrics = compare(reference, changed, transform.h, elapsed);
      metrics.correct_matches = matching_correctness(reference, changed, transform.h,
                                                      sift ? cv::NORM_L2 : cv::NORM_HAMMING);
      std::printf("BENCH %-8s %-12s count=%zu coverage=%.3f repeat=%.3f median=%.3f "
                  "p90=%.3f correct=%.3f ms=%.2f\n",
                  engine == 0 ? "ORB" : rootsift ? "RootSIFT" : "SIFT", transform.name,
                  metrics.count, metrics.coverage, metrics.repeatability, metrics.median,
                  metrics.p90, metrics.correct_matches, metrics.milliseconds);
      CHECK(metrics.count > 0 && metrics.repeatability > 0.05 && metrics.p90 <= 3.0);
      lardon3d_extracted_features_destroy(&changed);
    }
    lardon3d_extracted_features_destroy(&reference);
  }
  Lardon3DExtractedFeatures sift_reference{}, root_reference{};
  double reference_elapsed = 0;
  CHECK(extract(base, true, false, &sift_reference, &reference_elapsed) &&
        extract(base, true, true, &root_reference, &reference_elapsed) &&
        sift_reference.feature_count == root_reference.feature_count);
  for (uint32_t point = 0; point < sift_reference.feature_count; ++point) {
    CHECK(std::memcmp(&sift_reference.keypoints[point], &root_reference.keypoints[point],
                      sizeof(Lardon3DFeatureKeypoint)) == 0);
  }
  lardon3d_extracted_features_destroy(&sift_reference);
  lardon3d_extracted_features_destroy(&root_reference);
  Lardon3DSiftExtractorParameters raw = lardon3d_sift_precision_classic_v1(false);
  raw.grid_rows = raw.grid_cols = 1;
  raw.max_features = 128;
  raw.max_features_per_cell = raw.max_features;
  Lardon3DSiftExtractorParameters grid = lardon3d_sift_precision_classic_v1(false);
  grid.max_features = 128;
  grid.max_features_per_cell = 2;
  Lardon3DExtractedFeatures unbalanced{}, balanced{}, low{};
  double elapsed = 0;
  cv::Mat imbalanced = imbalanced_fixture();
  CHECK(extract(imbalanced, true, false, &unbalanced, &elapsed, raw));
  CHECK(extract(imbalanced, true, false, &balanced, &elapsed, grid));
  CHECK(extract(fixture(true), true, false, &low, &elapsed));
  Lardon3DExtractedFeatures uniform{};
  CHECK(extract(cv::Mat(192, 192, CV_8U, cv::Scalar(127)), true, false, &uniform, &elapsed) &&
        uniform.feature_count == 0 && uniform.quality.occupied_cells == 0);
  double raw_coverage = fixed_coverage(unbalanced);
  double selected_coverage = fixed_coverage(balanced);
  cv::Point2f imbalanced_center(static_cast<float>(imbalanced.cols) / 2.0F,
                                static_cast<float>(imbalanced.rows) / 2.0F);
  cv::Mat grid_affine = cv::getRotationMatrix2D(imbalanced_center, 15.0, 1.0);
  cv::Mat grid_rotated;
  cv::warpAffine(imbalanced, grid_rotated, grid_affine, imbalanced.size(), cv::INTER_LINEAR,
                 cv::BORDER_REFLECT);
  Lardon3DExtractedFeatures raw_rotated{}, grid_rotated_features{};
  CHECK(extract(grid_rotated, true, false, &raw_rotated, &elapsed, raw) &&
        extract(grid_rotated, true, false, &grid_rotated_features, &elapsed, grid));
  Metrics raw_grid_metrics = compare(unbalanced, raw_rotated, affine_homography(grid_affine), 0);
  Metrics selected_grid_metrics =
      compare(balanced, grid_rotated_features, affine_homography(grid_affine), 0);
  CHECK(balanced.quality.occupied_cells > 0 && low.feature_count > 0 &&
        selected_coverage > raw_coverage &&
        selected_grid_metrics.repeatability >= raw_grid_metrics.repeatability * 0.5);
  std::printf("BENCH grid raw_coverage=%.3f selected_coverage=%.3f raw_repeat=%.3f "
              "selected_repeat=%.3f low_texture=%u\n",
              raw_coverage, selected_coverage, raw_grid_metrics.repeatability,
              selected_grid_metrics.repeatability, low.feature_count);
  lardon3d_extracted_features_destroy(&unbalanced);
  lardon3d_extracted_features_destroy(&balanced);
  lardon3d_extracted_features_destroy(&low);
  lardon3d_extracted_features_destroy(&uniform);
  lardon3d_extracted_features_destroy(&raw_rotated);
  lardon3d_extracted_features_destroy(&grid_rotated_features);
  return 0;
}
