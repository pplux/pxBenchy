#include "gpu/vkc/device.h"
#include <tracy/Tracy.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if defined(PXB_VULKAN_LOADER_PATH)
#include <dlfcn.h>
#endif

static bool load_vulkan() {
  if (volkInitialize() == VK_SUCCESS) {
    printf("vulkan loader: volk default\n");
    return true;
  }
#if defined(PXB_VULKAN_LOADER_PATH)
  void* lib = dlopen(PXB_VULKAN_LOADER_PATH, RTLD_NOW | RTLD_LOCAL);
  if (lib) {
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (gipa) {
      volkInitializeCustom(gipa);
      printf("vulkan loader: baked fallback (%s)\n", PXB_VULKAN_LOADER_PATH);
      return true;
    }
  }
  fprintf(stderr, "error: Vulkan loader not found (volk default failed, baked fallback %s failed)\n",
          PXB_VULKAN_LOADER_PATH);
#else
  fprintf(stderr, "error: Vulkan loader not found\n");
#endif
  return false;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*) {
  const char* prefix = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "vk error" : "vk warning";
  fprintf(stderr, "[%s] %s\n", prefix, data->pMessage);
  return VK_FALSE;
}

static bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& e : exts)
    if (strcmp(e.extensionName, name) == 0) return true;
  return false;
}

// Tracy pool "vk_blocks": VkDeviceMemory blocks VMA allocates from the driver
static void VKAPI_PTR vma_block_alloc(VmaAllocator, uint32_t, VkDeviceMemory memory, VkDeviceSize size, void*) {
  TracyAllocN((void*)(uintptr_t)memory, size, "vk_blocks");
  (void)memory;
  (void)size;
}

static void VKAPI_PTR vma_block_free(VmaAllocator, uint32_t, VkDeviceMemory memory, VkDeviceSize, void*) {
  TracyFreeN((void*)(uintptr_t)memory, "vk_blocks");
  (void)memory;
}

namespace vkc::part {

bool instance_create(GpuDevice* dev, bool validation, uint32_t api) {
  dev->validation = validation;
#if defined(PXB_VK_LAYER_DIR)
  if (validation && !getenv("VK_ADD_LAYER_PATH") && !getenv("VK_LAYER_PATH"))
    setenv("VK_ADD_LAYER_PATH", PXB_VK_LAYER_DIR, 1);
#endif
  if (!load_vulkan()) return false;
  uint32_t have = 0;
  vkEnumerateInstanceVersion(&have);
  if (have < api) {
    fprintf(stderr, "error: Vulkan instance version %u.%u < %u.%u\n", VK_API_VERSION_MAJOR(have),
            VK_API_VERSION_MINOR(have), VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
    return false;
  }

  uint32_t count = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> avail(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());

  std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME};
#if defined(VK_USE_PLATFORM_METAL_EXT)
  exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
  exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
  if (validation) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  for (const char* e : exts) {
    if (!has_ext(avail, e)) {
      fprintf(stderr, "error: required instance extension %s not available\n", e);
      return false;
    }
  }
  VkInstanceCreateFlags flags = 0;
  if (has_ext(avail, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }

  std::vector<const char*> layers;
  if (validation) {
    const char* layer = "VK_LAYER_KHRONOS_validation";
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> lp(count);
    vkEnumerateInstanceLayerProperties(&count, lp.data());
    bool found = false;
    for (const auto& l : lp)
      if (strcmp(l.layerName, layer) == 0) found = true;
    if (!found) {
      fprintf(stderr, "error: validation layer %s not found (run without --validation)\n", layer);
      return false;
    }
    layers.push_back(layer);
    // synchronization validation needs VK_EXT_validation_features, provided by the layer
    vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
    std::vector<VkExtensionProperties> lext(count);
    vkEnumerateInstanceExtensionProperties(layer, &count, lext.data());
    if (!has_ext(lext, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
      fprintf(stderr, "error: %s does not provide %s\n", layer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
      return false;
    }
    exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
  }

  VkDebugUtilsMessengerCreateInfoEXT dbg = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  dbg.pfnUserCallback = debug_callback;
  VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
  VkValidationFeaturesEXT vf = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
  vf.enabledValidationFeatureCount = 1;
  vf.pEnabledValidationFeatures = enables;
  vf.pNext = &dbg;

  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "pxBenchy";
  app.pEngineName = "pxBenchy";
  app.apiVersion = api;

  VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pNext = validation ? &vf : nullptr; // dbg chained after vf also covers vkCreateInstance/vkDestroyInstance
  ci.flags = flags;
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = (uint32_t)layers.size();
  ci.ppEnabledLayerNames = layers.data();
  ci.enabledExtensionCount = (uint32_t)exts.size();
  ci.ppEnabledExtensionNames = exts.data();
  VkResult r = vkCreateInstance(&ci, nullptr, &dev->instance);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateInstance failed (%d)\n", (int)r);
    return false;
  }
  volkLoadInstance(dev->instance);

  if (validation) {
    r = vkCreateDebugUtilsMessengerEXT(dev->instance, &dbg, nullptr, &dev->messenger);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateDebugUtilsMessengerEXT failed (%d)\n", (int)r);
      return false;
    }
  }
  return true;
}

std::vector<VkPhysicalDevice> physical_devices(GpuDevice* dev, uint32_t min_api) {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(dev->instance, &count, nullptr);
  std::vector<VkPhysicalDevice> pds(count);
  vkEnumeratePhysicalDevices(dev->instance, &count, pds.data());
  std::vector<VkPhysicalDevice> out;
  for (VkPhysicalDevice pd : pds) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion >= min_api) out.push_back(pd);
  }
  return out;
}

