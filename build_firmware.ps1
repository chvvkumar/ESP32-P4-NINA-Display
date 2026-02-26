# build_firmware.ps1
# Compiles the ESP32-P4 NINA Display project with ESP-IDF 5.5.2
# and generates factory + OTA firmware binaries in the firmware/ folder.
#
# Usage:
#   .\build_firmware.ps1              # Normal build
#   .\build_firmware.ps1 -FullClean   # Clean rebuild
#   .\build_firmware.ps1 -OTA         # Build + OTA flash to device
#   .\build_firmware.ps1 -OTA -DeviceIP 192.168.1.100

param(
    [string]$IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.2",
    [switch]$FullClean,
    [switch]$OTA,
    [string]$DeviceIP = "192.168.1.201"
)

$ErrorActionPreference = "Stop"

# Interactive menu when invoked without arguments
if ($PSBoundParameters.Count -eq 0) {
    function Show-Menu {
        param($IdfPath, $FullClean, $OTA, $DeviceIP)
        Write-Host ""
        Write-Host "  ESP32-P4 NINA Display - Build Options" -ForegroundColor Cyan
        Write-Host "  ======================================" -ForegroundColor Cyan
        Write-Host ""
        $cleanLabel = if ($FullClean) { "Yes" } else { "No" }
        $cleanColor = if ($FullClean) { "Yellow" } else { "DarkGray" }
        $otaLabel = if ($OTA) { "Yes" } else { "No" }
        $otaColor = if ($OTA) { "Yellow" } else { "DarkGray" }
        Write-Host "  [1] Full Clean:  " -NoNewline; Write-Host $cleanLabel -ForegroundColor $cleanColor
        Write-Host "  [2] OTA Flash:   " -NoNewline; Write-Host $otaLabel -ForegroundColor $otaColor
        Write-Host "  [3] Device IP:   " -NoNewline; Write-Host $DeviceIP -ForegroundColor White
        Write-Host "  [4] IDF Path:    " -NoNewline; Write-Host $IdfPath -ForegroundColor White
        Write-Host ""
    }

    $OTA = $true  # Default to OTA enabled in interactive mode
    $menuLoop = $true
    while ($menuLoop) {
        Show-Menu -IdfPath $IdfPath -FullClean $FullClean -OTA $OTA -DeviceIP $DeviceIP
        $selection = Read-Host "  Enter option numbers to toggle (e.g. 1,2), or press Enter to build"
        if ([string]::IsNullOrWhiteSpace($selection)) {
            $menuLoop = $false
            continue
        }
        foreach ($choice in ($selection -split '[,\s]+')) {
            switch ($choice.Trim()) {
                '1' { $FullClean = -not $FullClean }
                '2' { $OTA = -not $OTA }
                '3' {
                    $newIP = Read-Host "  Device IP [$DeviceIP]"
                    if (-not [string]::IsNullOrWhiteSpace($newIP)) { $DeviceIP = $newIP }
                }
                '4' {
                    $newPath = Read-Host "  IDF Path [$IdfPath]"
                    if (-not [string]::IsNullOrWhiteSpace($newPath)) { $IdfPath = $newPath }
                }
                default { Write-Host "  Unknown option: $choice" -ForegroundColor Red }
            }
        }
    }

    # Show final summary
    $summary = @("Build")
    if ($FullClean) { $summary += "FullClean" }
    if ($OTA) { $summary += "OTA -> $DeviceIP" }
    Write-Host ""
    Write-Host "  Building: $($summary -join ' + ')" -ForegroundColor Cyan
    Write-Host ""
}

$ProjectDir = $PSScriptRoot
$FirmwareDir = Join-Path $ProjectDir "firmware"
$BuildDir = Join-Path $ProjectDir "build"

# Verify ESP-IDF path exists
if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    Write-Error "ESP-IDF not found at $IdfPath. Check the path and try again."
    exit 1
}

# Activate ESP-IDF environment
Write-Host "Activating ESP-IDF environment from $IdfPath ..." -ForegroundColor Cyan
& (Join-Path $IdfPath "export.ps1")
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    Write-Error "Failed to activate ESP-IDF environment."
    exit 1
}

# Full clean if requested or if build dir has a Python mismatch
$CmakeCache = Join-Path $BuildDir "CMakeCache.txt"
if (-not $FullClean -and (Test-Path $CmakeCache)) {
    $cached = Select-String -Path $CmakeCache -Pattern "PYTHON.*=(.+)" -AllMatches | ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
    $activePython = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($cached -and $activePython -and ($cached -ne $activePython)) {
        Write-Host "Detected Python mismatch (cached: $cached, active: $activePython). Running fullclean ..." -ForegroundColor Yellow
        $FullClean = $true
    }
}

