# Android Phase 0: HLE diagnosis and the first `nnMain` gap

This note records the Phase 0 instrumentation on the Android/arm64 host and the
static analysis of the first `nnMain` crash.  The binary inspected here is the
1.0.0 rewrite in `gamebin/TOTK_build/totk/rewritten/android-v0/main.patched.bin`
(SHA-256 `fc6aca25...c0d423a`).

## Instrumentation

### Host-call trace (`TOTK_ANDROID_TRACE_HLE`)

`games/totk/android/hle_stack_android.c` now mirrors the desktop
`runtime/hle_stack.c`, and `hle_stack_android.S` ports `hle_note_kept`:

- `runtime_hle_names[]` keeps the first import name for each host function.
- `runtime_hle_note()` counts every wrapped call, remembers the last call, and
  in mode 2 prints the first call of each function:
  `[hle]    0.123 h=0x... nn::oe::Initialize`.
- `runtime_hle_describe()` demangles C++ imports with `__cxa_demangle`; the
  build links `libc++abi`/`libunwind` for it.
- `runtime_hle_trace_set()` starts/stops tracing; the exit summary is sorted by
  call count.
- `hle_note_kept` saves `x0..x8`, `x16` and `q0..q7`, calls
  `runtime_hle_note(x17)`, restores everything and then enters the host
  function.  `runtime_hle_switch` only takes that path when
  `runtime_hle_trace` is nonzero.

`TOTK_ANDROID_TRACE_HLE=0` counts quietly and prints only the summary at exit;
`=1` counts and prints the summary; `=2` also logs the first call of each
function.  While tracing is on, the last wrapped call is remembered so it is
available to the crash handler.

### IPC trace (`TOTK_IPC_TRACE`)

`games/totk/runtime/ipc.c` records every request in the same layout the crash
report prints:

```
[ipc] service=lm command=0 object=1 type=6 data=8 result=0
```

The last request is kept even when tracing is off.  With tracing on it also
prints the opened `sm:` service, unknown services, unknown domain objects and
unknown commands.  On Android, an unknown `sm:` service now keeps its requested
name (`vi:m`, `nvdrv`, ...) instead of the generic `stub`, so the next missing
service is readable in the log and in the crash report.

### Crash report

`games/totk/android/loader_android.c` maps both `pc` and `lr` to
`module+offset`, walks the guest `x29` chain through `/proc/self/mem` (a stale
frame pointer ends the walk instead of faulting again), and prints the last
HLE call, the last IPC request and the guest thread handle:

```
[crash] sig=11 pc=main+0xeba4f4 lr=main+0x... fault=0x48 sp=0x... x8=0
[crash] frame 0: fp=0x... lr=main+0x...
[crash] frame 1: fp=0x... lr=main+0x...
[crash] last HLE: nn::os::GetTlsValue
[crash] last IPC: service=lm command=0 object=1 type=6 data=8 result=0
[crash] guest thread handle=0x...
```

## How to collect the dependency graph

```sh
TOTK_ANDROID_INITS=1 \
TOTK_ANDROID_RUN_NNMAIN=1 \
TOTK_ANDROID_TRACE_HLE=2 \
TOTK_IPC_TRACE=1 \
/data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android boot-sdk
```

The `[hle]` lines are in first-call order up to the crash; the `[ipc]` lines
show the `sm:` opens and the last request.  The crash block then names the last
HLE/IPC and the guest frames.

### Observed run (Android 15 arm64 emulator; identical on a physical arm64 device with Android 17 and 4 KiB pages)

