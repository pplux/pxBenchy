# Running benchmarks

## Command line

```
pxbenchy [--validation] [--vulkan 1.1|1.3] [--frames N] [--frames-warmup N] [--no-hud] [--offscreen] [--json] [--screenshot file.png] [--screenshot-frame N] <script>
```

| option | default | meaning |
|---|---|---|
| `--validation` | off | enable the Vulkan validation layer (with synchronization validation); use while developing a script, leave off for numbers |
| `--vulkan 1.1\|1.3` | 1.1 | Vulkan path: 1.1 core (render passes, sync1) or 1.3 (dynamic rendering, synchronization2). 1.3 needs a device with 1.3 + dynamicRendering + synchronization2: no fallback, exits 1 otherwise. Shown as `vulkan=` on the startup line, `vkX.Y` on the HUD, in the `run:` line and as `"vulkan"` in the JSON report |
| `--frames N` | 6000 | measured frames (`pxb.args.frames`); 0 = run until the window closes |
| `--frames-warmup N` | 60 | frames before timers are cleared (`pxb.args.frames_warmup`) |
| `--no-hud` | HUD on | no on-screen stats |
| `--offscreen` | off | render to an own image, no vsync; about once per second the last frame is copied to the screen |
| `--json` | off | print the report as one JSON line |
| `--screenshot file.png` | off | save frame `--screenshot-frame` as an RGBA8 PNG (scene only, no HUD, view orientation), print `screenshot: path (frame N, WxH)` and exit 0 without a report; error (exit 1) if the script ends first. Android: use an app-writable absolute path |
| `--screenshot-frame N` | 1 | 1-based frame (HUD frame number) to capture; a skipped frame moves it to the next one drawn |

## Desktop (macOS)

```
cmake --build build && ./build/pxbenchy --frames 1000 scripts/demo1_instancing.lua
```

`build-release` is the Release build used for numbers; `build-notracy` has Tracy off.

## Android

```
android/build_apk.sh && adb install -r build-android/pxbenchy.apk
adb shell am start -S -n com.pplux.pxbenchy/android.app.NativeActivity --es args "'--frames 1000 scripts/triangle.lua'"
adb logcat -s pxbenchy
```

Or in one step: `android/install_and_run.sh [app args...]` builds, installs, launches, follows the log until the app's
last line (`exit code: N`) and exits with that code.

`android/build_apk.sh` configures `build-android` when missing (Release, arm64-v8a, android-31), builds it, and packs
the `.so`, scripts and assets into a debug-signed APK. The startup line shows the build's git hash (`git=...`, also on
the HUD) to confirm what is installed. Tracy needs `adb forward tcp:8086 tcp:8086`.

## Report

At the end of the run, one row per timer, gpu rows first, then cpu, by name:

- `gpu busy`, `gpu interval`, `cpu interval`: automatic per-frame timers (GPU frame duration, GPU and CPU
  frame-to-frame time).
- One row per `gpu.timer` / `pxb.cpu_timer`.
- Columns: n, total, mean, sd, median, p99, p95, min, max in ms, with Hz in brackets (`+-` Hz for sd).
- `run: vulkan V, N frames in T s = F fps, display H Hz (refresh X ms)`: the `--vulkan` path, measured frames (after
  warm-up), their wall time, the resulting fps and the display refresh rate reported by the device (`display -` when
  unknown). Under FIFO, frames lost to vsync can be estimated as `T * H - N`.
- `offscreen presents` (with `--offscreen`), and the thermal headroom at the start and end of the measured frames
  (Android: 1.0 = severe throttling; 0 where unsupported).

Tracy also shows thermal plots (headroom, status, battery temperature) sampled once per second.
