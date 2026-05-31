# Writing a USB Class Driver for Xbox — distilled authoritative guide

Source: **"Writing USB Class Drivers for Xbox.doc"** (official Microsoft XDK doc, found
in usb_more.zip). This is the authoritative how-to for exactly what we're building. It
**confirms our reverse-engineered model and corrects several specifics** — the
corrections are flagged ⚠ because some would have caused real bugs. Full worked
source for every driver type is in usb_more.zip (`usb/xid`, `usb/xkbd`, `usb/hawk`,
`usb/mu`, `usb/usbsamp`, `usb/ohcd`).

## The model (confirmed)
- Xbox peripheral ports (XPP) = USB 1.1 + OpenHCI electrically; **class level diverges**
  from standard USB. Class drivers are **static libraries linked into the title**.
- The core USB stack (usbd.lib, part of xapilib.lib) enumerates class drivers at
  `XInitDevices` time.

## Registration — via LINKER SEGMENTS (this is how the 0x1b2774 table is built)
⚠ Refines our "+4/+8 table" finding: the table is assembled by the **linker**, not
hand-built. Each class driver puts its registration pointer in a segment
`.XPP$Class<Name>` (must sort between `.XPP$ClassA` and `.XPP$ClassZ`; if your class
name starts with Z, prefix a lead char). The linker merges `.XPP$ClassA..Z` into the
table the core driver walks. Use `DECLARE_XPP_TYPE` / `USB_CLASS_DRIVER_DECLARATION`
/ `USB_CLASS_DRIVER_DECLARATION_POINTER`.
- ⚠ **Only ONE driver per device class** — if two are linked, only the first
  alphabetically gets Add/Remove.
- ⚠ **Linker-discard trap:** put the registration table in the same module as an
  essential API the title always calls (e.g. your OpenDevice), or the linker drops
  the whole driver as unreferenced.

## The THREE entry points the driver must implement
Named `<Class>Init`, `<Class>AddDevice`, `<Class>RemoveDevice`:

### `ClassInit(IUsbInit *UsbInit)` — at XInitDevices, before hardware init
Register resource requirements. Call `UseDefaultCount()` first; if TRUE, you pick max
devices; else call `GetMaxDeviceTypeCount(XppDeviceType)` per type. Fill a
`USB_RESOURCE_REQUIREMENTS` and call `RegisterResources()` (pointer not cached — stack
OK). Struct (⚠ now we have its real fields):
```c
typedef struct _USB_RESOURCE_REQUIREMENTS {
    UCHAR ConnectorType;            // USB_CONNECTOR_TYPE_DIRECT/HIGH_POWER/LOW_POWER
    UCHAR MaxDevices;               // contract: max open at once (you MUST enforce)
    UCHAR MaxCompositeInterfaces;   // expect composite or not
    UCHAR MaxControlEndpoints;      // excluding default endpoint
    UCHAR MaxBulkEndpoints;
    UCHAR MaxInterruptEndpoints;
    UCHAR MaxControlTDperTransfer;
    UCHAR MaxBulkTDperTransfer;
    UCHAR MaxIsochEndpoints;        // ← camera: 1 (the iso video EP)
    UCHAR MaxIsochMaxBuffers;       // ← camera: buffers attached per iso EP
} USB_RESOURCE_REQUIREMENTS;
```
For the camera: ConnectorType high-power-ish, MaxDevices 1, MaxIsochEndpoints 1,
MaxIsochMaxBuffers = however many frame buffers you double/triple-buffer.

### `ClassAddDevice(IUsbDevice *Device)` — the attach (⚠ critical rules)
Called when a device with matching `bInterfaceId` connects. **At DPC/DISPATCH level —
MUST NOT BLOCK.** Responsibilities:
1. Determine if device is supported/functioning.
2. `Device->SetClassSpecificType(ordinal)`.
3. Allocate the **device extension** (dynamically or from an init-time free list).
4. `Device->SetExtension(ext)` — associate it (retrieve via `GetExtension` in Remove).
5. Cache the `IUsbDevice*`.
6. Call `Device->AddComplete(status)` — **ALL enumeration blocks until you do.**

⚠ These getters are **enumeration-time ONLY** (ASSERT/garbage otherwise):
`GetDeviceDescriptor` (⚠ **first 8 bytes only** — VID/PID need a separate request!),
`GetConfigurationDescriptor` (full, walkable), `GetInterfaceDescriptor` (the one this
Add was called for), `GetEndpointDescriptor(Type, Direction, Index)`.

