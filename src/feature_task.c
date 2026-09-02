#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>

#include "opencv_task_thread_control.h"
#include "task_internal.h"

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DProjectDbFeatureExtractTask parameters;
} Lardon3DFeatureTaskContext;

enum {
  FEATURE_BATCH_MAX = 12,
  FEATURE_BATCH_CHILD_STACK = 1024 * 1024,
};

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DProjectDbFeatureExtractBatchTask parameters;
} Lardon3DFeatureBatchTaskContext;

static void destroy_context(void *userdata) { free(userdata); }

static void runtime_state(const Lardon3DFeatureTaskContext *context, Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  (void)snprintf(state->project_path, sizeof(state->project_path), "%s", context->project_path);
}

static bool file_hash(const char *path, unsigned char output[32]) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
  unsigned char buffer[65536];
  while (ok) {
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    ok = EVP_DigestUpdate(ctx, buffer, (size_t)n) == 1;
  }
  unsigned int size = 0;
  ok = ok && EVP_DigestFinal_ex(ctx, output, &size) == 1 && size == 32;
  if (ctx) {
    EVP_MD_CTX_free(ctx);
  }
  if (close(fd) != 0) {
    ok = false;
  }
  return ok;
}

static uint64_t elapsed_ns(struct timespec a, struct timespec b) {
  uint64_t s = b.tv_sec >= a.tv_sec ? (uint64_t)(b.tv_sec - a.tv_sec) : 0;
  long n = b.tv_nsec - a.tv_nsec;
  if (n < 0 && s) {
    --s;
    n += 1000000000L;
  }
  return s <= UINT64_MAX / 1000000000ULL ? s * 1000000000ULL + (uint64_t)n : UINT64_MAX;
}

static void finished(const Lardon3DTask *task, void *userdata) {
#ifdef LARDON3D_FEATURE_TASK_TESTING
  const char *skip = getenv("LARDON3D_TEST_FEATURE_SKIP_FINISHED_CHECKPOINT");
  if (skip && strcmp(skip, "1") == 0) {
    return;
  }
#endif
  Lardon3DFeatureTaskContext *context = userdata;
  Lardon3DAppState state;
  runtime_state(context, &state);
  (void)lardon3d_project_checkpoint_feature_extract_task(&state, task, &context->parameters);
}

static Lardon3DProjectDbResult find_valid_feature_set(const Lardon3DFeatureTaskContext *context,
                                                      const unsigned char fingerprint[32]) {
  Lardon3DProjectDbFeatureSet existing;
  Lardon3DProjectDbResult found = lardon3d_project_db_find_feature_set(
      context->database, context->parameters.image_id, LARDON3D_FEATURE_EXTRACTOR_KIND,
      LARDON3D_FEATURE_EXTRACTOR_VERSION, fingerprint, &existing);
  if (found != LARDON3D_PROJECT_DB_OK) {
    return found;
  }
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  Lardon3DFeatureStoreResult valid =
      lardon3d_feature_reader_open(context->project_path, &existing, &reader, &metadata);
  lardon3d_feature_reader_close(reader);
  return valid == LARDON3D_FEATURE_STORE_OK ? LARDON3D_PROJECT_DB_OK : LARDON3D_PROJECT_DB_CORRUPT;
}

static bool load_validated_source(const Lardon3DFeatureTaskContext *context, char path[PATH_MAX]) {
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  if (lardon3d_project_db_load_image(context->database, context->parameters.image_id, &image,
                                     &asset) != LARDON3D_PROJECT_DB_OK) {
    return false;
  }
  int written = snprintf(path, PATH_MAX, "%s/%s", context->project_path, asset.path);
  if (written <= 0 || (size_t)written >= PATH_MAX) {
    return false;
  }
  unsigned char actual[32];
  return file_hash(path, actual) && memcmp(actual, asset.sha256, 32) == 0;
}

