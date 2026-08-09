#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <opencv2/calib3d.hpp>
#include <opencv2/core/version.hpp>
#include <string>
#include <vector>

namespace {

struct Generator {
  uint64_t state;

  uint32_t next_u32() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return static_cast<uint32_t>((state * 0x2545f4914f6cdd1dULL) >> 32);
  }

  double uniform() { return static_cast<double>(next_u32()) / 4294967296.0; }

  double normal() {
    const double u1 = std::max(uniform(), 1e-12);
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * CV_PI * u2);
  }
};

enum class Geometry {
  Healthy,
  WeakBaseline,
  WideBaseline,
  Concentrated,
  NearCollinear,
  Planar,
  RotationDominant,
  Duplicated,
};

struct Scenario {
  const char *name;
  Geometry geometry;
  int width;
  int height;
  int count;
  double noise;
  double outlier_ratio;
};

struct Corpus {
  std::vector<cv::Point2d> first;
  std::vector<cv::Point2d> second;
  std::vector<unsigned char> truth;
  cv::Mat fundamental;
};

struct Measurement {
  bool model_found = false;
  double precision = 0.0;
  double recall = 0.0;
  double median_sampson = 0.0;
  double elapsed_ms = 0.0;
  double median_ms = 0.0;
  double p95_ms = 0.0;
  double worst_ms = 0.0;
  uint64_t output_hash = 0;
  int inlier_count = 0;
};

enum class Method {
  Classic,
  UsacDefault,
  UsacMagsac,
  UsacAccurate,
  SeededMagsac,
};

const char *method_name(Method method) {
  switch (method) {
  case Method::Classic:
    return "FM_RANSAC";
  case Method::UsacDefault:
    return "USAC_DEFAULT";
  case Method::UsacMagsac:
    return "USAC_MAGSAC";
  case Method::UsacAccurate:
    return "USAC_ACCURATE";
  case Method::SeededMagsac:
    return "USAC_MAGSAC_LOCAL_SEED";
  }
  return "UNKNOWN";
}

cv::Mat rotation_y(double angle) {
  return (cv::Mat_<double>(3, 3) << std::cos(angle), 0.0, std::sin(angle), 0.0,
          1.0, 0.0, -std::sin(angle), 0.0, std::cos(angle));
}

cv::Mat skew(const cv::Vec3d &translation) {
  return (cv::Mat_<double>(3, 3) << 0.0, -translation[2], translation[1],
          translation[2], 0.0, -translation[0], -translation[1], translation[0],
          0.0);
}

cv::Point2d project(const cv::Mat &rotation, const cv::Vec3d &translation,
                    const cv::Vec3d &point, double focal, double cx,
                    double cy) {
  cv::Mat transformed = rotation * cv::Mat(point) + cv::Mat(translation);
  const double x = transformed.at<double>(0) / transformed.at<double>(2);
  const double y = transformed.at<double>(1) / transformed.at<double>(2);
  return {focal * x + cx, focal * y + cy};
}

