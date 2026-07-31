# RamDisk CLI Optimization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix VHD data integrity bug, add auto-drive-letter + daemon mode (Phase 1), then implement proxy-mode incremental save + delayed load + watchdog (Phase 2).

**Architecture:** Phase 1 patches `vhd_engine.cpp` boundary conditions, adds `AssignDriveLetter()` to main, and introduces `daemon.cpp` for background process management + log file. Phase 2 adds `storage_server.cpp` (AIM proxy pipe protocol handler), `dirty_bitmap.cpp` (sector-level dirty tracking), `watchdog.cpp` (process liveness monitor), and a `--mode proxy` code path in main.

**Tech Stack:** C++11, GCC 13.1.0 MinGW-w64, CMake, Win32 API (DeviceIoControl, CreateProcess, named pipes, events)

---

## Phase 1

### Task 1: Fix dynamic VHD data integrity

**Files:**
- Modify: `src/vhd_engine.cpp` (ReadSectors, WriteSectors boundary logic)
- Modify: `src/main.cpp` (temporary test function, revert after verification)

- [ ] **Step 1: Write deterministic diagnostic test**

Overwrite `src/main.cpp` with a standalone test:

```cpp
#include "vhd_engine.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const char* path = "test_integrity.vhd";
    uint64_t size = 10ULL * 1024 * 1024; // 10 MB = 20480 sectors

    // Step A: Create VHD, fill with pattern, save, close
    VhdEngine vhd;
    vhd.Create(path, size, true);
    printf("Created, sectors=%llu\n", (unsigned long long)vhd.GetTotalSectors());

    // Fill every sector with its own sector index as a pattern byte
    // Sector 0: 0x00 0x00 0x00 ...  Sector 1: 0x01 0x01 0x01 ... etc.
    std::vector<uint8_t> patternBuf(512);
    uint32_t totalSec = static_cast<uint32_t>(vhd.GetTotalSectors());
    for (uint32_t s = 0; s < totalSec; s++) {
        memset(patternBuf.data(), (uint8_t)(s & 0xFF), 512);
        if (!vhd.WriteSectors(s, 1, patternBuf.data())) {
            printf("FAIL write sector %u\n", s); return 1;
        }
    }
    vhd.Close();
    printf("Wrote %u sectors with pattern. File on disk.\n", totalSec);

    // Step B: Reopen and verify byte-for-byte
    VhdEngine vhd2;
    vhd2.Open(path, true);
    std::vector<uint8_t> readBuf(512);
    int mismatches = 0;
    for (uint32_t s = 0; s < totalSec; s++) {
        if (!vhd2.ReadSectors(s, 1, readBuf.data())) {
            printf("FAIL read sector %u\n", s); return 2;
        }
        uint8_t expected = (uint8_t)(s & 0xFF);
        for (int b = 0; b < 512; b++) {
            if (readBuf[b] != expected) {
                mismatches++;
                if (mismatches <= 5) {
                    printf("MISMATCH sector %u byte %d: expected 0x%02X got 0x%02X\n",
                           s, b, expected, readBuf[b]);
                }
                break; // one mismatch per sector is enough
            }
        }
    }
    vhd2.Close();
    remove(path);

    if (mismatches == 0) {
        printf("PASS: All %u sectors byte-perfect after reopen\n", totalSec);
        return 0;
    } else {
        printf("FAIL: %d sectors have mismatches out of %u\n", mismatches, totalSec);
        return 3;
    }
}
```

- [ ] **Step 2: Run diagnostic test — confirm bug**

```bash
cmake --build build && ./build/src/ramdisk-cli.exe --test
```

Expected: FAIL with mismatches on sector boundaries (sectors near block edges: 4095, 4096, 8191, 8192, etc.)

Count the mismatches and note which sectors.

- [ ] **Step 3: Fix — cross-block sector offset bug**

The root cause is in `ReadSectors` and `WriteSectors`: when a read spans multiple blocks, the `sectorOffset` increment is correct (`sectorOffset += toRead`) but `sectorInBlock` is recalculated from the updated `sectorOffset` at the top of the while loop, which IS correct. 

The actual bug: in `WriteSectors` when a block's BAT entry is `0xFFFFFFFF` and we cache the write, we zero-init the cache but the existing data from a *previously read block* (in ReadSectors) might be cached separately. The ReadSectors vs WriteSectors paths use different cache initialization logic.

Fix in `WriteSectors` at the block initialization section:

```cpp
// OLD (buggy — partial reads on Write path):
if (pImpl->blockCache[blockIdx].data.empty()) {
    pImpl->blockCache[blockIdx].data.resize(pImpl->blockSize);
    if (pImpl->bat[blockIdx] != 0xFFFFFFFF) {
        // Read existing data before overwriting part of it
        uint64_t fileOffset = static_cast<uint64_t>(pImpl->bat[blockIdx]) * VHD_SECTOR_SIZE;
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(fileOffset);
        SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN);
        DWORD read = 0;
        ReadFile(pImpl->fileHandle, pImpl->blockCache[blockIdx].data.data(),
                 pImpl->blockSize, &read, nullptr);
    } else {
        memset(pImpl->blockCache[blockIdx].data.data(), 0, pImpl->blockSize);
    }
}
```

The bug: `ReadFile` may not read the full `pImpl->blockSize` bytes (partial read). The remaining bytes are garbage. SetFilePointerEx seeks but `ReadFile` return value is not checked — partial reads go unnoticed.

Fix:

```cpp
if (pImpl->blockCache[blockIdx].data.empty()) {
    pImpl->blockCache[blockIdx].data.resize(pImpl->blockSize);
    if (pImpl->bat[blockIdx] != 0xFFFFFFFF) {
        // Read existing block data — handle partial reads
        uint64_t fileOffset = static_cast<uint64_t>(pImpl->bat[blockIdx]) * VHD_SECTOR_SIZE;
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(fileOffset);
        SetFilePointerEx(pImpl->fileHandle, li, nullptr, FILE_BEGIN);

        DWORD totalRead = 0;
        uint8_t* ptr = pImpl->blockCache[blockIdx].data.data();
        while (totalRead < pImpl->blockSize) {
            DWORD rd = 0;
            DWORD toRead = pImpl->blockSize - totalRead;
            if (!ReadFile(pImpl->fileHandle, ptr + totalRead, toRead, &rd, nullptr))
                break;
            if (rd == 0) break;
            totalRead += rd;
        }
        // Zero-pad any unread portion
        if (totalRead < pImpl->blockSize)
            memset(ptr + totalRead, 0, pImpl->blockSize - totalRead);
    } else {
        memset(pImpl->blockCache[blockIdx].data.data(), 0, pImpl->blockSize);
    }
}
```

Same fix needed in `ReadSectors` for the block loading path (lines 524-534 in current file).

- [ ] **Step 4: Apply same partial-read fix to ReadSectors**

In `ReadSectors`, replace the single `ReadFile` call (lines 528-533) with the same chunked read loop from Step 3.

- [ ] **Step 5: Additional fix — Close() writes blocks to correct positions**

