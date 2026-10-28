# Android Phase 2A: the null `nn::gfx` object and the NVN object graph

This note records the Phase 2A investigation on the Android/arm64 host. It
answers the question in the task directly:

> `main+0x1123f34` allocates a 0x230-byte `nn::gfx` object; the constructor at
> `main+0x1123f7c` receives `x0 = 0`. Is the allocator returning null, is an
> NVN object uninitialised, or is the guest heap range wrong?

The answer is **the allocator really returned null**, and the reason was not an
NVN object and not the heap range. The caller's per-resource size lookup
returned 0 for every entry, so the heap that should back the 0x230 object was
never created. The missing input was a RomFS resource
(`System/HeapSize/*.HeapDef.byml.zs`, plus the resource size table), not a
null-pointer check to skip.

## 1. The 0x230 allocation path

The crash site is a virtual method call from `main+0x811784`:

```
main+0x8117dc  ldr  x0, [x21, x24, lsl #3]   ; resource object
...
main+0x8117f4  blr  x8                       ; object->vtable[0xc0] = name
main+0x811838  bl   0x86686c                 ; create a sead heap from parent heap
main+0x811840  str  x0, [x19]                ; x19[0] = new heap (or 0)
...
main+0x81187c  blr  x9                       ; virtual Initialize, x1 = x19
```

The callee is `main+0x1123388`. It does:

```
main+0x11234ec  mov  w0, #0x230
main+0x11234f4  bl   0x7ff170                ; sead::operator new(size, align, heap)
main+0x11234f8  mov  x22, x0
main+0x11234fc  bl   main+0x1123ee0          ; constructor(this = x0)
```

At the crash, `[x20]` (the argument struct passed to `main+0x1123388`) was 0.
That field is the heap returned by `0x86686c`, so the callee took the
"no heap" fallback in `0x7ff170` and returned 0. The fallback also found no
current-thread heap, so the 0x230 allocation was null and the constructor
stored to address 0.

`0x86686c` creates the heap only if the **required-size vector** entry for the
resource is at least 0x101 bytes. The vector is built in `main+0x810c68` from
the resource name:

```
main+0x8119b0  ; ResourceSizeTable lookup("Heaps/<name>/Size")
main+0x810df8  ; vector[index] = size << 10
```

The crash dump showed:

```
0x810c68 size-vector count=77 data=...
0x810c68 resource[0..]: size=0
0x811784 requested heap size=0x... (a string pointer)
```

Every size was 0. The lookup failed because the game had not loaded the heap
definition resources. On this image the boot path opens:

```
content:/System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
content:/System/HeapSize/Default.Nin_NX_NVN.HeapDef.byml.zs
content:/System/HeapSize/Product.HeapDef.byml.zs
content:/System/Resource/ResourceSizeTable.Product.100.rsizetable.zs
```

With no `TOTK_ANDROID_ROMFS`, `nn::fs::MountRom("content")` fails and all of
those reads return not-found. The SDK keeps an empty size table, the size
vector stays 0, and the first heap allocation for the gfx resource fails. This
is not an NVN builder/window null and not a committed-page/heap-range error;
those were checked separately:

- `SetHeapSize(0x84200000)` succeeded at `0x330000000`.
- `MapMemory`/`UnmapMemory`/`QueryMemory` traces showed the committed stack
  area and the heap base correctly.
- The kernel never returned an error for the 0x230 allocation; the guest
  allocator itself returned 0 after the size lookup.

### Reproduction

The minimum RomFS subset that removes this crash is:

```
System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
System/Resource/ResourceSizeTable.Product.100.rsizetable.zs
System/RegionLangMask.txt
System/GameRomConfig/NormalProduct.game__RomConfig.bgyml
System/AddressTable/Product.100.Nin_NX_NVN.atbl.byml.zs
Pack/                               (Bootup/ResidentCommon/ZsDic and the rest)
RSDB/  Model/  TexToGo/  System/  Shader/  Grass/  Effect/  UI/ ...
```