Corpus generate(const Scenario &scenario, uint64_t seed) {
  Generator generator{seed};
  const double focal =
      0.85 * static_cast<double>(std::max(scenario.width, scenario.height));
  const double cx = 0.5 * scenario.width;
  const double cy = 0.5 * scenario.height;
  double baseline = 0.35;
  double angle = 0.08;
  if (scenario.geometry == Geometry::WeakBaseline)
    baseline = 0.025;
  if (scenario.geometry == Geometry::WideBaseline)
    baseline = 1.0;
  if (scenario.geometry == Geometry::RotationDominant) {
    baseline = 0.005;
    angle = 0.25;
  }
  const cv::Mat rotation = rotation_y(angle);
  const cv::Vec3d translation(baseline, 0.015, 0.01);
  const cv::Mat intrinsic =
      (cv::Mat_<double>(3, 3) << focal, 0.0, cx, 0.0, focal, cy, 0.0, 0.0, 1.0);
  const cv::Mat fundamental =
      intrinsic.inv().t() * skew(translation) * rotation * intrinsic.inv();

  Corpus corpus;
  corpus.fundamental = fundamental;
  corpus.first.reserve(scenario.count);
  corpus.second.reserve(scenario.count);
  corpus.truth.assign(scenario.count, 1);
  for (int index = 0; index < scenario.count; ++index) {
    double x = 3.0 * (generator.uniform() - 0.5);
    double y = 2.0 * (generator.uniform() - 0.5);
    double z = 4.0 + 5.0 * generator.uniform();
    if (scenario.geometry == Geometry::Concentrated) {
      x *= 0.12;
      y *= 0.12;
    } else if (scenario.geometry == Geometry::NearCollinear) {
      y = 0.015 * x + 0.002 * generator.normal();
    } else if (scenario.geometry == Geometry::Planar) {
      z = 6.0;
    } else if (scenario.geometry == Geometry::Duplicated &&
               index > scenario.count / 2) {
      x = 0.1;
      y = -0.1;
      z = 6.0;
    }
    const cv::Vec3d point(x, y, z);
    cv::Point2d first =
        project(cv::Mat::eye(3, 3, CV_64F), cv::Vec3d(), point, focal, cx, cy);
    cv::Point2d second = project(rotation, translation, point, focal, cx, cy);
    first.x += scenario.noise * generator.normal();
    first.y += scenario.noise * generator.normal();
    second.x += scenario.noise * generator.normal();
    second.y += scenario.noise * generator.normal();
    corpus.first.push_back(first);
    corpus.second.push_back(second);
  }
  const int outlier_count =
      static_cast<int>(std::lround(scenario.count * scenario.outlier_ratio));
  for (int index = 0; index < outlier_count; ++index) {
    const int target = scenario.count - 1 - index;
    corpus.second[target] = {scenario.width * generator.uniform(),
                             scenario.height * generator.uniform()};
    corpus.truth[target] = 0;
  }
  return corpus;
}

double sampson(const cv::Mat &fundamental, const cv::Point2d &first,
               const cv::Point2d &second) {
  const cv::Mat x1 = (cv::Mat_<double>(3, 1) << first.x, first.y, 1.0);
  const cv::Mat x2 = (cv::Mat_<double>(3, 1) << second.x, second.y, 1.0);
  const cv::Mat line2 = fundamental * x1;
  const cv::Mat line1 = fundamental.t() * x2;
  const double residual = x2.dot(line2);
  const double denominator = line1.at<double>(0) * line1.at<double>(0) +
                             line1.at<double>(1) * line1.at<double>(1) +
                             line2.at<double>(0) * line2.at<double>(0) +
                             line2.at<double>(1) * line2.at<double>(1);
  return denominator > 0.0 ? residual * residual / denominator : INFINITY;
}

uint64_t hash_output(const cv::Mat &model, const cv::Mat &mask) {
  uint64_t hash = 1469598103934665603ULL;
  auto mix = [&](const unsigned char *bytes, size_t size) {
    for (size_t index = 0; index < size; ++index) {
      hash ^= bytes[index];
      hash *= 1099511628211ULL;
    }
  };
  if (!model.empty()) {
    cv::Mat doubles;
    model.convertTo(doubles, CV_64F);
    mix(doubles.ptr<unsigned char>(), doubles.total() * doubles.elemSize());
  }
  if (!mask.empty())
    mix(mask.ptr<unsigned char>(), mask.total() * mask.elemSize());
  return hash;
}

