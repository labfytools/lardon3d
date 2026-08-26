#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <locale>
#include <lardon3d/dense_mvs.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/calib3d.hpp>
#include <openssl/evp.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                     \
      return false;                                                            \
    }                                                                          \
  } while (0)
namespace {
const char *good_ply =
    "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float "
    "y\nproperty float z\nend_header\n1 2 3\n4 5 6\n";
bool put(const std::string &p, const std::string &s, bool x = true) {
  FILE *f = fopen(p.c_str(), "wb");
  if (!f)
    return false;
  bool ok = fwrite(s.data(), 1, s.size(), f) == s.size() && !fclose(f);
  return ok && (!x || !chmod(p.c_str(), 0700));
}
std::string shell_octal(const std::string &data) {
  static const char digits[] = "01234567";
  std::string escaped;
  escaped.reserve(data.size() * 4);
  for (unsigned char value : data) {
    escaped.push_back('\\');
    escaped.push_back(digits[(value >> 6U) & 7U]);
    escaped.push_back(digits[(value >> 3U) & 7U]);
    escaped.push_back(digits[value & 7U]);
  }
  return escaped;
}
std::string script(const std::string &v, const std::string &body) {
  const bool densify = v.find("Densify") != std::string::npos || v == "D";
  const std::string options = densify
                                  ? "--max-threads --resolution-level "
                                    "--min-resolution --number-views --fusion-mode" +
                                        std::string(v.find("CUDA") != std::string::npos
                                                        ? " --cuda-device"
                                                        : "")
                                  : "--max-threads --image-folder";
  return "#!/bin/sh\nif [ \"$1\" = \"--help\" ]; then printf '%s\\n' "
         "'OpenMVS x64 v2.4.0 " + options + "'; exit 1; fi\n" + body;
}
struct F {
  std::string dir, ic, dp, log, image;
  Lardon3DSparseIncrementalCamera cam{};
  Lardon3DSparseIncrementalLandmark landmark{};
  Lardon3DSparseIncrementalLandmarkObservation observation{};
  Lardon3DSparseIncrementalObservation source_observation{};
  Lardon3DSparseIncrementalResult snap{};
  Lardon3DDenseMvsSourceImage src{};
  Lardon3DDenseMvsInput in{};
  F() {
    char p[] = "/tmp/l3d-mvs-XXXXXX";
    char *d = mkdtemp(p);
    if (!d)
      return;
    dir = d;
    ic = dir + "/InterfaceCOLMAP";
    dp = dir + "/DensifyPointCloud";
    log = dir + "/argv";
    image = dir + "/image.jpg";
    cam.image_id = 17;
    cam.component_key = 1;
    double r[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(cam.pose_cw.rotation_cw, r, sizeof r);
    snap.cameras = &cam;
    snap.camera_count = 1;
    landmark.landmark_id = 23;
    landmark.track_id = 29;
    landmark.component_key = 1;
    landmark.point = {1, 2, 3};
    landmark.reprojection_rmse_px = 0.25;
    landmark.observation_count = 1;
    snap.landmarks = &landmark;
    snap.landmark_count = 1;
    observation = {23, 29, 17, 31, 37, 0};
    snap.observations = &observation;
    snap.observation_count = 1;
    source_observation = {29, 17, 31, 37, 40, 20.25, 10.5};
    src.image_id = 17;
    src.source_path = image.c_str();
    src.calibration = {64, 48, 50, 51, 32, 24, 0, 0, 0, 0};
    for (size_t i = 0; i < 32; i++) {
      in.base_reconstruction_identity[i] = static_cast<unsigned char>(i + 1U);
      in.calibration_scope_identity[i] = static_cast<unsigned char>(0x40U + i);
    }
    in.snapshot = &snap;
    in.source_observations = &source_observation;
    in.source_observation_count = 1;
    in.source_images = &src;
    in.source_image_count = 1;
    in.parameters = {1, 320, 4, 0};
    in.execution_thread_count = 3;
    cv::Mat pixels(48, 64, CV_8UC3, cv::Scalar(20, 40, 60));
    if (!cv::imwrite(image, pixels)) return;
    FILE *image_file = fopen(image.c_str(), "rb");
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned int digest_size = 0;
    unsigned char buffer[4096];
    bool hashed = image_file && context &&
                  EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    while (hashed) {
      const size_t size = fread(buffer, 1, sizeof buffer, image_file);
      if (size && EVP_DigestUpdate(context, buffer, size) != 1) hashed = false;
      if (size < sizeof buffer) {
        hashed = hashed && !ferror(image_file);
        break;
      }
    }
    hashed = hashed &&
             EVP_DigestFinal_ex(context, src.immutable_sha256, &digest_size) == 1 &&
             digest_size == 32;
    EVP_MD_CTX_free(context);
    if (image_file) fclose(image_file);
    if (!hashed)
      return;
    sync();
  }
  void sync() {
    src.source_path = image.c_str();
    in.staging_directory = dir.c_str();
    in.interface_colmap_executable = ic.c_str();
    in.densify_point_cloud_executable = dp.c_str();
  }
};
bool install(F &f, const std::string &iv = "Interface 1",
             const std::string &dv = "Densify 1",
             const std::string &ply = good_ply) {
  std::string b =
      "if [ -n \"$DENSE_ARGV_LOG\" ]; then : > \"$DENSE_ARGV_LOG\"; for a in "
      "\"$@\"; do printf '%s\\n' \"$a\" >> \"$DENSE_ARGV_LOG\"; done; "
      "fi\no=\nwhile [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then shift; o=$1; "
      "fi; shift; done\nprintf '%b' '" +
      shell_octal(ply) + "' > \"$o.ply\"\n";
  return put(f.ic, script(iv, "exit 0\n")) && put(f.dp, script(dv, b));
}
Lardon3DDenseMvsStatus run(F &f, Lardon3DDenseMvsResult &r) {
  f.sync();
  return lardon3d_dense_mvs_run(&f.in, &r);
}
bool zero(const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i])
      return false;
  return true;
}
std::string result_workspace(const Lardon3DDenseMvsResult &result) {
  const std::string path(result.point_cloud_path);
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
std::vector<std::string> private_workspaces(const std::string &parent) {
  std::vector<std::string> paths;
  DIR *directory = opendir(parent.c_str());
  if (!directory) return paths;
  while (dirent *entry = readdir(directory)) {
    if (std::strncmp(entry->d_name, "lardon3d-mvs-", 13) == 0)
      paths.push_back(parent + "/" + entry->d_name);
  }
  closedir(directory);
  std::sort(paths.begin(), paths.end());
  return paths;
}
bool digest_file(const std::string &path, unsigned char digest[32]) {
  FILE *file = fopen(path.c_str(), "rb");
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  bool ok = file && context &&
            EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
  unsigned char buffer[4096];
  while (ok) {
    const size_t size = fread(buffer, 1, sizeof buffer, file);
    if (size && EVP_DigestUpdate(context, buffer, size) != 1) ok = false;
    if (size < sizeof buffer) { ok = ok && !ferror(file); break; }
  }
  unsigned int length = 0;
  ok = ok && EVP_DigestFinal_ex(context, digest, &length) == 1 && length == 32;
  EVP_MD_CTX_free(context);
  if (file && fclose(file) != 0) ok = false;
  return ok;
}

class CommaNumpunct : public std::numpunct<char> {
protected:
  char do_decimal_point() const override { return ','; }
};

class GlobalLocaleGuard {
public:
  GlobalLocaleGuard()
      : previous_(std::locale::global(
            std::locale(std::locale::classic(), new CommaNumpunct))) {}
  ~GlobalLocaleGuard() { std::locale::global(previous_); }

  GlobalLocaleGuard(const GlobalLocaleGuard &) = delete;
  GlobalLocaleGuard &operator=(const GlobalLocaleGuard &) = delete;

private:
  std::locale previous_;
};

bool goldens() {
  static const unsigned char pr[40] = {
      0x4c, 0x33, 0x44, 0x4d, 0x50, 0x52, 0x4d, 0x31, 1, 0, 0, 0, 1, 0,
      0,    0,    1,    0,    0,    0,    0x40, 1,    0, 0, 4, 0, 0, 0,
      1,    0,    0,    0,    1,    0,    0,    0,    1, 0, 0, 0};
  static const unsigned char pd[32] = {
      0x03, 0x82, 0x06, 0xe5, 0xe8, 0x72, 0x9e, 0x13, 0x87, 0x14, 0xc6,
      0xb4, 0xcc, 0x8b, 0xe4, 0x8c, 0x78, 0xa9, 0x94, 0xcf, 0x83, 0x3d,
      0xc0, 0x2a, 0xca, 0x89, 0xd6, 0xc1, 0x49, 0xd5, 0xdd, 0xe4};
  static const unsigned char br[148] = {
      0x4c, 0x33, 0x44, 0x4d, 0x42, 0x4b, 0x44, 0x31, 1,   0,   0,   0,   1,
      0,    0,    0,    1,    2,    3,    4,    5,    6,   7,   8,   9,   10,
      11,   12,   13,   14,   15,   16,   17,   18,   19,  20,  21,  22,  23,
      24,   25,   26,   27,   28,   29,   30,   31,   32,  33,  34,  35,  36,
      37,   38,   39,   40,   41,   42,   43,   44,   45,  46,  47,  48,  49,
      50,   51,   52,   53,   54,   55,   56,   57,   58,  59,  60,  61,  62,
      63,   64,   2,    0,    0,    0,    65,   66,   67,  68,  69,  70,  71,
      72,   73,   74,   75,   76,   77,   78,   79,   80,  81,  82,  83,  84,
      85,   86,   87,   88,   89,   90,   91,   92,   93,  94,  95,  96,  97,
      98,   99,   100,  101,  102,  103,  104,  105,  106, 107, 108, 109, 110,
      111,  112,  113,  114,  115,  116,  117,  118,  119, 120, 121, 122, 123,
      124,  125,  126,  127,  128};
  static const unsigned char bd[32] = {
      0x7d, 0xdb, 0xe5, 0xc5, 0x13, 0x7f, 0x71, 0x5d, 0x4a, 0xe7, 7,
      0xc8, 0xa4, 0xc9, 0x84, 0xf6, 0xa2, 0xe9, 0x76, 0x60, 0x52, 0xa7,
      0x60, 0x78, 0x1d, 0x5e, 0x24, 0xdd, 0xf3, 0x31, 0xae, 0x18};
  static const unsigned char ir[220] = {
      0x4c, 0x33, 0x44, 0x4d, 0x44, 0x49, 0x44, 0x32, 2, 0, 0, 0, 1, 0, 0, 0,
      1,    0,    0,    0,    1,    0,    0,    0,    1, 0, 0, 0, 1, 1, 1, 1,
      1,    1,    1,    1,    1,    1,    1,    1,    1, 1, 1, 1, 1, 1, 1, 1,
      1,    1,    1,    1,    1,    1,    1,    1,    1, 1, 1, 1, 2, 2, 2, 2,
      2,    2,    2,    2,    2,    2,    2,    2,    2, 2, 2, 2, 2, 2, 2, 2,
      2,    2,    2,    2,    2,    2,    2,    2,    2, 2, 2, 2, 3, 3, 3, 3,
      3,    3,    3,    3,    3,    3,    3,    3,    3, 3, 3, 3, 3, 3, 3, 3,
      3,    3,    3,    3,    3,    3,    3,    3,    3, 3, 3, 3, 6, 6, 6, 6,
      6,    6,    6,    6,    6,    6,    6,    6,    6, 6, 6, 6, 6, 6, 6, 6,
      6,    6,    6,    6,    6,    6,    6,    6,    6, 6, 6, 6, 4, 4, 4, 4,
      4,    4,    4,    4,    4,    4,    4,    4,    4, 4, 4, 4, 4, 4, 4, 4,
      4,    4,    4,    4,    4,    4,    4,    4,    4, 4, 4, 4, 5, 5, 5, 5,
      5,    5,    5,    5,    5,    5,    5,    5,    5, 5, 5, 5, 5, 5, 5, 5,
      5,    5,    5,    5,    5,    5,    5,    5,    5, 5, 5, 5};
  static const unsigned char id[32] = {
      0x61, 0x1c, 0x54, 0x2c, 0x4f, 0xe6, 0xf9, 0x1d, 0x8d, 0xf2, 0xea,
      0x77, 0x8e, 0xe8, 0xbe, 0x70, 0x8e, 0xb4, 0x81, 0x36, 0xf9, 0x7b,
      0xae, 0xaf, 0xe8, 0x45, 0xc0, 0x17, 0x93, 0x2d, 0x04, 0x90};
  unsigned char r[220]{}, d[32]{};
  Lardon3DDenseMvsParameters p{1, 320, 4, 1};
  CHECK(lardon3d_dense_mvs_parameter_fingerprint_record(&p, r) &&
        !memcmp(r, pr, 40));
  CHECK(lardon3d_dense_mvs_parameter_fingerprint(&p, d) && !memcmp(d, pd, 32));
  Lardon3DDenseMvsBackendManifest m{};
  for (size_t i = 0; i < 32; i++) {
    m.interface_colmap_version_identity[i] = static_cast<unsigned char>(i + 1U);
    m.interface_colmap_binary_sha256[i] = static_cast<unsigned char>(i + 33U);
    m.densify_point_cloud_version_identity[i] =
        static_cast<unsigned char>(i + 65U);
    m.densify_point_cloud_binary_sha256[i] =
        static_cast<unsigned char>(i + 97U);
  }
  CHECK(lardon3d_dense_mvs_backend_manifest_record(&m, r) &&
        !memcmp(r, br, 148));
  CHECK(lardon3d_dense_mvs_backend_manifest_digest(&m, d) &&
        !memcmp(d, bd, 32));
  Lardon3DDenseMvsIdentity x{};
  memset(x.base_reconstruction_identity, 1, 32);
  memset(x.source_image_set_identity, 2, 32);
  memset(x.calibration_scope_identity, 3, 32);
  memset(x.calibration_binding_identity, 6, 32);
  memset(x.backend_binary_sha256, 4, 32);
  memset(x.parameter_fingerprint, 5, 32);
  x.dense_kind = x.dense_version = x.backend_kind = x.backend_version = 1;
  CHECK(lardon3d_dense_mvs_identity_record(&x, r));
  for (size_t i = 0; i < 220; ++i)
    if (r[i] != ir[i]) {
      std::fprintf(stderr, "identity mismatch %zu: %u != %u\n", i,
                   static_cast<unsigned int>(r[i]),
                   static_cast<unsigned int>(ir[i]));
      return false;
    }
  CHECK(lardon3d_dense_mvs_identity_digest(&x, d) && !memcmp(d, id, 32));
  return true;
}

bool calibration_identity_contract() {
  F f;
  static const unsigned char expected[32] = {
      0xba, 0x29, 0x68, 0x86, 0x8b, 0x24, 0x5b, 0x84, 0x1d, 0xad, 0xfe,
      0x69, 0x84, 0xa1, 0x2d, 0x79, 0xe3, 0x18, 0x82, 0x9a, 0xde, 0x0e,
      0x9f, 0x94, 0x0f, 0xb7, 0x59, 0x4c, 0x81, 0x5c, 0xb5, 0x76};
  unsigned char base_binding[32], base_source[32];
  CHECK(lardon3d_dense_mvs_calibration_binding_identity(&f.src, 1,
                                                         base_binding) &&
        !memcmp(base_binding, expected, 32));
  CHECK(lardon3d_dense_mvs_source_image_set_identity(&f.src, 1, base_source));

  auto dense_digest = [&](const Lardon3DDenseMvsSourceImage &image,
                          unsigned char digest[32]) {
    Lardon3DDenseMvsIdentity identity{};
    memset(identity.base_reconstruction_identity, 1, 32);
    memset(identity.source_image_set_identity, 2, 32);
    memset(identity.calibration_scope_identity, 3, 32);
    memset(identity.backend_binary_sha256, 4, 32);
    memset(identity.parameter_fingerprint, 5, 32);
    identity.dense_kind = identity.dense_version = 1;
    identity.backend_kind = identity.backend_version = 1;
    return lardon3d_dense_mvs_calibration_binding_identity(
               &image, 1, identity.calibration_binding_identity) &&
           lardon3d_dense_mvs_identity_digest(&identity, digest);
  };
  unsigned char base_dense[32];
  CHECK(dense_digest(f.src, base_dense));

  auto changed = [&](auto mutate) {
    Lardon3DDenseMvsSourceImage image = f.src;
    mutate(image.calibration);
    unsigned char binding[32], dense[32], source[32];
    CHECK(lardon3d_dense_mvs_calibration_binding_identity(&image, 1, binding));
    CHECK(dense_digest(image, dense));
    CHECK(lardon3d_dense_mvs_source_image_set_identity(&image, 1, source));
    CHECK(memcmp(binding, base_binding, 32) && memcmp(dense, base_dense, 32));
    CHECK(!memcmp(source, base_source, 32));
    return true;
  };
  CHECK(changed([](auto &c) { ++c.width; }));
  CHECK(changed([](auto &c) { ++c.height; }));
  CHECK(changed([](auto &c) { c.fx += 1.0; }));
  CHECK(changed([](auto &c) { c.fy += 1.0; }));
  CHECK(changed([](auto &c) { c.cx += 1.0; }));
  CHECK(changed([](auto &c) { c.cy += 1.0; }));
  CHECK(changed([](auto &c) { c.k1 += 1.0; }));
  CHECK(changed([](auto &c) { c.k2 += 1.0; }));
  CHECK(changed([](auto &c) { c.p1 += 1.0; }));
  CHECK(changed([](auto &c) { c.p2 += 1.0; }));

  Lardon3DDenseMvsSourceImage signed_zero = f.src;
  signed_zero.calibration.k1 = -0.0;
  unsigned char zero_binding[32];
  CHECK(lardon3d_dense_mvs_calibration_binding_identity(&signed_zero, 1,
                                                         zero_binding));
  CHECK(!memcmp(zero_binding, base_binding, 32));

  Lardon3DDenseMvsSourceImage images[2] = {f.src, f.src};
  images[1].image_id = 18;
  images[1].calibration.fx = 75.0;
  unsigned char ordered[32], reordered[32];
  CHECK(lardon3d_dense_mvs_calibration_binding_identity(images, 2, ordered));
  std::swap(images[0], images[1]);
  CHECK(lardon3d_dense_mvs_calibration_binding_identity(images, 2, reordered));
  CHECK(!memcmp(ordered, reordered, 32));
  images[1].image_id = images[0].image_id;
  CHECK(!lardon3d_dense_mvs_calibration_binding_identity(images, 2, reordered));

  Lardon3DDenseMvsSourceImage invalid = f.src;
  invalid.calibration.fx = NAN;
  CHECK(!lardon3d_dense_mvs_calibration_binding_identity(&invalid, 1,
                                                          reordered));
  invalid = f.src;
  invalid.calibration.p2 = INFINITY;
  CHECK(!lardon3d_dense_mvs_calibration_binding_identity(&invalid, 1,
                                                          reordered));
  return true;
}

bool execution_configuration_not_scientific_identity() {
  F f;
  CHECK(install(f));
  Lardon3DDenseMvsResult first{}, second{};
  CHECK(run(f, first) == LARDON3D_DENSE_MVS_OK);
  const std::string other_staging = f.dir + "/other-staging";
  f.in.execution_thread_count = 1;
  f.in.staging_directory = other_staging.c_str();
  CHECK(lardon3d_dense_mvs_run(&f.in, &second) == LARDON3D_DENSE_MVS_OK);
  CHECK(!memcmp(first.dense_identity, second.dense_identity, 32));
  return true;
}

bool backend_argv() {
  F f;
  CHECK(install(f));
  setenv("DENSE_ARGV_LOG", f.log.c_str(), 1);
  Lardon3DDenseMvsResult base{};
  CHECK(run(f, base) == LARDON3D_DENSE_MVS_OK);
  std::ifstream z(f.log);
  std::vector<std::string> a;
  std::string s;
  while (getline(z, s))
    a.push_back(s);
  std::vector<std::pair<std::string, std::string>> e = {
      {"--resolution-level", "1"}, {"--min-resolution", "320"},
      {"--number-views", "4"},     {"--fusion-mode", "0"},
      {"--max-threads", "3"}};
  for (auto &o : e) {
    int n = 0;
    for (size_t i = 0; i < a.size(); i++)
      if (a[i] == o.first) {
        n++;
        CHECK(i + 1 < a.size() && a[i + 1] == o.second);
      }
    CHECK(n == 1);
  }
  CHECK(std::find(a.begin(), a.end(), "--cuda-device") == a.end());
  {
    F cuda;
    CHECK(install(cuda, "Interface 1", "Densify CUDA"));
    setenv("DENSE_ARGV_LOG", cuda.log.c_str(), 1);
    Lardon3DDenseMvsResult cuda_result{};
    CHECK(run(cuda, cuda_result) == LARDON3D_DENSE_MVS_OK);
    std::ifstream arguments(cuda.log);
    std::vector<std::string> values((std::istream_iterator<std::string>(arguments)), {});
    const auto option = std::find(values.begin(), values.end(), "--cuda-device");
    CHECK(option != values.end() && option + 1 != values.end() && *(option + 1) == "-2");
  }
  f.in.execution_thread_count = 9;
  Lardon3DDenseMvsResult t{};
  CHECK(run(f, t) == 0 && !memcmp(base.dense_identity, t.dense_identity, 32));
  auto changed = [&](const std::string &is, const std::string &ds) {
    CHECK(put(f.ic, is) && put(f.dp, ds));
    Lardon3DDenseMvsResult q{};
    CHECK(run(f, q) == 0);
    CHECK(memcmp(base.backend_implementation_sha256,
                 q.backend_implementation_sha256, 32));
    return true;
  };
  std::string gi = script("Interface 1", "exit 0\n"),
              gd = script("Densify 1",
                          "o=\nwhile [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then "
                          "shift; o=$1; fi; shift; done\nprintf '%s' '" +
                              std::string(good_ply) + "' > \"$o.ply\"\n");
  CHECK(changed(script("Interface 1", "# B1\nexit 0\n"), gd));
  CHECK(
      changed(gi, script("Densify 1", "# B2\n" + gd.substr(gd.find("o=\n")))));
  CHECK(changed(script("Interface 2", "exit 0\n"), gd));
  CHECK(changed(gi, script("Densify 2", gd.substr(gd.find("o=\n")))));
  F other;
  CHECK(install(other));
  Lardon3DDenseMvsResult q{};
  CHECK(run(other, q) == 0 && !memcmp(base.backend_implementation_sha256,
                                      q.backend_implementation_sha256, 32));
  unsetenv("DENSE_ARGV_LOG");
  return true;
}

bool rotations() {
  F f;
  CHECK(install(f));
  double a = 3.14159265358979323846 - 1e-6, c = cos(a), s = sin(a);
  struct R {
    double m[9], q[4];
  } rs[] = {{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0}},
            {{1, 0, 0, 0, -1, 0, 0, 0, -1}, {0, 1, 0, 0}},
            {{-1, 0, 0, 0, 1, 0, 0, 0, -1}, {0, 0, 1, 0}},
            {{-1, 0, 0, 0, -1, 0, 0, 0, 1}, {0, 0, 0, 1}},
            {{1, 0, 0, 0, c, -s, 0, s, c}, {cos(a / 2), sin(a / 2), 0, 0}}};
  for (size_t k = 0; k < 5; k++) {
    memcpy(f.cam.pose_cw.rotation_cw, rs[k].m, sizeof rs[k].m);
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == 0);
    std::ifstream in(result_workspace(r) + "/colmap/sparse/images.txt");
    uint64_t n;
    double q[4];
    CHECK(bool(in >> n >> q[0] >> q[1] >> q[2] >> q[3]));
    double dot = 0;
    for (int j = 0; j < 4; j++) {
      CHECK(std::isfinite(q[j]));
      dot += q[j] * rs[k].q[j];
    }
    CHECK(q[0] > 0 || (q[0] == 0 &&
                       (q[1] > 0 || (q[1] == 0 &&
                                     (q[2] > 0 || (q[2] == 0 && q[3] >= 0))))));
    CHECK(fabs(fabs(dot) - 1) < 1e-5);
    if (!k) {
      std::ifstream x(result_workspace(r) + "/colmap/sparse/images.txt");
      std::string one((std::istreambuf_iterator<char>(x)), {});
      Lardon3DDenseMvsResult rr{};
      CHECK(run(f, rr) == 0);
      std::ifstream y(result_workspace(rr) + "/colmap/sparse/images.txt");
      std::string two((std::istreambuf_iterator<char>(y)), {});
      CHECK(one == two);
    }
  }
  const double invalid[][9] = {
      {2, 0, 0, 0, 1, 0, 0, 0, 1},
      {1, 0.01, 0, 0, 1, 0, 0, 0, 1},
      {-1, 0, 0, 0, 1, 0, 0, 0, 1},
      {NAN, 0, 0, 0, 1, 0, 0, 0, 1},
      {1, 0, 0, 0, 1, 0, 0, 0, INFINITY}};
  for (const auto &matrix : invalid) {
    memcpy(f.cam.pose_cw.rotation_cw, matrix, sizeof matrix);
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == LARDON3D_DENSE_MVS_INVALID_SNAPSHOT);
  }
  double boundary[] = {1, 1e-6, 0, 0, 1, 0, 0, 0, 1};
  memcpy(f.cam.pose_cw.rotation_cw, boundary, sizeof boundary);
  Lardon3DDenseMvsResult accepted{};
  CHECK(run(f, accepted) == LARDON3D_DENSE_MVS_OK);
  boundary[1] = 1.000001e-6;
  memcpy(f.cam.pose_cw.rotation_cw, boundary, sizeof boundary);
  Lardon3DDenseMvsResult rejected{};
  CHECK(run(f, rejected) == LARDON3D_DENSE_MVS_INVALID_SNAPSHOT);
  double row_major_boundary[] = {1, 0, 0, 1e-6, 1, 0, 0, 0, 1};
  memcpy(f.cam.pose_cw.rotation_cw, row_major_boundary,
         sizeof row_major_boundary);
  Lardon3DDenseMvsResult row_major_accepted{};
  CHECK(run(f, row_major_accepted) == LARDON3D_DENSE_MVS_OK);
  row_major_boundary[3] = 1.000001e-6;
  memcpy(f.cam.pose_cw.rotation_cw, row_major_boundary,
         sizeof row_major_boundary);
  Lardon3DDenseMvsResult row_major_rejected{};
  CHECK(run(f, row_major_rejected) == LARDON3D_DENSE_MVS_INVALID_SNAPSHOT);
  return true;
}

