/* Android app-shell path: SurfaceView Surface -> ANativeWindow* -> host window
 * -> VkSwapchainKHR, with a continuously animated clear.
 *
 * The shell deliberately goes through the same runtime_native_window_* API the
 * game loader will use, so the swapchain/attach code is exercised before the
 * loader is linked in.  UI thread work is limited to acquiring the window and
 * attaching; presentation and the (future) loader run on worker threads.
 */
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gfx_backend_android.h"
#include "host_window_api.h"

#define TAG "totk-shell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* The loader is a separate build: when it is linked it provides this entry and
 * the worker below runs it on a non-UI thread.  Without it the shell still
 * proves the worker/attach/present path. */
extern int totk_android_loader_entry(void *host_window, int argc, char **argv)
    __attribute__((weak));

static ANativeWindow *g_window;
static void *g_host_window;
static GfxOffscreenTarget *g_present_target;

static pthread_t g_present_thread;
static pthread_t g_loader_thread;
static atomic_int g_present_run;
static atomic_int g_loader_run;
static atomic_int g_loader_started;
static atomic_uint g_presented;
static atomic_uint g_surface_presented;
static atomic_uint g_failed;

static void *present_loop(void *arg) {
    void *host = arg;
    GfxOffscreenTarget *target = g_present_target;
    GfxTrianglePipeline *triangle = gfx_backend_builtin_triangle_pipeline();
    unsigned frame = 0;
    unsigned draw_failures = 0;
    LOGI("present loop enter host=%p target=%p", host, (void *)target);
    while (atomic_load(&g_present_run)) {
        /* Draw a real scene into the renderer-owned offscreen target, then hand
         * that target to the swapchain.  The 120-frame ramp keeps the clear
         * colour animated so a capture differs between frames; the triangle
         * proves this is not the old clear-only present. */
        float t = (float)(frame % 120) / 119.0f;
        if (!target || !triangle ||
            !gfx_backend_triangle_draw(triangle, target,
                                       0.04f + 0.30f * t, 0.10f, 0.30f - 0.22f * t, 1.0f)) {
            if (draw_failures++ < 4) LOGE("triangle draw failed at frame %u", frame);
        }
        if (runtime_native_window_present(host)) {
            unsigned n = atomic_fetch_add(&g_presented, 1) + 1;
            if (runtime_native_window_surface_ready(host))
                atomic_fetch_add(&g_surface_presented, 1);
            if ((n % 60) == 0)
                LOGI("presented %u frames (surface=%u failed=%u)", n,
                     atomic_load(&g_surface_presented), atomic_load(&g_failed));
        } else {
            unsigned f = atomic_fetch_add(&g_failed, 1) + 1;
            LOGE("present failed at frame %u (total failures=%u)", frame, f);
            if (f > 10) break;
        }
        frame++;
        usleep(16666);
    }
    LOGI("present loop stopped after %u frames (surface=%u failed=%u)", frame,
         atomic_load(&g_surface_presented), atomic_load(&g_failed));
    return NULL;
}

typedef struct {
    void *host;
    char *work;
    char *mode;
} LoaderJob;

