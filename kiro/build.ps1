$ErrorActionPreference = "Stop"

# Configuration
$MSVC_VER = "14.44.35207"
$SDK_VER  = "10.0.26100.0"
$WDK_VER  = "10.0.28000.0"

$MSVC_ROOT = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\$MSVC_VER"
$SDK_ROOT  = "C:\Program Files (x86)\Windows Kits\10"

$MSVC_BIN = Join-Path $MSVC_ROOT "bin\HostX64\x64"
$MSVC_INC = Join-Path $MSVC_ROOT "include"
$MSVC_LIB = Join-Path $MSVC_ROOT "lib\x64"

$clPath   = Join-Path $MSVC_BIN "cl.exe"
$linkPath = Join-Path $MSVC_BIN "link.exe"

function Build-Console {
    Write-Host "=== Building KiroConsole.exe (usermode) ===" -ForegroundColor Cyan

    $sdkInc  = Join-Path $SDK_ROOT "Include\$SDK_VER\ucrt"
    $sdkLib  = Join-Path $SDK_ROOT "Lib\$SDK_VER\ucrt\x64"
    $umInc   = Join-Path $SDK_ROOT "Include\$SDK_VER\um"
    $sharedInc = Join-Path $SDK_ROOT "Include\$SDK_VER\shared"
    $winrtInc = Join-Path $SDK_ROOT "Include\$SDK_VER\winrt"
    $umLib   = Join-Path $SDK_ROOT "Lib\$SDK_VER\um\x64"

    New-Item -ItemType Directory -Force -Path "src\console\Release" | Out-Null

    $srcPath = "src\console\main.cpp"
    $objPath = "src\console\Release\main.obj"
    $outPath = "src\console\Release\KiroConsole.exe"

    $clArgs = @(
        "/nologo", "/EHsc", "/O2", "/W3",
        "/D", "_CRT_SECURE_NO_WARNINGS", "/D", "WIN32", "/D", "_WINDOWS",
        "/I", "`"$sdkInc`"", "/I", "`"$MSVC_INC`"", "/I", "src\driver",
        "/I", "`"$umInc`"", "/I", "`"$sharedInc`"", "/I", "`"$winrtInc`"",
        "/c", "`"$srcPath`"", "/Fo`"$objPath`""
    )

    Write-Host "Compiling..." -ForegroundColor DarkGray
    & $clPath $clArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "cl.exe failed."; exit 1 }

    $linkArgs = @(
        "/nologo", "/SUBSYSTEM:CONSOLE", "/MACHINE:X64",
        "/LIBPATH:`"$sdkLib`"", "/LIBPATH:`"$MSVC_LIB`"", "/LIBPATH:`"$umLib`"",
        "/OUT:`"$outPath`"", "`"$objPath`"",
        "kernel32.lib", "user32.lib", "advapi32.lib", "rpcrt4.lib"
    )

    Write-Host "Linking..." -ForegroundColor DarkGray
    & $linkPath $linkArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "link.exe failed."; exit 1 }

    if (Test-Path $outPath) {
        $size = (Get-Item $outPath).Length
        Write-Host "Build Successful: $outPath ($size bytes)" -ForegroundColor Green
    } else {
        Write-Error "exe not produced."
        exit 1
    }
}

function Build-Driver {
    Write-Host "=== Building KiroDriver.sys (kernel mode) ===" -ForegroundColor Cyan

    $WDK_INC  = Join-Path $SDK_ROOT "Include"
    $WDK_LIB  = Join-Path $SDK_ROOT "Lib\$WDK_VER\km\x64"

    $objPath = "src\driver\main.obj"
    $srcPath = "src\driver\main.c"
    $sysPath = "src\driver\KiroDriver.sys"

    $clArgs = @(
        "/nologo", "/W3", "/Z7", "/O2", "/Oi",
        "/D", "_AMD64_", "/D", "AMD64", "/D", "_WIN64",
        "/D", "_KERNEL_MODE",
        "/I", "`"$WDK_INC\$WDK_VER\km`"",
        "/I", "`"$WDK_INC\$SDK_VER\shared`"",
        "/I", "`"$WDK_INC\$SDK_VER\ucrt`"",
        "/I", "`"$MSVC_INC`"", "/I", "src\driver",
        "/c", "`"$srcPath`"", "/Fo`"$objPath`""
    )

    Write-Host "Compiling..." -ForegroundColor DarkGray
    & $clPath $clArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "cl.exe failed."; exit 1 }
    if (-not (Test-Path $objPath)) { Write-Error "obj not produced."; exit 1 }

    $linkArgs = @(
        "/nologo", "/SUBSYSTEM:NATIVE", "/DRIVER",
        "/ENTRY:DriverEntry", "/RELEASE", "/NODEFAULTLIB",
        "/LIBPATH:`"$WDK_LIB`"", "/LIBPATH:`"$MSVC_LIB`"",
        "ntoskrnl.lib", "ntstrsafe.lib", "BufferOverflowK.lib",
        "libcntpr.lib", "wdm.lib", "`"$objPath`"", "/OUT:$sysPath"
    )

    Write-Host "Linking..." -ForegroundColor DarkGray
    & $linkPath $linkArgs
    if ($LASTEXITCODE -ne 0) { Write-Error "link.exe failed."; exit 1 }

    if (Test-Path $sysPath) {
        $size = (Get-Item $sysPath).Length
        Write-Host "Build Successful: $sysPath ($size bytes)" -ForegroundColor Green
    } else {
        Write-Error "Linking failed - sys not produced."
        exit 1
    }
}

# Main
$target = $args[0]

if (-not $target -or $target -eq "all") {
    Build-Console
    Write-Host ""
    Build-Driver
} elseif ($target -eq "console") {
    Build-Console
} elseif ($target -eq "driver") {
    Build-Driver
} else {
    Write-Host "Usage: .\build.ps1 [console|driver|all]" -ForegroundColor Yellow
    exit 1
}
