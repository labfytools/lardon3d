#include <lardon3d/orb_vulkan_backend.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include <lardon3d/feature_extractor.h>

#include "matcher_vulkan_config.h"
#include "orb_vulkan_backend_internal.h"

#if LARDON3D_HAVE_VULKAN

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <vulkan/vulkan.h>

#include "orb_top2_spv.h"
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
#include "sift_top2_spv.h"
#include "sift_top2_fp64_spv.h"
#endif

namespace {

constexpr VkDeviceSize kDescriptorBufferBytes =
    static_cast<VkDeviceSize>(LARDON3D_FEATURE_MAX_FEATURES) * 32;
constexpr VkDeviceSize kOutputBufferBytes =
    static_cast<VkDeviceSize>(LARDON3D_FEATURE_MAX_FEATURES) * 4 * sizeof(uint32_t);
static_assert(kDescriptorBufferBytes * 2 + kOutputBufferBytes ==
              LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES);
constexpr uint64_t kDefaultVulkanWorkThreshold = 768ULL * 768ULL;
constexpr uint32_t kDefaultWorkgroupSize = 32;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
constexpr VkDeviceSize kSiftDescriptorBufferBytes =
    static_cast<VkDeviceSize>(LARDON3D_FEATURE_MAX_FEATURES) * 128 * sizeof(float);
#endif

enum class BackendState {
  kUninitialized,
  kAvailable,
  kUnavailable,
  kFailed,
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapping = nullptr;
  VkDeviceSize size = 0;
  bool coherent = false;
};

struct RawTop2 {
  uint32_t best_index;
  uint32_t best_distance;
  uint32_t second_index;
  uint32_t second_distance;
};

struct OrbRequestSlot {
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence completion_fence = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkQueryPool query_pool = VK_NULL_HANDLE;
  Buffer descriptors_a;
  Buffer descriptors_b;
  Buffer output;
  bool payload_allocated = false;
  bool completion_pending = false;
  uint32_t pending_feature_count_a = 0;
  uint32_t pending_feature_count_b = 0;
  uint64_t generation = 0;
  bool generation_retired = false;
};

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
struct RawSiftTop2 {
  uint32_t best_index;
  float best_squared_distance;
  uint32_t second_index;
  float second_squared_distance;
};
#endif

static uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
  auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

static bool validation_requested() {
  const char *value = std::getenv("LARDON3D_VULKAN_VALIDATION");
  return value && std::strcmp(value, "1") == 0;
}

static uint32_t configured_workgroup_size() {
  const char *value = std::getenv("LARDON3D_VULKAN_WORKGROUP_SIZE");
  if (!value || value[0] == '\0') {
    return kDefaultWorkgroupSize;
  }
  char *end = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if (!end || end[0] != '\0' ||
      (parsed != 32 && parsed != 64 && parsed != 128 && parsed != 256)) {
    return kDefaultWorkgroupSize;
  }
  return static_cast<uint32_t>(parsed);
}

static bool has_validation_layer() {
  uint32_t count = 0;
  if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkLayerProperties> layers(count);
  if (count > 0 &&
      vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
    return false;
  }
  return std::any_of(layers.begin(), layers.end(), [](const auto &layer) {
    return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
  });
}

}  // namespace

struct Lardon3DOrbVulkanBackend {
  /* Public top2 is one synchronous transaction even though its private begin
   * and finish deliberately release request-state ownership between calls.
   * Lock order is always synchronous_transaction_mutex -> mutex. Private
   * async begin/finish/discard and info take only mutex, so Matcher may keep a
   * request in flight across publication without holding this transaction
   * lock or deadlocking observation. */
  std::mutex synchronous_transaction_mutex;
  std::mutex mutex;
  BackendState state = BackendState::kUninitialized;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = UINT32_MAX;
  bool dedicated_compute_queue = false;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory_properties{};
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  VkPhysicalDeviceFeatures features{};
#endif
  VkCommandPool command_pool = VK_NULL_HANDLE;
  /* CONTRACT: the device, queue, command pool, pipeline and layouts are shared
   * immutable backend state. Only the bounded request slots duplicate command,
   * fence, descriptor, mapped input/readback and timestamp resources. A slot
   * generation makes private completion ownership request-bound. */
  OrbRequestSlot slots[LARDON3D_ORB_VULKAN_MAX_INFLIGHT];
  /* Mapped request payload follows the frozen sequence admission. Retained
   * count, rather than slot index, is authoritative because an exhausted
   * generation retires that slot permanently and depth one must retain the
   * other usable slot. Command/fence/descriptor/query objects remain bounded
   * session metadata and never imply retained mapped payload. */
  uint32_t configured_capacity = 1;
  uint32_t retained_capacity = 0;
  bool sequence_capacity_active = false;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  VkPipeline sift_pipeline = VK_NULL_HANDLE;
#endif
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  VkCommandBuffer sift_command_buffer = VK_NULL_HANDLE;
  VkFence sift_completion_fence = VK_NULL_HANDLE;
  VkDescriptorSet sift_descriptor_set = VK_NULL_HANDLE;
  VkQueryPool sift_query_pool = VK_NULL_HANDLE;
#endif
  bool timestamps_available = false;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  Buffer sift_descriptors_a;
  Buffer sift_descriptors_b;
  Buffer sift_output;
#endif
  uint32_t workgroup_size = kDefaultWorkgroupSize;
  uint64_t initialization_ns = 0;
  uint64_t last_dispatch_ns = 0;
  uint64_t last_gpu_ns = 0;
  /* Telemetry is monotonic, bounded, and request-state owned. Saturation is
   * explicit: measurement can stop gaining precision after UINT64_MAX, but
   * must never wrap into a false low-utilization control signal. */
  Lardon3DOrbVulkanTelemetry telemetry{};
  bool completion_observed = false;
  std::chrono::steady_clock::time_point completion_observed_at{};
};

