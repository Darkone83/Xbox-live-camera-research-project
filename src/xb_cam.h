#ifndef XB_CAM_H
#define XB_CAM_H
/*===========================================================================
    xb_cam.h -- Xbox USB Camera interface
                Reverse-engineered from Xbox Video Chat XBE (MS-124)

    Sources:
        - Xbox Video Chat XBE (default.xbe) full Ghidra C decompilation
        - xboxkrnl.pdb kernel symbol table (kernel export cross-reference)
        - Xbox Dev Wiki: Xbox Cam (OV530/OV519 chipset, OHCI EP1 IN)
        - Xbox-Headers.zip (XTL.h, Xbox.h, xvoice.h)
        NOTE: XDEVICE_TYPE_*_TABLE and XVoiceCreateMediaObject are XAPI
        (statically-linked) symbols, NOT kernel exports.

    ═══════════════════════════════════════════════════════════════════════
    ARCHITECTURE CORRECTION (2026-05, from pdb + decompilation cross-ref)
    ═══════════════════════════════════════════════════════════════════════

    The original model below (open a device handle from CameraStatusPath, then
    drive it with NtDeviceIoControlFile using codes 0x100/0x101/0x107) is
    WRONG. Cross-referencing xboxkrnl.pdb with the decompiled XBE shows:

      * The kernel exports NO registry-key API (no NtSetValueKey / NtOpenKey /
        NtQueryValueKey). CameraSetting config in the XBE is done through an
        internal COM-like registry-provider vtable (camera-object slots 3-11),
        which is dashboard/system-app infrastructure -- not reachable from a
        normal homebrew title, and not the Win32 Reg* API (which doesn't exist
        on Xbox at all).

      * The camera device is NEVER opened as a file/handle. There is no
        \Device\Camera... NtOpenFile/NtCreateFile anywhere in the XBE.

      * The "IOCTLs" 0x100/0x101/0x107 are NOT kernel IOCTLs. They are the
        minidriver's INTERNAL KS dispatch, routed in-process through
        FUN_000cf340(object, ...) -- a plain function call on the camera
        object, not NtDeviceIoControlFile.

      * Actual USB I/O is done by talking to the OHCI host controller directly,
        in-process (FUN_001be430 channel-open magic 0x4030, FUN_001bfd30 ISO
        setup, FUN_001bf360/FUN_001bf440 endpoint + alt-setting).

    CONCLUSION: the usbcamd minidriver is effectively in-process in the
    XBE/dashboard. A homebrew camera driver must operate at the USB/OHCI
    level -- claim the device on the host controller, run the OV519/OV530
    control-endpoint register init, set up the ISO IN endpoint, and pump
    frames. It is NOT an "open handle + IOCTL" client.

    The tables and offsets below remain accurate as documentation of the
    driver's INTERNAL model (vtable layout, internal dispatch codes, object
    field offsets). They are reference for building the real driver -- they
    are NOT a directly-callable kernel/Win32 API.

    --- ORIGINAL (INCORRECT) MODEL, kept for context: -----------------------
    The XBE was thought to communicate with usbcamd through:
        1. (registry writes to configure the driver)   <- vtable-mediated
        2. (IOCTL dispatch for stream control)          <- internal, not kernel
        3. (open device handle from CameraStatusPath)   <- never happens
    -------------------------------------------------------------------------

    The camera object (0x844 bytes, zeroed on alloc) is an internal
    kernel streaming (KS) filter object. It holds:
        - 5 vtable pointer groups at slots [0]..[5]
        - 23+ command handler setter/getter pairs (fn ptr per registry key)
        - USB state, EEPROM addr, stream state, frame counters

    ═══════════════════════════════════════════════════════════════════════
    VTABLE LAYOUT (confirmed from decompilation)
    ═══════════════════════════════════════════════════════════════════════

    Slot  Byte   Function confirmed
    ─────────────────────────────────────────────────────────────────────
     3    0x0C   Registry key read (QueryKey)
     4    0x10   Registry value free / object release
     5    0x14   Registry subkey open (open/create sub-path)
     6    0x18   Registry open with access mask
     7    0x1C   Registry open root key
     8    0x20   Object alloc (via camera allocator)
     9    0x24   EnableSystem / OpenKey (used in 5 functions)
    10    0x28   DisableSystem / CloseKey (used in 3 functions)
    11    0x2C   CloseHandle (most-used slot, 11x - registry close)
    12    0x30   Command execute (setter dispatch)
    13    0x34   Command query (getter dispatch)
    14    0x38   Stream state query
    15    0x3C   Unknown
    16    0x40   Unknown
    17    0x44   Stream stop
    18    0x48   Stream reset / query
    19    0x4C   Unknown
    21    0x54   TurnOnLed (called from EnableSystem path)
    22    0x58   LED state set (compare 0x7c1 vs 0xd1)
    24    0x60   DisableSystem path
    31    0x7C   Format set (called with IOCTL_SET_FORMAT params)
    68    0x110  Interface query (COM QueryInterface)
    69    0x114  Interface AddRef
    70    0x118  Interface Release
    72    0x120  Unknown (3 uses)

    ═══════════════════════════════════════════════════════════════════════
    COMMAND DISPATCH TABLE (all confirmed from UndefinedFunction_000ccad0)
    ═══════════════════════════════════════════════════════════════════════

    Each entry registered via:
        FUN_000cb4b0(regHandle, keyName, nameByteLen, &setter, &getter)

    Key name               Byte len  Setter slot  Getter slot
    ─────────────────────────────────────────────────────────
    EnableSystem           0x1A      [0x23]       [0x0E]
    DisableSystem          0x1C      [0x22]       [0x0D]
    SetUsbWork             0x16      [0x1F]       [0x0A]
    SetUsbInit             0x16      [0x20]       [0x0B]
    PowerDownCamera        0x20      [0x25]       [0x10]
    PowerOnCamera          0x1C      [0x24]       [0x0F]
    ResetUsb               0x12      [0x21]       [0x0C]
    ClearSnapButton        0x20      [0x2D]       [0x1A]
    EnableAutoLaunch       0x22      [0x2B]       [0x18]
    DisableAutoLaunch      0x24      [0x2C]       [0x19]
    CheckAutoLaunch        0x20      [0x1CA]      (stack ptr)
    EnableSsuspend         0x1E      [0x27]       [0x12]
    DisableSsuspend        0x20      [0x26]       [0x11]
    TurnOnLed              0x14      [0x28]       [0x13]
    TurnOffLed             0x16      [0x29]       [0x14]
    BlockStream            0x18      [0x1E]       [0x1B]
    StartStream            0x18      [0x1D]       [0x1C]
    CameraTimeout          0x1C      [0x30]       [0x31]
    UsbSetting             0x16      [0x2F]       [0x17]  (FUN_000cb950)
    CameraSetting          0x1C      [0x2E]       [0x16]  (FUN_000cd9d0)

    ═══════════════════════════════════════════════════════════════════════
    IOCTL DISPATCH (confirmed from FUN_000cf340)
    ═══════════════════════════════════════════════════════════════════════

    Code    Handler              Description
    ──────────────────────────────────────────────────────────────────────
    0x100   FUN_000ce510         Open/enumerate streams
                                 Sets up stream descriptors (up to 2 streams)
                                 Reads param_1[0x80] = stream count
    0x101   FUN_000cf120         Set stream format
                                 Validates format struct (min size 0x98)
                                 Calls FUN_001bf360 + FUN_001bf440
    0x102   FUN_000ce5c0         Select stream by index
    0x107   FUN_000cdd80         Start streaming
                                 Init ISO channel (magic 'DEVA' 0x45564544)
                                 Calls FUN_001bfd30 (semaphore + ISO setup)
    0x108   FUN_001bfad0         Stop streaming
    0x109   FUN_000ce490         Query stream state
                                 Calls FUN_001be6b0 on completion
    0x10A   FUN_000cf040         Set format if marker byte == 0x1B
    0x10B   (no-op)              Complete with status 0
    0x10D   FUN_001bef70         Reset
    0x10E   (inline)             Enumerate pins (up to 3 pins checked)

    ═══════════════════════════════════════════════════════════════════════
    OBJECT STRUCT OFFSETS (DWORD indices unless noted)
    ═══════════════════════════════════════════════════════════════════════

    State flags (byte offsets):
        0x7C2  USB enabled flag      (byte: 0=uninit 1=enabled, guard in FUN_000cad00)
        0x7C1  Current LED state     (byte)
        0x7C5  Suspend state flag    (byte)
        0x7C6  Auto-close flag       (byte)
        0xD1   Target LED state      (byte, compared with 0x7C1 in EnableSystem)
        0xBD   Interlace check flag  (byte, used in IOCTL 0x101)
        0xBE   Stream abort flag     (byte, checked in FUN_000ced10)
        0xBF   Unknown stream flag   (byte)
        0xC4   Feature flags         (byte, bit2=format match, bit3=push mode)

    DWORD fields:
        [0x08]  SupportEvent value     (from registry)
        [0x09]  BandwidthAllocateRule  (from registry)
        [0x1BE] Interface ptr A        (used with DAT_001edc78)
        [0x1BF] Interface ptr A        (used with DAT_001edc78, DAT_001edc48)
        [0x1C0] StillSupportType ptr   (from FUN_000ca2c0)
        [0x1C2] Interface ptr B        (used with DAT_001edc88)
        [0x1C3] Interface ptr C        (used with DAT_001edc48)
        [0x1C5] Interface ptr D
        [0x1C6] Interface ptr E
        [0x1C7] Interface ptr F
        [0x1C8] Stream active flag     (0=idle 1=streaming)
        [0x1C9] Registry event handle
        [0x1CE] DefaultQualityLevel    (2 if registry read fails)
        [0x1CF..0x1DE] DefaultYQuanTable   (16 DWORDs)
        [0x1DF..0x1EE] DefaultUVQuanTable  (16 DWORDs)
        [0x1F0] LED state copy         (saved on Enable, restored on Disable)
        [0x1F1] AutoClose flag         (byte in DWORD)
        [0x1F5] ShowCameraId flag      (byte)
        [0x20C] CameraStatusPath buf   (allocated, path to device)
        [0x20D] Status2HKR flag        (byte)
        [0x20E] UsbSetting             (byte)
        [0x20F] Allocator ptr          (memory manager interface)
        [0x210] CustomID value         (byte)
        [0x33]  CreateFileName ptr     (used in FUN_000cbf30)
        [0x36]  Semaphore handle       (KeInitializeSemaphore target)
        [0x6FC] Stream descriptor 0   (type 0)
        [0x700] Stream descriptor 1   (type 1)
        [0x70C] Stream semaphore ptr
        [0x728] Unknown ptr
        [0x72C] Unknown ptr
        [0x83C] Allocator ptr (same as 0x20F, second ref)
        [0x838] Serial number buffer ptr (from FUN_000cb9c0 EEPROM read)
        [0x835] E2PROM address         (byte, read from registry)

    ═══════════════════════════════════════════════════════════════════════
    REGISTRY KEYS (all confirmed from decompilation)
    ═══════════════════════════════════════════════════════════════════════

    Base: HKLM\System\CurrentControlSet\Services\Class\CameraSetting\

    Primary control keys (written to drive lifecycle):
        SetUsbInit            PowerOnCamera         SetUsbWork
        EnableSystem          DisableSystem         PowerDownCamera
        ResetUsb              StartStream           BlockStream
        CameraTimeout         IdleAltSetting        BandwidthAllocateRule

    Config read on init:
        CameraStatusPath      Status2HKR            CustomID
        StillSupportType      SupportStillPin       SupportEvent
        DefaultQualityLevel   EnableAutoClose        UseGpio0
        AddSerialNumber       ShowCameraId          PowerControl
        DefaultYQuanTable     DefaultUVQuanTable     E2PROMAddress
        EnableAutoLaunch      DisableAutoLaunch      CheckAutoLaunch
        EnableSsuspend        DisableSsuspend        ClearSnapButton
        TurnOnLed             TurnOffLed

    Stream properties (read/written during streaming):
        FourCC                Width                  Height
        FrameRate             CurrentFrameRate       MinFrameRate
        MaxFrameRate          TypFrameRate           BitCount
        Progressive           StreamType             QualityLevel
        SensorWidth           SensorHeight           RawFrameLength
        RawData               AlternateSetting       UsbCBR
        CheckInterlace        CheckTime              ExposureCheck
        EnableLowLightControl EnableSwControl        DisableAutoLFCheck

    Video proc amp / camera control:
        VideoProcAmp          VideoControl           VideoCompression
        CameraControl         CameraDataType         AdjustYUVCamSetting
        AdjustYUVUsbSetting   ReverseUVCamSetting    YQuanTable
        UVQuanTable           BadPixel               BadPixelMemSize
        PropertyId            DefaultValue           DefaultFlags
        LastValue             LastFlags              MaxValue
        MinValue              Step                   CustomProperty

    Stream open/close notification keys:
        OpenStreamCameraSetting    CloseStreamCameraSetting
        OpenStreamUsbSetting       CloseStreamUsbSetting

    Clock scaling keys:
        ClockUpCamRegs        ClockDownCamRegs       ClockUpUsbRegs
        ClockDownUsbRegs      PreClockUpCamRegs      PostClockUpCamRegs
        PreClockDownCamRegs   PostClockDownCamRegs   PreClockUpUsbRegs
        PostClockUpUsbRegs    PreClockDownUsbRegs    PostClockDownUsbRegs
        ClockUpTh             ClockDownTh

    Misc:
        CameraSetting\SupportCamera  (subkey, holds CurrentFormat)
        AttachedCamera               (event name for attach notification)
        Camera DeAttached            (event name for detach notification)
        SaveFormat                   SaveLastFrame
        SnapShot                     ButtonPushed
        PushModeEvent                RestoreCamReg
        RestoreUsbReg                CreateFileName
        FriendlyName                 DriverDesc
        FileDir

    Under usbcamd\:
        DebugTraceLevel

    ═══════════════════════════════════════════════════════════════════════
    COM INTERFACE IDs (internal, confirmed from camera section calls)
    ═══════════════════════════════════════════════════════════════════════

    These are passed as second arg to interface query calls and act as
    COM IID selectors. Meaning inferred from context.

        DAT_001ed398   -- stream descriptor interface
        DAT_001ed8c0   -- pin/stream enumeration
        DAT_001edad4   -- unknown
        DAT_001edaf8   -- unknown
        DAT_001edba8   -- stream buffer interface
        DAT_001edbd8   -- main stream interface A (start/stop path)
        DAT_001edbe8   -- unknown
        DAT_001edbf8   -- unknown
        DAT_001edc08   -- stream interface (used in FUN_000cf7c0 state check)
        DAT_001edc18   -- unknown
        DAT_001edc28   -- unknown
        DAT_001edc38   -- unknown
        DAT_001edc48   -- stream pin interface (stop path)
        DAT_001edc58   -- frame/packet interface
        DAT_001edc68   -- frame delivery interface
        DAT_001edc78   -- main control interface (most-used)
        DAT_001edc88   -- event interface (attach/detach)
        DAT_001edc98   -- format negotiation interface
        DAT_001edca8   -- format descriptor interface
        DAT_001edcb8   -- unknown
        DAT_001edcc8   -- device object interface (NtCreateFile path)
        DAT_001edcd8   -- unknown
        DAT_001edd80   -- unknown

    ═══════════════════════════════════════════════════════════════════════
    PIXEL FORMAT FourCC CODES
    ═══════════════════════════════════════════════════════════════════════

    Written to CameraSetting\SupportCamera\7649519\CurrentFormat (DWORD).
    YUY2 is preferred -- uploads directly to D3DFMT_YUY2 texture.

        XCAM_FMT_YUY2  0x32595559   D3DFMT_YUY2 compatible, preferred
        XCAM_FMT_IYUV  0x56555949
        XCAM_FMT_I420  0x30323449
        XCAM_FMT_YV12  0x32315659
        XCAM_FMT_AYUV  0x56555941
        XCAM_FMT_UYVY  0x59565955
        XCAM_FMT_YVYU  0x55595659
        XCAM_FMT_P422  0x32323450
        XCAM_FMT_NV11  0x3131564E
        XCAM_FMT_NV12  0x3231564E

    ═══════════════════════════════════════════════════════════════════════
    STARTUP SEQUENCE (confirmed from full call graph)
    ═══════════════════════════════════════════════════════════════════════

    1. Poll XGetDeviceChanges until camera detected on port
    2. Allocate camera object: ExAllocatePool(0x844), memset 0
    3. Install 5 vtable groups at offsets [0]..[4] of camera object
    4. FUN_000ccad0 (registry init):
         - Read all CameraSetting registry values into object fields
         - Register all command handlers via FUN_000cb4b0
         - Read DefaultYQuanTable + DefaultUVQuanTable (JPEG quantization)
         - Read E2PROMAddress -> param_1[0x835]
         - FUN_000cb9c0: read EEPROM serial number via I2C registers 2/3/4
    5. FUN_000cad00 (EnableSystem):
         - Guard: if param_1[0x7C2] != 0 return (already enabled)
         - Set param_1[0x7C2] = 1
         - Call vtable[9] (OpenKey / EnableSystem command)
         - Call vtable[25] (HalReadSMBusValue / USB init)
         - Sync LED: if LED state mismatch call vtable[22]
    6. IOCTL 0x100 (FUN_000ce510): enumerate streams
         - Reads stream count from param_1[0x80]
         - Fills stream descriptor array (0x50 bytes each)
         - Sets up data buffer ptrs (DAT_0024cbb0, DAT_0024cba4)
    7. IOCTL 0x101 (FUN_000cf120): set format
         - Validates format descriptor (min 0x98 bytes)
         - Calls FUN_000ce020 (format size calc)
         - Calls FUN_001bf360 (channel init, OHCI endpoint setup)
         - Calls FUN_001bf440 (alternate setting selection)
    8. IOCTL 0x107 (FUN_000cdd80): start streaming
         - Writes magic 'DEVA' (0x45564544) to stream struct
         - Calls FUN_001bfd30:
             KeInitializeSemaphore x2
             ExAllocatePool(0x12) for ISO transfer descriptor
             FUN_001be430 (USB channel open, magic 0x4030)
             FUN_001bfc50 (OHCI endpoint configure)
         - Sets puVar4[0x20]=1, [0x30]=1, [0x21]=1 then =2
    9. Open device handle:
         - Read CameraStatusPath from object[0x20C] (driver fills this)
         - NtOpenFile(&hCamera, ACCESS, &objAttr, &ioStatus, SHARE, FLAGS)
    10. Frame pump thread (PsCreateSystemThreadEx):
         - Loop: NtReadFile or XVoiceSubmitPacket on hCamera
         - FUN_000ce6a0 / FUN_000ced10 are the frame completion handlers
         - Frame data at IRP[0x18]+0x30 (confirmed in FUN_000ced10)
         - Timestamps in stream struct at [0x138][0x13C][0x140][0x144]

    SHUTDOWN SEQUENCE:
    1. IOCTL 0x108 (FUN_001bfad0): stop streaming
    2. FUN_000cad60 (DisableSystem):
         - Guard: if param_1[0x7C2] == 0 return
         - Set param_1[0x7C2] = 0
         - Call vtable[24], vtable[10]
         - Restore LED state
    3. NtClose(hCamera)
    4. ExFreePool(camera_object)

    ═══════════════════════════════════════════════════════════════════════
    FRAME DELIVERY INTERNALS (FUN_000ced10 confirmed)
    ═══════════════════════════════════════════════════════════════════════

    On frame completion:
        IRP buffer check: *puVar3 == 0x45425253 ('SRBE' magic)
        Frame data ptr: IRP[0x18]+0x28 = data length
                        IRP[0x18]+0x24 = data buffer
        Format at stream_struct[0x170]+0x28 = frame width spec
        Format at stream_struct[0x170]+0x2C = frame period (100ns units)
        Timestamp calculation uses __alldiv on period
        Frame counter at stream_struct[0x138..0x144]
        Reset flag at stream_struct[0x184] (resets frame counter)
        Calls FUN_001bea90(device, stream, IRP) to complete IRP

    ═══════════════════════════════════════════════════════════════════════
    D3D8 TEXTURE UPLOAD
    ═══════════════════════════════════════════════════════════════════════

    YUY2 frames upload directly to D3DFMT_YUY2 (Xbox NV2A extension).
    The GPU converts YUV->RGB on the fly during texture sampling.

    pDevice->CreateTexture(320, 240, 1, 0,
                           D3DFMT_YUY2, D3DPOOL_DEFAULT, &pCamTex);
    // Per frame:
    D3DLOCKED_RECT lr;
    pCamTex->LockRect(0, &lr, NULL, 0);
    memcpy(lr.pBits, frame_data, 320 * 240 * 2);
    pCamTex->UnlockRect(0);
    pDevice->SetTexture(0, pCamTex);

===========================================================================*/

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* ── USB VID/PID ──────────────────────────────────────────────────────────── */
#define XCAM_VID_XBOX           0x045E
#define XCAM_PID_XBOX           0x028C
#define XCAM_VID_EYETOY         0x054C
#define XCAM_PID_EYETOY         0x0155

