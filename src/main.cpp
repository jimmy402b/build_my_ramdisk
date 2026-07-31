#include "cli_parser.h"
#include "vhd_engine.h"
#include "aim_bridge.h"
#include "save_scheduler.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static VhdEngine g_vhd;
static AimBridge g_aim;
static SaveScheduler g_scheduler;
static std::string g_vhdPath;
static uint64_t g_sizeBytes = 0;
static int g_deviceNumber = 0;
static std::string g_mountPoint;

static bool DoSave() {
    if (!g_vhd.IsOpen()) { LogError("VHD not open"); return false; }
    std::vector<uint8_t> buffer(g_sizeBytes);

    // Find physical drive path from mounted devices
    auto devices = g_aim.ListDevices();
    std::string physPath;
    for (auto& d : devices) {
        if (d.number == g_deviceNumber) { physPath = d.physicalDrive; break; }
    }
    if (physPath.empty()) {
        LogError("Cannot find physical drive for device " + std::to_string(g_deviceNumber));
        return false;
    }
    // Convert \\?\PhysicalDrive1 → \\.\PhysicalDrive1
    if (StartsWith(physPath, "\\\\?\\")) physPath = "\\\\.\\" + physPath.substr(4);

    if (!g_aim.ReadDeviceData(physPath, g_sizeBytes, buffer.data())) {
        LogError("Failed to read device data");
        return false;
    }
    if (!g_vhd.WriteAll(buffer.data())) {
        LogError("Failed to write VHD: " + g_vhd.GetLastError());
        return false;
    }
    LogInfo("Saved " + FormatSize(g_sizeBytes) + " to " + g_vhdPath);
    return true;
}

static int CmdCreate(const CreateOptions& opts) {
    LogInfo("Creating VHD: " + opts.outputPath + " (" + FormatSize(opts.sizeBytes) + ")");
    if (!g_vhd.Create(opts.outputPath, opts.sizeBytes, opts.dynamic)) {
        LogError("Failed to create VHD: " + g_vhd.GetLastError());
        return 2;
    }
    g_vhd.Close();

    LogInfo("Mounting as RAM disk...");
    if (!g_aim.MountVm(opts.outputPath, opts.sizeBytes, g_mountPoint)) {
        LogError("Failed to mount RAM disk");
        return 3;
    }

    g_vhdPath = opts.outputPath;
    g_sizeBytes = opts.sizeBytes;

    if (!g_vhd.Open(opts.outputPath, true)) {
        LogError("Failed to re-open VHD: " + g_vhd.GetLastError());
        g_aim.Unmount(g_deviceNumber, true);
        return 2;
    }

    g_scheduler.SetSaveCallback(DoSave);
    g_scheduler.RegisterShutdownHandler();
    g_scheduler.Start(opts.saveIntervalSec);

    printf("\nRAM disk ready: %s (%s)\n", g_mountPoint.c_str(), opts.outputPath.c_str());
    if (opts.saveIntervalSec > 0)
        printf("Save interval: %ds | Press Ctrl+C to stop\n", opts.saveIntervalSec);
    else
        printf("Manual save only | Press Ctrl+C to stop\n");

    while (g_scheduler.IsRunning()) Sleep(1000);
    return 0;
}

static int CmdMount(const MountOptions& opts) {
    LogInfo("Opening VHD: " + opts.inputPath);
    if (!g_vhd.Open(opts.inputPath, !opts.readonly)) {
        LogError("Failed to open VHD: " + g_vhd.GetLastError());
        return 2;
    }

    g_vhdPath = opts.inputPath;
    g_sizeBytes = g_vhd.GetSizeBytes();
    LogInfo("VHD size: " + FormatSize(g_sizeBytes));

    LogInfo("Mounting as RAM disk...");
    if (!g_aim.MountVmFromFile(opts.inputPath, g_mountPoint)) {
        LogError("Failed to mount RAM disk");
        return 3;
    }

    if (!opts.readonly) {
        g_scheduler.SetSaveCallback(DoSave);
        g_scheduler.RegisterShutdownHandler();
        g_scheduler.Start(opts.saveIntervalSec);
    } else {
        LogInfo("Read-only mode");
    }

    printf("\nRAM disk ready: %s (%s, %s)\n",
           g_mountPoint.c_str(), opts.inputPath.c_str(),
           opts.readonly ? "read-only" : "writable");
    if (!opts.readonly && opts.saveIntervalSec > 0)
        printf("Save interval: %ds | Press Ctrl+C to stop\n", opts.saveIntervalSec);
    else if (!opts.readonly)
        printf("Manual save only | Press Ctrl+C to stop\n");

    while (g_scheduler.IsRunning()) Sleep(1000);
    return 0;
}

static int CmdSave(const SaveOptions& opts) {
    if (!g_vhd.IsOpen()) { LogError("No active VHD session"); return 2; }
    return DoSave() ? 0 : 2;
}

static int CmdUnmount(const UnmountOptions& opts) {
    if (!opts.noSave && g_vhd.IsOpen()) { LogInfo("Saving before unmount..."); DoSave(); }
    g_scheduler.Stop();
    g_vhd.Close();
    if (!g_aim.Unmount(opts.device, false)) {
        LogError("Failed to unmount device " + std::to_string(opts.device));
        return 3;
    }
    printf("RAM disk unmounted.\n");
    return 0;
}

static int CmdStatus() {
    auto devices = g_aim.ListDevices();
    if (devices.empty()) { printf("No RAM disks mounted.\n"); return 0; }
    for (auto& d : devices) {
        printf("[%d] %s -> %s (%s)\n", d.number,
               d.backingFile.empty() ? "(no file)" : d.backingFile.c_str(),
               d.physicalDrive.c_str(), FormatSize(d.sizeBytes).c_str());
    }
    return 0;
}

int main(int argc, char* argv[]) {
    auto r = ParseArgs(argc, argv);
    if (!r.error.empty()) { fprintf(stderr, "Error: %s\nTry 'ramdisk-cli help'\n", r.error.c_str()); return 1; }

    bool needsAim = (r.command == Command::Create || r.command == Command::Mount ||
                     r.command == Command::Save || r.command == Command::Unmount || r.command == Command::Status);
    // Check for admin privileges for commands that need AIM
    if (needsAim) {
        BOOL isElevated = FALSE;
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            TOKEN_ELEVATION elev;
            DWORD size = sizeof(elev);
            if (GetTokenInformation(hToken, TokenElevation, &elev, size, &size))
                isElevated = elev.TokenIsElevated;
            CloseHandle(hToken);
        }
        if (!isElevated) {
            fprintf(stderr, "Error: Administrator privileges required.\n");
            fprintf(stderr, "Please run this command from an elevated terminal (Run as Administrator).\n");
            return 3;
        }
        if (!AimBridge::IsAvailable()) {
            fprintf(stderr, "Error: aim_ll.exe not found. Install AIM Toolkit or set AIM_LL_PATH.\n");
            return 3;
        }
    }

    switch (r.command) {
    case Command::Create:   return CmdCreate(r.create);
    case Command::Mount:    return CmdMount(r.mount);
    case Command::Save:     return CmdSave(r.save);
    case Command::Unmount:  return CmdUnmount(r.unmount);
    case Command::Status:   return CmdStatus();
    case Command::Help:     PrintHelp(); return 0;
    case Command::Version:  PrintVersion(); return 0;
    default:                PrintHelp(); return 0;
    }
}
