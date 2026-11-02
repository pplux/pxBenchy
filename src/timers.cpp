#include "timers.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include "json.h"
#include "lua/gpu_api.h"

int64_t timer_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void timer_set_frame_begin(Timers* ts) {
  int64_t t = timer_now_ns();
  ts->pending_interval = ts->has_prev ? t - ts->prev_frame : -1;
  ts->has_prev = true;
  ts->prev_frame = t;
}

void timer_set_frame_end(Timers* ts, bool keep) {
  if (keep) {
    if (ts->pending_interval >= 0) { ts->cpu_interval.push_back(ts->pending_interval); }
    for (const Timers::Sample& p : ts->pending) { ts->cpu[p.id].ns.push_back(p.ns); }
  } else {
    timer_set_break_interval(ts);
  }
  ts->pending.clear();
  ts->pending_interval = -1;
}

void timer_set_break_interval(Timers* ts) {
  ts->has_prev = false;
  for (CpuTimerOpen& o : ts->open) { o.discard = true; }
}

std::string timer_set_close_open(Timers* ts) {
  std::string names;
  for (size_t i = 0; i < ts->open.size(); i++) {
    if (i) { names += ", "; }
    names += ts->cpu[ts->open[i].id].name;
  }
#ifdef TRACY_ENABLE
  for (size_t i = ts->open.size(); i-- > 0;) { ___tracy_emit_zone_end(ts->open[i].zone); }
#endif
  ts->open.clear();
  return names;
}

// Nearest rank on sorted samples: index ceil(p/100 * n) - 1.
static double pct(const std::vector<double>& s, size_t p) { return s[(p * s.size() + 99) / 100 - 1]; }

// Non-empty samples (ns) -> total, mean, sd, median, p99, p95, min, max in ms (sd: sample standard deviation, 0 for
// n=1).
static constexpr int STATS = 8;
static void stats(std::vector<double> v, double out[STATS]) {
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) { sum += x; }
  double mean = sum / v.size(), sq = 0;
  for (double x : v) { sq += (x - mean) * (x - mean); }
  double var = v.size() > 1 ? sq / (v.size() - 1) : 0.0;
  double ns[STATS] = {sum, mean, 0, pct(v, 50), pct(v, 99), pct(v, 95), v.front(), v.back()};
  for (int i = 0; i < STATS; i++) { out[i] = ns[i] / 1e6; }
  out[2] = std::sqrt(var) / 1e6;
}

// One report row as cells: kind, name, n, total (ms), then mean..max as "ms\tHz" (Hz = 1000 / ms, printed as
// "ms (Hz)" with both parts aligned per column). sd's Hz part is "+-" the sd in Hz, to first order 1000 * sd / mean^2.
static std::vector<std::string> row(const char* kind, const std::string& name, const std::vector<double>& v) {
  std::vector<std::string> r = {kind, name, std::to_string(v.size())};
  if (v.empty()) {
    r.insert(r.end(), STATS, "-");
    return r;
  }
  double ms[STATS];
  stats(v, ms);
  char buf[64];
  for (int i = 0; i < STATS; i++) {
    if (i == 2 && ms[1] > 0) {
      snprintf(buf, sizeof(buf), "%.3f\t+-%.1f", ms[2], 1000.0 * ms[2] / (ms[1] * ms[1]));
    } else if (i == 0 || i == 2 || ms[i] <= 0) {
      snprintf(buf, sizeof(buf), "%.3f", ms[i]);
    } else {
      snprintf(buf, sizeof(buf), "%.3f\t%.1f", ms[i], 1000.0 / ms[i]);
    }
    r.push_back(buf);
  }
  return r;
}

