/* In-process Horizon IPC server. Message framing follows the public HIPC/CMIF/TIPC description
   (libnx sf/hipc.h, cmif.h, tipc.h; ISC licence). The SDK client code runs unchanged: requests are
   read from the calling thread's TLS message buffer and answered in place. Unknown services and
   commands stop the runtime with their name and id instead of returning a fake success. */
#include "runtime.h"
#include "ipc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMIF_IN_MAGIC 0x49434653u   /* "SFCI" */
#define CMIF_OUT_MAGIC 0x4f434653u  /* "SFCO" */
#define MSG_BYTES 0x100
#define POINTER_BUFFER_SIZE 0x8000

enum { T_CLOSE = 2, T_REQUEST = 4, T_CONTROL = 5, T_REQUEST_CTX = 6, T_CONTROL_CTX = 7, T_TIPC_BASE = 16, T_TIPC_CLOSE = 15 };

typedef struct { const char *name; IpcHandler handler; IpcOpen open; void (*close)(void *); } Service;
static Service services[32];
static unsigned nservices;
static __thread uint64_t trace_fp, trace_lr;   /* guest frame of the current request, for failure reports */
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;

/* TOTK_IPC_TRACE=1 logs one line per request:
 *   [ipc] service=lm command=0 object=1 type=6 data=8 result=0
 * The same record is kept for the crash handler even when tracing is off. */
static RuntimeIpcRecord ipc_last;
static _Atomic unsigned ipc_last_seq;
static pthread_mutex_t ipc_last_lock = PTHREAD_MUTEX_INITIALIZER;
static int ipc_trace = -1;

static int ipc_trace_enabled(void) {
    int v = __atomic_load_n(&ipc_trace, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *e = getenv("TOTK_IPC_TRACE");
        v = e && *e && strcmp(e, "0") ? 1 : 0;
        __atomic_store_n(&ipc_trace, v, __ATOMIC_RELAXED);
    }
    return v;
}

static void ipc_record(const IpcSession *s, const IpcRequest *rq, uint32_t object, uint32_t result) {
    RuntimeIpcRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.valid = 1;
    snprintf(rec.service, sizeof rec.service, "%s", s && s->name ? s->name : "?");
    rec.command = rq ? rq->cmd : 0;
    rec.object = object;
    rec.type = rq ? rq->type : 0;
    rec.data_size = rq ? rq->data_size : 0;
    rec.result = result;
    pthread_mutex_lock(&ipc_last_lock);
    unsigned seq = atomic_load_explicit(&ipc_last_seq, memory_order_relaxed);
    atomic_store_explicit(&ipc_last_seq, seq + 1, memory_order_relaxed);
    memcpy(&ipc_last, &rec, sizeof rec);
    atomic_store_explicit(&ipc_last_seq, seq + 2, memory_order_release);
    pthread_mutex_unlock(&ipc_last_lock);
    if (!ipc_trace_enabled()) return;
    fprintf(stderr, "[ipc] service=%s command=%u object=%u type=%u data=%u result=%u\n",
            rec.service, rec.command, rec.object, rec.type, rec.data_size, rec.result);
}

int runtime_ipc_last(RuntimeIpcRecord *out) {
    if (!out) return 0;
    for (int tries = 0; tries < 4; tries++) {
        unsigned a = atomic_load_explicit(&ipc_last_seq, memory_order_acquire);
        if (a & 1u) continue;
        memcpy(out, &ipc_last, sizeof *out);
        unsigned b = atomic_load_explicit(&ipc_last_seq, memory_order_acquire);
        if (a == b) return out->valid;
    }
    memset(out, 0, sizeof *out);
    return 0;
}

void runtime_ipc_register(const char *name, IpcHandler handler, IpcOpen open, void (*close)(void *)) {
    pthread_mutex_lock(&registry);
    if (nservices == sizeof services / sizeof *services) runtime_fail("IPC service registry full");
    services[nservices++] = (Service){name, handler, open, close};
    pthread_mutex_unlock(&registry);
}

