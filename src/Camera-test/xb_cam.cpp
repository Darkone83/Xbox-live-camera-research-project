/*===========================================================================
    xb_cam.cpp -- OV519-family USB camera module for original Xbox
                  Team Resurgent / Darkone83

    This module owns the camera path manually rather than relying on a normal
    runtime class-driver attach. It walks the Xbox USB device tree, claims the
    target OV519/OV530-compatible camera node, initializes the bridge/sensor,
    opens the isochronous IN endpoint, assembles OV519 MJPEG frames, decodes
    them with picojpeg, and pushes the decoded 320x240 image into a swizzled
    A8R8G8B8 preview texture.

    The validated stream path is endpoint 0x81 with alt 3 / maxpkt 768 on the
    tested hardware, but the code also parses descriptors and falls back to the
    largest available isochronous IN endpoint when needed.

    All control transfers are MmIsAddressValid-gated and polled to avoid the
    lockups seen with the earlier callback/event experiments.
===========================================================================*/

#include <xtl.h>
#include <xgraphics.h>
#include "xbox_usb.h"
#include "xbox_kernel.h"
#include "dbg.h"
#include "xb_cam.h"
#include "picojpeg.h"

#define XCAM_TAG "XBCAM"

#define CAM_NODE_SIZE        0x20
#define CAM_MAX_NODES        32
#define CAM_TREE_BASE_OFF    0xE0
#define CAM_IDX_NONE         0x80
#define CAM_CLASS_VENDOR     0xFF
#define CAM_USBD_PENDING     0x40000000u

#define CAM_ISO_EP           0x81
#define CAM_ISO_MAXPKT       1023          /* full-speed iso upper bound; endpoint maxpkt is discovered */
#define CAM_ISO_FRAMES       8
#define CAM_FRAME_BYTES      (XCAM_FRAME_W * XCAM_FRAME_H * 3)   /* legacy sizing constant */

/*---------------------------------------------------------------------------
    State
---------------------------------------------------------------------------*/
static volatile int s_present = 0;
static int          s_port = -1;
static int          s_epAddr = 0;
static int          s_epMaxPkt = 0;
static int          s_addCalls = 0;
static IUsbDevice* s_camDev = 0;
static CamLogFn     s_logfn = 0;

static int          s_streaming = 0;
static void* s_epHandle = 0;
static unsigned char* s_isoBuf = 0;
static int          s_isoBufLen = 0;

/* OV519 frame assembly: image data spans ~25 iso buffers, delimited by SOF/EOF
   headers. Accumulate into s_frame; on EOF publish to s_frameReady. */
#define CAM_YUY2_BYTES   (XCAM_FRAME_W * XCAM_FRAME_H * 2)   /* legacy capacity baseline */
#define CAM_TEX_W        512                                /* pow2 texture (uses 320) */
#define CAM_TEX_H        256                                /* pow2 texture (uses 240) */
#define CAM_ARGB_BYTES   (CAM_TEX_W * CAM_TEX_H * 4)         /* full 4-byte texture buffer */
#define CAM_FRAME_CAP    (CAM_YUY2_BYTES + 8192)             /* slack for header/overrun */
static unsigned char* s_frame = 0;   /* accumulator */
static int            s_frameW = 0;   /* write cursor */
static unsigned char* s_frameReady = 0;   /* last completed frame */
static volatile int   s_frameReadyLen = 0;
static volatile int   s_completedFrames = 0;
static int            s_inFrame = 0;   /* 1 after SOF, 0 after EOF */
static int            s_isoPktSize = 768; /* per-iso-packet size (maxpkt) */

/* JPEG decode (picojpeg): the OV519 streams baseline JPEG per frame. */
static unsigned char* s_jpegBuf = 0;     /* private copy decoded each draw */
static const unsigned char* s_jpgPtr = 0; /* feed cursor */
static int            s_jpgRem = 0;     /* feed remaining */
static int            s_lastDecoded = -1;  /* s_completedFrames last decoded */
static unsigned char* s_rgb = 0;     /* decoded 4-byte frame buffer (pitch = CAM_TEX_W*4) */

static void Cam_Log(const char* msg)
{
    if (s_logfn) { s_logfn(XCAM_TAG, msg); }
    else { Dbg_Log(XCAM_TAG, msg); }
}

/*---------------------------------------------------------------------------
    Device tree + safe-read
---------------------------------------------------------------------------*/
class IUsbDevice; /* fwd */
class CDeviceTree {
public:
    char _opaque[0x200];
    IUsbDevice* AllocDevice();   /* ?AllocDevice@CDeviceTree@@QAEPAVIUsbDevice@@XZ */
};
extern CDeviceTree g_DeviceTree;
extern "C" BOOLEAN __stdcall MmIsAddressValid(PVOID VirtualAddress);

/* framework USB address allocator: ?USBD_AllocateUsbAddress@@YIEPAU_USBD_HOST_CONTROLLER@@@Z
   __fastcall, returns a free address byte and marks it used (no conflicts). */
struct _USBD_HOST_CONTROLLER;
unsigned char __fastcall USBD_AllocateUsbAddress(struct _USBD_HOST_CONTROLLER* hc);

static int Cam_Readable(const void* p, int len)
{
    if (p == 0) return 0;
    if (!MmIsAddressValid((PVOID)p)) return 0;
    if (!MmIsAddressValid((PVOID)((const char*)p + len - 1))) return 0;
    return 1;
}

/*---------------------------------------------------------------------------
    Registration breadcrumb
---------------------------------------------------------------------------*/
DECLARE_XPP_TYPE(XbCameraType)
USB_DEVICE_TYPE_TABLE_BEGIN(Cam)
USB_DEVICE_TYPE_TABLE_ENTRY(&XbCameraType_TABLE)
USB_DEVICE_TYPE_TABLE_END()
USB_CLASS_DRIVER_DECLARATION(Cam, 0xFF, 0x00, 0xFF)
#pragma data_seg(".XPP$ClassCam")
USB_CLASS_DECLARATION_POINTER(Cam)
#pragma data_seg(".XPP$Data")
#pragma comment(linker, "/include:_CamDescriptionPointer")

extern "C" VOID CamInit(IUsbInit* UsbInit)
{
    USB_RESOURCE_REQUIREMENTS rr;
    Cam_Log("CamInit: framework up");
    if (UsbInit == 0) return;
    rr.ConnectorType = USB_CONNECTOR_TYPE_HIGH_POWER; rr.MaxDevices = 1;
    rr.MaxCompositeInterfaces = 2; rr.MaxControlEndpoints = 1; rr.MaxBulkEndpoints = 1;
    rr.MaxInterruptEndpoints = 0; rr.MaxControlTDperTransfer = 0; rr.MaxBulkTDperTransfer = 0;
    rr.MaxIsochEndpoints = 1; rr.MaxIsochMaxBuffers = 4;
    UsbInit->RegisterResources(&rr);
}
extern "C" VOID CamAddDevice(IUsbDevice* Device) { s_addCalls++; if (Device) Device->AddComplete(USBD_STATUS_SUCCESS); }
extern "C" VOID CamRemoveDevice(IUsbDevice* Device) { if (Device) Device->RemoveComplete(); }

/*===========================================================================
    Lock-proof URB submit: NULL completion + poll Hdr.Status. Returns USBD
    status (0 ok), or 0x7FFFFFFF on submit-fail / never-completed.
===========================================================================*/
static volatile LONG s_lastRaw = 0;   /* raw SubmitRequest() return, for logging */

static ULONG Cam_SubmitPoll(IUsbDevice* dev, PURB urb)
{
    LONG st;
    int  spins;
    urb->Header.Status = (USBD_STATUS)CAM_USBD_PENDING;
    urb->Header.CompleteProc = 0;
    urb->Header.CompleteContext = 0;
    st = dev->SubmitRequest(urb);
    s_lastRaw = st;
    /* synchronous completion / immediate result: status is in the URB header */
    if ((*(volatile ULONG*)&urb->Header.Status) != CAM_USBD_PENDING)
        return (ULONG)urb->Header.Status;
    /* immediate error return that never went pending */
    if ((ULONG)st != CAM_USBD_PENDING)
        return (ULONG)st;
    /* async: poll */
    for (spins = 0; spins < 2000; spins++) {
        if ((*(volatile ULONG*)&urb->Header.Status) != CAM_USBD_PENDING) break;
        Sleep(1);
    }
    if ((*(volatile ULONG*)&urb->Header.Status) == CAM_USBD_PENDING) return 0x7FFFFFFF;
    return (ULONG)urb->Header.Status;
}

