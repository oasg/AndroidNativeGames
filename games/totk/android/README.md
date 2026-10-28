# Android/arm64 host (first milestone)

This directory contains the first native Android host for the rewritten TOTK
1.0.0 modules. It is deliberately small: loader + key page + relocation + host
gate + HLE-stack thunks + a minimal SVC layer.

## What works

- Maps `main`, `sdk`, `subsdk0` from `rewritten/android-v0` with SHA-256 checks.
- Fills the Android key page (`KCTX`, `KGATE`) and guest TLS/TLR slots.
- Resolves RELR/RELA and cross-module imports.
- Runs the host gate on a real arm64 Android device:
  `main` PLT → `sdk` timer code → `gate_android.S` → `runtime_dispatch` → back.
- Runs `nn::init::Start` through `nn::os` initialization and reaches the
  `loader-ready` callback.
- Implements CMIF domain sessions (`ConvertCurrentObjectToDomain`, domain
  request/response headers, domain object tables and implicit domain object 1).
- Registers a real `lm` service. File I/O uses the macOS-style `nn::fs`
  host overrides (`fs_host_android.c`, `romfs_android.c`); `fsp-srv` remains
  registered as an optional/diagnostic protocol path.
- Runs `main`, `subsdk0` and `sdk` `DT_INIT` and reaches the original
  `nnMain` entry with `TOTK_ANDROID_INITS=1`; the process stops there by
  default. `TOTK_ANDROID_RUN_NNMAIN=1` lets the original `nnMain` continue.
- On 1.0.0 the guest `DT_INIT` state is 1 (no fini callback), which makes the
  guest skip its `.init_array` loop. The loader runs the constructor pass on
  the guest stack itself (`TOTK_ANDROID_CTOR_PROBE=1` prints the count). See
  `docs/android-hle-phase1.md`.
- Guest entry points run on a real guest stack; each thread has its own 8 KiB
  TLS, ThreadLocalRegion and ThreadType, so `GetCurrentStackInfo` and
  `GetTlsValue`/`SetTlsValue` see the layout the SDK expects. The guest
  `ThreadType+0x1b0` is filled with the guest thread handle, which
  `nn::os::UnlockMutex` uses for its owner check.
- `nvnBootstrapLoader`/`nvnDeviceGetProcAddress` and the full NVN object graph
  run on the host (`nvn_android.c`): device, memory pool, buffer, texture and
  texture view, sampler and sampler pool, texture pool, program, queue, window,
  sync and event. Builders write real fields, mappings return the backing
  storage, handles are non-zero, and unknown object APIs are reported instead
  of silently succeeding.  The NVN object layer remains the source of truth;
  `nvnQueueSubmitCommands` keeps the guest submission order.  See
  `docs/android-hle-phase2a.md`.
- `nn::vi` uses the SDK display/initialize/vsync path, with host layer/native
  window objects for `CreateLayer`/`GetNativeWindow`.
- `nvdrv` is a real host service (`nvdrv_android.c`): Open/Close/Initialize/
  SetAruid/Ioctl3/MapSharedMem, nvmap create/alloc/free/id/param, nvhost-ctrl
  syncpoints, and a guest-visible 1 GiB nvmap window at `0x500000000` that
  `QueryMemory` reports as committed.  Unknown devices/ioctls return explicit
  errors.  `probe-nvdrv` covers the
  protocol.
- `nn::vi` has a host layer + offscreen `NativeWindow` (1920x1080 RGBA8,
  1280x720 in handheld); `probe-vi` checks the object and the NVN builder
  round trip.  `probe-nvn-submit` records one command buffer, submits it and
  presents through the selected graphics backend.
- The Android graphics backend is Vulkan (`gfx_vulkan_android.c`): real
  `VkInstance`/`VkDevice`/graphics queue, an offscreen `VkImage` target with
  GPU clear and readback, and an optional `VkSwapchainKHR` over an
  `ANativeWindow*`.  `TOTK_ANDROID_GFX_BACKEND=auto|vulkan|software` selects
  the path; forced Vulkan never falls back silently.  See
  `docs/android-hle-phase2b.md`.
- Shader spike 2B.2: the existing Ryujinx `totk_xlate` translator already emits
  Vulkan-targeted SPIR-V.  112 sampled system/tree/material shaders translated
  with 0 failures and all 112 passed `vkCreateShaderModule` on the Android
  GPU.  `tools/shader_spike.py` reproduces the sample and `probe-spirv`
  validates a cache on-device.  See
  `docs/android-hle-phase2b2-shader-spike.md`.
