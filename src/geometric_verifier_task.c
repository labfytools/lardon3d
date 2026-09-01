#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/geometric_verifier_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

#include "geometric_verifier_internal.h"
#include "task_internal.h"

enum {
  GEOMETRIC_VERIFIER_PAGE_CAPACITY =
      LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH + 1,
  /* Each participant owns at most the established conservative 4 MiB core
   * allowance plus one explicitly bounded 4 MiB child stack. Charging 8 MiB
   * per batch item safely covers the owner too and remains an operational
   * admission bound, never a Match-Result dataset limit. */
  GEOMETRIC_VERIFIER_MEMORY_BYTES_PER_ITEM = 8 * 1024 * 1024,
  GEOMETRIC_VERIFIER_CHILD_STACK_BYTES = 4 * 1024 * 1024,
};

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DProjectDbGeometricVerifierTask durable;
  uint32_t verifier_version;
} Lardon3DGeometricVerifierTaskContext;

typedef struct {
  Lardon3DGeometricVerifierResult status;
  Lardon3DGeometricVerifierPrepared *prepared;
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused;
  bool applicable;
} Lardon3DGeometricVerifierJob;

typedef struct {
  Lardon3DGeometricVerifierTaskContext *context;
  const Lardon3DProjectDbMatchResult *parents;
  Lardon3DGeometricVerifierJob *jobs;
  Lardon3DGeometricVerifierParameters parameters;
  size_t count;
  atomic_size_t next;
  atomic_bool stop;
} Lardon3DGeometricVerifierBatch;

#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
static atomic_uint observed_cpu_contracts;
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  bool armed;
  bool reached;
  bool released;
} Lardon3DGeometricVerifierTestBarrier;

#define GEOMETRIC_VERIFIER_TEST_BARRIER_INITIALIZER                         \
  { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false }

static Lardon3DGeometricVerifierTestBarrier sequence_barrier =
    GEOMETRIC_VERIFIER_TEST_BARRIER_INITIALIZER;
static Lardon3DGeometricVerifierTestBarrier prepublication_barrier =
    GEOMETRIC_VERIFIER_TEST_BARRIER_INITIALIZER;

void lardon3d_geometric_verifier_task_test_reset_cpu_contracts(void) {
  atomic_store_explicit(&observed_cpu_contracts, 0, memory_order_relaxed);
}

unsigned int lardon3d_geometric_verifier_task_test_cpu_contracts(void) {
  return atomic_load_explicit(&observed_cpu_contracts, memory_order_relaxed);
}

static void test_barrier_arm(Lardon3DGeometricVerifierTestBarrier *barrier) {
  (void)pthread_mutex_lock(&barrier->mutex);
  barrier->armed = true;
  barrier->reached = false;
  barrier->released = false;
  (void)pthread_mutex_unlock(&barrier->mutex);
}

static bool test_barrier_wait(Lardon3DGeometricVerifierTestBarrier *barrier) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return false;
  }
  deadline.tv_sec += 5;
  (void)pthread_mutex_lock(&barrier->mutex);
  bool ok = true;
  while (!barrier->reached && ok) {
    ok = pthread_cond_timedwait(&barrier->condition, &barrier->mutex,
                                &deadline) == 0;
  }
  (void)pthread_mutex_unlock(&barrier->mutex);
  return ok;
}

static void test_barrier_release(Lardon3DGeometricVerifierTestBarrier *barrier) {
  (void)pthread_mutex_lock(&barrier->mutex);
  barrier->released = true;
  (void)pthread_cond_broadcast(&barrier->condition);
  (void)pthread_mutex_unlock(&barrier->mutex);
}

static void test_barrier_reach(Lardon3DGeometricVerifierTestBarrier *barrier) {
  (void)pthread_mutex_lock(&barrier->mutex);
  if (barrier->armed) {
    barrier->reached = true;
    (void)pthread_cond_broadcast(&barrier->condition);
    while (!barrier->released) {
      (void)pthread_cond_wait(&barrier->condition, &barrier->mutex);
    }
    barrier->armed = false;
  }
  (void)pthread_mutex_unlock(&barrier->mutex);
}

