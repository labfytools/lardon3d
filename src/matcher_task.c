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
#include "matcher_task_benchmark_internal.h"
#include "orb_vulkan_backend_internal.h"
#include "resource_governor_internal.h"
#include "task_internal.h"

enum {
  MATCHER_TASK_PAGE_CAPACITY = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH + 1,
  MATCHER_TASK_MEMORY_BYTES = 10 * 1024 * 1024,
  MATCHER_TASK_CPU_THREADS = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
  MATCHER_TASK_WINDOW_PER_THREAD = 2,
  MATCHER_TASK_WINDOW_MAX = LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
  MATCHER_TASK_FIXED_LEGACY_CPU_THREADS = 12,
  MATCHER_TASK_PREVIOUS_CPU_THREADS = 8,
  /* The forced item-valid batch matrix is retained under
   * governor-v2-evidence/forced-batch{2,4,8,12}-items{,-b}.stdout.jsonl.
   * Combined cohort rate is (2*4113*1e9)/sum(wall_ns): 54.180767704,
   * 66.094373197, 74.784998723 and 76.755814095 pairs/s. Gains are
   * +21.988624373%, +13.148812987% and +2.635308425%; batch twelve is below
   * the 5% deadband. Safety remains proved through twelve for private forced
   * evidence, while normal AUTO exposes the useful maximum eight only. */
  MATCHER_TASK_BATCH_MAX_USEFUL = 8,
  MATCHER_TASK_BATCH_MAX_VALIDATED_SAFETY =
      LARDON3D_MATCHER_TASK_MAXIMUM_BATCH,
  /* Depth two is structurally safe and remains reproducible in private tests.
   * Controlled ABBA evidence is retained under
   * /home/fy59/Documents/Lardon/.real-pre-sfm-2026-08-30/
   * governor-v2-evidence/forced-depth{1,2}-{a,b}.stdout.jsonl. Each run records
   * 4113 durable pairs. Cohort rate is (2*4113*1e9)/sum(wall_ns), not
   * mean(per-run rate): 54.661652238 vs 55.797311953 pairs/s, +2.077617%.
   * That is below the established 5% throughput deadband, so normal AUTO
   * exposes useful depth one only. */
  MATCHER_TASK_DEPTH_MAX_USEFUL = 1,
  MATCHER_TASK_DEPTH_MAX_VALIDATED_SAFETY = LARDON3D_ORB_VULKAN_MAX_INFLIGHT,
};

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DOrbVulkanBackend *orb_vulkan_backend;
  bool normal_auto;
  bool auto_vulkan_available;
  bool explicit_vulkan;
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  /* Benchmark-only and Task-private. This operational control is reconstructed
   * from the opt-in runner process, never from Project DB or checkpoint state;
   * normal production builds contain neither the field nor its environment
   * token. The installed execution contract remains immutable either way. */
  bool benchmark_synchronous_pipeline;
  size_t benchmark_inflight_override;
  size_t benchmark_batch_override;
#endif
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
static atomic_size_t test_overlap_publications;
static atomic_uint_fast64_t test_max_retained_vulkan_payload;
enum {
  TEST_EVENT_GPU_SUBMIT = 1,
  TEST_EVENT_GPU_FINISH = 2,
  TEST_EVENT_PUBLICATION_START = 3,
  TEST_EVENT_PUBLICATION_FINISH = 4,
  TEST_EVENT_CAPACITY = 256,
};
typedef struct {
  int kind;
  uint64_t candidate_pair_id;
  size_t order;
} Lardon3DMatcherTaskTestEvent;
static Lardon3DMatcherTaskTestEvent test_events[TEST_EVENT_CAPACITY];
static atomic_size_t test_event_count;

static void test_record_event(int kind, uint64_t candidate_pair_id) {
  size_t order = atomic_fetch_add(&test_event_count, 1);
  if (order < TEST_EVENT_CAPACITY) {
    test_events[order] = (Lardon3DMatcherTaskTestEvent){
        .kind = kind,
        .candidate_pair_id = candidate_pair_id,
        .order = order,
    };
  }
}

void lardon3d_matcher_task_test_reset_backend_counters(void) {
  atomic_store(&test_vulkan_uses, 0);
  atomic_store(&test_forced_fallbacks, 0);
  atomic_store(&test_overlap_publications, 0);
  atomic_store(&test_max_retained_vulkan_payload, 0);
  atomic_store(&test_event_count, 0);
  memset(test_events, 0, sizeof(test_events));
}

size_t lardon3d_matcher_task_test_vulkan_uses(void) {
  return atomic_load(&test_vulkan_uses);
}

size_t lardon3d_matcher_task_test_forced_fallbacks(void) {
  return atomic_load(&test_forced_fallbacks);
}

uint64_t lardon3d_matcher_task_test_max_retained_vulkan_payload(void) {
  return atomic_load(&test_max_retained_vulkan_payload);
}

size_t lardon3d_matcher_task_test_overlap_publications(void) {
  return atomic_load(&test_overlap_publications);
}

size_t lardon3d_matcher_task_test_event_count(void) {
  size_t count = atomic_load(&test_event_count);
  return count < TEST_EVENT_CAPACITY ? count : TEST_EVENT_CAPACITY;
}

