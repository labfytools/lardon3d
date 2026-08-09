#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/matcher_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

enum {
  MATCHER_TASK_PAGE_CAPACITY = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH + 1,
  MATCHER_TASK_MEMORY_BYTES = 10 * 1024 * 1024,
  MATCHER_TASK_CPU_THREADS = 12,
};

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DOrbVulkanBackend *orb_vulkan_backend;
  Lardon3DProjectDbMatcherTask parameters;
} Lardon3DMatcherTaskContext;

static void destroy_context(void *userdata) { free(userdata); }

static void runtime_state(const Lardon3DMatcherTaskContext *context,
                          Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  state->orb_vulkan_backend = context->orb_vulkan_backend;
  (void)snprintf(state->project_path, sizeof(state->project_path), "%s",
                 context->project_path);
}

static void finished_callback(const Lardon3DTask *task, void *userdata) {
#ifdef LARDON3D_MATCHER_TASK_TESTING
  const char *skip = getenv("LARDON3D_TEST_MATCHER_SKIP_FINISHED_CHECKPOINT");
  if (skip && strcmp(skip, "1") == 0) {
    return;
  }
#endif
  Lardon3DMatcherTaskContext *context = userdata;
  Lardon3DAppState state;
  runtime_state(context, &state);
  (void)lardon3d_project_checkpoint_matcher_task(&state, task,
                                                 &context->parameters);
}

static uint64_t elapsed_ns(struct timespec begin, struct timespec end) {
  uint64_t seconds =
      end.tv_sec >= begin.tv_sec ? (uint64_t)(end.tv_sec - begin.tv_sec) : 0;
  long nanoseconds = end.tv_nsec - begin.tv_nsec;
  if (nanoseconds < 0 && seconds > 0) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  if (seconds > UINT64_MAX / 1000000000ULL) {
    return UINT64_MAX;
  }
  return seconds * 1000000000ULL + (uint64_t)nanoseconds;
}

static bool load_feature_sets(Lardon3DMatcherTaskContext *context,
                              const Lardon3DProjectDbCandidatePair *pair,
                              Lardon3DProjectDbFeatureSet *feature_set_a,
                              Lardon3DProjectDbFeatureSet *feature_set_b) {
  return lardon3d_project_db_find_feature_set(
             context->database, pair->image_id_a,
             context->parameters.feature_extractor_kind,
             context->parameters.feature_extractor_version,
             context->parameters.feature_parameter_fingerprint,
             feature_set_a) == LARDON3D_PROJECT_DB_OK &&
         lardon3d_project_db_find_feature_set(
             context->database, pair->image_id_b,
             context->parameters.feature_extractor_kind,
             context->parameters.feature_extractor_version,
             context->parameters.feature_parameter_fingerprint,
             feature_set_b) == LARDON3D_PROJECT_DB_OK;
}

static bool process_pair(Lardon3DTask *task,
                         Lardon3DMatcherTaskContext *context,
                         const Lardon3DProjectDbCandidatePair *pair) {
  if (!lardon3d_task_checkpoint(task)) {
    return false;
  }
  Lardon3DProjectDbFeatureSet feature_set_a;
  Lardon3DProjectDbFeatureSet feature_set_b;
  if (!load_feature_sets(context, pair, &feature_set_a, &feature_set_b)) {
    return lardon3d_task_fail(task, "Feature Sets du Matcher introuvables.");
  }
  Lardon3DMatcherParams matcher = {
      .kind = (Lardon3DMatcherKind)context->parameters.matcher_kind,
      .ratio_threshold = context->parameters.ratio_threshold,
  };
  Lardon3DProjectDbMatchResult result;
  if (lardon3d_matcher_match_and_publish_with_backend(
          context->project_path, context->database, pair, &feature_set_a,
          &feature_set_b, &matcher, context->orb_vulkan_backend, &result,
          NULL) != LARDON3D_MATCHER_OK) {
    return lardon3d_task_fail(task,
                              "Matching de la Candidate Pair impossible.");
  }
#ifdef LARDON3D_MATCHER_TASK_TESTING
  const char *pause = getenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION");
  if (pause && strcmp(pause, "1") == 0) {
    (void)lardon3d_task_pause(task);
    return lardon3d_task_checkpoint(task);
  }
#endif
  return true;
}

