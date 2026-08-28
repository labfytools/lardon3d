#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sqlite3.h>
#include <unistd.h>

extern "C" {
#include <lardon3d/image_catalog.h>
#include <lardon3d/project.h>
#include <lardon3d/raw_development_task.h>
#include <lardon3d/resource_governor.h>
}

extern "C" const Lardon3DTaskKindRegistry *lardon3d_task_kind_registry_production(void) {
  static const Lardon3DTaskKindRegistry registry{};
  return &registry;
}

#define CHECK(condition)                                                               \
  do {                                                                                 \
    if (!(condition)) {                                                                \
      std::fprintf(stderr, "Failure line %d: %s\n", __LINE__, #condition);             \
      return 1;                                                                        \
    }                                                                                  \
  } while (0)

static bool write_file(const std::string &path, const char *bytes) {
  int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) return false;
  size_t size = std::strlen(bytes);
  bool ok = write(descriptor, bytes, size) == static_cast<ssize_t>(size);
  return close(descriptor) == 0 && ok;
}

int main() {
  char directory_template[] = "/tmp/lardon3d-raw-task-XXXXXX";
  char *directory = mkdtemp(directory_template);
  CHECK(directory != nullptr);
  std::string root(directory);
  std::string database_path = root + "/project.lardon3d";
  CHECK(std::filesystem::create_directories(root + "/.lardon3d/checkpoints"));
  CHECK(std::filesystem::create_directories(root + "/assets/images"));
  std::string raw_path = root + "/source.arw";
  std::string png_path = root + "/derived.png";
  CHECK(write_file(raw_path, "explicit raw bytes"));
  CHECK(write_file(png_path, "derived png bytes"));

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  Lardon3DProjectDb *database = nullptr;
  CHECK(lardon3d_project_db_open(database_path.c_str(), &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbScanSet scanset{};
  Lardon3DProjectDbCapture capture{};
  CHECK(lardon3d_project_db_create_scanset(database, "raw-task", &scanset) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_create_capture(database, scanset.scanset_id, 7, &capture) ==
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
  policy.io_slot_capacity = 1;
  state.resource_governor = lardon3d_resource_governor_create(&state.hardware_profile, &policy);
  CHECK(state.resource_governor != nullptr);

  Lardon3DProjectDbImageAsset raw_asset{};
  Lardon3DProjectDbImageAsset derived_asset{};
  CHECK(lardon3d_image_catalog_publish_asset_file(
            &state, raw_path.c_str(), 7, UINT64_MAX, &raw_asset) ==
        LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
  CHECK(lardon3d_project_db_record_capture_source_asset(
            database, capture.capture_id, raw_asset.asset_id,
            LARDON3D_DB_CAPTURE_SOURCE_RAW) == LARDON3D_PROJECT_DB_OK);

  uint64_t task_id = 0;
  Lardon3DTask *task = lardon3d_project_create_raw_development_task(
      &state, capture.capture_id, raw_asset.asset_id, &task_id);
  CHECK(task != nullptr && task_id != 0);
  Lardon3DProjectDbTask generic{};
  Lardon3DProjectDbRawDevelopmentTask typed{};
  CHECK(lardon3d_project_db_load_task(database, task_id, &generic) == LARDON3D_PROJECT_DB_OK);
  CHECK(generic.has_checkpoint && generic.has_task_kind &&
        std::strcmp(generic.task_kind, LARDON3D_RAW_DEVELOPMENT_TASK_KIND) == 0);
  CHECK(lardon3d_project_db_load_raw_development_task(database, task_id, &typed) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(typed.capture_id == capture.capture_id && typed.source_asset_id == raw_asset.asset_id &&
        typed.phase == LARDON3D_RAW_DEVELOPMENT_TASK_PENDING && !typed.has_image);

  Lardon3DTaskDurableSnapshot snapshot{};
  CHECK(lardon3d_task_durable_snapshot(task, &snapshot));
  Lardon3DTaskReconstructionContext reconstruction{
      root.c_str(), database, state.resource_governor, nullptr};
  Lardon3DTaskKindBinding binding{};
  CHECK(lardon3d_raw_development_task_reconstruct(&snapshot, &reconstruction, &binding));
  CHECK(binding.callback != nullptr && binding.userdata != nullptr &&
        binding.userdata_destroy != nullptr && binding.finished_callback != nullptr);
  binding.userdata_destroy(binding.userdata);

  CHECK(lardon3d_image_catalog_publish_asset_file(
            &state, png_path.c_str(), 7, UINT64_MAX, &derived_asset) ==
        LARDON3D_IMAGE_CATALOG_ASSET_PUBLISHED);
  Lardon3DProjectDbAssetDerivation derivation{};
  derivation.parent_asset_id = raw_asset.asset_id;
  derivation.child_asset_id = derived_asset.asset_id;
  derivation.kind = LARDON3D_DB_ASSET_DERIVATION_GENERIC_VERSIONED;
  derivation.version = 1;
  derivation.has_producer_task = true;
  derivation.producer_task_id = task_id;
  derivation.created_at = 7;
  CHECK(lardon3d_project_db_record_asset_derivation(database, &derivation) ==
        LARDON3D_PROJECT_DB_OK);
  Lardon3DProjectDbImageRegisterStatus image_status{};
  Lardon3DProjectDbImage image{};
  CHECK(lardon3d_project_db_publish_derived_capture_image(
            database, capture.capture_id, derived_asset.asset_id, "derived.png", png_path.c_str(),
            task_id, 7, &image_status, &image) == LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_task_set_progress(task, 100, "published"));
  typed.phase = LARDON3D_RAW_DEVELOPMENT_TASK_PUBLISHED;
  typed.has_image = true;
  typed.image_id = image.image_id;
  CHECK(lardon3d_project_checkpoint_raw_development_task(&state, task, &typed) ==
        LARDON3D_PROJECT_TASK_CHECKPOINT_OK);
  Lardon3DProjectDbRawDevelopmentTask published{};
  CHECK(lardon3d_project_db_load_raw_development_task(database, task_id, &published) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(published.phase == LARDON3D_RAW_DEVELOPMENT_TASK_PUBLISHED &&
        published.image_id == image.image_id);

  Lardon3DProjectDbRawDevelopmentTask conflict = published;
  conflict.source_asset_id = derived_asset.asset_id;
  CHECK(lardon3d_project_checkpoint_raw_development_task(&state, task, &conflict) ==
        LARDON3D_PROJECT_TASK_CHECKPOINT_DB_ERROR);
  CHECK(lardon3d_project_db_load_raw_development_task(database, task_id, &published) ==
            LARDON3D_PROJECT_DB_OK &&
        published.source_asset_id == raw_asset.asset_id && published.image_id == image.image_id);

  CHECK(lardon3d_project_db_record_raw_development_task(
            database, &snapshot, "test.work", 1, nullptr, &typed,
            7) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);
  CHECK(lardon3d_project_db_record_raw_development_task(
            database, &snapshot, LARDON3D_RAW_DEVELOPMENT_TASK_KIND, 2, nullptr, &typed,
            7) == LARDON3D_PROJECT_DB_INVALID_ARGUMENT);

  sqlite3 *raw_database = nullptr;
  char update_mismatch_kind[128];
  std::snprintf(update_mismatch_kind, sizeof(update_mismatch_kind),
                "UPDATE tasks SET task_kind='test.work',task_kind_version=1 WHERE task_id=%llu",
                (unsigned long long)task_id);
  CHECK(sqlite3_open(database_path.c_str(), &raw_database) == SQLITE_OK);
  CHECK(sqlite3_exec(raw_database, update_mismatch_kind, nullptr, nullptr,
                     nullptr) == SQLITE_OK);
  CHECK(lardon3d_project_db_load_raw_development_task(database, task_id, &published) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  char update_recoverable_kind[128];
  std::snprintf(update_recoverable_kind, sizeof(update_recoverable_kind),
                "UPDATE tasks SET task_kind='raw.develop',task_kind_version=1 "
                "WHERE task_id=%llu",
                (unsigned long long)task_id);
  CHECK(sqlite3_exec(raw_database, update_recoverable_kind, nullptr, nullptr,
                     nullptr) == SQLITE_OK);
  CHECK(sqlite3_close(raw_database) == SQLITE_OK);

  lardon3d_task_destroy(task);
  lardon3d_resource_governor_destroy(state.resource_governor);
  lardon3d_project_db_close(database);

  /* This fixture bypasses schema CHECK enforcement deliberately to prove that
   * recovery treats malformed durable phase as corruption, not as pending. */
  CHECK(sqlite3_open(database_path.c_str(), &raw_database) == SQLITE_OK);
  CHECK(sqlite3_exec(raw_database, "PRAGMA ignore_check_constraints=ON", nullptr, nullptr,
                     nullptr) == SQLITE_OK);
  CHECK(sqlite3_exec(raw_database, "UPDATE raw_development_tasks SET phase=9", nullptr, nullptr,
                     nullptr) == SQLITE_OK);
  CHECK(sqlite3_close(raw_database) == SQLITE_OK);
  database = nullptr;
  CHECK(lardon3d_project_db_open(database_path.c_str(), &database, error) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(lardon3d_project_db_load_raw_development_task(database, task_id, &published) ==
        LARDON3D_PROJECT_DB_CORRUPT);
  lardon3d_project_db_close(database);
  CHECK(std::filesystem::remove_all(root) > 0);
  return 0;
}
