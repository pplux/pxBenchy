#include "menu.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <imgui.h>
#include <imgui_internal.h>
#include "git_version.h"
#include "device_info.h"
#include "platform/platform.h"
#include "platform/thermal.h"
#include "ui.h"
#include <tracy/Tracy.hpp>

// --frames / --frames-warmup steps of the -/+ buttons (no text input: no soft keyboard on Android). A value from the
// command line that is not listed steps to the nearest listed one.
static const long long FRAME_STEPS[] = {60, 120, 300, 600, 1200, 3000, 6000, 12000};
static const long long WARMUP_STEPS[] = {0, 10, 30, 60, 120, 300};

// Frames drawn after an input event or a state change: ImGui's trickle input queue spreads a tap (pos, down, up,
// leave) over several frames, and a click changes the layout one frame later.
constexpr int REDRAW_FRAMES = 3;
constexpr int IDLE_POLL_MS = 16; // event poll period while nothing changes (platform_pump does not block)

// Update check (Android): the latest GitHub release's tag_name against PXB_VERSION, on a thread at menu start. A newer
// one shows a Download button that opens the latest release page in the browser.
constexpr const char* RELEASES_API = "https://api.github.com/repos/pplux/pxBenchy/releases/latest";
constexpr const char* RELEASES_PAGE = "https://github.com/pplux/pxBenchy/releases/latest";

constexpr long long MENU_TIME_LIMIT = 60; // seconds

struct UpdateCheck {
  std::atomic<bool> done{false};
  std::string tag; // newer release's tag_name, empty when none or the check failed; written before done
};

struct Menu {
  RunOptions ro;
  bool time_limit = true;           // "Run all demos" ends each demo after MENU_TIME_LIMIT s of measured frames
  bool vk13 = false;                // gpu_vk13_supported()
  float scale = 1.0f;               // platform_ui_scale()
  PlatformInsets insets;            // platform_safe_insets(), kept clear of the full-screen windows
  std::vector<std::string> scripts; // platform_list_scripts()
  std::vector<std::string> demos;   // suite_scripts() ("Run all")
  std::vector<RunResult> results;   // every run of the app session, in run order
  std::string status;               // last action's outcome
  std::string device_summary;       // header: device_info_summary, set at start and after each Vulkan switch
  // touch drag-to-scroll (drag_scroll)
  ImGuiID drag_window = 0;
  bool dragging = false;
  float drag_scroll0 = 0.0f;
  // set while building a frame, done after it (device idle, no ImGui frame open)
  enum class Action { none, vulkan, validation, run, run_all, clear, save, share, share_text, copy, update };
  Action action = Action::none;
  GpuApi want_api = GpuApi::vk11;
  bool want_validation = false;
  size_t run_index = 0;
  std::shared_ptr<UpdateCheck> update; // shared with the check thread (detached: may outlive the menu)
  bool update_seen = false;            // update->done observed, update_tag set
  std::string update_tag;
};

// Value of the first `"key": "string"` in json, empty if missing (no escapes: tags have none).
static std::string json_field(const std::string& json, const char* key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = json.find(k);
  p = p == std::string::npos ? p : json.find_first_not_of(" \t\r\n", p + k.size());
  if (p == std::string::npos || json[p] != ':') { return ""; }
  p = json.find_first_not_of(" \t\r\n", p + 1);
  if (p == std::string::npos || json[p] != '"') { return ""; }
  size_t e = json.find('"', p + 1);
  return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
}

// "1.2.3" or "v1.2.3" -> {1, 2, 3}. False on any other shape.
static bool parse_version(const std::string& s, std::array<int, 3>* v) {
  const char* c = s.c_str() + (!s.empty() && s[0] == 'v' ? 1 : 0);
  int n = 0;
  return sscanf(c, "%d.%d.%d%n", &(*v)[0], &(*v)[1], &(*v)[2], &n) == 3 && c[n] == '\0';
}

