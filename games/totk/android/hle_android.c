/* Minimal HLE registry for the first Android SDK-start probe.
 *
 * Guest-defined symbols are resolved first.  Undefined imports without a real
 * host implementation get a loud logging stub, so relocation can finish and
 * the first failing service call is visible in the log. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static uint64_t hle_stub(void) { return 0; }

/* ---------- nn::oe / nn::apm ---------- */
static int handheld(void) {
    static int value = -1;
    if (value < 0) {
        const char *mode = getenv("TOTK_OPERATION_MODE");
        value = mode && !strcmp(mode, "handheld");
    }
    return value;
}
static uint64_t language_code(void) {
    const char *lang = getenv("TOTK_LANG");
    char code[8] = {0};
    if (lang && strlen(lang) <= 7) memcpy(code, lang, strlen(lang));
    else memcpy(code, "en-US", 5);
    uint64_t v; memcpy(&v, code, sizeof v); return v;
}
static void oe_push_message(uint32_t message);
static int oe_focus_pushed;
static void oe_init(void) {
    fprintf(stderr, "[totk-android] nn::oe initialized on the host\n");
    if (!oe_focus_pushed) {
        oe_focus_pushed = 1;
        oe_push_message(15);   /* focus gained, as the macOS window delegate sends */
    }
}
static uint64_t oe_desired_language(void) { return language_code(); }
static int oe_operation_mode(void) { return handheld() ? 0 : 1; }
static int oe_performance_mode(void) { return handheld() ? 0 : 1; }
static int oe_focus_state(void) { return 1; }
/* The SDK implementation calls nn::am::GetAudioController(), which this HLE
 * does not provide (it returns null).  Report the normal unity balance; the
 * host audio mixer has no applet volume policy yet. */
static uint64_t oe_expected_volume_balance(float *main_volume, float *library_volume) {
    if (main_volume) *main_volume = 1.0f;
    if (library_volume) *library_volume = 1.0f;
    return 0;
}
static uint64_t oe_set_expected_volume_balance(float main_volume, float library_volume) {
    (void)main_volume;
    (void)library_volume;
    return 0;
}
/* nn::oe notification queue.  The macOS frontend pushes message 15 when the
 * window gains focus; the game enables focus handling and its pre-title loop
 * waits for that notification.  Android has no window-focus callback, so
 * deliver one queued focus message from oe::Initialize. */
