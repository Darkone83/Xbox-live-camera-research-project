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

## 3.5 Class-driver attach contract — CONFIRMED from the named decomp

FID named the framework, and `XInitDevices` + `USBD_*` now read directly. The
class-driver registration/attach mechanism is fully resolved:

**The class-driver table** lives at `0x1b2774 .. 0x1b2790` — a small fixed array
(~6 slots) of pointers to **class-driver descriptors**. Each descriptor:
```
descriptor[0] = class       (byte)   ┐ matched by USBD_FindClassDriver against
descriptor[1] = subclass    (byte)   ┘ the device's _PNP_CLASS_ID (low 2 bytes)
descriptor[+4] = REGISTER fn ptr     ← called by XInitDevices for every entry at init
descriptor[+8] = ATTACH fn ptr       ← called by USBD_LoadClassDriver on a matching connect
```

**Init flow** (`XAPILIB::XInitDevices`, confirmed body):
```
XInitDevices(count, preallocTypes):
    IUsbInit(count, preallocTypes)              // construct USB framework
    for each entry in table [0x1b2774 .. 0x1b2790):
        if entry != NULL: (*(entry+4))(buf)     // REGISTER each class driver
    Process()                                    // run enumeration
    HCD_EnumHardware()                           // bring up controllers
```

**Connect/attach flow** (confirmed):
```
USBD enumerates: USBD_DeviceEnumStage0 → StagePre1 → fDeviceEnumStage1 → Stage3 → Stage6
  → device _PNP_CLASS_ID determined from descriptors
  → USBD_LoadClassDriver(device, classId):
       d = USBD_FindClassDriver(classId)         // scan table, match [0]=class [1]=subclass
       device[+0x10] = d
       (*(d+8))(device)                          // ATTACH — the driver takes the device
     (no match → AddComplete(0x80000400) = STATUS error)
```

**What this means for our driver:** the camera class driver is one table entry with
`{class, subclass, register(+4), attach(+8)}`. Our RE'd `FUN_000ced10` (registration)
+ the connect callbacks map onto exactly this. To build: provide a descriptor with
our class/subclass, a register fn (declares into the framework), and an attach fn
(grabs the `IUsbDevice`, then drives our command dispatch). The `_PNP_CLASS_ID` the
EyeToy enumerates as is the match key — capture it during bring-up.

**Confirmed real names** (replacing earlier `≈` guesses in §3):
```
enumeration : USBD_DeviceEnumStage0 / StagePre1 / fDeviceEnumStage1 / Stage3 / Stage6
matching    : USBD_FindClassDriver, USBD_LoadClassDriver
addressing  : USBD_AllocateUsbAddress / USBD_FreeUsbAddress
iso pipeline: OHCD_fIsochOpenEndpoint / AttachBuffer / StartTransfer / ProcessTD /
              StopTransfer / CloseEndpoint / CompleteCloseEndpoint
transfer    : HCD_SubmitRequest, OHCD_fDequeue{Control,Bulk,Interrupt}Transfer
bring-up    : XInitDevices, IUsbInit, HCD_EnumHardware, HCD_NewHostController
```
Note: FID named **functions**, not **struct types** — the params are still
`undefined4`/`int`. Struct layouts (`_URB`, descriptors, the class-driver
descriptor) still come from our RE (§7) + USB spec + RXDK headers (§4).

## 3.6 Worked reference: the voice class driver (xvoice.lib / XHawkMediaObject)

`xvoice.lib` contains a **complete, real USB class driver** on the same stack —
`XHawkMediaObject`, the voice-communicator driver. It is the closest structural
analog to the camera (a non-gamepad USB device doing isochronous streaming), so
it's our **template**: the camera driver is "this, but RGB frames instead of audio
packets." Method set (all confirmed from xvoice.lib symbols):

```
InitializeClass(ULONG, ULONG)        static  -- class registration (the +4 register entry);
                                              args ≈ resource counts (headphone/mic)
AllocateStreamingResources(ULONG,ULONG)      acquire iso pipe + buffers   (≈ camera StartStream)
ProgramTransfer()                            build + submit the iso URB    (the iso submit)
TransferComplete(_USBD_ISOCH_TRANSFER_STATUS*, void*)   ISO completion callback (static __stdcall)
CloseEndpoint() / CloseEndpointComplete(_URB*, XHawkMediaObject*)  teardown + completion
AbortMediaPackets()                          cancel in-flight transfers
Process(_XMEDIAPACKET*, _XMEDIAPACKET*)      per-packet pump (in/out)
SetSampleRate(uchar,uchar) / SetAGC(uchar,uchar)   device-specific control transfers
GetInfo/GetStatus/Flush/Discontinuity        XMediaObject plumbing
```

