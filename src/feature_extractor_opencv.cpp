#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <vector>

#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <openssl/sha.h>

extern "C" {
#include <lardon3d/feature_extractor.h>
}

extern "C" bool lardon3d_feature_opencv_configure_threads(unsigned int threads) {
  if (threads == 0 || threads > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
    return false;
  }
  cv::setNumThreads(static_cast<int>(threads));
  return cv::getNumThreads() == static_cast<int>(threads);
}

extern "C" unsigned int lardon3d_feature_opencv_thread_count(void) {
  int threads = cv::getNumThreads();
  return threads > 0 ? static_cast<unsigned int>(threads) : 1U;
}

extern "C" bool
lardon3d_feature_extractor_parameters_valid(const Lardon3DFeatureExtractorParameters *parameters) {
  return parameters && parameters->max_features > 0 &&
         parameters->max_features <= LARDON3D_FEATURE_MAX_FEATURES &&
         parameters->pyramid_levels >= 1 && parameters->pyramid_levels <= 16 &&
         parameters->fast_threshold >= 1 && parameters->fast_threshold <= 255;
}

static void put_u32(unsigned char *output, uint32_t value) {
  output[0] = static_cast<unsigned char>(value);
  output[1] = static_cast<unsigned char>(value >> 8);
  output[2] = static_cast<unsigned char>(value >> 16);
  output[3] = static_cast<unsigned char>(value >> 24);
}

static void put_u64(unsigned char *output, uint64_t value) {
  for (unsigned int index = 0; index < 8; ++index) {
    output[index] = static_cast<unsigned char>(value >> (8U * index));
  }
}

extern "C" void lardon3d_feature_extractor_parameter_fingerprint(
    const Lardon3DFeatureExtractorParameters *parameters, unsigned char fingerprint[32]) {
  if (!fingerprint) {
    return;
  }
  std::memset(fingerprint, 0, 32);
  if (!lardon3d_feature_extractor_parameters_valid(parameters)) {
    return;
  }
  unsigned char canonical[24] = {'L', '3', 'D', 'O', 'R', 'B', 'P', '1'};
  put_u32(canonical + 8, LARDON3D_FEATURE_EXTRACTOR_VERSION);
  put_u32(canonical + 12, parameters->max_features);
  put_u32(canonical + 16, parameters->pyramid_levels);
  put_u32(canonical + 20, parameters->fast_threshold);
  (void)SHA256(canonical, sizeof(canonical), fingerprint);
}

extern "C" bool
lardon3d_sift_extractor_parameters_valid(const Lardon3DSiftExtractorParameters *p) {
  return p && p->max_features >= 1 && p->max_features <= LARDON3D_FEATURE_MAX_FEATURES &&
         p->octave_layers >= 1 && p->octave_layers <= 8 && std::isfinite(p->contrast_threshold) &&
         p->contrast_threshold >= 0.001 && p->contrast_threshold <= 0.2 &&
         std::isfinite(p->edge_threshold) && p->edge_threshold >= 1.0 &&
         p->edge_threshold <= 100.0 && std::isfinite(p->sigma) && p->sigma >= 0.5 &&
         p->sigma <= 3.0 && p->grid_rows >= 1 && p->grid_rows <= 32 && p->grid_cols >= 1 &&
         p->grid_cols <= 32 && p->max_features_per_cell >= 1 &&
         p->max_features_per_cell <= LARDON3D_FEATURE_MAX_FEATURES;
}

extern "C" Lardon3DSiftExtractorParameters lardon3d_sift_precision_classic_v1(bool rootsift) {
  return {4096, 3, 0.02, 10.0, 1.6, 8, 8, 96, rootsift};
}

extern "C" void lardon3d_sift_extractor_parameter_fingerprint(
    const Lardon3DSiftExtractorParameters *p, unsigned char fingerprint[32]) {
  if (!fingerprint) return;
  std::memset(fingerprint, 0, 32);
  if (!lardon3d_sift_extractor_parameters_valid(p)) return;
  unsigned char canonical[60] = {'L', '3', 'D', 'S', 'I', 'F', 'T', '1'};
  put_u32(canonical + 8, 1);
  put_u32(canonical + 12, p->max_features);
  put_u32(canonical + 16, p->octave_layers);
  uint64_t bits = 0;
  std::memcpy(&bits, &p->contrast_threshold, 8);
  put_u64(canonical + 20, bits);
  std::memcpy(&bits, &p->edge_threshold, 8);
  put_u64(canonical + 28, bits);
  std::memcpy(&bits, &p->sigma, 8);
  put_u64(canonical + 36, bits);
  put_u32(canonical + 44, p->grid_rows);
  put_u32(canonical + 48, p->grid_cols);
  put_u32(canonical + 52, p->max_features_per_cell);
  put_u32(canonical + 56, p->rootsift ? 1U : 0U);
  (void)SHA256(canonical, sizeof(canonical), fingerprint);
}