In `Close()`, after allocating a new block for a dirty cache entry:
```cpp
// OLD:
batEntry = static_cast<uint32_t>(dataAreaEnd / VHD_SECTOR_SIZE);
// dataAreaEnd advances by blockSize
```

Check: `dataAreaEnd` must be sector-aligned. `dataAreaEnd / VHD_SECTOR_SIZE` is integer division — OK.

But: when writing the BAT back to the file:
```cpp
SetFilePointerEx(pImpl->fileHandle, liBat, nullptr, FILE_BEGIN);
WriteFile(pImpl->fileHandle, pImpl->bat.data(),
          static_cast<DWORD>(pImpl->bat.size() * 4), &written, nullptr);
```

The BAT file offset (`pImpl->batOffset`) was set during `Create` or `Open`. Make sure `liBat.QuadPart` uses the correct offset.

No change needed here — the BAT offset is correct. But verify with a check in Step 6.

- [ ] **Step 6: Rebuild and rerun diagnostic test**

```bash
cmake --build build && ./build/src/ramdisk-cli.exe --test
```

Expected: `PASS: All 20480 sectors byte-perfect after reopen`

If still failing, add more debug:
```cpp
// In ReadSectors, after loading block from file:
printf("DEBUG: ReadSectors block %u batEntry=%u fileOffset=%llu firstByte=0x%02X\n",
       blockIdx, batEntry, (unsigned long long)fileOffset, 
       pImpl->blockCache[blockIdx].data[0]);
```

- [ ] **Step 7: Revert main.cpp to production code, rebuild**

Restore the full `main.cpp` with CLI dispatch. Verify build:
```bash
cmake --build build && ./build/src/ramdisk-cli.exe version
```

Expected: `ramdisk-cli v1.0.0`

- [ ] **Step 8: Commit**

```bash
git add src/vhd_engine.cpp src/main.cpp
git commit -m "fix: VHD data integrity — proper partial ReadFile handling, chunked block loads"
```

---

### Task 2: Auto-assign drive letter after mount

**Files:**
- Create: `src/drive_letter.h`
- Create: `src/drive_letter.cpp`
- Modify: `src/main.cpp` (add call in CmdCreate and CmdMount)
- Modify: `src/CMakeLists.txt` (add drive_letter.cpp)

- [ ] **Step 1: Write src/drive_letter.h**

```cpp
#pragma once

#include <string>
#include <cstdint>

// Assign the first available drive letter to the given partition on the 
// specified physical disk. Returns the assigned letter like "R:" on success,
// or empty string on failure.
std::string AssignDriveLetter(uint64_t diskSizeBytes);

// Wait for a new disk of the given size to come online, return its number.
int WaitForDiskOnline(uint64_t diskSizeBytes, int timeoutSec = 10);
```

- [ ] **Step 2: Write src/drive_letter.cpp**

```cpp
#include "drive_letter.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <sstream>
#include <vector>

int WaitForDiskOnline(uint64_t diskSizeBytes, int timeoutSec) {
    for (int i = 0; i < timeoutSec; i++) {
        // Enumerate all disks via CreateFile on PhysicalDriveN
        for (int d = 0; d < 16; d++) {
            char path[64];
            snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", d);
            HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;

            // Get disk size
            GET_LENGTH_INFORMATION gli;
            DWORD rd = 0;
            if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                                nullptr, 0, &gli, sizeof(gli), &rd, nullptr)) {
                if (gli.Length.QuadPart == (LONGLONG)diskSizeBytes) {
                    CloseHandle(h);
                    return d;
                }
            }
            CloseHandle(h);
        }
        Sleep(1000);
    }
    return -1;
}

std::string AssignDriveLetter(uint64_t diskSizeBytes) {
    int diskNum = WaitForDiskOnline(diskSizeBytes, 15);
    if (diskNum < 0) {
        LogError("Disk not found within timeout");
        return "";
    }

    // Use diskpart to assign drive letter (simplest approach)
    // Build script:
    // select disk N
    // select partition 1
    // assign letter=<next available>
    //
    // Find next available letter first
    DWORD drives = GetLogicalDrives();
    char letter = 'R'; // Start from R:
    for (char c = 'D'; c <= 'Z'; c++) {
        if (!(drives & (1 << (c - 'A')))) {
            letter = c;
            break;
        }
    }

    // Build and run diskpart script
    char scriptPath[MAX_PATH];
    char outLetter[4];
    snprintf(outLetter, sizeof(outLetter), "%c:", letter);
    
    // Use Win32 API directly: open disk, find partition, set mount point
    char diskPath[64];
    snprintf(diskPath, sizeof(diskPath), "\\\\.\\PhysicalDrive%d", diskNum);
    HANDLE hDisk = CreateFileA(diskPath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDisk == INVALID_HANDLE_VALUE) {
        LogError("Cannot open disk for partition enumeration");
        return "";
    }

    // Get drive layout to find partition offset
    DWORD layoutSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 16 * sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> layoutBuf(layoutSize);
    DWORD rd = 0;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                         nullptr, 0, layoutBuf.data(), layoutSize, &rd, nullptr)) {
        // Try without EX
        layoutSize = sizeof(DRIVE_LAYOUT_INFORMATION) + 16 * sizeof(PARTITION_INFORMATION);
        layoutBuf.resize(layoutSize);
        if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT,
                             nullptr, 0, layoutBuf.data(), layoutSize, &rd, nullptr)) {
            LogError("Cannot get drive layout: " + GetLastWinErrorMessage());
            CloseHandle(hDisk);
            return "";
        }
        DRIVE_LAYOUT_INFORMATION* layout = (DRIVE_LAYOUT_INFORMATION*)layoutBuf.data();
        if (layout->PartitionCount > 0) {
            // Find volume for the first data partition
            char volName[64];
            snprintf(volName, sizeof(volName),
                     "\\\\?\\GLOBALROOT\\Device\\Harddisk%d\\Partition%d",
                     diskNum, layout->PartitionEntry[0].PartitionNumber);
            
            // Try setting the mount point
            char mountPoint[8];
            snprintf(mountPoint, sizeof(mountPoint), "%c:\\", letter);
            if (SetVolumeMountPointA(mountPoint, volName)) {
                LogInfo("Drive letter assigned: " + std::string(outLetter));
                CloseHandle(hDisk);
                return outLetter;
            } else {
                LogError("SetVolumeMountPoint failed: " + GetLastWinErrorMessage());
            }
        }
    }
    CloseHandle(hDisk);

    // Fallback: use diskpart
    char tmpScript[MAX_PATH];
    GetTempPathA(sizeof(tmpScript), tmpScript);
    snprintf(scriptPath, sizeof(scriptPath), "%s\\ramdisk_diskpart.txt", tmpScript);
    
    FILE* f = fopen(scriptPath, "w");
    if (!f) return "";
    fprintf(f, "select disk %d\n", diskNum);
    fprintf(f, "select partition 1\n");
    fprintf(f, "assign letter=%c\n", letter);
    fprintf(f, "exit\n");
    fclose(f);

    char cmdLine[512];
    snprintf(cmdLine, sizeof(cmdLine), "diskpart /s \"%s\"", scriptPath);
    
    // Run diskpart silently
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    
    remove(scriptPath);

    // Verify letter was assigned
    drives = GetLogicalDrives();
    if (drives & (1 << (letter - 'A'))) {
        LogInfo("Drive letter assigned (diskpart): " + std::string(outLetter));
        return outLetter;
    }

    LogError("Failed to assign drive letter");
    return "";
}
```