static void batch_runtime_state(const Lardon3DFeatureBatchTaskContext *context,
                                Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  (void)snprintf(state->project_path, sizeof(state->project_path), "%s", context->project_path);
}

static bool batch_checkpoint(Lardon3DFeatureBatchTaskContext *context, Lardon3DTask *task,
                             uint32_t cursor) {
  Lardon3DAppState state;
  batch_runtime_state(context, &state);
  Lardon3DProjectDbFeatureExtractBatchTask parameters = context->parameters;
  parameters.task_id = lardon3d_task_id(task);
  parameters.next_item_index = cursor;
  return lardon3d_project_checkpoint_feature_extract_batch_task(&state, task, &parameters) ==
         LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}

typedef struct {
  Lardon3DFeatureBatchTaskContext *context;
  uint32_t item_index;
  uint64_t image_id;
  char source_path[PATH_MAX];
  bool already_ready;
  Lardon3DFeatureExtractResult result;
  Lardon3DExtractedFeatures features;
} FeatureBatchWorker;

static void *extract_batch_item(void *value) {
  FeatureBatchWorker *worker = value;
  if (worker->already_ready) return NULL;
  Lardon3DFeatureExtractorParameters parameters = {
      worker->context->parameters.max_features,
      worker->context->parameters.pyramid_levels,
      worker->context->parameters.fast_threshold};
  worker->result = lardon3d_feature_extract_orb(worker->source_path, &parameters,
                                                &worker->features);
  return NULL;
}

typedef struct {
  FeatureBatchWorker *workers;
  size_t count;
  size_t first;
  size_t stride;
} FeatureBatchParticipant;

static void *extract_batch_participant(void *value) {
  FeatureBatchParticipant *participant = value;
  for (size_t index = participant->first; index < participant->count;
       index += participant->stride)
    (void)extract_batch_item(&participant->workers[index]);
  return NULL;
}

static bool extract_batch_window(FeatureBatchWorker *workers, size_t count,
                                 unsigned int admitted_threads) {
  if (!workers || count == 0 || count > FEATURE_BATCH_MAX || admitted_threads == 0) return false;
  size_t participants = count < admitted_threads ? count : admitted_threads;
  pthread_t children[FEATURE_BATCH_MAX - 1];
  FeatureBatchParticipant work[FEATURE_BATCH_MAX];
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  if (pthread_attr_setstacksize(&attributes, FEATURE_BATCH_CHILD_STACK) != 0) {
    (void)pthread_attr_destroy(&attributes);
    return false;
  }
  size_t created = 0;
  bool started = true;
  for (size_t participant = 1; participant < participants; ++participant) {
    work[participant] = (FeatureBatchParticipant){workers, count, participant, participants};
    /* Participants own only disjoint extraction outputs. SQLite, Feature Store
       publication, cursor advancement, and generic checkpoints remain owner-only. */
    if (pthread_create(&children[created], &attributes, extract_batch_participant,
                       &work[participant]) != 0) {
      started = false;
      break;
    }
    ++created;
  }
  if (pthread_attr_destroy(&attributes) != 0) started = false;
  work[0] = (FeatureBatchParticipant){workers, count, 0, participants};
  if (started) (void)extract_batch_participant(&work[0]);
  for (size_t index = 0; index < created; ++index)
    if (pthread_join(children[index], NULL) != 0) started = false;
  return started;
}

static void destroy_batch_outputs(FeatureBatchWorker *workers, size_t count) {
  if (!workers) return;
  /* A joined window owns every participant output until ordered owner
     publication consumes it. Any boundary failure must release the whole
     window; the Governor reservation must not outlive abandoned ORB buffers. */
  for (size_t index = 0; index < count; ++index)
    lardon3d_extracted_features_destroy(&workers[index].features);
}