static pthread_mutex_t oe_msg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t oe_msg_cv = PTHREAD_COND_INITIALIZER;
static uint32_t oe_msgs[16];
static unsigned oe_msg_head, oe_msg_tail;
static unsigned oe_try_calls, oe_pop_calls;
static void oe_push_message(uint32_t message) {
    pthread_mutex_lock(&oe_msg_lock);
    if (oe_msg_tail - oe_msg_head < 16) oe_msgs[oe_msg_tail++ % 16] = message;
    pthread_cond_broadcast(&oe_msg_cv);
    pthread_mutex_unlock(&oe_msg_lock);
}
unsigned runtime_oe_try_calls(void) { return __atomic_load_n(&oe_try_calls, __ATOMIC_RELAXED); }
unsigned runtime_oe_pop_calls(void) { return __atomic_load_n(&oe_pop_calls, __ATOMIC_RELAXED); }
static int oe_load_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_LOAD") != NULL;
    return on;
}
static int oe_try_pop_notification_message(uint32_t *message) {
    pthread_mutex_lock(&oe_msg_lock);
    int ok = oe_msg_head != oe_msg_tail;
    uint32_t m = ok ? oe_msgs[oe_msg_head++ % 16] : 0;
    pthread_mutex_unlock(&oe_msg_lock);
    if (message) *message = m;
    if (oe_load_trace() && oe_try_calls++ < 24)
        fprintf(stderr, "[totk-load] oe TryPopNotification -> %d msg=%u\n", ok, m);
    return ok;
}
static uint64_t oe_pop_notification_message(void) {
    pthread_mutex_lock(&oe_msg_lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    while (oe_msg_head == oe_msg_tail &&
           pthread_cond_timedwait(&oe_msg_cv, &oe_msg_lock, &ts) == 0) {}
    uint64_t m = oe_msg_head != oe_msg_tail ? oe_msgs[oe_msg_head++ % 16] : 0;
    pthread_mutex_unlock(&oe_msg_lock);
    if (oe_load_trace() && oe_pop_calls++ < 24)
        fprintf(stderr, "[totk-load] oe PopNotification -> %llu\n", (unsigned long long)m);
    return m;
}
static int oe_focus_mode(int mode) { fprintf(stderr, "[totk-android] focus handling mode %d\n", mode); return 0; }
static int oe_performance_config(int mode, int config) { fprintf(stderr, "[totk-android] performance configuration mode=%d config=%#x\n", mode, config); return 0; }
static int oe_cpu_boost(int mode) { fprintf(stderr, "[totk-android] CPU boost mode %d\n", mode); return 0; }
static int oe_notification(unsigned char enabled) { (void)enabled; return 0; }
/* No system startup logo is drawn on the Android host, so there is nothing to
 * dismiss.  The SDK's implementation opens the oe/am applet proxy and faults
 * when that host service has no logo session (sdk+0x39b080 after the
 * controller-support applet). */
static void oe_finish_startup_logo(void) { fprintf(stderr, "[totk-android] startup logo finished\n"); }
static uint64_t program_total_active_time(void) {
    static uint64_t start;
    if (!start) start = runtime_now_ns();
    return runtime_now_ns() - start;
}
/* The single native executable was launched directly, not by an applet
 * ExecuteProgram chain; -1 keeps the game's normal user-open path. */
static int applet_previous_program_index(void) { return -1; }
/* Boot has no pending launch parameter.  The macOS applet host answers this
 * synchronously; leaving the SDK implementation to talk to an unimplemented
 * applet proxy can stall the title. */
static int oe_try_pop_launch_parameter(uint64_t *size, void *buffer, uint64_t capacity) {
    (void)buffer; (void)capacity;
    if (size) *size = 0;
    return 0;
}
static uint64_t applet_resource_user_id(void) { return 1; }
void *runtime_nvn_bootstrap(const char *name);
void *runtime_hid_override(const char *name);

/* Host-side vi layer/window lifecycle.  The SDK's real CreateLayer needs an
 * Android dispdrv Parcel from a compositor; Phase 2A uses an explicit host
 * layer + offscreen NativeWindow description instead.  NVN stores the window
 * pointer in its builder, and nvnWindowGetNativeWindow returns the same host
 * object.  The app shell can attach a real ANativeWindow later. */
typedef struct {
    uint32_t magic;
    uint32_t pad;
    uint64_t id;
    void *window;
} HostViLayer;
#define HOST_VI_LAYER_MAGIC 0x594c5648u /* "HVLY" */
extern void *runtime_native_window_create(uint32_t width, uint32_t height, uint32_t format, uint64_t layer_id);
extern int runtime_native_window_info(const void *p, uint32_t *width, uint32_t *height,
                                      uint32_t *format, void **buffer, size_t *size);
static uint32_t vi_create_layer_host(void **out, void *display) {
    (void)display;
    if (!out) return 0xf601;
    HostViLayer *l = calloc(1, sizeof *l);
    if (!l) return 0xc01;
    static uint64_t next_layer = 1;
    l->magic = HOST_VI_LAYER_MAGIC;
    l->id = next_layer++;
    l->window = runtime_native_window_create(handheld() ? 1280 : 1920,
                                             handheld() ? 720 : 1080, 0x25, l->id);
    if (!l->window) { free(l); return 0xc01; }
    *out = l;
    return 0;
}
static uint32_t vi_get_native_window_host(void **out, void *layer) {
    HostViLayer *l = layer;
    if (!out || !l || l->magic != HOST_VI_LAYER_MAGIC) return 0xf601;
    *out = l->window;
    return 0;
}

/* nn::am::GetSelfController returns a pointer to the ISelfController proxy.
 * Until the am service is implemented, give the SDK a valid object whose
 * virtual calls succeed; the first method it needs (CreateLayer -> vtable+0xd0)
 * only checks the result.  A zeroed pointer here is what used to crash. */
static uint64_t am_self_vtable[0x40];
static uint64_t am_self_object[0x20];
static uint64_t am_virtual_stub(void) { return 0; }
/* ISelfController vtable+0xd0: CreateManagedDisplaySeparableLayer(Out<u64>,
 * Out<u64>).  nn::vi::CreateLayer calls it before it asks the display service
 * for a layer; a success with no written layer ids leaves the caller with
 * uninitialised objects. */
static uint64_t am_create_managed_layer(void *self, uint64_t *layer, uint64_t *separable) {
    (void)self;
    if (layer) *layer = 1;
    if (separable) *separable = 2;
    return 0;
}
static void *am_get_self_controller(void) {
    static int ready;
    if (!ready) {
        for (unsigned i = 0; i < sizeof am_self_vtable / sizeof *am_self_vtable; i++)
            am_self_vtable[i] = (uintptr_t)am_virtual_stub;
        am_self_vtable[0xd0 / 8] = (uintptr_t)am_create_managed_layer;
        am_self_object[0] = (uintptr_t)am_self_vtable;
        ready = 1;
    }
    return am_self_object;
}
int runtime_nvn_window_selftest(void *nw);
extern int runtime_native_window_clear(void *p, uint32_t rgba);
int runtime_vi_selftest(void) {
    void *layer = NULL, *nw = NULL;
    uint32_t w = 0, h = 0, fmt = 0;
    void *buf = NULL;
    size_t size = 0;
    if (vi_create_layer_host(&layer, NULL) != 0) return 0;
    if (vi_get_native_window_host(&nw, layer) != 0) return 0;
    if (!runtime_native_window_info(nw, &w, &h, &fmt, &buf, &size)) return 0;
    if (w != 1920 || h != 1080 || fmt != 0x25 || !buf || size != (size_t)1920 * 1080 * 4) return 0;
    if (!runtime_native_window_clear(nw, 0xff203040u)) return 0;
    if (!runtime_nvn_window_selftest(nw)) return 0;
    fprintf(stderr, "[totk-vi] selftest layer=%p native-window=%p %ux%u format=%#x\n",
            layer, nw, w, h, fmt);
    return 1;
}

static void apm_init(void) { fprintf(stderr, "[totk-android] nn::apm initialized on the host\n"); }
static void apm_finalize(void) { }
static int apm_performance_mode(void) { return handheld() ? 0 : 1; }
static int apm_request_mode(int mode) { fprintf(stderr, "[totk-android] apm performance mode %d\n", mode); return 0; }
static int apm_boost(int mode) { fprintf(stderr, "[totk-android] apm CPU boost mode %d\n", mode); return 0; }
static int apm_configuration(int mode) { (void)mode; return 0; }
static int apm_current_configuration(void) { return 0; }
static void aoc_init(void) { fprintf(stderr, "[totk-android] add-on content: none\n"); }
static int aoc_count(void) { return 0; }
static int aoc_list(int *out, int offset, int count) {
    (void)out;
    if (offset < 0 || count < 0) runtime_fail("invalid add-on content list range");
    return 0;
}
static void pctl_init(void) { fprintf(stderr, "[totk-android] parental controls: disabled\n"); }
static void err_init(void) { fprintf(stderr, "[totk-android] error contexts: local diagnostics\n"); }
/* The system error applet has no Android host.  Log the result instead of
 * letting the SDK open an applet whose completion event never arrives. */
static void err_show_error(uint32_t result) {
    fprintf(stderr, "[totk-android] nn::err::ShowError result=%#x (%u-%u)\n",
            result, 2000 + (result & 0x1ff), (result >> 9) & 0x1fff);
}
static uint64_t prepo_save(const void *report) {
    (void)report;
    static unsigned n;
    if (n++ < 8) fprintf(stderr, "[totk-android] play report kept local\n");
    return 0;
}
static uint64_t prepo_noop(void) { return 0; }
static uint64_t prepo_connection_status(void *report, const char *a, const char *b, const char *c) {
    (void)report; (void)a; (void)b; (void)c;
    return 0;
}
static void diag_abort_init(void) { fprintf(stderr, "[totk-android] native crash observer initialized\n"); }

/* The launch-parameter channel is an applet storage object.  Boot has no
 * pending parameters yet, so report an empty queue instead of dereferencing
 * the uninitialised applet proxy. */
static int applet_try_pop_parameter(void *storage, int kind) {
    (void)storage; (void)kind;
    return 0;
}

/* nn::nfp: the Android host has no NFC reader.  The generic IPC stub returned
 * success for nfp:user Open without a device object, and the SDK then
 * dereferenced NULL (sdk+0x378dec).  Keep the macOS host's no-device HLE so
 * the amiibo path cleanly reports "no device" and boot continues. */
#define NFP_DEVICE_NOT_FOUND 0x8073u   /* nfp module 115, description 64 */
static int nfp_state;
static unsigned nfp_initialize(void) { nfp_state = 1; return 0; }
static unsigned nfp_finalize(void) { nfp_state = 0; return 0; }
static int nfp_get_state(void) { return nfp_state; }
static unsigned nfp_list(void *handles, int *count, int max) {
    (void)handles; (void)max;
    if (count) *count = 0;
    return NFP_DEVICE_NOT_FOUND;
}
static unsigned nfp_attach_availability(void *event) {
    if (!event) runtime_fail("nfp availability event");
    hle_bind_system_event(event, 0);
    return 0;
}
static unsigned nfp_no_device(void) { return NFP_DEVICE_NOT_FOUND; }
static int nfp_device_state(const void *h) { (void)h; return 0; }

void *runtime_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2oe10InitializeEv", oe_init},
        {"_ZN2nn2oe18GetDesiredLanguageEv", oe_desired_language},
        {"_ZN2nn2oe16GetOperationModeEv", oe_operation_mode},
        {"_ZN2nn2oe17FinishStartupLogoEv", oe_finish_startup_logo},
        {"_ZN2nn2oe18GetPerformanceModeEv", oe_performance_mode},
        {"_ZN2nn2oe20GetCurrentFocusStateEv", oe_focus_state},
        {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_expected_volume_balance},
        {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", oe_set_expected_volume_balance},
        {"_ZN2nn2oe25TryPopNotificationMessageEPj", oe_try_pop_notification_message},
        {"_ZN2nn2oe22PopNotificationMessageEv", oe_pop_notification_message},
        {"_ZN2nn2oe21TryPopLaunchParameterEPmPvm", oe_try_pop_launch_parameter},
        {"_ZN2nn2oe25GetProgramTotalActiveTimeEv", program_total_active_time},
        {"_ZN2nn6applet23GetPreviousProgramIndexEv", applet_previous_program_index},
        {"_ZN2nn3err9ShowErrorENS_6ResultE", err_show_error},
        {"_ZN2nn5prepo10PlayReport4SaveEv", prepo_save},
        {"_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", prepo_save},
        {"_ZN2nn5prepo28RequestImmediateTransmissionEv", prepo_noop},
        {"_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", prepo_connection_status},
        {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_focus_mode},
        {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_performance_config},
        {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", oe_cpu_boost},
        {"_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_notification},
        {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_notification},
        {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_notification},
        {"_ZN2nn6applet23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2am23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2am17GetSelfControllerEv", am_get_self_controller},
        {"_ZN2nn3apm10InitializeEv", apm_init},
        {"_ZN2nn3apm8FinalizeEv", apm_finalize},
        {"_ZN2nn3apm18GetPerformanceModeEv", apm_performance_mode},
        {"_ZN2nn3apm22RequestPerformanceModeENS0_15PerformanceModeE", apm_request_mode},
        {"_ZN2nn3apm15SetCpuBoostModeENS0_12CpuBoostModeE", apm_boost},
        {"_ZN2nn3apm27GetPerformanceConfigurationENS0_15PerformanceModeE", apm_configuration},
        {"_ZN2nn3apm34GetCurrentPerformanceConfigurationEv", apm_current_configuration},
        {"_ZN2nn3aoc10InitializeEv", aoc_init},
        {"_ZN2nn3aoc17CountAddOnContentEv", aoc_count},
        {"_ZN2nn3aoc16ListAddOnContentEPiii", aoc_list},
        {"_ZN2nn4pctl10InitializeEv", pctl_init},
        {"_ZN2nn3err26InitializeContextWriterApiEv", err_init},
        {"_ZN2nn4diag34InitializeApplicationAbortObserverEv", diag_abort_init},
        {"_ZN2nn6applet37TryPopFromApplicationParameterChannelEPNS0_13StorageHandleENS0_19LaunchParameterKindE", applet_try_pop_parameter},
        {"_ZN2nn3nfp10InitializeEv", nfp_initialize},
        {"_ZN2nn3nfp8FinalizeEv", nfp_finalize},
        {"_ZN2nn3nfp8GetStateEv", nfp_get_state},
        {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list},
        {"_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", nfp_attach_availability},
        {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", nfp_device_state},
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_no_device},
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer_host},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window_host},
        {"nvnBootstrapLoader", runtime_nvn_bootstrap},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name)) return map[i].fn;
    void *hid = runtime_hid_override(name);
    if (hid) return hid;
    void *clock = runtime_time_override(name);
    if (clock) return clock;
    return runtime_fs_override(name);   /* nn::fs runs on the host, not through fsp-srv */
}

/* The milestone stops at the original nnMain entry.  TOTK_ANDROID_RUN_NNMAIN=1
 * lets the original function continue for the next HLE milestone. */
uintptr_t runtime_guest_stack_top(void);
uint64_t runtime_guest_call(void *fn, uintptr_t stack_top,
                            uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4);

static void nnmain_entry(void) {
    fprintf(stderr, "[totk-android] ENTER nnMain\n");
    if (getenv("TOTK_ANDROID_RUN_NNMAIN")) {
        runtime_guest_call(runtime_symbol("nnMain"), runtime_guest_stack_top(), 0, 0, 0, 0, 0);
        return;
    }
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"nnMain-entry\"}\n");
    fflush(stdout);
    runtime_hle_trace_report();
    _exit(0);
}

void *runtime_observer(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"nnMain", nnmain_entry},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}

void *runtime_hle_stub(const char *name) {
    static unsigned logged;
    if (logged++ < 400)
        fprintf(stderr, "[totk-android] unimplemented import -> stub: %s\n", name);
    return (void *)hle_stub;
}
