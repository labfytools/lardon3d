#include <lardon3d/dense_mvs.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <openssl/evp.h>
#include <poll.h>
#include <sstream>
#include <signal.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace lardon3d::dense_mvs_detail {
bool source_image_set_identity(const Lardon3DDenseMvsSourceImage *images,
                               size_t count, unsigned char digest[32]);
bool calibration_binding_identity(const Lardon3DDenseMvsSourceImage *images,
                                  size_t count, unsigned char digest[32]);
} // namespace lardon3d::dense_mvs_detail

namespace {
constexpr size_t kMaxVersion = 16384;
constexpr int kVersionTimeoutMs = 5000;
constexpr uint64_t kMaxSourceFileBytes = UINT64_C(1024) * 1024 * 1024;
constexpr uint64_t kMaxBackendFileBytes = UINT64_C(1024) * 1024 * 1024;
constexpr size_t kMaxBackendLogBytes = 1024 * 1024;
constexpr size_t kMaxPlyHeader = 1024 * 1024;
constexpr size_t kMaxPlyLine = 64 * 1024;
constexpr size_t kMaxPlyProperties = 256;
bool sha(const unsigned char *p, size_t n, unsigned char d[32]) {
  unsigned int length = 0;
  return p && d &&
         EVP_Digest(p, n, d, &length, EVP_sha256(), nullptr) == 1 &&
         length == 32;
}
bool executable(const char *path) {
  struct stat status {};
  return path && stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
         access(path, X_OK) == 0;
}
bool same_file_snapshot(const struct stat &before, const struct stat &after) {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}
bool sha_regular_file(const char *path, unsigned char digest[32],
                      uint64_t max_bytes = UINT64_MAX,
                      uint64_t *remaining_bytes = nullptr) {
  if (!path || !digest) return false;
  const int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) return false;
  struct stat status {};
  const uint64_t effective_max = remaining_bytes
                                     ? std::min(max_bytes, *remaining_bytes)
                                     : max_bytes;
  bool ok = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_size >= 0 &&
            static_cast<uint64_t>(status.st_size) <= effective_max;
  if (!ok) {
    (void)close(descriptor);
    return false;
  }
  FILE *file = fdopen(descriptor, "rb");
  if (!file) {
    (void)close(descriptor);
    return false;
  }
  const uint64_t expected_size = ok ? static_cast<uint64_t>(status.st_size) : 0;
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  ok = ok && context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  unsigned char buffer[8192];
  uint64_t total = 0;
  while (ok && total < expected_size) {
    const uint64_t remaining = expected_size - total;
    const size_t request = static_cast<size_t>(
        std::min<uint64_t>(remaining, sizeof buffer));
    const size_t size = std::fread(buffer, 1, request, file);
    if (size != 0) {
      total += size;
      ok = EVP_DigestUpdate(context, buffer, size) == 1;
    }
    if (size < request) ok = false;
  }
  struct stat final_status {};
  ok = ok && !std::ferror(file) && total == expected_size &&
       fstat(fileno(file), &final_status) == 0 &&
       same_file_snapshot(status, final_status);
  unsigned int length = 0;
  if (ok) ok = EVP_DigestFinal_ex(context, digest, &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  const int close_result = std::fclose(file);
  ok = ok && close_result == 0;
  if (ok && remaining_bytes) *remaining_bytes -= expected_size;
  return ok;
}
bool ms(uint64_t *value) {
  timespec time {};
  if (!value || clock_gettime(CLOCK_MONOTONIC, &time) || time.tv_sec < 0)
    return false;
  *value = static_cast<uint64_t>(time.tv_sec) * 1000 +
           static_cast<uint64_t>(time.tv_nsec) / 1000000;
  return true;
}
bool wait_nonblocking(pid_t child, int *status, bool *exited) {
  for (;;) {
    const pid_t result = waitpid(child, status, WNOHANG);
    if (result == child) { *exited = true; return true; }
    if (result == 0) return true;
    if (errno == ECHILD) { *exited = true; return true; }
    if (errno != EINTR) return false;
  }
}

/* Probe children have a deadline; backend execution intentionally does not. */
bool signal_group(pid_t group, int signal) {
  return kill(-group, signal) == 0 || errno == ESRCH;
}

bool owns_process_group(pid_t child) {
  const pid_t group = getpgid(child);
  return group == child || (group < 0 && errno == ESRCH);
}

bool terminate_and_reap(pid_t child, pid_t group) {
  int status = 0;
  bool exited = false;
  if (!wait_nonblocking(child, &status, &exited)) return false;
  if (!signal_group(group, SIGTERM)) return false;
  uint64_t start = 0;
  if (!ms(&start)) return false;
  while (true) {
    uint64_t now = 0;
    if (!ms(&now) || now - start >= 250) break;
    if (!wait_nonblocking(child, &status, &exited)) return false;
    if (exited && kill(-group, 0) != 0 && errno == ESRCH) return true;
    (void)poll(nullptr, 0, 10);
  }
  if (!signal_group(group, SIGKILL)) return false;
  if (!ms(&start)) return false;
  while (true) {
    uint64_t now = 0;
    if (!ms(&now) || now - start >= 250) return false;
    if (!wait_nonblocking(child, &status, &exited)) return false;
    if (exited) return true;
    (void)poll(nullptr, 0, 10);
  }
}

bool run(const std::vector<std::string>& a, const std::string &directory,
         const char *log_name) {
  if(a.empty())return false;
  std::vector<char*> v;
  for(const auto&x:a)v.push_back(const_cast<char*>(x.c_str()));
  v.push_back(nullptr);
  const std::string log_path = directory + "/" + log_name;
  int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (log_fd < 0) return false;
  int output[2];
  if (pipe(output) != 0) { close(log_fd); return false; }
  int launch[2];
  if (pipe(launch) != 0) {
    close(output[0]); close(output[1]); close(log_fd); return false;
  }
  if (fcntl(launch[1], F_SETFD, FD_CLOEXEC) != 0) {
    close(launch[0]); close(launch[1]); close(output[0]); close(output[1]);
    close(log_fd); return false;
  }
  pid_t c=fork();
  if(c<0){close(launch[0]);close(launch[1]);close(output[0]);close(output[1]);close(log_fd);return false;}
  if(!c){
    close(launch[0]);
    close(output[0]);
    const int input_fd = open("/dev/null", O_RDONLY);
    if (setpgid(0, 0) != 0 || input_fd < 0 || dup2(input_fd, STDIN_FILENO) < 0 ||
        dup2(output[1], STDOUT_FILENO) < 0 ||
        dup2(output[1], STDERR_FILENO) < 0) {
      const int error = errno;
      (void)!write(launch[1], &error, sizeof error);
      _exit(127);
    }
    close(input_fd);
    close(output[1]);
    close(log_fd);
    execv(v[0],v.data());
    const int error = errno;
    (void)!write(launch[1], &error, sizeof error);
    _exit(127);
  }
  close(launch[1]);
  close(output[1]);
  if (setpgid(c, c) != 0 && errno != EACCES && errno != ESRCH) {
    close(launch[0]); close(output[0]); close(log_fd);
    (void)terminate_and_reap(c, c);
    return false;
  }
  if (!owns_process_group(c)) {
    close(launch[0]); close(output[0]); close(log_fd);
    (void)terminate_and_reap(c, c);
    return false;
  }
  int launch_error = 0;
  ssize_t launch_size;
  do { launch_size = read(launch[0], &launch_error, sizeof launch_error); }
  while (launch_size < 0 && errno == EINTR);
  close(launch[0]);
  if (launch_size != 0) {
    close(output[0]); close(log_fd);
    (void)terminate_and_reap(c, c);
    return false;
  }
  int flags = fcntl(output[0], F_GETFL);
  if (flags < 0 || fcntl(output[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(output[0]); close(log_fd);
    (void)terminate_and_reap(c, c);
    return false;
  }
  int s = 0;
  bool exited = false, eof = false, io_ok = true;
  size_t retained = 0;
  while (!exited || !eof) {
    if (!exited && !wait_nonblocking(c, &s, &exited)) { io_ok = false; break; }
    for (;;) {
      unsigned char buffer[8192];
      const ssize_t size = read(output[0], buffer, sizeof buffer);
      if (size > 0) {
        const size_t keep = std::min<size_t>(static_cast<size_t>(size),
                                             kMaxBackendLogBytes - retained);
        if (keep && write(log_fd, buffer, keep) != static_cast<ssize_t>(keep))
          io_ok = false;
        retained += keep;
        continue;
      }
      if (size == 0) eof = true;
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        io_ok = false;
      break;
    }
    if (!io_ok || (exited && eof)) break;
    pollfd descriptor{output[0], POLLIN | POLLHUP, 0};
    if (poll(&descriptor, 1, 50) < 0 && errno != EINTR) { io_ok = false; break; }
    if (exited && !eof && (kill(-c, 0) == 0 || errno != ESRCH)) {
      if (!terminate_and_reap(c, c)) io_ok = false;
    }
  }
  close(output[0]);
  if (close(log_fd) != 0) io_ok = false;
  if (!io_ok) {
    (void)terminate_and_reap(c, c);
    return false;
  }
  const bool ok = WIFEXITED(s)&&WEXITSTATUS(s)==0;
  /* A successful direct child may still have left helpers in its owned group. */
  if (kill(-c, 0) == 0 || errno != ESRCH) {
    if (!terminate_and_reap(c, c)) return false;
  }
  return ok;
}

struct BackendCapabilities {
  bool cuda_option;
  unsigned char version_identity[32];
};

bool probe(const char *path, bool interface_colmap,
           BackendCapabilities *capabilities) {
  int fd[2];
  if (!path || !capabilities || pipe(fd)) return false;
  pid_t c=fork();
  if(c<0){close(fd[0]);close(fd[1]);return false;}
  if(!c){
    if (setpgid(0, 0) != 0) _exit(127);
    const char *arguments[] = {path, "--help", nullptr};
    (void)dup2(fd[1],STDOUT_FILENO);
    (void)dup2(fd[1],STDERR_FILENO);
    close(fd[0]);
    close(fd[1]);
    execv(path, const_cast<char *const *>(arguments));
    _exit(127);
  }
  if (setpgid(c, c) != 0 && errno != EACCES && errno != ESRCH) {
    close(fd[0]);
    close(fd[1]);
    (void)terminate_and_reap(c, c);
    return false;
  }
  if (!owns_process_group(c)) {
    close(fd[0]);
    close(fd[1]);
    (void)terminate_and_reap(c, c);
    return false;
  }
  close(fd[1]);
  const int flags=fcntl(fd[0],F_GETFL);
  if(flags<0||fcntl(fd[0],F_SETFL,flags|O_NONBLOCK)!=0){close(fd[0]);(void)terminate_and_reap(c,c);return false;}
  std::vector<unsigned char> out;
  uint64_t start=0;
  bool ok=ms(&start), eof=false, exited=false;
  const uint64_t deadline=start+kVersionTimeoutMs;
  int s=0;
  while (ok && (!exited || !eof)) {
    if (!exited && !wait_nonblocking(c, &s, &exited)) {
      ok = false;
      break;
    }
    for (;;) {
      unsigned char b[512];
      const ssize_t n=read(fd[0],b,sizeof b);
      if(n>0){if((size_t)n>kMaxVersion-out.size()){ok=false;break;}out.insert(out.end(),b,b+n);continue;}
      if(n==0)eof=true;
      else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)ok=false;
      break;
    }
    if (!ok || (exited && eof)) break;
    uint64_t now=0;
    if(!ms(&now)||now>=deadline){ok=false;break;}
    pollfd x{fd[0],POLLIN|POLLHUP,0};
    const int r=poll(&x,1,(int)std::min<uint64_t>(deadline-now,50));
    if(r<0&&errno!=EINTR){ok=false;break;}
  }
  close(fd[0]);
  if(!ok||!exited){(void)terminate_and_reap(c,c);return false;}
  const bool process_ok = eof && WIFEXITED(s) && !out.empty();
  if (kill(-c, 0) == 0 || errno != ESRCH) {
    if (!terminate_and_reap(c, c)) return false;
  }
  if (!process_ok) return false;
  const std::string output(out.begin(), out.end());
  const bool pinned = output.find("OpenMVS") != std::string::npos &&
                      output.find("v2.4.0") != std::string::npos;
  const bool common = output.find("--max-threads") != std::string::npos;
  const bool specific = interface_colmap
                            ? output.find("--image-folder") != std::string::npos
                            : output.find("--resolution-level") != std::string::npos &&
                                  output.find("--min-resolution") != std::string::npos &&
                                  output.find("--number-views") != std::string::npos &&
                                  output.find("--fusion-mode") != std::string::npos;
  static const unsigned char version_record[] = "OpenMVS v2.4.0";
  capabilities->cuda_option = output.find("--cuda-device") != std::string::npos;
  return pinned && common && specific &&
         sha(version_record, sizeof version_record - 1,
             capabilities->version_identity);
}
bool finite_cal(const Lardon3DSparseGeometryCalibration &calibration) {
  return calibration.width && calibration.height &&
         std::isfinite(calibration.fx) && std::isfinite(calibration.fy) &&
         std::isfinite(calibration.cx) && std::isfinite(calibration.cy) &&
         std::isfinite(calibration.k1) && std::isfinite(calibration.k2) &&
         std::isfinite(calibration.p1) && std::isfinite(calibration.p2) &&
         calibration.fx > 0 && calibration.fy > 0;
}
struct ObservationKey {
  uint64_t feature_set_id;
  uint32_t feature_index;

  bool operator<(const ObservationKey &other) const {
    return feature_set_id < other.feature_set_id ||
           (feature_set_id == other.feature_set_id &&
            feature_index < other.feature_index);
  }
};

struct ExportObservation {
  const Lardon3DSparseIncrementalLandmarkObservation *snapshot;
  const Lardon3DSparseIncrementalObservation *source;
  uint32_t point_id;
  double x;
  double y;
};

bool finalize(std::ofstream &file) {
  file.flush();
  const bool written = file.good();
  file.close();
  return written && !file.fail();
}

bool valid_rotation(const double *r) {
  if (!r) return false;
  for (int i = 0; i < 9; ++i) if (!std::isfinite(r[i])) return false;
  double residual = 0;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      double dot = 0;
      for (int k = 0; k < 3; ++k) dot += r[row * 3 + k] * r[col * 3 + k];
      residual = std::max(residual, std::fabs(dot - (row == col ? 1.0 : 0.0)));
    }
  }
  const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) -
                     r[1] * (r[3] * r[8] - r[5] * r[6]) +
                     r[2] * (r[3] * r[7] - r[4] * r[6]);
  return residual <= 1e-6 && std::fabs(det - 1.0) <= 1e-6;
}
bool quaternion(const double *rotation, double q[4]) {
  if (!rotation || !q || !valid_rotation(rotation)) return false;
  const double trace = rotation[0] + rotation[4] + rotation[8];
  double scale = 0;
  if (trace > 0) {
    scale = std::sqrt(trace + 1) * 2;
    q[0] = 0.25 * scale;
    q[1] = (rotation[7] - rotation[5]) / scale;
    q[2] = (rotation[2] - rotation[6]) / scale;
    q[3] = (rotation[3] - rotation[1]) / scale;
  } else if (rotation[0] > rotation[4] && rotation[0] > rotation[8]) {
    scale = std::sqrt(std::max(0.0, 1 + rotation[0] - rotation[4] - rotation[8])) * 2;
    q[0] = (rotation[7] - rotation[5]) / scale;
    q[1] = 0.25 * scale;
    q[2] = (rotation[1] + rotation[3]) / scale;
    q[3] = (rotation[2] + rotation[6]) / scale;
  } else if (rotation[4] > rotation[8]) {
    scale = std::sqrt(std::max(0.0, 1 + rotation[4] - rotation[0] - rotation[8])) * 2;
    q[0] = (rotation[2] - rotation[6]) / scale;
    q[1] = (rotation[1] + rotation[3]) / scale;
    q[2] = 0.25 * scale;
    q[3] = (rotation[5] + rotation[7]) / scale;
  } else {
    scale = std::sqrt(std::max(0.0, 1 + rotation[8] - rotation[0] - rotation[4])) * 2;
    q[0] = (rotation[3] - rotation[1]) / scale;
    q[1] = (rotation[2] + rotation[6]) / scale;
    q[2] = (rotation[5] + rotation[7]) / scale;
    q[3] = 0.25 * scale;
  }
  const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] +
                                q[2] * q[2] + q[3] * q[3]);
  if (!(norm > 0) || !std::isfinite(norm)) return false;
  for (size_t index = 0; index < 4; ++index) q[index] /= norm;
  const bool negative = q[0] < 0 ||
                        (q[0] == 0 &&
                         (q[1] < 0 ||
                          (q[1] == 0 && (q[2] < 0 || (q[2] == 0 && q[3] < 0)))));
  if (negative)
    for (size_t index = 0; index < 4; ++index) q[index] = -q[index];
  return std::all_of(q, q + 4, [](double value) { return std::isfinite(value); });
}

