# Proven Facts — Canon IXUS 870 IS Webcam Project

Last updated: 2026-03-01

This document contains ONLY verified, tested facts. No speculation, no history.
Each fact includes the evidence that proved it.

## Camera Hardware

| Fact | Value | Evidence |
|------|-------|----------|
| Processor | ARM926EJ-S (Digic IV) | Firmware analysis, CHDK docs |
| Byte order | Little-endian | Debug frame byte reads match LE interpretation |
| Video output | H.264 Baseline Profile, Level 3.1, 640x480 @ 30fps | MOV file analysis, bridge NAL parsing |
| NAL format in ring buffer | AVCC (4-byte BE length prefix) for P-frames | Bridge hex dumps: `00 00 8F C0 61 ...` |
| First-frame format | Annex B (start codes `00 00 00 01`) for SPS/PPS/IDR | Debug probe: NAL0=0x01000000, NAL4=0x1FE04267 |

## H.264 Parameters (from camera output)

| Parameter | Value | Evidence |
|-----------|-------|----------|
| SPS bytes | `67 42 E0 1F DA 02 80 F6 9B 80 80 83 01` (13 bytes) | MOV avcC atom extraction, confirmed by +0xC0 probe |
| PPS bytes | `68 CE 3C 80` (4 bytes) | MOV avcC atom extraction |
| IDR NAL type | 0x65 (type 5) | MOV file analysis |
| P-frame NAL type | 0x61 (type 1) | Bridge hex dumps, hundreds of frames |
| Typical P-frame size | 35-46 KB | Bridge frame stats (36804, 37372, 42900, etc.) |
| IDR frame size | ~53 KB | Debug probe: ISiz = 0x0000CFE8 |

## Ring Buffer Structure (base = 0x8968)

The ring buffer struct is always at RAM address 0x8968. Confirmed by reading
`*(0xFF93050C)` which consistently returns 0x8968 across all tests.

| Offset | Address | Type | Value (observed) | Description | Evidence |
|--------|---------|------|-------------------|-------------|----------|
| +0x1C | 0x8984 | ptr | (varies) | Current read pointer (advances each frame) | MovieFrameGetter decompilation line 971 |
| +0x28 | 0x8990 | uint | 0→1→2→... | Frame counter (incremented by MovieFrameGetter) | Debug probe: FCnt=2 on second call |
| +0x40 | 0x89A8 | uint | | Max frame count | MovieFrameGetter decompilation line 918 |
| +0x70 | 0x89D8 | uint | 0x00040000 (256KB) | Frame buffer capacity (NOT individual frame size) | Debug probe: FSiz=0x40000 |
| +0xC0 | 0x8A28 | ptr | 0x412C4720 | **First-frame pointer** — SPS+PPS+IDR data | Debug probe: FPtr=0x412C4720, bytes confirm SPS |
| +0xC4 | 0x8A2C | ptr | | Alternate/wrap buffer pointer | MovieFrameGetter decompilation line 973 |
| +0xC8 | 0x8A30 | ptr | | Buffer end pointer | MovieFrameGetter decompilation line 970 |
| +0xD4 | 0x8A3C | uint | | Running data offset (for MOV sample table) | MovieFrameGetter decompilation line 965 |
| +0x80 | 0x89E8 | uint | 0 or 1 | **task_MovWrite is_open flag**: 1=file open, 0=skip writes | Ghidra decompilation + v26g test |
| +0xD8 | 0x8A40 | uint | 0x000158AC (88236) | IDR offset in data area (MOV container metadata) | Multiple debug probes, consistent value |
| +0xDC | 0x8A44 | uint | 0x0000CFE8 (53224) | IDR size in data area | Debug probe: ISiz=0xCFE8 |

## Memory Addresses

