#include "ui.h"
#include <cfloat>
#include <cstdio>
#include <imgui.h>
#include "lua/timers_report.h"

const char* path_base_name(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

void gpu_ui_create(GpuUi* ui) {
  ui->ctx = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.Fonts->AddFontDefaultVector(); // scalable: drawn at the HUD size, not a scaled 13 px bitmap
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowBorderSize = 0.0f;
  s.WindowRounding = 0.0f;
  s.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.92f);
  s.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
  s.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
}

bool gpu_ui_init(GpuUi* ui, const GpuApiTable* gpu, GpuDevice* dev, VkFormat color_format) {
  ImGui::SetCurrentContext(ui->ctx);
  if (!gpu->imgui_init(dev, &ui->im, color_format)) { // already shut down what it created
    fprintf(stderr, "error: gpu_imgui_init failed\n");
    ui->im = GpuImGui{};
    return false;
  }
  return true;
}

void gpu_ui_input(GpuUi* ui) {
  ImGuiIO& io = ImGui::GetIO();
  platform_pointer_events(&ui->pointer);
  for (const PlatformPointerEvent& e : ui->pointer) {
    io.AddMouseSourceEvent(e.touch ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
    switch (e.kind) {
      case PlatformPointerEvent::pos: io.AddMousePosEvent(e.x, e.y); break;
      case PlatformPointerEvent::button: io.AddMouseButtonEvent(e.button_index, e.down); break;
      case PlatformPointerEvent::wheel: io.AddMouseWheelEvent(e.x, e.y); break;
      case PlatformPointerEvent::leave: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
    }
  }
}

void gpu_ui_new_frame(GpuUi* ui, VkExtent2D view_extent) {
  int64_t now = timer_now_ns();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)view_extent.width, (float)view_extent.height);
  io.DeltaTime = ui->last_ns && now > ui->last_ns ? (now - ui->last_ns) / 1e9f : 1.0f / 60.0f;
  ui->last_ns = now;
  ImGui::NewFrame();
}

void gpu_ui_renderer_shutdown(GpuUi* ui, const GpuApiTable* gpu, GpuDevice* dev) {
  ImGui::SetCurrentContext(ui->ctx);
  gpu->imgui_shutdown(dev, &ui->im);
  ui->im = GpuImGui{};
}

void gpu_ui_shutdown(GpuUi* ui) {
  ImGui::DestroyContext(ui->ctx);
  ui->ctx = nullptr;
  ui->last_ns = 0;
}