void lardon3d_geometric_verifier_task_test_arm_sequence_barrier(void) {
  test_barrier_arm(&sequence_barrier);
}

bool lardon3d_geometric_verifier_task_test_wait_sequence_barrier(void) {
  return test_barrier_wait(&sequence_barrier);
}

void lardon3d_geometric_verifier_task_test_release_sequence_barrier(void) {
  test_barrier_release(&sequence_barrier);
}

void lardon3d_geometric_verifier_task_test_arm_prepublication_barrier(void) {
  test_barrier_arm(&prepublication_barrier);
}

bool lardon3d_geometric_verifier_task_test_wait_prepublication_barrier(void) {
  return test_barrier_wait(&prepublication_barrier);
}

void lardon3d_geometric_verifier_task_test_release_prepublication_barrier(void) {
  test_barrier_release(&prepublication_barrier);
}
#endif

static void destroy_context(void *userdata) { free(userdata); }

static Lardon3DGeometricVerifierParameters
parameters_from_durable(const Lardon3DProjectDbGeometricVerifierTask *durable) {
  return (Lardon3DGeometricVerifierParameters){
      .threshold_pixels = durable->threshold_pixels,
      .confidence = durable->confidence,
      .max_iterations = durable->max_iterations,
      .min_inlier_count = durable->min_inlier_count,
      .min_inlier_ratio = durable->min_inlier_ratio,
      .seed_policy_version = durable->seed_policy_version,
      .canonicalization_version = durable->canonicalization_version,
  };
}

static void runtime_state(const Lardon3DGeometricVerifierTaskContext *context,
                          Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  (void)snprintf(state->project_path, sizeof(state->project_path), "%s",
                 context->project_path);
}

static void finished_callback(const Lardon3DTask *task, void *userdata) {
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
  const char *skip = getenv("LARDON3D_TEST_GEOMETRIC_SKIP_FINISHED_CHECKPOINT");
  if (skip && strcmp(skip, "1") == 0) {
    return;
  }
#endif
  Lardon3DGeometricVerifierTaskContext *context = userdata;
  Lardon3DAppState state;
  runtime_state(context, &state);
  (void)lardon3d_project_checkpoint_geometric_verifier_task(&state, task,
                                                            &context->durable);
}

static uint64_t elapsed_ns(struct timespec begin, struct timespec end) {
  uint64_t seconds =
      end.tv_sec >= begin.tv_sec ? (uint64_t)(end.tv_sec - begin.tv_sec) : 0;
  long nanoseconds = end.tv_nsec - begin.tv_nsec;
  if (nanoseconds < 0 && seconds > 0) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  return seconds * 1000000000ULL + (uint64_t)nanoseconds;
}

static void *prepare_worker(void *userdata) {
  Lardon3DGeometricVerifierBatch *batch = userdata;
  for (;;) {
    if (atomic_load_explicit(&batch->stop, memory_order_relaxed)) {
      return NULL;
    }
    size_t index =
        atomic_fetch_add_explicit(&batch->next, 1, memory_order_relaxed);
    if (index >= batch->count) {
      return NULL;
    }
    const Lardon3DProjectDbMatchResult *parent = &batch->parents[index];
    Lardon3DGeometricVerifierJob *job = &batch->jobs[index];
    job->status = LARDON3D_GEOMETRIC_VERIFIER_OK;
    if (parent->result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED ||
        parent->match_count == 0) {
      continue;
    }
    job->applicable = true;
    job->status = lardon3d_geometric_verifier_internal_prepare_version(
        batch->context->project_path, batch->context->database,
        parent->match_result_id, &batch->parameters,
        batch->context->verifier_version, &job->prepared, &job->result,
        &job->reused);
  }
}

#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
static bool inject_thread_create_failure(size_t child_index) {
  const char *value = getenv("LARDON3D_TEST_GEOMETRIC_THREAD_FAIL_AFTER");
  if (!value || !value[0]) {
    return false;
  }
  char *end = NULL;
  unsigned long parsed = strtoul(value, &end, 10);
  return end && *end == '\0' && parsed == child_index;
}
#endif

