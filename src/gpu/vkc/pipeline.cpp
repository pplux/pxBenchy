#include "gpu/vkc/pipeline.h"
#include <algorithm>
#include <cstring>

namespace vkc {

struct TypeInfo {
  const char* glsl;
  uint32_t size, align, count;
  bool is_int;
};

static const TypeInfo& type_info(GpuFieldType t) {
  static const TypeInfo infos[] = {
      {"float", 4, 4, 1, false},   {"vec2", 8, 8, 2, false}, {"vec3", 12, 16, 3, false}, {"vec4", 16, 16, 4, false},
      {"mat4", 64, 16, 16, false}, {"uint", 4, 4, 1, true},  {"uvec4", 16, 16, 4, true},
  };
  return infos[(int)t];
}

static bool parse_type(const std::string& s, GpuFieldType* out) {
  for (int i = 0; i < 7; i++) {
    if (s == type_info((GpuFieldType)i).glsl) {
      *out = (GpuFieldType)i;
      return true;
    }
  }
  return false;
}

static bool valid_ident(const std::string& s) {
  if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_')) { return false; }
  for (char c : s) {
    if (!(isalnum((unsigned char)c) || c == '_')) { return false; }
  }
  return true;
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

bool layout_build(const char* instance, const std::vector<GpuFieldDecl>& decls, GpuLayout* out, std::string* error) {
  GpuLayout l;
  l.instance = instance;
  if (!valid_ident(l.instance)) {
    *error = "invalid layout instance name '" + l.instance + "'";
    return false;
  }
  for (const GpuFieldDecl& d : decls) {
    GpuLayoutField f;
    f.name = d.name;
    if (!valid_ident(d.name)) {
      *error = "invalid field name '" + d.name + "' (must be a GLSL identifier)";
      return false;
    }
    if (!parse_type(d.type, &f.type)) {
      *error = "field '" + d.name + "': unknown type '" + d.type + "' (float, vec2, vec3, vec4, mat4, uint, uvec4)";
      return false;
    }
    for (const GpuLayoutField& o : l.fields) {
      if (o.name == d.name) {
        *error = "duplicate field name '" + d.name + "'";
        return false;
      }
    }
    l.fields.push_back(f);
  }
  std::sort(l.fields.begin(), l.fields.end(), [](const GpuLayoutField& a, const GpuLayoutField& b) {
    uint32_t sa = type_info(a.type).size, sb = type_info(b.type).size;
    return sa != sb ? sa > sb : a.name < b.name;
  });
  uint32_t off = 0, max_align = 4;
  for (GpuLayoutField& f : l.fields) {
    const TypeInfo& ti = type_info(f.type);
    off = align_up(off, ti.align);
    f.offset = off;
    off += ti.size;
    max_align = std::max(max_align, ti.align);
  }
  l.size = l.fields.empty() ? 0 : align_up(off, max_align);
  if (l.size > 128) {
    *error = "layout size " + std::to_string(l.size) + " bytes exceeds the 128 byte push constant limit";
    return false;
  }
  *out = std::move(l);
  return true;
}

std::string layout_glsl(const GpuLayout& layout) {
  if (layout.fields.empty()) { return std::string(); }
  std::string block = layout.instance;
  block[0] = (char)toupper((unsigned char)block[0]);
  std::string s = "layout(push_constant) uniform " + block + " {";
  for (const GpuLayoutField& f : layout.fields) {
    s += " layout(offset = " + std::to_string(f.offset) + ") " + type_info(f.type).glsl + " " + f.name + ";";
  }
  s += " } " + layout.instance + ";\n";
  return s;
}

// Shared by the float and integer packers: both element types are 4 bytes.
static bool pack(const GpuLayout& layout, const char* name, const void* values, uint32_t count, bool is_int,
                 void* block, std::string* error) {
  for (const GpuLayoutField& f : layout.fields) {
    if (f.name != name) { continue; }
    const TypeInfo& ti = type_info(f.type);
    if (ti.is_int != is_int) {
      *error = std::string("field '") + name + "' is " + ti.glsl + " and needs " + (ti.is_int ? "integers" : "floats");
      return false;
    }
    if (count != ti.count) {
      *error = std::string("field '") + name + "' is " + ti.glsl + " and needs " + std::to_string(ti.count) + " " +
               (is_int ? "integers" : "floats") + ", got " + std::to_string(count);
      return false;
    }
    memcpy((char*)block + f.offset, values, ti.count * 4);
    return true;
  }
  *error = std::string("unknown field '") + name + "'";
  return false;
}

bool layout_pack(const GpuLayout& layout, const char* name, const float* values, uint32_t count, void* block,
                 std::string* error) {
  return pack(layout, name, values, count, false, block, error);
}

bool layout_pack_u32(const GpuLayout& layout, const char* name, const uint32_t* values, uint32_t count, void* block,
                     std::string* error) {
  return pack(layout, name, values, count, true, block, error);
}

bool layout_field_is_int(const GpuLayout& layout, const char* name) {
  for (const GpuLayoutField& f : layout.fields) {
    if (f.name == name) { return type_info(f.type).is_int; }
  }
  return false;
}

std::string bindings_glsl(const std::vector<GpuBindingDecl>& bindings) {
  std::string s;
  for (size_t i = 0; i < bindings.size(); i++) {
    std::string b = std::to_string(i);
    if (bindings[i].kind == GpuBindingKind::texture) {
      s += "layout(set=0, binding=" + b + ") uniform sampler2D " + bindings[i].name + ";\n";
    } else {
      s += "layout(set=0, binding=" + b + ", rgba8) uniform image2D " + bindings[i].name + ";\n";
    }
  }
  return s;
}

static VkFormat to_vk_format(GpuVertexFormat f) {
  switch (f) {
    case GpuVertexFormat::float1: return VK_FORMAT_R32_SFLOAT;
    case GpuVertexFormat::float2: return VK_FORMAT_R32G32_SFLOAT;
    case GpuVertexFormat::float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case GpuVertexFormat::float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GpuVertexFormat::unorm4: return VK_FORMAT_R8G8B8A8_UNORM;
  }
  return VK_FORMAT_UNDEFINED;
}

static uint32_t format_size(GpuVertexFormat f) { return f == GpuVertexFormat::unorm4 ? 4 : ((uint32_t)f + 1) * 4; }

// Appends one binding's attributes (sequential locations from *location) and returns its stride.
static uint32_t add_attributes(const std::vector<GpuVertexFormat>& fmts, uint32_t binding, uint32_t* location,
                               std::vector<VkVertexInputAttributeDescription>* attrs) {
  uint32_t off = 0;
  for (GpuVertexFormat f : fmts) {
    attrs->push_back({(*location)++, binding, to_vk_format(f), off});
    off += format_size(f);
  }
  return off;
}

static VkShaderModule make_module(GpuDevice* dev, const std::vector<uint32_t>& spirv, std::string* error) {
  VkShaderModuleCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = spirv.size() * sizeof(uint32_t),
      .pCode = spirv.data(),
  };
  VkShaderModule m = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(dev->device, &ci, nullptr, &m);
  if (r != VK_SUCCESS) {
    *error = "vkCreateShaderModule failed (" + std::to_string((int)r) + ")";
    return VK_NULL_HANDLE;
  }
  return m;
}

// Vertex prelude: view_orientation(clip) rotates a clip-space position by the surface transform (Vulkan clip space,
// Y down: ROTATE_90 = 90 degrees clockwise on screen). Every vertex shader must write gl_Position through it.
static std::string view_orientation_glsl(VkSurfaceTransformFlagBitsKHR t) {
  const char* body = "p";
  if (t == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
    body = "vec4(-p.y, p.x, p.z, p.w)";
  } else if (t == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
    body = "vec4(-p.x, -p.y, p.z, p.w)";
  } else if (t == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
    body = "vec4(p.y, -p.x, p.z, p.w)";
  }
  return std::string("vec4 view_orientation(vec4 p) { return ") + body + "; }\n";
}

static bool validate_bindings(const std::vector<GpuBindingDecl>& bindings, std::string* error) {
  for (size_t i = 0; i < bindings.size(); i++) {
    const std::string& n = bindings[i].name;
    if (!valid_ident(n)) {
      *error = "invalid binding name '" + n + "' (must be a GLSL identifier)";
      return false;
    }
    for (size_t j = 0; j < i; j++) {
      if (bindings[j].name == n) {
        *error = "duplicate binding name '" + n + "'";
        return false;
      }
    }
  }
  return true;
}

// Set layout (null when bindings is empty) plus pipeline layout with one push range of `push_stages`. On failure
// returns the failing call's name in *what and VkResult; nothing is left allocated.
static VkResult make_layouts(GpuDevice* dev, const GpuPipelineDesc& desc, VkShaderStageFlags push_stages,
                             VkDescriptorSetLayout* set_layout, VkPipelineLayout* layout, const char** what) {
  VkResult r = VK_SUCCESS;
  if (!desc.bindings.empty()) {
    std::vector<VkDescriptorSetLayoutBinding> lbs;
    for (size_t i = 0; i < desc.bindings.size(); i++) {
      lbs.push_back({
          .binding = (uint32_t)i,
          .descriptorType = desc.bindings[i].kind == GpuBindingKind::texture ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_ALL,
      });
    }
    VkDescriptorSetLayoutCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = (uint32_t)lbs.size(),
        .pBindings = lbs.data(),
    };
    r = vkCreateDescriptorSetLayout(dev->device, &sci, nullptr, set_layout);
    if (r != VK_SUCCESS) {
      *what = "vkCreateDescriptorSetLayout";
      return r;
    }
  }
  VkPushConstantRange range = {push_stages, 0, desc.push.size};
  VkPipelineLayoutCreateInfo lci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = *set_layout ? 1u : 0u,
      .pSetLayouts = set_layout,
      .pushConstantRangeCount = desc.push.size > 0 ? 1u : 0u,
      .pPushConstantRanges = &range,
  };
  r = vkCreatePipelineLayout(dev->device, &lci, nullptr, layout);
  if (r != VK_SUCCESS) {
    *what = "vkCreatePipelineLayout";
    vkDestroyDescriptorSetLayout(dev->device, *set_layout, nullptr);
    *set_layout = VK_NULL_HANDLE;
  }
  return r;
}

