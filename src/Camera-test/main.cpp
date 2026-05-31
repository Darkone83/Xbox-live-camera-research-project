/*===========================================================================
    cameratest.cpp -- Xbox Live Camera / EyeToy test harness
                      Team Resurgent / Darkone83

    A minimal RXDK diagnostic harness for the xb_cam camera interface.
    It drives XCam_Init() / XCam_DrawToSurface() / XCam_Shutdown() and reports
    the outcome on screen so the camera path can be exercised on real
    hardware. This is a RESEARCH tool: the underlying camera access is
    reconstructed from static analysis and is NOT runtime validated, so the
    point of this program is to show *where* the sequence succeeds or fails.

    Detailed per-stage logging is emitted by xb_cam.cpp via OutputDebugString
    (visible over the debug/serial channel). This harness surfaces the final
    XCam_Init() return code and the streaming state on screen, and draws the
    live YUY2 preview when the camera streams.

    Controls
    --------
        A         Begin / retry the camera test
        B         Exit
        Y         Stop camera and return to idle (while previewing)
        START     Begin / retry (same as A)
        BACK      Exit (same as B)

    Input is read through the shared ScreenChat module (input.cpp/.h):
    InitInput() once at startup, PumpInput() each frame, and the unified
    BTN_* mask from GetButtons(). Edges are derived locally in ButtonEdges().

    Camera-not-found is a first-class outcome: if XCam_Init() reports that the
    camera could never be detected (CameraSetting key missing / CameraStatusPath
    never populated), the harness shows a friendly "CAMERA NOT FOUND" screen
    instead of a raw NTSTATUS dump, and offers a retry. A later-stage failure
    (open / format / start) still shows the technical result + return code.

    RXDK constraints honoured
    -------------------------
        - No sprintf / sscanf / strlen (small local int->text helpers instead)
        - C89-style declaration ordering (locals declared at top of scope)
        - file-scope statics for all persistent state
        - no per-frame heap allocations (font + camera textures created once,
          vertex data lives on the stack as small fixed arrays)

    Build
    -----
        Compiled as part of the cameratest RXDK project. Links against
        xb_cam.cpp (the camera module under test) and input.cpp (controller
        input). See README.md.
===========================================================================*/

#include <xtl.h>
#include "xb_cam.h"
#include "font.h"
#include "input.h"      /* ScreenChat unified controller module */

/*---------------------------------------------------------------------------
    xb_cam public API (matches the implementation in xb_cam.cpp).
    Declared here so the harness does not depend on edits to xb_cam.h.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif
    int  XCam_Init(DWORD dwPort);
    void XCam_Shutdown(void);
    int  XCam_IsStreaming(void);
    int  XCam_DrawToSurface(IDirect3DTexture8* pTex);
#ifdef __cplusplus
}
#endif

/*---------------------------------------------------------------------------
    Display / layout constants
---------------------------------------------------------------------------*/
#define SCR_W           640
#define SCR_H           480

#define FONT_ATLAS_W    128             /* 16 glyphs across                  */
#define FONT_ATLAS_H    32              /* 4 rows of glyphs                  */
#define FONT_COLS       16

#define TXT_SCALE       2               /* (legacy; unused with cam_font)    */
#define TXT_SIZE        FONT_SIZE_MEDIUM  /* cam_font size for harness text  */
#define TXT_CW          16   /* legacy; unused with cam_font */
#define TXT_CH          16   /* legacy; unused with cam_font */

/* Preview quad: 320x240 source scaled 1.5x -> 480x360, centred-ish         */
#define CAM_VIEW_W      480
#define CAM_VIEW_H      360
#define CAM_VIEW_X      ((SCR_W - CAM_VIEW_W) / 2)
#define CAM_VIEW_Y      80

/* FVF codes */
#define FVF_PC   (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
#define FVF_PCT  (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

/* Colours (A8R8G8B8) */
#define COL_BG       0xFF101418
#define COL_PANEL    0xFF1B2230
#define COL_TITLE    0xFFE000C8        /* Team Resurgent magenta            */
#define COL_BRAND    0xFFB060FF        /* Darkone 83 purple                 */
#define COL_TEXT     0xFFE6E6E6
#define COL_DIM      0xFF8A93A0
#define COL_OK       0xFF40D060
#define COL_FAIL     0xFFE0503C
#define COL_WARN     0xFFE0B040

/* Harness states */
#define ST_IDLE      0
#define ST_RUNNING   1
#define ST_PREVIEW   2
#define ST_RESULT    3
#define ST_NOTFOUND  4

