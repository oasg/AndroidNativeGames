#!/bin/sh
# Build, push and run one Android probe with a per-lane work dir and a shared
# device lock.  Wave 0 coordination helper; it owns no feature code.
#
#   games/totk/android/devrun.sh probe-vulkan-mrt
#   TOTK_LANE=lane1 games/totk/android/devrun.sh probe-vulkan-compute
#   TOTK_LANE=lane2 games/totk/android/devrun.sh --no-build probe-nvn-copy
#   TOTK_LANE=lane4 games/totk/android/devrun.sh probe-nvn-state A=1
#
# The lane work dir defaults to /tmp/totk-<lane>; rewritten/analysis/extracted
# are read-only symlinks into gamebin, so parallel lanes never overwrite each
# other's binary or generated runtime_tables.h.  Device access is serialized
# through /tmp/totk-device.lock.d; the lock is released on exit or signal.
set -eu

usage() {
    echo "usage: [TOTK_LANE=name] $0 [--no-build] <probe> [K=V ...]" >&2
    exit 64
}

build=1
if [ "${1:-}" = "--no-build" ]; then build=0; shift; fi
[ $# -ge 1 ] || usage
probe=$1
shift

lane=${TOTK_LANE:-wave0}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
# A lane worktree has no gamebin/; resolve the main checkout through the
# common git dir so every lane can share the read-only game inputs.
common=$(git -C "$root" rev-parse --git-common-dir)
case $common in
    /*) gitdir=$common ;;
    *)  gitdir=$root/$common ;;
esac
main=$(CDPATH= cd -- "$gitdir/.." && pwd)
src=${TOTK_SRC:-$main/gamebin/TOTK_build/totk}
work=${WORK:-/tmp/totk-$lane}
dev=${TOTK_DEV_DIR:-/data/local/tmp/totk-$lane}
shared_dev=${TOTK_SHARED_DEV_DIR:-/data/local/tmp/totk-android}
lockdir=${TOTK_DEVICE_LOCK:-/tmp/totk-device.lock.d}

[ -d "$src/rewritten/android-v0" ] || {
    echo "missing $src/rewritten/android-v0 (set TOTK_SRC)" >&2
    exit 1
}
mkdir -p "$work"
for d in rewritten analysis extracted; do
    [ -e "$work/$d" ] || ln -s "$src/$d" "$work/$d"
done

if [ "$build" = 1 ]; then
    ( cd "$root" && WORK="$work" games/totk/android/build.sh )
fi
bin=$work/build/android-v0/totk-android
[ -x "$bin" ] || { echo "missing $bin (run without --no-build)" >&2; exit 1; }

acquire_lock() {
    i=0
    while ! mkdir "$lockdir" 2>/dev/null; do
        if [ -f "$lockdir/pid" ]; then
            p=$(cat "$lockdir/pid" 2>/dev/null || true)
            if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
                rm -rf "$lockdir"
                continue
            fi
        fi
        i=$((i + 1))
        if [ "$i" -gt "${TOTK_LOCK_TIMEOUT:-600}" ]; then
            echo "device lock timeout: $lockdir" >&2
            exit 75
        fi
        sleep 1
    done
    echo $$ > "$lockdir/pid"
    trap 'rm -rf "$lockdir"' EXIT INT TERM HUP
}

acquire_lock
adb shell "mkdir -p '$dev'"
adb push "$bin" "$dev/totk-android" >/dev/null
for m in main sdk subsdk0; do
    f=$m.patched.bin
    if adb shell "[ -f '$shared_dev/$f' ]"; then
        adb shell "ln -sf '$shared_dev/$f' '$dev/$f'"
    else
        adb push "$src/rewritten/android-v0/$f" "$dev/$f" >/dev/null
    fi
done
adb shell "chmod 755 '$dev/totk-android'"

envs=
for kv in "$@"; do
    envs="$envs $kv"
done

set +e
adb shell "cd '$dev' && env$envs ./totk-android '$dev' '$probe'"
rc=$?
set -e
rm -rf "$lockdir"
trap - EXIT INT TERM HUP
exit "$rc"
