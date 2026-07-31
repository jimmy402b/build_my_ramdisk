#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

// Wrapper around aimapi.dll for direct Arsenal Image Mounter driver access.
// Replaces aim_ll.exe subprocess bridge with LoadLibrary + GetProcAddress.
class AimApi {
public:
    struct DeviceInfo {
        int deviceNumber;
        std::string mountPoint;      // drive letter or "?"
        uint64_t sizeBytes;
        std::string imageFile;
        std::string physicalDrive;
    };

    AimApi();
    ~AimApi();
    bool Load();

    // Direct ImScsiCreateDevice wrapper
    bool CreateDevice(uint64_t sizeBytes, const std::wstring& imageFile,
                      std::wstring& outMountPoint, int& outDeviceNumber,
                      bool writable = true);

    // Remove by device number
    bool RemoveDevice(int deviceNumber);

    // List all virtual disks
    std::vector<DeviceInfo> ListDevices();

    // Read raw data from PhysicalDrive (for save operations)
    bool ReadDeviceData(const std::string& physicalDrivePath,
                        uint64_t sizeBytes, void* buffer);

    // Find an available drive letter
    std::wstring FindFreeDriveLetter();

    bool IsAvailable() const { return m_hDll != nullptr; }

private:
    HMODULE m_hDll;
    bool LoadFunctions();

    // Function pointer typedefs — from aimapi.h
    typedef BOOL (WINAPI *FnGetDeviceList)(ULONG, HANDLE, void*, ULONG*);
    typedef BOOL (WINAPI *FnCreateDevice)(HWND, HANDLE, void*, LARGE_INTEGER*,
        DWORD*, LARGE_INTEGER*, DWORD*, LPCWSTR, BOOL, LPWSTR, BOOL);
    typedef BOOL (WINAPI *FnRemoveByNumber)(HWND, HANDLE, DWORD);
    typedef BOOL (WINAPI *FnCheckDriverVersion)(HANDLE);
    typedef BOOL (WINAPI *FnGetVersion)(ULONG*, ULONG*);

    FnGetDeviceList m_GetDeviceList = nullptr;
    FnCreateDevice m_CreateDevice = nullptr;
    FnRemoveByNumber m_RemoveByNumber = nullptr;
    FnCheckDriverVersion m_CheckDriverVersion = nullptr;
    FnGetVersion m_GetVersion = nullptr;
};
