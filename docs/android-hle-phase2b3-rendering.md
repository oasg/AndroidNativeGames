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

The program currently owns a host-created Vulkan pipeline from translated
SPIR-V.  A game NVN `prog_t` still carries the original shader data/control
pointers; cbuf UBOs are mapped from `.res.json`, while sbufs/textures/images and
full mip/format coverage remain the next descriptor step.

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
pipeline and both descriptor sets were created and drawn.  The backend also
implements SSBO/storage-image/combined-sampler descriptors, but the sampled
textured PS currently stalls Adreno pipeline compilation and is not claimed as
working yet.  That is the remaining descriptor work.

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
- `probe-totk-reflect`: two-set descriptor pipeline from `.res.json`
  cbufs; the current constant-black PS produces `visible:false`, while the
  pipeline/draw path passes.
- `probe-spirv`: all cached SPIR-V modules accepted by the driver.
- `probe-nvn-submit`: the older submit/present self-test still passes.

## 8. Next

1. Feed the NVN texture bridge from guest texture memory/layout automatically,
   and map the full translated `res.json` reflection to descriptor sets.
2. Map translated TOTK SPIR-V into `nvnProgramSetShaders*`; use the 2B.2 cache
   key, not a hardcoded built-in pipeline.  The current translated-shader probe
   executes a draw but its system/material pair produces no visible fragments;
   real game geometry needs vertex/UBO/descriptor binding.
3. Return to the `nn::gfx` context registry stop; until it is fixed, the game
   cannot reach its own first frame regardless of renderer readiness.
4. Unlock the physical device and keep the screen on, then re-run the Surface
   test; `adb install` now succeeds on the Xiaomi device.
