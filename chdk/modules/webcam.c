// CHDK Webcam Module
// Switches camera to video mode and captures frames as MJPEG.
// The compressed frame is made available via the module interface for
// streaming over PTP to a PC-side virtual webcam bridge.
//
// Two encoding paths are supported:
//
// 1. HARDWARE MJPEG (preferred): Uses the Digic IV JPCORE hardware encoder
//    via firmware functions StartMjpegMaking / GetContinuousMovieJpegVRAMData.
//    The hardware encoder produces JPEG frames from the video pipeline's
//    internal buffer at near-zero CPU cost.  Expected: 5-15+ FPS.
//
// 2. SOFTWARE MJPEG (fallback): Reads the viewport buffer (720x240 YUV411)
//    and compresses it with a software JPEG encoder (tje.c).
//    This is slower (~1.8 FPS) but works without firmware function access.
//
// On start, the module switches the camera from playback/PTP mode into
// record mode and then sets the shooting mode to video (MODE_VIDEO_STD).
// This activates the camera's video pipeline at 640x480.

#include "camera_info.h"
#include "viewport.h"
#include "shooting.h"
#include "modes.h"
#include "levent.h"
#include "clock.h"
#include "stdlib.h"
#include "module_def.h"
#include "callfunc.h"
#include "webcam.h"
#include "tje.h"
#include "shutdown.h"
#include "keyboard.h"
#include "semaphore.h"

// Maximum JPEG output buffer size (software fallback path).
#define JPEG_BUF_SIZE       (256 * 1024)

// Raw UYVY frame size: 640 x 480 x 2 bytes/pixel = 614,400 bytes.
// This is the size of the uncompressed video frame from the ISP pipeline.
#define UYVY_BUF_SIZE       (640 * 480 * 2)

// ============================================================
// Hardware MJPEG VRAM buffer constants (from Ghidra RE of FUN_ffaa2224)
//
// The Digic IV JPCORE hardware encoder writes JPEG frames via DMA to a
// fixed uncached RAM buffer.  These values are compiled into the firmware
// ROM at 0xFFAA2314 and 0xFFAA2318 and never change at runtime.
//
// 0x40EA23D0 is the uncached mirror of physical RAM at 0x00EA23D0.
// Using uncached access avoids CPU data cache coherency issues with DMA.
// ============================================================
#define HW_MJPEG_VRAM_ADDR      ((volatile unsigned char *)0x40EA23D0)
#define HW_MJPEG_VRAM_BUFSIZE   0x000D2F00  /* 864,000 bytes (~844 KB) */

// Double-buffer JPEG output to avoid tearing
#define NUM_JPEG_BUFS       2

// ============================================================
// Hardware MJPEG firmware function addresses (IXUS 870 IS, fw 1.01a)
//
// These addresses come from funcs_by_address.csv and are called via
// call_func_ptr() so the module doesn't need direct stub linkage.
// ============================================================
#define FW_StartMjpegMaking                 ((void *)0xFF9E8DD8)
#define FW_StopMjpegMaking                  ((void *)0xFF9E8DF8)
#define FW_GetContinuousMovieJpegVRAMData   ((void *)0xFFAA234C)
#define FW_GetMovieJpegVRAMHPixelsSize      ((void *)0xFF8C4178)
#define FW_GetMovieJpegVRAMVPixelsSize      ((void *)0xFF8C4184)
#define FW_StopContinuousVRAMData           ((void *)0xFF8C425C)

// Movie recording start/stop via CtrlSrv event system (Option D)
// UIFS_StartMovieRecord_FW posts event 0x9A1 to the control server queue.
// The actual pipeline initialization happens asynchronously in CtrlSrv task.
// Takes 1 arg: callback pointer (NULL = use default).
// Returns: 0=success, 0xFFFFFFF9=state check 1 fail, 0xFFFFFFFD=state check 2 fail.
#define FW_UIFS_StartMovieRecord            ((void *)0xFF883D50)
#define FW_UIFS_StopMovieRecord             ((void *)0xFF883D84)

// JPCORE power/clock management (discovered via Ghidra RE of FUN_ff8eeb6c)
// FUN_ff8eeb6c is a ref-counted power-on that calls three sub-functions
// on first invocation:
//   FUN_ff815288(0) — JPCORE clock/power gate enable
//   FUN_ff8152e8()  — additional clock domain enable
//   FUN_ff8ef6b4()  — JPCORE subsystem init (DMA, interrupts, buffers)
// BUT: FUN_ff8eeb6c checks *(0x8028)==0 first and returns early if so.
// If the JPCORE module wasn't loaded at boot, this flag is 0 and the
// function is a complete no-op — explaining why JPCORE never encodes.
#define FW_JPCORE_PowerInit                 ((void *)0xFF8EEB6C)
#define FW_JPCORE_PowerDeinit               ((void *)0xFF8EEBC8)
#define FW_JPCORE_ClockEnable               ((void *)0xFF815288)
#define FW_JPCORE_ClockEnable2              ((void *)0xFF8152E8)
#define FW_JPCORE_SubsystemInit             ((void *)0xFF8EF6B4)

// JPCORE quality lookup table writer (Option B: quality fix)
// FUN_ff849408(quality) sets piVar1[5] = quality + 1, which makes
// JPCORE_DMA_Start proceed (it checks piVar1[5] != -1).
// quality=0xe is what the firmware uses during actual video recording.
#define FW_JPCORE_SetQuality                ((void *)0xFF849408)

// DryOS semaphore function for keeping the recording pipeline alive.
// The AVI write path in sub_FF85D98C_my calls TakeSemaphore with a
// 1-second timeout after each sub_FF8EDBE0 (AVI frame write) call.
// If the write never completes (e.g., file system blocked in PTP mode),
// the timeout triggers and sets the recording state to "stopping".
// By pre-signaling with GiveSemaphore, TakeSemaphore returns immediately
// and the recording pipeline continues producing H.264 frames.
#define FW_GiveSemaphore                    ((void *)0xFF827584)

// JPCORE power struct at RAM 0x8028 (from ROM literal DAT_ff8eec70)
// Offset 0x00: init flag (must be !=0 for FUN_ff8eeb6c to proceed)
// Offset 0x04: semaphore handle
// Offset 0x10: secondary flag
// Offset 0x14: ref count
#define JPCORE_POWER_STRUCT ((volatile unsigned int *)0x00008028)

// Module state
static int webcam_active = 0;
static int webcam_jpeg_quality = 50;
static int webcam_mode_switched = 0;    // 1 if we switched to video mode

// Hardware encoder state
static int hw_mjpeg_active = 0;         // 1 if hardware MJPEG encoder is running
static int hw_mjpeg_available = 0;      // 1 if hardware path was successfully started

// JPEG double buffer (used by software path; hardware path provides its own buffer)
static unsigned char *jpeg_buf[NUM_JPEG_BUFS] = { 0, 0 };
static int jpeg_buf_size[NUM_JPEG_BUFS] = { 0, 0 };
static int jpeg_buf_current = 0;    // Index of the buffer being written
static int jpeg_buf_ready = -1;     // Index of the buffer ready for reading (-1 = none)

// Frame counter and timing
static unsigned int frame_count = 0;
static unsigned int last_frame_tick = 0;
static int current_fps = 0;

// Raw UYVY streaming state
static unsigned char *uyvy_buf = 0;        // Copy buffer for raw UYVY frames
static unsigned int frame_format = 0;       // WEBCAM_FMT_JPEG, WEBCAM_FMT_UYVY, or WEBCAM_FMT_H264
static unsigned int last_cb_count_raw = 0;  // rec_cb_count at last raw capture (stale detection)

// H.264 recording state (Option D: intercept encoded frames during recording)
static int recording_active = 0;           // 1 if we started video recording for H.264 capture
static int webcam_stop(void);              // forward declaration for use in webcam_start
static unsigned int last_spy_cnt = 0;      // spy[3] frame count at last H.264 capture (stale detection)
static unsigned int avi_sem_handle = 0;    // AVI write semaphore handle (from movtask[+0x14])

// Spy buffer: frame data area (H.264 data copied by movie_rec.c's spy_ring_write)
static unsigned char *frame_data_buf = NULL;
static int frame_sem = 0;                  // DryOS binary semaphore for frame signaling
#define SPY_BUF_SIZE 65536                 // 64 KB — enough for any H.264 frame

// Note: SPS/PPS for H.264 decoding is hardcoded in the PC bridge's FFmpeg
// decoder (extracted from the camera's MOV avcC atom). The spy buffer never
// contains SPS/PPS — Canon stores it only in the MOV container metadata.

// JPCORE hardware JPEG state (Option B: quality fix)
static unsigned int last_cb_count_hwjpeg = 0;  // rec_cb_count at last JPCORE check

// Captured frame dimensions
static unsigned int frame_width = 0;
static unsigned int frame_height = 0;

// Stale frame detection: checksum of a few bytes at the start of the viewport
static unsigned int last_vp_checksum = 0;

// Software path failure counters (for diagnostics)
static unsigned int sw_fail_null = 0;     // vid_get_viewport_active_buffer() returned NULL
static unsigned int sw_fail_stale = 0;    // viewport checksum unchanged (unused, kept for stats)
static unsigned int sw_fail_encode = 0;   // tje_encode_yuv411 returned 0
static unsigned int sw_total_calls = 0;   // total calls to capture_and_compress_frame_sw
static unsigned int sw_last_vp_addr = 0;  // last viewport buffer address (for diagnostics)

// Hardware frame: pointer + size from VRAM buffer after GetContinuousMovieJpegVRAMData
static unsigned char *hw_jpeg_data = 0;
static unsigned int hw_jpeg_size = 0;

// Frame index passed to GetContinuousMovieJpegVRAMData (wraps at 31 per firmware check)
static unsigned int hw_frame_index = 0;

