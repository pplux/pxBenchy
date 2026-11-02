#include "gpu/vk.h"
#include <bit>
#include <cassert>

namespace vkc {

// One row per Use bit, in bit order. Buffer-only uses are UNDEFINED.
static constexpr Layout LAYOUTS[] = {
    Layout::TRANSFER_SRC,     // COPY_SRC
    Layout::TRANSFER_DST,     // COPY_DST
    Layout::TRANSFER_SRC,     // BLIT_SRC
    Layout::TRANSFER_DST,     // BLIT_DST
    Layout::UNDEFINED,        // VERTEX_BUFFER
    Layout::UNDEFINED,        // INDEX_BUFFER
    Layout::UNDEFINED,        // INDIRECT_BUFFER
    Layout::UNDEFINED,        // UNIFORM_GRAPHICS
    Layout::UNDEFINED,        // STORAGE_READ_GRAPHICS
    Layout::GENERAL,          // STORAGE_RW_COMPUTE
    Layout::SHADER_READ_ONLY, // SAMPLED_FRAGMENT
    Layout::SHADER_READ_ONLY, // SAMPLED_COMPUTE
    Layout::COLOR_ATTACHMENT, // COLOR_ATTACH_READ
    Layout::COLOR_ATTACHMENT, // COLOR_ATTACH_WRITE
    Layout::DEPTH,            // DEPTH_ATTACH
    Layout::PRESENT_SRC,      // PRESENT
    Layout::UNDEFINED,        // HOST_READ
    Layout::UNDEFINED,        // ACQUIRE_COLOR
    Layout::UNDEFINED,        // ACQUIRE_COPY
};
static_assert(sizeof(LAYOUTS) / sizeof(LAYOUTS[0]) == USE_COUNT);

Layout layout_of(uint32_t uses) {
  if (!uses) { return Layout::UNDEFINED; }
  Layout l = LAYOUTS[std::countr_zero(uses)];
#ifndef NDEBUG
  for (uint32_t u = uses; u; u &= u - 1) { assert(LAYOUTS[std::countr_zero(u)] == l); }
#endif
  return l;
}

} // namespace vkc
