// CHDK Webcam Module
// Switches camera to video mode and starts H.264 recording.
// Intercepts encoded frames from the recording pipeline via a seqlock
// in shared memory + DryOS message queue notification, available over
// PTP to a PC bridge.
//
// movie_rec.c hooks spy_ring_write into sub_FF85D98C_my after each
// encoded frame.  spy_ring_write invalidates the CPU data cache for the
// frame data (JPCORE DMA bypasses cache), stores {ptr, size} via seqlock
// at 0xFF000, and sends a non-blocking notification via TryPostMessageQueue.
// capture_frame_h264() blocks on ReceiveMessageQueue until notified, then
// reads the seqlock for the latest frame data.
//
// On the first capture call, the IDR keyframe is read directly from the
// ring buffer's +0xC0 pointer (which persists throughout recording) and
// converted from hybrid Annex B format to pure AVCC for the bridge.

#include "camera_info.h"
#include "shooting.h"
#include "modes.h"
#include "clock.h"
#include "stdlib.h"
#include "module_def.h"
#include "callfunc.h"
#include "webcam.h"
#include "shutdown.h"

// Movie recording start/stop via CtrlSrv event system
// UIFS_StartMovieRecord posts event 0x9A1 to the control server queue.
// Takes 1 arg: callback pointer (NULL = use default).
// Returns: 0=success, 0xFFFFFFF9=state check 1 fail, 0xFFFFFFFD=state check 2 fail.
#define FW_UIFS_StartMovieRecord            ((void *)0xFF883D50)
#define FW_UIFS_StopMovieRecord             ((void *)0xFF883D84)

// Module state
static int webcam_active = 0;
static int webcam_jpeg_quality = 50;
static int webcam_mode_switched = 0;    // 1 if we switched to video mode

// Frame counter and timing
static unsigned int frame_count = 0;
static unsigned int last_frame_tick = 0;
static int current_fps = 0;

// Frame format and dimensions
static unsigned int frame_format = 0;
static unsigned int frame_width = 0;
static unsigned int frame_height = 0;

// Frame data pointer and size (set by capture_frame_h264)
static unsigned char *hw_jpeg_data = 0;
static unsigned int hw_jpeg_size = 0;

// H.264 recording state
static int recording_active = 0;
static int webcam_stop(void);              // forward declaration for use in webcam_start

// Spy buffer: frame data area (H.264 data copied from ring buffer)
static unsigned char *frame_data_buf = NULL;
#define SPY_BUF_SIZE 65536                 // 64 KB — enough for any H.264 frame

// IDR injection: the first H.264 frame (IDR) is lost due to a race condition
// (spy_ring_write fires before PTP polling starts). The IDR data persists at
// the ring buffer +0xC0 pointer throughout recording, in Annex B format.
// We read it directly and convert to AVCC on the first capture call.
static int idr_injected = 0;

// Note: SPS/PPS for H.264 decoding is hardcoded in the PC bridge's FFmpeg
// decoder (extracted from the camera's MOV avcC atom). The spy buffer never
// contains SPS/PPS — Canon stores it only in the MOV container metadata.

// ============================================================
// Shared memory: seqlock data + DryOS queue wakeup
// Data delivery uses seqlock protocol (proven at 22fps).
// DryOS message queue provides instant wakeup notification
// (replaces msleep(10) polling for lower latency).
// ============================================================
#define WEBCAM_SPY_ADDR    ((volatile unsigned int *)0x000FF000)
// spy[0] = magic    (0x52455753 = active, set by webcam.c)
// spy[1] = ptr      (frame data pointer, set by movie_rec.c, seqlock)
// spy[2] = size     (frame data size, set by movie_rec.c, seqlock)
// spy[3] = seq      (sequence counter: odd=writing, even=stable)
// spy[4] = queue    (DryOS message queue handle, set by webcam.c)
// spy[8] = dbg_wr   (debug queue write index)
// spy[9] = dbg_rd   (debug queue read index)

// DryOS message queue firmware functions (not stubbed in CHDK)
#define FW_CreateMessageQueue      ((void *)0xFF826F84)
#define FW_DeleteMessageQueue      ((void *)0xFF827008)
#define FW_ReceiveMessageQueue     ((void *)0xFF827098)
#define FW_TryReceiveMessageQueue  ((void *)0xFF827160)

static int frame_queue = 0;  // DryOS message queue handle

// ============================================================
// H.264 frame capture from recording spy buffer
// ============================================================

