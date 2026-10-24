/* Read-only RomFS for the Android host, backed by a directory supplied with
 * TOTK_ANDROID_ROMFS (for example an extracted RomFS folder).  This is the
 * Android counterpart of runtime/romfs.c: it keeps the same nn::fs host
 * interface but skips the macOS IVFC image and overlay verification. */
#include "runtime.h"
#include "fs_host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 256
#define HANDLE_BASE 0x100000000ULL

typedef struct { int fd; uint64_t size; } Entry;
static Entry entries[NH];

static const char *romfs_root(void) {
    const char *root = getenv("TOTK_ANDROID_ROMFS");
    return root && *root ? root : NULL;
}
static const char *romfs_mount(void) {
    const char *name = getenv("TOTK_ANDROID_ROMFS_MOUNT");
    return name && *name ? name : "rom";
}
int runtime_romfs_owns(const char *path) {
    if (!romfs_root() || !path) return 0;
    size_t n = strlen(romfs_mount());
    return !strncmp(path, romfs_mount(), n) && path[n] == ':';
}
int runtime_romfs_handle(uint64_t h) {
    return h >= HANDLE_BASE && h < HANDLE_BASE + NH;
}

static unsigned resolve(const char *path, char *out, size_t cap) {
    const char *root = romfs_root();
    if (!root) return NOT_FOUND;
    if (!runtime_romfs_owns(path)) return INVALID;
    size_t n = strlen(romfs_mount());
    const char *rest = path + n;
    if (rest[0] != ':' || rest[1] != '/') return INVALID;
    rest += 2;
    /* Reject parent traversal; the RomFS is a sandbox root. */
    for (const char *p = rest; *p;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return INVALID;
        if (memchr(p, '\\', len)) return INVALID;
        p = e ? e + 1 : p + len;
    }
    size_t rn = strlen(root);
    while (rn && root[rn - 1] == '/') rn--;
    int k = snprintf(out, cap, "%.*s/%s", (int)rn, root, rest);
    return k < 0 || (size_t)k >= cap ? INVALID : 0;
}

unsigned runtime_romfs_open(uint64_t *out, const char *path, int mode) {
    if (!out || (mode & ~1)) return UNSUPPORTED;   /* the Android RomFS is read-only */
    char p[2048];
    unsigned r = resolve(path, p, sizeof p);
    if (r) return r;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? NOT_FOUND : INVALID;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NOT_FOUND; }
    for (unsigned i = 0; i < NH; i++) if (entries[i].fd <= 0) {
        entries[i].fd = fd;
        entries[i].size = (uint64_t)st.st_size;
        *out = HANDLE_BASE + i;
        return 0;
    }
    close(fd);
    runtime_fail("RomFS file handle table full");
}

unsigned runtime_romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (!out || !runtime_romfs_handle(h) || offset < 0 || (!buffer && size)) return INVALID;
    Entry *e = &entries[h - HANDLE_BASE];
    if (e->fd <= 0) return INVALID;
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(e->fd, (uint8_t *)buffer + done, (size_t)(size - done), (off_t)(offset + (int64_t)done));
        if (n < 0) return INVALID;
        if (!n) break;
        done += (uint64_t)n;
    }
    *out = done;
    return 0;
}
unsigned runtime_romfs_read_options(uint64_t *out, uint64_t h, int64_t off, void *buf, uint64_t size) {
    return runtime_romfs_read(out, h, off, buf, size);
}
unsigned runtime_romfs_size(int64_t *out, uint64_t h) {
    if (!out || !runtime_romfs_handle(h) || entries[h - HANDLE_BASE].fd <= 0) return INVALID;
    *out = (int64_t)entries[h - HANDLE_BASE].size;
    return 0;
}
void runtime_romfs_close(uint64_t h) {
    if (!runtime_romfs_handle(h)) runtime_fail("close invalid RomFS handle");
    Entry *e = &entries[h - HANDLE_BASE];
    if (e->fd > 0) close(e->fd);
    e->fd = 0;
    e->size = 0;
}
unsigned runtime_romfs_entry_type(int *out, const char *path) {
    if (!out) return INVALID;
    char p[2048];
    unsigned r = resolve(path, p, sizeof p);
    if (r) return r;
    struct stat st;
    if (stat(p, &st)) return NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? 0 : 1;
    return 0;
}
