#ifndef LARDON3D_FEATURE_STORE_H
#define LARDON3D_FEATURE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/feature_extractor.h>
#include <lardon3d/project_db.h>

enum {
  LARDON3D_FEATURE_FILE_VERSION = 1,
  LARDON3D_FEATURE_FILE_HEADER_SIZE = 160,
  LARDON3D_FEATURE_KEYPOINT_RECORD_SIZE = 24,
  LARDON3D_FEATURE_READER_RANGE_MAX = 256,
  LARDON3D_FEATURE_FILE_MAX_SIZE = 16 * 1024 * 1024,
};

typedef enum {
  LARDON3D_FEATURE_DESCRIPTOR_U8 = 1,
  LARDON3D_FEATURE_DESCRIPTOR_F32 = 2
} Lardon3DFeatureDescriptorType;

typedef enum {
  LARDON3D_FEATURE_STORE_OK = 0,
  LARDON3D_FEATURE_STORE_ALREADY_PRESENT,
  LARDON3D_FEATURE_STORE_INVALID_ARGUMENT,
  LARDON3D_FEATURE_STORE_NOT_FOUND,
  LARDON3D_FEATURE_STORE_INVALID,
  LARDON3D_FEATURE_STORE_UNSUPPORTED_VERSION,
  LARDON3D_FEATURE_STORE_CORRUPT,
  LARDON3D_FEATURE_STORE_IO_ERROR,
  LARDON3D_FEATURE_STORE_DB_BUSY,
  LARDON3D_FEATURE_STORE_DB_ERROR,
  LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE
} Lardon3DFeatureStoreResult;

typedef struct Lardon3DFeatureReader Lardon3DFeatureReader;

typedef struct {
  uint32_t format_version;
  char extractor_kind[LARDON3D_TASK_KIND_CAPACITY];
  uint32_t extractor_version;
  uint32_t feature_count;
  uint32_t descriptor_dimension;
  Lardon3DFeatureDescriptorType descriptor_type;
  uint32_t image_width;
  uint32_t image_height;
  unsigned char source_image_sha256[32];
  unsigned char parameter_fingerprint[32];
} Lardon3DFeatureFileMetadata;

Lardon3DFeatureStoreResult lardon3d_feature_store_publish(
    Lardon3DAppState *state, uint64_t image_id, uint64_t producer_task_id,
    const Lardon3DFeatureExtractorParameters *parameters, const Lardon3DExtractedFeatures *features,
    Lardon3DProjectDbFeatureSet *feature_set);
Lardon3DFeatureStoreResult
lardon3d_feature_reader_open(const char *project_path,
                             const Lardon3DProjectDbFeatureSet *feature_set,
                             Lardon3DFeatureReader **reader, Lardon3DFeatureFileMetadata *metadata);
void lardon3d_feature_reader_close(Lardon3DFeatureReader *reader);
Lardon3DFeatureStoreResult lardon3d_feature_reader_keypoints(Lardon3DFeatureReader *reader,
                                                             uint32_t start,
                                                             Lardon3DFeatureKeypoint *keypoints,
                                                             size_t capacity);
Lardon3DFeatureStoreResult lardon3d_feature_reader_descriptors(Lardon3DFeatureReader *reader,
                                                               uint32_t start,
                                                               unsigned char *descriptors,
                                                               size_t feature_capacity,
                                                               size_t descriptor_capacity);

#endif