// Custom recording callback: captures arguments from the pipeline frame
// callback chain. When state[+0x114] is set to this function, the pipeline
// calls it after JPCORE encodes each frame. The arguments may contain
// the JPEG data pointer and size.
static volatile unsigned int rec_cb_arg0 = 0;
static volatile unsigned int rec_cb_arg1 = 0;
static volatile unsigned int rec_cb_arg2 = 0;
static volatile unsigned int rec_cb_arg3 = 0;
static volatile unsigned int rec_cb_count = 0;

// Samples captured AT CALLBACK TIME (ISR context):
// These tell us what the buffers contain at the exact moment the pipeline fires.
static volatile unsigned int rec_cb_dma_addr = 0;    // HW DMA output addr (phys)
static volatile unsigned int rec_cb_dma_sample = 0;   // First 4 bytes at DMA output (uncached)
static volatile unsigned int rec_cb_arg2_sample = 0;  // First 4 bytes at arg2
static volatile unsigned int rec_cb_db0_sample = 0;   // First 4 bytes at dbl-buf[0]
static volatile unsigned int rec_cb_db1_sample = 0;   // First 4 bytes at dbl-buf[1]
static volatile unsigned int rec_cb_buf2_sample = 0;  // First 4 bytes at buf[2] (JPCORE out)
static volatile unsigned int rec_cb_soi_found = 0;    // Which buffer had SOI at callback time

static void __attribute__((used)) rec_callback_spy(
    unsigned int a0, unsigned int a1, unsigned int a2, unsigned int a3)
{
    rec_cb_arg0 = a0;
    rec_cb_arg1 = a1;
    rec_cb_arg2 = a2;
    rec_cb_arg3 = a3;
    rec_cb_count++;

    // Read HW DMA register from main thread only (Block 8 diagnostics).
    // Reading JPCORE I/O regs inside the pipeline callback may cause bus
    // conflicts on ARM926EJ-S and was correlated with camera hangs.
    // rec_cb_dma_addr is set in the main-thread diagnostics instead.

    // Read CACHED addresses only (safe in ISR context).
    // Avoid reading uncached 0x40xxxxxx DMA buffers here — that caused
    // bus stalls/hangs on ARM926EJ-S when JPCORE is actively DMA'ing.
    {
        volatile unsigned int *s = (volatile unsigned int *)0x000070D8;
        volatile unsigned int *jpbuf = (volatile unsigned int *)0x00002580;
        // Store the addresses themselves (from cached RAM — safe)
        rec_cb_db0_sample = s[0x144/4];    // db0 address
        rec_cb_db1_sample = s[0x148/4];    // db1 address
        rec_cb_buf2_sample = jpbuf[2];     // buf[2] address
    }

    // Read arg2 ONLY if it's in cached (non-DMA) range
    if (a2 >= 0x00010000 && a2 < 0x04000000) {
        volatile unsigned char *p = (volatile unsigned char *)a2;
        rec_cb_arg2_sample = ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
                             ((unsigned int)p[2] << 8) | (unsigned int)p[3];
    } else {
        rec_cb_arg2_sample = a2;  // Just store the address itself
    }

    // SOI check deferred to main thread (Block 8 diagnostics) — not safe in ISR
    rec_cb_dma_sample = 0;
    rec_cb_soi_found = 0;
}

// Hardware encoder failure diagnostics
static unsigned int hw_fail_call = 0;      // GetContinuousMovieJpegVRAMData returned non-zero
static unsigned int hw_fail_soi = 0;       // No SOI marker at VRAM buffer
static unsigned int hw_fail_eoi = 0;       // No EOI marker found
static unsigned int hw_fail_total = 0;     // Total failed attempts
#define HW_FAIL_FALLBACK_THRESHOLD 3       // After this many failures, fall back to software

// Hardware diagnostic buffer (128 bytes, populated on SOI failures)
//
// Based on Ghidra RE: all functions use the same state struct at RAM 0x70D8
// (literal pool values at 0xFF8C2E24 and 0xFF8C43F4 both = 0x000070D8).
//
// Layout (v2 — state struct dump):
//   0- 3: *(state + 0x48)  MJPEG active flag (set by StartMjpegMaking)
//   4- 7: *(state + 0x4C)  Set alongside +0x48
//   8-11: *(state + 0x54)  DMA status (cleared by trigger, set by pipeline)
//  12-15: *(state + 0x58)  Frame index stored by DMA trigger
//  16-19: *(state + 0x5C)  DMA request state (3=requested, 4=stopped)
//  20-23: *(state + 0x64)  VRAM buffer addr stored by trigger (should be 0x40EA23D0)
//  24-27: *(state + 0xA0)  Callback stored by DMA trigger
//  28-31: *(state + 0xEC)  Pipeline active flag (1=EVF running)
//  32-35: *(state + 0xD4)  Video mode (2=VGA, 3=QVGA, 7=QVGA60)
//  36-39: *(state + 0x114) Recording callback 1 (set by sub_FF8C3BFC)
//  40-43: *(state + 0x6C)  Recording buffer addr (set by sub_FF8C3BFC)
//  44-47: *(state + 0x118) Recording callback 2 (set by sub_FF8C3BFC)
//  48-51: *(0x0007228C)    JPCORE enable flag (set by FUN_ff9e8190)
//  52-55: GetContinuousMovieJpegVRAMData return value
//  56-59: hw_frame_index at time of call
//  60-63: io_buf[0] after call
//  64-79: First 16 bytes of VRAM buffer at 0x40EA23D0
//  80-83: FUN_ff8c4288() return (MjpegActiveCheck)
//  84-87: GetMovieJpegVRAMHPixelsSize return
//  88-91: GetMovieJpegVRAMVPixelsSize return
//  92-95: *(state + 0xA4)  (cleared by StopContinuousVRAMData)
//  96-99: *(state + 0xB8)  Semaphore/event flag handle
// 100-103: *(state + 0x120) (param_3 stored by DMA trigger)
// --- v3: JPCORE output tracking (bytes 104-127) ---
// 104-107: *(0x2564) = JPCORE output buffer (piVar1[4] from FUN_ff849448)
// 108-111: First 4 bytes at JPCORE output buffer (check for FF D8 SOI)
// 112-115: *(state + 0x60) = ring buffer addr (set by state machine)
// 116-119: First 4 bytes at state[+0x60] (check for FF D8 SOI)
// 120-123: *(0x2580) = first JPCORE buffer array entry
// 124-127: *(0x2584) = second JPCORE buffer array entry
// --- v4: JPCORE control state (bytes 128-191) ---
// 128-131: *(0x2560) = piVar1[3] = JPCORE DMA active flag (1=running)
// 132-135: *(0x2568) = piVar1[5] = must be != -1 for JPCORE to start
// 136-139: *(0x2574) = piVar1[8] = must be 0 for JPCORE to start
// 140-143: *(0x2588) = buf[2] = actual JPCORE output buffer (for video mode 0-2)
// 144-147: first 4 bytes at *(0x2588) (check for FF D8 SOI)
// 148-151: *(0x12850) = secondary index iVar5 (must be 1 for JPCORE)
// 152-155: hw_jpeg_source = which buffer had JPEG (0=none, 1=VRAM, 2=jpcore_out, 3=buf[2])
// 156-159: actual JPEG buffer address used
// 160-163: JPCORE hw register 0xC0F04908 (DMA output destination)
// 164-167: *(state + 0x144) = double-buffer 0 address
// 168-171: *(state + 0x148) = double-buffer 1 address
// 172-175: *(0x257C) = piVar1[10] = frame index from lookup
// 176-179: *(0x2590) = buf[4]
// 180-183: *(0x2598) = buf[6]
// 184-187: *(state + 0xF0) = pipeline frame skip flag
// 188-191: (reserved)
// --- v5: PipelineStep3 state + JPCORE pipeline (bytes 192-255) ---
// 192-195: *(0x8228) = PS3 frame param (from PipelineStep2)
// 196-199: *(0x8230) = PS3 completion bitmask (7=all done, <7=stuck)
// 200-203: *(0x8234) = JPCORE_DMA_Start return (1=success)
// 204-207: *(0x8238) = step 2 completion flag
// 208-211: *(0x823C) = step 3 completion flag
// 212-215: piVar1[0] = JPCORE init flag (must be !=0)
// 216-219: piVar1[1]
// 220-223: piVar1[2]
// 224-227: piVar1[6]
// 228-231: piVar1[7] = semaphore handle
// 232-235: piVar1[9]
// 236-239: JPCORE HW reg 0xC0F04900 (slot 0xb base)
// 240-243: JPCORE HW reg 0xC0F04904 (slot 0xb +4)
// 244-247: JPCORE HW reg 0xC0F04908 (slot 0xb +8 = output addr)
// 248-251: SOI scan offset in piVar1[4] (0xFFFFFFFF = not found in 8KB)
// 252-255: (reserved)
// ============================================================
// Spy buffer: shared with movie_rec.c frame interceptor
// webcam.c initializes the header; movie_rec.c's spy_ring_write
// copies frame data into the buffer and signals the semaphore.
// ============================================================
#define WEBCAM_SPY_ADDR    ((volatile unsigned int *)0x000FF000)
// spy[0] = magic (0x52455753 = active, set by webcam.c)
// spy[1] = data_ptr (frame buffer, malloc'd by webcam.c)
// spy[2] = frame_size (actual bytes copied, set by movie_rec.c)
// spy[3] = frame_cnt (monotonic counter, set by movie_rec.c LAST)
// spy[4] = max_size (buffer capacity, set by webcam.c)
// spy[5] = sem_handle (DryOS semaphore, set by webcam.c)

// Ring buffer base: DAT_ff93050c (ROM literal → RAM address)
// Used by sub_FF92FE8C (MovieFrameGetter) during recording
#define FW_DAT_FF93050C    (*(volatile unsigned int *)0xFF93050C)

#define HW_DIAG_SIZE 576
static unsigned char hw_diag[HW_DIAG_SIZE];
static unsigned int hw_diag_len = 0;
#define FW_MjpegActiveCheck ((void *)0xFF8C4288)

// State struct base address (discovered from ROM literal pool via Ghidra)
// Both DAT_ff8c2e24 and DAT_ff8c43f4 resolve to 0x000070D8
#define HW_STATE_BASE ((volatile unsigned int *)0x000070D8)