```
[hle]    0.477 h=0x1001 __rel_plt_end
[ipc] service=sm: command=0 object=0 type=16 data=0 result=0
[totk-ipc] opened service lm
[ipc] sm opened service=lm
[ipc] service=sm: command=1 object=0 type=17 data=8 result=0
[ipc] service=lm command=3 object=0 type=7 data=8 result=0
[ipc] service=lm command=0 object=0 type=7 data=8 result=0
[totk-ipc] lm initialized for process 0
[ipc] service=lm command=0 object=1 type=6 data=8 result=0
[hle]    0.477 h=0x1001 nn::oe::Initialize()
[hle]    0.477 h=0x1001 nn::aoc::Initialize()
[hle]    0.477 h=0x1001 nn::err::InitializeContextWriterApi()
[hle]    0.477 h=0x1001 nn::pctl::Initialize()
[hle]    0.477 h=0x1001 nn::diag::InitializeApplicationAbortObserver()
[hle]    0.477 h=0x1001 nn::oe::SetCpuBoostMode(nn::oe::CpuBoostMode)
...
[hle]    0.478 h=0x1001 nnMain
[hle]    0.478 h=0x1001 nn::oe::SetFocusHandlingMode(nn::oe::FocusHandlingMode)
[totk-android] focus handling mode 2
[hle]    0.478 h=0x1001 nn::oe::SetOperationModeChangedNotificationEnabled(bool)
[crash] sig=11 pc=main+0xeba4f4 lr=main+0x866a58 fault=0x48 sp=... x8=0
[crash] frame 0: fp=... lr=main+0x866a58
[crash] frame 1: fp=... lr=main+0xe9b6f8
[crash] frame 2: fp=... lr=main+0xe9af3c
[crash] frame 3: fp=... lr=main+0xe99a48
[crash] frame 4: fp=... lr=main+0xe995f8
[crash] frame 5: fp=... lr=0x... (runtime_hle_switch)
[crash] frame 6: fp=... lr=sdk+0xfa0b4
[crash] last HLE: nn::oe::SetOperationModeChangedNotificationEnabled(bool)
[crash] last IPC: service=lm command=0 object=1 type=6 data=8 result=0
[crash] guest thread handle=0x1001
[hle] tracing off after 0.5 s: 13 calls to 10 functions
```

The backtrace is the important part: the crash is inside the `bl 0x8eef10`
at `main+0xe9b6f4` (frame 1 returns to `main+0xe9b6f8`), called from
`main+0xe9af24` (frame 2 returns to `main+0xe9af3c`), from the
`nn::oe::Initialize` wrapper at `main+0xe99a3c` (frame 3 returns to
`main+0xe99a48`).  So `sead::ThreadMgr::createInstance` (`main+0xe9b828`) has
not run yet; the crash is in the allocation that the ThreadMgr initializer does
before publishing the singleton.  The last HLE is the notification setter, not
`SetFocusHandlingMode`, because the focus call happened slightly earlier.

`probe`, `probe-hle`, `boot-sdk` and `TOTK_ANDROID_INITS=1 boot-sdk` all still
pass on the same emulator; the boot stops at `nnMain-entry` as before.

## What `main+0xeba4f4` actually reads

Disassembly of the 1.0.0 image:

```
eba4d8  stp  x29, x30, [sp, #-0x20]!
eba4dc  str  x19, [sp, #0x10]
eba4e0  mov  x29, sp
eba4e4  adrp x8, 0x4557000
eba4e8  mov  x19, x0
eba4ec  ldr  x8, [x8, #0x798]     ; GOT slot main+0x4557798
eba4f0  ldr  x8, [x8]             ; sead::ThreadMgr* (main+0x463c000)
eba4f4  ldr  w0, [x8, #0x48]      ; ThreadMgr->tls_slot  <-- crash
eba4f8  bl   nn::os::GetTlsValue
```

The GOT slot has a RELR entry:

```
where=0x4557798 kind=1027 addend=0x463c000
```

so after relocation `[main+0x4557798] = main+0x463c000`, and
`[main+0x463c000]` is the `sead::ThreadMgr*` singleton.  The singleton is
created lazily at `main+0xe9b828` (`sead::ThreadMgr::createInstance`):

```
e9b83c  ldr  x19, [x22]           ; existing instance?
e9b840  cbnz x19, done
...
e9b884  str  x21, [x8, #0xff8]
e9b888  str  x19, [x22]           ; publish ThreadMgr*
e9b890  ... return x19
```

