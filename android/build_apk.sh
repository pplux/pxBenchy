#!/usr/bin/env bash
# Configure the build dir if needed (arm64-v8a, android-31), build it, and package libpxbenchy.so into an APK signed
# with the debug keystore (no Gradle). Output: <build dir>/pxbenchy.apk (its path is the last stdout line).
# Usage: android/build_apk.sh [--release | --release-tracy]
#   debug (default):  build-android-debug, RelWithDebInfo (validation, Tracy), bundled validation layer, debuggable
#   --release:        build-android-release, Release (no validation, no Tracy), no validation layer, not debuggable
#   --release-tracy:  build-android-release-tracy, as --release plus Tracy
# INTERNET (Tracy's socket) only when the build dir has PXB_TRACY on.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE=1
CONFIG=()
if [[ $# == 0 ]]; then
  RELEASE=0
  BUILD="$ROOT/build-android-debug"
  CONFIG=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
elif [[ $# == 1 && "$1" == "--release" ]]; then
  BUILD="$ROOT/build-android-release"
  CONFIG=(-DCMAKE_BUILD_TYPE=Release)
elif [[ $# == 1 && "$1" == "--release-tracy" ]]; then
  BUILD="$ROOT/build-android-release-tracy"
  CONFIG=(-DCMAKE_BUILD_TYPE=Release -DPXB_TRACY=ON)
else
  echo "usage: $0 [--release | --release-tracy]" >&2
  exit 1
fi
STAGE="$BUILD/apk"
APK="$BUILD/pxbenchy.apk"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}}"

# pick DIR FILE [PREFERRED]: subdirectory of DIR that contains FILE (skips partial installs). PREFERRED when set and
# installed, else the highest version; empty if none.
pick() {
  if [[ -n "$3" && -e "$1/$3/$2" ]]; then echo "$3"; return; fi
  local d
  for d in "$1"/*/; do
    [[ -e "$d/$2" ]] && basename "$d"
  done | sort -V | tail -n1
}

# Known-good versions; override with these variables, or install them to stop falling back to the latest.
BT="$SDK/build-tools/$(pick "$SDK/build-tools" apksigner "${PXB_BUILD_TOOLS:-34.0.0}")"
ANDROID_JAR="$SDK/platforms/$(pick "$SDK/platforms" android.jar "${PXB_PLATFORM:-android-34}")/android.jar"
# NDK: PXB_NDK (a version under $SDK/ndk) when set, else the latest under $SDK/ndk. Without any there, fall back to
# ANDROID_NDK_HOME/ANDROID_NDK_ROOT (only when they exist), then the legacy ndk-bundle.
NDK="$(pick "$SDK/ndk" build/cmake/android.toolchain.cmake "${PXB_NDK:-}")"
if [[ -n "$NDK" ]]; then
  NDK="$SDK/ndk/$NDK"
else
  NDK="$SDK/ndk-bundle"
  for cand in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}"; do
    if [[ -n "$cand" && -d "$cand" ]]; then NDK="$cand"; break; fi
  done
fi
echo "using NDK: $NDK" >&2
echo "using build-tools: $BT" >&2
echo "using android.jar: $ANDROID_JAR" >&2
STRIP="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -n1 || true)"

KEYSTORE="$HOME/.android/debug.keystore"
SO="$BUILD/libpxbenchy.so"
VVL="$ROOT/deps/vvl-android/arm64-v8a/libVkLayer_khronos_validation.so"

die() { echo "error: $*" >&2; exit 1; }

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"

for f in "$BT/aapt2" "$BT/zipalign" "$BT/apksigner" "$ANDROID_JAR" "$STRIP" "$TOOLCHAIN"; do
  [[ -n "$f" && -e "$f" ]] || die "missing tool or file: ${f:-llvm-strip under $NDK} (SDK=$SDK, NDK=$NDK)"
done
command -v cmake >/dev/null || die "missing tool: cmake"
command -v zip >/dev/null || die "missing tool: zip"
command -v keytool >/dev/null || die "missing tool: keytool"
if [[ $RELEASE == 0 ]]; then
  [[ -f "$VVL" ]] || die "$VVL not found. Run deps/fetch.sh"
fi

# cmake output goes to stderr: callers read the APK path from stdout
# A cache configured with another NDK can't be reused (the toolchain is fixed at configure time): reconfigure from scratch.
if [[ -f "$BUILD/CMakeCache.txt" ]] &&
  ! grep -qxF "CMAKE_TOOLCHAIN_FILE:FILEPATH=$TOOLCHAIN" "$BUILD/CMakeCache.txt"; then
  echo "NDK changed, reconfiguring $BUILD" >&2
  rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
fi
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  GEN=()
  command -v ninja >/dev/null && GEN=(-G Ninja)
  cmake -S "$ROOT" -B "$BUILD" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-31 "${CONFIG[@]}" >&2
fi
cmake --build "$BUILD" >&2
[[ -f "$SO" ]] || die "$SO not found after the build"

if [[ ! -f "$KEYSTORE" ]]; then
  mkdir -p "$(dirname "$KEYSTORE")"
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US"
fi

rm -rf "$STAGE" "$APK"
mkdir -p "$STAGE/assets" "$STAGE/lib/arm64-v8a"
cp -R "$ROOT/assets" "$STAGE/assets/assets"
cp -R "$ROOT/scripts" "$STAGE/assets/scripts"
cp "$SO" "$STAGE/lib/arm64-v8a/libpxbenchy.so"
"$STRIP" --strip-unneeded "$STAGE/lib/arm64-v8a/libpxbenchy.so"
# debug: validation layer bundled, debuggable so the loader finds it. No Tracy: no INTERNET (only Tracy uses it)
MANIFEST="$ROOT/android/AndroidManifest.xml"
LINK=()
if [[ $RELEASE == 0 ]]; then
  cp "$VVL" "$STAGE/lib/arm64-v8a/"
  LINK=(--debug-mode)
fi
if ! grep -qx 'PXB_TRACY:BOOL=ON' "$BUILD/CMakeCache.txt"; then
  grep -v 'android.permission.INTERNET' "$MANIFEST" >"$STAGE/AndroidManifest.xml"
  MANIFEST="$STAGE/AndroidManifest.xml"
fi

# versionCode / versionName from project(VERSION) in CMakeLists.txt
read -r VERSION_CODE VERSION_NAME <"$BUILD/android_version.txt"

"$BT/aapt2" link -o "$STAGE/base.apk" --manifest "$MANIFEST" \
  -I "$ANDROID_JAR" -A "$STAGE/assets" --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
  ${LINK[@]+"${LINK[@]}"}
(cd "$STAGE" && zip -q -X "base.apk" lib/arm64-v8a/*.so)

# zipalign before apksigner (v2+ signing). -p page-aligns stored .so files.
"$BT/zipalign" -f -p 4 "$STAGE/base.apk" "$STAGE/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias androiddebugkey --out "$APK" "$STAGE/aligned.apk"

echo "$APK"