For the first graphics module initialization the important files are the
HeapDef, the resource size table and `Pack/Bootup.Nin_NX_NVN.pack.zs` (which
registers the per-module `ModuleInitializeParam` resources). The full base
RomFS is the real input; a partial tree only moves the crash to the next
missing resource.

## 2. The helper facts learned on the way

### `ThreadType+0x1b0` is the guest thread handle

`nn::os::UnlockMutex` (the `sdk` implementation) does a futex only when the
owner tag has the waiters bit. The current-thread id comes from:

```
TLR+0x1f8 -> ThreadType
ThreadType+0x1b0 -> thread id
```

The loader's `ThreadType` was zero in that field, so the mutex owner never
matched and the SDK took the contention path. `runtime_thread_set_guest_handle`
now writes the guest handle (`0x1001` for the main thread) into
`ThreadType+0x1b0` after `svc_thread_main_init()`. This is a host TLS field, not
an NVN object.

### Builder zeroing must use the host struct size

The NVN 55 objects the guest places on its stack are not always as large as the
reserved sizes in the D2R `nvn.h` (which is NVN 53). Zeroing the nominal
`NVNsamplerBuilder` size (0x60) overran the caller's saved `x30`; that produced
a `pc=0, lr=0` crash after a successful sampler registration. All builder
defaults now zero `sizeof(*builder)` and only write fields the host owns. The
same applies to object initializers.

### HID

`nn::hid::InitializeMouse`/`InitializeKeyboard` activate the SDK's HID shared
memory accessor, which the host does not have. They are now host no-device
initializers next to `InitializeNpad`; without them the SDK dereferenced a null
HID singleton (`sdk+0x497fec`).

## 3. NVN object graph (`nvn_android.c`)

The previous file returned `nvn_ok`/`nvn_nop` for almost everything. It now owns
the object graph and the pointer semantics the guest expects, while still not
submitting to a GPU:

- **Device**: real device builder flags, `nvnDeviceInitialize` magic, window
  origin/depth mode, the X1 device-info table, monotonic timestamps, and
  non-zero texture/texel/image/separate handles.
- **Memory pool**: builder storage/flags, initialization, CPU map, buffer
  address, size/flags, explicit failure for a CPU-invisible map.
- **Buffer**: builder storage, range-checked initialization, map and address,
  size/offset/pool getters.
- **Texture**: full builder fields, Tegra X1 block-linear storage size and
  alignment (so the guest sizes its pools consistently), storage/view offsets,
  CPU texel write/read, packaged-data copy, address, memory pool/offset/size.
- **Texture view**: the 16-byte TOTK view layout (levels/layers/format/swizzle/
  target) with getters and compare.
- **Sampler and sampler pool**: builder parameters, sampler object, descriptor
  pool registration (writes a defined 32-byte descriptor), getters.
- **Texture pool**: initialize/register/getters.
- **Program, sync, event**: object magic, shader copies, fence/wait results,
  event storage and signal.
- **Queue**: builder sizes/priority/memory, queue initialization, submit and
  present counters. `nvnQueueSubmitCommands` logs the first explicit GPU
  submission points (`GPU submit #N ...`) even though the host is CPU-only.
- **Window**: builder native window/textures/interval/active count, window
  initialization, native-window return, acquire/present/crop.
- **Unknown object APIs** are logged as `unimplemented NVN object API` and
  return an explicit zero-returning stub. They are never silently reported as
  success.

The NVN layer does not need the Metal backend; it is a CPU-only object graph.
The guest's own `nn::gfx` objects and the SEAD heaps remain real guest objects.

## 4. Current stop

With the required RomFS subset, the guest now passes the original 0x230 null
allocation, creates memory/sampler pools, initializes the NVN device, gets past
HID keyboard/mouse, and reaches `nn::gfx` window/queue setup. The current
crash is:

```
[crash] sig=11 pc=main+0x794dd0 lr=main+0x794db8 fault=0x10
[crash] frame 0: lr=main+0xea5f4
[crash] frame 1: lr=main+0x11db37c
[crash] frame 2: lr=main+0x11dad64
[crash] frame 3: lr=main+0x1139318
...
[crash] frame 8: lr=main+0x811880          ; the gfx window virtual call
[crash] last NVN api: nvnBufferMap
```

