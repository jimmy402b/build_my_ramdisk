#include "cli_parser.h"
#include "vhd_engine.h"
#include "aim_bridge.h"
#include "save_scheduler.h"
#include "utils.h"
#include "drive_letter.h"
#include "daemon.h"
#include "storage_server.h"
#include "watchdog.h"
#include "aim_api.h"
#include "tray_icon.h"

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
static int g_origArgc = 0;
static char** g_origArgv = nullptr;
static std::vector<uint8_t> g_lastSnapshot; // for quick-save
static bool g_useQuickSave = false;
static AimApi g_aimApi; // aimapi.dll (non-fatal if unavailable)

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

    // Flush ALL mounted volumes on this disk before reading.
    // FlushFileBuffers on PhysicalDrive only flushes disk cache, NOT NTFS volume cache.
    // We must enumerate and flush each volume (drive letter) on the disk.
    {
        DWORD drives = GetLogicalDrives();
        for (char c = 'A'; c <= 'Z'; c++) {
            if (drives & (1 << (c - 'A'))) {
                char volPath[8];
                snprintf(volPath, sizeof(volPath), "%c:\\", c);
                UINT type = GetDriveTypeA(volPath);
                if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
                    HANDLE hVol = CreateFileA(volPath, GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              nullptr, OPEN_EXISTING,
                                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (hVol != INVALID_HANDLE_VALUE) {
                        FlushFileBuffers(hVol);
                        CloseHandle(hVol);
                    }
                }
            }
        }
    }

    if (!g_aim.ReadDeviceData(physPath, g_sizeBytes, buffer.data())) {
        LogError("Failed to read device data");
        return false;
    }
    if (g_useQuickSave && !g_lastSnapshot.empty()) {
        uint64_t changed = 0;
        for (uint64_t off = 0; off < g_sizeBytes; off += 512) {
            if (memcmp(buffer.data() + off, g_lastSnapshot.data() + off, 512) != 0) {
                g_vhd.WriteSectors(off / 512, 1, buffer.data() + off);
                changed++;
            }
        }
        LogInfo("Quick save: " + std::to_string(changed) + " sectors (" + FormatSize(changed*512) + ")");
        WriteLog("Quick save: " + std::to_string(changed) + " sectors -> " + g_vhdPath);
    } else {
        if (!g_vhd.WriteAll(buffer.data())) {
            LogError("Failed to write VHD: " + g_vhd.GetLastError());
            return false;
        }
        LogInfo("Saved " + FormatSize(g_sizeBytes) + " to " + g_vhdPath);
        WriteLog("Save: " + FormatSize(g_sizeBytes) + " -> " + g_vhdPath);
    }
    g_lastSnapshot = std::move(buffer);
    return true;
}