void pipeline_destroy(GpuDevice* dev, GpuPipeline* pipe) {
  if (pipe->h) { vkDestroyPipeline(dev->device, pipe->h, nullptr); }
  if (pipe->layout) { vkDestroyPipelineLayout(dev->device, pipe->layout, nullptr); }
  if (pipe->set_layout) { vkDestroyDescriptorSetLayout(dev->device, pipe->set_layout, nullptr); }
  *pipe = GpuPipeline();
}

} // namespace vkc

namespace vkc::part {

bool compute_pipeline(GpuDevice* dev, const GpuPipelineDesc& desc, ShaderTarget target, GpuPipeline* out,
                      std::string* error) {
  if (desc.vs || desc.fs) {
    *error = "cs cannot be combined with vs/fs";
    return false;
  }
  if (!validate_bindings(desc.bindings, error)) { return false; }
  std::string prelude = layout_glsl(desc.push) + bindings_glsl(desc.bindings);
  std::vector<uint32_t> spv;
  if (!shader_compile(target, GpuShaderStage::compute, desc.cs, desc.cs_name, prelude.c_str(), &spv, error)) {
    return false;
  }
  VkShaderModule mod = make_module(dev, spv, error);
  if (!mod) { return false; }

  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const char* what = "";
  VkResult r = make_layouts(dev, desc, VK_SHADER_STAGE_COMPUTE_BIT, &set_layout, &layout, &what);
  if (r == VK_SUCCESS) {
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = mod,
                  .pName = "main"},
        .layout = layout,
    };
    r = vkCreateComputePipelines(dev->device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    if (r != VK_SUCCESS) { what = "vkCreateComputePipelines"; }
  }
  vkDestroyShaderModule(dev->device, mod, nullptr);
  if (r != VK_SUCCESS) {
    if (layout) { vkDestroyPipelineLayout(dev->device, layout, nullptr); }
    if (set_layout) { vkDestroyDescriptorSetLayout(dev->device, set_layout, nullptr); }
    *error = std::string(what) + " failed (" + std::to_string((int)r) + ")";
    return false;
  }
  out->h = pipeline;
  out->layout = layout;
  out->push = desc.push;
  out->set_layout = set_layout;
  out->bindings = desc.bindings;
  out->compute = true;
  return true;
}

