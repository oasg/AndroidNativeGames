/* nn::hid host backend for the Android boot lane.
 *
 * With no input script this reports one disconnected Pro Controller/Handheld
 * state (attributes 0), matching the old no-device behaviour.  A test script
 * (TOTK_ANDROID_INPUT_SCRIPT or TOTK_ANDROID_AUTOSTART) can instead press and
 * release real Npad buttons at fixed times.  Six-axis and vibration are stubs. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int hid_ok(void) { return 0; }
static int g_enabled, g_allowed = 1;
static uint32_t g_styles = 1;
static uint32_t hid_pad_id(void) {
    const char *mode = getenv("TOTK_OPERATION_MODE");
    return mode && !strcmp(mode, "handheld") ? 0x20u : 0u;
}
static uint32_t hid_pad_style(void) { return hid_pad_id() == 0x20u ? 2u : 1u; }
static int hid_initialize_npad(void) { g_enabled = 1; return 0; }
static int hid_supported_ids(const uint32_t *ids, uint64_t n) {
    g_allowed = 0;
    for (uint64_t i = 0; ids && i < n && i < 16; i++)
        if (ids[i] == hid_pad_id()) g_allowed = 1;
    if (getenv("TOTK_ANDROID_INPUT_TRACE")) {
        fprintf(stderr, "[totk-hid] supported ids n=%llu pad=%#x allowed=%d\n",
                (unsigned long long)n, hid_pad_id(), g_allowed);
    }
    return 0;
}
static int hid_supported_styles(uint32_t value) {
    g_styles = value;
    if (getenv("TOTK_ANDROID_INPUT_TRACE"))
        fprintf(stderr, "[totk-hid] supported styles=%#x pad_style=%#x\n", value, hid_pad_style());
    return 0;
}
/* Pro Controller for the normal player ids; id 0x20 is the handheld
 * controller and must report the handheld style bit or the game discards its
 * states even though GetNpadStates fills them. */
static uint32_t hid_style(const uint32_t *id) {
    uint32_t v = 0;
    if (id && g_enabled && g_allowed && *id == hid_pad_id())
        v = g_styles & hid_pad_style();
    if (getenv("TOTK_ANDROID_INPUT_TRACE")) {
        static unsigned logged;
        if (logged++ < 8)
            fprintf(stderr, "[totk-hid] style id=%#x -> %#x (enabled=%d allowed=%d styles=%#x)\n",
                    id ? *id : 0, v, g_enabled, g_allowed, g_styles);
    }
    return v;
}

/* Virtual Npad script.  A test run can press and release real button edges at
 * fixed times.  Syntax (comma separated):
 *
 *   TOTK_ANDROID_INPUT_SCRIPT="2.0:A:200,4.0:PLUS:200"
 *
 * Each entry is "<seconds>:<button>[:<hold ms>]".  Button names follow the
 * Switch Pro Controller bits: A B X Y L R ZL ZR PLUS MINUS LStick RStick
 * LEFT UP RIGHT DOWN.  TOTK_ANDROID_AUTOSTART=1 uses a built-in title-screen
 * sequence.  Nothing is pressed when neither env is set. */
#define HID_MAX_PRESSES 32
typedef struct { uint64_t at_ns, hold_ns, bits; int pressed, logged; char name[16]; } hid_press;
static hid_press g_presses[HID_MAX_PRESSES];
static unsigned g_npresses;
static uint64_t g_input_start_ns;
static int g_input_parsed;

