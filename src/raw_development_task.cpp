extern "C" {
#include <lardon3d/project.h>
#include <lardon3d/raw_development.h>
#include <lardon3d/raw_development_task.h>
#include <lardon3d/task_queue.h>
}

#include "opencv_task_thread_guard.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <new>
#include <pthread.h>

namespace {
/* One admitted RAW execution owns a conservative allowance for the bounded
 * 40 MP decoder image, LibRaw storage, BGR/PNG buffers, validation decode, and
 * immutable-asset publication. This is an operational hardware bound, not a
 * scientific limit on the number of Captures. The Queue/Governor reservation
 * exists only for this one Task callback and is released by Task runtime on
 * every terminal path. */
constexpr uint64_t kRawSingleWorkingBytes = UINT64_C(2) * 1024u * 1024u * 1024u;
constexpr size_t kRawBatchWindowMax = 8;
constexpr size_t kRawBatchChildStackBytes = 1024u * 1024u;
constexpr uint64_t kRawBatchParticipantBytes = UINT64_C(896) * 1024u * 1024u;
static_assert(kRawBatchParticipantBytes <= UINT64_MAX / kRawBatchWindowMax,
              "RAW batch memory envelope must fit uint64_t");

struct Context {
  char project_path[LARDON3D_APP_STATE_PATH_CAPACITY]{};
  Lardon3DProjectDb *database{};
  Lardon3DResourceGovernor *governor{};
  uint64_t capture_id{};
  uint64_t source_asset_id{};
};

struct BatchContext {
  char project_path[LARDON3D_APP_STATE_PATH_CAPACITY]{};
  Lardon3DProjectDb *database{};
  Lardon3DResourceGovernor *governor{};
  uint64_t selected_execution_id{};
};

void destroy(void *value) { delete static_cast<Context *>(value); }

void runtime(const Context *context, Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  std::snprintf(state->project_path, sizeof(state->project_path), "%s", context->project_path);
}

bool checkpoint(Context *context, Lardon3DTask *task,
                Lardon3DProjectDbRawDevelopmentTaskPhase phase, uint64_t image_id) {
  Lardon3DAppState state;
  runtime(context, &state);
  Lardon3DProjectDbRawDevelopmentTask parameters{
      lardon3d_task_id(task), context->capture_id, context->source_asset_id, phase,
      image_id != 0, image_id};
  return lardon3d_project_checkpoint_raw_development_task(&state, task, &parameters) ==
         LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}

bool run_impl(Lardon3DTask *task, Context *context) {
  Lardon3DProjectDbRawDevelopmentTask persisted{};
  if (lardon3d_project_db_load_raw_development_task(
          context->database, lardon3d_task_id(task), &persisted) != LARDON3D_PROJECT_DB_OK)
    return lardon3d_task_fail(task, "État durable du développement RAW invalide.");
  if (!lardon3d_task_checkpoint(task)) return false;
  if (persisted.phase == LARDON3D_RAW_DEVELOPMENT_TASK_PUBLISHED) {
    return lardon3d_task_set_progress(task, 100, "Développement RAW déjà publié.") &&
           checkpoint(context, task, LARDON3D_RAW_DEVELOPMENT_TASK_PUBLISHED,
                      persisted.image_id);
  }

  Lardon3DProjectDbCapture capture{};
  if (lardon3d_project_db_load_capture(context->database, context->capture_id, &capture) !=
      LARDON3D_PROJECT_DB_OK)
    return lardon3d_task_fail(task, "Capture du développement RAW introuvable.");
  Lardon3DAppState state;
  runtime(context, &state);
  Lardon3DRawDevelopmentOutput output{};
  Lardon3DRawDevelopmentResult developed = lardon3d_raw_develop_asset_to_capture(
      &state, context->capture_id, context->source_asset_id, lardon3d_task_id(task),
      capture.created_at, &output);
  if (developed != LARDON3D_RAW_DEVELOPMENT_OK)
    return lardon3d_task_fail(task, "Développement RAW S3-B1 impossible.");

  /* Immutable output publication may precede this checkpoint if the process
   * dies. The frozen developer is content-addressed and exact retry converges;
   * once this atomic typed/generic checkpoint succeeds, recovery never guesses
   * which image was published. */
  if (!lardon3d_task_set_progress(task, 100, "Représentation RAW publiée.") ||
      !checkpoint(context, task, LARDON3D_RAW_DEVELOPMENT_TASK_PUBLISHED,
                  output.image.image_id))
    return lardon3d_task_fail(task, "Checkpoint du développement RAW impossible.");
  return true;
}

bool run(Lardon3DTask *task, void *value) noexcept {
  try {
    Lardon3DOpenCvTaskThreadGuard threads(task);
    if (!threads.valid())
      return lardon3d_task_fail(task, "Contrat CPU OpenCV RAW invalide.");
    bool result = run_impl(task, static_cast<Context *>(value));
    if (!threads.restore())
      return lardon3d_task_fail(task, "Restauration OpenCV RAW impossible.");
    return result;
  } catch (const std::bad_alloc &) {
    return lardon3d_task_fail(task, "Mémoire insuffisante pour le développement RAW.");
  } catch (...) {
    return lardon3d_task_fail(task, "Erreur interne du développement RAW.");
  }
}

void finished(const Lardon3DTask *task, void *value) noexcept {
  try {
    Lardon3DAppState state;
    runtime(static_cast<Context *>(value), &state);
    (void)lardon3d_project_checkpoint_task(&state, task);
  } catch (...) {
  }
}

Context *make_context(const Lardon3DTaskReconstructionContext *runtime_context,
                      uint64_t capture_id, uint64_t source_asset_id) {
  if (!runtime_context || !runtime_context->project_path ||
      !runtime_context->project_path[0] || !runtime_context->project_db ||
      !runtime_context->resource_governor)
    return nullptr;
  Context *context = new (std::nothrow) Context;
  if (!context) return nullptr;
  int written = std::snprintf(context->project_path, sizeof(context->project_path), "%s",
                              runtime_context->project_path);
  if (written <= 0 || written >= static_cast<int>(sizeof(context->project_path))) {
    delete context;
    return nullptr;
  }
  context->database = runtime_context->project_db;
  context->governor = runtime_context->resource_governor;
  context->capture_id = capture_id;
  context->source_asset_id = source_asset_id;
  return context;
}

void destroy_batch(void *value) { delete static_cast<BatchContext *>(value); }

BatchContext *make_batch_context(
    const Lardon3DTaskReconstructionContext *runtime_context,
    uint64_t selected_execution_id) {
  if (!runtime_context || !runtime_context->project_path ||
      !runtime_context->project_path[0] || !runtime_context->project_db ||
      !runtime_context->resource_governor || selected_execution_id == 0)
    return nullptr;
  auto *context = new (std::nothrow) BatchContext;
  if (!context) return nullptr;
  int written = std::snprintf(context->project_path, sizeof(context->project_path), "%s",
                              runtime_context->project_path);
  if (written <= 0 || written >= static_cast<int>(sizeof(context->project_path))) {
    delete context;
    return nullptr;
  }
  context->database = runtime_context->project_db;
  context->governor = runtime_context->resource_governor;
  context->selected_execution_id = selected_execution_id;
  return context;
}

void batch_runtime(const BatchContext *context, Lardon3DAppState *state) {
  lardon3d_app_state_init(state);
  state->project_loaded = true;
  state->project_db = context->database;
  state->resource_governor = context->governor;
  std::snprintf(state->project_path, sizeof(state->project_path), "%s",
                context->project_path);
}

bool batch_checkpoint(BatchContext *context, Lardon3DTask *task) {
  Lardon3DAppState state;
  batch_runtime(context, &state);
  Lardon3DProjectDbRawDevelopmentBatchTask parameters{
      lardon3d_task_id(task), context->selected_execution_id};
  return lardon3d_project_checkpoint_raw_development_batch_task(
             &state, task, &parameters) == LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}

struct BatchWorker {
  BatchContext *context{};
  Lardon3DProjectDbSelectedExecutionItem item{};
  uint64_t producer_task_id{};
  Lardon3DRawDevelopmentResult raw_result{LARDON3D_RAW_DEVELOPMENT_INTERNAL_ERROR};
  Lardon3DProjectDbResult db_result{LARDON3D_PROJECT_DB_IO_ERROR};
  uint64_t image_id{};
};

void *develop_batch_item(void *value) {
  auto *worker = static_cast<BatchWorker *>(value);
  if (worker->item.representation_source ==
      LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE) {
    worker->db_result = lardon3d_project_db_get_selected_capture_image(
        worker->context->database, worker->item.capture_id, &worker->image_id);
    return nullptr;
  }
  Lardon3DProjectDbCapture capture{};
  worker->db_result = lardon3d_project_db_load_capture(
      worker->context->database, worker->item.capture_id, &capture);
  if (worker->db_result != LARDON3D_PROJECT_DB_OK) return nullptr;
  Lardon3DAppState state;
  batch_runtime(worker->context, &state);
  Lardon3DRawDevelopmentOutput output{};
  worker->raw_result = lardon3d_raw_develop_asset_to_capture(
      &state, worker->item.capture_id, worker->item.source_asset_id,
      worker->producer_task_id, capture.created_at, &output);
  if (worker->raw_result == LARDON3D_RAW_DEVELOPMENT_OK) {
    worker->image_id = output.image.image_id;
    worker->db_result = LARDON3D_PROJECT_DB_OK;
  }
  return nullptr;
}

struct BatchParticipant {
  BatchWorker *workers{};
  size_t count{};
  size_t first{};
  size_t stride{};
};

void *develop_batch_participant(void *value) {
  auto *participant = static_cast<BatchParticipant *>(value);
  for (size_t index = participant->first; index < participant->count;
       index += participant->stride)
    (void)develop_batch_item(&participant->workers[index]);
  return nullptr;
}

bool develop_batch_window(BatchContext *context, uint64_t producer_task_id,
                          const Lardon3DProjectDbSelectedExecutionItem *items,
                          size_t count, unsigned int admitted_threads,
                          std::array<BatchWorker, kRawBatchWindowMax> *workers) {
  if (!context || !items || !workers || count == 0 || count > kRawBatchWindowMax ||
      admitted_threads == 0)
    return false;
  size_t participants = std::min(count, static_cast<size_t>(admitted_threads));
  std::array<pthread_t, kRawBatchWindowMax - 1> children{};
  std::array<BatchParticipant, kRawBatchWindowMax> participant_work{};
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  if (pthread_attr_setstacksize(&attributes, kRawBatchChildStackBytes) != 0) {
    (void)pthread_attr_destroy(&attributes);
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    (*workers)[index].context = context;
    (*workers)[index].item = items[index];
    (*workers)[index].producer_task_id = producer_task_id;
  }
  size_t created = 0;
  bool started = true;
  for (size_t participant = 1; participant < participants; ++participant) {
    participant_work[participant] = {
        workers->data(), count, participant, participants};
    /* Each participant processes one disjoint strided subset. No child owns
     * selected cursor/progress publication, even when it finishes first. */
    if (pthread_create(&children[created], &attributes,
                       develop_batch_participant,
                       &participant_work[participant]) != 0) {
      started = false;
      break;
    }
    ++created;
  }
  if (pthread_attr_destroy(&attributes) != 0) started = false;
  participant_work[0] = {workers->data(), count, 0, participants};
  if (started) (void)develop_batch_participant(&participant_work[0]);
  for (size_t index = 0; index < created; ++index) {
    if (pthread_join(children[index], nullptr) != 0) started = false;
  }
  return started;
}

bool run_batch_impl(Lardon3DTask *task, BatchContext *context) {
  for (;;) {
    if (!lardon3d_task_checkpoint(task)) return false;
    Lardon3DProjectDbSelectedExecution execution{};
    if (lardon3d_project_db_load_selected_execution(
            context->database, context->selected_execution_id, &execution) !=
        LARDON3D_PROJECT_DB_OK)
      return lardon3d_task_fail(task, "Exécution sélectionnée RAW batch invalide.");
    if (execution.next_item_index == execution.item_count) {
      return lardon3d_task_set_durable_progress(
                 task, execution.item_count, execution.item_count,
                 "Développement RAW batch terminé.") &&
             batch_checkpoint(context, task);
    }
    if (execution.stage != LARDON3D_SELECTED_EXECUTION_REPRESENTATIONS)
      return lardon3d_task_fail(task, "Étape sélectionnée RAW batch invalide.");

    Lardon3DTaskExecutionContract contract{};
    if (!lardon3d_task_execution_contract(task, &contract) ||
        contract.batch_size == 0 || contract.batch_size > kRawBatchWindowMax ||
        contract.cpu_threads == 0)
      return lardon3d_task_fail(task, "Contrat Governor RAW batch invalide.");
    size_t count = std::min<size_t>(contract.batch_size,
                                    execution.item_count - execution.next_item_index);
    std::array<Lardon3DProjectDbSelectedExecutionItem, kRawBatchWindowMax> items{};
    for (size_t offset = 0; offset < count; ++offset) {
      if (lardon3d_project_db_load_selected_execution_item(
              context->database, context->selected_execution_id,
              execution.next_item_index + static_cast<uint32_t>(offset),
              &items[offset]) != LARDON3D_PROJECT_DB_OK)
        return lardon3d_task_fail(task, "Item sélectionné RAW batch invalide.");
    }

    std::array<BatchWorker, kRawBatchWindowMax> workers{};
    /* Each independent LibRaw/BGR/PNG participant fits the admitted 896 MiB
     * per-item envelope. Every child is joined before cursor publication or
     * reservation release. */
    if (!develop_batch_window(context, lardon3d_task_id(task), items.data(), count,
                              contract.cpu_threads, &workers))
      return lardon3d_task_fail(task, "Workers RAW batch impossibles.");
    if (!lardon3d_task_checkpoint(task)) return false;

    for (size_t offset = 0; offset < count; ++offset) {
      BatchWorker &worker = workers[offset];
      if (worker.db_result != LARDON3D_PROJECT_DB_OK || worker.image_id == 0 ||
          (worker.item.representation_source ==
               LARDON3D_SELECTED_REPRESENTATION_RAW_ASSET &&
           worker.raw_result != LARDON3D_RAW_DEVELOPMENT_OK))
        return lardon3d_task_fail(task, "Développement d'un item RAW batch impossible.");
      uint32_t item_index = worker.item.item_index;
      if (lardon3d_project_db_record_selected_representation(
              context->database, context->selected_execution_id, item_index,
              worker.image_id, item_index + 1u) != LARDON3D_PROJECT_DB_OK)
        return lardon3d_task_fail(task, "Publication ordonnée RAW batch impossible.");
      /* The selected cursor commit is the authority. Generic durable progress
       * follows it and may lag after a crash, but must never lead it. */
      if (!lardon3d_task_set_durable_progress(
              task, item_index + 1u, execution.item_count,
              "Représentation RAW batch publiée.") ||
          !batch_checkpoint(context, task))
        return lardon3d_task_fail(task, "Checkpoint RAW batch impossible.");
    }
    if (execution.next_item_index + count == execution.item_count)
      return true;
    Lardon3DResourceReservation *reservation = nullptr;
    if (!lardon3d_task_sequence_break(task, context->governor, &reservation,
                                      &contract))
      return false;
  }
}

bool run_batch(Lardon3DTask *task, void *value) noexcept {
  try {
    Lardon3DOpenCvTaskThreadGuard threads(task);
    if (!threads.valid())
      return lardon3d_task_fail(task, "Contrat CPU OpenCV RAW batch invalide.");
    /* External Task participants are the admitted CPU dimension. OpenCV stays
     * single-threaded so imencode/imdecode cannot create a nested fan-out. */
    cv::setNumThreads(1);
    if (cv::getNumThreads() != 1)
      return lardon3d_task_fail(task, "Configuration OpenCV RAW batch impossible.");
    bool result = run_batch_impl(task, static_cast<BatchContext *>(value));
    if (!threads.restore())
      return lardon3d_task_fail(task, "Restauration OpenCV RAW batch impossible.");
    return result;
  } catch (const std::bad_alloc &) {
    return lardon3d_task_fail(task, "Mémoire insuffisante pour le RAW batch.");
  } catch (...) {
    return lardon3d_task_fail(task, "Erreur interne du RAW batch.");
  }
}

void finished_batch(const Lardon3DTask *task, void *value) noexcept {
  try {
    Lardon3DAppState state;
    batch_runtime(static_cast<BatchContext *>(value), &state);
    (void)lardon3d_project_checkpoint_task(&state, task);
  } catch (...) {
  }
}
}  // namespace

