# Canon IXUS 870 IS Firmware Upgrade

## Development Rules (MUST follow)

- **After every bridge test**: ALWAYS ask the user for the result. Do NOT assume what happened — the user can see the camera physically. Wait for their response.
- **Document and commit BEFORE any code changes**: After receiving test results from the user, FIRST update docs with the findings, THEN commit. Only AFTER the commit may you proceed with further code changes.
- **Run bridge with `--timeout 20 --no-preview --no-webcam`** during firmware development (graceful shutdown after 20s, no virtual webcam needed).
- **Commit after each bridge test** with a message that describes what was tested and what the result was.
- **Use the debug frame protocol** (`spy_debug_reset/add/send`) in `movie_rec.c` for all camera→bridge diagnostic output. Do NOT inject debug data into H.264 frames. See [Debug Frame Protocol](docs/debug-frame-protocol.md) for API reference and payload format.

## Project Overview

This project contains resources and instructions for upgrading the firmware on the Canon IXUS 870 IS.

- **Also known as**: Canon PowerShot SD 880 IS (North America) / Canon IXY DIGITAL 920 IS (Japan)
- **Current firmware**: GM1.00E (version 1.00e — initial release, 2008-08-22)
- **Reference**: https://chdk.fandom.com/wiki/IXUS870IS
- **CHDK 1.6 User Manual**: https://chdk.fandom.com/wiki/CHDK_1.6_User_Manual

## Camera Specifications

- **Model**: Canon IXUS 870 IS (P-ID: 3196)
- **Processor**: Digic IV
- **Operating System**: DryOS
- **Sensor**: 10.0 MP effective, 1/2.3" CCD
- **Lens**: 4x optical zoom (28-112mm equiv.), F2.8-5.8
- **Stabilization**: Optical IS (lens shift)
- **Display**: 3" LCD, 230,000 pixels
- **Video**: 640x480@30fps, 320x240@30fps, 160x120@15fps (MOV, H.264 + Linear PCM)
- **Battery**: NB-5L lithium-ion (~310 shots per charge)
- **Dimensions**: 94 x 57 x 24mm, 155g

## Firmware Versions

| Version | Canon Version | Date | IS Firmware | IS Parameter | Dump File |
|---------|--------------|------|-------------|--------------|-----------|
| **1.02b** | — | 2009-01-09 | 2.09 | 2.07 | `ixus870_sd880_102b.7z` |
| **1.01a** *(current)* | 1.0.1.0 | 2008-10-15 | — | — | `ixus870_sd880_101a.7z` |
| **1.00e** | — | 2008-08-22 | — | — | `ixus870_sd880_100e.7z` |

- **1.01a** received an official Canon update released on 2009-02-05.

## Firmware Dumps

