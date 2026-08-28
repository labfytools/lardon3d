#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/hardware_profile.h>
#include <lardon3d/photo_quality_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>
}

namespace {

constexpr uint64_t kAnalysisWorkingBytes = UINT64_C(20) * 1024u * 1024u;

enum class Mode { kA6000, kS21 };

struct Options {
  Mode mode{};
  bool has_mode{};
  std::filesystem::path project_dir;
  std::vector<std::filesystem::path> roots;
};

/* This validation-only mirror keeps the reported admission estimate aligned
 * with the v1 Task contract: retained request/context storage plus one bounded
 * 20 MiB group analysis allowance. It does not own or execute Task state. */
struct PhotoQualityContextLayout {
  char project_path[LARDON3D_APP_STATE_PATH_CAPACITY];
  Lardon3DProjectDb *database;
  Lardon3DResourceGovernor *governor;
  uint64_t scanset_id;
  std::vector<Lardon3DAcquisitionCampaignSource> sources;
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations;
  std::vector<unsigned char> encoded;
  Lardon3DAcquisitionCampaignPlan plan;
};

void usage(const char *program) {
  std::fprintf(stderr,
               "Usage: %s --mode a6000|s21 --project-dir ABSOLUTE_EMPTY_DIR "
               "--root ABSOLUTE_DIR [--root ABSOLUTE_DIR ...]\n",
               program);
}

bool parse_options(int argc, char **argv, Options &options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if ((argument == "--mode" || argument == "--project-dir" || argument == "--root") &&
        i + 1 >= argc)
      return false;
    if (argument == "--mode") {
      const std::string value(argv[++i]);
      if (value == "a6000") options.mode = Mode::kA6000;
      else if (value == "s21") options.mode = Mode::kS21;
      else return false;
      options.has_mode = true;
    } else if (argument == "--project-dir") {
      options.project_dir = argv[++i];
    } else if (argument == "--root") {
      options.roots.emplace_back(argv[++i]);
    } else if (argument == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else {
      return false;
    }
  }
  return options.has_mode && !options.project_dir.empty() && !options.roots.empty() &&
         options.roots.size() <= LARDON3D_ACQUISITION_CAMPAIGN_MAX_ROOTS;
}

bool canonical_directory(const std::filesystem::path &input, std::filesystem::path &output) {
  std::error_code error;
  if (!input.is_absolute()) return false;
  output = std::filesystem::canonical(input, error);
  return !error && std::filesystem::is_directory(output, error) && !error &&
         output.string().size() < LARDON3D_ACQUISITION_CAMPAIGN_PATH_CAPACITY;
}

bool prepare_project_directory(const std::filesystem::path &input,
                               std::filesystem::path &output) {
  if (!input.is_absolute()) return false;
  output = input.lexically_normal();
  if (output != input || output.string().size() >= LARDON3D_APP_STATE_PATH_CAPACITY) return false;
  std::error_code error;
  if (std::filesystem::exists(output, error)) {
    if (error || !std::filesystem::is_directory(output, error) || error ||
        !std::filesystem::is_empty(output, error) || error)
      return false;
  } else if (!std::filesystem::create_directories(output, error) || error) {
    return false;
  }
  return std::filesystem::create_directories(output / ".lardon3d" / "checkpoints", error) &&
         !error;
}

const char *mode_name(Mode mode) { return mode == Mode::kA6000 ? "a6000" : "s21"; }

std::string stem(const char *path) {
  const char *name = std::strrchr(path, '/');
  name = name == nullptr ? path : name + 1;
  const char *dot = std::strrchr(name, '.');
  return std::string(name, dot == nullptr ? std::strlen(name) : static_cast<size_t>(dot - name));
}

bool build_confirmations(Mode mode, const Lardon3DAcquisitionCampaignDiscovery &discovery,
                         std::vector<Lardon3DAcquisitionCampaignConfirmation> &confirmations) {
  if (mode == Mode::kS21) {
    confirmations.reserve(discovery.source_count);
    for (size_t i = 0; i < discovery.source_count; ++i) {
      if (discovery.sources[i].source_kind != LARDON3D_ACQUISITION_SOURCE_JPEG) return false;
      Lardon3DAcquisitionCampaignConfirmation confirmation{};
      confirmation.source_count = 1;
      confirmation.source_indices[0] = i;
      confirmations.push_back(confirmation);
    }
    return true;
  }

  struct Pair { size_t raw = SIZE_MAX; size_t jpeg = SIZE_MAX; };
  std::map<std::string, Pair> pairs;
  for (size_t i = 0; i < discovery.source_count; ++i) {
    Pair &pair = pairs[stem(discovery.sources[i].path)];
    if (discovery.sources[i].source_kind == LARDON3D_ACQUISITION_SOURCE_RAW) {
      if (pair.raw != SIZE_MAX) return false;
      pair.raw = i;
    } else if (discovery.sources[i].source_kind == LARDON3D_ACQUISITION_SOURCE_JPEG) {
      if (pair.jpeg != SIZE_MAX) return false;
      pair.jpeg = i;
    } else {
      return false;
    }
  }
  confirmations.reserve(pairs.size());
  for (const auto &entry : pairs) {
    if (entry.second.raw == SIZE_MAX || entry.second.jpeg == SIZE_MAX) return false;
    Lardon3DAcquisitionCampaignConfirmation confirmation{};
    confirmation.source_count = 2;
    confirmation.source_indices[0] = entry.second.raw;
    confirmation.source_indices[1] = entry.second.jpeg;
    confirmations.push_back(confirmation);
  }
  return confirmations.size() * 2u == discovery.source_count;
}

void json_string(const char *text) {
  std::putchar('"');
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    switch (*p) {
      case '"': std::fputs("\\\"", stdout); break;
      case '\\': std::fputs("\\\\", stdout); break;
      case '\b': std::fputs("\\b", stdout); break;
      case '\f': std::fputs("\\f", stdout); break;
      case '\n': std::fputs("\\n", stdout); break;
      case '\r': std::fputs("\\r", stdout); break;
      case '\t': std::fputs("\\t", stdout); break;
      default:
        if (*p < 0x20u) std::printf("\\u%04x", static_cast<unsigned>(*p));
        else std::putchar(*p);
    }
  }
  std::putchar('"');
}

