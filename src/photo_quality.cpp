#include <lardon3d/photo_quality.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

/* Metrics v1 is a deterministic engineering recommendation policy calibrated
 * with synthetic fixtures and real Sony A6000 / Samsung S21 captures. These
 * cutoffs are not universal scientific validity criteria. Any value or mapping
 * change requires a new metrics/policy version and corresponding cache or
 * fingerprint invalidation rather than silently reinterpreting stored rows. */
constexpr double kSharpnessVeryLow = 0.00018;  // REJECT: severe normalized blur.
constexpr double kSharpnessLow = 0.00045;      // SUSPECT: review normalized blur.
constexpr unsigned char kBlackSampleMaximum = 5;  // Prototype-compatible black sample.
constexpr unsigned char kWhiteSampleMinimum = 250;  // Prototype-compatible white sample.
constexpr double kWhiteClippingSuspect = 0.08;  // SUSPECT above 8% white samples.
constexpr double kWhiteClippingSevere = 0.20;   // REJECT above 20% white samples.
constexpr double kBlackClippingSuspect = 0.15;  // SUSPECT above 15% black samples.
constexpr double kBlackClippingSevere = 0.35;   // REJECT above 35% black samples.
constexpr double kContrastLow = 0.025;  // SUSPECT below normalized standard deviation.
constexpr double kLowTextureFraction = 0.985;  // SUSPECT above low-gradient fraction.
constexpr double kSobelLowTextureMagnitude = 0.035;  // Low-gradient normalized cutoff.
/* Structural validation is deliberately bounded independently of JPEG pixel
 * dimensions. This is parser resource admission, not a scientific image-size
 * limit: every byte read or skipped consumes the same finite work budget. */
constexpr uint64_t kJpegStructuralByteLimit = 64ULL * 1024ULL * 1024ULL;

bool jpeg_dimensions(const char *path, uint32_t &width, uint32_t &height) {
  std::FILE *file = std::fopen(path, "rb");
  if (!file)
    return false;
  const auto close = [&file]() { std::fclose(file); };
  uint64_t structural_bytes = 0;
  const auto read_byte = [&file, &structural_bytes]() {
    if (structural_bytes == kJpegStructuralByteLimit)
      return EOF;
    const int value = std::fgetc(file);
    if (value != EOF)
      ++structural_bytes;
    return value;
  };
  const auto read_exact = [&file, &structural_bytes](void *destination, size_t count) {
    if (count > kJpegStructuralByteLimit - structural_bytes)
      return false;
    const size_t actual = std::fread(destination, 1, count, file);
    structural_bytes += actual;
    return actual == count;
  };
  const auto skip = [&file, &structural_bytes](uint64_t count) {
    if (count > kJpegStructuralByteLimit - structural_bytes || count > LONG_MAX)
      return false;
    if (std::fseek(file, static_cast<long>(count), SEEK_CUR) != 0)
      return false;
    structural_bytes += count;
    return true;
  };
  if (read_byte() != 0xff || read_byte() != 0xd8) {
    close();
    return false;
  }
  bool have_dimensions = false;
  bool in_entropy = false;
  int marker = -1;
  for (;;) {
    if (marker < 0) {
      int prefix = read_byte();
      if (in_entropy) {
        while (prefix != EOF) {
          if (prefix != 0xff) {
            prefix = read_byte();
            continue;
          }
          marker = read_byte();
          while (marker == 0xff)
            marker = read_byte();
          if (marker == 0x00 || (marker >= 0xd0 && marker <= 0xd7)) {
            prefix = read_byte();
            continue;
          }
          break;
        }
      } else {
        if (prefix != 0xff) {
          close();
          return false;
        }
        marker = read_byte();
      }
    }
    while (marker == 0xff)
      marker = read_byte();
    if (marker == EOF || marker == 0x00 || marker == 0xd8 ||
        (!in_entropy && marker >= 0xd0 && marker <= 0xd7)) {
      close();
      return false;
    }
    in_entropy = false;
    if (marker == 0xd9) {
      const int trailing = read_byte();
      close();
      return have_dimensions && trailing == EOF;
    }
    if (marker == 0x01) {
      marker = -1;
      continue;
    }
    int high = read_byte();
    int low = read_byte();
    if (high == EOF || low == EOF) {
      close();
      return false;
    }
    const unsigned length = (static_cast<unsigned>(high) << 8) |
                            static_cast<unsigned>(low);
    if (length < 2) {
      close();
      return false;
    }
    const bool sof = (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
                      marker != 0xc8 && marker != 0xcc);
    if (sof) {
      unsigned char header[6];
      if (length < 8 || !read_exact(header, sizeof(header))) {
        close();
        return false;
      }
      height = (static_cast<uint32_t>(header[1]) << 8) | header[2];
      width = (static_cast<uint32_t>(header[3]) << 8) | header[4];
      if (width == 0 || height == 0 || have_dimensions ||
          !skip(length - 8)) {
        close();
        return false;
      }
      have_dimensions = true;
    } else if (!skip(length - 2)) {
      close();
      return false;
    }
    if (marker == 0xda) {
      if (!have_dimensions) {
        close();
        return false;
      }
      in_entropy = true;
    }
    marker = -1;
    if (std::ferror(file)) {
      close();
      return false;
    }
  }
}

