# WORKING IMPLEMENTATION — Original Xbox USB Camera (Sony EyeToy)
## Team Resurgent / Darkone83

> **STATUS: WORKING ON HARDWARE.** A live, recognizable image streams from a Sony
> EyeToy on a retail original Xbox — manual USB enumeration, OV519 bridge + OV7648
> sensor bring-up, MJPEG frame assembly, software JPEG decode, swizzled display.
> This document is the **authoritative description of what the shipped driver
> actually does.** Where any other note in this repo disagrees with this file, this
> file wins. The canonical source is `examples/xb_cam.cpp` / `examples/main.cpp`;
> the authoritative USB struct/macro header is `examples/xbox_usb.h`.

This supersedes the project's earlier "standard USB iso-video device, no register
replay, rides XInitDevices" model. That model was wrong on every load-bearing point
(see `RESEARCH.md` §"Superseded models"). The truth is the opposite: we enumerate
the device ourselves, we replay a substantial OV519+OV7648 register init, and the
wire format is MJPEG, not raw RGB/YUV.

---

## 0. The pipeline in one paragraph

The camera is **not** a node in the framework's `g_DeviceTree` — the dashboard never
enumerates it. We walk the tree, find the **TI hub**, scan its downstream ports, and
locate the camera as a port that is *connected but not enabled*. We **reset that
port** (which enables it at USB address 0), **allocate our own device node**
(`g_DeviceTree.AllocDevice`), open EP0, `GET_DESCRIPTOR` to confirm VID/PID,
`SET_ADDRESS` to a framework-allocated address, reopen EP0 at that address,
`SET_CONFIGURATION`, and parse the config descriptor for the iso IN endpoint. Then
we **bring up the OV519 bridge and OV7648 sensor with a real register sequence**
(the gspca `ov519` path — `reg 0x72=0xEE` is the make-or-break), `SET_INTERFACE` to
the 320×240 alt, open/attach/start the iso pipe, and assemble OV519-delimited
**MJPEG** frames from the iso packets using each packet's real `BytesRead`. Each
completed JPEG is decoded with picojpeg to BGRA and pushed to a swizzled
`D3DFMT_A8R8G8B8` texture via `XGSwizzleRect` (the same path the on-screen font
uses), and drawn as a quad.

---

## 1. Hardware confirmed on the bench

- **Device under test:** Sony EyeToy — OmniVision **OV7648** sensor + **OV519** USB
  bridge. Enumerated VID/PID at address 0: **`0x054C / 0x0155`** (decimal 1356/341).
  - Note: `0x0155` is the *video-only* ID. A fully stock EyeToy may present `0x0154`;
    `Cam_IsCameraId()` currently accepts `054C:0155` and `045E:028C`. If you test a
    unit that comes up as `0154`, add it there.
- **Topology:** the camera hangs off the internal **TI USB hub** (VID 1105 / the hub
  reported in the walk), on **hub port 4** in the test rig, which was *connected but
  not enabled* until we reset it.
- **Bus:** OHCI, USB 1.1 full-speed. Iso IN endpoint **`0x81`**, the 320×240 alt
  reports **maxpkt 768**, iso buffer = 8 packets × 768 = 6144 bytes.
- **Sensor identity read back over SCCB:** MID `0x1C=0x7F`, `0x1D=0xA2` (OmniVision);
  PID `0x0A=0x76`, VER `0x0B=0x48` → **OV7648**.

---

## 2. Manual enumeration (we do this; the framework does not)

The camera is not a claimable VID/PID tree node, so the class-driver/`XInitDevices`
attach path never fires for it. The working sequence, all in `xb_cam.cpp`:

1. **`Cam_WalkTree()`** — walk `g_DeviceTree` from base `*(CDeviceTree+0xE0)`, node
   stride `0x20`. Inspect each node; identify the TI hub.
2. **`Cam_ScanHub()`** — on the hub, `GET_DESCRIPTOR(0x2900)` for the hub descriptor
   (port count), then per port `GET_STATUS` (`bmRequestType 0xA3`). The port that is
   **connected (bit0) but not enabled (bit1)** is the camera → `s_camHubPort`.
