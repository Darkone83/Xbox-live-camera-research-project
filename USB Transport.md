# USB transport + camera control — reverse-engineering notes

**Team Resurgent / Darkone83.** How the Video Chat XBE drives the Sony EyeToy,
mapped from `default.xbe` + `xboxkrnl.pdb` + the XDK headers. Companion to `Camera init.md`. The path is traced end to end here — app loop down to
OHCI — so this is the master map of the retail XBE (not of the homebrew driver; for
that, see `WORKING_IMPLEMENTATION.md`).

> This is the architecture trace (how the original XBE works). For authoritative
> **struct layouts and the typed API**, use `xbox_usb.h` (the `_URB` union,
> descriptors, and `USB_BUILD_*` macros are folded verbatim from the XDK `usb.h`).
> Where this trace's raw-byte offsets and the header disagree, **the header wins** —
> see the §7 update note.

---

## 0. The complete chain (top to bottom)

```
XInitDevices  → reads device-type table → camera descriptor @ 0x1b295c
   ├ INIT       FUN_001ba120  lock + zero state + event + camera-driver init
   │            (FUN_000c8b90 → FUN_000cefc0 → … → ced10 installs FUN_000cf340 dispatch)
   ├ CONNECT    FUN_001ba190  port# (obj+0x14), setup/pipes, DAT_00221944=1
   └ DISCONNECT FUN_001ba240  match handle → error+signal, DAT_00221944=0

once present, the app runs:
app camera-manager loop            0x00018E00   poll → open → set-format → capture → display
  └─ 5-call XAPILIB device API
       0x001BA280  poll / get device      -> handle or -1
       0x001BAA80  open + detect          (descriptor read, VID/PID)
       0x001BA8F0  set format / mode
       0x001BAD70  start capture          (+ backoff / retry)
       0x001BA9B0  poll / complete
  └─ command marshaller            0x001BDFA0   builds 'USBD' packets; cmd map 0x100..0x10d, 0, 3
       └─ camera-driver entry fn-ptrs (installed at registration):
            DAT_0022198c  main dispatch   (= param_5 of FUN_000ced10)
            DAT_00221AF8  frame-grab      (cmd 0)
            DAT_00221AFC  stream          (cmd 3)
            └─ camera class driver   FUN_000ceb50 / LAB_000cdea0 / FUN_000cdef0
                 (applies OV519 bridge + OV7648 sensor .set registers)
                 └─ transport:
                      0x001BE430  sync transfer primitive (event, submit, wait)
                        └─ 0x001B556D  per-type field fixups
                             └─ 0x001B5BBA  operation dispatch table
                                  └─ 0x001B84F0  per-pipe enqueue (control/bulk/iso)
                                       └─ TD builders 0x001B84B4 / 0x001B8445
                                            └─ OHCI hardware @ 0xFED00000
```

Two layers are still unread (see §9): the camera-driver register encoding
(`FUN_000ceb50` / `FUN_000cdef0`) and the enumeration/connect path that first
sets `DAT_00221944`/`48`.

---

## 1. Big picture

- **The USB stack is XAPILIB**, the XDK's XAPI library, statically linked. The
  XBE `XPP` section (`0x001B2760`–`0x001C543B`) is XAPILIB — confirmed by the
  XBE section table *and* by Ghidra resolving `XAPILIB::g_DeviceTypeInfoTableBegin`
  at `0x1B2760`. The `0xFED00000` mapping, contiguous DMA, and IRP submission are
  XAPILIB doing its normal USB job (same path `XInitDevices` uses for controllers).
- **The XDK headers expose no USB device API** — only XInput (HID/gamepad/MU),
  which can't see a camera. Everything below is internal XAPILIB reached via raw
  kernel exports (`Io*`, `Ob*`, `Mm*`, `Hal*`, `Ke*`) — see `xbox_kernel.h`.
- **The model is Windows-USB-like (USBD/URB).** Request blocks carry a
  function/type byte and are dispatched to per-operation handlers.

---

## 2. The 5-call device API (what the app actually uses)

