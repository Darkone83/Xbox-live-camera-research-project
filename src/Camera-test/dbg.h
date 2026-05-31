#ifndef DBG_H
#define DBG_H
/*---------------------------------------------------------------------------
    dbg.h -- camera test harness debug log: ON-SCREEN ring buffer + a
             crash-survivable disk mirror at D:\xb_cam.txt.

    Built on SceneChat's sc_log pattern (CreateFileA/WriteFile, C89, no CRT),
    restructured for BULLETPROOF per-line logging: every line OPENS, appends,
    and CLOSES the file. A closed file is committed, so after a fault the last
    line in D:\xb_cam.txt is guaranteed to be the last process that completed.

    Also keeps the last N lines in memory and draws them on screen via the
    cam_font renderer -- the only debug channel available on a modchipped
    retail box with no Super I/O (kernel DbgPrint goes nowhere readable).

    The driver (xbcam.cpp) logs through a registered function pointer
    (Dbg_GetSink) so it stays decoupled from the harness's D3D and remains
    standalone-buildable.

    Call order:
        Dbg_Init()                       once at startup (after D3D up is fine)
        Dbg_Log/_Int/_Hex(...)           anywhere
        Dbg_Draw(pDev, x, y, size)       each frame, to show the log on screen
        Dbg_Shutdown()                   on exit
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    void Dbg_Init(void);
    void Dbg_Log(const char* tag, const char* msg);
    void Dbg_Log_Int(const char* tag, const char* label, int val);
    void Dbg_Log_Hex(const char* tag, const unsigned char* buf, int len);
    void Dbg_Shutdown(void);

    /* Draw the last on-screen lines starting at (x,y), one per line, in `size`
       (FONT_SIZE_*). Returns the y of the next free line. */
    float Dbg_Draw(IDirect3DDevice8* pDevice, float x, float y, int size);

    /* Draw the log anchored to the lower-right: right edge at right_x, block bottom
       at bottom_y, right-aligned, stacked upward (newest nearest the bottom). */
    void Dbg_DrawCorner(IDirect3DDevice8* pDevice, float right_x, float bottom_y, int size);

    /* How many lines are currently held (for callers that want to lay out). */
    int  Dbg_LineCount(void);

    /* Driver sink: a plain (tag,msg) function pointer the driver can call without
       depending on the harness. Dbg_GetSink() returns &Dbg_Log; the driver stores
       it and calls through it (null-checked) so it builds standalone too. */
    typedef void (*DbgLogFn)(const char* tag, const char* msg);
    DbgLogFn Dbg_GetSink(void);

#ifdef __cplusplus
}
#endif

#endif /* DBG_H */