/*---------------------------------------------------------------------------
    Vertex types (pre-transformed screen-space)
---------------------------------------------------------------------------*/
typedef struct {
    float x, y, z, rhw;
    DWORD color;
} VERT_PC;

typedef struct {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
} VERT_PCT;

/*---------------------------------------------------------------------------
    File-scope state
---------------------------------------------------------------------------*/
static IDirect3D8* s_pD3D = NULL;
static IDirect3DDevice8* s_pDev = NULL;
static IDirect3DTexture8* s_pCamTex = NULL;

static WORD                s_prevMask = 0;       /* for button edge detection */

static int                 s_state = ST_IDLE;
static int                 s_initResult = 0;       /* last XCam_Init() rc   */
static DWORD               s_frameCount = 0;       /* preview frames drawn  */
static BOOL                s_camTexOK = FALSE;    /* YUY2 texture created  */

/*---------------------------------------------------------------------------
    Tiny text formatting helpers (no CRT string funcs)
---------------------------------------------------------------------------*/

/* Append a NUL-terminated source onto dst at *pos, advancing *pos.          */
static void StrAppend(char* dst, int* pos, const char* src)
{
    int i = 0;
    while (src[i] != '\0') {
        dst[*pos] = src[i];
        (*pos)++;
        i++;
    }
    dst[*pos] = '\0';
}

/* Append "0x" + 8 hex digits of v.                                          */
static void StrAppendHex32(char* dst, int* pos, DWORD v)
{
    static const char HEX[] = "0123456789ABCDEF";
    int i;
    dst[(*pos)++] = '0';
    dst[(*pos)++] = 'x';
    for (i = 28; i >= 0; i -= 4) {
        dst[(*pos)++] = HEX[(v >> i) & 0xF];
    }
    dst[*pos] = '\0';
}

/* Append unsigned decimal value v.                                          */
static void StrAppendU32(char* dst, int* pos, DWORD v)
{
    char tmp[12];
    int  n = 0;
    int  i;
    if (v == 0) {
        dst[(*pos)++] = '0';
        dst[*pos] = '\0';
        return;
    }
    while (v > 0 && n < 11) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = n - 1; i >= 0; i--) {
        dst[(*pos)++] = tmp[i];
    }
    dst[*pos] = '\0';
}

/*===========================================================================
    Font atlas construction
    Builds a 128x32 linear ARGB texture from the embedded 8x8 font. White
    opaque where a glyph bit is set, fully transparent elsewhere.
===========================================================================*/
static HRESULT BuildFontTexture(void)
{
    /* cam_font owns the atlas now; just initialise it. */
    return Font_Init(s_pDev) ? D3D_OK : E_FAIL;
}

/*===========================================================================
    Primitive drawing
===========================================================================*/
static void DrawSolidRect(int x, int y, int w, int h, DWORD color)
{
    VERT_PC v[4];
    float   fx = (float)x;
    float   fy = (float)y;
    float   fw = (float)(x + w);
    float   fh = (float)(y + h);

    v[0].x = fx; v[0].y = fy; v[0].z = 0.0f; v[0].rhw = 1.0f; v[0].color = color;
    v[1].x = fw; v[1].y = fy; v[1].z = 0.0f; v[1].rhw = 1.0f; v[1].color = color;
    v[2].x = fx; v[2].y = fh; v[2].z = 0.0f; v[2].rhw = 1.0f; v[2].color = color;
    v[3].x = fw; v[3].y = fh; v[3].z = 0.0f; v[3].rhw = 1.0f; v[3].color = color;

    s_pDev->SetTexture(0, NULL);
    s_pDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); /* diffuse */
    s_pDev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    s_pDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    s_pDev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    s_pDev->SetVertexShader(FVF_PC);
    s_pDev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VERT_PC));
}

/* Draw a textured quad from the camera texture (YUY2). */
static void DrawCamQuad(int x, int y, int w, int h)
{
    VERT_PCT v[4];
    float    fx = (float)x;
    float    fy = (float)y;
    float    fw = (float)(x + w);
    float    fh = (float)(y + h);

    v[0].x = fx; v[0].y = fy; v[0].z = 0.0f; v[0].rhw = 1.0f; v[0].color = 0xFFFFFFFF; v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = fw; v[1].y = fy; v[1].z = 0.0f; v[1].rhw = 1.0f; v[1].color = 0xFFFFFFFF; v[1].u = 1.0f; v[1].v = 0.0f;
    v[2].x = fx; v[2].y = fh; v[2].z = 0.0f; v[2].rhw = 1.0f; v[2].color = 0xFFFFFFFF; v[2].u = 0.0f; v[2].v = 1.0f;
    v[3].x = fw; v[3].y = fh; v[3].z = 0.0f; v[3].rhw = 1.0f; v[3].color = 0xFFFFFFFF; v[3].u = 1.0f; v[3].v = 1.0f;

    /* Xbox extension: enable YUV->RGB conversion during sampling.
       Colour comes from the texture; alpha from diffuse (opaque) so the
       preview can't be blended away by YUY2's undefined alpha channel.   */
    s_pDev->SetRenderState(D3DRS_YUVENABLE, TRUE);
    s_pDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); /* texture */
    s_pDev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    s_pDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); /* diffuse */
    s_pDev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    s_pDev->SetTexture(0, s_pCamTex);
    s_pDev->SetVertexShader(FVF_PCT);
    s_pDev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VERT_PCT));
    s_pDev->SetRenderState(D3DRS_YUVENABLE, FALSE);
    s_pDev->SetTexture(0, NULL);
}

