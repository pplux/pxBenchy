#include "gpu/vkc/shader.h"
#include <cstdlib>
#include <cstring>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace vkc {

bool shader_init() { return glslang::InitializeProcess(); }

void shader_shutdown() { glslang::FinalizeProcess(); }

static EShLanguage to_lang(GpuShaderStage stage) {
  switch (stage) {
    case GpuShaderStage::vertex: return EShLangVertex;
    case GpuShaderStage::fragment: return EShLangFragment;
    case GpuShaderStage::compute: return EShLangCompute;
  }
  return EShLangVertex;
}

// Returns the 1-based line `n` of src, empty if out of range.
static std::string source_line(const char* src, int n) {
  const char* p = src;
  for (int i = 1; i < n; i++) {
    p = strchr(p, '\n');
    if (!p) return std::string();
    p++;
  }
  const char* e = p;
  while (*e && *e != '\n' && *e != '\r') e++;
  return std::string(p, e);
}

// Rewrites glslang log text (see header for format) and appends it to *out.
static void format_log(const char* log, const char* src, const char* name, std::string* out) {
  const char* p = log;
  while (*p) {
    const char* e = strchr(p, '\n');
    std::string line = e ? std::string(p, e) : std::string(p);
    p = e ? e + 1 : p + line.size();
    if (line.empty()) continue;
    const char* l = line.c_str();
    const char* rest = nullptr;
    if (strncmp(l, "ERROR: 0:", 9) == 0) rest = l + 9;
    else if (strncmp(l, "WARNING: 0:", 11) == 0) rest = l + 11;
    char* end = nullptr;
    long n = rest ? strtol(rest, &end, 10) : 0;
    if (rest && end != rest && *end == ':') {
      *out += std::string(name) + ":" + std::to_string(n) + ":" + (end + 1) + "\n";
      std::string s = source_line(src, (int)n);
      if (!s.empty()) *out += "    " + s + "\n";
    } else if (strncmp(l, "ERROR: ", 7) == 0 && strstr(l, "compilation errors.")) {
      continue;
    } else {
      *out += line + "\n";
    }
  }
}

bool shader_compile(ShaderTarget target, GpuShaderStage stage, const char* source, const char* name,
                    const char* preamble, std::vector<uint32_t>* spirv, std::string* error) {
  EShLanguage lang = to_lang(stage);
  glslang::TShader shader(lang);
  shader.setStrings(&source, 1);
  if (preamble && *preamble) shader.setPreamble(preamble);
  shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
  shader.setEnvClient(glslang::EShClientVulkan, (glslang::EShTargetClientVersion)target.vulkan);
  shader.setEnvTarget(glslang::EShTargetSpv, (glslang::EShTargetLanguageVersion)target.spirv);
  EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(GetDefaultResources(), 450, false, msgs)) {
    error->clear();
    format_log(shader.getInfoLog(), source, name, error);
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(msgs)) {
    error->clear();
    format_log(program.getInfoLog(), source, name, error);
    return false;
  }
  spirv->clear();
  glslang::GlslangToSpv(*program.getIntermediate(lang), *spirv);
  return true;
}

} // namespace vkc
