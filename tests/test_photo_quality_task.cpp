#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>

#include <sqlite3.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

extern "C" {
#include <lardon3d/photo_quality_task.h>
#include <lardon3d/project.h>
#include <lardon3d/task_queue.h>
}

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                          \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                   #condition);                                                  \
      return 1;                                                                  \
    }                                                                            \
  } while (0)

extern "C" const Lardon3DTaskKindRegistry *lardon3d_task_kind_registry_production(void) {
  return nullptr;
}

static bool wait_terminal(Lardon3DTaskQueue *queue, uint64_t task_id,
                          Lardon3DTaskObservation *snapshot) {
  for (size_t attempt = 0; attempt < 500; ++attempt) {
    if (lardon3d_task_queue_get_observation(queue, task_id, snapshot) &&
        (snapshot->state == TASK_COMPLETED || snapshot->state == TASK_FAILED ||
         snapshot->state == TASK_CANCELLED))
      return true;
    usleep(10000);
  }
  return false;
}

static void source(Lardon3DAcquisitionCampaignSource *value, const std::string &path,
                   Lardon3DAcquisitionSourceKind kind) {
  std::snprintf(value->path, sizeof(value->path), "%s", path.c_str());
  value->source_kind = value->metadata.source_kind = kind;
  value->metadata_result = LARDON3D_ACQUISITION_OK;
  value->metadata.policy_version = LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
}

