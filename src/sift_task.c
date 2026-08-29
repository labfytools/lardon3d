#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lardon3d/feature_store.h>
#include <lardon3d/project.h>
#include <lardon3d/sift_task.h>
#include <lardon3d/task_queue.h>

typedef struct {
  char project_path[PATH_MAX];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  Lardon3DProjectDbSiftExtractTask parameters;
} SiftTaskContext;

static void destroy_context(void *userdata) { free(userdata); }

static void runtime_state(const SiftTaskContext *context, Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  snprintf(state->project_path, sizeof(state->project_path), "%s", context->project_path);
}

static uint64_t elapsed_ns(struct timespec a, struct timespec b) {
  uint64_t seconds = b.tv_sec >= a.tv_sec ? (uint64_t)(b.tv_sec - a.tv_sec) : 0;
  long nanoseconds = b.tv_nsec - a.tv_nsec;
  if (nanoseconds < 0 && seconds) {
    --seconds;
    nanoseconds += 1000000000L;
  }
  return seconds <= UINT64_MAX / UINT64_C(1000000000)
             ? seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds
             : UINT64_MAX;
}

static bool file_hash(const char *path, unsigned char output[32]) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat information;
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  bool ok = fstat(fd, &information) == 0 && S_ISREG(information.st_mode) && digest &&
            EVP_DigestInit_ex(digest, EVP_sha256(), NULL) == 1;
  unsigned char buffer[65536];
  while (ok) {
    ssize_t size = read(fd, buffer, sizeof(buffer));
    if (size < 0 && errno == EINTR) continue;
    if (size < 0) {
      ok = false;
      break;
    }
    if (size == 0) break;
    ok = EVP_DigestUpdate(digest, buffer, (size_t)size) == 1;
  }
  unsigned int length = 0;
  ok = ok && EVP_DigestFinal_ex(digest, output, &length) == 1 && length == 32;
  EVP_MD_CTX_free(digest);
  if (close(fd) != 0) ok = false;
  return ok;
}

static Lardon3DSiftExtractorParameters extract_parameters(const SiftTaskContext *context) {
  const Lardon3DProjectDbSiftExtractTask *p = &context->parameters;
  return (Lardon3DSiftExtractorParameters){p->max_features,
                                           p->octave_layers,
                                           p->contrast_threshold,
                                           p->edge_threshold,
                                           p->sigma,
                                           p->grid_rows,
                                           p->grid_cols,
                                           p->max_features_per_cell,
                                           strcmp(p->extractor_kind, "rootsift") == 0};
}

static bool source_path(const SiftTaskContext *context, char path[PATH_MAX]) {
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  if (lardon3d_project_db_load_image(context->database, context->parameters.image_id, &image,
                                     &asset) != LARDON3D_PROJECT_DB_OK)
    return false;
  int length = snprintf(path, PATH_MAX, "%s/%s", context->project_path, asset.path);
  unsigned char actual[32];
  return length > 0 && length < PATH_MAX && file_hash(path, actual) &&
         memcmp(actual, asset.sha256, 32) == 0;
}

static bool run(Lardon3DTask *task, void *userdata) {
  SiftTaskContext *context = userdata;
  if (!lardon3d_task_checkpoint(task)) return false;
  Lardon3DProjectDbFeatureSet existing;
  Lardon3DProjectDbResult found = lardon3d_project_db_find_feature_set(
      context->database, context->parameters.image_id, context->parameters.extractor_kind, 1,
      context->parameters.parameter_fingerprint, &existing);
  if (found == LARDON3D_PROJECT_DB_OK) {
    Lardon3DFeatureReader *reader = NULL;
    Lardon3DFeatureFileMetadata metadata;
    Lardon3DFeatureStoreResult valid = lardon3d_feature_reader_open(
        context->project_path, &existing, &reader, &metadata);
    lardon3d_feature_reader_close(reader);
    return valid == LARDON3D_FEATURE_STORE_OK
               ? lardon3d_task_set_progress(task, 100, "Feature Set SIFT déjà présent.")
               : lardon3d_task_fail(task, "Feature Set SIFT existant corrompu.");
  }
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND)
    return lardon3d_task_fail(task, "Recherche Feature Store SIFT impossible.");
  struct timespec begin;
  (void)clock_gettime(CLOCK_MONOTONIC, &begin);
  char path[PATH_MAX];
  if (!source_path(context, path)) return lardon3d_task_fail(task, "Asset image corrompu.");
  Lardon3DSiftExtractorParameters parameters = extract_parameters(context);
  Lardon3DExtractedFeatures features;
  if (lardon3d_feature_extract_sift(path, &parameters, &features) != LARDON3D_FEATURE_EXTRACT_OK)
    return lardon3d_task_fail(task, "Extraction SIFT impossible.");
  if (!lardon3d_task_checkpoint(task)) {
    lardon3d_extracted_features_destroy(&features);
    return false;
  }
