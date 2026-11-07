# Android TOTK Wave 0: frozen contract and lane harness

This document is the baseline every Wave 1 lane branches from (tag
`wave0-base`).  It freezes the additive backend API, the probe entry points,
and the build/device isolation rules so four lanes can work in parallel without
editing each other's files.

## 1. Scope

Wave 0 adds no rendering behaviour.  It only:

- freezes the compute/copy API in `games/totk/android/gfx_backend_android.h`;
- reserves four probe names in the loader through a weak hook file;
- gives every lane its own build directory and serializes adb access;
- records the pre-change probe baseline.

## 2. Frozen backend API (additive)

`gfx_backend_android.h` now declares:

- `GfxComputePipeline` + `gfx_backend_compute_pipeline_create/destroy`
- `gfx_backend_compute_bind_buffer/bind_texture/write_buffer/read_buffer`
- `gfx_backend_compute_dispatch` (record + submit + wait, synchronous)
- `gfx_backend_texture_copy/clear/upload/download`

Semantics are documented next to each declaration.  In particular:

- compute bindings use descriptor set 0 and the existing `GfxBindingSpec`
  reflection (no second reflection format);
- a dispatch or copy returns success only after the GPU work completed;
- storage images must have been created with storage usage; the backend
  rejects a texture that was not, instead of silently sampling it;
- `gfx_backend_texture_copy` uses `vkCmdCopyImage` for matching formats and a
  nearest-filter blit only when the formats differ.

Lane 1 implements this API; Lane 2 is the only caller.  Neither may change the
signatures.  A needed addition goes through Lane 0 as another additive commit.

## 3. Probe hooks

Probe dispatch stays in `loader/`, but the four new probes are
reserved by `probe_hooks_android.[ch]`:

| probe | strong definition | owner |
| --- | --- | --- |
| `probe-vulkan-compute` | `gfx_backend_compute_probe` | Lane 1 |
| `probe-vulkan-copy` | `gfx_backend_copy_probe` | Lane 1 |
| `probe-nvn-compute` | `nvn_compute_probe` | Lane 2 |
| `probe-nvn-copy` | `nvn_copy_probe` | Lane 2 |

Until the strong definition lands, the weak stub prints
`not implemented` and exits **77**.  A probe must never report a false pass.
`loader/` is owned by Lane 3 after Wave 0; Lane 1/2 do not edit it.

## 4. Build and run

Each lane gets a private work dir under `/tmp`; the read-only game inputs are
symlinked from `gamebin`, so parallel builds cannot overwrite one binary:

```sh
# builds /tmp/totk-lane1/build/android-v0/totk-android, pushes to
# /data/local/tmp/totk-lane1, runs under the shared device lock
TOTK_LANE=lane1 games/totk/android/devrun.sh probe-vulkan-compute

# reuse the existing binary (do not use after editing sources)
TOTK_LANE=lane1 games/totk/android/devrun.sh --no-build probe-vulkan-copy
```

`devrun.sh`:

- builds with `WORK=/tmp/totk-<lane>` (override with `WORK=`);
- links `rewritten/`, `analysis/`, `extracted/` from
  `gamebin/TOTK_build/totk` (override with `TOTK_SRC=`);
- pushes the binary to `/data/local/tmp/totk-<lane>` and symlinks the patched
  modules to the shared device copy;
- serializes every adb call through `/tmp/totk-device.lock.d`
  (`TOTK_DEVICE_LOCK`, `TOTK_LOCK_TIMEOUT`).

Shared device assets stay under `/data/local/tmp/totk-android` and are
referenced with environment variables:

- RomFS: `TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min/totk-romfs-min`
- full shader cache:
  `TOTK_ANDROID_SHADER_CACHE=/data/local/tmp/totk-android/lanes/shader/shader-cache`
- save/profile: default `/data/local/tmp/totk-android/user-data`; **title and
  save-slot runs must set**
  `TOTK_ANDROID_ACCOUNT_UID=f9cdd55b6d4c70efc1c8e47378dad985`.
  Without it the filesystem mounts the profile under UID 0, the imported
  `save/f9cd...` is invisible, and the game never reaches the UI producer
  (no 360x69/142x26 target, no copy/dispatch calls). This is not a rendering
  bug.

Wave 1 device tests are parallel only in code; adb runs queue on the lock.
Because there is one device, the 30-minute soak belongs to Wave 4: Lane 3 runs
only short smoke tests (a few minutes / a few thousand presents) during
Wave 1 and records the exact command for the final soak.

## 5. File ownership

| lane | may edit |
| --- | --- |
| 0 | `.gitignore`, `build.sh`, `devrun.sh`, `probe_hooks_android.[ch]`, `gfx_backend_android.h` (contract only), `docs/` |
| 1 | `gfx/`, new `gfx_compute_spv.h`, new `shaders/*` repo-owned test shaders |
| 2 | `nvn/` |
| 3 | `runtime/threads.c`, `loader/` |
| 4 | no production code; outputs only under `/tmp` and `/data/local/tmp` |
| 5 | `android/tools/`, `docs/` |

Rules:

- one writer per file; no lane touches another lane's file, not even a
  one-line fix;
- commit with explicit paths (`git add <file>`), never `git add -A`;
- game data, saves, keys, shader caches and `gamebin/` must never be staged;
- all lanes branch from `wave0-base`; Lane 2 branches from Lane 1's head after
  the API is green, then rebases onto integrated `main` before merge.

## 6. Acceptance gates

A Wave 4 run passes only when all of these hold:

1. 360x69 UI source reports `first_nz != none`;
2. the 0x3f target is not all zero;
3. the 1920x1080 window PPM has many colours (not just one) and the title /
   save-slot region is visible (not a loading strip);
4. A / Continue does not crash and reaches a 3D frame;
5. 30 minutes or 10k+ presents with no SIGSEGV and no 39 s core occupancy;
6. `probe-nvn-*`, `probe-vulkan-*`, `probe-shader-cache` all exit 0;
7. `git status` is clean apart from ignored work folders.

Lane 4 records the pass/fail table and evidence paths; a gate is never inferred
from a single `colors=1` frame.

## 7. Wave 0 baseline evidence

- `probe-vulkan` (offscreen selftest): pass on Adreno 830.
- `probe-vulkan-mrt`: pass, 2 colour attachments + depth.
- `probe-nvn-state`: pass.
- `probe-vulkan-compute` / `probe-nvn-copy`: exit 77 (`not implemented`), as
  intended before Wave 1.

Logs are kept outside the repository under `/tmp/totk-wave0/evidence/`.
