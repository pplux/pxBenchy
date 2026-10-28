#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gpu/vk.h"

namespace vkc {

// Compile target, numerically as glslang's: vulkan = VK_API_VERSION_X_Y, spirv = (major << 16) | (minor << 8).
struct ShaderTarget {
  uint32_t vulkan;
  uint32_t spirv;
};

// Wraps glslang::InitializeProcess / FinalizeProcess. Call init once before any compile, shutdown once after.
bool shader_init();
void shader_shutdown();

// Compiles GLSL (default #version 450, entry point main) to SPIR-V for target. No printing.
// preamble (nullptr/empty = none) is injected before the source via glslang setPreamble; the source may still
// start with its own #version, and reported line numbers refer to the source alone.
// On success fills *spirv and returns true. On failure returns false and sets *error to text where each
// glslang "ERROR: 0:<line>: <msg>" line becomes "<name>:<line>: <msg>" followed by the source line,
// indented by 4 spaces. Other glslang lines (e.g. link errors) pass through; the summary line is dropped.
bool shader_compile(ShaderTarget target, GpuShaderStage stage, const char* source, const char* name,
                    const char* preamble, std::vector<uint32_t>* spirv, std::string* error);

} // namespace vkc