// Check thread body: sets u->tag when the latest release is newer than this build, then u->done.
static void update_check(UpdateCheck* u) {
  std::string body, err;
  std::array<int, 3> cur{}, latest{};
  if (!platform_http_get(RELEASES_API, &body, &err)) {
    printf("update: check failed: %s\n", err.c_str());
  } else {
    std::string tag = json_field(body, "tag_name");
    if (!parse_version(PXB_VERSION, &cur) || !parse_version(tag, &latest)) {
      printf("update: cannot compare release '%s' with %s\n", tag.c_str(), PXB_VERSION);
    } else if (cur < latest) {
      printf("update: %s available (running %s)\n", tag.c_str(), PXB_VERSION);
      u->tag = tag;
    } else {
      printf("update: up to date (%s, latest release %s)\n", PXB_VERSION, tag.c_str());
    }
  }
  fflush(stdout);
  u->done.store(true);
}

static void set_status(Menu* m, const RunResult& r) {
  char buf[256];
  if (r.ok) {
    snprintf(buf, sizeof(buf), "%s: %.1f fps (vulkan %s)", path_base_name(r.script), r.fps, gpu_api_name(r.api));
  } else {
    snprintf(buf, sizeof(buf), "%s: %s (see log)", path_base_name(r.script),
             r.stopped ? "stopped"
             : r.quit  ? "quit"
                       : "failed");
  }
  m->status = buf;
}

static void add_status(Menu* m, const std::string& s) {
  m->status += m->status.empty() ? "" : "  ";
  m->status += s;
}

// Run reports of every run in the table, in run order (results_json / results_save input).
static std::vector<std::string> table_runs(const Menu* m) {
  std::vector<std::string> runs;
  for (const RunResult& r : m->results) { runs.push_back(r.json); }
  return runs;
}

// Result file (results_save) of every run in the table, status updated. Only on "Save results" or a Share that needs
// the file: nothing is written before the user asks. Returns the path (macOS) | content:// URI (Android), "" on
// failure.
static std::string save_results(Menu* m, Session* s) {
  std::string where;
  if (!results_save(s, table_runs(m), &where)) {
    add_status(m, "save failed (see log)");
    return "";
  }
  add_status(m, "saved " + where);
  printf("results: %s\n", where.c_str());
  fflush(stdout);
  return where;
}

// Share / Share as text / Copy the table's results JSON. Copy and Share as text write no file; Share (and Share as text
// above PXB_SHARE_TEXT_MAX) saves the table and shares that file. Status = the outcome.
static void share(Menu* m, Session* s, Menu::Action a) {
  m->status.clear();
  std::string json = results_json(s, table_runs(m));
  bool as_text = a == Menu::Action::copy || (a == Menu::Action::share_text && json.size() <= PXB_SHARE_TEXT_MAX);
  char kb[32];
  snprintf(kb, sizeof(kb), "%.1f KB", json.size() / 1024.0);
  std::string msg;
  if (a == Menu::Action::copy) {
    bool ok = platform_copy_text(json, &msg);
    add_status(m, ok ? std::string("Copied ") + kb : "Copy failed: " + msg);
  } else if (as_text) {
    bool ok = platform_share_text(json, &msg);
    add_status(m, ok ? msg + " (" + kb + " text)" : "Share as text failed: " + msg);
  } else { // Share, or text too large for an intent extra: the file instead
    std::string where = save_results(m, s);
    if (where.empty()) { return; }
    bool ok = platform_share_file(where, &msg);
    if (a == Menu::Action::share_text) { add_status(m, std::string(kb) + " is too large for text, sharing the file:"); }
    add_status(m, ok ? msg : "Share failed: " + msg);
  }
  printf("menu: share: %s\n", m->status.c_str());
  fflush(stdout);
}

static void print_result(const RunResult& r) {
  printf("menu: %s vulkan %s: %s\n", r.script.c_str(), gpu_api_name(r.api), r.ok ? "ok" : "failed");
  fwrite(r.text.data(), 1, r.text.size(), stdout);
  fflush(stdout);
}

// Queued pointer events into ImGui. True if any.
static bool ui_input(Session* s) {
  gpu_ui_input(&s->ui);
  return !s->ui.pointer.empty();
}

