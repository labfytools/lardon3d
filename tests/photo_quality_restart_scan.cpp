#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/acquisition_campaign.h>
#include <lardon3d/photo_quality_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>
}

namespace {

constexpr size_t kGroupsPerCamera = 128;

struct TaskIds {
  uint64_t a6000{};
  uint64_t s21{};
};

struct Campaign {
  std::vector<Lardon3DAcquisitionCampaignSource> sources;
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations;
};

bool setup_runtime(Lardon3DAppState &state) {
  state.hardware_profile.logical_cpu_count = 8;
  state.hardware_profile.page_size_bytes = 4096;
  state.hardware_profile.memory_total_bytes = UINT64_C(8) << 30;
  std::snprintf(state.hardware_profile.cpu_architecture,
                sizeof(state.hardware_profile.cpu_architecture), "restart-validation");
  Lardon3DResourcePolicy policy{};
  policy.maximum_cpu_load_ratio = 1.0;
  policy.maximum_io_pressure_avg10 = 100.0;
  policy.io_slot_capacity = 2;
  state.resource_governor =
      lardon3d_resource_governor_create(&state.hardware_profile, &policy);
  state.task_queue = state.resource_governor
                         ? lardon3d_task_queue_create(state.resource_governor, 4)
                         : nullptr;
  return state.task_queue != nullptr;
}

std::string stem(const char *path) {
  const char *name = std::strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *dot = std::strrchr(name, '.');
  return std::string(name, dot ? static_cast<size_t>(dot - name) : std::strlen(name));
}

bool root(const char *path, Lardon3DAcquisitionCampaignRoot &result) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_directory(canonical, error) || error) return false;
  const std::string value = canonical.string();
  if (value.size() >= sizeof(result.path)) return false;
  std::memcpy(result.path, value.c_str(), value.size() + 1u);
  return true;
}

bool discover_a6000(const char *raw, const char *jpeg, Campaign &campaign) {
  Lardon3DAcquisitionCampaignRoot roots[2]{};
  if (!root(raw, roots[0]) || !root(jpeg, roots[1])) return false;
  auto discovery = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  if (lardon3d_acquisition_campaign_discover(roots, 2, discovery.get()) !=
      LARDON3D_ACQUISITION_CAMPAIGN_OK)
    return false;
  struct Pair { size_t raw = SIZE_MAX; size_t jpeg = SIZE_MAX; };
  std::map<std::string, Pair> pairs;
  for (size_t i = 0; i < discovery->source_count; ++i) {
    Pair &pair = pairs[stem(discovery->sources[i].path)];
    if (discovery->sources[i].source_kind == LARDON3D_ACQUISITION_SOURCE_RAW)
      pair.raw = i;
    else if (discovery->sources[i].source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG)
      pair.jpeg = i;
  }
  std::vector<Pair> selected;
  for (const auto &entry : pairs) {
    if (entry.second.raw == SIZE_MAX || entry.second.jpeg == SIZE_MAX) continue;
    selected.push_back(entry.second);
    if (selected.size() == kGroupsPerCamera) break;
  }
  if (selected.size() != kGroupsPerCamera) return false;
  std::vector<size_t> indices;
  indices.reserve(selected.size() * 2u);
  for (const Pair &pair : selected) {
    indices.push_back(pair.raw);
    indices.push_back(pair.jpeg);
  }
  std::sort(indices.begin(), indices.end());
  std::map<size_t, size_t> remap;
  for (size_t old_index : indices) {
    remap.emplace(old_index, campaign.sources.size());
    campaign.sources.push_back(discovery->sources[old_index]);
  }
  for (const Pair &pair : selected) {
    Lardon3DAcquisitionCampaignConfirmation confirmation{};
    confirmation.source_count = 2;
    confirmation.source_indices[0] = remap[pair.raw];
    confirmation.source_indices[1] = remap[pair.jpeg];
    campaign.confirmations.push_back(confirmation);
  }
  return campaign.confirmations.size() == kGroupsPerCamera;
}

