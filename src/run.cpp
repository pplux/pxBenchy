#include "run.h"
#include <algorithm>
#include <cstdio>
#include "device_info.h"
#include "git_version.h"
#include "json.h"
#include "lua/bind_pxb.h"
#include "platform/platform.h"
#include "platform/thermal.h"
#include "ui.h"
#include <imgui.h>
#include <tracy/Tracy.hpp>

// Ends the run with an error, like a Lua error would (ok = false).
static void run_error(Run* r, const char* msg) {
  fprintf(stderr, "error: %s\n", msg);
  r->st = VmState::error;
}

// ---- frame lifecycle ----

// Between two frames (offscreen capture / blit submit, pause, unavailable frame): the GPU and CPU intervals from the
// last ended frame to the next one are dropped, open CPU timers discard their current occurrence.
static void break_interval(Run* r) {
  r->gpu->timers_break_interval(&r->timers.gpu);
  timer_set_break_interval(&r->timers);
}

// Opens the frame the script records into until its next gpu.present (waits for its slot, acquires), with its timers
// started. frame_begin failed (the swapchain is out of date or gone, the session recreates it): counted, the GPU and
// CPU intervals spanning it are dropped, false (the script is not resumed).
static bool open_frame(Run* r) {
  GpuLua* g = &r->g;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (!r->gpu->frame_begin(g->dev, g->sc, &cmd)) {
    r->frames_unavailable++;
    break_interval(r);
    return false;
  }
  r->gpu->descriptors_frame_begin(g->dev, &g->descriptors, g->sc->frame);
  g->frame_open = true;
  g->cmd = cmd;
  bind_gpu_frame_opened(g);
  FrameMark;
  timer_set_frame_begin(&r->timers);
  r->gpu->timers_frame_begin(g->dev, g->sc, &r->timers.gpu, cmd);
  return true;
}

// Ends the open frame (submit, present unless offscreen). Whether its timer results count is decided once the work
// after it is known (frame_decision, applied by frame_measure).
static void end_frame(Run* r) {
  GpuLua* g = &r->g;
  r->gpu->timers_frame_end(&r->timers.gpu, g->cmd);
  r->gpu->frame_end(g->dev, g->sc);
  g->frame_open = false;
  g->cmd = VK_NULL_HANDLE;
}

// What counts of the frame just ended. gpu / cpu false: its results are dropped, and the intervals into it and to the
// next frame. gpu_break / cpu_break: the interval to the next frame is dropped.
struct FrameDecision {
  bool gpu = true, cpu = true;
  bool gpu_break = false, cpu_break = false;
};

// The one place that decides what of the frame just ended is measured. present: it ended in a counted gpu.present
// (false: the script returned or failed, or the present failed: not measured). submit: an offscreen capture or blit
// submit followed it. since_copy: offscreen, presents since the last copy to the screen including this one (-1 =
// none pending). Not measured: warm-up frames (frame 1, the setup, is one), the frame not ended by a present. The
// intervals spanning a capture / blit submit are broken. Offscreen on Android the compositor's work on the copied image
// slows the GPU frame right after the copy (Pixel 9: +20 ms): its GPU results are dropped. GPU-bound, the CPU waits for
// that frame when it reuses its slot, PXB_FRAMES_IN_FLIGHT presents after the copy: that cpu interval (to the next
// frame) and the open CPU timers are dropped.
static FrameDecision frame_decision(const Run* r, bool present, bool submit, int since_copy) {
  FrameDecision d;
  if (!present || r->presents + 1 <= r->ro.frames_warmup) { d.gpu = d.cpu = false; }
  if (since_copy == 1) { d.gpu = false; }
  if (since_copy == (int)PXB_FRAMES_IN_FLIGHT) { d.cpu_break = true; }
  if (submit) { d.gpu_break = d.cpu_break = true; }
  return d;
}

// Applies frame_decision to the frame just ended (GPU: after its frame_end).
static void frame_measure(Run* r, FrameDecision d) {
  if (!d.gpu) {
    r->gpu->timers_drop_frame(&r->timers.gpu);
  } else if (d.gpu_break) {
    r->gpu->timers_break_interval(&r->timers.gpu);
  }
  timer_set_frame_end(&r->timers, d.cpu);
  if (d.cpu && d.cpu_break) { timer_set_break_interval(&r->timers); }
}

