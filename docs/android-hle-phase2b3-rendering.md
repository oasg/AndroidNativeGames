# Android HLE Phase 2B.3: rendering milestones

## 1. Status

| # | Milestone | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Vulkan instance/device | done | `probe-vulkan`, Adreno 830 / Vulkan 1.3.284 |
| 2 | offscreen clear | done | `probe-vulkan`, GPU clear/readback `px=4080bfff` |
| 3 | on-screen clear | done (emulator) | Surface test APK, 1,620+ frames at ~60 Hz, screenshot |
| 4 | triangle | done | `probe-vulkan-triangle`, 60 frames, screenshot |
| 5 | NVN program/pipeline/texture/draw | done | `probe-nvn-texture` (guest block-linear upload), `probe-totk-vs` visible TOTK VS/PS |
| 6 | game first frame | blocked | reaches first GPU submit; stops at `main+0x7a8600` null renderer object |
| 7 | continuous frames | done (host) | triangle 60 frames; surface 1,620+ frames; NVN 60 draws |
| 8 | shader cache + `nvnProgramSetShaders*` -> pipeline | done (host) | `probe-nvn-frame`: key hit, reflected pipeline, visible draw |
| 9 | command-buffer clear/viewport/draw translation | done (first slice) | `probe-nvn-frame` center `ff4d0dff`; miss fallback `0d141fff` |
| 10 | command-buffer resource + vertex/index translation | done (first slice) | `probe-nvn-state`: UBO `4080bfff`, texture `808080ff`, attr/index `808040ff` |

The game's own first frame is **not** claimed.  The renderer path is now
independently exercisable.  The former `nn::gfx` context-registry stop was a
`ThreadLocalRegion+0x1f0` TLS bug and is fixed; the boot now passes the UI
layout and audio renderer, and the remaining stop is the missing boot
`TexToGo`/`Event` resources on a texture-worker thread.

## 2. Offscreen clear

Already present from 2B.1:

```sh
totk-android <dir> probe-vulkan
```

Creates a 1280x720 RGBA8 `VkImage`, clears it on the GPU, copies it back, and
verifies the readback:

```text
[totk-vk] selftest PASS 1280x720 RGBA8 px=4080bfff device='Adreno (TM) 830'
```

## 3. On-screen clear

The Android Surface path is tested with a small, separate Activity that does
not need game data:

- `games/totk/android/surface_test/MainActivity.java`
- `games/totk/android/surface_test/surface_test.c`
- `games/totk/android/surface_test/build.sh`

The native side obtains `ANativeWindow_fromSurface`, creates
`VkAndroidSurfaceKHR`, a `VkSwapchainKHR` (4 images on the emulator), and
presents an animated clear continuously (1,620+ frames measured).

```sh
ANDROID_NDK_HOME=/opt/homebrew/share/android-ndk \
ANDROID_SDK_ROOT=/opt/homebrew/share/android-commandlinetools \
games/totk/android/surface_test/build.sh
adb install -r games/totk/android/surface_test/build/surface-test.apk
adb shell am start -n com.totk.surfacetest/.MainActivity
```

Emulator evidence (`emulator-5556`, SwiftShader, 2400x1080):

```text
I totk-vk: instance=... device='SwiftShader Device (LLVM 10.0.0)'
I totk-vk: Android surface 2400x1080 format=R8G8B8A8_UNORM images=4
I totk-surface: presented 60 frames
...
I totk-surface: presented 1620 frames
```

A screenshot shows the full SurfaceView covered by the animated clear.  The
physical test device (Xiaomi 24122RKC7C) now accepts `adb install` after the
install-via-USB permission was enabled: the streamed install succeeds and the
Activity starts.  In the last physical run the phone was dozing/locked
(`mWakefulness=Dozing`, `mScreenState=OFF`, `mDreamingLockscreen=true`), so
Android destroyed the Surface after one frame.  Unlock the phone and keep the
screen on before measuring the physical on-screen clear; the emulator result
above uses the same native library and backend.

## 4. Triangle

The triangle is a repository-owned GLSL pair, compiled to SPIR-V with the
NDK's `glslc` at build time:

- `games/totk/android/tools/triangle.vert`
- `games/totk/android/tools/triangle.frag`
- `games/totk/android/tools/prepare_triangle_spv.py`
- generated `triangle_spv.h`

