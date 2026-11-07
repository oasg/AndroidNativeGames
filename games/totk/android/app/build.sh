#!/bin/sh
# Build the minimal TOTK Android APK shell.
#
#   WORK=/path/to/TOTK_build/totk ./build.sh [output.apk]
#
# The work folder must contain rewritten/android-v0 and analysis/android-v0-port
# from prepare_android_cpu.py.  Game data is NOT packaged; the APK only contains
# the loader, HLE/GPU host code and a SurfaceView shell.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
android=$(CDPATH= cd -- "$here/.." && pwd)
repo=$(CDPATH= cd -- "$here/../../../.." && pwd)
work=${WORK:-$repo/gamebin/TOTK_build/totk}
python=${PYTHON:-python3}
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
sdk=${ANDROID_SDK_ROOT:-/opt/homebrew/share/android-commandlinetools}
bt=${ANDROID_BUILD_TOOLS:-$sdk/build-tools/35.0.0}
jar=${ANDROID_PLATFORM_JAR:-$sdk/platforms/android-35/android.jar}
host=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
cc="$ndk/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android26-clang"
out=${1:-$here/build/totk-android.apk}

if [ ! -x "$cc" ]; then
    echo "Android clang not found at $cc (set ANDROID_NDK_HOME)" >&2
    exit 1
fi

build=$here/build
rm -rf "$build"
mkdir -p "$build/classes" "$build/dex" "$build/lib/arm64-v8a"

# Pack the three guest images as native libraries.  An installed app cannot
# mmap(PROT_EXEC) the external-files copy (FUSE), and app-private data is
# noexec on current Android; the native library dir is the one exec-capable
# path the loader may map without root.  The loader falls back to
# lib<module>_patched.so when <module>.patched.bin is absent.
for m in main sdk subsdk0; do
    cp "$work/rewritten/android-v0/$m.patched.bin" \
       "$build/lib/arm64-v8a/lib${m}_patched.so"
done

# Same generated tables and built-in SPIR-V the CLI build uses.
"$python" "$android/../scripts/prepare_android_runtime.py" --work "$work"
gen="$work/build/android-v0"
"$python" "$android/tools/prepare_triangle_spv.py" --ndk "$ndk" --out "$gen/triangle_spv.h"

. "$android/native_sources.sh"
set --
while IFS= read -r src; do set -- "$@" "$src"; done <<EOF
$(totk_android_native_sources "$android")
EOF
set -- "$@" "$here/shell_android.c"

"$cc" -std=c11 -O2 -Wall -Wextra -shared -fPIC \
  -fno-stack-protector -ffixed-x18 -fno-sanitize=shadow-call-stack -fno-emulated-tls \
  -DTOTK_ANDROID_LIBRARY -D__ANDROID__ \
  -I"$android" -I"$android/../runtime" -I"$gen" \
  -I"$android/../third_party/d2r_gfx/gfx/nvn" -I"$android/../third_party/d2r_gfx/gfx/tex" \
  -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
  -Wl,-Bsymbolic -Wl,-soname,libtotk_android.so \
  -o "$build/lib/arm64-v8a/libtotk_android.so" "$@" \
  -pthread -lc++abi -lunwind -lvulkan -ldl -llog -landroid -lm

javac -source 8 -target 8 -cp "$jar" -d "$build/classes" \
  "$here/java/com/totk/android/MainActivity.java"
"$bt/d8" --lib "$jar" --output "$build/dex" \
  $(find "$build/classes" -name '*.class')

"$bt/aapt2" link -o "$build/unsigned.apk" -I "$jar" --manifest "$here/AndroidManifest.xml"
(cd "$build/dex" && zip -q "$build/unsigned.apk" classes.dex)
(cd "$build" && zip -q -r unsigned.apk lib)
"$bt/zipalign" -f 4 "$build/unsigned.apk" "$build/aligned.apk"

ks=${ANDROID_DEBUG_KEYSTORE:-$HOME/.android/debug.keystore}
if [ ! -f "$ks" ]; then
    mkdir -p "$(dirname "$ks")"
    keytool -genkeypair -keystore "$ks" -storepass android -keypass android \
      -alias androiddebugkey -dname 'CN=Android Debug,O=Android,C=US' \
      -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
fi
"$bt/apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android \
  --out "$out" "$build/aligned.apk"
echo "built $out"
