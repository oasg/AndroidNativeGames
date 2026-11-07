/* State shared by this module group. */
#ifndef TOTK_ANDROID_LOADER_STATE_H
#define TOTK_ANDROID_LOADER_STATE_H
#include "loader_types.h"


extern uint64_t timer_calls;
extern __thread uint8_t g_altstack[64 * 1024] __attribute__((aligned(16)));
extern int tls_probe_mode;
extern __thread GuestSlots *g_slots __attribute__((aligned(64)));
extern uint64_t runtime_host_ctx_off;
extern uint64_t runtime_guest_slots_off;

#endif