extern "C" bool lardon3d_raw_development_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *value,
    Lardon3DTaskKindBinding *binding) {
  try {
    auto *runtime_context = static_cast<Lardon3DTaskReconstructionContext *>(value);
    if (!snapshot || !runtime_context || !binding) return false;
    Lardon3DProjectDbRawDevelopmentTask persisted{};
    if (lardon3d_project_db_load_raw_development_task(
            runtime_context->project_db, snapshot->id, &persisted) != LARDON3D_PROJECT_DB_OK)
      return false;
    Context *context = make_context(runtime_context, persisted.capture_id,
                                    persisted.source_asset_id);
    if (!context) return false;
    *binding = {};
    binding->callback = run;
    binding->userdata = context;
    binding->userdata_destroy = destroy;
    binding->finished_callback = finished;
    binding->finished_userdata = context;
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" Lardon3DTask *lardon3d_project_create_raw_development_task(
    Lardon3DAppState *state, uint64_t capture_id, uint64_t source_asset_id,
    uint64_t *task_id) {
  if (task_id) *task_id = 0;
  try {
    if (!state || !state->project_loaded || !state->project_db ||
        !state->resource_governor || !task_id || capture_id == 0 || source_asset_id == 0)
      return nullptr;
    Lardon3DTaskReconstructionContext runtime_context{
        state->project_path, state->project_db, state->resource_governor, nullptr};
    Context *context = make_context(&runtime_context, capture_id, source_asset_id);
    if (!context) return nullptr;
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK) {
      delete context;
      return nullptr;
    }
    Lardon3DResourceEstimate estimate{kRawSingleWorkingBytes + sizeof(Context), 0, 0, 0,
                                      1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_MIXED};
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Développement RAW S3-B1", &estimate, LARDON3D_RAW_DEVELOPMENT_TASK_KIND,
        LARDON3D_RAW_DEVELOPMENT_TASK_KIND_VERSION, run, context, destroy);
    if (!task || !lardon3d_task_assign_id(task, id) ||
        !lardon3d_task_set_finished_callback(task, finished, context) ||
        !checkpoint(context, task, LARDON3D_RAW_DEVELOPMENT_TASK_PENDING, 0)) {
      lardon3d_task_destroy(task);
      return nullptr;
    }
    *task_id = id;
    return task;
  } catch (...) {
    return nullptr;
  }
}

