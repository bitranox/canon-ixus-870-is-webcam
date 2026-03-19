# Architecture

> Back to [README](../README.md)

## System Overview

```
┌──────────────────────────────────────────────────────────────────┐
│  CANON IXUS 870 IS (Digic IV, ARM926EJ-S, DryOS)               │
│                                                                  │
│  CCD ──► ISP ──► JPCORE H.264 encoder (640x480 @ 30fps)        │
│  10MP    Image          │                                        │
│          Signal         ▼                                        │
│          Proc    Ring buffer (256KB slots, AVCC format)          │
│                         │                                        │
│                  spy_ring_write (movie_rec.c)                    │
│                    │ cache invalidate + AVCC parse               │
│                    │ dual-slot seqlock at 0xFF000                │
│                    │ suppresses SD writes (+0x80=0)              │
│                    ▼                                             │
│                  capture_frame_h264 (webcam.c)                   │
│                    │ polls seqlock with msleep(10)               │
│                    │ zero-copy: passes ring buffer ptr to PTP    │
│                    ▼                                             │
│                  PTP opcode 0x9999 (CHDK_GetMJPEGFrame)         │
│                    │ sends H.264 AVCC frame (35-65 KB)          │
│                    │ zoom piggybacked on frame request           │
└────────────────────┼─────────────────────────────────────────────┘
                     │ USB 2.0 High Speed (480 Mbps)
┌────────────────────┼─────────────────────────────────────────────┐
│  PC (Windows)      │                                             │
│                    ▼                                             │
│  chdk-webcam.exe                                                 │
│    PTPClient (libusb-1.0) ── receives H.264 AVCC frames         │
│    H264Decoder (FFmpeg) ──── decodes to YUV420P                  │
│    FrameProcessor ────────── YUV→RGB24, upscale to 1280x720     │
│    PreviewWindow ─────────── Win32 GDI preview + zoom control    │
│    VirtualWebcam ─────────── DirectShow "CHDK Webcam" device     │
└──────────────────────────────────────────────────────────────────┘
```

## Camera Side

The webcam module intercepts H.264 frames from the camera's native video recording pipeline:

1. **Switch to video mode** -- activates the ISP + JPCORE H.264 encoding pipeline at 640x480@30fps.

2. **Start recording** -- `UIFS_StartMovieRecord` triggers the full recording pipeline. The camera produces H.264 NAL units in AVCC format (4-byte big-endian length prefix) into a ring buffer.

3. **Intercept frames** -- `spy_ring_write()` in `movie_rec.c` hooks the msg 6 handler (`sub_FF85D98C`). After each encoded frame, it invalidates the CPU data cache (JPCORE DMA bypasses cache), parses AVCC headers to get actual frame size, and publishes `{pointer, size}` via a dual-slot seqlock at shared memory address `0xFF000`.

4. **Suppress SD writes** -- `spy_ring_write` clears `ring_buf+0x80` (is_open flag), causing `task_MovWrite` to skip file writes while keeping the full pipeline running. Drain mode (`+0x88=1`) prevents MOV file creation at recording start.

5. **Zero-copy delivery** -- `capture_frame_h264()` in `webcam.c` polls the seqlock with `msleep(10)` (DryOS cooperative yield), peeks AVCC headers to determine exact frame size, then passes the ring buffer pointer directly to PTP -- no memcpy, no buffer allocation.

6. **Zoom** -- Zoom requests arrive piggybacked on frame requests (flag 0x4 in PTP param3, delta in param4). The PTP handler spawns a one-shot DryOS task via `CreateTask` that calls `shooting_set_zoom_rel()` -- the PTP handler returns immediately with a frame while the zoom motor runs concurrently.

## PC Side

The bridge decodes H.264 and presents the video:

1. **PTP client** -- polls frames over USB using CHDK opcode `0x9999` (sub-command `GetMJPEGFrame`). Each response contains one H.264 access unit in AVCC format.

2. **H.264 decode** -- FFmpeg `libavcodec` decodes AVCC frames to YUV420P. SPS/PPS are extracted from the camera's first-frame data and prepended as an `avcC` extradata blob. The decoder discards the first ~14 P-frames until the first natural IDR keyframe arrives (~0.5s startup delay).

3. **Frame processing** -- YUV420P is converted to RGB24 and upscaled from 640x480 to 1280x720 using bilinear interpolation.

4. **Preview window** -- Win32 GDI window with `StretchDIBits`. Captures keyboard and mouse wheel input for zoom control.

5. **Virtual webcam** -- DirectShow source filter registered as "CHDK Webcam", visible in all video conferencing apps.

## PTP Protocol

All webcam communication uses CHDK's vendor PTP opcode `0x9999` with sub-command `GetMJPEGFrame` (value 15).

### Start Streaming

```
Command:  opcode=0x9999, param1=15, param2=quality, param3=0x01 (WEBCAM_START)
Response: param1=0 (success), param4=0xBEEF (marker)
```

### Get Frame

```
Command:  opcode=0x9999, param1=15, param2=0, param3=flags, param4=zoom_delta
Response: param1=frame_size, param2=active, param3=gf_rc
Data:     H.264 AVCC frame (4-byte BE length prefix + NAL units)
```

Flags (param3, bitmask): `0x4` = WEBCAM_ZOOM (param4 = signed zoom delta)

### Stop Streaming

```
Command:  opcode=0x9999, param1=15, param2=0, param3=0x02 (WEBCAM_STOP)
Response: param1=0
```

## Dual-Slot Seqlock

The producer (`spy_ring_write` in `movie_rec.c`) alternates frames between two slots at `0xFF000`:
- Slot A: `hdr[1]`=ptr, `hdr[2]`=size, `hdr[3]`=sequence (odd=writing, even=stable)
- Slot B: `hdr[4]`=ptr, `hdr[5]`=size, `hdr[6]`=sequence

The consumer (`webcam.c`) reads whichever slot has a new even sequence number. This lock-free protocol handles the ~33ms frame interval vs ~29ms PTP round-trip without data races. Addresses `hdr[7..11]` must not be written (hardware interference -- IS motor, display).

## H.264 Stream Format

The camera's JPCORE encoder produces H.264 Baseline Profile Level 3.1:
- **SPS**: `67 42 E0 1F DA 02 80 F6 9B 80 80 83 01` (13 bytes)
- **PPS**: `68 CE 3C 80` (4 bytes)
- **GOP**: ~11 frames (1 IDR + ~10 P-frames), IDR every ~0.4s
- **NAL format**: AVCC (4-byte big-endian length prefix) for P-frames, Annex B for first frame

## Zero-Copy Safety

The ring buffer pointer is passed directly from camera RAM to the USB DMA engine -- no intermediate copy. This is safe because:
1. The encoder completes writing before `spy_ring_write` publishes the pointer
2. Ring buffer slots are 256KB -- recycling takes many frames at 30fps
3. The seqlock detects mid-read overwrites (sequence changes during read)
