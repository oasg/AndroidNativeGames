# Android Gate 3 status and known boundaries

Date: 2026-11-02  
Main: `7715d9b` (`threads/android: stop virtual-core starvation on host waits and SVCs`)  
Device: 2721dfe4, Adreno 830, `com.totk.android`

## 1. Summary

The Android host rendering stack is now wired end to end: NVN command replay,
compute/copy/clear, render-target association, FIFO UI drain, per-program UBO
history, the window pass, and present-target routing all execute. Gates 1 and 2
pass in the **empty-save/loading** state.

Gate 3's host-side visible-UI evidence is now reached: the 1920x1080 present
target contains the 360x69 UI (587 colours, bbox (52,959)-(411,1027), 24,840
non-background pixels). The real Android Surface/app window and the 3D field are
still blocked by the device lock / HLE deadlock and the separate 8,427-batch
state.

The 2048x4096 font atlas **is** written by the guest through plain memory (not
the NVN texel API); the host's cached copy is now reloaded by checksum. The text
pass's per-draw vertex buffers are recycled before replay; per-draw snapshots
now fix them. Window/present routing now follows the RT the sampler reads. The
one state that records a large UI batch still records it **queued**.

## 2. Delivered host fixes

| Lane | Delivered |
| --- | --- |
| L1 | Vulkan compute, texture copy/clear/upload/download, BC4 block regions, BC4 sampled-draw probe |
| L2 | NVN dispatch/copy/clear replay, FIFO scene queue, UI chain drain, UBO final-state + per-program history, view-aware RTs, window pass/present, indexed vertex range, CB accounting |
| L3 | Async preemption hardening, stall diagnostics, `BufferBlock` reflection fix, zero-UID save mount |
| L4 | Acceptance runs, PPM/frame analysis, EventFlow/Phive extraction and push, frame graph |
| L5 | Shader cache audit, dump/translate docs, missing-reflection audit |

Merged commits include `54e9bd4`, `77c8d57`, `20e199e`, `3b415f3`, `1d54170`,
`e34bf23`, `800f804`, `197fb5f`, `5be0876`, `f554d85` (L2), plus the L1/L3/L5
merge commits.

## 3. Acceptance matrix

| Gate | Status | Evidence |
| --- | --- | --- |
| 1. 360x69 UI source `first_nz != none` | **pass only in the no-save/loading state** | empty-save run: 42/64/146-colour 43x5/67x5 loading strip; real-save runs record no 360x69 at all |
| 2. 0x3f non-zero | pass, but it is the **clear/fade colour** | `a68a565f` decodes to B10G11R11 `(0.0498, 0.0791, 0.1191)` = the pass clear |
| 3. 1920x1080 window multi-colour + title/save UI | **host-side pass; app real Surface content pending** | `wip.ppm` 1920x1080: 587 colours, bg tan `a68a56`, 360x69 UI bbox (52,959)-(411,1027), 24,840 non-bg; app real Surface now presents 16,104/22,002 frames but shows the host clear `(13,20,31)` + status overlay (content path in progress) |
| 4. A/Continue into 3D | **fail** | opening-field phase reaches 180x112/720x450/1920x1080 0x7a; the 8,427-draw UI/field batches still never execute; no 3D frame |
| 5. 30 min / 10k presents stable | **partial** | app real Surface 16,104 presents (22,002 in a later run), zero `[totk-stall]` across 4-5 minute runs; 30-minute run not yet done |

## 4. Key findings

### 4.1 The 360x69 chain is the no-save UI

With the L3 mount fix and a real save, 120 s runs record **no 360x69 target**;
the UI is 400x225 / 100x56 / 200x112 before input and 180x112 / 720x450 after
(the opening-field load). The 360x69 chain appears 70,909 times only with an
empty/invisible save. It is a loading strip, not the title/save-slot UI.

### 4.2 0x3f is a six-instruction fade overlay

The only 1920x1080 pass into the `0x3f` window texture is PS `31754aa6…`, which
writes `c3[0][0]` verbatim and samples no texture. It is a constant-colour
fade/flash. The window pass faithfully samples it; it cannot show UI because the
UI was never composited into 0x3f.

### 4.3 The window pass and present path work

After the depth-free window pipeline and layout matching:

```text
[totk-frame] window pass layout ... presenting through the window format
[totk-frame] composite window target=window 1920x1080 program=0x340c69798 draws=6 nui=0
```

The window is no longer `target=offscreen`. The present target can be routed to
the last large scene RT. The window PPM is still all black because its source
(the fade/clear or the zero-mask UI composite) is black.

### 4.4 The font atlas is guest-written directly; the host cache was stale

The two masked passes seen in the save/loading state sample:

- `2048x4096` R8 (`0x366483df0`) — the font/glyph atlas;
- `780x60` A2B10G10R10 (`0x336d93668` and aliases) — still `first_nz=none`.

The old `first_nz` probe capped at 1 MiB. Sampling the **whole 8 MiB** storage
shows the atlas is not empty: the first nonzero byte is at ~7.7 MiB
(`first=7766080`, `nz=223/131072` sampled bytes), and the checksum changes over
time (`d865… → 890d… → a20d… → d4f1… → c992…`). The guest writes the atlas
through **plain memory**, bypassing every NVN texel API, which is why the API
counters, `rt_binds`, and the 1 MiB probe all said "none".

The host cached the first upload and never rebuilt it. A checksum-based reload
in `nvn_texture_gfx_ready_locked()` (Lane 2, patch
`/tmp/gate3-atlas-reload.patch`) detects the change, drops the cached
`GfxTexture`, and re-uploads. It fires five times in a 300 s run
(`[totk-nvn] mask reload … sum=…`).

The 780x60 A2B10G10R10 textures remain all zero across the whole run; their
producer is still unknown.

The CB accounting still shows no Copy/Clear/Dispatch/Blit producer, which is
consistent with direct guest writes:

```text
recorded=14092 finalized=14092 submitted=1200 retired=6592 call_cmds=491
unsubmitted=12891 | draws_buf=3734 gpu_buf=0 mask_buf=0
```

The 91 % "unsubmitted" buffers are the engine's normal small buffers replayed
through `CallCommands`; every one has `mask_candidate=0`. A temporary
`tex-write` probe confirms the `nvnTextureWriteTexels` calls (51 in gate34, 93
in a 300 s run) only write small 4x1/4x4/16x16/128x32/32x32 textures, never the
atlas.

### 4.5 The font pipeline is present

The guest contains `nn::font::ScalableFontEngine`, `nn::font::RectDrawer`, and
`sead/nvn_font` resources, and opens
`Font/Font.Nin_NX_NVN.bfarc.zs` (5.6 MB). The 2048x4096 R8 atlas is written
directly by the guest (4.4); the 780x60 text textures remain zero. The text
pass `8a20741c` runs and samples the atlas, but its target is still zero because
the per-draw vertex buffers are recycled (4.7).

### 4.6 The one large UI batch is recorded but never executes

In the Phive opening-field phase (after the Phive assets were pushed), the UI
producers are alive (grey 73x7 loading strip, yellow 50x50 box, red `#7d0000`
fill), but the large UI batches are queued:

```text
segs 2   cmd=[30,128)   draws=8427 tex=... fmt=0x3f 400x225 ... queued=1 nui=0
segs 699 cmd=[5776,5871) draws=8427 tex=... fmt=0x3f 64x64 ... queued=1 nui=0
```

No `rt 400x225 fmt=0x3f` or `rt 64x64 fmt=0x3f` scene step runs. This is the
most promising host-side lead: L2 added a scaled deep-backlog drain after the
Phive report, but that phase has not been re-run with the new drain.

A 300 s re-run with the input moved after boot
(`150:A:400,180:PLUS:400,210:A:400,240:A:400,270:PLUS:400`) reached the same
state and reproduced it exactly:

```text
[totk-frame] segs  2 cmd=[30,128) draws=8427 tex=... fmt=0x3f 400x225 ... queued=1 nui=0
[totk-frame] segs 699 cmd=[5776,5871) draws=8427 tex=... fmt=0x3f 64x64 ... queued=1 nui=0
```

No `rt 400x225/64x64 fmt=0x3f` executed; the `seg2-400x225-*` PPM is 1 colour
black, and the window PPM is the host clear `(13,20,31)/(13,20,30)`.

### 4.7 The text pass vertex buffers are recycled before replay

The text pass `ps=8a20741c…` (1920x1080, samples the 2048x4096 R8 atlas + a
96x96 BC4 atlas) still produces a zero target after the atlas reload. Its
replay-time vertex stream is degenerate:

```text
[totk-frame] composite-3f   vb 0 addr=0x3670fe000 size=112 first=00000000 00000000 00000000 ffffffff
```

The position attribute is at offset 0, so all four vertices are at the origin.
The guest recycles the VB between recording and the queued replay.
`nvn_cb_snapshot_indices()` already deep-copies index bytes into
`cmd->u.draw_indexed.index_data`, but there is no vertex-buffer equivalent, and
the segment replay uses one bind state (the last draw) for all 318 text draws.
Fixed in `a5ddaac` (merged `3f05ae9`): small per-draw vertex streams are
snapshotted at record time and replayed per draw. The 1920x1080 text target now
has 587 colours (background `a68a56`, bbox (52,959)-(411,1027)).

