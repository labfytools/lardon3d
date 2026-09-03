
#include <lardon3d/calibration_workflow.h>

#include <openssl/evp.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if(!(x)){ std::fprintf(stderr,"materialize failure %d: %s\n",__LINE__,#x); return false; } } while(0)

namespace {

std::string hex_repeat(char c) { return std::string(64, c); }

std::string hf(double value) {
  std::ostringstream o;
  o.imbue(std::locale::classic());
  o << std::hexfloat << value;
  return o.str();
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

bool sha_file(const std::filesystem::path& path, unsigned char out[32]) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << in.rdbuf();
  const std::string data = bytes.str();
  unsigned int n = 0;
  return in && EVP_Digest(data.data(), data.size(), out, &n, EVP_sha256(), nullptr) == 1 &&
         n == 32;
}

std::string sha_file_hex(const std::filesystem::path& path) {
  unsigned char value[32]{};
  if (!sha_file(path, value)) return {};
  static const char digits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    out[2 * i] = digits[value[i] >> 4];
    out[2 * i + 1] = digits[value[i] & 15];
  }
  return out;
}


struct Fixture {
  std::filesystem::path root, session, detection, solve, evidence, producer, campaign;
  Lardon3DCalibrationWorkflowInputFiles files{};
};

std::string coordinate_lines(const std::string& sha, double distance, unsigned band) {
  static const std::array<const char*, 9> labels = {
      "center","top","right","bottom","left","top_left","top_right","bottom_left","bottom_right"};
  std::ostringstream s;
  s << "pre_solve " << sha << " 0.1\n"
    << "clipping " << sha << " 0\n"
    << "coordinate " << sha << " qualified_decoder 1 0 1000 800 20 0 0\n";
  for (int i = 0; i < 20; ++i)
    s << "coordinate_point " << sha << " " << labels[static_cast<size_t>(i)%labels.size()]
      << " " << i << " " << i+1 << " " << i << " " << i+1 << "\n";
  s << "distance " << sha << " " << std::setprecision(17)
    << std::defaultfloat << distance << " " << band << "\n";
  return s.str();
}

std::string detection_view(const std::string& sha, unsigned region, unsigned band,
                           double distance, bool last) {
  std::ostringstream s;
  s << "{\"source_sha256\":\"" << sha
    << "\",\"orientation\":0,\"oriented_width\":1000,\"oriented_height\":800"
    << ",\"decision\":\"accepted\",\"reason\":\"-\""
    << ",\"frame_region\":" << region
    << ",\"distance_band\":" << band
    << ",\"holdout\":false"
    << ",\"target_occupancy\":\"" << hf(.3)
    << "\",\"normal_angle_degrees\":\"" << hf(25.0)
    << "\",\"measured_distance_metres\":\"" << hf(distance)
    << "\",\"pre_solve_corner_rms_px\":\"" << hf(.1)
    << "\",\"clipping_fraction\":\"" << hf(0.0)
    << "\",\"physical_target_quadrants\":3"
    << ",\"target_corner_quadrant_mask\":7"
    << ",\"corner_count\":20,\"residual_count\":20,\"high_residual_count\":0"
    << ",\"reprojection_rmse_px\":\"" << hf(.1)
    << "\",\"maximum_residual_px\":\"" << hf(.2)
    << "\",\"coordinate_equivalence\":{\"comparison_points\":20"
    << ",\"max_abs_dx_px\":\"" << hf(0.0)
    << "\",\"max_abs_dy_px\":\"" << hf(0.0)
    << "\",\"pass\":true},\"corners\":[";
  for (int i = 0; i < 20; ++i) {
    if (i) s << ",";
    s << "{\"id\":" << i << ",\"x\":\"" << hf(100.0+i)
      << "\",\"y\":\"" << hf(200.0+i) << "\"}";
  }
  s << "]}" << (last ? "\n" : " ,\n");
  return s.str();
}

std::string params(double first = 1000.0) {
  const double p[8] = {first,1001,500,400,.01,-.01,.001,-.001};
  std::ostringstream s;
  s << "[";
  for (int i = 0; i < 8; ++i) {
    if (i) s << ",";
    s << "\"" << hf(p[i]) << "\"";
  }
  s << "]";
  return s.str();
}

