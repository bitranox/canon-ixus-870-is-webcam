# Firmware Reverse Engineering (Ghidra)

> Back to [README](../README.md)

## Ghidra Project Setup

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

## Digic IV Memory Map

| Address Range | Region | Notes |
|---------------|--------|-------|
| `0x00000000`–`0x03FFFFFF` | RAM (DRAM) | ~64 MB, cached |
| `0x10000000`–`0x1FFFFFFF` | TCM / Cache | Tightly coupled memory |
| `0x40000000`–`0x43FFFFFF` | Uncached RAM mirror | Same physical RAM as 0x00000000, bypasses CPU cache |
| `0xC0000000`–`0xCFFFFFFF` | I/O registers | Hardware peripheral control |
| `0xFF800000`–`0xFFFFFFFF` | Flash ROM | Firmware code + read-only data |

## Digic IV Hardware Encoding Architecture

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

**Early conclusion (v0.0.1, WRONG — later disproved):**
> "H.264 frames cannot be used for webcam streaming because P-frames reference I-frames and the pipeline crashes when partially initialized."

**Actual solution (v35e):** H.264 streaming at 30 FPS works by running the **full, unmodified** recording pipeline (`UIFS_StartMovieRecord`) and intercepting encoded frames via a hook in the msg 6 handler (`spy_ring_write` in `movie_rec.c`). SD writes are suppressed by clearing `ring_buf+0x80` and using drain mode (`+0x88=1`). The H.264 GOP structure (~11 frames, IDR every ~0.4s) provides sufficient keyframes for the PC-side FFmpeg decoder. See [Proven Facts](proven-facts.md) for the complete architecture.

---

## Hardware MJPEG Encoder — Reverse Engineering Results (Legacy Investigation)

> **LEGACY — This section documents the failed hardware JPEG encoding approach (v4–v10, Feb 2026). The project moved to H.264 direct streaming via spy_ring_write (v22+). This content is preserved for historical reference only. For the current architecture, see [Architecture](architecture.md) and [Development Log](development-log.md).**

The firmware functions below were extensively investigated as potential hardware JPEG encoding paths. They are documented here for reference, but **do not produce JPEG output** on the IXUS 870 IS because the video pipeline routes to JP62 (H.264), not JPCORE (JPEG).

### Firmware Function Addresses (fw 1.01a)

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

### GetContinuousMovieJpegVRAMData — Detailed Calling Convention

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

### JPCORE VRAM Buffer

The hardware JPEG encoder writes frames via DMA to a fixed uncached RAM buffer. These constants are embedded in the firmware ROM literal pool.

| Constant | ROM Address | Value | Meaning |
|----------|------------|-------|---------|
| VRAM buffer address | `0xFFAA2314` | `0x40EA23D0` | Uncached RAM (physical: `0x00EA23D0`) |
| VRAM buffer max size | `0xFFAA2318` | `0x000D2F00` | 864,000 bytes (~844 KB) |

- The buffer address is in the **uncached RAM mirror** (`0x40000000` + offset), ensuring CPU reads always see the latest DMA-written data without cache coherency issues.
- The max size (844 KB) is the total buffer allocation; actual JPEG frames are typically 30–100 KB for 640x480.
- The **actual frame size** is determined by scanning for the JPEG EOI marker (`FF D9`) in the buffer data. In entropy-coded JPEG, `FF` bytes are always followed by `00` (byte stuffing), so `FF D9` uniquely identifies the end of image.
- Buffer contents remain valid after `GetContinuousMovieJpegVRAMData` returns (the DMA is stopped but RAM is not cleared) until the next frame capture overwrites it.

### StartEVFMovVGA — Video Mode Parameters

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

### MJPEG State Machine

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

### Movie Recording Task — Frame Capture Pattern

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

### NHSTUB Entries Added to stubs_entry.S

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

### Webcam Module Hardware Path — Current Implementation

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

### Known Uncertainties / Not Yet Tested

