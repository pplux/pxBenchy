#include "gpu/vk11.h"
#include <cstdio>
#include "gpu/vk11/caches.h"

namespace vk11 {

bool pass_begin(GpuDevice* dev, GpuSwapchain* sc, GpuPassCtx* ctx, VkCommandBuffer cmd, const GpuPassDesc& desc) {
  if (ctx->cmd) {
    fprintf(stderr, "error: begin_pass inside a pass\n");
    return false;
  }
  if (desc.depth && (ctx->depth.image == VK_NULL_HANDLE || ctx->depth.extent.width != sc->extent.width ||
                     ctx->depth.extent.height != sc->extent.height)) {
    texture_destroy(dev, &ctx->depth); // vk11's: forgets the framebuffers on its view
    if (!texture_create(dev, {sc->extent.width, sc->extent.height, GPU_PASS_DEPTH_FORMAT, GpuTextureKind::depth},
                        &ctx->depth)) {
      return false;
    }
  }
  bool offscreen = sc->offscreen && !desc.swapchain;
  if (!offscreen && !sc->acquired) {
    fprintf(stderr, "error: begin_pass: no swapchain image acquired\n");
    return false;
  }
  VkImage image = offscreen ? sc->offscreen_tex.image : sc->images[sc->image];
  VkImageView color_view = offscreen ? sc->offscreen_tex.view : sc->views[sc->image];
  VkFormat color_format = offscreen ? sc->offscreen_tex.format : sc->format.format;
  VkRenderPass rp = render_pass(dev, color_format, desc.depth ? GPU_PASS_DEPTH_FORMAT : VK_FORMAT_UNDEFINED,
                                desc.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
  if (rp == VK_NULL_HANDLE) {
    return false; // render_pass printed
  }
  VkFramebuffer fb = framebuffer(dev, rp, color_view, desc.depth ? ctx->depth.view : VK_NULL_HANDLE, sc->extent);
  if (fb == VK_NULL_HANDLE) {
    return false; // framebuffer printed
  }

  vkc::State* state = offscreen ? &sc->offscreen_tex.state[0] : &sc->image_states[sc->image];
  uint32_t color_uses = vkc::COLOR_ATTACH_WRITE;
  if (!desc.clear) {
    color_uses |= vkc::COLOR_ATTACH_READ; // LOAD reads the image
  }
  ensure_state(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, state, {color_uses});
  if (desc.depth) { ensure_state(cmd, &ctx->depth, {vkc::DEPTH_ATTACH}); }

  VkClearValue clear[2] = {};
  clear[0].color = {{desc.clear_color[0], desc.clear_color[1], desc.clear_color[2], desc.clear_color[3]}};
  clear[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = rp;
  bi.framebuffer = fb;
  bi.renderArea = {{0, 0}, sc->extent};
  bi.clearValueCount = desc.depth ? 2 : 1;
  bi.pClearValues = clear;
  vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp = {0, 0, (float)sc->extent.width, (float)sc->extent.height, 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, sc->extent};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  ctx->cmd = cmd;
  ctx->extent = sc->view_extent;
  return true;
}

bool pass_end(GpuPassCtx* ctx) {
  if (!ctx->cmd) {
    fprintf(stderr, "error: end_pass outside a pass\n");
    return false;
  }
  vkCmdEndRenderPass(ctx->cmd);
  ctx->cmd = VK_NULL_HANDLE;
  return true;
}

} // namespace vk11
