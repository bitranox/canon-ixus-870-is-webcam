# Getting Started

> Back to [README](../README.md)

Complete setup, build, deploy, and run instructions for the Canon IXUS 870 IS webcam project.

## Prerequisites

### Hardware

| Item | Notes |
|------|-------|
| Canon IXUS 870 IS | Also sold as PowerShot SD880 IS / IXY DIGITAL 920 IS |
| SD card | Any size, FAT16 or FAT32. Must be **unlocked** for CHDK manual boot. |
| USB Mini-B cable | Camera to PC connection |
| NB-5L battery | Fully charged before firmware updates |

### Software

| Tool | Version | Download |
|------|---------|----------|
| Docker Desktop | 29.2+ | https://www.docker.com/products/docker-desktop/ |
| Git for Windows | 2.40+ | https://gitforwindows.org/ |
| Visual Studio 2022 Build Tools | 17.x | https://visualstudio.microsoft.com/downloads/ (select "Desktop development with C++") |
| vcpkg | latest | https://github.com/microsoft/vcpkg |
| Zadig | 2.8+ | https://zadig.akeo.ie/ |
| 7-Zip | any | https://7-zip.org/ (for firmware extraction) |

**Optional (firmware analysis):**

| Tool | Version | Download |
|------|---------|----------|
| JDK 21 (Adoptium Temurin) | 21.x | https://adoptium.net/ |
| Ghidra | 12.x | https://ghidra-sre.org/ |

## Development Environment

Verified tool versions and paths (checked 2026-03-16):

| Tool | Version | Path | Status |
|------|---------|------|--------|
| Docker Desktop | 29.2.0 | (in PATH) | Installed |
| Git for Windows | 2.53.0 | (in PATH) | Installed |
| Visual Studio 2022 | BuildTools | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\` | Installed |
| Visual Studio 2019 | Community + BuildTools | `C:\Program Files (x86)\Microsoft Visual Studio\2019\` | Installed |
| CMake (VS 2022-bundled) | 3.x | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | Not in PATH -- use full path |
| MSVC C++ Toolset | 14.x | (inside VS 2022) | Installed |
| vcpkg | 2025-12-16 | `C:\vcpkg\vcpkg.exe` | Installed |
| 7-Zip | 25.01 | `C:\Program Files\7-Zip\7z.exe` | Installed |
| libusb | 1.0.29#1 | vcpkg (x64-windows) | Installed |
| libjpeg-turbo | 3.1.3 | vcpkg (x64-windows) | Installed |
| FFmpeg | 8.0.1 | vcpkg (x64-windows) | Installed (H.264 decode) |

**Firmware reverse engineering tools:**

| Tool | Version | Path | Status |
|------|---------|------|--------|
| JDK 21 (Adoptium Temurin) | 21.0.10 | `C:\Program Files\Eclipse Adoptium\jdk-21.0.10.13-hotspot` | Installed |
| Ghidra | 12.0.2 | `C:\ghidra_12.0.2_PUBLIC` | Installed |

**Not installed (optional):**

| Tool | Purpose |
|------|---------|
| Wireshark + USBPcap | USB packet capture & debugging |
| softcam | Virtual webcam DirectShow filter (alternative to built-in) |

## Installation

### 1. Clone the repository

```bash
git clone <repo-url> C:\projects\ixus870IS
cd C:\projects\ixus870IS
```

### 2. Install vcpkg (if not already installed)

```batch
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

### 3. Install C++ dependencies

```batch
C:\vcpkg\vcpkg.exe install libusb:x64-windows libjpeg-turbo:x64-windows ffmpeg[avcodec,swscale]:x64-windows
```

Or using the project manifest:

```batch
cd C:\projects\ixus870IS\bridge
C:\vcpkg\vcpkg.exe install
```

Installed packages:

| Package | Version | Purpose |
|---------|---------|---------|
| libusb | 1.0.x | USB communication (PTP protocol) |
| ffmpeg | 8.x | H.264 decoding (libavcodec, libswscale) |
| libjpeg-turbo | 3.x | JPEG decode (legacy path) |

### 4. Build the Docker image for CHDK cross-compilation