static void destroy_jobs(Lardon3DGeometricVerifierJob *jobs, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    lardon3d_geometric_verifier_internal_prepared_destroy(
        jobs[index].prepared);
    jobs[index].prepared = NULL;
  }
}

static bool prepare_batch(Lardon3DGeometricVerifierTaskContext *context,
                          const Lardon3DProjectDbMatchResult *parents,
                          size_t count, unsigned int cpu_threads,
                          Lardon3DGeometricVerifierJob *jobs) {
  memset(jobs, 0, sizeof(*jobs) * count);
  Lardon3DGeometricVerifierBatch batch = {
      .context = context,
      .parents = parents,
      .jobs = jobs,
      .parameters = parameters_from_durable(&context->durable),
      .count = count,
  };
  atomic_init(&batch.next, 0);
  atomic_init(&batch.stop, false);
  size_t participants = cpu_threads < count ? cpu_threads : count;
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
  const char *forced = getenv("LARDON3D_TEST_GEOMETRIC_FORCE_PARTICIPANTS");
  if (forced && forced[0]) {
    char *end = NULL;
    unsigned long parsed = strtoul(forced, &end, 10);
    /* This seam exists only to reach partial pthread_create cleanup while the
     * production Governor is deliberately in CPU slow-start. It is never an
     * equivalence or performance input and cannot exceed the production-safe
     * participant/window bounds. */
    if (end && *end == '\0' && parsed >= 1 &&
        parsed <=
            LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_SAFE_CPU_THREADS) {
      participants = (size_t)parsed;
    }
  }
#endif
  size_t child_count = participants > 0 ? participants - 1 : 0;
  pthread_t children
      [LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_SAFE_CPU_THREADS - 1];
  pthread_attr_t attributes;
  bool attributes_initialized = false;
  bool attributes_ready = child_count == 0;
  if (child_count > 0 && pthread_attr_init(&attributes) == 0) {
    attributes_initialized = true;
    attributes_ready = pthread_attr_setstacksize(
                           &attributes,
                           GEOMETRIC_VERIFIER_CHILD_STACK_BYTES) == 0;
  }
  size_t created = 0;
  bool creation_ok = attributes_ready;
  while (creation_ok && created < child_count) {
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
    if (inject_thread_create_failure(created)) {
      /* Deterministically let already-created participants and the owner claim
       * the fresh batch before injecting failure. This exercises destruction of
       * real opaque stages after a partial pthread_create sequence; it never
       * changes production scheduling or publication. */
      (void)prepare_worker(&batch);
      creation_ok = false;
      break;
    }
#endif
    if (pthread_create(&children[created], &attributes, prepare_worker,
                       &batch) != 0) {
      creation_ok = false;
      break;
    }
    ++created;
  }
  if (creation_ok) {
    (void)prepare_worker(&batch);
  } else {
    /* A partial-create failure cannot hand stack storage back while a child
     * still uses it. Stop new claims, join every created child, then let the
     * owner destroy any completed opaque stages. */
    atomic_store_explicit(&batch.stop, true, memory_order_relaxed);
  }
  bool joined = true;
  for (size_t index = 0; index < created; ++index) {
    joined = pthread_join(children[index], NULL) == 0 && joined;
  }
  if (attributes_initialized) {
    (void)pthread_attr_destroy(&attributes);
  }
  return creation_ok && joined;
}

static bool preflight_batch(Lardon3DTask *task,
                            Lardon3DGeometricVerifierJob *jobs, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    const Lardon3DGeometricVerifierJob *job = &jobs[index];
    bool ownership_valid =
        !job->applicable
            ? !job->prepared && !job->reused
            : (job->reused ? !job->prepared : job->prepared != NULL);
    if (job->status != LARDON3D_GEOMETRIC_VERIFIER_OK || !ownership_valid) {
      /* CONTRACT: preparation is the all-or-nothing half of a GV batch. The
       * callback owner must discover every participant failure before it may
       * publish index zero or advance the contiguous business cursor. */
      destroy_jobs(jobs, count);
      (void)lardon3d_task_fail(task, "Vérification géométrique impossible.");
      return false;
    }
  }
  return true;
}

