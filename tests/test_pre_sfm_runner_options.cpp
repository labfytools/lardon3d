#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#define main lardon3d_pre_sfm_real_execution_main
#include "pre_sfm_real_execution.cpp"
#undef main

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "failed line " << __LINE__ << ": " << #condition << '\n';  \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

static bool parse_case(std::initializer_list<const char *> arguments,
                       Options &options) {
  std::vector<std::string> storage(arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size());
  for (std::string &argument : storage) argv.push_back(argument.data());
  return parse_options(static_cast<int>(argv.size()), argv.data(), options);
}

static int invoke_case(std::initializer_list<const char *> arguments) {
  std::vector<std::string> storage(arguments.begin(), arguments.end());
  std::vector<char *> argv;
  argv.reserve(storage.size());
  for (std::string &argument : storage) argv.push_back(argument.data());
  return lardon3d_pre_sfm_real_execution_main(
      static_cast<int>(argv.size()), argv.data());
}

static bool capture_matcher_evidence(Runtime &runtime, std::string &output) {
  FILE *capture = std::tmpfile();
  if (!capture) return false;
  const int saved_stdout = dup(STDOUT_FILENO);
  if (saved_stdout < 0 || std::fflush(stdout) != 0 ||
      dup2(fileno(capture), STDOUT_FILENO) < 0) {
    if (saved_stdout >= 0) close(saved_stdout);
    std::fclose(capture);
    return false;
  }
  (void)end_matcher_evidence(runtime);
  const bool flushed = std::fflush(stdout) == 0;
  const bool restored = dup2(saved_stdout, STDOUT_FILENO) >= 0;
  close(saved_stdout);
  bool read_ok = flushed && restored && std::fseek(capture, 0, SEEK_SET) == 0;
  char buffer[1024];
  while (read_ok) {
    const size_t count = std::fread(buffer, 1, sizeof(buffer), capture);
    output.append(buffer, count);
    if (count < sizeof(buffer)) {
      read_ok = std::feof(capture) != 0 && std::ferror(capture) == 0;
      break;
    }
  }
  return std::fclose(capture) == 0 && read_ok;
}

