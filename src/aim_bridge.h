#pragma once

#include <string>
#include <vector>
#include <cstdint>

class AimBridge {
public:
    struct DeviceInfo {
        int number;
        std::string mountPoint;
        uint64_t sizeBytes;
        std::string backingFile;
    };

    static bool IsAvailable();
    bool MountVm(const std::string& vhdPath, uint64_t sizeBytes, std::string& outMountPoint);
    bool MountVmFromFile(const std::string& vhdPath, std::string& outMountPoint);
    bool Unmount(int deviceNumber, bool force = false);
    std::vector<DeviceInfo> ListDevices();
    bool ReadDeviceData(int deviceNumber, uint64_t sizeBytes, void* buffer);
    static std::string FindAimLl();

private:
    bool RunAimLl(const std::string& args, std::string& stdoutOutput);
    std::string FindPhysicalDrivePath(int deviceNumber);
};
