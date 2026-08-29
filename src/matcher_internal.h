#ifndef LARDON3D_MATCHER_INTERNAL_H
#define LARDON3D_MATCHER_INTERNAL_H

#include <lardon3d/matcher.h>

enum { LARDON3D_MATCHER_STAGED_PATH_CAPACITY = 4096 };

typedef struct {
  char temporary_path[LARDON3D_MATCHER_STAGED_PATH_CAPACITY];
  Lardon3DMatcherStats stats;
} Lardon3DMatcherStagedResult;

#ifdef __cplusplus
extern "C" {
#endif

/* Computes one pair into an operation-owned temporary Match File without
 * publishing an asset or mutating Project DB. The owner must either publish
 * the stage or discard it; this split is what permits deterministic parallel
 * compute followed by ordered, single-owner durable publication. */
Lardon3DMatcherResult lardon3d_matcher_stage(
    const char *project_path,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params, Lardon3DOrbVulkanBackend *backend,
    Lardon3DMatcherStagedResult *staged);

/* Publishes a successful stage using the existing Match Result identity,
 * reuse, repair, and atomic asset rules. This function consumes the stage on
 * every return path and must be called only by the Task callback owner. */
Lardon3DMatcherResult lardon3d_matcher_publish_staged(
    const char *project_path, Lardon3DProjectDb *database,
    const Lardon3DProjectDbCandidatePair *pair,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params, Lardon3DMatcherStagedResult *staged,
    Lardon3DProjectDbMatchResult *result);

void lardon3d_matcher_discard_staged(Lardon3DMatcherStagedResult *staged);

#ifdef __cplusplus
}
#endif

#endif