int main() {
  char directory_template[] = "/tmp/lardon3d-photo-quality-task-XXXXXX";
  char *directory = mkdtemp(directory_template);
  CHECK(directory != nullptr);
  const std::string root(directory);
  const std::string database_path = root + "/project.lardon3d";
  const std::string jpeg_path = root + "/1-pair.jpg";
  CHECK(std::filesystem::create_directories(root + "/.lardon3d/checkpoints"));
  cv::Mat image(128, 128, CV_8UC1);
  for (int y = 0; y < image.rows; ++y)
    for (int x = 0; x < image.cols; ++x)
      image.at<unsigned char>(y, x) = ((x / 8) + (y / 8)) % 2 ? 220 : 30;
  CHECK(cv::imwrite(jpeg_path, image));

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  Lardon3DProjectDb *database = nullptr;
  CHECK(lardon3d_project_db_open(database_path.c_str(), &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet scanset{};
  CHECK(lardon3d_project_db_create_scanset(database, "quality", &scanset) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DAppState state{};
  lardon3d_app_state_init(&state);
  state.project_loaded = true;
  state.project_db = database;
  std::snprintf(state.project_path, sizeof(state.project_path), "%s", root.c_str());
  state.hardware_profile.logical_cpu_count = 8;
  state.hardware_profile.page_size_bytes = 4096;
  state.hardware_profile.memory_total_bytes = UINT64_C(8) << 30;
  std::snprintf(state.hardware_profile.cpu_architecture,
                sizeof(state.hardware_profile.cpu_architecture), "test");
  Lardon3DResourcePolicy policy{};
  policy.maximum_cpu_load_ratio = 1.0;
  policy.maximum_io_pressure_avg10 = 100.0;
  policy.io_slot_capacity = 2;
  state.resource_governor =
      lardon3d_resource_governor_create(&state.hardware_profile, &policy);
  state.task_queue = state.resource_governor
                         ? lardon3d_task_queue_create(state.resource_governor, 4)
                         : nullptr;
  CHECK(state.task_queue != nullptr);

  Lardon3DAcquisitionCampaignSource sources[3]{};
  source(&sources[0], root + "/0-pair.arw", LARDON3D_ACQUISITION_SOURCE_RAW);
  source(&sources[1], jpeg_path, LARDON3D_ACQUISITION_SOURCE_JPEG);
  source(&sources[2], root + "/2-only.arw", LARDON3D_ACQUISITION_SOURCE_RAW);
  Lardon3DAcquisitionCampaignConfirmation pair{};
  pair.source_count = 2;
  pair.source_indices[0] = 0;
  pair.source_indices[1] = 1;
  Lardon3DPhotoQualityTaskRequest request{sources, 3, &pair, 1};
  Lardon3DAcquisitionCampaignPlan planned{};
  CHECK(lardon3d_acquisition_campaign_plan(sources, 3, &pair, 1, &planned) ==
            LARDON3D_ACQUISITION_CAMPAIGN_OK &&
        planned.group_count == 2);
  size_t request_probe_size = 0;
  CHECK(lardon3d_photo_quality_request_encode(&request, nullptr, 0, &request_probe_size));

  cv::setNumThreads(3);
  uint64_t task_id = 0;
  CHECK(lardon3d_project_enqueue_photo_quality(&state, scanset.scanset_id, &request, &task_id));
  Lardon3DTaskObservation terminal{};
  CHECK(wait_terminal(state.task_queue, task_id, &terminal));
  CHECK(terminal.state == TASK_COMPLETED && terminal.progress == 100 &&
        terminal.durable_progress_known && terminal.durable_completed == 2 &&
        terminal.durable_total == 2);
  CHECK(cv::getNumThreads() == 3);
  Lardon3DProjectDbPhotoQualityResult paired{}, raw_only{};
  CHECK(lardon3d_project_db_load_photo_quality_result(database, task_id, 1, &paired) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(paired.proxy_source_index == 1 &&
        paired.metrics.status == LARDON3D_PHOTO_QUALITY_METRIC_OK);
  CHECK(paired.group_id == planned.groups[0].group_id);
  CHECK(lardon3d_project_db_load_photo_quality_result(database, task_id, 2, &raw_only) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(raw_only.proxy_source_index == UINT32_MAX &&
        raw_only.metrics.status == LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE &&
        std::strstr(raw_only.metrics.reasons, "REQUIRES_JPEG_PROXY") != nullptr);
  Lardon3DProjectDbTask completed_generic{};
  CHECK(lardon3d_project_db_load_task(database, task_id, &completed_generic) ==
            LARDON3D_PROJECT_DB_OK &&
        completed_generic.sequence_count == 1);
  std::vector<unsigned char> corrupt_request(LARDON3D_PHOTO_QUALITY_TASK_REQUEST_MAX_BYTES);
  Lardon3DProjectDbPhotoQualityTask completed_parameters{};
  CHECK(lardon3d_project_db_load_photo_quality_task(database, task_id, corrupt_request.data(),
            corrupt_request.size(), &completed_parameters) == LARDON3D_PROJECT_DB_OK);
  CHECK(completed_parameters.group_count == 2 && completed_parameters.next_group_id == 3);
  CHECK(lardon3d_resource_governor_reservation_count(state.resource_governor) == 0);
  Lardon3DTaskDurableSnapshot mismatched_snapshot{};
  mismatched_snapshot.id = task_id;
  std::snprintf(mismatched_snapshot.name, sizeof(mismatched_snapshot.name),
                "quality kind mismatch");
  mismatched_snapshot.saved_state = mismatched_snapshot.recovery_state = TASK_PENDING;
  CHECK(lardon3d_project_db_record_photo_quality_task(
            database, &mismatched_snapshot,
            LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
            LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, nullptr,
            &completed_parameters, 2) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  /* Corrupt values above the v21 operational group bound must be rejected
   * while still 64-bit SQLite integers, before narrowing into public fields. */
  sqlite3 *corruptor = nullptr;
  CHECK(sqlite3_open(database_path.c_str(), &corruptor) == SQLITE_OK);
  CHECK(sqlite3_exec(corruptor,
                     "PRAGMA foreign_keys=OFF;PRAGMA ignore_check_constraints=ON",
                     nullptr, nullptr, nullptr) == SQLITE_OK);
  const auto execute_corruption = [&](const std::string &assignment) {
    const std::string sql = "UPDATE photo_quality_triage_tasks SET " + assignment +
                            " WHERE task_id=" + std::to_string(task_id);
    return sqlite3_exec(corruptor, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
  };
  std::vector<unsigned char> corrupt_output(completed_parameters.request_size);
  Lardon3DProjectDbPhotoQualityTask corrupt_parameters{};
  const auto expect_task_failure = [&](uint64_t corrupt_task_id,
                                       Lardon3DProjectDbResult expected) {
    std::memset(corrupt_output.data(), 0xa5, corrupt_output.size());
    std::memset(&corrupt_parameters, 0xa5, sizeof(corrupt_parameters));
    if (lardon3d_project_db_load_photo_quality_task(
            database, corrupt_task_id, corrupt_output.data(),
            corrupt_output.size(), &corrupt_parameters) != expected)
      return false;
    if (corrupt_parameters.task_id != 0 ||
        corrupt_parameters.scanset_id != 0 ||
        corrupt_parameters.next_group_id != 0 ||
        corrupt_parameters.group_count != 0 || corrupt_parameters.request ||
        corrupt_parameters.request_size != 0)
      return false;
    for (unsigned char byte : corrupt_output)
      if (byte != 0xa5)
        return false;
    return true;
  };
  /* No typed business row is ordinary absence; only a present row with broken
   * parent relations is durable corruption. Both failure classes hide outputs. */
  CHECK(expect_task_failure(INT64_MAX, LARDON3D_PROJECT_DB_NOT_FOUND));
  for (const char *assignment : {"group_count=-1", "group_count=5000", "group_count=4294967296",
                                 "next_group_id=0", "next_group_id=4", "next_group_id=1e20",
                                 "scanset_id=-1", "scanset_id=1e20",
                                 "scanset_id=9223372036854775807"}) {
    CHECK(execute_corruption(assignment));
    CHECK(expect_task_failure(task_id, LARDON3D_PROJECT_DB_CORRUPT));
    CHECK(execute_corruption("scanset_id=" + std::to_string(scanset.scanset_id) +
                             ",group_count=2,next_group_id=3"));
  }
  CHECK(execute_corruption("request=CAST(request AS TEXT)"));
  CHECK(expect_task_failure(task_id, LARDON3D_PROJECT_DB_CORRUPT));
  CHECK(execute_corruption("request=CAST(request AS BLOB)"));

  const auto execute_generic_corruption = [&](const std::string &assignment) {
    const std::string sql = "UPDATE tasks SET " + assignment +
                            " WHERE task_id=" + std::to_string(task_id);
    return sqlite3_exec(corruptor, sql.c_str(), nullptr, nullptr, nullptr) ==
           SQLITE_OK;
  };
  CHECK(execute_generic_corruption("task_kind=task_kind||char(0)||'tail'"));
  CHECK(expect_task_failure(task_id, LARDON3D_PROJECT_DB_CORRUPT));
  CHECK(execute_generic_corruption("task_kind='photo_quality.triage'"));
  CHECK(execute_generic_corruption("task_kind_version=2"));
  CHECK(expect_task_failure(task_id, LARDON3D_PROJECT_DB_CORRUPT));
  CHECK(execute_generic_corruption("task_kind_version=1"));
  CHECK(execute_generic_corruption("task_kind_version=1e20"));
  CHECK(expect_task_failure(task_id, LARDON3D_PROJECT_DB_CORRUPT));
  CHECK(execute_generic_corruption("task_kind_version=1"));

  /* The typed business row determines presence. Broken generic ownership must
   * be reported as corruption instead of disappearing behind an inner join. */
  uint64_t orphan_task_id = 0;
  CHECK(lardon3d_project_db_allocate_task_id(database, &orphan_task_id) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DTaskDurableSnapshot orphan_snapshot{};
  orphan_snapshot.id = orphan_task_id;
  std::snprintf(orphan_snapshot.name, sizeof(orphan_snapshot.name),
                "quality orphan fixture");
  orphan_snapshot.saved_state = orphan_snapshot.recovery_state = TASK_PENDING;
  Lardon3DProjectDbPhotoQualityTask orphan_parameters{
      orphan_task_id, scanset.scanset_id, 1, 2, corrupt_request.data(),
      completed_parameters.request_size};
  CHECK(lardon3d_project_db_record_photo_quality_task(
            database, &orphan_snapshot, LARDON3D_PHOTO_QUALITY_TASK_KIND,
            LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION, nullptr,
            &orphan_parameters, 1) == LARDON3D_PROJECT_DB_OK);
  CHECK(sqlite3_exec(
            corruptor,
            ("DELETE FROM tasks WHERE task_id=" +
             std::to_string(orphan_task_id)).c_str(),
            nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(expect_task_failure(orphan_task_id, LARDON3D_PROJECT_DB_CORRUPT));

  const auto execute_result_corruption = [&](const std::string &assignment) {
    const std::string sql = "UPDATE photo_quality_triage_results SET " + assignment +
                            " WHERE task_id=" + std::to_string(task_id) + " AND group_id=1";
    return sqlite3_exec(corruptor, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
  };
  for (const auto &values : std::vector<std::pair<std::string, std::string>>{
           {"proxy_source_index=-1", "proxy_source_index=1"},
           {"proxy_source_index=4294967296", "proxy_source_index=1"},
           {"proxy_source_index=1e20", "proxy_source_index=1"},
           {"metrics_version=-1", "metrics_version=1"},
           {"metric_status=-1", "metric_status=0"},
           {"recommendation=4", "recommendation=1"},
           {"human_override=3", "human_override=0"},
           {"decoded_width=-1", "decoded_width=128"},
           {"analysis_width=4294967296", "analysis_width=128"},
           {"analysis_height=0", "analysis_height=128"},
           {"sharpness_normalized=-1.0",
            "sharpness_normalized=" + std::to_string(paired.metrics.sharpness_normalized)},
           {"clipped_white_fraction=2.0",
            "clipped_white_fraction=" + std::to_string(paired.metrics.clipped_white_fraction)}}) {
    CHECK(execute_result_corruption(values.first));
    Lardon3DProjectDbPhotoQualityResult corrupt_result{};
    CHECK(lardon3d_project_db_load_photo_quality_result(database, task_id, 1, &corrupt_result) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    CHECK(execute_result_corruption(values.second));
  }
  /* Result identity must belong to the typed plan and precede its durable
   * cursor; otherwise publication would expose uncompleted plan state. */
  CHECK(execute_result_corruption("group_id=3"));
  Lardon3DProjectDbPhotoQualityResult corrupt_plan_result{};
  CHECK(lardon3d_project_db_load_photo_quality_result(database, task_id, 3,
            &corrupt_plan_result) == LARDON3D_PROJECT_DB_CORRUPT);
  CHECK(sqlite3_exec(corruptor,
      ("UPDATE photo_quality_triage_results SET group_id=1 WHERE task_id=" +
       std::to_string(task_id) + " AND group_id=3").c_str(), nullptr, nullptr, nullptr) ==
        SQLITE_OK);
  CHECK(execute_corruption("next_group_id=1"));
  CHECK(lardon3d_project_db_load_photo_quality_result(database, task_id, 1,
            &corrupt_plan_result) == LARDON3D_PROJECT_DB_CORRUPT);
  CHECK(execute_corruption("next_group_id=3"));
  sqlite3_close(corruptor);

  uint64_t cancelled_id = 0;
  Lardon3DTask *cancelled = lardon3d_project_create_photo_quality_task(
      &state, scanset.scanset_id, &request, &cancelled_id);
  CHECK(cancelled != nullptr);
  Lardon3DResourceEstimate quality_estimate{};
  CHECK(lardon3d_task_resource_estimate(cancelled, &quality_estimate));
  /* Admission includes the retained durable context in addition to the
   * documented 20 MiB one-group analyzer working set. */
  CHECK(quality_estimate.memory_fixed_bytes > UINT64_C(20) * 1024u * 1024u);
  CHECK(quality_estimate.memory_bytes_per_item == 0);
  lardon3d_task_request_cancel(cancelled);
  CHECK(lardon3d_task_queue_add(state.task_queue, cancelled, nullptr));
  CHECK(wait_terminal(state.task_queue, cancelled_id, &terminal));
  CHECK(terminal.state == TASK_CANCELLED);
  Lardon3DProjectDbPhotoQualityResult absent{};
  CHECK(raw_only.group_id == planned.groups[1].group_id);
  CHECK(lardon3d_project_db_load_photo_quality_result(database, cancelled_id, 1, &absent) ==
        LARDON3D_PROJECT_DB_NOT_FOUND);

  /* Seed the documented crash shape: plan group 1 and next group ID 2 are durable while
   * generic progress still lags. Registry recovery must begin at plan group 2 and
   * must neither re-analyze nor replace the already published pair result. */
  size_t encoded_size = 0;
  CHECK(lardon3d_photo_quality_request_encode(&request, nullptr, 0, &encoded_size));
  std::vector<unsigned char> encoded(encoded_size);
  CHECK(lardon3d_photo_quality_request_encode(&request, encoded.data(), encoded.size(),
                                               &encoded_size));
  uint64_t restart_id = 0;
  CHECK(lardon3d_project_db_allocate_task_id(database, &restart_id) == LARDON3D_PROJECT_DB_OK);
  Lardon3DTaskDurableSnapshot durable{};
  durable.id = restart_id;
  std::snprintf(durable.name, sizeof(durable.name), "quality restart");
  durable.saved_state = durable.recovery_state = TASK_PENDING;
  Lardon3DProjectDbPhotoQualityTask parameters{restart_id, scanset.scanset_id, 1, 2,
                                                encoded.data(), encoded.size()};
  CHECK(lardon3d_project_db_record_photo_quality_task(
            database, &durable, LARDON3D_PHOTO_QUALITY_TASK_KIND,
            LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION, nullptr, &parameters, 1) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbPhotoQualityResult sentinel = paired;
  sentinel.task_id = restart_id;
  sentinel.metrics.sharpness_raw = 12345.0;
  CHECK(lardon3d_project_db_record_photo_quality_result(database, &sentinel, 2) ==
        LARDON3D_PROJECT_DB_OK);
  /* Exact replay after the atomic cursor advance converges without replacing
   * identity or creating another result row. */
  CHECK(lardon3d_project_db_record_photo_quality_result(database, &sentinel, 2) ==
        LARDON3D_PROJECT_DB_OK);
  sqlite3 *replay_reader = nullptr;
  CHECK(sqlite3_open(database_path.c_str(), &replay_reader) == SQLITE_OK);
  sqlite3_stmt *count_statement = nullptr;
  CHECK(sqlite3_prepare_v2(replay_reader,
      "SELECT COUNT(*) FROM photo_quality_triage_results WHERE task_id=?1 AND group_id=1",
      -1, &count_statement, nullptr) == SQLITE_OK);
  CHECK(sqlite3_bind_int64(count_statement, 1, (sqlite3_int64)restart_id) == SQLITE_OK);
  CHECK(sqlite3_step(count_statement) == SQLITE_ROW &&
        sqlite3_column_int64(count_statement, 0) == 1);
  sqlite3_finalize(count_statement);
  Lardon3DProjectDbPhotoQualityResult conflicting = sentinel;
  conflicting.metrics.sharpness_raw += 1.0;
  CHECK(lardon3d_project_db_record_photo_quality_result(database, &conflicting, 2) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(sqlite3_exec(replay_reader,
      ("UPDATE photo_quality_triage_tasks SET next_group_id=3 WHERE task_id=" +
       std::to_string(restart_id)).c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(lardon3d_project_db_record_photo_quality_result(database, &sentinel, 2) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(sqlite3_exec(replay_reader,
      ("UPDATE photo_quality_triage_tasks SET next_group_id=2 WHERE task_id=" +
       std::to_string(restart_id)).c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(replay_reader);
  Lardon3DProjectDbPhotoQualityResult replay_retained{};
  CHECK(lardon3d_project_db_load_photo_quality_result(database, restart_id, 1,
            &replay_retained) == LARDON3D_PROJECT_DB_OK &&
        replay_retained.metrics.sharpness_raw == sentinel.metrics.sharpness_raw);
  Lardon3DProjectDbTask generic{};
  CHECK(lardon3d_project_db_load_task(database, restart_id, &generic) == LARDON3D_PROJECT_DB_OK);
  Lardon3DTaskDurableSnapshot restart_snapshot{};
  restart_snapshot.id = generic.task_id;
  std::memcpy(restart_snapshot.name, generic.name, sizeof(restart_snapshot.name));
  restart_snapshot.progress = generic.progress;
  restart_snapshot.saved_state = generic.saved_state;
  restart_snapshot.recovery_state = generic.recovery_state;
  restart_snapshot.started_at = generic.started_at;
  restart_snapshot.finished_at = generic.finished_at;
  restart_snapshot.sequence_count = generic.sequence_count;
  restart_snapshot.estimate = quality_estimate;
  Lardon3DTaskReconstructionContext reconstruction{root.c_str(), database,
                                                    state.resource_governor, nullptr};
  const Lardon3DTaskKindDescriptor quality_descriptor = {
      LARDON3D_PHOTO_QUALITY_TASK_KIND, LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION,
      lardon3d_photo_quality_task_reconstruct};
  Lardon3DTaskKindRegistry quality_registry{};
  CHECK(lardon3d_task_kind_registry_init(&quality_registry, &quality_descriptor, 1));
  const Lardon3DTaskKindRegistry *registry = &quality_registry;
  const Lardon3DTaskKindDescriptor *descriptor = nullptr;
  CHECK(lardon3d_task_kind_registry_lookup(registry, LARDON3D_PHOTO_QUALITY_TASK_KIND,
                                           LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION,
                                           &descriptor) == LARDON3D_TASK_KIND_OK);
  Lardon3DTask *restored = nullptr;
  CHECK(lardon3d_task_kind_registry_restore(registry, LARDON3D_PHOTO_QUALITY_TASK_KIND,
                                            LARDON3D_PHOTO_QUALITY_TASK_KIND_VERSION,
                                            &restart_snapshot, &reconstruction, &restored) ==
            LARDON3D_TASK_KIND_OK &&
        restored != nullptr);
  CHECK(lardon3d_task_queue_add(state.task_queue, restored, nullptr));
  CHECK(wait_terminal(state.task_queue, restart_id, &terminal));
  CHECK(terminal.state == TASK_COMPLETED && terminal.durable_progress_known &&
        terminal.durable_completed == 2 && terminal.durable_total == 2);
  Lardon3DProjectDbPhotoQualityResult retained{}, resumed_raw{};
  CHECK(lardon3d_project_db_load_photo_quality_result(database, restart_id, 1, &retained) ==
        LARDON3D_PROJECT_DB_OK && retained.metrics.sharpness_raw == 12345.0);
  CHECK(lardon3d_project_db_load_photo_quality_result(database, restart_id, 2, &resumed_raw) ==
        LARDON3D_PROJECT_DB_OK &&
        resumed_raw.metrics.status == LARDON3D_PHOTO_QUALITY_METRIC_UNAVAILABLE);

  lardon3d_task_queue_destroy(state.task_queue);
  lardon3d_resource_governor_destroy(state.resource_governor);
  lardon3d_project_db_close(database);
  std::filesystem::remove_all(root);
  return 0;
}
