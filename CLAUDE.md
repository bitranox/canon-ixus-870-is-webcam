# Canon IXUS 870 IS Firmware Upgrade

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

## Firmware Upgrade Process

**IMPORTANT — Accessing "Firmware Ver." on this camera:**
The "Firmware Ver." menu item is HIDDEN at the bottom of the Settings menu and is NOT visible by normal scrolling. To reach it:
1. Power on in **Playback mode**
2. Press **MENU**
3. Navigate to the **Settings tab** (wrench icon)
4. Press **UP arrow** — this jumps to the bottom of the menu, revealing "Firmware Ver." below "Grundeinstellungen" / "Reset All"

**SD card must be UNLOCKED** (write-protect tab in the unlocked position) for the firmware update option to appear.

### Steps

1. **Check current firmware version**: Playback mode > MENU > Settings > UP > Firmware Ver.
2. **Download firmware**: The update file is `IXY_920.FI2` (same file for all regional variants: IXUS 870 IS, SD880 IS, IXY 920 IS). Original Canon source (archived): `https://web.archive.org/web/2024/http://web.canon.jp/imaging/dcp/firm-e/pssd880is/data/pssd880is1010.exe` — extract `IXY_920.FI2` from the self-extracting .exe with 7-Zip.
3. **Prepare SD card**: Place the `.fi2` file in the **root** of the SD card. SD card must be **unlocked**.
4. **Install update**: Playback mode > MENU > Settings > UP > Firmware Ver. > follow prompts > confirm with FUNC.SET.
5. **Do not power off** the camera during the update process.

### Important Notes

- Always ensure the battery (NB-5L) is fully charged before starting a firmware update.
- Do not remove the SD card or battery during the update.
- After updating, rebuild CHDK with the matching `PLATFORMSUB` (e.g., `100e` → `101a`).

## CHDK (Canon Hack Development Kit)

CHDK is an optional third-party firmware enhancement that runs alongside the original Canon firmware. It does not overwrite or replace the factory firmware.

- **Supported firmware versions**: 1.00e, 1.01a, 1.02b
- **Features**: RAW/DNG shooting, scripting (uBASIC/Lua), extended bracketing, live histogram, zebra mode, override shutter/aperture/ISO, and more.
- **Installation**: Load CHDK onto a bootable SD card; it runs from the card without modifying the camera's internal firmware.
- **Compatibility**: You must download the CHDK build that matches your exact firmware version.

### CHDK Installation Methods

1. **Firmware Update Method (manual load each time)**:
   - Start camera in Playback mode
   - Press MENU > UP arrow > select Firmware Update > confirm with FUNC.SET
   - CHDK loads into RAM; must repeat after each power-off

2. **Bootable SD Card Method (automatic)**:
   - Configure SD card as bootable for automatic CHDK loading on startup
   - SD card lock switch must remain in the locked position

### Entering ALT Mode

CHDK features are accessed via ALT mode (typically short-press PRINT, SHORTCUT, or PLAY button). The `--ALT--` indicator appears on-screen. Exit ALT mode to take photos normally.

**Controls in ALT mode**:
- MENU: Access CHDK main menu
- FUNC.SET: Display scripts menu
- DISP: Return to previous menu
- Full Shutter: Execute or end scripts

**Half-shutter shortcuts**:
- Left: Toggle Zebra
- Right: Toggle OSD
- Up: Toggle Histogram
- Down: Toggle Overrides

### CHDK Main Menu Structure

- **Enhanced Photo Operations**: Exposure/focus overrides (Tv, Av, ISO, manual focus, hyperfocal)
- **Video Parameters**: Bitrate/quality control, remove 1GB file-size limit, optical zoom during recording
- **RAW (Digital Negative)**: RAW/DNG capture, bad-pixel removal, cached buffering, file naming
- **Edge Overlay**: Panorama alignment tools
- **Histogram**: Live exposure graphing
- **Zebra**: Highlight/shadow exposure warnings
- **Scripting**: Custom automation via uBASIC/Lua scripts, autostart, parameter management
- **CHDK Settings**: Display, interface, system configuration
- **Miscellaneous Stuff**: Utilities, debugging, battery/temperature/memory OSD

### OSD Display

Configurable on-screen display including: battery status, temperature, memory usage, DOF calculator, clock, USB indicator. Layout editor available for element positioning.

### Using as a Webcam (via CHDK + chdkptp)

CHDK enables a live view feed from the camera to a PC using the **PTP Extension** and the **chdkptp** client. The IXUS 870 IS is explicitly listed as a supported camera. This is not a native webcam — the camera does not appear as a video device — but the live view window can be captured with OBS to create a virtual webcam.

- **Reference**: https://chdk.fandom.com/wiki/PTP_Extension

**Required software**:

| Component | Purpose |
|-----------|---------|
| CHDK (1.00e build) | Firmware enhancement on camera |
| chdkptp | PC client for PTP communication + live view |
| libusb-win32 | Alternative USB driver (Windows) |
| OBS Studio (optional) | Capture live view window as virtual webcam for Zoom/Teams/etc. |

**Setup**:

1. Install CHDK on the camera (must match firmware version 1.00e)
2. Connect camera to PC via USB
3. Install libusb-win32 driver on PC
4. Run chdkptp and connect to the camera
5. Start live view in chdkptp — the camera's LCD feed streams to a window on the PC
6. (Optional) Use OBS Studio to capture the chdkptp window and enable Virtual Camera for use in video conferencing apps

**Limitations**:

- Frame rate is tied to the camera's LCD refresh — do not expect smooth 30fps video
- Live view may stop updating or show artifacts if the camera LCD turns off
- Requires chdkptp running on the PC at all times
- Resolution is limited to the camera's LCD buffer, not the full sensor resolution
- Additional features: camera can save JPGs directly to PC without storing on SD card

### Known CHDK Issues

- DNG file colors may be slightly misaligned.
- Brief power-on button presses can trigger review mode instead of normal startup.
- Optical/digital zoom transitions in video mode require releasing the zoom lever between steps.
- CHDK is experimental software — no confirmed camera damage reports, but provided without warranty.

## Native Webcam Project (Phase 1: MJPEG Streaming)

Goal: Turn the IXUS 870 IS into a native webcam using CHDK's video mode buffer (640x480) + MJPEG compression + PTP streaming + PC-side virtual webcam bridge. Camera appears as "CHDK Webcam" in Zoom/Teams/OBS.

### Development Environment (checked 2026-02-08)

#### Installed Software

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

#### Not Yet Installed / Not Found

| Tool | Purpose | Status |
|------|---------|--------|
| Wireshark + USBPcap | USB packet capture & debugging | Not installed |
| chdkptp | PTP connectivity testing | Not installed |
| Zadig | USB driver replacement (libusb-win32) | Not installed |
| softcam | Virtual webcam DirectShow filter | Not installed |
| OpenCV | Image processing (Phase 2) | Not installed via vcpkg |
| FFmpeg | H.264 decode (Phase 2) | Not installed via vcpkg |

#### Firmware Reverse Engineering Tools

| Tool | Version | Path | Status |
|------|---------|------|--------|
| JDK 21 (Adoptium Temurin) | 21.0.10 | `C:\Program Files\Eclipse Adoptium\jdk-21.0.10.13-hotspot` | Installed |
| Ghidra | 12.0.2 | `C:\ghidra_12.0.2_PUBLIC` | Installed |

### Project Directory Layout