The whole camera flow rides on five XAPILIB calls. `0x00018E00` orchestrates
them as a per-frame state machine. All live in the cleanly-analyzed `0x1BAxxx`
region (names reliable).

| Addr | Role | Notes |
|------|------|-------|
| `0x001BA280` | poll / get device | returns `DAT_00221948` (handle) if `DAT_00221944 != 0` && `DAT_0022193c == 0`, else `-1`. Pure state read — depends on enumeration setting those globals. |
| `0x001BAA80` | open + detect | builds a UDEV object (`FUN_001c00d0`), GET_DESCRIPTOR read, VID/PID compare (`045e`/`028c` → EyeToy hex-patch site). |
| `0x001BA8F0` | set format / mode | issues `bdfa0(0x102,0)`, `(0x10c,mode)`, `(0x101,0)`, `(3,1)`, `(3,2)`. Sets `DAT_00221938=1` (configured), `DAT_00221964=mode`. |
| `0x001BAD70` | start capture | backoff state machine (re-runs `ba8f0` after 10 fails, 500 ms via `FUN_0007a84a`); `bdfa0(3,3)` to stream; stores buffer `DAT_0022195c`, callback `DAT_00221960`; `bdfa0(0,0)` to grab. |
| `0x001BA9B0` | poll / complete | drains completion. |

**Reference loop** `0x00018E00`: poll (`ba280`) → if new device, open (`baa80`)
→ if mode changed, `ba8f0` → throttle to fps → `bad70(handle, buffer, &cb, 0)`
→ on frame-ready flag, `D3D8::D3DTexture_LockRect` + per-format convert into a
double-buffered texture (slot = `ctx+0x4C`, flipped each frame). This maps
directly onto our `XCam_PublishFrame` → texture path.

---

## 3. Command marshaller — `FUN_001bdfa0` (`0x001BDFA0`)

Every camera operation funnels here. It allocates a `'USBD'` (`0x44425355`)
request packet of `0x4c` bytes, fills it, and dispatches through the
camera-driver function pointers. These command codes are the same `0x100`/`0x101`/
`0x107` "IOCTLs" from the first research pass — **internal commands, not kernel
IOCTLs.**

Packet layout (the `'USBD'` request):
```
pkt[0]=0x4c (size)   pkt[1]=command   pkt[3]=request-specific descriptor ptr
pkt[4]=DAT_00221ae0 (device/config id)   pkt[5]=data buffer (size DAT_0022199c)
pkt[6]=result/param buffer ptr   pkt[7]=flag   pkt[8],[9]=params   pkt[0xe]=result size
```

Command map:
```
0x100  setup data buffer (size DAT_002219f0 -> DAT_00221ae8)        -> (*DAT_0022198c)
0x101  GET property/format  (0x44-byte desc; result 0x11 dwords -> DAT_00221aec) -> (*DAT_0022198c)
0x102  SET property/format  (0x44-byte desc, same shape)            -> (*DAT_0022198c)
0x106  configure (pkt[8]=pkt[9]=0xF, pkt[7]=1)                      -> (*DAT_0022198c)
0x107  calls FUN_001bdf50 first; pkt[6]=&DAT_002219c0              -> (*DAT_0022198c)
0x109  simple                                                       -> (*DAT_0022198c)
0x10c  GET data blob (alloc 0x1000; <=0x98 bytes -> DAT_00221b30)   -> (*DAT_0022198c)   <-- format/caps
0x10d  simple                                                       -> (*DAT_0022198c)
  0    GRAB FRAME  (header DAT_00221bc8=0x30, buffer _DAT_00221bf0=DAT_0022195c) -> (*DAT_00221af8)
  3    STREAM ctrl (arg=state; sets DAT_00221958=arg)               -> (*DAT_00221afc)
```
The `0x30` frame-grab header matches the URB header `{+0=0x30}` in §7 — consistent.

XREF: 20 call sites, all from the 5-call API (`ba4f0`, `ba8f0`, `ba9b0`, `baa80`,
`bad70`).

---

## 4. Camera-driver registration — `FUN_000ced10` → `FUN_001be360`

How the three dispatch function pointers get installed (they are **never**
written by a plain assignment — this is the only writer).

