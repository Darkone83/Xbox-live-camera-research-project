# Xbox Camera Test (Camera-test) — Team Resurgent / Darkone83

A homebrew RXDK test app + USB driver for the original Xbox camera: the **Sony
EyeToy** (`054C:0155`, OmniVision OV519 + OV7648) and the **Xbox Live Vision /
Xbox Cam** (`045E:028C`, OV530, OV519-compatible). It enumerates the camera
manually, brings up the OV519 bridge + OV7648 sensor, streams **MJPEG** over an
isochronous endpoint, decodes it in software, and shows a live image on screen,
with crash-survivable logging to `D:\xb_cam.txt`.

> **STATUS: WORKING ON HARDWARE.** A recognizable 320×240 image streams from an
> EyeToy on a retail box. Architecture below reflects the **shipped** design.
> Full detail: `WORKING_IMPLEMENTATION.md`, `OV519_OV7648_INIT.md`,
> `MJPEG_FRAME_FORMAT.md`.

> **History note.** Earlier revisions of this repo described a USB *class-driver*
> design (vendor-class 0xFF match, `.XPP$Class` registration, "the bridge
> self-configures, no register replay"). That model was disproven on hardware and
> has been retired to `archive/`. See `RESEARCH.md` for how the conclusions changed.

## Architecture (working model)

The camera is **not** enumerated by the system, so the driver does it manually:
walk `g_DeviceTree`, find the TI hub, scan its ports, reset the
connected-but-not-enabled port, allocate our own device node, `SET_ADDRESS` /
`SET_CONFIGURATION`, parse the config descriptor for the iso IN endpoint
(`0x81`, alt 3 = 320×240, maxpkt 768). It then replays the **OV519 + OV7648**
register init (gspca `ov519` path; `reg 0x72=0xEE` is mandatory), opens the iso
pipe, and reassembles **MJPEG** frames from OV519-delimited iso packets (SOF
`0x50` / EOF `0x51`, per-packet `BytesRead`). Each frame is decoded with picojpeg
to BGRA and drawn from a swizzled `D3DFMT_A8R8G8B8` texture via `XGSwizzleRect`.

### EyeToy on real hardware
The test unit enumerated as single-video at `054C:0155` and streamed without an
EEPROM reflash. `Cam_IsCameraId()` accepts `054C:0155` and `045E:028C`; if a stock
unit reports `0154`, add it there.

## Files (shipped)

| File | Role |
|---|---|
| `main.cpp` | Test harness — D3D8 setup, on-screen UI, the camera preview quad (A8R8G8B8 texture, no YUVENABLE), on-screen debug overlay. |
| `xb_cam.cpp` | The camera driver — manual enumeration, OV519/OV7648 bring-up, iso streaming, MJPEG assembly, picojpeg decode, swizzled display, and the public `XCam_*` API. |
| `xb_cam.h` | Public API (`XCam_Init` / `XCam_Shutdown` / `XCam_IsStreaming` / `XCam_DrawToSurface` / `XCam_SetLog` + `XCAM_*` constants). |
| `xbox_usb.h` | **Authoritative** Xbox USB header — `_URB` union, `USB_BUILD_*` macros, descriptors, `USBD_STATUS` table, `IUsbDevice`/`IUsbInit`, and (critically) the iso structs `USBD_ISOCH_BUFFER_DESCRIPTOR.Pattern[8]`, `USBD_ISOCH_PACKET_STATUS_WORD.BytesRead:12`, `USBD_ISOCH_TRANSFER_STATUS.PacketStatus[8]`. Only includes `<xtl.h>`. |
| `xbox_kernel.h` | Kernel-primitive shim (`Ke*`/`Mm*`/`DbgPrint`/`KEVENT`). |
| `picojpeg.cpp` / `.h` | Integer-only baseline-JPEG decoder (RXDK-safe). **Must be added to the `.vcxproj`** (compiles as C++). |
| `font.cpp` / `font.h` | Glyph renderer — swizzled `D3DFMT_A8R8G8B8` atlas via `XGSwizzleRect`. **This is the display path the camera preview copies.** Needs `font_atlas.h`, links `xgraphics.lib`. |
| `dbg.cpp` / `dbg.h` | On-screen ring buffer **and** crash-survivable mirror to `D:\xb_cam.txt` (per-line open/append/close, so the last line on disk survives a bugcheck). Driver logs via `XCam_SetLog`. |
| `input.cpp` / `input.h` | Controller module (GAMEPAD + MEMORY_UNIT). |

### Required external assets
- `font_atlas.h` for `font.cpp`.
- Link: `xapilib`, `xgraphics.lib` (`XGSwizzleRect`), D3D8, kernel lib if separate.

## On-screen / disk logging
No Super I/O on a modded retail box, so `DbgPrint` goes nowhere readable — **the
screen and `D:\xb_cam.txt` are the debug channels** (per-line flush survives a
bugcheck). Pair the EIP from a fatal screen with the linker `.MAP` to land on the
faulting function (see `bugcheck reference.md`).

Breadcrumbs to watch (working path):
- `camera PRESENT but NOT a claimable VID/PID node` → manual path engaged.
- `*** PORT n ENABLED -- device live at addr 0 ***` → port reset worked.
- `*** CAMERA NODE OWNED (VID/PID @ addr0) ***` → we own the device.
- `*** SENSOR SYNCED=1`, `is 76xx (7648-class)=1` → SCCB + sensor up.
- `*** ISO STREAM STARTED ***`, then `completed frames=…` → frames flowing.
- `luma avg=…`, `center R=…` → decode is producing a real image.

## Controls
- **A / START** — begin / retry the camera test
- **B / BACK** — exit
- **Y** — stop camera, return to idle

## Build notes (RXDK)
No `sprintf`/`sscanf`/`strlen`; C89 declaration ordering; file-scope statics; no
per-frame heap (all buffers `MmAllocateContiguousMemory` once, freed once with
`MmFreeContiguousMemory`). Add `main.cpp`, `xb_cam.cpp`, `picojpeg.cpp`, `dbg.cpp`,
`font.cpp`, `input.cpp` + headers; provide `font_atlas.h`; link the libs above.

## Status
Working on hardware: detects, enumerates, brings up the sensor, streams MJPEG, and
displays a live image. Remaining polish: stock-EyeToy PID acceptance, other
resolutions/alts, decode-cost profiling at larger modes.