Firmware dumps are located in `firmware-dumps\IXUS - SD Series\`. This directory contains a broad collection of Canon PowerShot P&S firmware dumps across the full IXUS/SD lineup. The three files relevant to this camera are listed in the table above.

## Documentation

Detailed reference documents (see `docs/` directory):
- [Camera & CHDK Reference](docs/camera-and-chdk.md) — firmware upgrade, CHDK installation, ALT mode, menus
- [Firmware Reverse Engineering](docs/firmware-reverse-engineering.md) — Ghidra project, memory map, ISP architecture, JPCORE pipeline, decompiled functions
- [Webcam Development Log](docs/webcam-development-log.md) — implementation progress, test results, failed approaches, current state
- [Debug Frame Protocol](docs/debug-frame-protocol.md) — camera-to-bridge debug channel, tagged key-value frames, SPSC queue

## Development Environment (checked 2026-02-08)

### Installed Software

| Tool | Version | Path | Status |
|------|---------|------|--------|
| Docker Desktop | 29.2.0 | (in PATH) | Installed |
| Git for Windows | 2.53.0 | (in PATH) | Installed |
| Visual Studio 2019 | Community + BuildTools | `C:\Program Files (x86)\Microsoft Visual Studio\2019\` | Installed (plan calls for 2022, but 2019 works) |
| CMake (VS-bundled) | 3.20 | `C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` | Installed (not in PATH — use full path or VS Developer Command Prompt) |
| MSVC C++ Toolset | 14.29.30133 | (inside VS 2019) | Installed |
| vcpkg | 2025-12-16 | `C:\vcpkg\vcpkg.exe` | Installed |
| 7-Zip | 25.01 | `C:\Program Files\7-Zip\7z.exe` | Installed |
| libusb | 1.0.29#1 | vcpkg (x64-windows) | Installed |
| libjpeg-turbo | 3.1.3 | vcpkg (x64-windows) | Installed |

### Not Yet Installed / Not Found

| Tool | Purpose | Status |
|------|---------|--------|
| Wireshark + USBPcap | USB packet capture & debugging | Not installed |
| chdkptp | PTP connectivity testing | Not installed |
| Zadig | USB driver replacement (libusb-win32) | Not installed |
| softcam | Virtual webcam DirectShow filter | Not installed |
| OpenCV | Image processing (Phase 2) | Not installed via vcpkg |
| FFmpeg | H.264 decode (Phase 2) | Not installed via vcpkg |

### Firmware Reverse Engineering Tools

| Tool | Version | Path | Status |
|------|---------|------|--------|
| JDK 21 (Adoptium Temurin) | 21.0.10 | `C:\Program Files\Eclipse Adoptium\jdk-21.0.10.13-hotspot` | Installed |
| Ghidra | 12.0.2 | `C:\ghidra_12.0.2_PUBLIC` | Installed |

## Project Directory Layout

```
C:\projects\ixus870IS\                          -- Project root
├── CLAUDE.md                                   -- This file (project docs)
├── CONCEPT.md                                  -- Project concept/plan
├── docs\                                       -- Reference documents
│   ├── camera-and-chdk.md                      -- Camera & CHDK reference
│   ├── firmware-reverse-engineering.md         -- Ghidra RE findings
│   ├── webcam-development-log.md              -- Webcam dev progress
│   └── debug-frame-protocol.md                -- Debug channel protocol & API
├── firmware-dumps\                             -- Canon P&S firmware dumps
├── firmware-analysis\                          -- Ghidra RE workspace
│   ├── ghidra_project\ixus870_101a\            -- Ghidra project (ARM, auto-analyzed)
│   ├── ixus870_sd880\sub\101a\                 -- Extracted firmware dump
│   │   ├── PRIMARY.BIN                         -- Main firmware binary (8,323,070 bytes)
│   │   └── Info.txt                            -- Dump metadata
│   ├── DecompileMjpeg.java                     -- Ghidra script: decompile MJPEG API functions
│   ├── DecompileInner.java                     -- Ghidra script: decompile inner/helper functions
│   ├── DecompileVRAM.java                      -- Ghidra script: decompile VRAM setup functions
│   ├── DecompileStateMachine.java              -- Ghidra script: +0x5C state machine & helpers
│   ├── DecompileJpcorePipeline.java            -- Ghidra script: JPCORE pipeline functions
│   ├── mjpeg_decompiled.txt                    -- Output: top-level MJPEG function decompilation
│   ├── mjpeg_inner_decompiled.txt              -- Output: inner function decompilation
│   ├── getvram_decompiled.txt                  -- Output: GetContinuousMovieJpegVRAMData deep dive
│   ├── dma_pipeline_decompiled.txt             -- Output: DMA/pipeline functions
│   ├── pipeline_loop_decompiled.txt            -- Output: pipeline loop (+0x5C access functions)
│   ├── statemachine_decompiled.txt             -- Output: +0x5C state machine & ring buffers
│   └── jpcore_pipeline_decompiled.txt          -- Output: JPCORE encoding pipeline
├── chdk\                                       -- CHDK source tree
│   ├── modules\webcam.c                        -- Webcam CHDK module (created)
│   ├── modules\webcam.h                        -- Webcam module header (created)
│   ├── modules\tje.c                           -- Tiny JPEG Encoder (created)
│   ├── modules\tje.h                           -- TJE header (created)
│   ├── core\ptp.c                              -- PTP handler (modified — added GetMJPEGFrame)
│   ├── core\ptp.h                              -- PTP header (modified)
│   ├── core\modules.c                          -- Module loader (modified)
│   ├── core\modules.h                          -- Module header (modified)
│   ├── modules\Makefile                        -- Module build rules (modified)
│   ├── modules\module_exportlist.c             -- Module exports (modified)
│   └── platform\ixus870_sd880\sub\101a\
│       └── stubs_entry.S                       -- Firmware stubs (modified — added MJPEG stubs)
└── bridge\                                     -- PC-side bridge application
    ├── CLAUDE.md                               -- Bridge build instructions
    ├── CMakeLists.txt                          -- CMake build config
    ├── build\                                  -- VS solution (generated)
    │   └── Release\                            -- Build output
    ├── src\ptp\ptp_client.h                    -- PTP client header
    ├── src\ptp\ptp_client.cpp                  -- PTP client (libusb, opcode 0x9999)
    ├── src\webcam\frame_processor.h            -- Frame processor header
    ├── src\webcam\frame_processor.cpp          -- MJPEG decode + YUV conversion
    ├── src\webcam\virtual_webcam.h             -- Virtual webcam header
    ├── src\webcam\virtual_webcam.cpp           -- DirectShow virtual webcam
    ├── src\main.cpp                            -- Main entry point
    └── driver\                                 -- USB driver files