/* Draw one glyph quad from the font atlas, tinted by color. */


/* Draw an ASCII string. Lowercase folded to uppercase (font is 0x20..0x5F). */
static void DrawText(int x, int y, const char* sz, DWORD color)
{
    /* forwards to cam_font (ScreenChat renderer). Handles embedded
       newlines the way the old monospace path did. */
    char  line[128];
    int   li = 0;
    int   cy = y;
    int   i = 0;
    char  c;
    int   lh = Font_GlyphHeight(TXT_SIZE) + 4;
    for (;;) {
        c = sz[i];
        if (c == '\n' || c == '\0') {
            line[li] = '\0';
            if (li > 0)
                Font_DrawText(s_pDev, (float)x, (float)cy, line, TXT_SIZE, color, 0);
            li = 0;
            cy += lh;
            if (c == '\0') break;
        }
        else if (li < 127) {
            line[li++] = c;
        }
        i++;
    }
}

/*===========================================================================
    Render state setup for 2D overlay (alpha-blended, point-sampled text)
===========================================================================*/
static void Setup2DStates(void)
{
    s_pDev->SetRenderState(D3DRS_ZENABLE, FALSE);
    s_pDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_pDev->SetRenderState(D3DRS_LIGHTING, FALSE);
    s_pDev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    s_pDev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    s_pDev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_pDev->SetRenderState(D3DRS_YUVENABLE, FALSE);

    /* Colour/alpha ops are set per-primitive (solid=diffuse, text=modulate,
       cam=texture). Here we only set the shared sampler + addressing.      */
    s_pDev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    s_pDev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    s_pDev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    s_pDev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
}

/*===========================================================================
    Screen layouts
===========================================================================*/
static void DrawHeader(void)
{
    DrawSolidRect(0, 0, SCR_W, 40, COL_PANEL);
    DrawText(16, 10, "** XBOX CAMERA TEST **", COL_TITLE);
    DrawText(SCR_W - 16 - 10 * TXT_CW, 10, "DARKONE83", COL_BRAND);
}

static void DrawFooter(const char* hints)
{
    DrawSolidRect(0, SCR_H - 32, SCR_W, 32, COL_PANEL);
    DrawText(16, SCR_H - 24, hints, COL_DIM);
}

static void DrawIdle(void)
{
    DrawHeader();
    DrawText(40, 90, "RESEARCH HARNESS - XB_CAM INTERFACE", COL_TEXT);
    DrawText(40, 130, "TARGET: SONY EYETOY  054C:0155", COL_BRAND);
    DrawText(40, 154, "  (XBOX LIVE CAM 045E:028C ALSO OK)", COL_DIM);
    DrawText(40, 200, "THIS WILL ATTEMPT THE FULL INIT", COL_TEXT);
    DrawText(40, 224, "SEQUENCE AND OPEN A LIVE PREVIEW.", COL_TEXT);
    DrawText(40, 270, "STAGE DETAIL IS LOGGED OVER THE", COL_DIM);
    DrawText(40, 294, "DEBUG/SERIAL CHANNEL VIA", COL_DIM);
    DrawText(40, 318, "OUTPUTDEBUGSTRING.", COL_DIM);
    DrawText(40, 380, "PRESS A TO BEGIN", COL_OK);
    DrawFooter("A=BEGIN   B=EXIT");
}

static void DrawRunning(void)
{
    DrawHeader();
    DrawText(40, 200, "DETECTING CAMERA...", COL_WARN);
    DrawText(40, 240, "PLEASE WAIT", COL_DIM);
    DrawFooter("WORKING...");
}

