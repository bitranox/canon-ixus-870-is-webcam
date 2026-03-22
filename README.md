# Canon IXUS 870 IS -- USB Webcam via CHDK

Turn a Canon IXUS 870 IS into a USB webcam with audio, streaming H.264 video at 640x480@30fps + 44.1kHz PCM audio from the camera's microphone. Video is decoded and upscaled to 1280x720 on the PC. Uses a custom CHDK module on the camera and a PC-side bridge application.

**Camera:** Canon IXUS 870 IS / PowerShot SD880 IS / IXY DIGITAL 920 IS
**Firmware:** 1.01a (Digic IV, ARM926EJ-S, DryOS)
**Status:** v36w -- stable video + audio streaming with zoom control

## Performance

### Video

| Metric | Value |
|--------|-------|
| Resolution | 640x480 native, upscaled to 1280x720 |
| Frame rate | 30.0 FPS (matches camera encoder output) |
| Decode rate | 99.2% (14 startup frames discarded before first IDR) |
| Frame size | 35-46 KB typical (H.264), up to ~65 KB (IDR keyframes) |
| PTP round-trip | min 6ms, avg 29ms, max 80ms |
| Heap allocation | 0 bytes (zero-copy from ring buffer to USB) |
| SD card writes | 0 bytes video (0-byte MOV file auto-cleaned) |
| Zoom | Real-time via preview window (+/- keys, mouse wheel), zero artifacts |

### Audio

| Metric | Value |
|--------|-------|
| Sample rate | 44100 Hz |
| Format | Mono 16-bit signed PCM |
| Source | Camera built-in microphone via WM1400 codec |
| Hardware path | Mic -> WM1400 -> I2S -> SSIO DMA -> RAM (0x43DE9FA8) |
| Per-frame payload | 2940 bytes (88200 bytes/sec / 30 fps) |
| Extra PTP round-trips | 0 (audio piggybacked on video frame response) |
| A/V sync | Frame-level (+-33ms), no drift |
| Startup mute | 1.0s (eliminates SSIO DMA initialization cracks) |
| Output options | WASAPI speakers, VB-Audio Virtual Cable, MKV recording |
| Virtual microphone | Via VB-Audio Virtual Cable -- works in Zoom, Teams, OBS |

### General

| Metric | Value |
|--------|-------|
| Streaming duration | Unlimited (auto-power-off disabled) |

## Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│  CANON IXUS 870 IS (Digic IV, ARM926EJ-S, DryOS)                    │
│                                                                      │
│  VIDEO PATH                          AUDIO PATH                      │
│  ──────────                          ──────────                      │
│  CCD ──► ISP ──► JPCORE H.264       Mic ──► WM1400 ──► I2S          │
│  10MP    Image     (640x480@30fps)              │                    │
│          Signal         │                       ▼                    │
│          Proc           ▼              SSIO DMA (0xC0820500)         │
│               Ring buffer (256KB)               │                    │
│                         │                       ▼                    │
│                         │              RAM buffer (0x43DE9FA8)       │
│                         │              88200 bytes/sec (44.1kHz)     │
│                         │                       │                    │
│                  spy_ring_write (movie_rec.c) ◄─┘                    │
│                    │ cache invalidate + AVCC parse                    │
│                    │ dual-slot seqlock at 0xFF000                     │
│                    │ reads 2940 bytes audio/frame to 0xFE000         │
│                    │ +0x80=2: blocks video writes, keeps audio alive  │
│                    ▼                                                  │
│                  PTP opcode 0x9999 (two-part send)                    │
│                    │ 1st: H.264 AVCC frame (35-65 KB, zero-copy)     │
│                    │ 2nd: 2940 bytes PCM audio (from 0xFE000)        │
│                    │ zoom delta piggybacked on request                │
└────────────────────┼──────────────────────────────────────────────────┘
                     │ USB 2.0 High Speed (480 Mbps)
