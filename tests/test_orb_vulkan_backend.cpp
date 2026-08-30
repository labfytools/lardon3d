#include <lardon3d/orb_vulkan_backend.h>

#include "../src/orb_vulkan_backend_internal.h"

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include <algorithm>
#include <atomic>
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
  for (unsigned int iteration = 0; iteration < 32; ++iteration) {
    std::vector<Lardon3DOrbTop2> actual_a(1024);
    std::vector<Lardon3DOrbTop2> actual_b(1024);
    Lardon3DOrbVulkanResult result_a = LARDON3D_ORB_VULKAN_FAILED;
    Lardon3DOrbVulkanResult result_b = LARDON3D_ORB_VULKAN_FAILED;
    std::atomic<unsigned int> ready{0};
    std::atomic<bool> start{false};
    auto await_start = [&] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    };
    std::thread thread_a([&] {
      await_start();
      result_a = lardon3d_orb_vulkan_top2(
          backend, a.data(), 1024, b.data(), 1024, actual_a.data(),
          actual_a.size());
    });
    std::thread thread_b([&] {
      await_start();
      result_b = lardon3d_orb_vulkan_top2(
          backend, b.data(), 1024, a.data(), 1024, actual_b.data(),
          actual_b.size());
    });
    while (ready.load(std::memory_order_acquire) != 2) {
      std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    thread_a.join();
    thread_b.join();
    /* Both calls start from the same gate and repeat enough times to exercise
     * the public transaction boundary without timing sleeps. Each result is
     * compared with its own directional CPU reference, catching cross-call
     * request/output substitution as well as spurious pending-slot failure. */
    if (result_a != LARDON3D_ORB_VULKAN_OK ||
        result_b != LARDON3D_ORB_VULKAN_OK ||
        !std::equal(expected_a.begin(), expected_a.end(), actual_a.begin(),
                    equal_top2) ||
        !std::equal(expected_b.begin(), expected_b.end(), actual_b.begin(),
                    equal_top2)) {
      return false;
    }
  }
  return true;
}

static bool check_driver_policy_case(const char *value, bool private_begin) {
  if ((value && setenv("MESA_SHADER_CACHE_DISABLE", value, 1) != 0) ||
      (!value && unsetenv("MESA_SHADER_CACHE_DISABLE") != 0)) {
    return false;
  }
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) return false;
  unsigned char descriptor[kDescriptorBytes]{};
  Lardon3DOrbTop2 output{7, 11, 13, 17, 19};
  const Lardon3DOrbTop2 unchanged = output;
  Lardon3DOrbVulkanInfo before{};
  Lardon3DOrbVulkanInfo after{};
  Lardon3DOrbVulkanRequest request{};
  bool ok = lardon3d_orb_vulkan_backend_info(backend, &before) &&
            !before.initialized && !before.available;
  Lardon3DOrbVulkanResult result = private_begin
      ? lardon3d_orb_vulkan_internal_top2_begin(
            backend, descriptor, 1, descriptor, 1, &request)
      : lardon3d_orb_vulkan_top2(
            backend, descriptor, 1, descriptor, 1, &output, 1);
  ok = ok && result == LARDON3D_ORB_VULKAN_UNAVAILABLE &&
       std::memcmp(&output, &unchanged, sizeof(output)) == 0 &&
       lardon3d_orb_vulkan_backend_info(backend, &after) && after.initialized &&
       !after.available;
  const char *retained = std::getenv("MESA_SHADER_CACHE_DISABLE");
  ok = ok && ((!value && !retained) ||
              (value && retained && std::strcmp(value, retained) == 0));
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok;
}

static bool check_driver_policy_gate() {
  /* Meson's safe test environment must not mask these late-boundary cases.
   * Every case uses a fresh backend before any successful Vulkan request, so a
   * failure proves rejection precedes Mesa rather than observing cached state. */
  bool ok = check_driver_policy_case(nullptr, false) &&
            check_driver_policy_case("false", true) &&
            check_driver_policy_case("malformed", false);
  return setenv("MESA_SHADER_CACHE_DISABLE", "true", 1) == 0 && ok;
}

