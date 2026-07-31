# RamDisk CLI v2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upgrade CLI MVP to Primo-Ramdisk-like experience via aimapi.dll direct API, big-endian VHD, auto-format, system tray GUI, and dynamic memory management.

**Architecture:** Replace `aim_ll.exe` subprocess bridge with `aimapi.dll` direct calls via `LoadLibrary`. Add tray icon GUI, big-endian VHD compatibility, auto-format, multi-disk support, and TRIM-based DMM. Decouple daemon from CLI via named events.

**Tech Stack:** C++11, GCC 13.1.0 MinGW-w64, CMake, Win32 API, aimapi.dll (55 exported functions)

---

## File Structure Changes

```
src/
├── aim_api.h/.cpp          [NEW] aimapi.dll wrapper (replaces aim_bridge)
├── aim_bridge.h/.cpp       [DEPRECATED] kept for reference, not linked
├── tray_icon.h/.cpp        [NEW] system tray GUI
├── vhd_engine.h/.cpp       [MODIFY] big-endian + fixed VHD
├── main.cpp                [MODIFY] AimApi integration, --auto-format, --dmm
├── cli_parser.h/.cpp       [MODIFY] new CLI flags
├── daemon.h/.cpp           [MODIFY] log fix, mutex, auto-start
├── save_scheduler.h/.cpp   [MODIFY] quick-save snapshot compare
├── drive_letter.h/.cpp     [MODIFY] ImScsiFindFreeDriveLetter usage
└── CMakeLists.txt          [MODIFY] add aim_api, tray_icon
```

---

### Task 1: AimApi — aimapi.dll 动态加载封装

**Files:**
- Create: `src/aim_api.h`
- Create: `src/aim_api.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write src/aim_api.h**

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

// Wrapper around aimapi.dll for direct kernel driver communication.
// Replaces aim_ll.exe subprocess bridge with function calls.
class AimApi {
public:
    struct DeviceInfo {
        int deviceNumber;
        std::string mountPoint;
        uint64_t sizeBytes;
        std::string imageFile;
        std::string physicalDrive;
        bool writable;
        uint32_t scsiPort;
        uint32_t scsiPath;
        uint32_t scsiTarget;
        uint32_t scsiLun;
    };

    AimApi();
    ~AimApi();

    // Load aimapi.dll. Returns false if not found.
    bool Load();

    // Check if API is loaded and driver is available.
    bool IsAvailable() const;

    // Create a VM-type virtual disk, optionally loading from image file.
    // imageFile may be empty to create blank disk.
    bool CreateVmDisk(uint64_t sizeBytes, const std::string& imageFile,
                      std::string& outMountPoint, int& outDeviceNumber,
                      bool writable = true, bool removable = false);

    // Remove a device by number.
    bool RemoveDevice(int deviceNumber);

    // List all virtual disks.
    std::vector<DeviceInfo> ListDevices();

    // Read raw data from a device's PhysicalDrive.
    bool ReadDeviceData(int deviceNumber, uint64_t sizeBytes, void* buffer);

    // Extend device size (live resize).
    bool ExtendDevice(int deviceNumber, uint64_t newSize);

    // Find an available drive letter.
    std::string FindFreeDriveLetter();

    // Get API/driver version strings.
    std::string GetApiVersion() const;
    std::string GetDriverVersion() const;

    // Direct IOCTL for TRIM/DMM operations.
    bool SendTrimCommand(int deviceNumber, const void* ranges, uint32_t count);

private:
    HMODULE m_hDll;
    bool LoadFunctions();

    // Function pointers — populated by LoadFunctions()
    // (exact signatures from aimapi.h in AIM source)
    typedef DWORD (WINAPI *ImScsiCreateDevice_t)(/*...*/);
    typedef DWORD (WINAPI *ImScsiRemoveDeviceByNumber_t)(DWORD);
    // ... (all 55 functions mapped)
};
```

- [ ] **Step 2: Write src/aim_api.cpp — Load + symbol resolution**