- 2B.3 rendering: a real Vulkan triangle pipeline (built-in GLSL -> SPIR-V at
  build time), offscreen `probe-vulkan-triangle` with 60 frames and PPM
  screenshot, an NVN command-buffer draw path (`probe-nvn-draw`, 60
  draws/presents), a sampled-texture path (`probe-vulkan-texture` and
  `probe-nvn-texture`, including guest block-linear deswizzle), a visible
  translated TOTK VS/PS draw with the full `.res.json` descriptor map
  (`probe-totk-vs` for cbuf-only probes, `probe-totk-reflect` for
  cbufs/sbufs/textures/images, `probe-nvn-reflect` through the NVN program),
  and a separate Android Surface clear APK under `surface_test/` (1,620+
  frames on the arm64 emulator).  See
  `docs/android-hle-phase2b3-rendering.md`.
- Shader cache: `nvnProgramSetShaders`/`Ext` hash the NVN control+code pair
  with the 2B.2 key and load `<stage>/<key>.spv` + `.res.json` from
  `TOTK_ANDROID_SHADER_CACHE`.  The first submit creates the reflected
  `VkPipeline`; a miss presents the recorded clear instead of a fake shader.
  `tools/prepare_shader_cache.py` packs the offline spike output into that
  layout, and `probe-nvn-frame` covers cache hit, pipeline creation,
  clear/viewport/scissor/draw translation and the miss fallback.
- Command-buffer resources and state: UBO/SSBO and texture binds are applied
  through the NVN stage/slot (`nvn_slot` in `.res.json`) using per-draw
  descriptor epochs (dynamic offsets for UBO/SSBO), combined handles use the
  sampler-pool entry, guest vertex buffers/attributes become a Vulkan vertex
  layout, `DrawElements` becomes `vkCmdDrawIndexed` (u8 widened to u16), and
  blend/color/channel-mask/polygon/depth-stencil state produces per-draw
  pipeline variants against a depth/stencil render target.  `probe-nvn-state`
  checks two UBO epochs in one command buffer, a magenta guest texture, a
  NEAREST sampler-pool sampler, depth-test accept/reject, blend, culling and an
  indexed `{pos,uv}` stream.  Shader-cache entries may be stored as
  `<key>-<attr/tex suffix>.spv`; `shader_spike.py --answers` and
  `prepare_shader_cache.py` produce and pack those bind-state variants.
- `nn::hid::InitializeNpad` is a host no-pad backend for now (zero Npad/6-axis
  state, vibration stub). Keyboard/mouse/touch/debug-pad/gesture initialization
  are host no-device initializers too, because the HID shared-memory service is
  not implemented yet.
- The guest thread stack guard uses accurate `QueryMemory` committed ranges and
  SVC `MapMemory`/`UnmapMemory`; guest TLS/TLR live at the bottom of each
  committed stack area.
- HLE thunks switch to the per-thread HostContext stack.
- The original `main+0x1123f7c` null 0x230 `nn::gfx` object was an allocator
  null caused by missing RomFS heap definitions. The boot path needs
  `System/HeapSize/*.HeapDef.byml.zs` and
  `System/Resource/ResourceSizeTable.*.rsizetable.zs` (plus `Pack/` and the
  boot `RSDB/`, `Model/`, `TexToGo/`, `System/` assets). Without them the
  resource-size vector is all zero and no heap is created; this is not masked
  by a null-pointer skip.

## Build

The work folder must contain `extracted/v0/exefs` and `rewritten/android-v0`
from `prepare_android_cpu.py`.

```sh
cd /path/to/MacNativeGames
WORK=gamebin/TOTK_build/totk games/totk/android/build.sh
```

This also regenerates `build/android-v0/runtime_tables.h` with
`scripts/prepare_android_runtime.py`. Set `ANDROID_NDK_HOME` if the NDK is not
at the Homebrew default (`/opt/homebrew/share/android-ndk`).

The CLI is linked dynamically (`-lvulkan -ldl`) because Android's Vulkan loader
is a shared library; a static executable cannot call the real driver.

## Run

```sh
adb push gamebin/TOTK_build/totk/build/android-v0/totk-android /data/local/tmp/totk-android/
adb push gamebin/TOTK_build/totk/rewritten/android-v0/*.patched.bin /data/local/tmp/totk-android/
adb shell chmod 755 /data/local/tmp/totk-android/totk-android
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-hle
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-tls
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvdrv
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-vi
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-submit
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-vulkan
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-vulkan-triangle
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-vulkan-texture
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-draw
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-texture
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-totk-vs
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-totk-reflect
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-frame
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-state
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-shader-cache
TOTK_ANDROID_TRACE_SVC=1 adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android boot-sdk
```

