#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>

#include "task_internal.h"
#include "visual_index_internal.h"

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DProjectDbVisualIndexUpdateTask parameters;
} VisualIndexTaskContext;

static void destroy_context(void *userdata) { free(userdata); }

static void runtime_state(const VisualIndexTaskContext *context, Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  snprintf(state->project_path, sizeof(state->project_path), "%s", context->project_path);
}

static uint64_t elapsed_ns(struct timespec begin, struct timespec end) {
  uint64_t seconds = end.tv_sec >= begin.tv_sec ? (uint64_t)(end.tv_sec - begin.tv_sec) : 0;
  long nanoseconds = end.tv_nsec - begin.tv_nsec;
  if (nanoseconds < 0 && seconds > 0) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  return seconds <= UINT64_MAX / 1000000000ULL
             ? seconds * 1000000000ULL + (uint64_t)nanoseconds
             : UINT64_MAX;
}

static void finished(const Lardon3DTask *task, void *userdata) {
#ifdef LARDON3D_VISUAL_INDEX_TASK_TESTING
  const char *skip = getenv("LARDON3D_TEST_VISUAL_INDEX_SKIP_FINISHED_CHECKPOINT");
  if (skip && strcmp(skip, "1") == 0) {
    return;
  }
#endif
  VisualIndexTaskContext *context = userdata;
  Lardon3DAppState state;
  runtime_state(context, &state);
  (void)lardon3d_project_checkpoint_visual_index_update_task(&state, task,
                                                             &context->parameters);
}

static bool run(Lardon3DTask *task, void *userdata) {
  VisualIndexTaskContext *context = userdata;
  Lardon3DAppState state;
  runtime_state(context, &state);
  for (;;) {
    if (!lardon3d_task_checkpoint(task)) {
      return false;
    }
    Lardon3DTaskExecutionContract contract;
    if (!lardon3d_task_execution_contract(task, &contract) || contract.batch_size == 0 ||
        contract.batch_size > LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX ||
        contract.cpu_threads == 0 ||
        contract.cpu_threads > LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX) {
      return lardon3d_task_fail(task, "Contrat Visual Index invalide.");
    }
    struct timespec begin = {0};
    struct timespec end = {0};
    bool timing_known = clock_gettime(CLOCK_MONOTONIC, &begin) == 0;
    uint64_t last = context->parameters.after_feature_set_id;
    size_t indexed = 0;
    Lardon3DVisualIndexResult result = lardon3d_visual_index_update_once_parallel(
        context->project_path, context->database, context->parameters.visual_index_id,
        lardon3d_task_id(task), context->parameters.after_feature_set_id, contract.batch_size,
        contract.cpu_threads, &last, &indexed);
    timing_known = timing_known && clock_gettime(CLOCK_MONOTONIC, &end) == 0;
    if (result == LARDON3D_VISUAL_INDEX_NO_CHANGE) {
      return lardon3d_task_set_progress(task, 100, "Visual Index à jour.");
    }
    if (result != LARDON3D_VISUAL_INDEX_OK &&
        result != LARDON3D_VISUAL_INDEX_PUBLISHED_NOT_DURABLE) {
      return lardon3d_task_fail(task, "Mise à jour Visual Index impossible.");
    }
    context->parameters.after_feature_set_id = last;
    size_t durable_indexed = result == LARDON3D_VISUAL_INDEX_OK ? indexed : 0;
    uint64_t duration_ns = timing_known ? elapsed_ns(begin, end) : 0;
    if (durable_indexed > 0 && duration_ns > 0) {
      lardon3d_resource_governor_record_batch(
          context->governor, LARDON3D_RESOURCE_TASK_CPU, durable_indexed,
          duration_ns, 0);
    }
    /* ALGORITHMIC: one participant owns one disjoint Feature Set slice, so the
     * immutable admitted CPU count cannot exceed the 16-member segment bound.
     * A segment whose directory publication is not durable remains visible for
     * restart semantics but is zero operational work and cannot train the next
     * sequence's portable CPU trial. */
    if (duration_ns > 0) {
      (void)lardon3d_task_internal_record_sequence(
          task, duration_ns, durable_indexed);
    }
    if (!lardon3d_task_set_progress(task, 99, "Segment Visual Index publié.") ||
        lardon3d_project_checkpoint_visual_index_update_task(&state, task,
                                                             &context->parameters) !=
            LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
      return lardon3d_task_fail(task, "Checkpoint Visual Index impossible.");
    }
#ifdef LARDON3D_VISUAL_INDEX_TASK_TESTING
    const char *pause = getenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT");
    if (pause && strcmp(pause, "1") == 0) {
      (void)lardon3d_task_pause(task);
    }
#endif
    Lardon3DResourceReservation *reservation = NULL;
    if (!lardon3d_task_sequence_break(task, context->governor, &reservation, &contract)) {
      return false;
    }
  }
}

