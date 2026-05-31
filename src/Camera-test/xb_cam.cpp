/*
 * xbcam.cpp -- Original Xbox USB camera class driver (skeleton)
 * Team Resurgent / Darkone83
 *
 * Structure modeled on the official XDK sample class driver (usbsamp.cpp) +
 * the iso streaming recipe from the SLIX/hawk drivers + the "Writing USB Class
 * Drivers for Xbox" guide. Targets the lean Xbox IUsbDevice path (NOT WDM/USBCAMD).
 *
 * Drives the Xbox Live Vision cam (VID 0x045E/PID 0x028C) and Sony EyeToy
 * (VID 0x054C/PID 0x0155), both Vendor-Specific class (0xFF) iso-video devices.
 *
 * CONFIDENCE MARKERS:
 *   (plain code)        -- confident; RXDK-provided, guide/source-confirmed.
 *   // RXDK: verify     -- API/type we BELIEVE RXDK exposes; if the linker says
 *                          undefined, it's a re-implementation candidate.
 *   // HW: verify       -- empirical; only real hardware confirms the value/behavior.
 *
 * This is a STRUCTURE, not a finished driver. It is meant to compile against
 * xbox_usb.h and then be thrown at the RXDK toolchain to generate the
 * undefined-symbol list = the re-implementation to-do list.
 */

#include <xtl.h>            // RXDK umbrella (always present)
#include "xbox_kernel.h"    // Ke*/Mm*/DbgPrint/KEVENT/ASSERT (shim: real DDK header or fallback decls)
#include "xbox_usb.h"       // our self-contained USB framework header

 // ---------------------------------------------------------------------------
 // Tunables (HW: verify all of these against the real device)
 // ---------------------------------------------------------------------------
#define CAM_VID_XBOX        0x045E
#define CAM_PID_XBOX        0x028C
#define CAM_VID_EYETOY      0x054C
#define CAM_PID_EYETOY      0x0155

// From the EEPROM descriptor dump: iface 0, class 0xFF, iso IN endpoint 0x81,
// alt-settings 0(idle)/1..4 = 384/512/768/896 bytes.
#define CAM_INTERFACE_CLASS     0xFF      // Vendor Specific (the match key)
#define CAM_ISO_ENDPOINT        0x81      // EP1 IN  (HW: verify on EyeToy)
#define CAM_STREAM_ALT          4         // alt-setting to stream at (896B)  // HW: verify best fit
#define CAM_IDLE_ALT            0

// Frame geometry -- default 320x240 RGB24. HW: verify the OV519 actually delivers this.
#define CAM_WIDTH               320
#define CAM_HEIGHT              240
#define CAM_BPP                 3                              // RGB24
#define CAM_FRAME_BYTES         (CAM_WIDTH * CAM_HEIGHT * CAM_BPP)   // 230400

#define CAM_NUM_FRAME_BUFFERS   2         // double-buffer; HW: tune (vs MaxIsochMaxBuffers)
#define CAM_MAX_DEVICES         1

// ---------------------------------------------------------------------------
// Registration -- the .XPP$Class linker-segment pattern (from usbsamp.cpp).
// The macros + USB_RESOURCE_REQUIREMENTS + the descriptor now come from
// xbox_usb.h (folded from usb.x). We DECLARE OUR OWN device type since there is
// no predefined XDEVICE_TYPE_CAMERA.
// ---------------------------------------------------------------------------
DECLARE_XPP_TYPE(XbCameraType)                     // creates XbCameraType_TABLE

USB_DEVICE_TYPE_TABLE_BEGIN(Cam)
USB_DEVICE_TYPE_TABLE_ENTRY(&XbCameraType_TABLE)
USB_DEVICE_TYPE_TABLE_END()

// Bind {class, subclass, protocol} -> this driver. Match Vendor-Specific 0xFF,
// wildcard subclass/protocol. Declares CamInit/CamAddDevice/CamRemoveDevice.
USB_CLASS_DRIVER_DECLARATION(Cam, CAM_INTERFACE_CLASS, 0xFF, 0xFF)

#pragma data_seg(".XPP$ClassCam")
USB_CLASS_DECLARATION_POINTER(Cam)
#pragma data_seg(".XPP$Data")