`gfx_backend_builtin_triangle_pipeline` creates a real VkRenderPass,
VkGraphicsPipeline, shader modules and framebuffer.  `probe-vulkan-triangle`
draws 60 frames, checks center/corner pixels, writes a PPM and hashes it:

```text
{"status":"pass","probe":"vulkan-triangle","frames":60,"fps":778.54,
 "center":"ff4d0dff","corner":"0d141fff",
 "screenshot":"/data/local/tmp/totk-android/triangle.ppm"}
```

The PPM can be pulled and converted for inspection:

```sh
adb pull /data/local/tmp/totk-android/triangle.ppm /tmp/triangle.ppm
sips -s format png /tmp/triangle.ppm --out /tmp/triangle.png
```

## 5. Minimal NVN draw path

The NVN object layer now has a real draw path, not just a log:

- `nvnCommandBufferDrawArrays` records first/count;
- `nvnCommandBufferEndRecording` returns a handle mapped to the command buffer;
- `nvnQueueSubmitCommands` looks up the handle and calls the Vulkan backend;
- `nvnQueuePresentTexture` presents the same native window;
- `probe-nvn-draw` creates an NVN device, queue, program, window and command
  buffer, records `DrawArrays(3)`, submits and presents 60 times.

```text
[totk-nvn] GPU submit #1 ... commands=1
[totk-nvn] NVN draw #1 program=... window=... vertices=3
[totk-vk] present #1 offscreen 1280x720 R8G8B8A8_UNORM
...
{"status":"pass","probe":"nvn-draw","frames":60,"screenshot":"..."}
```

The program owns a host-created Vulkan pipeline from translated SPIR-V.  A game
NVN `prog_t` still carries the original shader data/control pointers; the test
path now maps all four `.res.json` descriptor sections (`cbufs`, `sbufs`,
`textures`, `images`), including separate samplers, arrays, storage images and
texel buffers.  Real per-draw texture/image upload and full mip/format coverage
remain the next step.

### Minimal NVN texture path

`probe-nvn-texture` creates:

- a real `nvnMemoryPool` and a 2x2 `NVNtexture` through
  `nvnTextureInitialize`;
- a Vulkan `VkImage`, `VkDeviceMemory`, `VkImageView` and `VkSampler` from the
  NVN texture upload bridge;
- a textured triangle pipeline with a combined image sampler descriptor;
- 60 NVN command-buffer draws/presents.

The checker texture is visible in `/data/local/tmp/totk-android/nvn-texture.ppm`
(red/green/blue/white interpolation); center pixel `808080ff`, corner
`0d141fff`.  This is the minimal NVN program + pipeline + texture + draw path.

The upload bridge now has a guest-memory path:
`runtime_nvn_texture_upload_guest` runs the shared `nvn_deswizzle.c` Tegra
GOB decoder on the `nvnTextureInitialize` storage, then creates the Vulkan
image.  The 2x2 checker is written through `nvn_swizzle` into the guest texture
memory, so `probe-nvn-texture` no longer supplies host pixels.  The remaining
limit is level/layer/format coverage; the block-linear RGBA8 level-0 path is
implemented.

### Visibility from translated TOTK VS/PS + UBO

`probe-totk-vs` now uses the real translated system vertex shader
(`system-vs-8511c97f`) and a translated constant-white pixel shader
(`material-ps-3f9bbff4`).  It binds the two `vp_c4`/`vp_c11` UBOs and draws
three vertices generated by `gl_VertexIndex`, so the output is a visible
full-screen triangle:

```sh
adb push /tmp/totk-spv-all/system-vs-8511c97f.spv /data/local/tmp/totk-android/spv/system-vs-8511c97f.spv
adb push /tmp/totk-spv-all/material-ps-3f9bbff4.spv /data/local/tmp/totk-android/spv/totk-ps.spv
adb push /tmp/totk-shader-spike-all/out/system-vs-8511c97f.res.json /data/local/tmp/totk-android/spv/totk-vs.res.json
adb push /tmp/totk-shader-spike-material/out/material-ps-3f9bbff4.res.json /data/local/tmp/totk-android/spv/totk-ps.res.json
adb shell 'TOTK_ANDROID_SPV_VS=/data/local/tmp/totk-android/spv/system-vs-8511c97f.spv \
  TOTK_ANDROID_SPV_PS=/data/local/tmp/totk-android/spv/totk-ps.spv \
  TOTK_ANDROID_REFL_VS=/data/local/tmp/totk-android/spv/totk-vs.res.json \
  TOTK_ANDROID_REFL_PS=/data/local/tmp/totk-android/spv/totk-ps.res.json \
  /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-totk-vs'
```

