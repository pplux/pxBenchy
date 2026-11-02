#include "gpu/vkc/imgui.h"
#include "gpu/vkc/descriptors.h"
#include "gpu/vkc/pipeline.h"
#include "gpu/vkc/resources.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <imgui.h>

namespace vkc {

static const char* const IMGUI_VS = R"(
layout(location=0) in vec2 pos;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(location=0) out vec2 v_uv;
layout(location=1) out vec4 v_color;
void main() {
  v_uv = uv;
  v_color = color;
  gl_Position = view_orientation(vec4(pos * push.scale + push.translate, 0.0, 1.0));
})";

static const char* const IMGUI_FS = R"(
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(location=0) out vec4 col;
void main() {
  col = v_color * texture(tex, v_uv);
})";

static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert must be float2 pos, float2 uv, u8x4 color");

static constexpr VkIndexType IMGUI_INDEX_TYPE = sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

static void bind_state(GpuImGui* im, VkCommandBuffer cmd, const void* push) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, im->pipe.h);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &im->vertex[im->slot].h, &offset);
  vkCmdBindIndexBuffer(cmd, im->index[im->slot].h, 0, IMGUI_INDEX_TYPE);
  vkCmdPushConstants(cmd, im->pipe.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     im->pipe.push.size, push);
}

// View-space pixel rect [x0, x1) x [y0, y1) inside a w x h view -> image-space scissor, inverse of view_orientation
// (same mapping as screenshot_write).
static VkRect2D rotate_rect(VkSurfaceTransformFlagBitsKHR t, float w, float h, float x0, float y0, float x1, float y1) {
  float r[4] = {x0, y0, x1, y1};
  if (t == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
    r[0] = h - y1, r[1] = x0, r[2] = h - y0, r[3] = x1;
  } else if (t == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
    r[0] = w - x1, r[1] = h - y1, r[2] = w - x0, r[3] = h - y0;
  } else if (t == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
    r[0] = y0, r[1] = w - x1, r[2] = y1, r[3] = w - x0;
  }
  int32_t ix0 = (int32_t)r[0], iy0 = (int32_t)r[1];
  return {{ix0, iy0}, {(uint32_t)((int32_t)r[2] - ix0), (uint32_t)((int32_t)r[3] - iy0)}};
}

bool imgui_draw(GpuImGui* im, GpuPassCtx* ctx, const ImDrawData* dd) {
  if (!ctx->cmd) {
    fprintf(stderr, "error: imgui draw outside a pass\n");
    return false;
  }
  if (!im->pipe.h) { return false; }
  if (ctx->cmd != im->prepared) {
    fprintf(stderr, "error: imgui draw without imgui_prepare on this command buffer\n");
    return false;
  }
  im->prepared = VK_NULL_HANDLE; // one draw per prepare: the next prepare may rewrite the buffers
  if (!dd || dd->TotalVtxCount <= 0 || dd->DisplaySize.x <= 0.0f || dd->DisplaySize.y <= 0.0f) { return true; }

  float w = (float)ctx->extent.width, h = (float)ctx->extent.height;
  float scale[2] = {2.0f / dd->DisplaySize.x, 2.0f / dd->DisplaySize.y};
  float translate[2] = {-1.0f - dd->DisplayPos.x * scale[0], -1.0f - dd->DisplayPos.y * scale[1]};
  uint8_t push[128];
  std::string err;
  const GpuLayout& l = im->pipe.push;
  if (!layout_pack(l, "scale", scale, 2, push, &err) || !layout_pack(l, "translate", translate, 2, push, &err)) {
    fprintf(stderr, "error: imgui: %s\n", err.c_str());
    return false;
  }
  // ImGui units -> view pixels (the pass extent, not io.DisplayFramebufferScale)
  float sx = w / dd->DisplaySize.x, sy = h / dd->DisplaySize.y;
  VkCommandBuffer cmd = ctx->cmd;
  bind_state(im, cmd, push);
  const GpuImGuiTexture* bound = nullptr;
  uint32_t vtx_base = 0, idx_base = 0;
  for (const ImDrawList* list : dd->CmdLists) {
    for (const ImDrawCmd& c : list->CmdBuffer) {
      if (c.UserCallback) {
        continue; // not supported (the HUD and menu do not use callbacks)
      }
      float x0 = std::max((c.ClipRect.x - dd->DisplayPos.x) * sx, 0.0f);
      float y0 = std::max((c.ClipRect.y - dd->DisplayPos.y) * sy, 0.0f);
      float x1 = std::min((c.ClipRect.z - dd->DisplayPos.x) * sx, w);
      float y1 = std::min((c.ClipRect.w - dd->DisplayPos.y) * sy, h);
      if (x1 <= x0 || y1 <= y0 || c.ElemCount == 0) { continue; }
      GpuImGuiTexture* t = (GpuImGuiTexture*)(uintptr_t)c.GetTexID();
      if (!t) { continue; }
      if (t != bound) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, im->pipe.layout, 0, 1, &t->set, 0, nullptr);
        bound = t;
      }
      t->used |= 1u << im->slot;
      VkRect2D scissor = rotate_rect(im->transform, w, h, x0, y0, x1, y1);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdDrawIndexed(cmd, c.ElemCount, 1, idx_base + c.IdxOffset, (int32_t)(vtx_base + c.VtxOffset), 0);
    }
    vtx_base += (uint32_t)list->VtxBuffer.Size;
    idx_base += (uint32_t)list->IdxBuffer.Size;
  }
  VkRect2D full = rotate_rect(im->transform, w, h, 0.0f, 0.0f, w, h);
  vkCmdSetScissor(cmd, 0, 1, &full);
  return true;
}