const char *status_name(Lardon3DPhotoQualityMetricStatus status) {
  switch (status) {
    case LARDON3D_PHOTO_QUALITY_METRIC_OK: return "OK";
    case LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE: return "UNAVAILABLE";
    case LARDON3D_PHOTO_QUALITY_METRIC_INVALID_INPUT: return "INVALID_INPUT";
    case LARDON3D_PHOTO_QUALITY_METRIC_DECODE_ERROR: return "DECODE_ERROR";
  }
  return "UNKNOWN";
}

const char *recommendation_name(Lardon3DPhotoQualityRecommendation recommendation) {
  switch (recommendation) {
    case LARDON3D_PHOTO_QUALITY_GOOD: return "GOOD";
    case LARDON3D_PHOTO_QUALITY_SUSPECT: return "SUSPECT";
    case LARDON3D_PHOTO_QUALITY_REJECT: return "REJECT";
  }
  return "NONE";
}

bool wait_terminal(Lardon3DTaskQueue *queue, Lardon3DProjectDb *database, uint64_t task_id,
                   Lardon3DTaskSnapshot &snapshot, Lardon3DProjectDbTask &durable) {
  for (;;) {
    if (!lardon3d_task_queue_get(queue, task_id, &snapshot)) return false;
    if (snapshot.state == TASK_COMPLETED || snapshot.state == TASK_FAILED ||
        snapshot.state == TASK_CANCELLED) {
      /* Queue terminal state precedes the finished callback. Report only after
       * that callback has durably published the matching generic Task state and
       * sequence count, so validation never samples an intermediate row. */
      if (lardon3d_project_db_load_task(database, task_id, &durable) != LARDON3D_PROJECT_DB_OK)
        return false;
      if (durable.saved_state == snapshot.state) return true;
    }
    usleep(100000);
  }
}

void emit_group(const Lardon3DAcquisitionCampaignDiscovery &discovery,
                const Lardon3DAcquisitionCampaignGroup &group,
                const Lardon3DProjectDbPhotoQualityResult &result) {
  std::printf("{\"record\":\"group\",\"group_id\":%u,\"basis\":%u,\"source_indices\":[",
              result.group_id, static_cast<unsigned>(group.basis));
  for (size_t i = 0; i < group.source_count; ++i)
    std::printf("%s%zu", i == 0 ? "" : ",", group.source_indices[i]);
  std::fputs("],\"source_paths\":[", stdout);
  for (size_t i = 0; i < group.source_count; ++i) {
    if (i != 0) std::putchar(',');
    json_string(discovery.sources[group.source_indices[i]].path);
  }
  std::fputs("],\"proxy_source_index\":", stdout);
  if (result.proxy_source_index == UINT32_MAX) std::fputs("null,\"proxy_path\":null", stdout);
  else {
    std::printf("%u,\"proxy_path\":", result.proxy_source_index);
    json_string(discovery.sources[result.proxy_source_index].path);
  }
  std::printf(",\"status\":\"%s\",\"recommendation\":\"%s\",\"reasons\":",
              status_name(result.metrics.status),
              recommendation_name(result.metrics.recommendation));
  json_string(result.metrics.reasons);
  std::printf(",\"decoded_width\":%u,\"decoded_height\":%u,\"analysis_width\":%u,"
              "\"analysis_height\":%u,\"sharpness_raw\":%.17g,"
              "\"sharpness_normalized\":%.17g,\"clipped_black_fraction\":%.17g,"
              "\"clipped_white_fraction\":%.17g,\"contrast_raw\":%.17g,"
              "\"contrast_normalized\":%.17g,\"low_texture_fraction\":%.17g}\n",
              result.metrics.decoded_width, result.metrics.decoded_height,
              result.metrics.analysis_width, result.metrics.analysis_height,
              result.metrics.sharpness_raw, result.metrics.sharpness_normalized,
              result.metrics.clipped_black_fraction, result.metrics.clipped_white_fraction,
              result.metrics.contrast_raw, result.metrics.contrast_normalized,
              result.metrics.low_texture_fraction);
}

}  // namespace