### Shader SPIR-V spike

```sh
python3 games/totk/android/tools/shader_spike.py \
  --romfs /tmp/totk-romfs-min --out /tmp/totk-shader-spike
adb push /tmp/totk-shader-spike/out /data/local/tmp/totk-android/spv
adb shell 'TOTK_ANDROID_SPV_DIR=/data/local/tmp/totk-android/spv \
  /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-spirv'
```

`--all` translates every unique shader in the selected packages; the default
is two per package.  The spike uses the macOS `totk_xlate` prebuilt and keeps
game-derived output under `/tmp`.

### Shader cache

The runtime key is SHA-256 over the NVN control section and shader code.  Pack
a spike result into the device cache layout with:

```sh
python3 games/totk/android/tools/prepare_shader_cache.py \
  --spike /tmp/totk-shader-spike-all --out /tmp/totk-shader-cache
adb push /tmp/totk-shader-cache /data/local/tmp/totk-android/shader-cache
adb shell 'TOTK_ANDROID_SHADER_CACHE=/data/local/tmp/totk-android/shader-cache \
  /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-shader-cache'
adb shell 'TOTK_ANDROID_SHADER_CACHE=/data/local/tmp/totk-android/shader-cache \
  /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-frame'
```

`probe-shader-cache` validates every packed module with
`vkCreateShaderModule`.  `probe-nvn-frame` writes its own synthetic keys, so it
works without game shader files; set `TOTK_ANDROID_SHADER_CACHE_MISS=1` to
exercise the clear-only fallback and `TOTK_ANDROID_CLEAR_ONLY=1` for a
clear-only command buffer.  The packed output is game-derived and must stay in
the work folder.

### Surface clear test

The on-screen path is tested by a separate minimal APK with no game data:

```sh
ANDROID_NDK_HOME=/opt/homebrew/share/android-ndk \
ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools \
games/totk/android/surface_test/build.sh
adb install -r games/totk/android/surface_test/build/surface-test.apk
adb shell am start -n com.totk.surfacetest/.MainActivity
```

It creates a real `VkSwapchainKHR` from an `ANativeWindow` and presents an
animated clear.  The arm64 emulator reached 1,620+ frames (~60 Hz).  The physical
Xiaomi device now accepts `adb install` (the streamed install succeeds), but the
last run was on a locked/dozing screen, so Android destroyed the Surface after
one frame; unlock and keep the screen on to measure it on-device.

### Missing RomFS assets

The debug `romfs-min` set is not the full game image.  Boot needs the Boot UI
layout archives, which are **test data extracted from the local XCI**, not
repository files:

```text
UI/LayoutArchive/Boot.Product.100.Nin_NX_NVN.blarc.zs
UI/LayoutArchive/Common.Product.100.Nin_NX_NVN.blarc.zs
UI/LayoutArchive/Title.Product.100.Nin_NX_NVN.blarc.zs
Sound/Product.100.alto__AltoConfig.bgyml
```

The three archives and the sound config were extracted for the device test
with `games/totk/scripts/extract_romfs.py`-style NCA/RomFS parsing and pushed
under `/data/local/tmp/totk-android/romfs-min/`.  They are not committed.

List any remaining misses with:

```sh
adb shell 'cd /data/local/tmp/totk-android && \
  TOTK_ANDROID_INITS=1 TOTK_ANDROID_RUN_NNMAIN=1 \
  TOTK_ANDROID_FS_TRACE=1 \
  TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min/totk-romfs-min \
  ./totk-android /data/local/tmp/totk-android boot-sdk' 2>&1 | grep 'not found'
```

On the current device the full 5.5 GiB RomFS content is nested under
`romfs-min/totk-romfs-min`; point `TOTK_ANDROID_ROMFS` at that directory (the
top-level `romfs-min` is only the older partial overlay).  Add any missing
assets (or use the full RomFS) before treating the next crash as a
renderer/HLE issue.

### Android audio and time

`runtime/audio_renderer.c` is compiled for Android with
`audio_mixer_android.c`, a silent backend that preserves the `audren:u`
protocol (the game opens the renderer and submits REV11 updates).  `time_host.c`
provides the host `nn::time` clock; SVC 0x13/0x14 map/unmap shared memory with
copy semantics.  No AAudio output is wired yet.