bool lardon3d_matcher_task_test_event(
    size_t index, int *kind, uint64_t *candidate_pair_id, size_t *order) {
  if (!kind || !candidate_pair_id || !order ||
      index >= lardon3d_matcher_task_test_event_count()) {
    return false;
  }
  *kind = test_events[index].kind;
  *candidate_pair_id = test_events[index].candidate_pair_id;
  *order = test_events[index].order;
  return true;
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

static bool auto_vulkan_backend_candidate(Lardon3DOrbVulkanBackend *backend) {
  Lardon3DOrbVulkanInfo info;
  return backend && lardon3d_orb_vulkan_backend_info(backend, &info) &&
         (!info.initialized || info.available);
}

static bool auto_vulkan_runtime_candidate(const Lardon3DAppState *state) {
  /* AUTO creation is caller-thread metadata work only. Memory sizing does not
   * belong here: the Governor owns the exact reconstructed batch/depth, UMA
   * charge, current MemAvailable/PSI/swap snapshot, and the 3 GiB hard reserve
   * plus the 3--4 GiB caution policy.
   * A caller-side maximum-window guess could suppress a safe depth-1 contract
   * before CPU fallback was even considered. Driver initialization remains
   * deferred to begin() on Queue's affinity-constrained worker. */
  return LARDON3D_HAVE_VULKAN && state->hardware_profile.gpu_available &&
         auto_vulkan_backend_candidate(state->orb_vulkan_backend);
}

static Lardon3DTaskCapabilityEnvelope matcher_auto_envelope(
    const Lardon3DResourceEstimate *cpu, bool expose_vulkan,
    bool allow_depth_two, size_t benchmark_inflight_override,
    size_t benchmark_batch_override) {
  Lardon3DResourceEstimate vulkan = matcher_estimate(
      LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN);
  /* The AUTO operation is semantically MIXED even when this admission selects
   * one CPU or Vulkan capability. Preserve that truthful class in each
   * operational alternative; backend choice remains private and ephemeral. */
  vulkan.task_class = cpu->task_class;
  vulkan.maximum_batch_size = MATCHER_TASK_BATCH_MAX_USEFUL;
  /* The durable AUTO estimate remains the historical minimum depth-1
   * signature. Operational slot payload is reconstructed here: 2*8192*32
   * descriptor bytes plus 8192*4*uint32 readback = 655360 bytes per slot,
   * with no invented fixed charge for opaque shared driver objects. */
  vulkan.gpu_memory_fixed_bytes = LARDON3D_ORB_VULKAN_FIXED_BYTES;
  size_t maximum_inflight = MATCHER_TASK_DEPTH_MAX_USEFUL;
  size_t minimum_inflight = MATCHER_TASK_DEPTH_MAX_USEFUL;
  bool inflight_adaptive = false;
  bool benchmark_forced_vulkan_only = false;
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  if (expose_vulkan && benchmark_inflight_override != 0) {
    /* Benchmark matrices still travel through the normal Governor. Fixed
     * batch/depth remove adaptive-history confounding, while the one chosen
     * capability remains fully charged and immutable for every sequence. */
    minimum_inflight = benchmark_inflight_override;
    maximum_inflight = benchmark_inflight_override;
    inflight_adaptive = false;
    size_t batch = benchmark_batch_override != 0
        ? benchmark_batch_override : 2;
    vulkan.minimum_batch_size = batch;
    vulkan.maximum_batch_size = batch;
    benchmark_forced_vulkan_only = true;
  }
#else
  (void)benchmark_inflight_override;
  (void)benchmark_batch_override;
#endif
  (void)allow_depth_two;
#ifdef LARDON3D_MATCHER_TASK_TESTING
  /* Test-only deterministic admission seam: exercise production depth-two
   * ordering independently of rolling feedback history. Normal binaries do
   * not compile or respond to this token. */
  const char *forced_inflight = getenv("LARDON3D_TEST_MATCHER_INFLIGHT_LIMIT");
  if (expose_vulkan && benchmark_inflight_override == 0 && allow_depth_two
      && forced_inflight
      && strcmp(forced_inflight, "2") == 0) {
    minimum_inflight = MATCHER_TASK_DEPTH_MAX_VALIDATED_SAFETY;
    maximum_inflight = MATCHER_TASK_DEPTH_MAX_VALIDATED_SAFETY;
    inflight_adaptive = false;
    vulkan.minimum_batch_size = 2;
    vulkan.maximum_batch_size = 2;
  }
#endif
  Lardon3DTaskCapabilityEnvelope envelope = {
      /* A forced A/B cohort is evidence about one admitted Vulkan contract,
       * not AUTO fallback policy. Exposing only that capability makes GPU,
       * backend, UMA, and memory non-admission reject the experiment instead
       * of silently selecting adaptive CPU. Normal AUTO still exposes both. */
      .count = expose_vulkan ? benchmark_forced_vulkan_only ? 1 : 2 : 1,
      .capabilities = {
          {
              .estimate = expose_vulkan ? vulkan : *cpu,
              .backend = expose_vulkan
                             ? LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
                             : LARDON3D_RESOURCE_BACKEND_CPU,
              .inflight_limit = expose_vulkan ? maximum_inflight : 1,
              .minimum_inflight_limit = expose_vulkan ? minimum_inflight : 0,
              .gpu_memory_bytes_per_inflight =
                  expose_vulkan ? LARDON3D_ORB_VULKAN_PER_SLOT_BYTES : 0,
              .preferred = expose_vulkan,
              .cpu_reducible = !expose_vulkan,
              .batch_adaptive = expose_vulkan
                  ? vulkan.minimum_batch_size != vulkan.maximum_batch_size
                  : true,
              .sustained_gpu_batch_feedback = expose_vulkan
                  && !benchmark_forced_vulkan_only
                  && vulkan.minimum_batch_size != vulkan.maximum_batch_size,
              .inflight_adaptive = expose_vulkan && inflight_adaptive,
              .requires_runtime_backend = expose_vulkan,
          },
          {
              .estimate = *cpu,
              .backend = LARDON3D_RESOURCE_BACKEND_CPU,
              .inflight_limit = 1,
              /* CPU participants are the admitted cpu_threads dimension.
               * helpers remain zero until a distinct GPU helper is admitted. */
              .helper_limit = 0,
              .cpu_reducible = true,
              .batch_adaptive = true,
          },
      },
  };
  return envelope;
}

#ifdef LARDON3D_MATCHER_TASK_TESTING
bool lardon3d_matcher_task_test_auto_capability_envelope(
    size_t benchmark_inflight_override, size_t benchmark_batch_override,
    Lardon3DTaskCapabilityEnvelope *envelope) {
  if (!envelope) return false;
  Lardon3DResourceEstimate cpu = matcher_estimate(
      LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL);
  cpu.task_class = LARDON3D_RESOURCE_TASK_MIXED;
  *envelope = matcher_auto_envelope(
      &cpu, true, true, benchmark_inflight_override,
      benchmark_batch_override);
  return true;
}
#endif

static Lardon3DTaskCapabilityEnvelope matcher_fixed_envelope(
    const Lardon3DResourceEstimate *estimate, bool vulkan) {
  return (Lardon3DTaskCapabilityEnvelope){
      .count = 1,
      .capabilities = {{
          .estimate = *estimate,
          .backend = vulkan ? LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
                            : LARDON3D_RESOURCE_BACKEND_CPU,
          .inflight_limit = 1,
          .helper_limit = 0,
          .cpu_reducible = !vulkan,
      }},
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
  estimate.maximum_batch_size = 8;
  estimate.desired_cpu_threads = MATCHER_TASK_FIXED_LEGACY_CPU_THREADS;
  return estimate;
}

static Lardon3DResourceEstimate previous_matcher_estimate(bool vulkan) {
  Lardon3DResourceEstimate estimate = matcher_estimate(
      vulkan ? LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN
             : LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL);
  estimate.maximum_batch_size = 8;
  estimate.desired_cpu_threads = vulkan ? 1U : MATCHER_TASK_PREVIOUS_CPU_THREADS;
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

static void saturating_add_ns(uint64_t *total, uint64_t value) {
  *total = *total > UINT64_MAX - value ? UINT64_MAX : *total + value;
}

static uint64_t cumulative_delta(uint64_t before, uint64_t after) {
  return after >= before ? after - before : 0;
}

static void add_vulkan_telemetry_delta(
    const Lardon3DOrbVulkanTelemetry *before,
    const Lardon3DOrbVulkanTelemetry *after,
    Lardon3DResourceExecutionMetrics *metrics) {
  metrics->vulkan_submits = cumulative_delta(before->submits, after->submits);
  metrics->vulkan_completions =
      cumulative_delta(before->completions, after->completions);
  metrics->vulkan_submit_cpu_ns =
      cumulative_delta(before->submit_cpu_ns, after->submit_cpu_ns);
  metrics->vulkan_fence_wait_ns =
      cumulative_delta(before->fence_wait_ns, after->fence_wait_ns);
  metrics->vulkan_readback_ns =
      cumulative_delta(before->readback_ns, after->readback_ns);
  metrics->vulkan_gpu_time_known = after->gpu_timestamps_available
      && metrics->vulkan_completions > 0;
  metrics->vulkan_gpu_ns =
      cumulative_delta(before->gpu_execution_ns, after->gpu_execution_ns);
  metrics->vulkan_starvation_ns =
      cumulative_delta(before->starvation_ns, after->starvation_ns);
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
        .backend = NULL,
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
      .backend = NULL,
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
                         Lardon3DMatcherPairStage *stage,
                         uint64_t *publication_ns) {
  if (test_fail_pair("LARDON3D_TEST_MATCHER_FAIL_PUBLISH_PAIR_ID",
                     stage->pair.candidate_pair_id)) {
    return fail_task(task, "Publication Matcher injectée impossible.");
  }
  Lardon3DProjectDbMatchResult result;
  struct timespec begin;
  struct timespec end;
  (void)clock_gettime(CLOCK_MONOTONIC, &begin);
  Lardon3DMatcherResult published = lardon3d_matcher_publish_staged(
          context->project_path, context->database, &stage->pair,
          &stage->feature_set_a, &stage->feature_set_b, matcher, &stage->staged,
          &result);
  (void)clock_gettime(CLOCK_MONOTONIC, &end);
  if (publication_ns) {
    saturating_add_ns(publication_ns, elapsed_ns(begin, end));
  }
  if (published != LARDON3D_MATCHER_OK) {
    return fail_task(task, "Matching de la Candidate Pair impossible.");
  }
  return true;
}

static bool checkpoint_after_publication(Lardon3DTask *task) {
#ifdef LARDON3D_MATCHER_TASK_TESTING
  const char *pause = getenv("LARDON3D_TEST_MATCHER_PAUSE_AFTER_PUBLICATION");
  if (pause && strcmp(pause, "1") == 0) {
    (void)lardon3d_task_pause(task);
    return lardon3d_task_checkpoint(task);
  }
#else
  (void)task;
#endif
  return true;
}

typedef enum {
  VULKAN_SUBMISSION_EMPTY = 0,
  VULKAN_SUBMISSION_SUBMITTED,
  VULKAN_SUBMISSION_LOCAL_INELIGIBLE,
  VULKAN_SUBMISSION_OTHER_FAILED,
  VULKAN_SUBMISSION_BACKEND_FAILED,
} Lardon3DMatcherVulkanSubmissionState;

typedef enum {
  MATCHER_FALLBACK_NONE = 0,
  MATCHER_FALLBACK_LOCAL_INELIGIBLE,
  MATCHER_FALLBACK_BACKEND_FAILURE,
  MATCHER_FALLBACK_OTHER,
} Lardon3DMatcherFallbackCause;

typedef struct {
  Lardon3DMatcherVulkanSubmissionState state;
  Lardon3DMatcherPendingVulkanStage *pending;
} Lardon3DMatcherVulkanSubmission;

static void note_completed_fallback_item(
    Lardon3DResourceExecutionMetrics *metrics,
    Lardon3DMatcherFallbackCause cause) {
  uint64_t *counter = NULL;
  switch (cause) {
    case MATCHER_FALLBACK_NONE:
      return;
    case MATCHER_FALLBACK_LOCAL_INELIGIBLE:
      counter = &metrics->local_ineligible_fallback_items;
      break;
    case MATCHER_FALLBACK_BACKEND_FAILURE:
      counter = &metrics->backend_failure_fallback_items;
      break;
    case MATCHER_FALLBACK_OTHER:
      counter = &metrics->backend_other_fallback_items;
      break;
  }
  if (*counter == UINT64_MAX) {
    metrics->fallback_items_saturated = true;
  } else {
    ++*counter;
  }
}

static void commit_completed_fallback_item(
    Lardon3DTask *task, uint64_t candidate_pair_id,
    Lardon3DResourceExecutionMetrics *metrics,
    Lardon3DMatcherFallbackCause cause) {
  if (cause == MATCHER_FALLBACK_NONE) {
    return;
  }
  note_completed_fallback_item(metrics, cause);
  Lardon3DResourceFallbackItemCause governor_cause =
      cause == MATCHER_FALLBACK_LOCAL_INELIGIBLE
          ? LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE
          : cause == MATCHER_FALLBACK_BACKEND_FAILURE
              ? LARDON3D_RESOURCE_FALLBACK_ITEM_BACKEND_FAILURE
              : LARDON3D_RESOURCE_FALLBACK_ITEM_OTHER;
  /* CONTRACT: publication above is already durable. Commit the operational
   * item class now so a later pair's cancellation/computation/publication
   * failure cannot erase this prefix. Task's current-run watermark makes an
   * in-process retry idempotent; this does not create throughput feedback. */
  (void)lardon3d_task_internal_record_fallback_item(
      task, candidate_pair_id, governor_cause);
}

static void note_vulkan_backend_failure(
    Lardon3DMatcherTaskContext *context, bool *backend_failed) {
  *backend_failed = true;
  /* Backend failure is shared execution evidence, regardless of whether the
   * failing Task was AUTO or an explicit diagnostic override. Publish it to
   * the Governor immediately: later CPU fallback, cancellation, or durable
   * publication may fail and must not leave a broken backend advertised. */
  (void)lardon3d_resource_governor_internal_set_backend_available(
      context->governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN, false);
}

static Lardon3DMatcherVulkanSubmission begin_vulkan_submission(
    Lardon3DMatcherTaskContext *context, const Lardon3DMatcherParams *matcher,
    const Lardon3DMatcherPairStage *stage, bool *backend_failed,
    bool *vulkan_ineligible, bool *vulkan_other_failure) {
  Lardon3DMatcherPendingVulkanStage *pending = NULL;
  Lardon3DMatcherResult result;
  bool backend_fault = false;
  if (test_fail_pair("LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID",
                     stage->pair.candidate_pair_id)) {
    result = LARDON3D_MATCHER_INVALID_ARGUMENT;
  } else if (test_fail_pair("LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
                            stage->pair.candidate_pair_id)) {
    result = LARDON3D_MATCHER_FAILED;
    backend_fault = true;
  } else if (test_fail_pair(
                 "LARDON3D_TEST_MATCHER_FAIL_LOCAL_VULKAN_BEGIN_PAIR_ID",
                 stage->pair.candidate_pair_id)) {
    result = LARDON3D_MATCHER_IO_ERROR;
  } else {
    result = lardon3d_matcher_begin_vulkan_stage(
        context->project_path, &stage->feature_set_a, &stage->feature_set_b,
        matcher, context->orb_vulkan_backend, &pending, &backend_fault);
  }
  if (result == LARDON3D_MATCHER_OK && pending) {
    return (Lardon3DMatcherVulkanSubmission) {
        .state = VULKAN_SUBMISSION_SUBMITTED,
        .pending = pending,
    };
  }
  if (pending) {
    /* The private begin contract should return a handle only with OK. Keep
     * cleanup deterministic even if a backend violates that contract. */
    lardon3d_matcher_discard_vulkan_stage(pending);
    pending = NULL;
  }
  if (result == LARDON3D_MATCHER_INVALID_ARGUMENT) {
    /* Eligibility belongs to this complete pair only. It neither consumes a
     * pending slot nor changes shared backend health. */
    *vulkan_ineligible = true;
    return (Lardon3DMatcherVulkanSubmission) {
        .state = VULKAN_SUBMISSION_LOCAL_INELIGIBLE,
    };
  }
  if (!backend_fault) {
    /* Feature I/O, allocation and other pre-submit faults consume no request
     * and cannot say anything about the shared backend. This pair alone falls
     * back to CPU while already-submitted or later successors remain valid. */
    *vulkan_other_failure = true;
    return (Lardon3DMatcherVulkanSubmission) {
        .state = VULKAN_SUBMISSION_OTHER_FAILED,
    };
  }
  note_vulkan_backend_failure(context, backend_failed);
  return (Lardon3DMatcherVulkanSubmission) {
      .state = VULKAN_SUBMISSION_BACKEND_FAILED,
  };
}

static void discard_vulkan_submission(
    Lardon3DMatcherVulkanSubmission *submission) {
  if (submission->state == VULKAN_SUBMISSION_SUBMITTED
      && submission->pending) {
    lardon3d_matcher_discard_vulkan_stage(submission->pending);
  }
  submission->pending = NULL;
  submission->state = VULKAN_SUBMISSION_EMPTY;
}

static void discard_vulkan_submissions(
    Lardon3DMatcherVulkanSubmission *submissions, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    discard_vulkan_submission(&submissions[index]);
  }
}

static void fail_active_vulkan_submissions(
    Lardon3DMatcherVulkanSubmission *submissions, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    Lardon3DMatcherVulkanSubmission *submission = &submissions[index];
    if (submission->state != VULKAN_SUBMISSION_SUBMITTED) {
      /* Pair-local INVALID_ARGUMENT evidence is immutable with respect to a
       * neighboring request failure. Published/empty entries likewise remain
       * consumed. Only exact live requests lose backend health here. */
      continue;
    }
    if (submission->pending) {
      lardon3d_matcher_discard_vulkan_stage(submission->pending);
    }
    submission->pending = NULL;
    submission->state = VULKAN_SUBMISSION_BACKEND_FAILED;
  }
}

static void submit_vulkan_until_limit(
    Lardon3DMatcherTaskContext *context,
    const Lardon3DMatcherParams *matcher,
    Lardon3DMatcherPairStage *stages,
    size_t count,
    size_t inflight_limit,
    size_t *next_to_submit,
    size_t *pending_count,
    Lardon3DMatcherVulkanSubmission *submissions,
    bool *backend_failed,
    bool *vulkan_ineligible,
    bool *vulkan_other_failure) {
  while (!*backend_failed && *next_to_submit < count
         && *pending_count < inflight_limit) {
    size_t index = *next_to_submit;
    submissions[index] = begin_vulkan_submission(
        context, matcher, &stages[index], backend_failed,
        vulkan_ineligible, vulkan_other_failure);
    ++*next_to_submit;
    if (submissions[index].state == VULKAN_SUBMISSION_SUBMITTED) {
      ++*pending_count;
#ifdef LARDON3D_MATCHER_TASK_TESTING
      test_record_event(TEST_EVENT_GPU_SUBMIT,
                        stages[index].pair.candidate_pair_id);
      Lardon3DOrbVulkanTelemetry capacity_telemetry = {0};
      if (lardon3d_orb_vulkan_internal_telemetry(
              context->orb_vulkan_backend, &capacity_telemetry)) {
        uint_fast64_t observed =
            atomic_load(&test_max_retained_vulkan_payload);
        while (observed < capacity_telemetry.retained_payload_bytes
               && !atomic_compare_exchange_weak(
                   &test_max_retained_vulkan_payload, &observed,
                   capacity_telemetry.retained_payload_bytes)) {
        }
      }
#endif
    } else if (submissions[index].state
               == VULKAN_SUBMISSION_BACKEND_FAILED) {
      /* An actual backend failure invalidates every request slot in the shared
       * session. Consume the Task-private handles now and classify every later
       * pair for whole-pair CPU fallback; no stale finish may be redirected. */
      fail_active_vulkan_submissions(submissions, *next_to_submit);
      *pending_count = 0;
    }
  }
}

/* Queue worker count is one, so this owner alone advances the durable cursor.
 * Keep at most the immutable admitted depth (one or two) submitted while the
 * owner publishes the completed prefix. Publication (hash/fsync/SQLite) can
 * overlap private Vulkan work without allowing completion order to affect
 * ascending candidate identity. A pair without a submitted request always
 * executes wholly on CPU; finish() is called only for its exact request. */
static bool compute_publish_vulkan_window(
    Lardon3DTask *task, Lardon3DMatcherTaskContext *context,
    const Lardon3DMatcherParams *matcher, Lardon3DMatcherPairStage *stages,
    size_t count, size_t *published, bool *used_cpu, bool *used_vulkan,
    bool *backend_failed, bool *vulkan_ineligible,
    bool *vulkan_other_failure, size_t inflight_limit,
    Lardon3DResourceExecutionMetrics *metrics) {
  *published = 0;
  if (count == 0) {
    return true;
  }
  if (inflight_limit == 0
      || inflight_limit > LARDON3D_ORB_VULKAN_MAX_INFLIGHT) return false;
  Lardon3DMatcherVulkanSubmission submissions[MATCHER_TASK_WINDOW_MAX] = {0};
  size_t next_to_submit = 0;
  size_t pending_count = 0;
  submit_vulkan_until_limit(
      context, matcher, stages, count, inflight_limit, &next_to_submit,
      &pending_count, submissions, backend_failed, vulkan_ineligible,
      vulkan_other_failure);
  for (size_t index = 0; index < count; ++index) {
    if (!lardon3d_task_checkpoint(task)) {
      discard_vulkan_submissions(submissions, count);
      discard_window(stages, count);
      return false;
    }
    if (index >= next_to_submit) {
      submissions[index].state = VULKAN_SUBMISSION_BACKEND_FAILED;
      next_to_submit = index + 1;
    }
    Lardon3DMatcherVulkanSubmission *current = &submissions[index];
    Lardon3DMatcherResult compute_result = LARDON3D_MATCHER_OK;
    Lardon3DMatcherFallbackCause fallback_cause =
        current->state == VULKAN_SUBMISSION_LOCAL_INELIGIBLE
            ? MATCHER_FALLBACK_LOCAL_INELIGIBLE
            : current->state == VULKAN_SUBMISSION_BACKEND_FAILED
                ? MATCHER_FALLBACK_BACKEND_FAILURE
                : current->state == VULKAN_SUBMISSION_OTHER_FAILED
                    ? MATCHER_FALLBACK_OTHER
                : current->state == VULKAN_SUBMISSION_SUBMITTED
                    ? MATCHER_FALLBACK_NONE
                    : MATCHER_FALLBACK_OTHER;
    if (current->state == VULKAN_SUBMISSION_SUBMITTED) {
      bool finish_backend_fault = false;
#ifdef LARDON3D_MATCHER_TASK_TESTING
      bool injected_finish_failure = test_fail_pair(
          "LARDON3D_TEST_MATCHER_FAIL_VULKAN_FINISH_PAIR_ID",
          stages[index].pair.candidate_pair_id);
      if (injected_finish_failure) {
        lardon3d_matcher_discard_vulkan_stage(current->pending);
        compute_result = LARDON3D_MATCHER_FAILED;
        finish_backend_fault = true;
      } else
#endif
      {
        compute_result = lardon3d_matcher_finish_vulkan_stage(
            current->pending, &stages[index].staged,
            &finish_backend_fault);
#ifdef LARDON3D_MATCHER_TASK_TESTING
        if (!finish_backend_fault && test_fail_pair(
                "LARDON3D_TEST_MATCHER_FAIL_LOCAL_VULKAN_FINISH_PAIR_ID",
                stages[index].pair.candidate_pair_id)) {
          /* Deterministically model a local staging failure after the exact
           * backend finish succeeded and consumed its request. */
          lardon3d_matcher_discard_staged(&stages[index].staged);
          compute_result = LARDON3D_MATCHER_IO_ERROR;
        }
#endif
      }
      current->pending = NULL; /* finish consumes this exact request. */
      if (pending_count > 0) --pending_count;
#ifdef LARDON3D_MATCHER_TASK_TESTING
      if (!finish_backend_fault) {
        test_record_event(TEST_EVENT_GPU_FINISH,
                          stages[index].pair.candidate_pair_id);
      }
#endif
      if (compute_result != LARDON3D_MATCHER_OK) {
        if (finish_backend_fault) {
          fallback_cause = MATCHER_FALLBACK_BACKEND_FAILURE;
          note_vulkan_backend_failure(context, backend_failed);
          fail_active_vulkan_submissions(submissions, next_to_submit);
          pending_count = 0;
        } else {
          /* Successful top2 completion followed by local filtering, memory or
           * Match File staging failure invalidates only this pair's stage.
           * Preserve shared health and unrelated submitted successors. */
          fallback_cause = MATCHER_FALLBACK_OTHER;
          current->state = VULKAN_SUBMISSION_OTHER_FAILED;
          *vulkan_other_failure = true;
        }
      }
    }
    if (current->state != VULKAN_SUBMISSION_SUBMITTED
        || compute_result != LARDON3D_MATCHER_OK) {
      /* GPU failure is an operational fallback, not a partial scientific
       * result. Local ineligibility and local begin/finish faults likewise
       * publish no partial evidence.
       * Execute this complete pair on CPU. Locally eligible successors may
       * already be submitted at depth two but remain private until their own
       * ordered turn; a backend failure has discarded them above. No
       * successor can publish ahead of this CPU fallback. */
      lardon3d_matcher_discard_staged(&stages[index].staged);
      *used_cpu = true;
      struct timespec cpu_begin;
      struct timespec cpu_end;
      (void)clock_gettime(CLOCK_MONOTONIC, &cpu_begin);
      bool cpu_fallback_failed =
          test_fail_pair("LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
                         stages[index].pair.candidate_pair_id);
      if (cpu_fallback_failed ||
          lardon3d_matcher_stage(context->project_path,
                                 &stages[index].feature_set_a,
                                 &stages[index].feature_set_b, matcher, NULL,
                                 &stages[index].staged) != LARDON3D_MATCHER_OK) {
        /* CONTRACT: a locally ineligible oldest pair consumes no GPU slot, so
         * depth two may already own a submitted successor here. Consume every
         * request-bound handle before staged-output cleanup and return; only
         * then can sequence end shrink its mapped payload under the still-live
         * Governor reservation. No successor evidence is publishable. */
        discard_vulkan_submissions(submissions, count);
        discard_window(stages, count);
        return false;
      }
      (void)clock_gettime(CLOCK_MONOTONIC, &cpu_end);
      saturating_add_ns(&metrics->matcher_cpu_ns,
                        elapsed_ns(cpu_begin, cpu_end));
    }
    stages[index].computed = LARDON3D_MATCHER_OK;
    bool pair_used_vulkan = stages[index].staged.stats.used_vulkan;
    submit_vulkan_until_limit(
        context, matcher, stages, count, inflight_limit, &next_to_submit,
        &pending_count, submissions, backend_failed, vulkan_ineligible,
        vulkan_other_failure);
#ifdef LARDON3D_MATCHER_TASK_TESTING
    bool successor_submitted = false;
    for (size_t successor = index + 1; successor < next_to_submit;
         ++successor) {
      if (submissions[successor].state == VULKAN_SUBMISSION_SUBMITTED) {
        successor_submitted = true;
        break;
      }
    }
#endif
    if (pair_used_vulkan) {
      *used_vulkan = true;
    } else {
      *used_cpu = true;
    }
#ifdef LARDON3D_MATCHER_TASK_TESTING
    /* This event is deliberately ordered after begin returned and immediately
     * before owner publication. It proves the rolling invariant without
     * timing/sleep assumptions: the successor is in the backend before this
     * candidate can enter hashing, fsync, or DB publication. */
    if (successor_submitted) {
      atomic_fetch_add(&test_overlap_publications, 1);
    }
    test_record_event(TEST_EVENT_PUBLICATION_START,
                      stages[index].pair.candidate_pair_id);
#endif
    bool publication_succeeded =
        publish_pair(task, context, matcher, &stages[index],
                     &metrics->publication_ns);
#ifdef LARDON3D_MATCHER_TASK_TESTING
    test_record_event(TEST_EVENT_PUBLICATION_FINISH,
                      stages[index].pair.candidate_pair_id);
#endif
    if (!publication_succeeded) {
      discard_vulkan_submissions(submissions, count);
      discard_window(stages, count);
      return false;
    }
    /* CONTRACT: item telemetry follows the same durable boundary as cursor
     * movement. Classify this Vulkan-selected pair exactly once only after its
     * complete CPU result is published; pending or partial GPU evidence never
     * contributes, and successful Vulkan work contributes no fallback item. */
    if (!pair_used_vulkan) {
      commit_completed_fallback_item(
          task, stages[index].pair.candidate_pair_id, metrics,
          fallback_cause == MATCHER_FALLBACK_NONE
              ? MATCHER_FALLBACK_OTHER : fallback_cause);
    }
    /* A stage is consumed by publication.  Move the cursor only after its
     * atomic Match Result is durable; later in-flight work remains private. */
    context->parameters.after_candidate_pair_id = stages[index].pair.candidate_pair_id;
    ++*published;
    if (!checkpoint_after_publication(task)) {
      discard_vulkan_submissions(submissions, count);
      discard_window(stages, count);
      return false;
    }
#ifdef LARDON3D_MATCHER_TASK_TESTING
    if (pair_used_vulkan) {
      atomic_fetch_add(&test_vulkan_uses, 1);
    }
#endif
    current->state = VULKAN_SUBMISSION_EMPTY;
  }
  return true;
}

#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
/* Historical synchronous-fence control for benchmark evidence only. Each
 * selected GPU pair completes the existing public top2 transaction before its
 * canonical stage is published. A Vulkan error produces the public primitive's
 * complete CPU fallback; no partial GPU evidence or backend choice is durable. */
static bool compute_publish_synchronous_vulkan_window(
    Lardon3DTask *task, Lardon3DMatcherTaskContext *context,
    const Lardon3DMatcherParams *matcher, Lardon3DMatcherPairStage *stages,
    size_t count, size_t *published, bool *used_cpu, bool *used_vulkan,
    bool *backend_failed, bool *vulkan_ineligible,
    Lardon3DResourceExecutionMetrics *metrics) {
  *published = 0;
  for (size_t index = 0; index < count; ++index) {
    if (!lardon3d_task_checkpoint(task)) {
      discard_window(stages, count);
      return false;
    }
    struct timespec compute_begin;
    struct timespec compute_end;
    (void)clock_gettime(CLOCK_MONOTONIC, &compute_begin);
    bool injected_local = test_fail_pair(
        "LARDON3D_TEST_MATCHER_INELIGIBLE_VULKAN_BEGIN_PAIR_ID",
        stages[index].pair.candidate_pair_id);
    bool injected_backend_failure = test_fail_pair(
        "LARDON3D_TEST_MATCHER_FAIL_VULKAN_BEGIN_PAIR_ID",
        stages[index].pair.candidate_pair_id)
        || test_fail_pair(
            "LARDON3D_TEST_MATCHER_FAIL_VULKAN_FINISH_PAIR_ID",
            stages[index].pair.candidate_pair_id);
    bool injected_compute_failure = test_fail_pair(
        "LARDON3D_TEST_MATCHER_FAIL_COMPUTE_PAIR_ID",
        stages[index].pair.candidate_pair_id);
    Lardon3DOrbVulkanBackend *pair_backend =
        injected_local || injected_backend_failure
            ? NULL : context->orb_vulkan_backend;
    stages[index].computed = injected_compute_failure
        ? LARDON3D_MATCHER_FAILED
        : lardon3d_matcher_stage(
              context->project_path, &stages[index].feature_set_a,
              &stages[index].feature_set_b, matcher, pair_backend,
              &stages[index].staged);
    (void)clock_gettime(CLOCK_MONOTONIC, &compute_end);
    if (stages[index].computed != LARDON3D_MATCHER_OK) {
      discard_window(stages, count);
      return false;
    }
    Lardon3DMatcherFallbackCause fallback_cause = MATCHER_FALLBACK_NONE;
    if (stages[index].staged.stats.used_vulkan) {
      *used_vulkan = true;
#ifdef LARDON3D_MATCHER_TASK_TESTING
      atomic_fetch_add(&test_vulkan_uses, 1);
#endif
    } else {
      *used_cpu = true;
      saturating_add_ns(&metrics->matcher_cpu_ns,
                        elapsed_ns(compute_begin, compute_end));
      if (injected_backend_failure
          || stages[index].staged.stats.vulkan_fallback) {
        fallback_cause = MATCHER_FALLBACK_BACKEND_FAILURE;
        note_vulkan_backend_failure(context, backend_failed);
      } else {
        fallback_cause = MATCHER_FALLBACK_LOCAL_INELIGIBLE;
        *vulkan_ineligible = true;
      }
    }
    if (!publish_pair(task, context, matcher, &stages[index],
                      &metrics->publication_ns)) {
      discard_window(stages, count);
      return false;
    }
    commit_completed_fallback_item(
        task, stages[index].pair.candidate_pair_id, metrics, fallback_cause);
    context->parameters.after_candidate_pair_id =
        stages[index].pair.candidate_pair_id;
    ++*published;
    if (!checkpoint_after_publication(task)) {
      discard_window(stages, count);
      return false;
    }
  }
  return true;
}
#endif

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
  struct timespec durable_cycle_begin;
  bool durable_cycle_timing_known =
      clock_gettime(CLOCK_MONOTONIC, &durable_cycle_begin) == 0;

  for (;;) {
    if (!lardon3d_task_checkpoint(task)) {
      return false;
    }
    Lardon3DTaskExecutionContract contract;
    Lardon3DResourceCapabilitySelection execution_selection;
    if (!lardon3d_task_execution_contract(task, &contract) ||
        !lardon3d_task_internal_execution_selection(
            task, &execution_selection) ||
        contract.batch_size < LARDON3D_MATCHER_TASK_MINIMUM_BATCH ||
        contract.batch_size > LARDON3D_MATCHER_TASK_MAXIMUM_BATCH ||
        execution_selection.inflight_limit == 0 ||
        execution_selection.inflight_limit >
            LARDON3D_ORB_VULKAN_MAX_INFLIGHT) {
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
      (void)lardon3d_feature_opencv_configure_threads(previous_opencv_threads);
      return fail_task(task, "Configuration OpenCV Matcher impossible.");
    }
    size_t processed_in_batch = 0;
    bool batch_ok = true;
    bool sequence_used_cpu = false;
    bool sequence_used_vulkan = false;
    bool sequence_backend_failed = false;
    bool sequence_vulkan_ineligible = false;
    bool sequence_vulkan_other_failure = false;
    Lardon3DMatcherParams matcher = {
        .kind = (Lardon3DMatcherKind)context->parameters.matcher_kind,
        .ratio_threshold = context->parameters.ratio_threshold,
    };
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
    bool sequence_vulkan_synchronous =
        context->benchmark_synchronous_pipeline;
#else
    bool sequence_vulkan_synchronous = false;
#endif
    bool sequence_vulkan_requested = context->orb_vulkan_backend != NULL
        && contract.gpu_slots == 1
        && matcher.kind == LARDON3D_MATCHER_ORB_BF;
    bool sequence_capacity_lease = false;
    bool sequence_capacity_ready = true;
    if (sequence_vulkan_requested && !sequence_vulkan_synchronous) {
      sequence_capacity_lease =
          lardon3d_orb_vulkan_internal_begin_sequence(
              context->orb_vulkan_backend,
              (uint32_t)execution_selection.inflight_limit);
      if (!sequence_capacity_lease) {
        sequence_capacity_ready = false;
        note_vulkan_backend_failure(context, &sequence_backend_failed);
      }
    }
    Lardon3DResourceExecutionMetrics execution_metrics = {0};
    Lardon3DOrbVulkanTelemetry vulkan_before = {0};
    Lardon3DOrbVulkanTelemetry vulkan_after = {0};
    bool vulkan_telemetry_known = context->orb_vulkan_backend
        && lardon3d_orb_vulkan_internal_telemetry(
            context->orb_vulkan_backend, &vulkan_before);
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
      /* Only the GPU-admitted one-owner path may use a pending Vulkan slot.
       * CPU mode remains the established parallel staging path. */
      bool vulkan_selected = context->orb_vulkan_backend != NULL &&
                             contract.gpu_slots == 1 &&
                             matcher.kind == LARDON3D_MATCHER_ORB_BF;
      bool vulkan_synchronous =
          vulkan_selected && sequence_vulkan_synchronous;
      bool vulkan_rolling = vulkan_selected && !vulkan_synchronous
          && sequence_capacity_ready;
#ifdef LARDON3D_MATCHER_TASK_TESTING
      const char *force_fallback = getenv("LARDON3D_TEST_MATCHER_FORCE_FALLBACK");
      if (force_fallback && strcmp(force_fallback, "1") == 0) {
        if (vulkan_rolling) {
          atomic_fetch_add(&test_forced_fallbacks, 1);
        }
        vulkan_rolling = false;
        if (contract.gpu_slots == 1) {
          note_vulkan_backend_failure(context, &sequence_backend_failed);
        }
      }
#endif
      if (batch_ok && vulkan_rolling) {
        size_t published = 0;
        batch_ok = compute_publish_vulkan_window(task, context, &matcher, stages,
                                                 window_count, &published,
                                                 &sequence_used_cpu,
                                                 &sequence_used_vulkan,
                                                 &sequence_backend_failed,
                                                 &sequence_vulkan_ineligible,
                                                 &sequence_vulkan_other_failure,
                                                 execution_selection.inflight_limit,
                                                 &execution_metrics);
        processed_in_batch += published;
        total_processed += published;
      }
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
      else if (batch_ok && vulkan_synchronous) {
        size_t published = 0;
        batch_ok = compute_publish_synchronous_vulkan_window(
            task, context, &matcher, stages, window_count, &published,
            &sequence_used_cpu, &sequence_used_vulkan,
            &sequence_backend_failed, &sequence_vulkan_ineligible,
            &execution_metrics);
        processed_in_batch += published;
        total_processed += published;
      }
#endif
      else {
        /* CPU stages remain parallel private computation followed by ordered
         * owner publication.  Vulkan uses the rolling path above because its
         * successor must be submitted before this prefix is made durable. */
        if (batch_ok) {
          struct timespec cpu_begin;
          struct timespec cpu_end;
          (void)clock_gettime(CLOCK_MONOTONIC, &cpu_begin);
          if (!compute_window(context, &matcher, stages, window_count,
                              contract.cpu_threads)) {
            batch_ok = false;
          }
          (void)clock_gettime(CLOCK_MONOTONIC, &cpu_end);
          saturating_add_ns(&execution_metrics.matcher_cpu_ns,
                            elapsed_ns(cpu_begin, cpu_end));
        }
        if (batch_ok) {
          sequence_used_cpu = true;
        }
        Lardon3DMatcherFallbackCause fallback_cause =
            contract.gpu_slots != 1
                ? MATCHER_FALLBACK_NONE
                : sequence_backend_failed
                    ? MATCHER_FALLBACK_BACKEND_FAILURE
                    : MATCHER_FALLBACK_OTHER;
        for (size_t index = 0; index < window_count && batch_ok; ++index) {
          if (!lardon3d_task_checkpoint(task) ||
              stages[index].computed != LARDON3D_MATCHER_OK ||
              !publish_pair(task, context, &matcher, &stages[index],
                            &execution_metrics.publication_ns)) {
            batch_ok = false;
            break;
          }
          /* Cursor movement follows only the durable, ascending publication
           * prefix. A failed stage and every later stage remain unpublished. */
          context->parameters.after_candidate_pair_id = stages[index].pair.candidate_pair_id;
          commit_completed_fallback_item(
              task, stages[index].pair.candidate_pair_id,
              &execution_metrics, fallback_cause);
          ++processed_in_batch;
          ++total_processed;
          if (!checkpoint_after_publication(task)) {
            batch_ok = false;
            break;
          }
        }
      }
      discard_window(stages, window_count);
    }
    bool sequence_capacity_released = !sequence_capacity_lease
        || lardon3d_orb_vulkan_internal_end_sequence(
            context->orb_vulkan_backend);
    if (!lardon3d_feature_opencv_configure_threads(previous_opencv_threads)) {
      return fail_task(task, "Restauration OpenCV Matcher impossible.");
    }
    if (!sequence_capacity_released) {
      return fail_task(task, "Libération capacité Vulkan Matcher impossible.");
    }
    if (!batch_ok) {
      Lardon3DTaskSnapshot snapshot;
      if (lardon3d_task_snapshot(task, &snapshot) && snapshot.state != TASK_FAILED) {
        return fail_task(task, "Calcul Matcher parallèle impossible.");
      }
      return false;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t execution_wall_ns = elapsed_ns(begin, end);
    if (vulkan_telemetry_known
        && lardon3d_orb_vulkan_internal_telemetry(
            context->orb_vulkan_backend, &vulkan_after)) {
      add_vulkan_telemetry_delta(
          &vulkan_before, &vulkan_after, &execution_metrics);
    }
    (void)lardon3d_resource_governor_record_batch(
        context->governor, LARDON3D_RESOURCE_TASK_CPU, batch_count,
        execution_wall_ns, 0);
    Lardon3DResourceBackend actual_backend = sequence_used_cpu
            && sequence_used_vulkan
        ? LARDON3D_RESOURCE_BACKEND_MIXED
        : sequence_used_vulkan
            ? LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
            : LARDON3D_RESOURCE_BACKEND_CPU;
    const char *backend_reason = actual_backend ==
            LARDON3D_RESOURCE_BACKEND_MIXED
        ? sequence_backend_failed
            ? "vulkan-and-whole-pair-cpu-fallback"
            : sequence_vulkan_other_failure
                ? "vulkan-and-local-failure-cpu-fallback"
                : "vulkan-and-ineligible-pair-cpu-fallback"
        : actual_backend == LARDON3D_RESOURCE_BACKEND_CPU
                && contract.gpu_slots == 1
            ? sequence_backend_failed
                ? "vulkan-failed-whole-pair-cpu-fallback"
                : sequence_vulkan_other_failure
                    ? "vulkan-local-failure-whole-pair-cpu-fallback"
                    : sequence_vulkan_ineligible
                        ? "vulkan-ineligible-whole-pair-cpu-fallback"
                        : "gpu-selected-cpu-completed"
            : actual_backend == LARDON3D_RESOURCE_BACKEND_ORB_VULKAN
                ? "vulkan-completed"
                : "cpu-completed";
    bool exhausted = count <= contract.batch_size;
    unsigned int progress = exhausted ? 100U : 99U;
    if (!checkpoint_batch(task, context, progress, total_processed)) {
      return fail_task(task, "Checkpoint Matcher impossible.");
    }
    struct timespec durable_cycle_end;
    uint64_t durable_cycle_wall_ns = execution_wall_ns;
    if (durable_cycle_timing_known
        && clock_gettime(CLOCK_MONOTONIC, &durable_cycle_end) == 0) {
      uint64_t measured = elapsed_ns(durable_cycle_begin, durable_cycle_end);
      if (measured != 0) durable_cycle_wall_ns = measured;
    }
    /* CONTRACT: GPU batch adaptation optimizes the durable Task cadence, not
     * only shader/CPU execution. From the second sequence onward this interval
     * begins immediately before sequence_break, so it includes the successful
     * Governor observation/admission plus computation, owner publication and
     * the durable generic Matcher checkpoint. Failed checkpoints never train
     * the next immutable contract. Scientific output and cursor identity stay
     * independent of this operational clock. */
    (void)lardon3d_task_internal_record_sequence_execution_metrics(
        task, durable_cycle_wall_ns, processed_in_batch, actual_backend,
        backend_reason, &execution_metrics);
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
    if (context->benchmark_inflight_override != 0 && sequence_backend_failed) {
      /* Whole-pair CPU fallback remains canonical and any publication above is
       * already durable/checkpointed. It cannot, however, turn a failed Vulkan
       * A/B cohort into a successful CPU measurement. Fail only the private
       * benchmark Task; normal AUTO retains its established fallback result. */
      return fail_task(task, "Échec backend Vulkan pendant le benchmark A/B.");
    }
#endif
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
    durable_cycle_timing_known =
        clock_gettime(CLOCK_MONOTONIC, &durable_cycle_begin) == 0;
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
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  const char *benchmark_pipeline =
      getenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV);
  const char *benchmark_inflight =
      getenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV);
  const char *benchmark_batch =
      getenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV);
  bool valid_batch = !benchmark_batch || strcmp(benchmark_batch, "2") == 0 ||
                     strcmp(benchmark_batch, "4") == 0 ||
                     strcmp(benchmark_batch, "8") == 0 ||
                     strcmp(benchmark_batch, "12") == 0;
  if ((benchmark_pipeline && strcmp(benchmark_pipeline, "1") != 0) ||
      (benchmark_inflight && strcmp(benchmark_inflight, "1") != 0 &&
       strcmp(benchmark_inflight, "2") != 0) ||
      !valid_batch || (benchmark_batch && !benchmark_inflight) ||
      (benchmark_pipeline && benchmark_batch) ||
      (benchmark_pipeline && benchmark_inflight &&
       strcmp(benchmark_inflight, "2") == 0)) {
    /* Invalid inherited benchmark controls fail before any Task callback can
     * run. Production builds do not contain these strings or this branch. */
    free(context);
    return NULL;
  }
  context->benchmark_synchronous_pipeline = benchmark_pipeline != NULL;
  context->benchmark_inflight_override = !benchmark_inflight
      ? 0 : strcmp(benchmark_inflight, "1") == 0 ? 1 : 2;
  context->benchmark_batch_override = !benchmark_batch
      ? 0 : strcmp(benchmark_batch, "2") == 0 ? 2
          : strcmp(benchmark_batch, "4") == 0 ? 4
          : strcmp(benchmark_batch, "8") == 0 ? 8 : 12;
