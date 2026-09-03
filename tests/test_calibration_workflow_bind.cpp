// The fixture calls the C Project DB API directly. Include it with C linkage
// before Calibration Workflow's frozen Tooling/Bootstrap dependency chain.
extern "C" {
#include <lardon3d/project_db.h>
}

#include <lardon3d/calibration_workflow.h>
#include <lardon3d/optical_profiles.h>
#include <lardon3d/project_db.h>

#include <opencv2/imgcodecs.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  std::fprintf(stderr, "workflow binding failure %d: %s\n", __LINE__, #x); \
  return false; \
} } while (0)

namespace {

bool raw_sql(const std::filesystem::path& path, const std::string& sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) return false;
  const int code = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
  return sqlite3_close(database) == SQLITE_OK && code == SQLITE_OK;
}

bool digest(const std::vector<unsigned char>& bytes, unsigned char output[32]) {
  unsigned int size = 0;
  return EVP_Digest(bytes.data(), bytes.size(), output, &size,
                    EVP_sha256(), nullptr) == 1 && size == 32;
}

std::string hex(const unsigned char value[32]) {
  static const char digits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    out[2 * i] = digits[value[i] >> 4];
    out[2 * i + 1] = digits[value[i] & 15u];
  }
  return out;
}

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<unsigned char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path db_path;
  std::filesystem::path image_path;
  std::string image_asset_path;
  Lardon3DProjectDb *database = nullptr;
  uint64_t execution_id = 0;
  uint64_t configuration_id = 0;
  std::vector<unsigned char> image_bytes;
  Lardon3DCalibrationToolingView view{};
  Lardon3DCalibrationToolingCoordinateCheck check{};
  Lardon3DCalibrationWorkflowExternalEvidence external{};
};

void close_fixture(Fixture *fixture) {
  if (fixture->database) {
    lardon3d_project_db_close(fixture->database);
    fixture->database = nullptr;
  }
  if (!fixture->root.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(fixture->root, ec);
  }
}

bool seed_database_rows(const Fixture& fixture,
                        const unsigned char image_sha[32]) {
  std::ostringstream sql;
  sql << "INSERT INTO scansets(scanset_id,name,created_at,updated_at) "
         "VALUES(1,'scope',1,1);"
      << "INSERT INTO tasks VALUES(1,'quality','photo_quality.triage',1,"
         "5,5,100,1,0,0,0,0,1);"
      << "INSERT INTO tasks VALUES(2,'campaign','acquisition_campaign.run',1,"
         "5,5,100,1,0,0,0,0,1);"
      << "INSERT INTO photo_quality_triage_tasks VALUES(1,1,2,1,X'01');"
      << "INSERT INTO photo_quality_triage_results VALUES("
         "1,1,0,1,0,1,0,100,100,100,100,1.0,1.0,0.0,0.0,1.0,1.0,0.0,'GOOD');"
      << "INSERT INTO acquisition_campaign_tasks VALUES(2,1,2,2,X'02');"
      << "INSERT INTO captures(capture_id,scanset_id,created_at) VALUES(1,1,1);"
      << "INSERT INTO acquisition_campaign_captures VALUES(2,2,1);"
      << "INSERT INTO image_assets(asset_id,sha256,path,size_bytes,state,created_at) "
         "VALUES(1,X'" << hex(image_sha)
      << "','" << fixture.image_asset_path << "'," << fixture.image_bytes.size()
      << ",1,1);"
      << "INSERT INTO images(image_id,scanset_id,asset_id,original_name,"
         "source_path,imported_at) VALUES("
         "1,1,1,'test.png','/source/test.png',1);"
      << "INSERT INTO capture_images VALUES(1,1);";
  return raw_sql(fixture.db_path, sql.str());
}

