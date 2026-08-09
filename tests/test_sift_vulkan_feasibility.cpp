#include <lardon3d/orb_vulkan_backend.h>

extern "C" {
#include <lardon3d/match_file.h>
}

#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

struct Comparison {
  uint64_t queries = 0;
  uint64_t index_divergences = 0;
  uint64_t distance_bit_divergences = 0;
  uint64_t lowe_divergences = 0;
};

static uint32_t random_u32(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static bool fp64_requested() {
  const char *value = std::getenv("LARDON3D_VULKAN_SIFT_FP64");
  return value && std::strcmp(value, "1") == 0;
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

static bool compare_case(Lardon3DOrbVulkanBackend *backend,
                         const std::vector<float> &a,
                         const std::vector<float> &b,
                         Comparison *comparison) {
  uint32_t count_a = static_cast<uint32_t>(a.size() / 128);
  uint32_t count_b = static_cast<uint32_t>(b.size() / 128);
  cv::Mat matrix_a(static_cast<int>(count_a), 128, CV_32FC1,
                   const_cast<float *>(a.data()));
  cv::Mat matrix_b(static_cast<int>(count_b), 128, CV_32FC1,
                   const_cast<float *>(b.data()));
  cv::BFMatcher matcher(cv::NORM_L2, false);
  std::vector<std::vector<cv::DMatch>> expected;
  matcher.knnMatch(matrix_a, matrix_b, expected, 2);

  std::vector<Lardon3DSiftTop2> actual(count_a);
  if (lardon3d_sift_vulkan_top2(backend, a.data(), count_a, b.data(), count_b,
                                actual.data(), actual.size()) !=
      LARDON3D_ORB_VULKAN_OK) {
    return false;
  }
  for (uint32_t query = 0; query < count_a; ++query) {
    ++comparison->queries;
    const auto &cpu = expected[query];
    const auto &gpu = actual[query];
    uint32_t neighbors = static_cast<uint32_t>(cpu.size());
    bool indices_equal = gpu.neighbor_count == neighbors;
    if (neighbors > 0) {
      indices_equal = indices_equal &&
                      gpu.best_index == static_cast<uint32_t>(cpu[0].trainIdx);
    }
    if (neighbors > 1) {
      indices_equal = indices_equal &&
                      gpu.second_index == static_cast<uint32_t>(cpu[1].trainIdx);
    }
    if (!indices_equal) {
      ++comparison->index_divergences;
      if (comparison->index_divergences <= 4) {
        std::fprintf(stderr,
                     "index divergence q=%u cpu=(%d,%d) gpu=(%u,%u)\n",
                     query, cpu.empty() ? -1 : cpu[0].trainIdx,
                     cpu.size() < 2 ? -1 : cpu[1].trainIdx,
                     gpu.best_index, gpu.second_index);
      }
    }
    if (neighbors > 1) {
      float best = std::sqrt(gpu.best_squared_distance);
      float second = std::sqrt(gpu.second_squared_distance);
      if (best != cpu[0].distance || second != cpu[1].distance) {
        ++comparison->distance_bit_divergences;
      }
      bool cpu_lowe = cpu[1].distance != 0.0F &&
                      cpu[0].distance < 0.7F * cpu[1].distance;
      bool gpu_lowe = second != 0.0F && best < 0.7F * second;
      if (cpu_lowe != gpu_lowe) {
        ++comparison->lowe_divergences;
      }
    }
  }
  return true;
}

static bool files_and_hashes_differ(const char *path_a, const char *path_b) {
  std::array<unsigned char, 128> bytes_a{};
  std::array<unsigned char, 128> bytes_b{};
  int fd_a = open(path_a, O_RDONLY | O_CLOEXEC);
  int fd_b = open(path_b, O_RDONLY | O_CLOEXEC);
  if (fd_a < 0 || fd_b < 0) {
    if (fd_a >= 0) {
      close(fd_a);
    }
    if (fd_b >= 0) {
      close(fd_b);
    }
    return false;
  }
  ssize_t size_a = read(fd_a, bytes_a.data(), bytes_a.size());
  ssize_t size_b = read(fd_b, bytes_b.data(), bytes_b.size());
  close(fd_a);
  close(fd_b);
  if (size_a <= 0 || size_a != size_b) {
    return false;
  }

  unsigned char hash_a[EVP_MAX_MD_SIZE];
  unsigned char hash_b[EVP_MAX_MD_SIZE];
  unsigned int hash_size_a = 0;
  unsigned int hash_size_b = 0;
  bool hashed_a = EVP_Digest(bytes_a.data(), static_cast<size_t>(size_a), hash_a,
                             &hash_size_a, EVP_sha256(), nullptr) == 1;
  bool hashed_b = EVP_Digest(bytes_b.data(), static_cast<size_t>(size_b), hash_b,
                             &hash_size_b, EVP_sha256(), nullptr) == 1;
  return hashed_a && hashed_b &&
         std::memcmp(bytes_a.data(), bytes_b.data(), static_cast<size_t>(size_a)) != 0 &&
         (hash_size_a != hash_size_b ||
          std::memcmp(hash_a, hash_b, hash_size_a) != 0);
}

static bool match_file_difference(Lardon3DOrbVulkanBackend *backend) {
  std::vector<float> query(128, 0.0F);
  std::vector<float> candidates(2 * 128);
  for (size_t component = 0; component < 128; ++component) {
    float difference = component < 16 ? 0.25F :
                       component < 64 ? 0.00025F : 0.00000025F;
    candidates[component] = difference;
    candidates[128 + component] = difference * 2.0F;
  }
  cv::Mat matrix_a(1, 128, CV_32FC1, query.data());
  cv::Mat matrix_b(2, 128, CV_32FC1, candidates.data());
  cv::BFMatcher matcher(cv::NORM_L2, false);
  std::vector<std::vector<cv::DMatch>> cpu;
  matcher.knnMatch(matrix_a, matrix_b, cpu, 2);
  Lardon3DSiftTop2 gpu{};
  if (cpu.size() != 1 || cpu[0].size() != 2 ||
      lardon3d_sift_vulkan_top2(backend, query.data(), 1, candidates.data(), 2,
                                &gpu, 1) != LARDON3D_ORB_VULKAN_OK ||
      cpu[0][0].trainIdx != static_cast<int>(gpu.best_index) ||
      !(cpu[0][0].distance < 0.7F * cpu[0][1].distance)) {
    return false;
  }
  Lardon3DMatchFileEntry cpu_entry = {
      0, static_cast<uint32_t>(cpu[0][0].trainIdx), cpu[0][0].distance};
  Lardon3DMatchFileEntry gpu_entry = {
      0, gpu.best_index, std::sqrt(gpu.best_squared_distance)};
  if (cpu_entry.distance == gpu_entry.distance) {
    return false;
  }

  char cpu_path[] = "/tmp/lardon3d-sift-cpu-match-XXXXXX";
  char gpu_path[] = "/tmp/lardon3d-sift-gpu-match-XXXXXX";
  int cpu_fd = mkstemp(cpu_path);
  int gpu_fd = mkstemp(gpu_path);
  bool ok = cpu_fd >= 0 && gpu_fd >= 0;
  if (ok) {
    ok = lardon3d_match_file_write(cpu_fd, 2, 128, 1, 2, &cpu_entry, 1) ==
             LARDON3D_MATCH_FILE_OK &&
         lardon3d_match_file_write(gpu_fd, 2, 128, 1, 2, &gpu_entry, 1) ==
             LARDON3D_MATCH_FILE_OK;
  }
  if (cpu_fd >= 0) {
    close(cpu_fd);
  }
  if (gpu_fd >= 0) {
    close(gpu_fd);
  }
  ok = ok && files_and_hashes_differ(cpu_path, gpu_path);
  unlink(cpu_path);
  unlink(gpu_path);
  return ok;
}

static bool run_distribution(bool rootsift) {
  Lardon3DOrbVulkanBackend *backend = lardon3d_orb_vulkan_backend_create();
  if (!backend) {
    return false;
  }
  Comparison comparison;
  bool ok = true;
  const uint32_t sizes[] = {1, 2, 16, 64, 256, 1024, 8192};
  for (uint32_t size : sizes) {
    std::vector<float> a = make_descriptors(size, rootsift, 0x12345678U + size);
    std::vector<float> b = make_descriptors(size, rootsift, 0x87654321U + size);
    ok = compare_case(backend, a, b, &comparison) && ok;
  }

  std::vector<float> ties(8 * 128, 0.25F);
  ok = compare_case(backend, ties, ties, &comparison) && ok;

  std::vector<float> query(128, 0.0F);
  std::vector<float> near = make_descriptors(4096, false, 0x31415926U);
  for (float &value : near) {
    value = (value - 0.5F) * 0.0001F;
  }
  ok = compare_case(backend, query, near, &comparison) && ok;

  std::vector<float> equal_sums(1024 * 128);
  for (uint32_t row = 0; row < 1024; ++row) {
    uint32_t offset = (row * 37U) % 128U;
    for (uint32_t component = 0; component < 128; ++component) {
      uint32_t source = (component + offset) % 128U;
      float magnitude = source < 16U ? 0.25F :
                        source < 64U ? 0.00025F : 0.00000025F;
      equal_sums[static_cast<size_t>(row) * 128 + component] = magnitude;
    }
  }
  ok = compare_case(backend, query, equal_sums, &comparison) && ok;

  const float ratios[] = {
      std::nextafter(0.7F, 0.0F),
      0.7F,
      std::nextafter(0.7F, std::numeric_limits<float>::infinity()),
  };
  for (float ratio : ratios) {
    std::vector<float> boundary(2 * 128, 0.0F);
    boundary[0] = ratio;
    boundary[128] = 1.0F;
    ok = compare_case(backend, query, boundary, &comparison) && ok;
  }

  const uint32_t asymmetric[][2] = {
      {256, 8192}, {1024, 4096}, {4096, 1024}, {8192, 256},
  };
  for (const auto &counts : asymmetric) {
    std::vector<float> asymmetric_a =
        make_descriptors(counts[0], rootsift, 0x10203040U + counts[0]);
    std::vector<float> asymmetric_b =
        make_descriptors(counts[1], rootsift, 0x50607080U + counts[1]);
    ok = compare_case(backend, asymmetric_a, asymmetric_b, &comparison) && ok;
  }

  cv::Mat image(768, 768, CV_8UC1);
  uint32_t image_seed = rootsift ? 0x13572468U : 0x24681357U;
  for (int row = 0; row < image.rows; ++row) {
    for (int column = 0; column < image.cols; ++column) {
      uint32_t noise = random_u32(&image_seed);
      uint32_t row_term = static_cast<uint32_t>(row) * 17U;
      uint32_t column_term = static_cast<uint32_t>(column) * 31U;
      image.at<unsigned char>(row, column) =
          static_cast<unsigned char>((noise + row_term + column_term) & 0xffU);
    }
  }
  cv::GaussianBlur(image, image, cv::Size(3, 3), 0.8);
  cv::Ptr<cv::SIFT> sift = cv::SIFT::create(1024);
  std::vector<cv::KeyPoint> keypoints;
  cv::Mat real_descriptors;
  sift->detectAndCompute(image, cv::noArray(), keypoints, real_descriptors);
  if (real_descriptors.rows < 2 || real_descriptors.cols != 128) {
    ok = false;
  } else {
    std::vector<float> real(
        real_descriptors.ptr<float>(),
        real_descriptors.ptr<float>() +
            static_cast<size_t>(real_descriptors.rows) * 128);
    if (rootsift) {
      for (int row = 0; row < real_descriptors.rows; ++row) {
        float sum = 0.0F;
        for (size_t component = 0; component < 128; ++component) {
          sum += real[static_cast<size_t>(row) * 128 + component];
        }
        for (size_t component = 0; component < 128; ++component) {
          float &value = real[static_cast<size_t>(row) * 128 + component];
          value = std::sqrt(value / sum);
        }
      }
    }
    ok = compare_case(backend, real, real, &comparison) && ok;
  }

  std::vector<Lardon3DSiftTop2> reference(near.size() / 128);
  std::vector<Lardon3DSiftTop2> repeated(reference.size());
  ok = lardon3d_sift_vulkan_top2(
           backend, near.data(), static_cast<uint32_t>(reference.size()),
           near.data(), static_cast<uint32_t>(reference.size()),
           reference.data(), reference.size()) == LARDON3D_ORB_VULKAN_OK && ok;
  for (int repetition = 0; repetition < 100 && ok; ++repetition) {
    ok = lardon3d_sift_vulkan_top2(
             backend, near.data(), static_cast<uint32_t>(repeated.size()),
             near.data(), static_cast<uint32_t>(repeated.size()),
             repeated.data(), repeated.size()) == LARDON3D_ORB_VULKAN_OK &&
         std::memcmp(reference.data(), repeated.data(),
                     reference.size() * sizeof(reference[0])) == 0;
  }

  if (!fp64_requested()) {
    ok = match_file_difference(backend) && ok;
  }

  std::printf("%s queries=%llu index_divergences=%llu distance_divergences=%llu "
              "lowe_divergences=%llu\n",
              rootsift ? "rootsift" : "sift",
              static_cast<unsigned long long>(comparison.queries),
              static_cast<unsigned long long>(comparison.index_divergences),
              static_cast<unsigned long long>(comparison.distance_bit_divergences),
              static_cast<unsigned long long>(comparison.lowe_divergences));
  lardon3d_orb_vulkan_backend_destroy(backend);
  return ok;
}

int main() {
  cv::setNumThreads(12);
  return run_distribution(false) && run_distribution(true)
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
