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
  default. `TOTK_ANDROID_RUN_NNMAIN=1` lets the original `nnMain` continue for
  the next HLE milestone; it currently stops in the guest graphics setup.
- HLE thunks switch to the per-thread HostContext stack.

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

## Run

```sh
adb push gamebin/TOTK_build/totk/build/android-v0/totk-android /data/local/tmp/totk-android/
adb push gamebin/TOTK_build/totk/rewritten/android-v0/*.patched.bin /data/local/tmp/totk-android/
adb shell chmod 755 /data/local/tmp/totk-android/totk-android
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-hle
TOTK_ANDROID_TRACE_SVC=1 adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android boot-sdk
```

Expected results on an arm64 Android device:

- `probe`: 4 threads × 1000 `GetSystemTick` calls, timer gate count 4000.
- `probe-hle`: HLE thunk returns 7.
- `boot-sdk`: reaches `{"stage":"loader-ready"}`.
- `TOTK_ANDROID_INITS=1 boot-sdk`: runs the three module `DT_INIT`s and
  reaches `{"stage":"nnMain-entry"}`.
- `TOTK_ANDROID_STOP_AFTER_INITS=1`: stops after the constructors for
  diagnostics; it implies `TOTK_ANDROID_INITS`.
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
2. Continue past `nnMain` with the graphics/input/audio services; the first
   call after focus setup currently stops in the guest graphics setup.
3. Port the remaining memory SVCs (`MapPhysicalMemory` details, stack moves,
   precise `QueryMemory`) and the rest of the HLE service registry.