int reduced_grayscale_flag(uint32_t maximum_dimension) {
  if (maximum_dimension > 4u * LARDON3D_PHOTO_QUALITY_ANALYSIS_MAX_DIMENSION)
    return cv::IMREAD_REDUCED_GRAYSCALE_8;
  if (maximum_dimension > 2u * LARDON3D_PHOTO_QUALITY_ANALYSIS_MAX_DIMENSION)
    return cv::IMREAD_REDUCED_GRAYSCALE_4;
  if (maximum_dimension > LARDON3D_PHOTO_QUALITY_ANALYSIS_MAX_DIMENSION)
    return cv::IMREAD_REDUCED_GRAYSCALE_2;
  return cv::IMREAD_GRAYSCALE;
}

void initialize(Lardon3DPhotoQualityMetrics *output) {
  std::memset(output, 0, sizeof(*output));
  output->metrics_version = LARDON3D_PHOTO_QUALITY_METRICS_VERSION;
  output->status = LARDON3D_PHOTO_QUALITY_METRIC_INVALID_INPUT;
  output->recommendation = LARDON3D_PHOTO_QUALITY_REJECT;
}

void append_reason(char reasons[LARDON3D_PHOTO_QUALITY_REASON_CAPACITY], const char *reason) {
  const size_t used = std::strlen(reasons);
  if (used >= LARDON3D_PHOTO_QUALITY_REASON_CAPACITY - 1)
    return;
  (void)std::snprintf(reasons + used, LARDON3D_PHOTO_QUALITY_REASON_CAPACITY - used,
                      "%s%s", used ? ";" : "", reason);
}

}  // namespace

