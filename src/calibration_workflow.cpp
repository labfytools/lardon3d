#include <lardon3d/calibration_workflow.h>

#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <locale>
#include <map>
#include <openssl/evp.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kSha256Bytes = 32;
constexpr size_t kJsonDepthMax = 64;
constexpr size_t kJsonKeyMax = 128;

struct MappedFile {
  int fd = -1;
  const unsigned char *data = nullptr;
  size_t size = 0;
  unsigned char sha256[kSha256Bytes]{};

  ~MappedFile() {
    if (data && size) munmap(const_cast<unsigned char *>(data), size);
    if (fd >= 0) close(fd);
  }
  MappedFile() = default;
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;
};

bool digest_bytes(const unsigned char *data, size_t size, unsigned char output[32]) {
  unsigned int length = 0;
  return EVP_Digest(data, size, output, &length, EVP_sha256(), nullptr) == 1 && length == 32;
}

Lardon3DCalibrationWorkflowResult map_regular_file(const char *path, MappedFile *out) {
  if (!path || !*path || !out) return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ELOOP) return LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE;
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  }
  struct stat st{};
  if (fstat(fd, &st) != 0) {
    close(fd);
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    return LARDON3D_CALIBRATION_WORKFLOW_NON_REGULAR_FILE;
  }
  if (st.st_size <= 0) {
    close(fd);
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;
  }
  if (static_cast<uint64_t>(st.st_size) > LARDON3D_CALIBRATION_WORKFLOW_MAX_FILE_BYTES) {
    close(fd);
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
  }
  const size_t size = static_cast<size_t>(st.st_size);
  void *mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) {
    close(fd);
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  }
  out->fd = fd;
  out->data = static_cast<const unsigned char *>(mapped);
  out->size = size;
  if (!digest_bytes(out->data, out->size, out->sha256))
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  return LARDON3D_CALIBRATION_WORKFLOW_OK;
}

bool hex_digit(char c, unsigned *value) {
  if (c >= '0' && c <= '9') *value = static_cast<unsigned>(c - '0');
  else if (c >= 'a' && c <= 'f') *value = static_cast<unsigned>(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F') *value = static_cast<unsigned>(c - 'A' + 10);
  else return false;
  return true;
}

bool parse_sha256(std::string_view text, unsigned char output[32]) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    unsigned hi = 0, lo = 0;
    if (!hex_digit(text[2 * i], &hi) || !hex_digit(text[2 * i + 1], &lo)) return false;
    output[i] = static_cast<unsigned char>((hi << 4u) | lo);
  }
  return true;
}

bool nonzero_sha(const unsigned char value[32]) {
  unsigned char any = 0;
  for (size_t i = 0; i < 32; ++i) any |= value[i];
  return any != 0;
}

bool token_ok(std::string_view value) {
  if (value.empty() || value.size() >= LARDON3D_CALIBRATION_WORKFLOW_OPTICAL_STATE_TOKEN_CAPACITY)
    return false;
  for (unsigned char c : value) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':')) return false;
  }
  return true;
}

bool parse_u64_any(std::string_view text, uint64_t *value) {
  if (!value || text.empty()) return false;
  uint64_t parsed = 0;
  auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  *value = parsed;
  return true;
}

bool parse_u64_positive(std::string_view text, uint64_t *value) {
  return parse_u64_any(text, value) && *value != 0;
}

