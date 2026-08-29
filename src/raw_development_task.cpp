extern "C" {
#include <lardon3d/project.h>
#include <lardon3d/raw_development.h>
#include <lardon3d/raw_development_task.h>
#include <lardon3d/task_queue.h>
}

#include <cstdio>
#include <new>

namespace {
/* One admitted RAW execution owns a conservative allowance for the bounded
 * 40 MP decoder image, LibRaw storage, BGR/PNG buffers, validation decode, and
 * immutable-asset publication. This is an operational hardware bound, not a
 * scientific limit on the number of Captures. The Queue/Governor reservation
 * exists only for this one Task callback and is released by Task runtime on
 * every terminal path. */
constexpr uint64_t kRawWorkingBytes = UINT64_C(2) * 1024u * 1024u * 1024u;

struct Context {
  char project_path[LARDON3D_APP_STATE_PATH_CAPACITY]{};
  Lardon3DProjectDb *database{};
  Lardon3DResourceGovernor *governor{};
  uint64_t capture_id{};
  uint64_t source_asset_id{};
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
    return run_impl(task, static_cast<Context *>(value));
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
    Lardon3DResourceEstimate estimate{kRawWorkingBytes + sizeof(Context), 0, 0, 0,
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
