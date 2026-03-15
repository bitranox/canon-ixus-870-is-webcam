#include "conf.h"

void change_video_tables(__attribute__ ((unused))int a, __attribute__ ((unused))int b) {}


void  set_quality(int *x){ // -17 highest; +12 lowest
 if (conf.video_mode) *x=12-((conf.video_quality-1)*(12+17)/(99-1));
}


// ============================================================
// Debug frame queue (lock-free SPSC ring buffer)
// Producer: movie_rec.c (spy_debug_* API)
// Consumer: webcam.c (capture_frame_h264 reads slots)
// ============================================================
#define DBG_SLOT_SIZE   512
#define DBG_QUEUE_DEPTH 4
#define DBG_QUEUE_BASE  0x000FF040

static unsigned char dbg_build_buf[DBG_SLOT_SIZE];
static unsigned int  dbg_build_len = 0;
static unsigned int  dbg_seq = 0;

static void spy_debug_reset(void)
{
    dbg_build_buf[0]='D'; dbg_build_buf[1]='B';
    dbg_build_buf[2]='G'; dbg_build_buf[3]='!';
    *(unsigned int *)(dbg_build_buf + 4) = dbg_seq++;
    *(unsigned short *)(dbg_build_buf + 8) = 0;
    dbg_build_buf[10] = 0; dbg_build_buf[11] = 0;
    dbg_build_len = 12;
}

static void spy_debug_add(char t0, char t1, char t2, char t3, unsigned int value)
{
    if (dbg_build_len + 8 > DBG_SLOT_SIZE - 4) return;
    dbg_build_buf[dbg_build_len]   = t0;
    dbg_build_buf[dbg_build_len+1] = t1;
    dbg_build_buf[dbg_build_len+2] = t2;
    dbg_build_buf[dbg_build_len+3] = t3;
    *(unsigned int *)(dbg_build_buf + dbg_build_len + 4) = value;
    dbg_build_len += 8;
    (*(unsigned short *)(dbg_build_buf + 8))++;
}

static void spy_debug_send(void)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;
    unsigned int wr = hdr[12];
    unsigned int rd = hdr[13];
    unsigned int next_wr = (wr + 1) % DBG_QUEUE_DEPTH;
    unsigned int i;
    volatile unsigned char *slot;

    if (next_wr == rd) return;

    slot = (volatile unsigned char *)(DBG_QUEUE_BASE + wr * DBG_SLOT_SIZE);
    *(volatile unsigned short *)slot = (unsigned short)dbg_build_len;
    slot[2] = 0; slot[3] = 0;
    for (i = 0; i < dbg_build_len; i++)
        slot[4 + i] = dbg_build_buf[i];

    // ARM drain write buffer before advancing index
    asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0));
    hdr[12] = next_wr;
}

// SD write prevention: clear +0x80 (is_open flag) in the ring buffer struct.
// Decompiled task_MovWrite (0xFF92F1EC) case 2 handler:
//   if (error == 0 && *(base + 0x80) == 1 && size != 0)
//       FUN_ff85235c(fd, buf, size);  // actual file write
//   else
//       update write position (+0x18) only;  // no write, pipeline keeps flowing
//
// By clearing +0x80, the write condition fails and task_MovWrite just updates
// the consumed pointer.  The entire pipeline (sub_FF9300B4, sub_FF8EDBE0,
// message posting, queue processing) runs unmodified.  Only the deepest
// point — the actual file I/O call — is prevented.

// Invalidate CPU data cache lines for a memory range.
// JPCORE DMA writes H.264 data to physical memory bypassing the CPU cache.
// Without invalidation, CPU reads return stale cached data.
// After invalidation, reads fetch fresh data from memory.
// ARM946E-S cache line size = 32 bytes.
static void __attribute__((used,noinline)) spy_cache_invalidate(unsigned char *ptr, unsigned int size)
{
    unsigned int addr = (unsigned int)ptr & ~31u;
    unsigned int end  = (unsigned int)ptr + size;
    while (addr < end) {
        asm volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(addr));
        addr += 32;
    }
}