The constructor part calls `nn::os::AllocateTlsSlot(&mgr->slot, nullptr)` at
`main+0xe9b8d8..0xe9b8e8`; that is the slot later read at `+0x48`.  The
initializer is reached from the main init path:

```
main+0xe99580  (module/application init)
  -> main+0xe995f4
     -> main+0xe99a18
        -> main+0xe99a3c   (before the imported nn::oe::Initialize)
           -> main+0xe9af24 (sead ThreadMgr init)
              -> main+0xe9b698
                 -> main+0xe9b828 (createInstance, publishes main+0x463c000)
```

The last host log before the crash is `focus handling mode 2`, emitted by our
`nn::oe::SetFocusHandlingMode` override from the call at `main+0xe995b8`.
Immediately after it the same path runs the notification setters, the SEAD
initializer above, and then `nn::oe::Initialize` (our override prints
`nn::oe initialized on the host`, which is not seen).  Therefore the crash
happens after focus handling but before `nn::oe::Initialize` returns, while
`main+0x463c000` is still zero.

The getter at `main+0xeba4d8` is the "current heap" accessor used when a SEAD
allocation is made without an explicit heap.  For example the allocator at
`main+0x86686c` does:

```
866a48  adrp x8, 0x4557000
866a4c  ldr  x8, [x8, #0x768]
866a50  ldr  x0, [x8]
866a54  bl   main+0xeba4d8       ; current heap
```

so an allocation with a null heap takes the `nn::os::GetTlsValue` path and
dereferences the not-yet-published `ThreadMgr` singleton.

### Answer to 0.4

- The null global is **`main+0x463c000`**, a `sead::ThreadMgr*`; it is not an
  `nn::gfx`, NVN, `vi` or `dispdrv` object.
- The import on the crash path is **`nn::os::GetTlsValue`**.  The slot at
  `ThreadMgr+0x48` is allocated by **`nn::os::AllocateTlsSlot`**.  Both are
  guest SDK functions, not an unimplemented Horizon IPC service.
- The missing subsystem is the game's **SEAD `ThreadMgr` initialization**
  (`sead::ThreadMgr::createInstance`, `main+0xe9b828`), which has not published
  the singleton when the first current-heap allocation happens.
- It is an **initialization-order / init-not-run** problem, not a service
  returning 0.  The observed backtrace (`main+0xe9b6f8` -> `main+0xe9af3c` ->
  `main+0xe99a48` -> `main+0xe995f8`) is inside the initializer's logging
  allocation, before `main+0xe9b828`.
- That allocation is made with a null heap (`main+0xe9b6e0` sets `x0 = xzr`
  before `bl 0x8eef10`), so the allocator takes its current-heap fallback at
  `main+0x866a48`; the fallback is what calls `main+0xeba4d8`.  The logging
  heap global at `main+0x455710` is also zero at that point.  Both therefore
  need to be checked when fixing the SEAD bootstrap: create/publish
  `ThreadMgr` before the logging allocation, or give that call a valid root
  heap.

The task description mentions `main+0x369d798` as the source of `x8`.  In this
1.0.0 image `0x369d798` is inside a read-only string (`sead::ThreadMgr`
namespace data), not a pointer slot.  The actual chain is
`0x4557798 -> 0x463c000 -> ThreadMgr`.  Offsets are version-specific; the
disassembly above is for the verified `android-v0` rewrite.

## Dependency graph up to the crash

Observed order (only the first call of each HLE function is listed; the
summary counted 13 calls to 10 functions):

