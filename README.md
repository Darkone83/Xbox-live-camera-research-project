# cameratest

**Original Xbox camera test harness — Team Resurgent / Darkone83**

A small RXDK diagnostic program for the original Xbox that exercises the
`xb_cam` interface: the reverse-engineered access path for a **Sony EyeToy**
(`054C:0155`, OmniVision OV519) and the **Xbox Live Camera** (`045E:028C`,
OV530). The build targets the EyeToy. It runs the full camera bring-up,
reports the outcome on screen, and draws a live preview when the camera
streams. If no camera is detected it shows a friendly **CAMERA NOT FOUND**
screen and lets you retry — it never hard-crashes on a missing camera.

> ⚠️ **Research project.** The camera access path in `xb_cam.*` is
> reconstructed from *static analysis only* of the Xbox Video Chat XBE
> (MS-124) and has not yet been validated on hardware. The whole point of this
> harness is to find out *where* the sequence works and where it breaks. Treat
> everything here as a working hypothesis, not a finished driver. See
> [`RESEARCH.md`](RESEARCH.md) for the full reverse-engineering notes and the
> per-area confidence assessment.

## What it does

On launch the harness sits at an idle screen. Press **A** and it will:

1. Create a `D3DFMT_YUY2` preview texture (320×240).
2. Call `XCam_Init(0)` — which writes the `CameraSetting` lifecycle registry
   keys, reads back `CameraStatusPath`, opens the device, and walks the
   `IOCTL 0x100 → 0x101 → 0x107` enumerate/format/start path, then spins up the
   capture thread.
3. If init succeeds, switch to a **live preview**: each frame it pulls the
   latest YUY2 frame into the texture (`XCam_DrawToSurface`) and draws it. The
   NV2A converts YUY2→RGB during sampling via the `D3DRS_YUVENABLE` extension.
4. If the camera was **never detected** (the `CameraSetting` key is missing or
   `CameraStatusPath` is never populated), show a friendly **CAMERA NOT FOUND**
   screen with EyeToy-specific checks (plugged in? right port? usbcamd present?
   video-only descriptor?) and offer a retry.
5. If the camera *was* detected but a **later stage failed** (open / format /
   start), show the technical result — the `XCam_Init` return code (NTSTATUS,
   in hex) — so the failing stage can be diagnosed.

Detailed, per-stage progress is logged by `xb_cam.cpp` through
`OutputDebugString`, so watch the **debug / serial channel** to see exactly
which step failed (e.g. `CameraStatusPath not populated`, `IOCTL_SET_FORMAT
failed`).

Controller input is handled by the shared **ScreenChat input module**
(`input.cpp` / `input.h`): `InitInput()` registers the device types and opens
pads, `PumpInput()` runs each frame, and the harness reads the unified `BTN_*`
mask from `GetButtons()` (deriving press edges locally).

## Controls

| Button | Action |
|--------|--------|
| **A** / **START** | Begin / retry the camera test |
| **Y** | Stop the camera and return to idle (during preview) |
| **B** / **BACK** | Exit |

## Files

| File | Role |
|------|------|
| `cameratest.cpp` | The test harness — D3D8 setup, on-screen UI, the state machine that drives `xb_cam`, and the graceful not-found path. |
| `input.cpp` / `input.h` | Shared ScreenChat controller module: device registration, hotplug, Duke/Type-S detection, and the unified `BTN_*` mask. |
| `xbox_native.h` | Native NT/Rtl declaration shim (the RXDK XTL headers omit these). Declares the real xboxkrnl exports `xb_cam.cpp` needs, with Xbox-correct ANSI `OBJECT_ATTRIBUTES`. Include after `<xtl.h>`; link `xboxkrnl.lib`. |
| `gfxfont.h` | Embedded 8×8 bitmap font (ASCII `0x20`–`0x5F`) used to render the on-screen status text. No external font assets. |
| `xb_cam.h` / `xb_cam.cpp` | The camera interface under test. Reference / example module — reconstructed from the RE work. |
| `RESEARCH.md` | The full reverse-engineering write-up: hardware, driver architecture, camera object layout, registry + IOCTL maps, startup/shutdown sequences, the EyeToy hex patch, and the `XDEVICE_TYPE_CAMERA` finding. |