// ---------------------------------------------------------------------------
// Per-device state (modeled on SAMPLE_DEVICE_STATE + SLIX DEVICE_EXTENSION).
// Allocated up-front as a global array (XID model) to avoid dynamic alloc and
// the full close/remove state-machine complexity.
// ---------------------------------------------------------------------------
typedef struct _CAM_DEVICE_STATE {
    IUsbDevice* Device;

    BOOLEAN DeviceAttached : 1;
    BOOLEAN DeviceReady : 1;
    BOOLEAN Streaming : 1;
    BOOLEAN DeviceRemoved : 1;
    BOOLEAN RemovePending : 1;
    BOOLEAN ClosePending : 1;

    UCHAR   InterfaceNumber;
    UCHAR   IsoEndpointAddress;     // 0x81
    PVOID   IsoEndpointHandle;      // filled by ISOCH_OPEN_ENDPOINT

    // iso buffers (must be physically contiguous for DMA)
    PVOID   FrameBuffer[CAM_NUM_FRAME_BUFFERS];
    USBD_ISOCH_BUFFER_DESCRIPTOR BufferDesc[CAM_NUM_FRAME_BUFFERS];
    ULONG   WriteIndex;            // which buffer the HW is filling
    ULONG   ReadIndex;             // last completed buffer (for the app to grab)

    // reused for enumeration-time control requests + teardown
    URB     EnumUrb;
    URB     IsoUrb;
    URB     CloseUrb;
    KEVENT  CloseEvent;            // RXDK: verify KEVENT/KeInitializeEvent
} CAM_DEVICE_STATE, * PCAM_DEVICE_STATE;

static CAM_DEVICE_STATE g_CamState[CAM_MAX_DEVICES];   // HW: 4 if you index by port

// Enumeration is serialized, so one shared enum URB is fine (usbsamp pattern).
static URB g_CamEnumUrb;

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
static VOID CamIsoComplete(PUSBD_ISOCH_TRANSFER_STATUS Status, PVOID Context);  // iso frame callback
static VOID CamCloseEndpointsAsync(PCAM_DEVICE_STATE st);

// ===========================================================================
// CamInit  -- the "<Class>Init" entry point. Called during XInitDevices,
//             before hardware init. Register resource requirements.
// ===========================================================================
EXTERNUSB VOID CamInit(IUsbInit* UsbInit)                       // RXDK: verify EXTERNUSB
{
    USB_RESOURCE_REQUIREMENTS req;                              // from the guide
    DWORD i;

    DbgPrint("XBCAM: CamInit\n");                               // RXDK: verify DbgPrint

    // Up-front per-device init (events, etc.)
    for (i = 0; i < CAM_MAX_DEVICES; i++) {
        KeInitializeEvent(&g_CamState[i].CloseEvent,
            NotificationEvent, FALSE);             // RXDK: verify
    }

    // Fill resource requirements for an iso video device.
    // Field layout from the official guide (USB_RESOURCE_REQUIREMENTS).
    RtlZeroMemory(&req, sizeof(req));                           // RXDK: verify RtlZeroMemory
    req.ConnectorType = USB_CONNECTOR_TYPE_HIGH_POWER;   // RXDK: verify constant  // HW: verify
    req.MaxDevices = CAM_MAX_DEVICES;
    req.MaxCompositeInterfaces = 1;        // EyeToy is composite(3); we bind iface 0 only // HW: verify
    req.MaxControlEndpoints = 0;          // we use only the default control endpoint
    req.MaxBulkEndpoints = 0;
    req.MaxInterruptEndpoints = 0;
    req.MaxControlTDperTransfer = 0;
    req.MaxBulkTDperTransfer = 0;
    req.MaxIsochEndpoints = 1;          // the one iso video EP
    req.MaxIsochMaxBuffers = CAM_NUM_FRAME_BUFFERS;          // HW: tune

    UsbInit->RegisterResources(&req);      // pointer not cached; stack OK (guide)
}

