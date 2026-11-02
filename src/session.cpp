#include "session.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include "device_info.h"
#include "platform/platform.h"
#include "platform/thermal.h"
#include "lua/vm.h"
#include "lua/bind_pxb.h"
#include "lua/timers_report.h"
#include <imgui.h>
#include <tracy/Tracy.hpp>

// Device + surface and shader compiler for (api, validation), plus the `device:` line. On failure nothing is left.
static bool device_up(Session* s, GpuApi api, bool validation) {
  s->dev = GpuDevice{};
  s->api = api;
  s->validation = validation;
  s->gpu = gpu_api_table(api);
  if (!s->gpu->device_init(&s->dev, validation, platform_surface_handle())) { // shuts down what it created
    fprintf(stderr, "error: gpu_device_init failed\n");
    return false;
  }
  if (!s->gpu->shader_init()) {
    fprintf(stderr, "error: gpu_shader_init failed\n");
    s->gpu->device_shutdown(&s->dev);
    return false;
  }
  uint32_t v = s->dev.props.apiVersion;
  printf("device: %s api=%u.%u.%u queue_family=%u validation=%s display_timing=%s\n", s->dev.props.deviceName,
         VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), s->dev.queue_family,
         s->dev.validation ? "on" : "off", s->dev.display_timing ? "yes" : "no");
  fflush(stdout);
  s->dev_ready = true;
  return true;
}

static void swapchain_drop(Session* s) {
  s->gpu->swapchain_destroy(&s->dev, &s->sc);
  s->sc = GpuSwapchain{};
  s->sc_ready = false;
}

// Everything per device, in reverse order; the ImGui context stays. No-op when the device is down.
static void device_down(Session* s) {
  if (!s->dev_ready) { return; }
  {
    ZoneScopedN("wait_idle");
    vkDeviceWaitIdle(s->dev.device);
  }
  gpu_ui_renderer_shutdown(&s->ui, s->gpu, &s->dev);
  s->ui_ready = false;
  s->gpu->pass_shutdown(&s->dev, &s->pass);
  s->pass = GpuPassCtx{};
  swapchain_drop(s);
  s->gpu->shader_shutdown();
  s->gpu->device_shutdown(&s->dev);
  s->dev_ready = false;
}

bool session_init(Session* s, GpuApi api, bool validation) {
  if (!platform_init(1280, 720, "pxBenchy")) {
    fprintf(stderr, "error: platform_init failed\n");
    return false;
  }
  gpu_ui_create(&s->ui);
  if (!device_up(s, api, validation)) {
    gpu_ui_shutdown(&s->ui);
    platform_shutdown();
    return false;
  }
  thermal_start();
  return true;
}

bool session_switch(Session* s, GpuApi api, bool validation) {
  if (s->dev_ready && s->api == api && s->validation == validation) { return true; }
  bool had = s->dev_ready;
  GpuApi prev_api = s->api;
  bool prev_validation = s->validation;
  device_down(s);
  if (!platform_wait_surface()) { // Android: waits while the app is in the background; false = app destroyed
    s->quit = true;
    return false;
  }
  if (device_up(s, api, validation)) { return true; }
  fprintf(stderr, "error: switch to vulkan %s (validation %s) failed\n", gpu_api_name(api), validation ? "on" : "off");
  if (had && device_up(s, prev_api, prev_validation)) {
    fprintf(stderr, "switch: restored vulkan %s (validation %s)\n", gpu_api_name(prev_api),
            prev_validation ? "on" : "off");
  }
  return false;
}

void session_shutdown(Session* s) {
  device_down(s);
  thermal_stop();
  gpu_ui_shutdown(&s->ui);
  platform_shutdown();
}

// Device idle. Keeps the swapchain when offscreen / readable match, recreates it from scratch otherwise (both are fixed
// at the first create). The ImGui renderer is registered with the first swapchain of the device and follows its
// format; on a failure it is shut down, so the next call starts from a clean renderer.
static bool session_swapchain(Session* s, bool offscreen, bool readable) {
  if (s->sc_ready && (s->sc.offscreen != offscreen || s->sc.readable != readable)) { swapchain_drop(s); }
  if (!s->sc_ready) {
    s->sc.offscreen = offscreen;
    s->sc.readable = readable;
    if (!s->gpu->swapchain_create(&s->dev, &s->sc)) {
      swapchain_drop(s);
      return false;
    }
    s->sc_ready = true;
  }
  if (!s->ui_ready) {
    s->ui_ready = gpu_ui_init(&s->ui, s->gpu, &s->dev, s->sc.format.format);
    return s->ui_ready;
  }
  if (!s->sc.swapchain || s->gpu->imgui_set_format(&s->dev, &s->ui.im, s->sc.format.format)) { return true; }
  gpu_ui_renderer_shutdown(&s->ui, s->gpu, &s->dev);
  s->ui_ready = false;
  return false;
}