### SDK log manager (`lm`)

The former `sdk+0x356128` stop was not a thread/name manager.  It was
`nn::lm::detail::ShimLibraryGlobal::GetInstance`:

```text
guard     sdk+0xd18118 -> sdk+0xd80d78
holder    sdk+0xd18120 -> sdk+0xd80d70
getter    sdk+0x3560f0
ctor      sdk+0x356060  (CreateLogServiceByHipc / ILogService::OpenLogger)
```

The SDK's `lm` command 0 is `OpenLogger`.  The host service now returns an
`ILogger` session (moved handle for a direct request, domain object for a
domain request), so `ShimLibraryGlobal` initializes and logging/asserts no
longer crash.  The old `TOTK_ANDROID_LM_FAKE` diagnostic is gone.

### Host-call and IPC tracing

```sh
TOTK_ANDROID_INITS=1 TOTK_ANDROID_RUN_NNMAIN=1 \
TOTK_ANDROID_TRACE_HLE=2 TOTK_IPC_TRACE=1 \
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android boot-sdk
```

- `TOTK_ANDROID_TRACE_HLE=1` counts every wrapped HLE call and prints a
  summary sorted by call count at exit.
- `=2` also prints the first call of each function with its time, guest thread
  handle and name (`[hle]    0.123 h=0x... nn::oe::Initialize`).
- `=0` counts quietly and prints only the exit summary.
- While tracing is on, the last wrapped call is remembered so a crash report
  can name it.
- `TOTK_IPC_TRACE=1` prints one line per request:
  `[ipc] service=lm command=0 object=1 type=6 data=8 result=0`, plus lines for
  opened/unknown `sm:` services, unknown domain objects and unknown commands.

### Crash report

`SIGSEGV`/`SIGBUS`/`SIGILL` now print module-relative `pc` and `lr`, the guest
frame-pointer chain, the last HLE call, the last IPC request and the guest
thread handle:

```
[crash] sig=11 pc=main+0xeba4f4 lr=main+0x... fault=0x48 sp=0x... x8=0
[crash] frame 0: fp=0x... lr=main+0x...
[crash] frame 1: fp=0x... lr=main+0x...
[crash] last HLE: nn::os::GetTlsValue
[crash] last IPC: service=lm command=0 object=1 type=6 data=8 result=0
[crash] guest thread handle=0x...
```

See `docs/android-hle-phase0.md` for the first `nnMain` gap and
`docs/android-hle-phase1.md` for the SEAD constructor fix, TLS verification,
the service trace and the current next gap.

Expected results on an arm64 Android device:

- `probe`: 4 threads × 1000 `GetSystemTick` calls, timer gate count 4000.
- `probe-hle`: HLE thunk returns 7.
- `probe-tls`: allocates a TLS slot, checks `SetTlsValue`/`GetTlsValue` on the
  main thread and on a second thread, verifies the values do not leak between
  threads, and checks `GetCurrentStackInfo` covers the TLR.
- `probe-nvdrv`: opens `/dev/nvmap`, creates/allocates/params/frees an nvmap
  object and verifies its address is below 2^39.
- `probe-vi`: creates a host layer/native window, checks 1920x1080 RGBA8 and
  the 8,294,400-byte framebuffer, clears it and round-trips it through the NVN
  window builder.
- `probe-vulkan`: creates a real Vulkan instance/device, a 1280x720 RGBA8
  offscreen `VkImage`, clears it on the GPU, copies it back and verifies
  `px=4080bfff`.  Device on the test target: Adreno 830, Vulkan 1.3.284.
- `probe-vulkan-triangle`: renders 60 frames of the built-in triangle,
  verifies orange center / dark corner pixels, writes `triangle.ppm` and reports
  FPS.
- `probe-nvn-draw`: records 60 NVN command-buffer draws, submits and presents
  them through the Vulkan backend; verifies the same pixels and writes
  `nvn-draw.ppm`.
- `probe-vulkan-texture`: uploads a 2x2 RGBA8 checker to a `VkImage`/sampler,
  draws a textured triangle and verifies the interpolated center pixel.
- `probe-nvn-texture`: creates a real NVN memory pool/texture through
  `nvnTextureInitialize`, writes a checker through block-linear `nvn_swizzle`,
  deswizzles it and runs 60 NVN textured draws/presents.
