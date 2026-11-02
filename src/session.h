#pragma once
#include <functional>
#include <string>
#include <vector>
#include "gpu/vk.h"
#include "lua/bind_gpu.h"
#include "ui.h"
#include "lua/gpu_api.h"
#include "lua/timers_report.h"

// Device session and benchmark runs, shared by the command line (main.cpp) and the menu. Single-threaded: everything
// runs on the main (platform) thread.

// Per run (run_script). The menu's "Run all" uses the same options for every script.
struct RunOptions {
  long long frames = 6000; // 0 = run indefinitely
  long long frames_warmup = 60;
  long long time_limit = 0; // seconds of measured frames (after warm-up) before the run ends normally, 0 = off
  bool hud = true;
  bool offscreen = false;
  std::string screenshot; // "" = off
  long long screenshot_frame = 1;
  std::string script;
};

// Lives as long as the window: window / surface handle, thermal thread, ImGui context (created by session_init). Per
// device (session_init, recreated by session_switch): device + surface, shader compiler, swapchain
// (reused or recreated by each run and by session_ui_frame), the ImGui renderer and the UI pass context. Runs leave
// nothing else behind.
struct Session {
  GpuApi api = GpuApi::vk11;
  bool validation = false;
  const GpuApiTable* gpu = nullptr; // gpu_api_table(api)
  GpuDevice dev;
  bool dev_ready = false; // device + shader compiler up (false after a failed switch)
  GpuSwapchain sc;
  bool sc_ready = false;   // sc created; its offscreen / readable are fixed until it is destroyed
  GpuUi ui;                // ImGui context (whole session) + renderer (per device)
  bool ui_ready = false;   // renderer registered for this device (gpu_ui_init) and current sc format
  GpuPassCtx pass;         // session_ui_frame's pass (no depth)
  bool quit = false;       // sticky: the window was closed / app destroyed (run or session_pump)
  std::string vulkan_info; // gpu_vulkan_info_json, queried at startup (empty: none)
};

// What a run produced. Reports are empty when the run ended at a screenshot or never started.
struct RunResult {
  std::string script;
  bool ok = false;
  bool quit = false;         // the window was closed / app destroyed during the run (Run all stops)
  bool stopped = false;      // Back during the run (Run all stops)
  bool time_limited = false; // ended by RunOptions::time_limit (a normal end, with reports)
  GpuApi api = GpuApi::vk11;
  bool validation = false;
  bool offscreen = false;
  std::string text; // timers_report table + bind_gpu_report lines
  std::string json; // bind_gpu_report_json line, newline-terminated
  // with the reports: measured frames (after warm-up), their fps, timers_summary (menu results table)
  long long frames = 0;
  double fps = 0.0;
  std::vector<TimerSummary> timers;
};

// Platform window, thermal thread, then the device (api, validation): device + surface, shader compiler, and the
// `device:` line on stdout. On failure everything created is torn down.
bool session_init(Session* s, GpuApi api, bool validation);
// Device idle (no run active). Tears down the renderer, swapchain, shader compiler and device + surface, then creates
// them for (api, validation) on the same window; the ImGui context and thermal thread survive. No-op when both match
// and the device is up. On failure tries to restore the previous api / validation and returns false; if that fails
// too, dev_ready stays false (runs fail, session_switch may be retried, session_shutdown still works). Sets quit when
// the app is destroyed while waiting for its window (Android, between device down and up).
bool session_switch(Session* s, GpuApi api, bool validation);
void session_shutdown(Session* s);

// No run active, device idle: platform_pump, then a resize drops the swapchain, a surface loss also the surface (waits
// for the new window and recreates the surface; the next run or UI frame recreates the swapchain). Sets quit on QUIT
// (also when the wait for a new window ends in a quit). Returns the PXB_EV_* bits.
uint32_t session_pump(Session* s);
// No run active, device idle: while the app is in the background (platform_visible false), session_pump every 16 ms
// and draw nothing (a hidden window may never return swapchain images). Returns the PXB_EV_* bits seen; QUIT or BACK
// end the wait early.
uint32_t session_wait_visible(Session* s);

// No run active, device idle: one frame on a regular (not offscreen) swapchain, created or recreated as needed: clear,
// then the ImGui frame `build` makes between NewFrame and Render (DisplaySize = view_extent; queued input must already
// be fed with gpu_ui_input). Waits idle before returning, so the device stays idle between frames. False when no
// frame was drawn (no device, zero extent, failure); a swapchain that needs recreating is dropped and rebuilt by the
// next call.
bool session_ui_frame(Session* s, const std::function<void()>& build);

// One script: swapchain (reused or recreated), GpuLua, TimerSet and Lua VM, all destroyed before returning with the
// device idle. Prints nothing but errors and the script's own output; the caller prints the reports. Pointer input
// queued during the run is dropped (it must not click in the next UI frame).
RunResult run_script(Session* s, const RunOptions& ro);

// Result file: `{"pxbenchy":1,"device":<device_info_json>,"vulkan":<s->vulkan_info>,"runs":[<run JSON>, ...]}`,
// one run per line (`vulkan` multi-line, left out when empty). `runs`
// are RunResult::json strings (empty ones skipped). `device` describes s->dev (null when no device is up); each run
// also carries its own `device_info`, so runs from before a Vulkan switch keep theirs. Saved with
// platform_save_results as `<model>-<yyyymmdd-hhmmss>.json` (local time; model from platform_device_info, characters
// outside [A-Za-z0-9._-] replaced by `_`). False when nothing to save or the save failed; *where as
// platform_save_results.
std::string results_json(const Session* s, const std::vector<std::string>& runs);
bool results_save(const Session* s, const std::vector<std::string>& runs, std::string* where);