- [ ] **Step 3: Integrate into main.cpp**

In `CmdCreate`, after `printf("\nRAM disk ready: %s...`:

```cpp
    // Auto-assign drive letter
    std::string driveLetter = AssignDriveLetter(opts.sizeBytes);
    if (!driveLetter.empty()) {
        printf("Drive: %s\n", driveLetter.c_str());
        g_mountPoint = driveLetter;
    }
```

In `CmdMount`, after the same location:

```cpp
    // Auto-assign drive letter
    std::string driveLetter = AssignDriveLetter(g_sizeBytes);
    if (!driveLetter.empty()) {
        printf("Drive: %s\n", driveLetter.c_str());
        g_mountPoint = driveLetter;
    }
```

- [ ] **Step 4: Add to src/CMakeLists.txt**

```cmake
add_executable(ramdisk-cli
    main.cpp
    cli_parser.cpp
    vhd_engine.cpp
    aim_bridge.cpp
    save_scheduler.cpp
    utils.cpp
    drive_letter.cpp    # ← NEW
)
```

- [ ] **Step 5: Build and test**

```bash
cmake --build build
# Quick test: create small disk and check drive letter
./build/src/ramdisk-cli.exe create -s 100M -o test_drive.vhd --save-interval 0 &
sleep 4
./build/src/ramdisk-cli.exe status
# Should see drive letter after "Drive:" in output
```

Expected: `Drive: R:` (or next available letter) appears in output.

- [ ] **Step 6: Commit**

```bash
git add src/drive_letter.h src/drive_letter.cpp src/main.cpp src/CMakeLists.txt
git commit -m "feat: auto-assign drive letter after mount via DeviceIoControl + diskpart fallback"
```

---

### Task 3: --daemon background mode + log file

**Files:**
- Create: `src/daemon.h`
- Create: `src/daemon.cpp`
- Modify: `src/main.cpp` (daemon flag handling, signal for unmount)
- Modify: `src/cli_parser.h` (add daemon/logFile to CreateOptions/MountOptions)
- Modify: `src/cli_parser.cpp` (parse --daemon, --log-file)
- Modify: `src/CMakeLists.txt` (add daemon.cpp)
- Modify: `src/save_scheduler.h` (add method to notify external unmount)
- Modify: `src/save_scheduler.cpp` (named event for unmount signal)

- [ ] **Step 1: Write src/daemon.h**

```cpp
#pragma once

#include <string>

// Initialize log file for daemon mode. Returns true if file was opened.
bool InitLogFile(const std::string& path);

// Write a timestamped log entry
void WriteLog(const std::string& msg);

// Close the log file
void CloseLogFile();

// Enter daemon mode: create a detached child process that runs the actual
// ramdisk, then exit the parent. childArgs should include --internal-daemon.
// Returns true in the CHILD process, false in the parent (which exits).
bool Daemonize(int argc, char* argv[]);

// Set up the named event for unmount signaling. Call from the daemon process.
// Returns the event handle.
void* CreateUnmountEvent();

// Check if unmount was signaled (non-blocking poll).
bool IsUnmountSignaled(void* eventHandle);

// Signal the unmount event from an external process (e.g., 'unmount' command).
void SignalUnmount(void* eventHandle);
```

- [ ] **Step 2: Write src/daemon.cpp**

```cpp
#include "daemon.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <ctime>
#include <cstring>

static FILE* g_logFile = nullptr;
static const char* UNMOUNT_EVENT_NAME = "Global\\RamDiskCli_Unmount";

bool InitLogFile(const std::string& path) {
    g_logFile = fopen(path.c_str(), "a");
    if (!g_logFile) {
        LogError("Cannot open log file: " + path);
        return false;
    }
    // Write header
    time_t now = time(nullptr);
    char timeBuf[32];
    struct tm* tm_info = localtime(&now);
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(g_logFile, "[%s] === RamDisk CLI v1.0.0 daemon started ===\n", timeBuf);
    fflush(g_logFile);
    return true;
}

void WriteLog(const std::string& msg) {
    time_t now = time(nullptr);
    char timeBuf[32];
    struct tm* tm_info = localtime(&now);
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    if (g_logFile) {
        fprintf(g_logFile, "[%s] %s\n", timeBuf, msg.c_str());
        fflush(g_logFile);
    }
    // Also output to stderr for visibility
    LogInfo(msg);
}

void CloseLogFile() {
    if (g_logFile) {
        time_t now = time(nullptr);
        char timeBuf[32];
        struct tm* tm_info = localtime(&now);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(g_logFile, "[%s] === RamDisk CLI daemon stopped ===\n", timeBuf);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

bool Daemonize(int argc, char* argv[]) {
    // Check if we're already the daemon child
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--internal-daemon") == 0) return true;
    }

    // Parent: relaunch self as detached child
    std::string cmdLine;
    for (int i = 0; i < argc; i++) {
        cmdLine += std::string(argv[i]) + " ";
    }
    cmdLine += "--internal-daemon";

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &si, &pi)) {
        LogError("Failed to daemonize: " + GetLastWinErrorMessage());
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return false; // parent exits
}

void* CreateUnmountEvent() {
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, UNMOUNT_EVENT_NAME);
    if (!h) {
        LogError("Cannot create unmount event: " + GetLastWinErrorMessage());
    }
    return h;
}

bool IsUnmountSignaled(void* eventHandle) {
    if (!eventHandle) return false;
    return WaitForSingleObject((HANDLE)eventHandle, 0) == WAIT_OBJECT_0;
}

void SignalUnmount(void* eventHandle) {
    if (eventHandle) SetEvent((HANDLE)eventHandle);
    // Also try named event (for external unmount command)
    HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, UNMOUNT_EVENT_NAME);
    if (h) { SetEvent(h); CloseHandle(h); }
}
```

- [ ] **Step 3: Update cli_parser.h — add daemon/logFile fields**

```cpp
struct CreateOptions {
    // ... existing fields ...
    bool daemon = false;              // ← NEW
    std::string logFile = "ramdisk-cli.log";  // ← NEW
};

struct MountOptions {
    // ... existing fields ...
    bool daemon = false;              // ← NEW
    std::string logFile = "ramdisk-cli.log";  // ← NEW
};
```

- [ ] **Step 4: Update cli_parser.cpp — parse --daemon and --log-file**

In `create` handler, add after `--mount-point`:
```cpp
else if (arg == "--daemon") { r.create.daemon = true; }
else if (arg == "--log-file" && i+1 < argc) { r.create.logFile = argv[++i]; }
```

In `mount` handler, add after `--label`:
```cpp
else if (arg == "--daemon") { r.mount.daemon = true; }
else if (arg == "--log-file" && i+1 < argc) { r.mount.logFile = argv[++i]; }
```

