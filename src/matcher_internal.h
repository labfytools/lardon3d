#ifndef LARDON3D_MATCHER_INTERNAL_H
#define LARDON3D_MATCHER_INTERNAL_H

#include <stdbool.h>

#include <lardon3d/matcher.h>

enum { LARDON3D_MATCHER_STAGED_PATH_CAPACITY = 4096 };

typedef struct {
  char temporary_path[LARDON3D_MATCHER_STAGED_PATH_CAPACITY];
  Lardon3DMatcherStats stats;
} Lardon3DMatcherStagedResult;

typedef struct Lardon3DMatcherPendingVulkanStage Lardon3DMatcherPendingVulkanStage;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define LARDON3D_MATCHER_INTERNAL_VISIBILITY __attribute__((visibility("hidden")))
#else
#define LARDON3D_MATCHER_INTERNAL_VISIBILITY
#endif

/* Computes one pair into an operation-owned temporary Match File without
 * publishing an asset or mutating Project DB. The owner must either publish
 * the stage or discard it; this split is what permits deterministic parallel
 * compute followed by ordered, single-owner durable publication. */
LARDON3D_MATCHER_INTERNAL_VISIBILITY Lardon3DMatcherResult lardon3d_matcher_stage(
    const char *project_path,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params, Lardon3DOrbVulkanBackend *backend,
    Lardon3DMatcherStagedResult *staged);

/* Begin transfers one exact request into the backend and returns one
 * operation-owned pending handle. Finish consumes every non-null handle on
 * every result; discard is the cancellation path. backend_fault is mandatory,
 * starts false, and becomes true only when the corresponding Vulkan begin or
 * finish transaction fails. Local feature I/O, allocation, post-processing or
 * Match File staging errors remain ordinary Matcher failures so the Task does
 * not poison shared backend health. Neither private stage nor any partial GPU
 * evidence may be published by these seams. */
LARDON3D_MATCHER_INTERNAL_VISIBILITY Lardon3DMatcherResult
lardon3d_matcher_begin_vulkan_stage(
    const char *, const Lardon3DProjectDbFeatureSet *,
    const Lardon3DProjectDbFeatureSet *, const Lardon3DMatcherParams *,
    Lardon3DOrbVulkanBackend *, Lardon3DMatcherPendingVulkanStage **, bool *);
LARDON3D_MATCHER_INTERNAL_VISIBILITY Lardon3DMatcherResult
lardon3d_matcher_finish_vulkan_stage(
    Lardon3DMatcherPendingVulkanStage *, Lardon3DMatcherStagedResult *, bool *);
LARDON3D_MATCHER_INTERNAL_VISIBILITY void
lardon3d_matcher_discard_vulkan_stage(Lardon3DMatcherPendingVulkanStage *);

/* Publishes a successful stage using the existing Match Result identity,
 * reuse, repair, and atomic asset rules. This function consumes the stage on
 * every return path and must be called only by the Task callback owner. */
LARDON3D_MATCHER_INTERNAL_VISIBILITY Lardon3DMatcherResult
lardon3d_matcher_publish_staged(
    const char *project_path, Lardon3DProjectDb *database,
    const Lardon3DProjectDbCandidatePair *pair,
    const Lardon3DProjectDbFeatureSet *feature_set_a,
    const Lardon3DProjectDbFeatureSet *feature_set_b,
    const Lardon3DMatcherParams *params, Lardon3DMatcherStagedResult *staged,
    Lardon3DProjectDbMatchResult *result);

LARDON3D_MATCHER_INTERNAL_VISIBILITY void
lardon3d_matcher_discard_staged(Lardon3DMatcherStagedResult *staged);

#undef LARDON3D_MATCHER_INTERNAL_VISIBILITY

#ifdef __cplusplus
}
#endif

#endif
