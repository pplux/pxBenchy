#include "device_info.h"
#include <cstdio>
#include "git_version.h"
#include "json.h"
#include "platform/platform.h"

// Driver-specific driverVersion packing where known: MoltenVK major * 10000 + minor * 100 + patch (by driverID, or
// the Apple vendor ID when driver properties are missing); NVIDIA 10.8.8.6 bits. Intel on Windows (18.14) does not
// apply (macOS / Android only). Everything else in the VK_MAKE_VERSION layout (10.10.12; Qualcomm sets bit 31, so its
// major shows as 512+), as vulkan.gpuinfo.org shows most vendors; not every driver follows it.
static std::string driver_version_string(const GpuDevice* dev) {
  uint32_t vendor = dev->props.vendorID, v = dev->props.driverVersion;
  bool moltenvk = dev->driver_props ? dev->driver.driverID == VK_DRIVER_ID_MOLTENVK : vendor == 0x106B;
  char buf[64];
  if (moltenvk) {
    snprintf(buf, sizeof(buf), "%u.%u.%u", v / 10000, v / 100 % 100, v % 100);
  } else if (vendor == 0x10DE) {
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v >> 22, (v >> 14) & 0xff, (v >> 6) & 0xff, v & 0x3f);
  } else {
    snprintf(buf, sizeof(buf), "%u.%u.%u", v >> 22, (v >> 12) & 0x3ff, v & 0xfff);
  }
  return buf;
}

static const char* device_type_name(VkPhysicalDeviceType t) {
  switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
  }
}

std::string device_info_json(const GpuDevice* dev) {
  const VkPhysicalDeviceProperties& p = dev->props;
  std::string o = "{\"name\":" + json_string(p.deviceName) + ",\"type\":\"" + device_type_name(p.deviceType) + "\"";
  uint32_t v = p.apiVersion;
  str_appendf(&o, ",\"vendor_id\":%u,\"device_id\":%u,\"api_version\":\"%u.%u.%u\"", p.vendorID, p.deviceID,
              VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
  str_appendf(&o, ",\"driver_version\":%u,\"driver_version_decoded\":\"%s\"", p.driverVersion,
              driver_version_string(dev).c_str());
  if (dev->driver_props) {
    const VkPhysicalDeviceDriverProperties& d = dev->driver;
    str_appendf(&o, ",\"driver_id\":%u", (unsigned)d.driverID);
    o += ",\"driver_name\":" + json_string(d.driverName) + ",\"driver_info\":" + json_string(d.driverInfo);
    str_appendf(&o, ",\"driver_conformance\":\"%u.%u.%u.%u\"", d.conformanceVersion.major, d.conformanceVersion.minor,
                d.conformanceVersion.subminor, d.conformanceVersion.patch);
  }
  for (const auto& kv : platform_device_info()) { o += "," + json_string(kv.first) + ":" + json_string(kv.second); }
  o += ",\"git\":" + json_string(PXB_GIT_HASH) + ",\"version\":" + json_string(PXB_VERSION) +
       ",\"build_type\":" + json_string(PXB_BUILD_TYPE);
#ifdef TRACY_ENABLE
  o += ",\"tracy\":true}";
#else
  o += ",\"tracy\":false}";
#endif
  return o;
}

std::string device_info_summary(const GpuDevice* dev) {
  std::string o = dev->props.deviceName;
  if (dev->driver_props && dev->driver.driverName[0]) {
    o += std::string("  ") + dev->driver.driverName;
  } else {
    o += "  driver";
  }
  o += " " + driver_version_string(dev);
  std::string os;
  for (const auto& kv : platform_device_info()) {
    if (kv.first == "os") { os = kv.second == "android" ? "Android " : "macOS "; }
    if (kv.first == "model") { o += "  " + kv.second; }
    if (kv.first == "os_version") { o += "  " + os + kv.second; }
  }
  return o;
}