bool fail(const std::string &i, const std::string &d,
          Lardon3DDenseMvsStatus want) {
  F f;
  CHECK(put(f.ic, i) && put(f.dp, d));
  Lardon3DDenseMvsResult r{};
  CHECK(run(f, r) == want);
  CHECK(r.status == want && r.status != LARDON3D_DENSE_MVS_OK &&
        !r.point_count && !r.point_cloud_path[0]);
  return true;
}
bool processes() {
  std::string gi = script("I", "exit 0\n"), bad = script("D", "exit 9\n");
  CHECK(fail(script("I", "exit 8\n"), bad, LARDON3D_DENSE_MVS_BACKEND_ERROR));
  CHECK(fail(gi, bad, LARDON3D_DENSE_MVS_BACKEND_ERROR));
  {
    F f;
    CHECK(install(f));
    f.ic += ".missing";
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == LARDON3D_DENSE_MVS_BACKEND_ERROR &&
          zero(r.dense_identity, 32));
  }
  {
    F f;
    CHECK(install(f));
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == 0);
    struct stat info {};
    const std::string workspace = result_workspace(r);
    CHECK(stat((workspace + "/interface-colmap.log").c_str(), &info) == 0 &&
          (info.st_mode & 0777) == 0600);
    CHECK(stat((workspace + "/densify-point-cloud.log").c_str(), &info) == 0 &&
          (info.st_mode & 0777) == 0600);
  }
  {
    F f;
    std::string marker = f.dir + "/marker";
    std::string descendant = f.dir + "/descendant";
    std::string body = "printf dense-stdout; printf dense-stderr >&2; "
                       "(trap '' TERM; sleep 30; printf survived > '" + descendant +
                       "') & printf launched > '" + marker + "'; exit 7\n";
    CHECK(put(f.ic, script("I", "exit 0\n")) &&
          put(f.dp, script("D", body)));
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == LARDON3D_DENSE_MVS_BACKEND_ERROR);
    CHECK(access(marker.c_str(), F_OK) == 0);
    CHECK(access(descendant.c_str(), F_OK) != 0);
    const auto workspaces = private_workspaces(f.dir);
    CHECK(workspaces.size() == 1);
    std::ifstream log(workspaces[0] + "/densify-point-cloud.log");
    std::string output((std::istreambuf_iterator<char>(log)), {});
    CHECK(output.find("dense-stdout") != std::string::npos &&
          output.find("dense-stderr") != std::string::npos);
  }
  {
    F f;
    std::string delayed =
        "sleep 6\no=\nwhile [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then "
        "shift; o=$1; fi; shift; done\nprintf '%s' '" +
        std::string(good_ply) + "' > \"$o.ply\"\n";
    CHECK(put(f.ic, script("I", "exit 0\n")) && put(f.dp, script("D", delayed)));
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == LARDON3D_DENSE_MVS_OK);
  }
  {
    F f;
    std::string verbose =
        "dd if=/dev/zero bs=1100000 count=2 2>/dev/null\n"
        "o=\nwhile [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then shift; "
        "o=$1; fi; shift; done\nprintf '%s' '" +
        std::string(good_ply) + "' > \"$o.ply\"\n";
    CHECK(put(f.ic, script("I", "exit 0\n")) &&
          put(f.dp, script("D", verbose)));
    Lardon3DDenseMvsResult r{};
    CHECK(run(f, r) == LARDON3D_DENSE_MVS_OK);
    struct stat info {};
    CHECK(stat((result_workspace(r) + "/densify-point-cloud.log").c_str(), &info) == 0 &&
          info.st_size == 1024 * 1024);
  }
  F probe_fixture;
  const std::string probe_marker = probe_fixture.dir + "/help-entered";
  CHECK(setenv("PROBE_MARKER", probe_marker.c_str(), 1) == 0);
  std::string over =
      "#!/bin/sh\nif [ \"$1\" = --help ]; then printf entered > \"$PROBE_MARKER\"; "
      "i=0; while [ $i -lt 17000 ]; "
      "do printf x; i=$((i+1)); done; exit 0; fi\n";
  CHECK(fail(over, bad, LARDON3D_DENSE_MVS_BACKEND_ERROR));
  CHECK(access(probe_marker.c_str(), F_OK) == 0 && unlink(probe_marker.c_str()) == 0);
  CHECK(fail("#!/bin/sh\nif [ \"$1\" = --help ]; then printf entered > \"$PROBE_MARKER\"; "
             "printf v; exec 1>&- "
             "2>&-; sleep 20; fi\n",
             bad, LARDON3D_DENSE_MVS_BACKEND_ERROR));
  CHECK(access(probe_marker.c_str(), F_OK) == 0 && unlink(probe_marker.c_str()) == 0);
  CHECK(fail("#!/bin/sh\nif [ \"$1\" = --help ]; then printf entered > \"$PROBE_MARKER\"; "
             "sleep 20; fi\n", bad,
             LARDON3D_DENSE_MVS_BACKEND_ERROR));
  CHECK(access(probe_marker.c_str(), F_OK) == 0 && unlink(probe_marker.c_str()) == 0);
  CHECK(fail("#!/bin/sh\nif [ \"$1\" = --help ]; then printf entered > \"$PROBE_MARKER\"; "
             "trap '' TERM; while "
             ":; do sleep 1; done; fi\n",
             bad, LARDON3D_DENSE_MVS_BACKEND_ERROR));
  CHECK(access(probe_marker.c_str(), F_OK) == 0);
  unsetenv("PROBE_MARKER");
  return true;
}

