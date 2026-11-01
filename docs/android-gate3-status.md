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
what `nvnTextureWriteTexels` actually writes. In the gate34 state the counter is
51; in a 300 s run it reaches 93. The targets are small: 4x1/4x4 RGBA8, 16x16
RGBA8, 128x32 R8 and 32x32 R8 mips. **No write targets the 2048x4096 or 780x60
masks.** The text pass `8a20741c` runs and samples the 2048x4096 mask, but its
`gfx` is a zero upload (`rt=0`, `dirty=0`).

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

A 300 s re-run with the input moved after boot
(`150:A:400,180:PLUS:400,210:A:400,240:A:400,270:PLUS:400`) reached the same
state and reproduced it exactly:

```text
[totk-frame] segs  2 cmd=[30,128) draws=8427 tex=... fmt=0x3f 400x225 ... queued=1 nui=0
[totk-frame] segs 699 cmd=[5776,5871) draws=8427 tex=... fmt=0x3f 64x64 ... queued=1 nui=0
```

No `rt 400x225/64x64 fmt=0x3f` executed; the `seg2-400x225-*` PPM is 1 colour
black, and the window PPM is the host clear `(13,20,31)/(13,20,30)`. The
2048x4096 font atlas is still zero at every bind, so even a drained batch would
draw black text until the atlas producer runs.

## 5. App shell (parallel work)

The app shell is isolated in
`/Users/oasis/.local/share/opencode/worktree/9bf67f/app-tls` (branch
`app/tls-shell`, commit `d93d565`). The TLS blocker is solved:

- APK builds/installs; the three guest images are packed as
  `lib<name>_patched.so` so the loader can `mmap(PROT_EXEC)` them from
  `/data/app/.../lib/arm64` (external FUSE denies executable mappings).
- Bionic `pthread_key_create` returns `0x80000000 | n`, stores the value in a
  16-byte record, and the TLS-slot base is thread/layout dependent. The fix
  scans the thread's TLS mapping (from `/proc/self/maps`) for the pointers just
  stored, rejecting the 64-byte-aligned `__thread` copies, and verifies the same
  offsets on every later thread.
- Measured: `tls tp=... host_key=-2147483627 off=0x198 ... guest_key=-2147483626
  off=0x1a8 ...`; boot reaches `mapped all`, constructors, `ENTER nnMain`, save
  mount (zero-UID adoption picks `f9cd…`), `vi host native window 1920x1080`,
  and 808 presents.
- CLI probes PASS with the same fix (`probe-vulkan`, `probe-nvn-state`,
  `probe-nvn-copy`, `boot-sdk` loader-ready).
- The real Surface only received 3 presents: the display was OFF and the MIUI
  keyguard owned focus (`surfaceDestroyed` at 22:02:42), so the renderer stayed
  offscreen. This is environmental, not a code path: `input keyevent` is denied
  on this ROM, `/dev/input` is SELinux-denied, and MIUI denies `showWhenLocked`
  to `com.totk.android`.
- Next blocker after TLS: an HLE lock/futex deadlock. Both cores were held for
  127 s; the owner is a host frame in `libc.so+0xa6020` while holding the core
  lock (`guest=0`), with waiters at `sdk+0xde1888` (later
  `nn::os::TryPeekLightMessageQueue+0x80`).

### Device recovery needed (human unlock)

A reboot to escape the display-off/keyguard loop left the device in Direct Boot
with a lock credential. Activities no longer resolve, `adb install` is blocked,
and a failed reinstall removed `com.totk.android`; the 6.6 GB app external data
was removed with it. CLI data under `/data/local/tmp/totk-android` is intact.

1. Unlock the phone once (PIN/pattern/fingerprint).
2. `adb install -r --no-incremental \
   /Users/oasis/.local/share/opencode/worktree/9bf67f/app-tls/games/totk/android/app/build/totk-android.apk`
3. Re-create `/sdcard/Android/data/com.totk.android/files/{game,romfs,profile,shader-cache}`
   (the 6.6 GB romfs copy was deleted).
4. `adb shell svc power stayon true` after unlocking, then launch with
   `--es account_uid f9cdd55b6d4c70efc1c8e47378dad985`.

## 6. Remaining options

1. **Font atlas producer (the root blocker for text/UI).** The 2048x4096 R8
   mask is sampled by the text pass but never written by any recorded CPU/RT/
   copy/dispatch/image path. The `nn::font` rasterizer is either gated behind
   the title/text state or uses an API the recording does not show. Find that
   path before touching the queue or the blend state; without it the text draws
   are black even if they execute.
2. **8,427-draw drain (necessary but not sufficient).** The 300 s late-input
   run still shows `queued=1` for the 400x225/64x64 `0x3f` batches, and the
   400x225 PPM is black. Re-test the drain only after (1) is fixed, otherwise a
   drained batch still samples a zero atlas.
3. **App-shell HLE deadlock.** With TLS solved, the app boots to `nnMain` +
   save mount + a 1920x1080 window, then deadlocks holding a core lock in
   `libc.so+0xa6020` with waiters at `sdk+0xde1888`. This needs the device
   unlocked first; then instrument the HLE lock/futex owner and the
   `TryPeekLightMessageQueue` waiter.
4. **Accept the boundary.** The host fixes, the app TLS fix, and this report are
   preserved; Gate 3 is recorded as blocked by the guest title/UI state and the
   font atlas producer, not by the renderer or present path.

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
