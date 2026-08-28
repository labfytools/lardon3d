#include <lardon3d/photo_quality.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                                 \
      return 1;                                                                 \
    }                                                                           \
  } while (0)

static cv::Mat checker(int size) {
  cv::Mat image(size, size, CV_8UC1);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x)
      image.at<unsigned char>(y, x) = ((x / (size / 16)) + (y / (size / 16))) % 2 ? 220 : 30;
  return image;
}

int main() {
  char directory_template[] = "/tmp/lardon3d-photo-quality-XXXXXX";
  char *directory = mkdtemp(directory_template);
  CHECK(directory != nullptr);
  const std::string sharp_path = std::string(directory) + "/sharp.jpg";
  const std::string blur_path = std::string(directory) + "/blur.jpg";
  const std::string large_path = std::string(directory) + "/large.jpg";
  const std::string white_path = std::string(directory) + "/white.jpg";
  const std::string oversized_path = std::string(directory) + "/oversized.jpg";
  const std::string truncated_oversized_path =
      std::string(directory) + "/truncated-oversized.jpg";
  const std::string over_budget_oversized_path =
      std::string(directory) + "/over-budget-oversized.jpg";
  const std::string malformed_path = std::string(directory) + "/malformed.jpg";

  cv::Mat sharp = checker(512);
  cv::Mat blurred;
  cv::GaussianBlur(sharp, blurred, cv::Size(31, 31), 8.0);
  cv::Mat large;
  cv::resize(sharp, large, cv::Size(1024, 1024), 0.0, 0.0, cv::INTER_NEAREST);
  cv::Mat white(512, 512, CV_8UC1, cv::Scalar(255));
  CHECK(cv::imwrite(sharp_path, sharp));
  CHECK(cv::imwrite(blur_path, blurred));
  CHECK(cv::imwrite(large_path, large));
  CHECK(cv::imwrite(white_path, white));
  cv::Mat oversized(2, LARDON3D_PHOTO_QUALITY_JPEG_MAX_DIMENSION + 1, CV_8UC1,
                     cv::Scalar(127));
  CHECK(cv::imwrite(oversized_path, oversized));
  {
    std::FILE *over_budget = std::fopen(over_budget_oversized_path.c_str(), "wb");
    CHECK(over_budget != nullptr);
    const unsigned char header[] = {
        0xff, 0xd8,              // SOI
        0xff, 0xc0, 0x00, 0x08, 0x08, 0x00, 0x02, 0x20, 0x01, 0x00,
        0xff, 0xda, 0x00, 0x02  // SOS with an empty synthetic header.
    };
    CHECK(std::fwrite(header, 1, sizeof(header), over_budget) == sizeof(header));
    unsigned char entropy[64 * 1024];
    std::memset(entropy, 0x11, sizeof(entropy));
    /* The validator must reject finite-but-excessive structural work without
     * allocating the oversized raster or treating the parser cap as science. */
    for (size_t i = 0; i < 1025; ++i)
      CHECK(std::fwrite(entropy, 1, sizeof(entropy), over_budget) == sizeof(entropy));
    CHECK(std::fclose(over_budget) == 0);
  }
  {
    std::FILE *source = std::fopen(oversized_path.c_str(), "rb");
    std::FILE *truncated = std::fopen(truncated_oversized_path.c_str(), "wb");
    CHECK(source != nullptr && truncated != nullptr);
    unsigned char bytes[128];
    const size_t count = std::fread(bytes, 1, sizeof(bytes), source);
    CHECK(count > 16 && std::fwrite(bytes, 1, count, truncated) == count);
    CHECK(std::fclose(source) == 0 && std::fclose(truncated) == 0);
  }
  {
    std::FILE *malformed = std::fopen(malformed_path.c_str(), "wb");
    CHECK(malformed != nullptr);
    CHECK(std::fwrite("not-a-jpeg", 1, 10, malformed) == 10);
    CHECK(std::fclose(malformed) == 0);
  }

  Lardon3DPhotoQualityMetrics sharp_metrics{};
  Lardon3DPhotoQualityMetrics blur_metrics{};
  Lardon3DPhotoQualityMetrics large_metrics{};
  Lardon3DPhotoQualityMetrics white_metrics{};
  CHECK(lardon3d_photo_quality_analyze_jpeg(sharp_path.c_str(), &sharp_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_OK);
  CHECK(lardon3d_photo_quality_analyze_jpeg(blur_path.c_str(), &blur_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_OK);
  CHECK(sharp_metrics.sharpness_normalized > blur_metrics.sharpness_normalized * 4.0);
  CHECK(lardon3d_photo_quality_analyze_jpeg(white_path.c_str(), &white_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_OK);
  CHECK(white_metrics.clipped_white_fraction > 0.99);
  CHECK(white_metrics.recommendation == LARDON3D_PHOTO_QUALITY_REJECT);

  CHECK(lardon3d_photo_quality_analyze_jpeg(large_path.c_str(), &large_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_OK);
  const double scale_ratio = sharp_metrics.sharpness_normalized /
                             large_metrics.sharpness_normalized;
  CHECK(scale_ratio > 0.8 && scale_ratio < 1.25);

  /* An oversized SOF becomes pending evidence before OpenCV can allocate its
   * raster. The operational ceiling is neither a decode error nor rejection. */
  Lardon3DPhotoQualityMetrics oversized_metrics{};
  CHECK(lardon3d_photo_quality_analyze_jpeg(oversized_path.c_str(), &oversized_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE);
  CHECK(oversized_metrics.recommendation == LARDON3D_PHOTO_QUALITY_SUSPECT);
  CHECK(std::strstr(oversized_metrics.reasons, "DIMENSIONS_EXCEED") != nullptr);
  CHECK(!lardon3d_photo_quality_effective_include(oversized_metrics.recommendation,
                                                   LARDON3D_PHOTO_QUALITY_OVERRIDE_NONE));

  /* An oversized SOF does not suppress structural errors after that header. */
  Lardon3DPhotoQualityMetrics truncated_oversized_metrics{};
  CHECK(lardon3d_photo_quality_analyze_jpeg(truncated_oversized_path.c_str(),
                                             &truncated_oversized_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR);
  CHECK(std::strcmp(truncated_oversized_metrics.reasons, "JPEG_DECODE_ERROR") == 0);

  Lardon3DPhotoQualityMetrics over_budget_oversized_metrics{};
  CHECK(lardon3d_photo_quality_analyze_jpeg(over_budget_oversized_path.c_str(),
                                             &over_budget_oversized_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR);
  CHECK(std::strcmp(over_budget_oversized_metrics.reasons, "JPEG_DECODE_ERROR") == 0);

  /* Malformed input is a decode error and remains observably distinct from a
   * structurally valid JPEG rejected only by the pre-allocation proxy bound. */
  Lardon3DPhotoQualityMetrics malformed_metrics{};
  CHECK(lardon3d_photo_quality_analyze_jpeg(malformed_path.c_str(), &malformed_metrics) ==
        LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR);
  CHECK(malformed_metrics.recommendation == LARDON3D_PHOTO_QUALITY_REJECT);
  CHECK(std::strcmp(malformed_metrics.reasons, "JPEG_DECODE_ERROR") == 0);

  /* Reversing analysis order must not leak state or alter deterministic output. */
  Lardon3DPhotoQualityMetrics second_blur{};
  Lardon3DPhotoQualityMetrics second_sharp{};
  CHECK(lardon3d_photo_quality_analyze_jpeg(blur_path.c_str(), &second_blur) ==
        LARDON3D_PHOTO_QUALITY_METRIC_OK);
  CHECK(lardon3d_photo_quality_analyze_jpeg(sharp_path.c_str(), &second_sharp) ==
        LARDON3D_PHOTO_QUALITY_METRIC_OK);
  CHECK(second_blur.sharpness_normalized == blur_metrics.sharpness_normalized);
  CHECK(second_sharp.sharpness_normalized == sharp_metrics.sharpness_normalized);

  Lardon3DPhotoQualityMetrics raw_only{};
  lardon3d_photo_quality_raw_only(&raw_only);
  CHECK(raw_only.status == LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE);
  CHECK(std::strstr(raw_only.reasons, "REQUIRES_JPEG_PROXY") != nullptr);
  CHECK(!lardon3d_photo_quality_effective_include(LARDON3D_PHOTO_QUALITY_SUSPECT,
                                                  LARDON3D_PHOTO_QUALITY_OVERRIDE_NONE));
  CHECK(lardon3d_photo_quality_effective_include(LARDON3D_PHOTO_QUALITY_REJECT,
                                                 LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE));
  CHECK(!lardon3d_photo_quality_effective_include(LARDON3D_PHOTO_QUALITY_GOOD,
                                                  LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE));

  std::filesystem::remove_all(directory);
  return 0;
}
