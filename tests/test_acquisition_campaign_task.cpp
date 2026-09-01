#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <sqlite3.h>
#include <opencv2/core.hpp>

extern "C" {
#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/project_db.h>
#include <lardon3d/task_queue.h>
#include "../src/task_internal.h"
}

extern "C" bool
lardon3d_acquisition_campaign_task_internal_configure_restored(
    Lardon3DTask *, void *);
extern "C" bool lardon3d_acquisition_campaign_task_test_configure_execution(
    void *, const uint64_t *, size_t, uint32_t, unsigned int);
extern "C" bool lardon3d_acquisition_campaign_task_test_observed_threads(
    void *, unsigned int *, size_t, size_t *);

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "failure line %d: %s\n", __LINE__, #x);             \
      return 1;                                                                \
    }                                                                          \
  } while (0)

extern "C" const Lardon3DTaskKindRegistry *
lardon3d_task_kind_registry_production(void) {
  return nullptr;
}

static bool completed_task_callback(Lardon3DTask *, void *) { return true; }

static bool query_int64(sqlite3 *database, const std::string &sql,
                        sqlite3_int64 *value) {
  sqlite3_stmt *statement = nullptr;
  if (!database || !value ||
      sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) !=
          SQLITE_OK)
    return false;
  const int code = sqlite3_step(statement);
  const bool valid = code == SQLITE_ROW &&
                     sqlite3_column_type(statement, 0) == SQLITE_INTEGER;
  if (valid)
    *value = sqlite3_column_int64(statement, 0);
  return sqlite3_finalize(statement) == SQLITE_OK && valid;
}

static bool query_text(sqlite3 *database, const std::string &sql,
                       std::string *value) {
  sqlite3_stmt *statement = nullptr;
  if (!database || !value ||
      sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) !=
          SQLITE_OK)
    return false;
  const int code = sqlite3_step(statement);
  const unsigned char *text = code == SQLITE_ROW
                                  ? sqlite3_column_text(statement, 0)
                                  : nullptr;
  const int bytes = code == SQLITE_ROW ? sqlite3_column_bytes(statement, 0) : 0;
  const bool valid = code == SQLITE_ROW && text && bytes >= 0;
  if (valid)
    value->assign(reinterpret_cast<const char *>(text),
                  static_cast<size_t>(bytes));
  return sqlite3_finalize(statement) == SQLITE_OK && valid;
}

