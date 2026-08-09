#ifndef LARDON3D_TRACK_BUILDER_PROJECT_H
#define LARDON3D_TRACK_BUILDER_PROJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lardon3d/project_db.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *project_path;
  Lardon3DProjectDb *database;
  int verifier_kind;
  uint32_t verifier_version;
  const unsigned char *verifier_fingerprint;
  const uint64_t *gvr_ids;
  size_t gvr_count;
} Lardon3DTrackBuilderProjectRequest;

typedef struct {
  uint64_t track_set_id;
  uint64_t gvr_count;
  uint64_t raw_inlier_edge_count;
  uint64_t core_observation_count;
  uint64_t track_count;
  bool reused;
} Lardon3DTrackBuilderProjectResult;

typedef enum {
  LARDON3D_TRACK_BUILDER_PROJECT_OK = 0,
  LARDON3D_TRACK_BUILDER_PROJECT_INVALID_ARGUMENT,
  LARDON3D_TRACK_BUILDER_PROJECT_NOT_FOUND,
  LARDON3D_TRACK_BUILDER_PROJECT_INPUT_CORRUPT,
  LARDON3D_TRACK_BUILDER_PROJECT_DATABASE_ERROR,
  LARDON3D_TRACK_BUILDER_PROJECT_OUT_OF_MEMORY,
  LARDON3D_TRACK_BUILDER_PROJECT_CORE_ERROR,
} Lardon3DTrackBuilderProjectStatus;

Lardon3DTrackBuilderProjectStatus lardon3d_track_builder_build_project(
    const Lardon3DTrackBuilderProjectRequest *request,
    Lardon3DTrackBuilderProjectResult *result);

#ifdef LARDON3D_TRACK_BUILDER_PROJECT_TESTING
/* Test-only phase seam; never present in the production header/API. */
void lardon3d_track_builder_project_test_before_revalidation(
    Lardon3DProjectDb *database, const uint64_t *gvr_ids, size_t gvr_count);
#endif

#ifdef __cplusplus
}
#endif

#endif
