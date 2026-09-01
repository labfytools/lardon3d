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
#include <sqlite3.h>
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
#include <lardon3d/match_file.h>
#include <lardon3d/matcher_task.h>
#include <lardon3d/orb_vulkan_backend.h>
#include <lardon3d/photo_quality_task.h>
#include <lardon3d/project.h>
#include <lardon3d/raw_development_task.h>
#include <lardon3d/task_queue.h>
#include <lardon3d/track_builder.h>
#include <lardon3d/track_builder_task.h>
#include <lardon3d/visual_index.h>
#include <lardon3d/visual_index_task.h>

#include "../src/matcher_task_benchmark_internal.h"
#include "../src/orb_vulkan_backend_internal.h"
#include "../src/resource_governor_internal.h"
}

namespace {

enum class Mode { kA6000, kS21 };
enum class MatcherMode { kAuto, kCpu, kVulkan };
enum class MatcherPipeline { kRolling, kSynchronous };
enum class RestartBoundary { kNone, kRepresentations, kFeatures, kGeometry };

struct Options {
  Mode mode{};
  bool has_mode{};
  bool resume_geometry_existing{};
  bool resume_pre_gv_existing{};
  bool resume_candidate_existing{};
  unsigned int cpu_budget{};
  unsigned int gpu_budget{};
  bool has_gpu_budget{};
  MatcherMode matcher_mode{MatcherMode::kAuto};
  bool has_matcher_mode{};
  MatcherPipeline matcher_pipeline{MatcherPipeline::kRolling};
  bool has_matcher_pipeline{};
  unsigned int matcher_inflight_override{};
  bool has_matcher_inflight_override{};
  unsigned int matcher_batch_override{};
  bool has_matcher_batch_override{};
  bool stop_after_matcher{};
  bool stop_after_gv{};
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
  unsigned int gpu_budget{};
  bool has_gpu_budget{};
  MatcherMode matcher_mode{MatcherMode::kAuto};
  MatcherPipeline matcher_pipeline{MatcherPipeline::kRolling};
  unsigned int matcher_inflight_override{};
  unsigned int matcher_batch_override{};
  bool matcher_needed{};
  bool stop_after_matcher{};
  bool stop_after_gv{};
  bool matcher_evidence_active{};
  uint64_t matcher_diagnostic_serial{};
  uint64_t matcher_diagnostic_samples{};
  std::chrono::steady_clock::time_point matcher_wall_begin{};
  bool matcher_backend_before_known{};
  Lardon3DOrbVulkanTelemetry matcher_backend_before{};
  /* Runner-owned fixed scalars retain extrema from coalesced Governor
   * diagnostics. They never add per-sequence history, production state, or a
   * scientific acceptance input; `diagnostic_samples` states the sampling
   * limitation explicitly in the evidence record. */
  bool geometric_evidence_active{};
  uint64_t geometric_diagnostic_serial{};
  uint64_t geometric_diagnostic_samples{};
  std::chrono::steady_clock::time_point geometric_wall_begin{};
  bool geometric_memory_available_known{};
  uint64_t geometric_minimum_memory_available_bytes{};
  bool geometric_memory_psi_some_known{};
  uint32_t geometric_maximum_memory_psi_some_basis_points{};
  bool geometric_memory_psi_full_known{};
  uint32_t geometric_maximum_memory_psi_full_basis_points{};
  bool geometric_io_psi_some_known{};
  uint32_t geometric_maximum_io_psi_some_basis_points{};
  bool geometric_io_psi_full_known{};
  uint32_t geometric_maximum_io_psi_full_basis_points{};
  bool geometric_swap_delta_known{};
  uint64_t geometric_maximum_swap_pages_in_delta{};
  uint64_t geometric_maximum_swap_pages_out_delta{};
  bool geometric_process_rss_known{};
  uint64_t geometric_maximum_process_rss_bytes{};
  bool geometric_process_peak_rss_known{};
  uint64_t geometric_maximum_process_peak_rss_bytes{};
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

struct MatchAudit {
  char digest_hex[65]{};
  size_t match_result_count{};
  size_t match_asset_count{};
  size_t duplicate_candidate_pair_mappings{};
  bool candidate_mapping_contiguous{};
  bool matcher_cursor_complete{};
};

void usage(const char *program) {
  std::fprintf(
      stderr,
      "Usage: %s --mode a6000|s21 --project-dir ABSOLUTE_EMPTY_DIR "
      "--root ABSOLUTE_DIR [--root ABSOLUTE_DIR ...] [--limit 1..4096] "
      "[--restart-boundary representations|features|geometry]\n"
      "       %s --resume-geometry-existing --project-dir ABSOLUTE_EXISTING_DIR "
      "[--stop-after-gv]\n"
      "       %s --resume-pre-gv-existing --project-dir "
      "ABSOLUTE_EXISTING_DIR [--cpu-budget 1..12] [--gpu-budget 0..1] "
      "[--matcher-mode auto|cpu|vulkan] "
      "[--matcher-pipeline rolling|synchronous] [--matcher-inflight 1|2] "
      "[--matcher-batch 2|4|8|12] "
      "[--stop-after-matcher]\n"
      "       %s --resume-candidate-existing --project-dir "
      "ABSOLUTE_EXISTING_DIR [--cpu-budget 1..12] [--gpu-budget 0..1]\n",
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

bool parse_gpu_budget(const char *text, unsigned int &value) {
  char *end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed > 1) return false;
  value = static_cast<unsigned int>(parsed);
  return true;
}

bool parse_matcher_inflight(const char *text, unsigned int &value) {
  if (!text || (std::strcmp(text, "1") != 0 && std::strcmp(text, "2") != 0))
    return false;
  value = text[0] == '1' ? 1U : 2U;
  return true;
}

bool parse_matcher_batch(const char *text, unsigned int &value) {
  if (!text) return false;
  if (std::strcmp(text, "2") == 0) value = 2;
  else if (std::strcmp(text, "4") == 0) value = 4;
  else if (std::strcmp(text, "8") == 0) value = 8;
  else if (std::strcmp(text, "12") == 0) value = 12;
  else return false;
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
         argument == "--cpu-budget" || argument == "--gpu-budget" ||
         argument == "--matcher-mode" || argument == "--matcher-pipeline" ||
         argument == "--matcher-inflight" || argument == "--matcher-batch") &&
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
    } else if (argument == "--gpu-budget") {
      if (!parse_gpu_budget(argv[++index], options.gpu_budget)) return false;
      options.has_gpu_budget = true;
    } else if (argument == "--matcher-mode") {
      const std::string value(argv[++index]);
      if (value == "auto") options.matcher_mode = MatcherMode::kAuto;
      else if (value == "cpu") options.matcher_mode = MatcherMode::kCpu;
      else if (value == "vulkan") options.matcher_mode = MatcherMode::kVulkan;
      else return false;
      options.has_matcher_mode = true;
    } else if (argument == "--matcher-pipeline") {
      const std::string value(argv[++index]);
      if (value == "rolling") options.matcher_pipeline = MatcherPipeline::kRolling;
      else if (value == "synchronous")
        options.matcher_pipeline = MatcherPipeline::kSynchronous;
      else return false;
      options.has_matcher_pipeline = true;
    } else if (argument == "--matcher-inflight") {
      if (!parse_matcher_inflight(
              argv[++index], options.matcher_inflight_override)) return false;
      options.has_matcher_inflight_override = true;
    } else if (argument == "--matcher-batch") {
      if (!parse_matcher_batch(argv[++index], options.matcher_batch_override))
        return false;
      options.has_matcher_batch_override = true;
    } else if (argument == "--stop-after-matcher") {
      options.stop_after_matcher = true;
    } else if (argument == "--stop-after-gv") {
      options.stop_after_gv = true;
    } else {
      return false;
    }
  }
  const unsigned int resume_mode_count = options.resume_geometry_existing +
                                         options.resume_pre_gv_existing +
                                         options.resume_candidate_existing;
  if (resume_mode_count != 0) {
    if (options.has_mode || options.project_dir.empty() || !options.roots.empty() ||
        options.restart != RestartBoundary::kNone || resume_mode_count != 1)
      return false;
    if (options.resume_pre_gv_existing) {
      return !options.stop_after_gv;
    }
    if (options.resume_candidate_existing)
      return !options.has_matcher_mode && !options.has_matcher_pipeline &&
             !options.has_matcher_inflight_override &&
             !options.has_matcher_batch_override &&
             !options.stop_after_matcher && !options.stop_after_gv;
    return options.cpu_budget == 0 && !options.has_gpu_budget &&
           !options.has_matcher_mode && !options.has_matcher_pipeline &&
           !options.has_matcher_inflight_override &&
           !options.has_matcher_batch_override &&
           !options.stop_after_matcher;
  }
  if (options.cpu_budget != 0 || options.has_gpu_budget ||
      options.has_matcher_mode || options.has_matcher_pipeline ||
      options.has_matcher_inflight_override ||
      options.has_matcher_batch_override ||
      options.stop_after_matcher || options.stop_after_gv) return false;
  return options.has_mode && !options.project_dir.empty() && !options.roots.empty() &&
         options.roots.size() <= LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS;
}

class ScopedEnvironmentValue {
 public:
  explicit ScopedEnvironmentValue(const char *name) : name_(name) {
    const char *value = std::getenv(name_);
    existed_ = value != nullptr;
    if (value) value_ = value;
  }

  ScopedEnvironmentValue(const ScopedEnvironmentValue &) = delete;
  ScopedEnvironmentValue &operator=(const ScopedEnvironmentValue &) = delete;

  bool replace(const char *value) {
    changed_ = true;
    return value ? setenv(name_, value, 1) == 0 : unsetenv(name_) == 0;
  }

  ~ScopedEnvironmentValue() {
    if (!changed_) return;
    /* Process-owned benchmark controls must not leak into a later in-process
     * invocation. POSIX setenv/unsetenv are used only before runtime threads;
     * destruction is the all-exit restoration boundary and never throws. */
    if (existed_) (void)setenv(name_, value_.c_str(), 1);
    else (void)unsetenv(name_);
  }

 private:
  const char *name_;
  bool existed_{};
  bool changed_{};
  std::string value_;
};

class ScopedMatcherBenchmarkEnvironment {
 public:
  ScopedMatcherBenchmarkEnvironment()
      : pipeline_(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV),
        inflight_(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV),
        batch_(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) {}

