/*
 * xbox_usb.h  -- Team Resurgent / Darkone83
 *
 * Declarations for the XDK USB framework that lives in xapilib but is NOT in the
 * public SDK headers. Reconstructed from the decorated symbols in xapilib(.lib /
 * d.lib / p.lib) + the usbxapi.h fragment + our reverse-engineering. Including
 * this lets the compiler emit correctly-mangled __thiscall calls that the linker
 * resolves against xapilib.lib (the real XDK lib RXDK ships).
 *
 * SOURCE OF TRUTH: the C++ mangled names. Each method below lists its mangled
 * symbol so you can re-verify against `dumpbin /symbols xapilib.lib`.
 *
 * VERIFY-ON-HARDWARE markers [V] flag fields whose exact offset/type came from RE
 * rather than from a symbol; everything else is from Microsoft's own mangling.
 *
 *   ?SubmitRequest@IUsbDevice@@QAEJPAT_URB@@@Z
 *     -> public: long __thiscall IUsbDevice::SubmitRequest(union _URB *)
 *   (JPAT = returns long(J), arg is pointer(PA) to union(T) _URB)
 */
#ifndef XBOX_USB_H
#define XBOX_USB_H

 /* xtl.h is the XDK umbrella header -- always present when building for Xbox.
  * It provides the base types (UCHAR/USHORT/ULONG/WCHAR/PVOID/LONG/VOID),
  * XPP_DEVICE_TYPE, and XDEVICE_PREALLOC_TYPE. Everything ELSE this driver needs
  * from the (scattered / unavailable) internal USB headers is FOLDED IN below so
  * this file is self-contained -- we do NOT depend on usb100.h / usbxapi.h /
  * pshpack*.h being present. */
#include <xtl.h>

  /* USB drivers + XAPI USB code live in the .XPP section (per usbxapi.h). Match it
   * so our class driver lands where USBD expects. */
#pragma code_seg(".XPPCODE")
#pragma data_seg(".XPP$Data")
#pragma const_seg(".XPPRDATA")