bool parse_u32_any(std::string_view text, uint32_t *value) {
  uint64_t parsed = 0;
  if (!parse_u64_any(text, &parsed) || parsed > UINT32_MAX) return false;
  *value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_finite_double(std::string_view text, double *value) {
  if (!value || text.empty() || text.size() > 128) return false;
  std::istringstream stream{std::string(text)};
  stream.imbue(std::locale::classic());
  double parsed = 0.0;
  stream >> parsed;
  if (!stream || !stream.eof() || !std::isfinite(parsed)) return false;
  *value = parsed;
  return true;
}

std::vector<std::string_view> words(std::string_view line) {
  std::vector<std::string_view> out;
  size_t at = 0;
  while (at < line.size()) {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    if (at == line.size()) break;
    size_t end = at;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    out.push_back(line.substr(at, end - at));
    at = end;
  }
  return out;
}

struct SessionIdentity {
  std::string decoder;
  std::string decoder_version;
  std::string optical_state_token;
  unsigned char optical_state_sha256[32]{};
};

bool parse_session(std::string_view text, SessionIdentity *identity) {
  if (!identity || text.empty() || text.back() != '\n' || text.find('\0') != std::string_view::npos ||
      text.find('\r') != std::string_view::npos)
    return false;
  size_t at = 0;
  size_t line_number = 0;
  bool target = false, measurement = false, white_border = false, planarity = false;
  bool decoder = false, optical_state = false, image = false;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) return false;
    std::string_view line = text.substr(at, end - at);
    at = end + 1;
    ++line_number;
    if (line_number == 1) {
      if (line != "L3DCAL_SESSION_V1") return false;
      continue;
    }
    if (line.empty()) continue;
    auto w = words(line);
    if (w.empty()) return false;
    const auto tag = w[0];
    if (tag == "target") {
      uint32_t squares_x = 0, squares_y = 0; double square = 0, marker = 0;
      if (target || w.size() != 8 || !token_ok(w[1]) || w[3] != "DICT_5X5_100" ||
          !parse_u32_any(w[4], &squares_x) || !parse_u32_any(w[5], &squares_y) ||
          !parse_finite_double(w[6], &square) || !parse_finite_double(w[7], &marker) ||
          squares_x != 9 || squares_y != 7 || square != 30.0 || marker != 21.0) return false;
      unsigned char hash[32]; if (!parse_sha256(w[2], hash)) return false; target = true;
    } else if (tag == "measurement") {
      if (measurement || w.size() != 13 || !token_ok(w[1])) return false;
      double v = 0; if (!parse_finite_double(w[2], &v) || v <= 0 || v > .1) return false;
      for (size_t i = 3; i < w.size(); ++i) if (!parse_finite_double(w[i], &v)) return false;
      measurement = true;
    } else if (tag == "white_border") {
      double v = 0; if (white_border || w.size() != 2 || !parse_finite_double(w[1], &v) || v < 30.0) return false;
      white_border = true;
    } else if (tag == "planarity") {
      unsigned char hash[32];
      if (planarity || w.size() != 3 || w[1] != "PASS" || !parse_sha256(w[2], hash)) return false;
      planarity = true;
    } else if (tag == "decoder") {
      if (decoder || w.size() != 3 || !token_ok(w[1]) || !token_ok(w[2])) return false;
      identity->decoder.assign(w[1]); identity->decoder_version.assign(w[2]); decoder = true;
    } else if (tag == "optical_state") {
      if (optical_state || w.size() != 3 || !parse_sha256(w[1], identity->optical_state_sha256) ||
          !token_ok(w[2]) || w[2] == "UNKNOWN") return false;
      identity->optical_state_token.assign(w[2]); optical_state = true;
    } else if (tag == "image") {
      unsigned char hash[32]; uint32_t orientation = 0;
      if (w.size() != 4 || w[1].empty() || w[1].size() > 4096 || !parse_sha256(w[2], hash) ||
          !parse_u32_any(w[3], &orientation) ||
          (orientation != 90 && orientation != 180 && orientation != 270 && w[3] != "0")) return false;
      image = true;
    } else if (tag == "pre_solve" || tag == "clipping") {
      unsigned char hash[32]; double v = 0;
      if (w.size() != 3 || !parse_sha256(w[1], hash) || !parse_finite_double(w[2], &v)) return false;
    } else if (tag == "coordinate") {
      unsigned char hash[32]; uint32_t orientation = 0, width = 0, height = 0, count = 0; double dx = 0, dy = 0;
      if (w.size() != 10 || !parse_sha256(w[1], hash) || !token_ok(w[2]) || !token_ok(w[3]) ||
          !parse_u32_any(w[4], &orientation) || !parse_u32_any(w[5], &width) || !parse_u32_any(w[6], &height) ||
          !parse_u32_any(w[7], &count) || !parse_finite_double(w[8], &dx) || !parse_finite_double(w[9], &dy) ||
          width == 0 || height == 0 || count < 20 || count > 48 ||
          (orientation != 90 && orientation != 180 && orientation != 270 && w[4] != "0")) return false;
    } else if (tag == "coordinate_point") {
      unsigned char hash[32]; double a = 0, b = 0, c = 0, d = 0;
      if (w.size() != 7 || !parse_sha256(w[1], hash) || !token_ok(w[2]) ||
          !parse_finite_double(w[3], &a) || !parse_finite_double(w[4], &b) ||
          !parse_finite_double(w[5], &c) || !parse_finite_double(w[6], &d)) return false;
    } else if (tag == "distance") {
      unsigned char hash[32]; double meters = 0; uint32_t band = 0;
      if (w.size() != 4 || !parse_sha256(w[1], hash) || !parse_finite_double(w[2], &meters) ||
          !parse_u32_any(w[3], &band) || meters <= 0 || band > 2) return false;
    } else {
      return false;
    }
  }
  return target && measurement && white_border && planarity && decoder && optical_state && image;
}

