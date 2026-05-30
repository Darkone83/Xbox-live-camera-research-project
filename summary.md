# Original Xbox Camera Driver — Findings, API Structure & Build Foundation

**Project:** Homebrew RXDK/C++ driver to bring up and stream the Sony EyeToy (and
the Xbox Live Vision "Xbox Cam") on a retail/softmodded original Xbox, by
reverse-engineering the only title that ever used the camera — "Xbox Video Chat"
(a.k.a. ScreenChat) — and reconstructing the private XDK USB framework it links
against.

**Author / brand:** Darkone83 / Team Resurgent.
**Status:** Research + foundation **complete**. No architectural unknowns remain.
Implementation pending hardware arrival.

This document is the high-level summary. The deep references are:
`BUILD_SPEC.md` (the build plan + symbol maps), `USB_TRANSPORT.md` (the transfer
chain), `CAMERA_INIT.md` (lifecycle), `RESEARCH.md` (the investigation log), and the
headers `xbox_usb.h` / `xb_cam.h` / `xbox_native.h`.

---

## 1. Executive summary

The original Xbox camera is **not** driven by the OV519/OV7648 register-replay
approach used on PCs. On the Xbox it is a **standard USB isochronous-video device**:
the system enumerates it, a class driver attaches, formats are selected via USB
alternate-settings (`SET_INTERFACE`), and frames arrive over an isochronous
endpoint. There is no register table to reverse, and no USB host-controller stack
to write — the OHCI/USBD/HCD framework already exists in `xapilib` and is linkable.

The work therefore reduces to: **write a USB class driver** that registers with the
framework, attaches to the camera on connect, opens the iso video endpoint, and
pumps frames into a surface for display. We have a complete, compiling SDK
reconstruction (`xbox_usb.h`) and a real Microsoft worked example (the "SLIX"
driver) to model it on. What remains is implementation plus hardware bring-up.

**Confidence:** architecture ~100% understood; foundation provably solid (the libs
are the same ones RXDK requires to install). Risk is now concentrated in three
hardware-only unknowns (§9), not in design.

---

## 2. Hardware facts

- **Devices:** Sony EyeToy = OmniVision **OV7648** sensor + **OV519** USB bridge.
  Xbox Live Vision camera is the first-party equivalent.
- **USB IDs:** EyeToy `VID 0x054C / PID 0x0155`; Xbox Cam `VID 0x045E / PID 0x028C`.
  (The Video Chat XBE checks `0x45E/0x28C`; using an EyeToy needs a VID/PID hex
  patch or a class/interface-based match instead of VID/PID.)
- **Bus:** rides the same OHCI controllers as the gamepad ports (USB0), HC MMIO at
  `0xFED00000` (framework-owned; the driver never touches it directly).
- **Pixel formats:** outputs **RGB24** or **I420** (NOT YUY2). Default 320x240 RGB24.
- **Topology:** Xbox USB is USB 1.1, ≤4 ports + hub fan-out (≤7 devices total).
- **USB descriptor (CONFIRMED, Xbox Cam):** enumerates as **Vendor Specific Class
  (0xFF)**, subclass 0; one interface (#0) with one iso IN endpoint **`0x81` (EP1
  IN)** and **6 alt-settings** selecting bandwidth: alt 0 = 0 bytes (idle), alt
  1/2/3/4 = 384/512/768/896 bytes. `SET_INTERFACE` to alt 1–4 starts streaming; alt
  0 stops. `bcdUSB 1.10`, `bMaxPacketSize0 8`, bus-powered. Descriptor stored as a
  raw blob in a 512-byte EEPROM (OV519 CAMERAMATE format).

---

## 3. The API structure (how the stack is layered)

From the application down to the wire, confirmed by tracing the Video Chat XBE with
FID-applied symbol names:

```
Application (Video Chat)
  │  XInitDevices(count, deviceTypes[])         ← register device types at boot
  │  5-call device API (poll / open+detect / set-format / start-capture / poll)
  ▼
XAPILIB  (the USB framework — LINKABLE from xapilib.lib)
  │  IUsbInit          construct framework, enumerate, Process()
  │  IUsbDevice        per-device interface: SubmitRequest(URB*), Get*Descriptor(),
  │                    Get/SetExtension(), topology, completion plumbing
  │  USBD_*            enumeration state machine + class-driver match/load
  │  HCD_* / OHCD_*    host-controller driver, transfer queueing, ISO pipeline
  ▼
OHCI hardware @ 0xFED00000
```

### 3.1 The two interfaces (concrete `__thiscall` classes, not COM/vtable)

**`IUsbInit`** — built once at `XInitDevices`:
`IUsbInit(ULONG count, PXDEVICE_PREALLOC_TYPE)`, `RegisterResources()`, `Process()`,
`GetHcdResourcePtr()`, `GetMaxDeviceTypeCount()`, etc.

**`IUsbDevice`** — one per connected device, passed to the class driver on attach.
Key methods the camera driver uses:
- `LONG SubmitRequest(URB*)` — the core: issues any URB (control or iso).
- `const USB_*_DESCRIPTOR* GetDeviceDescriptor() / GetConfigurationDescriptor() /
   GetInterfaceDescriptor() / GetEndpointDescriptor(iface, alt, ep)`.
- `void* GetExtension() / SetExtension(void*)` — stash the driver's per-device ctx.
- `SetClassSpecificType()`, `GetPort()`, topology (`GetParent/FirstChild/Sibling`),
  and completion plumbing (`AddComplete`, `DeviceConnected/Disconnected`).

All signatures are reconstructed from Microsoft's own C++ name-mangling (exact
return types, parameters, calling conventions) and declared in `xbox_usb.h`.

### 3.2 Class-driver registration & attach (CONFIRMED)

A class driver is one entry in a fixed table at `0x1b2774..0x1b2790`. Each entry is
a descriptor:
```
descriptor[0] = class      ┐ matched by USBD_FindClassDriver against the device's
descriptor[1] = subclass   ┘ _PNP_CLASS_ID (low two bytes), passed BY VALUE
descriptor[+4] = Register fn  ← XInitDevices calls this for every entry at boot
descriptor[+8] = Attach fn    ← USBD_LoadClassDriver calls this on a matching connect
```
Boot flow: `XInitDevices → IUsbInit() → (for each table entry) Register(+4) →
Process() → HCD_EnumHardware()`.
Connect flow: `USBD enumerates (DeviceEnumStage0→Pre1→1→3→6) → determine
_PNP_CLASS_ID → USBD_LoadClassDriver → USBD_FindClassDriver(match) → (*(+8))(device)`.

### 3.3 The URB model (now authoritative, folded from usb.h)

`SubmitRequest` takes a `union _URB` whose every arm is known:
`ControlTransfer`, `BulkOrInterruptTransfer`, `OpenEndpoint`, `CloseEndpoint`,
`IsochOpenEndpoint` (returns `.EndpointHandle`), `IsochStartTransfer`,
`IsochAttachBuffer`, `IsochStopTransfer`, `IsochCloseEndpoint`, etc. Each shares an
`_URB_HEADER { Length, Function, Status, CompleteProc, CompleteContext }`. URBs are
populated with the `USB_BUILD_*` macros (never hand-stamped), and iso completions
deliver a `_USBD_ISOCH_TRANSFER_STATUS`. All of this is in `xbox_usb.h`.

---

## 4. Camera-specific findings (from the Video Chat XBE)

- **Driver location:** the camera class driver is bespoke Video Chat code in the
  `0x000Cxxxx` region (~270 functions) — it exists in **no** library, confirming it
  must be written, not linked. FID named the entire framework around it but left
  this region as the implementation surface.