3. **`Cam_ResetPort()`** — `SET_FEATURE(PORT_RESET=4)` (`bmRequestType 0x23`), poll
   `GET_STATUS` until enabled (bit1), then `CLEAR_FEATURE(C_PORT_RESET=20)`. Device
   is now live **at address 0**.
4. **`Cam_BuildNode()`** — `g_DeviceTree.AllocDevice()` gives us a node we own. Stamp
   it: `nb[0]=0xFE` (connected, *fresh-open* state — **not** `0x05`, which would take
   the parent-routed path), `nb[4]=port`, `nb[5]=0` (addr 0), `nb[6]=8` (EP0 maxpkt).
5. **`Cam_OpenDefaultEP()`** — submit a URB with `Header.Function = 0x82`
   (OpenDefaultEndpoint); fills the EP0 handle at node+8. (`0xC3` =
   CloseDefaultEndpoint for the reopen below.)
6. **`Cam_GetVidPid()`** — `GET_DESCRIPTOR(device,18)`, confirm `Cam_IsCameraId`.
7. **`SET_ADDRESS`** — allocate an address with
   `USBD_AllocateUsbAddress(hc)` where `hc = *(void**)(node+0x0C)`, send
   `SET_ADDRESS`, `Sleep(5)`, set `nb[5]=addr`, **close + reopen EP0** so the control
   path targets the new address.
8. **`SET_CONFIGURATION(1)`**.
9. **`Cam_FindIsoEndpoint(wantAlt=3)`** — see §3.

**Why `nb[0]=0xFE` and not `0x05`:** `0x05` routes EP0 opens through the parent and
the framework's enum kick, which frees the node when no class driver claims it.
`0xFE` selects `OpenDefaultEndpoint`'s fresh-open path, which uses the HC context at
node+0x0C directly. This is the single trick that lets us own a device the framework
would otherwise reclaim.

---

## 3. Endpoint discovery (`Cam_FindIsoEndpoint`)

- All descriptor reads use a **`MmAllocateContiguousMemory`** buffer — OHCI DMA must
  not straddle a page.
- Read the 9-byte config header for `wTotalLength`, then read the largest prefix that
  fits the control-TD pool. The pool cap is real: `TDs = ceil(len / 8) + 3` must fit,
  so on failure the code **shrinks the request to 3/4 and retries** until it succeeds
  (the EyeToy's video interface is interface 0, so its iso endpoints are near the
  front — a prefix is enough). In the test rig `wTotalLength=180`, prefix read = 135.
- Parse interface (`0x04`) / endpoint (`0x05`) descriptors. Select an **iso IN**
  endpoint: `(attr & 0x03)==0x01 && (addr & 0x80)`. Prefer the wanted alt (3 =
  320×240, maxpkt 768); fall back to the largest-maxpkt iso IN found.

---

## 4. OV519 + OV7648 bring-up (`Cam_InitSensor`) — the wall, and how it fell

Full register detail is in **`OV519_OV7648_INIT.md`**. The essential facts:

- **Vendor control protocol (OV519):**
  - bridge write: `bmRequestType 0x41, bRequest 0x01, wValue 0, wIndex=reg`, 1 byte.
  - bridge read: `0xC1, 0x01, 0, reg`, 1 byte.
- **Sensor SCCB (I2C) through the bridge:** write = `OvW(0x42,reg); OvW(0x45,val);
  OvW(0x47,0x01)`. Slave ids set with `OvW(0x41,0x42)`=W_SID, `OvW(0x44,0x43)`=R_SID.
  Read is the `0x43/0x47=3/0x47=5/0x45` handshake in `Cam_I2cR`.
- **THE breakthrough:** in `init_519`, **`reg 0x72` (GPIO_IO_CTRL0) bit 4 (0x10) must
  be cleared — write `0xEE`.** The hardware default `0xFF` leaves the sensor
  invisible; SCCB returns `0xFF` forever and detection fails. This one write is what
  unblocked the entire project.
