# Xbox Camera Test (Camera-test) — Team Resurgent / Darkone83

<div align=center>

<img src="https://github.com/Darkone83/Xbox-live-camera-research-project/blob/main/img/Camera_test.png" width=600>

</div>

A homebrew RXDK test app and USB camera driver for **OV519-family cameras on the original Xbox**, currently targeting:

- **Sony EyeToy / OV519 + OV7648 path** — tested VID/PID: `054C:0155`
- **Microsoft Xbox camera path** — accepted VID/PID in code: `045E:028C`

The driver manually brings up the camera on real Xbox hardware, initializes the OV519 bridge and OV7xx0 sensor path, streams **MJPEG** over a USB isochronous IN endpoint, decodes frames in software with picojpeg, and displays the live image through a swizzled `D3DFMT_A8R8G8B8` texture. Logging is mirrored to the screen and `D:\xb_cam.txt` for crash-survivable debugging.

> **STATUS: WORKING ON HARDWARE.** A recognizable 320×240 image streams from a real EyeToy on a retail original Xbox. This is chipset-specific OV519-family support, not generic UVC webcam support.

> **Scope note.** `Cam_IsCameraId()` currently accepts `054C:0155` and `045E:028C`. It does **not** currently accept `054C:0154`; add that only if you intentionally want to test stock EyeToy variants that report that PID.

---

## Architecture implemented by the code

The validated EyeToy path uses **manual USB bring-up** rather than relying on a normal camera class-driver attach. The source still contains a class-driver declaration/resource breadcrumb, but the working path is:

1. Walk `g_DeviceTree`.
2. Find the internal TI hub.
3. Scan hub ports for the connected camera port.
4. Reset the connected-but-not-enabled port.
5. Allocate and stamp an owned device node with `g_DeviceTree.AllocDevice()`.
6. Open default EP0 at address 0.
7. Read the device descriptor and confirm VID/PID.
8. Allocate a USB address, send `SET_ADDRESS`, then close/reopen EP0.
9. Send `SET_CONFIGURATION(1)`.
10. Parse the config descriptor for an iso IN endpoint.
11. Initialize the OV519 bridge and OV7xx0 sensor path. The code uses an OV7648-class path when PID high is `0x76`, otherwise an OV7620-style fallback table.
12. Select the streaming interface alt, open/attach/start iso transfer.
13. Reassemble MJPEG frames from OV519-delimited iso packets.
14. Decode JPEG to a 4-byte-per-pixel display buffer and swizzle it into an Xbox texture.

The code also has a direct path for an already-enumerated real camera VID/PID node if one is found in the device tree.

---

## Endpoint and stream behavior

The validated 320×240 path uses iso IN endpoint `0x81`, alt 3, max packet size 768 on the tested device. The code does not blindly assume those values in all cases:

- `Cam_FindIsoEndpoint(..., wantAlt=3, ...)` reads the configuration descriptor.
- It prefers alt 3.
- If alt 3 is not found, it falls back to the largest max-packet iso IN endpoint found.
- `Cam_StartStream()` uses the parsed interface, alt, endpoint address, and max packet size.

The iso buffer uses 8 packets per attach. `Pattern[p]` must be filled with the selected `maxpkt`; leaving it zero requests zero bytes and produces an empty stream.

---

## OV519 / sensor bring-up

The driver performs real register replay. The OV519 does not self-configure into a working stream for this homebrew path.

Important implemented details:

- OV519 bridge writes use vendor control request `0x41 / bRequest 0x01`.
- OV519 bridge reads use `0xC1 / bRequest 0x01`.
- Sensor SCCB/I2C is driven through OV519 registers `0x41`, `0x44`, `0x42`, `0x45`, and `0x47`.
- `reg 0x72 = 0xEE` is required in the implemented bring-up; leaving the hardware default blocked sensor detection.
- Sensor manufacturer ID is checked through `0x1C/0x1D`.
- `0x0A == 0x76` selects the OV76xx/OV7648-class path.
- The OV7648 path applies QVGA mode, sensor window, bridge geometry, frame-rate setup, restart, and LED enable.
- A non-76xx OV7620-style branch remains in the source but is not the primary tested EyeToy path.