/* Friendly graceful-failure screen: no camera detected at all. */
static void DrawNotFound(void)
{
    DrawHeader();
    DrawText(40, 110, "CAMERA NOT FOUND", COL_FAIL);
    DrawText(40, 160, "NO EYETOY DETECTED.", COL_TEXT);

    DrawText(40, 210, "CHECK:", COL_DIM);
    DrawText(40, 234, "- EYETOY PLUGGED INTO A CONTROLLER PORT", COL_DIM);
    DrawText(40, 258, "- TRY A DIFFERENT PORT", COL_DIM);
    DrawText(40, 282, "- USBCAMD DRIVER PRESENT ON THIS DASH", COL_DIM);
    DrawText(40, 306, "- EYETOY ENUMERATES AS VIDEO-ONLY", COL_DIM);

    DrawText(40, 400, "PRESS A TO RETRY", COL_OK);
    DrawFooter("A=RETRY   B=EXIT");
}

static void DrawResult(void)
{
    char line[96];
    int  pos = 0;

    DrawHeader();
    DrawText(40, 90, "CAMERA INIT FAILED", COL_FAIL);

    pos = 0;
    StrAppend(line, &pos, "RETURN CODE: ");
    StrAppendHex32(line, &pos, (DWORD)s_initResult);
    DrawText(40, 140, line, COL_TEXT);

    DrawText(40, 190, "CAMERA WAS DETECTED BUT A LATER", COL_DIM);
    DrawText(40, 214, "BRING-UP STAGE FAILED:", COL_DIM);
    DrawText(40, 250, "- DEVICE OPEN (NTOPENFILE)", COL_DIM);
    DrawText(40, 274, "- SET FORMAT (IOCTL 0X101)", COL_DIM);
    DrawText(40, 298, "- START STREAM (IOCTL 0X107)", COL_DIM);

    DrawText(40, 340, "SEE DEBUG OUTPUT FOR THE STAGE", COL_DIM);
    DrawText(40, 364, "THAT FAILED.", COL_DIM);

    DrawText(40, 400, "PRESS A TO RETRY", COL_OK);
    DrawFooter("A=RETRY   B=EXIT");
}

static void DrawPreview(void)
{
    char line[96];
    int  pos;

    DrawHeader();

    /* Preview frame border + image */
    DrawSolidRect(CAM_VIEW_X - 2, CAM_VIEW_Y - 2, CAM_VIEW_W + 4, CAM_VIEW_H + 4, COL_BRAND);
    if (s_camTexOK)
        DrawCamQuad(CAM_VIEW_X, CAM_VIEW_Y, CAM_VIEW_W, CAM_VIEW_H);
    else
        DrawSolidRect(CAM_VIEW_X, CAM_VIEW_Y, CAM_VIEW_W, CAM_VIEW_H, 0xFF000000);

    DrawText(CAM_VIEW_X, 52, "LIVE PREVIEW  320X240 YUY2", COL_TEXT);

    pos = 0;
    StrAppend(line, &pos, "STREAMING: ");
    StrAppend(line, &pos, XCam_IsStreaming() ? "YES" : "NO");
    DrawText(40, CAM_VIEW_Y + CAM_VIEW_H + 16, line, COL_OK);

    pos = 0;
    StrAppend(line, &pos, "FRAMES DRAWN: ");
    StrAppendU32(line, &pos, s_frameCount);
    DrawText(40, CAM_VIEW_Y + CAM_VIEW_H + 40, line, COL_DIM);

    if (!s_camTexOK)
        DrawText(40, CAM_VIEW_Y + CAM_VIEW_H + 64, "WARN: YUY2 TEXTURE NOT CREATED", COL_FAIL);

    DrawFooter("Y=STOP   B=EXIT");
}

/*===========================================================================
    Input -- driven by the ScreenChat input module (input.cpp/.h).
    InitInput() registers all device types (gamepad + MU + voice) and must be
    called once at startup; PumpInput() is called once per frame. GetButtons()
    returns the held BTN_* mask for the active port; we derive press edges.
===========================================================================*/

/* Returns the buttons newly pressed since the previous frame. Call once per
   frame, after PumpInput().                                                 */
static WORD ButtonEdges(void)
{
    WORD mask = GetButtons();
    WORD pressed = (WORD)(mask & ~s_prevMask);
    s_prevMask = mask;
    return pressed;
}

