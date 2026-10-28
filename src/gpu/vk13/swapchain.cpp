#include "gpu/vk13.h"
#include "gpu/vk13/sync.h"
#include <cstdio>
#include <tracy/Tracy.hpp>

namespace vk13 {

// Ends f.cmd (image to PRESENT_SRC_KHR when acquired), submits (waiting f.image_acquired at acquire_use's stage when
// acquired) and presents. Returns true if the present succeeded.
static bool end(GpuDevice* dev, GpuSwapchain* sc, GpuFrame& f, uint32_t acquire_use) {
  bool present = sc->acquired;
  sc->acquired = false;
  if (present)
    ensure_state(f.cmd, sc->images[sc->image], VK_IMAGE_ASPECT_COLOR_BIT, &sc->image_states[sc->image],
                 {vkc::PRESENT});
  vkEndCommandBuffer(f.cmd);

  VkSemaphore render_finished = present ? sc->render_finished[sc->image] : VK_NULL_HANDLE;
  VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = f.image_acquired;
  wait.stageMask = sync_of(acquire_use).stage;
  VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = render_finished;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = f.cmd;
  VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.waitSemaphoreInfoCount = present ? 1 : 0;
  si.pWaitSemaphoreInfos = &wait;
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  si.signalSemaphoreInfoCount = present ? 1 : 0;
  si.pSignalSemaphoreInfos = &signal;
  VkResult r;
  {
    ZoneScopedN("submit");
    r = vkQueueSubmit2(dev->queue, 1, &si, f.fence);
  }
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkQueueSubmit2 failed (%d)\n", (int)r);
    sc->submit_failed = true;
    return false;
  }
  if (!present) return false;
  return vkc::part::present(dev, sc, render_finished);
}

bool frame_begin(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer* out_cmd) {
  GpuFrame& f = sc->frames[sc->frame];
  if (!vkc::part::slot_begin(dev, sc, f, sc->offscreen ? vkc::USE_NONE : vkc::ACQUIRE_COLOR)) return false;
  TracyVkCollect(dev->tracy, f.cmd);
  if (sc->acquired)
    ensure_state(f.cmd, sc->images[sc->image], VK_IMAGE_ASPECT_COLOR_BIT, &sc->image_states[sc->image],
                 {vkc::COLOR_ATTACH_WRITE});
  *out_cmd = f.cmd;
  return true;
}

bool frame_end(GpuDevice* dev, GpuSwapchain* sc) {
  GpuFrame& f = sc->frames[sc->frame];
  bool ok = end(dev, sc, f, vkc::ACQUIRE_COLOR);
  if (!sc->submit_failed) sc->frame = (sc->frame + 1) % PXB_FRAMES_IN_FLIGHT;
  return ok;
}

// Full-extent copy, src in TRANSFER_SRC_OPTIMAL and dst in TRANSFER_DST_OPTIMAL.
static void copy_image(GpuSwapchain* sc, VkCommandBuffer cmd, VkImage src, VkImage dst) {
  VkImageCopy region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {sc->extent.width, sc->extent.height, 1};
  vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

static constexpr vkc::State COPY_SRC = {vkc::COPY_SRC};
static constexpr vkc::State COPY_DST = {vkc::COPY_DST};

bool capture(GpuDevice* dev, GpuSwapchain* sc) {
  if (!sc->offscreen || !vkc::part::slot_begin(dev, sc, sc->blit, vkc::USE_NONE)) return false;
  VkCommandBuffer cmd = sc->blit.cmd;
  ensure_state(cmd, &sc->offscreen_tex, COPY_SRC);
  ensure_state(cmd, &sc->capture, COPY_DST);
  copy_image(sc, cmd, sc->offscreen_tex.image, sc->capture.image);
  end(dev, sc, sc->blit, vkc::USE_NONE);
  return !sc->submit_failed;
}

bool blit_begin(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer* out_cmd) {
  if (!sc->offscreen || !vkc::part::slot_begin(dev, sc, sc->blit, vkc::ACQUIRE_COPY)) return false;
  VkCommandBuffer cmd = sc->blit.cmd;
  ensure_state(cmd, &sc->capture, COPY_SRC);
  ensure_state(cmd, sc->images[sc->image], VK_IMAGE_ASPECT_COLOR_BIT, &sc->image_states[sc->image], COPY_DST);
  copy_image(sc, cmd, sc->capture.image, sc->images[sc->image]);
  *out_cmd = cmd;
  return true;
}

bool blit_end(GpuDevice* dev, GpuSwapchain* sc) { return end(dev, sc, sc->blit, vkc::ACQUIRE_COPY); }

} // namespace vk13
