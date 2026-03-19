$bytes = [System.IO.File]::ReadAllBytes('C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN')
$base = 0xFF810000

function ReadU32($addr) {
    $off = $addr - $base
    if ($off -ge 0 -and $off -lt ($bytes.Length - 3)) {
        return [BitConverter]::ToUInt32($bytes, $off)
    }
    return $null
}

Write-Output "=== Config Lookup Table at ROM 0xFFAD748C ==="
Write-Output "Maps param_1 (PS3_state[+4]) -> buffer_index (iVar3)"
Write-Output "Used by JPCORE_DMA_Start: iVar3 = *(DAT_ff849764 + param_1 * 4)"
for ($i = 0; $i -lt 16; $i++) {
    $v = ReadU32 (0xFFAD748C + $i * 4)
    if ($null -ne $v) {
        $vInt = [int]$v
        if ($vInt -eq -1 -or $v -eq 0xFFFFFFFF) {
            Write-Output ("  [{0,2}] = 0xFFFFFFFF (-1) => INVALID" -f $i)
        } else {
            Write-Output ("  [{0,2}] = 0x{1:X8} ({1})" -f $i, $v)
        }
    }
}

Write-Output ""
Write-Output "=== Quality Table (DAT_ff84923c) ==="
$qualBase = ReadU32 0xFF84923C
Write-Output ("DAT_ff84923c = 0x{0:X8}" -f $qualBase)
if ($qualBase -ge 0xFF800000) {
    Write-Output "ROM address - reading entries:"
    for ($i = 0; $i -lt 8; $i++) {
        $v = ReadU32 ($qualBase + $i * 4)
        if ($null -ne $v) {
            Write-Output ("  [{0}] = 0x{1:X8}" -f $i, $v)
        }
    }
} else {
    Write-Output "RAM address - runtime values only"
}

Write-Output ""
Write-Output "=== Status Lookup Table (DAT_ff849768 = 0x00012850) ==="
Write-Output "RAM address 0x12850 - values set at runtime by FUN_ff8496a8"
Write-Output "Maps buffer_index (iVar3) -> status (must be 1 for JPCORE_DMA_Start)"

Write-Output ""
Write-Output "=== Callback/Buffer Array (DAT_ff84976c = 0x00002580) ==="
Write-Output "RAM address 0x2580 - runtime structure"
Write-Output "Layout:"
Write-Output "  [0] = callback function pointer"
Write-Output "  [1] = callback argument"
Write-Output "  [2 + iVar3*2] = output buffer address for buffer_index iVar3"
Write-Output "  [3 + iVar3*2] = output buffer size for buffer_index iVar3"
Write-Output ""
Write-Output "piVar1[4] = puVar2[iVar3 * 2 + 2] = output buffer address"

Write-Output ""
Write-Output "=== JPCORE State Struct (DAT_ff84924c = 0x2554) ==="
Write-Output "RAM address 0x2554"
Write-Output "Offsets (int[]):"
Write-Output "  [0]  +0x00 = initialized flag (1 after FUN_ff8496a8)"
Write-Output "  [1]  +0x04 = counter (incremented by FUN_ff8491a4)"
Write-Output "  [2]  +0x08 = secondary counter"
Write-Output "  [3]  +0x0C = JPCORE active flag (checked by interrupt handler)"
Write-Output "  [4]  +0x10 = output buffer address (piVar1[4], set from DAT_ff84976c)"
Write-Output "  [5]  +0x14 = quality index (CRITICAL: -1 = not configured, causes failure)"
Write-Output "  [6]  +0x18 = secondary quality index"
Write-Output "  [7]  +0x1C = event flag / semaphore handle"
Write-Output "  [8]  +0x20 = busy flag (must be 0 for JPCORE_DMA_Start)"
Write-Output "  [9]  +0x24 = secondary busy flag"
Write-Output "  [10] +0x28 = current buffer_index (piVar1[10] = iVar3)"

Write-Output ""
Write-Output "=== PS3 State (DAT_ff8f9028 = 0x8224) ==="
Write-Output "RAM address 0x8224"
Write-Output "Offsets:"
Write-Output "  +0x00 = initialized flag (set to 1 by FUN_ff8f8fc8)"
Write-Output "  +0x04 = PipelineStep2 value (param for JPCORE_DMA_Start)"
Write-Output "  +0x08 = encoding mode flag"
Write-Output "  +0x0C = completion bitmask (7 = all 3 steps complete)"
Write-Output "  +0x10 = JPCORE_DMA_Start result (step 1: 0=success, 1=fail)"
Write-Output "  +0x14 = step 2 result"
Write-Output "  +0x18 = step 3 result"
Write-Output "  +0x1C = barrier flag"

Write-Output ""
Write-Output "=== JPCORE_FrameComplete (FUN_ff8f8ce8) Output Path ==="
Write-Output "When completion_mask reaches 7 (all 3 steps done):"
Write-Output "  1. Resets mask to 0"
Write-Output "  2. Re-calculates mask from step results"
Write-Output "  3. Checks DAT_ff84924c[+0x0C] == 1 (JPCORE active)"
Write-Output "  4. Calls FUN_ff827460 to try semaphore (check if ready for next frame)"
Write-Output "  5. Calls FUN_ff8ef7f8(0xb, *(DAT_ff84924c + 0x10))"
Write-Output "     => JPCORE_SetOutputBuf(0xb, piVar1[4])"
Write-Output "     => Sets NEXT frame's output buffer to piVar1[4]"
Write-Output "  6. Writes 1 to DMA control register to trigger next frame"
Write-Output ""
Write-Output "NOTE: The JPEG data is at piVar1[4] (JPCORE state +0x10)"
Write-Output "      which was set from DAT_ff84976c[iVar3 * 2 + 2]"
Write-Output "      This is a RAM address set at runtime during JPCORE init."

Write-Output ""
Write-Output "=== FUN_ff8c335c FrameDispatch param_3 Analysis ==="
Write-Output "The param_3 passed to FrameDispatch comes from FUN_ff9e6ce8"
Write-Output "which calls XREF_FUN_ff8c335c(auStack_28, local_2c)"
Write-Output "auStack_28 is filled by XREF_FUN_ff9e6530 which processes pipeline output"
Write-Output "param_3 in FrameDispatch = what gets written to *(state[+0x6C])"
Write-Output "This appears to be the ISP output buffer address (frame data pointer)"