  bool configure(const Options &options) {
    const char *pipeline = options.matcher_pipeline ==
            MatcherPipeline::kSynchronous ? "1" : nullptr;
    const char *inflight = !options.has_matcher_inflight_override
        ? nullptr : options.matcher_inflight_override == 1 ? "1" : "2";
    const char *batch = !options.has_matcher_batch_override
        ? nullptr : options.matcher_batch_override == 2 ? "2"
            : options.matcher_batch_override == 4 ? "4"
            : options.matcher_batch_override == 8 ? "8" : "12";
    return pipeline_.replace(pipeline) && inflight_.replace(inflight) &&
           batch_.replace(batch);
  }

 private:
  ScopedEnvironmentValue pipeline_;
  ScopedEnvironmentValue inflight_;
  ScopedEnvironmentValue batch_;
};

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
  if (runtime.state.orb_vulkan_backend)
    lardon3d_orb_vulkan_backend_destroy(runtime.state.orb_vulkan_backend);
  if (runtime.state.project_db) lardon3d_project_db_close(runtime.state.project_db);
  runtime.state.task_queue = nullptr;
  runtime.state.resource_governor = nullptr;
  runtime.state.project_db = nullptr;
  runtime.state.orb_vulkan_backend = nullptr;
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
  if (runtime.has_gpu_budget) {
    if (runtime.gpu_budget != 0 && !runtime.state.hardware_profile.gpu_available) {
      std::fprintf(stderr, "requested GPU budget requires a detected GPU\n");
      stop_runtime(runtime);
      return false;
    }
    policy.gpu_slot_capacity = runtime.gpu_budget;
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
  /* AUTO and explicit Vulkan receive only an uninitialized backend object.
   * Metadata inspection here cannot start driver threads: first initialization
   * remains on Queue's affinity-constrained worker. Portable AUTO receives the
   * null stub result and exposes only CPU without a GPU side effect. */
  if (runtime.matcher_needed && runtime.matcher_mode != MatcherMode::kCpu)
    runtime.state.orb_vulkan_backend = lardon3d_orb_vulkan_backend_create();
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

const char *backend_name(Lardon3DResourceBackend backend) {
  switch (backend) {
    case LARDON3D_RESOURCE_BACKEND_FIXED: return "fixed";
    case LARDON3D_RESOURCE_BACKEND_CPU: return "cpu";
    case LARDON3D_RESOURCE_BACKEND_ORB_VULKAN: return "orb-vulkan";
    case LARDON3D_RESOURCE_BACKEND_MIXED: return "mixed";
  }
  return "invalid";
}

const char *pressure_name(Lardon3DResourcePressure pressure) {
  switch (pressure) {
    case LARDON3D_RESOURCE_PRESSURE_GREEN: return "green";
    case LARDON3D_RESOURCE_PRESSURE_YELLOW: return "yellow";
    case LARDON3D_RESOURCE_PRESSURE_RED: return "red";
  }
  return "invalid";
}

void print_json_string(const char *text) {
  std::putchar('"');
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(text ? text : "");
       *cursor; ++cursor) {
    switch (*cursor) {
      case '"': std::fputs("\\\"", stdout); break;
      case '\\': std::fputs("\\\\", stdout); break;
      case '\b': std::fputs("\\b", stdout); break;
      case '\f': std::fputs("\\f", stdout); break;
      case '\n': std::fputs("\\n", stdout); break;
      case '\r': std::fputs("\\r", stdout); break;
      case '\t': std::fputs("\\t", stdout); break;
      default:
        if (*cursor < 0x20)
          std::printf("\\u%04x", static_cast<unsigned int>(*cursor));
        else
          std::putchar(*cursor);
    }
  }
  std::putchar('"');
}

void print_known_u64(bool known, uint64_t value) {
  if (known) std::printf("%llu", static_cast<unsigned long long>(value));
  else std::fputs("null", stdout);
}

void print_known_u32(bool known, uint32_t value) {
  if (known) std::printf("%u", value);
  else std::fputs("null", stdout);
}

void emit_matcher_diagnostic_change(Runtime &runtime) {
  if (!runtime.matcher_evidence_active || !runtime.state.resource_governor) return;
  Lardon3DResourceSequenceDiagnostic diagnostic{};
  if (!lardon3d_resource_governor_internal_diagnostic_since(
          runtime.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, runtime.matcher_diagnostic_serial,
          &diagnostic))
    return;
  runtime.matcher_diagnostic_serial = diagnostic.serial;
  ++runtime.matcher_diagnostic_samples;
  std::printf(
      "{\"record\":\"matcher_diagnostic_sample\",\"sampling\":"
      "\"latest-change-coalescing\",\"serial\":%llu,\"selected_backend\":\"%s\","
      "\"actual_backend\":\"%s\",\"fallback\":%s,\"pressure\":\"%s\","
      "\"cpu\":%u,\"gpu\":%u,\"batch\":%zu,\"inflight\":%zu,"
      "\"helpers\":%u,\"io\":%u,\"host_memory_bytes\":%llu,"
      "\"gpu_memory_bytes\":%llu,\"uma\":%s,\"wall_ns\":%llu,"
      "\"items\":%zu,\"durable_rate_milli\":%llu,\"mem_available_bytes\":"
      , static_cast<unsigned long long>(diagnostic.serial),
      backend_name(diagnostic.backend), backend_name(diagnostic.actual_backend),
      diagnostic.backend_fallback ? "true" : "false",
      pressure_name(diagnostic.pressure), diagnostic.cpu_threads,
      diagnostic.gpu_slots, diagnostic.batch_size, diagnostic.inflight_limit,
      diagnostic.helper_limit, diagnostic.io_slots,
      static_cast<unsigned long long>(diagnostic.memory_bytes),
      static_cast<unsigned long long>(diagnostic.gpu_memory_bytes),
      runtime.state.hardware_profile.gpu_uses_shared_memory ? "true" : "false",
      static_cast<unsigned long long>(diagnostic.previous_wall_time_ns),
      diagnostic.items_completed,
      static_cast<unsigned long long>(diagnostic.durable_items_per_second_milli));
  print_known_u64(diagnostic.host.memory_available_known,
                  diagnostic.host.memory_available_bytes);
  std::fputs(",\"memory_psi_some_basis_points\":", stdout);
  print_known_u32(diagnostic.host.memory_psi_some_known,
                  diagnostic.host.memory_psi_some_basis_points);
  std::fputs(",\"memory_psi_full_basis_points\":", stdout);
  print_known_u32(diagnostic.host.memory_psi_full_known,
                  diagnostic.host.memory_psi_full_basis_points);
  std::fputs(",\"io_psi_some_basis_points\":", stdout);
  print_known_u32(diagnostic.host.io_psi_some_known,
                  diagnostic.host.io_psi_some_basis_points);
  std::fputs(",\"io_psi_full_basis_points\":", stdout);
  print_known_u32(diagnostic.host.io_psi_full_known,
                  diagnostic.host.io_psi_full_basis_points);
  std::printf(
      ",\"swap_delta_known\":%s,\"swap_pages_in_delta\":%llu,"
      "\"swap_pages_out_delta\":%llu,\"compute_pool_utilization_basis_points\":",
      diagnostic.host.swap_delta_known ? "true" : "false",
      static_cast<unsigned long long>(diagnostic.host.swap_pages_in_delta),
      static_cast<unsigned long long>(diagnostic.host.swap_pages_out_delta));
  print_known_u32(diagnostic.host.compute_pool_utilization_known,
                  diagnostic.host.compute_pool_utilization_basis_points);
  std::fputs(",\"gpu_busy_basis_points\":", stdout);
  print_known_u32(diagnostic.host.gpu_busy_known,
                  diagnostic.host.gpu_busy_basis_points);
  std::fputs(",\"process_rss_bytes\":", stdout);
  print_known_u64(diagnostic.host.process_rss_known,
                  diagnostic.host.process_rss_bytes);
  std::fputs(",\"process_peak_rss_bytes\":", stdout);
  print_known_u64(diagnostic.host.process_peak_rss_known,
                  diagnostic.host.process_peak_rss_bytes);
  std::printf(
      ",\"vulkan_submits\":%llu,\"vulkan_completions\":%llu,"
      "\"vulkan_submit_cpu_ns\":%llu,\"vulkan_fence_wait_ns\":%llu,"
      "\"vulkan_readback_ns\":%llu,\"vulkan_gpu_time_known\":%s,"
      "\"vulkan_gpu_ns\":%llu,\"vulkan_starvation_ns\":%llu,"
      "\"matcher_cpu_ns\":%llu,\"publication_ns\":%llu,"
      "\"local_ineligible_fallback_items\":%llu,"
      "\"backend_failure_fallback_items\":%llu,"
      "\"backend_other_fallback_items\":%llu,"
      "\"fallback_items_saturated\":%s,\"reason\":",
      static_cast<unsigned long long>(diagnostic.execution.vulkan_submits),
      static_cast<unsigned long long>(diagnostic.execution.vulkan_completions),
      static_cast<unsigned long long>(diagnostic.execution.vulkan_submit_cpu_ns),
      static_cast<unsigned long long>(diagnostic.execution.vulkan_fence_wait_ns),
      static_cast<unsigned long long>(diagnostic.execution.vulkan_readback_ns),
      diagnostic.execution.vulkan_gpu_time_known ? "true" : "false",
      static_cast<unsigned long long>(diagnostic.execution.vulkan_gpu_ns),
      static_cast<unsigned long long>(diagnostic.execution.vulkan_starvation_ns),
      static_cast<unsigned long long>(diagnostic.execution.matcher_cpu_ns),
      static_cast<unsigned long long>(diagnostic.execution.publication_ns),
      static_cast<unsigned long long>(
          diagnostic.execution.local_ineligible_fallback_items),
      static_cast<unsigned long long>(
          diagnostic.execution.backend_failure_fallback_items),
      static_cast<unsigned long long>(
          diagnostic.execution.backend_other_fallback_items),
      diagnostic.execution.fallback_items_saturated ? "true" : "false");
  print_json_string(diagnostic.reason);
  std::fputs(",\"backend_reason\":", stdout);
  print_json_string(diagnostic.backend_reason);
  std::fputs("}\n", stdout);
}

void begin_matcher_evidence(Runtime &runtime) {
  runtime.matcher_evidence_active = true;
  runtime.matcher_diagnostic_serial = 0;
  runtime.matcher_diagnostic_samples = 0;
  runtime.matcher_wall_begin = std::chrono::steady_clock::now();
  runtime.matcher_backend_before_known = runtime.state.orb_vulkan_backend &&
      lardon3d_orb_vulkan_internal_telemetry(
          runtime.state.orb_vulkan_backend, &runtime.matcher_backend_before);
}

uint64_t counter_delta(uint64_t before, uint64_t after) {
  return after >= before ? after - before : 0;
}

struct MatcherExperimentValidation {
  bool applicable{};
  bool valid{};
  uint64_t local_ineligible_fallback_items{};
  const char *reason{"not-forced"};
};