extern "C" bool lardon3d_project_enqueue_raw_development(
    Lardon3DAppState *state, uint64_t capture_id, uint64_t source_asset_id,
    uint64_t *task_id) {
  try {
    if (!state || !state->task_queue) return false;
    Lardon3DTask *task = lardon3d_project_create_raw_development_task(
        state, capture_id, source_asset_id, task_id);
    if (!task) return false;
    if (!lardon3d_task_queue_add(state->task_queue, task, nullptr)) {
      lardon3d_task_destroy(task);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" bool lardon3d_raw_development_batch_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *value,
    Lardon3DTaskKindBinding *binding) {
  try {
    auto *runtime_context = static_cast<Lardon3DTaskReconstructionContext *>(value);
    if (!snapshot || !runtime_context || !binding) return false;
    Lardon3DProjectDbRawDevelopmentBatchTask persisted{};
    if (lardon3d_project_db_load_raw_development_batch_task(
            runtime_context->project_db, snapshot->id, &persisted) !=
        LARDON3D_PROJECT_DB_OK)
      return false;
    BatchContext *context = make_batch_context(
        runtime_context, persisted.selected_execution_id);
    if (!context) return false;
    *binding = {};
    binding->callback = run_batch;
    binding->userdata = context;
    binding->userdata_destroy = destroy_batch;
    binding->finished_callback = finished_batch;
    binding->finished_userdata = context;
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" Lardon3DTask *lardon3d_project_create_raw_development_batch_task(
    Lardon3DAppState *state, uint64_t selected_execution_id, uint64_t *task_id) {
  if (task_id) *task_id = 0;
  try {
    if (!state || !state->project_loaded || !state->project_db ||
        !state->resource_governor || !task_id || selected_execution_id == 0)
      return nullptr;
    Lardon3DProjectDbSelectedExecution execution{};
    if (lardon3d_project_db_load_selected_execution(
            state->project_db, selected_execution_id, &execution) !=
            LARDON3D_PROJECT_DB_OK ||
        execution.stage != LARDON3D_SELECTED_EXECUTION_REPRESENTATIONS)
      return nullptr;
    Lardon3DTaskReconstructionContext runtime_context{
        state->project_path, state->project_db, state->resource_governor, nullptr};
    BatchContext *context = make_batch_context(&runtime_context,
                                               selected_execution_id);
    if (!context) return nullptr;
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id) !=
        LARDON3D_PROJECT_DB_OK) {
      delete context;
      return nullptr;
    }
    /* At the frozen 40 MP decoder ceiling, one participant owns LibRaw's
     * mosaic/16-bit four-channel workspace, the processed RGB/BGR copies, and
     * the later PNG/validation buffers. Their lifetime analysis plus the 1 MiB
     * child stack and allocator/codec headroom is bounded by 896 MiB. Charge
     * the tiny owner context through that per-item allowance too: a 7 GiB
     * post-reserve host budget can then admit all eight independently bounded
     * participants instead of losing one to a redundant fixed charge.
     *
     * The 1..8 capacity is operational admission only. Immutable selected
     * execution and item order remain the scientific, durable input, never a
     * dataset-size limit. */
    Lardon3DResourceEstimate estimate{
        0, 0, kRawBatchParticipantBytes, 0,
        1, kRawBatchWindowMax, static_cast<unsigned int>(kRawBatchWindowMax),
        0, 1, LARDON3D_RESOURCE_TASK_MIXED};
    Lardon3DTask *task = lardon3d_task_create_typed(
        "Développement RAW sélectionné", &estimate,
        LARDON3D_RAW_DEVELOPMENT_BATCH_TASK_KIND,
        LARDON3D_RAW_DEVELOPMENT_BATCH_TASK_KIND_VERSION, run_batch, context,
        destroy_batch);
    if (!task || !lardon3d_task_assign_id(task, id) ||
        !lardon3d_task_set_finished_callback(task, finished_batch, context) ||
        !batch_checkpoint(context, task)) {
      lardon3d_task_destroy(task);
      return nullptr;
    }
    *task_id = id;
    return task;
  } catch (...) {
    return nullptr;
  }
}

extern "C" bool lardon3d_project_enqueue_raw_development_batch(
    Lardon3DAppState *state, uint64_t selected_execution_id,
    uint64_t *task_id) {
  try {
    if (!state || !state->task_queue) return false;
    Lardon3DTask *task = lardon3d_project_create_raw_development_batch_task(
        state, selected_execution_id, task_id);
    if (!task) return false;
    if (!lardon3d_task_queue_add(state->task_queue, task, nullptr)) {
      lardon3d_task_destroy(task);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}
