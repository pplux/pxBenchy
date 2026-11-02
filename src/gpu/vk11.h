#pragma once
#include <string>
#include "gpu/vk.h"
#include "gpu/vkc/descriptors.h"
#include "gpu/vkc/device.h"
#include "gpu/vkc/imgui.h"
#include "gpu/vkc/pass.h"
#include "gpu/vkc/pipeline.h"
#include "gpu/vkc/resources.h"
#include "gpu/vkc/screenshot.h"
#include "gpu/vkc/shader.h"
#include "gpu/vkc/swapchain.h"
#include "gpu/vkc/timers.h"

namespace vk11 {

// Mode-neutral (vkc), shared by all modes.
using vkc::buffer_create;
using vkc::buffer_destroy;
using vkc::buffer_open;
using vkc::buffer_write;
using vkc::descriptors_alloc;
using vkc::descriptors_destroy;
using vkc::descriptors_frame_begin;
using vkc::descriptors_write;
using vkc::draw;
using vkc::imgui_draw;
using vkc::imgui_shutdown;
using vkc::layout_build;
using vkc::layout_field_is_int;
using vkc::layout_pack;
using vkc::layout_pack_u32;
using vkc::pass_shutdown;
using vkc::pipeline_destroy;
using vkc::present_mode_name;
using vkc::screenshot_destroy;
using vkc::screenshot_write;
using vkc::shader_init;
using vkc::shader_shutdown;
using vkc::surface_create;
using vkc::surface_destroy;
using vkc::texture_create;
using vkc::timer_create;
using vkc::timer_results;
using vkc::timers_break_interval;
using vkc::timers_busy;
using vkc::timers_clear;
using vkc::timers_drop_frame;
using vkc::timers_init;
using vkc::timers_interval;
using vkc::timers_resolve_all;
using vkc::timers_shutdown;

bool device_init(GpuDevice* dev, bool validation, void* native_surface); // instance + surface + device + caches
void device_shutdown(GpuDevice* dev); // destroys the render pass / framebuffer caches, then vkc::device_shutdown
// Vulkan info as a probe instance of this version sees it (temporary instance, no surface or validation): instance
// version, extensions and layers, then every physical device of this version with its properties, limits, features,
// memory, queues, extensions and formats. JSON object, members indented from `depth`; "null" without such an instance.
std::string info_json(int depth);
void texture_destroy(GpuDevice* dev, GpuTexture* tex); // forgets cached framebuffers on its views, then vkc's
// Creates frame slots (first call) and the swapchain; on an existing swapchain recreates it via
// oldSwapchain. Caller must ensure the device is idle when recreating. Forgets cached framebuffers on the views it
// destroys, then vkc's.
bool swapchain_create(GpuDevice* dev, GpuSwapchain* sc);
void swapchain_destroy(GpuDevice* dev, GpuSwapchain* sc); // swapchain + frame slots; device must be idle

// Brings a resource from its tracked state to `want`, recording at most one vkCmdPipelineBarrier into cmd.
// Barrier when tracked or wanted uses have a write (vkc::USE_WRITES), or (images) the layout differs; src = tracked,
// dst = want, then tracked = want. Read->read with the same layout records nothing and ORs want's uses into tracked,
// so a later write waits on all prior readers. Empty src stages become TOP_OF_PIPE, empty dst stages BOTTOM_OF_PIPE.
// Tracking assumes one queue with in-order submits; cross-frame hazards are the caller's job.
void ensure_state(VkCommandBuffer cmd, GpuBuffer* buf, vkc::State want);
void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, vkc::State want); // all mips
// Per-mip tracking over [first_mip, first_mip + mip_count): each mip follows the rule above; consecutive mips with the
// same tracked state share one barrier, all barriers go in one vkCmdPipelineBarrier (src stages ORed over them).
void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, uint32_t first_mip, uint32_t mip_count, vkc::State want);
// Raw image form (swapchain images): mip 0, layer 0.
void ensure_state(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, vkc::State* tracked, vkc::State want);

// Staged close() records ensure_state(COPY / TRANSFER_WRITE), vkCmdCopyBuffer of the written range into cmd (no
// submit, no wait) and ensure_state to the union of read states implied by usage (vertex/index/uniform/storage), so
// the buffer is usable inside a pass right after. Must not be called inside a pass. Mapped close() records nothing.
bool buffer_close(GpuBuffer* buf, VkCommandBuffer cmd);

// Uploads `mip` (RGBA8 texels, rows top to bottom, tightly packed, size = 4 * mip width * mip height) through a new
// mapped staging buffer: ensure_state(COPY / TRANSFER_WRITE, TRANSFER_DST_OPTIMAL) on that mip and
// vkCmdCopyBufferToImage into cmd. The mip stays in TRANSFER_DST. Must not be called inside a pass. *staging receives
// the staging buffer; the caller destroys it (buffer_destroy) once the GPU has executed cmd. Fails (prints) on a
// bad mip or size.
bool texture_upload(GpuDevice* dev, GpuTexture* tex, uint32_t mip, const void* data, VkDeviceSize size,
                    VkCommandBuffer cmd, GpuBuffer* staging);

// Blits the full extent of src mip into the full extent of dst mip (scaled with filter). Brings src mip to
// TRANSFER_SRC / BLIT read and dst mip to TRANSFER_DST / BLIT write via ensure_state. Must not be called inside a pass.
// Fails (prints) on an out-of-range mip or src == dst on the same mip.
bool texture_blit(VkCommandBuffer cmd, GpuTexture* src, uint32_t src_mip, GpuTexture* dst, uint32_t dst_mip,
                  VkFilter filter);

