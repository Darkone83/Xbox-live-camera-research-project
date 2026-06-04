# Working Implementation — Original Xbox OV519-Family Camera Driver
## Team Resurgent / Darkone83

> **STATUS: WORKING ON HARDWARE.** The current implementation streams a live, recognizable 320×240 image from a Sony EyeToy reporting `054C:0155` on retail original Xbox hardware.

This document describes what the current source in `src/Camera-test/` actually does. Where older notes mention a pure class-driver model, raw RGB/YUY2 output, or no register replay, those notes are historical only.

---

## 1. Scope

Current code support is limited to OV519-family camera paths recognized by `Cam_IsCameraId()`:

```c
if (vid == 0x054C && pid == 0x0155) return 1;
if (vid == 0x045E && pid == 0x028C) return 1;
```

This means:

- `054C:0155` is accepted and is the tested EyeToy path.
- `045E:028C` is accepted by the code for the Microsoft Xbox camera path.
- `054C:0154` is not accepted yet.
- Generic UVC webcams are not supported.

---

## 2. Detection and manual bring-up

The validated path is manual USB bring-up. The source still contains class-driver declaration/resource code, but working camera ownership happens through the following functions:

### `Cam_WalkTree()` / `Cam_InspectNode()`
Walks `g_DeviceTree`, skips unsafe root hub control transfers, reads real device descriptors where possible, identifies the TI hub, and records direct camera VID/PID nodes if present.

### `Cam_ScanHub()`
Uses hub class requests to read downstream port status. A connected-but-not-enabled port is treated as the stuck camera port.

### `Cam_ResetPort()`
Sends `SET_FEATURE(PORT_RESET)`, polls until the port is enabled, clears `C_PORT_RESET`, and leaves the device live at address 0.

### `Cam_BuildNode()`
Allocates an owned node through `g_DeviceTree.AllocDevice()` and stamps the minimum fields needed for the fresh EP0 open path:

- `nb[0] = 0xFE`
- `nb[4] = port`
- `nb[5] = 0`
- `nb[6] = 8`

### `Cam_OpenDefaultEP()` / `Cam_CloseDefaultEP()`
Uses URB function `0x82` to open EP0 and `0xC3` to close it.

### `Cam_BringUpManual()`
Reads VID/PID at address 0, confirms the camera ID, assigns a USB address through `USBD_AllocateUsbAddress()`, sends `SET_ADDRESS`, updates the node address byte, reopens EP0, sends `SET_CONFIGURATION(1)`, finds the iso endpoint, and starts streaming.

---

## 3. Endpoint discovery

`Cam_FindIsoEndpoint()` reads the config descriptor using a contiguous buffer. It first reads the 9-byte header to get `wTotalLength`, then reads a descriptor prefix that fits the Xbox control-transfer constraints by shrinking and retrying if needed.

Endpoint selection logic:

1. Parse interface descriptors and endpoint descriptors.
2. Find iso IN endpoints.
3. Prefer the requested alt setting, currently `wantAlt = 3`.
4. If that is unavailable, fall back to the largest max-packet iso IN endpoint found.

On the tested 320×240 path, this resolves to endpoint `0x81`, alt 3, max packet 768.

---

## 4. OV519 and sensor initialization

`Cam_InitSensor()` performs the actual camera bring-up. The key implemented pieces are:

- OV519 bridge write: vendor OUT `bmRequestType=0x41`, `bRequest=0x01`, `wIndex=reg`, one byte.
- OV519 bridge read: vendor IN `bmRequestType=0xC1`, `bRequest=0x01`, `wIndex=reg`, one byte.
- Sensor SCCB/I2C write through OV519: write register to `0x42`, value to `0x45`, trigger `0x47=0x01`.
- Sensor read uses the `0x43`, `0x47=0x03`, `0x47=0x05`, `0x45` handshake.

Critical implemented register:

```text
OV519 reg 0x72 = 0xEE
```

The code comments identify this as the make-or-break GPIO direction change needed for sensor detection.

Sensor detection reads:

- Manufacturer ID: `0x1C`, `0x1D`, expected `0x7F`, `0xA2`
- Sensor PID/VER: `0x0A`, `0x0B`

If `0x0A == 0x76`, the code uses the OV76xx/OV7648-class path. Otherwise, it uses the retained OV7620-style table.

