#include <lardon3d/orb_vulkan_backend.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include "vulkan_process_startup.h"

static uint32_t random_u32(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static std::vector<float> make_descriptors(uint32_t count, bool rootsift,
                                           uint32_t seed) {
  std::vector<float> descriptors(static_cast<size_t>(count) * 128);
  for (uint32_t row = 0; row < count; ++row) {
    float sum = 0.0F;
    for (uint32_t component = 0; component < 128; ++component) {
      float value = static_cast<float>(random_u32(&seed) & 0xffffU) / 65535.0F;
      descriptors[static_cast<size_t>(row) * 128 + component] = value;
      sum += value;
    }
    if (rootsift) {
      for (uint32_t component = 0; component < 128; ++component) {
        float &value = descriptors[static_cast<size_t>(row) * 128 + component];
        value = std::sqrt(value / sum);
      }
    }
  }
  return descriptors;
}

static double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

static double elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

static bool benchmark(uint32_t count_a, uint32_t count_b, bool rootsift) {
  std::vector<float> a = make_descriptors(count_a, rootsift, 0x12345678U);
  std::vector<float> b = make_descriptors(count_b, rootsift, 0x87654321U);
  cv::Mat matrix_a(static_cast<int>(count_a), 128, CV_32FC1, a.data());
  cv::Mat matrix_b(static_cast<int>(count_b), 128, CV_32FC1, b.data());
  cv::BFMatcher matcher(cv::NORM_L2, false);
  std::vector<std::vector<cv::DMatch>> cpu_output;
  matcher.knnMatch(matrix_a, matrix_b, cpu_output, 2);

  std::vector<double> cpu_samples;
  for (int repetition = 0; repetition < 5; ++repetition) {
    auto start = std::chrono::steady_clock::now();
    matcher.knnMatch(matrix_a, matrix_b, cpu_output, 2);
    cpu_samples.push_back(elapsed_ms(start));
  }

  std::vector<Lardon3DSiftTop2> gpu_output(count_a);
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) {
    return false;
  }
  auto cold_start = std::chrono::steady_clock::now();
  if (lardon3d_sift_vulkan_top2(backend, a.data(), count_a, b.data(), count_b,
                                gpu_output.data(), gpu_output.size()) !=
      LARDON3D_ORB_VULKAN_OK) {
    lardon3d_orb_vulkan_backend_destroy(backend);
    return false;
  }
  double cold_ms = elapsed_ms(cold_start);

  Lardon3DOrbVulkanBackend *shared_backend =
      lardon3d_orb_vulkan_backend_create();
  if (!shared_backend) {
    lardon3d_orb_vulkan_backend_destroy(backend);
    return false;
  }
  unsigned char orb_descriptor[32]{};
  Lardon3DOrbTop2 orb_output{};
  if (lardon3d_orb_vulkan_top2(shared_backend, orb_descriptor, 1,
                               orb_descriptor, 1, &orb_output, 1) !=
      LARDON3D_ORB_VULKAN_OK) {
    lardon3d_orb_vulkan_backend_destroy(shared_backend);
    lardon3d_orb_vulkan_backend_destroy(backend);
    return false;
  }
  auto after_orb_start = std::chrono::steady_clock::now();
  if (lardon3d_sift_vulkan_top2(shared_backend, a.data(), count_a, b.data(),
                                count_b, gpu_output.data(), gpu_output.size()) !=
      LARDON3D_ORB_VULKAN_OK) {
    lardon3d_orb_vulkan_backend_destroy(shared_backend);
    lardon3d_orb_vulkan_backend_destroy(backend);
    return false;
  }
  double after_orb_ms = elapsed_ms(after_orb_start);
  lardon3d_orb_vulkan_backend_destroy(shared_backend);

  std::vector<double> warm_samples;
  std::vector<double> gpu_samples;
  for (int repetition = 0; repetition < 5; ++repetition) {
    auto start = std::chrono::steady_clock::now();
    if (lardon3d_sift_vulkan_top2(backend, a.data(), count_a, b.data(), count_b,
                                  gpu_output.data(), gpu_output.size()) !=
        LARDON3D_ORB_VULKAN_OK) {
      lardon3d_orb_vulkan_backend_destroy(backend);
      return false;
    }
    warm_samples.push_back(elapsed_ms(start));
    Lardon3DOrbVulkanInfo info;
    if (!lardon3d_orb_vulkan_backend_info(backend, &info)) {
      lardon3d_orb_vulkan_backend_destroy(backend);
      return false;
    }
    gpu_samples.push_back(static_cast<double>(info.gpu_ns) / 1000000.0);
  }
  double cpu_ms = median(cpu_samples);
  double warm_ms = median(warm_samples);
  auto finalization_start = std::chrono::steady_clock::now();
  double finalization_checksum = 0.0;
  uint32_t exact_final_distances = 0;
  uint32_t compared_final_distances = 0;
  for (uint32_t query = 0; query < count_a; ++query) {
    const Lardon3DSiftTop2 &top2 = gpu_output[query];
    const uint32_t indices[2] = {top2.best_index, top2.second_index};
    for (uint32_t neighbor = 0; neighbor < top2.neighbor_count; ++neighbor) {
      double distance = cv::norm(matrix_a.row(static_cast<int>(query)),
                                 matrix_b.row(static_cast<int>(indices[neighbor])),
                                 cv::NORM_L2);
      finalization_checksum += distance;
      if (query < cpu_output.size() && neighbor < cpu_output[query].size() &&
          indices[neighbor] ==
              static_cast<uint32_t>(cpu_output[query][neighbor].trainIdx)) {
        ++compared_final_distances;
        if (static_cast<float>(distance) == cpu_output[query][neighbor].distance) {
          ++exact_final_distances;
        }
      }
    }
  }
  double finalization_ms = elapsed_ms(finalization_start);
  bool final_exact = compared_final_distances == count_a * 2U &&
                     exact_final_distances == compared_final_distances;
  if (!std::isfinite(finalization_checksum)) {
    lardon3d_orb_vulkan_backend_destroy(backend);
    return false;
  }
  std::printf("%s,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%s\n",
              rootsift ? "rootsift" : "sift", count_a, count_b, cpu_ms,
              cold_ms, after_orb_ms, warm_ms, median(gpu_samples),
              finalization_ms, cpu_ms / (warm_ms + finalization_ms),
              final_exact ? "yes" : "no");
  lardon3d_orb_vulkan_backend_destroy(backend);
  return true;
}

int main() {
  if (!lardon3d_vulkan_evidence_process_startup()) {
    std::fprintf(stderr,
                 "MESA_SHADER_CACHE_DISABLE must be true for safe CPU affinity\n");
    return EXIT_FAILURE;
  }
  cv::setNumThreads(12);
  std::printf("kind,count_a,count_b,cpu_ms,cold_ms,after_orb_ms,warm_ms,gpu_ms,"
              "finalization_ms,gain_with_finalization,final_exact\n");
  const uint32_t sizes[] = {256, 1024, 4096, 8192};
  for (bool rootsift : {false, true}) {
    for (uint32_t size : sizes) {
      if (!benchmark(size, size, rootsift)) {
        return EXIT_FAILURE;
      }
    }
  }
  const uint32_t asymmetric[][2] = {
      {256, 8192}, {1024, 4096}, {4096, 1024}, {8192, 256},
  };
  for (const auto &counts : asymmetric) {
    if (!benchmark(counts[0], counts[1], false)) {
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}