Key takeaways for the camera build:
- **Iso completions deliver a `_USBD_ISOCH_TRANSFER_STATUS` struct**, not a raw `_URB`.
  This is the iso-side completion type (distinct from the control `_URB` completion).
  The camera frame-grab completion receives this. [V] layout still to be mapped —
  pull `TransferComplete`'s body / the struct from a decomp of xvoice or RXDK headers.
- **Completion callbacks are `static __stdcall`** taking `(status/urb, instance)` —
  matches the `0x40`-bit "use callback" URB path.
- The lifecycle is exactly our Phase 3-5: register class → allocate streaming
  resources (open iso pipe) → program iso transfers → handle iso completions →
  pump packets → teardown. Build the camera driver to this shape.
- New named types to define (from xvoice): `_USBD_ISOCH_TRANSFER_STATUS`,
  `_HAWK_STREAMING_RESOURCES` (voice-specific, but a model for a camera
  streaming-resources struct), `_XMEDIAPACKET`/`_XMEDIAINFO` (the XMO buffer model —
  camera can use its own simpler frame buffer instead).

**If you want the single highest-value read next:** decompile `XHawkMediaObject::
ProgramTransfer` + `TransferComplete` (from xvoice.lib imported into Ghidra) — that
is a *working* iso submit + completion on this exact stack, and it directly fills
the one gap left in `xbox_usb.h` (the iso `_URB`/`_USBD_ISOCH_TRANSFER_STATUS` arm).

## 3.7 WORKED CLASS-DRIVER SOURCE: the SLIX driver (USB.zip / USBdesc/slixd)

`USB.zip` contains Microsoft's USB test tree, and `USBdesc/slixd/` is a **complete
class-driver in C++ SOURCE** (`slixdriver.cpp` + `islixd.cpp` + `i_slixdriver.h`).
This is the single most valuable artifact for the build — the camera driver is a
variation on this. It also names the keystone header: **`#include <usb.h>`** (the
XDK one) defines the `_URB` union, the `USB_BUILD_*` macros, and all iso/transfer
arms. If you can find `usb.h`, every remaining `[V]` in `xbox_usb.h` closes at once.

**The device context (`DEVICE_EXTENSION`) — our camera ctx template:**
```c
typedef struct _DEVICE_EXTENSION {
    IUsbDevice *Device;                  // the interface we call
    VOID*  Endpoints[MAX_ENDPOINTS];     // OPENED PIPE HANDLES (the req+0x10 source)
    UCHAR  EndpointType[MAX_ENDPOINTS];
    DWORD  flags;                        // DF_CONNECTED 0x1 / DF_INITIALIZED 0x2
    USB_INTERFACE_DESCRIPTOR     InterfaceDescriptor;
    USB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    USB_ENDPOINT_DESCRIPTOR      Endpoint1, Endpoint2;
    URB    Urb;                          // inline, reused for descriptor queries
    BYTE   bConfigData[200];             // full config descriptor (wTotalLength bytes)
    BYTE   bPort, bSlot, bInterfaceNumber;
} DEVICE_EXTENSION;                       // stored via Device->SetExtension(this)
```

**Attach (`SLIX_AddDevice(IUsbDevice *Device)` = the +8 entry) — the pattern:**
```c
ulPort = Device->GetPort();                                  // topology (>=16 -> slot 1)
pIfc   = Device->GetInterfaceDescriptor();                   // class/subclass/number
memcpy(&ext->InterfaceDescriptor, pIfc, sizeof(...));
pCfg   = Device->GetConfigurationDescriptor();               // grow-read handled by framework
memcpy(&ext->bConfigData, pCfg, pCfg->wTotalLength);         // keep FULL config blob
Device->SetExtension(ext);                                   // stash our ctx
for(i) ext->Endpoints[i] = (void*)-1;                        // -1 = closed
ext->Device = Device; ext->flags |= DF_CONNECTED;
Device->SetClassSpecificType(1);
USB_BUILD_CONTROL_TRANSFER(&ext->Urb, NULL, &buf, len,
     USB_TRANSFER_DIRECTION_IN, AddCompletionRoutine, ext, TRUE,
     (USB_DEVICE_TO_HOST|USB_VENDOR_COMMAND|USB_COMMAND_TO_INTERFACE),
     USB_REQUEST_GET_DESCRIPTOR, descType, ifaceNum, len);
Device->SubmitRequest(&ext->Urb);                            // async, AddCompletionRoutine fires
// completion routine calls Device->AddComplete(USBD_STATUS_SUCCESS)
```