static void Cam_ZeroUrb(void* p, int n) { int i; for (i = 0; i < n; i++) ((char*)p)[i] = 0; }

/* generic control transfer (IN if buf!=0 & dir-in flag) */
static ULONG Cam_Control(IUsbDevice* dev, UCHAR bmReqType, UCHAR bReq,
    USHORT wValue, USHORT wIndex, void* buf, USHORT len, UCHAR dir)
{
    URB_CONTROL_TRANSFER urb;
    Cam_ZeroUrb(&urb, sizeof(urb));
    urb.Hdr.Length = (UCHAR)sizeof(URB_CONTROL_TRANSFER);
    urb.Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER;
    urb.EndpointHandle = 0;
    urb.TransferBufferLength = len;
    urb.TransferBuffer = buf;
    urb.TransferDirection = dir;
    urb.ShortTransferOK = 1;
    urb.SetupPacket.bmRequestType = bmReqType;
    urb.SetupPacket.bRequest = bReq;
    urb.SetupPacket.wValue = wValue;
    urb.SetupPacket.wIndex = wIndex;
    urb.SetupPacket.wLength = len;
    return Cam_SubmitPoll(dev, (PURB)&urb);
}

/* real per-device VID/PID via GET_DESCRIPTOR(device,18). Returns 1 on success. */
static int Cam_GetVidPid(IUsbDevice* dev, int* vid, int* pid)
{
    unsigned char buf[18];
    ULONG st;
    int i;
    for (i = 0; i < 18; i++) buf[i] = 0;
    st = Cam_Control(dev, 0x80, 0x06 /*GET_DESCRIPTOR*/, 0x0100 /*device*/, 0,
        buf, 18, USB_TRANSFER_DIRECTION_IN);
    if (st != 0) return 0;
    *vid = (int)buf[8] | ((int)buf[9] << 8);
    *pid = (int)buf[10] | ((int)buf[11] << 8);
    return 1;
}

/* is this VID/PID one of our cameras? EyeToy 054C:0155, Xbox Video Camera (Japan
   Video Chat "Xbox Cam") 045E:028C -- NOT the Xbox 360 Live Vision camera */
static int Cam_IsCameraId(int vid, int pid)
{
    if (vid == 0x054C && pid == 0x0155) return 1;
    if (vid == 0x045E && pid == 0x028C) return 1;
    return 0;
}

/*===========================================================================
    Streaming bring-up
===========================================================================*/
/* parse the config descriptor for the iso IN endpoint: returns its interface,
   alt-setting, address and (real) max packet size -- no more guessing. */
static int Cam_FindIsoEndpoint(IUsbDevice* node, int wantAlt, int* ifnum, int* alt, int* epAddr, int* maxpkt)
{
    unsigned char* buf;          /* contiguous: OHCI DMA must not straddle pages */
    ULONG st;
    int total, i, curIf, curAlt, best, ret;

    buf = (unsigned char*)MmAllocateContiguousMemory(256);
    if (buf == 0) { Cam_Log("cfg buf alloc failed"); return 0; }
    for (i = 0; i < 256; i++) buf[i] = 0;

    /* step 1: 9-byte header for wTotalLength */
    st = Cam_Control(node, 0x80, 0x06 /*GET_DESCRIPTOR*/, 0x0200 /*config,index0*/, 0,
        buf, 9, USB_TRANSFER_DIRECTION_IN);
    Dbg_Log_Int(XCAM_TAG, "GET config hdr st", (int)st);
    if (st != 0) { MmFreeContiguousMemory(buf); return 0; }
    total = (int)buf[2] | ((int)buf[3] << 8);
    Dbg_Log_Int(XCAM_TAG, "config wTotalLength", total);
    if (total < 9) { MmFreeContiguousMemory(buf); return 0; }
    if (total > 255) total = 255;

    /* step 2: a single control transfer is capped by the control-TD pool:
       TDs = ceil(len/maxpkt8)+3 must be <= pool. Read the largest prefix that
       fits (the EyeToy video interface is interface 0, so its iso endpoints are
       near the start). */
    {
        int L = total;
        for (;;) {
            int j; for (j = 0; j < 256; j++) buf[j] = 0;
            st = Cam_Control(node, 0x80, 0x06, 0x0200, 0, buf, (USHORT)L, USB_TRANSFER_DIRECTION_IN);
            if (st == 0) break;
            if (L <= 24) {
                Dbg_Log_Int(XCAM_TAG, "config read failed at len", L);
                MmFreeContiguousMemory(buf); return 0;
            }
            L = (L * 3) / 4;            /* shrink and retry (queue check is pre-device, clean) */
        }
        Dbg_Log_Int(XCAM_TAG, "config prefix read len", L);
        total = L;                     /* parse only what we actually read */
    }

    {
        int wIf = -1, wAddr = 0, wMp = 0;     /* endpoint on the wanted alt */
        int bIf = -1, bAddr = 0, bMp = 0, bAlt = 0; /* largest-maxpkt fallback */
        curIf = -1; curAlt = 0; i = 0;
        while (i + 2 <= total) {
            int len = (int)buf[i];
            int type = (int)buf[i + 1];
            if (len < 2) break;
            if (type == 0x04 && i + 4 <= total) {
                curIf = (int)buf[i + 2];
                curAlt = (int)buf[i + 3];
            }
            else if (type == 0x05 && i + 6 <= total) {
                int addr = (int)buf[i + 2];
                int attr = (int)buf[i + 3];
                int mp = (int)buf[i + 4] | ((int)buf[i + 5] << 8);
                if ((attr & 0x03) == 0x01 && (addr & 0x80)) {
                    if (curAlt == wantAlt && mp > wMp) { wMp = mp; wIf = curIf; wAddr = addr; }
                    if (mp > bMp) { bMp = mp; bIf = curIf; bAddr = addr; bAlt = curAlt; }
                }
            }
            i += len;
        }
        if (wMp > 0) { *ifnum = wIf; *alt = wantAlt; *epAddr = wAddr; *maxpkt = wMp; best = wMp; }
        else if (bMp > 0) { *ifnum = bIf; *alt = bAlt; *epAddr = bAddr; *maxpkt = bMp; best = bMp; }
        else best = 0;
        ret = (best > 0) ? 1 : 0;
    }
    MmFreeContiguousMemory(buf);
    return ret;
}

/*===========================================================================
    OV519 bridge + OV7620 sensor init (from default.xbe decomp + .set tables)
      bridge reg write : vendor OUT, bRequest 1, wValue 0, wIndex=reg, 1 byte
      bridge reg read  : vendor IN,  bRequest 1, wValue 0, wIndex=reg, 1 byte
      sensor I2C write : reg_w(0x42,reg); reg_w(0x45,val); reg_w(0x47,0x01)
      sensor slave id  : reg_w(0x41,0x42)=W_SID, reg_w(0x44,0x43)=R_SID
===========================================================================*/
static ULONG Cam_OvW(IUsbDevice* dev, int reg, int val)
{
    unsigned char b = (unsigned char)val;
    return Cam_Control(dev, 0x41, 0x01, 0x0000, (USHORT)reg, &b, 1, USB_TRANSFER_DIRECTION_OUT);
}
static ULONG Cam_OvR(IUsbDevice* dev, int reg, unsigned char* out)
{
    *out = 0;
    return Cam_Control(dev, 0xC1, 0x01, 0x0000, (USHORT)reg, out, 1, USB_TRANSFER_DIRECTION_IN);
}
static void Cam_OvWMask(IUsbDevice* dev, int reg, int val, int mask)
{
    if (mask == 0xFF) { Cam_OvW(dev, reg, val); return; }
    {
        unsigned char old = 0;
        if (Cam_OvR(dev, reg, &old) == 0) Cam_OvW(dev, reg, (old & ~mask) | (val & mask));
        else                              Cam_OvW(dev, reg, val);
    }
}
/* OV7620 sensor register write through the OV519 I2C master */
static ULONG Cam_I2cW(IUsbDevice* dev, int reg, int val)
{
    ULONG s1, s2, s3;
    s1 = Cam_OvW(dev, 0x42, reg);     /* I2C sub-address = sensor register */
    s2 = Cam_OvW(dev, 0x45, val);     /* I2C data       = value           */
    s3 = Cam_OvW(dev, 0x47, 0x01);    /* I2C control    = initiate write   */
    Sleep(1);
    return s1 | s2 | s3;
}

