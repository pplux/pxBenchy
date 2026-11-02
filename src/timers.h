#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <tracy/TracyC.h>
#include "gpu/vk.h"

struct GpuApiTable;

struct CpuTimer {
  std::string name;
  std::vector<int64_t> ns;   // per occurrence
  const void* loc = nullptr; // ___tracy_source_location_data* (TRACY_ENABLE only), never freed
};

struct CpuTimerOpen {
  uint32_t id;
  int64_t t0;
  bool discard;       // spans a break (timer_set_break_interval, a discarded frame end): records nothing
  TracyCZoneCtx zone; // TRACY_ENABLE only
};

// All timers of a run: the GPU timers, CPU user timers, the automatic cpu interval and the creation order of all user
// timers (for the report). One per run (run_script in session.cpp), outlives the Lua state.
struct Timers {
  GpuTimers gpu;
  struct Entry {
    bool gpu;
    uint32_t id; // CpuTimer index, or GpuTimers::user index
  };
  std::vector<CpuTimer> cpu;
  std::vector<CpuTimerOpen> open; // LIFO
  std::vector<Entry> order;
  std::vector<int64_t> cpu_interval; // ns between consecutive frame begins (committed)
  // the open frame's results, committed or discarded by timer_set_frame_end: the cpu interval into it and the CPU
  // samples stopped in it
  struct Sample {
    uint32_t id; // CpuTimer index
    int64_t ns;
  };
  std::vector<Sample> pending;
  int64_t pending_interval = -1; // -1 = none
  bool has_prev = false;         // prev_frame starts the interval to the next frame begin (false after a break)
  int64_t prev_frame = 0;
};

int64_t timer_now_ns();
// At each successful frame begin (FrameMark): the cpu interval into this frame becomes pending.
void timer_set_frame_begin(Timers* ts);
// At each frame end: keep commits the frame's pending interval and CPU samples to the results, discard drops them and
// breaks like timer_set_break_interval.
void timer_set_frame_end(Timers* ts, bool keep);
// Between frames (offscreen capture / blit, PXB_FRAMES_IN_FLIGHT after the copy, pause, unavailable frame): the cpu
// interval to the next frame begin is dropped, open CPU timers discard their current occurrence.
void timer_set_break_interval(Timers* ts);
// Ends the open CPU timers without a result (and their Tracy zones). Returns their names, ", " separated ("" = none).
std::string timer_set_close_open(Timers* ts);
// The stats (total, mean, sd, median, p99, p95, min, max) of the automatic and user timers as a text table, gpu rows
// before cpu rows, by name within each. GPU results must be resolved.
std::string timers_report(const Timers* t, const GpuApiTable* gpu);
// Sample counts of every timer at the end of a 1 s window: window i holds the samples between mark i-1 (0 for the
// first) and mark i. gpu / cpu are indexed like Timers::gpu.user / Timers::cpu; a timer created later has a shorter
// vector (no samples before).
struct TimerMark {
  size_t busy = 0, gpu_interval = 0, cpu_interval = 0;
  std::vector<size_t> gpu, cpu;
};
TimerMark timer_mark(const Timers* t, const GpuApiTable* gpu);
// Mean in ms of samples (ns) [from, to), -1 when empty.
template <typename T> double timer_mean_ms(const std::vector<T>& v, size_t from, size_t to) {
  if (from >= to) { return -1.0; }
  double sum = 0;
  for (size_t i = from; i < to; i++) { sum += (double)v[i]; }
  return sum / (to - from) / 1e6;
}
// Same rows as a JSON array (automatic timers, then user timers in creation order): [{"kind", "name", "n", "total_ms",
// "mean_ms", "sd_ms", "median_ms", "p99_ms", "p95_ms", "min_ms", "max_ms", "series"}, ...], stats null when n is 0.
// series: {"mean_ms", "min_ms", "max_ms"}, one value per window of `marks` (null when the window has no sample).
std::string timers_report_json(const Timers* t, const GpuApiTable* gpu, const std::vector<TimerMark>& marks);
// User timers, one row per name (a gpu and a cpu timer of the same name share it), in creation order
// (Timers::order). gpu / cpu: index into Timers::gpu.user / Timers::cpu, -1 = none.
struct TimerRow {
  std::string name;
  int gpu = -1, cpu = -1;
};
std::vector<TimerRow> timer_user_rows(const Timers* t);
// Median per timer for the menu's results table: `busy` (gpu), `interval` (gpu and cpu), then the user timers in
// creation order, a gpu and a cpu timer of the same name in one row. -1 = no such timer or no samples.
struct TimerSummary {
  std::string name;
  double gpu_ms = -1.0;
  double cpu_ms = -1.0;
};
std::vector<TimerSummary> timers_summary(const Timers* t, const GpuApiTable* gpu);
