#include "gpu/vkc/resources.h"
#include <tracy/Tracy.hpp>
#include <cstdio>
#include <cstring>

namespace vkc {

bool create_raw_buffer(GpuDevice* dev, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, VkBuffer* h,
                       VmaAllocation* alloc, void** mapped) {
  VkBufferCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VmaAllocationCreateInfo ai = {};
  if (host_visible) {
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  } else {
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  }
  VmaAllocationInfo info;
  VkResult r = vmaCreateBuffer(dev->vma, &ci, &ai, h, alloc, &info);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vmaCreateBuffer failed (%d)\n", (int)r);
    return false;
  }
  TracyAllocN(*alloc, info.size, "vk_resources");
  if (mapped) *mapped = info.pMappedData;
  return true;
}

static void destroy_raw_buffer(GpuDevice* dev, VkBuffer* h, VmaAllocation* alloc) {
  if (*h) {
    TracyFreeN(*alloc, "vk_resources");
    vmaDestroyBuffer(dev->vma, *h, *alloc); // also unmaps
  }
  *h = VK_NULL_HANDLE;
  *alloc = nullptr;
}

bool buffer_create(GpuDevice* dev, const GpuBufferDesc& desc, GpuBuffer* out) {
  if (desc.size == 0 || desc.usage == 0) {
    fprintf(stderr, "error: buffer needs non-zero size and usage\n");
    return false;
  }
  VkBufferUsageFlags usage = 0;
  if (desc.usage & GPU_BUF_VERTEX) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (desc.usage & GPU_BUF_INDEX) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (desc.usage & GPU_BUF_UNIFORM) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (desc.usage & GPU_BUF_STORAGE) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (desc.usage & GPU_BUF_INDIRECT) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

  GpuBuffer b;
  b.size = desc.size;
  b.usage = desc.usage;
  b.mode = desc.mode;
  bool ok;
  if (desc.mode == GpuBufferMode::mapped) {
    ok = create_raw_buffer(dev, desc.size, usage, true, &b.h, &b.alloc, &b.mapped);
  } else {
    ok = create_raw_buffer(dev, desc.size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &b.h, &b.alloc, nullptr);
  }
  if (!ok) return false;
  *out = b;
  return true;
}

void buffer_destroy(GpuDevice* dev, GpuBuffer* buf) {
  for (uint32_t i = 0; i < PXB_FRAMES_IN_FLIGHT; i++)
    destroy_raw_buffer(dev, &buf->staging[i], &buf->staging_alloc[i]);
  destroy_raw_buffer(dev, &buf->h, &buf->alloc);
  *buf = GpuBuffer{};
}

bool buffer_open(GpuDevice* dev, GpuBuffer* buf, uint32_t slot) {
  if (buf->is_open) {
    fprintf(stderr, "error: buffer already open\n");
    return false;
  }
  if (buf->mode == GpuBufferMode::staged) {
    if (!buf->staging[slot] && !create_raw_buffer(dev, buf->size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                                                  &buf->staging[slot], &buf->staging_alloc[slot],
                                                  &buf->staging_map[slot]))
      return false;
    buf->slot = slot;
  }
  buf->is_open = true;
  buf->lo = buf->size;
  buf->hi = 0;
  return true;
}

bool buffer_write(GpuBuffer* buf, VkDeviceSize offset, const void* data, VkDeviceSize size) {
  if (!buf->is_open) {
    fprintf(stderr, "error: buffer write outside open/close\n");
    return false;
  }
  if (offset > buf->size || size > buf->size - offset) {
    fprintf(stderr, "error: buffer write out of bounds (offset %llu size %llu, buffer %llu)\n",
            (unsigned long long)offset, (unsigned long long)size, (unsigned long long)buf->size);
    return false;
  }
  if (size == 0) return true;
  void* dst = buf->mode == GpuBufferMode::staged ? buf->staging_map[buf->slot] : buf->mapped;
  memcpy((char*)dst + offset, data, size);
  if (offset < buf->lo) buf->lo = offset;
  if (offset + size > buf->hi) buf->hi = offset + size;
  return true;
}

uint32_t buffer_read_uses(uint32_t usage) {
  uint32_t uses = 0;
  if (usage & GPU_BUF_VERTEX) uses |= VERTEX_BUFFER;
  if (usage & GPU_BUF_INDEX) uses |= INDEX_BUFFER;
  if (usage & GPU_BUF_UNIFORM) uses |= UNIFORM_GRAPHICS;
  if (usage & GPU_BUF_STORAGE) uses |= STORAGE_READ_GRAPHICS;
  if (usage & GPU_BUF_INDIRECT) uses |= INDIRECT_BUFFER;
  return uses;
}

bool texture_create(GpuDevice* dev, const GpuTextureDesc& desc, GpuTexture* out) {
  bool color = desc.kind == GpuTextureKind::color;
  uint32_t full = 1;
  for (uint32_t m = desc.width > desc.height ? desc.width : desc.height; m > 1; m >>= 1) full++;
  if (desc.mip_levels < 1 || desc.mip_levels > full || desc.mip_levels > PXB_MAX_MIPS ||
      (desc.storage && !color)) {
    fprintf(stderr, "error: invalid texture mip_levels=%u (max %u) storage=%d\n", desc.mip_levels, full,
            (int)desc.storage);
    return false;
  }
  GpuTexture t;
  t.format = desc.format;
  t.extent = {desc.width, desc.height};
  t.kind = desc.kind;
  t.mip_levels = desc.mip_levels;
  t.storage = desc.storage;

  VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (color)
    usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (desc.storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  VkImageCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = desc.format,
      .extent = {desc.width, desc.height, 1},
      .mipLevels = desc.mip_levels,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
  VmaAllocationInfo info;
  VkResult r = vmaCreateImage(dev->vma, &ci, &ai, &t.image, &t.alloc, &info);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vmaCreateImage failed (%d)\n", (int)r);
    return false;
  }
  TracyAllocN(t.alloc, info.size, "vk_resources");
  VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = t.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = desc.format,
      .subresourceRange = {color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, desc.mip_levels, 0, 1},
  };
  r = vkCreateImageView(dev->device, &vi, nullptr, &t.view);
  for (uint32_t i = 0; r == VK_SUCCESS && desc.storage && i < desc.mip_levels; i++) {
    vi.subresourceRange.baseMipLevel = i;
    vi.subresourceRange.levelCount = 1;
    r = vkCreateImageView(dev->device, &vi, nullptr, &t.mip_views[i]);
  }
  if (r != VK_SUCCESS) {
    fprintf(stderr, "error: vkCreateImageView failed (%d)\n", (int)r);
    texture_destroy(dev, &t);
    return false;
  }
  *out = t;
  return true;
}

void texture_destroy(GpuDevice* dev, GpuTexture* tex) {
  if (tex->view) vkDestroyImageView(dev->device, tex->view, nullptr);
  for (VkImageView v : tex->mip_views)
    if (v) vkDestroyImageView(dev->device, v, nullptr);
  if (tex->image) {
    TracyFreeN(tex->alloc, "vk_resources");
    vmaDestroyImage(dev->vma, tex->image, tex->alloc);
  }
  *tex = GpuTexture{};
}

} // namespace vkc