static bool check_current_driver_policy_rejection() {
  const char *value = std::getenv("MESA_SHADER_CACHE_DISABLE");
  if (value && (std::strcmp(value, "true") == 0 ||
                std::strcmp(value, "1") == 0)) {
    return false;
  }
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) return false;
  unsigned char descriptor[kDescriptorBytes]{};
  Lardon3DOrbTop2 output{7, 11, 13, 17, 19};
  const Lardon3DOrbTop2 unchanged = output;
  Lardon3DOrbVulkanResult result = lardon3d_orb_vulkan_top2(
      backend, descriptor, 1, descriptor, 1, &output, 1);
  Lardon3DOrbVulkanInfo info{};
  bool ok = result == LARDON3D_ORB_VULKAN_UNAVAILABLE &&
            std::memcmp(&output, &unchanged, sizeof(output)) == 0 &&
            lardon3d_orb_vulkan_backend_info(backend, &info) &&
            info.initialized && !info.available;
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok;
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

static bool check_private_slot_contract(Lardon3DOrbVulkanBackend *backend) {
  constexpr uint32_t count = 1024;
  std::vector<unsigned char> a = make_descriptors(count, 0x30303030U);
  std::vector<unsigned char> b = make_descriptors(count, 0x40404040U);
  std::vector<Lardon3DOrbTop2> expected = opencv_top2(a, count, b, count);
  std::vector<Lardon3DOrbTop2> actual(count);
  Lardon3DOrbVulkanRequest invalid{};

  if (lardon3d_orb_vulkan_internal_top2_finish(
          backend, &invalid, actual.data(), actual.size()) !=
      LARDON3D_ORB_VULKAN_INVALID_ARGUMENT) {
    return false;
  }
  Lardon3DOrbVulkanRequest first{};
  if (lardon3d_orb_vulkan_internal_top2_begin(
          backend, a.data(), count, b.data(), count, &first) !=
      LARDON3D_ORB_VULKAN_OK) {
    return false;
  }
  /* Invalid finish output still consumes its exact request. A following
   * submit proves that slot was not stranded by argument validation. */
  if (lardon3d_orb_vulkan_internal_top2_finish(
          backend, &first, nullptr, 0) !=
          LARDON3D_ORB_VULKAN_INVALID_ARGUMENT ||
      lardon3d_orb_vulkan_internal_top2_finish(
          backend, &first, actual.data(), actual.size()) !=
          LARDON3D_ORB_VULKAN_FAILED) {
    return false;
  }
  Lardon3DOrbVulkanRequest second{};
  if (
      lardon3d_orb_vulkan_internal_top2_begin(
          backend, a.data(), count, b.data(), count, &second) !=
          LARDON3D_ORB_VULKAN_OK ||
      lardon3d_orb_vulkan_internal_top2_finish(
          backend, &second, actual.data(), actual.size()) !=
          LARDON3D_ORB_VULKAN_OK) {
    return false;
  }
  if (!std::equal(expected.begin(), expected.end(), actual.begin(), equal_top2)) {
    return false;
  }
  Lardon3DOrbVulkanRequest discarded{};
  Lardon3DOrbVulkanRequest reused{};
  if (lardon3d_orb_vulkan_internal_top2_begin(
          backend, a.data(), count, b.data(), count, &discarded) !=
          LARDON3D_ORB_VULKAN_OK ||
      lardon3d_orb_vulkan_internal_top2_discard(backend, &discarded) !=
          LARDON3D_ORB_VULKAN_OK ||
      lardon3d_orb_vulkan_internal_top2_begin(
          backend, a.data(), count, b.data(), count, &reused) !=
          LARDON3D_ORB_VULKAN_OK ||
      reused.slot != discarded.slot || reused.generation == discarded.generation ||
      lardon3d_orb_vulkan_internal_top2_finish(
          backend, &reused, actual.data(), actual.size()) !=
          LARDON3D_ORB_VULKAN_OK) {
    return false;
  }
  return std::equal(expected.begin(), expected.end(), actual.begin(), equal_top2);
}

