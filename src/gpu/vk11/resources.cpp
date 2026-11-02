#include "gpu/vk11.h"
#include <cstdio>
#include <cstring>
#include "gpu/vk11/caches.h"

namespace vk11 {

void texture_destroy(GpuDevice* dev, GpuTexture* tex) {
  framebuffers_forget(dev, tex->view);
  for (VkImageView v : tex->mip_views) { framebuffers_forget(dev, v); }
  vkc::texture_destroy(dev, tex);
}

bool buffer_close(GpuBuffer* buf, VkCommandBuffer cmd) {
  if (!buf->is_open) {
    fprintf(stderr, "error: buffer close without open\n");
    return false;
  }
  buf->is_open = false;
  if (buf->mode == GpuBufferMode::mapped || buf->hi <= buf->lo) { return true; }
  ensure_state(cmd, buf, {vkc::COPY_DST});
  VkBufferCopy region = {.srcOffset = buf->lo, .dstOffset = buf->lo, .size = buf->hi - buf->lo};
  vkCmdCopyBuffer(cmd, buf->staging[buf->slot], buf->h, 1, &region);
  // end in the read state implied by usage (no barriers are allowed inside a render pass)
  ensure_state(cmd, buf, {vkc::buffer_read_uses(buf->usage)});
  return true;
}

bool texture_upload(GpuDevice* dev, GpuTexture* tex, uint32_t mip, const void* data, VkDeviceSize size,
                    VkCommandBuffer cmd, GpuBuffer* staging) {
  if (mip >= tex->mip_levels) {
    fprintf(stderr, "error: texture upload: mip %u out of range (%u mips)\n", mip, tex->mip_levels);
    return false;
  }
  uint32_t w = tex->extent.width >> mip ? tex->extent.width >> mip : 1;
  uint32_t h = tex->extent.height >> mip ? tex->extent.height >> mip : 1;
  if (size != (VkDeviceSize)w * h * 4) {
    fprintf(stderr, "error: texture upload: %llu bytes, mip %u needs %llu\n", (unsigned long long)size, mip,
            (unsigned long long)w * h * 4);
    return false;
  }
  GpuBuffer st;
  st.size = size;
  st.mode = GpuBufferMode::mapped;
  if (!vkc::create_raw_buffer(dev, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &st.h, &st.alloc, &st.mapped)) {
    return false;
  }
  memcpy(st.mapped, data, size);
  ensure_state(cmd, tex, mip, 1, {vkc::COPY_DST});
  VkBufferImageCopy region = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
      .imageExtent = {w, h, 1},
  };
  vkCmdCopyBufferToImage(cmd, st.h, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  *staging = st;
  return true;
}

bool texture_blit(VkCommandBuffer cmd, GpuTexture* src, uint32_t src_mip, GpuTexture* dst, uint32_t dst_mip,
                  VkFilter filter) {
  if (src_mip >= src->mip_levels || dst_mip >= dst->mip_levels || (src == dst && src_mip == dst_mip)) {
    fprintf(stderr, "error: texture blit: bad mips (src %u of %u, dst %u of %u)\n", src_mip, src->mip_levels, dst_mip,
            dst->mip_levels);
    return false;
  }
  ensure_state(cmd, src, src_mip, 1, {vkc::BLIT_SRC});
  ensure_state(cmd, dst, dst_mip, 1, {vkc::BLIT_DST});
  auto mip_extent = [](const GpuTexture* t, uint32_t mip) {
    return VkOffset3D{(int32_t)(t->extent.width >> mip ? t->extent.width >> mip : 1),
                      (int32_t)(t->extent.height >> mip ? t->extent.height >> mip : 1), 1};
  };
  VkImageBlit region = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src_mip, 0, 1},
      .srcOffsets = {{0, 0, 0}, mip_extent(src, src_mip)},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_mip, 0, 1},
      .dstOffsets = {{0, 0, 0}, mip_extent(dst, dst_mip)},
  };
  vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
  return true;
}

} // namespace vk11