bool counter_partition(uint64_t first, uint64_t second, uint64_t third,
                       uint64_t fourth, uint64_t total) {
  if (first > total) return false;
  total -= first;
  if (second > total) return false;
  total -= second;
  if (third > total) return false;
  total -= third;
  return fourth == total;
}

MatcherExperimentValidation validate_forced_matcher_experiment(
    const Runtime &runtime, bool aggregate_known,
    const Lardon3DResourceSequenceAggregate &aggregate, bool last_known,
    const Lardon3DResourceSequenceDiagnostic &last,
    bool backend_delta_known, uint64_t backend_failures,
    uint64_t backend_discards, bool backend_slot_pending) {
  MatcherExperimentValidation result{};
  result.applicable = runtime.matcher_inflight_override != 0;
  if (!result.applicable) return result;
  const uint64_t depth = runtime.matcher_inflight_override;
  const uint64_t batch = runtime.matcher_batch_override != 0
      ? runtime.matcher_batch_override : 2;
  const uint64_t payload = depth * LARDON3D_ORB_VULKAN_PER_SLOT_BYTES;
  result.local_ineligible_fallback_items =
      aggregate.local_ineligible_fallback_items;
#define INVALID_EXPERIMENT(why)                                                 \
  do {                                                                          \
    result.reason = (why);                                                       \
    return result;                                                               \
  } while (false)
  if (!aggregate_known) INVALID_EXPERIMENT("aggregate-unavailable");
  if (aggregate.saturated) INVALID_EXPERIMENT("aggregate-saturated");
  if (aggregate.admission_count == 0 || aggregate.sequence_count == 0)
    INVALID_EXPERIMENT("no-completed-vulkan-sequence");
  if (aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_FIXED] != 0 ||
      aggregate.selected_backend_admissions[LARDON3D_RESOURCE_BACKEND_CPU] != 0 ||
      aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_MIXED] != 0 ||
      aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] != aggregate.admission_count)
    INVALID_EXPERIMENT("selected-contract-not-exclusively-vulkan");
  if (aggregate.admission_count != aggregate.sequence_count)
    INVALID_EXPERIMENT("admission-sequence-count-mismatch");
  if (aggregate.contract_change_count != 0)
    INVALID_EXPERIMENT("forced-contract-changed");
  if (!last_known || last.backend != LARDON3D_RESOURCE_BACKEND_ORB_VULKAN ||
      last.cpu_threads != 1 || last.gpu_slots != 1 ||
      last.batch_size != batch ||
      last.inflight_limit != depth || last.helper_limit != 0 ||
      last.io_slots != 1 ||
      last.memory_bytes != batch * UINT64_C(10) * 1024 * 1024 ||
      last.gpu_memory_bytes != payload)
    INVALID_EXPERIMENT("forced-contract-mismatch");
  if (!backend_delta_known) INVALID_EXPERIMENT("backend-telemetry-unavailable");
  if (backend_failures != 0 ||
      aggregate.backend_failure_fallback_sequences != 0 ||
      aggregate.backend_failure_fallback_items != 0)
    INVALID_EXPERIMENT("backend-failure");
  if (backend_discards != 0) INVALID_EXPERIMENT("backend-discard");
  if (backend_slot_pending) INVALID_EXPERIMENT("backend-slot-pending");
  if (aggregate.backend_other_fallback_sequences != 0 ||
      aggregate.backend_other_fallback_items != 0)
    INVALID_EXPERIMENT("unclassified-backend-fallback");
  if (aggregate.local_ineligible_fallback_items > aggregate.durable_items ||
      aggregate.backend_ineligible_fallback_sequences >
          aggregate.local_ineligible_fallback_items ||
      ((aggregate.local_ineligible_fallback_items == 0) !=
       (aggregate.backend_ineligible_fallback_sequences == 0)))
    INVALID_EXPERIMENT("fallback-item-classification-mismatch");
  if (!counter_partition(
          aggregate.backend_ineligible_fallback_sequences,
          aggregate.backend_failure_fallback_sequences,
          aggregate.backend_other_fallback_sequences, 0,
          aggregate.backend_fallback_sequences))
    INVALID_EXPERIMENT("fallback-classification-mismatch");
  if (!counter_partition(
          aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_FIXED],
          aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_CPU],
          aggregate.actual_backend_sequences[
              LARDON3D_RESOURCE_BACKEND_ORB_VULKAN],
          aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_MIXED],
          aggregate.sequence_count))
    INVALID_EXPERIMENT("actual-backend-count-mismatch");
  if (aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_FIXED] != 0 ||
      !counter_partition(
          aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_CPU],
          aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_MIXED],
          0, 0, aggregate.backend_ineligible_fallback_sequences) ||
      !counter_partition(
          aggregate.actual_backend_sequences[
              LARDON3D_RESOURCE_BACKEND_ORB_VULKAN],
          aggregate.backend_ineligible_fallback_sequences, 0, 0,
          aggregate.sequence_count))
    INVALID_EXPERIMENT("nonlocal-cpu-fallback");
  if (aggregate.vulkan_submits != aggregate.vulkan_completions)
    INVALID_EXPERIMENT("vulkan-submit-completion-mismatch");
  result.valid = true;
  result.reason = "valid-forced-vulkan-cohort";
#undef INVALID_EXPERIMENT
  return result;
}

void print_cpu_mask(const uint64_t mask[LARDON3D_RESOURCE_CPU_MASK_WORDS]) {
  std::putchar('[');
  bool first = true;
  for (unsigned int cpu = 0; cpu < LARDON3D_RESOURCE_CPU_MAX; ++cpu) {
    if ((mask[cpu / 64] & (UINT64_C(1) << (cpu % 64))) == 0) continue;
    std::printf("%s%u", first ? "" : ",", cpu);
    first = false;
  }
  std::putchar(']');
}