static void *loader_worker(void *arg) {
    LoaderJob *job = arg;
    LOGI("loader worker enter host=%p work=%s mode=%s", job->host,
         job->work ? job->work : "(null)", job->mode ? job->mode : "(null)");
    if (!totk_android_loader_entry) {
        LOGE("loader entry not linked in this APK; worker/attach path is ready");
    } else {
        char *argv[3] = { job->work, job->mode, NULL };
        int argc = job->work && job->mode ? 2 : (job->work ? 1 : 0);
        int rc = totk_android_loader_entry(job->host, argc, argv);
        LOGI("loader worker returned %d", rc);
    }
    free(job->work);
    free(job->mode);
    free(job);
    atomic_store(&g_loader_started, 0);
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_totk_surfacetest_MainActivity_nativeStart(JNIEnv *env, jobject thiz,
                                                   jobject holder) {
    (void)thiz;
    if (atomic_load(&g_present_run)) return;
    jclass cls = (*env)->GetObjectClass(env, holder);
    jmethodID mid = (*env)->GetMethodID(env, cls, "getSurface", "()Landroid/view/Surface;");
    jobject surface = (*env)->CallObjectMethod(env, holder, mid);
    if (!surface) { LOGE("SurfaceHolder.getSurface() returned null"); return; }
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    (*env)->DeleteLocalRef(env, surface);
    if (!win) { LOGE("ANativeWindow_fromSurface failed"); return; }

    uint32_t sw = (uint32_t)ANativeWindow_getWidth(win);
    uint32_t sh = (uint32_t)ANativeWindow_getHeight(win);
    LOGI("surface window=%p %ux%u", (void *)win, sw, sh);

    /* The host/NVN window keeps its own render size (1280x720); the swapchain
     * follows the surface extent.  This is the same split the game uses when
     * the NVN window is 720p/1080p and the display is larger. */
    void *host = runtime_native_window_create(1280, 720, 0x25, 0x5348454cULL);
    if (!host) {
        LOGE("runtime_native_window_create failed");
        ANativeWindow_release(win);
        return;
    }
    runtime_native_window_attach_android(win, host, sw, sh, 0x25);
    if (!runtime_native_window_surface_ready(host))
        LOGE("surface attach failed; presenting to the offscreen target only");
    else
        LOGI("surface attached anw=%p host=%p", (void *)win, host);

    /* A renderer-owned target, separate from the host window's own image, so
     * this exercises the same handoff NVN will use: draw into the target, set
     * it as the present source, then present through the host window. */
    g_present_target = gfx_backend_offscreen_create(1280, 720, 0x25);
    if (!g_present_target)
        LOGE("offscreen present target creation failed; using host target");
    else {
        runtime_native_window_set_present_target(host, g_present_target);
        LOGI("present target set target=%p %ux%u %s routed=%p",
             (void *)g_present_target,
             gfx_backend_offscreen_width(g_present_target),
             gfx_backend_offscreen_height(g_present_target),
             gfx_backend_offscreen_format_name(g_present_target),
             runtime_native_window_present_target(host));
    }

    g_window = win;
    g_host_window = host;
    atomic_store(&g_presented, 0);
    atomic_store(&g_surface_presented, 0);
    atomic_store(&g_failed, 0);
    atomic_store(&g_present_run, 1);
    if (pthread_create(&g_present_thread, NULL, present_loop, host) != 0) {
        LOGE("pthread_create(present) failed");
        atomic_store(&g_present_run, 0);
        runtime_native_window_set_present_target(host, NULL);
        if (g_present_target) { gfx_backend_offscreen_destroy(g_present_target); g_present_target = NULL; }
        runtime_native_window_attach_android(NULL, host, 0, 0, 0);
        runtime_native_window_destroy(host);
        g_window = NULL;
        g_host_window = NULL;
        ANativeWindow_release(win);
    }
}

JNIEXPORT void JNICALL
Java_com_totk_surfacetest_MainActivity_nativeStop(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    atomic_store(&g_present_run, 0);
    if (g_window) {
        pthread_join(g_present_thread, NULL);
        if (g_host_window) {
            /* Stop borrowing the renderer target before destroying either. */
            runtime_native_window_set_present_target(g_host_window, NULL);
            runtime_native_window_attach_android(NULL, g_host_window, 0, 0, 0);
            runtime_native_window_destroy(g_host_window);
            g_host_window = NULL;
        }
        if (g_present_target) {
            gfx_backend_offscreen_destroy(g_present_target);
            g_present_target = NULL;
        }
        ANativeWindow_release(g_window);
        g_window = NULL;
    }
    LOGI("shell stopped (presented=%u surface=%u failed=%u)",
         atomic_load(&g_presented), atomic_load(&g_surface_presented),
         atomic_load(&g_failed));
}

/* JNI entry for the next step: run the loader off the UI thread with the host
 * window already attached to the Surface.  `work` is the rewritten game data
 * directory and `mode` the loader mode (boot-sdk, probe-*, ...). */
JNIEXPORT void JNICALL
Java_com_totk_surfacetest_MainActivity_nativeRunLoader(JNIEnv *env, jobject thiz,
                                                       jstring work, jstring mode) {
    (void)thiz;
    if (atomic_load(&g_loader_started)) { LOGI("loader worker already running"); return; }
    if (!g_host_window) { LOGE("nativeRunLoader: no host window (surface not attached yet)"); return; }
    LoaderJob *job = calloc(1, sizeof *job);
    if (!job) return;
    job->host = g_host_window;
    if (work) {
        const char *s = (*env)->GetStringUTFChars(env, work, NULL);
        job->work = s ? strdup(s) : NULL;
        if (s) (*env)->ReleaseStringUTFChars(env, work, s);
    }
    if (mode) {
        const char *s = (*env)->GetStringUTFChars(env, mode, NULL);
        job->mode = s ? strdup(s) : NULL;
        if (s) (*env)->ReleaseStringUTFChars(env, mode, s);
    }
    atomic_store(&g_loader_started, 1);
    atomic_store(&g_loader_run, 1);
    if (pthread_create(&g_loader_thread, NULL, loader_worker, job) != 0) {
        LOGE("pthread_create(loader) failed");
        atomic_store(&g_loader_started, 0);
        free(job->work); free(job->mode); free(job);
    }
}

JNIEXPORT void JNICALL
Java_com_totk_surfacetest_MainActivity_nativeStopLoader(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    atomic_store(&g_loader_run, 0);
    if (atomic_load(&g_loader_started)) pthread_join(g_loader_thread, NULL);
    atomic_store(&g_loader_started, 0);
    LOGI("loader worker stopped");
}

JNIEXPORT jlong JNICALL
Java_com_totk_surfacetest_MainActivity_nativeHostWindow(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return (jlong)(intptr_t)g_host_window;
}

JNIEXPORT jint JNICALL
Java_com_totk_surfacetest_MainActivity_nativePresented(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)atomic_load(&g_presented);
}