**THE ISO STREAMING RECIPE (islixd.cpp) — this IS the camera frame path:**
```c
// open the iso endpoint
URB urb; RtlZeroMemory(&urb, sizeof(URB));
USB_BUILD_ISOCH_OPEN_ENDPOINT(&urb.IsochOpenEndpoint,
     ENDPOINT_NUM_DIRECTION(ep), 256 /*maxPacket*/, 0);
pud->SubmitRequest(&urb);
Endpoints[n] = urb.IsochOpenEndpoint.EndpointHandle;         // <-- the pipe handle!

// start the stream
URB urb2; RtlZeroMemory(&urb2, sizeof(URB));
USB_BUILD_ISOCH_START_TRANSFER(&urb2.IsochStartTransfer,
     Endpoints[n], 0, URB_FLAG_ISOCH_START_ASAP);
pud->SubmitRequest(&urb2);

// attach buffers to receive frames
USB_BUILD_ISOCH_ATTACH_BUFFER(&urb.IsochAttachBuffer,
     ourendpoint, USBD_DELAY_INTERRUPT_0_MS, &bufd);
pud->SubmitRequest(&urb);
// teardown: USB_BUILD_ISOCH_STOP_TRANSFER -> ISOCH_CLOSE_ENDPOINT
```

**Confirmed `_URB` union arms** (each has its own USB_BUILD_ macro):
`ControlTransfer`, `BulkOrInterruptTransfer`, `OpenEndpoint`, `CloseEndpoint`,
`OpenDefaultEndpoint`/`CloseDefaultEndpoint`, `IsochOpenEndpoint` (has
`.EndpointHandle`), `IsochStartTransfer`, `IsochStopTransfer`, `IsochAttachBuffer`,
`IsochCloseEndpoint`. Endpoint encoding: `ENDPOINT_TYPE(e)=(e>>4)&3`,
`ENDPOINT_NUM_DIRECTION(e)=e&0x8f`. Flag `URB_FLAG_ISOCH_START_ASAP`,
delay `USBD_DELAY_INTERRUPT_0_MS`.

**Build implication:** the camera driver = `SLIX_AddDevice` shape (read descriptors,
find the iso video endpoint, `SetExtension`) + the iso recipe above (open iso EP on
the video endpoint, start transfer, attach frame-sized buffers, on completion
process the RGB frame). All in real, compilable Microsoft code. The control path is
the `USB_BUILD_CONTROL_TRANSFER` form. **Get `usb.h` to make `xbox_usb.h` complete.**

## 3.8 USBCAMD.SYS — the camera minidriver framework (IMPORTANT, revises strategy)

`USBCamD.lib` is the **import library for `USBCAMD.SYS`** — the generic USB *camera*
minidriver framework (the Windows USBCAMD class-driver model, ported to Xbox). This
is the substrate the original camera support was built on. It is **title-supplied,
NOT a stock kernel driver** (absent from xdrivers.txt: usbd/usbhub/usbpnp/xid/mu),
which means Video Chat shipped it **statically linked into the XBE** — so the
`0x000Cxxxx` "bespoke" region is almost certainly **USBCAMD + an OV519/OV530
minidriver linked in**, not from-scratch code. These exports name those functions.

**Exported API (what USBCAMD.SYS provides):**
```
USBCAMD_DriverEntry@20            minidriver entry / registration
USBCAMD_InitializeNewInterface@16 interface setup on connect
USBCAMD_SelectAlternateInterface@8  SET_INTERFACE / alt-setting (bandwidth select)
USBCAMD_AdapterReceivePacket@16   iso frame-data callback (frames arrive here)
USBCAMD_ControlVendorCommand@36   OV519 vendor control transfers (reg read/write)
USBCAMD_GetRegistryKeyValue@20    config lookup
USBCAMD_Debug / DllUnload
```
The classic USBCAMD split: USBCAMD handles iso streaming + KS plumbing; a tiny
**minidriver** supplies the camera-specific bits (OV519 vendor commands, format/alt
selection, per-frame processing).