static int CmdCreate(const CreateOptions& opts) {
    if (opts.daemon) { if (!Daemonize(g_origArgc, g_origArgv)) return 0; InitLogFile(opts.logFile); }
    g_useQuickSave = opts.quickSave;
    LogInfo("Creating VHD: " + opts.outputPath + " (" + FormatSize(opts.sizeBytes) + ")");
    if (!g_vhd.Create(opts.outputPath, opts.sizeBytes, opts.dynamic)) {
        LogError("Failed to create VHD: " + g_vhd.GetLastError());
        return 2;
    }

    // Extract raw data from VHD for aim_ll (VHD has headers; aim_ll needs flat raw data)
    std::vector<uint8_t> rawData(opts.sizeBytes, 0);
    g_vhd.ReadAll(rawData.data());
    g_vhd.Close();

    // Write raw data to temp flat file for aim_ll
    std::string rawPath = opts.outputPath + ".raw";
    HANDLE hRaw = CreateFileA(rawPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hRaw == INVALID_HANDLE_VALUE) {
        LogError("Failed to create raw temp file: " + rawPath);
        return 4;
    }
    DWORD written = 0;
    WriteFile(hRaw, rawData.data(), static_cast<DWORD>(rawData.size()), &written, nullptr);
    CloseHandle(hRaw);

    if (opts.mode == "proxy") {
        // === PROXY MODE ===
        std::string pipeName = "\\\\.\\pipe\\ramdisk_proxy";
        static StorageServer server;
        static Watchdog watchdog;

        server.SetLogger([](const std::string& m) { WriteLog("Server: " + m); });
        watchdog.SetLogger([](const std::string& m) { WriteLog("Watchdog: " + m); });

        if (!server.Start(pipeName, opts.outputPath, opts.sizeBytes)) {
            LogError("Failed to start storage server"); return 4;
        }
        WriteLog("Storage server online at " + pipeName);

        if (!g_aim.MountProxy(pipeName, opts.sizeBytes, g_mountPoint)) {
            LogError("Failed to mount proxy device"); server.Stop(); return 3;
        }

        // Watchdog monitors the server (in daemon mode, runs as separate process)
        if (opts.daemon) {
            char myPath[MAX_PATH]; GetModuleFileNameA(nullptr, myPath, sizeof(myPath));
            std::string wArgs = "\"" + std::string(myPath) + "\" --internal-proxy-server " + pipeName + " " + opts.outputPath + " " + std::to_string(opts.sizeBytes);
            watchdog.Start(myPath, wArgs, pipeName);
        }

        g_vhdPath = opts.outputPath;
        g_sizeBytes = opts.sizeBytes;

        // Adjust save callback for proxy (use incremental save)
        g_scheduler.SetSaveCallback([]() -> bool {
            // In proxy mode, save is managed by StorageServer::SaveDirty
            // For standalone save command, use server.SaveDirty()
            return true;
        });
    } else {
        // === VM MODE (original flow) ===
        LogInfo("Mounting as RAM disk...");
        if (!g_aim.MountVm(rawPath, opts.sizeBytes, g_mountPoint)) {
            LogError("Failed to mount RAM disk");
            return 3;
        }
        remove(rawPath.c_str());

        g_vhdPath = opts.outputPath;
        g_sizeBytes = opts.sizeBytes;

        if (!g_vhd.Open(opts.outputPath, true)) {
            LogError("Failed to re-open VHD: " + g_vhd.GetLastError());
            g_aim.Unmount(g_deviceNumber, true);
            return 2;
        }

        g_scheduler.SetSaveCallback(DoSave);
    }
    g_scheduler.RegisterShutdownHandler();
    g_scheduler.Start(opts.saveIntervalSec);

    printf("\nRAM disk ready: %s (%s)\n", g_mountPoint.c_str(), opts.outputPath.c_str());
    if (opts.saveIntervalSec > 0)
        printf("Save interval: %ds | Press Ctrl+C to stop\n", opts.saveIntervalSec);
    else
        printf("Manual save only | Press Ctrl+C to stop\n");

    // Auto-assign drive letter (non-fatal)
    std::string dl = AssignDriveLetter(opts.sizeBytes);
    if (!dl.empty()) { printf("Drive: %s\n", dl.c_str()); WriteLog("Drive: " + dl); }
    else { WriteLog("Drive letter assignment skipped or failed"); }

    // Auto-format if requested
    if (opts.autoFormat && !dl.empty()) {
        WriteLog("Auto-formatting " + dl + " as " + opts.filesystem + "...");
        // Build and run diskpart script
        char scriptPath[MAX_PATH], tmpPath[MAX_PATH];
        GetTempPathA(sizeof(tmpPath), tmpPath);
        snprintf(scriptPath, sizeof(scriptPath), "%s\\ramdisk_fmt.txt", tmpPath);
        FILE* f = fopen(scriptPath, "w");
        if (f) {
            int dnum = 0;
            auto devs = g_aim.ListDevices();
            for (auto& d : devs) { if (d.number == g_deviceNumber) { dnum = d.number; break; } }
            // Find disk number by size
            for (int diskN = 0; diskN < 16; diskN++) {
                char phys[64]; snprintf(phys, sizeof(phys), "\\\\.\\PhysicalDrive%d", diskN);
                HANDLE hd = CreateFileA(phys, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (hd == INVALID_HANDLE_VALUE) continue;
                GET_LENGTH_INFORMATION gli; DWORD rd = 0;
                if (DeviceIoControl(hd, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &gli, sizeof(gli), &rd, nullptr)
                    && gli.Length.QuadPart == (LONGLONG)opts.sizeBytes) {
                    fprintf(f, "select disk %d\n", diskN);
                    fprintf(f, "online disk\n");
                    fprintf(f, "convert gpt\n");
                    fprintf(f, "create partition primary\n");
                    fprintf(f, "format fs=%s quick label=%s\n", opts.filesystem.c_str(), opts.label.c_str());
                    fprintf(f, "assign letter=%c\n", dl[0]);
                    fprintf(f, "exit\n");
                    CloseHandle(hd); break;
                }
                CloseHandle(hd);
            }
            fclose(f);
            // Run diskpart
            char cmd[512]; snprintf(cmd, sizeof(cmd), "diskpart /s \"%s\"", scriptPath);
            STARTUPINFOA dsi = { sizeof(dsi) }; dsi.dwFlags = STARTF_USESHOWWINDOW; dsi.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION dpi = {};
            if (CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &dsi, &dpi)) {
                WaitForSingleObject(dpi.hProcess, 30000);
                CloseHandle(dpi.hProcess); CloseHandle(dpi.hThread);
            }
            remove(scriptPath);
            WriteLog("Auto-format complete: " + dl + " (" + opts.filesystem + ", " + opts.label + ")");
        }
    }

    WriteLog("Mounted: " + opts.outputPath + " (" + FormatSize(opts.sizeBytes) +
             "), interval=" + std::to_string(opts.saveIntervalSec) + "s");

    void* hUnmount = CreateUnmountEvent();
    while (g_scheduler.IsRunning()) {
        if (IsUnmountSignaled(hUnmount)) { WriteLog("Unmount signal received"); break; }
        Sleep(500);
    }
    WriteLog("Saving before exit...");
    DoSave();
    g_vhd.Close();
    g_aim.Unmount(g_deviceNumber, true);
    WriteLog("Unmounted, exiting.");
    CloseLogFile();
    if (hUnmount) CloseHandle((HANDLE)hUnmount);
    return 0;
}

