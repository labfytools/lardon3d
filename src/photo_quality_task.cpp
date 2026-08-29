extern "C" {
#include <lardon3d/photo_quality_task.h>
#include <lardon3d/photo_quality.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>
}

#include <cstdio>
#include <memory>
#include <new>
#include <vector>

namespace {
// One admitted group owns this Metrics v1 operational working allowance. It
// covers the bounded 1024-edge raster and float intermediates, not dataset size.
constexpr uint64_t kAnalysisWorkingBytes = UINT64_C(20) * 1024u * 1024u;

struct Context {
  char project_path[LARDON3D_APP_STATE_PATH_CAPACITY]{};
  Lardon3DProjectDb *db{};
  Lardon3DResourceGovernor *governor{};
  uint64_t scanset{};
  std::vector<Lardon3DAcquisitionCampaignSource> sources;
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations;
  std::vector<unsigned char> encoded;
  Lardon3DAcquisitionCampaignPlan plan{};
};

uint64_t context_owned_bytes(const Context &context) {
  // The Context and its vector storage live for the complete Task lifetime.
  // Charging their actual retained capacities plus the exact one-group analysis
  // allowance makes each Governor admission truthful without imposing a global
  // source-count or scientific dataset-size limit.
  return sizeof(Context) +
         context.sources.capacity() * sizeof(Lardon3DAcquisitionCampaignSource) +
         context.confirmations.capacity() *
             sizeof(Lardon3DAcquisitionCampaignConfirmation) +
         context.encoded.capacity();
}

void destroy(void *value) { delete static_cast<Context *>(value); }

void runtime(Context *context, Lardon3DAppState &state) {
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = context->db;
  state.resource_governor = context->governor;
  std::snprintf(state.project_path, sizeof(state.project_path), "%s", context->project_path);
}

bool checkpoint(Context *context, Lardon3DTask *task, uint32_t next_group_id) {
  Lardon3DAppState state;
  runtime(context, state);
  Lardon3DProjectDbPhotoQualityTask parameters{
      lardon3d_task_id(task), context->scanset, next_group_id,
      static_cast<uint32_t>(context->plan.group_count), context->encoded.data(),
      context->encoded.size()};
  return lardon3d_project_checkpoint_photo_quality_task(&state, task, &parameters) ==
         LARDON3D_PROJECT_TASK_CHECKPOINT_OK;
}

bool run_impl(Lardon3DTask *task, void *value) {
  auto *context = static_cast<Context *>(value);
  Lardon3DProjectDbPhotoQualityTask persisted{};
  if (lardon3d_project_db_load_photo_quality_task(context->db, lardon3d_task_id(task),
          context->encoded.data(), context->encoded.size(), &persisted) != LARDON3D_PROJECT_DB_OK)
    return lardon3d_task_fail(task, "Triage photo durable introuvable.");

  for (uint32_t group_id = persisted.next_group_id; group_id <= context->plan.group_count;
       ++group_id) {
    if (!lardon3d_task_checkpoint(task)) return false;
    /* Durable group_id is the canonical one-based acquisition-plan identity.
     * Only this private index converts it to vector position; neither value is
     * scientific Capture identity. */
    const uint32_t group_index = group_id - 1u;
    const auto &group = context->plan.groups[group_index];
    if (group.group_id != group_id)
      return lardon3d_task_fail(task, "Identité de groupe de triage incohérente.");
    uint32_t proxy = UINT32_MAX;
    for (size_t i = 0; i < group.source_count; ++i) {
      size_t source_index = group.source_indices[i];
      if (context->sources[source_index].source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG) {
        proxy = static_cast<uint32_t>(source_index);
        break;
      }
    }
    Lardon3DProjectDbPhotoQualityResult result{};
    result.task_id = lardon3d_task_id(task);
    result.group_id = group_id;
    result.proxy_source_index = proxy;
    result.override_value = LARDON3D_PHOTO_QUALITY_OVERRIDE_NONE;
    if (proxy == UINT32_MAX)
      lardon3d_photo_quality_raw_only(&result.metrics);
    else
      (void)lardon3d_photo_quality_analyze_jpeg(context->sources[proxy].path, &result.metrics);

    /* Publish measurement and the next canonical group ID in one transaction.
     * Generic Task progress may lag after a crash, but can never lead a missing
     * result; restart resumes at group_id+1 without guessing identity. */
    const uint32_t next_group_id = group_id + 1u;
    if (lardon3d_project_db_record_photo_quality_result(context->db, &result, next_group_id) !=
        LARDON3D_PROJECT_DB_OK)
      return lardon3d_task_fail(task, "Publication du résultat de triage impossible.");
    unsigned progress = static_cast<unsigned>((uint64_t(group_id) * 100u) /
                                               context->plan.group_count);
    if (!lardon3d_task_set_progress(task, progress, "Groupe photo analysé.") ||
        !checkpoint(context, task, next_group_id))
      return lardon3d_task_fail(task, "Checkpoint du triage photo impossible.");
    if (group_id < context->plan.group_count) {
      /* One acquisition group owns one bounded admission. sequence_break releases
       * it before pause/cancel handling and Governor re-admission for the next. */
      Lardon3DTaskExecutionContract contract{};
      Lardon3DResourceReservation *reservation = nullptr;
      if (!lardon3d_task_sequence_break(task, context->governor, &reservation, &contract))
        return false;
    }
  }
  return true;
}

bool run(Lardon3DTask *task, void *value) noexcept {
  try { return run_impl(task, value); }
  catch (const std::bad_alloc &) { return lardon3d_task_fail(task, "Mémoire insuffisante."); }
  catch (...) { return lardon3d_task_fail(task, "Erreur interne du triage photo."); }
}

void finished(const Lardon3DTask *task, void *value) noexcept {
  try {
    Lardon3DAppState state;
    runtime(static_cast<Context *>(value), state);
    (void)lardon3d_project_checkpoint_task(&state, task);
  } catch (...) {}
}

Context *make_context(const char *path, Lardon3DProjectDb *db, Lardon3DResourceGovernor *governor,
                      uint64_t scanset, const Lardon3DPhotoQualityTaskRequest &request,
                      const unsigned char *encoded, size_t encoded_size) {
  std::unique_ptr<Context> context(new (std::nothrow) Context);
  if (!context || !path || !path[0] ||
      std::snprintf(context->project_path, sizeof(context->project_path), "%s", path) >=
          static_cast<int>(sizeof(context->project_path))) return nullptr;
  context->db = db; context->governor = governor; context->scanset = scanset;
  context->sources.assign(request.sources, request.sources + request.source_count);
  context->confirmations.assign(request.confirmations,
      request.confirmations + request.confirmation_count);
  context->encoded.assign(encoded, encoded + encoded_size);
  if (lardon3d_acquisition_campaign_plan(context->sources.data(), context->sources.size(),
          context->confirmations.data(), context->confirmations.size(), &context->plan) !=
          LARDON3D_ACQUISITION_CAMPAIGN_OK || context->plan.group_count == 0) return nullptr;
  return context.release();
}

Lardon3DAcquisitionCampaignTaskRequest campaign_request(const Lardon3DPhotoQualityTaskRequest &r) {
  Lardon3DAcquisitionCampaignTaskRequest wire{};
  wire.sources = r.sources; wire.source_count = r.source_count;
  wire.confirmations = r.confirmations; wire.confirmation_count = r.confirmation_count;
  wire.ingest_options.representation = LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE;
  wire.ingest_options.max_source_bytes = 1;
  return wire;
}
}