```

## Build Status (checked 2026-02-08)

### CHDK (camera-side)
- **Platform files**: `chdk\platform\ixus870_sd880\sub\101a\` — fully present
- **Build**: Docker image `chdkbuild` used for cross-compilation
- **Output**: `chdk\bin\DISKBOOT.BIN`, `chdk\CHDK\MODULES\webcam.flt`

### PC-side bridge (`chdk-webcam.exe`)
- **Build output**: `bridge\build\Release\` contains:
  - `chdk-webcam.exe` — main bridge application
  - `libusb-1.0.dll` — USB communication library
  - `turbojpeg.dll` — JPEG decode library
- **VS solution**: `bridge\build\chdk-webcam.sln`

## Build Commands

**Starting Docker Desktop** (required before CHDK builds — daemon takes ~2 minutes to be ready):
```
"C:/Program Files/Docker/Docker/Docker Desktop.exe" &
```
Wait until `docker info` succeeds before running builds.

**CHDK (camera-side) — Docker:**
```
docker run --rm -v "C:\projects\ixus870IS\chdk:/srv/src" chdkbuild make PLATFORM=ixus870_sd880 PLATFORMSUB=101a fir
```

**PC-side bridge — VS 2022 Build Tools:**

cmake is not on PATH; use the full path:
```
set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
```

Configure (only needed once or after CMakeLists.txt changes):
```
%CMAKE% -B C:\projects\ixus870IS\bridge\build -S C:\projects\ixus870IS\bridge -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Build:
```
%CMAKE% --build C:\projects\ixus870IS\bridge\build --config Release
```

Output binary: `bridge\build\Release\chdk-webcam.exe`

## Deploying CHDK to SD Card via SSH

The SD card reader is attached to a Linux host at `192.168.0.54` (accessible as `root` via SSH).

**SSH key**: `~/.ssh/id_ed25519` (passwordless, deployed via `ssh-copy-id`). No password prompt required.

**SD card device**: `/dev/mmcblk0p1` (FAT16, ~2GB)

**CRITICAL: The SD card must be physically inserted into the card reader on the Linux host.
It is NOT automatically mounted. `/mnt/sdcard` is just a directory on the root filesystem —
writing to it without mounting first will NOT write to the SD card!**

**Deployment workflow:**

1. **Ask the user to insert the SD card** into the reader on the Linux host
2. **Mount the SD card** (always ask user before mounting):
   ```
   ssh root@192.168.0.54 "mkdir -p /mnt/sdcard && mount /dev/mmcblk0p1 /mnt/sdcard"
   ```
3. **Copy files:**
   ```
   scp "C:/projects/ixus870IS/chdk/bin/DISKBOOT.BIN" root@192.168.0.54:/mnt/sdcard/DISKBOOT.BIN
   scp "C:/projects/ixus870IS/chdk/CHDK/MODULES/webcam.flt" root@192.168.0.54:/mnt/sdcard/CHDK/MODULES/webcam.flt
   ```
4. **Verify the file is correct** (always check after deploy):
   ```
   ssh root@192.168.0.54 "ls -la /mnt/sdcard/CHDK/MODULES/webcam.flt && md5sum /mnt/sdcard/CHDK/MODULES/webcam.flt"
   ```
   Compare the MD5 with the local file: `certutil -hashfile "C:\projects\ixus870IS\chdk\CHDK\MODULES\webcam.flt" MD5`
5. **Sync and unmount:**
   ```
   ssh root@192.168.0.54 "sync && umount /mnt/sdcard"
   ```
6. **Ask the user to move the SD card** back to the camera and power cycle

## Development Workflow

**Run the bridge with timeout** during camera firmware development. Always use `--timeout 20` so the bridge exits gracefully after 20 seconds — this ensures `stop_webcam()` runs and the camera stops recording. Without `--timeout`, killing the bridge (e.g. from Claude Code background bash) skips cleanup and the camera keeps recording indefinitely.
```
"C:/projects/ixus870IS/bridge/build/Release/chdk-webcam.exe" --timeout 20 --no-preview --no-webcam
```

**After each bridge test run, document the findings and commit all modified files** with a descriptive message documenting the current approach and test results. This ensures we can always revert to a known working state when regressions happen. Include `movie_rec.c` (untracked, must be `git add`'d explicitly) in commits since it contains the spy buffer hooks.
