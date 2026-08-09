#include <lardon3d/orb_vulkan_backend.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include <lardon3d/feature_extractor.h>

#include "matcher_vulkan_config.h"

#if LARDON3D_HAVE_VULKAN

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <vulkan/vulkan.h>

#include "orb_top2_spv.h"

namespace {

constexpr VkDeviceSize kDescriptorBufferBytes =
    static_cast<VkDeviceSize>(LARDON3D_FEATURE_MAX_FEATURES) * 32;
constexpr VkDeviceSize kOutputBufferBytes =
    static_cast<VkDeviceSize>(LARDON3D_FEATURE_MAX_FEATURES) * 4 * sizeof(uint32_t);
static_assert(kDescriptorBufferBytes * 2 + kOutputBufferBytes ==
              LARDON3D_ORB_VULKAN_PERMANENT_BUFFER_BYTES);
constexpr uint64_t kDefaultVulkanWorkThreshold = 768ULL * 768ULL;
constexpr uint32_t kDefaultWorkgroupSize = 32;

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
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkQueryPool query_pool = VK_NULL_HANDLE;
  bool timestamps_available = false;
  Buffer descriptors_a;
  Buffer descriptors_b;
  Buffer output;
  uint32_t workgroup_size = kDefaultWorkgroupSize;
  uint64_t initialization_ns = 0;
  uint64_t last_dispatch_ns = 0;
  uint64_t last_gpu_ns = 0;
};

namespace {

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
  destroy_buffer(backend, &backend->descriptors_a);
  destroy_buffer(backend, &backend->descriptors_b);
  destroy_buffer(backend, &backend->output);
  if (backend->query_pool != VK_NULL_HANDLE) {
    vkDestroyQueryPool(backend->device, backend->query_pool, nullptr);
  }
  if (backend->pipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(backend->device, backend->pipeline, nullptr);
  }
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
  backend->command_buffer = VK_NULL_HANDLE;
  backend->descriptor_set_layout = VK_NULL_HANDLE;
  backend->pipeline_layout = VK_NULL_HANDLE;
  backend->pipeline = VK_NULL_HANDLE;
  backend->descriptor_pool = VK_NULL_HANDLE;
  backend->descriptor_set = VK_NULL_HANDLE;
  backend->query_pool = VK_NULL_HANDLE;
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
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(backend->device, &command_info,
                               &backend->command_buffer) != VK_SUCCESS) {
    return false;
  }
  return true;
}

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

static bool create_buffers_and_descriptors(Lardon3DOrbVulkanBackend *backend) {
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = 3;
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
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
  if (vkAllocateDescriptorSets(backend->device, &set_info,
                               &backend->descriptor_set) != VK_SUCCESS ||
      !create_buffer(backend, kDescriptorBufferBytes, &backend->descriptors_a) ||
      !create_buffer(backend, kDescriptorBufferBytes, &backend->descriptors_b) ||
      !create_buffer(backend, kOutputBufferBytes, &backend->output)) {
    return false;
  }

  VkDescriptorBufferInfo buffer_info[3] = {
      {backend->descriptors_a.buffer, 0, backend->descriptors_a.size},
      {backend->descriptors_b.buffer, 0, backend->descriptors_b.size},
      {backend->output.buffer, 0, backend->output.size},
  };
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t index = 0; index < 3; ++index) {
    writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[index].dstSet = backend->descriptor_set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &buffer_info[index];
  }
  vkUpdateDescriptorSets(backend->device, 3, writes, 0, nullptr);

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
    if (vkCreateQueryPool(backend->device, &query_info, nullptr,
                          &backend->query_pool) != VK_SUCCESS) {
      backend->timestamps_available = false;
    }
  }
  return true;
}