extern "C" bool lardon3d_photo_quality_request_encode(const Lardon3DPhotoQualityTaskRequest *r,
    unsigned char *out, size_t cap, size_t *size) {
  if (!r) { if (size) *size = 0; return false; }
  try { auto wire = campaign_request(*r); return lardon3d_acquisition_campaign_request_encode(&wire, out, cap, size); }
  catch (...) { if (size) *size = 0; return false; }
}

extern "C" bool lardon3d_photo_quality_request_decode(const unsigned char *input, size_t size,
    Lardon3DAcquisitionCampaignSource *sources, size_t source_capacity,
    Lardon3DAcquisitionCampaignConfirmation *confirmations, size_t confirmation_capacity,
    Lardon3DPhotoQualityTaskRequest *request) {
  if (!request) return false;
  try {
    Lardon3DAcquisitionCampaignTaskRequest wire{};
    if (!lardon3d_acquisition_campaign_request_decode(input, size, sources, source_capacity,
            confirmations, confirmation_capacity, &wire)) return false;
    request->sources = wire.sources; request->source_count = wire.source_count;
    request->confirmations = wire.confirmations; request->confirmation_count = wire.confirmation_count;
    return true;
  } catch (...) { return false; }
}

extern "C" bool lardon3d_photo_quality_task_reconstruct(const Lardon3DTaskDurableSnapshot *snapshot,
    void *userdata, Lardon3DTaskKindBinding *binding) {
  try {
    auto *rt = static_cast<Lardon3DTaskReconstructionContext *>(userdata);
    if (!snapshot || !rt || !binding) return false;
    /* Restoration happens before Queue admission. These temporary buffers are
     * bounded by the durable codec/source capacities and are destroyed when
     * reconstruction returns; only the compact Context survives admission. */
    std::vector<unsigned char> blob(LARDON3D_PHOTO_QUALITY_TASK_REQUEST_MAX_BYTES);
    Lardon3DProjectDbPhotoQualityTask persisted{};
    if (lardon3d_project_db_load_photo_quality_task(rt->project_db, snapshot->id, blob.data(),
            blob.size(), &persisted) != LARDON3D_PROJECT_DB_OK) return false;
    std::vector<Lardon3DAcquisitionCampaignSource> sources(LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES);
    std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations(LARDON3D_ACQUISITION_CAMPAIGN_MAX_SOURCES);
    Lardon3DPhotoQualityTaskRequest request{};
    if (!lardon3d_photo_quality_request_decode(blob.data(), persisted.request_size, sources.data(),
            sources.size(), confirmations.data(), confirmations.size(), &request)) return false;
    auto *context = make_context(rt->project_path, rt->project_db, rt->resource_governor,
        persisted.scanset_id, request, blob.data(), persisted.request_size);
    if (!context) return false;
    *binding = {};
    binding->callback = run;
    binding->userdata = context;
    binding->userdata_destroy = destroy;
    binding->finished_callback = finished;
    binding->finished_userdata = context;
    return true;
  } catch (...) { return false; }
}

