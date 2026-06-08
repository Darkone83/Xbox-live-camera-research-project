# Original Xbox Camera — Reverse-Engineering Research Log
## Team Resurgent / Darkone83

> **Heads up — this is the old log, kept for the trail it leaves, not as instructions.**
> The two driver models worked out below were both wrong in the end. This section
> argued the camera is a standard USB iso-video device that the framework enumerates
> and a class driver attaches to on connect. An earlier branch chased an
> IOCTL/registry/usbcamd model (talking to a pre-loaded system driver from usermode);
> that's covered and buried in §9. The actual shipped driver does neither — it claims
> the camera manually off the device tree and replays the OV519 registers. For how it
> really works, read `WORKING_IMPLEMENTATION.md`; where this log disagrees with it,
> the working doc wins.
>
> The hardware facts gathered here held up — the camera object layout, the OHCI
> confirmation, the EEPROM descriptor, the EyeToy compatibility notes — so they're
> kept below for reference.

This is the investigation log, warts and all. The current build-facing docs are
`summary.md` (overview), `WORKING_IMPLEMENTATION.md` (the proven pipeline),
`OV519_OV7648_INIT.md` and `MJPEG_FRAME_FORMAT.md` (the register and frame detail),
`USB Transport.md` (transfer chain), `Camera init.md` (lifecycle), and the headers
`xbox_usb.h` / `xb_cam.h`.

---

## Table of Contents
1. Overview & verdict
2. Hardware
3. Source material & method
4. The USB framework (what we link against)
5. Class-driver registration & attach
6. The URB model & iso streaming
7. Camera object internals (from the XBE)
8. EyeToy compatibility & patch
9. Superseded model (IOCTL/registry) — why it was abandoned
10. Confidence assessment
11. Remaining unknowns (hardware-only)
12. References

---

## 1. Overview & verdict

The only retail software that ever used the original Xbox camera was the Japan-only
"Xbox Video Chat" title (MS-124 / ScreenChat), built for the first-party **Xbox Video
Camera** — the rare "Xbox Cam," not the later Xbox 360 "Xbox Live Vision" camera. The
EyeToy is register-compatible with it. Reversing that XBE — with FID-applied framework
symbol names — is where this log started, and at the time it pointed us at:

**The camera is a standard USB isochronous-video device.** It is enumerated by the
USB framework; a class driver attaches on connect; formats are descriptor-derived
and selected by USB alternate-setting (`SET_INTERFACE`); frames arrive over an
isochronous IN endpoint. There is **no OV519 register-table replay** anywhere in the
Xbox driver — every command path is register-free. The PC-side `.set` register
tables are not used on Xbox.

**The homebrew approach** is therefore to write a USB **class driver** against the
framework that already exists (and is linkable) in `xapilib`. No host-controller
stack needs to be written; the OHCI/USBD/HCD layer is present and named.

---

## 2. Hardware

