#include "platform/thermal.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <tracy/Tracy.hpp>

static std::thread s_thread;
static std::mutex s_mutex;
static std::condition_variable s_cv;
static bool s_stop = false;
static PlatformThermal s_latest;

#if !defined(__ANDROID__) && !defined(__APPLE__)
void platform_thermal(PlatformThermal* out) { *out = PlatformThermal{}; }
const char* platform_thermal_status_name(float) { return "?"; }
#endif

void thermal_start() {
  if (s_thread.joinable()) { return; }
  s_stop = false;
  s_latest = PlatformThermal{}; // a new session (Android relaunch in a cached process) starts from zeros
  s_thread = std::thread([] {
    tracy::SetThreadName("thermal");
    std::unique_lock<std::mutex> lock(s_mutex);
    while (!s_stop) {
      lock.unlock();
      PlatformThermal t;
      platform_thermal(&t);
      TracyPlot("thermal headroom", t.headroom);
      TracyPlot("thermal status", t.status);
      TracyPlot("battery C", t.battery_c);
#ifdef TRACY_ENABLE
      if (t.gpu_mhz > 0.0f) { TracyPlot("gpu MHz", t.gpu_mhz); }
      static const char* const cpu_names[4] = {"cpu0 MHz", "cpu1 MHz", "cpu2 MHz", "cpu3 MHz"};
      for (int i = 0; i < 4; i++) {
        if (t.cpu_mhz[i] > 0.0f) { TracyPlot(cpu_names[i], t.cpu_mhz[i]); }
      }
#endif
      lock.lock();
      s_latest = t;
      s_cv.wait_for(lock, std::chrono::seconds(1), [] { return s_stop; });
    }
  });
}

void thermal_stop() {
  if (!s_thread.joinable()) { return; }
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_stop = true;
  }
  s_cv.notify_all();
  s_thread.join();
}

PlatformThermal thermal_latest() {
  std::lock_guard<std::mutex> lock(s_mutex);
  return s_latest;
}
