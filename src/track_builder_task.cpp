#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <openssl/evp.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "track_builder_internal.hpp"

extern "C" {
#include <lardon3d/match_file.h>
#include <lardon3d/project.h>
#include <lardon3d/task_checkpoint.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/track_builder.h>
#include <lardon3d/track_builder_task.h>
}

namespace {
constexpr std::array<unsigned char, 8> kScopeMagic = {
    'L', '3', 'D', 'T', 'S', 'C', 'P', '1'};
constexpr uint32_t kScopeVersion = 1;
constexpr uint64_t kFixedMemory = 4ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumScopeBytes = 64ULL * 1024ULL * 1024ULL;

struct Context {
  std::string project_path;
  Lardon3DProjectDb *database = nullptr;
  Lardon3DResourceGovernor *governor = nullptr;
  Lardon3DProjectDbTrackBuilderTask durable{};
  std::vector<uint64_t> ids;
};

void destroy_context(void *userdata) { delete static_cast<Context *>(userdata); }

bool checked_add(uint64_t left, uint64_t right, uint64_t *result) {
  if (right > std::numeric_limits<uint64_t>::max() - left) return false;
  *result = left + right;
  return true;
}

bool checked_mul(uint64_t left, uint64_t right, uint64_t *result) {
  if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
  *result = left * right;
  return true;
}

bool compact_memory_estimate(uint64_t raw_edges, uint64_t feature_sets,
                             uint64_t max_match_count,
                             uint64_t *memory) {
  if (!memory || raw_edges > UINT32_MAX / 2U) return false;
  const uint64_t nodes = raw_edges * 2U;
  uint64_t required_slots = 0;
  if (!checked_add(nodes, nodes / 2U, &required_slots) ||
      !checked_add(required_slots, 1U, &required_slots)) return false;
  uint64_t slots = required_slots == 0 ? 0 : 8;
  while (slots < required_slots) {
    if (!checked_mul(slots, 2U, &slots)) return false;
  }
  uint64_t graph_and_peak_node_bytes = 0, edge_bytes = 0;
  uint64_t identity_bytes = 0, feature_bytes = 0, match_file_peak_bytes = 0;
  /* WHY/ACCOUNTING: 133 B/node is the simultaneous retained node (16),
   * DSU/group/conflict scratch (17), flat canonical output (24), worst
   * per-track publication serialization (44: stored observation capacity 16,
   * per-track vector capacity/objects up to 12, and publish rows 16), plus
   * two 16-byte sorted transient publication-validation keys. Identity slots
   * and raw indexed edges are charged by their actual reserved capacities. 640 B/Feature Set
   * bounds the metadata projection plus the adapter cache/hash allocation.
   * WHY/CONTRACT: resolve_gvr materializes exactly one Match File at a time;
   * charge its largest entry vector as a peak, not the sum across the scope. */
  if (!checked_mul(nodes, 133U, &graph_and_peak_node_bytes) ||
      !checked_mul(raw_edges, sizeof(lardon3d::track_builder_internal::Edge), &edge_bytes) ||
      !checked_mul(slots, sizeof(lardon3d::track_builder_internal::IdentitySlot),
                   &identity_bytes) ||
      !checked_mul(feature_sets, 640U, &feature_bytes) ||
      !checked_mul(max_match_count, sizeof(Lardon3DMatchFileEntry),
                   &match_file_peak_bytes)) return false;
  uint64_t total = kFixedMemory;
  if (!checked_add(total, graph_and_peak_node_bytes, &total) ||
      !checked_add(total, edge_bytes, &total) ||
      !checked_add(total, identity_bytes, &total) ||
      !checked_add(total, feature_bytes, &total) ||
      !checked_add(total, match_file_peak_bytes, &total)) return false;
  *memory = total;
  return true;
}

bool hash_bytes(const unsigned char *data, size_t size, unsigned char output[32]) {
  unsigned int length = 0;
  return EVP_Digest(data, size, output, &length, EVP_sha256(), nullptr) == 1 && length == 32;
}

bool scope_hash(const std::vector<uint64_t> &ids, unsigned char output[32]) {
  if (ids.size() > (std::numeric_limits<size_t>::max() - 8U) / 8U) return false;
  std::vector<unsigned char> bytes(8U + ids.size() * 8U);
  std::memcpy(bytes.data(), "L3DTSIS1", 8);
  for (size_t i = 0; i < ids.size(); ++i) {
    uint64_t value = ids[i];
    for (size_t byte = 0; byte < 8; ++byte) {
      bytes[8U + i * 8U + byte] = static_cast<unsigned char>(value & 0xffU);
      value >>= 8U;
    }
  }
  return hash_bytes(bytes.data(), bytes.size(), output);
}

bool write_all(int fd, const unsigned char *data, size_t size) {
  while (size != 0) {
    ssize_t written = write(fd, data, size);
    if (written <= 0) return false;
    data += static_cast<size_t>(written);
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool read_all(int fd, unsigned char *data, size_t size) {
  while (size != 0) {
    ssize_t read_count = read(fd, data, size);
    if (read_count <= 0) return false;
    data += static_cast<size_t>(read_count);
    size -= static_cast<size_t>(read_count);
  }
  return true;
}

bool scope_asset(const std::string &project_path, uint64_t task_id,
                 const std::vector<uint64_t> &ids, char relative[4096],
                 uint64_t *size_bytes, unsigned char sha256[32]) {
  int written = std::snprintf(relative, 4096, ".lardon3d/checkpoints/%llu.scope",
                              static_cast<unsigned long long>(task_id));
  if (written <= 0 || written >= 4096) return false;
  std::string final_path = project_path + "/" + relative;
  std::string temporary = final_path + ".tmp";
  std::string directory_path = project_path + "/.lardon3d/checkpoints";
  std::vector<unsigned char> bytes;
  bytes.reserve(20U + ids.size() * 8U);
  bytes.insert(bytes.end(), kScopeMagic.begin(), kScopeMagic.end());
  for (unsigned shift = 0; shift < 4; ++shift)
    bytes.push_back(static_cast<unsigned char>((kScopeVersion >> (shift * 8U)) & 0xffU));
  uint64_t count = ids.size();
  for (unsigned shift = 0; shift < 8; ++shift)
    bytes.push_back(static_cast<unsigned char>((count >> (shift * 8U)) & 0xffU));
  for (uint64_t id : ids)
    for (unsigned shift = 0; shift < 8; ++shift)
      bytes.push_back(static_cast<unsigned char>((id >> (shift * 8U)) & 0xffU));
  if (!hash_bytes(bytes.data(), bytes.size(), sha256)) return false;
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = write_all(fd, bytes.data(), bytes.size()) && fsync(fd) == 0;
  if (close(fd) != 0) ok = false;
  if (!ok) {
    (void)unlink(temporary.c_str());
    return false;
  }
  ok = rename(temporary.c_str(), final_path.c_str()) == 0;
  bool final_published = ok;
  if (ok) {
    int directory = open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
      ok = false;
    } else {
      bool synced = fsync(directory) == 0;
#ifdef LARDON3D_TRACK_BUILDER_TASK_TESTING
      const char *fail = std::getenv("LARDON3D_TRACK_BUILDER_TEST_FAIL_SCOPE_DIR_FSYNC");
      if (fail && std::strcmp(fail, "1") == 0) synced = false;
#endif
      bool closed = close(directory) == 0;
      ok = synced && closed;
    }
  }
  if (!ok) {
    (void)unlink(temporary.c_str());
    if (final_published) (void)unlink(final_path.c_str());
  }
  *size_bytes = bytes.size();
  return ok;
}

bool load_scope(const Context &context, std::vector<uint64_t> *ids) {
  std::string path = context.project_path + "/" + context.durable.scope_path;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 || context.durable.scope_size_bytes > kMaximumScopeBytes) {
    if (fd >= 0) (void)close(fd);
    return false;
  }
  std::vector<unsigned char> bytes(static_cast<size_t>(context.durable.scope_size_bytes));
  bool ok = read_all(fd, bytes.data(), bytes.size());
  if (close(fd) != 0) ok = false;
  unsigned char digest[32]{};
  ok = ok && bytes.size() >= 20 && std::memcmp(bytes.data(), kScopeMagic.data(), 8) == 0 &&
       bytes[8] == 1 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0 &&
       hash_bytes(bytes.data(), bytes.size(), digest) &&
       std::memcmp(digest, context.durable.scope_sha256, 32) == 0;
  if (!ok) return false;
  uint64_t count = 0;
  for (unsigned shift = 0; shift < 8; ++shift)
    count |= static_cast<uint64_t>(bytes[12 + shift]) << (shift * 8U);
  if (count == 0 || count > 10000000ULL || bytes.size() != 20U + count * 8U ||
      count != context.durable.gvr_count) return false;
  ids->resize(static_cast<size_t>(count));
  for (size_t i = 0; i < ids->size(); ++i) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 8; ++shift)
      value |= static_cast<uint64_t>(bytes[20 + i * 8 + shift]) << (shift * 8U);
    if (value == 0 || (i != 0 && (*ids)[i - 1] >= value)) return false;
    (*ids)[i] = value;
  }
  unsigned char scope[32]{};
  return scope_hash(*ids, scope) && std::memcmp(scope, context.durable.input_scope_hash, 32) == 0;
}

