/* Module: cache. */
#include "shader_cache_internal.h"

static char g_cache_root[RUNTIME_SHADER_PATH_MAX];

static int g_cache_root_set;

const char *runtime_shader_cache_root(void) {
    if (g_cache_root_set) return g_cache_root;
    const char *explicit_root = getenv("TOTK_ANDROID_SHADER_CACHE");
    if (explicit_root && *explicit_root) {
        snprintf(g_cache_root, sizeof g_cache_root, "%s", explicit_root);
    } else {
        const char *profile = getenv("TOTK_ANDROID_PROFILE");
        if (!profile || !*profile) profile = getenv("TOTK_ANDROID_FSROOT");
        if (!profile || !*profile) profile = "/data/local/tmp/totk-android";
        snprintf(g_cache_root, sizeof g_cache_root, "%s/shader-cache", profile);
    }
    g_cache_root_set = 1;
    return g_cache_root;
}

int mkdir_p(const char *path) {
    char tmp[RUNTIME_SHADER_PATH_MAX];
    size_t n = strlen(path);
    if (!n || n >= sizeof tmp) return 0;
    memcpy(tmp, path, n + 1);
    if (tmp[n - 1] == '/') tmp[--n] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return 0;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

int read_file(const char *path, uint8_t **out, size_t *size) {
    /* Do NOT give the virtual core up here: callers (nvn_program_set_shaders /
     * nvn_program_cached_pipeline) hold g_prog_lock across this read.  Handing
     * the core to a thread that then blocks on g_prog_lock inverts host lock
     * against virtual core (seen as a futex stall in nvnCommandBufferEndRecording). */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n <= 0 || n > (long)SHADER_SPV_MAX) { fclose(f); return 0; }
    rewind(f);
    uint8_t *p = malloc((size_t)n);
    if (!p) { fclose(f); return 0; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return 0; }
    fclose(f);
    *out = p;
    *size = (size_t)n;
    return 1;
}