static bool run_batch_body(Lardon3DTask *task, Lardon3DFeatureBatchTaskContext *context) {
  for (;;) {
    if (!lardon3d_task_checkpoint(task)) return false;
    Lardon3DProjectDbFeatureExtractBatchTask persisted;
    Lardon3DProjectDbSelectedExecution execution;
    if (lardon3d_project_db_load_feature_extract_batch_task(
            context->database, lardon3d_task_id(task), &persisted) != LARDON3D_PROJECT_DB_OK ||
        memcmp(&persisted.parameter_fingerprint, &context->parameters.parameter_fingerprint,
               32) != 0 ||
        lardon3d_project_db_load_selected_execution(context->database,
            context->parameters.selected_execution_id, &execution) != LARDON3D_PROJECT_DB_OK)
      return lardon3d_task_fail(task, "État durable Feature batch invalide.");
    if (persisted.next_item_index == execution.item_count)
      return lardon3d_task_set_durable_progress(task, execution.item_count, execution.item_count,
                                                "Extraction Feature batch terminée.") &&
             batch_checkpoint(context, task, execution.item_count);

    Lardon3DTaskExecutionContract contract;
    if (!lardon3d_task_execution_contract(task, &contract) || contract.cpu_threads == 0 ||
        contract.batch_size == 0 || contract.batch_size > FEATURE_BATCH_MAX)
      return lardon3d_task_fail(task, "Contrat Governor Feature batch invalide.");
    size_t count = execution.item_count - persisted.next_item_index;
    if (count > contract.batch_size) count = contract.batch_size;
    FeatureBatchWorker workers[FEATURE_BATCH_MAX] = {{0}};
    for (size_t offset = 0; offset < count; ++offset) {
      FeatureBatchWorker *worker = &workers[offset];
      Lardon3DProjectDbSelectedExecutionItem item;
      if (lardon3d_project_db_load_selected_execution_item(
              context->database, context->parameters.selected_execution_id,
              persisted.next_item_index + (uint32_t)offset, &item) != LARDON3D_PROJECT_DB_OK ||
          !item.has_image)
        return lardon3d_task_fail(task, "Item sélectionné Feature batch invalide.");
      worker->context = context;
      worker->item_index = item.item_index;
      worker->image_id = item.image_id;
      Lardon3DFeatureTaskContext single = {0};
      single.database = context->database;
      single.parameters.image_id = item.image_id;
      (void)snprintf(single.project_path, sizeof(single.project_path), "%s", context->project_path);
      Lardon3DProjectDbResult found = find_valid_feature_set(
          &single, context->parameters.parameter_fingerprint);
      worker->already_ready = found == LARDON3D_PROJECT_DB_OK;
      if (found == LARDON3D_PROJECT_DB_CORRUPT ||
          (found != LARDON3D_PROJECT_DB_OK && found != LARDON3D_PROJECT_DB_NOT_FOUND) ||
          (!worker->already_ready && !load_validated_source(&single, worker->source_path)))
        return lardon3d_task_fail(task, "Source ou Feature Set batch invalide.");
    }

    struct timespec begin = {0}, end = {0};
    bool timing_known = clock_gettime(CLOCK_MONOTONIC, &begin) == 0;
    if (!extract_batch_window(workers, count, contract.cpu_threads)) {
      destroy_batch_outputs(workers, count);
      return lardon3d_task_fail(task, "Participants Feature batch impossibles.");
    }
    if (!lardon3d_task_checkpoint(task)) {
      destroy_batch_outputs(workers, count);
      return false;
    }
    size_t durable_items = 0;
    Lardon3DAppState state;
    batch_runtime_state(context, &state);
    Lardon3DFeatureExtractorParameters parameters = {
        context->parameters.max_features, context->parameters.pyramid_levels,
        context->parameters.fast_threshold};
    for (size_t offset = 0; offset < count; ++offset) {
      FeatureBatchWorker *worker = &workers[offset];
      if (!worker->already_ready) {
        if (worker->result != LARDON3D_FEATURE_EXTRACT_OK) {
          destroy_batch_outputs(workers, count);
          return lardon3d_task_fail(task, "Extraction ORB batch impossible.");
        }
        Lardon3DProjectDbFeatureSet set;
        Lardon3DFeatureStoreResult published = lardon3d_feature_store_publish(
            &state, worker->image_id, lardon3d_task_id(task), &parameters,
            &worker->features, &set);
        lardon3d_extracted_features_destroy(&worker->features);
        if (published != LARDON3D_FEATURE_STORE_OK &&
            published != LARDON3D_FEATURE_STORE_ALREADY_PRESENT) {
          destroy_batch_outputs(workers, count);
          return lardon3d_task_fail(task, "Publication Feature batch impossible.");
        }
        if (published == LARDON3D_FEATURE_STORE_OK) ++durable_items;
      }
      if (lardon3d_project_db_advance_feature_extract_batch_task(
              context->database, lardon3d_task_id(task), worker->item_index,
              worker->item_index + 1u) != LARDON3D_PROJECT_DB_OK ||
          !lardon3d_task_set_durable_progress(task, worker->item_index + 1u,
                                              execution.item_count,
                                              "Feature batch publié.") ||
          !batch_checkpoint(context, task, worker->item_index + 1u)) {
        destroy_batch_outputs(workers, count);
        return lardon3d_task_fail(task, "Curseur Feature batch impossible.");
      }
    }
    timing_known = timing_known && clock_gettime(CLOCK_MONOTONIC, &end) == 0;
    if (timing_known)
      (void)lardon3d_task_internal_record_sequence(task, elapsed_ns(begin, end), durable_items);
    if (persisted.next_item_index + count == execution.item_count) return true;
    Lardon3DResourceReservation *reservation = NULL;
    if (!lardon3d_task_sequence_break(task, context->governor, &reservation, &contract))
      return false;
  }
}

