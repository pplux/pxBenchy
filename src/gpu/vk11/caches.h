#pragma once
#include <vector>
#include <volk.h>
#include "gpu/vk.h"

namespace vk11 {

// Render passes and framebuffers, created on first use and kept until device_shutdown (GpuDevice::mode).
// Few entries (swapchain images x load ops + offscreen + targets): vectors, linear search.
struct RenderPassEntry {
  VkFormat color = VK_FORMAT_UNDEFINED;
  VkFormat depth = VK_FORMAT_UNDEFINED; // UNDEFINED = no depth attachment
  VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
  VkRenderPass h = VK_NULL_HANDLE;
};

struct FramebufferEntry {
  VkRenderPass pass = VK_NULL_HANDLE;
  VkImageView color = VK_NULL_HANDLE;
  VkImageView depth = VK_NULL_HANDLE; // VK_NULL_HANDLE = no depth attachment
  VkExtent2D extent = {};
  VkFramebuffer h = VK_NULL_HANDLE;
};

struct Caches : GpuModeData {
  std::vector<RenderPassEntry> passes;
  std::vector<FramebufferEntry> framebuffers;
};

// One subpass, one color attachment (load = `load` (CLEAR | LOAD), store STORE) and, when depth is not UNDEFINED, a
// depth attachment (load CLEAR, store DONT_CARE, stencil DONT_CARE). initialLayout = finalLayout = subpass layout
// (COLOR_ATTACHMENT_OPTIMAL, DEPTH_STENCIL_ATTACHMENT_OPTIMAL): no transitions, ensure_state does them before the pass.
// Implicit external dependencies, 1 sample. VK_NULL_HANDLE (prints) on failure.
VkRenderPass render_pass(GpuDevice* dev, VkFormat color, VkFormat depth, VkAttachmentLoadOp load);
// For pipeline creation: compatibility ignores load/store ops and layouts, so this is the (color, depth, CLEAR) pass.
VkRenderPass compat_render_pass(GpuDevice* dev, VkFormat color, VkFormat depth);
// Framebuffer of pass over color (+ depth unless VK_NULL_HANDLE), 1 layer. VK_NULL_HANDLE (prints) on failure.
VkFramebuffer framebuffer(GpuDevice* dev, VkRenderPass pass, VkImageView color, VkImageView depth, VkExtent2D extent);
// Destroys every cached framebuffer referencing view; call before destroying the view. Device must be idle.
void framebuffers_forget(GpuDevice* dev, VkImageView view);
// Destroys all framebuffers and render passes and frees dev->mode. Safe when null.
void caches_destroy(GpuDevice* dev);

} // namespace vk11