| Address | Type | Value | Description | Evidence |
|---------|------|-------|-------------|----------|
| 0x8968 | RAM | struct | Ring buffer struct base | `*(0xFF93050C)` = 0x8968, all probes |
| 0x8DE4 | RAM | ptr | Data area base pointer (only valid after msg 5) | `*(0xFF930C78)` = 0x8DE4, debug probe D_00 |
| 0x412C4720 | VRAM/DMA | data | SPS+PPS+IDR frame data (Annex B) | +0xC0 probe, NAL bytes match SPS |
| 0x413EE010 | VRAM/DMA | data | H.264 data area (context_base + 0x200040) | DMA trace from v22b |
| 0x51A8 | RAM | struct | Movie record state struct | movie_rec.c asm: `LDR R4, =0x51A8` |
| 0xFF000 | RAM | shared | Webcam shared memory (magic, ptr, size, count, sem) | webcam.c protocol, all bridge tests |
| 0xFF040 | RAM | queue | Debug frame SPSC queue (4 slots x 512 bytes) | debug-frame-protocol.md |

## ROM Constants

| ROM Address | Points to | Description | Evidence |
|-------------|-----------|-------------|----------|
| 0xFF93050C | 0x8968 | Ring buffer struct pointer | Multiple debug reads |
| 0xFF930C78 | 0x8DE4 | Data area pointer storage | Debug probe: D_00=0x8DE4 |
| 0xFF85D6A4 | (context base ptr) | Movie record context base | msg5_handler_decompiled.txt |

## Firmware Functions