Result:

```json
{"status":"pass","probe":"totk-vs","ps":".../totk-ps.spv",
 "center":"ffffff00","corner":"ffffff00"}
```

Screenshot `/tmp/totk-draw-ps.png`: the translated TOTK VS produces the
full-screen triangle; the translated TOTK PS outputs constant white.  The
`cbufs` bindings are read from the generated `.res.json`.  A second probe,
`probe-totk-reflect`, builds a two-set pipeline (VS set 0, PS set 1) from the
whole reflection file:

```sh
TOTK_ANDROID_SPV_VS=.../system-vs-8511c97f.spv TOTK_ANDROID_SPV_PS=.../system-ps-170c9242.spv TOTK_ANDROID_REFL_VS=.../totk-vs.res.json TOTK_ANDROID_REFL_PS=.../system-ps-170c9242.res.json   totk-android <dir> probe-totk-reflect
```

Result:

```json
{"status":"pass","probe":"totk-reflect",
 "vs_bindings":2,"ps_bindings":1,
 "center":"0d141fff","corner":"0d141fff","visible":false}
```

The PS used here is a constant-black shader, so `visible:false` is expected; the
pipeline and both descriptor sets were created and drawn.

### Full `.res.json` descriptor mapping

`probe-totk-reflect` now reads the SPIR-V descriptor signature and the
`.res.json` source metadata together:

- `cbufs` -> `UNIFORM_BUFFER`, NVN slot -> `BindUniformBuffer`;
- `sbufs` -> `STORAGE_BUFFER`, with `cb_slot`/`cb_offset` kept for the pointer
  source and `flags & 1` as read/write;
- `textures` combos:
  - `type == 0` + `separate` -> `SAMPLER`;
  - `type != 0` + `separate` -> `SAMPLED_IMAGE`;
  - `type != 0` without `separate` -> `COMBINED_IMAGE_SAMPLER`;
  - `TextureBuffer` -> `UNIFORM_TEXEL_BUFFER` + `VkBufferView`;
- `images` -> `STORAGE_IMAGE` or `STORAGE_TEXEL_BUFFER`, with the
  `.res.json` format (`R8G8B8A8Unorm`, `R32G32B32A32Float`, ...) mapped to
  `VkFormat`;
- `array` 1 / N / 0 -> descriptor count 1 / N / pool-sized runtime array.

The SPIR-V signature is authoritative: the Android parser reads
`OpDecorate DescriptorSet/Binding`, `OpTypeImage`, `OpTypeSampler`,
`OpTypeSampledImage`, arrays and pointer storage classes, then merges only the
NVN slot/handle/format fields from `.res.json`.  This also adds the implicit
Ryujinx `support_buffer` binding that `.res.json` does not list.

On the Adreno 830 the following combinations create a pipeline and draw:

- 5 combined samplers + 4 UBOs + `support_buffer` (`system-ps-06b4f948`);
- separate `texture2D` + `sampler`;
- `texture2D[4]` + `sampler[4]` descriptor arrays;
- SSBO + storage image + separate texture/sampler;
- `samplerBuffer` (uniform texel buffer) + `imageBuffer` (storage texel
  buffer).

The previous "sampled PS hangs" repro was a **VS/PS interface mismatch**: the
probe paired a PS that reads input locations 0, 1, 2, 3 with `system-vs` that
only writes location 2.  `probe-totk-reflect` now checks the entry-point
interface first and fails with the missing locations instead of entering the
driver compiler.  A matching VS makes the same 5-texture PS compile and draw.

```sh
TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH=1 can force the old diagnostic path;
it is not a valid pipeline test.
```