bool persist(Context *context, const Lardon3DTask *task) {
  Lardon3DTaskDurableSnapshot snapshot{};
  if (!lardon3d_task_durable_snapshot(task, &snapshot)) return false;
  char relative[4096];
  if (std::snprintf(relative, sizeof(relative), "%s", context->durable.scope_path) <= 0)
    return false;
  Lardon3DProjectDbCheckpoint checkpoint{};
  std::snprintf(checkpoint.path, sizeof(checkpoint.path), ".lardon3d/checkpoints/%llu.chk",
                static_cast<unsigned long long>(snapshot.id));
  checkpoint.format_version = LARDON3D_TASK_CHECKPOINT_VERSION;
  checkpoint.durability = LARDON3D_DB_CHECKPOINT_DURABLE;
  checkpoint.updated_at = 0;
  std::string checkpoint_path = context->project_path + "/" + checkpoint.path;
  Lardon3DTaskCheckpointResult saved = lardon3d_task_checkpoint_save(checkpoint_path.c_str(), &snapshot);
  if (saved != LARDON3D_TASK_CHECKPOINT_OK &&
      saved != LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE) return false;
  checkpoint.durability = saved == LARDON3D_TASK_CHECKPOINT_OK
                              ? LARDON3D_DB_CHECKPOINT_DURABLE
                              : LARDON3D_DB_CHECKPOINT_PUBLISHED_NOT_DURABLE;
  Lardon3DProjectDbTrackBuilderTask durable = context->durable;
#ifdef LARDON3D_TRACK_BUILDER_TASK_TESTING
  const char *fail = std::getenv("LARDON3D_TRACK_BUILDER_TEST_FAIL_INITIAL_PERSIST");
  if (fail && std::strcmp(fail, "1") == 0) return false;
#endif
  return lardon3d_project_db_record_track_builder_task(
             context->database, &snapshot, LARDON3D_TRACK_BUILDER_TASK_KIND,
             LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION, &checkpoint, &durable, 0) ==
         LARDON3D_PROJECT_DB_OK;
}

