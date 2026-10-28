#pragma once
#include <volk.h>
#include "gpu/vk.h"

namespace vk11 {

// sync1 translation of a set of vkc uses: OR of the table rows for the set bits. All uses map to one layout.
struct Sync {
  VkPipelineStageFlags stage = 0;
  VkAccessFlags access = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

Sync sync_of(uint32_t uses);

} // namespace vk11