int main() {
  Lardon3DAcquisitionCampaignSource sources[2]{};
  std::snprintf(sources[0].path, sizeof(sources[0].path), "/input/a.arw");
  std::snprintf(sources[1].path, sizeof(sources[1].path), "/input/a.jpg");
  sources[0].source_kind = sources[0].metadata.source_kind =
      LARDON3D_ACQUISITION_SOURCE_RAW;
  sources[1].source_kind = sources[1].metadata.source_kind =
      LARDON3D_ACQUISITION_SOURCE_JPEG;
  for (auto &source : sources) {
    source.metadata_result = LARDON3D_ACQUISITION_OK;
    source.metadata.policy_version =
        LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
  }
  Lardon3DAcquisitionCampaignConfirmation confirmation{};
  confirmation.source_count = 2;
  confirmation.source_indices[0] = 0;
  confirmation.source_indices[1] = 1;
  Lardon3DAcquisitionCampaignTaskRequest input{};
  input.sources = sources;
  input.source_count = 2;
  input.confirmations = &confirmation;
  input.confirmation_count = 1;
  input.ingest_options.representation = LARDON3D_ACQUISITION_SELECT_DEVELOP_RAW;
  input.ingest_options.select_representation = 1;
  input.ingest_options.imported_at = 123;
  input.ingest_options.max_source_bytes = 456;
  Lardon3DAcquisitionCampaignPlan input_plan{};
  CHECK(lardon3d_acquisition_campaign_plan(
            sources, 2, &confirmation, 1, &input_plan) ==
            LARDON3D_ACQUISITION_CAMPAIGN_OK &&
        input_plan.group_count == 1);

  size_t size = 0;
  CHECK(
      lardon3d_acquisition_campaign_request_encode(&input, nullptr, 0, &size));
  CHECK(size > 32);
  std::vector<unsigned char> encoded(size);
  CHECK(lardon3d_acquisition_campaign_request_encode(&input, encoded.data(),
                                                     encoded.size(), &size));
  size_t required_size = 777;
  std::vector<unsigned char> insufficient(encoded.size() - 1u);
  CHECK(!lardon3d_acquisition_campaign_request_encode(
            &input, insufficient.data(), insufficient.size(), &required_size) &&
        required_size == encoded.size());
  required_size = 777;
  CHECK(!lardon3d_acquisition_campaign_request_encode(
            &input, nullptr, 1, &required_size) &&
        required_size == encoded.size());
  required_size = 777;
  CHECK(!lardon3d_acquisition_campaign_request_encode(
            nullptr, nullptr, 0, &required_size) &&
        required_size == 0);
  CHECK(!lardon3d_acquisition_campaign_request_encode(
      &input, nullptr, 0, nullptr));
  Lardon3DAcquisitionCampaignSource decoded_sources[2]{};
  Lardon3DAcquisitionCampaignConfirmation decoded_confirmations[2]{};
  Lardon3DAcquisitionCampaignTaskRequest decoded{};
  CHECK(lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size(), decoded_sources, 2, decoded_confirmations,
      2, &decoded));
  CHECK(decoded.source_count == 2 && decoded.confirmation_count == 1);
  CHECK(decoded_confirmations[0].source_count == 2);
  CHECK(decoded.ingest_options.representation ==
        LARDON3D_ACQUISITION_SELECT_DEVELOP_RAW);
  CHECK(decoded.ingest_options.select_representation == 1);
  CHECK(decoded.ingest_options.imported_at == 123 &&
        decoded.ingest_options.max_source_bytes == 456);
  CHECK(std::strcmp(decoded_sources[0].path, "/input/a.arw") == 0);
  CHECK(!lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size(), nullptr, 2, decoded_confirmations, 2,
      &decoded));
  CHECK(!lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size(), decoded_sources, 2, nullptr, 2,
      &decoded));

  constexpr size_t imported_at_offset = 8u + 5u * sizeof(uint32_t);
  for (size_t i = 0; i < sizeof(uint64_t); ++i)
    encoded[imported_at_offset + i] = 0xffu;
  CHECK(!lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size(), decoded_sources, 2, decoded_confirmations,
      2, &decoded));
  for (size_t i = 0; i < sizeof(uint64_t); ++i)
    encoded[imported_at_offset + i] =
        static_cast<unsigned char>(uint64_t(123) >> (8u * i));

  input.ingest_options.imported_at = -1;
  required_size = 777;
  CHECK(!lardon3d_acquisition_campaign_request_encode(
            &input, nullptr, 0, &required_size) &&
        required_size == 0);
  input.ingest_options.imported_at = 123;
  input.ingest_options.max_source_bytes = 0;
  CHECK(!lardon3d_acquisition_campaign_request_encode(&input, nullptr, 0,
                                                       &size));
  input.ingest_options.max_source_bytes = 456;

  encoded[8] = 2;
  CHECK(!lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size(), decoded_sources, 2, decoded_confirmations,
      2, &decoded));
  encoded[8] = 1;
  CHECK(!lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size() - 1, decoded_sources, 2,
      decoded_confirmations, 2, &decoded));
  CHECK(!lardon3d_acquisition_campaign_request_decode(
      encoded.data(), encoded.size(), decoded_sources, 1, decoded_confirmations,
      2, &decoded));

  char database_path[] = "/tmp/lardon3d-campaign-task-XXXXXX";
  int descriptor = mkstemp(database_path);
  CHECK(descriptor >= 0 && close(descriptor) == 0 &&
        unlink(database_path) == 0);
  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  Lardon3DProjectDb *database = nullptr;
  CHECK(lardon3d_project_db_open(database_path, &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet target{}, other{};
  CHECK(lardon3d_project_db_create_scanset(database, "target", &target) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_create_scanset(database, "other", &other) ==
        LARDON3D_PROJECT_DB_OK);
  uint64_t allocated_task_id = 0;
  CHECK(lardon3d_project_db_allocate_task_id(database, &allocated_task_id) ==
            LARDON3D_PROJECT_DB_OK &&
        allocated_task_id == 1);
  Lardon3DTaskDurableSnapshot snapshot{};
  snapshot.id = 1;
  std::snprintf(snapshot.name, sizeof(snapshot.name), "campaign");
  snapshot.saved_state = snapshot.recovery_state = TASK_PENDING;
  Lardon3DProjectDbAcquisitionCampaignTask durable{
      1, target.scanset_id, 0, 1, encoded.data(), encoded.size()};
  CHECK(lardon3d_project_db_record_acquisition_campaign_task(
            database, &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
            LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, nullptr, &durable,
            1) == LARDON3D_PROJECT_DB_OK);
  uint64_t mismatched_task_id = 0;
  CHECK(lardon3d_project_db_allocate_task_id(database, &mismatched_task_id) ==
            LARDON3D_PROJECT_DB_OK &&
        mismatched_task_id == 2);
  Lardon3DTaskDurableSnapshot mismatched_snapshot = snapshot;
  mismatched_snapshot.id = mismatched_task_id;
  Lardon3DProjectDbAcquisitionCampaignTask mismatched_durable = durable;
  mismatched_durable.task_id = mismatched_task_id;
  /* Typed business persistence must never be recorded under another generic
   * dispatch identity, even when every other field is valid. */
  CHECK(lardon3d_project_db_record_acquisition_campaign_task(
            database, &mismatched_snapshot, "photo_quality.triage", 1, nullptr,
            &mismatched_durable, 1) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  std::vector<unsigned char> changed = encoded;
  changed.back() ^= 1u;
  snapshot.progress = 50;
  durable.request = changed.data();
  CHECK(lardon3d_project_db_record_acquisition_campaign_task(
            database, &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
            LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, nullptr, &durable,
            2) == LARDON3D_PROJECT_DB_CONSTRAINT);
  Lardon3DProjectDbTask generic{};
  CHECK(lardon3d_project_db_load_task(database, 1, &generic) ==
            LARDON3D_PROJECT_DB_OK &&
        generic.progress == 0);
  snapshot.progress = 0;
  durable.request = encoded.data();
  Lardon3DProjectDbCapture first{}, wrong{}, second{};
  CHECK(lardon3d_project_db_create_capture(database, target.scanset_id, 1,
                                           &first) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_create_capture(database, other.scanset_id, 1,
                                           &wrong) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_create_capture(database, target.scanset_id, 1,
                                           &second) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 1, wrong.capture_id, 1) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 2, second.capture_id, 2) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 1, first.capture_id, 1) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 1, first.capture_id, 1) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 1, second.capture_id, 1) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  Lardon3DProjectDbAcquisitionCampaignTask measured_task{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, nullptr, 0, &measured_task) == LARDON3D_PROJECT_DB_OK);
  CHECK(measured_task.request == nullptr &&
        measured_task.request_size == encoded.size());
  std::vector<unsigned char> loaded(encoded.size());
  Lardon3DProjectDbAcquisitionCampaignTask loaded_task{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, loaded.data(), loaded.size(), &loaded_task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_task.next_group_id == 1 && loaded_task.group_count == 1 &&
        loaded_task.request_size == encoded.size() &&
        std::memcmp(loaded.data(), encoded.data(), encoded.size()) == 0);
  std::vector<unsigned char> short_request(encoded.size() - 1u);
  Lardon3DProjectDbAcquisitionCampaignTask short_task{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, short_request.data(), short_request.size(),
            &short_task) == LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(short_task.task_id == 0 && short_task.request == nullptr &&
        short_task.request_size == 0);
  Lardon3DProjectDbAcquisitionCampaignCapture loaded_capture{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 1, &loaded_capture) == LARDON3D_PROJECT_DB_OK &&
        loaded_capture.capture_id == first.capture_id);
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 2, &loaded_capture) ==
        LARDON3D_PROJECT_DB_CONSTRAINT);
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 0, &loaded_capture) ==
        LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  Lardon3DHardwareProfile profile{16, 4096, UINT64_MAX, false, 0, false,
                                  false, 0, "test", ""};
  Lardon3DResourcePolicy policy{};
  policy.maximum_cpu_load_ratio = 1.0;
  policy.maximum_io_pressure_avg10 = 100.0;
  policy.io_slot_capacity = 1;
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);

  sqlite3 *corruptor = nullptr;
  CHECK(sqlite3_open(database_path, &corruptor) == SQLITE_OK);
  CHECK(sqlite3_exec(corruptor, "PRAGMA ignore_check_constraints=ON", nullptr,
                     nullptr, nullptr) == SQLITE_OK);
  const auto execute_sql = [&](const std::string &sql) {
    return sqlite3_exec(corruptor, sql.c_str(), nullptr, nullptr, nullptr) ==
           SQLITE_OK;
  };
  const auto campaign_sql = [&](const std::string &assignment) {
    const std::string sql = "UPDATE acquisition_campaign_tasks SET " +
                            assignment + " WHERE task_id=1";
    return execute_sql(sql);
  };
  const auto generic_sql = [&](const std::string &assignment) {
    return execute_sql("UPDATE tasks SET " + assignment + " WHERE task_id=1");
  };
  const std::string valid_campaign_empty =
      "scanset_id=" + std::to_string(target.scanset_id) +
      ",next_group_id=0,group_count=1,request=CAST(request AS BLOB)";
  const auto restore_empty_prefix = [&]() {
    return execute_sql(
               "DELETE FROM acquisition_campaign_captures WHERE task_id=1") &&
           campaign_sql(valid_campaign_empty) &&
           generic_sql("task_kind='acquisition_campaign.run',"
                       "task_kind_version=1");
  };
  const auto restore_valid_prefix = [&]() {
    return restore_empty_prefix() &&
           execute_sql(
               "INSERT INTO acquisition_campaign_captures"
               "(task_id,group_id,capture_id) VALUES(1,1," +
               std::to_string(first.capture_id) + ")") &&
           campaign_sql("next_group_id=1");
  };

  Lardon3DTaskReconstructionContext count_reconstruction{
      "/tmp", database, governor, nullptr};
  Lardon3DTaskKindBinding count_binding{};
  Lardon3DProjectDbAcquisitionCampaignTask corrupt_task{};

  /* SQLite dynamic types and signed-wide values must be rejected before the
   * retention transaction inserts a mapping or advances the cursor. */
  const std::vector<std::string> campaign_corruptions{
      "scanset_id=-1",
      "scanset_id=9223372036854775807",
      "scanset_id=1.5",
      "scanset_id='1x'",
      "next_group_id=-1",
      "next_group_id=9223372036854775807",
      "next_group_id=1.5",
      "next_group_id='0x'",
      "group_count=-1",
      "group_count=0",
      "group_count=4097",
      "group_count=9223372036854775807",
      "group_count=1.5",
      "group_count='1x'",
      "request=CAST(request AS TEXT)",
  };
  for (const std::string &assignment : campaign_corruptions) {
    CHECK(restore_empty_prefix() && campaign_sql(assignment));
    std::string cursor_before, cursor_after;
    sqlite3_int64 mapping_count_before = -1;
    sqlite3_int64 mapping_count_after = -1;
    CHECK(query_text(corruptor,
                     "SELECT quote(next_group_id) FROM "
                     "acquisition_campaign_tasks WHERE task_id=1",
                     &cursor_before));
    CHECK(query_int64(corruptor,
                      "SELECT COUNT(*) FROM acquisition_campaign_captures "
                      "WHERE task_id=1",
                      &mapping_count_before));
    CHECK(lardon3d_project_db_load_acquisition_campaign_task(
              database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
              database, 1, 1, first.capture_id, 1) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    CHECK(query_text(corruptor,
                     "SELECT quote(next_group_id) FROM "
                     "acquisition_campaign_tasks WHERE task_id=1",
                     &cursor_after));
    CHECK(query_int64(corruptor,
                      "SELECT COUNT(*) FROM acquisition_campaign_captures "
                      "WHERE task_id=1",
                      &mapping_count_after));
    CHECK(cursor_after == cursor_before && mapping_count_before == 0 &&
          mapping_count_after == 0);
  }

  const std::vector<std::string> generic_corruptions{
      "task_kind='wrong'",
      "task_kind=1",
      "task_kind=1.5",
      "task_kind=task_kind||char(0)||'tail'",
      "task_kind_version=-1",
      "task_kind_version=2",
      "task_kind_version=9223372036854775807",
      "task_kind_version=1.5",
      "task_kind_version='1x'",
  };
  for (const std::string &assignment : generic_corruptions) {
    CHECK(restore_empty_prefix() && generic_sql(assignment));
    sqlite3_int64 mapping_count = -1;
    CHECK(lardon3d_project_db_load_acquisition_campaign_task(
              database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    loaded_capture = {99, 99};
    CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
              database, 1, 1, &loaded_capture) ==
              LARDON3D_PROJECT_DB_CORRUPT &&
          loaded_capture.group_id == 0 && loaded_capture.capture_id == 0);
    CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
              database, 1, 1, first.capture_id, 1) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    CHECK(query_int64(corruptor,
                      "SELECT COUNT(*) FROM acquisition_campaign_captures "
                      "WHERE task_id=1",
                      &mapping_count) &&
          mapping_count == 0);
  }

  /* A missing prefix and an ahead mapping are corruption, not resumable
   * identity. Mapping output stays zero and reconstruction installs nothing. */
  CHECK(restore_empty_prefix() && campaign_sql("next_group_id=1"));
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  loaded_capture = {99, 99};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 1, &loaded_capture) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        loaded_capture.group_id == 0 && loaded_capture.capture_id == 0);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 1, wrong.capture_id, 1) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  sqlite3_int64 missing_mapping_count = -1;
  CHECK(query_int64(corruptor,
                    "SELECT COUNT(*) FROM acquisition_campaign_captures "
                    "WHERE task_id=1",
                    &missing_mapping_count) &&
        missing_mapping_count == 0);
  count_binding = {};
  CHECK(!lardon3d_acquisition_campaign_task_reconstruct(
      &snapshot, &count_reconstruction, &count_binding));

  CHECK(restore_valid_prefix() && campaign_sql("next_group_id=0"));
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  loaded_capture = {99, 99};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 1, &loaded_capture) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        loaded_capture.capture_id == 0);

  CHECK(restore_valid_prefix() &&
        execute_sql(
            "INSERT INTO acquisition_campaign_captures"
            "(task_id,group_id,capture_id) VALUES(1,2," +
            std::to_string(second.capture_id) + ")"));
  loaded_capture = {99, 99};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 1, &loaded_capture) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        loaded_capture.capture_id == 0);

  CHECK(restore_valid_prefix() &&
        execute_sql("UPDATE acquisition_campaign_captures SET capture_id=" +
                    std::to_string(wrong.capture_id) +
                    " WHERE task_id=1 AND group_id=1"));
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  loaded_capture = {99, 99};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, 1, 1, &loaded_capture) ==
            LARDON3D_PROJECT_DB_CORRUPT &&
        loaded_capture.capture_id == 0);
  CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
            database, 1, 1, first.capture_id, 1) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  std::string retained_capture;
  CHECK(query_text(corruptor,
                   "SELECT quote(capture_id) FROM "
                   "acquisition_campaign_captures WHERE task_id=1",
                   &retained_capture) &&
        retained_capture == std::to_string(wrong.capture_id));

  const std::vector<std::string> mapping_corruptions{
      "group_id=0",
      "group_id=-1",
      "group_id=1.5",
      "group_id='bad'",
      "capture_id=-1",
      "capture_id=1.5",
      "capture_id='bad'",
      "capture_id=9223372036854775807",
  };
  for (const std::string &assignment : mapping_corruptions) {
    CHECK(restore_valid_prefix() &&
          execute_sql("UPDATE acquisition_campaign_captures SET " +
                      assignment + " WHERE task_id=1"));
    std::string mapping_before, mapping_after;
    CHECK(query_text(
        corruptor,
        "SELECT quote(group_id)||':'||quote(capture_id) FROM "
        "acquisition_campaign_captures WHERE task_id=1",
        &mapping_before));
    CHECK(lardon3d_project_db_load_acquisition_campaign_task(
              database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    loaded_capture = {99, 99};
    CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
              database, 1, 1, &loaded_capture) ==
              LARDON3D_PROJECT_DB_CORRUPT &&
          loaded_capture.capture_id == 0);
    CHECK(lardon3d_project_db_retain_acquisition_campaign_capture(
              database, 1, 1, first.capture_id, 1) ==
          LARDON3D_PROJECT_DB_CORRUPT);
    CHECK(query_text(
        corruptor,
        "SELECT quote(group_id)||':'||quote(capture_id) FROM "
        "acquisition_campaign_captures WHERE task_id=1",
        &mapping_after));
    CHECK(mapping_after == mapping_before);
  }

  /* A row-level count can be within v20 bounds yet disagree with the plan
   * deterministically decoded from the immutable request. Recovery must reject
   * that relation before installing callback ownership. */
  CHECK(restore_valid_prefix() && campaign_sql("group_count=2"));
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, loaded.data(), loaded.size(), &corrupt_task) ==
        LARDON3D_PROJECT_DB_OK);
  count_binding = {};
  CHECK(!lardon3d_acquisition_campaign_task_reconstruct(
      &snapshot, &count_reconstruction, &count_binding));
  CHECK(restore_valid_prefix());
  CHECK(sqlite3_close(corruptor) == SQLITE_OK);
  char project_directory[] = "/tmp/lardon3d-campaign-project-XXXXXX";
  CHECK(mkdtemp(project_directory) != nullptr);
  CHECK(std::filesystem::create_directories(
      std::string(project_directory) + "/.lardon3d/checkpoints"));
  Lardon3DAppState app_state{};
  lardon3d_app_state_init(&app_state);
  app_state.project_loaded = true;
  app_state.project_db = database;
  app_state.resource_governor = governor;
  std::snprintf(app_state.project_path, sizeof(app_state.project_path), "%s",
                project_directory);
  uint64_t invalid_enqueue_id = UINT64_MAX;
  CHECK(!lardon3d_project_enqueue_acquisition_campaign(
            &app_state, target.scanset_id, &input, &invalid_enqueue_id) &&
        invalid_enqueue_id == 0);
  invalid_enqueue_id = UINT64_MAX;
  CHECK(!lardon3d_project_enqueue_acquisition_campaign(
            nullptr, target.scanset_id, &input, &invalid_enqueue_id) &&
        invalid_enqueue_id == 0);
  uint64_t raw_task_id = 0;
  Lardon3DTask *raw_task =
      lardon3d_project_create_acquisition_campaign_task(
          &app_state, target.scanset_id, &input, &raw_task_id);
  CHECK(raw_task != nullptr && raw_task_id == 3);
  Lardon3DResourceEstimate raw_estimate{};
  CHECK(lardon3d_task_resource_estimate(raw_task, &raw_estimate));
  constexpr uint64_t raw_working_bytes =
      UINT64_C(2) * 1024u * 1024u * 1024u;
  CHECK(raw_estimate.memory_fixed_bytes > raw_working_bytes + encoded.size() &&
        raw_estimate.memory_bytes_per_item == 64u * 1024u &&
        raw_estimate.desired_cpu_threads == 1 &&
        raw_estimate.task_class == LARDON3D_RESOURCE_TASK_MIXED);
  lardon3d_task_destroy(raw_task);

  input.ingest_options.representation =
      LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE;
  uint64_t jpeg_task_id = 0;
  Lardon3DTask *jpeg_task =
      lardon3d_project_create_acquisition_campaign_task(
          &app_state, target.scanset_id, &input, &jpeg_task_id);
  CHECK(jpeg_task != nullptr && jpeg_task_id == 4);
  Lardon3DResourceEstimate jpeg_estimate{};
  CHECK(lardon3d_task_resource_estimate(jpeg_task, &jpeg_estimate));
  Lardon3DTaskDurableSnapshot jpeg_snapshot{};
  CHECK(lardon3d_task_durable_snapshot(jpeg_task, &jpeg_snapshot));
  CHECK(jpeg_estimate.memory_fixed_bytes < raw_working_bytes &&
        raw_estimate.memory_fixed_bytes ==
            jpeg_estimate.memory_fixed_bytes + raw_working_bytes &&
        jpeg_estimate.task_class == LARDON3D_RESOURCE_TASK_IMPORT);
  lardon3d_task_destroy(jpeg_task);
  input.ingest_options.representation =
      LARDON3D_ACQUISITION_SELECT_DEVELOP_RAW;

  /* Existing v22 Tasks carry the old operational signature. It is accepted
   * exactly, normalized only for admission, and never rewritten as scientific
   * or generic Task identity. */
  Lardon3DResourceEstimate legacy_estimate = raw_estimate;
  legacy_estimate.memory_fixed_bytes -= raw_working_bytes + encoded.size();
  legacy_estimate.task_class = LARDON3D_RESOURCE_TASK_IMPORT;
  snapshot.estimate = legacy_estimate;
  Lardon3DTaskReconstructionContext valid_reconstruction{
      project_directory, database, governor, nullptr};
  Lardon3DTaskKindBinding recovered{};
  CHECK(lardon3d_acquisition_campaign_task_reconstruct(
      &snapshot, &valid_reconstruction, &recovered));
  Lardon3DTask *restored = lardon3d_task_restore_typed(
      &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
      LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, recovered.callback,
      recovered.userdata, recovered.userdata_destroy);
  CHECK(restored &&
        lardon3d_acquisition_campaign_task_internal_configure_restored(
            restored, recovered.userdata));
  Lardon3DResourceDecision decision{};
  Lardon3DResourceReservation *reservation = nullptr;
  CHECK(lardon3d_task_internal_reserve_available(
      restored, governor, &decision, &reservation));
  Lardon3DResourceReservationInfo admitted{};
  CHECK(reservation && lardon3d_resource_reservation_get_active(
                           governor, reservation, &admitted));
  CHECK(admitted.memory_bytes > raw_working_bytes);
  Lardon3DTaskDurableSnapshot unchanged{};
  CHECK(lardon3d_task_durable_snapshot(restored, &unchanged));
  CHECK(unchanged.estimate.memory_fixed_bytes ==
            legacy_estimate.memory_fixed_bytes &&
        unchanged.estimate.memory_bytes_per_item ==
            legacy_estimate.memory_bytes_per_item &&
        unchanged.estimate.task_class == legacy_estimate.task_class);
  CHECK(lardon3d_resource_governor_release(governor, reservation));
  lardon3d_task_destroy(restored);

  Lardon3DProjectDbAcquisitionCampaignTask jpeg_persisted{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, jpeg_task_id, nullptr, 0, &jpeg_persisted) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(jpeg_estimate.memory_fixed_bytes > jpeg_persisted.request_size);
  jpeg_snapshot.estimate = jpeg_estimate;
  jpeg_snapshot.estimate.memory_fixed_bytes -= jpeg_persisted.request_size;
  recovered = {};
  CHECK(lardon3d_acquisition_campaign_task_reconstruct(
      &jpeg_snapshot, &valid_reconstruction, &recovered));
  restored = lardon3d_task_restore_typed(
      &jpeg_snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
      LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, recovered.callback,
      recovered.userdata, recovered.userdata_destroy);
  CHECK(restored &&
        lardon3d_acquisition_campaign_task_internal_configure_restored(
            restored, recovered.userdata));
  unchanged = {};
  CHECK(lardon3d_task_durable_snapshot(restored, &unchanged) &&
        unchanged.estimate.memory_fixed_bytes ==
            jpeg_snapshot.estimate.memory_fixed_bytes &&
        unchanged.estimate.task_class == LARDON3D_RESOURCE_TASK_IMPORT);
  lardon3d_task_destroy(restored);

  snapshot.estimate = raw_estimate;
  ++snapshot.estimate.memory_fixed_bytes;
  recovered = {};
  CHECK(lardon3d_acquisition_campaign_task_reconstruct(
      &snapshot, &valid_reconstruction, &recovered));
  restored = lardon3d_task_restore_typed(
      &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
      LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, recovered.callback,
      recovered.userdata, recovered.userdata_destroy);
  CHECK(restored &&
        !lardon3d_acquisition_campaign_task_internal_configure_restored(
            restored, recovered.userdata));
  lardon3d_task_destroy(restored);
  snapshot.estimate = raw_estimate;

  Lardon3DAcquisitionCampaignSource execution_sources[2]{};
  std::snprintf(execution_sources[0].path,
                sizeof(execution_sources[0].path), "/input/group-a.jpg");
  std::snprintf(execution_sources[1].path,
                sizeof(execution_sources[1].path), "/input/group-b.jpg");
  for (auto &source : execution_sources) {
    source.source_kind = source.metadata.source_kind =
        LARDON3D_ACQUISITION_SOURCE_JPEG;
    source.metadata_result = LARDON3D_ACQUISITION_OK;
    source.metadata.policy_version =
        LARDON3D_ACQUISITION_PAIRING_POLICY_VERSION;
  }
  Lardon3DAcquisitionCampaignTaskRequest execution_request{};
  execution_request.sources = execution_sources;
  execution_request.source_count = 2;
  execution_request.ingest_options.representation =
      LARDON3D_ACQUISITION_SELECT_JPEG_SOURCE;
  execution_request.ingest_options.select_representation = 1;
  execution_request.ingest_options.imported_at = 321;
  execution_request.ingest_options.max_source_bytes = 1024;
  Lardon3DAcquisitionCampaignPlan execution_plan{};
  CHECK(lardon3d_acquisition_campaign_plan(
            execution_sources, 2, nullptr, 0, &execution_plan) ==
            LARDON3D_ACQUISITION_CAMPAIGN_OK &&
        execution_plan.group_count == 2);
  const uint64_t fixture_captures[2]{first.capture_id, second.capture_id};
  const int previous_opencv_threads = cv::getNumThreads();
  cv::setNumThreads(3);
  CHECK(cv::getNumThreads() == 3);

  uint64_t execution_task_id = 0;
  Lardon3DTask *execution_task =
      lardon3d_project_create_acquisition_campaign_task(
          &app_state, target.scanset_id, &execution_request,
          &execution_task_id);
  CHECK(execution_task && execution_task_id != 0);
  Lardon3DTaskDurableSnapshot execution_snapshot{};
  CHECK(lardon3d_task_durable_snapshot(execution_task, &execution_snapshot));
  lardon3d_task_destroy(execution_task);
  recovered = {};
  CHECK(lardon3d_acquisition_campaign_task_reconstruct(
      &execution_snapshot, &valid_reconstruction, &recovered));
  void *execution_userdata = recovered.userdata;
  restored = lardon3d_task_restore_typed(
      &execution_snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
      LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, recovered.callback,
      recovered.userdata, recovered.userdata_destroy);
  CHECK(restored &&
        lardon3d_acquisition_campaign_task_internal_configure_restored(
            restored, execution_userdata) &&
        lardon3d_acquisition_campaign_task_test_configure_execution(
            execution_userdata, fixture_captures, 2, 0, 3));
  Lardon3DResourceDecision execution_decision{};
  Lardon3DResourceReservation *execution_reservation = nullptr;
  CHECK(lardon3d_task_internal_reserve_available(
      restored, governor, &execution_decision, &execution_reservation));
  Lardon3DResourceReservationInfo execution_admission{};
  CHECK(execution_reservation &&
        lardon3d_resource_reservation_get_active(
            governor, execution_reservation, &execution_admission) &&
        execution_admission.cpu_threads == 1);
  CHECK(lardon3d_task_start(restored, governor, execution_reservation));
  Lardon3DTaskObservation execution_completed{};
  CHECK(lardon3d_task_observation(restored, &execution_completed) &&
        execution_completed.state == TASK_COMPLETED &&
        execution_completed.durable_progress_known &&
        execution_completed.durable_completed == 2 &&
        execution_completed.durable_total == 2 &&
        lardon3d_task_sequence_count(restored) == 1 &&
        cv::getNumThreads() == 3);
  unsigned int observed_threads[2]{};
  size_t observed_count = 0;
  CHECK(lardon3d_acquisition_campaign_task_test_observed_threads(
            execution_userdata, observed_threads, 2, &observed_count) &&
        observed_count == 2 && observed_threads[0] == 1 &&
        observed_threads[1] == 1);
  Lardon3DProjectDbAcquisitionCampaignTask executed_persisted{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, execution_task_id, nullptr, 0, &executed_persisted) ==
            LARDON3D_PROJECT_DB_OK &&
        executed_persisted.next_group_id == 2 &&
        executed_persisted.group_count == 2);
  Lardon3DProjectDbAcquisitionCampaignCapture execution_capture{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, execution_task_id, 1, &execution_capture) ==
            LARDON3D_PROJECT_DB_OK &&
        execution_capture.capture_id == first.capture_id);
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, execution_task_id, 2, &execution_capture) ==
            LARDON3D_PROJECT_DB_OK &&
        execution_capture.capture_id == second.capture_id);
  lardon3d_task_destroy(restored);

  uint64_t failure_task_id = 0;
  execution_task = lardon3d_project_create_acquisition_campaign_task(
      &app_state, target.scanset_id, &execution_request, &failure_task_id);
  CHECK(execution_task && failure_task_id != 0 &&
        failure_task_id != execution_task_id);
  Lardon3DTaskDurableSnapshot failure_snapshot{};
  CHECK(lardon3d_task_durable_snapshot(execution_task, &failure_snapshot));
  lardon3d_task_destroy(execution_task);
  recovered = {};
  CHECK(lardon3d_acquisition_campaign_task_reconstruct(
      &failure_snapshot, &valid_reconstruction, &recovered));
  void *failure_userdata = recovered.userdata;
  restored = lardon3d_task_restore_typed(
      &failure_snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
      LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, recovered.callback,
      recovered.userdata, recovered.userdata_destroy);
  CHECK(restored &&
        lardon3d_acquisition_campaign_task_internal_configure_restored(
            restored, failure_userdata) &&
        lardon3d_acquisition_campaign_task_test_configure_execution(
            failure_userdata, fixture_captures, 2, 2, 3));
  execution_reservation = nullptr;
  CHECK(lardon3d_task_internal_reserve_available(
      restored, governor, &execution_decision, &execution_reservation));
  CHECK(lardon3d_task_start(restored, governor, execution_reservation));
  CHECK(lardon3d_task_observation(restored, &execution_completed) &&
        execution_completed.state == TASK_FAILED &&
        execution_completed.durable_progress_known &&
        execution_completed.durable_completed == 1 &&
        execution_completed.durable_total == 2 &&
        lardon3d_task_sequence_count(restored) == 1 &&
        cv::getNumThreads() == 3);
  observed_count = 0;
  observed_threads[0] = observed_threads[1] = 0;
  CHECK(lardon3d_acquisition_campaign_task_test_observed_threads(
            failure_userdata, observed_threads, 2, &observed_count) &&
        observed_count == 2 && observed_threads[0] == 1 &&
        observed_threads[1] == 1);
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, failure_task_id, nullptr, 0, &executed_persisted) ==
            LARDON3D_PROJECT_DB_OK &&
        executed_persisted.next_group_id == 1);
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, failure_task_id, 1, &execution_capture) ==
            LARDON3D_PROJECT_DB_OK &&
        execution_capture.capture_id == first.capture_id);
  execution_capture = {};
  CHECK(lardon3d_project_db_load_acquisition_campaign_capture(
            database, failure_task_id, 2, &execution_capture) ==
            LARDON3D_PROJECT_DB_NOT_FOUND &&
        execution_capture.capture_id == 0);
  lardon3d_task_destroy(restored);
  cv::setNumThreads(previous_opencv_threads > 0 ? previous_opencv_threads : 1);

  sqlite3 *id_reader = nullptr;
  CHECK(sqlite3_open(database_path, &id_reader) == SQLITE_OK);
  sqlite3_int64 collision_id = 0;
  CHECK(query_int64(id_reader,
                    "SELECT value FROM metadata WHERE key='next_task_id'",
                    &collision_id) &&
        collision_id > 0 && sqlite3_close(id_reader) == SQLITE_OK);
  Lardon3DResourceEstimate collision_estimate{
      1024, 0, 0, 0, 1, 1, 1, 0, 1, LARDON3D_RESOURCE_TASK_IMPORT};
  Lardon3DTask *collision_task = lardon3d_task_create(
      "queue collision", &collision_estimate, completed_task_callback, nullptr);
  CHECK(collision_task &&
        lardon3d_task_assign_id(collision_task,
                                static_cast<uint64_t>(collision_id)));
  app_state.task_queue = lardon3d_task_queue_create(governor, 4);
  CHECK(app_state.task_queue &&
        lardon3d_task_queue_add(app_state.task_queue, collision_task, nullptr));
  uint64_t rejected_enqueue_id = UINT64_MAX;
  CHECK(!lardon3d_project_enqueue_acquisition_campaign(
            &app_state, UINT64_MAX, &input, &rejected_enqueue_id) &&
        rejected_enqueue_id == 0);
  uint64_t durable_transfer_failure_id = UINT64_MAX;
  CHECK(!lardon3d_project_enqueue_acquisition_campaign(
            &app_state, target.scanset_id, &input,
            &durable_transfer_failure_id) &&
        durable_transfer_failure_id == static_cast<uint64_t>(collision_id));
  Lardon3DProjectDbAcquisitionCampaignTask transfer_failure{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, durable_transfer_failure_id, nullptr, 0,
            &transfer_failure) == LARDON3D_PROJECT_DB_OK &&
        transfer_failure.next_group_id == 0);
  lardon3d_task_queue_destroy(app_state.task_queue);
  app_state.task_queue = nullptr;

  std::vector<char> overlong_project_path(LARDON3D_APP_STATE_PATH_CAPACITY + 1,
                                          'x');
  overlong_project_path.back() = '\0';
  Lardon3DTaskReconstructionContext reconstruction{
      overlong_project_path.data(), database, governor, nullptr};
  Lardon3DTaskKindBinding binding{};
  CHECK(!lardon3d_acquisition_campaign_task_reconstruct(
      &snapshot, &reconstruction, &binding));
  lardon3d_resource_governor_destroy(governor);
  lardon3d_project_db_close(database);
  CHECK(unlink(database_path) == 0);
  CHECK(std::filesystem::remove_all(project_directory) > 0);
  return 0;
}