`FUN_000ced10` (`0x000CED10`, camera driver) builds a 15-dword descriptor on the
stack and calls `FUN_001be360(&desc)`:
```
desc[0] = 0x3c          (size)            -> DAT_00221984
desc[1] = 0                                -> DAT_00221988
desc[2] = param_5       (main dispatch)    -> DAT_0022198c   <-- (*main) in FUN_001bdfa0
desc[3] = FUN_000ceb50  (camera handler)   -> DAT_00221990
desc[4] = &LAB_000cdea0 (camera handler)   -> DAT_00221994
desc[5] = bufsize1                          -> DAT_00221998
desc[7] = bufsize2                          -> DAT_002219a0
... (15 dwords total -> 0x221984..0x2219c0)
```
`FUN_001be360` (`0x001BE360`): if `DAT_00221938 == 0`, copies the 15 dwords into
`0x221984`, then allocates `DAT_00221ae0` (size `DAT_00221998`) and `DAT_00221ae4`
(size `DAT_002219a0`).

Key facts:
- The **main per-command dispatch** (`DAT_0022198c`) is `param_5` of `FUN_000ced10`.
- `FUN_000ced10` has **no static caller** in the decomp → it is invoked
  **indirectly** via a driver/device-object dispatch pointer. So `param_5` can't
  be chased upward textually; find it via the driver-object hookup, or just read
  the concrete handlers below.
