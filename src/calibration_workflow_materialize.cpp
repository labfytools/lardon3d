
#include <lardon3d/calibration_workflow.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <locale.h>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr size_t kShaBytes = 32;
constexpr size_t kMaxObservations =
    LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS * 48u;

struct ShaLess {
  bool operator()(const std::array<unsigned char, 32>& a,
                  const std::array<unsigned char, 32>& b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
  }
};


bool hex_digit(char c, unsigned *value) {
  if (c >= '0' && c <= '9') *value = static_cast<unsigned>(c - '0');
  else if (c >= 'a' && c <= 'f') *value = static_cast<unsigned>(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F') *value = static_cast<unsigned>(c - 'A' + 10);
  else return false;
  return true;
}

bool parse_sha(std::string_view text, unsigned char output[32]) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    unsigned hi = 0, lo = 0;
    if (!hex_digit(text[2 * i], &hi) || !hex_digit(text[2 * i + 1], &lo))
      return false;
    output[i] = static_cast<unsigned char>((hi << 4u) | lo);
  }
  return true;
}

std::array<unsigned char, 32> sha_array(const unsigned char value[32]) {
  std::array<unsigned char, 32> out{};
  std::memcpy(out.data(), value, 32);
  return out;
}

bool digest(const unsigned char *data, size_t size, unsigned char output[32]) {
  unsigned int length = 0;
  return EVP_Digest(data, size, output, &length, EVP_sha256(), nullptr) == 1 &&
         length == 32;
}

Lardon3DCalibrationWorkflowResult read_verified(
    const char *path, const unsigned char expected[32], std::string *output) {
  if (!path || !expected || !output)
    return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;
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
  if (static_cast<uint64_t>(st.st_size) >
      LARDON3D_CALIBRATION_WORKFLOW_MAX_FILE_BYTES) {
    close(fd);
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
  }
  const size_t size = static_cast<size_t>(st.st_size);
  output->assign(size, '\0');
  size_t used = 0;
  while (used < size) {
    ssize_t n = pread(fd, output->data() + used, size - used,
                      static_cast<off_t>(used));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      close(fd);
      return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
    }
    used += static_cast<size_t>(n);
  }
  if (close(fd) != 0) return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  unsigned char actual[32]{};
  if (!digest(reinterpret_cast<const unsigned char *>(output->data()),
              output->size(), actual))
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;
  if (std::memcmp(actual, expected, 32) != 0)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
  return LARDON3D_CALIBRATION_WORKFLOW_OK;
}

bool parse_u64(std::string_view text, uint64_t *value) {
  if (!value || text.empty()) return false;
  uint64_t v = 0;
  auto result = std::from_chars(text.data(), text.data() + text.size(), v, 10);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    return false;
  *value = v;
  return true;
}

bool parse_u32(std::string_view text, uint32_t *value) {
  uint64_t v = 0;
  if (!parse_u64(text, &v) || v > UINT32_MAX) return false;
  *value = static_cast<uint32_t>(v);
  return true;
}

bool parse_double_c(std::string_view text, double *value) {
  if (!value || text.empty() || text.size() > 128) return false;
  char buffer[129];
  std::memcpy(buffer, text.data(), text.size());
  buffer[text.size()] = '\0';
  locale_t locale = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
  if (!locale) return false;
  char *end = nullptr;
  errno = 0;
  double v = strtod_l(buffer, &end, locale);
  freelocale(locale);
  if (errno == ERANGE || end != buffer + text.size() || !std::isfinite(v))
    return false;
  *value = v;
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

bool token_ok(std::string_view token) {
  if (token.empty() ||
      token.size() >= LARDON3D_CALIBRATION_WORKFLOW_OPTICAL_STATE_TOKEN_CAPACITY)
    return false;
  for (unsigned char c : token) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
          c == ':'))
      return false;
  }
  return true;
}

struct SessionView {
  std::array<unsigned char, 32> sha{};
  uint32_t orientation = 0;
  double pre_solve = -1.0;
  double clipping = -1.0;
  double distance = -1.0;
  uint32_t distance_band = UINT32_MAX;
  uint32_t coordinate_orientation = UINT32_MAX;
  uint32_t coordinate_width = 0;
  uint32_t coordinate_height = 0;
  uint32_t coordinate_count = 0;
  unsigned coordinate_coverage = 0;
  std::vector<Lardon3DCalibrationToolingCoordinateCheck> checks;
};

struct SessionData {
  std::string target_id;
  std::string instrument;
  std::string decoder;
  std::string decoder_version;
  std::string optical_token;
  unsigned char generator_sha[32]{};
  unsigned char planarity_sha[32]{};
  unsigned char optical_sha[32]{};
  double measurements[10]{};
  double measurement_resolution = 0.0;
  double white_border = 0.0;
  std::vector<SessionView> views;
};

