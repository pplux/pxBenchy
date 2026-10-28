#include "gpu/vk11.h"
#include "gpu/vk11/caches.h"

namespace vk11 {

static constexpr vkc::ShaderTarget SHADER_TARGET = {VK_API_VERSION_1_1, 0x10300}; // Vulkan 1.1, SPIR-V 1.3

bool pipeline_create(GpuDevice* dev, const GpuPipelineDesc& desc, GpuPipeline* out, std::string* error) {
  if (desc.cs) return vkc::part::compute_pipeline(dev, desc, SHADER_TARGET, out, error);
  VkRenderPass rp = compat_render_pass(dev, desc.color_format, desc.depth_format);
  if (rp == VK_NULL_HANDLE) {
    *error = "vkCreateRenderPass failed";
    return false;
  }
  return vkc::part::graphics_pipeline(dev, desc, SHADER_TARGET, nullptr, rp, out, error);
}

} // namespace vk11
