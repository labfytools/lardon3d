#ifndef LARDON3D_ACQUISITION_INGEST_H
#define LARDON3D_ACQUISITION_INGEST_H

#include <stddef.h>
#include <stdint.h>

#include <lardon3d/app_state.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_ACQUISITION_INGEST_MAX_SOURCES 64u

typedef enum {
  LARDON3D_ACQUISITION_INGEST_OK = 0,
  LARDON3D_ACQUISITION_INGEST_INVALID_ARGUMENT,
  LARDON3D_ACQUISITION_INGEST_SCANSET_NOT_FOUND,
  LARDON3D_ACQUISITION_INGEST_PUBLISH_ERROR,
  LARDON3D_ACQUISITION_INGEST_METADATA_ERROR,
  LARDON3D_ACQUISITION_INGEST_DUPLICATE_ASSET,
  LARDON3D_ACQUISITION_INGEST_DB_ERROR,
  LARDON3D_ACQUISITION_INGEST_DEVELOPMENT_ERROR,
  LARDON3D_ACQUISITION_INGEST_CONSTRAINT,
  LARDON3D_ACQUISITION_INGEST_INTERNAL_ERROR
} Lardon3DAcquisitionIngestResult;

typedef enum {
  LARDON3D_ACQUISITION_GROUP_AUTOMATIC = 1,
  LARDON3D_ACQUISITION_GROUP_CALLER_EXPLICIT = 2
} Lardon3DAcquisitionGroupingMode;

typedef enum {
  LARDON3D_ACQUISITION_GROUP_SINGLETON = 0,
  LARDON3D_ACQUISITION_GROUP_STRONG = 1,
  LARDON3D_ACQUISITION_GROUP_EXPLICIT = 2
} Lardon3DAcquisitionGroupBasis;

typedef enum {
  LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE = 1,
  LARDON3D_ACQUISITION_SELECT_DEVELOP_RAW = 2
} Lardon3DAcquisitionRepresentation;

typedef struct {
  const char *source_path; /* Borrowed for the duration of the call. */
  uint32_t explicit_group;
} Lardon3DAcquisitionIngestSource;

typedef struct {
  Lardon3DAcquisitionGroupingMode grouping;
  Lardon3DAcquisitionRepresentation representation;
  uint8_t select_representation;
  uint8_t reserved[7];
  uint64_t resume_capture_id;
  uint64_t producer_task_id;
  int64_t imported_at;
  uint64_t max_source_bytes;
} Lardon3DAcquisitionIngestOptions;

typedef struct {
  uint64_t capture_id;
  Lardon3DAcquisitionGroupBasis basis;
  size_t source_count;
  size_t source_indices[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES];
} Lardon3DAcquisitionIngestGroup;

typedef struct {
  size_t group_count;
  Lardon3DAcquisitionIngestGroup groups[LARDON3D_ACQUISITION_INGEST_MAX_SOURCES];
} Lardon3DAcquisitionIngestOutput;

Lardon3DAcquisitionIngestResult lardon3d_acquisition_ingest(
    Lardon3DAppState *state, uint64_t scanset_id,
    const Lardon3DAcquisitionIngestSource *sources, size_t source_count,
    const Lardon3DAcquisitionIngestOptions *options, Lardon3DAcquisitionIngestOutput *output);

#ifdef __cplusplus
}
#endif

#endif