int coverage_bit(std::string_view label) {
  static const std::array<std::string_view, 9> labels = {
      "center", "top", "right", "bottom", "left",
      "top_left", "top_right", "bottom_left", "bottom_right"};
  for (size_t i = 0; i < labels.size(); ++i)
    if (label == labels[i]) return static_cast<int>(i);
  return -1;
}

bool parse_session_materialization(std::string_view text, SessionData *out) {
  if (!out || text.empty() || text.back() != '\n' ||
      text.find('\0') != std::string_view::npos ||
      text.find('\r') != std::string_view::npos)
    return false;
  std::map<std::array<unsigned char, 32>, size_t, ShaLess> index;
  bool target = false, measurement = false, border = false, planarity = false;
  bool decoder = false, optical = false;
  size_t at = 0, line_number = 0;
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
    if (w[0] == "target") {
      uint32_t sx = 0, sy = 0;
      double square = 0.0, marker = 0.0;
      if (target || w.size() != 8 || !token_ok(w[1]) ||
          !parse_sha(w[2], out->generator_sha) ||
          w[3] != "DICT_5X5_100" || !parse_u32(w[4], &sx) ||
          !parse_u32(w[5], &sy) || !parse_double_c(w[6], &square) ||
          !parse_double_c(w[7], &marker) || sx != 9 || sy != 7 ||
          square != 30.0 || marker != 21.0)
        return false;
      out->target_id.assign(w[1]);
      target = true;
    } else if (w[0] == "measurement") {
      if (measurement || w.size() != 13 || !token_ok(w[1]) ||
          !parse_double_c(w[2], &out->measurement_resolution))
        return false;
      out->instrument.assign(w[1]);
      for (size_t i = 0; i < 10; ++i)
        if (!parse_double_c(w[i + 3], &out->measurements[i])) return false;
      measurement = true;
    } else if (w[0] == "white_border") {
      if (border || w.size() != 2 || !parse_double_c(w[1], &out->white_border))
        return false;
      border = true;
    } else if (w[0] == "planarity") {
      if (planarity || w.size() != 3 || w[1] != "PASS" ||
          !parse_sha(w[2], out->planarity_sha))
        return false;
      planarity = true;
    } else if (w[0] == "decoder") {
      if (decoder || w.size() != 3 || !token_ok(w[1]) || !token_ok(w[2]))
        return false;
      out->decoder.assign(w[1]);
      out->decoder_version.assign(w[2]);
      decoder = true;
    } else if (w[0] == "optical_state") {
      if (optical || w.size() != 3 || !parse_sha(w[1], out->optical_sha) ||
          !token_ok(w[2]) || w[2] == "UNKNOWN")
        return false;
      out->optical_token.assign(w[2]);
      optical = true;
    } else if (w[0] == "image") {
      if (w.size() != 4 || w[1].empty() || w[1].size() > 4096) return false;
      SessionView view;
      if (!parse_sha(w[2], view.sha.data()) || !parse_u32(w[3], &view.orientation) ||
          (view.orientation != 0 && view.orientation != 90 &&
           view.orientation != 180 && view.orientation != 270))
        return false;
      auto inserted = index.emplace(view.sha, out->views.size());
      if (!inserted.second ||
          out->views.size() >= LARDON3D_CALIBRATION_WORKFLOW_MAX_SELECTED_ITEMS)
        return false;
      out->views.push_back(view);
    } else if (w[0] == "pre_solve" || w[0] == "clipping" ||
               w[0] == "distance" || w[0] == "coordinate" ||
               w[0] == "coordinate_point") {
      if (w.size() < 2) return false;
      std::array<unsigned char, 32> sha{};
      if (!parse_sha(w[1], sha.data())) return false;
      auto found = index.find(sha);
      if (found == index.end()) return false;
      SessionView& view = out->views[found->second];
      if (w[0] == "pre_solve") {
        if (w.size() != 3 || view.pre_solve >= 0 ||
            !parse_double_c(w[2], &view.pre_solve))
          return false;
      } else if (w[0] == "clipping") {
        if (w.size() != 3 || view.clipping >= 0 ||
            !parse_double_c(w[2], &view.clipping))
          return false;
      } else if (w[0] == "distance") {
        if (w.size() != 4 || view.distance > 0 ||
            !parse_double_c(w[2], &view.distance) ||
            !parse_u32(w[3], &view.distance_band) ||
            view.distance <= 0 || view.distance_band > 2)
          return false;
      } else if (w[0] == "coordinate") {
        double initial_dx = 0.0, initial_dy = 0.0;
        if (w.size() != 10 || view.coordinate_count != 0 ||
            w[2] != out->decoder || w[3] != out->decoder_version ||
            !parse_u32(w[4], &view.coordinate_orientation) ||
            !parse_u32(w[5], &view.coordinate_width) ||
            !parse_u32(w[6], &view.coordinate_height) ||
            !parse_u32(w[7], &view.coordinate_count) ||
            !parse_double_c(w[8], &initial_dx) ||
            !parse_double_c(w[9], &initial_dy) ||
            view.coordinate_orientation != view.orientation ||
            view.coordinate_width == 0 || view.coordinate_height == 0 ||
            view.coordinate_count < 20 || view.coordinate_count > 48 ||
            initial_dx < 0 || initial_dy < 0)
          return false;
        view.checks.reserve(view.coordinate_count);
      } else {
        double sx = 0.0, sy = 0.0, fx = 0.0, fy = 0.0;
        if (w.size() != 7 || view.coordinate_count == 0 ||
            view.checks.size() >= view.coordinate_count ||
            !parse_double_c(w[3], &sx) || !parse_double_c(w[4], &sy) ||
            !parse_double_c(w[5], &fx) || !parse_double_c(w[6], &fy))
          return false;
        int bit = coverage_bit(w[2]);
        if (bit < 0) return false;
        view.coordinate_coverage |= 1u << static_cast<unsigned>(bit);
        Lardon3DCalibrationToolingCoordinateCheck check{};
        std::memcpy(check.source_sha256, view.sha.data(), 32);
        check.orientation_degrees = view.orientation;
        check.dx_px = sx - fx;
        check.dy_px = sy - fy;
        view.checks.push_back(check);
      }
    } else {
      return false;
    }
  }
  if (!target || !measurement || !border || !planarity || !decoder || !optical ||
      out->views.empty())
    return false;
  for (const SessionView& view : out->views) {
    if (view.pre_solve < 0 || view.clipping < 0 || view.distance <= 0 ||
        view.distance_band > 2 || view.coordinate_count < 20 ||
        view.checks.size() != view.coordinate_count ||
        view.coordinate_coverage != 0x1ffu)
      return false;
  }
  return true;
}

