# Android Phase 1: SEAD ThreadMgr, TLS, and the service trace

This note records the Phase 1 work on the Android/arm64 host. The image is the
1.0.0 rewrite in
`gamebin/TOTK_build/totk/rewritten/android-v0/main.patched.bin` (SHA-256
`fc6aca25...c0d423a`). Results below were reproduced on the arm64 device
(`ro.product.cpu.abi=arm64-v8a`, Android 17, 4 KiB pages).

The Phase 0 crash was:

```
[crash] sig=11 pc=main+0xeba4f4 lr=main+0x866a58 fault=0x48
[crash] frame 1: lr=main+0xe9b6f8
[crash] frame 2: lr=main+0xe9af3c
[crash] last HLE: nn::oe::SetOperationModeChangedNotificationEnabled(bool)
```

It is gone. Phase 1 now passes SEAD initialization and reaches the first service
gaps after `nnMain` starts.

## 1. DT_INIT: state 1 means the constructors were skipped

The Android key page exposes the compiler's `DT_INIT` state word at
`main+0x4597a70`. Phase 0 read `state=1` and assumed it might mean "constructors
ran but the module has no fini callback". It does not.

`DT_INIT` (`main+0x30`) calls the imported `__nnmusl_init_dso`
(`sdk+0x8d880`) and stores its `w0` in the state word:

```
main+0x11c  str w0, [x20, #0xa70]      ; state
main+0x120  ldr w8, [x20, #0xa70]
main+0x124  cmp w8, #1
main+0x128  b.eq main+0x154            ; state == 1 -> return, no ctor loop
main+0x13c  ... load .init_array[start]
main+0x144  ldr x8, [x20], #8          ; constructor
main+0x148  blr x8
```

`sdk+0x8d880` returns `2` only when the module has a fini callback
(`[module+0x158] != 0`); without one it returns `1` at `sdk+0x8e2d0`. The
1.0.0 Android rewrite takes the `1` path, so **the `.init_array` loop in
`DT_INIT` never runs**. The constructors were not skipped because of a bad
`__nnmusl_init_dso` pointer: the loader resolves that GOT slot to
`sdk+0x8d880` correctly.

A direct probe confirms the two returns:

```
dso(0,0,...)                -> 0x2   (early "no arguments" path)
dso(real 18 arguments)      -> 0x1   (no fini callback)
```

Replacing only the return value with `2` made the guest run the constructor
loop, which proved the state semantics. The permanent fix is a loader init hook,
not a guest patch:

```
if (state == 1)
    run_init_array(&module);       /* loader_android.c */
```

`run_init_array()` walks the module's `DT_INIT_ARRAY`/`DT_INIT_ARRAYSZ` range
and calls every non-null entry on the guest stack. The guest image is not
modified. On 1.0.0 the counts are:

```
constructors main    (host pass): 10539 of 10539
constructors subsdk0 (host pass): 45 of 45
constructors sdk     (host pass): 116 of 116
```

`TOTK_ANDROID_CTOR_PROBE=1` prints the state before `DT_INIT`, the number of
entries, the number of non-null entries, the post-`DT_INIT` state and the count
actually called. This is the construction counter/probe requested in Phase 1A.

The constructors alone do not publish `sead::ThreadMgr`; `main+0x463c000` is
still zero after the constructor pass. SEAD creates it later from
`main+0xe9b828`, but the logging allocation at `main+0xe9b6f4` happens first and
takes the current-heap fallback. That fallback is what needs a real root heap.
The loader now gives the guest a real stack/TLS environment (section 2), and
SEAD initialization proceeds through `nn::oe::Initialize` and
`nnMain`.

## 2. TLS / thread-local semantics

### Guest TLS layout used by the rewrite

The Android rewrite does not use the Darwin TSD keys. It uses:

```
TPIDR_EL0 + KCTX_OFF        -> GuestSlots*
GuestSlots[KTLS]  (+0x00)   -> 8 KiB guest TLS
GuestSlots[KTLR]  (+0x10)   -> 0x200 ThreadLocalRegion
```

The loader sets the key page `KCTX` entry to the offset of the per-thread
`g_slots` pointer, and stores that pointer in the thread's TLS. This is the same
indirection the offline verifier uses in `third_party/d2r/equiv.py`
(`TCB + 0x100 -> GuestSlots`).

### Why `GetCurrentStackInfo` aborted

The SDK's `nn::os::GetCurrentStackInfo` (`sdk+0x12ba20`) calls
`nn::os::detail::DebugImplByHorizon::GetCurrentStackInfo` (`sdk+0x12f378`).
The Android rewrite replaces its `mov x8, sp` with:

```
mrs  x8, TPIDR_EL0
... load GuestSlots[KTLR] into x8
b    sdk+0x12f384
```

The function then reads:

```
x9  = [KTLR + 0x1f8]       ; ThreadType*
x10 = [x9 + 0x70]
x11 = x9 + 0x50            ; or x10 + 0x40
x9  = x9 + 0x58            ; or x10 + 0x48
x9  = [x11]                ; stack base
x10 = [x9]                 ; stack size
```

and aborts if `KTLR` is not inside `[base, base+size)`. On Horizon the TLS is
carved out of the thread's stack, so the check works. The first Android host
put the 8 KiB TLS in `calloc()` memory outside the stack, while guest code ran
on the host pthread stack. `nn::os` recorded the host stack, and the check
aborted.

The fix has two parts:

1. `runtime_guest_stack_info()` (services_android.c) reports the guest stack
   range (`low_space + STACK_OFF`, `STACK_SIZE`).
2. `runtime_thread_init()` carves a per-thread 1 MiB committed stack area from
   the top of the guest stack, places `TLS (8 KiB) + TLR (0x200) + ThreadType
   (0x400)` at the bottom of that area, fills `ThreadType+0x50/+0x58` with the
   area base/size, and stores the ThreadType pointer at `TLR+0x1f8`.  The
   loader also tracks committed pages so `QueryMemory` reports the 1 MiB area
   rather than the whole 256 MiB reservation.
3. Guest entry points (`nn::init::Start`, `nnMain`) are called through the
   small `runtime_guest_call` trampoline (`thread_enter_android.S`), which
   switches `sp` to the guest stack before running guest code. HLE calls still
   switch to the per-thread host stack through `gate_android.S`.

After this, the SDK stores the guest stack in the ThreadType:

```
[totk-android] thread tls tlr=0x31ff02000 dummy=0x31ff03000 stack=0x31ff00000+0x100000
[totk-android] tls state ktlr=0x31ff02000 tt=... base=0x31ff00000 size=0x100000
```

and the abort at `sdk+0x12f40c` is gone.

### `probe-tls`

`totk-android <dir> probe-tls` starts `nn::init::Start`, runs the module
constructors, then:

```
AllocateTlsSlot(&slot, nullptr)            -> result 0, slot 2
SetTlsValue(slot, 0x1122334455667788)
GetTlsValue(slot) == 0x1122334455667788
spawn a second host thread:
    runtime_thread_init() (independent guest TLS/ThreadType)
    SetTlsValue(slot, 0xaabbccddeeff0011)
    GetTlsValue(slot) == 0xaabbccddeeff0011
main thread's value is still 0x1122334455667788
GetCurrentThread() != nullptr
GetCurrentStackInfo(&base,&size) covers KTLR
FreeTlsSlot(slot)
```

Result on the device:

```
{"status":"pass","host":"android","probe":"tls","slot":2,
 "thread_independent":true,"stack":"0x31ff00000+0x100000"}
```

Notes / still open in Phase 1B:

- `SetTlsValue` has no return value in this SDK; it writes
  `ThreadType+0x80+slot*8` through `TLR+0x1f8`. The probe therefore checks the
  value, not a return code.
- `FreeTlsSlot` is exercised. The per-thread **destructor callback** is not yet
  called on host-thread exit; that needs a guest-thread exit path and is
  tracked for the next TLS pass.
- `SetMemoryAllocatorForThreadLocal` is not exercised yet. It belongs to the
  TLS allocator, not the slot layout, and is the next probe to add.

## 3. Service trace and dependency graph

Command used:

```sh
TOTK_ANDROID_INITS=1 \
TOTK_ANDROID_RUN_NNMAIN=1 \
TOTK_ANDROID_TRACE_HLE=2 \
TOTK_IPC_TRACE=1 \
/data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android boot-sdk
```

Observed order (abridged; `[hle]` first-call order, `[ipc]` every request):