/* OV519 bridge registers (7620519.set : UsbSetting)  -- Index,Value,Mask */
static const unsigned char k_usbSet[] = {
    0x5d,0x03,0xff, 0x53,0x9b,0x9b, 0x54,0x0f,0xff, 0xa2,0x20,0xff, 0xa3,0x18,0xff,
    0xa4,0x04,0xff, 0xa5,0x28,0xff, 0x37,0x00,0xff, 0x55,0x02,0xff, 0x22,0x1d,0xff,
    0x17,0x50,0xff, 0x37,0x00,0xff, 0x40,0xff,0xff, 0x46,0x00,0xff
};
/* OV7620 sensor registers (7620519.set : CameraSetting) -- Index,Value (all mask ff) */
static const unsigned char k_camSet[] = {
    0x12,0x80, 0x00,0x00, 0x01,0x80, 0x02,0x80, 0x03,0xb0, 0x06,0x80, 0x07,0x00,
    0x0c,0x24, 0x0d,0x24, 0x11,0x01, 0x12,0x24, 0x13,0x01, 0x15,0x01, 0x16,0x03,
    0x17,0x2f, 0x18,0xcf, 0x19,0x06, 0x1a,0xf5, 0x1b,0x00, 0x20,0x18, 0x21,0x80,
    0x22,0x80, 0x23,0x00, 0x26,0xa2, 0x27,0xea, 0x29,0x00, 0x2a,0x80, 0x2b,0x00,
    0x2c,0x88, 0x2d,0xd5, 0x2e,0x80, 0x2f,0x44, 0x60,0x27, 0x61,0x02, 0x62,0x5f,
    0x63,0xcc, 0x64,0x57, 0x65,0x83, 0x66,0x55, 0x67,0xb0, 0x68,0xcf, 0x69,0x76,
    0x6a,0x22, 0x6b,0x00, 0x6c,0x02, 0x6d,0x44, 0x6e,0x40, 0x6f,0x1d, 0x70,0x8b,
    0x71,0x00, 0x72,0x14, 0x73,0x54, 0x74,0x00, 0x75,0x8e, 0x76,0x00, 0x77,0xff,
    0x78,0x80, 0x79,0x80, 0x7a,0x80, 0x7b,0xe2, 0x7c,0x00, 0x12,0x20, 0x12,0x24
};

/* read an OV7620 sensor register back through the OV519 I2C master (verify) */
static ULONG Cam_I2cR(IUsbDevice* dev, int reg, unsigned char* out)
{
    unsigned char ctl = 0; int tries; ULONG st;
    *out = 0;
    Cam_OvW(dev, 0x43, reg);            /* READ sub-address goes in reg 0x43 (slave id set by caller) */
    Cam_OvW(dev, 0x47, 0x03);           /* control = 3 */
    Sleep(1);
    for (tries = 0; tries < 4; tries++) { Cam_OvR(dev, 0x47, &ctl); if (ctl & 0x01) break; }
    Cam_OvW(dev, 0x47, 0x05);           /* control = 5 */
    Sleep(1);
    for (tries = 0; tries < 4; tries++) { Cam_OvR(dev, 0x47, &ctl); if (ctl & 0x01) break; }
    st = Cam_OvR(dev, 0x45, out);       /* data */
    Cam_OvW(dev, 0x42, 0xff);           /* commit/cleanup (per FUN_000c98f0) */
    Cam_OvW(dev, 0x45, 0x00);
    Cam_OvW(dev, 0x40, 0x01);
    return st;
}

/* 320x240 bridge frame-size/window regs (table label retained from OV519 refs) */
static const unsigned char k_usb320[] = {
    0x10,0x14,0xff, 0x11,0x1e,0xff, 0x12,0x00,0xff, 0x13,0x00,0xff, 0x14,0x00,0xff,
    0x15,0x00,0xff, 0x16,0x00,0xff, 0x25,0x01,0xff, 0x26,0x00,0xff
};
/* 320x240 sensor regs (Index,Value,Mask; table label retained from OV519 refs) */
static const unsigned char k_cam320[] = {
    0x2b,0x00,0xff, 0x14,0xa4,0xff, 0x11,0x01,0xff, 0x28,0x00,0x20, 0x24,0x20,0xff,
    0x25,0x30,0xff, 0x2d,0xd5,0x40, 0x67,0xb0,0xf0, 0x74,0x20,0xff, 0x75,0x00,0x01
};
/* FrameRate0 (320, 30fps): bridge a4,04 23,ff ; sensor 2b,00 11,01 */
static const unsigned char k_usbFR[] = { 0xa4,0x04,0xff, 0x23,0xff,0xff };
static const unsigned char k_camFR[] = { 0x2b,0x00,0xff, 0x11,0x01,0xff };

/* OV7620 sensor write with mask (read-modify-write via I2C when mask != 0xff) */
static void Cam_I2cWMask(IUsbDevice* dev, int reg, int val, int mask)
{
    if (mask == 0xFF) { Cam_I2cW(dev, reg, val); return; }
    {
        unsigned char old = 0;
        Cam_I2cR(dev, reg, &old);
        Cam_I2cW(dev, reg, (old & ~mask) | (val & mask));
    }
}

