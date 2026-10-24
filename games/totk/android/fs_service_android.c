/* Host IFileSystemProxy ("fsp-srv") for the Android host.
 *
 * The service keeps the Horizon CMIF protocol but maps the file systems to a
 * host directory: TOTK_ANDROID_FSROOT (default /data/local/tmp/totk-android/fsroot).
 * IStorage is backed by TOTK_ANDROID_STORAGE when set. */
#include "runtime.h"
#include "ipc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DIR_ENTRY_SIZE 0x310
#define FS_RESULT_BAD_PATH 0xe401u
#define FS_RESULT_NOT_FOUND 0x202u

static uint64_t current_process;

/* ---------- host paths ---------- */
static const char *host_root(void) {
    const char *root = getenv("TOTK_ANDROID_FSROOT");
    return root && *root ? root : "/data/local/tmp/totk-android/fsroot";
}

static int request_path(const IpcRequest *rq, char *out, size_t cap) {
    const IpcBuffer *b = NULL;
    if (rq->na) b = &rq->a[0];
    else if (rq->nw) b = &rq->w[0];
    else if (rq->nx) b = &rq->x[0];
    if (!b || !b->addr || !b->size) return -1;
    size_t n = b->size < cap - 1 ? b->size : cap - 1;
    memcpy(out, ipc_guest(b->addr, b->size), n);
    out[n] = 0;
    return 0;
}

static int join_path(const char *root, const char *path, char *out, size_t cap) {
    while (*path == '/') path++;
    const char *slash = strchr(path, '/');
    const char *colon = strchr(path, ':');
    if (colon && (!slash || colon < slash)) {
        path = colon + 1;
        while (*path == '/') path++;
    }
    if (strstr(path, "..")) return -1;
    int n = snprintf(out, cap, "%s/%s", root, path);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int resolve_path(const IpcRequest *rq, char *out, size_t cap) {
    char raw[0x400], path[PATH_MAX];
    if (request_path(rq, raw, sizeof raw)) return -1;
    if (join_path(host_root(), raw, path, sizeof path)) return -1;
    snprintf(out, cap, "%s", path);
    return 0;
}

/* ---------- IStorage ---------- */
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
        if (rq->data_size < 16) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t offset, size;
        memcpy(&offset, rq->data, 8);
        memcpy(&size, rq->data + 8, 8);
        uint64_t read = offset < st->size ? st->size - offset : 0;
        if (read > size) read = size;
        if (read && rq->nb && rq->b[0].size) {
            if (read > rq->b[0].size) read = rq->b[0].size;
            void *dst = ipc_guest(rq->b[0].addr, read);
            ssize_t got = pread(st->fd, dst, (size_t)read, (off_t)offset);
            if (got < 0) { rs->result = FS_RESULT_BAD_PATH; return; }
            read = (uint64_t)got;
        }
        IPC_OUT(rs, read);
        return;
    }
    case 4:
        IPC_OUT(rs, st->size);
        return;
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

/* ---------- IFileSystem ---------- */
typedef struct {
    char root[PATH_MAX];
} FsRoot;

typedef struct {
    int fd;
} FsFile;

typedef struct {
    char **names;
    unsigned char *isdir;
    uint64_t *sizes;
    unsigned count, index, filter;
} FsDir;

static int open_mode(int mode) {
    if ((mode & 2) && (mode & 1)) return O_RDWR;
    if (mode & 2) return O_WRONLY;
    return O_RDONLY;
}