#ifdef __cplusplus
extern "C" {
#endif

    /* ------------------------------------------------------------------ *
     *  Standard USB 1.0/1.1 descriptors + constants.
     *  FOLDED IN VERBATIM from the XDK usb100.h (authoritative -- matches our RE
     *  reconstruction exactly; kept here so this header is self-contained and does
     *  not depend on usb100.h being present). Packed byte layout, little-endian.
     * ------------------------------------------------------------------ */
#pragma pack(push, 1)

     /* descriptor bDescriptorType values */
#define USB_DEVICE_DESCRIPTOR_TYPE          0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE   0x02
#define USB_STRING_DESCRIPTOR_TYPE          0x03
#define USB_INTERFACE_DESCRIPTOR_TYPE       0x04
#define USB_ENDPOINT_DESCRIPTOR_TYPE        0x05
#define USB_DESCRIPTOR_MAKE_TYPE_AND_INDEX(d, i) ((USHORT)((USHORT)(d)<<8 | (i)))

/* bmAttributes endpoint type (the iso selector we need) */
#define USB_ENDPOINT_TYPE_MASK              0x03
#define USB_ENDPOINT_TYPE_CONTROL           0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS       0x01   /* the camera video endpoint */
#define USB_ENDPOINT_TYPE_BULK              0x02
#define USB_ENDPOINT_TYPE_INTERRUPT         0x03

/* endpoint direction (bit in bEndpointAddress) */
#define USB_ENDPOINT_DIRECTION_MASK         0x80
#define USB_ENDPOINT_DIRECTION_OUT(a)       (!((a) & USB_ENDPOINT_DIRECTION_MASK))
#define USB_ENDPOINT_DIRECTION_IN(a)        ((a) & USB_ENDPOINT_DIRECTION_MASK)

/* standard request codes (chapter 9) */
#define USB_REQUEST_GET_STATUS              0x00
#define USB_REQUEST_CLEAR_FEATURE           0x01
#define USB_REQUEST_SET_FEATURE             0x03
#define USB_REQUEST_SET_ADDRESS             0x05
#define USB_REQUEST_GET_DESCRIPTOR          0x06   /* our §7 SETUP bRequest */
#define USB_REQUEST_SET_DESCRIPTOR          0x07
#define USB_REQUEST_GET_CONFIGURATION       0x08
#define USB_REQUEST_SET_CONFIGURATION       0x09
#define USB_REQUEST_GET_INTERFACE           0x0A
#define USB_REQUEST_SET_INTERFACE           0x0B   /* selects the video alt-setting */
#define USB_REQUEST_SYNC_FRAME              0x0C

/* device classes */
#define USB_DEVICE_CLASS_RESERVED           0x00
#define USB_DEVICE_CLASS_AUDIO              0x01
#define USB_DEVICE_CLASS_COMMUNICATIONS     0x02
#define USB_DEVICE_CLASS_HUMAN_INTERFACE    0x03
#define USB_DEVICE_CLASS_VENDOR_SPECIFIC    0xFF   /* EyeToy/OV519 likely enumerates here */

/* NOTE: IUsbDevice::GetDeviceDescriptor() is mangled to return
 * _USB_DEVICE_DESCRIPTOR8 -- same 18-byte layout as _USB_DEVICE_DESCRIPTOR,
 * the "8" is just the XDK's internal variant name. We define both; treat as
 * interchangeable at the call site. */
    typedef struct _USB_DEVICE_DESCRIPTOR {
        UCHAR  bLength;
        UCHAR  bDescriptorType;       /* 0x01 */
        USHORT bcdUSB;
        UCHAR  bDeviceClass;
        UCHAR  bDeviceSubClass;
        UCHAR  bDeviceProtocol;
        UCHAR  bMaxPacketSize0;
        USHORT idVendor;              /* EyeToy 0x054C / Xbox Cam 0x045E */
        USHORT idProduct;             /* EyeToy 0x0155 / Xbox Cam 0x028C */
        USHORT bcdDevice;
        UCHAR  iManufacturer;
        UCHAR  iProduct;
        UCHAR  iSerialNumber;
        UCHAR  bNumConfigurations;
    } USB_DEVICE_DESCRIPTOR, * PUSB_DEVICE_DESCRIPTOR;
    typedef struct _USB_DEVICE_DESCRIPTOR _USB_DEVICE_DESCRIPTOR8;          /* alias: GetDeviceDescriptor's return */
    typedef       USB_DEVICE_DESCRIPTOR   USB_DEVICE_DESCRIPTOR8, * PUSB_DEVICE_DESCRIPTOR8;

    typedef struct _USB_ENDPOINT_DESCRIPTOR {
        UCHAR  bLength;
        UCHAR  bDescriptorType;       /* 0x05 */
        UCHAR  bEndpointAddress;      /* dir bit 0x80 | ep number */
        UCHAR  bmAttributes;          /* & 0x03 == USB_ENDPOINT_TYPE_ISOCHRONOUS for video */
        USHORT wMaxPacketSize;        /* iso bandwidth per (micro)frame */
        UCHAR  bInterval;
    } USB_ENDPOINT_DESCRIPTOR, * PUSB_ENDPOINT_DESCRIPTOR;

    typedef struct _USB_CONFIGURATION_DESCRIPTOR {
        UCHAR  bLength;
        UCHAR  bDescriptorType;       /* 0x02 */
        USHORT wTotalLength;          /* grow-and-reread key (RE: FUN_001bfc50) */
        UCHAR  bNumInterfaces;
        UCHAR  bConfigurationValue;
        UCHAR  iConfiguration;
        UCHAR  bmAttributes;
        UCHAR  MaxPower;
    } USB_CONFIGURATION_DESCRIPTOR, * PUSB_CONFIGURATION_DESCRIPTOR;

    typedef struct _USB_INTERFACE_DESCRIPTOR {
        UCHAR bLength;
        UCHAR bDescriptorType;        /* 0x04 */
        UCHAR bInterfaceNumber;
        UCHAR bAlternateSetting;      /* the format/mode selector (RE: cdef0) */
        UCHAR bNumEndpoints;
        UCHAR bInterfaceClass;
        UCHAR bInterfaceSubClass;
        UCHAR bInterfaceProtocol;
        UCHAR iInterface;
    } USB_INTERFACE_DESCRIPTOR, * PUSB_INTERFACE_DESCRIPTOR;

    typedef struct _USB_STRING_DESCRIPTOR {
        UCHAR bLength;
        UCHAR bDescriptorType;        /* 0x03 */
        WCHAR bString[1];
    } USB_STRING_DESCRIPTOR, * PUSB_STRING_DESCRIPTOR;

    typedef struct _USB_COMMON_DESCRIPTOR {
        UCHAR bLength;
        UCHAR bDescriptorType;
    } USB_COMMON_DESCRIPTOR, * PUSB_COMMON_DESCRIPTOR;

    /* Xbox-modified hub descriptor (usb100.h notes: USB 1.1 only, <=7 devices;
     * confirms the <=4-port + hub topology seen in the connect callback). */
    typedef struct _USB_HUB_DESCRIPTOR {
        UCHAR  bDescriptorLength;
        UCHAR  bDescriptorType;
        UCHAR  bNumberOfPorts;
        USHORT wHubCharacteristics;
        UCHAR  bPowerOnToPowerGood;
        UCHAR  bHubControlCurrent;
        UCHAR  DeviceRemovable;       /* Xbox: single byte (<=7 devices) */
    } USB_HUB_DESCRIPTOR, * PUSB_HUB_DESCRIPTOR;

#pragma pack(pop)

    /* ------------------------------------------------------------------ *
     *  Framework types referenced by the interface signatures.
     *  [V] = layout from RE/spec, not from a symbol -- verify against RXDK headers.
     * ------------------------------------------------------------------ */

     /* _PNP_CLASS_ID -- the class-driver match key. FindClassDriver compares
      * descriptor[0]=class against (id & 0xff) and descriptor[1]=subclass against
      * ((id>>8)&0xff). CONFIRMED passed BY VALUE in the mangled signatures
      * (T_PNP_CLASS_ID, not PAT...), so it's a small value type -- a 32-bit id.
      *   ?USBD_FindClassDriver@@YGPAU_USB_CLASS_DRIVER_DESCRIPTION@@T_PNP_CLASS_ID@@@Z
      *   ?USBD_LoadClassDriver@@YIXPAVIUsbDevice@@T_PNP_CLASS_ID@@@Z */
    typedef ULONG PNP_CLASS_ID, _PNP_CLASS_ID;

    /* ============================================================================
     *  URB FRAMEWORK -- FOLDED IN VERBATIM FROM THE XDK usb.h (AUTHORITATIVE).
     *  This REPLACES all earlier [V] reconstructions. These are the exact MS
     *  definitions the framework expects -- zero guesswork. Use the USB_BUILD_*
     *  macros (below) to populate, never hand-stamp offsets.
     * ============================================================================ */
    typedef LONG USBD_STATUS;
#define USBD_STATUS_SUCCESS        ((USBD_STATUS)0x00000000L)
#define USBD_STATUS_PENDING        ((USBD_STATUS)0x40000000L)
#define USBD_STATUS_ERROR          ((USBD_STATUS)0x80000000L)
#define USBD_STATUS_UNSUPPORTED_DEVICE ((USBD_STATUS)0x80000400L)
#define USBD_SUCCESS(S) ((USBD_STATUS)(S) >= 0)
#define USBD_ERROR(S)   ((USBD_STATUS)(S) < 0)

    /* opaque framework types referenced by signatures (names confirmed from libs;
     * internals not needed by a class driver) */
    typedef struct _KEVENT               KEVENT, * PKEVENT;               /* NT event (xbox_native.h) */
    typedef struct _USBD_HOST_CONTROLLER USBD_HOST_CONTROLLER, * PUSBD_HOST_CONTROLLER;
    typedef struct _TRANSFER             TRANSFER, * PTRANSFER;

    /* URB Hdr.Function codes */
#define URB_FUNCTION_USBD_PROCESSED             0x80
#define URB_FUNCTION_ASYNCHRONOUS               0x40
#define URB_FUNCTION_CONTROL_TRANSFER           (0x00 | URB_FUNCTION_ASYNCHRONOUS)
#define URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER (0x01 | URB_FUNCTION_ASYNCHRONOUS)
#define URB_FUNCTION_OPEN_ENDPOINT              0x02
#define URB_FUNCTION_CLOSE_ENDPOINT             (0x03 | URB_FUNCTION_ASYNCHRONOUS)
#define URB_FUNCTION_GET_ENDPOINT_STATE         0x04
#define URB_FUNCTION_SET_ENDPOINT_STATE         0x05
#define URB_FUNCTION_ABORT_ENDPOINT             (0x06 | URB_FUNCTION_ASYNCHRONOUS)
#define URB_FUNCTION_GET_FRAME_NUMBER           0x07
#define URB_FUNCTION_OPEN_DEFAULT_ENDPOINT      (URB_FUNCTION_USBD_PROCESSED | URB_FUNCTION_OPEN_ENDPOINT)
#define URB_FUNCTION_CLOSE_DEFAULT_ENDPOINT     (URB_FUNCTION_USBD_PROCESSED | URB_FUNCTION_CLOSE_ENDPOINT)
#define URB_FUNCTION_RESET_PORT                 (URB_FUNCTION_USBD_PROCESSED | 0x08)
#define URB_FUNCTION_ISOCH_OPEN_ENDPOINT        0x09
#define URB_FUNCTION_ISOCH_CLOSE_ENDPOINT       (0x0A | URB_FUNCTION_ASYNCHRONOUS)
#define URB_FUNCTION_ISOCH_ATTACH_BUFFER        0x0B
#define URB_FUNCTION_ISOCH_START_TRANSFER       0x0C
#define URB_FUNCTION_ISOCH_STOP_TRANSFER        0x0D

/* transfer direction + setup-packet bmRequestType bits */
#define USB_TRANSFER_DIRECTION_OUT  0x01
#define USB_TRANSFER_DIRECTION_IN   0x02
#define USB_HOST_TO_DEVICE          0x00
#define USB_DEVICE_TO_HOST          0x80
#define USB_STANDARD_COMMAND        0x00
#define USB_CLASS_COMMAND           0x20
#define USB_VENDOR_COMMAND          0x40
#define USB_COMMAND_TO_DEVICE       0x00
#define USB_COMMAND_TO_INTERFACE    0x01
#define USB_COMMAND_TO_ENDPOINT     0x02
/* iso interrupt-delay (NOTE usb.h bug 9512: only _0_MS is safe) */
#define USBD_DELAY_INTERRUPT_0_MS   0
#define USBD_DELAY_INTERRUPT_NONE   7
#define URB_FLAG_ISOCH_START_ASAP   0x0001
#define URB_FLAG_ISOCH_CIRCULAR_DMA 0x0001
#define USBD_ISOCH_START_FRAME_RANGE 1024

    typedef union _URB* PURB;
    typedef VOID(*PURB_COMPLETE_PROC)(PURB Urb, PVOID Context);

    struct _URB_HCD_AREA {
        union { USHORT HcdTDCount; USHORT HcdOriginalLength; };
        USHORT  HcdUrbFlags;
        PURB    HcdUrbLink;
    };
    struct _URB_HEADER {
        UCHAR               Length;
        UCHAR               Function;        /* URB_FUNCTION_* */
        USBD_STATUS         Status;          /* result, read after completion */
        PURB_COMPLETE_PROC  CompleteProc;    /* completion routine (or NULL) */
        PVOID               CompleteContext;
    };
    typedef struct _USB_CONTROL_SETUP_PACKET {
        UCHAR  bmRequestType;
        UCHAR  bRequest;
        USHORT wValue;
        USHORT wIndex;
        USHORT wLength;
    } USB_CONTROL_SETUP_PACKET;

    typedef struct _URB_CONTROL_TRANSFER {
        struct _URB_HEADER       Hdr;
        PVOID                    EndpointHandle;
        ULONG                    TransferBufferLength;
        PVOID                    TransferBuffer;
        UCHAR                    TransferDirection;
        BOOLEAN                  ShortTransferOK;
        UCHAR                    InterruptDelay;
        UCHAR                    Padding;
        struct _URB_HCD_AREA     Hca;
        USB_CONTROL_SETUP_PACKET SetupPacket;
    } URB_CONTROL_TRANSFER, * PURB_CONTROL_TRANSFER;

    typedef struct _URB_BULK_OR_INTERRUPT_TRANSFER {
        struct _URB_HEADER    Hdr;
        PVOID                 EndpointHandle;
        ULONG                 TransferBufferLength;
        PVOID                 TransferBuffer;
        UCHAR                 TransferDirection;
        BOOLEAN               ShortTransferOK;
        UCHAR                 InterruptDelay;
        UCHAR                 Padding;
        struct _URB_HCD_AREA  Hca;
    } URB_BULK_OR_INTERRUPT_TRANSFER, * PURB_BULK_OR_INTERRUPT_TRANSFER;

    typedef struct _URB_OPEN_ENDPOINT {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;
        UCHAR               FunctionAddress;
        UCHAR               EndpointAddress;
        UCHAR               EndpointType;
        UCHAR               Interval;
        PULONG              DataToggleBits;
        USHORT              MaxPacketSize;
        BOOLEAN             LowSpeed;
    } URB_OPEN_ENDPOINT, * PURB_OPEN_ENDPOINT;

    typedef struct _URB_CLOSE_ENDPOINT {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;
        PURB                HcdNextClose;
        PULONG              DataToggleBits;
    } URB_CLOSE_ENDPOINT, * PURB_CLOSE_ENDPOINT;

    typedef struct _URB_GET_SET_ENDPOINT_STATE {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;
        ULONG               EndpointState;
    } URB_GET_SET_ENDPOINT_STATE, * PURB_GET_SET_ENDPOINT_STATE;

    typedef struct _URB_ABORT_ENDPOINT {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;
        PURB                HcdNextAbort;
    } URB_ABORT_ENDPOINT, * PURB_ABORT_ENDPOINT;

    typedef struct _URB_RESET_PORT {
        struct _URB_HEADER  Hdr;
        UCHAR               DeviceNode;
        UCHAR               PortNumber;
    } URB_RESET_PORT, * PURB_RESET_PORT;

    typedef struct _URB_GET_FRAME_NUMBER {
        struct _URB_HEADER  Hdr;
        UCHAR               DeviceNode;
        ULONG               FrameNumber;
    } URB_GET_FRAME_NUMBER, * PURB_GET_FRAME_NUMBER;

    /* ---- iso (the camera frame path) ---- */
    typedef struct _USBD_ISOCH_PACKET_STATUS_WORD {
        USHORT BytesRead : 12;
        USHORT ConditionCode : 4;
    } USBD_ISOCH_PACKET_STATUS_WORD, * PUSBD_ISOCH_PACKET_STATUS_WORD;

    typedef struct _USBD_ISOCH_TRANSFER_STATUS {        /* <-- iso completion payload */
        USBD_STATUS                   Status;
        ULONG                         FrameCount;
        USBD_ISOCH_PACKET_STATUS_WORD PacketStatus[8];
    } USBD_ISOCH_TRANSFER_STATUS, * PUSBD_ISOCH_TRANSFER_STATUS;

    typedef VOID(*PFNUSBD_ISOCH_TRANSFER_COMPLETE)(PUSBD_ISOCH_TRANSFER_STATUS Status, PVOID Context);

    typedef struct _USBD_ISOCH_BUFFER_DESCRIPTOR {      /* attach this to receive frames */
        ULONG                           FrameCount;
        PVOID                           TransferBuffer;   /* MmAllocateContiguousMemory'd */
        USHORT                          Pattern[8];
        PFNUSBD_ISOCH_TRANSFER_COMPLETE TransferComplete; /* our frame callback */
        PVOID                           Context;
    } USBD_ISOCH_BUFFER_DESCRIPTOR, * PUSBD_ISOCH_BUFFER_DESCRIPTOR;

    typedef struct _URB_ISOCH_ATTACH_BUFFER {
        struct _URB_HEADER            Hdr;
        PVOID                         EndpointHandle;
        UCHAR                         InterruptDelay;
        PUSBD_ISOCH_BUFFER_DESCRIPTOR BufferDescriptor;
    } URB_ISOCH_ATTACH_BUFFER, * PURB_ISOCH_ATTACH_BUFFER;

    typedef struct _URB_ISOCH_START_TRANSFER {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;
        ULONG               FrameNumber;
        ULONG               Flags;
    } URB_ISOCH_START_TRANSFER, * PURB_ISOCH_START_TRANSFER;

    typedef struct _URB_ISOCH_STOP_TRANSFER {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;
    } URB_ISOCH_STOP_TRANSFER, * PURB_ISOCH_STOP_TRANSFER;

    typedef struct _URB_ISOCH_OPEN_ENDPOINT {
        struct _URB_HEADER  Hdr;
        PVOID               EndpointHandle;   /* <-- filled in by USBD; stash this */
        UCHAR               FunctionAddress;
        UCHAR               EndpointAddress;
        USHORT              MaxPacketSize;
        USHORT              Flags;
        USHORT              Pad;
    } URB_ISOCH_OPEN_ENDPOINT, * PURB_ISOCH_OPEN_ENDPOINT;

    typedef struct _URB_CLOSE_ENDPOINT URB_ISOCH_CLOSE_ENDPOINT, * PURB_ISOCH_CLOSE_ENDPOINT;

    /* ---- the union SubmitRequest takes (all arms, real names) ---- */
    typedef union _URB {
        struct _URB_HEADER             Header;
        URB_CONTROL_TRANSFER           ControlTransfer;
        URB_BULK_OR_INTERRUPT_TRANSFER BulkOrInterruptTransfer;
        URB_BULK_OR_INTERRUPT_TRANSFER CommonTransfer;
        URB_OPEN_ENDPOINT              OpenEndpoint;
        URB_CLOSE_ENDPOINT             CloseEndpoint;
        URB_GET_SET_ENDPOINT_STATE     GetSetEndpointState;
        URB_ABORT_ENDPOINT             AbortEndpoint;
        URB_RESET_PORT                 ResetPort;
        URB_GET_FRAME_NUMBER           GetFrame;
        URB_ISOCH_ATTACH_BUFFER        IsochAttachBuffer;
        URB_ISOCH_START_TRANSFER       IsochStartTransfer;
        URB_ISOCH_STOP_TRANSFER        IsochStopTransfer;
        URB_ISOCH_OPEN_ENDPOINT        IsochOpenEndpoint;
        URB_ISOCH_CLOSE_ENDPOINT       IsochCloseEndpoint;
    } URB;
    /* PURB already typedef'd above (union _URB *) */

    /* ---- USB_BUILD_* macros (folded from usb.h) -- the supported way to fill a URB.
     *      Always RtlZeroMemory(&urb,sizeof(URB)) first. ---- */
#define USB_BUILD_ISOCH_OPEN_ENDPOINT(_u_, _EndpointAddress_, _MaxPacketSize_, _Flags_) {\
    (_u_)->Hdr.Length=sizeof(URB_ISOCH_OPEN_ENDPOINT);\
    (_u_)->Hdr.Function=URB_FUNCTION_ISOCH_OPEN_ENDPOINT;\
    (_u_)->Hdr.CompleteProc=NULL; (_u_)->Hdr.CompleteContext=NULL;\
    (_u_)->EndpointAddress=(_EndpointAddress_);\
    (_u_)->MaxPacketSize=(_MaxPacketSize_);\
    (_u_)->Flags=(_Flags_); }