#ifdef LARDON3D_FEATURE_TASK_TESTING
  const char *pause = getenv("LARDON3D_TEST_SIFT_PAUSE_BEFORE_PUBLISH");
  if (pause && strcmp(pause, "1") == 0) {
    lardon3d_task_pause(task);
    if (!lardon3d_task_checkpoint(task)) {
      lardon3d_extracted_features_destroy(&features);
      return false;
    }
  }
#endif
  Lardon3DAppState state;
  runtime_state(context, &state);
  Lardon3DProjectDbFeatureSet set;
  uint32_t capabilities = LARDON3D_FEATURE_HAS_SCALE | LARDON3D_FEATURE_HAS_ORIENTATION |
                          LARDON3D_FEATURE_HAS_RESPONSE | LARDON3D_FEATURE_HAS_OCTAVE;
  Lardon3DFeatureStoreResult published = lardon3d_feature_store_publish_v2(
      &state, context->parameters.image_id, lardon3d_task_id(task),
      context->parameters.extractor_kind, 1, context->parameters.parameter_fingerprint,
      LARDON3D_FEATURE_DESCRIPTOR_F32, 128, capabilities, &features, &set);
  lardon3d_extracted_features_destroy(&features);
  if (published != LARDON3D_FEATURE_STORE_OK &&
      published != LARDON3D_FEATURE_STORE_ALREADY_PRESENT &&
      published != LARDON3D_FEATURE_STORE_PUBLISHED_NOT_DURABLE)
    return lardon3d_task_fail(task, "Publication SIFT impossible.");
  struct timespec end;
  (void)clock_gettime(CLOCK_MONOTONIC, &end);
  (void)lardon3d_resource_governor_record_batch(
      context->governor, LARDON3D_RESOURCE_TASK_CPU, 1, elapsed_ns(begin, end), 0);
  return lardon3d_task_set_progress(task, 100, "Feature Set SIFT publié.");
}

static void finished(const Lardon3DTask *task, void *userdata) {
#ifdef LARDON3D_FEATURE_TASK_TESTING
  const char *skip = getenv("LARDON3D_TEST_SIFT_SKIP_FINISHED_CHECKPOINT");
  if (skip && strcmp(skip, "1") == 0) return;
#endif
  SiftTaskContext *context = userdata;
  Lardon3DAppState state;
  runtime_state(context, &state);
  lardon3d_project_checkpoint_sift_extract_task(&state, task, &context->parameters);
}

static SiftTaskContext *make_context(const Lardon3DTaskReconstructionContext *runtime,
                                     const Lardon3DProjectDbSiftExtractTask *parameters) {
  if (!runtime || !runtime->project_path || !runtime->project_db || !runtime->resource_governor)
    return NULL;
  SiftTaskContext *context = calloc(1, sizeof(*context));
  if (!context) return NULL;
  int size = snprintf(context->project_path, sizeof(context->project_path), "%s",
                      runtime->project_path);
  if (size <= 0 || (size_t)size >= sizeof(context->project_path)) {
    free(context);
    return NULL;
  }
  context->database = runtime->project_db;
  context->governor = runtime->resource_governor;
  context->parameters = *parameters;
  return context;
}

