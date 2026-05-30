# Camera init — findings & bring-up plan

**Team Resurgent / Darkone83.** What the `*.set` files give us, how they map to
a real homebrew bring-up, and what's still open.

## The `.set` files

These are `usbcamd` INF-style setting files — the per-camera register tables
the driver applies. Filenames decode as *sensor + OV519 bridge*:

| File | Sensor + bridge | Notes |
|------|-----------------|-------|
| `7648519.set` | **OV7648 + OV519** | **= the Sony EyeToy (SLEH-00030). Primary target.** |
| `7620519.set` | OV7620 + OV519 | |
| `7620e519.set`| OV7620E + OV519 | |
| `7635519.set` | OV7635 + OV519 | |
| `7640519.set` | OV7640 + OV519 | |
| `6630f519.set`| OV6630 + OV519 | |

Each file defines two register spaces and a set of format/framerate blocks:

- **`UsbSetting`** → **OV519 bridge** registers, applied over a USB **vendor
  control transfer**.
- **`CameraSetting`** → **OV7648 sensor** registers, applied via **SCCB (I2C)**
  through the bridge, sensor address **`0x42`**.

Entry format is `index, value, mask` (the file's own header documents it):
`mask == 0xFF` = write directly; `mask < 0xFF` = read-modify-write.

Detection block (four fields: *I2C subaddr, register, mask, expected value*):
```
VersionCheck       42 0A FF 76     ; sensor reg 0x0A == 0x76
MinorVersionCheck  42 0B FF 48     ; sensor reg 0x0B == 0x48   => OV7648
ResetCamera        42 12 80 FF     ; sensor reg 0x12 = 0x80    (COM7 reset)
```

## Format correction (affects the harness)

The EyeToy delivers **RGB24** or **I420** — **not YUY2**. Modes in `7648519.set`:
160×120, 176×144, **320×240**, 352×288, 640×480, each in RGB24 and I420. The
driver's default (`CurrentFormat 0x32024024`) is **320×240 RGB24**.

So the cameratest preview must change when frames are real: create the preview
texture as `D3DFMT_X8R8G8B8` (or `LIN_X8R8G8B8`) and expand RGB24→32bpp per row
— **not** the current `D3DFMT_YUY2` path. (I420 is the alternative; it needs a
YUV→RGB conversion since the NV2A's YUY2 is packed 4:2:2, not planar 4:2:0.)

## Extracted tables

`ov7648_519.h` holds the verbatim EyeToy tables for a 320×240 RGB24 @30fps
bring-up: detection constants, `BridgeInit` (16 regs), `SensorInit` (40 regs),
the 320×240 RGB24 format regs, the 30fps deltas, and `ALT_320 = 3` (the USB
isochronous alternate setting for 320×240). The other resolutions/rates live in
the `.set` files if needed.

## How the real app reaches USB (from the XBE binary + pdb)

There is **no camera device handle and no kernel IOCTL**, and — corrected from
an earlier note — the app does **not** call into a kernel USB service either.
The `ObReferenceObjectByName` in the XBE resolves to `\Device\CdRom0` (the disc
check), not the camera.

Instead, the Video Chat app **carries its own OHCI USB host-controller driver
and drives the hardware directly.** Confirmed in the binary:

- Maps the OHCI #0 register block: base `0xFED00000`, length `0x1000`, plus
  `HalGetInterruptVector(1, …)` for the controller IRQ (gated on an
  `XboxHardwareInfo` revision check).
- `HalReadWritePCISpace` (×2) to locate/configure the controller via PCI config.
- `MmAllocateContiguousMemory` (×6) for physically-contiguous DMA structures —
  a `0x1000` page (HCCA) and `(n+2)*0x800` iso transfer buffers.
- `KeConnectInterrupt` (×5) to service controller interrupts.
- `IoCreateDevice` (driver object `FUN_001b2dc8`, extension `0x170`, type `0x3a`)
  to build its own USB device stack; `IoAllocateIrp` + `IofCallDriver` to move
  IRPs **down its own stack**, bottoming out at the OHCI registers it mapped.

All those kernel calls are real `xboxkrnl` exports — but they are the building
blocks of a host-controller driver, not a shortcut to one.

## Feasibility verdict (honest)

Initialising the camera is **not** a small "detect-and-init" addition. The path
the real app uses is a full **USB + OHCI host-controller driver**: PCI setup,
MMIO register programming at `0xFED00000`, HCCA/ED/TD descriptor management,
interrupt servicing, isochronous scheduling, and USB enumeration — *then* the
OV519/OV7648 register init (which we now have) and ISO frame assembly.

The hard part is ownership: the Xbox **kernel's built-in OHCD already owns these
controllers** (it services gamepads and MUs). A homebrew driver that maps
`0xFED00000` and connects the USB interrupt is contending with the kernel for
the controller — a prime source of bugchecks and lost controller input. The
Video Chat app could do this because, as a system/dashboard app, it runs in a
context where it takes over USB.

So there are three realistic routes, smallest-effort first:

- **(B) Attach to the kernel's USB stack.** *If* the retail kernel enumerates
  the camera and exposes a USB device object a title can submit URBs to, this is
  a few-hundred-line driver using the exported IRP primitives — no hardware
  ownership, no conflict. **Unverified**; this is the pivotal unknown to settle
  next (see open items).
- **(C) Port an existing OG-Xbox homebrew USB stack** that already coexists with
  the kernel, if one exists, and add an OV519 class driver on top.
- **(A) Write a from-scratch direct-OHCI driver** like the app does. Highest
  effort and risk; only if (B) and (C) are dead ends.

## Bring-up plan

```
Phase 0  Decide the USB route (gates everything)
  - Investigate route (B): does the retail kernel expose a USB device object
    for the camera that a title can submit URBs to? If yes -> small driver.
    If no -> route (C) port a stack, or route (A) write a direct-OHCI HCD.

Phase A  USB transport (size depends on Phase 0)
  - Stand up whichever transport Phase 0 selected, and prove a control
    transfer round-trips (e.g. GET_DESCRIPTOR for device VID/PID).

Phase B  Detect the EyeToy
  - Match USB VID/PID 054C:0155 (EyeToy) or 045E:028C (Xbox Cam).
  - Read sensor regs 0x0A/0x0B over SCCB; expect 0x76/0x48 (OV7648).

Phase C  Init  (data = ov7648_519.h, this is the solved part)
  - Reset: sensor 0x12 = 0x80; settle.
  - Apply OV7648_519_BridgeInit, then OV7648_519_SensorInit.
  - Apply 320x240 RGB24: Bridge320RGB24 + Sensor320RGB24.
  - Apply 30fps deltas; SET_INTERFACE alt = ALT_320 (3).

Phase D  Stream
  - Submit ISO IN URBs on EP1; assemble RGB24 frames.
  - Hand each finished frame to XCam_PublishFrame() (already wired).

Phase E  Display
  - Switch the preview texture to X8R8G8B8; expand RGB24->32bpp.
```

The register-level work (Phases B–E data) is now fully in hand from the `.set`
files. The open risk is concentrated in Phase 0/A — the USB transport.

## Still open (the honest list)

1. **The USB transport route (Phase 0)** — the one real blocker now. Settle
   whether the retail kernel exposes a camera USB device object to titles
   (route B). This determines whether the remaining work is a small URB-client
   driver or a full direct-OHCI HCD. Everything below assumes the transport
   exists.
2. **OV519 control-transfer encoding** — how a bridge `index,value` becomes a
   vendor control request, and how a sensor `index,value` becomes an SCCB write
   through the bridge. Cross-check the function that consumes
   `UsbSetting`/`CameraSetting` in the XBE against the OV519 register map
   (Linux `gspca/ov519` + OV519 datasheet).
3. **URB / IRP layout** — exact fields for whatever transport is used. A wrong
   layout is the most likely bugcheck; confirm against the XBE submit path
   (`FUN_001b556d`) before sending anything live.

## References

- OmniVision **OV7648** sensor datasheet (SCCB register map, COM7 reset).
- **OV519** bridge datasheet / Linux `gspca_ov519` driver (bridge register
  access + SCCB tunneling).
- USB 1.1 / OHCI spec (isochronous IN, EP1, alternate settings).
- `RESEARCH.md` → *Cross-Reference Corrections* for the kernel/architecture facts.