```
nn::init::Start
  __rel_plt_end                        (unresolved linker symbol -> stub; not a service)
  sm: command 0                        (Connect/Initialize)
  sm: command 1                        -> opened service lm
  lm command 3                         (control: QueryPointerBufferSize)
  lm command 0                         (control: ConvertCurrentObjectToDomain)
  lm command 0 object=1                (lm::Initialize; last IPC before the crash)
  nn::oe::Initialize
  nn::aoc::Initialize
  nn::err::InitializeContextWriterApi
  nn::pctl::Initialize
  nn::diag::InitializeApplicationAbortObserver
  nn::oe::SetCpuBoostMode
  main / subsdk0 / sdk DT_INIT
  nnMain
    nn::oe::SetCpuBoostMode             (second call)
    nn::oe::SetFocusHandlingMode(2)     -> "focus handling mode 2"
    nn::oe::SetOperationModeChangedNotificationEnabled   <-- last HLE
      sead ThreadMgr init (main+0xe9af24 -> main+0xe9b698)
        logging allocation (main+0xe9b6f4, heap = 0)
          current-heap fallback (main+0x866a48)
            main+0xeba4d8 -> nn::os::GetTlsValue(ThreadMgr->slot)
            ThreadMgr (main+0x463c000) == null -> SIGSEGV at main+0xeba4f4
      nn::oe::Initialize                 (not reached; no IPC/SVC after the crash)
```

The important result is that the first `nnMain` gap is inside SEAD, before
`nn::oe::Initialize` and before any of the graphics services.  There is no IPC
request after the `lm` initialization.

## Next HLE service list (do not start with Vulkan)

Priority order for the next stage:

1. **Fix the SEAD `ThreadMgr` bootstrap.**  The trace confirms that
   `nn::os::GetTlsValue` runs before `ThreadMgr` is published, from the
   logging allocation at `main+0xe9b6f4` (null heap).  Create/publish
   `ThreadMgr` before that allocation, or pass it a valid root heap.  This is
   not a service to implement.
2. **`nn::os` TLS / thread-local / heap semantics.**  `AllocateTlsSlot`,
   `GetTlsValue`, `SetTlsValue`, `SetMemoryAllocatorForThreadLocal`,
   `GetCurrentThread`, `GetCurrentStackInfo`.  These live in `sdk`; validate
   that the Android HostContext guest TLS (8 KiB) and the KCTX key page expose
   the slots the SDK expects.
3. **`nn::vi`** (`vi:m`/`vi:s`, `dispdrv`): `Initialize`, `OpenDefaultDisplay`,
   `CreateLayer`, `GetDisplayVsyncEvent`, `GetNativeWindow`.  First make the
   IPC exchange and handles correct; a fake success is not enough.
4. **`nvn` / `nvdrv`**: `nvnBootstrapLoader` and the GPU service.  Only after
   `vi` has a layer/window; keep the existing Metal backend as the eventual
   implementation, do not add a Vulkan path just to get past a boot call.
5. **`nn::hid`**: `InitializeNpad`, supported Npad ids/styles, Npad states,
   six-axis, vibration.
6. **`nn::audio`**: audio renderer open/start and the device sink.
7. **`nn::am` / `nn::applet`**: lifecycle, focus and applet messages (the
   `GetAppletResourceUserId` stub is already there).
8. **`nn::account` / `nn::settings`**: preselected user, nickname, language and
   region.
9. **`nn::nfp` / `nn::ldn` / `nn::prepo`**: after the core boot path.

Each item should be added only when the ordered trace shows the game actually
calls it; the trace and the crash report are the source of truth for the next
service.

## Acceptance status

- `TOTK_ANDROID_TRACE_HLE=2`: verified on an Android 15 arm64 emulator and a
  physical arm64 device (Android 17, 4 KiB pages); ordered first-call log, and
  the sorted summary is printed on a crash as well as at `atexit`.
- `TOTK_ANDROID_TRACE_HLE=0`: verified; quiet counting and the sorted summary
  when `nnMain-entry` stops the process.
- `TOTK_IPC_TRACE=1`: verified; one line per request plus opened/unknown
  service/object/command lines.
- Crash report: verified; module offsets, guest frames, last HLE, last IPC and
  thread handle.
- `main+0xeba4f4`: identified as the null `sead::ThreadMgr` singleton.
- `probe` (4000 timer calls), `probe-hle` (result 7), `boot-sdk` (loader-ready)
  and `TOTK_ANDROID_INITS=1 boot-sdk` (nnMain-entry) all pass on the emulator.
- Next work is the service list above, not a Vulkan renderer.