The OV7648 path applies reset, QVGA mode, sensor window, bridge geometry for 320×240, frame-rate setup, bridge restart, and LED enable.

---

## 5. Iso streaming

`Cam_StartStream()` performs:

1. `Cam_InitSensor()`
2. `SET_INTERFACE(alt)` using the parsed interface and alt
3. `ISOCH_OPEN_ENDPOINT` using the parsed endpoint address and max packet size
4. contiguous iso buffer allocation: `8 * maxpkt`
5. frame/JPEG/RGB buffer allocation
6. fill `USBD_ISOCH_BUFFER_DESCRIPTOR.Pattern[p] = maxpkt` for all 8 packets
7. `ISOCH_ATTACH_BUFFER`
8. `ISOCH_START_TRANSFER(ASAP)`

The attach completion callback is `Cam_IsoComplete()`. It is `__stdcall`, non-blocking, parses completed packets, and re-arms the attach URB.

---

## 6. MJPEG frame assembly

The wire format is MJPEG. The completion path processes 8 iso packets per callback.

For each packet:

- `PacketStatus[p].BytesRead` is treated as the real valid byte count.
- `FF FF FF 50` starts a frame.
- `FF FF FF 51` ends a frame.
- The SOF packet has a 16-byte OV519 header that is stripped.
- Intermediate packet data is appended while inside a frame.
- EOF publishes the accumulated JPEG to `s_frameReady` and increments `s_completedFrames`.

This avoids the earlier corruption problem where copying full max-packet lengths dragged stale bytes into the JPEG stream.

---

## 7. JPEG decode and display

`XCam_DrawToSurface()` decodes only when `s_completedFrames` changes.

Decode path:

- copy `s_frameReady` into `s_jpegBuf`
- feed picojpeg through `Cam_JpgFeed()`
- decode MCUs into `s_rgb`
- write 4 bytes per pixel with alpha set to `0xFF`

The function name `Cam_DecodeJpegToYUY2()` is legacy. It now produces a 4-byte-per-pixel display buffer, not YUY2.

The code intentionally writes color channels as:

```c
row[px * 4 + 0] = R;
row[px * 4 + 1] = G;
row[px * 4 + 2] = B;
row[px * 4 + 3] = 0xFF;
```

The source comments call this an R/B swap relative to the normal BGRA slot expectation. Keep that wording because it reflects the current implementation.

Display path:

- `main.cpp` creates a `D3DFMT_A8R8G8B8` texture.
- Texture size is 512×256, with the 320×240 camera image in the top-left.
- `XCam_DrawToSurface()` calls `XGSwizzleRect(s_rgb, 512*4, NULL, ...)` into the texture.
- No YUV render state is used.

---

## 8. Implemented public API

The current `xb_cam.cpp` implements:

- `XCam_SetLog()`
- `XCam_Init()`
- `XCam_IsStreaming()`
- `XCam_DrawToSurface()`
- `XCam_Shutdown()`

`xb_cam.h` also declares older helper prototypes — `XCam_IsConnected()`, `XCam_GetCount()`, `XCam_GetInfo()`, and `XCam_Select()` — but those are not implemented in the current `xb_cam.cpp`. Treat them as stale declarations unless they are later implemented.

## 9. API behavior caveat

Current `XCam_Init()` return semantics are loose:

- It returns `XCAM_STATUS_NO_DEVICE` if `s_present` is false.
- In the manual path, it returns `XCAM_STATUS_OK` after attempting bring-up, even if streaming did not start.
- In the already-enumerated path, it logs `StartStream rc` but still returns OK.

Therefore the factual API rule is:

> `XCam_Init() == 0` means the detection/start attempt was accepted. Use `XCam_IsStreaming()` to confirm the stream is actually live.

A future cleanup should return `XCAM_STATUS_OPEN_FAILED` if `s_streaming` remains false.

---

## 10. Current source cleanup notes

The implementation is working, but stale names/comments remain:

- `xb_cam.h` top comment still describes the retired async class-driver model.
- `main.cpp` still says the path is not runtime validated.
- `main.cpp` UI/comments still say YUY2 in several places.
- `Cam_DecodeJpegToYUY2()` name is legacy.
- `CAM_FRAME_BYTES` and some comments still mention RGB24/YUY2.
- `xb_cam.h` declares multi-camera helper APIs that are not implemented in `xb_cam.cpp`.

These are documentation/comment cleanup items, not evidence that the implemented path is still theoretical.
