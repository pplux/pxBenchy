#include "gpu/vk13.h"
#include <cstdio>
#include <string>
#include <vector>

namespace vk13 {

static bool create_pipeline(GpuDevice* dev, GpuImGui* im, VkFormat color_format) {
  GpuPipelineDesc desc;
  if (!vkc::part::imgui_pipeline_desc(color_format, &desc)) { return false; }
  std::string err;
  if (!pipeline_create(dev, desc, &im->pipe, &err)) {
    fprintf(stderr, "error: imgui pipeline: %s\n", err.c_str());
    return false;
  }
  im->format = color_format;
  im->transform = dev->transform;
  return true;
}

bool imgui_init(GpuDevice* dev, GpuImGui* im, VkFormat color_format) {
  bool ok = create_pipeline(dev, im, color_format) && vkc::part::imgui_init(dev, im);
  if (!ok) { imgui_shutdown(dev, im); }
  return ok;
}

bool imgui_set_format(GpuDevice* dev, GpuImGui* im, VkFormat color_format) {
  if (color_format == im->format) { return true; }
  pipeline_destroy(dev, &im->pipe);
  im->format = VK_FORMAT_UNDEFINED;
  return create_pipeline(dev, im, color_format) && vkc::part::imgui_rebind(dev, im);
}

bool imgui_prepare(GpuDevice* dev, GpuSwapchain* sc, GpuImGui* im, VkCommandBuffer cmd, ImDrawData* dd) {
  std::vector<vkc::part::ImGuiUpload> uploads;
  if (!vkc::part::imgui_prepare_begin(dev, sc, im, cmd, dd, &uploads)) { return false; }
  for (const vkc::part::ImGuiUpload& u : uploads) {
    GpuBuffer staging;
    if (!texture_upload(dev, u.tex, 0, u.pixels, u.size, cmd, &staging)) { return false; }
    im->staging[im->slot].push_back(staging);
    ensure_state(cmd, u.tex, {vkc::SAMPLED_FRAGMENT});
  }
  return vkc::part::imgui_prepare_end(dev, im, cmd, dd, uploads);
}

} // namespace vk13
