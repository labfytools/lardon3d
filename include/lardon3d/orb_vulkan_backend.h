#ifndef LARDON3D_ORB_VULKAN_BACKEND_H
#define LARDON3D_ORB_VULKAN_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Lardon3DOrbVulkanBackend Lardon3DOrbVulkanBackend;

enum {
  LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES = 640 * 1024,
};

typedef struct {
  uint32_t neighbor_count;
  uint32_t best_index;
  uint32_t best_distance;
  uint32_t second_index;
  uint32_t second_distance;
} Lardon3DOrbTop2;

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
typedef struct {
  uint32_t neighbor_count;
  uint32_t best_index;
  float best_squared_distance;
  uint32_t second_index;
  float second_squared_distance;
} Lardon3DSiftTop2;
#endif

typedef enum {
  LARDON3D_ORB_VULKAN_OK = 0,
  LARDON3D_ORB_VULKAN_UNAVAILABLE,
  LARDON3D_ORB_VULKAN_FAILED,
  LARDON3D_ORB_VULKAN_INVALID_ARGUMENT
} Lardon3DOrbVulkanResult;

typedef struct {
  bool available;
  bool initialized;
  bool dedicated_compute_queue;
  char device_name[256];
  uint32_t workgroup_size;
  uint64_t permanent_payload_bytes;
  uint64_t initialization_ns;
  uint64_t dispatch_ns;
  uint64_t gpu_ns;
} Lardon3DOrbVulkanInfo;

Lardon3DOrbVulkanBackend *lardon3d_orb_vulkan_backend_create(void);
void lardon3d_orb_vulkan_backend_destroy(Lardon3DOrbVulkanBackend *backend);

bool lardon3d_orb_vulkan_should_use(uint32_t feature_count_a,
                                    uint32_t feature_count_b);

Lardon3DOrbVulkanResult lardon3d_orb_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *descriptors_a,
    uint32_t feature_count_a, const unsigned char *descriptors_b,
    uint32_t feature_count_b, Lardon3DOrbTop2 *output, size_t output_capacity);

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
Lardon3DOrbVulkanResult lardon3d_sift_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const float *descriptors_a,
    uint32_t feature_count_a, const float *descriptors_b,
    uint32_t feature_count_b, Lardon3DSiftTop2 *output, size_t output_capacity);
#endif

bool lardon3d_orb_vulkan_backend_info(Lardon3DOrbVulkanBackend *backend,
                                      Lardon3DOrbVulkanInfo *info);

#ifdef __cplusplus
}
#endif

#endif
