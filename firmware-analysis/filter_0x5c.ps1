# Filter +0x5C accesses to functions near the MJPEG state struct users
# Focus on: 0xFF8C2000-0xFF8C5000 (LiveImage), 0xFF849000-0xFF84A000 (JPCORE),
# 0xFF8E0000-0xFF900000 (pipeline), 0xFF9E0000-0xFF9F0000 (video), 0xFFAA0000-0xFFAB0000 (GetContMov)
# Also exclude PC-relative (R15) since those are literal pool loads, not struct accesses

$firmwarePath = "C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN"
$baseAddr = 0xFF810000
$bytes = [System.IO.File]::ReadAllBytes($firmwarePath)

$ranges = @(
    @{ Start = 0xFF8C2000; End = 0xFF8C5000; Name = "LiveImage/EVF" },
    @{ Start = 0xFF849000; End = 0xFF84A000; Name = "JPCORE_DMA" },
    @{ Start = 0xFF8E0000; End = 0xFF900000; Name = "Pipeline/Encoder" },
    @{ Start = 0xFF920000; End = 0xFF940000; Name = "MovieRec" },
    @{ Start = 0xFF9E0000; End = 0xFF9F0000; Name = "Video/MJPEG" },
    @{ Start = 0xFFAA0000; End = 0xFFAB0000; Name = "GetContMovJpeg" }
)

Write-Output "=== Filtered +0x5C accesses (excluding PC-relative) ==="
Write-Output ""

foreach ($range in $ranges) {
    $startOff = $range.Start - $baseAddr
    $endOff = $range.End - $baseAddr
    $found = @()

    for ($i = $startOff; $i -lt $endOff -and $i -lt ($bytes.Length - 3); $i += 4) {
        $b0 = $bytes[$i]
        $b1 = $bytes[$i+1]
        $b2 = $bytes[$i+2]
        $b3 = $bytes[$i+3]
        $rn = $b2 -band 0x0F

        # Skip PC-relative (R15) - those are literal pool loads, not struct accesses
        if ($rn -eq 15) { continue }

        $addr = $baseAddr + $i

        # Check for LDR
        if ($b0 -eq 0x5C -and ($b2 -band 0xF0) -eq 0x90 -and $b3 -eq 0xE5) {
            $rd = ($b1 -band 0xF0) -shr 4
            $found += "  0x$($addr.ToString('X8'))  LDR R$rd, [R$rn, #0x5C]"
        }

        # Check for STR
        if ($b0 -eq 0x5C -and ($b2 -band 0xF0) -eq 0x80 -and $b3 -eq 0xE5) {
            $rd = ($b1 -band 0xF0) -shr 4
            $found += "  0x$($addr.ToString('X8'))  STR R$rd, [R$rn, #0x5C]"
        }
    }

    if ($found.Count -gt 0) {
        Write-Output "--- $($range.Name) (0x$($range.Start.ToString('X8'))-0x$($range.End.ToString('X8'))) ---"
        foreach ($f in $found) { Write-Output $f }
        Write-Output ""
    }
}

# Also read the additional ROM values
Write-Output "=== Additional ROM values ==="
$offset = 0xFF84924C - $baseAddr
$val = [BitConverter]::ToUInt32($bytes, $offset)
Write-Output "DAT_ff84924c (JPCORE state struct) = 0x$($val.ToString('X8'))"

$offset = 0xFF849764 - $baseAddr
$val = [BitConverter]::ToUInt32($bytes, $offset)
Write-Output "DAT_ff849764 (output config table) = 0x$($val.ToString('X8'))"

$offset = 0xFF849768 - $baseAddr
$val = [BitConverter]::ToUInt32($bytes, $offset)
Write-Output "DAT_ff849768 (output type table)   = 0x$($val.ToString('X8'))"

$offset = 0xFF84976C - $baseAddr
$val = [BitConverter]::ToUInt32($bytes, $offset)
Write-Output "DAT_ff84976c (callback/buf array)  = 0x$($val.ToString('X8'))"