// Capture an H.264 encoded frame via seqlock + DryOS queue wakeup.
// movie_rec.c's spy_ring_write() stores frame ptr/size via seqlock at
// 0xFF000 and sends a non-blocking notification via TryPostMessageQueue.
// We call ReceiveMessageQueue to block until notified (up to 100ms),
// then drain extra notifications and read the seqlock for latest data.
//
// The data is H.264 NAL units in AVCC format (4-byte big-endian
// length prefix, as used in MOV containers).
//
// Returns frame size on success, 0 if no frame available.
static int capture_frame_h264(void)
{
    volatile unsigned int *hdr = WEBCAM_SPY_ADDR;
    unsigned int size;

    if (!recording_active || !frame_data_buf) return 0;
    if (hdr[0] != 0x52455753) return 0;

    // Check debug queue (lock-free SPSC: we read hdr[8]=write_idx, we own hdr[9]=read_idx)
    // Validate "DBG!" magic at slot[4..7] to reject DMA-corrupted indices.
    {
        unsigned int wr = hdr[8];
        unsigned int rd = hdr[9];
        if (wr != rd && wr < 4 && rd < 4) {
            volatile unsigned char *slot =
                (volatile unsigned char *)(0x000FF040 + rd * 512);
            unsigned int dbg_size = *(volatile unsigned short *)slot;
            if (dbg_size >= 12 && dbg_size <= 508
                && slot[4] == 'D' && slot[5] == 'B'
                && slot[6] == 'G' && slot[7] == '!') {
                unsigned int i;
                for (i = 0; i < dbg_size; i++)
                    frame_data_buf[i] = slot[4 + i];

                hw_jpeg_data = frame_data_buf;
                hw_jpeg_size = dbg_size;
                frame_width = 640;
                frame_height = 480;
                frame_format = WEBCAM_FMT_DEBUG;
                hdr[9] = (rd + 1) % 4;
                return (int)dbg_size;
            }
            hdr[9] = (rd + 1) % 4;  // Invalid slot, skip
        }
    }

    // IDR injection: on first call, read IDR from ring buffer +0xC0 pointer.
    // The data is in hybrid format (Annex B start codes for SPS/PPS, then AVCC
    // length prefix for IDR NAL). We extract the IDR NAL in AVCC format.
    // Ring buffer struct at 0x8968: +0x28 = frame counter, +0xC0 = first-frame ptr,
    // +0xDC = IDR size.
    if (!idr_injected && recording_active && frame_data_buf) {
        unsigned int fcnt = *(volatile unsigned int *)0x8990;  // +0x28: frame counter
        if (fcnt >= 1) {
            unsigned int first_ptr = *(volatile unsigned int *)0x8A28;  // +0xC0
            unsigned int idr_size  = *(volatile unsigned int *)0x8A44;  // +0xDC
            unsigned int scan_limit = (idr_size > 0 && idr_size < 60000)
                                      ? idr_size + 200 : 55000;
            if (scan_limit > SPY_BUF_SIZE) scan_limit = SPY_BUF_SIZE;
            if (first_ptr != 0 && first_ptr > 0x1000 && first_ptr < 0x80000000) {
                unsigned char *src = (unsigned char *)first_ptr;
                unsigned int dst_pos = 0;
                unsigned int i = 0;
                int got_vcl = 0;
                unsigned int nal_count = 0;

                // Find first Annex B start code (3-byte 00 00 01 or 4-byte 00 00 00 01)
                while (i + 3 < scan_limit) {
                    if (src[i]==0 && src[i+1]==0 && src[i+2]==1) {
                        i += 3;
                        break;
                    }
                    if (src[i]==0 && src[i+1]==0 && src[i+2]==0 && (i+3) < scan_limit && src[i+3]==1) {
                        i += 4;
                        break;
                    }
                    i++;
                }

                // Process each NAL unit: convert start codes to AVCC length prefixes
                while (i < scan_limit && !got_vcl) {
                    unsigned int nal_start = i;
                    unsigned int nal_end = scan_limit;
                    unsigned int j, nal_len, nal_type;

                    // Find next start code
                    for (j = i + 1; j + 2 < scan_limit; j++) {
                        if (src[j]==0 && src[j+1]==0 && src[j+2]==1) {
                            nal_end = (j > 0 && src[j-1]==0) ? j - 1 : j;
                            break;
                        }
                    }

                    nal_len = nal_end - nal_start;
                    if (nal_len < 1 || nal_len > 60000) break;
                    if (dst_pos + 4 + nal_len > SPY_BUF_SIZE) break;

                    // Write AVCC 4-byte big-endian length prefix
                    frame_data_buf[dst_pos]     = (nal_len >> 24) & 0xFF;
                    frame_data_buf[dst_pos + 1] = (nal_len >> 16) & 0xFF;
                    frame_data_buf[dst_pos + 2] = (nal_len >> 8) & 0xFF;
                    frame_data_buf[dst_pos + 3] = nal_len & 0xFF;
                    memcpy(frame_data_buf + dst_pos + 4, src + nal_start, nal_len);
                    dst_pos += 4 + nal_len;

                    nal_type = src[nal_start] & 0x1F;
                    nal_count++;
                    if (nal_type == 5) got_vcl = 1;  // IDR found — stop

                    // Advance past the start code we found
                    if (nal_end < scan_limit) {
                        j = nal_end;
                        if (j + 3 < scan_limit && src[j]==0 && src[j+1]==0 && src[j+2]==0 && src[j+3]==1)
                            i = j + 4;
                        else if (j + 2 < scan_limit && src[j]==0 && src[j+1]==0 && src[j+2]==1)
                            i = j + 3;
                        else
                            break;  // unexpected format
                    } else {
                        break;  // No more NAL units
                    }
                }

                // Hybrid format fallback: SPS/PPS use Annex B start codes, but IDR
                // uses AVCC 4-byte BE length prefix. If scanner didn't find IDR via
                // start codes, check for AVCC IDR at byte 24 (after SC+SPS+SC+PPS).
                if (!got_vcl && nal_count >= 1 && dst_pos > 0) {
                    unsigned int pps_end = 24;
                    if (pps_end + 4 < scan_limit) {
                        unsigned int avcc_len = ((unsigned int)src[pps_end] << 24)
                                              | ((unsigned int)src[pps_end+1] << 16)
                                              | ((unsigned int)src[pps_end+2] << 8)
                                              | src[pps_end+3];
                        unsigned int idr_nal_pos = pps_end + 4;
                        if (avcc_len > 0 && avcc_len < 60000
                            && idr_nal_pos < scan_limit
                            && (src[idr_nal_pos] & 0x1F) == 5) {
                            // IDR NAL only (no SPS/PPS — bridge has them from avcC init)
                            dst_pos = 0;
                            memcpy(frame_data_buf, src + pps_end, 4 + avcc_len);
                            dst_pos = 4 + avcc_len;
                            got_vcl = 1;
                        }
                    }
                }

                if (got_vcl && dst_pos > 0) {
                    idr_injected = 1;
                    hw_jpeg_data = frame_data_buf;
                    hw_jpeg_size = dst_pos;
                    frame_width = 640;
                    frame_height = 480;
                    frame_format = WEBCAM_FMT_H264;
                    return (int)dst_pos;
                }
            }
        }
    }

    // Read frame via seqlock protocol.
    // msleep(10) yields CPU to DryOS so spy_ring_write (in movie_record_task)
    // gets scheduled. On single-core ARM926, cache is coherent — spy_ring_write
    // writes hdr[1..3] to the CPU cache, we read from the same cache. No cache
    // eviction needed; the msleep is purely for task scheduling.
    // After detecting new hdr[3], hdr[1]/hdr[2] are already in cache (written
    // by spy_ring_write before the second hdr[3]++ increment), so no second
    // msleep is needed before reading.
    {
        static unsigned int last_seq = 0;
        int polls;

        for (polls = 0; polls < 100; polls++) {
            unsigned int seq_before, seq_after;
            unsigned char *src_ptr;

            seq_before = hdr[3];

            // Skip if write in progress (odd) or no new data (unchanged)
            if ((seq_before & 1) || seq_before == last_seq) {
                msleep(10);
                continue;
            }

            // Frame ready. Minimal yield so recording pipeline stays scheduled.
            msleep(1);
            src_ptr = (unsigned char *)hdr[1];
            size = hdr[2];
            if (!src_ptr || size == 0) continue;
            if (size > SPY_BUF_SIZE) size = SPY_BUF_SIZE;

            memcpy(frame_data_buf, src_ptr, size);

            seq_after = hdr[3];
            if (seq_before == seq_after) {
                last_seq = seq_before;
                break;  // Consistent copy
            }
            // Seq changed during copy — frame overwritten, retry
        }
        if (polls >= 100) return 0;
    }

    // Parse AVCC NAL units to determine actual H.264 frame size.
    // The ring buffer chunk may be larger than the encoded frame.
    // AVCC format: [4-byte big-endian length][NAL data] per NAL unit.
    // Canon Baseline profile: one slice NAL per frame (type 1 or 5),
    // optionally preceded by SEI (type 6).
    {
        unsigned char *p = frame_data_buf;
        unsigned int pos = 0;
        unsigned int total = 0;
        int nal_count = 0;
        int have_vcl = 0;

        while (pos + 5 <= size && nal_count < 8) {
            unsigned int nal_len = ((unsigned int)p[pos] << 24) |
                                  ((unsigned int)p[pos+1] << 16) |
                                  ((unsigned int)p[pos+2] << 8) |
                                  (unsigned int)p[pos+3];
            unsigned int nal_type;

            if (nal_len < 2 || nal_len > 120000) break;

            nal_type = p[pos + 4] & 0x1F;
            if (p[pos + 4] & 0x80) break;  // forbidden_zero_bit set

            total = pos + 4 + nal_len;
            pos = total;
            nal_count++;

            if (nal_type == 1 || nal_type == 5) {
                have_vcl = 1;
                break;
            }
        }

        if (!have_vcl || total == 0 || total > size) return 0;

        hw_jpeg_data = frame_data_buf;
        hw_jpeg_size = total;
        frame_width = 640;
        frame_height = 480;
        frame_format = WEBCAM_FMT_H264;

        return (int)total;
    }
}