/* ── Camera object ────────────────────────────────────────────────────────── */
#define XCAM_OBJECT_SIZE        0x844   /* ExAllocatePool size, confirmed     */
#define XCAM_CHAN_MAGIC         0x4348414E  /* 'CHAN' ISO channel struct      */
#define XCAM_STREAM_MAGIC       0x45564544  /* 'DEVA' stream struct marker    */
#define XCAM_IRP_MAGIC          0x45425253  /* 'SRBE' IRP completion marker   */

/* ── Object byte offsets (not DWORD indices) ─────────────────────────────── */
#define XCAM_OFF_USB_ENABLED    0x7C2   /* byte: 0=uninit 1=enabled          */
#define XCAM_OFF_LED_CURRENT    0x7C1   /* byte: current LED state           */
#define XCAM_OFF_LED_TARGET     0xD1    /* byte: target LED state            */
#define XCAM_OFF_SUSPEND        0x7C5   /* byte: suspend state               */
#define XCAM_OFF_AUTOCLOSE      0x7C6   /* byte: auto-close flag             */
#define XCAM_OFF_INTERLACE      0xBD    /* byte: interlace check             */
#define XCAM_OFF_ABORT          0xBE    /* byte: stream abort flag           */
#define XCAM_OFF_FEATURES       0xC4    /* byte: feature bits                */
#define XCAM_OFF_EEPROM_ADDR    0x835   /* byte: E2PROM address              */
#define XCAM_OFF_EEPROM_BUF     0x838*4 /* ptr: serial number buffer        */
#define XCAM_OFF_STREAMING      0x1C8*4 /* DWORD: 0=idle 1=streaming        */
#define XCAM_OFF_STATUS_BUF     0x20C*4 /* ptr: CameraStatusPath buffer     */
#define XCAM_OFF_ALLOCATOR      0x20F*4 /* ptr: memory allocator interface  */
#define XCAM_OFF_STREAM0        0x6FC*4 /* ptr: stream descriptor type 0    */
#define XCAM_OFF_STREAM1        0x700*4 /* ptr: stream descriptor type 1    */