class JsonCursor {
 public:
  explicit JsonCursor(std::string_view text) : text_(text) {}
  void ws() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\n' ||
            text_[at_] == '\r' || text_[at_] == '\t'))
      ++at_;
  }
  bool ch(char expected) {
    ws();
    if (at_ >= text_.size() || text_[at_] != expected) return false;
    ++at_;
    return true;
  }
  bool key(std::string_view expected) {
    std::string value;
    return string(&value) && value == expected && ch(':');
  }
  bool string(std::string *output) {
    ws();
    if (at_ >= text_.size() || text_[at_] != '"') return false;
    ++at_;
    output->clear();
    while (at_ < text_.size()) {
      unsigned char c = static_cast<unsigned char>(text_[at_++]);
      if (c == '"') return true;
      if (c < 0x20 || c == '\\') return false;
      output->push_back(static_cast<char>(c));
    }
    return false;
  }
  bool u64(uint64_t *output) {
    ws();
    size_t start = at_;
    if (start >= text_.size() || text_[start] < '0' || text_[start] > '9')
      return false;
    while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
    return parse_u64(text_.substr(start, at_ - start), output);
  }
  bool u32(uint32_t *output) {
    uint64_t v = 0;
    if (!u64(&v) || v > UINT32_MAX) return false;
    *output = static_cast<uint32_t>(v);
    return true;
  }
  bool boolean(bool *output) {
    ws();
    if (text_.substr(at_, 4) == "true") {
      at_ += 4; *output = true; return true;
    }
    if (text_.substr(at_, 5) == "false") {
      at_ += 5; *output = false; return true;
    }
    return false;
  }
  bool hex_double(double *output) {
    std::string value;
    return string(&value) && parse_double_c(value, output);
  }
  bool done() {
    ws();
    return at_ == text_.size();
  }
 private:
  std::string_view text_;
  size_t at_ = 0;
};

bool comma(JsonCursor *j) { return j->ch(','); }

uint32_t rejection_reason(std::string_view reason) {
  static const std::array<std::string_view, 10> reasons = {
      "source_size", "decode", "decoded_dimensions", "insufficient_charuco",
      "invalid_charuco_id", "occupancy", "target_physical_quadrants",
      "clipping", "pre_solve_corner_rms", "coordinate_equivalence"};
  for (size_t i = 0; i < reasons.size(); ++i)
    if (reason == reasons[i]) return static_cast<uint32_t>(i + 1);
  return 0;
}

