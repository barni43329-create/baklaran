// KiroDriver - Ring 0 WDM driver for Kiro ID reset
// Pure WDM (no KMDF). Performs kernel-level ID reset and cache clearing
#include <ntddk.h>
#include <ntstrsafe.h>
#include "common.h"

// Define FILE_DIRECTORY_INFORMATION manually to avoid ntifs.h conflicts
typedef struct _FILE_DIRECTORY_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FILE_DIRECTORY_INFORMATION, *PFILE_DIRECTORY_INFORMATION;

// Declare ZwQueryDirectoryFile
NTSYSAPI
NTSTATUS
NTAPI
ZwQueryDirectoryFile(
    IN HANDLE FileHandle,
    IN HANDLE Event OPTIONAL,
    IN PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    IN PVOID ApcContext OPTIONAL,
    OUT PIO_STATUS_BLOCK IoStatusBlock,
    OUT PVOID FileInformation,
    IN ULONG Length,
    IN FILE_INFORMATION_CLASS FileInformationClass,
    IN BOOLEAN ReturnSingleEntry,
    IN PUNICODE_STRING FileName OPTIONAL,
    IN BOOLEAN RestartScan
);

#define DEVICE_NAME_USER    L"\\DosDevices\\KiroReset"
#define DEVICE_NAME_KERNEL  L"\\Device\\KiroReset"

static UNICODE_STRING  g_DeviceName;
static UNICODE_STRING  g_SymbolicLinkName;
static PDEVICE_OBJECT  g_DeviceObject = NULL;

VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS KiroCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);
NTSTATUS KiroDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp);

// ---- tiny self-contained formatter (no RtlStringCbPrintfA to avoid UCRT) ----
static VOID KiroAppendStr(_Inout_updates_z_(cap) PSTR dst, _In_ size_t cap, _In_z_ PCSTR src) {
    if (cap == 0) return;
    size_t cur = 0;
    while (cur + 1 < cap && dst[cur] != '\0') ++cur;
    while (cur + 1 < cap && *src) { dst[cur++] = *src++; }
    dst[cur] = '\0';
}

static VOID KiroAppendHex64(_Inout_updates_z_(cap) PSTR dst, _In_ size_t cap, _In_ ULONG64 v) {
    if (cap == 0) return;
    static const char hex[] = "0123456789abcdef";
    char tmp[17]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        while (v && n < 16) { tmp[n++] = hex[v & 0xF]; v >>= 4; }
    }
    // reverse
    char rev[17];
    for (int i = 0; i < n; ++i) rev[i] = tmp[n - 1 - i];
    rev[n] = '\0';
    KiroAppendStr(dst, cap, rev);
}

static VOID KiroAppendULong(_Inout_updates_z_(cap) PSTR dst, _In_ size_t cap, _In_ ULONG v) {
    if (cap == 0) return;
    char tmp[12]; int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    }
    char rev[12];
    for (int i = 0; i < n; ++i) rev[i] = tmp[n - 1 - i];
    rev[n] = '\0';
    KiroAppendStr(dst, cap, rev);
}

static VOID KiroWriteOutput(
    _Out_writes_bytes_(bufLen) PUCHAR  OutputBuffer,
    _In_  size_t bufLen,
    _In_z_ PCSTR  Text,
    _Out_ PULONG  BytesWritten
) {
    *BytesWritten = 0;
    if (OutputBuffer == NULL || bufLen == 0) {
        return;
    }
    RtlZeroMemory(OutputBuffer, bufLen);
    KiroAppendStr((PSTR)OutputBuffer, bufLen, Text);
    size_t copied = 0;
    RtlStringCbLengthA((PSTR)OutputBuffer, bufLen, &copied);
    *BytesWritten = (ULONG)copied;
}

// Registry helper - write string value to registry
static NTSTATUS KiroWriteRegistryValue(
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR ValueName,
    _In_ PCWSTR ValueData
) {
    HANDLE keyHandle = NULL;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING keyPath, valueName;
    NTSTATUS status;

    RtlInitUnicodeString(&keyPath, KeyPath);
    InitializeObjectAttributes(&objAttr, &keyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_ALL_ACCESS, &objAttr);
    if (!NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: Failed to open key %wZ (0x%08X)\n", &keyPath, status);
        return status;
    }

    RtlInitUnicodeString(&valueName, ValueName);
    
    ULONG dataSize = (ULONG)(wcslen(ValueData) + 1) * sizeof(WCHAR);
    status = ZwSetValueKey(keyHandle, &valueName, 0, REG_SZ, ValueData, dataSize);
    
    ZwClose(keyHandle);
    
    if (NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: Wrote registry %wZ\\%wZ\n", &keyPath, &valueName);
    }
    
    return status;
}

