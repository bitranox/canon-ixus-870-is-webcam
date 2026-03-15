// CHDK Webcam Module
// Intercepts H.264 frames from the recording pipeline via seqlock
// and streams them over PTP to a PC-side bridge.

#ifndef WEBCAM_H
#define WEBCAM_H

#include "flt.h"

// Update version if changes are made to the module interface
#define WEBCAM_VERSION          {1,0}

// Frame format identifiers
#define WEBCAM_FMT_JPEG  0      // JPEG compressed frame
#define WEBCAM_FMT_UYVY  1      // Raw UYVY (YUV422) frame, 2 bytes per pixel
#define WEBCAM_FMT_H264  2      // H.264 NAL unit(s) for one video frame
#define WEBCAM_FMT_DEBUG 3      // Debug diagnostic frame (tagged key-value entries)
#define WEBCAM_FMT_H264_MULTI 4 // Multiple H.264 frames batched: [u16 count][u32 sz][data]...

// Frame info returned to callers
typedef struct {
    unsigned char  *data;       // Pointer to frame data (H.264 NAL units or debug)
    unsigned int    size;       // Size of frame data in bytes
    unsigned int    width;      // Frame width
    unsigned int    height;     // Frame height
    unsigned int    frame_num;  // Monotonic frame counter
    unsigned int    format;     // WEBCAM_FMT_H264, WEBCAM_FMT_DEBUG, etc.
} webcam_frame_t;

// Webcam status
typedef struct {
    int             active;         // 1 if webcam streaming is active
    int             frames_sent;    // Total frames sent
    int             fps;            // Approximate current FPS
    int             jpeg_quality;   // Current JPEG quality (1-100)
    int             frame_size;     // Last frame size in bytes
    unsigned int    width;          // Current frame width
    unsigned int    height;         // Current frame height
} webcam_status_t;

// Module interface
typedef struct {
    base_interface_t    base;

    // Start webcam streaming mode.
    // jpeg_quality: 1-100 (lower = smaller frames, higher = better quality)
    // Returns 0 on success, non-zero on error.
    int (*start)(int jpeg_quality);

    // Stop webcam streaming mode.
    // Returns 0 on success.
    int (*stop)(void);

    // Get the latest frame.
    // frame: output pointer to frame info (valid until next get_frame call)
    // Returns 0 on success, non-zero if no frame available.
    int (*get_frame)(webcam_frame_t *frame);

    // Get current webcam status.
    void (*get_status)(webcam_status_t *status);
} libwebcam_sym;

extern libwebcam_sym* libwebcam;

#endif
