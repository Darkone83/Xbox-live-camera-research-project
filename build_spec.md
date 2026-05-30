# Camera reconstruction — BUILD SPEC

**Team Resurgent / Darkone83.** The buildable plan: what to link, what to write,
and the function-by-function map from our reverse-engineering to the real XDK
symbols. This is the capstone of the investigation — open this to start coding.
Detail lives in `USB_TRANSPORT.md` (the traced architecture) and `CAMERA_INIT.md`
(the verdict). Confirmed against the real XDK libs (`xapilib`, `xboxkrnl`,
`xonline`, `xgraphics`).

---

## 0. The approach in one paragraph

The Xbox USB stack is a **named, linkable C++ framework in `xapilib`** (`IUsbDevice`,
`IUsbInit`, `_URB`, OHCD with a full iso pipeline). The camera class driver is
**not** in any XDK lib — it was bespoke Video Chat code — so **we write it, using
our RE as the spec.** Plan: link `xapilib` + `xboxkrnl`, register a camera device
type via `XInitDevices`, and when it connects, drive `IUsbDevice`
(`SubmitRequest(_URB*)`, `Get*Descriptor`) for control + iso transfers; assemble
RGB24 frames; display via D3D8. No OHCI stack to write, no register tables to
replay — both fears were disproven.

---

## 1. What links from where

| Need | Lib | Notes |
|---|---|---|
| USB framework (`IUsbDevice`/`IUsbInit`/`_URB`/USBD/OHCD) | `xapilib.lib` | the whole stack, linkable, named |
| Kernel primitives (`Ke/Io/Ex/Hal/Ob/Rtl`) | `xboxkrnl.lib` | decorations verified (see §5) |
| Frame→texture swizzle | `xgraphics.lib` | optional; RXDK exposes it; display is routine |
| D3D8 display | `d3d8.lib` | harness already uses it |
| **USB struct headers** | **RXDK `include/`** | the one missing piece (see §4) |

The camera class driver: **not in any lib** (confirmed — absent from `xapilib`
*and* `xonline`). We implement it; our RE is the spec.

---

## 2. The framework API you call (xapilib — verified present)

**Registration / bring-up**
```
XInitDevices(DWORD count, PXDEVICE_PREALLOC_TYPE preallocTypes)
IUsbInit::IUsbInit(ULONG count, _XDEVICE_PREALLOC_TYPE*)   // ctor
IUsbInit::RegisterResources(_USB_RESOURCE_REQUIREMENTS*)
IUsbInit::Process()
USBD_BeginClassDescriptionTable / EndClassDescriptionTable  // class-driver table markers
USBD_LoadClassDriver(IUsbDevice*, _PNP_CLASS_ID)            // USBD matches+loads a class driver
```

**Per-device (the workhorse interface)**
```
IUsbDevice::SubmitRequest(_URB*)            // = our FUN_001be430 core
IUsbDevice::CancelRequest(_URB*)
IUsbDevice::GetDeviceDescriptor()           -> const _USB_DEVICE_DESCRIPTOR8*
IUsbDevice::GetConfigurationDescriptor()    -> const _USB_CONFIGURATION_DESCRIPTOR*
IUsbDevice::GetInterfaceDescriptor()        -> const _USB_INTERFACE_DESCRIPTOR*
IUsbDevice::GetEndpointDescriptor(if,alt,ep)-> const _USB_ENDPOINT_DESCRIPTOR*
IUsbDevice::OpenDefaultEndpoint(_URB*)
IUsbDevice::GetClassId() / SetClassSpecificType(uchar)
IUsbDevice::GetExtension() / SetExtension(void*)   // stash our per-device state here
IUsbDevice::GetPort() / GetInterfaceNumber()
USBD_AllocateMemory / USBD_CompleteRequest
```

**Framework guts (used *by* the framework, not by us — but they exist & match §12)**
```
OHCD_fIsochOpenEndpoint / fIsochAttachBuffer / fIsochStartTransfer /
OHCD_fIsochProcessTD / fIsochStopTransfer / fIsochCloseEndpoint
HCD_SubmitRequest
```

---

## 3. RE → XDK symbol map (the heart of the spec)