static bool sift_point_before(const cv::KeyPoint &a, size_t ai, const cv::KeyPoint &b, size_t bi) {
  if (a.response != b.response) return a.response > b.response;
  if (a.pt.y != b.pt.y) return a.pt.y < b.pt.y;
  if (a.pt.x != b.pt.x) return a.pt.x < b.pt.x;
  return ai < bi;
}

static bool select_sift_points(const cv::Mat &image, const std::vector<cv::KeyPoint> &points,
                               const Lardon3DSiftExtractorParameters *parameters,
                               std::vector<size_t> &selected, std::vector<uint32_t> &cells) {
  std::vector<size_t> order(points.size());
  for (size_t index = 0; index < order.size(); ++index) order[index] = index;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return sift_point_before(points[a], a, points[b], b);
  });
  cells.assign(static_cast<size_t>(parameters->grid_rows) * parameters->grid_cols, 0);
  selected.reserve(parameters->max_features);
  float width = static_cast<float>(image.cols), height = static_cast<float>(image.rows);
  for (size_t source : order) {
    const cv::KeyPoint &point = points[source];
    if (!std::isfinite(point.pt.x) || !std::isfinite(point.pt.y) || point.pt.x < 0 ||
        point.pt.y < 0 || point.pt.x >= width || point.pt.y >= height)
      return false;
    uint32_t column = std::min(
        parameters->grid_cols - 1,
        static_cast<uint32_t>(point.pt.x / width * static_cast<float>(parameters->grid_cols)));
    uint32_t row = std::min(
        parameters->grid_rows - 1,
        static_cast<uint32_t>(point.pt.y / height * static_cast<float>(parameters->grid_rows)));
    uint32_t &used = cells[static_cast<size_t>(row) * parameters->grid_cols + column];
    if (used < parameters->max_features_per_cell) {
      selected.push_back(source);
      ++used;
    }
    if (selected.size() == parameters->max_features) break;
  }
  return true;
}

static Lardon3DFeatureExtractResult copy_sift_output(
    const std::vector<cv::KeyPoint> &points, const cv::Mat &descriptors,
    const std::vector<size_t> &selected, const Lardon3DSiftExtractorParameters *parameters,
    Lardon3DExtractedFeatures *output) {
  size_t count = selected.size();
  Lardon3DFeatureKeypoint *keypoints = count ? static_cast<Lardon3DFeatureKeypoint *>(
                                                  std::calloc(count, sizeof(*keypoints)))
                                             : nullptr;
  unsigned char *bytes = count ? static_cast<unsigned char *>(
                                     std::malloc(count * 128U * sizeof(float)))
                               : nullptr;
  if (count && (!keypoints || !bytes)) {
    std::free(keypoints);
    std::free(bytes);
    return LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY;
  }
  for (size_t index = 0; index < count; ++index) {
    const cv::KeyPoint &point = points[selected[index]];
    int octave = point.octave & 255;
    if (octave >= 128) octave -= 256;
    keypoints[index] = {point.pt.x, point.pt.y, point.size, point.angle, point.response, octave};
    const float *source = descriptors.ptr<float>(static_cast<int>(selected[index]));
    float *destination = reinterpret_cast<float *>(bytes) + index * 128U;
    double l1 = 0.0;
    for (size_t dimension = 0; dimension < 128; ++dimension) {
      if (!std::isfinite(source[dimension]) || source[dimension] < 0.0F ||
          source[dimension] > 512.0F) {
        std::free(keypoints);
        std::free(bytes);
        return LARDON3D_FEATURE_EXTRACT_ERROR;
      }
      if (parameters->rootsift) l1 += static_cast<double>(source[dimension]);
    }
    for (size_t dimension = 0; dimension < 128; ++dimension) {
      destination[dimension] =
          parameters->rootsift
              ? (l1 > 0.0 ? static_cast<float>(std::sqrt(source[dimension] / l1)) : 0.0F)
              : source[dimension];
    }
  }
  output->feature_count = static_cast<uint32_t>(count);
  output->keypoints = keypoints;
  output->descriptors = bytes;
  output->descriptor_bytes = count * 128U * sizeof(float);
  return LARDON3D_FEATURE_EXTRACT_OK;
}

