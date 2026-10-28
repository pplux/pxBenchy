#pragma once
#include <string>
#include "gpu/vk.h"
#include "gpu/vkc/descriptors.h"
#include "gpu/vkc/device.h"
#include "gpu/vkc/pass.h"
#include "gpu/vkc/pipeline.h"
#include "gpu/vkc/resources.h"
#include "gpu/vkc/screenshot.h"
#include "gpu/vkc/shader.h"
#include "gpu/vkc/swapchain.h"
#include "gpu/vkc/text.h"
#include "gpu/vkc/timers.h"

namespace vk13 {

// Mode-neutral (vkc), shared by all modes.
using vkc::surface_create;
using vkc::surface_destroy;
using vkc::device_shutdown;
using vkc::shader_init;
using vkc::shader_shutdown;
using vkc::buffer_create;
using vkc::buffer_destroy;
using vkc::buffer_open;
using vkc::buffer_write;
using vkc::texture_create;
using vkc::texture_destroy;
using vkc::layout_build;
using vkc::layout_pack;
using vkc::layout_pack_u32;
using vkc::layout_field_is_int;
using vkc::pipeline_destroy;
using vkc::draw;
using vkc::pass_shutdown;
using vkc::descriptors_frame_begin;
using vkc::descriptors_alloc;
using vkc::descriptors_write;
using vkc::descriptors_destroy;
using vkc::present_mode_name;
using vkc::swapchain_create;
using vkc::swapchain_destroy;
using vkc::screenshot_write;
using vkc::screenshot_destroy;
using vkc::timers_init;
using vkc::timers_shutdown;
using vkc::timers_break_interval;
using vkc::timers_resolve_all;
using vkc::timers_clear;
using vkc::timer_create;
using vkc::timers_busy;
using vkc::timers_interval;
using vkc::timer_results;
using vkc::text_draw;
using vkc::text_shutdown;

bool device_init(GpuDevice* dev, bool validation, void* native_surface); // instance + surface + device

// Brings a resource from its tracked state to `want`, recording at most one vkCmdPipelineBarrier2 into cmd.
// Barrier when tracked or wanted uses have a write (vkc::USE_WRITES), or (images) the layout differs; src = tracked,
// dst = want, then tracked = want. Read->read with the same layout records nothing and ORs want's uses into tracked,
// so a later write waits on all prior readers.
// Tracking assumes one queue with in-order submits; cross-frame hazards are the caller's job.
void ensure_state(VkCommandBuffer cmd, GpuBuffer* buf, vkc::State want);
void ensure_state(VkCommandBuffer cmd, GpuTexture* tex, vkc::State want); // all mips
// Per-mip tracking over [first_mip, first_mip + mip_count): each mip follows the rule above; consecutive mips with the
// same tracked state share one barrier, all barriers go in one vkCmdPipelineBarrier2.
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
// Dynamic rendering (no render pass), dynamic viewport/scissor, no MSAA, at most one descriptor set (set 0, from
// desc.bindings). Binding names must be unique GLSL identifiers.
// On failure returns false with *error set (shader errors verbatim from shader_compile); nothing is leaked.
bool pipeline_create(GpuDevice* dev, const GpuPipelineDesc& desc, GpuPipeline* out, std::string* error);

// Begins dynamic rendering on the current swapchain image, or offscreen_tex in offscreen mode (call between
// frame_begin and frame_end), and sets
// a full-extent viewport (Y down) and scissor. With depth, the depth texture is (re)created when the swapchain extent
// differs; this assumes the extent only changes after a device-idle swapchain recreate. Fails (prints) if a pass is
// already open.
bool pass_begin(GpuDevice* dev, GpuSwapchain* sc, GpuPassCtx* ctx, VkCommandBuffer cmd, const GpuPassDesc& desc);
// Fails (prints) outside a pass.
bool pass_end(GpuPassCtx* ctx);

// Waits the frame slot, acquires an image, begins the command buffer and transitions the image to
// COLOR_ATTACHMENT_OPTIMAL. Offscreen: no acquire (the frame renders into offscreen_tex). Returns false (no frame) when
// there is no swapchain or it needs recreating.
bool frame_begin(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer* out_cmd);
// With an acquired image: transitions it to PRESENT_SRC_KHR, submits and presents; returns true if the present
// succeeded. Without (offscreen): submits only, returns false. A submit failure sets submit_failed; the slot fence stays
// unsignaled, so frame_begin must not be called again.
bool frame_end(GpuDevice* dev, GpuSwapchain* sc);
// Offscreen, outside a frame: waits the previous capture/blit, submits the copy offscreen_tex -> capture (queue order:
// after the last submitted frame, no acquire). False when there is no swapchain or the submit failed.
bool capture(GpuDevice* dev, GpuSwapchain* sc);
// Offscreen, outside a frame: waits the previous capture/blit, acquires a swapchain image, begins the blit command
// buffer and records the copy capture -> image. The caller may record passes with GpuPassDesc::swapchain (HUD) into
// *out_cmd. False (nothing to end) when there is no swapchain or acquire fails.
bool blit_begin(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer* out_cmd);
// Transitions the image to PRESENT_SRC_KHR, submits and presents; returns true if the present succeeded.
bool blit_end(GpuDevice* dev, GpuSwapchain* sc);

// Inside an open frame, before the HUD: records a copy of the frame's render target (acquired swapchain image, or
// offscreen_tex) into a new host-readable buffer. Needs GpuSwapchain::readable without --offscreen. False (prints, nothing
// recorded) on an unsupported format or allocation failure.
bool screenshot_record(GpuDevice* dev, GpuSwapchain* sc, VkCommandBuffer cmd, GpuScreenshot* shot);

// Call right after vk13::frame_begin (slot fence already waited): resolves the slot's previous frame, resets its pool and
// writes the frame-start timestamp and opens the Tracy GPU zone `gpu_frame`. Fails (prints) if a frame is already open.
bool timers_frame_begin(GpuDevice* dev, GpuSwapchain* sc, GpuTimers* t, VkCommandBuffer cmd);
// Call right before vk13::frame_end: closes `gpu_frame` and writes the frame-end timestamp. Timers still open are reported (prints), closed
// in the Tracy zone and discarded. Fails (prints) outside a frame.
bool timers_frame_end(GpuTimers* t, VkCommandBuffer cmd);
// Same command buffer as the open frame, LIFO, a timer cannot be nested in itself. Fail (print) otherwise.
bool timer_begin(GpuTimers* t, VkCommandBuffer cmd, uint32_t id);
bool timer_end(GpuTimers* t, VkCommandBuffer cmd, uint32_t id);

bool text_init(GpuDevice* dev, GpuText* t, VkFormat color_format);
// Recreates the pipeline when color_format differs (after a swapchain recreate; device must be idle).
bool text_set_format(GpuDevice* dev, GpuText* t, VkFormat color_format);

} // namespace vk13