// Suppress MOV file creation when webcam is active.
// Called from movie_record_task case 2 (after sub_FF85DE1C finishes recording init).
//
// sub_FF85DE1C → XREF_FUN_ff92f734 sets ring_buf+0x50 (filename) and posts
// msg 1 to task_MovWrite's queue.  We can't prevent that (it's in ROM).
// But task_MovWrite has a built-in cancel mechanism: when ring_buf+0x88 == 1,
// it enters "drain mode" — consumes queued messages without processing them.
// FUN_ff9309e4 (case 1 = file creation) never runs, so no MOV file is created.
//
// Timing: spy_suppress_mov runs in movie_record_task (after sub_FF85DE1C),
// before movie_record_task yields at ReceiveMessageQueue.  spy_ring_write
// clears +0x88 back to 0 on the first frame, so subsequent messages
// (case 2 = frame write) are processed normally (but writes are still
// skipped because +0x80 = 0).
//
// Also sets +0x48 = -1 (invalid fd) as a safety measure — prevents case 7
// (file close) from closing a stale fd if one exists from a previous recording.
static void __attribute__((used,noinline)) spy_suppress_mov(void)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;
    if (hdr[0] == 0x52455753) {
        *(volatile unsigned int *)0x89F0 = 1;           // ring_buf+0x88: drain mode
        *(volatile unsigned int *)0x89B0 = 0xFFFFFFFF;  // ring_buf+0x48: fd = -1
    }
}

// Check if webcam is active. Returns 1 to skip error path, 0 for normal error handling.
// Called from error paths in sub_FF85D98C_my to prevent STATE=1 (permanent pipeline death).
static int __attribute__((used,noinline)) spy_skip_error_path(void)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;
    return (hdr[0] == 0x52455753) ? 1 : 0;
}

// TakeSemaphore passthrough — calls firmware sub_FF8274B4 via function pointer.
// Needed because sub_FF8274B4 has no linker stub, so BL from inline asm won't resolve.
// JPCORE hardware encode completes in ~1-5ms; the original 1000ms timeout is correct.
static int __attribute__((used,noinline)) spy_take_sem_short(int sem, int timeout)
{
    int (*real_take_sem)(int, int) = (int (*)(int, int))0xFF8274B4;
    return real_take_sem(sem, timeout);
}

// Deliver H.264 frame to webcam module via seqlock protocol.
// Called from sub_FF85D98C_my after sub_FF92FE8C returns each encoded frame.
// Invalidates CPU cache for the frame data (JPCORE DMA bypasses cache).
//
// Data delivery uses seqlock protocol in shared memory (proven working at
// 22fps in v26g).  DryOS kernel signaling (semaphores, message queues) is
// NOT viable — causes context switches that starve the recording pipeline.
//
// Shared memory layout at 0x000FF000:
//   [0]  magic      = 0x52455753 when active (set by webcam.c)
//   [1]  slot_a_ptr = frame data pointer (dual-slot seqlock, slot A)
//   [2]  slot_a_sz  = frame data size (slot A)
//   [3]  slot_a_seq = sequence counter (odd=writing, even=stable, slot A)
//   [4]  slot_b_ptr = frame data pointer (dual-slot seqlock, slot B)
//   [5]  slot_b_sz  = frame data size (slot B)
//   [6]  slot_b_seq = sequence counter (odd=writing, even=stable, slot B)
//   [12] dbg_wr     = debug queue write index
//   [13] dbg_rd     = debug queue read index
//
// Producer alternates: frame 1→A, frame 2→B, frame 3→A, ...
// This doubles the buffer so two frames arriving during one bridge
// PTP round-trip go to different slots — neither is lost.