```
C:\projects\ixus870IS\                          -- Project root
├── CLAUDE.md                                   -- This file (project docs)
├── CONCEPT.md                                  -- Project concept/plan
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

### Build Status (checked 2026-02-08)

#### CHDK (camera-side)
- **Platform files**: `chdk\platform\ixus870_sd880\sub\101a\` — fully present
- **Build**: Docker image `chdkbuild` used for cross-compilation
- **Output**: `chdk\bin\DISKBOOT.BIN`, `chdk\CHDK\MODULES\webcam.flt`

#### PC-side bridge (`chdk-webcam.exe`)
- **Build output**: `bridge\build\Release\` contains:
  - `chdk-webcam.exe` — main bridge application
  - `libusb-1.0.dll` — USB communication library
  - `turbojpeg.dll` — JPEG decode library
- **VS solution**: `bridge\build\chdk-webcam.sln`

### Build Commands

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

### Deploying CHDK to SD Card via SSH

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

### Development Workflow

**Run the bridge without preview** during camera firmware development to avoid unnecessary overhead:
```
"C:/projects/ixus870IS/bridge/build/Release/chdk-webcam.exe" --no-preview --no-webcam
```
Focus on getting the camera firmware pipeline working first; add preview/webcam later.

**After each bridge test run, document the findings and commit all modified files** with a descriptive message documenting the current approach and test results. This ensures we can always revert to a known working state when regressions happen. Include `movie_rec.c` (untracked, must be `git add`'d explicitly) in commits since it contains the spy buffer hooks.

## Firmware Reverse Engineering (Ghidra)

### Ghidra Project Setup

- **Project**: `C:\projects\ixus870IS\firmware-analysis\ghidra_project\ixus870_101a`
- **Binary**: `PRIMARY.BIN` (8,323,070 bytes, firmware version 1.01a)
- **Processor**: ARM:LE:32:v5t (ARM926EJ-S / ARMv5TEJ)
- **Base address**: `0xFF810000` (flash ROM mapping on Digic IV)
- **Analysis**: Full auto-analysis completed (~200 seconds headless)

**Headless Ghidra command** (for running scripts):
```
"C:\ghidra_12.0.2_PUBLIC\support\analyzeHeadless.bat" ^
    "C:\projects\ixus870IS\firmware-analysis\ghidra_project" ixus870_101a ^
    -process PRIMARY.BIN -noanalysis ^
    -scriptPath "C:\projects\ixus870IS\firmware-analysis" ^
    -postScript ScriptName.java
```
Note: Python scripts do NOT work in Ghidra 12 headless mode (requires PyGhidra). Use Java `GhidraScript` subclasses instead.

### Digic IV Memory Map

| Address Range | Region | Notes |
|---------------|--------|-------|
| `0x00000000`–`0x03FFFFFF` | RAM (DRAM) | ~64 MB, cached |
| `0x10000000`–`0x1FFFFFFF` | TCM / Cache | Tightly coupled memory |
| `0x40000000`–`0x43FFFFFF` | Uncached RAM mirror | Same physical RAM as 0x00000000, bypasses CPU cache |
| `0xC0000000`–`0xCFFFFFFF` | I/O registers | Hardware peripheral control |
| `0xFF800000`–`0xFFFFFFFF` | Flash ROM | Firmware code + read-only data |

### Digic IV Hardware Encoding Architecture

**Important:** The three-block JPCORE/JP62/JP57 model documented by Magic Lantern for Canon EOS DSLRs does **NOT** apply to the IXUS 870 IS (PowerShot compact). A thorough firmware analysis found **zero references** to the `0xC0E0xxxx`, `0xC0E1xxxx`, or `0xC0E2xxxx` I/O register ranges anywhere in the firmware code or data.

**IXUS 870 IS ISP architecture:**
The IXUS 870 IS uses a **single ISP-attached encoding pipeline** controlled entirely through registers in the `0xC0F0xxxx`–`0xC0F1xxxx` range:

| Register Range | Purpose |
|----------------|---------|
| `0xC0F04000`–`0xC0F04300` | DMA channel control (4 channels) |
| `0xC0F05000`–`0xC0F0521C` | ISP routing (16 channels) |
| `0xC0F0D000`–`0xC0F0D0A4` | ISP sensor/color configuration |
| `0xC0F11008`–`0xC0F11150` | Pipeline mode/scaler control |
| `0xC0F110C4` | **Pipeline mode selector** (4=EVF, 5=video recording) |
| `0xC0F111C0`–`0xC0F111C8` | Pipeline resizer configuration |
| `0xC0F11344`–`0xC0F113BC` | Video recording pipeline setup |
| `0xC0F1A008`–`0xC0F1A014` | Pipeline scaler registers |

The difference between still JPEG and video H.264 encoding is a **mode parameter** to the same hardware, not a different hardware destination:
- **Mode 0 (Still JPEG)**: Uses `PipelineRouting(0, 0x01)`, JPCORE encodes via `FUN_ff8eb574_PipelineStep3` → `FUN_ff849448_JPCORE_DMA_Start`
- **Mode 4 (EVF)**: Uses `PipelineRouting(0, 0x11)` via `PipelineConfig_FFA02DDC`
- **Mode 5 (Video recording)**: Uses `PipelineRouting(0, 0x11)` with H.264 encoder (`H264EncPass.c`)

Firmware strings confirm the encoding subsystem:
- `JpCore.c`, `JpCoreIntrHandler`, `JpCore2IntrHandler` — two interrupt handlers suggest dual encoding capability
- `EncodeJpegPass.c`, `EncodePictJpegPass.c` — still image JPEG paths
- `H264EncPass.c`, `H264ThumbnailDecPass.c` — video H.264 paths

**The IXUS 870 IS records video as H.264 in MOV container** (not MJPEG in AVI). The predecessor IXUS 860 IS (Digic III) used AVI/MJPEG — Canon kept the legacy "MJPEG" function names (`StartMjpegMaking`, `GetContinuousMovieJpegVRAMData`) in the Digic IV firmware even though the underlying encoder changed to H.264. These function names appear in 586+ CHDK platform files across generations.

**Why `GetContinuousMovieJpegVRAMData` never produces JPEG output:**
The function requires the full movie recording pipeline to be initialized via `sub_FF8C3BFC`, which sets up frame dispatch pointers (`state[+0x114]`, `state[+0x118]`, `state[+0x6C]`). The frame dispatch function (`FUN_ff8c335c`) checks `state[+0xF0]==1` AND `state[+0x48]==1` AND `state[+0x6C]!=NULL` before delivering encoded frames. Without the complete recording task initialization, no frames are delivered to the VRAM buffer. Additionally, the video path produces H.264 frames (not JPEG) which cannot be decoded independently due to inter-frame prediction.

**Why H.264 frames cannot be used for webcam streaming:**
H.264 uses inter-frame prediction (P-frames reference I-frames), so individual frames cannot be decoded independently. The movie recording pipeline (`task_MovieRecord`, `task_MovWrite`) crashes when partially initialized.

**Result:** The raw UYVY approach (capturing pre-encoding ISP output at ~5 FPS) is the optimal solution for webcam streaming on this camera.

### Hardware MJPEG Encoder — Reverse Engineering Results (Legacy Investigation)

The firmware functions below were extensively investigated as potential hardware JPEG encoding paths. They are documented here for reference, but **do not produce JPEG output** on the IXUS 870 IS because the video pipeline routes to JP62 (H.264), not JPCORE (JPEG).

#### Firmware Function Addresses (fw 1.01a)

All addresses confirmed via `funcs_by_address.csv` and Ghidra decompilation.

| Function | ROM Address | Args | Returns | Purpose |
|----------|------------|------|---------|---------|
| `StartMjpegMaking_FW` | `0xFF9E8DD8` | 0 | 0 (always) | Activate JPCORE encoder for EVF pipeline |
| `StopMjpegMaking_FW` | `0xFF9E8DF8` | 0 | 0 (always) | Deactivate JPCORE encoder |
| `GetContinuousMovieJpegVRAMData_FW` | `0xFFAA234C` | 4 | 0=success | Synchronous one-frame capture (see below) |
| `GetMovieJpegVRAMHPixelsSize_FW` | `0xFF8C4178` | 0 | width | Read horizontal pixel count from global struct |
| `GetMovieJpegVRAMVPixelsSize_FW` | `0xFF8C4184` | 0 | height | Read vertical pixel count from global struct |
| `StopContinuousVRAMData_FW` | `0xFF8C425C` | 4 | varies | Release current frame DMA, clean up |
| `StartEVFMovVGA_FW` | `0xFF9E8944` | 4 | bool | Start EVF at 640x480 @ 30fps |

Related firmware functions (unnamed, by address):

| Address | Called By | Purpose |
|---------|-----------|---------|
| `FUN_ff8c3d38` | StartMjpegMaking | Inner: validates semaphore, sets state +0x48=1, calls FUN_ff9e8190 |
| `FUN_ff8c3c94` | StopMjpegMaking | Inner: resets state +0x48=0, calls FUN_ff9e81a0, optional cleanup callback |
| `FUN_ffaa2224` | GetContinuousMovieJpegVRAMData | Inner: checks MJPEG active, triggers DMA, returns VRAM addr/size |
| `FUN_ff8c4208` | FUN_ffaa2224 | Set up one-shot DMA frame capture with callback |
| `FUN_ff8c4288` | FUN_ffaa2224 | Check if MJPEG engine is active (returns 1 if yes) |
| `LAB_ffaa12b0` | (callback) | JPCORE DMA completion callback — signals event flag |
| `FUN_ff869508` | GetContinuousMovieJpegVRAMData | Event flag signal (DryOS `SetEventFlag`) |
| `FUN_ff869330` | GetContinuousMovieJpegVRAMData | Event flag wait (DryOS `WaitForEventFlag`) |
| `FUN_ff9e8190` | FUN_ff8c3d38 | Enable JPCORE pipeline |
| `FUN_ff9e81a0` | FUN_ff8c3c94 | Disable JPCORE pipeline |
| `sub_FF92FE8C` | movie_rec.c | Movie recorder frame getter: 4 output pointers → (jpeg_ptr, jpeg_size, meta1, meta2) |

#### GetContinuousMovieJpegVRAMData — Detailed Calling Convention

**Ghidra decompiled signature:**
```c
uint GetContinuousMovieJpegVRAMData_FW(
    undefined4 *param_1,   // R0: pointer to uint32 frame index (INPUT, dereferenced)
    undefined4  param_2,   // R1: unused
    undefined4  param_3,   // R2: scratch (overwritten internally with VRAM addr)
    undefined4  param_4    // R3: scratch (overwritten internally with VRAM size)
);
// Returns: 0 on success, non-zero on failure (0x11 = MJPEG not active, 3 = frame index >= 31)
```

**Execution flow** (from Ghidra RE):
```
1. Read frame_index = *param_1          (must be < 31)
2. Signal event flag                     (clear for waiting)
3. Call FUN_ffaa2224(frame_index, &stack_buf, callback, 0)
   └─ Check FUN_ff8c4288() == 1         (MJPEG engine active?)
   └─ Call FUN_ff8c4208(frame_index, callback, 0)  (trigger DMA capture)
   └─ Write stack_buf[0] = *(0xFFAA2314) = 0x40EA23D0  (VRAM address)
   └─ Write stack_buf[1] = *(0xFFAA2318) = 0x000D2F00  (buffer size)
   └─ Print "MJVRAM Address  : %p" and "MJVRAM Size     : 0x%x"
   └─ Return 0
