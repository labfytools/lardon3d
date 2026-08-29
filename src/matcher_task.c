#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#ifdef LARDON3D_MATCHER_TASK_TESTING
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/matcher_task.h>
#include <lardon3d/feature_extractor.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

#include "matcher_vulkan_config.h"
#include "matcher_internal.h"

enum {
  MATCHER_TASK_PAGE_CAPACITY = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH + 1,
  MATCHER_TASK_MEMORY_BYTES = 10 * 1024 * 1024,
  MATCHER_TASK_CPU_THREADS = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
  MATCHER_TASK_WINDOW_PER_THREAD = 2,
  MATCHER_TASK_WINDOW_MAX = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
  MATCHER_TASK_LEGACY_CPU_THREADS = 12,
};

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DOrbVulkanBackend *orb_vulkan_backend;
  Lardon3DProjectDbMatcherTask parameters;
} Lardon3DMatcherTaskContext;

typedef struct {
  Lardon3DProjectDbCandidatePair pair;
  Lardon3DProjectDbFeatureSet feature_set_a;
  Lardon3DProjectDbFeatureSet feature_set_b;
  Lardon3DMatcherStagedResult staged;
  Lardon3DMatcherResult computed;
} Lardon3DMatcherPairStage;

typedef struct {
  const Lardon3DMatcherTaskContext *context;
  const Lardon3DMatcherParams *matcher;
  Lardon3DOrbVulkanBackend *backend;
  Lardon3DMatcherPairStage *stages;
  size_t count;
  size_t participant;
  size_t participants;
} Lardon3DMatcherWorker;

#ifdef LARDON3D_MATCHER_TASK_TESTING
static atomic_size_t test_vulkan_uses;
static atomic_size_t test_forced_fallbacks;

void lardon3d_matcher_task_test_reset_backend_counters(void) {
  atomic_store(&test_vulkan_uses, 0);
  atomic_store(&test_forced_fallbacks, 0);
}

size_t lardon3d_matcher_task_test_vulkan_uses(void) {
  return atomic_load(&test_vulkan_uses);
}

size_t lardon3d_matcher_task_test_forced_fallbacks(void) {
  return atomic_load(&test_forced_fallbacks);
}
#endif

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

static Lardon3DResourceEstimate matcher_estimate(Lardon3DMatcherTaskMode mode) {
  bool vulkan = mode == LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN;
  return (Lardon3DResourceEstimate){
      .memory_fixed_bytes = 0,
      .gpu_memory_fixed_bytes =
          vulkan ? LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES : 0,
      .memory_bytes_per_item = MATCHER_TASK_MEMORY_BYTES,
      .gpu_memory_bytes_per_item = 0,
      .minimum_batch_size = LARDON3D_MATCHER_TASK_MINIMUM_BATCH,
      .maximum_batch_size = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
      .desired_cpu_threads = vulkan ? 1U : MATCHER_TASK_CPU_THREADS,
      .desired_gpu_slots = vulkan ? 1U : 0U,
      .desired_io_slots = 1,
      .task_class = LARDON3D_RESOURCE_TASK_CPU,
  };
}

static bool estimate_equals(const Lardon3DResourceEstimate *left,
                            const Lardon3DResourceEstimate *right) {
  return left && right &&
         left->memory_fixed_bytes == right->memory_fixed_bytes &&
         left->gpu_memory_fixed_bytes == right->gpu_memory_fixed_bytes &&
         left->memory_bytes_per_item == right->memory_bytes_per_item &&
         left->gpu_memory_bytes_per_item == right->gpu_memory_bytes_per_item &&
         left->minimum_batch_size == right->minimum_batch_size &&
         left->maximum_batch_size == right->maximum_batch_size &&
         left->desired_cpu_threads == right->desired_cpu_threads &&
         left->desired_gpu_slots == right->desired_gpu_slots &&
         left->desired_io_slots == right->desired_io_slots &&
         left->task_class == right->task_class;
}

