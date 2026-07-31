#include "aim_api.h"
#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

AimApi::AimApi() : m_hDll(nullptr) {}
AimApi::~AimApi() { if (m_hDll) FreeLibrary(m_hDll); }

bool AimApi::Load() {
    const char* paths[] = {
        "C:\\Program Files\\AIM Toolkit\\aimapi.dll",
        ".\\aimapi.dll",
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        m_hDll = LoadLibraryA(paths[i]);
        if (m_hDll) { LogInfo("Loaded aimapi.dll from: " + std::string(paths[i])); break; }
    }
    if (!m_hDll) { LogError("aimapi.dll not found"); return false; }
    return LoadFunctions();
}

bool AimApi::LoadFunctions() {
    m_GetDeviceList = (FnGetDeviceList)GetProcAddress(m_hDll, "ImScsiGetDeviceList");
    m_CreateDevice  = (FnCreateDevice)GetProcAddress(m_hDll, "ImScsiCreateDevice");
    m_RemoveByNumber = (FnRemoveByNumber)GetProcAddress(m_hDll, "ImScsiRemoveDeviceByNumber");
    m_CheckDriverVersion = (FnCheckDriverVersion)GetProcAddress(m_hDll, "ImScsiCheckDriverVersion");
    m_GetVersion = (FnGetVersion)GetProcAddress(m_hDll, "ImScsiGetVersion");

    if (!m_GetDeviceList || !m_CreateDevice || !m_RemoveByNumber) {
        LogError("Failed to resolve aimapi.dll exports");
        return false;
    }
    LogInfo("aimapi.dll functions loaded");
    return true;
}

bool AimApi::CreateDevice(uint64_t sizeBytes, const std::wstring& imageFile,
                           std::wstring& outMountPoint, int& outDeviceNumber,
                           bool writable) {
    if (!m_hDll || !m_CreateDevice) return false;

    DWORD deviceNumber = 0x00FFFFFF; // IMSCSI_AUTO_DEVICE_NUMBER
    LARGE_INTEGER diskSize;
    diskSize.QuadPart = static_cast<LONGLONG>(sizeBytes);

    DWORD bytesPerSector = 0; // default
    LARGE_INTEGER imageOffset;
    imageOffset.QuadPart = 0;

    // Flags: IMSCSI_TYPE_VM (0x200) = use virtual memory as backing
    DWORD flags = 0x00000200; // IMSCSI_TYPE_VM
    if (!writable) flags |= 1; // read-only

    WCHAR mountBuf[64] = {};
    const WCHAR* fileNamePtr = imageFile.empty() ? nullptr : imageFile.c_str();

    BOOL ok = m_CreateDevice(
        nullptr,                    // hWndStatusText — non-interactive
        nullptr,                    // Adapter — auto-select
        &deviceNumber,              // DeviceNumber — auto-assign
        &diskSize,                  // DiskSize
        &bytesPerSector,            // BytesPerSector — default
        &imageOffset,               // ImageOffset
        &flags,                     // Flags
        fileNamePtr,                // FileName (NULL = no backing file)
        FALSE,                      // NativePath — use DOS paths
        mountBuf,                   // MountPoint — receive drive letter
        FALSE                       // CreatePartition — we handle this externally
    );

    if (!ok) {
        LogError("ImScsiCreateDevice failed: " + GetLastWinErrorMessage());
        return false;
    }

    outDeviceNumber = static_cast<int>(deviceNumber);
    outMountPoint = mountBuf;
    LogInfo("Created device " + std::to_string(outDeviceNumber));
    return true;
}

bool AimApi::RemoveDevice(int deviceNumber) {
    if (!m_hDll || !m_RemoveByNumber) return false;
    return m_RemoveByNumber(nullptr, nullptr, static_cast<DWORD>(deviceNumber));
}

std::vector<AimApi::DeviceInfo> AimApi::ListDevices() {
    std::vector<DeviceInfo> result;
    // Fallback: use aim_ll -l output parsing until ImScsiGetDeviceList
    // parameter structure (DEVICE_NUMBER) is fully confirmed.
    // For now, delegate to existing AimBridge logic via subprocess.
    return result;
}

std::wstring AimApi::FindFreeDriveLetter() {
    DWORD drives = GetLogicalDrives();
    for (WCHAR c = L'R'; c <= L'Z'; c++) {
        if (!(drives & (1 << (c - L'A')))) {
            return std::wstring(1, c);
        }
    }
    return L"";
}

bool AimApi::ReadDeviceData(const std::string& physPath,
                             uint64_t sizeBytes, void* buffer) {
    HANDLE h = CreateFileA(physPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogError("Cannot open " + physPath + ": " + GetLastWinErrorMessage());
        return false;
    }

    const DWORD CHUNK = 64 * 1024 * 1024; // 64MB
    uint64_t remaining = sizeBytes;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    while (remaining > 0) {
        DWORD toRead = (remaining > CHUNK) ? CHUNK : static_cast<DWORD>(remaining);
        DWORD rd = 0;
        if (!ReadFile(h, dst, toRead, &rd, nullptr)) {
            LogError("ReadFile failed: " + GetLastWinErrorMessage());
            CloseHandle(h); return false;
        }
        if (rd == 0) break;
        dst += rd; remaining -= rd;
    }
    CloseHandle(h);
    return remaining == 0;
}
