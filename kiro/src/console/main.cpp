// Kiro Ring 0 Console - usermode controller for KiroDriver.sys
// STRICT: device must be opened via the kernel driver, no fallback path.
#include <windows.h>
#include <iostream>
#include <string>
#include <random>
#include <exception>
#include <vector>
#include "../driver/common.h"

static std::string GenGuid() {
    UUID u;
    UuidCreate(&u);
    char buf[64] = {};
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        u.Data1, u.Data2, u.Data3,
        u.Data4[0], u.Data4[1], u.Data4[2], u.Data4[3],
        u.Data4[4], u.Data4[5], u.Data4[6], u.Data4[7]);
    return buf;
}

static std::string GenMachineId64() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 64; ++i) out.push_back(hex[gen() & 0xF]);
    return out;
}

static std::vector<std::wstring> GetKiroCachePaths() {
    std::vector<std::wstring> paths;
    wchar_t* appdataW = nullptr;
    size_t len = 0;
    errno_t err;

    // Get APPDATA using wide character version for safety
    err = _wdupenv_s(&appdataW, &len, L"APPDATA");
    if (err == 0 && appdataW != nullptr) {
        std::wstring roaming(appdataW);
        free(appdataW);

        // Main Kiro data directories
        std::vector<std::wstring> folders = {
            L"Cache", L"Code Cache", L"GPUCache", L"CachedData",
            L"DawnGraphiteCache", L"DawnWebGPUCache", L"Network",
            L"logs", L"Crashpad", L"Local Storage", L"Session Storage",
            L"Service Worker", L"Shared Dictionary"
        };

        for (const auto& folder : folders) {
            std::wstring fullPath = roaming + L"\\Kiro\\" + folder;
            // Validate path length before adding
            if (fullPath.length() < MAX_PATH - 1) {
                paths.push_back(fullPath);
            }
        }

        // Add workspace and global storage
        std::wstring basePath = roaming + L"\\Kiro\\User";
        if (basePath.length() < MAX_PATH - 20) {
            paths.push_back(basePath + L"\\History");
            paths.push_back(basePath + L"\\workspaceStorage");
            paths.push_back(basePath + L"\\globalStorage\\kiro.kiroagent");
        }
        
        // Add entire Kiro directory (validate first)
        std::wstring kiroDir = roaming + L"\\Kiro";
        if (kiroDir.length() < MAX_PATH - 1) {
            paths.push_back(kiroDir);
        }

        // Windows Recent files
        std::wstring recentDir = roaming + L"\\Microsoft\\Windows\\Recent";
        if (recentDir.length() < MAX_PATH - 1) {
            paths.push_back(recentDir);
        }
        
        // Start Menu Programs
        std::wstring programsDir = roaming + L"\\Microsoft\\Windows\\Start Menu\\Programs\\Kiro";
        if (programsDir.length() < MAX_PATH - 1) {
            paths.push_back(programsDir);
        }
    }

    // Get LOCALAPPDATA
    err = _wdupenv_s(&appdataW, &len, L"LOCALAPPDATA");
    if (err == 0 && appdataW != nullptr) {
        std::wstring local(appdataW);
        free(appdataW);
        std::wstring localPath = local + L"\\Programs\\Kiro";
        if (localPath.length() < MAX_PATH - 1) {
            paths.push_back(localPath);
        }
    }

    // Get USERPROFILE
    err = _wdupenv_s(&appdataW, &len, L"USERPROFILE");
    if (err == 0 && appdataW != nullptr) {
        std::wstring user(appdataW);
        free(appdataW);
        
        std::wstring kiroDot = user + L"\\.kiro";
        if (kiroDot.length() < MAX_PATH - 1) {
            paths.push_back(kiroDot);
        }
        
        // AWS SSO cache
        std::wstring awsSso = user + L"\\.aws\\sso\\cache";
        if (awsSso.length() < MAX_PATH - 1) {
            paths.push_back(awsSso);
        }
        
        // Trae data
        std::wstring traeDir = user + L"\\.trae";
        if (traeDir.length() < MAX_PATH - 1) {
            paths.push_back(traeDir);
        }
    }

    return paths;
}