All sources are flat in the project root — no path-prefixed includes (local
headers are included with `""`).

## Building

This targets the original Xbox under **RXDK** (MSVC 2003 / C89 declaration
ordering). Add `cameratest.cpp`, `input.cpp`, the headers, and `xb_cam.cpp` to
an RXDK project and build an XBE.

**SDK headers are not committed here.** The harness and the input module
compile against the standard RXDK `XTL.h` / D3D8 / WinBase / Xbox headers
(`XInput*`, `XGetDeviceChanges`, `D3D*`, `Sleep`, `OutputDebugString`).

`xb_cam.cpp` additionally needs the **native NT / registry declarations**
(`NtOpenFile`, `NtDeviceIoControlFile`, `RegOpenKeyExW`, `OBJECT_ATTRIBUTES`,
`UNICODE_STRING`, `NT_SUCCESS`, …). These live in the kernel-side RXDK headers,
not in the trimmed public XTL set — make sure your include path points at a
full RXDK install.

RXDK build conventions honoured throughout: no `sprintf`/`sscanf`/`strlen`,
C89 declaration ordering, file-scope statics for persistent state, and no
per-frame heap allocations (the font and camera textures are created once).

## Hardware notes

- **Sony EyeToy** is the build target, and compatibility is **confirmed**: the
  VID/PID hex patch on the detection path (`045E:028C` → `054C:0155`) is proven,
  because OV519 (EyeToy) and OV530 (Xbox Cam) are register-level compatible —
  once the comparison is patched the driver drives either chip identically. See
  the *EyeToy Patch* section of [`RESEARCH.md`](RESEARCH.md). The EyeToy must
  still enumerate as a single video interface; composite descriptors may be
  rejected even after the patch.
- **Xbox Live Camera** works as-is once detected, with no patch.

## Native API status (important)

The camera module `xb_cam.cpp` is a prototype written against desktop Win32 /
NT conventions. Cross-referencing `xboxkrnl.pdb` and the decompiled Video Chat
XBE shows three things that must be reconciled before it builds and runs:

- The **Win32 `Reg*` API does not exist on Xbox**, and the kernel exports **no
  registry-key functions** at all. The real app's `CameraSetting` access goes
  through an internal registry-provider vtable that is dashboard/system-app
  infrastructure — not reachable from a normal homebrew title.
- Object names for device opens are **ANSI** (`OBJECT_ATTRIBUTES.ObjectName` is
  `PANSI_STRING`), opened via `NtCreateFile`/`NtOpenFile`.
- The kernel-level surface available to homebrew is file + IOCTL + EEPROM:
  `NtCreateFile`, `NtOpenFile`, `NtReadFile`, `NtWriteFile`, `NtClose`,
  `NtDeviceIoControlFile`, `RtlInitAnsiString`, `ExQueryNonVolatileSetting`.

`xbox_native.h` provides correct declarations for that kernel surface. But the
decompilation also shows the camera is **never opened as a handle** and the
`0x100/0x101/0x107` "IOCTLs" are the minidriver's *internal* dispatch — USB I/O
goes through the **OHCI host controller directly, in-process**. So the real
homebrew path is a USB/OHCI-level driver (reimplementing the `usbcamd`
minidriver), not an "open handle + IOCTL" client. See RESEARCH.md →
*Cross-Reference Corrections*. Until that exists, `xb_cam.cpp` reports
`XCAM_STATUS_NO_DEVICE` and the harness shows **CAMERA NOT FOUND**.

## Known open questions

These are the things most likely to make the harness land on the failure
screen, drawn from the *Remaining Unknowns* in `RESEARCH.md`:

- The exact DWORD **trigger values** for the lifecycle registry keys
  (currently assumed `1`).
- The **`CameraStatusPath`** string format the driver publishes.
- The **format descriptor** layout for `IOCTL 0x101` (modelled on
  `KSDATARANGE_VIDEO`, minimum `0x98` bytes).
- Whether **usbcamd** loads automatically on detection or needs an explicit
  trigger.

If you get a hardware capture working, the return code and the debug log from a
run are exactly the data needed to close these out.

---

*Team Resurgent · Darkone83 · GitHub: [Darkone83](https://github.com/Darkone83)*