`W` = we write it (our RE is the spec). `L` = we link/call it (real XDK symbol).
`≈` = role match (mapped by behavior, not confirmed address).

| Our RE (XBE addr) | Real XDK mechanism | W/L | Role |
|---|---|---|---|
| URB struct (§7 layout) | `_URB` | L | the request block |
| `FUN_001be430` (submit+KEVENT wait) | `IUsbDevice::SubmitRequest(_URB*)` + event | L | sync transfer |
| descriptor reads in `FUN_001baa80` | `GetDeviceDescriptor` / `GetConfigurationDescriptor` | L | enumerate |
| `FUN_001bfd30` (iso setup, hdr `0x4030`) | ≈ `OHCD_fIsochOpenEndpoint` + `fIsochStartTransfer` | L | iso bring-up |
| `FUN_001bfc50` (cfg-desc read + parse) | `GetConfigurationDescriptor` + parse | L | alt-setting table |
| `FUN_001b8445` (TD enqueue) | ≈ `OHCD_fIsochAttachBuffer` / iso TD mgmt | L | queue a transfer |
| `FUN_001b8358` (doorbell) | ≈ OHCD schedule kick | L | start HC work |
| `FUN_001b414c` (completion → cb) | `_URB` completion: `(*urb+0x08)(urb, urb+0x0C)` | L | frame-ready callback |
| `XPP_DEVICE_TYPE` @ `0x1b295c` | a `_XPP_DEVICE_TYPE` we construct | **W** | the camera device type |
| `FUN_001ba120` (INIT cb) | our device-type init callback | **W** | lock+state+driver init |
| `FUN_001ba190` (CONNECT cb) | our connect callback (port#, setup, present) | **W** | device arrived |
| `FUN_001ba240` (DISCONNECT cb) | our disconnect callback | **W** | device removed |
| `FUN_001bdfa0` (USBD marshaller) | build `_URB`s → `SubmitRequest` | **W** | command marshaller |
| `FUN_000cf340` (dispatch) | our class-driver command handler | **W** | command switch |
| `FUN_000ce5c0`/`ce0a0` (SET format) | format index → alt-setting select | **W** | set format |
| `FUN_000cdef0` (alt-setting cfg) | `SET_INTERFACE` + iso endpoint config | **W** | select stream iface |
| `FUN_000cdd80` (START stream) | build+submit iso `_URB`s (`0x4030`) | **W** | start streaming |
| `FUN_000ced10`→`FUN_001be360` (reg) | install our dispatch / class-driver desc | **W** | register driver |
| class match (lib) | `_USB_CLASS_DRIVER_DESCRIPTION` + `USBD_LoadClassDriver` | L | USBD loads our driver |

The `W` rows = the camera class driver we implement. The `L` rows = the framework
we call. The RE gives the exact behavior each `W` row must reproduce.

---

## 4. Structs needed (and where each comes from)

| Struct | Source |
|---|---|
| `_USB_DEVICE_DESCRIPTOR8`, `_USB_CONFIGURATION_DESCRIPTOR`, `_USB_INTERFACE_DESCRIPTOR`, `_USB_ENDPOINT_DESCRIPTOR` | **USB 1.1 spec** — standard, self-definable |
| `_XPP_DEVICE_TYPE` | **have:** `ULONG Reserved[3]` (Xbox.h); internally `{init, connect, disconnect}` |
| `_XDEVICE_PREALLOC_TYPE` | **have:** `{ PXPP_DEVICE_TYPE; DWORD count }` (Xbox.h) |
| `_URB` | partly RE'd (§7 in USB_TRANSPORT.md); full layout from **RXDK** |
| `_USB_CLASS_DRIVER_DESCRIPTION`, `_PNP_CLASS_ID`, `_USB_RESOURCE_REQUIREMENTS`, `_HCD_RESOURCE_REQUIREMENTS` | **RXDK `include/`** (XDK-specific) |

The only real fetch left is the RXDK USB headers for the XDK-specific structs.

---

## 5. xbox_native.h — confirmed kernel decorations

From `xboxkrnl.lib` (exact). `@name@N` = `__fastcall`; `_name@N` = `__stdcall`;
`N` = stack bytes (`N/4` = dword args).

```
__fastcall  @IofCallDriver@8            (2)    __fastcall  @ObfDereferenceObject@4   (1)
__stdcall   _ObReferenceObjectByName@20 (5)    __stdcall   _IoCreateDevice@24        (6)
__stdcall   _IoAllocateIrp@4            (1)    __stdcall   _IoInitializeIrp@12       (3)
__stdcall   _ExAllocatePoolWithTag@8    (2)    __stdcall   _ExFreePool@4             (1)
__stdcall   _KeInitializeEvent@12       (3)    __stdcall   _KeWaitForSingleObject@20 (5)
__stdcall   _KeInitializeSemaphore@12   (3)    __stdcall   _KeReleaseSemaphore@16    (4)
__stdcall   _KeSetEvent@12              (3)    __stdcall   _KeSetEventBoostPriority@8(2)
__stdcall   _KeDelayExecutionThread@12  (3)    __stdcall   _KeGetCurrentIrql@0       (0)
__stdcall   _HalGetInterruptVector@8    (2)    __stdcall   _RtlInitializeCriticalSection@4 (1)
```
Action: align `xbox_native.h` to these and add `KeSetEventBoostPriority`. (USB stack
is **not** in `xboxkrnl` — it's all `xapilib`, so no USB externs go in this header.)

---

## 6. Phased build plan

```
Phase 0  Instrumentation FIRST
  - synchronous flush-per-checkpoint logger (one deletable file)
  - emit/keep linker .MAP; small EIP->.map resolver
  - CerBios LCD fatal screen is the other channel (Arg2=EIP -> .map)

Phase 1  Register + bring-up                              [tests: linkage + descriptor triggers]
  - construct a camera _XPP_DEVICE_TYPE {init, connect, disconnect}  (RE: ba120/190/240)
  - XInitDevices(..., {&cameraType, n}); confirm the connect callback fires
  - if linkage to IUsbDevice/IUsbInit fails -> that's THE wall; pivot to replicate

Phase 2  Detect                                           [milestone: first transfer]
  - in connect: get IUsbDevice; GetDeviceDescriptor -> VID/PID
  - 054C:0155 (EyeToy) or 045E:028C (Xbox Cam)
  - (optional sensor PID read 0x0A/0x0B -> 0x76/0x48 via control _URB)

Phase 3  Select format                                    [no register replay]
  - GetConfigurationDescriptor -> find video interface, alt-settings, iso EP
  - pick 320x240 RGB24 by index; SET_INTERFACE to the streaming alt   (RE: cdef0)

Phase 4  Static frame                                     [the headline goal]
  - build iso _URB (header 0x4030), SubmitRequest; completion cb -> frame ready
  - assemble one RGB24 320x240 frame                       (RE: cdd80 + §12)
  - hand to XCam_PublishFrame() (already wired)

Phase 5  Motion
  - loop the grab; double-buffer iso transfers              (RE: cdd80 scheduling)

Phase 6  Display
  - preview texture X8R8G8B8; RGB24->32bpp; XGraphics swizzle as needed
```

---

## 7. Open / can-only-close-on-hardware

- RXDK USB struct headers (§4) — last fetch.
- Does RXDK's `xapilib` expose `IUsbDevice`/`IUsbInit`/class-driver registration?
  It must (controllers use it); confirm in RXDK source.
- Hardware: device-type triggers, first transfer round-trips, iso timing, stability.

## 8. Assets on hand

```
cameratest.cpp / gfxfont.h        working D3D8 harness (shows state; ready for frames)
xb_cam.cpp / xb_cam.h             camera-module skeleton (public API wired)
xbox_native.h                     kernel shim (align to §5)
ov7648_519.h                      .set tables — REFERENCE ONLY (Xbox path is descriptor-driven)
input.cpp / input.h               trimmed ScreenChat input
USB_TRANSPORT.md                  the traced architecture (incl. §12 frame path)
CAMERA_INIT.md                    the register-free verdict + lifecycle
RESEARCH.md / README.md           background + file index
```

## 9. One-line status

Architecture traced end to end; framework is linkable and named; camera driver is
fully blueprinted; the only fetch left is RXDK's USB headers. From here it's
implementation + the hardware grind — every step now has a known target.