static bool initialize_locked(Lardon3DOrbVulkanBackend *backend) {
  if (backend->state == BackendState::kAvailable) {
    return true;
  }
  if (backend->state != BackendState::kUninitialized) {
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
                 create_buffers_and_descriptors(backend);
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

static VkResult record_and_submit(Lardon3DOrbVulkanBackend *backend,
                                  uint32_t count_a, uint32_t count_b) {
  VkResult result = vkResetCommandBuffer(backend->command_buffer, 0);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(backend->command_buffer, &begin_info);
  if (result != VK_SUCCESS) {
    return result;
  }
  if (backend->timestamps_available) {
    vkCmdResetQueryPool(backend->command_buffer, backend->query_pool, 0, 2);
    vkCmdWriteTimestamp(backend->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        backend->query_pool, 0);
  }
  vkCmdBindPipeline(backend->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    backend->pipeline);
  vkCmdBindDescriptorSets(backend->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          backend->pipeline_layout, 0, 1,
                          &backend->descriptor_set, 0, nullptr);
  uint32_t counts[2] = {count_a, count_b};
  vkCmdPushConstants(backend->command_buffer, backend->pipeline_layout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(counts), counts);
  uint32_t groups = (count_a + backend->workgroup_size - 1) /
                    backend->workgroup_size;
  vkCmdDispatch(backend->command_buffer, groups, 1, 1);
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(backend->command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0,
                       nullptr);
  if (backend->timestamps_available) {
    vkCmdWriteTimestamp(backend->command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        backend->query_pool, 1);
  }
  result = vkEndCommandBuffer(backend->command_buffer);
  if (result != VK_SUCCESS) {
    return result;
  }
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &backend->command_buffer;
  result = vkQueueSubmit(backend->queue, 1, &submit_info, VK_NULL_HANDLE);
  if (result != VK_SUCCESS) {
    return result;
  }
  return vkQueueWaitIdle(backend->queue);
}

static void read_gpu_time(Lardon3DOrbVulkanBackend *backend) {
  backend->last_gpu_ns = 0;
  if (!backend->timestamps_available) {
    return;
  }
  uint64_t timestamps[2]{};
  VkResult result = vkGetQueryPoolResults(
      backend->device, backend->query_pool, 0, 2, sizeof(timestamps), timestamps,
      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  if (result == VK_SUCCESS && timestamps[1] >= timestamps[0]) {
    double nanoseconds = static_cast<double>(timestamps[1] - timestamps[0]) *
                         backend->properties.limits.timestampPeriod;
    backend->last_gpu_ns = static_cast<uint64_t>(nanoseconds);
  }
}

}  // namespace

extern "C" Lardon3DOrbVulkanBackend *lardon3d_orb_vulkan_backend_create(void) {
  return new (std::nothrow) Lardon3DOrbVulkanBackend();
}

extern "C" void lardon3d_orb_vulkan_backend_destroy(
    Lardon3DOrbVulkanBackend *backend) {
  if (!backend) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(backend->mutex);
    destroy_vulkan(backend);
  }
  delete backend;
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

extern "C" Lardon3DOrbVulkanResult lardon3d_orb_vulkan_top2(
    Lardon3DOrbVulkanBackend *backend, const unsigned char *descriptors_a,
    uint32_t feature_count_a, const unsigned char *descriptors_b,
    uint32_t feature_count_b, Lardon3DOrbTop2 *output, size_t output_capacity) {
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
      output[index] = Lardon3DOrbTop2{};
    }
    return LARDON3D_ORB_VULKAN_OK;
  }

  std::lock_guard<std::mutex> lock(backend->mutex);
  if (!initialize_locked(backend)) {
    return LARDON3D_ORB_VULKAN_UNAVAILABLE;
  }
  VkDeviceSize bytes_a = static_cast<VkDeviceSize>(feature_count_a) * 32;
  VkDeviceSize bytes_b = static_cast<VkDeviceSize>(feature_count_b) * 32;
  std::memcpy(backend->descriptors_a.mapping, descriptors_a,
              static_cast<size_t>(bytes_a));
  std::memcpy(backend->descriptors_b.mapping, descriptors_b,
              static_cast<size_t>(bytes_b));
  if (!synchronize_host_write(backend, backend->descriptors_a, bytes_a) ||
      !synchronize_host_write(backend, backend->descriptors_b, bytes_b)) {
    return fail_session_locked(backend);
  }

  auto start = std::chrono::steady_clock::now();
#ifdef LARDON3D_ORB_VULKAN_TESTING
  const char *force_failure = std::getenv("LARDON3D_TEST_VULKAN_DEVICE_LOST");
  if (force_failure && std::strcmp(force_failure, "1") == 0) {
    return fail_session_locked(backend);
  }
#endif
  VkResult dispatch_result = record_and_submit(backend, feature_count_a,
                                               feature_count_b);
  backend->last_dispatch_ns = elapsed_ns(start);
  if (dispatch_result != VK_SUCCESS ||
      !synchronize_host_read(backend, backend->output)) {
    return fail_session_locked(backend);
  }
  read_gpu_time(backend);

  const RawTop2 *raw = static_cast<const RawTop2 *>(backend->output.mapping);
  uint32_t neighbors = std::min(feature_count_b, 2U);
  for (uint32_t index = 0; index < feature_count_a; ++index) {
    output[index].neighbor_count = neighbors;
    output[index].best_index = raw[index].best_index;
    output[index].best_distance = raw[index].best_distance;
    output[index].second_index = neighbors == 2 ? raw[index].second_index : 0;
    output[index].second_distance = neighbors == 2 ? raw[index].second_distance : 0;
  }
  return LARDON3D_ORB_VULKAN_OK;
}

extern "C" bool lardon3d_orb_vulkan_backend_info(
    Lardon3DOrbVulkanBackend *backend, Lardon3DOrbVulkanInfo *info) {
  if (!backend || !info) {
    return false;
  }
  std::lock_guard<std::mutex> lock(backend->mutex);
  std::memset(info, 0, sizeof(*info));
  info->available = backend->state == BackendState::kAvailable;
  info->initialized = backend->state != BackendState::kUninitialized;
  info->dedicated_compute_queue = backend->dedicated_compute_queue;
  info->workgroup_size = backend->workgroup_size;
  info->permanent_payload_bytes = static_cast<uint64_t>(
      kDescriptorBufferBytes * 2 + kOutputBufferBytes);
  info->initialization_ns = backend->initialization_ns;
  info->dispatch_ns = backend->last_dispatch_ns;
  info->gpu_ns = backend->last_gpu_ns;
  if (backend->physical_device != VK_NULL_HANDLE) {
    std::snprintf(info->device_name, sizeof(info->device_name), "%s",
                  backend->properties.deviceName);
  }
  return true;
}

#else

struct Lardon3DOrbVulkanBackend {};

extern "C" Lardon3DOrbVulkanBackend *lardon3d_orb_vulkan_backend_create(void) {
  return new (std::nothrow) Lardon3DOrbVulkanBackend();
}

extern "C" void lardon3d_orb_vulkan_backend_destroy(
    Lardon3DOrbVulkanBackend *backend) {
  delete backend;
}

extern "C" bool lardon3d_orb_vulkan_should_use(uint32_t, uint32_t) {
  return false;
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

extern "C" bool lardon3d_orb_vulkan_backend_info(
    Lardon3DOrbVulkanBackend *backend, Lardon3DOrbVulkanInfo *info) {
  if (!backend || !info) {
    return false;
  }
  std::memset(info, 0, sizeof(*info));
  return true;
}

#endif
