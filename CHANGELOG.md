# Changelog

All notable changes to this project will be documented in this file.

## [0.3.0] - 2026-03-22

### Summary

Audio capture from camera microphone + recording to MKV + bridge file management. Audio is piggybacked on video frames via a two-part PTP send — zero extra round-trips, zero-copy video preserved.

### Added

- **Audio capture** — 44100 Hz mono 16-bit PCM from camera microphone via SSIO DMA intercept
- **+0x80=2 trick** — blocks video SD writes while keeping audio pipeline alive
- **msg 8 intercept** — captures audio DMA buffer pointer for per-frame PCM reading (2940 bytes/frame)
- **Two-part PTP send** — video (zero-copy from ring buffer) then audio (from shared memory) in one data phase
- **`--record FILE`** — save video + audio to MKV file via FFmpeg libavformat
- **`--audio-out`** — play camera audio through PC speakers via WASAPI
- **`--ls PATH`** — list directory on camera via CHDK Lua
- **`--delete PATH`** — delete file on camera via CHDK Lua
- **`--download REMOTE LOCAL`** — download file from camera via CHDK Lua
- **`--exec SCRIPT`** — execute arbitrary Lua on camera, read result
- **Auto MOV cleanup** — deletes leftover 0-byte MOV files at session start with camera reboot
- **1-second audio mute** — eliminates startup cracks from SSIO DMA initialization

### Fixed

- **execute_script** — transaction ID mismatch and missing null terminator
- **read_script_msg** — CHDK returns raw data (no header on this camera)
- **Beeping on stop** — restore +0x80=1 before StopMovieRecord

### Technical

- SSIO DMA hardware path: Mic → WM1400 → I2S → 0xC0220088 → SSIO DMA (0xC0820500) → RAM buffer (0x43DE9FA8)
- Audio format: 44100 Hz, mono, 16-bit signed PCM, 88200 bytes per 1-second chunk
- Minimal SD writes: 0-byte MOV file created for audio pipeline init, auto-cleaned
- ISP color shift: FAT writes before recording corrupt display; fixed via delete+reboot sequence

## [0.2.0] - 2026-03-16

### Summary

Zoom control from the preview window and async DryOS task architecture. Zero artifacts during zoom — the camera's lens motor runs concurrently with frame delivery.

### Added

- **Zoom control** — +/- keys and mouse wheel in the preview window zoom the camera in/out (10 positions, 28–112mm equiv.)
- **Async zoom via DryOS task** — `CreateTask("ZoomTask")` runs `shooting_set_zoom_rel()` in a separate task so the PTP handler is never blocked by lens motor movement
- **WEBCAM_ZOOM flag (0x4)** — zoom delta piggybacked on frame requests (param3 flags, param4 delta) — zero extra PTP round-trips

### Changed

- Preview window now captures `WM_KEYDOWN` and `WM_MOUSEWHEEL` for zoom input
- `PTPClient::zoom()` accumulates pending delta; `get_frame()` piggybacks it on next frame request
- PTP protocol param3 is now a bitmask (START=0x1, STOP=0x2, ZOOM=0x4)

## [0.1.0] - 2026-03-16

### Summary

Complete rewrite from raw UYVY streaming (~5 FPS) to H.264 interception at full 30 FPS with zero-copy delivery. The camera's native H.264 encoder output is captured directly from the recording pipeline ring buffer without any on-camera processing overhead.

### Camera Module (webcam.flt)

- **H.264 frame interception** — `spy_ring_write()` hooks the msg 6 handler in `movie_rec.c` to capture encoded frames from the JPCORE recording pipeline
- **Dual-slot seqlock** — lock-free frame delivery via alternating slots at 0xFF000 (hdr[1..3] / hdr[4..6]), safe on single-core ARM with DMA
- **Zero-copy** — ring buffer pointer passed directly to PTP, no memcpy or buffer allocation (0 bytes heap)
- **AVCC parsing** — producer and consumer both parse 4-byte big-endian NAL length headers to determine exact frame size (vs 256KB chunk size from MovieFrameGetter)
- **SD write suppression** — clears ring_buf+0x80 (is_open) to skip file writes; drain mode (+0x88=1) prevents MOV file creation
- **CPU cache management** — `spy_ring_write` invalidates D-cache for frame data (JPCORE DMA bypasses CPU cache)
- **Error path bypass** — skips sub_FF930358 + STATE=1 when webcam is active, preventing permanent pipeline death from JPCORE timeout false positives
- **Debug frame protocol** — tagged key-value diagnostic frames via SPSC queue at 0xFF040 (disabled during streaming to avoid artifacts)

### PC Bridge (chdk-webcam.exe)

- **H.264 decoder** — FFmpeg libavcodec, SPS+PPS from camera's first-frame avcC blob, handles AVCC format
- **1280x720 upscaling** — bilinear interpolation from native 640x480, preview window matches output resolution
- **PTP file upload** — `--upload LOCAL REMOTE` deploys firmware to camera SD card over USB, `--reboot` reloads
- **Debug mode** — `--debug` enables per-frame CSV logging (NAL type, size, decode result, RTT)
- **Frame dump** — `--dump-frames DIR` saves raw H.264 frames for offline analysis
- **Timeout** — `--timeout N` for graceful shutdown (ensures `stop_webcam()` runs on camera)

### Performance (v35e)

