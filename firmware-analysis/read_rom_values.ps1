$bytes = [System.IO.File]::ReadAllBytes('C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN')
$base = 0xFF810000

function Read-ROM-U32($addr) {
    $off = $addr - $base
    return [BitConverter]::ToUInt32($bytes, $off)
}

$v1 = Read-ROM-U32 0xFF8C2E24
$v2 = Read-ROM-U32 0xFF8C43F4
$v3 = Read-ROM-U32 0xFF9E8250
$v4 = Read-ROM-U32 0xFFAA1BB8
$v5 = Read-ROM-U32 0xFF8C4418
$v6 = Read-ROM-U32 0xFF8C2E98

Write-Host ("DAT_ff8c2e24 (EVF state base)   = 0x{0:X8}" -f $v1)
Write-Host ("DAT_ff8c43f4 (DMA state base)   = 0x{0:X8}" -f $v2)
Write-Host ("DAT_ff9e8250 (JPCORE flag addr)  = 0x{0:X8}" -f $v3)
Write-Host ("DAT_ffaa1bb8 (event flag addr)   = 0x{0:X8}" -f $v4)
Write-Host ("DAT_ff8c4418 (DMA trigger const) = 0x{0:X8}" -f $v5)
Write-Host ("DAT_ff8c2e98 (resolution ptr)    = 0x{0:X8}" -f $v6)

if ($v1 -eq $v2) {
    Write-Host "`nRESULT: DAT_ff8c2e24 and DAT_ff8c43f4 point to the SAME structure!"
} else {
    Write-Host ("`nRESULT: DAT_ff8c2e24 and DAT_ff8c43f4 point to DIFFERENT structures!")
    Write-Host ("  EVF struct at 0x{0:X8}, DMA struct at 0x{1:X8}" -f $v1, $v2)
    Write-Host ("  Offset between them: 0x{0:X}" -f [Math]::Abs($v2 - $v1))
}
