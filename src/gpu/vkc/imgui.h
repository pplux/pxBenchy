#pragma once
#include <cstdint>
#include <vector>
#include "gpu/vk.h"

struct ImDrawData;
struct ImTextureData;

namespace vkc {

// Inside an open pass, after imgui_prepare on the same command buffer with the same draw data: binds the pipeline and
// the slot's buffers, then one vkCmdDrawIndexed per ImDrawCmd with its texture and clip rect as scissor (rotated by the
// surface transform), and restores the full scissor. ImGui coordinates map DisplaySize onto the pass extent
// (view_extent), whatever io.DisplayFramebufferScale says. User callbacks are not supported (skipped). Fails (prints)
// outside a pass or without a matching imgui_prepare.
bool imgui_draw(GpuImGui* im, GpuPassCtx* ctx, const ImDrawData* dd);
// Device must be idle. With a current ImGui context, marks its textures destroyed and clears the backend fields set by
// imgui_init.
void imgui_shutdown(GpuDevice* dev, GpuImGui* im);

} // namespace vkc

namespace vkc::part {

// ImGui pipeline description for color_format (sources, push layout, ImDrawVert layout, texture binding, alpha
// blend). Fails (prints) if the push layout cannot be built.
bool imgui_pipeline_desc(VkFormat color_format, GpuPipelineDesc* desc);
// After the mode created im->pipe: creates the descriptor pool and registers the renderer in the current ImGui context
// (BackendFlags RendererHasTextures | RendererHasVtxOffset, renderer name / user data). Fails (prints) without a
// current ImGui context or when the pool cannot be created.
bool imgui_init(GpuDevice* dev, GpuImGui* im);
// After the mode recreated im->pipe (device idle): reallocates every texture's descriptor set with the new set layout.
bool imgui_rebind(GpuDevice* dev, GpuImGui* im);

// A texture ImGui wants (re)uploaded: RGBA8 pixels of tex's extent into mip 0.
struct ImGuiUpload {
  ImTextureData* td;
  GpuTexture* tex;
  const void* pixels;
  VkDeviceSize size;
};

// imgui_prepare, split around the uploads. _begin picks im->slot (GPU_IMGUI_BLIT_SLOT for sc->blit.cmd, sc->frame for
// the current frame's cmd), frees the slot's staging buffers and dead textures, honors destroy / create requests and
// appends to *uploads the textures to upload. The mode then records each upload into cmd, keeps its staging buffer in
// im->staging[im->slot] and brings the texture to SAMPLED_FRAGMENT. _end marks the uploaded textures OK and fills the
// slot's vertex / index buffers. On a _begin failure or an upload failure, _end must not be called.
bool imgui_prepare_begin(GpuDevice* dev, GpuSwapchain* sc, GpuImGui* im, VkCommandBuffer cmd, ImDrawData* dd,
                         std::vector<ImGuiUpload>* uploads);
bool imgui_prepare_end(GpuDevice* dev, GpuImGui* im, VkCommandBuffer cmd, ImDrawData* dd,
                       const std::vector<ImGuiUpload>& uploads);

} // namespace vkc::part