See `OV519_OV7648_INIT.md` and `WORKING_IMPLEMENTATION.md` for the register-level notes.

---

## MJPEG frame assembly and display

The wire format is **MJPEG**, not raw RGB24, I420, or YUY2.

The iso completion path:

- uses `PacketStatus[p].BytesRead` as the real packet length;
- detects OV519 SOF/EOF markers: `FF FF FF 50` and `FF FF FF 51`;
- strips the 16-byte SOF packet header;
- accumulates JPEG bytes into a frame buffer;
- publishes the completed JPEG frame on EOF.

`XCam_DrawToSurface()` decodes newly completed JPEG frames with picojpeg and writes into a 512×256, 4-byte-per-pixel buffer for a 320×240 image in the top-left. The texture is `D3DFMT_A8R8G8B8` and is filled with `XGSwizzleRect()`.

The decode function is still named `Cam_DecodeJpegToYUY2()` for legacy reasons, but it no longer outputs YUY2.

---

## Public API behavior

`xb_cam.h` exposes:

- `XCam_SetLog()`
- `XCam_Init()`
- `XCam_Shutdown()`
- `XCam_IsStreaming()`
- `XCam_DrawToSurface()`

The header also declares older detection/multi-camera helper prototypes (`XCam_IsConnected()`, `XCam_GetCount()`, `XCam_GetInfo()`, `XCam_Select()`), but the current `xb_cam.cpp` in this bundle does **not** implement those functions. Treat those declarations as stale until implementations are added or the prototypes are removed.

Important current behavior: `XCam_Init()` may return `XCAM_STATUS_OK` after attempting bring-up even if streaming did not actually start. Callers should check `XCam_IsStreaming()` before treating the camera as live. A future code cleanup should make `XCam_Init()` return `XCAM_STATUS_OPEN_FAILED` if `s_streaming` is still false after bring-up.

---

## Files

| File | Role |
|---|---|
| `src/Camera-test/main.cpp` | RXDK test harness, D3D8 setup, UI, input, preview texture creation, calls the `XCam_*` API. |
| `src/Camera-test/xb_cam.cpp` | Camera implementation: device-tree walk, hub scan/reset, owned node creation, descriptor parsing, OV519/OV7xx0 bring-up, iso streaming, MJPEG assembly, picojpeg decode, swizzled display copy. |
| `src/Camera-test/xb_cam.h` | Public API for init, shutdown, streaming-state checks, logging, and draw-to-texture. |
| `src/Camera-test/xbox_usb.h` | Xbox USB structs, URB helpers, descriptors, class-driver declarations, and iso structs used by this project. |
| `src/Camera-test/picojpeg.cpp/.h` | Software baseline JPEG decoder used for MJPEG frames. |
| `src/Camera-test/dbg.cpp/.h` | On-screen and `D:\xb_cam.txt` logging. |
| `src/Camera-test/font.cpp/.h` | Font rendering and the proven swizzled A8R8G8B8 display path. |
| `src/Camera-test/input.cpp/.h` | Controller input and device initialization. |

---

## Controls

- **A / START** — begin or retry the camera test
- **Y** — stop the camera and return to idle
- **B / BACK** — exit

---

## Current limitations

- Not generic UVC support.
- Publicly accepted IDs are limited to `054C:0155` and `045E:028C` in the current code.
- `054C:0154` is not accepted unless you add it.
- Only the 320×240 path has been exercised as the validated path.
- Larger modes and alternate settings are not profiled.
- `XCam_Init()` return semantics should be tightened if this becomes a reusable driver API.
- `xb_cam.h` declares older multi-camera helper APIs that are not implemented in the current `xb_cam.cpp`.