cv::Mat estimate(Method method, const std::vector<cv::Point2d> &first,
                 const std::vector<cv::Point2d> &second, double threshold,
                 int seed, cv::Mat &mask) {
  if (method == Method::SeededMagsac) {
    cv::UsacParams params;
    params.confidence = 0.999;
    params.maxIterations = 5000;
    params.randomGeneratorState = seed;
    params.sampler = cv::SAMPLING_UNIFORM;
    params.score = cv::SCORE_METHOD_MAGSAC;
    params.threshold = threshold;
    params.isParallel = false;
    return cv::findFundamentalMat(first, second, mask, params);
  }
  int flag = cv::FM_RANSAC;
  if (method == Method::UsacDefault)
    flag = cv::USAC_DEFAULT;
  if (method == Method::UsacMagsac)
    flag = cv::USAC_MAGSAC;
  if (method == Method::UsacAccurate)
    flag = cv::USAC_ACCURATE;
  return cv::findFundamentalMat(first, second, flag, threshold, 0.999, 5000,
                                mask);
}

Measurement measure(Method method, const Corpus &corpus, double threshold,
                    int seed, bool use_float) {
  cv::Mat mask;
  const auto start = std::chrono::steady_clock::now();
  cv::Mat model;
  try {
    if (use_float) {
      std::vector<cv::Point2f> first(corpus.first.begin(), corpus.first.end());
      std::vector<cv::Point2f> second(corpus.second.begin(),
                                      corpus.second.end());
      model =
          estimate(method, std::vector<cv::Point2d>(first.begin(), first.end()),
                   std::vector<cv::Point2d>(second.begin(), second.end()),
                   threshold, seed, mask);
    } else {
      model =
          estimate(method, corpus.first, corpus.second, threshold, seed, mask);
    }
  } catch (const cv::Exception &) {
    model.release();
    mask.release();
  }
  const auto stop = std::chrono::steady_clock::now();
  Measurement result;
  result.elapsed_ms =
      std::chrono::duration<double, std::milli>(stop - start).count();
  result.model_found =
      model.rows == 3 && model.cols == 3 && mask.total() == corpus.truth.size();
  result.output_hash = hash_output(model, mask);
  if (!result.model_found)
    return result;
  int true_positive = 0;
  int false_positive = 0;
  int false_negative = 0;
  std::vector<double> errors;
  for (size_t index = 0; index < corpus.truth.size(); ++index) {
    const bool selected = mask.ptr<unsigned char>()[index] != 0;
    if (selected)
      ++result.inlier_count;
    if (selected && corpus.truth[index])
      ++true_positive;
    if (selected && !corpus.truth[index])
      ++false_positive;
    if (!selected && corpus.truth[index])
      ++false_negative;
    if (corpus.truth[index])
      errors.push_back(
          sampson(model, corpus.first[index], corpus.second[index]));
  }
  result.precision = true_positive + false_positive
                         ? static_cast<double>(true_positive) /
                               (true_positive + false_positive)
                         : 0.0;
  result.recall = true_positive + false_negative
                      ? static_cast<double>(true_positive) /
                            (true_positive + false_negative)
                      : 0.0;
  std::sort(errors.begin(), errors.end());
  result.median_sampson = errors.empty() ? 0.0 : errors[errors.size() / 2];
  return result;
}

void print_measurement(const Scenario &scenario, Method method,
                       double threshold, const Measurement &measurement,
                       bool repeatable, const char *precision) {
  std::printf(
      "%s,%s,%s,%d,%.2f,%.2f,%.2f,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s\n",
      scenario.name, method_name(method), precision, scenario.count,
      scenario.noise, scenario.outlier_ratio, threshold,
      measurement.model_found ? 1 : 0, measurement.precision,
      measurement.recall, measurement.median_sampson, measurement.median_ms,
      measurement.p95_ms, measurement.worst_ms, repeatable ? "yes" : "no");
}

} // namespace

