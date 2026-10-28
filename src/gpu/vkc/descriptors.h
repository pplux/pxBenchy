#pragma once
#include <cstdint>
#include <vector>
#include "gpu/vk.h"

namespace vkc {

// Resets every pool of the slot. Call only after the slot's fence has been waited.
void descriptors_frame_begin(GpuDevice* dev, GpuDescriptorPools* dp, uint32_t slot);
// Allocates a set valid until the slot's next frame_begin; chains a new pool when the current ones are full.
// VK_NULL_HANDLE (error printed) on failure.
VkDescriptorSet descriptors_alloc(GpuDevice* dev, GpuDescriptorPools* dp, uint32_t slot, VkDescriptorSetLayout layout);
// One write per binding i with views[i]: texture = device sampler + SHADER_READ_ONLY_OPTIMAL, image = GENERAL.
void descriptors_write(GpuDevice* dev, VkDescriptorSet set, const std::vector<GpuBindingDecl>& bindings,
                       const VkImageView* views);
void descriptors_destroy(GpuDevice* dev, GpuDescriptorPools* dp); // caller ensures the GPU no longer uses them

} // namespace vkc
