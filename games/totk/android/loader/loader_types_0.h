/* Type and object layouts. */
#ifndef TOTK_ANDROID_LOADER_TYPES_0_H
#define TOTK_ANDROID_LOADER_TYPES_0_H
#include "loader_prelude.h"



typedef struct {
    uintptr_t host_stack_top;   /* offset 0: gate_android.S reads this */
    uintptr_t host_stack_base;  /* offset 8 */
    uint8_t *guest_tls;         /* guest TPIDR_EL0 value (zeroed 8 KiB) */
    uint8_t *guest_tlr;         /* guest TPIDRRO_EL0 value (zeroed 0x200) */
    uint8_t *dummy;
    uintptr_t guest_stack_top;  /* guest code entry stack (not the HLE stack) */
} HostContext;



typedef struct {
    uint64_t k18, ktls, ktlr, reserved[5];
} GuestSlots;



typedef struct { uint32_t slot; uint64_t value; int ok; } TlsThreadProbe;



/* ------------------------------------------------------------------------- */
/* SPIR-V descriptor signature                                               */

typedef struct {
    uint32_t kind;        /* 0 none, 1 image, 2 sampler, 3 sampled image, 4 array,
                           * 5 runtime array, 6 struct, 7 pointer */
    uint32_t elem;
    uint32_t length;
    uint32_t dim, depth, arrayed, ms, sampled, format;
    uint32_t storage, block;
} ReflectSpvType;



typedef struct { uint32_t id, ptr, storage; } ReflectSpvVar;

#endif
