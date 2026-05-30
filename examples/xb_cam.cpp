/*===========================================================================
    xb_cam.cpp -- Xbox USB Camera implementation
                  Team Resurgent / Darkone83

    Based on reverse engineering of Xbox Video Chat XBE (MS-124).
    See RESEARCH.md and xb_cam.h for full documentation.

    STATUS: PROTOTYPE — NOT RUNTIME VALIDATED
    This is a best-effort implementation from static analysis only.
    Expect to need runtime debugging, particularly around:
      - Registry DWORD trigger values (currently using 1 as default)
      - CameraStatusPath string format
      - Format descriptor layout for IOCTL 0x101
      - usbcamd load timing

    RXDK CONSTRAINTS:
      - No sprintf/sscanf/strlen
      - C89 declaration ordering (all vars at top of scope)
      - No per-frame heap allocations
      - Ftoi() for float-to-int
      - file-scope statics for persistent state
===========================================================================*/

#include "xb_cam.h"
#include <xtl.h>

/*---------------------------------------------------------------------------
    Internal state
---------------------------------------------------------------------------*/

static HANDLE       s_hCam          = INVALID_HANDLE_VALUE;
static BOOL         s_bStreaming     = FALSE;
static BOOL         s_bInitialized  = FALSE;

/* Double-buffer for frame data — render thread reads while capture writes  */
static BYTE         s_frameBuf[2][XCAM_FRAME_SIZE];
static int          s_frameWrite    = 0;    /* capture writes to this index */
static int          s_frameRead     = 1;    /* render reads from this index */
static BOOL         s_frameReady    = FALSE;
static CRITICAL_SECTION s_frameLock;

/* Capture thread                                                            */
static HANDLE       s_hCaptureThread = NULL;
static BOOL         s_bCaptureRun    = FALSE;

/*---------------------------------------------------------------------------
    Forward declarations
---------------------------------------------------------------------------*/
static DWORD WINAPI XCam_CaptureThread(LPVOID pParam);
static int  XCam_WriteRegKey(HKEY hKey, LPCWSTR szName, DWORD dwValue);
static int  XCam_ReadRegStr(HKEY hKey, LPCWSTR szName, LPWSTR szOut, DWORD cbMax);
static int  XCam_SendIOCTL(DWORD dwCode, LPVOID pIn, DWORD cbIn,
                            LPVOID pOut, DWORD cbOut);