static std::string SendIoctl(
    HANDLE  hDevice,
    DWORD   code,
    PVOID   inBuf,  DWORD inLen,
    PVOID   outBuf, DWORD outLen
) {
    DWORD bytesReturned = 0;
    std::cerr << "[console] Sending IOCTL code=0x" << std::hex << code << std::dec 
              << " inLen=" << inLen << " outLen=" << outLen << std::endl;
    std::cerr << "[console] Expected codes: RESET=0x" << std::hex << IOCTL_KIRO_RESET_IDS 
              << " CACHE=0x" << IOCTL_KIRO_CLEAR_CACHE << " WRITE=0x" << IOCTL_KIRO_WRITE_REGISTRY
              << " DEL=0x" << IOCTL_KIRO_DELETE_REGISTRY << " CLEAN=0x" << IOCTL_KIRO_CLEAN_REGISTRY_PATTERN 
              << std::dec << std::endl;
    if (!DeviceIoControl(hDevice, code, inBuf, inLen, outBuf, outLen, &bytesReturned, NULL)) {
        // Driver unloaded, handle went bad, or IOCTL rejected.
        std::cerr << "[console] DeviceIoControl failed (Win32=" << GetLastError() << ")" << std::endl;
        return "";
    }
    if (outBuf == NULL || bytesReturned == 0) return std::string();
    return std::string((const char*)outBuf, (size_t)bytesReturned < (size_t)outLen ? bytesReturned : outLen);
}