4. Wait for event flag                   (blocks until callback fires)
5. Call StopContinuousVRAMData           (release DMA for this frame)
6. Return 0
```

**CRITICAL**: This function does **NOT** return the JPEG data pointer or size through its parameters. The JPEG data is at the fixed VRAM buffer address. The function's purpose is synchronization — it triggers a DMA capture, waits for completion, then returns.

**Correct calling pattern from CHDK module** (via `call_func_ptr`):
```c
unsigned int io_buf[2];
unsigned int args[4];

io_buf[0] = frame_index;    // 0..30, wraps at 31
io_buf[1] = 0;

args[0] = (unsigned int)io_buf;  // R0: pointer to frame index
args[1] = 0;                      // R1: unused
args[2] = 0;                      // R2: scratch
args[3] = 0;                      // R3: scratch

ret = call_func_ptr((void *)0xFFAA234C, args, 4);
// ret == 0: success, JPEG data now at 0x40EA23D0
```

#### JPCORE VRAM Buffer

The hardware JPEG encoder writes frames via DMA to a fixed uncached RAM buffer. These constants are embedded in the firmware ROM literal pool.

| Constant | ROM Address | Value | Meaning |
|----------|------------|-------|---------|
| VRAM buffer address | `0xFFAA2314` | `0x40EA23D0` | Uncached RAM (physical: `0x00EA23D0`) |
| VRAM buffer max size | `0xFFAA2318` | `0x000D2F00` | 864,000 bytes (~844 KB) |

- The buffer address is in the **uncached RAM mirror** (`0x40000000` + offset), ensuring CPU reads always see the latest DMA-written data without cache coherency issues.
- The max size (844 KB) is the total buffer allocation; actual JPEG frames are typically 30–100 KB for 640x480.
- The **actual frame size** is determined by scanning for the JPEG EOI marker (`FF D9`) in the buffer data. In entropy-coded JPEG, `FF` bytes are always followed by `00` (byte stuffing), so `FF D9` uniquely identifies the end of image.
- Buffer contents remain valid after `GetContinuousMovieJpegVRAMData` returns (the DMA is stopped but RAM is not cleared) until the next frame capture overwrites it.

#### StartEVFMovVGA — Video Mode Parameters

`StartEVFMovVGA_FW` (0xFF9E8944) configures the EVF video pipeline:

| Parameter | Value | Decoded |
|-----------|-------|---------|
| Resolution H | `0x280` | 640 pixels |
| Resolution V | `0x1E0` | 480 pixels |
| Frame rate | `0x1E` | 30 fps |
| Quality mode | `2` | VGA mode |

Other modes available in firmware:
- `StartEVFMovQVGA60_FW` (0xFF9E8A24): 320x240 @ 60fps, mode=1
- `StartEVFMovXGA_FW` (0xFF9E8C58): resolution from global, 30fps, mode=3
- `StartEVFMovHD_FW` (0xFF9E8D10): resolution from global, 30fps, mode=3

#### MJPEG State Machine

The MJPEG engine uses a global state structure accessed via `DAT_ff8c2e24`. Key offsets:

| Offset | Purpose |
|--------|---------|
| +0x38, +0x3C, +0x40 | Zeroed during EVF setup (`FUN_ff8c3c64`) |
| +0x48 | MJPEG active flag: 0=off, 1=on (set by StartMjpegMaking, cleared by StopMjpegMaking) |
| +0x4C | Set to 1 alongside +0x48 during start |
| +0x54 | Frame counter (incremented by FUN_ff8c2938 frame setup) |
| +0x5C | State machine: 3→4→5 during frame capture |
| +0x6C | Recording buffer address (set by sub_FF8C3BFC param_2) |
| +0x80 | Optional cleanup callback function pointer (called during stop) |
| +0xB0 | DryOS event flag handle for synchronization |
| +0xEC | Pipeline state: must be 1 for StartMjpegMaking to proceed |
| +0x114 | Recording callback 1 (set by sub_FF8C3BFC param_1) — checked by pipeline to route data to JPCORE |
| +0x118 | Recording callback 2 (set by sub_FF8C3BFC param_3) |

#### Movie Recording Task — Frame Capture Pattern

The movie recording task (`movie_record_task` at 0xFF85E03C, patched in `movie_rec.c`) uses a **different** function for frame retrieval than `GetContinuousMovieJpegVRAMData`:

```asm
; movie_rec.c sub_FF85D98C_my, lines 141-148
ADD     R3, SP, #0x28    ; output: metadata2
ADD     R2, SP, #0x2C    ; output: metadata1
ADD     R1, SP, #0x30    ; output: JPEG data size
ADD     R0, SP, #0x34    ; output: JPEG data pointer
BL      sub_FF92FE8C     ; unnamed frame getter
```

`sub_FF92FE8C` (unnamed in firmware CSV) takes **4 output pointers** and fills them with:
- `sp[0x34]` = JPEG data pointer (used for file write operations)
- `sp[0x30]` = JPEG data size (checked against 0, used in size calculations)
- `sp[0x2C]` = metadata (passed to AVI write functions)
- `sp[0x28]` = metadata (passed to AVI write functions)

This function is called within the movie recording task context after pipeline setup via `sub_FF8C3BFC`. It may require full movie recording state initialization to work correctly — the webcam module uses `GetContinuousMovieJpegVRAMData` instead for simpler synchronization.

#### NHSTUB Entries Added to stubs_entry.S

Seven firmware function stubs were added to `chdk/platform/ixus870_sd880/sub/101a/stubs_entry.S`:

```asm
NHSTUB(StartMjpegMaking                       ,0xff9e8dd8)
NHSTUB(StopMjpegMaking                        ,0xff9e8df8)
NHSTUB(GetContinuousMovieJpegVRAMData         ,0xffaa234c)
NHSTUB(GetMovieJpegVRAMHPixelsSize            ,0xff8c4178)
NHSTUB(GetMovieJpegVRAMVPixelsSize            ,0xff8c4184)
NHSTUB(StopContinuousVRAMData                 ,0xff8c425c)
NHSTUB(StartEVFMovVGA                         ,0xff9e8944)
```

Note: The webcam module currently calls these via `call_func_ptr()` with hardcoded addresses (no stub linkage needed), but the stubs are declared for potential future use by other code.

#### Webcam Module Hardware Path — Current Implementation

The webcam module (`chdk/modules/webcam.c`) implements two encoding paths:

**Hardware path** (preferred, ~5-15+ FPS expected):
1. `webcam_start()` → switches to video mode → `hw_mjpeg_start()`:
   - Calls `StartMjpegMaking` (0 args) to activate JPCORE
   - Queries frame dimensions via `GetMovieJpegVRAMHPixelsSize` / `VPixelsSize`
2. `hw_mjpeg_get_frame()` per frame:
   - Calls `GetContinuousMovieJpegVRAMData` with 4 args (frame_index via pointer in R0)
   - On success (returns 0), reads JPEG data from fixed VRAM buffer at `0x40EA23D0`
   - Scans for JPEG EOI marker (`FF D9`) to determine actual frame size
   - Increments frame index (wraps at 31)
3. `hw_mjpeg_stop()`:
   - Calls `StopMjpegMaking` (0 args)

**Software path** (fallback, ~1.8 FPS):
- Reads viewport buffer (720x240 YUV411 UYVYYY)
- Software JPEG compression via `tje.c` (Tiny JPEG Encoder)
- Double-buffered output to avoid tearing

#### Known Uncertainties / Not Yet Tested

- **VRAM buffer interpretation**: The value `0x40EA23D0` at ROM `0xFFAA2314` is assumed to be the direct buffer address. If it's actually a pointer-to-pointer (an additional level of indirection that Ghidra's decompiler may have collapsed), the actual buffer address would be `*(uint32_t *)0x40EA23D0`. On-camera testing will reveal which interpretation is correct.
- **Frame size determination**: The EOI scan approach works for well-formed JPEG but adds O(n) overhead per frame. If a hardware register or global variable stores the actual encoded size, it would be faster. Candidate: the MJPEG state structure at `DAT_ff8c2e24` may have a frame size field.
- **GetContinuousMovieJpegVRAMData blocking behavior**: The event flag wait has timeout=0 (possibly infinite). If the JPCORE never fires the callback (e.g., encoder not properly initialized), the call will hang. The 500ms `msleep()` after mode switch in `webcam_start()` is intended to let the pipeline stabilize, but may need tuning.
- **Repeated calls**: Each call to `GetContinuousMovieJpegVRAMData` internally calls `StopContinuousVRAMData` after getting one frame. It's unclear if this causes performance overhead for repeated single-frame captures vs. a true continuous mode.
- **Alternative: `sub_FF92FE8C`**: The movie recording task uses this unnamed function which clearly returns JPEG data through 4 output pointers. If `GetContinuousMovieJpegVRAMData` doesn't work well for streaming, this function could be tried, though it may require full movie recording pipeline setup.

#### Hardware Encoder Debugging Status (2026-02-11)

**Root cause found (from deep RE)**: The VRAM buffer at `0x40EA23D0` stores **YUV pipeline frame data** (output of pipeline slot 0 / resizer), NOT JPEG-encoded data. The JPCORE hardware encoder (pipeline slot 0xb) writes to a **completely separate buffer** at `piVar1[4] = *(0x2564)`, which is populated from a buffer array at `*(0x2588)` during JPCORE_DMA_Start.

**Confirmed by Ghidra RE**:
- `GetContinuousMovieJpegVRAMData` returns 0 (success) — the event flag IS signaled correctly
- The +0x5C state machine works: 3→(pipeline callback fires)→4→5
- Counter at +0x54 IS incremented by FUN_ff8c2938 (frame setup), not the state machine
- The callback fires on the 2nd pipeline frame (when counter > frame_index + 1)
- `0x40EA23D0` is a ring buffer base for YUV frames, NOT JPCORE output
- JPCORE output buffer comes from RAM 0x2588 (`buf[2]` for video modes 0-2)

**JPCORE encoding pipeline** (decompiled from PipelineStep3 at 0xFF8EB574):
```
Pipeline frame callback (FUN_ff8c1fe4):
  1. PipelineStep0 — resize/color conversion
  2. PipelineStep1
  3. MjpegEncodingCheck — sets JPCORE enable flag = state[+0x48]
  4. PipelineStep2 — stores frame param into JPCORE state
  5. PipelineStep3 — *** calls JPCORE_DMA_Start ***:
     - Checks 10 pipeline flags via FUN_ff8eaa10
     - If any flag is non-zero: configures JPCORE hardware registers
     - Calls FUN_ff849448(frame_param, ..., FrameComplete_callback)
     - FUN_ff849448 conditions for encoding:
       (a) *(0x12850 + idx*4) == 1 (secondary index)
       (b) *(0x2568) != -1 (piVar1[5])
       (c) *(0x2574) == 0 (piVar1[8])
     - If all conditions met: piVar1[4] = buf[idx*2+2] (output buffer)
     - Programs JPCORE hardware register 0xC0F04908 with output addr
     - Triggers JPCORE encoding
  6. FrameProcessing
  7. VideoModeProcessing
