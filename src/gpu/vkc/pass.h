#pragma once
#include "gpu/vk.h"

namespace vkc {

// Binds pipeline, buffers and push constants, then vkCmdDrawIndexed (or vkCmdDrawIndexedIndirect with desc.indirect).
// No barriers inside a pass: fails (prints) outside a pass, with missing args, or when a vertex/index buffer's tracked
// access has a write bit (e.g. staged buffer not closed yet).
bool draw(GpuPassCtx* ctx, const GpuDrawDesc& desc);
void pass_shutdown(GpuDevice* dev, GpuPassCtx* ctx); // destroys the depth texture; device must be idle

} // namespace vkc