bool discover_s21(const char *directory, Campaign &campaign) {
  Lardon3DAcquisitionCampaignRoot source_root{};
  if (!root(directory, source_root)) return false;
  auto discovery = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  if (lardon3d_acquisition_campaign_discover(&source_root, 1, discovery.get()) !=
      LARDON3D_ACQUISITION_CAMPAIGN_OK)
    return false;
  for (size_t i = 0; i < discovery->source_count &&
                     campaign.confirmations.size() < kGroupsPerCamera; ++i) {
    if (discovery->sources[i].source_kind != LARDON3D_ACQUISITION_SOURCE_JPEG) continue;
    Lardon3DAcquisitionCampaignConfirmation confirmation{};
    confirmation.source_count = 1;
    confirmation.source_indices[0] = campaign.sources.size();
    campaign.sources.push_back(discovery->sources[i]);
    campaign.confirmations.push_back(confirmation);
  }
  return campaign.confirmations.size() == kGroupsPerCamera;
}

bool wait_cursors_then_pause(Lardon3DAppState &state, uint64_t first_id, uint64_t second_id) {
  std::vector<unsigned char> first_request(LARDON3D_PHOTO_QUALITY_TASK_REQUEST_MAX_BYTES);
  bool first_requested = false;
  for (size_t attempt = 0; attempt < 10000; ++attempt) {
    Lardon3DProjectDbPhotoQualityTask first{};
    if (!first_requested &&
        lardon3d_project_db_load_photo_quality_task(
            state.project_db, first_id, first_request.data(), first_request.size(), &first) ==
            LARDON3D_PROJECT_DB_OK && first.next_group_id >= 2 &&
        first.next_group_id <= first.group_count) {
      first_requested = lardon3d_task_queue_pause(state.task_queue, first_id);
    }
    if (first_requested) {
      Lardon3DTaskSnapshot first_snapshot{}, second_snapshot{};
      if (lardon3d_task_queue_get(state.task_queue, first_id, &first_snapshot) &&
          lardon3d_task_queue_get(state.task_queue, second_id, &second_snapshot) &&
          first_snapshot.state == TASK_PAUSED && second_snapshot.state == TASK_PENDING)
        return true;
    }
    usleep(1000);
  }
  Lardon3DTaskSnapshot first_snapshot{}, second_snapshot{};
  Lardon3DProjectDbPhotoQualityTask first{};
  (void)lardon3d_task_queue_get(state.task_queue, first_id, &first_snapshot);
  (void)lardon3d_task_queue_get(state.task_queue, second_id, &second_snapshot);
  (void)lardon3d_project_db_load_photo_quality_task(
      state.project_db, first_id, first_request.data(), first_request.size(), &first);
  std::fprintf(stderr,
               "pause timeout: A state=%s cursor=%u requested=%d message=%s; "
               "S state=%s message=%s\n",
               lardon3d_task_state_name(first_snapshot.state), first.next_group_id,
               first_requested, first_snapshot.message,
               lardon3d_task_state_name(second_snapshot.state), second_snapshot.message);
  return false;
}

bool wait_completed(Lardon3DAppState &state, uint64_t task_id) {
  for (size_t attempt = 0; attempt < 180000; ++attempt) {
    Lardon3DTaskSnapshot snapshot{};
    if (lardon3d_task_queue_get(state.task_queue, task_id, &snapshot)) {
      if (snapshot.state == TASK_COMPLETED) return true;
      if (snapshot.state == TASK_FAILED || snapshot.state == TASK_CANCELLED) return false;
    }
    usleep(10000);
  }
  return false;
}

