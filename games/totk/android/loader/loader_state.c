/* Definitions of the state declared in loader_state.h. */
#include "loader_state.h"
uint64_t timer_calls;
/* Per-thread alternate signal stack so a stack-overflow SIGSEGV in the
 * Adreno/LLVM submit path still reaches crash_handler instead of dying with a
 * bare "Segmentation fault". */
__thread uint8_t g_altstack[64 * 1024] __attribute__((aligned(16)));
int tls_probe_mode;
__thread GuestSlots *g_slots __attribute__((aligned(64)));
uint64_t runtime_host_ctx_off;
uint64_t runtime_guest_slots_off;
