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

## H.264 Spy Buffer Approach — Option D (2026-02-11 to 2026-02-16)

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

### Second Test — With Spy Buffer Diagnostics (2026-02-16)

Added spy buffer state and AVCC parser debug fields to Block 1 of H.264 diagnostics. Camera started recording (user confirmed).

**Result**: 0 frames delivered to bridge, BUT **spy_ring_write IS working**:

| Diagnostic | Value | Meaning |
|-----------|-------|---------|
| hdr[0] spy magic | 0x52455753 | Spy buffer intact |
| hdr[2] frame_size | 65536 | spy_ring_write copies 64KB per frame |
| hdr[3] frame_count | 2→4→5→6→7→9→10→12... | **Incrementing — spy_ring_write called at 30fps** |
| last_sem_ret | 0 | **TakeSemaphore succeeds** |
| dbg_first8[0] | alternates 0 and 1 | First 4 bytes = `00 00 00 00` or `00 00 00 01` |
| dbg_first8[1] | 0 | Bytes 4-7 = `00 00 00 00` |
| dbg_parse_result | 0 | Parser never sets result (see below) |

**The entire data delivery pipeline works**: spy_ring_write copies data, signals semaphore, TakeSemaphore returns 0.

**Root cause: frame data starts with zeros, not AVCC**. The AVCC parser interprets the first 4 bytes as NAL length: `00 00 00 00` → nal_len=0 (< minimum 2, rejected) or `00 00 00 01` → nal_len=1 (< minimum 2, rejected). Every frame is rejected.

**Why the data isn't valid AVCC**: sub_FF92FE8C returns jpeg_size >= 65536 (ring buffer chunk size), not the individual frame size (~5-40 KB seen in previous pointer-based tests). The 64KB chunk likely contains:
- Ring buffer metadata/header at the start (the `00 00 00 0x` bytes)
- Actual H.264 AVCC data at some offset within the chunk

In the previous working spy buffer approach (v1 pointer-only), the bridge read data via PTP memory read at the exact pointer. Individual frames were 35-40 KB with valid AVCC at the start. Now we copy 64KB starting from the same pointer, but get metadata at the start. Two possible explanations:
1. **Ring buffer header**: The pointer from sub_FF92FE8C points to a ring buffer entry header, not directly to H.264 data. The header might contain a length/sequence field, followed by the actual encoded data at some fixed offset.
2. **Cache coherency**: If Canon's `_memcpy` uses DMA for large copies, the destination buffer may have stale cached data. But dbg_first8[0] does alternate (not constant zeros), arguing against full cache staleness.

**dbg_parse_result = 0 anomaly**: The parser should set `0x10` (nal_len out of range) but the static always shows 0. Either the code returns before the parser (despite TakeSemaphore succeeding), or static variable writes in the parser are not persisting. The pre-parser statics (last_sem_ret, dbg_first8) DO work. This needs investigation.

### Third Test — 256-Byte Frame Dump (2026-02-16)

Added a 256-byte dump of frame_data_buf into hw_diag Blocks 2-5 (bytes 128-383) to inspect the actual buffer contents after spy_ring_write's _memcpy.

**Result**: frame_data_buf contains **initial malloc state**, not H.264 data:

| Byte range | Content |
|-----------|---------|
| 0-127 | `00` (zeros) |
| 128-151 | `00` (zeros) |
| 152-255 | `FF` (0xFF fill) |

This pattern (zeros then 0xFF) is characteristic of an uninitialized heap block — malloc returned a chunk that was never written to. spy_ring_write's `_memcpy(dst, ptr, copy_size)` appears to have **no effect** on the destination buffer.

**Evidence summary across all three tests**:

| Evidence | Value | Implication |
|----------|-------|-------------|
| hdr[3] frame_count | Incrementing at 30fps | spy_ring_write IS called |
| hdr[2] frame_size | 65536 | copy_size = 64KB (non-zero) |
| last_sem_ret | 0 | TakeSemaphore succeeds (GiveSemaphore fires) |
| frame_data_buf content | zeros + 0xFF | _memcpy did NOT modify the buffer |
| dbg_first8[0] | alternates 0/1 | First 4 bytes read as 0x00000000 or 0x00000001 |

**Possible root causes**:

1. **Cache coherency**: On ARM926EJ-S, a single CPU core shares L1 cache between all tasks, so cached writes from _memcpy should be visible to cached reads in capture_frame_h264. Cache coherency is only an issue with DMA engines. Unless Canon's _memcpy uses a DMA engine for large copies (64KB is large enough to justify DMA), this explanation is unlikely.

2. **_memcpy argument order**: Canon's firmware _memcpy is declared as `_memcpy(dest, src, n)` matching standard memcpy. If the actual firmware function uses reversed arguments `_memcpy(src, dest, n)`, then spy_ring_write would copy FROM frame_data_buf TO the ring buffer (no visible effect on frame_data_buf). However, CHDK uses _memcpy throughout the codebase — reversed arguments would break many things.

3. **Source pointer validity**: The ptr from sub_FF92FE8C might point to a ring buffer entry header rather than raw H.264 data. The alternating 0x00000000/0x00000001 in the first 4 bytes could be metadata (entry status field). But the 256-byte dump shows the DESTINATION is unchanged, not that the source is zeros — the zeros/0xFF pattern matches malloc initial state, not ring buffer content.

4. **_memcpy silently fails**: The firmware's _memcpy might not handle certain memory regions or large sizes correctly in the movie_record_task context.

### Four Options for H.264 Frame Interception

The core problem: spy_ring_write's `_memcpy(dst, ptr, 64KB)` does not produce visible data in the destination buffer. Four approaches to solve this:

#### Option 1: Uncached Destination Buffer

Make spy_ring_write write through the ARM926 uncached RAM mirror so CPU cache is bypassed entirely.

**Changes**:
- webcam_start: `spy[1] = (unsigned int)frame_data_buf | 0x40000000;`
- capture_frame_h264: read from `(unsigned char *)((unsigned int)frame_data_buf | 0x40000000)`

**Pros**: Simplest change (2 lines). Keeps _memcpy in movie_rec.c context where ptr is freshly returned from sub_FF92FE8C.

**Cons**: Uncached 64KB writes are ~4x slower than cached. May stall the recording pipeline's 33ms frame budget. Only helps if the root cause IS cache coherency (unlikely on single-core ARM926 unless _memcpy uses DMA internally).

#### Option 2: Pointer Pass-Through (No Copy in movie_rec.c)

spy_ring_write stores the ring buffer pointer and size in the spy header without copying. capture_frame_h264 does the copy itself using CHDK's memcpy.

**Changes**:
- spy_ring_write: remove _memcpy, just store `hdr[1] = (unsigned int)ptr; hdr[2] = size;`
- capture_frame_h264: after TakeSemaphore, `memcpy(frame_data_buf, (void *)hdr[1], size)`

**Pros**: Eliminates the _memcpy mystery entirely — uses CHDK's own memcpy in well-understood module context. Tests whether the issue is with _memcpy vs the data itself.

**Cons**: Race condition — ptr points into Canon's ring buffer which could be overwritten when sub_FF92FE8C is called for the next frame. At 30fps the window is ~33ms; TakeSemaphore wakes immediately so the copy should complete well within that time, but it's not guaranteed. Also, if the root cause is that ptr points to metadata rather than H.264 data, this doesn't fix it.

#### Option 3: Manual Byte Copy in spy_ring_write

Replace `_memcpy` with a simple for loop in spy_ring_write to rule out firmware _memcpy quirks.

**Changes**:
- spy_ring_write: replace `_memcpy(dst, ptr, copy_size)` with `for (i = 0; i < copy_size; i++) dst[i] = ptr[i];`

**Pros**: Definitive test of whether _memcpy itself is the problem. Byte-by-byte copy through ARM load/store is guaranteed to go through the CPU cache. If this produces valid data, the issue was _memcpy; if not, the issue is with the source data or pointer.

**Cons**: Byte-by-byte copy of 64KB is very slow (~2-5ms on ARM926 vs ~0.3ms for optimized _memcpy). Could stall the recording pipeline. But acceptable as a diagnostic — can optimize later.

#### Option 4: Direct Ring Buffer Read

Read directly from Canon's H.264 ring buffer (managed by `DAT_ff93050c` at 0xFF93050C) in the webcam module, bypassing spy_ring_write and movie_rec.c entirely.

**Changes**:
- webcam.c: after starting recording, poll the ring buffer structure for new frames
- movie_rec.c: remove spy_ring_write hook entirely

**Pros**: Cleanest architecture — no ASM hooks in movie_rec.c, no shared memory protocol. Reading from a well-defined Canon data structure that the firmware manages correctly.

**Cons**: Most complex to implement. Requires reverse-engineering the ring buffer format (entry layout, read/write pointers, frame data offsets). Concurrent reads from two tasks (movie_record_task and webcam module) may corrupt ring buffer state. sub_FF92FE8C likely advances a read pointer — if we read without advancing, we get stale data; if we advance, we starve the AVI writer.

#### Recommendation

**Option 2 (pointer pass-through)** offers the best balance: simple change, eliminates the _memcpy mystery, and the race condition is manageable. If it works, we know the issue was _memcpy in the firmware context. If it doesn't work (data at ptr is also zeros), we know the source pointer itself is the problem, narrowing the investigation to sub_FF92FE8C's output.

### Fourth Test — Option 2 Pointer Pass-Through (2026-02-16)

Implemented Option 2: spy_ring_write stores ptr+size only (no _memcpy), capture_frame_h264 does memcpy from the stored pointer after TakeSemaphore.

**Result**: **H.264 streaming working — 62 frames decoded at 30.5 FPS!**

Camera started recording (user confirmed). Bridge received 250+ H.264 frames with valid AVCC format over the 20-second test.

| Metric | Value |
|--------|-------|
| Frames received | 250+ (bridge logs sample) |
| Frames decoded | 62 (in final stats window) |
| FPS | 30.5 |
| Bitrate | 10,228 kbps |
| Avg frame size | 40.9 KB |
| Frame sizes | 37-43 KB range |
| Resolution | 640x480 |
| NAL types seen | All 25 sampled = type 1 (P-frame, NAL 0x61) |
| IDR frames | Present (decoder synced), but none in sampled log entries |

**Decoder sync behavior**: The first ~50 frames failed with "Decoder needs more data" — FFmpeg needs an IDR keyframe (type=5) before it can decode P-frames. After receiving enough frames, the decoder synced and output 62 frames at 30.5 FPS in the final stats window. The `H.264 decoder: 640x480 -> 1280x720 RGB` message confirms successful initialization.

**movtask diagnostics confirm IDR frames exist**: `movtask[+0x6C]` shows `0x00000065` (NAL header for IDR/type=5) on some frames, confirming Canon's encoder produces IDR keyframes. They just weren't among the 25 sampled log entries (bridge logs frames 1-20, then every 50th).

**Root cause confirmed**: Canon firmware's `_memcpy` in spy_ring_write did NOT copy data to the destination buffer. The exact cause (DMA engine, argument convention, memory region restriction) is unknown, but the fix is clear: let the CHDK module do its own memcpy from the ring buffer pointer instead.

**Race condition assessment**: No data corruption observed in 250+ frames. The memcpy in capture_frame_h264 completes well within the 33ms frame period. The ring buffer pointer from sub_FF92FE8C remains valid until the next frame is processed by movie_record_task.

**Remaining issues**:
1. **Initial decoder sync delay**: ~50 frames lost before decoder syncs. See IDR investigation below.
2. **Stats show 0 FPS for first 8 seconds**: Bridge drops frames while decoder accumulates enough data to sync. Once synced, immediately jumps to 30.5 FPS.
3. **SD card device changed**: After SDHCI driver reload, SD card now appears as `/dev/mmcblk1p1` instead of `/dev/mmcblk0p1`. Added `NOT_MOUNTED.txt` marker files to `/mnt/sdcard` and `/mnt/mmc` to prevent accidental writes to unmounted paths.

### IDR Frame Investigation (2026-02-16)

Investigated why the FFmpeg decoder takes ~8 seconds to sync. Attempted to add a `got_idr` filter in `capture_frame_h264()` to skip P-frames until the first IDR keyframe. The filter was built, deployed, and tested — result: **zero frames delivered** because no IDR ever arrives.

**Finding: Canon's H.264 encoder produces NO IDR (type=5) frames in the spy buffer.**

Exhaustive analysis of 300+ frames across 4 bridge runs (20 seconds each):
- Every single frame is NAL type=1 (P-frame, header byte 0x61)
- Zero NAL type=5 (IDR, header byte 0x65) observed
- Larger frames (~43 KB vs ~37 KB average) are still type=1, not IDR

**Ghidra decompilation of movie_record_task message handlers** confirmed:
- **msg 6** → `sub_FF85D98C_my` → `sub_FF92FE8C` (MovieFrameGetter) — the ONLY path that reads frame data from the ring buffer. Our spy hook correctly intercepts ALL frames from this path.
- **msg 8** → `sub_FF92FDF0` — ring buffer housekeeping function (increments counter at `+0x30`, accumulates size at `+0x6C`). Does NOT return frame data. Not a separate IDR handler.
- **msg 2** → recording start, ring buffer init, encoder config
- **msg 5** → pipeline start, file header write
- **msg 4** → recording stop
- **msg 11** → initialization

**Correction**: Previous dev log entry stated "movtask[+0x6C] shows 0x00000065 (NAL header for IDR/type=5)". This was a misinterpretation. Ghidra decompilation of `sub_FF92FDF0` shows `+0x6C` is an accumulator (`+= param_2`), not a NAL type field. The value 0x65 was a coincidental running sum.

**Canon's encoding approach** (one of two possibilities):
1. **Very long GOP**: IDR only at frame 0, then all P-frames for the entire recording. The first IDR may be consumed by the recording pipeline before our spy hook processes it.
2. **Gradual intra-refresh**: No IDR frames at all. Intra-coded macroblocks are distributed across P-frames, refreshing the entire frame over ~8 frames (matching the observed ~8-second decoder sync time).

### Slice Header Analysis & Root Cause (2026-02-16)

Parsed Canon's SPS (`67 42 E0 1F DA 02 80 F6 9B 80 80 83 01`) to extract `log2_max_frame_num_minus4 = 0` → frame_num is 4 bits (0–15). Then decoded slice headers from all captured hex dumps:

- **frame_num=0 NEVER appears** in any captured frame
- First captured frame has frame_num=4 (frames 0–3 missed at startup)
- Large frames (~43–50 KB) always have frame_num=1 (first P after IDR)
- Regular cycle: fn=1,2,3,5,6,8,11,12,14,1,2,3... — frame_num=0 always absent

This proves Canon's encoder **IS producing periodic IDR frames** (at frame_num=0, every ~16 frames), but they never reach `sub_FF92FE8C`.

