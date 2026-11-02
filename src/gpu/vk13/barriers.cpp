#include "gpu/vk13.h"
#include "gpu/vk13/sync.h"
#include "gpu/vkc/barriers.h"

namespace vk13 {

void ensure_state(VkCommandBuffer cmd, GpuBuffer* buf, vkc::State want) {
  vkc::State from;
  if (!vkc::part::track(&buf->state, want, false, &from)) { return; }
  Sync src = sync_of(from.uses), dst = sync_of(want.uses);
  VkBufferMemoryBarrier2 b = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
  b.srcStageMask = src.stage;
  b.srcAccessMask = src.access;
  b.dstStageMask = dst.stage;
  b.dstAccessMask = dst.access;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.buffer = buf->h;
  b.offset = 0;
  b.size = VK_WHOLE_SIZE;
  VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.bufferMemoryBarrierCount = 1;
  di.pBufferMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &di);
}

void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, vkc::State want) {
  ensure_state(cmd, tex, 0, tex->mip_levels, want);
}

void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, uint32_t first_mip, uint32_t mip_count, vkc::State want) {
  vkc::part::MipTransition ts[PXB_MAX_MIPS];
  uint32_t n = vkc::part::track_mips(tex->state, first_mip, mip_count, want, ts);
  if (!n) { return; }
  VkImageAspectFlags aspect =
      tex->kind == GpuTextureKind::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageMemoryBarrier2 bs[PXB_MAX_MIPS];
  Sync dst = sync_of(want.uses);
  for (uint32_t i = 0; i < n; i++) {
    Sync src = sync_of(ts[i].from.uses);
    VkImageMemoryBarrier2& b = bs[i];
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src.stage;
    b.srcAccessMask = src.access;
    b.dstStageMask = dst.stage;
    b.dstAccessMask = dst.access;
    b.oldLayout = src.layout;
    b.newLayout = dst.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->image;
    b.subresourceRange = {aspect, ts[i].first_mip, ts[i].mip_count, 0, 1};
  }
  VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = n;
  di.pImageMemoryBarriers = bs;
  vkCmdPipelineBarrier2(cmd, &di);
}

void ensure_state(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, vkc::State* tracked, vkc::State want) {
  vkc::State from;
  if (!vkc::part::track(tracked, want, true, &from)) { return; }
  Sync src = sync_of(from.uses), dst = sync_of(want.uses);
  VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = src.stage;
  b.srcAccessMask = src.access;
  b.dstStageMask = dst.stage;
  b.dstAccessMask = dst.access;
  b.oldLayout = src.layout;
  b.newLayout = dst.layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, 1, 0, 1};
  VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &di);
}

} // namespace vk13