- `DAT_00221AF8` / `DAT_00221AFC` (frame-grab / stream) are **not** in this block
  (they're at `0x221af8/afc`, beyond `0x2219c0`) — installed elsewhere (TBD).

Concrete camera-class-driver entry points to read for the `.set` application:
```
0x000CEB50  FUN_000ceb50   cancel handler           (DAT_00221990)
0x000CDEA0  LAB_000cdea0   installed handler        (DAT_00221994)
0x000CDEF0  FUN_000cdef0   streaming alt-setting / iso config
```

---

## 4.5 Camera class driver — dispatch & the register-free verdict

The main dispatch `DAT_0022198c` (= `param_5`) is **`FUN_000cf340`** (`0x000CF340`).
It reads the command at `packet+4` and routes:
```
0x100 buffer setup → FUN_000ce510    0x109 status?  → FUN_000ce490
0x101 GET format   → FUN_000cf120    0x10a special  → FUN_000cf040
0x102 SET format   → FUN_000ce5c0    0x10b/0x10e    → FUN_001bf850 + FUN_001bf660
0x107 START stream → FUN_000cdd80    0x10d          → FUN_001bef70
0x108 STOP stream  → FUN_001bfad0
```
(`0` grab / `3` stream-state go to `DAT_00221AF8`/`DAT_00221AFC`, not this switch.)

**VERDICT: no register replay.** All command paths traced; none write OV519/OV7648
registers — every leaf call is XAPILIB transport:
- `0x102` SET format → `FUN_000ce5c0` (validate index) → `FUN_000ce0a0`: indexes a
  format table (`device+4+i*4`), flushes pipe ops (`bf850`/`bf660`), drains
  transfers (`FUN_001be9b0`, pure `Ke*` sync), clears slot.
- STREAM ctrl → `FUN_000cec70` → `FUN_000cdef0`: selects a USB **alt-setting** by
  mode index (iface table `+0xA4`, stride `0x14`, iso max-pkt at `+0x10`);
  IRQL-aware (defers via work-item `FUN_000ce4f0` at ≥DISPATCH_LEVEL).
- `0x107` START → `FUN_000cdd80`: builds iso stream object (`'DEVA'` =
  `0x45564544`), `KeInitializeSemaphore`, then `FUN_001bfd30` (iso setup).
- `FUN_000ceb50` (`DAT_00221990`) is the **cancel** path (unlink from req-list at
  `ctx+0x110`, complete `STATUS_CANCELLED 0xC0000120`).

So the EyeToy is a **standard USB iso-video device**: formats from descriptors,
selection by alt-setting, frames over iso. The `.set` register tables are the PC
approach and are **not used**. (This was the old verdict and it was wrong — the homebrew driver does replay the registers. See `Camera init.md` and `OV519_OV7648_INIT.md`.)

Structs from these handlers:
```
camera ctx:  +0x68 cur req  +0x110 req-list  +0x121 configured  +0x128 mode index
             +0x170 buffer   +0x185/+0x186 stream flags
iface obj:   +0x90 handle    +0xA4 alt-setting table (stride 0x14, +0x10 iso max-pkt)
             +0xA8 sel max-pkt  +0xE0/+0xE4 pipe open/cfg fn-ptrs  +0xFC max-pkt
stream obj:  +0 'DEVA'  +0x20 count  +0x21 state (2=run)  +0x3F max-pkt  +0xD semaphore
```

---

## 5. The UDEV device object — `FUN_001c00d0` (`0x001C00D0`)

The "open a device" primitive. Only caller = `FUN_001baa80`.
- `ExAllocatePoolWithTag(0x48, 'USBD')` + a `0x40`-byte aux block.
- magic `0x56454455` = **"UDEV"** at `+0`.
- `+0x1C = 0x1000` transfer-buffer size; type bytes at `+0x10`/`+0x11`.
- `+0x28` ← copies an ~18-byte SETUP/descriptor template (`param_2`) — same offset
  as the SETUP packet in the URB (§7).
- runs the initial setup transfer via `FUN_001c2a30` (which calls `FUN_001be430`).

So "open device" = allocate a UDEV object + issue one control transfer. That is
exactly the shape our PID-read milestone needs.

---

## 6. Transport layer (beneath the marshaller)

| Addr | Role |
|------|------|
| `0x001BE430` | **Synchronous transfer primitive**: build KEVENT, submit, wait. 20 callers; entry for control + iso. |
| `0x001B556D` | Per-type field fixups, then dispatch. |
| `0x001B5BBA` | **Operation dispatch table** (type byte → handler). |
| `0x001B84F0` | Per-pipe enqueue; dispatches on `pipe+0x11` (0/2/3 → control/bulk/iso). |
| `0x001B84B4` / `0x001B8445` | Control / iso TD builders → OHCI. |
| `0x001BF850` | Locked submit wrapper. |
| `0x001C2A30` | Transfer helper used by the UDEV constructor. |
| `0x001B3119` | OHCI controller init — maps `0xFED00000`, grabs IRQ1. |
| `0x001B3B1D` | `IoCreateDevice` — builds the USB device object. |
| `0x001B414C` | Completion — writes status, fires callback / signals event. |

### Submit chain
```
FUN_001be430  (KeInitializeEvent, submit, KeWaitForSingleObject)
   └─ FUN_001b556d  (per-type field fixups)
        └─ FUN_001b5bba  (dispatch on request type byte)
             └─ FUN_001b84f0  (per-pipe enqueue -> control/bulk/iso TD)
                  └─ FUN_001b414c  (completion: status + callback/event)
```

### Dispatch table (`FUN_001b5bba`, type byte at `req+0x01`)
```
0x02 → FUN_001b56f7      0x0C → FUN_001b7d27
0x04 → FUN_001b586b      0x0D → FUN_001b7e46
0x05 → FUN_001b588c      0x40/0x41 → FUN_001b84f0   (control + iso family)
0x07 → FUN_001b6ffd      0x43 → FUN_001b5b00
0x09 → FUN_001b7946      0x46 → FUN_001b5b67
0x0B → FUN_001b7bc0      0x4A → FUN_001b7af3
                         default → 0x80000200 (error)
```
Control transfers use the header `{+0=0x30, +1=0x40}` → `FUN_001b84f0`; the
operation is distinguished by the request fields (SETUP vs iso), not the type byte.

---

## 7. The request block (URB) — control transfer

> **UPDATE:** the authoritative `_URB` layout is now folded into `xbox_usb.h`
> verbatim from the XDK `usb.h`. The raw-byte observation below was a correct read
> of the *bytes* in `FUN_001baa80`, but the field *interpretation* was wrong: the
> `0x30`/`0x40` at `+0x00`/`+0x01` are the `_URB_HEADER`'s **`Length`** and
> **`Function`** fields, NOT a "subcode/type", and the real struct
> (`URB_CONTROL_TRANSFER`: `Hdr, EndpointHandle, TransferBufferLength,
> TransferBuffer, TransferDirection, ShortTransferOK, InterruptDelay, Padding, Hca,
> SetupPacket`) orders fields differently. **Use `xbox_usb.h` + the `USB_BUILD_*`
> macros — do not hand-stamp these offsets.** The raw dump is kept only as the
> original observation that the bytes matched.

Raw bytes observed in `FUN_001baa80`'s GET_DESCRIPTOR build (interpretation
corrected above):
```
+0x00 : 0x30          = Hdr.Length (sizeof URB_CONTROL_TRANSFER), NOT a "subcode"
+0x01 : 0x40          = Hdr.Function (URB_FUNCTION_CONTROL_TRANSFER = async bit), NOT "type"
+0x04 : <status out>  = Hdr.Status
...                     (callback/context = Hdr.CompleteProc/CompleteContext)
+0x10 : <handle>      = EndpointHandle
+0x14 : 0x12          = TransferBufferLength (18 = device descriptor)
+0x18 : <buffer>      = TransferBuffer
SETUP : 80 06 00 01 00 00 12 00  = SetupPacket {bmRequestType=0x80, bRequest=0x06
                                    GET_DESCRIPTOR, wValue=0x0100, wIndex=0, wLength=18}
```
Call: `IUsbDevice::SubmitRequest(&urb)` (= `FUN_001be430`).

