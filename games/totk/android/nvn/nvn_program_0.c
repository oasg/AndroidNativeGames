/* NVN module: program_0. */
#include "nvn_internal.h"

void nvn_global_ubo_set(int stage, int index, uint64_t address, size_t size) {
    if (stage < 0 || stage >= 6 || index < 0 || index >= 16) return;
    pthread_mutex_lock(&g_global_ubo_lock);
    g_global_ubo[stage][index].address = address;
    g_global_ubo[stage][index].size = size;
    g_global_ubo[stage][index].valid = (address != 0 && size != 0);
    pthread_mutex_unlock(&g_global_ubo_lock);
}

int nvn_global_ubo_get(int stage, int index, uint64_t *address, size_t *size) {
    int ok = 0;
    if (stage < 0 || stage >= 6 || index < 0 || index >= 16) return 0;
    pthread_mutex_lock(&g_global_ubo_lock);
    if (g_global_ubo[stage][index].valid) {
        *address = g_global_ubo[stage][index].address;
        *size = g_global_ubo[stage][index].size;
        ok = 1;
    }
    pthread_mutex_unlock(&g_global_ubo_lock);
    return ok;
}

void nvn_global_ssbo_set(int stage, int index, uint64_t address, size_t size) {
    if (stage < 0 || stage >= 6 || index < 0 || index >= 16) return;
    pthread_mutex_lock(&g_global_ssbo_lock);
    g_global_ssbo[stage][index].address = address;
    g_global_ssbo[stage][index].size = size;
    g_global_ssbo[stage][index].valid = (address != 0 && size != 0);
    pthread_mutex_unlock(&g_global_ssbo_lock);
}

int nvn_global_ssbo_get(int stage, int index, uint64_t *address, size_t *size) {
    int ok = 0;
    if (stage < 0 || stage >= 6 || index < 0 || index >= 16) return 0;
    pthread_mutex_lock(&g_global_ssbo_lock);
    if (g_global_ssbo[stage][index].valid) {
        *address = g_global_ssbo[stage][index].address;
        *size = g_global_ssbo[stage][index].size;
        ok = 1;
    }
    pthread_mutex_unlock(&g_global_ssbo_lock);
    return ok;
}

void nvn_prog_ubo_set(const prog_t *p, int stage, int index,
                             uint64_t address, size_t size) {
    if (!p || stage < 0 || stage >= 6 || index < 0 || index >= 16) return;
    pthread_mutex_lock(&g_prog_ubo_lock);
    nvn_prog_ubo_t *e = NULL, *oldest = NULL;
    for (unsigned i = 0; i < g_prog_ubo_count; i++) {
        if (g_prog_ubos[i].prog == p && g_prog_ubos[i].stage == (uint32_t)stage &&
            g_prog_ubos[i].index == (uint32_t)index) { e = &g_prog_ubos[i]; break; }
        if (!oldest || g_prog_ubos[i].age < oldest->age) oldest = &g_prog_ubos[i];
    }
    if (!e) {
        if (g_prog_ubo_count < NVN_PROG_UBO_MAX) e = &g_prog_ubos[g_prog_ubo_count++];
        else if (oldest) e = oldest;
    }
    if (e) {
        e->prog = p;
        e->stage = (uint32_t)stage;
        e->index = (uint32_t)index;
        e->address = address;
        e->size = size;
        e->age = ++g_prog_ubo_age;
    }
    pthread_mutex_unlock(&g_prog_ubo_lock);
}

int nvn_prog_ubo_get(const prog_t *p, int stage, int index,
                            uint64_t *address, size_t *size) {
    int ok = 0;
    if (!p) return 0;
    pthread_mutex_lock(&g_prog_ubo_lock);
    for (unsigned i = 0; i < g_prog_ubo_count; i++) {
        nvn_prog_ubo_t *e = &g_prog_ubos[i];
        if (e->prog != p || e->stage != (uint32_t)stage || e->index != (uint32_t)index)
            continue;
        if (!e->address || !e->size) break;
        if (address) *address = e->address;
        if (size) *size = e->size;
        e->age = ++g_prog_ubo_age;
        ok = 1;
        break;
    }
    pthread_mutex_unlock(&g_prog_ubo_lock);
    return ok;
}