```

**JPCORE continuous encode loop**:
```
1. PipelineStep3 → JPCORE_DMA_Start → programs hardware, triggers
2. JPCORE hardware encodes frame → fires interrupt
3. FUN_ff849168 (interrupt handler) → releases semaphore, calls FrameComplete
4. FUN_ff8f8ce8 (FrameComplete) → checks all 3 completion bits set (=7)
   → if piVar1[3]==1: re-programs JPCORE and triggers again
5. → goto 2 (continuous loop until piVar1[3] set to 0)
```

**JPCORE RAM structures** (from ROM literal pool analysis):

| RAM Address | Name | Purpose |
|-------------|------|---------|
| `0x00002554` | JPCORE state (DAT_ff84924c) | JPCORE DMA control struct |
| `0x00002560` | piVar1[3] | JPCORE DMA active flag (1=running) |
| `0x00002564` | piVar1[4] | **JPCORE output buffer address** |
| `0x00002568` | piVar1[5] | Must be != -1 for JPCORE to start |
| `0x00002570` | piVar1[7] | Semaphore handle |
| `0x00002574` | piVar1[8] | Must be 0 for JPCORE to start |
| `0x0000257C` | piVar1[10] | Frame index from ROM lookup |
| `0x00002580` | Buffer array base | Array of JPCORE output buffer addresses |
| `0x00002588` | buf[2] | **Actual JPCORE output for VGA mode** |
| `0x00008224` | DAT_ff8f9028 | JPCORE frame completion state |
| `0x00012850` | Secondary index table | Controls JPCORE trigger (entry must be 1) |
| `0xC0F04908` | JPCORE HW register | DMA output destination (I/O register) |

**Current fix approach**: The webcam module now tries 3 buffer locations in order:
1. `0x40EA23D0` (original VRAM assumption)
2. `*(0x2564)` (JPCORE piVar1[4] output)
3. `*(0x2588)` (JPCORE buf[2] for VGA mode)

Checks for JPEG SOI marker (FF D8) at each location. Uses the first valid one.

**v4/v5 diagnostics** report all JPCORE control state fields to identify if any condition prevents encoding.

#### On-Device Testing Results (2026-02-11)

**v5b diagnostics confirmed**:
- ROM constants: `0x40EA23D0` / `0x000D2F00` — correct, match expected
- MJPEG active check (`FUN_ff8c4288`): returns 1 — engine thinks it's active
- Dimensions: 640×480 — correct
- `GetContinuousMovieJpegVRAMData` return: 0 — claims success
- PS3 completion mask = 6 (bits 1,2 set, bit 0 missing = **JPCORE interrupt never fires**)
- VRAM buffer content: unchanged after call (0xAA marker test — wrote 0xAA to first 32 bytes before call, bytes remained 0xAA after)
- JPCORE HW register `0xC0F04908` shows changing values (0x0070D780→0x0070D760→0x0070D6A0→0x0070D640) suggesting JPCORE hardware IS doing something but never completing

**Key insight — JPCORE_DMA_Start return value semantics**: Returns **0 on SUCCESS**, 1 on FAILURE (previously misinterpreted). PS3[4]=0 means JPCORE was successfully started. The completion mask of 6 is the CORRECT result — bit 0 only gets set when the JPCORE hardware interrupt fires after completing a frame encode.

**Root cause**: JPCORE hardware is configured and started but **never receives input data** from the sensor pipeline. The pipeline frame callback (`FUN_ff8c1fe4`) routes sensor data through PipelineStep0→1→2→3, where Step3 calls JPCORE_DMA_Start. But the recording callbacks in the MJPEG state struct must be populated for the pipeline to route data to JPCORE's input.

#### Recording Pipeline Setup — sub_FF8C3BFC

**sub_FF8C3BFC is trivially simple** — just stores 3 values to the MJPEG state struct at `0x70D8`:
```c
void sub_FF8C3BFC(uint32_t param_1, uint32_t param_2, uint32_t param_3) {
    volatile uint32_t *state = (volatile uint32_t *)0x70D8;
    state[0x6C/4]  = param_2;   // recording buffer address
    state[0x114/4] = param_1;   // recording callback 1
    state[0x118/4] = param_3;   // recording callback 2
}
```

**movie_record_task calls it at `loc_FF85E0AC`** (state 11 handler) with:
```asm
LDR     R0, =0xFF85D370      ; recording callback 1
LDR     R1, =0x1AB94         ; recording buffer
LDR     R2, =0xFF85D28C      ; recording callback 2
BL      sub_FF8C3BFC
```

**Important**: movie_record_task's own state struct is at `0x51A8` (R4 base), which is SEPARATE from the MJPEG state struct at `0x70D8`. The movie_status variable is at `0x51E4` (= 0x51A8 + 0x3C).

#### Failed Approaches to Activate Hardware Encoder

| # | Approach | What It Does | Why It Failed |
|---|----------|-------------|---------------|
| 1 | `set_movie_status(2)` | Sets `*(0x51E4) = 2` | Only sets a variable in movie_record_task's state struct. Doesn't trigger the firmware task or call sub_FF8C3BFC. |
| 2 | `levent_set_record()` | Posts "PressRecButton"/"UnpressRecButton" UI events via `PostLogicalEventToUI()` | UI event loop doesn't process recording events when camera is connected via USB in PTP mode. movie_status stayed at 0 after the call. |
| 3 | Direct `sub_FF8C3BFC` call | Calls `sub_FF8C3BFC(0xFF85D370, 0x1AB94, 0xFF85D28C)` directly via `call_func_ptr()` to populate recording callbacks | **CRASHED camera within 1 second.** FUN_ff8c335c calls state[+0x114] as a function pointer on every pipeline frame. The movie_record_task callbacks expect R4=0x51A8 context which isn't set up → immediate crash. |

#### Recording Callbacks — FUN_ff8c335c Analysis

**CRITICAL FINDING**: The recording callbacks stored by sub_FF8C3BFC are for **frame delivery** (downstream), NOT for JPCORE activation (upstream). Decompilation of `FUN_ff8c335c` (the ONLY function that reads these offsets) reveals:

```c
// FUN_ff8c335c — called from pipeline to deliver encoded frames
// Gate: state[+0xF0]==1 AND state[+0x48]==1
void FUN_ff8c335c(uint *param_1, uint param_2, uint param_3, uint param_4) {
    if (state[+0x6C] != 0) {
        // Write frame metadata INTO the recording buffer at state[+0x6C]
        *(state[+0x6C]) = param_3;
        // ... more writes at +4, +8, +0xC, +0x10, +0x14, +0x18
    }
    if (state[+0x114] != 0) {
        (*state[+0x114])(state[+0x6C]);  // CALL as function pointer!
    }
    if (state[+0x118] != 0) {
        (*state[+0x118])();              // CALL as function pointer!
    }
}
```

- `state[+0x6C]` = pointer to recording buffer struct (writes frame data INTO it)
- `state[+0x114]` = called as `callback_1(recording_buffer)` — notifies movie_record_task of new frame
- `state[+0x118]` = called as `callback_2()` — secondary notification
- Both are NULL-checked before calling
- Setting these to movie_record_task's callbacks (0xFF85D370, 0xFF85D28C) **CRASHES** the camera because those functions expect movie_record_task context (R4=0x51A8 state pointer) which isn't set up

**Cross-reference confirmation**: Ghidra xref analysis found:
- `0x71EC` (state[+0x114]): READ only by FUN_ff8c335c, WRITTEN only by sub_FF8C3BFC
- `0x71F0` (state[+0x118]): READ only by FUN_ff8c335c, WRITTEN only by sub_FF8C3BFC
- `0x7144` (state[+0x6C]): READ only by FUN_ff8c335c (3 refs), WRITTEN only by sub_FF8C3BFC

#### Webcam Module — Current hw_mjpeg_start() Sequence

```c
// 1. Switch to video mode (activates EVF pipeline)
// 2. msleep(500) — let pipeline stabilize
// 3. StartMjpegMaking (0 args) — activate JPCORE, set state[+0x48]=1
// 4. Wait for JPCORE completion mask == 7 (up to 4 seconds)
// 5. GetContinuousMovieJpegVRAMData — trigger DMA, wait for JPCORE frame
```

**Status**: v7 build deployed — reverted sub_FF8C3BFC crash, restored StartMjpegMaking. Camera runs without crashing; JPCORE reports active but produces no JPEG output.

**Remaining mystery**: JPCORE hardware is configured and started (DMA_Start returns 0=success, piVar1[3]=1, registers changing) but never fires its completion interrupt. The JPCORE output buffer is programmed. But no JPEG data appears. The **input data path** to JPCORE remains the unsolved problem.

#### Pipeline Frame Callback — Call Chain Analysis (2026-02-11)

The per-frame callback `FUN_ff8c1fe4_PipelineFrameCallback` (called by EVF pipeline for every frame) executes this sequence:

```
1. PipelineStep0 (FUN_ff8eed74)     — Power/clock enable only (not input routing)
2. PipelineStep1 (FUN_ff8f6e24)     — Initialize pipeline step 1 state
3. MjpegEncodingCheck (FUN_ff9e8104) — If state[+0x48]=1, queues MJPEG encode command
4. PipelineStep2 (FUN_ff8f8dd8)     — Store output buffer selector
5. PipelineStep3 (FUN_ff8eb574)     — Configure & start JPCORE DMA encoding
6. FrameProcessing (FUN_ff9e5328)   — Route frame data (LCD vs video recording)
7. VideoModeProcessing (FUN_ff9e7994)
```

**FrameProcessing dispatch** (FUN_ff9e5328): Uses `state[+0xD4]` to determine data routing:
- **mode 1** → `FUN_ff9e51d8` (general/EVF path — likely LCD display only)
- **mode 2** → `FUN_ff9e508c` (video recording path — may route data to JPCORE input)

**Hypothesis**: In EVF mode (our webcam context), state[+0xD4] is likely 1, causing FrameProcessing to use the LCD-only path. This means the ISP output goes to the display buffer but NOT to the JPCORE pipeline input. During actual movie recording, state[+0xD4] = 2 would route data through FUN_ff9e508c, which connects the ISP output to JPCORE's hardware pipeline input. **This may be the root cause of JPCORE never receiving input data.**

#### PS3 Completion Mask — Corrected Interpretation

The PipelineStep3 completion mask at `DAT_ff8f9028 + 0x0C`:
```
Bit 0: Set if JPCORE_DMA_Start returns 1 (FAILURE)
Bit 1: Always set (pre-JPCORE flag = 1)
Bit 2: Always set (post-processing flag = 1)
```

- **Mask = 6** (binary 110): NORMAL — JPCORE_DMA_Start succeeded (returned 0)
- **Mask = 7** (binary 111): JPCORE_DMA_Start FAILED (returned 1)
- The previous interpretation (mask=7 = success) was **incorrect**

#### movie_record_task — Frame Acquisition Flow

From `movie_rec.c` (sub_FF85D98C_my), the movie recording frame flow is:
```
1. Message 0xb → sub_FF8C3BFC(callback1=0xFF85D370, buf=0x1AB94, callback2=0xFF85D28C)
   → Sets up frame delivery callbacks in MJPEG state struct
   → Also stores 0xFF85DD14 at movie_state[+0xA0]
