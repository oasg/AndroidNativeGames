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
"$cc" -std=c11 -O2 -static -Wall -Wextra \
  -fno-stack-protector -ffixed-x18 -fno-sanitize=shadow-call-stack -fno-emulated-tls \
  -I"$here/../runtime" -I"$out" \
  "$here/loader_android.c" "$here/hle_android.c" "$here/hle_stack_android.c" \
  "$here/services_android.c" "$here/fs_service_android.c" \
  "$here/../runtime/threads.c" "$here/../runtime/ipc.c" \
  "$here/sha256.c" "$here/gate_android.S" "$here/hle_stack_android.S" \
  "$here/thread_enter_android.S" -D__ANDROID__ \
  -pthread -o "$out/totk-android"
echo "built $out/totk-android"
