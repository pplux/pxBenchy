#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

constexpr uint32_t PXB_EV_RESIZED = 1u << 0;
constexpr uint32_t PXB_EV_SURFACE_LOST = 1u << 1;
constexpr uint32_t PXB_EV_QUIT = 1u << 2;
constexpr uint32_t PXB_EV_BACK = 1u << 3; // Android Back key / gesture, macOS Escape: stop the run, leave the menu

bool platform_init(int width, int height, const char* title); // creates window (macOS) / waits first window (Android)
void platform_shutdown();
void* platform_surface_handle(); // CAMetalLayer* (macOS) | ANativeWindow* (Android) | nullptr while lost
void platform_drawable_size(uint32_t* w, uint32_t* h); // pixels (macOS: bounds * backingScaleFactor)
uint32_t platform_pump();                              // non-blocking; bitmask of PXB_EV_*
bool platform_wait_surface();                          // blocks pumping until surface ready (true) or quit (false)
// False while the app is in the background: draw nothing, keep pumping (a hidden window may never return swapchain
// images, so acquire would block). Android: from APP_CMD_PAUSE to APP_CMD_RESUME | macOS: always true.
bool platform_visible();
bool platform_read_file(const char* path, std::string* out); // whole file; filesystem (macOS) | APK assets (Android)
// `scripts/<name>.lua` paths (platform_read_file paths), sorted by name: directory `scripts` relative to the working
// directory (macOS) | APK assets dir `scripts` (Android). Empty when the directory is missing.
std::vector<std::string> platform_list_scripts();

// Device identification for reports: ordered (key, value) pairs, keys in snake_case, empty values left out; any thread.
// First pair ("os", "android" | "macos"). Android (system properties): manufacturer, model, soc_manufacturer,
// soc_model, build_fingerprint, sdk, os_version (ro.build.version.release). macOS: model (sysctl hw.model), os_version
// (NSProcessInfo operatingSystemVersionString).
std::vector<std::pair<std::string, std::string>> platform_device_info();

// Saves a results file (`name` = file name, no directory; `json` = contents) where the user can reach it; main thread.
// macOS: `results/<name>` relative to the working directory (directory created; an existing file is not overwritten:
// `-1`, `-2`, ... before the extension). Android: MediaStore Files `Documents/pxBenchy/<name>` (application/json,
// IS_PENDING while writing; MediaStore renames on a name clash), no storage permission (API 29+). *where = the path
// (macOS) | the `content://` URI (Android, for sharing). False on failure (reason on stderr).
bool platform_save_results(const std::string& name, const std::string& json, std::string* where);

// Sharing a results file (main thread). *msg = short status on success ("share sheet opened") or the reason on failure.
// Android (JNI on the activity, no Java code): share_file = ACTION_SEND chooser with EXTRA_STREAM = `where` (the
// content:// URI from platform_save_results, read permission granted to the target); share_text = ACTION_SEND
// chooser with EXTRA_TEXT; copy_text = ClipboardManager.setPrimaryClip (plain text). Text above PXB_SHARE_TEXT_MAX
// fails (Binder transaction limit, ~1 MB shared per process). macOS: share_file = reveal `where` in Finder;
// share_text = copy_text = general pasteboard (no text share sheet without an NSView anchor).
constexpr size_t PXB_SHARE_TEXT_MAX = 500 * 1024;
bool platform_share_file(const std::string& where, std::string* msg);
bool platform_share_text(const std::string& text, std::string* msg);
bool platform_copy_text(const std::string& text, std::string* msg);

// Update check (menu). http_get: HTTPS GET, blocking, any thread; true on HTTP 200 with *body = the response (at most
// PXB_HTTP_BODY_MAX bytes), else false with *err = the reason. open_url: url in the browser, main thread; *msg = status
// or reason. Android: java.net.HttpURLConnection (5 s timeouts) / ACTION_VIEW, over JNI | macOS: not supported (false).
constexpr size_t PXB_HTTP_BODY_MAX = 1024 * 1024;
bool platform_http_get(const std::string& url, std::string* body, std::string* err);
bool platform_open_url(const std::string& url, std::string* msg);

// UI scale from the display density: Android AConfiguration_getDensity / 160 | macOS window backingScaleFactor.
float platform_ui_scale();

// Pixels at each edge of the view covered by system UI, in the view's space; main thread. Android: root window
// insets of the system bars + display cutout (e.g. the Samsung tablet taskbar, which the fullscreen theme does not
// hide), 0 when unknown | macOS: 0.
struct PlatformInsets {
  float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};
PlatformInsets platform_safe_insets();

// Pointer input for the UI, queued by platform_pump. Positions in pixels of the view as the app sees it (origin
// top-left, the swapchain's view_extent space: Android touch coordinates are already in the rotated window space).
// Touch (Android, first finger only): down = pos + button 0 down, up = pos + button 0 up + leave, so nothing stays
// hovered; cancel = leave + button 0 up (no click). Mouse (macOS): pos on move/drag, buttons 0 left, 1 right, 2 middle,
// wheel (x, y = horizontal, vertical steps, positive = left / up), leave when the cursor exits the view.
struct PlatformPointerEvent {
  enum Kind : uint8_t { pos, button, wheel, leave };
  Kind kind = pos;
  bool touch = false; // event from a touch screen (ImGuiMouseSource_TouchScreen)
  bool down = false;  // button: pressed (true) | released
  int button_index = 0;
  float x = 0.0f, y = 0.0f; // pos: pixels | wheel: steps
};
// Moves the queued events (oldest first) into *out, replacing its contents, and empties the queue.
void platform_pointer_events(std::vector<PlatformPointerEvent>* out);
// Platform implementations only: appends to the queue (consecutive pos events coalesce; bounded size).
void platform_pointer_push(const PlatformPointerEvent& e);

// Thermal state; fields a platform cannot read stay 0. Blocking (JNI on Android): call from the thermal thread only.
struct PlatformThermal {
  float headroom = 0.0f;  // Android: AThermal_getThermalHeadroom (1.0 = severe throttling)
  float status = 0.0f;    // Android: AThermal_getCurrentThermalStatus 0-6 | macOS: NSProcessInfo thermalState 0-3
  float battery_c = 0.0f; // Android: ACTION_BATTERY_CHANGED temperature, degrees C
#ifdef TRACY_ENABLE
  // Tracy plots only (not readable on every device): Android sysfs, 0 when not readable. GPU clock (Mali
  // mali0/device/cur_freq, Adreno kgsl-3d0/devfreq/cur_freq) and the CPU clusters' clocks (cpufreq policies in order,
  // little cores first), MHz
  float gpu_mhz = 0.0f;
  float cpu_mhz[4] = {};
#endif
};
void platform_thermal(PlatformThermal* out);
// PlatformThermal::status as a word: Android none / light / moderate / severe / critical / emergency / shutdown (0-6)
// | macOS nominal / fair / serious / critical (0-3); "?" outside the range.
const char* platform_thermal_status_name(float status);