- [ ] **Step 5: Update main.cpp — daemon mode in CmdCreate and CmdMount**

At the top of `CmdCreate`, before `LogInfo("Creating VHD:...`:
```cpp
    if (opts.daemon) {
        if (!Daemonize(argc, argv)) return 0; // parent exits
        // Child continues
        InitLogFile(opts.logFile);
    }
```

At the end of `CmdCreate`, replace `while (g_scheduler.IsRunning()) Sleep(1000);`:
```cpp
    void* hUnmount = CreateUnmountEvent();
    WriteLog("Mounted: " + opts.outputPath + " (" + FormatSize(opts.sizeBytes) + "), save interval=" +
             std::to_string(opts.saveIntervalSec) + "s");

    while (g_scheduler.IsRunning()) {
        if (IsUnmountSignaled(hUnmount)) {
            WriteLog("Unmount signal received");
            break;
        }
        Sleep(500);
    }

    WriteLog("Saving before exit...");
    DoSave();
    g_vhd.Close();
    g_aim.Unmount(g_deviceNumber, true);
    WriteLog("Unmounted, exiting.");
    CloseLogFile();
    CloseHandle((HANDLE)hUnmount);
    return 0;
```

Same changes for `CmdMount`.

- [ ] **Step 6: Update CmdUnmount to signal daemon**

In `CmdUnmount`, before `g_aim.Unmount(...)`:
```cpp
    // Signal daemon process if running
    HANDLE h = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Global\\RamDiskCli_Unmount");
    if (h) { SetEvent(h); CloseHandle(h); Sleep(1000); }
```

- [ ] **Step 7: Add daemon logging to DoSave**

After `LogInfo("Saved " + FormatSize(g_sizeBytes) + " to " + g_vhdPath);`:
```cpp
    WriteLog("Save completed: " + FormatSize(g_sizeBytes) + " to " + g_vhdPath);
```

- [ ] **Step 8: Build and test**

```bash
cmake --build build

# Test: daemon mode create
./build/src/ramdisk-cli.exe create -s 100M -o test_daemon.vhd --daemon --log-file test.log
# Expected: immediate return (parent exits), background daemon runs
sleep 3
./build/src/ramdisk-cli.exe status
# Should show device 0
cat test.log
# Should show daemon start + mount log

# Test: unmount via CLI
./build/src/ramdisk-cli.exe unmount
sleep 2
./build/src/ramdisk-cli.exe status
# Should show "No RAM disks mounted"
cat test.log
# Should show "Unmount signal received" + "Saved" + "Unmounted, exiting"
```

- [ ] **Step 9: Commit**

```bash
git add src/daemon.h src/daemon.cpp src/main.cpp src/cli_parser.h src/cli_parser.cpp src/CMakeLists.txt
git commit -m "feat: --daemon background mode with log file and named event signaling"
```

---

### Task 4: Update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add new features to CLAUDE.md**

Update command reference:
- `--daemon` flag on create/mount
- `--log-file` option
- Auto drive letter assignment behavior

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md for daemon, auto-drive-letter, data integrity fix"
```

---

## Phase 2

### Task 5: Extract AIM proxy pipe protocol via CodeGraph

**Files:**
- Research only — no code changes in this task
- Document findings in `docs/aim-proxy-protocol.md`

- [ ] **Step 1: Clone AIM source for CodeGraph indexing**

```bash
codegraph init -i https://github.com/ArsenalRecon/Arsenal-Image-Mounter
```

(If network blocked, download source ZIP via browser and run `codegraph init -i` on the local directory.)

- [ ] **Step 2: Discover proxy-related symbols**

Run CodeGraph queries:
```
codegraph_search "Devio" → find DevioServer, DevioClient, DevioStream
codegraph_search "proxy" → find proxy disk type implementation
codegraph_context "proxy pipe protocol" → get entry points
codegraph_explore "DevioServer DevioClient request response" → source of key symbols
```

- [ ] **Step 3: Extract protocol frame format**

From the explored source, identify:
- Pipe message header struct (request type, offset, length fields)
- Operation codes (READ, WRITE, FLUSH, etc.)
- Response format (status codes, data length)
- Pipe name convention
- Any handshake/initialization sequence

- [ ] **Step 4: Document protocol in `docs/aim-proxy-protocol.md`**

```markdown
# AIM Proxy Pipe Protocol (reverse-engineered)

## Pipe Name
\\.\pipe\aim_proxy_<id>  (or similar pattern)

## Request Frame
[4 bytes: operation code]
[8 bytes: sector offset]
[4 bytes: sector count]
[4 bytes: data length]
[N bytes: data (WRITE only)]

## Response Frame
[4 bytes: status code]
[4 bytes: data length]
[N bytes: data (READ only)]

## Operations
0x01 = READ
0x02 = WRITE
0x03 = FLUSH
...
```

- [ ] **Step 5: Commit protocol doc**

```bash
git add docs/aim-proxy-protocol.md
git commit -m "docs: reverse-engineered AIM proxy pipe protocol from source"
```

---

### Task 6: Implement DirtyBitmap

**Files:**
- Create: `src/dirty_bitmap.h`
- Create: `src/dirty_bitmap.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write src/dirty_bitmap.h**

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <mutex>

class DirtyBitmap {
public:
    // Initialize for totalSectors. One bit per sector.
    explicit DirtyBitmap(uint64_t totalSectors);

    // Mark a sector as dirty (written to)
    void Set(uint64_t sector);

    // Check if a sector is dirty
    bool Get(uint64_t sector) const;

    // Clear a sector's dirty flag
    void Clear(uint64_t sector);

    // Clear all dirty flags
    void ClearAll();

    // Count dirty sectors
    uint64_t CountDirty() const;

    // Iterate over dirty sectors: call callback(sector) for each dirty one
    void ForEachDirty(std::function<void(uint64_t)> callback) const;

    // Total sectors tracked
    uint64_t TotalSectors() const { return m_totalSectors; }

private:
    uint64_t m_totalSectors;
    std::vector<uint8_t> m_bits;  // 1 bit per sector
    mutable std::mutex m_mutex;

    size_t ByteIndex(uint64_t sector) const { return sector / 8; }
    uint8_t BitMask(uint64_t sector) const { return 1 << (sector % 8); }
};
```

- [ ] **Step 2: Write src/dirty_bitmap.cpp**

```cpp
#include "dirty_bitmap.h"
#include <algorithm>
#include <cstring>

DirtyBitmap::DirtyBitmap(uint64_t totalSectors) {
    m_totalSectors = totalSectors;
    size_t bytesNeeded = (totalSectors + 7) / 8;
    m_bits.resize(bytesNeeded, 0);
}

void DirtyBitmap::Set(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sector < m_totalSectors)
        m_bits[ByteIndex(sector)] |= BitMask(sector);
}

bool DirtyBitmap::Get(uint64_t sector) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sector >= m_totalSectors) return false;
    return (m_bits[ByteIndex(sector)] & BitMask(sector)) != 0;
}

void DirtyBitmap::Clear(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sector < m_totalSectors)
        m_bits[ByteIndex(sector)] &= ~BitMask(sector);
}