```cpp
#include "aim_api.h"
#include "utils.h"
#include <cstdio>

AimApi::AimApi() : m_hDll(nullptr) {}
AimApi::~AimApi() { if (m_hDll) FreeLibrary(m_hDll); }

bool AimApi::Load() {
    // Search paths for aimapi.dll
    const char* paths[] = {
        "C:\\Program Files\\AIM Toolkit\\aimapi.dll",
        ".\\aimapi.dll",
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        m_hDll = LoadLibraryA(paths[i]);
        if (m_hDll) break;
    }
    if (!m_hDll) { LogError("aimapi.dll not found"); return false; }
    return LoadFunctions();
}

bool AimApi::IsAvailable() const {
    return m_hDll != nullptr;
}
```

- [ ] **Step 3: Write AimApi::CreateVmDisk**

```cpp
bool AimApi::CreateVmDisk(uint64_t sizeBytes, const std::string& imageFile,
                           std::string& outMountPoint, int& outDeviceNumber,
                           bool writable, bool removable) {
    if (!m_hDll) return false;

    // Use ImScsiCreateDevice — exact parameter layout from RamDyn.exe analysis:
    // ImScsiCreateDevice(imageFile, sizeKB, flags, mountPoint, ...)
    //
    // Our approach: use CreateProcess to aim_ll.exe as fallback while we
    // reverse the exact ImScsiCreateDevice parameter layout from AIM source.
    // For now, delegate to aim_ll for vm creation and use aimapi for queries.

    // STEP 1: Create blank VM disk via aim_ll (temporary — will replace with
    // direct API once parameter layout is confirmed from aimapi.h)
    std::string rawPath = imageFile + ".raw";
    // ... VHD→raw bridge as before ...

    // STEP 2: Query device info via aimapi
    // ImScsiGetDeviceList → enumerate → find matching device → get mountPoint
    // ImScsiFindFreeDriveLetter → get available letter

    return true; // placeholder — full implementation below
}
```

- [ ] **Step 4: Write remaining AimApi methods (ListDevices, RemoveDevice, ReadDeviceData)**

```cpp
std::vector<AimApi::DeviceInfo> AimApi::ListDevices() {
    std::vector<DeviceInfo> result;
    if (!m_hDll) return result;

    // Use ImScsiGetDeviceList → iterates devices
    // Use ImScsiQueryDevice → get per-device info
    // Use ImScsiGetScsiAddressForDisk → get SCSI address
    // Use GetPhysicalDriveFromDeviceNumber → map to PhysicalDriveN

    // Fallback: use aim_ll -l output parsing (existing code in AimBridge)
    // until ImScsiGetDeviceList parameter layout is confirmed
    return result;
}
```

- [ ] **Step 5: Confirm ImScsiCreateDevice parameter layout from AIM source**

```bash
# Read aimapi.h from downloaded AIM source
grep -A 30 "ImScsiCreateDevice" c:/tmp/aim_src/*/Unmanaged\ Source/aimapi/aimapi.h
# Expected: find function signature with parameter types
```

Update AimApi::CreateVmDisk with correct parameter mapping after confirming the layout.

- [ ] **Step 6: Build and verify**

```bash
cmake --build build
./build/src/ramdisk-cli.exe version
# Expect: ramdisk-cli v2.0.0
```

- [ ] **Step 7: Commit**

```bash
git add src/aim_api.h src/aim_api.cpp src/CMakeLists.txt
git commit -m "feat: add AimApi module — aimapi.dll LoadLibrary wrapper"
```

---

### Task 2: Replace AimBridge with AimApi in main.cpp

**Files:**
- Modify: `src/main.cpp` — switch from AimBridge to AimApi
- Modify: `src/CMakeLists.txt` — remove aim_bridge from build

- [ ] **Step 1: Add AimApi include and global**

Replace `#include "aim_bridge.h"` with `#include "aim_api.h"`.
Replace `static AimBridge g_aim;` with `static AimApi g_aim;`.

- [ ] **Step 2: Update mount calls (CmdCreate, CmdMount)**

Replace:
```cpp
g_aim.MountVm(rawPath, opts.sizeBytes, g_mountPoint)
```
With:
```cpp
int devNum = 0;
g_aim.CreateVmDisk(opts.sizeBytes, "", g_mountPoint, devNum);
g_deviceNumber = devNum;
```

- [ ] **Step 3: Update DoSave — use AimApi::ReadDeviceData**

