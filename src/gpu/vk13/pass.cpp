#include "gpu/vk13.h"
#include <cstdio>

namespace vk13 {

bool pass_begin(GpuDevice* dev, GpuSwapchain* sc, GpuPassCtx* ctx, VkCommandBuffer cmd, const GpuPassDesc& desc) {
  if (ctx->cmd) {
    fprintf(stderr, "error: begin_pass inside a pass\n");
    return false;
  }
  if (desc.depth && (ctx->depth.image == VK_NULL_HANDLE || ctx->depth.extent.width != sc->extent.width ||
                     ctx->depth.extent.height != sc->extent.height)) {
    texture_destroy(dev, &ctx->depth);
    if (!texture_create(dev, {sc->extent.width, sc->extent.height, GPU_PASS_DEPTH_FORMAT, GpuTextureKind::depth},
                        &ctx->depth))
      return false;
  }
  bool offscreen = sc->offscreen && !desc.swapchain;
  if (!offscreen && !sc->acquired) {
    fprintf(stderr, "error: begin_pass: no swapchain image acquired\n");
    return false;
  }
  VkImage image = offscreen ? sc->offscreen_tex.image : sc->images[sc->image];
  vkc::State* state = offscreen ? &sc->offscreen_tex.state[0] : &sc->image_states[sc->image];
  uint32_t color_uses = vkc::COLOR_ATTACH_WRITE;
  if (!desc.clear) color_uses |= vkc::COLOR_ATTACH_READ; // LOAD reads the image
  ensure_state(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, state, {color_uses});

  VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = offscreen ? sc->offscreen_tex.view : sc->views[sc->image];
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = desc.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color = {{desc.clear_color[0], desc.clear_color[1], desc.clear_color[2], desc.clear_color[3]}};

  VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (desc.depth) {
    ensure_state(cmd, &ctx->depth, {vkc::DEPTH_ATTACH});
    depth.imageView = ctx->depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};
  }

  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, sc->extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  ri.pDepthAttachment = desc.depth ? &depth : nullptr;
  vkCmdBeginRendering(cmd, &ri);

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
  vkCmdEndRendering(ctx->cmd);
  ctx->cmd = VK_NULL_HANDLE;
  return true;
}

} // namespace vk13
