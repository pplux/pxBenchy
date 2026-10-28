#include "gpu/vkc/descriptors.h"
#include <cstdio>

namespace vkc {

static constexpr uint32_t POOL_SETS = 256;

static VkDescriptorPool create_pool(GpuDevice* dev) {
  VkDescriptorPoolSize sizes[] = {
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POOL_SETS},
    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, POOL_SETS},
  };
  VkDescriptorPoolCreateInfo ci = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = POOL_SETS,
    .poolSizeCount = 2,
    .pPoolSizes = sizes,
  };
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkResult r = vkCreateDescriptorPool(dev->device, &ci, nullptr, &pool);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateDescriptorPool failed (%d)\n", (int)r);
    return VK_NULL_HANDLE;
  }
  return pool;
}

void descriptors_frame_begin(GpuDevice* dev, GpuDescriptorPools* dp, uint32_t slot) {
  for (VkDescriptorPool p : dp->pools[slot]) vkResetDescriptorPool(dev->device, p, 0);
  dp->current[slot] = 0;
}

VkDescriptorSet descriptors_alloc(GpuDevice* dev, GpuDescriptorPools* dp, uint32_t slot, VkDescriptorSetLayout layout) {
  std::vector<VkDescriptorPool>& pools = dp->pools[slot];
  uint32_t& cur = dp->current[slot];
  for (;;) {
    if (cur == pools.size()) {
      VkDescriptorPool p = create_pool(dev);
      if (!p) return VK_NULL_HANDLE;
      pools.push_back(p);
    }
    VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pools[cur],
      .descriptorSetCount = 1,
      .pSetLayouts = &layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult r = vkAllocateDescriptorSets(dev->device, &ai, &set);
    if (r == VK_SUCCESS) return set;
    if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
      fprintf(stderr, "error: vkAllocateDescriptorSets failed (%d)\n", (int)r);
      return VK_NULL_HANDLE;
    }
    cur++; // full: next pool in the chain
  }
}

void descriptors_write(GpuDevice* dev, VkDescriptorSet set, const std::vector<GpuBindingDecl>& bindings,
                       const VkImageView* views) {
  std::vector<VkDescriptorImageInfo> infos(bindings.size());
  std::vector<VkWriteDescriptorSet> writes(bindings.size());
  for (size_t i = 0; i < bindings.size(); i++) {
    bool tex = bindings[i].kind == GpuBindingKind::texture;
    infos[i] = {
      .sampler = tex ? dev->sampler : VK_NULL_HANDLE,
      .imageView = views[i],
      .imageLayout = tex ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
    };
    writes[i] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = (uint32_t)i,
      .descriptorCount = 1,
      .descriptorType = tex ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .pImageInfo = &infos[i],
    };
  }
  vkUpdateDescriptorSets(dev->device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

void descriptors_destroy(GpuDevice* dev, GpuDescriptorPools* dp) {
  for (uint32_t s = 0; s < PXB_FRAMES_IN_FLIGHT; s++) {
    for (VkDescriptorPool p : dp->pools[s]) vkDestroyDescriptorPool(dev->device, p, nullptr);
    dp->pools[s].clear();
    dp->current[s] = 0;
  }
}

} // namespace vkc
