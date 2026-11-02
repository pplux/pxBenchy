#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <tracy/TracyC.h>

struct GpuApiTable;
struct GpuTimers;

struct CpuTimer {
  std::string name;
  std::vector<int64_t> ns;   // per occurrence
  const void* loc = nullptr; // ___tracy_source_location_data* (TRACY_ENABLE only), never freed
};

struct CpuTimerOpen {
  uint32_t id;
  int64_t t0;
  bool discard;       // started before the warm-up reset: records nothing
  TracyCZoneCtx zone; // TRACY_ENABLE only
};

// CPU user timers, the automatic cpu interval and the creation order of all user timers (for the report). One per run
// (run_script in session.cpp), outlives the Lua state.
struct TimerSet {
  struct Entry {
    bool gpu;
    uint32_t id; // CpuTimer index, or GpuTimers::user index
  };
  std::vector<CpuTimer> cpu;
  std::vector<CpuTimerOpen> open; // LIFO
  std::vector<Entry> order;
  std::vector<int64_t> cpu_interval; // ns between consecutive frame begins
  bool has_prev = false;
  bool skip_next = false; // the next cpu interval spans an offscreen blit
  int64_t prev_frame = 0;
};

int64_t timer_now_ns();
void timer_set_frame_begin(TimerSet* ts); // at each successful frame begin (FrameMark), records the cpu interval
// Offscreen blit: the cpu interval to the next frame begin is dropped, open CPU timers discard their current
// occurrence.
void timer_set_break_interval(TimerSet* ts);
// Drops CPU results and the cpu interval; open CPU timers discard their current occurrence.
void timer_set_clear(TimerSet* ts);
// The stats (total, mean, sd, median, p99, p95, min, max) of the automatic and user timers as a text table, gpu rows
// before cpu rows, by name within each. GPU results must be resolved.
std::string timers_report(const TimerSet* ts, const GpuTimers* gt, const GpuApiTable* gpu);
// Same rows as a JSON array (automatic timers, then user timers in creation order): [{"kind", "name", "n", "total_ms",
// "mean_ms", "sd_ms", "median_ms", "p99_ms", "p95_ms", "min_ms", "max_ms"}, ...], stats null when n is 0.
std::string timers_report_json(const TimerSet* ts, const GpuTimers* gt, const GpuApiTable* gpu);
// User timers, one row per name (a gpu and a cpu timer of the same name share it), in creation order
// (TimerSet::order). gpu / cpu: index into GpuTimers::user / TimerSet::cpu, -1 = none.
struct TimerRow {
  std::string name;
  int gpu = -1, cpu = -1;
};
std::vector<TimerRow> timer_user_rows(const TimerSet* ts, const GpuTimers* gt);
// Median per timer for the menu's results table: `busy` (gpu), `interval` (gpu and cpu), then the user timers in
// creation order, a gpu and a cpu timer of the same name in one row. -1 = no such timer or no samples.
struct TimerSummary {
  std::string name;
  double gpu_ms = -1.0;
  double cpu_ms = -1.0;
};
std::vector<TimerSummary> timers_summary(const TimerSet* ts, const GpuTimers* gt, const GpuApiTable* gpu);