/* ── Command handler slots (DWORD index into camera object) ───────────────── */
#define XCAM_SLOT_START_STREAM  0x1D
#define XCAM_SLOT_BLOCK_STREAM  0x1E
#define XCAM_SLOT_USB_WORK      0x1F
#define XCAM_SLOT_USB_INIT      0x20
#define XCAM_SLOT_RESET_USB     0x21
#define XCAM_SLOT_DIS_SYSTEM    0x22
#define XCAM_SLOT_EN_SYSTEM     0x23
#define XCAM_SLOT_POWER_ON      0x24
#define XCAM_SLOT_POWER_DOWN    0x25
#define XCAM_SLOT_DIS_SUSPEND   0x26
#define XCAM_SLOT_EN_SUSPEND    0x27
#define XCAM_SLOT_LED_ON        0x28
#define XCAM_SLOT_LED_OFF       0x29
#define XCAM_SLOT_USB_SETTING   0x2F

/* ── IOCTL codes ──────────────────────────────────────────────────────────── */
#define XCAM_IOCTL_OPEN_STREAMS 0x100   /* enumerate streams                 */
#define XCAM_IOCTL_SET_FORMAT   0x101   /* set stream format descriptor      */
#define XCAM_IOCTL_SELECT_STREAM 0x102  /* select stream by index            */
#define XCAM_IOCTL_START        0x107   /* start streaming (ISO setup)       */
#define XCAM_IOCTL_STOP         0x108   /* stop streaming                    */
#define XCAM_IOCTL_QUERY_STATE  0x109   /* query stream state                */
#define XCAM_IOCTL_SET_FMT_MARK 0x10A   /* set format if marker == 0x1B     */
#define XCAM_IOCTL_NOP          0x10B   /* no-op, returns 0                  */
#define XCAM_IOCTL_RESET        0x10D   /* reset                             */
#define XCAM_IOCTL_ENUM_PINS    0x10E   /* enumerate pins (up to 3)          */