static IpcSession *session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    IpcSession *s = calloc(1, sizeof *s);
    if (!s) runtime_fail("IPC session allocation");
    *s = (IpcSession){.name = name, .handler = handler, .state = state, .close = close};
    return s;
}
uint32_t runtime_ipc_new_session(const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    return runtime_handle_new(1, session_new(name, handler, state, close));
}
void runtime_ipc_session_closed(void *p) {
    IpcSession *s = p;
    if (s->domain) {
        for (unsigned i = 0; i < s->domain->nobjects; i++) {
            IpcObject *o = &s->domain->objects[i];
            if (o->owned && o->close) o->close(o->state);
        }
        free(s->domain);
    }
    if (s->close) s->close(s->state);   /* the base object's state belongs to the session */
    free(s);
}

/* ---------- Domain objects ---------- */
static uint32_t domain_convert(IpcSession *s) {
    if (s->domain) return s->self_id;
    IpcDomain *d = calloc(1, sizeof *d);
    if (!d) runtime_fail("IPC domain allocation");
    s->domain = d;
    d->next_id = 1;
    s->self_id = d->next_id++;
    d->objects[d->nobjects++] = (IpcObject){
        .id = s->self_id, .name = s->name, .handler = s->handler,
        .state = s->state, .close = NULL, .owned = 0,
    };
    return s->self_id;
}
uint32_t ipc_domain_add_object(IpcSession *s, const char *name, IpcHandler handler, void *state, void (*close)(void *)) {
    if (!s || !s->domain) runtime_fail("IPC object registration outside a domain");
    IpcDomain *d = s->domain;
    if (d->nobjects == IPC_MAX_OBJECTS) runtime_fail("IPC domain object table full");
    uint32_t id = d->next_id++;
    if (!id) runtime_fail("IPC domain object id wrapped");
    d->objects[d->nobjects++] = (IpcObject){
        .id = id, .name = name, .handler = handler,
        .state = state, .close = close, .owned = 1,
    };
    return id;
}
static IpcObject *domain_find(IpcDomain *d, uint32_t id) {
    for (unsigned i = 0; i < d->nobjects; i++) if (d->objects[i].id == id) return &d->objects[i];
    return NULL;
}
static int domain_remove(IpcDomain *d, uint32_t id) {
    for (unsigned i = 0; i < d->nobjects; i++) {
        if (d->objects[i].id != id) continue;
        IpcObject *o = &d->objects[i];
        if (o->owned && o->close) o->close(o->state);
        d->objects[i] = d->objects[--d->nobjects];
        return 1;
    }
    return 0;
}
void runtime_ipc_tmem_closed(void *p) { free(p); }

void *ipc_guest(uint64_t addr, uint64_t size) {
    if (!addr && size) runtime_fail("IPC null buffer");
    if (addr + size < addr) runtime_fail("IPC buffer overflow");
    return (void *)(uintptr_t)addr;
}
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n) {
    if (off + n > rq->data_size) { fprintf(stderr, "[totk-ipc] cmd %u argument %u+%u beyond %u bytes\n", rq->cmd, off, n, rq->data_size); runtime_fail("IPC argument out of range"); }
    return rq->data + off;
}
void ipc_out(IpcResponse *rs, const void *p, uint32_t n) {
    if (rs->data_size + n > sizeof rs->data) runtime_fail("IPC response too large");
    memcpy(rs->data + rs->data_size, p, n); rs->data_size += n;
}
_Noreturn void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) {
    if (ipc_trace_enabled())
        fprintf(stderr, "[ipc] unknown command service=%s command=%u type=%u data=%u\n",
                s->name, rq->cmd, rq->type, rq->data_size);
    fprintf(stderr, "[totk-ipc] %s: command %u not implemented (type %u, data %u bytes, x%u a%u b%u w%u c%u)\n",
            s->name, rq->cmd, rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc);
    runtime_trace_guest(trace_fp, trace_lr);
    runtime_fail("IPC command not implemented");
}

/* ---------- HIPC framing ---------- */
static uint64_t static_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[0] >> 12) & 0xf) << 32) | ((uint64_t)((d[0] >> 6) & 0x3f) << 36); }
static uint64_t buffer_addr(const uint32_t *d) { return d[1] | ((uint64_t)((d[2] >> 28) & 0xf) << 32) | ((uint64_t)((d[2] >> 2) & 0x3fffff) << 36); }
static uint64_t buffer_size(const uint32_t *d) { return d[0] | ((uint64_t)((d[2] >> 24) & 0xf) << 32); }