#define USB_BUILD_ISOCH_START_TRANSFER(_u_, _EndpointHandle_, _FrameNumber_, _Flags_) {\
    (_u_)->Hdr.Length=sizeof(URB_ISOCH_START_TRANSFER);\
    (_u_)->Hdr.Function=URB_FUNCTION_ISOCH_START_TRANSFER;\
    (_u_)->Hdr.CompleteProc=NULL; (_u_)->Hdr.CompleteContext=NULL;\
    (_u_)->EndpointHandle=(_EndpointHandle_);\
    (_u_)->FrameNumber=(_FrameNumber_); (_u_)->Flags=(_Flags_); }

#define USB_BUILD_ISOCH_ATTACH_BUFFER(_u_, _EndpointHandle_, _InterruptDelay_, _BufferDescriptor_) {\
    (_u_)->Hdr.Length=sizeof(URB_ISOCH_ATTACH_BUFFER);\
    (_u_)->Hdr.Function=URB_FUNCTION_ISOCH_ATTACH_BUFFER;\
    (_u_)->Hdr.CompleteProc=NULL; (_u_)->Hdr.CompleteContext=NULL;\
    (_u_)->EndpointHandle=(_EndpointHandle_);\
    (_u_)->InterruptDelay=(_InterruptDelay_);\
    (_u_)->BufferDescriptor=(_BufferDescriptor_); }