static void Cam_InitSensor(IUsbDevice* dev)
{
    int i, sensor76 = 0;
    unsigned char rb = 0xAB;

    /* 0) init_519 generic bridge bring-up (the OV519 connect path; the .set
          UsbSetting deliberately omits this and starts at 0x5d). The CRITICAL reg
          is 0x72 = 0xEE: GPIO_IO_CTRL0 bit 4 (0x10) MUST be cleared or sensor
          detection fails. Hardware default was 0xFF (bit4 set) -> sensor invisible. */
    Cam_OvW(dev, 0x5a, 0x6d);   /* EnableSystem */
    Cam_OvW(dev, 0x53, 0x9b);   /* don't enable the microcontroller */
    Cam_OvW(dev, 0x54, 0xff);   /* EN_CLK1 (bit2 jpeg enable) */
    Cam_OvW(dev, 0x5d, 0x03);   /* turn off suspend mode */
    Cam_OvW(dev, 0x49, 0x01);
    Cam_OvW(dev, 0x48, 0x00);
    Cam_OvW(dev, 0x72, 0xee);   /* GPIO dir: bit4 CLEAR -- required for sensor detect */
    Cam_OvW(dev, 0x51, 0x0f);   /* RESET1 assert */
    Cam_OvW(dev, 0x51, 0x00);   /* RESET1 release */
    Cam_OvW(dev, 0x22, 0x00);
    Sleep(10);

    /* init_ov_sensor(OV7xx0_SID=0x42): set slave ids, reset the sensor via SCCB
       (reg 0x12 = 0x80 = COM7 reset), wait 150ms, then read manufacturer ID
       (0x1c == 0x7F, 0x1d == 0xA2). Retry with reset + dummy read if not synced. */
    Cam_OvW(dev, 0x41, 0x42);   /* W_SID = slave */
    Cam_OvW(dev, 0x44, 0x43);   /* R_SID = slave + 1 */
    {
        int tries, synced = 0;
        unsigned char idh, idl, dummy;
        Cam_I2cW(dev, 0x12, 0x80);   /* COM7 sensor reset */
        Sleep(150);
        for (tries = 0; tries < 5; tries++) {
            idh = 0xCC; idl = 0xCC;
            Cam_I2cR(dev, 0x1c, &idh);
            Cam_I2cR(dev, 0x1d, &idl);
            Dbg_Log_Int(XCAM_TAG, "detect try", tries);
            Dbg_Log_Int(XCAM_TAG, "  MIDH 0x1c", (int)idh);
            Dbg_Log_Int(XCAM_TAG, "  MIDL 0x1d", (int)idl);
            if (idh == 0x7f && idl == 0xa2) { synced = 1; break; }
            Cam_I2cW(dev, 0x12, 0x80);   /* reset again */
            Sleep(150);
            dummy = 0; Cam_I2cR(dev, 0x00, &dummy);   /* dummy read to sync I2C */
        }
        Dbg_Log_Int(XCAM_TAG, "*** SENSOR SYNCED", synced);
    }

    /* ---- Identify the sensor: reg 0x0a (PID high). !=0x76 => OV7620;
            ==0x76 => 76xx family (OV7648 etc., minimal init). ---- */
    {
        unsigned char pidh = 0, pidl = 0;
        Cam_I2cR(dev, 0x0a, &pidh);
        Cam_I2cR(dev, 0x0b, &pidl);
        Dbg_Log_Int(XCAM_TAG, "sensor PID 0x0a", (int)pidh);
        Dbg_Log_Int(XCAM_TAG, "sensor VER 0x0b", (int)pidl);
        sensor76 = (pidh == 0x76) ? 1 : 0;
        Dbg_Log_Int(XCAM_TAG, "is 76xx (7648-class)", sensor76);
    }

    /* ---- Bridge mode-init (ov519_mode_init_regs / mode_init_519): YUV input
            path, I2C timing (0x40/0x46), audio clock, FRAR. ---- */
    Cam_OvW(dev, 0x5d, 0x03);
    Cam_OvW(dev, 0x53, 0x9f);
    Cam_OvW(dev, 0x54, 0x0f);
    Cam_OvW(dev, 0xa2, 0x20);
    Cam_OvW(dev, 0xa3, 0x18);
    Cam_OvW(dev, 0xa4, 0x04);
    Cam_OvW(dev, 0xa5, 0x28);
    Cam_OvW(dev, 0x37, 0x00);
    Cam_OvW(dev, 0x55, 0x02);   /* 4.096 MHz audio clock */
    Cam_OvW(dev, 0x22, 0x1d);
    Cam_OvW(dev, 0x17, 0x50);
    Cam_OvW(dev, 0x40, 0xff);   /* I2C timeout counter */
    Cam_OvW(dev, 0x46, 0x00);   /* I2C clock prescaler */
    Cam_OvW(dev, 0x59, 0x04);
    Cam_OvW(dev, 0xff, 0x00);

    /* ---- Full sensor init (the .set CameraSetting alone is insufficient) ---- */
    if (sensor76) {
        /* norm_7640/7648: reset, then 0x12=0x14; bridge selects 8-bit input */
        Cam_I2cW(dev, 0x12, 0x80); Sleep(150);
        Cam_I2cW(dev, 0x12, 0x14);
        Cam_OvWMask(dev, 0x20, 0x10, 0x10);   /* OV519_R20_DFR: 8-bit input mode */

        /* mode_init_ov_sensor_regs (OV7648, QVGA=320x240): mode + AWB regs */
        Cam_I2cWMask(dev, 0x14, 0x20, 0x20);  /* qvga */
        Cam_I2cWMask(dev, 0x28, 0x00, 0x20);
        Cam_I2cWMask(dev, 0x2d, 0x40, 0x40);  /* anti-shake (undocumented) */
        Cam_I2cWMask(dev, 0x67, 0xf0, 0xf0);
        Cam_I2cWMask(dev, 0x74, 0x20, 0x20);  /* higher auto gain */
        Cam_I2cWMask(dev, 0x12, 0x04, 0x04);  /* AWB on */
        Cam_I2cW(dev, 0x11, 0x00);            /* clockdiv = 0 (30fps) */

        /* set_ov_sensor_window (OV7648, QVGA): hwsbase 0x1a, vwsbase 0x03,
           hwscale 1, vwscale 0. THIS is the window -- without it the OV51x
           delivers ALL-ZERO isoc data (which is exactly what we saw). */
        Cam_I2cW(dev, 0x17, 0x1a);                  /* HSTART */
        Cam_I2cW(dev, 0x18, 0x1a + (320 >> 1));     /* HSTOP  = 0xba */
        Cam_I2cW(dev, 0x19, 0x03);                  /* VSTART */
        Cam_I2cW(dev, 0x1a, 0x03 + (240 >> 0));     /* VSTOP  = 0xf3 */
    }
    else {
        /* norm_7620 full OV7620 register table */
        static const unsigned char k_norm7620[] = {
            0x12,0x80, 0x00,0x00, 0x01,0x80, 0x02,0x80, 0x03,0xc0, 0x06,0x60,
            0x07,0x00, 0x0c,0x24, 0x0c,0x24, 0x0d,0x24, 0x11,0x01, 0x12,0x24,
            0x13,0x01, 0x14,0x84, 0x15,0x01, 0x16,0x03, 0x17,0x2f, 0x18,0xcf,
            0x19,0x06, 0x1a,0xf5, 0x1b,0x00, 0x20,0x18, 0x21,0x80, 0x22,0x80,
            0x23,0x00, 0x26,0xa2, 0x27,0xea, 0x28,0x22, 0x29,0x00, 0x2a,0x10,
            0x2b,0x00, 0x2c,0x88, 0x2d,0x91, 0x2e,0x80, 0x2f,0x44, 0x60,0x27,
            0x61,0x02, 0x62,0x5f, 0x63,0xd5, 0x64,0x57, 0x65,0x83, 0x66,0x55,
            0x67,0x92, 0x68,0xcf, 0x69,0x76, 0x6a,0x22, 0x6b,0x00, 0x6c,0x02,
            0x6d,0x44, 0x6e,0x80, 0x6f,0x1d, 0x70,0x8b, 0x71,0x00, 0x72,0x14,
            0x73,0x54, 0x74,0x00, 0x75,0x8e, 0x76,0x00, 0x77,0xff, 0x78,0x80,
            0x79,0x80, 0x7a,0x80, 0x7b,0xe2, 0x7c,0x00
        };
        for (i = 0; i + 1 < (int)sizeof(k_norm7620); i += 2) {
            Cam_I2cW(dev, k_norm7620[i], k_norm7620[i + 1]);
            if (k_norm7620[i] == 0x12 && k_norm7620[i + 1] == 0x80) Sleep(150);
        }
    }

    /* ---- Bridge frame geometry: 320x240, YUV422 (R25=0x03) ---- */
    Cam_OvW(dev, 0x10, 320 >> 4);   /* H_SIZE = 0x14 */
    Cam_OvW(dev, 0x11, 240 >> 3);   /* V_SIZE = 0x1e */
    Cam_OvW(dev, 0x12, 0x00);
    Cam_OvW(dev, 0x13, 0x00);
    Cam_OvW(dev, 0x14, 0x00);
    Cam_OvW(dev, 0x15, 0x00);
    Cam_OvW(dev, 0x16, 0x00);
    Cam_OvW(dev, 0x25, 0x03);       /* FORMAT = YUV422 */
    Cam_OvW(dev, 0x26, 0x00);

    /* ---- Frame rate (OV7648, 30fps default): bridge regs ---- */
    Cam_OvW(dev, 0xa4, 0x0c);
    Cam_OvW(dev, 0x23, 0xff);

    /* ---- ov51x_restart: un-block the bridge stream FIFO, then LED on ---- */
    Cam_OvW(dev, 0x51, 0x0f);
    Cam_OvW(dev, 0x51, 0x00);
    Cam_OvW(dev, 0x22, 0x1d);              /* FRAR */
    Cam_OvWMask(dev, 0x71, 0x01, 0x01);    /* LED on (GPIO_DATA_OUT0 bit0) */

    /* verify the sensor is still synced after full init */
    Cam_I2cR(dev, 0x1c, &rb); Dbg_Log_Int(XCAM_TAG, "post MIDH 0x1c (exp 7F)", (int)rb);
    Cam_I2cR(dev, 0x1d, &rb); Dbg_Log_Int(XCAM_TAG, "post MIDL 0x1d (exp A2)", (int)rb);

    Cam_Log("sensor init applied (norm_7620 + geometry + restart)");
}

/*===========================================================================
    Streaming bring-up (real endpoint params from the config descriptor)
===========================================================================*/
static USBD_ISOCH_BUFFER_DESCRIPTOR s_bd;       /* persistent: framework holds &s_bd */
static URB_ISOCH_ATTACH_BUFFER      s_attUrb;   /* persistent: re-armed in the callback */
static volatile int                 s_isoFrames = 0;
static IUsbDevice* s_isoDev = 0;

/* iso frame-complete callback. The framework calls this __stdcall with two args
   (status*, context) and does NOT null-check it -- so it must be non-NULL and
   __stdcall. We re-arm the same buffer to keep streaming (ISOCH_ATTACH is
   synchronous, so it's DPC-safe; no Sleep/poll here). */