namespace {

static void saturating_add(uint64_t *value, uint64_t increment) {
  *value = *value > UINT64_MAX - increment ? UINT64_MAX : *value + increment;
}

static void telemetry_event(Lardon3DOrbVulkanBackend *backend) {
  saturating_add(&backend->telemetry.serial, 1);
}

static void destroy_buffer(Lardon3DOrbVulkanBackend *backend, Buffer *buffer) {
  if (!backend || !buffer || backend->device == VK_NULL_HANDLE) {
    return;
  }
  if (buffer->mapping) {
    vkUnmapMemory(backend->device, buffer->memory);
  }
  if (buffer->buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(backend->device, buffer->buffer, nullptr);
  }
  if (buffer->memory != VK_NULL_HANDLE) {
    vkFreeMemory(backend->device, buffer->memory, nullptr);
  }
  *buffer = Buffer{};
}

static void destroy_vulkan(Lardon3DOrbVulkanBackend *backend) {
  if (!backend) {
    return;
  }
  if (backend->device != VK_NULL_HANDLE) {
    (void)vkDeviceWaitIdle(backend->device);
  }
  for (OrbRequestSlot &slot : backend->slots) {
    destroy_buffer(backend, &slot.descriptors_a);
    destroy_buffer(backend, &slot.descriptors_b);
    destroy_buffer(backend, &slot.output);
    if (slot.query_pool != VK_NULL_HANDLE) {
      vkDestroyQueryPool(backend->device, slot.query_pool, nullptr);
    }
  }
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  destroy_buffer(backend, &backend->sift_descriptors_a);
  destroy_buffer(backend, &backend->sift_descriptors_b);
  destroy_buffer(backend, &backend->sift_output);
#endif
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  if (backend->sift_query_pool != VK_NULL_HANDLE) {
    vkDestroyQueryPool(backend->device, backend->sift_query_pool, nullptr);
  }
#endif
  if (backend->pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(backend->device, backend->pipeline, nullptr);
  }
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  if (backend->sift_pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(backend->device, backend->sift_pipeline, nullptr);
  }
#endif
  if (backend->pipeline_layout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(backend->device, backend->pipeline_layout, nullptr);
  }
  if (backend->descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(backend->device, backend->descriptor_pool, nullptr);
  }
  if (backend->descriptor_set_layout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(backend->device, backend->descriptor_set_layout, nullptr);
  }
  if (backend->command_pool != VK_NULL_HANDLE) {
    vkDestroyCommandPool(backend->device, backend->command_pool, nullptr);
  }
  for (OrbRequestSlot &slot : backend->slots) {
    if (slot.completion_fence != VK_NULL_HANDLE) {
      vkDestroyFence(backend->device, slot.completion_fence, nullptr);
    }
  }
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  if (backend->sift_completion_fence != VK_NULL_HANDLE) {
    vkDestroyFence(backend->device, backend->sift_completion_fence, nullptr);
  }
#endif
  if (backend->device != VK_NULL_HANDLE) {
    vkDestroyDevice(backend->device, nullptr);
  }
  if (backend->instance != VK_NULL_HANDLE) {
    vkDestroyInstance(backend->instance, nullptr);
  }
  backend->instance = VK_NULL_HANDLE;
  backend->physical_device = VK_NULL_HANDLE;
  backend->device = VK_NULL_HANDLE;
  backend->queue = VK_NULL_HANDLE;
  backend->command_pool = VK_NULL_HANDLE;
  for (OrbRequestSlot &slot : backend->slots) {
    slot = OrbRequestSlot{};
  }
  backend->configured_capacity = 1;
  backend->retained_capacity = 0;
  backend->sequence_capacity_active = false;
  backend->descriptor_set_layout = VK_NULL_HANDLE;
  backend->pipeline_layout = VK_NULL_HANDLE;
  backend->pipeline = VK_NULL_HANDLE;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  backend->sift_pipeline = VK_NULL_HANDLE;
#endif
  backend->descriptor_pool = VK_NULL_HANDLE;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  backend->sift_command_buffer = VK_NULL_HANDLE;
  backend->sift_completion_fence = VK_NULL_HANDLE;
  backend->sift_descriptor_set = VK_NULL_HANDLE;
  backend->sift_query_pool = VK_NULL_HANDLE;
#endif
  backend->timestamps_available = false;
}

static bool create_instance(Lardon3DOrbVulkanBackend *backend) {
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "Lardon3D ORB Matcher";
  application.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  application.pEngineName = "Lardon3D";
  application.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  application.apiVersion = VK_API_VERSION_1_1;

  const char *validation_layer = "VK_LAYER_KHRONOS_validation";
  bool enable_validation = validation_requested() && has_validation_layer();
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &application;
  create_info.enabledLayerCount = enable_validation ? 1U : 0U;
  create_info.ppEnabledLayerNames = enable_validation ? &validation_layer : nullptr;
  return vkCreateInstance(&create_info, nullptr, &backend->instance) == VK_SUCCESS;
}

static bool find_compute_queue(VkPhysicalDevice device, uint32_t *family,
                               bool *dedicated) {
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  if (count == 0) {
    return false;
  }
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
  uint32_t fallback = UINT32_MAX;
  for (uint32_t index = 0; index < count; ++index) {
    VkQueueFlags flags = families[index].queueFlags;
    if (families[index].queueCount == 0 || (flags & VK_QUEUE_COMPUTE_BIT) == 0) {
      continue;
    }
    if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
      *family = index;
      *dedicated = true;
      return true;
    }
    if (fallback == UINT32_MAX) {
      fallback = index;
    }
  }
  if (fallback == UINT32_MAX) {
    return false;
  }
  *family = fallback;
  *dedicated = false;
  return true;
}

static int device_score(VkPhysicalDevice device, uint32_t *family,
                        bool *dedicated) {
  if (!find_compute_queue(device, family, dedicated)) {
    return -1;
  }
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(device, &properties);
  if (properties.limits.maxComputeWorkGroupInvocations < 32 ||
      properties.limits.maxComputeWorkGroupSize[0] < 32 ||
      properties.limits.maxStorageBufferRange < kDescriptorBufferBytes) {
    return -1;
  }
  int score = *dedicated ? 100 : 0;
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
    score += 30;
  } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
    score += 20;
  }
  const char *requested = std::getenv("LARDON3D_VULKAN_DEVICE");
  if (requested && requested[0] != '\0' &&
      std::strstr(properties.deviceName, requested)) {
    score += 1000;
  }
  return score;
}

static bool select_device(Lardon3DOrbVulkanBackend *backend) {
  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(backend->instance, &count, nullptr) != VK_SUCCESS ||
      count == 0) {
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  if (vkEnumeratePhysicalDevices(backend->instance, &count, devices.data()) != VK_SUCCESS) {
    return false;
  }
  int best_score = -1;
  for (VkPhysicalDevice device : devices) {
    uint32_t family = UINT32_MAX;
    bool dedicated = false;
    int score = device_score(device, &family, &dedicated);
    if (score > best_score) {
      best_score = score;
      backend->physical_device = device;
      backend->queue_family = family;
      backend->dedicated_compute_queue = dedicated;
    }
  }
  if (backend->physical_device == VK_NULL_HANDLE) {
    return false;
  }
  vkGetPhysicalDeviceProperties(backend->physical_device, &backend->properties);
  vkGetPhysicalDeviceMemoryProperties(backend->physical_device,
                                      &backend->memory_properties);
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  vkGetPhysicalDeviceFeatures(backend->physical_device, &backend->features);
#endif
  backend->workgroup_size = configured_workgroup_size();
  return backend->workgroup_size <=
             backend->properties.limits.maxComputeWorkGroupInvocations &&
         backend->workgroup_size <=
             backend->properties.limits.maxComputeWorkGroupSize[0];
}

static bool create_device_and_commands(Lardon3DOrbVulkanBackend *backend) {
  float priority = 0.5F;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = backend->queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  VkPhysicalDeviceFeatures enabled_features{};
  enabled_features.shaderFloat64 = backend->features.shaderFloat64;
  device_info.pEnabledFeatures = &enabled_features;
#endif
  if (vkCreateDevice(backend->physical_device, &device_info, nullptr,
                     &backend->device) != VK_SUCCESS) {
    return false;
  }
  vkGetDeviceQueue(backend->device, backend->queue_family, 0, &backend->queue);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = backend->queue_family;
  if (vkCreateCommandPool(backend->device, &pool_info, nullptr,
                          &backend->command_pool) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo command_info{};
  command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_info.commandPool = backend->command_pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  VkCommandBuffer command_buffers[LARDON3D_ORB_VULKAN_MAX_INFLIGHT]{};
  command_info.commandBufferCount = LARDON3D_ORB_VULKAN_MAX_INFLIGHT;
  if (vkAllocateCommandBuffers(backend->device, &command_info,
                               command_buffers) != VK_SUCCESS) {
    return false;
  }
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (uint32_t index = 0; index < LARDON3D_ORB_VULKAN_MAX_INFLIGHT; ++index) {
    backend->slots[index].command_buffer = command_buffers[index];
    if (vkCreateFence(backend->device, &fence_info, nullptr,
                      &backend->slots[index].completion_fence) != VK_SUCCESS) {
      return false;
    }
  }
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(backend->device, &command_info,
                               &backend->sift_command_buffer) != VK_SUCCESS ||
      vkCreateFence(backend->device, &fence_info, nullptr,
                    &backend->sift_completion_fence) != VK_SUCCESS) {
    return false;
  }
#endif
  return true;
}

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
static bool create_shader_pipeline(Lardon3DOrbVulkanBackend *backend,
                                   const uint32_t *code, size_t code_size,
                                   VkPipeline *pipeline) {
  VkShaderModuleCreateInfo shader_info{};
  shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_info.codeSize = code_size;
  shader_info.pCode = code;
  VkShaderModule shader = VK_NULL_HANDLE;
  if (vkCreateShaderModule(backend->device, &shader_info, nullptr, &shader) != VK_SUCCESS) {
    return false;
  }
  VkSpecializationMapEntry workgroup_entry{0, 0, sizeof(uint32_t)};
  VkSpecializationInfo specialization{};
  specialization.mapEntryCount = 1;
  specialization.pMapEntries = &workgroup_entry;
  specialization.dataSize = sizeof(backend->workgroup_size);
  specialization.pData = &backend->workgroup_size;
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = shader;
  stage.pName = "main";
  stage.pSpecializationInfo = &specialization;
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage = stage;
  pipeline_info.layout = backend->pipeline_layout;
  VkResult result = vkCreateComputePipelines(backend->device, VK_NULL_HANDLE, 1,
                                             &pipeline_info, nullptr, pipeline);
  vkDestroyShaderModule(backend->device, shader, nullptr);
  return result == VK_SUCCESS;
}
#endif

