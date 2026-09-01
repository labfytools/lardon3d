#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
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
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>
#include <lardon3d/image_catalog.h>
#include <lardon3d/project.h>
#include <lardon3d/precision_features.h>
#include <lardon3d/sift_task.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>

#include "../src/opencv_task_thread_control.h"
#include "../src/resource_governor_internal.h"
#include "../src/task_internal.h"

#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "Échec ligne %d: %s\n", __LINE__, #x);                                       \
      return false;                                                                                \
    }                                                                                              \
  } while (0)
static bool join_path(char out[PATH_MAX], const char *a, const char *b) {
  int n = snprintf(out, PATH_MAX, "%s/%s", a, b);
  return n > 0 && (size_t)n < PATH_MAX;
}
static bool remove_tree(const char *p) {
  struct stat s;
  if (lstat(p, &s) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(s.st_mode)) {
    return unlink(p) == 0;
  }
  DIR *d = opendir(p);
  if (!d) {
    return false;
  }
  bool ok = true;
  for (struct dirent *e = readdir(d); e; e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    char c[PATH_MAX];
    if (!join_path(c, p, e->d_name) || !remove_tree(c)) {
      ok = false;
    }
  }
  if (closedir(d) || rmdir(p)) {
    ok = false;
  }
  return ok;
}
static bool write_pgm(const char *p) {
  FILE *f = fopen(p, "wb");
  if (!f) {
    return false;
  }
  fprintf(f, "P5\n192 192\n255\n");
  for (unsigned y = 0; y < 192; y++) {
    for (unsigned x = 0; x < 192; x++) {
      unsigned base = ((((x / 12) ^ (y / 12)) & 1U) != 0U) ? 205U : 35U;
      unsigned texture = (x * 17U + y * 29U + (x * y) % 37U) % 43U;
      int dx = (int)x - 96;
      int dy = (int)y - 96;
      unsigned ring = (dx * dx + dy * dy > 32 * 32 && dx * dx + dy * dy < 42 * 42) ? 45U : 0U;
      unsigned char v = (unsigned char)(base + texture + ring > 255U
                                            ? 255U
                                            : base + texture + ring);
      if (fwrite(&v, 1, 1, f) != 1) {
        fclose(f);
        return false;
      }
    }
  }
  return fclose(f) == 0;
}
static bool write_uniform_pgm(const char *path) {
  FILE *file = fopen(path, "wb");
  if (!file) return false;
  bool ok = fprintf(file, "P5\n192 192\n255\n") > 0;
  unsigned char row[192];
  memset(row, 127, sizeof(row));
  for (size_t y = 0; ok && y < 192; ++y) ok = fwrite(row, 1, sizeof(row), file) == sizeof(row);
  return fclose(file) == 0 && ok;
}

static bool wait_state(Lardon3DTaskQueue *q, uint64_t id,
                       Lardon3DTaskState wanted,
                       Lardon3DTaskSnapshot *out);

static bool extracted_equal(const Lardon3DExtractedFeatures *left,
                            const Lardon3DExtractedFeatures *right) {
  if (!left || !right || left->image_width != right->image_width ||
      left->image_height != right->image_height ||
      left->feature_count != right->feature_count ||
      left->descriptor_bytes != right->descriptor_bytes ||
      memcmp(&left->quality, &right->quality, sizeof(left->quality)) != 0 ||
      (left->descriptor_bytes > 0 &&
       memcmp(left->descriptors, right->descriptors,
              left->descriptor_bytes) != 0)) {
    return false;
  }
  for (uint32_t index = 0; index < left->feature_count; ++index) {
    const Lardon3DFeatureKeypoint *a = &left->keypoints[index];
    const Lardon3DFeatureKeypoint *b = &right->keypoints[index];
    if (memcmp(&a->x, &b->x, sizeof(a->x)) != 0 ||
        memcmp(&a->y, &b->y, sizeof(a->y)) != 0 ||
        memcmp(&a->size, &b->size, sizeof(a->size)) != 0 ||
        memcmp(&a->angle_degrees, &b->angle_degrees,
               sizeof(a->angle_degrees)) != 0 ||
        memcmp(&a->response, &b->response, sizeof(a->response)) != 0 ||
        a->octave != b->octave) {
      return false;
    }
  }
  return true;
}

typedef enum {
  TEST_EXTRACT_ORB = 0,
  TEST_EXTRACT_SIFT = 1,
  TEST_EXTRACT_ROOTSIFT = 2,
  TEST_EXTRACT_COUNT = 3,
} TestExtractKind;

typedef struct {
  const char *path;
  unsigned int expected_threads;
  TestExtractKind kind;
  Lardon3DExtractedFeatures output;
} AdaptiveExtractWork;

static bool adaptive_extract_callback(Lardon3DTask *task, void *userdata) {
  AdaptiveExtractWork *work = userdata;
  Lardon3DOpenCvTaskThreadControl control;
  if (!lardon3d_opencv_task_threads_begin(task, INT_MAX, &control) ||
      lardon3d_feature_opencv_thread_count() != work->expected_threads) {
    return false;
  }
  Lardon3DFeatureExtractResult result;
  if (work->kind == TEST_EXTRACT_ORB) {
    const Lardon3DFeatureExtractorParameters parameters = {512, 4, 10};
    result = lardon3d_feature_extract_orb(work->path, &parameters,
                                         &work->output);
  } else {
    Lardon3DSiftExtractorParameters parameters =
        lardon3d_sift_precision_classic_v1(
            work->kind == TEST_EXTRACT_ROOTSIFT);
    parameters.max_features = 512;
    result = lardon3d_feature_extract_sift(work->path, &parameters,
                                          &work->output);
  }
  bool restored = lardon3d_opencv_task_threads_end(&control);
  return result == LARDON3D_FEATURE_EXTRACT_OK && restored &&
         lardon3d_task_set_progress(task, 100, "Extraction adaptative testée.");
}

static bool retry_failed_opencv_restoration(void) {
  unsigned int original = lardon3d_feature_opencv_thread_count();
  Lardon3DOpenCvTaskThreadControl control = {
      .previous = original,
      .restore_required = true,
  };
  unsigned int temporary = original == 1 ? 2 : 1;
  if (!lardon3d_feature_opencv_configure_threads(temporary)) return false;
  char failed_threads[32];
  int written = snprintf(failed_threads, sizeof(failed_threads), "%u", original);
  bool ok = written > 0 && (size_t)written < sizeof(failed_threads) &&
            setenv("LARDON3D_TEST_OPENCV_CONFIGURE_FAILURE_THREADS", failed_threads, 1) == 0;
  /* The injected failure occurs after setNumThreads(), so verification must
   * retain the restoration obligation even though the observed value changed. */
  ok = ok && !lardon3d_opencv_task_threads_end(&control) && control.restore_required;
  ok = unsetenv("LARDON3D_TEST_OPENCV_CONFIGURE_FAILURE_THREADS") == 0 && ok;
  ok = lardon3d_opencv_task_threads_end(&control) && !control.restore_required &&
       lardon3d_feature_opencv_thread_count() == original && ok;
  return ok;
}