bool create_optical_configuration(Fixture *fixture) {
  Lardon3DOpticalCameraBodyProfile body_input{};
  std::memcpy(body_input.manufacturer, "Generic", sizeof("Generic"));
  std::memcpy(body_input.model, "Binding body", sizeof("Binding body"));
  std::memcpy(body_input.name, "Binding body", sizeof("Binding body"));
  Lardon3DOpticalCameraBodyProfile body{};
  if (lardon3d_optical_camera_body_create(
          fixture->database, &body_input, &body) != LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalLensProfile lens_input{};
  lens_input.interface_kind = LARDON3D_OPTICAL_LENS_MANUAL;
  lens_input.focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_PRIME;
  lens_input.minimum_focal_um = 35000;
  lens_input.maximum_focal_um = 35000;
  std::memcpy(lens_input.manufacturer, "Generic", sizeof("Generic"));
  std::memcpy(lens_input.model, "Binding 35", sizeof("Binding 35"));
  std::memcpy(lens_input.name, "Binding 35 mm", sizeof("Binding 35 mm"));
  Lardon3DOpticalLensProfile lens{};
  if (lardon3d_optical_lens_create(
          fixture->database, &lens_input, &lens) != LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalConfiguration config_input{};
  config_input.camera_body_profile_id = body.camera_body_profile_id;
  config_input.lens_profile_id = lens.lens_profile_id;
  config_input.has_focal_length = true;
  config_input.focal_length_um = 35000;
  Lardon3DOpticalConfiguration configuration{};
  if (lardon3d_optical_configuration_create(
          fixture->database, &config_input, &configuration) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  if (lardon3d_optical_capture_assign_explicit(
          fixture->database, 1, configuration.optical_configuration_id) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  fixture->configuration_id = configuration.optical_configuration_id;
  return true;
}

void populate_external(Fixture *fixture, uint32_t width, uint32_t height) {
  auto& e = fixture->external;
  std::memset(&e, 0, sizeof(e));
  e.boundary.selected_execution_id = fixture->execution_id;
  e.boundary.optical_configuration_id = fixture->configuration_id;
  e.boundary.capture_count = 1;
  e.boundary.capture_ids[0] = 1;
  std::memset(e.boundary.optical_state_sha256, 0x11, 32);
  std::memset(e.optical_state_sha256, 0x11, 32);
  std::memset(e.target_sha256, 0x21, 32);
  std::memset(e.solver_executable_sha256, 0x31, 32);
  std::memset(e.solver_configuration_sha256, 0x41, 32);
  std::memset(e.initialization_evidence_sha256, 0x51, 32);
  std::memset(e.validation_evidence_sha256, 0x61, 32);

  e.target_family =
      LARDON3D_CALIBRATION_TOOLING_TARGET_CHARUCO_9X7_DICT_5X5_100;
  e.target_squares_x = 9;
  e.target_squares_y = 7;
  e.target_square_length_mm = 30.0;
  e.target_marker_length_mm = 21.0;
  e.target_active_width_mm = 270.0;
  e.target_active_height_mm = 210.0;
  e.target_white_border_mm = 30.0;
  for (double& measurement : e.target_measurements_mm) measurement = 30.0;
  e.measurement_resolution_mm = 0.1;
  e.target_flatness_mm = NAN;
  e.holdout_rmse_px = 0.1;
  e.holdout_maximum_residual_px = 0.2;
  e.oriented_width = width;
  e.oriented_height = height;

  fixture->view.accepted = 1;
  fixture->view.corner_count = 40;
  fixture->view.residual_count = 40;
  fixture->view.target_corner_quadrant_mask = 15;
  fixture->view.target_occupancy = 0.3;
  fixture->view.normal_angle_degrees = 25.0;
  fixture->view.distance_metres = 0.4;
  fixture->view.reprojection_rmse_px = 0.1;
  fixture->view.maximum_residual_px = 0.2;
  fixture->view.quadrant = 0;
  fixture->view.distance_band = 1;
  e.views = &fixture->view;
  e.view_count = 1;

  e.coordinate_checks = &fixture->check;
  e.coordinate_check_count = 1;

  const double parameters[8] = {
      80.0, 81.0, 50.0, 40.0, 0.01, -0.01, 0.001, -0.001};
  for (size_t run = 0; run < 3; ++run)
    std::memcpy(e.repeated_parameters[run], parameters, sizeof(parameters));
  std::memcpy(e.fit_parameters, parameters, sizeof(parameters));
  e.support_images = 40;
  e.support_observations = 1600;
  e.reprojection_rmse_px = 0.1;
  e.maximum_residual_px = 0.2;
  e.maximum_parameter_delta = 0.0;
  e.validation_flags = LARDON3D_CALIBRATION_TOOLING_VALIDATION_FLAGS;
}

bool make_fixture(Fixture *fixture, uint32_t width = 100,
                  uint32_t height = 80, bool complete = true) {
  char temp[] = "/tmp/lardon3d-workflow-bind-XXXXXX";
  char *root = mkdtemp(temp);
  if (!root) return false;
  fixture->root = root;
  fixture->db_path = fixture->root / "project.db";

  cv::Mat image(static_cast<int>(height), static_cast<int>(width),
                CV_8UC1, cv::Scalar(127));
  if (!cv::imencode(".png", image, fixture->image_bytes))
    return false;
  unsigned char image_sha[32]{};
  if (!digest(fixture->image_bytes, image_sha)) return false;
  const std::string image_hex = hex(image_sha);
  fixture->image_asset_path =
      "assets/images/" + image_hex.substr(0, 2) + "/" + image_hex;
  fixture->image_path = fixture->root / fixture->image_asset_path;
  if (!std::filesystem::create_directories(fixture->image_path.parent_path()) ||
      !write_bytes(fixture->image_path, fixture->image_bytes))
    return false;

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  if (lardon3d_project_db_open(
          fixture->db_path.c_str(), &fixture->database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  lardon3d_project_db_close(fixture->database);
  fixture->database = nullptr;

  if (!seed_database_rows(*fixture, image_sha)) return false;
  if (lardon3d_project_db_open(
          fixture->db_path.c_str(), &fixture->database, error) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  if (!create_optical_configuration(fixture)) return false;
  if (lardon3d_project_db_set_selected_capture_image(
          fixture->database, 1, 1) != LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DProjectDbSelectedExecutionItem item{};
  item.item_index = 0;
  item.quality_group_id = 1;
  item.campaign_group_id = 2;
  item.capture_id = 1;
  item.representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE;
  item.source_asset_id = 0;
  Lardon3DProjectDbSelectedExecution execution{};
  if (lardon3d_project_db_create_selected_execution(
          fixture->database, 1, 2, &item, 1, 10, &execution) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  fixture->execution_id = execution.execution_id;

  if (complete &&
      lardon3d_project_db_record_selected_representation(
          fixture->database, fixture->execution_id, 0, 1, 1) !=
          LARDON3D_PROJECT_DB_OK)
    return false;

  populate_external(fixture, width, height);
  return true;
}

bool valid_binding() {
  Fixture fixture;
  CHECK(make_fixture(&fixture));

  Lardon3DProjectDbSelectedExecution before{};
  CHECK(lardon3d_project_db_load_selected_execution(
            fixture.database, fixture.execution_id, &before) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(before.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION);
  CHECK(!before.has_calibration_scope);

  Lardon3DCalibrationToolingEntry entries[1]{};
  Lardon3DCalibrationToolingEvidence tooling{};
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &fixture.external,
            entries, 1, &tooling) == LARDON3D_CALIBRATION_WORKFLOW_OK);

  CHECK(tooling.entries == entries && tooling.entry_count == 1);
  CHECK(tooling.views == fixture.external.views && tooling.view_count == 1);
  CHECK(entries[0].image_id == 1);
  CHECK(entries[0].width == 100 && entries[0].height == 80);
  CHECK(entries[0].fx == 80.0 && entries[0].fy == 81.0);
  CHECK(entries[0].fit_fx == 80.0);
  CHECK(entries[0].support_images == 40 &&
        entries[0].support_observations == 1600);
  CHECK(entries[0].validation_flags ==
        LARDON3D_CALIBRATION_TOOLING_VALIDATION_FLAGS);

  Lardon3DCalibrationToolingEntry second[1]{};
  Lardon3DCalibrationToolingEvidence second_tooling{};
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &fixture.external,
            second, 1, &second_tooling) == LARDON3D_CALIBRATION_WORKFLOW_OK);
  CHECK(std::memcmp(entries, second, sizeof(entries)) == 0);

  Lardon3DProjectDbSelectedExecution after{};
  CHECK(lardon3d_project_db_load_selected_execution(
            fixture.database, fixture.execution_id, &after) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(after.stage == before.stage &&
        after.next_item_index == before.next_item_index &&
        after.has_calibration_scope == before.has_calibration_scope &&
        after.calibration_scope_id == before.calibration_scope_id);

  close_fixture(&fixture);
  return true;
}

bool mismatch_rejections() {
  Fixture fixture;
  CHECK(make_fixture(&fixture));
  Lardon3DCalibrationToolingEntry entries[1]{};
  Lardon3DCalibrationToolingEvidence tooling{};

  auto changed = fixture.external;
  changed.boundary.capture_ids[0] = 999;
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &changed,
            entries, 1, &tooling) ==
        LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);

  changed = fixture.external;
  ++changed.boundary.optical_configuration_id;
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &changed,
            entries, 1, &tooling) ==
        LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);

  changed = fixture.external;
  ++changed.oriented_width;
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &changed,
            entries, 1, &tooling) ==
        LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);

  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &fixture.external,
            entries, 0, &tooling) == LARDON3D_CALIBRATION_WORKFLOW_CAPACITY);

  const auto backing = fixture.image_path.parent_path() / "backing.png";
  std::error_code rename_error;
  std::filesystem::rename(fixture.image_path, backing, rename_error);
  CHECK(!rename_error);
  CHECK(symlink("backing.png", fixture.image_path.c_str()) == 0);
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &fixture.external,
            entries, 1, &tooling) ==
        LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE);
  CHECK(unlink(fixture.image_path.c_str()) == 0);
  rename_error.clear();
  std::filesystem::rename(backing, fixture.image_path, rename_error);
  CHECK(!rename_error);

  close_fixture(&fixture);

  CHECK(make_fixture(&fixture, 100, 80, false));
  CHECK(lardon3d_calibration_workflow_bind_selected_execution(
            fixture.database, fixture.root.c_str(), &fixture.external,
            entries, 1, &tooling) ==
        LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);
  close_fixture(&fixture);
  return true;
}

}  // namespace

int main() {
  if (!valid_binding() || !mismatch_rejections()) return 1;
  std::puts("CALIBRATION_WORKFLOW_SELECTED_EXECUTION_BINDING_V1=PASS");
  return 0;
}