`main+0x794d30` walks a `nn::gfx` device/context registry at
`[main+0x4542f08+0x18]` looking for the object returned by `main+0xa59ac`; the
list is empty, so it takes the error path and the guest's own exception
unwinder dereferences a null. The last NVN call is `nvnBufferMap`, which
returned a valid mapping (no invalid-buffer log). This is now an
`nn::gfx` device-registry/initialization gap, not the original null 0x230
allocation and not a skipped NVN object. It is left explicit.

The next checks are:

1. Why the `nn::gfx` device/context object created at `main+0xe9d404` is not in
   the registry walked by `main+0x794d30` (the object is handed to the SDK;
   check the `nn::gfx::Device` create/register calls and the NVN device
   capability values that gate them).
2. Continue with the full RomFS so the resource-driven module initialization
   cannot mask the next missing input.
3. The first `nvnQueueSubmitCommands` call is already instrumented; it should
   appear in the log as soon as the registry path is fixed.

This stop was resolved by the `ThreadLocalRegion+0x1f0` fix in section 9; the
registry is now populated and the game stops later, in window heap
construction.

## 5. Acceptance status

- The original `main+0x1123f7c` crash is explained: allocator null because the
  required-size lookup returned 0 for every resource; the missing input is the
  RomFS heap definition/resource size table. No null check was skipped.
- The NVN object graph is implemented with real builder fields, object magic,
  mappings, handles and explicit unknown-API errors.
- The guest now reaches `nn::gfx` window/queue construction and stops at
  `main+0x794dd0` in the `nn::gfx` device registry, with the frame chain and
  the last NVN call recorded.
- `nvnQueueSubmitCommands` and `nvnQueuePresentTexture` log the first explicit
  GPU submission/present points; no GPU work is executed.

## 6. Phase 2A.3: nvdrv is a real service

`games/totk/android/nvdrv_android.c` replaces the generic Android stub for
`nvdrv`, `nvdrv:a`, `nvdrv:s` and `nvdrv:t`.  It implements the public
`INvDrvServices` command shape:

| Cmd | Name | Host behaviour |
| --- | --- | --- |
| 0 | Open | Opens a real per-session fd for `/dev/nvmap`, `/dev/nvhost-*`, `/dev/nvdisp-*`, `/dev/nvsched-ctrl`; unknown paths return an error |
| 2 | Close | Closes the fd |
| 3 | Initialize | Accepts the process/transfer-memory handles and returns a real u32 status |
| 4 | QueryEvent | Returns a real event handle through the move table |
| 5 | MapSharedMem | Aliases the nvmap guest object to the transfer memory range, or fails explicitly |
| 8 | SetAruid | Stores the ARUID and returns success |
| 12 | Ioctl3 | Dispatches nvmap/nvhost-ctrl ioctls with the real in/out buffers |
| 13 | SetGraphicsFirmwareMemoryMarginEnabled | Stores the flag, no output |

The nvmap backing memory is **guest-visible**: a dedicated 1 GiB window at
`0x500000000` (below the 39-bit Horizon limit) is reserved; each block is
`memset` on free, `runtime_guest_commit` records the range and
`runtime_guest_map_extra` makes `QueryMemory` report the window as committed,
so the SDK's address allocator cannot hand the same address to the game.  This
is the "memory mapping and guest address match" requirement; it is not a host
`malloc` pointer.  The window is a bump allocator: freed blocks are zeroed but
not reused yet, so a long session with many texture/buffer allocations will
eventually exhaust it (the next step is a free list inside the window).

Implemented nvmap ioctls: `CREATE`, `FROM_ID`, `ALLOC`, `FREE`, `MMAP`,
`PARAM`, `GET_ID`, and the tag/cache/export calls.  Implemented nvhost-ctrl
ioctls: syncpoint read/incr/wait/wait-event, event alloc/free, module mutex,
and `GET_CONFIG`.  Unknown ioctls and unknown devices return an explicit
`0x1a8` instead of a fake success.  `GetStatus`/`InitializeDevtools` are also
explicit errors until their versioned structures are known.