void nvn_prog_ubo_forget(const prog_t *p) {
    if (!p) return;
    pthread_mutex_lock(&g_prog_ubo_lock);
    for (unsigned i = 0; i < g_prog_ubo_count; ) {
        if (g_prog_ubos[i].prog == p) {
            g_prog_ubos[i] = g_prog_ubos[--g_prog_ubo_count];
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&g_prog_ubo_lock);
}

GfxReflectedPipeline *prog_pipe_find(prog_shader_state_t *s, int fmt,
                                            uint64_t layout_sig, uint64_t vertex_sig,
                                            uint64_t variant_sig) {
    for (unsigned i = 0; i < s->npipes && i < NVN_PROG_PIPE_SLOTS; i++)
        if (s->pipes[i] && s->pipe_fmt[i] == fmt &&
            s->pipe_layout_sig[i] == layout_sig &&
            s->pipe_vertex_sig[i] == vertex_sig &&
            s->pipe_variant_sig[i] == variant_sig)
            return s->pipes[i];
    return NULL;
}

void prog_pipe_add(prog_shader_state_t *s, GfxReflectedPipeline *pipe, int fmt,
                          uint64_t layout_sig, uint64_t vertex_sig,
                          uint64_t variant_sig) {
    unsigned slot = s->npipes < NVN_PROG_PIPE_SLOTS ? s->npipes++ : s->pipe_next++ % NVN_PROG_PIPE_SLOTS;
    if (s->pipes[slot]) {
        if (s->pipeline == s->pipes[slot]) s->pipeline = NULL;
        gfx_backend_reflected_pipeline_destroy(s->pipes[slot]);
    }
    s->pipes[slot] = pipe;
    s->pipe_fmt[slot] = fmt;
    s->pipe_layout_sig[slot] = layout_sig;
    s->pipe_vertex_sig[slot] = vertex_sig;
    s->pipe_variant_sig[slot] = variant_sig;
}

void prog_pipe_clear(prog_shader_state_t *s) {
    for (unsigned i = 0; i < NVN_PROG_PIPE_SLOTS; i++) {
        if (s->pipes[i]) gfx_backend_reflected_pipeline_destroy(s->pipes[i]);
        s->pipes[i] = NULL;
    }
    s->npipes = s->pipe_next = 0;
    s->pipeline = NULL;
}

void prog_stage_clear(prog_shader_state_t *s, unsigned stage) {
    if (!s || stage >= RUNTIME_SHADER_STAGES) return;
    if (stage == 5 && s->compute_pipeline) {
        gfx_backend_compute_pipeline_destroy(s->compute_pipeline);
        s->compute_pipeline = NULL;
        s->compute_nspecs = 0;
    }
    free(s->spv[stage]);
    s->spv[stage] = NULL;
    s->spv_size[stage] = 0;
    s->spv_path[stage][0] = 0;
    s->res_path[stage][0] = 0;
    s->key[stage][0] = 0;
    memset(&s->id[stage], 0, sizeof s->id[stage]);
    if (stage < 2) {
        free(s->vspv[stage]);
        s->vspv[stage] = NULL;
        s->vspv_size[stage] = 0;
        s->vspv_path[stage][0] = 0;
        s->vres_path[stage][0] = 0;
        s->variant[0] = 0;
        s->variant_sig = 0;
    }
    s->loaded &= ~(1u << stage);
    prog_pipe_clear(s);
    s->pipeline_format = 0;
    s->layout_sig = 0;
}

void prog_state_clear_locked(prog_shader_state_t *s) {
    if (!s) return;
    for (unsigned i = 0; i < RUNTIME_SHADER_STAGES; i++) prog_stage_clear(s, i);
    s->loaded = 0;
}

unsigned prog_state_hash(const prog_t *p) {
    uintptr_t v = (uintptr_t)p >> 4;
    v ^= v >> 17;
    v *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(v & (PROG_STATE_BUCKETS - 1));
}

prog_shader_state_t *prog_state_find(prog_t *p) {
    for (prog_state_slot_t *e = g_prog_states[prog_state_hash(p)]; e; e = e->next)
        if (e->program == p) return e->state;
    return NULL;
}

prog_shader_state_t *prog_state_install(prog_t *p, prog_shader_state_t *s) {
    unsigned b = prog_state_hash(p);
    for (prog_state_slot_t *e = g_prog_states[b]; e; e = e->next) {
        if (e->program == p) { e->state = s; return s; }
    }
    prog_state_slot_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->program = p;
    e->state = s;
    e->next = g_prog_states[b];
    g_prog_states[b] = e;
    return s;
}

prog_shader_state_t *prog_state_remove(prog_t *p) {
    unsigned b = prog_state_hash(p);
    prog_state_slot_t **link = &g_prog_states[b];
    while (*link) {
        prog_state_slot_t *e = *link;
        if (e->program == p) {
            prog_shader_state_t *s = e->state;
            *link = e->next;
            free(e);
            return s;
        }
        link = &e->next;
    }
    return NULL;
}

/* Host-only program resources.  The guest NVNprogram contains no host
 * pointers; everything that used to live at +0x68.. is kept here. */
prog_shader_state_t *prog_state_ensure(prog_t *p) {
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_find(p);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (s) {
            s->owner = p;
            if (!prog_state_install(p, s)) { free(s); s = NULL; }
        }
    }
    pthread_mutex_unlock(&g_prog_lock);
    return s;
}