static void __stdcall Cam_IsoComplete(void* status, void* context)
{
    PUSBD_ISOCH_TRANSFER_STATUS ts = (PUSBD_ISOCH_TRANSFER_STATUS)status;
    int p;
    (void)context;
    s_isoFrames++;

    /* Parse the just-received packets for OV519 frame structure. Each iso frame i
       sits at offset i*maxpkt; its ACTUAL length is PacketStatus[i].BytesRead (12b).
       Using the real length is essential -- copying the full maxpkt drags in stale
       bytes that corrupt the JPEG bitstream and bury the EOI marker. */
    if (s_frame && s_frameReady && ts) {
        for (p = 0; p < CAM_ISO_FRAMES; p++) {
            unsigned char* pk = s_isoBuf + p * s_isoPktSize;
            int avail = (int)(ts->PacketStatus[p].BytesRead);   /* real bytes this frame */
            if (avail <= 0 || avail > s_isoPktSize) {
                if (avail > s_isoPktSize) avail = s_isoPktSize; else continue;
            }
            if (avail >= 4 && pk[0] == 0xff && pk[1] == 0xff && pk[2] == 0xff &&
                (pk[3] == 0x50 || pk[3] == 0x51)) {
                if (pk[3] == 0x50) {          /* start of frame */
                    s_frameW = 0;
                    s_inFrame = 1;
                    pk += 16; avail -= 16;    /* strip 16-byte header */
                    if (avail > 0 && s_frameW + avail <= CAM_FRAME_CAP) {
                        int b; for (b = 0; b < avail; b++) s_frame[s_frameW + b] = pk[b];
                        s_frameW += avail;
                    }
                }
                else {                      /* end of frame -> publish */
                    int n = s_frameW; int b;
                    if (n > CAM_FRAME_CAP) n = CAM_FRAME_CAP;
                    for (b = 0; b < n; b++) s_frameReady[b] = s_frame[b];
                    s_frameReadyLen = n;
                    s_completedFrames++;
                    s_frameW = 0;
                    s_inFrame = 0;
                }
            }
            else if (s_inFrame) {           /* intermediate image data */
                if (s_frameW + avail <= CAM_FRAME_CAP) {
                    int b; for (b = 0; b < avail; b++) s_frame[s_frameW + b] = pk[b];
                    s_frameW += avail;
                }
            }
        }
    }

    s_attUrb.Hdr.Status = (USBD_STATUS)CAM_USBD_PENDING;
    s_attUrb.Hdr.CompleteProc = 0;
    s_attUrb.Hdr.CompleteContext = 0;
    if (s_isoDev) s_isoDev->SubmitRequest((PURB)&s_attUrb);
}

static int Cam_StartStream(IUsbDevice* dev, int ifnum, int alt, int epAddr, int maxpkt)
{
    URB_ISOCH_OPEN_ENDPOINT      uOpen;
    URB_ISOCH_START_TRANSFER     uStart;
    ULONG st;

    if (dev == 0) return -1;
    s_isoDev = dev;

    Cam_InitSensor(dev);            /* wake the OV7620 before selecting the iso alt */

    st = Cam_Control(dev, 0x01, 0x0B /*SET_INTERFACE*/, (USHORT)alt, (USHORT)ifnum,
        0, 0, USB_TRANSFER_DIRECTION_OUT);
    Dbg_Log_Int(XCAM_TAG, "SET_INTERFACE st", (int)st);

    Cam_ZeroUrb(&uOpen, sizeof(uOpen));
    uOpen.Hdr.Length = (UCHAR)sizeof(uOpen);
    uOpen.Hdr.Function = URB_FUNCTION_ISOCH_OPEN_ENDPOINT;
    uOpen.FunctionAddress = 0;                       /* SubmitRequest fills from node[5] */
    uOpen.EndpointAddress = (UCHAR)epAddr;
    uOpen.MaxPacketSize = (USHORT)maxpkt;
    uOpen.Flags = 0;
    st = Cam_SubmitPoll(dev, (PURB)&uOpen);
    Dbg_Log_Int(XCAM_TAG, "ISOCH_OPEN st", (int)st);
    Dbg_Log_Int(XCAM_TAG, "ISOCH_OPEN rawret", (int)s_lastRaw);
    if (st != 0) return -2;
    s_epHandle = uOpen.EndpointHandle;

    s_isoBufLen = CAM_ISO_FRAMES * maxpkt;
    s_isoPktSize = maxpkt;
    s_isoBuf = (unsigned char*)MmAllocateContiguousMemory(s_isoBufLen);
    if (s_isoBuf == 0) { Cam_Log("iso buf alloc FAILED"); return -3; }
    { int i; for (i = 0; i < s_isoBufLen; i++) s_isoBuf[i] = 0; }

    /* frame-assembly buffers */
    s_frame = (unsigned char*)MmAllocateContiguousMemory(CAM_FRAME_CAP);
    s_frameReady = (unsigned char*)MmAllocateContiguousMemory(CAM_FRAME_CAP);
    s_jpegBuf = (unsigned char*)MmAllocateContiguousMemory(CAM_FRAME_CAP);
    s_rgb = (unsigned char*)MmAllocateContiguousMemory(CAM_ARGB_BYTES);
    if (s_frame == 0 || s_frameReady == 0 || s_jpegBuf == 0 || s_rgb == 0) { Cam_Log("frame buf alloc FAILED"); return -3; }
    { int i; for (i = 0; i < CAM_ARGB_BYTES; i++) s_rgb[i] = 0; }
    { int i; for (i = 0; i < CAM_FRAME_CAP; i++) { s_frame[i] = 0; s_frameReady[i] = 0; } }
    s_frameW = 0; s_inFrame = 0; s_frameReadyLen = 0; s_completedFrames = 0; s_lastDecoded = -1;

    Cam_ZeroUrb(&s_bd, sizeof(s_bd));
    s_bd.FrameCount = CAM_ISO_FRAMES;
    s_bd.TransferBuffer = s_isoBuf;
    /* Pattern[i] = bytes to transfer for iso frame i. Cam_ZeroUrb left these 0,
       so every frame requested 0 bytes -> empty stream. Fill with maxpkt. */
    { int p; for (p = 0; p < CAM_ISO_FRAMES && p < 8; p++) s_bd.Pattern[p] = (USHORT)maxpkt; }
    s_bd.TransferComplete = (PFNUSBD_ISOCH_TRANSFER_COMPLETE)Cam_IsoComplete;  /* __stdcall, non-NULL */
    s_bd.Context = 0;
    Cam_ZeroUrb(&s_attUrb, sizeof(s_attUrb));
    s_attUrb.Hdr.Length = (UCHAR)sizeof(s_attUrb);
    s_attUrb.Hdr.Function = URB_FUNCTION_ISOCH_ATTACH_BUFFER;
    s_attUrb.EndpointHandle = s_epHandle;
    s_attUrb.InterruptDelay = 0;
    s_attUrb.BufferDescriptor = &s_bd;
    st = Cam_SubmitPoll(dev, (PURB)&s_attUrb);
    Dbg_Log_Int(XCAM_TAG, "ISOCH_ATTACH st", (int)st);
    if (st != 0) return -4;

    Cam_ZeroUrb(&uStart, sizeof(uStart));
    uStart.Hdr.Length = (UCHAR)sizeof(uStart);
    uStart.Hdr.Function = URB_FUNCTION_ISOCH_START_TRANSFER;
    uStart.EndpointHandle = s_epHandle;
    uStart.FrameNumber = 0;
    uStart.Flags = URB_FLAG_ISOCH_START_ASAP;
    st = Cam_SubmitPoll(dev, (PURB)&uStart);
    Dbg_Log_Int(XCAM_TAG, "ISOCH_START st", (int)st);
    if (st != 0) return -5;

    s_streaming = 1;
    Cam_Log("*** ISO STREAM STARTED ***");
    return 0;
}

/*===========================================================================
    Active walk (detection) -- unchanged
===========================================================================*/
static char* Cam_GetNodeBase(void) { char* t = (char*)(void*)&g_DeviceTree; if (!Cam_Readable(t + CAM_TREE_BASE_OFF, 4)) return 0; return *(char**)(t + CAM_TREE_BASE_OFF); }
static IUsbDevice* Cam_Node(char* base, int idx) { char* n; if (idx < 0 || idx >= CAM_MAX_NODES) return 0; n = base + idx * CAM_NODE_SIZE; if (!Cam_Readable(n, CAM_NODE_SIZE)) return 0; return (IUsbDevice*)(void*)n; }
static int Cam_NodeIndex(char* base, IUsbDevice* dev) { char* p; int off; if (dev == 0) return -1; p = (char*)(void*)dev; if (p < base) return -1; off = (int)(p - base); if (off % CAM_NODE_SIZE) return -1; off /= CAM_NODE_SIZE; if (off < 0 || off >= CAM_MAX_NODES) return -1; return off; }

static int s_camFound = 0;   /* a node whose REAL VID/PID is a camera */
static int s_camHubPort = -1; /* TI-hub port that holds the unenumerated camera */
static IUsbDevice* s_hubDev = 0; /* the TI hub itself (for port control) */

/* USB hub class requests on a REAL hub device: enumerate its downstream ports
   and log which carry a device. The port that is connected but has no tree node
   is where the camera is stuck. (bmRequestType 0xA0=hub, 0xA3=port/other.) */