static bool json_valid_and_submit_cpu(const std::string &line,
                                      bool require_submit_cpu) {
  sqlite3 *database = nullptr;
  sqlite3_stmt *statement = nullptr;
  bool ok = sqlite3_open(":memory:", &database) == SQLITE_OK &&
            sqlite3_prepare_v2(
                database,
                "SELECT json_valid(?1), json_extract(?1, "
                "'$.vulkan_submit_cpu_ns')",
                -1, &statement, nullptr) == SQLITE_OK &&
            sqlite3_bind_text(statement, 1, line.c_str(), -1,
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_ROW &&
            sqlite3_column_int(statement, 0) == 1;
  if (ok && require_submit_cpu) {
    ok = sqlite3_column_type(statement, 1) == SQLITE_INTEGER &&
         sqlite3_column_int64(statement, 1) == 123456789;
  }
  if (statement) sqlite3_finalize(statement);
  if (database && sqlite3_close(database) != SQLITE_OK) ok = false;
  return ok;
}

static bool matcher_evidence_aggregate_case() {
  Lardon3DHardwareProfile profile{};
  profile.logical_cpu_count = 16;
  profile.page_size_bytes = 4096;
  profile.memory_total_bytes = UINT64_C(16) * 1024 * 1024 * 1024;
  profile.gpu_available = true;
  profile.gpu_uses_shared_memory = true;
  std::snprintf(profile.cpu_architecture, sizeof(profile.cpu_architecture),
                "%s", "test");
  Lardon3DResourcePolicy policy{};
  if (!lardon3d_resource_policy_default(&profile, &policy)) return false;
  policy.gpu_slot_capacity = 1;
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  if (!governor) return false;

  Lardon3DTaskCapabilityEnvelope envelope{};
  envelope.count = 1;
  envelope.capabilities[0].estimate.memory_bytes_per_item = 10 * 1024 * 1024;
  envelope.capabilities[0].estimate.minimum_batch_size = 4;
  envelope.capabilities[0].estimate.maximum_batch_size = 4;
  envelope.capabilities[0].estimate.desired_cpu_threads = 1;
  envelope.capabilities[0].estimate.desired_gpu_slots = 1;
  envelope.capabilities[0].estimate.desired_io_slots = 1;
  envelope.capabilities[0].estimate.task_class = LARDON3D_RESOURCE_TASK_GPU;
  envelope.capabilities[0].backend = LARDON3D_RESOURCE_BACKEND_ORB_VULKAN;
  envelope.capabilities[0].inflight_limit = 2;
  envelope.capabilities[0].minimum_inflight_limit = 2;
  envelope.capabilities[0].gpu_memory_bytes_per_inflight = 640 * 1024;
  Lardon3DResourceSnapshot snapshot{};
  if (clock_gettime(CLOCK_MONOTONIC, &snapshot.captured_at) != 0) {
    lardon3d_resource_governor_destroy(governor);
    return false;
  }
  snapshot.memory_available_bytes = UINT64_C(8) * 1024 * 1024 * 1024;
  snapshot.swap_activity_known = true;
  Lardon3DResourceCapabilitySelection selection{};
  Lardon3DResourceReservation *reservation = nullptr;
  const bool admitted =
      lardon3d_resource_governor_internal_reserve_capability(
          governor, &snapshot, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &envelope, &selection,
          &reservation) &&
      reservation;
  const bool released =
      admitted && lardon3d_resource_governor_release(governor, reservation);
  Lardon3DResourceExecutionMetrics metrics{};
  metrics.vulkan_submits = 2;
  metrics.vulkan_completions = 2;
  metrics.vulkan_submit_cpu_ns = 123456789;
  metrics.vulkan_fence_wait_ns = 11;
  metrics.vulkan_readback_ns = 12;
  metrics.vulkan_gpu_time_known = true;
  metrics.vulkan_gpu_ns = 13;
  metrics.vulkan_starvation_ns = 14;
  metrics.matcher_cpu_ns = 15;
  metrics.publication_ns = 16;
  metrics.local_ineligible_fallback_items = 1;
  const bool recorded = released &&
      lardon3d_resource_governor_internal_record_fallback_items(
          governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &selection,
          LARDON3D_RESOURCE_FALLBACK_ITEM_LOCAL_INELIGIBLE, 1) &&
      lardon3d_resource_governor_internal_record_sequence_execution_metrics(
          governor, LARDON3D_MATCHER_TASK_KIND,
          LARDON3D_MATCHER_TASK_KIND_VERSION, &selection, 1000, 1,
          LARDON3D_RESOURCE_BACKEND_MIXED,
          "vulkan-and-ineligible-pair-cpu-fallback", &metrics);
  Runtime runtime{};
  runtime.state.resource_governor = governor;
  runtime.state.hardware_profile = profile;
  runtime.matcher_inflight_override = 2;
  runtime.matcher_batch_override = 4;
  begin_matcher_evidence(runtime);
  std::string output;
  const bool captured = recorded && capture_matcher_evidence(runtime, output);
  lardon3d_resource_governor_destroy(governor);
  if (!captured) {
    std::cerr << "unable to construct/capture Matcher evidence fixture: "
              << "admitted=" << admitted << " released=" << released
              << " recorded=" << recorded << '\n';
    return false;
  }

  bool saw_aggregate = false;
  size_t offset = 0;
  while (offset < output.size()) {
    const size_t newline = output.find('\n', offset);
    const size_t end = newline == std::string::npos ? output.size() : newline;
    const std::string line = output.substr(offset, end - offset);
    if (!line.empty()) {
      const bool aggregate = line.find(
          "\"record\":\"matcher_evidence_aggregate\"") !=
          std::string::npos;
      if (!json_valid_and_submit_cpu(line, aggregate)) {
        std::cerr << "invalid Matcher evidence JSON: " << line << '\n';
        return false;
      }
      saw_aggregate = saw_aggregate || aggregate;
    }
    if (newline == std::string::npos) break;
    offset = newline + 1;
  }
  const bool exact_field =
      output.find("\"vulkan_submit_cpu_ns\":123456789") !=
      std::string::npos;
  const bool affinity_fields =
      output.find("\"runtime_thread_policy_active\":true") !=
          std::string::npos &&
      output.find("\"mesa_shader_cache_disabled\":true") !=
          std::string::npos &&
      output.find("\"runtime_thread_policy_reason\":"
                  "\"worker-self-affinity-plus-mesa-disk-cache-disabled\"") !=
          std::string::npos &&
      output.find("auxiliary_affinity_active") == std::string::npos;
  const bool inflight_field =
      output.find("\"matcher_inflight_override\":2") != std::string::npos;
  const bool batch_field =
      output.find("\"matcher_batch_override\":4") != std::string::npos;
  const bool experiment_fields =
      output.find("\"experiment_valid\":false") != std::string::npos &&
      output.find("\"experiment_reason\":\"backend-telemetry-unavailable\"") !=
          std::string::npos &&
      output.find("\"local_ineligible_fallback_sequences\":1") !=
          std::string::npos &&
      output.find("\"local_ineligible_fallback_items\":1") !=
          std::string::npos &&
      output.find("\"backend_failure_fallback_items\":0") !=
          std::string::npos &&
      output.find("\"backend_other_fallback_items\":0") !=
          std::string::npos &&
      output.find(
          "\"comparison_requires_equal_local_ineligible_fallback_items\":"
          "true") != std::string::npos;
  if (!saw_aggregate || !exact_field || !affinity_fields || !inflight_field ||
      !batch_field || !experiment_fields)
    std::cerr << "missing exact Matcher aggregate field: " << output << '\n';
  return saw_aggregate && exact_field && affinity_fields && inflight_field &&
         batch_field && experiment_fields;
}

static bool forced_matcher_experiment_validation_case() {
  Runtime runtime{};
  runtime.matcher_inflight_override = 1;
  Lardon3DResourceSequenceAggregate aggregate{};
  aggregate.admission_count = 2;
  aggregate.sequence_count = 2;
  aggregate.selected_backend_admissions[
      LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] = 2;
  aggregate.actual_backend_sequences[
      LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] = 2;
  aggregate.vulkan_submits = 4;
  aggregate.vulkan_completions = 4;
  Lardon3DResourceSequenceDiagnostic last{};
  last.backend = LARDON3D_RESOURCE_BACKEND_ORB_VULKAN;
  last.actual_backend = LARDON3D_RESOURCE_BACKEND_ORB_VULKAN;
  last.cpu_threads = 1;
  last.gpu_slots = 1;
  last.batch_size = 2;
  last.inflight_limit = 1;
  last.io_slots = 1;
  last.memory_bytes = 2 * 10 * 1024 * 1024;
  last.gpu_memory_bytes = 640 * 1024;
  MatcherExperimentValidation validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (!validated.valid || validated.local_ineligible_fallback_items != 0)
    return false;

  /* The second controlled cohort differs only in admitted inflight/payload;
   * selected backend, CPU/GPU/batch/helpers, and scientific output stay fixed. */
  runtime.matcher_inflight_override = 2;
  last.inflight_limit = 2;
  last.gpu_memory_bytes = 2 * 640 * 1024;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (!validated.valid) return false;

  runtime.matcher_batch_override = 4;
  last.batch_size = 4;
  last.memory_bytes = 4 * 10 * 1024 * 1024;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (!validated.valid) return false;
  last.batch_size = 2;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (validated.valid ||
      std::string(validated.reason) != "forced-contract-mismatch")
    return false;
  runtime.matcher_batch_override = 0;
  last.batch_size = 2;
  last.memory_bytes = 2 * 10 * 1024 * 1024;

  aggregate.actual_backend_sequences[
      LARDON3D_RESOURCE_BACKEND_ORB_VULKAN] = 1;
  aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_MIXED] = 1;
  aggregate.backend_fallback_sequences = 1;
  aggregate.backend_ineligible_fallback_sequences = 1;
  aggregate.local_ineligible_fallback_items = 3;
  aggregate.durable_items = 4;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (!validated.valid || validated.local_ineligible_fallback_items != 3)
    return false;
  aggregate.local_ineligible_fallback_items = 0;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (validated.valid ||
      std::string(validated.reason) !=
          "fallback-item-classification-mismatch")
    return false;

  aggregate.backend_ineligible_fallback_sequences = 0;
  aggregate.backend_failure_fallback_sequences = 1;
  aggregate.local_ineligible_fallback_items = 0;
  aggregate.backend_failure_fallback_items = 1;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (validated.valid || std::string(validated.reason) != "backend-failure")
    return false;

  aggregate.backend_failure_fallback_sequences = 0;
  aggregate.backend_failure_fallback_items = 0;
  aggregate.backend_other_fallback_items = 1;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  if (validated.valid ||
      std::string(validated.reason) != "unclassified-backend-fallback")
    return false;

  aggregate = {};
  aggregate.admission_count = 1;
  aggregate.sequence_count = 1;
  aggregate.selected_backend_admissions[LARDON3D_RESOURCE_BACKEND_CPU] = 1;
  aggregate.actual_backend_sequences[LARDON3D_RESOURCE_BACKEND_CPU] = 1;
  validated = validate_forced_matcher_experiment(
      runtime, true, aggregate, true, last, true, 0, 0, false);
  return !validated.valid &&
         std::string(validated.reason) ==
             "selected-contract-not-exclusively-vulkan";
}