std::string pose() {
  return "{\"rvec\":[\"" + hf(0) + "\",\"" + hf(0) + "\",\"" + hf(0) +
         "\"],\"tvec_m\":[\"" + hf(0) + "\",\"" + hf(0) + "\",\"" + hf(1) + "\"]}";
}

std::string solve_text(bool mismatch) {
  std::ostringstream s;
  s << "{\n\"format\":\"L3DCAL_SOLVE_V1\",\n\"runs\":[\n";
  for (int run = 0; run < 3; ++run) {
    s << "{\"run\":" << run << ",\"params\":" << params(mismatch && run==1 ? 999.0 : 1000.0)
      << ",\"opencv_rms_px\":\"" << hf(.1) << "\",\"poses\":["
      << pose() << "," << pose() << "]}" << (run==2 ? "\n" : " ,\n");
  }
  s << "],\n\"fit_params\":" << params() << "\n}\n";
  return s.str();
}

std::string evidence_text(const std::string& optical,
                          const std::string& generator,
                          const std::string& planarity,
                          const std::string& a,
                          const std::string& b) {
  std::ostringstream s;
  s << "{\n\"format\":\"L3DCAL_EVIDENCE_BUNDLE_V1\",\n"
    << "\"target\":{\"id\":\"board\",\"generator_sha256\":\"" << generator
    << "\",\"instrument\":\"caliper\",\"resolution_mm\":\"" << hf(.1)
    << "\",\"planarity_evidence_sha256\":\"" << planarity << "\"},\n"
    << "\"optical_sha256\":\"" << optical << "\",\n"
    << "\"optical_state\":\"body_objective_zoom_focus_stabilization_format_pipeline\",\n"
    << "\"validation_flags\":\"0xf\",\n"
    << "\"global_rmse_px\":\"" << hf(.1)
    << "\",\"maximum_residual_px\":\"" << hf(.2)
    << "\",\"high_residual_fraction\":\"" << hf(0.0) << "\",\n"
    << "\"holdout\":{\"rmse_px\":\"" << hf(.1)
    << "\",\"maximum_px\":\"" << hf(.2) << "\"},\n"
    << "\"maximum_parameter_delta_px\":\"" << hf(0.0) << "\",\n"
    << "\"deterministic_full_solve_equality\":true,\n\"residuals\":[";
  bool first = true;
  for (const std::string* sha : {&a, &b}) {
    for (int i = 0; i < 20; ++i) {
      if (!first) s << ",";
      first = false;
      s << "{\"source_sha256\":\"" << *sha << "\",\"corner_id\":" << i
        << ",\"dx_px\":\"" << hf(.1) << "\",\"dy_px\":\"" << hf(0.0)
        << "\",\"rmse_px\":\"" << hf(.1) << "\"}";
    }
  }
  s << "]\n}\n";
  return s.str();
}

