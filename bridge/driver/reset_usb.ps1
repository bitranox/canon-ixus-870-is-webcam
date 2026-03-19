$id = "USB\VID_04A9&PID_3196\1DDC1E390BA44A1984543E22C72C3E73"
Write-Host "Disabling device..."
pnputil /disable-device "$id" 2>&1
Start-Sleep -Seconds 3
Write-Host "Re-enabling device..."
pnputil /enable-device "$id" 2>&1
Start-Sleep -Seconds 3
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match "VID_04A9" }
Write-Host "Status: $($dev.FriendlyName) Service=$($dev.Service) Status=$($dev.Status)"