- **VRAM buffer interpretation**: The value `0x40EA23D0` at ROM `0xFFAA2314` is assumed to be the direct buffer address. If it's actually a pointer-to-pointer (an additional level of indirection that Ghidra's decompiler may have collapsed), the actual buffer address would be `*(uint32_t *)0x40EA23D0`. On-camera testing will reveal which interpretation is correct.
- **Frame size determination**: The EOI scan approach works for well-formed JPEG but adds O(n) overhead per frame. If a hardware register or global variable stores the actual encoded size, it would be faster. Candidate: the MJPEG state structure at `DAT_ff8c2e24` may have a frame size field.
- **GetContinuousMovieJpegVRAMData blocking behavior**: The event flag wait has timeout=0 (possibly infinite). If the JPCORE never fires the callback (e.g., encoder not properly initialized), the call will hang. The 500ms `msleep()` after mode switch in `webcam_start()` is intended to let the pipeline stabilize, but may need tuning.
- **Repeated calls**: Each call to `GetContinuousMovieJpegVRAMData` internally calls `StopContinuousVRAMData` after getting one frame. It's unclear if this causes performance overhead for repeated single-frame captures vs. a true continuous mode.
- **Alternative: `sub_FF92FE8C`**: The movie recording task uses this unnamed function which clearly returns JPEG data through 4 output pointers. If `GetContinuousMovieJpegVRAMData` doesn't work well for streaming, this function could be tried, though it may require full movie recording pipeline setup.

### Hardware Encoder Debugging Status (2026-02-11)

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

### On-Device Testing Results (2026-02-11)

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

### Recording Pipeline Setup — sub_FF8C3BFC

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

### Failed Approaches to Activate Hardware Encoder

| # | Approach | What It Does | Why It Failed |
|---|----------|-------------|---------------|
| 1 | `set_movie_status(2)` | Sets `*(0x51E4) = 2` | Only sets a variable in movie_record_task's state struct. Doesn't trigger the firmware task or call sub_FF8C3BFC. |
| 2 | `levent_set_record()` | Posts "PressRecButton"/"UnpressRecButton" UI events via `PostLogicalEventToUI()` | UI event loop doesn't process recording events when camera is connected via USB in PTP mode. movie_status stayed at 0 after the call. |
| 3 | Direct `sub_FF8C3BFC` call | Calls `sub_FF8C3BFC(0xFF85D370, 0x1AB94, 0xFF85D28C)` directly via `call_func_ptr()` to populate recording callbacks | **CRASHED camera within 1 second.** FUN_ff8c335c calls state[+0x114] as a function pointer on every pipeline frame. The movie_record_task callbacks expect R4=0x51A8 context which isn't set up → immediate crash. |

### Recording Callbacks — FUN_ff8c335c Analysis

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

### Webcam Module — Current hw_mjpeg_start() Sequence

```c
// 1. Switch to video mode (activates EVF pipeline)
// 2. msleep(500) — let pipeline stabilize
// 3. StartMjpegMaking (0 args) — activate JPCORE, set state[+0x48]=1
// 4. Wait for JPCORE completion mask == 7 (up to 4 seconds)
// 5. GetContinuousMovieJpegVRAMData — trigger DMA, wait for JPCORE frame
```

**Status**: v7 build deployed — reverted sub_FF8C3BFC crash, restored StartMjpegMaking. Camera runs without crashing; JPCORE reports active but produces no JPEG output.

**Remaining mystery**: JPCORE hardware is configured and started (DMA_Start returns 0=success, piVar1[3]=1, registers changing) but never fires its completion interrupt. The JPCORE output buffer is programmed. But no JPEG data appears. The **input data path** to JPCORE remains the unsolved problem.

### Pipeline Frame Callback — Call Chain Analysis (2026-02-11)

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

### PS3 Completion Mask — Corrected Interpretation

The PipelineStep3 completion mask at `DAT_ff8f9028 + 0x0C`:
```
Bit 0: Set if JPCORE_DMA_Start returns 1 (FAILURE)
Bit 1: Always set (pre-JPCORE flag = 1)
Bit 2: Always set (post-processing flag = 1)
```

