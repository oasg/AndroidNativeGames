#!/bin/sh
# Build the first Android/arm64 host probe.
#
#   WORK=/path/to/TOTK_build/totk ./build.sh
#
# The work folder must contain extracted/v0/exefs and rewritten/android-v0 from
# prepare_android_cpu.py.  Set ANDROID_NDK_HOME if the NDK is not at the
# Homebrew default.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${WORK:-$here/../../../gamebin/TOTK_build/totk}
python=${PYTHON:-python3}
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
host=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
cc="$ndk/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android26-clang"
if [ ! -x "$cc" ]; then
  echo "Android clang not found at $cc (set ANDROID_NDK_HOME)" >&2
  exit 1
fi
"$python" "$here/../scripts/prepare_android_runtime.py" --work "$work"
out="$work/build/android-v0"
"$python" "$here/tools/prepare_triangle_spv.py" --ndk "$ndk" --out "$out/triangle_spv.h"
"$cc" -std=c11 -O2 -Wall -Wextra \
  -fno-stack-protector -ffixed-x18 -fno-sanitize=shadow-call-stack -fno-emulated-tls \
  -I"$here/../runtime" -I"$out" \
  -I"$here/../third_party/d2r_gfx/gfx/nvn" -I"$here/../third_party/d2r_gfx/gfx/tex" \
  "$here/loader_android.c" "$here/hle_android.c" "$here/hle_stack_android.c" \
  "$here/services_android.c" "$here/fs_service_android.c" "$here/vi_android.c" \
  "$here/account_android.c" "$here/nvn_android.c" "$here/nvdrv_android.c" \
  "$here/native_window_android.c" "$here/gfx_vulkan_android.c" "$here/hid_android.c" \
  "$here/fs_host_android.c" "$here/romfs_android.c" "$here/shader_cache_android.c" \
  "$here/../runtime/audio_renderer.c" "$here/../runtime/audio_reverb.c" \
  "$here/audio_mixer_android.c" "$here/../runtime/time_host.c" \
  "$here/../runtime/threads.c" "$here/../runtime/ipc.c" \
  "$here/../third_party/d2r_gfx/gfx/nvn/nvn_tex.c" \
  "$here/../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c" \
  "$here/nvn_texture_guest_android.c" \
  "$here/sha256.c" "$here/gate_android.S" "$here/hle_stack_android.S" \
  "$here/thread_enter_android.S" -D__ANDROID__ \
  -pthread -lc++abi -lunwind -lvulkan -ldl -llog -lm -o "$out/totk-android"
echo "built $out/totk-android"