A host self-test exercises the real CMIF handler end to end:

```
totk-android <dir> probe-nvdrv
```

It opens `/dev/nvmap`, creates a 0x20000-byte object, allocates it, checks
`NVMAP_IOC_PARAM` size and that the window is below 2^39, then frees/closes.
Passing on the arm64 device means Open/Ioctl3/Close no longer go through the
generic stub.

## 7. Phase 2A.4: host NativeWindow

`games/totk/android/native_window_android.c` defines the host
`HostNativeWindow`:

- width, height, NVN format (0x25 = RGBA8), bytes per pixel, stride, buffer
  count and a host framebuffer;
- an opaque `ANativeWindow*` slot that the app shell can attach later;
- `runtime_native_window_info` and `runtime_native_window_clear`.

`nn::vi::CreateLayer` / `nn::vi::GetNativeWindow` are host overrides built on
the same object.  They create a `HostViLayer` (magic, layer id, window) and
return the window pointer.  `nvnWindowBuilderSetNativeWindow` stores that
pointer, and `nvnWindowGetNativeWindow` returns it unchanged.  The default is
offscreen 1920x1080 (1280x720 in handheld mode).

```
totk-android <dir> probe-vi
```

checks layer -> native window -> 1920x1080/RGBA8/8,294,400-byte framebuffer,
a software clear, and the NVN builder/getter round trip.

The real Horizon `vi` `OpenLayer` (command 2020) carries an Android Parcel with
a `dispdrv` binder object.  The host cannot fabricate that binder without a
compositor; the host layer/native-window object is the explicit replacement in
this phase, not a silently accepted Parcel.  The VI IPC commands that the
real SDK path uses are logged; the host path is what the game currently
exercises.

## 8. Phase 2A.5: offscreen first, app shell next

The offscreen path is complete and testable without an APK:

```
totk-android <dir> probe-nvn-submit
```

builds a real NVN device/queue/window in the CPU-only layer, records a command
buffer, calls `nvnQueueSubmitCommands` once, clears the host framebuffer and
calls `nvnQueuePresentTexture`.  The log contains the first explicit points:

```
[totk-nvn] GPU submit #1 queue=0x... commands=1 (CPU-only host)
[totk-nvn] present #1 window=0x... index=0 (CPU-only host)
```

This proves the object graph and submit/present instrumentation, but it is a
host self-test, not the game's first frame.  Phase 2B.1 puts a real Vulkan
offscreen target behind the same host native window; see
`docs/android-hle-phase2b.md`.

The Android app shell is deliberately still a follow-up.  To put the
framebuffer on a Surface, the app needs:

1. a `SurfaceView` / `NativeActivity` that owns a real `ANativeWindow*`;
2. a JNI entry that runs the existing loader on a worker thread;
3. `runtime_native_window_attach_android(window, surface)` so
   `vi_create_layer_host` attaches the Surface to the same host object;
4. a renderer that writes the offscreen framebuffer (or binds a Vulkan/GL
   target) before `nvnQueuePresentTexture`.

The current executable remains the adb-driven CLI; the offscreen mode is the
supported Phase 2A target.

## 9. Current game stop (updated)

With the host VI layer restored, the game gets through:
- the original 0x230 null allocation,
- NVN device, queue, memory pools, sampler/texture pools, command buffers,
- VI layer + 1920x1080 host native window,
- `nn::gfx` context registration and window/queue construction.

The former `main+0x794dd0` stop was **not** a missing `nn::gfx` registration
call.  The SDK stores the guest `TPIDR_EL0` value in the ThreadLocalRegion at
`TPIDRRO_EL0+0x1f0` and restores it with
`msr tpidr_el0, [tpidrro_el0+0x1f0]` (`sdk+0x12fe58`, `sdk+0x1302d4`).  The
Android thread init only filled `+0x1f8` (ThreadType), so the first restore
cleared the guest TP.  The next `nn::gfx` context cache store at
`TPIDR_EL0+0x10` then faulted.

