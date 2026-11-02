#pragma once
#include "platform/platform.h"

// Background thread sampling platform_thermal once per second into Tracy plots ("thermal headroom", "thermal status",
// "battery C") and a snapshot for the HUD. Never blocks the caller beyond start/stop.
void thermal_start();
void thermal_stop();              // wakes and joins the thread; no-op if not started
PlatformThermal thermal_latest(); // last sample (zeros until the first one)
