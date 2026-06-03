# OV519 ISO PACKET + MJPEG FRAME FORMAT — reference
## Team Resurgent / Darkone83

> **STATUS: HARDWARE-VERIFIED.** Describes the on-wire format the EyeToy actually
> delivers and how the working driver (`src/Camera-Test/xb_cam.cpp`, `Cam_IsoComplete` +
> `Cam_DecodeJpegToYUY2`) reassembles and decodes it. This replaces the earlier
> (wrong) claim that the camera outputs raw **RGB24 / I420**. It does not. It
> outputs **baseline MJPEG**.

---

## 1. Why MJPEG

USB 1.1 full-speed cannot carry raw 320×240 video. Raw QVGA YUV422 at 30 fps is
~4.6 MB/s; full-speed tops out near 1 MB/s. The OV519 has an on-chip **JPEG engine**
(enabled via bridge `reg 0x54` bit 2) that compresses each frame to ~2.4–3.8 KB.
So the iso endpoint carries a stream of small JPEG frames — Motion JPEG.

Observed per-frame size on the bench: `jpegLen` ≈ 2896–3760 bytes. Each frame begins
`FF D8 FF E0` (JPEG SOI + JFIF APP0).

---

## 2. The iso buffer

- Endpoint `0x81`, alt 3 (320×240), **maxpkt 768**.
- Buffer = **8 iso packets × 768 = 6144 bytes**, contiguous
  (`MmAllocateContiguousMemory`).
- `USBD_ISOCH_BUFFER_DESCRIPTOR`:
  - `FrameCount = 8`
  - `TransferBuffer = s_isoBuf`
  - **`Pattern[8]` = `{maxpkt × 8}`** — bytes requested per iso frame. **If left 0,
    the controller transfers nothing.** This was a silent-stream bug; fill all 8.
  - `TransferComplete = Cam_IsoComplete` (must be **non-NULL** and **`__stdcall`** —
    the framework does not null-check it).

A typical completed buffer carried ~5300 of 6144 bytes of useful data.

---

## 3. Per-packet length — use `BytesRead`, never maxpkt

The completion receives `USBD_ISOCH_TRANSFER_STATUS`. Packet *i* sits at
`s_isoBuf + i*maxpkt`, but its **real length is `PacketStatus[i].BytesRead`** — a
**12-bit** field (`USBD_ISOCH_PACKET_STATUS_WORD { USHORT BytesRead:12;
USHORT ConditionCode:4; }`).

> Copying the full `maxpkt` instead of `BytesRead` drags stale tail bytes into the
> JPEG bitstream and **buries the EOI marker** — the decoder then fails or produces
> garbage. Using `BytesRead` is mandatory, not an optimization.

These three structs (`Pattern[8]`, `PacketStatus[8]`, `BytesRead:12`) are the load-
bearing iso definitions and live in the authoritative header `examples/xbox_usb.h`.

---

## 4. OV519 packet framing

Within each iso packet, the OV519 marks frame boundaries with a header:

```
byte[0..2] == 0xFF 0xFF 0xFF
byte[3]    == 0x50  -> START of frame (SOF)
byte[3]    == 0x51  -> END   of frame (EOF)
```

- **SOF packet:** reset the accumulator (`s_frameW=0`, `s_inFrame=1`), **strip the
  16-byte header**, append the remaining bytes.
- **EOF packet:** publish the accumulated frame (`s_frame` → `s_frameReady`,
  `s_completedFrames++`), clear `s_inFrame`.
- **Any other packet while `s_inFrame`:** append its `BytesRead` bytes verbatim.

A frame therefore spans roughly 25 iso buffers. `s_frameReady` holds the latest
complete JPEG; the draw path decodes it only when `s_completedFrames` changes.

(Framing follows gspca `ov519.c` `ov519_pkt_scan`; the 16-byte SOF header and the
`0x50`/`0x51` discriminator are confirmed against the live stream.)

---

## 5. The JPEG payload (picojpeg)

picojpeg is integer-only (no malloc / float / libc) → RXDK-safe. Observed image
properties from the live decode:

```
width 320  height 240  comps 3  scanType 2 (YH2V1)
MCUSPerRow 20  MCUSPerCol 30   (MCU = 16x8)
```

- **scanType 2 = YH2V1:** MCU is 16×8 px = two horizontal 8×8 luma blocks. Block
  offsets into picojpeg's MCU buffers for `nbx=2, nby=1` are `{0, 64}`.
- The decoder feeds bytes through `Cam_JpgFeed` (a cursor over `s_jpegBuf`).
- **Output is BGRA** to match `D3DFMT_A8R8G8B8` byte order, with **R and B
  swapped** in the write to correct the camera's channel order (the blue-cast fix).
  Grayscale (`comps==1`) writes `B=G=R=Y`.

Decoded luma sanity on the bench (proving a real image well before display worked):
`luma min 34, max 252, avg 149, center 199` on a bright frame.

---

## 6. Frame lifecycle summary

```
iso completion (per 6144-byte buffer, 8 packets):
  for each packet p:
    avail = PacketStatus[p].BytesRead        ; real length (12-bit)
    if FFFFFF + 0x50  -> new frame, strip 16B header, start accumulating
    if FFFFFF + 0x51  -> publish s_frame -> s_frameReady, frames++
    else if in-frame  -> append avail bytes
  re-arm the attach URB (keep streaming)

draw (per present, decode only on new frame):
  if completedFrames changed:
    copy s_frameReady -> s_jpegBuf
    picojpeg decode -> s_rgb (BGRA, 512x256 buffer, image in top-left 320x240)
  XGSwizzleRect(s_rgb, 512*4, NULL, texBits, 512, 256, 0, 4)   ; font path
  draw textured quad (A8R8G8B8, no YUVENABLE)
```

## References
- gspca Linux `ov519.c` (`ov519_pkt_scan`, the SOF/EOF header).
- picojpeg (Rich Geldreich) — baseline JPEG, integer-only.
- `examples/xbox_usb.h` — `USBD_ISOCH_*` struct definitions (authoritative).
- `WORKING_IMPLEMENTATION.md` §5–6.