struct CampaignState {
  uint64_t execution_id = 0;
  uint64_t optical_configuration_id = 0;
  std::string optical_state_token;
  unsigned char optical_state_sha256[32]{};
  std::vector<uint64_t> captures;
};

bool parse_campaign_state(std::string_view text, CampaignState *state) {
  if (!state || text.empty() || text.back() != '\n' || text.find('\0') != std::string_view::npos ||
      text.find('\r') != std::string_view::npos) return false;
  std::vector<std::string_view> lines;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at); if (end == std::string_view::npos) return false;
    if (end > at) lines.push_back(text.substr(at, end - at));
    at = end + 1;
  }
  if (lines.size() < 5 || lines[0] != "L3DCAL_CAMPAIGN_STATE_V1") return false;
  auto execution = words(lines[1]);
  auto configuration = words(lines[2]);
  auto optical = words(lines[3]);
  if (execution.size() != 2 || execution[0] != "execution" || !parse_u64_positive(execution[1], &state->execution_id) ||
      configuration.size() != 2 || configuration[0] != "optical_configuration" ||
      !parse_u64_positive(configuration[1], &state->optical_configuration_id) || optical.size() != 3 ||
      optical[0] != "optical_state" || !parse_sha256(optical[1], state->optical_state_sha256) ||
      !token_ok(optical[2]) || optical[2] == "UNKNOWN") return false;
  state->optical_state_token.assign(optical[2]);
  for (size_t i = 4; i < lines.size(); ++i) {
    auto capture = words(lines[i]);
    uint64_t index = 0, capture_id = 0;
    if (capture.size() != 3 || capture[0] != "capture" || !parse_u64_any(capture[1], &index) ||
        !parse_u64_positive(capture[2], &capture_id) || index != state->captures.size()) return false;
    if (state->captures.size() >= LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS) return false;
    state->captures.push_back(capture_id);
  }
  return !state->captures.empty();
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}
  bool parse(std::map<std::string, std::string> *top_scalars) {
    top_scalars_ = top_scalars;
    skip_ws();
    if (!parse_object(0, true)) return false;
    skip_ws();
    return at_ == text_.size();
  }
 private:
  bool parse_object(size_t depth, bool top) {
    if (depth > kJsonDepthMax || !take('{')) return false;
    skip_ws(); if (take('}')) return true;
    std::set<std::string> keys;
    while (true) {
      std::string key;
      if (!parse_string(&key, true) || key.size() > kJsonKeyMax || !keys.insert(key).second) return false;
      skip_ws(); if (!take(':')) return false; skip_ws();
      if (!parse_value(depth + 1, top ? &key : nullptr)) return false;
      skip_ws(); if (take('}')) return true; if (!take(',')) return false; skip_ws();
    }
  }
  bool parse_array(size_t depth) {
    if (depth > kJsonDepthMax || !take('[')) return false;
    skip_ws(); if (take(']')) return true;
    while (true) {
      if (!parse_value(depth + 1, nullptr)) return false;
      skip_ws(); if (take(']')) return true; if (!take(',')) return false; skip_ws();
    }
  }
  bool parse_value(size_t depth, const std::string *top_key) {
    if (depth > kJsonDepthMax || at_ >= text_.size()) return false;
    if (text_[at_] == '{') return parse_object(depth, false);
    if (text_[at_] == '[') return parse_array(depth);
    if (text_[at_] == '"') {
      std::string value;
      if (!parse_string(top_key ? &value : nullptr, top_key != nullptr)) return false;
      if (top_key) (*top_scalars_)[*top_key] = value;
      return true;
    }
    size_t start = at_;
    if (parse_literal("true") || parse_literal("false") || parse_literal("null") || parse_number()) {
      if (top_key) (*top_scalars_)[*top_key] = std::string(text_.substr(start, at_ - start));
      return true;
    }
    return false;
  }
  bool parse_string(std::string *out, bool capture) {
    if (!take('"')) return false;
    if (capture && out) out->clear();
    while (at_ < text_.size()) {
      unsigned char c = static_cast<unsigned char>(text_[at_++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c == '\\') {
        if (at_ >= text_.size()) return false;
        char e = text_[at_++];
        if (e == 'u') {
          if (at_ + 4 > text_.size()) return false;
          for (size_t i = 0; i < 4; ++i) { unsigned v = 0; if (!hex_digit(text_[at_ + i], &v)) return false; }
          at_ += 4;
        } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) return false;
        if (capture) return false;
      } else if (capture && out) {
        out->push_back(static_cast<char>(c));
      }
    }
    return false;
  }
  bool parse_number() {
    size_t p = at_;
    if (p < text_.size() && text_[p] == '-') ++p;
    if (p >= text_.size()) return false;
    if (text_[p] == '0') ++p;
    else {
      if (text_[p] < '1' || text_[p] > '9') return false;
      while (p < text_.size() && std::isdigit(static_cast<unsigned char>(text_[p]))) ++p;
    }
    if (p < text_.size() && text_[p] == '.') {
      ++p; size_t digits = p; while (p < text_.size() && std::isdigit(static_cast<unsigned char>(text_[p]))) ++p;
      if (p == digits) return false;
    }
    if (p < text_.size() && (text_[p] == 'e' || text_[p] == 'E')) {
      ++p; if (p < text_.size() && (text_[p] == '+' || text_[p] == '-')) ++p;
      size_t digits = p; while (p < text_.size() && std::isdigit(static_cast<unsigned char>(text_[p]))) ++p;
      if (p == digits) return false;
    }
    at_ = p; return true;
  }
  bool parse_literal(std::string_view literal) {
    if (text_.substr(at_, literal.size()) != literal) return false;
    at_ += literal.size(); return true;
  }
  void skip_ws() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) ++at_; }
  bool take(char c) { if (at_ >= text_.size() || text_[at_] != c) return false; ++at_; return true; }
  std::string_view text_;
  size_t at_ = 0;
  std::map<std::string, std::string> *top_scalars_ = nullptr;
};

