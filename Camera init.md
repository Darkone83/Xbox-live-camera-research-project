# Camera init — findings & bring-up plan

**Team Resurgent / Darkone83.** How the Video Chat driver actually brings up the
Sony EyeToy, and how that maps to a homebrew driver. **Major revision:** earlier
passes assumed we'd replay the OV519/OV7648 `.set` register tables. Reading the
camera class driver end to end shows it **does not** — see the verdict below.
Companion to `USB_TRANSPORT.md` (the full transport + dispatch map).

---

## Verdict: the EyeToy is a standard USB iso-video device — no register replay

The camera class driver (`FUN_000C…`) was traced through all its command paths.
**None of them write OV519/OV7648 registers.** Evidence:

| Path | Handler | What it does | Registers? |
|------|---------|--------------|------------|
| SET format `0x102` | `FUN_000ce5c0` → `FUN_000ce0a0` | validates a format **index**, flushes pipe ops, drains transfers (`FUN_001be9b0`), clears the slot | **none** |
| STREAM ctrl | `FUN_000cec70` → `FUN_000cdef0` | selects a USB **alt-setting** by mode index (descriptor table, stride `0x14`), opens the iso pipe | **none** |
| START stream `0x107` | `FUN_000cdd80` → `FUN_001bfd30` | builds an iso stream object (`'DEVA'` magic), `KeInitializeSemaphore`, iso scheduling | **none** |

Every leaf call is XAPILIB transport (`0x1Bxxxx`). There is **no** vendor SETUP
construction, **no** SCCB write path, **no** OV519 register constants (`0x12=0x80`
reset, addr `0x42`, etc.) anywhere in format-set or streaming.

**Conclusion:** the driver treats the EyeToy as a standard USB iso-video device.
Formats live in a **format-index table populated from the device's descriptors**
during enumeration; selection is by index → **USB alt-setting (`SET_INTERFACE`)**;
frames arrive over an **isochronous endpoint**. The OV519 bridge handles the
sensor internally. So the homebrew is a **standard USB iso client, not a custom
register-init driver** — the single biggest de-risking of the project.

**Two small confirmations still outstanding** (don't change the model, just make
it airtight):
1. The open/detect path `FUN_001baa80` — glance for any vendor command after the
   GET_DESCRIPTOR + VID/PID check (expected: none).
2. The `0x10c` "set mode" command (issued by `FUN_001ba8f0`) hit `default` in the
   `FUN_000cf340` switch — find where it's handled (expected: another
   descriptor/alt-setting selector).

---

## The camera command model (from `FUN_000cf340`, the main dispatch)

`FUN_001bdfa0` marshals each command into a `'USBD'` packet and calls
`(*DAT_0022198c)` = **`FUN_000cf340`**, which switches on `packet+4`:

```
0x100  buffer setup   → FUN_000ce510      0x109  status?      → FUN_000ce490
0x101  GET format     → FUN_000cf120      0x10a  special      → FUN_000cf040
0x102  SET format     → FUN_000ce5c0      0x10b/0x10e         → FUN_001bf850 + FUN_001bf660
0x107  START stream   → FUN_000cdd80      0x10d               → FUN_001bef70
0x108  STOP stream    → FUN_001bfad0
```
(Commands `0` grab-frame and `3` stream-state route to the separate
`DAT_00221AF8`/`DAT_00221AFC` pointers, not this switch.)

---

## The `.set` files (PC-driver reference — likely NOT needed for Xbox)

These are `usbcamd` INF-style register tables from the **PC** OV519 driver. They
are the PC approach to bring-up; the **Xbox driver does not use them** (verdict
above). Kept as reference + fallback, and the detection block is still a handy
sanity check if we ever talk to the sensor directly.

| File | Sensor + bridge | |
|------|-----------------|---|
| `7648519.set` | **OV7648 + OV519** | **= Sony EyeToy (SLEH-00030). Primary target.** |
| `7620519` / `7620e519` / `7635519` / `7640519` / `6630f519` | other OV sensors + OV519 | share the OV519 bridge |

- `UsbSetting` → OV519 bridge regs (vendor control). `CameraSetting` → OV7648
  sensor regs (SCCB via bridge, addr `0x42`). Entry = `index, value, mask`
  (`mask==0xFF` write; `mask<0xFF` read-modify-write).
- Detection (still potentially useful): sensor `0x0A==0x76`, `0x0B==0x48` → OV7648;
  reset `0x12=0x80`.

`ov7648_519.h` holds these tables verbatim. **Status: reference only** unless the
two outstanding confirmations turn up a register path after all.

---

## Format / harness correction (still valid)

The EyeToy delivers **RGB24** or **I420**, **not YUY2**. Modes: 160×120, 176×144,
**320×240**, 352×288, 640×480, each RGB24 + I420; driver default
(`CurrentFormat 0x32024024`) = **320×240 RGB24**.

When frames are real, the cameratest preview must create the texture as
`D3DFMT_X8R8G8B8` (or `LIN_X8R8G8B8`) and expand RGB24→32bpp per row — **not** the
current YUY2 path. (I420 needs a YUV→RGB convert; NV2A YUY2 is packed 4:2:2, not
planar 4:2:0.) The reference loop `FUN_00018E00` does exactly this: `LockRect` a
per-slot texture, convert via per-format callbacks, flip a double buffer.

---

## Bring-up plan (revised — standard USB iso client)