static int remove_tree(const char *path, int keep_root) {
    struct stat sb;
    if (lstat(path, &sb) != 0) return -1;
    if (!S_ISDIR(sb.st_mode) || S_ISLNK(sb.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
        remove_tree(child, 0);
    }
    closedir(d);
    return keep_root ? 0 : rmdir(path);
}

static void file_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    FsFile *f = s->state;
    switch (rq->cmd) {
    case 0: { /* Read(offset, size, buffer) */
        if (rq->data_size < 16) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t offset, size;
        memcpy(&offset, rq->data, 8);
        memcpy(&size, rq->data + 8, 8);
        uint64_t read = 0;
        if (rq->nb && rq->b[0].size) {
            if (size > rq->b[0].size) size = rq->b[0].size;
            void *dst = ipc_guest(rq->b[0].addr, size);
            ssize_t got = pread(f->fd, dst, (size_t)size, (off_t)offset);
            if (got < 0) { rs->result = FS_RESULT_BAD_PATH; return; }
            read = (uint64_t)got;
        }
        IPC_OUT(rs, read);
        return;
    }
    case 1: { /* Write(offset, size, buffer) */
        if (rq->data_size < 16 || !rq->na) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t offset, size;
        memcpy(&offset, rq->data, 8);
        memcpy(&size, rq->data + 8, 8);
        if (size > rq->a[0].size) size = rq->a[0].size;
        void *src = ipc_guest(rq->a[0].addr, size);
        ssize_t put = pwrite(f->fd, src, (size_t)size, (off_t)offset);
        if (put < 0) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t written = (uint64_t)put;
        IPC_OUT(rs, written);
        return;
    }
    case 2: /* Flush */
        if (fsync(f->fd) != 0) { rs->result = FS_RESULT_BAD_PATH; return; }
        return;
    case 3: { /* SetSize */
        if (rq->data_size < 8) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t size; memcpy(&size, rq->data, 8);
        if (ftruncate(f->fd, (off_t)size) != 0) { rs->result = FS_RESULT_BAD_PATH; return; }
        return;
    }
    case 4: { /* GetSize */
        struct stat sb;
        if (fstat(f->fd, &sb) != 0) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t size = (uint64_t)sb.st_size;
        IPC_OUT(rs, size);
        return;
    }
    default:
        fprintf(stderr, "[totk-ipc] IFile command %u not implemented\n", rq->cmd);
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
    }
}

static void file_close(void *p) {
    FsFile *f = p;
    close(f->fd);
    free(f);
}

static FsDir *dir_open(const char *path, unsigned filter) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    FsDir *dir = calloc(1, sizeof *dir);
    if (!dir) { closedir(d); runtime_fail("fsp directory allocation"); }
    dir->filter = filter;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) continue;
        struct stat sb;
        if (stat(child, &sb) != 0) continue;
        int isdir = S_ISDIR(sb.st_mode);
        if (filter && !(filter & (isdir ? 1u : 2u))) continue;
        char **names = realloc(dir->names, (dir->count + 1) * sizeof *names);
        unsigned char *dirs = realloc(dir->isdir, dir->count + 1);
        uint64_t *sizes = realloc(dir->sizes, (dir->count + 1) * sizeof *sizes);
        if (!names || !dirs || !sizes) runtime_fail("fsp directory entry allocation");
        dir->names = names; dir->isdir = dirs; dir->sizes = sizes;
        dir->names[dir->count] = strdup(e->d_name);
        if (!dir->names[dir->count]) runtime_fail("fsp directory name allocation");
        dir->isdir[dir->count] = (unsigned char)isdir;
        dir->sizes[dir->count] = isdir ? 0 : (uint64_t)sb.st_size;
        dir->count++;
    }
    closedir(d);
    return dir;
}

static void directory_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    FsDir *dir = s->state;
    switch (rq->cmd) {
    case 0: { /* Read: output DirectoryEntry array */
        uint64_t count = 0;
        if (rq->nb && rq->b[0].size) {
            unsigned max = (unsigned)(rq->b[0].size / DIR_ENTRY_SIZE);
            void *dst = ipc_guest(rq->b[0].addr, rq->b[0].size);
            while (count < max && dir->index < dir->count) {
                uint8_t *entry = (uint8_t *)dst + count * DIR_ENTRY_SIZE;
                memset(entry, 0, DIR_ENTRY_SIZE);
                strncpy((char *)entry, dir->names[dir->index], 0x300);
                entry[0x304] = dir->isdir[dir->index] ? 0 : 1;
                memcpy(entry + 0x308, &dir->sizes[dir->index], 8);
                dir->index++;
                count++;
            }
        }
        IPC_OUT(rs, count);
        return;
    }
    case 1: { /* GetEntryCount */
        uint64_t count = dir->count;
        IPC_OUT(rs, count);
        return;
    }
    default:
        fprintf(stderr, "[totk-ipc] IDirectory command %u not implemented\n", rq->cmd);
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
    }
}

