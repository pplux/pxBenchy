#include "gpu/vkc/swapchain.h"
#include <algorithm>
#include <cstdio>
#include "gpu/vkc/resources.h"
#include "platform/platform.h"
#include <tracy/Tracy.hpp>

namespace vkc {

const char* present_mode_name(VkPresentModeKHR m) {
  switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
  }
}

static bool create_frame(GpuDevice* dev, GpuFrame& f) {
  {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = dev->queue_family;
    VkResult r = vkCreateCommandPool(dev->device, &pci, nullptr, &f.pool);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateCommandPool failed (%d)\n", (int)r);
      return false;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = f.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(dev->device, &ai, &f.cmd);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkAllocateCommandBuffers failed (%d)\n", (int)r);
      return false;
    }
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    r = vkCreateFence(dev->device, &fci, nullptr, &f.fence);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateFence failed (%d)\n", (int)r);
      return false;
    }
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    r = vkCreateSemaphore(dev->device, &sci, nullptr, &f.image_acquired);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateSemaphore failed (%d)\n", (int)r);
      return false;
    }
  }
  return true;
}

static bool create_frames(GpuDevice* dev, GpuSwapchain* sc) {
  for (GpuFrame& f : sc->frames) {
    if (!create_frame(dev, f)) { return false; }
  }
  return !sc->offscreen || create_frame(dev, sc->blit);
}

static void destroy_frame(GpuDevice* dev, GpuFrame& f) {
  if (f.image_acquired) { vkDestroySemaphore(dev->device, f.image_acquired, nullptr); }
  if (f.fence) { vkDestroyFence(dev->device, f.fence, nullptr); }
  if (f.pool) { vkDestroyCommandPool(dev->device, f.pool, nullptr); }
  f = GpuFrame{};
}

static void destroy_images(GpuDevice* dev, GpuSwapchain* sc) {
  texture_destroy(dev, &sc->offscreen_tex);
  texture_destroy(dev, &sc->capture);
  for (VkImageView v : sc->views) { vkDestroyImageView(dev->device, v, nullptr); }
  for (VkSemaphore s : sc->render_finished) { vkDestroySemaphore(dev->device, s, nullptr); }
  sc->views.clear();
  sc->image_states.clear();
  sc->render_finished.clear();
  sc->images.clear();
}

bool swapchain_create(GpuDevice* dev, GpuSwapchain* sc) {
  if (!sc->frames[0].pool && !create_frames(dev, sc)) { return false; }
  sc->needs_recreate = false;

  VkSurfaceCapabilitiesKHR caps;
  VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physical, dev->surface, &caps);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (%d)\n", (int)r);
    return false;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == 0xFFFFFFFFu) {
    platform_drawable_size(&extent.width, &extent.height);
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  // The app rotates its output itself (view_orientation), so the compositor does not: preTransform = currentTransform
  // and, at 90/270, images in native orientation (currentExtent is the rotated, as-seen size).
  if (!dev->transform_set) {
    dev->transform = caps.currentTransform;
    dev->transform_set = true;
  } else if (caps.currentTransform != dev->transform) {
    fprintf(stderr, "error: surface transform changed (0x%x -> 0x%x): pipelines bake view_orientation\n",
            (unsigned)dev->transform, (unsigned)caps.currentTransform);
    return false;
  }
  VkExtent2D view_extent = extent;
  if (dev->transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
    extent = {view_extent.height, view_extent.width};
  }

  VkSwapchainKHR old = sc->swapchain;
  if (extent.width == 0 || extent.height == 0) {
    // minimized: drop the swapchain and retry on a later iteration
    destroy_images(dev, sc);
    if (old) { vkDestroySwapchainKHR(dev->device, old, nullptr); }
    sc->swapchain = VK_NULL_HANDLE;
    sc->needs_recreate = true;
    return true;
  }

  uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, dev->surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, dev->surface, &count, formats.data());
  if (formats.empty()) {
    fprintf(stderr, "error: surface reports no formats\n");
    return false;
  }
  VkSurfaceFormatKHR format = formats[0];
  for (const VkSurfaceFormatKHR& f : formats) {
    if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      format = f;
      break;
    }
  }

  vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, dev->surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, dev->surface, &count, modes.data());
  // no MAILBOX: it discards rendered frames, FIFO keeps every frame (vsync-capped)
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // always supported
  if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) {
    mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  }

  uint32_t image_count = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) { image_count = std::min(image_count, caps.maxImageCount); }

  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & alpha)) {
    for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
      if (caps.supportedCompositeAlpha & bit) {
        alpha = (VkCompositeAlphaFlagBitsKHR)bit;
        break;
      }
    }
  }

  VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = dev->surface;
  ci.minImageCount = image_count;
  ci.imageFormat = format.format;
  ci.imageColorSpace = format.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (sc->offscreen) {
    // offscreen frames reach the swapchain by vkCmdCopyImage
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      fprintf(stderr, "error: --offscreen: swapchain images do not support TRANSFER_DST\n");
      return false;
    }
    ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  if (sc->readable && !sc->offscreen) {
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
      fprintf(stderr, "error: --screenshot: swapchain images do not support TRANSFER_SRC (try --offscreen)\n");
      return false;
    }
    ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = dev->transform;
  ci.compositeAlpha = alpha;
  ci.presentMode = mode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = old;
  r = vkCreateSwapchainKHR(dev->device, &ci, nullptr, &sc->swapchain);
  destroy_images(dev, sc);
  if (old) { vkDestroySwapchainKHR(dev->device, old, nullptr); }
  if (r != VK_SUCCESS) {
    sc->swapchain = VK_NULL_HANDLE;
    fprintf(stderr, "error: vkCreateSwapchainKHR failed (%d)\n", (int)r);
    return false;
  }
  sc->format = format;
  sc->present_mode = mode;
  sc->extent = extent;
  sc->view_extent = view_extent;
  sc->refresh_ns = 0;
  if (dev->display_timing) {
    VkRefreshCycleDurationGOOGLE rc = {};
    if (vkGetRefreshCycleDurationGOOGLE(dev->device, sc->swapchain, &rc) == VK_SUCCESS) {
      sc->refresh_ns = rc.refreshDuration;
    }
  }

  vkGetSwapchainImagesKHR(dev->device, sc->swapchain, &count, nullptr);
  sc->images.resize(count);
  sc->image_states.resize(count);
  vkGetSwapchainImagesKHR(dev->device, sc->swapchain, &count, sc->images.data());
  for (VkImage img : sc->images) {
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    r = vkCreateImageView(dev->device, &vci, nullptr, &view);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateImageView failed (%d)\n", (int)r);
      return false;
    }
    sc->views.push_back(view);
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem = VK_NULL_HANDLE;
    r = vkCreateSemaphore(dev->device, &sci, nullptr, &sem);
    if (r != VK_SUCCESS) {
      fprintf(stderr, "error: vkCreateSemaphore failed (%d)\n", (int)r);
      return false;
    }
    sc->render_finished.push_back(sem);
  }

  if (sc->offscreen &&
      (!texture_create(dev, {extent.width, extent.height, format.format, GpuTextureKind::color}, &sc->offscreen_tex) ||
       !texture_create(dev, {extent.width, extent.height, format.format, GpuTextureKind::color}, &sc->capture))) {
    return false;
  }

  if (!sc->printed) {
    printf("swapchain: present_mode=%s format=%d colorspace=%d images=%u extent=%ux%u refresh=%.3fms offscreen=%d "
           "transform=0x%x view=%ux%u\n",
           present_mode_name(mode), (int)format.format, (int)format.colorSpace, count, extent.width, extent.height,
           sc->refresh_ns / 1e6, sc->offscreen ? 1 : 0, (unsigned)dev->transform, view_extent.width,
           view_extent.height);
    if (mode != VK_PRESENT_MODE_IMMEDIATE_KHR) {
      printf("warning: IMMEDIATE present mode not available, using %s\n", present_mode_name(mode));
    }
    fflush(stdout);
    sc->printed = true;
  }
  return true;
}