// ===========================================================================
// CamAddDevice -- the "<Class>AddDevice" entry point. Called at DPC level when
//                 a matching device connects. MUST NOT BLOCK. Must AddComplete.
// ===========================================================================
EXTERNUSB VOID CamAddDevice(IUsbDevice* Device)
{
    DWORD dwPort;
    PCAM_DEVICE_STATE st;
    const USB_INTERFACE_DESCRIPTOR* ifd;
    const USB_ENDPOINT_DESCRIPTOR* epd;

    DbgPrint("XBCAM: CamAddDevice\n");
    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);              // RXDK: verify

    // --- port sanity (usbsamp pattern) ---
    dwPort = Device->GetPort();
    if (dwPort >= CAM_MAX_DEVICES) {                            // HW: if indexing by port use XGetPortCount()
        Device->AddComplete(USBD_STATUS_UNSUPPORTED_DEVICE);
        return;
    }
    st = &g_CamState[dwPort];

    if (st->DeviceAttached) {
        Device->AddComplete(USBD_STATUS_UNSUPPORTED_DEVICE);
        return;
    }

    // --- confirm this is our device (enumeration-time getters only) ---
    // NOTE: GetDeviceDescriptor() returns only 8 bytes -- VID/PID are NOT in it.
    // Match by interface class instead (which IS available here), or send a full
    // GET_DESCRIPTOR if you need VID/PID. We match by the vendor class.
    ifd = Device->GetInterfaceDescriptor();
    if (ifd == NULL || ifd->bInterfaceClass != CAM_INTERFACE_CLASS) {
        Device->AddComplete(USBD_STATUS_UNSUPPORTED_DEVICE);
        return;
    }
    st->InterfaceNumber = ifd->bInterfaceNumber;

    // --- find the iso IN endpoint (type, direction, index) ---
    // CORRECTED signature per the guide: (EndpointType, Direction, Index).
    epd = Device->GetEndpointDescriptor(USB_ENDPOINT_TYPE_ISOCHRONOUS, 1 /*IN*/, 0);
    if (epd == NULL) {
        DbgPrint("XBCAM: no iso endpoint\n");
        Device->AddComplete(USBD_STATUS_UNSUPPORTED_DEVICE);
        return;
    }
    st->IsoEndpointAddress = epd->bEndpointAddress;            // expect 0x81  // HW: verify

    // --- cache + register ---
    st->Device = Device;
    st->DeviceAttached = TRUE;
    Device->SetClassSpecificType(0);       // ordinal into our type table
    Device->SetExtension(st);              // retrieve via GetExtension in Remove

    // Nothing async needed at enum time for this device, so complete now.
    // (If we needed control requests here, we'd cascade completions and call
    //  AddComplete from the last one -- see the guide. Always set a watchdog.)
    Device->AddComplete(USBD_STATUS_SUCCESS);
}

// ===========================================================================
// CamRemoveDevice -- the "<Class>RemoveDevice" entry point. DPC level, no block.
//                    Close endpoints (async state machine), then RemoveComplete.
// ===========================================================================
EXTERNUSB VOID CamRemoveDevice(IUsbDevice* Device)
{
    PCAM_DEVICE_STATE st = (PCAM_DEVICE_STATE)Device->GetExtension();

    DbgPrint("XBCAM: CamRemoveDevice\n");
    if (st == NULL) { Device->RemoveComplete(); return; }

    st->DeviceRemoved = TRUE;

    // If streaming, tear the iso endpoint down (async). For the simple model we
    // stop+close and then RemoveComplete from the close completion chain.
    if (st->Streaming || st->IsoEndpointHandle) {
        CamCloseEndpointsAsync(st);        // calls RemoveComplete when done
    }
    else {
        st->DeviceAttached = FALSE;
        Device->RemoveComplete();          // after this, Device is invalid
    }
}