#define USB_BUILD_ISOCH_STOP_TRANSFER(_u_, _EndpointHandle_) {\
    (_u_)->Hdr.Length=sizeof(URB_ISOCH_STOP_TRANSFER);\
    (_u_)->Hdr.Function=URB_FUNCTION_ISOCH_STOP_TRANSFER;\
    (_u_)->Hdr.CompleteProc=NULL; (_u_)->Hdr.CompleteContext=NULL;\
    (_u_)->EndpointHandle=(_EndpointHandle_); }

     /* control transfer: (urb, ep=NULL default, buf, len, dir, completeProc, ctx,
      * shortOK, bmRequestType, bRequest, wValue, wIndex, wLength) */
#define USB_BUILD_CONTROL_TRANSFER(_u_, _EndpointHandle_, _TransferBuffer_, _TransferBufferLength_,\
        _TransferDirection_, _CompleteProc_, _CompleteContext_, _ShortTransferOK_,\
        _bmRequestType_, _bRequest_, _wValue_, _wIndex_, _wLength_) {\
    (_u_)->Hdr.Length=sizeof(URB_CONTROL_TRANSFER);\
    (_u_)->Hdr.Function=URB_FUNCTION_CONTROL_TRANSFER;\
    (_u_)->Hdr.CompleteProc=(_CompleteProc_); (_u_)->Hdr.CompleteContext=(_CompleteContext_);\
    (_u_)->EndpointHandle=(_EndpointHandle_);\
    (_u_)->TransferBuffer=(_TransferBuffer_); (_u_)->TransferBufferLength=(_TransferBufferLength_);\
    (_u_)->TransferDirection=(_TransferDirection_); (_u_)->ShortTransferOK=(_ShortTransferOK_);\
    (_u_)->InterruptDelay=USBD_DELAY_INTERRUPT_0_MS;\
    (_u_)->SetupPacket.bmRequestType=(_bmRequestType_); (_u_)->SetupPacket.bRequest=(_bRequest_);\
    (_u_)->SetupPacket.wValue=(_wValue_); (_u_)->SetupPacket.wIndex=(_wIndex_);\
    (_u_)->SetupPacket.wLength=(_wLength_); }



    typedef struct _USB_RESOURCE_REQUIREMENTS  USB_RESOURCE_REQUIREMENTS, * PUSB_RESOURCE_REQUIREMENTS;   /* [V] opaque */
    typedef struct _HCD_RESOURCE_REQUIREMENTS  HCD_RESOURCE_REQUIREMENTS, * PHCD_RESOURCE_REQUIREMENTS;   /* [V] opaque */

    /* The class-driver descriptor (one entry in the table at 0x1b2774).
     * Layout CONFIRMED from decomp (BUILD_SPEC §3.5):
     *   [0]=class  [1]=subclass  [+4]=Register(fn)  [+8]=Attach(fn)  */
    typedef struct _USB_CLASS_DRIVER_DESCRIPTION {
        UCHAR  bClass;                                  /* [+0] match: class    */
        UCHAR  bSubClass;                               /* [+1] match: subclass */
        USHORT wReserved;                               /* [+2] [V] padding     */
        VOID(__cdecl* Register)(PVOID initContext);    /* [+4] called by XInitDevices for each entry */
        VOID(__stdcall* Attach)(PVOID usbDevice);      /* [+8] called by USBD_LoadClassDriver on match */
        /* further fields per RE / RXDK header -- [V] */
    } USB_CLASS_DRIVER_DESCRIPTION, * PUSB_CLASS_DRIVER_DESCRIPTION;

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ------------------------------------------------------------------ *
 *  The interfaces. These are CONCRETE __thiscall classes (no vftable symbols
 *  found -> not virtual), so we declare matching non-virtual methods. The
 *  compiler mangles each call to the exact symbol shown; the linker binds it to
 *  xapilib.lib. Do NOT add/remove/reorder members that change mangling.
 *
 *  Signatures decoded from the mangled names (MSVC scheme):
 *    Q = public, A=non-const / B=const, E=__thiscall;
 *    J=long, K=ulong, E=uchar, X=void, H=int, PA=ptr, PB=const ptr,
 *    T=union, U=struct, V=class, @Z ends arg list, XZ = (void).
 * ------------------------------------------------------------------ */