void swapchain_destroy(GpuDevice* dev, GpuSwapchain* sc) {
  destroy_images(dev, sc);
  if (sc->swapchain) { vkDestroySwapchainKHR(dev->device, sc->swapchain, nullptr); }
  sc->swapchain = VK_NULL_HANDLE;
  for (GpuFrame& f : sc->frames) { destroy_frame(dev, f); }
  destroy_frame(dev, sc->blit);
  sc->frame = 0;
}

} // namespace vkc

namespace vkc::part {

// Acquires the next image into sc->image signaling `sem`; its tracked state starts as `use` (ACQUIRE_*: UNDEFINED, at
// the wait stage).
static bool acquire(GpuDevice* dev, GpuSwapchain* sc, VkSemaphore sem, uint32_t use) {
  VkResult r;
  {
    ZoneScopedN("acquire");
    r = vkAcquireNextImageKHR(dev->device, sc->swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &sc->image);
  }
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    sc->needs_recreate = true;
    return false;
  }
  if (r == VK_SUBOPTIMAL_KHR) {
    sc->needs_recreate = true; // image acquired and semaphore pending: render and present it, recreate after
  } else if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkAcquireNextImageKHR failed (%d)\n", (int)r);
    sc->needs_recreate = true;
    return false;
  }
  sc->acquired = true;
  // acquired image: contents discarded, ordered after the semaphore wait stage
  sc->image_states[sc->image] = {use};
  return true;
}

bool slot_begin(GpuDevice* dev, GpuSwapchain* sc, GpuFrame& f, uint32_t acquire_use) {
  if (!sc->swapchain || sc->needs_recreate || sc->submit_failed) { return false; }
  {
    ZoneScopedN("frame_fence_wait");
    vkWaitForFences(dev->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
  }
  sc->acquired = false;
  if (acquire_use != vkc::USE_NONE && !acquire(dev, sc, f.image_acquired, acquire_use)) { return false; }
  vkResetFences(dev->device, 1, &f.fence); // only after a successful acquire, so the fence is always submitted
  vkResetCommandPool(dev->device, f.pool, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.cmd, &bi);
  return true;
}

bool present(GpuDevice* dev, GpuSwapchain* sc, VkSemaphore render_finished) {
  VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &render_finished;
  pi.swapchainCount = 1;
  pi.pSwapchains = &sc->swapchain;
  pi.pImageIndices = &sc->image;
  VkResult r;
  {
    ZoneScopedN("present");
    r = vkQueuePresentKHR(dev->queue, &pi);
  }
  if (r == VK_SUBOPTIMAL_KHR) {
    sc->needs_recreate = true;
    return true;
  }
  if (r != VK_SUCCESS) {
    if (r != VK_ERROR_OUT_OF_DATE_KHR) { fprintf(stderr, "error: vkQueuePresentKHR failed (%d)\n", (int)r); }
    sc->needs_recreate = true;
    return false;
  }
  return true;
}

} // namespace vkc::part