`runtime_thread_init` now also writes the guest TLS pointer to `TLR+0x1f0`.
The registry is consequently populated:

```
gfx-registry mgr=...
gfx-registry head=...: <context> <context> <vtable> 0
gfx-registry node[0]=... key+0x5e8=0x51 next=0
```

The game proceeds past the registry, creates the NVN device and graphics
threads, and reaches the Boot UI and audio initialization.  The debug RomFS
initially lacked the Boot layout archives; extracting `UI/LayoutArchive/`
(`Boot`, `Common`, `Title`) from the XCI moves the stop past the null tree
insert.  `Sound/Product.100.alto__AltoConfig.bgyml` is also needed.

The Android build now includes the real `audren:u` renderer protocol with a
null (silent) mixer backend, the host `nn::time` clock, and SVC 0x13/0x14
`MapSharedMemory`/`UnmapSharedMemory` (copy semantics for now).  With those,
the game opens the audio renderer, processes the REV11 request, mounts
save/cache storage, and reaches the next stop:

```text
[crash] sig=11 pc=sdk+0x356128 lr=sdk+0x35562c fault=0x0
[crash] last NVN api: nvnTextureGetViewOffset
[crash] last IPC: service=IAudioDevice command=7 ...
```

The next stop was in the SDK **log manager**, not a thread/name manager.
`sdk+0x3560f0` is `nn::lm::detail::ShimLibraryGlobal::GetInstance`; its guard
is the pointer at `sdk+0xd18118`, the instance holder is at `sdk+0xd18120`, and
the zeroed storage is `sdk+0xd80d70`.  The constructor at `sdk+0x356060` calls
`nn::lm::CreateLogServiceByHipc` (`sdk+0x356b50`) and the resulting
`ILogService::OpenLogger`.  The SDK's `lm` command 0 is `OpenLogger`: the host
service now returns an `ILogger` (a moved session for a direct request, a
domain object for a domain request), so `ShimLibraryGlobal` initializes.

The first log/assert no longer crashes.  The next stop was a null texture
builder, and it was **data**, not a builder API bug: the material table entry
for `SceneMaterial.ng_model7.15.x_0` failed because the base RomFS file
`TexToGo/miasma_altitude2.bntx.zs` was absent.  Adding that one base-XCI file
(the Water assets were also extracted for neighbouring materials) moves the
stop past `main+0x84cb1c` and `main+0xc49974`.

The next HLE gap was `nn::oe`:

- `SetExpectedVolumeBalance` / `GetExpectedVolumeBalance` called
  `nn::am::GetAudioController()`, which returns null in the host HLE;
- `TryPopNotificationMessage` waited on a SystemEvent that the host
  `nn::oe::Initialize` does not attach.

Those main imports now have host overrides (unity volume, no notification).
The game then reaches its **first real GPU submit**:

```text
[totk-nvn] GPU submit #1 queue=... commands=1 (CPU-only host)
[crash] sig=11 pc=main+0x7a8600 lr=main+0x7a8434 fault=0x150
[crash] last NVN api: nvnQueueFenceSync
```

At `main+0x7a8600` the renderer object's first qword is null before the
`+0x150` virtual call.  This is after the first submit, not the log manager or
a missing base RomFS file.  The remaining 56 "not found" paths are
update-only/optional and none exist in base v0.

`nvnMemoryPoolMap` still reports `pool is CPU_NO_ACCESS`; it remains a separate
pool-flag item.

## 10. Verification

All of these pass on the arm64 device:

```sh
totk-android <dir> probe
totk-android <dir> probe-hle
totk-android <dir> probe-tls
totk-android <dir> probe-nvdrv
totk-android <dir> probe-vi
totk-android <dir> probe-nvn-submit
TOTK_ANDROID_INITS=1 TOTK_ANDROID_STOP_AFTER_INITS=1 \
  TOTK_ANDROID_ROMFS=/data/local/tmp/totk-android/romfs-min \
  totk-android /data/local/tmp/totk-android boot-sdk
```

`probe-tls` still uses the boot-sdk path; it is not a separate binary.