// ---- HUD, windows ----

// Live stats as an ImGui window (top-left, no decoration) in its own LOAD pass on the swapchain image (frame cmd:
// counted in gpu busy; offscreen: blit cmd). The ImGui frame is built each time (D1). Text height follows the former
// 8x8 HUD: 8 px per 360 px of the shorter view side. The style (gpu_ui_init) is white on nearly opaque black:
// blending happens in linear space on the sRGB swapchain, so translucent or grey backgrounds wash out.
static void draw_hud(Run* r, VkCommandBuffer cmd) {
  const GpuLua* g = &r->g;
  const GpuApiTable* gpu = r->gpu;
  VkExtent2D ext = g->sc->view_extent;
  if (!ext.width || !ext.height) { return; }
  int64_t now = timer_now_ns();
  float scale = std::min(ext.width, ext.height) / 360.0f;

  gpu_ui_input(r->ui);
  gpu_ui_new_frame(r->ui, ext);
  ImGui::PushFont(nullptr, std::max(13.0f, 8.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * scale, 0.0f));
  ImGui::SetNextWindowPos(ImVec2(4.0f * scale, 4.0f * scale));
  ImGui::Begin("hud", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  long long n = r->presents + 1;
  ImGui::Text("%s", path_base_name(r->ro.script));
  if (r->ro.frames) {
    ImGui::Text("frame %lld/%lld%s  elapsed %.1f s", n, r->ro.frames_warmup + r->ro.frames,
                r->presents < r->ro.frames_warmup ? " WARMUP" : "", (now - r->start_ns) / 1e9);
  } else {
    ImGui::Text("frame %lld%s  elapsed %.1f s", n, r->presents < r->ro.frames_warmup ? " WARMUP" : "",
                (now - r->start_ns) / 1e9);
  }
  char display[32];
  if (g->sc->refresh_ns) {
    snprintf(display, sizeof(display), "display %.0f Hz", 1e9 / g->sc->refresh_ns);
  } else {
    snprintf(display, sizeof(display), "display -");
  }
  // values of the last completed 1 s window (a single sample is too noisy to read)
  const RunWindow* last = r->windows.empty() ? nullptr : &r->windows.back();
  const RunWindow* prev = r->windows.size() > 1 ? &r->windows[r->windows.size() - 2] : nullptr;
  const TimerMark none;
  const TimerMark& m1 = last ? last->timers : none;
  const TimerMark& m0 = prev ? prev->timers : none;
  double fps = 0.0;
  if (last) {
    double dt = last->t_s - (prev ? prev->t_s : 0.0f);
    fps = dt > 0 ? (last->presents - (prev ? prev->presents : r->measure_start_presents)) / dt : 0.0;
  }
  double busy_ms = timer_mean_ms(gpu->timers_busy(&r->timers.gpu), m0.busy, m1.busy);
  ImGui::Text("fps %.1f (%.2f ms)  gpu busy %.3f ms  %s", fps, fps > 0 ? 1000.0 / fps : 0.0,
              busy_ms < 0 ? 0.0 : busy_ms, display);
  ImGui::Text("vk%s  %s%s  validation %s  %ux%u  git %s", gpu_api_name(g->api),
              gpu->present_mode_name(g->sc->present_mode), g->sc->offscreen ? " offscreen" : "",
              g->dev->validation ? "on" : "off", ext.width, ext.height, PXB_GIT_HASH);
  PlatformThermal th = thermal_latest();
  ImGui::Text("thermal headroom %.2f  status %s  battery %.1f C", th.headroom, platform_thermal_status_name(th.status),
              th.battery_c);
  // user timers, one row per name (a cpu and a gpu timer may share it), in creation order
  std::vector<TimerRow> rows = timer_user_rows(&r->timers);
  if (!rows.empty() && ImGui::BeginTable("timers", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("timer (ms, 1 s mean)");
    ImGui::TableSetupColumn("gpu");
    ImGui::TableSetupColumn("cpu");
    ImGui::TableHeadersRow();
    auto at = [](const std::vector<size_t>& v, int id) { return (size_t)id < v.size() ? v[id] : 0; };
    for (const TimerRow& row : rows) {
      double gpu_ms = -1.0, cpu_ms = -1.0;
      if (row.gpu >= 0) {
        gpu_ms = timer_mean_ms(gpu->timer_results(&r->timers.gpu, row.gpu), at(m0.gpu, row.gpu), at(m1.gpu, row.gpu));
      }
      if (row.cpu >= 0) { cpu_ms = timer_mean_ms(r->timers.cpu[row.cpu].ns, at(m0.cpu, row.cpu), at(m1.cpu, row.cpu)); }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(row.name.c_str());
      for (double v : {gpu_ms, cpu_ms}) {
        ImGui::TableNextColumn();
        if (v >= 0) {
          ImGui::Text("%.3f", v);
        } else {
          ImGui::TextUnformatted("-");
        }
      }
    }
    ImGui::EndTable();
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  ImGui::Render();

  ImDrawData* dd = ImGui::GetDrawData();
  if (!gpu->imgui_prepare(g->dev, g->sc, &r->ui->im, cmd, dd)) { return; }
  GpuPassDesc d;
  d.swapchain = true;
  if (!gpu->pass_begin(g->dev, g->sc, &r->hud_pass, cmd, d)) { return; }
  gpu->imgui_draw(&r->ui->im, &r->hud_pass, dd);
  gpu->pass_end(&r->hud_pass);
}

static void close_window(Run* r, int64_t now) {
  GpuLua* g = &r->g;
  PlatformThermal th = thermal_latest();
  r->windows.push_back(RunWindow{(float)((now - r->measure_start_ns) / 1e9), r->presents, th.headroom, th.status,
                                 timer_mark(&r->timers, r->gpu)});
  r->window_t0 = now;
}

// After each present: closes the window once 1 s of wall time has passed.
static void tick_window(Run* r) {
  int64_t now = timer_now_ns();
  if (now - r->window_t0 >= 1000000000) { close_window(r, now); }
}

// ---- present ----

// The script yielded in gpu.present: ends its frame (screenshot, HUD), then the offscreen capture / copy. True for a
// counted present; *submit and *since_copy: frame_decision's inputs. The next frame opens when the script is resumed
// (run_step).
static bool present(Run* r, bool* submit, int* since_copy) {
  GpuLua* g = &r->g;
  const GpuApiTable* gpu = r->gpu;
  GpuScreenshot shot;
  bool shoot = !r->ro.screenshot.empty() && r->presents + 1 >= r->ro.screenshot_frame;
  // before the HUD: the screenshot shows the scene only
  if (shoot && !gpu->screenshot_record(g->dev, g->sc, g->cmd, &shot)) {
    end_frame(r);
    run_error(r, "gpu.present: screenshot failed");
    return false;
  }
  if (r->ro.hud && !g->sc->offscreen) { draw_hud(r, g->cmd); }
  end_frame(r);
  if (shoot) {
    bind_gpu_wait_idle(g);
    if (g->sc->submit_failed) {
      gpu->screenshot_destroy(g->dev, &shot);
      run_error(r, "gpu.present: submit failed");
      return false;
    }
    if (!gpu->screenshot_write(g->dev, g->sc, &shot, r->ro.screenshot.c_str())) {
      run_error(r, "gpu.present: screenshot failed");
      return false;
    }
    printf("screenshot: %s (frame %lld, %ux%u)\n", r->ro.screenshot.c_str(), r->presents + 1, g->sc->view_extent.width,
           g->sc->view_extent.height);
    fflush(stdout);
    r->screenshot_taken = true; // the run ends
    return false;
  }
  if (g->sc->offscreen && !g->sc->submit_failed) {
    // about once per wall second: own submit captures the last submitted frame; two frames later (its fence waited,
    // nothing in flight depends on it) another submit copies the capture to the screen (+ HUD) and presents. What
    // that drops from the timers: frame_decision.
    int64_t now = timer_now_ns();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (r->since_copy >= 0) {
      *since_copy = ++r->since_copy;
      if (r->since_copy == (int)PXB_FRAMES_IN_FLIGHT) { r->since_copy = -1; }
    }
    if (r->blit_in > 0) {
      if (--r->blit_in == 0 && gpu->blit_begin(g->dev, g->sc, &cmd)) {
        if (r->ro.hud) { draw_hud(r, cmd); }
        gpu->blit_end(g->dev, g->sc);
        r->offscreen_presents++;
        r->since_copy = 0;
        *submit = true;
      }
    } else if (now - r->last_present_ns >= 1000000000 && gpu->capture(g->dev, g->sc)) {
      r->last_present_ns = now;
      r->blit_in = 2;
      *submit = true;
    }
  }
  if (g->sc->submit_failed) {
    run_error(r, "gpu.present: submit failed");
    return false;
  }
  return true;
}

// After vm_start / vm_resume: the script yielded in gpu.present, or returned / failed (its open pass and frame are
// closed so the device work is complete and shutdown is clean). Then the frame's measurement (frame_decision) and,
// for a counted present: the end of the warm-up (measuring begins: bookkeeping only, warm-up results were dropped per
// frame), the windows, the end of the run after the warm-up + ro.frames presents (complete).
static void script_stopped(Run* r) {
  bool counted = false, submit = false;
  int since_copy = -1;
  if (r->st == VmState::yield) {
    counted = present(r, &submit, &since_copy);
  } else {
    GpuLua* g = &r->g;
    if (g->pass.cmd) { r->gpu->pass_end(&g->pass); }
    g->in_pass = false;
    end_frame(r);
  }
  frame_measure(r, frame_decision(r, counted, submit, since_copy));
  if (!counted) { return; }
  if (++r->presents == r->ro.frames_warmup) {
    r->measure_start_ns = r->window_t0 = timer_now_ns();
    r->measure_start_presents = r->presents;
    r->offscreen_presents = 0;
    r->windows.clear();
  }
  tick_window(r);
  if (r->ro.frames && r->presents == r->ro.frames_warmup + r->ro.frames) { r->complete = true; }
}

// ---- report ----

// Wall time of the measured frames so far (0 during warm-up).
static int64_t run_measured_ns(const Run* r) {
  return r->presents >= r->ro.frames_warmup ? timer_now_ns() - r->measure_start_ns : 0;
}

// After the script ended or was stopped (its frame ended), before the report: a script still in gpu.present (any end:
// complete, time limit, screenshot, Back, quit) is resumed once more, without a frame and not measured, so gpu.present
// returns false and the script leaves its loop, releases its resources and returns. The CPU timers it keeps open
// across gpu.present are closed quietly first (the Lua state's sentinel warns only for scripts that end with one).
// Then waits idle, resolves the GPU timers.
static void run_finish(Run* r) {
  r->measure_end_ns = timer_now_ns();
  if (r->st == VmState::yield) {
    timer_set_close_open(&r->timers);
    r->st = vm_resume(r->L);
  }
  bind_gpu_wait_idle(&r->g);
  r->gpu->timers_resolve_all(r->g.dev, &r->timers.gpu);
}

// After run_finish: the measured frames (presents after the warm-up), their wall time in *secs.
static long long run_measured(const Run* r, double* secs) {
  *secs = (r->measure_end_ns - r->measure_start_ns) / 1e9;
  return r->presents - r->measure_start_presents;
}

// After run_finish, the lines printed after the timers_report table: `run: N frames in T s = F fps, display H Hz
// (refresh X ms)` (`display -` when unknown) for the measured frames, plus `offscreen presents: N (blits, spanning
// intervals excluded)` in offscreen mode, `frames unavailable: N` when frame opens failed, then `thermal: headroom
// start S end E max M, status max NAME, battery B C` over the windows (battery: at the report) (start = end of the
// first window, 0 where unsupported; left out without windows).
static std::string run_report(const Run* r) {
  const GpuLua* g = &r->g;
  std::string o;
  double secs = 0.0;
  long long frames = run_measured(r, &secs);
  str_appendf(&o, "run: vulkan %s, %lld frames in %.3f s = %.1f fps, ", gpu_api_name(g->api), frames, secs,
              secs > 0 ? frames / secs : 0.0);
  if (g->sc->refresh_ns) {
    str_appendf(&o, "display %.1f Hz (refresh %.3f ms)\n", 1e9 / g->sc->refresh_ns, g->sc->refresh_ns / 1e6);
  } else {
    o += "display -\n";
  }
  if (g->sc->offscreen) {
    str_appendf(&o, "offscreen presents: %lld (blits; spanning intervals and the frame after each excluded)\n",
                r->offscreen_presents);
  }
  if (r->time_limited) { str_appendf(&o, "time limit: %.0f s reached\n", (double)r->ro.time_limit); }
  if (r->frames_unavailable) {
    str_appendf(&o, "frames unavailable: %lld (swapchain out of date / gone: not drawn, spanning intervals excluded)\n",
                r->frames_unavailable);
  }
  if (!r->windows.empty()) {
    float headroom_max = 0.0f, status_max = 0.0f;
    for (const RunWindow& w : r->windows) {
      headroom_max = std::max(headroom_max, w.headroom);
      status_max = std::max(status_max, w.status);
    }
    str_appendf(&o, "thermal: headroom start %.3f end %.3f max %.3f, status max %s, battery %.1f C\n",
                r->windows.front().headroom, r->windows.back().headroom, headroom_max,
                platform_thermal_status_name(status_max), thermal_latest().battery_c);
  }
  return o;
}

// --json: the whole report (run info, measured frames / time / fps, offscreen presents, per-window series of frames and
// thermal, timers_report_json with the windows' timer series) as one JSON line, newline-terminated.
static std::string run_report_json(const Run* r) {
  const GpuLua* g = &r->g;
  const GpuSwapchain* sc = g->sc;
  std::string o = "{\"script\":" + json_string(r->ro.script) + ",\"device\":" + json_string(g->dev->props.deviceName) +
                  ",\"vulkan\":\"" + gpu_api_name(g->api) + "\"";
  char buf[256];
  snprintf(buf, sizeof(buf),
           ",\"validation\":%s,\"present_mode\":\"%s\",\"offscreen\":%s,\"hud\":%s,\"extent\":[%u,%u]"
           ",\"frames\":%lld,\"frames_warmup\":%lld",
           g->dev->validation ? "true" : "false", r->gpu->present_mode_name(sc->present_mode),
           sc->offscreen ? "true" : "false", r->ro.hud ? "true" : "false", sc->view_extent.width,
           sc->view_extent.height, r->ro.frames, r->ro.frames_warmup);
  o += buf;
  if (sc->refresh_ns) {
    snprintf(buf, sizeof(buf), ",\"refresh_ms\":%.6f", sc->refresh_ns / 1e6);
  } else {
    snprintf(buf, sizeof(buf), ",\"refresh_ms\":null");
  }
  o += buf;
  double secs = 0.0;
  long long frames = run_measured(r, &secs);
  snprintf(buf, sizeof(buf), ",\"measured_frames\":%lld,\"measured_time_s\":%.6f,\"fps\":%.3f", frames, secs,
           secs > 0 ? frames / secs : 0.0);
  o += buf;
  if (sc->offscreen) {
    snprintf(buf, sizeof(buf), ",\"offscreen_presents\":%lld", r->offscreen_presents);
  } else {
    snprintf(buf, sizeof(buf), ",\"offscreen_presents\":null");
  }
  o += buf;
  snprintf(buf, sizeof(buf), ",\"time_limit_s\":%lld,\"time_limited\":%s,\"frames_unavailable\":%lld", r->ro.time_limit,
           r->time_limited ? "true" : "false", r->frames_unavailable);
  o += buf;
  // per window: t_s, frames (presents in it), thermal headroom / status at its end
  std::string cols[4];
  std::vector<TimerMark> marks;
  long long presents = r->measure_start_presents;
  for (size_t i = 0; i < r->windows.size(); i++) {
    const RunWindow& w = r->windows[i];
    const char* sep = i ? "," : "";
    str_appendf(&cols[0], "%s%.3f", sep, w.t_s);
    str_appendf(&cols[1], "%s%lld", sep, w.presents - presents);
    str_appendf(&cols[2], "%s%.4f", sep, w.headroom);
    str_appendf(&cols[3], "%s%.0f", sep, w.status);
    presents = w.presents;
    marks.push_back(w.timers);
  }
  o += ",\"series\":{\"t_s\":[" + cols[0] + "],\"frames\":[" + cols[1] + "],\"thermal_headroom\":[" + cols[2] +
       "],\"thermal_status\":[" + cols[3] + "]}";
  str_appendf(&o, ",\"battery_c\":%.1f", thermal_latest().battery_c); // once per run: barely moves during one
  o += ",\"device_info\":" + device_info_json(g->dev);
  o += ",\"timers\":" + timers_report_json(&r->timers, r->gpu, marks) + "}\n";
  return o;
}

// ---- run ----

// Opens a frame, then starts (first open) or resumes the script and handles its stop. Nothing when the frame cannot be
// opened: the session recreates the swapchain between frames and the next run_step tries again. Then the time limit:
// the run ends once the measured frames (after warm-up) reach ro.time_limit.
static void run_frame(Run* r) {
  if (open_frame(r)) {
    r->st = r->started ? vm_resume(r->L) : vm_start(r->L, r->ro.script);
    r->started = true;
    script_stopped(r);
  }
  if (run_active(r) && r->ro.time_limit && run_measured_ns(r) >= r->ro.time_limit * 1000000000LL) {
    r->time_limited = true;
  }
}

bool run_init(Run* r, const RunOptions& ro, GpuApi api, GpuDevice* dev, GpuSwapchain* sc, GpuUi* ui) {
  r->ro = ro;
  r->ro.frames_warmup = std::max(r->ro.frames_warmup, 1LL); // frame 1 holds the script's setup: always warm-up
  r->gpu = gpu_api_table(api);
  r->ui = ui;
  r->start_ns = r->window_t0 = r->measure_start_ns = timer_now_ns();
  if (!bind_gpu_init(&r->g, api, dev, sc, &r->timers)) {
    bind_gpu_shutdown(&r->g); // timers_init failed: frees what it created
    return false;
  }
  r->L = vm_create();
  if (r->L) {
    bind_gpu_register(r->L, &r->g);
    bind_pxb_register(r->L, &r->timers);
    run_frame(r);
  } else {
    r->st = VmState::error;
  }
  return true;
}

bool run_active(const Run* r) {
  return (r->L && !r->started) || (r->st == VmState::yield && !r->screenshot_taken && !r->time_limited && !r->complete);
}

bool run_step(Run* r) {
  run_frame(r);
  return run_active(r);
}

void run_wait_idle(Run* r) { bind_gpu_wait_idle(&r->g); }

void run_swapchain_recreated(Run* r) { r->blit_in = 0; }

void run_paused(Run* r, int64_t ns) {
  r->start_ns += ns;
  r->measure_start_ns += ns;
  r->window_t0 += ns;
  r->last_present_ns += ns;
  break_interval(r);
}

void run_result(Run* r, RunResult* res) {
  const RunOptions& ro = r->ro;
  res->time_limited = r->time_limited;
  // a normal end (the script returned, or the run ended it) whose final resume returned
  bool normal =
      r->st == VmState::done || (r->st == VmState::yield && (r->screenshot_taken || r->time_limited || r->complete));
  run_finish(r);
  res->ok = normal && r->st == VmState::done;
  if (r->presents < ro.frames_warmup) {
    r->windows.clear(); // ended during warm-up: nothing measured
  } else if (r->presents > (r->windows.empty() ? r->measure_start_presents : r->windows.back().presents)) {
    close_window(r, r->measure_end_ns); // the partial last window, with the GPU results resolved by run_finish
  }
  if (res->ok && !ro.screenshot.empty() && !r->screenshot_taken) {
    fprintf(stderr, "error: --screenshot: script ended before frame %lld\n", ro.screenshot_frame);
    res->ok = false;
  }
  if (!r->screenshot_taken) { // screenshot runs end without a report
    res->text = timers_report(&r->timers, r->gpu) + run_report(r);
    res->json = run_report_json(r);
    double secs = 0.0;
    res->frames = run_measured(r, &secs);
    res->fps = secs > 0 ? res->frames / secs : 0.0;
    res->timers = timers_summary(&r->timers, r->gpu);
  }
}

void run_shutdown(Run* r) {
  vm_destroy(r->L);
  r->L = nullptr;
  bind_gpu_shutdown(&r->g); // waits idle
  r->gpu->pass_shutdown(r->g.dev, &r->hud_pass);
}