bool lardon3d_sift_extract_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
                                       Lardon3DTaskKindBinding *binding) {
  Lardon3DTaskReconstructionContext *runtime = userdata;
  Lardon3DProjectDbSiftExtractTask parameters;
  if (!snapshot || !runtime || !binding ||
      lardon3d_project_db_load_sift_extract_task(runtime->project_db, snapshot->id, &parameters) !=
          LARDON3D_PROJECT_DB_OK)
    return false;
  Lardon3DSiftExtractorParameters configuration = {
      parameters.max_features, parameters.octave_layers, parameters.contrast_threshold,
      parameters.edge_threshold, parameters.sigma, parameters.grid_rows, parameters.grid_cols,
      parameters.max_features_per_cell, strcmp(parameters.extractor_kind, "rootsift") == 0};
  unsigned char fingerprint[32];
  lardon3d_sift_extractor_parameter_fingerprint(&configuration, fingerprint);
  if (memcmp(fingerprint, parameters.parameter_fingerprint, 32) != 0) return false;
  SiftTaskContext *context = make_context(runtime, &parameters);
  if (!context) return false;
  *binding = (Lardon3DTaskKindBinding){
      .callback = run,
      .userdata = context,
      .userdata_destroy = destroy_context,
      .finished_callback = finished,
      .finished_userdata = context,
  };
  return true;
}

Lardon3DTask *lardon3d_project_create_sift_extract_task(
    Lardon3DAppState *state, uint64_t image_id, const Lardon3DSiftExtractorParameters *parameters,
    uint64_t *task_id) {
  if (task_id) *task_id = 0;
  if (!state || !state->project_loaded || !state->project_db || !state->resource_governor ||
      !task_id || !lardon3d_sift_extractor_parameters_valid(parameters))
    return NULL;
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  if (lardon3d_project_db_load_image(state->project_db, image_id, &image, &asset) !=
      LARDON3D_PROJECT_DB_OK)
    return NULL;
  uint64_t id = 0;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK)
    return NULL;
  Lardon3DProjectDbSiftExtractTask durable = {
      .task_id = id,
      .image_id = image_id,
      .extractor_version = 1,
      .max_features = parameters->max_features,
      .octave_layers = parameters->octave_layers,
      .contrast_threshold = parameters->contrast_threshold,
      .edge_threshold = parameters->edge_threshold,
      .sigma = parameters->sigma,
      .grid_rows = parameters->grid_rows,
      .grid_cols = parameters->grid_cols,
      .max_features_per_cell = parameters->max_features_per_cell};
  snprintf(durable.extractor_kind, sizeof(durable.extractor_kind), "%s",
           parameters->rootsift ? LARDON3D_ROOTSIFT_EXTRACTOR_KIND : LARDON3D_SIFT_EXTRACTOR_KIND);
  lardon3d_sift_extractor_parameter_fingerprint(parameters, durable.parameter_fingerprint);
  Lardon3DTaskReconstructionContext runtime = {
      .project_path = state->project_path,
      .project_db = state->project_db,
      .resource_governor = state->resource_governor,
      .orb_vulkan_backend = state->orb_vulkan_backend,
  };
  SiftTaskContext *context = make_context(&runtime, &durable);
  if (!context) return NULL;
  Lardon3DResourceEstimate estimate = {.memory_fixed_bytes = 64ULL * 1024 * 1024,
                                       .memory_bytes_per_item = 1024ULL * 1024 * 1024,
                                       .minimum_batch_size = 1,
                                       .maximum_batch_size = 1,
                                       /* Keep the immutable estimate independent of Matcher's
                                        * temporary process-wide single-thread setting. Governor
                                        * reduction makes this ceiling equal the startup pool. */
                                       .desired_cpu_threads = 12,
                                       .desired_io_slots = 1,
                                       .task_class = LARDON3D_RESOURCE_TASK_CPU};
  const char *kind = parameters->rootsift ? LARDON3D_ROOTSIFT_EXTRACT_TASK_KIND
                                          : LARDON3D_SIFT_EXTRACT_TASK_KIND;
  Lardon3DTask *task = lardon3d_task_create_typed("Extraction SIFT", &estimate, kind, 1, run,
                                                  context, destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished, context) ||
      lardon3d_project_checkpoint_sift_extract_task(state, task, &durable) !=
          LARDON3D_PROJECT_TASK_CHECKPOINT_OK) {
    lardon3d_task_destroy(task);
    return NULL;
  }
  *task_id = id;
  return task;
}

bool lardon3d_project_enqueue_sift_extract(
    Lardon3DAppState *state, uint64_t image_id, const Lardon3DSiftExtractorParameters *parameters,
    uint64_t *task_id) {
  if (!state || !state->task_queue) return false;
  Lardon3DTask *task = lardon3d_project_create_sift_extract_task(state, image_id, parameters,
                                                                 task_id);
  if (!task) return false;
  if (!lardon3d_task_queue_add(state->task_queue, task, NULL)) {
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}
