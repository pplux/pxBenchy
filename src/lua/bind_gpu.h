#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include "gpu/vk.h"
#include "lua/gpu_api.h"
#include "timers.h"

struct lua_State;

// State shared by the gpu.* Lua bindings. One per run (run_script in session.cpp), outlives the Lua state (its __gc
// enqueues into `dead`).
struct GpuLua {
  GpuApi api = GpuApi::vk13;
  const GpuApiTable* gpu = nullptr; // gpu_api_table(api): every gpu call goes through it
  GpuDevice* dev = nullptr;
  GpuSwapchain* sc = nullptr;
  GpuPassCtx pass;
  GpuDescriptorPools descriptors;       // transient sets, reset per frame slot when the run opens a frame
  Timers* timers = nullptr;             // all timers of the run (owned by the run), cleared after warm-up
  bool frame_open = false;              // frame_begin done, not yet ended (set by the run); false at the final resume
  bool in_pass = false;                 // a pass is open
  VkCommandBuffer cmd = VK_NULL_HANDLE; // the open frame's, set by the run
  uint64_t opens = 0;                   // successful frame opens so far
  struct Dead {
    std::variant<GpuBuffer, GpuPipeline, GpuTexture> obj; // GpuBuffer also for upload staging
    uint64_t tag;                                         // `opens` at enqueue time
  };
  std::vector<Dead> dead; // deferred destroy queue
  // Textures written outside a pass (upload, blit dst, dispatch image) since the last begin_pass, which brings all
  // their mips to shader-read (fragment) before the pass starts. Garbage-collected textures are removed.
  std::vector<GpuTexture*> dirty;
};

// Timers init; call after swapchain create.
bool bind_gpu_init(GpuLua* g, GpuApi api, GpuDevice* dev, GpuSwapchain* sc, Timers* timers);
void bind_gpu_register(lua_State* L, GpuLua* g); // between vm_create and vm_start
// After the run's successful frame_begin (frame_open and cmd set): counts the open, frees deferred destroys whose
// frames completed.
void bind_gpu_frame_opened(GpuLua* g);
// Device idle, deferred destroys done: before swapchain / surface changes, the screenshot write and the report.
void bind_gpu_wait_idle(GpuLua* g);
// After vm_destroy: waits idle, flushes the delete queue, shuts down pass ctx, timers and descriptor pools.
void bind_gpu_shutdown(GpuLua* g);