Replace `g_aim.ReadDeviceData(physPath, size, buffer)` with:
```cpp
g_aim.ReadDeviceData(g_deviceNumber, g_sizeBytes, buffer.data())
```

- [ ] **Step 4: Update CmdUnmount, CmdStatus, CmdSave**

Switch all `g_aim.ListDevices()`, `g_aim.Unmount()`, etc. to AimApi equivalents.

- [ ] **Step 5: Remove aim_bridge from CMakeLists.txt**

```cmake
# Remove aim_bridge.cpp from add_executable
# Add aim_api.cpp instead
```

- [ ] **Step 6: Build and test**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 64M -o test_api.vhd --save-interval 0 &
sleep 3
./build/src/ramdisk-cli.exe status
# Verify: device shown with correct size and path
```

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp src/CMakeLists.txt
git commit -m "refactor: replace AimBridge (aim_ll subprocess) with AimApi (aimapi.dll direct)"
```

---

### Task 3: VHD 大端序修复

**Files:**
- Modify: `src/vhd_engine.cpp`

- [ ] **Step 1: Add byte-swap helpers**

```cpp
static uint32_t Swap32(uint32_t v) {
    uint8_t* b = (uint8_t*)&v;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}
static uint64_t Swap64(uint64_t v) {
    uint8_t* b = (uint8_t*)&v;
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
           ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
           ((uint64_t)b[6] << 8)  |  (uint64_t)b[7];
}
```

- [ ] **Step 2: FillFooter — swap all multi-byte fields before checksum**

In `FillFooter`, add after setting all host-order values and before checksum:
```cpp
f.features = Swap32(f.features);
f.fileFormatVersion = Swap32(f.fileFormatVersion);
f.dataOffset = Swap64(f.dataOffset);
f.timestamp = Swap32(f.timestamp);
f.creatorVersion = Swap32(f.creatorVersion);
f.originalSize = Swap64(f.originalSize);
f.currentSize = Swap64(f.currentSize);
f.diskGeometry = Swap32(f.diskGeometry);
f.diskType = Swap32(f.diskType);
// checksum computed on big-endian footer
f.checksum = CalcFooterChecksum(f);
```

- [ ] **Step 3: Open() — swap footer fields back to host**

After reading footer from disk:
```cpp
footer.features = Swap32(footer.features);
footer.fileFormatVersion = Swap32(footer.fileFormatVersion);
footer.dataOffset = Swap64(footer.dataOffset);
footer.timestamp = Swap32(footer.timestamp);
footer.creatorVersion = Swap32(footer.creatorVersion);
footer.originalSize = Swap64(footer.originalSize);
footer.currentSize = Swap64(footer.currentSize);
footer.diskGeometry = Swap32(footer.diskGeometry);
footer.diskType = Swap32(footer.diskType);
```

- [ ] **Step 4: Create() — write dynhdr + BAT in big-endian**

```cpp
dynHdr.tableOffset = Swap64(batFileOffset);
dynHdr.headerVersion = Swap32(0x00010000);
dynHdr.maxTableEntries = Swap32(maxTableEntries);
dynHdr.blockSize = Swap32(blockSize);
// BAT: all 0xFFFFFFFF which is symmetric under Swap32 — no change needed
```

- [ ] **Step 5: Open() — swap dynhdr + BAT back**

After reading dynhdr:
```cpp
dynHdr.tableOffset = Swap64(dynHdr.tableOffset);
dynHdr.maxTableEntries = Swap32(dynHdr.maxTableEntries);
dynHdr.blockSize = Swap32(dynHdr.blockSize);
```
After reading BAT:
```cpp
for (uint32_t i = 0; i < maxEntries; i++)
    bat[i] = Swap32(bat[i]);
```

- [ ] **Step 6: Close() — swap BAT before write, footer copy swap**

When writing BAT back:
```cpp
std::vector<uint32_t> batBe(pImpl->bat);
for (auto& v : batBe) v = Swap32(v);
WriteFile(h, batBe.data(), ...);
```
When reading footer copy from disk: add swap→modify→swap-back sequence.

- [ ] **Step 7: Fix line 244 in Create — use Swap64**

```cpp
footer.dataOffset = Swap64(VHD_SECTOR_SIZE);  // was: VHD_SECTOR_SIZE
```

