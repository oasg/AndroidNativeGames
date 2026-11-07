# TOTK Android shader dump -> cache flow

This document freezes the offline shader path used by the Android port:

```
guest NVN control/code  --(TOTK_ANDROID_SHADER_DUMP)-->  <key>.ctl + <key>.code
        |                                                        |
        |                                                  translate_dump.py
        |                                                        v
        |                                          <stage>/<key>.spv + .res.json
        |                                          <stage>/<key>-<suffix>.spv + .res.json
        v                                                        |
  runtime_shader_identity()  -------------------------------->  shader_cache/
```

It covers the UI/compute capture, the exact offline command, the cache layout,
and how to prove that the runtime key matches the file on disk.  Game-derived
bytes, dumps, caches and saves stay under `/tmp` or `/data/local/tmp`; nothing
in this flow is ever committed.

Related documents:

- `docs/android-hle-phase2b3-rendering.md` sections 9.4 and 10 (UI compute
  state and next steps);
- `docs/android-shader-cache-audit.md` (the Lane 5 cache audit and the
  `d946...` reflection);
- `docs/android-wave0-contract.md` (lane ownership and device assets).

## 1. Runtime key

`runtime_shader_identity()` in `games/totk/android/shader_cache/`
computes the cache key.  For a bound NVN shader:

```text
key_len = control[0x20] if 0x7cc <= control[0x20] < 0x880 else 0x880
length  = control[0x704]
stage   = control[0x714]            # 0=vs 1=ps 2=gs 3=tcs 4=tes 5=cs
key     = sha256(control[0:key_len] || code[0:length])
```

The `.ctl` file written by the dump is exactly `control[0:key_len]`; the
`.code` file is exactly `code[0:length]`.  Therefore:

```text
sha256(<key>.ctl || <key>.code) == <key>
```

This is the check that proves the dump and the cache entry are the same NVN
module.  It was verified for all 3,198 dump pairs used by this lane (0
mismatches).

The runtime looks in `<root>/<stage>/<key>.spv` first, then `<root>/<key>.spv`,
then the lexicographically first `<key>-*.spv` variant in the same
directories.  A variant alone is not enough: `nvnProgramSetShaders*` loads the
canonical module first and only then asks for a variant, so a missing canonical
`<key>.spv` is a cache miss even if variants exist.

## 2. Dump environment variables

Set these on the device process (the Android loader reads them with
`getenv()`; there is no config file):

| variable | meaning |
| --- | --- |
| `TOTK_ANDROID_SHADER_DUMP=<dir>` | on a cache miss, write `<key>.ctl` and `<key>.code` into this directory.  The directory is created with `mkdir()` if needed. |
| `TOTK_ANDROID_SHADER_CACHE=<dir>` | cache root the runtime reads.  Dumping only happens on a miss, so this determines what is captured. |
| `TOTK_ANDROID_TRACE_NVN=1` | log `[totk-nvn] dumped shader <stage> <key> ctl=... code=...` for every dump. |
| `TOTK_ANDROID_TRACE_SCENE_STEPS=1` | log the `360x69` / `1920x1080` UI scene steps (`[totk-scene-step] rt ... draws=...`). |
| `TOTK_ANDROID_L2_FRAME=<path.ppm>` | write the window PPM after a scene pass. |
| `TOTK_ANDROID_L2_FRAME_AT=<draw>` | pin the PPM to one NVN draw (e.g. `600`); omit to dump the first rendered scene frame. |
| `TOTK_ANDROID_L2_FRAME_EVERY=<n>` | periodic `<path>-<draw>.ppm` frames. |
| `TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min/totk-romfs-min` | game RomFS. |
| `TOTK_ANDROID_PROFILE=/data/local/tmp/totk-android/user-data` | save/temp/cache root; the `f9cd...` save lives here. |
| `TOTK_ANDROID_ACCOUNT_UID=f9cdd55b6d4c70efc1c8e47378dad985` | account UID used for the save path.  Without it the save mounts under UID 0. |
| `TOTK_ANDROID_RUN_NNMAIN=1` | continue past the `nnMain` entry into the game. |
| `TOTK_ANDROID_INPUT_SCRIPT="2.0:A:200,4.0:PLUS:200"` | scripted HID input (A/Continue, Plus). |

Do **not** set `TOTK_FREE_CORES=1` for TOTK: it invalidates the per-core guest
identity and crashes at `main+0x79a9a4` (section 9.1 of the 2B.3 document).
The virtual-core scheduler is the supported recipe.

