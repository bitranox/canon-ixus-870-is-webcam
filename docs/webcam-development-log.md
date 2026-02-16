# Webcam Development Log

> Back to [CLAUDE.md](../CLAUDE.md)

## Native Webcam Project (Phase 1: MJPEG Streaming)

Goal: Turn the IXUS 870 IS into a native webcam using CHDK's video mode buffer (640x480) + MJPEG compression + PTP streaming + PC-side virtual webcam bridge. Camera appears as "CHDK Webcam" in Zoom/Teams/OBS.

## v11 Series — Pipeline Callback Discovery & Software Path Working (2026-02-11)

**Custom pipeline callback (`rec_callback_spy`)**: Installed at `state[+0x114]`, fires at 30fps from the video pipeline. The callback receives 4 arguments:
- `arg0` = 0 (always)
- `arg1` = our callback address (self-reference)
- `arg2` = **640x480 YUV frame buffer address** (rotates through 3 uncached buffers)
- `arg3` = 1 (always)

**arg2 triple-buffer rotation** (uncached RAM mirror addresses):

| Buffer | Uncached Address | Cached Mirror | Spacing |
|--------|-----------------|---------------|---------|
| 0 | `0x40BAADD0` | `0x00BAADD0` | — |
| 1 | `0x40C7DCD0` | `0x00C7DCD0` | +0xD2F00 (863 KB) |
| 2 | `0x40D50BD0` | `0x00D50BD0` | +0xD2F00 (863 KB) |

These are the **full 640x480 video pipeline output frames** — the same data that JPCORE would encode during movie recording. Format is likely **UYVY (YUV422)**: byte pattern analysis shows gradual Y-value transitions consistent with UYVY interleaved format. For 640x480 UYVY: 640×480×2 = 614,400 bytes per frame, fits within the 863 KB buffer spacing.

**v11 iteration summary**:

| Version | Change | Result |
|---------|--------|--------|
| v11c | ISR callback reads uncached buffers + I/O regs | Camera hang on first get_frame (heavy ISR reads) |
| v11c-fix | ISR reads cached-only | Still hangs (GetContinuousMovieJpegVRAMData blocks forever) |
| v11d | Removed GetContinuousMovieJpegVRAMData, scan VRAM directly | 622 frames, then hang (64KB uncached scans per frame) |
| v11e | Removed 64KB uncached buffer scans | **2 frames displayed!** Then pipeline killed by hw_mjpeg_stop() |
| v11f | Don't call hw_mjpeg_stop() on HW fallback | **56 sec at 1.9 FPS**, then stopped (stale frame detection) |
| v11g | Removed stale frame detection | **172 sec at 1.9→2.5 FPS**, stopped only by camera auto-power-off |

**Critical lessons learned**:
1. **GetContinuousMovieJpegVRAMData blocks forever** — DryOS event flag wait timeout=0 means infinite wait when recording pipeline isn't fully set up. NEVER call this function.
2. **Uncached RAM reads (0x40xxxxxx) cause cumulative bus stalls** — reading >64 KB per frame from uncached DMA buffers on ARM926EJ-S causes camera hangs after ~600 frames. Keep uncached reads minimal (<8 KB per frame max).
3. **hw_mjpeg_stop() kills the video pipeline** — it restores state[+0xD4]=1 and calls StopMjpegMaking, which stops the LCD and viewport updates. DO NOT call during fallback — only on webcam_stop().
4. **Software JPEG path works at 1.9-2.5 FPS** — encoding the 720x240 viewport buffer with tje.c at quality 50. Frame sizes ~16.8 KB. Camera stable for 3+ minutes (limited by auto-power-off, not software).
5. **Stale frame detection was killing the stream** — the 4-byte checksum approach would stop sending frames when the viewport froze momentarily. Removed entirely.

## v12 Series — 640x480 Video Pipeline Streaming Working (2026-02-11)

**v12: 640x480 UYVY software encoding from video pipeline**

Added `tje_encode_uyvy()` to the TJE encoder to handle the UYVY (YUV422) format from the video pipeline. The `capture_and_compress_frame_sw()` function now reads 640x480 frames from `rec_cb_arg2` (pipeline callback buffer) first, with fallback to the 720x240 viewport.

**Key findings**:

1. **Video pipeline format is UYVY with SIGNED chroma**: The Digic IV ISP output uses signed bytes (-128..+127, centered at 0) for chroma (U/V), NOT unsigned (0..255, centered at 128). Same convention as the viewport's UYVYYY format. Using `(int)(signed char)row[offset]` for chroma extraction — `(int)row[offset] - 128` produces a green color cast because neutral chroma (0x00) becomes -128 instead of 0.

