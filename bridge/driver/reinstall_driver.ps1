# Reinstall WinUSB driver for Canon IXUS 870 IS with correct DeviceInterfaceGUID
# Must be run as Administrator

$ErrorActionPreference = "Stop"

$deviceInstanceId = "USB\VID_04A9&PID_3196\1DDC1E390BA44A1984543E22C72C3E73"
$infPath = "C:\projects\ixus870IS\bridge\driver\canon_chdk.inf"

Write-Host "=== Canon IXUS 870 IS WinUSB Driver Reinstall ===" -ForegroundColor Cyan

# Step 1: Find and remove old driver
Write-Host "`n[1/4] Looking for existing OEM driver..."
$drivers = pnputil /enum-drivers /format txt 2>&1
$lines = $drivers -split "`n"
$currentOem = $null
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'oem\d+\.inf') {
        $currentOem = ($lines[$i] -split ':')[1].Trim()
    }
    if ($lines[$i] -match 'canon_chdk') {
        Write-Host "  Found: $currentOem (canon_chdk.inf)"
        Write-Host "  Removing old driver..."
        pnputil /delete-driver $currentOem /uninstall /force 2>&1
        Write-Host "  Removed." -ForegroundColor Green
        break
    }
}

# Step 2: Remove the device to clear cached GUID
Write-Host "`n[2/4] Removing device to clear cached interface GUID..."
pnputil /remove-device "$deviceInstanceId" 2>&1
Start-Sleep -Seconds 2

# Step 3: Add new driver with updated GUID
Write-Host "`n[3/4] Installing updated driver..."
$result = pnputil /add-driver "$infPath" /install 2>&1
Write-Host $result

# Step 4: Rescan for devices
Write-Host "`n[4/4] Scanning for hardware changes..."
pnputil /scan-devices 2>&1
Start-Sleep -Seconds 3

# Verify
Write-Host "`n=== Verification ===" -ForegroundColor Cyan
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_04A9' }
if ($dev) {
    Write-Host "Device found:" -ForegroundColor Green
    Write-Host "  Name: $($dev.FriendlyName)"
    Write-Host "  Status: $($dev.Status)"
    Write-Host "  Service: $($dev.Service)"

    # Check the interface GUID
    $interfaces = pnputil /enum-interfaces /instanceid "$deviceInstanceId" 2>&1
    Write-Host "`nDevice interfaces:"
    Write-Host $interfaces
} else {
    Write-Host "WARNING: Device not found after reinstall!" -ForegroundColor Yellow
    Write-Host "Try unplugging and replugging the camera."
}

Write-Host "`nDone. Press Enter to close."
Read-Host