// ============================================================
// Frame capture with timing
// ============================================================

static int capture_frame(void)
{
    int size;

    size = capture_frame_h264();

    if (size > 0) {
        frame_count++;
        {
            unsigned int now = get_tick_count();
            if (last_frame_tick > 0 && now > last_frame_tick) {
                int delta = now - last_frame_tick;
                if (delta > 0) {
                    int instant_fps = 1000 / delta;
                    current_fps = (current_fps * 7 + instant_fps * 3) / 10;
                }
            }
            last_frame_tick = now;
        }
    }

    return size;
}

// ============================================================
// Module interface implementation
// ============================================================

// Switch camera to video mode for webcam streaming.
// Returns 0 on success, -1 on failure.
static int switch_to_video_mode(void)
{
    int retries;

    // Step 1: Switch from playback to record mode.
    if (camera_info.state.mode_play) {
        switch_mode_usb(1);

        for (retries = 0; retries < 20; retries++) {
            msleep(100);
            if (!camera_info.state.mode_play) break;
        }
        if (camera_info.state.mode_play) {
            return -1;
        }
    }

    // Step 2: Switch shooting mode to video (MODE_VIDEO_STD).
    if (!camera_info.state.mode_video) {
        shooting_set_mode_chdk(MODE_VIDEO_STD);

        for (retries = 0; retries < 30; retries++) {
            msleep(100);
            if (camera_info.state.mode_video) break;
        }
    }

    webcam_mode_switched = 1;
    return 0;
}