struct DetectionView {
  Lardon3DCalibrationToolingView tooling{};
  uint32_t width = 0;
  uint32_t height = 0;
  bool coordinate_pass = false;
  std::set<uint32_t> corner_ids;
};

bool parse_detection(std::string_view text, const SessionData& session,
                     std::vector<DetectionView> *views) {
  JsonCursor j(text);
  std::string format, decoder, decoder_version;
  if (!j.ch('{') || !j.key("format") || !j.string(&format) ||
      format != "L3DCAL_DETECTION_V1" || !comma(&j) ||
      !j.key("decoder") || !j.string(&decoder) || decoder != session.decoder ||
      !comma(&j) || !j.key("decoder_version") ||
      !j.string(&decoder_version) || decoder_version != session.decoder_version ||
      !comma(&j) || !j.key("views") || !j.ch('['))
    return false;

  std::map<std::array<unsigned char, 32>, size_t, ShaLess> session_index;
  for (size_t i = 0; i < session.views.size(); ++i)
    session_index.emplace(session.views[i].sha, i);
  std::set<std::array<unsigned char, 32>, ShaLess> seen;
  std::array<unsigned char, 32> previous{};
  bool have_previous = false;

  j.ws();
  if (!j.ch(']')) {
    while (true) {
      DetectionView dv;
      std::string sha_text, decision, reason;
      uint32_t physical_quadrants = 0, comparison_points = 0;
      double coordinate_dx = 0.0, coordinate_dy = 0.0;
      bool coordinate_pass = false, holdout = false;
      if (!j.ch('{') || !j.key("source_sha256") || !j.string(&sha_text) ||
          !parse_sha(sha_text, dv.tooling.source_sha256) || !comma(&j) ||
          !j.key("orientation") || !j.u32(&dv.tooling.orientation_degrees) ||
          !comma(&j) || !j.key("oriented_width") || !j.u32(&dv.width) ||
          !comma(&j) || !j.key("oriented_height") || !j.u32(&dv.height) ||
          !comma(&j) || !j.key("decision") || !j.string(&decision) ||
          !comma(&j) || !j.key("reason") || !j.string(&reason) ||
          !comma(&j) || !j.key("frame_region") || !j.u32(&dv.tooling.quadrant) ||
          !comma(&j) || !j.key("distance_band") ||
          !j.u32(&dv.tooling.distance_band) || !comma(&j) ||
          !j.key("holdout") || !j.boolean(&holdout) || !comma(&j) ||
          !j.key("target_occupancy") || !j.hex_double(&dv.tooling.target_occupancy) ||
          !comma(&j) || !j.key("normal_angle_degrees") ||
          !j.hex_double(&dv.tooling.normal_angle_degrees) || !comma(&j) ||
          !j.key("measured_distance_metres") ||
          !j.hex_double(&dv.tooling.distance_metres) || !comma(&j) ||
          !j.key("pre_solve_corner_rms_px") ||
          !j.hex_double(&dv.tooling.corner_rms_px) || !comma(&j) ||
          !j.key("clipping_fraction") ||
          !j.hex_double(&dv.tooling.clipped_fraction) || !comma(&j) ||
          !j.key("physical_target_quadrants") || !j.u32(&physical_quadrants) ||
          !comma(&j) || !j.key("target_corner_quadrant_mask") ||
          !j.u32(&dv.tooling.target_corner_quadrant_mask) || !comma(&j) ||
          !j.key("corner_count") || !j.u32(&dv.tooling.corner_count) ||
          !comma(&j) || !j.key("residual_count") ||
          !j.u32(&dv.tooling.residual_count) || !comma(&j) ||
          !j.key("high_residual_count") ||
          !j.u32(&dv.tooling.high_residual_count) || !comma(&j) ||
          !j.key("reprojection_rmse_px") ||
          !j.hex_double(&dv.tooling.reprojection_rmse_px) || !comma(&j) ||
          !j.key("maximum_residual_px") ||
          !j.hex_double(&dv.tooling.maximum_residual_px) || !comma(&j) ||
          !j.key("coordinate_equivalence") || !j.ch('{') ||
          !j.key("comparison_points") || !j.u32(&comparison_points) ||
          !comma(&j) || !j.key("max_abs_dx_px") || !j.hex_double(&coordinate_dx) ||
          !comma(&j) || !j.key("max_abs_dy_px") || !j.hex_double(&coordinate_dy) ||
          !comma(&j) || !j.key("pass") || !j.boolean(&coordinate_pass) ||
          !j.ch('}') || !comma(&j) || !j.key("corners") || !j.ch('['))
        return false;

      auto key = sha_array(dv.tooling.source_sha256);
      auto sit = session_index.find(key);
      if (sit == session_index.end() || !seen.insert(key).second) return false;
      if (have_previous &&
          !std::lexicographical_compare(previous.begin(), previous.end(),
                                        key.begin(), key.end()))
        return false;
      previous = key;
      have_previous = true;
      const SessionView& sv = session.views[sit->second];

      size_t corner_count = 0;
      j.ws();
      if (!j.ch(']')) {
        while (true) {
          uint32_t id = 0;
          double x = 0.0, y = 0.0;
          if (!j.ch('{') || !j.key("id") || !j.u32(&id) || !comma(&j) ||
              !j.key("x") || !j.hex_double(&x) || !comma(&j) ||
              !j.key("y") || !j.hex_double(&y) || !j.ch('}') ||
              !dv.corner_ids.insert(id).second)
            return false;
          ++corner_count;
          j.ws();
          if (j.ch(']')) break;
          if (!comma(&j)) return false;
        }
      }
      if (!j.ch('}')) return false;

      dv.tooling.accepted = decision == "accepted" ? 1u : 0u;
      if (decision != "accepted" && decision != "rejected") return false;
      if (dv.tooling.accepted) {
        if (reason != "-" || !coordinate_pass || dv.width == 0 || dv.height == 0 ||
            dv.width != sv.coordinate_width ||
            dv.height != sv.coordinate_height ||
            dv.tooling.orientation_degrees != sv.orientation ||
            dv.tooling.corner_rms_px != sv.pre_solve ||
            dv.tooling.distance_metres != sv.distance ||
            dv.tooling.distance_band != sv.distance_band ||
            comparison_points != sv.coordinate_count ||
            dv.tooling.corner_count != corner_count ||
            dv.tooling.residual_count != dv.tooling.corner_count)
          return false;
      } else {
        dv.tooling.rejection_reason = rejection_reason(reason);
        if (reason == "-" || dv.tooling.rejection_reason == 0 || holdout)
          return false;
      }
      dv.tooling.holdout = holdout ? 1u : 0u;
      unsigned pop = 0;
      for (unsigned bit = 0; bit < 4; ++bit)
        pop += (dv.tooling.target_corner_quadrant_mask >> bit) & 1u;
      if (physical_quadrants != pop ||
          dv.tooling.high_residual_count > dv.tooling.residual_count ||
          coordinate_dx < 0 || coordinate_dy < 0)
        return false;
      dv.coordinate_pass = coordinate_pass;
      views->push_back(std::move(dv));
      j.ws();
      if (j.ch(']')) break;
      if (!comma(&j)) return false;
    }
  }
  if (!j.ch('}') || !j.done() || views->size() != session.views.size())
    return false;
  return true;
}