bool physical_device_pick(GpuDevice* dev, const std::vector<VkPhysicalDevice>& candidates, const char* need) {
  int best_score = -1;
  for (VkPhysicalDevice pd : candidates) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);

    uint32_t ecount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, nullptr);
    std::vector<VkExtensionProperties> exts(ecount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, exts.data());
    if (!has_ext(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qfs.data());
    int family = -1;
    for (uint32_t i = 0; i < qcount; i++) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, dev->surface, &present);
      if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        family = (int)i;
        break;
      }
    }
    if (family < 0) continue;

    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                            : 0;
    if (score > best_score) {
      best_score = score;
      dev->physical = pd;
      dev->props = props;
      dev->queue_family = (uint32_t)family;
    }
  }
  if (!dev->physical) {
    fprintf(stderr, "error: no Vulkan device with %s + VK_KHR_swapchain + graphics/present queue\n", need);
    return false;
  }
  return true;
}

bool device_create(GpuDevice* dev, void* features_next) {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(dev->physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> avail(count);
  vkEnumerateDeviceExtensionProperties(dev->physical, nullptr, &count, avail.data());

  std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  if (has_ext(avail, "VK_KHR_portability_subset")) exts.push_back("VK_KHR_portability_subset");
  dev->display_timing = has_ext(avail, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
  if (dev->display_timing) exts.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = dev->queue_family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = features_next;
  VkPhysicalDeviceFeatures supported;
  vkGetPhysicalDeviceFeatures(dev->physical, &supported);
  dev->multi_draw_indirect = supported.multiDrawIndirect && supported.drawIndirectFirstInstance;
  f2.features.multiDrawIndirect = dev->multi_draw_indirect;
  f2.features.drawIndirectFirstInstance = dev->multi_draw_indirect;

  VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.pNext = &f2;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &qci;
  ci.enabledExtensionCount = (uint32_t)exts.size();
  ci.ppEnabledExtensionNames = exts.data();
  VkResult r = vkCreateDevice(dev->physical, &ci, nullptr, &dev->device);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateDevice failed (%d)\n", (int)r);
    return false;
  }
  volkLoadDevice(dev->device);
  vkGetDeviceQueue(dev->device, dev->queue_family, 0, &dev->queue);
  return true;
}

bool allocator_create(GpuDevice* dev, uint32_t api) {
  static VmaDeviceMemoryCallbacks callbacks = {vma_block_alloc, vma_block_free, nullptr};
  VmaAllocatorCreateInfo ci = {};
  ci.physicalDevice = dev->physical;
  ci.device = dev->device;
  ci.instance = dev->instance;
  ci.vulkanApiVersion = api;
  ci.pDeviceMemoryCallbacks = &callbacks;
  VmaVulkanFunctions fns;
  VkResult r = vmaImportVulkanFunctionsFromVolk(&ci, &fns);
  if (r == VK_SUCCESS) {
    ci.pVulkanFunctions = &fns;
    r = vmaCreateAllocator(&ci, &dev->vma);
  }
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vmaCreateAllocator failed (%d)\n", (int)r);
    return false;
  }
  return true;
}

