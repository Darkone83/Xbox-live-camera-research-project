# Original Xbox Camera Driver — Factual Summary Against Current Code
## Team Resurgent / Darkone83

> **STATUS: WORKING ON HARDWARE.** The current source implements a working OV519-family camera path on retail original Xbox hardware. The validated path streams a recognizable 320×240 image from a Sony EyeToy reporting `054C:0155`.

This is **chipset-specific OV519-family support**, not generic USB webcam/UVC support.

---

## Executive summary

The current implementation manually brings up an OV519-family camera on the original Xbox, initializes the OV519 bridge and OV76xx sensor path, captures MJPEG over USB isochronous transfer, decodes the JPEG frames in software, and displays them through an Xbox `D3DFMT_A8R8G8B8` texture.

The current code accepts these VID/PID pairs:

- `054C:0155` — tested EyeToy / OV519 path
- `045E:028C` — Microsoft Xbox camera path accepted by the code

The code does **not** currently accept `054C:0154`.

---

## What the code actually does

1. Walks `g_DeviceTree` and inspects readable USB nodes.
2. Finds the internal TI hub and scans downstream ports.
3. Resets the connected-but-not-enabled port when the camera is stuck behind the hub.
4. Allocates an owned device node with `g_DeviceTree.AllocDevice()`.
5. Opens default EP0, reads VID/PID at address 0, then sends `SET_ADDRESS`.
6. Reopens EP0 at the assigned USB address.
7. Sends `SET_CONFIGURATION(1)`.
8. Parses the configuration descriptor for an iso IN endpoint, preferring alt 3.
9. Runs `Cam_InitSensor()` to configure the OV519 bridge and OV76xx/OV7648 sensor path.
10. Starts iso streaming with `ISOCH_OPEN_ENDPOINT`, `ISOCH_ATTACH_BUFFER`, and `ISOCH_START_TRANSFER`.
11. Reassembles MJPEG frames using OV519 SOF/EOF packet markers and `PacketStatus[p].BytesRead`.
12. Decodes completed JPEG frames with picojpeg.
13. Writes a 4-byte-per-pixel image buffer and swizzles it into an A8R8G8B8 D3D texture.

The code also has a direct already-enumerated camera-node path if a real camera VID/PID node is found during the tree walk.

---

## Hardware facts reflected by the source

- Validated frame size: 320×240.
- Tested EyeToy VID/PID: `054C:0155`.
- Accepted Microsoft camera VID/PID: `045E:028C`.
- Iso endpoint selection is descriptor-based. The tested path resolves to endpoint `0x81`, alt 3, max packet 768.
- Iso attach uses 8 packets per buffer.
- `Pattern[p] = maxpkt` is required for all 8 iso frames.
- Wire format is MJPEG.
- Display texture is `D3DFMT_A8R8G8B8`, not YUY2.

---

## Important corrections to stale wording

| Stale wording | Current code reality |
|---|---|
| “Underlying driver is the async USB class driver; `CamAddDevice` attaches on hotplug.” | Class-driver declarations still exist, but the working path manually owns the camera through hub reset + `AllocDevice()`. |
| “The camera is never a tree node.” | The validated path uses manual enumeration, but the code also handles an already-enumerated real camera VID/PID node. |
| “Endpoint 0x81 / alt 3 / maxpkt 768 is hardcoded.” | The code parses descriptors, prefers alt 3, and falls back to the largest iso IN endpoint. The tested path is 0x81 / alt 3 / 768. |
| “YUY2 preview.” | The current texture is `D3DFMT_A8R8G8B8`; YUY2 names/comments are legacy. |
| “`XCam_Init()` OK means streaming succeeded.” | Current code can return OK after a bring-up attempt even if streaming failed. Check `XCam_IsStreaming()`. |
| “RGB24 frame output.” | Wire format is MJPEG; decoded output is 4-byte-per-pixel display data. |

---

## Current limitations

- Only `054C:0155` and `045E:028C` are accepted by `Cam_IsCameraId()`.
- `054C:0154` remains a follow-up/test item.
- Only the QVGA 320×240 path is validated.
- Larger modes and alternate settings are not profiled.
- The public API return behavior should be tightened if this becomes a library instead of a test app.
- Several source comments and UI strings still need cleanup to remove retired class-driver/YUY2 wording.
- `xb_cam.h` declares multi-camera helper APIs that are not implemented in the current `xb_cam.cpp`.

---

## Historical value

The important breakthrough is not simply that a camera image appears. The project proves a working pattern for manually owning and driving a USB video-class-like vendor device on the original Xbox when the normal exposed device path is not enough. The OV519 camera path becomes a practical reference for future Xbox USB device work.
