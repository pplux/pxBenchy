#pragma once
#include <cstdint>
#include <vector>
#include "gpu/vk.h"

namespace vkc {

bool surface_create(GpuDevice* dev, void* native_surface); // CAMetalLayer* | ANativeWindow*
void surface_destroy(GpuDevice* dev);
void device_shutdown(GpuDevice* dev); // destroys surface too; safe on a partially initialized device

} // namespace vkc

// Device creation pieces shared by the modes; each mode's device_init picks its version and features.
namespace vkc::part {

// Loads the Vulkan loader (volk), then creates the instance with apiVersion = api: surface + platform surface
// extensions, portability enumeration when available, and with validation the Khronos layer, synchronization
// validation and a debug messenger printing warnings/errors. Fails (prints) when the instance version is below api.
bool instance_create(GpuDevice* dev, bool validation, uint32_t api);

// Temporary instance for probing physical devices (no surface, no layers, no debug messenger): apiVersion = api,
// portability enumeration when available. Its functions go to *table only; volk's global instance and device functions
// are left as they were. VK_NULL_HANDLE (silent) when the loader is missing, the instance version is below api or
// creation fails. Destroy with table->vkDestroyInstance.
VkInstance probe_instance_create(uint32_t api, VolkInstanceTable* table);

// Physical devices of dev->instance (or of a probe instance, through its table) with apiVersion >= min_api, in
// enumeration order.
std::vector<VkPhysicalDevice> physical_devices(GpuDevice* dev, uint32_t min_api);
std::vector<VkPhysicalDevice> physical_devices(VkInstance instance, const VolkInstanceTable* table, uint32_t min_api);

// Picks among candidates one with VK_KHR_swapchain and a graphics + present (dev->surface) queue family, preferring
// discrete over integrated over the rest; fills physical, props, queue_family and driver / driver_props. Fails (prints)
// when none qualifies: "no Vulkan device with <need> + VK_KHR_swapchain + graphics/present queue".
bool physical_device_pick(GpuDevice* dev, const std::vector<VkPhysicalDevice>& candidates, const char* need);

// Everything vkCreateDevice needs except the mode features. `info` points into the struct, so it is not copyable.
struct DeviceBuild {
  std::vector<const char*> exts;
  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {};
  VkPhysicalDeviceFeatures2 f2 = {}; // pNext left null for the mode features
  VkDeviceCreateInfo info = {};      // pNext = &f2
  DeviceBuild() = default;
  DeviceBuild(const DeviceBuild&) = delete;
  DeviceBuild& operator=(const DeviceBuild&) = delete;
};

// Fills *b for one queue of queue_family with swapchain, portability subset and display timing (when available; sets
// dev->display_timing) and f2 { multiDrawIndirect + drawIndirectFirstInstance when supported; sets
// dev->multi_draw_indirect }. The mode then chains its features to b->f2.pNext, calls vkCreateDevice into
// dev->device and passes the result to device_create_end.
void device_create_begin(GpuDevice* dev, DeviceBuild* b);
// Takes the vkCreateDevice result: fails (prints) on error, else loads the device functions (volk) and dev->queue.
bool device_create_end(GpuDevice* dev, VkResult r);

// VMA allocator for vulkanApiVersion = api, Vulkan functions from volk, device memory blocks reported to Tracy.
bool allocator_create(GpuDevice* dev, uint32_t api);
bool tracy_context_create(GpuDevice* dev);
bool sampler_create(GpuDevice* dev); // GpuDevice::sampler

} // namespace vkc::part
