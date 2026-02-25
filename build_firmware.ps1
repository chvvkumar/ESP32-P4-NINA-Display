# build_firmware.ps1
# Compiles the ESP32-P4 NINA Display project with ESP-IDF 5.5.2
# and generates factory + OTA firmware binaries in the firmware/ folder.
#
# Usage:
#   .\build_firmware.ps1              # Normal (release) build
#   .\build_firmware.ps1 -Perf        # Performance profiling build
#   .\build_firmware.ps1 -FullClean   # Clean rebuild
#   .\build_firmware.ps1 -OTA         # Build + OTA flash to device
#   .\build_firmware.ps1 -OTA -DeviceIP 192.168.1.100

param(
    [string]$IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.2",
    [switch]$FullClean,
    [switch]$Perf,
    [switch]$OTA,
    [string]$DeviceIP = "192.168.1.201"
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$FirmwareDir = Join-Path $ProjectDir "firmware"
$BuildDir = Join-Path $ProjectDir "build"
$SdkConfig = Join-Path $ProjectDir "sdkconfig"
$BuildModeFile = Join-Path $BuildDir ".build_mode"
$BuildMode = if ($Perf) { "perf" } else { "release" }

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

# Detect build mode switch — delete sdkconfig to force clean reconfigure
if (-not $FullClean -and (Test-Path $BuildModeFile) -and (Test-Path $SdkConfig)) {
    $prevMode = (Get-Content $BuildModeFile -Raw).Trim()
    if ($prevMode -ne $BuildMode) {
        Write-Host "Build mode changed ($prevMode -> $BuildMode), removing sdkconfig to reconfigure ..." -ForegroundColor Yellow
        Remove-Item $SdkConfig -Force
    }
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

# Configure performance monitoring in sdkconfig
if ($Perf) {
    Write-Host "`nBuild mode: PERF (performance profiling enabled)" -ForegroundColor Magenta
} else {
    Write-Host "`nBuild mode: RELEASE (performance profiling disabled)" -ForegroundColor Green
}

if (Test-Path $SdkConfig) {
    # Patch existing sdkconfig
    $lines = Get-Content $SdkConfig
    $patched = $false
    $newLines = @()
    foreach ($line in $lines) {
        if ($line -match '^CONFIG_PERF_MONITOR_ENABLED=' -or $line -match '^# CONFIG_PERF_MONITOR_ENABLED is not set') {
            if ($Perf) {
                $newLines += 'CONFIG_PERF_MONITOR_ENABLED=y'
            } else {
                $newLines += '# CONFIG_PERF_MONITOR_ENABLED is not set'
            }
            $patched = $true
        } elseif ($line -match '^CONFIG_PERF_REPORT_INTERVAL_S=') {
            if ($Perf) {
                $newLines += $line
            }
            # Drop the line for release builds (dependent option, Kconfig handles it)
        } else {
            $newLines += $line
        }
    }
    if (-not $patched -and $Perf) {
        $newLines += 'CONFIG_PERF_MONITOR_ENABLED=y'
        $newLines += 'CONFIG_PERF_REPORT_INTERVAL_S=30'
    }
    Set-Content $SdkConfig $newLines
} elseif ($Perf) {
    # No sdkconfig yet — create minimal one so idf.py build picks up perf settings
    Write-Host "Creating sdkconfig with perf monitoring enabled ..." -ForegroundColor Yellow
    Set-Content $SdkConfig @(
        'CONFIG_PERF_MONITOR_ENABLED=y'
        'CONFIG_PERF_REPORT_INTERVAL_S=30'
    )
}
# If no sdkconfig and release mode: idf.py build creates it from Kconfig defaults (perf=n)

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

# Record build mode for next run
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}
Set-Content $BuildModeFile $BuildMode

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

# Output filenames differ by build mode
$Suffix = if ($Perf) { "-perf" } else { "" }
$FactoryBin = Join-Path $FirmwareDir "nina-display${Suffix}-factory.bin"
$OtaBin     = Join-Path $FirmwareDir "nina-display${Suffix}-ota.bin"

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

Write-Host "`nFirmware generated successfully! [$BuildMode]" -ForegroundColor Green
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
            -TimeoutSec 120
        Write-Host "OTA update successful! Device is rebooting." -ForegroundColor Green
    } catch {
        Write-Error "OTA upload failed: $_"
        exit 1
    }
}
