#!/bin/sh
# Build the synthetic fixture and the arm64 Android probe.
# Requires the Android NDK (set ANDROID_NDK_HOME or ANDROID_NDK_ROOT).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -z "$ndk" ]; then
  for d in "$HOME/Library/Android/sdk/ndk"/*; do
    [ -d "$d" ] && ndk=$d
  done
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
  echo "Android NDK not found; set ANDROID_NDK_HOME" >&2
  exit 1
fi
host=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
cc="$ndk/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android26-clang"
if [ ! -x "$cc" ]; then
  echo "clang not found at $cc" >&2
  exit 1
fi
python=${PYTHON:-python3}
"$python" "$here/make_fixture.py" --out "$here/build" --python "$python"
"$cc" -O2 -static -fno-stack-protector -fno-sanitize=shadow-call-stack -ffixed-x18 \
  -I"$here/build" "$here/fixture_probe.c" "$here/fixture_gate.S" \
  -o "$here/build/fixture_probe"
echo "built $here/build/fixture_probe"