bool finite_point(const Lardon3DSparseGeometryPoint3 &point) {
  return std::isfinite(point.x) && std::isfinite(point.y) &&
         std::isfinite(point.z);
}

/* Two fixed-size CV_8UC3 image buffers cover decode plus undistortion. */
constexpr uint64_t kMaxImageDimension = 16384;
constexpr uint64_t kMaxImagePixels = 40000000;
constexpr uint64_t kMaxImageWorkingSetBytes = 240000000;

bool bounded_image_allocation(uint32_t width, uint32_t height) {
  if (!width || !height || width > kMaxImageDimension ||
      height > kMaxImageDimension || width > UINT64_MAX / height)
    return false;
  const uint64_t pixels = static_cast<uint64_t>(width) * height;
  constexpr uint64_t bytes_per_pixel_for_two_images = 2U * 3U;
  return pixels <= kMaxImagePixels &&
         pixels <= kMaxImageWorkingSetBytes / bytes_per_pixel_for_two_images;
}

uint16_t be16(const unsigned char *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                               static_cast<uint16_t>(p[1]));
}
uint32_t be32(const unsigned char *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}
uint32_t le32(const unsigned char *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
         uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool encoded_image_dimensions(const char *path, uint32_t *width,
                              uint32_t *height) {
  if (!path || !width || !height) return false;
  const int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (descriptor < 0) return false;
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    (void)close(descriptor);
    return false;
  }
  unsigned char data[64 * 1024];
  size_t size = 0;
  bool read_ok = true;
  while (size < sizeof data) {
    ssize_t amount;
    do {
      amount = read(descriptor, data + size, sizeof data - size);
    } while (amount < 0 && errno == EINTR);
    if (amount < 0) {
      read_ok = false;
      break;
    }
    if (amount == 0) break;
    size += static_cast<size_t>(amount);
  }
  read_ok = close(descriptor) == 0 && read_ok;
  if (!read_ok) return false;
  if (size >= 24 && !std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) &&
      !std::memcmp(data + 12, "IHDR", 4)) {
    *width = be32(data + 16); *height = be32(data + 20); return true;
  }
  if (size >= 26 && data[0] == 'B' && data[1] == 'M') {
    *width = le32(data + 18);
    const int32_t signed_height = static_cast<int32_t>(le32(data + 22));
    if (signed_height == INT32_MIN) return false;
    *height = static_cast<uint32_t>(std::abs(signed_height));
    return true;
  }
  if (size >= 4 && data[0] == 0xff && data[1] == 0xd8) {
    size_t offset = 2;
    while (offset + 4 <= size) {
      if (data[offset++] != 0xff) return false;
      while (offset < size && data[offset] == 0xff) ++offset;
      if (offset >= size) return false;
      const unsigned char marker = data[offset++];
      if (marker == 0xd8 || marker == 0xd9) continue;
      if (offset + 2 > size) return false;
      const uint16_t length = be16(data + offset);
      if (length < 2 || offset + length > size) return false;
      if ((marker >= 0xc0 && marker <= 0xc3) ||
          (marker >= 0xc5 && marker <= 0xc7) ||
          (marker >= 0xc9 && marker <= 0xcb) ||
          (marker >= 0xcd && marker <= 0xcf)) {
        if (length < 7) return false;
        *height = be16(data + offset + 3); *width = be16(data + offset + 5);
        return true;
      }
      offset += length;
    }
  }
  return false;
}