static bool publish_batch(Lardon3DTask *task,
                          Lardon3DGeometricVerifierTaskContext *context,
                          const Lardon3DProjectDbMatchResult *parents,
                          Lardon3DGeometricVerifierJob *jobs, size_t count,
                          uint64_t *processed, size_t *durable_items) {
  /* INVARIANT: preflight has proved every joined slot. From here through the
   * typed cursor + generic checkpoint, this dispatched batch is one engaged
   * unit. Pause/cancel is deliberately not observed between ordered owner-only
   * publications; the next control boundary follows the durable checkpoint. */
  for (size_t index = 0; index < count; ++index) {
    Lardon3DGeometricVerifierJob *job = &jobs[index];
    if (job->applicable && job->prepared) {
      job->status = lardon3d_geometric_verifier_internal_publish_prepared(
          context->database, job->prepared, &job->result, &job->reused);
      lardon3d_geometric_verifier_internal_prepared_destroy(job->prepared);
      job->prepared = NULL;
      if (job->status != LARDON3D_GEOMETRIC_VERIFIER_OK) {
        destroy_jobs(jobs, count);
        (void)lardon3d_task_fail(task,
                                "Publication géométrique impossible.");
        return false;
      }
      if (!job->reused) {
        ++*durable_items;
      }
    }
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
    const char *pause =
        getenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION");
    if (job->applicable && pause && strcmp(pause, "1") == 0) {
      /* This testing-only crash seam intentionally interrupts the normal
       * engaged-unit rule to preserve proof of the frozen residual restart
       * boundary: publication may be durable while the business cursor still
       * names the prior parent. Production never observes controls here. */
      (void)lardon3d_task_pause(task);
      destroy_jobs(jobs, count);
      (void)lardon3d_task_checkpoint(task);
      return false;
    }
#endif
    // Publication/reuse must be durable before the business cursor advances.
    // Restart may repeat an already published parent, but exact identity
    // reuse makes that retry converge without duplicating or guessing GVRs.
    context->durable.after_match_result_id = parents[index].match_result_id;
    ++*processed;
  }
  destroy_jobs(jobs, count);
  return true;
}

static bool checkpoint_batch(Lardon3DTask *task,
                             Lardon3DGeometricVerifierTaskContext *context,
                             unsigned int progress, uint64_t processed) {
  char message[LARDON3D_TASK_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "Match Results traités:%lu",
                 (unsigned long)processed);
  if (!lardon3d_task_set_progress(task, progress, message)) {
    return false;
  }
  Lardon3DAppState state;
  runtime_state(context, &state);
  return lardon3d_project_checkpoint_geometric_verifier_task(
             &state, task, &context->durable) ==
         LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}