#endif
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
  Lardon3DResourceEstimate automatic = cpu;
  automatic.task_class = LARDON3D_RESOURCE_TASK_MIXED;
  const Lardon3DResourceEstimate vulkan =
      matcher_estimate(LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN);
  const Lardon3DResourceEstimate legacy_cpu = legacy_matcher_estimate(false);
  const Lardon3DResourceEstimate legacy_vulkan = legacy_matcher_estimate(true);
  const Lardon3DResourceEstimate previous_cpu = previous_matcher_estimate(false);
  const Lardon3DResourceEstimate previous_vulkan = previous_matcher_estimate(true);
  bool current_cpu = estimate_equals(&snapshot->estimate, &cpu);
  bool current_auto = estimate_equals(&snapshot->estimate, &automatic);
  bool current_vulkan = estimate_equals(&snapshot->estimate, &vulkan);
  bool historical_cpu = estimate_equals(&snapshot->estimate, &legacy_cpu);
  bool historical_vulkan = estimate_equals(&snapshot->estimate, &legacy_vulkan);
  bool previous_cpu_mode = estimate_equals(&snapshot->estimate, &previous_cpu);
  bool previous_vulkan_mode = estimate_equals(&snapshot->estimate, &previous_vulkan);
  bool vulkan_mode = current_vulkan || historical_vulkan || previous_vulkan_mode;
  if ((!current_auto && !current_cpu && !current_vulkan && !historical_cpu &&
       !historical_vulkan &&
       !previous_cpu_mode && !previous_vulkan_mode) ||
      ((vulkan_mode || current_auto) &&
       configuration.matcher.kind != LARDON3D_MATCHER_ORB_BF)) {
    return false;
  }
  Lardon3DMatcherTaskContext *context = make_context(runtime, &parameters);
  if (!context) {
    return false;
  }
  bool auto_cpu_mode = current_auto;
  bool backend_available = false;
  if (auto_cpu_mode) {
    Lardon3DTaskCapabilityEnvelope runtime_probe_envelope =
        matcher_auto_envelope(&cpu, true, true, 0, 0);
    bool hardware_safe =
        lardon3d_resource_governor_internal_capability_hardware_safe(
            runtime->resource_governor,
            &runtime_probe_envelope.capabilities[0]);
    /* Only normal AUTO recovery owns runtime eligibility reconstruction.
     * Fixed CPU/Vulkan and historical overrides must be order-independent and
     * cannot clear shared AUTO state. This metadata check performs no Vulkan
     * call; first initialization still belongs to Queue's constrained worker. */
    backend_available = LARDON3D_HAVE_VULKAN && hardware_safe &&
                        auto_vulkan_backend_candidate(
                            runtime->orb_vulkan_backend);
    (void)lardon3d_resource_governor_internal_set_backend_available(
        runtime->resource_governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN,
        backend_available);
  }
  context->normal_auto = auto_cpu_mode;
  context->auto_vulkan_available = backend_available;
  context->explicit_vulkan = vulkan_mode;
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  if (context->benchmark_inflight_override != 0 &&
      (!auto_cpu_mode || !backend_available)) {
    free(context);
    return false;
  }