// ============================================================
// Hardware MJPEG encoder
// ============================================================

// Start the hardware MJPEG encoder.
//
// Calls StartMjpegMaking to activate the JPCORE hardware encoder, then
// waits for JPCORE to start producing frames.
//
// Previous approaches that failed:
//   - set_movie_status(2): only set a variable, didn't trigger firmware
//   - levent_set_record(): UI events not processed in PTP mode
//   - sub_FF8C3BFC with movie_record_task callbacks: CRASHED the camera
//     because the callbacks (0xFF85D370, 0xFF85D28C) are called as function
//     pointers from FUN_ff8c335c and expect movie_record_task context.
//     These callbacks are for FRAME DELIVERY, not JPCORE activation.
//
// Must be called after the camera is in video mode with an active EVF pipeline.
// Returns 0 on success, -1 on failure.
static int hw_mjpeg_start(void)
{
    int retries;

    if (hw_mjpeg_active) return 0;

    // ================================================================
    // JPCORE POWER / CLOCK INITIALIZATION
    //
    // The JPCORE hardware block needs to be powered and clocked before
    // it can encode frames.  FUN_ff8eeb6c is the firmware's ref-counted
    // power-on function, but it checks *(0x8028)!=0 first and returns
    // early if the JPCORE subsystem module wasn't initialized at boot.
    //
    // Strategy:
    //   1. Try FUN_ff8eeb6c() (proper ref-counted path)
    //   2. If *(0x8028)==0 (no-op), call the three sub-functions directly:
    //      FUN_ff815288(0)  — clock/power gate
    //      FUN_ff8152e8()   — clock domain 2
    //      FUN_ff8ef6b4()   — JPCORE subsystem init
    // ================================================================
    {
        volatile unsigned int *jpwr = JPCORE_POWER_STRUCT;
        unsigned int init_flag = jpwr[0];
        unsigned int ref_count = jpwr[5];  // offset 0x14

        // Try the firmware's ref-counted path first
        call_func_ptr(FW_JPCORE_PowerInit, 0, 0);

        // Check if it actually did anything
        if (init_flag == 0) {
            // FUN_ff8eeb6c was a no-op because the JPCORE module wasn't
            // initialized.  Call the three hardware init functions directly.
            unsigned int args_0[1] = { 0 };
            call_func_ptr(FW_JPCORE_ClockEnable, args_0, 1);
            call_func_ptr(FW_JPCORE_ClockEnable2, 0, 0);
            call_func_ptr(FW_JPCORE_SubsystemInit, 0, 0);
            msleep(50);  // Let hardware stabilize after power-on
        }
    }

    // Activate the JPCORE hardware encoder.
    // Sets state[+0x48] = 1 (MJPEG active flag).
    // Requires state[+0xEC] == 1 (pipeline ready), which is set by
    // EVFSetupInner when entering video mode.
    call_func_ptr(FW_StartMjpegMaking, 0, 0);

    // ================================================================
    // OPTION B: JPCORE QUALITY FIX
    //
    // The JPCORE hardware encoder requires a valid quality index at
    // RAM 0x2568 (piVar1[5]). Without initialization, piVar1[5] = -1,
    // causing JPCORE_DMA_Start to return early (JPEG never produced).
    //
    // FUN_ff849408(quality) is a trivial RAM write that sets:
    //   piVar1[5] = quality + 1
    // With quality = 0xe, piVar1[5] becomes 0xf (valid).
    // This is the same value the firmware uses during video recording.
    //
    // After this fix, JPCORE_DMA_Start proceeds and the hardware encoder
    // should write JPEG frames to the output buffer at *(0x2564).
    // ================================================================
    {
        unsigned int quality_arg = 0xe;
        call_func_ptr(FW_JPCORE_SetQuality, &quality_arg, 1);
    }

    // CRITICAL: Force state[+0xD4] = 2 to switch FrameProcessing to video
    // recording path (FUN_ff9e508c) instead of EVF/LCD path (FUN_ff9e51d8).
    //
    // The difference: FrameProcessing calls FUN_ffa02ddc with mode=5 (video)
    // vs mode=4 (EVF). Mode 5 configures the pipeline resizer to route data
    // to JPCORE's hardware input, while mode 4 only outputs to the LCD.
    // Without this, JPCORE is started but never receives input data.
    //
    // state[+0xD4] is checked in PipelineFrameCallback → FrameProcessing:
    //   value 2 or 3 → iVar2=2 → FUN_ff9e508c (video path, mode 5)
    //   anything else → iVar2=1 → FUN_ff9e51d8 (EVF path, mode 4)
    {
        volatile unsigned int *s = HW_STATE_BASE;
        s[0xD4/4] = 2;  // Force video recording FrameProcessing path
    }

    // Write ISP routing register directly to route ISP output to JPCORE.
    // FUN_ffa02ddc normally does this when called with mode=5 (video path),
    // but state[+0xD4]=2 only controls the FrameProcessing dispatch — it
    // doesn't immediately reconfigure the ISP. Writing the register directly
    // should connect ISP → JPCORE without needing sub_FF8C3BFC.
    //
    // 0xC0F110C4: ISP source select register (write-only)
    //   4 = EVF/LCD path
    //   5 = video/JPCORE encoding path
    {
        volatile unsigned int *isp_src = (volatile unsigned int *)0xC0F110C4;
        *isp_src = 5;
    }

    // NOTE: sub_FF8C3BFC(0xFF85D370, 0x1AB94, 0xFF85D28C) — recording
    // pipeline setup — CRASHES even with pipeline in video mode (v10 test).
    // The callbacks need full movie_record_task context (ring buffer at
    // DAT_ff93050c, AVI writer state) which isn't initialized. DO NOT call.
    //
    // Instead, set state[+0x114] to our own spy callback that just captures
    // the arguments. This tells us: (a) if the callback IS called at all,
    // (b) what arguments it receives (potentially JPEG data ptr/size).
    {
        volatile unsigned int *s = HW_STATE_BASE;
        s[0x114/4] = (unsigned int)rec_callback_spy;
        // Leave state[+0x6C] and state[+0x118] as 0 for safety
    }

    // Initialize the shared spy buffer at 0xFF000.
    // movie_rec.c will write JPEG ptr/size here during real recording.
    {
        volatile unsigned int *spy = WEBCAM_SPY_ADDR;
        int si;
        for (si = 0; si < 16; si++) spy[si] = 0;
    }

    // Wait for pipeline to start.
    // For raw UYVY streaming, we just need rec_callback_spy to fire
    // (rec_cb_count > 0). Also accept JPCORE ready as a break condition.
    for (retries = 0; retries < 20; retries++) {
        msleep(100);
        if (rec_cb_count > 0) break;  // Callback firing — pipeline active
        {
            volatile unsigned int *ps3 = (volatile unsigned int *)0x00008224;
            volatile unsigned int *jpcore = (volatile unsigned int *)0x00002554;
            unsigned int cmask = ps3[3];
            unsigned int active = jpcore[3];
            if ((cmask & 6) == 6 && active == 1) break;
        }
    }

    hw_mjpeg_active = 1;
    hw_mjpeg_available = 1;

    // Collect DMA chain diagnostics after starting recording.
    // This answers: "is the pipeline set up properly?"
    // Uses same layout as failure diagnostics for bridge parser compatibility.
    {
        volatile unsigned int *s = HW_STATE_BASE;
        volatile unsigned int *ps3 = (volatile unsigned int *)0x00008224;
        volatile unsigned int *jpcore = (volatile unsigned int *)0x00002554;
        volatile unsigned int *jpbuf  = (volatile unsigned int *)0x00002580;
        unsigned int val;
        int i;

        #define DIAG_U32(off, v) do { \
            hw_diag[(off)]   = (v);       hw_diag[(off)+1] = (v)>>8; \
            hw_diag[(off)+2] = (v)>>16;   hw_diag[(off)+3] = (v)>>24; \
        } while(0)

        DIAG_U32( 0, s[0x48/4]);   // +0x48 MJPEG active
        DIAG_U32( 4, s[0x4C/4]);   // +0x4C
        DIAG_U32( 8, s[0x54/4]);   // +0x54 DMA status
        DIAG_U32(12, s[0x58/4]);   // +0x58 frame index
        DIAG_U32(16, s[0x5C/4]);   // +0x5C DMA request state
        DIAG_U32(20, s[0x64/4]);   // +0x64 VRAM buffer addr
        DIAG_U32(24, s[0xA0/4]);   // +0xA0 callback
        DIAG_U32(28, s[0xEC/4]);   // +0xEC pipeline active
        DIAG_U32(32, s[0xD4/4]);   // +0xD4 video mode
        DIAG_U32(36, s[0x114/4]);  // +0x114 rec callback 1 (from sub_FF8C3BFC)
        DIAG_U32(40, s[0x6C/4]);   // +0x6C rec buffer
        DIAG_U32(44, s[0x118/4]);  // +0x118 rec callback 2
        DIAG_U32(48, *(volatile unsigned int *)0x0007228C); // JPCORE enable
        DIAG_U32(52, *(volatile unsigned int *)0x000051E4); // movie_status
        DIAG_U32(56, (unsigned int)retries);                // wait retries used
        {
            volatile unsigned int *jpwr = JPCORE_POWER_STRUCT;
            // Pack JPCORE power struct state into one diagnostic word:
            // bits 0-7:  init flag (jpwr[0])
            // bits 8-15: ref count (jpwr[5], offset 0x14)
            // bits 16-23: secondary flag (jpwr[4], offset 0x10)
            // bits 24-31: 0xAB marker
            DIAG_U32(60, 0xAB000000 | ((jpwr[4] & 0xFF) << 16)
                                     | ((jpwr[5] & 0xFF) << 8)
                                     | (jpwr[0] & 0xFF));
        }
        for (i = 0; i < 16; i++) hw_diag[64 + i] = HW_MJPEG_VRAM_ADDR[i];
        val = call_func_ptr(FW_MjpegActiveCheck, 0, 0);
        DIAG_U32(80, val);
        val = call_func_ptr(FW_GetMovieJpegVRAMHPixelsSize, 0, 0);
        DIAG_U32(84, val);
        val = call_func_ptr(FW_GetMovieJpegVRAMVPixelsSize, 0, 0);
        DIAG_U32(88, val);
        DIAG_U32(92, s[0xA4/4]);
        DIAG_U32(96, s[0xB8/4]);
        DIAG_U32(100, s[0x120/4]);
        {
            unsigned int jpout = jpcore[4];
            DIAG_U32(104, jpout);
            if (jpout >= 0x00010000 && jpout < 0x80000000) {
                volatile unsigned char *p = (volatile unsigned char *)jpout;
                hw_diag[108] = p[0]; hw_diag[109] = p[1];
                hw_diag[110] = p[2]; hw_diag[111] = p[3];
            } else {
                DIAG_U32(108, 0xDEAD0001);
            }
        }
        DIAG_U32(112, s[0x60/4]);
        {
            unsigned int rb = s[0x60/4];
            if (rb >= 0x00010000 && rb < 0x80000000) {
                volatile unsigned char *p = (volatile unsigned char *)rb;
                hw_diag[116] = p[0]; hw_diag[117] = p[1];
                hw_diag[118] = p[2]; hw_diag[119] = p[3];
            } else {
                DIAG_U32(116, 0xDEAD0002);
            }
        }
        DIAG_U32(120, jpbuf[0]);
        DIAG_U32(124, jpbuf[1]);
        DIAG_U32(128, jpcore[3]);   // piVar1[3] DMA active
        DIAG_U32(132, jpcore[5]);
        DIAG_U32(136, jpcore[8]);
        {
            unsigned int b2 = jpbuf[2];
            DIAG_U32(140, b2);
            if (b2 >= 0x00010000 && b2 < 0x80000000) {
                volatile unsigned char *p = (volatile unsigned char *)b2;
                hw_diag[144] = p[0]; hw_diag[145] = p[1];
                hw_diag[146] = p[2]; hw_diag[147] = p[3];
            } else {
                DIAG_U32(144, 0xDEAD0003);
            }
        }
        DIAG_U32(148, *(volatile unsigned int *)0x00012850);
        DIAG_U32(152, s[0xF4/4]);   // state[+0xF4] pipeline config
        DIAG_U32(156, *(volatile unsigned int *)0xC0F0490C); // JPCORE reg +0x0C
        DIAG_U32(160, *(volatile unsigned int *)0xC0F04908);
        DIAG_U32(164, s[0x144/4]);
        DIAG_U32(168, s[0x148/4]);
        DIAG_U32(172, jpcore[10]);
        DIAG_U32(176, jpbuf[4]);
        DIAG_U32(180, jpbuf[6]);
        DIAG_U32(184, s[0xF0/4]);
        DIAG_U32(188, *(volatile unsigned int *)0xC0F04910); // JPCORE reg +0x10
        DIAG_U32(192, ps3[1]);     // PS3 frame param
        DIAG_U32(196, ps3[3]);     // PS3 completion mask
        DIAG_U32(200, ps3[4]);     // JPCORE_DMA_Start return
        DIAG_U32(204, ps3[5]);
        DIAG_U32(208, ps3[6]);
        DIAG_U32(212, jpcore[0]);
        DIAG_U32(216, jpcore[1]);
        DIAG_U32(220, jpcore[2]);
        DIAG_U32(224, jpcore[6]);
        DIAG_U32(228, jpcore[7]);
        DIAG_U32(232, jpcore[9]);
        DIAG_U32(236, *(volatile unsigned int *)0xC0F04900);
        DIAG_U32(240, *(volatile unsigned int *)0xC0F04904);
        DIAG_U32(244, *(volatile unsigned int *)0xC0F04908);
        {
            unsigned int soi_off = 0xFFFFFFFF;
            unsigned int jpout = jpcore[4];
            if (jpout >= 0x00010000 && jpout < 0x80000000) {
                volatile unsigned char *p = (volatile unsigned char *)jpout;
                unsigned int si;
                for (si = 0; si < 8190; si++) {
                    if (p[si] == 0xFF && p[si+1] == 0xD8) {
                        soi_off = si;
                        break;
                    }
                }
            }
            DIAG_U32(248, soi_off);
        }
        DIAG_U32(252, *(volatile unsigned int *)0xC0F110C4);  // ISP source select (4=EVF, 5=VIDEO)

        #undef DIAG_U32
        hw_diag_len = HW_DIAG_SIZE;
    }

    // Query the hardware encoder's frame dimensions
    {
        unsigned int w, h;
        w = call_func_ptr(FW_GetMovieJpegVRAMHPixelsSize, 0, 0);
        h = call_func_ptr(FW_GetMovieJpegVRAMVPixelsSize, 0, 0);
        if (w > 0 && w <= 1280 && h > 0 && h <= 1024) {
            frame_width = w;
            frame_height = h;
        }
    }

    return 0;
}