2. **Color symptom explained**: White appeared green because unsigned interpretation of 0x00 chroma gives `0 - 128 = -128`, pushing both Cb and Cr strongly negative. In YCbCr, low Cb + low Cr = green. The signed interpretation correctly reads 0x00 as 0 (neutral).

3. **Camera auto-power-off prevention**: Added `disable_shutdown()` on webcam start and `enable_shutdown()` on webcam stop. Required adding these functions to `modules/module_exportlist.c` (they were core CHDK functions but not exported to modules). This prevents the ~3-minute inactivity timer from killing the USB connection.

4. **Frame size reduction confirms color fix**: With correct signed chroma, frame sizes dropped from ~48 KB to ~26 KB at quality 50. Neutral chroma values compress much better than the saturated extremes produced by the unsigned interpretation.

**v12 iteration summary**:

| Version | Change | Result |
|---------|--------|--------|
| v12a | UYVY encoder + rec_cb_arg2 reader (unsigned chroma) | 640x480 at 1.2→1.5 FPS, ~48 KB/frame, green color cast, 3 min limit |
| v12b | Signed chroma fix + disable_shutdown() | **640x480 at 1.3 FPS, ~26 KB/frame, correct colors, 10+ min stable** |

**Current working configuration** (v12b):
- **Resolution**: 640x480 from ISP video pipeline (rec_cb_arg2)
- **Format**: UYVY (YUV422) with signed chroma bytes
- **Encoding**: Software JPEG via tje.c on ARM926EJ-S @ ~200MHz
- **FPS**: 1.3 FPS sustained
- **Frame size**: ~26 KB at quality 50
- **Stability**: Unlimited streaming (disable_shutdown prevents auto-power-off)
- **Zero dropped frames** during normal operation
- **Cached mirror access**: Reading from `cb_addr & 0x0FFFFFFF` (cached RAM) instead of uncached `0x40xxxxxx` DMA mirror for performance

**Files modified in v12**:
- `chdk/modules/tje.c` — Added `extract_y_block_uyvy()`, `extract_chroma_block_uyvy()`, `tje_encode_uyvy()` with signed chroma
- `chdk/modules/tje.h` — Added `tje_encode_uyvy()` declaration
- `chdk/modules/webcam.c` — 640x480 rec_cb_arg2 path in `capture_and_compress_frame_sw()`, `disable_shutdown()`/`enable_shutdown()`
- `chdk/modules/module_exportlist.c` — Exported `disable_shutdown` and `enable_shutdown` to modules

## H.264 Spy Buffer Approach — Option D (2026-02-11 to 2026-02-15)

**Concept**: Instead of trying to activate JPCORE independently, use the camera's own movie recording pipeline. Start actual video recording via `UIFS_StartMovieRecord`, then intercept the H.264 encoded frames from `sub_FF85D98C_my` in `movie_rec.c` via a spy buffer at RAM `0x000FF000`.

**Spy buffer protocol** (written by movie_rec.c, read by webcam module):

| Offset | Field | Value |
|--------|-------|-------|
| spy[0] | magic | `0x52455753` ("SREW") after first frame |
| spy[1] | jpeg_ptr | H.264 data pointer from sub_FF92FE8C |
| spy[2] | jpeg_size | H.264 data size (ring buffer chunk, ~256 KB) |
| spy[3] | frame_cnt | Incremented each successful frame (written LAST) |
| spy[4] | init_flag | `0xCAFE0001` after init case runs |
| spy[5] | err_code | Last sub_FF92FE8C error code |
| spy[6] | err_cnt | Error count |
| spy[7-10] | metadata | Frame metadata, task state, callback addr |

**H.264 frame format**: Canon outputs AVCC format (4-byte big-endian NAL length prefix). NAL types: 1=P-frame (0x61), 5=IDR keyframe (0x65), 6=SEI. IDR interval ~8 frames. SPS/PPS are NOT in the spy buffer — they're only in the MOV container metadata. The PC bridge has hardcoded SPS/PPS extracted from a real MOV file.

**What works**:
- Recording starts (movie_status=4)
- Spy buffer produces valid H.264 frames (~35-40 KB each, valid AVCC)
- PC bridge receives and decodes them via FFmpeg (confirmed up to 22 consecutive frames in one session)
- `capture_frame_h264()` reads spy buffer non-blocking (check once, return immediately)

