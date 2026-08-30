#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

extern "C" {
#include <lardon3d/acquisition_campaign_task.h>
#include <lardon3d/project_db.h>
#include "../src/task_internal.h"
}

extern "C" bool
lardon3d_acquisition_campaign_task_internal_configure_restored(
    Lardon3DTask *, void *);

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

  size_t size = 0;
  CHECK(
      lardon3d_acquisition_campaign_request_encode(&input, nullptr, 0, &size));
  CHECK(size > 32);
  std::vector<unsigned char> encoded(size);
  CHECK(lardon3d_acquisition_campaign_request_encode(&input, encoded.data(),
                                                     encoded.size(), &size));
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
  CHECK(!lardon3d_acquisition_campaign_request_encode(&input, nullptr, 0,
                                                       &size));
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
  Lardon3DTaskDurableSnapshot snapshot{};
  snapshot.id = 1;
  std::snprintf(snapshot.name, sizeof(snapshot.name), "campaign");
  snapshot.saved_state = snapshot.recovery_state = TASK_PENDING;
  Lardon3DProjectDbAcquisitionCampaignTask durable{
      1, target.scanset_id, 0, 2, encoded.data(), encoded.size()};
  CHECK(lardon3d_project_db_record_acquisition_campaign_task(
            database, &snapshot, LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND,
            LARDON3D_ACQUISITION_CAMPAIGN_TASK_KIND_VERSION, nullptr, &durable,
            1) == LARDON3D_PROJECT_DB_OK);
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
  std::vector<unsigned char> loaded(encoded.size());
  Lardon3DProjectDbAcquisitionCampaignTask loaded_task{};
  CHECK(lardon3d_project_db_load_acquisition_campaign_task(
            database, 1, loaded.data(), loaded.size(), &loaded_task) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(loaded_task.next_group_id == 1 && loaded_task.group_count == 2 &&
        loaded_task.request_size == encoded.size() &&
        std::memcmp(loaded.data(), encoded.data(), encoded.size()) == 0);
  snapshot.estimate = Lardon3DResourceEstimate{
      256 * 1024, 0, 64 * 1024, 0, 1, 1, 1, 0, 1,
      LARDON3D_RESOURCE_TASK_IMPORT};
  Lardon3DHardwareProfile profile{16, 4096, UINT64_MAX, false, 0, false,
                                  false, 0, "test", ""};
  Lardon3DResourcePolicy policy{};
  policy.maximum_cpu_load_ratio = 1.0;
  policy.maximum_io_pressure_avg10 = 100.0;
  policy.io_slot_capacity = 1;
  Lardon3DResourceGovernor *governor =
      lardon3d_resource_governor_create(&profile, &policy);
  CHECK(governor != nullptr);
  Lardon3DTaskReconstructionContext valid_reconstruction{
      "/tmp", database, governor, nullptr};
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
  CHECK(admitted.memory_bytes > 320 * 1024);
  Lardon3DTaskDurableSnapshot unchanged{};
  CHECK(lardon3d_task_durable_snapshot(restored, &unchanged));
  CHECK(unchanged.estimate.memory_fixed_bytes == 256 * 1024 &&
        unchanged.estimate.memory_bytes_per_item == 64 * 1024);
  CHECK(lardon3d_resource_governor_release(governor, reservation));
  lardon3d_task_destroy(restored);
  lardon3d_resource_governor_destroy(governor);
  std::vector<char> overlong_project_path(LARDON3D_APP_STATE_PATH_CAPACITY + 1,
                                          'x');
  overlong_project_path.back() = '\0';
  Lardon3DTaskReconstructionContext reconstruction{
      overlong_project_path.data(), database, nullptr, nullptr};
  Lardon3DTaskKindBinding binding{};
  CHECK(!lardon3d_acquisition_campaign_task_reconstruct(
      &snapshot, &reconstruction, &binding));
  lardon3d_project_db_close(database);
  CHECK(unlink(database_path) == 0);
  return 0;
}