// Stop the hardware MJPEG encoder.
static void hw_mjpeg_stop(void)
{
    if (!hw_mjpeg_active) return;

    // Restore state[+0xD4] to EVF mode before stopping
    {
        volatile unsigned int *s = HW_STATE_BASE;
        s[0xD4/4] = 1;  // Back to EVF/LCD path
    }

    // Stop the JPCORE hardware encoder.
    call_func_ptr(FW_StopMjpegMaking, 0, 0);

    // Power down JPCORE (ref-counted, matches PowerInit call in start)
    call_func_ptr(FW_JPCORE_PowerDeinit, 0, 0);

    // Wait for pipeline to settle
    {
        int retries;
        for (retries = 0; retries < 20; retries++) {
            msleep(100);
            if (get_movie_status() <= 1) break;
        }
    }

    hw_mjpeg_active = 0;
    hw_jpeg_data = 0;
    hw_jpeg_size = 0;
}

// Find the end of a JPEG frame by scanning for the EOI marker (FF D9).
// In entropy-coded JPEG data, 0xFF bytes are always followed by 0x00
// (byte stuffing), so the sequence FF D9 uniquely identifies EOI.
// Returns the total frame size (including EOI), or 0 if not found.
static unsigned int find_jpeg_eoi(volatile unsigned char *buf, unsigned int max_len)
{
    unsigned int i;
    // Start scanning after SOI (FF D8) at offset 2
    for (i = 2; i < max_len - 1; i++) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
            return i + 2;
        }
    }
    return 0;
}