// Registry helper - delete value from registry
static NTSTATUS KiroDeleteRegistryValue(
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR ValueName
) {
    HANDLE keyHandle = NULL;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING keyPath, valueName;
    NTSTATUS status;

    RtlInitUnicodeString(&keyPath, KeyPath);
    InitializeObjectAttributes(&objAttr, &keyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_ALL_ACCESS, &objAttr);
    if (!NT_SUCCESS(status)) {
        // Key might not exist, that's okay
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&valueName, ValueName);
    status = ZwDeleteValueKey(keyHandle, &valueName);
    
    ZwClose(keyHandle);
    
    if (NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: Deleted registry value %wZ\\%wZ\n", &keyPath, &valueName);
    }
    
    return status;
}

// Registry helper - delete all values matching pattern from a key
static NTSTATUS KiroDeleteRegistryValuesByPattern(
    _In_ PCWSTR KeyPath,
    _In_ PCWSTR Pattern
) {
    HANDLE keyHandle = NULL;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING keyPath;
    NTSTATUS status;
    ULONG resultLength = 0;
    PKEY_VALUE_FULL_INFORMATION valueInfo = NULL;
    ULONG bufferSize = 4096;
    ULONG index = 0;

    RtlInitUnicodeString(&keyPath, KeyPath);
    InitializeObjectAttributes(&objAttr, &keyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenKey(&keyHandle, KEY_ALL_ACCESS, &objAttr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    valueInfo = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, bufferSize, 'Kiro');
    if (valueInfo == NULL) {
        ZwClose(keyHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (index = 0; ; index++) {
        status = ZwEnumerateValueKey(keyHandle, index, KeyValueFullInformation,
                                      valueInfo, bufferSize, &resultLength);
        
        if (status == STATUS_NO_MORE_ENTRIES) break;
        if (status == STATUS_BUFFER_OVERFLOW) {
            ExFreePoolWithTag(valueInfo, 'Kiro');
            bufferSize = resultLength + 512;
            valueInfo = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, bufferSize, 'Kiro');
            if (valueInfo == NULL) {
                ZwClose(keyHandle);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            continue;
        }
        if (!NT_SUCCESS(status)) break;

        // Check if value name matches pattern
        UNICODE_STRING valueName;
        valueName.Buffer = valueInfo->Name;
        valueName.Length = (USHORT)valueInfo->NameLength;
        valueName.MaximumLength = (USHORT)valueInfo->NameLength;

        if (wcsstr(valueInfo->Name, Pattern) != NULL) {
            // Delete this value
            ZwDeleteValueKey(keyHandle, &valueName);
            DbgPrint("KiroDriver: Deleted matching value %wZ\n", &valueName);
        }
    }

    ExFreePoolWithTag(valueInfo, 'Kiro');
    ZwClose(keyHandle);
    return STATUS_SUCCESS;
}

// File system helper - delete directory recursively
static NTSTATUS KiroDeleteDirectoryRecursive(_In_ PCWSTR DirectoryPath) {
    HANDLE dirHandle = NULL;
    OBJECT_ATTRIBUTES objAttr;
    UNICODE_STRING dirPath;
    NTSTATUS status;
    IO_STATUS_BLOCK ioStatus;
    PUCHAR buffer = NULL;
    ULONG bufferSize = 4096;
    BOOLEAN restartScan = TRUE;

    RtlInitUnicodeString(&dirPath, DirectoryPath);
    InitializeObjectAttributes(&objAttr, &dirPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = ZwOpenFile(&dirHandle, GENERIC_READ | DELETE | SYNCHRONIZE, &objAttr, 
                        &ioStatus, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    
    if (!NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: Failed to open dir %wZ (0x%08X)\n", &dirPath, status);
        return status;
    }

    buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, bufferSize, 'Kiro');
    if (buffer == NULL) {
        ZwClose(dirHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Enumerate and delete contents
    while (TRUE) {
        status = ZwQueryDirectoryFile(dirHandle, NULL, NULL, NULL, &ioStatus, 
                                      buffer, bufferSize, FileDirectoryInformation,
                                      FALSE, NULL, restartScan);
        restartScan = FALSE;

        if (status == STATUS_NO_MORE_FILES) break;
        if (!NT_SUCCESS(status)) {
            DbgPrint("KiroDriver: Query dir failed (0x%08X)\n", status);
            ExFreePoolWithTag(buffer, 'Kiro');
            ZwClose(dirHandle);
            return status;
        }

        PFILE_DIRECTORY_INFORMATION dirInfo = (PFILE_DIRECTORY_INFORMATION)buffer;

        if (dirInfo->FileNameLength == 2 && dirInfo->FileName[0] == L'.' && dirInfo->FileName[1] == L'.') continue;
        if (dirInfo->FileNameLength == 1 && dirInfo->FileName[0] == L'.') continue;

        UNICODE_STRING fileName;
        fileName.Buffer = dirInfo->FileName;
        fileName.Length = (USHORT)dirInfo->FileNameLength;
        fileName.MaximumLength = (USHORT)dirInfo->FileNameLength;

        if (dirInfo->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recursively delete subdirectory
            WCHAR subPath[KIRO_MAX_PATH];
            RtlStringCchCopyNW(subPath, KIRO_MAX_PATH, DirectoryPath, KIRO_MAX_PATH - 1);
            RtlStringCchCatNW(subPath, KIRO_MAX_PATH, L"\\", KIRO_MAX_PATH - 1);
            RtlStringCchCatNW(subPath, KIRO_MAX_PATH, fileName.Buffer, KIRO_MAX_PATH - 1);
            KiroDeleteDirectoryRecursive(subPath);
        } else {
            // Delete file
            HANDLE fileHandle;
            OBJECT_ATTRIBUTES fileAttr;
            InitializeObjectAttributes(&fileAttr, &fileName, OBJ_CASE_INSENSITIVE, dirHandle, NULL);
            status = ZwOpenFile(&fileHandle, DELETE | SYNCHRONIZE, &fileAttr, &ioStatus,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
            if (NT_SUCCESS(status)) {
                FILE_DISPOSITION_INFORMATION dispInfo;
                dispInfo.DeleteFile = TRUE;
                ZwSetInformationFile(fileHandle, &ioStatus, &dispInfo, sizeof(dispInfo), FileDispositionInformation);
                ZwClose(fileHandle);
            }
        }
    }

    ExFreePoolWithTag(buffer, 'Kiro');
    ZwClose(dirHandle);

    // Delete the directory itself
    status = ZwOpenFile(&dirHandle, DELETE | SYNCHRONIZE, &objAttr, &ioStatus,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
    if (NT_SUCCESS(status)) {
        FILE_DISPOSITION_INFORMATION dispInfo;
        dispInfo.DeleteFile = TRUE;
        status = ZwSetInformationFile(dirHandle, &ioStatus, &dispInfo, sizeof(dispInfo), FileDispositionInformation);
        ZwClose(dirHandle);
    }

    if (NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: Deleted directory %wZ\n", &dirPath);
    }

    return status;
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
) {
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;

    RtlInitUnicodeString(&g_DeviceName,       DEVICE_NAME_KERNEL);
    RtlInitUnicodeString(&g_SymbolicLinkName, DEVICE_NAME_USER);

    status = IoCreateDevice(
        DriverObject, 0, &g_DeviceName,
        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &deviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: IoCreateDevice failed (0x%08X)\n", status);
        return status;
    }

    status = IoCreateSymbolicLink(&g_SymbolicLinkName, &g_DeviceName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("KiroDriver: IoCreateSymbolicLink failed (0x%08X)\n", status);
        IoDeleteDevice(deviceObject);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = KiroCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = KiroCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KiroDeviceControl;
    DriverObject->DriverUnload                         = DriverUnload;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    g_DeviceObject = deviceObject;

    DbgPrint("KiroDriver: Loaded. Device=%wZ Link=%wZ\n",
             &g_DeviceName, &g_SymbolicLinkName);
    return STATUS_SUCCESS;
}

VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    IoDeleteSymbolicLink(&g_SymbolicLinkName);
    if (g_DeviceObject) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
    DbgPrint("KiroDriver: Unloaded.\n");
}

NTSTATUS KiroCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS KiroDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS           status  = STATUS_SUCCESS;
    ULONG              infoOut = 0;
    PUCHAR             inBuf   = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    PUCHAR             outBuf  = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    size_t             inLen   = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    size_t             outLen  = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

    CHAR response[KIRO_OUTPUT_MAX];
    response[0] = '\0';

    DbgPrint("KiroDriver: IOCTL received code=0x%08X inLen=%zu outLen=%zu\n",
             irpSp->Parameters.DeviceIoControl.IoControlCode, inLen, outLen);
    
    // Log expected IOCTL codes for debugging
    DbgPrint("KiroDriver: RESET_IDS=0x%08X CLEAR_CACHE=0x%08X WRITE_REG=0x%08X DEL_REG=0x%08X CLEAN_PATTERN=0x%08X\n",
             IOCTL_KIRO_RESET_IDS, IOCTL_KIRO_CLEAR_CACHE, IOCTL_KIRO_WRITE_REGISTRY,
             IOCTL_KIRO_DELETE_REGISTRY, IOCTL_KIRO_CLEAN_REGISTRY_PATTERN);

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_KIRO_RESET_IDS: {
        if (inLen < sizeof(KIRO_RESET_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            KiroAppendStr(response, sizeof(response), "KIRO: input too small");
            break;
        }
        PKIRO_RESET_INPUT input = (PKIRO_RESET_INPUT)inBuf;
        if (input == NULL) {
            status = STATUS_INVALID_PARAMETER;
            KiroAppendStr(response, sizeof(response), "KIRO: invalid input ptr");
            break;
        }

        DbgPrint("KiroDriver: Reset IDs machineId=%s devDeviceId=%s\n", 
                 input->NewMachineGuid, input->NewDevDeviceGuid);

        // Convert ANSI to Unicode for registry
        WCHAR machineGuidW[65], devDeviceGuidW[38], sqmIdW[40], crashIdW[38];
        ANSI_STRING ansiStr;
        UNICODE_STRING unicodeStr;

        // Machine ID
        RtlInitAnsiString(&ansiStr, input->NewMachineGuid);
        unicodeStr.Buffer = machineGuidW;
        unicodeStr.MaximumLength = sizeof(machineGuidW);
        unicodeStr.Length = 0;
        RtlAnsiStringToUnicodeString(&unicodeStr, &ansiStr, FALSE);

        // Dev Device ID
        RtlInitAnsiString(&ansiStr, input->NewDevDeviceGuid);
        unicodeStr.Buffer = devDeviceGuidW;
        unicodeStr.MaximumLength = sizeof(devDeviceGuidW);
        unicodeStr.Length = 0;
        RtlAnsiStringToUnicodeString(&unicodeStr, &ansiStr, FALSE);

        // SQM ID
        RtlInitAnsiString(&ansiStr, input->NewSqmId);
        unicodeStr.Buffer = sqmIdW;
        unicodeStr.MaximumLength = sizeof(sqmIdW);
        unicodeStr.Length = 0;
        RtlAnsiStringToUnicodeString(&unicodeStr, &ansiStr, FALSE);

        // Crash ID
        RtlInitAnsiString(&ansiStr, input->NewCrashId);
        unicodeStr.Buffer = crashIdW;
        unicodeStr.MaximumLength = sizeof(crashIdW);
        unicodeStr.Length = 0;
        RtlAnsiStringToUnicodeString(&unicodeStr, &ansiStr, FALSE);

        // Write to registry - these are the same paths the PowerShell script uses
        // We'll write to a kernel-level location that overrides usermode
        NTSTATUS regStatus = STATUS_SUCCESS;
        
        // Write machine ID to kernel registry location
        regStatus = KiroWriteRegistryValue(
            L"\\Registry\\Machine\\Software\\Kiro",
            L"MachineId",
            machineGuidW
        );
        if (!NT_SUCCESS(regStatus)) {
            KiroAppendStr(response, sizeof(response), "KIRO: registry write failed");
            status = regStatus;
            break;
        }

        regStatus = KiroWriteRegistryValue(
            L"\\Registry\\Machine\\Software\\Kiro",
            L"DevDeviceId",
            devDeviceGuidW
        );
        if (!NT_SUCCESS(regStatus)) {
            KiroAppendStr(response, sizeof(response), "KIRO: registry write failed");
            status = regStatus;
            break;
        }

        KiroAppendStr(response, sizeof(response), "KIRO[RING0]: IDs written to kernel registry");
        KiroAppendStr(response, sizeof(response), " machineId=");
        KiroAppendStr(response, sizeof(response), input->NewMachineGuid);
        break;
    }

    case IOCTL_KIRO_CLEAR_CACHE: {
        if (inLen < sizeof(KIRO_CACHE_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            KiroAppendStr(response, sizeof(response), "KIRO: input too small");
            break;
        }
        PKIRO_CACHE_INPUT input = (PKIRO_CACHE_INPUT)inBuf;
        if (input == NULL) {
            status = STATUS_INVALID_PARAMETER;
            KiroAppendStr(response, sizeof(response), "KIRO: invalid input ptr");
            break;
        }

        DbgPrint("KiroDriver: Clear cache path=%ws\n", input->BasePath);

        // Delete the directory recursively from kernel mode
        status = KiroDeleteDirectoryRecursive(input->BasePath);
        if (NT_SUCCESS(status)) {
            KiroAppendStr(response, sizeof(response), "KIRO[RING0]: cache cleared successfully");
        } else {
            KiroAppendStr(response, sizeof(response), "KIRO: cache clear failed");
            status = STATUS_SUCCESS; // Don't fail the whole operation
        }
        break;
    }

    case IOCTL_KIRO_WRITE_REGISTRY: {
        if (inLen < sizeof(KIRO_REGISTRY_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            KiroAppendStr(response, sizeof(response), "KIRO: input too small");
            break;
        }
        PKIRO_REGISTRY_INPUT input = (PKIRO_REGISTRY_INPUT)inBuf;
        if (input == NULL) {
            status = STATUS_INVALID_PARAMETER;
            KiroAppendStr(response, sizeof(response), "KIRO: invalid input ptr");
            break;
        }

        DbgPrint("KiroDriver: Write registry key=%ws val=%ws\n", input->KeyPath, input->ValueName);

        status = KiroWriteRegistryValue(input->KeyPath, input->ValueName, input->ValueData);
        if (NT_SUCCESS(status)) {
            KiroAppendStr(response, sizeof(response), "KIRO[RING0]: registry value written");
        } else {
            KiroAppendStr(response, sizeof(response), "KIRO: registry write failed");
        }
        break;
    }

    case IOCTL_KIRO_DELETE_REGISTRY: {
        if (inLen < sizeof(KIRO_REGISTRY_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            KiroAppendStr(response, sizeof(response), "KIRO: input too small");
            break;
        }
        PKIRO_REGISTRY_INPUT input = (PKIRO_REGISTRY_INPUT)inBuf;
        if (input == NULL) {
            status = STATUS_INVALID_PARAMETER;
            KiroAppendStr(response, sizeof(response), "KIRO: invalid input ptr");
            break;
        }

        DbgPrint("KiroDriver: Delete registry key=%ws val=%ws\n", input->KeyPath, input->ValueName);

        status = KiroDeleteRegistryValue(input->KeyPath, input->ValueName);
        if (NT_SUCCESS(status)) {
            KiroAppendStr(response, sizeof(response), "KIRO[RING0]: registry value deleted");
        } else {
            KiroAppendStr(response, sizeof(response), "KIRO: registry delete failed");
        }
        break;
    }

    case IOCTL_KIRO_CLEAN_REGISTRY_PATTERN: {
        if (inLen < sizeof(KIRO_REGISTRY_INPUT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            KiroAppendStr(response, sizeof(response), "KIRO: input too small");
            break;
        }
        PKIRO_REGISTRY_INPUT input = (PKIRO_REGISTRY_INPUT)inBuf;
        if (input == NULL) {
            status = STATUS_INVALID_PARAMETER;
            KiroAppendStr(response, sizeof(response), "KIRO: invalid input ptr");
            break;
        }

        DbgPrint("KiroDriver: Clean registry pattern=%ws key=%ws\n", input->ValueName, input->KeyPath);

        status = KiroDeleteRegistryValuesByPattern(input->KeyPath, input->ValueName);
        if (NT_SUCCESS(status)) {
            KiroAppendStr(response, sizeof(response), "KIRO[RING0]: registry pattern cleaned");
        } else {
            KiroAppendStr(response, sizeof(response), "KIRO: registry pattern clean failed");
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        KiroAppendStr(response, sizeof(response), "KIRO: unknown IOCTL");
        break;
    }

    KiroWriteOutput(outBuf, outLen, response, &infoOut);

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = infoOut;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