std::string ply(const std::string &props, const std::string &rows = "1 2 3\n",
                int n = 1, const std::string &fmt = "ascii") {
  return "ply\nformat " + fmt + " 1.0\nelement vertex " + std::to_string(n) +
         "\n" + props + "end_header\n" + rows;
}
void append_u32_le(std::string &data, uint32_t value) {
  for (unsigned int shift = 0; shift < 32; shift += 8)
    data.push_back(static_cast<char>((value >> shift) & 0xffU));
}
void append_f32_le(std::string &data, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  append_u32_le(data, bits);
}
std::string binary_ply(float x, float y, float z, bool trailing = false) {
  std::string data =
      "ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property uchar red\nend_header\n";
  append_f32_le(data, x);
  append_f32_le(data, y);
  append_f32_le(data, z);
  data.push_back(static_cast<char>(127));
  if (trailing) data.push_back('x');
  return data;
}
std::string openmvs_binary_ply(bool truncate_views = false) {
  std::string data =
      "ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property list uchar uint view_indices\n"
      "property list uchar float view_weights\nend_header\n";
  append_f32_le(data, 1.25F);
  append_f32_le(data, -2.5F);
  append_f32_le(data, 3.75F);
  data.push_back(2);
  append_u32_le(data, 7);
  if (!truncate_views) append_u32_le(data, 11);
  data.push_back(2);
  append_f32_le(data, 0.75F);
  append_f32_le(data, 0.25F);
  return data;
}
std::string oversized_binary_list_ply() {
  std::string data =
      "ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
      "property float x\nproperty float y\nproperty float z\n"
      "property list uint uint view_indices\nend_header\n";
  append_f32_le(data, 1.0F);
  append_f32_le(data, 2.0F);
  append_f32_le(data, 3.0F);
  append_u32_le(data, UINT32_MAX);
  return data;
}
bool plys() {
  std::string xyz = "property float x\nproperty float y\nproperty float z\n";
  std::vector<std::pair<std::string, bool>> v = {
      {good_ply, true},
      {"ply\r\nformat ascii 1.0\r\nelement vertex 1\r\nproperty float x\r\n"
       "property float y\r\nproperty float z\r\nend_header\r\n1 2 3\r\n",
       true},
      {"ply\rformat ascii 1.0\nelement vertex 1\n" + xyz +
           "end_header\n1 2 3\n",
       false},
      {ply(xyz, "1 2\r 3\n"), false},
      {ply("property float y\nproperty float z\n", "2 3\n"), false},
      {ply("property float x\nproperty float z\n", "1 3\n"), false},
      {ply("property float x\nproperty float y\n", "1 2\n"), false},
      {ply("property float x\nproperty float x\nproperty float y\nproperty "
           "float z\n",
           "1 1 2 3\n"),
       false},
      {ply("property float x\nproperty float y\nproperty float y\nproperty "
           "float z\n",
           "1 2 2 3\n"),
       false},
      {ply("property float x\nproperty float y\nproperty float z\nproperty "
           "float z\n",
           "1 2 3 3\n"),
       false},
      {ply("property float\n", "1\n"), false},
      {ply(xyz, "1 2 3\n", 2), false},
      {ply(xyz, "nan 2 3\n"), false},
      {ply(xyz, "inf 2 3\n"), false},
      {ply(xyz + "property uchar red\n", "1 2 3 255\n"), true},
      {ply(xyz, "1 2 3\n \t\r\n"), true},
      {ply(xyz, "1 2 3\n \t\r"), false},
      {ply(xyz, "1 2 3\ngarbage\n"), false},
      {"ply\nformat ascii 1.0\nelement vertex 1\n" + xyz +
           "element face 0\nend_header\n1 2 3\n", false},
      {ply(xyz, "", 0), false},
      {ply(xyz, "", 1, "binary_little_endian"), false},
      {binary_ply(1.25F, -2.5F, 3.75F), true},
      {binary_ply(NAN, 2.0F, 3.0F), false},
      {binary_ply(1.0F, 2.0F, 3.0F, true), false},
      {openmvs_binary_ply(), true},
      {openmvs_binary_ply(true), false},
      {oversized_binary_list_ply(), false},
      {ply(xyz + "property list uchar uint view_indices\n", "1 2 3 0\n"), false}};
  std::string too_many = xyz;
  for (int i = 3; i < 257; ++i)
    too_many += "property float p" + std::to_string(i) + "\n";
  v.push_back({ply(too_many, ""), false});
  v.push_back({"ply\nformat ascii 1.0\ncomment " +
                   std::string(64 * 1024 + 1, 'x') + "\nend_header\n", false});
  v.push_back({ply(xyz, std::string(64 * 1024 + 1, '1') + "\n"), false});
  std::string huge_header = "ply\nformat ascii 1.0\n";
  while (huge_header.size() <= 1024 * 1024)
    huge_header += "comment " + std::string(1000, 'x') + "\n";
  v.push_back({huge_header + "end_header\n", false});
  std::string crlf_header = "ply\r\nformat ascii 1.0\r\n";
  size_t normalized_size = std::string("ply\nformat ascii 1.0\n").size();
  for (size_t i = 0; i < 1000; ++i) {
    crlf_header += "comment " + std::string(1000, 'x') + "\r\n";
    normalized_size += 1009;
  }
  const size_t end_header_size = std::string("end_header\n").size();
  CHECK(normalized_size + end_header_size < 1024 * 1024);
  const size_t filler_size = 1024 * 1024 - normalized_size - end_header_size;
  CHECK(filler_size >= 9 && filler_size <= 64 * 1024 + 1);
  crlf_header += "comment " + std::string(filler_size - 9, 'x') + "\r\n";
  crlf_header += "end_header\r\n";
  v.push_back({crlf_header, false});
  for (size_t case_index = 0; case_index < v.size(); ++case_index) {
    auto &c = v[case_index];
    F f;
    CHECK(install(f, "I", "D", c.first));
    Lardon3DDenseMvsResult r{};
    auto st = run(f, r);
    if ((st == 0) != c.second)
      std::fprintf(stderr, "PLY case %zu returned %d\n", case_index,
                   static_cast<int>(st));
    CHECK((st == 0) == c.second);
    if (!c.second)
      CHECK(r.status == LARDON3D_DENSE_MVS_INVALID_OUTPUT);
  }
  return true;
}

