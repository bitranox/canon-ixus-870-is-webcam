 = 0xFF810000

Write-Output '=== Scanning 0xFF8C1000 to 0xFF8C2A00 ==='
for ( -lt 0xFF8C2A00;  = 
    ,  -band 0xFFFF0000) -eq 0xE92D0000) {
        Write-Output ('  PUSH at 0x{0:X8}: 0x{1:X8}' -f )
    }
    if ((,  -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8}' -f  -band 0xFFFF0000) -eq 0xE8BD0000 -and (,  -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8}' -f  = 0xFF8C0800;  += 4) {
     -  = [BitConverter]::ToUInt32()
    if ((,  -eq 0xE12FFF1E) {
        Write-Output ('  BX LR at 0x{0:X8}' -f  -band 0xFFFF0000) -eq 0xE8BD0000 -and (,  -eq 0xE1A0F00E) {
        Write-Output ('  MOV PC, LR at 0x{0:X8}' -f  = 0xFF8C2900;  += 4) {
     -  = [BitConverter]::ToUInt32()
    if ((,  -band 0xFFFFF000) -eq 0xE24DD000) {
        Write-Output ('  SUB SP at 0x{0:X8}: 0x{1:X8}' -f )
    }
    if ()
    }
    if (( -band 0x8000) -ne 0) {
        Write-Output ('  POP+PC at 0x{0:X8}: 0x{1:X8}' -f )
    }
    if ()
    }
}
