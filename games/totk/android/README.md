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
  `loader-ready` callback. Module initializers and `nnMain` are the next step.
- HLE thunks switch to the per-thread HostContext stack.
- The first module initializer (`lm`) is reached with
  `TOTK_ANDROID_INITS=1`; it stops at CMIF domain control, which is the next
  HLE/IPC milestone. `TOTK_ANDROID_IPC_STUB=1` is a diagnostic that answers
  domain requests with zero data.

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
- **Alias region**: the first host reserves 16 GiB at `0x7000000000`; the full
  64 GiB / shrink logic from `services.c` is next.

## Next

1. Port the remaining memory SVCs (`MapPhysicalMemory`, stack moves, precise
   `QueryMemory`) and the HLE service registry.
2. Run module `DT_INIT` constructors and stop at the first missing service.
3. Port `nn::fs`/`nn::os`/`nn::oe` host overrides, then input/audio and the
   Vulkan/NVN graphics layer.
