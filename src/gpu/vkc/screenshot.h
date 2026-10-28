#pragma once
#include "gpu/vk.h"

namespace vkc {

// After the frame's submit has completed (device idle): writes the pixels as an RGBA8 PNG in view orientation (the
// surface transform undone), alpha forced to 255, then frees the buffer. False (prints) if the PNG write fails.
bool screenshot_write(GpuDevice* dev, const GpuSwapchain* sc, GpuScreenshot* shot, const char* path);
void screenshot_destroy(GpuDevice* dev, GpuScreenshot* shot);

} // namespace vkc

namespace vkc::part {

// Creates shot's host-readable buffer for one frame of sc. False (prints, nothing allocated) on an unsupported format
// or allocation failure.
bool screenshot_alloc(GpuDevice* dev, const GpuSwapchain* sc, GpuScreenshot* shot);

} // namespace vkc::part