bool end_matcher_evidence(Runtime &runtime) {
  if (!runtime.matcher_evidence_active)
    return runtime.matcher_inflight_override == 0;
  emit_matcher_diagnostic_change(runtime);
  const uint64_t wall_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - runtime.matcher_wall_begin).count());
  Lardon3DResourceSequenceAggregate aggregate{};
  const bool aggregate_known =
      lardon3d_resource_governor_internal_sequence_aggregate(
          runtime.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &aggregate);
  Lardon3DResourceSequenceDiagnostic last{};
  const bool last_known = lardon3d_resource_governor_internal_last_diagnostic(
      runtime.state.resource_governor, LARDON3D_MATCHER_TASK_KIND,
      LARDON3D_MATCHER_TASK_KIND_VERSION, &last);
  Lardon3DResourceCpuPolicyDiagnostic cpu_policy{};
  const bool cpu_policy_known = lardon3d_resource_governor_internal_cpu_policy(
      runtime.state.resource_governor, &cpu_policy);
  Lardon3DOrbVulkanTelemetry backend_after{};
  const bool backend_after_known = runtime.state.orb_vulkan_backend &&
      lardon3d_orb_vulkan_internal_telemetry(runtime.state.orb_vulkan_backend,
                                             &backend_after);
  const bool backend_delta_known = runtime.matcher_backend_before_known &&
                                   backend_after_known;
  const uint64_t backend_failures = backend_delta_known
      ? counter_delta(runtime.matcher_backend_before.failures,
                      backend_after.failures) : 0;
  const uint64_t backend_discards = backend_delta_known
      ? counter_delta(runtime.matcher_backend_before.discards,
                      backend_after.discards) : 0;
  const bool backend_slot_pending = backend_after_known &&
                                    backend_after.slot_pending;
  const MatcherExperimentValidation experiment =
      validate_forced_matcher_experiment(
          runtime, aggregate_known, aggregate, last_known, last,
          backend_delta_known, backend_failures, backend_discards,
          backend_slot_pending);
  uint64_t durable_rate_milli = 0;
  if (aggregate_known && aggregate.durable_items > 0 && wall_ns > 0 &&
      aggregate.durable_items <= UINT64_MAX / UINT64_C(1000000000000))
    durable_rate_milli = aggregate.durable_items * UINT64_C(1000000000000) / wall_ns;
  std::printf(
      "{\"record\":\"matcher_evidence_aggregate\",\"aggregate_scope\":"
      "\"governor-recorded-sequences\",\"diagnostic_sampling\":"
      "\"latest-change-coalescing\",\"diagnostic_samples\":%llu,"
      "\"wall_ns\":%llu,\"durable_pairs\":%llu,"
      "\"durable_pairs_per_second_milli\":%llu,\"admissions\":%llu,"
      "\"sequences\":%llu,\"contract_changes\":%llu,"
      "\"selected_cpu_admissions\":%llu,\"selected_vulkan_admissions\":%llu,"
      "\"actual_cpu_sequences\":%llu,\"actual_vulkan_sequences\":%llu,"
      "\"actual_mixed_sequences\":%llu,\"fallback_sequences\":%llu,"
      "\"local_ineligible_fallback_sequences\":%llu,"
      "\"backend_failure_fallback_sequences\":%llu,"
      "\"backend_other_fallback_sequences\":%llu,"
      "\"local_ineligible_fallback_items\":%llu,"
      "\"backend_failure_fallback_items\":%llu,"
      "\"backend_other_fallback_items\":%llu,"
      "\"sequence_wall_ns\":%llu,\"min_mem_available_bytes\":",
      static_cast<unsigned long long>(runtime.matcher_diagnostic_samples),
      static_cast<unsigned long long>(wall_ns),
      static_cast<unsigned long long>(aggregate.durable_items),
      static_cast<unsigned long long>(durable_rate_milli),
      static_cast<unsigned long long>(aggregate.admission_count),
      static_cast<unsigned long long>(aggregate.sequence_count),
      static_cast<unsigned long long>(aggregate.contract_change_count),
      static_cast<unsigned long long>(aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_CPU]),
      static_cast<unsigned long long>(aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN]),
      static_cast<unsigned long long>(aggregate.actual_backend_sequences[
          LARDON3D_RESOURCE_BACKEND_CPU]),
      static_cast<unsigned long long>(aggregate.actual_backend_sequences[
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN]),
      static_cast<unsigned long long>(aggregate.actual_backend_sequences[
          LARDON3D_RESOURCE_BACKEND_MIXED]),
      static_cast<unsigned long long>(aggregate.backend_fallback_sequences),
      static_cast<unsigned long long>(
          aggregate.backend_ineligible_fallback_sequences),
      static_cast<unsigned long long>(
          aggregate.backend_failure_fallback_sequences),
      static_cast<unsigned long long>(
          aggregate.backend_other_fallback_sequences),
      static_cast<unsigned long long>(
          aggregate.local_ineligible_fallback_items),
      static_cast<unsigned long long>(
          aggregate.backend_failure_fallback_items),
      static_cast<unsigned long long>(
          aggregate.backend_other_fallback_items),
      static_cast<unsigned long long>(aggregate.total_wall_time_ns));
  print_known_u64(aggregate_known && aggregate.memory_available_known,
                  aggregate.minimum_memory_available_bytes);
  std::fputs(",\"max_gpu_busy_basis_points\":", stdout);
  print_known_u32(aggregate_known && aggregate.gpu_busy_known,
                  aggregate.maximum_gpu_busy_basis_points);
  std::fputs(",\"max_process_rss_bytes\":", stdout);
  print_known_u64(aggregate_known && aggregate.process_rss_known,
                  aggregate.maximum_process_rss_bytes);
  std::fputs(",\"max_process_peak_rss_bytes\":", stdout);
  print_known_u64(aggregate_known && aggregate.process_peak_rss_known,
                  aggregate.maximum_process_peak_rss_bytes);
  std::printf(
      ",\"publication_ns\":%llu,\"vulkan_submits\":%llu,"
      "\"vulkan_completions\":%llu,\"vulkan_submit_cpu_ns\":%llu,"
      "\"vulkan_fence_wait_ns\":%llu,"
      "\"vulkan_readback_ns\":%llu,\"known_vulkan_gpu_ns\":%llu,"
      "\"vulkan_gpu_known_sequences\":%llu,\"vulkan_starvation_ns\":%llu,"
      "\"matcher_cpu_ns\":%llu,\"aggregate_saturated\":%s,"
      "\"backend_counter_delta_known\":%s,\"backend_failures\":%llu,"
      "\"backend_discards\":%llu,\"backend_slot_pending\":%s,"
      "\"affinity_known\":%s,\"affinity_active\":%s,"
      "\"runtime_thread_policy_active\":%s,"
      "\"mesa_shader_cache_disabled\":%s,"
      "\"compute_cpu_count\":%u,\"reserved_cpu_count\":%u,"
      "\"compute_mask\":",
      static_cast<unsigned long long>(aggregate.publication_ns),
      static_cast<unsigned long long>(aggregate.vulkan_submits),
      static_cast<unsigned long long>(aggregate.vulkan_completions),
      static_cast<unsigned long long>(aggregate.vulkan_submit_cpu_ns),
      static_cast<unsigned long long>(aggregate.vulkan_fence_wait_ns),
      static_cast<unsigned long long>(aggregate.vulkan_readback_ns),
      static_cast<unsigned long long>(aggregate.vulkan_gpu_ns),
      static_cast<unsigned long long>(aggregate.vulkan_gpu_known_sequences),
      static_cast<unsigned long long>(aggregate.vulkan_starvation_ns),
      static_cast<unsigned long long>(aggregate.matcher_cpu_ns),
      aggregate.saturated ? "true" : "false",
      backend_delta_known ? "true" : "false",
      static_cast<unsigned long long>(backend_failures),
      static_cast<unsigned long long>(backend_discards),
      backend_slot_pending ? "true" : "false",
      cpu_policy_known ? "true" : "false",
      cpu_policy_known && cpu_policy.affinity_active ? "true" : "false",
      cpu_policy_known && cpu_policy.runtime_thread_policy_active
          ? "true" : "false",
      cpu_policy_known && cpu_policy.mesa_shader_cache_disabled
          ? "true" : "false",
      cpu_policy.compute_cpu_count, cpu_policy.reserved_cpu_count);
  print_cpu_mask(cpu_policy.compute_mask);
  std::fputs(",\"reserved_mask\":", stdout);
  print_cpu_mask(cpu_policy.reserved_mask);
  std::fputs(",\"runtime_thread_policy_reason\":", stdout);
  print_json_string(cpu_policy_known
      ? cpu_policy.runtime_thread_policy_reason : "unknown");
  std::fputs(",\"matcher_inflight_override\":", stdout);
  print_known_u32(runtime.matcher_inflight_override != 0,
                  runtime.matcher_inflight_override);
  std::fputs(",\"matcher_batch_override\":", stdout);
  print_known_u32(runtime.matcher_batch_override != 0,
                  runtime.matcher_batch_override);
  std::fputs(",\"experiment_valid\":", stdout);
  if (experiment.applicable)
    std::fputs(experiment.valid ? "true" : "false", stdout);
  else
    std::fputs("null", stdout);
  std::fputs(",\"experiment_reason\":", stdout);
  if (experiment.applicable) print_json_string(experiment.reason);
  else std::fputs("null", stdout);
  std::fputs(
      ",\"comparison_requires_equal_local_ineligible_fallback_items\":",
      stdout);
  if (experiment.applicable) std::fputs("true", stdout);
  else std::fputs("null", stdout);
  std::printf(",\"uma\":%s,\"last_contract_known\":%s",
              runtime.state.hardware_profile.gpu_uses_shared_memory ? "true" : "false",
              last_known ? "true" : "false");
  if (last_known) {
    std::printf(",\"last_selected_backend\":\"%s\",\"last_actual_backend\":\"%s\","
                "\"last_cpu\":%u,\"last_gpu\":%u,\"last_batch\":%zu,"
                "\"last_inflight\":%zu,\"last_helpers\":%u,\"last_reason\":",
                backend_name(last.backend), backend_name(last.actual_backend),
                last.cpu_threads, last.gpu_slots, last.batch_size,
                last.inflight_limit, last.helper_limit);
    print_json_string(last.reason);
  }
  std::fputs("}\n", stdout);
  runtime.matcher_evidence_active = false;
  return !experiment.applicable || experiment.valid;
}

void begin_geometric_evidence(Runtime &runtime) {
  runtime.geometric_evidence_active = true;
  runtime.geometric_diagnostic_serial = 0;
  runtime.geometric_diagnostic_samples = 0;
  runtime.geometric_wall_begin = std::chrono::steady_clock::now();
  runtime.geometric_memory_available_known = false;
  runtime.geometric_minimum_memory_available_bytes = 0;
  runtime.geometric_memory_psi_some_known = false;
  runtime.geometric_maximum_memory_psi_some_basis_points = 0;
  runtime.geometric_memory_psi_full_known = false;
  runtime.geometric_maximum_memory_psi_full_basis_points = 0;
  runtime.geometric_io_psi_some_known = false;
  runtime.geometric_maximum_io_psi_some_basis_points = 0;
  runtime.geometric_io_psi_full_known = false;
  runtime.geometric_maximum_io_psi_full_basis_points = 0;
  runtime.geometric_swap_delta_known = false;
  runtime.geometric_maximum_swap_pages_in_delta = 0;
  runtime.geometric_maximum_swap_pages_out_delta = 0;
  runtime.geometric_process_rss_known = false;
  runtime.geometric_maximum_process_rss_bytes = 0;
  runtime.geometric_process_peak_rss_known = false;
  runtime.geometric_maximum_process_peak_rss_bytes = 0;
}

void sample_geometric_diagnostic(Runtime &runtime) {
  if (!runtime.geometric_evidence_active || !runtime.state.resource_governor)
    return;
  Lardon3DResourceSequenceDiagnostic diagnostic{};
  if (!lardon3d_resource_governor_internal_diagnostic_since(
          runtime.state.resource_governor,
          LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND,
          LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION,
          runtime.geometric_diagnostic_serial, &diagnostic))
    return;
  runtime.geometric_diagnostic_serial = diagnostic.serial;
  ++runtime.geometric_diagnostic_samples;
  if (diagnostic.host.memory_available_known &&
      (!runtime.geometric_memory_available_known ||
       diagnostic.host.memory_available_bytes <
           runtime.geometric_minimum_memory_available_bytes)) {
    runtime.geometric_memory_available_known = true;
    runtime.geometric_minimum_memory_available_bytes =
        diagnostic.host.memory_available_bytes;
  }
#define RETAIN_GEOMETRIC_MAX(known_field, maximum_field, source_known, source) \
  do {                                                                          \
    if ((source_known) && (!(known_field) || (source) > (maximum_field))) {      \
      (known_field) = true;                                                      \
      (maximum_field) = (source);                                                \
    }                                                                            \
  } while (false)
  RETAIN_GEOMETRIC_MAX(runtime.geometric_memory_psi_some_known,
                       runtime.geometric_maximum_memory_psi_some_basis_points,
                       diagnostic.host.memory_psi_some_known,
                       diagnostic.host.memory_psi_some_basis_points);
  RETAIN_GEOMETRIC_MAX(runtime.geometric_memory_psi_full_known,
                       runtime.geometric_maximum_memory_psi_full_basis_points,
                       diagnostic.host.memory_psi_full_known,
                       diagnostic.host.memory_psi_full_basis_points);
  RETAIN_GEOMETRIC_MAX(runtime.geometric_io_psi_some_known,
                       runtime.geometric_maximum_io_psi_some_basis_points,
                       diagnostic.host.io_psi_some_known,
                       diagnostic.host.io_psi_some_basis_points);
  RETAIN_GEOMETRIC_MAX(runtime.geometric_io_psi_full_known,
                       runtime.geometric_maximum_io_psi_full_basis_points,
                       diagnostic.host.io_psi_full_known,
                       diagnostic.host.io_psi_full_basis_points);
  if (diagnostic.host.swap_delta_known) {
    runtime.geometric_swap_delta_known = true;
    runtime.geometric_maximum_swap_pages_in_delta = std::max(
        runtime.geometric_maximum_swap_pages_in_delta,
        diagnostic.host.swap_pages_in_delta);
    runtime.geometric_maximum_swap_pages_out_delta = std::max(
        runtime.geometric_maximum_swap_pages_out_delta,
        diagnostic.host.swap_pages_out_delta);
  }
  RETAIN_GEOMETRIC_MAX(runtime.geometric_process_rss_known,
                       runtime.geometric_maximum_process_rss_bytes,
                       diagnostic.host.process_rss_known,
                       diagnostic.host.process_rss_bytes);
  RETAIN_GEOMETRIC_MAX(runtime.geometric_process_peak_rss_known,
                       runtime.geometric_maximum_process_peak_rss_bytes,
                       diagnostic.host.process_peak_rss_known,
                       diagnostic.host.process_peak_rss_bytes);
#undef RETAIN_GEOMETRIC_MAX
}