bool private_workspace_isolation() {
  F f;
  CHECK(install(f));
  Lardon3DDenseMvsResult first{};
  CHECK(run(f, first) == LARDON3D_DENSE_MVS_OK);
  const std::string first_workspace = result_workspace(first);
  CHECK(!first_workspace.empty());
  CHECK(put(f.dp, script("D", "exit 0\n")));
  Lardon3DDenseMvsResult second{};
  CHECK(run(f, second) == LARDON3D_DENSE_MVS_INVALID_OUTPUT);
  CHECK(second.status == LARDON3D_DENSE_MVS_INVALID_OUTPUT &&
        !second.point_cloud_path[0] && second.point_count == 0);
  const auto workspaces = private_workspaces(f.dir);
  CHECK(workspaces.size() == 2 && workspaces[0] != workspaces[1]);
  CHECK((workspaces[0] == first_workspace || workspaces[1] == first_workspace));
  return true;
}

bool provenance_path() {
  F f;
  CHECK(install(f));
  Lardon3DDenseMvsResult r{};
  CHECK(run(f, r) == 0);
  unsigned char p[32], s[32], d[32];
  CHECK(lardon3d_dense_mvs_parameter_fingerprint(&f.in.parameters, p));
  CHECK(lardon3d_dense_mvs_source_image_set_identity(&f.src, 1, s));
  CHECK(!memcmp(r.parameter_fingerprint, p, 32) &&
        !memcmp(r.base_reconstruction_identity,
                f.in.base_reconstruction_identity, 32) &&
        !memcmp(r.source_image_set_identity, s, 32));
  CHECK(memcmp(r.base_reconstruction_identity, r.dense_identity, 32) &&
        r.point_count == 2 &&
        result_workspace(r).find(f.dir + "/lardon3d-mvs-") == 0 &&
        strlen(r.point_cloud_path) < sizeof r.point_cloud_path);
  Lardon3DDenseMvsIdentity x{};
  memcpy(x.base_reconstruction_identity, f.in.base_reconstruction_identity, 32);
  memcpy(x.source_image_set_identity, s, 32);
  memcpy(x.calibration_scope_identity, f.in.calibration_scope_identity, 32);
  CHECK(lardon3d_dense_mvs_calibration_binding_identity(
      &f.src, 1, x.calibration_binding_identity));
  memcpy(x.backend_binary_sha256, r.backend_implementation_sha256, 32);
  memcpy(x.parameter_fingerprint, p, 32);
  x.dense_kind = x.dense_version = x.backend_kind = x.backend_version = 1;
  CHECK(lardon3d_dense_mvs_identity_digest(&x, d) &&
        !memcmp(d, r.dense_identity, 32) &&
        !memcmp(x.backend_binary_sha256, r.backend_implementation_sha256, 32));
  std::string huge(LARDON3D_DENSE_MVS_PATH_CAPACITY, 'x');
  f.in.staging_directory = huge.c_str();
  Lardon3DDenseMvsResult bad{};
  CHECK(lardon3d_dense_mvs_run(&f.in, &bad) ==
        LARDON3D_DENSE_MVS_INVALID_ARGUMENT);
  CHECK(bad.status == LARDON3D_DENSE_MVS_INVALID_ARGUMENT &&
        bad.status != LARDON3D_DENSE_MVS_OK && !bad.point_cloud_path[0] &&
        !bad.point_count);
  return true;
}