```
Phase 0  Register the device + bring-up  (via XInitDevices — NOT custom enum)
  - The camera rides XInitDevices like a controller: a device-type descriptor
    (callbacks INIT/CONNECT/DISCONNECT) is registered, XAPILIB enumerates the
    device and creates the pipes, and fires the CONNECT callback with a port#.
  - Work item: register the camera device-type descriptor with XInitDevices in
    RXDK (or confirm RXDK's XAPILIB already carries it). See USB_TRANSPORT.md §11.
  - No port reset / SET_ADDRESS / SET_CONFIGURATION of our own — XAPILIB does it.

Phase A  Prove one control transfer
  - GET_DESCRIPTOR (device) -> VID/PID. Confirms host hookup + URB layout.
  - VID/PID: 054C:0155 (EyeToy) or 045E:028C (Xbox Cam).

Phase B  Select format
  - Pick the 320x240 RGB24 entry by index (NO register replay).
  - SET_INTERFACE to the streaming alt-setting; configure the iso endpoint
    (max-packet from the interface descriptor, see FUN_000cdef0).

Phase C  Stream
  - Start iso (mirror FUN_000cdd80 -> FUN_001bfd30): build iso stream object,
    schedule iso IN transfers on the video EP, assemble RGB24 frames.
  - Hand each finished frame to XCam_PublishFrame() (already wired).

Phase D  Display
  - Preview texture -> X8R8G8B8; expand RGB24 -> 32bpp.
```

The register-level work that used to be "Phase C init" is **gone** — there's no
register table to apply. The risk is now concentrated entirely in Phase 0
(transport / enumeration).

---

## Still open (the honest list)

**Understanding (~98% — these are the last reads):**
1. `FUN_000cefc0 → FUN_000ced10` — the dispatch-install link (verify in Ghidra).
2. `FUN_000cf340` default case — confirm `0x10c`/`0x106` fill a caps blob, not regs.
3. `DAT_00221af8`/`DAT_00221afc` install site — grab/stream handler pointers
   (data write-XREF on `0x00221AF8`).
4. Setup cluster `FUN_001b52a5/AB/B1/B7` — config-descriptor read + pipe creation
   (the `req+0x10` source) + format/alt-setting table fill.

**Build-prep:**
5. Iso URB layout — `FUN_001bfd30` + iso branch of `FUN_001b84f0`.
6. Frame assembly — iso packets → 320×240 RGB24.

**Hardware-only:** does the XInitDevices descriptor trigger on RXDK; first control
transfer round-trip; iso scheduling; stability. These don't close by reading.

---

## Debugging & instrumentation (build this FIRST when we resume)

Target is a **modded retail unit, no devkit / no SuperIO** — no live kernel debug,
no xbdm `Dm*` tools. Two channels instead:

- **CerBios fatal-error screen** (LCD):
  ```
  FatalError:  <CerBios code>
  BugCheck     <NT code>      e.g. 0000001E = KMODE_EXCEPTION_NOT_HANDLED
  <Arg1> <Arg2>               Arg1 = exception (C0000005 = access violation)
  <Arg3> <Arg4>               Arg2 = EIP (faulting addr -> function via .MAP)
                              Arg3 = 0 read / 1 write   Arg4 = faulting data addr
  ```
  A low Arg4 = null/garbage pointer + struct offset — the classic bad
  device-object / URB pointer signature. Arg2 (EIP) is the gold.
- **Verbose synchronous log to one deletable file**, **flushed every checkpoint**
  (a bugcheck resets the box; buffered logs lose the line that matters). Keep it
  logical (phase, device, endpoint, sent, expected vs got).
- **Build:** emit + keep the linker **`.MAP`** (MSVC `/MAP`); XBE load base is
  fixed, so an EIP resolves statically to a function. Write a small EIP→`.map`
  resolver alongside the logger.

---

## Resume checkpoint

- **Architecture: traced end to end, ~98%.** `XInitDevices` + device-type
  descriptor (`0x1b295c`) → INIT/CONNECT/DISCONNECT callbacks
  (`FUN_001ba120`/`190`/`240`) → camera-driver dispatch `FUN_000cf340` →
  register-free handlers → iso streaming → display. Full map + lifecycle in
  `USB_TRANSPORT.md` (§0, §4.5, §11).
- **Bring-up: rides `XInitDevices`** — no enumeration of our own. XAPILIB creates
  the device + pipe objects; we register a descriptor and get a port-number
  callback.
- **Verdict: standard USB iso-video client, no register replay.** `.set` tables
  (`ov7648_519.h`) are reference-only.
- **Last reads to hit 100%:** `cefc0→ced10`, `cf340` default, `af8/afc` install,
  the `52xx` setup cluster (see Still-open). Then iso URB layout + frame assembly
  for the build. The rest closes only on hardware.
- **First milestone unchanged:** one control transfer — read sensor `0x0A`/`0x0B`,
  expect `0x76`/`0x48` (recipe in `USB_TRANSPORT.md`). Needs the pipe object,
  which XAPILIB creates during the connect callback.

## References

- OmniVision **OV7648** datasheet; **OV519** datasheet / Linux `gspca_ov519`
  (reference only now — Xbox path is descriptor/alt-setting driven).
- USB 1.1 / OHCI (isochronous IN, alternate settings, iso scheduling).
- `USB_TRANSPORT.md` — the master architecture map.
- `RESEARCH.md` → *Cross-Reference Corrections*.