int main() {
    try {
        // HARD REQUIREMENT: open the kernel device FIRST. No banner, no menu, no fallback.
        HANDLE hDevice = CreateFileW(
            L"\\\\.\\KiroReset",
            GENERIC_READ | GENERIC_WRITE,
            0,          // no sharing
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        if (hDevice == INVALID_HANDLE_VALUE) {
            // Pure kernel-only: if the driver is not loaded, the program does not run.
            std::cerr << "[console] kernel driver not loaded (Win32=" << GetLastError() << ")" << std::endl;
            std::cerr << "[console] Please run Start-Driver.bat first" << std::endl;
            system("pause");
            return 1;
        }

        // From here on, every operation goes through the driver. No usermode shortcut.
        std::cout << "KIRO RING 0 (usermode console)" << std::endl;
        std::cout << "device=\\\\.\\KiroReset  handle=0x" << std::hex << (uintptr_t)hDevice << std::dec << std::endl;
        std::cout << std::endl;

        for (;;) {
            std::cout << "[1] reset IDs (kernel registry)" << std::endl
                      << "[2] clear cache directory" << std::endl
                      << "[3] write registry value" << std::endl
                      << "[4] nuclear reset (all)" << std::endl
                      << "[0] exit"     << std::endl
                      << "> ";

            int choice = -1;
            if (!(std::cin >> choice)) {
                // EOF / bad input - keep the device open, just re-prompt
                std::cin.clear();
                continue;
            }
            if (choice == 0) break;

            if (choice == 1) {
                // Only the kernel can persist new IDs. The console only generates candidates.
                std::string machineId = GenMachineId64();
                std::string devId     = GenGuid();
                std::string sqmId     = "{" + GenGuid() + "}";
                std::string crashId   = GenGuid();

                std::cout << "  candidate machineId  = " << machineId << std::endl;
                std::cout << "  candidate devDeviceId= " << devId     << std::endl;
                std::cout << "  candidate sqmId      = " << sqmId     << std::endl;
                std::cout << "  candidate crashId    = " << crashId   << std::endl;

                KIRO_RESET_INPUT in{};
                strncpy_s(in.NewMachineGuid, machineId.c_str(), sizeof(in.NewMachineGuid) - 1);
                strncpy_s(in.NewDevDeviceGuid, devId.c_str(), sizeof(in.NewDevDeviceGuid) - 1);
                strncpy_s(in.NewSqmId, sqmId.c_str(), sizeof(in.NewSqmId) - 1);
                strncpy_s(in.NewCrashId, crashId.c_str(), sizeof(in.NewCrashId) - 1);

                char outBuf[KIRO_OUTPUT_MAX] = {};
                std::string resp = SendIoctl(hDevice, IOCTL_KIRO_RESET_IDS,
                                             &in, sizeof(in), outBuf, sizeof(outBuf));
                if (resp.empty()) {
                    std::cerr << "[console] Failed to reset IDs" << std::endl;
                } else {
                    std::cout << "[kernel] " << resp << std::endl;
                }
            }
            else if (choice == 2) {
                std::cout << "path> ";
                std::wstring path;
                // Use getline instead of >> to handle paths with spaces safely
                std::getline(std::wcin, path);
                
                // Validate path length before using
                if (path.empty() || path.length() >= KIRO_MAX_PATH - 1) {
                    std::cerr << "[console] Invalid path (empty or too long)" << std::endl;
                    continue;
                }

                KIRO_CACHE_INPUT in{};
                wcsncpy_s(in.BasePath, KIRO_MAX_PATH, path.c_str(), _TRUNCATE);

                char outBuf[KIRO_OUTPUT_MAX] = {};
                std::string resp = SendIoctl(hDevice, IOCTL_KIRO_CLEAR_CACHE,
                                             &in, sizeof(in), outBuf, sizeof(outBuf));
                if (resp.empty()) {
                    std::cerr << "[console] Failed to clear cache" << std::endl;
                } else {
                    std::cout << "[kernel] " << resp << std::endl;
                }
            }
            else if (choice == 3) {
                std::cout << "keyPath> ";
                std::wstring keyPath;
                std::getline(std::wcin, keyPath);
                std::cout << "valueName> ";
                std::wstring valueName;
                std::getline(std::wcin, valueName);
                std::cout << "valueData> ";
                std::wstring valueData;
                std::getline(std::wcin, valueData);

                // Validate input lengths
                if (keyPath.empty() || keyPath.length() >= KIRO_MAX_PATH - 1 ||
                    valueName.length() >= KIRO_MAX_PATH - 1 ||
                    valueData.length() >= KIRO_MAX_PATH - 1) {
                    std::cerr << "[console] Invalid input (empty or too long)" << std::endl;
                    continue;
                }

                KIRO_REGISTRY_INPUT in{};
                wcsncpy_s(in.KeyPath, KIRO_MAX_PATH, keyPath.c_str(), _TRUNCATE);
                wcsncpy_s(in.ValueName, KIRO_MAX_PATH, valueName.c_str(), _TRUNCATE);
                wcsncpy_s(in.ValueData, KIRO_MAX_PATH, valueData.c_str(), _TRUNCATE);

                char outBuf[KIRO_OUTPUT_MAX] = {};
                std::string resp = SendIoctl(hDevice, IOCTL_KIRO_WRITE_REGISTRY,
                                             &in, sizeof(in), outBuf, sizeof(outBuf));
                if (resp.empty()) {
                    std::cerr << "[console] Failed to write registry" << std::endl;
                } else {
                    std::cout << "[kernel] " << resp << std::endl;
                }
            }
            else if (choice == 4) {
                // Nuclear reset - do everything automatically
                std::cout << "[nuclear] Performing full automatic reset..." << std::endl;

                // 1. Reset IDs
                std::string machineId = GenMachineId64();
                std::string devId     = GenGuid();
                std::string sqmId     = "{" + GenGuid() + "}";
                std::string crashId   = GenGuid();

                std::cout << "[nuclear] Resetting IDs..." << std::endl;
                KIRO_RESET_INPUT resetIn{};
                strncpy_s(resetIn.NewMachineGuid, machineId.c_str(), sizeof(resetIn.NewMachineGuid) - 1);
                strncpy_s(resetIn.NewDevDeviceGuid, devId.c_str(), sizeof(resetIn.NewDevDeviceGuid) - 1);
                strncpy_s(resetIn.NewSqmId, sqmId.c_str(), sizeof(resetIn.NewSqmId) - 1);
                strncpy_s(resetIn.NewCrashId, crashId.c_str(), sizeof(resetIn.NewCrashId) - 1);

                char outBuf[KIRO_OUTPUT_MAX] = {};
                std::string resp = SendIoctl(hDevice, IOCTL_KIRO_RESET_IDS,
                                             &resetIn, sizeof(resetIn), outBuf, sizeof(outBuf));
                if (resp.empty()) {
                    std::cerr << "[console] Failed to reset IDs" << std::endl;
                } else {
                    std::cout << "[kernel] " << resp << std::endl;
                }

                // 2. Clear cache directories automatically
                std::cout << "[nuclear] Clearing cache directories..." << std::endl;
                std::vector<std::wstring> cachePaths = GetKiroCachePaths();
                int clearedCount = 0;
                for (const auto& cachePath : cachePaths) {
                    // Skip paths that are too long
                    if (cachePath.length() >= KIRO_MAX_PATH - 1) {
                        std::cerr << "[nuclear] Skipping path too long: " << cachePath.length() << " chars" << std::endl;
                        continue;
                    }
                    
                    KIRO_CACHE_INPUT cacheIn{};
                    wcsncpy_s(cacheIn.BasePath, KIRO_MAX_PATH, cachePath.c_str(), _TRUNCATE);

                    resp = SendIoctl(hDevice, IOCTL_KIRO_CLEAR_CACHE,
                                    &cacheIn, sizeof(cacheIn), outBuf, sizeof(outBuf));
                    if (!resp.empty()) {
                        std::cout << "[kernel] " << resp << std::endl;
                        clearedCount++;
                    }
                }
                std::cout << "[nuclear] Cleared " << clearedCount << " directories." << std::endl;

                // 3. Clean registry traces
                std::cout << "[nuclear] Cleaning registry traces..." << std::endl;
                
                // Clean MuiCache for Kiro entries
                KIRO_REGISTRY_INPUT regIn{};
                wcsncpy_s(regIn.KeyPath, L"\\Registry\\User\\CurrentUser\\Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", KIRO_MAX_PATH - 1);
                wcsncpy_s(regIn.ValueName, L"kiro", KIRO_MAX_PATH - 1);
                
                resp = SendIoctl(hDevice, IOCTL_KIRO_CLEAN_REGISTRY_PATTERN,
                                &regIn, sizeof(regIn), outBuf, sizeof(outBuf));
                if (!resp.empty()) {
                    std::cout << "[kernel] " << resp << std::endl;
                }

                // Clean HKLM\Software\Kiro
                wcsncpy_s(regIn.KeyPath, L"\\Registry\\Machine\\Software\\Kiro", KIRO_MAX_PATH - 1);
                wcsncpy_s(regIn.ValueName, L"", KIRO_MAX_PATH - 1);
                
                resp = SendIoctl(hDevice, IOCTL_KIRO_DELETE_REGISTRY,
                                &regIn, sizeof(regIn), outBuf, sizeof(outBuf));
                if (!resp.empty()) {
                    std::cout << "[kernel] " << resp << std::endl;
                }

                std::cout << "[nuclear] Automatic reset complete." << std::endl;
                std::cout << "[nuclear] New Machine ID: " << machineId << std::endl;
                std::cout << "[nuclear] New Device ID:  " << devId << std::endl;
            }
            else {
                std::cout << "?" << std::endl;
            }
            std::cout << std::endl;
        }

        CloseHandle(hDevice);
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "[console] Exception: " << e.what() << std::endl;
        system("pause");
        return 1;
    }
    catch (...) {
        std::cerr << "[console] Unknown exception occurred" << std::endl;
        system("pause");
        return 1;
    }
}