// Get a hardware-encoded JPEG frame.
// v11: Multi-source spy approach.
// 1. Check movie_rec.c spy buffer at 0xFF000 (JPEG from real recording)
// 2. Try JPCORE buffers (VRAM, double-buffers, piVar1[4])
// 3. Always collect comprehensive diagnostics (512 bytes)
//
// Returns frame size on success, 0 on failure.
static int hw_mjpeg_get_frame(void)
{
    volatile unsigned int *spy = WEBCAM_SPY_ADDR;
    volatile unsigned char *src = 0;
    unsigned int soi_off = 0;
    unsigned int frame_size;
    unsigned int fi;
    int source = 0;
    // 0=none, 1=spy_buf(recording), 2=VRAM, 3=db0, 4=db1, 5=piVar1[4], 6=buf[2], 7=rec_cb_arg2
    int gf_rc = -1;

    // ---- Source 1: Check spy buffer from movie_rec.c ----
    // If real recording is active, movie_rec.c writes JPEG ptr/size here.
    {
        unsigned int spy_magic = spy[0];
        unsigned int spy_ptr = spy[1];
        unsigned int spy_size = spy[2];
        unsigned int spy_cnt = spy[3];

        if (spy_magic == 0x52455753 && spy_ptr != 0 && spy_size > 0
            && spy_size < JPEG_BUF_SIZE && spy_cnt > 0) {
            // Real recording frame available!
            volatile unsigned char *p = (volatile unsigned char *)spy_ptr;
            // Verify it looks like JPEG (FF D8 at start)
            if (p[0] == 0xFF && p[1] == 0xD8) {
                src = p;
                soi_off = 0;
                frame_size = spy_size;
                source = 1;
            }
        }
    }

    // ---- Source 2: Scan VRAM buffer directly ----
    // Previously called GetContinuousMovieJpegVRAMData here, but it blocks
    // forever on the DryOS event flag (timeout=0) when the recording pipeline
    // isn't fully set up.  The pipeline callback IS firing at 30fps, so just
    // scan the VRAM buffer directly.
    if (!src) {
        volatile unsigned char *p = HW_MJPEG_VRAM_ADDR;
        for (fi = 0; fi < 8190; fi++) {
            if (p[fi] == 0xFF && p[fi + 1] == 0xD8) {
                soi_off = fi; src = p; source = 2; break;
            }
        }
        gf_rc = -99;  // indicate we skipped the firmware call
    }

    // Sources 3-7 REMOVED: scanning 64KB of uncached DMA buffers per frame
    // caused cumulative bus stalls (camera hangs after ~600 frames) and
    // produced only false-positive SOI matches in YUV viewport data.
    // The JPCORE is initialized but not connected to the pipeline input,
    // so no buffer contains real JPEG data.

    // ============================================================
    // MASSIVE SPY DIAGNOSTICS — capture everything, every frame
    // ============================================================
    {
        volatile unsigned int *s = HW_STATE_BASE;
        volatile unsigned int *ps3 = (volatile unsigned int *)0x00008224;
        volatile unsigned int *jpcore = (volatile unsigned int *)0x00002554;
        volatile unsigned int *jpbuf  = (volatile unsigned int *)0x00002580;
        volatile unsigned int *movtask = (volatile unsigned int *)0x000051A8;
        unsigned int ring_base;
        int i;

        #define DIAG_U32(off, v) do { \
            hw_diag[(off)]   = (v);       hw_diag[(off)+1] = (v)>>8; \
            hw_diag[(off)+2] = (v)>>16;   hw_diag[(off)+3] = (v)>>24; \
        } while(0)

        // ---- Block 0 (0-63): MJPEG State @ 0x70D8 ----
        DIAG_U32( 0, s[0x48/4]);    // MJPEG active
        DIAG_U32( 4, s[0x4C/4]);    // paired flag
        DIAG_U32( 8, s[0x54/4]);    // DMA status
        DIAG_U32(12, s[0x58/4]);    // DMA frame idx
        DIAG_U32(16, s[0x5C/4]);    // DMA req state
        DIAG_U32(20, s[0x60/4]);    // ring buf addr
        DIAG_U32(24, s[0x64/4]);    // VRAM buf addr
        DIAG_U32(28, s[0x6C/4]);    // rec buffer
        DIAG_U32(32, s[0x80/4]);    // cleanup callback
        DIAG_U32(36, s[0xA0/4]);    // DMA callback
        DIAG_U32(40, s[0xB0/4]);    // event flag
        DIAG_U32(44, s[0xD4/4]);    // video mode
        DIAG_U32(48, s[0xEC/4]);    // pipeline active
        DIAG_U32(52, s[0xF0/4]);    // frame skip
        DIAG_U32(56, s[0x114/4]);   // rec callback 1
        DIAG_U32(60, s[0x118/4]);   // rec callback 2

        // ---- Block 1 (64-127): Movie Task @ 0x51A8 ----
        DIAG_U32(64, movtask[0]);          // +0x00
        DIAG_U32(68, movtask[0x1C/4]);     // +0x1C msg queue
        DIAG_U32(72, movtask[0x24/4]);     // +0x24 flag
        DIAG_U32(76, movtask[0x28/4]);     // +0x28 counter
        DIAG_U32(80, movtask[0x2C/4]);     // +0x2C flag
        DIAG_U32(84, movtask[0x30/4]);     // +0x30 counter
        DIAG_U32(88, movtask[0x38/4]);     // +0x38 counter
        DIAG_U32(92, movtask[0x3C/4]);     // +0x3C STATE (0=idle,3=init,4=rec,5=stop)
        DIAG_U32(96, movtask[0x48/4]);     // +0x48
        DIAG_U32(100, movtask[0x4C/4]);    // +0x4C
        DIAG_U32(104, movtask[0x50/4]);    // +0x50 frame counter
        DIAG_U32(108, movtask[0x54/4]);    // +0x54 error code
        DIAG_U32(112, movtask[0xA0/4]);    // +0xA0 callback
        DIAG_U32(116, movtask[0x68/4]);    // +0x68
        DIAG_U32(120, movtask[0x6C/4]);    // +0x6C
        DIAG_U32(124, *(volatile unsigned int *)0x000051E4); // movie_status

        // ---- Block 2 (128-191): Ring Buffer @ DAT_ff93050c ----
        ring_base = FW_DAT_FF93050C;
        DIAG_U32(128, ring_base);           // ring buffer base address
        if (ring_base >= 0x00001000 && ring_base < 0x04000000) {
            volatile unsigned int *rb = (volatile unsigned int *)ring_base;
            DIAG_U32(132, rb[0x1C/4]);      // write ptr
            DIAG_U32(136, rb[0x28/4]);      // frame count
            DIAG_U32(140, rb[0x40/4]);      // max frames
            DIAG_U32(144, rb[0x5C/4]);      // frame data size
            DIAG_U32(148, rb[0x70/4]);      // total frame size
            DIAG_U32(152, rb[0xBC/4]);      // index
            DIAG_U32(156, rb[0xC0/4]);      // buffer start
            DIAG_U32(160, rb[0xC4/4]);      // buffer end/wrap
            DIAG_U32(164, rb[0xC8/4]);      // status
            DIAG_U32(168, rb[0xD4/4]);      // value
            DIAG_U32(172, rb[0x148/4]);     // remaining lo
            DIAG_U32(176, rb[0x14C/4]);     // remaining hi
            DIAG_U32(180, rb[0x150/4]);     // used lo
            DIAG_U32(184, rb[0x154/4]);     // used hi
            DIAG_U32(188, rb[0x94/4]);      // divisor
        } else {
            for (i = 132; i < 192; i += 4) DIAG_U32(i, 0xDEAD0000);
        }

        // ---- Block 3 (192-255): JPCORE + HW regs ----
        DIAG_U32(192, *(volatile unsigned int *)0x0007228C);  // JPCORE flag
        DIAG_U32(196, jpcore[0]);      // init
        DIAG_U32(200, jpcore[3]);      // active
        DIAG_U32(204, jpcore[4]);      // output buf
        DIAG_U32(208, jpcore[5]);      // must != -1
        DIAG_U32(212, jpcore[10]);     // index
        DIAG_U32(216, jpbuf[2]);       // buf[2]
        DIAG_U32(220, ps3[3]);         // completion mask
        DIAG_U32(224, *(volatile unsigned int *)0xC0F04908);  // HW DMA out
        DIAG_U32(228, s[0x144/4]);     // dbl-buf 0
        DIAG_U32(232, s[0x148/4]);     // dbl-buf 1
        DIAG_U32(236, s[0x120/4]);     // param3
        DIAG_U32(240, s[0xA4/4]);      // cleared by StopCont
        DIAG_U32(244, s[0xB8/4]);      // semaphore
        DIAG_U32(248, *(volatile unsigned int *)0x00012850);  // iVar5
        DIAG_U32(252, (unsigned int)source);  // which source found JPEG

        // ---- Block 4 (256-319): Spy Results ----
        DIAG_U32(256, spy[0]);         // magic
        DIAG_U32(260, spy[1]);         // jpeg_ptr from recording
        DIAG_U32(264, spy[2]);         // jpeg_size from recording
        DIAG_U32(268, spy[3]);         // frame_count from recording
        DIAG_U32(272, spy[4]);         // init_flag
        DIAG_U32(276, spy[5]);         // last error
        DIAG_U32(280, spy[6]);         // error count
        DIAG_U32(284, spy[7]);         // metadata1
        DIAG_U32(288, spy[8]);         // metadata2
        DIAG_U32(292, spy[9]);         // task_state at frame time
        DIAG_U32(296, spy[10]);        // task_callback at frame time
        DIAG_U32(300, rec_cb_count);   // pipeline callback spy count
        DIAG_U32(304, rec_cb_arg0);    // pipeline callback arg0
        DIAG_U32(308, rec_cb_arg1);    // pipeline callback arg1
        DIAG_U32(312, rec_cb_arg2);    // pipeline callback arg2
        DIAG_U32(316, rec_cb_arg3);    // pipeline callback arg3

        // ---- Block 5 (320-383): Buffer Samples ----
        // First 16 bytes of VRAM
        for (i = 0; i < 16; i++) hw_diag[320 + i] = HW_MJPEG_VRAM_ADDR[i];
        // First 16 bytes at spy jpeg_ptr (if valid)
        {
            unsigned int sp = spy[1];
            if (sp >= 0x00010000 && sp < 0x44000000) {
                volatile unsigned char *p = (volatile unsigned char *)sp;
                for (i = 0; i < 16; i++) hw_diag[336 + i] = p[i];
            } else {
                for (i = 0; i < 16; i++) hw_diag[336 + i] = 0xEE;
            }
        }
        // First 16 bytes at rec_cb_arg2 (pipeline callback buffer — THE KEY LEAD)
        {
            unsigned int cb2 = rec_cb_arg2;
            if (cb2 >= 0x40000000 && cb2 < 0x44000000) {
                volatile unsigned char *p = (volatile unsigned char *)cb2;
                for (i = 0; i < 16; i++) hw_diag[352 + i] = p[i];
            } else if (cb2 >= 0x00010000 && cb2 < 0x04000000) {
                volatile unsigned char *p = (volatile unsigned char *)cb2;
                for (i = 0; i < 16; i++) hw_diag[352 + i] = p[i];
            } else {
                for (i = 0; i < 16; i++) hw_diag[352 + i] = 0xDD;
            }
        }
        // First 16 bytes at rec_cb_arg2 + 0x100 (check deeper into buffer)
        {
            unsigned int cb2 = rec_cb_arg2;
            if (cb2 >= 0x00010000 && cb2 < 0x44000000) {
                volatile unsigned char *p = (volatile unsigned char *)(cb2 + 0x100);
                for (i = 0; i < 16; i++) hw_diag[368 + i] = p[i];
            } else {
                for (i = 0; i < 16; i++) hw_diag[368 + i] = 0xAA;
            }
        }

        // ---- Block 6 (384-455): ISP routing + dispatch state ----
        DIAG_U32(384, *(volatile unsigned int *)0xC0F110C4);  // ISP source select (4=EVF, 5=VIDEO)
        DIAG_U32(388, hw_frame_index);
        DIAG_U32(392, *(volatile unsigned int *)0xC0F111C4);  // ISP scaling factor
        DIAG_U32(396, *(volatile unsigned int *)0xC0F111C0);  // ISP enable (should be 1)
        DIAG_U32(400, *(volatile unsigned int *)0xC0F111C8);  // ISP special config
        DIAG_U32(404, *(volatile unsigned int *)0xC0F0103C);  // JPCORE enable reg
        DIAG_U32(408, *(volatile unsigned int *)0x0000B8C0);  // dispatch path (1=EVF, 2=video)
        DIAG_U32(412, s[0xD4/4]);   // video mode (should be 2 for video dispatch)
        DIAG_U32(416, s[0x48/4]);   // MJPEG active
        DIAG_U32(420, s[0xF0/4]);   // frame skip / pipeline state
        DIAG_U32(424, s[0xEC/4]);   // pipeline active
        DIAG_U32(428, (unsigned int)gf_rc);   // GCMJVD return
        DIAG_U32(432, *(volatile unsigned int *)0x0003F298);  // secondary state +4 (written by ff8c335c)
        DIAG_U32(436, *(volatile unsigned int *)0x0003F29C);  // secondary state +8
        DIAG_U32(440, s[0x6C/4]);   // rec buffer (must be !=0 for callback data)
        DIAG_U32(444, s[0x114/4]);  // rec callback 1 (our spy)
        DIAG_U32(448, s[0x100/4]);
        DIAG_U32(452, s[0x104/4]);

        // ---- Block 7 (456-511): More JPCORE + ring buffer ----
        DIAG_U32(456, jpcore[1]);
        DIAG_U32(460, jpcore[2]);
        DIAG_U32(464, jpcore[6]);
        DIAG_U32(468, jpcore[7]);     // semaphore
        DIAG_U32(472, jpcore[9]);
        DIAG_U32(476, jpbuf[0]);      // buf[0]
        DIAG_U32(480, jpbuf[1]);      // buf[1]
        DIAG_U32(484, jpbuf[4]);      // buf[4]
        DIAG_U32(488, jpbuf[6]);      // buf[6]
        DIAG_U32(492, ps3[1]);        // PS3 frame param
        DIAG_U32(496, ps3[4]);        // JPCORE_DMA_Start ret
        DIAG_U32(500, ps3[5]);        // step2
        DIAG_U32(504, ps3[6]);        // step3
        DIAG_U32(508, *(volatile unsigned int *)0xC0F04900); // HW slot base

        // ---- Block 8 (512-575): ISR Callback + Main-thread Buffer Samples ----
        // ISR callback captures addresses/HW regs only (safe).
        // Main thread (here) reads buffer contents and checks for SOI.
        // ---- Block 8 (512-575): ISR Callback Addresses + Counters ----
        // Lightweight: only addresses, HW reg, and counters.
        // NO uncached buffer reads — those caused bus stalls after ~600 frames.
        rec_cb_dma_addr = *(volatile unsigned int *)0xC0F04908;
        DIAG_U32(512, rec_cb_dma_addr);      // HW DMA reg (main thread read)
        DIAG_U32(516, rec_cb_arg2_sample);   // arg2 value
        DIAG_U32(520, rec_cb_db0_sample);    // db0 addr
        DIAG_U32(524, rec_cb_db1_sample);    // db1 addr
        DIAG_U32(528, rec_cb_buf2_sample);   // buf[2] addr
        DIAG_U32(532, rec_cb_count);         // callback count
        DIAG_U32(536, hw_fail_soi);          // SOI failure count
        DIAG_U32(540, hw_fail_eoi);          // EOI failure count
        DIAG_U32(544, hw_fail_total);        // total failures
        DIAG_U32(548, hw_frame_index);       // frames captured (false positives removed)
        // 552-575: JPCORE power struct @ 0x8028
        {
            volatile unsigned int *jpwr = JPCORE_POWER_STRUCT;
            DIAG_U32(552, jpwr[0]);     // init flag (must be !=0)
            DIAG_U32(556, jpwr[1]);     // semaphore handle
            DIAG_U32(560, jpwr[4]);     // secondary flag (offset 0x10)
            DIAG_U32(564, jpwr[5]);     // ref count (offset 0x14)
            DIAG_U32(568, jpwr[2]);     // offset 0x08
            DIAG_U32(572, jpwr[3]);     // offset 0x0C
        }

        #undef DIAG_U32
        hw_diag_len = HW_DIAG_SIZE;
    }

    if (!src) {
        hw_fail_soi++;
        hw_fail_total++;
        return 0;
    }

    // For source=1 (spy buffer), frame_size is already set from spy[2].
    // For other sources, scan for EOI marker to determine frame size.
    if (source != 1) {
        unsigned int scan_limit = HW_MJPEG_VRAM_BUFSIZE - soi_off;
        if (scan_limit > JPEG_BUF_SIZE) scan_limit = JPEG_BUF_SIZE;
        frame_size = find_jpeg_eoi(src + soi_off, scan_limit);
        if (frame_size == 0) {
            hw_fail_eoi++;
            hw_fail_total++;
            return 0;
        }
    }

    // Allocate copy buffer if needed (hardware path shares jpeg_buf[0])
    if (!jpeg_buf[0]) {
        jpeg_buf[0] = malloc(JPEG_BUF_SIZE);
        if (!jpeg_buf[0]) return 0;
    }

    // Copy frame data to our buffer.
    // Source buffers may be overwritten by next DMA/frame, so copy now.
    if (frame_size > JPEG_BUF_SIZE) frame_size = JPEG_BUF_SIZE;
    {
        volatile unsigned char *frame_start = src + soi_off;
        for (fi = 0; fi < frame_size; fi++) {
            jpeg_buf[0][fi] = frame_start[fi];
        }
    }

    hw_jpeg_data = jpeg_buf[0];
    hw_jpeg_size = frame_size;
    hw_frame_index++;

    return (int)frame_size;
}

