# Debug Frame Protocol

Camera-to-bridge debug channel for passing structured diagnostic data without interfering with the H.264 video stream.

> **WARNING (v35e):** Debug frames MUST NOT be sent during live streaming. Each debug frame takes priority over an H.264 frame in the PTP response, causing the bridge to miss one video frame per debug frame sent. This produces systematic H.264 decoder artifacts (proven fact #30). The debug protocol is available for controlled diagnostic sessions only — never during normal webcam operation.

## Overview

Debug frames use a dedicated format (`WEBCAM_FMT_DEBUG = 3` / `FRAME_FMT_DEBUG = 3`) carried over the existing PTP `GetMJPEGFrame` command. The bridge recognizes format=3 and routes the payload to a debug printer instead of the video decoder. Debug frames are never counted as video frames and never reach the webcam output.

## Architecture

```
movie_rec.c (producer)          webcam.c (consumer/relay)         main.cpp (printer)
  spy_debug_reset()               capture_frame_h264()             print_debug_frame()
  spy_debug_add(tag, val)  --->   reads queue slot           --->  structured stderr output
  spy_debug_send()                sets format = DEBUG              appends to debug_frames.log
       |                               |
       v                               v
  Lock-free SPSC queue           PTP param4 = 3
  in shared memory               (WEBCAM_FMT_DEBUG)
```

## Debug Payload Format

Each debug frame contains a header and tagged key-value entries:

```
Header (12 bytes):
  [0-3]   magic "DBG!" (0x44 0x42 0x47 0x21)
  [4-7]   sequence number (uint32 LE, auto-incremented)
  [8-9]   entry_count (uint16 LE)
  [10-11] reserved (0)

Entries (8 bytes each):
  [0-3]  tag (4 ASCII chars, e.g. "RBas", "IdrP")
  [4-7]  value (uint32 LE)
```

Max entries per frame: 62 (12 + 62x8 = 508 bytes, fits in one 512-byte slot).

## Queue Layout (Shared Memory)

The debug queue lives in the spy buffer memory region at `0x000FF000`:

```
0xFF000 + 0x00  hdr[0]      magic (0x52455753 = active)
0xFF000 + 0x04  hdr[1]      frame data pointer (seqlock, written by movie_rec.c)
0xFF000 + 0x08  hdr[2]      frame data size (seqlock, written by movie_rec.c)
0xFF000 + 0x0C  hdr[3]      seqlock sequence counter (odd=writing, even=stable)
0xFF000 + 0x10  hdr[4..11]  reserved
0xFF000 + 0x30  hdr[12]     debug queue write_idx (0..3), written by movie_rec.c only
0xFF000 + 0x34  hdr[13]     debug queue read_idx  (0..3), written by webcam.c only

0xFF000 + 0x40  slot 0 (512 bytes): [0-1] payload_size (u16 LE), [2-3] reserved, [4..511] payload
0xFF000 + 0x240 slot 1 (512 bytes)
0xFF000 + 0x440 slot 2 (512 bytes)
0xFF000 + 0x640 slot 3 (512 bytes)
                                     end = 0xFF840 (within 4KB page)
```

- **Queue full**: `(write_idx + 1) % 4 == read_idx` (drop new frame)
- **Queue empty**: `write_idx == read_idx`
- **Capacity**: 3 pending debug frames (4 slots, 1 always empty per ring buffer convention)
- **Thread safety**: Lock-free SPSC — movie_rec.c only writes `write_idx`, webcam.c only writes `read_idx`. Safe on single-core ARM.
- **Memory barrier**: `spy_debug_send()` uses ARM Drain Write Buffer (`mcr p15,0,r0,c7,c10,4`) before advancing `write_idx` to ensure slot data is committed before the index update becomes visible. Required because `volatile` does NOT prevent hardware write buffer reordering on ARM926EJ-S (ARMv5).

## Camera-Side API (movie_rec.c)

Three functions for building and sending debug frames:

```c
// Start building a new debug frame (resets buffer, sets magic + sequence number)
spy_debug_reset();

// Append a tagged uint32 entry (4-char tag + value)
spy_debug_add('R','B','a','s', rb_base);
spy_debug_add('I','d','r','P', idr_ptr_val);
// ... up to 62 entries per frame

// Enqueue the frame into the SPSC ring buffer
spy_debug_send();
```

### Usage Example

```c
spy_debug_reset();
spy_debug_add('R','B','a','s', rb_base);
spy_debug_add('I','d','r','P', idr_ptr_val);
spy_debug_add('I','d','r','S', idr_size);
spy_debug_add('M','5','P','t', msg5_idr_ptr);
spy_debug_add('M','5','S','z', msg5_idr_size);
spy_debug_add('D','a','t','P', data_at_ptr);
spy_debug_add('M','5','C','t', msg5_count);
spy_debug_send();
```

### Tag Naming Convention

Tags are 4 ASCII characters. Use a descriptive abbreviation:
- First 2-3 chars: category (e.g. "RB" = ring buffer, "Idr" = IDR, "M5" = msg 5)
- Last 1-2 chars: specific field (e.g. "as" = base, "Pt" = pointer, "Sz" = size, "Ct" = count)

## Bridge-Side Output

### stderr (real-time)

```
=== DEBUG FRAME #0 (7 entries, 68 bytes) ===
  RBas = 0x41BE0600  (1103234560)
  IdrP = 0x41C00000  (1103101952)
  IdrS = 0x00002A40  (10816)
  M5Pt = 0x41C00000  (1103101952)
  M5Sz = 0x00002A40  (10816)
  DatP = 0x00000065  (101)
  M5Ct = 0x00000001  (1)
=== END DEBUG ===
```

### Log file (`debug_frames.log`)

Created alongside the bridge exe. Appends all debug frames for post-mortem analysis.

## Files

| File | Role |
|------|------|
| `chdk/platform/ixus870_sd880/sub/101a/movie_rec.c` | Producer: `spy_debug_reset/add/send` API, queue write |
| `chdk/modules/webcam.c` | Consumer: reads queue slots, sets `WEBCAM_FMT_DEBUG` |
| `chdk/modules/webcam.h` | Defines `WEBCAM_FMT_DEBUG = 3` |
| `bridge/src/ptp/ptp_client.h` | Defines `FRAME_FMT_DEBUG = 3` |
| `bridge/src/main.cpp` | `print_debug_frame()` — stderr output + log file |