extern "C" Lardon3DTask *lardon3d_project_create_photo_quality_task(Lardon3DAppState *state,
    uint64_t scanset, const Lardon3DPhotoQualityTaskRequest *request, uint64_t *task_id) {
  if (task_id) *task_id = 0;
  try {
    if (!state || !state->project_loaded || !state->project_db || !state->resource_governor ||
        !request || !task_id) return nullptr;
    Lardon3DProjectDbScanSet row{};
    if (lardon3d_project_db_load_scanset(state->project_db, scanset, &row) != LARDON3D_PROJECT_DB_OK) return nullptr;
    size_t size = 0;
    if (!lardon3d_photo_quality_request_encode(request, nullptr, 0, &size)) return nullptr;
    std::vector<unsigned char> blob(size);
    if (!lardon3d_photo_quality_request_encode(request, blob.data(), blob.size(), &size)) return nullptr;
    auto *context = make_context(state->project_path, state->project_db, state->resource_governor,
        scanset, *request, blob.data(), size);
    if (!context) return nullptr;
    uint64_t id = 0;
    if (lardon3d_project_db_allocate_task_id(state->project_db, &id) !=
        LARDON3D_PROJECT_DB_OK) {
      delete context;
      return nullptr;
    }
    const uint64_t retained = context_owned_bytes(*context);
    if (retained > UINT64_MAX - kAnalysisWorkingBytes) { delete context; return nullptr; }
    Lardon3DResourceEstimate estimate{retained + kAnalysisWorkingBytes, 0, 0, 0,
                                      1, 1, 1, 0, 1,
                                      LARDON3D_RESOURCE_TASK_IMPORT};
    auto *task = lardon3d_task_create_typed("Triage qualité photo", &estimate,
        LARDON3D_PHOTO_QUALITY_TASK_KIND, 1, run, context, destroy);
    if (!task || !lardon3d_task_assign_id(task, id) ||
        !lardon3d_task_set_finished_callback(task, finished, context) || !checkpoint(context, task, 1)) {
      lardon3d_task_destroy(task); return nullptr;
    }
    *task_id = id; return task;
  } catch (...) { return nullptr; }
}

extern "C" bool lardon3d_project_enqueue_photo_quality(Lardon3DAppState *state, uint64_t scanset,
    const Lardon3DPhotoQualityTaskRequest *request, uint64_t *task_id) {
  try {
    if (!state || !state->task_queue) return false;
    auto *task = lardon3d_project_create_photo_quality_task(state, scanset, request, task_id);
    if (!task) return false;
    if (!lardon3d_task_queue_add(state->task_queue, task, nullptr)) { lardon3d_task_destroy(task); return false; }
    return true;
  } catch (...) { return false; }
}
