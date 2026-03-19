 = [System.IO.File]::ReadAllBytes('C:\projects\ixus870IS\firmware-analysis\ixus870_sd880\sub\101a\PRIMARY.BIN')
 = 0xFF810000

Write-Output '=== Scanning 0xFF8C1000 to 0xFF8C2A00 for ALL prologues/epilogues ==='
for ( = 0xFF8C1000;  -lt 0xFF8C2A00;  += 4) {
     =  - 
     = [BitConverter]::ToUInt32(, )
    if (( -band 0xFFFF0000) -eq 0xE92D0000) {
        Write-Output ('  PUSH at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if (( -band 0xFFFFF000) -eq 0xE24DD000) {
        Write-Output ('  SUB SP at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if ( -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8}' -f )
    }
    if (( -band 0xFFFF0000) -eq 0xE8BD0000 -and ( -band 0x8000) -ne 0) {
        Write-Output ('  POP+PC at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if ( -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8}' -f )
    }
}

Write-Output ''
Write-Output '=== Scanning 0xFF8C0800 to 0xFF8C1000 for prologues/epilogues ==='
for ( = 0xFF8C0800;  -lt 0xFF8C1000;  += 4) {
     =  - 
     = [BitConverter]::ToUInt32(, )
    if (( -band 0xFFFF0000) -eq 0xE92D0000) {
        Write-Output ('  PUSH at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if ( -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8}' -f )
    }
    if (( -band 0xFFFF0000) -eq 0xE8BD0000 -and ( -band 0x8000) -ne 0) {
        Write-Output ('  POP+PC at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if ( -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8}' -f )
    }
}

Write-Output ''
Write-Output '=== Scanning 0xFF8C2900 to 0xFF8C3100 for end of function ==='
for ( = 0xFF8C2900;  -lt 0xFF8C3100;  += 4) {
     =  - 
     = [BitConverter]::ToUInt32(, )
    if (( -band 0xFFFF0000) -eq 0xE92D0000) {
        Write-Output ('  PUSH at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if (( -band 0xFFFFF000) -eq 0xE24DD000) {
        Write-Output ('  SUB SP at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if ( -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8}' -f )
    }
    if (( -band 0xFFFF0000) -eq 0xE8BD0000 -and ( -band 0x8000) -ne 0) {
        Write-Output ('  POP+PC at 0x{0:X8}: 0x{1:X8}' -f , )
    }
    if ( -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8}' -f )
    }
}