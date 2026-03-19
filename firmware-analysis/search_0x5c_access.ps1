# Search firmware ROM for all ARM instructions that access offset 0x5C from a register.
# This finds code that reads or writes the DMA request state field in the MJPEG state struct.
#
# ARM encoding:
#   LDR Rd, [Rn, #0x5C]  = E59nd05C (big endian) = 5C d0 9n E5 (little endian bytes)
#   STR Rd, [Rn, #0x5C]  = E58nd05C (big endian) = 5C d0 8n E5 (little endian bytes)
#   LDRB/STRB would have different opcodes but offset 0x5C for word access is what we want.

$firmwarePath = "C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN"
$baseAddr = 0xFF810000
$bytes = [System.IO.File]::ReadAllBytes($firmwarePath)

Write-Output "=== Searching for LDR Rd, [Rn, #0x5C] instructions ==="
Write-Output "Pattern: byte[0]=0x5C, byte[2]=0x9n, byte[3]=0xE5"
Write-Output ""

$results = @()

for ($i = 0; $i -lt ($bytes.Length - 3); $i += 4) {
    $b0 = $bytes[$i]
    $b1 = $bytes[$i+1]
    $b2 = $bytes[$i+2]
    $b3 = $bytes[$i+3]

    # Check for LDR: byte0=0x5C, byte2 high nibble=9, byte3=0xE5
    if ($b0 -eq 0x5C -and ($b2 -band 0xF0) -eq 0x90 -and $b3 -eq 0xE5) {
        $addr = $baseAddr + $i
        $rn = $b2 -band 0x0F
        $rd = ($b1 -band 0xF0) -shr 4
        $results += [PSCustomObject]@{
            Address = "0x$($addr.ToString('X8'))"
            Type = "LDR"
            Rd = "R$rd"
            Rn = "R$rn"
            Instruction = "LDR R$rd, [R$rn, #0x5C]"
        }
    }

    # Check for STR: byte0=0x5C, byte2 high nibble=8, byte3=0xE5
    if ($b0 -eq 0x5C -and ($b2 -band 0xF0) -eq 0x80 -and $b3 -eq 0xE5) {
        $addr = $baseAddr + $i
        $rn = $b2 -band 0x0F
        $rd = ($b1 -band 0xF0) -shr 4
        $results += [PSCustomObject]@{
            Address = "0x$($addr.ToString('X8'))"
            Type = "STR"
            Rd = "R$rd"
            Rn = "R$rn"
            Instruction = "STR R$rd, [R$rn, #0x5C]"
        }
    }
}

Write-Output "Found $($results.Count) instructions accessing offset 0x5C:"
Write-Output ""
$results | Format-Table -AutoSize

# Also read DAT_ff84924c to find the JPCORE state struct address
Write-Output ""
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

$offset = 0xFF84923C - $baseAddr
$val = [BitConverter]::ToUInt32($bytes, $offset)
Write-Output "DAT_ff84923c (QT table pointer)    = 0x$($val.ToString('X8'))"

$offset = 0xFF8F000C - $baseAddr
$val = [BitConverter]::ToUInt32($bytes, $offset)
Write-Output "DAT_ff8f000c (HW register base)    = 0x$($val.ToString('X8'))"