2. For each frame (message 6) → sub_FF85D98C_my:
   a. BLX movie_state[+0xA0] → calls 0xFF85DD14 (pre-frame callback, purpose TBD)
   b. BL sub_FF92FE8C(sp+0x34, sp+0x30, sp+0x2C, sp+0x28)
      → 4 output pointers: (jpeg_ptr, jpeg_size, meta1, meta2)
      → Returns 0 on failure, non-zero with JPEG data on success
   c. Passes JPEG data to sub_FF8EDBE0 (AVI frame encoder)
```

**sub_FF92FE8C** is the direct JPEG frame getter used during movie recording. It may be usable from the webcam module as an alternative to `GetContinuousMovieJpegVRAMData`, but likely requires the recording pipeline to be properly initialized.

#### JPCORE Input Data Path — Root Cause Found (2026-02-11)

**Problem solved**: The FrameProcessing function (`FUN_ff9e5328`) dispatches to two different pipeline configuration paths based on `state[+0xD4]`:

| state[+0xD4] | FrameProcessing path | Pipeline config function | FUN_ffa02ddc mode | Effect |
|--------------|---------------------|------------------------|-------------------|--------|
| 0 or 1 | FUN_ff9e51d8 (EVF/LCD) | FUN_ffa03618 | **mode 4** | ISP → LCD only |
| 2 or 3 | FUN_ff9e508c (video recording) | FUN_ffa03bc8 | **mode 5** | ISP → LCD + **JPCORE input** |

**Mode 5 in FUN_ffa02ddc** configures the pipeline resizer to route processed frame data to JPCORE's hardware input bus (the source that the routing register at 0xC0F05040 connects to). Without mode 5, JPCORE is started but its input bus has no data — hence the hardware never fires its completion interrupt.

**Fix**: After calling `StartMjpegMaking`, set `state[+0xD4] = 2` to force the video recording FrameProcessing path on every pipeline frame. Both paths (EVF and video) end with the same callback registrations — the only functional difference is the pipeline routing mode (4 vs 5) and the pipeline config function.

**Safety analysis**: FUN_ff9e508c (video path) does NOT access movie_record_task state (DAT_ff85d02c). It only operates on pipeline configuration structures (DAT_ff9e4e60, DAT_ff9e5d28). Unlike the sub_FF8C3BFC crash (which set function pointers that got called from ISR context), this change only affects which pipeline routing function is called during FrameProcessing — well within the normal firmware code path.

#### Decompiled Functions (jpcore_input2_decompiled.txt)

| Address | Name | Size | Key Finding |
|---------|------|------|-------------|
| `0xFF92FE8C` | sub_FF92FE8C | 552B | Movie frame getter — operates on DAT_ff93050c ring buffer state. Returns JPEG ptr/size via 4 output params. Error codes: 0x80000001 (frame limit), 0x80000003 (allocation), 0x80000005 (buffer full), 0x80000007 (overflow). |
| `0xFFA08764` | FUN_ffa08764 | 300B | SumiaMan — post-JPCORE image quality/resize configuration. Maps param to modes 0/1/2. Not involved in JPCORE input routing. |
| `0xFFA08D40` | FUN_ffa08d40 | 308B | RshdMan — post-JPCORE distortion correction. Maps param to modes 0/1/2. Not involved in JPCORE input routing. |
| `0xFF9E508C` | FUN_ff9e508c | 332B | **Video recording FrameProcessing** — calls FUN_ffa03bc8 (pipeline config) + FUN_ffa02ddc(mode=5). **Mode 5 routes data to JPCORE.** |
| `0xFF9E51D8` | FUN_ff9e51d8 | 336B | **EVF FrameProcessing** — calls FUN_ffa03618 (pipeline config) + FUN_ffa02ddc(mode=4). Mode 4 = LCD only. |
| `0xFF8C4208` | FUN_ff8c4208 | 48B | DMA trigger — sets state[+0x5C]=3 (request), state[+0x58]=frame_index, state[+0xA0]=callback, state[+0x64]=VRAM buffer addr. |
| `0xFF85DD14` | FUN_ff85dd14 | 1B | Movie pre-frame callback — **NOP** (`return;`). Does nothing before sub_FF92FE8C. |
| `0xFF9E8190` | FUN_ff9e8190 | 16B | JPCORE enable — just sets `*DAT_ff9e8250 = 1`. That's all StartMjpegMaking does (beyond state[+0x48]=1). |
| `0xFF9E81A0` | FUN_ff9e81a0 | 60B | JPCORE disable — sets `*DAT_ff9e8250 = 0`, waits for semaphore, cleanup. |

#### Webcam Module — Current hw_mjpeg_start() Sequence (v8)

```c
// 1. Switch to video mode (activates EVF pipeline)
// 2. msleep(500) — let pipeline stabilize
// 3. StartMjpegMaking (0 args) — activate JPCORE, set state[+0x48]=1
// 4. Set state[+0xD4] = 2 — force video recording FrameProcessing path
//    (routes ISP data to JPCORE input via FUN_ffa02ddc mode 5)
// 5. Wait for JPCORE completion mask == 6 with piVar1[3] == 1
// 6. GetContinuousMovieJpegVRAMData — trigger DMA, wait for JPCORE frame
```

**v8 test result** (2026-02-12): `state[+0xD4]=2` took effect (confirmed in diagnostics). Camera did NOT crash. BUT:
- **LCD went black** with thin horizontal red/blue lines after a few seconds — confirms FUN_ff9e508c IS being called and IS reconfiguring the display pipeline (for movie recording format, incompatible with EVF display)
- **JPCORE still produced no JPEG output** — SOI scan at piVar1[4] and 0x40EA23D0 both found nothing
- Wait loop completed immediately (retries=0) — cmask=6 fix works correctly
- `state[+0x5C] = 5` (DMA never requested — GetContinuousMovieJpegVRAMData was NOT called in v8)
- VRAM@0x40EA23D0 had YUV-looking data (`01 64 07 62 5D 57...`), not JPEG
- **Key finding**: v8 tested state[+0xD4]=2 WITHOUT GetContinuousMovieJpegVRAMData. Pre-v8 tested GetContinuousMovieJpegVRAMData WITHOUT state[+0xD4]=2. Neither alone works. v9 combines both.

#### Pipeline Config Decompilation (pipeline_config_decompiled.txt)

Decompilation of FUN_ffa02ddc and 6 related pipeline configuration functions revealed a **critical architectural finding**:

**Both FUN_ff9e508c (video) and FUN_ff9e51d8 (EVF) are nearly identical.** The ONLY differences are:
1. `FUN_ffa03bc8` (VideoRecPipelineSetup, 836 bytes) vs `FUN_ffa03618` (EVFPipelineSetup, 1236 bytes) — different ISP scaler register blocks
2. `FUN_ffa02ddc(&local_3c, 5, ...)` vs `FUN_ffa02ddc(&local_3c, 4, ...)` — writes mode 5 vs 4 to a single ISP register

**Everything after these two calls is IDENTICAL in both paths**:
```c
// Same in both FUN_ff9e508c and FUN_ff9e51d8:
FUN_ffa0473c(5);
FUN_ff8f70f8(*puVar1, 1);                    // PipelineScalerConfig2
FUN_ff8f7110(2);                              // PipelineScalerConfig1
FUN_ff8f7128(param_3 != 1);
FUN_ff8efa6c(0, 0x11);                       // PipelineRouting — SAME routing in both!
FUN_ff9e4df8(0, DAT_ff9e5d2c, 0, 1);         // DMAInterruptSetup — SAME
FUN_ff8efabc_JPCORE_RegisterCallback(0, ...); // JPCORE callback — SAME
```

**Implication**: The JPCORE routing (`PipelineRouting(0, 0x11)`) is **identical** in both video and EVF paths. The difference is NOT about whether JPCORE gets routed — it's about the ISP scaler format. The VideoRecPipelineSetup configures the scaler output for JPCORE-compatible format (and breaks LCD display), while EVFPipelineSetup configures for LCD display format.

**FUN_ffa02ddc (PipelineConfig)**: For modes 4 and 5, both fall to the default case (copy dimensions directly, no scaling). The only hardware register difference is `DAT_ffa039f8 = 4` (EVF) vs `DAT_ffa039f8 = 5` (video). This single register likely controls the ISP resizer output format.

| Function | Address | Size | Purpose |
|----------|---------|------|---------|
| `PipelineConfig_FFA02DDC` | `0xFFA02DDC` | 292B | ISP resizer config — mode param → scaling factor + format register |
| `VideoRecPipelineSetup_FFA03BC8` | `0xFFA03BC8` | 836B | Video recording scaler — writes 9 registers at DAT_ffa04960-ffa04978 |
| `EVFPipelineSetup_FFA03618` | `0xFFA03618` | 1236B | EVF/LCD scaler — writes 2×7 register blocks at DAT_ffa0494c/ffa04950 |
| `PipelineRouting_FF8EFA6C` | `0xFF8EFA6C` | 20B | Route ISP stage → output; writes param_2 to `DAT_ff8f0020[param_1*4]` with shadow at 0x340000 |
| `DMAInterruptSetup_FF9E4DF8` | `0xFF9E4DF8` | 104B | DMA channel config — line stride, frame height, register block writes |
| `PipelineScalerConfig1_FF8F7110` | `0xFF8F7110` | 12B | Single register write to DAT_ff8f7314 (scaler mode) |
| `PipelineScalerConfig2_FF8F70F8` | `0xFF8F70F8` | 24B | Packed register write: `(param_2 << 16) | (param_1 - 1)` to DAT_ff8f7310 |

#### v9 Approach — Combined GetContinuousMovieJpegVRAMData + state[+0xD4]=2

**Hypothesis**: Previous tests failed because each change was tested in isolation:
- Pre-v8: GetContinuousMovieJpegVRAMData called → returned 0, but pipeline in EVF mode → scaler format wrong for JPCORE → no encoding
- v8: state[+0xD4]=2 set → pipeline in video mode (LCD corrupted) → but GetContinuousMovieJpegVRAMData NOT called → state[+0x5C]=5 (DMA never requested) → JPCORE output never captured to accessible buffer

**v9 combines both**: state[+0xD4]=2 (correct scaler format) AND GetContinuousMovieJpegVRAMData (synchronous DMA capture trigger).

```c
// hw_mjpeg_start():
// 1. Switch to video mode (activates EVF pipeline)
// 2. msleep(500) — let pipeline stabilize
// 3. StartMjpegMaking (0 args) — activate JPCORE, set state[+0x48]=1
// 4. Set state[+0xD4] = 2 — force video recording scaler format
// 5. Wait for JPCORE startup (mask=6, piVar1[3]=1)