static bool alloc_set(GpuDevice* dev, GpuImGui* im, GpuImGuiTexture* t) {
  VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = im->pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &im->pipe.set_layout,
  };
  VkResult r = vkAllocateDescriptorSets(dev->device, &ai, &t->set);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: imgui: vkAllocateDescriptorSets failed (%d), more than %u textures?\n", (int)r,
            GPU_IMGUI_MAX_TEXTURES);
    t->set = VK_NULL_HANDLE;
    return false;
  }
  descriptors_write(dev, t->set, im->pipe.bindings, &t->tex.view);
  return true;
}

// vkc::texture_destroy, not vk11's: ImGui textures are only sampled, never pass attachments, so no framebuffer cache
// entry references their views.
static void free_texture(GpuDevice* dev, GpuImGui* im, GpuImGuiTexture* t) {
  if (t->set) { vkFreeDescriptorSets(dev->device, im->pool, 1, &t->set); }
  texture_destroy(dev, &t->tex);
}

// Frees dead textures no slot may still sample.
static void free_dead(GpuDevice* dev, GpuImGui* im) {
  size_t keep = 0;
  for (std::unique_ptr<GpuImGuiTexture>& t : im->textures) {
    if (t->dead && !t->used) {
      free_texture(dev, im, t.get());
    } else {
      im->textures[keep++] = std::move(t);
    }
  }
  im->textures.resize(keep);
}

void imgui_shutdown(GpuDevice* dev, GpuImGui* im) {
  if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData == im) {
    for (ImTextureData* td : ImGui::GetPlatformIO().Textures) {
      if (!td->BackendUserData) { continue; }
      td->SetTexID(ImTextureID_Invalid);
      td->BackendUserData = nullptr;
      td->SetStatus(ImTextureStatus_Destroyed);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
  }
  for (std::unique_ptr<GpuImGuiTexture>& t : im->textures) { free_texture(dev, im, t.get()); }
  im->textures.clear();
  for (uint32_t i = 0; i < GPU_IMGUI_SLOTS; i++) {
    buffer_destroy(dev, &im->vertex[i]);
    buffer_destroy(dev, &im->index[i]);
    for (GpuBuffer& b : im->staging[i]) { buffer_destroy(dev, &b); }
    im->staging[i].clear();
  }
  if (im->pool) { vkDestroyDescriptorPool(dev->device, im->pool, nullptr); }
  im->pool = VK_NULL_HANDLE;
  pipeline_destroy(dev, &im->pipe);
  im->format = VK_FORMAT_UNDEFINED;
  im->prepared = VK_NULL_HANDLE;
}

} // namespace vkc