static VisualIndexTaskContext *make_context(
    const Lardon3DTaskReconstructionContext *runtime,
    const Lardon3DProjectDbVisualIndexUpdateTask *parameters) {
  if (!runtime || !runtime->project_path || !runtime->project_db ||
      !runtime->resource_governor || !parameters) {
    return NULL;
  }
  VisualIndexTaskContext *context = calloc(1, sizeof(*context));
  if (!context) {
    return NULL;
  }
  int written = snprintf(context->project_path, sizeof(context->project_path), "%s",
                         runtime->project_path);
  if (written <= 0 || (size_t)written >= sizeof(context->project_path)) {
    free(context);
    return NULL;
  }
  context->database = runtime->project_db;
  context->governor = runtime->resource_governor;
  context->parameters = *parameters;
  return context;
}

bool lardon3d_visual_index_update_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
                                              void *userdata,
                                              Lardon3DTaskKindBinding *binding) {
  Lardon3DTaskReconstructionContext *runtime = userdata;
  if (!snapshot || !runtime || !binding) {
    return false;
  }
  Lardon3DProjectDbVisualIndexUpdateTask parameters;
  if (lardon3d_project_db_load_visual_index_update_task(runtime->project_db, snapshot->id,
                                                        &parameters) != LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  VisualIndexTaskContext *context = make_context(runtime, &parameters);
  if (!context) {
    return false;
  }
  *binding = (Lardon3DTaskKindBinding){.callback = run,
                                       .userdata = context,
                                       .userdata_destroy = destroy_context,
                                       .finished_callback = finished,
                                       .finished_userdata = context};
  return true;
}

Lardon3DTask *lardon3d_project_create_visual_index_update_task(
    Lardon3DAppState *state, uint64_t visual_index_id, uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!state || !state->project_loaded || !state->project_db || !state->resource_governor ||
      !task_id || visual_index_id == 0) {
    return NULL;
  }
  Lardon3DProjectDbVisualIndex index;
  if (lardon3d_project_db_load_visual_index(state->project_db, visual_index_id, &index) !=
      LARDON3D_PROJECT_DB_OK) {
    return NULL;
  }
  uint64_t id = 0;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK) {
    return NULL;
  }
  Lardon3DProjectDbVisualIndexUpdateTask parameters = {
      .task_id = id,
      .visual_index_id = visual_index_id,
      .after_feature_set_id = 0,
  };
  Lardon3DTaskReconstructionContext runtime = {
      .project_path = state->project_path,
      .project_db = state->project_db,
      .resource_governor = state->resource_governor,
      .orb_vulkan_backend = state->orb_vulkan_backend,
  };
  VisualIndexTaskContext *context = make_context(&runtime, &parameters);
  if (!context) {
    return NULL;
  }
  Lardon3DResourceEstimate estimate = {.memory_fixed_bytes = 8ULL * 1024 * 1024,
                                       /* RESOURCE: includes each bounded 512 KiB reader stack and
                                        * its per-Feature-Set staging/reader state. */
                                       .memory_bytes_per_item = 2ULL * 1024 * 1024,
                                       .minimum_batch_size = 1,
                                       .maximum_batch_size =
                                           LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX,
                                       /* ALGORITHMIC: no segment contains more
                                        * independent work than this bound. */
                                       .desired_cpu_threads =
                                           LARDON3D_VISUAL_INDEX_SEGMENT_FEATURE_SET_MAX,
                                       .desired_io_slots = 1,
                                       .task_class = LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DTask *task = lardon3d_task_create_typed(
      "Mise à jour Visual Index", &estimate, LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND,
      LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND_VERSION, run, context, destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished, context) ||
      lardon3d_project_checkpoint_visual_index_update_task(state, task, &parameters) !=
          LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

bool lardon3d_project_enqueue_visual_index_update(Lardon3DAppState *state,
                                                  uint64_t visual_index_id,
                                                  uint64_t *task_id) {
  if (!state || !state->task_queue) {
    return false;
  }
  Lardon3DTask *task =
      lardon3d_project_create_visual_index_update_task(state, visual_index_id, task_id);
  if (!task) {
    return false;
  }
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
