# Running benchmarks

## Command line

```
pxbenchy [--validation] [--vulkan auto|1.1|1.3] [--frames N] [--frames-warmup N] [--no-hud] [--offscreen] [--json] [--screenshot file.png] [--screenshot-frame N] [--vulkan-info] [<script>]
```

Without a script the app opens the menu (below); `--vulkan`, `--validation`,
`--frames`, `--frames-warmup` and `--no-hud` set its initial state (offscreen always starts on), `--json` and
`--screenshot` are errors there.

| option | default | meaning |
|---|---|---|
| `--validation` | off | enable the Vulkan validation layer (with synchronization validation); use while developing a script, leave off for numbers. Not in Release builds |
| `--vulkan auto\|1.1\|1.3` | auto | Vulkan path: 1.1 core (render passes, sync1) or 1.3 (dynamic rendering, synchronization2). `auto` probes the Vulkan driver at startup (temporary instance, no validation) and picks 1.3 when a device has 1.3 + dynamicRendering + synchronization2, else 1.1. An explicit 1.3 on a device without them fails: no fallback, exits 1. The resolved path is shown as `vulkan=` on the startup line (`vulkan=1.3 (auto)` when probed), `vkX.Y` on the HUD, in the `run:` line and as `"vulkan"` in the JSON report |
| `--frames N` | 6000 | measured frames (`pxb.args.frames`); 0 = run until the window closes |
| `--frames-warmup N` | 60 | frames before timers are cleared (`pxb.args.frames_warmup`) |
| `--no-hud` | HUD on | no on-screen stats |
| `--offscreen` | off | render to an own image, no vsync; about once per second the last frame is copied to the screen |
| `--json` | off | print the report as one JSON line |
| `--screenshot file.png` | off | save frame `--screenshot-frame` as an RGBA8 PNG (scene only, no HUD, view orientation), print `screenshot: path (frame N, WxH)` and exit 0 without a report; error (exit 1) if the script ends first. Android: use an app-writable absolute path |
| `--screenshot-frame N` | 1 | 1-based frame (HUD frame number) to capture; a skipped frame moves it to the next one drawn |
| `--vulkan-info` | off | print the Vulkan info (see [Vulkan info](#vulkan-info)) and exit 0; no window, other options ignored |

## Menu

Shown on a launcher start on Android and when `pxbenchy` runs without a script. Sized for touch, scaled by
the display density.

- Vulkan 1.1 / 1.3 (initial: as `--vulkan auto`; 1.3 greyed out when the probe finds no 1.3 device) and validation
  on / off (not in Release builds): each change recreates the device (prints a new `device:` line).
- Offscreen on / off (starts on); frames and warm-up with -/+ buttons over fixed steps (frames 60 .. 12000, warm-up 0 .. 300).
- Scripts: every `scripts/*.lua` with its own Run button; "Run all demos" runs every `scripts/demo*.lua`
  in name order with the same options, back to back; Back during a script stops the remaining ones.
- Results: one row per run (script, vulkan, `val` / `off` options, fps, median gpu busy and cpu frame interval in ms),
  kept for the app session in run order ("Run all demos" adds one row per script). Tap a row for the median of each
  timer. Clear empties the table. "Save results" saves every run in the table. The status line shows the path (macOS)
  or `content://` URI (Android).
- Share, Share as text, Copy: send the table's results JSON (disabled while the table is empty). Copy and Share as
  text write no file; Share saves the table to a new file and shares it. Android: Share opens the share sheet with the JSON file
  (`application/json`, subject `pxBenchy results <model>`; mail, Slack, Drive, ...); Share as text opens it with the
  JSON as plain text (files over 500 KB are shared as a file instead); Copy puts the JSON on the clipboard (over
  500 KB: refused, use Share). macOS: Share reveals the file in Finder; Share as text and Copy copy the JSON to the
  pasteboard. The status line shows the outcome (`Copied 3.2 KB`, `share sheet opened`, or the error).
- Update check (Android): at start, the latest GitHub release (`api.github.com/repos/pplux/pxBenchy/releases/latest`)
  `tag_name` (`vX.Y.Z` or `X.Y.Z`) is compared with the build's version; a newer one shows `pxBenchy <tag> available`
  with a Download button that opens the latest release page in the browser. The outcome is printed (`update: ...`); a
  failed check (offline, no releases) shows nothing.
- During a run only the HUD is drawn. Back (Android) or Escape (macOS) stops the run and returns to the menu; in the
  menu it exits the app. The run's report is also printed (stdout / logcat), as on the command line.

Back or Escape during a command-line run stops it the same way (report printed, exit 1).

## Desktop (macOS)

```
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug && cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-debug && ./build-debug/pxbenchy --frames 1000 scripts/demo1_instancing.lua
```

`build-release` is the build used for numbers. Release builds (`Release`, `MinSizeRel`) have no `--validation` and
Tracy off by default; other build types have validation and Tracy on. `-DPXB_TRACY=ON|OFF` overrides Tracy in any
build type, e.g. `cmake -DPXB_TRACY=ON -B build-release` to profile a release build.

## Android

```
android/build_apk.sh && adb install -r build-android-debug/pxbenchy.apk
adb shell am start -S -n com.pplux.pxbenchy/android.app.NativeActivity --es args "'--frames 1000 scripts/triangle.lua'"
adb logcat -s pxbenchy
```

Without `--es args` (or from the launcher) the app opens the menu. Or in one step: `android/install_and_run.sh
[--release | --release-tracy] [app args...]` builds, installs, launches, follows the log until the app's
last line (`exit code: N`) and exits with that code.

`android/build_apk.sh [--release | --release-tracy]` configures the build dir when missing (arm64-v8a, android-31), builds it,
and packs the `.so`, scripts and assets into a signed APK (see Signing). Three variants, same package id
(installing one replaces the other):

- debug (default): `build-android-debug/pxbenchy.apk`, RelWithDebInfo (validation, Tracy), bundled validation
  layer, debuggable
- `--release`: `build-android-release/pxbenchy-<version>.apk` (the name to attach to the GitHub release), Release (no `--validation`, no validation toggle in the menu, no
  Tracy), no validation layer, not debuggable. The one to publish
- `--release-tracy`: `build-android-release-tracy/pxbenchy.apk`, as `--release` plus Tracy

The APK's versionName / versionCode come from `project(pxbenchy VERSION x.y.z)` in `CMakeLists.txt` (versionCode
= major*10000 + minor*100 + patch): bump it for each published release. `build_apk.sh` prints `version: x.y.z (versionCode N), release tag vx.y.z` before the APK path. The startup line shows the build's git hash (`git=...`, also on
the HUD) to confirm what is installed. Tracy needs `adb forward tcp:8086 tcp:8086`. INTERNET is always requested
(update check, Tracy's socket).

Signing: Android installs an update only over an APK signed with the same key. Unset, `build_apk.sh` uses
`~/.android/debug.keystore` (created when missing; differs per machine). For published APKs use one permanent
keystore, kept outside the repo:

```
keytool -genkeypair -keystore pxbenchy-release.p12 -storetype PKCS12 -alias pxbenchy -keyalg RSA -keysize 4096 \
  -validity 10000 -dname "CN=pxBenchy"
PXB_KEYSTORE=/path/pxbenchy-release.p12 PXB_KEYSTORE_PASS=... android/build_apk.sh --release
```

`PXB_KEYSTORE` (path) enables it and needs `PXB_KEYSTORE_PASS` (keystore password, also the key's: PKCS12 uses one);
`PXB_KEY_ALIAS` only when the keystore holds several keys. `build_apk.sh` prints `signing with: <keystore>`.
Losing the keystore means users must uninstall to move to APKs signed with a new one.

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

### JSON report

`--json` prints the same data as one JSON line: run settings (`script`, `vulkan`,
`validation`, `present_mode`, `offscreen`, `hud`, `extent`, `frames`, `frames_warmup`, `refresh_ms`), the measured
`measured_frames`, `measured_time_s`, `fps`, `offscreen_presents`, `thermal_headroom_start` / `_end`, and `timers`.
`device` is the Vulkan device name (kept for compatibility); `device_info` describes the device, driver and build:

| Field | Source |
|---|---|
| `name`, `type` | `deviceName`, `deviceType` (`integrated`, `discrete`, `virtual`, `cpu`, `other`) |
| `vendor_id`, `device_id` | `vendorID`, `deviceID` (decimal) |
| `api_version` | device `apiVersion` as `major.minor.patch` (MoltenVK reports at most the instance version: 1.1.x on `--vulkan 1.1`) |
| `driver_version` | raw `driverVersion` |
| `driver_version_decoded` | MoltenVK: `v / 10000 . v / 100 % 100 . v % 100`; NVIDIA: 10.8.8.6 bits; others: `VK_MAKE_VERSION` layout (10.10.12 bits; Qualcomm sets bit 31, so its major reads 512+). Vendor encodings vary: treat it as a hint, keep the raw value |
| `driver_id`, `driver_name`, `driver_info`, `driver_conformance` | `VkPhysicalDeviceDriverProperties` (core 1.2; on `--vulkan 1.1` only when the device exposes `VK_KHR_driver_properties`); left out when unavailable |
| `os` | `android` or `macos` |
| `manufacturer`, `model`, `soc_manufacturer`, `soc_model`, `build_fingerprint`, `sdk`, `os_version` | Android system properties `ro.product.manufacturer`, `ro.product.model`, `ro.soc.manufacturer`, `ro.soc.model`, `ro.build.fingerprint`, `ro.build.version.sdk`, `ro.build.version.release` (strings; empty ones left out) |
| `model`, `os_version` | macOS `sysctl hw.model`, `NSProcessInfo.operatingSystemVersionString` |
| `git` | build's git hash (as on the startup line) |
| `version` | app version, `project(VERSION)` in `CMakeLists.txt` |
| `build_type` | `CMAKE_BUILD_TYPE` (`Debug`, `Release`, `RelWithDebInfo`; empty when unset) |
| `tracy` | Tracy compiled in (`PXB_TRACY`) |

The menu header shows a one-line summary (device, driver and version, model, OS).

### Result files

The menu's "Save results" and Share write one JSON file, every run of the table in run order:

```
{"pxbenchy":1,"device":{...},"vulkan":{...},"runs":[
{...},
{...}
]}
```

`device` is the `device_info` object of the device at save time (`null` when no device is up); `runs` holds the JSON
report of each run, as printed by `--json`, each with its own `device_info` (a table with 1.1 and 1.3 runs keeps the
right `api_version` per run). `vulkan` is the Vulkan info below, queried once at startup. Runs without a report (the
run could not start) are left out. Single-script runs (command line, adb) write no file.

### Vulkan info

Queried at startup with temporary instances (no window, no validation), printed by `--vulkan-info` and saved as
`vulkan` in result files. Multi-line JSON, `{"vk11": {...}, "vk13": {...}}`: what an instance of each version sees
(`null` when the loader has no instance of that version):

- `instance_version`, `instance_extensions` (`{name: specVersion}`), `layers`
- `devices`: every physical device of at least that version, each with the `VkPhysicalDeviceProperties` members
  (`apiVersion` as `major.minor.patch`, UUIDs as hex), `limits`, `sparseProperties`, `features` (1.0 core),
  `memoryHeaps`, `memoryTypes`, `queueFamilies`, `extensions` (`{name: specVersion}`) and `formats`: every core
  format (VkFormat 1..184) with any support, keyed by its VkFormat value, as `[linearTilingFeatures,
  optimalTilingFeatures, bufferFeatures]`
- then the mode's own structs, each under its Vulkan struct name. `vk11`: the separate 1.1 structs
  (`VkPhysicalDevice16BitStorageFeatures`, `...MultiviewFeatures`, `...VariablePointersFeatures`,
  `...ProtectedMemoryFeatures`, `...SamplerYcbcrConversionFeatures`, `...ShaderDrawParametersFeatures`,
  `...IDProperties`, `...Maintenance3Properties`, `...MultiviewProperties`, `...PointClippingProperties`,
  `...ProtectedMemoryProperties`, `...SubgroupProperties`, and `...DriverProperties` with
  `VK_KHR_driver_properties`). `vk13`: `VkPhysicalDeviceVulkan11/12/13Features` and `...Properties`

Flags and enums are their raw numbers.

- Name: `<model>-<yyyymmdd-hhmmss>.json`, local time; `model` from `device_info` (characters other than letters,
  digits, `.`, `-`, `_` become `_`; `device` when unknown).
- macOS: `results/` under the working directory (created when missing, gitignored); an existing name gets `-1`, `-2`,
  ... before `.json`.
- Android: MediaStore Downloads, `Download/pxBenchy/` (Files app: Downloads > pxBenchy), `application/json`, no storage
  permission. The printed `content://media/...` URI is what sharing uses; MediaStore renames on a name clash.