static bool create_pipeline(Lardon3DOrbVulkanBackend *backend) {
  VkDescriptorSetLayoutBinding bindings[3]{};
  for (uint32_t index = 0; index < 3; ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_info{};
  descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_info.bindingCount = 3;
  descriptor_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(backend->device, &descriptor_info, nullptr,
                                  &backend->descriptor_set_layout) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  push_range.size = 2 * sizeof(uint32_t);
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &backend->descriptor_set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (vkCreatePipelineLayout(backend->device, &layout_info, nullptr,
                             &backend->pipeline_layout) != VK_SUCCESS) {
    return false;
  }

  VkShaderModuleCreateInfo shader_info{};
  shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_info.codeSize = lardon3d_orb_top2_spv_size;
  shader_info.pCode = lardon3d_orb_top2_spv;
  VkShaderModule shader = VK_NULL_HANDLE;
  if (vkCreateShaderModule(backend->device, &shader_info, nullptr, &shader) != VK_SUCCESS) {
    return false;
  }
  VkSpecializationMapEntry workgroup_entry{0, 0, sizeof(uint32_t)};
  VkSpecializationInfo specialization{};
  specialization.mapEntryCount = 1;
  specialization.pMapEntries = &workgroup_entry;
  specialization.dataSize = sizeof(backend->workgroup_size);
  specialization.pData = &backend->workgroup_size;
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = shader;
  stage.pName = "main";
  stage.pSpecializationInfo = &specialization;
  VkComputePipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage = stage;
  pipeline_info.layout = backend->pipeline_layout;
  VkResult result = vkCreateComputePipelines(backend->device, VK_NULL_HANDLE, 1,
                                             &pipeline_info, nullptr,
                                             &backend->pipeline);
  vkDestroyShaderModule(backend->device, shader, nullptr);
  return result == VK_SUCCESS;
}

static bool select_memory_type(Lardon3DOrbVulkanBackend *backend,
                               uint32_t memory_type_bits, uint32_t *type_index,
                               bool *coherent) {
  int best_score = -1;
  for (uint32_t index = 0; index < backend->memory_properties.memoryTypeCount; ++index) {
    if ((memory_type_bits & (1U << index)) == 0) {
      continue;
    }
    VkMemoryPropertyFlags flags =
        backend->memory_properties.memoryTypes[index].propertyFlags;
    if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
      continue;
    }
    int score = 0;
    if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
      score += 2;
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
      score += 4;
    }
    if (score > best_score) {
      best_score = score;
      *type_index = index;
      *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }
  }
  return best_score >= 0;
}