static void Cam_ScanHub(IUsbDevice* dev, int gamepadPort)
{
    unsigned char hd[16];
    unsigned char ps[4];
    int nports, p, i;
    ULONG st;

    for (i = 0; i < 16; i++) hd[i] = 0;
    st = Cam_Control(dev, 0xA0, 0x06 /*GET_DESCRIPTOR*/, 0x2900 /*hub desc*/, 0,
        hd, 9, USB_TRANSFER_DIRECTION_IN);
    Dbg_Log_Int(XCAM_TAG, "  HUB desc st", (int)st);
    if (st != 0) return;
    nports = (int)hd[2];
    Dbg_Log_Int(XCAM_TAG, "  HUB nPorts", nports);
    if (nports < 1 || nports > 8) return;

    for (p = 1; p <= nports; p++) {
        int status;
        for (i = 0; i < 4; i++) ps[i] = 0;
        st = Cam_Control(dev, 0xA3, 0x00 /*GET_STATUS*/, 0, (USHORT)p,
            ps, 4, USB_TRANSFER_DIRECTION_IN);
        if (st != 0) { Dbg_Log_Int(XCAM_TAG, "  port GETSTATUS fail @port", p); continue; }
        status = (int)ps[0] | ((int)ps[1] << 8);
        Dbg_Log_Int(XCAM_TAG, "  --- port", p);
        Dbg_Log_Int(XCAM_TAG, "    status(dec)", status);
        Dbg_Log_Int(XCAM_TAG, "    connected", (status & 0x0001) ? 1 : 0);
        Dbg_Log_Int(XCAM_TAG, "    enabled", (status & 0x0002) ? 1 : 0);
        Dbg_Log_Int(XCAM_TAG, "    lowspeed", (status & 0x0200) ? 1 : 0);
        (void)gamepadPort;
        if (status & 0x0001) {            /* a device is on this port */
            Cam_Log("    *** device present on this port ***");
            if (!(status & 0x0002)) s_camHubPort = p;  /* connected but NOT enabled = camera */
        }
    }
}

/* USB hub port feature codes */
#define CAM_PORT_RESET     4
#define CAM_C_PORT_RESET   20
#define CAM_C_PORT_CONNECT 16

/* Reset (and thereby enable) a downstream hub port. After a successful reset the
   hub auto-enables the port and the device responds at address 0. */
static int Cam_ResetPort(IUsbDevice* hub, int port)
{
    unsigned char ps[4];
    ULONG st;
    int i, tries, status;

    Dbg_Log_Int(XCAM_TAG, "ResetPort: resetting port", port);
    st = Cam_Control(hub, 0x23, 0x03 /*SET_FEATURE*/, CAM_PORT_RESET, (USHORT)port,
        0, 0, USB_TRANSFER_DIRECTION_OUT);
    Dbg_Log_Int(XCAM_TAG, "  SET PORT_RESET st", (int)st);
    if (st != 0) return -1;

    /* wait for reset to finish: poll status until reset-change latches / enabled */
    status = 0;
    for (tries = 0; tries < 20; tries++) {
        Sleep(15);
        for (i = 0; i < 4; i++) ps[i] = 0;
        st = Cam_Control(hub, 0xA3, 0x00, 0, (USHORT)port, ps, 4, USB_TRANSFER_DIRECTION_IN);
        if (st != 0) continue;
        status = (int)ps[0] | ((int)ps[1] << 8);
        if (status & 0x0002) break;            /* enabled */
    }
    Dbg_Log_Int(XCAM_TAG, "  post-reset status(dec)", status);
    Dbg_Log_Int(XCAM_TAG, "  enabled now", (status & 0x0002) ? 1 : 0);
    Dbg_Log_Int(XCAM_TAG, "  lowspeed", (status & 0x0200) ? 1 : 0);

    /* clear the reset-change flag so the hub state machine is tidy */
    Cam_Control(hub, 0x23, 0x01 /*CLEAR_FEATURE*/, CAM_C_PORT_RESET, (USHORT)port,
        0, 0, USB_TRANSFER_DIRECTION_OUT);

    if (status & 0x0002) { Cam_Log("  *** PORT 4 ENABLED -- device live at addr 0 ***"); return 0; }
    Cam_Log("  port did not enable");
    return -2;
}

/* Build a node we OWN for the device on (hub, port), routing it via the hub's
   HC context. Mirrors what the framework's DeviceConnected sets, minus the enum
   kick that would free it on no-class-driver. Device stays at address 0. */
#define CAM_URB_OPEN_DEFAULT_EP  0x82   /* SubmitRequest routes this to OpenDefaultEndpoint */

   /* Build a node we OWN for the device on (hub, port). AllocDevice already fills
      +0xC (HC context) and +6 (EP0 maxpkt); we set the routing identity for a fresh
      device at address 0. No InsertChild -- OpenDefaultEndpoint's fresh-open path
      uses +0xC directly, not the parent. */
static IUsbDevice* Cam_BuildNode(IUsbDevice* hub, int port)
{
    IUsbDevice* node;
    unsigned char* nb;
    (void)hub;

    node = g_DeviceTree.AllocDevice();
    if (node == 0) { Cam_Log("AllocDevice FAILED"); return 0; }
    if (!Cam_Readable(node, CAM_NODE_SIZE)) { Cam_Log("AllocDevice gave bad ptr"); return 0; }

    nb = (unsigned char*)(void*)node;
    nb[0] = 0xFE;                 /* state: connected (NOT 0x05 -> fresh-open path) */
    nb[4] = (unsigned char)port;  /* hub port (bit7=0 -> full speed) */
    nb[5] = 0;                    /* USB address 0 */
    nb[6] = 8;                    /* EP0 max packet (safe default) */

    Cam_Log("Cam_BuildNode: node allocated");
    return node;
}

/* Open EP0: SubmitRequest dispatches func 0x82 to OpenDefaultEndpoint, which fills
   node+8 (the default-endpoint handle the control path needs). Synchronous. */
static ULONG Cam_OpenDefaultEP(IUsbDevice* node)
{
    URB u;
    Cam_ZeroUrb(&u, sizeof(u));
    u.Header.Length = (UCHAR)sizeof(URB);
    u.Header.Function = CAM_URB_OPEN_DEFAULT_EP;
    return Cam_SubmitPoll(node, (PURB)&u);
}

/* Close EP0 (func 0xC3 -> CloseDefaultEndpoint) so we can reopen it at a new addr */
static ULONG Cam_CloseDefaultEP(IUsbDevice* node)
{
    URB u;
    Cam_ZeroUrb(&u, sizeof(u));
    u.Header.Length = (UCHAR)sizeof(URB);
    u.Header.Function = 0xC3;   /* URB_FUNCTION_CLOSE_DEFAULT_ENDPOINT */
    return Cam_SubmitPoll(node, (PURB)&u);
}

/* Full manual bring-up on our own node -> the path to a frame. */
static void Cam_BringUpManual(IUsbDevice* hub, int port)
{
    IUsbDevice* node;
    int vid, pid, rc;
    ULONG st;

    node = Cam_BuildNode(hub, port);
    if (node == 0) return;

    st = Cam_OpenDefaultEP(node);
    Dbg_Log_Int(XCAM_TAG, "OPEN_DEFAULT_EP st", (int)st);
    Dbg_Log_Int(XCAM_TAG, "  rawret", (int)s_lastRaw);
    if ((LONG)st < 0) { Cam_Log("EP0 open failed -- cannot talk to device"); return; }

    vid = -1; pid = -1;
    if (!Cam_GetVidPid(node, &vid, &pid)) { Cam_Log("manual GET_DESCRIPTOR failed @addr0"); return; }
    Dbg_Log_Int(XCAM_TAG, "manual VID(dec)", vid);
    Dbg_Log_Int(XCAM_TAG, "manual PID(dec)", pid);
    if (!Cam_IsCameraId(vid, pid)) { Cam_Log("addr0 device is NOT the camera"); return; }

    Cam_Log("*** CAMERA NODE OWNED (VID/PID @ addr0) ***");
    s_camDev = node; s_camFound = 1; s_port = port;

    /* device is in Default state (addr 0) -> must SET_ADDRESS before configuring */
    {
        unsigned char* nb = (unsigned char*)(void*)node;
        void* hc = *(void**)(nb + 0x0C);
        unsigned char addr = USBD_AllocateUsbAddress((struct _USBD_HOST_CONTROLLER*)hc);
        Dbg_Log_Int(XCAM_TAG, "alloc addr", (int)addr);

        st = Cam_Control(node, 0x00, 0x05 /*SET_ADDRESS*/, (USHORT)addr, 0, 0, 0, USB_TRANSFER_DIRECTION_OUT);
        Dbg_Log_Int(XCAM_TAG, "SET_ADDRESS st", (int)st);
        if (st != 0) { Cam_Log("SET_ADDRESS failed"); return; }

        Sleep(5);                 /* USB 2ms address-recovery (generous) */
        nb[5] = addr;             /* node now targets the new address */
        Cam_CloseDefaultEP(node); /* re-open EP0 so it points at the new address */
        st = Cam_OpenDefaultEP(node);
        Dbg_Log_Int(XCAM_TAG, "EP0 reopen st", (int)st);
        if ((LONG)st < 0) { Cam_Log("EP0 reopen failed"); return; }
    }

    st = Cam_Control(node, 0x00, 0x09 /*SET_CONFIGURATION*/, 1, 0, 0, 0, USB_TRANSFER_DIRECTION_OUT);
    Dbg_Log_Int(XCAM_TAG, "SET_CONFIG st", (int)st);

    {
        int ifnum = 0, alt = 0, epAddr = 0, maxpkt = 0;
        if (!Cam_FindIsoEndpoint(node, 3 /*320x240*/, &ifnum, &alt, &epAddr, &maxpkt)) {
            Cam_Log("no iso IN endpoint in config descriptor");
            return;
        }
        Dbg_Log_Int(XCAM_TAG, "iso ifnum", ifnum);
        Dbg_Log_Int(XCAM_TAG, "iso alt", alt);
        Dbg_Log_Int(XCAM_TAG, "iso epAddr", epAddr);
        Dbg_Log_Int(XCAM_TAG, "iso maxpkt", maxpkt);
        rc = Cam_StartStream(node, ifnum, alt, epAddr, maxpkt);
        Dbg_Log_Int(XCAM_TAG, "StartStream rc", rc);
    }
}