// ===========================================================================
// CamStartCapture -- open iso EP, SET_INTERFACE to a streaming alt, start, attach
//                    buffers. This is the iso recipe (SLIX/hawk), the camera path.
//                    NOT an entry point -- called by our open/start API.
// ===========================================================================
static LONG CamStartCapture(PCAM_DEVICE_STATE st)
{
    LONG status;
    ULONG i;

    if (st->Streaming) return USBD_STATUS_SUCCESS;

    // 1) Allocate contiguous frame buffers (DMA target). HW: tune count/size.
    for (i = 0; i < CAM_NUM_FRAME_BUFFERS; i++) {
        st->FrameBuffer[i] = MmAllocateContiguousMemory(CAM_FRAME_BYTES);  // RXDK: verify
        if (st->FrameBuffer[i] == NULL) {
            // unwind
            while (i--) { MmFreeContiguousMemory(st->FrameBuffer[i]); st->FrameBuffer[i] = NULL; }
            return USBD_STATUS_NO_MEMORY;
        }
    }

    // 2) SET_INTERFACE to the streaming alt-setting (bandwidth). On Xbox this is
    //    a control transfer; build with the macro. (Could also be a framework
    //    alt-select call -- HW: verify which the stack wants.)
    RtlZeroMemory(&st->IsoUrb, sizeof(URB));
    USB_BUILD_CONTROL_TRANSFER(&st->IsoUrb.ControlTransfer,
        NULL,                                  // default endpoint
        NULL, 0,                               // no data stage
        USB_TRANSFER_DIRECTION_OUT,
        NULL, NULL,                            // synchronous (no callback)
        TRUE,
        (USB_HOST_TO_DEVICE | USB_STANDARD_COMMAND | USB_COMMAND_TO_INTERFACE),
        USB_REQUEST_SET_INTERFACE,
        CAM_STREAM_ALT,                        // wValue = alt setting
        st->InterfaceNumber,                   // wIndex = interface
        0);
    status = st->Device->SubmitRequest(&st->IsoUrb);
    if (USBD_ERROR(status)) return status;     // HW: verify

    // 3) Open the iso endpoint -> grab the handle.
    RtlZeroMemory(&st->IsoUrb, sizeof(URB));
    USB_BUILD_ISOCH_OPEN_ENDPOINT(&st->IsoUrb.IsochOpenEndpoint,
        st->IsoEndpointAddress, 0 /*maxPacket: 0=from descriptor*/, 0);  // HW: verify maxPacket arg
    status = st->Device->SubmitRequest(&st->IsoUrb);
    if (USBD_ERROR(status)) return status;
    st->IsoEndpointHandle = st->IsoUrb.IsochOpenEndpoint.EndpointHandle;

    // 4) Start the iso stream.
    RtlZeroMemory(&st->IsoUrb, sizeof(URB));
    USB_BUILD_ISOCH_START_TRANSFER(&st->IsoUrb.IsochStartTransfer,
        st->IsoEndpointHandle, 0, URB_FLAG_ISOCH_START_ASAP);
    status = st->Device->SubmitRequest(&st->IsoUrb);
    if (USBD_ERROR(status)) return status;

    // 5) Attach the frame buffers; each completion delivers a frame.
    for (i = 0; i < CAM_NUM_FRAME_BUFFERS; i++) {
        RtlZeroMemory(&st->BufferDesc[i], sizeof(USBD_ISOCH_BUFFER_DESCRIPTOR));
        st->BufferDesc[i].FrameCount = 1;                 // HW: verify (frames per buffer)
        st->BufferDesc[i].TransferBuffer = st->FrameBuffer[i];
        st->BufferDesc[i].TransferComplete = CamIsoComplete;
        st->BufferDesc[i].Context = st;

        RtlZeroMemory(&st->IsoUrb, sizeof(URB));
        USB_BUILD_ISOCH_ATTACH_BUFFER(&st->IsoUrb.IsochAttachBuffer,
            st->IsoEndpointHandle, USBD_DELAY_INTERRUPT_0_MS, &st->BufferDesc[i]);
        status = st->Device->SubmitRequest(&st->IsoUrb);
        if (USBD_ERROR(status)) return status;
    }

    st->Streaming = TRUE;
    return USBD_STATUS_SUCCESS;
}

