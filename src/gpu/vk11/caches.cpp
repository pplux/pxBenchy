#include "gpu/vk11/caches.h"
#include <cstdio>

namespace vk11 {

static VkRenderPass create_render_pass(GpuDevice* dev, VkFormat color, VkFormat depth, VkAttachmentLoadOp load) {
  VkAttachmentDescription atts[2] = {};
  atts[0].format = color;
  atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[0].loadOp = load;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  atts[1].format = depth;
  atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  bool has_depth = depth != VK_FORMAT_UNDEFINED;

  VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;
  sub.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

  VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  ci.attachmentCount = has_depth ? 2 : 1;
  ci.pAttachments = atts;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  VkRenderPass h = VK_NULL_HANDLE;
  VkResult r = vkCreateRenderPass(dev->device, &ci, nullptr, &h);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateRenderPass failed (%d)\n", (int)r);
    return VK_NULL_HANDLE;
  }
  return h;
}

VkRenderPass render_pass(GpuDevice* dev, VkFormat color, VkFormat depth, VkAttachmentLoadOp load) {
  Caches* c = dev->vk11_caches;
  for (const RenderPassEntry& e : c->passes)
    if (e.color == color && e.depth == depth && e.load == load) return e.h;
  VkRenderPass h = create_render_pass(dev, color, depth, load);
  if (h) c->passes.push_back({color, depth, load, h});
  return h;
}

VkRenderPass compat_render_pass(GpuDevice* dev, VkFormat color, VkFormat depth) {
  return render_pass(dev, color, depth, VK_ATTACHMENT_LOAD_OP_CLEAR);
}

VkFramebuffer framebuffer(GpuDevice* dev, VkRenderPass pass, VkImageView color, VkImageView depth, VkExtent2D extent) {
  Caches* c = dev->vk11_caches;
  for (const FramebufferEntry& e : c->framebuffers)
    if (e.pass == pass && e.color == color && e.depth == depth && e.extent.width == extent.width &&
        e.extent.height == extent.height)
      return e.h;
  VkImageView views[2] = {color, depth};
  VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  ci.renderPass = pass;
  ci.attachmentCount = depth ? 2 : 1;
  ci.pAttachments = views;
  ci.width = extent.width;
  ci.height = extent.height;
  ci.layers = 1;
  VkFramebuffer h = VK_NULL_HANDLE;
  VkResult r = vkCreateFramebuffer(dev->device, &ci, nullptr, &h);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateFramebuffer failed (%d)\n", (int)r);
    return VK_NULL_HANDLE;
  }
  c->framebuffers.push_back({pass, color, depth, extent, h});
  return h;
}

void framebuffers_forget(GpuDevice* dev, VkImageView view) {
  Caches* c = dev->vk11_caches;
  if (!c || !view) return;
  for (size_t i = 0; i < c->framebuffers.size();) {
    FramebufferEntry& e = c->framebuffers[i];
    if (e.color == view || e.depth == view) {
      vkDestroyFramebuffer(dev->device, e.h, nullptr);
      e = c->framebuffers.back();
      c->framebuffers.pop_back();
    } else {
      i++;
    }
  }
}

void caches_destroy(GpuDevice* dev) {
  Caches* c = dev->vk11_caches;
  if (!c) return;
  for (const FramebufferEntry& e : c->framebuffers) vkDestroyFramebuffer(dev->device, e.h, nullptr);
  for (const RenderPassEntry& e : c->passes) vkDestroyRenderPass(dev->device, e.h, nullptr);
  delete c;
  dev->vk11_caches = nullptr;
}

} // namespace vk11