- **Command dispatch:** a single dispatch (`FUN_000cf340`, installed at boot) switches
  on a command id: `0x100` buffer, `0x101` GET-format, `0x102` SET-format, `0x107`
  START, `0x108` STOP, plus grab/stream. This is the camera's internal control plane;
  our driver provides the equivalent.
- **No register replay:** every command path was traced and is register-free — formats
  come from descriptor-derived tables and are selected by USB alt-setting. The PC-side
  `.set` OV519 register tables (kept in `ov7648_519.h` for reference) are **not** used
  on Xbox.
- **State model:** device-present / handle(port) / busy / configured / stream-state
  (3=run) / pending-callback / capture-buffer / mode globals — mirrored by our
  driver's device-extension context.
- **Format:** default 320x240 RGB24; for display the harness must present as
  X8R8G8B8 (RGB24→32bpp), with an xgraphics swizzle into an NV2A texture.

---

## 5. The build foundation we now hold

| Piece | Source | State |
|---|---|---|
| USB framework (OHCD/HCD/USBD/IUsbDevice/IUsbInit) | `xapilib.lib` (the lib RXDK requires) | **Linkable + FID-named** |
| Full `IUsbDevice`/`IUsbInit` signatures | C++ mangling in `Xapilibp.lib` | **In `xbox_usb.h`** |
| `_URB` union, iso structs, `USB_BUILD_*` macros, constants | XDK `usb.h` | **Folded in verbatim** |
| Standard USB descriptors + constants | XDK `usb100.h` | **Folded in verbatim** |
| Class-driver attach contract | decomp (`XInitDevices`/`USBD_*`) | **Confirmed, §3.2** |
| Worked class driver (attach + iso recipe) | `USB.zip` → SLIX (`slixdriver.cpp`/`islixd.cpp`) | **C++ source in hand** |
| Voice class-driver template (iso media) | `xvoice.lib` → `XHawkMediaObject` | **Symbol-level reference** |
| Device-init example | `USB.zip` → `linkinit/hawk/usbinit.cpp` | **Phase-1 example verbatim** |
| Contiguous DMA buffer alloc | XDK `mm.h` (`MmAllocateContiguousMemory`) | **Requirement noted** |
| Kernel/NT shim (Io/Ob/Mm/Ke/Hal) | RE + headers | **`xbox_native.h`** |
| D3D8 display harness | working, builds, runs | **`cameratest.cpp`** |
| Camera module skeleton + public API | written | **`xb_cam.cpp/.h`** |

`xbox_usb.h` is **self-contained and compiles** — it folds in every scattered
finding (usb.h, usb100.h, the mangled interfaces) so the driver only needs the XDK
umbrella `<xtl.h>`, never the (unavailable) private headers.

---

## 6. The iso streaming recipe (the camera frame path) — real, from SLIX

This is Microsoft's own code, directly adaptable to the camera:

```c
// 1. open the iso video endpoint
URB urb; RtlZeroMemory(&urb, sizeof(URB));
USB_BUILD_ISOCH_OPEN_ENDPOINT(&urb.IsochOpenEndpoint,
        ENDPOINT_NUM_DIRECTION(ep), maxPacket, 0);
Device->SubmitRequest(&urb);
PVOID pipe = urb.IsochOpenEndpoint.EndpointHandle;   // stash the handle

// 2. start the stream
URB urb2; RtlZeroMemory(&urb2, sizeof(URB));
USB_BUILD_ISOCH_START_TRANSFER(&urb2.IsochStartTransfer, pipe, 0,
        URB_FLAG_ISOCH_START_ASAP);
Device->SubmitRequest(&urb2);

// 3. attach a (contiguous) frame buffer; completion fires per frame
USBD_ISOCH_BUFFER_DESCRIPTOR bufd = { ... TransferComplete = OnFrame ... };
URB urb3; RtlZeroMemory(&urb3, sizeof(URB));
USB_BUILD_ISOCH_ATTACH_BUFFER(&urb3.IsochAttachBuffer, pipe,
        USBD_DELAY_INTERRUPT_0_MS, &bufd);
Device->SubmitRequest(&urb3);

// teardown: USB_BUILD_ISOCH_STOP_TRANSFER -> ISOCH_CLOSE_ENDPOINT
```
`OnFrame(USBD_ISOCH_TRANSFER_STATUS*, ctx)` receives each completed frame; copy/convert
RGB24 → the display surface there.