- [ ] **Step 8: Fix endFooter.dataOffset in Create — already Swap64**

Line 291 already uses `Swap64(VHD_SECTOR_SIZE)`.

- [ ] **Step 9: Build, test self-consistency**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 10M -o test_be.vhd --save-interval 0 &
sleep 3
./build/src/ramdisk-cli.exe status
# Must show device correctly
./build/src/ramdisk-cli.exe save
./build/src/ramdisk-cli.exe unmount
# Must unmount cleanly
./build/src/ramdisk-cli.exe mount -i test_be.vhd --save-interval 0 &
sleep 3
./build/src/ramdisk-cli.exe status
# Remount must succeed
```

- [ ] **Step 10: Test aim_cli compatibility**

```bash
"/c/Program Files/AIM Toolkit/aim_cli.exe" --ramdisk --filename="test_be.vhd" 2>&1 | head -3
# Expected: "Using image file... Creating RAM disk..." (NOT "VHD file is corrupt")
```

- [ ] **Step 11: Test full E2E persistence regression**

```bash
# create→format→write→save→unmount→remount→verify
# Must pass file content verification
```

- [ ] **Step 12: Commit**

```bash
git add src/vhd_engine.cpp
git commit -m "fix: VHD big-endian compliance — footer, dynhdr, BAT byte-swapped on disk"
```

---

### Task 4: 固定 VHD 支持

**Files:**
- Modify: `src/vhd_engine.cpp`

- [ ] **Step 1: Add fixed VHD creation logic**

In `VhdEngine::Create`, add `dynamic == false` branch:
```cpp
if (!dynamic) {
    // Fixed VHD: raw data + 512-byte footer at EOF
    // 1. Pre-allocate file to sizeBytes + 512
    LARGE_INTEGER li; li.QuadPart = sizeBytes + VHD_FOOTER_SIZE;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    SetEndOfFile(h);

    // 2. Write footer at EOF position
    VhdFooter footer;
    FillFooter(footer, sizeBytes, VHD_TYPE_FIXED);
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    // seek back 512 bytes
    li.QuadPart = sizeBytes;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    DWORD written = 0;
    WriteFile(h, &footer, sizeof(footer), &written, nullptr);

    // 3. Initialize state
    pImpl->dynamic = false;
    pImpl->bat.clear();
    pImpl->blockCache.clear();
    pImpl->totalSectors = sizeBytes / VHD_SECTOR_SIZE;
    pImpl->sizeBytes = sizeBytes;
    // ... (standard state init)
}
```

- [ ] **Step 2: Add fixed VHD open logic**

In `VhdEngine::Open`, handle `diskType == 2`:
```cpp
if (footer.diskType == VHD_TYPE_FIXED) {
    pImpl->dynamic = false;
    pImpl->sizeBytes = footer.currentSize;
    pImpl->totalSectors = footer.currentSize / VHD_SECTOR_SIZE;
    // Fixed VHD has no BAT — direct sector R/W
}
```

- [ ] **Step 3: ReadSectors / WriteSectors for fixed VHD**

For fixed VHD, no BAT lookup needed:
```cpp
if (!pImpl->dynamic) {
    // Direct read: sector → fileOffset = sector * 512
    uint64_t fileOffset = sectorOffset * VHD_SECTOR_SIZE;
    LARGE_INTEGER li; li.QuadPart = fileOffset;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    ReadFile/WriteFile directly;
}
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 10M -o test_fixed.vhd -t fixed --save-interval 0
# Verify: fixed VHD file size = 10MB + 512 bytes
ls -la test_fixed.vhd  # expect ~10486272 bytes
```

- [ ] **Step 5: Commit**

```bash
git add src/vhd_engine.cpp
git commit -m "feat: add fixed VHD (type=2) create/open/read/write support"
```

---

### Task 5: Daemon 日志修复 + 进程唯一性

**Files:**
- Modify: `src/daemon.h`
- Modify: `src/daemon.cpp`

- [ ] **Step 1: Fix log buffer flushing**

In `WriteLog`, add file locking:
```cpp
void WriteLog(const std::string& msg) {
    // ... timestamp formatting ...
    if (g_logFile) {
        // Use a mutex to prevent interleaved writes
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        fprintf(g_logFile, "[%s] %s\n", tb, msg.c_str());
        fflush(g_logFile);  // Force write
        // Also FlushFileBuffers for OS-level durability
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_logFile)));
    }
    LogInfo(msg);
}
```

- [ ] **Step 2: Add process uniqueness via mutex**

In `Daemonize`, before spawning child:
```cpp
static const char* DAEMON_MUTEX = "Global\\RamDiskCli_Daemon_Mutex";
HANDLE hMutex = CreateMutexA(nullptr, FALSE, DAEMON_MUTEX);
if (GetLastError() == ERROR_ALREADY_EXISTS) {
    LogError("Another daemon is already running");
    CloseHandle(hMutex);
    return false;
}
// Store hMutex globally — released on process exit
```

- [ ] **Step 3: Add auto-start registry key**

New function `SetAutoStart(bool enable)`:
```cpp
void SetAutoStart(bool enable) {
    HKEY hKey;
    RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey);
    if (enable) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, sizeof(path));
        std::string cmd = "\"" + std::string(path) + "\" --auto-start";
        RegSetValueExA(hKey, "RamDiskCLI", 0, REG_SZ,
                       (BYTE*)cmd.c_str(), cmd.size() + 1);
    } else {
        RegDeleteValueA(hKey, "RamDiskCLI");
    }
    RegCloseKey(hKey);
}
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 64M -o test_daemon.vhd --daemon --log-file test.log
sleep 2
cat test.log  # Must show daemon started + mounted entries
./build/src/ramdisk-cli.exe unmount
sleep 2
cat test.log  # Must show unmount signal + saved + exiting
```

- [ ] **Step 5: Commit**

```bash
git add src/daemon.h src/daemon.cpp
git commit -m "fix: daemon log flush, process mutex, auto-start registry"
```

---

### Task 6: 自动格式化 + 盘符分配

**Files:**
- Modify: `src/drive_letter.h/.cpp`
- Modify: `src/main.cpp`
- Modify: `src/cli_parser.h/.cpp` — add `--auto-format`, `--fs`, `--label` flags

- [ ] **Step 1: Add ImScsiFindFreeDriveLetter call**

```cpp
// In drive_letter.cpp, use AimApi::FindFreeDriveLetter()
std::string AssignDriveLetter(uint64_t diskSizeBytes) {
    // Wait for disk online
    int diskNum = WaitForDiskOnline(diskSizeBytes, 15);
    if (diskNum < 0) return "";

    // Get free letter from aimapi
    char letter = 'R'; // fallback
    // AimApi::FindFreeDriveLetter() returns first available

    // Use diskpart to initialize + format + assign
    // ...
}
```

- [ ] **Step 2: Add auto-format to CmdCreate**

After mount + drive letter assignment:
```cpp
if (opts.autoFormat) {
    std::string fsType = opts.filesystem.empty() ? "NTFS" : opts.filesystem;
    std::string label = opts.label;

    // Build diskpart script
    FILE* f = fopen(tmpScript, "w");
    fprintf(f, "select disk %d\n", diskNum);
    fprintf(f, "online disk\n");
    fprintf(f, "convert gpt\n");
    fprintf(f, "create partition primary\n");
    fprintf(f, "format fs=%s quick label=%s\n", fsType.c_str(), label.c_str());
    fprintf(f, "assign letter=%c\n", driveLetter[0]);
    fprintf(f, "exit\n");
    fclose(f);

    // Run diskpart
    // ...
}
```

- [ ] **Step 3: Add --auto-format, --fs flags to CLI parser**

```cpp
// In create handler:
else if (arg == "--auto-format") { r.create.autoFormat = true; }
else if (arg == "--fs" && i+1 < argc) { r.create.filesystem = argv[++i]; }
```

- [ ] **Step 4: Build and test**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 100M -o test_auto.vhd --auto-format --fs NTFS --label FASTDISK
# Expected: single command → disk mounted, formatted, drive letter assigned
ls /r/  # or whatever letter was assigned
```