- `probe-totk-vs`: draws a visible full-screen triangle with the translated
  TOTK `system-vs` and `material-ps`, using `.res.json` cbuf bindings and UBOs.
- `probe-totk-reflect`: creates the two-set descriptor pipeline from the full
  `.res.json` (`cbufs`/`sbufs`/`textures`/`images`) and checks the SPIR-V
  descriptor signature.  Combined samplers, separate image+sampler pairs,
  texture arrays, storage images, SSBOs and uniform/storage texel buffers all
  compile on the Adreno 830.  It also rejects a VS/PS pair whose fragment
  input locations are not written by the vertex stage instead of relying on a
  driver failure.
- `probe-nvn-reflect`: the same full reflection map loaded into an NVN program
  and drawn through `nvnQueueSubmitCommands`; verifies NVN program -> Vulkan
  pipeline -> present with real `.res.json` bindings.
- `probe-vulkan-reflect-texture`: creates a reflected pipeline from the
  repository's `triangle_tex` shaders, then binds a real `GfxTexture` after
  pipeline creation and draws it (`808080` center / dark corner).
- `probe-vulkan-reflect-ubo`: creates a reflected UBO pipeline, writes
  `(0.25, 0.5, 0.75)` into the UBO after pipeline creation, draws it and reads
  back `4080bfff`.
- `probe-nvn-texture-reflect`: runs a real NVN texture object through the
  repository `triangle_tex` reflected pipeline and an NVN draw/present, proving
  the descriptor resource replacement path.
- `probe-nvn-bind`: creates texture/sampler pools, packs a combined NVN handle,
  resolves it through the pool maps and exercises
  `SetTexturePool`/`SetSamplerPool`/`BindTexture`/`BindImage`/
  `BindUniformBuffer`/`BindStorageBuffer`.
- `probe-nvn-cb-lifetime`: keeps 96 live command buffers, submits them, then
  finalizes one, re-submits its stale handle and reuses the storage.  It guards
  the dynamic command-buffer registry and rejects stale `CMDB` objects instead
  of handing a destroyed program/window to the Vulkan driver.
- `probe-nvn-draw` with `TOTK_ANDROID_SPV_VS`/`TOTK_ANDROID_SPV_PS` loads a
  translated TOTK SPIR-V pair into the NVN program and executes the draw.  The
  sampled system/material pair is not a visible triangle (`visible:false`);
  real per-draw texture/UBO upload is the next step, not descriptor mapping.
- `probe-nvn-submit`: builds a CPU-only NVN device/queue/window, records one
  command buffer, submits it and presents it through Vulkan.  This is the
  first explicit `nvnQueueSubmitCommands`/`nvnQueuePresentTexture` point; it is
  a self-test, not the game's own frame.
- `probe-spirv`: loads every `.spv` in `TOTK_ANDROID_SPV_DIR` and creates a
  real `VkShaderModule` for each.  The 2B.2 sample was 112 modules, 0 failed.
- `boot-sdk`: reaches `{"stage":"loader-ready"}`.
- `TOTK_ANDROID_INITS=1 boot-sdk`: runs the three module `DT_INIT`s and
  reaches `{"stage":"nnMain-entry"}`.
- `TOTK_ANDROID_STOP_AFTER_INITS=1`: stops after the constructors for
  diagnostics; it implies `TOTK_ANDROID_INITS`.
- `TOTK_ANDROID_CTOR_PROBE=1`: logs the `DT_INIT` state before/after each
  module, the `.init_array` entry count, and the number of constructors the
  loader calls. Use it to confirm the 1.0.0 state-1 path.
- `TOTK_ANDROID_TLS_TRACE=1`: logs each thread's guest TLS/TLR/ThreadType and
  guest stack top.
- `TOTK_ANDROID_FS_PROBE=1`: after the constructors, exercises the `nn::fs`
  host overrides: temp write/read, save mount/create/write/commit, directory
  listing, and (when `TOTK_ANDROID_ROMFS` is set) a RomFS mount/read.
  `TOTK_ANDROID_PROFILE=<dir>` selects the save/temp/cache root;
  `TOTK_ANDROID_ROMFS=<dir>` points at an extracted RomFS directory. Add
  `TOTK_ANDROID_FS_BY_SERVICE=1` to send the probe through `fsp-srv` instead
  of the host overrides.
- `TOTK_ANDROID_STORAGE=<file>` still backs the optional `fsp-srv`
  `OpenDataStorageByCurrentProcess` diagnostic path.

## Android-specific notes

