#pragma once
#include "gpu/vk11.h"
#include "gpu/vk13.h"

enum class GpuApi { vk11, vk13 };

// Every gpu function called from src/lua and src/session.cpp, filled per mode from the same list, so a mode missing a
// function or with a different signature fails to compile. Overloads are listed apart, one field each.
#define PXB_GPU_API(X)                                                                                                 \
  X(device_init)                                                                                                       \
  X(device_shutdown)                                                                                                   \
  X(surface_create)                                                                                                    \
  X(surface_destroy)                                                                                                   \
  X(shader_init)                                                                                                       \
  X(shader_shutdown)                                                                                                   \
  X(buffer_create)                                                                                                     \
  X(buffer_destroy)                                                                                                    \
  X(buffer_open)                                                                                                       \
  X(buffer_write)                                                                                                      \
  X(buffer_close)                                                                                                      \
  X(texture_create)                                                                                                    \
  X(texture_destroy)                                                                                                   \
  X(texture_upload)                                                                                                    \
  X(texture_blit)                                                                                                      \
  X(layout_build)                                                                                                      \
  X(layout_pack)                                                                                                       \
  X(layout_pack_u32)                                                                                                   \
  X(layout_field_is_int)                                                                                               \
  X(pipeline_create)                                                                                                   \
  X(pipeline_destroy)                                                                                                  \
  X(pass_begin)                                                                                                        \
  X(pass_end)                                                                                                          \
  X(pass_shutdown)                                                                                                     \
  X(draw)                                                                                                              \
  X(descriptors_frame_begin)                                                                                           \
  X(descriptors_alloc)                                                                                                 \
  X(descriptors_write)                                                                                                 \
  X(descriptors_destroy)                                                                                               \
  X(present_mode_name)                                                                                                 \
  X(swapchain_create)                                                                                                  \
  X(swapchain_destroy)                                                                                                 \
  X(frame_begin)                                                                                                       \
  X(frame_end)                                                                                                         \
  X(capture)                                                                                                           \
  X(blit_begin)                                                                                                        \
  X(blit_end)                                                                                                          \
  X(screenshot_record)                                                                                                 \
  X(screenshot_write)                                                                                                  \
  X(screenshot_destroy)                                                                                                \
  X(timers_init)                                                                                                       \
  X(timers_shutdown)                                                                                                   \
  X(timers_frame_begin)                                                                                                \
  X(timers_frame_end)                                                                                                  \
  X(timers_break_interval)                                                                                             \
  X(timers_resolve_all)                                                                                                \
  X(timers_clear)                                                                                                      \
  X(timer_create)                                                                                                      \
  X(timer_begin)                                                                                                       \
  X(timer_end)                                                                                                         \
  X(timers_busy)                                                                                                       \
  X(timers_interval)                                                                                                   \
  X(timer_results)                                                                                                     \
  X(imgui_init)                                                                                                        \
  X(imgui_set_format)                                                                                                  \
  X(imgui_prepare)                                                                                                     \
  X(imgui_draw)                                                                                                        \
  X(imgui_shutdown)

struct GpuApiTable {
#define PXB_GPU_API_FIELD(f) decltype(&vk13::f) f;
  PXB_GPU_API(PXB_GPU_API_FIELD)
#undef PXB_GPU_API_FIELD
  void (*ensure_state_tex)(VkCommandBuffer cmd, GpuTexture* tex, vkc::State want);
  void (*ensure_state_mips)(VkCommandBuffer cmd, GpuTexture* tex, uint32_t first_mip, uint32_t mip_count,
                            vkc::State want);
};

// Static table of the mode; the only place that names a mode namespace.
const GpuApiTable* gpu_api_table(GpuApi api);
const char* gpu_api_name(GpuApi api); // "1.1" / "1.3" (--vulkan, startup line, report, HUD)

// vk13::supported(), probed on the first call and cached: picks the backend for `--vulkan auto` (D7) and enables the
// menu's 1.3 toggle.
bool gpu_vk13_supported();

// `{"vk11": vk11::info_json, "vk13": vk13::info_json}`: what each mode's instance version sees (multi-line JSON).
std::string gpu_vulkan_info_json();
