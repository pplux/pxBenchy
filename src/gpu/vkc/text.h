#pragma once
#include <cstdint>
#include "gpu/vk.h"

namespace vkc {

// Inside an open pass. x, y: top-left pixel of the first cell; cell = 8 * scale pixels; '\n' starts a new line at x;
// bytes >= 128 draw as '?'. Glyph on-bits use color, off-bits bg (alpha blended, bg alpha 0 = invisible).
bool text_draw(GpuText* t, GpuPassCtx* ctx, float x, float y, const char* str, uint32_t scale, const float color[4],
               const float bg[4]);
void text_shutdown(GpuDevice* dev, GpuText* t); // device must be idle

} // namespace vkc

namespace vkc::part {

// Text pipeline description for color_format (sources, push layout, vertex layout, blend); desc->fs points to a
// static string. Fails (prints) if the push layout cannot be built.
bool text_pipeline_desc(VkFormat color_format, GpuPipelineDesc* desc);

} // namespace vkc::part
