# Read ROM values at key addresses to find callback function pointers
# Firmware: IXUS 870 IS / SD 880 IS, version 1.01a
# Base address: 0xFF810000

$firmwarePath = "C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN"
$baseAddr = 0xFF810000

function Read-ROM-U32 {
    param([long]$romAddr)
    $offset = $romAddr - $baseAddr
    if ($offset -lt 0 -or $offset -gt 0x800000) {
        Write-Output "  ERROR: offset $offset out of range for addr 0x$($romAddr.ToString('X8'))"
        return
    }
    $bytes = [System.IO.File]::ReadAllBytes($firmwarePath)
    $val = [BitConverter]::ToUInt32($bytes, $offset)
    Write-Output "  ROM[0x$($romAddr.ToString('X8'))] = 0x$($val.ToString('X8'))  (offset 0x$($offset.ToString('X')))"
    return $val
}

Write-Output "=== ROM Callback Pointers ==="
Write-Output ""

Write-Output "DAT_ff8f8ce8 (JPCORE completion callback passed to FUN_ff849448):"
Read-ROM-U32 0xFF8F8CE8

Write-Output ""
Write-Output "DAT_ff8f9028 (PipelineStep3 state struct):"
Read-ROM-U32 0xFF8F9028

Write-Output ""
Write-Output "DAT_ffaa1bb8 (event flag handle for GetContMovJpeg):"
Read-ROM-U32 0xFFAA1BB8

Write-Output ""
Write-Output "DAT_ff8c2e2c (LiveImageTask state ptr passed to FUN_ff9e8360):"
Read-ROM-U32 0xFF8C2E2C

Write-Output ""
Write-Output "DAT_ff9e8238 (LiveImageTask state - used by FUN_ff9e8104):"
Read-ROM-U32 0xFF9E8238

Write-Output ""
Write-Output "DAT_ff9e824c (LiveImageTask message buffer array):"
Read-ROM-U32 0xFF9E824C

Write-Output ""
Write-Output "=== GetContinuousMovieJpegVRAMData function body ROM reads ==="
Write-Output ""

# Read bytes around 0xFFAA234C to understand the function structure
Write-Output "First 16 words of GetContinuousMovieJpegVRAMData (0xFFAA234C):"
$bytes = [System.IO.File]::ReadAllBytes($firmwarePath)
for ($i = 0; $i -lt 16; $i++) {
    $addr = 0xFFAA234C + ($i * 4)
    $offset = $addr - $baseAddr
    $val = [BitConverter]::ToUInt32($bytes, $offset)
    Write-Output "  0x$($addr.ToString('X8')): 0x$($val.ToString('X8'))"
}

Write-Output ""
Write-Output "=== StopContinuousVRAMData function body (0xFF8C425C) ==="
Write-Output ""
Write-Output "First 16 words of StopContinuousVRAMData:"
for ($i = 0; $i -lt 16; $i++) {
    $addr = 0xFF8C425C + ($i * 4)
    $offset = $addr - $baseAddr
    $val = [BitConverter]::ToUInt32($bytes, $offset)
    Write-Output "  0x$($addr.ToString('X8')): 0x$($val.ToString('X8'))"
}

Write-Output ""
Write-Output "=== DMA Completion Callback area (0xFFAA12B0) ==="
Write-Output ""
Write-Output "First 16 words:"
for ($i = 0; $i -lt 16; $i++) {
    $addr = 0xFFAA12B0 + ($i * 4)
    $offset = $addr - $baseAddr
    $val = [BitConverter]::ToUInt32($bytes, $offset)
    Write-Output "  0x$($addr.ToString('X8')): 0x$($val.ToString('X8'))"
}
