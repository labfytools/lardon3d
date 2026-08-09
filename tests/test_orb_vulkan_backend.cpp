#include <lardon3d/orb_vulkan_backend.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kDescriptorBytes = 32;

static uint32_t next_random(uint32_t *state) {
  *state = *state * 1664525U + 1013904223U;
  return *state;
}

static std::vector<unsigned char> make_descriptors(uint32_t count, uint32_t seed) {
  std::vector<unsigned char> descriptors(static_cast<size_t>(count) * kDescriptorBytes);
  for (unsigned char &value : descriptors) {
    value = static_cast<unsigned char>(next_random(&seed) >> 24);
  }
  return descriptors;
}

static std::vector<Lardon3DOrbTop2> opencv_top2(
    const std::vector<unsigned char> &a, uint32_t count_a,
    const std::vector<unsigned char> &b, uint32_t count_b) {
  std::vector<Lardon3DOrbTop2> output(count_a);
  if (count_a == 0 || count_b == 0) {
    return output;
  }
  cv::Mat matrix_a(static_cast<int>(count_a), kDescriptorBytes, CV_8U,
                   const_cast<unsigned char *>(a.data()));
  cv::Mat matrix_b(static_cast<int>(count_b), kDescriptorBytes, CV_8U,
                   const_cast<unsigned char *>(b.data()));
  cv::BFMatcher matcher(cv::NORM_HAMMING, false);
  std::vector<std::vector<cv::DMatch>> matches;
  matcher.knnMatch(matrix_a, matrix_b, matches, 2);
  for (uint32_t query = 0; query < count_a; ++query) {
    const auto &knn = matches[query];
    output[query].neighbor_count = static_cast<uint32_t>(knn.size());
    if (!knn.empty()) {
      output[query].best_index = static_cast<uint32_t>(knn[0].trainIdx);
      output[query].best_distance = static_cast<uint32_t>(knn[0].distance);
    }
    if (knn.size() >= 2) {
      output[query].second_index = static_cast<uint32_t>(knn[1].trainIdx);
      output[query].second_distance = static_cast<uint32_t>(knn[1].distance);
    }
  }
  return output;
}

static bool equal_top2(const Lardon3DOrbTop2 &a, const Lardon3DOrbTop2 &b) {
  return a.neighbor_count == b.neighbor_count && a.best_index == b.best_index &&
         a.best_distance == b.best_distance && a.second_index == b.second_index &&
         a.second_distance == b.second_distance;
}

static bool check_case(Lardon3DOrbVulkanBackend *backend, uint32_t count_a,
                       uint32_t count_b, uint32_t seed) {
  std::vector<unsigned char> a = make_descriptors(count_a, seed);
  std::vector<unsigned char> b = make_descriptors(count_b, seed ^ 0xa5a5a5a5U);
  std::vector<Lardon3DOrbTop2> expected = opencv_top2(a, count_a, b, count_b);
  std::vector<Lardon3DOrbTop2> actual(count_a);
  Lardon3DOrbVulkanResult result = lardon3d_orb_vulkan_top2(
      backend, a.data(), count_a, b.data(), count_b, actual.data(), actual.size());
  if (result != LARDON3D_ORB_VULKAN_OK) {
    std::fprintf(stderr, "Vulkan top-2 failed for %u x %u: %d\n", count_a,
                 count_b, static_cast<int>(result));
    return false;
  }
  for (uint32_t query = 0; query < count_a; ++query) {
    if (!equal_top2(expected[query], actual[query])) {
      std::fprintf(stderr,
                   "top-2 mismatch at %u for %u x %u: "
                   "CPU=(%u,%u,%u,%u,%u) Vulkan=(%u,%u,%u,%u,%u)\n",
                   query, count_a, count_b, expected[query].neighbor_count,
                   expected[query].best_index, expected[query].best_distance,
                   expected[query].second_index, expected[query].second_distance,
                   actual[query].neighbor_count, actual[query].best_index,
                   actual[query].best_distance, actual[query].second_index,
                   actual[query].second_distance);
      return false;
    }
  }
  return true;
}

static bool check_ties(Lardon3DOrbVulkanBackend *backend) {
  constexpr uint32_t count_a = 4;
  constexpr uint32_t count_b = 16;
  std::vector<unsigned char> a(count_a * kDescriptorBytes, 0);
  std::vector<unsigned char> b(count_b * kDescriptorBytes, 0);
  std::memset(b.data() + 7 * kDescriptorBytes, 0xff, kDescriptorBytes);
  std::memset(a.data() + 3 * kDescriptorBytes, 0xff, kDescriptorBytes);
  std::vector<Lardon3DOrbTop2> expected = opencv_top2(a, count_a, b, count_b);
  std::vector<Lardon3DOrbTop2> actual(count_a);
  if (lardon3d_orb_vulkan_top2(backend, a.data(), count_a, b.data(), count_b,
                               actual.data(), actual.size()) !=
      LARDON3D_ORB_VULKAN_OK) {
    return false;
  }
  return std::equal(expected.begin(), expected.end(), actual.begin(), equal_top2);
}

