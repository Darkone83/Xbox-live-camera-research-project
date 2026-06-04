/*
 * xb_cam.h -- public interface for the OV519-family Xbox camera module
 * Team Resurgent / Darkone83
 *
 * Synchronous harness-facing API for the current manual USB bring-up path.
 * XCam_Init() attempts to locate/claim a supported OV519/OV530-compatible
 * camera, initialize the bridge/sensor, and start capture. Because the current
 * implementation can still return OK after a late streaming failure, callers
 * should check XCam_IsStreaming() before treating the preview as live.
 *
 * XCam_DrawToSurface() copies the latest decoded MJPEG frame into the supplied
 * D3DFMT_A8R8G8B8 texture.
 */
#ifndef XB_CAM_H
#define XB_CAM_H

#include <xtl.h>   /* DWORD, IDirect3DTexture8 */

#ifdef __cplusplus
extern "C" {
#endif

	/* Current tested frame geometry. */
#define XCAM_FRAME_W            320
#define XCAM_FRAME_H            240

/* Status codes from XCam_Init(). 0 = success; negatives = failure. */
#define XCAM_STATUS_OK           0
#define XCAM_STATUS_NO_DEVICE    (-2)   /* no camera attached on the port       */
#define XCAM_STATUS_OPEN_FAILED  (-3)   /* attached but capture bring-up failed  */
/* (-1 is also treated as "not found" by the harness, for compatibility.)       */

	/* Optional log sink the harness registers so camera breadcrumbs land in the
	   same on-screen / D:\xb_cam.txt channel. Same shape as dbg.h's DbgLogFn,
	   so XCam_SetLog(Dbg_GetSink()) just works. */
	typedef void (*CamLogFn)(const char* tag, const char* msg);
	void XCam_SetLog(CamLogFn fn);

	int  XCam_Init(DWORD dwPort);                       /* 0=ok, else negative      */
	void XCam_Shutdown(void);                           /* stop + release           */
	int  XCam_IsStreaming(void);                        /* nonzero if streaming     */
	int  XCam_DrawToSurface(IDirect3DTexture8* pTex);   /* copy frame -> texture; 0=ok */

	/* --- reserved detection / multi-camera enumeration API --- */
	/* Declarations kept for planned/future harness use. The current xb_cam.cpp
	   implementation does not provide these helpers yet; current code uses the
	   single active manual camera path through XCam_Init()/XCam_IsStreaming(). */
	int  XCam_IsConnected(void);                        /* 1 if a camera is attached            */
	int  XCam_GetCount(void);                           /* # of claimable camera devices        */
	int  XCam_GetInfo(int index, int* port, int* vid, int* pid); /* 0=ok, -1=bad index          */
	int  XCam_Select(int index);                        /* choose active camera; 0=ok, -1=bad   */

#ifdef __cplusplus
}
#endif

#endif /* XB_CAM_H */