⚠ To send requests during enum: return from ClassAddDevice WITHOUT AddComplete; your
completion routine runs (still at enum time); cascade as needed; eventually call
AddComplete. **Set a watchdog** — a NAKing device hangs forever (HW never gives up).

`AddComplete(USBD_STATUS_SUCCESS)` = supported. `AddComplete(USBD_STATUS_UNSUPPORTED_DEVICE)`
= reject (port turned off, no RemoveDevice). Other errors → treated as unsupported,
but core resets + retries up to **5 times** if still attached.

### `ClassRemoveDevice(IUsbDevice *Device)` — disconnect
Also at DPC, must not block. Close all open endpoints (async → cascading state
machine). Invalidate exposed handles. Call `Device->RemoveComplete()` promptly
(<~100ms) or you cause general enum problems. After RemoveComplete the IUsbDevice is
invalid. ⚠ Never called during EnumerationTime (simplifies sync).

## Open/close synchronization (⚠ the hard part the guide spells out)
ClassAddDevice/RemoveDevice both at DPC → synchronize by raising IRQL to
DISPATCH_LEVEL. Since close blocks (can't stay at raised IRQL), use **pending flags**:
- Close: raise IRQL, kick close state machine, lower, wait on close event. State
  machine sets "close pending", checks "remove pending"; if remove pending, returns;
  else runs, then on finishing checks remove-pending again → calls RemoveComplete.
- Remove: sets "remove pending", checks "close pending"; if close pending, returns
  (close finishes the remove); else runs to completion, calls RemoveComplete, checks
  close-pending, may finish close steps + set close event.
- ⚠ Simplification available: drivers that auto-open endpoints right after AddComplete
  and close them before RemoveComplete avoid most of this — at the cost of allocating
  all memory up front (XID's model). **Recommended for the camera** (simpler).

## URBs (confirms xbox_usb.h)
- Xbox URBs ≠ WDM URBs — **no IRP**; all rolled into the URB. Submit via
  `IUsbDevice::SubmitRequest`.
- Two flavors: **synchronous** (done when SubmitRequest returns) and **asynchronous**
  (completion routine called later; async functions are the bolded ones — Control,
  Bulk/Interrupt, Close, Abort, Isoch Close). 
- Allocate URB anywhere (stack OK if you block until completion). Fill via the
  **`USB_BUILD_XXX` macros (recommended)** — exactly what xbox_usb.h provides.
- The `_URB` union in the guide matches xbox_usb.h exactly (Header, ControlTransfer,
  BulkOrInterruptTransfer, CommonTransfer, OpenEndpoint, CloseEndpoint, ... Isoch*).

## Exposing your API
Titles run in kernel mode — no IOCTLs/ring transitions. Your driver's API is just
library methods. Organize device extensions however you like; use `Device->GetPort()`
(from ClassAddDevice) to index them.

## What this means for the camera driver (action items)
1. ⚠ **Don't read VID/PID from GetDeviceDescriptor()** — it's 8 bytes. Send a real
   GET_DESCRIPTOR (full 18) via USB_BUILD_CONTROL_TRANSFER, or match by interface
   class (0xFF) which GetInterfaceDescriptor DOES give you at enum time.
2. ⚠ Register via `.XPP$Class<Name>` linker segment + the three `Class*` entry points,
   NOT a hand-built table.
3. ⚠ ClassAddDevice is at DPC, non-blocking — read descriptors, SetExtension,
   AddComplete. Cascade with completion routines + a watchdog if sending requests.
4. Use the XID "allocate-up-front + auto-open endpoints" model to avoid the full
   close/remove state-machine complexity.
5. `GetEndpointDescriptor(USB_ENDPOINT_TYPE_ISOCHRONOUS, IN, 0)` to find the iso video
   EP at enum time.
6. RegisterResources with MaxIsochEndpoints=1, MaxIsochMaxBuffers=your buffer count.

## Worked source to copy from (all in usb_more.zip)
- `usb/usbsamp/usbsamp.cpp` — the **sample class driver** (start here)
- `usb/xid/` + `usb/xidex/` — gamepad (interrupt), the alloc-up-front model
- `usb/xkbd/kbd.cpp` — keyboard (simpler)
- `usb/hawk/` — voice (ISO — closest to camera streaming)
- `usb/ohcd/isoch.c` + `isoch.h` — the iso implementation itself (ground truth for iso)
- `usb/usbd/usbinit.cpp` — XInitDevices/registration internals
- `usb/inc/hcdi.h` — host controller interface (full)
- `usb/kdexts/usb/` — kernel-debugger USB struct dumpers (struct layouts!)