// hw_mjpeg_get_frame():
// 1. Call GetContinuousMovieJpegVRAMData(frame_index) — sync DMA capture
//    (sets state[+0x5C]=3, waits for event flag, calls StopContinuousVRAMData)
// 2. Scan VRAM buffer (0x40EA23D0) for JPEG SOI
// 3. Scan piVar1[4] for JPEG SOI (fallback)
// 4. Scan JPCORE HW register 0xC0F04908 address (fallback)
// 5. Copy first valid JPEG frame to output buffer
```

#### v9 Test Results (2026-02-12)

**Result**: GetContinuousMovieJpegVRAMData returns 0 (success), but NO JPEG data in any buffer. DMA req state stays at 5 (never transitions to 3→4).

**Key diagnostics**:
- `GCMJVD return = 0` — function claims success
- `DMA req state = 5` — DMA_Trigger should set this to 3, StopContinuousVRAMData sets to 4, but we see 5 (unchanged)
- `source = 0` — no JPEG SOI found in VRAM buffer, piVar1[4], or JPCORE HW register address
- `rec callback 1 = 0x00000000` — pipeline NOT connected for recording output
- `rec buffer = 0x00000000` — no output buffer configured
- `PS3 completion mask = 6` — JPCORE_DMA_Start ran successfully
- `JPCORE HW 0xC0F04908` changes each frame (0x0070D600→0x0070D720→0x0070D660) — JPCORE hardware IS active
- LCD shows live preview with horizontal line artifacts (state[+0xD4]=2 causes video scaler format instead of EVF)

**Root cause**: Recording callbacks at state[+0x114/+0x118] are NULL, so JPCORE-encoded output is never transferred to an accessible buffer. The JPCORE IS encoding frames (confirmed by PS3 mask and changing HW register), but without the recording callbacks, nobody collects the output.

**Previous attempt that crashed**: Calling sub_FF8C3BFC with movie_record_task's callbacks (0xFF85D370, 0xFF85D28C, 0x1AB94) crashed the camera because the callbacks expect movie_record_task context (AVI ring buffer etc.) to be initialized.

#### Recording Pipeline Decompilation (2026-02-12)

New functions decompiled in `firmware-analysis/rec_pipeline_decompiled.txt`:

| Function | Address | Size | Purpose |
|----------|---------|------|---------|
| `RecPipelineSetup` | `0xFF8C3BFC` | 20 bytes | Trivial setter: writes 3 values to MJPEG state |
| `DMA_Trigger` | `0xFF8C4208` | 48 bytes | Sets state fields for DMA request (no hardware trigger) |
| `MovieFrameGetter` | `0xFF92FE8C` | 552 bytes | Reads JPEG from circular buffer, returns ptr+size |
| `MjpegActiveCheck` | `0xFF8C4288` | 12 bytes | Returns state[+0xEC] (pipeline active flag) |
| `StopContinuousVRAMData` | `0xFF8C425C` | 44 bytes | Clears DMA state, sets +0x5c=4 |
| `GetContMovieJpeg_Inner` | `0xFFAA2224` | 120 bytes | Checks active, calls DMA_Trigger, returns VRAM addr/size |
| `PipelineFrameCallback` | `0xFF8C1FE4` | 200 bytes | Per-frame ISR: Step0→Step1→MjpegCheck→Step2→Step3→FrameProc→VideoProc |
| `EVFSetup` | `0xFF8C3C64` | 48 bytes | Clears state[+0x38/3C/40], calls EVFSetupInner |
| `StartMjpegInner` | `0xFF8C3D38` | 84 bytes | Validates semaphore, sets +0x48=1, calls JPCORE_Enable |

**Critical Finding — sub_FF8C3BFC is trivial (20 bytes)**:
```c
void sub_FF8C3BFC(param_1, param_2, param_3) {
    state[+0x114] = param_1;  // rec callback 1 (function pointer)
    state[+0x6C]  = param_2;  // rec buffer (RAM address)
    state[+0x118] = param_3;  // rec callback 2 (function pointer)
}
```
In movie_record_task init: R0=0xFF85D370, R1=0x1AB94, R2=0xFF85D28C → these are ROM function addresses and a RAM buffer structure.

**Critical Finding — DMA_Trigger just sets state fields (48 bytes)**:
```c
void FUN_ff8c4208(param_1, param_2, param_3) {
    state[+0x120] = param_3;  // trigger context
    state[+0x58]  = param_1;  // frame index
    state[+0x5c]  = 3;        // DMA request state = REQUESTED
    state[+0x54]  = 0;        // clear status
    state[+0xa0]  = param_2;  // DMA completion callback
    state[+0x64]  = ROM_CONST; // VRAM buffer address
}
```
No hardware DMA is triggered here — just state flags. The actual hardware DMA must be triggered by the pipeline frame callback when it sees state[+0x5c]==3.

**Critical Finding — MovieFrameGetter uses separate data structure**:
`sub_FF92FE8C` accesses `DAT_ff93050c` (a circular buffer manager), NOT the MJPEG state at 0x70D8. It requires the recording callbacks to transfer JPCORE output → circular buffer first. Won't work without full recording pipeline initialization.

**Architecture Understanding**:
```
Normal movie recording flow:
1. movie_record_task init case → sub_FF8C3BFC(callback, buffer, callback2)
   → Sets state[+0x114]=callback, state[+0x6C]=buffer, state[+0x118]=callback2
