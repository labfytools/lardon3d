#include <lardon3d/calibration_workflow.h>

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "workflow failure %d: %s\n", __LINE__, #x); return false; } } while (0)

namespace {

std::string hex_repeat(char c) { return std::string(64, c); }

bool write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream bytes; bytes << in.rdbuf();
  std::string data = bytes.str();
  unsigned char digest[32]{}; unsigned int size = 0;
  if (!in || EVP_Digest(data.data(), data.size(), digest, &size, EVP_sha256(), nullptr) != 1 || size != 32) return {};
  static const char digits[] = "0123456789abcdef";
  std::string hex(64, '0');
  for (size_t i = 0; i < 32; ++i) { hex[2*i] = digits[digest[i] >> 4]; hex[2*i+1] = digits[digest[i] & 15]; }
  return hex;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path session, detection, solve, evidence, producer, campaign;
  Lardon3DCalibrationWorkflowInputFiles files{};
};

std::string session_text(const std::string& optical) {
  const std::string generator = hex_repeat('2'), planarity = hex_repeat('3'), image = hex_repeat('4');
  std::ostringstream s;
  s << "L3DCAL_SESSION_V1\n"
    << "target board " << generator << " DICT_5X5_100 9 7 30 21\n"
    << "measurement caliper 0.1 30 30 30 30 30 30 30 30 30 30\n"
    << "white_border 30\n"
    << "planarity PASS " << planarity << "\n"
    << "decoder qualified_decoder 1\n"
    << "optical_state " << optical << " body_objective_zoom_focus_stabilization_format_pipeline\n"
    << "image /nonexistent " << image << " 0\n"
    << "pre_solve " << image << " 0.1\n"
    << "clipping " << image << " 0.0\n"
    << "coordinate " << image << " qualified_decoder 1 0 100 100 20 0 0\n";
  for (int i = 0; i < 20; ++i)
    s << "coordinate_point " << image << " center " << i << " 0 " << i << " 0\n";
  s << "distance " << image << " 0.4 1\n";
  return s.str();
}

bool make_valid(Fixture *f) {
  char temp[] = "/tmp/lardon3d-calibration-workflow-XXXXXX";
  char *root = mkdtemp(temp); if (!root) return false;
  f->root = root;
  f->session = f->root / "session.l3dcal";
  f->detection = f->root / "detection.json";
  f->solve = f->root / "solve.json";
  f->evidence = f->root / "evidence.json";
  f->producer = f->root / "producer.json";
  f->campaign = f->root / "campaign.l3dcal";
  const std::string optical = hex_repeat('1');
  if (!write_text(f->session, session_text(optical))) return false;
  const std::string session_sha = sha256_file(f->session); if (session_sha.empty()) return false;
  if (!write_text(f->detection,
      "{\n\"format\":\"L3DCAL_DETECTION_V1\",\n\"decoder\":\"qualified_decoder\",\n\"decoder_version\":\"1\",\n\"views\":[]\n}\n")) return false;
  if (!write_text(f->solve, "{\n\"format\":\"L3DCAL_SOLVE_V1\",\n\"runs\":[],\n\"fit_params\":[]\n}\n")) return false;
  if (!write_text(f->evidence,
      "{\n\"format\":\"L3DCAL_EVIDENCE_BUNDLE_V1\",\n\"optical_sha256\":\"" + optical +
      "\",\n\"optical_state\":\"body_objective_zoom_focus_stabilization_format_pipeline\",\n\"validation_flags\":\"0xf\",\n\"residuals\":[]\n}\n")) return false;
  if (!write_text(f->producer,
      "{\n\"format\":\"L3DCAL_PRODUCER_V1\",\n\"solver_executable_sha256\":\"" + hex_repeat('a') +
      "\",\n\"solver_configuration_sha256\":\"" + hex_repeat('b') +
      "\",\n\"session_sha256\":\"" + session_sha +
      "\",\n\"opencv_version\":\"5.0.0\",\n\"opencv_build_sha256\":\"" + hex_repeat('c') +
      "\",\n\"threads\":1,\n\"rng_seed\":1278432342,\n\"optical_sha256\":\"" + optical + "\"\n}\n")) return false;
  if (!write_text(f->campaign,
      "L3DCAL_CAMPAIGN_STATE_V1\nexecution 42\noptical_configuration 7\noptical_state " + optical +
      " body_objective_zoom_focus_stabilization_format_pipeline\ncapture 0 101\ncapture 1 102\n")) return false;
  f->files = {f->session.c_str(), f->detection.c_str(), f->solve.c_str(), f->evidence.c_str(), f->producer.c_str(), f->campaign.c_str()};
  return true;
}