#ifdef __cplusplus

class IUsbDevice {
public:
    /* ??0IUsbDevice@@QAE@XZ */
    IUsbDevice();

    /* ?SubmitRequest@IUsbDevice@@QAEJPAT_URB@@@Z
       public: long __thiscall SubmitRequest(union _URB *) */
    LONG  SubmitRequest(_URB* urb);

    /* ?CancelRequest@IUsbDevice@@QAEJPAT_URB@@@Z */
    LONG  CancelRequest(_URB* urb);

    /* ?GetDeviceDescriptor@IUsbDevice@@QBEPBU_USB_DEVICE_DESCRIPTOR8@@XZ
       public: struct _USB_DEVICE_DESCRIPTOR8 const* __thiscall (void) const */
    const USB_DEVICE_DESCRIPTOR8* GetDeviceDescriptor() const;

    /* ?GetConfigurationDescriptor@IUsbDevice@@QBEPBU_USB_CONFIGURATION_DESCRIPTOR@@XZ */
    const USB_CONFIGURATION_DESCRIPTOR* GetConfigurationDescriptor() const;

    /* ?GetInterfaceDescriptor@IUsbDevice@@QBEPBU_USB_INTERFACE_DESCRIPTOR@@XZ */
    const USB_INTERFACE_DESCRIPTOR* GetInterfaceDescriptor() const;

