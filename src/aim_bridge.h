#pragma once

#include <string>
#include <vector>
#include <cstdint>

class AimBridge {
public:
    struct DeviceInfo {
        int number = -1;
        std::string physicalDrive;  // e.g. "\\?\PhysicalDrive1"
        std::string mountPoint;     // drive letter or "?"
        uint64_t sizeBytes = 0;
        std::string backingFile;
    };

    static bool IsAvailable();
    bool MountVm(const std::string& vhdPath, uint64_t sizeBytes, std::string& outMountPoint);
    bool MountVmFromFile(const std::string& vhdPath, std::string& outMountPoint);
    bool MountProxy(const std::string& pipeName, uint64_t sizeBytes, std::string& outMountPoint);
    bool Unmount(int deviceNumber, bool force = false);
    std::vector<DeviceInfo> ListDevices();
    bool ReadDeviceData(const std::string& physicalDrivePath, uint64_t sizeBytes, void* buffer);
    static std::string FindAimLl();

private:
    bool RunAimLl(const std::string& args, std::string& stdoutOutput);
};