static bool run_batch(Lardon3DTask *task, void *userdata) {
  Lardon3DFeatureBatchTaskContext *context = userdata;
  Lardon3DTaskExecutionContract contract;
  if (!lardon3d_task_execution_contract(task, &contract) || contract.cpu_threads == 0)
    return lardon3d_task_fail(task, "Contrat CPU Feature batch invalide.");
  Lardon3DOpenCvTaskThreadControl control = {
      .previous = lardon3d_feature_opencv_thread_count(),
      .restore_required = true,
  };
  /* Cross-image participants are the admitted CPU dimension. OpenCV stays at
     one thread per extraction to prevent nested oversubscription; Queue owns
     the sole process-wide guard. Configuration may mutate before verification
     fails, so restoration duty exists before configure and covers every exit. */
  if (!lardon3d_feature_opencv_configure_threads(1)) {
    (void)lardon3d_opencv_task_threads_end(&control);
    return lardon3d_task_fail(task, "Configuration OpenCV Feature batch impossible.");
  }
  bool result = run_batch_body(task, context);
  if (!lardon3d_opencv_task_threads_end(&control))
    return lardon3d_task_fail(task, "Restauration OpenCV Feature batch impossible.");
  return result;
}

#ifdef LARDON3D_FEATURE_TASK_TESTING
bool lardon3d_feature_extract_batch_test_run(Lardon3DTask *task) {
  Lardon3DFeatureBatchTaskContext context = {0};
  return run_batch(task, &context);
}
#endif

static void finished_batch(const Lardon3DTask *task, void *userdata) {
  Lardon3DFeatureBatchTaskContext *context = userdata;
  Lardon3DProjectDbFeatureExtractBatchTask persisted;
  if (lardon3d_project_db_load_feature_extract_batch_task(
          context->database, lardon3d_task_id(task), &persisted) == LARDON3D_PROJECT_DB_OK)
    (void)batch_checkpoint(context, (Lardon3DTask *)task, persisted.next_item_index);
}