// With desc.cs: compute pipeline (prelude = push block + bindings; graphics-only fields are ignored).
// Subpass 0 of the cached compatible render pass for (color_format, depth_format): valid only inside vk11::pass_begin.
// Dynamic viewport/scissor, no MSAA, at most one descriptor set (set 0, from desc.bindings). Binding names must be
// unique GLSL identifiers. Shaders target Vulkan 1.1 / SPIR-V 1.3.
// On failure returns false with *error set (shader errors verbatim from shader_compile); nothing is leaked.
bool pipeline_create(GpuDevice* dev, const GpuPipelineDesc& desc, GpuPipeline* out, std::string* error);

// Begins a cached render pass (load = CLEAR or LOAD per desc.clear) on a cached framebuffer of the current swapchain
// image, or offscreen_tex in offscreen mode (call between frame_begin and frame_end), after ensure_state brings the
// attachments to their subpass layouts, and sets a full-extent viewport (Y down) and scissor. With depth, the depth
// texture is (re)created (vk11::texture_destroy, forgetting its framebuffers) when the swapchain extent differs; this
// assumes the extent only changes after a device-idle swapchain recreate. Fails (prints) if a pass is already open or
// a render pass / framebuffer cannot be created.
bool pass_begin(GpuDevice* dev, GpuSwapchain* sc, GpuPassCtx* ctx, VkCommandBuffer cmd, const GpuPassDesc& desc);
// Fails (prints) outside a pass.
bool pass_end(GpuPassCtx* ctx);

// Waits the frame slot, acquires an image, begins the command buffer and transitions the image to
// COLOR_ATTACHMENT_OPTIMAL. Offscreen: no acquire (the frame renders into offscreen_tex). Returns false (no frame) when
// there is no swapchain or it needs recreating.
bool frame_begin(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer* out_cmd);
// With an acquired image: transitions it to PRESENT_SRC_KHR, submits (vkQueueSubmit, waiting the acquire at
// COLOR_ATTACHMENT_OUTPUT) and presents; returns true if the present succeeded. Without (offscreen): submits only,
// returns false. A submit failure sets submit_failed; the slot fence stays unsignaled, so frame_begin must not be
// called again.
bool frame_end(GpuDevice* dev, GpuSwapchain* sc);
// Offscreen, outside a frame: waits the previous capture/blit, submits the copy offscreen_tex -> capture (queue order:
// after the last submitted frame, no acquire). False when there is no swapchain or the submit failed.
bool capture(GpuDevice* dev, GpuSwapchain* sc);
// Offscreen, outside a frame: waits the previous capture/blit, acquires a swapchain image, begins the blit command
// buffer and records the copy capture -> image. The caller may record passes with GpuPassDesc::swapchain (HUD) into
// *out_cmd. False (nothing to end) when there is no swapchain or acquire fails.
bool blit_begin(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer* out_cmd);
// Transitions the image to PRESENT_SRC_KHR, submits (vkQueueSubmit, waiting the acquire at TRANSFER) and presents;
// returns true if the present succeeded.
bool blit_end(GpuDevice* dev, GpuSwapchain* sc);

// Inside an open frame, before the HUD: records a copy of the frame's render target (acquired swapchain image, or
// offscreen_tex) into a new host-readable buffer. Needs GpuSwapchain::readable without --offscreen. False (prints,
// nothing recorded) on an unsupported format or allocation failure.
bool screenshot_record(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer cmd, GpuScreenshot* shot);

// Call right after vk11::frame_begin (slot fence already waited): resolves the slot's previous frame, resets its pool
// and writes the frame-start timestamp and opens the Tracy GPU zone `gpu_frame`. Fails (prints) if a frame is already
// open.
bool timers_frame_begin(GpuDevice* dev, GpuSwapchain* sc, GpuTimers* t, VkCommandBuffer cmd);
// Call right before vk11::frame_end: closes `gpu_frame` and writes the frame-end timestamp. Timers still open are
// reported (prints), closed in the Tracy zone and discarded. Fails (prints) outside a frame.
bool timers_frame_end(GpuTimers* t, VkCommandBuffer cmd);
// Same command buffer as the open frame, LIFO, a timer cannot be nested in itself. Fail (print) otherwise.
bool timer_begin(GpuTimers* t, VkCommandBuffer cmd, uint32_t id);
bool timer_end(GpuTimers* t, VkCommandBuffer cmd, uint32_t id);

// ImGui renderer (draw/shutdown in vkc/imgui.h). Needs a current ImGui context: registers itself as its renderer
// backend (RendererHasTextures: font atlas and other textures are created/updated/destroyed on ImGui's requests).
// After dev->transform is set (first swapchain_create).
bool imgui_init(GpuDevice* dev, GpuImGui* im, VkFormat color_format);
// Recreates the pipeline when color_format differs (after a swapchain recreate; device must be idle).
bool imgui_set_format(GpuDevice* dev, GpuImGui* im, VkFormat color_format);
// Outside a pass, after ImGui::Render(), on the open frame's command buffer (after frame_begin) or the offscreen blit's
// (after blit_begin); then imgui_draw with the same dd inside a pass on that command buffer, at most once. Buffers
// are kept per slot (frame slot or blit) and reused only after that slot's fence wait, so draws still in flight on
// either path are never overwritten. Honors dd->Textures requests (uploads recorded into cmd; textures ImGui destroys
// are freed once every slot that drew them has been waited) and copies the vertices/indices into the slot's mapped
// buffers.
// Fails (prints) on another command buffer, a non-RGBA32 texture or an allocation failure.
bool imgui_prepare(GpuDevice* dev, GpuSwapchain* sc, GpuImGui* im, VkCommandBuffer cmd, ImDrawData* dd);

} // namespace vk11