static bool run(Lardon3DTask *task, void *userdata) {
  Lardon3DGeometricVerifierTaskContext *context = userdata;
  uint64_t processed = 0;
  for (;;) {
    if (!lardon3d_task_checkpoint(task)) {
      return false;
    }
    Lardon3DTaskExecutionContract contract;
    if (!lardon3d_task_execution_contract(task, &contract) ||
        contract.cpu_threads == 0 ||
        contract.cpu_threads >
            LARDON3D_GEOMETRIC_VERIFIER_TASK_VALIDATED_USEFUL_CPU_THREADS ||
        contract.batch_size < LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH ||
        contract.batch_size > LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH) {
      return lardon3d_task_fail(
          task, "Contrat CPU/lot Geometric Verifier invalide.");
    }
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
    (void)atomic_fetch_or_explicit(&observed_cpu_contracts,
                                   1U << contract.cpu_threads,
                                   memory_order_relaxed);
#endif
    Lardon3DProjectDbMatchResult page[GEOMETRIC_VERIFIER_PAGE_CAPACITY];
    size_t count = 0;
    size_t capacity = contract.batch_size + 1;
    if (lardon3d_project_db_list_match_results(
            context->database, context->durable.after_match_result_id, page,
            capacity, &count) != LARDON3D_PROJECT_DB_OK) {
      return lardon3d_task_fail(task, "Pagination Match Result impossible.");
    }
    if (count == 0) {
      return lardon3d_task_set_progress(task, 100,
                                        "Vérification géométrique terminée.");
    }
    size_t batch_count =
        count < contract.batch_size ? count : contract.batch_size;
    struct timespec begin;
    struct timespec end;
    (void)clock_gettime(CLOCK_MONOTONIC, &begin);
    Lardon3DGeometricVerifierJob jobs
        [LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH];
    size_t durable_items = 0;
    /* Queue owns this one callback and Governor owns the admitted width.
     * Children only prepare independent immutable Match Results; the callback
     * owner joins them all, publishes in parent order, and advances one
     * contiguous durable cursor before any sequence_break releases admission. */
    if (!prepare_batch(context, page, batch_count, contract.cpu_threads,
                       jobs)) {
      destroy_jobs(jobs, batch_count);
      return lardon3d_task_fail(task,
                                "Création des participants GV impossible.");
    }
    if (!preflight_batch(task, jobs, batch_count)) {
      return false;
    }
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
    /* This barrier is after the all-slot preflight and before index zero, so
     * tests can issue control requests without timing luck or weakening the
     * production engaged-batch boundary. */
    test_barrier_reach(&prepublication_barrier);
#endif
    if (!publish_batch(task, context, page, jobs, batch_count, &processed,
                       &durable_items)) {
      return false;
    }
    bool exhausted = count <= contract.batch_size;
    if (!checkpoint_batch(task, context, exhausted ? 100U : 99U, processed)) {
      return lardon3d_task_fail(task,
                                "Checkpoint Geometric Verifier impossible.");
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t duration_ns = elapsed_ns(begin, end);
    /* Feedback covers scientific preparation, owner-only publication, cursor
     * advancement, and the durable batch checkpoint—not CPU utilization. */
    (void)lardon3d_resource_governor_record_batch(
        context->governor, LARDON3D_RESOURCE_TASK_CPU, durable_items,
        duration_ns, 0);
    (void)lardon3d_task_internal_record_sequence(task, duration_ns,
                                                 durable_items);
    if (exhausted) {
      /* The final dispatched batch needs the same post-checkpoint control
       * boundary as a non-final sequence. Otherwise a pause arriving during
       * preparation/publication could be silently converted to COMPLETED; a
       * cancel must likewise win only after the engaged prefix is durable. */
      if (!lardon3d_task_checkpoint(task)) {
        return false;
      }
      return lardon3d_task_set_progress(task, 100,
                                        "Vérification géométrique terminée.");
    }
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
    /* Test acknowledgement occurs only after the cursor checkpoint and before
     * sequence_break releases admission. Policy changes made at the barrier
     * therefore affect the real next Governor decision. */
    test_barrier_reach(&sequence_barrier);
#endif
    Lardon3DResourceReservation *reservation = NULL;
    if (!lardon3d_task_sequence_break(task, context->governor, &reservation,
                                      &contract)) {
      return false;
    }
  }
}

static Lardon3DGeometricVerifierTaskContext *
make_context(const Lardon3DTaskReconstructionContext *runtime,
             const Lardon3DProjectDbGeometricVerifierTask *durable) {
  if (!runtime || !runtime->project_path || !runtime->project_db ||
      !runtime->resource_governor || !durable) {
    return NULL;
  }
  Lardon3DGeometricVerifierParameters parameters =
      parameters_from_durable(durable);
  unsigned char fingerprint[32];
  uint32_t verifier_version = 0;
  if (!lardon3d_geometric_verifier_parameters_valid(&parameters)) {
    return NULL;
  }
  // Project DB intentionally has no duplicate verifier-version column in this
  // task payload. The exact versioned fingerprint unambiguously recovers
  // historical v1/v2 or current v3 while keeping all identities immutable.
  for (uint32_t candidate = LARDON3D_GEOMETRIC_VERIFIER_VERSION_V1;
       candidate <= LARDON3D_GEOMETRIC_VERIFIER_VERSION_V3; ++candidate) {
    if (lardon3d_geometric_verifier_fingerprint_for_version(
            &parameters, candidate, fingerprint) &&
        memcmp(fingerprint, durable->parameter_fingerprint,
               sizeof(fingerprint)) == 0) {
      verifier_version = candidate;
      break;
    }
  }
  if (verifier_version == 0) {
    return NULL;
  }
  Lardon3DGeometricVerifierTaskContext *context = calloc(1, sizeof(*context));
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
  context->durable = *durable;
  context->verifier_version = verifier_version;
  return context;
}

bool lardon3d_geometric_verifier_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  Lardon3DTaskReconstructionContext *runtime = userdata;
  if (!snapshot || !runtime || !binding) {
    return false;
  }
  Lardon3DProjectDbGeometricVerifierTask durable;
  if (lardon3d_project_db_load_geometric_verifier_task(
          runtime->project_db, snapshot->id, &durable) !=
      LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  Lardon3DGeometricVerifierTaskContext *context =
      make_context(runtime, &durable);
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

Lardon3DTask *lardon3d_project_create_geometric_verifier_task(
    Lardon3DAppState *state,
    const Lardon3DGeometricVerifierTaskConfiguration *configuration,
    uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!state || !state->project_loaded || !state->project_db ||
      !state->resource_governor || !configuration || !task_id ||
      !lardon3d_geometric_verifier_parameters_valid(&configuration->verifier)) {
    return NULL;
  }
  uint64_t id = 0;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) !=
      LARDON3D_PROJECT_DB_OK) {
    return NULL;
  }
  Lardon3DProjectDbGeometricVerifierTask durable = {
      .task_id = id,
      .threshold_pixels = configuration->verifier.threshold_pixels,
      .confidence = configuration->verifier.confidence,
      .max_iterations = configuration->verifier.max_iterations,
      .min_inlier_count = configuration->verifier.min_inlier_count,
      .min_inlier_ratio = configuration->verifier.min_inlier_ratio,
      .seed_policy_version = configuration->verifier.seed_policy_version,
      .canonicalization_version =
          configuration->verifier.canonicalization_version,
  };
  if (!lardon3d_geometric_verifier_fingerprint_for_version(
          &configuration->verifier, LARDON3D_GEOMETRIC_VERIFIER_VERSION,
          durable.parameter_fingerprint)) {
    return NULL;
  }
  Lardon3DTaskReconstructionContext runtime = {
      .project_path = state->project_path,
      .project_db = state->project_db,
      .resource_governor = state->resource_governor,
  };
  Lardon3DGeometricVerifierTaskContext *context =
      make_context(&runtime, &durable);
  if (!context) {
    return NULL;
  }
  const Lardon3DResourceEstimate estimate = {
      .memory_bytes_per_item = GEOMETRIC_VERIFIER_MEMORY_BYTES_PER_ITEM,
      .minimum_batch_size = LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH,
      .maximum_batch_size = LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH,
      /* CPU8 is the largest width whose complete preparation + ordered publish
       * + checkpoint rate improved materially. CPU12 was exact and safe but
       * added only 2.68% over CPU8, below Governor's 5% acceptance threshold;
       * the independently safe participant/window bound remains 16. */
      .desired_cpu_threads =
          LARDON3D_GEOMETRIC_VERIFIER_TASK_VALIDATED_USEFUL_CPU_THREADS,
      .desired_io_slots = 1,
      .task_class = LARDON3D_RESOURCE_TASK_CPU,
  };
  Lardon3DTask *task =
      lardon3d_task_create_typed("Geometric Verification", &estimate,
                                 LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND,
                                 LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION,
                                 run, context, destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished_callback, context) ||
      lardon3d_project_checkpoint_geometric_verifier_task(
          state, task, &durable) != LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

bool lardon3d_project_enqueue_geometric_verifier_task(
    Lardon3DAppState *state,
    const Lardon3DGeometricVerifierTaskConfiguration *configuration,
    uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!state || !state->task_queue) {
    return false;
  }
  Lardon3DTask *task = lardon3d_project_create_geometric_verifier_task(
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