- **Mask = 6** (binary 110): NORMAL — JPCORE_DMA_Start succeeded (returned 0)
- **Mask = 7** (binary 111): JPCORE_DMA_Start FAILED (returned 1)
- The previous interpretation (mask=7 = success) was **incorrect**

### movie_record_task — Frame Acquisition Flow

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

### JPCORE Input Data Path — Root Cause Found (2026-02-11)

**Problem solved**: The FrameProcessing function (`FUN_ff9e5328`) dispatches to two different pipeline configuration paths based on `state[+0xD4]`:

| state[+0xD4] | FrameProcessing path | Pipeline config function | FUN_ffa02ddc mode | Effect |
|--------------|---------------------|------------------------|-------------------|--------|
| 0 or 1 | FUN_ff9e51d8 (EVF/LCD) | FUN_ffa03618 | **mode 4** | ISP → LCD only |
| 2 or 3 | FUN_ff9e508c (video recording) | FUN_ffa03bc8 | **mode 5** | ISP → LCD + **JPCORE input** |

**Mode 5 in FUN_ffa02ddc** configures the pipeline resizer to route processed frame data to JPCORE's hardware input bus (the source that the routing register at 0xC0F05040 connects to). Without mode 5, JPCORE is started but its input bus has no data — hence the hardware never fires its completion interrupt.

**Fix**: After calling `StartMjpegMaking`, set `state[+0xD4] = 2` to force the video recording FrameProcessing path on every pipeline frame. Both paths (EVF and video) end with the same callback registrations — the only functional difference is the pipeline routing mode (4 vs 5) and the pipeline config function.

**Safety analysis**: FUN_ff9e508c (video path) does NOT access movie_record_task state (DAT_ff85d02c). It only operates on pipeline configuration structures (DAT_ff9e4e60, DAT_ff9e5d28). Unlike the sub_FF8C3BFC crash (which set function pointers that got called from ISR context), this change only affects which pipeline routing function is called during FrameProcessing — well within the normal firmware code path.

### Decompiled Functions (jpcore_input2_decompiled.txt)

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

### Webcam Module — Current hw_mjpeg_start() Sequence (v8)

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

### Pipeline Config Decompilation (pipeline_config_decompiled.txt)

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

### v9 Approach — Combined GetContinuousMovieJpegVRAMData + state[+0xD4]=2

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

### v9 Test Results (2026-02-12)

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

### Recording Pipeline Decompilation (2026-02-12)

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

### All Decompiled Functions (firmware-analysis directory)

| File | Functions |
|------|-----------|
| `getvram_decompiled.txt` | GetContinuousMovieJpegVRAMData, FUN_ffaa2224, StopContinuousVRAMData, StartMjpegMaking_Inner, StopMjpegMaking_Inner, DMA_Completion_Callback, EVF_FieldClear, RecordingPipelineSetup, MovieFrameGetter, JPCORE_FrameComplete, JPCORE_DMA_Start, EVF_pipeline_setup, EventFlagSet, EventFlagWait, EncodeFrame, PostEncode1/2 |
| `dma_pipeline_decompiled.txt` | FUN_ff8c4208_DMA_trigger, MjpegActiveCheck, JPCORE_enable/disable, RecordingPipelineSetup, MovieFrameGetter, EVF_init, EVF_pipeline_setup |
| `statemachine_decompiled.txt` | FUN_ff8c21c8 (+0x5C state machine), FUN_ff9e79c8, FUN_ff9e5adc, FUN_ff8ef950, FUN_ffad3d98, FUN_ff827584, FUN_ff8c1fe4 (pipeline frame callback) |
| `pipeline_loop_decompiled.txt` | FUN_ff8c2170, FUN_ff8c2938 (frame setup), FUN_ff8c41a8 (alt DMA), FUN_ff8c4238 (reset) |
| `jpcore_pipeline_decompiled.txt` | MjpegEncodingCheck, PipelineStep2/3, FrameProcessing, VideoModeProcessing, JPCORE_SetOutputBuf, JPCORE_RegisterCallback, JPCOREConfig, JPCORE_Interrupt_Handler |