static uint64_t hid_button_bit(const char *name) {
    static const struct { const char *name; uint64_t bit; } m[] = {
        {"A",1ull<<0}, {"B",1ull<<1}, {"X",1ull<<2}, {"Y",1ull<<3},
        {"LSTICK",1ull<<4}, {"RSTICK",1ull<<5}, {"L",1ull<<6}, {"R",1ull<<7},
        {"ZL",1ull<<8}, {"ZR",1ull<<9}, {"PLUS",1ull<<10}, {"START",1ull<<10},
        {"MINUS",1ull<<11}, {"SELECT",1ull<<11}, {"LEFT",1ull<<12},
        {"UP",1ull<<13}, {"RIGHT",1ull<<14}, {"DOWN",1ull<<15},
    };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++)
        if (!strcasecmp(name, m[i].name)) return m[i].bit;
    return 0;
}
static void hid_add_press(double at_s, uint64_t bits, unsigned hold_ms, const char *name) {
    if (g_npresses >= HID_MAX_PRESSES || !bits) return;
    hid_press *p = &g_presses[g_npresses++];
    p->at_ns = (uint64_t)(at_s * 1e9);
    p->hold_ns = (uint64_t)(hold_ms ? hold_ms : 160) * 1000000ull;
    p->bits = bits;
    snprintf(p->name, sizeof p->name, "%s", name);
}
static void hid_input_parse(void) {
    if (g_input_parsed) return;
    g_input_parsed = 1;
    const char *script = getenv("TOTK_ANDROID_INPUT_SCRIPT");
    if (script && *script) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", script);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *c1 = strchr(tok, ':');
            if (!c1) { fprintf(stderr, "[totk-hid] bad input token %s (want sec:button:ms)\n", tok); continue; }
            *c1++ = 0;
            char *c2 = strchr(c1, ':');
            unsigned hold = 160;
            if (c2) { *c2++ = 0; hold = (unsigned)strtoul(c2, NULL, 10); }
            double at = strtod(tok, NULL);
            uint64_t bit = hid_button_bit(c1);
            if (!bit) { fprintf(stderr, "[totk-hid] unknown input button %s\n", c1); continue; }
            hid_add_press(at, bit, hold, c1);
        }
    } else if (getenv("TOTK_ANDROID_AUTOSTART")) {
        /* Default title-screen sequence: A to enter the menu, then Plus to
         * skip/confirm, repeated so a slow load still sees the edges. */
        hid_add_press(15.0, 1ull<<0, 200, "A");
        hid_add_press(25.0, 1ull<<0, 200, "A");
        hid_add_press(35.0, 1ull<<0, 200, "A");
        hid_add_press(45.0, 1ull<<0, 200, "A");
        hid_add_press(55.0, 1ull<<10, 200, "PLUS");
        hid_add_press(65.0, 1ull<<0, 200, "A");
        hid_add_press(80.0, 1ull<<0, 200, "A");
        hid_add_press(100.0, 1ull<<0, 200, "A");
        hid_add_press(130.0, 1ull<<10, 200, "PLUS");
        hid_add_press(160.0, 1ull<<0, 200, "A");
    }
    if (g_npresses)
        fprintf(stderr, "[totk-hid] input script: %u press(es) %s\n", g_npresses,
                script ? "(TOTK_ANDROID_INPUT_SCRIPT)" : "(autostart)");
}
static int hid_states_core(void *states, int count, const void *id, uint32_t want_style) {
    if (!states || count <= 0 || count > 16) return 0;
    hid_input_parse();
    if (!g_npresses) {
        /* No script: keep the old no-device behaviour, but still report one
         * state (attributes 0), as nn::hid does for a disconnected pad. */
        memset(states, 0, 0x28);
        uint64_t sampling = ++g_input_start_ns;
        memcpy(states, &sampling, sizeof sampling);
        return 1;
    }
    uint64_t now = runtime_now_ns();
    if (!g_input_start_ns) g_input_start_ns = now;
    uint64_t elapsed = now - g_input_start_ns;
    uint64_t bits = 0;
    for (unsigned i = 0; i < g_npresses; i++) {
        hid_press *p = &g_presses[i];
        int down = elapsed >= p->at_ns && elapsed < p->at_ns + p->hold_ns;
        if (down) bits |= p->bits;
        if (down != p->pressed) {
            p->pressed = down;
            if (down && !p->logged) {
                p->logged = 1;
                fprintf(stderr, "[totk-hid] press %s (bit %#llx) at %.2f s\n", p->name,
                        (unsigned long long)p->bits, elapsed / 1e9);
            } else if (!down && p->logged) {
                fprintf(stderr, "[totk-hid] release %s at %.2f s\n", p->name, elapsed / 1e9);
            }
        }
    }
    if (getenv("TOTK_ANDROID_INPUT_TRACE")) {
        static unsigned traced;
        const uint32_t *pid = id;
        if (traced++ < 8)
            fprintf(stderr, "[totk-hid] states count=%d id=%#x want=%u thread=%#x buttons=%#llx\n",
                    count, pid ? *pid : 0, want_style, runtime_thread_current_handle(),
                    (unsigned long long)bits);
        static uint64_t last_bits; static unsigned polls;
        if (bits && (bits != last_bits || polls++ % 120 == 0))
            fprintf(stderr, "[totk-hid] poll thread=%#x id=%#x want=%u connected=%d buttons=%#llx\n",
                    runtime_thread_current_handle(), pid ? *pid : 0, want_style,
                    g_enabled && g_allowed && pid && *pid == hid_pad_id() &&
                    want_style == hid_pad_style() && (g_styles & want_style),
                    (unsigned long long)bits);
        last_bits = bits;
    }
    static uint64_t sample;
    uint64_t sampling = ++sample;
    const uint32_t *pid = id;
    int connected = g_enabled && g_allowed && pid && *pid == hid_pad_id() &&
                    want_style == hid_pad_style() && (g_styles & want_style);
    /* Only the first state is valid when the return value is 1.  Leave the
     * caller's older samples alone: nn::hid's history is what the game compares
     * against to see a button edge. */
    uint8_t *s = (uint8_t *)states;
    memset(s, 0, 0x28);                                 /* sampling +0, attrs +32 */
    memcpy(s, &sampling, sizeof sampling);
    if (connected) {
        memcpy(s + 8, &bits, sizeof bits);              /* buttons at +8 */
        uint32_t attrs = 1;                             /* connected */
        memcpy(s + 32, &attrs, sizeof attrs);           /* attributes at +32 */
    }
    return 1;   /* one NpadState filled; the SDK return value is the count */
}
static int hid_full_states(void *s, int n, const void *id) { return hid_states_core(s, n, id, 1); }
static int hid_handheld_states(void *s, int n, const void *id) { return hid_states_core(s, n, id, 2); }
static int hid_joy_states(void *s, int n, const void *id) { return hid_states_core(s, n, id, 0); }
static int hid_no_sixaxis(void *states, int count, const void *handles) {
    (void)handles;
    if (states && count > 0 && count <= 16) memset(states, 0, (size_t)count * 0x60);
    return 0;
}
static int hid_no_handles(void *handles, int count, const void *id, uint32_t style) {
    (void)id; (void)style;
    if (handles && count > 0 && count <= 16) memset(handles, 0, (size_t)count * 4);
    return 0;
}
static int hid_send_vibration(const void *handle, const void *value) {
    (void)handle; (void)value;
    return 0;
}

