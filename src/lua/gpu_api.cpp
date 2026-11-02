#include "lua/gpu_api.h"

static GpuApiTable fill_vk11() {
  GpuApiTable t;
#define PXB_GPU_API_FILL(f) t.f = &vk11::f;
  PXB_GPU_API(PXB_GPU_API_FILL)
#undef PXB_GPU_API_FILL
  t.ensure_state_tex = &vk11::ensure_state;
  t.ensure_state_mips = &vk11::ensure_state;
  return t;
}

static GpuApiTable fill_vk13() {
  GpuApiTable t;
#define PXB_GPU_API_FILL(f) t.f = &vk13::f;
  PXB_GPU_API(PXB_GPU_API_FILL)
#undef PXB_GPU_API_FILL
  t.ensure_state_tex = &vk13::ensure_state;
  t.ensure_state_mips = &vk13::ensure_state;
  return t;
}

const GpuApiTable* gpu_api_table(GpuApi api) {
  static const GpuApiTable vk11_table = fill_vk11();
  static const GpuApiTable vk13_table = fill_vk13();
  if (api == GpuApi::vk11) { return &vk11_table; }
  return &vk13_table;
}

const char* gpu_api_name(GpuApi api) { return api == GpuApi::vk11 ? "1.1" : "1.3"; }

bool gpu_vk13_supported() {
  static const bool supported = vk13::supported();
  return supported;
}

std::string gpu_vulkan_info_json() {
  return "{\n  \"vk11\": " + vk11::info_json(1) + ",\n  \"vk13\": " + vk13::info_json(1) + "\n}";
}
