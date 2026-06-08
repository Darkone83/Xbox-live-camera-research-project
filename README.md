# Xbox Camera Test (Camera-test)
**Team Resurgent / Darkone83**

<div align=center>

<img src="https://github.com/Darkone83/Xbox-live-camera-research-project/blob/main/img/Camera_test.png" width=800>

</div>

A homebrew RXDK app that pulls a live picture off an OV519-family USB camera on a retail original Xbox — something the console was never built to do. It walks the USB device tree by hand, claims the camera, brings up the OV519 bridge and the OV7xx0 sensor, streams MJPEG over an isochronous IN endpoint, decodes it in software with picojpeg, and blits the result through a swizzled `D3DFMT_A8R8G8B8` texture. Every step gets mirrored to the screen and to `D:\xb_cam.txt`, because on this hardware a crash with no log is a wasted afternoon.

It works. A real EyeToy on a retail Xbox puts a recognizable 320×240 image on the TV — and it does it on a **completely unmodified** EyeToy: no VID/PID patch, no EEPROM reflash, no solder. That part surprised me. I'd budgeted time for the mod and never needed it.

One thing up front, since it keeps getting misread: this is chipset-specific OV519-family support. It is **not** a generic UVC webcam driver and never claimed to be.

## What it targets

- **Sony EyeToy** (OV519 bridge + OV7648 sensor) — `054C:0155`. This is the one actually tested on hardware.
- **Xbox Video Camera** — the rare Japan-only "Xbox Cam" from the Xbox Video Chat kit (OmniVision OV530, register-compatible with the OV519) — `045E:028C`. The code accepts it, but I haven't had one in hand to confirm.

> For anyone vetting this: the first-party camera here is the Japan-only **Xbox Video Camera**. It is *not* the Xbox 360 "Xbox Live Vision" camera — different console, different chip generation, different decade. They get confused constantly; they're unrelated.

`Cam_IsCameraId()` matches those two IDs only. It does **not** match `054C:0154`, which some stock EyeToys report — add it yourself if you want to test one of those.

## How the bring-up works

The dashboard won't enumerate this camera, so the driver does it manually instead of waiting for a class-driver attach. There's still a class-driver breadcrumb left in the source, but it's not the path that runs. The real sequence:

1. Walk `g_DeviceTree` and find the internal TI hub.
2. Scan the hub's ports for the one the camera is sitting on.
3. Reset that port — it comes up connected but not enabled.
4. Allocate and stamp our own device node with `g_DeviceTree.AllocDevice()`.
5. Open EP0 at address 0, read the device descriptor, check the VID/PID.
6. Assign a USB address, `SET_ADDRESS`, then close and reopen EP0 at the new address.
7. `SET_CONFIGURATION(1)`.
8. Parse the config descriptor for an isochronous IN endpoint.
9. Bring up the OV519 bridge and the sensor (see below).
10. Select the streaming alt-setting, open/attach/start the iso transfer.
11. Reassemble MJPEG frames from the OV519-delimited iso packets.
12. Decode the JPEG and swizzle it into the preview texture.

If a real camera VID/PID node is already sitting in the tree, there's a direct path that skips the manual claim.

## Stream behavior

The tested 320×240 path lands on iso IN endpoint `0x81`, alt 3, max packet 768 — but none of that is hardcoded. `Cam_FindIsoEndpoint()` reads the config descriptor, prefers alt 3, and falls back to the largest iso IN endpoint it can find if alt 3 isn't there. `Cam_StartStream()` then uses whatever the descriptor actually reported.

The iso buffer uses 8 packets per attach. `Pattern[p]` has to be filled with the chosen `maxpkt` for every packet — leave it zero and you're asking for zero bytes, which gets you a very stable stream of nothing. That one cost me a while.

## OV519 and sensor bring-up

The OV519 does not wake up into a working stream on its own for this path — it needs the registers replayed. The details that matter:

- Bridge writes go through vendor request `0x41` / bRequest `0x01`; reads through `0xC1` / `0x01`.
- Sensor SCCB/I2C is driven through OV519 registers `0x41`, `0x44`, `0x42`, `0x45`, `0x47`.
- `reg 0x72 = 0xEE` is mandatory. The hardware default leaves a GPIO bit set that makes the sensor invisible to detection — clearing bit 4 is what makes it show up.
- Sensor ID is read back through `0x1C`/`0x1D`; reg `0x0A == 0x76` selects the OV76xx/OV7648-class path. Anything else drops to an OV7620-style fallback table that's still in the tree but isn't the tested EyeToy path.
- The OV7648 path does QVGA mode, the sensor window, bridge geometry, frame-rate setup, a restart, and the LED enable.

`OV519_OV7648_INIT.md` and `WORKING_IMPLEMENTATION.md` have the register-level notes.

## Frames and display

The wire format is **MJPEG** — not raw RGB24, not I420, not YUY2. The iso completion handler:

- uses `PacketStatus[p].BytesRead` for the real per-packet length (the packets are short most of the time);
- watches for the OV519 SOF/EOF markers `FF FF FF 50` and `FF FF FF 51`;
- strips the 16-byte SOF header;
- accumulates JPEG bytes until EOF, then publishes the frame.

`XCam_DrawToSurface()` decodes the latest frame with picojpeg into a 512×256, 4-byte-per-pixel buffer (image in the top-left 320×240) and pushes it into an `A8R8G8B8` texture with `XGSwizzleRect()`. This is the same swizzle path `font.cpp` uses; getting the preview onto the proven font path is what finally killed the color-flashing and the horizontal shear I'd been fighting on the old YUY2 texture.

One leftover: the decode function is still called `Cam_DecodeJpegToYUY2()`. It hasn't output YUY2 in a long time — the name just stuck. Renaming it is on the list.

## API

`xb_cam.h` exposes the live set:

- `XCam_SetLog()`
- `XCam_Init()`
- `XCam_Shutdown()`
- `XCam_IsStreaming()`
- `XCam_DrawToSurface()`

It also still declares `XCam_IsConnected()`, `XCam_GetCount()`, `XCam_GetInfo()`, and `XCam_Select()` from an earlier multi-camera design. None of those are implemented in this `xb_cam.cpp` — they're leftovers, ignore them until they're either built or deleted.

Worth knowing: `XCam_Init()` can return `XCAM_STATUS_OK` after a bring-up attempt even when streaming never actually started. Check `XCam_IsStreaming()` before you trust the preview. If this ever turns into a real reusable driver, `Init()` should fail when `s_streaming` is still false — but for a test harness it's fine.

## Files

| File | Role |
|---|---|
| `src/Camera-test/main.cpp` | RXDK harness: D3D8 setup, UI, input, preview texture, calls into `XCam_*`. |
| `src/Camera-test/xb_cam.cpp` | The driver: tree walk, hub reset, node creation, descriptor parsing, OV519/sensor bring-up, iso streaming, MJPEG assembly, picojpeg decode, swizzled display copy. |
| `src/Camera-test/xb_cam.h` | Public API. |
| `src/Camera-test/xbox_usb.h` | Xbox USB structs, URB helpers, descriptors, iso structs. The authoritative USB header for this project. |
| `src/Camera-test/picojpeg.cpp/.h` | Software baseline JPEG decoder. |
| `src/Camera-test/dbg.cpp/.h` | On-screen + `D:\xb_cam.txt` logging. |
| `src/Camera-test/font.cpp/.h` | Font rendering and the swizzled A8R8G8B8 display path the preview rides on. |
| `src/Camera-test/input.cpp/.h` | Controller input and device init. |

## Controls

- **A / START** — start (or retry) the camera
- **Y** — stop and return to idle
- **B / BACK** — exit

## Known limits

- OV519-family only. Not generic UVC.
- Only `054C:0155` and `045E:028C` are accepted; `054C:0154` isn't unless you add it.
- Only the 320×240 path has been exercised. Larger modes and other alt-settings aren't profiled.
- `XCam_Init()` return semantics are loose (see above).
- `xb_cam.h` still advertises the unimplemented multi-camera helpers.
