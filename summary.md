# Original Xbox Camera Driver — Findings & Working Implementation
## Team Resurgent / Darkone83

> **STATUS: WORKING ON HARDWARE.** A Sony EyeToy streams a live, recognizable
> 320×240 image on a retail original Xbox. This summary reflects the **shipped,
> proven** design. The authoritative deep-dive is `WORKING_IMPLEMENTATION.md`;
> register detail is in `OV519_OV7648_INIT.md`; the wire format is in
> `MJPEG_FRAME_FORMAT.md`; the authoritative USB header is `examples/xbox_usb.h`.

> **This document was rewritten.** Its previous version asserted the camera was a
> "standard USB iso-video device, no register replay, rides XInitDevices, outputs
> RGB24." Every one of those claims was disproven on hardware. The history of how
> the project reached the wrong conclusions (and corrected them) lives in
> `RESEARCH.md`; this file now describes only what is true.

---

## 1. Executive summary

The original Xbox camera **is** driven the way PC drivers drive it: a real
OV519 bridge + OV7648 sensor register bring-up, with frames arriving as **MJPEG**
over an isochronous endpoint. Two surprises defined the project:

1. **The camera is not enumerated by the system.** It is not a node in
   `g_DeviceTree`; the class-driver/`XInitDevices` attach path never fires for it.
   The driver therefore performs **manual USB enumeration** — hub port scan, port
   reset, own device node, `SET_ADDRESS` / `SET_CONFIGURATION`, descriptor parse.
2. **Register replay is required, not avoidable.** The OV519 does **not**
   self-configure. The gspca `ov519` init sequence (and the `.set` tables) are the
   core of bring-up; the one make-or-break write is `reg 0x72 = 0xEE`.

Output is **baseline MJPEG** (~3 KB/frame), reassembled from OV519-delimited iso
packets and decoded in software (picojpeg) to BGRA for display.

---

## 2. Hardware facts (confirmed on the bench)

- **EyeToy:** OmniVision **OV7648** sensor + **OV519** bridge. Enumerated VID/PID at
  address 0 = **`0x054C / 0x0155`**. (Live Vision / Xbox Cam = `0x045E / 0x028C`,
  OV530, OV519-compatible — same driver path.)
- **Sensor IDs over SCCB:** MID `0x1C=0x7F`, `0x1D=0xA2`; PID `0x0A=0x76`,
  `0x0B=0x48` → OV7648.
- **Bus/topology:** OHCI, USB 1.1 full-speed. Camera hangs off the internal **TI
  hub**, on a port that is *connected but not enabled* until reset. Iso IN endpoint
  **`0x81`**, alt 3 = 320×240, **maxpkt 768**, iso buffer 8×768 = 6144 bytes.
- **Wire format:** MJPEG (JPEG SOI `FF D8 FF E0`), **not** RGB24/I420/YUY2.

---

## 3. The working pipeline (one line per stage)

1. **Walk** `g_DeviceTree`; find the TI hub.
2. **Scan hub ports**; the connected-but-not-enabled port is the camera.
3. **Reset** that port → device live at address 0.
4. **Own a node** (`g_DeviceTree.AllocDevice`, state `0xFE` fresh-open), open EP0.
5. **Identify** via `GET_DESCRIPTOR`; `SET_ADDRESS` (framework-allocated) + reopen
   EP0; `SET_CONFIGURATION(1)`.
6. **Find the iso IN endpoint** in the config descriptor (contiguous DMA buffer,
   shrink-and-retry within the control-TD pool).
7. **Bring up OV519 + OV7648** (`init_519` with `0x72=0xEE`, `init_ov_sensor`,
   OV7648 QVGA + window, geometry, `ov51x_restart`, LED on).
8. **`SET_INTERFACE`(alt 3)** → `ISOCH_OPEN/ATTACH/START`; fill `Pattern[8]=maxpkt`.
9. **Assemble MJPEG** from iso packets using `PacketStatus[i].BytesRead` and the
   OV519 `0x50`/`0x51` SOF/EOF framing (strip the 16-byte SOF header).
10. **Decode** (picojpeg → BGRA) and **display** via swizzled `A8R8G8B8` +
    `XGSwizzleRect` (the proven `font.cpp` path; no `YUVENABLE`).

---

## 4. Why the earlier model was wrong (kept as a warning)

| Old claim | Reality |
|---|---|
| Standard iso-video device, no register replay | Extensive OV519+OV7648 register init required; `0x72=0xEE` was the wall |
| `.set` tables not used on Xbox | `.set` + gspca data are the **source** of the working init |
| Rides `XInitDevices`; no enumeration of our own | Camera isn't a tree node; we enumerate manually |
| Match by interface class 0xFF | We own the node by hub-port reset + `AllocDevice`, not class match |
| Outputs RGB24/I420, default 320×240 RGB24 | Outputs **MJPEG**; decoded to BGRA |
| EyeToy is composite/needs reflash | Test unit presented single-video at `054C:0155` |

---

## 5. What this unlocks

The manual-enumeration technique (own a node the framework won't claim, drive it
over `IUsbDevice`/`SubmitRequest`) generalizes to **other non-enumerated USB devices
on the original Xbox**. The camera driver is the template; the iso + register-replay
pattern is reusable for other vendor-specific USB peripherals.

---

## 6. Document map

- `WORKING_IMPLEMENTATION.md` — authoritative pipeline (read this first).
- `OV519_OV7648_INIT.md` — the register sequence.
- `MJPEG_FRAME_FORMAT.md` — iso packet + JPEG format.
- `EEPROM Descriptor.md` — first-party Xbox Cam descriptor (hardware reference).
- `bugcheck reference.md` — debugging codes (kept; thesis-independent).
- `RESEARCH.md`, `USB Transport.md` — historical RE of the retail Video Chat XBE
  (reference; **not** the implemented path).
- `archive/` — the superseded class-driver model docs.
- `examples/` — the shipped source; `xbox_usb.h` is the authoritative USB header.