static bool run_body(Lardon3DTask *task, void *userdata, size_t *durable_items) {
  Lardon3DFeatureTaskContext *context = userdata;
  *durable_items = 0;
  if (!lardon3d_task_checkpoint(task)) {
    return false;
  }
  struct timespec begin = {0};
  bool timing_known = clock_gettime(CLOCK_MONOTONIC, &begin) == 0;
  unsigned char fingerprint[32];
  lardon3d_feature_extractor_parameter_fingerprint(
      &(Lardon3DFeatureExtractorParameters){context->parameters.max_features,
                                            context->parameters.pyramid_levels,
                                            context->parameters.fast_threshold},
      fingerprint);
  Lardon3DProjectDbResult found = find_valid_feature_set(context, fingerprint);
  if (found == LARDON3D_PROJECT_DB_OK) {
    return lardon3d_task_set_progress(task, 100, "Features déjà présentes.");
  }
  if (found == LARDON3D_PROJECT_DB_CORRUPT) {
    return lardon3d_task_fail(task, "Feature Set READY absent ou corrompu.");
  }
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND) {
    return lardon3d_task_fail(task, "Recherche Feature Store impossible.");
  }
  char path[PATH_MAX];
  if (!load_validated_source(context, path)) {
    return lardon3d_task_fail(task, "Asset image absent ou corrompu.");
  }
  Lardon3DFeatureExtractorParameters parameters = {context->parameters.max_features,
                                                   context->parameters.pyramid_levels,
                                                   context->parameters.fast_threshold};
  Lardon3DExtractedFeatures features;
  Lardon3DFeatureExtractResult extracted =
      lardon3d_feature_extract_orb(path, &parameters, &features);
  if (extracted != LARDON3D_FEATURE_EXTRACT_OK) {
    return lardon3d_task_fail(task, "Décodage ou extraction ORB impossible.");
  }
  if (!lardon3d_task_checkpoint(task)) {
    lardon3d_extracted_features_destroy(&features);
    return false;
  }
#ifdef LARDON3D_FEATURE_TASK_TESTING
  const char *pause = getenv("LARDON3D_TEST_FEATURE_PAUSE_BEFORE_PUBLISH");
  if (pause && strcmp(pause, "1") == 0) {
    (void)lardon3d_task_pause(task);
    if (!lardon3d_task_checkpoint(task)) {
      lardon3d_extracted_features_destroy(&features);
      return false;
    }
  }
#endif
  Lardon3DAppState state;
  runtime_state(context, &state);
  Lardon3DProjectDbFeatureSet set;
  Lardon3DFeatureStoreResult published = lardon3d_feature_store_publish(
      &state, context->parameters.image_id, lardon3d_task_id(task), &parameters, &features, &set);
  lardon3d_extracted_features_destroy(&features);
  if (published != LARDON3D_FEATURE_STORE_OK &&
      published != LARDON3D_FEATURE_STORE_ALREADY_PRESENT &&
      published != LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE) {
    return lardon3d_task_fail(task, "Publication Feature Store impossible.");
  }
  *durable_items = published == LARDON3D_FEATURE_STORE_OK ? 1 : 0;
  struct timespec end = {0};
  timing_known = timing_known && clock_gettime(CLOCK_MONOTONIC, &end) == 0;
  if (*durable_items > 0 && timing_known) {
    (void)lardon3d_resource_governor_record_batch(
        context->governor, LARDON3D_RESOURCE_TASK_CPU, *durable_items,
        elapsed_ns(begin, end), 0);
  }
  return lardon3d_task_set_progress(task, 100, "Feature Set publié.");
}

