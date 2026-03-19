$bytes = [System.IO.File]::ReadAllBytes('C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN')
$base = 0xFF810000

# Scan backwards from 0xFF8C2620 all the way to 0xFF8C1E00 for function prologues
Write-Output '=== Scanning 0xFF8C2400 back to 0xFF8C1E00 for PUSH ==='
for ($a = 0xFF8C2400; $a -gt 0xFF8C1E00; $a -= 4) {
    $off = $a - $base
    $instr = [BitConverter]::ToUInt32($bytes, $off)
    if (($instr -band 0xFFFF0000) -eq 0xE92D0000) {
        Write-Output ('  PUSH at 0x{0:X8}: 0x{1:X8}' -f $a, $instr)
    }
    if (($instr -band 0xFFFFF000) -eq 0xE24DD000) {
        Write-Output ('  SUB SP at 0x{0:X8}: 0x{1:X8}' -f $a, $instr)
    }
    if ($instr -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8} -- next instr at 0x{1:X8} may be function entry' -f $a, ($a + 4))
    }
    if (($instr -band 0xFFFF0000) -eq 0xE8BD0000 -and ($instr -band 0x8000) -ne 0) {
        Write-Output ('  POP+PC at 0x{0:X8}: 0x{1:X8} -- next at 0x{2:X8} may be entry' -f $a, $instr, ($a + 4))
    }
    if ($instr -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8} -- next at 0x{1:X8} may be entry' -f $a, ($a + 4))
    }
}

Write-Output ''
Write-Output '=== Also scanning for function returns between 0xFF8C2170 and 0xFF8C2620 ==='
for ($a = 0xFF8C2170; $a -lt 0xFF8C2620; $a += 4) {
    $off = $a - $base
    $instr = [BitConverter]::ToUInt32($bytes, $off)
    if (($instr -band 0xFFFF0000) -eq 0xE92D0000) {
        Write-Output ('  PUSH at 0x{0:X8}: 0x{1:X8}' -f $a, $instr)
    }
    if ($instr -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8}' -f $a)
    }
    if (($instr -band 0xFFFF0000) -eq 0xE8BD0000 -and ($instr -band 0x8000) -ne 0) {
        Write-Output ('  POP+PC at 0x{0:X8}: 0x{1:X8}' -f $a, $instr)
    }
    if ($instr -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8}' -f $a)
    }
}

Write-Output ''
Write-Output '=== Detailed decode of switch at 0xFF8C2620 ==='
for ($i = 0xFF8C2618; $i -lt 0xFF8C2650; $i += 4) {
    $off = $i - $base
    $instr = [BitConverter]::ToUInt32($bytes, $off)
    $hex = '0x{0:X8}' -f $instr
    
    $decoded = ''
    
    if (($instr -band 0x0F700000) -eq 0x05900000) {
        $rd = ($instr -shr 12) -band 0xF
        $rn = ($instr -shr 16) -band 0xF
        $imm = $instr -band 0xFFF
        $decoded = 'LDR R{0}, [R{1}, #0x{2:X}]' -f $rd, $rn, $imm
    }
    elseif (($instr -band 0x0FF00000) -eq 0x03500000) {
        $rn = ($instr -shr 16) -band 0xF
        $imm = $instr -band 0xFF
        $rot = (($instr -shr 8) -band 0xF) * 2
        if ($rot -gt 0) { $imm = ($imm -shr $rot) -bor ($imm -shl (32 - $rot)) }
        $decoded = 'CMP R{0}, #{1}' -f $rn, $imm
    }
    elseif (($instr -band 0x0FFFF000) -eq 0x008FF000 -or ($instr -band 0x0FFFFFFF) -eq 0x008FF100) {
        $rm = $instr -band 0xF
        $decoded = 'ADD PC, PC, R{0}, LSL#2 (jump table)' -f $rm
    }
    elseif (($instr -band 0x0F000000) -eq 0x0A000000) {
        $offset = $instr -band 0x00FFFFFF
        if ($offset -band 0x800000) { $offset = $offset -bor (-16777216) }
        $target = $i + 8 + $offset * 4
        $cond = ($instr -shr 28) -band 0xF
        $condStr = @('EQ','NE','CS','CC','MI','PL','VS','VC','HI','LS','GE','LT','GT','LE','AL','NV')[$cond]
        $decoded = 'B{0} 0x{1:X8}' -f $condStr, $target
    }
    elseif (($instr -band 0xF0000000) -eq 0x90000000 -and ($instr -band 0x0FFFFFFF) -eq 0x008FF100) {
        $rm = $instr -band 0xF
        $decoded = 'ADDLS PC, PC, R{0}, LSL#2' -f $rm
    }
    
    if ($decoded -eq '') { $decoded = '(undecoded)' }
    Write-Output ('  0x{0:X8}: {1}  {2}' -f $i, $hex, $decoded)
}

Write-Output ''
Write-Output '=== Detailed decode around 0xFF8C28C8 ==='
for ($i = 0xFF8C28A0; $i -lt 0xFF8C28F8; $i += 4) {
    $off = $i - $base
    $instr = [BitConverter]::ToUInt32($bytes, $off)
    $hex = '0x{0:X8}' -f $instr
    
    $decoded = ''
    if (($instr -band 0x0F700000) -eq 0x05800000) {
        $rd = ($instr -shr 12) -band 0xF
        $rn = ($instr -shr 16) -band 0xF
        $imm = $instr -band 0xFFF
        $decoded = 'STR R{0}, [R{1}, #0x{2:X}]' -f $rd, $rn, $imm
    }
    elseif (($instr -band 0x0F700000) -eq 0x05C00000) {
        $rd = ($instr -shr 12) -band 0xF
        $rn = ($instr -shr 16) -band 0xF
        $imm = $instr -band 0xFFF
        $decoded = 'STRB R{0}, [R{1}, #0x{2:X}]' -f $rd, $rn, $imm
    }
    elseif (($instr -band 0x0F700000) -eq 0x05900000) {
        $rd = ($instr -shr 12) -band 0xF
        $rn = ($instr -shr 16) -band 0xF
        $imm = $instr -band 0xFFF
        $decoded = 'LDR R{0}, [R{1}, #0x{2:X}]' -f $rd, $rn, $imm
    }
    elseif (($instr -band 0x0FF00000) -eq 0x03A00000) {
        $rd = ($instr -shr 12) -band 0xF
        $imm = $instr -band 0xFF
        $decoded = 'MOV R{0}, #{1}' -f $rd, $imm
    }
    elseif (($instr -band 0x0E000000) -eq 0x0A000000) {
        $offset = $instr -band 0x00FFFFFF
        if ($offset -band 0x800000) { $offset = $offset -bor (-16777216) }
        $target = $i + 8 + $offset * 4
        $isLink = ($instr -band 0x01000000) -ne 0
        $cond = ($instr -shr 28) -band 0xF
        $condStr = @('EQ','NE','CS','CC','MI','PL','VS','VC','HI','LS','GE','LT','GT','LE','AL','NV')[$cond]
        $prefix = if ($isLink) { 'BL' } else { 'B' }
        $decoded = '{0}{1} 0x{2:X8}' -f $prefix, $condStr, $target
    }
    
    if ($decoded -eq '') { $decoded = '(undecoded)' }
    Write-Output ('  0x{0:X8}: {1}  {2}' -f $i, $hex, $decoded)
}