/*===========================================================================
    XCam_Init
    Call after XGetDeviceChanges reports camera on dwPort.
    Returns 0 on success, negative NTSTATUS on failure.
===========================================================================*/
int XCam_Init(DWORD dwPort)
{
    HKEY    hKey        = NULL;
    HKEY    hSubKey     = NULL;
    LONG    lResult;
    WCHAR   szPath[256];
    DWORD   cbPath;
    DWORD   dwDisp;
    int     iRet        = -1;

    if (s_bInitialized)
        return 0;

    InitializeCriticalSection(&s_frameLock);

    /*-----------------------------------------------------------------------
        Step 1: Open CameraSetting registry key
    -----------------------------------------------------------------------*/
    lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"System\\CurrentControlSet\\Services\\Class\\CameraSetting",
        0, KEY_ALL_ACCESS, &hKey);

    if (lResult != ERROR_SUCCESS) {
        /* Key may not exist yet — camera driver not loaded              */
        OutputDebugStringA("XCam_Init: CameraSetting key not found\n");
        return -1;
    }

    /*-----------------------------------------------------------------------
        Step 2: Write lifecycle registry keys to configure the driver
        NOTE: Trigger DWORD values are assumed 1 — may need adjustment.
              Runtime: if a step fails check event log for driver errors.
    -----------------------------------------------------------------------*/

    /* USB initialization sequence */
    XCam_WriteRegKey(hKey, XCAM_KEY_ENABLE_SYSTEM, 1);
    XCam_WriteRegKey(hKey, XCAM_KEY_USB_INIT,      1);
    XCam_WriteRegKey(hKey, XCAM_KEY_POWER_ON,      1);
    XCam_WriteRegKey(hKey, XCAM_KEY_USB_WORK,      1);

    /* Write desired pixel format */
    RegCreateKeyExW(hKey, L"Supportcamera\\7649519", 0, NULL, 0,
                    KEY_ALL_ACCESS, NULL, &hSubKey, &dwDisp);
    if (hSubKey) {
        DWORD dwFmt = XCAM_FMT_YUY2;
        RegSetValueExW(hSubKey, L"CurrentFormat", 0, REG_DWORD,
                       (BYTE*)&dwFmt, sizeof(DWORD));
        RegCloseKey(hSubKey);
        hSubKey = NULL;
    }

    /* Start stream */
    XCam_WriteRegKey(hKey, XCAM_KEY_START_STREAM, 1);

    /*-----------------------------------------------------------------------
        Step 3: IOCTL 0x100 — open/enumerate streams
        The driver populates CameraStatusPath after camera is accepted.
        Read it to get the device path.
    -----------------------------------------------------------------------*/

    /* Wait briefly for driver to process registry writes                    */
    Sleep(50);

    /* Read CameraStatusPath — driver fills this after USB enumeration      */
    cbPath = sizeof(szPath);
    if (XCam_ReadRegStr(hKey, XCAM_KEY_STATUS_PATH, szPath, cbPath) != 0) {
        OutputDebugStringA("XCam_Init: CameraStatusPath not populated\n");
        OutputDebugStringA("  Camera may not be detected or driver not loaded\n");
        RegCloseKey(hKey);
        return -1;
    }

    OutputDebugStringA("XCam_Init: CameraStatusPath = ");
    /* Note: can't use %S in RXDK — log manually if needed */

    /*-----------------------------------------------------------------------
        Step 4: Open device handle
    -----------------------------------------------------------------------*/
    {
        OBJECT_ATTRIBUTES   oa;
        UNICODE_STRING      uPath;
        IO_STATUS_BLOCK     iosb;
        NTSTATUS            status;

        RtlInitUnicodeString(&uPath, szPath);
        InitializeObjectAttributes(&oa, &uPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

        status = NtOpenFile(&s_hCam,
                            GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                            &oa, &iosb,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            FILE_SYNCHRONOUS_IO_NONALERT);

        if (!NT_SUCCESS(status)) {
            OutputDebugStringA("XCam_Init: NtOpenFile failed\n");
            RegCloseKey(hKey);
            return (int)status;
        }
    }

    /*-----------------------------------------------------------------------
        Step 5: IOCTL 0x100 — enumerate streams
    -----------------------------------------------------------------------*/
    iRet = XCam_SendIOCTL(XCAM_IOCTL_OPEN_STREAMS, NULL, 0, NULL, 0);
    if (iRet < 0) {
        OutputDebugStringA("XCam_Init: IOCTL_OPEN_STREAMS failed\n");
        goto fail;
    }

    /*-----------------------------------------------------------------------
        Step 6: IOCTL 0x101 — set stream format
        Format descriptor layout is inferred from Windows KSDATARANGE_VIDEO.
        Min size: 0x98 bytes. This is the highest-risk section.
        If this IOCTL fails, the format descriptor layout needs adjustment.
    -----------------------------------------------------------------------*/
    {
        /*
            KSDATARANGE_VIDEO approximation (0x98 bytes):
            Offset  Size  Field
            0x00    4     Size (0x98)
            0x04    4     Flags
            0x08    8     MajorFormat GUID (video)
            0x18    8     SubFormat GUID (YUY2)
            0x28    8     Specifier GUID (videoinfo)
            0x38    4     VideoStandard
            0x3C    4     FrameSize
            0x40    4     Width
            0x44    4     Height
            0x48    4     FrameInterval min (100ns)
            0x4C    4     FrameInterval max (100ns)
            0x50    4     FrameInterval step
            0x54    4     DefaultFrameInterval
            0x58    ...   padding to 0x98
        */
        BYTE fmt[0x98];
        DWORD *pFmt = (DWORD*)fmt;

        memset(fmt, 0, sizeof(fmt));
        pFmt[0]  = 0x98;            /* Size                                 */
        pFmt[1]  = 0;               /* Flags                                */
        /* MajorFormat: KSDATAFORMAT_TYPE_VIDEO                             */
        /* {73646976-0000-0010-8000-00AA00389B71}                           */
        pFmt[2]  = 0x73646976;
        pFmt[3]  = 0x00100000;
        pFmt[4]  = 0x00AA8000;
        pFmt[5]  = 0x71389B00;
        /* SubFormat: MEDIASUBTYPE_YUY2                                     */
        /* {32595559-0000-0010-8000-00AA00389B71}                           */
        pFmt[6]  = XCAM_FMT_YUY2;
        pFmt[7]  = 0x00100000;
        pFmt[8]  = 0x00AA8000;
        pFmt[9]  = 0x71389B00;
        /* Specifier: FORMAT_VideoInfo                                      */
        pFmt[0xA] = XCAM_FRAME_W;
        pFmt[0xB] = XCAM_FRAME_H;
        /* Frame interval: 333333 * 100ns = 30fps                          */
        pFmt[0x12] = 333333;
        pFmt[0x13] = 333333;
        pFmt[0x14] = 1;
        pFmt[0x15] = 333333;

        iRet = XCam_SendIOCTL(XCAM_IOCTL_SET_FORMAT, fmt, sizeof(fmt), NULL, 0);
        if (iRet < 0) {
            OutputDebugStringA("XCam_Init: IOCTL_SET_FORMAT failed\n");
            OutputDebugStringA("  Format descriptor layout may need adjustment\n");
            goto fail;
        }
    }

    /*-----------------------------------------------------------------------
        Step 7: IOCTL 0x107 — start streaming
    -----------------------------------------------------------------------*/
    iRet = XCam_SendIOCTL(XCAM_IOCTL_START, NULL, 0, NULL, 0);
    if (iRet < 0) {
        OutputDebugStringA("XCam_Init: IOCTL_START failed\n");
        goto fail;
    }

    s_bStreaming = TRUE;

    /*-----------------------------------------------------------------------
        Step 8: Start capture thread
    -----------------------------------------------------------------------*/
    s_bCaptureRun = TRUE;
    s_hCaptureThread = CreateThread(NULL, 0, XCam_CaptureThread,
                                    NULL, 0, NULL);
    if (!s_hCaptureThread) {
        OutputDebugStringA("XCam_Init: CreateThread failed\n");
        iRet = -1;
        goto fail;
    }

    RegCloseKey(hKey);
    s_bInitialized = TRUE;
    OutputDebugStringA("XCam_Init: success\n");
    return 0;

fail:
    if (s_hCam != INVALID_HANDLE_VALUE) {
        NtClose(s_hCam);
        s_hCam = INVALID_HANDLE_VALUE;
    }
    if (hKey) RegCloseKey(hKey);
    DeleteCriticalSection(&s_frameLock);
    return iRet;
}

