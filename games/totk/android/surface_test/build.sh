#!/bin/sh
# Build a minimal Surface clear test APK (no game data).
# usage: AAPT2=... D8=... APKSIGNER=... ZIPALIGN=... ./build.sh [output.apk]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../../../.." && pwd)
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
sdk=${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}
bt=${ANDROID_BUILD_TOOLS:-$sdk/build-tools/35.0.0}
jar=${ANDROID_PLATFORM_JAR:-$sdk/platforms/android-35/android.jar}
host=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
cc="$ndk/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android26-clang"
out=${1:-$here/build/surface-test.apk}
work=$here/build
rm -rf "$work"; mkdir -p "$work/classes" "$work/dex" "$work/lib/arm64-v8a"
python3 "$repo/games/totk/android/tools/prepare_triangle_spv.py" --ndk "$ndk" --out "$work/triangle_spv.h" >/dev/null
"$cc" -shared -fPIC -O2 -I"$repo/games/totk/android" -I"$work" \
  -o "$work/lib/arm64-v8a/libtotk_surface.so" \
  "$here/surface_test.c" "$repo/games/totk/android/gfx_vulkan_android.c" \
  -lvulkan -landroid -llog -pthread
javac -source 8 -target 8 -cp "$jar" -d "$work/classes" "$here/java/com/totk/surfacetest/MainActivity.java"
"$bt/d8" --lib "$jar" --output "$work/dex" $(find "$work/classes" -name '*.class')
"$bt/aapt2" link -o "$work/unsigned.apk" -I "$jar" --manifest "$here/AndroidManifest.xml"
(cd "$work/dex" && zip -q "$work/unsigned.apk" classes.dex)
(cd "$work" && zip -q unsigned.apk lib/arm64-v8a/libtotk_surface.so)
"$bt/zipalign" -f 4 "$work/unsigned.apk" "$work/aligned.apk"
ks=${ANDROID_DEBUG_KEYSTORE:-$HOME/.android/debug.keystore}
if [ ! -f "$ks" ]; then
  mkdir -p "$(dirname "$ks")"
  keytool -genkeypair -keystore "$ks" -storepass android -keypass android -alias androiddebugkey \
    -dname 'CN=Android Debug,O=Android,C=US' -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
"$bt/apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android --out "$out" "$work/aligned.apk"
echo "built $out"