static Lardon3DResourceEstimate legacy_matcher_estimate(bool vulkan) {
  Lardon3DResourceEstimate estimate = matcher_estimate(
      vulkan ? LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN
             : LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL);
  /* Historical checkpoints reserved the Matcher working set once as fixed
   * memory. Exact reconstruction must recognize that complete old admission
   * shape before converting it to the current per-pair reservation. */
  estimate.memory_fixed_bytes = MATCHER_TASK_MEMORY_BYTES;
  estimate.memory_bytes_per_item = 0;
  estimate.desired_cpu_threads = MATCHER_TASK_LEGACY_CPU_THREADS;
  return estimate;
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

static bool fail_task(Lardon3DTask *task, const char *message) {
  (void)lardon3d_task_fail(task, message);
  /* A successful state transition is not scientific callback success. */
  return false;
}

static bool test_fail_pair(const char *name, uint64_t candidate_pair_id) {
#ifdef LARDON3D_MATCHER_TASK_TESTING
  const char *value = getenv(name);
  if (value) {
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    return end && *end == '\0' && parsed == candidate_pair_id;
  }
#else
  (void)name;
  (void)candidate_pair_id;
#endif
  return false;
}

static void *compute_worker(void *userdata) {
  Lardon3DMatcherWorker *worker = userdata;
  for (size_t index = worker->participant; index < worker->count;
       index += worker->participants) {
    Lardon3DMatcherPairStage *stage = &worker->stages[index];
    if (test_fail_pair("LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
                       stage->pair.candidate_pair_id)) {
      stage->computed = LARDON3D_MATCHER_FAILED;
      continue;
    }
    Lardon3DOrbVulkanBackend *backend = worker->backend;
#ifdef LARDON3D_MATCHER_TASK_TESTING
    const char *force_fallback = getenv("LARDON3D_TEST_MATCHER_FORCE_FALLBACK");
    if (backend && force_fallback && strcmp(force_fallback, "1") == 0) {
      backend = NULL;
      atomic_fetch_add(&test_forced_fallbacks, 1);
    }
#endif
    stage->computed = lardon3d_matcher_stage(
        worker->context->project_path, &stage->feature_set_a,
        &stage->feature_set_b, worker->matcher, backend, &stage->staged);
#ifdef LARDON3D_MATCHER_TASK_TESTING
    if (stage->staged.stats.used_vulkan) {
      atomic_fetch_add(&test_vulkan_uses, 1);
    }
#endif
  }
  return NULL;
}

static void discard_window(Lardon3DMatcherPairStage *stages, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    lardon3d_matcher_discard_staged(&stages[index].staged);
  }
}

static bool compute_window(const Lardon3DMatcherTaskContext *context,
                           const Lardon3DMatcherParams *matcher,
                           Lardon3DMatcherPairStage *stages, size_t count,
                           unsigned int cpu_threads) {
  size_t participants = count < cpu_threads ? count : cpu_threads;
  pthread_t children[MATCHER_TASK_WINDOW_MAX - 1];
  Lardon3DMatcherWorker workers[MATCHER_TASK_WINDOW_MAX];
  size_t launched = 0;
  for (size_t participant = 1; participant < participants; ++participant) {
    workers[participant] = (Lardon3DMatcherWorker){
        .context = context,
        .matcher = matcher,
        .backend = cpu_threads == 1 ? context->orb_vulkan_backend : NULL,
        .stages = stages,
        .count = count,
        .participant = participant,
        .participants = participants,
    };
    if (pthread_create(&children[launched], NULL, compute_worker,
                       &workers[participant]) != 0) {
      break;
    }
    ++launched;
  }
  if (launched + 1 != participants) {
    for (size_t index = 0; index < launched; ++index) {
      (void)pthread_join(children[index], NULL);
    }
    return false;
  }
  workers[0] = (Lardon3DMatcherWorker){
      .context = context,
      .matcher = matcher,
      .backend = cpu_threads == 1 ? context->orb_vulkan_backend : NULL,
      .stages = stages,
      .count = count,
      .participant = 0,
      .participants = participants,
  };
  (void)compute_worker(&workers[0]);
  bool joined = true;
  for (size_t index = 0; index < launched; ++index) {
    if (pthread_join(children[index], NULL) != 0) {
      joined = false;
    }
  }
  return joined;
}