/*===========================================================================
    XCam_Shutdown
===========================================================================*/
void XCam_Shutdown(void)
{
    HKEY hKey = NULL;

    if (!s_bInitialized)
        return;

    /* Stop capture thread */
    s_bCaptureRun = FALSE;
    if (s_hCaptureThread) {
        WaitForSingleObject(s_hCaptureThread, 1000);
        CloseHandle(s_hCaptureThread);
        s_hCaptureThread = NULL;
    }

    /* Stop streaming */
    if (s_bStreaming && s_hCam != INVALID_HANDLE_VALUE) {
        XCam_SendIOCTL(XCAM_IOCTL_STOP, NULL, 0, NULL, 0);
        s_bStreaming = FALSE;
    }

    /* Write shutdown registry keys */
    RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"System\\CurrentControlSet\\Services\\Class\\CameraSetting",
        0, KEY_ALL_ACCESS, &hKey);

    if (hKey) {
        XCam_WriteRegKey(hKey, XCAM_KEY_BLOCK_STREAM, 1);
        XCam_WriteRegKey(hKey, XCAM_KEY_POWER_DOWN,   1);
        XCam_WriteRegKey(hKey, XCAM_KEY_RESET_USB,    1);
        XCam_WriteRegKey(hKey, XCAM_KEY_DISABLE_SYSTEM, 1);
        RegCloseKey(hKey);
    }

    /* Close device handle */
    if (s_hCam != INVALID_HANDLE_VALUE) {
        NtClose(s_hCam);
        s_hCam = INVALID_HANDLE_VALUE;
    }

    DeleteCriticalSection(&s_frameLock);
    s_bInitialized = FALSE;
    OutputDebugStringA("XCam_Shutdown: done\n");
}