static bool checkpoint_batch(Lardon3DTask *task,
                             Lardon3DMatcherTaskContext *context,
                             unsigned int progress, uint64_t processed) {
  char message[LARDON3D_TASK_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "Candidate Pairs traitées:%lu",
                 (unsigned long)processed);
  if (!lardon3d_task_set_progress(task, progress, message)) {
    return false;
  }
  Lardon3DAppState state;
  runtime_state(context, &state);
  return lardon3d_project_checkpoint_matcher_task(&state, task,
                                                  &context->parameters) ==
         LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}

static bool run(Lardon3DTask *task, void *userdata) {
  Lardon3DMatcherTaskContext *context = userdata;
  uint64_t total_processed = 0;

  for (;;) {
    if (!lardon3d_task_checkpoint(task)) {
      return false;
    }
    Lardon3DTaskExecutionContract contract;
    if (!lardon3d_task_execution_contract(task, &contract) ||
        contract.batch_size < LARDON3D_MATCHER_TASK_MINIMUM_BATCH ||
        contract.batch_size > LARDON3D_MATCHER_TASK_MAXIMUM_BATCH) {
      return lardon3d_task_fail(task, "Contrat de lot Matcher invalide.");
    }

    Lardon3DProjectDbCandidatePair page[MATCHER_TASK_PAGE_CAPACITY];
    size_t count = 0;
    size_t page_capacity = contract.batch_size + 1;
    if (lardon3d_project_db_list_candidate_pairs(
            context->database, context->parameters.after_candidate_pair_id,
            page, page_capacity, &count) != LARDON3D_PROJECT_DB_OK) {
      return lardon3d_task_fail(task, "Pagination Candidate Pair impossible.");
    }
    if (count == 0) {
      return lardon3d_task_set_progress(task, 100, "Matching terminé.");
    }

    size_t batch_count =
        count < contract.batch_size ? count : contract.batch_size;
    struct timespec begin;
    struct timespec end;
    (void)clock_gettime(CLOCK_MONOTONIC, &begin);
    for (size_t index = 0; index < batch_count; ++index) {
      if (!process_pair(task, context, &page[index])) {
        return false;
      }
      context->parameters.after_candidate_pair_id =
          page[index].candidate_pair_id;
      ++total_processed;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    (void)lardon3d_resource_governor_record_batch(
        context->governor, LARDON3D_RESOURCE_TASK_CPU, batch_count,
        elapsed_ns(begin, end), 0);

    bool exhausted = count <= contract.batch_size;
    unsigned int progress = exhausted ? 100U : 99U;
    if (!checkpoint_batch(task, context, progress, total_processed)) {
      return lardon3d_task_fail(task, "Checkpoint Matcher impossible.");
    }
    if (exhausted) {
      return lardon3d_task_set_progress(task, 100, "Matching terminé.");
    }

#ifdef LARDON3D_MATCHER_TASK_TESTING
    const char *pause_after_batch =
        getenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_BATCH");
    if (pause_after_batch && strcmp(pause_after_batch, "1") == 0) {
      (void)lardon3d_task_pause(task);
      if (!lardon3d_task_checkpoint(task)) {
        return false;
      }
    }
#endif

    Lardon3DResourceReservation *reservation = NULL;
    if (!lardon3d_task_sequence_break(task, context->governor, &reservation,
                                      &contract)) {
      return false;
    }
  }
}

static bool
valid_configuration(const Lardon3DMatcherTaskConfiguration *configuration) {
  if (!configuration ||
      !lardon3d_task_kind_is_valid(configuration->feature_extractor_kind) ||
      configuration->feature_extractor_version == 0) {
    return false;
  }
  bool kind_matches =
      (configuration->matcher.kind == LARDON3D_MATCHER_ORB_BF &&
       strcmp(configuration->feature_extractor_kind, "orb") == 0) ||
      (configuration->matcher.kind == LARDON3D_MATCHER_SIFT_BF &&
       strcmp(configuration->feature_extractor_kind, "sift") == 0) ||
      (configuration->matcher.kind == LARDON3D_MATCHER_ROOTSIFT_BF &&
       strcmp(configuration->feature_extractor_kind, "rootsift") == 0);
  return kind_matches && isfinite(configuration->matcher.ratio_threshold) &&
         configuration->matcher.ratio_threshold > 0.0F &&
         configuration->matcher.ratio_threshold < 1.0F;
}

static Lardon3DMatcherTaskContext *
make_context(const Lardon3DTaskReconstructionContext *runtime,
             const Lardon3DProjectDbMatcherTask *parameters) {
  if (!runtime || !runtime->project_path || !runtime->project_db ||
      !runtime->resource_governor || !parameters) {
    return NULL;
  }
  Lardon3DMatcherTaskContext *context = calloc(1, sizeof(*context));
  if (!context) {
    return NULL;
  }
  int written = snprintf(context->project_path, sizeof(context->project_path),
                         "%s", runtime->project_path);
  if (written <= 0 || (size_t)written >= sizeof(context->project_path)) {
    free(context);
    return NULL;
  }
  context->database = runtime->project_db;
  context->governor = runtime->resource_governor;
  context->orb_vulkan_backend = runtime->orb_vulkan_backend;
  context->parameters = *parameters;
  return context;
}

bool lardon3d_matcher_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  Lardon3DTaskReconstructionContext *runtime = userdata;
  if (!snapshot || !runtime || !binding) {
    return false;
  }
  Lardon3DProjectDbMatcherTask parameters;
  if (lardon3d_project_db_load_matcher_task(runtime->project_db, snapshot->id,
                                            &parameters) !=
      LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  Lardon3DMatcherTaskConfiguration configuration = {
      .feature_extractor_version = parameters.feature_extractor_version,
      .matcher =
          {
              .kind = (Lardon3DMatcherKind)parameters.matcher_kind,
              .ratio_threshold = parameters.ratio_threshold,
          },
  };
  (void)snprintf(configuration.feature_extractor_kind,
                 sizeof(configuration.feature_extractor_kind), "%s",
                 parameters.feature_extractor_kind);
  if (!valid_configuration(&configuration)) {
    return false;
  }
  Lardon3DMatcherTaskContext *context = make_context(runtime, &parameters);
  if (!context) {
    return false;
  }
  *binding = (Lardon3DTaskKindBinding){
      .callback = run,
      .userdata = context,
      .userdata_destroy = destroy_context,
      .finished_callback = finished_callback,
      .finished_userdata = context,
  };
  return true;
}

Lardon3DTask *lardon3d_project_create_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!state || !state->project_loaded || !state->project_db ||
      !state->resource_governor || !task_id ||
      !valid_configuration(configuration)) {
    return NULL;
  }
  uint64_t id = 0;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) !=
      LARDON3D_PROJECT_DB_OK) {
    return NULL;
  }
  Lardon3DProjectDbMatcherTask parameters = {
      .task_id = id,
      .matcher_kind = (int)configuration->matcher.kind,
      .ratio_threshold = configuration->matcher.ratio_threshold,
      .feature_extractor_version = configuration->feature_extractor_version,
  };
  (void)snprintf(parameters.feature_extractor_kind,
                 sizeof(parameters.feature_extractor_kind), "%s",
                 configuration->feature_extractor_kind);
  memcpy(parameters.feature_parameter_fingerprint,
         configuration->feature_parameter_fingerprint,
         sizeof(parameters.feature_parameter_fingerprint));
  Lardon3DTaskReconstructionContext runtime = {
      .project_path = state->project_path,
      .project_db = state->project_db,
      .resource_governor = state->resource_governor,
      .orb_vulkan_backend = state->orb_vulkan_backend,
  };
  Lardon3DMatcherTaskContext *context = make_context(&runtime, &parameters);
  if (!context) {
    return NULL;
  }
  bool may_use_vulkan = configuration->matcher.kind == LARDON3D_MATCHER_ORB_BF &&
                        state->orb_vulkan_backend &&
                        state->hardware_profile.gpu_available;
  const Lardon3DResourceEstimate estimate = {
      .memory_fixed_bytes = MATCHER_TASK_MEMORY_BYTES,
      .gpu_memory_fixed_bytes = may_use_vulkan
                                    ? LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES
                                    : 0,
      .minimum_batch_size = LARDON3D_MATCHER_TASK_MINIMUM_BATCH,
      .maximum_batch_size = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
      .desired_cpu_threads = MATCHER_TASK_CPU_THREADS,
      .desired_gpu_slots = may_use_vulkan ? 1U : 0U,
      .desired_io_slots = 1,
      .task_class = LARDON3D_RESOURCE_TASK_CPU,
  };
  Lardon3DTask *task = lardon3d_task_create_typed(
      "Matching Candidate Pairs", &estimate, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, run, context, destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished_callback, context) ||
      lardon3d_project_checkpoint_matcher_task(state, task, &parameters) !=
          LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

bool lardon3d_project_enqueue_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id) {
  if (!state || !state->task_queue) {
    return false;
  }
  Lardon3DTask *task =
      lardon3d_project_create_matcher_task(state, configuration, task_id);
  if (!task) {
    return false;
  }
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
