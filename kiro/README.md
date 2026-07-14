# Kiro Ring 0

A Windows kernel driver and user-mode console application for system-level operations.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 BuildTools with MSVC
- Windows Driver Kit (WDK) 10
- Administrator privileges for driver operations

## Project Structure

```
kiro/
├── src/
│   ├── console/      # User-mode application
│   └── driver/       # Kernel-mode driver
├── build.ps1         # Unified build script
├── driver.ps1        # Driver management script
└── Run-Kiro-Reset.bat # Launcher for nuclear reset
```

## Building

Build both components:
```powershell
.\build.ps1
```

Build only the console application:
```powershell
.\build.ps1 console
```

Build only the driver:
```powershell
.\build.ps1 driver
```

## Driver Management

Install and start the driver:
```powershell
.\driver.ps1 install
```

Stop the driver:
```powershell
.\driver.ps1 stop
```

Remove the driver:
```powershell
.\driver.ps1 remove
```

Check driver status:
```powershell
.\driver.ps1 status
```

## Running the Nuclear Reset

```batch
Run-Kiro-Reset.bat
```

## Output Locations

- Console: `src\console\Release\KiroConsole.exe`
- Driver: `src\driver\KiroDriver.sys`

## Configuration

Edit the version numbers in `build.ps1` if your toolchain versions differ:
- `$MSVC_VER` - MSVC version
- `$SDK_VER` - Windows SDK version
- `$WDK_VER` - WDK version