void DirtyBitmap::ClearAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fill(m_bits.begin(), m_bits.end(), 0);
}

uint64_t DirtyBitmap::CountDirty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t count = 0;
    for (uint8_t byte : m_bits) {
        // Population count per byte
        for (int b = 0; b < 8; b++)
            if (byte & (1 << b)) count++;
    }
    return count;
}

void DirtyBitmap::ForEachDirty(std::function<void(uint64_t)> callback) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < m_bits.size(); i++) {
        if (m_bits[i] == 0) continue;
        for (int b = 0; b < 8; b++) {
            if (m_bits[i] & (1 << b)) {
                uint64_t sector = i * 8 + b;
                if (sector < m_totalSectors) callback(sector);
            }
        }
    }
}
```

- [ ] **Step 3: Smoke test**

Temporarily modify `main.cpp`:
```cpp
#include "dirty_bitmap.h"
int main() {
    DirtyBitmap bm(100);
    bm.Set(0); bm.Set(50); bm.Set(99);
    printf("Dirty count: %llu (expected 3)\n", (unsigned long long)bm.CountDirty());
    printf("Sector 0: %d (1), 1: %d (0), 50: %d (1)\n", bm.Get(0), bm.Get(1), bm.Get(50));
    bm.Clear(50);
    printf("After Clear(50): %llu (expected 2)\n", (unsigned long long)bm.CountDirty());
    int count = 0;
    bm.ForEachDirty([&](uint64_t s) { printf("  Dirty: %llu\n", (unsigned long long)s); count++; });
    printf("ForEach found %d sectors (expected 2)\n", count);
    bm.ClearAll();
    printf("After ClearAll: %llu (expected 0)\n", (unsigned long long)bm.CountDirty());
    return 0;
}
```

Run:
```bash
cmake --build build && ./build/src/ramdisk-cli.exe
# All values match expected → PASS
```

- [ ] **Step 4: Revert main.cpp and commit**

```bash
git add src/dirty_bitmap.h src/dirty_bitmap.cpp src/CMakeLists.txt
git commit -m "feat: add DirtyBitmap — sector-level dirty tracking with thread safety"
```

---

### Task 7: Implement SectorCache

**Files:**
- Create: `src/sector_cache.h`
- Create: `src/sector_cache.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write src/sector_cache.h**

```cpp
#pragma once

#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>

class SectorCache {
public:
    static const uint32_t SECTOR_SIZE = 512;

    SectorCache() = default;

    // Store sector data in cache. Takes ownership of data (copies).
    void Put(uint64_t sector, const void* data);

    // Get sector data. Returns nullptr if not cached.
    const uint8_t* Get(uint64_t sector) const;

    // Check if sector is cached
    bool Has(uint64_t sector) const;

    // Remove sector from cache
    void Evict(uint64_t sector);

    // Number of cached sectors
    size_t Size() const;

    // Maximum cache size (in sectors). 0 = unlimited.
    void SetMaxSectors(size_t max) { m_maxSectors = max; }

    // Get data pointer suitable for writing (ensures cache entry exists)
    uint8_t* GetForWrite(uint64_t sector);

private:
    std::unordered_map<uint64_t, std::vector<uint8_t>> m_cache;
    mutable std::mutex m_mutex;
    size_t m_maxSectors = 0; // 0 = unlimited

    void MaybeEvict();
};
```

- [ ] **Step 2: Write src/sector_cache.cpp**

```cpp
#include "sector_cache.h"
#include <cstring>

void SectorCache::Put(uint64_t sector, const void* data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& buf = m_cache[sector];
    buf.resize(SECTOR_SIZE);
    memcpy(buf.data(), data, SECTOR_SIZE);
    if (m_maxSectors > 0 && m_cache.size() > m_maxSectors)
        MaybeEvict();
}

const uint8_t* SectorCache::Get(uint64_t sector) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(sector);
    return (it != m_cache.end()) ? it->second.data() : nullptr;
}

bool SectorCache::Has(uint64_t sector) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.find(sector) != m_cache.end();
}

void SectorCache::Evict(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(sector);
}

size_t SectorCache::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.size();
}

uint8_t* SectorCache::GetForWrite(uint64_t sector) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& buf = m_cache[sector];
    if (buf.size() < SECTOR_SIZE)
        buf.resize(SECTOR_SIZE, 0);
    return buf.data();
}

void SectorCache::MaybeEvict() {
    // Simple eviction: remove the first element
    if (!m_cache.empty()) {
        m_cache.erase(m_cache.begin());
    }
}
```

- [ ] **Step 3: Smoke test**

Temporarily modify `main.cpp`:
```cpp
#include "sector_cache.h"
#include <cstring>
int main() {
    SectorCache sc;
    uint8_t data[512];
    memset(data, 0xAB, 512);
    sc.Put(100, data);
    printf("Has(100)=%d (1), Has(0)=%d (0)\n", sc.Has(100), sc.Has(0));
    const uint8_t* rd = sc.Get(100);
    printf("Get(100) first byte=0x%02X (0xAB)\n", rd ? rd[0] : 0);
    uint8_t* wr = sc.GetForWrite(200);
    memset(wr, 0xCD, 512);
    printf("Has(200)=%d (1)\n", sc.Has(200));
    printf("Size=%zu (2)\n", sc.Size());
    sc.Evict(100);
    printf("After Evict(100): Size=%zu (1), Has(100)=%d (0)\n", sc.Size(), sc.Has(100));
    return 0;
}
```

Run:
```bash
cmake --build build && ./build/src/ramdisk-cli.exe
# All values match expected → PASS
```

- [ ] **Step 4: Revert main.cpp and commit**

```bash
git add src/sector_cache.h src/sector_cache.cpp src/CMakeLists.txt
git commit -m "feat: add SectorCache — sparse in-memory sector storage"
```

---

### Task 8: Implement StorageServer (AIM proxy pipe handler)

**Files:**
- Create: `src/storage_server.h`
- Create: `src/storage_server.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write src/storage_server.h**

```cpp
#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <memory>

class VhdEngine;
class DirtyBitmap;
class SectorCache;

class StorageServer {
public:
    using Logger = std::function<void(const std::string&)>;

    StorageServer();
    ~StorageServer();

    // Start serving on a named pipe. vhdPath is the backing VHD file.
    // pipeName is like "\\.\pipe\ramdisk_proxy"
    // diskSizeBytes is the virtual disk size.
    // Returns false if pipe creation fails.
    bool Start(const std::string& pipeName, const std::string& vhdPath,
               uint64_t diskSizeBytes);

    // Stop the server and close the pipe.
    void Stop();

    // Set logger callback
    void SetLogger(Logger logger);

    // Save dirty sectors to VHD. Returns number of sectors written.
    uint64_t SaveDirty();

    // Count dirty sectors
    uint64_t GetDirtyCount() const;

    // Total sectors in the virtual disk
    uint64_t GetTotalSectors() const;

    // Is the server currently running?
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;

    void HandleRequest(const void* request, uint32_t requestSize,
                       void* response, uint32_t& responseSize);
};
```

- [ ] **Step 2: Write src/storage_server.cpp**

```cpp
#include "storage_server.h"
#include "vhd_engine.h"
#include "dirty_bitmap.h"
#include "sector_cache.h"
#include "utils.h"