- [ ] **Step 5: Commit**

```bash
git add src/drive_letter.cpp src/main.cpp src/cli_parser.h src/cli_parser.cpp
git commit -m "feat: auto-format with --auto-format --fs NTFS --label NAME"
```

---

### Task 7: 多盘管理

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/cli_parser.h/.cpp`

- [ ] **Step 1: Refactor globals to support per-device state**

Replace single globals with a map:
```cpp
struct DeviceState {
    std::string vhdPath;
    uint64_t sizeBytes;
    std::string mountPoint;
    SaveScheduler scheduler;
    VhdEngine vhd;  // Owned per-device
};
static std::map<int, DeviceState> g_devices;
static AimApi g_aim; // Shared across devices
```

- [ ] **Step 2: Update CmdCreate/CmdMount — register device**

```cpp
int devNum = g_aim.FindNextDeviceNumber();
DeviceState& state = g_devices[devNum];
state.vhdPath = opts.outputPath;
state.sizeBytes = opts.sizeBytes;
// ...
```

- [ ] **Step 3: Update CmdSave/CmdUnmount/CmdStatus — use device number**

All commands accept `--device N` to specify which disk.

- [ ] **Step 4: Update CmdStatus — show all devices**

```cpp
for (auto& [num, state] : g_devices) {
    printf("[%d] %s -> %s (%s) | save: %ds | %s\n", ...);
}
```

- [ ] **Step 5: Build and test multi-device scenario**

```bash
./build/src/ramdisk-cli.exe create -s 64M -o disk1.vhd --save-interval 300 --auto-format --label DISK1
./build/src/ramdisk-cli.exe create -s 128M -o disk2.vhd --save-interval 600 --auto-format --label DISK2
./build/src/ramdisk-cli.exe status
# Expected: two devices shown
./build/src/ramdisk-cli.exe unmount --device 0
./build/src/ramdisk-cli.exe unmount --device 1
```

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp src/cli_parser.h src/cli_parser.cpp
git commit -m "feat: multi-disk management with --device N selector"
```