Lardon3DDenseMvsStatus validate_snapshot(const Lardon3DDenseMvsInput &input) {
  const auto &snapshot = *input.snapshot;
  if (!input.source_observations ||
      input.source_observation_count != snapshot.observation_count)
    return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  if ((snapshot.camera_count && !snapshot.cameras) ||
      (snapshot.landmark_count && !snapshot.landmarks) ||
      (snapshot.observation_count && !snapshot.observations) ||
      snapshot.camera_count == 0 || snapshot.landmark_count == 0 ||
      snapshot.observation_count == 0 ||
      snapshot.camera_count > UINT32_MAX ||
      snapshot.landmark_count > UINT32_MAX)
    return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;

  std::map<uint64_t, uint64_t> camera_components;
  std::map<uint64_t, const Lardon3DSparseIncrementalLandmark *> landmarks;
  std::map<ObservationKey, const Lardon3DSparseIncrementalObservation *> sources;
  std::map<uint64_t, uint64_t> observed;
  std::set<std::pair<uint64_t, uint32_t>> positions;
  for (size_t index = 0; index < snapshot.camera_count; ++index) {
    const auto &camera = snapshot.cameras[index];
    if (!camera.image_id || !camera.component_key ||
        !camera_components.emplace(camera.image_id, camera.component_key).second)
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
    for (double value : camera.pose_cw.translation_cw)
      if (!std::isfinite(value)) return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
    if (!valid_rotation(camera.pose_cw.rotation_cw))
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
  }
  for (size_t index = 0; index < snapshot.landmark_count; ++index) {
    const auto &landmark = snapshot.landmarks[index];
    if (!landmark.landmark_id || !landmark.track_id ||
        landmark.observation_count == 0 || !finite_point(landmark.point) ||
        !std::isfinite(landmark.reprojection_rmse_px) ||
        !landmarks.emplace(landmark.landmark_id, &landmark).second)
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
  }
  for (size_t index = 0; index < input.source_observation_count; ++index) {
    const auto &observation = input.source_observations[index];
    if (!observation.image_id || !observation.track_id ||
        !observation.feature_set_id || !std::isfinite(observation.x) ||
        !std::isfinite(observation.y) ||
        !sources.emplace(ObservationKey{observation.feature_set_id,
                                        observation.feature_index},
                         &observation)
             .second)
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  }
  for (size_t index = 0; index < snapshot.observation_count; ++index) {
    const auto &observation = snapshot.observations[index];
    const auto landmark = landmarks.find(observation.landmark_id);
    const auto camera = camera_components.find(observation.image_id);
    const auto source = sources.find(
        {observation.feature_set_id, observation.feature_index});
    if (landmark == landmarks.end() || camera == camera_components.end() ||
        observation.track_id != landmark->second->track_id ||
        camera->second != landmark->second->component_key ||
        observation.position_in_track >= landmark->second->observation_count ||
        !positions.emplace(observation.track_id,
                           observation.position_in_track)
             .second)
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
    if (source == sources.end() ||
        source->second->image_id != observation.image_id ||
        source->second->track_id != observation.track_id)
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    ++observed[observation.landmark_id];
  }
  for (const auto &entry : landmarks)
    if (observed[entry.first] != entry.second->observation_count)
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
  return LARDON3D_DENSE_MVS_OK;
}