void cleanup_unowned_creation_assets(const std::string &project_path,
                                     const std::string &scope_path,
                                     uint64_t task_id) {
  /* WHY/OWNERSHIP: task_id was freshly allocated and no durable Task row owns
   * these paths until the initial persist succeeds. This helper is used only
   * on that pre-record creation path; recovery/existing durable tasks must
   * never have their scope or checkpoint removed here. */
  std::string scope = project_path + "/" + scope_path;
  std::string checkpoint = project_path + "/.lardon3d/checkpoints/" +
                           std::to_string(task_id) + ".chk";
  (void)unlink((scope + ".tmp").c_str());
  (void)unlink(scope.c_str());
  (void)unlink((checkpoint + ".next").c_str());
  (void)unlink((checkpoint + ".tmp").c_str());
  (void)unlink(checkpoint.c_str());
}

struct UnownedCreationAssets {
  std::string project_path;
  std::string scope_path;
  uint64_t task_id = 0;
  bool active = true;

  ~UnownedCreationAssets() {
    if (active) cleanup_unowned_creation_assets(project_path, scope_path, task_id);
  }
};

bool project_checkpoint(void *userdata) {
  return lardon3d_task_checkpoint(static_cast<Lardon3DTask *>(userdata));
}

bool run(Lardon3DTask *task, void *userdata) {
  try {
  Context *context = static_cast<Context *>(userdata);
  if (!lardon3d_task_checkpoint(task)) return false;
  Lardon3DTrackBuilderProjectRequest request = {
      context->project_path.c_str(), context->database, context->durable.verifier_kind,
      context->durable.verifier_version, context->durable.verifier_fingerprint,
      context->ids.data(), context->ids.size()};
  Lardon3DTrackBuilderProjectResult result{};
  Lardon3DTrackBuilderProjectStatus status =
      lardon3d::track_builder_internal::build_project(
          &request, &result, project_checkpoint, task);
  if (status == LARDON3D_TRACK_BUILDER_PROJECT_INTERRUPTED) return false;
  if (status != LARDON3D_TRACK_BUILDER_PROJECT_OK)
    return lardon3d_task_fail(task, "Track Builder publication impossible.");
  return lardon3d_task_set_progress(task, 100, result.reused ? "Track Set réutilisé."
                                                               : "Track Set publié.");
  } catch (...) {
    return lardon3d_task_fail(task, "Track Builder runtime exception.");
  }
}

