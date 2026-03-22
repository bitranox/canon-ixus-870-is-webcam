// CHDK Webcam Module — Zero-Copy H.264
// Switches camera to video mode and starts H.264 recording.
// Intercepts encoded frames from the recording pipeline via a seqlock
// in shared memory, available over PTP to a PC bridge.
//
// movie_rec.c hooks spy_ring_write into sub_FF85D98C_my after each
// encoded frame.  spy_ring_write invalidates the CPU data cache for the
// frame data (JPCORE DMA bypasses cache), stores {ptr, size} via seqlock
// at 0xFF000.  capture_frame_h264() polls the seqlock with msleep(10)
// to yield the CPU to DryOS, then passes the ring buffer pointer
// directly to PTP (zero-copy).
//
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

// Small static buffer for debug frames only
static unsigned char debug_frame_buf[512];

// Frame copy buffer — memcpy from ring buffer before passing to PTP.
// 128KB static BSS (not heap malloc — safe from DryOS heap exhaustion).
// Actual H.264 frames are typically 5-40KB but can exceed 64KB during zoom.
#define FRAME_BUF_SIZE 131072
static unsigned char frame_data_buf[FRAME_BUF_SIZE];


// ============================================================
// Shared memory: seqlock data + msleep polling
// Data delivery uses seqlock protocol (proven at 22fps).
// msleep(10) yields CPU to DryOS scheduler so movie_record_task
// can run spy_ring_write.  DryOS kernel signaling (semaphores,
// message queues) is NOT viable — causes context switches that
// starve the recording pipeline (proven facts #10).
// ============================================================
#define WEBCAM_SPY_ADDR    ((volatile unsigned int *)0x000FF000)
// spy[0]  = magic      (0x52455753 = active, set by webcam.c)
// spy[1]  = slot_a_ptr (frame data pointer, dual-slot seqlock, slot A)
// spy[2]  = slot_a_sz  (frame data size, slot A)
// spy[3]  = slot_a_seq (sequence counter: odd=writing, even=stable, slot A)
// spy[4]  = slot_b_ptr (frame data pointer, dual-slot seqlock, slot B)
// spy[5]  = slot_b_sz  (frame data size, slot B)
// spy[6]  = slot_b_seq (sequence counter: odd=writing, even=stable, slot B)
// spy[12] = dbg_wr     (debug queue write index)
// spy[13] = dbg_rd     (debug queue read index)
// spy[14] = (reserved)

// Dual-slot seqlock tracking (module-level for persistence across capture calls)
static unsigned int last_seq_a = 0;    // last seen slot A sequence
static unsigned int last_seq_b = 0;    // last seen slot B sequence

// ============================================================
// H.264 frame capture from recording spy buffer
// ============================================================

