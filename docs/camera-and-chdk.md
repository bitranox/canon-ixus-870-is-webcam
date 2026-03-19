# Camera & CHDK Reference

> Back to [README](../README.md)

## Firmware Upgrade

For step-by-step firmware upgrade and CHDK installation instructions, see [Getting Started — Camera Setup](getting-started.md#camera-setup-first-time).

**Key detail for this camera:** The "Firmware Ver." menu item is HIDDEN at the bottom of the Settings menu. Press **UP arrow** to jump past "Reset All" to reveal it. SD card must be **unlocked**.

## CHDK (Canon Hack Development Kit)

CHDK is an optional third-party firmware enhancement that runs alongside the original Canon firmware. It does not overwrite or replace the factory firmware.

- **Supported firmware versions**: 1.00e, 1.01a, 1.02b
- **Features**: RAW/DNG shooting, scripting (uBASIC/Lua), extended bracketing, live histogram, zebra mode, override shutter/aperture/ISO, and more.
- **Installation**: Load CHDK onto a bootable SD card; it runs from the card without modifying the camera's internal firmware. See [Getting Started — CHDK Installation](getting-started.md#chdk-installation) for step-by-step instructions.
- **Compatibility**: You must download the CHDK build that matches your exact firmware version.

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

### Using as a Webcam

#### This project's approach (H.264 bridge, 30 FPS)

This project streams H.264 video at 640x480@30fps by running the camera's native recording pipeline and intercepting encoded frames via a custom CHDK module. A PC-side bridge (`chdk-webcam.exe`) decodes the H.264 stream and presents it as a DirectShow virtual webcam device visible in Zoom, Teams, OBS, etc. See [README](../README.md) and [Getting Started](getting-started.md) for setup.

- **30 FPS** from the actual video encoder (not LCD refresh)
- **640x480 native**, upscaled to 1280x720 on PC
- **Zoom control** from the preview window (+/- keys, mouse wheel)
- **Zero-copy** frame delivery, no on-camera processing overhead

#### Alternative: chdkptp live view (generic CHDK, ~5-10 FPS)

CHDK also supports a generic live view feed via the **PTP Extension** and the **chdkptp** client. This captures the camera's LCD buffer, not the video encoder output.

- **Reference**: https://chdk.fandom.com/wiki/PTP_Extension

**Limitations of chdkptp approach** (not applicable to this project):

- Frame rate tied to the camera's LCD refresh (~5-10 FPS, not 30 FPS)
- Resolution limited to the LCD buffer, not the full sensor resolution
- Requires OBS to capture the chdkptp window for use as a virtual webcam
- Live view may stop updating if the camera LCD turns off

### Known CHDK Issues

- DNG file colors may be slightly misaligned.
- Brief power-on button presses can trigger review mode instead of normal startup.
- Optical/digital zoom transitions in video mode require releasing the zoom lever between steps.
- CHDK is experimental software — no confirmed camera damage reports, but provided without warranty.
