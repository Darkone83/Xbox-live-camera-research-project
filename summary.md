# What the camera driver actually does
**Team Resurgent / Darkone83**

The source in this repo brings up an OV519-family camera on a retail original Xbox and puts a live picture on screen. The path that's been tested is a Sony EyeToy reporting `054C:0155`, streaming a recognizable 320×240 image.

This is OV519-family support, tied to that chipset. It is not a generic USB webcam / UVC driver.

## The short version

The driver claims an OV519-family camera by hand, configures the OV519 bridge and the OV7xx0 sensor, pulls MJPEG over a USB isochronous endpoint, decodes the JPEG in software, and shows it through an Xbox `D3DFMT_A8R8G8B8` texture.

It accepts two IDs:

- `054C:0155` — the EyeToy / OV519 path that's been tested.
- `045E:028C` — the Japan-only first-party **Xbox Video Camera** (OmniVision OV530, OV519-compatible). Accepted in code, not yet tested on real hardware. This is *not* the Xbox 360 "Xbox Live Vision" camera; it's the rare Video Chat-kit unit that only shipped in Japan.

It does not accept `054C:0154`.

## What the code does, step by step

1. Walks `g_DeviceTree` and reads the USB nodes it can.
2. Finds the internal TI hub and scans the downstream ports.
3. Resets the port the camera is stuck behind (connected but not enabled).
4. Allocates an owned device node via `g_DeviceTree.AllocDevice()`.
5. Opens EP0 at address 0, reads VID/PID, sends `SET_ADDRESS`.
6. Reopens EP0 at the assigned address.
7. `SET_CONFIGURATION(1)`.
8. Parses the config descriptor for an iso IN endpoint, preferring alt 3.
9. `Cam_InitSensor()` configures the bridge and sensor — OV7648-class when reg `0x0A` reads `0x76`, otherwise the OV7620-style fallback.
10. Starts iso streaming (`ISOCH_OPEN_ENDPOINT` / `ATTACH_BUFFER` / `START_TRANSFER`).
11. Reassembles MJPEG using the OV519 SOF/EOF markers and `PacketStatus[p].BytesRead`.
12. Decodes with picojpeg.
13. Writes a 4-byte-per-pixel buffer and swizzles it into an A8R8G8B8 texture.

There's also a direct path for an already-enumerated camera node, if one happens to be in the tree.

## Hardware facts the source pins down

- Validated frame size: 320×240.
- Tested EyeToy: `054C:0155`.
- First-party Xbox Video Camera accepted in code: `045E:028C`.
- Iso endpoint is chosen from the descriptor. The tested unit resolves to `0x81`, alt 3, max packet 768.
- 8 iso packets per attached buffer.
- `Pattern[p] = maxpkt` for all 8, or the stream comes back empty.
- Wire format is MJPEG.
- Preview texture is `A8R8G8B8`, not YUY2.

## Things I got wrong earlier (and what's true now)

The first cut of these notes assumed a lot that the hardware later disproved. Leaving the corrections here so nobody re-introduces them:

| What the old notes said | What's actually true |
|---|---|
| The async USB class driver runs the camera; `CamAddDevice` attaches on hotplug. | The class-driver declarations are still in the tree, but the working path claims the camera manually — hub reset plus `AllocDevice()`. |
| The camera is never a tree node. | The tested path enumerates it manually, but the code also handles a camera that's already enumerated. |
| `0x81` / alt 3 / maxpkt 768 is hardcoded. | It's parsed. Alt 3 is preferred, with a fallback to the largest iso IN endpoint. The tested unit just happens to be `0x81`/3/768. |
| YUY2 preview. | The preview is `A8R8G8B8`. The only YUY2 left is a stale function name. |
| `XCam_Init()` returning OK means streaming started. | It can return OK after a failed bring-up. Check `XCam_IsStreaming()`. |
| RGB24 output. | The wire format is MJPEG; decoded output is 4-byte-per-pixel display data. |

## Known limits

- `Cam_IsCameraId()` accepts `054C:0155` and `045E:028C` only.
- `054C:0154` is a follow-up.
- Only QVGA 320×240 is validated; larger modes aren't profiled.
- The API return behavior needs tightening before this is a library instead of a test app.
- `xb_cam.h` still declares multi-camera helpers that aren't implemented.

## Why this matters

The point isn't just that a picture shows up. It's a working pattern for manually owning and driving a vendor USB video device on the original Xbox when the normal exposed path won't touch it. The OV519 camera is the proof; the enumeration-and-iso plumbing is reusable for the next awkward USB device someone wants to talk to.