| Address | Name | Size | Purpose | Evidence |
|---------|------|------|---------|----------|
| 0xFF85D3BC | sub_FF85D3BC | 680 | Msg 5 handler: IDR encode + MOV header write | Ghidra decompilation |
| 0xFF85D98C | sub_FF85D98C | ~600 | Msg 6 handler: P-frame processing + file write | Ghidra decompilation, movie_rec.c |
| 0xFF92FE8C | MovieFrameGetter | 552 | Returns frame pointer/size from ring buffer | idr_architecture_decompiled.txt |
| 0xFF930B04 | FUN_ff930b04 | 28 | Sets data area base at 0x8DE4 | msg5_functions_decompiled.txt |
| 0xFF93048C | FUN_ff93048c | 24 | Reads +0xD8/+0xDC (IDR offset/size) | msg5_functions_decompiled.txt |
| 0xFF930B20 | FUN_ff930b20 | 344 | MOV container header writer | msg5_functions_decompiled.txt |
| 0xFF8EDDFC | FUN_ff8eddfc | 560 | First frame encode (JPCORE pipeline setup) | msg5_functions_decompiled.txt |
| 0xFF85DD14 | (callback stub) | ~4 | No-op callback at +0xA0 (set by msg 11) | Ghidra, callback_usage_decompiled.txt |
| 0xFF8C3BFC | RecPipelineSetup | | Recording pipeline initialization | movie_rec.c asm, msg5 decompilation |
| 0xFF8EDBE0 | sub_FF8EDBE0 | | Async SD card write submit; writes actual frame size to [SP,#0x3C] | Debug probe: EDot matches frame size |
| 0xFF8EDC88 | sub_FF8EDC88 | | SD write cleanup; called with arg 1 (first chunk) and 0 (final) | Decompilation, msg 6 flow |
| 0xFF9300B4 | sub_FF9300B4 | | Ring buffer slot free: advances +0x1C read ptr by param_2 bytes | Debug probe + decompilation |
| 0xFF92F1EC | task_MovWrite | | DryOS task: receives ring buffer data via queue, writes to SD card | Ghidra decompilation (DecompileMovWrite.java) |
| 0xFF85235C | FUN_ff85235c | | File write: writes buffer to fd, returns bytes written | task_MovWrite case 2 decompilation |

## Message Flow (movie_record_task)

Messages are received on a queue at `*(0x51A8 + 0x1C)`. Message value minus 2 indexes the switch table.

| Message | Case | Handler | Fires during webcam? | Evidence |
|---------|------|---------|----------------------|----------|
| msg 2 | 0 | unlock_optical_zoom + sub_FF85DE1C | Yes (once at start) | Recording works |
| msg 3 | 1 | Check state, set stop flag | Unknown | |
| msg 4 | 2 | sub_FF85D6CC | Unknown | |
| msg 5 | 3 | sub_FF85D3BC (IDR encode) | **NO** | msg5_done=0 across 300+ debug frames |
| msg 6 | 4 | sub_FF85D98C_my (P-frame) | Yes (continuously, ~30fps) | Hundreds of frames received |
| msg 7 | 5 | sub_FF85D218 (stop recording) | Yes (at end) | Recording stops cleanly |
| msg 8 | 6 | sub_FF92FDF0 | Unknown | |
| msg 9 | 7 | NOP (fall through) | Unknown | |
| msg 10 | 8 | sub_FF85E28C | Unknown | |
| msg 11 | 9 | RecPipelineSetup, STATE=1 | Yes (once at start) | Recording initializes |

## State Machine (+0x3C at 0x51E4)

| State | Meaning | Evidence |
|-------|---------|----------|
| 1 | Initialized (set by msg 11) | movie_rec.c: `STR R5, [R4,#0x3C]` |
| 2 | Pipeline ready | Dev log analysis |
| 3 | First frame pending (callback promotes 2→3) | callback_usage_decompiled.txt |
| 4 | Recording active (frames processed) | movie_rec.c: msg 6 checks STATE==4 |
| 5 | Recording stopped | movie_rec.c: after stop sequence |

Our patch accepts STATE 3 or 4 (original firmware only accepts 4).

## Proven Problems

### 1. Msg 5 never fires during webcam recording
**Evidence**: msg5_done static variable stays 0 across 300+ debug frames over a full 20-second recording session. spy_msg5_debug (hooked after sub_FF85D3BC) never executes.
**Implication**: The IDR encoding path in msg 5 is NOT triggered by UIFS_StartMovieRecord.

### 2. IDR frame lost due to race condition
**Evidence**: Bridge receives 300+ frames, ALL are NAL type 0x61 (P-frame). Zero IDR frames (type 0x65). MovieFrameGetter's frame counter reaches 2+ by the time debug fires, confirming the first frame was consumed.
**Cause**: spy_ring_write checks `hdr[0] == 0x52455753` (webcam magic). The magic isn't set when the first msg 6 fires because the webcam module hasn't initialized shared memory yet. The first frame (IDR from +0xC0) is silently dropped.

### 3. First-frame data is Annex B, not AVCC
**Evidence**: Bytes at +0xC0 pointer: `00 00 00 01 67 42 E0 1F DA 02 80 F6` = Annex B start code + SPS NAL. Subsequent P-frames are AVCC format: `00 00 8F C0 61 ...` = 4-byte length prefix + P-frame NAL.
**Implication**: If we send the +0xC0 data as frame #0, the bridge must handle Annex B format for the first frame.

### 4. Shared memory (0xFF000) is corrupted by DMA
**Evidence**: Magic value at hdr[0] shows random garbage on PTP polls. Debug frames only survive when sent during msg 6 calls that coincide with PTP timing.
**Implication**: Cannot rely on shared memory for persistent state. Use statics in BSS instead.

### 5. Camera crashes are often USB-level, not code-level
**Evidence**: Same code (exact commit 08aba0d) crashes on one run, works after battery pull. Restarting the bridge (without battery pull) sometimes clears USB hangs.
**Implication**: Not all crashes indicate code bugs. Try bridge restart first, then battery pull.

### 6. SD write pipeline parameters (sub_FF9300B4)
**Evidence**: Debug probe at loc_FF85DC84 captured sub_FF9300B4's actual parameters:
- `[SP,#0x34]` (R0) = frame pointer from ring buffer (matches +0x1C read pointer)
- `[SP,#0x3C]` (R1) = actual frame size written by sub_FF8EDBE0 (e.g. 0xB000=45056 for IDR, 0x92F8=37624 for P-frame)
- `[SP,#0x30]` = always 0x40000 (256KB buffer capacity from MovieFrameGetter, NOT frame size)
- `+0xD4` data offset increments by `[SP,#0x3C]` each frame (cumulative byte counter)
**Implication**: sub_FF9300B4 cannot be called standalone — it depends on internal state set by sub_FF8EDBE0. Tested: sub_FF9300B4 alone (0 frames/crash), sub_FF8EDC88(0)+sub_FF9300B4 (crash after 1s), both with correct AVCC-derived sizes. The ring buffer freeing is tightly coupled to the SD write pipeline.

### 7. TakeSemaphore timeout sensitivity
**Evidence**: 50ms timeout → 347 decoded frames, stable. 1ms timeout → 1 frame then all gf_rc=-1.
**Cause**: 1ms is too short — sub_FF8EDBE0 hasn't started the async write, so returning fake success while the DMA state is inconsistent corrupts the pipeline.

### 8. SD write prevention via +0x80 flag clear
**Evidence**: Setting `*(0x89E8) = 0` (ring buffer +0x80) in spy_ring_write → 457 frames over 20 seconds, 0-byte MOV file, SD usage unchanged (9.4M / 1%), clean shutdown.
**Mechanism**: task_MovWrite case 2 checks `*(base + 0x80) == 1` before calling FUN_ff85235c (file write). When 0, it skips the write but still updates the consumed pointer at `+0x18`. The entire pipeline runs unmodified.
**Implication**: This is the deepest possible hook point. All ring buffer management, DMA, semaphores, and message posting run normally. Only the actual file I/O is prevented.

### 9. sub_FF9300B4 cannot be replaced or skipped
**Evidence**: Replacing sub_FF9300B4 with minimal bookkeeping (read pointer, frame counter, cumulative offset) → encoder stalls after 3-4 frames. Adding full bookkeeping (+0x64, +0x60, +0x148/+0x14C, +0x150/+0x154, wrap-around) → same stall. Skipping the SD pipeline entirely (sub_FF8EDBE0, TakeSemaphore, sub_FF8EDC88) → crash or stall.
**Cause**: sub_FF9300B4 handles undocumented internal state (sample tables, flush logic) that the H.264 encoder feedback loop depends on. The recording pipeline is tightly coupled — all components must run.

### 10. DryOS kernel signaling from movie_record_task breaks the recording pipeline
**Evidence**: Both DryOS message queues and binary semaphores cause catastrophic throughput drops when used to signal from spy_ring_write (movie_record_task context) to the PTP task.
- **Message queues** (v27a-v27d): CreateMessageQueue via call_func_ptr → 21-62 frames (vs 404 baseline). Even creating the queue without using it dropped to 62.
- **Binary semaphores** (v29a): CreateBinarySemaphore (stubbed, direct call) + GiveSemaphore from spy_ring_write → 20 frames in 20s (~1fps). Camera appeared normal but pipeline was severely throttled.
**Cause**: GiveSemaphore/TryPostMessageQueue from spy_ring_write triggers an immediate DryOS context switch to the PTP task (blocked on TakeSemaphore/ReceiveMessageQueue), preempting movie_record_task. The recording pipeline starves.
**Implication**: Cannot use any DryOS kernel signaling mechanism (queues, semaphores) from the recording pipeline for webcam frame delivery. Must use the seqlock approach with msleep(10) polling.

### 12. SPSC ring buffer with VRAM pointer indirection does not work
**Evidence**: 4-slot SPSC ring buffer storing {ptr, size} per slot → 66-71 frames in 20s (3.5-4.4fps) vs seqlock baseline ~24fps. Drain-to-latest variant (consumer skips to newest frame) performed equally poorly. Both tested with camera recording normally (red light solid, no crash).
**Cause**: The seqlock consumer is read-only (never writes to shared memory). The SPSC ring requires the consumer to write hdr[2] (rd_idx) to advance the read pointer. This bidirectional shared memory access between movie_record_task and PTP task introduces timing interference that destroys throughput. The exact mechanism is unclear (not cache coherency — single-core ARM shares cache), but the effect is reproducible and severe.
**Implication**: Frame delivery from movie_record_task to the PTP task must use the seqlock pattern where the consumer only reads. Any protocol requiring consumer writes to shared memory is not viable.

### 11. CPU cache protects shared memory from DMA corruption
**Evidence**: Uncached reads via 0x400FF000 → 26 decoded / 83 produced (seqlock reads garbage). Cached reads via 0x000FF000 → 349-404 decoded. spy_ring_write writes to cache, webcam.c reads from same cache (single-core ARM926).
**Mechanism**: JPCORE DMA corrupts physical memory at 0xFF000 (proven fact #4). The CPU cache holds the correct values written by spy_ring_write. msleep(10) between polls is for DryOS task scheduling (letting movie_record_task run), NOT for cache eviction.
**Implication**: Never use uncached alias for shared memory reads. The cached address (0x000FF000) is correct. Both msleep calls in the seqlock loop are for CPU yielding — removing either one starves the recording pipeline.

## Current Webcam Data Flow

```
JPCORE hardware
    │ encodes H.264 frames into VRAM/DMA buffer
    ▼
Ring buffer struct (0x8968)
    │ +0xC0 = first-frame pointer (SPS+PPS+IDR, Annex B)
    │ +0x1C = subsequent frame pointer (P-frames, AVCC)
    ▼
MovieFrameGetter (0xFF92FE8C)
    │ returns frame pointer + size to msg 6 handler
    ▼
sub_FF85D98C_my (msg 6 handler)
    │ calls spy_ring_write (delivers frame + prevents SD writes)
    │ then continues through SD write pipeline (all runs unmodified)
    ▼
spy_ring_write
    │ invalidates CPU cache for frame data (JPCORE DMA bypasses cache)
    │ stores {ptr, size} via seqlock at 0xFF000 (hdr[1..3])
    │ clears +0x80 (is_open) at 0x89E8 → prevents SD file writes
    ▼
SD write pipeline (runs unmodified)
    │ sub_FF8EDBE0 → TakeSemaphore → sub_FF8EDC88 → sub_FF9300B4
    │ → posts message to task_MovWrite queue
    ▼
task_MovWrite (0xFF92F1EC)
    │ case 2: checks +0x80 → finds 0 → skips FUN_ff85235c
    │ still updates consumed pointer (+0x18) → pipeline keeps flowing
    ▼
webcam.c (CHDK module)
    │ polls seqlock (100 × msleep(10)), memcpy from VRAM on match
    │ copies frame data to PTP response buffer
    ▼
PTP USB transfer → bridge → FFmpeg decode → virtual webcam
```

## Current Performance

| Metric | Value | Evidence |
|--------|-------|----------|
| Frames produced | 494 in 20s (~25 fps) | v30c bridge output |
| Frames decoded | 478 (97%) | v30c: 478/494 |
| Decoded FPS | 23.9 fps | v30c: measured first-to-last frame |
| Total FPS (incl. drops) | 28.4 fps | v30c bridge output |
| IDR keyframes | 39 (~2/sec from GOP) | v30c bridge output |
| Average bitrate | ~8 Mbps | v30c bridge output |
| SD card writes | 0 bytes | 0-byte MOV file, SD usage unchanged |
| Frame loss source | Seqlock overwrites + decode failures | ~2fps seqlock loss, ~4fps decode loss |

## What Needs to Happen Next

1. **Code cleanup**: Remove unused debug functions (spy_idr_capture, spy_msg5_debug statics). Remove leftover DryOS queue defines from webcam.c. Evaluate whether spy_take_sem_short is still needed with +0x80 approach.
2. **Virtual webcam integration**: Connect the H.264 decode output to a DirectShow virtual webcam filter for use in video conferencing apps.