bool valid_and_identity() {
  Fixture f; CHECK(make_valid(&f));
  Lardon3DCalibrationWorkflowInputBoundary boundary{};
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_OK);
  CHECK(boundary.selected_execution_id == 42 && boundary.optical_configuration_id == 7 && boundary.capture_count == 2);
  CHECK(boundary.capture_ids[0] == 101 && boundary.capture_ids[1] == 102);
  CHECK(std::strcmp(boundary.optical_state_token, "body_objective_zoom_focus_stabilization_format_pipeline") == 0);
  std::filesystem::remove_all(f.root); return true;
}

bool malformed_and_provenance() {
  Fixture f; CHECK(make_valid(&f));
  Lardon3DCalibrationWorkflowInputBoundary boundary{};
  CHECK(write_text(f.solve, "{\"format\":\"L3DCAL_SOLVE_V1\",}"));
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE);
  CHECK(boundary.selected_execution_id == 0);
  CHECK(write_text(f.solve, "{\"format\":\"L3DCAL_SOLVE_V1\",\"runs\":[]}"));
  std::string producer; { std::ifstream in(f.producer); std::ostringstream x; x << in.rdbuf(); producer = x.str(); }
  const std::string actual = sha256_file(f.session); CHECK(!actual.empty());
  const size_t at = producer.find(actual); CHECK(at != std::string::npos); producer.replace(at, 64, hex_repeat('0'));
  CHECK(write_text(f.producer, producer));
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);
  std::filesystem::remove_all(f.root);
  CHECK(make_valid(&f));
  std::string campaign; { std::ifstream in(f.campaign); std::ostringstream x; x << in.rdbuf(); campaign = x.str(); }
  const size_t opt = campaign.find(hex_repeat('1')); CHECK(opt != std::string::npos); campaign.replace(opt, 64, hex_repeat('9'));
  CHECK(write_text(f.campaign, campaign));
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);
  std::filesystem::remove_all(f.root); return true;
}

bool file_boundary() {
  Fixture f; CHECK(make_valid(&f));
  Lardon3DCalibrationWorkflowInputBoundary boundary{};
  const std::filesystem::path fifo = f.root / "fifo";
  CHECK(mkfifo(fifo.c_str(), 0600) == 0);
  auto saved = f.files.producer_path; f.files.producer_path = fifo.c_str();
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE);
  f.files.producer_path = saved;
  const std::filesystem::path link = f.root / "producer-link";
  CHECK(symlink(f.producer.c_str(), link.c_str()) == 0); saved = f.files.producer_path; f.files.producer_path = link.c_str();
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE);
  f.files.producer_path = saved;
  const std::filesystem::path huge = f.root / "huge";
  int fd = open(huge.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600); CHECK(fd >= 0);
  CHECK(ftruncate(fd, static_cast<off_t>(LARDON3D_CALIBRATION_WORKFLOW_MAX_FILE_BYTES) + 1) == 0); CHECK(close(fd) == 0);
  saved = f.files.producer_path; f.files.producer_path = huge.c_str();
  CHECK(lardon3d_calibration_workflow_validate_input_boundary(&f.files, &boundary) == LARDON3D_CALIBRATION_WORKFLOW_CAPACITY);
  f.files.producer_path = saved;
  std::filesystem::remove_all(f.root); return true;
}

} // namespace

int main() {
  if (!valid_and_identity() || !malformed_and_provenance() || !file_boundary()) return 1;
  std::puts("CALIBRATION_WORKFLOW_INPUT_BOUNDARY_V1=PASS");
  return 0;
}
