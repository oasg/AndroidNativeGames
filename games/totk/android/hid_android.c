/* Minimal nn::hid host backend for Phase 1.
 *
 * The SDK's InitializeNpad activates a HID shared-memory session through the
 * hid service.  That protocol is Phase 1C's next layer; until Android input is
 * connected, report a valid "no Npad connected" state and zero the caller's
 * state buffers.  Six-axis and vibration are stubs, as requested for Phase 1.
 * This keeps the game's initialization alive without faking a pad press. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int hid_ok(void) { return 0; }
static uint32_t hid_style(void) { return 1; }          /* Pro Controller style bit */

/* SDK state sizes: NpadCommonState (all Npad*State) is 0x28, the six-axis state
 * is 0x60, and the six-axis/vibration handles are u32.  Zero exactly the array
 * the caller allocated; count is its element count. */
static int hid_no_states(void *states, int count, const void *id) {
    (void)id;
    if (states && count > 0 && count <= 16) memset(states, 0, (size_t)count * 0x28);
    return 0;
}
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
    if (out) *out = (hid_support_result){ .players = 1, .selected = 0 };
    static int once;
    if (!once++) fprintf(stderr, "[totk-hid] controller support applet: one player (handheld)\n");
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
        {"_ZN2nn3hid14InitializeNpadEv", hid_ok},
        /* Keyboard/mouse/touch also activate the SDK's HID shared-memory
         * accessor.  There is no hid service on this host yet, so they are
         * host no-device initializers just like InitializeNpad. */
        {"_ZN2nn3hid15InitializeMouseEv", hid_ok},
        {"_ZN2nn3hid18InitializeKeyboardEv", hid_ok},
        {"_ZN2nn3hid21InitializeTouchScreenEv", hid_ok},
        {"_ZN2nn3hid18InitializeDebugPadEv", hid_ok},
        {"_ZN2nn3hid17InitializeGestureEv", hid_ok},
        {"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", hid_ok},
        {"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_ok},
        {"_ZN2nn3hid15GetNpadStyleSetERKj", hid_style},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_no_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_no_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_no_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_no_states},
        {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_no_states},
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