int main(int argc, char **argv) {
  const std::array<Scenario, 20> scenarios = {
      {{"healthy_exact", Geometry::Healthy, 1920, 1080, 256, 0.0, 0.0},
       {"healthy_low", Geometry::Healthy, 1920, 1080, 256, 0.35, 0.1},
       {"healthy_medium", Geometry::Healthy, 1920, 1080, 1024, 0.75, 0.3},
       {"healthy_high", Geometry::Healthy, 4000, 3000, 1024, 1.5, 0.5},
       {"outliers_70", Geometry::Healthy, 1920, 1080, 1024, 0.75, 0.7},
       {"weak_baseline", Geometry::WeakBaseline, 1920, 1080, 256, 0.5, 0.3},
       {"wide_baseline", Geometry::WideBaseline, 4000, 3000, 256, 0.75, 0.3},
       {"concentrated", Geometry::Concentrated, 1920, 1080, 256, 0.5, 0.3},
       {"near_collinear", Geometry::NearCollinear, 1920, 1080, 256, 0.35, 0.3},
       {"planar", Geometry::Planar, 1920, 1080, 256, 0.5, 0.3},
       {"rotation_dominant", Geometry::RotationDominant, 1920, 1080, 256, 0.5,
        0.3},
       {"minimum_7", Geometry::Healthy, 1280, 720, 7, 0.0, 0.0},
       {"minimum_8", Geometry::Healthy, 1280, 720, 8, 0.0, 0.0},
       {"minimum_15", Geometry::Healthy, 1280, 720, 15, 0.5, 0.3},
       {"small_16", Geometry::Healthy, 1280, 720, 16, 0.5, 0.3},
       {"all_false", Geometry::Healthy, 1920, 1080, 256, 0.5, 1.0},
       {"duplicates", Geometry::Duplicated, 1920, 1080, 256, 0.35, 0.3},
       {"large_4096", Geometry::Healthy, 4000, 3000, 4096, 0.75, 0.5},
       {"large_8192", Geometry::Healthy, 4000, 3000, 8192, 0.75, 0.7}}};
  const std::array<Method, 5> methods = {
      Method::Classic, Method::UsacDefault, Method::UsacMagsac,
      Method::UsacAccurate, Method::SeededMagsac};
  if (argc == 2 && std::strcmp(argv[1], "--params") == 0) {
    const cv::UsacParams params;
    std::printf("parallel=%d lo_iterations=%d lo_method=%d lo_sample=%d "
                "neighbors=%d sampler=%d "
                "score=%d polisher=%d polisher_iterations=%d\n",
                params.isParallel ? 1 : 0, params.loIterations, params.loMethod,
                params.loSampleSize, params.neighborsSearch, params.sampler,
                params.score, params.final_polisher,
                params.final_polisher_iterations);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--determinism-only") == 0) {
    const Scenario difficult{
        "cross_process", Geometry::Healthy, 4000, 3000, 1024, 0.75, 0.7};
    const Corpus corpus = generate(difficult, 0x6c6172646f6eULL);
    for (Method method : methods) {
      const Measurement result =
          measure(method, corpus, 1.5, 0x4c334431, false);
      std::printf("%s,%016llx\n", method_name(method),
                  static_cast<unsigned long long>(result.output_hash));
    }
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--acceptance-only") == 0) {
    const std::array<Scenario, 8> acceptance = {
        {{"good_64", Geometry::Healthy, 1920, 1080, 64, 0.75, 0.3},
         {"good_256", Geometry::Healthy, 1920, 1080, 256, 0.75, 0.5},
         {"good_1024", Geometry::Healthy, 1920, 1080, 1024, 0.75, 0.7},
         {"false_64", Geometry::Healthy, 1920, 1080, 64, 0.75, 1.0},
         {"false_256", Geometry::Healthy, 1920, 1080, 256, 0.75, 1.0},
         {"false_1024", Geometry::Healthy, 1920, 1080, 1024, 0.75, 1.0},
         {"false_4096", Geometry::Healthy, 4000, 3000, 4096, 0.75, 1.0},
         {"weak_256", Geometry::WeakBaseline, 1920, 1080, 256, 0.75, 0.5}}};
    for (const Scenario &scenario : acceptance) {
      const Corpus corpus = generate(scenario, 0x6c6172646f6eULL);
      const Measurement result =
          measure(Method::SeededMagsac, corpus, 1.5, 0x4c334431, false);
      std::printf("%s,count=%d,ratio=%.6f,precision=%.6f,recall=%.6f\n",
                  scenario.name, result.inlier_count,
                  static_cast<double>(result.inlier_count) / scenario.count,
                  result.precision, result.recall);
    }
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--rank-only") == 0) {
    const Scenario scenario{"rank", Geometry::Healthy, 4000, 3000, 8192, 0.75,
                            0.7};
    const Corpus corpus = generate(scenario, 0x6c6172646f6eULL);
    cv::Mat mask;
    cv::Mat model = estimate(Method::SeededMagsac, corpus.first, corpus.second,
                             1.5, 0x4c334431, mask);
    cv::Mat singular_values;
    cv::SVD::compute(model, singular_values);
    std::printf("singular_values=%.17g,%.17g,%.17g\n",
                singular_values.at<double>(0), singular_values.at<double>(1),
                singular_values.at<double>(2));
    return 0;
  }
  std::printf(
      "# corpus=lardon3d-geometric-v1 compiler=%s opencv=%s threads=%d seed=%d "
      "confidence=0.999 max_iterations=5000\n",
      __VERSION__, CV_VERSION, cv::getNumThreads(), 0x4c334431);
  std::printf("scenario,algorithm,points,count,noise,outliers,threshold,found,"
              "precision,recall,"
              "median_sampson,median_ms,p95_ms,worst_ms,repeatable_32x\n");
  for (const Scenario &scenario : scenarios) {
    const Corpus corpus = generate(scenario, 0x6c6172646f6eULL);
    for (Method method : methods) {
      const Measurement first = measure(method, corpus, 1.5, 0x4c334431, false);
      Measurement summary = first;
      bool repeatable = true;
      std::vector<double> timings;
      timings.reserve(32);
      for (int repetition = 0; repetition < 32; ++repetition) {
        const Measurement repeated =
            measure(method, corpus, 1.5, 0x4c334431, false);
        repeatable = repeatable && repeated.output_hash == first.output_hash;
        timings.push_back(repeated.elapsed_ms);
      }
      std::sort(timings.begin(), timings.end());
      summary.median_ms = timings[timings.size() / 2];
      summary.p95_ms = timings[30];
      summary.worst_ms = timings.back();
      print_measurement(scenario, method, 1.5, summary, repeatable, "f64");
    }
  }
  const Scenario tuning{
      "threshold_tuning", Geometry::Healthy, 1920, 1080, 1024, 0.75, 0.5};
  const Corpus tuning_corpus = generate(tuning, 0x6c6172646f6eULL);
  for (double threshold : {0.5, 1.0, 1.5, 2.0, 3.0}) {
    const Measurement value = measure(Method::SeededMagsac, tuning_corpus,
                                      threshold, 0x4c334431, false);
    Measurement summary = value;
    summary.median_ms = value.elapsed_ms;
    summary.p95_ms = value.elapsed_ms;
    summary.worst_ms = value.elapsed_ms;
    print_measurement(tuning, Method::SeededMagsac, threshold, summary, true,
                      "f64");
  }
  for (bool use_float : {true, false}) {
    const Measurement value = measure(Method::SeededMagsac, tuning_corpus, 1.5,
                                      0x4c334431, use_float);
    Measurement summary = value;
    summary.median_ms = value.elapsed_ms;
    summary.p95_ms = value.elapsed_ms;
    summary.worst_ms = value.elapsed_ms;
    print_measurement(tuning, Method::SeededMagsac, 1.5, summary, true,
                      use_float ? "f32" : "f64");
  }
  return 0;
}