static uint32_t session_events(Session* s, uint32_t ev);

// Between two frames of a run (the script yielded in gpu.present): window events (swapchain / surface recreated), the
// background pause, Back / quit and the time limit. False ends the run, res says why (nothing set: failure).
static bool run_between_frames(Session* s, GpuLua* g, const RunOptions& ro, RunResult* res) {
  uint32_t ev = platform_pump();
  bool hidden = !platform_visible();
  if (hidden || (ev & (PXB_EV_SURFACE_LOST | PXB_EV_RESIZED)) || s->sc.needs_recreate) {
    bind_gpu_wait_idle(g); // the swapchain or surface may go
  }
  ev = session_events(s, ev);
  if (s->sc_ready && s->sc.needs_recreate) { swapchain_drop(s); }
  if (hidden && !(ev & (PXB_EV_QUIT | PXB_EV_BACK))) { // UNVERIFIED (cannot be exercised on macOS)
    printf("run: paused (app in the background)\n");
    fflush(stdout);
    int64_t t0 = timer_now_ns();
    ev |= session_wait_visible(s);
    int64_t ns = timer_now_ns() - t0;
    bind_gpu_paused(g, ns);
    printf("run: resumed after %.1f s (excluded from the measurement)\n", ns / 1e9);
    fflush(stdout);
  }
  if (ev & PXB_EV_QUIT) {
    printf("run: quit requested\n");
    res->quit = true;
    return false;
  }
  if (ev & PXB_EV_BACK) {
    printf("run: stopped (back)\n");
    res->stopped = true;
    return false;
  }
  if (!s->sc_ready) {
    bind_gpu_swapchain_recreated(g);
    if (!session_swapchain(s, ro.offscreen, !ro.screenshot.empty())) {
      fprintf(stderr, "error: swapchain recreation failed\n");
      return false;
    }
  }
  if (ro.time_limit && bind_gpu_measured_ns(g) >= ro.time_limit * 1000000000LL) {
    res->time_limited = g->time_limited = true;
    return false;
  }
  return true;
}

RunResult run_script(Session* s, const RunOptions& ro) {
  RunResult res;
  res.script = ro.script;
  res.api = s->api;
  res.validation = s->validation;
  res.offscreen = ro.offscreen;
  if (!s->dev_ready) {
    fprintf(stderr, "error: no device (failed vulkan switch)\n");
    return res;
  }
  GpuLua script;
  TimerSet ts;
  bool sc_ok = session_swapchain(s, ro.offscreen, !ro.screenshot.empty());
  if (sc_ok && bind_gpu_init(&script, s->api, &s->dev, &s->sc, &s->ui, &ts, ro.frames, ro.frames_warmup, ro.hud)) {
    lua_State* L = vm_create();
    if (L) {
      bind_gpu_register(L, &script);
      bind_pxb_register(L, &ts, ro.frames, ro.frames_warmup);
    }
    script.path = ro.script;
    script.screenshot = ro.screenshot;
    script.screenshot_frame = ro.screenshot_frame;
    script.time_limit_ns = ro.time_limit * 1000000000LL;
    // the frame loop: the script yields at each gpu.present
    VmState st = L ? vm_start(L, ro.script) : VmState::error;
    while (st == VmState::yield && !script.screenshot_taken && run_between_frames(s, &script, ro, &res)) {
      st = vm_resume(L);
    }
    res.ok = st == VmState::done || (st == VmState::yield && (script.screenshot_taken || res.time_limited));
    bind_gpu_finish(&script);
    if (res.ok && !ro.screenshot.empty() && !script.screenshot_taken) {
      fprintf(stderr, "error: --screenshot: script ended before frame %lld\n", ro.screenshot_frame);
      res.ok = false;
    }
    if (!script.screenshot_taken) { // screenshot runs end without a report
      res.text = timers_report(&ts, &script.timers, s->gpu) + bind_gpu_report(&script);
      res.json = bind_gpu_report_json(&script, ro.script.c_str());
      double secs = 0.0;
      res.frames = bind_gpu_measured(&script, &secs);
      res.fps = secs > 0 ? res.frames / secs : 0.0;
      res.timers = timers_summary(&ts, &script.timers, s->gpu);
    }
    vm_destroy(L);
    bind_gpu_shutdown(&script);
  } else {
    if (sc_ok) {
      bind_gpu_shutdown(&script); // timers_init failed: frees what it created
    }
    fprintf(stderr, "error: swapchain setup failed\n");
  }
  // Touches during the run (HUD frames ignore them; with --no-hud or offscreen they stay queued) must not click in the
  // next UI frame: drop the queued pointer events and ImGui's pending input and mouse state.
  platform_pointer_events(&s->ui.pointer);
  s->ui.pointer.clear();
  ImGui::GetIO().ClearEventsQueue();
  ImGui::GetIO().ClearInputMouse();
  return res;
}

