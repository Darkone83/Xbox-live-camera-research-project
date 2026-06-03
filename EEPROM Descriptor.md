# Xbox Camera EEPROM — parsed descriptor reference

Source: `Xbox_Camera_EEPROM.bin` (512 bytes, 24x04 EEPROM on the Xbox Video Camera
mainboard). Parsed directly from the binary; matches the xboxdevwiki transcription.
This is the **authoritative** source for what our driver should expect the device to
report. (OV530 bridge, OV519-compatible, per the CAMERAMATE datasheet format.)

## Layout overview
```
0x000-0x01F : index ramp 00..1F (EEPROM self-test / address bytes, not data)
0x020-0x08D : OV519 CAMERAMATE config block (bridge reads this to BUILD the
              descriptors below; encodes the same bandwidths)
0x08E-0x0F8 : standard USB descriptors (device + config + 5 interface/EP pairs)
0x14C-0x1A B: string descriptors ("Microsoft", "Xbox Video Camera")
0x1AC-0x1FF : 0xFF padding
```

## Device descriptor (@ 0x8E) — 18 bytes
```
12 01 10 01 00 00 00 08 5e 04 8c 02 00 01 01 02 00 01
bLength=18  bcdUSB=1.10  bDeviceClass=0  bMaxPacketSize0=8
idVendor=0x045E  idProduct=0x028C  bcdDevice=1.00
iManufacturer=1  iProduct=2  iSerial=0  bNumConfigurations=1
```

## Configuration descriptor (@ 0xA0) — 9 bytes
```
09 02 59 00 01 01 00 80 fa
wTotalLength=0x59 (89)  bNumInterfaces=1  bConfigurationValue=1
bmAttributes=0x80 (bus powered)  MaxPower=0xFA*2 = 500 mA
```

### *** DEVICE/INTERFACE COUNT — the EyeToy compatibility crux ***
Byte **@0x00A4 = `0x01`** (the config descriptor's `bNumInterfaces`). The wiki labels
this "Number of USB devices/interfaces." **Xbox Cam = 1; Sony EyeToy = 3.**

This single byte is what the wiki means by "the hardware descriptor must match the
Xbox Video Camera descriptor of **1 USB device/video only**." The EyeToy is a
**composite device (3 interfaces)** in its PS2 role; Video Chat's original driver
hard-expects a **single** video interface. So a VID/PID software patch alone is NOT
enough for an EyeToy on the *original* app — its descriptor also has to present as
single-video. Two ways to satisfy this:
  1. **EEPROM reflash** the EyeToy: set this count to 1 and trim to the single
     video interface/endpoint layout.
  2. **Our custom driver tolerates it:** because WE control the attach logic, the
     camera class driver can bind only interface 0's video function and ignore the
     EyeToy's extra interfaces — no EEPROM change needed. (A real advantage of
     writing our own driver vs. running the patched original app.)

## Interface 0 — alt-setting table (the format/bandwidth selector)
All alts: interface 0, **bInterfaceClass=0xFF (Vendor Specific)**, subclass 0,
protocol 0, one iso IN endpoint **0x81 (EP1 IN)**, bInterval=1. The class driver
matches on **class 0xFF / subclass 0x00**; bandwidth is chosen by `SET_INTERFACE`:

| Alt | wMaxPacketSize | Use |
|----|----|----|
| 0  | 0    | idle / zero-bandwidth (default; SET_INTERFACE 0 = stop) |
| 1  | 384  | streaming |
| 2  | 512  | streaming |
| 3  | 768  | streaming |
| 4  | 896  | streaming |

(Per-frame bytes at USB 1.1 full-speed = wMaxPacketSize x 1000 frames/s, so
alt4 ~= 896 KB/s raw iso bandwidth.)

## Strings
- iManufacturer(1) @ 0x152: "Microsoft"
- iProduct(2)      @ 0x172: "Xbox Video Camera"
- (langid 0x0409)

## OV519 CAMERAMATE config block (0x20-0x8D) — NEW (not in the wiki summary)
This is the OV519/OV530 bridge's own EEPROM configuration table, read at power-on to
generate the USB descriptors above. It is NOT something our Xbox driver issues — the
bridge consumes it internally. It matters for ONE reason: **this block is what an
EyeToy's EEPROM must effectively present for it to enumerate as a single-video,
Xbox-compatible device** (the wiki's "hardware descriptor must match" requirement
lives here). It encodes the same bandwidths (0x180=384, 0x200=512, 0x300=768,
0x380=896) seen in the alt-settings.

```
0x20: 00 6a 00 59 00 12 04 00 c6 22 00 ca 32 00 ec 00
0x30: 01 1e 00 01 1e 00 01 1e 00 01 1e 00 01 1e 00 00
0x40: 00 10 00 00 11 00 18 00 80 01 11 01 1b 00 80 01
0x50: 11 02 1c 00 80 01 11 03 1e 00 80 01 11 04 1f 00
0x60: 80 01 21 21 18 50 80 02 31 00 28 80 80 03 41 30
0x70: 28 80 80 04 51 30 20 80 80 05 61 30 38 04 80 06
0x80: 71 00 38 08 80 07 81 00 18 08 80 08 01 1e 12 01
```

## Driver implications (confirmed, no longer [V])
1. **Class-driver match key = class 0xFF, subclass 0x00.** Register the camera class
   driver descriptor with these bytes.
2. **Iso endpoint is 0x81 (EP1 IN)** — open this for streaming.
3. **Start streaming = SET_INTERFACE(iface0, alt N>=1); stop = alt 0.** Pick alt by
   the bandwidth/frame size you need (alt 4 = 896 for the largest).
4. **VID/PID 0x045E/0x028C** for the Xbox Cam; EyeToy is 0x054C/0x0155.
5. **Interface count: Xbox Cam=1, EyeToy=3.** The EyeToy is composite (3 interfaces);
   our driver should bind interface 0 (video) and ignore the rest, so no EyeToy
   EEPROM reflash is required (unlike the patched original app, which expects count=1).

---

## CORRECTIONS (added after hardware success)

1. **The EyeToy did not need to present as single-video.** The "bNumInterfaces must
   be 1 / EyeToy is composite-3 / reflash or bind-interface-0" framing was written
   for the *retail* Video Chat app. Our test EyeToy enumerated at address 0 as
   `054C:0155` (the video-only ID) with a config we parsed directly, and streamed
   without any EEPROM change. Whether a fully stock unit (possibly `0154`) behaves
   the same is untested.

2. **Class 0xFF is not our match key.** The shipped driver does not register a
   `{class,subclass,Register,Attach}` descriptor and let the core stack attach. The
   camera is not enumerated by the system, so that path never fires. We instead walk
   `g_DeviceTree`, find the TI hub, reset the connected-but-not-enabled port, and
   `g_DeviceTree.AllocDevice()` our own node. See `WORKING_IMPLEMENTATION.md` §2.

3. **What held:** start streaming by `SET_INTERFACE(iface0, alt N)` and stop with
   alt 0; iso IN endpoint `0x81`. The shipped driver pins alt 3 (320×240, maxpkt
   768). The bandwidth/alt table below is accurate for the first-party descriptor.

4. **Format:** the device streams MJPEG over the iso endpoint (see
   `MJPEG_FRAME_FORMAT.md`); the descriptor's bandwidth figures are the iso
   envelope, not a raw pixel format guarantee.
