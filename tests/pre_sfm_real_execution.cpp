#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <openssl/evp.h>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <lardon3d/acquisition_campaign.h>
#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/candidate_pair_task.h>
#include <lardon3d/feature_extractor.h>
#include <lardon3d/feature_task.h>
#include <lardon3d/geometric_verifier_task.h>
#include <lardon3d/hardware_profile.h>
#include <lardon3d/matcher_task.h>
#include <lardon3d/photo_quality_task.h>
#include <lardon3d/project.h>
#include <lardon3d/raw_development_task.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/track_builder.h>
#include <lardon3d/track_builder_task.h>
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>
}

namespace {

enum class Mode { kA6000, kS21 };
enum class RestartBoundary { kNone, kRepresentations, kFeatures, kGeometry };

struct Options {
  Mode mode{};
  bool has_mode{};
  bool resume_geometry_existing{};
  bool resume_pre_gv_existing{};
  bool resume_candidate_existing{};
  unsigned int cpu_budget{};
  std::filesystem::path project_dir;
  std::vector<std::filesystem::path> roots;
  size_t limit{LARDON3D_VISUAL_INDEX_CANDIDATE_MAX};
  RestartBoundary restart{RestartBoundary::kNone};
};

struct Campaign {
  std::vector<Lardon3DAcquisitionCampaignSource> sources;
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations;
};

struct Runtime {
  Lardon3DAppState state{};
  std::string project_path;
  std::string database_path;
  unsigned int cpu_budget{};
};

struct Evidence {
  size_t selected{};
  size_t features{};
  size_t pairs{};
  size_t matches{};
  size_t verified{};
  size_t rejected{};
  size_t tracks{};
};

void usage(const char *program) {
  std::fprintf(
      stderr,
      "Usage: %s --mode a6000|s21 --project-dir ABSOLUTE_EMPTY_DIR "
      "--root ABSOLUTE_DIR [--root ABSOLUTE_DIR ...] [--limit 1..4096] "
      "[--restart-boundary representations|features|geometry]\n"
      "       %s --resume-geometry-existing --project-dir "
      "ABSOLUTE_EXISTING_DIR\n"
      "       %s --resume-pre-gv-existing --project-dir "
      "ABSOLUTE_EXISTING_DIR\n"
      "       %s --resume-candidate-existing --project-dir "
      "ABSOLUTE_EXISTING_DIR [--cpu-budget 1..12]\n",
      program,
      program,
      program,
      program);
}

bool parse_size(const char *text, size_t &value) {
  char *end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
      parsed > LARDON3D_VISUAL_INDEX_CANDIDATE_MAX)
    return false;
  value = static_cast<size_t>(parsed);
  return true;
}

bool parse_cpu_budget(const char *text, unsigned int &value) {
  size_t parsed = 0;
  if (!parse_size(text, parsed) || parsed > 12) return false;
  value = static_cast<unsigned int>(parsed);
  return true;
}

bool parse_options(int argc, char **argv, Options &options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--help") {
      usage(argv[0]);
      std::exit(0);
    }
    if (argument == "--resume-geometry-existing") {
      options.resume_geometry_existing = true;
      continue;
    }
    if (argument == "--resume-pre-gv-existing") {
      options.resume_pre_gv_existing = true;
      continue;
    }
    if (argument == "--resume-candidate-existing") {
      options.resume_candidate_existing = true;
      continue;
    }
    if ((argument == "--mode" || argument == "--project-dir" || argument == "--root" ||
         argument == "--limit" || argument == "--restart-boundary" ||
         argument == "--cpu-budget") &&
        index + 1 >= argc)
      return false;
    if (argument == "--mode") {
      const std::string value(argv[++index]);
      if (value == "a6000") options.mode = Mode::kA6000;
      else if (value == "s21") options.mode = Mode::kS21;
      else return false;
      options.has_mode = true;
    } else if (argument == "--project-dir") {
      options.project_dir = argv[++index];
    } else if (argument == "--root") {
      options.roots.emplace_back(argv[++index]);
    } else if (argument == "--limit") {
      if (!parse_size(argv[++index], options.limit)) return false;
    } else if (argument == "--restart-boundary") {
      const std::string value(argv[++index]);
      if (value == "representations") options.restart = RestartBoundary::kRepresentations;
      else if (value == "features") options.restart = RestartBoundary::kFeatures;
      else if (value == "geometry") options.restart = RestartBoundary::kGeometry;
      else return false;
    } else if (argument == "--cpu-budget") {
      if (!parse_cpu_budget(argv[++index], options.cpu_budget)) return false;
    } else {
      return false;
    }
  }
  const unsigned int resume_mode_count = options.resume_geometry_existing +
                                         options.resume_pre_gv_existing +
                                         options.resume_candidate_existing;
  if (resume_mode_count != 0)
    return !options.has_mode && !options.project_dir.empty() && options.roots.empty() &&
           options.restart == RestartBoundary::kNone &&
           resume_mode_count == 1 &&
           (options.resume_candidate_existing || options.cpu_budget == 0);
  if (options.cpu_budget != 0) return false;
  return options.has_mode && !options.project_dir.empty() && !options.roots.empty() &&
         options.roots.size() <= LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS;
}

bool prepare_existing_project(const std::filesystem::path &input, Runtime &runtime) {
  if (!input.is_absolute() || input.lexically_normal() != input) return false;
  std::error_code error;
  if (!std::filesystem::is_directory(input, error) || error ||
      !std::filesystem::is_regular_file(input / "project.lardon3d", error) || error ||
      !std::filesystem::is_directory(input / ".lardon3d" / "checkpoints", error) || error)
    return false;
  runtime.project_path = input.string();
  runtime.database_path = (input / "project.lardon3d").string();
  return runtime.project_path.size() < LARDON3D_APP_STATE_PATH_CAPACITY;
}

bool prepare_empty_project(const std::filesystem::path &input, std::string &path) {
  if (!input.is_absolute() || input.lexically_normal() != input) return false;
  std::error_code error;
  if (std::filesystem::exists(input, error)) {
    if (error || !std::filesystem::is_directory(input, error) || error ||
        !std::filesystem::is_empty(input, error) || error)
      return false;
  } else if (!std::filesystem::create_directories(input, error) || error) {
    return false;
  }
  path = input.string();
  if (path.size() >= LARDON3D_APP_STATE_PATH_CAPACITY) return false;
  return std::filesystem::create_directories(input / ".lardon3d" / "checkpoints", error) &&
         !error;
}

bool canonical_root(const std::filesystem::path &input, Lardon3DAcquisitionCampaignRoot &root) {
  if (!input.is_absolute()) return false;
  std::error_code error;
  const auto canonical = std::filesystem::canonical(input, error);
  if (error || !std::filesystem::is_directory(canonical, error) || error) return false;
  const std::string path = canonical.string();
  if (path.size() >= sizeof(root.path)) return false;
  std::memcpy(root.path, path.c_str(), path.size() + 1u);
  return true;
}

std::string source_stem(const char *path) {
  const char *name = std::strrchr(path, '/');
  name = name == nullptr ? path : name + 1;
  const char *dot = std::strrchr(name, '.');
  return std::string(name, dot == nullptr ? std::strlen(name)
                                          : static_cast<size_t>(dot - name));
}

