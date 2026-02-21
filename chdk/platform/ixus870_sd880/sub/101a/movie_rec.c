#include "conf.h"

// Firmware stubs used by spy_ring_write
extern void _GiveSemaphore(int sem);

void change_video_tables(__attribute__ ((unused))int a, __attribute__ ((unused))int b) {}


void  set_quality(int *x){ // -17 highest; +12 lowest
 if (conf.video_mode) *x=12-((conf.video_quality-1)*(12+17)/(99-1));
}


// Store H.264 frame pointer and signal semaphore (Option 2: pointer pass-through).
// Called from sub_FF85D98C_my after sub_FF92FE8C returns encoded frame.
// Does NOT copy data — just stores the ring buffer pointer for the webcam module
// to read directly via its own memcpy.
//
// Store H.264 frame pointer and signal semaphore (Option 2: pointer pass-through).
// Called from sub_FF85D98C_my after sub_FF92FE8C returns encoded frame.
// Does NOT copy data — just stores the ring buffer pointer for the webcam module
// to read directly via its own memcpy.
//
// Simple overwrite protocol: always writes the latest frame.
// The consumer reads whenever it wakes on the semaphore.
//
// Shared memory protocol at 0x000FF000 (initialized by webcam.c):
//   [0] magic    = 0x52455753 when active (set by webcam.c)
//   [1] src_ptr  = ring buffer pointer (written here, read by webcam.c)
//   [2] size     = frame data size (written here)
//   [3] count    = frame counter (written LAST, here)
//   [5] sem      = semaphore handle (set by webcam.c)
static void __attribute__((used,noinline)) spy_ring_write(unsigned char *ptr, unsigned int size)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;
    unsigned int sem_handle;

    if (hdr[0] != 0x52455753) return;  // Not initialized by webcam.c

    hdr[1] = (unsigned int)ptr;        // Source pointer (ring buffer address)
    hdr[2] = size;                     // Frame data size
    hdr[3]++;                          // Frame counter (incremented LAST)

    sem_handle = hdr[5];
    if (sem_handle != 0) {
        _GiveSemaphore(sem_handle);
    }
}

static int idr_sent = 0;   // msg 6 call counter (0=not yet, 1+=sent)
static int msg5_done = 0;  // Set by spy_msg5_debug when msg 5 completes

// Msg 5 debug capture — stores ring buffer values right after IDR encoding
static unsigned int msg5_rb_base = 0;
static unsigned int msg5_idr_ptr = 0;
static unsigned int msg5_idr_size = 0;
static unsigned int msg5_count = 0;

// ============================================================
// Debug frame queue (lock-free SPSC ring buffer)
// Producer: movie_rec.c (spy_idr_capture via spy_debug_* API)
// Consumer: webcam.c (capture_frame_h264 reads slots)
// ============================================================
#define DBG_SLOT_SIZE   512
#define DBG_QUEUE_DEPTH 4
#define DBG_QUEUE_BASE  0x000FF040

static unsigned char dbg_build_buf[DBG_SLOT_SIZE];
static unsigned int  dbg_build_len = 0;
static unsigned int  dbg_seq = 0;

// Start building a new debug frame
static void spy_debug_reset(void)
{
    dbg_build_buf[0]='D'; dbg_build_buf[1]='B';
    dbg_build_buf[2]='G'; dbg_build_buf[3]='!';
    *(unsigned int *)(dbg_build_buf + 4) = dbg_seq++;
    *(unsigned short *)(dbg_build_buf + 8) = 0;
    dbg_build_buf[10] = 0; dbg_build_buf[11] = 0;
    dbg_build_len = 12;
}

// Append a tagged uint32 entry to the current debug frame
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

// Enqueue the debug frame — copy to next queue slot, advance write_idx
static void spy_debug_send(void)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;
    unsigned int wr = hdr[8];
    unsigned int rd = hdr[9];
    unsigned int next_wr = (wr + 1) % DBG_QUEUE_DEPTH;
    unsigned int i;
    volatile unsigned char *slot;

    if (next_wr == rd) return;  // Queue full — drop frame

    slot = (volatile unsigned char *)(DBG_QUEUE_BASE + wr * DBG_SLOT_SIZE);
    *(volatile unsigned short *)slot = (unsigned short)dbg_build_len;
    slot[2] = 0; slot[3] = 0;
    for (i = 0; i < dbg_build_len; i++)
        slot[4 + i] = dbg_build_buf[i];

    hdr[8] = next_wr;  // Advance write index LAST (memory barrier via volatile)
}