`TOTK_ANDROID_SHADER_CACHE_MISS=1` makes the `probe-nvn-frame` selftest skip
writing its synthetic cache entry; it is not needed for a game dump and is
listed here only so the flag is not confused with `TOTK_ANDROID_SHADER_DUMP`.

## 3. Capturing a UI run

### 3.1 Targeted capture (recommended)

Keep the current cache and point the dump at a fresh directory.  Only modules
the run misses are written, which is exactly the "what is missing for this UI
state" set:

```sh
DUMP=/data/local/tmp/totk-android/lanes/shader/shader-dump-ui12
CACHE=/data/local/tmp/totk-android/lanes/shader/shader-cache

adb shell "rm -rf '$DUMP' && mkdir -p '$DUMP'"

cd /Users/oasis/code/MacNativeGames-lane5
TOTK_LANE=lane5 games/totk/android/devrun.sh boot-sdk \
  TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min/totk-romfs-min \
  TOTK_ANDROID_PROFILE=/data/local/tmp/totk-android/user-data \
  TOTK_ANDROID_ACCOUNT_UID=f9cdd55b6d4c70efc1c8e47378dad985 \
  TOTK_ANDROID_RUN_NNMAIN=1 \
  TOTK_ANDROID_SHADER_CACHE="$CACHE" \
  TOTK_ANDROID_SHADER_DUMP="$DUMP" \
  TOTK_ANDROID_TRACE_NVN=1 \
  TOTK_ANDROID_TRACE_SCENE_STEPS=1 \
  TOTK_ANDROID_L2_FRAME=/data/local/tmp/totk-android/lanes/shader/ui12.ppm \
  TOTK_ANDROID_L2_FRAME_AT=600 \
  2>&1 | tee /tmp/totk-lane5/ui12.log
```

`devrun.sh` serializes adb through `/tmp/totk-device.lock.d`, pushes the
current binary to `/data/local/tmp/totk-lane5`, and symlinks the patched
modules.  The game assets stay in the shared `/data/local/tmp/totk-android`
directory.

### 3.2 Full capture (empty cache)

To capture every shader a UI state requests, including modules that currently
hit, point the cache at an empty directory.  The run falls back to clear-only
frames (the runtime logs `shader cache MISS`), but every bound module is
dumped:

```sh
EMPTY=/data/local/tmp/totk-android/lanes/shader/cache-empty
adb shell "rm -rf '$EMPTY' && mkdir -p '$EMPTY'"
# same command as 3.1 with
#   TOTK_ANDROID_SHADER_CACHE=$EMPTY
#   TOTK_ANDROID_SHADER_DUMP=<fresh dump dir>
```

Do not point this at the real cache: an empty root makes the runtime miss on
every program and produces a much slower, black-frame run.

### 3.3 Raw adb form (no devrun)

If the binary is already on the device:

```sh
adb shell "cd /data/local/tmp/totk-android/lanes/shader && \
  env TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min/totk-romfs-min \
      TOTK_ANDROID_PROFILE=/data/local/tmp/totk-android/user-data \
      TOTK_ANDROID_ACCOUNT_UID=f9cdd55b6d4c70efc1c8e47378dad985 \
      TOTK_ANDROID_RUN_NNMAIN=1 \
      TOTK_ANDROID_SHADER_CACHE=$CACHE \
      TOTK_ANDROID_SHADER_DUMP=$DUMP \
      TOTK_ANDROID_TRACE_NVN=1 \
      ./totk-android /data/local/tmp/totk-android boot-sdk"
```

## 4. Pull and inspect the dump

```sh
mkdir -p /tmp/totk-lane5/dump-ui12
adb pull "$DUMP" /tmp/totk-lane5/dump-ui12
ls /tmp/totk-lane5/dump-ui12/*.ctl | wc -l
```

Each module is a pair:

```text
<key>.ctl    # control bytes, key_len = 0x7cc..0x880
<key>.code   # code bytes, length = control[0x704]
```

The stage is `control[0x714]`; `translate_dump.py` only accepts 0/1/5
(`vs`/`ps`/`cs`).  A pair with a short control, a missing `.code`, an
unsupported stage, or a bad `c1/length` range is skipped and listed in the
translation report.

Quick key check for a dump directory:

```sh
python3 - /tmp/totk-lane5/dump-ui12 <<'PY'
import hashlib, pathlib, struct, sys
bad = 0
for ctl in pathlib.Path(sys.argv[1]).glob('*.ctl'):
    b = ctl.read_bytes()
    length = struct.unpack_from('<I', b, 0x704)[0]
    code = ctl.with_suffix('.code').read_bytes()
    if hashlib.sha256(b + code[:length]).hexdigest() != ctl.stem:
        bad += 1
        print('MISMATCH', ctl.stem)
print('mismatches:', bad)
PY
```

