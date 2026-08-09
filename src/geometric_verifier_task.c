#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lardon3d/geometric_verifier_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

enum {
  GEOMETRIC_VERIFIER_PAGE_CAPACITY =
      LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH + 1,
  GEOMETRIC_VERIFIER_MEMORY_BYTES = 4 * 1024 * 1024,
  GEOMETRIC_VERIFIER_CPU_THREADS = 1,
};

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DProjectDbGeometricVerifierTask durable;
} Lardon3DGeometricVerifierTaskContext;

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

static bool process_parent(Lardon3DTask *task,
                           Lardon3DGeometricVerifierTaskContext *context,
                           const Lardon3DProjectDbMatchResult *parent) {
  if (!lardon3d_task_checkpoint(task)) {
    return false;
  }
  if (parent->result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED ||
      parent->match_count == 0) {
    return true;
  }
  Lardon3DGeometricVerifierParameters parameters =
      parameters_from_durable(&context->durable);
  Lardon3DProjectDbGeometricVerificationResult result;
  bool reused = false;
  if (lardon3d_geometric_verifier_verify_and_publish(
          context->project_path, context->database, parent->match_result_id,
          &parameters, &result, &reused) != LARDON3D_GEOMETRIC_VERIFIER_OK) {
    return lardon3d_task_fail(task, "Vérification géométrique impossible.");
  }
#ifdef LARDON3D_GEOMETRIC_VERIFIER_TASK_TESTING
  const char *pause = getenv("LARDON3D_TEST_GEOMETRIC_PAUSE_AFTER_PUBLICATION");
  if (pause && strcmp(pause, "1") == 0) {
    (void)lardon3d_task_pause(task);
    return lardon3d_task_checkpoint(task);
  }
#endif
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
        contract.batch_size < LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH ||
        contract.batch_size > LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH) {
      return lardon3d_task_fail(task,
                                "Contrat de lot Geometric Verifier invalide.");
    }
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
    for (size_t index = 0; index < batch_count; ++index) {
      if (!process_parent(task, context, &page[index])) {
        return false;
      }
      context->durable.after_match_result_id = page[index].match_result_id;
      ++processed;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    (void)lardon3d_resource_governor_record_batch(
        context->governor, LARDON3D_RESOURCE_TASK_CPU, batch_count,
        elapsed_ns(begin, end), 0);
    bool exhausted = count <= contract.batch_size;
    if (!checkpoint_batch(task, context, exhausted ? 100U : 99U, processed)) {
      return lardon3d_task_fail(task,
                                "Checkpoint Geometric Verifier impossible.");
    }
    if (exhausted) {
      return lardon3d_task_set_progress(task, 100,
                                        "Vérification géométrique terminée.");
    }
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
  lardon3d_geometric_verifier_fingerprint(&parameters, fingerprint);
  if (!lardon3d_geometric_verifier_parameters_valid(&parameters) ||
      memcmp(fingerprint, durable->parameter_fingerprint,
             sizeof(fingerprint)) != 0) {
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
  lardon3d_geometric_verifier_fingerprint(&configuration->verifier,
                                          durable.parameter_fingerprint);
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
      .memory_fixed_bytes = GEOMETRIC_VERIFIER_MEMORY_BYTES,
      .minimum_batch_size = LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH,
      .maximum_batch_size = LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH,
      .desired_cpu_threads = GEOMETRIC_VERIFIER_CPU_THREADS,
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