extern "C" const Lardon3DTaskKindRegistry *lardon3d_task_kind_registry_production(void) {
  return nullptr;
}

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, options)) { usage(argv[0]); return 2; }

  std::filesystem::path project_dir;
  if (!prepare_project_directory(options.project_dir, project_dir)) {
    std::fprintf(stderr, "project directory must be an absolute, normalized empty directory\n");
    return 2;
  }
  std::vector<Lardon3DAcquisitionCampaignRoot> roots(options.roots.size());
  for (size_t i = 0; i < options.roots.size(); ++i) {
    std::filesystem::path canonical;
    if (!canonical_directory(options.roots[i], canonical)) {
      std::fprintf(stderr, "root must resolve to a bounded directory: %s\n",
                   options.roots[i].c_str());
      return 2;
    }
    const std::string path = canonical.string();
    std::memcpy(roots[i].path, path.c_str(), path.size() + 1u);
  }

  auto discovery = std::make_unique<Lardon3DAcquisitionCampaignDiscovery>();
  const auto discovery_result =
      lardon3d_acquisition_campaign_discover(roots.data(), roots.size(), discovery.get());
  if (discovery_result != LARDON3D_ACQUISITION_CAMPAIGN_OK || discovery->source_count == 0) {
    std::fprintf(stderr, "acquisition discovery failed: %u\n",
                 static_cast<unsigned>(discovery_result));
    return 1;
  }
  std::vector<Lardon3DAcquisitionCampaignConfirmation> confirmations;
  if (!build_confirmations(options.mode, *discovery, confirmations)) {
    std::fprintf(stderr, "%s source set does not satisfy the required exact grouping shape\n",
                 mode_name(options.mode));
    return 1;
  }
  auto plan = std::make_unique<Lardon3DAcquisitionCampaignPlan>();
  if (lardon3d_acquisition_campaign_plan(discovery->sources, discovery->source_count,
                                         confirmations.data(), confirmations.size(), plan.get()) !=
          LARDON3D_ACQUISITION_CAMPAIGN_OK ||
      plan->group_count != confirmations.size() ||
      plan->summary.explicit_group_count != confirmations.size()) {
    std::fprintf(stderr, "explicit campaign planning failed\n");
    return 1;
  }

  const std::string database_path = (project_dir / "project.lardon3d").string();
  char database_error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  Lardon3DProjectDb *database = nullptr;
  if (lardon3d_project_db_open(database_path.c_str(), &database, database_error) !=
      LARDON3D_PROJECT_DB_OK) {
    std::fprintf(stderr, "project database open failed: %s\n", database_error);
    return 1;
  }
  Lardon3DProjectDbScanSet scanset{};
  if (lardon3d_project_db_create_scanset(database, mode_name(options.mode), &scanset) !=
      LARDON3D_PROJECT_DB_OK) {
    std::fprintf(stderr, "ScanSet creation failed\n");
    lardon3d_project_db_close(database);
    return 1;
  }

  Lardon3DHardwareProfile profile{};
  char hardware_error[256]{};
  Lardon3DResourcePolicy policy{};
  if (!lardon3d_hardware_profile_detect(&profile, hardware_error, sizeof(hardware_error)) ||
      !lardon3d_resource_policy_default(&profile, &policy)) {
    std::fprintf(stderr, "hardware/resource policy detection failed: %s\n", hardware_error);
    lardon3d_project_db_close(database);
    return 1;
  }
  Lardon3DResourceGovernor *governor = lardon3d_resource_governor_create(&profile, &policy);
  Lardon3DTaskQueue *queue = governor ? lardon3d_task_queue_create(governor, 2) : nullptr;
  if (!queue) {
    std::fprintf(stderr, "Queue/Governor creation failed\n");
    lardon3d_resource_governor_destroy(governor);
    lardon3d_project_db_close(database);
    return 1;
  }
  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = database;
  state.resource_governor = governor;
  state.task_queue = queue;
  state.hardware_profile = profile;
  const std::string project_path = project_dir.string();
  std::memcpy(state.project_path, project_path.c_str(), project_path.size() + 1u);

  Lardon3DPhotoQualityTaskRequest request{discovery->sources, discovery->source_count,
                                          confirmations.data(), confirmations.size()};
  size_t encoded_size = 0;
  if (!lardon3d_photo_quality_request_encode(&request, nullptr, 0, &encoded_size)) {
    std::fprintf(stderr, "request size calculation failed\n");
    lardon3d_task_queue_destroy(queue);
    lardon3d_resource_governor_destroy(governor);
    lardon3d_project_db_close(database);
    return 1;
  }
  const uint64_t task_memory_estimate =
      kAnalysisWorkingBytes + sizeof(PhotoQualityContextLayout) +
      discovery->source_count * sizeof(Lardon3DAcquisitionCampaignSource) +
      confirmations.size() * sizeof(Lardon3DAcquisitionCampaignConfirmation) + encoded_size;
  const auto started = std::chrono::steady_clock::now();
  uint64_t task_id = 0;
  if (!lardon3d_project_enqueue_photo_quality(&state, scanset.scanset_id, &request, &task_id)) {
    std::fprintf(stderr, "photo_quality.triage enqueue failed\n");
    lardon3d_task_queue_destroy(queue);
    lardon3d_resource_governor_destroy(governor);
    lardon3d_project_db_close(database);
    return 1;
  }
  Lardon3DTaskSnapshot terminal{};
  Lardon3DProjectDbTask durable_task{};
  if (!wait_terminal(queue, database, task_id, terminal, durable_task) ||
      terminal.state != TASK_COMPLETED) {
    std::fprintf(stderr, "photo_quality.triage ended in %s: %s\n",
                 lardon3d_task_state_name(terminal.state), terminal.message);
    lardon3d_task_queue_destroy(queue);
    lardon3d_resource_governor_destroy(governor);
    lardon3d_project_db_close(database);
    return 1;
  }

  size_t good = 0, suspect = 0, reject = 0, unavailable = 0, metric_errors = 0;
  for (uint32_t group_index = 0; group_index < plan->group_count; ++group_index) {
    const uint32_t group_id = plan->groups[group_index].group_id;
    Lardon3DProjectDbPhotoQualityResult result{};
    if (lardon3d_project_db_load_photo_quality_result(database, task_id, group_id, &result) !=
        LARDON3D_PROJECT_DB_OK) {
      std::fprintf(stderr, "missing durable result for group %u\n", group_id);
      lardon3d_task_queue_destroy(queue);
      lardon3d_resource_governor_destroy(governor);
      lardon3d_project_db_close(database);
      return 1;
    }
    if (result.metrics.status == LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE) ++unavailable;
    else if (result.metrics.status != LARDON3D_PHOTO_QUALITY_METRIC_OK) ++metric_errors;
    if (result.metrics.recommendation == LARDON3D_PHOTO_QUALITY_GOOD) ++good;
    else if (result.metrics.recommendation == LARDON3D_PHOTO_QUALITY_SUSPECT) ++suspect;
    else if (result.metrics.recommendation == LARDON3D_PHOTO_QUALITY_REJECT) ++reject;
    emit_group(*discovery, plan->groups[group_index], result);
  }
  struct rusage usage{};
  const bool loaded_usage = getrusage(RUSAGE_SELF, &usage) == 0;
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("{\"record\":\"summary\",\"mode\":\"%s\",\"task_id\":%llu,"
              "\"sources\":%zu,\"groups\":%zu,\"good\":%zu,\"suspect\":%zu,"
              "\"reject\":%zu,\"unavailable\":%zu,\"metric_errors\":%zu,"
              "\"discovered_entries\":%zu,\"unsupported_entries\":%zu,"
              "\"metadata_ok\":%zu,\"metadata_errors\":%zu,"
              "\"request_encoded_bytes\":%zu,\"task_memory_estimate_bytes\":%llu,"
              "\"sequence_count\":%u,\"elapsed_seconds\":%.6f,\"peak_rss_kib\":%ld}\n",
              mode_name(options.mode), static_cast<unsigned long long>(task_id),
              discovery->source_count, plan->group_count, good, suspect, reject, unavailable,
              metric_errors, discovery->summary.discovered_entry_count,
              discovery->summary.unsupported_entry_count, discovery->summary.metadata_ok_count,
              discovery->summary.metadata_error_count, encoded_size,
              static_cast<unsigned long long>(task_memory_estimate),
              durable_task.sequence_count, elapsed,
              loaded_usage ? usage.ru_maxrss : -1L);

  lardon3d_task_queue_destroy(queue);
  lardon3d_resource_governor_destroy(governor);
  lardon3d_project_db_close(database);
  return loaded_usage ? 0 : 1;
}