/*===========================================================================
    Camera lifecycle helpers
===========================================================================*/
static void BeginCameraTest(void)
{
    HRESULT hr;

    s_frameCount = 0;
    s_state = ST_RUNNING;

    /* Draw one "initialising" frame so the user sees feedback before the
       (potentially blocking) init call. */
    s_pDev->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG, 1.0f, 0);
    s_pDev->BeginScene();
    Setup2DStates();
    DrawRunning();
    s_pDev->EndScene();
    s_pDev->Present(NULL, NULL, NULL, NULL);

    /* Lazily create the YUY2 preview texture once. */
    if (s_pCamTex == NULL) {
        hr = s_pDev->CreateTexture(XCAM_FRAME_W, XCAM_FRAME_H, 1, 0,
            D3DFMT_YUY2, D3DPOOL_MANAGED, &s_pCamTex);
        s_camTexOK = SUCCEEDED(hr);
        if (!s_camTexOK)
            s_pCamTex = NULL;
    }

    s_initResult = XCam_Init(0);

    if (s_initResult == 0) {
        s_state = ST_PREVIEW;
        OutputDebugStringA("cameratest: XCam_Init OK, entering preview\n");
    }
    else if ((s_initResult == -1 ||
        s_initResult == (int)XCAM_STATUS_NO_DEVICE) &&
        !XCam_IsStreaming()) {
        /* The camera could not be found at all: CameraSetting key missing or
           CameraStatusPath never populated by usbcamd. (We also confirm the
           stream never started, so a rare post-start failure that also returns
           -1 is not misreported as "not found".) Fail gracefully with a
           friendly message rather than a raw NTSTATUS dump.                  */
        s_state = ST_NOTFOUND;
        OutputDebugStringA("cameratest: camera not found\n");
    }
    else {
        /* Camera was seen but a later bring-up stage failed (open / format /
           start). Show the technical result so the stage can be diagnosed.   */
        s_state = ST_RESULT;
        OutputDebugStringA("cameratest: XCam_Init failed (post-detect)\n");
    }
}

static void StopCameraTest(void)
{
    XCam_Shutdown();
    s_state = ST_IDLE;
    OutputDebugStringA("cameratest: camera stopped\n");
}

/*===========================================================================
    D3D setup / teardown
===========================================================================*/
static BOOL InitD3D(void)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT               hr;

    s_pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (s_pD3D == NULL)
        return FALSE;

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = SCR_W;
    pp.BackBufferHeight = SCR_H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = s_pD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &pp, &s_pDev);
    if (FAILED(hr))
        return FALSE;

    hr = BuildFontTexture();
    if (FAILED(hr))
        return FALSE;

    return TRUE;
}

static void ShutdownD3D(void)
{
    if (s_pCamTex) { s_pCamTex->Release();  s_pCamTex = NULL; }
    Font_Shutdown();
    if (s_pDev) { s_pDev->Release();     s_pDev = NULL; }
    if (s_pD3D) { s_pD3D->Release();     s_pD3D = NULL; }
}

/*===========================================================================
    Per-state rendering dispatch
===========================================================================*/
static void RenderFrame(void)
{
    s_pDev->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG, 1.0f, 0);
    s_pDev->BeginScene();
    Setup2DStates();

    switch (s_state) {
    case ST_IDLE:     DrawIdle();     break;
    case ST_PREVIEW:  DrawPreview();  break;
    case ST_RESULT:   DrawResult();   break;
    case ST_NOTFOUND: DrawNotFound(); break;
    default:          DrawRunning();  break;
    }

    s_pDev->EndScene();
    s_pDev->Present(NULL, NULL, NULL, NULL);
}

/*===========================================================================
    Entry point
===========================================================================*/
void __cdecl main(void)
{
    WORD press;
    BOOL running = TRUE;

    InitInput();        /* registers device types + opens already-present pads */

    if (!InitD3D()) {
        /* Nothing we can draw to; bail out. */
        ShutdownD3D();
        return;
    }

    while (running) {
        PumpInput();             /* refresh controller state (hotplug + reads) */
        press = ButtonEdges();   /* buttons newly pressed this frame           */

        switch (s_state) {
        case ST_IDLE:
            if (press & (BTN_A | BTN_START))
                BeginCameraTest();
            else if (press & (BTN_B | BTN_BACK))
                running = FALSE;
            break;

        case ST_PREVIEW:
            if (s_camTexOK)
                XCam_DrawToSurface(s_pCamTex);
            s_frameCount++;
            if (press & BTN_Y)
                StopCameraTest();
            else if (press & (BTN_B | BTN_BACK)) {
                StopCameraTest();
                running = FALSE;
            }
            break;

        case ST_RESULT:
        case ST_NOTFOUND:
            if (press & (BTN_A | BTN_START))
                BeginCameraTest();
            else if (press & (BTN_B | BTN_BACK))
                running = FALSE;
            break;

        default:
            break;
        }

        if (running)
            RenderFrame();
    }

    /* Clean up */
    if (XCam_IsStreaming())
        XCam_Shutdown();
    ShutdownD3D();
}