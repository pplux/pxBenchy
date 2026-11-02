#include "gpu/vk13.h"

namespace vk13 {

static constexpr vkc::ShaderTarget SHADER_TARGET = {VK_API_VERSION_1_3, 0x10600}; // Vulkan 1.3, SPIR-V 1.6

bool pipeline_create(GpuDevice* dev, const GpuPipelineDesc& desc, GpuPipeline* out, std::string* error) {
  if (desc.cs) { return vkc::part::compute_pipeline(dev, desc, SHADER_TARGET, out, error); }
  vkc::part::GraphicsPipelineBuild b;
  if (!vkc::part::graphics_pipeline_begin(dev, desc, SHADER_TARGET, &b, error)) { return false; }
  VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &desc.color_format,
      .depthAttachmentFormat = desc.depth_format,
  };
  b.info.pNext = &rendering;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(dev->device, VK_NULL_HANDLE, 1, &b.info, nullptr, &pipeline);
  return vkc::part::graphics_pipeline_end(dev, desc, &b, r, pipeline, out, error);
}

} // namespace vk13
