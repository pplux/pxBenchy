#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gpu/vk.h"
#include "gpu/vkc/shader.h"

namespace vkc {

// Builds a layout from unordered decls. Fails (false + *error) on unknown type, duplicate name, invalid GLSL
// identifier, or size > 128 bytes (the guaranteed maxPushConstantsSize). Empty decls give an empty layout.
bool layout_build(const char* instance, const std::vector<GpuFieldDecl>& decls, GpuLayout* out, std::string* error);

// GLSL block declaration with explicit offsets, e.g. "layout(push_constant) uniform Push { ... } push;".
// Empty string for an empty layout.
std::string layout_glsl(const GpuLayout& layout);

// Writes `count` floats at the field's offset into `block` (layout.size bytes). count must be 1/2/3/4/16 matching
// the field type; mat4 is column-major as given. Fails (false + *error) on unknown name, wrong count or an integer
// field.
bool layout_pack(const GpuLayout& layout, const char* name, const float* values, uint32_t count, void* block,
                 std::string* error);
// Integer form of layout_pack: count must be 1 (uint) or 4 (uvec4). Fails on a float field.
bool layout_pack_u32(const GpuLayout& layout, const char* name, const uint32_t* values, uint32_t count, void* block,
                     std::string* error);
// True when the field exists and is uint/uvec4.
bool layout_field_is_int(const GpuLayout& layout, const char* name);

// GLSL declarations for set 0, binding = list index, e.g. "layout(set=0, binding=0) uniform sampler2D tex;".
// Empty string for an empty list.
std::string bindings_glsl(const std::vector<GpuBindingDecl>& bindings);

void pipeline_destroy(GpuDevice* dev, GpuPipeline* pipe); // caller ensures the GPU no longer uses it

} // namespace vkc

// Pipeline creation pieces shared by the modes; each mode's pipeline_create adds its render-target hookup.
namespace vkc::part {

// Compute pipeline from desc.cs, compiled for target (prelude = push block + bindings; graphics-only fields are
// ignored).
// On failure returns false with *error set (shader errors verbatim from shader_compile); nothing is leaked.
bool compute_pipeline(GpuDevice* dev, const GpuPipelineDesc& desc, ShaderTarget target, GpuPipeline* out,
                      std::string* error);

// Graphics pipeline from desc.vs/fs, compiled for target, except the render-target hookup: p_next is chained into
// VkGraphicsPipelineCreateInfo and render_pass is its renderPass (subpass 0). Dynamic viewport/scissor, no MSAA, at
// most one descriptor set (set 0, from desc.bindings). Binding names must be unique GLSL identifiers.
// On failure returns false with *error set (shader errors verbatim from shader_compile); nothing is leaked.
bool graphics_pipeline(GpuDevice* dev, const GpuPipelineDesc& desc, ShaderTarget target, const void* p_next,
                       VkRenderPass render_pass, GpuPipeline* out, std::string* error);

} // namespace vkc::part