static void __attribute__((used,noinline)) spy_ring_write(unsigned char *ptr, unsigned int size)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;

    if (hdr[0] == 0x52455753) {
        spy_cache_invalidate(ptr, size);

        // Report NAL type to bridge via debug frame (every 30th frame)
        {
            static unsigned int nal_frame_count = 0;
            nal_frame_count++;
            if ((nal_frame_count % 30) == 1 && size >= 5) {
                spy_debug_reset();
                spy_debug_add('N','A','L','T', ptr[4]);        // NAL byte (type in low 5 bits)
                spy_debug_add('F','S','I','Z', size);          // frame size
                spy_debug_add('F','N','U','M', nal_frame_count); // frame number
                spy_debug_send();
            }
        }

        // Prevent SD card writes: clear task_MovWrite's is_open flag.
        // 0x89E8 = ring buffer struct (0x8968) + 0x80.
        // task_MovWrite checks this before every file write; when 0,
        // it skips the write but still updates the consumed pointer.
        *(volatile unsigned int *)0x89E8 = 0;

        // Clear drain mode so task_MovWrite processes frame messages normally.
        // spy_suppress_mov set +0x88=1 during init to drain the file-creation
        // message.  Now that frames are flowing, clear it so case 2 (write)
        // messages reach the consumed-pointer update path (writes still skip
        // because +0x80=0 above).
        *(volatile unsigned int *)0x89F0 = 0;

        // Stall detection: report inter-call gaps > 50ms via debug frame.
        // Helps identify what's blocking movie_record_task during clustered drops.
        {
            static unsigned int last_tick = 0;
            long (*fw_get_tick)(void) = (long (*)(void))0x3223EC;
            unsigned int now = (unsigned int)fw_get_tick();
            if (last_tick != 0) {
                unsigned int delta = now - last_tick;
                // get_tick_count returns milliseconds. Report gaps > 50ms.
                if (delta > 50) {
                    spy_debug_reset();
                    spy_debug_add('G','A','P','!', delta);     // gap in milliseconds
                    spy_debug_add('N','O','W','T', now);       // current tick (ms)
                    spy_debug_add('L','S','T','T', last_tick); // last tick (ms)
                    spy_debug_send();
                }
            }
            last_tick = now;
        }

        // Determine actual H.264 frame size from AVCC length prefix.
        // MovieFrameGetter returns the 256KB chunk size, not encoded size.
        // Parse AVCC: [4-byte BE length][NAL data], possibly 2 NALs (SEI+slice).
        {
            unsigned int actual = size;
            if (size >= 5) {
                unsigned int n = ((unsigned int)ptr[0] << 24)
                               | ((unsigned int)ptr[1] << 16)
                               | ((unsigned int)ptr[2] << 8) | ptr[3];
                if (n > 0 && n < 120000) {
                    actual = 4 + n;
                    // Check for second NAL (e.g. SEI + slice)
                    if (actual + 5 <= size) {
                        unsigned int n2 = ((unsigned int)ptr[actual] << 24)
                                        | ((unsigned int)ptr[actual+1] << 16)
                                        | ((unsigned int)ptr[actual+2] << 8)
                                        | ptr[actual+3];
                        if (n2 > 0 && n2 < 120000)
                            actual += 4 + n2;
                    }
                }
            }

            // Dual-slot seqlock: alternate between slot A (hdr[1..3]) and
            // slot B (hdr[4..6]). Store actual encoded size, not chunk size.
            {
                static int slot = 0;
                if (slot == 0) {
                    hdr[3]++;
                    hdr[1] = (unsigned int)ptr;
                    hdr[2] = actual;
                    hdr[3]++;
                } else {
                    hdr[6]++;
                    hdr[4] = (unsigned int)ptr;
                    hdr[5] = actual;
                    hdr[6]++;
                }
                slot ^= 1;
            }
        }
    }
}

