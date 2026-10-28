#include "gpu/vk11.h"
#include "gpu/vk11/sync.h"

namespace vk11 {

bool screenshot_record(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer cmd, GpuScreenshot* shot) {
  if (!vkc::part::screenshot_alloc(dev, sc, shot)) return false;

  const vkc::State src = {vkc::COPY_SRC};
  VkImage image;
  if (sc->offscreen) {
    image = sc->offscreen_tex.image;
    ensure_state(cmd, &sc->offscreen_tex, src);
  } else {
    image = sc->images[sc->image];
    ensure_state(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, &sc->image_states[sc->image], src);
  }
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {sc->extent.width, sc->extent.height, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot->buf, 1, &region);
  // copy -> host read (both stage masks non-zero: TRANSFER, HOST)
  Sync copy = sync_of(vkc::COPY_DST), host = sync_of(vkc::HOST_READ);
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = copy.access;
  mb.dstAccessMask = host.access;
  vkCmdPipelineBarrier(cmd, copy.stage, host.stage, 0, 1, &mb, 0, nullptr, 0, nullptr);
  return true;
}

} // namespace vk11