static bool create_buffer(Lardon3DOrbVulkanBackend *backend, VkDeviceSize size,
                          Buffer *buffer) {
  buffer->size = size;
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(backend->device, &buffer_info, nullptr, &buffer->buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(backend->device, buffer->buffer, &requirements);
  uint32_t type_index = 0;
  if (!select_memory_type(backend, requirements.memoryTypeBits, &type_index,
                          &buffer->coherent)) {
    return false;
  }
  VkMemoryAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = type_index;
  if (vkAllocateMemory(backend->device, &allocate_info, nullptr, &buffer->memory) !=
          VK_SUCCESS ||
      vkBindBufferMemory(backend->device, buffer->buffer, buffer->memory, 0) !=
          VK_SUCCESS ||
      vkMapMemory(backend->device, buffer->memory, 0, size, 0,
                  &buffer->mapping) != VK_SUCCESS) {
    return false;
  }
  return true;
}

static bool create_descriptors_and_queries(Lardon3DOrbVulkanBackend *backend) {
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  pool_size.descriptorCount =
      3 * (LARDON3D_ORB_VULKAN_MAX_INFLIGHT + 1);
#else
  pool_size.descriptorCount = 3 * LARDON3D_ORB_VULKAN_MAX_INFLIGHT;
#endif
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
  pool_info.maxSets = LARDON3D_ORB_VULKAN_MAX_INFLIGHT + 1;
#else
  pool_info.maxSets = LARDON3D_ORB_VULKAN_MAX_INFLIGHT;
#endif
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(backend->device, &pool_info, nullptr,
                             &backend->descriptor_pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = backend->descriptor_pool;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &backend->descriptor_set_layout;
  for (OrbRequestSlot &slot : backend->slots) {
    if (vkAllocateDescriptorSets(backend->device, &set_info,
                                 &slot.descriptor_set) != VK_SUCCESS) {
      return false;
    }
  }

  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(backend->physical_device, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(backend->physical_device, &family_count,
                                           families.data());
  backend->timestamps_available =
      backend->queue_family < family_count &&
      families[backend->queue_family].timestampValidBits > 0;
  if (backend->timestamps_available) {
    VkQueryPoolCreateInfo query_info{};
    query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = 2;
    for (OrbRequestSlot &slot : backend->slots) {
      if (vkCreateQueryPool(backend->device, &query_info, nullptr,
                            &slot.query_pool) != VK_SUCCESS) {
        backend->timestamps_available = false;
        break;
      }
    }
#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
    if (backend->timestamps_available &&
        vkCreateQueryPool(backend->device, &query_info, nullptr,
                          &backend->sift_query_pool) != VK_SUCCESS) {
      backend->timestamps_available = false;
    }
#endif
  }
  return true;
}

static void release_slot_payload(Lardon3DOrbVulkanBackend *backend,
                                 OrbRequestSlot *slot) {
  if (!backend || !slot) return;
  destroy_buffer(backend, &slot->descriptors_a);
  destroy_buffer(backend, &slot->descriptors_b);
  destroy_buffer(backend, &slot->output);
  slot->payload_allocated = false;
}

static bool allocate_slot_payload(Lardon3DOrbVulkanBackend *backend,
                                  uint32_t slot_index) {
  if (!backend || slot_index >= LARDON3D_ORB_VULKAN_MAX_INFLIGHT) return false;
  OrbRequestSlot *slot = &backend->slots[slot_index];
  if (slot->payload_allocated) return true;
  if (slot->generation_retired) return false;
#ifdef LARDON3D_ORB_VULKAN_TESTING
  const char *forced_slot = std::getenv(
      "LARDON3D_TEST_VULKAN_SLOT_ALLOCATION_FAILURE");
  if (forced_slot && forced_slot[0] == static_cast<char>('0' + slot_index)
      && forced_slot[1] == '\0') {
    return false;
  }
#endif
  if (!create_buffer(backend, kDescriptorBufferBytes, &slot->descriptors_a)
      || !create_buffer(backend, kDescriptorBufferBytes, &slot->descriptors_b)
      || !create_buffer(backend, kOutputBufferBytes, &slot->output)) {
    release_slot_payload(backend, slot);
    return false;
  }
  VkDescriptorBufferInfo buffer_info[3] = {
      {slot->descriptors_a.buffer, 0, slot->descriptors_a.size},
      {slot->descriptors_b.buffer, 0, slot->descriptors_b.size},
      {slot->output.buffer, 0, slot->output.size},
  };
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t index = 0; index < 3; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = slot->descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(backend->device, 3, writes, 0, nullptr);
  slot->payload_allocated = true;
  return true;
}

static bool request_pending_locked(const Lardon3DOrbVulkanBackend *backend) {
  for (const OrbRequestSlot &slot : backend->slots) {
    if (slot.completion_pending) return true;
  }
  return false;
}

static bool resize_payload_locked(Lardon3DOrbVulkanBackend *backend,
                                  uint32_t capacity) {
  if (!backend || capacity == 0
      || capacity > LARDON3D_ORB_VULKAN_MAX_INFLIGHT
      || request_pending_locked(backend)) {
    return false;
  }
  /* An exhausted generation can never be reset by payload recreation. Retire
   * its allocation first; a smaller/default capacity may then retain another
   * usable slot without ever reviving an ancient handle. */
  for (OrbRequestSlot &slot : backend->slots) {
    if (slot.payload_allocated && slot.generation_retired) {
      if (backend->retained_capacity == 0) return false;
      release_slot_payload(backend, &slot);
      --backend->retained_capacity;
    }
  }
  while (backend->retained_capacity < capacity) {
    bool allocated = false;
    for (uint32_t index = 0; index < LARDON3D_ORB_VULKAN_MAX_INFLIGHT;
         ++index) {
      OrbRequestSlot *slot = &backend->slots[index];
      if (!slot->payload_allocated && !slot->generation_retired) {
        if (!allocate_slot_payload(backend, index)) return false;
        ++backend->retained_capacity;
        allocated = true;
        break;
      }
    }
    if (!allocated) return false;
  }
  while (backend->retained_capacity > capacity) {
    OrbRequestSlot *release = nullptr;
    for (OrbRequestSlot &slot : backend->slots) {
      if (slot.payload_allocated && slot.generation_retired) {
        release = &slot;
        break;
      }
    }
    if (!release) {
      for (uint32_t index = LARDON3D_ORB_VULKAN_MAX_INFLIGHT; index > 0;
           --index) {
        if (backend->slots[index - 1].payload_allocated) {
          release = &backend->slots[index - 1];
          break;
        }
      }
    }
    if (!release) return false;
    release_slot_payload(backend, release);
    --backend->retained_capacity;
  }
  backend->configured_capacity = capacity;
  return true;
}

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
static bool create_sift_resources_locked(Lardon3DOrbVulkanBackend *backend) {
  if (backend->sift_pipeline != VK_NULL_HANDLE) {
    return true;
  }
  const char *fp64 = std::getenv("LARDON3D_VULKAN_SIFT_FP64");
  bool use_fp64 = fp64 && std::strcmp(fp64, "1") == 0;
  const uint32_t *code = use_fp64 ? lardon3d_sift_top2_fp64_spv
                                  : lardon3d_sift_top2_spv;
  size_t code_size = use_fp64 ? lardon3d_sift_top2_fp64_spv_size
                              : lardon3d_sift_top2_spv_size;
  if (!create_shader_pipeline(backend, code, code_size,
                              &backend->sift_pipeline)) {
    return false;
  }
  VkDescriptorSetAllocateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  set_info.descriptorPool = backend->descriptor_pool;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &backend->descriptor_set_layout;
  if (vkAllocateDescriptorSets(backend->device, &set_info,
                               &backend->sift_descriptor_set) != VK_SUCCESS ||
      !create_buffer(backend, kSiftDescriptorBufferBytes,
                     &backend->sift_descriptors_a) ||
      !create_buffer(backend, kSiftDescriptorBufferBytes,
                     &backend->sift_descriptors_b) ||
      !create_buffer(backend, kOutputBufferBytes, &backend->sift_output)) {
    return false;
  }
  VkDescriptorBufferInfo buffer_info[3] = {
      {backend->sift_descriptors_a.buffer, 0, backend->sift_descriptors_a.size},
      {backend->sift_descriptors_b.buffer, 0, backend->sift_descriptors_b.size},
      {backend->sift_output.buffer, 0, backend->sift_output.size},
  };
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t index = 0; index < 3; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = backend->sift_descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(backend->device, 3, writes, 0, nullptr);
  return true;
}
#endif

static bool initialize_locked(Lardon3DOrbVulkanBackend *backend) {
  if (backend->state == BackendState::kAvailable) {
    return true;
  }
  if (backend->state != BackendState::kUninitialized) {
    return false;
  }
  const char *mesa_cache_disabled = std::getenv("MESA_SHADER_CACHE_DISABLE");
  /* CONTRACT: only a process-start boundary may establish this environment.
   * A backend call can occur after arbitrary library threads exist, so it must
   * never call setenv here. Reject and cache UNAVAILABLE before the first Mesa
   * or Vulkan symbol can create affinity-widening disk-cache helpers. Exact
   * true/1 are the only process policy values validated on the target host. */
  if (!mesa_cache_disabled
      || (std::strcmp(mesa_cache_disabled, "true") != 0
          && std::strcmp(mesa_cache_disabled, "1") != 0)) {
    backend->state = BackendState::kUnavailable;
    return false;
  }
  const char *disabled = std::getenv("LARDON3D_VULKAN_DISABLE");
  if (disabled && std::strcmp(disabled, "1") == 0) {
    backend->state = BackendState::kUnavailable;
    return false;
  }
  auto start = std::chrono::steady_clock::now();
  bool success = create_instance(backend) && select_device(backend) &&
                 create_device_and_commands(backend) && create_pipeline(backend) &&
                 create_descriptors_and_queries(backend) &&
                 resize_payload_locked(backend, backend->configured_capacity);
  backend->initialization_ns = elapsed_ns(start);
  if (!success) {
    destroy_vulkan(backend);
    backend->state = BackendState::kUnavailable;
    return false;
  }
  backend->state = BackendState::kAvailable;
  return true;
}

static Lardon3DOrbVulkanResult fail_session_locked(
    Lardon3DOrbVulkanBackend *backend) {
  saturating_add(&backend->telemetry.failures, 1);
  telemetry_event(backend);
  destroy_vulkan(backend);
  backend->state = BackendState::kFailed;
  return LARDON3D_ORB_VULKAN_FAILED;
}

static bool synchronize_host_write(Lardon3DOrbVulkanBackend *backend,
                                   const Buffer &buffer, VkDeviceSize size) {
  if (buffer.coherent || size == 0) {
    return true;
  }
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = buffer.memory;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  return vkFlushMappedMemoryRanges(backend->device, 1, &range) == VK_SUCCESS;
}

static bool synchronize_host_read(Lardon3DOrbVulkanBackend *backend,
                                  const Buffer &buffer) {
  if (buffer.coherent) {
    return true;
  }
  VkMappedMemoryRange range{};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = buffer.memory;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  return vkInvalidateMappedMemoryRanges(backend->device, 1, &range) == VK_SUCCESS;
}

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
static VkResult record_and_submit_sift(Lardon3DOrbVulkanBackend *backend,
                                  VkPipeline pipeline,
                                  VkDescriptorSet descriptor_set,
                                  uint32_t count_a, uint32_t count_b) {
  VkCommandBuffer command_buffer = backend->sift_command_buffer;
  VkResult result = vkResetCommandBuffer(command_buffer, 0);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(command_buffer, &begin_info);
  if (result != VK_SUCCESS) {
    return result;
  }
  if (backend->timestamps_available) {
    vkCmdResetQueryPool(command_buffer, backend->sift_query_pool, 0, 2);
    vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        backend->sift_query_pool, 0);
  }
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline);
  vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          backend->pipeline_layout, 0, 1,
                          &descriptor_set, 0, nullptr);
  uint32_t counts[2] = {count_a, count_b};
  vkCmdPushConstants(command_buffer, backend->pipeline_layout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(counts), counts);
  vkCmdDispatch(command_buffer, count_a, 1, 1);
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
                       nullptr);
  if (backend->timestamps_available) {
    vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        backend->sift_query_pool, 1);
  }
  result = vkEndCommandBuffer(command_buffer);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer;
  result = vkResetFences(backend->device, 1, &backend->sift_completion_fence);
  if (result != VK_SUCCESS) return result;
  result = vkQueueSubmit(backend->queue, 1, &submit_info,
                         backend->sift_completion_fence);
  if (result != VK_SUCCESS) {
    return result;
  }
  return VK_SUCCESS;
}
#endif