prog_host_t prog_host_view(prog_t *p) {
    prog_host_t h;
    memset(&h, 0, sizeof h);
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_find(p);
    if (s) {
        h.gfx_pipeline = s->gfx_pipeline;
        h.gfx_textured_pipeline = s->gfx_textured_pipeline;
        h.gfx_texture = s->gfx_texture;
        h.gfx_reflected_pipeline = s->gfx_reflected_pipeline;
        h.textured = s->textured;
    }
    pthread_mutex_unlock(&g_prog_lock);
    return h;
}

void prog_host_set(prog_t *p, void *gfx_pipeline, void *gfx_textured_pipeline,
                          void *gfx_texture, GfxReflectedPipeline *reflected,
                          uint32_t textured) {
    prog_shader_state_t *s = prog_state_ensure(p);
    if (!s) return;
    pthread_mutex_lock(&g_prog_lock);
    if (gfx_pipeline) s->gfx_pipeline = gfx_pipeline;
    if (gfx_textured_pipeline) s->gfx_textured_pipeline = gfx_textured_pipeline;
    if (gfx_texture) s->gfx_texture = gfx_texture;
    if (reflected) s->gfx_reflected_pipeline = reflected;
    s->textured = textured;
    pthread_mutex_unlock(&g_prog_lock);
}

uint32_t nvnProgramInitialize(prog_t *p, const nvn_dev_t *d) { nvn_api("nvnProgramInitialize");
    if (!p || !nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnProgramInitialize(device)")) return 0;
    memset(p, 0, sizeof *p);
    p->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    p->serial = (uint64_t)(uintptr_t)p;
    return 1;
}

void prog_state_free_locked(prog_shader_state_t *s) {
    if (!s) return;
    if (s->gfx_reflected_pipeline)
        gfx_backend_reflected_pipeline_destroy(s->gfx_reflected_pipeline);
    s->gfx_reflected_pipeline = NULL;
    prog_state_clear_locked(s);
    free(s);
}

void nvn_prog_release(prog_shader_state_t *s) {
    if (!s) return;
    pthread_mutex_lock(&g_prog_lock);
    if (s->refs) s->refs--;
    if (!s->refs && s->orphan) prog_state_free_locked(s);
    pthread_mutex_unlock(&g_prog_lock);
}

/* Retain a program state for a recorded dispatch.  Mirrors the segment path:
 * a finalized guest program stays alive until the last reference is dropped. */
void nvn_prog_ref(prog_shader_state_t *s) {
    if (!s) return;
    pthread_mutex_lock(&g_prog_lock);
    s->refs++;
    pthread_mutex_unlock(&g_prog_lock);
}

void nvnProgramFinalize(prog_t *p) {
    if (!p) return;
    nvn_prog_ubo_forget(p);
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_remove(p);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] ProgramFinalize %p state=%p refs=%u\n",
                (void *)p, (void *)s, s ? s->refs : 0);
    if (s) {
        s->orphan = 1;
        /* A queued scene segment may still reference this program's pipeline.
         * Keep the host state alive until the last segment is dropped. */
        if (!s->refs) prog_state_free_locked(s);
    }
    pthread_mutex_unlock(&g_prog_lock);
    p->magic = 0;
}

void nvnProgramSetDebugLabel(prog_t *p, const char *l) { (void)p; (void)l; }