The NVN side has the corresponding bind surface:
`nvnCommandBufferSetTexturePool`, `SetSamplerPool`, `BindUniformBuffer(s)`,
`BindStorageBuffer(s)`, `BindTexture(s)` and `BindImage(s)`, with host-side
pool id -> `tex_t`/`samp_t` maps.  `probe-nvn-bind` checks the combined handle
packing (`texture id | sampler id << 20`) and lookup, and
`probe-nvn-reflect` loads a `.spv + .res.json` pair through
`runtime_nvn_program_load_spirv_reflect` and draws it.

### Replacing dummy descriptors with real resources

The reflected pipeline creates a valid dummy resource for every binding so the
layout and pipeline exist even before a draw.  Three runtime entry points then
replace those descriptors without recreating the pipeline:

- `gfx_backend_reflected_bind_texture(pipeline, set, binding, texture)`;
- `gfx_backend_reflected_write_uniform(pipeline, set, binding, offset, data, size)`;
- `gfx_backend_reflected_write_storage(...)`.

`probe-vulkan-reflect-texture` uses the repository's `triangle_tex` shaders,
binds a real 2x2 checker after pipeline creation and draws it.  Result:

```text
[totk-vk] reflected bind texture set=1 binding=0 texture=...
[totk-vk] reflected texture selftest center=808080ff corner=0d141fff
{"status":"pass","probe":"vulkan-reflect-texture"}
```

`probe-vulkan-reflect-ubo` writes `(0.25, 0.5, 0.75, 1.0)` into a reflected
UBO after pipeline creation and reads back `4080bfff`.  On the NVN side,
`runtime_nvn_program_bind_texture` accepts a `tex_t` and
`probe-nvn-texture-reflect` creates a real guest block-linear texture, uploads
it, binds it to the reflected PS and executes an NVN submit/present.  These
three probes run without game shader files; the NVN texture comes from the
guest memory pool.

The same path is ready for `sbufs` data
(`gfx_backend_reflected_write_storage`) and for storage-image/texel-buffer
resources; they still use the dummy image/buffer until the per-draw upload
bridge lands.

### Translated TOTK shader -> NVN program

`probe-nvn-draw` can also load a translated TOTK SPIR-V pair into the NVN
program object:

```sh
adb push /tmp/totk-spv-all/system-vs-75ff918f.spv /data/local/tmp/totk-android/spv/system-vs.spv
adb push /tmp/totk-spv-all/material-ps-a9320a70.spv /data/local/tmp/totk-android/spv/material-ps.spv
adb shell 'TOTK_ANDROID_SPV_VS=/data/local/tmp/totk-android/spv/system-vs.spv \
  TOTK_ANDROID_SPV_PS=/data/local/tmp/totk-android/spv/material-ps.spv \
  /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-nvn-draw'
```

It creates the Vulkan pipeline and executes 60 NVN draws.  The sample
system/material pair writes `gl_Position=(1,1)` and a zero fragment color, so
it produces no visible triangle; the probe reports `"visible":false`.  Visible
game output requires the full NVN vertex/UBO/texture binding, not a missing
shader translation step.

`probe-nvn-reflect` uses the same NVN program path with the complete
`.res.json` reflection:

```sh
TOTK_ANDROID_SPV_VS=.../match.spv TOTK_ANDROID_SPV_PS=.../system-ps-06b4f948.spv \
TOTK_ANDROID_REFL_VS=.../empty.res.json TOTK_ANDROID_REFL_PS=.../system-ps-06b4f948.res.json \
TOTK_ANDROID_NVN_DRAW_FRAMES=2 totk-android <dir> probe-nvn-reflect
```

The sampled PS is a real 5-texture material/system shader; the probe creates
its full two-set layout, loads the descriptor map into the NVN program, submits
two draws and presents a PPM.

### Shader cache and `nvnProgramSetShaders*` -> pipeline

The Android runtime now consumes the 2B.2 key directly instead of a
probe-selected path.  `nvnProgramSetShaders` and
`nvnProgramSetShadersExt` hash the NVN control section (`0x880`, or its
declared short section size) and the code up to the length in the control
section, then look up:

```text
<cache>/<stage>/<key>.spv
<cache>/<stage>/<key>.res.json
```

under `TOTK_ANDROID_SHADER_CACHE` (default `<profile>/shader-cache`).  The
seed's `<key>-<variant>.spv` names and a flat `<key>.spv` are accepted as
fallbacks.  A hit loads the module and marks the stage; when both VS and PS
are present, the first submit creates the reflected `VkPipeline`, keyed by the
target format.  A miss is logged with the key and the frame falls back to the
recorded clear, never to a fabricated shader.

