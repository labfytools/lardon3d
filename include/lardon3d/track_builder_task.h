#ifndef LARDON3D_TRACK_BUILDER_TASK_H
#define LARDON3D_TRACK_BUILDER_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include <lardon3d/app_state.h>
#include <lardon3d/task_kind_registry.h>
#include <lardon3d/track_builder_project.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LARDON3D_TRACK_BUILDER_TASK_KIND "track_builder.run"

enum {
  LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION = 1,
};

typedef struct {
  const char *project_path;
  Lardon3DProjectDb *database;
  int verifier_kind;
  uint32_t verifier_version;
  const unsigned char *verifier_fingerprint;
  const uint64_t *gvr_ids;
  size_t gvr_count;
} Lardon3DTrackBuilderTaskConfiguration;

Lardon3DTask *lardon3d_project_create_track_builder_task(
    Lardon3DAppState *state,
    const Lardon3DTrackBuilderTaskConfiguration *configuration,
    uint64_t *task_id);

bool lardon3d_project_enqueue_track_builder_task(
    Lardon3DAppState *state,
    const Lardon3DTrackBuilderTaskConfiguration *configuration,
    uint64_t *task_id);

bool lardon3d_track_builder_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

/* Checked operational envelope for the compact RAM-only implementation.
 * This is admission accounting, not Builder identity or scientific policy. */
bool lardon3d_track_builder_task_memory_estimate(
    uint64_t raw_edge_count, uint64_t feature_set_count,
    uint64_t *memory_bytes);

/* Additive operational estimator used when the exact scope scan has resolved
 * the largest parent Match Result. The legacy E/F estimator remains ABI-stable. */
bool lardon3d_track_builder_task_memory_estimate_with_match_peak(
    uint64_t raw_edge_count, uint64_t feature_set_count,
    uint64_t max_match_count, uint64_t *memory_bytes);

#ifdef __cplusplus
}
#endif

#endif
