@echo off
setlocal
cd /d "%~dp0"

echo ================================================
echo    KIRO NUCLEAR RESET - LAUNCHER
echo ================================================
echo.

REM Check if the PowerShell script exists
if not exist "Kiro-Nuclear-Reset.ps1" (
    echo [ERROR] Kiro-Nuclear-Reset.ps1 not found in this directory!
    echo Please make sure both files are in the same folder.
    pause
    exit /b
)

echo Launching Nuclear Reset...
powershell -NoProfile -ExecutionPolicy Bypass -File "Kiro-Nuclear-Reset.ps1"

echo.
echo Operation finished.
pause