---

## 8. PID-read milestone — concrete recipe

Build a control-IN URB with `USB_BUILD_CONTROL_TRANSFER` (xbox_usb.h), retargeted to
read one OV7648 sensor register over the OV519 control endpoint:
- buffer = 1 byte; `wLength` = 1.
- SETUP → **OV519 vendor register-read** (not GET_DESCRIPTOR): vendor
  `bmRequestType` (~`0xC1` = `USB_DEVICE_TO_HOST|USB_VENDOR_COMMAND|...`), OV519 read
  `bRequest`, `wIndex` = register (`0x0A`).
- `Device->SubmitRequest(&urb)`, read the byte: expect `0x76` (reg `0x0A`), then
  `0x48` (reg `0x0B`) → confirms OV7648 / the EyeToy.

Proves host hookup + URB layout + OV519 encoding at once.

---

## 9. State globals

```
0x00221944  device-present flag           0x00221948  device handle
0x0022194c  USB-ready / connected         0x0022193c  busy / error flag
0x00221938  configured flag (set by ba8f0) 0x00221958  stream state (3 = running)
0x0022195c  capture buffer (set by bad70) 0x00221960  pending-capture callback
0x00221964  current mode (set by ba8f0)

-- marshaller / packet --
0x00221ae0  device/config id (pkt[4])     0x00221ae4  used in 0x101/0x102 descriptor
0x00221ae8  cmd-0x100 buffer              0x00221aec  GET result block (0x101; pkt[3] for 0/3)
0x00221b30  format/caps blob (0x10c,0x101) 0x0022199c data-buffer size (pkt[5])
0x002219f0  cmd-0x100 buffer size         0x002219c0 cmd-0x107 buffer

-- registration block @ 0x221984 (15 dwords) --
0x0022198c  main dispatch fn-ptr (= param_5 of FUN_000ced10)
0x00221990  fn-ptr = FUN_000ceb50         0x00221994  fn-ptr = LAB_000cdea0
0x00221998  bufsize1                       0x002219a0  bufsize2
0x00221af8  frame-grab fn-ptr (set TBD)    0x00221afc  stream fn-ptr (set TBD)

-- bad70 backoff --
0x001b2984  backoff timestamp             0x001b297c  fail counter
```

---

## 10. Still open

**Quick confirmations (readable):**
1. `FUN_000cefc0 → FUN_000ced10` — the dispatch-install link in the init chain
   (flat decomp inlines `cefc0`; verify in Ghidra).
