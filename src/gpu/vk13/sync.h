#pragma once
#include <volk.h>
#include "gpu/vk.h"

namespace vk13 {

// sync2 translation of a set of vkc uses: OR of the table rows for the set bits. All uses map to one layout.
struct Sync {
  VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
  VkAccessFlags2 access = VK_ACCESS_2_NONE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

Sync sync_of(uint32_t uses);

} // namespace vk13