static Lardon3DResourceSnapshot deterministic_resource_snapshot(double cpu_load_1m) {
  return (Lardon3DResourceSnapshot) {
      .memory_available_bytes = UINT64_C(12) * 1024 * 1024 * 1024,
      .memory_free_bytes = UINT64_C(8) * 1024 * 1024 * 1024,
      .cpu_load_1m = cpu_load_1m,
      .cpu_pressure_known = true,
      .cpu_pressure_avg10 = 0.0,
      .memory_pressure_known = true,
      .memory_pressure_avg10 = 0.0,
      .io_pressure_known = true,
      .io_pressure_avg10 = 0.0,
      .swap_activity_known = true,
      .swap_pages_in = 0,
      .swap_pages_out = 0,
  };
}

static bool deterministic_capture_preserves_load_gate(void) {
  Lardon3DHardwareProfile profile = {
      .logical_cpu_count = 5,
      .page_size_bytes = 4096,
      .memory_total_bytes = UINT64_C(16) * 1024 * 1024 * 1024,
      .cpu_architecture = "test",
  };
  Lardon3DResourcePolicy policy = {
      .system_cpu_reserve = 4,
      .maximum_cpu_load_ratio = 1.0,
      .maximum_io_pressure_avg10 = 100.0,
      .io_slot_capacity = 1,
  };
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  if (!governor) return false;
  Lardon3DResourceEstimate estimate = {
      .minimum_batch_size = 1,
      .maximum_batch_size = 1,
      .desired_cpu_threads = 1,
      .task_class = LARDON3D_RESOURCE_TASK_CPU,
  };
  Lardon3DResourceSnapshot snapshot = deterministic_resource_snapshot(5.0);
  Lardon3DResourceDecision decision;
  Lardon3DResourceReservation *reservation = NULL;
  /* Equality is intentional: production rejects load >= logical CPUs times
   * the configured ratio. The seam controls telemetry, not policy. */
  bool ok = lardon3d_resource_governor_internal_set_capture_snapshot(
          governor, &snapshot)
      && lardon3d_resource_governor_reserve_available(
          governor, &estimate, &decision, &reservation)
      && decision.kind == LARDON3D_RESOURCE_WAIT
      && reservation == NULL
      && strcmp(decision.reason, "Charge CPU trop élevée.") == 0;
  snapshot = deterministic_resource_snapshot(0.0);
  ok = lardon3d_resource_governor_internal_set_capture_snapshot(
          governor, &snapshot)
      && lardon3d_resource_governor_reserve_available(
          governor, &estimate, &decision, &reservation)
      && decision.kind == LARDON3D_RESOURCE_START
      && reservation != NULL
      && ok;
  if (reservation) {
    ok = lardon3d_resource_governor_release(governor, reservation) && ok;
  }
  ok = lardon3d_resource_governor_internal_set_capture_snapshot(
      governor, NULL) && ok;
  lardon3d_resource_governor_destroy(governor);
  return ok;
}

