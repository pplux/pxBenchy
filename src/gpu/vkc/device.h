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

// Physical devices of dev->instance with apiVersion >= min_api, in enumeration order.
std::vector<VkPhysicalDevice> physical_devices(GpuDevice* dev, uint32_t min_api);

// Picks among candidates one with VK_KHR_swapchain and a graphics + present (dev->surface) queue family, preferring
// discrete over integrated over the rest; fills physical, props and queue_family. Fails (prints) when none qualifies:
// "no Vulkan device with <need> + VK_KHR_swapchain + graphics/present queue".
bool physical_device_pick(GpuDevice* dev, const std::vector<VkPhysicalDevice>& candidates, const char* need);

// Creates the device (one queue of queue_family) with swapchain, portability subset and display timing (when
// available) and VkPhysicalDeviceFeatures2 { multiDrawIndirect + drawIndirectFirstInstance when supported } chained to
// features_next (mode features, may be null).
bool device_create(GpuDevice* dev, void* features_next);

// VMA allocator for vulkanApiVersion = api, Vulkan functions from volk, device memory blocks reported to Tracy.
bool allocator_create(GpuDevice* dev, uint32_t api);
bool tracy_context_create(GpuDevice* dev);
bool sampler_create(GpuDevice* dev); // GpuDevice::sampler

} // namespace vkc::part