Lardon3DDenseMvsStatus validate_sources(const Lardon3DDenseMvsInput &input) {
  const auto &snapshot = *input.snapshot;
  if (!input.source_observations ||
      input.source_observation_count != snapshot.observation_count ||
      input.source_image_count != snapshot.camera_count)
    return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  std::set<uint64_t> camera_ids;
  for (size_t index = 0; index < snapshot.camera_count; ++index)
    camera_ids.insert(snapshot.cameras[index].image_id);
  std::set<uint64_t> source_ids;
  for (size_t index = 0; index < input.source_image_count; ++index) {
    const auto &image = input.source_images[index];
    if (!image.image_id || !image.source_path || !*image.source_path ||
        !finite_cal(image.calibration) ||
        !bounded_image_allocation(image.calibration.width,
                                  image.calibration.height) ||
        !source_ids.insert(image.image_id).second)
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    uint32_t encoded_width = 0, encoded_height = 0;
    if (!encoded_image_dimensions(image.source_path, &encoded_width,
                                  &encoded_height) ||
        !bounded_image_allocation(encoded_width, encoded_height) ||
        encoded_width != image.calibration.width ||
        encoded_height != image.calibration.height)
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  }
  if (source_ids != camera_ids) return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  return LARDON3D_DENSE_MVS_OK;
}

