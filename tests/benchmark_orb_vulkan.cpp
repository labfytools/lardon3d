#include <lardon3d/orb_vulkan_backend.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

constexpr uint32_t kDescriptorBytes = 32;

static uint32_t next_random(uint32_t *state) {
  *state = *state * 1664525U + 1013904223U;
  return *state;
}

static std::vector<unsigned char> descriptors(uint32_t count, uint32_t seed) {
  std::vector<unsigned char> result(static_cast<size_t>(count) * kDescriptorBytes);
  for (unsigned char &value : result) {
    value = static_cast<unsigned char>(next_random(&seed) >> 24);
  }
  return result;
}

template <typename Function>
static double milliseconds(Function function) {
  auto start = std::chrono::steady_clock::now();
  function();
  auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

static double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

static double benchmark_cpu(const std::vector<unsigned char> &a,
                            const std::vector<unsigned char> &b, uint32_t count) {
  cv::Mat matrix_a(static_cast<int>(count), kDescriptorBytes, CV_8U,
                   const_cast<unsigned char *>(a.data()));
  cv::Mat matrix_b(static_cast<int>(count), kDescriptorBytes, CV_8U,
                   const_cast<unsigned char *>(b.data()));
  cv::BFMatcher matcher(cv::NORM_HAMMING, false);
  std::vector<double> samples;
  std::vector<std::vector<cv::DMatch>> output;
  matcher.knnMatch(matrix_a, matrix_b, output, 2);
  for (int repetition = 0; repetition < 7; ++repetition) {
    samples.push_back(milliseconds([&] {
      output.clear();
      matcher.knnMatch(matrix_a, matrix_b, output, 2);
    }));
  }
  return median(samples);
}

static bool benchmark_size(Lardon3DOrbVulkanBackend *backend, uint32_t count) {
  std::vector<unsigned char> a = descriptors(count, 0x12340000U + count);
  std::vector<unsigned char> b = descriptors(count, 0xabcd0000U + count);
  std::vector<Lardon3DOrbTop2> output(count);
  double cpu_ms = benchmark_cpu(a, b, count);
  std::vector<double> samples;
  for (int repetition = 0; repetition < 8; ++repetition) {
    Lardon3DOrbVulkanResult result = LARDON3D_ORB_VULKAN_FAILED;
    double elapsed = milliseconds([&] {
      result = lardon3d_orb_vulkan_top2(backend, a.data(), count, b.data(), count,
                                        output.data(), output.size());
    });
    if (result != LARDON3D_ORB_VULKAN_OK) {
      return false;
    }
    if (repetition > 0) {
      samples.push_back(elapsed);
    }
  }
  Lardon3DOrbVulkanInfo info{};
  if (!lardon3d_orb_vulkan_backend_info(backend, &info)) {
    return false;
  }
  std::printf("%u cpu_ms=%.3f vulkan_ms=%.3f submit_ms=%.3f gpu_ms=%.3f "
              "selector=%s\n",
              count, cpu_ms, median(samples),
              static_cast<double>(info.dispatch_ns) / 1.0e6,
              static_cast<double>(info.gpu_ns) / 1.0e6,
              lardon3d_orb_vulkan_should_use(count, count) ? "vulkan" : "cpu");
  return true;
}

static bool benchmark_sustained(Lardon3DOrbVulkanBackend *backend) {
  const uint32_t sizes[] = {1024, 4096, 8192, 4096};
  std::vector<std::vector<unsigned char>> inputs_a;
  std::vector<std::vector<unsigned char>> inputs_b;
  std::vector<std::vector<Lardon3DOrbTop2>> outputs;
  for (uint32_t size : sizes) {
    inputs_a.push_back(descriptors(size, 0x98760000U + size));
    inputs_b.push_back(descriptors(size, 0x67890000U + size));
    outputs.emplace_back(size);
  }
  constexpr int repetitions = 5000;
  double elapsed = milliseconds([&] {
    for (int repetition = 0; repetition < repetitions; ++repetition) {
      size_t slot = static_cast<size_t>(repetition) % std::size(sizes);
      uint32_t size = sizes[slot];
      Lardon3DOrbVulkanResult result = lardon3d_orb_vulkan_top2(
          backend, inputs_a[slot].data(), size, inputs_b[slot].data(), size,
          outputs[slot].data(), outputs[slot].size());
      if (result != LARDON3D_ORB_VULKAN_OK) {
        std::fprintf(stderr, "sustained dispatch failed at repetition %d\n", repetition);
        std::abort();
      }
    }
  });
  std::printf("sustained jobs=%d total_ms=%.3f pairs_per_second=%.3f\n", repetitions,
              elapsed, repetitions * 1000.0 / elapsed);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) {
    return 1;
  }
  bool ok = true;
  const uint32_t sizes[] = {256, 512, 768, 1024, 4096, 8192};
  for (uint32_t size : sizes) {
    ok = benchmark_size(backend, size) && ok;
  }
  if (argc == 2 && std::strcmp(argv[1], "--sustained") == 0) {
    ok = benchmark_sustained(backend) && ok;
  }
  Lardon3DOrbVulkanInfo info{};
  if (lardon3d_orb_vulkan_backend_info(backend, &info)) {
    std::printf("device=%s workgroup=%u cold_init_ms=%.3f payload=%llu\n",
                info.device_name, info.workgroup_size,
                static_cast<double>(info.initialization_ns) / 1.0e6,
                static_cast<unsigned long long>(info.permanent_payload_bytes));
  }
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok ? 0 : 1;
}