Offline packing uses the 2B.2 spike output:

```sh
python3 games/totk/android/tools/prepare_shader_cache.py \
  --spike /tmp/totk-shader-spike-all --out /tmp/totk-shader-cache
adb push /tmp/totk-shader-cache /data/local/tmp/totk-android/shader-cache
```

The packer renames each spike job stem to the NVN key in
`spike-report.json`; `tests/test_android_shader_cache.py` covers the layout,
bad-module rejection and stale-entry cleanup.  The output is game-derived and
stays in the work folder.  `probe-shader-cache` validates the packed
`<stage>/<key>.spv` files (`vkCreateShaderModule` on the device) and counts the
matching reflections:

```text
{"status":"pass","probe":"shader-cache","modules":92,"reflections":92,"failed":0}
```

`probe-nvn-frame` stores two synthetic NVN control/code pairs, loads them back
and draws the translated triangle through a real NVN command buffer:

```text
[totk-nvn] shader cache hit #1 vs f2cb... code=256 +res.json
[totk-nvn] shader cache hit #2 ps 3619... code=256 +res.json
[totk-nvn] program=0x... create pipeline from cache vs=... ps=... format=0x25
[totk-nvn] NVN draw #1 ... reflected
[totk-nvn] frame selftest center=ff4d0dff corner=0d141fff ok=1
{"status":"pass","probe":"nvn-frame",...}
```

With `TOTK_ANDROID_SHADER_CACHE_MISS=1` the same probe writes nothing, the
program reports two misses, and the frame is `0d141fff` everywhere.  The
synthetic key derivation was checked against the same SHA-256
`prepare_shader_seed.py` uses.

### Command-buffer translation order

The command buffer records the guest order and translates it in that order:

1. `nvnCommandBufferClearColor` / `ClearColori` / `ClearColorui` -> the
   render-pass clear value (a later clear becomes `vkCmdClearAttachments`);
2. `nvnCommandBufferSetViewport(s)` / `SetScissor(s)` -> dynamic
   viewport/scissor;
3. `nvnCommandBufferDrawArrays` -> `vkCmdDraw` with the reflected pipeline.

`nvnCommandBufferBindProgram` selects the program.  `SetRenderTargets`
resolves the window from its color texture, so draw and present share one host
target.  When the guest's native-window pointer is not a host window yet (the
VI/app-shell handoff is still incomplete), `nvnWindowInitialize` creates an
offscreen 1920x1080 RGBA8 host window so the cached pipeline has a target.
Descriptor/resource binds are translated after the clear/viewport/draw slice,
in the order the guest records them.

### Resource and vertex/index translation

The next slice is in place:

- `nvnCommandBufferBindUniformBuffer(s)` and
  `nvnCommandBufferBindStorageBuffer(s)` store the last address/size per NVN
  stage/slot.  At submit, the reflected pipeline maps the NVN slot through
  `.res.json` (`nvn_slot`) and writes the guest memory into the real UBO/SSBO.
- `nvnCommandBufferBindTexture(s)` resolves the combined handle through the
  texture pool, uploads the guest block-linear texture and writes the real
  image/sampler descriptor.
- `nvnCommandBufferBindVertexBuffer(s)` stores the guest address range;
  `BindVertexAttribState`/`BindVertexStreamState` decode
  `{fmt:8, offset:19, stream:5}` and `{stride, divisor}` into a Vulkan vertex
  layout.  The pipeline is recreated when the layout changes.
- `nvnCommandBufferDrawElements`/`DrawElementsBaseVertex` translate to
  `vkCmdBindIndexBuffer` + `vkCmdDrawIndexed`; u8 indices are widened to u16.

`probe-nvn-state` covers all three paths without game data:

```text
[totk-nvn] state probe ubo     center=4080bfff ok=1
[totk-nvn] state probe texture center=808080ff ok=1
[totk-nvn] state probe attr    center=808040ff ok=1
[totk-nvn] state selftest PASS
{"status":"pass","probe":"nvn-state"}
```

The UBO path binds `(0.25, 0.5, 0.75, 1.0)`; the texture path binds a guest
2x2 checker; the attr path uses an interleaved `{pos.xy, uv.xy}` vertex stream
with two `R32G32_SFLOAT` attributes and a u16 index buffer.

