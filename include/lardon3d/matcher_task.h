#ifndef LARDON3D_MATCHER_TASK_H
#define LARDON3D_MATCHER_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include <lardon3d/app_state.h>
#include <lardon3d/matcher.h>
#include <lardon3d/task_kind_registry.h>

#define LARDON3D_MATCHER_TASK_KIND "matcher.run"

enum {
  LARDON3D_MATCHER_TASK_KIND_VERSION = 1,
  LARDON3D_MATCHER_TASK_MINIMUM_BATCH = 1,
  LARDON3D_MATCHER_TASK_MAXIMUM_BATCH = 8,
};

typedef struct {
  char feature_extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t feature_extractor_version;
  unsigned char feature_parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  Lardon3DMatcherParams matcher;
} Lardon3DMatcherTaskConfiguration;

Lardon3DTask *lardon3d_project_create_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id);

bool lardon3d_project_enqueue_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id);

bool lardon3d_matcher_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

#endif