2. `FUN_000cf340` default case — confirm `0x10c`/`0x106` just fill a caps/format
   blob, not a register write (last untraced command path).
3. `DAT_00221af8` / `DAT_00221afc` — RESOLVED ENOUGH (§12). Indirect-install glue
   (only a read from `FUN_001bdfa0` @ `0x1be0a4`); identity resists static analysis
   but it's iso-submit glue over the mapped transport — not blocking.
4. Setup cluster `FUN_001b52a5/52AB/52B1/52B7` — largely confirmed via
   `FUN_001bfc50`/`FUN_001bf050` (§12: config-descriptor read + parse → tables +
   pipe). Read those two for full detail.

**Build-prep — now AUTHORITATIVE (not just on paper):**
5. Iso URB layout — DONE & TYPED: folded verbatim from XDK `usb.h` into
   `xbox_usb.h` (`IsochOpenEndpoint`/`StartTransfer`/`AttachBuffer` arms +
   `USB_BUILD_ISOCH_*` macros + `_USBD_ISOCH_TRANSFER_STATUS`). SLIX driver is a
   worked example. (Submit `FUN_001be430`; per-packet OHCI iso-TD fields = std spec.)
6. Frame path — DONE: completion `FUN_001b414c` fires `urb+0x08(urb, urb+0x0C)`;
   buffer `DAT_0022195c` + `0x30` header; convert RGB24→32bpp.

**Closes only on hardware:** XInitDevices descriptor triggering on RXDK, first
control transfer round-trip, iso scheduling, stability.

## Resolved this pass
- **Enumeration / lifecycle — ANSWERED (see §11).** Camera comes up via
  `XInitDevices` + a device-type descriptor (`0x1b295c`); INIT/CONNECT/DISCONNECT
  callbacks `FUN_001ba120`/`190`/`240`. No enumeration code of our own.
- **Register question — no register replay.** All camera command paths register-free.
- **Main dispatch — `DAT_0022198c` = `FUN_000cf340`** with the full command map (§4.5).
- Dispatch mechanism (`FUN_001bdfa0` marshaller + camera-driver fn-ptrs).
- The 5-call API + reference manager loop (`0x00018E00`).
- Registration (`FUN_000ced10` → `FUN_001be360`, 15-dword descriptor).
- The UDEV device object (`FUN_001c00d0`); "open = alloc + control xfer".
- Command code maps for both `FUN_001bdfa0` and `FUN_000cf340`.

---

## 11. Enumeration & device lifecycle

The camera is **not** brought up by code we'd write — it rides `XInitDevices`,
the same XDK call that brings up controllers and MUs. `XInitDevices` walks a
table of **device-type descriptors**; the camera's entry is a static struct at
`0x1b295c`:

```
0x1b295c  FUN_001ba120   INIT callback
0x1b2960  FUN_001ba190   CONNECT callback
0x1b2964  FUN_001ba240   DISCONNECT callback
0x1b2968  0x01           flag / type id
```
(`FUN_001ba120` and `FUN_001ba190` both also referenced directly from
`XInitDevices` at `0x1b2e41` / `0x1b2c41`.)

**INIT — `FUN_001ba120`** (XInitDevices-time, once):
- `RtlInitializeCriticalSection(&DAT_002217f8)` — the lock all camera fns use.
- zeros the `0xA4`-dword state block from `0x221938` (device/marshaller/registration globals).
- `KeInitializeEvent(&DAT_002217e8, …)` — the disconnect-notification event.
- `FUN_000c8b90 → FUN_000cefc0` = camera-driver init → installs the dispatch
  pointer (`DAT_0022198c = FUN_000cf340`) via `FUN_000ced10` → `FUN_001be360`.
  *(the `cefc0 → ced10` hop is verifiable in Ghidra; flat decomp inlines `cefc0`.)*

**CONNECT — `FUN_001ba190`** (device enumerated by XAPILIB):
- `uVar1 = FUN_001b532d()` = the port number (`deviceObj+0x14`), must be `< 4`.
- stores it as the handle (`DAT_00221948`), runs the setup cluster
  (`FUN_001b52a5/AB/B1/B7` — descriptor read + pipe creation), sets
  `DAT_00221944 = 1` (present), signals `FUN_001b6333(0)`.

