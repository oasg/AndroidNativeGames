/* Module: tls_probe. */
#include "loader_internal.h"

uint64_t host_add(uint64_t a, uint64_t b) { return a + b; }

uint64_t guest_call1(void *fn, uint64_t a0) {
    return runtime_guest_call(fn, runtime_guest_stack_top(), a0, 0, 0, 0, 0);
}

uint64_t guest_call2(void *fn, uint64_t a0, uint64_t a1) {
    return runtime_guest_call(fn, runtime_guest_stack_top(), a0, a1, 0, 0, 0);
}

void *tls_probe_thread(void *p) {
    TlsThreadProbe *t = p;
    runtime_thread_init();
    void *set = runtime_symbol("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm");
    void *get = runtime_symbol("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE");
    if (!set || !get) { t->ok = 0; return NULL; }
    guest_call2(set, t->slot, t->value);
    uint64_t v = guest_call1(get, t->slot);
    uint64_t tlr_tls = 0;
    if (g_slots) memcpy(&tlr_tls, (void *)(uintptr_t)(g_slots->ktlr + 0x1f0), 8);
    t->ok = v == t->value && g_slots && tlr_tls == g_slots->ktls;
    fprintf(stderr, "[totk-tls] thread slot=%u get=%#llx tlr_tls=%#llx ok=%d\n",
            t->slot, (unsigned long long)v, (unsigned long long)tlr_tls, t->ok);
    return NULL;
}

void tls_probe(void) {
    void *alloc = runtime_symbol("_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE");
    void *set = runtime_symbol("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm");
    void *get = runtime_symbol("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE");
    void *free_slot = runtime_symbol("_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE");
    void *cur = runtime_symbol("_ZN2nn2os16GetCurrentThreadEv");
    void *stack = runtime_symbol("_ZN2nn2os19GetCurrentStackInfoEPmS1_");
    fprintf(stderr, "[totk-tls] symbols alloc=%p set=%p get=%p free=%p cur=%p stack=%p\n",
            alloc, set, get, free_slot, cur, stack);
    need(alloc && set && get && free_slot && cur && stack, "nn::os TLS symbols");

    uint32_t slot = 0xffffffffu;
    uint32_t ar = (uint32_t)guest_call2(alloc, (uintptr_t)&slot, 0);
    fprintf(stderr, "[totk-tls] AllocateTlsSlot -> result=%#x slot=%u\n", ar, slot);
    need(ar == 0 && slot != 0xffffffffu, "AllocateTlsSlot");
    guest_call2(set, slot, 0x1122334455667788ull);
    uint64_t gv = guest_call1(get, slot);
    fprintf(stderr, "[totk-tls] GetTlsValue(main) -> %#llx\n", (unsigned long long)gv);
    need(gv == 0x1122334455667788ull, "GetTlsValue(main)");

    void *thread = (void *)(uintptr_t)guest_call1(cur, 0);
    uint64_t base = 0, size = 0;
    uint64_t sb = 0, ss = 0;
    runtime_guest_call(stack, runtime_guest_stack_top(), (uintptr_t)&sb, (uintptr_t)&ss, 0, 0, 0);
    base = sb; size = ss;
    fprintf(stderr, "[totk-tls] GetCurrentThread=%p stack=%#llx+%#llx\n",
            thread, (unsigned long long)base, (unsigned long long)size);
    need(thread != NULL, "GetCurrentThread");
    uint64_t tlr = 0;
    if (g_slots) tlr = g_slots->ktlr;
    need(base && size && tlr >= base && tlr < base + size, "GetCurrentStackInfo covers TLR");
    uint64_t tlr_tls = 0;
    if (tlr) memcpy(&tlr_tls, (void *)(uintptr_t)(tlr + 0x1f0), 8);
    need(g_slots && tlr_tls == g_slots->ktls,
         "ThreadLocalRegion +0x1f0 holds the guest TLS pointer");

    TlsThreadProbe tp = {.slot = slot, .value = 0xaabbccddeeff0011ull, .ok = 0};
    pthread_t th;
    need(pthread_create(&th, NULL, tls_probe_thread, &tp) == 0, "TLS probe thread");
    need(pthread_join(th, NULL) == 0, "TLS probe join");
    need(tp.ok, "per-thread TLS value");
    need(guest_call1(get, slot) == 0x1122334455667788ull, "TLS value leaked across threads");

    guest_call1(free_slot, slot);
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"tls\","
           "\"slot\":%u,\"thread_independent\":true,\"stack\":\"%#llx+%#llx\"}\n",
           slot, (unsigned long long)base, (unsigned long long)size);
    fflush(stdout);
    _exit(0);
}
