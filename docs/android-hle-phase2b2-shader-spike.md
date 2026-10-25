# Android HLE Phase 2B.2: NVN shader -> SPIR-V spike

## 1. Result

**The translation chain is feasible for a selected shader set, using the
existing translator.**  No new Maxwell decoder was written for this spike.

- 112 unique shaders sampled from `Shader/system`, `Shader/tree` and
  `Shader/material` (TOTK 1.0.0):
  - 42 vertex shaders, 70 pixel shaders;
  - 92/92 system+tree and 20/20 material translated successfully;
  - 0 translation failures.
- Total SPIR-V: 3,113,444 bytes; average 27,798 bytes; smallest 988 bytes,
  largest 96,776 bytes.
- All 112 modules were accepted by the **real Adreno 830 Vulkan driver** via
  `vkCreateShaderModule`; 0/112 failed.
- SPIR-V version reported by the translator is 1.3.

This is a **sample**, not a claim that every one of the game's ~28,769 shaders
translates.  It proves the chain is viable and gives the size/failure baseline
for the next decision.

## 2. What already existed

The macOS route already had the important part:

- `games/totk/prebuilt/macos-arm64/totk_xlate` is the NativeAOT Ryujinx
  translator.
- `games/totk/tools/totk_xlate/Program.cs` translates Maxwell code with
  `TargetLanguage.Spirv`, `TargetApi.Vulkan`, writes `<key>.spv` and
  `<key>.res.json`.
- `Service.cs` then runs SPIRV-Cross on that `.spv` to produce the macOS MSL
  and reflection.
- `games/totk/scripts/prepare_shader_seed.py` already extracts the shader
  control/code pairs, forms `c1` and invokes the service.

Therefore Android does **not** need a new NVN-to-SPIR-V translator for 2B.2.
It needs to stop the existing chain after SPIR-V and package that result.

The macOS `dmsh_build.py` flow gives the map for the Android equivalent:

```text
TOTK RomFS .bfsha/.sharcb
  -> NVN control + Maxwell code
  -> Ryujinx translator (TargetLanguage.Spirv)
  -> <key>.spv + reflection
  -> [macOS] SPIRV-Cross -> MSL -> metallib
  -> [Android] SPIR-V shader cache -> VkShaderModule -> VkPipeline
```

## 3. Spike tool

New host-side spike:

```sh
python3 games/totk/android/tools/shader_spike.py \
    --romfs /tmp/totk-romfs-min \
    --out /tmp/totk-shader-spike
```

The 112-shader sample in this report was produced with:

```sh
python3 games/totk/android/tools/shader_spike.py --romfs /tmp/totk-romfs-min \
    --out /tmp/totk-shader-spike-all \
    --packages 'Shader/system.Product.100.product.Nin_NX_NVN.bfsha.zs,Shader/tree.Product.100.product.Nin_NX_NVN.bfsha.zs' \
    --all
python3 games/totk/android/tools/shader_spike.py --romfs /tmp/totk-romfs-min \
    --out /tmp/totk-shader-spike-material \
    --packages 'Shader/material.Product.100.product.Nin_NX_NVN.bfsha.zs' \
    --limit-per-file 20
```

It:

1. reads the selected `.bfsha.zs` packages from the extracted RomFS;
2. decodes TOTK Zstandard and parses BNSH/BAHS control+code records;
3. writes the exact Maxwell code slice and `c1` used by the existing
   translator;
4. invokes the checksum-verified `totk_xlate`;
5. checks SPIR-V magic/header/version and writes `spike-report.json` / `.tsv`.

Options:

```sh
--all                 # every unique shader in the selected packages
--limit-per-file N    # default 2
--packages A,B,C      # default system, tree, material
--keep-code           # keep the sliced Maxwell code for inspection
```

Generated game bytes default to `/tmp/totk-shader-spike`; they are not added
to the repository.

## 4. On-device validation

The Android build now has `probe-spirv`:

```sh
adb push /tmp/totk-shader-spike-all/out /data/local/tmp/totk-android/spv
adb shell 'TOTK_ANDROID_SPV_DIR=/data/local/tmp/totk-android/spv \
  /data/local/tmp/totk-android/totk-android \
  /data/local/tmp/totk-android probe-spirv'
```

`gfx_backend_validate_spirv` checks:

- 4-byte size alignment and minimum header size;
- magic `0x07230203`;
- header version fields;
- real `vkCreateShaderModule` on the selected Vulkan device.

Result:

```text
[totk-vk] SPIR-V module OK name='system-vs-75ff918f.spv' bytes=1612 version=1.3
...
{"status":"pass","host":"android","probe":"spirv","modules":112,"failed":0}
```

This catches bad SPIR-V before pipeline creation and proves the Adreno driver
can consume the translator's output.

## 5. Decision and risk

### What is proven

- The existing Ryujinx translator emits Vulkan-targeted SPIR-V from real TOTK
  Maxwell shaders.
- The sample covers simple system shaders and larger material/tree shaders.
- The Android Vulkan driver accepts every sampled module.
- The output size is reasonable for a startup/title subset.

### What is not proven

- All ~28,769 game shaders, all binding variants and all instructions.
- The full shader cache size: at 27.8 KiB average, 28,769 modules would be
  roughly 800 MiB before deduplication; a startup-only subset is much smaller.
- On-device translation.  `totk_xlate` is a macOS arm64 NativeAOT program and
  does not run on Android.  The Android route must be an **offline SPIR-V
  cache**, not a runtime .NET translator.
- Pipeline compatibility: `vkCreateShaderModule` validates the module, but a
  draw pipeline still needs the NVN binding/reflection map, descriptor sets
  and render-pass formats.

### Time-box decision

Proceed with **SPIR-V shader cache** as the Android shader route:

1. Translate the startup/title shader set offline on macOS with the existing
   tool; store `<variant>.spv` + reflection under the Android asset/cache dir.
2. Key it by the existing control+code SHA-256 plus bind-state suffix (the same
   identity the macOS shader seed uses).
3. At runtime, load the SPIR-V by key and create `VkShaderModule`/`VkPipeline`.
4. On cache miss, do not pretend a shader exists: log the key and fall back.
   For the first frame, the fallback is a clear/simple triangle, not a
   promise of full game visuals.

This keeps the first Android frame achievable while the full shader set and
pipeline binding work continue.

## 6. Next steps

- 2B.2a: translate the startup/title shader subset (not all 28k) and measure
  the packaged size.
- 2B.2b: map the NVN reflection (`cbufs`/`sbufs`/`textures`/`images`) to
  Vulkan descriptor sets and create the first real pipeline.
- 2B.2c: run the first actual draw; keep the clear/simple-triangle fallback for
  missing shader variants.
- 2B.3: Android Surface/JNI app shell.

The spike tool and `probe-spirv` stay as regression checks for every later
translator/cache change.
