# Android CPU rewrite

This is the CPU-side milestone for an Android/arm64 port. It does **not** build
an APK, load the game or touch the graphics layer. It rewrites the TOTK 1.0.0
NSO modules (`main`, `sdk`, `subsdk0`) into images that can run natively on
Linux/Android arm64 and verifies every rewrite site offline.

The macOS/iOS output is unchanged and remains the default (`--target darwin`).

## Target differences

| Subject | Darwin (macOS/iOS) | Android |
|---|---|---|
| x18 | reserved by the OS, virtualized per thread | native; not scanned or rewritten |
| thread base | `TPIDRRO_EL0` + Darwin TSD key×8 | `TPIDR_EL0` + `GuestSlots*` offset |
| host gate | per-thread `KGATE` slot | global key-page `KGATE` address |
| parameters | per-thread TSD slots | global key-page values |
| SVC | host gate | host gate (Linux `svc` is a real syscall, so coverage is critical) |
| counters | host gate, 19.2 MHz | host gate, 19.2 MHz |
| exclusive windows | moved for x18 sites | must not contain a rewrite site; asserted |

Android target rules:

- `x18`/`w18` is not scanned. Host code must be built with
  `-ffixed-x18` or without Shadow Call Stack, or save/restore x18 at the
  guest↔host boundary. `runtime/gate.S` currently skips x18; the Android host
  gate must restore it.
- `mrs/msr tpidr_el0` and `mrs tpidrro_el0` are rewritten to a per-thread
  `GuestSlots` array.
- `svc`, `cntpct_el0` and `cntvct_el0` still go through host gates.
- A rewrite site inside an `LDXR/STXR` window is rejected. The 1.0.0 modules
  are expected to have none; a future version needs a dedicated lowering.

## ABI

`GuestSlots` (one per guest thread, pointed to by a `__thread` variable):

```c
typedef struct {
    uint64_t k18;      /* unused on Android, layout-compatible with Darwin */
    uint64_t ktls;     /* guest TPIDR_EL0 value */
    uint64_t ktlr;     /* guest TPIDRRO_EL0 value */
    uint64_t reserved[5];
} GuestSlots;
```

Key page (8 × 64-bit, after the data segment):

| Index | Name | Meaning |
|---|---|---|
| 0 | `KCTX` | offset of `GuestSlots*` from `TPIDR_EL0` |
| 1 | `KTLS` | reserved (GuestSlots index 1) |
| 2 | `KTLR` | reserved (GuestSlots index 2) |
| 3 | `KGATE` | host gate function address |
| 4 | `KRES0` | resolution parameter 0 |
| 5 | `KRES1` | resolution parameter 1 |
| 6 | `KASPECT` | aspect-ratio cap |
| 7 | `KASPECTUI` | UI aspect-ratio cap |

The host computes `KCTX` at startup:

```c
__thread GuestSlots *g_guest_slots;
offset = (char *)&g_guest_slots - (char *)read_tpidr_el0();
key[KCTX] = offset;
```

A slot read in a rewritten block is:

```asm
mrs  xT, tpidr_el0
adrp xS, key_page
ldr  xS, [xS, #KCTX*8]
add  xT, xT, xS
ldr  xT, [xT]              ; GuestSlots*
ldr  xN, [xT, #slot*8]     ; KTLS or KTLR
```

A host gate block is:

```asm
stp  x29, x30, [sp, #-16]!
adrp x30, key_page
ldr  x30, [x30, #KGATE*8]
blr  x30
ldp  x29, x30, [sp], #16
b    original+4
```

The gate identifies itself by `x30` (the return address), exactly like the
Darwin gate but without the TSD lookup.

## Commands

Rewrite and verify the real 1.0.0 modules. The normal build extracts them under
`<work>/totk/extracted/v0/exefs` (default work: `TOTK_build` next to the game
file); pass that inner `totk` folder as `--work`. In a development tree that
already has `games/totk/extracted`, omit `--work`.

```sh
python3 games/totk/scripts/prepare_android_cpu.py --work /path/to/TOTK_build/totk --jobs 4
```

Outputs:

```text
games/totk/rewritten/android-v0/{main,sdk,subsdk0}.patched.bin
games/totk/analysis/android-v0-port/android-cpu-validation.json
```

A single module can be checked directly:

```sh
PY=python3
D2R=games/totk/third_party/d2r
$PY $D2R/patch_nso.py extracted/v0/exefs/main rewritten/android-v0 main --target android
LLVM_BIN=/path/to/llvm/bin $PY $D2R/structck.py rewritten/android-v0 main extracted/v0/exefs/main
$PY $D2R/equiv.py rewritten/android-v0 main extracted/v0/exefs/main --jobs 4
```

`--funcs <file>`, `--params <file>` and `--obs <file>` are test-only hooks for
synthetic fixtures (function starts instead of `eh_frame_hdr` discovery, and
extra parameter/observation gates keyed to the fixture build id).
`--allow-unverified-input` is test-only.

## Synthetic fixture

`games/totk/tests/android/` contains a minimal NSO with one function per
rewrite kind (`svc`, TLS read/write, TLR read, counter, exclusive increment).
It needs no game data. The fixture also carries a `movz/movk` parameter gate
and a `sub sp` observation gate.

```sh
# offline structure + equivalence
python3 -m unittest tests.test_android_cpu_rewrite -v

# on an arm64 Android device/emulator (NDK required)
cd games/totk/tests/android
ANDROID_NDK_HOME=/path/to/ndk ./build.sh
./run.sh
```

The probe maps the rewritten image, fills the key page and a synthetic
TCB/GuestSlots, swaps `TPIDR_EL0` only around the fixture calls, and checks the
return values and the exclusive cell. The fixture functions and the gate do
not call libc/TLS while `TPIDR_EL0` is swapped.

## Android host milestone

`games/totk/android/` now contains the first native host: loader, key page,
relocation, `gate_android.S`, HLE-stack thunks and a minimal SVC layer.

```sh
WORK=gamebin/TOTK_build/totk games/totk/android/build.sh
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe
adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-hle
TOTK_ANDROID_TRACE_SVC=1 adb shell /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android boot-sdk
```

Verified on an arm64 Android 17 device (4 KiB pages):

- real TOTK 1.0.0 rewrite: 1024 structural sites, 924 semantic sites, 428/428
  negative controls detected;
- `probe`: main PLT → sdk → host timer gate, 4 threads × 1000 calls;
- `probe-hle`: host-stack thunk;
- `boot-sdk`: `nn::init::Start` reaches the `loader-ready` callback.
- `TOTK_ANDROID_INITS=1 boot-sdk` reaches the first module initializer (`lm`)
  and stops at CMIF domain control; domains and the real service registry are
  the next HLE milestone.

The Android host uses native TLS (`-fno-emulated-tls`, 64-byte-aligned
`__thread` variables), maps the image `R-X` first and makes data/key page
writable afterwards, and restores guest x18 in the gate.

## Scope

- TOTK 1.0.0 only.
- The first Android host loader, gate, HLE-stack and SDK-start probe live in
  `games/totk/android/`; full HLE services, module initializers, graphics,
  audio, input, packaging and gameplay remain out of scope.
- `svc`/timer gate semantics are not executed offline; only their structure is
  checked. On-device probes cover the timer gate and SDK start.
- The Android host must close the x18/SCS and `TPIDRRO_EL0` gaps described
  above; the rewriter itself only emits the code sequences.