Lardon3DDenseMvsStatus export_colmap(const Lardon3DDenseMvsInput *in,
                                     const std::string &directory) {
  std::vector<const Lardon3DSparseIncrementalCamera *> cameras;
  for (size_t i = 0; i < in->snapshot->camera_count; ++i)
    cameras.push_back(in->snapshot->cameras + i);
  std::sort(cameras.begin(), cameras.end(), [](const auto *a, const auto *b) {
    return a->image_id < b->image_id;
  });
  if (cameras.size() != in->source_image_count)
    return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  std::vector<const Lardon3DSparseIncrementalLandmark *> landmarks;
  for (size_t index = 0; index < in->snapshot->landmark_count; ++index)
    landmarks.push_back(in->snapshot->landmarks + index);
  std::sort(landmarks.begin(), landmarks.end(), [](const auto *a, const auto *b) {
    return a->landmark_id < b->landmark_id;
  });
  std::map<uint64_t, uint32_t> point_ids;
  std::map<ObservationKey, const Lardon3DSparseIncrementalObservation *> source_coordinates;
  for (size_t index = 0; index < in->source_observation_count; ++index) {
    const auto &observation = in->source_observations[index];
    source_coordinates.emplace(
        ObservationKey{observation.feature_set_id, observation.feature_index},
        &observation);
  }
  for (size_t index = 0; index < landmarks.size(); ++index)
    point_ids.emplace(landmarks[index]->landmark_id, static_cast<uint32_t>(index + 1));

  std::map<uint64_t, std::vector<ExportObservation>> image_observations;
  for (size_t index = 0; index < in->snapshot->observation_count; ++index) {
    const auto *observation = in->snapshot->observations + index;
    const auto coordinate = source_coordinates.find(
        {observation->feature_set_id, observation->feature_index});
    image_observations[observation->image_id].push_back(
        {observation, coordinate->second, point_ids.at(observation->landmark_id), 0, 0});
  }
  for (auto &entry : image_observations)
    std::sort(entry.second.begin(), entry.second.end(), [](const auto &a, const auto &b) {
      if (a.point_id != b.point_id) return a.point_id < b.point_id;
      if (a.snapshot->feature_set_id != b.snapshot->feature_set_id)
        return a.snapshot->feature_set_id < b.snapshot->feature_set_id;
      return a.snapshot->feature_index < b.snapshot->feature_index;
    });

  const std::string sparse_directory = directory + "/sparse";
  const std::string image_directory = directory + "/images";
  if ((mkdir(sparse_directory.c_str(), 0700) != 0 && errno != EEXIST) ||
      (mkdir(image_directory.c_str(), 0700) != 0 && errno != EEXIST))
    return LARDON3D_DENSE_MVS_IO_ERROR;

  std::ofstream camera_file(sparse_directory + "/cameras.txt",
                            std::ios::out | std::ios::trunc);
  std::ofstream image_file(sparse_directory + "/images.txt",
                           std::ios::out | std::ios::trunc);
  std::ofstream point_file(sparse_directory + "/points3D.txt",
                           std::ios::out | std::ios::trunc);
  if (!camera_file || !image_file || !point_file)
    return LARDON3D_DENSE_MVS_IO_ERROR;
  for (std::ofstream *file : {&camera_file, &image_file, &point_file}) {
    file->imbue(std::locale::classic());
    *file << std::scientific
          << std::setprecision(std::numeric_limits<double>::max_digits10 - 1);
  }
  std::map<uint32_t, std::vector<std::pair<uint32_t, size_t>>> tracks;
  uint64_t id = 1;
  for (const auto *camera : cameras) {
    if (id > 1 && cameras[id - 2]->image_id == camera->image_id)
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
    const Lardon3DDenseMvsSourceImage *image = nullptr;
    for (size_t index = 0; index < in->source_image_count; ++index)
      if (in->source_images[index].image_id == camera->image_id)
        image = in->source_images + index;
    if (!image || !finite_cal(image->calibration))
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    double q[4];
    if (!quaternion(camera->pose_cw.rotation_cw, q))
      return LARDON3D_DENSE_MVS_INVALID_SNAPSHOT;
    const auto &k = image->calibration;
    if (!bounded_image_allocation(k.width, k.height))
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    uint32_t encoded_width = 0, encoded_height = 0;
    if (!encoded_image_dimensions(image->source_path, &encoded_width,
                                  &encoded_height) ||
        !bounded_image_allocation(encoded_width, encoded_height) ||
        encoded_width != k.width || encoded_height != k.height)
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    cv::Mat source_image;
    try {
      source_image = cv::imread(image->source_path, cv::IMREAD_COLOR);
    } catch (const cv::Exception &error) {
      if (error.code == cv::Error::StsNoMem) throw std::bad_alloc();
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    }
    if (source_image.empty() || source_image.cols != static_cast<int>(k.width) ||
        source_image.rows != static_cast<int>(k.height))
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    const cv::Mat camera_matrix = cv::Mat(cv::Matx33d(
        k.fx, 0, k.cx,
        0, k.fy, k.cy,
        0, 0, 1));
    const cv::Mat distortion = cv::Mat(cv::Matx<double, 1, 4>(
        k.k1, k.k2, k.p1, k.p2));
    cv::Mat undistorted;
    try {
      cv::undistort(source_image, undistorted, camera_matrix, distortion,
                    camera_matrix);
    } catch (const cv::Exception &error) {
      if (error.code == cv::Error::StsNoMem) throw std::bad_alloc();
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    }
    const std::string image_name = "image_" + std::to_string(id) + ".png";
    try {
      if (!cv::imwrite(image_directory + "/" + image_name, undistorted))
        return LARDON3D_DENSE_MVS_IO_ERROR;
    } catch (const cv::Exception &error) {
      if (error.code == cv::Error::StsNoMem) throw std::bad_alloc();
      return LARDON3D_DENSE_MVS_IO_ERROR;
    }

    auto &observations = image_observations[camera->image_id];
    std::vector<cv::Point2d> distorted_points;
    distorted_points.reserve(observations.size());
    for (const auto &observation : observations)
      distorted_points.emplace_back(observation.source->x, observation.source->y);
    std::vector<cv::Point2d> undistorted_points;
    if (!distorted_points.empty()) {
      try {
        cv::undistortPoints(distorted_points, undistorted_points, camera_matrix,
                            distortion, cv::noArray(), camera_matrix);
      } catch (const cv::Exception &error) {
        if (error.code == cv::Error::StsNoMem) throw std::bad_alloc();
        return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
      }
    }
    for (size_t index = 0; index < observations.size(); ++index) {
      observations[index].x = undistorted_points[index].x;
      observations[index].y = undistorted_points[index].y;
      if (!std::isfinite(observations[index].x) ||
          !std::isfinite(observations[index].y))
        return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
    }

    /* InterfaceCOLMAP subtracts 0.5 from COLMAP's principal point. */
    camera_file << id << " PINHOLE " << k.width << ' ' << k.height << ' '
                << k.fx << ' ' << k.fy << ' ' << k.cx + 0.5 << ' '
                << k.cy + 0.5 << '\n';
    image_file << id << ' ' << q[0] << ' ' << q[1] << ' ' << q[2] << ' ' << q[3]
               << ' ' << camera->pose_cw.translation_cw[0] << ' '
               << camera->pose_cw.translation_cw[1] << ' '
               << camera->pose_cw.translation_cw[2] << ' ' << id << ' '
               << image_name << '\n';
    for (size_t index = 0; index < observations.size(); ++index) {
      const auto &observation = observations[index];
      image_file << observation.x << ' ' << observation.y << ' '
                 << observation.point_id << ' ';
      tracks[observation.point_id].emplace_back(static_cast<uint32_t>(id), index);
    }
    image_file << '\n';
    if (!camera_file || !image_file) return LARDON3D_DENSE_MVS_IO_ERROR;
    ++id;
  }

  for (const auto *landmark : landmarks) {
    const uint32_t point_id = point_ids.at(landmark->landmark_id);
    point_file << point_id << ' ' << landmark->point.x << ' ' << landmark->point.y
               << ' ' << landmark->point.z << " 0 0 0 "
               << landmark->reprojection_rmse_px;
    const auto track = tracks.find(point_id);
    if (track != tracks.end())
      for (const auto &member : track->second)
        point_file << ' ' << member.first << ' ' << member.second;
    point_file << '\n';
    if (!point_file) return LARDON3D_DENSE_MVS_IO_ERROR;
  }
  if (!finalize(camera_file) || !finalize(image_file) || !finalize(point_file))
    return LARDON3D_DENSE_MVS_IO_ERROR;
  return LARDON3D_DENSE_MVS_OK;
}

