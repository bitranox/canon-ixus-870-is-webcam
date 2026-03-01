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
| IDR frame size | ~64 KB (bridge), ~53 KB (ring buffer probe) | v31a: IDR frames 64564-64636 bytes at bridge |
| GOP structure | ~11 frames (1 IDR + ~10 P-frames) | v31a: IDRs at cam#2,15,26,37,49 = every ~11 frames |
| IDR frequency | ~2.5/sec at 30fps | v31a: 5 IDRs in 2s. Camera produces IDRs autonomously — no re-injection needed |

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
| 0xFF8EDBE0 | sub_FF8EDBE0 | 168 | **JPCORE encode submission** (NOT SD write): stores params into encode state (0x7F6C), calls FUN_ff8eda90 to configure and trigger JPCORE hardware encode. param_14 = &[SP,#0x38] (result address) | v31 comprehensive decompilation |
| 0xFF8EDA90 | FUN_ff8eda90 | 448 | JPCORE hardware encode trigger: registers callbacks, configures output buffer/routing/mode, starts encode | v31 write_pipeline_decompiled.txt |
| 0xFF8ED6DC | FUN_ff8ed6dc | 144 | JPCORE encode completion callback: writes 0 to `*(encode_state+0x60)` = [SP,#0x38] (success), calculates encoded size | v31 callbacks_decompiled.txt |
| 0xFF8F18A4 | FUN_ff8f18a4 | 96 | JPCORE output handler: reads JPCORE output position, tracks encoded bytes, calls caller callback | v31 callbacks_decompiled.txt |
| 0xFF849168 | JPCORE_IRQ | 60 | JPCORE interrupt handler: clears interrupt, calls GiveSemaphore(*(DAT_ff84924c+0x1c)), calls registered callback | v31 write_pipeline_decompiled.txt |
| 0xFF8EDC88 | sub_FF8EDC88 | 60 | JPCORE post-encode cleanup: clears pipeline events, power management | Decompilation, msg 6 flow |
| 0xFF9300B4 | sub_FF9300B4 | | Ring buffer slot free: advances +0x1C read ptr by param_2 bytes | Debug probe + decompilation |
| 0xFF92F1EC | task_MovWrite | | DryOS task: receives ring buffer data via queue, writes to SD card | Ghidra decompilation (DecompileMovWrite.java) |
| 0xFF85235C | FUN_ff85235c | | File write: writes buffer to fd, returns bytes written | task_MovWrite case 2 decompilation |
| 0x3223EC | get_tick_count | 20 | DryOS millisecond tick counter (calls _GetSystemTime at 0x30F290) | main.bin.dump: `003223ec <get_tick_count>:`, confirmed working in v32d stall detection |

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

### 2. First IDR may be lost due to race condition (but camera produces more)
**Evidence**: The very first IDR can be dropped because spy_ring_write checks `hdr[0] == 0x52455753` (webcam magic) which isn't set when the first msg 6 fires. However, the camera's H.264 encoder produces IDR keyframes autonomously every ~11 frames (~2.5/sec). v31a confirmed: 5 IDRs in 2s (cam#2,15,26,37,49), 100% decode rate, zero re-injection needed.
**Implication**: IDR re-injection is unnecessary. The camera's natural GOP provides sufficient IDRs for decoder sync.

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
**Cause**: 1ms is too short — sub_FF8EDBE0 hasn't started the JPCORE encode, so returning fake success while the encode state is inconsistent corrupts the pipeline. The TakeSemaphore waits for JPCORE hardware encode completion (~1-5ms per frame), NOT for SD card writes.

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

### 13. sub_FF8EDBE0 is JPCORE encode submission, NOT SD write
**Evidence**: Comprehensive 3-level decompilation of sub_FF8EDBE0, FUN_ff8eda90, and all callees (206 functions, v31 analysis). FUN_ff8eda90 configures JPCORE hardware registers (JPCORE_SetOutputBuf, PipelineRouting, JPCORE_RegisterCallback), sets encode mode, and triggers hardware encode. No SD card or DMA-to-SD operations whatsoever.
**Key addresses**:
- Encode state struct: 0x7F6C (DAT_ff8ed984) — stores all 14 parameters from sub_FF8EDBE0
- JPCORE encode state: 0x80B4 (DAT_ff8f12ac) — tracks JPCORE hardware state
- JPCORE completion callback: FUN_ff8ed6dc (writes 0 to caller's result location on success)
- JPCORE output handler: FUN_ff8f18a4 (tracks encoded byte count)
- JPCORE interrupt handler: 0xFF849168 (calls GiveSemaphore to signal encode completion)
**Implication**: TakeSemaphore after sub_FF8EDBE0 waits for JPCORE hardware encode (~1-5ms for a 40KB frame), NOT for SD writes. The original 1000ms timeout is ~200-1000x longer than needed.

### 14. Error path (STATE=1) is permanent pipeline death
**Evidence**: Disassembly of the +0xA0 callback chain:
- msg 11 init: callback = 0xFF85DD14 (BX LR, no-op), STATE=1
- msg 2 start: callback = 0xFF85DDA8 (one-shot setup), STATE=2
- First msg 6: 0xFF85DDA8 → replaces callback with 0xFF85DD18
- Second msg 6: 0xFF85DD18 → sets STATE=3, replaces callback with 0xFF85DD14 (no-op)
- All subsequent msg 6: callback = BX LR (no-op)

After normal startup, the callback is permanently 0xFF85DD14 (BX LR). When the error path sets STATE=1, no callback will ever promote it back to 3 or 4. All subsequent msg 6 calls check `STATE == 3 || STATE == 4` and fall through (skip all frame processing).
**Implication**: The error path (sub_FF930358 + STATE=1) must NEVER fire during webcam operation. A single error permanently kills the pipeline for the rest of the recording session.

### 16. Error path bypass keeps pipeline alive indefinitely
**Evidence**: v31b added `spy_skip_error_path()` check at all 4 error blocks in sub_FF85D98C_my. When webcam is active, skips sub_FF930358 (drain) + STATE=1 assignment, calls sub_FF8EDC88 (JPCORE cleanup) and continues. Result: camera recorded full 10s session (192 decoded, 19.2fps, 18 IDRs, 0 USB errors). Previously died after ~2s.
**Mechanism**: The error paths fire when `[SP,#0x38]` is non-zero (JPCORE callback hasn't written success yet due to spy_take_sem_short masking the timeout). Bypassing the drain + STATE=1 lets the pipeline continue — the JPCORE encode completes asynchronously and the next frame processes normally.
**Implication**: The error path bypass is safe because: (1) no SD writes to corrupt, (2) ring buffer flows via +0x80=0, (3) sub_FF8EDC88 cleans up JPCORE state, (4) encode completes asynchronously.

### 15. JPCORE semaphore signaling chain
**Evidence**: Full trace from v31 decompilation:
1. sub_FF8EDBE0 stores &[SP,#0x38] at encode_state+0x60
2. FUN_ff8eda90 registers FUN_ff8ed6dc as JPCORE pipeline 3 callback
3. JPCORE hardware encodes frame
4. JPCORE interrupt handler (0xFF849168): calls GiveSemaphore(*(DAT_ff84924c+0x1c))
5. FUN_ff8ed6dc: writes 0 to *(*(encode_state+0x60)) = [SP,#0x38] (success result)
6. TakeSemaphore returns in msg 6 handler
7. msg 6 checks [SP,#0x38]: 0 = success, non-zero = error
**Implication**: The semaphore is signaled by JPCORE hardware interrupt completion. The result variable [SP,#0x38] is written by FUN_ff8ed6dc from interrupt context. If TakeSemaphore times out before the callback fires, [SP,#0x38] has its previous value (not guaranteed to be 0).

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
    │ AVCC-parses frame to get exact H.264 size (not 256KB chunk)
    │ stores {ptr, actual_size} via dual-slot seqlock at 0xFF000
    │ (alternates hdr[1..3] / hdr[4..6] — hdr[7..9] MUST NOT be written)
    │ clears +0x80 (is_open) at 0x89E8 → prevents SD file writes
    ▼
JPCORE encode pipeline (runs unmodified)
    │ sub_FF8EDBE0 → JPCORE hardware encode → TakeSemaphore (waits ~1-5ms)
    │ → sub_FF8EDC88 (cleanup) → sub_FF9300B4 (ring buffer free)
    │ → posts message to task_MovWrite queue
    ▼
task_MovWrite (0xFF92F1EC)
    │ case 2: checks +0x80 → finds 0 → skips FUN_ff85235c
    │ still updates consumed pointer (+0x18) → pipeline keeps flowing
    ▼
webcam.c (CHDK module)
    │ polls dual-slot seqlock (100 × msleep(10))
    │ peeks AVCC headers from camera RAM to determine exact frame size
    │ memcpy only exact frame bytes (not full buffer)
    │ returns single H.264 frame per PTP response
    ▼
PTP USB transfer → bridge → FFmpeg decode → virtual webcam
```

## Current Performance

| Metric | Value | Evidence |
|--------|-------|----------|
| Frames produced | ~312 in 10s (~30 fps) | v32g bridge output (10s session) |
| Frames received | 301/312 (96.5% capture) | v32g: dual-slot seqlock + AVCC peek |
| Frames decoded | 301/301 (100%) | v32g: zero decode failures |
| Decoded FPS | 30.1 fps | v32g: AVCC peek reduces memcpy, frees CPU for pipeline |
| Total FPS (incl. drops) | 30.1 fps | v32g: matches camera output rate |
| IDR keyframes | 21 in 10s (~2/sec, GOP ~15) | v32g bridge output |
| SD card writes | 0 bytes | 0-byte MOV file, SD usage unchanged |
| Max decode streak | 301 frames (10s) | v32g: entire session, zero failures |
| Best 60s result | 1691/1691 (100%) in 60s, 28.2fps | v32d: dual-slot seqlock, no AVCC peek |

### 17. Original TakeSemaphore timeout (1000ms) is correct for webcam

**Evidence**: v31c reverted spy_take_sem_short from 50ms fake-success to passthrough with original 1000ms timeout. Result: 226/235 decoded (96.2%), 22.6fps, max streak 210 frames, full 10s session. Previous 50ms timeout caused intermittent recording death after ~2s.
**Mechanism**: JPCORE hardware encode completes in ~1-5ms. The 1000ms timeout never fires during normal operation. The 50ms timeout occasionally fired when JPCORE was slow (e.g. IDR frames), returning fake success while [SP,#0x38] was still non-zero. Even with error path bypass, the corrupted JPCORE state caused pipeline instability.
**Implication**: spy_take_sem_short is now a simple passthrough. The error path bypass (proven fact #16) remains as a safety net but should rarely fire with 1000ms timeout.

### 18. IDR frames ARE lost to single-slot seqlock overwrites

**Evidence**: v32 added IDR-only seqlock (hdr[4..6]) — still 25 IDRs (no improvement, wrong approach). v32b replaced with dual-slot alternating seqlock (producer alternates all frames between slot A and B) — **40 IDRs in 20s** (vs 25 with single-slot). 98.9% decode rate vs 67.9%.
**Cause**: Single-slot seqlock holds only 1 frame. At 30fps (33ms interval) with ~36ms PTP round-trip, 2 frames often arrive per poll. The older frame (which may be an IDR) is overwritten by the newer P-frame. The IDR-only seqlock (v32) didn't help because it preserved IDRs but still lost P-frames, breaking decode chains.
**Implication**: Dual-slot seqlock is the correct solution. Both IDR and P-frame preservation matter for H.264 decode chain integrity.

### 19. MovieFrameGetter returns chunk size (256KB), not encoded frame size

**Evidence**: v32b debug frames show FSIZ=0x40000 (262144) for every frame. The `size` parameter from sub_FF92FE8C ([SP,#0x30]) is the ring buffer chunk allocation size, not the actual H.264 encoded size (~35-46KB). Changing the consumer from `sz <= SPY_BUF_SIZE` (reject >64KB) to `sz = min(sz, SPY_BUF_SIZE)` (clamp to 64KB) fixed the regression. The AVCC parser after memcpy determines actual frame size from 4-byte BE length prefixes.
**Implication**: Always clamp frame size to SPY_BUF_SIZE, never reject. The AVCC parser is the authoritative source of frame boundaries.

### 20. No producer stalls > 50ms in 60-second recording

**Evidence**: v32d added `get_tick_count` (0x3223EC) stall detection to spy_ring_write — reports inter-call gaps > 50ms via debug frame. Result: 0 GAP events in 60 seconds, 100% decode rate (1691/1691), max streak 1691.
**Previous wrong approach**: Hardware timer 0xC0242014 — wrong address, returned ~4 billion values, fired on every frame, destroyed performance to 8.5%. Address 0x3223F0 — 4 bytes past entry point, corrupted stack, crashed camera immediately.
**Implication**: The clustered drops seen in v32c (98.2%) were transient timing variance, not systematic producer stalls. The recording pipeline runs smoothly at 30fps with no significant gaps.

### 21. msleep(10) is the optimal seqlock polling interval

**Evidence**: v32e tested msleep values 1, 5, 10, 12 over 60 seconds each:
- msleep(10): 100% decode, 1691 received, 28.2fps, max streak 1691
- msleep(12): 100% decode, 1637 received, 27.3fps (fewer frames captured)
- msleep(5): 94.3% decode, 1219 received, 19.1fps (starved producer — only 1280 frames produced)
- msleep(1): 98.5% decode, 1713 received, 28.1fps (25 decode failures from timing contention)
**Implication**: msleep(10) is the minimum sleep that gives movie_record_task enough CPU for full 30fps production. Lower values starve the producer or cause seqlock read contention. Higher values miss more frames. The ~3.5% capture loss (61 frames/min) is inherent to the USB PTP round-trip (~35ms) and cannot be improved by polling faster.

### 22. hdr[7..9] MUST NOT be written — causes hardware interference (DEFINITIVELY CONFIRMED)

**Evidence**: Multiple approaches tested in v32f session 2:
- Every-frame write to hdr[7..9] (triple-slot seqlock): dark display, IS motor clicking, garbage data
- Write-once hdr[7] (BSS address publish): same dark display, IS motor clicking
- BSS `static unsigned int slot_data[9]` + hdr[7] pointer: camera crashes at 0s
- **Triple-slot + AVCC peek** (minimal CPU: copies only 3-50KB per frame instead of 64KB): still dark display + IS motor clicking. This rules out CPU starvation as the cause — it IS the addresses themselves.

**Implication**: spy buffer offsets 7-9 at 0xFF000 overlap with something hardware-sensitive. Only hdr[1..6] (dual-slot) is safe for seqlock data. All triple-slot approaches have been exhausted and failed. The maximum number of seqlock slots is 2.

### 23. Yield msleep(10) after detecting new frame is mandatory

**Evidence**: Removing yield msleep between frame detection and memcpy read:
- Frame sizes drop from ~40KB to 2-9KB (corrupt/partial data)
- Decode rate drops from 87.6% to 48.8%
- FPS drops from 17.6 to 2.6
- USB crash at 8.3s

With yield restored: stable operation, correct frame sizes.

**Cause**: DryOS cooperative multitasking. Without msleep(), the PTP task monopolizes CPU during memcpy + AVCC parse, starving movie_record_task and all ISP/display/IS motor tasks. Both msleep calls in the poll loop are essential: one for waiting (no new data), one for yielding (before reading).

### 24. Consumer must peek AVCC headers before memcpy (minimum copy)

**Evidence**: Producer writes AVCC-parsed actual size to seqlock s[1], but on AVCC parse failure it falls back to raw chunk size (up to 256KB, clamped to 64KB by SPY_BUF_SIZE). Consumer should not trust s[1] blindly.

**Optimization**: Read AVCC length headers directly from camera RAM (src[0..3], ~20 bytes of header reads), compute exact frame size, then memcpy only that amount. Reduces worst-case copy from 64KB to actual frame size (typically 9-45KB). Less CPU time = more time for ISP/display/IS motor tasks.

**Implementation**: Peek AVCC length headers from `src` (camera RAM pointer) before any memcpy. Walk NAL units to find VCL NAL (type 1 or 5), compute exact frame size, then memcpy only that amount into frame_data_buf.

### 25. 128KB multi_frame_buf malloc causes dark screen + IS motor clicking

**Evidence**: All v32f variants with `multi_frame_buf` (128KB cascade malloc) exhibited dark display and IS motor clicking. v32g removed multi_frame_buf entirely (keeping only the 64KB frame_data_buf) — no dark screen, no clicking, 100% decode, 30.1fps.
- v32f with multi_frame_buf + dual-slot: dark screen, 76.3% decode, USB crash
- v32f with multi_frame_buf + triple-slot: dark screen, 92.4% decode
- v32g without multi_frame_buf: **no dark screen**, 100% decode, 30.1fps

**Cause**: DryOS heap exhaustion. 64KB (frame_data_buf) + 128KB (multi_frame_buf) = 192KB from DryOS heap. The camera's ISP, display controller, and IS motor controller share the same heap. 128KB extra allocation starves these subsystems.

**Implication**: Maximum safe malloc from webcam module is ~64KB. Multi-frame batching requires an alternative approach that doesn't allocate a large buffer (e.g., stack-based for small batches, or different packing strategy).

## What Needs to Happen Next

1. **Virtual webcam integration**: Connect the H.264 decode output to a DirectShow virtual webcam filter for use in video conferencing apps.