**DISCONNECT — `FUN_001ba240`** (device removed):
- if `FUN_001b532d() == DAT_00221948` (our device): if an op pends
  (`DAT_00221940`), set `DAT_0022193c = 1` (error) + `KeSetEvent(&DAT_002217e8)`;
  then `DAT_00221944 = 0` (gone).

**Homebrew implication:** Phase 0 is *register a device-type descriptor with
`XInitDevices`* (or confirm RXDK's XAPILIB already carries it) — not write
enumeration. XAPILIB creates the device + pipe objects; we get a callback with a
port number and drive the 5-call API. The pipe object for `req+0x10` comes from
the setup cluster, owned by XAPILIB.

---

## 12. Frame data path (static frame & motion)

End to end, one frame:
```
1. read+parse CONFIG descriptor  FUN_001bfc50 → FUN_001bf050
                                  (alloc 0x40, read wTotalLength@buf+2, grow+re-read;
                                   parse fills the alt-setting/format table)
2. open iso pipe                 fn-ptr @ streamobj+0xCC
3. build iso URB                 header 0x4030 (control = 0x40); submit FUN_001be430
4. enqueue TD + doorbell         FUN_001b8445 → FUN_001b8358
                                  (queue: pipe+0x28 head / +0x2C tail; TD link +0x24;
                                   TD packet-count +0x20, must be <4)
5. HC fills frame buffer         DAT_0022195c (+ 0x30 request header)
6. completion fires callback     FUN_001b414c: (*(urb+0x08))(urb, *(urb+0x0C))
                                  → the app frame-ready cb stashed in DAT_00221960
7. assemble + display            LockRect + RGB24→32bpp via the per-format
                                  callbacks (PTR_DAT_001ff190), as FUN_00018E00 does
```

Concrete layout to code against:
- **URB completion:** callback ptr at `urb+0x08`, context at `urb+0x0C`.
- **iso vs control:** iso request header = `0x4030`, control = `0x40`; both submit
  via `FUN_001be430` (same primitive).
- **TD queue (OHCI):** pipe head `+0x28`, tail `+0x2C`; TD next-link `+0x24`;
  TD packet-count `+0x20` (`<4`); doorbell `FUN_001b8358`.
- **config-descriptor read:** grow-to-`wTotalLength` (`buf+2`) pattern; parse via
  `FUN_001bf050` → populates the descriptor-derived format/alt-setting tables.

**Grab / stream handlers (`DAT_00221af8` / `DAT_00221afc`):** the camera-driver's
iso-submit glue. Installed **indirectly** (like `DAT_0022198c`) — `0x221af8` shows
only a read, from `FUN_001bdfa0` @ `0x1be0a4`; no static write XREF. Exact identity
resists static analysis, but **not on the critical path**: a homebrew issues the
grab via the marshaller, or replicates the iso submit above directly.

**Motion** = the same loop: cmd `3` (stream) primes via `DAT_00221afc`; cmd `0`
(grab) repeats; `FUN_000cdd80` sets the iso transfer count (stream-obj `+0x20`)
and double-buffers.

## Remaining unknowns are now only (hardware):
- **EyeToy `_PNP_CLASS_ID` / interface class** — the class-driver match key; read at attach.
- **RXDK linkage in practice** — expected fine (same `xapilib.lib` RXDK requires); a
  compile-and-link confirms the extern declarations resolve.
- **hardware**: descriptor triggers on RXDK, first control transfer round-trips,
  iso timing, stability.

> Iso URB layout is no longer an unknown — the full iso arms (`IsochOpenEndpoint`
> etc.), `_USBD_ISOCH_TRANSFER_STATUS`, `_USBD_ISOCH_BUFFER_DESCRIPTOR`, and the
> `USB_BUILD_ISOCH_*` macros are folded verbatim from XDK `usb.h` into `xbox_usb.h`,
> and the SLIX driver is a worked example of the
> open→start→attach sequence. Per-packet OHCI iso-TD fields are standard OHCI spec.
