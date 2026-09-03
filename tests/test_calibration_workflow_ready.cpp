// The fixture calls the C Project DB API directly. Establish C linkage before
// Calibration Workflow's frozen Tooling/Bootstrap dependency chain.
extern "C" {
#include <lardon3d/project_db.h>
}

#include <lardon3d/calibration_workflow.h>
#include <lardon3d/optical_profiles.h>

#include <opencv2/imgcodecs.hpp>
#include <openssl/evp.h>
#include <sqlite3.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  std::fprintf(stderr, "workflow ready failure %d: %s\n", __LINE__, #x); \
  return false; \
} } while (0)

namespace {

constexpr size_t kViewCount = 60;
constexpr size_t kChecksPerView = 20;
constexpr size_t kCoordinateCount = kViewCount * kChecksPerView;

std::string hex_repeat(unsigned char value) {
  static const char digits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    out[2 * i] = digits[value >> 4];
    out[2 * i + 1] = digits[value & 15u];
  }
  return out;
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

std::string hf(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::hexfloat << value;
  return out.str();
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<unsigned char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

bool digest(const unsigned char *bytes, size_t size, unsigned char output[32]) {
  unsigned int written = 0;
  return EVP_Digest(bytes, size, output, &written, EVP_sha256(), nullptr) == 1 &&
         written == 32;
}

std::string sha_file_hex(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << in.rdbuf();
  const std::string data = bytes.str();
  unsigned char value[32]{};
  if (!in || !digest(reinterpret_cast<const unsigned char *>(data.data()),
                     data.size(), value))
    return {};
  return hex(value);
}

bool raw_sql(const std::filesystem::path& path, const std::string& sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) return false;
  const int code = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
  return sqlite3_close(database) == SQLITE_OK && code == SQLITE_OK;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path db_path;
  std::filesystem::path session;
  std::filesystem::path detection;
  std::filesystem::path solve;
  std::filesystem::path evidence;
  std::filesystem::path producer;
  std::filesystem::path campaign;
  std::filesystem::path image_path;
  std::string image_asset_path;
  std::vector<unsigned char> image_bytes;
  Lardon3DProjectDb *database = nullptr;
  uint64_t execution_id = 0;
  uint64_t configuration_id = 0;
  Lardon3DCalibrationWorkflowInputFiles files{};
};

void close_fixture(Fixture *fixture) {
  if (fixture->database) {
    lardon3d_project_db_close(fixture->database);
    fixture->database = nullptr;
  }
  if (!fixture->root.empty()) {
    std::error_code error;
    std::filesystem::remove_all(fixture->root, error);
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
         "VALUES(1,X'" << hex(image_sha) << "','" << fixture.image_asset_path
      << "'," << fixture.image_bytes.size() << ",1,1);"
      << "INSERT INTO images(image_id,scanset_id,asset_id,original_name,"
         "source_path,imported_at) VALUES("
         "1,1,1,'selected.png','/source/selected.png',1);"
      << "INSERT INTO capture_images VALUES(1,1);";
  return raw_sql(fixture.db_path, sql.str());
}

bool create_optical_configuration(Fixture *fixture) {
  Lardon3DOpticalCameraBodyProfile body_input{};
  std::memcpy(body_input.manufacturer, "Generic", sizeof("Generic"));
  std::memcpy(body_input.model, "Workflow body", sizeof("Workflow body"));
  std::memcpy(body_input.name, "Workflow body", sizeof("Workflow body"));
  Lardon3DOpticalCameraBodyProfile body{};
  if (lardon3d_optical_camera_body_create(fixture->database, &body_input, &body) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalLensProfile lens_input{};
  lens_input.interface_kind = LARDON3D_OPTICAL_LENS_MANUAL;
  lens_input.focal_range_kind = LARDON3D_OPTICAL_FOCAL_RANGE_PRIME;
  lens_input.minimum_focal_um = 35000;
  lens_input.maximum_focal_um = 35000;
  std::memcpy(lens_input.manufacturer, "Generic", sizeof("Generic"));
  std::memcpy(lens_input.model, "Workflow 35", sizeof("Workflow 35"));
  std::memcpy(lens_input.name, "Workflow 35 mm", sizeof("Workflow 35 mm"));
  Lardon3DOpticalLensProfile lens{};
  if (lardon3d_optical_lens_create(fixture->database, &lens_input, &lens) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DOpticalConfiguration input{};
  input.camera_body_profile_id = body.camera_body_profile_id;
  input.lens_profile_id = lens.lens_profile_id;
  input.has_focal_length = true;
  input.focal_length_um = 35000;
  Lardon3DOpticalConfiguration configuration{};
  if (lardon3d_optical_configuration_create(
          fixture->database, &input, &configuration) != LARDON3D_PROJECT_DB_OK)
    return false;
  if (lardon3d_optical_capture_assign_explicit(
          fixture->database, 1, configuration.optical_configuration_id) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  fixture->configuration_id = configuration.optical_configuration_id;
  return true;
}

std::string parameters() {
  const double values[8] = {1000.0, 1001.0, 500.0, 400.0,
                            0.01, -0.01, 0.001, -0.001};
  std::ostringstream out;
  out << '[';
  for (size_t i = 0; i < 8; ++i) {
    if (i) out << ',';
    out << '"' << hf(values[i]) << '"';
  }
  out << ']';
  return out.str();
}

std::string pose() {
  return "{\"rvec\":[\"" + hf(0.0) + "\",\"" + hf(0.0) + "\",\"" +
         hf(0.0) + "\"],\"tvec_m\":[\"" + hf(0.0) + "\",\"" + hf(0.0) +
         "\",\"" + hf(1.0) + "\"]}";
}

bool write_external_inputs(Fixture *fixture, double holdout_rmse) {
  const std::string optical = hex_repeat(0x11);
  const std::string generator = hex_repeat(0x12);
  const std::string planarity = hex_repeat(0x13);
  static const std::array<const char *, 9> labels = {
      "center", "top", "right", "bottom", "left", "top_left",
      "top_right", "bottom_left", "bottom_right"};
  static const uint32_t quadrants[10] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4};
  static const uint32_t distances[10] = {0, 1, 2, 0, 1, 2, 0, 1, 2, 0};
  static const double angles[10] = {25, 40, 55, 25, 40, 55, 25, 40, 55, 25};

  std::ostringstream session;
  session << "L3DCAL_SESSION_V1\n"
          << "target board " << generator << " DICT_5X5_100 9 7 30 21\n"
          << "measurement caliper 0.1 30 30 30 30 30 30 30 30 30 30\n"
          << "white_border 30\n"
          << "planarity PASS " << planarity << "\n"
          << "decoder qualified_decoder 1\n"
          << "optical_state " << optical
          << " body_objective_zoom_focus_stabilization_format_pipeline\n";
  for (size_t i = 0; i < kViewCount; ++i) {
    const std::string sha = hex_repeat(static_cast<unsigned char>(i + 0x20));
    const uint32_t group = static_cast<uint32_t>(i / 6);
    const double distance = 1.0 + static_cast<double>(distances[group]) * 0.3;
    session << "image /retained/view-" << i << ".png " << sha << " 0\n"
            << "pre_solve " << sha << " 0.1\n"
            << "clipping " << sha << " 0\n"
            << "coordinate " << sha
            << " qualified_decoder 1 0 1000 800 20 0 0\n";
    for (size_t point = 0; point < kChecksPerView; ++point) {
      session << "coordinate_point " << sha << ' '
              << labels[point % labels.size()] << ' ' << point << ' ' << point + 1
              << ' ' << point << ' ' << point + 1 << "\n";
    }
    session << "distance " << sha << ' ' << std::setprecision(17)
            << std::defaultfloat << distance << ' ' << distances[group] << "\n";
  }
  if (!write_text(fixture->session, session.str())) return false;

  std::ostringstream detection;
  detection << "{\n\"format\":\"L3DCAL_DETECTION_V1\",\n"
            << "\"decoder\":\"qualified_decoder\",\n"
            << "\"decoder_version\":\"1\",\n\"views\":[\n";
  for (size_t i = 0; i < kViewCount; ++i) {
    const std::string sha = hex_repeat(static_cast<unsigned char>(i + 0x20));
    const uint32_t group = static_cast<uint32_t>(i / 6);
    const double distance = 1.0 + static_cast<double>(distances[group]) * 0.3;
    detection << "{\"source_sha256\":\"" << sha
              << "\",\"orientation\":0,\"oriented_width\":1000,"
                 "\"oriented_height\":800,\"decision\":\"accepted\","
                 "\"reason\":\"-\",\"frame_region\":" << quadrants[group]
              << ",\"distance_band\":" << distances[group]
              << ",\"holdout\":" << ((i % 6) == 4 ? "true" : "false")
              << ",\"target_occupancy\":\"" << hf(0.4)
              << "\",\"normal_angle_degrees\":\"" << hf(angles[group])
              << "\",\"measured_distance_metres\":\"" << hf(distance)
              << "\",\"pre_solve_corner_rms_px\":\"" << hf(0.1)
              << "\",\"clipping_fraction\":\"" << hf(0.0)
              << "\",\"physical_target_quadrants\":3,"
                 "\"target_corner_quadrant_mask\":7,\"corner_count\":40,"
                 "\"residual_count\":40,\"high_residual_count\":0,"
                 "\"reprojection_rmse_px\":\"" << hf(0.4)
              << "\",\"maximum_residual_px\":\"" << hf(0.8)
              << "\",\"coordinate_equivalence\":{\"comparison_points\":20,"
                 "\"max_abs_dx_px\":\"" << hf(0.0)
              << "\",\"max_abs_dy_px\":\"" << hf(0.0)
              << "\",\"pass\":true},\"corners\":[";
    for (size_t corner = 0; corner < 40; ++corner) {
      if (corner) detection << ',';
      detection << "{\"id\":" << corner << ",\"x\":\""
                << hf(100.0 + static_cast<double>(corner)) << "\",\"y\":\""
                << hf(200.0 + static_cast<double>(corner)) << "\"}";
    }
    detection << "]}" << (i + 1 == kViewCount ? "\n" : ",\n");
  }
  detection << "]\n}\n";
  if (!write_text(fixture->detection, detection.str())) return false;

  std::ostringstream solve;
  solve << "{\n\"format\":\"L3DCAL_SOLVE_V1\",\n\"runs\":[\n";
  for (size_t run = 0; run < 3; ++run) {
    solve << "{\"run\":" << run << ",\"params\":" << parameters()
          << ",\"opencv_rms_px\":\"" << hf(0.4) << "\",\"poses\":[";
    for (size_t i = 0; i < kViewCount; ++i) {
      if (i) solve << ',';
      solve << pose();
    }
    solve << "]}" << (run == 2 ? "\n" : ",\n");
  }
  solve << "],\n\"fit_params\":" << parameters() << "\n}\n";
  if (!write_text(fixture->solve, solve.str())) return false;

  std::ostringstream evidence;
  evidence << "{\n\"format\":\"L3DCAL_EVIDENCE_BUNDLE_V1\",\n"
           << "\"target\":{\"id\":\"board\",\"generator_sha256\":\""
           << generator
           << "\",\"instrument\":\"caliper\",\"resolution_mm\":\""
           << hf(0.1) << "\",\"planarity_evidence_sha256\":\"" << planarity
           << "\"},\n\"optical_sha256\":\"" << optical
           << "\",\n\"optical_state\":"
              "\"body_objective_zoom_focus_stabilization_format_pipeline\",\n"
           << "\"validation_flags\":\"0xf\",\n\"global_rmse_px\":\""
           << hf(0.4) << "\",\"maximum_residual_px\":\"" << hf(0.8)
           << "\",\"high_residual_fraction\":\"" << hf(0.0)
           << "\",\n\"holdout\":{\"rmse_px\":\"" << hf(holdout_rmse)
           << "\",\"maximum_px\":\"" << hf(0.8)
           << "\"},\n\"maximum_parameter_delta_px\":\"" << hf(0.0)
           << "\",\n\"deterministic_full_solve_equality\":true,\n"
              "\"residuals\":[";
  bool first = true;
  for (size_t i = 0; i < kViewCount; ++i) {
    const std::string sha = hex_repeat(static_cast<unsigned char>(i + 0x20));
    for (size_t corner = 0; corner < 40; ++corner) {
      if (!first) evidence << ',';
      first = false;
      evidence << "{\"source_sha256\":\"" << sha << "\",\"corner_id\":"
               << corner << ",\"dx_px\":\"" << hf(0.4)
               << "\",\"dy_px\":\"" << hf(0.0) << "\",\"rmse_px\":\""
               << hf(0.4) << "\"}";
    }
  }
  evidence << "]\n}\n";
  if (!write_text(fixture->evidence, evidence.str())) return false;

  const std::string session_sha = sha_file_hex(fixture->session);
  if (session_sha.empty()) return false;
  if (!write_text(
          fixture->producer,
          "{\n\"format\":\"L3DCAL_PRODUCER_V1\",\n"
          "\"solver_executable_sha256\":\"" + hex_repeat(0xa1) + "\",\n"
          "\"solver_configuration_sha256\":\"" + hex_repeat(0xb1) + "\",\n"
          "\"session_sha256\":\"" + session_sha + "\",\n"
          "\"opencv_version\":\"5.0.0\",\n"
          "\"opencv_build_sha256\":\"" + hex_repeat(0xc1) + "\",\n"
          "\"threads\":1,\n\"rng_seed\":1278432342,\n"
          "\"optical_sha256\":\"" + optical + "\"\n}\n"))
    return false;

  if (!write_text(
          fixture->campaign,
          "L3DCAL_CAMPAIGN_STATE_V1\nexecution " +
              std::to_string(fixture->execution_id) +
              "\noptical_configuration " + std::to_string(fixture->configuration_id) +
              "\noptical_state " + optical +
              " body_objective_zoom_focus_stabilization_format_pipeline\n"
              "capture 0 1\n"))
    return false;

  fixture->files = {fixture->session.c_str(), fixture->detection.c_str(),
                    fixture->solve.c_str(), fixture->evidence.c_str(),
                    fixture->producer.c_str(), fixture->campaign.c_str()};
  return true;
}

bool make_fixture(Fixture *fixture, double holdout_rmse = 0.4) {
  char temporary[] = "/tmp/lardon3d-workflow-ready-XXXXXX";
  char *root = mkdtemp(temporary);
  if (!root) return false;
  fixture->root = root;
  fixture->db_path = fixture->root / "project.db";
  fixture->session = fixture->root / "session";
  fixture->detection = fixture->root / "detection";
  fixture->solve = fixture->root / "solve";
  fixture->evidence = fixture->root / "evidence";
  fixture->producer = fixture->root / "producer";
  fixture->campaign = fixture->root / "campaign";

  cv::Mat image(800, 1000, CV_8UC1, cv::Scalar(127));
  if (!cv::imencode(".png", image, fixture->image_bytes)) return false;
  unsigned char image_sha[32]{};
  if (!digest(fixture->image_bytes.data(), fixture->image_bytes.size(), image_sha))
    return false;
  const std::string image_hex = hex(image_sha);
  // Binding accepts only the canonical content-addressed managed-asset layout;
  // the session view hashes are external evidence and are not Capture identity.
  fixture->image_asset_path =
      "assets/images/" + image_hex.substr(0, 2) + "/" + image_hex;
  fixture->image_path = fixture->root / fixture->image_asset_path;
  if (!std::filesystem::create_directories(fixture->image_path.parent_path()) ||
      !write_bytes(fixture->image_path, fixture->image_bytes))
    return false;

  char error[LARDON3D_PROJECT_DB_ERROR_CAPACITY]{};
  if (lardon3d_project_db_open(fixture->db_path.c_str(), &fixture->database,
                               error) != LARDON3D_PROJECT_DB_OK)
    return false;
  lardon3d_project_db_close(fixture->database);
  fixture->database = nullptr;
  if (!seed_database_rows(*fixture, image_sha)) return false;
  if (lardon3d_project_db_open(fixture->db_path.c_str(), &fixture->database,
                               error) != LARDON3D_PROJECT_DB_OK)
    return false;
  if (!create_optical_configuration(fixture)) return false;
  if (lardon3d_project_db_set_selected_capture_image(fixture->database, 1, 1) !=
      LARDON3D_PROJECT_DB_OK)
    return false;

  Lardon3DProjectDbSelectedExecutionItem item{};
  item.item_index = 0;
  item.quality_group_id = 1;
  item.campaign_group_id = 2;
  item.capture_id = 1;
  item.representation_source = LARDON3D_SELECTED_REPRESENTATION_SOURCE_IMAGE;
  Lardon3DProjectDbSelectedExecution execution{};
  if (lardon3d_project_db_create_selected_execution(
          fixture->database, 1, 2, &item, 1, 10, &execution) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  fixture->execution_id = execution.execution_id;
  if (lardon3d_project_db_record_selected_representation(
          fixture->database, fixture->execution_id, 0, 1, 1) !=
      LARDON3D_PROJECT_DB_OK)
    return false;
  return write_external_inputs(fixture, holdout_rmse);
}

bool is_calibration_without_scope(const Fixture& fixture) {
  Lardon3DProjectDbSelectedExecution execution{};
  return lardon3d_project_db_load_selected_execution(
             fixture.database, fixture.execution_id, &execution) ==
             LARDON3D_PROJECT_DB_OK &&
         execution.stage == LARDON3D_SELECTED_EXECUTION_CALIBRATION &&
         !execution.has_calibration_scope && execution.calibration_scope_id == 0;
}

Lardon3DCalibrationWorkflowResult complete(
    Fixture *fixture, Lardon3DCalibrationToolingView *views, size_t view_capacity,
    Lardon3DCalibrationToolingCoordinateCheck *checks, size_t check_capacity,
    Lardon3DCalibrationToolingEntry *entries, unsigned char *artifact,
    size_t artifact_capacity, size_t *artifact_size,
    Lardon3DCalibrationBootstrapOutput *output) {
  return lardon3d_calibration_workflow_complete(
      fixture->database, fixture->root.c_str(), &fixture->files, views,
      view_capacity, checks, check_capacity, entries, 1, artifact,
      artifact_capacity, artifact_size, output);
}

bool happy_path_and_exact_retry() {
  Fixture fixture;
  CHECK(make_fixture(&fixture));
  CHECK(is_calibration_without_scope(fixture));

  Lardon3DCalibrationToolingView views[kViewCount]{};
  Lardon3DCalibrationToolingCoordinateCheck checks[kCoordinateCount]{};
  Lardon3DCalibrationToolingEntry entries[1]{};
  unsigned char artifact[LARDON3D_CALIBRATION_BOOTSTRAP_MAX_BYTES]{};
  size_t artifact_size = 0;
  Lardon3DCalibrationBootstrapOutput output{};
  CHECK(complete(&fixture, views, kViewCount, checks, kCoordinateCount, entries,
                 artifact, sizeof(artifact), &artifact_size, &output) ==
        LARDON3D_CALIBRATION_WORKFLOW_OK);
  CHECK(artifact_size == 292 && output.calibration_count == 1 &&
        output.scope.scope_id != 0 && output.scope.member_count == 1);

  Lardon3DProjectDbSelectedExecution ready{};
  CHECK(lardon3d_project_db_load_selected_execution(
            fixture.database, fixture.execution_id, &ready) ==
        LARDON3D_PROJECT_DB_OK);
  CHECK(ready.stage == LARDON3D_SELECTED_EXECUTION_READY &&
        ready.has_calibration_scope &&
        ready.calibration_scope_id == output.scope.scope_id);

  const std::vector<unsigned char> first_artifact(
      artifact, artifact + artifact_size);
  const Lardon3DCalibrationBootstrapOutput first_output = output;
  std::memset(artifact, 0, sizeof(artifact));
  artifact_size = 0;
  std::memset(&output, 0, sizeof(output));
  CHECK(complete(&fixture, views, kViewCount, checks, kCoordinateCount, entries,
                 artifact, sizeof(artifact), &artifact_size, &output) ==
        LARDON3D_CALIBRATION_WORKFLOW_OK);
  CHECK(artifact_size == first_artifact.size() &&
        std::memcmp(artifact, first_artifact.data(), artifact_size) == 0);
  CHECK(output.scope.scope_id == first_output.scope.scope_id &&
        output.scope.member_count == first_output.scope.member_count &&
        std::memcmp(output.scope.scientific_hash,
                    first_output.scope.scientific_hash, 32) == 0 &&
        std::memcmp(output.artifact_sha256,
                    first_output.artifact_sha256, 32) == 0);

  close_fixture(&fixture);
  return true;
}

bool pre_import_failures_preserve_calibration() {
  Fixture fixture;
  CHECK(make_fixture(&fixture));
  const std::string optical = hex_repeat(0x11);
  CHECK(write_text(
      fixture.campaign,
      "L3DCAL_CAMPAIGN_STATE_V1\nexecution " +
          std::to_string(fixture.execution_id + 1) + "\noptical_configuration " +
          std::to_string(fixture.configuration_id) + "\noptical_state " + optical +
          " body_objective_zoom_focus_stabilization_format_pipeline\n"
          "capture 0 1\n"));

  Lardon3DCalibrationToolingView views[kViewCount]{};
  Lardon3DCalibrationToolingCoordinateCheck checks[kCoordinateCount]{};
  Lardon3DCalibrationToolingEntry entries[1]{};
  unsigned char artifact[292]{};
  size_t artifact_size = 99;
  Lardon3DCalibrationBootstrapOutput output{};
  CHECK(complete(&fixture, views, kViewCount, checks, kCoordinateCount, entries,
                 artifact, sizeof(artifact), &artifact_size, &output) ==
        LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);
  CHECK(artifact_size == 0 && output.scope.scope_id == 0 &&
        is_calibration_without_scope(fixture));
  close_fixture(&fixture);

  CHECK(make_fixture(&fixture));
  artifact_size = 99;
  std::memset(&output, 0x7f, sizeof(output));
  CHECK(complete(&fixture, views, kViewCount - 1, checks, kCoordinateCount,
                 entries, artifact, sizeof(artifact), &artifact_size, &output) ==
        LARDON3D_CALIBRATION_WORKFLOW_CAPACITY);
  CHECK(artifact_size == 0 && output.scope.scope_id == 0 &&
        is_calibration_without_scope(fixture));
  close_fixture(&fixture);
  return true;
}

bool tooling_science_rejection_is_not_ready() {
  Fixture fixture;
  // The external bundle is structurally and provenance-valid, but 0.8 px is
  // deliberately above Tooling's frozen hold-out RMSE acceptance threshold.
  CHECK(make_fixture(&fixture, 0.8));
  Lardon3DCalibrationToolingView views[kViewCount]{};
  Lardon3DCalibrationToolingCoordinateCheck checks[kCoordinateCount]{};
  Lardon3DCalibrationToolingEntry entries[1]{};
  unsigned char artifact[292]{};
  size_t artifact_size = 99;
  Lardon3DCalibrationBootstrapOutput output{};
  CHECK(complete(&fixture, views, kViewCount, checks, kCoordinateCount, entries,
                 artifact, sizeof(artifact), &artifact_size, &output) ==
        LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE);
  CHECK(artifact_size == 0 && output.scope.scope_id == 0 &&
        is_calibration_without_scope(fixture));
  close_fixture(&fixture);
  return true;
}

}  // namespace

int main() {
  if (!happy_path_and_exact_retry() ||
      !pre_import_failures_preserve_calibration() ||
      !tooling_science_rejection_is_not_ready())
    return 1;
  std::puts("CALIBRATION_WORKFLOW_READY_V1=PASS");
  return 0;
}