static bool publish_pair(Lardon3DTask *task,
                         Lardon3DMatcherTaskContext *context,
                         const Lardon3DMatcherParams *matcher,
                         Lardon3DMatcherPairStage *stage) {
  if (test_fail_pair("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
                     stage->pair.candidate_pair_id)) {
    return fail_task(task, "Publication Matcher injectée impossible.");
  }
  Lardon3DProjectDbMatchResult result;
  if (lardon3d_matcher_publish_staged(
          context->project_path, context->database, &stage->pair,
          &stage->feature_set_a, &stage->feature_set_b, matcher, &stage->staged,
          &result) != LARDON3D_MATCHER_OK) {
    return fail_task(task, "Matching de la Candidate Pair impossible.");
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
      return fail_task(task, "Contrat de lot Matcher invalide.");
    }

    Lardon3DProjectDbCandidatePair page[MATCHER_TASK_PAGE_CAPACITY];
    size_t count = 0;
    size_t page_capacity = contract.batch_size + 1;
    if (lardon3d_project_db_list_candidate_pairs(
            context->database, context->parameters.after_candidate_pair_id,
            page, page_capacity, &count) != LARDON3D_PROJECT_DB_OK) {
      return fail_task(task, "Pagination Candidate Pair impossible.");
    }
    if (count == 0) {
      return lardon3d_task_set_progress(task, 100, "Matching terminé.");
    }

    size_t batch_count =
        count < contract.batch_size ? count : contract.batch_size;
    struct timespec begin;
    struct timespec end;
    (void)clock_gettime(CLOCK_MONOTONIC, &begin);
    if (contract.cpu_threads == 0 || contract.cpu_threads > MATCHER_TASK_CPU_THREADS) {
      return fail_task(task, "Contrat CPU Matcher invalide.");
    }
    unsigned int previous_opencv_threads = lardon3d_feature_opencv_thread_count();
    if (!lardon3d_feature_opencv_configure_threads(1)) {
      return fail_task(task, "Configuration OpenCV Matcher impossible.");
    }
    size_t processed_in_batch = 0;
    bool batch_ok = true;
    Lardon3DMatcherParams matcher = {
        .kind = (Lardon3DMatcherKind)context->parameters.matcher_kind,
        .ratio_threshold = context->parameters.ratio_threshold,
    };
    while (processed_in_batch < batch_count && batch_ok) {
      size_t remaining = batch_count - processed_in_batch;
      size_t window_count = (size_t)contract.cpu_threads * MATCHER_TASK_WINDOW_PER_THREAD;
      if (window_count > MATCHER_TASK_WINDOW_MAX) window_count = MATCHER_TASK_WINDOW_MAX;
      if (window_count > remaining) window_count = remaining;
      Lardon3DMatcherPairStage stages[MATCHER_TASK_WINDOW_MAX] = {0};
      for (size_t index = 0; index < window_count; ++index) {
        stages[index].pair = page[processed_in_batch + index];
        if (!load_feature_sets(context, &stages[index].pair,
                               &stages[index].feature_set_a,
                               &stages[index].feature_set_b)) {
          batch_ok = false;
          break;
        }
      }
      /* The Queue callback is one admitted compute participant. At most
       * cpu_threads-1 children compute private stages, all are joined before
       * ordered publication and before reservation release/sequence_break. */
      if (batch_ok && !compute_window(context, &matcher, stages, window_count,
                                      contract.cpu_threads)) {
        batch_ok = false;
      }
      for (size_t index = 0; index < window_count && batch_ok; ++index) {
        if (!lardon3d_task_checkpoint(task) ||
            stages[index].computed != LARDON3D_MATCHER_OK ||
            !publish_pair(task, context, &matcher, &stages[index])) {
          batch_ok = false;
          break;
        }
        /* Cursor movement follows only the durable, ascending publication
         * prefix. A failed stage and every later stage remain unpublished. */
        context->parameters.after_candidate_pair_id = stages[index].pair.candidate_pair_id;
        ++processed_in_batch;
        ++total_processed;
      }
      discard_window(stages, window_count);
    }
    (void)lardon3d_feature_opencv_configure_threads(previous_opencv_threads);
    if (!batch_ok) {
      Lardon3DTaskSnapshot snapshot;
      if (lardon3d_task_snapshot(task, &snapshot) && snapshot.state != TASK_FAILED) {
        return fail_task(task, "Calcul Matcher parallèle impossible.");
      }
      return false;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    (void)lardon3d_resource_governor_record_batch(
        context->governor, LARDON3D_RESOURCE_TASK_CPU, batch_count,
        elapsed_ns(begin, end), 0);

    bool exhausted = count <= contract.batch_size;
    unsigned int progress = exhausted ? 100U : 99U;
    if (!checkpoint_batch(task, context, progress, total_processed)) {
      return fail_task(task, "Checkpoint Matcher impossible.");
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
  const Lardon3DResourceEstimate cpu = matcher_estimate(
      LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL);
  const Lardon3DResourceEstimate vulkan =
      matcher_estimate(LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN);
  const Lardon3DResourceEstimate legacy_cpu = legacy_matcher_estimate(false);
  const Lardon3DResourceEstimate legacy_vulkan = legacy_matcher_estimate(true);
  bool current_cpu = estimate_equals(&snapshot->estimate, &cpu);
  bool current_vulkan = estimate_equals(&snapshot->estimate, &vulkan);
  bool historical_cpu = estimate_equals(&snapshot->estimate, &legacy_cpu);
  bool historical_vulkan = estimate_equals(&snapshot->estimate, &legacy_vulkan);
  bool vulkan_mode = current_vulkan || historical_vulkan;
  if ((!current_cpu && !current_vulkan && !historical_cpu &&
       !historical_vulkan) ||
      (vulkan_mode && configuration.matcher.kind != LARDON3D_MATCHER_ORB_BF)) {
    return false;
  }
  Lardon3DMatcherTaskContext *context = make_context(runtime, &parameters);
  if (!context) {
    return false;
  }
  /* Exact whole-estimate signatures select the operational mode. This avoids
   * guessing backend identity from one field and rejects neighboring malformed
   * snapshots. A missing backend after restart remains the already validated
   * exact CPU fallback, while the immutable GPU reservation is retained. */
  if (!vulkan_mode) {
    context->orb_vulkan_backend = NULL;
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

Lardon3DTask *lardon3d_project_create_matcher_task_with_mode(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration,
    Lardon3DMatcherTaskMode mode, uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!state || !state->project_loaded || !state->project_db ||
      !state->resource_governor || !task_id ||
      !valid_configuration(configuration) ||
      (mode != LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL &&
       mode != LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN) ||
      (mode == LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN &&
       (!LARDON3D_HAVE_VULKAN ||
        configuration->matcher.kind != LARDON3D_MATCHER_ORB_BF ||
        !state->hardware_profile.gpu_available ||
        !state->orb_vulkan_backend))) {
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
  bool vulkan_mode = mode == LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN;
  Lardon3DMatcherTaskContext *context = make_context(&runtime, &parameters);
  if (!context) {
    return NULL;
  }
  /* Execution mode is fixed before admission. The Governor may reduce a
   * parallel task to one CPU thread, but that CPU-only task still must not use
   * Vulkan without the GPU resources declared by its immutable estimate. */
  if (!vulkan_mode) {
    context->orb_vulkan_backend = NULL;
  }
  /* Each staged pair can retain the full bounded Matcher working set until
   * ordered publication. The selected immutable estimate covers the entire
   * window and never limits scientific dataset cardinality. */
  Lardon3DResourceEstimate estimate = matcher_estimate(mode);
#ifdef LARDON3D_MATCHER_TASK_TESTING
  /* Tests may reduce CPU fan-out without selecting a backend. Vulkan remains
   * reachable only through the explicit public mode selector above. */
  if (!vulkan_mode) {
    const char *test_threads = getenv("LARDON3D_TEST_MATCHER_CPU_THREADS");
    if (test_threads) {
      char *end = NULL;
      unsigned long parsed = strtoul(test_threads, &end, 10);
      if (end && *end == '\0' && parsed >= 1 &&
          parsed <= MATCHER_TASK_CPU_THREADS) {
        estimate.desired_cpu_threads = (unsigned int)parsed;
      }
    }
  }
#endif
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

Lardon3DTask *lardon3d_project_create_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id) {
  return lardon3d_project_create_matcher_task_with_mode(
      state, configuration, LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL, task_id);
}

bool lardon3d_project_enqueue_matcher_task_with_mode(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration,
    Lardon3DMatcherTaskMode mode, uint64_t *task_id) {
  if (!state || !state->task_queue) {
    return false;
  }
  Lardon3DTask *task =
      lardon3d_project_create_matcher_task_with_mode(state, configuration, mode,
                                                     task_id);
  if (!task) {
    return false;
  }
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}

bool lardon3d_project_enqueue_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id) {
  return lardon3d_project_enqueue_matcher_task_with_mode(
      state, configuration, LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL, task_id);
}
