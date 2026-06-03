# Camera Research — Document Index & Status
## Team Resurgent / Darkone83

**The driver works on hardware.** This index is the map of the revised research set
after that milestone. Read top-to-bottom for the working truth; the historical RE is
kept but clearly marked.

## Authoritative (the working truth)
| Doc | What |
|---|---|
| `WORKING_IMPLEMENTATION.md` | The proven end-to-end pipeline. **Start here.** Where anything else disagrees, this wins. |
| `OV519_OV7648_INIT.md` | The exact OV519 bridge + OV7648 sensor register sequence. |
| `MJPEG_FRAME_FORMAT.md` | OV519 iso packet framing + the MJPEG/JPEG payload + decode. |
| `summary.md` | Executive summary (rewritten to the working model). |
| `README.md` | App/build overview (rewritten; lists the shipped files). |

## Reference / hardware facts (kept, thesis-independent)
| Doc | What |
|---|---|
| `bugcheck reference.md` | BugCheck codes + debugging workflow. Unchanged. |
| `EEPROM Descriptor.md` | First-party Xbox Cam descriptor parse (corrected interpretive notes). |
| `Camera init.md` | Revised: corrected verdict + the detection recipe, `.set` map, and instrumentation method that proved correct. |

## Historical RE of the retail XBE (reference, NOT the implemented path)
| Doc | What |
|---|---|
| `RESEARCH.md` | The investigation log. Status-bannered: two models in it are superseded. |
| `USB Transport.md` | Accurate trace of how the retail Video Chat XBE works — not how the homebrew driver works. |

## Archived (approach not taken)
| Doc | Why archived |
|---|---|
| `archive/class driver guide.md` | The class-driver/`XInitDevices` attach model — never shipped (camera isn't system-enumerated). |
| `archive/build_spec.md` | Build plan for that model. Symbol maps/struct provenance may still help; the strategy is dead. |

## Source & headers
- `src/Camera-Test/xb_cam.cpp`, `src/Camera-Test/main.cpp`, `src/Camera-Test/xb_cam.h` — the shipped driver/harness.
- `src/Camera-Test/xbox_usb.h` — **the master authoritative USB header** (see below).
- `set/*.set` — OV register tables (now primary; `7648519.set` = EyeToy).

---

## xbox_usb.h is the master authoritative USB header

All USB struct layouts, the `_URB` union, the `USB_BUILD_*` macros, the
`USBD_STATUS` table, and the `IUsbDevice`/`IUsbInit` interfaces are defined in
**`xbox_usb.h`**. It includes only `<xtl.h>`. Every other doc that quotes a struct or
offset defers to it; where a reverse-engineering trace's raw byte offsets disagree
with the header, **the header wins** — and that rule is now proven on hardware, not
just asserted.

Verified against the shipped driver: `xbox_usb.h` already contains the three iso
definitions the working code depends on, with the exact fields that made streaming
work —
- `USBD_ISOCH_BUFFER_DESCRIPTOR.Pattern[8]` (per-frame byte request; must be filled),
- `USBD_ISOCH_PACKET_STATUS_WORD { BytesRead:12; ConditionCode:4; }` (real per-packet
  length), and
- `USBD_ISOCH_TRANSFER_STATUS.PacketStatus[8]`.

Every USB symbol referenced by `xb_cam.cpp` resolves inside `xbox_usb.h`; nothing in
the working driver depends on the external XDK `usb.h`. No struct edits are required
to make it authoritative — it already is. Keep `src/Camera-Test/xbox_usb.h` as the single
canonical copy and delete/avoid any divergent duplicates.
