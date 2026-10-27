/* Android host audio mixer backend.
 *
 * The renderer service in runtime/audio_renderer.c owns the nn::audio wire
 * protocol, voice state and frame timing.  This backend keeps that path alive
 * without a platform audio device: it consumes the same server state and
 * produces silence.  It is deliberately a no-op so the audren:u service can
 * initialize on devices/emulators where a CoreAudio/AAudio backend is not
 * available yet; a real AAudio sink can replace these functions without
 * changing the renderer protocol. */
#include "audio_mixer.h"
#include "audio_renderer.h"
#include "ipc.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static _Atomic float device_volume = 1.0f;
static const char device_name[] = "AudioTvOutput";

static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    if (!b->size) return;
    char *dst = ipc_guest(b->addr, b->size);
    size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size);
    memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, b->index};
}

static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}

void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    int pointer;
    switch (rq->cmd) {
    case 0: case 6: {
        IpcBuffer b = out_buffer(rq, &pointer);
        int32_t n = b.size >= 0x100 ? 1 : 0;
        if (n) put_name(&b, rs, pointer);
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 7: atomic_store(&device_volume, IPC_ARG(rq, float, 0)); return;
    case 2: case 8: { float v = atomic_load(&device_volume); IPC_OUT(rs, v); return; }
    case 3: case 10: { IpcBuffer b = out_buffer(rq, &pointer); put_name(&b, rs, pointer); return; }
    case 5: { int32_t n = 2; IPC_OUT(rs, n); return; }
    case 13: { IpcBuffer b = out_buffer(rq, &pointer); put_name(&b, rs, pointer); return; }
    case 14: {
        IpcBuffer b = out_buffer(rq, &pointer);
        int32_t n = b.size >= 0x100 ? 1 : 0;
        if (n) put_name(&b, rs, pointer);
        IPC_OUT(rs, n);
        return;
    }
    case 4: case 11: case 12: {
        static uint32_t ev;
        if (!ev) ev = runtime_event_new();
        rs->copy[rs->ncopy++] = runtime_event_readable(ev);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

void audio_mixer_open(AudioRenderer *r) {
    if (r) r->mixer = NULL;
}

void audio_mixer_set_running(AudioRenderer *r, int running) {
    (void)r;
    (void)running;
}

void audio_mixer_close(AudioRenderer *r) {
    if (r) r->mixer = NULL;
}

void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters,
                                uint32_t splitter_bytes, const uint8_t *mixes,
                                uint32_t mix_bytes, int mix_count) {
    (void)r;
    (void)splitters;
    (void)splitter_bytes;
    (void)mixes;
    (void)mix_bytes;
    (void)mix_count;
}

void audio_mixer_update_effects(AudioRenderer *r) {
    (void)r;
}

void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size) {
    (void)r;
    (void)id;
    if (output && size) memset(output, 0, size);
}

uint32_t audio_mixer_sink_written(AudioRenderer *r, int id) {
    (void)r;
    (void)id;
    return 0;
}

void audio_mixer_render(AudioRenderer *r, double seconds) {
    (void)r;
    (void)seconds;
}

int audio_mixer_read(AudioRenderer *r, float *lr, int frames) {
    (void)r;
    if (lr && frames > 0) memset(lr, 0, (size_t)frames * 2 * sizeof(float));
    return frames;
}
