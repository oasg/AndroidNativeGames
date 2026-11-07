/* Minimal Android app shell for the TOTK Android host.
 *
 * The shell owns only the Android-specific pieces: SurfaceView surface
 * handoff, app-private data roots, environment setup and logcat.  All loader,
 * HLE, NVN and Vulkan work stays in the existing android/ sources, compiled
 * into libtotk_android.so.  The loader runs on a worker thread; the UI thread
 * never enters guest code.
 */
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "totk-app"

extern void runtime_native_window_set_pending_surface(void *native_window, uint32_t width,
                                                      uint32_t height, uint32_t nvn_format);
extern void runtime_native_window_clear_pending_surface(void);
extern int totk_android_loader_entry(void *host_window, int argc, char **argv);

static ANativeWindow *g_surface;
static pthread_t g_loader_thread;
static atomic_int g_loader_started;

/* ------------------------------------------------------------------ */
/* native stderr/stdout -> logcat.  The loader is intentionally chatty and a
 * crash report is useless if Android drops it.  A pipe reader keeps the
 * writer from blocking while preserving line order. */

static int g_log_read;
static pthread_t g_log_thread;
static atomic_int g_log_started;

static void *log_reader(void *arg) {
    (void)arg;
    char chunk[8192];
    char line[8192];
    size_t used = 0;
    ssize_t n;
    while ((n = read(g_log_read, chunk, sizeof chunk)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (used) {
                    line[used] = 0;
                    __android_log_write(ANDROID_LOG_INFO, TAG, line);
                    used = 0;
                }
                continue;
            }
            if (used + 1 < sizeof line) line[used++] = c;
            else {
                line[used] = 0;
                __android_log_write(ANDROID_LOG_INFO, TAG, line);
                used = 0;
            }
        }
    }
    if (used) {
        line[used] = 0;
        __android_log_write(ANDROID_LOG_INFO, TAG, line);
    }
    return NULL;
}

static void start_logging(void) {
    if (atomic_exchange(&g_log_started, 1)) return;
    int fds[2];
    if (pipe(fds) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, TAG, "pipe() failed; native logs stay on stderr");
        return;
    }
    g_log_read = fds[0];
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    if (pthread_create(&g_log_thread, NULL, log_reader, NULL) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, TAG, "log reader thread failed");
        return;
    }
    pthread_detach(g_log_thread);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm;
    (void)reserved;
    start_logging();
    return JNI_VERSION_1_6;
}

/* ------------------------------------------------------------------ */
/* Surface handoff.  No host window is created here: the game's nn::vi layer
 * creates one at its own resolution, and native_window_android.c attaches this
 * surface to the first such window.  The swapchain follows the real Surface
 * extent, so 720p/1080p/1440p/20:9 panels all present through the same path. */

