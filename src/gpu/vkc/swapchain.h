#pragma once
#include <cstdint>
#include "gpu/vk.h"

namespace vkc {

const char* present_mode_name(VkPresentModeKHR m); // "IMMEDIATE", "FIFO", ...

// Creates frame slots (first call) and the swapchain; on an existing swapchain recreates it via
// oldSwapchain. Caller must ensure the device is idle when recreating.
bool swapchain_create(GpuDevice* dev, GpuSwapchain* sc);
void swapchain_destroy(GpuDevice* dev, GpuSwapchain* sc); // swapchain + frame slots; device must be idle

} // namespace vkc

// Frame slot pieces shared by the modes; each mode records its own barriers and submit.
namespace vkc::part {

// Waits f's fence, then (if acquire_use != USE_NONE) acquires an image, resets fence + pool and begins f.cmd. The
// acquired image's tracked state starts as acquire_use (ACQUIRE_*: UNDEFINED, at the wait stage). False when there is
// no swapchain, it needs recreating, a submit failed, or acquire fails.
bool slot_begin(GpuDevice* dev, GpuSwapchain* sc, GpuFrame& f, uint32_t acquire_use);
// Presents sc->image waiting render_finished. Returns true if the present succeeded; sets needs_recreate on
// SUBOPTIMAL / OUT_OF_DATE / errors.
bool present(GpuDevice* dev, GpuSwapchain* sc, VkSemaphore render_finished);

} // namespace vkc::part