static void directory_close(void *p) {
    FsDir *dir = p;
    for (unsigned i = 0; i < dir->count; i++) free(dir->names[i]);
    free(dir->names); free(dir->isdir); free(dir->sizes); free(dir);
}

static void fsystem_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    FsRoot *fs = s->state;
    char raw[0x400], path[PATH_MAX];
    switch (rq->cmd) {
    case 0: { /* CreateFile(path, size, option) */
        if (request_path(rq, raw, sizeof raw) || join_path(fs->root, raw, path, sizeof path)) {
            rs->result = FS_RESULT_BAD_PATH; return;
        }
        int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (fd < 0) { rs->result = FS_RESULT_NOT_FOUND; return; }
        if (rq->data_size >= 8) {
            uint64_t size; memcpy(&size, rq->data, 8);
            if (ftruncate(fd, (off_t)size) != 0) { close(fd); rs->result = FS_RESULT_BAD_PATH; return; }
        }
        close(fd);
        return;
    }
    case 1: { /* DeleteFile */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        if (unlink(path) != 0) rs->result = FS_RESULT_NOT_FOUND;
        return;
    }
    case 2: { /* CreateDirectory */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        if (mkdir(path, 0777) != 0) rs->result = FS_RESULT_BAD_PATH;
        return;
    }
    case 3: { /* DeleteDirectory */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        if (rmdir(path) != 0) rs->result = FS_RESULT_NOT_FOUND;
        return;
    }
    case 4: { /* DeleteDirectoryRecursively */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        if (remove_tree(path, 0) != 0) rs->result = FS_RESULT_NOT_FOUND;
        return;
    }
    case 5: case 6: { /* RenameFile / RenameDirectory(old, new) */
        char raw2[0x400], path2[PATH_MAX];
        const IpcBuffer *save = rq->na > 1 ? &rq->a[1] : NULL;
        if (!save || request_path(rq, raw, sizeof raw) || join_path(fs->root, raw, path, sizeof path)) {
            rs->result = FS_RESULT_BAD_PATH; return;
        }
        size_t n = save->size < sizeof raw2 - 1 ? save->size : sizeof raw2 - 1;
        memcpy(raw2, ipc_guest(save->addr, save->size), n); raw2[n] = 0;
        if (join_path(fs->root, raw2, path2, sizeof path2)) { rs->result = FS_RESULT_BAD_PATH; return; }
        if (rename(path, path2) != 0) rs->result = FS_RESULT_NOT_FOUND;
        return;
    }
    case 7: { /* GetEntryType */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        struct stat sb;
        if (stat(path, &sb) != 0) { rs->result = FS_RESULT_NOT_FOUND; return; }
        uint32_t type = S_ISDIR(sb.st_mode) ? 0 : 1;
        IPC_OUT(rs, type);
        return;
    }
    case 8: { /* OpenFile(path, mode) */
        if (request_path(rq, raw, sizeof raw) || join_path(fs->root, raw, path, sizeof path)) {
            rs->result = FS_RESULT_BAD_PATH; return;
        }
        int mode = 0;
        if (rq->data_size >= 4) memcpy(&mode, rq->data, 4);
        int fd = open(path, open_mode(mode) | O_CLOEXEC);
        if (fd < 0) { rs->result = FS_RESULT_NOT_FOUND; return; }
        FsFile *f = calloc(1, sizeof *f);
        if (!f) { close(fd); runtime_fail("fsp file allocation"); }
        f->fd = fd;
        uint32_t id = ipc_domain_add_object(s, "IFile", file_handler, f, file_close);
        IPC_OUT_OBJECT(rs, id);
        return;
    }
    case 9: { /* OpenDirectory(path, filter_flags) */
        if (request_path(rq, raw, sizeof raw) || join_path(fs->root, raw, path, sizeof path)) {
            rs->result = FS_RESULT_BAD_PATH; return;
        }
        unsigned filter = 0;
        if (rq->data_size >= 8) { uint64_t v; memcpy(&v, rq->data, 8); filter = (unsigned)v; }
        FsDir *dir = dir_open(path, filter);
        if (!dir) { rs->result = FS_RESULT_NOT_FOUND; return; }
        uint32_t id = ipc_domain_add_object(s, "IDirectory", directory_handler, dir, directory_close);
        IPC_OUT_OBJECT(rs, id);
        return;
    }
    case 10: /* Commit */
        return;
    case 11: case 12: { /* GetFreeSpaceSize / GetTotalSpaceSize */
        struct statvfs sv;
        if (statvfs(fs->root, &sv) != 0) { rs->result = FS_RESULT_BAD_PATH; return; }
        uint64_t value = rq->cmd == 11 ? (uint64_t)sv.f_bavail * sv.f_frsize
                                       : (uint64_t)sv.f_blocks * sv.f_frsize;
        IPC_OUT(rs, value);
        return;
    }
    case 13: { /* CleanDirectoryRecursively */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        if (remove_tree(path, 1) != 0) rs->result = FS_RESULT_NOT_FOUND;
        return;
    }
    case 14: { /* GetFileTimeStampRaw -> 3 u64 + u8 */
        if (resolve_path(rq, path, sizeof path)) { rs->result = FS_RESULT_BAD_PATH; return; }
        struct stat sb;
        if (stat(path, &sb) != 0) { rs->result = FS_RESULT_NOT_FOUND; return; }
        uint8_t out[0x20] = {0};
        uint64_t created = (uint64_t)sb.st_ctim.tv_sec;
        uint64_t modified = (uint64_t)sb.st_mtim.tv_sec;
        uint64_t accessed = (uint64_t)sb.st_atim.tv_sec;
        memcpy(out, &created, 8); memcpy(out + 8, &modified, 8); memcpy(out + 16, &accessed, 8);
        ipc_out(rs, out, sizeof out);
        return;
    }
    default:
        fprintf(stderr, "[totk-ipc] IFileSystem command %u not implemented\n", rq->cmd);
        rs->result = IPC_RESULT_UNKNOWN_COMMAND;
    }
}