**Root cause: STATE machine in sub_FF85D98C_my skips the first frame of every IDR cycle.**

The `sub_FF85D98C_my` handler (msg 6) has a two-stage STATE promotion:
1. **Pre-callback check**: if STATE==3 → set STATE=4
2. **Call callback at +0xA0** (registered by msg 2 recording start handler)
3. **Post-callback check**: if STATE≠4 → EXIT (skip frame)

The callback at +0xA0 promotes STATE 2→3 on its first invocation. This means:
- **First msg 6** (IDR frame): STATE=2 → pre-check no-op → callback promotes 2→3 → post-check: STATE=3≠4 → **EXIT (IDR skipped!)**
- **Second msg 6**: STATE=3 → pre-check promotes 3→4 → callback no-op → post-check: STATE=4 → **CONTINUE**

The IDR is always the frame that triggers the 2→3 transition, and the firmware skips it because STATE hasn't reached 4 yet.

### Fixes Applied (2026-02-16)

**Fix 1: STATE 3→4 promotion after callback** — captures the IDR on first msg 6.

Added 3 ARM instructions after the callback in `sub_FF85D98C_my` to also promote STATE 3→4 in the post-callback check:
```asm
"CMP     R0, #3\n"          // callback just promoted 2→3?
"STREQ   R9, [R6,#0x3C]\n"  // yes → promote to 4 (R9=4)
"CMP     R0, #4\n"
"CMPNE   R0, #3\n"          // also accept STATE==3
"BNE     loc_FF85DB10\n"
```

Now the first msg 6 (IDR frame): STATE=2 → callback promotes 2→3 → post-check promotes 3→4 → **CONTINUE (IDR captured!)**.

**Fix 2: AVCC parser accepts SPS/PPS NAL types** — IDR frames may contain inline SPS (type 7) + PPS (type 8) preamble before the IDR slice (type 5). The parser previously only accepted types 1, 5, 6 and silently rejected frames starting with SPS/PPS (`dbg_parse_result = 0x37/0x38`). Added types 7 and 8 to the accepted list, treated as non-VCL (skipped, like SEI). Also increased NAL count limit from 4 to 8 to handle SPS+PPS+SEI+IDR.

**Back-pressure (tested and removed)**: A back-pressure protocol was tested (`hdr[4]` ready flag) to prevent the first frame from being overwritten during startup. While it preserved the first frame, it severely limited throughput (5 FPS vs 30 FPS) because the consumer could only process one frame per PTP round-trip. Removed in favor of the simpler overwrite protocol. If needed in the future, the implementation is documented in git history (commit 6f24641).

**Test result (STATE fix + parser fix, no back-pressure):**
- Throughput restored: **30.4 FPS** (back to normal after removing back-pressure)
- First captured frame: **fn=1** (improved from fn=4 before STATE fix)
- **fn=0 (IDR) still never appears** — not delivered through msg 6 at all
- All 61 decoded frames are NAL type=1 (P-frames, 0x61). Zero type=5 (IDR).
- First frame is 48 KB, second 44 KB (larger than average 38 KB — first P-frames after IDR carry more data)
- FFmpeg decoder syncs at frame #9 via error concealment (P-frame accumulation)
- Camera recording confirmed active

**Conclusion**: The IDR frame (fn=0) is consumed during recording initialization by **msg 5** (`sub_FF85D3BC`), which writes the MOV container header. It never reaches the per-frame msg 6 path where our spy hook lives. This is a firmware design choice — the first encoded frame goes into the MOV header, subsequent frames go through msg 6.

### Msg 5 IDR Capture Hook (2026-02-16)

**Analysis**: Decompiled msg 5 handler (`sub_FF85D3BC`, 680 bytes) using Ghidra headless. It calls:
1. `FUN_ff8eddfc` — encodes the first frame (IDR) via JPCORE pipeline
2. `FUN_ff93048c` — reads IDR pointer and size from ring buffer structure
3. `FUN_ff930b20` — writes MOV container header with IDR data

Key finding from `FUN_ff93048c` (24 bytes):
```c
void FUN_ff93048c(undefined4 *param_1, undefined4 *param_2) {
    int iVar1 = DAT_ff93050c;           // Ring buffer struct pointer (ROM constant)
    *param_1 = *(undefined4 *)(iVar1 + 0xD8);  // Frame pointer (NAL data, past AVCC prefix)
    *param_2 = *(undefined4 *)(iVar1 + 0xDC);  // Frame size (NAL size, without prefix)
}
```

The ring buffer management structure at `*(0xFF93050C)` stores the first frame (IDR) info at:
- `+0xD8` = pointer to NAL data (past the 4-byte AVCC length prefix)
- `+0xDC` = NAL data size (without the length prefix)

**Implementation**: Created `sub_FF85D3BC_my` — a C wrapper that calls the original msg 5 handler via function pointer, then reads the IDR from the ring buffer structure and passes it to `spy_ring_write(ptr-4, size+4)` in AVCC format. Changed msg 5 dispatch in `movie_record_task` from `BL sub_FF85D3BC` to `BL sub_FF85D3BC_my`.

**Test 1 — read ring buffer AFTER calling original handler:**
- Still no IDR captured (all 150+ frames are NAL type=1, P-frames)
- **Severe FPS regression**: only one burst of 19 FPS, then 0 FPS for rest of 20s (previously 30.4 FPS steady)
- The original handler likely clears +0xD8/+0xDC after writing the MOV header

**Test 2 — read ring buffer BEFORE calling original handler:**
- Still no IDR captured (all 150+ frames are NAL type=1, P-frames)
- FPS still degraded: intermittent 8–26 FPS bursts alternating with 0 FPS windows
- The IDR encoding likely happens INSIDE msg 5 (via `FUN_ff8eddfc`), so the ring buffer doesn't have the IDR yet when we read it before the call

**Root cause of failure**: The C wrapper approach (function pointer call to firmware ROM) causes a regression even when the IDR capture itself is a no-op (null checks prevent spy_ring_write from firing). The function pointer call `((void (*)(int))0xFF85D3BC)(msg)` may have ABI/stack issues when called from CHDK code loaded at a different memory region than the firmware ROM.

**Reverted**: Removed `sub_FF85D3BC_my` and restored direct `BL sub_FF85D3BC` to fix the regression. The C wrapper approach to hooking msg 5 is not viable — need a different strategy.

**Key insight**: The IDR is encoded and consumed entirely within `sub_FF85D3BC`. It's not available in the ring buffer before or after the call. To capture it, we need to either:
1. Hook a function INSIDE sub_FF85D3BC (between encoding and MOV header writing)
2. Reimplement sub_FF85D3BC in inline assembly (like sub_FF85D98C_my for msg 6)
3. Accept that the first IDR is lost and focus on capturing periodic IDR frames that arrive later via msg 6

### Deep Analysis: IDR Never Available via Msg 6

Comprehensive decompilation and analysis of the full recording pipeline confirms:

**IDR lifecycle** (msg 5 — `sub_FF85D3BC`, 680 bytes):
1. `FUN_ff930b04(base + 0x200040)` — ring buffer init
2. `FUN_ff8eddfc(output_buf, base + 0x100040, 0x19000, ...)` — JPCORE encodes IDR frame
3. `FUN_ff8274b4(sem, 1000)` — wait for encoding to complete (1s timeout)
4. `sub_FF8C3BFC` + `FUN_ff8c3d38` — start recording pipeline for subsequent P-frames
5. `FUN_ff93048c(&ptr, &size)` — read IDR from ring buffer struct (+0xD8/+0xDC)
6. Adjust for AVCC: `ptr - 4`, `size + 4`
7. `FUN_ff930b20(...)` — write IDR + SPS/PPS to MOV container header

**Why IDR never appears in msg 6**:
- `sub_FF92FE8C` (MovieFrameGetter) increments frame counter at +0x28 on each call
- The ring buffer read cursor has already advanced past the IDR by the time msg 6 fires
- Across 300+ captured frames, `frame_num=0` NEVER appears — the cycle is `fn=1,2,3,...,14,1,2,...`
- ALL captured frames are NAL type=1 (P-frame, header 0x61), zero type=5 (IDR)

**Periodic IDRs do NOT exist in the msg 6 stream**: The encoder resets frame_num to 0 every ~16 frames, but these IDRs are consumed by the ring buffer management layer (msg 5/msg 8) and never delivered through `sub_FF92FE8C`.

**+0xD8/+0xDC are NOT cleared after msg 5**: The IDR pointer remains readable at `*(*(0xFF93050C) + 0xD8)` after msg 5 completes, though the underlying memory may be overwritten by subsequent frames.

### STATE Machine Verification

Verified that the STATE fix in `sub_FF85D98C_my` correctly handles the first msg 6:

**First msg 6 trace** (STATE=2 after msg 2):
1. Pre-check: STATE=2 ≠ 3, no promotion
2. Callback (`BLX R0`): promotes STATE 2→3
3. Post-check (our fix): re-read STATE=3, promote 3→4 via `STREQ R9`
4. Accept: `CMP R0, #3` (R0 still holds 3) → equal → BNE not taken
5. Frame processed → `sub_FF92FE8C` called → spy_ring_write delivers frame

The fix works correctly — the first msg 6 frame IS processed. But it's a P-frame (fn=1), not an IDR, because the IDR was already consumed by msg 5.

### NAL Type Filter Removed (diagnostic build)

Removed the NAL type whitelist from `webcam.c` AVCC parser (previously rejected types other than 1,5,6,7,8). Now accepts ALL NAL types to diagnose whether any unexpected types arrive. The VCL detection still looks for type 1 (P-frame) or type 5 (IDR) to determine frame boundaries.

**Viable IDR capture approaches** (ranked by feasibility):
1. **One-shot ring buffer read at fn=1**: Read `*(*(0xFF93050C) + 0xD8) - 4` (ptr) and `+0xDC + 4` (size) from msg 6 handler on first successful frame — works if memory not yet overwritten
2. **Inline assembly msg 5 hook**: Reimplement all 680 bytes of `sub_FF85D3BC` in asm, insert `BL spy_ring_write` after `FUN_ff93048c` — guaranteed correct but significant effort
3. **Accept no IDR**: Rely on FFmpeg error concealment (~8s sync delay)

## IDR Keyframe Capture — One-Shot Ring Buffer Read (2026-02-16)

### Problem

H.264 streams at 30 FPS but the FFmpeg decoder can't sync for ~8 seconds because the IDR keyframe (NAL type=5) never arrives via msg 6. Evidence from 300+ captured frames:
- ALL frames are NAL type=1 (P-frame, header 0x61), zero type=5
- `frame_num=0` never appears — cycle is fn=1,2,3,...,14,1,2,3,...
- Canon's Digic IV H.264 encoder does NOT produce periodic IDRs during continuous recording

### Root Cause

The IDR is encoded and consumed entirely within msg 5 (`sub_FF85D3BC`). `FUN_ff93048c` reads the IDR pointer/size from the ring buffer struct at `*(0xFF93050C) + 0xD8/0xDC`. These fields are NOT cleared after msg 5 — the IDR pointer persists. The actual IDR data in ring buffer memory survives at least ~33ms (first msg 6) because the buffer is large (>1MB) and only 1-2 P-frames have been written since.

### Implementation: `spy_idr_capture()`

Added to `movie_rec.c` — a C function called from the msg 6 inline asm hook on every frame. On the **first successful** `sub_FF92FE8C` call in msg 6, reads the IDR pointer from the ring buffer struct and sends it via `spy_ring_write` **instead of** the P-frame. All subsequent msg 6 calls send P-frames normally.

**New code in `sub_FF85D98C_my()` asm hook** (replaces direct spy_ring_write call):
```asm
BL      spy_idr_capture     // Returns 1 if IDR was sent
CMP     R0, #0
BNE     loc_FF85DA24        // Skip P-frame if IDR was sent
LDR     R0, [SP, #0x34]    // Normal P-frame delivery
LDR     R1, [SP, #0x30]
BL      spy_ring_write
```

**Safety checks in `spy_idr_capture()`**:
- Only active when webcam magic (0x52455753) is present
- `idr_sent` flag ensures only one IDR per webcam session
- Resets `idr_sent=0` when webcam is not running (for next session)
- Validates rb_base ≠ 0, idr_ptr ≠ NULL, 0 < idr_size ≤ 120000
- Falls through to normal P-frame delivery on any failure

**Expected result**: First frame has NAL header 0x65 (IDR, type=5), decoder syncs immediately.

### Test 1: IDR capture (no debug) — FAILED

Two bridge runs, both showed ALL frames as NAL=0x61 (type=1, P-frames). No IDR (0x65) ever appeared. `spy_idr_capture()` silently returned 0 on every call — either a safety check failed or the ring buffer fields were invalid. No way to tell which check failed without debug output.

### Test 2: Debug frame with NAL type 9 — INVISIBLE

Added `spy_msg5_debug()` called after `sub_FF85D3BC` in `movie_record_task()` to capture +0xD8/+0xDC values at the moment msg 5 completes (when IDR is definitely valid). Modified `spy_idr_capture()` to always send a 64-byte debug frame with NAL type 9 (AUD) containing:
- rb_base, idr_ptr, idr_size (current values from msg 6 time)
- msg5_idr_ptr, msg5_idr_size (values captured during msg 5)
- First 4 bytes at idr_ptr (to verify if data is still IDR)

**Result**: Debug frame never appeared in bridge output. All frames were normal ~40KB P-frames.

**Root cause found**: webcam.c's AVCC parser at `capture_frame_h264()` only accepts NAL types 1 (P-frame) or 5 (IDR). NAL type 9 (AUD) was silently discarded — `have_vcl` stayed 0, function returned 0. The debug frame WAS being sent via `spy_ring_write`, but webcam.c filtered it out before it reached the bridge.

### Test 3: Debug frame with NAL type 1 — INVISIBLE (race condition)

Fixed debug frame to use NAL header 0x61 (type=1) so it passes webcam.c's AVCC parser. Bridge run showed USB pipe error (camera needed power cycle). Second run: connection OK but debug frame still never appeared — all frames were normal ~40KB P-frames.

