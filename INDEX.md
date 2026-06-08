# Document index
**Team Resurgent / Darkone83**

The driver works on hardware, so this set is split between the docs that describe the
*working* driver and the older reverse-engineering log that got us there. If you only
read one thing, read `WORKING_IMPLEMENTATION.md`. Where anything here disagrees with
it, it wins — it's the one that matches what actually runs.

## Start here — how the driver actually works

| Doc | What it covers |
|---|---|
| `WORKING_IMPLEMENTATION.md` | The end-to-end pipeline as shipped. The source of truth. |
| `OV519_OV7648_INIT.md` | The exact OV519 bridge + OV7648 sensor register sequence. |
| `MJPEG_FRAME_FORMAT.md` | OV519 iso packet framing, the MJPEG payload, and the decode. |
| `summary.md` | Short overview of what the code does. |
| `README.md` | App/build overview and the file list. |

## Reference — hardware facts, independent of any driver model

| Doc | What it covers |
|---|---|
| `EEPROM Descriptor.md` | The first-party Xbox Video Camera's USB descriptor, parsed from its EEPROM. |
| `Camera init.md` | The sensor-detection recipe, the `.set` map, and the debugging method that carried the bring-up. |
| `bugcheck reference.md` | BugCheck codes and the debugging workflow. The most reusable doc here for any USB/iso work. |

## Historical — the RE log (kept for the trail, not as instructions)

| Doc | What it covers |
|---|---|
| `RESEARCH.md` | The investigation log. Two driver models in it were later disproven; it's banner-flagged and kept honest. |
| `USB Transport.md` | An accurate trace of how the *retail* Video Chat XBE drives the camera — not how this homebrew driver works. |

## Source and headers

The shipped driver and harness live in `src/Camera-test/`:

- `xb_cam.cpp`, `main.cpp`, `xb_cam.h` — the driver and the test harness.
- `xbox_usb.h` — the authoritative USB header (see below).
- `set/*.set` — the OV register tables. `7648519.set` is the EyeToy.

## Why `xbox_usb.h` is authoritative

Every USB struct layout, the `_URB` union, the `USB_BUILD_*` macros, the `USBD_STATUS`
table, and the `IUsbDevice`/`IUsbInit` interfaces are defined in `xbox_usb.h`, which
pulls in nothing but `<xtl.h>`. When a reverse-engineering trace's raw byte offsets
disagree with this header, trust the header — and that's not just a style rule anymore,
it's been proven on hardware.

The three iso definitions the working stream depends on are all in there, with the
exact fields that made it work:

- `USBD_ISOCH_BUFFER_DESCRIPTOR.Pattern[8]` — the per-frame byte request; has to be filled.
- `USBD_ISOCH_PACKET_STATUS_WORD { BytesRead:12; ConditionCode:4; }` — the real per-packet length.
- `USBD_ISOCH_TRANSFER_STATUS.PacketStatus[8]`.

Every USB symbol `xb_cam.cpp` touches resolves inside `xbox_usb.h`; nothing in the
working driver leans on the external XDK `usb.h`. Keep `src/Camera-test/xbox_usb.h` as
the single canonical copy and don't let a divergent duplicate creep in.