## 5. Offline translation

`translate_dump.py` feeds the exact runtime bytes to the same checksum-verified
`totk_xlate` translator used by the macOS route.  It slices the Maxwell code
after the 0x30-byte SPH header (`code[0x30:length]`) and passes
`c1 = code[c1:length]`.

```sh
cd /Users/oasis/code/MacNativeGames-lane5
python3 games/totk/android/tools/translate_dump.py \
  --dump /tmp/totk-lane5/dump-ui12 \
  --out /tmp/totk-lane5/cache-ui12 \
  --answers auto \
  --clean
```

Options that matter:

- `--translator <path>`: default
  `games/totk/prebuilt/macos-arm64/totk_xlate`;
- `--answers auto`: derive one bind-state variant per canonical module from the
  translated SPIR-V and `.res.json` (same suffix spelling as
  `nvn_variant_suffix()`).  Without it only canonical modules are written.
- `--answers <rows.json>`: explicit answer rows, same format accepted by
  `prepare_shader_cache.py`;
- `--clean`: remove the output first;
- `--report <path>`: default `<out>/translation-report.json`.

Output layout (exactly what `shader_cache/` reads):

```text
<out>/vs/<key>.spv
<out>/vs/<key>.res.json
<out>/vs/<key>-<suffix>.spv
<out>/vs/<key>-<suffix>.res.json
<out>/ps/...
<out>/cs/...
<out>/translation-report.json
```

`<out>/translation-report.json` contains:

- `summary.canonical`, `summary.variants`, `summary.reflections`,
  `summary.skipped`, `summary.missing_canonical`;
- `modules[]` with `nvn_key`, `stage`, `variant`, `spv`, `reflection`, `source`;
- `keys[]` grouped by key with the canonical flag and variant list;
- `skipped[]` with the reason for each rejected `.ctl`/`.code` pair.

The canonical module is always kept as the fallback.  A variant filename is
`<key>-<suffix>` (one hyphen).  Inside the translator's temporary work
directory the job is named `<key>--<suffix>` (two hyphens) and the variant
`.res.json` still carries that internal `key` field; the audit tool accepts
both spellings.

### 5.1 Relationship to `prepare_shader_cache.py`

Both tools write the same `<stage>/<key>.spv` layout:

| tool | input | extra output |
| --- | --- | --- |
| `translate_dump.py` | runtime `.ctl`/`.code` pairs from `TOTK_ANDROID_SHADER_DUMP` | `translation-report.json` |
| `prepare_shader_cache.py` | `shader_spike.py` output + `spike-report.json` | `cache.json` |

`prepare_shader_cache.py` is the naming authority: `translate_dump.py` imports
`answer_suffix()`, `answer_attr()` and `answer_from_reflection()` from it.  Use
`translate_dump.py` for game captures and `prepare_shader_cache.py` for a
RomFS-only spike; do not mix their reports.

## 6. Merge into the device cache

Translate into a staging root, then add only files that do not exist yet.
Cache entries are content-addressed, so an existing key is identical; never
delete or truncate the live cache to "refresh" it.

```sh
CACHE=/data/local/tmp/totk-android/lanes/shader/shader-cache
STAGE=/tmp/totk-lane5/cache-ui12

# push only missing files (keeps the existing cache intact)
(cd "$STAGE" && find . -type f) | while read -r rel; do
  adb shell "[ -e '$CACHE/$rel' ]" || adb push "$STAGE/$rel" "$CACHE/$rel"
done
```

For a large batch, `adb push "$STAGE/." "$CACHE/"` is faster; it overwrites
only identical content-addressed entries.  Do not push `.ctl`/`.code`/PPM
files into the cache root.

## 7. Verify the cache

### 7.1 Layout and reflection

`audit_shader_cache.py` checks the stage directory, the SPIR-V header, the
`.res.json` pairing, the key field, and the NVN slot mapping:

```sh
python3 games/totk/android/tools/audit_shader_cache.py \
  --cache /tmp/totk-lane5/cache-ui12 \
  --dump /tmp/totk-lane5/dump-ui12 \
  --json /tmp/totk-lane5/audit-ui12.json \
  --markdown /tmp/totk-lane5/audit-ui12.md
```