bool parse_parameter_array(JsonCursor *j, double output[8]) {
  if (!j->ch('[')) return false;
  for (size_t i = 0; i < 8; ++i) {
    if (!j->hex_double(&output[i])) return false;
    if (i + 1 != 8 && !comma(j)) return false;
  }
  return j->ch(']');
}

bool parse_vec3(JsonCursor *j) {
  if (!j->ch('[')) return false;
  for (size_t i = 0; i < 3; ++i) {
    double value = 0.0;
    if (!j->hex_double(&value)) return false;
    if (i + 1 != 3 && !comma(j)) return false;
  }
  return j->ch(']');
}

struct SolveData {
  double repeated[3][8]{};
  double fit[8]{};
};

bool parse_solve(std::string_view text, size_t accepted_count, SolveData *out) {
  JsonCursor j(text);
  std::string format;
  if (!j.ch('{') || !j.key("format") || !j.string(&format) ||
      format != "L3DCAL_SOLVE_V1" || !comma(&j) ||
      !j.key("runs") || !j.ch('['))
    return false;
  for (uint32_t run = 0; run < 3; ++run) {
    uint32_t run_id = UINT32_MAX;
    double opencv_rms = 0.0;
    if (!j.ch('{') || !j.key("run") || !j.u32(&run_id) || run_id != run ||
        !comma(&j) || !j.key("params") ||
        !parse_parameter_array(&j, out->repeated[run]) || !comma(&j) ||
        !j.key("opencv_rms_px") || !j.hex_double(&opencv_rms) ||
        opencv_rms < 0 || !comma(&j) || !j.key("poses") || !j.ch('['))
      return false;
    size_t poses = 0;
    j.ws();
    if (!j.ch(']')) {
      while (true) {
        if (!j.ch('{') || !j.key("rvec") || !parse_vec3(&j) ||
            !comma(&j) || !j.key("tvec_m") || !parse_vec3(&j) || !j.ch('}'))
          return false;
        ++poses;
        j.ws();
        if (j.ch(']')) break;
        if (!comma(&j)) return false;
      }
    }
    if (poses != accepted_count || !j.ch('}')) return false;
    if (run != 2 && !comma(&j)) return false;
  }
  if (!j.ch(']') || !comma(&j) || !j.key("fit_params") ||
      !parse_parameter_array(&j, out->fit) || !j.ch('}') || !j.done())
    return false;
  for (size_t run = 1; run < 3; ++run)
    if (std::memcmp(out->repeated[0], out->repeated[run],
                    sizeof(out->repeated[0])) != 0)
      return false;
  return true;
}