std::string timers_report(const Timers* t, const GpuApiTable* gpu) {
  const GpuTimers* gt = &t->gpu;
  std::string o;
  std::vector<std::vector<std::string>> rows = {
      {"kind", "name", "n", "total", "mean", "sd", "median", "p99", "p95", "min", "max"}};
  rows.push_back(row("gpu", "busy", gpu->timers_busy(gt)));
  rows.push_back(row("gpu", "interval", gpu->timers_interval(gt)));
  rows.push_back(row("cpu", "interval", std::vector<double>(t->cpu_interval.begin(), t->cpu_interval.end())));
  for (const Timers::Entry& e : t->order) {
    if (e.gpu) {
      rows.push_back(row("gpu", gt->user[e.id].name, gpu->timer_results(gt, e.id)));
    } else {
      const CpuTimer& c = t->cpu[e.id];
      rows.push_back(row("cpu", c.name, std::vector<double>(c.ns.begin(), c.ns.end())));
    }
  }
  // gpu rows first, then cpu; by name within each kind
  std::stable_sort(rows.begin() + 1, rows.end(), [](const auto& a, const auto& b) {
    if ((a[0] == "gpu") != (b[0] == "gpu")) { return a[0] == "gpu"; }
    return a[1] < b[1];
  });
  // per column: w = ms part (or whole cell), hw = Hz part (0 = column has none)
  constexpr size_t COLS = 3 + STATS;
  size_t w[COLS] = {}, hw[COLS] = {};
  for (const auto& r : rows) {
    for (size_t i = 0; i < r.size(); i++) {
      size_t tab = r[i].find('\t');
      w[i] = std::max(w[i], tab == std::string::npos ? r[i].size() : tab);
      if (tab != std::string::npos) { hw[i] = std::max(hw[i], r[i].size() - tab - 1); }
    }
  }
  for (size_t i = 0; i < COLS; i++) { // a header wider than "ms (Hz)" widens the ms part
    if (hw[i] && rows[0][i].size() > w[i] + hw[i] + 3) { w[i] = rows[0][i].size() - hw[i] - 3; }
  }
  for (size_t k = 0; k < rows.size(); k++) {
    const auto& r = rows[k];
    for (size_t i = 0; i < r.size(); i++) {
      const char* sep = i ? " | " : "";
      size_t tab = r[i].find('\t');
      if (i < 2) { // kind, name left-aligned
        str_appendf(&o, "%s%-*s", sep, (int)w[i], r[i].c_str());
      } else if (k == 0 || !hw[i]) { // header, or a column without Hz
        str_appendf(&o, "%s%*s", sep, (int)(w[i] + (hw[i] ? hw[i] + 3 : 0)), r[i].c_str());
      } else if (tab == std::string::npos) { // "-" or ms only
        str_appendf(&o, "%s%*s%*s", sep, (int)w[i], r[i].c_str(), (int)(hw[i] + 3), "");
      } else {
        str_appendf(&o, "%s%*s (%*s)", sep, (int)w[i], r[i].substr(0, tab).c_str(), (int)hw[i], r[i].c_str() + tab + 1);
      }
    }
    o += k == 0 ? "  (ms (Hz))\n" : "\n";
  }
  return o;
}

std::vector<TimerRow> timer_user_rows(const Timers* t) {
  const GpuTimers* gt = &t->gpu;
  std::vector<TimerRow> out;
  for (const Timers::Entry& e : t->order) {
    const std::string& name = e.gpu ? gt->user[e.id].name : t->cpu[e.id].name;
    auto it = std::find_if(out.begin(), out.end(), [&](const TimerRow& r) { return r.name == name; });
    if (it == out.end()) { it = out.insert(out.end(), TimerRow{name}); }
    (e.gpu ? it->gpu : it->cpu) = (int)e.id;
  }
  return out;
}

std::vector<TimerSummary> timers_summary(const Timers* t, const GpuApiTable* gpu) {
  const GpuTimers* gt = &t->gpu;
  std::vector<TimerSummary> out;
  auto add = [&](const std::string& name, bool is_gpu, std::vector<double> v) {
    auto it = std::find_if(out.begin(), out.end(), [&](const TimerSummary& t) { return t.name == name; });
    if (it == out.end()) { it = out.insert(out.end(), TimerSummary{name}); }
    if (v.empty()) { return; }
    std::sort(v.begin(), v.end());
    (is_gpu ? it->gpu_ms : it->cpu_ms) = pct(v, 50) / 1e6;
  };
  add("busy", true, gpu->timers_busy(gt));
  add("interval", true, gpu->timers_interval(gt));
  add("interval", false, std::vector<double>(t->cpu_interval.begin(), t->cpu_interval.end()));
  for (const TimerRow& r : timer_user_rows(t)) {
    if (r.gpu >= 0) { add(r.name, true, gpu->timer_results(gt, r.gpu)); }
    if (r.cpu >= 0) {
      const CpuTimer& c = t->cpu[r.cpu];
      add(r.name, false, std::vector<double>(c.ns.begin(), c.ns.end()));
    }
  }
  return out;
}