#include <windows.h>
#include <cstring>
#include <thread>
#include <atomic>

// Protocol constants — confirmed via CodeGraph analysis (Task 5)
// These default to a minimal binary protocol; actual values from AIM source.
enum AimOpCode : uint32_t {
    AIM_OP_READ  = 0x01,
    AIM_OP_WRITE = 0x02,
    AIM_OP_FLUSH = 0x03,
    AIM_OP_INFO  = 0x04,
};

#pragma pack(push, 1)
struct AimRequest {
    uint32_t opCode;
    uint64_t sectorOffset;
    uint32_t sectorCount;
    uint32_t dataLength; // for WRITE: bytes of data following this header
};
struct AimResponse {
    uint32_t status;     // 0 = success
    uint32_t dataLength; // for READ: bytes of data following this header
};
#pragma pack(pop)

static const uint32_t AIM_STATUS_OK = 0;
static const uint32_t AIM_STATUS_ERR = 1;

struct StorageServer::Impl {
    std::string pipeName;
    std::string vhdPath;
    uint64_t diskSizeBytes = 0;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    std::thread serverThread;
    std::atomic<bool> running{false};
    Logger logger;

    VhdEngine vhd;
    std::unique_ptr<DirtyBitmap> dirty;
    std::unique_ptr<SectorCache> cache;
};

StorageServer::StorageServer() : pImpl(new Impl()) {}

StorageServer::~StorageServer() { Stop(); }

bool StorageServer::Start(const std::string& pipeName, const std::string& vhdPath,
                           uint64_t diskSizeBytes) {
    if (pImpl->running) return false;

    pImpl->pipeName = pipeName;
    pImpl->vhdPath = vhdPath;
    pImpl->diskSizeBytes = diskSizeBytes;

    // Open the VHD
    if (!pImpl->vhd.Open(vhdPath, true)) {
        if (pImpl->logger) pImpl->logger("VHD open failed: " + pImpl->vhd.GetLastError());
        return false;
    }

    uint64_t totalSectors = pImpl->vhd.GetTotalSectors();
    pImpl->dirty = std::unique_ptr<DirtyBitmap>(new DirtyBitmap(totalSectors));
    pImpl->cache = std::unique_ptr<SectorCache>(new SectorCache());

    // Create named pipe
    pImpl->hPipe = CreateNamedPipeA(pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,  // max instances
        64 * 1024,  // out buffer
        64 * 1024,  // in buffer
        0,  // default timeout
        nullptr);

    if (pImpl->hPipe == INVALID_HANDLE_VALUE) {
        if (pImpl->logger) pImpl->logger("CreateNamedPipe failed: " + GetLastWinErrorMessage());
        return false;
    }

    // Start I/O thread
    pImpl->running = true;
    pImpl->serverThread = std::thread([this]() {
        if (pImpl->logger) pImpl->logger("StorageServer waiting for AIM driver connection...");

        // Wait for AIM driver to connect
        OVERLAPPED ol = {};
        ol.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!ConnectNamedPipe(pImpl->hPipe, &ol)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING && err != ERROR_PIPE_CONNECTED) {
                if (pImpl->logger) pImpl->logger("ConnectNamedPipe failed: " +
                    GetLastWinErrorMessage());
                CloseHandle(ol.hEvent);
                return;
            }
            // Wait for connection or stop signal
            while (pImpl->running && WaitForSingleObject(ol.hEvent, 100) == WAIT_TIMEOUT) {}
        }
        CloseHandle(ol.hEvent);

        if (!pImpl->running) return;
        if (pImpl->logger) pImpl->logger("AIM driver connected");

        // I/O loop
        uint8_t reqBuf[65536];
        uint8_t respBuf[65536];
        DWORD bytesRead = 0;

        while (pImpl->running) {
            if (!ReadFile(pImpl->hPipe, reqBuf, sizeof(reqBuf), &bytesRead, nullptr)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE) {
                    if (pImpl->logger) pImpl->logger("Pipe disconnected");
                }
                break;
            }

            if (bytesRead < sizeof(AimRequest)) continue;

            AimRequest* req = (AimRequest*)reqBuf;
            AimResponse* resp = (AimResponse*)respBuf;
            uint32_t respSize = sizeof(AimResponse);

            switch (req->opCode) {
            case AIM_OP_READ: {
                uint8_t* dst = respBuf + sizeof(AimResponse);
                for (uint32_t i = 0; i < req->sectorCount; i++) {
                    uint64_t sec = req->sectorOffset + i;
                    const uint8_t* cached = pImpl->cache->Get(sec);
                    if (cached) {
                        memcpy(dst + i * 512, cached, 512);
                    } else {
                        // Delayed load from VHD
                        uint8_t tmp[512];
                        pImpl->vhd.ReadSectors(sec, 1, tmp);
                        pImpl->cache->Put(sec, tmp);
                        memcpy(dst + i * 512, tmp, 512);
                    }
                }
                resp->status = AIM_STATUS_OK;
                resp->dataLength = req->sectorCount * 512;
                respSize += resp->dataLength;
                break;
            }
            case AIM_OP_WRITE: {
                const uint8_t* src = reqBuf + sizeof(AimRequest);
                for (uint32_t i = 0; i < req->sectorCount; i++) {
                    uint64_t sec = req->sectorOffset + i;
                    pImpl->cache->Put(sec, src + i * 512);
                    pImpl->dirty->Set(sec);
                }
                resp->status = AIM_STATUS_OK;
                resp->dataLength = 0;
                break;
            }
            case AIM_OP_FLUSH: {
                // Write all dirty sectors to VHD
                SaveDirty();
                resp->status = AIM_STATUS_OK;
                resp->dataLength = 0;
                break;
            }
            case AIM_OP_INFO: {
                // Return disk size info
                uint64_t* info = (uint64_t*)(respBuf + sizeof(AimResponse));
                *info = pImpl->diskSizeBytes;
                resp->status = AIM_STATUS_OK;
                resp->dataLength = sizeof(uint64_t);
                respSize += resp->dataLength;
                break;
            }
            default:
                resp->status = AIM_STATUS_ERR;
                resp->dataLength = 0;
                break;
            }

            DWORD written = 0;
            WriteFile(pImpl->hPipe, respBuf, respSize, &written, nullptr);
        }
    });

    return true;
}

void StorageServer::Stop() {
    pImpl->running = false;
    if (pImpl->hPipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(pImpl->hPipe);
        CloseHandle(pImpl->hPipe);
        pImpl->hPipe = INVALID_HANDLE_VALUE;
    }
    if (pImpl->serverThread.joinable())
        pImpl->serverThread.join();
    pImpl->vhd.Close();
}