static void parse(const uint32_t *m, IpcRequest *rq, const uint32_t **data_words, unsigned *ndata) {
    memset(rq, 0, sizeof *rq);
    rq->type = m[0] & 0xffff;
    rq->nx = (m[0] >> 16) & 0xf; rq->na = (m[0] >> 20) & 0xf; rq->nb = (m[0] >> 24) & 0xf; rq->nw = (m[0] >> 28) & 0xf;
    *ndata = m[1] & 0x3ff;
    unsigned recv_mode = (m[1] >> 10) & 0xf, special = m[1] >> 31;
    if (rq->nx > IPC_MAX_BUF || rq->na > IPC_MAX_BUF || rq->nb > IPC_MAX_BUF || rq->nw > IPC_MAX_BUF) runtime_fail("IPC too many buffers");
    const uint32_t *p = m + 2;
    if (special) {
        uint32_t h = *p++;
        rq->has_pid = h & 1; rq->ncopy = (h >> 1) & 0xf; rq->nmove = (h >> 5) & 0xf;
        if (rq->ncopy > 8 || rq->nmove > 8) runtime_fail("IPC too many handles");
        if (rq->has_pid) { memcpy(&rq->pid, p, 8); p += 2; }
        for (unsigned i = 0; i < rq->ncopy; i++) rq->copy[i] = *p++;
        for (unsigned i = 0; i < rq->nmove; i++) rq->move[i] = *p++;
    }
    for (unsigned i = 0; i < rq->nx; i++, p += 2) rq->x[i] = (IpcBuffer){static_addr(p), p[0] >> 16, p[0] & 0x3f};
    IpcBuffer *lists[3] = {rq->a, rq->b, rq->w}; unsigned counts[3] = {rq->na, rq->nb, rq->nw};
    for (unsigned k = 0; k < 3; k++) for (unsigned i = 0; i < counts[k]; i++, p += 3) lists[k][i] = (IpcBuffer){buffer_addr(p), buffer_size(p), i};
    *data_words = p;
    const uint32_t *recv = p + *ndata;
    rq->nc = recv_mode == 2 ? 1 : recv_mode > 2 ? recv_mode - 2 : 0;
    if (rq->nc > IPC_MAX_BUF) runtime_fail("IPC too many receive buffers");
    for (unsigned i = 0; i < rq->nc; i++, recv += 2) rq->c[i] = (IpcBuffer){recv[0] | ((uint64_t)(recv[1] & 0xffff) << 32), recv[1] >> 16, i};
    if ((const uint8_t *)recv - (const uint8_t *)m > MSG_BYTES) runtime_fail("IPC request exceeds message buffer");
}

static void respond(uint32_t *m, int tipc, int domain, const IpcResponse *rs) {
    uint32_t out[MSG_BYTES / 4]; memset(out, 0, sizeof out);
    unsigned special = rs->ncopy || rs->nmove;
    uint32_t *p = out + 2;
    if (special) {
        *p++ = (rs->ncopy << 1) | (rs->nmove << 5);
        for (unsigned i = 0; i < rs->ncopy; i++) *p++ = rs->copy[i];
        for (unsigned i = 0; i < rs->nmove; i++) *p++ = rs->move[i];
    }
    for (unsigned i = 0; i < rs->nxout; i++, p += 2) {
        uint64_t a = rs->xout[i].addr;
        p[0] = (rs->xout[i].index & 0x3f) | (uint32_t)(((a >> 36) & 0x3f) << 6) | (uint32_t)(((a >> 32) & 0xf) << 12) | (uint32_t)(rs->xout[i].size << 16);
        p[1] = (uint32_t)a;
    }
    uint8_t *words = (uint8_t *)p, *payload;
    unsigned header = tipc ? 4 : 16;
    if (tipc) payload = words;
    else payload = (uint8_t *)out + (((words - (uint8_t *)out) + 15) & ~15);
    unsigned domain_header = domain && !tipc ? 16 : 0;
    unsigned used = (unsigned)(payload - words) + domain_header + header + rs->data_size;
    if (domain_header) used += rs->nobjects * 4;
    unsigned nwords = tipc ? (used + 3) / 4 : (used + 16 + 3) / 4;   /* CMIF keeps alignment slack like libnx */
    if ((size_t)(words - (uint8_t *)out) + nwords * 4 > MSG_BYTES) runtime_fail("IPC response exceeds message buffer");
    if (domain_header) {
        uint32_t dh[4] = {rs->nobjects, 0, 0, 0};
        memcpy(payload, dh, sizeof dh); payload += sizeof dh;
    }
    if (tipc) memcpy(payload, &rs->result, 4);
    else { uint32_t h[4] = {CMIF_OUT_MAGIC, 0, rs->result, 0}; memcpy(payload, h, 16); }
    memcpy(payload + header, rs->data, rs->data_size);
    if (domain_header) memcpy(payload + header + rs->data_size, rs->objects, rs->nobjects * 4);
    out[0] = (rs->nxout & 0xf) << 16;
    out[1] = (nwords & 0x3ff) | (special ? 1u << 31 : 0);
    memcpy(m, out, MSG_BYTES);
}