**Current state (2026-02-16)**: `spy_ring_write()` in movie_rec.c copies frame data to webcam module's buffer and signals semaphore for synchronous delivery. Recording starts via `UIFS_StartMovieRecord` with a retry loop waiting for `movie_status == VIDEO_RECORD_IN_PROGRESS`.

**Recording start method**: `UIFS_StartMovieRecord` (0xFF883D50) posts event 0x9A1 to CtrlSrv which processes it asynchronously. **Takes ~1 second** to reach movie_status=4 (VIDEO_RECORD_IN_PROGRESS). A retry loop (up to 50 × 100ms = 5 seconds) is required — the old "fire-and-forget" pattern with only 200ms wait was insufficient and left movie_status at 0.

**Key discoveries (2026-02-16)**:
1. **Recording needs ~1 second to start**: UIFS_StartMovieRecord returns 0 immediately, but movie_status reaches 4 only after ~10 retries (1 second). Without the retry loop, the webcam module sees movie_status=0 and never activates frame capture.
2. **movtask[+0x14] (0x51BC) is NOT a valid DryOS semaphore handle**: The value at this address (~0x04EDBxxx / ~0x04F4xxxx) is NOT a semaphore. Calling GiveSemaphore on it crashes the camera within 9 frames. **DO NOT call GiveSemaphore on this value.**
3. **AVI writer signals its own semaphore**: With stock recording flow (no NOP patches), the AVI writer completes normally and signals its own completion semaphore. No external GiveSemaphore keepalive is needed.
4. **spy_ring_write runs safely before AVI write**: The BL call is placed after sub_FF92FE8C returns the frame but before the AVI write path. ARM AAPCS guarantees R4-R11 are preserved across the BL. The memcpy (~320us for 64KB on ARM926) + GiveSemaphore (~1us) complete well within the frame period.
5. **Previous recording deaths were caused by spy buffer patches themselves**: With completely stock movie_rec.c, recording stays alive indefinitely. The NOP/skip-all-AVI approaches were trying to fix a problem created by the old spy buffer hooks.

**Previous failed approach — NOP AVI write + TakeSemaphore**: All 3 `sub_FF8EDBE0` (AVI write) calls and all 3 `sub_FF8274B4` (TakeSemaphore) calls were NOPped. Recording still died because the spy buffer hooks disrupted the pipeline flow. This approach has been abandoned.

**Failed approaches to keep recording alive**:

| Approach | Result |
|----------|--------|
| Spy wait loop (200ms msleep in capture_frame_h264) | Only 2 frames, then camera stops responding |
| Increased bridge polling interval (33ms sleep) | 5 frames, still stops |
| GiveSemaphore keepalive (100 pre-signals) | Inconsistent: 20+ frames in one session, ~1s in others |
| GiveSemaphore keepalive (10 pre-signals) | Recording dies immediately (not enough runway) |
| GiveSemaphore on movtask[+0x14] (0x51BC) | **Camera crashes after 9 frames** — NOT a valid semaphore handle |
| Re-enable `hw_mjpeg_start()` before recording | Recording dies after ~1s (conflicts with pipeline JPCORE setup) |
| Force AVI write success (call sub_FF8EDBE0 then MOV R0,#0 + STR R0,[SP,#0x38]) | Camera still shuts off — sub_FF8EDBE0 itself may block/stall |
| Force TakeSemaphore success (call sub_FF8274B4 then MOV R0,#0 before CMP R0,#9) | Combined with above, still dies — blocking I/O not eliminated |
| `kbd_key_press(KEY_VIDEO)` instead of UIFS_StartMovieRecord | **Recording never starts** (movie_status=0). IXUS 870 has no physical VIDEO button — KEY_VIDEO (18) is NOT in the platform keymap |
| `PostLogicalEventToUI("PressMovieButton")` | Returns valid event ID 0x9A6 but is **ignored in PTP/USB mode** — movie_status stays 0 |
| Skip AVI write entirely (`B loc_FF85DCBC` after spy write) | Camera shuts off — skipped critical register/state setup |
| Zero JPEG size to skip AVI write naturally (`STR #0, [SP, #0x30]`) | Camera shuts off — same issue, critical state not maintained |
| NOP AVI write + TakeSemaphore (3 sites) | Shows 23'06'' and stops immediately (AVI bookkeeping without preceding write) |
| Skip ALL AVI code (`B loc_FF85DB10` after spy) | Shows 0'' and stops immediately (no frame counter progress) |
| `kbd_key_press(KEY_SHOOT_FULL)` | Doesn't start recording in video mode via PTP |
| Fire-and-forget (200ms wait, no retry) | movie_status=0 — UIFS_StartMovieRecord needs ~1s to complete |
| **Stock movie_rec.c + retry loop (no GiveSemaphore)** | **WORKS — recording stays alive for full 30s test** |

**Important operational notes**:
- **`hw_mjpeg_start()` must NOT be called before recording**: It conflicts with the recording pipeline's own JPCORE setup and causes frames to stop after 1-2 seconds. The committed version correctly skips it.
- **movie_rec.c has spy_ring_write()**: Copies frame data + signals semaphore BEFORE the AVI write path. All standard AVI write + TakeSemaphore calls remain intact.
- **UIFS_StartMovieRecord + retry loop**: Call returns immediately, then wait up to 5 seconds (50 × 100ms) for movie_status to reach VIDEO_RECORD_IN_PROGRESS (~10 retries = 1 second typical).
- **No GiveSemaphore on AVI handle**: movtask[+0x14] is NOT a valid semaphore. The AVI writer signals its own semaphore.

**Spy buffer protocol (v2 — data-copying + semaphore)**:

The original spy buffer only stored a POINTER to Canon's H.264 ring buffer. The v2 protocol copies actual frame data and uses a semaphore for synchronization:

| Offset | Field | Writer | Description |
|--------|-------|--------|-------------|
| hdr[0] | magic | webcam.c | `0x52455753` = active, `0` = disabled |
| hdr[1] | data_ptr | webcam.c | Pointer to malloc'd frame buffer (64KB) |
| hdr[2] | frame_size | movie_rec.c | Actual bytes copied by spy_ring_write |
| hdr[3] | frame_cnt | movie_rec.c | Monotonic counter (written LAST) |
| hdr[4] | max_size | webcam.c | Buffer capacity (65536) |
| hdr[5] | sem_handle | webcam.c | DryOS binary semaphore for signaling |
| hdr[11] | rec_ret | webcam.c | UIFS_StartMovieRecord return value |
| hdr[12] | status_imm | webcam.c | movie_status immediately after call |
| hdr[13] | status_final | webcam.c | movie_status after retry loop |
| hdr[14] | retry_cnt | webcam.c | Number of retries before recording started |

**Files involved**:
- `chdk/platform/ixus870_sd880/sub/101a/movie_rec.c` — `spy_ring_write()` + BL call in sub_FF85D98C_my
- `chdk/modules/webcam.c` — spy buffer init, semaphore creation, retry loop, `capture_frame_h264()` with TakeSemaphore
- `bridge/src/webcam/h264_decoder.cpp` — FFmpeg AVCC-to-Annex-B converter + decoder
- `bridge/src/webcam/h264_decoder.h` — H.264 decoder header

### First Test — spy_ring_write + Semaphore Delivery (2026-02-16)

**Build**: commit `c7dc45f` (spy_ring_write + semaphore frame delivery + retry loop)

**Test**: `chdk-webcam.exe --timeout 20 --no-preview --no-webcam`

**Result**: Camera started recording (user confirmed visually). **0 frames received by bridge** in 20 seconds. All frame polls returned `gf_rc=-1`.

**Decoded H.264 diagnostics** (capture_frame_h264 populates Block 0 in H.264 format, but bridge displays it with MJPEG state labels — decode table):

| Bridge Display (MJPEG label) | H.264 Diagnostic Field | Value | Meaning |
|------------------------------|----------------------|-------|---------|
| +0x48 MJPEG active = 1211250228 | D32(0) H264 marker | 0x48323634 ("H264") | Format identifier |
| +0x4C paired flag = 1 | D32(4) recording_active | 1 | **Recording active in webcam module** |
| +0x54 DMA status = 2205816 | D32(8) frame_data_buf | ~0x21A1E8 | Buffer allocated |
| +0x58 DMA frame idx = 82969244 | D32(12) frame_sem | ~0x04F1A9AC | Semaphore created |
| +0x5C DMA req state = 0 | D32(16) avi_sem_handle | 0 | No AVI semaphore |
| +0x60 ring buf addr = 4 | D32(20) movie_status | **4** | **VIDEO_RECORD_IN_PROGRESS** |
| +0x64 VRAM buf addr = 4 | D32(24) movie task STATE | **4** | Task state = running |
| +0x6C rec buffer = 3→4→6→7→8... | D32(28) movtask[+0x50] frame counter | **Incrementing** | **Frames being processed** |
| +0x80 cleanup cb = 0 | D32(32) rec_ret | 0 | UIFS_StartMovieRecord returned 0 (success) |
| +0xA0 DMA callback = 0 | D32(36) status_imm | 0 | movie_status=0 immediately after call |
| +0xB0 event flag = 4 | D32(40) status_final | 4 | movie_status=4 after retry loop |
| +0xD4 video mode = 10 | D32(44) retry_cnt | **10** | ~1 second to start recording |
| +0xF0 frame skip = 1 | D32(52) mode_video | 1 | Camera in video mode |
| +0x118 rec callback 2 = 1 | D32(60) mode_rec | 1 | Camera in rec mode |

**Initial diagnostics** (captured right after start_webcam returns, before recording completes async startup): ALL ZEROS — movie_status=0, all spy fields zero, all pipeline state zero. This is expected since UIFS_StartMovieRecord takes ~1 second to complete.

**Key findings**:
1. **Recording started successfully**: movie_status=4, movie task STATE=4, UIFS_StartMovieRecord returned 0, took 10 retries (~1 second)
2. **Movie task frame counter IS incrementing** (3→4→6→7→8...): sub_FF85D98C_my IS being called and frames ARE being processed by the firmware
3. **0 frames delivered via spy buffer**: TakeSemaphore always times out (or capture_frame_h264 finds no valid data)
4. **Blocks 1-8 all zeros in per-frame diagnostics**: capture_frame_h264() only populates Block 0 — the spy buffer state at 0x000FF000 is NOT visible in per-frame diagnostics
5. **Bridge diagnostic format mismatch**: The bridge displays Block 0 with MJPEG state labels, making the H.264 data appear as nonsensical values (e.g., "MJPEG active = 1211250228" is actually the H264 format marker)

**Root cause analysis — why spy_ring_write() produces no frames**:

The movie task frame counter increments at `loc_FF85DCBC` in sub_FF85D98C_my. This location is reached via TWO paths:

1. **Normal path**: sub_FF92FE8C returns 0 → spy_ring_write called → loc_FF85DA24 → loc_FF85DB24 → check jpeg_size → loc_FF85DCBC
2. **Frame skip path**: Frame rate check at movtask[+0x02] decides to skip → jumps to loc_FF85DA24 (bypassing sub_FF92FE8C entirely) → loc_FF85DB24 → jpeg_size=0 (stack init) → loc_FF85DCBC

Without spy buffer diagnostics in the per-frame output, we cannot distinguish between:
- **Scenario A**: sub_FF92FE8C returns 0 but jpeg_size=0 → spy_ring_write copies 0 bytes → capture_frame_h264 sees size=0, returns 0
- **Scenario B**: Frame skip logic bypasses sub_FF92FE8C entirely → spy_ring_write never called → TakeSemaphore times out
- **Scenario C**: spy buffer magic at 0x000FF000 is overwritten by recording ring buffer → spy_ring_write returns early

**Next step**: Add spy buffer fields (hdr[0] magic, hdr[2] frame_size, hdr[3] frame_count, TakeSemaphore return value) to the H.264 diagnostics in capture_frame_h264(). This will reveal whether spy_ring_write is being called and what data it's writing.

## Future Ideas (Not Yet Implemented)

### Raw YUV Pipeline Streaming (640x480)

Stream raw 640x480 YUV frames from the video pipeline directly to the PC, bypassing on-camera JPEG encoding entirely. This could dramatically increase FPS since the ARM926 CPU would only need to memcpy the frame data, not encode it.

**Approach**:
- Camera: capture `rec_cb_arg2` buffer (640x480 UYVY, ~614 KB/frame) from pipeline callback, send raw bytes over PTP
- PC bridge: receive raw YUV data, encode to JPEG using libjpeg-turbo (PC encodes ~1000x faster than ARM926)

**Bandwidth analysis**: 614 KB × 5 fps = 3 MB/s. USB 2.0 High Speed theoretical max ~40 MB/s. Should be feasible.

**Pros**: No encoding overhead on camera, potentially 5-15+ fps (limited only by USB transfer speed + memcpy)
**Cons**: ~614 KB per frame vs ~26 KB for JPEG (24x more USB bandwidth), needs bridge decoder changes

### JPCORE Hardware Encoder — Status: BLOCKED

JPCORE hardware is confirmed initialized and processing frames (PS3 mask=6, piVar1[3]=1, HW registers changing). But the encoded JPEG output is DISCARDED because state[+0x114] was set to our spy callback instead of a real frame delivery callback. The original movie_record_task callbacks CRASH the camera because they need full AVI recording context. A custom lightweight callback that captures JPCORE output without crashing is the missing piece — but the JPCORE output format and delivery mechanism from FUN_ff8c335c are not yet fully understood.