static void fsystem_close(void *p) { free(p); }

static void return_fs(IpcSession *s, IpcResponse *rs, const char *root) {
    FsRoot *fs = calloc(1, sizeof *fs);
    if (!fs) runtime_fail("fsp file system allocation");
    snprintf(fs->root, sizeof fs->root, "%s", root);
    uint32_t id = ipc_domain_add_object(s, "IFileSystem", fsystem_handler, fs, fsystem_close);
    IPC_OUT_OBJECT(rs, id);
}

/* ---------- fsp-srv ---------- */
static void fsp_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: { /* SetCurrentProcess(ProcessId) */
        if (rq->data_size < 8) { rs->result = FS_RESULT_BAD_PATH; return; }
        memcpy(&current_process, rq->data, 8);
        fprintf(stderr, "[totk-ipc] fsp-srv current process %#llx\n",
                (unsigned long long)current_process);
        return;
    }
    case 0: case 7: case 8: case 10: case 11: case 17: case 18:
    case 51: case 52: case 53: {
        char path[PATH_MAX];
        const char *root = host_root();
        if (rq->cmd == 17 && !request_path(rq, path, sizeof path) && path[0] == '/') root = path;
        return_fs(s, rs, root);
        return;
    }
    case 1004: { /* SetGlobalAccessLogMode(u32) */
        if (rq->data_size < 4) { rs->result = FS_RESULT_BAD_PATH; return; }
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
            rs->result = FS_RESULT_NOT_FOUND;
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