┌────────────────────┼──────────────────────────────────────────────────┐
│  PC (Windows)      │                                                  │
│                    ▼                                                  │
│  chdk-webcam.exe                                                      │
│    PTPClient ─────────── split: video (N-2940 bytes) + audio (2940)   │
│    H264Decoder ──────── FFmpeg libavcodec ──► YUV420P                 │
│    FrameProcessor ───── YUV→RGB24, upscale to 1280x720               │
│    PreviewWindow ────── Win32 GDI preview + zoom control              │
│    VirtualWebcam ────── DirectShow "CHDK Webcam" device               │
│    AudioOutput ──────── WASAPI ──► speakers / VB-Audio Virtual Cable  │
│    AVRecorder ───────── FFmpeg libavformat ──► MKV (video + audio)    │
└───────────────────────────────────────────────────────────────────────┘
```

For detailed architecture documentation, see [Architecture](docs/architecture.md).

## Features

**Video:**
- **30 FPS H.264 streaming** from the camera's native JPCORE encoder
- **Zero-copy frame delivery** -- ring buffer pointer passed directly to USB DMA, 0 bytes heap
- **Real-time zoom control** -- 10 positions (28-112mm equiv.), async DryOS task, zero artifacts
- **Dual-slot seqlock** -- lock-free producer-consumer for reliable frame delivery

**Audio:**
- **44.1 kHz live audio** from camera microphone via SSIO DMA hardware intercept
- **Zero extra round-trips** -- 2940 bytes PCM piggybacked on each video frame (two-part PTP send)
- **Virtual microphone** -- VB-Audio Virtual Cable routes camera mic to Zoom/Teams/OBS
- **WASAPI audio output** -- play camera mic through PC speakers or any audio device
- **Frame-level A/V sync** -- no drift, audio and video delivered atomically per frame

**Output:**
- **DirectShow virtual webcam** -- appears as "CHDK Webcam" in all video apps
- **MKV recording** -- save video + audio to file with `--record` (FFmpeg libavformat)
- **Preview window** -- real-time 1280x720 video with keyboard/mouse zoom control

**Management:**
- **PTP firmware upload** -- deploy over USB without removing the SD card
- **Camera file ops** -- `--ls`, `--delete`, `--download`, `--exec` via CHDK Lua over PTP

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

### Audio in Conferencing Apps (Zoom, Teams, OBS)

The camera captures 44.1kHz mono audio from its built-in microphone. To route it as a virtual microphone for conferencing apps:

**One-time setup:**
1. Download [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (free, ~1MB)
2. Run `VBCABLE_Setup_x64.exe` as Administrator
3. Reboot when prompted

**Stream with audio:**
```
chdk-webcam.exe --audio-device "VB-Audio"
```

**In Zoom/Teams/OBS:**
- Video: select **"CHDK Webcam"**
- Microphone: select **"CABLE Output (VB-Audio Virtual Cable)"**

**Tip:** To monitor the audio yourself, open Windows Sound Settings > CABLE Input > Properties > Listen > check "Listen to this device" and select your speakers.

**Record to file:**
```
chdk-webcam.exe --record output.mkv
```
Saves H.264 video + PCM audio to a single MKV file. Can be combined with `--audio-device`.

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started](docs/getting-started.md) | Setup, build, deploy, run, and troubleshooting |
| [Architecture](docs/architecture.md) | System design, PTP protocol, seqlock, H.264 format, audio capture |
| [Camera & CHDK Reference](docs/camera-and-chdk.md) | Firmware upgrade, CHDK installation, ALT mode |
| [Firmware Reverse Engineering](docs/firmware-reverse-engineering.md) | Ghidra project, memory map, ISP architecture |
| [Development Log](docs/development-log.md) | Full implementation history (v0-v36w) |
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

**Video:**
- **Windows only** -- the bridge uses Win32 GDI and DirectShow (no Linux/macOS support)
- **Single camera model** -- built specifically for the IXUS 870 IS (firmware 1.01a)
- **640x480 native** -- limited by the camera's H.264 encoder; upscaled to 1280x720 on PC
- **~0.5s video startup delay** -- decoder discards P-frames until first natural IDR keyframe

**Audio:**
- **Mono only** -- camera has a single built-in microphone
- **~1s audio startup mute** -- SSIO DMA initialization produces cracks; first 30 frames silenced
- **No native virtual microphone** -- Windows 10/11 requires a kernel-mode driver for DirectShow audio input devices; use [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (free) as workaround
- **0-byte MOV file per session** -- required to initialize the audio pipeline (SSIO DMA only starts with active recording); auto-cleaned at next session start

**General:**
- **~5s shutdown delay** -- firmware MOV finalization ("Daten werden bearbeitet") after stop

## License

This project uses CHDK (GPL) components. Provided for educational and personal use.

## Acknowledgments

- [CHDK project](https://chdk.fandom.com/) -- the foundation that makes this possible
- [Ghidra](https://ghidra-sre.org/) -- firmware reverse engineering
- [FFmpeg](https://ffmpeg.org/) -- H.264 decoding
- [libusb](https://libusb.info/) -- cross-platform USB access