2. Per frame: PipelineFrameCallback → MjpegEncodingCheck → JPCORE encodes
3. JPCORE completion → calls state[+0x114] callback → transfers to ring buffer
4. movie_record_task frame case → sub_FF92FE8C() reads from ring buffer
5. Frame data → AVI container

Our webcam situation:
1. StartMjpegMaking → JPCORE encoding activated ✓
2. state[+0xD4]=2 → video recording scaler path ✓
3. PipelineFrameCallback → JPCORE encodes each frame ✓ (PS3 mask=6)
4. JPCORE completion → state[+0x114]=NULL → output DISCARDED ✗
5. GetContinuousMovieJpegVRAMData → waits for DMA that never fires → returns 0 immediately
```

**sub_FF8C3BFC with movie callbacks CRASHES even with pipeline in video mode (v10 test)**:
Tested calling sub_FF8C3BFC(0xFF85D370, 0x1AB94, 0xFF85D28C) AFTER StartMjpegMaking + state[+0xD4]=2 with JPCORE confirmed active. Camera still crashed (USB pipe error during start_webcam). The callbacks need the full movie_record_task context — not just the pipeline mode, but the entire AVI recording subsystem (ring buffer at DAT_ff93050c, frame counters, AVI writer state at 0x51A8, etc.). These callbacks are fundamentally incompatible with our use case. DO NOT retry.

**Why state[+0x5c]=5 persists**: GetContinuousMovieJpegVRAMData likely returns 0 without calling DMA_Trigger because:
- The event flag wait (timeout=0) may be non-blocking in DryOS, returning immediately
- Or the function has an early-return path we haven't decompiled in the outer wrapper

#### All Decompiled Functions (firmware-analysis directory)

| File | Functions |
|------|-----------|
| `getvram_decompiled.txt` | GetContinuousMovieJpegVRAMData, FUN_ffaa2224, StopContinuousVRAMData, StartMjpegMaking_Inner, StopMjpegMaking_Inner, DMA_Completion_Callback, EVF_FieldClear, RecordingPipelineSetup, MovieFrameGetter, JPCORE_FrameComplete, JPCORE_DMA_Start, EVF_pipeline_setup, EventFlagSet, EventFlagWait, EncodeFrame, PostEncode1/2 |
| `dma_pipeline_decompiled.txt` | FUN_ff8c4208_DMA_trigger, MjpegActiveCheck, JPCORE_enable/disable, RecordingPipelineSetup, MovieFrameGetter, EVF_init, EVF_pipeline_setup |
| `statemachine_decompiled.txt` | FUN_ff8c21c8 (+0x5C state machine), FUN_ff9e79c8, FUN_ff9e5adc, FUN_ff8ef950, FUN_ffad3d98, FUN_ff827584, FUN_ff8c1fe4 (pipeline frame callback) |
| `pipeline_loop_decompiled.txt` | FUN_ff8c2170, FUN_ff8c2938 (frame setup), FUN_ff8c41a8 (alt DMA), FUN_ff8c4238 (reset) |
| `jpcore_pipeline_decompiled.txt` | MjpegEncodingCheck, PipelineStep2/3, FrameProcessing, VideoModeProcessing, JPCORE_SetOutputBuf, JPCORE_RegisterCallback, JPCOREConfig, JPCORE_Interrupt_Handler |

#### v11 Series — Pipeline Callback Discovery & Software Path Working (2026-02-11)

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

#### v12 Series — 640x480 Video Pipeline Streaming Working (2026-02-11)

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

#### H.264 Spy Buffer Approach — Option D (2026-02-11 to 2026-02-15)

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

**Current problem (2026-02-15)**: The recording pipeline starts (movie_status=4) but stops after ~1 second. The `sub_FF85D98C_my` callback stops being called. After this, the camera stops responding to USB/PTP requests (bridge hangs on USB transfers).

**Root cause**: The AVI file write path in `sub_FF85D98C_my` — each frame involves `sub_FF8EDBE0` (AVI write) + semaphore wait (TakeSemaphore at `[0x51A8+0x14]` with 1s timeout). If the write fails or times out, the recording task state is set to 1 (stopping).

**Partial fix — GiveSemaphore keepalive**: Pre-signaling the AVI write semaphore with `GiveSemaphore(handle)` prevents TakeSemaphore from timing out. In one session this extended recording to 20+ frames (5 decoded at 2.3 FPS). However, results are inconsistent — subsequent tests show recording dying after ~1 second despite the keepalive. Possible causes: semaphore handle validation (handle at `[0x51A8+0x14]` may be garbage), SD card full (1.2 GB MOV from prior runs filled the 1.9 GB card), or `hw_mjpeg_start()` conflicting with the recording pipeline.

**Failed approaches to keep recording alive**:

| Approach | Result |
|----------|--------|
| Skip AVI write entirely (`B loc_FF85DCBC` after spy write) | Camera shuts off — skipped critical register/state setup |
| Zero JPEG size to skip AVI write naturally (`STR #0, [SP, #0x30]`) | Camera shuts off — same issue, critical state not maintained |
| Spy wait loop (200ms msleep in capture_frame_h264) | Only 2 frames, then camera stops responding |
| Increased bridge polling interval (33ms sleep) | 5 frames, still stops |
| GiveSemaphore keepalive (100 pre-signals) | Inconsistent: 20+ frames in one session, ~1s in others |
| GiveSemaphore keepalive (10 pre-signals) | Recording dies immediately (not enough runway) |
| Re-enable `hw_mjpeg_start()` before recording | Recording dies after ~1s (conflicts with pipeline JPCORE setup) |