```
nn::init::Start
  sm: Connect / GetServiceHandle("lm")
  lm: Initialize(process 0)
  nn::oe::Initialize, aoc::Initialize, err::InitializeContextWriterApi,
  pctl::Initialize, diag::InitializeApplicationAbortObserver,
  nn::oe::SetCpuBoostMode
  main DT_INIT (state 1 -> loader init_array pass)
  subsdk0 DT_INIT, sdk DT_INIT
  nnMain
    nn::oe::SetCpuBoostMode
    nn::oe::SetFocusHandlingMode(2)
    nn::oe::SetOperationModeChangedNotificationEnabled
    sead::ThreadMgr init / logging / createInstance
    nn::oe::Initialize (second call on the game path)
    nn::oe::GetDesiredLanguage / GetOperationMode / GetPerformanceMode /
              GetCurrentFocusState
    nn::fs: MountRom, OpenFile ...
    sm: GetServiceHandle("nvdrv")
      nvdrv: command 3, 8, 13
    sm: GetServiceHandle("vi:m"), ("vi:s"), ("vi:u")
      vi:m: command 2          -> IApplicationDisplayService
      app: command 102, 101, 100, 1010, 5202
    nvdrv: command 0, 12, 2
    sm: GetServiceHandle("acc:u0")
      acc:u0: command 3, 0 (domain), 160, 150
    nvnBootstrapLoader / nvnDeviceInitialize / nvnDeviceGetInteger
    vi host layer/window objects
    nn::hid::InitializeNpad (host no-pad)
    nn::gfx window/queue setup
      null object -> crash at main+0x1123f7c (current gap)
```

Services opened up to the crash:

```
lm        (LogManager, first module initializer)
nvdrv     (GPU driver, generic stub so far)
vi:m      (manager root service)
vi:s      (system root service)
vi:u      (user root service)
acc:u0    (account service for the application)
```

HLE functions called up to the current `nn::gfx` crash (summary, 36 calls to
23 functions):

```
6  nn::fs::OpenFile
3  nn::hid::InitializeNpad
3  nn::oe::SetOperationModeChangedNotificationEnabled
2  nn::oe::SetCpuBoostMode
2  nvnBootstrapLoader
2  nn::oe::Initialize
2  nn::oe::GetDesiredLanguage
1  nn::oe::SetFocusHandlingMode
1  nn::oe::SetPerformanceConfiguration
1  nn::fs::QueryMountRomCacheSize
1  nn::fs::MountRom
1  nn::vi::CreateLayer
1  nn::vi::GetNativeWindow
1  nn::oe::GetOperationMode
1  nn::oe::GetPerformanceMode
1  nn::oe::GetCurrentFocusState
1  nnMain
1  nn::aoc::Initialize
1  nn::err::InitializeContextWriterApi
1  nn::pctl::Initialize
1  nn::diag::InitializeApplicationAbortObserver
1  nn::applet::GetAppletResourceUserId
1  nn::applet::TryPopFromApplicationParameterChannel
```

### v1.0.0 vi root / app display

`vi:m` is the `ISystemRootService` used by `nn::vi::Initialize`. Command 2
(`GetDisplayServiceWithProxyNameExchange`) returns an
`IApplicationDisplayService` session; the loader implements that session as a
real IPC session (not a fake result). The boot path then sends:

```
102  GetManagerDisplayService         -> side service session
101  GetSystemDisplayService          -> side service session
100  GetRelayService                  -> side service session
1010 OpenLayer (72-byte request)      -> layer id
5202 GetDisplayVsyncEvent             -> event handle
```

The root/IPC lifecycle now works. The app-display side services are real
sessions; commands they do not implement report `IPC_RESULT_UNKNOWN_COMMAND`
instead of a fake success, so the next gap is visible. The layer id and vsync
event are the minimum values the SDK consumes. No Vulkan/Metal renderer is
connected, and `OpenLayer` does not yet return a real native-window buffer.
Those are Phase 2 items.

### nvdrv / acc

The first command that still goes to the generic Android stub (result 0, no
protocol objects) is:

```
[totk-ipc] stub service nvdrv command 0 (type 6, 4 bytes)
[ipc] unknown command service=nvdrv command=0 type=6 data=00000000
```

nvdrv commands 0, 12, 2 (plus the earlier 3, 8, 13) are still the generic
Android stub. This is the first clear C2 gap, and it is left explicit instead
of pretending the GPU is initialized.

`acc:u0` is a minimal one-local-user service. Commands 160 (8-byte input) and
150 (1-byte output) are answered with the sizes the client proxy expects.
Command 160 has no output; command 150 returns one byte. The nickname,
language and region are still supplied by `nn::oe`; there is no account profile
object yet.

## 4. SEAD task follow-up: it was the NVN device, not am/applet

The `main+0x214a5e0` crash was traced to the global at `main+0x463c048`:

```
main+0x214a5a0  ldr x22, [main+0x45586f0]   ; -> main+0x463c048
main+0x214a5dc  ldr x9,  [x22]
main+0x214a5e0  ldr x8,  [x9, #0x20]        ; x9 == 0
```

`main+0x463c048` is the **NVN device object**. The graphics initializer at
`main+0xe9d404` creates it only after:

```
nvnBootstrapLoader("nvnDeviceInitialize")       ; main+0xe9d530
nvnBootstrapLoader("nvnDeviceGetProcAddress")   ; main+0xe9d540
nvnDeviceGetInteger(device, 0) == 55
nvnDeviceGetInteger(device, 1) == 15
```

The Android `nvnBootstrapLoader` was still the generic stub returning 0, so the
device was never created and `main+0x463c048` stayed null. This is **not** an
am/applet lifecycle problem; `sead::ThreadMgr` is published
(`main+0x463c000 = 0x330900200`) before the task runs.

`games/totk/android/nvn_android.c` now implements the initialization contract:

- `nvnBootstrapLoader` / `nvnDeviceGetProcAddress` return real host entries.
- `nvnDeviceInitialize` returns success.
- `nvnDeviceGetInteger` reports the Tegra X1 table; entries 0/1 are `55/15`.
- memory-pool, buffer, texture, sampler, program, queue and window builder/
  initialize entries are opaque host objects that return success and preserve
  the native window set by `nvnWindowBuilderSetNativeWindow`.

With this, `main+0x463c048` is non-null, and the crash at
`main+0x214a5e0` is gone. The SP/stack work then exposed two more real gaps:

- **Thread stack guards**: `QueryMemory` reported the whole low reservation as
  mapped, so `AddressSpaceAllocator` could not find a guard page. The loader now
  tracks committed pages, reports only those, and implements SVC `0x04`
  `MapMemory` (copy) / `0x05` `UnmapMemory`. `MapPhysicalMemory` accepts low
  guest pages as well as the alias window.
- **VI layer/window**: the SDK's `CreateLayer` validates the compositor's
  indirect-layer table. Phase 1 has no compositor, so the public
  `nn::vi::CreateLayer` / `GetNativeWindow` imports now return real host layer
  and native-window objects (the desktop host does the same with its Metal
  layer). The SDK display/initialize/vsync path is unchanged.
- **HID**: `InitializeNpad`'s hid service/shared-memory path is replaced by the
  host `nn::hid` initialization override. It reports no connected Npad and
  zeroes Npad/six-axis state; vibration is a stub. Android input comes later.

## 5. Current next gap

After NVN, VI and HID initialization, the guest reaches its `nn::gfx`
window/queue setup and crashes here:

```
[crash] sig=11 pc=main+0x1123f7c lr=main+0x1123ef4 fault=0x8
[crash] frame 0: lr=main+0x1123500
[crash] frame 1: lr=main+0x811880
[crash] frame 2: lr=main+0x811014
[crash] last IPC: service=acc:u0 command=150 object=1 type=6 data=0 result=0
```

`main+0x1123f34` allocates a 0x230-byte `nn::gfx` object and calls its
constructor; the constructor receives a null `this` at `main+0x1123f7c`
(`strb wzr,[x0,#8]`, `x0=0`). This is inside NVN window/queue object setup, after
`nvnWindowBuilderSetDefaults`/`nvnWindowBuilderSetDevice` and the NVN proc
lookups. The next step is the real NVN window/queue object graph (or the exact
allocator that returned null), not more service stubs.

The service-side gaps still open, in trace order:

1. `nvdrv` initialization commands 0/12/2 (device, memory, swapchain); it is
   still the generic Android stub.
2. NVN window/queue/present object graph and native-window object semantics
   behind `main+0x1123f7c` (Phase 2 renderer boundary).
3. `audio`, `am`/`applet` lifecycle, `settings`: not reached yet.

## 6. Acceptance status

Verified on the arm64 device:

- `probe`: pass (4 threads x 1000 timer calls, 4000 total).
- `probe-hle`: pass (result 7).
- `boot-sdk`: pass (`loader-ready`).
- `TOTK_ANDROID_INITS=1 boot-sdk`: pass (`module-inits`), constructors run.
- `TOTK_ANDROID_STOP_AFTER_INITS=1`: pass.
- `probe-tls`: pass (single-thread, cross-thread, stack info).
- `TOTK_ANDROID_RUN_NNMAIN=1`: no longer crashes at `main+0xeba4f4` or at the
  SEAD task object `main+0x214a5e0`; SEAD initialization completes,
  `nn::oe initialized on the host` is logged, and the NVN device object is
  created.
- The next crash is `main+0x1123f7c`, a null `nn::gfx` window/queue object
  after NVN/VI/HID initialization.

Not in Phase 1:

- Vulkan/Metal renderer and first frame.
- Complete input/audio.
- A real `OpenLayer` native window.
- Performance work.
