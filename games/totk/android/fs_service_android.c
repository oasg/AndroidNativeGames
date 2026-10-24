/* Host IFileSystemProxy ("fsp-srv") for the Android host.
 *
 * The first milestone implements the process/access-log commands used while
 * nn::fs starts, plus a host-file IStorage behind
 * OpenDataStorageByCurrentProcess.  File-system objects are added command by
 * command as the startup trace reaches them; unknown commands fail loudly
 * instead of pretending that a missing directory or file exists. */
#include "runtime.h"
#include "ipc.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static uint64_t current_process;

typedef struct {
    int fd;
    uint64_t size;
} Storage;

static Storage *storage_open(void) {
    const char *path = getenv("TOTK_ANDROID_STORAGE");
    if (!path || !*path) return NULL;
    Storage *st = calloc(1, sizeof *st);
    if (!st) runtime_fail("fsp storage allocation");
    st->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (st->fd < 0) { free(st); return NULL; }
    struct stat sb;
    if (fstat(st->fd, &sb) != 0 || sb.st_size < 0) { close(st->fd); free(st); return NULL; }
    st->size = (uint64_t)sb.st_size;
    return st;
}

static void storage_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    Storage *st = s->state;
    switch (rq->cmd) {
    case 0: { /* Read(offset, size, buffer) */
        if (rq->data_size < 16) { rs->result = 0xe401; return; }
        uint64_t offset, size;
        memcpy(&offset, rq->data, 8);
        memcpy(&size, rq->data + 8, 8);
        uint64_t read = offset < st->size ? st->size - offset : 0;
        if (read > size) read = size;
        if (read && rq->nb && rq->b[0].size) {
            if (read > rq->b[0].size) read = rq->b[0].size;
            void *dst = ipc_guest(rq->b[0].addr, read);
            ssize_t got = pread(st->fd, dst, (size_t)read, (off_t)offset);
            if (got < 0) { rs->result = 0xe401; return; }
            read = (uint64_t)got;
        }
        IPC_OUT(rs, read);
        return;
    }
    case 4: { /* GetSize */
        IPC_OUT(rs, st->size);
        return;
    }
    default:
        fprintf(stderr, "[totk-ipc] IStorage command %u not implemented\n", rq->cmd);
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
    }
}

static void storage_close(void *p) {
    Storage *st = p;
    close(st->fd);
    free(st);
}

static void fsp_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: { /* SetCurrentProcess(ProcessId) */
        if (rq->data_size < 8) { rs->result = 0xe401; return; }
        memcpy(&current_process, rq->data, 8);
        fprintf(stderr, "[totk-ipc] fsp-srv current process %#llx\n",
                (unsigned long long)current_process);
        return;
    }
    case 1004: { /* SetGlobalAccessLogMode(u32) */
        if (rq->data_size < 4) { rs->result = 0xe401; return; }
        uint32_t mode; memcpy(&mode, rq->data, 4);
        fprintf(stderr, "[totk-ipc] fsp-srv access log mode %u\n", mode);
        return;
    }
    case 1005: { /* GetGlobalAccessLogMode -> u32 */
        uint32_t mode = 0; IPC_OUT(rs, mode); return;
    }
    case 200: { /* OpenDataStorageByCurrentProcess -> IStorage */
        Storage *st = storage_open();
        if (!st) {
            fprintf(stderr, "[totk-ipc] fsp-srv: TOTK_ANDROID_STORAGE is not set or unreadable\n");
            rs->result = 0xe401;
            return;
        }
        uint32_t id = ipc_domain_add_object(s, "IStorage", storage_handler, st, storage_close);
        IPC_OUT_OBJECT(rs, id);
        return;
    }
    default:
        fprintf(stderr, "[totk-ipc] fsp-srv command %u not implemented (data %u bytes, x%u a%u b%u w%u c%u)\n",
                rq->cmd, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc);
        (void)s;
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
    }
}

__attribute__((constructor)) static void register_fsp(void) {
    runtime_ipc_register("fsp-srv", fsp_handler, NULL, NULL);
}