/* ControllerSupportResultInfo: int8 player count, 3 pad bytes, u32 selected
 * id.  The Switch shows its "connect controllers" applet here; the Android host
 * has one local player, so report that immediately instead of opening a
 * library applet whose completion event the HLE cannot produce. */
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } hid_support_result;
_Static_assert(sizeof(hid_support_result) == 8, "controller support result ABI");
static uint32_t hid_show_controller_support(hid_support_result *out, const void *arg) {
    (void)arg;
    if (out) *out = (hid_support_result){ .players = 1, .selected = hid_pad_id() };
    static int once;
    if (!once++) fprintf(stderr, "[totk-hid] controller support applet: one player on %#x\n", hid_pad_id());
    return 0;
}
static uint32_t hid_show_controller_support_v1(const void *arg) {
    (void)arg;
    return 0;
}
static uint32_t hid_show_controller_support_sys(hid_support_result *out, const void *arg, int flag) {
    (void)flag;
    return hid_show_controller_support(out, arg);
}
static uint32_t hid_show_controller_support_sys_v1(const void *arg, int flag) {
    (void)arg; (void)flag;
    return 0;
}
static uint32_t hid_show_controller_noop(const void *arg) { (void)arg; return 0; }
static uint32_t hid_show_controller_noop2(const void *arg, int x) { (void)arg; (void)x; return 0; }

void *runtime_hid_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn3hid14InitializeNpadEv", hid_initialize_npad},
        /* Keyboard/mouse/touch also activate the SDK's HID shared-memory
         * accessor.  There is no hid service on this host yet, so they are
         * host no-device initializers just like InitializeNpad. */
        {"_ZN2nn3hid15InitializeMouseEv", hid_ok},
        {"_ZN2nn3hid18InitializeKeyboardEv", hid_ok},
        {"_ZN2nn3hid21InitializeTouchScreenEv", hid_ok},
        {"_ZN2nn3hid18InitializeDebugPadEv", hid_ok},
        {"_ZN2nn3hid17InitializeGestureEv", hid_ok},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", hid_supported_ids},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_supported_styles},
        {"_ZN2nn3hid15GetNpadStyleSetERKj", hid_style},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_full_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_joy_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_joy_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_joy_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_handheld_states},
        {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_no_handles},
        {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_no_sixaxis},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_ok},
        {"_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_no_handles},
        {"_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_ok},
        {"_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", hid_send_vibration},
        {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hid_ok},
        {"_ZN2nn3hid14DisconnectNpadERKj", hid_ok},
        {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", hid_show_controller_support},
        {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", hid_show_controller_support_v1},
        {"_ZN2nn3hid6system30ShowControllerSupportForSystemEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgEb", hid_show_controller_support_sys},
        {"_ZN2nn3hid6system30ShowControllerSupportForSystemERKNS0_20ControllerSupportArgEb", hid_show_controller_support_sys_v1},
        {"_ZN2nn3hid24ShowControllerStrapGuideEv", hid_ok},
        {"_ZN2nn3hid28ShowControllerFirmwareUpdateERKNS0_27ControllerFirmwareUpdateArgE", hid_show_controller_noop},
        {"_ZN2nn3hid6system37ShowControllerFirmwareUpdateForSystemERKNS0_27ControllerFirmwareUpdateArgENS1_23ControllerSupportCallerE", hid_show_controller_noop2},
        {"_ZN2nn3hid6system35ShowControllerKeyRemappingForSystemERKNS1_25ControllerKeyRemappingArgENS1_23ControllerSupportCallerE", hid_show_controller_noop2},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
