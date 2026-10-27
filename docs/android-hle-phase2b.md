# Android HLE Phase 2B.1: Vulkan backend selection

## 1. Decision

Android uses **Vulkan**, not the macOS Metal backend:

- `VkInstance` / `VkPhysicalDevice` / `VkDevice` / `VkQueue`;
- `VkSwapchainKHR` over an `ANativeWindow*` when a Surface is available;
- an offscreen `VkImage` render target for the adb CLI and the first
  bring-up frames.

The macOS `d2r_gfx` Metal backend is not reused.  Its NVN semantics are reused:

- NVN object lifetime and builder/initializer rules;
- NVN texture storage layout and format ids;
- program/shader data and control pointers;
- command recording and submit/present order.

The Metal-specific resource, pipeline and presentation code stays on macOS.

## 2. Architecture

The boundary is `games/totk/android/gfx_backend_android.h`:

```c
gfx_backend_vulkan_init();
gfx_backend_vk_format_for_nvn(nvn_format);
GfxOffscreenTarget *gfx_backend_offscreen_create(width, height, nvn_format);
gfx_backend_offscreen_clear(target, r, g, b, a);
gfx_backend_offscreen_present(target);
gfx_backend_attach_android_surface(ANativeWindow *, width, height, nvn_format);
gfx_backend_surface_present(r, g, b, a);
```

`games/totk/android/gfx_vulkan_android.c` owns:

- instance, physical device and graphics queue;
- device, command pool, one primary command buffer and a fence;
- offscreen image, image view, device memory and a host-visible readback
  buffer;
- optional Android surface, swapchain, image views and per-image layouts.

`games/totk/android/native_window_android.c` selects the backend:

| `TOTK_ANDROID_GFX_BACKEND` | Behaviour |
| --- | --- |
| unset / `auto` | try Vulkan; if unavailable or the format is unsupported, log and use the software mirror |
| `vulkan` | require Vulkan and a Vulkan target; failure returns `NULL` |
| `software` | never initialize Vulkan; use the CPU mirror |

The NVN layer in `nvn_android.c` is unchanged as the object graph.  It keeps
the NVN reserved object sizes, magics, builders, memory-pool/buffer mappings and
the actual call order.  `nvnQueuePresentTexture` forwards the final present to
the host native window, which presents through Vulkan when a target exists.

## 3. What is reused from d2r_gfx, and what is not

### NVN object semantics

Reused: the Android `nvn_android.c` object graph was built from the same NVN
contract as the desktop layer, including reserved sizes, builder defaults,
memory-pool flags, buffer mapping and submit/present instrumentation.

Not reused: Metal command encoders, Metal textures/buffers, Metal pipelines,
CAMetalLayer and the macOS presentation loop.

### Texture layout and format mapping

The Tegra X1 block-linear storage calculation remains in
`nvn_android.c` (`nvn_tex_layout`): GOB size, tile height/depth, packaged
layout, mip offsets, linear targets and TOTK's `MINIMAL_LAYOUT` path.  This is
still what sizes and offsets guest memory.

The Vulkan bridge maps NVN format ids to `VkFormat` in
`gfx_backend_vk_format_for_nvn`:

- RGBA8/RGBA8_SRGB/RGBX8 -> `R8G8B8A8_UNORM` / `R8G8B8A8_SRGB`;
- RGBA16F/RGBA16/RGBA32F;
- depth/stencil formats;
- RGB565/RGBA4/RGB5A1/RGB10A2;
- R11G11B10F/RGB9E5F;
- DXT/BC1, BC3, BC4, BC5, BC7.

Block-linear guest storage is **not** silently converted into a Vulkan image.
A Vulkan image created from NVN block-linear memory needs an explicit
external-memory / DRM-format-modifier path, and the current Adreno driver does
not expose the Tegra GOB layout directly.  Phase 2B.1 therefore uses
`VK_IMAGE_TILING_OPTIMAL` for the offscreen target and keeps the NVN layout
calculation for guest allocation.  A later texture-upload step must make that
conversion explicit (linear staging buffer or a supported modifier), not fake a
block-linear `VkImage`.

### Pipeline and shader lifecycle

The NVN `prog_t` object still stores the six shader data/control pointers and
the stage count.  The Vulkan backend does not yet create `VkShaderModule` or
`VkPipeline` objects; that belongs to the next renderer step.  Keeping the NVN
program object as the source of truth means the Vulkan pipeline cache can be
built from the same shader blob + control data without changing the game-facing
object model.

### Command order

`nvnQueueSubmitCommands` keeps the guest's command-handle order and logs the
first submits.  The Vulkan backend uses one graphics queue and one fence, so
clear/copy/present work is serialized in submission order.  There is no attempt
to reorder or merge guest submissions.

## 4. Vulkan implementation notes

- Instance API level is 1.0 for the Android 26 minimum; the device on the test
  target exposes Vulkan 1.3.284.
