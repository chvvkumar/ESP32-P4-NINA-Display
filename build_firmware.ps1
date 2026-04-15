# build_firmware.ps1
# Compiles the ESP32-P4 NINA Display project with ESP-IDF 5.5.2
# and generates factory + OTA firmware binaries in the firmware/ folder.
#
# Usage:
#   .\build_firmware.ps1              # Normal build
#   .\build_firmware.ps1 -FullClean   # Clean rebuild
#   .\build_firmware.ps1 -OTA         # Build + OTA flash to both devices
#   .\build_firmware.ps1 -OTA -Devices "NinaDash1.lan","NinaDash2.lan"

[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingPlainTextForPassword', 'Password',
    Justification='Dev tool: password sourced from env var or interactive prompt; not persisted.')]
param(
    [string]$IdfPath = "C:\Espressif\frameworks\esp-idf-v5.5.2",
    [switch]$FullClean,
    [switch]$OTA,
    [string[]]$Devices = @("NinaDash2.lan"),
    [string]$Password = $(if ($env:NINADASH_PASSWORD) { $env:NINADASH_PASSWORD } else { "changeme123!" })
)

$ErrorActionPreference = "Stop"

# Interactive menu when invoked without arguments
if ($PSBoundParameters.Count -eq 0) {
    function Show-Menu {
        param($IdfPath, $FullClean, $OTA, $Devices)
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
        Write-Host "  [3] Devices:     " -NoNewline; Write-Host ($Devices -join ", ") -ForegroundColor White
        Write-Host "  [4] IDF Path:    " -NoNewline; Write-Host $IdfPath -ForegroundColor White
        Write-Host ""
    }

    $OTA = $true  # Default to OTA enabled in interactive mode
    $menuLoop = $true
    while ($menuLoop) {
        Show-Menu -IdfPath $IdfPath -FullClean $FullClean -OTA $OTA -Devices $Devices
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
                    $newDevices = Read-Host "  Devices (comma-separated) [$($Devices -join ', ')]"
                    if (-not [string]::IsNullOrWhiteSpace($newDevices)) { $Devices = ($newDevices -split '[,\s]+') | ForEach-Object { $_.Trim() } | Where-Object { $_ } }
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
    if ($OTA) { $summary += "OTA -> $($Devices -join ', ')" }
    Write-Host ""
    Write-Host "  Building: $($summary -join ' + ')" -ForegroundColor Cyan
    Write-Host ""
}

$ProjectDir = $PSScriptRoot
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

if (-not $AppBin -or -not (Test-Path $AppBin)) {
    Write-Error "Missing build artifact: $AppBin"
    exit 1
}

# The app binary is used directly for OTA; no separate firmware/ copy needed
$OtaSize = (Get-Item $AppBin).Length
$OtaSizeMB = [math]::Round($OtaSize / 1MB, 2)

Write-Host "`nBuild successful!" -ForegroundColor Green
Write-Host "  App binary: $AppBin" -ForegroundColor Green
Write-Host "              $OtaSizeMB MB ($OtaSize bytes)" -ForegroundColor Green

# OTA flash to devices if requested (parallel)
if ($OTA) {
    # Device auth: /api/ota requires a valid session cookie.
    if ([string]::IsNullOrEmpty($Password)) {
        $sec = Read-Host -Prompt "Admin password for $($Devices -join ', ')" -AsSecureString
        $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
        try { $Password = [System.Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
        finally { [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
    }

    Write-Host "`nUploading OTA firmware to $($Devices.Count) devices in parallel ..." -ForegroundColor Cyan

    [array]$jobs = foreach ($Device in $Devices) {
        Start-Job -ArgumentList $Device, $AppBin, $Password -ScriptBlock {
            param($Device, $AppBin, $Password)

            # 1. Login to get session cookie
            $LoginUrl = "http://${Device}/api/login"
            $loginBody = @{ password = $Password } | ConvertTo-Json -Compress
            $loginResp = Invoke-WebRequest -Uri $LoginUrl -Method Post `
                -Body $loginBody -ContentType "application/json" -TimeoutSec 15 `
                -UseBasicParsing
            if ($loginResp.StatusCode -ne 200) {
                throw "login failed: HTTP $($loginResp.StatusCode)"
            }
            $setCookie = $loginResp.Headers['Set-Cookie']
            if ($setCookie -is [array]) { $setCookie = $setCookie[0] }
            if (-not $setCookie -or $setCookie -notmatch 'session=([^;]+)') {
                throw "login succeeded but no session cookie returned"
            }
            $sessionCookie = "session=$($Matches[1])"

            # 2. Upload firmware with session cookie
            $OtaUrl = "http://${Device}/api/ota"
            $fileBytes = [System.IO.File]::ReadAllBytes($AppBin)
            $otaResp = Invoke-WebRequest -Uri $OtaUrl -Method Post `
                -Body $fileBytes `
                -ContentType "application/octet-stream" `
                -Headers @{ Cookie = $sessionCookie } `
                -TimeoutSec 600 -UseBasicParsing
            if ($otaResp.StatusCode -lt 200 -or $otaResp.StatusCode -ge 300) {
                throw "ota upload failed: HTTP $($otaResp.StatusCode)"
            }
        }
    }

    $failed = @()
    foreach ($job in $jobs) {
        $device = $Devices[$jobs.IndexOf($job)]
        Write-Host "  Waiting for $device ..." -ForegroundColor Gray -NoNewline
        $null = $job | Wait-Job
        if ($job.State -eq 'Completed') {
            Write-Host " OK" -ForegroundColor Green
        } else {
            Write-Host " FAILED" -ForegroundColor Red
            $job | Receive-Job -ErrorAction SilentlyContinue | Out-Null
            Write-Host "    $($job.ChildJobs[0].JobStateInfo.Reason)" -ForegroundColor Red
            $failed += $device
        }
        $job | Remove-Job
    }

    if ($failed.Count -gt 0) {
        Write-Error "OTA failed for: $($failed -join ', ')"
        exit 1
    }
    Write-Host "All devices updated successfully!" -ForegroundColor Green
}
