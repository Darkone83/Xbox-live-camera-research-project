# OV519 BRIDGE + OV7648 SENSOR — register init reference
## Team Resurgent / Darkone83

> **STATUS: HARDWARE-VERIFIED.** This is the register sequence the working driver
> (`examples/xb_cam.cpp`, `Cam_InitSensor`) actually applies to bring a Sony EyeToy
> from cold to a streaming 320×240 MJPEG source. It replaces the project's earlier
> claim that "the OV519 self-configures and no register replay is needed" — that was
> false. The gspca Linux `ov519` driver and the `.set` register tables (this repo's
> `set/`) were the correct reference all along; they are **primary**, not "reference
> only."

The single most important line in this whole file: **`reg 0x72 = 0xEE`** in the
bridge bring-up. With bit 4 (0x10) set (the hardware default `0xFF`), the sensor is
invisible on SCCB and detection never succeeds. Clearing it is the make-or-break.

---

## 1. Transport primitives

### OV519 bridge (vendor control on EP0)
| Op | bmRequestType | bRequest | wValue | wIndex | data |
|---|---|---|---|---|---|
| write reg | `0x41` | `0x01` | `0x0000` | reg | 1 byte = value |
| read reg  | `0xC1` | `0x01` | `0x0000` | reg | 1 byte ← value |

`Cam_OvW(reg,val)`, `Cam_OvR(reg,&out)`. Masked write `Cam_OvWMask(reg,val,mask)` =
read-modify-write unless `mask==0xFF`.

### OV7648 sensor (SCCB / I2C through the bridge)
- **Slave ids:** `OvW(0x41, 0x42)` = write slave (W_SID), `OvW(0x44, 0x43)` = read
  slave (R_SID).
- **Write** `Cam_I2cW(reg,val)`: `OvW(0x42,reg)` (sub-address) → `OvW(0x45,val)`
  (data) → `OvW(0x47,0x01)` (initiate) → `Sleep(1)`.
- **Read** `Cam_I2cR(reg,&out)`: `OvW(0x43,reg)` → `OvW(0x47,0x03)` → poll `0x47`
  bit0 → `OvW(0x47,0x05)` → poll `0x47` bit0 → `OvR(0x45,&out)`, then cleanup
  (`OvW(0x42,0xFF); OvW(0x45,0); OvW(0x40,0x01)`).
- Masked sensor write `Cam_I2cWMask` = RMW via `Cam_I2cR`/`Cam_I2cW`.

---

## 2. `init_519` — generic bridge bring-up (the OV519 connect path)

Applied first. The `.set` `UsbSetting` table deliberately starts later (at `0x5d`);
this generic prologue is from gspca and is required for detection.

```
OvW 0x5a 0x6d   ; EnableSystem
OvW 0x53 0x9b   ; do NOT enable the microcontroller
OvW 0x54 0xff   ; EN_CLK1 (bit2 = JPEG engine enable)
OvW 0x5d 0x03   ; leave suspend mode
OvW 0x49 0x01
OvW 0x48 0x00
OvW 0x72 0xee   ; *** GPIO_IO_CTRL0: bit4 CLEAR — REQUIRED for sensor detect ***
OvW 0x51 0x0f   ; RESET1 assert
OvW 0x51 0x00   ; RESET1 release
OvW 0x22 0x00
Sleep 10
```

---

## 3. `init_ov_sensor` — slave id + reset + detect

```
OvW 0x41 0x42            ; W_SID
OvW 0x44 0x43            ; R_SID
I2cW 0x12 0x80           ; COM7 sensor reset
Sleep 150
loop up to 5:
    I2cR 0x1c -> idh     ; manufacturer id high  (expect 0x7F)
    I2cR 0x1d -> idl     ; manufacturer id low   (expect 0xA2)
    if idh==0x7F && idl==0xA2: SYNCED, break
    I2cW 0x12 0x80 ; Sleep 150 ; I2cR 0x00 (dummy, resync I2C)
```

Bench result: synced on the first try (`0x7F / 0xA2`).

---

## 4. Sensor identification

```
I2cR 0x0a -> pidh        ; OV7648 reads 0x76
I2cR 0x0b -> pidl        ; OV7648 reads 0x48
sensor76 = (pidh == 0x76)
```

`sensor76` selects the OV7648 minimal path (§6). A non-76xx sensor takes the full
`norm_7620` table instead (§7) — retained for completeness, not the EyeToy path.

---

## 5. Bridge mode-init (`ov519_mode_init_regs`)

YUV input path, I2C timing, audio clock, FRAR:

```
OvW 0x5d 0x03   OvW 0x53 0x9f   OvW 0x54 0x0f
OvW 0xa2 0x20   OvW 0xa3 0x18   OvW 0xa4 0x04   OvW 0xa5 0x28
OvW 0x37 0x00   OvW 0x55 0x02   ; 4.096 MHz audio clock
OvW 0x22 0x1d   OvW 0x17 0x50
OvW 0x40 0xff   ; I2C timeout counter
OvW 0x46 0x00   ; I2C clock prescaler
OvW 0x59 0x04   OvW 0xff 0x00
```

---

## 6. OV7648 sensor init (the EyeToy path) — `mode_init_ov_sensor_regs` + window

The `.set` `CameraSetting` table **alone is insufficient**; this is the gspca path.

```
; reset, then 8-bit input select
I2cW 0x12 0x80 ; Sleep 150
I2cW 0x12 0x14
OvWMask 0x20 0x10 0x10        ; OV519_R20_DFR: 8-bit input mode

; mode/AWB
I2cWMask 0x14 0x20 0x20       ; QVGA
I2cWMask 0x28 0x00 0x20
I2cWMask 0x2d 0x40 0x40       ; anti-shake (undocumented)
I2cWMask 0x67 0xf0 0xf0
I2cWMask 0x74 0x20 0x20       ; higher auto gain
I2cWMask 0x12 0x04 0x04       ; AWB on
I2cW    0x11 0x00             ; clockdiv 0 -> 30 fps

; set_ov_sensor_window (QVGA): hwsbase 0x1a, vwsbase 0x03, hwscale 1, vwscale 0
I2cW 0x17 0x1a               ; HSTART
I2cW 0x18 0xba               ; HSTOP = 0x1a + 320/2
I2cW 0x19 0x03               ; VSTART
I2cW 0x1a 0xf3               ; VSTOP  = 0x03 + 240
```

> **Window warning (copied from the source comment because it was literally true):**
> if the window is wrong you get **ALL-ZERO isoc data**. That was the exact symptom
> before these four writes were added.

---

## 7. `norm_7620` — full OV7620 table (non-EyeToy fallback)

Applied only when `sensor76 == 0`. 60-odd `Index,Value` pairs written over SCCB; a
`0x12=0x80` entry triggers a `Sleep(150)`. Present in `Cam_InitSensor` verbatim;
not used for the OV7648 EyeToy. Kept so the driver also handles OV7620-class bridges.

---

## 8. Bridge frame geometry + rate + restart

```
; geometry: 320x240, YUV422
OvW 0x10 0x14   ; H_SIZE = 320>>4
OvW 0x11 0x1e   ; V_SIZE = 240>>3
OvW 0x12 0x00   OvW 0x13 0x00   OvW 0x14 0x00   OvW 0x15 0x00   OvW 0x16 0x00
OvW 0x25 0x03   ; FORMAT = YUV422
OvW 0x26 0x00

; frame rate (OV7648, 30 fps)
OvW 0xa4 0x0c   OvW 0x23 0xff

; ov51x_restart: unblock the stream FIFO, then LED on
OvW 0x51 0x0f   OvW 0x51 0x00
OvW 0x22 0x1d              ; FRAR
OvWMask 0x71 0x01 0x01     ; LED on (GPIO_DATA_OUT0 bit0)
```

Post-init verification re-reads `0x1c`/`0x1d` (expect `0x7F`/`0xA2`) to confirm the
sensor is still alive after the full sequence.

---

## 9. The `.set` files (`set/`) — what they are, now promoted to primary

`usbcamd` INF-style register tables from the PC OV519 driver. `UsbSetting` =
OV519 bridge regs; `CameraSetting` = sensor regs over SCCB (slave `0x42`). Entry =
`index, value, mask` (mask `0xFF` = write, `< 0xFF` = read-modify-write).

| File | Sensor + bridge |
|---|---|
| `7648519.set` | **OV7648 + OV519 = Sony EyeToy. Primary target.** |
| `7620519` / `7620e519` / `7635519` / `7640519` / `6630f519` | other OV sensors on OV519 |

Detection constants (used and confirmed): sensor `0x0A==0x76`, `0x0B==0x48` →
OV7648; `0x12=0x80` = reset. These tables, together with gspca `ov519.c`, supplied
the register values above. **They are not "reference only" — they are the source of
the working init.**

## References
- gspca Linux `ov519` driver (`init_519`, `init_ov_sensor`,
  `ov519_mode_init_regs`, `mode_init_ov_sensor_regs`, `set_ov_sensor_window`,
  `ov51x_restart`, `norm_7620`).
- OmniVision OV7648 / OV519 register usage.
- `set/7648519.set` (this repo).
- `WORKING_IMPLEMENTATION.md` §4 (how this fits the whole pipeline).