// Menu style for one session_ui_frame: the HUD style (gpu_ui_init) with sizes for touch, scaled by density.
static bool ui_frame(Session* s, float scale, const std::function<void()>& build) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGuiStyle saved = st;
  st.ScaleAllSizes(1.5f * scale);
  st.WindowPadding = ImVec2(10.0f * scale, 10.0f * scale);
  st.FramePadding = ImVec2(10.0f * scale, 8.0f * scale); // 16 dp font: 32 dp tall buttons
  st.ItemSpacing = ImVec2(8.0f * scale, 6.0f * scale);
  st.CellPadding = ImVec2(8.0f * scale, 4.0f * scale);
  st.FrameRounding = 6.0f * scale;
  st.ChildRounding = 6.0f * scale;
  st.ScrollbarSize = 14.0f * scale;
  st.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.02f, 0.02f, 1.0f);
  st.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
  bool ok = session_ui_frame(s, [&] {
    ImGui::PushFont(nullptr, 16.0f * scale);
    build();
    ImGui::PopFont();
  });
  st = saved;
  return ok;
}

// Touch drag-to-scroll for the current child window (call before EndChild; ImGui has none built in). A touch that
// starts inside the window, not on its scrollbar, and moves past the drag threshold scrolls it with the finger and
// cancels the widget under the finger (no click on release). Mouse input keeps the wheel and scrollbar.
static void drag_scroll(Menu* m) {
  ImGuiIO& io = ImGui::GetIO();
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (ImGui::IsMouseClicked(0) && io.MouseSource == ImGuiMouseSource_TouchScreen &&
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
      ImGui::GetActiveID() != ImGui::GetWindowScrollbarID(w, ImGuiAxis_Y)) {
    m->drag_window = w->ID;
    m->dragging = false;
    m->drag_scroll0 = ImGui::GetScrollY();
  }
  if (m->drag_window != w->ID) { return; }
  if (!ImGui::IsMouseDown(0)) {
    m->drag_window = 0;
    return;
  }
  float dy = ImGui::GetMouseDragDelta(0, 0.0f).y;
  if (!m->dragging && std::fabs(dy) > 10.0f * m->scale) { m->dragging = true; }
  if (!m->dragging) { return; }
  ImGui::ClearActiveID();
  ImGui::SetScrollY(m->drag_scroll0 - dy);
}

// Segmented choice button: highlighted when selected. Returns true when tapped (and enabled).
static bool choice(const char* label, bool selected, bool enabled, float width) {
  ImGui::BeginDisabled(!enabled);
  if (selected) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)); }
  bool r = ImGui::Button(label, ImVec2(width, 0.0f));
  if (selected) { ImGui::PopStyleColor(); }
  ImGui::EndDisabled();
  return r && !selected;
}

// SameLine when a button labeled `label` still fits on the current line, else it goes on the next one.
static void same_line_if_fits(const char* label) {
  const ImGuiStyle& st = ImGui::GetStyle();
  float w = ImGui::CalcTextSize(label, nullptr, true).x + 2.0f * st.FramePadding.x;
  float x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + st.ItemSpacing.x + w;
  if (x <= ImGui::GetContentRegionMax().x) { ImGui::SameLine(); }
}

static void row_label(const char* label, float x) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(x);
}

// -/+ over steps: the next listed value below / above *v.
template <size_t N> static void stepper(const char* id, long long* v, const long long (&steps)[N], float width) {
  ImGui::PushID(id);
  if (ImGui::Button("-", ImVec2(width, 0.0f))) {
    for (size_t i = N; i-- > 0;) {
      if (steps[i] < *v) {
        *v = steps[i];
        break;
      }
    }
  }
  ImGui::SameLine();
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", *v);
  ImGui::AlignTextToFramePadding();
  float tw = ImGui::CalcTextSize(buf).x, x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + (2.0f * width - tw) * 0.5f);
  ImGui::TextUnformatted(buf);
  ImGui::SameLine(x + 2.0f * width + ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::Button("+", ImVec2(width, 0.0f))) {
    for (long long s : steps) {
      if (s > *v) {
        *v = s;
        break;
      }
    }
  }
  ImGui::PopID();
}

static void begin_fullscreen(const char* name, const PlatformInsets& in) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + in.left, vp->Pos.y + in.top));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x - in.left - in.right, vp->Size.y - in.top - in.bottom));
  ImGui::Begin(name, nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
}