#endif
  /* Exact whole-estimate signatures reject neighboring malformed snapshots.
   * New MIXED ORB is normal AUTO. Every CPU-class ORB signature is restored as
   * fixed CPU for explicit/recovery safety; Vulkan and non-ORB forms stay fixed. */
  if (!vulkan_mode && !auto_cpu_mode) {
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

bool lardon3d_matcher_task_internal_configure_restored(
    Lardon3DTask *task, void *userdata) {
  Lardon3DMatcherTaskContext *context = userdata;
  if (!task || !context) {
    return task && context;
  }
  Lardon3DResourceEstimate cpu;
  if (!lardon3d_task_resource_estimate(task, &cpu)) {
    return false;
  }
  bool allow_depth_two = true;
  size_t benchmark_inflight_override = 0;
  size_t benchmark_batch_override = 0;
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  allow_depth_two = !context->benchmark_synchronous_pipeline;
  benchmark_inflight_override = context->benchmark_inflight_override;
  benchmark_batch_override = context->benchmark_batch_override;
#endif
  Lardon3DTaskCapabilityEnvelope envelope = context->normal_auto
      ? matcher_auto_envelope(&cpu, context->auto_vulkan_available,
                              allow_depth_two, benchmark_inflight_override,
                              benchmark_batch_override)
      : matcher_fixed_envelope(&cpu, context->explicit_vulkan);
  return lardon3d_task_internal_set_capability_envelope(task, &envelope);
}

static Lardon3DTask *create_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration,
    Lardon3DMatcherTaskMode mode, bool automatic, uint64_t *task_id) {
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
  bool orb_auto = automatic &&
                  configuration->matcher.kind == LARDON3D_MATCHER_ORB_BF;
  bool runtime_vulkan = orb_auto && auto_vulkan_runtime_candidate(state);
  bool auto_vulkan = runtime_vulkan;
  if (orb_auto) {
    /* Availability is Governor-owned runtime state. Portable builds and
     * unsafe/unavailable hardware publish false without touching a GPU. */
    (void)lardon3d_resource_governor_internal_set_backend_available(
        state->resource_governor, LARDON3D_RESOURCE_BACKEND_ORB_VULKAN,
        runtime_vulkan);
  }
  Lardon3DMatcherTaskContext *context = make_context(&runtime, &parameters);
  if (!context) {
    return NULL;
  }
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
  if (context->benchmark_inflight_override != 0 &&
      (!orb_auto || !auto_vulkan)) {
    free(context);
    return NULL;
  }
#endif
  /* Execution mode is fixed before admission. The Governor may reduce a
   * parallel task to one CPU thread, but that CPU-only task still must not use
   * Vulkan without the GPU resources declared by its immutable estimate. */
  if (!vulkan_mode && !auto_vulkan) {
    context->orb_vulkan_backend = NULL;
  }
  /* Each staged pair can retain the full bounded Matcher working set until
   * ordered publication. The selected immutable estimate covers the entire
   * window and never limits scientific dataset cardinality. */
  Lardon3DResourceEstimate estimate = matcher_estimate(mode);
  if (orb_auto) {
    /* Normal AUTO may execute a complete CPU or Vulkan sequence. MIXED is an
     * honest durable resource class, not a backend tag or scientific identity. */
    estimate.task_class = LARDON3D_RESOURCE_TASK_MIXED;
  }
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
  Lardon3DTaskCapabilityEnvelope automatic_envelope;
  bool envelope_ready = true;
  if (task) {
    bool allow_depth_two = true;
    size_t benchmark_inflight_override = 0;
    size_t benchmark_batch_override = 0;
#ifdef LARDON3D_MATCHER_TASK_BENCHMARK_PIPELINE
    allow_depth_two = !context->benchmark_synchronous_pipeline;
    benchmark_inflight_override = context->benchmark_inflight_override;
    benchmark_batch_override = context->benchmark_batch_override;
#endif
    automatic_envelope = orb_auto
        ? matcher_auto_envelope(&estimate, auto_vulkan, allow_depth_two,
                                benchmark_inflight_override,
                                benchmark_batch_override)
        : matcher_fixed_envelope(&estimate, vulkan_mode);
    envelope_ready = lardon3d_task_internal_set_capability_envelope(
        task, &automatic_envelope);
  }
  if (!task || !envelope_ready || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished_callback, context) ||
      lardon3d_project_checkpoint_matcher_task(state, task, &parameters) !=
          LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

Lardon3DTask *lardon3d_project_create_matcher_task_with_mode(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration,
    Lardon3DMatcherTaskMode mode, uint64_t *task_id) {
  return create_matcher_task(state, configuration, mode, false, task_id);
}

Lardon3DTask *lardon3d_project_create_matcher_task(
    Lardon3DAppState *state,
    const Lardon3DMatcherTaskConfiguration *configuration, uint64_t *task_id) {
  return create_matcher_task(
      state, configuration, LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL, true,
      task_id);
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
  if (!state || !state->task_queue) {
    return false;
  }
  Lardon3DTask *task = lardon3d_project_create_matcher_task(
      state, configuration, task_id);
  if (!task) {
    return false;
  }
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