static bool run(Lardon3DTask *task, void *userdata) {
  struct timespec begin = {0};
  struct timespec end = {0};
  bool timing_known = clock_gettime(CLOCK_MONOTONIC, &begin) == 0;
  Lardon3DOpenCvTaskThreadControl threads;
  if (!lardon3d_opencv_task_threads_begin(task, INT_MAX, &threads)) {
    /* begin may have changed OpenCV before verification failed. If its first
     * rollback was transiently unsuccessful, this bounded second attempt must
     * discharge the process-global ownership before the callback returns. */
    if (threads.restore_required &&
        !lardon3d_opencv_task_threads_end(&threads)) {
      return lardon3d_task_fail(task, "Restauration OpenCV Features impossible.");
    }
    return lardon3d_task_fail(task, "Contrat CPU OpenCV Features invalide.");
  }
  /* EXTERNAL LIBRARY: OpenCV accepts a positive signed-int thread count.
   * Queue has one callback owner, so this process-wide setting cannot race
   * another Task. Governor clamps the portable INT_MAX capability to the host
   * compute pool; the guard applies that immutable admission and restores it
   * on every return without changing Feature identity. */
  size_t durable_items = 0;
  bool result = run_body(task, userdata, &durable_items);
  if (!lardon3d_opencv_task_threads_end(&threads)) {
    (void)lardon3d_task_fail(task, "Restauration OpenCV Features impossible.");
    return false;
  }
  timing_known = timing_known && clock_gettime(CLOCK_MONOTONIC, &end) == 0;
  if (result && timing_known) {
    /* Only extraction followed by this Task's durable publication contributes
     * work. READY reuse, an ALREADY_PRESENT collision, and uncertain directory
     * sync remain successful outcomes, but record zero so feedback can neither
     * train nor advance the next immutable CPU trial. */
    (void)lardon3d_task_internal_record_sequence(
        task, elapsed_ns(begin, end), durable_items);
  }
  return result;
}

static Lardon3DFeatureTaskContext *
make_context(const Lardon3DTaskReconstructionContext *runtime,
             const Lardon3DProjectDbFeatureExtractTask *parameters) {
  if (!runtime || !runtime->project_path || !runtime->project_db || !runtime->resource_governor ||
      !parameters) {
    return NULL;
  }
  Lardon3DFeatureTaskContext *context = calloc(1, sizeof(*context));
  if (!context) {
    return NULL;
  }
  int n =
      snprintf(context->project_path, sizeof(context->project_path), "%s", runtime->project_path);
  if (n <= 0 || (size_t)n >= sizeof(context->project_path)) {
    free(context);
    return NULL;
  }
  context->database = runtime->project_db;
  context->governor = runtime->resource_governor;
  context->parameters = *parameters;
  return context;
}