// Settings rows: label column width (longest label) and choice button width.
static float settings_label_w() { return ImGui::CalcTextSize("Validation").x + 2.0f * ImGui::GetStyle().ItemSpacing.x; }
static float settings_button_w(const Menu* m) { return 64.0f * m->scale; }

static void build_settings(Menu* m, Session* s) {
  float label_w = settings_label_w(), w = settings_button_w(m);
  row_label("Vulkan", label_w);
  if (choice("1.1", s->api == GpuApi::vk11, true, w)) {
    m->action = Menu::Action::vulkan;
    m->want_api = GpuApi::vk11;
  }
  ImGui::SameLine();
  if (choice("1.3", s->api == GpuApi::vk13, m->vk13, w)) {
    m->action = Menu::Action::vulkan;
    m->want_api = GpuApi::vk13;
  }
  if (!m->vk13) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("1.3 not supported");
  }
#ifdef PXB_VALIDATION
  row_label("Validation", label_w);
  for (bool on : {false, true}) {
    if (on) { ImGui::SameLine(); }
    if (choice(on ? "on##val" : "off##val", s->validation == on, true, w)) {
      m->action = Menu::Action::validation;
      m->want_validation = on;
    }
  }
#endif
  row_label("Offscreen", label_w);
  for (bool on : {false, true}) {
    if (on) { ImGui::SameLine(); }
    if (choice(on ? "on##off" : "off##off", m->ro.offscreen == on, true, w)) { m->ro.offscreen = on; }
  }
  row_label("Time limit", label_w);
  for (bool on : {false, true}) {
    if (on) { ImGui::SameLine(); }
    if (choice(on ? "60 s##time" : "off##time", m->time_limit == on, true, w)) { m->time_limit = on; }
  }
  float sw = 0.5f * w;
  row_label("Frames", label_w);
  stepper("frames", &m->ro.frames, FRAME_STEPS, sw);
  row_label("Warm-up", label_w);
  stepper("warmup", &m->ro.frames_warmup, WARMUP_STEPS, sw);
}

static void build_scripts(Menu* m) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Scripts");
  same_line_if_fits("Run all demos");
  ImGui::BeginDisabled(m->demos.empty());
  if (ImGui::Button("Run all demos")) { m->action = Menu::Action::run_all; }
  ImGui::EndDisabled();
  if (m->scripts.empty()) { ImGui::TextDisabled("no scripts/*.lua found"); }
  for (size_t i = 0; i < m->scripts.size(); i++) {
    ImGui::PushID((int)i);
    if (ImGui::Button("Run")) {
      m->action = Menu::Action::run;
      m->run_index = i;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path_base_name(m->scripts[i]));
    ImGui::PopID();
  }
}

static void ms_cell(double v, bool align) {
  ImGui::TableNextColumn();
  if (align) { ImGui::AlignTextToFramePadding(); }
  if (v >= 0) {
    ImGui::Text("%.3f", v);
  } else {
    ImGui::TextDisabled("-");
  }
}

