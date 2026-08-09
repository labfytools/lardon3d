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

#ifdef __cplusplus
}
#endif

#endif