bool failure_atomic_and_mapping() {
  F f;
  CHECK(install(f));
  Lardon3DDenseMvsResult result{};
  CHECK(run(f, result) == LARDON3D_DENSE_MVS_OK);

  result.point_count = 99;
  std::memset(result.dense_identity, 0xa5, sizeof result.dense_identity);
  f.cam.pose_cw.rotation_cw[0] = 2.0;
  CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SNAPSHOT);
  CHECK(result.status == LARDON3D_DENSE_MVS_INVALID_SNAPSHOT &&
        result.point_count == 0 && result.point_cloud_path[0] == '\0' &&
        zero(result.dense_identity, sizeof result.dense_identity));
  f.cam.pose_cw.rotation_cw[0] = 1.0;

  CHECK(install(f));
  f.src.calibration.fx = std::numeric_limits<double>::denorm_min();
  f.src.calibration.fy = std::numeric_limits<double>::denorm_min();
  f.src.calibration.k1 = 0.1;
  result.point_count = 99;
  std::memset(result.dense_identity, 0xa5, sizeof result.dense_identity);
  CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  CHECK(result.status == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE &&
        result.point_count == 0 && result.point_cloud_path[0] == '\0' &&
        zero(result.dense_identity, sizeof result.dense_identity));
  f.src.calibration = {64, 48, 50, 51, 32, 24, 0, 0, 0, 0};

  const std::string marker = f.dir + "/capability-called";
  const std::string backend_marker = f.dir + "/backend-called";
  CHECK(put(f.ic,
            "#!/bin/sh\nif [ \"$1\" = --help ]; then printf '%s\\n' "
            "'OpenMVS x64 v2.4.0 --max-threads --image-folder'; "
            "printf called > '" + marker + "'; exit 1; fi\nprintf backend > '" +
                backend_marker + "'\n"));
  f.src.immutable_sha256[0] ^= 1U;
  CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  CHECK(access(marker.c_str(), F_OK) != 0 &&
        access(backend_marker.c_str(), F_OK) != 0);
  f.src.immutable_sha256[0] ^= 1U;

  f.src.source_path = f.dir.c_str();
  CHECK(lardon3d_dense_mvs_run(&f.in, &result) ==
        LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  f.src.source_path = f.image.c_str();

  F io;
  CHECK(install(io));
  const std::string blocked = io.dir + "/blocked";
  CHECK(put(blocked, "not-a-directory", false));
  io.in.staging_directory = blocked.c_str();
  CHECK(lardon3d_dense_mvs_run(&io.in, &result) == LARDON3D_DENSE_MVS_IO_ERROR);
  CHECK(result.status == LARDON3D_DENSE_MVS_IO_ERROR && result.point_count == 0);

  F pointers;
  CHECK(install(pointers));
  pointers.snap.observations = nullptr;
  CHECK(run(pointers, result) == LARDON3D_DENSE_MVS_INVALID_SNAPSHOT);
  pointers.snap.observations = &pointers.observation;
  pointers.in.source_observations = nullptr;
  CHECK(run(pointers, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  return true;
}

size_t open_fd_count() {
  DIR *directory = opendir("/proc/self/fd");
  if (!directory) return SIZE_MAX;
  size_t count = 0;
  while (const dirent *entry = readdir(directory))
    if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, ".."))
      ++count;
  closedir(directory);
  return count;
}

