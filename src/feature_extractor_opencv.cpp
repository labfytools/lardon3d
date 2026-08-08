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
    if (points.size() > parameters->max_features ||
        points.size() > std::numeric_limits<uint32_t>::max() ||
        (!points.empty() && (descriptors.rows != static_cast<int>(points.size()) ||
                             descriptors.cols != LARDON3D_FEATURE_DESCRIPTOR_DIMENSION ||
                             descriptors.type() != CV_8UC1))) {
      return LARDON3D_FEATURE_EXTRACT_ERROR;
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
    return LARDON3D_FEATURE_EXTRACT_OK;
  } catch (const cv::Exception &) {
    return LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID;
  } catch (const std::bad_alloc &) {
    return LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY;
  } catch (...) {
    return LARDON3D_FEATURE_EXTRACT_ERROR;
  }
}