int main() {
  CHECK(unsetenv("MESA_SHADER_CACHE_DISABLE") == 0);
  CHECK(lardon3d_resource_governor_internal_configure_driver_policy() ==
        LARDON3D_RESOURCE_DRIVER_POLICY_DEFAULTED);
  CHECK(std::string(std::getenv("MESA_SHADER_CACHE_DISABLE")) == "true");
  CHECK(lardon3d_resource_governor_internal_configure_driver_policy() ==
        LARDON3D_RESOURCE_DRIVER_POLICY_INHERITED_SAFE);
  CHECK(setenv("MESA_SHADER_CACHE_DISABLE", "false", 1) == 0);
  CHECK(lardon3d_resource_governor_internal_configure_driver_policy() ==
        LARDON3D_RESOURCE_DRIVER_POLICY_REJECTED_UNSAFE);
  CHECK(std::string(std::getenv("MESA_SHADER_CACHE_DISABLE")) == "false");
  CHECK(setenv("MESA_SHADER_CACHE_DISABLE", "true", 1) == 0);

  Options options;
  CHECK(parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                    "/tmp/not-opened"}, options));
  CHECK(options.matcher_mode == MatcherMode::kAuto &&
        options.matcher_pipeline == MatcherPipeline::kRolling &&
        !options.has_matcher_mode && !options.has_matcher_pipeline &&
        !options.has_matcher_inflight_override &&
        options.matcher_inflight_override == 0 &&
        !options.has_matcher_batch_override &&
        options.matcher_batch_override == 0);

  options = {};
  CHECK(parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                    "/tmp/not-opened", "--matcher-mode", "auto",
                    "--matcher-pipeline", "synchronous"}, options));
  CHECK(options.matcher_mode == MatcherMode::kAuto &&
        options.matcher_pipeline == MatcherPipeline::kSynchronous &&
        options.has_matcher_mode && options.has_matcher_pipeline);

  options = {};
  CHECK(parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                    "/tmp/not-opened", "--matcher-mode", "vulkan",
                    "--matcher-pipeline", "rolling", "--gpu-budget", "1"},
                   options));
  CHECK(options.matcher_mode == MatcherMode::kVulkan &&
        options.matcher_pipeline == MatcherPipeline::kRolling);

  options = {};
  CHECK(parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                    "/tmp/not-opened", "--matcher-mode", "auto",
                    "--matcher-pipeline", "rolling", "--matcher-inflight",
                    "1"}, options));
  CHECK(options.matcher_inflight_override == 1 &&
        options.has_matcher_inflight_override);
  options = {};
  CHECK(parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                    "/tmp/not-opened", "--matcher-inflight", "2"},
                   options));
  CHECK(options.matcher_inflight_override == 2 &&
        options.has_matcher_inflight_override);
  for (const unsigned batch : {2U, 4U, 8U, 12U}) {
    options = {};
    const std::string batch_text = std::to_string(batch);
    CHECK(parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                      "/tmp/not-opened", "--matcher-inflight", "1",
                      "--matcher-batch", batch_text.c_str()}, options));
    CHECK(options.matcher_batch_override == batch &&
          options.has_matcher_batch_override);
  }

  options = {};
  CHECK(!parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-mode", "invalid"}, options));
  options = {};
  CHECK(!parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-pipeline", "invalid"},
                    options));
  for (const char *invalid_inflight : {"0", "3", "01", "+1", "2x"}) {
    options = {};
    CHECK(!parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                       "/tmp/not-opened", "--matcher-inflight",
                       invalid_inflight}, options));
  }
  for (const char *invalid_batch : {"1", "3", "6", "10", "16", "02",
                                    "+2", "4x"}) {
    options = {};
    CHECK(!parse_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                       "/tmp/not-opened", "--matcher-inflight", "1",
                       "--matcher-batch", invalid_batch}, options));
  }
  options = {};
  CHECK(!parse_case({"runner", "--resume-candidate-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-mode", "auto"}, options));
  options = {};
  CHECK(!parse_case({"runner", "--resume-candidate-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-inflight", "1"},
                    options));
  options = {};
  CHECK(!parse_case({"runner", "--resume-candidate-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-inflight", "1",
                     "--matcher-batch", "2"}, options));
  options = {};
  CHECK(!parse_case({"runner", "--resume-geometry-existing", "--project-dir",
                     "/tmp/not-opened", "--cpu-budget", "2"}, options));
  options = {};
  CHECK(!parse_case({"runner", "--mode", "s21", "--project-dir",
                     "/tmp/not-opened", "--root", "/tmp", "--matcher-mode",
                     "auto"}, options));

  /* These contradictions are rejected by main before project inspection or a
   * durable Task allocation, which is the CLI contract the harness relies on. */
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-mode", "cpu",
                     "--matcher-pipeline", "synchronous"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-mode", "vulkan",
                     "--gpu-budget", "0"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-inflight", "2",
                     "--gpu-budget", "0"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-mode", "cpu",
                     "--matcher-inflight", "1"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-mode", "vulkan",
                     "--matcher-inflight", "1"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-pipeline", "synchronous",
                     "--matcher-inflight", "2"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-batch", "2"}) == 2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-pipeline", "synchronous",
                     "--matcher-inflight", "1", "--matcher-batch", "2"}) ==
        2);
  CHECK(invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-inflight", "1",
                     "--matcher-batch", "4", "--gpu-budget", "0"}) == 2);

  /* The runner owns all three process controls only for one invocation. Even a
   * project-validation exit must restore exact inherited values, while absent
   * controls remain absent after the same path. */
  CHECK(setenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV,
               "inherited-pipeline", 1) == 0 &&
        setenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV,
               "inherited-inflight", 1) == 0 &&
        setenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV,
               "inherited-batch", 1) == 0 &&
        invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-inflight", "2",
                     "--matcher-batch", "8"}) == 2 &&
        std::string(std::getenv(
            LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV)) ==
            "inherited-pipeline" &&
        std::string(std::getenv(
            LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV)) ==
            "inherited-inflight" &&
        std::string(std::getenv(
            LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV)) ==
            "inherited-batch" &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV) == 0 &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == 0 &&
        unsetenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == 0 &&
        invoke_case({"runner", "--resume-pre-gv-existing", "--project-dir",
                     "/tmp/not-opened", "--matcher-inflight", "1",
                     "--matcher-batch", "4"}) == 2 &&
        std::getenv(LARDON3D_MATCHER_TASK_BENCHMARK_SYNCHRONOUS_ENV) ==
            nullptr &&
        std::getenv(LARDON3D_MATCHER_TASK_BENCHMARK_INFLIGHT_ENV) == nullptr &&
        std::getenv(LARDON3D_MATCHER_TASK_BENCHMARK_BATCH_ENV) == nullptr);
  CHECK(matcher_evidence_aggregate_case());
  CHECK(forced_matcher_experiment_validation_case());
  return EXIT_SUCCESS;
}
