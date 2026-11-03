# pxBenchy

Vulkan GPU benchmark (Android, macOS). Benchmarks are Lua scripts in `scripts/`.

## Build (Android)

Needs the Android SDK + NDK (`ANDROID_HOME`).

```
deps/fetch.sh                 # once: third-party sources
android/build_apk.sh          # debug APK: build-android-debug/pxbenchy.apk
android/build_apk.sh --release
```

Install and open the menu:

```
adb install -r build-android-debug/pxbenchy.apk
adb shell am start -n com.pplux.pxbenchy/android.app.NativeActivity
```

## Automation

Run every script on Vulkan 1.1 and 1.3, save the results and exit, then pull them:

```
adb install -r build-android-release/pxbenchy.apk
adb shell am start -S -n com.pplux.pxbenchy/android.app.NativeActivity --es args "'--run-all --save'"
while adb shell pidof com.pplux.pxbenchy >/dev/null; do sleep 5; done
adb pull /sdcard/Documents/pxBenchy results
tools/report.py results -o report.html
```

Progress: `adb logcat -s pxbenchy`.

## More

- Options, menu, results: [docs/running.md](docs/running.md)
- Lua API: [docs/lua-api.md](docs/lua-api.md)
- Desktop: `cmake -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release`
