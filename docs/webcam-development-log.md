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