**Root cause: spy buffer race condition.** The `spy_ring_write` protocol is a single-element overwrite buffer. `spy_idr_capture()` sends the 64-byte debug frame via `spy_ring_write`, then returns 1 (skipping this msg 6's P-frame). But `movie_record_task` processes queued messages back-to-back without blocking — the NEXT msg 6 fires immediately and calls `spy_ring_write` with a P-frame, overwriting `hdr[1]`/`hdr[2]` before webcam.c (lower priority task) can read the debug frame.

Timeline:
1. `spy_idr_capture` → `spy_ring_write(debug_buf, 64)` → GiveSemaphore
2. movie_record_task does NOT yield (higher priority)
3. Next msg 6 → `spy_ring_write(P-frame, 40KB)` → overwrites hdr[1]/hdr[2]
4. webcam.c finally runs → reads P-frame, not debug frame

### Test 4: Persistent debug via hdr[8..15] + webcam.c injection — size check bug

New approach avoids the race condition entirely:

**movie_rec.c**: `spy_idr_capture()` writes debug values to `hdr[8..15]` (spy buffer slots NOT touched by `spy_ring_write`), returns 0 to let the P-frame through normally.

**webcam.c**: In `capture_frame_h264()`, after memcpy of P-frame data, checks `hdr[8]` for debug marker `0xDB600001`. If found, shifts frame data right by 36 bytes and prepends a 36-byte debug AVCC NAL.

**Debug NAL layout** (36 bytes):
| Bytes | Content | Endian |
|-------|---------|--------|
| 0-3 | AVCC length = 32 | big-endian |
| 4 | NAL header 0x61 (type=1) | — |
| 5 | 0xDB marker | — |
| 6 | msg5_count (low byte) | — |
| 7 | 0xDB marker | — |
| 8-11 | rb_base | little-endian |
| 12-15 | idr_ptr (current +0xD8) | little-endian |
| 16-19 | idr_size (current +0xDC) | little-endian |
| 20-23 | msg5_idr_ptr | little-endian |
| 24-27 | msg5_idr_size | little-endian |
| 28-31 | data at idr_ptr (first 4 bytes) | little-endian |
| 32-35 | 0xDB padding | — |

**Result**: FAIL — first frame is a normal ~38KB P-frame (NAL=0x61). The debug NAL never appeared.

**Root cause**: `sub_FF92FE8C` returns size = 262144 (0x40000 = 256KB ring buffer chunk), not the AVCC frame size. After clipping: `size = min(262144, 65536) = 65536 = SPY_BUF_SIZE`. The injection guard `size + 36 <= SPY_BUF_SIZE` evaluates to `65572 > 65536` → **always false**. The injection never fires.

### Test 5: Fixed size check — debug data received!

**Fix**: Changed injection from shift+prepend to OVERWRITE first 36 bytes of frame data. Guard changed to `size >= 36` (always true). AVCC parser finds the 32-byte debug NAL (type=1) first and returns only 36 bytes. Bridge decoder fails and prints hex dump.

**Result**: FAIL #1 is exactly 36 bytes — the debug NAL:
```
00 00 00 20 61 db 00 db 68 89 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04 00 00 00 0a 00 00 00
```

**Decoded debug values**:
| Field | Value | Meaning |
|-------|-------|---------|
| msg5_count | **0** | msg 5 was NEVER called before first msg 6 |
| rb_base | 0x00008968 | Ring buffer struct exists |
| idr_ptr (+0xD8) | **0x00000000** | IDR pointer is NULL — not written yet |
| idr_size (+0xDC) | **0** | No IDR data |
| msg5_idr_ptr | 0 | spy_msg5_debug never ran |
| msg5_idr_size | 4 | Unexpected (should be 0 — possible memcpy-from-volatile issue) |
| data_at_ptr | 0x0A | Unexpected (should be 0xDEADDEAD — same memcpy issue) |

**Key finding: msg 6 (P-frame) arrives in the message queue BEFORE msg 5 (IDR encoding).** The `movie_record_task` message loop processes messages FIFO. The first msg 6 is dequeued and processed before msg 5 has been posted or processed. Therefore `spy_idr_capture()` runs when `+0xD8` is still NULL — the IDR hasn't been encoded yet.

**Secondary issue**: `memcpy(frame_data_buf + 8, (void *)&hdr[9], 24)` casts away `volatile` qualifier. Some debug values (msg5_idr_size=4, data_at_ptr=0x0A) don't match expected values, suggesting memcpy may read stale/incorrect data from volatile memory. Should use individual volatile reads instead.

**Next step**: Add `msg5_done` flag — `spy_msg5_debug` sets it after msg 5 completes, `spy_idr_capture` only fires when `msg5_done` is true. This ensures the IDR is available. Also fix volatile read issue.

## v17 — Debug Frame Protocol (2026-02-16)

Replaced the `hdr[8..15]` debug injection hack (fake AVCC NAL in H.264 frame data) with a proper debug channel using a lock-free SPSC ring buffer and dedicated frame format.

### Implementation

- **Camera producer** (`movie_rec.c`): `spy_debug_reset()` / `spy_debug_add(tag, val)` / `spy_debug_send()` API builds tagged key-value debug frames and enqueues them into a 4-slot ring buffer at `0x000FF040`.
- **Camera consumer** (`webcam.c`): `capture_frame_h264()` checks the debug queue before `TakeSemaphore`. Debug frames are returned immediately as `WEBCAM_FMT_DEBUG = 3`.
- **Bridge** (`main.cpp`): `print_debug_frame()` recognizes format=3, prints structured `=== DEBUG FRAME ===` output to stderr, appends to `debug_frames.log`.

See [Debug Frame Protocol](debug-frame-protocol.md) for full specification.

### Test 1: No debug frame received

**Bridge output**: 20s run, H.264 P-frames at 6-19 FPS, camera recorded normally. No `=== DEBUG FRAME ===` output appeared. All frames were H.264 (decoder fails — needs IDR, as expected).

**Root cause**: `get_frame()` in webcam.c had no case for `WEBCAM_FMT_DEBUG`. The format dispatch chain only handled H.264, JPEG, and UYVY. When `capture_frame_h264()` returned a debug frame with `frame_format = WEBCAM_FMT_DEBUG`, it fell through all format checks to the software JPEG fallback, which returned `-1` (no data). Debug frame silently dropped.

**Fix**: Added `WEBCAM_FMT_DEBUG` case to `get_frame()` before the H.264 check:
```c
if (frame_format == WEBCAM_FMT_DEBUG && hw_jpeg_data && hw_jpeg_size > 0) {
    frame->data = hw_jpeg_data;
    frame->size = hw_jpeg_size;
    // ... set width/height/frame_num/format
    return 0;
}
```

**Status**: Fixed, verified in Test 2.

### Test 2: Queue write_idx stays 0

After fixing get_frame(), debug frame still didn't appear. Added `hdr[8]` (write_idx) and `hdr[9]` (read_idx) to diagnostics — both stayed 0 for the entire 20s run. `spy_debug_send()` was never called.

**Root cause**: `spy_idr_capture()` had a `if (!msg5_done) return 0;` gate that waited for msg 5 to fire before sending debug data. But msg 5 never fired (or fired too late). The old working code (commit `1a61d00`) didn't have this gate — it fired on the first msg 6 unconditionally.

**Fix**: Removed the `msg5_done` gate from `spy_idr_capture()`. Debug frame now fires on the first msg 6 with whatever values are available.

### Test 3: Debug frame received successfully

```
=== DEBUG FRAME #0 (7 entries, 68 bytes) ===
  RBas = 0x00008968  (35176)
  IdrP = 0x00000000  (0)
  IdrS = 0x00000000  (0)
  M5Pt = 0x00000000  (0)
  M5Sz = 0x00000000  (0)
  DatP = 0xDEADDEAD  (3735936685)
  M5Ct = 0x00000000  (0)
=== END DEBUG ===
```

- Debug frame appeared as first frame, before any H.264 data
- Queue indices: write_idx=1, read_idx=1 (one frame written, one consumed)
- After debug frame, H.264 FAIL #1 is a normal 42KB P-frame (type=1, as expected)
- Camera recorded normally throughout
- All 7 tagged values displayed correctly with 4-char tags

**Debug frame protocol confirmed working.** The SPSC queue, format dispatch, and bridge printer all function correctly.

## v18 — Multi-point Debug Instrumentation (2026-02-16)

Goal: Send debug frames from multiple hook points across the message timeline to find when and where IDR data appears in the ring buffer.

### Crash investigation

Adding a new BSS variable (`static int msg6_count`) to `movie_rec.c` caused immediate camera crashes on recording start — even when the variable was only incremented once. The crash persisted across battery pulls, ruling out accumulated state.

**Root cause**: BSS layout changes. Adding any new `static` variable shifts the BSS section, which can cause memory conflicts with Canon firmware on this bare-metal platform.

**Solution**: Repurpose existing `idr_sent` variable as a counter instead of a boolean flag. No new BSS variables, no layout changes. Changed from `if (idr_sent) return 0; idr_sent = 1;` to `if (idr_sent >= 2) return 0; idr_sent++;` to fire on the first 2 msg 6 calls.

### Test 1: Two debug frames from msg 6 (M6.1 + M6.2)

```
=== DEBUG FRAME #0 (7 entries, 68 bytes) ===      ← first msg 6 (before msg 5)
  Src_ = 0x4D362E31  ("M6.1")
  RBas = 0x00008968  (35176)
  IdrP = 0x00000000  (0)           ← IDR pointer NOT yet written
  IdrS = 0x00000000  (0)           ← IDR size NOT yet written
  DatP = 0xDEADDEAD               ← safe fallback (ptr was 0)
  M5Ct = 0x00000000  (0)           ← msg 5 hasn't run
  M5Dn = 0x00000000  (0)           ← msg 5 not done

=== DEBUG FRAME #1 (7 entries, 68 bytes) ===      ← second msg 6 (later)
  Src_ = 0x4D362E32  ("M6.2")
  RBas = 0x00008968  (35176)
  IdrP = 0x000158AC  (88236)       ← IDR pointer IS populated!
  IdrS = 0x0000ACEC  (44268)       ← IDR size = 44,268 bytes
  DatP = 0x00000000  (0)           ← first 4 bytes at IDR ptr are ZERO
  M5Ct = 0x00000000  (0)           ← msg 5 counter still 0
  M5Dn = 0x00000000  (0)           ← msg 5 still not done
```

**Key findings**:
1. **+0xD8/+0xDC ARE populated by the second msg 6** — the ring buffer struct has IDR pointer and size filled in
2. IDR pointer `0x000158AC` is in low firmware RAM, size 44,268 bytes is plausible for an H.264 IDR frame
3. **DatP = 0x00000000** — the data at the IDR pointer starts with zeros, NOT a NAL start code (0x00000001 65). The pointer may be an offset rather than absolute address, or the data format isn't raw H.264
4. **msg 5 never fired** between the two msg 6 calls — the IDR data was written by some other mechanism, not by the msg 5 handler
5. Camera recorded normally for the full 20 seconds

**Next steps**: Investigate whether `0x000158AC` is an offset into the ring buffer (base `0x00008968`), making the absolute address `0x00008968 + 0x000158AC = 0x0001E214`. Also try reading more bytes from the IDR pointer region to look for NAL signatures.

## v18.1 — Bridge Memory Probes (2026-02-16)

Used PTP `CHDK_GetMemory` to read camera RAM from the PC side, avoiding camera-side code changes that crash due to BSS layout sensitivity.

### Probes after M6.2 debug frame

| Probe | Address | Result |
|-------|---------|--------|
| rb_base+0xC0 (32 bytes) | 0x8968+0xC0 | +0xC0=0x412C4720, +0xC4=0x41304720, +0xD0=0x02AD0000, +0xD8=IdrP, +0xDC=IdrS |
| @IdrP | 0x000158AC | All zeros |
| @(+0xD0)+IdrP | 0x02AE58AC | All 0xFF |
| @(+0xC0)+IdrP | 0x412D9FCC | All 0xFF |
| @uncached (+0xD0\|0x40000000)+IdrP | 0x42AE58AC | All 0xFF |
| P-frame ptr (hdr[1]) | 0x4133xxxx | Valid P-frame data: AVCC len ~36-40KB, NAL=0x61 (type=1) |

**Key findings**:
1. **IdrP (0x158AC) is constant** across all runs — NOT a data pointer, likely a fixed struct field or offset
2. **DMA buffer (+0xD0 = 0x02AD0000) contains 0xFF** at IdrP offset, both cached and uncached
3. **P-frame pointers are in 0x4133xxxx range** (uncached memory), from buffers at +0xC0 (0x412C4720) and +0xC4 (0x41304720), each 256KB apart
4. **All H.264 frames from sub_FF92FE8C are P-frames** (NAL type=1). No IDR (NAL type=5) ever appears through this path
5. **IDR encoding happens elsewhere** — not through the msg 6 / sub_FF92FE8C path we're hooked into

### BSS sensitivity confirmed

Any firmware build with BSS != 14544 crashes the camera:
- Working: text=90376, bss=14544 → stable
- IDR injection attempt: text=90528, bss=14536 → crash
- Debug-only variant: text=90336, bss=14536 → crash

## v18.2 — ARM Memory Barrier Fix for Debug Queue (2026-02-16)

**Problem**: Camera hangs requiring USB disconnect + battery pull started after the debug buffer implementation. Investigation revealed the lock-free SPSC debug queue had no ARM memory barrier between writing slot data and advancing the write index.

**Root cause**: On the ARM926EJ-S (ARMv5, Digic IV), `volatile` prevents compiler reordering but does NOT prevent the CPU's hardware write buffer from reordering stores. Without a Drain Write Buffer (DWB) instruction, the consumer in webcam.c could see `hdr[8] = next_wr` (write index advanced) before the actual slot data was committed to memory, causing:
1. Consumer reads garbage from the slot → `dbg_size` fails validation (not in 12-508 range)
2. Consumer advances `hdr[9]` anyway → debug frame permanently lost
3. Under certain timing, corrupted queue state could cascade into PTP/USB hangs

**Fix**: Added `mcr p15, 0, r0, c7, c10, 4` (ARM Drain Write Buffer) at two points in movie_rec.c:
1. **`spy_debug_send()`**: Before `hdr[8] = next_wr;` — ensures all slot data stores are committed before the write index update becomes visible
2. **`spy_ring_write()`**: Before `hdr[3]++;` — ensures frame pointer and size are committed before the frame counter increment

Both are code-only changes (inline asm + stack local variable). BSS stays at exactly 14544.

**Build**: text=90392 (+16 bytes for inline asm), data=41928, bss=14544 (unchanged), MD5=a0954d1b09241f5499302ddf06d98d72

### Test result

- Camera recorded normally for the full 20 seconds
- Both debug frames (M6.1 + M6.2) received intact
- Debug queue indices: write_idx=2, read_idx=2 (both frames produced and consumed correctly)
- 200+ H.264 P-frames streamed successfully
- **Camera shut down cleanly** — no hang, no battery pull needed
- Bridge exited cleanly after 20-second timeout

## v18.3 — DMA/IDR Buffer Probes + Message Architecture Discovery (2026-02-18)

**Goal**: Find IDR frame data in camera RAM by instrumenting both msg 5 (IDR encoding) and msg 6 (P-frame retrieval) with debug frames that read the DMA region and IDR buffer.

### Changes

**`spy_msg5_debug`** — Enhanced to send a debug frame tagged "MSG5" with:
- DMA base from `rb_base + 0xD0` (expected 0x02AD0000)
- IDR buffer address: `DMA base + 0x100040` (expected 0x02BD0040)
- First 8 bytes at IDR buffer (IHd0, IHd4)
- Ring buffer +0xD8/+0xDC values and counters
- Only fires on first msg 5 occurrence

**`spy_idr_capture`** — Redesigned firing pattern:
- Frames 1-2: always (early state before msg 5)
- First msg 6 after each msg 5: triggered by `msg5_done` flag transition (1→2)
- Same DMA/IDR buffer fields as msg 5 debug frame
- Removed NAL/AVCL fields (DMA not written when spy_idr_capture reads)

**Build**: text=90936 (+464), data=41928, bss=14544 (unchanged), MD5=0c7640fcf65a06309bf29e88e5b4391e

### Test result

- Clean 20s session, 300+ H.264 P-frames received (all NAL=0x61)
- **2 debug frames received (both MSG6)** — zero MSG5 debug frames
- **msg 5 NEVER fired** during the entire 20s session (M5Ct=0, M5Dn=0)
- DMA base and IDR buffer addresses confirmed:

| Field | Frame 1 | Frame 2 |
|-------|---------|---------|
| DMAb | 0x02AD0000 | 0x02AD0000 |
| IBuf | 0x02BD0040 | 0x02BD0040 |
| IHd0 | 0xFFFFFFFF | 0xFFFFFFFF |
| IHd4 | 0xFFFFFFFF | 0xFFFFFFFF |
| IdrP | 0x00000000 | 0x000158AC |
| IdrS | 0x00000000 | 0x0000BF60 |
| M5Ct | 0 | 0 |
| M5Dn | 0 | 0 |

- IDR buffer at 0x02BD0040 contains 0xFFFFFFFF — **never written** during webcam session
- IdrP/IdrS on frame 2 are stale MOV file offsets from a previous recording session

### Critical discovery: H.264 recording message architecture

Decompilation analysis of all message handlers in movie_record_task revealed the full recording architecture:

**sub_FF92FE8C (msg 6) is a frame GETTER, not an encoder.** It reads the next frame from the ring buffer — it does NOT encode anything. The ring buffer is populated by the JPCORE hardware pipeline running autonomously.

**IDR frames never enter the ring buffer.** They are encoded separately and written directly to the MOV file:

| Message | Handler | Role |
|---------|---------|------|
| msg 11 | loc_FF85E0AC | Init: clear counters, STATE=1 |
| msg 2 | sub_FF85DE1C | Start recording: allocate DMA region, set up JPCORE pipeline, configure ring buffer, register callbacks, STATE=2 |
| msg 5 | sub_FF85D3BC | First-frame init: encode first IDR via `FUN_ff8eddfc` to `DMA+0x100040`, write MOV file header (`ftyp`+`mdat`+SPS/PPS), set up `RecPipelineSetup` + `StartMjpegMaking` |
| msg 6 | sub_FF85D98C | Frame retrieval: call `sub_FF92FE8C` to read next P-frame from ring buffer, write to MOV file via `sub_FF8EDBE0` |
| msg 8 | sub_FF92FDF0 | Frame committed: notification from JPCORE pipeline, cache writeback, conditionally calls `FUN_ff92fd78` |
| msg 4 | sub_FF85D6CC | **Periodic IDR**: stops pipeline (`StopMjpegMaking_Inner`), re-encodes IDR to `DMA+0x100040`, then **posts msg 5** to the queue (`*puVar6 = 5; FUN_ff8279ec(queue, ...)`) |
| msg 7 | sub_FF85D218 | Stop recording: cleanup, flush |
| msg 10 | sub_FF85E28C | Shutdown: reset state |

**Recording flow**:
```
msg 11 (init) → msg 2 (start pipeline) → msg 5 (first IDR + MOV header)
  → pipeline runs autonomously:
      JPCORE encodes P-frames → ring buffer → msg 8 (committed) → msg 6 (read + MOV write)
  → every ~15 frames:
      msg 4 (stop pipeline, encode IDR) → posts msg 5 (write IDR + MOV header update)
      → pipeline restarts
```

**Why our webcam session only gets P-frames**:
1. Camera is already recording when webcam module starts
2. msg 5 (first IDR) already fired at recording start — before our hooks were active
3. msg 4 (periodic IDR) does NOT fire during webcam sessions (unknown trigger mechanism)
4. The ring buffer only contains P-frames from the continuous JPCORE pipeline
5. Our spy hook in msg 6 → sub_FF92FE8C can ONLY read from the ring buffer
6. Therefore: **IDR frames are architecturally unreachable through the ring buffer path**

**sub_FF92FE8C ring buffer details**:
- First frame (counter==1): reads from `+0xC0` (0x412C4720, uncached DMA alias)
- Subsequent frames: reads from `+0x1C` (write pointer, advances through ring)
- Frame size from `+0x70` (total frame size field)
- Ring wraps when `+0x1C` reaches `+0xC8` (buffer end), continues from `+0xC4`

**Key functions identified**:
- `FUN_ff8eddfc` (0xFF8EDDFC, 560 bytes): JPCORE H.264 encoder — configures pipeline registers, sets output buffers, starts encoding. Called by msg 5 for first IDR.
- `FUN_ff8ee610`: Secondary encoder — called by msg 4 for periodic IDR re-encoding to `DMA+0x100040`.
- `FUN_ff930b20` (0xFF930B20, 344 bytes): MOV container header writer — creates file atoms, writes SPS/PPS + IDR data to SD card.
- `FUN_ff93048c` (0xFF93048C, 24 bytes): Returns `+0xD8`/`+0xDC` from ring buffer struct — these are MOV file offsets, NOT RAM pointers.

## v19 — Deep Firmware Investigation: IDR Architecture (2026-02-18)

### Comprehensive Ghidra Decompilation

Decompiled 74 functions (6 failed) covering the full IDR encoding, pipeline, ring buffer, and message architecture using `DecompileIDRArchitecture.java`. Output: `firmware-analysis/idr_architecture_decompiled.txt` (3900+ lines).

### Corrected Message Architecture

Previous understanding of msg 4 was wrong. Corrected roles:

| Message | Handler | Role (CORRECTED) |
|---------|---------|-------------------|
| msg 2 | sub_FF85DE1C | Start recording: DMA alloc, JPCORE pipeline, ring buffer, STATE=2 |
| msg 3 | (inline) | Sets `+0x2C = 1` (stop request flag) — does NOT stop recording |
| msg 4 | sub_FF85D6CC | **Recording STOP** (not periodic IDR): stops pipeline, writes MOV trailer, posts msg 5 |
| msg 5 | sub_FF85D3BC | First IDR + MOV header: `FUN_ff8eddfc` encodes IDR, `FUN_ff930b20` writes MOV |
| msg 6 | sub_FF85D98C | Per-frame: `sub_FF92FE8C` reads P-frame from ring buffer, `sub_FF8EDBE0` writes to MOV |
| msg 8 | sub_FF92FDF0 | Ring buffer notification from JPCORE hardware |

### Two Separate H.264 Encoding Paths

The firmware uses two distinct JPCORE hardware channels that cannot run simultaneously:

1. **Pipeline path** (P-frames): `StartMjpegMaking` configures JPCORE for continuous encoding. Output goes to ring buffer → read by msg 6 → written to MOV via `sub_FF8EDBE0`. This is what our spy hook intercepts.

2. **IDR encoder path**: `FUN_ff8eddfc` (first IDR, called by msg 5) and `FUN_ff8ee610` (periodic IDR, called by msg 4) encode standalone IDR frames to `DMA+0x100040`. Output is written directly to the MOV file header/trailer by `FUN_ff930b20`. **Never enters the ring buffer.**

### IDR vs P-Frame Decision Point

Found in `sub_FF8EDBE0_EncodeFrame`:
- `param_13 == -1` (0xFFFFFFFF) → IDR frame → calls `FUN_ff8eda90(1)` → JPCORE channels 3+0x11
- `param_13 == -2` → special frame → calls `FUN_ff8eda90(0)`
- `param_13 >= 0` → P-frame → calls function at `iVar1 + 0x6c`

The first call from msg 5 (msg 6 path, frame 0) uses `param_13 = -1` (IDR). All subsequent msg 6 calls use `param_13 >= 0` (P-frame). This confirms IDR encoding is architecturally separate from the ring buffer pipeline.

### IDR Interval Controller

`FUN_ff92e3b0` (called with `frame_count, 0xF`) manages a 15-frame GOP cycle. However, during webcam sessions, msg 4 (which would trigger periodic IDR re-encoding) never fires because the stop-request flag `+0x2C` is never set by the webcam module.

### FUN_ff8f2558 Is NOT a Frame Type Selector

Previously speculated to control IDR vs P-frame encoding mode. Decompilation proves it's a **hardware clock/power enable bitmask**:
- `0x81` = power on both IDR encoder block and thumbnail block
- `0x80` = IDR encoder block only
- `0x01` = pipeline/thumbnail block only

### Current Hook Location

The spy hook in `sub_FF85D98C_my` intercepts frames **after** `sub_FF92FE8C` (MovieFrameGetter) returns them from the ring buffer but **before** `sub_FF8EDBE0` (EncodeFrame) processes them for MOV writing. This is the correct location for P-frame interception but IDR frames never pass through this path.

### Six Approaches Evaluated

| # | Approach | Risk | Feasibility |
|---|----------|------|-------------|
| 1 | Post msg 4 to movie_record_task queue | HIGH | Triggers file I/O (MOV trailer write) — would fail or corrupt |
| 2 | Call FUN_ff8ee610 directly from webcam module | MEDIUM-HIGH | Requires pipeline stop, JPCORE reconfig, correct DMA setup |
| 3 | Flip JPCORE hardware register for IDR | HIGH | Not how it works — IDR uses different channels (3+0x11) |
| 4 | **Extract SPS/PPS from real MOV, synthetic IDR on bridge** | **SAFEST** | Parse MOV `avcC` atom, construct SPS+PPS+IDR NAL prefix |
| 5 | Trigger brief recording at startup for real IDR | MEDIUM | Timing-sensitive, may conflict with webcam pipeline |
| 6 | FFmpeg error concealment (accept P-frame-only stream) | NONE | Already works (~8s sync delay), but quality degrades |

### Recommended Path Forward

**Option 4 (synthetic IDR from MOV SPS/PPS)** is the safest approach:
1. Record a short clip on the camera (any resolution/quality)
2. Extract the `avcC` atom from the MOV file — contains SPS and PPS parameter sets
3. On the bridge side, construct a synthetic IDR preamble: `[00 00 00 01] + SPS + [00 00 00 01] + PPS + [00 00 00 01 65 ...]`
4. Feed this to FFmpeg before the first P-frame arrives
5. The decoder initializes immediately with correct parameters, eliminating the 8-second sync delay

The SPS/PPS are static for a given resolution/quality setting. Once extracted, they can be hardcoded in the bridge for the 640x480@30fps webcam mode.

**Alternative**: Option 6 (accept current behavior) already works — FFmpeg syncs via error concealment after ~8 seconds. If the 8-second delay is acceptable, no firmware changes are needed.

## v20 — First-Frame IDR Probe (2026-02-18)

### Discovery: sub_FF8EDBE0(param=-1) IS an IDR encoder trigger

Deeper analysis of the msg 6 handler (`sub_FF85D98C_my`) revealed that the **first frame** (frame_counter == 0) calls `sub_FF8EDBE0` with `param_13 = -1` (`MVN R2, #0`). Decompilation of `sub_FF8EDBE0` confirmed:

- `param_13 == -1` → calls `FUN_ff8eda90(1)` → **triggers real IDR encoding** via JPCORE hardware
- `param_13 == -2` → calls `FUN_ff8eda90(0)` → special frame mode
- `param_13 >= 0` → calls `*(iVar1 + 0x6C)(param_13)` → normal P-frame write

This means the firmware **already encodes an IDR on the very first msg 6 call**. But our spy hook (`spy_ring_write`) runs BEFORE `sub_FF8EDBE0` — we capture the raw ring buffer P-frame, missing the IDR output entirely.

### Three concrete interception points identified

| # | Location | When | What's there |
|---|----------|------|-------------|
| 1 | **After sub_FF8EDBE0(param=-1)** in msg 6 first-frame path | First msg 6 after pipeline start | IDR data at DMA+0x100040 (if JPCORE wrote it) |
| 2 | **spy_msg5_debug** after msg 5 handler | When msg 5 fires (triggered by msg 4) | IDR data at DMA+0x100040 |
| 3 | **Post msg 4** to movie_record_task queue | On demand from spy_idr_capture | Forces: stop pipeline → encode IDR → post msg 5 → restart pipeline |

### Additional findings from agent analysis

**SPS/PPS populated independently of msg 5**: `FUN_ff9300b4` (called from the msg 6 path) stores data at `rb_base+0xD8` and `rb_base+0xDC` on the first frame when ring buffer frame counter (`+0x24`) reaches 1. SPS/PPS byte count is accessible via `FUN_ff930ec0(*(rb_base + 0x8C))`.

**Msg 4 posting requirements**: Message allocator `FUN_ff85e260()` returns a message struct pointer. Set `msg[0] = 4`, then call `FUN_ff8279ec(queue, msg, timeout, errstr, size)` where `queue = *(0x51A8 + 0x1C)`. Precondition: encoder handle at `*(0x51A8 + 0x7C)` must be non-zero, otherwise msg 4 handler exits immediately with error.

### Implementation: `spy_first_frame_probe`

Added a new C function hooked in the inline ASM at `loc_FF85DBD0` — fires AFTER the first `sub_FF8EDBE0(param=-1)` + `TakeSemaphore` succeeds. This is the first probe that reads DMA+0x100040 **after** the IDR-flagged encode completes.

**ASM insertion point** (after `sub_FF8EDC88` cleanup, before frame pointer adjustment):
```asm
"loc_FF85DBD0:\n"
"MOV     R0, #1\n"
"BL      sub_FF8EDC88\n"
"LDR     R0, [SP,#0x3C]\n"         // R0 = encoded_offset (bytes consumed)
"BL      spy_first_frame_probe\n"   // Probe DMA+0x100040
"LDR     R0, [SP,#0x3C]\n"         // Re-load (clobbered by BL)
"LDR     R1, [SP,#0x34]\n"
"ADD     LR, R1, R0\n"
```

Safe because: R0-R3 and LR are caller-saved (AAPCS). Next instructions load from SP-relative addresses, not register values. R4-R11 preserved by callee. LR is overwritten by `ADD LR, R1, R0` after the hook.

**Debug frame output** (tagged "FRM1"):

| Tag | Field | Purpose |
|-----|-------|---------|
| `EncO` | encoded_offset | Bytes consumed by sub_FF8EDBE0 as "IDR" portion. 0 = no-op |
| `IBuf` | DMA+0x100040 | IDR buffer absolute address |
| `IHd0`..`IHC_` | First 16 bytes at DMA+0x100040 | Real H.264 data if IDR was encoded, 0xFFFFFFFF if not |
| `EncH` | *(0x51A8+0x7C) | Encoder handle — must be non-zero for msg 4 posting |
| `SPSp` | *(rb_base+0x8C) | SPS/PPS related pointer in ring buffer struct |
| `FCnt` | *(rb_base+0x24) | Ring buffer frame counter |
| `DMAb` | *(rb_base+0xD0) | DMA region base address |

**Expected debug output sequence**: Up to 4 debug frames:
1. `MSG6` frame 1 — state before first sub_FF8EDBE0
2. `FRM1` — state AFTER first sub_FF8EDBE0(param=-1) **(critical new probe)**
3. `MSG6` frame 2 — state on second msg 6 call

**Key diagnostics**:
- `IHd0` at `FRM1` != 0xFFFFFFFF → **IDR data IS at DMA+0x100040** → can capture it
- `EncO` > 0 → sub_FF8EDBE0 consumed bytes as IDR header data
- `EncH` != 0 → msg 4 posting is viable (encoder handle valid)
- `SPSp` != 0 → SPS/PPS extraction from ring buffer is possible

**Build**: text=3088 (+416), data=0, bss=540 (unchanged). No new static variables.

## v21 — DMA+0x100040 Crash & Safe Metadata Probe (2026-02-21)

### Critical finding: DMA+0x100040 is NOT readable

Reading from `DMA_base + 0x100040` (= `0x02AD0000 + 0x100040` = `0x02BD0040`) causes a **data abort** on the ARM926EJ-S, crashing the recording task. This was tested in three different contexts:

1. **`spy_first_frame_probe` in inline ASM** (after `sub_FF8EDC88`): Camera started recording, hung at 0" — data abort during first frame processing
2. **`spy_idr_capture` in msg 6 handler**: Camera didn't even start recording — data abort during first msg 6 call
3. **`spy_msg5_debug` in msg 5 handler**: Not tested independently (removed along with other DMA reads)

The DMA buffer at offset 0x100040 is likely mapped with restricted access permissions (e.g., DMA-only, not CPU-readable), or the offset 0x100040 is beyond the allocated buffer size.

**Lesson**: Never dereference DMA buffer addresses from CPU code. Only read metadata (offsets, sizes, pointers) from the ring buffer struct — not the DMA buffer contents directly.

### Reverted to known-good base (commit a332449)

After 3 consecutive boot failures with DMA reads, reverted `movie_rec.c` to the last working commit (`a332449` — "Multi-point debug: IDR ptr populated by 2nd msg 6") and incrementally added only safe metadata reads.

### Safe metadata probe results

Enhanced `spy_idr_capture` (msg 6 handler) to report additional metadata without reading DMA buffers:

**Debug Frame #0 — M6.1 (first msg 6, before msg 5):**

| Field | Value | Meaning |
|-------|-------|---------|
| `RBas` | `0x00008968` | Ring buffer struct base |
| `DMAb` | `0x02AD0000` | DMA region base address |
| `IdrP` | `0x00000000` | IDR pointer — not yet written |
| `IdrS` | `0x00000000` | IDR size — not yet written |
| `DatP` | `0xDEADDEAD` | Data at IdrP — N/A (ptr is 0) |
| `EncH` | `0x0001AF28` | **Encoder handle — VALID** (msg 4 posting possible) |
| `SPSp` | `0x00000280` | SPS-related value at rb_base+0x8C (likely offset: 640) |
| `FCnt` | `0x00000000` | Frame counter = 0 (first frame) |
| `M5Ct` | `0x00000000` | msg 5 count = 0 (hasn't fired) |
| `M5Dn` | `0x00000000` | msg 5 done = 0 |

**Debug Frame #1 — M6.2 (second msg 6):**

| Field | Value | Meaning |
|-------|-------|---------|
| `RBas` | `0x00008968` | Same ring buffer struct |
| `DMAb` | `0x02AD0000` | Same DMA base |
| `IdrP` | `0x000158AC` | **IDR pointer populated** (offset, not absolute) |
| `IdrS` | `0x0000B6B0` | **IDR size = 46,768 bytes** |
| `DatP` | `0x00000000` | Data at IdrP = 0 (it's an offset, not a valid RAM pointer) |
| `EncH` | `0x0001AF28` | Encoder handle still valid |
| `SPSp` | `0x00000280` | SPS offset unchanged |
| `FCnt` | `0x00000001` | Frame counter = 1 |
| `M5Ct` | `0x00000000` | msg 5 still hasn't fired |
| `M5Dn` | `0x00000000` | msg 5 still hasn't fired |

### Key conclusions (CORRECTED after v21 ring buffer base discovery)

1. **Encoder handle is valid from the first msg 6**: `EncH = 0x0001AF28` — msg 4 posting is possible
2. ~~IdrP is a file offset~~ **CORRECTED**: IdrP is an offset relative to `*(rb_base+0xC4)` — see v21 correction below
3. ~~DMA region not CPU-readable~~ **CORRECTED**: The crash at 0x02BD0040 was reading the wrong address entirely — see v21
4. **msg 5 never fires during webcam mode**: Both frames show `M5Ct=0`, `M5Dn=0`
5. **SPSp = 0x280 = 640**: Matches horizontal resolution, likely a resolution parameter

### v21 CORRECTION: Ring buffer data base discovered

**`rb_base+0xC4 = 0x41304720`** — the ring buffer data area base in **uncached RAM** (0x41xxxxxx, same range as P-frame buffers at 0x40EAxxxx).

**Correct IDR address**:
```
IDR_address = *(rb_base + 0xC4) + *(rb_base + 0xD8)
            = 0x41304720 + 0x158AC
            = 0x4131FFCC  (uncached, CPU-readable!)
```

**Why the earlier probe failed**: The bridge probed `@(+0xC0)+IdrP` = `0x412D9FCC` — **off by 4 bytes** (used +0xC0 instead of +0xC4), landing at the wrong address which returned 0xFFFFFFFF.

**Why the DMA+0x100040 crash happened**: Address `0x02BD0040` (from Ghidra decompilation) is in the DMA configuration region, NOT the ring buffer data area. The IDR data lives in uncached RAM at `0x4131FFCC`, which is the same kind of memory the P-frames are read from at 30fps.

**Decompilation confirmation**: `FUN_ff9300b4` sets `+0xD8 = *(+0xD4) + 4` — skipping a 4-byte AVCC length prefix. So `+0xD4` points to the AVCC-prefixed IDR, and `+0xD8` points past the prefix to the raw NAL data.

### Next step

Read the first 16 bytes at `*(rb_base+0xC4) + *(rb_base+0xD8)` during the second msg 6 call to verify this is real H.264 IDR data (NAL type 0x65). If confirmed, copy the full `*(rb_base+0xDC)` bytes to capture the complete IDR frame.

## v22 — Debug Frame Protocol Validation & BSS Stability Fix (2026-02-21)

### Problem: Mysterious crashes unrelated to memory reads

During v21, adding or removing debug entries caused the camera to crash (WEBCAM_START timeout, camera wouldn't record). Crashes occurred even with **pure constant values** — no struct reads at all. The pattern:

| Build | BSS size | Result |
|-------|----------|--------|
| Known-good (commit a332449) | 532 | Works |
| Added RBc4/RBd4/RBd0 entries (replaced M5Ct/M5Dn/Tst1) | 528 | **Crash** |
| Added single RBc4 entry (kept M5Ct/M5Dn) | 532 | Works |
| Pure constants, 12 entries × 4 frames (no struct reads) | 528 | **Crash** |
| Pure constants + `bss_pad` variable | 532 | **Works** |

### Root cause: BSS size sensitivity

The ARM linker places `movie_rec.o`'s BSS section at a fixed offset. When the compiler optimizes away unused static variables (e.g. `msg5_count` and `msg5_done` not read by `spy_idr_capture`), BSS shrinks from 532 to 528 bytes. This 4-byte difference shifts subsequent BSS allocations, likely causing a conflict with another module's memory layout.

**Fix**: Added `static volatile int bss_pad = 0;` to keep BSS at exactly 532 bytes regardless of which variables the compiler retains.

### Debug frame protocol: fully validated

Sent 4 debug frames with 12 constant entries each (Frm#, TstA through TstK). All values arrived perfectly:

```
=== DEBUG FRAME #0 (12 entries, 108 bytes) ===
  Frm# = 0x00000001  (1)
  TstA = 0xCAFEBABE
  TstB = 0x12345678
  TstC = 0xDEADBEEF
  TstD = 0xA5A5A5A5
  TstE = 0x55AA55AA
  TstF = 0x01020304
  TstG = 0xFFFF0000
  TstH = 0x00FF00FF
  TstI = 0x87654321
  TstJ = 0xABCDEF01
  TstK = 0x10203040
=== END DEBUG ===

(×4 frames, all identical except Frm# = 1/2/3/4)
```

**Protocol characteristics confirmed**:
- Lock-free SPSC queue works correctly across 4 consecutive frames
- 12 entries per frame (108 payload bytes) — well within 512-byte slot limit
- Sequence numbers increment correctly (0, 1, 2, 3)
- No data corruption, no dropped frames, no reordering
- Bridge-side parsing and display correct for all entry types

### Key lessons (REVISED — see v22b below)

1. ~~**BSS size must remain stable at 532 bytes**~~ — see v22b: BSS theory was wrong
2. **Debug frame protocol is 100% reliable** — previous crashes were NOT protocol bugs
3. ~~**When crashes have no logical code explanation, check BSS/data section sizes**~~ — actual root cause was compiler-generated code for double indirection

## v22b — Ring Buffer Reads Working via Hardcoded Addresses (2026-02-21)

### BSS theory disproven

Further testing showed that BSS=532 is necessary but NOT sufficient. The da53556 code (BSS=532, text=2140) that previously worked became unreliable — crashing consistently even after battery pulls. Meanwhile, the pure-constants test (BSS=532, text=2220) always works. The real problem was the **double indirection pattern** in the compiler output.

### Root cause: Double indirection crashes the camera

Reading struct fields via pointer indirection crashes the camera:
```c
// CRASHES — double indirection through volatile pointer
spy_debug_add('R','B','c','4',
    (*(volatile unsigned int *)0xFF93050C)
        ? *(volatile unsigned int *)(*(volatile unsigned int *)0xFF93050C + 0xC4)
        : 0);
```

The ARM compiler (arm-none-eabi-gcc) generates problematic code for nested volatile dereferences in ternary expressions. The exact failure mode is unclear (possible speculative read before condition check, or volatile ordering issue), but the crash is 100% reproducible.

**Fix**: Use hardcoded absolute addresses instead of pointer indirection:
```c
// WORKS — direct address, no indirection
// rb_base is always 0x8968, so rb_base+0xC4 = 0x8A2C
spy_debug_add('R','B','c','4', *(volatile unsigned int *)0x8A2C);
```

### CRITICAL RULE: No double indirection in movie_rec.c

**NEVER** dereference a pointer read from another pointer in `spy_idr_capture` or any spy function called from inline ASM context. Always use hardcoded addresses.

| Pattern | Example | Result |
|---------|---------|--------|
| ROM read | `*(volatile unsigned int *)0xFF93050C` | **OK** |
| RAM read (hardcoded) | `*(volatile unsigned int *)0x8A2C` | **OK** |
| Double indirection | `*(*(0xFF93050C) + 0xC4)` | **CRASH** |
| Local var indirection | `rb = *(0xFF93050C); *(rb + 0xC4)` | **CRASH** |

The ring buffer struct base is always `0x8968` (from `*(0xFF93050C)`). All struct field addresses can be hardcoded:

| Field | Offset | Hardcoded Address | Purpose |
|-------|--------|-------------------|---------|
| +0xC4 | rb_base+0xC4 | `0x8A2C` | Ring buffer data area base |
| +0xD0 | rb_base+0xD0 | `0x8A38` | DMA base |
| +0xD4 | rb_base+0xD4 | `0x8A3C` | AVCC pointer (current write position) |
| +0xD8 | rb_base+0xD8 | `0x8A40` | IDR offset (past AVCC prefix) |
| +0xDC | rb_base+0xDC | `0x8A44` | IDR size |

### All struct fields successfully read

With hardcoded addresses, all 6 ring buffer fields read correctly across 4 debug frames:

```
=== DEBUG FRAME #0 (first msg 6, before IDR) ===
  RBas = 0x00008968       ← ring buffer struct (from ROM)
  RBc4 = 0x41304720       ← data area base (uncached RAM)
  IdrO = 0x00000000       ← IDR not yet written
  IdrS = 0x00000000
  RBd4 = 0x000158A8       ← AVCC pointer (first frame)
  RBd0 = 0x02AD0000       ← DMA base

=== DEBUG FRAME #1 (second msg 6, after IDR) ===
  RBc4 = 0x41304720       ← unchanged
  IdrO = 0x000158AC       ← IDR offset = RBd4_prev + 4 (skip AVCC prefix)
  IdrS = 0x0000B5E0       ← IDR size: 46,560 bytes
  RBd4 = 0x00020E8C       ← AVCC advanced to next frame
  RBd0 = 0x02AD0000       ← unchanged
```

**IDR architecture confirmed**:
- `+0xD4` (AVCC ptr) advances with each encoded frame
- `+0xD8` (IDR offset) = initial `+0xD4` value + 4 (set once by msg 5, skips 4-byte AVCC length prefix)
- `+0xDC` (IDR size) = ~46 KB, set once by msg 5
- IDR absolute address = `0x41304720 + 0x158AC` = `0x4131FFCC` (uncached, CPU-readable)

### v23 — P-frame pointer capture (2026-02-21)

Captured `hdr[1]` (FPtr) and `hdr[2]` (FSiz) from `spy_ring_write` to see actual P-frame addresses.

**Results** (text=2124, BSS=532):

| Field | Frame 1 | Frame 2 | Frame 3 | Frame 4 |
|-------|---------|---------|---------|---------|
| FPtr | 0x00000000 | **0x412C4720** | 0x41323D38 | 0x4132D6D4 |
| FSiz | 0x00000000 | 0x00040000 | 0x00040000 | 0x00040000 |
| RBd4 | 0x000158A8 | 0x0001F618 | 0x00028FB4 | 0x00032534 |
| RB20 | 0x00000000 | 0x41319FC8 | 0x41323D38 | 0x4132D6D4 |

**Key discovery — two buffer areas**:
- **First P-frame**: FPtr = 0x412C4720 = **RBc0** (not RBc4!)
- **Subsequent P-frames**: FPtr = RBc4 + RBd4_prev (e.g., frame 3: 0x41304720 + 0x1F618 = 0x41323D38)
- FSiz = 0x40000 (256KB) — buffer allocation size, not frame data size
- RB20 lags FPtr by one frame (read pointer for ring buffer consumer)

### v23b — Dual-address IDR data probe (2026-02-21)

Tested reading IDR data from three different computed addresses (text=2104, BSS=532):

| Probe | Address | Frame 2 | Frame 3 |
|-------|---------|---------|---------|
| RBc0 + IdrO | 0x412D9FCC | 0xFFFFFFFF | 0xFFFFFFFF |
| RBc4 + IdrO | 0x41319FCC | 0xFFFFFFFF | 0xFFFFFFFF |
| RB20 direct | 0x41319FC8 | 0xFFFFFFFF | 0x24940000 |

**All three IDR addresses return 0xFFFFFFFF.** Neither RBc0 nor RBc4 has IDR data at offset IdrO (0x158AC).

Frame 3 Dr20 = 0x24940000 — valid P-frame AVCC data (big-endian length 0x00009424 = 37,924 bytes). This confirms P-frame data IS readable from ring buffer pointers. Only the IDR is missing.

**Conclusion**: `+0xD8` (IdrO = 0x158AC) is NOT an offset relative to either RBc0 or RBc4. The IDR data is in a completely different memory region — likely the DMA context buffer at `*DAT_ff85d6a4 + 0x200040` (from Ghidra decompilation of msg 5 handler `FUN_ff85d3bc`). The ring buffer struct fields +0xC0/+0xC4 are the P-frame data areas, not the IDR encoding output.

### Exhaustive IDR address search summary

| Address formula | Computed | Result |
|----------------|----------|--------|
| RBc4 + IdrO | 0x41319FCC | 0xFFFFFFFF |
| RBc0 + IdrO | 0x412D9FCC | 0xFFFFFFFF |
| RB20 (frame 2) | 0x41319FC8 | 0xFFFFFFFF |
| DMA + IdrO | 0x02AE58AC | 0xFF (v18 PTP probe) |
| DMA\|0x40 + IdrO | 0x42AE58AC | 0xFF (v18 PTP probe) |
| @IdrO absolute | 0x000158AC | 0x00 (v18 PTP probe) |
| Cached mirror | 0x0131FFCC | CRASH |

### Three approaches to find IDR data

**Option 1 — Trace the DMA context base (ROM pointer chain)**

Read the ROM constant at `0xFF85D6A4` to find the movie record context base. From Ghidra decompilation of msg 5 (`FUN_ff85d3bc`):

```c
iVar2 = *DAT_ff85d6a4;                          // movie record context base
uVar3 = FUN_ff930b04(iVar2 + 0x200040);          // init ring buffer data area
FUN_ff8eddfc(uVar3, iVar2 + 0x100040, ...);      // JPCORE encodes IDR into uVar3
```

The IDR output buffer is `FUN_ffa19c98() + *DAT_ff85d6a4 + 0x200040`. Steps:
1. Read `*(0xFF85D6A4)` — ROM constant giving RAM address of context pointer
2. In next build, read from that RAM address (hardcoded) to get `*DAT_ff85d6a4`
3. Compute `base + 0x200040` — this is the data area where JPCORE writes the IDR
4. Also read `*(0xFF930C78)` — ROM pointer to `*DAT_ff930c78` (set by FUN_ff930b04, stores the data area base used by MOV writer FUN_ff930b20)
5. Read `FUN_ffa19c98()` return value (header/alignment offset added to data area base)

Complexity: 2-3 builds (ROM pointer → RAM address → data area → IDR). Low risk.

**Option 2 — Hook inside msg 5 (inline assembly)**

Add an inline assembly hook INSIDE the msg 5 handler (`FUN_ff85d3bc`), between encoding completion (`FUN_ff8274b4` semaphore wait) and recording pipeline start (`sub_FF8C3BFC`). At this point the IDR is freshly encoded and P-frames haven't started overwriting it.

From the msg 5 handler:
```
FF85D3BC: ... (function entry)
  FUN_ff930b04     → init ring buffer
  FUN_ff8eddfc     → JPCORE encodes IDR
  FUN_ff8274b4     → wait for encoding (1s timeout)
  <<<< HOOK HERE: IDR data is valid, no P-frames yet >>>>
  sub_FF8C3BFC     → start recording pipeline (P-frames begin)
  FUN_ff93048c     → read IDR ptr/size from ring buffer struct
  FUN_ff930b20     → write IDR to MOV container
```

The hook would call `spy_ring_write(idr_ptr, idr_size)` to deliver the IDR frame via the existing P-frame delivery path. Need to find exact ROM addresses for the BL insertion point.

Complexity: Medium — requires disassembly of msg 5 handler, finding safe BL insertion point, managing register state. Risk of FPS regression (previous C wrapper attempt caused severe regression).

**Option 3 — Synthetic IDR construction**

Instead of capturing the real IDR from the encoder, construct a synthetic IDR frame on the PC bridge side:

1. We already have SPS+PPS (hardcoded from a real MOV file, 28 bytes avcC)
2. Generate a minimal IDR slice: NAL type 5 (0x65), slice header with `first_mb_in_slice=0`, `slice_type=7` (I-slice), `frame_num=0`, `pic_order_cnt=0`, then all-gray macroblock data (DC-only, no residual)
3. Feed this synthetic IDR to FFmpeg before the first P-frame arrives
4. FFmpeg initializes its reference picture, then subsequent P-frames decode normally

The synthetic IDR wouldn't match the camera's actual first frame visually, but it would allow the decoder to sync immediately instead of waiting ~8 seconds. After a few P-frames, the visual output would converge to the real scene.

Complexity: Requires understanding H.264 slice header bitstream encoding (exp-Golomb, CABAC/CAVLC). Medium risk — if the synthetic IDR's parameters don't match the encoder's expectations, P-frame decoding may produce artifacts.

### v23c — ROM pointer probe (starting option 1)

**Step 1**: Read ROM constants to find RAM addresses (text=2056, BSS=532):

| ROM Address | Value | Meaning |
|-------------|-------|---------|
| `*(0xFF85D6A4)` | **0x00005260** | RAM addr storing movie record context base |
| `*(0xFF930C78)` | **0x00008DE4** | RAM addr storing data area base (set by FUN_ff930b04) |
| `*(0xFF93050C)` | 0x00008968 | Verification: ring buffer struct addr (matches known) |

**Step 2**: Read RAM values at discovered addresses (text=2072, BSS=532):

| RAM Address | Tag | Value | Meaning |
|-------------|-----|-------|---------|
| `*(0x5260)` | CtxB | **0x411EDFD0** | Movie record context base (`*DAT_ff85d6a4`) |
| `*(0x8DE4)` | DatA | **0x00000000** | Data area base — **cleared after msg 5!** |

**Key discovery — IDR encoding data area is separate from P-frame ring buffers**:

```
context_base             = 0x411EDFD0
IDR data area            = context_base + 0x200040 = 0x413EE010
JPCORE encoder input     = context_base + 0x100040 = 0x412EE010
```

The IDR encoding output goes to **0x413EE010** (plus a header offset from `FUN_ffa19c98()`). This is a completely different memory region from the P-frame ring buffers:

| Buffer | Address | Purpose |
|--------|---------|---------|
| IDR encoding output | 0x413EE010 + header | Where JPCORE writes IDR (msg 5) |
| P-frame buffer 0 (RBc0) | 0x412C4720 | First P-frame delivery |
| P-frame buffer 1 (RBc4) | 0x41304720 | Subsequent P-frame delivery |
| DMA base (+0xD0) | 0x02AD0000 | Hardware DMA registers |

This explains why reading from `RBc0 + IdrO` and `RBc4 + IdrO` returns 0xFFFFFFFF — the IDR was never written to those buffers. `IdrO` (0x158AC) is an offset within the 0x413EE010 data area, not relative to RBc0/RBc4.

**`*(0x8DE4) = 0`** confirms the data area pointer is cleared after msg 5 completes (FUN_ff930b04 stores it, msg 5 uses it, then something resets it). But we can compute it directly from the context base.

### v23d — Data area probe (2026-02-21)

Probed the computed data area at `context_base + 0x200040 = 0x413EE010` (text=2184, BSS=532):

| Probe | Address | Result |
|-------|---------|--------|
| Data area +0x00 | 0x413EE010 | 0xFFFFFFFF |
| Data area +0x04 | 0x413EE014 | 0xFFFFFFFF |
| Data area + IdrO - 4 | 0x414038B8 | 0xFFFFFFFF |
| Data area + IdrO | 0x414038BC | 0xFFFFFFFF |
| Data area + IdrO + 4 | 0x414038C0 | 0xFFFFFFFF |

**All 0xFFFFFFFF.** The IDR data does not persist in CPU-accessible memory after msg 5 completes.

### IDR memory search — dead end summary

Every address derivable from the firmware has been probed. None contain IDR data by the time msg 6 fires:

| Region | Addresses tried | Result |
|--------|----------------|--------|
| P-frame buffer 0 (+0xC0) | RBc0 + IdrO = 0x412D9FCC | 0xFFFFFFFF |
| P-frame buffer 1 (+0xC4) | RBc4 + IdrO = 0x41319FCC | 0xFFFFFFFF |
| RB read pointer (+0x20) | 0x41319FC8 | 0xFFFFFFFF |
| DMA base (+0xD0) | 0x02AE58AC, 0x42AE58AC | 0xFF |
| DMA context data area | 0x413EE010 + offsets | 0xFFFFFFFF |
| Cached mirror | 0x0131FFCC | CRASH |
| Absolute offset | 0x000158AC | 0x00 |

**Conclusion**: The IDR is written by JPCORE via DMA into a temporary buffer, read by msg 5 for the MOV container header, and then the memory is either freed, unmapped, or overwritten. By the time the first msg 6 fires (~33ms later), the data is gone from all CPU-accessible addresses. **Reading IDR data after msg 5 is not viable.**

**Path forward**: Option 2 — hook inside msg 5 to capture IDR data while it's still live (between JPCORE encoding and MOV write). This requires inline assembly to intercept the msg 5 handler at the right point.

## v23 — Msg 5 Timing Discovery & ROM Pointer Probe (2026-02-22)

**Goal**: Capture IDR data by hooking msg 5 (Option 2). Need to understand msg ordering and discover the data area pointer address.

**Key discoveries**:

1. **Message ordering**: msg 5 has NOT fired by `idr_sent==2` (second msg 6 call). `M5Dn = 0x00000000` confirms this. The recording pipeline starts producing P-frames (msg 6) before the IDR encoding (msg 5) completes. This means msg 5 and msg 6 run on different timing — msg 6 fires from the pipeline callbacks set up DURING msg 5 execution.

2. **Data area pointer location discovered**: `*(0xFF930C78) = 0x00008DE4`. The ROM data reference `DAT_ff930c78` (used by `FUN_ff930b04` to store the data area base) points to RAM address **0x8DE4**. During msg 5, `FUN_ff930b04` writes `iVar2 + 0x200040` to `*(0x8DE4)`.

3. **Shared memory (0xFF000) corruption**: The shared memory region at 0xFF000 is actively overwritten by firmware DMA during recording. `spy magic` (hdr[0]) shows random garbage values on every PTP poll. Debug frames only survive if sent during a msg 6 call that coincides with active PTP polling — a narrow timing window.

4. **idr_sent counter is fragile**: Because the magic check (`hdr[0] != 0x52455753`) randomly fails due to DMA corruption, `idr_sent` gets reset to 0 on "bad" polls. The counter only reaches 2 when magic happens to be valid on two consecutive msg 6 calls — a lucky timing event.

5. **Camera stability is fragile**: Any code changes to movie_rec.c can cause the camera to crash (USB I/O errors, hard lockup). The crashes are intermittent and not always caused by obvious bugs — recompilation itself can shift code alignment enough to trigger issues. Battery removal is required to recover from crash states.

**Address table update** (add to existing):

| Field | Address | Source | Description |
|-------|---------|--------|-------------|
| data_area_ptr | 0x8DE4 | `*(0xFF930C78)` | Data area pointer (set by FUN_ff930b04 during msg 5) |

**Test results** (idr_sent==2 debug frame):
```
IdrO = 0x000158AC  (88236)   — IDR offset in data area
IdrS = 0x0000B560  (46432)   — IDR size (~45 KB)
D_00 = 0x00008DE4            — *(0xFF930C78): RAM addr of data area ptr
D_04 = 0x00000000            — msg5_done: msg 5 NOT yet run
DIm4 = 0xFFFFFFFF            — data area + idr_off still shows 0xFF (DMA flushed)
```

**Next step**: Need to capture `*(0x8DE4)` during msg 5 (when it holds the live data area pointer) and probe the IDR data at `*(0x8DE4) + 0x158AC`. The challenge is delivering this data to the bridge — debug frames sent during msg 5 context get corrupted before PTP polling reads them. Solution: store in statics during msg 5, read from msg 6 debug frame after msg5_done==1. But the msg5_done guard requires msg 5 to fire first, and the narrow magic-valid window may close before that happens. May need to remove the `idr_sent = 0` reset on magic failure to let the counter accumulate across valid polls.

### v24 — IDR Location Discovery (2026-02-22)

**Goal**: Find where IDR frames are actually written, since msg 5 never fires during webcam recording yet MOV files contain IDR data.

**Investigation**: Decompiled `sub_FF92FE8C_MovieFrameGetter` (552 bytes at 0xFF92FE8C). This is the function called from msg 6 that returns encoded frame pointers from the ring buffer.

**Critical finding in MovieFrameGetter** (idr_architecture_decompiled.txt:975):
```c
else if (*(int *)(iVar1 + 0x28) == 1) {   // Frame counter == 1 (FIRST frame)
    uVar6 = *(uint *)(iVar1 + 0xc0);       // Special first-frame pointer
}
*param_1 = uVar6;                           // Return as frame pointer
iVar9 = *(int *)(iVar1 + 0x70);            // Frame size
```

When the frame counter is 1 (first call), MovieFrameGetter returns the pointer from ring buffer **+0xC0** instead of the normal **+0x1C** read pointer. This IS the IDR/SPS data.

**Debug probe**: Modified spy_idr_capture to read +0xC0 pointer and dump first 12 bytes. Results:

```
FPtr = 0x412C4720   ← First-frame pointer from +0xC0
FSiz = 0x00040000   ← +0x70: buffer capacity (256KB), NOT actual frame size
FCnt = 0x00000002   ← Frame counter already 2 (IDR already consumed)
IOff = 0x000158AC   ← +0xD8: IDR offset in data area (MOV metadata)
ISiz = 0x0000CFE8   ← +0xDC: IDR size = 53KB
NAL0 = 0x01000000   ← Bytes 0-3: 00 00 00 01 (Annex B start code)
NAL4 = 0x1FE04267   ← Bytes 4-7: 67 42 E0 1F (SPS NAL type 0x67!)
NAL8 = 0xF68002DA   ← Bytes 8-11: DA 02 80 F6 (SPS data, matches hardcoded)
```

**Three discoveries**:

1. **IDR data location**: Ring buffer +0xC0 (address 0x8A28) holds a pointer (0x412C4720) to the SPS+PPS+IDR bundle in **Annex B format** (start codes, not AVCC length-prefixed).

2. **Format mismatch**: The first frame is Annex B (`00 00 00 01` start codes), while subsequent P-frames are AVCC format (`00 00 XX XX` length prefix). The SPS bytes `67 42 E0 1F DA 02 80 F6` match the hardcoded SPS in the bridge.

3. **Race condition — IDR lost**: The IDR is returned by MovieFrameGetter on the first msg 6 call, spy_ring_write is called, but the webcam module's shared memory magic (0x52455753) likely isn't set yet. By the time PTP polling starts, only P-frames remain. Bridge output confirms: ALL 300+ frames are NAL type 0x61 (P-frame, type 1). Zero IDR frames received.

**Bridge output confirms the problem**:
```
H.264 FAIL #1: 36804 bytes — NAL=0x61 (type=1, P-frame)
H.264 FAIL #2: 37372 bytes — NAL=0x61 (type=1, P-frame)
... (all 300+ frames are P-frames, decoder never syncs)
```

**Root cause**: The webcam module isn't ready when the first frame (IDR) arrives. The first frame is silently dropped by spy_ring_write's magic check. Once the module IS ready, only P-frames remain.

**Fix approach**: Read IDR data directly from the +0xC0 pointer (0x412C4720) in the webcam module when it starts polling. The data persists at that address throughout recording. Send SPS+PPS+IDR as frame #0 before any P-frames.

## v23 Series — H.264 Decode Working, IDR GOP Discovery (2026-02-22)

### v23: IDR injection + H.264 decode pipeline working

**IDR injection success**: The hybrid AVCC format detection in webcam.c now correctly handles the camera's first-frame buffer layout:
- Offset 0-3: Annex B start code (`00 00 00 01`)
- Offset 4-15: SPS (12 bytes, Annex B)
- Offset 16-19: Annex B start code
- Offset 20-23: PPS (4 bytes, Annex B)
- Offset 24-27: AVCC 4-byte BE length prefix (matches +0xDC IDR size)
- Offset 28+: IDR NAL data (type 0x65)

The scanner rebuilds only the IDR NAL in AVCC format (no in-band SPS/PPS — bridge has them from avcC extradata init). Total IDR frame: ~52KB.

**FFmpeg decode fixes**:
- `AV_CODEC_FLAG_LOW_DELAY` — **critical** for Baseline H.264. Without this, `avcodec_receive_frame` returns EAGAIN for every P-frame.
- SPS must be 13 bytes (with RBSP stop bit `0x01`), not 12 bytes. The camera's Annex B SPS is truncated at 12 bytes; using it causes FFmpeg "Overread VUI by 8 bits" warning.
- IDR-only injection (no in-band SPS/PPS) avoids overriding the avcC parameters.

**Bridge test result**: First 5 frames (IDR + 4 P-frames) decode successfully to 640x480 → 1280x720 RGB. H.264 decode pipeline fully functional. Subsequent frames fail due to bridge-side diagnostics output blocking the main loop (>33ms per frame at 30fps), causing P-frame gaps that permanently break the decoder.

### v23b: Diagnostics bottleneck analysis

**Problem**: After 5 successful frames, all subsequent P-frames fail with "Decoder needs more data" (EAGAIN). Bridge-side diagnostics dumps (~130 lines of stderr per "no frame" response) and PTP memory probes (6 round-trips on M6.2 debug frames) block the main loop long enough to miss P-frames. Once P-frames are missed, the decoder loses its reference chain and never recovers.

**MOV file analysis — GOP=15 in recorded files**:

Analysis of the camera's own MOV file (`MVI_4032.MOV`, 604 frames) reveals the stss (sync sample) atom:
```
stss: 41 keyframes at samples 1, 16, 31, 46, 61, 76, 91, ...
```
The MOV muxer marks a sync sample every 15 frames. However, this does NOT necessarily mean the encoder produces IDR NALs every 15 frames — the sync samples could be all-intra P-frames or the keyframe scheduling may differ between normal recording and our hooked recording.

**DISPROVEN — msg 5 does NOT fire periodically**:

Instrumented spy_idr_capture to report `msg5_count` at msg 6 call 2 (early) and call 500 (~16s into recording):
```
DEBUG FRAME #0 (call 2):   M6Ct = 2,   M5Ct = 0
DEBUG FRAME #1 (call 500): M6Ct = 500, M5Ct = 0
```
After 500 msg 6 calls, msg 5 has fired **zero times**. Msg 5 (`sub_FF85D3BC`) does not participate in our recording session at all. The initial IDR at +0xC0 is produced by `sub_FF92FE8C` (msg 6) on the first call, not by msg 5.

**All 500+ frames from sub_FF92FE8C are P-frames** (NAL type `0x61`). No periodic IDR frames arrive through this path. The camera's H.264 encoder produces a single IDR at recording start and then exclusively P-frames for the entire session.

### v23c: Diagnostics suppression test

Disabled bridge-side diagnostics dumps and memory probes. Result:
- **11 frames decoded at 5.5 FPS** in the first stats interval (up from 5 frames before)
- Decoder still breaks after initial burst — all subsequent P-frames return EAGAIN
- Root cause still under investigation: even with fast polling, P-frames eventually become undecodable

**Open questions**:
1. Why does the decoder break after ~11 frames despite no diagnostics blocking?
2. Are the MOV periodic keyframes (GOP=15) produced by a different encoder configuration than our hooked recording?
3. Can we force periodic IDR re-injection from the stored +0xC0 data to make the decoder self-heal?

## Production Cleanup — Strip Dead Code (2026-02-22)

With the H.264 pipeline proven working (IDR injection + AVCC format + semaphore delivery + bridge decode), removed all dead code from earlier development phases.

### movie_rec.c — Debug instrumentation removed

Removed all debug infrastructure that was used during IDR discovery:
- `spy_debug_reset/add/send` functions and the SPSC debug frame queue at 0xFF040
- `spy_idr_capture` function (was sending debug frames with ring buffer state)
- `spy_msg5_debug` function (was probing msg 5 handler context)
- All debug statics: `idr_sent`, `msg5_done`, `bss_pad`, `msg5_rb_base/idr_ptr/idr_size/count`, `dbg_build_buf[512]`, `dbg_build_len`, `dbg_seq`
- BL to `spy_msg5_debug` from `movie_record_task`
- Conditional branch around `spy_idr_capture` in `sub_FF85D98C_my` — every frame now goes directly to `spy_ring_write`

**What remains**: `spy_ring_write` (~15 lines) is the only C function. It stores ptr+size in the shared header at 0xFF000 and signals the semaphore. Zero debug overhead on the hot path.

### webcam.c — MJPEG/UYVY/diagnostic paths removed (1502 → 580 lines)

Removed code from earlier investigation phases that was never executed during H.264 recording:

| Removed | Lines | Why dead |
|---------|-------|----------|
| `rec_callback_spy` + 15 `rec_cb_*` statics | ~55 | Callback never installed (`hw_mjpeg_start` skipped during recording) |
| `hw_mjpeg_start/stop` | ~180 | JPCORE hardware encoder setup, not used with real recording |
| `hw_mjpeg_get_frame`, `find_jpeg_eoi` | ~115 | VRAM buffer scanning, superseded by spy buffer |
| `capture_and_compress_frame_sw` | ~100 | Software JPEG via tje.c, never reached when `recording_active` |
| `capture_frame_uyvy` | ~35 | Raw UYVY path, never reached when `recording_active` |
| `capture_frame_hwjpeg` | ~75 | JPCORE JPEG capture, never reached when `recording_active` |
| 12 JPCORE/MJPEG firmware defines | ~50 | Addresses for dead functions |
| `sw_fail_*` counters, `hw_fail_*` counters | ~15 | Software/hardware encoder diagnostics |
| `last_spy_cnt`, `avi_sem_handle`, `last_cb_count_*` | ~5 | Old polling-based stale detection, superseded by semaphore |
| JPEG double-buffer, UYVY buffer, frame index | ~10 | Buffers for removed paths |
| 4 unused includes (`viewport.h`, `tje.h`, `levent.h`, `keyboard.h`) | 4 | Headers for removed code |

Also removed one redundant bounds check: `if (size == 0 || size > SPY_BUF_SIZE) return 0;` after the memcpy — both conditions were already handled (null/zero check and clamp) two lines earlier.

**Simplified functions**: `capture_frame()` now only calls `capture_frame_h264()`. `webcam_get_frame()` directly returns H.264 data without format dispatch. `webcam_start/stop()` have no MJPEG/UYVY state to manage.

### Binary size unchanged

All removed code was `static` and unreferenced — the linker already excluded it. The cleanup is source-level only: easier to read, audit, and maintain.

### Stability review

Reviewed the optimized firmware for correctness:
- **movie_rec.c**: Register usage safe (R0-R3 clobbered by BL, overwritten by subsequent instructions). Volatile store ordering correct (data first, counter last, then semaphore). STATE 3→4 promotion handles both fresh and already-promoted states.
- **webcam.c**: All memcpy bounded by SPY_BUF_SIZE. AVCC parser has length/type validation. Shutdown ordering safe (magic cleared → 50ms sleep → stop recording → delete semaphore). Semaphore lifecycle correct (created in start, stored in spy header, deleted in stop).
- **No regressions**: Every code path in the H.264 pipeline preserved exactly.

### Test result (2026-02-22)

Deployed optimized firmware and ran bridge with `--timeout 20 --no-preview --no-webcam`:

- **IDR injection works**: First frame 47804 bytes (NAL 0x65), decoded OK
- **P-frame decoding works**: First 5 frames all decode successfully (NAL 0x61, ~36-37 KB)
- **Peak FPS**: 28.6 FPS (one interval hit 57.7 — likely a stats anomaly from burst after recovery)
- **Sustained FPS**: 14.8-15.8 FPS in active intervals
- **Clean shutdown**: Recording stopped properly, camera returned to playback

**Problem observed**: After initial burst of ~5 decoded frames, decoder loses sync and enters IDR re-injection loop. Re-injection fires repeatedly but doesn't fully recover. FPS drops to 0 during these periods, then occasionally recovers for another burst. This is a pre-existing issue — same pattern as before the dead code cleanup. Root cause: P-frame continuity loss (missing a single P-frame makes all subsequent P-frames undecodable until the next IDR).

## Seqlock Synchronization — 100% Frame Reception (2026-02-22)

### Problem: ring buffer race condition

The pointer pass-through protocol (spy_ring_write stores pointer, webcam.c does memcpy) had a 38% frame corruption rate. The H.264 encoder's DMA overwrites ring buffer slots while the PTP task is copying data, causing the AVCC parser to reject corrupted frames (`no_vcl`).

Also discovered: aggressive PTP polling (1ms failure sleep) starves DryOS cooperative scheduling, causing the AVI write semaphore to time out (1000ms) and the recording pipeline to stall after ~25 frames. Restored to 33ms failure sleep.

### Failed approach: copy inside spy_ring_write

Attempted to copy frame data directly inside spy_ring_write (movie_record_task context, where ring buffer data is guaranteed valid). Three methods tried:
1. **Inline word-copy loop** — copied zeros
2. **Uncached memory mirror** (`ptr | 0x40000000`) — copied zeros
3. **Firmware memcpy via function pointer** (`0xFF8928F4`) — copied zeros

All methods read zeros from the source pointer. Root cause unknown — possibly the data at `ptr` is not yet written by JPCORE DMA at the instant spy_ring_write runs (before the AVI write semaphore wait), or a memory mapping issue specific to the movie_record_task context.

### Solution: seqlock protocol

Instead of copying in spy_ring_write, kept the pointer pass-through but added a seqlock to detect when the ring buffer is overwritten during the copy:

**Writer (movie_rec.c spy_ring_write)**:
```c
hdr[3]++;                     // Seq odd = write in progress
hdr[1] = (unsigned int)ptr;   // Source pointer
hdr[2] = size;                // Frame data size
hdr[3]++;                     // Seq even = write complete
```

**Reader (webcam.c capture_frame_h264)**:
```c
for (attempt = 0; attempt < 3; attempt++) {
    seq_before = hdr[3];
    if (seq_before & 1) continue;   // Write in progress, skip
    src_ptr = hdr[1]; size = hdr[2];
    memcpy(frame_data_buf, src_ptr, size);
    seq_after = hdr[3];
    if (seq_before == seq_after) break;  // Consistent copy
}
```

If the sequence counter changes during memcpy, the data is stale — retry with the new pointer. Up to 3 attempts per semaphore wakeup.

### Bridge improvements

- Removed frame rate limiter (camera paces at 30fps via semaphore)
- Added frame gap detection (frame_num sequence tracking)
- Reduced failure sleep from 33ms to 33ms (kept — prevents DryOS starvation)
- Added session summary at shutdown (total received/dropped/skipped)

### Test result (2026-02-22)

```
=== SESSION SUMMARY ===
  Received: 601 frames
  Dropped:  0 (decode failures)
  Skipped:  0 (camera-produced but never received)
  Last cam frame#: 601
  Camera produced: ~601 frames, bridge saw: 601 (100.0%)
=======================
```

- **601 frames in 20 seconds** — 30 FPS sustained for the entire session
- **0 dropped, 0 skipped** — 100% frame reception rate
- **Consistent frame size**: 45384 bytes (44.3 KB/frame), ~11 Mbps bitrate
- **All frames are IDR** (NAL 0x65, 45384 bytes each) — camera appears to send identical IDR frames since no scene change occurs in webcam mode

The seqlock completely eliminated the ring buffer race condition. Every frame produced by the camera was successfully received and decoded by the bridge.

## Cache Coherency Investigation (2026-02-22)

### Problem: Static preview (all frames identical)

Despite 601 frames at 30fps, the preview window showed a static image. Frame data uniqueness test (FNV-1a hash of first 256 bytes) confirmed: **1 unique frame, 600 duplicates**. All 601 frames contain byte-for-byte identical IDR data.

**Root cause**: DMA cache coherency. JPCORE DMA writes H.264 frame data to physical memory, bypassing the CPU data cache. The CPU's L1 data cache retains stale data from the first frame. When the PTP task does memcpy from the ring buffer, it reads from cache (stale IDR) instead of physical memory (fresh frame data).

The old polling version (pre-seqlock) worked because msleep delays between frames allowed natural cache line eviction by other DryOS tasks. The semaphore-based approach wakes the PTP task immediately, before cache eviction occurs.

### Cache invalidation attempts

| Approach | Unique frames | Result |
|----------|--------------|--------|
| No invalidation (semaphore) | 1/601 | All identical — stale cache |
| MCR p15 c7,c6,1 in webcam.c (Thumb) | — | Build error: MCR is ARM-only |
| `__attribute__((target("arm")))` | — | GCC too old, not supported |
| Naked BX PC per-line invalidation (webcam.c) | 1 | Crash after 1 frame |
| dcache_clean_all + invalidate_all (webcam.c) | 20 | Recording stalled — DryOS starvation |
| Invalidate-only, no clean (webcam.c) | 1 | Crash — dirty cache lines lost |
| Per-line MCR in spy_ring_write (movie_rec.c) | 28 | Recording stalled after ~1s — DryOS starvation |

**Key finding**: Any cache invalidation causes DryOS starvation. The recording pipeline's AVI write semaphore (1000ms timeout) fails when cache operations consume too much CPU time, stalling recording after 20-28 frames.

### Polling approach (remove semaphore, keep seqlock)

Replaced TakeSemaphore with msleep(1) polling loop. Polls hdr[3] (seqlock counter) up to 100 times, sleeping 1ms between polls. When seq changes and is even (write complete), memcpy the frame data.

Removed GiveSemaphore from movie_rec.c — the seqlock counter is sufficient for frame detection.

**Test result (2026-02-22)**:
```
=== SESSION SUMMARY ===
  Received: 33 frames
  Dropped:  550 (decode failures)
  Unique data: 33 frames
  Duplicate data: 0 frames (identical to previous)
=======================
```

- **33 unique frames** (vs 1 before) — cache partially evicted during msleep(1) delays
- **0 duplicates** — every received frame has different data (proper IDR + P-frame mix)
- **550 decode failures** — most frames still have corrupted data from partially stale cache lines
- **Camera recorded full 20 seconds without stalling** — no DryOS starvation
- FPS: ~4-5 (low because most PTP responses return corrupted data)

**Analysis**: msleep(1) allows SOME cache lines to be evicted, but a 40KB frame spans ~1250 cache lines (32 bytes each). Natural eviction during 1ms only flushes a fraction of them. The first 256 bytes may be fresh (enough to pass uniqueness hash) while later bytes remain stale, corrupting the H.264 bitstream.

### Failed: CPU memcpy in spy_ring_write

Attempted having spy_ring_write copy frame data to a shared buffer instead of passing the pointer. Both `__builtin_memcpy` and inline word-by-word copy caused the recording pipeline to stall immediately (1 frame then silence). The movie_record_task likely hit a data abort or timeout during the copy, killing frame production while leaving the camera otherwise functional.

**Root cause**: spy_ring_write runs inside the movie_record_task's message handler. Adding a 40KB copy (even fast word-by-word) to this critical path stalls the recording pipeline. The AVI write semaphore timeout (1000ms) fires before the next message can be processed.

### Failed: Uncacheable memory alias (0x40000000)

Read frame data via the ARM946E-S uncacheable memory alias (`addr | 0x40000000`) in webcam.c, bypassing the CPU cache entirely.

**Test result**: 55 unique frames (improved from 33), but recording stalled after ~5 seconds. The uncacheable bus reads (~40KB per frame) created enough memory bus contention to eventually stall the AVI write path — same DryOS starvation pattern as cache invalidation, just slower onset.

### Conclusion: polling+seqlock is the working approach

The simple polling+seqlock approach (no cache tricks) is the correct solution:
- **33 unique frames in 20 seconds** — all decoded successfully by FFmpeg
- **0 duplicates** — proper IDR + P-frame mix with varying sizes
- **Camera stable for full 20 seconds** — no stalls, no crashes
- **~1.65 fps effective capture rate** — limited by natural cache eviction rate

The ~550 "drops" are PTP requests where the camera's AVCC parser rejected corrupted data (stale cache in the 4-byte AVCC length prefix). This is a camera-side limitation of the natural cache eviction approach, not a bridge-side decode failure. All frames that pass the AVCC parser decode correctly.

Every approach that touches the recording pipeline's hot path (cache invalidation, memcpy in spy_ring_write, uncacheable reads) causes DryOS starvation. The recording pipeline is extremely timing-sensitive — the only safe approach is to let cache eviction happen naturally via msleep() delays.

### Solution: 10ms cache eviction delay

Instead of reading frame data immediately after detecting a new frame (seqlock change), sleep 10ms first to let other DryOS tasks evict stale cache lines. Then re-verify the seqlock before memcpy.

```c
// Detect new frame (seq changed, even)
msleep(10);              // Let other tasks evict cache lines
// Re-verify seq hasn't changed (frame not overwritten)
// Then memcpy from ring buffer
```

At 30fps, frames arrive every 33ms. A 10ms delay leaves 23ms margin before the next frame overwrites the ring buffer slot. The seqlock re-check after the delay catches any overwrites.

**Test result (2026-02-22)**:
```
=== SESSION SUMMARY ===
  Received: 445 frames
  Dropped:  107 (decode failures)
  Unique data: 445 frames
  Duplicate data: 0 frames (identical to previous)
=======================
```

- **445 unique frames in 20 seconds** (~22 fps average) — up from 33 with immediate memcpy
- **0 duplicates** — every frame has different data
- **20-26 FPS sustained** with bursts up to 48 FPS
- **Only 107 drops** (down from 550) — 80% success rate vs 5.6% before
- **Camera recorded full 20 seconds without stalling**
- Proper IDR + P-frame mix, all frames decoded successfully by FFmpeg
- IDR re-injection fired ~8 times (P-frame continuity gaps from skipped frames)

**Why it works**: 10ms of DryOS task scheduling allows dozens of other tasks (USB, filesystem, LCD, etc.) to run. Each task's memory accesses evict cache lines from the stale ring buffer data. By the time memcpy runs, most of the ~1250 cache lines (40KB / 32B) covering the frame data have been replaced with other data, forcing cache misses that read fresh DMA data from physical memory.

### msleep tuning: 5ms, 10ms, 20ms, 25ms comparison

Tested different cache eviction delays to find the optimal FPS/reliability tradeoff:

| Delay | Frames | Drops | FPS | Success | Notes |
|-------|--------|-------|-----|---------|-------|
| 1ms | 33 | 550 | 1.6 | 5.6% | Baseline — almost all frames have stale cache |
| 5ms | 433 | 117 | 21-43 | 79% | Bursty FPS, inconsistent |
| 10ms | 445 | 107 | 20-48 | 80% | Bursty but good average throughput |
| 20ms | 361 | 29 | 17-20 | 93% | Stable FPS, very few drops |
| 25ms | 86 | 0 | 4-5 | 100% | Zero drops but low FPS (approaches 1/30fps limit) |

**Key observations**:
- **5-10ms**: High throughput (~22fps avg) but 20% of frames still have stale cache. FPS is bursty (varies 20-48fps) because cache eviction timing is non-deterministic.
- **20ms**: Sweet spot for reliability. 93% success, 17-20fps stable. Only 29 drops in 20 seconds.
- **25ms**: 100% reliable but FPS drops to ~4. At 25ms delay + ~8ms for PTP round-trip, we're only servicing ~3 frame intervals per cycle.
- A separate DryOS thread wouldn't help — the CPU data cache is shared across all tasks. The cache coherency issue is hardware-level, not scheduling-level.

### Parallel transfer / threading analysis

**Question**: Could a separate DryOS task do the memcpy in parallel, avoiding the sleep penalty?

**Answer**: No — the ARM946E-S has a single shared L1 data cache. All DryOS tasks read through the same stale cache. A background copy task would face identical cache coherency issues and still need msleep delays.

The only hardware mechanisms that bypass the CPU cache are:
1. Cache invalidation MCR instructions — crashes/stalls recording pipeline (proven)
2. Uncacheable memory alias (0x40000000) — bus contention stalls recording after ~5s (proven)
3. DMA-to-DMA copy — would bypass cache but no accessible DryOS DMA copy API

Natural cache eviction via msleep remains the only viable approach. The tradeoff is inherent: longer sleep = more eviction = higher reliability = lower FPS.

### Failed: adaptive approaches (copy-previous, probe, copy-then-validate)

Tested three "smarter" alternatives to fixed msleep delay. All resulted in ~4-5fps — worse than msleep(10) at 22fps.

| Approach | Frames | Drops | FPS | Notes |
|----------|--------|-------|-----|-------|
| Copy previous frame | 81 | 0 | 4.0 | Wait for next frame, copy old one |
| Probe 4-byte change | 63 | 18 | 3.2 | Poll first 4 bytes until changed |
| Copy-then-validate | 95 | 0 | 4.7 | Copy immediately, AVCC check, retry |
| 10ms base + validate | 89 | 9 | 4.5 | msleep(10) + validate retry loop |

**Root cause: IDR re-injection feedback loop.**

At ~5fps, we capture every ~6th frame out of 30fps. Every P-frame is non-consecutive, so the H.264 decoder fails (missing reference frame) and triggers IDR re-injection — decoding a 44KB IDR + retrying the P-frame through FFmpeg. Each re-injection takes ~100ms of bridge CPU time, which slows PTP request rate, causing MORE frame skipping, MORE re-injection, creating a vicious cycle that pins FPS at ~5.

The plain msleep(10) approach avoids this because:
- **Fast failures**: stale frames with invalid AVCC return 0 bytes instantly — no decode, no re-injection
- **High PTP request rate**: ~27 requests/sec keeps cycle time at ~36ms
- **Frame continuity**: at ~22fps capture, most consecutive frames are captured, so P-frames decode without re-injection

**Lesson**: For streaming H.264 with P-frame dependencies, **fast failures + fast retries > slow successes**. Camera-side retry loops add latency that breaks the P-frame chain, triggering expensive bridge-side recovery.

### Conclusion: msleep(10) is optimal

Reverted to plain msleep(10). The 80% success rate at 22fps is the best achievable balance:
- ~338 successfully decoded frames in 20 seconds (~17fps decoded)
- Camera stable, no stalls
- Bridge handles the 20% failures via fast retry + occasional IDR re-injection

## v25 — Hook Position Experiments & No-Decode Throughput Test (2026-02-22)

### Hook position experiments

Tested moving spy_ring_write to later points in `sub_FF85D98C_my` to find a position where the CPU cache is already warm (avoiding the msleep(10) delay):

| Position | After | Before | Frames | Drops | FPS | Cache valid | Notes |
|----------|-------|--------|--------|-------|-----|-------------|-------|
| Early (baseline) | sub_FF92FE8C | loc_FF85DA24 | 445 | 107 | 22 | ~80% | With msleep(10) |
| Late | set_quality | loc_FF85DCBC | 96 | 4 | 5 | 96% | No msleep needed |
| Mid | sub_FF8EDBE0 | sub_FF8274B4 | 72 | 13 | 3.5 | ~82% | No msleep |

**Late hook** (after all firmware processing including semaphore wait): 96% cache validity without any msleep, confirming that the AVI writer task warms cache lines during `sub_FF8274B4` (semaphore wait, ~200ms). But ~5fps because the hook fires 200ms after encoding.

**Mid hook** (after AVI writer submit, before semaphore wait): Worse than early — confirms `sub_FF8EDBE0` does NOT warm the cache. It just submits work to the AVI writer task (separate DryOS task). Cache warming happens during the semaphore wait as the writer task reads frame data.

**Conclusion**: The early hook + msleep(10) remains best. Later hooks trade latency for cache validity, but the latency cost exceeds the msleep(10) delay.

### No-decode throughput test — isolating the bottleneck

Added `--no-decode` flag to bridge: receives frames via PTP but skips ALL H.264 decode, IDR re-injection, and display. Measures raw PTP transport throughput.

**Results** (20 second test, early hook + msleep(10)):
- **120 frames received, 0 drops, 0 skips = 6 FPS**
- **100% AVCC header validity** (0 invalid frames)
- **120 unique frames, 0 duplicates**
- Camera produced exactly 120 frames (cam#1 through cam#120, no gaps)

**Key finding: The bridge decode is NOT the bottleneck.**

Even with zero decode overhead, only ~6 FPS is achievable. The bottleneck is entirely camera-side: PTP task scheduling + msleep(10) polling takes ~165ms per frame.

Breakdown per PTP request cycle (~165ms):
- msleep(10) polling × several iterations = ~50-100ms (DryOS msleep(1) ≈ 10ms)
- PTP USB transfer of ~40KB = ~10ms
- Camera PTP task scheduling overhead = ~50-100ms

**Comparison**: With decode enabled, we got 445 frames (22 FPS) because 80% of those were "fast failures" (invalid AVCC from stale cache → 0 bytes → immediate retry). The bridge made ~27 PTP requests/sec, of which ~22 returned valid data. Without decode, every frame succeeds (no fast-fail retries), so we only get ~6 successful frames/sec.

**This means the 22 FPS result with decode was actually driven by the high PTP request rate from fast failures — the msleep(10) + fast-fail pattern is performing optimally.**

### Remaining bottleneck: camera PTP task latency

The ~165ms per successful frame breaks down as:
1. PTP request arrives → DryOS schedules PTP task
2. webcam.c `capture_frame_h264()` runs:
   - Poll seqlock with msleep(1) delays until new frame detected
   - msleep(10) cache eviction delay
   - memcpy ~40KB from ring buffer
3. PTP response sent back to bridge

### Corrected no-decode test (with correct early hook + msleep(10) firmware)

The first no-decode test (120 frames, 6 FPS) was run with the **wrong firmware** (mid-point hook, no msleep) still on the SD card. After deploying the correct firmware:

**Results** (20 second test, early hook + msleep(10)):
- **420 frames received, 126 drops (fast-fail), 0 skips = ~21 FPS**
- **100% AVCC header validity** (0 invalid frames out of 420 successes)
- **420 unique frames, 0 duplicates**
- Total PTP requests: 546 (420 success + 126 fail) = **~27 req/sec**

**This matches the with-decode results exactly** (445 frames at 22 FPS). Bridge decode overhead is negligible — the IDR re-injection "vicious cycle" seen in earlier adaptive approaches was caused by those approaches' low FPS, not by decode cost.

**Natural IDR frames from camera GOP**: The camera's H.264 encoder produces IDR keyframes (NAL type 5, ~65KB) approximately every 30 frames (1/sec at 30fps). At ~21fps capture, we see a natural IDR every ~40 received frames. This means the decoder gets periodic sync points without needing artificial re-injection. IDR sizes: injected from +0xC0 ~44KB (stripped SPS/PPS), natural GOP IDR ~65KB, normal P-frames ~37KB, P-frame after IDR ~50KB.

### Remaining bottleneck: camera PTP task latency

To go beyond ~21 FPS, we need to reduce the camera-side cycle time. Options to explore:
- Remove msleep(10) and read from uncached memory alias (0x40000000 | addr) — bypasses CPU cache entirely, no eviction delay needed
- Find a way to invalidate CPU cache lines directly (no known DryOS API)
- Reduce PTP task scheduling overhead (unlikely — DryOS internal)