---

### Task 8: 系统托盘 GUI

**Files:**
- Create: `src/tray_icon.h`
- Create: `src/tray_icon.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write src/tray_icon.h**

```cpp
#pragma once
#include <windows.h>
#include <functional>
#include <string>

class TrayIcon {
public:
    using Callback = std::function<void()>;

    TrayIcon();
    ~TrayIcon();

    // Create tray icon. Returns false on failure.
    bool Create(HINSTANCE hInstance, const std::string& tooltip);

    // Set right-click menu callbacks
    void OnOpenDisk(Callback cb);
    void OnSave(Callback cb);
    void OnUnmount(Callback cb);
    void OnExit(Callback cb);

    // Update tooltip text (e.g. show disk usage)
    void SetTooltip(const std::string& text);

    // Show a balloon notification
    void ShowBalloon(const std::string& title, const std::string& msg);

    // Message loop (blocks until exit)
    void Run();

    // Stop the message loop
    void Quit();

private:
    HWND m_hWnd;
    NOTIFYICONDATAA m_nid;
    HMENU m_hMenu;
    Callback m_onOpen, m_onSave, m_onUnmount, m_onExit;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void BuildMenu();
};
```

- [ ] **Step 2: Write src/tray_icon.cpp — Win32 Shell_NotifyIcon**

```cpp
#include "tray_icon.h"

#define WM_TRAYICON (WM_APP + 1)
#define IDM_OPEN     1001
#define IDM_SAVE     1002
#define IDM_UNMOUNT  1003
#define IDM_EXIT     1004

TrayIcon::TrayIcon() : m_hWnd(nullptr), m_hMenu(nullptr) {}

bool TrayIcon::Create(HINSTANCE hInstance, const std::string& tooltip) {
    // Register window class
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RamDiskTrayClass";
    RegisterClassA(&wc);

    m_hWnd = CreateWindowA("RamDiskTrayClass", "", 0, 0, 0, 0, 0,
                           nullptr, nullptr, hInstance, this);

    memset(&m_nid, 0, sizeof(m_nid));
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    strncpy(m_nid.szTip, tooltip.c_str(), sizeof(m_nid.szTip) - 1);

    Shell_NotifyIconA(NIM_ADD, &m_nid);
    BuildMenu();
    return true;
}

