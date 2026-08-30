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
  /* This is an operational window and participant ceiling, not a scientific
   * dataset limit. Every staged pair remains private until ordered publish. */
  LARDON3D_MATCHER_TASK_MAXIMUM_BATCH = 12,
};

typedef struct {
  char feature_extractor_kind[LARDON3D_PROJECT_DB_KIND_CAPACITY];
  uint32_t feature_extractor_version;
  unsigned char feature_parameter_fingerprint[LARDON3D_PROJECT_DB_SHA256_SIZE];
  Lardon3DMatcherParams matcher;
} Lardon3DMatcherTaskConfiguration;

/* Selects only an explicit debug/benchmark/reproduction override. Normal
 * create/enqueue is Governor-owned AUTO: validated ORB Vulkan is preferred
 * when runtime support and admission are safe, otherwise the complete CPU
 * implementation is selected. Matcher parameters, fingerprints, Match
 * Results, and Match Files are identical across operational selections.
 * SIFT/RootSIFT remain CPU-only. */
typedef enum {
  LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL = 0,
  LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN = 1,
} Lardon3DMatcherTaskMode;

Lardon3DTask *lardon3d_project_create_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id);

/* Creates a Matcher task with an explicit immutable operational mode. On
 * success, task_id receives the durable Task ID and the caller owns the Task.
 * Invalid or unavailable mode selections return NULL and leave task_id zero. */
Lardon3DTask *lardon3d_project_create_matcher_task_with_mode(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration,
    Lardon3DMatcherTaskMode mode, uint64_t *task_id);

bool lardon3d_project_enqueue_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id);

/* Creates and transfers an explicitly selected Matcher task to state's Queue.
 * Failure leaves task_id zero unless durable task creation succeeded before a
 * Queue insertion failure, matching the existing enqueue ownership contract. */
bool lardon3d_project_enqueue_matcher_task_with_mode(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration,
    Lardon3DMatcherTaskMode mode, uint64_t *task_id);

bool lardon3d_matcher_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);

#endif
