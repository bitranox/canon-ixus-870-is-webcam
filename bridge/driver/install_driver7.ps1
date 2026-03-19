$log = "C:\projects\ixus870IS\bridge\driver\install_log7.txt"
Start-Transcript -Path $log -Force

$ErrorActionPreference = "Continue"
$driverDir = "C:\projects\ixus870IS\bridge\driver"
$infFile = "canon_chdk.inf"
$catFile = "canon_chdk.cat"
$signtool = "C:\Program Files (x86)\Windows Kits\10\App Certification Kit\signtool.exe"
$pwsh = "C:\Program Files\PowerShell\7\pwsh.exe"

Write-Host "=== CHDK WinUSB Driver Installer v7 (using pwsh for catalog) ==="

# Step 1: Find certificate
Write-Host "`n[1/5] Checking certificate..."
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store("My", "LocalMachine")
$store.Open("ReadOnly")
$cert = $null
foreach ($c in $store.Certificates) {
    if ($c.Subject -eq "CN=CHDK Driver Signing") {
        $cert = $c
        break
    }
}
$store.Close()

if (-not $cert) {
    Write-Host "  Creating new certificate..."
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=CHDK Driver Signing" -CertStoreLocation "Cert:\LocalMachine\My" -NotAfter (Get-Date).AddYears(5)
    foreach ($sn in @("Root", "TrustedPublisher")) {
        $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($sn, "LocalMachine")
        $s.Open("ReadWrite")
        $s.Add($cert)
        $s.Close()
    }
}
Write-Host "  Certificate: $($cert.Thumbprint)"
$thumbprint = $cert.Thumbprint

# Step 2: Create catalog using pwsh (PowerShell 7)
Write-Host "`n[2/5] Creating catalog file (via pwsh)..."
$tempDir = "$env:TEMP\chdk_driver_$(Get-Random)"
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
Copy-Item "$driverDir\$infFile" "$tempDir\$infFile"
$tempCat = "$tempDir\$catFile"
Remove-Item "$driverDir\$catFile" -ErrorAction SilentlyContinue

$catResult = & $pwsh -NoProfile -Command "New-FileCatalog -Path '$tempDir' -CatalogFilePath '$tempCat' -CatalogVersion 2.0; if (Test-Path '$tempCat') { 'SUCCESS' } else { 'FAIL' }" 2>&1
Write-Host "  Result: $catResult"

# Step 3: Sign catalog
Write-Host "`n[3/5] Signing catalog..."
if (Test-Path $tempCat) {
    $signResult = & $signtool sign /fd SHA256 /sha1 $thumbprint /s My /sm $tempCat 2>&1
    Write-Host "  Sign: $signResult"
    Copy-Item $tempCat "$driverDir\$catFile" -Force
} else {
    Write-Host "  ERROR: No catalog file!"
    Stop-Transcript
    exit 1
}

# Step 4: Install driver
Write-Host "`n[4/5] Installing driver..."
$result = & pnputil /add-driver "$driverDir\$infFile" /install 2>&1
foreach ($line in $result) { Write-Host "  $line" }

# Step 5: Update device
Write-Host "`n[5/5] Updating device..."
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -match "VID_04A9&PID_3196" }
if ($dev) {
    Write-Host "  Before: $($dev.FriendlyName) Service=$($dev.Service)"
    & pnputil /remove-device $dev.InstanceId 2>&1 | Out-Null
    Start-Sleep -Seconds 3
    & pnputil /scan-devices 2>&1 | Out-Null
    Start-Sleep -Seconds 5
    $dev2 = Get-PnpDevice | Where-Object { $_.InstanceId -match "VID_04A9&PID_3196" }
    if ($dev2) {
        Write-Host "  After: $($dev2.FriendlyName) Service=$($dev2.Service) Status=$($dev2.Status)"
    } else {
        Write-Host "  Not re-detected - try replugging USB"
    }
} else {
    Write-Host "  Camera not found"
}

Remove-Item $tempDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "`n=== Done ==="
Stop-Transcript
