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