/* ── Registry base path ───────────────────────────────────────────────────── */
#define XCAM_REG_BASE \
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Class\\CameraSetting\\"
#define XCAM_REG_USBCAMD \
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\usbcamd\\"

/* ── Registry keys ────────────────────────────────────────────────────────── */
/* Lifecycle control */
#define XCAM_KEY_ENABLE_SYSTEM      L"EnableSystem"
#define XCAM_KEY_DISABLE_SYSTEM     L"DisableSystem"
#define XCAM_KEY_USB_INIT           L"SetUsbInit"
#define XCAM_KEY_USB_WORK           L"SetUsbWork"
#define XCAM_KEY_POWER_ON           L"PowerOnCamera"
#define XCAM_KEY_POWER_DOWN         L"PowerDownCamera"
#define XCAM_KEY_RESET_USB          L"ResetUsb"
#define XCAM_KEY_START_STREAM       L"StartStream"
#define XCAM_KEY_BLOCK_STREAM       L"BlockStream"
#define XCAM_KEY_CAMERA_TIMEOUT     L"CameraTimeout"
#define XCAM_KEY_IDLE_ALT           L"IdleAltSetting"
#define XCAM_KEY_BANDWIDTH          L"BandwidthAllocateRule"
/* Config (read on init) */
#define XCAM_KEY_STATUS_PATH        L"CameraStatusPath"
#define XCAM_KEY_STATUS_HKR         L"Status2HKR"
#define XCAM_KEY_CUSTOM_ID          L"CustomID"
#define XCAM_KEY_STILL_TYPE         L"StillSupportType"
#define XCAM_KEY_STILL_PIN          L"SupportStillPin"
#define XCAM_KEY_SUPPORT_EVENT      L"SupportEvent"
#define XCAM_KEY_DEFAULT_QUALITY    L"DefaultQualityLevel"
#define XCAM_KEY_AUTO_CLOSE         L"EnableAutoClose"
#define XCAM_KEY_USE_GPIO           L"UseGpio0"
#define XCAM_KEY_ADD_SERIAL         L"AddSerialNumber"
#define XCAM_KEY_SHOW_ID            L"ShowCameraId"
#define XCAM_KEY_POWER_CTRL         L"PowerControl"
#define XCAM_KEY_YQUAN              L"DefaultYQuanTable"
#define XCAM_KEY_UVQUAN             L"DefaultUVQuanTable"
#define XCAM_KEY_EEPROM_ADDR        L"E2PROMAddress"
#define XCAM_KEY_LED_ON             L"TurnOnLed"
#define XCAM_KEY_LED_OFF            L"TurnOffLed"
/* Stream format */
#define XCAM_KEY_CURRENT_FORMAT \
    L"Supportcamera\\7649519\\CurrentFormat"