if ($FullClean) {
    Write-Host "`nRunning fullclean ..." -ForegroundColor Yellow
    Push-Location $ProjectDir
    try {
        idf.py fullclean
    } finally {
        Pop-Location
    }
}

# Build the project
Write-Host "`nBuilding project ..." -ForegroundColor Cyan
Push-Location $ProjectDir
try {
    idf.py build
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed."
        exit 1
    }
} finally {
    Pop-Location
}

# Create firmware output directory
if (-not (Test-Path $FirmwareDir)) {
    New-Item -ItemType Directory -Path $FirmwareDir | Out-Null
}

# Paths to build artifacts
$Bootloader = Join-Path $BuildDir "bootloader" "bootloader.bin"
$PartTable  = Join-Path $BuildDir "partition_table" "partition-table.bin"

# Detect the app binary name from the CMake project name
$ProjectDescJson = Join-Path $BuildDir "project_description.json"
if (Test-Path $ProjectDescJson) {
    $projDesc = Get-Content $ProjectDescJson -Raw | ConvertFrom-Json
    $AppBin = Join-Path $BuildDir "$($projDesc.app_bin)"
} else {
    # Fallback: find the largest .bin in build/ that isn't bootloader or partition table
    $AppBin = Get-ChildItem -Path $BuildDir -Filter "*.bin" -File |
              Where-Object { $_.Name -notmatch "bootloader|partition" } |
              Sort-Object Length -Descending |
              Select-Object -First 1 -ExpandProperty FullName
}

# Verify all required binaries exist
foreach ($bin in @($Bootloader, $PartTable, $AppBin)) {
    if (-not $bin -or -not (Test-Path $bin)) {
        Write-Error "Missing build artifact: $bin"
        exit 1
    }
}
Write-Host "App binary: $AppBin" -ForegroundColor Gray

$FactoryBin = Join-Path $FirmwareDir "nina-display-factory.bin"
$OtaBin     = Join-Path $FirmwareDir "nina-display-ota.bin"

# Merge into a single factory binary using esptool
# ESP32-P4 OTA layout: bootloader@0x2000, partition-table@0x8000, app@0x20000 (ota_0)
Write-Host "`nMerging into factory firmware binary ..." -ForegroundColor Cyan
python -m esptool --chip esp32p4 merge_bin `
    --flash_mode dio `
    --flash_size 32MB `
    --flash_freq 80m `
    -o $FactoryBin `
    0x2000  $Bootloader `
    0x8000  $PartTable `
    0x20000 $AppBin

if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to merge firmware binary."
    exit 1
}

# Copy app binary as OTA update file (raw app image, no bootloader/partition table)
Copy-Item -Path $AppBin -Destination $OtaBin -Force

$FactorySize = (Get-Item $FactoryBin).Length
$FactorySizeMB = [math]::Round($FactorySize / 1MB, 2)
$OtaSize = (Get-Item $OtaBin).Length
$OtaSizeMB = [math]::Round($OtaSize / 1MB, 2)

Write-Host "`nFirmware generated successfully!" -ForegroundColor Green
Write-Host "  Factory: $FactoryBin" -ForegroundColor Green
Write-Host "           $FactorySizeMB MB ($FactorySize bytes)" -ForegroundColor Green
Write-Host "  OTA:     $OtaBin" -ForegroundColor Green
Write-Host "           $OtaSizeMB MB ($OtaSize bytes)" -ForegroundColor Green
Write-Host "`nFlash with: esptool.py --chip esp32p4 write_flash 0x0 `"$FactoryBin`"" -ForegroundColor Yellow
Write-Host "OTA update: Upload `"$OtaBin`" via the web interface at http://<device-ip>/" -ForegroundColor Yellow

# OTA flash to device if requested
if ($OTA) {
    $OtaUrl = "http://${DeviceIP}/api/ota"
    Write-Host "`nUploading OTA firmware to $OtaUrl ..." -ForegroundColor Cyan

    try {
        $fileBytes = [System.IO.File]::ReadAllBytes($OtaBin)
        $null = Invoke-WebRequest -Uri $OtaUrl -Method Post `
            -Body $fileBytes `
            -ContentType "application/octet-stream" `
            -TimeoutSec 600
        Write-Host "OTA update successful! Device is rebooting." -ForegroundColor Green
    } catch {
        Write-Error "OTA upload failed: $_"
        exit 1
    }
}
