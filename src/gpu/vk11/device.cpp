#include "gpu/vk11.h"
#include "gpu/vk11/caches.h"

namespace vk11 {

// Vulkan 1.1 devices, no extra features.
static bool pick_physical_device(GpuDevice* dev) {
  return vkc::part::physical_device_pick(dev, vkc::part::physical_devices(dev, VK_API_VERSION_1_1), "1.1");
}

static bool create_device(GpuDevice* dev) {
  vkc::part::DeviceBuild b;
  vkc::part::device_create_begin(dev, &b);
  VkResult r = vkCreateDevice(dev->physical, &b.info, nullptr, &dev->device);
  return vkc::part::device_create_end(dev, r);
}

bool device_init(GpuDevice* dev, bool validation, void* native_surface) {
  bool ok = vkc::part::instance_create(dev, validation, VK_API_VERSION_1_1) && surface_create(dev, native_surface) &&
            pick_physical_device(dev) && create_device(dev) && vkc::part::allocator_create(dev, VK_API_VERSION_1_1) &&
            vkc::part::tracy_context_create(dev) && vkc::part::sampler_create(dev);
  if (!ok) {
    device_shutdown(dev);
    return false;
  }
  dev->mode = std::make_unique<Caches>();
  return true;
}

void device_shutdown(GpuDevice* dev) {
  caches_destroy(dev);
  vkc::device_shutdown(dev);
}

} // namespace vk11