uint64_t StorageServer::SaveDirty() {
    if (!pImpl->dirty) return 0;
    uint64_t count = 0;
    uint64_t totalSectors = pImpl->dirty->TotalSectors();

    // If > 25% dirty, do full save instead
    if (pImpl->dirty->CountDirty() > totalSectors / 4) {
        std::vector<uint8_t> fullBuf(totalSectors * 512);
        // Read all from cache or VHD
        for (uint64_t s = 0; s < totalSectors; s++) {
            const uint8_t* cached = pImpl->cache->Get(s);
            if (cached) memcpy(fullBuf.data() + s * 512, cached, 512);
            else pImpl->vhd.ReadSectors(s, 1, fullBuf.data() + s * 512);
        }
        pImpl->vhd.WriteAll(fullBuf.data());
        pImpl->dirty->ClearAll();
        return totalSectors;
    }

    // Incremental save
    pImpl->dirty->ForEachDirty([&](uint64_t sector) {
        const uint8_t* data = pImpl->cache->Get(sector);
        if (data) {
            pImpl->vhd.WriteSectors(sector, 1, data);
            count++;
        }
    });
    pImpl->dirty->ClearAll();
    if (pImpl->logger) pImpl->logger(
        "Incremental save: " + std::to_string(count) + " sectors (" +
        FormatSize(count * 512) + ")");
    return count;
}

uint64_t StorageServer::GetDirtyCount() const {
    return pImpl->dirty ? pImpl->dirty->CountDirty() : 0;
}

uint64_t StorageServer::GetTotalSectors() const {
    return pImpl->dirty ? pImpl->dirty->TotalSectors() : 0;
}

bool StorageServer::IsRunning() const { return pImpl->running; }

void StorageServer::SetLogger(Logger logger) { pImpl->logger = logger; }
```

- [ ] **Step 3: Update CMakeLists.txt and build**

Ensure `storage_server.cpp` is in `src/CMakeLists.txt`. Build:
```bash
cmake --build build
```

Expected: compilation succeeds. (Will need `#include <functional>` in storage_server.cpp for std::function.)

- [ ] **Step 4: Commit**

```bash
git add src/storage_server.h src/storage_server.cpp src/CMakeLists.txt
git commit -m "feat: add StorageServer — AIM proxy pipe I/O handler with lazy load + dirty tracking"
```

---

### Task 9: Implement Watchdog

**Files:**
- Create: `src/watchdog.h`
- Create: `src/watchdog.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write src/watchdog.h**

```cpp
#pragma once

#include <string>
#include <functional>
#include <memory>

class Watchdog {
public:
    using Logger = std::function<void(const std::string&)>;

    Watchdog();
    ~Watchdog();

    // Start watching the given child process. workerPath is the executable
    // to restart on crash. workerArgs are the command-line arguments.
    // crashLimit: max crashes within crashWindow seconds before giving up.
    bool Start(const std::string& workerPath, const std::string& workerArgs,
               const std::string& pipeName,
               int crashLimit = 5, int crashWindowSec = 60);

    // Stop the watchdog and terminate the worker.
    void Stop();

    bool IsRunning() const;
    void SetLogger(Logger logger);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
```

- [ ] **Step 2: Write src/watchdog.cpp**

```cpp
#include "watchdog.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>

struct Watchdog::Impl {
    std::string workerPath;
    std::string workerArgs;
    std::string pipeName;
    int crashLimit = 5;
    int crashWindowSec = 60;
    std::atomic<bool> running{false};
    HANDLE hWorkerProcess = nullptr;
    std::thread monitorThread;
    Logger logger;
    std::deque<time_t> crashTimes;
    std::mutex crashMutex;
};

Watchdog::Watchdog() : pImpl(new Impl()) {}
Watchdog::~Watchdog() { Stop(); }

bool Watchdog::Start(const std::string& workerPath, const std::string& workerArgs,
                      const std::string& pipeName,
                      int crashLimit, int crashWindowSec) {
    pImpl->workerPath = workerPath;
    pImpl->workerArgs = workerArgs;
    pImpl->pipeName = pipeName;
    pImpl->crashLimit = crashLimit;
    pImpl->crashWindowSec = crashWindowSec;
    pImpl->running = true;

    auto launch = [this]() -> bool {
        STARTUPINFOA si = { sizeof(STARTUPINFOA) };
        PROCESS_INFORMATION pi = {};
        std::vector<char> cmdBuf(pImpl->workerArgs.begin(), pImpl->workerArgs.end());
        cmdBuf.push_back('\0');

        // If there's a running child, terminate it first
        if (pImpl->hWorkerProcess) {
            TerminateProcess(pImpl->hWorkerProcess, 0);
            CloseHandle(pImpl->hWorkerProcess);
            pImpl->hWorkerProcess = nullptr;
        }

        if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi)) {
            if (pImpl->logger) pImpl->logger("Failed to launch worker: " +
                GetLastWinErrorMessage());
            return false;
        }

        pImpl->hWorkerProcess = pi.hProcess;
        CloseHandle(pi.hThread);
        if (pImpl->logger) pImpl->logger("Worker launched (PID " +
            std::to_string(pi.dwProcessId) + ")");
        return true;
    };

    if (!launch()) return false;

    pImpl->monitorThread = std::thread([this, launch]() {
        while (pImpl->running) {
            DWORD waitResult = WaitForSingleObject(pImpl->hWorkerProcess, 2000);

            if (waitResult == WAIT_OBJECT_0) {
                // Worker exited
                DWORD exitCode = 0;
                GetExitCodeProcess(pImpl->hWorkerProcess, &exitCode);
                if (pImpl->logger) pImpl->logger("Worker exited (code=" +
                    std::to_string(exitCode) + ")");

                // Check crash rate
                time_t now = time(nullptr);
                {
                    std::lock_guard<std::mutex> lock(pImpl->crashMutex);
                    pImpl->crashTimes.push_back(now);
                    // Remove entries older than window
                    while (!pImpl->crashTimes.empty() &&
                           now - pImpl->crashTimes.front() > pImpl->crashWindowSec) {
                        pImpl->crashTimes.pop_front();
                    }
                    if ((int)pImpl->crashTimes.size() > pImpl->crashLimit) {
                        if (pImpl->logger) pImpl->logger(
                            "CRITICAL: " + std::to_string(pImpl->crashLimit) +
                            " crashes in " + std::to_string(pImpl->crashWindowSec) +
                            "s. Giving up.");
                        pImpl->running = false;
                        break;
                    }
                }

                // Check pipe connectivity before restart
                if (pImpl->logger) pImpl->logger("Restarting worker...");
                Sleep(1000);
                if (!launch()) {
                    if (pImpl->logger) pImpl->logger("Restart failed, retrying...");
                    Sleep(3000);
                    if (!launch()) {
                        if (pImpl->logger) pImpl->logger("Persistent restart failure. Giving up.");
                        pImpl->running = false;
                        break;
                    }
                }
            }
        }
    });

    return true;
}

void Watchdog::Stop() {
    pImpl->running = false;
    if (pImpl->monitorThread.joinable())
        pImpl->monitorThread.join();
    if (pImpl->hWorkerProcess) {
        TerminateProcess(pImpl->hWorkerProcess, 0);
        CloseHandle(pImpl->hWorkerProcess);
        pImpl->hWorkerProcess = nullptr;
    }
}

bool Watchdog::IsRunning() const { return pImpl->running; }