/* ---------- Control requests (CMIF) ---------- */
static void control(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {                                                                 /* ConvertCurrentObjectToDomain */
        uint32_t id = domain_convert(s); IPC_OUT(rs, id); return;
    }
    case 3: { uint16_t n = POINTER_BUFFER_SIZE; IPC_OUT(rs, n); return; }   /* QueryPointerBufferSize */
    case 2: case 4:                                                           /* CloneCurrentObject(Ex) */
        rs->move[rs->nmove++] = runtime_ipc_new_session(s->name, s->handler, s->state, NULL);
        return;
    default:
        fprintf(stderr, "[totk-ipc] %s: control command %u not implemented\n", s->name, rq->cmd);
        if (ipc_trace_enabled())
            fprintf(stderr, "[ipc] unknown command service=%s command=%u type=%u\n",
                    s->name, rq->cmd, rq->type);
        runtime_fail("IPC control command not implemented");
    }
}

static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t read16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void dispatch(IpcSession *s, uint32_t *m) {
    IpcRequest rq; const uint32_t *words; unsigned ndata;
    parse(m, &rq, &words, &ndata);
    if (getenv("TOTK_IPC_DUMP") && rq.na + rq.nb) { fprintf(stderr, "[totk-ipc] raw %s:", s->name); for (unsigned i = 0; i < 20; i++) fprintf(stderr, " %08x", m[i]); fputc('\n', stderr); }
    IpcResponse rs; memset(&rs, 0, sizeof rs);
    if (rq.type == T_CLOSE || rq.type == T_TIPC_CLOSE) return;
    int tipc = rq.type >= T_TIPC_BASE;
    uint32_t object_id = 0;
    const uint8_t *bytes = (const uint8_t *)words, *end = bytes + ndata * 4;
    if (tipc) { rq.cmd = rq.type - T_TIPC_BASE; rq.data = bytes; rq.data_size = (uint32_t)(end - bytes); }
    else if (rq.type == T_REQUEST || rq.type == T_REQUEST_CTX || rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) {
        const uint8_t *payload = (const uint8_t *)m + (((bytes - (const uint8_t *)m) + 15) & ~15);
        if (payload + 16 > end) runtime_fail("IPC CMIF header beyond message");
        const uint8_t *d = payload;
        int domain_request = s->domain && (rq.type == T_REQUEST || rq.type == T_REQUEST_CTX);
        /* Some SDK clients talk to a freshly opened service as an implicit domain
           (the base object is id 1) without sending ConvertCurrentObjectToDomain. */
        if (!domain_request && (rq.type == T_REQUEST || rq.type == T_REQUEST_CTX) &&
            (d[0] == 1 || d[0] == 2) && payload + 32 <= end && read32(d + 16) == CMIF_IN_MAGIC) {
            domain_convert(s);
            domain_request = 1;
        }
        if (domain_request) {
            /* CmifDomainInHeader: type, num_in_objects, data_size, object_id, padding, token */
            unsigned dtype = d[0], nin = d[1];
            uint16_t dsize = read16(d + 2);
            object_id = read32(d + 4);
            if (dtype == 2) {                                              /* domain Close */
                if (!domain_remove(s->domain, object_id)) {
                    fprintf(stderr, "[totk-ipc] %s: close unknown domain object %u\n", s->name, object_id);
                    if (ipc_trace_enabled())
                        fprintf(stderr, "[ipc] unknown object service=%s object=%u command=close\n",
                                s->name, object_id);
                    rs.result = IPC_RESULT_UNKNOWN_COMMAND;
                }
                ipc_record(s, &rq, object_id, rs.result);
                respond(m, 0, 1, &rs);
                return;
            }
            if (dtype != 1) {
                fprintf(stderr, "[totk-ipc] %s: domain request type %u\n", s->name, dtype);
                runtime_fail("IPC domain request type not implemented");
            }
            if (dsize < 16 || d + 16 + dsize > end) runtime_fail("IPC domain request size");
            if (nin > IPC_MAX_OBJECTS) runtime_fail("IPC too many input objects");
            const uint8_t *objp = d + 16 + dsize;
            for (unsigned i = 0; i < nin; i++, objp += 4) {
                if (objp + 4 > end) runtime_fail("IPC input object beyond message");
                rq.objects[i] = read32(objp);
            }
            rq.nobjects = nin;
            rq.data = d + 32; rq.data_size = dsize - 16;                 /* skip CmifDomainInHeader + CmifInHeader */
            uint32_t h[4]; memcpy(h, d + 16, 16);
            if (h[0] != CMIF_IN_MAGIC) runtime_fail("IPC bad domain CMIF header");
            rq.cmd = h[2];
            IpcObject *obj = domain_find(s->domain, object_id);
            if (!obj) {
                fprintf(stderr, "[totk-ipc] %s: unknown domain object %u command %u\n", s->name, object_id, rq.cmd);
                if (ipc_trace_enabled())
                    fprintf(stderr, "[ipc] unknown object service=%s object=%u command=%u\n",
                            s->name, object_id, rq.cmd);
                rs.result = IPC_RESULT_UNKNOWN_COMMAND;
                ipc_record(s, &rq, object_id, rs.result);
                respond(m, 0, 1, &rs);
                return;
            }
            IpcSession view = *s;
            view.name = obj->name; view.handler = obj->handler; view.state = obj->state;
            view.close = NULL; view.self_id = obj->id;
            obj->handler(&view, &rq, &rs);
            ipc_record(&view, &rq, object_id, rs.result);
            respond(m, 0, 1, &rs);
            return;
        }
        if (!domain_request) {
            uint32_t h[4]; memcpy(h, payload, 16);
            if (h[0] != CMIF_IN_MAGIC) {
                fprintf(stderr, "[totk-ipc] %s: bad CMIF magic %#x type=%u domain=%d words:",
                        s->name, h[0], rq.type, s->domain != NULL);
                for (unsigned i = 0; i < 8; i++) fprintf(stderr, " %08x", payload[i * 4] | (payload[i * 4 + 1] << 8) |
                        (payload[i * 4 + 2] << 16) | ((uint32_t)payload[i * 4 + 3] << 24));
                fputc('\n', stderr);
                runtime_fail("IPC bad CMIF header");
            }
            rq.cmd = h[2]; rq.data = payload + 16; rq.data_size = payload + 16 <= end ? (uint32_t)(end - payload - 16) : 0;
        }
    } else {
        fprintf(stderr, "[totk-ipc] %s: message type %u at %p:", s->name, rq.type, (void *)m);
        for (unsigned i = 0; i < 16; i++) fprintf(stderr, " %08x", m[i]);
        fputc('\n', stderr); runtime_fail("IPC message type not implemented");
    }
    if (rq.type == T_CONTROL || rq.type == T_CONTROL_CTX) control(s, &rq, &rs);
    else s->handler(s, &rq, &rs);
    ipc_record(s, &rq, object_id, rs.result);
    respond(m, tipc, 0, &rs);
}

