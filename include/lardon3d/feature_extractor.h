#ifndef LARDON3D_FEATURE_EXTRACTOR_H
#define LARDON3D_FEATURE_EXTRACTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LARDON3D_FEATURE_EXTRACTOR_KIND "orb"
#define LARDON3D_SIFT_EXTRACTOR_KIND "sift"
#define LARDON3D_ROOTSIFT_EXTRACTOR_KIND "rootsift"
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
  uint32_t max_features;
  uint32_t octave_layers;
  double contrast_threshold;
  double edge_threshold;
  double sigma;
  uint32_t grid_rows;
  uint32_t grid_cols;
  uint32_t max_features_per_cell;
  bool rootsift;
} Lardon3DSiftExtractorParameters;

typedef struct {
  uint32_t occupied_cells;
  uint32_t total_cells;
  double coverage_ratio;
  double feature_density_per_megapixel;
} Lardon3DFeatureQualityMetrics;

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
  Lardon3DFeatureQualityMetrics quality;
  size_t descriptor_bytes;
} Lardon3DExtractedFeatures;

typedef enum {
  LARDON3D_FEATURE_EXTRACT_OK = 0,
  LARDON3D_FEATURE_EXTRACT_INVALID_ARGUMENT,
  LARDON3D_FEATURE_EXTRACT_IMAGE_NOT_FOUND,
  LARDON3D_FEATURE_EXTRACT_IMAGE_INVALID,
  LARDON3D_FEATURE_EXTRACT_OUT_OF_MEMORY,
  LARDON3D_FEATURE_EXTRACT_ERROR
} Lardon3DFeatureExtractResult;

/* Configure OpenCV's process-wide internal CPU-thread limit before workers start.
 * `threads` must be in 1..INT_MAX. The setting is operational: it does not alter
 * extractor fingerprints or FeatureSet identity. Callers must not race this API
 * with extraction; the application runtime owns configuration and nested users
 * must restore the preceding value before releasing their Task reservation. */
bool lardon3d_feature_opencv_configure_threads(unsigned int threads);
/* Return OpenCV's current process-wide limit, normalized to at least one thread. */
unsigned int lardon3d_feature_opencv_thread_count(void);

bool lardon3d_feature_extractor_parameters_valid(
    const Lardon3DFeatureExtractorParameters *parameters);
void lardon3d_feature_extractor_parameter_fingerprint(
    const Lardon3DFeatureExtractorParameters *parameters, unsigned char fingerprint[32]);
Lardon3DFeatureExtractResult
lardon3d_feature_extract_orb(const char *path, const Lardon3DFeatureExtractorParameters *parameters,
                             Lardon3DExtractedFeatures *features);
bool lardon3d_sift_extractor_parameters_valid(const Lardon3DSiftExtractorParameters *parameters);
Lardon3DSiftExtractorParameters lardon3d_sift_precision_classic_v1(bool rootsift);
void lardon3d_sift_extractor_parameter_fingerprint(
    const Lardon3DSiftExtractorParameters *parameters, unsigned char fingerprint[32]);
Lardon3DFeatureExtractResult
lardon3d_feature_extract_sift(const char *path, const Lardon3DSiftExtractorParameters *parameters,
                              Lardon3DExtractedFeatures *features);
void lardon3d_extracted_features_destroy(Lardon3DExtractedFeatures *features);

#endif