static void __attribute__((used,noinline)) spy_msg5_debug(void)
{
    unsigned int rb_base = *(volatile unsigned int *)0xFF93050C;
    msg5_count++;
    msg5_rb_base = rb_base;
    if (rb_base != 0) {
        msg5_idr_ptr = *(volatile unsigned int *)(rb_base + 0xD8);
        msg5_idr_size = *(volatile unsigned int *)(rb_base + 0xDC);
    }
    msg5_done = 1;  // Signal that IDR encoding is complete
}

static int __attribute__((used,noinline)) spy_idr_capture(void)
{
    volatile unsigned int *hdr = (volatile unsigned int *)0x000FF000;
    unsigned int rb_base, idr_ptr_val, idr_size, data_at_ptr;
    unsigned int dma_base, enc_handle, sps_ptr, frm_cnt;

    if (hdr[0] != 0x52455753) { idr_sent = 0; return 0; }
    if (idr_sent >= 2) return 0;  // fire on first 2 msg 6 calls
    idr_sent++;

    rb_base     = *(volatile unsigned int *)0xFF93050C;
    idr_ptr_val = (rb_base) ? *(volatile unsigned int *)(rb_base + 0xD8) : 0;
    idr_size    = (rb_base) ? *(volatile unsigned int *)(rb_base + 0xDC) : 0;
    data_at_ptr = (idr_ptr_val && idr_ptr_val < 0x40000000)
                  ? *(volatile unsigned int *)idr_ptr_val : 0xDEADDEAD;
    dma_base    = (rb_base) ? *(volatile unsigned int *)(rb_base + 0xD0) : 0;
    enc_handle  = *(volatile unsigned int *)(0x51A8 + 0x7C);
    sps_ptr     = (rb_base) ? *(volatile unsigned int *)(rb_base + 0x8C) : 0;
    frm_cnt     = (rb_base) ? *(volatile unsigned int *)(rb_base + 0x24) : 0;

    spy_debug_reset();
    spy_debug_add('S','r','c','_', (idr_sent == 1) ? 0x4D362E31 : 0x4D362E32);
    spy_debug_add('R','B','a','s', rb_base);
    spy_debug_add('D','M','A','b', dma_base);
    spy_debug_add('I','d','r','P', idr_ptr_val);
    spy_debug_add('I','d','r','S', idr_size);
    spy_debug_add('D','a','t','P', data_at_ptr);
    spy_debug_add('E','n','c','H', enc_handle);
    spy_debug_add('S','P','S','p', sps_ptr);
    spy_debug_add('F','C','n','t', frm_cnt);
    spy_debug_add('M','5','C','t', msg5_count);
    spy_debug_add('M','5','D','n', msg5_done);
    spy_debug_send();

    return 0;
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
                 "BL      spy_msg5_debug\n"  // Capture +0xD8/+0xDC right after IDR encoding
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

                // One-shot IDR capture: on first frame, send IDR instead of P-frame
                "BL      spy_idr_capture\n"    // Returns 1 if IDR was sent
                "CMP     R0, #0\n"
                "BNE     loc_FF85DA24\n"       // Skip P-frame if IDR was sent
                // Normal P-frame delivery
                "LDR     R0, [SP, #0x34]\n"    // R0 = jpeg_ptr from sub_FF92FE8C
                "LDR     R1, [SP, #0x30]\n"    // R1 = jpeg_size
                "BL      spy_ring_write\n"     // Send P-frame

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
                 "BL      sub_FF8274B4\n"
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
                 "BL      sub_FF8274B4\n"
                 "CMP     R0, #9\n"
                 "BNE     loc_FF85DBB4\n"
 "loc_FF85DBA4:\n"
                 "BL      sub_FF930358\n"
                 "MOV     R0, #0x90000\n"
                 "STR     R5, [R6,#0x3C]\n"
                 "B       loc_FF85DB18\n"
 "loc_FF85DBB4:\n"
                 "LDR     R0, [SP,#0x38]\n"
                 "CMP     R0, #0\n"
                 "BEQ     loc_FF85DBD0\n"
 "loc_FF85DBC0:\n"
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
                 "BL      sub_FF8274B4\n"
                 "CMP     R0, #9\n"
                 "BNE     loc_FF85DC64\n"
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