// ============================================================
// Software JPEG encoder (fallback path)
// ============================================================

// Capture a frame from the viewport buffer and compress to JPEG.
// Returns the size of the compressed JPEG, or 0 on error.
static int capture_and_compress_frame_sw(void)
{
    int jpeg_size;
    int buf_idx;
    unsigned int cb_addr;

    sw_total_calls++;

    // Select the next write buffer
    buf_idx = jpeg_buf_current;

    // Ensure buffer is allocated
    if (!jpeg_buf[buf_idx]) {
        jpeg_buf[buf_idx] = malloc(JPEG_BUF_SIZE);
        if (!jpeg_buf[buf_idx]) {
            return 0;
        }
    }

    // Try 640x480 video pipeline buffer first (from rec_callback_spy arg2).
    // The pipeline callback fires at 30fps and provides the full-resolution
    // video frame buffer address in arg2. These are 640x480 UYVY (YUV422)
    // frames from the ISP output — the same data that JPCORE would encode.
    //
    // Use the CACHED mirror (mask off 0x40000000) for performance.
    // The DMA writes to uncached RAM, but the cached mirror will fetch
    // fresh data on first access since our buffer is >> cache size (16 KB).
    cb_addr = rec_cb_arg2;
    if (cb_addr >= 0x40010000 && cb_addr < 0x44000000 && rec_cb_count > 0) {
        const unsigned char *frame = (const unsigned char *)(cb_addr & 0x0FFFFFFF);
        sw_last_vp_addr = cb_addr;

        jpeg_size = tje_encode_uyvy(
            jpeg_buf[buf_idx],
            JPEG_BUF_SIZE,
            640,
            480,
            frame,
            640 * 2,   // UYVY stride = width * 2
            webcam_jpeg_quality
        );

        if (jpeg_size > 0 && jpeg_size <= JPEG_BUF_SIZE) {
            jpeg_buf_size[buf_idx] = jpeg_size;
            frame_width = 640;
            frame_height = 480;
            jpeg_buf_ready = buf_idx;
            jpeg_buf_current = (buf_idx + 1) % NUM_JPEG_BUFS;
            return jpeg_size;
        }
        sw_fail_encode++;
    }

    // Fallback: 720x240 viewport buffer (LCD preview)
    {
        void *vp_buf;
        int vp_width, vp_height, vp_byte_width;

        vp_buf = vid_get_viewport_active_buffer();
        if (!vp_buf) {
            sw_fail_null++;
            return 0;
        }
        sw_last_vp_addr = (unsigned int)vp_buf;

        vp_byte_width = vid_get_viewport_byte_width();
        vp_height = vid_get_viewport_height_proper();

        if (vp_byte_width >= 6) {
            vp_width = (vp_byte_width * 4) / 6;
        } else {
            vp_width = 0;
        }

        if (vp_width < 16 || vp_height < 16 || vp_byte_width < 16) {
            vp_width = 720;
            vp_height = 240;
            vp_byte_width = (720 * 6) / 4;
        }

        jpeg_size = tje_encode_yuv411(
            jpeg_buf[buf_idx],
            JPEG_BUF_SIZE,
            vp_width,
            vp_height,
            (const unsigned char *)vp_buf,
            vp_byte_width,
            webcam_jpeg_quality
        );

        if (jpeg_size > 0 && jpeg_size <= JPEG_BUF_SIZE) {
            jpeg_buf_size[buf_idx] = jpeg_size;
            frame_width = vp_width;
            frame_height = vp_height;
            jpeg_buf_ready = buf_idx;
            jpeg_buf_current = (buf_idx + 1) % NUM_JPEG_BUFS;
        } else {
            sw_fail_encode++;
        }

        return jpeg_size;
    }
}

// ============================================================
// Raw UYVY capture (zero encoding, PC-side conversion)
// ============================================================

// Capture a raw 640x480 UYVY frame from the video pipeline callback.
// The rec_callback_spy (installed by hw_mjpeg_start) provides the frame
// buffer address in rec_cb_arg2 at 30fps. We copy the raw UYVY data
// to our own buffer for PTP transfer — the PC bridge converts to RGB.
//
// Returns UYVY_BUF_SIZE (614400) on success, 0 if no new frame available.
static int capture_frame_uyvy(void)
{
    unsigned int cb_cnt, cb_addr;

    cb_cnt = rec_cb_count;
    if (cb_cnt == 0) return 0;              // Callback hasn't fired yet
    if (cb_cnt == last_cb_count_raw) return 0; // Same frame as last time

    cb_addr = rec_cb_arg2;
    if (cb_addr < 0x40010000 || cb_addr >= 0x44000000) return 0;

    last_cb_count_raw = cb_cnt;

    if (!uyvy_buf) {
        uyvy_buf = malloc(UYVY_BUF_SIZE);
        if (!uyvy_buf) return 0;
    }

    // Copy 614KB UYVY from uncached DMA buffer to our private buffer.
    // Use word-aligned copy for performance (~3ms vs ~16ms byte-by-byte).
    // Source is uncached RAM (0x40xxxxxx) — guaranteed fresh, no cache
    // coherency issues with DMA.
    {
        const unsigned int *src = (const unsigned int *)cb_addr;
        unsigned int *dst = (unsigned int *)uyvy_buf;
        unsigned int i, n = UYVY_BUF_SIZE / 4;
        for (i = 0; i < n; i++) dst[i] = src[i];
    }

    frame_width = 640;
    frame_height = 480;
    frame_format = WEBCAM_FMT_UYVY;

    return UYVY_BUF_SIZE;
}