static bool run_adaptive_output_equivalence(const char *path) {
  cpu_set_t allowed_mask;
  CPU_ZERO(&allowed_mask);
  if (sched_getaffinity(0, sizeof(allowed_mask), &allowed_mask) != 0) {
    return true;
  }
  unsigned int allowed_ids[LARDON3D_RESOURCE_CPU_MAX];
  size_t allowed_count = 0;
  for (unsigned int cpu = 0; cpu < CPU_SETSIZE &&
       cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
    if (CPU_ISSET((size_t)cpu, &allowed_mask)) {
      allowed_ids[allowed_count++] = cpu;
    }
  }
  if (allowed_count == 0) return true;
  /* Counts above the historical host ceiling execute only when the process
   * affinity genuinely exposes them; unavailable CPUs are never fabricated. */
  const unsigned int requested[] = {1, 2, 4, 8, 12, 16, 32, 64};
  Lardon3DExtractedFeatures baseline[TEST_EXTRACT_COUNT] = {0};
  bool have_baseline[TEST_EXTRACT_COUNT] = {false};
  unsigned int previous_threads = lardon3d_feature_opencv_thread_count();
  bool ok = true;
  for (size_t request_index = 0;
       request_index < sizeof(requested) / sizeof(requested[0]) && ok;
       ++request_index) {
    unsigned int threads = requested[request_index];
    if (threads > allowed_count) continue;
    Lardon3DHardwareProfile profile = {
        .logical_cpu_count = threads + 4,
        .page_size_bytes = 4096,
        .memory_total_bytes = UINT64_C(16) * 1024 * 1024 * 1024,
        .cpu_architecture = "test",
    };
    Lardon3DResourcePolicy policy = {
        .system_cpu_reserve = 4,
        .maximum_cpu_load_ratio = 1.0,
        .maximum_io_pressure_avg10 = 100.0,
        .io_slot_capacity = 1,
    };
    Lardon3DResourceGovernor *governor =
        lardon3d_resource_governor_create(&profile, &policy);
    Lardon3DResourceCpuTopologyInput topology = {
        .affinity_available = true,
        .topology_available = false,
        .allowed_cpu_count = threads,
    };
    for (unsigned int index = 0; index < threads; ++index) {
      topology.allowed_cpu_ids[index] = allowed_ids[index];
    }
    Lardon3DResourceSnapshot resources = deterministic_resource_snapshot(0.0);
    Lardon3DTaskQueue *queue = NULL;
    if (!governor ||
        !lardon3d_resource_governor_internal_configure_cpu_topology(
            governor, &topology) ||
        !lardon3d_resource_governor_internal_set_capture_snapshot(
            governor, &resources) ||
        !(queue = lardon3d_task_queue_create(governor, TEST_EXTRACT_COUNT))) {
      lardon3d_task_queue_destroy(queue);
      lardon3d_resource_governor_destroy(governor);
      ok = false;
      break;
    }
    AdaptiveExtractWork work[TEST_EXTRACT_COUNT] = {0};
    uint64_t ids[TEST_EXTRACT_COUNT] = {0};
    Lardon3DResourceEstimate estimate = {
        .minimum_batch_size = 1,
        .maximum_batch_size = 1,
        .desired_cpu_threads = INT_MAX,
        .task_class = LARDON3D_RESOURCE_TASK_CPU,
    };
    static const char *const task_kinds[TEST_EXTRACT_COUNT] = {
        "features.extract",
        "features.extract.sift",
        "features.extract.rootsift",
    };
    for (size_t kind = 0; kind < TEST_EXTRACT_COUNT && ok; ++kind) {
      work[kind] = (AdaptiveExtractWork) {
          .path = path,
          .expected_threads = threads,
          .kind = (TestExtractKind)kind,
      };
      Lardon3DTask *task = lardon3d_task_create_typed(
          "Extraction adaptative", &estimate, task_kinds[kind], 1,
          adaptive_extract_callback, &work[kind], NULL);
      Lardon3DTaskCapabilityEnvelope envelope = {
          .count = 1,
          .capabilities = {{
              .estimate = estimate,
              .backend = LARDON3D_RESOURCE_BACKEND_FIXED,
              .inflight_limit = 1,
          }},
      };
      envelope.capabilities[0].estimate.desired_cpu_threads = threads;
      /* Scientific equality is tested at exact admitted counts independently
       * of feedback timing; Governor progression/deadband has its own fully
       * deterministic rate-injection regression. */
      ok = task && lardon3d_task_internal_set_capability_envelope(
          task, &envelope) && lardon3d_task_queue_add(queue, task, &ids[kind]);
      if (!ok && task) lardon3d_task_destroy(task);
    }
    Lardon3DTaskSnapshot snapshot;
    for (size_t kind = 0; kind < TEST_EXTRACT_COUNT && ok; ++kind) {
      ok = wait_state(queue, ids[kind], TASK_COMPLETED, &snapshot);
      if (!ok) break;
      if (!have_baseline[kind]) {
        baseline[kind] = work[kind].output;
        work[kind].output = (Lardon3DExtractedFeatures) {0};
        have_baseline[kind] = true;
      } else {
        ok = extracted_equal(&baseline[kind], &work[kind].output);
      }
    }
    for (size_t kind = 0; kind < TEST_EXTRACT_COUNT; ++kind) {
      lardon3d_extracted_features_destroy(&work[kind].output);
    }
    lardon3d_task_queue_destroy(queue);
    if (!lardon3d_resource_governor_internal_set_capture_snapshot(
            governor, NULL)) {
      ok = false;
    }
    lardon3d_resource_governor_destroy(governor);
  }
  for (size_t kind = 0; kind < TEST_EXTRACT_COUNT; ++kind) {
    lardon3d_extracted_features_destroy(&baseline[kind]);
  }
  return lardon3d_feature_opencv_configure_threads(previous_threads) && ok;
}
static bool runtime(Lardon3DAppState *s) {
  s->hardware_profile = (Lardon3DHardwareProfile){.logical_cpu_count = 64,
                                                  .page_size_bytes = 4096,
                                                  .memory_total_bytes = UINT64_MAX,
                                                  .cpu_architecture = "test"};
  Lardon3DResourcePolicy p = {
      .maximum_cpu_load_ratio = 1, .maximum_io_pressure_avg10 = 100, .io_slot_capacity = 2};
  s->resource_governor = lardon3d_resource_governor_create(&s->hardware_profile, &p);
  if (!s->resource_governor) return false;
  Lardon3DResourceSnapshot resources = deterministic_resource_snapshot(0.0);
  if (!lardon3d_resource_governor_internal_set_capture_snapshot(
          s->resource_governor, &resources)) {
    lardon3d_resource_governor_destroy(s->resource_governor);
    s->resource_governor = NULL;
    s->task_queue = NULL;
    return false;
  }
  s->task_queue = lardon3d_task_queue_create(s->resource_governor, 4);
  if (!s->task_queue) {
    lardon3d_resource_governor_destroy(s->resource_governor);
    s->resource_governor = NULL;
    return false;
  }
  return true;
}
static bool wait_state(Lardon3DTaskQueue *q, uint64_t id, Lardon3DTaskState wanted,
                       Lardon3DTaskSnapshot *out) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return false;
  /* The equality fixture executes every genuinely available requested CPU
   * contract. Instrumented OpenCV is slower than the normal build; keep the
   * wait bounded without mistaking sanitizer overhead for scientific drift. */
  deadline.tv_sec += 30;
  for (;;) {
    if (lardon3d_task_queue_get(q, id, out) && out->state == wanted) {
      return true;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
      return false;
    }
    sched_yield();
  }
}

static bool last_sequence_is_no_work(Lardon3DAppState *state, const char *task_kind) {
  Lardon3DResourceSequenceDiagnostic diagnostic;
  return lardon3d_resource_governor_internal_last_diagnostic(
             state->resource_governor, task_kind, 1, &diagnostic) &&
         diagnostic.items_completed == 0 &&
         diagnostic.durable_items_per_second_milli == 0 &&
         diagnostic.cpu_threads == 1 &&
         strcmp(diagnostic.reason, "throughput-no-work") == 0;
}

typedef struct {
  Lardon3DAppState *state;
  uint64_t orb_feature_set_id;
  uint64_t sift_feature_set_id;
  Lardon3DFeatureConsolidationParameters parameters;
  Lardon3DProjectDbFeatureSupportSet support;
  Lardon3DProjectDbResult result;
} ConsolidationThreadContext;

static void *consolidate_thread(void *userdata) {
  ConsolidationThreadContext *context = userdata;
  context->result = lardon3d_consolidate_orb_sift(
      context->state, context->orb_feature_set_id, context->sift_feature_set_id,
      &context->parameters, &context->support);
  return NULL;
}