// ===========================================================================
// CamIsoComplete -- iso frame completion callback. Called per completed buffer.
//                   Runs at raised IRQL: keep it FAST and NON-BLOCKING.
// ===========================================================================
static VOID CamIsoComplete(PUSBD_ISOCH_TRANSFER_STATUS Status, PVOID Context)
{
    PCAM_DEVICE_STATE st = (PCAM_DEVICE_STATE)Context;
    ULONG buf;

    if (st->DeviceRemoved) return;

    // Status->Status / Status->PacketStatus[] tell us per-frame health.
    if (USBD_SUCCESS(Status->Status)) {
        // HW: the frame is now in one of st->FrameBuffer[]. Real per-packet
        // reassembly (where frame boundaries are, any OV519 packet headers) is
        // the one genuinely empirical piece -- determine it by inspecting what
        // actually arrives. For now: advance the read index so the app can grab.
        st->ReadIndex = st->WriteIndex;
        st->WriteIndex = (st->WriteIndex + 1) % CAM_NUM_FRAME_BUFFERS;
        // CamPublishFrame(st->FrameBuffer[st->ReadIndex]);   // -> Phase 6 display / Phase 5 UDP
    }

    // Re-attach this buffer to keep the stream going (continuous capture).
    if (st->Streaming && !st->DeviceRemoved) {
        buf = st->WriteIndex;
        RtlZeroMemory(&st->BufferDesc[buf], sizeof(USBD_ISOCH_BUFFER_DESCRIPTOR));
        st->BufferDesc[buf].FrameCount = 1;
        st->BufferDesc[buf].TransferBuffer = st->FrameBuffer[buf];
        st->BufferDesc[buf].TransferComplete = CamIsoComplete;
        st->BufferDesc[buf].Context = st;

        RtlZeroMemory(&st->IsoUrb, sizeof(URB));     // HW: a single shared IsoUrb in the
        // completion path may race; may need
        // a per-buffer URB. Verify under load.
        USB_BUILD_ISOCH_ATTACH_BUFFER(&st->IsoUrb.IsochAttachBuffer,
            st->IsoEndpointHandle, USBD_DELAY_INTERRUPT_0_MS, &st->BufferDesc[buf]);
        st->Device->SubmitRequest(&st->IsoUrb);
    }
}

// ===========================================================================
// CamStopCapture -- stop the iso stream, free buffers. (Phase teardown.)
// ===========================================================================
static LONG CamStopCapture(PCAM_DEVICE_STATE st)
{
    ULONG i;
    if (!st->Streaming) return USBD_STATUS_SUCCESS;
    st->Streaming = FALSE;

    if (st->IsoEndpointHandle) {
        RtlZeroMemory(&st->IsoUrb, sizeof(URB));
        USB_BUILD_ISOCH_STOP_TRANSFER(&st->IsoUrb.IsochStopTransfer, st->IsoEndpointHandle);
        st->Device->SubmitRequest(&st->IsoUrb);     // HW: verify sync vs async here
    }

    // SET_INTERFACE back to idle (alt 0) to release bandwidth.
    RtlZeroMemory(&st->IsoUrb, sizeof(URB));
    USB_BUILD_CONTROL_TRANSFER(&st->IsoUrb.ControlTransfer,
        NULL, NULL, 0, USB_TRANSFER_DIRECTION_OUT, NULL, NULL, TRUE,
        (USB_HOST_TO_DEVICE | USB_STANDARD_COMMAND | USB_COMMAND_TO_INTERFACE),
        USB_REQUEST_SET_INTERFACE, CAM_IDLE_ALT, st->InterfaceNumber, 0);
    st->Device->SubmitRequest(&st->IsoUrb);

    for (i = 0; i < CAM_NUM_FRAME_BUFFERS; i++) {
        if (st->FrameBuffer[i]) {
            MmFreeContiguousMemory(st->FrameBuffer[i]);
            st->FrameBuffer[i] = NULL;
        }
    }
    return USBD_STATUS_SUCCESS;
}

// ===========================================================================
// CamCloseEndpointsAsync -- teardown helper for RemoveDevice. Simplified: stop,
//   close iso endpoint, free, then RemoveComplete. (Guide's full close/remove
//   state machine with pending flags would go here if we needed open/close API.)
// ===========================================================================
static VOID CamCloseEndpointsAsync(PCAM_DEVICE_STATE st)
{
    IUsbDevice* dev = st->Device;

    CamStopCapture(st);

    if (st->IsoEndpointHandle) {
        // ISOCH close is async; for the skeleton we issue it and proceed.
        RtlZeroMemory(&st->CloseUrb, sizeof(URB));
        st->CloseUrb.IsochCloseEndpoint.Hdr.Length = sizeof(URB_ISOCH_CLOSE_ENDPOINT);
        st->CloseUrb.IsochCloseEndpoint.Hdr.Function = URB_FUNCTION_ISOCH_CLOSE_ENDPOINT;
        st->CloseUrb.IsochCloseEndpoint.EndpointHandle = st->IsoEndpointHandle;
        dev->SubmitRequest(&st->CloseUrb);          // HW: should cascade RemoveComplete
        // from the close completion routine.
        st->IsoEndpointHandle = NULL;
    }

    st->DeviceAttached = FALSE;
    dev->RemoveComplete();                          // after this, dev is invalid
}

