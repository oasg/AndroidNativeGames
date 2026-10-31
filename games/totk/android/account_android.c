/* Minimal nn::account (acc:u0) service for the Android host.
 *
 * One local user exists.  The UID is all zeroes for now so it matches the save
 * data probe; nickname, language and region are supplied by nn::oe/settings
 * overrides.  Command ids are the IAccountServiceForApplication protocol. */
#include "runtime.h"
#include "ipc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t local_uid[16];
/* Test hook: TOTK_ANDROID_ACCOUNT_UID=<32 hex chars> makes the host account
 * report the same nn::account::Uid as the save-data export.  The UID is a
 * 128-bit value printed high word first by fs_host_android. */
static void account_uid_init(void) {
    const char *e = getenv("TOTK_ANDROID_ACCOUNT_UID");
    if (!e || strlen(e) != 32) return;
    uint8_t raw[16];
    for (unsigned i = 0; i < 16; i++) {
        unsigned v = 0;
        for (unsigned k = 0; k < 2; k++) {
            char c = e[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return;
        }
        raw[i] = (uint8_t)v;
    }
    /* fs_host prints uid[1] then uid[0], so store the low/high 64-bit halves
     * little-endian. */
    for (unsigned i = 0; i < 8; i++) {
        local_uid[i] = raw[15 - i];
        local_uid[8 + i] = raw[7 - i];
    }
    fprintf(stderr, "[totk-account] uid=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
            local_uid[15], local_uid[14], local_uid[13], local_uid[12],
            local_uid[11], local_uid[10], local_uid[9], local_uid[8],
            local_uid[7], local_uid[6], local_uid[5], local_uid[4],
            local_uid[3], local_uid[2], local_uid[1], local_uid[0]);
}

/* Consumed by fs_host_android.c to choose save/<uid>.  Keep the byte order
 * identical to the fs path format: uid[0] is the low 64-bit half, uid[1] the
 * high half, and fs prints uid[1] first. */
void runtime_account_uid(uint64_t out[2]) {
    if (!out) return;
    memcpy(&out[0], local_uid, 8);
    memcpy(&out[1], local_uid + 8, 8);
}

static unsigned account_logged;
static void account_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    if (account_logged++ < 80)
        fprintf(stderr, "[totk-account] command %u data=%u bytes\n", rq->cmd, rq->data_size);
    switch (rq->cmd) {
    case 0:                                     /* Initialize(ProcessId, ...) */
        return;
    case 1: {                                   /* GetUserExistence(Uid) -> bool */
        uint8_t exists = 1;
        IPC_OUT(rs, exists);
        return;
    }
    case 2:                                     /* ListAllUsers -> count, Uid[] */
    case 3: {                                   /* ListOpenUsers -> count, Uid[] */
        int32_t count = 1;
        IPC_OUT(rs, count);
        ipc_out(rs, local_uid, sizeof local_uid);
        return;
    }
    case 4:                                     /* GetLastOpenedUser -> Uid */
        ipc_out(rs, local_uid, sizeof local_uid);
        return;
    case 150: {                                 /* 1-byte status query */
        uint8_t yes = 1;
        IPC_OUT(rs, yes);
        return;
    }
    case 160:                                   /* 8-byte input, acknowledgement only */
        return;
    default:
        /* Do not report success for a command whose reply shape we do not
         * know; the trace then names the next gap. */
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
        return;
    }
}

__attribute__((constructor)) static void register_account(void) {
    account_uid_init();
    runtime_ipc_register("acc:u0", account_handler, NULL, NULL);
    runtime_ipc_register("acc:u1", account_handler, NULL, NULL);
    runtime_ipc_register("acc:u2", account_handler, NULL, NULL);
}