// ============================================================
// JPCORE hardware JPEG capture (Option B: quality fix)
// ============================================================

// Capture a hardware-encoded JPEG frame from the JPCORE output buffer.
// After the quality fix in hw_mjpeg_start(), JPCORE should write JPEG
// frames to the buffer at piVar1[4] (*(0x2564)).
//
// Returns frame size on success, 0 if no JPEG available.
static int capture_frame_hwjpeg(void)
{
    volatile unsigned int *jpcore = (volatile unsigned int *)0x00002554;
    unsigned int jpout;
    volatile unsigned char *p;
    unsigned int cb_cnt;

    if (!hw_mjpeg_active) return 0;

    // Stale frame detection: wait for a new pipeline callback
    cb_cnt = rec_cb_count;
    if (cb_cnt == 0) return 0;
    if (cb_cnt == last_cb_count_hwjpeg) return 0;

    // Try multiple candidate addresses for JPCORE JPEG output:
    //   1. piVar1[4] at *(0x2564) — JPCORE state struct output buf
    //   2. HW DMA register 0xC0F04908 — actual DMA output destination
    //   3. rec_cb_arg2 — pipeline callback arg2 (frame buffer)
    {
        unsigned int candidates[3];
        int ci, found = 0;

        candidates[0] = jpcore[4];                                    // piVar1[4]
        candidates[1] = *(volatile unsigned int *)0xC0F04908;         // HW DMA out reg
        candidates[2] = rec_cb_arg2;                                  // callback arg2

        for (ci = 0; ci < 3 && !found; ci++) {
            jpout = candidates[ci];
            if (jpout < 0x00010000 || jpout >= 0x44000000) continue;

            // Access via uncached mirror to see DMA-written data
            if (jpout < 0x04000000) {
                p = (volatile unsigned char *)(jpout | 0x40000000);
            } else {
                p = (volatile unsigned char *)jpout;
            }

            // Check for JPEG SOI marker (FF D8)
            if (p[0] == 0xFF && p[1] == 0xD8) {
                found = 1;
            }
        }

        if (!found) return 0;
    }

    // Mark this callback count as consumed
    last_cb_count_hwjpeg = cb_cnt;

    // Scan for EOI to determine frame size
    {
        unsigned int frame_size = find_jpeg_eoi(p, HW_MJPEG_VRAM_BUFSIZE);
        unsigned int fi;

        if (frame_size == 0 || frame_size > JPEG_BUF_SIZE) return 0;

        // Allocate copy buffer if needed
        if (!jpeg_buf[0]) {
            jpeg_buf[0] = malloc(JPEG_BUF_SIZE);
            if (!jpeg_buf[0]) return 0;
        }

        // Copy JPEG data (source may be overwritten by next JPCORE encode)
        for (fi = 0; fi < frame_size; fi++) {
            jpeg_buf[0][fi] = p[fi];
        }

        hw_jpeg_data = jpeg_buf[0];
        hw_jpeg_size = frame_size;
        frame_width = 640;
        frame_height = 480;
        frame_format = WEBCAM_FMT_JPEG;

        return (int)frame_size;
    }
}

// ============================================================
// H.264 frame capture from recording spy buffer (Option D)
// ============================================================