/*===========================================================================
    XCam_GetFrame
    Returns pointer to most recent YUY2 frame, or NULL if no frame ready.
    Frame is XCAM_FRAME_SIZE bytes (320*240*2).
    Caller must NOT free this pointer — it points into internal double-buffer.
    Valid until next call to XCam_GetFrame.
===========================================================================*/
BYTE* XCam_GetFrame(void)
{
    BYTE *pFrame = NULL;

    if (!s_bInitialized || !s_frameReady)
        return NULL;

    EnterCriticalSection(&s_frameLock);
    pFrame        = s_frameBuf[s_frameRead];
    s_frameRead   = s_frameWrite;   /* swap to latest written buffer */
    s_frameReady  = FALSE;
    LeaveCriticalSection(&s_frameLock);

    return pFrame;
}

/*===========================================================================
    XCam_IsStreaming
===========================================================================*/
int XCam_IsStreaming(void)
{
    return s_bStreaming ? 1 : 0;
}

/*===========================================================================
    XCam_CaptureThread (internal)

    Submits read requests to the camera device and copies frame data into
    the double-buffer.

    NOTE: The actual read mechanism (NtReadFile vs URB submission) is not
    confirmed from static analysis. NtReadFile is attempted first since it
    is the simplest path. If frames don't arrive, try raw URB via
    NtDeviceIoControlFile with a custom read IOCTL.
===========================================================================*/
static DWORD WINAPI XCam_CaptureThread(LPVOID pParam)
{
    BYTE            rawBuf[XCAM_FRAME_SIZE + 0x40]; /* extra for IRP header */
    IO_STATUS_BLOCK iosb;
    NTSTATUS        status;
    int             writeIdx;

    OutputDebugStringA("XCam_CaptureThread: started\n");

    while (s_bCaptureRun) {
        if (s_hCam == INVALID_HANDLE_VALUE)
            break;

        /*
            Attempt NtReadFile to pull a frame.
            If the driver uses a different read model (push mode / event
            driven), this will fail with STATUS_NOT_IMPLEMENTED or
            STATUS_INVALID_DEVICE_REQUEST — in that case switch to
            URB/IOCTL approach.
        */
        memset(&iosb, 0, sizeof(iosb));
        status = NtReadFile(s_hCam, NULL, NULL, NULL,
                            &iosb, rawBuf, XCAM_FRAME_SIZE, NULL, NULL);

        if (!NT_SUCCESS(status)) {
            /* Log and try to recover */
            Sleep(33);
            continue;
        }

        if (iosb.Information < XCAM_FRAME_SIZE) {
            /* Partial frame — skip */
            continue;
        }

        /* Copy into write buffer under lock, then swap */
        EnterCriticalSection(&s_frameLock);
        writeIdx     = 1 - s_frameRead;    /* write to whichever isn't being read */
        memcpy(s_frameBuf[writeIdx], rawBuf, XCAM_FRAME_SIZE);
        s_frameWrite = writeIdx;
        s_frameReady = TRUE;
        LeaveCriticalSection(&s_frameLock);
    }

    OutputDebugStringA("XCam_CaptureThread: stopped\n");
    return 0;
}