### 4.8 Window/present routing

The window pass binds `0x336d9ad68` (0x3f, 1920x1080). Its bridge starts with
no RT and empty guest storage (`src=guest`), then the queued 1920x1080 text
segment associates the RT (`tex-rt ... gen=1`); the window sampler rebuilds
from it (`src=rt-gen`, `first_nz=0 center=a68a565f`). `nvn_window_present_rt()`
now routes the present to the RT the sampler actually reads, with an alias
fallback and `g_last_scene_rt` as fallback; the headless L2 dump follows the
present target.

Verified:

- `wiwin-500/1000/1500.ppm`: 25/73/13 colours, tan background, text bbox.
- `wip.ppm` (present target): 587 colours, tan background, 360x69 UI bbox
  (52,959)-(411,1027), 24,840 non-background pixels.
- `[totk-vk] present #... colors=1` is the colour-attachment count, not a
  pixel-colour count; the content evidence is the present PPM / `first_nz`.

The real-surface branch of `native_window_android.c` already presents
`present_target`. The headless no-surface branch and
`runtime_native_window_screenshot()` still use `w->gfx_target` (the overlay), so
the raw CLI `present #`/screenshot stays the host clear. Minimal app-side
change in both places:

```c
GfxOffscreenTarget *t = w->present_target ? w->present_target : w->gfx_target;
```

## 5. App shell (real Surface)

The app shell lives in
`/Users/oasis/.local/share/opencode/worktree/9bf67f/app-tls` (branch
`app/tls-shell`). Key commits: `d93d565` (TLS), `3270628` (main renderer merge),
`cec9be4` (lazy account UID), `539b07a1` (data/crash diagnostics), `e0a5d7a`
(RT cache), `88f4904` (Lane 3 core deadlock).

- **TLS**: bionic `pthread_key_create` returns `0x80000000 | n`; the slot base
  is thread/layout dependent. The loader discovers both slots once by scanning
  the thread's TLS mapping and verifies the same offsets on every thread.
  Measured `off=0x198` / `0x1a8`; boot reaches `mapped all` → constructors →
  `ENTER nnMain`.
- **Guest images**: packed as `lib<name>_patched.so`, mapped PROT_EXEC from
  `/data/app/.../lib/arm64` (external FUSE denies executable mappings).
- **Data ownership crash (root cause of the null VI/account object)**: the app
  UID changed on reinstall (`10340`/`u0_a340`), while the external
  `files/romfs`/`profile` dirs were still owned by the previous UID
  (`10280`/`u0_a280`, group `ext_data_rw` = 1078). The app was not in that
  group, so Scoped Storage denied traversal and the guest got a null heap in the
  boot resource loader (`main+0x1123f7c`, `strb wzr,[x0,#8]`). The data was
  recreated as readable copies (`romfs`, `profile-rw`, `shader-cache-rw`,
  `game`), `chmod -R a+rX` (`a+rwX` for profile/cache), and the save backed up
  to `/data/local/tmp/totk-save-backup/save`.
- **App shell diagnostics**: always-on guest crash stack, system-property traces
  (`debug.totk.*`), `MountRom` unreadable-root reporting, and a
  `profile-rw`/`shader-cache-rw` fallback in `MainActivity`.
- **Render-target cache**: the boot logo creates a new guest target each frame;
  the 128-entry RT cache had no eviction. Added LRU eviction/reuse (never the
  current present/scene RT; `nvn_tex_forget_rt` clears sampled copies). Headless
  boot: `creates=1984 live=128 evictions=1856`, zero `cache full`. App: 453
  presents past the logo, zero `cache full`.
- **Virtual-core deadlock**: Android `services/` never called
  `runtime_core_checkpoint()` at SVCs, and the LightMessageQueue lock loop uses
  SVC `0x36` (`SynchronizePreemptionState`). Added the checkpoint/SVC `0x36`,
  unblocked SIGUSR2 for loader/guest threads, allowed the SIGUSR2 handler to
  switch a core stopped at a blocking futex/`clock_nanosleep`, released cores
  around blocking fs/romfs/fsp reads, and split `g_prog_lock` around shader
  cache/pipeline creation. Real app: `startup logo finished`, 16,104 real
  swapchain `surface target present #` (22,002 in a later run), **zero
  `[totk-stall]`** across 4–5 minute runs. Screenshot
  `/tmp/lane3g-shot-final.png`.