struct EvidenceData {
  double global_rmse = 0.0;
  double global_max = 0.0;
  double high_fraction = 0.0;
  double holdout_rmse = 0.0;
  double holdout_max = 0.0;
  double maximum_delta = 0.0;
  uint32_t flags = 0;
};

bool parse_flags(std::string_view text, uint32_t *flags) {
  if (text.size() != 3 || text[0] != '0' || text[1] != 'x') return false;
  unsigned v = 0;
  if (!hex_digit(text[2], &v) || v > UINT32_MAX) return false;
  *flags = static_cast<uint32_t>(v);
  return true;
}

bool parse_evidence(std::string_view text, const SessionData& session,
                    const std::vector<DetectionView>& views,
                    EvidenceData *out) {
  JsonCursor j(text);
  std::string format, target_id, generator, instrument, planarity, optical,
      optical_state, flags_text;
  double resolution = 0.0;
  bool deterministic = false;
  if (!j.ch('{') || !j.key("format") || !j.string(&format) ||
      format != "L3DCAL_EVIDENCE_BUNDLE_V1" || !comma(&j) ||
      !j.key("target") || !j.ch('{') ||
      !j.key("id") || !j.string(&target_id) || !comma(&j) ||
      !j.key("generator_sha256") || !j.string(&generator) || !comma(&j) ||
      !j.key("instrument") || !j.string(&instrument) || !comma(&j) ||
      !j.key("resolution_mm") || !j.hex_double(&resolution) || !comma(&j) ||
      !j.key("planarity_evidence_sha256") || !j.string(&planarity) ||
      !j.ch('}') || !comma(&j) ||
      !j.key("optical_sha256") || !j.string(&optical) || !comma(&j) ||
      !j.key("optical_state") || !j.string(&optical_state) || !comma(&j) ||
      !j.key("validation_flags") || !j.string(&flags_text) || !comma(&j) ||
      !j.key("global_rmse_px") || !j.hex_double(&out->global_rmse) ||
      !comma(&j) || !j.key("maximum_residual_px") ||
      !j.hex_double(&out->global_max) || !comma(&j) ||
      !j.key("high_residual_fraction") || !j.hex_double(&out->high_fraction) ||
      !comma(&j) || !j.key("holdout") || !j.ch('{') ||
      !j.key("rmse_px") || !j.hex_double(&out->holdout_rmse) || !comma(&j) ||
      !j.key("maximum_px") || !j.hex_double(&out->holdout_max) ||
      !j.ch('}') || !comma(&j) ||
      !j.key("maximum_parameter_delta_px") ||
      !j.hex_double(&out->maximum_delta) || !comma(&j) ||
      !j.key("deterministic_full_solve_equality") ||
      !j.boolean(&deterministic) || !comma(&j) ||
      !j.key("residuals") || !j.ch('['))
    return false;

  unsigned char generator_sha[32]{}, planarity_sha[32]{}, optical_sha[32]{};
  if (target_id != session.target_id || instrument != session.instrument ||
      resolution != session.measurement_resolution ||
      !parse_sha(generator, generator_sha) ||
      !parse_sha(planarity, planarity_sha) || !parse_sha(optical, optical_sha) ||
      std::memcmp(generator_sha, session.generator_sha, 32) != 0 ||
      std::memcmp(planarity_sha, session.planarity_sha, 32) != 0 ||
      std::memcmp(optical_sha, session.optical_sha, 32) != 0 ||
      optical_state != session.optical_token ||
      !parse_flags(flags_text, &out->flags) || out->flags != 0x0fu ||
      !deterministic || out->global_rmse < 0 || out->global_max < 0 ||
      out->high_fraction < 0 || out->high_fraction > 1 ||
      out->holdout_rmse < 0 || out->holdout_max < 0 ||
      out->maximum_delta < 0)
    return false;

  std::map<std::array<unsigned char, 32>, size_t, ShaLess> view_index;
  for (size_t i = 0; i < views.size(); ++i)
    view_index.emplace(sha_array(views[i].tooling.source_sha256), i);
  std::vector<uint32_t> counts(views.size(), 0), highs(views.size(), 0);
  std::set<std::pair<size_t, uint32_t>> residual_ids;
  uint64_t total = 0, total_high = 0;

  j.ws();
  if (!j.ch(']')) {
    while (true) {
      std::string sha_text;
      uint32_t corner_id = 0;
      double dx = 0.0, dy = 0.0, rmse = 0.0;
      if (!j.ch('{') || !j.key("source_sha256") || !j.string(&sha_text) ||
          !comma(&j) || !j.key("corner_id") || !j.u32(&corner_id) ||
          !comma(&j) || !j.key("dx_px") || !j.hex_double(&dx) ||
          !comma(&j) || !j.key("dy_px") || !j.hex_double(&dy) ||
          !comma(&j) || !j.key("rmse_px") || !j.hex_double(&rmse) ||
          !j.ch('}'))
        return false;
      unsigned char sha[32]{};
      if (!parse_sha(sha_text, sha)) return false;
      auto found = view_index.find(sha_array(sha));
      if (found == view_index.end()) return false;
      size_t vi = found->second;
      const DetectionView& dv = views[vi];
      if (!dv.tooling.accepted || dv.corner_ids.find(corner_id) == dv.corner_ids.end() ||
          !residual_ids.emplace(vi, corner_id).second ||
          rmse != dv.tooling.reprojection_rmse_px)
        return false;
      ++counts[vi];
      ++total;
      const double squared = dx * dx + dy * dy;
      if (!std::isfinite(squared)) return false;
      if (squared > 1.0) {
        ++highs[vi];
        ++total_high;
      }
      if (total > kMaxObservations) return false;
      j.ws();
      if (j.ch(']')) break;
      if (!comma(&j)) return false;
    }
  }
  if (!j.ch('}') || !j.done() || total == 0) return false;
  for (size_t i = 0; i < views.size(); ++i) {
    if (views[i].tooling.accepted) {
      if (counts[i] != views[i].tooling.residual_count ||
          highs[i] != views[i].tooling.high_residual_count)
        return false;
    } else if (counts[i] != 0) {
      return false;
    }
  }
  const double calculated_fraction =
      static_cast<double>(total_high) / static_cast<double>(total);
  if (calculated_fraction != out->high_fraction) return false;
  return true;
}

