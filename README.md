# Canon IXUS 870 IS -- USB Webcam via CHDK

Turn a Canon IXUS 870 IS into a USB webcam streaming H.264 video at 640x480@30fps, decoded and upscaled to 1280x720 on the PC. Uses a custom CHDK module on the camera and a PC-side bridge application with a preview window and optional DirectShow virtual webcam device.

**Camera:** Canon IXUS 870 IS / PowerShot SD880 IS / IXY DIGITAL 920 IS
**Firmware:** 1.01a (Digic IV, ARM926EJ-S, DryOS)
**Status:** v36p -- stable video + audio streaming with zoom control

## Performance

| Metric | Value |
|--------|-------|
| Resolution | 640x480 native, upscaled to 1280x720 |
| Frame rate | 30.0 FPS (matches camera encoder output) |
| Decode rate | 99.2% (14 startup frames discarded before first IDR) |
| Frame size | 35-46 KB typical (H.264), up to ~65 KB (IDR keyframes) |
| PTP round-trip | min 6ms, avg 29ms, max 80ms |
| Heap allocation | 0 bytes (zero-copy from ring buffer to USB) |
| SD card writes | 0 bytes video (0-byte MOV file auto-cleaned) |
| Audio | 44100 Hz mono 16-bit PCM from camera microphone |
| Streaming duration | Unlimited (auto-power-off disabled) |
| Zoom | Real-time via preview window (+/- keys, mouse wheel), zero artifacts |

## Architecture

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

For detailed architecture documentation, see [Architecture](docs/architecture.md).

## Features

- **30 FPS H.264 streaming** from the camera's native video encoder
- **44.1 kHz audio** from camera microphone, piggybacked on video frames
- **Zero-copy frame delivery** -- no on-camera memory allocation or copying
- **Real-time zoom control** -- 10 positions (28-112mm), zero artifacts during zoom
- **Record to MKV** -- save video + audio to file with `--record`
- **DirectShow virtual webcam** -- appears as "CHDK Webcam" in Zoom, Teams, OBS
- **Preview window** -- real-time video with zoom input via keyboard and mouse wheel
- **WASAPI audio output** -- play camera mic through PC speakers or virtual audio cable (`--audio-out`)
- **PTP firmware upload** -- deploy new firmware over USB without removing the SD card
- **Camera file management** -- `--ls`, `--delete`, `--download`, `--exec` files on camera via PTP
- **Dual-slot seqlock** -- lock-free producer-consumer protocol for reliable frame delivery

## Pre-built Binaries

Ready-to-use binaries are included in the repository -- no compilation required:

| File | Path | Purpose |
|------|------|---------|
| `chdk-webcam.exe` | [`bridge/build/Release/chdk-webcam.exe`](bridge/build/Release/chdk-webcam.exe) | PC-side bridge application |
| `DISKBOOT.BIN` | [`chdk/bin/DISKBOOT.BIN`](chdk/bin/DISKBOOT.BIN) | CHDK firmware for the camera |
| `webcam.flt` | [`chdk/CHDK/MODULES/webcam.flt`](chdk/CHDK/MODULES/webcam.flt) | Webcam module for CHDK |

## Quick Start

1. **Copy firmware to SD card** -- put `DISKBOOT.BIN` in the SD card root, `webcam.flt` in `CHDK/MODULES/`
2. **Boot camera with CHDK** -- Playback mode > MENU > Settings > UP > Firmware Ver. > FUNC.SET
3. **Install USB driver** (first time) -- use [Zadig](https://zadig.akeo.ie/) to set "Canon Digital Camera" to libusb-win32
4. **Stream** -- run `chdk-webcam.exe` -- the camera appears as "CHDK Webcam"

To build from source, see **[Getting Started](docs/getting-started.md)**.

### Zoom Control

When the preview window is focused:
- **Keyboard:** `+` / `-` keys (or numpad `+` / `-`) to zoom in/out
- **Mouse wheel:** scroll up to zoom in, scroll down to zoom out

### Audio in Conferencing Apps

The camera captures 44.1kHz mono audio from its built-in microphone. To use it in Zoom, Teams, or OBS:

1. Install [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (free)
2. Run: `chdk-webcam.exe --audio-out`
3. In the conferencing app: select **"CHDK Webcam"** for video, **"CABLE Output"** for microphone

To record video + audio to a file: `chdk-webcam.exe --record output.mkv`

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started](docs/getting-started.md) | Setup, build, deploy, run, and troubleshooting |
| [Architecture](docs/architecture.md) | System design, PTP protocol, seqlock, H.264 format |
| [Camera & CHDK Reference](docs/camera-and-chdk.md) | Firmware upgrade, CHDK installation, ALT mode |
| [Firmware Reverse Engineering](docs/firmware-reverse-engineering.md) | Ghidra project, memory map, ISP architecture |
| [Development Log](docs/development-log.md) | Full implementation history (v0-v35e) |
| [Debug Frame Protocol](docs/debug-frame-protocol.md) | Camera-to-bridge debug channel (disabled during streaming) |
| [Proven Facts](docs/proven-facts.md) | Verified addresses, data formats, constraints (32 facts) |
| [Changelog](CHANGELOG.md) | Version history |

## Camera Specifications

| Spec | Value |
|------|-------|
| Model | Canon IXUS 870 IS (P-ID: 3196) |
| Also known as | PowerShot SD880 IS / IXY DIGITAL 920 IS |
| Processor | Digic IV (ARM926EJ-S) |
| OS | DryOS |
| Sensor | 10.0 MP, 1/2.3" CCD |
| Lens | 4x zoom (28-112mm equiv.), F2.8-5.8, Optical IS |
| Video | 640x480@30fps MOV (H.264 Baseline + Linear PCM) |
| USB | Mini-B, USB 2.0 High Speed |
| Battery | NB-5L lithium-ion |
| Released | 2008 |

## Known Limitations

- **Windows only** -- the bridge uses Win32 GDI and DirectShow (no Linux/macOS support)
- **Single camera model** -- built specifically for the IXUS 870 IS (firmware 1.01a)
- **640x480 native** -- limited by the camera's video encoder; upscaled to 1280x720 on PC
- **~1s startup delay** -- H.264 decoder needs first IDR; audio muted for 1s (startup cracks)
- **~5s shutdown delay** -- firmware MOV finalization ("Daten werden bearbeitet")
- **0-byte MOV file** -- created per session for audio pipeline init, auto-cleaned at next start

## License

This project uses CHDK (GPL) components. Provided for educational and personal use.

## Acknowledgments

- [CHDK project](https://chdk.fandom.com/) -- the foundation that makes this possible
- [Ghidra](https://ghidra-sre.org/) -- firmware reverse engineering
- [FFmpeg](https://ffmpeg.org/) -- H.264 decoding
- [libusb](https://libusb.info/) -- cross-platform USB access
