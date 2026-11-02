#pragma once
#include <cstdint>
#include <vector>
#include "gpu/vk.h"

namespace vkc {

bool timers_init(GpuDevice* dev, GpuTimers* t);
void timers_shutdown(GpuDevice* dev, GpuTimers* t); // destroys pools; device must be idle

// After frame_end (offscreen blit submitted next): the interval from that frame to the next one is dropped.
void timers_break_interval(GpuTimers* t);
// After frame_end: that frame's busy and user timer results are dropped, and the interval to the next frame.
void timers_drop_frame(GpuTimers* t);
// Resolves every pending slot in frame order; call after vkDeviceWaitIdle.
void timers_resolve_all(GpuDevice* dev, GpuTimers* t);
void timers_clear(GpuTimers* t); // drops accumulated results (frame and user timers), next interval starts over

// Creates user timer `name` (also a Tracy GPU zone), works when timers are disabled. Returns its index, or UINT32_MAX
// when the name exists.
uint32_t timer_create(GpuTimers* t, const char* name);

const std::vector<double>& timers_busy(const GpuTimers* t);                // ns per frame
const std::vector<double>& timers_interval(const GpuTimers* t);            // ns per frame
const std::vector<double>& timer_results(const GpuTimers* t, uint32_t id); // ns per occurrence

} // namespace vkc

namespace vkc::part {

// Reads the slot's results (its frame is complete: fence waited or device idle) and appends them.
void timers_resolve_slot(GpuDevice* dev, GpuTimers* t, GpuTimerSlot* s);

} // namespace vkc::part
