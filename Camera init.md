# Camera init — bring-up notes
## Team Resurgent / Darkone83

An earlier version of this file carried the verdict that *"the EyeToy is a standard USB
iso-video device — no register replay; the OV519 handles the sensor internally; the
`.set` tables are reference-only."* All of that turned out to be wrong. On hardware the
driver replays a substantial OV519 + OV7648 register init, and the `.set`/gspca data was
exactly where it came from. The corrected, authoritative bring-up lives in
`WORKING_IMPLEMENTATION.md` and `OV519_OV7648_INIT.md`. This file is kept for the parts
that did hold up: the sensor detection recipe, the `.set` map, and the instrumentation
method that proved everything out.

---

## Corrected verdict: register replay IS required

The OV519 does **not** self-configure from its EEPROM into a streaming sensor. The
working driver issues:
- a generic OV519 bring-up (`init_519`) whose `reg 0x72 = 0xEE` (clear GPIO bit 4)
  is the difference between a detectable sensor and a dead SCCB bus;
- an OV7648 reset + detect over SCCB (slave `0x42`);
- the OV7648 QVGA mode regs **and** `set_ov_sensor_window` (without the window, the
  iso stream is all zeroes);
- bridge geometry + frame-rate + `ov51x_restart` + LED.

See `OV519_OV7648_INIT.md` for the exact sequence. The original "no register" claim
came from reading only the retail Video Chat XBE's *high-level* command dispatch
(format-index / alt-setting selection) and not finding SCCB writes there — but that
app relied on a minidriver layer that does the register work; the homebrew driver
has to do it itself.

---

## Sensor detection (this part was right, and we used it)

```
slave id 0x42 (write) / 0x43 (read)
I2cW 0x12 0x80   ; reset, Sleep 150
I2cR 0x1c -> 0x7F , 0x1d -> 0xA2   ; OmniVision manufacturer id
I2cR 0x0a -> 0x76 , 0x0b -> 0x48   ; OV7648
```

Confirmed on the bench exactly as written.

---

## The `.set` files (`set/`) — now PRIMARY, not reference-only

`usbcamd` INF-style register tables from the PC OV519 driver. `UsbSetting` =
OV519 bridge regs; `CameraSetting` = sensor regs over SCCB. Entry =
`index, value, mask` (`mask==0xFF` write; `< 0xFF` read-modify-write).

| File | Sensor + bridge |
|---|---|
| `7648519.set` | **OV7648 + OV519 = Sony EyeToy. Primary target.** |
| `7620519` / `7620e519` / `7635519` / `7640519` / `6630f519` | other OV sensors on OV519 |

Together with gspca `ov519.c` these tables supplied the working register values.

---

## Format reality (corrected)

The EyeToy delivers **MJPEG**, not RGB24/I420/YUY2. The display path is a swizzled
`D3DFMT_A8R8G8B8` texture filled via `XGSwizzleRect` (the proven `font.cpp` path,
**no `YUVENABLE`**) — not the X8R8G8B8/RGB24-expand path this file previously
prescribed. See `MJPEG_FRAME_FORMAT.md`.

---

## Debugging & instrumentation (proved essential — keep verbatim)

Target is a modded retail unit — no devkit, no Super I/O, no live kernel debug. Two
channels carried the entire bring-up:

- **Fatal-error screen (CerBios LCD):** BugCheck code (Arg1) + up to 4 args. For
  exception bugchecks the args carry the exception code, **faulting EIP**, and
  address. Resolve the EIP against the linker `.MAP`. (Full table:
  `bugcheck reference.md`.)
- **Verbose synchronous log to one deletable file (`D:\xb_cam.txt`), flushed every
  checkpoint** — a bugcheck resets the box, so buffered logs lose the line that
  matters. Open/append/close per line. Log phase, device, endpoint, sent vs got.
- **Build with `/MAP`** and keep an EIP→`.map` resolver. XBE load base is fixed, so
  an EIP resolves statically to a function.

This is the method that turned every crash and every empty stream into a specific,
fixable line. It is unchanged and recommended for any future device work.

## References
- `WORKING_IMPLEMENTATION.md` — the authoritative bring-up.
- `OV519_OV7648_INIT.md` — the register sequence.
- `MJPEG_FRAME_FORMAT.md` — the wire/JPEG format.
- gspca `ov519.c`; OmniVision OV7648 / OV519; `set/7648519.set`.