namespace vkc::part {

bool imgui_pipeline_desc(VkFormat color_format, GpuPipelineDesc* desc) {
  std::string err;
  if (!layout_build("push", {{"scale", "vec2"}, {"translate", "vec2"}}, &desc->push, &err)) {
    fprintf(stderr, "error: imgui layout: %s\n", err.c_str());
    return false;
  }
  desc->vs = IMGUI_VS;
  desc->fs = IMGUI_FS;
  desc->vs_name = "imgui.vs";
  desc->fs_name = "imgui.fs";
  desc->vertex_layout = {GpuVertexFormat::float2, GpuVertexFormat::float2, GpuVertexFormat::unorm4}; // ImDrawVert
  desc->bindings = {{"tex", GpuBindingKind::texture}};
  desc->blend = GpuBlend::alpha;
  desc->color_format = color_format;
  return true;
}

bool imgui_init(GpuDevice* dev, GpuImGui* im) {
  if (!ImGui::GetCurrentContext()) {
    fprintf(stderr, "error: imgui init: no current ImGui context\n");
    return false;
  }
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, GPU_IMGUI_MAX_TEXTURES};
  VkDescriptorPoolCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = GPU_IMGUI_MAX_TEXTURES,
      .poolSizeCount = 1,
      .pPoolSizes = &size,
  };
  VkResult r = vkCreateDescriptorPool(dev->device, &ci, nullptr, &im->pool);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: imgui: vkCreateDescriptorPool failed (%d)\n", (int)r);
    im->pool = VK_NULL_HANDLE;
    return false;
  }
  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = "pxbenchy_gpu";
  io.BackendRendererUserData = im;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  pio.Renderer_TextureMaxWidth = pio.Renderer_TextureMaxHeight = (int)dev->props.limits.maxImageDimension2D;
  io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
  return true;
}

bool imgui_rebind(GpuDevice* dev, GpuImGui* im) {
  for (std::unique_ptr<GpuImGuiTexture>& t : im->textures) {
    t->used = 0; // device idle
  }
  free_dead(dev, im);
  for (std::unique_ptr<GpuImGuiTexture>& t : im->textures) {
    if (t->set) { vkFreeDescriptorSets(dev->device, im->pool, 1, &t->set); }
    t->set = VK_NULL_HANDLE;
    if (!alloc_set(dev, im, t.get())) { return false; }
  }
  return true;
}

// Honors one ImTextureData request; creates and updates are appended to *uploads for the mode to record.
static bool update_texture(GpuDevice* dev, GpuImGui* im, ImTextureData* td, std::vector<ImGuiUpload>* uploads) {
  GpuImGuiTexture* t = (GpuImGuiTexture*)td->BackendUserData;
  if (td->Status == ImTextureStatus_WantDestroy) {
    if (t) {
      t->dead = true; // freed by free_dead once no slot may sample it
    }
    td->SetTexID(ImTextureID_Invalid);
    td->BackendUserData = nullptr;
    td->SetStatus(ImTextureStatus_Destroyed);
    return true;
  }
  if (td->Format != ImTextureFormat_RGBA32) {
    fprintf(stderr, "error: imgui: texture format %d not supported (RGBA32 only)\n", (int)td->Format);
    return false;
  }
  if (td->Status == ImTextureStatus_WantCreate) {
    if (t) { t->dead = true; }
    std::unique_ptr<GpuImGuiTexture> owned = std::make_unique<GpuImGuiTexture>();
    t = owned.get();
    if (!texture_create(dev, {(uint32_t)td->Width, (uint32_t)td->Height, VK_FORMAT_R8G8B8A8_UNORM}, &t->tex)) {
      return false;
    }
    if (!alloc_set(dev, im, t)) {
      texture_destroy(dev, &t->tex);
      return false;
    }
    im->textures.push_back(std::move(owned));
    td->SetTexID((ImTextureID)(uintptr_t)t);
    td->BackendUserData = t;
  }
  if (!t) {
    fprintf(stderr, "error: imgui: texture update before create\n");
    return false;
  }
  // WantUpdates: the whole texture is uploaded again (the atlas is small and rarely changes). Pixels sampled by
  // earlier submits are protected by the SAMPLED -> COPY_DST barrier (same queue, submission order).
  uploads->push_back({td, &t->tex, td->Pixels, (VkDeviceSize)td->GetSizeInBytes()});
  return true;
}