static int webcam_start(int jpeg_quality)
{
    // If still active from a previous bridge session (bridge crashed without
    // calling stop), force a clean shutdown first.
    if (webcam_active) {
        webcam_stop();
    }

    if (jpeg_quality < 1) jpeg_quality = 1;
    if (jpeg_quality > 100) jpeg_quality = 100;
    webcam_jpeg_quality = jpeg_quality;

    // Mark active BEFORE the slow mode switch to prevent
    // module_tick_unloader() from unloading us during msleep() calls.
    webcam_active = 1;

    // Prevent camera auto-power-off during webcam streaming.
    disable_shutdown();

    // Reset frame state
    hw_jpeg_data = 0;
    hw_jpeg_size = 0;
    frame_format = 0;
    recording_active = 0;
    idr_injected = 0;

    // Switch camera to video mode
    if (switch_to_video_mode() < 0) {
        webcam_active = 0;
        return -2;
    }

    // Wait for video pipeline to stabilize before starting recording.
    msleep(500);

    // Allocate frame data buffer for local parsing.
    if (!frame_data_buf) {
        frame_data_buf = malloc(SPY_BUF_SIZE);
    }

    // Initialize shared spy buffer header (must be done BEFORE starting
    // recording so spy_ring_write sees valid state from the first frame).
    {
        volatile unsigned int *spy = WEBCAM_SPY_ADDR;
        int si;
        for (si = 0; si < 16; si++) spy[si] = 0;
        spy[0] = 0x52455753;                          // Magic (enable LAST)
    }

    // Start video recording via the firmware's CtrlSrv event system.
    // UIFS_StartMovieRecord posts event 0x9A1 — CtrlSrv processes it
    // asynchronously. We must WAIT for movie_status to reach 4 (recording).
    {
        int rec_retries;
        unsigned int rec_args[1] = { 0 };  // NULL callback = use default
        unsigned int rec_ret;

        rec_ret = call_func_ptr(FW_UIFS_StartMovieRecord, rec_args, 1);

        if (rec_ret == 0) {
            // Wait up to 5 seconds for CtrlSrv to start recording
            for (rec_retries = 0; rec_retries < 50; rec_retries++) {
                msleep(100);
                if (get_movie_status() == VIDEO_RECORD_IN_PROGRESS) {
                    recording_active = 1;
                    break;
                }
            }
        }
    }

    frame_count = 0;
    last_frame_tick = 0;
    current_fps = 0;
    frame_width = 0;
    frame_height = 0;

    return 0;
}

