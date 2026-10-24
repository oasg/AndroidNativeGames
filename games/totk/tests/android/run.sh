#!/bin/sh
# Push and run the probe on a connected arm64 Android device/emulator.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
adb=${ADB:-adb}
"$adb" push "$here/build/fixture_probe" /data/local/tmp/fixture_probe >/dev/null
"$adb" push "$here/build/rewritten/main.patched.bin" /data/local/tmp/main.patched.bin >/dev/null
"$adb" shell chmod 755 /data/local/tmp/fixture_probe
"$adb" shell /data/local/tmp/fixture_probe /data/local/tmp/main.patched.bin
