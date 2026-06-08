# Working implementation — original Xbox OV519-family camera driver
**Team Resurgent / Darkone83**

This is the doc that matches what runs. The driver streams a live, recognizable
320×240 image from a Sony EyeToy (`054C:0155`) on a retail original Xbox. Where the
older notes talk about a pure class-driver attach, raw RGB/YUY2 output, or "no register
replay needed," they're describing dead ends — this is the corrected version.

## What's supported

`Cam_IsCameraId()` is the gate:

```c
if (vid == 0x054C && pid == 0x0155) return 1;   /* EyeToy  — tested */
if (vid == 0x045E && pid == 0x028C) return 1;   /* Xbox Video Camera (Japan) — untested */
return 0;
```

So `054C:0155` is the tested EyeToy path; `045E:028C` is the first-party Japan-only
Xbox Video Camera, accepted but not yet run on hardware. `054C:0154` isn't accepted
yet, and generic UVC webcams aren't supported — this is OV519-family only.

## Claiming the camera

The dashboard won't enumerate this camera, so the driver claims it by hand instead of
waiting for a class-driver attach. (There's still class-driver declaration code in the
tree; it isn't the path that runs.) The walk goes:

`Cam_WalkTree()` / `Cam_InspectNode()` step through `g_DeviceTree`, skipping the unsafe
root-hub control transfers, reading real device descriptors where they can, and noting
the TI hub — plus any camera VID/PID node that's already enumerated.

`Cam_ScanHub()` reads the downstream port status with hub class requests. A port that's
connected but not enabled is the camera, stuck.

`Cam_ResetPort()` sends `SET_FEATURE(PORT_RESET)`, polls until the port enables, clears
`C_PORT_RESET`, and leaves the device live at address 0.

`Cam_BuildNode()` allocates our own node through `g_DeviceTree.AllocDevice()` and stamps
the minimum fields the fresh-EP0 path needs: `nb[0]=0xFE`, `nb[4]=port`, `nb[5]=0`,
`nb[6]=8`.

`Cam_OpenDefaultEP()` / `Cam_CloseDefaultEP()` open and close EP0 with URB functions
`0x82` and `0xC3`.

`Cam_BringUpManual()` ties it together: read VID/PID at address 0, confirm the ID,
allocate an address with `USBD_AllocateUsbAddress()`, `SET_ADDRESS`, update the node's
address byte, reopen EP0, `SET_CONFIGURATION(1)`, find the iso endpoint, start streaming.

## Finding the endpoint

`Cam_FindIsoEndpoint()` reads the config descriptor into a contiguous buffer — first the
9-byte header for `wTotalLength`, then as much of the descriptor as the Xbox
control-transfer limits allow, shrinking and retrying if a read is too big. It parses the
interface and endpoint descriptors, looks for iso IN endpoints, prefers the requested alt
(currently `wantAlt = 3`), and falls back to the largest-max-packet iso IN endpoint if
that alt isn't there. On the tested unit this lands on `0x81`, alt 3, max packet 768.

## Bringing up the OV519 and sensor

`Cam_InitSensor()` does the real work. The OV519 does not wake into a working stream on
its own — the registers have to be replayed. In brief:

- Bridge write: vendor OUT, `bmRequestType=0x41`, `bRequest=0x01`, `wIndex=reg`, one byte.
- Bridge read: vendor IN, `0xC1` / `0x01` / `wIndex=reg`, one byte.
- Sensor write over SCCB/I2C through the bridge: register to `0x42`, value to `0x45`, trigger `0x47=0x01`.
- Sensor read: the `0x43` / `0x47=0x03` / `0x47=0x05` / `0x45` handshake.

The one register that makes or breaks the whole thing is `OV519 reg 0x72 = 0xEE` — its
hardware default leaves a GPIO bit set that hides the sensor from SCCB. Detection reads
the manufacturer ID at `0x1C`/`0x1D` (expect `0x7F`/`0xA2`) and the sensor PID/VER at
`0x0A`/`0x0B`. If `0x0A == 0x76` the code takes the OV76xx/OV7648 path; otherwise it
falls back to the retained OV7620 table. The OV7648 path runs reset, QVGA mode, the
sensor window, bridge geometry for 320×240, frame-rate setup, a bridge restart, and the
LED enable. Full sequence in `OV519_OV7648_INIT.md`.

## Streaming

`Cam_StartStream()`: `Cam_InitSensor()`, then `SET_INTERFACE(alt)` on the parsed
interface/alt, `ISOCH_OPEN_ENDPOINT` with the parsed address and max packet, a contiguous
`8 * maxpkt` iso buffer, the frame/JPEG/display buffers, then fill
`USBD_ISOCH_BUFFER_DESCRIPTOR.Pattern[p] = maxpkt` for all 8 packets, `ISOCH_ATTACH_BUFFER`,
`ISOCH_START_TRANSFER(ASAP)`. The completion callback is `Cam_IsoComplete()` — `__stdcall`,
non-blocking, parses the completed packets and re-arms the attach URB.

## Reassembling MJPEG

The wire format is MJPEG, eight iso packets per callback. For each packet, the real byte
count is `PacketStatus[p].BytesRead`. `FF FF FF 50` starts a frame, `FF FF FF 51` ends
one; the SOF packet carries a 16-byte OV519 header that gets stripped, and everything
between gets appended. EOF publishes the accumulated JPEG to `s_frameReady` and bumps
`s_completedFrames`. Using `BytesRead` rather than the full max-packet length is what
fixed the earlier corruption — copying full packets dragged stale bytes into the JPEG.

## Decode and display

`XCam_DrawToSurface()` only decodes when `s_completedFrames` changes: copy `s_frameReady`
into `s_jpegBuf`, run it through picojpeg via `Cam_JpgFeed()`, decode the MCUs into
`s_rgb` at 4 bytes per pixel with alpha forced to `0xFF`. The color write is deliberately:

```c
row[px * 4 + 0] = R;
row[px * 4 + 1] = G;
row[px * 4 + 2] = B;
row[px * 4 + 3] = 0xFF;
```

The source comments call that an R/B swap relative to ordinary BGRA — keep the wording,
it matches the display path. `main.cpp` makes a 512×256 `D3DFMT_A8R8G8B8` texture with the
320×240 image in the top-left, and `XCam_DrawToSurface()` blits it in with
`XGSwizzleRect(s_rgb, 512*4, NULL, ...)`. No YUV render state anywhere — this rides the
same swizzle path as `font.cpp`.

The decode function is still named `Cam_DecodeJpegToYUY2()`. It hasn't produced YUY2 in a
long time; the name is just a leftover and renaming it is on the list.

## The public API

`xb_cam.cpp` implements five functions: `XCam_SetLog()`, `XCam_Init()`,
`XCam_IsStreaming()`, `XCam_DrawToSurface()`, `XCam_Shutdown()`. The usable path today is
`XCam_Init()` → `XCam_IsStreaming()` → `XCam_DrawToSurface()` → `XCam_Shutdown()`.

`xb_cam.h` also still declares `XCam_IsConnected()`, `XCam_GetCount()`, `XCam_GetInfo()`,
and `XCam_Select()` from an old multi-camera design. None of those are implemented here —
treat them as stale until they're built or deleted.

One sharp edge worth knowing: `XCam_Init()` returns `XCAM_STATUS_NO_DEVICE` when nothing's
present, but in both the manual and already-enumerated paths it returns `XCAM_STATUS_OK`
after the bring-up attempt *even if streaming never started* (the already-enumerated path
logs `StartStream rc` and still returns OK). So `XCam_Init() == 0` means "the attempt was
accepted," not "the stream is live" — check `XCam_IsStreaming()` for that. If this ever
becomes a real reusable driver, `Init()` should return `XCAM_STATUS_OPEN_FAILED` when
`s_streaming` is still false; for a test harness it's fine as is.