static int webcam_stop(void)
{
    webcam_active = 0;

    // Re-enable auto-power-off (disabled in webcam_start)
    enable_shutdown();

    // Disable spy buffer first — movie_rec.c's spy_ring_write returns
    // early when magic is cleared, preventing writes during shutdown.
    {
        volatile unsigned int *spy = WEBCAM_SPY_ADDR;
        spy[0] = 0;
    }
    msleep(50);  // Let any in-progress spy_ring_write complete

    // Stop recording if we started it
    if (recording_active) {
        int stop_retries;
        call_func_ptr(FW_UIFS_StopMovieRecord, 0, 0);

        for (stop_retries = 0; stop_retries < 50; stop_retries++) {
            msleep(100);
            if (get_movie_status() != VIDEO_RECORD_IN_PROGRESS) break;
        }
        recording_active = 0;
    }

    idr_injected = 0;

    // Clean up spy buffer resources
    if (frame_data_buf) {
        free(frame_data_buf);
        frame_data_buf = NULL;
    }

    frame_count = 0;
    current_fps = 0;

    // Switch back to playback mode if we changed it
    if (webcam_mode_switched) {
        switch_mode_usb(0);
        webcam_mode_switched = 0;
    }

    return 0;
}

static int webcam_get_frame(webcam_frame_t *frame)
{
    int size;

    if (!webcam_active || !frame) {
        return -1;
    }

    size = capture_frame();
    if (size <= 0) {
        return -1;
    }

    frame->data = hw_jpeg_data;
    frame->size = hw_jpeg_size;
    frame->width = frame_width;
    frame->height = frame_height;
    frame->frame_num = frame_count;
    frame->format = frame_format;

    return 0;
}

static void webcam_get_status(webcam_status_t *status)
{
    if (!status) return;

    status->active = webcam_active;
    status->frames_sent = frame_count;
    status->fps = current_fps;
    status->jpeg_quality = webcam_jpeg_quality;
    status->width = frame_width;
    status->height = frame_height;
    status->frame_size = hw_jpeg_size;
    status->hw_fail_call = 0;
    status->hw_fail_soi = 0;
    status->hw_fail_eoi = 0;
    status->hw_available = 0;
    status->diag_data = 0;
    status->diag_len = 0;
}

// ============================================================
// Module lifecycle
// ============================================================

static int _module_unloader(void)
{
    webcam_stop();
    return 0;
}

static int _module_can_unload(void)
{
    return (webcam_active == 0);
}

static int _module_exit_alt(void)
{
    webcam_stop();
    return 0;
}

// ============================================================
// Module definition
// ============================================================

libwebcam_sym _libwebcam = {
    {
        0,                      // loader
        _module_unloader,       // unloader
        _module_can_unload,     // can_unload
        _module_exit_alt,       // exit_alt
        0                       // run
    },
    webcam_start,
    webcam_stop,
    webcam_get_frame,
    webcam_get_status
};

ModuleInfo _module_info = {
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    WEBCAM_VERSION,
    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,
    ANY_PLATFORM_ALLOWED,
    (int32_t)"Webcam",
    MTYPE_EXTENSION,
    &_libwebcam.base,
    ANY_VERSION,                // conf_ver
    ANY_VERSION,                // cam_screen_ver
    ANY_VERSION,                // cam_sensor_ver
    ANY_VERSION,                // cam_info_ver
    0                           // symbol
};