### Xbox Video Camera / "Xbox Cam" (first-party, Japan-only)
- Sensor/bridge: OmniVision OV530 (register-compatible with the EyeToy's OV519). USB `VID 0x045E / PID 0x028C`.
- Output: RGB24 or I420 (NOT YUY2). Default 320x240 RGB24.

### Sony EyeToy — CONFIRMED COMPATIBLE
- OmniVision **OV7648** sensor + **OV519** bridge. USB `VID 0x054C / PID 0x0155`.
- Register-level compatible with the Xbox Cam bridge; once the VID/PID detection
  check is satisfied (patch or class-based match), the driver drives either chip
  identically.

### Bus / topology
- Rides the OHCI controllers shared with the gamepad ports (USB0). HC MMIO at
  `0xFED00000` (framework-owned). USB 1.1, ≤4 ports + hub (≤7 devices).

### USB descriptor — CONFIRMED (parsed from the actual EEPROM binary)
The Xbox Cam's full descriptor is known and **parsed directly from the 512-byte
EEPROM dump** (`Xbox_Camera_EEPROM.bin`; see `EEPROM_DESCRIPTOR.md` for the full
annotated breakdown). It pins down every value our driver needs:
- Device: `bcdUSB 1.10`, `bDeviceClass 0`, `bMaxPacketSize0 8`, VID `0x045E` PID
  `0x028C`, 1 configuration. `wTotalLength 0x59` (89 bytes).
- **Interface 0 enumerates as `bInterfaceClass 0xFF` (Vendor Specific), subclass 0,
  protocol 0.** → the class-driver **match key is class 0xFF / subclass 0x00**
  (answers the `_PNP_CLASS_ID` question for the Xbox Cam; verify on the EyeToy).
- **One iso IN endpoint `0x81` (EP1 IN)**, with **6 alternate settings** selecting
  bandwidth via `SET_INTERFACE`:

  | Alt | wMaxPacketSize | use |
  |----|----|----|
  | 0 | 0    | idle / zero-bandwidth (default when not streaming) |
  | 1 | 384  | streaming |
  | 2 | 512  | streaming |
  | 3 | 768  | streaming |
  | 4 | 896  | streaming |

  (Alt 0 = stop; SET_INTERFACE to 1–4 to start at increasing bandwidth.)
- Descriptor is stored as a raw blob in a 512-byte 24x04 **EEPROM** on the camera
  board (OV519 CAMERAMATE format; no firmware). VID/PID are byte-swapped in the
  dump (`5E04→045E`, `8C02→028C`). The EyeToy descriptor must present as a single
  video device to match what Video Chat expects.

---

## 3. Source material & method

- **`default.xbe`** — the Video Chat title, decompiled in Ghidra.
- **FID (Function ID):** built a fingerprint DB from the retail `xapilib.lib`
  (language/compiler-spec `x86:LE:32:default:windows`, matched to the XBE) plus
  d3d8/dsound/xgraphics/xonline. Result: ~5000 framework functions named (entire
  OHCD/HCD/USBD/D3D surface), leaving the bespoke camera driver in `0x000Cxxxx`
  (~270 functions) and the camera-control glue in `0x001Bxxxx` as the unnamed
  implementation surface — i.e. exactly what we must write.
- **Library symbol mining:** `xapilib.lib`/`Xapilibp.lib`/`xapilibd.lib` (interface
  signatures), `xvoice.lib` (the `XHawkMediaObject` voice class driver — an iso-media
  analog), `usbd.lib`/`ohcd.lib`/`usbhub.lib` (the stack implementation).
- **Headers folded in:** XDK `usb.h` (the `_URB` union, `USB_BUILD_*` macros, iso
  structs), `usb100.h` (standard descriptors/constants), `usbxapi.h` (`USBD_Init`
  entry + `.XPP` segment), `mm.h` (contiguous DMA alloc).
- **Worked source:** `USB.zip` → the **SLIX driver** (`slixdriver.cpp` / `islixd.cpp`
  / `i_slixdriver.h`) — a complete USB class driver in C++, and `linkinit/hawk/
  usbinit.cpp` — a minimal `XInitDevices` registration example.

### Key decompiled functions (app region)
- `FUN_000cf340` — camera command dispatch (installed at boot; switches on cmd id
  `0x100` buf / `0x101` GET-format / `0x102` SET-format / `0x107` START / `0x108` STOP).
- `FUN_001ba280/baa80/ba8f0/bad70/ba9b0` — the 5-call device API
  (poll / open+detect / set-format / start-capture / poll).
- `FUN_001be430` — the transfer-submit core (`IUsbDevice::SubmitRequest`).
- `FUN_001baa80` — the GET_DESCRIPTOR URB build (the control-URB template).

---

## 4. The USB framework (what we link against)

Layering, app → wire:
```
App → XInitDevices / 5-call API
  → IUsbInit (construct, enumerate, Process)
  → IUsbDevice (per device: SubmitRequest, Get*Descriptor, Get/SetExtension, ...)
  → USBD_* (enumeration state machine + class-driver match/load)
  → HCD_* / OHCD_* (host controller, transfer queueing, ISO pipeline)
  → OHCI @ 0xFED00000
```

**`IUsbDevice`** and **`IUsbInit`** are concrete `__thiscall` C++ classes (no vftable
symbols → not virtual). Full method signatures are reconstructed from Microsoft's
own name-mangling and declared in `xbox_usb.h`. Camera-relevant `IUsbDevice` methods:
`SubmitRequest(URB*)`, `GetDeviceDescriptor()/GetConfigurationDescriptor()/
GetInterfaceDescriptor()/GetEndpointDescriptor(iface,alt,ep)`, `Get/SetExtension()`,
`SetClassSpecificType()`, `GetPort()`, `AddComplete()`.

The framework is **linkable** — `xapilib.lib` is the real XDK lib and is a required
RXDK install dependency, so these symbols resolve at link time.

---

## 5. Class-driver registration & attach (CONFIRMED)

A class driver is one entry in a fixed table at `0x1b2774..0x1b2790`. Descriptor:
```
[0] = class      ┐ matched against device _PNP_CLASS_ID (low two bytes), BY VALUE
[1] = subclass   ┘ (USBD_FindClassDriver)
[+4] = Register fn   ← XInitDevices calls for every entry at boot
[+8] = Attach fn     ← USBD_LoadClassDriver calls on a matching connect
```
- **Boot:** `XInitDevices → IUsbInit() → (each entry) Register(+4) → Process()
  → HCD_EnumHardware()`.
- **Connect:** `USBD_DeviceEnumStage0→Pre1→1→3→6 → determine _PNP_CLASS_ID →
  USBD_LoadClassDriver → USBD_FindClassDriver(match) → (*(+8))(IUsbDevice*)`.
- No match → `AddComplete(USBD_STATUS_UNSUPPORTED_DEVICE 0x80000400)`.

The camera class driver supplies `{class, subclass, Register, Attach}`. Match is by
**class/subclass, not VID/PID** — so the EyeToy's interface class must be known at
attach (read it, or register a catch-all and filter by VID/PID inside Attach).

---

## 6. The URB model & iso streaming

`SubmitRequest` takes a `union _URB` (folded verbatim from XDK `usb.h` into
`xbox_usb.h`). Every arm is known: `ControlTransfer`, `BulkOrInterruptTransfer`,
`OpenEndpoint`, `CloseEndpoint`, `IsochOpenEndpoint` (returns `.EndpointHandle`),
`IsochStartTransfer`, `IsochAttachBuffer`, `IsochStopTransfer`, `IsochCloseEndpoint`.
Common prefix `_URB_HEADER { Length, Function, Status, CompleteProc,
CompleteContext }`. Populate via `USB_BUILD_*` macros (never hand-stamp offsets).
Iso completions deliver `_USBD_ISOCH_TRANSFER_STATUS`; frame buffers are described by
`_USBD_ISOCH_BUFFER_DESCRIPTOR` and must be physically contiguous
(`MmAllocateContiguousMemory`).

**Iso streaming recipe** (from the SLIX driver — real, adaptable):
```c
USB_BUILD_ISOCH_OPEN_ENDPOINT(&urb.IsochOpenEndpoint, ENDPOINT_NUM_DIRECTION(ep), maxPkt, 0);
Device->SubmitRequest(&urb);  pipe = urb.IsochOpenEndpoint.EndpointHandle;
USB_BUILD_ISOCH_START_TRANSFER(&urb2.IsochStartTransfer, pipe, 0, URB_FLAG_ISOCH_START_ASAP);
Device->SubmitRequest(&urb2);
USB_BUILD_ISOCH_ATTACH_BUFFER(&urb3.IsochAttachBuffer, pipe, USBD_DELAY_INTERRUPT_0_MS, &bufd);
Device->SubmitRequest(&urb3);   // bufd.TransferComplete fires per frame
```

---

## 7. Camera object internals (from the XBE)

These describe the *original* in-XBE camera object. They are the internal model our
driver re-implements (the device-extension context, state machine, LED/format
control). Still valid RE; not all are needed for a minimal bring-up.

### Allocation
`operator new(0x844)` then zeroed (`FUN_000cdaa0`) — an 0x844-byte object.

### Vtable layout (confirmed slots)
| Off | Slot | Role |
|----|----|----|
| 0x0C | 3 | (old registry-provider QueryKey — see §9) |
| 0x10–0x2C | 4–11 | (old registry-provider open/close — see §9) |
| 0x30 | 12 | Command setter dispatch |
| 0x34 | 13 | Command getter dispatch |
| 0x38 | 14 | Stream state query |
| 0x48 | 18 | Stream reset / query |
| 0x54 | 21 | LED set (from EnableSystem) |
| 0x58 | 22 | LED state write |
| 0x60 | 24 | DisableSystem path |
| 0x7C | 31 | Format set |
| 0x110/0x114/0x118 | 68/69/70 | COM QueryInterface / AddRef / Release |

### Key field offsets (selected)
| Off | Type | Field |
|----|----|----|
| 0x7C2 | byte | USB enabled (0=uninit,1=enabled) |
| 0x7C1 / 0xD1 | byte | current / target LED state |
| 0xC4 | byte | feature flags (bit2=format match, bit3=push mode) |
| 0x835 | byte | E2PROM address |
| [0x1C8] | DWORD | stream active (0=idle,1=streaming) |
| [0x1CE] | DWORD | DefaultQualityLevel (2 if read fails) |
| [0x1CF..0x1EE] | DWORD×32 | default Y / UV quant tables |
| [0x6FC]/[0x700] | ptr | stream descriptor type 0 / 1 |
| [0x838] | ptr | serial-number buffer (from EEPROM) |

---

## 8. EyeToy compatibility & patch

- **Register-compatible.** OV519 (EyeToy) and the Xbox Cam bridge (OV530) are
  register-level compatible, so the same control sequences drive either chip.
- **TWO barriers, not one** (correcting an earlier "only VID/PID" assumption):
  1. **VID/PID** — Xbox Cam `0x045E/0x028C` vs EyeToy `0x054C/0x0155`. The decomp's
     compare is at `FUN_001baa80` (`if (sStack_40==0x45e && sStack_3e==0x28c)`).
  2. **Interface count** — Xbox Cam descriptor has **bNumInterfaces = 1** (single
     video device; EEPROM offset 0x00A4). The **EyeToy is composite with 3
     interfaces.** Video Chat's original driver expects a single video device, so a
     VID/PID patch alone is insufficient on the *original app*.
- **Original app path:** patch the `0x045E/0x028C` compare AND ensure the camera
  presents as single-video (EyeToy EEPROM reflash to count=1 + trimmed descriptors).
- **Our custom-driver path (advantage):** because we own the Attach logic, the camera
  class driver can **bind only interface 0 (video) and ignore the EyeToy's extra
  interfaces** — no EEPROM reflash needed. Match by class 0xFF/subclass 0 (+ optional
  VID/PID filter). This is a concrete reason the custom driver is more flexible than
  the patched retail app. See `EEPROM_DESCRIPTOR.md`.

---

## 9. Superseded model (IOCTL / registry) — why it was abandoned

An earlier branch hypothesized homebrew access via: open a device handle from the
registry-stored `CameraStatusPath`, then drive it with `NtDeviceIoControlFile`
IOCTLs (`0x100/0x101/0x107`), with camera config written as registry `CameraSetting`
keys. **This was wrong**, for reasons later confirmed:

1. **The Xbox kernel exports no registry-key API** (`xboxkrnl.pdb` has no
   `NtOpenKey`/`NtSetValueKey`/etc.). The XBE's `CameraSetting` access goes through an
   **in-object COM-like provider** (vtable slots 3–11, mask `0xF003F`) that is
   dashboard/system-app code, not reachable from a normal title.
2. **There is no device handle.** The XBE never opens `\Device\Camera...`; the
   `0x100/0x101/0x107` codes are the minidriver's *internal* KS dispatch routed
   in-process via `FUN_000cf340(object, ...)`, not `NtDeviceIoControlFile`. The
   `usbcamd` minidriver is effectively linked into the app.
3. **Win32 `Reg*` / `XVoiceCreateMediaObject` / `XDEVICE_TYPE_CAMERA` don't apply.**
   No camera device-type table exists; `XVoiceCreateMediaObject` rejects non-voice
   pointers. There is no kernel camera driver of any kind.

**Conclusion that redirected the project:** the camera is driven entirely in-process
over USB, so the reproducible homebrew path is a **USB class driver** at the
`IUsbDevice`/framework level (§4–6) — not a handle+IOCTL client. The RE facts from
this branch (object layout §7, OHCI confirmation, EyeToy patch §8) carry forward; the
*access model* did not.

(Kernel surface that IS available to homebrew, all confirmed `xboxkrnl.pdb` exports:
`NtCreateFile/NtOpenFile/NtReadFile/NtWriteFile/NtClose/NtDeviceIoControlFile`,
`Rtl*AnsiString`, `ExQueryNonVolatileSetting`, `ObCreateObject`, plus the `Mm*`
contiguous-memory and `Ob`/`Io` primitives in `xbox_native.h`. Object names for
file/device paths are ANSI (`PANSI_STRING`), root `ObDosDevicesDirectory()`.)

---

## 10. Confidence assessment

| Area | Confidence | Notes |
|---|---|---|
| Overall architecture | ~100% | Traced end-to-end, framework named via FID |
| USB framework interface (`IUsbDevice`/`IUsbInit`) | 100% | From MS C++ mangling |
| `_URB` union / iso structs / `USB_BUILD_*` | 100% | Folded verbatim from XDK `usb.h` |
| Class-driver attach contract | 100% | Confirmed from `XInitDevices`/`USBD_*` decomp |
| Worked driver template (SLIX) | 100% | Complete C++ source in hand |
| Camera object internals (§7) | 90% | Direct from decomp; some inferred |
| EyeToy compatibility | 100% | VID/PID patch proven; register-compatible |
| Linkage on RXDK in practice | ~90% | Same libs as XDK (required to install); confirm by linking |
| EyeToy `_PNP_CLASS_ID` / iso timing | — | Hardware-only (see §11) |

---

## 11. Remaining unknowns (hardware-only — no file can close these)

1. **EyeToy `_PNP_CLASS_ID` / interface class** — the class-driver match key. Read at
   attach; may require a catch-all registration + VID/PID filter.
2. **Linkage confirmation** — expected to work; only compile-and-link proves the
   exact extern declarations / mangling resolve against `xapilib.lib`.
3. **First control-transfer round-trip** — does `GetDeviceDescriptor` return the
   expected 18 bytes on real hardware.
4. **Iso timing / stability** — sustaining isochronous video on a retail box without
   bugchecks; entirely empirical.

---

## 12. References

- `FINDINGS_SUMMARY.md` — high-level overview.
- `BUILD_SPEC.md` — build plan, RE→XDK symbol map, §3.5 attach contract, §3.6 voice
  reference, §3.7 SLIX worked source, phased plan.
- `USB_TRANSPORT.md` — the full transfer chain, dispatch, enumeration, frame path.
- `CAMERA_INIT.md` — register-free verdict + lifecycle.
- `xbox_usb.h` — the self-contained USB framework header (interfaces, `_URB`,
  descriptors, macros).
- `xb_cam.h/.cpp`, `xbox_native.h`, `cameratest.cpp` — driver skeleton, kernel shim,
  D3D8 harness.
- Source assets: `xapilib.lib`/`Xapilibp.lib`, `xvoice.lib`, `usbd.lib`/`ohcd.lib`,
  `usb.h`/`usb100.h`/`usbxapi.h`/`mm.h`, `USB.zip` (SLIX driver + linkinit examples).
