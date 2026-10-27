# Android HLE Phase 2B.3: rendering milestones

## 1. Status

| # | Milestone | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Vulkan instance/device | done | `probe-vulkan`, Adreno 830 / Vulkan 1.3.284 |
| 2 | offscreen clear | done | `probe-vulkan`, GPU clear/readback `px=4080bfff` |
| 3 | on-screen clear | done (emulator) | Surface test APK, 1,620+ frames at ~60 Hz, screenshot |
| 4 | triangle | done | `probe-vulkan-triangle`, 60 frames, screenshot |
| 5 | NVN program/pipeline/texture/draw | done | `probe-nvn-texture` (guest block-linear upload), `probe-totk-vs` visible TOTK VS/PS |
| 6 | game first frame | blocked | game still stops at `nn::gfx` context registry |
| 7 | continuous frames | done (host) | triangle 60 frames; surface 1,620+ frames; NVN 60 draws |

The game's own first frame is **not** claimed.  The renderer path is now
independently exercisable, and the remaining blocker is the guest `nn::gfx`
context registry, not Vulkan, shader translation or presentation.

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

## 6. Game first frame

The game still stops at:

```text
[crash] sig=11 pc=main+0x794dd0 lr=main+0x794db8 fault=0x10
[crash] last NVN api: nvnBufferMap
```

The `nn::gfx` context registry is empty; the guest abort path then tries to
allocate a 0x5f0-byte error/context object from a 64 KiB fallback heap and
crashes in its unwinder.  This is the same pre-existing stop documented in
`android-hle-phase2a.md`; the new Vulkan renderer, triangle, NVN draw and
Surface clear do not change it.  Shader translation is not the blocker for
this game stop.

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
- `probe-spirv`: all cached SPIR-V modules accepted by the driver.
- `probe-nvn-submit`: the older submit/present self-test still passes.

## 8. Next

1. Feed real storage-image/texel-buffer resources and guest storage ranges
   into the reflected descriptor sets at draw time; real sampled textures, NVN
   texture objects and uniform buffers are now bound after pipeline creation.
   The remaining work is the upload/copy path for images and full mip/format
   coverage.
2. Use the 2B.2 cache key when loading `nvnProgramSetShaders*` instead of a
   probe-selected path, and persist the translated `.spv + .res.json` pair.
3. Return to the `nn::gfx` context registry stop; until it is fixed, the game
   cannot reach its own first frame regardless of renderer readiness.
4. Unlock the physical device and keep the screen on, then re-run the Surface
   test; `adb install` now succeeds on the Xiaomi device.