void finished(const Lardon3DTask *task, void *userdata) {
  try {
#ifdef LARDON3D_TRACK_BUILDER_TASK_TESTING
  const char *skip = std::getenv("LARDON3D_TRACK_BUILDER_TEST_SKIP_FINISHED");
  if (skip && std::strcmp(skip, "1") == 0) return;
#endif
  (void)persist(static_cast<Context *>(userdata), task);
  } catch (...) {
    /* Terminal checkpoint failure is reported by the durable DB on recovery. */
  }
}

bool make_context(const Lardon3DTaskReconstructionContext *runtime,
                  const Lardon3DProjectDbTrackBuilderTask &durable,
                  Context **output) {
  if (!runtime || !runtime->project_path || !runtime->project_db ||
      !runtime->resource_governor || durable.builder_version != LARDON3D_TRACK_BUILDER_VERSION ||
      durable.scope_format_version != kScopeVersion) return false;
  auto *context = new (std::nothrow) Context;
  if (!context) return false;
  context->project_path = runtime->project_path;
  context->database = runtime->project_db;
  context->governor = runtime->resource_governor;
  context->durable = durable;
  if (!load_scope(*context, &context->ids)) {
    delete context;
    return false;
  }
  unsigned char fingerprint[32]{};
  if (!lardon3d_track_builder_fingerprint(fingerprint) ||
      std::memcmp(fingerprint, durable.builder_fingerprint, 32) != 0) {
    delete context;
    return false;
  }
  *output = context;
  return true;
}
} // namespace

extern "C" bool lardon3d_track_builder_task_memory_estimate(
    uint64_t raw_edge_count, uint64_t feature_set_count,
    uint64_t *memory_bytes) {
  return compact_memory_estimate(raw_edge_count, feature_set_count, 0, memory_bytes);
}

extern "C" bool lardon3d_track_builder_task_memory_estimate_with_match_peak(
    uint64_t raw_edge_count, uint64_t feature_set_count,
    uint64_t max_match_count, uint64_t *memory_bytes) {
  return compact_memory_estimate(raw_edge_count, feature_set_count,
                                 max_match_count, memory_bytes);
}

