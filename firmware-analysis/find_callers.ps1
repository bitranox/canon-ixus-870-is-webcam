param([string]$TargetHex = "0xFF849408")

$bytes = [System.IO.File]::ReadAllBytes('C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN')
$base = [uint32]::Parse("FF810000", [System.Globalization.NumberStyles]::HexNumber)
$target = [uint32]::Parse($TargetHex.Replace("0x","").Replace("0X",""), [System.Globalization.NumberStyles]::HexNumber)

Write-Output ("Searching for BL instructions calling 0x{0:X8}..." -f $target)

$found = 0
for ($idx = 0; $idx -lt $bytes.Length - 3; $idx += 4) {
    $word = [BitConverter]::ToUInt32($bytes, $idx)
    # Check for BL instruction (condition code in top nibble, 0xB in next)
    if (($word -band 0x0F000000) -eq 0x0B000000) {
        $offset = [int]($word -band 0x00FFFFFF)
        if ($offset -band 0x800000) {
            $offset = $offset -bor ([int]0xFF000000)
        }
        $pc = [long]$base + [long]$idx + 8
        $dest = [uint32](($pc + [long]$offset * 4) -band 0xFFFFFFFF)
        if ($dest -eq $target) {
            $addr = [uint32]($base + $idx)
            Write-Output ("  BL at 0x{0:X8} -> 0x{1:X8}" -f $addr, $dest)
            $found++
        }
    }
}
Write-Output ("Total: {0} callers found" -f $found)