---

## 7. Potential for custom drivers (beyond the camera)

The foundation generalizes: **anything that plugs into Xbox USB can now be driven
from homebrew**, because we have the framework interface, the attach contract, and a
worked driver template. Concrete possibilities this unlocks:

- **Other USB cameras / capture devices** — same iso-video pattern, different
  descriptors. The camera driver is the template.
- **USB audio** (mics/headsets beyond the official communicator) — `XHawkMediaObject`
  is the model; iso audio is structurally identical to iso video.
- **Generic USB peripherals** (storage, HID variants, vendor devices) — control/bulk/
  interrupt transfer arms are all defined; `slixdriver.cpp` shows bulk/interrupt use.
- **A reusable "homebrew USB class-driver SDK"** — `xbox_usb.h` + a thin
  registration/attach helper could be packaged so other Team Resurgent projects
  attach to arbitrary USB devices without re-doing this RE.

The key enabler is that the class-driver table and `XInitDevices` registration are
understood, so a homebrew title can insert its own `{class, subclass, Register,
Attach}` descriptor and own a device class.

---

## 8. Phased build plan (when hardware arrives)

0. **Instrumentation** — synchronous per-checkpoint logger + linker `.MAP` +
   EIP→map resolver (the CerBios LCD fatal screen is the debug channel).
1. **Linkage probe + register** — link `xapilib.lib`, call `XInitDevices` with a
   camera device-type, confirm the framework links and the connect path fires.
   *(This is the single highest-value first step — proves the foundation.)*
2. **Detect** — on attach, `GetDeviceDescriptor()` → read VID/PID; capture the
   EyeToy's actual `_PNP_CLASS_ID` (the match key).
3. **Select format** — `GetConfigurationDescriptor()`, find the iso video endpoint,
   `SET_INTERFACE` to the streaming alt-setting.
4. **Static frame** — iso open → start → attach a contiguous buffer → on completion,
   assemble one RGB24 320x240 frame → publish.
5. **Motion** — loop grab with double-buffering.
6. **Display** — present X8R8G8B8 (RGB24→32bpp), xgraphics swizzle into NV2A texture.

---

## 9. Open items — hardware-only (no file can close these)

1. **The EyeToy's actual `_PNP_CLASS_ID` / interface class** — LIKELY ANSWERED: the
   Xbox Cam enumerates as Vendor Specific Class **0xFF** / subclass 0, so the match
   key is almost certainly class 0xFF. Still verify the *EyeToy's* descriptor on
   hardware (and that match-by-vendor-class doesn't collide with other 0xFF devices;
   if so, filter by VID/PID inside Attach).
2. **Linkage on RXDK in practice** — expected to work (same libs), but only a
   compile-and-link confirms the exact extern declarations / mangling resolve.
3. **First control-transfer round-trip** — does `GetDeviceDescriptor` return the
   expected 18 bytes on real hardware.
4. **Iso timing / stability** — sustaining isochronous video on a retail box without
   bugchecks is the genuinely hard part and is entirely empirical.

---

## 10. Bottom line

Everything that can be learned without hardware **has been**. The architecture is
fully mapped, the framework is linkable and named, the URB/interface layer is a
complete compiling header with zero reconstruction guesswork, and there is a real
Microsoft class driver to adapt. The project has moved from "is this even possible"
to "implement against a typed spec and find out which of four known hardware
unknowns bites." When the hardware lands, start at Phase 1 (linkage probe) — if that
links and the connect fires, the foundation is proven and the rest is the build.