void __attribute__((naked,noinline)) movie_record_task(){
 // from 0xFF85E03C (found via call to taskcreate_AviWrite)
 asm volatile(

                 "STMFD   SP!, {R2-R8,LR}\n"
                 "LDR     R7, =0x2710\n"
                 "LDR     R4, =0x51A8\n"
                 "MOV     R6, #0\n"
                 "MOV     R5, #1\n"
 "loc_FF85E050:\n"
                 "LDR     R0, [R4,#0x1C]\n"
                 "MOV     R2, #0\n"
                 "ADD     R1, SP, #0xC\n"
                 "BL      sub_FF827098\n"
                 "LDR     R0, [R4,#0x24]\n"
                 "CMP     R0, #0\n"
                 "BNE     loc_FF85E120\n"
                 "LDR     R0, [SP,#0xC]\n"
                 "LDR     R1, [R0]\n"
                 "SUB     R1, R1, #2\n"
                 "CMP     R1, #9\n"
                 "ADDLS   PC, PC, R1,LSL#2\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E084:\n"
                 "B       loc_FF85E0D4\n"
 "loc_FF85E088:\n"
                 "B       loc_FF85E0F4\n"
 "loc_FF85E08C:\n"
                 "B       loc_FF85E104\n"
 "loc_FF85E090:\n"
                 "B       loc_FF85E10C\n"
 "loc_FF85E094:\n"
                 "B       loc_FF85E0DC\n"
 "loc_FF85E098:\n"
                 "B       loc_FF85E114\n"
 "loc_FF85E09C:\n"
                 "B       loc_FF85E0E4\n"
 "loc_FF85E0A0:\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E0A4:\n"
                 "B       loc_FF85E11C\n"
 "loc_FF85E0A8:\n"
                 "B       loc_FF85E0AC\n"
 "loc_FF85E0AC:\n"
                 "STR     R6, [R4,#0x38]\n"
                 "LDR     R0, =0xFF85DD14\n"
                 "LDR     R2, =0xFF85D28C\n"
                 "LDR     R1, =0x1AB94\n"
                 "STR     R0, [R4,#0xA0]\n"
                 "LDR     R0, =0xFF85D370\n"
                 "STR     R6, [R4,#0x28]\n"
                 "BL      sub_FF8C3BFC\n"
                 "STR     R5, [R4,#0x3C]\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E0D4:\n"
                 "BL      unlock_optical_zoom\n" // + (used ixus980)
                 "BL      sub_FF85DE1C\n"
                 "BL      spy_suppress_mov\n"    // + drain task_MovWrite msg 1 (no MOV file)
                 "B       loc_FF85E120\n"
 "loc_FF85E0DC:\n"
                 "BL      sub_FF85D98C_my\n"  //--------------->
                 "B       loc_FF85E120\n"
 "loc_FF85E0E4:\n"
                 "LDR     R1, [R0,#0x10]\n"
                 "LDR     R0, [R0,#4]\n"
                 "BL      sub_FF92FDF0\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E0F4:\n"
                 "LDR     R0, [R4,#0x3C]\n"
                 "CMP     R0, #5\n"
                 "STRNE   R5, [R4,#0x2C]\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E104:\n"
                 "BL      sub_FF85D6CC\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E10C:\n"
                 "BL      sub_FF85D3BC\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E114:\n"
                 "BL      sub_FF85D218\n"
                 "B       loc_FF85E120\n"
 "loc_FF85E11C:\n"
                 "BL      sub_FF85E28C\n"
 "loc_FF85E120:\n"
                 "LDR     R1, [SP,#0xC]\n"
                 "MOV     R3, #0x430\n"
                 "STR     R6, [R1]\n"
                 "STR     R3, [SP]\n"
                 "LDR     R0, [R4,#0x20]\n"
                 "LDR     R3, =0xFF85D018\n"
                 "MOV     R2, R7\n"
                 "BL      sub_FF8279EC\n"
                 "B       loc_FF85E050\n"
 );
}