bool end_geometric_evidence(Runtime &runtime) {
  if (!runtime.geometric_evidence_active) return false;
  sample_geometric_diagnostic(runtime);
  const uint64_t wall_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - runtime.geometric_wall_begin)
          .count());
  Lardon3DResourceSequenceAggregate aggregate{};
  const bool aggregate_known =
      lardon3d_resource_governor_internal_sequence_aggregate(
          runtime.state.resource_governor,
          LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND,
          LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION, &aggregate);
  Lardon3DResourceSequenceDiagnostic last{};
  const bool last_known = lardon3d_resource_governor_internal_last_diagnostic(
      runtime.state.resource_governor,
      LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND,
      LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND_VERSION, &last);
  Lardon3DResourceCpuPolicyDiagnostic cpu_policy{};
  const bool cpu_policy_known =
      lardon3d_resource_governor_internal_cpu_policy(
          runtime.state.resource_governor, &cpu_policy);
  const bool contract_valid =
      last_known && last.backend == LARDON3D_RESOURCE_BACKEND_FIXED &&
      last.actual_backend == LARDON3D_RESOURCE_BACKEND_FIXED &&
      !last.backend_fallback && last.cpu_threads >= 1 &&
      last.cpu_threads <=
          LARDON3D_GEOMETRIC_VERIFIER_TASK_VALIDATED_USEFUL_CPU_THREADS &&
      last.gpu_slots == 0 && last.io_slots == 1 &&
      last.batch_size >= LARDON3D_GEOMETRIC_VERIFIER_TASK_MINIMUM_BATCH &&
      last.batch_size <= LARDON3D_GEOMETRIC_VERIFIER_TASK_MAXIMUM_BATCH &&
      last.memory_bytes ==
          UINT64_C(8) * 1024 * 1024 * last.batch_size &&
      last.gpu_memory_bytes == 0;
  const bool admission_valid =
      aggregate_known && !aggregate.saturated &&
      aggregate.admission_count > 0 &&
      aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_FIXED] == aggregate.admission_count &&
      aggregate.selected_backend_admissions[LARDON3D_RESOURCE_BACKEND_CPU] == 0 &&
      aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] == 0 &&
      contract_valid && cpu_policy_known && cpu_policy.affinity_active &&
      cpu_policy.runtime_thread_policy_active;
  std::printf(
      "{\"record\":\"geometric_governor_evidence\","
      "\"diagnostic_sampling\":\"latest-change-coalescing\","
      "\"diagnostic_samples\":%llu,\"wall_ns\":%llu,"
      "\"admissions\":%llu,\"governor_sequence_observations\":%llu,"
      "\"selected_fixed_admissions\":%llu,\"selected_cpu_admissions\":%llu,"
      "\"selected_vulkan_admissions\":%llu,\"contract_changes\":%llu,"
      "\"min_mem_available_bytes\":",
      static_cast<unsigned long long>(runtime.geometric_diagnostic_samples),
      static_cast<unsigned long long>(wall_ns),
      static_cast<unsigned long long>(aggregate.admission_count),
      static_cast<unsigned long long>(aggregate.sequence_count),
      static_cast<unsigned long long>(aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_FIXED]),
      static_cast<unsigned long long>(aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_CPU]),
      static_cast<unsigned long long>(aggregate.selected_backend_admissions[
          LARDON3D_RESOURCE_BACKEND_ORB_VULKAN]),
      static_cast<unsigned long long>(aggregate.contract_change_count));
  print_known_u64(runtime.geometric_memory_available_known,
                  runtime.geometric_minimum_memory_available_bytes);
  std::fputs(",\"max_memory_psi_some_basis_points\":", stdout);
  print_known_u32(runtime.geometric_memory_psi_some_known,
                  runtime.geometric_maximum_memory_psi_some_basis_points);
  std::fputs(",\"max_memory_psi_full_basis_points\":", stdout);
  print_known_u32(runtime.geometric_memory_psi_full_known,
                  runtime.geometric_maximum_memory_psi_full_basis_points);
  std::fputs(",\"max_io_psi_some_basis_points\":", stdout);
  print_known_u32(runtime.geometric_io_psi_some_known,
                  runtime.geometric_maximum_io_psi_some_basis_points);
  std::fputs(",\"max_io_psi_full_basis_points\":", stdout);
  print_known_u32(runtime.geometric_io_psi_full_known,
                  runtime.geometric_maximum_io_psi_full_basis_points);
  std::fputs(",\"observed_max_swap_pages_in_delta\":", stdout);
  print_known_u64(runtime.geometric_swap_delta_known,
                  runtime.geometric_maximum_swap_pages_in_delta);
  std::fputs(",\"observed_max_swap_pages_out_delta\":", stdout);
  print_known_u64(runtime.geometric_swap_delta_known,
                  runtime.geometric_maximum_swap_pages_out_delta);
  std::fputs(",\"max_process_rss_bytes\":", stdout);
  print_known_u64(runtime.geometric_process_rss_known,
                  runtime.geometric_maximum_process_rss_bytes);
  std::fputs(",\"max_process_peak_rss_bytes\":", stdout);
  print_known_u64(runtime.geometric_process_peak_rss_known,
                  runtime.geometric_maximum_process_peak_rss_bytes);
  std::printf(
      ",\"aggregate_saturated\":%s,\"affinity_known\":%s,"
      "\"affinity_active\":%s,\"runtime_thread_policy_active\":%s,"
      "\"compute_cpu_count\":%u,\"reserved_cpu_count\":%u,"
      "\"compute_mask\":",
      aggregate.saturated ? "true" : "false",
      cpu_policy_known ? "true" : "false",
      cpu_policy_known && cpu_policy.affinity_active ? "true" : "false",
      cpu_policy_known && cpu_policy.runtime_thread_policy_active ? "true" : "false",
      cpu_policy.compute_cpu_count, cpu_policy.reserved_cpu_count);
  print_cpu_mask(cpu_policy.compute_mask);
  std::fputs(",\"reserved_mask\":", stdout);
  print_cpu_mask(cpu_policy.reserved_mask);
  std::printf(",\"last_contract_known\":%s", last_known ? "true" : "false");
  if (last_known) {
    std::printf(
        ",\"last_selected_backend\":\"%s\",\"last_actual_backend\":\"%s\","
        "\"last_pressure\":\"%s\",\"last_cpu\":%u,\"last_gpu\":%u,"
        "\"last_batch\":%zu,\"last_io\":%u,\"last_host_memory_bytes\":%llu,"
        "\"last_gpu_memory_bytes\":%llu,\"last_reason\":",
        backend_name(last.backend), backend_name(last.actual_backend),
        pressure_name(last.pressure), last.cpu_threads, last.gpu_slots,
        last.batch_size, last.io_slots,
        static_cast<unsigned long long>(last.memory_bytes),
        static_cast<unsigned long long>(last.gpu_memory_bytes));
    print_json_string(last.reason);
  }
  std::printf(",\"governor_admission_pass\":%s}\n",
              admission_valid ? "true" : "false");
  runtime.geometric_evidence_active = false;
  return admission_valid;
}

