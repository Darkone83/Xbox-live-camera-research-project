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

	int  XCam_Init(DWORD dwPort);                       /* 0=ok, else negative      */
	void XCam_Shutdown(void);                           /* stop + release           */
	int  XCam_IsStreaming(void);                        /* nonzero if streaming     */
	int  XCam_DrawToSurface(IDirect3DTexture8* pTex);   /* copy frame -> texture; 0=ok */

#ifdef __cplusplus
}
#endif

#endif /* XB_CAM_H */