static int CmdMount(const MountOptions& opts) {
    if (opts.daemon) { if (!Daemonize(g_origArgc, g_origArgv)) return 0; InitLogFile(opts.logFile); }
    g_useQuickSave = opts.quickSave;
    LogInfo("Opening VHD: " + opts.inputPath);
    if (!g_vhd.Open(opts.inputPath, !opts.readonly)) {
        LogError("Failed to open VHD: " + g_vhd.GetLastError());
        return 2;
    }

    g_vhdPath = opts.inputPath;
    g_sizeBytes = g_vhd.GetSizeBytes();
    LogInfo("VHD size: " + FormatSize(g_sizeBytes));

    // Extract raw data from VHD for aim_ll
    std::vector<uint8_t> rawData(g_sizeBytes, 0);
    g_vhd.ReadAll(rawData.data());

    // Write to temp flat file
    std::string rawPath = opts.inputPath + ".raw";
    HANDLE hRaw = CreateFileA(rawPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hRaw == INVALID_HANDLE_VALUE) {
        LogError("Failed to create raw temp file");
        return 4;
    }
    DWORD written = 0;
    WriteFile(hRaw, rawData.data(), static_cast<DWORD>(rawData.size()), &written, nullptr);
    CloseHandle(hRaw);

    LogInfo("Mounting as RAM disk...");
    if (!g_aim.MountVm(rawPath, g_sizeBytes, g_mountPoint)) {
        LogError("Failed to mount RAM disk");
        remove(rawPath.c_str());
        return 3;
    }
    remove(rawPath.c_str());

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

    std::string dl = AssignDriveLetter(g_sizeBytes);
    if (!dl.empty()) printf("Drive: %s\n", dl.c_str());

    WriteLog("Mounted: " + opts.inputPath + " (" + FormatSize(g_sizeBytes) + ")");

    void* hUnmount = CreateUnmountEvent();
    while (g_scheduler.IsRunning()) {
        if (IsUnmountSignaled(hUnmount)) { WriteLog("Unmount signal received"); break; }
        Sleep(500);
    }
    WriteLog("Saving before exit...");
    DoSave();
    g_vhd.Close();
    g_aim.Unmount(g_deviceNumber, true);
    WriteLog("Unmounted, exiting.");
    CloseLogFile();
    if (hUnmount) CloseHandle((HANDLE)hUnmount);
    return 0;
}

