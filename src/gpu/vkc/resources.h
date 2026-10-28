#pragma once
#include <cstdint>
#include "gpu/vk.h"

namespace vkc {

// Creates buffer + allocation. host_visible: sequential-write, persistently mapped, HOST_COHERENT (memcpy, no flush);
// otherwise device-preferred. On failure nothing is left allocated.
bool create_raw_buffer(GpuDevice* dev, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, VkBuffer* h,
                       VmaAllocation* alloc, void** mapped);

bool buffer_create(GpuDevice* dev, const GpuBufferDesc& desc, GpuBuffer* out);
void buffer_destroy(GpuDevice* dev, GpuBuffer* buf); // caller ensures the GPU no longer uses it

// File-like write API. write() fails (prints, returns false) outside open()/close() or out of bounds. Staged open()
// writes into staging[slot]: slot = the current frame slot, whose fence the caller has waited (frame begun), since that
// staging buffer last fed the copy of the previous frame in the same slot. Mapped: slot ignored, the script must not
// rewrite memory the GPU may still read.
bool buffer_open(GpuDevice* dev, GpuBuffer* buf, uint32_t slot);
bool buffer_write(GpuBuffer* buf, VkDeviceSize offset, const void* data, VkDeviceSize size);

// Union of read uses implied by GpuBufferUsage flags (vertex/index/uniform/storage/indirect).
uint32_t buffer_read_uses(uint32_t usage);

// color: COLOR_ATTACHMENT | SAMPLED | TRANSFER_SRC | TRANSFER_DST (+ STORAGE). depth: DEPTH_STENCIL_ATTACHMENT.
// 2D, 1 layer. Fails (prints) on an invalid mip count.
bool texture_create(GpuDevice* dev, const GpuTextureDesc& desc, GpuTexture* out);
void texture_destroy(GpuDevice* dev, GpuTexture* tex); // caller ensures the GPU no longer uses it

} // namespace vkc