TimerMark timer_mark(const Timers* t, const GpuApiTable* gpu) {
  const GpuTimers* gt = &t->gpu;
  TimerMark m;
  m.busy = gpu->timers_busy(gt).size();
  m.gpu_interval = gpu->timers_interval(gt).size();
  m.cpu_interval = t->cpu_interval.size();
  for (uint32_t i = 0; i < gt->user.size(); i++) { m.gpu.push_back(gpu->timer_results(gt, i).size()); }
  for (const CpuTimer& c : t->cpu) { m.cpu.push_back(c.ns.size()); }
  return m;
}

// ends: sample count at each window end. Appends ,"series":{"mean_ms":[..],"min_ms":[..],"max_ms":[..]}.
static void json_series(std::string* o, const std::vector<double>& v, const std::vector<size_t>& ends) {
  std::string cols[3];
  char buf[32];
  size_t from = 0;
  for (size_t i = 0; i < ends.size(); i++) {
    const char* sep = i ? "," : "";
    if (from >= ends[i]) {
      for (std::string& c : cols) { c += sep + std::string("null"); }
      continue;
    }
    double sum = 0, lo = v[from], hi = v[from];
    for (size_t k = from; k < ends[i]; k++) {
      sum += v[k];
      lo = std::min(lo, v[k]);
      hi = std::max(hi, v[k]);
    }
    double ms[3] = {sum / (ends[i] - from) / 1e6, lo / 1e6, hi / 1e6};
    for (int c = 0; c < 3; c++) {
      snprintf(buf, sizeof(buf), "%s%.4f", sep, ms[c]);
      cols[c] += buf;
    }
    from = ends[i];
  }
  *o += ",\"series\":{\"mean_ms\":[" + cols[0] + "],\"min_ms\":[" + cols[1] + "],\"max_ms\":[" + cols[2] + "]}";
}

static void json_row(std::string* o, const char* kind, const std::string& name, const std::vector<double>& v,
                     const std::vector<size_t>& ends) {
  static const char* const keys[] = {"total_ms", "mean_ms", "sd_ms",  "median_ms",
                                     "p99_ms",   "p95_ms",  "min_ms", "max_ms"};
  char buf[64];
  if (o->size() > 1) { *o += ","; }
  snprintf(buf, sizeof(buf), "{\"kind\":\"%s\",\"name\":", kind);
  *o += buf + json_string(name);
  snprintf(buf, sizeof(buf), ",\"n\":%zu", v.size());
  *o += buf;
  double ms[STATS] = {};
  if (!v.empty()) { stats(v, ms); }
  for (int i = 0; i < STATS; i++) {
    if (v.empty()) {
      snprintf(buf, sizeof(buf), ",\"%s\":null", keys[i]);
    } else {
      snprintf(buf, sizeof(buf), ",\"%s\":%.6f", keys[i], ms[i]);
    }
    *o += buf;
  }
  json_series(o, v, ends);
  *o += "}";
}

std::string timers_report_json(const Timers* t, const GpuApiTable* gpu, const std::vector<TimerMark>& marks) {
  const GpuTimers* gt = &t->gpu;
  auto ends = [&](auto count) {
    std::vector<size_t> e;
    for (const TimerMark& m : marks) { e.push_back(count(m)); }
    return e;
  };
  auto at = [](const std::vector<size_t>& v, uint32_t id) { return id < v.size() ? v[id] : 0; };
  std::string o = "[";
  json_row(&o, "gpu", "busy", gpu->timers_busy(gt), ends([](const TimerMark& m) { return m.busy; }));
  json_row(&o, "gpu", "interval", gpu->timers_interval(gt), ends([](const TimerMark& m) { return m.gpu_interval; }));
  json_row(&o, "cpu", "interval", std::vector<double>(t->cpu_interval.begin(), t->cpu_interval.end()),
           ends([](const TimerMark& m) { return m.cpu_interval; }));
  for (const Timers::Entry& e : t->order) {
    if (e.gpu) {
      json_row(&o, "gpu", gt->user[e.id].name, gpu->timer_results(gt, e.id),
               ends([&](const TimerMark& m) { return at(m.gpu, e.id); }));
    } else {
      const CpuTimer& c = t->cpu[e.id];
      json_row(&o, "cpu", c.name, std::vector<double>(c.ns.begin(), c.ns.end()),
               ends([&](const TimerMark& m) { return at(m.cpu, e.id); }));
    }
  }
  return o + "]";
}
