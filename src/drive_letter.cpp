#include "drive_letter.h"
#include "utils.h"
#include <windows.h>
#include <cstdio>
#include <vector>

std::string AssignDriveLetter(uint64_t diskSizeBytes) {
    // Find the disk number
    int diskNum = -1;
    for (int attempt = 0; attempt < 15; attempt++) {
        for (int d = 0; d < 16; d++) {
            char path[64];
            snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", d);
            HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            GET_LENGTH_INFORMATION gli;
            DWORD rd = 0;
            if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                                &gli, sizeof(gli), &rd, nullptr)) {
                if (gli.Length.QuadPart == (LONGLONG)diskSizeBytes) { diskNum = d; CloseHandle(h); break; }
            }
            CloseHandle(h);
        }
        if (diskNum >= 0) break;
        Sleep(1000);
    }
    if (diskNum < 0) { LogError("Disk not found for drive letter assignment"); return ""; }

    // Find first available drive letter
    DWORD drives = GetLogicalDrives();
    char letter = 'D';
    for (char c = 'R'; c <= 'Z'; c++) { if (!(drives & (1 << (c - 'A')))) { letter = c; break; } }

    // Use diskpart to assign letter
    char tmpPath[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    char scriptPath[MAX_PATH];
    snprintf(scriptPath, sizeof(scriptPath), "%s\\ramdisk_assign.txt", tmpPath);

    FILE* f = fopen(scriptPath, "w");
    if (!f) return "";
    fprintf(f, "select disk %d\nselect partition 1\nassign letter=%c\nexit\n", diskNum, letter);
    fclose(f);

    char cmdLine[512];
    snprintf(cmdLine, sizeof(cmdLine), "diskpart /s \"%s\"", scriptPath);
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    remove(scriptPath);

    drives = GetLogicalDrives();
    char result[4];
    snprintf(result, sizeof(result), "%c:", letter);
    if (drives & (1 << (letter - 'A'))) { LogInfo("Drive letter assigned: " + std::string(result)); return result; }
    LogError("Failed to assign drive letter " + std::string(result));
    return "";
}