static bool check_two_request_identity(Lardon3DOrbVulkanBackend *backend) {
  constexpr uint32_t count = 1024;
  std::vector<unsigned char> a = make_descriptors(count, 0x71717171U);
  std::vector<unsigned char> b = make_descriptors(count, 0x72727272U);
  std::vector<unsigned char> c = make_descriptors(count, 0x73737373U);
  std::vector<Lardon3DOrbTop2> expected_ab = opencv_top2(a, count, b, count);
  std::vector<Lardon3DOrbTop2> expected_cb = opencv_top2(c, count, b, count);
  std::vector<Lardon3DOrbTop2> actual_ab(count);
  std::vector<Lardon3DOrbTop2> actual_cb(count);
  Lardon3DOrbVulkanRequest request_ab{};
  Lardon3DOrbVulkanRequest request_cb{};
  Lardon3DOrbVulkanRequest third{};
  if (!lardon3d_orb_vulkan_internal_begin_sequence(
          backend, LARDON3D_ORB_VULKAN_MAX_INFLIGHT)) {
    return false;
  }
  bool ok = lardon3d_orb_vulkan_internal_top2_begin(
          backend, a.data(), count, b.data(), count, &request_ab) ==
          LARDON3D_ORB_VULKAN_OK &&
      lardon3d_orb_vulkan_internal_top2_begin(
          backend, c.data(), count, b.data(), count, &request_cb) ==
          LARDON3D_ORB_VULKAN_OK &&
      request_ab.slot != request_cb.slot &&
      lardon3d_orb_vulkan_internal_top2_begin(
          backend, a.data(), count, c.data(), count, &third) ==
          LARDON3D_ORB_VULKAN_FAILED;
  Lardon3DOrbVulkanRequest mismatched = request_ab;
  mismatched.generation = request_cb.generation + 1;
  if (mismatched.generation == 0) mismatched.generation = 1;
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_top2_finish(
             backend, &mismatched, actual_ab.data(), actual_ab.size()) ==
             LARDON3D_ORB_VULKAN_FAILED
        && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &request_cb, actual_cb.data(), actual_cb.size()) ==
             LARDON3D_ORB_VULKAN_OK
        && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &request_ab, actual_ab.data(), actual_ab.size()) ==
             LARDON3D_ORB_VULKAN_OK;
  } else {
    if (request_ab.generation != 0) {
      (void)lardon3d_orb_vulkan_internal_top2_discard(backend, &request_ab);
    }
    if (request_cb.generation != 0) {
      (void)lardon3d_orb_vulkan_internal_top2_discard(backend, &request_cb);
    }
  }
  bool ended = lardon3d_orb_vulkan_internal_end_sequence(backend);
  return ok && ended
      && std::equal(expected_ab.begin(), expected_ab.end(), actual_ab.begin(),
                    equal_top2)
      && std::equal(expected_cb.begin(), expected_cb.end(), actual_cb.begin(),
                    equal_top2);
}