bool make_fixture(Fixture *f, bool mismatch_solve = false) {
  char temp[]="/tmp/lardon3d-materialize-XXXXXX";
  char *r=mkdtemp(temp); if(!r) return false;
  f->root=r; f->session=f->root/"session"; f->detection=f->root/"detection";
  f->solve=f->root/"solve"; f->evidence=f->root/"evidence";
  f->producer=f->root/"producer"; f->campaign=f->root/"campaign";
  const std::string optical=hex_repeat('1'), generator=hex_repeat('2'),
                    planarity=hex_repeat('3'), a=hex_repeat('4'), b=hex_repeat('5');
  std::ostringstream session;
  session << "L3DCAL_SESSION_V1\n"
          << "target board " << generator << " DICT_5X5_100 9 7 30 21\n"
          << "measurement caliper 0.1 30 30 30 30 30 30 30 30 30 30\n"
          << "white_border 30\n"
          << "planarity PASS " << planarity << "\n"
          << "decoder qualified_decoder 1\n"
          << "optical_state " << optical << " body_objective_zoom_focus_stabilization_format_pipeline\n"
          << "image /a " << a << " 0\n"
          << "image /b " << b << " 0\n"
          << coordinate_lines(a,.3,0) << coordinate_lines(b,.5,1);
  if(!write_text(f->session,session.str())) return false;
  if(!write_text(f->detection,
      "{\n\"format\":\"L3DCAL_DETECTION_V1\",\n\"decoder\":\"qualified_decoder\",\n"
      "\"decoder_version\":\"1\",\n\"views\":[\n" +
      detection_view(a,0,0,.3,false)+detection_view(b,1,1,.5,true)+"]\n}\n")) return false;
  if(!write_text(f->solve,solve_text(mismatch_solve))) return false;
  if(!write_text(f->evidence,evidence_text(optical,generator,planarity,a,b))) return false;
  const std::string session_sha = sha_file_hex(f->session);
  if (session_sha.empty()) return false;
  if(!write_text(f->producer,
      "{\n\"format\":\"L3DCAL_PRODUCER_V1\",\n"
      "\"solver_executable_sha256\":\"" + hex_repeat('a') + "\",\n"
      "\"solver_configuration_sha256\":\"" + hex_repeat('b') + "\",\n"
      "\"session_sha256\":\"" + session_sha + "\",\n"
      "\"opencv_version\":\"5.0.0\",\n"
      "\"opencv_build_sha256\":\"" + hex_repeat('c') + "\",\n"
      "\"threads\":1,\n\"rng_seed\":1278432342,\n"
      "\"optical_sha256\":\"" + optical + "\"\n}\n")) return false;
  if(!write_text(f->campaign,
      "L3DCAL_CAMPAIGN_STATE_V1\n"
      "execution 42\n"
      "optical_configuration 7\n"
      "optical_state " + optical +
      " body_objective_zoom_focus_stabilization_format_pipeline\n"
      "capture 0 101\ncapture 1 102\n")) return false;

  f->files={f->session.c_str(),f->detection.c_str(),f->solve.c_str(),
            f->evidence.c_str(),f->producer.c_str(),f->campaign.c_str()};
  return true;
}

bool valid() {
  Fixture f; CHECK(make_fixture(&f));
  Lardon3DCalibrationToolingView views[2]{};
  Lardon3DCalibrationToolingCoordinateCheck checks[40]{};
  Lardon3DCalibrationWorkflowExternalEvidence out{};
  CHECK(lardon3d_calibration_workflow_materialize_external_evidence(
      &f.files,views,2,checks,40,&out)==LARDON3D_CALIBRATION_WORKFLOW_OK);
  CHECK(out.view_count==2 && out.coordinate_check_count==40);
  CHECK(out.support_images==2 && out.support_observations==40);
  CHECK(out.validation_flags==15 && out.target_white_border_mm==30.0);
  CHECK(out.repeated_parameters[0][0]==1000.0 && out.fit_parameters[0]==1000.0);
  CHECK(views[0].accepted==1 && views[0].corner_count==20 &&
        views[1].distance_band==1);
  CHECK(checks[0].dx_px==0.0 && checks[0].dy_px==0.0);
  unsigned char session_sha[32]{};
  CHECK(sha_file(f.session, session_sha));
  CHECK(std::memcmp(out.initialization_evidence_sha256, session_sha, 32) == 0);
  std::filesystem::remove_all(f.root);
  return true;
}

bool capacity_and_mismatch() {
  Fixture f; CHECK(make_fixture(&f));
  Lardon3DCalibrationToolingView views[2]{};
  Lardon3DCalibrationToolingCoordinateCheck checks[40]{};
  Lardon3DCalibrationWorkflowExternalEvidence out{};
  CHECK(lardon3d_calibration_workflow_materialize_external_evidence(
      &f.files,views,1,checks,40,&out)==LARDON3D_CALIBRATION_WORKFLOW_CAPACITY);
  std::filesystem::remove_all(f.root);

  CHECK(make_fixture(&f,true));
  CHECK(lardon3d_calibration_workflow_materialize_external_evidence(
      &f.files,views,2,checks,40,&out)==LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH);
  std::filesystem::remove_all(f.root);
  return true;
}

} // namespace

int main(){
  if(!valid()||!capacity_and_mismatch()) return 1;
  std::puts("CALIBRATION_WORKFLOW_EVIDENCE_MATERIALIZATION_V1=PASS");
  return 0;
}