bool enqueue_campaign(Lardon3DAppState &state, uint64_t scanset_id,
                      const Campaign &campaign, uint64_t &task_id) {
  Lardon3DPhotoQualityTaskRequest request{
      campaign.sources.data(), campaign.sources.size(), campaign.confirmations.data(),
      campaign.confirmations.size()};
  Lardon3DAcquisitionCampaignPlan plan{};
  if (lardon3d_acquisition_campaign_plan(
          request.sources, request.source_count, request.confirmations,
          request.confirmation_count, &plan) != LARDON3D_ACQUISITION_CAMPAIGN_OK) {
    std::fprintf(stderr, "quality request planning failed\n");
    return false;
  }
  size_t encoded_size = 0;
  if (!lardon3d_photo_quality_request_encode(&request, nullptr, 0, &encoded_size)) {
    std::fprintf(stderr, "quality request encoding probe failed\n");
    return false;
  }
  Lardon3DTask *task =
      lardon3d_project_create_photo_quality_task(&state, scanset_id, &request, &task_id);
  if (!task) {
    std::fprintf(stderr, "quality durable Task creation failed: encoded=%zu\n", encoded_size);
    return false;
  }
  if (!lardon3d_task_queue_add(state.task_queue, task, nullptr)) {
    std::fprintf(stderr, "quality Queue transfer failed\n");
    lardon3d_task_destroy(task);
    return false;
  }
  return true;
}

int child_run(int output, const Campaign &a6000, const Campaign &s21) {
  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  if (!setup_runtime(state) || !lardon3d_project_create(&state, "Quality Restart")) return 10;
  Lardon3DProjectDbScanSet a6000_scanset{}, s21_scanset{};
  if (lardon3d_project_db_create_scanset(state.project_db, "A6000", &a6000_scanset) !=
          LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_create_scanset(state.project_db, "S21 FE", &s21_scanset) !=
          LARDON3D_PROJECT_DB_OK)
    return 11;
  TaskIds ids{};
  if (!enqueue_campaign(state, a6000_scanset.scanset_id, a6000, ids.a6000)) {
    std::fprintf(stderr, "A6000 quality enqueue failed: %s\n", state.status_message);
    return 12;
  }
  if (!enqueue_campaign(state, s21_scanset.scanset_id, s21, ids.s21)) {
    std::fprintf(stderr, "S21 quality enqueue failed: %s\n", state.status_message);
    return 12;
  }
  if (!wait_cursors_then_pause(state, ids.a6000, ids.s21))
    return 13;
  /* Overrides are operator decisions independent of measured metrics. They are
   * intentionally durable before this validation process dies. */
  if (lardon3d_project_db_set_photo_quality_override(
          state.project_db, ids.a6000, 1, LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE) !=
          LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_set_photo_quality_override(
          state.project_db, ids.a6000, 2, LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE) !=
          LARDON3D_PROJECT_DB_OK)
    return 14;
  if (write(output, &ids, sizeof(ids)) != static_cast<ssize_t>(sizeof(ids))) return 15;
  /* Deliberately bypass Task/Queue destruction. The parent must recover only
   * from production checkpoints and typed cursors, as after process loss. */
  return 0;
}

bool verify_override(Lardon3DProjectDb *database, uint64_t task_id, uint32_t group_id,
                     Lardon3DPhotoQualityOverride expected) {
  Lardon3DProjectDbPhotoQualityResult result{};
  return lardon3d_project_db_load_photo_quality_result(database, task_id, group_id, &result) ==
             LARDON3D_PROJECT_DB_OK &&
         /* Overrides are valid for every measured recommendation, including the
          * deliberate non-GOOD A6000 target used by this restart smoke test. */
         result.override_value == expected;
}

}  // namespace