// ===========================================================================
// Public API the harness (cameratest.cpp) calls -- XCam_*.
// Bridges the harness's synchronous Init/Draw/Shutdown contract onto the async
// class-driver internals. The driver's CamAddDevice has (or hasn't) attached a
// device by the time XCam_Init runs; we act on that state.
// Titles run in kernel mode, so these are plain library calls (no IOCTLs).
// ===========================================================================
#include "xb_cam.h"

// XCam_Init -- start the camera on dwPort. Returns 0 on success,
// XCAM_STATUS_NO_DEVICE if nothing attached, XCAM_STATUS_OPEN_FAILED if bring-up failed.
extern "C" int XCam_Init(DWORD dwPort)
{
    LONG s;
    if (dwPort >= CAM_MAX_DEVICES)               // HW: if indexing by port, use XGetPortCount()
        return XCAM_STATUS_NO_DEVICE;
    if (!g_CamState[dwPort].DeviceAttached)      // CamAddDevice hasn't seen a camera
        return XCAM_STATUS_NO_DEVICE;
    s = CamStartCapture(&g_CamState[dwPort]);
    if (USBD_ERROR(s))
        return XCAM_STATUS_OPEN_FAILED;
    return XCAM_STATUS_OK;
}

// XCam_Shutdown -- stop capture on whichever port is streaming.
extern "C" void XCam_Shutdown(void)
{
    ULONG p;
    for (p = 0; p < CAM_MAX_DEVICES; p++) {
        if (g_CamState[p].Streaming)
            CamStopCapture(&g_CamState[p]);
    }
}

// XCam_IsStreaming -- nonzero if any port is streaming.
extern "C" int XCam_IsStreaming(void)
{
    ULONG p;
    for (p = 0; p < CAM_MAX_DEVICES; p++) {
        if (g_CamState[p].Streaming)
            return 1;
    }
    return 0;
}

// XCam_DrawToSurface -- copy the most-recent frame into the harness's texture.
// The harness creates a D3DFMT_YUY2 texture (XCAM_FRAME_W x XCAM_FRAME_H).
// NOTE: the camera's native format (RGB24 per the EEPROM vs YUY2 the harness
// expects) is a HW: verify item -- this is where any conversion goes once the
// real on-wire format is known. For now we copy raw bytes row-by-row.
extern "C" int XCam_DrawToSurface(IDirect3DTexture8* pTex)
{
    PCAM_DEVICE_STATE st = NULL;
    ULONG p;
    D3DLOCKED_RECT lr;                            // RXDK: verify D3D types via <xtl.h>
    BYTE* src, * dst;
    ULONG y, copyBytes;

    if (pTex == NULL) return -1;

    for (p = 0; p < CAM_MAX_DEVICES; p++) {
        if (g_CamState[p].Streaming) { st = &g_CamState[p]; break; }
    }
    if (st == NULL) return XCAM_STATUS_NO_DEVICE;

    src = (BYTE*)st->FrameBuffer[st->ReadIndex];
    if (src == NULL) return -1;

    if (FAILED(pTex->LockRect(0, &lr, NULL, 0)))  // RXDK: verify IDirect3DTexture8::LockRect
        return -1;

    // HW: row stride + format. Harness texture is YUY2 (2 bytes/pixel = 640B/row);
    // our frame buffer is currently sized RGB24 (3 bytes/pixel). The correct copy
    // depends on the real delivered format -- this is the conversion seam.
    // Conservative raw copy of min(row bytes); REPLACE once format is confirmed.
    copyBytes = XCAM_FRAME_W * 2;                 // YUY2 dest row; HW: verify
    dst = (BYTE*)lr.pBits;
    for (y = 0; y < XCAM_FRAME_H; y++) {
        // HW: this assumes a contiguous frame; real per-packet layout may differ.
        memcpy(dst, src, copyBytes);              // RXDK: verify memcpy (or RtlCopyMemory)
        dst += lr.Pitch;
        src += copyBytes;                         // HW: source stride = format-dependent
    }

    pTex->UnlockRect(0);
    return XCAM_STATUS_OK;
}