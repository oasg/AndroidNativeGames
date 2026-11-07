/* Module: inits. */
#include "loader_internal.h"

unsigned hash_name(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

void make_hash(Module *m) {
    unsigned cap = 16;
    while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof(unsigned));
    need(m->hash != NULL, "symbol hash allocation");
    m->hash_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) if (m->symbols[i].defined) {
        const char *s = m->symbols[i].name;
        unsigned p = hash_name(s) & m->hash_mask;
        while (m->hash[p] && strcmp(s, m->symbols[m->hash[p]].name)) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}

void *module_symbol(Module *m, const char *name) {
    if (!m->hash) return NULL;
    unsigned p = hash_name(name) & m->hash_mask;
    while (m->hash[p]) {
        const Symbol *s = &m->symbols[m->hash[p]];
        if (!strcmp(name, s->name)) return s->value < m->key_off ? m->base + s->value : NULL;
        p = (p + 1) & m->hash_mask;
    }
    return NULL;
}

void map_module(Module *m, const char *dir) {
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    need(n > 0 && (size_t)n < sizeof path, "module path too long");
    fprintf(stderr, "[totk-android] map %s from %s\n", m->name, path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* The APK shell packs the three images as native libraries so the
         * loader can map them PROT_EXEC from the app's lib dir.  External
         * storage (FUSE) denies executable mappings, so this fallback is the
         * only writable-free path for an installed app. */
        n = snprintf(path, sizeof path, "%s/lib%s_patched.so", dir, m->name);
        need(n > 0 && (size_t)n < sizeof path, "module path too long");
        fprintf(stderr, "[totk-android] map %s from native lib %s\n", m->name, path);
        fd = open(path, O_RDONLY);
    }
    need(fd >= 0, "open patched module");
    struct stat st;
    need(fstat(fd, &st) == 0 && (size_t)st.st_size == m->file_size, "patched module size");
    /* Map the whole image executable first; data/key page are made writable
     * below.  Mapping it writable first then adding PROT_EXEC is denied by
     * Android's execmod policy (verified on an arm64 device). */
    void *base = mmap(NULL, m->file_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    close(fd);
    need(base != MAP_FAILED, "map patched module");
    fprintf(stderr, "[totk-android] mmap %s -> %p\n", m->name, base);
    m->base = base;
    m->size = page_up(m->file_size);
    char hex[65];
    sha256_hex(base, m->file_size, hex);
    fprintf(stderr, "[totk-android] sha %s -> %s\n", m->name, hex);
    need(!strcmp(hex, m->digest), "image SHA256 differs from verified rewrite");
    need(m->key_off % page_size() == 0 && m->key_off + 0x4000 <= m->patch_off &&
         m->patch_off <= m->file_size, "key/patch layout");
    /* The image was mapped executable first.  Make only the data and key
     * page writable; text and the patch area stay R-X. */
    if (mprotect(m->base + m->data_off, m->key_off - m->data_off, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "[totk-android] data RW %s: %s\n", m->name, strerror(errno));
        runtime_fail("data RW");
    }
    if (mprotect(m->base + m->key_off, 0x4000, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "[totk-android] key RW %s: %s\n", m->name, strerror(errno));
        runtime_fail("key page RW");
    }
    uint64_t *key = (void *)(m->base + m->key_off);
    for (int i = 0; i < 8; i++) key[i] = 0;
    key[0] = runtime_guest_slots_off;       /* KCTX */
    key[3] = (uintptr_t)&runtime_gate;      /* KGATE */
    make_hash(m);
    fprintf(stderr, "[totk-android] mapped %s at %p size=%zu symbols=%u gates=%u\n",
            m->name, m->base, m->file_size, m->nsym, m->ngate);
}

void relocate(Module *m) {
    for (unsigned i = 0; i < m->nreloc; i++) {
        const Reloc *r = &m->relocs[i];
        need(r->where >= m->data_off && r->where + 8 <= m->key_off && r->symbol < m->nsym,
             "relocation bounds");
        uintptr_t v;
        if (r->kind == 1027) {
            need(r->addend >= 0 && (uint64_t)r->addend < m->key_off, "relative addend");
            v = (uintptr_t)m->base + r->addend;
            m->relative++;
        } else {
            need(r->kind == 257 || r->kind == 1025 || r->kind == 1026, "relocation kind");
            const Symbol *s = &m->symbols[r->symbol];
            void *p = (r->kind == 1026 || !strcmp(s->name, "nnMain")) ? runtime_observer(s->name) : NULL;
            if (!p) p = runtime_override(s->name);
            if (p) p = runtime_hle_wrap_named(p, s->name);   /* host replacements run on the host stack */
            if (!p && s->defined) p = s->value < m->key_off ? m->base + s->value : NULL;
            if (!p) p = runtime_symbol(s->name);
            if (!p && s->binding != 2) {
                p = runtime_hle_stub(s->name);
                if (p) p = runtime_hle_wrap_named(p, s->name);
            }
            if (!p && s->binding != 2) {
                fprintf(stderr, "[totk-android] unresolved %s in %s\n", s->name, m->name);
                runtime_fail("strong import unresolved");
            }
            if (!p) m->weak++;
            if (p && !s->defined) m->cross++;
            v = (uintptr_t)p + (r->kind == 1026 ? 0 : r->addend);
            m->symbolic++;
        }
        memcpy(m->base + r->where, &v, 8);
    }
}

void seal(Module *m) {
    size_t p = page_size();
    size_t text = page_up(m->text_end);
    size_t ro_end = m->data_off & ~(p - 1);
    need(text <= ro_end, "text/data overlap");
    /* text and the patch area were mapped R-X; do not add PROT_EXEC here. */
    (void)text;
    if (ro_end > page_up(m->text_end) && mprotect(m->base + page_up(m->text_end), ro_end - page_up(m->text_end), PROT_READ)) {
        fprintf(stderr, "[totk-android] rodata R %s: %s\n", m->name, strerror(errno));
        runtime_fail("rodata R");
    }
    if (mprotect(m->base + m->data_off, m->key_off - m->data_off, PROT_READ | PROT_WRITE)) {
        fprintf(stderr, "[totk-android] data RW %s: %s\n", m->name, strerror(errno));
        runtime_fail("data RW");
    }
    if (mprotect(m->base + m->key_off, 0x4000, PROT_READ)) {
        fprintf(stderr, "[totk-android] key R %s: %s\n", m->name, strerror(errno));
        runtime_fail("key page R");
    }
    if (m->file_size > m->patch_off)
        __builtin___clear_cache((char *)m->base + m->patch_off, (char *)m->base + m->file_size);
    __builtin___clear_cache((char *)m->base, (char *)m->base + page_up(m->text_end));
}

/* STOP_AFTER_INITS is a diagnostic for the constructor pass, so it implies INITS. */
int inits_enabled(void) {
    return tls_probe_mode || getenv("TOTK_ANDROID_INITS") || getenv("TOTK_ANDROID_STOP_AFTER_INITS");
}

/* Use the host override when present; runtime_symbol() returns the guest SDK
 * function, which would go through fsp-srv and defeat this probe. */
void *fs_entry(const char *name) {
    if (getenv("TOTK_ANDROID_FS_BY_SERVICE")) return runtime_symbol(name);
    void *p = runtime_override(name);
    return p ? p : runtime_symbol(name);
}

void fs_probe(void) {
    const char *profile = getenv("TOTK_ANDROID_PROFILE");
    if (!profile) profile = getenv("TOTK_ANDROID_FSROOT");
    if (!profile) profile = "/data/local/tmp/totk-android/user-data";
    mkdir(profile, 0777);

    uint8_t *sdk = runtime_module_base("sdk");
    if (sdk) fprintf(stderr, "[totk-android] fs state=%u flag=%u\n", sdk[0xc688c0], sdk[0xc688d0]);
    void (*fs_init)(void) = (void (*)(void))runtime_symbol("_ZN2nn2fs24InitializeForApplicationEv");
    if (fs_init && sdk && sdk[0xc688c0] != 1 && !sdk[0xc688d0]) {
        fprintf(stderr, "[totk-android] InitializeForApplication -> %d\n", ((int (*)(void))fs_init)());
    }
    int (*mount_temp)(const char *) = (int (*)(const char *))fs_entry("_ZN2nn2fs21MountTemporaryStorageEPKc");
    int (*mount_save)(const char *, const uint64_t *) = (int (*)(const char *, const uint64_t *))fs_entry("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    int (*mount_rom)(const char *, void *, uint64_t) = (int (*)(const char *, void *, uint64_t))fs_entry("_ZN2nn2fs8MountRomEPKcPvm");
    int (*create_file)(const char *, int64_t) = (int (*)(const char *, int64_t))fs_entry("_ZN2nn2fs10CreateFileEPKcl");
    int (*delete_file)(const char *) = (int (*)(const char *))fs_entry("_ZN2nn2fs10DeleteFileEPKc");
    int (*write_file)(uint64_t, int64_t, const void *, uint64_t, const int32_t *) = (int (*)(uint64_t, int64_t, const void *, uint64_t, const int32_t *))fs_entry("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    int (*commit)(const char *) = (int (*)(const char *))fs_entry("_ZN2nn2fs6CommitEPKc");
    int (*open_file)(uint64_t *, const char *, int) = (int (*)(uint64_t *, const char *, int))fs_entry("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    int (*get_size)(int64_t *, uint64_t) = (int (*)(int64_t *, uint64_t))fs_entry("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
    int (*read_file)(uint64_t *, uint64_t, int64_t, void *, uint64_t) = (int (*)(uint64_t *, uint64_t, int64_t, void *, uint64_t))fs_entry("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    int (*open_dir)(uint64_t *, const char *, int) = (int (*)(uint64_t *, const char *, int))fs_entry("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
    int (*read_dir)(int64_t *, void *, uint64_t, int64_t) = (int (*)(int64_t *, void *, uint64_t, int64_t))fs_entry("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
    void (*close_file)(uint64_t) = (void (*)(uint64_t))fs_entry("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    void (*close_dir)(uint64_t) = (void (*)(uint64_t))fs_entry("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
    need(mount_temp && open_file && get_size && read_file && open_dir && read_dir, "nn::fs probe symbols");

    int mr = mount_temp("temp");
    fprintf(stderr, "[totk-android] MountTemporaryStorage -> %d\n", mr);
    need(mr == 0, "MountTemporaryStorage");

    /* mount_temp() creates/cleans <profile>/temp, so write the file afterwards. */
    char temp_dir[PATH_MAX], test_path[PATH_MAX];
    snprintf(temp_dir, sizeof temp_dir, "%s/temp", profile);
    snprintf(test_path, sizeof test_path, "%s/test.txt", temp_dir);
    int fd = open(test_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    need(fd >= 0, "create fs probe file");
    need(write(fd, "hello", 5) == 5, "write fs probe file");
    close(fd);

    uint64_t file = 0;
    int fr = open_file(&file, "temp:/test.txt", 1);
    fprintf(stderr, "[totk-android] OpenFile -> %d handle=%#llx\n", fr, (unsigned long long)file);
    need(fr == 0, "OpenFile temp:/test.txt");
    int64_t size = -1;
    int sr = get_size(&size, file);
    fprintf(stderr, "[totk-android] GetFileSize -> %d size=%lld\n", sr, (long long)size);
    need(sr == 0 && size == 5, "GetFileSize temp:/test.txt");
    char data[32] = {0};
    uint64_t read = 0;
    int rr = read_file(&read, file, 0, data, sizeof data);
    fprintf(stderr, "[totk-android] ReadFile -> %d read=%llu data=%.*s\n",
            rr, (unsigned long long)read, (int)(read < sizeof data ? read : sizeof data), data);
    need(rr == 0 && read == 5 && !memcmp(data, "hello", 5), "ReadFile temp:/test.txt");
    if (close_file) close_file(file);
    uint64_t dir = 0;
    int dr = open_dir(&dir, "temp:/", 3);
    fprintf(stderr, "[totk-android] OpenDirectory -> %d handle=%#llx\n", dr, (unsigned long long)dir);
    need(dr == 0, "OpenDirectory temp:/");
    uint8_t entries[0x310 * 8];
    int64_t count = 0;
    int er = read_dir(&count, entries, dir, 8);
    fprintf(stderr, "[totk-android] ReadDirectory -> %d count=%lld\n", er, (long long)count);
    need(er == 0 && count >= 1, "ReadDirectory temp:/");
    if (close_dir) close_dir(dir);
    if (mount_save && create_file && write_file && commit) {
        const uint64_t uid[2] = {0, 0};
        int mr = mount_save("save", uid);
        fprintf(stderr, "[totk-android] MountSaveData -> %d\n", mr);
        need(mr == 0, "MountSaveData save");
        if (delete_file) delete_file("save:/probe.sav");   /* keep the probe repeatable */
        int cr = create_file("save:/probe.sav", 4);
        fprintf(stderr, "[totk-android] CreateFile save:/probe.sav -> %d\n", cr);
        need(cr == 0, "CreateFile save:/probe.sav");
        uint64_t save_file = 0;
        int ofr = open_file(&save_file, "save:/probe.sav", 2);
        fprintf(stderr, "[totk-android] OpenFile save:/probe.sav -> %d handle=%#llx\n",
                ofr, (unsigned long long)save_file);
        need(ofr == 0, "OpenFile save:/probe.sav");
        int wr = write_file(save_file, 0, "save", 4, NULL);
        fprintf(stderr, "[totk-android] WriteFile save:/probe.sav -> %d\n", wr);
        need(wr == 0, "WriteFile save:/probe.sav");
        if (close_file) close_file(save_file);
        int comr = commit("save");
        fprintf(stderr, "[totk-android] Commit save -> %d\n", comr);
        need(comr == 0, "Commit save");
    }
    if (getenv("TOTK_ANDROID_ROMFS") && mount_rom) {
        int mrr = mount_rom("rom", NULL, 0);
        fprintf(stderr, "[totk-android] MountRom -> %d\n", mrr);
        need(mrr == 0, "MountRom rom");
        uint64_t rom_file = 0;
        int ror = open_file(&rom_file, "rom:/hello.txt", 1);
        fprintf(stderr, "[totk-android] OpenFile rom:/hello.txt -> %d handle=%#llx\n",
                ror, (unsigned long long)rom_file);
        need(ror == 0, "OpenFile rom:/hello.txt");
        char rom_data[32] = {0};
        uint64_t rom_read = 0;
        int rrr = read_file(&rom_read, rom_file, 0, rom_data, sizeof rom_data);
        fprintf(stderr, "[totk-android] ReadFile rom:/hello.txt -> %d read=%llu data=%.*s\n",
                rrr, (unsigned long long)rom_read,
                (int)(rom_read < sizeof rom_data ? rom_read : sizeof rom_data), rom_data);
        need(rrr == 0 && rom_read == 10 && !memcmp(rom_data, "hello-rom\n", 10), "ReadFile rom:/hello.txt");
        if (close_file) close_file(rom_file);
    }
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"fs\",\"read\":%llu,\"entries\":%lld}\n",
           (unsigned long long)read, (long long)count);
    fflush(stdout);
}

/* Run a module's .init_array on the host.  The guest DT_INIT wrapper only
 * runs this loop when __nnmusl_init_dso reports a fini callback (state 2); when
 * the module has none it returns 1 and the wrapper skips every constructor.
 * On this 1.0.0 Android rewrite that leaves SEAD's globals uninitialised, so
 * the loader performs the constructor pass itself after the guest's musl
 * bookkeeping has run.  This is an init hook: the guest image is untouched. */
uint64_t run_init_array(Module *m) {
    uint64_t n = m->init_size / 8, called = 0;
    uint64_t watch_before = 0;
    uint8_t *main_base = runtime_module_base("main");
    int watch = getenv("TOTK_ANDROID_CTOR_PROBE") && main_base;
    if (watch) memcpy(&watch_before, main_base + 0x463c048, 8);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t fn = 0;
        memcpy(&fn, m->base + m->init_array + i * 8, 8);
        if (!fn) continue;
        ((void (*)(void))(uintptr_t)fn)();
        called++;
        if (watch) {
            uint64_t now = 0;
            memcpy(&now, main_base + 0x463c048, 8);
            if (now != watch_before) {
                fprintf(stderr, "[totk-android] constructor %llu sets main+0x463c048 = %#llx\n",
                        (unsigned long long)i, (unsigned long long)now);
                watch_before = now;
            }
        }
    }
    return called;
}

void module_inits(void) {
    if (inits_enabled()) {
        for (unsigned i = 0; i < NMODULES; i++) {
            fprintf(stderr, "[totk-android] ENTER %s DT_INIT\n", modules[i].name);
            if (getenv("TOTK_ANDROID_CTOR_PROBE")) {
                const Module *m = &modules[i];
                uint32_t before = 0;
                memcpy(&before, m->base + m->ctor_flag, 4);
                uint64_t entries = m->init_size / 8, nonnull = 0;
                for (uint64_t k = 0; k < entries; k++) {
                    uint64_t fn = 0;
                    memcpy(&fn, m->base + m->init_array + k * 8, 8);
                    if (fn) nonnull++;
                }
                fprintf(stderr, "[totk-android] ctor probe %s state_before=%u .init_array=%llu entries (%llu non-null)\n",
                        m->name, before, (unsigned long long)entries, (unsigned long long)nonnull);
            }
            uint64_t ret = ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t, uint64_t))
                            (modules[i].base + modules[i].init))(0, 0, 0, 0, 0, 0, 0, 0, 0);
            uint32_t state;
            memcpy(&state, modules[i].base + modules[i].ctor_flag, 4);
            fprintf(stderr, "[totk-android] RETURN %s DT_INIT state=%u ret=%#llx init_array=%llu\n",
                    modules[i].name, state, (unsigned long long)ret,
                    (unsigned long long)(modules[i].init_size / 8));
            need(state == 1 || state == 2, "DT_INIT deferred or failed");
            if (state == 1) {
                uint64_t called = run_init_array(&modules[i]);
                fprintf(stderr, "[totk-android] constructors %s (host pass): %llu of %llu\n",
                        modules[i].name, (unsigned long long)called,
                        (unsigned long long)(modules[i].init_size / 8));
            }
        }
        if (getenv("TOTK_ANDROID_CTOR_PROBE")) {
            uint64_t tlr = 0, tt = 0, tb = 0, ts = 0, t70 = 0;
            if (g_slots) { tlr = g_slots->ktlr; memcpy(&tt, (void *)(uintptr_t)(tlr + 0x1f8), 8); }
            if (tt) { memcpy(&tb, (void *)(uintptr_t)(tt + 0x50), 8); memcpy(&ts, (void *)(uintptr_t)(tt + 0x58), 8); memcpy(&t70, (void *)(uintptr_t)(tt + 0x70), 8); }
            fprintf(stderr, "[totk-android] tls state ktlr=%#llx tt=%#llx base=%#llx size=%#llx t70=%#llx\n",
                    (unsigned long long)tlr, (unsigned long long)tt,
                    (unsigned long long)tb, (unsigned long long)ts, (unsigned long long)t70);
            uint8_t *mb = runtime_module_base("main");
            if (mb) {
                uint64_t thread_mgr = 0;
                memcpy(&thread_mgr, mb + 0x463c000, 8);   /* 1.0.0-only diagnostic */
                fprintf(stderr, "[totk-android] after inits sead::ThreadMgr=%#llx\n",
                        (unsigned long long)thread_mgr);
            }
        }
        if (tls_probe_mode) tls_probe();
        if (getenv("TOTK_ANDROID_FS_PROBE")) fs_probe();
    }
    if (getenv("TOTK_ANDROID_STOP_AFTER_INITS")) {
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"module-inits\"}\n");
        fflush(stdout);
        runtime_hle_trace_report();
        _exit(0);
    }
}

void module_finis(void) { }

char *read_text_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) { close(fd); return NULL; }
    char *b = malloc((size_t)st.st_size + 1);
    ssize_t n = b ? read(fd, b, (size_t)st.st_size) : -1;
    close(fd);
    if (!b || n != st.st_size) { free(b); return NULL; }
    b[n] = 0;
    if (size) *size = (size_t)n;
    return b;
}
