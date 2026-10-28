#pragma once
#include <cstdint>
#include <string>

constexpr uint32_t PXB_EV_RESIZED      = 1u << 0;
constexpr uint32_t PXB_EV_SURFACE_LOST = 1u << 1;
constexpr uint32_t PXB_EV_QUIT         = 1u << 2;

bool     platform_init(int width, int height, const char* title); // creates window (macOS) / waits first window (Android)
void     platform_shutdown();
void*    platform_surface_handle();                // CAMetalLayer* (macOS) | ANativeWindow* (Android) | nullptr while lost
void     platform_drawable_size(uint32_t* w, uint32_t* h);  // pixels (macOS: bounds * backingScaleFactor)
uint32_t platform_pump();                          // non-blocking; bitmask PXB_EV_RESIZED | PXB_EV_SURFACE_LOST | PXB_EV_QUIT
bool     platform_wait_surface();                  // blocks pumping until surface ready (true) or quit (false)
bool     platform_read_file(const char* path, std::string* out); // whole file; filesystem (macOS) | APK assets (Android)

// Thermal state; fields a platform cannot read stay 0. Blocking (JNI on Android): call from the thermal thread only.
struct PlatformThermal {
  float headroom = 0.0f;  // Android: AThermal_getThermalHeadroom (1.0 = severe throttling)
  float status = 0.0f;    // Android: AThermal_getCurrentThermalStatus 0-6 | macOS: NSProcessInfo thermalState 0-3
  float battery_c = 0.0f; // Android: ACTION_BATTERY_CHANGED temperature, degrees C
};
void     platform_thermal(PlatformThermal* out);