### Two build paths (this find opens Path B)
**Path A — full class driver (original plan):** write at the `IUsbDevice` level, open
iso endpoints, build URBs by hand (SLIX/XID style). Max control, most work
(Phases 3-4 are all on us).

**Path B — minidriver on USBCAMD (newly visible, likely easier):** provide the OV519
specifics and let USBCAMD do the iso streaming + frame delivery. Far less iso
plumbing — USBCAMD does the hard Phase-4 part. Needs: link `USBCamD.lib`, supply the
minidriver callbacks, and have `USBCAMD.SYS` code present (it was static-linked into
Video Chat, so the framework is in our decomp — we can identify it via these export
names and reuse the pattern).

### Recommended next step (decomp, free, pre-hardware)
Apply these `USBCAMD_*` names to the decomp (they're in `0x000Cxxxx`). That will:
1. Confirm the `0x000Cxxxx` region = USBCAMD + minidriver (not opaque app code).
2. Reveal the **OV519 minidriver** — the camera-specific vendor commands, format
   tables, and per-frame handling — which is the actual thing we need to replicate.
3. Decide A vs B: if USBCAMD is cleanly separable, Path B is the lighter build.

This likely **collapses the iso-streaming risk** (Phase 4) if Path B works, since
USBCAMD owns the streaming loop.

## 4. Structs needed (and where each comes from)

**Iso buffer allocation (from mm.h):** iso/frame buffers must be **physically
contiguous** (the HC DMAs into them). Allocate with `MmAllocateContiguousMemory(size)`
(or `MmAllocateContiguousMemoryEx` for alignment), NOT regular pool;
`MmGetPhysicalAddress` if a phys addr is ever needed. Free with
`MmFreeContiguousMemory`. (`MmMapIoSpace` maps `0xFED00000` but the framework owns
the controller — we don't need it.) Confirmed kernel driver set (xdrivers.txt):
`usbd.sys`, `usbhub.sys`, `usbpnp.sys`, `xid.sys`, `mu.sys` — camera rides usbd+usbpnp.


| Struct | Source |
|---|---|
| `_USB_DEVICE_DESCRIPTOR8`, `_USB_CONFIGURATION_DESCRIPTOR`, `_USB_INTERFACE_DESCRIPTOR`, `_USB_ENDPOINT_DESCRIPTOR` | **USB 1.1 spec** — standard, self-definable |
| `_XPP_DEVICE_TYPE` | **have:** `ULONG Reserved[3]` (Xbox.h); internally `{init, connect, disconnect}` |
| `_XDEVICE_PREALLOC_TYPE` | **have:** `{ PXPP_DEVICE_TYPE; DWORD count }` (Xbox.h) |
| `_URB` | partly RE'd (§7 in USB_TRANSPORT.md); full layout from **RXDK** |
| `_USB_CLASS_DRIVER_DESCRIPTION`, `_PNP_CLASS_ID`, `_USB_RESOURCE_REQUIREMENTS`, `_HCD_RESOURCE_REQUIREMENTS` | **RXDK `include/`** (XDK-specific). NOTE: class-driver descriptor layout now confirmed from decomp (§3.5): `[0]=class, [1]=subclass, [+4]=register fn, [+8]=attach fn`. |

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
  - CONFIRMED VALUES (Xbox Cam descriptor, xboxdevwiki): interface 0, class 0xFF
    (Vendor Specific); iso IN endpoint 0x81 (EP1 IN); alt-settings select bandwidth:
    alt0=0B (idle), alt1=384B, alt2=512B, alt3=768B, alt4=896B. SET_INTERFACE to
    alt 1-4 to start; alt 0 to stop. (EyeToy: verify its descriptor matches.)

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

COMPLETE at the spec/header level. The keystone `usb.h` is folded into
`xbox_usb.h` verbatim: the full `_URB` union (all arms incl. iso),
`_USBD_ISOCH_TRANSFER_STATUS`, `_USBD_ISOCH_BUFFER_DESCRIPTOR`, the `USB_BUILD_*`
macros, and all status/function/direction constants -- zero reconstruction
guesswork remains. Framework is linkable + named; class-driver attach contract
confirmed (§3.5); a complete worked driver (SLIX, §3.7) is the template; iso
recipe is real MS code. Remaining is implementation + hardware bring-up: the
EyeToy's `_PNP_CLASS_ID` (read at attach), the first transfer round-trip, and iso
timing/stability on a retail box. Every step now has a known, typed target.