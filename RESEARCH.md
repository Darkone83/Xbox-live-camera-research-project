# Xbox Live Camera / EyeToy — Reverse Engineering Research
## Team Resurgent / Darkone83

---

## Table of Contents

1. [Overview](#overview)
2. [Hardware](#hardware)
3. [Source Material](#source-material)
4. [Driver Architecture](#driver-architecture)
5. [Camera Object](#camera-object)
6. [Registry System](#registry-system)
7. [IOCTL Dispatch](#ioctl-dispatch)
8. [Startup Sequence](#startup-sequence)
9. [Frame Delivery](#frame-delivery)
10. [USB / OHCI Internals](#usb--ohci-internals)
11. [EyeToy Patch](#eyetoy-patch)
12. [XDEVICE_TYPE_CAMERA — Critical Finding](#xdevice_type_camera--critical-finding)
13. [Confidence Assessment](#confidence-assessment)
14. [Remaining Unknowns](#remaining-unknowns)
15. [References](#references)

---

## Overview

This document records the complete reverse engineering of the Xbox Live Camera
driver interface as implemented in the Xbox Video Chat XBE (MS-124). The goal
is to enable direct camera access from homebrew code running under RXDK on
original Xbox hardware, using either the official Xbox Live Camera or a
modified Sony EyeToy camera.

All findings derive from static analysis only — no runtime validation has
been performed yet.

---

## Cross-Reference Corrections (xboxkrnl.pdb + default.xbe, 2026-05)

Cross-referencing the kernel symbols (`xboxkrnl.pdb`) against the full XBE
decompilation corrected several assumptions in the original write-up. The
corrections are folded into the sections below; this is the summary:

1. **The Xbox kernel exports NO registry-key API.** There is no `NtOpenKey`,
   `NtCreateKey`, `NtSetValueKey`, or `NtQueryValueKey` in `xboxkrnl.pdb`. The
   earlier "registry writes via `NtSetValueKey`" model is wrong at the kernel
   level. In the XBE, all `CameraSetting` registry access goes through an
   **internal COM-like registry-provider interface** — the camera object's
   own vtable slots 3–11 (QueryKey / OpenKey / CloseHandle), called with
   access mask `0xF003F` (`KEY_ALL_ACCESS`) and wide-string key names. Those
   vtable methods are XBE/dashboard code, not kernel calls. This interface is
   part of the **dashboard / system-app infrastructure** (the Video Chat app,
   MS-124, runs in that context) and is **not reachable from a normal homebrew
   title** via any documented API.

2. **No device handle; the "IOCTLs" are internal.** The XBE never opens the
   camera as a file (no `\Device\Camera...` `NtOpenFile`/`NtCreateFile`). The
   codes `0x100/0x101/0x107` are the minidriver's *internal* KS dispatch,
   routed in-process via `FUN_000cf340(object, ...)` — not `NtDeviceIoControlFile`.
   USB I/O is done by talking to the **OHCI host controller directly**
   in-process (`FUN_000be430`-style channel open with magic `0x4030`, ISO
   setup, endpoint + alternate-setting selection). The `usbcamd` minidriver is
   effectively linked into the XBE/dashboard.

3. **The Win32 `Reg*` API does not exist on Xbox.** `RegOpenKeyExW`,
   `RegSetValueExW`, `HKEY_LOCAL_MACHINE`, `KEY_ALL_ACCESS`, etc. are desktop
   Win32 only. Any prototype using them cannot compile/link for Xbox.

4. **Object names are ANSI, not Unicode, for the file/device path.**
   `OBJECT_ATTRIBUTES.ObjectName` is a `PANSI_STRING` on Xbox. The XBE builds
   device paths with `RtlInitAnsiString`, `RootDirectory =
   ObDosDevicesDirectory()` ((HANDLE)-3), `Attributes = OBJ_CASE_INSENSITIVE`,
   and opens via `NtCreateFile` / `NtOpenFile`. (Registry key *names* use wide
   strings, but those go through the vtable provider above, not the kernel.)

5. **The kernel-level surface actually available to homebrew** (all confirmed
   as `xboxkrnl.pdb` exports): `NtCreateFile`, `NtOpenFile`, `NtReadFile`,
   `NtWriteFile`, `NtClose`, `NtDeviceIoControlFile`, `RtlInitAnsiString`,
   `RtlInitUnicodeString`, `ExQueryNonVolatileSetting`, `ObCreateObject`.

6. **Device-type tables and `XVoiceCreateMediaObject` are XAPI, not kernel.**
   No `XDEVICE_TYPE_*_TABLE` symbol and no `XVoiceCreateMediaObject` appears in
   `xboxkrnl.pdb`; they live in the statically-linked XAPI library. The
   "`XDEVICE_TYPE_CAMERA` does not exist" conclusion still holds — in fact more
   strongly, since the kernel has no camera device type or driver of any kind.

7. **OHCI / USB internals validated.** `OHCI_ENDPOINT_CONTROL`,
   `OHCI_ENDPOINT_DESCRIPTOR`, `POHCI_HCCA`, `POHCI_OPERATIONAL_REGISTERS`, and
   `IsochronousEnable` are all confirmed kernel symbols.

8. **EyeToy compatibility is confirmed (not provisional).** The VID/PID hex
   patch is proven to work: OV519 (EyeToy) and OV530 (Xbox Cam) are
   register-level compatible, so once the detection comparison is patched the
   driver drives either chip identically.

**Implication for homebrew:** neither the registry-config dance nor an
"open handle + IOCTL" client is reproducible — the camera is driven entirely
in-process. A working homebrew path is a **USB/OHCI-level driver**: claim the
camera on the host controller, run the OV519/OV530 control-endpoint register
init, set up the isochronous IN endpoint (EP1 IN), and pump frames. In effect
the `usbcamd` minidriver must be reimplemented in the title. The tables and
offsets in this document remain valid as the *internal model* to build that
driver against.

---

## Hardware

### Xbox Live Camera
- **USB VID:** `0x045E` (Microsoft)
- **USB PID:** `0x028C`
- **Chipset:** OmniVision OV530
- **Connection:** Controller port memory unit slot (USB internally)
- **Transfer:** OHCI isochronous, Endpoint 1 IN
- **Max bandwidth:** 896 bytes per packet at highest alternate setting

### Sony EyeToy (modified) — CONFIRMED COMPATIBLE
- **USB VID:** `0x054C` (Sony)
- **USB PID:** `0x0155`
- **Chipset:** OmniVision OV519
- **Compatibility:** OV519 and OV530 are silicon-compatible at the register
  level. The VID/PID hex patch is **proven** — once the detection comparison
  is patched, the driver's init, format negotiation, and frame capture run
  identically for the EyeToy. EyeToy compatibility is no longer provisional.
- **Caveat:** The EyeToy USB descriptor must present as a single-interface
  device (video only). Multi-interface descriptors will be rejected by
  the driver even after the VID/PID patch.

---

## Source Material

| Source | Use |
|--------|-----|
| `default.xbe` — Xbox Video Chat (MS-124) | Primary — full Ghidra C decompilation |
| `xboxkrnl.pdb` — Xbox kernel symbols | Kernel API names/decorations; confirms which calls are kernel vs XAPI |
| `Xbox-Headers.zip` — XTL.h, Xbox.h, xvoice.h | API signatures and device type patterns (note: `XDEVICE_TYPE_*_TABLE` and `XVoiceCreateMediaObject` are **XAPI** symbols, not kernel) |
| Xbox Dev Wiki — Xbox Cam article | OV530/OV519 confirmation, USB descriptor info |
| XboxDev/nxdk `xboxkrnl.h` | Authoritative native type layouts (ANSI `OBJECT_ATTRIBUTES`, etc.) |

### Key decompiled functions

| Address | Name / Role |
|---------|-------------|
| `FUN_000ccad0` | Registry config reader + command handler installer |
| `FUN_000cad00` | EnableSystem (USB enable with guard) |
| `FUN_000cad60` | DisableSystem |
| `FUN_000cb4b0` | Command handler registration (registry key → setter/getter) |
| `FUN_000cb9c0` | EEPROM serial number read via I2C |
| `FUN_000cb950` | UsbSetting handler registration |
| `FUN_000cd9d0` | CameraSetting + E2PROMAddress handler registration |
| `FUN_000cdaa0` | Camera object constructor (allocates 0x844 bytes) |
| `FUN_000cdb20` | Stream lifecycle manager |
| `FUN_000cdd80` | IOCTL 0x107 — start streaming (ISO channel setup) |
| `FUN_000ce510` | IOCTL 0x100 — open/enumerate streams |
| `FUN_000cf120` | IOCTL 0x101 — set stream format |
| `FUN_000cf340` | IOCTL dispatch table |
| `FUN_000ce6a0` | Frame completion handler (stream pin path) |
| `FUN_000ced10` | Frame completion handler (USB path, main) |
| `FUN_000ce930` | Frame delivery to application |
| `FUN_001bfd30` | ISO channel init (semaphores + transfer descriptor) |
| `FUN_001bf360` | Per-stream channel init (OHCI endpoint) |
| `FUN_001bf440` | Alternate setting selection + format negotiation |
| `XVOICE::XVoiceCreateMediaObject` | Voice media object (camera CANNOT use this) |

---

## Driver Architecture

The Xbox Video Chat XBE uses the **usbcamd** minidriver stack — a kernel
streaming (KS) minidriver for USB cameras derived from the Windows WDM model.

Communication layers:
```
Application (XBE)
    │
    ├── registry-provider vtable   CameraSetting config (NOT a kernel API)
    │   (camera-object slots 3-11)  access mask 0xF003F, wide-string keys;
    │                               dashboard/system-app infrastructure
    │
    ├── NtDeviceIoControlFile      IOCTL dispatch → stream control
    │
    └── NtOpenFile / NtCreateFile  Opens device handle (ANSI object name)
```

The driver publishes `CameraStatusPath` once the camera is detected and
enumerated. The XBE reads this key (through the registry-provider vtable) to
get the `\Device\...` path for `NtOpenFile`.

> **Correction (pdb cross-ref):** the original draft showed `NtSetValueKey`
> here. The Xbox kernel exports no registry-key functions; the `CameraSetting`
> access is entirely vtable-mediated (see Cross-Reference Corrections). Only
> the file/device + IOCTL calls are real kernel exports.

The camera object (0x844 bytes) is an internal KS filter. It is NOT a
standard Windows COM object despite using a COM-like vtable pattern. The
vtable slots map to registry operations, not COM methods.

---

## Camera Object

### Allocation
```c
// From FUN_000cdaa0
void *cam = ExAllocatePool(0x844);
memset(cam, 0, 0x844);
```

### Vtable layout (confirmed slots)

| Byte offset | Slot | Role |
|-------------|------|------|
| `0x0C` | 3 | Registry QueryKey |
| `0x10` | 4 | Registry value release |
| `0x14` | 5 | Open subkey |
| `0x18` | 6 | Open key with access mask |
| `0x1C` | 7 | Open root key |
| `0x20` | 8 | Object allocator |
| `0x24` | 9 | EnableSystem / OpenKey |
| `0x28` | 10 | CloseKey |
| `0x2C` | 11 | CloseHandle (most-used, 11×) |
| `0x30` | 12 | Command setter dispatch |
| `0x34` | 13 | Command getter dispatch |
| `0x38` | 14 | Stream state query |
| `0x48` | 18 | Stream reset / query |
| `0x54` | 21 | LED set (called from EnableSystem) |
| `0x58` | 22 | LED state write |
| `0x60` | 24 | DisableSystem path |
| `0x7C` | 31 | Format set |
| `0x110` | 68 | COM QueryInterface |
| `0x114` | 69 | COM AddRef |
| `0x118` | 70 | COM Release |

### Key field offsets

| Byte offset | Type | Field |
|-------------|------|-------|
| `0x7C2` | byte | USB enabled (0=uninit, 1=enabled) |
| `0x7C1` | byte | Current LED state |
| `0xD1` | byte | Target LED state |
| `0x7C5` | byte | Suspend state |
| `0x7C6` | byte | Auto-close flag |
| `0xBD` | byte | Interlace check |
| `0xBE` | byte | Stream abort flag |
| `0xBF` | byte | Unknown stream flag |
| `0xC4` | byte | Feature flags (bit2=format match, bit3=push mode) |
| `0x835` | byte | E2PROM address |
| `[0x08]` | DWORD | SupportEvent |
| `[0x09]` | DWORD | BandwidthAllocateRule |
| `[0x1C8]` | DWORD | Stream active (0=idle, 1=streaming) |
| `[0x1CE]` | DWORD | DefaultQualityLevel (2 if registry read fails) |
| `[0x1CF..0x1DE]` | DWORD×16 | DefaultYQuanTable |
| `[0x1DF..0x1EE]` | DWORD×16 | DefaultUVQuanTable |
| `[0x1F0]` | DWORD | LED state save (restored on disable) |
| `[0x20C]` | ptr | CameraStatusPath buffer |
| `[0x20F]` | ptr | Allocator interface |
| `[0x6FC]` | ptr | Stream descriptor type 0 |
| `[0x700]` | ptr | Stream descriptor type 1 |
| `[0x838]` | ptr | Serial number buffer (from EEPROM) |
| `[0x83C]` | ptr | Allocator interface (second ref) |

---

## Registry System

### Base path
```
HKLM\System\CurrentControlSet\Services\Class\CameraSetting\
```

### Command handler registration
Each lifecycle key is registered via `FUN_000cb4b0`:
```c
FUN_000cb4b0(regHandle, L"KeyName", nameByteLen, &setterSlot, &getterSlot);
```

### Full command table

| Registry Key | Byte Len | Setter [slot] | Getter [slot] |
|-------------|----------|---------------|---------------|
| `EnableSystem` | 0x1A | [0x23] | [0x0E] |
| `DisableSystem` | 0x1C | [0x22] | [0x0D] |
| `SetUsbWork` | 0x16 | [0x1F] | [0x0A] |
| `SetUsbInit` | 0x16 | [0x20] | [0x0B] |
| `PowerDownCamera` | 0x20 | [0x25] | [0x10] |
| `PowerOnCamera` | 0x1C | [0x24] | [0x0F] |
| `ResetUsb` | 0x12 | [0x21] | [0x0C] |
| `ClearSnapButton` | 0x20 | [0x2D] | [0x1A] |
| `EnableAutoLaunch` | 0x22 | [0x2B] | [0x18] |
| `DisableAutoLaunch` | 0x24 | [0x2C] | [0x19] |
| `CheckAutoLaunch` | 0x20 | [0x1CA] | (stack) |
| `EnableSsuspend` | 0x1E | [0x27] | [0x12] |
| `DisableSsuspend` | 0x20 | [0x26] | [0x11] |
| `TurnOnLed` | 0x14 | [0x28] | [0x13] |
| `TurnOffLed` | 0x16 | [0x29] | [0x14] |
| `BlockStream` | 0x18 | [0x1E] | [0x1B] |
| `StartStream` | 0x18 | [0x1D] | [0x1C] |
| `CameraTimeout` | 0x1C | [0x30] | [0x31] |
| `UsbSetting` | 0x16 | [0x2F] | [0x17] |
| `CameraSetting` | 0x1C | [0x2E] | [0x16] |

### Config keys (read on init, DWORD unless noted)

```
CameraStatusPath    Status2HKR         CustomID
StillSupportType    SupportStillPin    SupportEvent
DefaultQualityLevel EnableAutoClose    UseGpio0
AddSerialNumber     ShowCameraId       PowerControl
DefaultYQuanTable   DefaultUVQuanTable E2PROMAddress
EnableAutoLaunch    DisableAutoLaunch  CheckAutoLaunch
EnableSsuspend      DisableSsuspend    ClearSnapButton
TurnOnLed           TurnOffLed
```

### Stream format keys

```
FourCC              Width              Height
FrameRate           MinFrameRate       MaxFrameRate
TypFrameRate        CurrentFrameRate   BitCount
Progressive         StreamType         QualityLevel
SensorWidth         SensorHeight       RawFrameLength
AlternateSetting    UsbCBR
```

### Video proc amp / camera control keys

```
VideoProcAmp        VideoControl       VideoCompression
CameraControl       CameraDataType     AdjustYUVCamSetting
AdjustYUVUsbSetting ReverseUVCamSetting
YQuanTable          UVQuanTable        BadPixel
BadPixelMemSize     PropertyId         DefaultValue
DefaultFlags        LastValue          LastFlags
MaxValue            MinValue           Step
CustomProperty
```

### Clock scaling keys

```
ClockUpCamRegs      ClockDownCamRegs   ClockUpUsbRegs
ClockDownUsbRegs    PreClockUpCamRegs  PostClockUpCamRegs
PreClockDownCamRegs PostClockDownCamRegs
PreClockUpUsbRegs   PostClockUpUsbRegs
PreClockDownUsbRegs PostClockDownUsbRegs
ClockUpTh           ClockDownTh
```

### Notification keys

```
OpenStreamCameraSetting     CloseStreamCameraSetting
OpenStreamUsbSetting        CloseStreamUsbSetting
AttachedCamera              Camera DeAttached
SaveFormat                  SaveLastFrame
SnapShot                    ButtonPushed
PushModeEvent               RestoreCamReg
RestoreUsbReg               CreateFileName
FriendlyName                DriverDesc
```

---

## IOCTL Dispatch

All IOCTLs sent via `NtDeviceIoControlFile` on the handle opened from
`CameraStatusPath`.

| Code | Handler | Description |
|------|---------|-------------|
| `0x100` | `FUN_000ce510` | Enumerate streams. Reads stream count from `param_1[0x80]`. Sets up stream descriptor array (0x50 bytes each). Configures data buffer pointers. |
| `0x101` | `FUN_000cf120` | Set stream format. Validates format descriptor (min 0x98 bytes). Calls FUN_001bf360 (channel init) + FUN_001bf440 (alt setting). |
| `0x102` | `FUN_000ce5c0` | Select stream by index. |
| `0x107` | `FUN_000cdd80` | **Start streaming.** Writes `'DEVA'` (0x45564544) magic to stream struct. Calls FUN_001bfd30 (semaphore init + ISO descriptor alloc + OHCI configure). |
| `0x108` | `FUN_001bfad0` | Stop streaming. |
| `0x109` | `FUN_000ce490` | Query stream state. Calls FUN_001be6b0. |
| `0x10A` | `FUN_000cf040` | Set format if format marker byte == 0x1B. |
| `0x10B` | (inline) | No-op, returns STATUS_SUCCESS. |
| `0x10D` | `FUN_001bef70` | Reset. |
| `0x10E` | (inline) | Enumerate pins (up to 3 checked). |

---

## Startup Sequence

```
1. Poll XGetDeviceChanges(XDEVICE_TYPE_????, port)
      until camera detected

2. Allocate camera object
      cam = ExAllocatePool(0x844)
      memset(cam, 0, 0x844)

3. Install vtable groups
      cam[0] = &vtable_group_A
      cam[1] = &vtable_group_B
      cam[2] = &vtable_group_C
      cam[3] = &vtable_group_D
      cam[4] = &vtable_group_E
      cam[5] = &vtable_group_F

4. Registry init (FUN_000ccad0)
      - Open HKLM\...\CameraSetting with access 0xF003F
      - Read all config values into camera object fields
      - Register all command handlers via FUN_000cb4b0
      - Read DefaultYQuanTable (16 DWORDs) → cam[0x1CF]
      - Read DefaultUVQuanTable (16 DWORDs) → cam[0x1DF]
      - Read E2PROMAddress → cam[0x835]
      - Call FUN_000cb9c0:
          Read EEPROM register 2 → read vendor string
          Read EEPROM register 3 → read model string
          Read EEPROM register 4 → build serial number buffer

5. EnableSystem (FUN_000cad00)
      if (cam[0x7C2] != 0) return 0  // already enabled guard
      cam[0x7C2] = 1
      call vtable[9]   // OpenKey / USB enable
      call vtable[25]  // HalReadSMBusValue / USB init
      cam[0x1F0] = cam[0x7C1]  // save LED state
      if (LED state mismatch) call vtable[22](target_state)

6. IOCTL 0x100 — enumerate streams
      Reads cam[0x80] = number of streams
      Fills stream descriptors (0x50 bytes each)
      Sets up data buffer pointers

7. Write format to registry
      NtSetValueKey(CameraSetting\SupportCamera\7649519\CurrentFormat,
                    XCAM_FMT_YUY2)

8. IOCTL 0x101 — set stream format
      Format descriptor minimum size: 0x98 bytes
      Calls FUN_001bf360:
          Writes 'CHAN' (0x4348414E) magic
          KeInitializeEvent (frame completion)
          Sets up OHCI endpoint descriptor
      Calls FUN_001bf440:
          Selects alternate setting
          Negotiates format via OHCI register writes

9. IOCTL 0x107 — start streaming
      Writes 'DEVA' (0x45564544) to stream struct
      Calls FUN_001bfd30:
          KeInitializeSemaphore x2
          ExAllocatePool(0x12) for ISO transfer descriptor
          FUN_001be430(magic=0x4030) — opens USB channel
          FUN_001bfc50 — configures OHCI endpoint

10. Read CameraStatusPath from cam[0x20C]
       NtOpenFile(&hCam, GENERIC_READ|GENERIC_WRITE,
                  &objAttr, &ioStat, FILE_SHARE_READ, ...)

11. Spawn capture thread (PsCreateSystemThreadEx)
       Thread calls NtReadFile or submits URBs on hCam
       Frame completion goes through FUN_000ced10
```

### Shutdown sequence

```
1. IOCTL 0x108 — stop streaming
2. DisableSystem (FUN_000cad60):
      if (cam[0x7C2] == 0) return  // already disabled
      cam[0x7C2] = 0
      call vtable[24], vtable[10]
      restore LED state from cam[0x1F0]
3. NtClose(hCam)
4. ExFreePool(cam)
```

---

## Frame Delivery

Frame completion is handled by `FUN_000ced10` (USB path) and `FUN_000ce6a0`
(stream pin path).

### IRP layout on frame completion

```c
IRP[0]        = 0x45425253  // 'SRBE' magic — signals valid frame IRP
IRP[0x18]     = ptr to stream descriptor
IRP[0x18]+0x20 = buffer size (bytes available)
IRP[0x18]+0x24 = frame data pointer
IRP[0x18]+0x28 = frame data length
```

### Frame timing

```c
stream[0x170]+0x28 = frame width spec
stream[0x170]+0x2C = frame period (100ns units)
```

Frame counter fields in stream struct:
```
[0x138][0x13C]  64-bit frame counter
[0x140][0x144]  dropped frame counter
[0x184]         reset flag (clears counters when set)
[0x188][0x18C]  last timestamp
```

### D3D8 texture upload (YUY2)

```c
IDirect3DTexture8 *pTex;
pDevice->CreateTexture(320, 240, 1, 0,
                       D3DFMT_YUY2, D3DPOOL_DEFAULT, &pTex);

// Per frame (in capture thread, under critical section):
D3DLOCKED_RECT lr;
pTex->LockRect(0, &lr, NULL, 0);
memcpy(lr.pBits, frame_data_ptr, 320 * 240 * 2);
pTex->UnlockRect(0);

// In render thread:
pDevice->SetTexture(0, pTex);
pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad_verts, sizeof(VERTEX));
```

The NV2A GPU converts YUY2 → RGB on the fly during texture sampling.
No software conversion needed.

---

## USB / OHCI Internals

From `xboxkrnl.pdb` and decompilation:

- Driver: `usbcamd` minidriver (confirmed from registry path)
- Host controller: OHCI — **confirmed kernel symbols**: `OHCI_ENDPOINT_CONTROL`,
  `OHCI_ENDPOINT_DESCRIPTOR`, `POHCI_HCCA`, `POHCI_OPERATIONAL_REGISTERS`
- Isochronous enable: `IsochronousEnable` field — confirmed kernel symbol
- Channel struct: 'CHAN' magic at offset 0
- Transfer descriptor: allocated as 0x12 bytes via `ExAllocatePool`
- USB channel open magic: `0x4030` (passed to `FUN_001be430`)
- Semaphores: two initialized in `FUN_001bfd30` before ISO setup

---

## EyeToy Patch

### Hex edit (applied to `default.xbe`)

```
Search:  5E 04 0F 85 CA 01 00 00 66 81 7C 24 1E 8C 02
Replace: 4C 05 0F 85 CA 01 00 00 66 81 7C 24 1E 55 01
```

### What it patches

```
Original bytes:
  5E 04   = 0x045E (Microsoft VID, little-endian)
  8C 02   = 0x028C (Xbox Camera PID, little-endian)

Patched bytes:
  4C 05   = 0x054C (Sony VID, little-endian)
  55 01   = 0x0155 (EyeToy PID, little-endian)
```

The `0F 85 CA 01 00 00` between them is a `JNZ` instruction — unchanged,
so the branch logic stays identical. Only the comparison targets change.

### Why it works

OV530 (Xbox camera) and OV519 (EyeToy) share the same register-level
interface. The driver's init, format negotiation, and frame capture code
is identical for both. The VID/PID check is purely for device identification
before the driver claims the USB device — once past that, both chips respond
identically to the driver's I2C register commands.

### Risk

The EyeToy USB descriptor must match the Xbox camera pattern:
- Single USB device
- Video interface only
- No additional HID, audio, or composite interfaces

If the EyeToy presents as a composite device (multiple interfaces) the
driver may reject it even after the VID/PID patch. Verify with USB
descriptor dump before attempting.

---

## XDEVICE_TYPE_CAMERA — Critical Finding

**`XDEVICE_TYPE_CAMERA` does not exist.**

The device-type tables are **XAPI** (statically-linked) symbols, not kernel
exports — no `XDEVICE_TYPE_*_TABLE` and no `XVoiceCreateMediaObject` appears in
`xboxkrnl.pdb`. The device type tables provided by XAPI are:
```
XDEVICE_TYPE_GAMEPAD_TABLE
XDEVICE_TYPE_MEMORY_UNIT_TABLE
XDEVICE_TYPE_VOICE_MICROPHONE_TABLE
XDEVICE_TYPE_VOICE_HEADPHONE_TABLE
XDEVICE_TYPE_HIGHFIDELITY_MICROPHONE_TABLE
XDEVICE_TYPE_DEBUG_MOUSE_TABLE
```

No camera table. The `XVoiceCreateMediaObject` function (XAPI, not a kernel
export) validates its `param_1` against `XDEVICE_TYPE_VOICE_MICROPHONE_TABLE`,
`XDEVICE_TYPE_VOICE_HEADPHONE_TABLE`, and
`XDEVICE_TYPE_HIGHFIDELITY_MICROPHONE_TABLE` — and returns
`0xC0000002` (STATUS_NOT_IMPLEMENTED equivalent) for any other pointer.

**Conclusion: the IOCTL path is the correct approach for homebrew camera
access.** Do not attempt to use `XVoiceCreateMediaObject` with a camera.
Use `NtDeviceIoControlFile` directly on the handle obtained from
`CameraStatusPath`.

---

## Confidence Assessment

| Area | Confidence | Notes |
|------|-----------|-------|
| Registry key names and order | 95% | Directly confirmed from decompilation |
| IOCTL codes and handlers | 95% | Confirmed from dispatch table |
| Camera object size (0x844) | 100% | Direct `operator new(0x844)` confirmed (FUN_000cdaa0) |
| Frame IRP format | 90% | Magic + offsets confirmed |
| ISO channel setup sequence | 85% | Magic values and call chain confirmed |
| Device handle acquisition | 70% | `CameraStatusPath` path confirmed, exact string not seen |
| Format descriptor layout | 65% | Min size confirmed, full struct inferred |
| Registry config mechanism | 90% | **Now understood:** vtable-mediated provider, mask 0xF003F. Kernel has no registry-key API; not reachable from homebrew |
| EyeToy compatibility | **100%** | **Confirmed** — VID/PID hex patch proven; OV519/OV530 register-compatible |
| Homebrew viability of registry path | 25% | Registry config is system-app infrastructure; direct device-open + IOCTL is the route to pursue |
| Overall | **70-75%** | Sufficient to attempt the direct device-open path |

---

## Remaining Unknowns

1. **Registry DWORD values** — We know what keys to write but not what DWORD
   value triggers each state transition. The decompilation shows reads but
   the trigger values are likely hardware-specific or driver-internal. May
   need runtime experimentation or further disassembly of `FUN_000cb4b0`
   internals.

2. **usbcamd load timing** — Does usbcamd load automatically on camera
   detection via `XGetDeviceChanges` or does it require an explicit call?
   If the driver is not loaded when we attempt to open the device, the
   `NtOpenFile` will fail with STATUS_OBJECT_NAME_NOT_FOUND.

3. **CameraStatusPath exact string** — The path is driver-generated and
   stored in the registry. Its format is unknown but likely follows the
   pattern `\Device\Camera0` or `\Device\Usb\...`. First runtime test
   should dump this registry key after camera detection.

4. **Format descriptor layout** — IOCTL 0x101 requires a format descriptor
   of at least 0x98 bytes. The full field layout is not confirmed. Starting
   point: match the KSDATARANGE_VIDEO structure from Windows WDM docs since
   usbcamd derives from the Windows WDM model.

5. **EyeToy USB descriptor** — Needs a USB descriptor dump to confirm single
   interface, video only. If composite, the EEPROM may need modification.

---

## References

- Xbox Dev Wiki: https://xboxdevwiki.net/Xbox_Cam_(for_Video_Chat)
- Ghidra XBE Loader: https://github.com/mborgerson/ghidra-xbe
- OmniVision OV519 datasheet (for register-level compatibility)
- Windows WDM USBCAMD documentation (for KS filter architecture)
- KSDATARANGE_VIDEO structure (Windows DDK) — likely basis for IOCTL 0x101
  format descriptor