bool source_file_types_and_dimensions() {
  for (const std::string extension : {".jpg", ".png", ".bmp"}) {
    F f;
    CHECK(install(f));
    const std::string encoded = f.dir + "/source" + extension;
    cv::Mat pixels(48, 64, CV_8UC3, cv::Scalar(20, 40, 60));
    CHECK(cv::imwrite(encoded, pixels));
    f.image = encoded;
    f.src.source_path = f.image.c_str();
    CHECK(digest_file(f.image, f.src.immutable_sha256));
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_OK);
    f.src.calibration.width++;
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  }

  F f;
  CHECK(install(f));
  const std::string fifo = f.dir + "/source-fifo";
  CHECK(mkfifo(fifo.c_str(), 0600) == 0);
  f.image = fifo;
  const size_t descriptors_before = open_fd_count();
  CHECK(descriptors_before != SIZE_MAX);
  for (size_t iteration = 0; iteration < 32; ++iteration) {
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  }
  CHECK(open_fd_count() == descriptors_before);

  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    Lardon3DDenseMvsResult result{};
    const auto status = run(f, result);
    _exit(status == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE ? 0 : 1);
  }
  int child_status = 0;
  bool exited = false;
  for (size_t iteration = 0; iteration < 200; ++iteration) {
    const pid_t waited = waitpid(child, &child_status, WNOHANG);
    CHECK(waited >= 0);
    if (waited == child) {
      exited = true;
      break;
    }
    timespec delay{0, 10 * 1000 * 1000};
    (void)nanosleep(&delay, nullptr);
  }
  if (!exited) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &child_status, 0);
  }
  CHECK(exited && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

  f.image = f.dir;
  Lardon3DDenseMvsResult directory_result{};
  CHECK(run(f, directory_result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  CHECK(open_fd_count() == descriptors_before);
  return true;
}

bool source_preflight_and_backend_bound() {
  auto rejected_before_probe = [](F &f) {
    const std::string marker = f.dir + "/probe-called";
    CHECK(put(f.ic, "#!/bin/sh\nprintf called > '" + marker + "'\n") &&
          put(f.dp, "#!/bin/sh\nprintf called > '" + marker + "'\n"));
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
    CHECK(access(marker.c_str(), F_OK) != 0);
    return true;
  };
  {
    F f;
    f.src.calibration.fx = NAN;
    CHECK(rejected_before_probe(f));
  }
  {
    F f;
    f.source_observation.image_id++;
    CHECK(rejected_before_probe(f));
  }
  {
    F f;
    f.src.image_id++;
    CHECK(rejected_before_probe(f));
  }
  {
    F f;
    Lardon3DSparseIncrementalLandmarkObservation snapshot_observations[2] = {
        f.observation, f.observation};
    Lardon3DSparseIncrementalObservation source_observations[2] = {
        f.source_observation, f.source_observation};
    f.snap.observations = snapshot_observations;
    f.snap.observation_count = 2;
    f.landmark.observation_count = 2;
    f.in.source_observations = source_observations;
    f.in.source_observation_count = 2;
    CHECK(rejected_before_probe(f));
  }
  {
    F f;
    const unsigned char png_header[] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13,
        'I', 'H', 'D', 'R', 0, 0, 0x4e, 0x20, 0, 0, 0x4e, 0x20};
    CHECK(put(f.image, std::string(reinterpret_cast<const char *>(png_header),
                                   sizeof png_header), false));
    CHECK(digest_file(f.image, f.src.immutable_sha256));
    CHECK(rejected_before_probe(f));
  }
  {
    F f;
    CHECK(install(f));
    const std::string marker = f.dir + "/help-entered";
    CHECK(put(f.ic,
              "#!/bin/sh\nif [ \"$1\" = --help ]; then printf '%s\\n' "
              "'OpenMVS x64 v2.4.0 --max-threads --image-folder'; "
              "printf called > '" + marker + "'; exit 1; fi\nexit 0\n"));
    CHECK(truncate(f.ic.c_str(),
                   static_cast<off_t>(UINT64_C(1024) * 1024 * 1024 + 1)) == 0);
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_BACKEND_ERROR);
    CHECK(access(marker.c_str(), F_OK) == 0);
  }
  {
    F f;
    CHECK(install(f));
    CHECK(truncate(f.dp.c_str(),
                   static_cast<off_t>(UINT64_C(1024) * 1024 * 1024)) == 0);
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_BACKEND_ERROR);
  }
  return true;
}

