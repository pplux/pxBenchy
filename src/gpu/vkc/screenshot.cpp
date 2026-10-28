#include "gpu/vkc/screenshot.h"
#include <cstdio>
#include <vector>
#include <stb_image_write.h>

namespace vkc {

static bool is_bgra(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM; }
static bool is_rgba(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM; }

bool screenshot_write(GpuDevice* dev, const GpuSwapchain* sc, GpuScreenshot* shot, const char* path) {
  vmaInvalidateAllocation(dev->vma, shot->alloc, 0, VK_WHOLE_SIZE);
  const uint8_t* px = (const uint8_t*)shot->mapped;
  uint32_t iw = sc->extent.width, w = sc->view_extent.width, h = sc->view_extent.height;
  bool bgra = is_bgra(sc->format.format);
  std::vector<uint8_t> out((size_t)w * h * 4);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      // view pixel -> image pixel, inverse of view_orientation (pipeline.cpp)
      uint32_t ix = x, iy = y;
      if (dev->transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        ix = h - 1 - y;
        iy = x;
      } else if (dev->transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        ix = w - 1 - x;
        iy = h - 1 - y;
      } else if (dev->transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        ix = y;
        iy = w - 1 - x;
      }
      const uint8_t* s = px + ((size_t)iy * iw + ix) * 4;
      uint8_t* d = &out[((size_t)y * w + x) * 4];
      d[0] = bgra ? s[2] : s[0];
      d[1] = s[1];
      d[2] = bgra ? s[0] : s[2];
      d[3] = 255;
    }
  }
  screenshot_destroy(dev, shot);
  if (!stbi_write_png(path, (int)w, (int)h, 4, out.data(), (int)w * 4)) {
    fprintf(stderr, "error: --screenshot: cannot write '%s'\n", path);
    return false;
  }
  return true;
}

void screenshot_destroy(GpuDevice* dev, GpuScreenshot* shot) {
  if (shot->buf) vmaDestroyBuffer(dev->vma, shot->buf, shot->alloc);
  *shot = GpuScreenshot{};
}

} // namespace vkc

namespace vkc::part {

bool screenshot_alloc(GpuDevice* dev, const GpuSwapchain* sc, GpuScreenshot* shot) {
  if (!is_bgra(sc->format.format) && !is_rgba(sc->format.format)) {
    fprintf(stderr, "error: --screenshot: unsupported swapchain format %d\n", (int)sc->format.format);
    return false;
  }
  VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = (VkDeviceSize)sc->extent.width * sc->extent.height * 4;
  ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo ai = {};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo info;
  VkResult r = vmaCreateBuffer(dev->vma, &ci, &ai, &shot->buf, &shot->alloc, &info);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: --screenshot: vmaCreateBuffer failed (%d)\n", (int)r);
    *shot = GpuScreenshot{};
    return false;
  }
  shot->mapped = info.pMappedData;
  return true;
}

} // namespace vkc::part
