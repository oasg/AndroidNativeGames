/* Module: crash_1. */
#include "loader_internal.h"

void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *u = ctx;
    uint64_t pc = u->uc_mcontext.pc;
    uint64_t lr = u->uc_mcontext.regs[30];
    uint64_t sp = u->uc_mcontext.sp;
    uint64_t fp = u->uc_mcontext.regs[29];
    char pcd[128], lrd[128];
    describe_address(pc, pcd, sizeof pcd);
    describe_address(lr, lrd, sizeof lrd);
    fprintf(stderr, "[crash] sig=%d pc=%s lr=%s fault=%p sp=%#llx x0=%#llx x1=%#llx x8=%#llx"
                    " x9=%#llx x10=%#llx x11=%#llx x12=%#llx x19=%#llx x20=%#llx\n",
            sig, pcd, lrd, si ? si->si_addr : NULL, (unsigned long long)sp,
            (unsigned long long)u->uc_mcontext.regs[0], (unsigned long long)u->uc_mcontext.regs[1],
            (unsigned long long)u->uc_mcontext.regs[8], (unsigned long long)u->uc_mcontext.regs[9],
            (unsigned long long)u->uc_mcontext.regs[10], (unsigned long long)u->uc_mcontext.regs[11],
            (unsigned long long)u->uc_mcontext.regs[12], (unsigned long long)u->uc_mcontext.regs[19],
            (unsigned long long)u->uc_mcontext.regs[20]);

    if (getenv("TOTK_ANDROID_CRASH_DUMP")) {
        const unsigned long long *rr = (const unsigned long long *)u->uc_mcontext.regs;
        /* The relocated `mrs tpidr_el0` reads GuestSlots->ktls.  Keep the
         * host TP and slot values in the dump so a lost guest TP is visible. */
        uint64_t host_tp = read_tpidr_el0();
        uint64_t slots_at = 0, ktls = 0, ktlr = 0, canary = 0;
        int slots_ok = 0, slots_read = 0;
        if (runtime_guest_slots_off) {
            slots_ok = crash_read((uintptr_t)(host_tp + runtime_guest_slots_off), &slots_at, 8);
            if (slots_at) {
                slots_read = crash_read((uintptr_t)(slots_at + 8), &ktls, 8);
                crash_read((uintptr_t)(slots_at + 16), &ktlr, 8);
                crash_read((uintptr_t)(slots_at + 0x18), &canary, 8);
            }
        }
        if (rr[0] >= 0x1000) {
            uint64_t w[32] = {0};
            if (crash_read(rr[0], w, sizeof w)) {
                fprintf(stderr, "[crash] x0[%#llx] +0x00:", (unsigned long long)rr[0]);
                for (unsigned k = 0; k < 16; k++) fprintf(stderr, " %016llx", (unsigned long long)w[k]);
                fprintf(stderr, "\n[crash] x0         +0x40:");
                for (unsigned k = 8; k < 24; k++) fprintf(stderr, " %016llx", (unsigned long long)w[k]);
                fputc('\n', stderr);
                for (unsigned li = 0; li < 2; li++) {
                    unsigned base = 9 + li * 3;   /* w[9+li*3] = +0x48/+0x60 sentinel */
                    uint64_t sent = rr[0] + 0x48 + li * 0x18;
                    uint64_t head = w[base + 1], cnt = w[base + 2] & 0xffffffffu;
                    fprintf(stderr, "[crash] list%u sent=%#llx head=%#llx count=%llu chain:",
                            li, (unsigned long long)sent, (unsigned long long)head,
                            (unsigned long long)cnt);
                    uint64_t n = head;
                    for (unsigned k = 0; k < 40 && n; k++) {
                        uint64_t nxt = 0;
                        crash_read(n + 8, &nxt, 8);
                        fprintf(stderr, " %#llx->%#llx", (unsigned long long)n,
                                (unsigned long long)nxt);
                        if (nxt == 0 || nxt == sent) break;
                        n = nxt;
                    }
                    fputc('\n', stderr);
                }
                uint64_t node = w[13];   /* x0+0x68: second list head */
                if (node >= 0x1000) {
                    uint64_t nw[8] = {0};
                    if (crash_read(node, nw, sizeof nw)) {
                        fprintf(stderr, "[crash] node[%#llx]:", (unsigned long long)node);
                        for (unsigned k = 0; k < 8; k++) fprintf(stderr, " %016llx", (unsigned long long)nw[k]);
                        fputc('\n', stderr);
                    }
                }
            }
        }
        fprintf(stderr, "[crash] tls tp=%#llx slots=%#llx slots_ok=%d read=%d ktls=%#llx"
                " ktlr=%#llx canary=%#llx\n", (unsigned long long)host_tp,
                (unsigned long long)slots_at, slots_ok, slots_read,
                (unsigned long long)ktls, (unsigned long long)ktlr,
                (unsigned long long)canary);
        if (rr[1] && !(rr[1] & 7)) {
            uint64_t s[2] = {0};
            if (crash_read(rr[1], s, sizeof s)) {
                char text[129] = {0};
                uint64_t len = s[1] & 0xffffffffu;
                if (len >= sizeof text) len = sizeof text - 1;
                if (s[0] && len && crash_read(s[0], text, (size_t)len))
                    fprintf(stderr, "[crash] x1 string ptr=%#llx len=%llu '%.*s'\n",
                            (unsigned long long)s[0],
                            (unsigned long long)(s[1] & 0xffffffffu),
                            (int)len, text);
            }
        }
        for (unsigned i = 0; i < NMODULES; i++) {
            uint64_t key0 = 0;
            if (crash_read((uintptr_t)(modules[i].base + modules[i].key_off), &key0, 8))
                fprintf(stderr, "[crash] key[0] %s=%#llx\n", modules[i].name,
                        (unsigned long long)key0);
        }
        for (unsigned i = 0; i <= 28; i++) {
            char d[128];
            if (!rr[i]) continue;
            describe_address(rr[i], d, sizeof d);
            if (d[0] == 'm' || d[0] == 's')   /* module-relative address */
                fprintf(stderr, "[crash] x%u=%s\n", i, d);
        }
        /* With a zero frame pointer (tail call / indirect branch) walk the
         * raw stack instead: any word that points into a module is a
         * likely return address. */
        if (!fp) {
            uint64_t sv[96] = {0};
            if (crash_read(sp, sv, sizeof sv)) {
                for (unsigned i = 0; i < 96; i += 4)
                    fprintf(stderr, "[crash] rawstack+%#x: %#llx %#llx %#llx %#llx\n",
                            i * 8, (unsigned long long)sv[i], (unsigned long long)sv[i+1],
                            (unsigned long long)sv[i+2], (unsigned long long)sv[i+3]);
                for (unsigned i = 0; i < 96; i++) {
                    char d[128];
                    if (!sv[i]) continue;
                    describe_address(sv[i], d, sizeof d);
                    if (d[0] == 'm' || d[0] == 's')
                        fprintf(stderr, "[crash] stack+%#x: %s\n", i * 8, d);
                }
                /* Recover a frame chain when the faulting x29 is zero: scan
                 * for {candidate_fp, return_address} records. */
                for (uintptr_t a = sp; a < sp + 0x400; a += 8) {
                    uint64_t pair[2] = {0};
                    if (!crash_read(a, pair, sizeof pair)) break;
                    char d[128]; describe_address(pair[1], d, sizeof d);
                    if ((d[0] == 'm' || d[0] == 's') &&
                        pair[0] >= sp && pair[0] < sp + 0x100000)
                        fprintf(stderr, "[crash] frame-scan %#llx: fp=%#llx lr=%s\n",
                                (unsigned long long)a,
                                (unsigned long long)pair[0], d);
                }
            }
        }
        {
            uint64_t q[8] = {0};
            if (crash_read(sp, q, sizeof q))
                fprintf(stderr, "[crash] sp object: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                        (unsigned long long)q[0], (unsigned long long)q[1],
                        (unsigned long long)q[2], (unsigned long long)q[3],
                        (unsigned long long)q[4], (unsigned long long)q[5],
                        (unsigned long long)q[6], (unsigned long long)q[7]);
        }
        fprintf(stderr, "[crash] regs x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx x6=%#llx x7=%#llx\n",
                (unsigned long long)rr[0], (unsigned long long)rr[1], (unsigned long long)rr[2],
                (unsigned long long)rr[3], (unsigned long long)rr[4], (unsigned long long)rr[5],
                (unsigned long long)rr[6], (unsigned long long)rr[7]);
        fprintf(stderr, "[crash] regs x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx x24=%#llx x25=%#llx x26=%#llx x27=%#llx x28=%#llx\n",
                (unsigned long long)rr[19], (unsigned long long)rr[20], (unsigned long long)rr[21],
                (unsigned long long)rr[22], (unsigned long long)rr[23], (unsigned long long)rr[24],
                (unsigned long long)rr[25], (unsigned long long)rr[26], (unsigned long long)rr[27],
                (unsigned long long)rr[28]);
        for (unsigned i = 0; i < 4; i++) {
            uint64_t obj = rr[i];
            if (i == 1 && obj && !(obj & 7)) {
                uint64_t q[16] = {0};
                if (crash_read(obj, q, sizeof q)) {
                    fprintf(stderr, "[crash] x1 object %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                            (unsigned long long)obj,
                            (unsigned long long)q[0], (unsigned long long)q[1],
                            (unsigned long long)q[2], (unsigned long long)q[3],
                            (unsigned long long)q[4], (unsigned long long)q[5],
                            (unsigned long long)q[6], (unsigned long long)q[7]);
                    uint64_t fallback = q[6];
                    if (fallback && !(fallback & 7)) {
                        uint64_t fq[16] = {0};
                        if (crash_read(fallback, fq, sizeof fq))
                            fprintf(stderr, "[crash] x1 fallback %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long)fallback,
                                    (unsigned long long)fq[0], (unsigned long long)fq[1],
                                    (unsigned long long)fq[2], (unsigned long long)fq[3],
                                    (unsigned long long)fq[4], (unsigned long long)fq[5],
                                    (unsigned long long)fq[6], (unsigned long long)fq[7]);
                    }
                    uint64_t vtable = q[0], fn = 0;
                    if (vtable && !(vtable & 7) && crash_read(vtable + 0x10, &fn, 8)) {
                        char d[128]; describe_address(fn, d, sizeof d);
                        fprintf(stderr, "[crash] x1 vtable=%#llx +0x10 -> %s\n",
                                (unsigned long long)vtable, d);
                    }
                    if (fallback && !(fallback & 7)) {
                        uint64_t sub = 0;
                        if (crash_read(fallback + 0x48, &sub, 8) && sub && !(sub & 7)) {
                            uint64_t sq[16] = {0};
                            if (crash_read(sub, sq, sizeof sq))
                                fprintf(stderr, "[crash] x1 fallback+0x48 %#llx: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                                        (unsigned long long)sub,
                                        (unsigned long long)sq[0], (unsigned long long)sq[1],
                                        (unsigned long long)sq[2], (unsigned long long)sq[3],
                                        (unsigned long long)sq[4], (unsigned long long)sq[5],
                                        (unsigned long long)sq[6], (unsigned long long)sq[7]);
                        }
                        uint64_t subv = 0, subfn = 0;
                        if (crash_read(sub, &subv, 8) && subv && !(subv & 7) &&
                            crash_read(subv + 0x38, &subfn, 8)) {
                            char d[128]; describe_address(subfn, d, sizeof d);
                            fprintf(stderr, "[crash] sub vtable=%#llx +0x38 -> %s\n",
                                    (unsigned long long)subv, d);
                        }
                        uint64_t fv = 0, ffn = 0;
                        if (crash_read(fallback, &fv, 8) && fv && !(fv & 7) &&
                            crash_read(fv + 0x10, &ffn, 8)) {
                            char d[128]; describe_address(ffn, d, sizeof d);
                            fprintf(stderr, "[crash] x1 fallback vtable=%#llx +0x10 -> %s\n",
                                    (unsigned long long)fv, d);
                        }
                    }
                }
            }
            uint64_t q[9] = {0};
            if (!obj || (obj & 7) || !crash_read(obj, q, sizeof q)) continue;
            fprintf(stderr, "[crash] [x%u]: +0=%#llx +8=%#llx +0x20=%#llx +0x38=%#llx +0x40=%#llx\n",
                    i, (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[4], (unsigned long long)q[7], (unsigned long long)q[8]);
        }
        if (rr[22]) {
            uint64_t v[12] = {0};
            if (crash_read(rr[22], v, sizeof v))
                fprintf(stderr, "[crash] [x22] +0=%#llx +8=%#llx +0x10=%#llx +0x18=%#llx +0x20=%#llx\n",
                        (unsigned long long)v[0], (unsigned long long)v[1],
                        (unsigned long long)v[2], (unsigned long long)v[3],
                        (unsigned long long)v[4]);
        }
        /* The two callee-saved pointers around the nn::gfx allocation path:
         * x20 is the argument struct, x21 the object that owns the allocation.
         * Follow one level so a null heap/allocator is visible without a
         * debugger. */
        for (unsigned i = 19; i <= 22; i++) {
            uint64_t obj = rr[i], q[32] = {0};
            if (!obj || (obj & 7) || !crash_read(obj, q, sizeof q)) continue;
            fprintf(stderr, "[crash] [x%u]=%#llx: +0=%#llx +8=%#llx +0x10=%#llx +0x18=%#llx"
                    " +0x20=%#llx +0x28=%#llx +0x30=%#llx +0x38=%#llx\n",
                    i, (unsigned long long)obj,
                    (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[2], (unsigned long long)q[3],
                    (unsigned long long)q[4], (unsigned long long)q[5],
                    (unsigned long long)q[6], (unsigned long long)q[7]);
            for (unsigned j = 0; j < 8; j++) {
                uint64_t p = q[j], v = 0;
                if (!p || (p & 7) || !crash_read(p, &v, 8)) continue;
                fprintf(stderr, "[crash]   [x%u+%#x] -> %#llx: [0]=%#llx\n", i, j * 8,
                        (unsigned long long)p, (unsigned long long)v);
            }
        }
        uint8_t *mb = runtime_module_base("main");
        if (mb) {
            static const struct { uint32_t off; const char *name; } globals[] = {
                {0x4557768, "alloc-global"}, {0x4557798, "current-thread"},
                {0x4557728, "resource-table"}, {0x4557730, "resource-2"},
                {0x4563010, "gfx-mgr"}, {0x4563018, "gfx-mgr-2"},
                {0x45586f0, "gfx-device-slot"},
                {0x4540aa8, "nvn-proc-aa8"}, {0x4540ab0, "nvn-proc-ab0"},
                {0x453efa0, "got-unlock-mutex"}, {0x453ef98, "got-lock-mutex"},
                {0x453ef88, "got-memset"}, {0x453efb8, "got-sqrtf"},
                {0x45409a0, "nvn-proc-9a0"}, {0x45409a8, "nvn-proc-9a8"},
                {0x45407a0, "nvn-proc-7a0"}, {0x45409f8, "nvn-proc-9f8"},
            };
            for (unsigned i = 0; i < sizeof globals / sizeof *globals; i++) {
                uint64_t p = 0, v = 0;
                if (!crash_read((uintptr_t)(mb + globals[i].off), &p, 8)) continue;
                if (p && !(p & 7) && crash_read(p, &v, 8)) {
                    fprintf(stderr, "[crash] %s @%#x: %#llx -> [0]=%#llx\n",
                            globals[i].name, globals[i].off,
                            (unsigned long long)p, (unsigned long long)v);
                } else {
                    fprintf(stderr, "[crash] %s @%#x: %#llx (null)\n",
                            globals[i].name, globals[i].off, (unsigned long long)p);
                }
                if (!strcmp(globals[i].name, "resource-table") && p && !(p & 7)) {
                    uint64_t t[16] = {0};
                    if (crash_read(p, t, sizeof t))
                        for (unsigned j = 0; j < 16; j += 2)
                            fprintf(stderr, "[crash]   resource-table+%#x: %#llx %#llx\n",
                                    j * 8, (unsigned long long)t[j],
                                    (unsigned long long)t[j + 1]);
                }
            }
            /* nn::gfx device/context registry: [main+0x4542f08] -> manager,
             * manager+0x18 -> list head; nodes link at +0x598 and carry the
             * thread id at +0x5e8.  This is the lookup that fails at
             * main+0x794d30. */
            {
                uint64_t mgr = 0, head = 0;
                if (crash_read((uintptr_t)(mb + 0x4542f08), &mgr, 8) && mgr && !(mgr & 7)) {
                    fprintf(stderr, "[crash] gfx-registry mgr=%#llx\n", (unsigned long long)mgr);
                    if (crash_read(mgr, &head, 8))
                        fprintf(stderr, "[crash] gfx-registry mgr[0]=%#llx\n", (unsigned long long)head);
                    {
                        uint64_t q[8] = {0};
                        if (crash_read(mgr + 0x18, q, sizeof q))
                            fprintf(stderr, "[crash] gfx-registry mgr+0x18..0x50: %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long)q[0], (unsigned long long)q[1],
                                    (unsigned long long)q[2], (unsigned long long)q[3],
                                    (unsigned long long)q[4], (unsigned long long)q[5],
                                    (unsigned long long)q[6], (unsigned long long)q[7]);
                    }
                    if (crash_read(mgr + 0x18, &head, 8) && head && !(head & 7)) {
                        uint64_t hv[4] = {0};
                        if (crash_read(head, hv, sizeof hv))
                            fprintf(stderr, "[crash] gfx-registry head=%#llx: %#llx %#llx %#llx %#llx\n",
                                    (unsigned long long)head, (unsigned long long)hv[0],
                                    (unsigned long long)hv[1], (unsigned long long)hv[2],
                                    (unsigned long long)hv[3]);
                        uint64_t node = hv[0];
                        if (crash_read(head, &node, 8)) {
                            for (unsigned n = 0; n < 8 && node && !(node & 7); n++) {
                                uint64_t key = 0, next = 0;
                                crash_read(node + 0x5e8, &key, 8);
                                crash_read(node + 0x598, &next, 8);
                                fprintf(stderr, "[crash] gfx-registry node[%u]=%#llx key+0x5e8=%#llx next=%#llx\n",
                                        n, (unsigned long long)node, (unsigned long long)key,
                                        (unsigned long long)next);
                                node = next;
                            }
                        }
                    }
                }
            }
            uint8_t *sb = runtime_module_base("sdk");
            if (sb) {
                static const uint32_t sdk_slots[] = {0xd0308, 0xd0310, 0xd0318, 0xd0320, 0xd0328, 0xd0330,
                                                     0xddc000, 0xddc008, 0xddc010, 0xddc018, 0xddc020, 0xddc028};
                for (unsigned i = 0; i < sizeof sdk_slots / sizeof *sdk_slots; i++) {
                    uint64_t v = 0;
                    if (crash_read((uintptr_t)(sb + sdk_slots[i]), &v, 8))
                        fprintf(stderr, "[crash] sdk-got+%#x: %#llx\n",
                                sdk_slots[i], (unsigned long long)v);
                }
            }
            uint64_t g[16] = {0};
            if (crash_read((uintptr_t)(mb + 0x463c000), g, sizeof g))
                for (unsigned i = 0; i < 16; i += 2)
                    fprintf(stderr, "[crash] sead+%#x: %#llx %#llx\n", i * 8,
                            (unsigned long long)g[i], (unsigned long long)g[i + 1]);
        }
    }

    /* Guest frame-pointer chain: at x29 the pair is {caller fp, return address}. */
    uint64_t chain_fp = fp;
    uint64_t caller_heap = 0;
    if (chain_fp && !(chain_fp & 7)) {
        for (unsigned n = 0; n < 16 && chain_fp; n++) {
            uint64_t pair[2];
            if (!crash_read(chain_fp, pair, sizeof pair)) break;
            char fd[128];
            describe_address(pair[1], fd, sizeof fd);
            fprintf(stderr, "[crash] frame %u: fp=%#llx lr=%s\n",
                    n, (unsigned long long)chain_fp, fd);
            if (getenv("TOTK_ANDROID_CRASH_DUMP") && n == 2) {
                /* 0x811784: x29 = sp+0x10, saves x23 (the sead heap the
                 * allocation uses) at fp+0x38; the size requested from that
                 * heap is kept at fp-8. */
                uint64_t v[8] = {0};
                if (crash_read(chain_fp - 8, v, 8))
                    fprintf(stderr, "[crash] 0x811784 requested heap size=%#llx\n",
                            (unsigned long long)v[0]);
                if (crash_read(chain_fp + 0x38, v, 8)) {
                    fprintf(stderr, "[crash] 0x811784 x23=%#llx\n", (unsigned long long)v[0]);
                    caller_heap = v[0];
                }
            }
            if (getenv("TOTK_ANDROID_CRASH_DUMP") && n == 3) {
                /* 0x810c68: x29 = sp+0x10.  sp+0x108/0x110 is the per-resource
                 * required-size vector; sp+0x118/0x120 is the object vector. */
                uint64_t base = chain_fp - 0xa9930, v[4] = {0};
                uint32_t count = 0, ocount = 0;
                uint64_t data = 0, odata = 0;
                if (crash_read(base + 0x108, &count, 4)) { }
                if (crash_read(base + 0x110, &data, 8)) { }
                if (crash_read(base + 0x118, &ocount, 4)) { }
                if (crash_read(base + 0x120, &odata, 8)) { }
                fprintf(stderr, "[crash] 0x810c68 size-vector count=%u data=%#llx"
                        " object-vector count=%u data=%#llx\n",
                        count, (unsigned long long)data, ocount, (unsigned long long)odata);
                for (uint32_t i = 0; i < count && i < 8 && data; i++) {
                    uint64_t s = 0, o = 0;
                    if (!crash_read(data + i * 8, &s, 8)) break;
                    if (odata) crash_read(odata + i * 8, &o, 8);
                    fprintf(stderr, "[crash] 0x810c68 resource[%u]: size=%#llx object=%#llx\n",
                            i, (unsigned long long)s, (unsigned long long)o);
                }
                if (crash_read(base + 0x98, v, 8))
                    fprintf(stderr, "[crash] 0x810c68 x21/resource=%#llx\n",
                            (unsigned long long)v[0]);
            }
            uint64_t next = pair[0];
            if (!next || (next & 7) || next <= chain_fp || next - chain_fp > (64u << 20)) break;
            chain_fp = next;
        }
    } else {
        char fd[128];
        describe_address(lr, fd, sizeof fd);
        fprintf(stderr, "[crash] frame 0: fp=%#llx lr=%s\n", (unsigned long long)fp, fd);
    }

    if (getenv("TOTK_ANDROID_CRASH_DUMP") && caller_heap && !(caller_heap & 7)) {
        uint64_t v[32] = {0};
        if (crash_read(caller_heap, v, sizeof v))
            for (unsigned i = 0; i < 32; i += 2)
                fprintf(stderr, "[crash] heap %#llx+%#x: %#llx %#llx\n",
                        (unsigned long long)caller_heap, i * 8,
                        (unsigned long long)v[i], (unsigned long long)v[i + 1]);
    }

    char hle[256];
    if (runtime_hle_last_call(hle, sizeof hle)) fprintf(stderr, "[crash] last HLE: %s\n", hle);
    else fprintf(stderr, "[crash] last HLE: none\n");
    fprintf(stderr, "[crash] last NVN api: %s (last proc lookup: %s)\n",
            runtime_nvn_last_api(), runtime_nvn_last_proc());

    RuntimeIpcRecord ipc;
    if (runtime_ipc_last(&ipc) && ipc.valid)
        fprintf(stderr, "[crash] last IPC: service=%s command=%u object=%u type=%u data=%u result=%u\n",
                ipc.service, ipc.command, ipc.object, ipc.type, ipc.data_size, ipc.result);
    else fprintf(stderr, "[crash] last IPC: none\n");

    fprintf(stderr, "[crash] guest thread handle=%#x\n", runtime_thread_current_handle());
    runtime_hle_trace_report();   /* no allocation/stdio locking beyond this handler */
    _exit(128 + sig);
}
