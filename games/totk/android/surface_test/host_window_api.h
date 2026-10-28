/* Host NativeWindow API used by the Android app shell.
 *
 * This is the narrow surface handoff between a Java SurfaceView (or an
 * android_app window) and the host window that NVN/VI present through.  It is
 * declared separately from native_window_android.c so the shell APKs and a
 * future loader build can share the contract without pulling runtime.h in.
 */
#ifndef TOTK_ANDROID_HOST_WINDOW_API_H
#define TOTK_ANDROID_HOST_WINDOW_API_H

#include <stddef.h>
#include <stdint.h>

/* Create/destroy the host description.  `format` is an NVN format id
 * (0x25 = RGBA8); `layer_id` is only diagnostic. */
void *runtime_native_window_create(uint32_t width, uint32_t height, uint32_t format,
                                   uint64_t layer_id);
void runtime_native_window_destroy(void *host_window);

/* Attach the app shell's ANativeWindow* to the host window and create the
 * VkSwapchainKHR.  host_window may be NULL, in which case a shell-owned host
 * window is created (see runtime_native_window_shell_window).  Passing
 * native_window == NULL detaches and keeps the offscreen target. */
void runtime_native_window_attach_android(void *native_window, void *host_window,
                                          uint32_t width, uint32_t height,
                                          uint32_t nvn_format);

/* Host window implicitly created by runtime_native_window_attach_android when
 * host_window was NULL. */
void *runtime_native_window_shell_window(void);

/* The ANativeWindow* currently attached, or NULL. */
void *runtime_native_window_android(const void *host_window);

int runtime_native_window_surface_ready(const void *host_window);
unsigned runtime_native_window_present_count(const void *host_window);
unsigned runtime_native_window_surface_present_count(const void *host_window);

/* Clear + present through the same path the NVN window uses: swapchain when a
 * surface is attached, Vulkan offscreen target otherwise. */
int runtime_native_window_clear_rgba(void *host_window, const float rgba[4]);
int runtime_native_window_present(void *host_window);

/* Optional loader entry implemented by a loader-enabled build.  The worker
 * thread started by MainActivity.nativeRunLoader calls it when linked. */
int totk_android_loader_entry(void *host_window, int argc, char **argv);

#endif
