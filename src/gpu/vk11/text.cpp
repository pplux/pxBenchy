#include "gpu/vk11.h"
#include <cstdio>
#include <string>

namespace vk11 {

static bool create_pipeline(GpuDevice* dev, GpuText* t, VkFormat color_format) {
  GpuPipelineDesc desc;
  if (!vkc::part::text_pipeline_desc(color_format, &desc)) return false;
  std::string err;
  if (!pipeline_create(dev, desc, &t->pipe, &err)) {
    fprintf(stderr, "error: text pipeline: %s\n", err.c_str());
    return false;
  }
  t->format = color_format;
  return true;
}

static bool create_mapped(GpuDevice* dev, GpuBuffer* buf, uint32_t usage, const void* data, VkDeviceSize size) {
  if (!buffer_create(dev, {size, usage, GpuBufferMode::mapped}, buf)) return false;
  return buffer_open(dev, buf, 0) && buffer_write(buf, 0, data, size) && buffer_close(buf, VK_NULL_HANDLE);
}

bool text_init(GpuDevice* dev, GpuText* t, VkFormat color_format) {
  static const float quad[] = {0, 0, 1, 0, 1, 1, 0, 1};
  static const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
  bool ok = create_pipeline(dev, t, color_format) && create_mapped(dev, &t->quad, GPU_BUF_VERTEX, quad, sizeof(quad)) &&
            create_mapped(dev, &t->index, GPU_BUF_INDEX, idx, sizeof(idx));
  if (!ok) text_shutdown(dev, t);
  return ok;
}

bool text_set_format(GpuDevice* dev, GpuText* t, VkFormat color_format) {
  if (color_format == t->format) return true;
  pipeline_destroy(dev, &t->pipe);
  t->format = VK_FORMAT_UNDEFINED;
  return create_pipeline(dev, t, color_format);
}

} // namespace vk11