The CHDK source tree needs an ARM cross-compiler. A Docker image named `chdkbuild` provides this:

```bash
cd C:\projects\ixus870IS\chdk
docker build -t chdkbuild .
```

> **Note:** Docker Desktop takes ~2 minutes to start after launch. Wait until `docker info` succeeds before running builds.

### 5. Install the USB driver (first time only)

The camera must use the `libusb-win32` driver instead of the default Canon PTP/WIA driver:

1. Connect the camera via USB
2. Power on the camera, load CHDK
3. Open [Zadig](https://zadig.akeo.ie/)
4. Select **"Canon Digital Camera"** from the device list
5. Set the target driver to **libusb-win32**
6. Click **Replace Driver**

This only needs to be done once per PC. To revert, use Device Manager to reinstall the original Canon driver.

## Building

### CHDK (camera firmware)

**Start Docker Desktop first** if it isn't running:
```batch
"C:\Program Files\Docker\Docker\Docker Desktop.exe"
```

Wait until `docker info` succeeds, then build:

```bash
docker run --rm -v "C:\projects\ixus870IS\chdk:/srv/src" chdkbuild make PLATFORM=ixus870_sd880 PLATFORMSUB=101a fir
```

**Output:**
- `chdk/bin/DISKBOOT.BIN` -- main CHDK firmware (~130 KB)
- `chdk/CHDK/MODULES/webcam.flt` -- webcam module (~8 KB)

**Build parameters:**

| Parameter | Value | Notes |
|-----------|-------|-------|
| `PLATFORM` | `ixus870_sd880` | Camera model directory |
| `PLATFORMSUB` | `101a` | Firmware version subdirectory |

Other valid `PLATFORMSUB` values: `100e` (fw 1.00e), `102b` (fw 1.02b). The webcam module is developed and tested against `101a`.

**Clean build:**
```bash
docker run --rm -v "C:\projects\ixus870IS\chdk:/srv/src" chdkbuild make PLATFORM=ixus870_sd880 PLATFORMSUB=101a clean
```

### Bridge (PC application)

CMake is not in PATH on this system -- use the full path from the Visual Studio installation:

```batch
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
```

**Configure** (only needed once, or after `CMakeLists.txt` changes):

```batch
%CMAKE% -B C:\projects\ixus870IS\bridge\build -S C:\projects\ixus870IS\bridge -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Build:**

```batch
%CMAKE% --build C:\projects\ixus870IS\bridge\build --config Release
```

**Output:** `bridge/build/Release/chdk-webcam.exe`

**Dependencies linked:**

| Library | CMake define | Required |
|---------|-------------|----------|
| libusb-1.0 | (always) | Yes |
| FFmpeg (avcodec, avutil, swscale) | `HAS_FFMPEG` | Yes (H.264 decode) |
| libjpeg-turbo / turbojpeg | `HAS_TURBOJPEG` | Optional (legacy JPEG path) |
| softcam | (auto-detect) | Optional (virtual webcam) |
| ole32, strmiids | (Win32 always) | Yes (DirectShow) |

## Deploying to the Camera

### Option A: PTP upload (recommended)

Upload directly over USB without removing the SD card. Camera must be powered on with CHDK running.

```batch
bridge\build\Release\chdk-webcam.exe ^
    --upload chdk\bin\DISKBOOT.BIN A/DISKBOOT.BIN ^
    --upload chdk\CHDK\MODULES\webcam.flt A/CHDK/MODULES/webcam.flt ^
    --reboot
```

- `--upload LOCAL REMOTE` copies a local file to the camera's SD card. Remote paths use `A/` prefix for the SD root.
- `--reboot` reboots the camera after upload so the new firmware is loaded.
- Wait ~10 seconds after reboot before starting the bridge.

**Which files to upload after code changes:**

| Changed file | Upload |
|-------------|--------|
| `movie_rec.c` | `DISKBOOT.BIN` (contains the recording hook) |
| `webcam.c` | `webcam.flt` (CHDK module) |
| `ptp.c` or `ptp.h` | `DISKBOOT.BIN` (part of core firmware) |
| Bridge source (`main.cpp`, etc.) | Nothing -- just rebuild the bridge |

> **Important:** If `movie_rec.c` changed, you MUST upload `DISKBOOT.BIN`. Uploading only `webcam.flt` leaves the old producer running with the new consumer.

### Option B: SD card

1. Power off the camera, remove the SD card
2. Copy `chdk/bin/DISKBOOT.BIN` to the SD card root
3. Copy `chdk/CHDK/MODULES/webcam.flt` to `CHDK/MODULES/` on the SD card
4. Reinsert the SD card (must be **unlocked**)
5. Boot the camera in **Playback mode**
6. Load CHDK: **MENU > Settings > (press UP) > Firmware Ver. > FUNC.SET**

## Running

### Basic usage

```batch
:: Preview window + virtual webcam
bridge\build\Release\chdk-webcam.exe

:: Preview only, no virtual webcam
bridge\build\Release\chdk-webcam.exe --no-webcam

:: Preview with zoom control, 60s timeout
bridge\build\Release\chdk-webcam.exe --no-webcam --timeout 60
```

The camera appears as **"CHDK Webcam"** in Zoom, Teams, OBS, and any DirectShow-compatible application.

### Development / debugging

```batch
:: Per-frame debug logging, 10s timeout, no preview or webcam
bridge\build\Release\chdk-webcam.exe --debug --timeout 10 --no-preview --no-webcam

:: Dump raw H.264 frames to disk for analysis
bridge\build\Release\chdk-webcam.exe --dump-frames C:\temp\frames --timeout 10
```

Always use `--timeout` during development -- without it, killing the bridge skips `stop_webcam()` cleanup and the camera keeps recording indefinitely.

### Zoom control

When the preview window is focused:
- **+** / **-** keys (or numpad) -- zoom in / out
- **Mouse wheel** -- scroll to zoom

The IXUS 870 IS has 10 zoom positions (28-112mm equivalent). Zoom runs asynchronously in a separate DryOS task -- zero frame drops or artifacts during zoom.

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `-q, --quality N` | 50 | JPEG quality on camera (1-100) |
| `-w, --width N` | 1280 | Output width |
| `-h, --height N` | 720 | Output height |
| `-f, --fps N` | 30 | Target FPS |
| `--flip-h` | off | Mirror horizontally |
| `--flip-v` | off | Flip vertically |
| `--no-webcam` | off | Skip DirectShow virtual webcam |
| `--no-preview` | off | Skip preview window |
| `--no-decode` | off | Skip H.264 decode (measure raw PTP throughput) |
| `--verbose` | off | Per-frame statistics |
| `--debug` | off | Per-frame CSV debug logging to stderr |
| `--timeout N` | none | Exit after N seconds (graceful shutdown) |
| `--dump-frames DIR` | none | Save raw H.264 frames to DIR |
| `--record FILE` | none | Record video + audio to MKV file |
| `--audio-out` | off | Play camera audio through PC speakers (WASAPI) |
| `--upload LOCAL REMOTE` | none | Upload file to camera SD card (repeatable) |
| `--download REMOTE LOCAL` | none | Download file from camera |
| `--delete PATH` | none | Delete file on camera |
| `--ls PATH` | none | List directory on camera |
| `--exec SCRIPT` | none | Execute Lua script on camera |
| `--reboot` | off | Reboot camera after upload |

## Development Workflow

### Typical iteration cycle

1. Edit camera-side code (`movie_rec.c`, `webcam.c`, `ptp.c`)
2. Build CHDK: `docker run --rm -v "..." chdkbuild make ...`
3. Build bridge (if changed): `cmake --build ...`
4. Upload to camera: `chdk-webcam.exe --upload ... --reboot`
5. Wait ~10 seconds for reboot
6. Test: `chdk-webcam.exe --debug --timeout 10 --no-webcam`
7. Check results, document findings
8. Commit

### Key source files

| File | Role |
|------|------|
| `chdk/platform/ixus870_sd880/sub/101a/movie_rec.c` | **Producer** -- hooks into the H.264 recording pipeline, publishes frames via seqlock |
| `chdk/modules/webcam.c` | **Consumer** -- polls seqlock, passes frames to PTP (zero-copy) |
| `chdk/core/ptp.c` | PTP command handler -- start/stop/get_frame/zoom |
| `bridge/src/main.cpp` | Bridge main loop -- PTP polling, decode, display, zoom |
| `bridge/src/ptp/ptp_client.cpp` | USB PTP client -- libusb, frame retrieval, zoom, upload |
| `bridge/src/webcam/h264_decoder.cpp` | FFmpeg H.264 decoder |
| `bridge/src/webcam/preview_window.cpp` | Win32 GDI preview + zoom input |

### Camera-side coding constraints

These are hard-won lessons from extensive testing. Violating them crashes the camera.

| Constraint | Consequence of violation |
|-----------|------------------------|
| No double pointer dereference in `movie_rec.c` (e.g., `*(*(addr) + offset)`) | ARM compiler generates code that crashes the camera |
| Only write to seqlock slots hdr[0..6] and hdr[12..13] at 0xFF000 | hdr[7..11] causes dark screen, IS motor clicking, or immediate crash |
| Always `msleep(10)` after detecting a new frame in the polling loop | Without yield, DryOS starves the recording pipeline: 48.8% decode, USB crash |
| Never send debug frames during streaming | Each debug frame steals a PTP round-trip, causing H.264 decoder artifacts |
| Max DryOS heap allocation: ~130 KB | 192 KB total causes dark screen (ISP/IS motor starved) |
| Long-running operations (zoom, focus) must run in a separate DryOS task | Blocking the PTP handler prevents frame retrieval -- missed frames -- artifacts |

### Documentation

Two documentation files must be kept updated:

- **`docs/development-log.md`** -- Chronological history. Append a new section for each test/iteration. Include what was tried, raw test output, failures, and reasoning.
- **`docs/proven-facts.md`** -- Only verified, 100% confirmed facts. No history, no speculation. Update when a test proves something new. Remove entries if disproven.

Read `docs/proven-facts.md` at the start of each session to understand the current state.

## Camera Setup (First Time)

### Firmware version check

1. Power on the camera
2. **MENU > Settings > (press UP) > Firmware Ver.**
3. Current firmware is displayed (e.g., "GM1.00E")

### Firmware upgrade to 1.01a (recommended)

1. Download the Canon firmware update: search for `pssd880is1010.exe` (Canon's official updater)
2. Extract with 7-Zip -- the firmware file inside is `IXY_920.FI2`
3. Copy `IXY_920.FI2` to the SD card root
4. Power on camera in **Playback mode**
5. **MENU > Settings > (press UP) > Firmware Ver. > FUNC.SET**
6. Confirm the update -- do NOT power off during the process
7. Camera reboots automatically when done

### CHDK installation

1. Copy `DISKBOOT.BIN` to the SD card root
2. Create the directory `CHDK/MODULES/` on the SD card
3. Copy `webcam.flt` to `CHDK/MODULES/`
4. Insert SD card into camera (**unlocked**)
5. Boot in **Playback mode**
6. Load CHDK: **MENU > Settings > (press UP) > Firmware Ver. > FUNC.SET**

> **Note:** "Firmware Ver." is hidden at the bottom of the Settings menu. You must press **UP** to jump past "Reset All" to reveal it.

CHDK must be loaded each time the camera powers on (manual boot method). For automatic boot, the SD card can be made bootable -- see the [CHDK wiki](https://chdk.fandom.com/wiki/CHDK_1.6_User_Manual).

## Project Directory Layout

```
C:\projects\ixus870IS\
├── README.md                                   -- Project overview & quick start
├── CLAUDE.md                                   -- AI assistant rules
├── CHANGELOG.md                                -- Version history
├── docs\
│   ├── getting-started.md                      -- This file
│   ├── architecture.md                         -- System design & PTP protocol
│   ├── camera-and-chdk.md                      -- Camera & CHDK reference
│   ├── firmware-reverse-engineering.md         -- Ghidra RE findings
│   ├── development-log.md                      -- Full dev history (v0-v35e)
│   ├── debug-frame-protocol.md                 -- Debug channel protocol
│   └── proven-facts.md                         -- Verified facts (32 entries)
├── firmware-dumps\                             -- Canon P&S firmware dumps
├── firmware-analysis\                          -- Ghidra RE workspace
│   ├── ghidra_project\ixus870_101a\            -- Ghidra project (ARM, auto-analyzed)
│   ├── Decompile*.java                         -- Ghidra decompilation scripts
│   ├── ReadDataValues.java                     -- Ghidra data reference script
│   └── *_decompiled.txt                        -- Decompilation output
├── chdk\                                       -- CHDK source tree
│   ├── modules\webcam.c                        -- Webcam module (zero-copy H.264 consumer)
│   ├── modules\webcam.h                        -- Module interface (libwebcam_sym)
│   ├── core\ptp.c                              -- PTP handler (GetMJPEGFrame + async zoom)
│   ├── core\ptp.h                              -- PTP protocol definitions
│   └── platform\ixus870_sd880\sub\101a\
│       ├── movie_rec.c                         -- Recording hook (spy_ring_write + seqlock)
│       └── stubs_entry.S                       -- Firmware function stubs
└── bridge\                                     -- PC-side bridge application
    ├── CMakeLists.txt                          -- CMake build config
    ├── vcpkg.json                              -- Dependency manifest
    ├── build\Release\                          -- Build output
    └── src\
        ├── main.cpp                            -- Entry point, streaming loop, zoom
        ├── ptp\
        │   ├── ptp_client.h                    -- PTP/CHDK protocol definitions
        │   └── ptp_client.cpp                  -- PTP client (libusb, frame, zoom, upload)
        └── webcam\
            ├── h264_decoder.h                  -- H.264 decoder interface
            ├── h264_decoder.cpp                -- FFmpeg H.264 decoder (AVCC -> YUV420P)
            ├── frame_processor.h               -- Frame processor interface
            ├── frame_processor.cpp             -- YUV->RGB24 conversion + upscaling
            ├── preview_window.h                -- Preview window interface
            ├── preview_window.cpp              -- Win32 GDI preview + zoom input
            ├── virtual_webcam.h                -- Virtual webcam interface
            └── virtual_webcam.cpp              -- DirectShow virtual webcam filter
```

## Troubleshooting

### Build issues

| Problem | Solution |
|---------|----------|
| `cmake` not found | Use the full path: `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"` |
| Docker build fails | Ensure Docker Desktop is running and `docker info` succeeds |
| vcpkg packages not found | Run `C:\vcpkg\vcpkg.exe install` from the `bridge/` directory, or install packages manually |
| `FFmpeg not found` warning | Install FFmpeg via vcpkg: `C:\vcpkg\vcpkg.exe install ffmpeg[avcodec,swscale]:x64-windows` |
| Linker error: `pwsh.exe not found` | Harmless vcpkg post-build warning -- the exe was built successfully |

### Runtime issues

| Problem | Solution |
|---------|----------|
| "No Canon camera found" | Ensure camera is on with CHDK loaded and libusb-win32 driver installed (Zadig) |
| USB timeout after connect | Power-cycle camera, restart bridge |
| No frames received | Verify `webcam.flt` exists at `A/CHDK/MODULES/` on SD card |
| First 0.5s of video is black/corrupt | Normal -- H.264 decoder discards P-frames until first IDR keyframe |
| Camera keeps recording after bridge exits | Always use `--timeout N` during development |
| Bridge hangs with no data | Restart the bridge first (USB-level hang). Battery pull only if that doesn't help. |
| Artifacts during zoom | Ensure both `DISKBOOT.BIN` and `webcam.flt` are updated (async zoom requires both) |

## References

| Document | Content |
|----------|---------|
| [README](../README.md) | Project overview & quick start |
| [Architecture](architecture.md) | System design, PTP protocol, data flow |
| [Camera & CHDK Reference](camera-and-chdk.md) | ALT mode, menus, webcam comparison |
| [Firmware Reverse Engineering](firmware-reverse-engineering.md) | Ghidra findings, memory map, ISP architecture |
| [Development Log](development-log.md) | Full implementation history (v0-v35e) |
| [Debug Frame Protocol](debug-frame-protocol.md) | Camera-to-bridge debug channel API |
| [Proven Facts](proven-facts.md) | Verified addresses, formats, constraints |