Limits of this slice:

- descriptor state is **last-write-wins per submit**; a command buffer that
  changes a UBO/texture between draws needs per-draw descriptor sets (the next
  step);
- a combined texture handle currently uses the texture's default sampler, not
  the sampler-pool entry encoded in the handle;
- guest UBO/SSBO ranges larger than the host allocation (64 KiB UBO, 4 KiB
  SSBO) are copied up to the allocation and logged as truncated;
- attributes whose NVN stream has no bound stride are dropped instead of
  producing an invalid pipeline;
- offline shaders must be translated with the matching `attr`/`tex` bind-state
  answers for the SPIR-V input types to match the guest data;
- depth/stencil targets and blend/raster state are still fixed.

## 6. Game first frame

The former `main+0x794dd0` stop was a TLS-layout bug, not an empty
`nn::gfx` registry.  Horizon's ThreadLocalRegion stores the guest
`TPIDR_EL0` at `TPIDRRO_EL0+0x1f0`; the SDK restores it with
`msr tpidr_el0, [tpidrro_el0+0x1f0]`.  `runtime_thread_init` now fills that
field (it already filled the `+0x1f8` ThreadType pointer), so the registry
lookup succeeds and the game continues past it:

```text
gfx-registry head=...: <context> <context> <vtable> 0
gfx-registry node[0]=... key+0x5e8=0x51 next=0
```

The game now reaches Boot UI and audio initialization.  The debug RomFS needs
the three `UI/LayoutArchive/` archives (extracted from the XCI for the device
test) and `Sound/Product.100.alto__AltoConfig.bgyml`.  The Android build also
includes the real `audren:u` renderer protocol with a silent mixer backend,
the host `nn::time` clock, and SVC 0x13/0x14 shared-memory mapping.  It opens
the audio renderer, processes the REV11 request, mounts save/cache storage and
stops at:

```text
[crash] sig=11 pc=sdk+0x356128 lr=sdk+0x35562c fault=0x0
[crash] last NVN api: nvnTextureGetViewOffset
[crash] last IPC: service=IAudioDevice command=7 ...
```

That was `nn::lm::detail::ShimLibraryGlobal::GetInstance`, the SDK log-manager
singleton (`sdk+0x3560f0`; guard `sdk+0xd18118`, holder `sdk+0xd18120`, storage
`sdk+0xd80d70`).  The SDK's `lm` command 0 is `OpenLogger`; the host service now
returns an `ILogger` (moved session for a direct request, domain object for a
domain request), so the singleton initializes and logging/asserts no longer
crash.

The next stop was a null texture builder.  It was data, not a builder API bug:
the base RomFS lacked `TexToGo/miasma_altitude2.bntx.zs`, so the material table
entry for `SceneMaterial.ng_model7.15.x_0` was not registered.  Adding that
base-XCI file (plus neighbouring Water assets) moves past `main+0x84cb1c` and
`main+0xc49974`.

The following HLE gaps were fixed with main-import overrides:

- `nn::oe::SetExpectedVolumeBalance` / `GetExpectedVolumeBalance` called
  `nn::am::GetAudioController()`, which the host does not provide;
- `nn::oe::TryPopNotificationMessage` waited on a SystemEvent that the host
  `nn::oe::Initialize` does not attach.

The game now reaches its **first real GPU submit**:

```text
[totk-nvn] GPU submit #1 queue=... commands=1 (CPU-only host)
[crash] sig=11 pc=main+0x7a8600 lr=main+0x7a8434 fault=0x150
[crash] last NVN api: nvnQueueFenceSync
```

At `main+0x7a8600` the renderer object's first qword is null before the
`+0x150` virtual call.  This is after the first submit, not a log-manager or
base-RomFS stop.  The 56 remaining "not found" paths are update-only/optional
and none exist in base v0.  TLS remains valid and the Vulkan/NVN probes still
pass.  The earlier non-fatal
`nvnMemoryPoolMap: pool is CPU_NO_ACCESS` remains a separate pool-flag item.

## 7. Verification commands

