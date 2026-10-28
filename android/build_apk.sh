#!/usr/bin/env bash
# Configure build-android if needed (Release, arm64-v8a, android-31), build it, and package libpxbenchy.so into a
# signed debug APK (no Gradle). Output: build-android/pxbenchy.apk (its path is the last stdout line).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-android"
STAGE="$BUILD/apk"
APK="$BUILD/pxbenchy.apk"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}}"
BT="$SDK/build-tools/34.0.0"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"
# ANDROID_NDK_HOME may point nowhere; only trust it when it exists.
if [[ -n "${ANDROID_NDK_HOME:-}" && -d "$ANDROID_NDK_HOME" ]]; then
  NDK="$ANDROID_NDK_HOME"
else
  NDK="$SDK/ndk/26.3.11579264"
fi
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
[[ -f "$VVL" ]] || die "$VVL not found. Run deps/fetch.sh"

# cmake output goes to stderr: callers read the APK path from stdout
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
  GEN=()
  command -v ninja >/dev/null && GEN=(-G Ninja)
  cmake -S "$ROOT" -B "$BUILD" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release >&2
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
cp "$VVL" "$STAGE/lib/arm64-v8a/"

"$BT/aapt2" link -o "$STAGE/base.apk" --manifest "$ROOT/android/AndroidManifest.xml" \
  -I "$ANDROID_JAR" -A "$STAGE/assets"
(cd "$STAGE" && zip -q -X "base.apk" lib/arm64-v8a/*.so)

# zipalign before apksigner (v2+ signing). -p page-aligns stored .so files.
"$BT/zipalign" -f -p 4 "$STAGE/base.apk" "$STAGE/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias androiddebugkey --out "$APK" "$STAGE/aligned.apk"

echo "$APK"