bool complete_colmap_contract() {
  F f;
  CHECK(install(f));
  f.src.calibration.k1 = 0.12;
  f.src.calibration.k2 = -0.03;
  f.src.calibration.p1 = 0.002;
  f.src.calibration.p2 = -0.001;
  Lardon3DDenseMvsResult result{};
  CHECK(run(f, result) == LARDON3D_DENSE_MVS_OK);

  const std::string workspace = result_workspace(result);
  cv::Mat exported = cv::imread(workspace + "/colmap/images/image_1.png",
                                cv::IMREAD_UNCHANGED);
  CHECK(!exported.empty() && exported.cols == 64 && exported.rows == 48);

  std::ifstream images(workspace + "/colmap/sparse/images.txt");
  std::string pose_line, observation_line;
  CHECK(bool(std::getline(images, pose_line)) &&
        bool(std::getline(images, observation_line)));
  std::istringstream observation_tokens(observation_line);
  observation_tokens.imbue(std::locale::classic());
  double exported_x = 0, exported_y = 0;
  uint32_t point_id = 0;
  CHECK(bool(observation_tokens >> exported_x >> exported_y >> point_id) &&
        point_id == 1);

  const auto &k = f.src.calibration;
  const cv::Mat camera = cv::Mat(cv::Matx33d(
      k.fx, 0, k.cx,
      0, k.fy, k.cy,
      0, 0, 1));
  const cv::Mat distortion = cv::Mat(cv::Matx<double, 1, 4>(
      k.k1, k.k2, k.p1, k.p2));
  std::vector<cv::Point2d> source = {
      {f.source_observation.x, f.source_observation.y}};
  std::vector<cv::Point2d> expected;
  cv::undistortPoints(source, expected, camera, distortion, cv::noArray(), camera);
  CHECK(exported_x == expected[0].x && exported_y == expected[0].y);

  std::ifstream points(workspace + "/colmap/sparse/points3D.txt");
  uint32_t exported_point_id = 0, red = 0, green = 0, blue = 0;
  double x = 0, y = 0, z = 0, error = 0;
  uint32_t image_id = 0;
  size_t point2d_index = SIZE_MAX;
  CHECK(bool(points >> exported_point_id >> x >> y >> z >> red >> green >> blue >>
             error >> image_id >> point2d_index));
  CHECK(exported_point_id == 1 && x == 1 && y == 2 && z == 3 &&
        red == 0 && green == 0 && blue == 0 && error == 0.25 &&
        image_id == 1 && point2d_index == 0);
  return true;
}

bool multi_track_membership() {
  F f;
  CHECK(install(f));
  Lardon3DSparseIncrementalCamera cameras[2] = {f.cam, f.cam};
  cameras[1].image_id = 18;
  Lardon3DSparseIncrementalLandmark landmarks[2] = {f.landmark, f.landmark};
  landmarks[0].observation_count = 2;
  landmarks[1].landmark_id = 24;
  landmarks[1].track_id = 30;
  landmarks[1].point = {4, 5, 6};
  landmarks[1].observation_count = 2;
  Lardon3DSparseIncrementalLandmarkObservation observations[4] = {
      {24, 30, 18, 34, 40, 0},
      {23, 29, 17, 31, 37, 0},
      {24, 30, 17, 33, 39, 1},
      {23, 29, 18, 32, 38, 1}};
  Lardon3DSparseIncrementalObservation source_observations[4] = {
      {30, 18, 34, 40, 41, 14, 15},
      {29, 17, 31, 37, 40, 10, 11},
      {30, 17, 33, 39, 42, 12, 13},
      {29, 18, 32, 38, 43, 16, 17}};
  Lardon3DDenseMvsSourceImage images[2] = {f.src, f.src};
  images[1].image_id = 18;
  f.snap.cameras = cameras;
  f.snap.camera_count = 2;
  f.snap.landmarks = landmarks;
  f.snap.landmark_count = 2;
  f.snap.observations = observations;
  f.snap.observation_count = 4;
  f.in.source_observations = source_observations;
  f.in.source_observation_count = 4;
  f.in.source_images = images;
  f.in.source_image_count = 2;
  Lardon3DDenseMvsResult result{};
  const auto status = run(f, result);
  if (status != LARDON3D_DENSE_MVS_OK)
    std::fprintf(stderr, "multi-track returned %d\n", static_cast<int>(status));
  CHECK(status == LARDON3D_DENSE_MVS_OK);
  std::ifstream points(result_workspace(result) + "/colmap/sparse/points3D.txt");
  std::string first, second;
  CHECK(bool(std::getline(points, first)) && bool(std::getline(points, second)));
  CHECK(first == "1 1.0000000000000000e+00 2.0000000000000000e+00 "
                 "3.0000000000000000e+00 0 0 0 2.5000000000000000e-01 1 0 2 0");
  CHECK(second == "2 4.0000000000000000e+00 5.0000000000000000e+00 "
                  "6.0000000000000000e+00 0 0 0 2.5000000000000000e-01 1 1 2 1");
  const std::string test_path(__FILE__);
  const size_t tests_component = test_path.rfind("tests/test_dense_mvs.cpp");
  CHECK(tests_component != std::string::npos);
  std::ifstream source(test_path.substr(0, tests_component) + "src/dense_mvs.cpp");
  const std::string implementation((std::istreambuf_iterator<char>(source)), {});
  CHECK(implementation.find("for (const auto &entry : image_observations)") ==
        std::string::npos);
  CHECK(implementation.find("before.st_dev == after.st_dev") !=
            std::string::npos &&
        implementation.find("before.st_ino == after.st_ino") !=
            std::string::npos &&
        implementation.find("before.st_size == after.st_size") !=
            std::string::npos &&
        implementation.find("before.st_mtim.tv_nsec == after.st_mtim.tv_nsec") !=
            std::string::npos &&
        implementation.find("before.st_ctim.tv_nsec == after.st_ctim.tv_nsec") !=
            std::string::npos);
  return true;
}