It exits `0` only when there are no missing canonical keys and no layout
problems.  A missing canonical key is listed with the dump directory that
captured it; variants alone do not count as present.

### 7.2 On-device Vulkan module creation

`probe-shader-cache` walks `<root>/<stage>/<64-hex>.spv`, validates every file
with `gfx_backend_validate_spirv()` (which calls `vkCreateShaderModule` on the
Adreno 830) and counts `.res.json` pairs:

```sh
cd /Users/oasis/code/MacNativeGames-lane5
TOTK_LANE=lane5 games/totk/android/devrun.sh probe-shader-cache \
  TOTK_ANDROID_SHADER_CACHE=/data/local/tmp/totk-android/lanes/shader/shader-cache
```

Expected JSON:

```json
{"status":"pass","host":"android","probe":"shader-cache",
 "root":"/data/local/tmp/totk-android/lanes/shader/shader-cache",
 "modules":3960,"reflections":3951,"failed":0}
```

`modules` counts canonical 64-hex names only.  The probe's filename filter
(`n != 64 + 4`) skips `<key>-<suffix>.spv` variants, so a full-cache check
needs a second run against a validation mirror.  The mirror is a temporary
tree of symlinks with a 64-hex name derived from the original path; it is not
a runtime cache:

```sh
adb shell 'rm -rf /data/local/tmp/totk-lane5/spv-validate; \
  mkdir -p /data/local/tmp/totk-lane5/spv-validate/vs \
           /data/local/tmp/totk-lane5/spv-validate/ps \
           /data/local/tmp/totk-lane5/spv-validate/cs; \
  cd /data/local/tmp/totk-android/lanes/shader/shader-cache; \
  for s in vs ps cs; do for f in $s/*.spv; do \
    b=$(basename "$f"); h=$(printf "%s/%s" "$s" "$b" | md5sum | cut -c1-32); \
    h=$h$h; \
    ln -sf "$PWD/$f" "/data/local/tmp/totk-lane5/spv-validate/$s/$h.spv"; \
    r=${f%.spv}.res.json; [ -f "$r" ] && \
      ln -sf "$PWD/$r" "/data/local/tmp/totk-lane5/spv-validate/$s/$h.res.json"; \
  done; done'

TOTK_LANE=lane5 games/totk/android/devrun.sh --no-build probe-shader-cache \
  TOTK_ANDROID_SHADER_CACHE=/data/local/tmp/totk-lane5/spv-validate
```

Lane 5 measured:

| tree | modules | reflections | failed |
| --- | --- | --- | --- |
| live cache (canonical names only) | 3,960 | 3,951 | 0 |
| validation mirror (all `.spv`) | 5,819 | 5,810 | 0 |

All 5,819 SPIR-V files created a `VkShaderModule` on the Adreno 830.  The 9
modules without `.res.json` are synthetic `probe-nvn-state` entries, not game
modules; they are still usable as SPIR-V but cannot supply descriptor
reflection.  `docs/android-shader-cache-audit.md` section 1.1 lists them and
explains why no dump can regenerate the missing reflection.

### 7.3 Key/file consistency

For every canonical entry the audit tool checks `res.json.key == filename`
when the field exists.  For a variant `<key>-<suffix>.res.json` it accepts the
translator's internal `<key>--<suffix>` spelling.  If a key does not match,
the entry is reported as a problem rather than silently used.

## 8. Filling missing UI compute/copy/clear/blit modules

1. Capture with section 3.1 (targeted) or 3.2 (full).
2. Pull the dump and run section 7.1 against the live cache manifest to list
   missing canonical keys.
3. Translate the dump (section 5).
4. Push only the missing files (section 6).
5. Re-run section 7.2 and the UI run.

Lane 5's audit of the current device cache found **no missing keys**: all
3,198 dumped keys (vs 1,897 / ps 1,227 / cs 74 unique) are present as
canonical `.spv`, all 113 unique historical `shader cache MISS` keys from the
lead logs are present, and the latest UI log (`ui4.log`) has 16 cache hits and
0 misses.  See `docs/android-shader-cache-audit.md`.

Copy/clear/blit are **not** cache modules in this build.  They are NVN command
shims (`nvnCommandBufferClearTexture`, `CopyBufferToTexture`,
`CopyTextureToTexture`, `nvnCommandBufferBlit`) that are still
`nvn_zero_call()` in `nvn/`; no amount of shader-cache work fills
them.  The UI compute modules are all buffer/texture modules with zero storage
images, so they cannot write pixels directly either.  This is a Lane 2
blocker, not a cache gap.
