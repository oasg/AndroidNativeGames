/* Internal module API. */
#ifndef TOTK_ANDROID_SERVICES_API_0_H
#define TOTK_ANDROID_SERVICES_API_0_H
#include "services_state.h"

/* Minimal Android Horizon SVC layer for the first SDK-start probe.
 * Thread/synchronization SVCs come from runtime/threads.c (svc_thread);
 * memory/time/debug are the small set nn::init::Start needs before nnMain. */



void runtime_guest_frame_report(uint64_t fp, uint64_t lr);

/* The macOS services layer calls this before dispatching every SVC (services.c:
 * runtime_svc).  The Android layer used to rely only on the SIGUSR2 async
 * preemption path, so a guest spin loop that does enter an SVC (the SDK's
 * LightMessageQueue lock loop calls SVC 0x36 while it is contended) could never
 * hand its virtual core to a better waiter.  Keep the same preemption point as
 * the other host: cheap when no waiter beats this thread. */
void runtime_core_checkpoint(void);

void runtime_guest_map_extra(uintptr_t base, uintptr_t size);

void runtime_guest_commit(uintptr_t base, uintptr_t size);

void runtime_guest_uncommit(uintptr_t base, uintptr_t size);

int commit_range(uintptr_t addr, uintptr_t *base, uintptr_t *end);

uintptr_t next_commit(uintptr_t addr);

uint8_t *reserve_fixed(uint64_t at, uint64_t size);

void memory_init(void);

void query_memory(uint64_t out, uint64_t addr);

void logger_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

uint32_t lm_open_logger(IpcSession *s, IpcResponse *rs);

void lm_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

__attribute__((constructor)) static void register_android_services(void);

void runtime_guest_stack_info(uint64_t *base, uint64_t *size);

uint64_t get_info(unsigned id);

uint64_t runtime_svc(unsigned num, uint64_t *r, const char *module, uint64_t offset);

#endif