static VkResult record_and_submit(Lardon3DOrbVulkanBackend *backend,
                                  OrbRequestSlot *slot,
                                  uint32_t count_a, uint32_t count_b) {
  VkResult result = vkResetCommandBuffer(slot->command_buffer, 0);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(slot->command_buffer, &begin_info);
  if (result != VK_SUCCESS) {
    return result;
  }
  if (backend->timestamps_available) {
    vkCmdResetQueryPool(slot->command_buffer, slot->query_pool, 0, 2);
    vkCmdWriteTimestamp(slot->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        slot->query_pool, 0);
  }
  vkCmdBindPipeline(slot->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    backend->pipeline);
  vkCmdBindDescriptorSets(slot->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          backend->pipeline_layout, 0, 1,
                          &slot->descriptor_set, 0, nullptr);
  uint32_t counts[2] = {count_a, count_b};
  vkCmdPushConstants(slot->command_buffer, backend->pipeline_layout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(counts), counts);
  uint32_t groups = (count_a + backend->workgroup_size - 1) /
                    backend->workgroup_size;
  vkCmdDispatch(slot->command_buffer, groups, 1, 1);
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(slot->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
                       nullptr);
  if (backend->timestamps_available) {
    vkCmdWriteTimestamp(slot->command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        slot->query_pool, 1);
  }
  result = vkEndCommandBuffer(slot->command_buffer);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot->command_buffer;
  /* A slot fence establishes completion ownership without draining unrelated
   * queue work. The synchronous wrapper still waits before exposing output. */
  result = vkResetFences(backend->device, 1, &slot->completion_fence);
  if (result != VK_SUCCESS) return result;
  result = vkQueueSubmit(backend->queue, 1, &submit_info,
                         slot->completion_fence);
  if (result != VK_SUCCESS) {
    return result;
  }
  return VK_SUCCESS;
}

static void read_gpu_time(Lardon3DOrbVulkanBackend *backend,
                          VkQueryPool query_pool) {
  backend->last_gpu_ns = 0;
  if (!backend->timestamps_available) {
    return;
  }
  uint64_t timestamps[2]{};
  VkResult result = vkGetQueryPoolResults(
      backend->device, query_pool, 0, 2, sizeof(timestamps), timestamps,
      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  if (result == VK_SUCCESS && timestamps[1] >= timestamps[0]) {
    double nanoseconds = static_cast<double>(timestamps[1] - timestamps[0]) *
                         backend->properties.limits.timestampPeriod;
    backend->last_gpu_ns = static_cast<uint64_t>(nanoseconds);
  }
}

}  // namespace

extern "C" Lardon3DOrbVulkanBackend *lardon3d_orb_vulkan_backend_create(void) {
  try {
    return new (std::nothrow) Lardon3DOrbVulkanBackend();
  } catch (...) {
    return nullptr;
  }
}

extern "C" void lardon3d_orb_vulkan_backend_destroy(
    Lardon3DOrbVulkanBackend *backend) {
  if (!backend) {
    return;
  }
  try {
    {
      /* Destruction follows the public wrapper's lock order and cannot tear
       * down a session in the middle of one synchronous begin->finish call.
       * As for every destroy API, callers still own exclusion from future use. */
      std::lock_guard<std::mutex> transaction_lock(
          backend->synchronous_transaction_mutex);
      std::lock_guard<std::mutex> state_lock(backend->mutex);
      destroy_vulkan(backend);
    }
    delete backend;
  } catch (...) {
    /* Destruction is a C ABI boundary. A synchronization exception must not
     * escape; retaining an unusable backend is safer than an unlocked delete. */
  }
}

extern "C" bool lardon3d_orb_vulkan_should_use(uint32_t feature_count_a,
                                                uint32_t feature_count_b) {
  if (feature_count_a == 0 || feature_count_b == 0 ||
      feature_count_a > LARDON3D_FEATURE_MAX_FEATURES ||
      feature_count_b > LARDON3D_FEATURE_MAX_FEATURES) {
    return false;
  }
  return static_cast<uint64_t>(feature_count_a) * feature_count_b >=
         kDefaultVulkanWorkThreshold;
}

static Lardon3DOrbVulkanResult orb_vulkan_top2_begin_impl(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *descriptors_a,
    uint32_t feature_count_a, const unsigned char *descriptors_b,
    uint32_t feature_count_b, Lardon3DOrbVulkanRequest *request,
    bool private_sequence_request);

extern "C" bool lardon3d_orb_vulkan_internal_begin_sequence(
    Lardon3DOrbVulkanBackend *backend, uint32_t inflight_capacity) {
  if (!backend || inflight_capacity == 0
      || inflight_capacity > LARDON3D_ORB_VULKAN_MAX_INFLIGHT) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (backend->sequence_capacity_active || request_pending_locked(backend)) {
      return false;
    }
    if (backend->state == BackendState::kUninitialized) {
      backend->configured_capacity = inflight_capacity;
    } else if (backend->state == BackendState::kAvailable) {
      if (!resize_payload_locked(backend, inflight_capacity)) return false;
    } else {
      return false;
    }
    backend->sequence_capacity_active = true;
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" bool lardon3d_orb_vulkan_internal_end_sequence(
    Lardon3DOrbVulkanBackend *backend) {
  if (!backend) return false;
  try {
    std::lock_guard<std::mutex> lock(backend->mutex);
    if (request_pending_locked(backend)) {
      return false;
    }
    if (!backend->sequence_capacity_active) {
      /* Session failure destroys payload and clears the lease before Matcher
       * reaches its cleanup boundary. Treat that already-complete cleanup as
       * success, while rejecting a duplicate end on a healthy backend. */
      return backend->state != BackendState::kAvailable;
    }
    if (backend->state == BackendState::kAvailable) {
      if (!resize_payload_locked(backend, 1)) {
        /* Failure to restore the depth-one allocation cannot leave a stale
         * sequence lease or ambiguous retained payload. No work is pending at
         * this boundary, so failing the session is deterministic cleanup. */
        (void)fail_session_locked(backend);
        return false;
      }
    } else {
      backend->configured_capacity = 1;
    }
    backend->sequence_capacity_active = false;
    return true;
  } catch (...) {
    return false;
  }
}

#ifdef LARDON3D_ORB_VULKAN_TESTING
extern "C" bool lardon3d_orb_vulkan_internal_test_set_slot_generation(
    Lardon3DOrbVulkanBackend *backend, uint32_t slot, uint64_t generation) {
  if (!backend || slot >= LARDON3D_ORB_VULKAN_MAX_INFLIGHT
      || generation == 0) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(backend->mutex);
    OrbRequestSlot *request_slot = &backend->slots[slot];
    if (backend->state != BackendState::kAvailable
        || request_slot->completion_pending
        || !request_slot->payload_allocated
        || request_slot->generation_retired) {
      return false;
    }
    request_slot->generation = generation;
    return true;
  } catch (...) {
    return false;
  }
}
#endif