int ref_path_for(const char *spv_path, char *out, size_t cap) {
    size_t n = strlen(spv_path);
    if (n < 4 || n + 5 >= cap || strcmp(spv_path + n - 4, ".spv")) return 0;
    snprintf(out, cap, "%.*s.res.json", (int)(n - 4), spv_path);
    return 1;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* <root>/<subdir>/<key>.spv, or <root>/<key>.spv for an empty subdir. */
int try_candidate(const char *root, const char *subdir, const char *name,
                         RuntimeShaderModule *out) {
    char spv[RUNTIME_SHADER_PATH_MAX], res[RUNTIME_SHADER_PATH_MAX];
    if (*subdir)
        snprintf(spv, sizeof spv, "%s/%s/%s", root, subdir, name);
    else
        snprintf(spv, sizeof spv, "%s/%s", root, name);
    uint8_t *data = NULL;
    size_t size = 0;
    if (!read_file(spv, &data, &size)) return 0;
    if (!is_spv(data, size)) {
        free(data);
        return 0;
    }
    out->spv = data;
    out->spv_size = size;
    snprintf(out->spv_path, sizeof out->spv_path, "%s", spv);
    out->res_path[0] = 0;
    if (ref_path_for(spv, res, sizeof res) && file_exists(res))
        snprintf(out->res_path, sizeof out->res_path, "%s", res);
    return 1;
}

/* The seed names a variant <key>-<suffix>.  Pick the lexicographically first
 * match in the stage directory, then in the cache root. */
int find_variant(const char *root, const char *subdir, const char *key,
                        char *name, size_t cap) {
    char dir[RUNTIME_SHADER_PATH_MAX];
    if (*subdir) snprintf(dir, sizeof dir, "%s/%s", root, subdir);
    else snprintf(dir, sizeof dir, "%s", root);
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t klen = strlen(key);
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *n = ent->d_name;
        size_t nlen = strlen(n);
        if (nlen < klen + 5 || strncmp(n, key, klen) != 0 || n[klen] != '-') continue;
        if (strcmp(n + nlen - 4, ".spv") != 0) continue;
        if (!found || strcmp(n, name) < 0) {
            snprintf(name, cap, "%s", n);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

int runtime_shader_cache_load(const char *root, const RuntimeShaderIdentity *id,
                              RuntimeShaderModule *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!id) return 0;
    if (!root || !*root) root = runtime_shader_cache_root();
    out->stage = id->stage;
    const char *subdir = runtime_shader_stage_name(id->stage);
    char name[RUNTIME_SHADER_PATH_MAX];

    snprintf(name, sizeof name, "%s.spv", id->key);
    if (try_candidate(root, subdir, name, out)) return 1;
    if (try_candidate(root, "", name, out)) return 1;

    if (find_variant(root, subdir, id->key, name, sizeof name) &&
        try_candidate(root, subdir, name, out))
        return 1;
    if (find_variant(root, "", id->key, name, sizeof name) &&
        try_candidate(root, "", name, out))
        return 1;
    return 0;
}

int suffix_ok(const char *s) {
    if (!s || !*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

int runtime_shader_cache_load_variant(const char *root, const RuntimeShaderIdentity *id,
                                      const char *suffix, RuntimeShaderModule *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!id) return 0;
    if (!root || !*root) root = runtime_shader_cache_root();
    if (!suffix_ok(suffix)) return runtime_shader_cache_load(root, id, out);
    const char *subdir = runtime_shader_stage_name(id->stage);
    char name[RUNTIME_SHADER_PATH_MAX];
    snprintf(name, sizeof name, "%s-%s.spv", id->key, suffix);
    if (try_candidate(root, subdir, name, out)) return 1;
    if (try_candidate(root, "", name, out)) return 1;
    return runtime_shader_cache_load(root, id, out);
}

int runtime_shader_cache_has_variants(const char *root, const RuntimeShaderIdentity *id) {
    if (!id) return 0;
    if (!root || !*root) root = runtime_shader_cache_root();
    const char *subdirs[2] = { runtime_shader_stage_name(id->stage), "" };
    size_t klen = strlen(id->key);
    for (unsigned k = 0; k < 2; k++) {
        char dir[RUNTIME_SHADER_PATH_MAX];
        if (subdirs[k][0]) snprintf(dir, sizeof dir, "%s/%s", root, subdirs[k]);
        else snprintf(dir, sizeof dir, "%s", root);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *ent;
        int found = 0;
        while ((ent = readdir(d))) {
            const char *n = ent->d_name;
            size_t nlen = strlen(n);
            if (nlen > klen + 5 && !strncmp(n, id->key, klen) && n[klen] == '-' &&
                !strcmp(n + nlen - 4, ".spv")) { found = 1; break; }
        }
        closedir(d);
        if (found) return 1;
    }
    return 0;
}

void runtime_shader_cache_module_free(RuntimeShaderModule *module) {
    if (!module) return;
    free(module->spv);
    module->spv = NULL;
    module->spv_size = 0;
    module->spv_path[0] = 0;
    module->res_path[0] = 0;
}

int write_file(const char *path, const void *data, size_t size) {
    char tmp[RUNTIME_SHADER_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { unlink(tmp); return 0; }
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

int runtime_shader_cache_store(const char *root, const RuntimeShaderIdentity *id,
                               const uint8_t *spv, size_t spv_size,
                               const char *res_json) {
    if (!id || !is_spv(spv, spv_size)) return 0;
    if (!root || !*root) root = runtime_shader_cache_root();
    const char *subdir = runtime_shader_stage_name(id->stage);
    char dir[RUNTIME_SHADER_PATH_MAX], path[RUNTIME_SHADER_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", root, subdir);
    if (!mkdir_p(dir)) return 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, id->key);
    if (!write_file(path, spv, spv_size)) return 0;
    if (res_json && *res_json) {
        snprintf(path, sizeof path, "%s/%s.res.json", dir, id->key);
        if (!write_file(path, res_json, strlen(res_json))) return 0;
    }
    return 1;
}
