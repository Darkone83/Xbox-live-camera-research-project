/*---------------------------------------------------------------------------
    dbg.cpp -- on-screen + crash-survivable disk logger for the camera harness.

    Derived from SceneChat's sc_log.cpp (CreateFileA/WriteFile, C89, no CRT),
    restructured so EVERY line opens, appends, and closes D:\xb_cam.txt. That
    per-line close is the crash-survivability guarantee: a closed file is fully
    committed, so the last line on disk is the last process that completed
    before any fault.

    Each line is ALSO mirrored into an in-memory ring buffer that Dbg_Draw()
    renders on screen via cam_font -- the only readable debug channel on a
    modchipped retail box with no Super I/O.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dbg.h"
#include "font.h"

#define LOG_PATH      "D:\\xb_cam.txt"
#define DBG_LINES     18        /* on-screen ring capacity                  */
#define DBG_LINE_LEN  80        /* chars per line (incl. NUL)               */

/*    On-screen ring buffer                                                    */
static char  s_ring[DBG_LINES][DBG_LINE_LEN];
static int   s_count = 0;       /* total lines ever logged                  */
static DWORD s_t0 = 0;

/*    ---- tiny formatting helpers (no CRT), build a line into a caller buf ---- */

static int AppStr(char* d, int p, const char* s) {
    int i = 0;
    while (s[i] && p < DBG_LINE_LEN - 1) { d[p++] = s[i++]; }
    d[p] = '\0';
    return p;
}
static int AppHex32(char* d, int p, DWORD v) {
    static const char h[] = "0123456789ABCDEF";
    int i;
    if (p < DBG_LINE_LEN - 2) { d[p++] = '0'; d[p++] = 'x'; }
    for (i = 28; i >= 0 && p < DBG_LINE_LEN - 1; i -= 4)
        d[p++] = h[(v >> i) & 0xF];
    d[p] = '\0';
    return p;
}
static int AppHex8(char* d, int p, unsigned char b) {
    static const char h[] = "0123456789ABCDEF";
    if (p < DBG_LINE_LEN - 2) { d[p++] = h[b >> 4]; d[p++] = h[b & 0xF]; }
    d[p] = '\0';
    return p;
}
static int AppInt(char* d, int p, int v) {
    char tmp[12]; int n = 0, j; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { if (p < DBG_LINE_LEN - 1) d[p++] = '0'; d[p] = '\0'; return p; }
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg && p < DBG_LINE_LEN - 1) d[p++] = '-';
    for (j = n - 1; j >= 0 && p < DBG_LINE_LEN - 1; j--) d[p++] = tmp[j];
    d[p] = '\0';
    return p;
}
static int AppTag(char* d, int p, const char* tag) {
    int i = 0;
    while (tag[i] && i < 10 && p < DBG_LINE_LEN - 1) { d[p++] = tag[i++]; }
    while (i < 10 && p < DBG_LINE_LEN - 1) { d[p++] = ' '; i++; }
    d[p] = '\0';
    return p;
}

/*    ---- the bulletproof disk write: open, append, write, CLOSE ---- */

static void DiskAppendLine(const char* line) {
    HANDLE hf;
    DWORD  w, len = 0;
    hf = CreateFileA(LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hf, 0, NULL, FILE_END);   /* append */
    while (line[len]) len++;
    WriteFile(hf, line, len, &w, NULL);
    WriteFile(hf, "\r\n", 2, &w, NULL);
    CloseHandle(hf);                         /* CLOSE = committed to disk */
}

/*    ---- push a finished line to both sinks ---- */

static void Emit(const char* line) {
    int slot = s_count % DBG_LINES;
    int i = 0;
    while (line[i] && i < DBG_LINE_LEN - 1) { s_ring[slot][i] = line[i]; i++; }
    s_ring[slot][i] = '\0';
    s_count++;
    DiskAppendLine(line);                    /* crash-survivable mirror */
}

/* Build the "[T+xxxxxxxx] [TAG       ] " prefix into buf, return new pos. */
static int Prefix(char* buf, const char* tag) {
    int p = 0;
    DWORD t = GetTickCount() - s_t0;
    p = AppStr(buf, p, "[T+");
    p = AppHex32(buf, p, t);
    p = AppStr(buf, p, "] [");
    p = AppTag(buf, p, tag);
    p = AppStr(buf, p, "] ");
    return p;
}

/*    ---- public API ---- */

void Dbg_Init(void) {
    char buf[DBG_LINE_LEN];
    int  p;
    s_count = 0;
    s_t0 = GetTickCount();
    /* a session banner, also proves the file is writable */
    p = AppStr(buf, 0, "===== xb_cam log  T0=");
    p = AppHex32(buf, p, s_t0);
    p = AppStr(buf, p, " =====");
    Emit(buf);
}

void Dbg_Log(const char* tag, const char* msg) {
    char buf[DBG_LINE_LEN];
    int  p = Prefix(buf, tag);
    p = AppStr(buf, p, msg);
    Emit(buf);
}

void Dbg_Log_Int(const char* tag, const char* label, int val) {
    char buf[DBG_LINE_LEN];
    int  p = Prefix(buf, tag);
    p = AppStr(buf, p, label);
    p = AppStr(buf, p, "=");
    p = AppInt(buf, p, val);
    Emit(buf);
}

void Dbg_Log_Hex(const char* tag, const unsigned char* data, int len) {
    char buf[DBG_LINE_LEN];
    int  p = Prefix(buf, tag);
    int  i;
    if (len > 16) len = 16;          /* one screen line worth */
    for (i = 0; i < len; i++) {
        p = AppHex8(buf, p, data[i]);
        if (p < DBG_LINE_LEN - 1) buf[p++] = ' ';
        buf[p] = '\0';
    }
    Emit(buf);
}

void Dbg_Shutdown(void) {
    Dbg_Log("DBG", "shutdown");
}

int Dbg_LineCount(void) {
    return (s_count < DBG_LINES) ? s_count : DBG_LINES;
}

float Dbg_Draw(IDirect3DDevice8* pDevice, float x, float y, int size) {
    int   shown = Dbg_LineCount();
    int   lh = Font_GlyphHeight(size) + 2;
    int   i;
    int   start = (s_count <= DBG_LINES) ? 0 : (s_count - DBG_LINES);
    for (i = 0; i < shown; i++) {
        int slot = (start + i) % DBG_LINES;
        Font_DrawText(pDevice, x, y, s_ring[slot], size, FONT_GREEN, 0);
        y += lh;
    }
    return y;
}

/* Lower-right anchored log: right-edge at `right_x`, block bottom at `bottom_y`.
   Lines are right-aligned and stacked upward from the bottom so the newest is
   nearest the bottom edge. */
void Dbg_DrawCorner(IDirect3DDevice8* pDevice, float right_x, float bottom_y, int size) {
    int   shown = Dbg_LineCount();
    int   lh = Font_GlyphHeight(size) + 2;
    int   i;
    int   start = (s_count <= DBG_LINES) ? 0 : (s_count - DBG_LINES);
    /* top of the block so the last line ends at bottom_y */
    float y = bottom_y - (float)(shown * lh);
    for (i = 0; i < shown; i++) {
        int slot = (start + i) % DBG_LINES;
        Font_DrawTextRight(pDevice, right_x, y, s_ring[slot], size, FONT_GREEN);
        y += lh;
    }
}

DbgLogFn Dbg_GetSink(void) {
    return Dbg_Log;
}