# Kiro Nuclear Reset Script
# This script resets all known IDs and clears all cache/telemetry for a "super fresh" Kiro install.

$ErrorActionPreference = "SilentlyContinue"

Write-Host "================================================" -ForegroundColor Cyan
Write-Host "          KIRO NUCLEAR RESET INITIATED          " -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan

# 1. Close Kiro
Write-Host "`n[1/5] Closing Kiro processes..." -ForegroundColor Yellow
Get-Process Kiro, kiro-agent | Stop-Process -Force 2>$null
Start-Sleep -Seconds 2

# 2. Define Paths
$kiroRoaming = "$env:APPDATA\Kiro"
$kiroLocal = "$env:LOCALAPPDATA\Programs\Kiro"
$kiroHome = "$env:USERPROFILE\.kiro"
$storageJson = "$kiroRoaming\User\globalStorage\storage.json"
$argvJson = "$kiroHome\argv.json"

# 3. Generate New IDs
Write-Host "[2/5] Generating fresh IDs..." -ForegroundColor Yellow
$newMachineId = [guid]::NewGuid().ToString("n") + [guid]::NewGuid().ToString("n") # 64 chars
$newDevDeviceId = [guid]::NewGuid().ToString()
$newSqmId = "{" + [guid]::NewGuid().ToString().ToUpper() + "}"
$newCrashId = [guid]::NewGuid().ToString()

# 4. Apply New IDs to Config Files
Write-Host "[3/5] Applying new IDs to config files..." -ForegroundColor Yellow

# Update storage.json
if (Test-Path $storageJson) {
    try {
        $data = Get-Content $storageJson | ConvertFrom-Json
        $data."telemetry.machineId" = $newMachineId
        $data."telemetry.devDeviceId" = $newDevDeviceId
        $data."telemetry.sqmId" = $newSqmId
        $data | ConvertTo-Json -Depth 10 | Set-Content $storageJson
        Write-Host "  - Updated storage.json" -ForegroundColor Green
    } catch {
        Write-Host "  ! Error updating storage.json" -ForegroundColor Red
    }
}

# Update argv.json
if (Test-Path $argvJson) {
    try {
        $content = Get-Content $argvJson -Raw
        $newContent = $content -replace '"crash-reporter-id": ".*?"', ('"crash-reporter-id": "' + $newCrashId + '"')
        $newContent | Set-Content $argvJson
        Write-Host "  - Updated argv.json" -ForegroundColor Green
    } catch {
        Write-Host "  ! Error updating argv.json" -ForegroundColor Red
    }
}

# 5. Nuclear Cache & Telemetry Purge
Write-Host "[4/5] Performing nuclear cache purge..." -ForegroundColor Yellow
$foldersToPurge = @(
    "Cache", "Code Cache", "GPUCache", "CachedData", 
    "DawnGraphiteCache", "DawnWebGPUCache", "Network", 
    "logs", "Crashpad", "Local Storage", "Session Storage",
    "Service Worker", "Shared Dictionary",
    "User\History", "User\workspaceStorage", "User\globalStorage\kiro.kiroagent"
)

foreach ($folder in $foldersToPurge) {
    $path = Join-Path $kiroRoaming $folder
    if (Test-Path $path) {
        Write-Host "  - Purging $folder..."
        Remove-Item -Path $path -Recurse -Force -ErrorAction SilentlyContinue
        # Recreate essential ones empty if needed
        if ($folder -in @("Cache", "logs", "Network")) {
            New-Item -ItemType Directory -Path $path -Force | Out-Null
        }
    }
}

# Clear any other .log or .bak files in the root Roaming folder
Get-ChildItem -Path $kiroRoaming -Filter "*.log" | Remove-Item -Force
Get-ChildItem -Path $kiroRoaming -Filter "*.bak" | Remove-Item -Force

# 6. Registry Check (Classes/MuiCache)
Write-Host "[5/5] Cleaning registry traces..." -ForegroundColor Yellow
# Removing MuiCache entries often resets app state in Windows eyes
$muiCachePath = "HKCU:\Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache"
Get-ItemProperty -Path $muiCachePath | Get-Member -MemberType NoteProperty | Where-Object { $_.Name -like "*Kiro.exe*" } | ForEach-Object {
    Remove-ItemProperty -Path $muiCachePath -Name $_.Name
    Write-Host "  - Removed Registry MuiCache: $($_.Name)"
}

Write-Host "`n================================================" -ForegroundColor Cyan
Write-Host "       RESET COMPLETE - KIRO IS NOW FRESH       " -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "New Machine ID: $newMachineId"
Write-Host "New Device ID:  $newDevDeviceId"
Write-Host "`nYou can now restart Kiro."
