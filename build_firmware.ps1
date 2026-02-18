# build_firmware.ps1
# Compiles the ESP32-P4 NINA Display project with ESP-IDF 5.5.2
# and generates a factory firmware binary in the firmware/ folder.

param(
    [string]$IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.2",
    [switch]$FullClean
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
$FirmwareDir = Join-Path $ProjectDir "firmware"

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
$BuildDir = Join-Path $ProjectDir "build"
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

$OutputBin = Join-Path $FirmwareDir "nina-display-factory.bin"

# Merge into a single factory binary using esptool
# ESP32-P4 layout: bootloader@0x2000, partition-table@0x8000, app@0x10000
Write-Host "`nMerging into factory firmware binary ..." -ForegroundColor Cyan
python -m esptool --chip esp32p4 merge_bin `
    --flash_mode dio `
    --flash_size 32MB `
    --flash_freq 80m `
    -o $OutputBin `
    0x2000  $Bootloader `
    0x8000  $PartTable `
    0x10000 $AppBin

if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to merge firmware binary."
    exit 1
}

$Size = (Get-Item $OutputBin).Length
$SizeMB = [math]::Round($Size / 1MB, 2)
Write-Host "`nFactory firmware generated successfully!" -ForegroundColor Green
Write-Host "  Output: $OutputBin" -ForegroundColor Green
Write-Host "  Size:   $SizeMB MB ($Size bytes)" -ForegroundColor Green
Write-Host "`nFlash with: esptool.py --chip esp32p4 write_flash 0x0 `"$OutputBin`"" -ForegroundColor Yellow