void TrayIcon::BuildMenu() {
    m_hMenu = CreatePopupMenu();
    AppendMenuA(m_hMenu, MF_STRING, IDM_OPEN, "Open RAM Disk");
    AppendMenuA(m_hMenu, MF_STRING, IDM_SAVE, "Save Now");
    AppendMenuA(m_hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(m_hMenu, MF_STRING, IDM_EXIT, "Exit");
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayIcon* self = (TrayIcon*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    if (!self) return DefWindowProc(hWnd, msg, wParam, lParam);

    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            TrackPopupMenu(self->m_hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                          pt.x, pt.y, 0, hWnd, nullptr);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            if (self->m_onOpen) self->m_onOpen();
        }
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDM_OPEN:  if (self->m_onOpen) self->m_onOpen(); break;
        case IDM_SAVE:  if (self->m_onSave) self->m_onSave(); break;
        case IDM_EXIT:  if (self->m_onExit) self->m_onExit(); break;
        }
    }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void TrayIcon::Run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void TrayIcon::Quit() { PostMessage(m_hWnd, WM_DESTROY, 0, 0); }

// ... SetTooltip, ShowBalloon, callback setters ...
```

- [ ] **Step 3: Add tray entry point in main.cpp**

```cpp
// ramdisk-tray sub-command or separate executable
if (argc >= 2 && strcmp(argv[1], "tray") == 0) {
    TrayIcon tray;
    tray.Create(GetModuleHandle(nullptr), "RamDisk CLI");
    tray.OnOpenDisk([]() { ShellExecuteA(nullptr, "open", g_mountPoint.c_str(), ...); });
    tray.OnSave([]() { DoSave(); });
    tray.OnExit([]() { /* save + unmount + PostQuitMessage */ });
    tray.Run();
    return 0;
}
```

- [ ] **Step 4: Build and smoke test**

```bash
cmake --build build
# Start tray in background, verify icon appears
./build/src/ramdisk-cli.exe tray &
sleep 2
# Right-click → Exit
```

- [ ] **Step 5: Commit**

```bash
git add src/tray_icon.h src/tray_icon.cpp src/main.cpp src/CMakeLists.txt
git commit -m "feat: add system tray GUI with right-click menu"
```

---

### Task 9: 快速增量保存（vm 模式快照对比）

**Files:**
- Modify: `src/save_scheduler.h/.cpp`
- Modify: `src/main.cpp`

- [ ] **Step 1: Add snapshot-based dirty detection**

```cpp
// In DoSave, compare with previous save snapshot
static std::vector<uint8_t> g_lastSnapshot; // from previous save