bool discover_campaign(const Options &options, Campaign &campaign) {
  std::vector<Lardon3DAcquisitionCampaignRoot> roots(options.roots.size());
  for (size_t index = 0; index < roots.size(); ++index)
    if (!canonical_root(options.roots[index], roots[index])) {
      std::fprintf(stderr, "campaign root %zu is not an absolute readable directory: %s\n",
                   index, options.roots[index].c_str());
      return false;
    }
  auto discovery = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  const auto discovery_result =
      lardon3d_acquisition_campaign_discover(roots.data(), roots.size(), discovery.get());
  if (discovery_result != LARDON3D_ACQUISITION_CAMPAIGN_OK) {
    std::fprintf(stderr, "campaign discovery failed with result %d\n",
                 static_cast<int>(discovery_result));
    return false;
  }

  std::vector<std::vector<size_t>> groups;
  if (options.mode == Mode::kS21) {
    for (size_t index = 0; index < discovery->source_count && groups.size() < options.limit;
         ++index) {
      if (discovery->sources[index].source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG)
        groups.push_back({index});
    }
  } else {
    struct Pair { size_t raw{SIZE_MAX}; size_t jpeg{SIZE_MAX}; };
    std::map<std::string, Pair> pairs;
    for (size_t index = 0; index < discovery->source_count; ++index) {
      Pair &pair = pairs[source_stem(discovery->sources[index].path)];
      if (discovery->sources[index].source_kind == LARDON3D_ACQUISITION_SOURCE_RAW) {
        if (pair.raw != SIZE_MAX) {
          std::fprintf(stderr, "duplicate RAW stem in discovery: %s\n",
                       source_stem(discovery->sources[index].path).c_str());
          return false;
        }
        pair.raw = index;
      } else if (discovery->sources[index].source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG) {
        if (pair.jpeg != SIZE_MAX) {
          std::fprintf(stderr, "duplicate JPEG stem in discovery: %s\n",
                       source_stem(discovery->sources[index].path).c_str());
          return false;
        }
        pair.jpeg = index;
      }
    }
    for (const auto &entry : pairs) {
      if (entry.second.raw == SIZE_MAX || entry.second.jpeg == SIZE_MAX) {
        std::fprintf(stderr, "unpaired A6000 stem: %s (%s missing)\n", entry.first.c_str(),
                     entry.second.raw == SIZE_MAX ? "RAW" : "JPEG");
        return false;
      }
      groups.push_back({entry.second.raw, entry.second.jpeg});
      if (groups.size() == options.limit) break;
    }
  }
  if (groups.empty()) {
    std::fprintf(stderr, "campaign discovery found no usable groups among %zu sources\n",
                 discovery->source_count);
    return false;
  }

  std::vector<size_t> selected_indices;
  for (const auto &group : groups)
    selected_indices.insert(selected_indices.end(), group.begin(), group.end());
  std::sort(selected_indices.begin(), selected_indices.end());
  std::vector<size_t> compact_index(discovery->source_count, SIZE_MAX);
  for (const size_t discovery_index : selected_indices) {
    compact_index[discovery_index] = campaign.sources.size();
    campaign.sources.push_back(discovery->sources[discovery_index]);
  }

  /* Filename stems are used only to form explicit CALLER_EXPLICIT input before
   * Captures exist. The compact source array retains discovery's global canonical
   * path order, while confirmations map each pair independently of that order.
   * Once S3-E returns, every phase below follows durable group, Capture, Asset and
   * image IDs and never treats this convenience key as identity. */
  for (const auto &group : groups) {
    Lardon3DAcquisitionCampaignConfirmation confirmation{};
    confirmation.source_count = group.size();
    for (size_t member = 0; member < group.size(); ++member)
      confirmation.source_indices[member] = compact_index[group[member]];
    campaign.confirmations.push_back(confirmation);
  }
  auto plan = std::make_unique<Lardon3DAcquisitionCampaignPlan>();
  const auto plan_result = lardon3d_acquisition_campaign_plan(
      campaign.sources.data(), campaign.sources.size(), campaign.confirmations.data(),
      campaign.confirmations.size(), plan.get());
  if (plan_result != LARDON3D_ACQUISITION_CAMPAIGN_OK ||
      plan->group_count != campaign.confirmations.size()) {
    std::fprintf(stderr,
                 "explicit campaign plan rejected: result=%d sources=%zu confirmations=%zu "
                 "groups=%zu\n",
                 static_cast<int>(plan_result), campaign.sources.size(),
                 campaign.confirmations.size(), plan->group_count);
    return false;
  }
  return true;
}

void stop_runtime(Runtime &runtime) {
  if (runtime.state.task_queue) lardon3d_task_queue_destroy(runtime.state.task_queue);
  if (runtime.state.resource_governor)
    lardon3d_resource_governor_destroy(runtime.state.resource_governor);
  if (runtime.state.project_db) lardon3d_project_db_close(runtime.state.project_db);
  runtime.state.task_queue = nullptr;
  runtime.state.resource_governor = nullptr;
  runtime.state.project_db = nullptr;
}

bool start_runtime(Runtime &runtime) {
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  lardon3d_app_state_init(&runtime.state);
  if (lardon3d_project_db_open(runtime.database_path.c_str(), &runtime.state.project_db, error) !=
      LARDON3D_PROJECT_DB_OK) {
    std::fprintf(stderr, "project DB open failed: %s\n", error);
    return false;
  }
  char hardware_error[256]{};
  Lardon3DResourcePolicy policy{};
  if (!lardon3d_hardware_profile_detect(&runtime.state.hardware_profile, hardware_error,
                                        sizeof(hardware_error)) ||
      !lardon3d_resource_policy_default(&runtime.state.hardware_profile, &policy)) {
    std::fprintf(stderr, "hardware policy failed: %s\n", hardware_error);
    stop_runtime(runtime);
    return false;
  }
  if (runtime.cpu_budget != 0) {
    if (runtime.cpu_budget > runtime.state.hardware_profile.logical_cpu_count) {
      std::fprintf(stderr, "requested CPU budget %u exceeds logical CPU count %u\n",
                   runtime.cpu_budget, runtime.state.hardware_profile.logical_cpu_count);
      stop_runtime(runtime);
      return false;
    }
    // This runner-only override is applied before Governor creation and is never
    // persisted. The Governor remains the sole admission owner: reserving N
    // logical CPUs constrains the recovered Candidate Task's execution contract.
    policy.system_cpu_reserve = runtime.state.hardware_profile.logical_cpu_count -
                                runtime.cpu_budget;
  }
  unsigned int feature_threads = runtime.state.hardware_profile.logical_cpu_count -
                                 policy.system_cpu_reserve;
  if (feature_threads > 12) feature_threads = 12;
  /* Match the production startup contract: OpenCV's process-wide pool is
   * configured from the same detected profile and Governor policy before any
   * Queue worker exists. The runner-only CPU override therefore constrains
   * both admission and nested feature extraction consistently. */
  if (!lardon3d_feature_opencv_configure_threads(feature_threads)) {
    std::fprintf(stderr, "OpenCV thread configuration failed for %u threads\n",
                 feature_threads);
    stop_runtime(runtime);
    return false;
  }
  runtime.state.resource_governor =
      lardon3d_resource_governor_create(&runtime.state.hardware_profile, &policy);
  runtime.state.task_queue = runtime.state.resource_governor
                                 ? lardon3d_task_queue_create(runtime.state.resource_governor, 2)
                                 : nullptr;
  if (!runtime.state.task_queue) {
    stop_runtime(runtime);
    return false;
  }
  runtime.state.project_loaded = true;
  std::memcpy(runtime.state.project_path, runtime.project_path.c_str(),
              runtime.project_path.size() + 1u);
  /* Task owns execution state, Queue owns bounded dispatch/backpressure, and
   * Governor owns each task's admission reservation. Sequential enqueue/wait
   * below releases that reservation at the task boundary and never reserves an
   * entire campaign or creates a parallel worker/scheduler subsystem. */
  return true;
}