JNIEXPORT jboolean JNICALL
Java_com_totk_android_MainActivity_nativeAttachSurface(JNIEnv *env, jobject thiz,
                                                       jobject holder, jint width, jint height) {
    (void)thiz;
    if (!holder) return JNI_FALSE;
    jclass cls = (*env)->GetObjectClass(env, holder);
    jmethodID mid = (*env)->GetMethodID(env, cls, "getSurface", "()Landroid/view/Surface;");
    if (!mid) {
        (*env)->DeleteLocalRef(env, cls);
        return JNI_FALSE;
    }
    jobject surface = (*env)->CallObjectMethod(env, holder, mid);
    if (!surface) {
        (*env)->DeleteLocalRef(env, cls);
        __android_log_write(ANDROID_LOG_ERROR, TAG, "holder.getSurface() is null");
        return JNI_FALSE;
    }
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    (*env)->DeleteLocalRef(env, surface);
    (*env)->DeleteLocalRef(env, cls);
    if (!win) {
        __android_log_write(ANDROID_LOG_ERROR, TAG, "ANativeWindow_fromSurface failed");
        return JNI_FALSE;
    }
    ANativeWindow *old = g_surface;
    g_surface = win;
    runtime_native_window_set_pending_surface(win, (uint32_t)width, (uint32_t)height, 0x25);
    if (old && old != win) ANativeWindow_release(old);
    __android_log_print(ANDROID_LOG_INFO, TAG, "surface attached %dx%d anw=%p",
                        (int)width, (int)height, (void *)win);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_totk_android_MainActivity_nativeDetachSurface(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    runtime_native_window_clear_pending_surface();
    if (g_surface) {
        ANativeWindow_release(g_surface);
        g_surface = NULL;
    }
    __android_log_write(ANDROID_LOG_INFO, TAG, "surface detached; renderer stays on its offscreen target");
}

/* ------------------------------------------------------------------ */
/* Loader worker. */

typedef struct {
    char *work;
    char *mode;
} LoaderJob;

static int set_env(const char *name, const char *value) {
    if (!name) return -1;
    if (value && *value) return setenv(name, value, 1);
    unsetenv(name);
    return 0;
}

static void *loader_worker(void *arg) {
    LoaderJob *job = arg;
    __android_log_print(ANDROID_LOG_INFO, TAG, "loader start work=%s mode=%s",
                        job->work ? job->work : "(null)",
                        job->mode ? job->mode : "(null)");
    /* loader_main() uses the CLI shape: argv[1] = rewritten module dir,
     * argv[2] = mode.  The program name is conventional. */
    char *argv[3] = { (char *)"totk-android", job->work, job->mode };
    int rc = totk_android_loader_entry(NULL, 3, argv);
    __android_log_print(ANDROID_LOG_INFO, TAG, "loader returned %d", rc);
    free(job->work);
    free(job->mode);
    free(job);
    atomic_store(&g_loader_started, 0);
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_totk_android_MainActivity_nativeStartLoader(JNIEnv *env, jobject thiz,
                                                     jstring work, jstring mode,
                                                     jstring romfs, jstring profile,
                                                     jstring cache, jstring account_uid,
                                                     jstring operation_mode,
                                                     jboolean inits, jboolean run_nnmain) {
    (void)thiz;
    if (atomic_exchange(&g_loader_started, 1)) {
        __android_log_write(ANDROID_LOG_WARN, TAG, "loader already running");
        return JNI_FALSE;
    }
    LoaderJob *job = calloc(1, sizeof *job);
    if (!job) {
        atomic_store(&g_loader_started, 0);
        return JNI_FALSE;
    }
    const char *s;
    if (work && (s = (*env)->GetStringUTFChars(env, work, NULL))) {
        job->work = strdup(s);
        (*env)->ReleaseStringUTFChars(env, work, s);
    }
    if (mode && (s = (*env)->GetStringUTFChars(env, mode, NULL))) {
        job->mode = strdup(s);
        (*env)->ReleaseStringUTFChars(env, mode, s);
    }
    if (!job->work) job->work = strdup("");
    if (!job->mode || !*job->mode) {
        free(job->mode);
        job->mode = strdup("boot-sdk");
    }
    if (!job->work || !job->mode) {
        free(job->work);
        free(job->mode);
        free(job);
        atomic_store(&g_loader_started, 0);
        return JNI_FALSE;
    }

    char *v = NULL;
    if (romfs && (s = (*env)->GetStringUTFChars(env, romfs, NULL))) {
        v = strdup(s); (*env)->ReleaseStringUTFChars(env, romfs, s);
        set_env("TOTK_ANDROID_ROMFS", v); free(v);
    }
    if (profile && (s = (*env)->GetStringUTFChars(env, profile, NULL))) {
        v = strdup(s); (*env)->ReleaseStringUTFChars(env, profile, s);
        set_env("TOTK_ANDROID_PROFILE", v); free(v);
    }
    if (cache && (s = (*env)->GetStringUTFChars(env, cache, NULL))) {
        v = strdup(s); (*env)->ReleaseStringUTFChars(env, cache, s);
        set_env("TOTK_ANDROID_SHADER_CACHE", v); free(v);
    }
    if (account_uid && (s = (*env)->GetStringUTFChars(env, account_uid, NULL))) {
        v = strdup(s); (*env)->ReleaseStringUTFChars(env, account_uid, s);
        set_env("TOTK_ANDROID_ACCOUNT_UID", v); free(v);
    }
    if (operation_mode && (s = (*env)->GetStringUTFChars(env, operation_mode, NULL))) {
        v = strdup(s); (*env)->ReleaseStringUTFChars(env, operation_mode, s);
        set_env("TOTK_OPERATION_MODE", v); free(v);
    }
    set_env("TOTK_ANDROID_INITS", inits ? "1" : NULL);
    set_env("TOTK_ANDROID_RUN_NNMAIN", run_nnmain ? "1" : NULL);

    if (pthread_create(&g_loader_thread, NULL, loader_worker, job) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, TAG, "pthread_create(loader) failed");
        free(job->work);
        free(job->mode);
        free(job);
        atomic_store(&g_loader_started, 0);
        return JNI_FALSE;
    }
    __android_log_write(ANDROID_LOG_INFO, TAG, "loader worker started");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_totk_android_MainActivity_nativeLoaderRunning(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    return atomic_load(&g_loader_started) ? JNI_TRUE : JNI_FALSE;
}