bool wait_completed(Runtime &runtime, uint64_t task_id, const char *phase) {
  for (;;) {
    emit_matcher_diagnostic_change(runtime);
    sample_geometric_diagnostic(runtime);
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
        emit_matcher_diagnostic_change(runtime);
        sample_geometric_diagnostic(runtime);
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
                uint64_t &matcher_task_id,
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
  begin_matcher_evidence(runtime);
  const bool matcher_ok =
      lardon3d_project_enqueue_matcher_task(&runtime.state, &matcher, &task_id) &&
      wait_completed(runtime, task_id, "matcher.run");
  const bool matcher_experiment_valid = end_matcher_evidence(runtime);
  if (!matcher_ok || !matcher_experiment_valid)
    return false;
  matcher_task_id = task_id;
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

bool digest_update(EVP_MD_CTX *context, const void *bytes, size_t size) {
  return EVP_DigestUpdate(context, bytes, size) == 1;
}

bool digest_u32(EVP_MD_CTX *context, uint32_t value) {
  unsigned char encoded[4];
  for (size_t index = 0; index < sizeof(encoded); ++index) {
    encoded[index] = static_cast<unsigned char>(value & 0xffU);
    value >>= 8U;
  }
  return digest_update(context, encoded, sizeof(encoded));
}

bool digest_u64(EVP_MD_CTX *context, uint64_t value) {
  unsigned char encoded[8];
  for (size_t index = 0; index < sizeof(encoded); ++index) {
    encoded[index] = static_cast<unsigned char>(value & 0xffU);
    value >>= 8U;
  }
  return digest_update(context, encoded, sizeof(encoded));
}

bool digest_string(EVP_MD_CTX *context, const char *text, size_t capacity) {
  const size_t length = strnlen(text, capacity);
  return length < capacity && length <= UINT32_MAX &&
         digest_u32(context, static_cast<uint32_t>(length)) &&
         digest_update(context, text, length);
}

struct CandidateStream {
  uint64_t cursor{};
  Lardon3DProjectDbCandidatePair page[64]{};
  size_t count{};
  size_t index{};
  bool exhausted{};
};

bool next_candidate(Lardon3DProjectDb *database, CandidateStream &stream,
                    Lardon3DProjectDbCandidatePair &candidate, bool &has_value) {
  has_value = false;
  if (stream.exhausted) return true;
  if (stream.index == stream.count) {
    stream.index = 0;
    stream.count = 0;
    if (lardon3d_project_db_list_candidate_pairs(database, stream.cursor,
                                                  stream.page, 64,
                                                  &stream.count) !=
        LARDON3D_PROJECT_DB_OK)
      return false;
    if (stream.count == 0) {
      stream.exhausted = true;
      return true;
    }
  }
  candidate = stream.page[stream.index++];
  if (candidate.candidate_pair_id <= stream.cursor) return false;
  stream.cursor = candidate.candidate_pair_id;
  has_value = true;
  return true;
}

struct MatchStream {
  uint64_t cursor{};
  Lardon3DProjectDbMatchResult page[64]{};
  size_t count{};
  size_t index{};
  bool exhausted{};
};

bool next_match(Lardon3DProjectDb *database, MatchStream &stream,
                Lardon3DProjectDbMatchResult &match, bool &has_value) {
  has_value = false;
  if (stream.exhausted) return true;
  if (stream.index == stream.count) {
    stream.index = 0;
    stream.count = 0;
    if (lardon3d_project_db_list_match_results(database, stream.cursor,
                                                stream.page, 64,
                                                &stream.count) !=
        LARDON3D_PROJECT_DB_OK)
      return false;
    if (stream.count == 0) {
      stream.exhausted = true;
      return true;
    }
  }
  match = stream.page[stream.index++];
  if (match.match_result_id <= stream.cursor) return false;
  stream.cursor = match.match_result_id;
  has_value = true;
  return true;
}

bool query_match_audit_sql(const Runtime &runtime, uint64_t &latest_matcher_task_id,
                           size_t &duplicate_mappings) {
  latest_matcher_task_id = 0;
  duplicate_mappings = 0;
  sqlite3 *database = nullptr;
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_open_v2(runtime.database_path.c_str(), &database,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    if (database) sqlite3_close(database);
    return false;
  }
  bool ok = sqlite3_prepare_v2(
                database,
                "SELECT task_id FROM matcher_tasks ORDER BY task_id DESC LIMIT 1",
                -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW;
  if (ok) {
    const sqlite3_int64 task_id = sqlite3_column_int64(statement, 0);
    ok = task_id > 0;
    if (ok) latest_matcher_task_id = static_cast<uint64_t>(task_id);
  }
  if (statement) sqlite3_finalize(statement);
  statement = nullptr;
  ok = ok && sqlite3_prepare_v2(
                 database,
                 "SELECT COALESCE(SUM(n-1),0) FROM (SELECT COUNT(*) AS n "
                 "FROM match_results GROUP BY candidate_pair_id HAVING COUNT(*)>1)",
                 -1, &statement, nullptr) == SQLITE_OK &&
       sqlite3_step(statement) == SQLITE_ROW;
  if (ok) {
    const sqlite3_int64 duplicates = sqlite3_column_int64(statement, 0);
    ok = duplicates >= 0 && static_cast<uint64_t>(duplicates) <= SIZE_MAX;
    if (ok) duplicate_mappings = static_cast<size_t>(duplicates);
  }
  if (statement) sqlite3_finalize(statement);
  return sqlite3_close(database) == SQLITE_OK && ok;
}

bool validate_match_asset(const Runtime &runtime,
                          const Lardon3DProjectDbMatchResult &match) {
  if (match.result_status == LARDON3D_MATCH_RESULT_STATUS_NO_MATCH)
    return match.match_count == 0 && !match.has_match_asset &&
           match.match_asset_path[0] == '\0' && match.match_asset_size_bytes == 0;
  if (match.result_status != LARDON3D_MATCH_RESULT_STATUS_MATCHED ||
      match.match_count == 0 || !match.has_match_asset ||
      match.match_asset_path[0] == '\0' || match.match_asset_size_bytes == 0)
    return false;
  const std::filesystem::path relative(match.match_asset_path);
  if (relative.is_absolute() || relative.lexically_normal() != relative) return false;
  for (const auto &component : relative)
    if (component == "..") return false;
  Lardon3DProjectDbFeatureSet feature_a{};
  Lardon3DProjectDbFeatureSet feature_b{};
  if (lardon3d_project_db_load_feature_set(runtime.state.project_db,
                                            match.feature_set_id_a,
                                            &feature_a) != LARDON3D_PROJECT_DB_OK ||
      lardon3d_project_db_load_feature_set(runtime.state.project_db,
                                            match.feature_set_id_b,
                                            &feature_b) != LARDON3D_PROJECT_DB_OK)
    return false;
  const std::string full_path =
      (std::filesystem::path(runtime.project_path) / relative).string();
  Lardon3DMatchFileHeader header{};
  return lardon3d_match_file_validate_asset(
             full_path.c_str(), match.match_asset_sha256,
             match.match_asset_size_bytes, &header, match.feature_set_id_a,
             match.feature_set_id_b, feature_a.feature_count,
             feature_b.feature_count) == LARDON3D_MATCH_FILE_OK &&
         header.match_count == match.match_count;
}

bool audit_match_results(Runtime &runtime, uint64_t &matcher_task_id,
                         MatchAudit &audit) {
  uint64_t latest_matcher_task_id = 0;
  if (!query_match_audit_sql(runtime, latest_matcher_task_id,
                             audit.duplicate_candidate_pair_mappings) ||
      audit.duplicate_candidate_pair_mappings != 0)
    return false;
  if (matcher_task_id == 0) matcher_task_id = latest_matcher_task_id;
  if (matcher_task_id == 0) return false;
  Lardon3DProjectDbTask durable_task{};
  Lardon3DProjectDbMatcherTask durable_matcher{};
  if (lardon3d_project_db_load_task(runtime.state.project_db, matcher_task_id,
                                     &durable_task) != LARDON3D_PROJECT_DB_OK ||
      durable_task.saved_state != TASK_COMPLETED ||
      lardon3d_project_db_load_matcher_task(runtime.state.project_db,
                                             matcher_task_id,
                                             &durable_matcher) !=
          LARDON3D_PROJECT_DB_OK)
    return false;

  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  DigestContext digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  static constexpr unsigned char format_tag[8] = {
      'L', '3', 'D', 'M', 'R', 'D', '1', '\0'};
  if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1 ||
      !digest_update(digest.get(), format_tag, sizeof(format_tag)))
    return false;

  CandidateStream candidates;
  MatchStream matches;
  uint64_t last_candidate_id = 0;
  for (;;) {
    Lardon3DProjectDbCandidatePair candidate{};
    Lardon3DProjectDbMatchResult match{};
    bool has_candidate = false;
    bool has_match = false;
    if (!next_candidate(runtime.state.project_db, candidates, candidate,
                        has_candidate) ||
        !next_match(runtime.state.project_db, matches, match, has_match) ||
        has_candidate != has_match)
      return false;
    if (!has_candidate) break;
    if (candidate.candidate_pair_id != match.candidate_pair_id ||
        !validate_match_asset(runtime, match))
      return false;
    const size_t matcher_kind_length =
        strnlen(match.matcher_kind, sizeof(match.matcher_kind));
    if (matcher_kind_length == sizeof(match.matcher_kind)) return false;
    const unsigned char has_asset = match.has_match_asset ? 1U : 0U;
    unsigned char zero_sha[32]{};
    if (!digest_u64(digest.get(), match.candidate_pair_id) ||
        !digest_u64(digest.get(), match.feature_set_id_a) ||
        !digest_u64(digest.get(), match.feature_set_id_b) ||
        !digest_string(digest.get(), match.matcher_kind,
                       sizeof(match.matcher_kind)) ||
        !digest_u32(digest.get(), match.matcher_version) ||
        !digest_update(digest.get(), match.parameter_fingerprint,
                       sizeof(match.parameter_fingerprint)) ||
        !digest_u32(digest.get(), static_cast<uint32_t>(match.result_status)) ||
        !digest_u32(digest.get(), match.match_count) ||
        !digest_update(digest.get(), &has_asset, sizeof(has_asset)) ||
        !digest_update(digest.get(), match.has_match_asset
                           ? match.match_asset_sha256 : zero_sha,
                       sizeof(zero_sha)) ||
        !digest_u64(digest.get(), match.match_asset_size_bytes))
      return false;
    ++audit.match_result_count;
    if (match.has_match_asset) ++audit.match_asset_count;
    last_candidate_id = candidate.candidate_pair_id;
  }
  audit.candidate_mapping_contiguous = true;
  audit.matcher_cursor_complete =
      durable_matcher.after_candidate_pair_id == last_candidate_id;
  if (!audit.matcher_cursor_complete ||
      !digest_u64(digest.get(), static_cast<uint64_t>(audit.match_result_count)))
    return false;
  unsigned char output[EVP_MAX_MD_SIZE]{};
  unsigned int output_size = 0;
  if (EVP_DigestFinal_ex(digest.get(), output, &output_size) != 1 ||
      output_size != 32)
    return false;
  static constexpr char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < 32; ++index) {
    audit.digest_hex[2 * index] = digits[output[index] >> 4U];
    audit.digest_hex[2 * index + 1] = digits[output[index] & 0x0fU];
  }
  audit.digest_hex[64] = '\0';
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

bool match_result_frontier(Runtime &runtime, size_t &count,
                           uint64_t &last_match_result_id) {
  count = 0;
  last_match_result_id = 0;
  for (;;) {
    Lardon3DProjectDbMatchResult page[64]{};
    size_t page_count = 0;
    if (lardon3d_project_db_list_match_results(
            runtime.state.project_db, last_match_result_id, page, 64,
            &page_count) != LARDON3D_PROJECT_DB_OK)
      return false;
    if (page_count > SIZE_MAX - count) return false;
    count += page_count;
    if (page_count != 0) {
      const uint64_t next = page[page_count - 1].match_result_id;
      if (next <= last_match_result_id) return false;
      last_match_result_id = next;
    }
    if (page_count < 64) return true;
  }
}

enum class GeometryRecoveryKind { kNone, kGeometry, kTrackBuilder };

bool geometry_recovery_kind(const Lardon3DProjectRecoveryEntry &entry,
                            GeometryRecoveryKind &kind) {
  kind = GeometryRecoveryKind::kNone;
  if ((entry.status != LARDON3D_PROJECT_RECOVERABLE &&
       entry.status != LARDON3D_PROJECT_RECOVERABLE_PUBLISHED_NOT_DURABLE) ||
      entry.snapshot.recovery_state != TASK_PENDING)
    return false;
  if (std::strcmp(entry.task_kind, LARDON3D_GEOMETRIC_VERIFIER_TASK_KIND) == 0) {
    kind = GeometryRecoveryKind::kGeometry;
    return true;
  }
  if (std::strcmp(entry.task_kind, LARDON3D_TRACK_BUILDER_TASK_KIND) == 0 &&
      entry.task_kind_version == LARDON3D_TRACK_BUILDER_TASK_KIND_VERSION) {
    kind = GeometryRecoveryKind::kTrackBuilder;
    return true;
  }
  return false;
}

bool find_pending_geometry_task(Runtime &runtime, uint64_t &geometry_task_id,
                                uint64_t &track_task_id) {
  geometry_task_id = 0;
  track_task_id = 0;
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
      GeometryRecoveryKind kind{};
      if (!geometry_recovery_kind(entry, kind) || geometry_task_id != 0 ||
          track_task_id != 0) {
        /* The production recovery API resumes every pending Task. This
         * GV-only proof route must never replay an upstream or downstream kind
         * merely because it shares the same project. */
        std::fprintf(stderr,
                     "geometry recovery refuses unsafe pending task %llu of kind %s v%u\n",
                     static_cast<unsigned long long>(entry.task_id),
                     entry.task_kind, entry.task_kind_version);
        return false;
      }
      if (kind == GeometryRecoveryKind::kGeometry)
        geometry_task_id = entry.task_id;
      else
        track_task_id = entry.task_id;
    }
    if (count < 8) return true;
  }
}

bool recover_geometry_task(Runtime &runtime, uint64_t geometry_task_id,
                           Lardon3DProjectRecoverySummary &recovery) {
  recovery = {};
  return lardon3d_project_resume_recoverable_tasks(
             &runtime.state, lardon3d_task_kind_registry_production(),
             &recovery) == LARDON3D_PROJECT_DB_OK &&
         recovery.inspected == 1 && recovery.resumed == 1 &&
         recovery.failed == 0 && !recovery.queue_full &&
         wait_completed(runtime, geometry_task_id,
                        "geometric_verifier.run recovered existing");
}