- **Current content gap**: the real Surface still shows the host clear
  `(13,20,31)` plus the status overlay, not the title/save UI. The app logs
  `nvnCommandBufferBindStorageBuffer: invalid binding` at the logo→title
  transition and the RT churn continues. `wip.ppm` (the host-side present
  target) already contains the 587-colour/360x69 UI; the remaining work is
  mapping that RT into the real-surface present (Lane 2).
- **Main integration**: RT cache `ce57f74`, Lane 3 shared runtime
  `7715d9b`; the app shell commits stay on `app/tls-shell`.
- Device: unlocked; data readable; save backed up. The display times out after
  ~70 s and destroys the Surface, so keep the screen on for long runs.

## 6. Remaining options

1. **Real-Surface content (Lane 2, in progress).** The app now presents 16k+
   frames to the real swapchain with zero stalls, but the Surface is the host
   clear `(13,20,31)` + status overlay, not the title/save UI. The logo→title
   transition logs `nvnCommandBufferBindStorageBuffer: invalid binding` and RT
   churn continues. Trace the invalid binding (stage/index/handle/size) and the
   window present RT vs the sampler RT; `wip.ppm` already has the 587-colour /
   360x69 UI. This is the last host-renderer step for the app.
2. **8,427-draw / opening-field 3D.** The 400x225/64x64 `0x3f` UI batches still
   show `queued=1` in the Phive/opening-field state; the 3D field has not been
   reached. This is the remaining Gate 4 item, not the text/UI path.
3. **Stability.** Run 30 min on the real Surface (16,104/22,002 presents already
   reached with zero stalls). The display times out after ~70 s and destroys the
   Surface; add `FLAG_KEEP_SCREEN_ON` or keep the screen on for the long run.
4. **App data/UID.** The external data is now readable copies
   (`romfs`/`profile-rw`/`shader-cache-rw`/`game`); the save is backed up at
   `/data/local/tmp/totk-save-backup/save`. Keep the data owner aligned with the
   app UID on reinstall; otherwise Scoped Storage denies romfs and the guest
   crashes with the null resource heap.
5. **Accept the boundary.** The host-side Gate 3 UI evidence is met (`wip.ppm`,
   587 colours, 360x69 UI bbox); the app now runs/presents (Lane 3), and the
   Surface content + 3D field remain open.

## 7. Reproduce

```sh
cd /Users/oasis/code/MacNativeGames
WORK=gamebin/TOTK_build/totk games/totk/android/build.sh

# device data (already present)
# /data/local/tmp/totk-android/{main,sdk,subsdk0}.patched.bin
# /data/local/tmp/totk-android/romfs-min/totk-romfs-min
# /data/local/tmp/totk-android/lanes/shader/shader-cache
# /data/local/tmp/totk-android/user-data

TOTK_ANDROID_INITS=1 TOTK_ANDROID_RUN_NNMAIN=1 \
TOTK_ANDROID_ACCOUNT_UID=f9cdd55b6d4c70efc1c8e47378dad985 \
TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min/totk-romfs-min \
TOTK_ANDROID_SHADER_CACHE=/data/local/tmp/totk-android/lanes/shader/shader-cache \
TOTK_ANDROID_INPUT_SCRIPT='25:A:400,40:PLUS:400,55:A:400,70:A:400,90:PLUS:400,120:A:400,150:A:400' \
TOTK_ANDROID_TRACE_SCENE_STEPS=1 TOTK_ANDROID_TRACE_COMPOSITE=1 \
  ./totk-android /data/local/tmp/totk-android boot-sdk
```

The text/UI case uses the later input script and the L2 frame dump:

```sh
TOTK_ANDROID_INPUT_SCRIPT='150:A:400,180:PLUS:400,210:A:400,240:A:400' \
TOTK_ANDROID_L2_FRAME=/data/local/tmp/totk-lane2/win.ppm \
TOTK_ANDROID_L2_FRAME_EVERY=500
```

Evidence:

- `/tmp/gate3-wip.ppm` — present target (1920x1080), 587 colours, tan bg,
  360x69 UI bbox (52,959)-(411,1027), 24,840 non-bg
- `/tmp/gate3-wiwin1000.ppm` — window L2 dump, 73 colours, tan bg
- `/data/local/tmp/totk-lane2/final.log`, `wip.ppm`, `wiwin-*.ppm`
- `/tmp/totk-lane2/evidence/cbacct.log` — decisive command-buffer accounting
- `/tmp/totk-lane4/gate34/REPORT-gate34.md` — 0x7a fade/mask evidence
- `/tmp/totk-lane4/framegraph/REPORT-framegraph.md` — 360x69 closed loop
- `/tmp/totk-lane4/phive/REPORT-phive.md` — 8,427-draw queued UI batches
- `/tmp/gate3-texwrite-trace.patch` — temporary CPU-write probe