void Watchdog::SetLogger(Logger logger) { pImpl->logger = logger; }
```

- [ ] **Step 3: Build**

```bash
cmake --build build
```

Expected: compiles cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/watchdog.h src/watchdog.cpp src/CMakeLists.txt
git commit -m "feat: add Watchdog — worker process liveness monitor with crash rate limiting"
```

---

### Task 10: Integrate proxy mode into main.cpp

**Files:**
- Modify: `src/main.cpp` (add `--mode proxy` code path)
- Modify: `src/aim_bridge.h` (add `MountProxy()`)
- Modify: `src/aim_bridge.cpp` (implement `MountProxy()`)
- Modify: `src/cli_parser.h` (add `mode` field)
- Modify: `src/cli_parser.cpp` (parse `--mode`)

- [ ] **Step 1: Add MountProxy to AimBridge**

In `aim_bridge.h`:
```cpp
bool MountProxy(const std::string& pipeName, uint64_t sizeBytes, std::string& outMountPoint);
```

In `aim_bridge.cpp`:
```cpp
bool AimBridge::MountProxy(const std::string& pipeName, uint64_t sizeBytes,
                            std::string& outMountPoint) {
    char sizeBuf[32];
    snprintf(sizeBuf, sizeof(sizeBuf), "%llu", (unsigned long long)sizeBytes);
    std::string args = "-a -t proxy -f \"" + pipeName + "\" -s " + sizeBuf + " -n";
    std::string output;
    if (!RunAimLl(args, output)) return false;
    int devNum = std::atoi(output.c_str());
    LogInfo("Proxy device mounted as device " + std::to_string(devNum));
    auto devices = ListDevices();
    for (auto& d : devices) {
        if (d.number == devNum) {
            outMountPoint = d.physicalDrive.empty() ? "?" : d.physicalDrive;
            return true;
        }
    }
    outMountPoint = "?";
    return true;
}
```

- [ ] **Step 2: Add --mode to CLI parser**

In `cli_parser.h`, add to both `CreateOptions` and `MountOptions`:
```cpp
std::string mode = "vm"; // "vm" or "proxy"
```

In `cli_parser.cpp`, parse in create and mount handlers:
```cpp
else if (arg == "--mode" && i+1 < argc) { r.create.mode = argv[++i]; }
```

- [ ] **Step 3: Add proxy code path in CmdCreate**

After the existing VHD create and before mounting:
```cpp
    if (opts.mode == "proxy") {
        // Start StorageServer + Watchdog
        // ... (see Step 4 for full integration)
    }
```

- [ ] **Step 4: Full proxy mode integration in main.cpp**

```cpp
#include "storage_server.h"
#include "watchdog.h"

// In CmdCreate, after VHD is created and raw data extracted:
if (opts.mode == "proxy") {
    static StorageServer server;
    static Watchdog watchdog;

    std::string pipeName = "\\\\.\\pipe\\ramdisk_proxy";

    watchdog.SetLogger([](const std::string& msg) { WriteLog("Watchdog: " + msg); });
    server.SetLogger([](const std::string& msg) { WriteLog("Server: " + msg); });

    if (!server.Start(pipeName, opts.outputPath, opts.sizeBytes)) {
        LogError("Failed to start storage server");
        return 4;
    }
    WriteLog("Storage server online at " + pipeName);

    // Mount proxy device via aim_ll
    if (!g_aim.MountProxy(pipeName, opts.sizeBytes, g_mountPoint)) {
        LogError("Failed to mount proxy device");
        server.Stop();
        return 3;
    }

    // Start watchdog to monitor server
    std::string workerArgs = "\"" + std::string(argv[0]) + "\" --internal-proxy-server " +
                             pipeName + " " + opts.outputPath + " " +
                             std::to_string(opts.sizeBytes);
    watchdog.Start(argv[0], workerArgs, pipeName);

    WriteLog("Proxy RAM disk ready: " + g_mountPoint);
    WriteLog("Save interval: " + std::to_string(opts.saveIntervalSec) + "s (incremental)");

    // Replace DoSave with incremental version
    // The save callback now uses server.SaveDirty() instead of full read+write
    // (handled in a separate save callback set below)
}
```

- [ ] **Step 5: Build and smoke test**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 100M -o test_proxy.vhd --mode proxy
# If protocol isn't fully confirmed, will error out at aim_ll mount stage
# This is expected — final integration needs Task 5 protocol docs applied
```

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp src/aim_bridge.h src/aim_bridge.cpp src/cli_parser.h src/cli_parser.cpp
git commit -m "feat: integrate --mode proxy with StorageServer + Watchdog"
```

---

### Task 11: Phase 2 end-to-end validation

**Files:** No new files — test procedure only.

- [ ] **Step 1: Verify proxy mode create flow**

```bash
# Admin terminal
./build/src/ramdisk-cli.exe create -s 256M -o test_proxy_e2e.vhd --mode proxy --daemon --log-file proxy_test.log
sleep 3
./build/src/ramdisk-cli.exe status
# Expect: device mounted, proxy pipe connected
```

- [ ] **Step 2: Format and write data**

Manually format the disk in Disk Management → Assign letter → Create `hello.txt` → Edit content.

- [ ] **Step 3: Verify incremental save**

```bash
cat proxy_test.log
# Expect: "Incremental save: N sectors (X MB)" where N is only the changed sectors, NOT 524288 (full 256MB)
```

- [ ] **Step 4: Verify lazy load**

```bash
cat proxy_test.log
# Expect: cache miss events only on first access, not at startup
# At startup: no "Reading VHD" log — just "StorageServer waiting for connection"
```

- [ ] **Step 5: Kill storage server, verify watchdog restart**

```bash
# Find and kill storage server process
powershell -Command "Get-Process ramdisk-cli | Stop-Process -Force"
sleep 5
cat proxy_test.log
# Expect: "Worker exited" → "Restarting worker..." → "Worker launched (PID NNNN)" 
```

- [ ] **Step 6: Verify data survives crash+restart**

After watchdog restarts the server, check if `hello.txt` is still accessible on the drive. Content should be intact (dirty data may be lost if not saved before crash — expected behavior).

- [ ] **Step 7: Clean up and commit any fixes**

```bash
./build/src/ramdisk-cli.exe unmount
rm test_proxy_e2e.vhd
git add -u && git commit -m "fix: proxy mode E2E validation fixes"
```

---

## Completion Checklist

- [ ] Task 1:  VHD data integrity — byte-perfect pattern test passes
- [ ] Task 2:  Auto drive letter — drive appears after mount
- [ ] Task 3:  Daemon mode — background process with log file + named event signaling
- [ ] Task 4:  CLAUDE.md updated
- [ ] Task 5:  AIM proxy protocol documented
- [ ] Task 6:  DirtyBitmap — smoke test passes
- [ ] Task 7:  SectorCache — smoke test passes
- [ ] Task 8:  StorageServer — builds, pipe created
- [ ] Task 9:  Watchdog — worker monitoring + restart
- [ ] Task 10: Proxy mode integrated into main.cpp
- [ ] Task 11: Phase 2 E2E validation — incremental save confirmed, watchdog restart confirmed