static bool check_private_telemetry_lifecycle() {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) return false;
  constexpr uint32_t count = 1024;
  std::vector<unsigned char> descriptors =
      make_descriptors(count, 0x60606060U);
  std::vector<Lardon3DOrbTop2> output(count);
  Lardon3DOrbVulkanTelemetry telemetry{};
  Lardon3DOrbVulkanRequest first{};
  bool ok = lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
      && telemetry.submits == 0 && telemetry.completions == 0
      && !telemetry.slot_pending && telemetry.pending_slots == 0;
  ok = ok && lardon3d_orb_vulkan_internal_top2_begin(
      backend, descriptors.data(), count, descriptors.data(), count, &first)
      == LARDON3D_ORB_VULKAN_OK;
  ok = ok && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
      && telemetry.submits == 1 && telemetry.completions == 0
      && telemetry.slot_pending && telemetry.pending_slots == 1
      && telemetry.serial > 0;
  ok = ok && lardon3d_orb_vulkan_internal_top2_finish(
      backend, &first, output.data(), output.size()) == LARDON3D_ORB_VULKAN_OK;
  ok = ok && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
      && telemetry.submits == 1 && telemetry.completions == 1
      && !telemetry.slot_pending && telemetry.pending_slots == 0;
  Lardon3DOrbVulkanRequest second{};
  ok = ok && lardon3d_orb_vulkan_internal_top2_begin(
      backend, descriptors.data(), count, descriptors.data(), count, &second)
      == LARDON3D_ORB_VULKAN_OK;
  ok = ok && lardon3d_orb_vulkan_internal_top2_discard(backend, &second)
      == LARDON3D_ORB_VULKAN_OK;
  ok = ok && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
      && telemetry.submits == 2 && telemetry.completions == 1
      && telemetry.discards == 1 && !telemetry.slot_pending
      && telemetry.pending_slots == 0;
  /* Slot reuse after discard remains operational and metrics remain
   * cumulative; no history allocation or public info-struct change is used. */
  Lardon3DOrbVulkanRequest third{};
  ok = ok && lardon3d_orb_vulkan_internal_top2_begin(
      backend, descriptors.data(), count, descriptors.data(), count, &third)
      == LARDON3D_ORB_VULKAN_OK;
  ok = ok && lardon3d_orb_vulkan_internal_top2_finish(
      backend, &third, output.data(), output.size()) == LARDON3D_ORB_VULKAN_OK;
  ok = ok && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
      && telemetry.submits == 3 && telemetry.completions == 2
      && telemetry.discards == 1 && !telemetry.slot_pending;
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok;
}

#ifdef LARDON3D_ORB_VULKAN_TESTING
static bool check_capacity_lifecycle() {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) return false;
  std::vector<unsigned char> descriptors = make_descriptors(1024, 0x81818181U);
  std::vector<Lardon3DOrbTop2> output(1024);
  Lardon3DOrbVulkanInfo info{};
  Lardon3DOrbVulkanTelemetry telemetry{};
  bool ok = lardon3d_orb_vulkan_backend_info(backend, &info)
      && !info.initialized && info.permanent_payload_bytes == 0
      && lardon3d_orb_vulkan_top2(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             output.data(), output.size()) == LARDON3D_ORB_VULKAN_OK
      && lardon3d_orb_vulkan_backend_info(backend, &info)
      && info.permanent_payload_bytes ==
          LARDON3D_ORB_VULKAN_PER_SLOT_BYTES
      && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
      && telemetry.retained_capacity == 1
      && telemetry.retained_payload_bytes ==
          LARDON3D_ORB_VULKAN_PER_SLOT_BYTES
      && !telemetry.sequence_capacity_active;

  if (ok) {
    ok = setenv("LARDON3D_TEST_VULKAN_SLOT_ALLOCATION_FAILURE", "1", 1) == 0
        && !lardon3d_orb_vulkan_internal_begin_sequence(backend, 2)
        && lardon3d_orb_vulkan_backend_info(backend, &info)
        && info.permanent_payload_bytes ==
            LARDON3D_ORB_VULKAN_PER_SLOT_BYTES
        && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
        && telemetry.retained_capacity == 1
        && !telemetry.sequence_capacity_active;
  }
  bool environment_restored =
      unsetenv("LARDON3D_TEST_VULKAN_SLOT_ALLOCATION_FAILURE") == 0;
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_begin_sequence(backend, 2)
        && lardon3d_orb_vulkan_backend_info(backend, &info)
        && info.permanent_payload_bytes ==
            2 * LARDON3D_ORB_VULKAN_PER_SLOT_BYTES
        && lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
        && telemetry.retained_capacity == 2
        && telemetry.sequence_capacity_active
        && lardon3d_orb_vulkan_internal_end_sequence(backend)
        && lardon3d_orb_vulkan_backend_info(backend, &info)
        && info.permanent_payload_bytes ==
            LARDON3D_ORB_VULKAN_PER_SLOT_BYTES;
  }

  Lardon3DOrbVulkanRequest pending{};
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_begin_sequence(backend, 1)
        && lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &pending) == LARDON3D_ORB_VULKAN_OK
        && !lardon3d_orb_vulkan_internal_end_sequence(backend)
        && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &pending, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_OK
        && lardon3d_orb_vulkan_internal_end_sequence(backend);
  }
  lardon3d_orb_vulkan_backend_destroy(backend);
  return environment_restored && ok;
}

