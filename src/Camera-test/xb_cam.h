/*
 * xb_cam.h -- public interface for the Xbox camera module (xbcam.cpp)
 * Team Resurgent / Darkone83
 *
 * The API the test harness (cameratest.cpp) links against. The harness was
 * written to a synchronous Init/Draw/Shutdown contract; the underlying driver
 * is the async USB class driver (CamAddDevice attaches on hotplug), so these
 * entry points BRIDGE the two: XCam_Init() checks whether a camera attached and
 * starts capture; XCam_DrawToSurface() copies the latest frame.
 *
 * (The previous 610-line IOCTL-era interface is preserved as
 *  xb_cam.h.old_ioctl_era for reference; it described the superseded model.)
 */
#ifndef XB_CAM_H
#define XB_CAM_H

#include <xtl.h>   /* DWORD, IDirect3DTexture8 */

#ifdef __cplusplus
extern "C" {
#endif

	/* Frame geometry (default 320x240). HW: verify the OV519 delivers this. */
#define XCAM_FRAME_W            320
#define XCAM_FRAME_H            240

/* Status codes from XCam_Init(). 0 = success; negatives = failure. */
#define XCAM_STATUS_OK           0
#define XCAM_STATUS_NO_DEVICE    (-2)   /* no camera attached on the port       */
#define XCAM_STATUS_OPEN_FAILED  (-3)   /* attached but capture bring-up failed  */
/* (-1 is also treated as "not found" by the harness, for compatibility.)       */

	/* Optional log sink the harness registers so the (async) driver's
	   breadcrumbs land in the same on-screen / D:\xb_cam.txt channel. Same
	   shape as dbg.h's DbgLogFn, so XCam_SetLog(Dbg_GetSink()) just works. */
	typedef void (*CamLogFn)(const char* tag, const char* msg);
	void XCam_SetLog(CamLogFn fn);

	int  XCam_Init(DWORD dwPort);                       /* 0=ok, else negative      */
	void XCam_Shutdown(void);                           /* stop + release           */
	int  XCam_IsStreaming(void);                        /* nonzero if streaming     */
	int  XCam_DrawToSurface(IDirect3DTexture8* pTex);   /* copy frame -> texture; 0=ok */

	/* --- detection / multi-camera enumeration --- */
	/* A camera can be ATTACHED (seen by the framework, transient) without being a
	   CLAIMABLE persistent device yet. IsConnected reflects attachment; GetCount
	   is the number of claimable camera devices (each with port/VID/PID), which
	   the harness can list and cycle through. */
	int  XCam_IsConnected(void);                        /* 1 if a camera is attached            */
	int  XCam_GetCount(void);                           /* # of claimable camera devices        */
	int  XCam_GetInfo(int index, int* port, int* vid, int* pid); /* 0=ok, -1=bad index          */
	int  XCam_Select(int index);                        /* choose active camera; 0=ok, -1=bad   */

#ifdef __cplusplus
}
#endif

#endif /* XB_CAM_H */