#define XCAM_KEY_FOURCC             L"FourCC"
#define XCAM_KEY_WIDTH              L"Width"
#define XCAM_KEY_HEIGHT             L"Height"
#define XCAM_KEY_FRAMERATE          L"FrameRate"
#define XCAM_KEY_MIN_FRAMERATE      L"MinFrameRate"
#define XCAM_KEY_MAX_FRAMERATE      L"MaxFrameRate"
#define XCAM_KEY_TYP_FRAMERATE      L"TypFrameRate"
#define XCAM_KEY_CUR_FRAMERATE      L"CurrentFrameRate"
#define XCAM_KEY_BITCOUNT           L"BitCount"
#define XCAM_KEY_PROGRESSIVE        L"Progressive"
#define XCAM_KEY_STREAM_TYPE        L"StreamType"
#define XCAM_KEY_QUALITY            L"QualityLevel"
#define XCAM_KEY_SENSOR_W           L"SensorWidth"
#define XCAM_KEY_SENSOR_H           L"SensorHeight"
#define XCAM_KEY_RAW_LEN            L"RawFrameLength"
#define XCAM_KEY_ALT_SETTING        L"AlternateSetting"
#define XCAM_KEY_USB_CBR            L"UsbCBR"
/* Notification events */
#define XCAM_KEY_ATTACHED           L"AttachedCamera"
#define XCAM_KEY_DETACHED           L"Camera DeAttached"
#define XCAM_KEY_OPEN_CAM           L"OpenStreamCameraSetting"
#define XCAM_KEY_CLOSE_CAM          L"CloseStreamCameraSetting"
#define XCAM_KEY_OPEN_USB           L"OpenStreamUsbSetting"
#define XCAM_KEY_CLOSE_USB          L"CloseStreamUsbSetting"
#define XCAM_KEY_USB_SETTING        L"UsbSetting"
#define XCAM_KEY_CAM_SETTING        L"CameraSetting"
#define XCAM_KEY_SUP_CAM            L"CameraSetting\\SupportCamera"
#define XCAM_KEY_CREATE_FILE        L"CreateFileName"
/* usbcamd */
#define XCAM_KEY_DEBUG_TRACE        L"DebugTraceLevel"