/*===========================================================================
    XCam_SendIOCTL (internal)
===========================================================================*/
static int XCam_SendIOCTL(DWORD dwCode, LPVOID pIn, DWORD cbIn,
                           LPVOID pOut, DWORD cbOut)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS        status;

    if (s_hCam == INVALID_HANDLE_VALUE)
        return -1;

    memset(&iosb, 0, sizeof(iosb));
    status = NtDeviceIoControlFile(s_hCam, NULL, NULL, NULL,
                                   &iosb, dwCode,
                                   pIn,  cbIn,
                                   pOut, cbOut);
    return NT_SUCCESS(status) ? 0 : (int)status;
}

/*===========================================================================
    XCam_WriteRegKey (internal)
===========================================================================*/
static int XCam_WriteRegKey(HKEY hKey, LPCWSTR szName, DWORD dwValue)
{
    LONG lResult = RegSetValueExW(hKey, szName, 0, REG_DWORD,
                                  (BYTE*)&dwValue, sizeof(DWORD));
    return (lResult == ERROR_SUCCESS) ? 0 : -1;
}

/*===========================================================================
    XCam_ReadRegStr (internal)
===========================================================================*/
static int XCam_ReadRegStr(HKEY hKey, LPCWSTR szName, LPWSTR szOut, DWORD cbMax)
{
    DWORD dwType = 0;
    DWORD cbData = cbMax;
    LONG  lResult;

    lResult = RegQueryValueExW(hKey, szName, NULL, &dwType,
                               (BYTE*)szOut, &cbData);
    if (lResult != ERROR_SUCCESS)
        return -1;
    if (dwType != REG_SZ && dwType != REG_EXPAND_SZ)
        return -1;
    return 0;
}

/*===========================================================================
    XCam_DrawToSurface
    Convenience: upload latest frame to a D3DFMT_YUY2 texture.
    pTex must be created as:
        pDevice->CreateTexture(XCAM_FRAME_W, XCAM_FRAME_H, 1, 0,
                               D3DFMT_YUY2, D3DPOOL_DEFAULT, &pTex)
    Returns 0 if a new frame was uploaded, -1 if no new frame was ready.
===========================================================================*/
int XCam_DrawToSurface(IDirect3DTexture8 *pTex)
{
    BYTE           *pFrame;
    D3DLOCKED_RECT  lr;
    HRESULT         hr;
    BYTE           *pSrc;
    BYTE           *pDst;
    int             y;

    pFrame = XCam_GetFrame();
    if (!pFrame)
        return -1;

    hr = pTex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr))
        return -1;

    pSrc = pFrame;
    pDst = (BYTE*)lr.pBits;

    if (lr.Pitch == XCAM_FRAME_STRIDE) {
        /* Pitch matches — single copy */
        memcpy(pDst, pSrc, XCAM_FRAME_SIZE);
    } else {
        /* Copy row by row if pitch differs */
        for (y = 0; y < XCAM_FRAME_H; y++) {
            memcpy(pDst, pSrc, XCAM_FRAME_STRIDE);
            pSrc += XCAM_FRAME_STRIDE;
            pDst += lr.Pitch;
        }
    }

    pTex->UnlockRect(0);
    return 0;
}

/*===========================================================================
    USAGE EXAMPLE
    Paste into your Chat_Init / Chat_Update / Chat_Draw:

    --- Init ---
    IDirect3DTexture8 *g_pCamTex = NULL;

    // After controller input confirms camera port:
    if (XCam_Init(0) == 0) {
        pDevice->CreateTexture(XCAM_FRAME_W, XCAM_FRAME_H, 1, 0,
                               D3DFMT_YUY2, D3DPOOL_DEFAULT, &g_pCamTex);
    }

    --- Update (per frame) ---
    if (g_pCamTex && XCam_IsStreaming()) {
        XCam_DrawToSurface(g_pCamTex);
    }

    --- Draw ---
    if (g_pCamTex) {
        // Set up a fullscreen quad or PiP quad
        pDevice->SetTexture(0, g_pCamTex);
        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2,
                                 quad_verts, sizeof(VERTEX));
        pDevice->SetTexture(0, NULL);
    }

    --- Shutdown ---
    XCam_Shutdown();
    if (g_pCamTex) { g_pCamTex->Release(); g_pCamTex = NULL; }

===========================================================================*/
