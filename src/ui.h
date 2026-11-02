#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gpu/vk.h"
#include "lua/gpu_api.h"
#include "platform/platform.h"

struct ImGuiContext;

// ImGui context (style, fonts) and its renderer on the device. Owned by the session (session.h), outlives GpuLua: the
// context lives from session_init to session_shutdown (gpu_ui_create / gpu_ui_shutdown), the renderer as long as the
// device.
struct GpuUi {
  ImGuiContext* ctx = nullptr;
  GpuImGui im;
  int64_t last_ns = 0;                       // wall time of the last ImGui frame (io.DeltaTime)
  std::vector<PlatformPointerEvent> pointer; // scratch for gpu_ui_input
};

// File name part of path (after the last / or \; all of it when none). Points into path.
const char* path_base_name(const std::string& path);

// Before ImGui::NewFrame: feeds the queued platform pointer events (platform_pointer_events) to the current context.
void gpu_ui_input(GpuUi* ui);

// Creates the context and makes it current: no ini / log file, scalable default font, HUD style. Once per session
// (session_init), before the device.
void gpu_ui_create(GpuUi* ui);
// After swapchain create: makes the context current and registers the renderer (imgui_init) for color_format. On
// failure the renderer is left clean (as after gpu_ui_renderer_shutdown), so a later call may retry.
bool gpu_ui_init(GpuUi* ui, const GpuApiTable* gpu, GpuDevice* dev, VkFormat color_format);
// Starts an ImGui frame for view_extent: DisplaySize, DeltaTime since the last frame (1/60 s for the first), then
// ImGui::NewFrame.
void gpu_ui_new_frame(GpuUi* ui, VkExtent2D view_extent);
// Device idle: shuts the renderer down (its textures are marked destroyed; ImGui re-requests them from the next
// renderer) and resets it. The context survives: call before the device goes (backend / validation switch).
void gpu_ui_renderer_shutdown(GpuUi* ui, const GpuApiTable* gpu, GpuDevice* dev);
// After gpu_ui_renderer_shutdown: destroys the context.
void gpu_ui_shutdown(GpuUi* ui);
