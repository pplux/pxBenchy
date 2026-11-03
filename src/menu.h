#pragma once
#include "session.h"

// Launcher menu, shown when no script is given (Android launcher start, macOS without args): pick
// Vulkan 1.1 / 1.3 and validation (session_switch), offscreen, frames and warm-up; run one script or all of them
// (back to back); results of the session in a table. The initial state comes
// from ro and the session's api / validation. Redraws only on input or state change. Back (Android) / Escape (macOS)
// or closing the window leaves it; Back during a run stops the run and returns here. Returns the exit code.
// start: command line actions done once at menu start, as the buttons do them (no input needed: adb).
struct MenuStart {
  long long time_limit = 30; // initial time limit, s (0 = off)
  bool run_all = false;      // press "Run all" once the menu is up
  bool save = false;         // then Save and leave the menu (exit code 1 when the save fails); needs run_all
};
int menu_run(Session* s, const RunOptions& ro, const MenuStart& start);