static int CmdSave(const SaveOptions& opts) {
    // Standalone save: find device, read PhysicalDrive, write to VHD
    auto devices = g_aim.ListDevices();
    if (devices.empty()) { LogError("No RAM disks mounted"); return 3; }

    for (auto& d : devices) {
        if (opts.device != 0 && d.number != opts.device) continue;
        if (d.backingFile.empty()) continue;

        // Clean NT path prefix and map .raw back to .vhd
        std::string vhdPath = d.backingFile;
        if (StartsWith(vhdPath, "\\??\\")) vhdPath = vhdPath.substr(4);
        // If mounted via raw temp file, map back to VHD
        if (vhdPath.size() > 4 && vhdPath.substr(vhdPath.size() - 4) == ".raw") {
            vhdPath = vhdPath.substr(0, vhdPath.size() - 4);
        }

        LogInfo("Saving device " + std::to_string(d.number) + " -> " + vhdPath);

        VhdEngine vhd;
        if (!vhd.Open(vhdPath, true)) {
            LogError("Cannot open VHD: " + vhd.GetLastError());
            return 2;
        }

        uint64_t size = vhd.GetSizeBytes();
        std::vector<uint8_t> buffer(size);

        std::string physPath = d.physicalDrive;
        if (StartsWith(physPath, "\\\\?\\")) physPath = "\\\\.\\" + physPath.substr(4);

        // Flush all volumes before reading device
        {
            DWORD drives = GetLogicalDrives();
            for (char c = 'A'; c <= 'Z'; c++) {
                if (drives & (1 << (c - 'A'))) {
                    char vp[8]; snprintf(vp, sizeof(vp), "%c:\\", c);
                    UINT t = GetDriveTypeA(vp);
                    if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE) {
                        HANDLE hv = CreateFileA(vp, GENERIC_READ | GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                nullptr, OPEN_EXISTING,
                                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                        if (hv != INVALID_HANDLE_VALUE) { FlushFileBuffers(hv); CloseHandle(hv); }
                    }
                }
            }
        }

        if (!g_aim.ReadDeviceData(physPath, size, buffer.data())) {
            LogError("Failed to read device data");
            return 3;
        }

        if (!vhd.WriteAll(buffer.data())) {
            LogError("Failed to write VHD: " + vhd.GetLastError());
            return 2;
        }

        vhd.Close();
        LogInfo("Saved " + FormatSize(size) + " to " + d.backingFile);
    }
    return 0;
}

static int CmdUnmount(const UnmountOptions& opts) {
    // Try signaling daemon first
    HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Global\\RamDiskCli_Unmount");
    if (h) {
        // Daemon is running — signal it and let it handle save + unmount
        SetEvent(h); CloseHandle(h);
        printf("Unmount signal sent to daemon.\n");
        // Give daemon time to save + unmount
        for (int i = 0; i < 30; i++) {
            Sleep(500);
            auto devices = g_aim.ListDevices();
            bool found = false;
            for (auto& d : devices) if (d.number == opts.device) { found = true; break; }
            if (!found) { printf("Daemon unmounted successfully.\n"); return 0; }
        }
        printf("Daemon did not unmount within timeout. Forcing...\n");
    }

    // No daemon — do it ourselves
    if (!opts.noSave) { SaveOptions so; so.device = opts.device; CmdSave(so); }
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
        std::string path = d.backingFile;
        if (StartsWith(path, "\\??\\")) path = path.substr(4);
        if (path.size() > 4 && path.substr(path.size() - 4) == ".raw")
            path = path.substr(0, path.size() - 4);
        printf("[%d] %s -> %s (%s)\n", d.number,
               path.empty() ? "(no file)" : path.c_str(),
               d.physicalDrive.c_str(), FormatSize(d.sizeBytes).c_str());
    }
    return 0;
}

static int CmdTray() {
    TrayIcon tray;
    tray.Create(GetModuleHandle(nullptr), "RamDisk CLI");
    tray.OnOpenDisk([]() { if (!g_mountPoint.empty()) ShellExecuteA(nullptr, "open", g_mountPoint.c_str(), nullptr, nullptr, SW_SHOW); });
    tray.OnSave([]() { DoSave(); });
    tray.OnExit([]() { PostQuitMessage(0); });
    tray.Run();
    return 0;
}

int main(int argc, char* argv[]) {
    g_origArgc = argc; g_origArgv = argv;
    g_aimApi.Load(); // non-fatal
    // Filter --internal-daemon before parsing
    std::vector<char*> filteredArgs;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--internal-daemon") != 0 &&
            strcmp(argv[i], "--internal-proxy-server") != 0)
            filteredArgs.push_back(argv[i]);
    }
    auto r = ParseArgs((int)filteredArgs.size(), filteredArgs.data());
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
    case Command::Tray:     return CmdTray();
    case Command::Help:     PrintHelp(); return 0;
    case Command::Version:  PrintVersion(); return 0;
    default:                PrintHelp(); return 0;
    }
}