bool recover_track_builder_task(Runtime &runtime, uint64_t track_task_id,
                                Lardon3DProjectRecoverySummary &recovery) {
  recovery = {};
  return lardon3d_project_resume_recoverable_tasks(
             &runtime.state, lardon3d_task_kind_registry_production(),
             &recovery) == LARDON3D_PROJECT_DB_OK &&
         recovery.inspected == 1 && recovery.resumed == 1 &&
         recovery.failed == 0 && !recovery.queue_full &&
         wait_completed(runtime, track_task_id,
                        "track_builder.run recovered existing");
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

  if ((runtime.matcher_pipeline == MatcherPipeline::kSynchronous ||
       runtime.matcher_inflight_override != 0 ||
       runtime.matcher_batch_override != 0) &&
      matcher_task_id != 0) {
    /* Pipeline is deliberately absent from Task identity/checkpoints. Refuse a
     * recovered Matcher rather than pretending a non-persisted benchmark
     * control survived restart. Rolling production recovery remains normal. */
    std::fprintf(stderr,
                 "benchmark Matcher pipeline/inflight controls require a new "
                 "Matcher task; a pending Matcher cannot retain them\n");
    stop_runtime(runtime);
    return false;
  }

  Lardon3DProjectRecoverySummary matcher_recovery{};
  const char *matcher_action = "already_complete";
  begin_matcher_evidence(runtime);
  if (after_candidate.matches < after_candidate.pairs) {
    if (matcher_task_id != 0) {
      if (!recover_one_pre_gv_task(runtime, matcher_task_id,
                                   "matcher.run recovered",
                                   matcher_recovery)) {
        end_matcher_evidence(runtime);
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
      bool enqueued = false;
      if (runtime.matcher_mode == MatcherMode::kAuto) {
        enqueued = lardon3d_project_enqueue_matcher_task(
            &runtime.state, &matcher, &matcher_task_id);
      } else {
        const Lardon3DMatcherTaskMode matcher_mode =
            runtime.matcher_mode == MatcherMode::kVulkan
                ? LARDON3D_MATCHER_TASK_MODE_ORB_VULKAN
                : LARDON3D_MATCHER_TASK_MODE_CPU_PARALLEL;
        enqueued = lardon3d_project_enqueue_matcher_task_with_mode(
            &runtime.state, &matcher, matcher_mode, &matcher_task_id);
      }
      if (!enqueued ||
          !wait_completed(runtime, matcher_task_id, "matcher.run existing")) {
        end_matcher_evidence(runtime);
        stop_runtime(runtime);
        return false;
      }
      matcher_action = "enqueued";
    }
  } else if (matcher_task_id != 0) {
    std::fprintf(stderr,
                 "complete Match Results coexist with a pending Matcher task\n");
    end_matcher_evidence(runtime);
    stop_runtime(runtime);
    return false;
  }
  const bool matcher_experiment_valid = end_matcher_evidence(runtime);

  Evidence after{};
  MatchAudit match_audit{};
  const bool ok = matcher_experiment_valid &&
                  collect_evidence(runtime, 0, after) &&
                  after.features == before.features &&
                  after.pairs >= before.pairs && after.matches == after.pairs &&
                  after.verified == before.verified &&
                  after.rejected == before.rejected &&
                  after.tracks == before.tracks &&
                  audit_match_results(runtime, matcher_task_id, match_audit) &&
                  match_audit.match_result_count == after.matches;
  const char *matcher_mode_name = runtime.matcher_mode == MatcherMode::kAuto
      ? "auto" : runtime.matcher_mode == MatcherMode::kCpu ? "cpu" : "vulkan";
  const char *pipeline_name = runtime.matcher_pipeline == MatcherPipeline::kRolling
      ? "rolling" : "synchronous";
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
      "\"matcher_mode\":\"%s\",\"matcher_pipeline\":\"%s\","
      "\"matcher_inflight_override\":%s,\"matcher_batch_override\":%s,"
      "\"match_output_digest\":\"%s\",\"match_digest_format\":\"L3DMRD1\","
      "\"match_asset_validation\":\"sha-size-header-and-entries\","
      "\"match_asset_count\":%zu,\"duplicate_candidate_pair_mappings\":%zu,"
      "\"candidate_mapping_contiguous\":%s,\"matcher_cursor_complete\":%s,"
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
      before.tracks, after.tracks, matcher_mode_name, pipeline_name,
      runtime.matcher_inflight_override == 0 ? "null" :
          runtime.matcher_inflight_override == 1 ? "1" : "2",
      runtime.matcher_batch_override == 0 ? "null" :
          runtime.matcher_batch_override == 2 ? "2" :
          runtime.matcher_batch_override == 4 ? "4" :
          runtime.matcher_batch_override == 8 ? "8" : "12",
      match_audit.digest_hex, match_audit.match_asset_count,
      match_audit.duplicate_candidate_pair_mappings,
      match_audit.candidate_mapping_contiguous ? "true" : "false",
      match_audit.matcher_cursor_complete ? "true" : "false");
  stop_runtime(runtime);
  return ok;
}