bool bounded_line(std::ifstream &file, std::string &line, bool *had_newline,
                  size_t *raw_size) {
  line.clear();
  *raw_size = 0;
  char value = 0;
  while (file.get(value)) {
    if (*raw_size == SIZE_MAX) return false;
    ++*raw_size;
    if (value == '\n') {
      *had_newline = true;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line.find('\r') == std::string::npos;
    }
    if (line.size() == kMaxPlyLine) return false;
    line.push_back(value);
  }
  *had_newline = false;
  return file.eof() && !line.empty() && line.find('\r') == std::string::npos;
}

bool bounded_header_line(std::ifstream &file, std::string &line, size_t &total) {
  bool had_newline = false;
  size_t raw_size = 0;
  if (!bounded_line(file, line, &had_newline, &raw_size)) return false;
  if (total > kMaxPlyHeader || raw_size > kMaxPlyHeader - total) return false;
  total += raw_size;
  return true;
}
bool ply_data_line(std::ifstream &file, std::string &line) {
  bool had_newline = false;
  size_t raw_size = 0;
  return bounded_line(file, line, &had_newline, &raw_size);
}

enum class PlyScalar {
  I8, U8, I16, U16, I32, U32, F32, F64
};

struct PlyProperty {
  std::string name;
  PlyScalar scalar{};
  PlyScalar count_scalar{};
  size_t scalar_size = 0;
  size_t count_size = 0;
  bool list = false;
};

bool ply_integer(PlyScalar scalar) {
  return scalar != PlyScalar::F32 && scalar != PlyScalar::F64;
}

bool ply_scalar(const std::string &name, PlyScalar *scalar, size_t *size) {
  if (name == "char" || name == "int8") {
    *scalar = PlyScalar::I8;
    *size = 1;
  } else if (name == "uchar" || name == "uint8") {
    *scalar = PlyScalar::U8;
    *size = 1;
  } else if (name == "short" || name == "int16") {
    *scalar = PlyScalar::I16;
    *size = 2;
  } else if (name == "ushort" || name == "uint16") {
    *scalar = PlyScalar::U16;
    *size = 2;
  } else if (name == "int" || name == "int32") {
    *scalar = PlyScalar::I32;
    *size = 4;
  } else if (name == "uint" || name == "uint32") {
    *scalar = PlyScalar::U32;
    *size = 4;
  } else if (name == "float" || name == "float32") {
    *scalar = PlyScalar::F32;
    *size = 4;
  } else if (name == "double" || name == "float64") {
    *scalar = PlyScalar::F64;
    *size = 8;
  } else {
    return false;
  }
  return true;
}

bool read_little_endian(std::ifstream &file, PlyScalar scalar, double *value) {
  size_t size = 0;
  PlyScalar ignored{};
  const char *name = scalar == PlyScalar::I8 ? "int8" :
                     scalar == PlyScalar::U8 ? "uint8" :
                     scalar == PlyScalar::I16 ? "int16" :
                     scalar == PlyScalar::U16 ? "uint16" :
                     scalar == PlyScalar::I32 ? "int32" :
                     scalar == PlyScalar::U32 ? "uint32" :
                     scalar == PlyScalar::F32 ? "float32" : "float64";
  if (!ply_scalar(name, &ignored, &size)) return false;
  unsigned char bytes[8]{};
  if (!file.read(reinterpret_cast<char *>(bytes), static_cast<std::streamsize>(size)))
    return false;
  uint64_t bits = 0;
  for (size_t index = 0; index < size; ++index)
    bits |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
  switch (scalar) {
    case PlyScalar::I8:
      *value = static_cast<int8_t>(bytes[0]);
      break;
    case PlyScalar::U8:
      *value = bytes[0];
      break;
    case PlyScalar::I16:
      *value = static_cast<int16_t>(static_cast<uint16_t>(bits));
      break;
    case PlyScalar::U16:
      *value = static_cast<uint16_t>(bits);
      break;
    case PlyScalar::I32:
      *value = static_cast<int32_t>(static_cast<uint32_t>(bits));
      break;
    case PlyScalar::U32:
      *value = static_cast<uint32_t>(bits);
      break;
    case PlyScalar::F32: {
      const uint32_t raw = static_cast<uint32_t>(bits);
      float decoded = 0;
      std::memcpy(&decoded, &raw, sizeof decoded);
      *value = decoded;
      break;
    }
    case PlyScalar::F64: {
      double decoded = 0;
      std::memcpy(&decoded, &bits, sizeof decoded);
      *value = decoded;
      break;
    }
  }
  return true;
}

