#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gpu/vk.h"
#include "lua/gpu_api.h"

struct lua_State;
struct TimerSet;

// State shared by the gpu.* Lua bindings. Owned by app_main, outlives the Lua state (its __gc enqueues into `dead`).
struct GpuLua {
  GpuApi api = GpuApi::vk13;
  const GpuApiTable* gpu = nullptr;   // gpu_api_table(api): every gpu call goes through it
  GpuDevice* dev = nullptr;
  GpuSwapchain* sc = nullptr;
  GpuPassCtx pass;
  GpuTimers timers;
  GpuText text;
  GpuDescriptorPools descriptors;     // transient sets, reset per frame slot in ensure_frame
  TimerSet* ts = nullptr;             // CPU timers + cpu interval, cleared with the GPU timers after warm-up
  long long frames_warmup = 0;        // gpu.present count that triggers the warm-up reset (0 = none)
  long long frames = 0;               // --frames (0 = run indefinitely), for the HUD
  bool hud = true;                    // draw the HUD at each present (--no-hud turns it off)
  int64_t start_ns = 0;               // wall time at init (HUD elapsed)
  int64_t fps_t0 = 0;                 // HUD fps window start
  long long fps_frames = 0;           // presents in the current window
  double fps = 0.0;                   // last completed 1 s window
  int64_t last_present_ns = 0;        // offscreen: wall time of the last periodic present
  long long offscreen_presents = 0;   // offscreen: blits since the warm-up reset
  float headroom_start = 0.0f;        // thermal headroom when measuring starts (warm-up reset, or init without warm-up)
  int blit_in = 0;                    // offscreen: presents until the captured frame is blitted (0 = no capture pending)
  long long presents = 0;
  int64_t measure_start_ns = 0;       // wall time when measuring began (warm-up reset, or init without warm-up)
  long long measure_start_presents = 0; // presents at that point
  int64_t measure_end_ns = 0;         // wall time when the script ended (bind_gpu_finish)
  bool frame_open = false;            // frame_begin + timers_frame_begin done, not yet ended
  bool skipped = false;               // frame_begin failed: ops are no-ops until the next present
  bool in_pass = false;               // a pass is open (real, or virtual on a skipped frame)
  bool quit = false;                  // sticky: platform QUIT seen by gpu.present
  std::string screenshot;             // --screenshot path ("" = off)
  long long screenshot_frame = 1;     // --screenshot-frame: first open frame with presents + 1 >= this is captured
  bool screenshot_taken = false;      // written: gpu.present stopped the script (vm_stop)
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint64_t opens = 0;                 // successful frame opens so far
  enum class DeadKind { buffer, pipeline, texture };
  struct Dead {
    DeadKind kind;
    void* p;                          // GpuBuffer* (also upload staging) | GpuPipeline* | GpuTexture*
    uint64_t tag;                     // `opens` at enqueue time
  };
  std::vector<Dead> dead;             // deferred destroy queue
  // Textures written outside a pass (upload, blit dst, dispatch image) since the last begin_pass, which brings all their
  // mips to shader-read (fragment) before the pass starts. Garbage-collected textures are removed.
  std::vector<GpuTexture*> dirty;
};

// Timers + text init; call after swapchain create.
bool bind_gpu_init(GpuLua* g, GpuApi api, GpuDevice* dev, GpuSwapchain* sc, TimerSet* ts, long long frames,
                   long long frames_warmup, bool hud);
void bind_gpu_register(lua_State* L, GpuLua* g);                 // between vm_create and vm_run
// After vm_run, before the report: ends open pass/frame, waits idle, resolves the GPU timers.
void bind_gpu_finish(GpuLua* g);
// After timers_report: prints `run: N frames in T s = F fps, display H Hz (refresh X ms)` (`display -` when unknown)
// for the measured frames, plus `offscreen presents: N (blits, spanning intervals excluded)` in offscreen mode,
// then `thermal headroom: start S end E (delta D)` (start = when measuring began, 0 where unsupported).
void bind_gpu_report(const GpuLua* g);
// --json: the whole report (run info, measured frames / time / fps, offscreen presents, thermal headroom start/end,
// timers_report_json) as one JSON line.
void bind_gpu_report_json(const GpuLua* g, const char* script);
// After vm_destroy: waits idle, flushes the delete queue, shuts down pass ctx, timers, text and descriptor pools.
void bind_gpu_shutdown(GpuLua* g);