bool run_existing_geometry(Runtime &runtime) {
  if (!start_runtime(runtime)) return false;
  Lardon3DGeometricVerifierTaskConfiguration configuration{
      lardon3d_geometric_verifier_default_parameters()};
  unsigned char fingerprint[32]{};
  lardon3d_geometric_verifier_fingerprint(&configuration.verifier, fingerprint);
  Evidence before{};
  size_t exact_before = 0;
  size_t match_results_before = 0;
  uint64_t match_frontier_before = 0;
  uint64_t pending_geometry_task_id = 0;
  uint64_t pending_track_task_id = 0;
  if (!collect_evidence(runtime, 0, before) ||
      !count_exact_gvrs(runtime, fingerprint, exact_before) ||
      !match_result_frontier(runtime, match_results_before,
                             match_frontier_before) ||
      !find_pending_geometry_task(runtime, pending_geometry_task_id,
                                  pending_track_task_id)) {
    stop_runtime(runtime);
    return false;
  }

  /* This route deliberately starts at the immutable Match Result boundary.
   * It enqueues no quality, campaign, representation, feature, index,
   * candidate, or Matcher work. Task/Queue/Governor retain their production
   * ownership, and exact current identity reuse makes a full rerun convergent.
   * The stop flag is runner-only: it suppresses only the legacy Track Builder
   * call after a completed/audited production GV Task. */
  uint64_t geometry_task_id = pending_geometry_task_id;
  Lardon3DProjectRecoverySummary recovery{};
  const char *geometry_action = pending_track_task_id != 0
                                    ? "reused-completed"
                                : pending_geometry_task_id == 0
                                    ? "enqueued"
                                    : "recovered";
  bool task_completed = true;
  bool governor_admission_ok = true;
  if (pending_track_task_id == 0) {
    begin_geometric_evidence(runtime);
    task_completed = pending_geometry_task_id != 0
        ? recover_geometry_task(runtime, geometry_task_id, recovery)
        : lardon3d_project_enqueue_geometric_verifier_task(
              &runtime.state, &configuration, &geometry_task_id) &&
              wait_completed(runtime, geometry_task_id,
                             "geometric_verifier.run current existing");
    governor_admission_ok = end_geometric_evidence(runtime);
  }
  if (!task_completed || !governor_admission_ok) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"geometric_verifier.run\",\"task_id\":%llu,"
                "\"task_action\":\"%s\",\"governor_admission_pass\":%s,"
                "\"task_errors\":1}\n",
                static_cast<unsigned long long>(geometry_task_id),
                geometry_action, governor_admission_ok ? "true" : "false");
    stop_runtime(runtime);
    return false;
  }

  std::vector<uint64_t> verified_ids;
  size_t applicable = 0;
  size_t verified = 0;
  size_t rejected = 0;
  size_t exact_after = 0;
  Evidence after{};
  size_t match_results_after = 0;
  uint64_t match_frontier_after = 0;
  uint64_t matcher_task_id = 0;
  MatchAudit match_audit{};
  Lardon3DProjectDbTask durable_task{};
  Lardon3DProjectDbGeometricVerifierTask durable_geometry{};
  if (!collect_verified_ids(runtime, fingerprint, verified_ids, &applicable, &verified,
                            &rejected) ||
      !count_exact_gvrs(runtime, fingerprint, exact_after) ||
      !collect_evidence(runtime, 0, after) ||
      !match_result_frontier(runtime, match_results_after,
                             match_frontier_after) ||
      !audit_match_results(runtime, matcher_task_id, match_audit) ||
      (pending_track_task_id == 0 &&
       lardon3d_project_db_load_task(runtime.state.project_db, geometry_task_id,
                                     &durable_task) != LARDON3D_PROJECT_DB_OK) ||
      (pending_track_task_id == 0 &&
       lardon3d_project_db_load_geometric_verifier_task(
           runtime.state.project_db, geometry_task_id,
           &durable_geometry) != LARDON3D_PROJECT_DB_OK) ||
      exact_after != applicable || verified + rejected != applicable ||
      match_results_before != before.matches ||
      match_results_after != after.matches ||
      match_audit.match_result_count != after.matches ||
      before.features != after.features || before.pairs != after.pairs ||
      before.matches != after.matches ||
      match_frontier_before != match_frontier_after ||
      (pending_track_task_id == 0 &&
       (durable_task.saved_state != TASK_COMPLETED ||
        durable_task.recovery_state != TASK_COMPLETED ||
        durable_task.progress != 100 ||
        durable_geometry.after_match_result_id != match_frontier_after))) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"current_evidence_audit\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }

  // Without the proof-only terminal seam, preserve the legacy route: Track
  // Builder receives only exact current-policy VERIFIED IDs. Historical v1/v2
  // evidence is neither replayed nor mixed, and the exact scope identity
  // reuses an existing Track Set instead of publishing duplicate lineage.
  uint64_t track_task_id = 0;
  Lardon3DProjectDbTrackSet track_set{};
  Lardon3DProjectRecoverySummary track_recovery{};
  if (!runtime.stop_after_gv && pending_track_task_id != 0 &&
      !recover_track_builder_task(runtime, pending_track_task_id,
                                  track_recovery)) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"track_builder.run recovered\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }
  if (!runtime.stop_after_gv &&
      !build_tracks(runtime, verified_ids, fingerprint, &track_task_id,
                    &track_set)) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"track_builder.run\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }
  if (pending_track_task_id != 0) track_task_id = pending_track_task_id;
  if (!runtime.stop_after_gv) {
    Evidence after_tracks{};
    if (!collect_evidence(runtime, 0, after_tracks) ||
        after_tracks.features != after.features ||
        after_tracks.pairs != after.pairs ||
        after_tracks.matches != after.matches ||
        after_tracks.verified != after.verified ||
        after_tracks.rejected != after.rejected) {
      std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                  "\"stage\":\"downstream_evidence_audit\","
                  "\"task_errors\":1}\n");
      stop_runtime(runtime);
      return false;
    }
    after = after_tracks;
  }
  if (runtime.stop_after_gv && before.tracks != after.tracks) {
    std::printf("{\"record\":\"existing_geometry_summary\",\"ok\":false,"
                "\"stage\":\"downstream_stop_audit\",\"task_errors\":1}\n");
    stop_runtime(runtime);
    return false;
  }

  const size_t created = exact_after >= exact_before ? exact_after - exact_before : 0;
  char fingerprint_hex[65]{};
  static constexpr char digits[] = "0123456789abcdef";
  for (size_t index = 0; index < 32; ++index) {
    fingerprint_hex[index * 2] = digits[fingerprint[index] >> 4U];
    fingerprint_hex[index * 2 + 1] = digits[fingerprint[index] & 0x0fU];
  }
  std::printf(
      "{\"record\":\"existing_geometry_summary\",\"ok\":true,"
      "\"verifier_version\":%u,\"verifier_fingerprint\":\"%s\","
      "\"geometry_task_id\":%llu,\"geometry_task_action\":\"%s\","
      "\"geometry_recovery_inspected\":%zu,\"geometry_recovery_resumed\":%zu,"
      "\"geometry_task_state\":\"COMPLETE\",\"geometry_task_progress\":%u,"
      "\"geometry_task_sequence_count\":%u,"
      "\"geometry_cursor_after_match_result_id\":%llu,"
      "\"geometry_cursor_complete\":true,\"match_results_consumed\":%zu,"
      "\"track_task_id\":%llu,\"applicable_matched_parents\":%zu,"
      "\"verified\":%zu,\"rejected\":%zu,\"gvrs_before\":%zu,"
      "\"gvrs_created\":%zu,\"gvrs_after\":%zu,"
      "\"duplicate_gvr_mappings\":0,\"gvr_mapping_order_canonical\":true,"
      "\"feature_sets_before\":%zu,\"feature_sets_after\":%zu,"
      "\"candidate_pairs_before\":%zu,\"candidate_pairs_after\":%zu,"
      "\"match_results_before\":%zu,\"match_results_after\":%zu,"
      "\"match_output_digest\":\"%s\",\"match_digest_format\":\"L3DMRD1\","
      "\"match_asset_count\":%zu,\"duplicate_candidate_pair_mappings\":%zu,"
      "\"matcher_task_id\":%llu,\"matcher_cursor_complete\":%s,"
      "\"track_set_id\":%llu,\"track_count\":%llu,"
      "\"track_set_reused\":%s,"
      "\"tracks_before\":%zu,\"tracks_after\":%zu,"
      "\"stop_after_gv\":%s,\"feature_replay\":0,\"candidate_replay\":0,"
      "\"matcher_replay\":0,\"task_errors\":0,\"upstream_replayed\":false,"
      "\"mixed_verifier_lineage\":false,\"track_builder_enqueued\":%s,"
      "\"sparse_sfm_run\":false,\"governor_admission_pass\":true}\n",
      LARDON3D_GEOMETRIC_VERIFIER_VERSION, fingerprint_hex,
      static_cast<unsigned long long>(geometry_task_id),
      geometry_action, recovery.inspected, recovery.resumed,
      durable_task.progress, durable_task.sequence_count,
      static_cast<unsigned long long>(durable_geometry.after_match_result_id),
      after.matches,
      static_cast<unsigned long long>(track_task_id), applicable, verified, rejected, exact_before,
      created, exact_after, before.features, after.features, before.pairs,
      after.pairs, before.matches, after.matches, match_audit.digest_hex,
      match_audit.match_asset_count,
      match_audit.duplicate_candidate_pair_mappings,
      static_cast<unsigned long long>(matcher_task_id),
      match_audit.matcher_cursor_complete ? "true" : "false",
      static_cast<unsigned long long>(track_set.track_set_id),
      static_cast<unsigned long long>(track_set.track_count),
      track_task_id == 0 && track_set.track_set_id != 0 ? "true" : "false",
      before.tracks, after.tracks, runtime.stop_after_gv ? "true" : "false",
      runtime.stop_after_gv ? "false" : "true");
  stop_runtime(runtime);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  /* The runner is a production-runtime consumer even though its evidence
   * controls are benchmark-only. Establish Mesa's no-disk-worker policy before
   * argument handling or any application pthread creation. */
  const Lardon3DResourceDriverPolicyResult driver_policy =
      lardon3d_resource_governor_internal_configure_driver_policy();
  if (driver_policy == LARDON3D_RESOURCE_DRIVER_POLICY_FAILED ||
      driver_policy == LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE) {
    std::fprintf(stderr,
                 "MESA_SHADER_CACHE_DISABLE must be true for safe CPU affinity\n");
    return 2;
  }
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage(argv[0]);
    return 2;
  }
  /* Reject this contradictory operational request before opening a project or
   * allocating a durable Task ID. A Vulkan Matcher has an immutable GPU=1
   * estimate, while GPU budget zero expressly removes GPU admission. */
  if (options.matcher_mode == MatcherMode::kVulkan && options.has_gpu_budget &&
      options.gpu_budget == 0) {
    std::fprintf(stderr, "--matcher-mode vulkan requires --gpu-budget 1\n");
    return 2;
  }
  if (options.has_matcher_inflight_override && options.has_gpu_budget &&
      options.gpu_budget == 0) {
    /* A forced A/B cohort has no CPU admission alternative. Reject the
     * contradictory budget before project inspection so a typo cannot create
     * a durable Task that merely waits or silently measures CPU. */
    std::fprintf(stderr, "--matcher-inflight requires --gpu-budget 1\n");
    return 2;
  }
  if (options.matcher_mode == MatcherMode::kCpu &&
      options.matcher_pipeline == MatcherPipeline::kSynchronous) {
    std::fprintf(stderr,
                 "--matcher-pipeline synchronous requires auto or vulkan; "
                 "CPU has no Vulkan fence baseline\n");
    return 2;
  }
  if (options.has_matcher_inflight_override &&
      options.matcher_mode != MatcherMode::kAuto) {
    std::fprintf(stderr,
                 "--matcher-inflight requires normal AUTO Matcher mode\n");
    return 2;
  }
  if (options.has_matcher_batch_override &&
      !options.has_matcher_inflight_override) {
    std::fprintf(stderr,
                 "--matcher-batch requires --matcher-inflight 1 or 2\n");
    return 2;
  }
  if (options.has_matcher_batch_override &&
      options.matcher_pipeline != MatcherPipeline::kRolling) {
    std::fprintf(stderr, "--matcher-batch requires rolling pipeline\n");
    return 2;
  }
  if (options.matcher_pipeline == MatcherPipeline::kSynchronous &&
      options.matcher_inflight_override == 2) {
    std::fprintf(stderr,
                 "--matcher-pipeline synchronous supports inflight 1 only\n");
    return 2;
  }
  /* These environment seams are compiled only into this runner's private
   * Matcher Task object. Defaults actively clear inherited controls, while the
   * scoped owner restores their exact prior values on every return path. */
  ScopedMatcherBenchmarkEnvironment benchmark_environment;
  if (!benchmark_environment.configure(options)) {
    std::fprintf(stderr, "unable to configure benchmark Matcher controls\n");
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
    runtime.gpu_budget = options.gpu_budget;
    runtime.has_gpu_budget = options.has_gpu_budget;
    runtime.matcher_mode = options.matcher_mode;
    runtime.matcher_pipeline = options.matcher_pipeline;
    runtime.matcher_inflight_override = options.matcher_inflight_override;
    runtime.matcher_batch_override = options.matcher_batch_override;
    runtime.matcher_needed = options.resume_pre_gv_existing;
    runtime.stop_after_matcher = options.stop_after_matcher;
    runtime.stop_after_gv = options.stop_after_gv;
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
  runtime.matcher_needed = true;
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
  uint64_t matcher_task_id = 0;
  std::vector<uint64_t> verified_ids;
  Lardon3DGeometricVerifierParameters verifier = lardon3d_geometric_verifier_default_parameters();
  unsigned char verifier_fingerprint[32]{};
  lardon3d_geometric_verifier_fingerprint(&verifier, verifier_fingerprint);
  if (ok)
    ok = downstream(runtime, features, orb, geometry_task_id, matcher_task_id,
                    verified_ids);
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
  MatchAudit match_audit{};
  if (ok)
    ok = collect_evidence(runtime, execution.execution_id, evidence) &&
         audit_match_results(runtime, matcher_task_id, match_audit) &&
         match_audit.match_result_count == evidence.matches;
  if (ok) {
    std::printf("{\"record\":\"summary\",\"mode\":\"%s\","
                "\"execution_id\":%llu,\"selected\":%zu,\"feature_sets\":%zu,"
                "\"candidate_pairs\":%zu,\"matches\":%zu,\"verified\":%zu,"
                "\"rejected\":%zu,\"tracks\":%zu,"
                "\"matcher_mode\":\"auto\",\"matcher_pipeline\":\"rolling\","
                "\"matcher_inflight_override\":null,"
                "\"matcher_batch_override\":null,"
                "\"match_output_digest\":\"%s\","
                "\"match_digest_format\":\"L3DMRD1\","
                "\"match_asset_validation\":\"sha-size-header-and-entries\","
                "\"match_asset_count\":%zu,"
                "\"duplicate_candidate_pair_mappings\":%zu,"
                "\"candidate_mapping_contiguous\":%s,"
                "\"matcher_cursor_complete\":%s,"
                "\"orb_max_features_per_set\":8192,"
                "\"visual_index_max_features_per_set\":1024,"
                "\"visual_index_feature_set_limit\":4096,"
                "\"candidate_top_k_limit\":64,"
                "\"calibration_attached\":false,\"sparse_sfm_run\":false}\n",
                mode_name, static_cast<unsigned long long>(execution.execution_id),
                evidence.selected, evidence.features, evidence.pairs, evidence.matches,
                evidence.verified, evidence.rejected, evidence.tracks,
                match_audit.digest_hex, match_audit.match_asset_count,
                match_audit.duplicate_candidate_pair_mappings,
                match_audit.candidate_mapping_contiguous ? "true" : "false",
                match_audit.matcher_cursor_complete ? "true" : "false");
  }
  stop_runtime(runtime);
  return ok ? 0 : 1;
}
