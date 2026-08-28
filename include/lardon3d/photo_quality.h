#ifndef LARDON3D_PHOTO_QUALITY_H
#define LARDON3D_PHOTO_QUALITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_PHOTO_QUALITY_METRICS_VERSION = 1,
  LARDON3D_PHOTO_QUALITY_REASON_CAPACITY = 256,
  /* Metrics v1 retains at most a 1024-pixel longest edge. This engineering
   * policy bounds deterministic per-group memory; it is not image validity. */
  LARDON3D_PHOTO_QUALITY_ANALYSIS_MAX_DIMENSION = 1024,
  /* A valid JPEG above this 8192-pixel proxy edge is operationally unavailable
   * before allocation. It remains SUSPECT/pending rather than a decode error. */
  LARDON3D_PHOTO_QUALITY_JPEG_MAX_DIMENSION = 8192,
};

typedef enum {
  LARDON3D_PHOTO_QUALITY_METRIC_OK = 0,
  LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE,
  LARDON3D_PHOTO_QUALITY_METRIC_INVALID_INPUT,
  LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR
} Lardon3DPhotoQualityMetricStatus;

typedef enum {
  LARDON3D_PHOTO_QUALITY_GOOD = 1,
  LARDON3D_PHOTO_QUALITY_SUSPECT = 2,
  LARDON3D_PHOTO_QUALITY_REJECT = 3
} Lardon3DPhotoQualityRecommendation;

typedef enum {
  LARDON3D_PHOTO_QUALITY_OVERRIDE_NONE = 0,
  LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE = 1,
  LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE = 2
} Lardon3DPhotoQualityOverride;

typedef struct {
  uint32_t metrics_version;
  Lardon3DPhotoQualityMetricStatus status;
  Lardon3DPhotoQualityRecommendation recommendation;
  uint32_t decoded_width;
  uint32_t decoded_height;
  uint32_t analysis_width;
  uint32_t analysis_height;
  double sharpness_raw;
  double sharpness_normalized;
  double clipped_black_fraction;
  double clipped_white_fraction;
  double contrast_raw;
  double contrast_normalized;
  double low_texture_fraction;
  char reasons[LARDON3D_PHOTO_QUALITY_REASON_CAPACITY];
} Lardon3DPhotoQualityMetrics;

/* Analyze one JPEG proxy/source using one bounded reduced-resolution grayscale
 * decode. JPEG dimensions above the operational 8192-pixel decode ceiling
 * return UNAVAILABLE + SUSPECT without allocating the raster; this is pending
 * selection evidence, not a decode error or scientific rejection. Accepted
 * images are decoded at an OpenCV JPEG reduction selected to retain at most a
 * 1024-pixel maximum dimension. output is required, caller-owned, and always
 * initialized when provided, including on invalid input and decode failure.
 * path must name a readable JPEG.
 * No Capture,
 * Asset, image_id, SHA-256, path, or basename identity is inferred or changed. */
Lardon3DPhotoQualityMetricStatus lardon3d_photo_quality_analyze_jpeg(
    const char *path, Lardon3DPhotoQualityMetrics *output);

/* Produce the required explicit result for a group that has no decodable JPEG
 * proxy. RAW bytes are not developed merely for triage. */
void lardon3d_photo_quality_raw_only(Lardon3DPhotoQualityMetrics *output);

/* Default inclusion is recommendation-derived (GOOD only); a human override
 * remains a separate durable decision and never rewrites measured metrics. */
int lardon3d_photo_quality_effective_include(
    Lardon3DPhotoQualityRecommendation recommendation,
    Lardon3DPhotoQualityOverride override_value);

#ifdef __cplusplus
}
#endif

#endif