bool DoSaveQuick() {
    std::vector<uint8_t> current(g_sizeBytes);
    g_aim.ReadDeviceData(g_deviceNumber, g_sizeBytes, current.data());

    if (g_lastSnapshot.empty()) {
        // First save — full write
        g_vhd.WriteAll(current.data());
        g_lastSnapshot = std::move(current);
        return true;
    }

    // Compare sector-by-sector, write only changed ones
    uint64_t changedSectors = 0;
    for (uint64_t off = 0; off < g_sizeBytes; off += 512) {
        if (memcmp(current.data() + off, g_lastSnapshot.data() + off, 512) != 0) {
            uint64_t sector = off / 512;
            g_vhd.WriteSectors(sector, 1, current.data() + off);
            changedSectors++;
        }
    }

    LogInfo("Quick save: " + std::to_string(changedSectors) + " sectors changed (" +
            FormatSize(changedSectors * 512) + ")");
    g_lastSnapshot = std::move(current);
    return true;
}
```

- [ ] **Step 2: Add --quick-save CLI flag**

```cpp
bool g_useQuickSave = false;
// In CmdCreate/CmdMount:
if (opts.quickSave) g_useQuickSave = true;
```

- [ ] **Step 3: Build and test**

```bash
cmake --build build
./build/src/ramdisk-cli.exe create -s 64M -o test_qs.vhd --quick-save
# Write small amount of data to disk
# Save — should report fewer changed sectors than total
```

- [ ] **Step 4: Commit**

```bash
git add src/save_scheduler.cpp src/main.cpp
git commit -m "feat: quick incremental save via PhysicalDrive snapshot comparison"
```

---

### Task 10: 动态内存管理 (DMM / TRIM)

**Files:**
- Modify: `src/main.cpp`
- Create: `src/trim_manager.h/.cpp`
- Modify: `src/cli_parser.h/.cpp`

- [ ] **Step 1: Research ImScsiDeviceIoControl TRIM parameters**

From AIM source `aimapi.h`, find the TRIM IOCTL definition:
```bash
grep -rn "TRIM\|UNMAP\|trim\|unmap" c:/tmp/aim_src/*/Unmanaged\ Source/
# Find IOCTL_SCSI_UNMAP or similar control codes
```

- [ ] **Step 2: Write src/trim_manager.h**

```cpp
#pragma once
#include <cstdint>
#include <thread>
#include <atomic>

class TrimManager {
public:
    using TrimCallback = std::function<bool(int deviceNumber, const void* ranges, uint32_t count)>;

    TrimManager();
    ~TrimManager();

    // Start background TRIM thread
    bool Start(int deviceNumber, int intervalSec, TrimCallback cb);
    void Stop();
    bool IsRunning() const;

private:
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    int m_deviceNumber = 0;
    int m_intervalSec = 60;
    TrimCallback m_callback;

    void TrimLoop();
};
```

- [ ] **Step 3: Write src/trim_manager.cpp — NTFS $Bitmap scanner**

```cpp
void TrimManager::TrimLoop() {
    while (m_running) {
        // Sleep for interval
        for (int i = 0; i < m_intervalSec && m_running; i++)
            Sleep(1000);
        if (!m_running) break;

        // Open PhysicalDrive and scan NTFS $Bitmap
        // (Simplified: use FSCTL_GET_VOLUME_BITMAP to find free clusters)
        // Build TRIM ranges for free clusters
        // Send via ImScsiDeviceIoControl(IOCTL_SCSI_UNMAP, ranges)

        LogInfo("TRIM: released N ranges (X MB freed)");
    }
}
```

- [ ] **Step 4: Integrate into CmdCreate with --dmm flag**

```cpp
if (opts.dmm) {
    TrimManager trimMgr;
    trimMgr.Start(g_deviceNumber, opts.cleanInterval,
        [](int dev, const void* ranges, uint32_t count) {
            return g_aim.SendTrimCommand(dev, ranges, count);
        });
}
```

- [ ] **Step 5: Build and smoke test**

```bash
cmake --build build
```

- [ ] **Step 6: Commit**

```bash
git add src/trim_manager.h src/trim_manager.cpp src/main.cpp src/cli_parser.h src/cli_parser.cpp
git commit -m "feat: dynamic memory management via TRIM/UNMAP + NTFS bitmap scanner"
```

---

### Task 11: 更新 CLAUDE.md + PROJECT_STATUS.md

**Files:**
- Modify: `CLAUDE.md`
- Modify: `PROJECT_STATUS.md`

- [ ] **Step 1: Update CLAUDE.md**

Add v2 features, new CLI flags, aimapi architecture note.

- [ ] **Step 2: Update PROJECT_STATUS.md**

Mark completed v2 tasks as green.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md PROJECT_STATUS.md
git commit -m "docs: update CLAUDE.md and PROJECT_STATUS.md for v2 features"
```

---

## Completion Checklist

- [ ] Task 1:  AimApi aimapi.dll wrapper → builds, IsAvailable() returns true
- [ ] Task 2:  AimApi replaces AimBridge in main.cpp → create/mount/save/unmount all work
- [ ] Task 3:  VHD big-endian → aim_cli accepts our VHD files
- [ ] Task 4:  Fixed VHD create/open → `-t fixed` creates valid fixed VHD
- [ ] Task 5:  Daemon log flush + mutex + auto-start → log file complete, no double-start
- [ ] Task 6:  Auto-format → single command creates formatted, accessible disk
- [ ] Task 7:  Multi-disk → two simultaneous RAM disks work independently
- [ ] Task 8:  Tray GUI → icon appears, right-click menu works
- [ ] Task 9:  Quick save → saves only report changed sectors
- [ ] Task 10: DMM/TRIM → trim_manager builds (full NTFS scanning is Phase 2+)
- [ ] Task 11: Documentation updated
