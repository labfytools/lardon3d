#ifndef LARDON3D_TRACK_BUILDER_H
#define LARDON3D_TRACK_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LARDON3D_TRACK_BUILDER_VERSION = 1,
  LARDON3D_TRACK_BUILDER_FINGERPRINT_SIZE = 32,
  LARDON3D_TRACK_BUILDER_FINGERPRINT_INPUT_SIZE = 48,
  LARDON3D_TRACK_BUILDER_KIND_CAPACITY = 64,
};

typedef enum {
  LARDON3D_TRACK_BUILDER_OK = 0,
  LARDON3D_TRACK_BUILDER_INVALID_ARGUMENT,
  LARDON3D_TRACK_BUILDER_CONSTRAINT,
  LARDON3D_TRACK_BUILDER_CORRUPT_INPUT,
  LARDON3D_TRACK_BUILDER_OUT_OF_MEMORY,
} Lardon3DTrackBuilderResult;

typedef struct {
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint64_t image_id;
  char extractor_kind[LARDON3D_TRACK_BUILDER_KIND_CAPACITY];
  uint32_t extractor_version;
  unsigned char parameter_fingerprint[32];
  uint32_t descriptor_type;
  uint32_t descriptor_dimension;
} Lardon3DTrackBuilderObservation;

typedef struct {
  const Lardon3DTrackBuilderObservation *first;
  const Lardon3DTrackBuilderObservation *second;
} Lardon3DTrackBuilderEdge;

typedef struct {
  uint64_t feature_set_id;
  uint32_t feature_index;
  uint64_t image_id;
} Lardon3DTrackBuilderTrackObservation;

typedef struct {
  size_t observation_count;
  Lardon3DTrackBuilderTrackObservation *observations;
} Lardon3DTrackBuilderTrack;

typedef struct {
  size_t track_count;
  Lardon3DTrackBuilderTrack *tracks;
} Lardon3DTrackBuilderResultSet;

/* The result must be zero-initialized before the first build. Inputs remain
 * caller-owned and are not modified. Output ownership is returned to the
 * caller; free it exactly (or repeatedly) with result_free. */
Lardon3DTrackBuilderResult lardon3d_track_builder_build(
    const Lardon3DTrackBuilderObservation *observations,
    size_t observation_count, const Lardon3DTrackBuilderEdge *edges,
    size_t edge_count, Lardon3DTrackBuilderResultSet *result);

void lardon3d_track_builder_result_free(Lardon3DTrackBuilderResultSet *result);

bool lardon3d_track_builder_fingerprint_bytes(
    unsigned char bytes[LARDON3D_TRACK_BUILDER_FINGERPRINT_INPUT_SIZE]);

bool lardon3d_track_builder_fingerprint(
    unsigned char fingerprint[LARDON3D_TRACK_BUILDER_FINGERPRINT_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