static bool check_serialized_threads(Lardon3DOrbVulkanBackend *backend) {
  std::vector<unsigned char> a = make_descriptors(1024, 0x11111111U);
  std::vector<unsigned char> b = make_descriptors(1024, 0x22222222U);
  std::vector<Lardon3DOrbTop2> expected_a = opencv_top2(a, 1024, b, 1024);
  std::vector<Lardon3DOrbTop2> expected_b = opencv_top2(b, 1024, a, 1024);
  std::vector<Lardon3DOrbTop2> actual_a(1024);
  std::vector<Lardon3DOrbTop2> actual_b(1024);
  Lardon3DOrbVulkanResult result_a = LARDON3D_ORB_VULKAN_FAILED;
  Lardon3DOrbVulkanResult result_b = LARDON3D_ORB_VULKAN_FAILED;
  std::thread thread_a([&] {
    result_a = lardon3d_orb_vulkan_top2(backend, a.data(), 1024, b.data(), 1024,
                                        actual_a.data(), actual_a.size());
  });
  std::thread thread_b([&] {
    result_b = lardon3d_orb_vulkan_top2(backend, b.data(), 1024, a.data(), 1024,
                                        actual_b.data(), actual_b.size());
  });
  thread_a.join();
  thread_b.join();
  return result_a == LARDON3D_ORB_VULKAN_OK &&
         result_b == LARDON3D_ORB_VULKAN_OK &&
         std::equal(expected_a.begin(), expected_a.end(), actual_a.begin(), equal_top2) &&
         std::equal(expected_b.begin(), expected_b.end(), actual_b.begin(), equal_top2);
}

static bool check_cached_unavailable() {
  if (setenv("LARDON3D_VULKAN_DISABLE", "1", 1) != 0) {
    return false;
  }
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  std::vector<unsigned char> descriptors = make_descriptors(1024, 0x10101010U);
  std::vector<Lardon3DOrbTop2> output(1024);
  Lardon3DOrbVulkanResult first = lardon3d_orb_vulkan_top2(
      backend, descriptors.data(), 1024, descriptors.data(), 1024, output.data(),
      output.size());
  unsetenv("LARDON3D_VULKAN_DISABLE");
  Lardon3DOrbVulkanResult second = lardon3d_orb_vulkan_top2(
      backend, descriptors.data(), 1024, descriptors.data(), 1024, output.data(),
      output.size());
  lardon3d_orb_vulkan_backend_destroy(backend);
  return first == LARDON3D_ORB_VULKAN_UNAVAILABLE &&
         second == LARDON3D_ORB_VULKAN_UNAVAILABLE;
}

static bool check_invalid_inputs(Lardon3DOrbVulkanBackend *backend) {
  unsigned char descriptor[kDescriptorBytes]{};
  Lardon3DOrbTop2 output{};
  return lardon3d_orb_vulkan_top2(nullptr, descriptor, 1, descriptor, 1, &output, 1) ==
             LARDON3D_ORB_VULKAN_INVALID_ARGUMENT &&
         lardon3d_orb_vulkan_top2(backend, descriptor, 8193, descriptor, 1, &output,
                                  1) == LARDON3D_ORB_VULKAN_INVALID_ARGUMENT &&
         lardon3d_orb_vulkan_top2(backend, descriptor, 1, descriptor, 1, &output,
                                  0) == LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
}

#ifdef LARDON3D_ORB_VULKAN_TESTING
static bool check_cached_device_failure() {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  std::vector<unsigned char> descriptors = make_descriptors(1024, 0x20202020U);
  std::vector<Lardon3DOrbTop2> output(1024);
  if (setenv("LARDON3D_TEST_VULKAN_DEVICE_LOST", "1", 1) != 0) {
    return false;
  }
  Lardon3DOrbVulkanResult first = lardon3d_orb_vulkan_top2(
      backend, descriptors.data(), 1024, descriptors.data(), 1024, output.data(),
      output.size());
  unsetenv("LARDON3D_TEST_VULKAN_DEVICE_LOST");
  Lardon3DOrbVulkanResult second = lardon3d_orb_vulkan_top2(
      backend, descriptors.data(), 1024, descriptors.data(), 1024, output.data(),
      output.size());
  lardon3d_orb_vulkan_backend_destroy(backend);
  return first == LARDON3D_ORB_VULKAN_FAILED &&
         second == LARDON3D_ORB_VULKAN_UNAVAILABLE;
}
#endif

}  // namespace

int main() {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) {
    return 1;
  }
  const uint32_t sizes[] = {0, 1, 2, 16, 64, 256, 1024, 4096, 8192};
  bool ok = true;
  for (uint32_t size : sizes) {
    ok = check_case(backend, size, size, 0x12345678U + size) && ok;
  }
  ok = check_case(backend, 64, 1, 0x99112233U) && ok;
  ok = check_case(backend, 64, 2, 0x88112233U) && ok;
  ok = check_ties(backend) && ok;
  ok = check_invalid_inputs(backend) && ok;
  ok = check_serialized_threads(backend) && ok;
  ok = !lardon3d_orb_vulkan_should_use(256, 256) && ok;
  ok = !lardon3d_orb_vulkan_should_use(512, 512) && ok;
  ok = lardon3d_orb_vulkan_should_use(768, 768) && ok;
  ok = lardon3d_orb_vulkan_should_use(1024, 1024) && ok;

  Lardon3DOrbVulkanInfo info{};
  ok = lardon3d_orb_vulkan_backend_info(backend, &info) && info.available && ok;
  if (info.available) {
    std::printf("device=%s workgroup=%u payload=%llu init_ms=%.3f gpu_ms=%.3f\n",
                info.device_name, info.workgroup_size,
                static_cast<unsigned long long>(info.permanent_payload_bytes),
                static_cast<double>(info.initialization_ns) / 1.0e6,
                static_cast<double>(info.gpu_ns) / 1.0e6);
  }
  lardon3d_orb_vulkan_backend_destroy(backend);
  ok = check_cached_unavailable() && ok;
#ifdef LARDON3D_ORB_VULKAN_TESTING
  ok = check_cached_device_failure() && ok;
#endif
  return ok ? 0 : 1;
}