- **init_ov_sensor:** set slave ids, `I2C(0x12,0x80)` (COM7 reset), `Sleep(150)`,
  then read MID `0x1C/0x1D` expecting `0x7F/0xA2`, retry up to 5× with reset + a
  dummy `0x00` read to resync the I2C state machine.
- **OV7648 QVGA path** (sensor reads `0x0A==0x76`): minimal sensor reset
  (`0x12=0x80` then `0x12=0x14`), bridge 8-bit input (`R20` mask 0x10), the
  `mode_init_ov_sensor_regs` masks (`0x14/0x28/0x2d/0x67/0x74/0x12`, clockdiv
  `0x11=0`), and **`set_ov_sensor_window`** (`0x17=0x1a`, `0x18=0xba`, `0x19=0x03`,
  `0x1a=0xf3`). The window matters: get it wrong and **the iso stream is all zeroes**
  — exactly the symptom we hit before adding it.
- **Bridge geometry + restart:** frame size (`R10=0x14`, `R11=0x1e`, format
  `R25=0x03`), frame rate (`0xa4=0x0c`, `0x23=0xff`), then `ov51x_restart`
  (`0x51` 0x0f→0x00, `0x22=0x1d` FRAR) and **LED on** (`0x71` bit0). After init the
  code re-reads `0x1C/0x1D` to confirm the sensor is still synced.

The OV7620 (`norm_7620`) branch is retained for non-76xx sensors but is not the
EyeToy path.

---

## 5. Iso streaming + MJPEG frame assembly

### 5.1 Lock-proof transfer (`Cam_SubmitPoll`)
Every non-iso URB is submitted with a **NULL completion** and `Header.Status` preset
to a private `PENDING` sentinel (`0x40000000`), then **polled** (sync result lands in
the header; async spins on the status with `Sleep(1)`, cap 2000). No `KEVENT` waits,
no blocking — this is why the box never soft-locks during bring-up.

### 5.2 Start (`Cam_StartStream`)
`Cam_InitSensor` → `SET_INTERFACE(alt 3)` → `ISOCH_OPEN_ENDPOINT(ep 0x81, maxpkt)`
→ allocate the iso buffer (`8 × maxpkt`, contiguous) and the frame-assembly buffers
→ `ISOCH_ATTACH_BUFFER` → `ISOCH_START_TRANSFER(ASAP)`.

**The `Pattern[]` fix:** `USBD_ISOCH_BUFFER_DESCRIPTOR.Pattern[8]` is the per-iso-frame
byte request. Zeroing the descriptor left it 0, so the controller asked for **0 bytes
per frame** → empty stream. The fix is to fill `Pattern[p] = maxpkt` for all 8 frames.
This is non-obvious and is the difference between a silent pipe and a flowing one.

### 5.3 Frame assembly (`Cam_IsoComplete`, `__stdcall`)
The completion is called `__stdcall(status, context)` and is **not** null-checked by
the framework, so it must be non-NULL and `__stdcall`. It re-arms the same attach URB
at the end to keep streaming (attach is synchronous → DPC-safe; no Sleep/poll here).

Per buffer it parses 8 iso packets. **Each packet's real length is
`PacketStatus[p].BytesRead`** (a 12-bit field). Using the real length is essential —
copying the full maxpkt drags stale bytes in and buries the JPEG EOI. OV519 framing:
each packet that starts `FF FF FF` with byte3 `0x50` = **start of frame**, byte3
`0x51` = **end of frame**; SOF carries a **16-byte header** that is stripped. Image
bytes accumulate into `s_frame`; on EOF the frame is published to `s_frameReady` and
`s_completedFrames++`. Full format detail in **`MJPEG_FRAME_FORMAT.md`**.

The wire payload is **baseline MJPEG** (`FF D8 FF E0 …`), ~2.4–3.8 KB/frame — full-speed
USB cannot carry raw 320×240, so the OV519 JPEG engine (enabled via `reg 0x54` bit2)
compresses on-chip.

---

## 6. Decode + display