extern "C" Lardon3DOrbVulkanResult lardon3d_orb_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *descriptors_a,
    uint32_t feature_count_a, const unsigned char *descriptors_b,
    uint32_t feature_count_b, Lardon3DOrbTop2 *output, size_t output_capacity) {
  try {
    if (!backend || feature_count_a > LARDON3D_FEATURE_MAX_FEATURES ||
        feature_count_b > LARDON3D_FEATURE_MAX_FEATURES ||
        (feature_count_a > 0 && (!descriptors_a || !output ||
                                output_capacity < feature_count_a)) ||
        (feature_count_b > 0 && !descriptors_b)) {
      return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
    }
    /* Two public callers must each own an indivisible synchronous request.
     * The dedicated transaction lock spans private begin->finish; the private
     * request-state mutex remains short-lived so async Matcher overlap is not
     * serialized across its publication boundary. */
    std::lock_guard<std::mutex> transaction_lock(
        backend->synchronous_transaction_mutex);
    if (feature_count_a == 0) {
      return LARDON3D_ORB_VULKAN_OK;
    }
    if (feature_count_b == 0) {
      for (uint32_t index = 0; index < feature_count_a; ++index) {
        output[index] = Lardon3DOrbTop2{};
      }
      return LARDON3D_ORB_VULKAN_OK;
    }

    Lardon3DOrbVulkanRequest request{};
    Lardon3DOrbVulkanResult started = orb_vulkan_top2_begin_impl(
        backend, descriptors_a, feature_count_a, descriptors_b,
        feature_count_b, &request, false);
    if (started != LARDON3D_ORB_VULKAN_OK) return started;
    return lardon3d_orb_vulkan_internal_top2_finish(
        backend, &request, output, output_capacity);
  } catch (...) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
}

static Lardon3DOrbVulkanResult orb_vulkan_top2_begin_impl(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *descriptors_a,
    uint32_t feature_count_a, const unsigned char *descriptors_b,
    uint32_t feature_count_b, Lardon3DOrbVulkanRequest *request,
    bool private_sequence_request) {
  if (!backend || feature_count_a == 0 || feature_count_b == 0 ||
      feature_count_a > LARDON3D_FEATURE_MAX_FEATURES ||
      feature_count_b > LARDON3D_FEATURE_MAX_FEATURES || !descriptors_a ||
      !descriptors_b || !request) return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  *request = Lardon3DOrbVulkanRequest{};
  std::lock_guard<std::mutex> lock(backend->mutex);
  if (!private_sequence_request) {
    if (backend->sequence_capacity_active) {
      return LARDON3D_ORB_VULKAN_FAILED;
    }
    backend->configured_capacity = 1;
    if (backend->state == BackendState::kAvailable
        && !resize_payload_locked(backend, 1)) {
      return LARDON3D_ORB_VULKAN_FAILED;
    }
  }
  if (!initialize_locked(backend)) {
    return LARDON3D_ORB_VULKAN_UNAVAILABLE;
  }
  uint32_t slot_index = LARDON3D_ORB_VULKAN_MAX_INFLIGHT;
  for (uint32_t index = 0; index < LARDON3D_ORB_VULKAN_MAX_INFLIGHT; ++index) {
    OrbRequestSlot *candidate = &backend->slots[index];
    if (!candidate->payload_allocated || candidate->completion_pending
        || candidate->generation_retired) {
      continue;
    }
    if (candidate->generation == UINT64_MAX) {
      /* Generation is request identity, not a wrapping counter. Once the last
       * value has been issued this slot is retired before any new submission;
       * an ancient generation-one handle can therefore never become current. */
      candidate->generation_retired = true;
      continue;
    }
    if (!candidate->completion_pending) {
      slot_index = index;
      break;
    }
  }
  if (slot_index == LARDON3D_ORB_VULKAN_MAX_INFLIGHT) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
  OrbRequestSlot *slot = &backend->slots[slot_index];
  auto submit_cpu_start = std::chrono::steady_clock::now();
  VkDeviceSize bytes_a = static_cast<VkDeviceSize>(feature_count_a) * 32;
  VkDeviceSize bytes_b = static_cast<VkDeviceSize>(feature_count_b) * 32;
  std::memcpy(slot->descriptors_a.mapping, descriptors_a,
              static_cast<size_t>(bytes_a));
  std::memcpy(slot->descriptors_b.mapping, descriptors_b,
              static_cast<size_t>(bytes_b));
  if (!synchronize_host_write(backend, slot->descriptors_a, bytes_a) ||
      !synchronize_host_write(backend, slot->descriptors_b, bytes_b)) {
    return fail_session_locked(backend);
  }

  auto start = std::chrono::steady_clock::now();
#ifdef LARDON3D_ORB_VULKAN_TESTING
  const char *force_failure = std::getenv("LARDON3D_TEST_VULKAN_DEVICE_LOST");
  if (force_failure && std::strcmp(force_failure, "1") == 0) {
    return fail_session_locked(backend);
  }
#endif
  VkResult dispatch_result = record_and_submit(backend, slot, feature_count_a,
                                               feature_count_b);
  backend->last_dispatch_ns = elapsed_ns(start);
  if (dispatch_result != VK_SUCCESS) {
    return fail_session_locked(backend);
  }
  const uint64_t next_generation = slot->generation + 1;
  slot->completion_pending = true;
  slot->pending_feature_count_a = feature_count_a;
  slot->pending_feature_count_b = feature_count_b;
  slot->generation = next_generation;
  request->slot = slot_index;
  request->generation = slot->generation;
  uint64_t submit_cpu_ns = elapsed_ns(submit_cpu_start);
  saturating_add(&backend->telemetry.submits, 1);
  saturating_add(&backend->telemetry.submit_cpu_ns, submit_cpu_ns);
  if (backend->completion_observed) {
    saturating_add(&backend->telemetry.starvation_ns,
                   elapsed_ns(backend->completion_observed_at));
  }
  backend->completion_observed = false;
  telemetry_event(backend);
  return LARDON3D_ORB_VULKAN_OK;
}

