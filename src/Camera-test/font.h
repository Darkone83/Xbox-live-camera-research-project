#ifndef CAM_FONT_H
#define CAM_FONT_H
/*---------------------------------------------------------------------------
    cam_font.h -- Font renderer for the camera test harness.

    This is ScreenChat's proven font module (swizzled D3DFMT_A8R8G8B8 atlas via
    XGSwizzleRect -- D3DFMT_LIN_* locks the NV2A when sampled) with the UI
    virtual-resolution scaling REMOVED. The harness is fixed 640x480, so all
    coordinates are raw screen-space pixels (UI_Sx/UI_Sy collapse to identity).

    Requires: font_atlas.h (the pre-baked atlas + GlyphMetrics tables),
              xgraphics.lib (XGSwizzleRect).

    Call order:
        Font_Init(pDevice)    -- once, after the D3D8 device is created
        Font_Shutdown()       -- on device release
        Font_DrawText(...)    -- during scene render
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_SIZE_SMALL    0
#define FONT_SIZE_MEDIUM   1
#define FONT_SIZE_LARGE    2

    /* ARGB packing */
#define FONT_RGBA(r,g,b,a) (((DWORD)(a)<<24)|((DWORD)(r)<<16)|((DWORD)(g)<<8)|(DWORD)(b))
#define FONT_WHITE          FONT_RGBA(255,255,255,255)
#define FONT_GREEN          FONT_RGBA( 57,255, 20,255)
#define FONT_PURPLE         FONT_RGBA(139, 92,246,255)
#define FONT_GRAY           FONT_RGBA(170,170,170,255)
#define FONT_DARKGRAY       FONT_RGBA( 80, 80, 80,255)
#define FONT_BLACK          FONT_RGBA(  0,  0,  0,255)
#define FONT_RED            FONT_RGBA(220, 50, 50,255)

    int  Font_Init(IDirect3DDevice8* pDevice);
    void Font_Shutdown(void);

    int  Font_MeasureText(const char* str, int size);
    int  Font_GlyphHeight(int size);

    void Font_DrawText(IDirect3DDevice8* pDevice,
        float x, float y,
        const char* str,
        int size,
        DWORD colour,
        int max_w);

    void Font_DrawTextCentered(IDirect3DDevice8* pDevice,
        float cx, float y,
        float width,
        const char* str,
        int size,
        DWORD colour);

    void Font_DrawTextRight(IDirect3DDevice8* pDevice,
        float x, float y,
        const char* str,
        int size,
        DWORD colour);

#ifdef __cplusplus
}
#endif

#endif /* CAM_FONT_H */