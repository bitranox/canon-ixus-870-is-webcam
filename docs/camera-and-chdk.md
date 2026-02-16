# Camera & CHDK Reference

> Back to [CLAUDE.md](../CLAUDE.md)

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
