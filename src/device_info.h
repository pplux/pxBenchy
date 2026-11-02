#pragma once
#include <string>
#include "gpu/vk.h"

// The `device_info` JSON object (no newline) of an initialized device: Vulkan properties (name, type, vendor / device
// id, api version, raw + decoded driver version, VkPhysicalDeviceDriverProperties fields when available), the
// platform_device_info pairs and the git hash. bind_gpu_report_json embeds it; also usable once per session.
std::string device_info_json(const GpuDevice* dev);
// One line for UI headers: `<deviceName>  <driverName | driver> <decoded driver version>  <model>  <Android | macOS>
// <os_version>` (unknown platform parts left out).
std::string device_info_summary(const GpuDevice* dev);