// Capture an H.264 encoded frame via seqlock + msleep polling.
// movie_rec.c's spy_ring_write() stores frame ptr/size via seqlock at
// 0xFF000.  We poll with msleep(10) to yield the CPU so movie_record_task
// can run, then read the seqlock for latest data.
//
// The data is H.264 NAL units in AVCC format (4-byte big-endian
// length prefix, as used in MOV containers).
//
// Returns frame size on success, 0 if no frame available.
static int capture_frame_h264(void)
{
    volatile unsigned int *hdr = WEBCAM_SPY_ADDR;

    if (!recording_active) return 0;
    if (hdr[0] != 0x52455753) return 0;

    // Check debug queue (lock-free SPSC: we read hdr[12]=write_idx, we own hdr[13]=read_idx)
    // Validate "DBG!" magic at slot[4..7] to reject DMA-corrupted indices.
    {
        unsigned int wr = hdr[12];
        unsigned int rd = hdr[13];
        if (wr != rd && wr < 4 && rd < 4) {
            volatile unsigned char *slot =
                (volatile unsigned char *)(0x000FF040 + rd * 512);
            unsigned int dbg_size = *(volatile unsigned short *)slot;
            if (dbg_size >= 12 && dbg_size <= 508
                && slot[4] == 'D' && slot[5] == 'B'
                && slot[6] == 'G' && slot[7] == '!') {
                unsigned int i;
                for (i = 0; i < dbg_size; i++)
                    debug_frame_buf[i] = slot[4 + i];

                hw_jpeg_data = debug_frame_buf;
                hw_jpeg_size = dbg_size;
                frame_width = 640;
                frame_height = 480;
                frame_format = WEBCAM_FMT_DEBUG;
                hdr[13] = (rd + 1) % 4;
                return (int)dbg_size;
            }
            hdr[13] = (rd + 1) % 4;  // Invalid slot, skip
        }
    }

    // Seqlock read with memcpy + post-copy verification.
    // After AVCC peek validates the frame, memcpy to local buffer, then
    // re-check seqlock. If seqlock changed during copy, the ring buffer
    // slot was recycled — discard and retry. This eliminates the long
    // USB DMA window where zero-copy was vulnerable to overwrites.
    // msleep(10) for polling only — yields CPU to DryOS scheduler.
    {
        int polls;

        for (polls = 0; polls < 100; polls++) {
            unsigned int seq_a = hdr[3];
            unsigned int seq_b = hdr[6];
            int new_a = !(seq_a & 1) && seq_a != last_seq_a && seq_a != 0;
            int new_b = !(seq_b & 1) && seq_b != last_seq_b && seq_b != 0;

            if (!new_a && !new_b) {
                msleep(10);
                continue;
            }

            {
                volatile unsigned int *s;
                unsigned char *src;
                unsigned int sz, seq, copy_sz;

                // Pick oldest unseen slot (lower seq = older frame)
                if (new_a && new_b) {
                    if (seq_a <= seq_b) {
                        s = hdr + 1; seq = seq_a;
                    } else {
                        s = hdr + 4; seq = seq_b;
                    }
                } else if (new_a) {
                    s = hdr + 1; seq = seq_a;
                } else {
                    s = hdr + 4; seq = seq_b;
                }

                // Read frame pointer and size, AVCC parse, memcpy, verify.
                {
                    src = (unsigned char *)s[0];
                    sz = s[1];
                    if (!src || sz == 0) { msleep(10); continue; }
                    if (sz > 120000) sz = 120000;

                    // AVCC peek: determine exact frame size from NAL lengths
                    {
                        unsigned int pos = 0, total = 0;
                        int nal_count = 0, have_vcl = 0;
                        while (pos + 5 <= sz && nal_count < 8) {
                            unsigned int nal_len = ((unsigned int)src[pos] << 24) |
                                                  ((unsigned int)src[pos+1] << 16) |
                                                  ((unsigned int)src[pos+2] << 8) |
                                                  (unsigned int)src[pos+3];
                            unsigned int nal_type;
                            if (nal_len < 2 || nal_len > 120000) break;
                            nal_type = src[pos + 4] & 0x1F;
                            if (src[pos + 4] & 0x80) break;
                            total = pos + 4 + nal_len;
                            pos = total;
                            nal_count++;
                            if (nal_type == 1 || nal_type == 5) { have_vcl = 1; break; }
                        }
                        if (!have_vcl || total == 0 || total > sz) {
                            msleep(10);
                            continue;  // AVCC parse failed — skip frame
                        }
                        copy_sz = total;
                    }

                    // Copy video to frame_data_buf, then append audio.
                    // This breaks zero-copy but enables audio piggybacking.
                    // memcpy cost: ~1ms for 40KB frame (negligible vs 29ms PTP RTT).
                    {
                        unsigned int total = copy_sz;
                        unsigned int i;

                        // Copy video
                        if (copy_sz > FRAME_BUF_SIZE - 4096)
                            copy_sz = FRAME_BUF_SIZE - 4096; // leave room for audio
                        for (i = 0; i < copy_sz; i++)
                            frame_data_buf[i] = src[i];

                        // ALWAYS append exactly 2940 bytes of audio.
                        // Before msg 8 fires (~1s), this is silence (zeros).
                        // After msg 8, this is real PCM from the microphone.
                        // Bridge always strips last 2940 bytes as audio.
                        {
                            volatile unsigned int *ashm = (volatile unsigned int *)0x000FE000;
                            unsigned int audio_avail = ashm[3];

                            if (copy_sz + 2940 <= FRAME_BUF_SIZE) {
                                if (audio_avail > 0 && audio_avail <= 2940) {
                                    volatile unsigned char *asrc =
                                        (volatile unsigned char *)(0x000FE000 + 16);
                                    for (i = 0; i < audio_avail; i++)
                                        frame_data_buf[copy_sz + i] = asrc[i];
                                    // Zero-pad if less than 2940
                                    for (; i < 2940; i++)
                                        frame_data_buf[copy_sz + i] = 0;
                                } else {
                                    // No audio yet — append silence
                                    for (i = 0; i < 2940; i++)
                                        frame_data_buf[copy_sz + i] = 0;
                                }
                                copy_sz += 2940;
                            }
                        }
                    }
                }

                if (s == hdr + 1) last_seq_a = seq_a;
                else last_seq_b = seq_b;

                hw_jpeg_data = frame_data_buf;
                hw_jpeg_size = copy_sz;
                frame_width = 640;
                frame_height = 480;
                frame_format = WEBCAM_FMT_H264;
                return (int)copy_sz;
            }
        }

        return 0;
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

    // Switch camera to video mode
    if (switch_to_video_mode() < 0) {
        webcam_active = 0;
        return -2;
    }

    // Wait for video pipeline to stabilize before starting recording.
    msleep(500);

    // Initialize shared spy buffer header (must be done BEFORE starting
    // recording so spy_ring_write sees valid state from the first frame).
    {
        volatile unsigned int *spy = WEBCAM_SPY_ADDR;
        int si;
        for (si = 0; si < 16; si++) spy[si] = 0;
        spy[0] = 0x52455753;                          // Magic (enable LAST)
    }

    // Reset seqlock tracking for new session
    last_seq_a = 0;
    last_seq_b = 0;

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

        // Restore normal recording state before stopping.
        // +0x80=1 lets StopMovieRecord finalize properly (no beeping/errors).
        volatile unsigned int *ring = (volatile unsigned int *)0x8968;
        ring[0x80/4] = 1;

        call_func_ptr(FW_UIFS_StopMovieRecord, 0, 0);

        for (stop_retries = 0; stop_retries < 50; stop_retries++) {
            msleep(100);
            if (get_movie_status() != VIDEO_RECORD_IN_PROGRESS) break;
        }
        recording_active = 0;

        // MOV file deletion handled by bridge via PTP Lua after stop.
    }

    frame_count = 0;
    current_fps = 0;

    // Switch back to playback mode if we changed it
    if (webcam_mode_switched) {
        switch_mode_usb(0);
        webcam_mode_switched = 0;
    }

    enable_shutdown();
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