// Mapped buffer of at least size bytes. The old buffer is only used by the slot's earlier submits (fence waited).
static bool ensure_size(GpuDevice* dev, GpuBuffer* buf, uint32_t usage, VkDeviceSize size) {
  if (buf->h && buf->size >= size) { return true; }
  buffer_destroy(dev, buf);
  VkDeviceSize n = 16384;
  while (n < size) { n *= 2; }
  return buffer_create(dev, {n, usage, GpuBufferMode::mapped}, buf);
}

bool imgui_prepare_begin(GpuDevice* dev, GpuSwapchain* sc, GpuImGui* im, VkCommandBuffer cmd, ImDrawData* dd,
                         std::vector<ImGuiUpload>* uploads) {
  if (!im->pipe.h) { return false; }
  uint32_t slot;
  if (cmd == sc->frames[sc->frame].cmd) {
    slot = sc->frame;
  } else if (cmd == sc->blit.cmd) {
    slot = GPU_IMGUI_BLIT_SLOT;
  } else {
    fprintf(stderr, "error: imgui prepare: command buffer is neither the open frame's nor the blit's\n");
    return false;
  }
  im->prepared = VK_NULL_HANDLE;
  // frame_begin / blit_begin waited the slot's fence: its earlier uploads and draws have executed
  for (GpuBuffer& b : im->staging[slot]) { buffer_destroy(dev, &b); }
  im->staging[slot].clear();
  for (std::unique_ptr<GpuImGuiTexture>& t : im->textures) { t->used &= ~(1u << slot); }
  free_dead(dev, im);
  im->slot = slot;
  if (!dd) { return false; }
  if (dd->Textures) {
    for (ImTextureData* td : *dd->Textures) {
      if (td->Status != ImTextureStatus_OK && !update_texture(dev, im, td, uploads)) { return false; }
    }
  }
  return true;
}

bool imgui_prepare_end(GpuDevice* dev, GpuImGui* im, VkCommandBuffer cmd, ImDrawData* dd,
                       const std::vector<ImGuiUpload>& uploads) {
  for (const ImGuiUpload& u : uploads) { u.td->SetStatus(ImTextureStatus_OK); }
  uint32_t slot = im->slot;
  if (dd->TotalVtxCount > 0) {
    if (!ensure_size(dev, &im->vertex[slot], GPU_BUF_VERTEX, (VkDeviceSize)dd->TotalVtxCount * sizeof(ImDrawVert)) ||
        !ensure_size(dev, &im->index[slot], GPU_BUF_INDEX, (VkDeviceSize)dd->TotalIdxCount * sizeof(ImDrawIdx))) {
      return false;
    }
    uint8_t* vtx = (uint8_t*)im->vertex[slot].mapped;
    uint8_t* idx = (uint8_t*)im->index[slot].mapped;
    for (const ImDrawList* list : dd->CmdLists) {
      size_t vn = (size_t)list->VtxBuffer.Size * sizeof(ImDrawVert);
      size_t in = (size_t)list->IdxBuffer.Size * sizeof(ImDrawIdx);
      memcpy(vtx, list->VtxBuffer.Data, vn);
      memcpy(idx, list->IdxBuffer.Data, in);
      vtx += vn;
      idx += in;
    }
  }
  im->prepared = cmd;
  return true;
}

} // namespace vkc::part