bool bounded_inputs_and_empty_camera_observations() {
  {
    F f;
    const std::string backend_marker = f.dir + "/backend-called";
    CHECK(put(f.ic, "#!/bin/sh\nprintf called > '" + backend_marker + "'\n") &&
          put(f.dp, "#!/bin/sh\nprintf called > '" + backend_marker + "'\n"));
    CHECK(truncate(f.image.c_str(),
                   static_cast<off_t>(UINT64_C(1024) * 1024 * 1024 + 1)) == 0);
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
    CHECK(result.status == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE &&
          access(backend_marker.c_str(), F_OK) != 0);
  }
  {
    F f;
    CHECK(install(f));
    f.in.parameters.fusion_mode = 1;
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_ARGUMENT);
    CHECK(result.status == LARDON3D_DENSE_MVS_INVALID_ARGUMENT &&
          access(f.log.c_str(), F_OK) != 0);
  }
  {
    F f;
    CHECK(install(f));
    f.src.calibration.width = UINT32_MAX;
    f.src.calibration.height = UINT32_MAX;
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
    CHECK(result.status == LARDON3D_DENSE_MVS_INVALID_SOURCE_IMAGE);
  }
  {
    F f;
    CHECK(install(f));
    Lardon3DSparseIncrementalCamera cameras[2] = {f.cam, f.cam};
    cameras[1].image_id = 18;
    Lardon3DDenseMvsSourceImage images[2] = {f.src, f.src};
    images[1].image_id = 18;
    f.snap.cameras = cameras;
    f.snap.camera_count = 2;
    f.in.source_images = images;
    f.in.source_image_count = 2;
    Lardon3DDenseMvsResult result{};
    CHECK(run(f, result) == LARDON3D_DENSE_MVS_OK);

    std::ifstream exported(result_workspace(result) + "/colmap/sparse/images.txt");
    std::string first_pose, first_points, second_pose, second_points;
    CHECK(bool(std::getline(exported, first_pose)) &&
          bool(std::getline(exported, first_points)) &&
          bool(std::getline(exported, second_pose)) &&
          bool(std::getline(exported, second_points)));
    CHECK(!first_points.empty() && !second_pose.empty() && second_points.empty());
  }
  return true;
}

bool deterministic_colmap_numbers() {
  F f;
  CHECK(install(f));
  const double calibration[] = {
      501.2345678901234, 502.3456789012345, 319.4567890123456,
      239.5678901234567, 0.1234567890123456, -0.2345678901234567,
      0.0034567890123456, -0.0045678901234567};
  f.src.calibration.fx = calibration[0];
  f.src.calibration.fy = calibration[1];
  f.src.calibration.cx = calibration[2];
  f.src.calibration.cy = calibration[3];
  f.src.calibration.k1 = calibration[4];
  f.src.calibration.k2 = calibration[5];
  f.src.calibration.p1 = calibration[6];
  f.src.calibration.p2 = calibration[7];
  const double translation[] = {
      9.876543210987654, -8.765432109876543, 0.0076543210987654};
  std::memcpy(f.cam.pose_cw.translation_cw, translation, sizeof translation);

  GlobalLocaleGuard locale_guard;
  Lardon3DDenseMvsResult result{};
  CHECK(run(f, result) == LARDON3D_DENSE_MVS_OK);
  const std::string workspace = result_workspace(result);
  std::ifstream cameras(workspace + "/colmap/sparse/cameras.txt");
  std::ifstream images(workspace + "/colmap/sparse/images.txt");
  const std::string camera_text((std::istreambuf_iterator<char>(cameras)), {});
  const std::string image_text((std::istreambuf_iterator<char>(images)), {});

  std::istringstream camera_tokens(camera_text);
  std::istringstream image_tokens(image_text);
  camera_tokens.imbue(std::locale::classic());
  image_tokens.imbue(std::locale::classic());
  uint64_t camera_id = 0, image_id = 0, image_camera_id = 0;
  unsigned int width = 0, height = 0;
  std::string model, image_name;
  std::string camera_numbers[4], quaternion_numbers[4], translation_numbers[3];
  CHECK(bool(camera_tokens >> camera_id >> model >> width >> height));
  for (std::string &token : camera_numbers)
    CHECK(bool(camera_tokens >> token));
  CHECK(bool(image_tokens >> image_id));
  for (std::string &token : quaternion_numbers)
    CHECK(bool(image_tokens >> token));
  for (std::string &token : translation_numbers)
    CHECK(bool(image_tokens >> token));
  CHECK(bool(image_tokens >> image_camera_id >> image_name));
  CHECK(camera_id == 1 && image_id == 1 && image_camera_id == 1 &&
        model == "PINHOLE" && width == 64 && height == 48 &&
        image_name == "image_1.png");

  auto scientific_round_trip = [](const std::string &token, double expected) {
    CHECK(token.find('.') != std::string::npos);
    CHECK(token.find_first_of("eE") != std::string::npos);
    std::istringstream parser(token);
    parser.imbue(std::locale::classic());
    double parsed = 0;
    CHECK(bool(parser >> parsed));
    CHECK(parser.peek() == std::char_traits<char>::eof());
    CHECK(parsed == expected);
    return true;
  };
  const double exported_calibration[] = {
      calibration[0], calibration[1], calibration[2] + 0.5,
      calibration[3] + 0.5};
  for (size_t i = 0; i < 4; ++i)
    CHECK(scientific_round_trip(camera_numbers[i], exported_calibration[i]));
  for (const std::string &token : quaternion_numbers) {
    CHECK(token.find('.') != std::string::npos);
    CHECK(token.find_first_of("eE") != std::string::npos);
  }
  for (size_t i = 0; i < 3; ++i)
    CHECK(scientific_round_trip(translation_numbers[i], translation[i]));
  CHECK(camera_text.find(',') == std::string::npos &&
        image_text.find(',') == std::string::npos);
  return true;
}
} // namespace
int main() {
  return goldens() && calibration_identity_contract() &&
                 execution_configuration_not_scientific_identity() &&
                 backend_argv() && rotations() && processes() && plys() &&
                 private_workspace_isolation() && provenance_path() &&
                 failure_atomic_and_mapping() && complete_colmap_contract() &&
                 source_file_types_and_dimensions() &&
                 multi_track_membership() &&
                 source_preflight_and_backend_bound() &&
                 bounded_inputs_and_empty_camera_observations() &&
                 deterministic_colmap_numbers()
             ? 0
             : 1;
}