bool ply(const std::string &path, uint64_t *point_count) {
  struct stat status {};
  if (!point_count || stat(path.c_str(), &status) || !S_ISREG(status.st_mode) ||
      status.st_size <= 0) return false;
  std::ifstream file(path, std::ios::in | std::ios::binary);
  std::string line;
  size_t header_size = 0;
  if (!bounded_header_line(file, line, header_size) || line != "ply" ||
      !bounded_header_line(file, line, header_size)) return false;
  const bool ascii = line == "format ascii 1.0";
  const bool binary_little_endian = line == "format binary_little_endian 1.0";
  if (!ascii && !binary_little_endian) return false;
  bool vertex_element = false;
  bool header_complete = false;
  uint64_t vertices = 0;
  std::vector<std::vector<std::string>> properties;
  while (bounded_header_line(file, line, header_size)) {
    if (line == "end_header") { header_complete = true; break; }
    std::istringstream fields(line);
    fields.imbue(std::locale::classic());
    std::string kind, type, name;
    if (!(fields >> kind)) return false;
    if (kind == "comment" || kind == "obj_info") continue;
    if (kind == "element") {
      if (vertex_element || !(fields >> type >> name) || type != "vertex") return false;
      char *end = nullptr;
      errno = 0;
      const unsigned long long parsed = strtoull(name.c_str(), &end, 10);
      if (errno || !end || *end || parsed == 0) return false;
      vertices = parsed;
      vertex_element = true;
      continue;
    }
    if (kind == "property") {
      if (!vertex_element || !(fields >> type) || properties.size() >= kMaxPlyProperties)
        return false;
      if (type == "list") {
        std::string count_type, item_type;
        if (ascii || !(fields >> count_type >> item_type >> name)) return false;
        properties.push_back({type, count_type, item_type, name});
      } else {
        if (!(fields >> name)) return false;
        properties.push_back({type, name});
      }
      continue;
    }
    return false;
  }
  int xi = -1, yi = -1, zi = -1;
  std::vector<PlyProperty> decoded_properties;
  for (size_t i = 0; i < properties.size(); ++i) {
    PlyProperty property;
    property.list = properties[i][0] == "list";
    property.name = property.list ? properties[i][3] : properties[i][1];
    const std::string &item_type = property.list ? properties[i][2] : properties[i][0];
    if (!ply_scalar(item_type, &property.scalar, &property.scalar_size)) return false;
    if (property.list &&
        (!ply_scalar(properties[i][1], &property.count_scalar, &property.count_size) ||
         !ply_integer(property.count_scalar)))
      return false;
    decoded_properties.push_back(property);
    int *index = property.name == "x" ? &xi : property.name == "y" ? &yi :
                 property.name == "z" ? &zi : nullptr;
    if (index) {
      if (*index >= 0 || property.list ||
          (item_type != "float" && item_type != "float32" && item_type != "double" &&
           item_type != "float64"))
        return false;
      *index = static_cast<int>(i);
    }
  }
  if (!header_complete || !vertex_element || xi < 0 || yi < 0 || zi < 0 ||
      properties.empty()) return false;
  uint64_t remaining = 0;
  if (binary_little_endian) {
    const std::streampos body_offset = file.tellg();
    if (body_offset < 0 || static_cast<uint64_t>(body_offset) >
                               static_cast<uint64_t>(status.st_size))
      return false;
    remaining = static_cast<uint64_t>(status.st_size) - static_cast<uint64_t>(body_offset);
    uint64_t minimum_vertex_size = 0;
    for (const PlyProperty &property : decoded_properties) {
      const size_t property_minimum = property.list ? property.count_size : property.scalar_size;
      if (property_minimum > UINT64_MAX - minimum_vertex_size) return false;
      minimum_vertex_size += property_minimum;
    }
    if (minimum_vertex_size == 0 || vertices > remaining / minimum_vertex_size) return false;
  }
  for (uint64_t vertex = 0; vertex < vertices; ++vertex) {
    if (ascii) {
      if (!ply_data_line(file, line)) return false;
      std::istringstream fields(line);
      fields.imbue(std::locale::classic());
      for (size_t index = 0; index < decoded_properties.size(); ++index) {
        double value = 0;
        if (!(fields >> value) || !std::isfinite(value))
          return false;
      }
      std::string extra;
      if (fields >> extra) return false;
    } else {
      for (size_t index = 0; index < decoded_properties.size(); ++index) {
        const PlyProperty &property = decoded_properties[index];
        if (property.list) {
          double count_value = 0;
          if (property.count_size > remaining ||
              !read_little_endian(file, property.count_scalar, &count_value) ||
              count_value < 0)
            return false;
          remaining -= property.count_size;
          const uint64_t count = static_cast<uint64_t>(count_value);
          if (count > remaining / property.scalar_size) return false;
          const uint64_t bytes = count * property.scalar_size;
          if (bytes > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
              !file.seekg(static_cast<std::streamoff>(bytes), std::ios::cur))
            return false;
          remaining -= bytes;
          continue;
        }
        double value = 0;
        if (property.scalar_size > remaining ||
            !read_little_endian(file, property.scalar, &value) ||
            ((static_cast<int>(index) == xi || static_cast<int>(index) == yi ||
              static_cast<int>(index) == zi) && !std::isfinite(value)))
          return false;
        remaining -= property.scalar_size;
      }
    }
  }
  if (binary_little_endian)
    return remaining == 0 && file.peek() == std::char_traits<char>::eof() &&
           (*point_count = vertices, true);
  char trailing;
  bool pending_cr = false;
  while (file.get(trailing)) {
    if (pending_cr) {
      if (trailing != '\n') return false;
      pending_cr = false;
    } else if (trailing == '\r') {
      pending_cr = true;
    } else if (!std::isspace(static_cast<unsigned char>(trailing))) {
      return false;
    }
  }
  if (pending_cr) return false;
  *point_count = vertices;
  return true;
}
Lardon3DDenseMvsStatus run_impl(const Lardon3DDenseMvsInput *input,
                                Lardon3DDenseMvsResult *result) {
  if (!input || !result || !input->snapshot || !input->source_images ||
      !input->source_image_count || !input->staging_directory ||
      !input->execution_thread_count || !input->interface_colmap_executable ||
      !input->densify_point_cloud_executable)
    return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;
  if (input->parameters.fusion_mode != 0)
    return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;

  unsigned char source_identity[32], calibration_binding_identity[32];
  unsigned char parameter_fingerprint[32], backend_identity[32];
  if (!lardon3d_dense_mvs_parameter_fingerprint(
          &input->parameters, parameter_fingerprint))
    return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;
  if (!lardon3d::dense_mvs_detail::source_image_set_identity(
          input->source_images, input->source_image_count, source_identity))
    return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  if (!lardon3d::dense_mvs_detail::calibration_binding_identity(
          input->source_images, input->source_image_count,
          calibration_binding_identity))
    return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;

  const Lardon3DDenseMvsStatus snapshot_status = validate_snapshot(*input);
  if (snapshot_status != LARDON3D_DENSE_MVS_OK) return snapshot_status;
  const Lardon3DDenseMvsStatus source_status = validate_sources(*input);
  if (source_status != LARDON3D_DENSE_MVS_OK) return source_status;
  for (size_t index = 0; index < input->source_image_count; ++index) {
    unsigned char actual[32];
    const auto &image = input->source_images[index];
    if (!sha_regular_file(image.source_path, actual, kMaxSourceFileBytes) ||
        std::memcmp(actual, image.immutable_sha256, sizeof actual) != 0)
      return LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  }

  if (!executable(input->interface_colmap_executable) ||
      !executable(input->densify_point_cloud_executable))
    return LARDON3D_DENSE_MVS_BACKEND_ERROR;

  BackendCapabilities interface_capabilities{};
  BackendCapabilities densify_capabilities{};
  if (!probe(input->interface_colmap_executable, true, &interface_capabilities) ||
      !probe(input->densify_point_cloud_executable, false, &densify_capabilities))
    return LARDON3D_DENSE_MVS_BACKEND_ERROR;

  Lardon3DDenseMvsBackendManifest manifest{};
  uint64_t remaining_backend_hash_bytes = kMaxBackendFileBytes;
  if (!sha_regular_file(input->interface_colmap_executable,
                        manifest.interface_colmap_binary_sha256,
                        kMaxBackendFileBytes, &remaining_backend_hash_bytes) ||
      !sha_regular_file(input->densify_point_cloud_executable,
                        manifest.densify_point_cloud_binary_sha256,
                        kMaxBackendFileBytes, &remaining_backend_hash_bytes))
    return LARDON3D_DENSE_MVS_BACKEND_ERROR;
  std::memcpy(manifest.interface_colmap_version_identity,
              interface_capabilities.version_identity, 32);
  std::memcpy(manifest.densify_point_cloud_version_identity,
              densify_capabilities.version_identity, 32);
  if (!lardon3d_dense_mvs_backend_manifest_digest(&manifest, backend_identity))
    return LARDON3D_DENSE_MVS_BACKEND_ERROR;

  Lardon3DDenseMvsIdentity identity{};
  std::memcpy(identity.base_reconstruction_identity,
              input->base_reconstruction_identity, 32);
  std::memcpy(identity.source_image_set_identity, source_identity, 32);
  std::memcpy(identity.calibration_scope_identity,
              input->calibration_scope_identity, 32);
  std::memcpy(identity.calibration_binding_identity,
              calibration_binding_identity, 32);
  identity.dense_kind = identity.backend_kind = LARDON3D_DENSE_MVS_KIND_OPENMVS;
  identity.dense_version = identity.backend_version = LARDON3D_DENSE_MVS_VERSION;
  std::memcpy(identity.backend_binary_sha256, backend_identity, 32);
  std::memcpy(identity.parameter_fingerprint, parameter_fingerprint, 32);
  unsigned char dense_identity[32];
  if (!lardon3d_dense_mvs_identity_digest(&identity, dense_identity))
    return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;

  const std::string staging(input->staging_directory);
  constexpr char workspace_suffix[] = "/lardon3d-mvs-XXXXXX";
  constexpr char cloud_suffix[] = "/dense.ply";
  if (staging.size() > SIZE_MAX - sizeof workspace_suffix ||
      staging.size() + sizeof workspace_suffix - 1 >
          SIZE_MAX - sizeof cloud_suffix ||
      staging.size() + sizeof workspace_suffix - 1 + sizeof cloud_suffix - 1 >=
          sizeof result->point_cloud_path)
    return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;
  if (mkdir(staging.c_str(), 0700) != 0 && errno != EEXIST)
    return LARDON3D_DENSE_MVS_IO_ERROR;
  std::vector<char> workspace_template(staging.begin(), staging.end());
  workspace_template.insert(workspace_template.end(), workspace_suffix,
                            workspace_suffix + sizeof workspace_suffix);
  char *created = mkdtemp(workspace_template.data());
  if (!created) return LARDON3D_DENSE_MVS_IO_ERROR;
  const std::string root(created);
  const std::string workspace = root + "/colmap";
  const std::string scene = root + "/scene.mvs";
  const std::string cloud_stem = root + "/dense";
  const std::string cloud = cloud_stem + ".ply";
  if (mkdir(workspace.c_str(), 0700) != 0)
    return LARDON3D_DENSE_MVS_IO_ERROR;
  const Lardon3DDenseMvsStatus export_status = export_colmap(input, workspace);
  if (export_status != LARDON3D_DENSE_MVS_OK) return export_status;
  if (!run({input->interface_colmap_executable, "-i", workspace, "-o", scene,
            "--max-threads", std::to_string(input->execution_thread_count)},
           root, "interface-colmap.log"))
    return LARDON3D_DENSE_MVS_BACKEND_ERROR;
  std::vector<std::string> densify_arguments = {
      input->densify_point_cloud_executable,
      scene,
      "-o",
      cloud_stem,
      "--resolution-level",
      std::to_string(input->parameters.resolution_level),
      "--min-resolution",
      std::to_string(input->parameters.minimum_resolution),
      "--number-views",
      std::to_string(input->parameters.number_views),
      "--fusion-mode",
      std::to_string(input->parameters.fusion_mode),
      "--max-threads",
      std::to_string(input->execution_thread_count),
  };
  if (densify_capabilities.cuda_option) {
    densify_arguments.emplace_back("--cuda-device");
    densify_arguments.emplace_back("-2");
  }
  if (!run(densify_arguments, root, "densify-point-cloud.log"))
    return LARDON3D_DENSE_MVS_BACKEND_ERROR;

  if (!ply(cloud, &result->point_count)) return LARDON3D_DENSE_MVS_INVALID_OUTPUT;
  std::memcpy(result->dense_identity, dense_identity, 32);
  std::memcpy(result->parameter_fingerprint, parameter_fingerprint, 32);
  std::memcpy(result->base_reconstruction_identity,
              input->base_reconstruction_identity, 32);
  std::memcpy(result->source_image_set_identity, source_identity, 32);
  std::memcpy(result->backend_implementation_sha256, backend_identity, 32);
  const int copied = std::snprintf(result->point_cloud_path,
                                   sizeof result->point_cloud_path, "%s", cloud.c_str());
  if (copied < 0 || static_cast<size_t>(copied) >= sizeof result->point_cloud_path)
    return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;
  return LARDON3D_DENSE_MVS_OK;
}
}

extern "C" Lardon3DDenseMvsStatus lardon3d_dense_mvs_run(
    const Lardon3DDenseMvsInput *input, Lardon3DDenseMvsResult *result) {
  if (!result) return LARDON3D_DENSE_MVS_INVALID_ARGUMENT;
  Lardon3DDenseMvsResult candidate{};
  Lardon3DDenseMvsStatus status;
  try {
    status = run_impl(input, &candidate);
  } catch (const std::bad_alloc &) {
    status = LARDON3D_DENSE_MVS_OUT_OF_MEMORY;
  } catch (const cv::Exception &error) {
    status = error.code == cv::Error::StsNoMem
                 ? LARDON3D_DENSE_MVS_OUT_OF_MEMORY
                 : LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE;
  } catch (...) {
    status = LARDON3D_DENSE_MVS_IO_ERROR;
  }
  if (status == LARDON3D_DENSE_MVS_OK) {
    candidate.status = status;
    *result = candidate;
  } else {
    std::memset(result, 0, sizeof *result);
    result->status = status;
  }
  return status;
}

extern "C" void lardon3d_dense_mvs_result_destroy(Lardon3DDenseMvsResult *result) {
  if (result) std::memset(result, 0, sizeof *result);
}
