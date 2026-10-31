/* Probe entry points shared by the loader and the feature lanes.
 *
 * Wave 0 freezes these four names so the loader's argv dispatch does not need
 * to be edited by the backend/NVN lanes.  probe_hooks_android.c provides weak
 * defaults that fail explicitly (exit 77); the lane that owns the feature
 * defines a strong version in its own translation unit.
 */
#ifndef TOTK_ANDROID_PROBE_HOOKS_H
#define TOTK_ANDROID_PROBE_HOOKS_H

/* games/totk/android/gfx_vulkan_android.c (Lane 1). */
int gfx_backend_compute_probe(const char *work_dir);
int gfx_backend_copy_probe(const char *work_dir);

/* games/totk/android/nvn_android.c (Lane 2). */
int nvn_compute_probe(const char *work_dir);
int nvn_copy_probe(const char *work_dir);

#endif