static void build_results(Menu* m) {
  ImGui::AlignTextToFramePadding();
  ImGui::Text("Results (%zu)", m->results.size());
  same_line_if_fits("Clear");
  ImGui::BeginDisabled(m->results.empty());
  if (ImGui::Button("Clear")) { m->action = Menu::Action::clear; }
  same_line_if_fits("Save results");
  if (ImGui::Button("Save results")) { m->action = Menu::Action::save; }
  static const std::pair<const char*, Menu::Action> share_buttons[] = {
      {"Share", Menu::Action::share}, {"Share as text", Menu::Action::share_text}, {"Copy", Menu::Action::copy}};
  for (const auto& b : share_buttons) {
    same_line_if_fits(b.first);
    if (ImGui::Button(b.first)) { m->action = b.second; }
  }
  ImGui::EndDisabled();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PopStyleColor();
  ImGui::BeginChild("results", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
  if (m->results.empty()) { ImGui::TextDisabled("no runs yet"); }
  if (!m->results.empty() &&
      ImGui::BeginTable("results", 6,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
    ImGui::TableSetupColumn("script / timer", ImGuiTableColumnFlags_WidthStretch);
    for (const char* c : {"vk", "opts", "fps", "gpu", "cpu"}) {
      ImGui::TableSetupColumn(c, ImGuiTableColumnFlags_WidthFixed);
    }
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < m->results.size(); i++) {
      const RunResult& r = m->results[i];
      ImGui::PushID((int)i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const char* state = !r.ok ? (r.stopped ? " (stopped)" : " (failed)") : r.time_limited ? " (time limit)" : "";
      bool open = ImGui::TreeNodeEx("run", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_FramePadding, "%s%s",
                                    path_base_name(r.script), state);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(gpu_api_name(r.api));
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(r.validation ? (r.offscreen ? "val off" : "val") : (r.offscreen ? "off" : "-"));
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (r.frames > 0) {
        ImGui::Text("%.1f", r.fps);
      } else {
        ImGui::TextDisabled("-");
      }
      double busy = -1.0, interval = -1.0;
      for (const TimerSummary& t : r.timers) {
        if (t.name == "busy") { busy = t.gpu_ms; }
        if (t.name == "interval") { interval = t.cpu_ms; }
      }
      ms_cell(busy, true);
      ms_cell(interval, true);
      if (open) {
        for (const TimerSummary& t : r.timers) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          float indent = 12.0f * m->scale;
          ImGui::Indent(indent);
          ImGui::TextUnformatted(t.name.c_str());
          ImGui::Unindent(indent);
          ImGui::TableNextColumn();
          ImGui::TableNextColumn();
          ImGui::TableNextColumn();
          ms_cell(t.gpu_ms, false);
          ms_cell(t.cpu_ms, false);
        }
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  drag_scroll(m);
  ImGui::EndChild();
}

static void build_menu(Menu* m, Session* s) {
  begin_fullscreen("menu", m->insets);
  char thermal[64];
  snprintf(thermal, sizeof(thermal), "  thermal headroom %.2f", thermal_latest().headroom);
  ImGui::Text("pxBenchy %s  %s  git %s", PXB_VERSION, m->device_summary.c_str(), PXB_GIT_HASH);
  // on the header line when it fits (2304 px at scale 1.75 does not), else first on the status line
  float x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
  bool header = x + ImGui::CalcTextSize(thermal).x <= ImGui::GetContentRegionMax().x;
  if (header) { ImGui::SameLine(); }
  ImGui::TextDisabled("%s", header ? thermal : thermal + 2);
  if (!header) { ImGui::SameLine(0.0f, ImGui::CalcTextSize("  ").x); }
  if (!m->status.empty()) {
    ImGui::TextWrapped("%s", m->status.c_str()); // run + "stopped after" + saved URI can exceed the width
  } else {
    ImGui::TextDisabled("Back / Esc: exit. During a run: stop it.");
  }
  if (!m->update_tag.empty()) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("pxBenchy %s available", m->update_tag.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Download")) { m->action = Menu::Action::update; }
  }
  // left: settings + scripts, one scroll; at least as wide as a settings row (label + 2 buttons + scrollbar)
  const ImGuiStyle& st = ImGui::GetStyle();
  float row_w = settings_label_w() + 2.0f * settings_button_w(m) + st.ItemSpacing.x + st.ScrollbarSize;
  float left_w = std::floor(std::max(ImGui::GetContentRegionAvail().x * 0.32f, row_w));
  ImGui::BeginChild("left", ImVec2(left_w, 0.0f));
  build_settings(m, s);
  ImGui::Separator();
  build_scripts(m);
  drag_scroll(m);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("right", ImVec2(0.0f, 0.0f));
  build_results(m);
  ImGui::EndChild();
  ImGui::End();
}

// "Run all": `scripts/demo*.lua` from platform_list_scripts, sorted.
static std::vector<std::string> suite_scripts() {
  std::vector<std::string> out;
  for (const std::string& p : platform_list_scripts()) {
    if (p.rfind("scripts/demo", 0) == 0) { out.push_back(p); }
  }
  return out;
}

// Runs script with the menu's options: prints the report, sets the status and adds the run to the results table.
static RunResult run_one(Menu* m, Session* s, const std::string& script, long long time_limit) {
  RunOptions ro = m->ro;
  ro.script = script;
  ro.time_limit = time_limit;
  RunResult r = run_script(s, ro);
  print_result(r);
  set_status(m, r);
  if (!r.quit) { m->results.push_back(r); }
  return r;
}

// Runs the action chosen in the last frame; the device is idle (session_ui_frame).
static void run_action(Menu* m, Session* s) {
  switch (m->action) {
    case Menu::Action::none: break;
    case Menu::Action::vulkan:
    case Menu::Action::validation: {
      GpuApi api = m->action == Menu::Action::vulkan ? m->want_api : s->api;
      bool val = m->action == Menu::Action::validation ? m->want_validation : s->validation;
      printf("menu: switch to vulkan %s, validation %s\n", gpu_api_name(api), val ? "on" : "off");
      fflush(stdout);
      if (session_switch(s, api, val)) {
        m->status = std::string("vulkan ") + gpu_api_name(api) + (val ? ", validation on" : ", validation off");
      } else {
        m->status = std::string("switch to vulkan ") + gpu_api_name(api) + " failed (see log)";
      }
      // also after a failed switch: the previous device may be restored, or none is up
      m->device_summary = s->dev_ready ? device_info_summary(&s->dev) : "";
      break;
    }
    case Menu::Action::run: run_one(m, s, m->scripts[m->run_index], 0); break;
    case Menu::Action::run_all: {
      size_t runs = 0;
      while (runs < m->demos.size()) {
        printf("menu: run all %zu/%zu %s\n", runs + 1, m->demos.size(), m->demos[runs].c_str());
        fflush(stdout);
        RunResult r = run_one(m, s, m->demos[runs++], m->time_limit ? MENU_TIME_LIMIT : 0);
        if (r.quit || r.stopped) { break; }
      }
      if (runs < m->demos.size() && !s->quit) { m->status += "  (run all stopped after " + std::to_string(runs) + ")"; }
      break;
    }
    case Menu::Action::clear: m->results.clear(); break;
    case Menu::Action::save:
      m->status.clear();
      save_results(m, s);
      break;
    case Menu::Action::share:
    case Menu::Action::share_text:
    case Menu::Action::copy: share(m, s, m->action); break;
    case Menu::Action::update: {
      std::string msg;
      m->status = platform_open_url(RELEASES_PAGE, &msg) ? "Download: " + msg : "Download failed: " + msg;
      break;
    }
  }
  m->action = Menu::Action::none;
}

int menu_run(Session* s, const RunOptions& ro) {
  Menu m;
  m.ro = ro;
  m.ro.script.clear();
  m.ro.screenshot.clear();
  m.vk13 = gpu_vk13_supported();
  m.scale = platform_ui_scale();
  m.scripts = platform_list_scripts();
  m.demos = suite_scripts();
  m.device_summary = s->dev_ready ? device_info_summary(&s->dev) : "";
  m.update = std::make_shared<UpdateCheck>();
#if defined(__ANDROID__) // desktop builds come from source: no check
  std::thread([u = m.update] { update_check(u.get()); }).detach();
#endif
  int redraw = REDRAW_FRAMES;
  while (!s->quit) {
    uint32_t ev = session_pump(s);
    if (ev & (PXB_EV_QUIT | PXB_EV_BACK)) { break; }
    if (!m.update_seen && m.update->done.load()) {
      m.update_seen = true;
      m.update_tag = m.update->tag;
      redraw = REDRAW_FRAMES;
    }
    if (ev & (PXB_EV_RESIZED | PXB_EV_SURFACE_LOST)) {
      redraw = REDRAW_FRAMES;
      m.scale = platform_ui_scale(); // moved to another display / density change
    }
    if (ui_input(s)) { redraw = REDRAW_FRAMES; }
    if (redraw == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(IDLE_POLL_MS));
      continue;
    }
    m.insets = platform_safe_insets(); // per drawn frame: the taskbar can be shown / hidden without a resize
    if (!ui_frame(s, m.scale, [&] { build_menu(&m, s); })) {
      if (!s->dev_ready) {
        fprintf(stderr, "error: menu: no device\n");
        return 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(IDLE_POLL_MS)); // minimized, or swapchain dropped: retry
      continue;
    }
    redraw--;
    if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) { redraw = std::max(redraw, 1); }
    if (m.action != Menu::Action::none) {
      run_action(&m, s);
      redraw = REDRAW_FRAMES;
    }
  }
  return 0;
}