bool scalar(const std::map<std::string, std::string>& values, const char *key, std::string *out) {
  auto it = values.find(key); if (it == values.end()) return false; *out = it->second; return true;
}

bool parse_json_top(std::string_view text, std::map<std::string, std::string> *top) {
  if (text.empty() || text.find('\0') != std::string_view::npos) return false;
  JsonParser parser(text); return parser.parse(top);
}

bool require_format(const std::map<std::string, std::string>& top, const char *expected) {
  auto it = top.find("format"); return it != top.end() && it->second == expected;
}

}  // namespace

extern "C" Lardon3DCalibrationWorkflowResult lardon3d_calibration_workflow_validate_input_boundary(
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationWorkflowInputBoundary *boundary) {
  if (!files || !boundary || !files->session_path || !files->detection_path || !files->solve_path ||
      !files->evidence_path || !files->producer_path || !files->campaign_state_path)
    return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;
  std::memset(boundary, 0, sizeof(*boundary));

  MappedFile session, detection, solve, evidence, producer, campaign;
  MappedFile *mapped[] = {&session, &detection, &solve, &evidence, &producer, &campaign};
  const char *paths[] = {files->session_path, files->detection_path, files->solve_path,
                         files->evidence_path, files->producer_path, files->campaign_state_path};
  for (size_t i = 0; i < 6; ++i) {
    Lardon3DCalibrationWorkflowResult r = map_regular_file(paths[i], mapped[i]);
    if (r != LARDON3D_CALIBRATION_WORKFLOW_OK) return r;
  }

  SessionIdentity session_identity;
  if (!parse_session(std::string_view(reinterpret_cast<const char *>(session.data), session.size), &session_identity))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;
  CampaignState campaign_state;
  if (!parse_campaign_state(std::string_view(reinterpret_cast<const char *>(campaign.data), campaign.size), &campaign_state))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;

  std::map<std::string, std::string> detection_top, solve_top, evidence_top, producer_top;
  if (!parse_json_top(std::string_view(reinterpret_cast<const char *>(detection.data), detection.size), &detection_top) ||
      !parse_json_top(std::string_view(reinterpret_cast<const char *>(solve.data), solve.size), &solve_top) ||
      !parse_json_top(std::string_view(reinterpret_cast<const char *>(evidence.data), evidence.size), &evidence_top) ||
      !parse_json_top(std::string_view(reinterpret_cast<const char *>(producer.data), producer.size), &producer_top))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;
  if (!require_format(detection_top, "L3DCAL_DETECTION_V1") || !require_format(solve_top, "L3DCAL_SOLVE_V1") ||
      !require_format(evidence_top, "L3DCAL_EVIDENCE_BUNDLE_V1") || !require_format(producer_top, "L3DCAL_PRODUCER_V1"))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;

  std::string detection_decoder, detection_version, evidence_optical, evidence_state;
  std::string producer_executable, producer_configuration, producer_session, producer_optical, producer_threads;
  if (!scalar(detection_top, "decoder", &detection_decoder) || !scalar(detection_top, "decoder_version", &detection_version) ||
      !scalar(evidence_top, "optical_sha256", &evidence_optical) || !scalar(evidence_top, "optical_state", &evidence_state) ||
      !scalar(producer_top, "solver_executable_sha256", &producer_executable) ||
      !scalar(producer_top, "solver_configuration_sha256", &producer_configuration) ||
      !scalar(producer_top, "session_sha256", &producer_session) || !scalar(producer_top, "optical_sha256", &producer_optical) ||
      !scalar(producer_top, "threads", &producer_threads))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;

  unsigned char evidence_optical_sha[32], producer_optical_sha[32], producer_session_sha[32];
  unsigned char executable_sha[32], configuration_sha[32];
  if (!parse_sha256(evidence_optical, evidence_optical_sha) || !parse_sha256(producer_optical, producer_optical_sha) ||
      !parse_sha256(producer_session, producer_session_sha) || !parse_sha256(producer_executable, executable_sha) ||
      !parse_sha256(producer_configuration, configuration_sha) || producer_threads != "1" ||
      !nonzero_sha(executable_sha) || !nonzero_sha(configuration_sha))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;

  if (detection_decoder != session_identity.decoder || detection_version != session_identity.decoder_version ||
      std::memcmp(evidence_optical_sha, session_identity.optical_state_sha256, 32) != 0 ||
      evidence_state != session_identity.optical_state_token ||
      std::memcmp(producer_optical_sha, session_identity.optical_state_sha256, 32) != 0 ||
      std::memcmp(producer_session_sha, session.sha256, 32) != 0 ||
      std::memcmp(campaign_state.optical_state_sha256, session_identity.optical_state_sha256, 32) != 0 ||
      campaign_state.optical_state_token != session_identity.optical_state_token)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  std::memcpy(boundary->session_sha256, session.sha256, 32);
  std::memcpy(boundary->detection_sha256, detection.sha256, 32);
  std::memcpy(boundary->solve_sha256, solve.sha256, 32);
  std::memcpy(boundary->evidence_sha256, evidence.sha256, 32);
  std::memcpy(boundary->producer_sha256, producer.sha256, 32);
  std::memcpy(boundary->campaign_state_sha256, campaign.sha256, 32);
  std::memcpy(boundary->optical_state_sha256, session_identity.optical_state_sha256, 32);
  std::memcpy(boundary->solver_executable_sha256, executable_sha, 32);
  std::memcpy(boundary->solver_configuration_sha256, configuration_sha, 32);
  boundary->selected_execution_id = campaign_state.execution_id;
  boundary->optical_configuration_id = campaign_state.optical_configuration_id;
  boundary->capture_count = static_cast<uint32_t>(campaign_state.captures.size());
  for (size_t i = 0; i < campaign_state.captures.size(); ++i) boundary->capture_ids[i] = campaign_state.captures[i];
  std::memcpy(boundary->optical_state_token, session_identity.optical_state_token.data(), session_identity.optical_state_token.size());
  boundary->optical_state_token[session_identity.optical_state_token.size()] = '\0';
  return LARDON3D_CALIBRATION_WORKFLOW_OK;
}