static Lardon3DOrbVulkanResult orb_vulkan_top2_finish_impl(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request, Lardon3DOrbTop2 *output,
    size_t output_capacity) {
  if (!backend || !request || request->generation == 0 ||
      request->slot >= LARDON3D_ORB_VULKAN_MAX_INFLIGHT) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  OrbRequestSlot *slot = &backend->slots[request->slot];
  if (!slot->completion_pending || slot->generation != request->generation) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
  const uint32_t feature_count_a = slot->pending_feature_count_a;
  const uint32_t feature_count_b = slot->pending_feature_count_b;
  auto wait_start = std::chrono::steady_clock::now();
  VkResult wait = VK_SUCCESS;
#ifdef LARDON3D_ORB_VULKAN_TESTING
  const char *force_wait_failure = std::getenv(
      "LARDON3D_TEST_VULKAN_FINISH_WAIT_FAILURE");
  if (force_wait_failure && std::strcmp(force_wait_failure, "1") == 0) {
    wait = VK_ERROR_DEVICE_LOST;
  } else
#endif
  {
    wait = vkWaitForFences(backend->device, 1, &slot->completion_fence,
                           VK_TRUE, UINT64_MAX);
  }
  saturating_add(&backend->telemetry.fence_wait_ns, elapsed_ns(wait_start));
  slot->completion_pending = false;
  slot->pending_feature_count_a = 0;
  slot->pending_feature_count_b = 0;
  if (wait != VK_SUCCESS) {
    return fail_session_locked(backend);
  }
  if (feature_count_a == 0 || feature_count_b == 0 ||
      feature_count_a > LARDON3D_FEATURE_MAX_FEATURES ||
      feature_count_b > LARDON3D_FEATURE_MAX_FEATURES) {
    return fail_session_locked(backend);
  }
  saturating_add(&backend->telemetry.completions, 1);
  backend->completion_observed = true;
  backend->completion_observed_at = std::chrono::steady_clock::now();
  telemetry_event(backend);
  if (!output || output_capacity < feature_count_a) {
    /* Even invalid consumer storage consumes the unique completed request;
     * the next begin can never inherit or overwrite an abandoned slot. */
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  auto readback_start = std::chrono::steady_clock::now();
#ifdef LARDON3D_ORB_VULKAN_TESTING
  const char *force_readback_failure = std::getenv(
      "LARDON3D_TEST_VULKAN_READBACK_FAILURE");
  /* Test builds inject the failure after exact fence ownership was consumed.
   * Production takes the same fail-session branch only on a real mapped-memory
   * synchronization failure, so no request or partial top-2 evidence survives. */
  if (force_readback_failure
      && std::strcmp(force_readback_failure, "1") == 0) {
    return fail_session_locked(backend);
  }
#endif
  if (!synchronize_host_read(backend, slot->output)) {
    return fail_session_locked(backend);
  }
  read_gpu_time(backend, slot->query_pool);

  const RawTop2 *raw = static_cast<const RawTop2 *>(slot->output.mapping);
  uint32_t neighbors = std::min(feature_count_b, 2U);
  for (uint32_t index = 0; index < feature_count_a; ++index) {
    output[index].neighbor_count = neighbors;
    output[index].best_index = raw[index].best_index;
    output[index].best_distance = raw[index].best_distance;
    output[index].second_index = neighbors == 2 ? raw[index].second_index : 0;
    output[index].second_distance = neighbors == 2 ? raw[index].second_distance : 0;
  }
  saturating_add(&backend->telemetry.readback_ns,
                 elapsed_ns(readback_start));
  saturating_add(&backend->telemetry.gpu_execution_ns, backend->last_gpu_ns);
  telemetry_event(backend);
  return LARDON3D_ORB_VULKAN_OK;
}

static Lardon3DOrbVulkanResult orb_vulkan_top2_discard_impl(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request) {
  if (!backend || !request || request->generation == 0 ||
      request->slot >= LARDON3D_ORB_VULKAN_MAX_INFLIGHT) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  OrbRequestSlot *slot = &backend->slots[request->slot];
  if (!slot->completion_pending || slot->generation != request->generation) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
  if (slot->completion_pending) {
    saturating_add(&backend->telemetry.discards, 1);
    telemetry_event(backend);
    VkResult wait = VK_SUCCESS;
    auto wait_start = std::chrono::steady_clock::now();
#ifdef LARDON3D_ORB_VULKAN_TESTING
    const char *force_failure = std::getenv(
        "LARDON3D_TEST_VULKAN_WAIT_FAILURE");
    if (force_failure && std::strcmp(force_failure, "1") == 0) {
      wait = VK_ERROR_DEVICE_LOST;
    } else
#endif
    {
      wait = vkWaitForFences(backend->device, 1, &slot->completion_fence,
                             VK_TRUE, UINT64_MAX);
    }
    saturating_add(&backend->telemetry.fence_wait_ns,
                   elapsed_ns(wait_start));
    slot->completion_pending = false;
    slot->pending_feature_count_a = 0;
    slot->pending_feature_count_b = 0;
    if (wait != VK_SUCCESS) {
      /* A failed wait invalidates all reusable command/buffer state. Destroy
       * and permanently fail this session before another submit can race it. */
      return fail_session_locked(backend);
    }
  }
  return LARDON3D_ORB_VULKAN_OK;
}

extern "C" Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_begin(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *descriptors_a,
    uint32_t feature_count_a, const unsigned char *descriptors_b,
    uint32_t feature_count_b, Lardon3DOrbVulkanRequest *request) {
  try {
    return orb_vulkan_top2_begin_impl(
        backend, descriptors_a, feature_count_a, descriptors_b,
        feature_count_b, request, true);
  } catch (...) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
}

extern "C" Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_finish(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request, Lardon3DOrbTop2 *output,
    size_t output_capacity) {
  try {
    return orb_vulkan_top2_finish_impl(backend, request, output,
                                       output_capacity);
  } catch (...) {
    /* If locking/host access raised after a request became active, make one
     * bounded discard attempt before reporting failure. This preserves the
     * consume-on-every-result contract even for C++ runtime failures. */
    try {
      (void)orb_vulkan_top2_discard_impl(backend, request);
    } catch (...) {
      /* A second synchronization exception is contained at the C boundary. */
    }
    return LARDON3D_ORB_VULKAN_FAILED;
  }
}

extern "C" Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_discard(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request) {
  try {
    return orb_vulkan_top2_discard_impl(backend, request);
  } catch (...) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
}

extern "C" bool lardon3d_orb_vulkan_internal_telemetry(
    Lardon3DOrbVulkanBackend *backend,
    Lardon3DOrbVulkanTelemetry *telemetry) {
  if (!backend || !telemetry) return false;
  /* Private C consumers receive an all-or-nothing snapshot. Mutex/runtime
   * exceptions are contained here and can only make telemetry unknown; they
   * never escape C or alter the active scientific request. */
  try {
    std::lock_guard<std::mutex> lock(backend->mutex);
    *telemetry = backend->telemetry;
    telemetry->gpu_timestamps_available = backend->timestamps_available;
    telemetry->pending_slots = 0;
    for (const OrbRequestSlot &slot : backend->slots) {
      if (slot.completion_pending) ++telemetry->pending_slots;
    }
    telemetry->slot_pending = telemetry->pending_slots != 0;
    telemetry->retained_capacity = backend->retained_capacity;
    telemetry->retained_payload_bytes =
        static_cast<uint64_t>(backend->retained_capacity)
            * LARDON3D_ORB_VULKAN_PER_SLOT_BYTES;
    telemetry->sequence_capacity_active =
        backend->sequence_capacity_active;
    return true;
  } catch (...) {
    *telemetry = Lardon3DOrbVulkanTelemetry{};
    return false;
  }
}

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
static Lardon3DOrbVulkanResult sift_vulkan_top2_impl(
    Lardon3DOrbVulkanBackend *backend, const float *descriptors_a,
    uint32_t feature_count_a, const float *descriptors_b,
    uint32_t feature_count_b, Lardon3DSiftTop2 *output, size_t output_capacity) {
  if (!backend || feature_count_a > LARDON3D_FEATURE_MAX_FEATURES ||
      feature_count_b > LARDON3D_FEATURE_MAX_FEATURES ||
      (feature_count_a > 0 && (!descriptors_a || !output ||
                              output_capacity < feature_count_a)) ||
      (feature_count_b > 0 && !descriptors_b)) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  if (feature_count_a == 0) {
    return LARDON3D_ORB_VULKAN_OK;
  }
  if (feature_count_b == 0) {
    for (uint32_t index = 0; index < feature_count_a; ++index) {
      output[index] = Lardon3DSiftTop2{};
    }
    return LARDON3D_ORB_VULKAN_OK;
  }

  std::lock_guard<std::mutex> lock(backend->mutex);
  if (!initialize_locked(backend)) {
    return LARDON3D_ORB_VULKAN_UNAVAILABLE;
  }
  if (!create_sift_resources_locked(backend)) {
    return fail_session_locked(backend);
  }
  VkDeviceSize bytes_a = static_cast<VkDeviceSize>(feature_count_a) * 128 *
                         sizeof(float);
  VkDeviceSize bytes_b = static_cast<VkDeviceSize>(feature_count_b) * 128 *
                         sizeof(float);
  std::memcpy(backend->sift_descriptors_a.mapping, descriptors_a,
              static_cast<size_t>(bytes_a));
  std::memcpy(backend->sift_descriptors_b.mapping, descriptors_b,
              static_cast<size_t>(bytes_b));
  if (!synchronize_host_write(backend, backend->sift_descriptors_a, bytes_a) ||
      !synchronize_host_write(backend, backend->sift_descriptors_b, bytes_b)) {
    return fail_session_locked(backend);
  }

  auto start = std::chrono::steady_clock::now();
  VkResult dispatch_result = record_and_submit_sift(
      backend, backend->sift_pipeline, backend->sift_descriptor_set,
      feature_count_a, feature_count_b);
  backend->last_dispatch_ns = elapsed_ns(start);
  if (dispatch_result != VK_SUCCESS ||
      vkWaitForFences(backend->device, 1, &backend->sift_completion_fence,
                      VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
      !synchronize_host_read(backend, backend->sift_output)) {
    return fail_session_locked(backend);
  }
  read_gpu_time(backend, backend->sift_query_pool);

  const RawSiftTop2 *raw =
      static_cast<const RawSiftTop2 *>(backend->sift_output.mapping);
  uint32_t neighbors = std::min(feature_count_b, 2U);
  for (uint32_t index = 0; index < feature_count_a; ++index) {
    output[index].neighbor_count = neighbors;
    output[index].best_index = raw[index].best_index;
    output[index].best_squared_distance = raw[index].best_squared_distance;
    output[index].second_index = neighbors == 2 ? raw[index].second_index : 0;
    output[index].second_squared_distance =
        neighbors == 2 ? raw[index].second_squared_distance : 0.0F;
  }
  return LARDON3D_ORB_VULKAN_OK;
}

extern "C" Lardon3DOrbVulkanResult lardon3d_sift_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const float *descriptors_a,
    uint32_t feature_count_a, const float *descriptors_b,
    uint32_t feature_count_b, Lardon3DSiftTop2 *output,
    size_t output_capacity) {
  /* Feasibility remains a C ABI. Mutex/allocation/runtime exceptions must not
   * cross it, including failures while the shared initialization gate runs. */
  try {
    return sift_vulkan_top2_impl(
        backend, descriptors_a, feature_count_a, descriptors_b,
        feature_count_b, output, output_capacity);
  } catch (...) {
    return LARDON3D_ORB_VULKAN_FAILED;
  }
}
#endif

extern "C" bool lardon3d_orb_vulkan_backend_info(
    Lardon3DOrbVulkanBackend *backend, Lardon3DOrbVulkanInfo *info) {
  if (!backend || !info) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(backend->mutex);
    std::memset(info, 0, sizeof(*info));
    info->available = backend->state == BackendState::kAvailable;
    info->initialized = backend->state != BackendState::kUninitialized;
    info->dedicated_compute_queue = backend->dedicated_compute_queue;
    info->workgroup_size = backend->workgroup_size;
    info->permanent_payload_bytes = static_cast<uint64_t>(
        LARDON3D_ORB_VULKAN_FIXED_BYTES
        + backend->retained_capacity *
            LARDON3D_ORB_VULKAN_PER_SLOT_BYTES);
    info->initialization_ns = backend->initialization_ns;
    info->dispatch_ns = backend->last_dispatch_ns;
    info->gpu_ns = backend->last_gpu_ns;
    if (backend->physical_device != VK_NULL_HANDLE) {
      std::snprintf(info->device_name, sizeof(info->device_name), "%s",
                    backend->properties.deviceName);
    }
    return true;
  } catch (...) {
    std::memset(info, 0, sizeof(*info));
    return false;
  }
}