bool graphics_pipeline_begin(GpuDevice* dev, const GpuPipelineDesc& desc, ShaderTarget target, GraphicsPipelineBuild* b,
                             std::string* error) {
  if (!desc.vs || !desc.fs) {
    *error = "pipeline needs vs and fs sources";
    return false;
  }
  bool has_depth = desc.depth_format != VK_FORMAT_UNDEFINED;
  if (!has_depth && (desc.depth_test || desc.depth_write)) {
    *error = "depth test/write requires a depth format";
    return false;
  }
  if (!strstr(desc.vs, "view_orientation(")) {
    *error = "vertex shader must write gl_Position = view_orientation(clip_position)";
    return false;
  }
  if (!validate_bindings(desc.bindings, error)) { return false; }
  std::string prelude = layout_glsl(desc.push) + bindings_glsl(desc.bindings);
  std::string vs_prelude = prelude + view_orientation_glsl(dev->transform);
  std::vector<uint32_t> vs_spv, fs_spv;
  if (!shader_compile(target, GpuShaderStage::vertex, desc.vs, desc.vs_name, vs_prelude.c_str(), &vs_spv, error)) {
    return false;
  }
  if (!shader_compile(target, GpuShaderStage::fragment, desc.fs, desc.fs_name, prelude.c_str(), &fs_spv, error)) {
    return false;
  }

  b->vs_mod = make_module(dev, vs_spv, error);
  if (!b->vs_mod) { return false; }
  b->fs_mod = make_module(dev, fs_spv, error);
  if (!b->fs_mod) {
    vkDestroyShaderModule(dev->device, b->vs_mod, nullptr);
    b->vs_mod = VK_NULL_HANDLE;
    return false;
  }

  const char* what = "";
  VkResult r = make_layouts(dev, desc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, &b->set_layout,
                            &b->layout, &what);
  if (r != VK_SUCCESS) {
    vkDestroyShaderModule(dev->device, b->vs_mod, nullptr);
    vkDestroyShaderModule(dev->device, b->fs_mod, nullptr);
    b->vs_mod = b->fs_mod = VK_NULL_HANDLE;
    *error = std::string(what) + " failed (" + std::to_string((int)r) + ")";
    return false;
  }

  uint32_t location = 0;
  uint32_t stride = add_attributes(desc.vertex_layout, 0, &location, &b->attrs);
  if (!desc.vertex_layout.empty()) { b->bindings.push_back({0, stride, VK_VERTEX_INPUT_RATE_VERTEX}); }
  stride = add_attributes(desc.instance_layout, 1, &location, &b->attrs);
  if (!desc.instance_layout.empty()) { b->bindings.push_back({1, stride, VK_VERTEX_INPUT_RATE_INSTANCE}); }
  b->vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = (uint32_t)b->bindings.size(),
      .pVertexBindingDescriptions = b->bindings.data(),
      .vertexAttributeDescriptionCount = (uint32_t)b->attrs.size(),
      .pVertexAttributeDescriptions = b->attrs.data(),
  };

  VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if (desc.topology == GpuTopology::lines) {
    topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  } else if (desc.topology == GpuTopology::points) {
    topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  }
  b->ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = topo,
  };
  b->vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
  };
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  if (desc.cull == GpuCull::back) {
    cull = VK_CULL_MODE_BACK_BIT;
  } else if (desc.cull == GpuCull::front) {
    cull = VK_CULL_MODE_FRONT_BIT;
  }
  b->rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = cull,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  b->ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  b->ds = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE,
      .depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
  };
  bool alpha = desc.blend == GpuBlend::alpha;
  b->cba = {
      .blendEnable = alpha ? VK_TRUE : VK_FALSE,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask =
          VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  b->cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &b->cba,
  };
  b->dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
  b->dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
  b->dy = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = b->dyn,
  };
  b->stages[0] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_VERTEX_BIT,
                  .module = b->vs_mod,
                  .pName = "main"};
  b->stages[1] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                  .module = b->fs_mod,
                  .pName = "main"};
  b->info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = b->stages,
      .pVertexInputState = &b->vi,
      .pInputAssemblyState = &b->ia,
      .pViewportState = &b->vp,
      .pRasterizationState = &b->rs,
      .pMultisampleState = &b->ms,
      .pDepthStencilState = has_depth ? &b->ds : nullptr,
      .pColorBlendState = &b->cb,
      .pDynamicState = &b->dy,
      .layout = b->layout,
  };
  return true;
}

bool graphics_pipeline_end(GpuDevice* dev, const GpuPipelineDesc& desc, GraphicsPipelineBuild* b, VkResult r,
                           VkPipeline pipeline, GpuPipeline* out, std::string* error) {
  vkDestroyShaderModule(dev->device, b->vs_mod, nullptr);
  vkDestroyShaderModule(dev->device, b->fs_mod, nullptr);
  if (r != VK_SUCCESS) {
    if (b->layout) { vkDestroyPipelineLayout(dev->device, b->layout, nullptr); }
    if (b->set_layout) { vkDestroyDescriptorSetLayout(dev->device, b->set_layout, nullptr); }
    *error = "vkCreateGraphicsPipelines failed (" + std::to_string((int)r) + ")";
    return false;
  }
  out->h = pipeline;
  out->layout = b->layout;
  out->push = desc.push;
  out->set_layout = b->set_layout;
  out->bindings = desc.bindings;
  return true;
}

} // namespace vkc::part