static void Cam_InspectNode(IUsbDevice* dev, int idx)
{
    const USB_INTERFACE_DESCRIPTOR* id;
    const USB_ENDPOINT_DESCRIPTOR* ep;
    ULONG port; int cls; int vid; int pid; int hasIso;
    Dbg_Log_Int(XCAM_TAG, "--- node", idx);
    port = dev->GetPort();

    /* root hubs (parent==0x80) are VIRTUAL: their device ptr is uninitialized
       (0xCCCCCCCC) and SubmitRequest on them faults. Only transfer to real
       downstream devices. */
    {
        unsigned char parent = ((const unsigned char*)(const void*)dev)[1];
        if (parent == CAM_IDX_NONE) {
            Cam_Log("  (root hub -- skip control xfer)");
            return;
        }
    }

    /* real per-device identity (this is per-device, unlike the shared iface global) */
    vid = -1; pid = -1;
    if (Cam_GetVidPid(dev, &vid, &pid)) {
        Dbg_Log_Int(XCAM_TAG, "  VID(dec)", vid);
        Dbg_Log_Int(XCAM_TAG, "  PID(dec)", pid);
    }
    else {
        Cam_Log("  (no device descriptor)");
    }

    /* TI internal hub (0451:2046) -> scan its ports to find the camera */
    if (vid == 0x0451 && pid == 0x2046) {
        Cam_Log("  TI hub -- scanning downstream ports");
        s_hubDev = dev;
        Cam_ScanHub(dev, -1);
    }

    cls = -1;
    id = dev->GetInterfaceDescriptor();
    if (Cam_Readable(id, 9) && id->bLength >= 9 && id->bLength < 64)
        cls = (int)id->bInterfaceClass;

    hasIso = 0;
    if (cls >= 0) {
        ep = dev->GetEndpointDescriptor(USB_ENDPOINT_TYPE_ISOCHRONOUS, 1, 0);
        if (Cam_Readable(ep, 7) && ep->bLength >= 7 && ep->bLength <= 9) {
            hasIso = 1; s_epAddr = (int)ep->bEndpointAddress; s_epMaxPkt = (int)ep->wMaxPacketSize;
        }
    }

    /* presence: any 0xFF / iso signal still proves the camera is on the bus */
    if (cls == CAM_CLASS_VENDOR || hasIso) s_present = 1;

    /* DEFINITIVE: only claim s_camDev when the REAL VID/PID is a camera */
    if (Cam_IsCameraId(vid, pid)) {
        Cam_Log("  *** REAL CAMERA device (VID/PID match) ***");
        s_camDev = dev; s_port = (int)port; s_camFound = 1;
    }
}

static void Cam_WalkTree(void)
{
    char* base; IUsbDevice* stack[CAM_MAX_NODES]; unsigned char visited[CAM_MAX_NODES];
    int sp, count, i;
    base = Cam_GetNodeBase();
    if (base == 0) { Cam_Log("walk: base unreadable"); return; }
    for (i = 0; i < CAM_MAX_NODES; i++) visited[i] = 0;
    sp = 0; count = 0;
    for (i = 0; i < CAM_MAX_NODES; i++) {
        IUsbDevice* ni = Cam_Node(base, i); unsigned char* b;
        if (ni == 0) continue; b = (unsigned char*)(void*)ni;
        if (b[1] == CAM_IDX_NONE && b[2] != CAM_IDX_NONE && Cam_Node(base, b[2]) != 0) { if (sp < CAM_MAX_NODES) stack[sp++] = ni; }
    }
    { IUsbDevice* n0 = Cam_Node(base, 0); if (n0 && sp < CAM_MAX_NODES) stack[sp++] = n0; }
    while (sp > 0 && count < CAM_MAX_NODES) {
        IUsbDevice* dev = stack[--sp]; int idx = Cam_NodeIndex(base, dev);
        unsigned char* b; int ci, guard;
        if (idx < 0 || visited[idx]) continue;
        visited[idx] = 1; count++;
        Cam_InspectNode(dev, idx);
        b = (unsigned char*)(void*)dev; ci = (int)b[2]; guard = 0;
        while (ci != CAM_IDX_NONE && guard < CAM_MAX_NODES) {
            IUsbDevice* c = Cam_Node(base, ci); unsigned char* cb; int cidx;
            if (c == 0) break; cb = (unsigned char*)(void*)c; cidx = Cam_NodeIndex(base, c);
            if (cidx >= 0 && !visited[cidx] && sp < CAM_MAX_NODES) stack[sp++] = c;
            ci = (int)cb[3]; guard++;
        }
    }
    Dbg_Log_Int(XCAM_TAG, "nodes visited", count);
}

/*===========================================================================
    XCam_* bridge
===========================================================================*/
extern "C" void XCam_SetLog(CamLogFn fn) { s_logfn = fn; }

extern "C" int XCam_Init(DWORD dwPort)
{
    (void)dwPort;
    Cam_Log("XCam_Init: detect + start stream");
    s_present = 0; s_camDev = 0; s_streaming = 0; s_camFound = 0; s_camHubPort = -1; s_hubDev = 0;
    Cam_WalkTree();
    if (!s_present) return XCAM_STATUS_NO_DEVICE;

    if (!s_camFound) {
        Cam_Log("camera PRESENT but NOT a claimable VID/PID node");
        if (s_camHubPort >= 0 && s_hubDev) {
            Dbg_Log_Int(XCAM_TAG, "  camera is on TI-hub port", s_camHubPort);
            if (Cam_ResetPort(s_hubDev, s_camHubPort) == 0)
                Cam_BringUpManual(s_hubDev, s_camHubPort);
        }
        else {
            Cam_Log("  camera port not found via hub scan");
        }
        return XCAM_STATUS_OK;
    }

    /* (walk found an already-enumerated camera node directly: parse + stream it) */
    Dbg_Log_Int(XCAM_TAG, "REAL camera node in tree; port", s_port);
    {
        int ifnum = 0, alt = 0, epAddr = 0, maxpkt = 0;
        if (Cam_FindIsoEndpoint(s_camDev, 3 /*320x240*/, &ifnum, &alt, &epAddr, &maxpkt)) {
            int rc = Cam_StartStream(s_camDev, ifnum, alt, epAddr, maxpkt);
            Dbg_Log_Int(XCAM_TAG, "StartStream rc", rc);
        }
    }
    return XCAM_STATUS_OK;
}

extern "C" int XCam_IsStreaming(void) { return s_streaming; }

/* picojpeg input-feed callback: hand it bytes from the copied JPEG frame. */
static unsigned char Cam_JpgFeed(unsigned char* pBuf, unsigned char bufSize,
    unsigned char* pBytesRead, void* pData)
{
    int n = bufSize;
    (void)pData;
    if (n > s_jpgRem) n = s_jpgRem;
    { int i; for (i = 0; i < n; i++) pBuf[i] = s_jpgPtr[i]; }
    s_jpgPtr += n; s_jpgRem -= n;
    *pBytesRead = (unsigned char)n;
    return 0;
}