uint32_t session_pump(Session* s) { return session_events(s, platform_pump()); }

uint32_t session_wait_visible(Session* s) {
  uint32_t ev = 0;
  while (!platform_visible() && !(ev & (PXB_EV_QUIT | PXB_EV_BACK))) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    ev |= session_pump(s);
  }
  return ev;
}

// Device idle: the window events of one platform_pump (session_pump), for the menu and between the frames of a run.
static uint32_t session_events(Session* s, uint32_t ev) {
  // ev is a bitmask: handled in priority order, a surface loss covers a resize
  if (s->quit) { return ev | PXB_EV_QUIT; }

  if (ev & PXB_EV_QUIT) {
    s->quit = true;
    return ev;
  }
  if (ev & PXB_EV_SURFACE_LOST) { // UNVERIFIED (cannot be exercised on macOS)
    if (s->dev_ready) {
      swapchain_drop(s);
      s->gpu->surface_destroy(&s->dev);
    }
    if (!platform_wait_surface()) {
      s->quit = true;
      return ev | PXB_EV_QUIT;
    }
    if (s->dev_ready && !s->gpu->surface_create(&s->dev, platform_surface_handle())) {
      fprintf(stderr, "error: surface recreation failed\n"); // the next run fails at swapchain setup
    }
    return ev;
  }
  if ((ev & PXB_EV_RESIZED) && s->dev_ready) { swapchain_drop(s); }
  return ev;
}

bool session_ui_frame(Session* s, const std::function<void()>& build) {
  if (!s->dev_ready || !session_swapchain(s, false, false)) { return false; }
  if (!s->sc.swapchain) { // zero extent (minimized): drop it, so the next call creates it again
    swapchain_drop(s);
    return false;
  }
  VkExtent2D ext = s->sc.view_extent;
  if (!ext.width || !ext.height) { return false; }
  gpu_ui_new_frame(&s->ui, ext);
  build();
  ImGui::Render();
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  bool ok = false;
  if (s->gpu->frame_begin(&s->dev, &s->sc, &cmd)) {
    ImDrawData* dd = ImGui::GetDrawData();
    GpuPassDesc d;
    d.clear = true;
    d.clear_color[0] = d.clear_color[1] = d.clear_color[2] = 0.02f;
    d.clear_color[3] = 1.0f;
    d.swapchain = true;
    // a failed prepare / pass still ends the frame: the acquired image must be presented
    if (s->gpu->imgui_prepare(&s->dev, &s->sc, &s->ui.im, cmd, dd) &&
        s->gpu->pass_begin(&s->dev, &s->sc, &s->pass, cmd, d)) {
      s->gpu->imgui_draw(&s->ui.im, &s->pass, dd);
      s->gpu->pass_end(&s->pass);
      ok = true;
    }
    ok = s->gpu->frame_end(&s->dev, &s->sc) && ok;
  }
  {
    ZoneScopedN("wait_idle");
    vkDeviceWaitIdle(s->dev.device);
  }
  if (s->sc.needs_recreate || s->sc.submit_failed) { swapchain_drop(s); }
  return ok;
}

std::string results_json(const Session* s, const std::vector<std::string>& runs) {
  std::string out = "{\"pxbenchy\":1,\"device\":";
  out += s->dev_ready ? device_info_json(&s->dev) : "null";
  if (!s->vulkan_info.empty()) { out += ",\"vulkan\":" + s->vulkan_info; }
  out += ",\"runs\":[";
  bool first = true;
  for (const std::string& r : runs) {
    if (r.empty()) { continue; }
    out += first ? "\n" : ",\n";
    out.append(r, 0, r.back() == '\n' ? r.size() - 1 : r.size());
    first = false;
  }
  out += "\n]}\n";
  return out;
}

bool results_save(const Session* s, const std::vector<std::string>& runs, std::string* where) {
  bool any = false;
  for (const std::string& r : runs) { any = any || !r.empty(); }
  if (!any) {
    fprintf(stderr, "error: results: no run reports to save\n");
    return false;
  }
  // platform_device_info leaves empty values out
  auto info = platform_device_info();
  auto it = std::find_if(info.begin(), info.end(), [](const auto& kv) { return kv.first == "model"; });
  std::string model = it != info.end() ? it->second : "device";
  std::replace_if(
      model.begin(), model.end(), [](unsigned char c) { return !std::isalnum(c) && c != '.' && c != '-'; }, '_');
  time_t t = time(nullptr);
  struct tm lt = {};
  localtime_r(&t, &lt);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &lt);
  return platform_save_results(model + "-" + stamp + ".json", results_json(s, runs), where);
}