    /* ?GetEndpointDescriptor@IUsbDevice@@QBEPBU_USB_ENDPOINT_DESCRIPTOR@@EEE@Z
       (uchar, uchar, uchar) const -- (interface, alt, endpoint) */
    const USB_ENDPOINT_DESCRIPTOR* GetEndpointDescriptor(UCHAR iface, UCHAR alt, UCHAR ep) const;

    /* ?OpenDefaultEndpoint@IUsbDevice@@AAEJPAT_URB@@@Z  (private: AAE) */
    /* ?CloseDefaultEndpoint@IUsbDevice@@AAEJPAT_URB@@@Z (private: AAE) */

    /* ?GetExtension@IUsbDevice@@QBEPAXXZ  -> void* (void) const */
    void* GetExtension() const;
    /* ?SetExtension@IUsbDevice@@QAEPAXPAX@Z -> void* (void*) */
    void* SetExtension(void* ext);

    /* ?GetClassId@IUsbDevice@@QBE... (stash our per-device state via Get/SetExtension) */

    /* ?SetClassSpecificType@IUsbDevice@@QAEXE@Z -> void (uchar) */
    void  SetClassSpecificType(UCHAR type);

    /* tree / topology (return IUsbDevice*  == PAV1) */
    IUsbDevice* GetParent() const;          /* ?GetParent@@QBEPAV1@XZ */
    IUsbDevice* GetFirstChild() const;      /* ?GetFirstChild@@QBEPAV1@XZ */
    IUsbDevice* GetSibling() const;         /* ?GetSibling@@QBEPAV1@XZ */
    IUsbDevice* FindChild(UCHAR port) const;/* ?FindChild@@QBEPAV1@E@Z */
    void        InsertChild(IUsbDevice* child);     /* ?InsertChild@@QAEXPAV1@@Z */
    UCHAR       RemoveChild(IUsbDevice* child);     /* ?RemoveChild@@QAEEPAV1@@Z */