bool tracy_context_create(GpuDevice* dev) {
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // Tracy re-begins the same buffer
  pci.queueFamilyIndex = dev->queue_family;
  VkResult r = vkCreateCommandPool(dev->device, &pci, nullptr, &dev->tracy_pool);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateCommandPool failed (%d)\n", (int)r);
    return false;
  }
  VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = dev->tracy_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  r = vkAllocateCommandBuffers(dev->device, &ai, &dev->tracy_cmd);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkAllocateCommandBuffers failed (%d)\n", (int)r);
    return false;
  }
  dev->tracy = TracyVkContext(dev->physical, dev->device, dev->queue, dev->tracy_cmd);
  return true;
}

bool sampler_create(GpuDevice* dev) {
  VkSamplerCreateInfo ci = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR,
    .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .minLod = 0.0f,
    .maxLod = VK_LOD_CLAMP_NONE,
  };
  VkResult r = vkCreateSampler(dev->device, &ci, nullptr, &dev->sampler);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateSampler failed (%d)\n", (int)r);
    return false;
  }
  return true;
}

} // namespace vkc::part

namespace vkc {

bool surface_create(GpuDevice* dev, void* native_surface) {
  if (!native_surface) {
    fprintf(stderr, "error: no native surface\n");
    return false;
  }
  VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
#if defined(VK_USE_PLATFORM_METAL_EXT)
  VkMetalSurfaceCreateInfoEXT ci = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
  ci.pLayer = (const CAMetalLayer*)native_surface;
  r = vkCreateMetalSurfaceEXT(dev->instance, &ci, nullptr, &dev->surface);
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
  VkAndroidSurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  ci.window = (ANativeWindow*)native_surface;
  r = vkCreateAndroidSurfaceKHR(dev->instance, &ci, nullptr, &dev->surface);
#endif
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: surface creation failed (%d)\n", (int)r);
    return false;
  }
  return true;
}

void surface_destroy(GpuDevice* dev) {
  if (dev->surface) vkDestroySurfaceKHR(dev->instance, dev->surface, nullptr);
  dev->surface = VK_NULL_HANDLE;
}

void device_shutdown(GpuDevice* dev) {
  if (dev->sampler) vkDestroySampler(dev->device, dev->sampler, nullptr);
  dev->sampler = VK_NULL_HANDLE;
  if (dev->tracy) {
    TracyVkDestroy(dev->tracy);
  }
  dev->tracy = nullptr;
  if (dev->tracy_pool) vkDestroyCommandPool(dev->device, dev->tracy_pool, nullptr);
  dev->tracy_pool = VK_NULL_HANDLE;
  dev->tracy_cmd = VK_NULL_HANDLE;
  if (dev->vma) vmaDestroyAllocator(dev->vma);
  dev->vma = nullptr;
  if (dev->device) vkDestroyDevice(dev->device, nullptr);
  dev->device = VK_NULL_HANDLE;
  dev->queue = VK_NULL_HANDLE;
  if (dev->instance) {
    surface_destroy(dev);
    if (dev->messenger) vkDestroyDebugUtilsMessengerEXT(dev->instance, dev->messenger, nullptr);
    vkDestroyInstance(dev->instance, nullptr);
  }
  dev->messenger = VK_NULL_HANDLE;
  dev->instance = VK_NULL_HANDLE;
  dev->physical = VK_NULL_HANDLE;
}

} // namespace vkc