**Important operational notes**:
- **SD card space**: Recording writes real MOV files to the SD card. Each recording session creates a ~1 GB MOV file. Delete old MOV files from `DCIM/100CANON/` regularly to prevent SD card full errors.
- **`hw_mjpeg_start()` must NOT be called before `UIFS_StartMovieRecord`**: It conflicts with the recording pipeline's own JPCORE setup and causes frames to stop after 1-2 seconds. The committed version correctly skips it.
- **Semaphore handle validation**: The handle at `[0x51A8+0x14]` must be validated (not 0, not 0xFFFFFFFF, within RAM range) before calling GiveSemaphore, or the camera crashes.

**Files involved**:
- `chdk/platform/ixus870_sd880/sub/101a/movie_rec.c` — spy buffer hooks in `sub_FF85D98C_my` (inline ASM)
- `chdk/modules/webcam.c` — `capture_frame_h264()`, recording start/stop via `UIFS_StartMovieRecord`
- `bridge/src/webcam/h264_decoder.cpp` — FFmpeg AVCC-to-Annex-B converter + decoder
- `bridge/src/webcam/h264_decoder.h` — H.264 decoder header

### Future Ideas (Not Yet Implemented)

#### Raw YUV Pipeline Streaming (640x480)

Stream raw 640x480 YUV frames from the video pipeline directly to the PC, bypassing on-camera JPEG encoding entirely. This could dramatically increase FPS since the ARM926 CPU would only need to memcpy the frame data, not encode it.

**Approach**:
- Camera: capture `rec_cb_arg2` buffer (640x480 UYVY, ~614 KB/frame) from pipeline callback, send raw bytes over PTP
- PC bridge: receive raw YUV data, encode to JPEG using libjpeg-turbo (PC encodes ~1000x faster than ARM926)

**Bandwidth analysis**: 614 KB × 5 fps = 3 MB/s. USB 2.0 High Speed theoretical max ~40 MB/s. Should be feasible.

**Pros**: No encoding overhead on camera, potentially 5-15+ fps (limited only by USB transfer speed + memcpy)
**Cons**: ~614 KB per frame vs ~26 KB for JPEG (24x more USB bandwidth), needs bridge decoder changes

#### JPCORE Hardware Encoder — Status: BLOCKED

JPCORE hardware is confirmed initialized and processing frames (PS3 mask=6, piVar1[3]=1, HW registers changing). But the encoded JPEG output is DISCARDED because state[+0x114] was set to our spy callback instead of a real frame delivery callback. The original movie_record_task callbacks CRASH the camera because they need full AVI recording context. A custom lightweight callback that captures JPCORE output without crashing is the missing piece — but the JPCORE output format and delivery mechanism from FUN_ff8c335c are not yet fully understood.
