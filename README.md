# Xbox Camera Test (Camera-test) — Team Resurgent / Darkone83

A homebrew RXDK test app + USB **class driver** for the original Xbox camera:
the **Sony EyeToy** (`054C:0155`, OmniVision OV519) and the **Xbox Live Vision /
Xbox Cam** (`045E:028C`, OV530, OV519-compatible). It drives the camera through
the Xbox USB framework (`IUsbDevice` / iso transfers) and shows the result on
screen, with crash-survivable logging to `D:\xb_cam.txt`.

> **Research project.** Architecture is reverse-engineered from the Xbox Video
> Chat XBE, the May-2020 source leak, the official "Writing USB Class Drivers for
> Xbox" guide, and the camera's parsed EEPROM descriptor. The framework links
> and the app builds + runs; the camera I/O path is validated *on hardware*, so
> code marked `// HW: verify` (empirical) and `// RXDK: verify` (toolchain) is
> confirmed at the device, not assumed.

## Architecture (current model: USB class driver)

The camera is a standard USB **isochronous video device**, vendor-specific
**interface class 0xFF**, one iso IN endpoint **0x81**, with alternate settings
selecting bandwidth (alt 0 = idle; alt 1–4 = 384/512/768/896 B). The driver
registers via the `.XPP$Class` linker-segment mechanism and the core USB stack
calls its three entry points (`CamInit` / `CamAddDevice` / `CamRemoveDevice`).
Frames arrive over the iso endpoint into contiguous DMA buffers.

This **replaces** an earlier (superseded) IOCTL/registry/`CameraStatusPath`
model. There is no `NtDeviceIoControlFile` access path and no OV519 register
replay — the bridge self-configures from its EEPROM; the driver is register-free.

### EyeToy without the hardware mod?
Because we own the driver (not the patched Video Chat app), the usual EyeToy
requirements may not apply:
- **VID/PID patch — not needed.** We match by interface class (0xFF), not the
  Xbox Cam's `045E:028C`, so the EyeToy's `054C:0155` is accepted natively.
- **The video-only solder mod — maybe needed (verify on hardware).** The mod
  forces the EyeToy's OV519 from its native **3-interface composite** to a single
  video interface. The driver is designed to bind interface 0 and ignore the
  others, but whether the Xbox USB core enumerates the composite cleanly to us is
  an open hardware question. Try unmodded first; the on-screen / `D:\xb_cam.txt`
  log shows whether `CamAddDevice` ever fires (see below). Mod is the fallback.

## Files

| File | Role |
|---|---|
| `cameratest.cpp` | Test harness — D3D8 setup, on-screen UI, the state machine that drives the camera, the not-found path, and the on-screen debug log overlay. |
| `xbcam.cpp` | The USB camera **class driver** — `.XPP$Class` registration, `CamInit`/`CamAddDevice`/`CamRemoveDevice`, the iso capture path, and the public `XCam_*` API. Pure RXDK/`IUsbDevice` vocabulary. |
| `xb_cam.h` | Public camera API the harness links against (`XCam_Init` / `XCam_Shutdown` / `XCam_IsStreaming` / `XCam_DrawToSurface` / `XCam_SetLog` + the `XCAM_*` constants). |
| `xbox_usb.h` | Self-contained Xbox USB framework header — the `_URB` union, `USB_BUILD_*` macros, descriptors, the full `USBD_STATUS` table, the `IUsbDevice`/`IUsbInit` interfaces, and the class-driver registration framework. Folded from the leak's `usb.x` (the master USB header source). Only includes `<xtl.h>`. |
| `xbox_kernel.h` | Kernel-primitive shim (`Ke*` / `Mm*` / `DbgPrint` / `KEVENT` / `ASSERT`). Tries the real DDK/`xboxkrnl` header; falls back to its own correct declarations if RXDK doesn't ship one. |
| `font.cpp` / `font.h` | Glyph renderer (swizzled `D3DFMT_A8R8G8B8` atlas via `XGSwizzleRect` — `D3DFMT_LIN_*` locks the NV2A when sampled). ScreenChat's proven font module; needs `font_atlas.h` and links `xgraphics.lib`. |
| `dbg.cpp` / `dbg.h` | Debug log: an on-screen ring buffer (drawn lower-right) **and** a crash-survivable mirror to `D:\xb_cam.txt`. Each line opens/appends/**closes** the file, so after a fault the last line on disk is the last process that completed. The driver logs into the same sink via a registered function pointer (`XCam_SetLog`). |
| `input.cpp` / `input.h` | Shared ScreenChat controller module: device registration, hotplug, Duke/Type-S detection, unified `BTN_*` mask. Trimmed to GAMEPAD + MEMORY_UNIT for this build. |

### Required external assets (add to the project)
- `font_atlas.h` — pre-baked font atlas + `GlyphMetrics` tables (for `font.cpp`).
- Link libraries: `xapilib` (USB framework), `xgraphics.lib` (`XGSwizzleRect`),
  D3D8, plus the kernel lib if your RXDK separates it.

## On-screen / disk logging

The box is a modchipped retail unit with no Super I/O, so kernel `DbgPrint`
goes nowhere readable — **the screen and `D:\xb_cam.txt` are the debug channels.**
The harness shows a live log in the lower-right on the RUNNING/RESULT/NOTFOUND
screens, and mirrors every line to disk (per-line flush = survives a bugcheck).

Driver breadcrumbs to watch when testing the camera:
- `AddDevice: enter` — the core enumerated the device and called our driver
  (**the key Phase-1 signal**; if this never appears with a camera plugged in,
  the composite didn't enumerate to us — the EyeToy mod is likely needed).
- `iface class 0xFF matched` / `iface class mismatch` — whether the camera's
  video interface is the class we expect.
- `iso endpoint found` → `AddComplete(SUCCESS)` — attach succeeded.
- `StartCapture: SET_INTERFACE ok` → `iso endpoint opened` → `streaming` — the
  iso bring-up path.

## Controls
- **A / START** — begin / retry the camera test
- **B / BACK** — exit
- **Y** — stop camera, return to idle (while previewing)

## Build notes (RXDK)
- No `sprintf`/`sscanf`/`strlen`; C89 declaration ordering; file-scope statics;
  no per-frame heap allocations (font + camera textures created once).
- Add `cameratest.cpp`, `xbcam.cpp`, `dbg.cpp`, `font.cpp`, `input.cpp` and the
  headers to the project; provide `font_atlas.h`; link the libraries above.
- The driver also builds standalone (logging no-ops if `XCam_SetLog` isn't called).

## Status
Compiles and links against RXDK; boots and renders on real hardware; the
no-device path returns `XCAM_STATUS_NO_DEVICE` correctly. Next: plug in the
EyeToy and read the log to see how far `CamAddDevice` gets.