extern "C" Lardon3DFeatureExtractResult lardon3d_feature_extract_sift(
    const char *path, const Lardon3DSiftExtractorParameters *p, Lardon3DExtractedFeatures *out) {
  if (out) std::memset(out, 0, sizeof(*out));
  if (!path || !path[0] || !out || !lardon3d_sift_extractor_parameters_valid(p))
    return LARDON3D_FEATURE_EXTRACT_INVALID_ARGUMENT;
  struct stat information{};
  if (lstat(path, &information) != 0)
    return errno == ENOENT ? LARDON3D_FEATURE_EXTRACT_IMAGE_NOT_FOUND
                           : LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  if (!S_ISREG(information.st_mode) || S_ISLNK(information.st_mode))
    return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  try {
    cv::Mat image = cv::imread(path, cv::IMREAD_GRAYSCALE);
    uint64_t pixels = image.empty() ? 0 : static_cast<uint64_t>(image.cols) * image.rows;
    if (image.empty() || image.cols <= 0 || image.rows <= 0 ||
        pixels > LARDON3D_FEATURE_MAX_IMAGE_PIXELS)
      return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
    uint32_t candidates =
        std::min(static_cast<uint32_t>(LARDON3D_FEATURE_MAX_FEATURES), p->max_features * 2U);
    cv::Ptr<cv::SIFT> extractor = cv::SIFT::create(
        static_cast<int>(candidates), static_cast<int>(p->octave_layers), p->contrast_threshold,
        p->edge_threshold, p->sigma, true);
    std::vector<cv::KeyPoint> points;
    cv::Mat descriptors;
    extractor->detectAndCompute(image, cv::noArray(), points, descriptors);
    if ((!points.empty() && (descriptors.type() != CV_32FC1 || descriptors.cols != 128 ||
                             descriptors.rows != static_cast<int>(points.size()))) ||
        points.size() > 65536U)
      return LARDON3D_FEATURE_EXTRACT_ERROR;
    std::vector<size_t> selected;
    std::vector<uint32_t> cells;
    if (!select_sift_points(image, points, p, selected, cells))
      return LARDON3D_FEATURE_EXTRACT_ERROR;
    Lardon3DFeatureExtractResult copied = copy_sift_output(points, descriptors, selected, p, out);
    if (copied != LARDON3D_FEATURE_EXTRACT_OK) return copied;
    uint32_t occupied = static_cast<uint32_t>(
        std::count_if(cells.begin(), cells.end(), [](uint32_t value) { return value > 0; }));
    size_t cell_count = cells.size();
    out->image_width = static_cast<uint32_t>(image.cols);
    out->image_height = static_cast<uint32_t>(image.rows);
    out->quality = {occupied, static_cast<uint32_t>(cell_count),
                    static_cast<double>(occupied) / static_cast<double>(cell_count),
                    pixels ? static_cast<double>(selected.size()) * 1000000.0 /
                                 static_cast<double>(pixels)
                           : 0.0};
    return LARDON3D_FEATURE_EXTRACT_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  } catch (const std::bad_alloc &) {
    return LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY;
  } catch (...) {
    return LARDON3D_FEATURE_EXTRACT_ERROR;
  }
}

extern "C" void lardon3d_extracted_features_destroy(Lardon3DExtractedFeatures *features) {
  if (!features) {
    return;
  }
  std::free(features->keypoints);
  std::free(features->descriptors);
  std::memset(features, 0, sizeof(*features));
}