- Android surface extensions (`VK_KHR_surface`,
  `VK_KHR_android_surface`) and `VK_KHR_swapchain` are enabled only when the
  loader reports them.  If they are absent, offscreen targets still work and
  the log says surface presentation is unavailable.
- The offscreen image is `VK_IMAGE_TILING_OPTIMAL` with
  `COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST`.  Clear uses
  `vkCmdClearColorImage`; a GPU copy to a host-visible buffer is used for
  verification and to update the software mirror.
- The swapchain path uses `vkCreateAndroidSurfaceKHR`, chooses an RGBA8 format
  and the surface's color space, checks `TRANSFER_DST`/`COLOR_ATTACHMENT`
  support, uses FIFO present mode, and transitions each acquired image
  `UNDEFINED/PRESENT_SRC -> TRANSFER_DST -> PRESENT_SRC`.
- `vkAcquireNextImageKHR`, `vkQueueSubmit` and `vkQueuePresentKHR` use a fence
  for deterministic ordering; out-of-date swapchains are reported, not hidden.

## 5. Build change

The CLI is now linked **dynamically** with `-lvulkan -ldl` instead of `-static`.
Android's Vulkan loader is a shared library, so a static executable cannot
resolve `vkCreateInstance` or `dlopen("libvulkan.so")`; the dynamic build is the
only honest way to use the real driver.  The existing probes were rerun on the
arm64 device after this change.

## 6. Verification

New probe:

```sh
totk-android <dir> probe-vulkan
```

On the test device (Adreno 830, Vulkan 1.3.284):

```text
[totk-vk] instance=... device=... queue-family=0 api=1.3.284 device='Adreno (TM) 830'
[totk-vk] offscreen 1280x720 nvn=0x25 vk=R8G8B8A8_UNORM(37) bpp=4 image=...
[totk-vk] selftest PASS 1280x720 RGBA8 px=4080bfff device='Adreno (TM) 830'
{"status":"pass","host":"android","probe":"vulkan-offscreen"}
```

`px=4080bfff` is the GPU readback of a clear to `(0.25, 0.5, 0.75, 1.0)` in
RGBA8: `0x40 0x80 0xbf 0xff`.

Existing integration points now use the backend:

```text
probe-vi          host native window backend=vulkan 1920x1080 RGBA8
probe-nvn-submit  [totk-nvn] GPU submit #1 ... [totk-vk] present #1 offscreen
```

Backend selection tests:

```sh
TOTK_ANDROID_GFX_BACKEND=software totk-android <dir> probe-vi
TOTK_ANDROID_GFX_BACKEND=vulkan   totk-android <dir> probe-vulkan
```

The software path produces `backend=software`; the forced Vulkan path fails
loudly if Vulkan is unavailable or the target format is unsupported.

## 7. Current limits and next steps

- There is no Android app shell yet, so the real `ANativeWindow` / swapchain
  path compiles but is not exercised by an APK.  The offscreen path is the
  supported CLI target.
- NVN block-linear textures are still guest-storage only for arbitrary game
  layouts; 2B.3 added a minimal RGBA8 texture upload/sampler/descriptor path
  (`probe-vulkan-texture` / `probe-nvn-texture`).  A full block-linear/modifier
  bridge remains the next renderer task.
- NVN `prog_t` shader blobs are compiled through the offline cache:
  `nvnProgramSetShaders*` hashes the NVN control+code pair with the 2B.2 key,
  loads `<stage>/<key>.spv` (+ optional `.res.json`) from
  `TOTK_ANDROID_SHADER_CACHE` and creates the reflected pipeline on the first
  submit (`probe-nvn-frame`).  A miss presents the recorded clear instead of a
  fabricated shader; packing the real startup subset is the remaining step.
  See `docs/android-hle-phase2b2-shader-spike.md` for the spike.
- The former `nn::gfx` context-registry stop (`main+0x794dd0`) was a missing
  `ThreadLocalRegion+0x1f0` guest-TP field; it is fixed.  The boot now reaches
  the Boot UI layout archives, the audio renderer (silent Android mixer) and the
  SDK log manager, whose `lm` command 0 is `OpenLogger` and is now answered, so
  `ShimLibraryGlobal` initializes.  The current stop is the missing boot
  `TexToGo`/`Event` resource set on a texture-worker thread
  (`main+0xc49974`).  The Vulkan self-test proves the
  device/target/clear/present path independently.
- 2B.3 adds the real triangle, the NVN command-buffer draw path and an
  on-screen Surface clear test; see
  `docs/android-hle-phase2b3-rendering.md`.

The next steps are, in order:

1. 2B.2: texture storage -> Vulkan image upload and sampler/format validation;
2. 2B.3: NVN program/shader -> `VkShaderModule`/`VkPipeline` cache (done:
   `nvnProgramSetShaders*` loads the offline keyed cache and creates the
   reflected pipeline on first submit);
3. 2B.4: command-buffer clear/draw recording and a first real frame, starting
   with clear/viewport/scissor/draw and then vertex/descriptor resources;
4. 2B.5: Android `SurfaceView`/JNI app shell with
   `runtime_native_window_attach_android`.