    /* status / topology getters */
    ULONG GetPort() const;                  /* ?GetPort@@QBEKXZ  -> ulong */
    UCHAR GetHubPort() const;               /* ?GetHubPort@@QBEEXZ */
    UCHAR GetInterfaceNumber() const;       /* ?GetInterfaceNumber@@QBEEXZ */
    UCHAR GetLowSpeed() const;              /* ?GetLowSpeed@@QBEEXZ */
    UCHAR IsEnumTime() const;               /* ?IsEnumTime@@QBEEXZ */
    UCHAR IsHardwareConnected() const;      /* ?IsHardwareConnected@@QBEEXZ */

    /* connect / disconnect / completion plumbing */
    void  DeviceConnected(UCHAR a, UCHAR b);/* ?DeviceConnected@@QAEXEE@Z */
    void  DeviceDisconnected(UCHAR a);      /* ?DeviceDisconnected@@QAEXE@Z */
    void  DeviceNotResponding();            /* ?DeviceNotResponding@@QAEXXZ */
    void  AddComplete(LONG status);         /* ?AddComplete@@QAEXJ@Z */
    void  RemoveComplete();                 /* ?RemoveComplete@@QAEXXZ */
    void  DisableComplete(LONG s, void* p); /* ?DisableComplete@@QAEXJPAX@Z */
    void  ResetComplete(LONG s, void* p);   /* ?ResetComplete@@QAEXJPAX@Z */
    void  SetExternalPort();                /* ?SetExternalPort@@QAEXXZ */

    /* statics (SG = static __stdcall) */
    static LONG  NtStatusFromUsbdStatus(LONG usbdStatus);  /* ?...@@SGJJ@Z */
    static ULONG Win32FromUsbdStatus(LONG usbdStatus);     /* ?...@@SGKJ@Z */
    static void  IncrementBusyHubCount();                  /* ?...@@SGXXZ */
    static void  DecrementBusyHubCount();                  /* ?...@@SGXXZ */
};