#else

struct Lardon3DOrbVulkanBackend {
  std::mutex mutex;
  Lardon3DOrbVulkanTelemetry telemetry{};
};

extern "C" Lardon3DOrbVulkanBackend *lardon3d_orb_vulkan_backend_create(void) {
  try {
    return new (std::nothrow) Lardon3DOrbVulkanBackend();
  } catch (...) {
    return nullptr;
  }
}

extern "C" void lardon3d_orb_vulkan_backend_destroy(
    Lardon3DOrbVulkanBackend *backend) {
  try {
    delete backend;
  } catch (...) {
    /* Portable C boundary retains the same exception-containment contract. */
  }
}

extern "C" bool lardon3d_orb_vulkan_should_use(uint32_t, uint32_t) {
  return false;
}

extern "C" bool lardon3d_orb_vulkan_internal_begin_sequence(
    Lardon3DOrbVulkanBackend *backend, uint32_t inflight_capacity) {
  (void)backend;
  (void)inflight_capacity;
  return false;
}

extern "C" bool lardon3d_orb_vulkan_internal_end_sequence(
    Lardon3DOrbVulkanBackend *backend) {
  return backend != nullptr;
}

extern "C" Lardon3DOrbVulkanResult lardon3d_orb_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *, uint32_t feature_count_a,
    const unsigned char *, uint32_t feature_count_b, Lardon3DOrbTop2 *output,
    size_t output_capacity) {
  if (!backend || (feature_count_a > 0 && (!output || output_capacity < feature_count_a))) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  if (feature_count_a == 0 || feature_count_b == 0) {
    for (uint32_t index = 0; index < feature_count_a; ++index) {
      output[index] = Lardon3DOrbTop2{};
    }
    return LARDON3D_ORB_VULKAN_OK;
  }
  return LARDON3D_ORB_VULKAN_UNAVAILABLE;
}

extern "C" Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_begin(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *,
    uint32_t feature_count_a, const unsigned char *, uint32_t feature_count_b,
    Lardon3DOrbVulkanRequest *request) {
  if (!backend || feature_count_a == 0 || feature_count_b == 0 || !request) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  *request = Lardon3DOrbVulkanRequest{};
  return LARDON3D_ORB_VULKAN_UNAVAILABLE;
}

extern "C" Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_finish(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request, Lardon3DOrbTop2 *output,
    size_t output_capacity) {
  if (!backend || !request || request->generation == 0 || !output ||
      output_capacity == 0) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  return LARDON3D_ORB_VULKAN_UNAVAILABLE;
}

extern "C" Lardon3DOrbVulkanResult
lardon3d_orb_vulkan_internal_top2_discard(
    Lardon3DOrbVulkanBackend *backend,
    const Lardon3DOrbVulkanRequest *request) {
  return backend && request && request->generation != 0
             ? LARDON3D_ORB_VULKAN_OK
             : LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
}

extern "C" bool lardon3d_orb_vulkan_internal_telemetry(
    Lardon3DOrbVulkanBackend *backend,
    Lardon3DOrbVulkanTelemetry *telemetry) {
  if (!backend || !telemetry) return false;
  /* Preserve the same C exception boundary in the portable build. */
  try {
    std::lock_guard<std::mutex> lock(backend->mutex);
    *telemetry = backend->telemetry;
    return true;
  } catch (...) {
    *telemetry = Lardon3DOrbVulkanTelemetry{};
    return false;
  }
}

#ifdef LARDON3D_ORB_VULKAN_TESTING
extern "C" bool lardon3d_orb_vulkan_internal_test_set_slot_generation(
    Lardon3DOrbVulkanBackend *, uint32_t, uint64_t) {
  return false;
}
#endif

#ifdef LARDON3D_SIFT_VULKAN_FEASIBILITY
extern "C" Lardon3DOrbVulkanResult lardon3d_sift_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const float *, uint32_t feature_count_a,
    const float *, uint32_t feature_count_b, Lardon3DSiftTop2 *output,
    size_t output_capacity) {
  if (!backend || (feature_count_a > 0 && (!output || output_capacity < feature_count_a))) {
    return LARDON3D_ORB_VULKAN_INVALID_ARGUMENT;
  }
  if (feature_count_a == 0 || feature_count_b == 0) {
    for (uint32_t index = 0; index < feature_count_a; ++index) {
      output[index] = Lardon3DSiftTop2{};
    }
    return LARDON3D_ORB_VULKAN_OK;
  }
  return LARDON3D_ORB_VULKAN_UNAVAILABLE;
}
#endif

extern "C" bool lardon3d_orb_vulkan_backend_info(
    Lardon3DOrbVulkanBackend *backend, Lardon3DOrbVulkanInfo *info) {
  if (!backend || !info) {
    return false;
  }
  std::memset(info, 0, sizeof(*info));
  return true;
}

#endif
