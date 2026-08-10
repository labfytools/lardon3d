#include <lardon3d/sparse_sfm_incremental.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <dirent.h>
#include <sys/resource.h>
#include <vector>

static size_t open_file_descriptor_count() {
  DIR *directory = opendir("/proc/self/fd");
  if (!directory) return 0;
  size_t count = 0;
  while (readdir(directory)) ++count;
  closedir(directory);
  return count >= 2 ? count - 2 : 0;
}

static Lardon3DSparseGeometryPoint2 project(
    const Lardon3DSparseGeometryCalibration &calibration,
    const Lardon3DSparseGeometryPose &pose,
    const Lardon3DSparseGeometryPoint3 &point) {
  const double x = point.x + pose.translation_cw[0];
  const double y = point.y + pose.translation_cw[1];
  const double z = point.z + pose.translation_cw[2];
  return {calibration.fx * x / z + calibration.cx,
          calibration.fy * y / z + calibration.cy};
}

int main(int argc, char **argv) {
  const size_t camera_count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8;
  const size_t track_count = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 500;
  if (camera_count < 2 || track_count < 6 || camera_count > 64 ||
      track_count > 10000)
    return EXIT_FAILURE;
  const Lardon3DSparseGeometryCalibration calibration = {
      4000, 3000, 2000.0, 2000.0, 2000.0, 1500.0, 0.0, 0.0, 0.0, 0.0};
  std::vector<Lardon3DSparseIncrementalImage> images;
  std::vector<Lardon3DSparseIncrementalObservation> observations;
  images.reserve(camera_count);
  observations.reserve(camera_count * track_count);
  for (size_t camera = 0; camera < camera_count; ++camera)
    images.push_back({100 + camera, calibration});
  for (size_t track = 0; track < track_count; ++track) {
    const Lardon3DSparseGeometryPoint3 point = {
        -1.5 + static_cast<double>(track % 31) * 0.1,
        -1.0 + static_cast<double>((track / 31) % 23) * 0.09,
        8.0 + static_cast<double>(track % 17) * 0.07};
    for (size_t camera = 0; camera < camera_count; ++camera) {
      Lardon3DSparseGeometryPose pose = {
          {1, 0, 0, 0, 1, 0, 0, 0, 1},
          {static_cast<double>(camera) * 0.3, 0, 0}};
      const Lardon3DSparseGeometryPoint2 pixel = project(calibration, pose, point);
      observations.push_back({track + 1, 100 + camera, 1000 + camera,
                              static_cast<uint32_t>(track), 8192,
                              pixel.x, pixel.y});
    }
  }
  Lardon3DSparseIncrementalParameters parameters;
  if (!lardon3d_sparse_incremental_parameters_default(&parameters)) return EXIT_FAILURE;
  parameters.maximum_registration_rounds = 64;
  Lardon3DSparseIncrementalInput input = {
      1, 2, images.data(), images.size(), observations.data(), observations.size()};
  Lardon3DSparseIncrementalResult result = {};
  const auto started = std::chrono::steady_clock::now();
  const Lardon3DSparseIncrementalStatus status =
      lardon3d_sparse_incremental_run(&input, &parameters, &result);
  const auto stopped = std::chrono::steady_clock::now();
  struct rusage usage = {};
  getrusage(RUSAGE_SELF, &usage);
  const double wall_seconds =
      std::chrono::duration<double>(stopped - started).count();
  const double cpu_seconds =
      static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
      static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
          1000000.0;
  std::printf("status=%d cameras=%zu tracks=%zu observations=%zu components=%zu "
              "registered=%zu landmarks=%zu rounds=%llu seeds=%llu "
              "wall_s=%.6f cpu_s=%.6f peak_rss_kib=%ld fds=%zu\n",
              static_cast<int>(status), camera_count, track_count,
              observations.size(), result.component_count, result.camera_count,
              result.landmark_count,
              static_cast<unsigned long long>(result.registration_rounds),
              static_cast<unsigned long long>(result.seed_candidates_considered),
              wall_seconds, cpu_seconds, usage.ru_maxrss,
              open_file_descriptor_count());
  const bool valid = status == LARDON3D_SPARSE_INCREMENTAL_COMPLETE &&
                     result.camera_count == camera_count && result.landmark_count > 0;
  lardon3d_sparse_incremental_result_destroy(&result);
  return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
