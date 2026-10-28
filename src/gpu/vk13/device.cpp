#include "gpu/vk13.h"
#include <vector>

namespace vk13 {

// Vulkan 1.3 devices with dynamicRendering + synchronization2.
static bool pick_physical_device(GpuDevice* dev) {
  std::vector<VkPhysicalDevice> candidates;
  for (VkPhysicalDevice pd : vkc::part::physical_devices(dev, VK_API_VERSION_1_3)) {
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (f13.dynamicRendering && f13.synchronization2) candidates.push_back(pd);
  }
  return vkc::part::physical_device_pick(dev, candidates, "1.3 + dynamicRendering + synchronization2");
}

static bool create_device(GpuDevice* dev) {
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  return vkc::part::device_create(dev, &f13);
}

bool device_init(GpuDevice* dev, bool validation, void* native_surface) {
  bool ok = vkc::part::instance_create(dev, validation, VK_API_VERSION_1_3) &&
            surface_create(dev, native_surface) && pick_physical_device(dev) && create_device(dev) &&
            vkc::part::allocator_create(dev, VK_API_VERSION_1_3) && vkc::part::tracy_context_create(dev) &&
            vkc::part::sampler_create(dev);
  if (!ok) device_shutdown(dev);
  return ok;
}

} // namespace vk13
