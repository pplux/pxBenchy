#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gpu/vk.h"
#include "lua/bind_gpu.h"
#include "lua/gpu_api.h"
#include "lua/vm.h"
#include "timers.h"

struct GpuUi;

// One benchmark run: the Lua VM, its GpuLua and Timers, from init to the result. Driven by the session (run_script in
// session.cpp), which owns the device, swapchain, UI and platform events. The run owns the frames: it opens one before
// it starts / resumes the script, and ends it (HUD, screenshot, offscreen copy, windows) after the script yields in
// gpu.present or returns. When the swapchain cannot provide a frame the script is not resumed: the session recreates
// the swapchain between frames and the next run_step tries again. The run decides when it ends; a script still in
// gpu.present is then resumed once more without a frame (gpu.present returns false: run_result).

// Per run (run_script). The menu's "Run all" uses the same options for every script.
struct RunOptions {
  long long frames = 3600;      // 0 = run indefinitely
  long long frames_warmup = 60; // presents left out of the stats before measuring; at least 1 (frame 1, the setup)
  long long time_limit = 0;     // seconds of measured frames (after warm-up) before the run ends normally, 0 = off
  bool hud = true;
  bool offscreen = false;
  std::string screenshot; // "" = off
  long long screenshot_frame = 1;
  std::string script;
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
  std::string text; // timers_report table + run report lines
  std::string json; // run report JSON line, newline-terminated
  // with the reports: measured frames (after warm-up), their fps, timers_summary (menu results table)
  long long frames = 0;
  double fps = 0.0;
  std::vector<TimerSummary> timers;
};

// One 1 s wall window of the measured range (the last one ends with the run, shorter): feeds the HUD (last window) and
// the report's series.
struct RunWindow {
  float t_s = 0.0f;       // window end, s since measuring began
  long long presents = 0; // presents at the window end
  float headroom = 0.0f;  // thermal_latest() at the window end
  float status = 0.0f;
  TimerMark timers;
};

struct Run {
  RunOptions ro;
  const GpuApiTable* gpu = nullptr; // gpu_api_table(api)
  Timers timers;                    // outlives g (bind_gpu_shutdown shuts down timers.gpu)
  GpuLua g;
  GpuUi* ui = nullptr;              // HUD drawing (gpu_ui_init done)
  GpuPassCtx hud_pass;              // HUD pass, on the frame cmd or the offscreen blit cmd
  lua_State* L = nullptr;           // null when vm_create failed
  VmState st = VmState::error;      // after the last vm_start / vm_resume
  bool started = false;             // vm_start done (deferred while frame 1 cannot be opened)
  long long frames_unavailable = 0; // frame opens that failed (swapchain out of date / gone): the script not resumed
  int64_t start_ns = 0;             // wall time at init (HUD elapsed)
  int64_t window_t0 = 0;            // current window start
  std::vector<RunWindow> windows;   // since measuring began (warm-up windows dropped then)
  int64_t last_present_ns = 0;      // offscreen: wall time of the last periodic present
  long long offscreen_presents = 0; // offscreen: blits since measuring began
  int blit_in = 0;                  // offscreen: presents until the captured frame is blitted (0 = no capture pending)
  int since_copy = -1;              // offscreen: presents since the last copy to the screen (-1 = none pending)
  long long presents = 0;
  int64_t measure_start_ns = 0;         // wall time when measuring began (after the warm-up presents)
  long long measure_start_presents = 0; // presents at that point
  int64_t measure_end_ns = 0;           // wall time when the script ended (run_result)
  bool screenshot_taken = false;        // written after gpu.present (ro.screenshot): the run ends
  bool time_limited = false;            // run_step ended the run at ro.time_limit (report)
  bool complete = false;                // the warm-up + ro.frames presents done (ro.frames > 0): the run ends
};

// After the session's swapchain is set up: bind_gpu_init, Lua VM with the gpu / pxb bindings, then opens frame 1 and
// vm_start (the script runs to its first gpu.present; setup records into frame 1), then ends the frame. When frame 1
// cannot be opened, vm_start waits for the first run_step that opens one. False when bind_gpu_init failed; nothing is
// left then (no run_shutdown). A failed vm_create still returns true, with st = error.
bool run_init(Run* r, const RunOptions& ro, GpuApi api, GpuDevice* dev, GpuSwapchain* sc, GpuUi* ui);
// True while the script is between frames (yielded in gpu.present, or not started yet), no screenshot was taken, the
// time limit was not reached and the run is not complete (warm-up + ro.frames presents).
bool run_active(const Run* r);
// Opens a frame, resumes the script to its next gpu.present (or end), ends the frame; returns run_active. A frame that
// cannot be opened is counted (frames_unavailable), breaks the timer intervals, and the script is not resumed. Ends
// the run after the warm-up + ro.frames presents (complete) or once the measured frames reach ro.time_limit
// (time_limited).
bool run_step(Run* r);
// Between frames, for the session's frame loop:
void run_wait_idle(Run* r);           // device idle, deferred destroys done: before swapchain / surface changes
void run_swapchain_recreated(Run* r); // the offscreen capture went with the old swapchain
void run_paused(Run* r, int64_t ns);  // ns without frames: clocks move forward, intervals spanning it dropped
// After the last step: resumes a script still in gpu.present once more, without a frame (gpu.present returns false, not
// measured), waits idle, resolves the timers, then fills res (ok: a normal end and the script returned; time_limited;
// reports unless a screenshot run).
void run_result(Run* r, RunResult* res);
// After run_result (only when run_init succeeded): destroys the Lua VM, then GpuLua and the HUD pass; leaves the device
// idle.
void run_shutdown(Run* r);
