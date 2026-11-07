# Shared native source list for the Android host (CLI and APK shell).
#
# usage: . "$here/native_sources.sh"; totk_android_native_sources "$here"
#
# The function prints one path per line so callers can preserve paths with
# spaces by reading it line by line instead of relying on word splitting.
#
# Host code is grouped by feature under subdirectories:
#   loader/        module mapping, relocation, crash reports, reflection, CLI
#   nvn/           NVN object graph, command recording, scene submission
#   gfx/           Vulkan backend (device, targets, pipelines, compute)
#   nvdrv/         nvdrv/nvmap/nvhost service
#   services/      Android SVC layer and lm logging service
#   shader_cache/  NNVN shader identity and SPIR-V cache
totk_android_native_sources() {
    android=$1
    for dir in loader nvn gfx nvdrv services shader_cache; do
        for src in "$android/$dir"/*.c; do
            printf '%s\n' "$src"
        done
    done
    cat <<EOF
$android/probe_hooks_android.c
$android/hle_android.c
$android/hle_stack_android.c
$android/fs_service_android.c
$android/vi_android.c
$android/account_android.c
$android/native_window_android.c
$android/hid_android.c
$android/fs_host_android.c
$android/romfs_android.c
$android/../runtime/audio_renderer.c
$android/../runtime/audio_reverb.c
$android/audio_mixer_android.c
$android/../runtime/time_host.c
$android/../runtime/threads.c
$android/../runtime/ipc.c
$android/../third_party/d2r_gfx/gfx/nvn/nvn_tex.c
$android/../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c
$android/nvn_texture_guest_android.c
$android/sha256.c
$android/gate_android.S
$android/hle_stack_android.S
$android/thread_enter_android.S
EOF
}
