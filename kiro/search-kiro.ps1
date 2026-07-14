Write-Host "=== Searching for Kiro installations ===" -ForegroundColor Cyan

# Search common locations
$locations = @(
    "C:\Program Files",
    "C:\Program Files (x86)",
    $env:APPDATA,
    $env:LOCALAPPDATA,
    $env:USERPROFILE
)

Write-Host "`n=== File System ===" -ForegroundColor Yellow
foreach ($location in $locations) {
    if (Test-Path $location) {
        Write-Host "Searching: $location" -ForegroundColor DarkGray
        try {
            Get-ChildItem -Path $location -Filter "*kiro*" -Recurse -ErrorAction SilentlyContinue | 
            Select-Object -First 30 FullName
        } catch {
            # Ignore errors
        }
    }
}

Write-Host "`n=== Registry ===" -ForegroundColor Yellow
# Search registry for Kiro-related keys
$registryPaths = @(
    "HKCU:\ software",
    "HKLM:\Software",
    "HKCU:\Software\Classes",
    "HKLM:\Software\Classes"
)

foreach ($regPath in $registryPaths) {
    if (Test-Path $regPath) {
        try {
            Get-ChildItem -Path $regPath -Recurse -ErrorAction SilentlyContinue | 
            Where-Object { $_.Name -like "*kiro*" } | 
            Select-Object -First 20 Name
        } catch {
            # Ignore errors
        }
    }
}

Write-Host "`n=== Environment Variables ===" -ForegroundColor Yellow
Get-ChildItem Env: | Where-Object { $_.Name -like "*kiro*" -or $_.Value -like "*kiro*" }

Write-Host "`n=== Search Complete ===" -ForegroundColor Green