/* ── Pixel format FourCC ──────────────────────────────────────────────────── */
#define XCAM_FMT_YUY2   0x32595559  /* preferred -- D3DFMT_YUY2             */
#define XCAM_FMT_IYUV   0x56555949
#define XCAM_FMT_I420   0x30323449
#define XCAM_FMT_YV12   0x32315659
#define XCAM_FMT_AYUV   0x56555941
#define XCAM_FMT_UYVY   0x59565955
#define XCAM_FMT_YVYU   0x55595659
#define XCAM_FMT_P422   0x32323450
#define XCAM_FMT_NV11   0x3131564E
#define XCAM_FMT_NV12   0x3231564E

/* ── OHCI constants ───────────────────────────────────────────────────────── */
#define XCAM_ISO_ENDPOINT   1       /* EP1 IN                                */
#define XCAM_ISO_MAX_PKT    896     /* bytes/packet at highest alt setting   */
#define XCAM_ISO_XFER_SIZE  0x12    /* ISO transfer descriptor alloc size    */
#define XCAM_USB_CHAN_OPEN  0x4030  /* channel open magic (FUN_001be430)     */

/* ── Frame geometry (YUY2 @ 320x240) ─────────────────────────────────────── */
#define XCAM_FRAME_W        320
#define XCAM_FRAME_H        240
#define XCAM_FRAME_BPP      2       /* YUY2: 2 bytes per pixel               */
#define XCAM_FRAME_STRIDE   (XCAM_FRAME_W * XCAM_FRAME_BPP)
#define XCAM_FRAME_SIZE     (XCAM_FRAME_STRIDE * XCAM_FRAME_H)