extern "C" Lardon3DPhotoQualityMetricStatus lardon3d_photo_quality_analyze_jpeg(
    const char *path, Lardon3DPhotoQualityMetrics *output) {
  if (!output)
    return LARDON3D_PHOTO_QUALITY_METRIC_INVALID_INPUT;
  initialize(output);
  if (!path || !path[0])
    return output->status;

  try {
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    if (!jpeg_dimensions(path, source_width, source_height)) {
      output->status = LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR;
      append_reason(output->reasons, "JPEG_DECODE_ERROR");
      return output->status;
    }
    const uint32_t source_maximum = std::max(source_width, source_height);
    output->decoded_width = source_width;
    output->decoded_height = source_height;
    if (source_maximum > LARDON3D_PHOTO_QUALITY_JPEG_MAX_DIMENSION) {
      /* This is an operational admission ceiling, not malformed input and not
       * a scientific rejection. Preserve a pending SUSPECT result so a proxy,
       * policy change, or explicit human override can resolve selection. */
      output->status = LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE;
      output->recommendation = LARDON3D_PHOTO_QUALITY_SUSPECT;
      append_reason(output->reasons, "JPEG_DIMENSIONS_EXCEED_OPERATIONAL_LIMIT");
      return output->status;
    }
    /* Complete structural validation through EOI precedes allocation. This is
     * what distinguishes an operationally oversized proxy from a truncated
     * file that merely contains a syntactically sufficient oversized SOF. */
    cv::Mat decoded = cv::imread(path, reduced_grayscale_flag(source_maximum));
    if (decoded.empty() || decoded.cols <= 0 || decoded.rows <= 0) {
      output->status = LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR;
      append_reason(output->reasons, "JPEG_DECODE_ERROR");
      return output->status;
    }
    const double scale = static_cast<double>(LARDON3D_PHOTO_QUALITY_ANALYSIS_MAX_DIMENSION) /
                         static_cast<double>(std::max(decoded.cols, decoded.rows));
    const int width = std::max(1, static_cast<int>(std::lround(decoded.cols * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(decoded.rows * scale)));
    cv::Mat analysis;
    cv::resize(decoded, analysis, cv::Size(width, height), 0.0, 0.0,
               scale < 1.0 ? cv::INTER_AREA : cv::INTER_NEAREST);
    decoded.release();
    output->analysis_width = static_cast<uint32_t>(analysis.cols);
    output->analysis_height = static_cast<uint32_t>(analysis.rows);

    cv::Mat normalized;
    analysis.convertTo(normalized, CV_32F, 1.0 / 255.0);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(normalized, mean, deviation);
    output->contrast_raw = deviation[0] * 255.0;
    output->contrast_normalized = deviation[0];

    cv::Mat laplacian;
    cv::Laplacian(normalized, laplacian, CV_32F, 3);
    cv::Scalar lap_mean;
    cv::Scalar lap_deviation;
    cv::meanStdDev(laplacian, lap_mean, lap_deviation);
    output->sharpness_raw = lap_deviation[0] * lap_deviation[0] * 255.0 * 255.0;
    output->sharpness_normalized = lap_deviation[0] * lap_deviation[0];
    laplacian.release();

    cv::Mat gradient_x;
    cv::Mat gradient_y;
    cv::Sobel(normalized, gradient_x, CV_32F, 1, 0, 3);
    cv::Sobel(normalized, gradient_y, CV_32F, 0, 1, 3);
    cv::Mat magnitude;
    cv::magnitude(gradient_x, gradient_y, magnitude);
    output->low_texture_fraction =
        static_cast<double>(cv::countNonZero(magnitude < kSobelLowTextureMagnitude)) /
        static_cast<double>(magnitude.total());
    output->clipped_black_fraction =
        static_cast<double>(cv::countNonZero(analysis <= kBlackSampleMaximum)) /
        static_cast<double>(analysis.total());
    output->clipped_white_fraction =
        static_cast<double>(cv::countNonZero(analysis >= kWhiteSampleMinimum)) /
        static_cast<double>(analysis.total());

    bool severe = false;
    bool suspect = false;
    if (output->sharpness_normalized < kSharpnessVeryLow) {
      append_reason(output->reasons, "SHARPNESS_VERY_LOW");
      severe = true;
    } else if (output->sharpness_normalized < kSharpnessLow) {
      append_reason(output->reasons, "SHARPNESS_LOW");
      suspect = true;
    }
    if (output->clipped_white_fraction > kWhiteClippingSevere ||
        output->clipped_black_fraction > kBlackClippingSevere) {
      append_reason(output->reasons, "EXPOSURE_CLIPPING_SEVERE");
      severe = true;
    } else if (output->clipped_white_fraction > kWhiteClippingSuspect ||
               output->clipped_black_fraction > kBlackClippingSuspect) {
      append_reason(output->reasons, "EXPOSURE_CLIPPING");
      suspect = true;
    }
    if (output->contrast_normalized < kContrastLow ||
        output->low_texture_fraction > kLowTextureFraction) {
      append_reason(output->reasons, "LOW_TEXTURE_OR_CONTRAST");
      suspect = true;
    }
    output->status = LARDON3D_PHOTO_QUALITY_METRIC_OK;
    output->recommendation = severe ? LARDON3D_PHOTO_QUALITY_REJECT
                                    : suspect ? LARDON3D_PHOTO_QUALITY_SUSPECT
                                              : LARDON3D_PHOTO_QUALITY_GOOD;
    if (!output->reasons[0])
      append_reason(output->reasons, "OK");
    return output->status;
  } catch (...) {
    // C++ and OpenCV exceptions are contained at the public C17 ABI boundary.
    initialize(output);
    output->status = LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR;
    append_reason(output->reasons, "JPEG_DECODE_ERROR");
    return output->status;
  }
}

extern "C" void lardon3d_photo_quality_raw_only(Lardon3DPhotoQualityMetrics *output) {
  if (!output)
    return;
  initialize(output);
  output->status = LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE;
  output->recommendation = LARDON3D_PHOTO_QUALITY_SUSPECT;
  append_reason(output->reasons, "METRIC_UNAVAILABLE_REQUIRES_JPEG_PROXY");
}

extern "C" int lardon3d_photo_quality_effective_include(
    Lardon3DPhotoQualityRecommendation recommendation,
    Lardon3DPhotoQualityOverride override_value) {
  if (override_value == LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE)
    return 1;
  if (override_value == LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE)
    return 0;
  return recommendation == LARDON3D_PHOTO_QUALITY_GOOD ? 1 : 0;
}