/* clamp helper */
static unsigned char Cam_Clamp8(int v) { if (v < 0) return 0; if (v > 255) return 255; return (unsigned char)v; }

/* Decode the current JPEG frame (s_jpegBuf, len) into the persistent 4-byte
   texture source buffer s_rgb (pitch = CAM_TEX_W*4). The function name is
   legacy from the earlier YUY2 experiment; the current output path feeds a
   swizzled D3DFMT_A8R8G8B8 texture. */
static int Cam_DecodeJpegToYUY2(int jlen)
{
    pjpeg_image_info_t info;
    unsigned char rc;
    int mcuX = 0, mcuY = 0;
    int W = XCAM_FRAME_W, H = XCAM_FRAME_H;
    int pitch = CAM_TEX_W * 4;   /* full-texture row stride */
    int yMin = 255, yMax = 0; long ySum = 0; int yCount = 0;
    static int s_statLog = 0;
    int doStat = ((s_statLog++ % 120) == 0);

    s_jpgPtr = s_jpegBuf; s_jpgRem = jlen;
    rc = pjpeg_decode_init(&info, Cam_JpgFeed, 0, 0);
    if (rc != 0) { Dbg_Log_Int(XCAM_TAG, "pjpeg init rc", (int)rc); return -1; }

    {
        static int s_dlog = 0;
        if ((s_dlog++ % 120) == 0) {
            Dbg_Log_Int(XCAM_TAG, "jpg width", info.m_width);
            Dbg_Log_Int(XCAM_TAG, "jpg height", info.m_height);
            Dbg_Log_Int(XCAM_TAG, "jpg comps", info.m_comps);
            Dbg_Log_Int(XCAM_TAG, "jpg scanType", (int)info.m_scanType);
            Dbg_Log_Int(XCAM_TAG, "jpg MCUSPerRow", info.m_MCUSPerRow);
            Dbg_Log_Int(XCAM_TAG, "jpg MCUSPerCol", info.m_MCUSPerCol);
        }
    }

    for (;;) {
        int nbx, nby, bi, ofsTab[4], bxTab[4], byTab[4], nblk;
        rc = pjpeg_decode_mcu();
        if (rc == PJPG_NO_MORE_BLOCKS) break;
        if (rc != 0) { Dbg_Log_Int(XCAM_TAG, "pjpeg mcu rc", (int)rc); break; }

        nbx = info.m_MCUWidth >> 3;
        nby = info.m_MCUHeight >> 3;
        nblk = nbx * nby;
        if (nblk == 1) { ofsTab[0] = 0; bxTab[0] = 0; byTab[0] = 0; }
        else if (nbx == 2 && nby == 1) { ofsTab[0] = 0; bxTab[0] = 0; byTab[0] = 0; ofsTab[1] = 64; bxTab[1] = 1; byTab[1] = 0; }
        else if (nbx == 1 && nby == 2) { ofsTab[0] = 0; bxTab[0] = 0; byTab[0] = 0; ofsTab[1] = 128; bxTab[1] = 0; byTab[1] = 1; }
        else {
            ofsTab[0] = 0; bxTab[0] = 0; byTab[0] = 0; ofsTab[1] = 64; bxTab[1] = 1; byTab[1] = 0;
            ofsTab[2] = 128; bxTab[2] = 0; byTab[2] = 1; ofsTab[3] = 192; bxTab[3] = 1; byTab[3] = 1;
        }

        for (bi = 0; bi < nblk; bi++) {
            int ofs = ofsTab[bi];
            int bpx = mcuX * info.m_MCUWidth + bxTab[bi] * 8;
            int bpy = mcuY * info.m_MCUHeight + byTab[bi] * 8;
            int r, c;
            for (r = 0; r < 8; r++) {
                int py = bpy + r;
                unsigned char* row;
                if (py >= H) continue;
                row = s_rgb + py * pitch;
                for (c = 0; c < 8; c++) {
                    int px = bpx + c, idx, R, G, B;
                    if (px >= W) continue;
                    idx = ofs + r * 8 + c;
                    R = info.m_pMCUBufR[idx];
                    if (info.m_comps == 1) {
                        row[px * 4 + 0] = (unsigned char)R;   /* B */
                        row[px * 4 + 1] = (unsigned char)R;   /* G */
                        row[px * 4 + 2] = (unsigned char)R;   /* R */
                        row[px * 4 + 3] = 0xFF;               /* A */
                        if (doStat) { if (R < yMin) yMin = R; if (R > yMax) yMax = R; ySum += R; yCount++; }
                        continue;
                    }
                    G = info.m_pMCUBufG[idx];
                    B = info.m_pMCUBufB[idx];
                    row[px * 4 + 0] = (unsigned char)R;   /* B slot <- R */
                    row[px * 4 + 1] = (unsigned char)G;   /* G */
                    row[px * 4 + 2] = (unsigned char)B;   /* R slot <- B */
                    row[px * 4 + 3] = 0xFF;               /* A */
                    if (doStat) { int Y = (77 * R + 150 * G + 29 * B) >> 8; if (Y < yMin) yMin = Y; if (Y > yMax) yMax = Y; ySum += Y; yCount++; }
                }
            }
        }

        mcuX++;
        if (mcuX == info.m_MCUSPerRow) { mcuX = 0; mcuY++; }
    }
    if (doStat) {
        Dbg_Log_Int(XCAM_TAG, "luma min", yMin);
        Dbg_Log_Int(XCAM_TAG, "luma max", yMax);
        Dbg_Log_Int(XCAM_TAG, "luma avg", yCount ? (int)(ySum / yCount) : -1);
        Dbg_Log_Int(XCAM_TAG, "center R(160,120)", (int)s_rgb[120 * pitch + 160 * 4 + 2]);
    }
    return 0;
}

/* Blit the latest decoded frame into the A8R8G8B8 preview texture every call. */
extern "C" int XCam_DrawToSurface(IDirect3DTexture8* pTex)
{
    D3DLOCKED_RECT lr;
    int jlen;
    static int s_drawCalls = 0;
    if (pTex == 0 || s_rgb == 0) return -1;

    /* decode only when a new frame has completed -> s_rgb */
    if (s_completedFrames != s_lastDecoded) {
        jlen = s_frameReadyLen;
        if (jlen > 4 && jlen <= CAM_FRAME_CAP) {
            { int i; for (i = 0; i < jlen; i++) s_jpegBuf[i] = s_frameReady[i]; }
            s_lastDecoded = s_completedFrames;
            if ((s_drawCalls++ % 120) == 0) {
                Dbg_Log_Int(XCAM_TAG, "completed frames", s_completedFrames);
                Dbg_Log_Int(XCAM_TAG, "  jpegLen", jlen);
                Dbg_Log_Int(XCAM_TAG, "  soi", (int)((s_jpegBuf[0] << 8) | s_jpegBuf[1]));
                Dbg_Log_Int(XCAM_TAG, "  eoiTail", (int)((s_jpegBuf[jlen - 2] << 8) | s_jpegBuf[jlen - 1]));
            }
            Cam_DecodeJpegToYUY2(jlen);
        }
    }

    /* Push s_rgb to the texture. The cam texture is a swizzled
       D3DFMT_A8R8G8B8 surface (same proven path as the on-screen font), so we
       XGSwizzleRect the linear 4-byte source buffer in. Texture is pow2
       (512x256); only the top-left 320x240 contains the image. */
    {
        D3DSURFACE_DESC desc;
        if (FAILED(pTex->GetLevelDesc(0, &desc))) return -1;
        if (FAILED(pTex->LockRect(0, &lr, 0, 0))) return -1;
        /* full-size source, NULL rect -- the proven font.cpp swizzle pattern */
        XGSwizzleRect(s_rgb, CAM_TEX_W * 4, 0, lr.pBits, desc.Width, desc.Height, 0, 4);
        pTex->UnlockRect(0);
    }
    return 0;
}

extern "C" void XCam_Shutdown(void)
{
    if (s_isoBuf) { MmFreeContiguousMemory(s_isoBuf);      s_isoBuf = 0; }
    if (s_frame) { MmFreeContiguousMemory(s_frame);       s_frame = 0; }
    if (s_frameReady) { MmFreeContiguousMemory(s_frameReady);  s_frameReady = 0; }
    if (s_jpegBuf) { MmFreeContiguousMemory(s_jpegBuf);     s_jpegBuf = 0; }
    if (s_rgb) { MmFreeContiguousMemory(s_rgb);         s_rgb = 0; }
    s_streaming = 0;
    Cam_Log("XCam_Shutdown");
}