- **Native TLS**: the NDK defaults to emulated TLS in this configuration. The
  build uses `-fno-emulated-tls` and aligns `g_host`/`g_slots` to 64 bytes.
  Without that, `TPIDR_EL0 + KCTX` is not the `__thread` address.
- **ThreadLocalRegion**: `TPIDRRO_EL0+0x1f0` holds the guest `TPIDR_EL0` value
  and `+0x1f8` the ThreadType pointer.  Both are initialized per thread; the
  SDK restores TP from `+0x1f0` during thread setup/teardown.
- **W^X**: map the image `R-X` first, then make data/key page writable. Mapping
  it writable then adding `PROT_EXEC` is denied by Android (`execmod`).
- **x18**: host code is built with `-ffixed-x18`; `gate_android.S` restores
  guest x18 and has a `bti c` landing pad.
- **Page size**: the loader uses `sysconf(_SC_PAGESIZE)`; the test device is
  4 KiB. 16 KiB devices are not yet tested.
- **Alias region**: the host reserves up to 64 GiB at `0x7000000000`, halving
  the size until the mapping succeeds; if the fixed window is unavailable it
  takes a mapping of the same size at any host address. `GetInfo` reports
  whichever base and size are used, and `MapPhysicalMemory` /
  `UnmapPhysicalMemory` only accept that window.

## Next

1. Finish the optional `fsp-srv` file-system protocol (`IFileSystem`/`IFile`
   objects) and point `IStorage` at the extracted RomFS/save data. The
   production file path now uses the `nn::fs` host overrides instead.
2. Continue past `nnMain` with the graphics/input/audio services. The original
   `main+0x1123f7c` null allocator is explained and fixed by the RomFS heap
   definitions. The former `nn::gfx` context-registry stop
   (`main+0x794dd0`) was a missing `ThreadLocalRegion+0x1f0` guest-TP field and
   is fixed.  The boot now loads the three `UI/LayoutArchive/` archives (test
   data extracted from the XCI), `Sound/Product.100.alto__AltoConfig.bgyml`,
   and runs the `audren:u` renderer with a silent Android mixer plus the host
   `nn::time` clock.  The SDK `lm` logger now initializes (command 0 is
   `OpenLogger`), and `TexToGo/miasma_altitude2.bntx.zs` fixed the null material
   builder.  `nn::oe` volume/notification overrides are in place, and the game
   reaches its **first real GPU submit**.  The next stop is
   `main+0x7a8600` after `nvnQueueFenceSync`, where a renderer object's first
   qword is null.  See `docs/android-hle-phase2a.md`.
3. `nvdrv` is now a real service (`nvdrv_android.c`): Open/Close/Initialize/
   SetAruid/Ioctl3/MapSharedMem plus a guest-visible nvmap window at
   `0x500000000`.  `probe-nvdrv` exercises it.  The next nvdrv work is the
   versioned GetStatus/display/scheduler ioctls used after the first frame.
4. `vi` has a host layer/native-window object (`native_window_android.c`), and
   `probe-vi`/`probe-nvn-submit` prove the builder round trip and the first
   submit/present points through the Vulkan backend.  The game's own first
   frame now depends on the remaining boot `TexToGo`/`Event` resources above.
5. 2B.2: ship an **offline SPIR-V shader cache**, not an on-device .NET
   translator.  The 112-shader spike translated 112/112 and the Adreno driver
   accepted 112/112 `VkShaderModule`s.  The Android reflection path now maps
   `cbufs`/`sbufs`/`textures`/`images` from `.res.json` plus the SPIR-V
   descriptor signature, including separate samplers/textures, arrays,
   storage images and texel buffers.  Next: translate the startup/title
   subset into that cache and feed the actual NVN pool objects into the
   descriptors.  Missing variants fall back to clear/simple triangle; full
   game visuals are not promised yet.  See
   `docs/android-hle-phase2b2-shader-spike.md`.
6. 2B.3: texture storage -> `VkImage` upload/format-modifier validation and
   the Android Surface app shell (SurfaceView + JNI + a worker running the
   loader, attaching the `ANativeWindow*` to the host native window).  The
   descriptor map, NVN bind state, real texture/UBO descriptor replacement and
   the Surface clear APK are now in place; per-draw image/storage-image upload
   and full mip/format coverage are next.
7. Port the remaining memory SVCs (`MapMemory` is currently a copy, not a
   shared alias; `MapPhysicalMemory` details) and the rest of the HLE service
   registry.