```sh
totk-android <dir> probe-vulkan
totk-android <dir> probe-vulkan-triangle
totk-android <dir> probe-vulkan-texture
totk-android <dir> probe-nvn-draw
totk-android <dir> probe-nvn-texture
totk-android <dir> probe-totk-vs
totk-android <dir> probe-totk-reflect
totk-android <dir> probe-nvn-reflect
totk-android <dir> probe-vulkan-reflect-texture
totk-android <dir> probe-vulkan-reflect-ubo
totk-android <dir> probe-nvn-texture-reflect
totk-android <dir> probe-nvn-bind
totk-android <dir> probe-nvn-cb-lifetime
totk-android <dir> probe-nvn-frame
totk-android <dir> probe-nvn-state
totk-android <dir> probe-shader-cache
totk-android <dir> probe-spirv
totk-android <dir> probe-nvn-submit
```

Expected:

- `probe-vulkan`: GPU clear/readback PASS.
- `probe-vulkan-triangle`: 60 frames, center orange, corner clear, PPM.
- `probe-vulkan-texture`: 2x2 checker texture, center `808080ff`, PPM.
- `probe-nvn-draw`: 60 NVN draws/presents, center orange, PPM.
- `probe-nvn-texture`: 60 NVN textured draws/presents from guest block-linear
  storage, checker PPM.
- `probe-totk-vs`: visible translated TOTK VS/PS triangle with UBOs; cbuf
  bindings from `.res.json`.
- `probe-totk-reflect`: two-set descriptor pipeline from the complete
  `.res.json` and the SPIR-V signature; rejects a VS/PS interface mismatch and
  logs the resolved binding kinds.  With a matching VS, the 5-texture
  `system-ps-06b4f948` pipeline/draw passes.
- `probe-nvn-reflect`: the same full reflection map loaded through an NVN
  program, drawn and presented.
- `probe-vulkan-reflect-texture`: real texture bound after pipeline creation;
  center `808080ff`, corner `0d141fff`.
- `probe-vulkan-reflect-ubo`: UBO written after pipeline creation; center
  `4080bfff`.
- `probe-nvn-texture-reflect`: real guest block-linear texture through an NVN
  program and reflected pipeline; visible draw and PPM.
- `probe-nvn-bind`: NVN texture/sampler pool registration, combined-handle
  packing and the descriptor bind entry points pass.
- `probe-nvn-cb-lifetime`: 96 live command buffers, stale-handle rejection and
  storage reuse; the dynamic registry no longer drops the 65th or hands a
  finalized buffer to the driver.
- `probe-nvn-frame`: writes two synthetic NVN shader keys, hits the offline
  cache, creates the reflected pipeline and translates
  clear/viewport/scissor/draw; center `ff4d0dff`, corner `0d141fff`.
- `probe-nvn-state`: binds a UBO, a guest texture and an indexed vertex stream
  through the NVN command buffer; centers `4080bfff`, `808080ff`,
  `808040ff`.
- `probe-shader-cache`: every packed `<stage>/<key>.spv` is accepted by
  `vkCreateShaderModule` and its `.res.json` is present.
- `probe-spirv`: all cached SPIR-V modules accepted by the driver.
- `probe-nvn-submit`: the older submit/present self-test still passes.

## 8. Next

1. Move descriptor binds to per-draw state: a command buffer that changes a
   UBO/texture between draws needs separate descriptor sets (or dynamic
   offsets), and combined handles should use the sampler-pool entry instead of
   the texture's default sampler.  The current slice is last-write-wins per
   submit (`probe-nvn-state`).
2. Pack the startup/title shader subset into the offline cache and run the
   game's own `nvnProgramSetShaders*` against it.  The key/lookup/pipeline path
   is in place (`probe-nvn-frame`); a cache miss still presents a clear frame.
3. Translate the remaining render state (depth/stencil targets, blend,
   rasterization) and pack shaders with the matching `attr`/`tex` bind-state
   answers.  Vertex streams/attributes, indexed draws and the real cbuf/texture
   binds are in place (`probe-nvn-state`).
4. Fix the null renderer object at `main+0x7a8600` after the first GPU submit
   (`nvnQueueFenceSync`).  The lm logger, base RomFS material, `nn::oe` volume
   and notification paths are now in place; the game reaches its first submit.
5. Unlock the physical device and keep the screen on, then re-run the Surface
   test; `adb install` now succeeds on the Xiaomi device.