### 6.1 Decode (`Cam_DecodeJpegToYUY2` — name is legacy; it now outputs BGRA)
picojpeg (integer-only, no malloc/float/libc → RXDK-safe) decodes the published JPEG.
The EyeToy JPEG is 320×240, 3 components, `scanType=2` (YH2V1: MCU 16×8, 20×30 MCUs).
Block layout is handled per `m_MCUWidth/Height` (the `nbx==2,nby==1` case uses block
offsets `{0,64}`). Grayscale (`comps==1`) writes `B=G=R=Y`.

**Pixel order:** output is **BGRA** to match `D3DFMT_A8R8G8B8` byte order on Xbox
(little-endian `0xAARRGGBB` → bytes B,G,R,A). The decode writes
`row[px*4+0]=R, +1=G, +2=B, +3=0xFF` — i.e. **R and B are swapped relative to naive
BGRA** to correct the camera's channel order (the blue-cast fix). If a future sensor
shows the cast again, that swap is the knob.

### 6.2 Display (the proven font path)
`main.cpp` creates the preview as a **`D3DFMT_A8R8G8B8`** texture sized to a power of
two (**512×256**) — swizzle requires pow2 — and samples UVs over the used 320×240
region (`u=320/512`, `v=240/256`). **No `D3DRS_YUVENABLE`** (it's an RGB texture now).

`XCam_DrawToSurface` decodes only on a new completed frame, into a full
**512×256 BGRA** buffer `s_rgb` (image in the top-left, rest black). It blits with
**`XGSwizzleRect(s_rgb, 512*4, NULL, lr.pBits, desc.Width, desc.Height, 0, 4)`** —
full-size source, **NULL rect**, bpp 4. This is byte-for-byte the `font.cpp` pattern,
which is *known to render correctly on this console*. Two earlier display bugs both
came from deviating from it: a YUY2 texture (swizzled format, wrong byte order →
flashing colors) and a **sub-rect** swizzle (320-wide rect into a 512 texture →
horizontal shearing). The full-source NULL-rect swizzle fixed both.

---

## 7. RXDK constraints honored throughout

C89 declaration ordering; file-scope statics; **no `sprintf`/`strlen`/per-frame heap**
(all buffers allocated once via `MmAllocateContiguousMemory`, freed once in
`XCam_Shutdown` with `MmFreeContiguousMemory` — never the wrong free, per
`bugcheck reference.md`); iso completion is non-blocking; `picojpeg.cpp` is added to
the `.vcxproj` and compiles as C++; includes are `<xtl.h>`, `<xgraphics.h>`,
`"xbox_usb.h"`, `"xbox_kernel.h"`, `"dbg.h"`, `"xb_cam.h"`, `"picojpeg.h"`.

---

## 8. What each earlier symptom actually was (debug ledger)

| Symptom | Root cause | Fix |
|---|---|---|
| SCCB reads return `0xFF`, sensor never detected | `reg 0x72` bit4 set (HW default `0xFF`) | `init_519` writes `0x72=0xEE` |
| Iso stream all zeroes | sensor window unset | `set_ov_sensor_window` (`0x17/0x18/0x19/0x1a`) |
| Iso pipe opens but no bytes | `Pattern[8]` left 0 → 0 bytes/frame requested | fill `Pattern[p]=maxpkt` |
| JPEG won't decode / EOI buried | copied full maxpkt, not real length | use `PacketStatus[p].BytesRead` |
| Flashing colors on screen | YUY2 texture is swizzled + wrong byte order | decode to BGRA, `A8R8G8B8` |
| Horizontal shearing | sub-rect `XGSwizzleRect` | full-source NULL-rect swizzle (font path) |
| Blue/purple cast | R↔B channel order | swap R/B in the decode write |

---

## 9. Known follow-ups (not blockers)

- Stock-EyeToy PID (`0x0154`) acceptance in `Cam_IsCameraId` if a non-video-mod unit
  is tested.
- The `maxpkt` for non-320×240 alts and other resolutions (only alt 3 / 320×240 is
  exercised).
- Decode cost: picojpeg per frame is fine at QVGA; larger modes unprofiled.
- Generalization: the manual-enumeration + own-node technique is reusable for other
  non-enumerated USB devices on Xbox (see `RESEARCH.md` follow-ups).