bool lardon3d_feature_extract_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
                                          void *userdata, Lardon3DTaskKindBinding *binding) {
  Lardon3DTaskReconstructionContext *runtime = userdata;
  if (!snapshot || !runtime || !binding) {
    return false;
  }
  Lardon3DProjectDbFeatureExtractTask parameters;
  if (lardon3d_project_db_load_feature_extract_task(runtime->project_db, snapshot->id,
                                                    &parameters) != LARDON3D_PROJECT_DB_OK ||
      strcmp(parameters.extractor_kind, LARDON3D_FEATURE_EXTRACTOR_KIND) != 0 ||
      parameters.extractor_version != 1) {
    return false;
  }
  unsigned char expected[32];
  lardon3d_feature_extractor_parameter_fingerprint(
      &(Lardon3DFeatureExtractorParameters){parameters.max_features, parameters.pyramid_levels,
                                            parameters.fast_threshold},
      expected);
  if (memcmp(expected, parameters.parameter_fingerprint, 32) != 0) {
    return false;
  }
  Lardon3DFeatureTaskContext *context = make_context(runtime, &parameters);
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

Lardon3DTask *
lardon3d_project_create_feature_extract_task(Lardon3DAppState *state, uint64_t image_id,
                                             const Lardon3DFeatureExtractorParameters *parameters,
                                             uint64_t *task_id) {
  if (task_id) {
    *task_id = 0;
  }
  if (!state || !state->project_loaded || !state->project_db || !state->resource_governor ||
      !parameters || !task_id || !lardon3d_feature_extractor_parameters_valid(parameters)) {
    return NULL;
  }
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  if (lardon3d_project_db_load_image(state->project_db, image_id, &image, &asset) !=
      LARDON3D_PROJECT_DB_OK) {
    return NULL;
  }
  uint64_t id;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK) {
    return NULL;
  }
  Lardon3DProjectDbFeatureExtractTask durable = {.task_id = id,
                                                 .image_id = image_id,
                                                 .extractor_version = 1,
                                                 .max_features = parameters->max_features,
                                                 .pyramid_levels = parameters->pyramid_levels,
                                                 .fast_threshold = parameters->fast_threshold};
  snprintf(durable.extractor_kind, sizeof(durable.extractor_kind), "%s",
           LARDON3D_FEATURE_EXTRACTOR_KIND);
  lardon3d_feature_extractor_parameter_fingerprint(parameters, durable.parameter_fingerprint);
  Lardon3DTaskReconstructionContext runtime = {
      .project_path = state->project_path,
      .project_db = state->project_db,
      .resource_governor = state->resource_governor,
      .orb_vulkan_backend = state->orb_vulkan_backend,
  };
  Lardon3DFeatureTaskContext *context = make_context(&runtime, &durable);
  if (!context) {
    return NULL;
  }
  Lardon3DResourceEstimate estimate = {.memory_fixed_bytes = 64ULL * 1024 * 1024,
                                       .memory_bytes_per_item = 512ULL * 1024 * 1024,
                                       .minimum_batch_size = 1,
                                       .maximum_batch_size = 1,
                                       /* EXTERNAL LIBRARY: OpenCV's positive-int API is the safe
                                        * ceiling; Governor supplies the portable host maximum. */
                                       .desired_cpu_threads = INT_MAX,
                                       .desired_io_slots = 1,
                                       .task_class = LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DTask *task = lardon3d_task_create_typed("Extraction de features", &estimate,
                                                  LARDON3D_FEATURE_EXTRACT_TASK_KIND, 1, run,
                                                  context, destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished, context) ||
      lardon3d_project_checkpoint_feature_extract_task(state, task, &durable) !=
          LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

bool lardon3d_project_enqueue_feature_extract(Lardon3DAppState *state, uint64_t image_id,
                                              const Lardon3DFeatureExtractorParameters *parameters,
                                              uint64_t *task_id) {
  if (!state || !state->task_queue) {
    return false;
  }
  Lardon3DTask *task =
      lardon3d_project_create_feature_extract_task(state, image_id, parameters, task_id);
  if (!task) {
    return false;
  }
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}

static Lardon3DFeatureBatchTaskContext *make_batch_context(
    const Lardon3DTaskReconstructionContext *runtime,
    const Lardon3DProjectDbFeatureExtractBatchTask *parameters) {
  if (!runtime || !runtime->project_path || !runtime->project_db ||
      !runtime->resource_governor || !parameters) return NULL;
  Lardon3DFeatureBatchTaskContext *context = calloc(1, sizeof(*context));
  if (!context) return NULL;
  int n = snprintf(context->project_path, sizeof(context->project_path), "%s",
                   runtime->project_path);
  if (n <= 0 || (size_t)n >= sizeof(context->project_path)) {
    free(context);
    return NULL;
  }
  context->database = runtime->project_db;
  context->governor = runtime->resource_governor;
  context->parameters = *parameters;
  return context;
}

bool lardon3d_feature_extract_batch_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  Lardon3DTaskReconstructionContext *runtime = userdata;
  if (!snapshot || !runtime || !binding) return false;
  Lardon3DProjectDbFeatureExtractBatchTask parameters;
  if (lardon3d_project_db_load_feature_extract_batch_task(
          runtime->project_db, snapshot->id, &parameters) != LARDON3D_PROJECT_DB_OK)
    return false;
  unsigned char expected[32];
  lardon3d_feature_extractor_parameter_fingerprint(
      &(Lardon3DFeatureExtractorParameters){parameters.max_features,
                                            parameters.pyramid_levels,
                                            parameters.fast_threshold}, expected);
  if (memcmp(expected, parameters.parameter_fingerprint, 32) != 0) return false;
  Lardon3DFeatureBatchTaskContext *context = make_batch_context(runtime, &parameters);
  if (!context) return false;
  *binding = (Lardon3DTaskKindBinding){.callback = run_batch,
                                       .userdata = context,
                                       .userdata_destroy = destroy_context,
                                       .finished_callback = finished_batch,
                                       .finished_userdata = context};
  return true;
}

Lardon3DTask *lardon3d_project_create_feature_extract_batch_task(
    Lardon3DAppState *state, uint64_t selected_execution_id,
    const Lardon3DFeatureExtractorParameters *parameters, uint64_t *task_id) {
  if (task_id) *task_id = 0;
  if (!state || !state->project_loaded || !state->project_db ||
      !state->resource_governor || !parameters || !task_id ||
      !lardon3d_feature_extractor_parameters_valid(parameters) || selected_execution_id == 0)
    return NULL;
  Lardon3DProjectDbSelectedExecution execution;
  if (lardon3d_project_db_load_selected_execution(state->project_db, selected_execution_id,
                                                   &execution) != LARDON3D_PROJECT_DB_OK ||
      execution.stage < LARDON3D_SELECTED_EXECUTION_CALIBRATION || execution.item_count == 0)
    return NULL;
  uint64_t id = 0;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK)
    return NULL;
  Lardon3DProjectDbFeatureExtractBatchTask durable = {
      .task_id = id,
      .selected_execution_id = selected_execution_id,
      .next_item_index = 0,
      .extractor_version = LARDON3D_FEATURE_EXTRACTOR_VERSION,
      .max_features = parameters->max_features,
      .pyramid_levels = parameters->pyramid_levels,
      .fast_threshold = parameters->fast_threshold};
  (void)snprintf(durable.extractor_kind, sizeof(durable.extractor_kind), "%s",
                 LARDON3D_FEATURE_EXTRACTOR_KIND);
  lardon3d_feature_extractor_parameter_fingerprint(parameters,
                                                   durable.parameter_fingerprint);
  Lardon3DTaskReconstructionContext runtime = {
      state->project_path, state->project_db, state->resource_governor,
      state->orb_vulkan_backend};
  Lardon3DFeatureBatchTaskContext *context = make_batch_context(&runtime, &durable);
  if (!context) return NULL;
  /* One admitted participant owns decode, ORB pyramid/keypoints/descriptors,
     and publication staging bounded by the existing conservative 512 MiB/item.
     The 1..12 ceiling is operational benchmark capacity, not selected-domain
     cardinality; Governor feedback records each joined window's useful items. */
  Lardon3DResourceEstimate estimate = {
      .memory_fixed_bytes = 64ULL * 1024 * 1024,
      .memory_bytes_per_item = 512ULL * 1024 * 1024,
      .minimum_batch_size = 1,
      .maximum_batch_size = FEATURE_BATCH_MAX,
      .desired_cpu_threads = FEATURE_BATCH_MAX,
      .desired_io_slots = 1,
      .task_class = LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DTask *task = lardon3d_task_create_typed(
      "Extraction Feature sélectionnée", &estimate,
      LARDON3D_FEATURE_EXTRACT_BATCH_TASK_KIND,
      LARDON3D_FEATURE_EXTRACT_BATCH_TASK_KIND_VERSION, run_batch, context,
      destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished_batch, context) ||
      !batch_checkpoint(context, task, 0)) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

bool lardon3d_project_enqueue_feature_extract_batch(
    Lardon3DAppState *state, uint64_t selected_execution_id,
    const Lardon3DFeatureExtractorParameters *parameters, uint64_t *task_id) {
  if (!state || !state->task_queue) return false;
  Lardon3DTask *task = lardon3d_project_create_feature_extract_batch_task(
      state, selected_execution_id, parameters, task_id);
  if (!task) return false;
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