/* ---------- sm: service manager ---------- */
#ifdef __ANDROID__
/* First Android milestone: unknown services/commands are logged and answered
 * with result 0 so the next missing dependency is visible instead of aborting. */
static void android_stub_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static unsigned logged;
    if (logged++ < 200)
        fprintf(stderr, "[totk-ipc] stub service %s command %u (type %u, %u bytes)\n",
                s->name, rq->cmd, rq->type, rq->data_size);
    if (ipc_trace_enabled()) {
        fprintf(stderr, "[ipc] unknown command service=%s command=%u type=%u data=",
                s->name, rq->cmd, rq->type);
        for (unsigned i = 0; i * 4 + 4 <= rq->data_size; i++)
            fprintf(stderr, "%s%08x", i ? " " : "", IPC_ARG(rq, uint32_t, i * 4));
        fputc('\n', stderr);
    }
    rs->result = 0;
}
#endif
static void sm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: return;                                          /* RegisterClient / Initialize */
    case 1: {                                                /* GetServiceHandle(name) */
        char name[9] = {0}; memcpy(name, ipc_arg(rq, 0, 8), 8);
        pthread_mutex_lock(&registry);
        const Service *found = NULL;
        for (unsigned i = 0; i < nservices; i++) if (!strcmp(services[i].name, name)) found = &services[i];
        pthread_mutex_unlock(&registry);
#ifdef __ANDROID__
        if (!found) {
            fprintf(stderr, "[totk-ipc] sm: opening stub service \"%s\"\n", name);
            if (ipc_trace_enabled())
                fprintf(stderr, "[ipc] sm unknown service \"%s\" (stubbed)\n", name);
            /* Keep the requested name so later request lines name the real
               missing service instead of the generic "stub". */
            char *copy = strdup(name);
            rs->move[rs->nmove++] = runtime_ipc_new_session(copy ? copy : "stub", android_stub_service, NULL, NULL);
            return;
        }
#else
        if (!found) { fprintf(stderr, "[totk-ipc] sm: service \"%s\" not implemented\n", name); runtime_trace_guest(trace_fp, trace_lr); runtime_fail("IPC service not implemented"); }
#endif
        rs->move[rs->nmove++] = runtime_ipc_new_session(found->name, found->handler, found->open ? found->open() : NULL, found->close);
        fprintf(stderr, "[totk-ipc] opened service %s\n", found->name);
        if (ipc_trace_enabled()) fprintf(stderr, "[ipc] sm opened service=%s\n", found->name);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- SVCs ---------- */
int runtime_ipc_svc(unsigned num, uint64_t *r) {
    switch (num) {
    case 0x1f: {                                             /* ConnectToNamedPort(x1 name) -> w1 */
        char name[13] = {0}; memcpy(name, (const void *)(uintptr_t)r[1], 12);
        if (strcmp(name, "sm:")) { fprintf(stderr, "[totk-ipc] named port \"%s\"\n", name); runtime_fail("IPC named port not implemented"); }
        r[0] = 0; r[1] = runtime_ipc_new_session("sm:", sm, NULL, NULL); return 1;
    }
    case 0x21: case 0x22: {                                  /* SendSyncRequest(w0) / WithUserBuffer(x0, x1, w2) */
        uint32_t h = (uint32_t)(num == 0x21 ? r[0] : r[2]);
        IpcSession *s = runtime_handle_get(h, 1);
        if (!s) { r[0] = 0xe401; return 1; }
        uint32_t *m = num == 0x21 ? pthread_getspecific(runtime_keys[2]) : (uint32_t *)(uintptr_t)r[0];
        if (!m || (num == 0x22 && r[1] < MSG_BYTES)) runtime_fail("IPC message buffer");
        trace_fp = r[29]; trace_lr = r[30];
        dispatch(s, m); r[0] = 0; return 1;
    }
    case 0x15: {                                             /* CreateTransferMemory(x1 addr, x2 size, w3 perm) -> w1 */
        IpcTransferMemory *t = malloc(sizeof *t);
        if (!t) runtime_fail("transfer memory allocation");
        *t = (IpcTransferMemory){r[1], r[2], (uint32_t)r[3]};
        r[0] = 0; r[1] = runtime_handle_new(0, t); return 1;
    }
    default: return 0;
    }
}