// Capture an H.264 encoded frame via semaphore-based delivery.
// movie_rec.c's spy_ring_write() copies frame data to frame_data_buf
// and signals frame_sem on every encoded frame.  We block on the
// semaphore (max 100ms) for synchronized delivery — no polling.
//
// The data is H.264 NAL units in AVCC format (4-byte big-endian
// length prefix, as used in MOV containers).
//
// Returns frame size on success, 0 if no frame available.
static int capture_frame_h264(void)
{
    volatile unsigned int *hdr = WEBCAM_SPY_ADDR;
    unsigned int size;
    int sem_ret;
    static int last_sem_ret = -99;   // TakeSemaphore result from previous call
    static unsigned int dbg_first8[2] = {0,0};  // First 8 bytes of frame data
    static unsigned int dbg_nal_len = 0;        // First NAL length from parser
    static unsigned int dbg_nal_hdr = 0;        // First NAL header byte
    static unsigned int dbg_parse_result = 0;   // Parser outcome bitfield

    // ---- Populate diagnostics for bridge visibility ----
    // hw_mjpeg_get_frame diagnostics are never called during recording,
    // so we must fill hw_diag here for the bridge to display.
    {
        #define D32(off, v) do { \
            hw_diag[(off)]   = (v);       hw_diag[(off)+1] = (v)>>8; \
            hw_diag[(off)+2] = (v)>>16;   hw_diag[(off)+3] = (v)>>24; \
        } while(0)

        volatile unsigned int *movtask = (volatile unsigned int *)0x000051A8;

        // Block 0 (0-63): H.264 capture state
        D32( 0, 0x48323634);            // marker "H264" so bridge knows this is H.264 diag
        D32( 4, recording_active);
        D32( 8, (unsigned int)frame_data_buf);
        D32(12, (unsigned int)frame_sem);
        D32(16, avi_sem_handle);
        D32(20, *(volatile unsigned int *)0x000051E4); // movie_status NOW
        D32(24, movtask[0x3C/4]);       // movie task STATE
        D32(28, movtask[0x50/4]);       // movie task frame counter
        // Recording start diagnostics from webcam_start
        D32(32, hdr[11]);              // rec_ret
        D32(36, hdr[12]);              // movie_status @ 0ms
        D32(40, hdr[13]);              // movie_status after retry loop
        D32(44, hdr[14]);              // retry count (0-49, 50=timed out)
        D32(48, (unsigned int)last_sem_ret);  // TakeSemaphore result from prev call
        // Current camera state
        D32(52, camera_info.state.mode_video);  // mode_video flag
        D32(56, camera_info.state.mode_play);   // mode_play flag
        D32(60, camera_info.state.mode_rec);    // mode_rec flag

        // Block 1 (64-127): Spy buffer state + movie task frame-skip fields
        D32(64, hdr[0]);               // spy magic (should be 0x52455753)
        D32(68, hdr[1]);               // spy data_ptr
        D32(72, hdr[2]);               // spy frame_size (written by spy_ring_write)
        D32(76, hdr[3]);               // spy frame_count (written by spy_ring_write)
        D32(80, hdr[4]);               // spy max_size
        D32(84, hdr[5]);               // spy sem_handle
        // Movie task fields related to frame skip logic in sub_FF85D98C_my
        // LDRH [R6,#2] reads halfword at byte +2 = upper 16 bits of word 0 (LE)
        D32(88, (movtask[0] >> 16) & 0xFFFF);  // movtask+0x02 halfword (frame skip mode)
        // LDRH [R6,#4] reads halfword at byte +4 = lower 16 bits of word 1 (LE)
        D32(92, movtask[1] & 0xFFFF);           // movtask+0x04 halfword (frame skip rate)
        D32(96, movtask[0x48/4]);      // movtask+0x48 (frame skip calc param)
        D32(100, movtask[0x2C/4]);     // movtask+0x2C (stop flag)
        D32(104, movtask[0x40/4]);     // movtask+0x40 (max frames)
        // AVCC parser debug (from previous call)
        D32(108, dbg_first8[0]);       // first 4 bytes of frame data
        D32(112, dbg_first8[1]);       // next 4 bytes of frame data
        D32(116, dbg_nal_len);         // first NAL length parsed
        D32(120, dbg_nal_hdr);         // first NAL header byte
        D32(124, dbg_parse_result);    // parser outcome bits

        #undef D32
        hw_diag_len = HW_DIAG_SIZE;
    }

    if (!recording_active || !frame_data_buf || !frame_sem) return 0;
    if (hdr[0] != 0x52455753) return 0;

    // Block until movie_rec.c signals a new frame (max 100ms).
    // spy_ring_write() does memcpy + GiveSemaphore(frame_sem).
    sem_ret = TakeSemaphore(frame_sem, 100);
    last_sem_ret = sem_ret;
    if (sem_ret != 0) return 0;  // Timeout — no frame available

    // Frame data is already in frame_data_buf (copied by spy_ring_write).
    size = hdr[2];  // Actual bytes copied

    // Capture first 8 bytes for diagnostics regardless of parser outcome
    if (frame_data_buf && size >= 8) {
        dbg_first8[0] = ((unsigned int)frame_data_buf[0] << 24) |
                        ((unsigned int)frame_data_buf[1] << 16) |
                        ((unsigned int)frame_data_buf[2] << 8) |
                        (unsigned int)frame_data_buf[3];
        dbg_first8[1] = ((unsigned int)frame_data_buf[4] << 24) |
                        ((unsigned int)frame_data_buf[5] << 16) |
                        ((unsigned int)frame_data_buf[6] << 8) |
                        (unsigned int)frame_data_buf[7];
    }

    if (size == 0 || size > SPY_BUF_SIZE) return 0;

    // Parse AVCC NAL units to determine actual H.264 frame size.
    // spy_ring_write copies min(ring_chunk_size, 64KB).  The actual
    // encoded frame (~5-40 KB) is a prefix of that data.
    //
    // AVCC format: [4-byte big-endian length][NAL data] per NAL unit.
    // Canon Baseline profile: one slice NAL per frame (type 1 or 5),
    // optionally preceded by SEI (type 6).
    {
        unsigned char *p = frame_data_buf;
        unsigned int pos = 0;
        unsigned int total = 0;
        int nal_count = 0;
        int have_vcl = 0;

        while (pos + 5 <= size && nal_count < 4) {
            unsigned int nal_len = ((unsigned int)p[pos] << 24) |
                                  ((unsigned int)p[pos+1] << 16) |
                                  ((unsigned int)p[pos+2] << 8) |
                                  (unsigned int)p[pos+3];
            unsigned int nal_type;

            // Capture first NAL debug info
            if (nal_count == 0) {
                dbg_nal_len = nal_len;
                dbg_nal_hdr = p[pos + 4];
            }

            if (nal_len < 2 || nal_len > 120000) {
                dbg_parse_result = 0x10 | nal_count;  // len out of range
                break;
            }

            nal_type = p[pos + 4] & 0x1F;
            if (p[pos + 4] & 0x80) {
                dbg_parse_result = 0x20 | nal_count;  // forbidden_zero_bit
                break;
            }

            if (nal_type != 1 && nal_type != 5 && nal_type != 6) {
                dbg_parse_result = 0x30 | nal_type;   // unexpected NAL type
                break;
            }

            total = pos + 4 + nal_len;
            pos = total;
            nal_count++;

            if (nal_type == 1 || nal_type == 5) {
                have_vcl = 1;
                dbg_parse_result = 0x100 | nal_count; // success
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
// Unified frame capture (H.264 > HW JPEG > raw UYVY > SW JPEG)
// ============================================================

// Capture a frame using the best available method.
// Returns frame size on success, 0 on error.
static int capture_frame(void)
{
    int size;

    // Priority 1: H.264 from recording spy buffer (Option D).
    // Smallest frames (~5-20 KB), highest potential FPS (30 fps).
    // Only available when recording is active.
    size = capture_frame_h264();
    if (size > 0) goto got_frame;

    // When recording is active, skip slow fallback paths (UYVY/SW JPEG)
    // which would block the PTP task and starve the recording pipeline.
    // Return 0 immediately — the bridge retries after 33ms.
    if (recording_active) return 0;

    // Priority 2: JPCORE hardware JPEG (Option B).
    // Small frames (~30-100 KB), high FPS (10-30 fps).
    // Available after quality fix, requires JPCORE producing data.
    size = capture_frame_hwjpeg();
    if (size > 0) goto got_frame;

    // Priority 3: Raw UYVY from video pipeline (zero encoding overhead).
    // Large frames (614 KB), ~5 FPS due to USB bandwidth.
    // Available once rec_callback_spy starts firing (~100ms after start).
    size = capture_frame_uyvy();
    if (size > 0) goto got_frame;

    // Priority 4: Software JPEG encoding (slowest fallback).
    // ~1.8 FPS, used only if all other paths fail.
    size = capture_and_compress_frame_sw();
    frame_format = WEBCAM_FMT_JPEG;

got_frame:
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
    // calling stop), force a clean shutdown first. Without this, the stale
    // recording_active/last_spy_cnt values prevent new frame detection.
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
    // The camera's ~3 minute inactivity timer would otherwise kill USB.
    disable_shutdown();

    // Reset hardware encoder state
    hw_mjpeg_active = 0;
    hw_mjpeg_available = 0;
    hw_jpeg_data = 0;
    hw_jpeg_size = 0;
    hw_frame_index = 0;

    // Reset raw UYVY state
    frame_format = WEBCAM_FMT_JPEG;
    last_cb_count_raw = 0;

    // Reset Option B/D state
    last_cb_count_hwjpeg = 0;
    recording_active = 0;
    last_spy_cnt = 0;
    avi_sem_handle = 0;

    // Switch camera to video mode
    if (switch_to_video_mode() < 0) {
        webcam_active = 0;
        return -2; // Mode switch failed
    }

    // Wait for video pipeline to stabilize before starting recording.
    msleep(500);

    // Skip hw_mjpeg_start() — calling StartMjpegMaking before recording
    // conflicts with the recording pipeline's own JPCORE setup and causes
    // the spy buffer to stop updating after 1-2 frames.

    // Allocate spy buffer data area and create frame delivery semaphore.
    // movie_rec.c's spy_ring_write will memcpy frame data here and signal
    // the semaphore, so capture_frame_h264 can block instead of polling.
    if (!frame_data_buf) {
        frame_data_buf = malloc(SPY_BUF_SIZE);
    }
    if (frame_data_buf && !frame_sem) {
        frame_sem = CreateBinarySemaphore("WebcamFrame", 0);
    }

    // Initialize shared spy buffer header (must be done BEFORE starting
    // recording so spy_ring_write sees valid state from the first frame).
    {
        volatile unsigned int *spy = WEBCAM_SPY_ADDR;
        int si;
        for (si = 0; si < 16; si++) spy[si] = 0;   // Clear all
        spy[1] = (unsigned int)frame_data_buf;       // Data buffer pointer
        spy[4] = SPY_BUF_SIZE;                       // Max buffer size
        spy[5] = (unsigned int)frame_sem;             // Semaphore handle
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

        {
            volatile unsigned int *spy = WEBCAM_SPY_ADDR;
            spy[11] = rec_ret;
            spy[12] = get_movie_status();  // immediately after call
        }

        if (rec_ret == 0) {
            // Wait up to 5 seconds for CtrlSrv to start recording
            for (rec_retries = 0; rec_retries < 50; rec_retries++) {
                msleep(100);
                if (get_movie_status() == VIDEO_RECORD_IN_PROGRESS) {
                    recording_active = 1;
                    break;
                }
            }

            // Store retry count and final movie_status for diagnostics
            {
                volatile unsigned int *spy = WEBCAM_SPY_ADDR;
                spy[13] = get_movie_status();
                spy[14] = (unsigned int)rec_retries;
            }

            // No GiveSemaphore needed — with stock movie_rec.c, the AVI
            // writer completes normally and signals its own semaphore.
            // The value at movtask[+0x14] is NOT a valid DryOS semaphore
            // handle; calling GiveSemaphore on it crashes the camera.
        }

        hw_diag_len = HW_DIAG_SIZE;
    }

    // Allocate JPEG double-buffers (needed for software fallback path
    // when recording is not active or fails to start).
    {
        int i;
        for (i = 0; i < NUM_JPEG_BUFS; i++) {
            if (!jpeg_buf[i]) {
                jpeg_buf[i] = malloc(JPEG_BUF_SIZE);
                if (!jpeg_buf[i]) {
                    int j;
                    for (j = 0; j < i; j++) {
                        free(jpeg_buf[j]);
                        jpeg_buf[j] = 0;
                    }
                    webcam_active = 0;
                    return -1;
                }
            }
            jpeg_buf_size[i] = 0;
        }
    }

    jpeg_buf_current = 0;
    jpeg_buf_ready = -1;
    frame_count = 0;
    last_frame_tick = 0;
    current_fps = 0;
    frame_width = 0;
    frame_height = 0;
    last_vp_checksum = 0;

    return 0;
}

static int webcam_stop(void)
{
    int i;

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

    // Stop recording if we started it (Option D)
    if (recording_active) {
        int stop_retries;
        call_func_ptr(FW_UIFS_StopMovieRecord, 0, 0);

        for (stop_retries = 0; stop_retries < 50; stop_retries++) {
            msleep(100);
            if (get_movie_status() != VIDEO_RECORD_IN_PROGRESS) break;
        }
        recording_active = 0;
    }

    // Stop hardware encoder if running
    hw_mjpeg_stop();
    hw_mjpeg_available = 0;

    // Reset Option B/D state
    last_cb_count_hwjpeg = 0;
    last_spy_cnt = 0;
    avi_sem_handle = 0;

    // Clean up spy buffer resources (semaphore + data buffer)
    if (frame_sem) {
        DeleteSemaphore(frame_sem);
        frame_sem = 0;
    }
    if (frame_data_buf) {
        free(frame_data_buf);
        frame_data_buf = NULL;
    }

    // Free raw UYVY buffer
    if (uyvy_buf) {
        free(uyvy_buf);
        uyvy_buf = 0;
    }

    // Free software encoder buffers
    for (i = 0; i < NUM_JPEG_BUFS; i++) {
        if (jpeg_buf[i]) {
            free(jpeg_buf[i]);
            jpeg_buf[i] = 0;
        }
        jpeg_buf_size[i] = 0;
    }

    jpeg_buf_current = 0;
    jpeg_buf_ready = -1;
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

    // Capture a new frame
    size = capture_frame();

    if (size <= 0) {
        return -1;
    }

    // H.264 path (Option D: from recording spy buffer)
    if (frame_format == WEBCAM_FMT_H264 && hw_jpeg_data && hw_jpeg_size > 0) {
        frame->data = hw_jpeg_data;
        frame->size = hw_jpeg_size;
        frame->width = frame_width;
        frame->height = frame_height;
        frame->frame_num = frame_count;
        frame->format = WEBCAM_FMT_H264;
        return 0;
    }

    // JPCORE hardware JPEG path (Option B) or spy buffer JPEG
    if (frame_format == WEBCAM_FMT_JPEG && hw_jpeg_data && hw_jpeg_size > 0) {
        frame->data = hw_jpeg_data;
        frame->size = hw_jpeg_size;
        frame->width = frame_width;
        frame->height = frame_height;
        frame->frame_num = frame_count;
        frame->format = WEBCAM_FMT_JPEG;
        return 0;
    }

    // Raw UYVY path
    if (frame_format == WEBCAM_FMT_UYVY && uyvy_buf) {
        frame->data = uyvy_buf;
        frame->size = UYVY_BUF_SIZE;
        frame->width = frame_width;
        frame->height = frame_height;
        frame->frame_num = frame_count;
        frame->format = WEBCAM_FMT_UYVY;
        return 0;
    }

    // Software JPEG fallback
    if (jpeg_buf_ready < 0) {
        return -1;
    }

    frame->data = jpeg_buf[jpeg_buf_ready];
    frame->size = jpeg_buf_size[jpeg_buf_ready];
    frame->width = frame_width;
    frame->height = frame_height;
    frame->frame_num = frame_count;
    frame->format = WEBCAM_FMT_JPEG;

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

    if (hw_mjpeg_available && hw_jpeg_size > 0) {
        status->frame_size = hw_jpeg_size;
    } else {
        status->frame_size = (jpeg_buf_ready >= 0) ? jpeg_buf_size[jpeg_buf_ready] : 0;
    }
    status->hw_fail_call = hw_fail_call;
    status->hw_fail_soi = hw_fail_soi;
    status->hw_fail_eoi = hw_fail_eoi;
    status->hw_available = hw_mjpeg_available;
    status->diag_data = (hw_diag_len > 0) ? hw_diag : 0;
    status->diag_len = hw_diag_len;
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
