#ifndef LARDON3D_FEATURE_EXTRACTOR_H
#define LARDON3D_FEATURE_EXTRACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LARDON3D_FEATURE_EXTRACTOR_KIND "orb"
enum {
  LARDON3D_FEATURE_EXTRACTOR_VERSION = 1,
  LARDON3D_FEATURE_DESCRIPTOR_DIMENSION = 32,
  LARDON3D_FEATURE_MAX_FEATURES = 8192,
  LARDON3D_FEATURE_MAX_IMAGE_PIXELS = 100000000,
};

typedef struct {
  uint32_t max_features;
  uint32_t pyramid_levels;
  uint32_t fast_threshold;
} Lardon3DFeatureExtractorParameters;

typedef struct {
  float x;
  float y;
  float size;
  float angle_degrees;
  float response;
  int32_t octave;
} Lardon3DFeatureKeypoint;

typedef struct {
  uint32_t image_width;
  uint32_t image_height;
  uint32_t feature_count;
  Lardon3DFeatureKeypoint *keypoints;
  unsigned char *descriptors;
} Lardon3DExtractedFeatures;

typedef enum {
  LARDON3D_FEATURE_EXTRACT_OK = 0,
  LARDON3D_FEATURE_EXTRACT_INVALID_ARGUMENT,
  LARDON3D_FEATURE_EXTRACT_IMAGE_NOT_FOUND,
  LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID,
  LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY,
  LARDON3D_FEATURE_EXTRACT_ERROR
} Lardon3DFeatureExtractResult;

bool lardon3d_feature_extractor_parameters_valid(
    const Lardon3DFeatureExtractorParameters *parameters);
void lardon3d_feature_extractor_parameter_fingerprint(
    const Lardon3DFeatureExtractorParameters *parameters, unsigned char fingerprint[32]);
Lardon3DFeatureExtractResult
lardon3d_feature_extract_orb(const char *path, const Lardon3DFeatureExtractorParameters *parameters,
                             Lardon3DExtractedFeatures *features);
void lardon3d_extracted_features_destroy(Lardon3DExtractedFeatures *features);

#endif