/* ── Stream descriptor size ───────────────────────────────────────────────── */
#define XCAM_STREAM_DESC_SZ 0x50    /* per stream, from FUN_000ce510         */

/* ── XDEVICE_TYPE_CAMERA ──────────────────────────────────────────────────── */
/* No camera device type exists. The XDEVICE_TYPE_*_TABLE tables are XAPI
   symbols (not kernel), and there is no camera table among them.
   XVoiceCreateMediaObject (XAPI) validates against the three voice tables and
   REJECTS anything else, so it cannot be used for a camera. There is also no
   "open handle + NtDeviceIoControlFile" path -- see the ARCHITECTURE
   CORRECTION at the top. A homebrew driver must talk to the camera at the
   USB/OHCI level in-process.                                                 */

   /* ── XMediaObject callback offsets (if XVoice path is attempted) ──────────── */
#define XCAM_MEDIAOBJ_ERR_CB    0x10    /* error callback fn ptr offset      */
#define XCAM_MEDIAOBJ_READY_CB  0x14    /* frame ready callback fn ptr       */

/* ── Status codes returned by IOCTL handlers ─────────────────────────────── */
#define XCAM_STATUS_OK          0x00000000
#define XCAM_STATUS_BUSY        0x80000022  /* stream already in use         */
#define XCAM_STATUS_NO_MEM      0xC000009A
#define XCAM_STATUS_CONFLICT    0xC000009C  /* already active, not available */
#define XCAM_STATUS_INVALID     0xC000000D
#define XCAM_STATUS_ABORTED     0xC0000120
#define XCAM_STATUS_NOT_IMPL    0xC0000002
#define XCAM_STATUS_NO_DEVICE   0xC0000272
#define XCAM_STATUS_BAD_FORMAT  0xC0000206
#define XCAM_STATUS_MORE_DATA   0x80000005

/* ── API (real declarations; extern "C" gives the definitions C linkage) ──── */
    int   XCam_Init(DWORD dwPort);          /* 0 = ok, else NTSTATUS/-1; reports  */
    /* XCAM_STATUS_NO_DEVICE until the     */
    /* USB/OHCI layer exists               */
    void  XCam_Shutdown(void);
    int   XCam_IsStreaming(void);
    BYTE* XCam_GetFrame(void);              /* latest YUY2 frame, or NULL          */
    int   XCam_DrawToSurface(IDirect3DTexture8* pTex);

    /* Hand-off point for the (future) USB/OHCI capture path: publishes one
       complete YUY2 frame (XCAM_FRAME_SIZE bytes) into the double buffer.        */
    void  XCam_PublishFrame(const BYTE* pYUY2);

#ifdef __cplusplus
}
#endif

#endif /* XB_CAM_H */