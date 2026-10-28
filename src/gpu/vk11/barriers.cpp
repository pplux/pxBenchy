#include "gpu/vk11.h"
#include "gpu/vk11/sync.h"
#include "gpu/vkc/barriers.h"

namespace vk11 {

// sync1 stage masks must not be 0: nothing to wait on is TOP_OF_PIPE, nothing waiting is BOTTOM_OF_PIPE.
static VkPipelineStageFlags src_stages(VkPipelineStageFlags s) {
  return s ? s : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
}

static VkPipelineStageFlags dst_stages(VkPipelineStageFlags s) {
  return s ? s : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}

void ensure_state(VkCommandBuffer cmd, GpuBuffer* buf, vkc::State want) {
  vkc::State from;
  if (!vkc::part::track(&buf->state, want, false, &from)) return;
  Sync src = sync_of(from.uses), dst = sync_of(want.uses);
  VkBufferMemoryBarrier b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  b.srcAccessMask = src.access;
  b.dstAccessMask = dst.access;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = buf->h;
  b.offset = 0;
  b.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, src_stages(src.stage), dst_stages(dst.stage), 0, 0, nullptr, 1, &b, 0, nullptr);
}

void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, vkc::State want) {
  ensure_state(cmd, tex, 0, tex->mip_levels, want);
}

void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, uint32_t first_mip, uint32_t mip_count, vkc::State want) {
  vkc::part::MipTransition ts[PXB_MAX_MIPS];
  uint32_t n = vkc::part::track_mips(tex->state, first_mip, mip_count, want, ts);
  if (!n) return;
  VkImageAspectFlags aspect =
      tex->kind == GpuTextureKind::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageMemoryBarrier bs[PXB_MAX_MIPS];
  Sync dst = sync_of(want.uses);
  VkPipelineStageFlags src_stage = 0;
  for (uint32_t i = 0; i < n; i++) {
    Sync src = sync_of(ts[i].from.uses);
    src_stage |= src.stage;
    VkImageMemoryBarrier& b = bs[i];
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src.access;
    b.dstAccessMask = dst.access;
    b.oldLayout = src.layout;
    b.newLayout = dst.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->image;
    b.subresourceRange = {aspect, ts[i].first_mip, ts[i].mip_count, 0, 1};
  }
  vkCmdPipelineBarrier(cmd, src_stages(src_stage), dst_stages(dst.stage), 0, 0, nullptr, 0, nullptr, n, bs);
}

void ensure_state(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, vkc::State* tracked, vkc::State want) {
  vkc::State from;
  if (!vkc::part::track(tracked, want, true, &from)) return;
  Sync src = sync_of(from.uses), dst = sync_of(want.uses);
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src.access;
  b.dstAccessMask = dst.access;
  b.oldLayout = src.layout;
  b.newLayout = dst.layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, src_stages(src.stage), dst_stages(dst.stage), 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace vk11
