/* Minimal Android Surface test: real ANativeWindow -> VkSwapchainKHR clear.
 * This is intentionally separate from the game loader so it can run as a
 * normal Activity without game data. */
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "gfx_backend_android.h"

#define TAG "totk-surface"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static ANativeWindow *g_window;
static pthread_t g_thread;
static atomic_int g_run;
static atomic_int g_started;

static void *surface_loop(void *arg) {
    (void)arg;
    ANativeWindow *win = g_window;
    if (!win) return NULL;
    LOGI("surface loop enter window=%p", (void *)win);
    ANativeWindow_acquire(win);
    LOGI("attach_android_surface begin");
    if (!gfx_backend_attach_android_surface(win, 1280, 720, 0x25)) {
        LOGE("attach_android_surface failed");
        ANativeWindow_release(win);
        return NULL;
    }
    atomic_store(&g_started, 1);
    unsigned frame = 0;
    while (atomic_load(&g_run)) {
        float t = (float)(frame % 120) / 119.0f;
        if (!gfx_backend_surface_present(0.08f + 0.25f * t, 0.12f, 0.30f - 0.20f * t, 1.0f)) {
            LOGE("surface present failed at frame %u", frame);
            break;
        }
        frame++;
        if ((frame % 60) == 0) LOGI("presented %u frames", frame);
        usleep(16666);
    }
    LOGI("surface loop stopped after %u frames", frame);
    gfx_backend_detach_android_surface();
    ANativeWindow_release(win);
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_totk_surfacetest_MainActivity_nativeStart(JNIEnv *env, jobject thiz, jobject holder) {
    (void)thiz;
    LOGI("nativeStart");
    if (atomic_load(&g_run)) return;
    jclass cls = (*env)->GetObjectClass(env, holder);
    jmethodID mid = (*env)->GetMethodID(env, cls, "getSurface", "()Landroid/view/Surface;");
    jobject surface = (*env)->CallObjectMethod(env, holder, mid);
    if (!surface) { LOGE("SurfaceHolder.getSurface() returned null"); return; }
    g_window = ANativeWindow_fromSurface(env, surface);
    (*env)->DeleteLocalRef(env, surface);
    if (!g_window) { LOGE("ANativeWindow_fromSurface failed"); return; }
    atomic_store(&g_run, 1);
    atomic_store(&g_started, 0);
    if (pthread_create(&g_thread, NULL, surface_loop, NULL) != 0) {
        LOGE("pthread_create failed");
        ANativeWindow_release(g_window);
        g_window = NULL;
        atomic_store(&g_run, 0);
    }
}

JNIEXPORT void JNICALL
Java_com_totk_surfacetest_MainActivity_nativeStop(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    atomic_store(&g_run, 0);
    if (g_window) {
        pthread_join(g_thread, NULL);
        ANativeWindow_release(g_window);
        g_window = NULL;
    }
    LOGI("surface stopped");
}
