#!/usr/bin/env bash
# Build the APK, install it, launch it and follow logcat until the app exits (exit code = the app's; Ctrl+C stops).
# Usage: android/install_and_run.sh [app args...]
#   e.g. android/install_and_run.sh --validation scripts/demo1_instancing.lua
# App args are passed as the intent extra "args". Device: ANDROID_SERIAL if several.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="com.pplux.pxbenchy"

die() { echo "error: $*" >&2; exit 1; }

if [[ -z "${ANDROID_SERIAL:-}" ]]; then
  devices=($(adb devices | awk 'NR > 1 && $2 == "device" { print $1 }'))
  if [[ ${#devices[@]} -gt 1 ]]; then
    export ANDROID_SERIAL="${devices[0]}"
    echo "warning: ${#devices[@]} devices connected (${devices[*]}), using the first one." >&2
    echo "To pick one, set it, e.g.: export ANDROID_SERIAL=${devices[0]}" >&2
  fi
fi

APK="$("$ROOT/android/build_apk.sh" | tail -n1)"

if ! out="$(adb install -r "$APK" 2>&1)"; then
  echo "$out" >&2
  if grep -q INSTALL_FAILED_UPDATE_INCOMPATIBLE <<<"$out"; then
    die "installed app is signed with a different key. Run: adb uninstall $PKG"
  fi
  die "adb install failed"
fi

adb logcat -c
if [[ $# -gt 0 ]]; then
  adb shell am start -S -n "$PKG/android.app.NativeActivity" --es args "'$*'"
else
  adb shell am start -S -n "$PKG/android.app.NativeActivity"
fi

# follow the log until the app prints its last line ("exit code: N"), then exit with that code. The process stays
# cached after the activity finishes, so its pid is no use. A fifo instead of a pipe so logcat can be killed (bash 3.2).
FIFO="$(mktemp -u)"
mkfifo "$FIFO"
adb logcat -s pxbenchy >"$FIFO" &
LOGCAT=$!
disown "$LOGCAT" # no "Terminated" job message when it is killed
trap 'kill $LOGCAT 2>/dev/null; rm -f "$FIFO"' EXIT
code=1
while IFS= read -r line; do
  echo "$line"
  if [[ "$line" == *"pxbenchy: exit code: "* ]]; then
    code="${line##*exit code: }"
    break
  fi
done <"$FIFO"
exit "$code"