extern "C" const Lardon3DTaskKindRegistry *lardon3d_task_kind_registry_production(void) {
  static const Lardon3DTaskKindDescriptor descriptors[] = {{
      LARDON3D_PHOTO_QUALITY_TASK_KIND, LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION,
      lardon3d_photo_quality_task_reconstruct}};
  static const Lardon3DTaskKindRegistry registry{descriptors, 1};
  return &registry;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "Usage: %s A6000_RAW_DIR A6000_JPEG_DIR S21_DIR\n", argv[0]);
    return 2;
  }
  Campaign a6000, s21;
  if (!discover_a6000(argv[1], argv[2], a6000) || !discover_s21(argv[3], s21)) {
    std::fprintf(stderr, "real corpus discovery or bounded grouping failed\n");
    return 1;
  }
  char project_root[] = "/tmp/lardon3d-quality-restart-XXXXXX";
  if (!mkdtemp(project_root) || setenv("LARDON3D_PROJECTS_ROOT", project_root, 1) != 0)
    return 1;
  int descriptors[2]{};
  if (pipe(descriptors) != 0) return 1;
  const pid_t child = fork();
  if (child < 0) return 1;
  if (child == 0) {
    close(descriptors[0]);
    const int result = child_run(descriptors[1], a6000, s21);
    close(descriptors[1]);
    _exit(result);
  }
  close(descriptors[1]);
  TaskIds ids{};
  const ssize_t read_size = read(descriptors[0], &ids, sizeof(ids));
  close(descriptors[0]);
  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      read_size != static_cast<ssize_t>(sizeof(ids))) {
    std::fprintf(stderr, "restart fixture child failed: status=%d read=%zd\n", status, read_size);
    return 1;
  }

  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  if (!setup_runtime(state) || !lardon3d_project_open(&state, "Quality Restart")) return 1;
  Lardon3DProjectRecoverySummary recovery{};
  if (!lardon3d_project_last_recovery_summary(&state, &recovery) || recovery.inspected != 2 ||
      recovery.resumed != 2 || recovery.skipped != 0 || recovery.failed != 0 ||
      !wait_completed(state, ids.a6000) || !wait_completed(state, ids.s21))
    return 1;
  if (lardon3d_project_db_set_photo_quality_override(
          state.project_db, ids.s21, 1, LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE) !=
          LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_set_photo_quality_override(
          state.project_db, ids.s21, 2, LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE) !=
          LARDON3D_PROJECT_DB_OK)
    return 1;
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = nullptr;
  lardon3d_project_close(&state);
  state.task_queue = lardon3d_task_queue_create(state.resource_governor, 4);
  if (!state.task_queue || !lardon3d_project_open(&state, "Quality Restart")) return 1;
  Lardon3DProjectRecoverySummary completed_reopen{};
  if (!lardon3d_project_last_recovery_summary(&state, &completed_reopen) ||
      completed_reopen.inspected != 0 || completed_reopen.resumed != 0)
    return 1;
  if (!verify_override(state.project_db, ids.a6000, 1,
                       LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE) ||
      !verify_override(state.project_db, ids.a6000, 2,
                       LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE) ||
      !verify_override(state.project_db, ids.s21, 1,
                       LARDON3D_PHOTO_QUALITY_OVERRIDE_EXCLUDE) ||
      !verify_override(state.project_db, ids.s21, 2,
                       LARDON3D_PHOTO_QUALITY_OVERRIDE_INCLUDE) ||
      lardon3d_resource_governor_reservation_count(state.resource_governor) != 0)
    return 1;

  std::printf("QUALITY_RESTART_RESULT=PASS A6000_GROUPS=%zu S21_GROUPS=%zu "
              "RECOVERY_INSPECTED=%zu RECOVERY_RESUMED=%zu INCLUDE_EXCLUDE=PERSISTED\n",
              a6000.confirmations.size(), s21.confirmations.size(), recovery.inspected,
              recovery.resumed);
  lardon3d_task_queue_destroy(state.task_queue);
  state.task_queue = nullptr;
  lardon3d_project_close(&state);
  lardon3d_resource_governor_destroy(state.resource_governor);
  std::error_code error;
  std::filesystem::remove_all(project_root, error);
  unsetenv("LARDON3D_PROJECTS_ROOT");
  return error ? 1 : 0;
}
