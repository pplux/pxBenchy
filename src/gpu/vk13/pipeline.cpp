#include "gpu/vk13.h"

namespace vk13 {

static constexpr vkc::ShaderTarget SHADER_TARGET = {VK_API_VERSION_1_3, 0x10600}; // Vulkan 1.3, SPIR-V 1.6

bool pipeline_create(GpuDevice* dev, const GpuPipelineDesc& desc, GpuPipeline* out, std::string* error) {
  if (desc.cs) return vkc::part::compute_pipeline(dev, desc, SHADER_TARGET, out, error);
  VkPipelineRenderingCreateInfo rendering = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    .colorAttachmentCount = 1,
    .pColorAttachmentFormats = &desc.color_format,
    .depthAttachmentFormat = desc.depth_format,
  };
  return vkc::part::graphics_pipeline(dev, desc, SHADER_TARGET, &rendering, VK_NULL_HANDLE, out, error);
}

} // namespace vk13
