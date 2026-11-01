# Android Gate 3 status and known boundaries

Date: 2026-11-01  
Main: `f554d85` (`merge: L2 command-buffer accounting`)  
Device: 2721dfe4, Adreno 830, `com.totk.android`

## 1. Summary

The Android host rendering stack is now wired end to end: NVN command replay,
compute/copy/clear, render-target association, FIFO UI drain, per-program UBO
history, the window pass, and present-target routing all execute. Gates 1 and 2
pass in the **empty-save/loading** state.

Gate 3 (a visible title/save-slot UI, or a real 3D frame, through the window) is
**not reached**. The remaining gap is not a dropped host command: in every state
reached, the guest records no producer for the UI masks/font atlas, and the one
state that does record a large UI batch records it **queued and never submits
it**.

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
| 3. 1920x1080 window multi-colour + title/save UI | **fail** | window PPM `1 colour (black)`; 0x3f is the fade overlay; no UI composite |
| 4. A/Continue into 3D | **fail** | opening-field phase reaches 180x112/720x450/1920x1080 0x7a, but the window presents the offscreen fade target only |
| 5. 30 min / 10k presents stable | not run to gate | longest observed run 9,697 presents without a crash |

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

### 4.4 No producer for the UI masks / font atlas in the reached states

The two masked passes seen in the save/loading state sample:

- `2048x4096` R8 (`0x366483df0`), `first_nz=none`, 24 sample binds, `rt_binds=0`,
  `dirty=0`, 0 CPU writes;
- `780x60` A2B10G10R10 (`0x336d93668` and aliases), `first_nz=none`.

No command buffer contains a Copy/Clear/Dispatch/Blit or a draw targeting either
mask. The accounting run is decisive:

```text
recorded=14092 finalized=14092 submitted=1200 retired=6592 call_cmds=491
unsubmitted=12891 | draws_buf=3734 gpu_buf=0 mask_buf=0
```

The 91 % "unsubmitted" buffers are the engine's normal small buffers replayed
through `CallCommands`; every one has `mask_candidate=0`.

A temporary `tex-write` probe (patch: `/tmp/gate3-texwrite-trace.patch`) shows
what the 51 `nvnTextureWriteTexels` calls actually write: only 4x1/4x4 RGBA8
(`first_nz=3/none`), never the 2048x4096 or 780x60 masks.

### 4.5 The font pipeline is present, but not producing

The guest contains `nn::font::ScalableFontEngine`, `nn::font::RectDrawer`, and
`sead/nvn_font` resources, and opens
`Font/Font.Nin_NX_NVN.bfarc.zs` (5.6 MB). The 2048x4096 R8 atlas and the 780x60
text textures are allocated, but nothing fills them in the states reached. The
font rasterizer is either gated behind the title/save UI state or uses a path
that does not appear in the recording (no BindImage/Dispatch/Copy/DrawTexture).

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

## 5. App shell (parallel work)

The uncommitted Android app shell is isolated in
`/Users/oasis/.local/share/opencode/worktree/9bf67f/app-tls` (branch
`app/tls-shell`), with an independent subagent working on it.

- APK builds/installs; the three guest images are packed as
  `lib<name>_patched.so` so the loader can `mmap(PROT_EXEC)` them from
  `/data/app/.../lib/arm64` (external FUSE denies executable mappings).
- The real Surface attaches at 1440x3200 / 3200x1440.
- Blocker: a dlopened shared library uses global-dynamic TLS, so
  `tpidr_el0 + fixed offset` differs per thread (`per-thread TLS offset
  changed`). `-ftls-model=initial-exec` makes Android's `dlopen` reject the
  library. The current experiment stores `HostContext`/`GuestSlots` in two
  `pthread_key_t` slots (`TP + key*8`), with a TCB-slot verification.
- The same TLS experiment must not reach the CLI: a CLI built with it crashed
  at boot (`sig=11`, `guest thread handle=0`). The CLI path must stay on the
  committed loader, or the key-slot path must be guarded by
  `TOTK_ANDROID_LIBRARY`.

## 6. Remaining options

1. **Verify the 8,427-draw drain** (recommended host-side lead). Use the
   Phive-timed sequence after boot (~100-150 s) and the current main build:
   `25:A:400,40:PLUS:400,55:A:400,70:A:400,90:PLUS:400,120:A:400,150:A:400`,
   ~300 s. Check `[totk-frame] segs ... draws=8427 ... queued=` and
   `rt 400x225/64x64 fmt=0x3f`, then capture those targets' PPMs.
2. **Title/save state machine / asset trigger.** The masks are not a missing
   host command in the reached states; the title flow (EventFlow/Phive/save
   metadata) must advance before the UI producer runs. Compare a 1.0.0 save and
   a new-game flow; the 1.4.3 update `CaveBounding` assets are still absent.
3. **Font atlas path.** If a future state starts text rendering, trace the
   `nn::font` rasterizer's output path (storage image/BindImage, Dispatch,
   Copy, or CPU writes) before implementing any producer.
4. **Accept the boundary.** Keep the CLI/offscreen evidence and the host fixes;
   document Gate 3 as blocked by the guest title/UI state, not the renderer.

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

Evidence:

- `/tmp/totk-lane2/evidence/cbacct.log` — decisive command-buffer accounting
- `/tmp/totk-lane4/gate34/REPORT-gate34.md` — 0x7a fade/mask evidence
- `/tmp/totk-lane4/framegraph/REPORT-framegraph.md` — 360x69 closed loop
- `/tmp/totk-lane4/phive/REPORT-phive.md` — 8,427-draw queued UI batches
- `/tmp/gate3-win1500.ppm` — clean-HEAD window PPM, 1 colour black
- `/tmp/gate3-texwrite-trace.patch` — temporary CPU-write probe
