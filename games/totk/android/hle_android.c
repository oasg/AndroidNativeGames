/* Minimal HLE registry for the first Android SDK-start probe.
 *
 * Guest-defined symbols are resolved first.  Undefined imports without a real
 * host implementation get a loud logging stub, so relocation can finish and
 * the first failing service call is visible in the log. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
static void oe_init(void) { fprintf(stderr, "[totk-android] nn::oe initialized on the host\n"); }
static uint64_t oe_desired_language(void) { return language_code(); }
static int oe_operation_mode(void) { return handheld() ? 0 : 1; }
static int oe_performance_mode(void) { return handheld() ? 0 : 1; }
static int oe_focus_state(void) { return 1; }
static int oe_focus_mode(int mode) { fprintf(stderr, "[totk-android] focus handling mode %d\n", mode); return 0; }
static int oe_performance_config(int mode, int config) { fprintf(stderr, "[totk-android] performance configuration mode=%d config=%#x\n", mode, config); return 0; }
static int oe_cpu_boost(int mode) { fprintf(stderr, "[totk-android] CPU boost mode %d\n", mode); return 0; }
static int oe_notification(unsigned char enabled) { (void)enabled; return 0; }
static uint64_t applet_resource_user_id(void) { return 1; }
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
static void diag_abort_init(void) { fprintf(stderr, "[totk-android] native crash observer initialized\n"); }

void *runtime_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2oe10InitializeEv", oe_init},
        {"_ZN2nn2oe18GetDesiredLanguageEv", oe_desired_language},
        {"_ZN2nn2oe16GetOperationModeEv", oe_operation_mode},
        {"_ZN2nn2oe18GetPerformanceModeEv", oe_performance_mode},
        {"_ZN2nn2oe20GetCurrentFocusStateEv", oe_focus_state},
        {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_focus_mode},
        {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_performance_config},
        {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", oe_cpu_boost},
        {"_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_notification},
        {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_notification},
        {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_notification},
        {"_ZN2nn6applet23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2am23GetAppletResourceUserIdEv", applet_resource_user_id},
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
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name)) return map[i].fn;
    return runtime_fs_override(name);   /* nn::fs runs on the host, not through fsp-srv */
}

/* The milestone stops at the original nnMain entry.  TOTK_ANDROID_RUN_NNMAIN=1
 * lets the original function continue for the next HLE milestone. */
static void nnmain_entry(void) {
    fprintf(stderr, "[totk-android] ENTER nnMain\n");
    if (getenv("TOTK_ANDROID_RUN_NNMAIN")) {
        ((void (*)(void))runtime_symbol("nnMain"))();
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
