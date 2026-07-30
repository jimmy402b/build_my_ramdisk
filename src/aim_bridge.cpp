#include "aim_bridge.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>

std::string AimBridge::FindAimLl() {
    const char* env = getenv("AIM_LL_PATH");
    if (env && GetFileAttributesA(env) != INVALID_FILE_ATTRIBUTES)
        return env;

    const char* defaultPath = "C:\\Program Files\\AIM Toolkit\\aim_ll.exe";
    if (GetFileAttributesA(defaultPath) != INVALID_FILE_ATTRIBUTES)
        return defaultPath;

    const char* localPath = ".\\aim_ll.exe";
    if (GetFileAttributesA(localPath) != INVALID_FILE_ATTRIBUTES)
        return localPath;

    return "";
}

bool AimBridge::IsAvailable() {
    return !FindAimLl().empty();
}

bool AimBridge::RunAimLl(const std::string& args, std::string& stdoutOutput) {
    std::string aimPath = FindAimLl();
    if (aimPath.empty()) { LogError("aim_ll.exe not found"); return false; }

    std::string cmdLine = "\"" + aimPath + "\" " + args;

    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        LogError("CreatePipe failed: " + GetLastWinErrorMessage());
        return false;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        LogError("CreateProcess failed: " + GetLastWinErrorMessage());
        CloseHandle(hRead); CloseHandle(hWrite); return false;
    }
    CloseHandle(hWrite);

    DWORD waitResult = WaitForSingleObject(pi.hProcess, 30000);
    if (waitResult != WAIT_OBJECT_0) {
        LogError("aim_ll timed out");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);
        return false;
    }

    char buf[4096];
    DWORD bytesRead;
    std::ostringstream oss;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0'; oss << buf;
    }
    stdoutOutput = oss.str();

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);
    return exitCode == 0;
}

bool AimBridge::MountVm(const std::string& vhdPath, uint64_t sizeBytes, std::string& outMountPoint) {
    char sizeBuf[32];
    if (sizeBytes % (1024ULL * 1024 * 1024) == 0)
        snprintf(sizeBuf, sizeof(sizeBuf), "%lluG", (unsigned long long)(sizeBytes / (1024ULL*1024*1024)));
    else if (sizeBytes % (1024 * 1024) == 0)
        snprintf(sizeBuf, sizeof(sizeBuf), "%lluM", (unsigned long long)(sizeBytes / (1024*1024)));
    else
        snprintf(sizeBuf, sizeof(sizeBuf), "%llu", (unsigned long long)sizeBytes);

    std::string args = "-a -t vm -f \"" + vhdPath + "\" -s " + sizeBuf + " -n";
    std::string output;
    if (!RunAimLl(args, output)) return false;

    int devNum = std::atoi(output.c_str());
    LogInfo("Mounted as device " + std::to_string(devNum));

    auto devices = ListDevices();
    for (auto& d : devices) {
        if (d.number == devNum) { outMountPoint = d.mountPoint; return true; }
    }
    outMountPoint = "?";
    return true;
}

bool AimBridge::MountVmFromFile(const std::string& vhdPath, std::string& outMountPoint) {
    std::string args = "-a -t vm -f \"" + vhdPath + "\" -n";
    std::string output;
    if (!RunAimLl(args, output)) return false;

    int devNum = std::atoi(output.c_str());
    LogInfo("Mounted VHD as device " + std::to_string(devNum));

    auto devices = ListDevices();
    for (auto& d : devices) {
        if (d.number == devNum) { outMountPoint = d.mountPoint; return true; }
    }
    outMountPoint = "?";
    return true;
}

bool AimBridge::Unmount(int deviceNumber, bool force) {
    char argBuf[64];
    snprintf(argBuf, sizeof(argBuf), "%s -u %d", force ? "-D" : "-d", deviceNumber);
    std::string output;
    return RunAimLl(argBuf, output);
}

std::vector<AimBridge::DeviceInfo> AimBridge::ListDevices() {
    std::vector<DeviceInfo> result;
    std::string output;
    if (!RunAimLl("-l", output)) return result;

    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        DeviceInfo info;
        std::istringstream liness(line);
        liness >> info.number;
        std::string token;
        if (liness >> token) {
            if (token.size() == 2 && token[1] == ':') { info.mountPoint = token; liness >> info.sizeBytes; }
            else { info.mountPoint = ""; info.sizeBytes = std::stoull(token); }
        }
        std::string rest;
        std::getline(liness, rest);
        info.backingFile = Trim(rest);
        result.push_back(info);
    }
    return result;
}

std::string AimBridge::FindPhysicalDrivePath(int deviceNumber) {
    return "\\\\.\\PhysicalDrive" + std::to_string(deviceNumber);
}

bool AimBridge::ReadDeviceData(int deviceNumber, uint64_t sizeBytes, void* buffer) {
    std::string physPath = FindPhysicalDrivePath(deviceNumber);
    LogInfo("Reading device data from: " + physPath);

    HANDLE h = CreateFileA(physPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogError("Cannot open device: " + GetLastWinErrorMessage());
        return false;
    }

    const DWORD CHUNK = 64 * 1024 * 1024;
    uint64_t remaining = sizeBytes;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    while (remaining > 0) {
        DWORD toRead = (remaining > CHUNK) ? CHUNK : static_cast<DWORD>(remaining);
        DWORD bytesRead = 0;
        if (!ReadFile(h, dst, toRead, &bytesRead, nullptr)) {
            LogError("ReadFile failed: " + GetLastWinErrorMessage());
            CloseHandle(h); return false;
        }
        if (bytesRead == 0) break;
        dst += bytesRead; remaining -= bytesRead;
    }
    CloseHandle(h);
    return remaining == 0;
}