static bool run_test(void) {
  char root[] = "/tmp/lardon3d-feature-task-XXXXXX";
  CHECK(deterministic_capture_preserves_load_gate());
  CHECK(mkdtemp(root) && setenv("LARDON3D_PROJECTS_ROOT", root, 1) == 0);
  char source[PATH_MAX];
  CHECK(join_path(source, root, "source.pgm") && write_pgm(source));
  CHECK(run_adaptive_output_equivalence(source));
  CHECK(retry_failed_opencv_restoration());
  Lardon3DAppState state;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_create(&state, "Features"));
  CHECK(lardon3d_feature_opencv_configure_threads(3));
  Lardon3DProjectDbScanSet scanset;
  CHECK(lardon3d_image_catalog_create_scanset(&state, "A", &scanset));
  Lardon3DProjectDbImage image;
  Lardon3DProjectDbImageAsset asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, source, 0, &image, &asset) ==
        LARDON3D_IMAGE_CATALOG_IMPORTED);
  Lardon3DFeatureExtractorParameters parameters = {512, 4, 10};
  CHECK(setenv("LARDON3D_TEST_FEATURE_PAUSE_BEFORE_PUBLISH", "1", 1) == 0 &&
        setenv("LARDON3D_TEST_FEATURE_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(&state, image.image_id, &parameters, &task_id));
  Lardon3DTaskSnapshot snapshot;
  CHECK(wait_state(state.task_queue, task_id, TASK_PAUSED, &snapshot));
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  CHECK(lardon3d_feature_opencv_thread_count() == 3);
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  CHECK(unsetenv("LARDON3D_TEST_FEATURE_PAUSE_BEFORE_PUBLISH") == 0 &&
        unsetenv("LARDON3D_TEST_FEATURE_SKIP_FINISHED_CHECKPOINT") == 0);
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  CHECK(lardon3d_feature_opencv_configure_threads(3));
  Lardon3DProjectRecoverySummary summary;
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 1);
  CHECK(wait_state(state.task_queue, task_id, TASK_COMPLETED, &snapshot) &&
        snapshot.progress == 100);
  CHECK(lardon3d_feature_opencv_thread_count() == 3);
  unsigned char fp[32];
  lardon3d_feature_extractor_parameter_fingerprint(&parameters, fp);
  Lardon3DProjectDbFeatureSet set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id,
                                             LARDON3D_FEATURE_EXTRACTOR_KIND, 1, fp,
                                             &set) == LARDON3D_PROJECT_DB_OK);
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 0);
  CHECK(lardon3d_project_db_load_feature_set(state.project_db, set.feature_set_id, &set) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(unlink(source) == 0);
  Lardon3DFeatureReader *reader = NULL;
  Lardon3DFeatureFileMetadata metadata;
  CHECK(lardon3d_feature_reader_open(state.project_path, &set, &reader, &metadata) ==
        LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);

  uint64_t duplicate_orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &parameters, &duplicate_orb_task) &&
        wait_state(state.task_queue, duplicate_orb_task, TASK_COMPLETED, &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_FEATURE_EXTRACT_TASK_KIND));

  /* The asset and DB row are visible, but forced directory-sync uncertainty
   * means this extraction is not durable throughput evidence. */
  Lardon3DFeatureExtractorParameters nondurable_orb_parameters = {508, 4, 10};
  uint64_t nondurable_orb_task = 0;
  CHECK(setenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC", "1", 1) == 0 &&
        lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &nondurable_orb_parameters,
            &nondurable_orb_task) &&
        wait_state(state.task_queue, nondurable_orb_task, TASK_COMPLETED, &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_FEATURE_EXTRACT_TASK_KIND) &&
        unsetenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC") == 0);
  uint64_t post_no_work_orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &parameters, &post_no_work_orb_task) &&
        wait_state(state.task_queue, post_no_work_orb_task, TASK_COMPLETED,
                   &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_FEATURE_EXTRACT_TASK_KIND));

  /* Configuration failure is injected after OpenCV mutates its process-wide
   * pool. Feature, SIFT, and RootSIFT must each restore the captured value. */
  CHECK(lardon3d_feature_opencv_configure_threads(3));
  CHECK(setenv("LARDON3D_TEST_OPENCV_CONFIGURE_FAILURE_THREADS", "1", 1) == 0);
  Lardon3DFeatureExtractorParameters failed_orb_parameters = {509, 4, 10};
  uint64_t failed_orb_task = 0;
  CHECK(lardon3d_project_enqueue_feature_extract(
            &state, image.image_id, &failed_orb_parameters, &failed_orb_task) &&
        wait_state(state.task_queue, failed_orb_task, TASK_FAILED, &snapshot) &&
        lardon3d_feature_opencv_thread_count() == 3);

  Lardon3DSiftExtractorParameters failed_sift_parameters =
      lardon3d_sift_precision_classic_v1(false);
  failed_sift_parameters.max_features = 511;
  uint64_t failed_sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &failed_sift_parameters,
            &failed_sift_task) &&
        wait_state(state.task_queue, failed_sift_task, TASK_FAILED, &snapshot) &&
        lardon3d_feature_opencv_thread_count() == 3);
  failed_sift_parameters.rootsift = true;
  failed_sift_parameters.max_features = 510;
  uint64_t failed_rootsift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &failed_sift_parameters,
            &failed_rootsift_task) &&
        wait_state(state.task_queue, failed_rootsift_task, TASK_FAILED, &snapshot) &&
        lardon3d_feature_opencv_thread_count() == 3 &&
        unsetenv("LARDON3D_TEST_OPENCV_CONFIGURE_FAILURE_THREADS") == 0);

  Lardon3DSiftExtractorParameters sift_parameters = lardon3d_sift_precision_classic_v1(false);
  sift_parameters.max_features = 512;
  uint64_t sift_task_id = 0, concurrent_identical_sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id, &sift_parameters,
                                              &sift_task_id) &&
        lardon3d_project_enqueue_sift_extract(&state, image.image_id, &sift_parameters,
                                              &concurrent_identical_sift_task));
  CHECK(sift_task_id != concurrent_identical_sift_task &&
        wait_state(state.task_queue, sift_task_id, TASK_COMPLETED, &snapshot) &&
        wait_state(state.task_queue, concurrent_identical_sift_task, TASK_COMPLETED, &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_SIFT_EXTRACT_TASK_KIND));
  unsigned char sift_fingerprint[32];
  lardon3d_sift_extractor_parameter_fingerprint(&sift_parameters, sift_fingerprint);
  unsigned char same_sift_fingerprint[32], changed_sift_fingerprint[32];
  Lardon3DSiftExtractorParameters changed_sift_parameters = sift_parameters;
  changed_sift_parameters.sigma = 1.7;
  lardon3d_sift_extractor_parameter_fingerprint(&sift_parameters, same_sift_fingerprint);
  lardon3d_sift_extractor_parameter_fingerprint(&changed_sift_parameters,
                                                changed_sift_fingerprint);
  CHECK(memcmp(sift_fingerprint, same_sift_fingerprint, 32) == 0 &&
        memcmp(sift_fingerprint, changed_sift_fingerprint, 32) != 0);
  Lardon3DSiftExtractorParameters fingerprint_variants[8];
  for (size_t variant = 0; variant < 8; ++variant)
    fingerprint_variants[variant] = sift_parameters;
  fingerprint_variants[0].max_features--;
  fingerprint_variants[1].octave_layers++;
  fingerprint_variants[2].contrast_threshold += 0.001;
  fingerprint_variants[3].edge_threshold += 1.0;
  fingerprint_variants[4].grid_rows--;
  fingerprint_variants[5].grid_cols--;
  fingerprint_variants[6].max_features_per_cell--;
  fingerprint_variants[7].rootsift = true;
  for (size_t variant = 0; variant < 8; ++variant) {
    lardon3d_sift_extractor_parameter_fingerprint(&fingerprint_variants[variant],
                                                  changed_sift_fingerprint);
    CHECK(memcmp(sift_fingerprint, changed_sift_fingerprint, 32) != 0);
  }
  Lardon3DProjectDbFeatureSet sift_set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id, "sift", 1,
                                             sift_fingerprint, &sift_set) ==
            LARDON3D_PROJECT_DB_OK &&
        sift_set.total_cells == 64 && sift_set.occupied_cells > 0 &&
        sift_set.coverage_ratio > 0.0 && sift_set.coverage_ratio <= 1.0 &&
        sift_set.feature_density_per_megapixel > 0.0);
  Lardon3DVisualIndexConfiguration incompatible_index_configuration = {1, 256, 128};
  uint64_t incompatible_visual_index = 0;
  CHECK(lardon3d_visual_index_create(state.project_db, &sift_set,
                                     &incompatible_index_configuration,
                                     &incompatible_visual_index) ==
            LARDON3D_VISUAL_INDEX_INVALID_ARGUMENT &&
        incompatible_visual_index == 0);
  CHECK(lardon3d_feature_reader_open(state.project_path, &sift_set, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK &&
        metadata.format_version == 2 && metadata.descriptor_dimension == 128 &&
        metadata.descriptor_type == LARDON3D_FEATURE_DESCRIPTOR_F32);
  float sift_descriptors[128];
  if (metadata.feature_count > 0) {
    CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, sift_descriptors, 1,
                                                  sizeof(sift_descriptors)) ==
          LARDON3D_FEATURE_STORE_OK);
  }
  lardon3d_feature_reader_close(reader);
  uint64_t duplicate_sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id, &sift_parameters,
                                              &duplicate_sift_task));
  CHECK(wait_state(state.task_queue, duplicate_sift_task, TASK_COMPLETED, &snapshot));
  Lardon3DProjectDbFeatureSet duplicate_sift_set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id, "sift", 1,
                                             sift_fingerprint, &duplicate_sift_set) ==
            LARDON3D_PROJECT_DB_OK &&
        duplicate_sift_set.feature_set_id == sift_set.feature_set_id);
  Lardon3DSiftExtractorParameters rootsift_parameters = sift_parameters;
  rootsift_parameters.rootsift = true;
  uint64_t rootsift_task_id = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id, &rootsift_parameters,
                                              &rootsift_task_id));
  CHECK(wait_state(state.task_queue, rootsift_task_id, TASK_COMPLETED, &snapshot));
  unsigned char rootsift_fingerprint[32];
  lardon3d_sift_extractor_parameter_fingerprint(&rootsift_parameters, rootsift_fingerprint);
  Lardon3DProjectDbFeatureSet rootsift_set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id, "rootsift", 1,
                                             rootsift_fingerprint, &rootsift_set) ==
            LARDON3D_PROJECT_DB_OK &&
        rootsift_set.feature_set_id != sift_set.feature_set_id);
  uint64_t duplicate_rootsift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &rootsift_parameters,
            &duplicate_rootsift_task) &&
        wait_state(state.task_queue, duplicate_rootsift_task, TASK_COMPLETED, &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_ROOTSIFT_EXTRACT_TASK_KIND));

  Lardon3DSiftExtractorParameters nondurable_sift_parameters = sift_parameters;
  nondurable_sift_parameters.max_features = 508;
  uint64_t nondurable_sift_task = 0;
  CHECK(setenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC", "1", 1) == 0 &&
        lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &nondurable_sift_parameters,
            &nondurable_sift_task) &&
        wait_state(state.task_queue, nondurable_sift_task, TASK_COMPLETED, &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_SIFT_EXTRACT_TASK_KIND));
  nondurable_sift_parameters.rootsift = true;
  nondurable_sift_parameters.max_features = 509;
  uint64_t nondurable_rootsift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(
            &state, image.image_id, &nondurable_sift_parameters,
            &nondurable_rootsift_task) &&
        wait_state(state.task_queue, nondurable_rootsift_task, TASK_COMPLETED, &snapshot) &&
        last_sequence_is_no_work(&state, LARDON3D_ROOTSIFT_EXTRACT_TASK_KIND) &&
        unsetenv("LARDON3D_TEST_FEATURE_FAIL_DIRECTORY_SYNC") == 0);
  /* SIFT and RootSIFT CPU12/CPU1 checkpoints are exact historical operational
   * signatures. Recovery uses the portable OpenCV ceiling in memory without publishing an
   * estimate-only checkpoint; a neighboring CPU2 shape is corruption. */
  const uint64_t precision_task_ids[2] = {sift_task_id, rootsift_task_id};
  const char *precision_kinds[2] = {LARDON3D_SIFT_EXTRACT_TASK_KIND,
                                    LARDON3D_ROOTSIFT_EXTRACT_TASK_KIND};
  Lardon3DTaskReconstructionContext precision_reconstruction = {
      .project_path = state.project_path,
      .project_db = state.project_db,
      .resource_governor = state.resource_governor,
      .orb_vulkan_backend = state.orb_vulkan_backend,
  };
  char orb_checkpoint[PATH_MAX];
  CHECK(snprintf(orb_checkpoint, sizeof(orb_checkpoint),
                 "%s/.lardon3d/checkpoints/%lu.chk", state.project_path,
                 (unsigned long)task_id) > 0);
  Lardon3DTaskDurableSnapshot current_orb;
  CHECK(lardon3d_task_checkpoint_load(orb_checkpoint, &current_orb, NULL) ==
            LARDON3D_TASK_CHECKPOINT_OK &&
        current_orb.estimate.desired_cpu_threads == INT_MAX);
  const unsigned int legacy_orb_counts[] = {12, 1};
  for (size_t legacy = 0;
       legacy < sizeof(legacy_orb_counts) / sizeof(legacy_orb_counts[0]);
       ++legacy) {
    Lardon3DTaskDurableSnapshot historical_orb = current_orb;
    historical_orb.estimate.desired_cpu_threads = legacy_orb_counts[legacy];
    historical_orb.progress = 50;
    historical_orb.saved_state = TASK_RUNNING;
    historical_orb.recovery_state = TASK_PENDING;
    historical_orb.finished_at = (struct timespec){0};
    Lardon3DTask *restored_orb = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              LARDON3D_FEATURE_EXTRACT_TASK_KIND,
              LARDON3D_FEATURE_EXTRACT_TASK_KIND_VERSION, &historical_orb,
              &precision_reconstruction, &restored_orb) ==
              LARDON3D_TASK_KIND_OK &&
          restored_orb);
    Lardon3DResourceEstimate effective_orb;
    CHECK(lardon3d_task_resource_estimate(restored_orb, &effective_orb) &&
          effective_orb.desired_cpu_threads == INT_MAX);
    lardon3d_task_destroy(restored_orb);
  }
  Lardon3DTaskDurableSnapshot malformed_orb = current_orb;
  malformed_orb.estimate.desired_cpu_threads = 2;
  Lardon3DTask *restored_orb = NULL;
  CHECK(lardon3d_task_kind_registry_restore(
            lardon3d_task_kind_registry_production(),
            LARDON3D_FEATURE_EXTRACT_TASK_KIND,
            LARDON3D_FEATURE_EXTRACT_TASK_KIND_VERSION, &malformed_orb,
            &precision_reconstruction, &restored_orb) ==
            LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED &&
        restored_orb == NULL);
  Lardon3DTaskDurableSnapshot still_current_orb;
  CHECK(lardon3d_task_checkpoint_load(orb_checkpoint, &still_current_orb,
                                      NULL) ==
            LARDON3D_TASK_CHECKPOINT_OK &&
        still_current_orb.estimate.desired_cpu_threads == INT_MAX);

  for (size_t precision_index = 0; precision_index < 2; ++precision_index) {
    char precision_checkpoint[PATH_MAX];
    char precision_staged[PATH_MAX];
    CHECK(snprintf(precision_checkpoint, sizeof(precision_checkpoint),
                   "%s/.lardon3d/checkpoints/%lu.chk", state.project_path,
                   (unsigned long)precision_task_ids[precision_index]) > 0 &&
          snprintf(precision_staged, sizeof(precision_staged), "%s.next",
                   precision_checkpoint) > 0);
    Lardon3DTaskDurableSnapshot current_precision;
    CHECK(lardon3d_task_checkpoint_load(precision_checkpoint,
                                        &current_precision, NULL) ==
          LARDON3D_TASK_CHECKPOINT_OK);
    Lardon3DTaskDurableSnapshot cpu12_precision = current_precision;
    cpu12_precision.estimate.desired_cpu_threads = 12;
    cpu12_precision.progress = 50;
    cpu12_precision.saved_state = TASK_RUNNING;
    cpu12_precision.recovery_state = TASK_PENDING;
    cpu12_precision.finished_at = (struct timespec){0};
    Lardon3DTask *cpu12_restored = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              precision_kinds[precision_index],
              LARDON3D_SIFT_EXTRACT_TASK_KIND_VERSION, &cpu12_precision,
              &precision_reconstruction, &cpu12_restored) ==
              LARDON3D_TASK_KIND_OK &&
          cpu12_restored);
    Lardon3DResourceEstimate cpu12_effective;
    CHECK(lardon3d_task_resource_estimate(cpu12_restored,
                                          &cpu12_effective) &&
          cpu12_effective.desired_cpu_threads == INT_MAX);
    lardon3d_task_destroy(cpu12_restored);
    Lardon3DTaskDurableSnapshot historical_precision = current_precision;
    historical_precision.estimate.desired_cpu_threads = 1;
    historical_precision.progress = 50;
    historical_precision.saved_state = TASK_RUNNING;
    historical_precision.recovery_state = TASK_PENDING;
    historical_precision.finished_at = (struct timespec){0};
    CHECK(lardon3d_task_checkpoint_save(precision_checkpoint,
                                        &historical_precision) ==
          LARDON3D_TASK_CHECKPOINT_OK);
    Lardon3DTask *restored_precision = NULL;
    Lardon3DTaskKindResult precision_restore =
        lardon3d_task_kind_registry_restore(
            lardon3d_task_kind_registry_production(),
            precision_kinds[precision_index],
            LARDON3D_SIFT_EXTRACT_TASK_KIND_VERSION,
            &historical_precision, &precision_reconstruction,
            &restored_precision);
    if (precision_restore != LARDON3D_TASK_KIND_OK) {
      fprintf(stderr, "precision restore %zu failed: %d\n", precision_index,
              (int)precision_restore);
    }
    CHECK(precision_restore == LARDON3D_TASK_KIND_OK &&
          restored_precision);
    Lardon3DResourceEstimate effective_precision;
    CHECK(lardon3d_task_resource_estimate(restored_precision,
                                          &effective_precision) &&
          effective_precision.desired_cpu_threads == INT_MAX);
    lardon3d_task_destroy(restored_precision);
    Lardon3DTaskDurableSnapshot durable_precision;
    CHECK(lardon3d_task_checkpoint_load(precision_checkpoint,
                                        &durable_precision, NULL) ==
              LARDON3D_TASK_CHECKPOINT_OK &&
          durable_precision.estimate.desired_cpu_threads == 1 &&
          access(precision_staged, F_OK) != 0 && errno == ENOENT);
    Lardon3DTaskDurableSnapshot malformed_precision = historical_precision;
    malformed_precision.estimate.desired_cpu_threads = 2;
    restored_precision = NULL;
    CHECK(lardon3d_task_kind_registry_restore(
              lardon3d_task_kind_registry_production(),
              precision_kinds[precision_index],
              LARDON3D_SIFT_EXTRACT_TASK_KIND_VERSION,
              &malformed_precision, &precision_reconstruction,
              &restored_precision) ==
              LARDON3D_TASK_KIND_RECONSTRUCTION_FAILED &&
          restored_precision == NULL);
  }
  CHECK(lardon3d_feature_reader_open(state.project_path, &rootsift_set, &reader, &metadata) ==
        LARDON3D_FEATURE_STORE_OK);
  if (metadata.feature_count > 0) {
    float root_descriptor[128];
    CHECK(lardon3d_feature_reader_descriptors_f32(reader, 0, root_descriptor, 1,
                                                  sizeof(root_descriptor)) ==
          LARDON3D_FEATURE_STORE_OK);
    double squared_norm = 0.0;
    for (size_t dimension = 0; dimension < 128; ++dimension) {
      CHECK(isfinite(root_descriptor[dimension]) && root_descriptor[dimension] >= 0.0F);
      squared_norm += (double)root_descriptor[dimension] * root_descriptor[dimension];
    }
    CHECK(squared_norm == 0.0 || (squared_norm > 0.999 && squared_norm < 1.001));
  }
  lardon3d_feature_reader_close(reader);
  Lardon3DFeatureConsolidationParameters support_parameters =
      lardon3d_feature_consolidation_v1();
  ConsolidationThreadContext support_contexts[2] = {
      {.state = &state,
       .orb_feature_set_id = set.feature_set_id,
       .sift_feature_set_id = sift_set.feature_set_id,
       .parameters = support_parameters},
      {.state = &state,
       .orb_feature_set_id = set.feature_set_id,
       .sift_feature_set_id = sift_set.feature_set_id,
       .parameters = support_parameters}};
  pthread_t support_threads[2];
  CHECK(pthread_create(&support_threads[0], NULL, consolidate_thread, &support_contexts[0]) == 0 &&
        pthread_create(&support_threads[1], NULL, consolidate_thread, &support_contexts[1]) == 0 &&
        pthread_join(support_threads[0], NULL) == 0 && pthread_join(support_threads[1], NULL) == 0);
  Lardon3DProjectDbFeatureSupportSet support = support_contexts[0].support;
  CHECK(support_contexts[0].result == LARDON3D_PROJECT_DB_OK &&
        support_contexts[1].result == LARDON3D_PROJECT_DB_OK && support.group_count > 0 &&
        support.group_count <= set.feature_count + sift_set.feature_count &&
        support_contexts[1].support.feature_support_set_id == support.feature_support_set_id &&
        support_contexts[1].support.group_count == support.group_count);
  Lardon3DProjectDbFeatureSupportGroup support_page[256];
  uint64_t after_support_group = 0;
  uint32_t listed_support_groups = 0;
  bool has_confirmed_support = false;
  do {
    size_t support_page_count = 0;
    CHECK(lardon3d_project_db_list_feature_support_groups(
              state.project_db, support.feature_support_set_id, after_support_group,
              support_page, 256, &support_page_count) == LARDON3D_PROJECT_DB_OK);
    if (support_page_count == 0) break;
    for (size_t group = 0; group < support_page_count; ++group) {
      CHECK(support_page[group].feature_support_set_id == support.feature_support_set_id &&
            support_page[group].feature_support_group_id > after_support_group &&
            support_page[group].support_count >= 1 && support_page[group].support_count <= 2 &&
            support_page[group].distance_pixels >= 0.0 &&
            support_page[group].distance_pixels <= support_parameters.association_radius_pixels);
      has_confirmed_support |= support_page[group].support_count == 2;
      after_support_group = support_page[group].feature_support_group_id;
    }
    listed_support_groups += (uint32_t)support_page_count;
  } while (listed_support_groups < support.group_count);
  CHECK(listed_support_groups == support.group_count && has_confirmed_support);
  Lardon3DProjectDbFeatureSupportGroup isolated_rank = {.feature_support_group_id = 1,
                                                        .support_count = 1};
  Lardon3DProjectDbFeatureSupportGroup confirmed_rank = {.feature_support_group_id = 2,
                                                         .support_count = 2};
  CHECK(lardon3d_feature_support_higher_quality(&confirmed_rank, &isolated_rank) &&
        !lardon3d_feature_support_higher_quality(&isolated_rank, &confirmed_rank));
  Lardon3DSiftExtractorParameters cancelled_parameters = sift_parameters;
  cancelled_parameters.max_features = 499;
  unsigned char cancelled_fingerprint[32];
  lardon3d_sift_extractor_parameter_fingerprint(&cancelled_parameters, cancelled_fingerprint);
  CHECK(setenv("LARDON3D_TEST_SIFT_PAUSE_BEFORE_PUBLISH", "1", 1) == 0);
  uint64_t cancelled_sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id, &cancelled_parameters,
                                              &cancelled_sift_task) &&
        wait_state(state.task_queue, cancelled_sift_task, TASK_PAUSED, &snapshot) &&
        lardon3d_task_queue_cancel(state.task_queue, cancelled_sift_task) &&
        wait_state(state.task_queue, cancelled_sift_task, TASK_CANCELLED, &snapshot) &&
        unsetenv("LARDON3D_TEST_SIFT_PAUSE_BEFORE_PUBLISH") == 0);
  Lardon3DProjectDbFeatureSet cancelled_set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id, "sift", 1,
                                             cancelled_fingerprint, &cancelled_set) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);
  char uniform_source[PATH_MAX];
  CHECK(join_path(uniform_source, root, "uniform.pgm") && write_uniform_pgm(uniform_source));
  Lardon3DProjectDbImage uniform_image;
  Lardon3DProjectDbImageAsset uniform_asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, uniform_source, 0,
                                           &uniform_image, &uniform_asset) ==
        LARDON3D_IMAGE_CATALOG_IMPORTED);
  uint64_t uniform_sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, uniform_image.image_id, &sift_parameters,
                                              &uniform_sift_task) &&
        wait_state(state.task_queue, uniform_sift_task, TASK_COMPLETED, &snapshot));
  Lardon3DProjectDbFeatureSet uniform_sift_set;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, uniform_image.image_id, "sift", 1,
                                             sift_fingerprint, &uniform_sift_set) ==
            LARDON3D_PROJECT_DB_OK &&
        uniform_sift_set.feature_count == 0 && unlink(uniform_source) == 0);
  Lardon3DSiftExtractorParameters recovery_parameters = sift_parameters;
  recovery_parameters.max_features = 500;
  CHECK(setenv("LARDON3D_TEST_SIFT_PAUSE_BEFORE_PUBLISH", "1", 1) == 0 &&
        setenv("LARDON3D_TEST_SIFT_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t recovery_sift_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id, &recovery_parameters,
                                              &recovery_sift_task));
  CHECK(wait_state(state.task_queue, recovery_sift_task, TASK_PAUSED, &snapshot));
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  CHECK(unsetenv("LARDON3D_TEST_SIFT_PAUSE_BEFORE_PUBLISH") == 0 &&
        unsetenv("LARDON3D_TEST_SIFT_SKIP_FINISHED_CHECKPOINT") == 0);
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 1);
  CHECK(wait_state(state.task_queue, recovery_sift_task, TASK_COMPLETED, &snapshot));
  unsigned char recovery_fingerprint[32];
  lardon3d_sift_extractor_parameter_fingerprint(&recovery_parameters, recovery_fingerprint);
  Lardon3DProjectDbFeatureSet recovered_sift;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, image.image_id, "sift", 1,
                                             recovery_fingerprint, &recovered_sift) ==
        LARDON3D_PROJECT_DB_OK);

  char second_source[PATH_MAX];
  CHECK(join_path(second_source, root, "second-source.pgm") && write_pgm(second_source));
  FILE *second_file = fopen(second_source, "ab");
  CHECK(second_file && fwrite("x", 1, 1, second_file) == 1 && fclose(second_file) == 0);
  Lardon3DProjectDbImage second_image;
  Lardon3DProjectDbImageAsset second_asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, second_source, 0,
                                           &second_image, &second_asset) ==
            LARDON3D_IMAGE_CATALOG_IMPORTED &&
        second_image.image_id != image.image_id);
  Lardon3DSiftExtractorParameters first_parallel = sift_parameters;
  Lardon3DSiftExtractorParameters second_parallel = sift_parameters;
  first_parallel.max_features = 496;
  second_parallel.max_features = 497;
  uint64_t first_parallel_task = 0, second_parallel_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, image.image_id, &first_parallel,
                                              &first_parallel_task) &&
        lardon3d_project_enqueue_sift_extract(&state, second_image.image_id, &second_parallel,
                                              &second_parallel_task) &&
        wait_state(state.task_queue, first_parallel_task, TASK_COMPLETED, &snapshot) &&
        wait_state(state.task_queue, second_parallel_task, TASK_COMPLETED, &snapshot));
  unsigned char second_parallel_fingerprint[32];
  lardon3d_sift_extractor_parameter_fingerprint(&second_parallel,
                                                second_parallel_fingerprint);
  Lardon3DProjectDbFeatureSet second_sift;
  CHECK(lardon3d_project_db_find_feature_set(state.project_db, second_image.image_id, "sift", 1,
                                             second_parallel_fingerprint, &second_sift) ==
            LARDON3D_PROJECT_DB_OK &&
        unlink(second_source) == 0 &&
        lardon3d_feature_reader_open(state.project_path, &second_sift, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);

  Lardon3DVisualIndexConfiguration index_configuration = {1, 256, 128};
  Lardon3DVisualIndexConfiguration nondurable_index_configuration = {1, 255, 128};
  uint64_t nondurable_visual_index_id = 0;
  uint64_t nondurable_visual_task_id = 0;
  CHECK(lardon3d_visual_index_create(
            state.project_db, &set, &nondurable_index_configuration,
            &nondurable_visual_index_id) == LARDON3D_VISUAL_INDEX_OK);
  CHECK(setenv("LARDON3D_TEST_VISUAL_INDEX_FAIL_DIR_FSYNC", "1", 1) == 0 &&
        setenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT", "1", 1) == 0);
  CHECK(lardon3d_project_enqueue_visual_index_update(
      &state, nondurable_visual_index_id, &nondurable_visual_task_id));
  CHECK(wait_state(state.task_queue, nondurable_visual_task_id, TASK_PAUSED,
                   &snapshot));
  CHECK(last_sequence_is_no_work(
      &state, LARDON3D_VISUAL_INDEX_UPDATE_TASK_KIND));
  CHECK(unsetenv("LARDON3D_TEST_VISUAL_INDEX_FAIL_DIR_FSYNC") == 0 &&
        unsetenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT") == 0 &&
        lardon3d_task_queue_resume(
            state.task_queue, nondurable_visual_task_id) &&
        wait_state(state.task_queue, nondurable_visual_task_id, TASK_COMPLETED,
                   &snapshot));
  uint64_t visual_index_id = 0;
  CHECK(lardon3d_visual_index_create(state.project_db, &set, &index_configuration,
                                     &visual_index_id) == LARDON3D_VISUAL_INDEX_OK);
  CHECK(setenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT", "1", 1) == 0 &&
        setenv("LARDON3D_TEST_VISUAL_INDEX_SKIP_FINISHED_CHECKPOINT", "1", 1) == 0);
  uint64_t visual_task_id = 0;
  CHECK(lardon3d_project_enqueue_visual_index_update(&state, visual_index_id, &visual_task_id));
  CHECK(wait_state(state.task_queue, visual_task_id, TASK_PAUSED, &snapshot));
  char visual_checkpoint[PATH_MAX];
  Lardon3DTaskDurableSnapshot historical_visual;
  CHECK(snprintf(visual_checkpoint, sizeof(visual_checkpoint),
                 "%s/.lardon3d/checkpoints/%lu.chk", state.project_path,
                 (unsigned long)visual_task_id) > 0 &&
        lardon3d_task_checkpoint_load(visual_checkpoint, &historical_visual,
                                      NULL) ==
            LARDON3D_TASK_CHECKPOINT_OK &&
        historical_visual.estimate.desired_cpu_threads == 16);
  historical_visual.estimate.desired_cpu_threads = 12;
  CHECK(lardon3d_task_checkpoint_save(visual_checkpoint,
                                      &historical_visual) ==
        LARDON3D_TASK_CHECKPOINT_OK);
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  state.resource_governor = NULL;
  CHECK(unsetenv("LARDON3D_TEST_VISUAL_INDEX_PAUSE_AFTER_SEGMENT") == 0 &&
        unsetenv("LARDON3D_TEST_VISUAL_INDEX_SKIP_FINISHED_CHECKPOINT") == 0);
  lardon3d_app_state_init(&state);
  CHECK(runtime(&state) && lardon3d_project_open(&state, "Features"));
  CHECK(lardon3d_project_last_recovery_summary(&state, &summary) && summary.resumed == 1);
  CHECK(wait_state(state.task_queue, visual_task_id, TASK_COMPLETED, &snapshot));
  Lardon3DProjectDbVisualIndexSegment visual_segments[2];
  size_t visual_segment_count = 0;
  CHECK(lardon3d_project_db_list_visual_index_segments(
            state.project_db, visual_index_id, 0, visual_segments, 2, &visual_segment_count) ==
            LARDON3D_PROJECT_DB_OK &&
        visual_segment_count == 1);
  CHECK(lardon3d_project_db_load_feature_set(state.project_db, second_sift.feature_set_id,
                                             &second_sift) == LARDON3D_PROJECT_DB_OK &&
        lardon3d_feature_reader_open(state.project_path, &second_sift, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);

  char second_managed_path[PATH_MAX];
  CHECK(join_path(second_managed_path, state.project_path, second_asset.path));
  int second_managed_fd = open(second_managed_path, O_WRONLY | O_CLOEXEC);
  unsigned char corrupt_byte = 0;
  CHECK(second_managed_fd >= 0 && pwrite(second_managed_fd, &corrupt_byte, 1, 32) == 1 &&
        close(second_managed_fd) == 0);
  Lardon3DSiftExtractorParameters corrupt_source_parameters = sift_parameters;
  corrupt_source_parameters.max_features = 498;
  uint64_t corrupt_source_task = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, second_image.image_id,
                                              &corrupt_source_parameters,
                                              &corrupt_source_task) &&
        wait_state(state.task_queue, corrupt_source_task, TASK_FAILED, &snapshot) &&
        lardon3d_feature_reader_open(state.project_path, &second_sift, &reader, &metadata) ==
            LARDON3D_FEATURE_STORE_OK);
  lardon3d_feature_reader_close(reader);

  CHECK(write_pgm(source));
  Lardon3DProjectDbImage missing_image;
  Lardon3DProjectDbImageAsset missing_asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, source, 0, &missing_image,
                                           &missing_asset) ==
        LARDON3D_IMAGE_CATALOG_ALREADY_PRESENT);
  char managed_path[PATH_MAX];
  CHECK(join_path(managed_path, state.project_path, missing_asset.path));
  CHECK(unlink(managed_path) == 0);
  uint64_t missing_task_id = 0;
  Lardon3DFeatureExtractorParameters missing_parameters = {511, 4, 10};
  CHECK(lardon3d_project_enqueue_feature_extract(&state, missing_image.image_id,
                                                 &missing_parameters, &missing_task_id));
  CHECK(wait_state(state.task_queue, missing_task_id, TASK_FAILED, &snapshot));

  char invalid_source[PATH_MAX];
  CHECK(join_path(invalid_source, root, "invalid.pgm"));
  FILE *invalid = fopen(invalid_source, "wb");
  CHECK(invalid && fwrite("not an image", 1, 12, invalid) == 12 && fclose(invalid) == 0);
  Lardon3DProjectDbImage invalid_image;
  Lardon3DProjectDbImageAsset invalid_asset;
  CHECK(lardon3d_image_catalog_import_file(&state, scanset.scanset_id, invalid_source, 0,
                                           &invalid_image,
                                           &invalid_asset) == LARDON3D_IMAGE_CATALOG_IMPORTED);
  uint64_t invalid_task_id = 0;
  Lardon3DFeatureExtractorParameters invalid_parameters = {510, 4, 10};
  CHECK(lardon3d_project_enqueue_feature_extract(&state, invalid_image.image_id,
                                                 &invalid_parameters, &invalid_task_id));
  CHECK(wait_state(state.task_queue, invalid_task_id, TASK_FAILED, &snapshot));
  uint64_t invalid_sift_task_id = 0;
  CHECK(lardon3d_project_enqueue_sift_extract(&state, invalid_image.image_id, &sift_parameters,
                                              &invalid_sift_task_id) &&
        wait_state(state.task_queue, invalid_sift_task_id, TASK_FAILED, &snapshot));
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = NULL;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  CHECK(remove_tree(root));
  return true;
}
int main(void) { return run_test() ? 0 : 1; }