static bool check_generation_saturation() {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) return false;
  std::vector<unsigned char> descriptors = make_descriptors(1024, 0x91919191U);
  std::vector<Lardon3DOrbTop2> output(1024);
  Lardon3DOrbVulkanRequest ancient{};
  Lardon3DOrbVulkanRequest terminal{};
  Lardon3DOrbVulkanRequest future{};
  bool ok = lardon3d_orb_vulkan_internal_begin_sequence(backend, 2)
      && lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &ancient) == LARDON3D_ORB_VULKAN_OK
      && ancient.generation == 1
      && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &ancient, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_OK
      && lardon3d_orb_vulkan_internal_test_set_slot_generation(
             backend, ancient.slot, UINT64_MAX - 1)
      /* UINT64_MAX is the terminal valid request identity. The following
       * begin must issue it exactly once; only the subsequent begin retires
       * this slot, before any submission could wrap to generation one. */
      && lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &terminal) == LARDON3D_ORB_VULKAN_OK
      && terminal.slot == ancient.slot && terminal.generation == UINT64_MAX
      && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &terminal, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_OK
      && lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &future) == LARDON3D_ORB_VULKAN_OK
      && future.slot != ancient.slot && future.generation == 1
      && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &ancient, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_FAILED;
  Lardon3DOrbVulkanTelemetry telemetry{};
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
        && telemetry.pending_slots == 1
        && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &future, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_OK
        && lardon3d_orb_vulkan_internal_end_sequence(backend);
  }
  Lardon3DOrbVulkanRequest after_boundary{};
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_begin_sequence(backend, 1)
        && lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &after_boundary) == LARDON3D_ORB_VULKAN_OK
        && after_boundary.slot == future.slot
        && after_boundary.generation == 2
        && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &ancient, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_FAILED
        && lardon3d_orb_vulkan_internal_top2_finish(
             backend, &after_boundary, output.data(), output.size()) ==
             LARDON3D_ORB_VULKAN_OK
        && lardon3d_orb_vulkan_internal_end_sequence(backend);
  }
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok;
}

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


static bool check_discard_wait_failure() {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) {
    return false;
  }
  std::vector<unsigned char> descriptors = make_descriptors(1024, 0x50505050U);
  Lardon3DOrbVulkanRequest request{};
  bool ok = lardon3d_orb_vulkan_internal_top2_begin(
                backend, descriptors.data(), 1024, descriptors.data(), 1024,
                &request) ==
            LARDON3D_ORB_VULKAN_OK;
  if (ok && setenv("LARDON3D_TEST_VULKAN_WAIT_FAILURE", "1", 1) != 0) {
    ok = false;
  }
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_top2_discard(backend, &request) ==
         LARDON3D_ORB_VULKAN_FAILED;
  }
  unsetenv("LARDON3D_TEST_VULKAN_WAIT_FAILURE");
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &request) ==
         LARDON3D_ORB_VULKAN_UNAVAILABLE;
  }
  Lardon3DOrbVulkanTelemetry telemetry{};
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
        && telemetry.submits == 1 && telemetry.discards == 1
        && telemetry.failures == 1 && !telemetry.slot_pending;
  }
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok;
}