| Metric | Before (v0.0.1) | After (v0.1.0+) |
|--------|-----------------|------------------|
| Frame rate | ~5 FPS | 30.0 FPS |
| Frame format | Raw UYVY (614 KB) | H.264 (35-65 KB) |
| Decode rate | 100% | 99.2% |
| Output resolution | 640x480 | 1280x720 |
| Heap allocation | 614 KB | 0 bytes |
| Zoom | Not available | Real-time, zero artifacts |

### Firmware Reverse Engineering (continued)

- **Ring buffer struct** at 0x8968 — 20+ offsets documented (+0x1C read ptr, +0x48 fd, +0x50 filename, +0x70 capacity, +0x80 is_open, +0x88 drain, +0xC0 first-frame ptr)
- **H.264 stream parameters** — SPS/PPS extracted from MOV avcC atom, GOP ~11 frames, IDR every ~0.4s
- **task_MovWrite** decompiled — drain mode mechanism, file write suppression via +0x80 flag
- **JPCORE semaphore chain** — full trace from encode submission through hardware interrupt to completion callback
- **USB subsystem** (DiUSB20) — 272 functions decompiled, confirmed native UVC is infeasible (no ISO endpoints, ROM descriptors, hardcoded PTP class requests)
- **32 proven facts** documented with evidence — addresses, formats, constraints, failed approaches

### Known Limitations

- Resolution fixed at 640x480 (camera H.264 encoder native output)
- ~0.5s startup delay (decoder discards P-frames until first natural IDR keyframe)
- Windows only (DirectShow bridge) — Linux v4l2loopback port would be straightforward
- Firmware addresses hardcoded for IXUS 870 IS fw 1.01a
- Maximum 2 seqlock slots (hdr[7..11] at 0xFF000 causes hardware interference)

## [0.0.1] - 2026-02-15

### Summary

First stable release. The Canon IXUS 870 IS streams 640x480 raw UYVY video at ~4.9 FPS over USB to a PC-side bridge that presents a DirectShow virtual webcam device.

### Camera Module (webcam.flt)

- **Raw UYVY streaming** — captures uncompressed 640x480 frames directly from the Digic IV ISP recording pipeline DMA buffers, bypassing all on-camera encoding
- **Pipeline callback spy** — installs a callback at `state[+0x114]` in the firmware's recording pipeline to intercept frame buffer addresses at 30fps
- **Video mode activation** — switches camera from Playback/PTP to Video Recording mode, forces `state[+0xD4]=2` for recording path dispatch
- **JPCORE power management** — powers on JPCORE hardware block (required for recording pipeline callbacks to fire, even though JPEG output is unused)
- **Auto-power-off disabled** — prevents camera shutdown during streaming via `disable_shutdown()`
- **Software JPEG fallback** — Tiny JPEG Encoder (tje.c) for UYVY-to-JPEG conversion at ~1.3 FPS when raw path is unavailable
- **PTP integration** — opcode 0x9999 sub-command 15 (GetMJPEGFrame) with start/stop/get_frame multiplexing and format encoding in param4 high byte

### PC Bridge (chdk-webcam.exe)

- **PTP client** — libusb-1.0 based Canon PTP/CHDK protocol implementation with session management
- **UYVY-to-RGB conversion** — BT.601 fixed-point color conversion with Digic IV signed chroma handling (int8_t, not uint8_t-128)
- **DirectShow virtual webcam** — "CHDK Webcam" source filter visible in Zoom, Teams, OBS, and all DirectShow-compatible apps
- **Preview window** — Win32 GDI live preview at native 640x480
- **Automatic format detection** — handles both UYVY (raw) and JPEG frames based on param4 format byte
- **Diagnostic output** — pipeline state dump on start, per-second FPS/bitrate/dropped frame statistics
- **CLI options** — quality, flip-h/v, no-webcam, no-preview, verbose modes

### Firmware Reverse Engineering

- **Ghidra project** — fully analyzed IXUS 870 IS firmware 1.01a (ARM:LE:32:v5t, base 0xFF810000)
- **~40 firmware functions decompiled** — video pipeline, JPCORE encoder, ISP routing, DMA management, power control, state machine
- **20+ Ghidra scripts** — Java-based headless decompilation scripts for targeted function analysis
- **Key discoveries:**
  - Recording pipeline state structure at RAM 0x70D8 with ~20 documented offsets
  - JPCORE hardware encoder state at RAM 0x2554 and buffer array at 0x2580
  - ISP routing registers (0xC0F110C4) and their write-only behavior with shadow copies at 0x340000+
  - PipelineFrameCallback → FrameProcessing dispatch controlled by state[+0xD4]
  - Triple DMA ring buffer rotation (0x40BAADD0, 0x40C7DCD0, 0x40D50BD0)
  - Digic IV signed chroma encoding (non-standard UYVY format)
  - IXUS 870 IS uses H.264 (MOV) for video, not MJPEG — StartMjpegMaking functions are legacy/unused for encoding on this generation

### Known Limitations

- Frame rate limited to ~5 FPS by USB 2.0 bulk transfer of 614KB/frame
- Resolution fixed at 640x480 (camera video mode native output)
- Windows only (DirectShow bridge) — Linux v4l2loopback port would be straightforward
- Firmware addresses hardcoded for IXUS 870 IS fw 1.01a
- Camera LCD may show artifacts during streaming (recording path overrides display pipeline)