extern "C" Lardon3DFeatureExtractResult
lardon3d_feature_extract_orb(const char *path, const Lardon3DFeatureExtractorParameters *parameters,
                             Lardon3DExtractedFeatures *features) {
  if (features) {
    std::memset(features, 0, sizeof(*features));
  }
  if (!path || !path[0] || !features || !lardon3d_feature_extractor_parameters_valid(parameters)) {
    return LARDON3D_FEATURE_EXTRACT_INVALID_ARGUMENT;
  }
  struct stat information{};
  if (lstat(path, &information) != 0) {
    return errno == ENOENT ? LARDON3D_FEATURE_EXTRACT_IMAGE_NOT_FOUND
                           : LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  }
  if (!S_ISREG(information.st_mode) || S_ISLNK(information.st_mode)) {
    return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  }
  try {
    cv::Mat image = cv::imread(path, cv::IMREAD_GRAYSCALE);
    if (image.empty()) {
      return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
    }
    if (image.cols <= 0 || image.rows <= 0 ||
        static_cast<uint64_t>(image.cols) * static_cast<uint64_t>(image.rows) >
            LARDON3D_FEATURE_MAX_IMAGE_PIXELS) {
      return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
    }
    cv::Ptr<cv::ORB> extractor =
        cv::ORB::create(static_cast<int>(parameters->max_features), 1.2F,
                        static_cast<int>(parameters->pyramid_levels), 31, 0, 2,
                        cv::ORB::HARRIS_SCORE, 31, static_cast<int>(parameters->fast_threshold));
    std::vector<cv::KeyPoint> points;
    cv::Mat descriptors;
    extractor->detectAndCompute(image, cv::noArray(), points, descriptors);
    if (points.size() > std::numeric_limits<uint32_t>::max() ||
        (!points.empty() && (descriptors.rows != static_cast<int>(points.size()) ||
                             descriptors.cols != LARDON3D_FEATURE_DESCRIPTOR_DIMENSION ||
                             descriptors.type() != CV_8UC1))) {
      return LARDON3D_FEATURE_EXTRACT_ERROR;
    }
    if (points.size() > parameters->max_features) {
      /* OpenCV documents nfeatures as a requested maximum but may return one extra
       * point while distributing per-level quotas. The Feature Set contract is an
       * exact bound, so retain OpenCV's deterministic prefix and matching rows. */
      points.resize(parameters->max_features);
      descriptors = descriptors.rowRange(0, static_cast<int>(parameters->max_features));
    }
    size_t count = points.size();
    Lardon3DFeatureKeypoint *keypoints =
        count == 0 ? nullptr
                   : static_cast<Lardon3DFeatureKeypoint *>(
                         std::calloc(count, sizeof(Lardon3DFeatureKeypoint)));
    unsigned char *bytes = count == 0 ? nullptr
                                      : static_cast<unsigned char *>(std::malloc(
                                            count * LARDON3D_FEATURE_DESCRIPTOR_DIMENSION));
    if (count > 0 && (!keypoints || !bytes)) {
      std::free(keypoints);
      std::free(bytes);
      return LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < count; ++index) {
      const cv::KeyPoint &point = points[index];
      if (!std::isfinite(point.pt.x) || !std::isfinite(point.pt.y) || !std::isfinite(point.size) ||
          !std::isfinite(point.angle) || !std::isfinite(point.response) || point.pt.x < 0.0F ||
          point.pt.y < 0.0F || point.pt.x >= static_cast<float>(image.cols) ||
          point.pt.y >= static_cast<float>(image.rows) || point.size <= 0.0F ||
          point.angle < 0.0F || point.angle >= 360.0F) {
        std::free(keypoints);
        std::free(bytes);
        return LARDON3D_FEATURE_EXTRACT_ERROR;
      }
      keypoints[index] = {point.pt.x,  point.pt.y,     point.size,
                          point.angle, point.response, point.octave};
      std::memcpy(bytes + index * LARDON3D_FEATURE_DESCRIPTOR_DIMENSION,
                  descriptors.ptr(static_cast<int>(index)), LARDON3D_FEATURE_DESCRIPTOR_DIMENSION);
    }
    features->image_width = static_cast<uint32_t>(image.cols);
    features->image_height = static_cast<uint32_t>(image.rows);
    features->feature_count = static_cast<uint32_t>(count);
    features->keypoints = keypoints;
    features->descriptors = bytes;
    features->descriptor_bytes = count * LARDON3D_FEATURE_DESCRIPTOR_DIMENSION;
    return LARDON3D_FEATURE_EXTRACT_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  } catch (const std::bad_alloc &) {
    return LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY;
  } catch (...) {
    return LARDON3D_FEATURE_EXTRACT_ERROR;
  }
}