static bool check_finish_failure(const char *failure_variable,
                                 uint64_t expected_completions) {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend || !failure_variable) {
    lardon3d_orb_vulkan_backend_destroy(backend);
    return false;
  }
  std::vector<unsigned char> descriptors = make_descriptors(1024, 0x61616161U);
  std::vector<Lardon3DOrbTop2> output(1024);
  Lardon3DOrbVulkanRequest request{};
  bool ok = lardon3d_orb_vulkan_internal_top2_begin(
                backend, descriptors.data(), 1024, descriptors.data(), 1024,
                &request) == LARDON3D_ORB_VULKAN_OK
      && setenv(failure_variable, "1", 1) == 0;
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_top2_finish(
             backend, &request, output.data(), output.size()) ==
         LARDON3D_ORB_VULKAN_FAILED;
  }
  bool environment_restored = unsetenv(failure_variable) == 0;
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_top2_begin(
             backend, descriptors.data(), 1024, descriptors.data(), 1024,
             &request) == LARDON3D_ORB_VULKAN_UNAVAILABLE;
  }
  Lardon3DOrbVulkanTelemetry telemetry{};
  if (ok) {
    ok = lardon3d_orb_vulkan_internal_telemetry(backend, &telemetry)
        && telemetry.submits == 1
        && telemetry.completions == expected_completions
        && telemetry.failures == 1 && telemetry.pending_slots == 0
        && !telemetry.slot_pending;
  }
  lardon3d_orb_vulkan_backend_destroy(backend);
  return environment_restored && ok;
}
#endif

}  // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--unsafe-policy-only") == 0) {
    return check_current_driver_policy_rejection() ? 0 : 1;
  }
  if (argc != 1) return 2;
  if (!check_driver_policy_gate()) {
    std::fprintf(stderr, "Vulkan driver process-policy gate failed\n");
    return 1;
  }
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
  const bool private_slot_ok = check_private_slot_contract(backend);
  if (!private_slot_ok) {
    std::fprintf(stderr, "private Vulkan slot contract failed\n");
  }
  ok = private_slot_ok && ok;
  const bool two_request_ok = check_two_request_identity(backend);
  if (!two_request_ok) {
    std::fprintf(stderr, "private Vulkan two-request identity failed\n");
  }
  ok = two_request_ok && ok;
  const bool telemetry_ok = check_private_telemetry_lifecycle();
  if (!telemetry_ok) {
    std::fprintf(stderr, "private Vulkan telemetry lifecycle failed\n");
  }
  ok = telemetry_ok && ok;
  ok = check_serialized_threads(backend) && ok;
  ok = !lardon3d_orb_vulkan_should_use(256, 256) && ok;
  ok = !lardon3d_orb_vulkan_should_use(512, 512) && ok;
  ok = lardon3d_orb_vulkan_should_use(768, 768) && ok;
  ok = lardon3d_orb_vulkan_should_use(1024, 1024) && ok;

  Lardon3DOrbVulkanInfo info{};
  ok = lardon3d_orb_vulkan_backend_info(backend, &info) && info.available &&
       info.permanent_payload_bytes ==
           LARDON3D_ORB_VULKAN_PER_SLOT_BYTES && ok;
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
  const bool capacity_ok = check_capacity_lifecycle();
  if (!capacity_ok) {
    std::fprintf(stderr, "Vulkan sequence-capacity lifecycle failed\n");
  }
  ok = capacity_ok && ok;
  const bool generation_ok = check_generation_saturation();
  if (!generation_ok) {
    std::fprintf(stderr, "Vulkan request generation saturation failed\n");
  }
  ok = generation_ok && ok;
  ok = check_cached_device_failure() && ok;
  const bool discard_wait_ok = check_discard_wait_failure();
  if (!discard_wait_ok) {
    std::fprintf(stderr, "Vulkan discard wait-failure contract failed\n");
  }
  ok = discard_wait_ok && ok;
  const bool finish_wait_ok = check_finish_failure(
      "LARDON3D_TEST_VULKAN_FINISH_WAIT_FAILURE", 0);
  if (!finish_wait_ok) {
    std::fprintf(stderr, "Vulkan finish wait-failure contract failed\n");
  }
  ok = finish_wait_ok && ok;
  const bool readback_ok = check_finish_failure(
      "LARDON3D_TEST_VULKAN_READBACK_FAILURE", 1);
  if (!readback_ok) {
    std::fprintf(stderr, "Vulkan readback-failure contract failed\n");
  }
  ok = readback_ok && ok;
#endif
  return ok ? 0 : 1;
}