bool validation_binding(const Lardon3DCalibrationWorkflowInputBoundary& boundary,
                        unsigned char output[32]) {
  static const char domain[] = "L3DCAL_WORKFLOW_VALIDATION_V1\n";
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) return false;
  bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
            EVP_DigestUpdate(ctx, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(ctx, boundary.detection_sha256, 32) == 1 &&
            EVP_DigestUpdate(ctx, boundary.solve_sha256, 32) == 1 &&
            EVP_DigestUpdate(ctx, boundary.evidence_sha256, 32) == 1 &&
            EVP_DigestUpdate(ctx, boundary.producer_sha256, 32) == 1;
  unsigned int length = 0;
  ok = ok && EVP_DigestFinal_ex(ctx, output, &length) == 1 && length == 32;
  EVP_MD_CTX_free(ctx);
  return ok;
}

}  // namespace

extern "C" Lardon3DCalibrationWorkflowResult
lardon3d_calibration_workflow_materialize_external_evidence(
    const Lardon3DCalibrationWorkflowInputFiles *files,
    Lardon3DCalibrationToolingView *views, size_t view_capacity,
    Lardon3DCalibrationToolingCoordinateCheck *coordinate_checks,
    size_t coordinate_check_capacity,
    Lardon3DCalibrationWorkflowExternalEvidence *output) {
  if (!files || !views || !coordinate_checks || !output)
    return LARDON3D_CALIBRATION_WORKFLOW_INVALID_ARGUMENT;
  std::memset(output, 0, sizeof(*output));

  Lardon3DCalibrationWorkflowInputBoundary boundary{};
  Lardon3DCalibrationWorkflowResult result =
      lardon3d_calibration_workflow_validate_input_boundary(files, &boundary);
  if (result != LARDON3D_CALIBRATION_WORKFLOW_OK) return result;

  std::string session_text, detection_text, solve_text, evidence_text;
  const struct {
    const char *path;
    const unsigned char *sha;
    std::string *text;
  } inputs[] = {
      {files->session_path, boundary.session_sha256, &session_text},
      {files->detection_path, boundary.detection_sha256, &detection_text},
      {files->solve_path, boundary.solve_sha256, &solve_text},
      {files->evidence_path, boundary.evidence_sha256, &evidence_text},
  };
  for (const auto& input : inputs) {
    result = read_verified(input.path, input.sha, input.text);
    if (result != LARDON3D_CALIBRATION_WORKFLOW_OK) return result;
  }

  SessionData session;
  if (!parse_session_materialization(session_text, &session))
    return LARDON3D_CALIBRATION_WORKFLOW_MALFORMED_EVIDENCE;
  if (std::memcmp(session.optical_sha, boundary.optical_state_sha256, 32) != 0 ||
      session.optical_token != boundary.optical_state_token)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  std::vector<DetectionView> parsed_views;
  if (!parse_detection(detection_text, session, &parsed_views))
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  size_t accepted = 0;
  uint64_t support_observations = 0;
  uint32_t oriented_width = 0;
  uint32_t oriented_height = 0;
  for (const DetectionView& view : parsed_views) {
    if (view.tooling.accepted) {
      if (accepted == 0) {
        oriented_width = view.width;
        oriented_height = view.height;
      } else if (view.width != oriented_width ||
                 view.height != oriented_height) {
        return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
      }
      ++accepted;
      support_observations += view.tooling.residual_count;
      if (support_observations > UINT32_MAX)
        return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
    }
  }
  if (accepted == 0 || oriented_width == 0 || oriented_height == 0)
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  SolveData solve;
  if (!parse_solve(solve_text, accepted, &solve))
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;
  EvidenceData evidence;
  if (!parse_evidence(evidence_text, session, parsed_views, &evidence))
    return LARDON3D_CALIBRATION_WORKFLOW_PROVENANCE_MISMATCH;

  size_t check_count = 0;
  for (const SessionView& view : session.views) {
    if (SIZE_MAX - check_count < view.checks.size())
      return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;
    check_count += view.checks.size();
  }
  if (parsed_views.size() > view_capacity ||
      check_count > coordinate_check_capacity)
    return LARDON3D_CALIBRATION_WORKFLOW_CAPACITY;

  for (size_t i = 0; i < parsed_views.size(); ++i)
    views[i] = parsed_views[i].tooling;
  size_t at = 0;
  for (const SessionView& view : session.views)
    for (const auto& check : view.checks) coordinate_checks[at++] = check;

  output->boundary = boundary;
  std::memcpy(output->target_sha256, session.generator_sha, 32);
  std::memcpy(output->optical_state_sha256, boundary.optical_state_sha256, 32);
  std::memcpy(output->solver_executable_sha256,
              boundary.solver_executable_sha256, 32);
  std::memcpy(output->solver_configuration_sha256,
              boundary.solver_configuration_sha256, 32);
  std::memcpy(output->initialization_evidence_sha256,
              boundary.session_sha256, 32);
  if (!validation_binding(boundary, output->validation_evidence_sha256))
    return LARDON3D_CALIBRATION_WORKFLOW_IO_ERROR;

  output->target_family =
      LARDON3D_CALIBRATION_TOOLING_TARGET_CHARUCO_9X7_DICT_5X5_100;
  output->target_squares_x = 9;
  output->target_squares_y = 7;
  output->target_square_length_mm = 30.0;
  output->target_marker_length_mm = 21.0;
  output->target_active_width_mm = 270.0;
  output->target_active_height_mm = 210.0;
  output->target_white_border_mm = session.white_border;
  for (size_t i = 0; i < 10; ++i)
    output->target_measurements_mm[i] = session.measurements[i];
  output->measurement_resolution_mm = session.measurement_resolution;
  output->target_flatness_mm = NAN;
  output->holdout_rmse_px = evidence.holdout_rmse;
  output->holdout_maximum_residual_px = evidence.holdout_max;
  output->extra_distortion_coefficient_count = 0;
  output->oriented_width = oriented_width;
  output->oriented_height = oriented_height;
  output->views = views;
  output->view_count = parsed_views.size();
  output->coordinate_checks = coordinate_checks;
  output->coordinate_check_count = check_count;
  std::memcpy(output->repeated_parameters, solve.repeated,
              sizeof(output->repeated_parameters));
  std::memcpy(output->fit_parameters, solve.fit,
              sizeof(output->fit_parameters));
  output->support_images = static_cast<uint32_t>(accepted);
  output->support_observations = static_cast<uint32_t>(support_observations);
  output->reprojection_rmse_px = evidence.global_rmse;
  output->maximum_residual_px = evidence.global_max;
  output->high_residual_fraction = evidence.high_fraction;
  output->maximum_parameter_delta = evidence.maximum_delta;
  output->validation_flags = evidence.flags;
  return LARDON3D_CALIBRATION_WORKFLOW_OK;
}