static bool reconstruct_impl(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  if (!snapshot || !userdata || !binding) return false;
  auto *runtime = static_cast<Lardon3DTaskReconstructionContext *>(userdata);
  Lardon3DProjectDbTrackBuilderTask durable{};
  if (lardon3d_project_db_load_track_builder_task(runtime->project_db, snapshot->id, &durable) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  Context *context = nullptr;
  if (!make_context(runtime, durable, &context)) return false;
  *binding = {};
  binding->callback = run;
  binding->userdata = context;
  binding->userdata_destroy = destroy_context;
  binding->finished_callback = finished;
  binding->finished_userdata = context;
  return true;
}

static Lardon3DTask *create_impl(
    Lardon3DAppState *state,
    const Lardon3DTrackBuilderTaskConfiguration *configuration, uint64_t *task_id) {
  if (task_id) *task_id = 0;
  if (!state || !state->project_loaded || !state->project_db || !state->resource_governor ||
      !configuration || !configuration->project_path || !configuration->verifier_fingerprint ||
      !configuration->gvr_ids || configuration->gvr_count == 0 || configuration->verifier_kind <= 0 ||
      configuration->verifier_version == 0 ||
      (configuration->gvr_count > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) ||
      configuration->gvr_count > (kMaximumScopeBytes - 20U) / 8U) return nullptr;
  std::vector<uint64_t> ids(configuration->gvr_ids,
                            configuration->gvr_ids + configuration->gvr_count);
  for (size_t i = 0; i < ids.size(); ++i)
    if (ids[i] == 0 || (i != 0 && ids[i - 1] >= ids[i])) return nullptr;
  uint64_t raw_edges = 0;
  uint64_t max_match_count = 0;
  std::vector<uint64_t> feature_sets;
  feature_sets.reserve(ids.size() * 2U);
  for (uint64_t id : ids) {
    Lardon3DProjectDbGeometricVerificationResult gvr{};
    if (lardon3d_project_db_load_geometric_verification_result(state->project_db, id, &gvr) !=
        LARDON3D_PROJECT_DB_OK || !checked_add(raw_edges, gvr.inlier_count, &raw_edges)) return nullptr;
    Lardon3DProjectDbMatchResult match{};
    if (lardon3d_project_db_load_match_result(state->project_db, gvr.match_result_id,
                                               &match) != LARDON3D_PROJECT_DB_OK)
      return nullptr;
    max_match_count = std::max(max_match_count,
                               static_cast<uint64_t>(match.match_count));
    feature_sets.push_back(match.feature_set_id_a);
    feature_sets.push_back(match.feature_set_id_b);
  }
  std::sort(feature_sets.begin(), feature_sets.end());
  feature_sets.erase(std::unique(feature_sets.begin(), feature_sets.end()),
                     feature_sets.end());
  uint64_t memory = 0;
  if (!compact_memory_estimate(raw_edges, feature_sets.size(), max_match_count,
                               &memory)) return nullptr;
  unsigned char scope[32]{}, builder[32]{};
  if (!scope_hash(ids, scope) || !lardon3d_track_builder_fingerprint(builder)) return nullptr;
  uint64_t id = 0;
  if (lardon3d_project_db_allocate_task_id(state->project_db, &id) != LARDON3D_PROJECT_DB_OK) return nullptr;
  Context *context = new (std::nothrow) Context;
  if (!context) return nullptr;
  context->project_path = configuration->project_path;
  context->database = state->project_db;
  context->governor = state->resource_governor;
  context->ids = ids;
  context->durable.task_id = id;
  std::snprintf(context->durable.builder_kind, sizeof(context->durable.builder_kind), "track_builder");
  context->durable.builder_version = LARDON3D_TRACK_BUILDER_VERSION;
  std::memcpy(context->durable.builder_fingerprint, builder, 32);
  context->durable.verifier_kind = configuration->verifier_kind;
  context->durable.verifier_version = configuration->verifier_version;
  std::memcpy(context->durable.verifier_fingerprint, configuration->verifier_fingerprint, 32);
  std::memcpy(context->durable.input_scope_hash, scope, 32);
  context->durable.gvr_count = ids.size();
  context->durable.scope_format_version = kScopeVersion;
  int scope_written = std::snprintf(
      context->durable.scope_path, sizeof(context->durable.scope_path),
      ".lardon3d/checkpoints/%llu.scope", static_cast<unsigned long long>(id));
  if (scope_written <= 0 || static_cast<size_t>(scope_written) >=
                                sizeof(context->durable.scope_path)) {
    delete context;
    return nullptr;
  }
  /* Arm before publication so allocation exceptions cannot strand an asset. */
  UnownedCreationAssets unowned{context->project_path, context->durable.scope_path, id, true};
  if (!scope_asset(context->project_path, id, ids, context->durable.scope_path,
                   &context->durable.scope_size_bytes, context->durable.scope_sha256)) {
    delete context;
    return nullptr;
  }
  /* Disarm only after the Task row transaction succeeds. Copies keep
   * exception rollback independent of the Context lifetime. */
  Lardon3DResourceEstimate estimate = {memory, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_CPU};
  Lardon3DTask *task = lardon3d_task_create_typed(
      "Track Builder", &estimate, LARDON3D_TRACK_BUILDER_TASK_KIND,
      LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION, run, context, destroy_context);
  if (!task || !lardon3d_task_assign_id(task, id) ||
      !lardon3d_task_set_finished_callback(task, finished, context) || !persist(context, task)) {
    if (task) lardon3d_task_destroy(task); else delete context;
    return nullptr;
  }
  unowned.active = false;
  if (task_id) *task_id = id;
  return task;
}

extern "C" bool lardon3d_track_builder_task_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *userdata,
    Lardon3DTaskKindBinding *binding) {
  try {
    return reconstruct_impl(snapshot, userdata, binding);
  } catch (...) {
    return false;
  }
}

extern "C" Lardon3DTask *lardon3d_project_create_track_builder_task(
    Lardon3DAppState *state,
    const Lardon3DTrackBuilderTaskConfiguration *configuration, uint64_t *task_id) {
  try {
    return create_impl(state, configuration, task_id);
  } catch (...) {
    if (task_id) *task_id = 0;
    return nullptr;
  }
}

extern "C" bool lardon3d_project_enqueue_track_builder_task(
    Lardon3DAppState *state,
    const Lardon3DTrackBuilderTaskConfiguration *configuration, uint64_t *task_id) {
  try {
    Lardon3DTask *task = lardon3d_project_create_track_builder_task(
        state, configuration, task_id);
    if (!task || !state || !state->task_queue ||
        !lardon3d_task_queue_add(state->task_queue, task, nullptr)) {
      if (task) lardon3d_task_destroy(task);
      return false;
    }
    return true;
  } catch (...) {
    if (task_id) *task_id = 0;
    return false;
  }
}