class IUsbInit {
public:
    /* ??0IUsbInit@@QAE@KPAU_XDEVICE_PREALLOC_TYPE@@@Z
       IUsbInit(unsigned long count, _XDEVICE_PREALLOC_TYPE *types) */
    IUsbInit(ULONG count, PXDEVICE_PREALLOC_TYPE types);

    /* ?RegisterResources@IUsbInit@@QAEXPAU_USB_RESOURCE_REQUIREMENTS@@@Z */
    void  RegisterResources(PUSB_RESOURCE_REQUIREMENTS req);

    /* ?Process@IUsbInit@@QAEXXZ */
    void  Process();

    /* ?GetHcdResourcePtr@IUsbInit@@QAEPAU_HCD_RESOURCE_REQUIREMENTS@@XZ */
    PHCD_RESOURCE_REQUIREMENTS GetHcdResourcePtr();

    /* ?GetMaxDeviceTypeCount@IUsbInit@@QAEKPAU_XPP_DEVICE_TYPE@@@Z  (ulong)(XPP_DEVICE_TYPE*) */
    ULONG GetMaxDeviceTypeCount(PXPP_DEVICE_TYPE types);

    UCHAR GetMaxCompositeInterfaces();      /* ?...@@QAEEXZ */
    UCHAR GetNodeCount();                   /* ?...@@QAEEXZ */
    int   UseDefaultCount();                /* ?...@@QAEHXZ */
};

#endif /* __cplusplus */

/* ------------------------------------------------------------------ *
 *  C entry point (from usbxapi.h) -- what XInitDevices wraps.
 *    EXTERNUSB VOID USBD_Init(DWORD NumDeviceTypes, PXDEVICE_PREALLOC_TYPE);
 * ------------------------------------------------------------------ */
#ifdef __cplusplus
extern "C" {
#endif
    VOID USBD_Init(DWORD NumDeviceTypes, PXDEVICE_PREALLOC_TYPE DeviceTypes);
#ifdef __cplusplus
}
#endif

/* ------------------------------------------------------------------ *
 *  USBD framework functions -- signatures CONFIRMED from usbd.lib mangling.
 *  These are the enumeration + class-driver machinery (BUILD_SPEC §3.5).
 *  Calling convention from the mangle: YG = __stdcall, YI = __fastcall.
 *  Declared so a class driver can reference them; the camera driver mainly
 *  needs the IUsbDevice interface, but these document the load/attach path.
 * ------------------------------------------------------------------ */
#ifdef __cplusplus
extern "C" {
#endif

    /* ?USBD_FindClassDriver@@YGPAU_USB_CLASS_DRIVER_DESCRIPTION@@T_PNP_CLASS_ID@@@Z
       __stdcall, returns the matching class-driver descriptor (or NULL). */
    PUSB_CLASS_DRIVER_DESCRIPTION __stdcall USBD_FindClassDriver(PNP_CLASS_ID classId);

    /* ?USBD_LoadClassDriver@@YIXPAVIUsbDevice@@T_PNP_CLASS_ID@@@Z
       __fastcall. Finds the class driver for classId and calls its attach (+8). */
       /* (declared in C++ section below where IUsbDevice is a type) */

       /* ?USBD_SubmitSynchronousRequestComplete@@YGXPAT_URB@@PAU_KEVENT@@@Z
          __stdcall completion that signals the KEVENT -- the event-wait pattern the
          control URB (+0x08 callback / +0x0C context) uses for synchronous transfers.
          NOTE: _URB is a union (mangled T_URB); _KEVENT typedef'd above. */
    void __stdcall USBD_SubmitSynchronousRequestComplete(URB* urb, KEVENT* evt);

#ifdef __cplusplus
}  /* extern "C" */

/* C++ -only prototypes that take IUsbDevice* (PAVIUsbDevice).
   Enumeration stages each take (_URB*, IUsbDevice*); confirmed from usbd.lib:
     ?USBD_DeviceEnumStage2/3/5/6 / StagePre4 @@YGX PAT_URB@@ PAVIUsbDevice@@ @Z
     ?USBD_fDeviceEnumStage4 @@YIX ...   (__fastcall)
   Declared for reference; the class driver does not call these directly. */
extern "C" void __fastcall USBD_LoadClassDriver(IUsbDevice* dev, PNP_CLASS_ID classId);

/* usbtest.lib helper -- bridges a device-type+port to an IUsbDevice (useful in
   attach/diagnostics):
   ?GetXidDeviceInterface@@YGPAVIUsbDevice@@PAU_XPP_DEVICE_TYPE@@KK@Z */
extern "C" IUsbDevice* __stdcall GetXidDeviceInterface(PXPP_DEVICE_TYPE type, ULONG a, ULONG b);

#endif /* __cplusplus */

#endif /* XBOX_USB_H */