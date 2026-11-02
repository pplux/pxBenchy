#pragma once
#include <string>

// JSON text helpers shared by the reports (src/lua), device_info and the Vulkan info (src/gpu/vkc/info).
std::string json_string(const std::string& s); // quoted + escaped
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void str_appendf(std::string* o, const char* fmt, ...); // printf appended to *o