bool wait_completed(Runtime &runtime, uint64_t task_id, const char *phase) {
  for (;;) {
    Lardon3DTaskSnapshot snapshot{};
    if (!lardon3d_task_queue_get(runtime.state.task_queue, task_id, &snapshot)) return false;
    if (snapshot.state == TASK_COMPLETED || snapshot.state == TASK_FAILED ||
        snapshot.state == TASK_CANCELLED) {
      Lardon3DProjectDbTask durable{};
      if (lardon3d_project_db_load_task(runtime.state.project_db, task_id, &durable) ==
              LARDON3D_PROJECT_DB_OK &&
          durable.saved_state == snapshot.state) {
        if (snapshot.state != TASK_COMPLETED) {
          std::fprintf(stderr, "%s task %llu ended in %s: %s\n", phase,
                       static_cast<unsigned long long>(task_id),
                       lardon3d_task_state_name(snapshot.state), snapshot.message);
          return false;
        }
        return lardon3d_task_queue_remove(runtime.state.task_queue, task_id);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool restart_runtime(Runtime &runtime, uint64_t execution_id, const char *boundary) {
  stop_runtime(runtime);
  if (!start_runtime(runtime)) return false;
  Lardon3DProjectDbSelectedExecution execution{};
  if (lardon3d_project_db_load_selected_execution(runtime.state.project_db, execution_id,
                                                   &execution) != LARDON3D_PROJECT_DB_OK) {
    std::fprintf(stderr, "restart at %s lost retained execution %llu\n", boundary,
                 static_cast<unsigned long long>(execution_id));
    return false;
  }
  /* This boundary deliberately has no sidecar or durable top-level phase. The
   * caller retains only execution_id; the next phase reconstructs exact image,
   * FeatureSet or result IDs from v22 durable records and their cursors. */
  std::printf("{\"record\":\"restart\",\"kind\":\"REOPEN_ONLY\","
              "\"boundary\":\"%s\",\"execution_id\":%llu}\n",
              boundary, static_cast<unsigned long long>(execution_id));
  return true;
}

bool recover_quality_task(Runtime &runtime, uint64_t task_id) {
  /* Durable Task creation publishes its typed request and initial cursor before
   * Queue transfer. The original unqueued object has been discarded to model
   * loss before first dispatch. Recovery must use the existing registry to
   * reconstruct callback state, then transfer the Task through Queue and
   * Governor admission; the runner adds no top-level durable state, test hook,
   * or alternate worker path. */
  stop_runtime(runtime);
  if (!start_runtime(runtime)) return false;
  Lardon3DProjectRecoverySummary recovery{};
  if (lardon3d_project_resume_recoverable_tasks(
          &runtime.state, lardon3d_task_kind_registry_production(), &recovery) !=
          LARDON3D_PROJECT_DB_OK ||
      recovery.resumed != 1 || recovery.failed != 0)
    return false;
  std::printf("{\"record\":\"restart\","
              "\"kind\":\"REGISTRY_QUEUE_GOVERNOR_RESTART\","
              "\"task_id\":%llu,\"inspected\":%zu,\"resumed\":%zu}\n",
              static_cast<unsigned long long>(task_id), recovery.inspected, recovery.resumed);
  return wait_completed(runtime, task_id, "photo_quality.triage recovered");
}

bool run_task_pair(Runtime &runtime, uint64_t scanset_id, const Campaign &campaign,
                   bool prove_restart, uint64_t &quality_task_id, uint64_t &campaign_task_id) {
  Lardon3DPhotoQualityTaskRequest quality_request{
      campaign.sources.data(), campaign.sources.size(), campaign.confirmations.data(),
      campaign.confirmations.size()};
  if (prove_restart) {
    Lardon3DTask *pending = lardon3d_project_create_photo_quality_task(
        &runtime.state, scanset_id, &quality_request, &quality_task_id);
    if (!pending) return false;
    lardon3d_task_destroy(pending);
    if (!recover_quality_task(runtime, quality_task_id)) {
      std::fprintf(stderr, "quality Task could not be restored at its pending boundary\n");
      return false;
    }
  } else if (!lardon3d_project_enqueue_photo_quality(&runtime.state, scanset_id, &quality_request,
                                                      &quality_task_id) ||
             !wait_completed(runtime, quality_task_id, "photo_quality.triage")) {
    return false;
  }
  Lardon3DAcquisitionCampaignTaskRequest campaign_request{};
  campaign_request.sources = campaign.sources.data();
  campaign_request.source_count = campaign.sources.size();
  campaign_request.confirmations = campaign.confirmations.data();
  campaign_request.confirmation_count = campaign.confirmations.size();
  campaign_request.ingest_options.representation = LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE;
  campaign_request.ingest_options.select_representation = 1;
  campaign_request.ingest_options.imported_at = std::time(nullptr);
  campaign_request.ingest_options.max_source_bytes = UINT64_MAX;
  return lardon3d_project_enqueue_acquisition_campaign(
             &runtime.state, scanset_id, &campaign_request, &campaign_task_id) &&
         wait_completed(runtime, campaign_task_id, "acquisition_campaign.run");
}

bool create_selection(Runtime &runtime, Mode mode, uint64_t quality_task_id,
                      uint64_t campaign_task_id, size_t group_count,
                      Lardon3DProjectDbSelectedExecution &execution) {
  std::vector<Lardon3DProjectDbSelectedExecutionItem> items;
  for (size_t index = 0; index < group_count; ++index) {
    const uint32_t group_id = static_cast<uint32_t>(index + 1u);
    Lardon3DProjectDbPhotoQualityResult quality{};
    Lardon3DProjectDbAcquisitionCampaignCapture campaign{};
    if (lardon3d_project_db_load_photo_quality_result(runtime.state.project_db, quality_task_id,
                                                       group_id, &quality) !=
            LARDON3D_PROJECT_DB_OK ||
        lardon3d_project_db_load_acquisition_campaign_capture(
            runtime.state.project_db, campaign_task_id, group_id, &campaign) !=
            LARDON3D_PROJECT_DB_OK)
      return false;
    if (quality.metrics.recommendation != LARDON3D_PHOTO_QUALITY_GOOD ||
        quality.override_value != LARDON3D_PHOTO_QUALITY_OVERRIDE_NONE)
      continue;
    Lardon3DProjectDbSelectedExecutionItem item{};
    item.item_index = static_cast<uint32_t>(items.size());
    item.quality_group_id = group_id;
    item.campaign_group_id = group_id;
    item.capture_id = campaign.capture_id;
    if (mode == Mode::kA6000) {
      Lardon3DProjectDbCaptureSourceAsset sources[4]{};
      size_t source_count = 0;
      if (lardon3d_project_db_list_capture_source_assets(runtime.state.project_db,
                                                         campaign.capture_id, 0, sources, 4,
                                                         &source_count) !=
          LARDON3D_PROJECT_DB_OK)
        return false;
      size_t raw_count = 0;
      for (size_t source = 0; source < source_count; ++source) {
        if (sources[source].source_kind == LARDON3D_DB_CAPTURE_SOURCE_RAW) {
          item.source_asset_id = sources[source].asset_id;
          ++raw_count;
        }
      }
      if (raw_count != 1) return false;
      item.representation_source = LARDON3D_SELECTED_REPRESENTATION_RAW_ASSET;
    } else {
      item.representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE;
    }
    items.push_back(item);
  }
  if (items.empty()) {
    std::printf("{\"record\":\"selection\",\"selected\":0,"
                "\"reason\":\"no GOOD+NONE groups\"}\n");
    return true;
  }
  /* These item records are the only bridge between independent quality and
   * campaign group namespaces. Equal numeric IDs are supplied explicitly and
   * never used to infer Capture, Asset, or image identity. */
  return lardon3d_project_db_create_selected_execution(
             runtime.state.project_db, quality_task_id, campaign_task_id, items.data(),
             items.size(), std::time(nullptr), &execution) == LARDON3D_PROJECT_DB_OK;
}

bool publish_representations(Runtime &runtime, uint64_t execution_id,
                             std::vector<uint64_t> &image_ids) {
  Lardon3DProjectDbSelectedExecution execution{};
  if (lardon3d_project_db_load_selected_execution(runtime.state.project_db, execution_id,
                                                   &execution) != LARDON3D_PROJECT_DB_OK)
    return false;
  for (uint32_t index = execution.next_item_index; index < execution.item_count; ++index) {
    Lardon3DProjectDbSelectedExecutionItem item{};
    if (lardon3d_project_db_load_selected_execution_item(runtime.state.project_db, execution_id,
                                                          index, &item) != LARDON3D_PROJECT_DB_OK)
      return false;
    uint64_t image_id = 0;
    if (item.representation_source == LARDON3D_SELECTED_REPRESENTATION_RAW_ASSET) {
      uint64_t task_id = 0;
      if (!lardon3d_project_enqueue_raw_development(&runtime.state, item.capture_id,
                                                     item.source_asset_id, &task_id) ||
          !wait_completed(runtime, task_id, "raw.develop"))
        return false;
      Lardon3DProjectDbRawDevelopmentTask raw{};
      if (lardon3d_project_db_load_raw_development_task(runtime.state.project_db, task_id, &raw) !=
              LARDON3D_PROJECT_DB_OK ||
          raw.phase != LARDON3D_RAW_DEVELOPMENT_TASK_PUBLISHED || !raw.has_image ||
          raw.capture_id != item.capture_id || raw.source_asset_id != item.source_asset_id)
        return false;
      image_id = raw.image_id;
    } else {
      if (lardon3d_project_db_get_selected_capture_image(runtime.state.project_db, item.capture_id,
                                                          &image_id) != LARDON3D_PROJECT_DB_OK)
        return false;
    }
    if (lardon3d_project_db_record_selected_representation(runtime.state.project_db, execution_id,
                                                            index, image_id, index + 1u) !=
        LARDON3D_PROJECT_DB_OK)
      return false;
  }
  if (lardon3d_project_db_load_selected_execution(runtime.state.project_db, execution_id,
                                                   &execution) != LARDON3D_PROJECT_DB_OK ||
      execution.stage != LARDON3D_SELECTED_EXECUTION_CALIBRATION)
    return false;
  image_ids.clear();
  for (uint32_t index = 0; index < execution.item_count; ++index) {
    Lardon3DProjectDbSelectedExecutionItem item{};
    if (lardon3d_project_db_load_selected_execution_item(runtime.state.project_db, execution_id,
                                                          index, &item) != LARDON3D_PROJECT_DB_OK ||
        !item.has_image)
      return false;
    image_ids.push_back(item.image_id);
  }
  return true;
}

bool extract_features(Runtime &runtime, const std::vector<uint64_t> &image_ids,
                      const Lardon3DFeatureExtractorParameters &parameters,
                      std::vector<Lardon3DProjectDbFeatureSet> &feature_sets) {
  unsigned char fingerprint[32]{};
  lardon3d_feature_extractor_parameter_fingerprint(&parameters, fingerprint);
  feature_sets.clear();
  for (const uint64_t image_id : image_ids) {
    Lardon3DProjectDbFeatureSet feature{};
    if (lardon3d_project_db_find_feature_set(runtime.state.project_db, image_id,
                                             LARDON3D_FEATURE_EXTRACTOR_KIND,
                                             LARDON3D_FEATURE_EXTRACTOR_VERSION, fingerprint,
                                             &feature) != LARDON3D_PROJECT_DB_OK) {
      uint64_t task_id = 0;
      if (!lardon3d_project_enqueue_feature_extract(&runtime.state, image_id, &parameters,
                                                     &task_id) ||
          !wait_completed(runtime, task_id, "features.extract") ||
          lardon3d_project_db_find_feature_set(
              runtime.state.project_db, image_id, LARDON3D_FEATURE_EXTRACTOR_KIND,
              LARDON3D_FEATURE_EXTRACTOR_VERSION, fingerprint, &feature) !=
              LARDON3D_PROJECT_DB_OK)
        return false;
    }
    feature_sets.push_back(feature);
  }
  return true;
}

bool collect_verified_ids(Runtime &runtime, const unsigned char verifier_fingerprint[32],
                          std::vector<uint64_t> &verified_ids,
                          size_t *applicable = nullptr, size_t *verified = nullptr,
                          size_t *rejected = nullptr) {
  verified_ids.clear();
  if (applicable) *applicable = 0;
  if (verified) *verified = 0;
  if (rejected) *rejected = 0;
  uint64_t match_cursor = 0;
  for (;;) {
    Lardon3DProjectDbMatchResult matches[64]{};
    size_t match_count = 0;
    if (lardon3d_project_db_list_match_results(runtime.state.project_db, match_cursor, matches, 64,
                                                &match_count) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < match_count; ++index) {
      match_cursor = matches[index].match_result_id;
      if (matches[index].result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED ||
          matches[index].match_count == 0)
        continue;
      if (applicable) ++*applicable;
      Lardon3DProjectDbGeometricVerificationResult result{};
      if (lardon3d_project_db_find_geometric_verification_result(
              runtime.state.project_db, matches[index].match_result_id,
              LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, LARDON3D_GEOMETRIC_VERIFIER_VERSION,
              verifier_fingerprint, &result) != LARDON3D_PROJECT_DB_OK)
        return false;
      if (result.status == LARDON3D_GEOMETRIC_VERIFIED) {
        if (verified) ++*verified;
        verified_ids.push_back(result.geometric_verification_result_id);
      } else if (result.status == LARDON3D_GEOMETRIC_REJECTED) {
        if (rejected) ++*rejected;
      } else {
        return false;
      }
    }
    if (match_count < 64) break;
  }
  std::sort(verified_ids.begin(), verified_ids.end());
  return std::adjacent_find(verified_ids.begin(), verified_ids.end()) == verified_ids.end();
}

bool downstream(Runtime &runtime, const std::vector<Lardon3DProjectDbFeatureSet> &feature_sets,
                const Lardon3DFeatureExtractorParameters &orb, uint64_t &geometry_task_id,
                std::vector<uint64_t> &verified_ids) {
  Lardon3DVisualIndexConfiguration index_configuration{
      LARDON3D_VISUAL_INDEX_VERSION, 1024, 256};
  uint64_t visual_index_id = 0;
  if (feature_sets.empty() ||
      lardon3d_visual_index_create(runtime.state.project_db, &feature_sets.front(),
                                   &index_configuration, &visual_index_id) !=
          LARDON3D_VISUAL_INDEX_OK)
    return false;
  uint64_t task_id = 0;
  if (!lardon3d_project_enqueue_visual_index_update(&runtime.state, visual_index_id, &task_id) ||
      !wait_completed(runtime, task_id, "visual_index.update"))
    return false;

  Lardon3DVisualIndexQueryOptions query{
      std::min<uint32_t>(64, static_cast<uint32_t>(feature_sets.size())), 1,
      LARDON3D_VISUAL_INDEX_SAME_SCANSET, true};
  if (!lardon3d_project_enqueue_candidate_pair_generate(&runtime.state, visual_index_id, &query,
                                                         &task_id) ||
      !wait_completed(runtime, task_id, "candidate_pair.generate"))
    return false;
  Lardon3DMatcherTaskConfiguration matcher{};
  std::snprintf(matcher.feature_extractor_kind, sizeof(matcher.feature_extractor_kind), "%s",
                LARDON3D_FEATURE_EXTRACTOR_KIND);
  matcher.feature_extractor_version = LARDON3D_FEATURE_EXTRACTOR_VERSION;
  lardon3d_feature_extractor_parameter_fingerprint(&orb, matcher.feature_parameter_fingerprint);
  matcher.matcher.kind = LARDON3D_MATCHER_ORB_BF;
  matcher.matcher.ratio_threshold = lardon3d_matcher_default_ratio(LARDON3D_MATCHER_ORB_BF);
  if (!lardon3d_project_enqueue_matcher_task(&runtime.state, &matcher, &task_id) ||
      !wait_completed(runtime, task_id, "matcher.run"))
    return false;
  Lardon3DGeometricVerifierTaskConfiguration verifier{
      lardon3d_geometric_verifier_default_parameters()};
  if (!lardon3d_project_enqueue_geometric_verifier_task(&runtime.state, &verifier,
                                                         &geometry_task_id) ||
      !wait_completed(runtime, geometry_task_id, "geometric_verifier.run"))
    return false;

  unsigned char verifier_fingerprint[32]{};
  lardon3d_geometric_verifier_fingerprint(&verifier.verifier, verifier_fingerprint);
  return collect_verified_ids(runtime, verifier_fingerprint, verified_ids);
}

bool build_tracks(Runtime &runtime, const std::vector<uint64_t> &verified_ids,
                  const unsigned char verifier_fingerprint[32],
                  uint64_t *task_id_output = nullptr,
                  Lardon3DProjectDbTrackSet *track_set_output = nullptr) {
  if (task_id_output) *task_id_output = 0;
  if (track_set_output) *track_set_output = {};
  if (verified_ids.empty()) return true;
  if (verified_ids.size() > (std::numeric_limits<size_t>::max() - 8U) / 8U) return false;
  std::vector<unsigned char> scope_bytes(8U + verified_ids.size() * 8U);
  std::memcpy(scope_bytes.data(), "L3DTSIS1", 8);
  for (size_t index = 0; index < verified_ids.size(); ++index) {
    uint64_t value = verified_ids[index];
    for (size_t byte = 0; byte < 8; ++byte) {
      scope_bytes[8U + index * 8U + byte] = static_cast<unsigned char>(value & 0xffU);
      value >>= 8U;
    }
  }
  Lardon3DProjectDbTrackSet exact{};
  std::snprintf(exact.builder_kind, sizeof(exact.builder_kind), "track_builder");
  exact.builder_version = LARDON3D_TRACK_BUILDER_VERSION;
  unsigned int digest_size = 0;
  if (!lardon3d_track_builder_fingerprint(exact.parameter_fingerprint) ||
      EVP_Digest(scope_bytes.data(), scope_bytes.size(), exact.input_scope_hash, &digest_size,
                 EVP_sha256(), nullptr) != 1 ||
      digest_size != 32)
    return false;
  exact.verifier_kind = LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL;
  exact.verifier_version = LARDON3D_GEOMETRIC_VERIFIER_VERSION;
  std::memcpy(exact.verifier_fingerprint, verifier_fingerprint, 32);
  exact.gvr_count = verified_ids.size();
  Lardon3DProjectDbTrackSet existing{};
  const Lardon3DProjectDbResult found =
      lardon3d_project_db_find_track_set(runtime.state.project_db, &exact, &existing);
  if (found == LARDON3D_PROJECT_DB_OK) {
    if (track_set_output) *track_set_output = existing;
    return true;
  }
  if (found != LARDON3D_PROJECT_DB_NOT_FOUND) return false;
  Lardon3DTrackBuilderTaskConfiguration configuration{
      runtime.project_path.c_str(), runtime.state.project_db,
      LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL, LARDON3D_GEOMETRIC_VERIFIER_VERSION,
      verifier_fingerprint, verified_ids.data(), verified_ids.size()};
  uint64_t task_id = 0;
  if (!lardon3d_project_enqueue_track_builder_task(&runtime.state, &configuration, &task_id) ||
      !wait_completed(runtime, task_id, "track_builder.run"))
    return false;
  Lardon3DProjectDbTrackBuilderTask durable{};
  Lardon3DProjectDbTrackSet identity{};
  if (lardon3d_project_db_load_track_builder_task(runtime.state.project_db, task_id, &durable) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  std::memcpy(identity.builder_kind, durable.builder_kind, sizeof(identity.builder_kind));
  identity.builder_version = durable.builder_version;
  std::memcpy(identity.parameter_fingerprint, durable.builder_fingerprint,
              sizeof(identity.parameter_fingerprint));
  identity.verifier_kind = durable.verifier_kind;
  identity.verifier_version = durable.verifier_version;
  std::memcpy(identity.verifier_fingerprint, durable.verifier_fingerprint,
              sizeof(identity.verifier_fingerprint));
  std::memcpy(identity.input_scope_hash, durable.input_scope_hash,
              sizeof(identity.input_scope_hash));
  identity.gvr_count = durable.gvr_count;
  Lardon3DProjectDbTrackSet published{};
  if (lardon3d_project_db_find_track_set(runtime.state.project_db, &identity, &published) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  if (task_id_output) *task_id_output = task_id;
  if (track_set_output) *track_set_output = published;
  return true;
}

bool collect_evidence(Runtime &runtime, uint64_t execution_id, Evidence &evidence) {
  Lardon3DProjectDbSelectedExecution execution{};
  if (execution_id != 0 &&
      lardon3d_project_db_load_selected_execution(runtime.state.project_db, execution_id,
                                                   &execution) == LARDON3D_PROJECT_DB_OK)
    evidence.selected = execution.item_count;
  uint64_t cursor = 0;
  for (;;) {
    Lardon3DProjectDbFeatureSet page[64]{};
    size_t count = 0;
    if (lardon3d_project_db_list_feature_sets(runtime.state.project_db, cursor, page, 64, &count) !=
        LARDON3D_PROJECT_DB_OK)
      return false;
    evidence.features += count;
    if (count) cursor = page[count - 1].feature_set_id;
    if (count < 64) break;
  }
  cursor = 0;
  for (;;) {
    Lardon3DProjectDbCandidatePair page[64]{};
    size_t count = 0;
    if (lardon3d_project_db_list_candidate_pairs(runtime.state.project_db, cursor, page, 64,
                                                  &count) != LARDON3D_PROJECT_DB_OK)
      return false;
    evidence.pairs += count;
    if (count) cursor = page[count - 1].candidate_pair_id;
    if (count < 64) break;
  }
  cursor = 0;
  for (;;) {
    Lardon3DProjectDbMatchResult page[64]{};
    size_t count = 0;
    if (lardon3d_project_db_list_match_results(runtime.state.project_db, cursor, page, 64,
                                                &count) != LARDON3D_PROJECT_DB_OK)
      return false;
    evidence.matches += count;
    for (size_t index = 0; index < count; ++index) {
      Lardon3DProjectDbGeometricVerificationResult geometries[4]{};
      size_t geometry_count = 0;
      if (lardon3d_project_db_list_geometric_verification_results(
              runtime.state.project_db, page[index].match_result_id, 0, geometries, 4,
              &geometry_count) != LARDON3D_PROJECT_DB_OK)
        return false;
      for (size_t geometry = 0; geometry < geometry_count; ++geometry) {
        if (geometries[geometry].status == LARDON3D_GEOMETRIC_VERIFIED) ++evidence.verified;
        else ++evidence.rejected;
      }
    }
    if (count) cursor = page[count - 1].match_result_id;
    if (count < 64) break;
  }
  Lardon3DProjectDbTrackSet tracks[8]{};
  size_t track_set_count = 0;
  if (lardon3d_project_db_list_track_sets(runtime.state.project_db, 0, tracks, 8,
                                           &track_set_count) != LARDON3D_PROJECT_DB_OK)
    return false;
  for (size_t index = 0; index < track_set_count; ++index) evidence.tracks += tracks[index].track_count;
  return true;
}

bool count_exact_gvrs(Runtime &runtime, const unsigned char fingerprint[32],
                      size_t &count) {
  count = 0;
  uint64_t match_cursor = 0;
  for (;;) {
    Lardon3DProjectDbMatchResult matches[64]{};
    size_t match_count = 0;
    if (lardon3d_project_db_list_match_results(runtime.state.project_db, match_cursor, matches, 64,
                                                &match_count) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < match_count; ++index) {
      match_cursor = matches[index].match_result_id;
      uint64_t geometry_cursor = 0;
      for (;;) {
        Lardon3DProjectDbGeometricVerificationResult page[8]{};
        size_t geometry_count = 0;
        if (lardon3d_project_db_list_geometric_verification_results(
                runtime.state.project_db, matches[index].match_result_id, geometry_cursor, page, 8,
                &geometry_count) != LARDON3D_PROJECT_DB_OK)
          return false;
        for (size_t geometry = 0; geometry < geometry_count; ++geometry) {
          if (page[geometry].verifier_kind == LARDON3D_GEOMETRIC_VERIFIER_FUNDAMENTAL &&
              page[geometry].verifier_version == LARDON3D_GEOMETRIC_VERIFIER_VERSION &&
              std::memcmp(page[geometry].parameter_fingerprint, fingerprint, 32) == 0)
            ++count;
          geometry_cursor = page[geometry].geometric_verification_result_id;
        }
        if (geometry_count < 8) break;
      }
    }
    if (match_count < 64) break;
  }
  return true;
}

bool find_pending_pre_gv_task(Runtime &runtime, uint64_t &candidate_task_id,
                              uint64_t &matcher_task_id) {
  candidate_task_id = 0;
  matcher_task_id = 0;
  uint64_t cursor = 0;
  for (;;) {
    Lardon3DProjectRecoveryEntry entries[8]{};
    size_t count = 0;
    if (lardon3d_project_list_recoverable(
            &runtime.state, lardon3d_task_kind_registry_production(), cursor,
            entries, 8, &count) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < count; ++index) {
      const auto &entry = entries[index];
      cursor = entry.task_id;
      if ((entry.status != LARDON3D_PROJECT_RECOVERABLE &&
           entry.status !=
               LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE) ||
          entry.snapshot.recovery_state != TASK_PENDING) {
        std::fprintf(stderr,
                     "pending task %llu is not safely recoverable (status=%d)\n",
                     static_cast<unsigned long long>(entry.task_id),
                     static_cast<int>(entry.status));
        return false;
      }
      if (std::strcmp(entry.task_kind,
                      LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND) == 0) {
        if (candidate_task_id != 0) return false;
        candidate_task_id = entry.task_id;
      } else if (std::strcmp(entry.task_kind, LARDON3D_MATCHER_TASK_KIND) == 0) {
        if (matcher_task_id != 0) return false;
        matcher_task_id = entry.task_id;
      } else {
        // The production recovery API resumes every pending task. Refuse an
        // unrelated task rather than replaying an earlier pipeline stage.
        std::fprintf(stderr,
                     "PRE-GV recovery refuses pending task %llu of kind %s\n",
                     static_cast<unsigned long long>(entry.task_id),
                     entry.task_kind);
        return false;
      }
    }
    if (count < 8) break;
  }
  return !(candidate_task_id != 0 && matcher_task_id != 0);
}

bool recover_one_pre_gv_task(Runtime &runtime, uint64_t task_id,
                             const char *phase,
                             Lardon3DProjectRecoverySummary &recovery) {
  recovery = {};
  if (lardon3d_project_resume_recoverable_tasks(
          &runtime.state, lardon3d_task_kind_registry_production(),
          &recovery) != LARDON3D_PROJECT_DB_OK ||
      recovery.resumed != 1 || recovery.failed != 0 || recovery.queue_full)
    return false;
  return wait_completed(runtime, task_id, phase);
}

bool find_pending_candidate_only_task(Runtime &runtime, uint64_t &candidate_task_id) {
  candidate_task_id = 0;
  uint64_t cursor = 0;
  for (;;) {
    Lardon3DProjectRecoveryEntry entries[8]{};
    size_t count = 0;
    if (lardon3d_project_list_recoverable(
            &runtime.state, lardon3d_task_kind_registry_production(), cursor,
            entries, 8, &count) != LARDON3D_PROJECT_DB_OK)
      return false;
    for (size_t index = 0; index < count; ++index) {
      const auto &entry = entries[index];
      cursor = entry.task_id;
      if ((entry.status != LARDON3D_PROJECT_RECOVERABLE &&
           entry.status != LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE) ||
          entry.snapshot.recovery_state != TASK_PENDING ||
          std::strcmp(entry.task_kind,
                      LARDON3D_CANDIDATE_PAIR_GENERATE_TASK_KIND) != 0 ||
          candidate_task_id != 0) {
        std::fprintf(stderr,
                     "candidate-only recovery refuses unsafe pending task %llu of kind %s\n",
                     static_cast<unsigned long long>(entry.task_id), entry.task_kind);
        return false;
      }
      candidate_task_id = entry.task_id;
    }
    if (count < 8) break;
  }
  // An absent entry is intentionally rejected: it means the requested pending
  // Candidate boundary is missing or already complete, neither of which this
  // repair mode may reinterpret or replace.
  return candidate_task_id != 0;
}

// This repair route owns exactly one durable Candidate Task. Registry recovery
// enqueues through the production Queue and Governor, but the preflight makes
// that all-task API safe by admitting no Matcher, GV, Track, SfM, or prior work.
bool run_existing_candidate(Runtime &runtime) {
  if (!start_runtime(runtime)) return false;
  Evidence before{};
  uint64_t candidate_task_id = 0;
  if (!collect_evidence(runtime, 0, before) ||
      !find_pending_candidate_only_task(runtime, candidate_task_id)) {
    stop_runtime(runtime);
    return false;
  }
  Lardon3DProjectRecoverySummary recovery{};
  const bool recovered = recover_one_pre_gv_task(
      runtime, candidate_task_id, "candidate_pair.generate recovered", recovery);
  Evidence after{};
  const bool ok = recovered && collect_evidence(runtime, 0, after) &&
                  after.features == before.features && after.pairs >= before.pairs &&
                  after.matches == before.matches && after.verified == before.verified &&
                  after.rejected == before.rejected && after.tracks == before.tracks;
  std::printf(
      "{\"record\":\"existing_candidate_summary\",\"ok\":%s,"
      "\"candidate_task_id\":%llu,\"candidate_action\":\"recovered\","
      "\"candidate_recovery_inspected\":%zu,\"candidate_recovery_resumed\":%zu,"
      "\"cpu_budget\":%u,\"feature_sets_before\":%zu,\"feature_sets_after\":%zu,"
      "\"candidate_pairs_before\":%zu,\"candidate_pairs_after\":%zu,"
      "\"match_results_before\":%zu,\"match_results_after\":%zu,"
      "\"gvrs_before\":%zu,\"gvrs_after\":%zu,\"tracks_before\":%zu,"
      "\"tracks_after\":%zu,\"prior_stages_replayed\":false,"
      "\"matcher_enqueued\":false,\"gv_enqueued\":false,"
      "\"track_builder_enqueued\":false,\"sparse_sfm_run\":false}\n",
      ok ? "true" : "false", static_cast<unsigned long long>(candidate_task_id),
      recovery.inspected, recovery.resumed, runtime.cpu_budget, before.features,
      after.features, before.pairs, after.pairs, before.matches, after.matches,
      before.verified + before.rejected, after.verified + after.rejected,
      before.tracks, after.tracks);
  stop_runtime(runtime);
  return ok;
}

// This repair route owns only the already-durable Candidate/Matcher boundary.
// It may recover those production Tasks or enqueue the missing Matcher, but it
// must never reconstruct prior acquisition stages or cross into GV/Tracks/SfM.
bool run_existing_pre_gv(Runtime &runtime) {
  if (!start_runtime(runtime)) return false;
  Evidence before{};
  if (!collect_evidence(runtime, 0, before) || before.verified != 0 ||
      before.rejected != 0 || before.tracks != 0) {
    std::fprintf(stderr,
                 "PRE-GV mode requires a project with no GV or Track evidence\n");
    stop_runtime(runtime);
    return false;
  }

  uint64_t candidate_task_id = 0;
  uint64_t pending_matcher_task_id = 0;
  if (!find_pending_pre_gv_task(runtime, candidate_task_id,
                                pending_matcher_task_id)) {
    stop_runtime(runtime);
    return false;
  }
  Lardon3DProjectRecoverySummary candidate_recovery{};
  bool candidate_recovered = false;
  if (candidate_task_id != 0) {
    if (!recover_one_pre_gv_task(runtime, candidate_task_id,
                                 "candidate_pair.generate recovered",
                                 candidate_recovery)) {
      stop_runtime(runtime);
      return false;
    }
    candidate_recovered = true;
  }

  Evidence after_candidate{};
  if (!collect_evidence(runtime, 0, after_candidate) ||
      after_candidate.matches > after_candidate.pairs ||
      after_candidate.verified != before.verified ||
      after_candidate.rejected != before.rejected ||
      after_candidate.tracks != before.tracks) {
    stop_runtime(runtime);
    return false;
  }

  uint64_t newly_pending_candidate = 0;
  uint64_t matcher_task_id = 0;
  if (!find_pending_pre_gv_task(runtime, newly_pending_candidate,
                                matcher_task_id) ||
      newly_pending_candidate != 0) {
    stop_runtime(runtime);
    return false;
  }
  if (pending_matcher_task_id != 0) matcher_task_id = pending_matcher_task_id;

  Lardon3DProjectRecoverySummary matcher_recovery{};
  const char *matcher_action = "already_complete";
  if (after_candidate.matches < after_candidate.pairs) {
    if (matcher_task_id != 0) {
      if (!recover_one_pre_gv_task(runtime, matcher_task_id,
                                   "matcher.run recovered",
                                   matcher_recovery)) {
        stop_runtime(runtime);
        return false;
      }
      matcher_action = "recovered";
    } else {
      const Lardon3DFeatureExtractorParameters orb{8192, 8, 20};
      Lardon3DMatcherTaskConfiguration matcher{};
      std::snprintf(matcher.feature_extractor_kind,
                    sizeof(matcher.feature_extractor_kind), "%s",
                    LARDON3D_FEATURE_EXTRACTOR_KIND);
      matcher.feature_extractor_version = LARDON3D_FEATURE_EXTRACTOR_VERSION;
      lardon3d_feature_extractor_parameter_fingerprint(
          &orb, matcher.feature_parameter_fingerprint);
      matcher.matcher.kind = LARDON3D_MATCHER_ORB_BF;
      matcher.matcher.ratio_threshold =
          lardon3d_matcher_default_ratio(LARDON3D_MATCHER_ORB_BF);
      if (!lardon3d_project_enqueue_matcher_task(&runtime.state, &matcher,
                                                  &matcher_task_id) ||
          !wait_completed(runtime, matcher_task_id, "matcher.run existing")) {
        stop_runtime(runtime);
        return false;
      }
      matcher_action = "enqueued";
    }
  } else if (matcher_task_id != 0) {
    std::fprintf(stderr,
                 "complete Match Results coexist with a pending Matcher task\n");
    stop_runtime(runtime);
    return false;
  }

  Evidence after{};
  const bool ok = collect_evidence(runtime, 0, after) &&
                  after.features == before.features &&
                  after.pairs >= before.pairs && after.matches == after.pairs &&
                  after.verified == before.verified &&
                  after.rejected == before.rejected &&
                  after.tracks == before.tracks;
  std::printf(
      "{\"record\":\"existing_pre_gv_summary\",\"ok\":%s,"
      "\"candidate_task_id\":%llu,\"candidate_action\":\"%s\","
      "\"candidate_recovery_inspected\":%zu,"
      "\"candidate_recovery_resumed\":%zu,\"matcher_task_id\":%llu,"
      "\"matcher_action\":\"%s\",\"matcher_recovery_resumed\":%zu,"
      "\"feature_sets_before\":%zu,\"feature_sets_after\":%zu,"
      "\"candidate_pairs_before\":%zu,\"candidate_pairs_after\":%zu,"
      "\"match_results_before\":%zu,\"match_results_after\":%zu,"
      "\"gvrs_before\":%zu,\"gvrs_after\":%zu,"
      "\"tracks_before\":%zu,\"tracks_after\":%zu,"
      "\"prior_stages_replayed\":false,\"gv_enqueued\":false,"
      "\"track_builder_enqueued\":false,\"sparse_sfm_run\":false}\n",
      ok ? "true" : "false",
      static_cast<unsigned long long>(candidate_task_id),
      candidate_recovered ? "recovered" : "already_complete",
      candidate_recovery.inspected, candidate_recovery.resumed,
      static_cast<unsigned long long>(matcher_task_id), matcher_action,
      matcher_recovery.resumed, before.features, after.features, before.pairs,
      after.pairs, before.matches, after.matches,
      before.verified + before.rejected, after.verified + after.rejected,
      before.tracks, after.tracks);
  stop_runtime(runtime);
  return ok;
}

bool run_existing_geometry(Runtime &runtime) {
  if (!start_runtime(runtime)) return false;
  Lardon3DGeometricVerifierTaskConfiguration configuration{
      lardon3d_geometric_verifier_default_parameters()};
  unsigned char fingerprint[32]{};
  lardon3d_geometric_verifier_fingerprint(&configuration.verifier, fingerprint);
  size_t exact_before = 0;
  if (!count_exact_gvrs(runtime, fingerprint, exact_before)) {
    stop_runtime(runtime);
    return false;
  }

  /* This route deliberately starts at the immutable Match Result boundary.
   * It enqueues no quality, campaign, representation, feature, index,
   * candidate, or Matcher work. Task/Queue/Governor retain their production
   * ownership, and exact current identity reuse makes a full rerun convergent. */
  uint64_t geometry_task_id = 0;
  if (!lardon3d_project_enqueue_geometric_verifier_task(
          &runtime.state, &configuration, &geometry_task_id) ||
      !wait_completed(runtime, geometry_task_id,
                      "geometric_verifier.run current existing")) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"geometric_verifier.run\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }

  std::vector<uint64_t> verified_ids;
  size_t applicable = 0;
  size_t verified = 0;
  size_t rejected = 0;
  size_t exact_after = 0;
  if (!collect_verified_ids(runtime, fingerprint, verified_ids, &applicable, &verified,
                            &rejected) ||
      !count_exact_gvrs(runtime, fingerprint, exact_after) || exact_after != applicable) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"current_evidence_audit\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }

  // Track Builder receives only exact current-policy VERIFIED IDs. Historical
  // v1/v2 evidence is neither replayed nor mixed, and the exact scope identity
  // reuses an existing Track Set instead of publishing duplicate lineage.
  uint64_t track_task_id = 0;
  Lardon3DProjectDbTrackSet track_set{};
  if (!build_tracks(runtime, verified_ids, fingerprint, &track_task_id, &track_set)) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"track_builder.run\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }

  const size_t created = exact_after >= exact_before ? exact_after - exact_before : 0;
  std::printf(
      "{\"record\":\"existing_geometry_summary\",\"ok\":true,"
      "\"verifier_version\":%u,\"geometry_task_id\":%llu,"
      "\"track_task_id\":%llu,\"applicable_matched_parents\":%zu,"
      "\"verified\":%zu,\"rejected\":%zu,\"gvrs_before\":%zu,"
      "\"gvrs_created\":%zu,\"gvrs_after\":%zu,"
      "\"track_set_id\":%llu,\"track_count\":%llu,"
      "\"track_set_reused\":%s,"
      "\"task_errors\":0,\"upstream_replayed\":false,"
      "\"mixed_verifier_lineage\":false,\"sparse_sfm_run\":false}\n",
      LARDON3D_GEOMETRIC_VERIFIER_VERSION,
      static_cast<unsigned long long>(geometry_task_id),
      static_cast<unsigned long long>(track_task_id), applicable, verified, rejected, exact_before,
      created, exact_after, static_cast<unsigned long long>(track_set.track_set_id),
      static_cast<unsigned long long>(track_set.track_count),
      track_task_id == 0 && track_set.track_set_id != 0 ? "true" : "false");
  stop_runtime(runtime);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage(argv[0]);
    return 2;
  }
  Runtime runtime;
  if (options.resume_geometry_existing || options.resume_pre_gv_existing ||
      options.resume_candidate_existing) {
    if (!prepare_existing_project(options.project_dir, runtime)) {
      std::fprintf(stderr, "existing project must be absolute, normalized, and durable\n");
      return 2;
    }
    runtime.cpu_budget = options.cpu_budget;
    if (options.resume_candidate_existing)
      return run_existing_candidate(runtime) ? 0 : 1;
    return (options.resume_pre_gv_existing ? run_existing_pre_gv(runtime)
                                           : run_existing_geometry(runtime)) ? 0 : 1;
  }
  if (!prepare_empty_project(options.project_dir, runtime.project_path)) {
    std::fprintf(stderr, "project directory must be absolute, normalized and empty\n");
    return 2;
  }
  runtime.database_path = (options.project_dir / "project.lardon3d").string();
  Campaign campaign;
  if (!discover_campaign(options, campaign) || !start_runtime(runtime)) {
    std::fprintf(stderr, "campaign discovery or runtime setup failed\n");
    return 1;
  }

  const char *mode_name = options.mode == Mode::kA6000 ? "a6000" : "s21";
  Lardon3DProjectDbScanSet scanset{};
  if (lardon3d_project_db_create_scanset(runtime.state.project_db, mode_name, &scanset) !=
      LARDON3D_PROJECT_DB_OK) {
    stop_runtime(runtime);
    return 1;
  }
  uint64_t quality_task_id = 0;
  uint64_t campaign_task_id = 0;
  Lardon3DProjectDbSelectedExecution execution{};
  bool ok = run_task_pair(runtime, scanset.scanset_id, campaign,
                          options.restart != RestartBoundary::kNone, quality_task_id,
                          campaign_task_id) &&
            create_selection(runtime, options.mode, quality_task_id, campaign_task_id,
                             campaign.confirmations.size(), execution);
  if (!ok || execution.execution_id == 0) {
    stop_runtime(runtime);
    return ok ? 0 : 1;
  }

  std::vector<uint64_t> image_ids;
  ok = publish_representations(runtime, execution.execution_id, image_ids);
  if (ok && options.restart == RestartBoundary::kRepresentations)
    ok = restart_runtime(runtime, execution.execution_id, "representations") &&
         publish_representations(runtime, execution.execution_id, image_ids);

  /* ORB is run only for exact images retained by the selected execution. The
   * 4096 option limit is an operational Visual Index v1 bound, not a scientific
   * campaign-size rule; callers use one empty project per campaign. */
  const Lardon3DFeatureExtractorParameters orb{8192, 8, 20};
  std::vector<Lardon3DProjectDbFeatureSet> features;
  if (ok) ok = extract_features(runtime, image_ids, orb, features);
  if (ok && options.restart == RestartBoundary::kFeatures) {
    ok = restart_runtime(runtime, execution.execution_id, "features") &&
         publish_representations(runtime, execution.execution_id, image_ids) &&
         extract_features(runtime, image_ids, orb, features);
  }

  uint64_t geometry_task_id = 0;
  std::vector<uint64_t> verified_ids;
  Lardon3DGeometricVerifierParameters verifier = lardon3d_geometric_verifier_default_parameters();
  unsigned char verifier_fingerprint[32]{};
  lardon3d_geometric_verifier_fingerprint(&verifier, verifier_fingerprint);
  if (ok) ok = downstream(runtime, features, orb, geometry_task_id, verified_ids);
  if (ok && options.restart == RestartBoundary::kGeometry) {
    ok = restart_runtime(runtime, execution.execution_id, "geometry");
    if (ok) ok = collect_verified_ids(runtime, verifier_fingerprint, verified_ids);
  }
  if (ok) ok = build_tracks(runtime, verified_ids, verifier_fingerprint);
  if (ok && verified_ids.empty())
    std::printf("{\"record\":\"tracks\",\"created\":false,"
                "\"reason\":\"empty verified GVR set\"}\n");

  /* Real A6000/S21 calibration is unavailable. The selected execution remains
   * truthfully at CALIBRATION, and this runner never fabricates a scope or runs
   * sparse SfM; verified geometry and tracks are the terminal evidence. */
  if (ok) {
    stop_runtime(runtime);
    ok = start_runtime(runtime);
  }
  Evidence evidence{};
  if (ok) ok = collect_evidence(runtime, execution.execution_id, evidence);
  if (ok) {
    std::printf("{\"record\":\"summary\",\"mode\":\"%s\","
                "\"execution_id\":%llu,\"selected\":%zu,\"feature_sets\":%zu,"
                "\"candidate_pairs\":%zu,\"matches\":%zu,\"verified\":%zu,"
                "\"rejected\":%zu,\"tracks\":%zu,"
                "\"orb_max_features_per_set\":8192,"
                "\"visual_index_max_features_per_set\":1024,"
                "\"visual_index_feature_set_limit\":4096,"
                "\"candidate_top_k_limit\":64,"
                "\"calibration_attached\":false,\"sparse_sfm_run\":false}\n",
                mode_name, static_cast<unsigned long long>(execution.execution_id),
                evidence.selected, evidence.features, evidence.pairs, evidence.matches,
                evidence.verified, evidence.rejected, evidence.tracks);
  }
  stop_runtime(runtime);
  return ok ? 0 : 1;
}