void __attribute__((naked,noinline)) sub_FF85D98C_my(){
 asm volatile(

                 "STMFD   SP!, {R4-R10,LR}\n"
                 "SUB     SP, SP, #0x40\n"
                 "MOV     R7, #0\n"
                 "LDR     R6, =0x51A8\n"
                 "MOV     R4, R0\n"
                 "STR     R7, [SP,#0x30]\n"
                 "STR     R7, [SP,#0x28]\n"
                 "LDR     R0, [R6,#0x3C]\n"
                 "MOV     R9, #4\n"
                 "CMP     R0, #3\n"
                 "STREQ   R9, [R6,#0x3C]\n"
                 "LDR     R0, [R6,#0xA0]\n"
                 "MOV     R8, #0\n"
                 "BLX     R0\n"
                 // After callback: also promote STATE 3→4 so the first
                 // frame (IDR keyframe) is not skipped.  The callback at
                 // +0xA0 promotes STATE 2→3 on the first msg 6, but the
                 // original firmware only checks for STATE==4 here, causing
                 // the IDR to be dropped every time.
                 "LDR     R0, [R6,#0x3C]\n"
                 "CMP     R0, #3\n"
                 "STREQ   R9, [R6,#0x3C]\n"  // STATE 3→4 (R9=4)
                 "CMP     R0, #4\n"
                 "CMPNE   R0, #3\n"          // also accept 3 (just promoted)
                 "BNE     loc_FF85DB10\n"
                 "LDRH    R0, [R6,#2]\n"
                 "MOV     R5, #1\n"
                 "CMP     R0, #1\n"
                 "BNE     loc_FF85DA08\n"
                 "LDRH    R1, [R6,#4]\n"
                 "LDR     R0, [R6,#0x48]\n"
                 "MUL     R0, R1, R0\n"
                 "MOV     R1, #0x3E8\n"
                 "BL      sub_FFAD3D98\n"
                 "MOV     R1, R0\n"
                 "LDR     R0, [R6,#0x50]\n"
                 "BL      sub_FFAD3D98\n"
                 "CMP     R1, #0\n"
                 "BNE     loc_FF85DA24\n"
 "loc_FF85DA08:\n"
                 "ADD     R3, SP, #0x28\n"
                 "ADD     R2, SP, #0x2C\n"
                 "ADD     R1, SP, #0x30\n"
                 "ADD     R0, SP, #0x34\n"
                 "BL      sub_FF92FE8C\n"
                 "MOVS    R8, R0\n"
                 "BNE     loc_FF85DA40\n"

                // Deliver every frame to webcam module.
                // spy_ring_write invalidates CPU cache and stores ptr/size via seqlock.
                "LDR     R0, [SP, #0x34]\n"    // R0 = frame ptr from sub_FF92FE8C
                "LDR     R1, [SP, #0x30]\n"    // R1 = frame size
                "BL      spy_ring_write\n"

 "loc_FF85DA24:\n"
                 "LDR     R0, [R6,#0x2C]\n"
                 "CMP     R0, #1\n"
                 "BNE     loc_FF85DB24\n"
                 "LDR     R0, [R6,#0x50]\n"
                 "LDR     R1, [R6,#0x40]\n"
                 "CMP     R0, R1\n"
                 "BCC     loc_FF85DB24\n"
 "loc_FF85DA40:\n"
                 "CMP     R8, #0x80000001\n"
                 "STREQ   R9, [R6,#0x54]\n"
                 "BEQ     loc_FF85DA78\n"
                 "CMP     R8, #0x80000003\n"
                 "STREQ   R5, [R6,#0x54]\n"
                 "BEQ     loc_FF85DA78\n"
                 "CMP     R8, #0x80000005\n"
                 "MOVEQ   R0, #2\n"
                 "BEQ     loc_FF85DA74\n"
                 "CMP     R8, #0x80000007\n"
                 "STRNE   R7, [R6,#0x54]\n"
                 "BNE     loc_FF85DA78\n"
                 "MOV     R0, #3\n"
 "loc_FF85DA74:\n"
                 "STR     R0, [R6,#0x54]\n"
 "loc_FF85DA78:\n"
                 "LDR     R0, =0x1ABC8\n"
                 "LDR     R0, [R0,#8]\n"
                 "CMP     R0, #0\n"
                 "BEQ     loc_FF85DA90\n"
                 "BL      sub_FF847B84\n"
                 "B       loc_FF85DA94\n"
 "loc_FF85DA90:\n"
                 "BL      sub_FF85D218\n"
 "loc_FF85DA94:\n"
                 "LDR     R0, [R4,#0x14]\n"
                 "LDR     R1, [R4,#0x18]\n"
                 "ADD     R3, SP, #0x38\n"
                 "MVN     R2, #1\n"
                 "ADD     R7, SP, #0x18\n"
                 "STMIA   R7, {R0-R3}\n"
                 "MOV     R0, #0\n"
                 "ADD     R1, SP, #0x3C\n"
                 "ADD     R7, SP, #0x8\n"
                 "LDRD    R2, [R6,#0x68]\n"
                 "STMIA   R7, {R0-R3}\n"
                 "MOV     R3, #0\n"
                 "MOV     R2, #0x40\n"
                 "STRD    R2, [SP]\n"
                 "LDMIB   R4, {R0,R1}\n"
                 "LDR     R3, =0x1ABE0\n"
                 "MOV     R2, R10\n"
                 "BL      sub_FF8EDBE0\n"
                 "LDR     R0, [R6,#0x14]\n"
                 "MOV     R1, #0x3E8\n"
                 "BL      spy_take_sem_short\n"
                 "CMP     R0, #9\n"
                 "BEQ     loc_FF85DBA4\n"
                 "LDR     R0, [SP,#0x38]\n"
                 "CMP     R0, #0\n"
                 "BNE     loc_FF85DBC0\n"
                 "MOV     R0, #1\n"
                 "BL      sub_FF8EDC88\n"
                 "BL      sub_FF8EDCC4\n"
                 "MOV     R0, #5\n"
                 "STR     R0, [R6,#0x3C]\n"
 "loc_FF85DB10:\n"
                 "ADD     SP, SP, #0x40\n"
                 "LDMFD   SP!, {R4-R10,PC}\n"
 "loc_FF85DB18:\n"
                 "BL      sub_FF879164\n"
                 "MOV     R0, #1\n"
                 "B       loc_FF85DC5C\n"
                 // Error bypass for webcam: skip drain + STATE=1, just cleanup + exit
 "spy_err_bypass_1:\n"
                 "MOV     R0, #1\n"
                 "BL      sub_FF8EDC88\n"
                 "B       loc_FF85DB10\n"
 "spy_err_bypass_2:\n"
                 "MOV     R0, #0\n"
                 "BL      sub_FF8EDC88\n"
                 "B       loc_FF85DB10\n"
 "loc_FF85DB24:\n"
                 "LDR     R12, [SP,#0x30]\n"
                 "CMP     R12, #0\n"
                 "BEQ     loc_FF85DCBC\n"
                 "STR     R5, [R6,#0x30]\n"
                 "LDR     R0, [R6,#0x50]\n"
                 "LDR     R8, [R4,#0xC]\n"
                 "CMP     R0, #0\n"
                 "LDRNE   LR, [SP,#0x34]\n"
                 "BNE     loc_FF85DBEC\n"
                 "LDR     R0, [R4,#0x14]\n"
                 "LDR     R1, [R4,#0x18]\n"
                 "ADD     R3, SP, #0x38\n"
                 "MVN     R2, #0\n"
                 "ADD     R9, SP, #0x18\n"
                 "STMIA   R9, {R0-R3}\n"
                 "LDRD    R2, [R6,#0x68]\n"
                 "LDR     R0, [SP,#0x28]\n"
                 "ADD     R1, SP, #0x3C\n"
                 "ADD     R9, SP, #0x8\n"
                 "STMIA   R9, {R0-R3}\n"
                 "LDR     R3, [SP,#0x2C]\n"
                 "STR     R12, [SP]\n"
                 "STR     R3, [SP,#4]\n"
                 "LDMIB   R4, {R0,R1}\n"
                 "LDR     R3, [SP,#0x34]\n"
                 "MOV     R2, R8\n"
                 "BL      sub_FF8EDBE0\n"
                 "LDR     R0, [R6,#0x14]\n"
                 "MOV     R1, #0x3E8\n"
                 "BL      spy_take_sem_short\n"
                 "CMP     R0, #9\n"
                 "BNE     loc_FF85DBB4\n"
 "loc_FF85DBA4:\n"
                 // Webcam active? Skip drain + STATE=1 (permanent pipeline death)
                 "BL      spy_skip_error_path\n"
                 "CMP     R0, #0\n"
                 "BNE     spy_err_bypass_1\n"
                 "BL      sub_FF930358\n"
                 "MOV     R0, #0x90000\n"
                 "STR     R5, [R6,#0x3C]\n"
                 "B       loc_FF85DB18\n"
 "loc_FF85DBB4:\n"
                 "LDR     R0, [SP,#0x38]\n"
                 "CMP     R0, #0\n"
                 "BEQ     loc_FF85DBD0\n"
 "loc_FF85DBC0:\n"
                 "BL      spy_skip_error_path\n"
                 "CMP     R0, #0\n"
                 "BNE     spy_err_bypass_1\n"
                 "BL      sub_FF930358\n"
                 "MOV     R0, #0xA0000\n"
                 "STR     R5, [R6,#0x3C]\n"
                 "B       loc_FF85DB18\n"
 "loc_FF85DBD0:\n"
                 "MOV     R0, #1\n"
                 "BL      sub_FF8EDC88\n"
                 "LDR     R0, [SP,#0x3C]\n"
                 "LDR     R1, [SP,#0x34]\n"
                 "ADD     LR, R1, R0\n"
                 "LDR     R1, [SP,#0x30]\n"
                 "SUB     R12, R1, R0\n"
 "loc_FF85DBEC:\n"
                 "LDR     R0, [R4,#0x14]\n"
                 "LDR     R2, [R6,#0x4C]\n"
                 "LDR     R1, [R4,#0x18]\n"
                 "ADD     R3, SP, #0x38\n"
                 "ADD     R9, SP, #0x18\n"
                 "STMIA   R9, {R0-R3}\n"
                 "LDRD    R2, [R6,#0x68]\n"
                 "LDR     R0, [SP,#0x28]\n"
                 "ADD     R1, SP, #0x3C\n"
                 "ADD     R9, SP, #0x8\n"
                 "STMIA   R9, {R0-R3}\n"
                 "LDR     R3, [SP,#0x2C]\n"
                 "STR     R12, [SP]\n"
                 "STR     R3, [SP,#4]\n"
                 "LDMIB   R4, {R0,R1}\n"
                 "MOV     R3, LR\n"
                 "MOV     R2, R8\n"
                 "BL      sub_FF8EDBE0\n"
                 "LDR     R0, [R6,#0x14]\n"
                 "MOV     R1, #0x3E8\n"
                 "BL      spy_take_sem_short\n"
                 "CMP     R0, #9\n"
                 "BNE     loc_FF85DC64\n"
                 "BL      spy_skip_error_path\n"
                 "CMP     R0, #0\n"
                 "BNE     spy_err_bypass_2\n"
                 "BL      sub_FF930358\n"
                 "MOV     R0, #0x90000\n"
                 "STR     R5, [R6,#0x3C]\n"
                 "BL      sub_FF879164\n"
                 "MOV     R0, #0\n"
 "loc_FF85DC5C:\n"
                 "BL      sub_FF8EDC88\n"
                 "B       loc_FF85DB10\n"
 "loc_FF85DC64:\n"
                 "LDR     R0, [SP,#0x38]\n"
                 "CMP     R0, #0\n"
                 "BEQ     loc_FF85DC84\n"
                 "BL      spy_skip_error_path\n"
                 "CMP     R0, #0\n"
                 "BNE     spy_err_bypass_2\n"
                 "BL      sub_FF930358\n"
                 "MOV     R0, #0xA0000\n"
                 "STR     R5, [R6,#0x3C]\n"
                 "BL      sub_FF879164\n"
                 "B       loc_FF85DB10\n"
 "loc_FF85DC84:\n"
                 "MOV     R0, #0\n"
                 "BL      sub_FF8EDC88\n"
                 "LDR     R0, [SP,#0x34]\n"
                 "LDR     R1, [SP,#0x3C]\n"
                 "BL      sub_FF9300B4\n"
                 "LDR     R0, [R6,#0x4C]\n"
                 "LDR     R3, =0x5214\n"    // ->----
                 "ADD     R1, R0, #1\n"     //       |
                 "STR     R1, [R6,#0x4C]\n" //       |
                 "STR     R3, [SP]\n"       //       |
                 "LDR     R0, [SP,#0x3C]\n" //       |
                 "SUB     R3, R3, #4\n"     // ->----|
                 "MOV     R2, #0xF\n"       //       |
                 "BL      sub_FF92E3B0\n"   //       |
                 "LDR     R0, =0x5214-4\n"  // -<----     // +
                 "BL      set_quality\n"                  // +
 "loc_FF85DCBC:\n"
                 "LDR     R0, [R6,#0x50]\n"
                 "ADD     R0, R0, #1\n"
                 "STR     R0, [R6,#0x50]\n"
                 "LDR     R1, [R6,#0x78]\n"
                 "MUL     R0, R1, R0\n"
                 "LDR     R1, [R6,#0x74]\n"
                 "BL      sub_FFAD3D98\n"
                 "MOV     R4, R0\n"
                 "BL      sub_FF930390\n"
                 "LDR     R1, [R6,#0x70]\n"
                 "CMP     R1, R4\n"
                 "BNE     loc_FF85DCF8\n"
                 "LDR     R0, [R6,#0x34]\n"
                 "CMP     R0, #1\n"
                 "BNE     loc_FF85DD0C\n"
 "loc_FF85DCF8:\n"
                 "LDR     R1, [R6,#0x84]\n"
                 "MOV     R0, R4\n"
                 "BLX     R1\n"
                 "STR     R4, [R6,#0x70]\n"
                 "STR     R7, [R6,#0x34]\n"
 "loc_FF85DD0C:\n"
                 "STR     R7, [R6,#0x30]\n"
                 "B       loc_FF85DB10\n"
 );
}
