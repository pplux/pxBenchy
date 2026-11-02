#include "gpu/vk13.h"
#include <vector>

namespace vk13 {

// The features this mode needs; get = vkGetPhysicalDeviceFeatures2 of pd's instance (global or probe table).
static bool has_features(PFN_vkGetPhysicalDeviceFeatures2 get, VkPhysicalDevice pd) {
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f13;
  get(pd, &f2);
  return f13.dynamicRendering && f13.synchronization2;
}

// Vulkan 1.3 devices with dynamicRendering + synchronization2.
static bool pick_physical_device(GpuDevice* dev) {
  std::vector<VkPhysicalDevice> candidates;
  for (VkPhysicalDevice pd : vkc::part::physical_devices(dev, VK_API_VERSION_1_3)) {
    if (has_features(vkGetPhysicalDeviceFeatures2, pd)) { candidates.push_back(pd); }
  }
  return vkc::part::physical_device_pick(dev, candidates, "1.3 + dynamicRendering + synchronization2");
}

bool supported() {
  VolkInstanceTable table;
  VkInstance instance = vkc::part::probe_instance_create(VK_API_VERSION_1_3, &table);
  if (!instance) { return false; }
  bool ok = false;
  for (VkPhysicalDevice pd : vkc::part::physical_devices(instance, &table, VK_API_VERSION_1_3)) {
    ok = ok || has_features(table.vkGetPhysicalDeviceFeatures2, pd);
  }
  table.vkDestroyInstance(instance, nullptr);
  return ok;
}

static bool create_device(GpuDevice* dev) {
  vkc::part::DeviceBuild b;
  vkc::part::device_create_begin(dev, &b);
  VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  b.f2.pNext = &f13;
  VkResult r = vkCreateDevice(dev->physical, &b.info, nullptr, &dev->device);
  return vkc::part::device_create_end(dev, r);
}

bool device_init(GpuDevice* dev, bool validation, void* native_surface) {
  bool ok = vkc::part::instance_create(dev, validation, VK_API_VERSION_1_3) && surface_create(dev, native_surface) &&
            pick_physical_device(dev) && create_device(dev) && vkc::part::allocator_create(dev, VK_API_VERSION_1_3) &&
            vkc::part::tracy_context_create(dev) && vkc::part::sampler_create(dev);
  if (!ok) { device_shutdown(dev); }
  return ok;
}

} // namespace vk13
