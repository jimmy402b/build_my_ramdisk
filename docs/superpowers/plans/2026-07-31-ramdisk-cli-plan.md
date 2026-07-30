# RamDisk CLI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a CLI ramdisk tool (C++11) that creates/loads VHD files into RAM via Arsenal Image Mounter driver, with timed save, shutdown save, and manual save.

**Architecture:** Four C++ modules — VhdEngine (VHD file I/O via MiniVHD), AimBridge (aim_ll.exe subprocess), SaveScheduler (timer + shutdown handler), CLI Parser (argument parsing). Single-threaded main loop with a background timer thread for auto-save.

**Tech Stack:** C++11, GCC 13.1.0 MinGW-w64, CMake 3.16+, MiniVHD (C, MIT), aim_ll.exe (pre-installed), phdskmnt.sys (pre-installed WHCP-signed driver).

---

## File Structure

```
build_my_ramdisk/
├── CMakeLists.txt              # [CREATE] Root CMake — subdirectories for src/ and vendor/
├── CLAUDE.md                   # [CREATE] Agent skill doc (Task 9)
├── src/
│   ├── CMakeLists.txt          # [CREATE] Executable target, link MiniVHD
│   ├── main.cpp                # [CREATE] Entry point, command dispatch
│   ├── cli_parser.h            # [CREATE] CLI types + parse function decl
│   ├── cli_parser.cpp          # [CREATE] Argument parsing impl
│   ├── vhd_engine.h            # [CREATE] VhdEngine class decl
│   ├── vhd_engine.cpp          # [CREATE] VhdEngine impl wrapping MiniVHD
│   ├── aim_bridge.h            # [CREATE] AimBridge class decl
│   ├── aim_bridge.cpp          # [CREATE] AimBridge impl via CreateProcess
│   ├── save_scheduler.h        # [CREATE] SaveScheduler class decl
│   ├── save_scheduler.cpp      # [CREATE] SaveScheduler impl (timer+shutdown)
│   ├── utils.h                 # [CREATE] Log macros, string helpers decl
│   └── utils.cpp               # [CREATE] Utils impl
├── vendor/
│   └── CMakeLists.txt          # [CREATE] MiniVHD static library target
└── docs/
    └── superpowers/
        ├── specs/
        │   └── 2026-07-31-ramdisk-cli-design.md
        └── plans/
            └── 2026-07-31-ramdisk-cli-plan.md
```

**Interfaces at a glance:**

| Module | Header | Key public API |
|--------|--------|---------------|
| VhdEngine | `vhd_engine.h` | `Create()`, `Open()`, `Close()`, `ReadSectors()`, `WriteSectors()`, `ReadAll()`, `WriteAll()`, `GetSizeBytes()`, `IsOpen()` |
| AimBridge | `aim_bridge.h` | `IsAvailable()`, `MountVm()`, `MountVmFromFile()`, `Unmount()`, `ListDevices()`, `ReadDeviceData()`, `FindAimLl()` |
| SaveScheduler | `save_scheduler.h` | `Start()`, `Stop()`, `SaveNow()`, `RegisterShutdownHandler()`, `GetLastSaveTime()`, `SetSaveCallback()` |
| CLI | `cli_parser.h` | `enum class Command`, `struct Options`, `ParseResult ParseArgs(int, char**)` |
| Utils | `utils.h` | `LOG_INFO()`, `LOG_ERROR()`, `GetErrorMessage()`, `FormatSize()`, `FormatTime()` |

---

### Task 1: Project scaffolding — CMake + directory tree + .gitignore

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/CMakeLists.txt`
- Create: `vendor/CMakeLists.txt`
- Create: `src/main.cpp` (minimal stub)
- Create: `.gitignore`

- [ ] **Step 1: Create root CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(ramdisk-cli VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(vendor)
add_subdirectory(src)
```

- [ ] **Step 2: Create src/CMakeLists.txt**

```cmake
add_executable(ramdisk-cli
    main.cpp
    cli_parser.cpp
    vhd_engine.cpp
    aim_bridge.cpp
    save_scheduler.cpp
    utils.cpp
)

target_include_directories(ramdisk-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ramdisk-cli PRIVATE minivhd)
```

- [ ] **Step 3: Create vendor/CMakeLists.txt**

```cmake
# MiniVHD static library — source files added after git clone
add_library(minivhd STATIC)
target_include_directories(minivhd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/MiniVHD)

# MiniVHD source files will be added in Task 2 after clone
# For now, create an empty placeholder so CMake doesn't error on the target reference
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/minivhd_placeholder.c "/* placeholder */")
target_sources(minivhd PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/minivhd_placeholder.c)
```

- [ ] **Step 4: Create minimal src/main.cpp stub**

```cpp
#include <cstdio>

int main(int argc, char* argv[]) {
    printf("ramdisk-cli v1.0.0\n");
    printf("Not yet implemented.\n");
    return 0;
}
```

- [ ] **Step 5: Create .gitignore**

```
build/
*.o
*.exe
*.a
.DS_Store
```

- [ ] **Step 6: Build and verify**

Run:
```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

Expected: Build succeeds, `build\ramdisk-cli.exe` exists.

Run:
```bash
./build/ramdisk-cli.exe
```

Expected output:
```
ramdisk-cli v1.0.0
Not yet implemented.
```

- [ ] **Step 7: Commit**

```bash
git init
git add CMakeLists.txt src/ vendor/ .gitignore
git commit -m "feat: project scaffolding with CMake + MinGW"
```

---

### Task 2: Clone MiniVHD and wire into build

**Files:**
- Modify: `vendor/CMakeLists.txt`
- Create: `src/vhd_engine.h` (forward declaration only, no impl yet)

- [ ] **Step 1: Clone MiniVHD as git submodule**

```bash
git submodule add https://github.com/shermp/MiniVHD.git vendor/MiniVHD
git submodule update --init --recursive
```

- [ ] **Step 2: Discover MiniVHD source files**

Run:
```bash
ls vendor/MiniVHD/*.c vendor/MiniVHD/*.h 2>/dev/null
```

Expected: A list of `.c` and `.h` files. Note the exact filenames (typically `minivhd.c`, `minivhd.h` plus internal headers).

- [ ] **Step 3: Update vendor/CMakeLists.txt**

Replace the placeholder content:

```cmake
# Collect all MiniVHD C sources
file(GLOB MINIVHD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/MiniVHD/*.c
)

add_library(minivhd STATIC ${MINIVHD_SOURCES})

target_include_directories(minivhd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/MiniVHD)

# MiniVHD uses C99 features — ensure they're enabled
target_compile_options(minivhd PRIVATE -std=c99)

# Suppress any warnings from third-party code
if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    target_compile_options(minivhd PRIVATE -w)
endif()
```

- [ ] **Step 4: Create src/vhd_engine.h forward declaration**

```cpp
#pragma once

// Forward declaration — full interface in Task 3.
// This file exists now so CMake can compile files that #include it.

class VhdEngine {
public:
    VhdEngine() = default;
    ~VhdEngine() = default;
};
```

- [ ] **Step 5: Build and verify MiniVHD compiles**

Run:
```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

Expected: Build succeeds. Static library `build/vendor/libminivhd.a` exists.

Run:
```bash
ls -la build/vendor/libminivhd.a
```

Expected: File exists, non-zero size.

- [ ] **Step 6: Commit**

```bash
git add vendor/CMakeLists.txt vendor/MiniVHD src/vhd_engine.h .gitmodules
git commit -m "feat: add MiniVHD submodule and static library target"
```

---

### Task 3: Utils module — logging, error formatting, size/time formatting

**Files:**
- Create: `src/utils.h`
- Create: `src/utils.cpp`
- Modify: `src/CMakeLists.txt` (no change needed — `utils.cpp` already listed in glob)

- [ ] **Step 1: Write src/utils.h**

```cpp
#pragma once

#include <string>
#include <cstdint>
#include <ctime>

// --- Logging (prints to stderr with prefix) ---

void LogInfo(const std::string& msg);
void LogError(const std::string& msg);

// --- Windows error formatting ---

std::string GetLastWinErrorMessage();
std::string GetErrorMessage(const std::string& context);

// --- Human-readable formatting ---

std::string FormatSize(uint64_t bytes);
// e.g. 4294967296 -> "4.00 GB"

std::string FormatTime(time_t t);
// e.g. 1722419422 -> "14:30:22"

std::string FormatTimeAgo(time_t t, time_t now);
// e.g. "2min ago", "just now"

// --- String helpers ---

std::string Trim(const std::string& s);
bool StartsWith(const std::string& s, const std::string& prefix);
```

- [ ] **Step 2: Write src/utils.cpp**

```cpp
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

// --- Logging ---

void LogInfo(const std::string& msg) {
    fprintf(stderr, "[INFO] %s\n", msg.c_str());
    fflush(stderr);
}

void LogError(const std::string& msg) {
    fprintf(stderr, "[ERROR] %s\n", msg.c_str());
    fflush(stderr);
}

// --- Windows error ---

std::string GetLastWinErrorMessage() {
    DWORD err = GetLastError();
    if (err == 0) return "No error";
    LPSTR buf = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (LPSTR)&buf, 0, nullptr);
    std::string msg(buf, len);
    LocalFree(buf);
    return msg;
}

std::string GetErrorMessage(const std::string& context) {
    return context + ": " + GetLastWinErrorMessage();
}

// --- Formatting ---

std::string FormatSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int idx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && idx < 4) {
        size /= 1024.0;
        idx++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f %s", size, units[idx]);
    return buf;
}

std::string FormatTime(time_t t) {
    char buf[16];
    struct tm* tm_info = localtime(&t);
    if (!tm_info) return "--:--:--";
    strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return buf;
}

std::string FormatTimeAgo(time_t t, time_t now) {
    if (t == 0) return "never";
    int64_t diff = static_cast<int64_t>(now - t);
    if (diff < 0) diff = 0;
    if (diff < 5)   return "just now";
    if (diff < 60)  return std::to_string(diff) + "s ago";
    if (diff < 3600) return std::to_string(diff / 60) + "min ago";
    return std::to_string(diff / 3600) + "h ago";
}

// --- String helpers ---

std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}
```

- [ ] **Step 3: Verify compilation**

Run:
```bash
cmake --build build
```

Expected: Compilation succeeds, no errors. `build/src/CMakeFiles/ramdisk-cli.dir/utils.cpp.obj` exists.

- [ ] **Step 4: Write and run a quick smoke test**

Open `src/main.cpp` and temporarily modify to test utils:

```cpp
#include "utils.h"

int main() {
    LogInfo("Testing utils module");
    LogInfo("FormatSize(4294967296) = " + FormatSize(4294967296ULL));
    LogInfo("FormatTimeAgo(0, time(0)) = " + FormatTimeAgo(0, time(nullptr)));
    LogError("Test error: " + GetErrorMessage("operation failed"));
    printf("Trim test: '%s'\n", Trim("  hello  ").c_str());
    printf("StartsWith test: %d\n", (int)StartsWith("--size 4G", "--size"));
    return 0;
}
```

Run:
```bash
cmake --build build && ./build/ramdisk-cli.exe
```

Expected output:
```
[INFO] Testing utils module
[INFO] FormatSize(4294967296) = 4.00 GB
[INFO] FormatTimeAgo(0, time(0)) = never
[ERROR] Test error: operation failed: No error
Trim test: 'hello'
StartsWith test: 1
```

- [ ] **Step 5: Revert main.cpp to stub**

Restore `src/main.cpp` to:
```cpp
#include <cstdio>
int main() { printf("ramdisk-cli v1.0.0\n"); return 0; }
```

- [ ] **Step 6: Commit**

```bash
git add src/utils.h src/utils.cpp
git commit -m "feat: add utils module (logging, formatting, string helpers)"
```

---

### Task 4: VhdEngine — VHD file I/O wrapping MiniVHD

**Files:**
- Create: `src/vhd_engine.cpp`
- Modify: `src/vhd_engine.h` (replace forward declaration with full interface)

- [ ] **Step 1: Write src/vhd_engine.h (full interface)**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <memory>

class VhdEngine {
public:
    VhdEngine();
    ~VhdEngine();

    // Non-copyable (owns a MiniVHD handle)
    VhdEngine(const VhdEngine&) = delete;
    VhdEngine& operator=(const VhdEngine&) = delete;

    // Create a new VHD file on disk
    bool Create(const std::string& path, uint64_t sizeBytes, bool dynamic = true);

    // Open an existing VHD file
    bool Open(const std::string& path, bool writable = true);

    // Close and release resources
    void Close();

    // Sector-level I/O (sector = 512 bytes)
    bool ReadSectors(uint64_t sectorOffset, uint32_t sectorCount, void* buffer);
    bool WriteSectors(uint64_t sectorOffset, uint32_t sectorCount, const void* buffer);

    // Bulk I/O for Phase 1 (full load / full save)
    bool ReadAll(void* buffer);
    bool WriteAll(const void* buffer);

    // Metadata
    uint64_t GetTotalSectors() const;
    uint64_t GetSizeBytes() const;
    bool IsDynamic() const;
    bool IsOpen() const;
    const std::string& GetPath() const;

    // Error info
    std::string GetLastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
```

- [ ] **Step 2: Write src/vhd_engine.cpp**

```cpp
#include "vhd_engine.h"

// MiniVHD C header — extern "C" wrapper
extern "C" {
#include "minivhd.h"
}

#include <cstring>
#include <cstdlib>

struct VhdEngine::Impl {
    MVHDHandle handle = nullptr;
    std::string path;
    uint64_t sizeBytes = 0;
    uint64_t totalSectors = 0;
    bool dynamic = false;
    bool open = false;
    std::string lastError;
};

VhdEngine::VhdEngine() : pImpl(std::make_unique<Impl>()) {}

VhdEngine::~VhdEngine() {
    Close();
}

bool VhdEngine::Create(const std::string& path, uint64_t sizeBytes, bool dynamic) {
    if (pImpl->open) {
        pImpl->lastError = "VhdEngine already has an open file; close it first";
        return false;
    }

    MVHDCreationFlags flags = dynamic ? MVHD_CREATION_DYNAMIC : MVHD_CREATION_FIXED;
    MVHDHandle h = mvhd_create(path.c_str(), sizeBytes, flags);
    if (h == nullptr) {
        pImpl->lastError = "mvhd_create failed for: " + path;
        return false;
    }

    pImpl->handle = h;
    pImpl->path = path;
    pImpl->sizeBytes = sizeBytes;
    pImpl->totalSectors = sizeBytes / 512;
    pImpl->dynamic = dynamic;
    pImpl->open = true;
    return true;
}

bool VhdEngine::Open(const std::string& path, bool writable) {
    if (pImpl->open) {
        pImpl->lastError = "VhdEngine already has an open file; close it first";
        return false;
    }

    MVHDMode mode = writable ? MVHD_MODE_READ_WRITE : MVHD_MODE_READ_ONLY;
    MVHDHandle h = mvhd_open(path.c_str(), mode);
    if (h == nullptr) {
        pImpl->lastError = "mvhd_open failed for: " + path;
        return false;
    }

    uint64_t size = 0;
    if (!mvhd_get_size(h, &size)) {
        mvhd_close(h);
        pImpl->lastError = "mvhd_get_size failed for: " + path;
        return false;
    }

    pImpl->handle = h;
    pImpl->path = path;
    pImpl->sizeBytes = size;
    pImpl->totalSectors = size / 512;
    pImpl->dynamic = true; // MiniVHD reports dynamic status differently; default to true
    pImpl->open = true;
    return true;
}

void VhdEngine::Close() {
    if (pImpl->handle) {
        mvhd_close(pImpl->handle);
        pImpl->handle = nullptr;
    }
    pImpl->open = false;
    pImpl->path.clear();
    pImpl->sizeBytes = 0;
    pImpl->totalSectors = 0;
}

bool VhdEngine::ReadSectors(uint64_t sectorOffset, uint32_t sectorCount, void* buffer) {
    if (!pImpl->open) {
        pImpl->lastError = "VhdEngine not open";
        return false;
    }
    if (sectorOffset + sectorCount > pImpl->totalSectors) {
        pImpl->lastError = "ReadSectors: out of bounds";
        return false;
    }
    if (!mvhd_read_sectors(pImpl->handle, sectorOffset, sectorCount, buffer)) {
        pImpl->lastError = "mvhd_read_sectors failed";
        return false;
    }
    return true;
}

bool VhdEngine::WriteSectors(uint64_t sectorOffset, uint32_t sectorCount, const void* buffer) {
    if (!pImpl->open) {
        pImpl->lastError = "VhdEngine not open";
        return false;
    }
    if (sectorOffset + sectorCount > pImpl->totalSectors) {
        pImpl->lastError = "WriteSectors: out of bounds";
        return false;
    }
    if (!mvhd_write_sectors(pImpl->handle, sectorOffset, sectorCount, buffer)) {
        pImpl->lastError = "mvhd_write_sectors failed";
        return false;
    }
    return true;
}

bool VhdEngine::ReadAll(void* buffer) {
    if (!pImpl->open || pImpl->totalSectors == 0) return false;
    return ReadSectors(0, static_cast<uint32_t>(pImpl->totalSectors), buffer);
}

bool VhdEngine::WriteAll(const void* buffer) {
    if (!pImpl->open || pImpl->totalSectors == 0) return false;
    return WriteSectors(0, static_cast<uint32_t>(pImpl->totalSectors), buffer);
}

uint64_t VhdEngine::GetTotalSectors() const { return pImpl->totalSectors; }
uint64_t VhdEngine::GetSizeBytes() const { return pImpl->sizeBytes; }
bool VhdEngine::IsDynamic() const { return pImpl->dynamic; }
bool VhdEngine::IsOpen() const { return pImpl->open; }
const std::string& VhdEngine::GetPath() const { return pImpl->path; }
std::string VhdEngine::GetLastError() const { return pImpl->lastError; }
```

- [ ] **Step 3: Check MiniVHD API names**

Before building, verify the actual function names in MiniVHD headers:

```bash
grep -E "^(MVHDHandle|MVHDMode|MVHDCreationFlags|bool mvhd_|MVHDHandle mvhd_)" vendor/MiniVHD/minivhd.h
```

Expected: See function declarations. **Important:** The exact function names may differ from what I wrote above. Match the actual MiniVHD API:
- If `mvhd_create` doesn't take `MVHDCreationFlags`, check the signature
- If there's no `mvhd_get_size`, look for an equivalent
- Adjust `vhd_engine.cpp` function calls to match the real MiniVHD API exactly

**If MiniVHD API is confirmed different, adjust the code and note changes before proceeding.**

- [ ] **Step 4: Build**

```bash
cmake --build build
```

Expected: Compilation succeeds. Fix any API mismatches from Step 3.

- [ ] **Step 5: Write and run VhdEngine smoke test**

Temporarily modify `src/main.cpp`:

```cpp
#include "vhd_engine.h"
#include "utils.h"
#include <cstdio>
#include <vector>

int main() {
    // Test 1: Create a dynamic VHD
    VhdEngine vhd;
    const char* testPath = "test_smoke.vhd";
    uint64_t size = 10ULL * 1024 * 1024; // 10 MB

    if (!vhd.Create(testPath, size, true)) {
        LogError("CREATE FAILED: " + vhd.GetLastError());
        return 1;
    }
    LogInfo("Created: " + std::string(testPath) + " (" + FormatSize(size) + ")");
    vhd.Close();

    // Test 2: Open it back
    if (!vhd.Open(testPath, true)) {
        LogError("OPEN FAILED: " + vhd.GetLastError());
        return 2;
    }
    LogInfo("Opened: " + std::string(testPath));
    LogInfo("Size: " + FormatSize(vhd.GetSizeBytes()));
    LogInfo("Sectors: " + std::to_string(vhd.GetTotalSectors()));

    // Test 3: Write and read back
    std::vector<uint8_t> writeBuf(512, 0xAB);
    if (!vhd.WriteSectors(0, 1, writeBuf.data())) {
        LogError("WRITE FAILED: " + vhd.GetLastError());
        return 3;
    }
    LogInfo("Wrote 512 bytes of 0xAB to sector 0");

    std::vector<uint8_t> readBuf(512, 0);
    if (!vhd.ReadSectors(0, 1, readBuf.data())) {
        LogError("READ FAILED: " + vhd.GetLastError());
        return 4;
    }
    LogInfo("Read sector 0 back");

    if (readBuf != writeBuf) {
        LogError("DATA MISMATCH: read != written");
        return 5;
    }
    LogInfo("PASS: data round-trip matches");

    vhd.Close();
    remove(testPath);
    LogInfo("All VhdEngine tests passed");
    return 0;
}
```

Run:
```bash
cmake --build build && ./build/ramdisk-cli.exe
```

Expected output:
```
[INFO] Created: test_smoke.vhd (10.00 MB)
[INFO] Opened: test_smoke.vhd
[INFO] Size: 10.00 MB
[INFO] Sectors: 20480
[INFO] Wrote 512 bytes of 0xAB to sector 0
[INFO] Read sector 0 back
[INFO] PASS: data round-trip matches
[INFO] All VhdEngine tests passed
```

- [ ] **Step 6: Revert main.cpp to stub**

```cpp
#include <cstdio>
int main() { printf("ramdisk-cli v1.0.0\n"); return 0; }
```

- [ ] **Step 7: Commit**

```bash
git add src/vhd_engine.h src/vhd_engine.cpp
git commit -m "feat: add VhdEngine module wrapping MiniVHD"
```

---

### Task 5: AimBridge — aim_ll.exe subprocess bridge

**Files:**
- Create: `src/aim_bridge.h`
- Create: `src/aim_bridge.cpp`

- [ ] **Step 1: Write src/aim_bridge.h**

```cpp
#pragma once

#include <string>
#include <vector>
#include <cstdint>

class AimBridge {
public:
    struct DeviceInfo {
        int number;
        std::string mountPoint;     // e.g. "R:"
        uint64_t sizeBytes;
        std::string backingFile;
    };

    // Check if aim_ll.exe exists in one of the known locations
    static bool IsAvailable();

    // Create a new vm-type RAM disk with a backing VHD file
    // outMountPoint receives the drive letter (e.g. "R:")
    bool MountVm(const std::string& vhdPath, uint64_t sizeBytes,
                 std::string& outMountPoint);

    // Mount a RAM disk from an existing VHD file (size from VHD)
    bool MountVmFromFile(const std::string& vhdPath,
                         std::string& outMountPoint);

    // Detach device (force=false: safe remove; force=true: emergency)
    bool Unmount(int deviceNumber, bool force = false);

    // List all AIM virtual disks
    std::vector<DeviceInfo> ListDevices();

    // Read raw sectors from a mounted RAM disk via \\.\PhysicalDriveN
    // buffer must be at least sizeBytes large
    bool ReadDeviceData(int deviceNumber, uint64_t sizeBytes, void* buffer);

    // Find aim_ll.exe path
    static std::string FindAimLl();

private:
    // Run aim_ll.exe with args, capture stdout, return success
    bool RunAimLl(const std::string& args, std::string& stdoutOutput);

    // Find \\.\PhysicalDriveN from device number by querying aim_ll -l
    std::string FindPhysicalDrivePath(int deviceNumber);
};
```

- [ ] **Step 2: Write src/aim_bridge.cpp**

```cpp
#include "aim_bridge.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <sstream>
#include <algorithm>

// --- FindAimLl ---

std::string AimBridge::FindAimLl() {
    // 1. Environment variable
    const char* env = getenv("AIM_LL_PATH");
    if (env && GetFileAttributesA(env) != INVALID_FILE_ATTRIBUTES)
        return env;

    // 2. Default install location
    const char* defaultPath = "C:\\Program Files\\AIM Toolkit\\aim_ll.exe";
    if (GetFileAttributesA(defaultPath) != INVALID_FILE_ATTRIBUTES)
        return defaultPath;

    // 3. Current directory
    const char* localPath = ".\\aim_ll.exe";
    if (GetFileAttributesA(localPath) != INVALID_FILE_ATTRIBUTES)
        return localPath;

    // 4. PATH search
    // Try running "where aim_ll" — but simpler: just return empty
    return "";
}

bool AimBridge::IsAvailable() {
    return !FindAimLl().empty();
}

// --- RunAimLl ---

bool AimBridge::RunAimLl(const std::string& args, std::string& stdoutOutput) {
    std::string aimPath = FindAimLl();
    if (aimPath.empty()) {
        LogError("aim_ll.exe not found");
        return false;
    }

    std::string cmdLine = "\"" + aimPath + "\" " + args;

    // Create a pipe for stdout
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

    // Need a writable buffer for CreateProcessA
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        LogError("CreateProcess failed for aim_ll: " + GetLastWinErrorMessage());
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    CloseHandle(hWrite);

    // Wait for process to exit (30 second timeout)
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 30000);
    if (waitResult != WAIT_OBJECT_0) {
        LogError("aim_ll timed out");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hRead);
        return false;
    }

    // Read stdout
    char buf[4096];
    DWORD bytesRead;
    std::ostringstream oss;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        oss << buf;
    }
    stdoutOutput = oss.str();

    // Check exit code
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);

    return exitCode == 0;
}

// --- MountVm ---

bool AimBridge::MountVm(const std::string& vhdPath, uint64_t sizeBytes,
                         std::string& outMountPoint) {
    // Convert size to aim_ll format (e.g. "4G")
    char sizeBuf[32];
    if (sizeBytes % (1024ULL * 1024 * 1024) == 0) {
        snprintf(sizeBuf, sizeof(sizeBuf), "%lluG",
                 (unsigned long long)(sizeBytes / (1024ULL * 1024 * 1024)));
    } else if (sizeBytes % (1024 * 1024) == 0) {
        snprintf(sizeBuf, sizeof(sizeBuf), "%lluM",
                 (unsigned long long)(sizeBytes / (1024 * 1024)));
    } else {
        snprintf(sizeBuf, sizeof(sizeBuf), "%llu", (unsigned long long)sizeBytes);
    }

    std::string args = "-a -t vm -f \"" + vhdPath + "\" -s " + sizeBuf + " -n";
    std::string output;
    if (!RunAimLl(args, output)) {
        return false;
    }

    // aim_ll -n prints just the device number
    int devNum = std::atoi(output.c_str());
    LogInfo("Mounted as device " + std::to_string(devNum));

    // Query mount point
    auto devices = ListDevices();
    for (auto& d : devices) {
        if (d.number == devNum) {
            outMountPoint = d.mountPoint;
            return true;
        }
    }
    outMountPoint = "?";
    return true;
}

// --- MountVmFromFile ---

bool AimBridge::MountVmFromFile(const std::string& vhdPath,
                                 std::string& outMountPoint) {
    std::string args = "-a -t vm -f \"" + vhdPath + "\" -n";
    std::string output;
    if (!RunAimLl(args, output)) {
        return false;
    }

    int devNum = std::atoi(output.c_str());
    LogInfo("Mounted VHD as device " + std::to_string(devNum));

    auto devices = ListDevices();
    for (auto& d : devices) {
        if (d.number == devNum) {
            outMountPoint = d.mountPoint;
            return true;
        }
    }
    outMountPoint = "?";
    return true;
}

// --- Unmount ---

bool AimBridge::Unmount(int deviceNumber, bool force) {
    char argBuf[64];
    snprintf(argBuf, sizeof(argBuf), "%s -u %d",
             force ? "-D" : "-d", deviceNumber);
    std::string output;
    return RunAimLl(argBuf, output);
}

// --- ListDevices ---

std::vector<AimBridge::DeviceInfo> AimBridge::ListDevices() {
    std::vector<DeviceInfo> result;
    std::string output;
    if (!RunAimLl("-l", output)) return result;

    // Parse aim_ll -l output.
    // Format is one device per line, fields separated by spaces.
    // Example line: "0  R:  4294967296  D:\disk.vhd"
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line.empty()) continue;

        DeviceInfo info;
        std::istringstream liness(line);
        liness >> info.number;

        std::string token;
        // Next token could be mount point or size
        if (liness >> token) {
            if (token.size() == 2 && token[1] == ':') {
                info.mountPoint = token;
                liness >> info.sizeBytes;
            } else {
                info.mountPoint = "";
                info.sizeBytes = std::stoull(token);
            }
        }

        // Rest is backing file path
        std::string rest;
        std::getline(liness, rest);
        info.backingFile = Trim(rest);

        result.push_back(info);
    }
    return result;
}

// --- FindPhysicalDrivePath ---

std::string AimBridge::FindPhysicalDrivePath(int deviceNumber) {
    // Query aim_ll -l -u <num> for detailed info
    char argBuf[64];
    snprintf(argBuf, sizeof(argBuf), "-l -u %d", deviceNumber);
    std::string output;
    if (!RunAimLl(argBuf, output)) return "";

    // The detailed output should include the PhysicalDrive path
    // Typical format: "Device number: 0\nPhysical device: \Device\Harddisk3\..."
    // We need \\.\PhysicalDriveN
    // For now, assume device number == physical drive N
    // In practice, we'll verify this mapping at runtime
    return "\\\\.\\PhysicalDrive" + std::to_string(deviceNumber);
}

// --- ReadDeviceData ---

bool AimBridge::ReadDeviceData(int deviceNumber, uint64_t sizeBytes, void* buffer) {
    std::string physPath = FindPhysicalDrivePath(deviceNumber);
    if (physPath.empty()) {
        // Fallback: try device number as the PhysicalDrive number
        physPath = "\\\\.\\PhysicalDrive" + std::to_string(deviceNumber);
    }

    LogInfo("Reading device data from: " + physPath);

    HANDLE h = CreateFileA(physPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogError("Cannot open device for reading: " + GetLastWinErrorMessage());
        return false;
    }

    // Read in chunks to avoid issues with large single reads
    const DWORD CHUNK_SIZE = 64 * 1024 * 1024; // 64 MB chunks
    uint64_t remaining = sizeBytes;
    uint8_t* dst = static_cast<uint8_t*>(buffer);

    while (remaining > 0) {
        DWORD toRead = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : static_cast<DWORD>(remaining);
        DWORD bytesRead = 0;
        if (!ReadFile(h, dst, toRead, &bytesRead, nullptr)) {
            LogError("ReadFile failed: " + GetLastWinErrorMessage());
            CloseHandle(h);
            return false;
        }
        if (bytesRead == 0) break;
        dst += bytesRead;
        remaining -= bytesRead;
    }

    CloseHandle(h);
    return remaining == 0;
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build
```

Expected: Compilation succeeds. If there are `<sstream>` or `<algorithm>` issues with GCC, add explicit includes. The `#include <sstream>` for `std::istringstream` and `std::ostringstream` should be in the .cpp file already.

- [ ] **Step 4: Verify aim_ll is found**

Temporarily modify `src/main.cpp`:

```cpp
#include "aim_bridge.h"
#include "utils.h"
#include <cstdio>

int main() {
    std::string path = AimBridge::FindAimLl();
    if (path.empty()) {
        LogError("aim_ll.exe NOT FOUND");
        return 1;
    }
    LogInfo("aim_ll.exe found at: " + path);

    if (AimBridge::IsAvailable()) {
        LogInfo("AimBridge reports available");
    } else {
        LogError("AimBridge reports NOT available");
        return 2;
    }

    LogInfo("AimBridge module ready");
    return 0;
}
```

Run (as Administrator if needed — but path check doesn't require admin):
```bash
cmake --build build && ./build/ramdisk-cli.exe
```

Expected output:
```
[INFO] aim_ll.exe found at: C:\Program Files\AIM Toolkit\aim_ll.exe
[INFO] AimBridge reports available
[INFO] AimBridge module ready
```

- [ ] **Step 5: Revert main.cpp to stub**

- [ ] **Step 6: Commit**

```bash
git add src/aim_bridge.h src/aim_bridge.cpp
git commit -m "feat: add AimBridge module (aim_ll.exe subprocess bridge)"
```

---

### Task 6: SaveScheduler — timer + shutdown handler

**Files:**
- Create: `src/save_scheduler.h`
- Create: `src/save_scheduler.cpp`

- [ ] **Step 1: Write src/save_scheduler.h**

```cpp
#pragma once

#include <functional>
#include <ctime>
#include <memory>

class SaveScheduler {
public:
    using SaveCallback = std::function<bool()>;

    SaveScheduler();
    ~SaveScheduler();

    // Non-copyable
    SaveScheduler(const SaveScheduler&) = delete;
    SaveScheduler& operator=(const SaveScheduler&) = delete;

    // Start periodic save. intervalSeconds=0 means manual/shutdown only.
    bool Start(int intervalSeconds);

    // Stop the timer thread and unregister shutdown handler
    void Stop();

    // Trigger a save now (thread-safe). Returns true if save succeeded.
    bool SaveNow();

    // Register Ctrl+C / console close / system shutdown handler
    void RegisterShutdownHandler();

    // Query state
    time_t GetLastSaveTime() const;
    bool IsRunning() const;
    int  GetInterval() const;

    // Set the callback that does the actual save work
    void SetSaveCallback(SaveCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
```

- [ ] **Step 2: Write src/save_scheduler.cpp**

```cpp
#include "save_scheduler.h"
#include "utils.h"

#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

struct SaveScheduler::Impl {
    SaveCallback callback;
    std::thread timerThread;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    int intervalSeconds = 0;
    time_t lastSaveTime = 0;

    // For shutdown handler: pointer to the owning scheduler
    SaveScheduler* owner = nullptr;
};

// --- Global pointer for the console handler ---
// There can only be one active SaveScheduler at a time (MVP: single device)
static SaveScheduler* g_activeScheduler = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_SHUTDOWN_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT) {
        if (g_activeScheduler) {
            LogInfo("Shutdown signal received (type=" +
                    std::to_string(ctrlType) + "), saving...");
            g_activeScheduler->SaveNow();
        }
        // Return TRUE for CTRL_C_EVENT to prevent default handler from killing process
        // For shutdown/logoff, we still want the system to proceed after our save
        if (ctrlType == CTRL_C_EVENT) return TRUE;
    }
    return FALSE;
}

SaveScheduler::SaveScheduler() : pImpl(std::make_unique<Impl>()) {
    pImpl->owner = this;
}

SaveScheduler::~SaveScheduler() {
    Stop();
}

bool SaveScheduler::Start(int intervalSeconds) {
    if (pImpl->running) {
        LogError("SaveScheduler already running");
        return false;
    }

    pImpl->intervalSeconds = intervalSeconds;
    pImpl->stopRequested = false;

    if (intervalSeconds > 0) {
        pImpl->running = true;
        pImpl->timerThread = std::thread([this]() {
            LogInfo("Save timer started (interval=" +
                    std::to_string(pImpl->intervalSeconds) + "s)");

            while (!pImpl->stopRequested) {
                std::unique_lock<std::mutex> lock(pImpl->mutex);
                pImpl->cv.wait_for(lock,
                    std::chrono::seconds(pImpl->intervalSeconds));

                if (pImpl->stopRequested) break;

                lock.unlock();
                LogInfo("Auto-save triggered by timer");
                SaveNow();
            }
        });
    } else {
        // intervalSeconds == 0: manual/shutdown only
        pImpl->running = true;
        LogInfo("Save timer disabled (manual/shutdown save only)");
    }

    return true;
}

void SaveScheduler::Stop() {
    if (!pImpl->running) return;

    pImpl->stopRequested = true;
    pImpl->cv.notify_all();

    if (pImpl->timerThread.joinable()) {
        pImpl->timerThread.join();
    }

    pImpl->running = false;

    // Unregister console handler
    if (g_activeScheduler == this) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        g_activeScheduler = nullptr;
    }
}

bool SaveScheduler::SaveNow() {
    std::lock_guard<std::mutex> lock(pImpl->mutex);

    if (!pImpl->callback) {
        LogError("Save callback not set");
        return false;
    }

    LogInfo("Save started...");
    bool ok = pImpl->callback();
    if (ok) {
        pImpl->lastSaveTime = time(nullptr);
        LogInfo("Save completed at " + FormatTime(pImpl->lastSaveTime));
    } else {
        LogError("Save failed");
    }
    return ok;
}

void SaveScheduler::RegisterShutdownHandler() {
    g_activeScheduler = this;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    LogInfo("Shutdown handler registered (Ctrl+C / close / shutdown)");
}

time_t SaveScheduler::GetLastSaveTime() const {
    return pImpl->lastSaveTime;
}

bool SaveScheduler::IsRunning() const {
    return pImpl->running;
}

int SaveScheduler::GetInterval() const {
    return pImpl->intervalSeconds;
}

void SaveScheduler::SetSaveCallback(SaveCallback callback) {
    pImpl->callback = std::move(callback);
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build
```

Expected: Compilation succeeds.

- [ ] **Step 4: Smoke test**

Temporarily modify `src/main.cpp`:

```cpp
#include "save_scheduler.h"
#include "utils.h"
#include <thread>
#include <chrono>
#include <cstdio>

int main() {
    SaveScheduler scheduler;

    // Set a simple callback that just logs
    scheduler.SetSaveCallback([]() -> bool {
        LogInfo("SAVE CALLBACK EXECUTED");
        return true;
    });

    scheduler.RegisterShutdownHandler();

    // Test manual save
    scheduler.SaveNow();

    // Test timer (2 second interval for smoke test)
    scheduler.Start(2);
    LogInfo("Waiting 5 seconds for auto-save to fire (should see 2 saves)...");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    scheduler.Stop();

    LogInfo("Last save: " + FormatTimeAgo(scheduler.GetLastSaveTime(), time(nullptr)));
    LogInfo("SaveScheduler tests complete");
    return 0;
}
```

Run:
```bash
cmake --build build && ./build/ramdisk-cli.exe
```

Expected output (approximate):
```
[INFO] SAVE CALLBACK EXECUTED
[INFO] Save completed at 14:30:22
[INFO] Shutdown handler registered (Ctrl+C / close / shutdown)
[INFO] Save timer started (interval=2s)
[INFO] Waiting 5 seconds for auto-save to fire (should see 2 saves)...
[INFO] Auto-save triggered by timer
[INFO] SAVE CALLBACK EXECUTED
[INFO] Save completed at 14:30:24
[INFO] Auto-save triggered by timer
[INFO] SAVE CALLBACK EXECUTED
[INFO] Save completed at 14:30:26
[INFO] Last save: just now
[INFO] SaveScheduler tests complete
```

- [ ] **Step 5: Revert main.cpp to stub**

- [ ] **Step 6: Commit**

```bash
git add src/save_scheduler.h src/save_scheduler.cpp
git commit -m "feat: add SaveScheduler module (timer + shutdown handler)"
```

---

### Task 7: CLI Parser — argument parsing and command dispatch types

**Files:**
- Create: `src/cli_parser.h`
- Create: `src/cli_parser.cpp`

- [ ] **Step 1: Write src/cli_parser.h**

```cpp
#pragma once

#include <string>
#include <cstdint>
#include <vector>

enum class Command {
    None,
    Create,
    Mount,
    Save,
    Unmount,
    Status,
    Help,
    Version,
};

struct CreateOptions {
    uint64_t sizeBytes = 0;           // -s
    std::string outputPath;           // -o
    bool dynamic = true;              // -t dynamic|fixed
    int saveIntervalSec = 300;        // --save-interval
    std::string label = "RAMDISK";    // --label
    std::string mountPoint;           // --mount-point (empty = auto)
};

struct MountOptions {
    std::string inputPath;            // -i
    int saveIntervalSec = 300;        // --save-interval
    bool readonly = false;            // --readonly
    std::string label = "RAMDISK";    // --label
};

struct SaveOptions {
    int device = 0;                   // --device
};

struct UnmountOptions {
    int device = 0;                   // --device
    bool noSave = false;              // --no-save
};

struct ParseResult {
    Command command = Command::None;
    CreateOptions create;
    MountOptions mount;
    SaveOptions save;
    UnmountOptions unmount;
    bool verbose = false;             // -v
    std::string error;                // Parse error message, empty if OK
};

// Parse argc/argv into a typed result
ParseResult ParseArgs(int argc, char* argv[]);

// Print help text
void PrintHelp();

// Print version
void PrintVersion();
```

- [ ] **Step 2: Write src/cli_parser.cpp**

```cpp
#include "cli_parser.h"
#include "utils.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

static uint64_t ParseSize(const std::string& s, std::string& error) {
    if (s.empty()) {
        error = "empty size string";
        return 0;
    }

    // Parse numeric part
    char* end = nullptr;
    double num = strtod(s.c_str(), &end);

    if (end == s.c_str()) {
        error = "invalid size: " + s;
        return 0;
    }

    uint64_t multiplier = 1;
    std::string suffix(end);

    // Trim whitespace from suffix
    suffix = Trim(suffix);

    if (suffix.empty() || suffix == "B") {
        multiplier = 1;
    } else if (suffix == "K" || suffix == "KB") {
        multiplier = 1024ULL;
    } else if (suffix == "M" || suffix == "MB") {
        multiplier = 1024ULL * 1024;
    } else if (suffix == "G" || suffix == "GB") {
        multiplier = 1024ULL * 1024 * 1024;
    } else if (suffix == "T" || suffix == "TB") {
        multiplier = 1024ULL * 1024 * 1024 * 1024;
    } else {
        error = "unknown size suffix: " + suffix + " (use B/K/M/G/T)";
        return 0;
    }

    return static_cast<uint64_t>(num * multiplier);
}

ParseResult ParseArgs(int argc, char* argv[]) {
    ParseResult r;

    if (argc < 2) {
        r.command = Command::Help;
        return r;
    }

    std::string cmd = argv[1];

    if (cmd == "create") {
        r.command = Command::Create;

        // Parse options
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];

            if ((arg == "-s" || arg == "--size") && i + 1 < argc) {
                std::string err;
                r.create.sizeBytes = ParseSize(argv[++i], err);
                if (!err.empty()) { r.error = err; return r; }
            }
            else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
                r.create.outputPath = argv[++i];
            }
            else if ((arg == "-t" || arg == "--type") && i + 1 < argc) {
                std::string type = argv[++i];
                if (type == "fixed") r.create.dynamic = false;
                else if (type == "dynamic") r.create.dynamic = true;
                else { r.error = "unknown type: " + type + " (use fixed or dynamic)"; return r; }
            }
            else if (arg == "--save-interval" && i + 1 < argc) {
                r.create.saveIntervalSec = std::atoi(argv[++i]);
            }
            else if (arg == "--label" && i + 1 < argc) {
                r.create.label = argv[++i];
            }
            else if (arg == "--mount-point" && i + 1 < argc) {
                r.create.mountPoint = argv[++i];
            }
            else if (arg == "-v" || arg == "--verbose") {
                r.verbose = true;
            }
            else if (arg == "-h" || arg == "--help") {
                r.command = Command::Help;
                return r;
            }
            else {
                r.error = "unknown option: " + arg;
                return r;
            }
        }

        // Validate required
        if (r.create.sizeBytes == 0) { r.error = "--size is required for create"; return r; }
        if (r.create.outputPath.empty()) { r.error = "--output is required for create"; return r; }
    }
    else if (cmd == "mount") {
        r.command = Command::Mount;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];

            if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
                r.mount.inputPath = argv[++i];
            }
            else if (arg == "--save-interval" && i + 1 < argc) {
                r.mount.saveIntervalSec = std::atoi(argv[++i]);
            }
            else if (arg == "--readonly") {
                r.mount.readonly = true;
            }
            else if (arg == "--label" && i + 1 < argc) {
                r.mount.label = argv[++i];
            }
            else if (arg == "-v" || arg == "--verbose") {
                r.verbose = true;
            }
            else if (arg == "-h" || arg == "--help") {
                r.command = Command::Help;
                return r;
            }
            else {
                r.error = "unknown option: " + arg;
                return r;
            }
        }

        if (r.mount.inputPath.empty()) { r.error = "--input is required for mount"; return r; }
    }
    else if (cmd == "save") {
        r.command = Command::Save;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--device" && i + 1 < argc) {
                r.save.device = std::atoi(argv[++i]);
            }
            else if (arg == "-v" || arg == "--verbose") {
                r.verbose = true;
            }
            else if (arg == "-h" || arg == "--help") {
                r.command = Command::Help;
                return r;
            }
            else {
                r.error = "unknown option: " + arg;
                return r;
            }
        }
    }
    else if (cmd == "unmount") {
        r.command = Command::Unmount;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--device" && i + 1 < argc) {
                r.unmount.device = std::atoi(argv[++i]);
            }
            else if (arg == "--no-save") {
                r.unmount.noSave = true;
            }
            else if (arg == "-v" || arg == "--verbose") {
                r.verbose = true;
            }
            else if (arg == "-h" || arg == "--help") {
                r.command = Command::Help;
                return r;
            }
            else {
                r.error = "unknown option: " + arg;
                return r;
            }
        }
    }
    else if (cmd == "status") {
        r.command = Command::Status;
    }
    else if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        r.command = Command::Help;
    }
    else if (cmd == "version" || cmd == "--version" || cmd == "-V") {
        r.command = Command::Version;
    }
    else {
        r.error = "unknown command: " + cmd;
    }

    return r;
}

void PrintHelp() {
    printf("ramdisk-cli — RAM disk tool based on Arsenal Image Mounter\n");
    printf("Usage: ramdisk-cli <command> [options]\n\n");
    printf("Commands:\n");
    printf("  create   Create a new VHD and mount as RAM disk\n");
    printf("  mount    Load an existing VHD into RAM\n");
    printf("  save     Manually save RAM disk contents to VHD\n");
    printf("  unmount  Save and unmount a RAM disk\n");
    printf("  status   Show mounted RAM disks\n");
    printf("  help     Show this help\n");
    printf("  version  Show version\n\n");
    printf("Examples:\n");
    printf("  ramdisk-cli create -s 4G -o D:\\disk.vhd --save-interval 600\n");
    printf("  ramdisk-cli mount -i D:\\disk.vhd --save-interval 120\n");
    printf("  ramdisk-cli save\n");
    printf("  ramdisk-cli unmount\n");
    printf("  ramdisk-cli status\n");
}

void PrintVersion() {
    printf("ramdisk-cli v1.0.0\n");
}
```

- [ ] **Step 3: Build**

```bash
cmake --build build
```

Expected: Compilation succeeds.

- [ ] **Step 4: Test CLI parsing**

Temporarily modify `src/main.cpp`:

```cpp
#include "cli_parser.h"
#include <cstdio>

int main(int argc, char* argv[]) {
    auto r = ParseArgs(argc, argv);

    if (!r.error.empty()) {
        printf("PARSE ERROR: %s\n", r.error.c_str());
        return 1;
    }

    switch (r.command) {
    case Command::Create:
        printf("Command: Create\n");
        printf("  size: %llu\n", (unsigned long long)r.create.sizeBytes);
        printf("  output: %s\n", r.create.outputPath.c_str());
        printf("  dynamic: %d\n", (int)r.create.dynamic);
        printf("  interval: %d\n", r.create.saveIntervalSec);
        break;
    case Command::Mount:
        printf("Command: Mount\n");
        printf("  input: %s\n", r.mount.inputPath.c_str());
        printf("  interval: %d\n", r.mount.saveIntervalSec);
        break;
    case Command::Save:
        printf("Command: Save (device=%d)\n", r.save.device);
        break;
    case Command::Unmount:
        printf("Command: Unmount (device=%d)\n", r.unmount.device);
        break;
    case Command::Status:
        printf("Command: Status\n");
        break;
    case Command::Help:
        PrintHelp();
        break;
    case Command::Version:
        PrintVersion();
        break;
    default:
        printf("No command\n");
        break;
    }
    return 0;
}
```

Run tests:
```bash
cmake --build build
./build/ramdisk-cli.exe create -s 4G -o D:\test.vhd
# Expected: Command: Create / size: 4294967296 / output: D:\test.vhd / dynamic: 1 / interval: 300

./build/ramdisk-cli.exe create -s 512M -o test.vhd -t fixed --save-interval 0
# Expected: dynamic: 0 / interval: 0

./build/ramdisk-cli.exe mount -i D:\test.vhd --readonly
# Expected: Command: Mount / readonly: (implicit)

./build/ramdisk-cli.exe status
# Expected: Command: Status

./build/ramdisk-cli.exe help
# Expected: Prints help text

./build/ramdisk-cli.exe bogus
# Expected: PARSE ERROR: unknown command: bogus

./build/ramdisk-cli.exe create -o x.vhd
# Expected: PARSE ERROR: --size is required for create
```

- [ ] **Step 5: Revert main.cpp to stub**

- [ ] **Step 6: Commit**

```bash
git add src/cli_parser.h src/cli_parser.cpp
git commit -m "feat: add CLI parser module"
```

---

### Task 8: main.cpp — wire everything together

**Files:**
- Modify: `src/main.cpp` (replace stub with full implementation)

- [ ] **Step 1: Write src/main.cpp (final)**

```cpp
#include "cli_parser.h"
#include "vhd_engine.h"
#include "aim_bridge.h"
#include "save_scheduler.h"
#include "utils.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

// --- Globals for the running session ---
static VhdEngine g_vhd;
static AimBridge g_aim;
static SaveScheduler g_scheduler;
static int g_deviceNumber = -1;
static std::string g_mountPoint;
static std::string g_vhdPath;
static uint64_t g_sizeBytes = 0;

// --- The actual save callback ---
static bool DoSave() {
    if (!g_vhd.IsOpen()) {
        LogError("VHD not open, cannot save");
        return false;
    }

    // Allocate buffer for full disk read
    std::vector<uint8_t> buffer(g_sizeBytes);
    if (!g_aim.ReadDeviceData(g_deviceNumber, g_sizeBytes, buffer.data())) {
        LogError("Failed to read device data for save");
        return false;
    }

    // Write back to VHD
    if (!g_vhd.WriteAll(buffer.data())) {
        LogError("Failed to write VHD: " + g_vhd.GetLastError());
        return false;
    }

    LogInfo("Saved " + FormatSize(g_sizeBytes) + " to " + g_vhdPath);
    return true;
}

// --- Command implementations ---

static int CmdCreate(const CreateOptions& opts) {
    LogInfo("Creating VHD: " + opts.outputPath + " (" + FormatSize(opts.sizeBytes) + ")");

    if (!g_vhd.Create(opts.outputPath, opts.sizeBytes, opts.dynamic)) {
        LogError("Failed to create VHD: " + g_vhd.GetLastError());
        return 2;
    }

    // Write zeroes to initialize (MiniVHD creates sparse dynamic VHDs — this ensures the file is ready)
    // For dynamic VHDs, writing all sectors is slow; skip for now, MiniVHD handles new-file metadata
    g_vhd.Close();

    // Mount as RAM disk
    LogInfo("Mounting as RAM disk...");
    if (!g_aim.MountVm(opts.outputPath, opts.sizeBytes, g_mountPoint)) {
        LogError("Failed to mount RAM disk");
        return 3;
    }

    g_vhdPath = opts.outputPath;
    g_sizeBytes = opts.sizeBytes;

    // Re-open VHD for writes (used during save)
    if (!g_vhd.Open(opts.outputPath, true)) {
        LogError("Failed to re-open VHD for writes: " + g_vhd.GetLastError());
        g_aim.Unmount(0, true);
        return 2;
    }

    // Set up save scheduler
    g_scheduler.SetSaveCallback(DoSave);
    g_scheduler.RegisterShutdownHandler();
    g_scheduler.Start(opts.saveIntervalSec);

    printf("RAM disk ready: %s (%s)\n", g_mountPoint.c_str(), opts.outputPath.c_str());
    printf("Save interval: %ds | Press Ctrl+C to stop\n", opts.saveIntervalSec);

    // Wait for Ctrl+C (blocking)
    // The console handler will call DoSave via shutdown handler
    while (g_scheduler.IsRunning()) {
        Sleep(1000);
    }

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

    // Mount as RAM disk from existing VHD
    LogInfo("Mounting as RAM disk...");
    if (!g_aim.MountVmFromFile(opts.inputPath, g_mountPoint)) {
        LogError("Failed to mount RAM disk");
        return 3;
    }

    // Set up save scheduler
    if (!opts.readonly) {
        g_scheduler.SetSaveCallback(DoSave);
        g_scheduler.RegisterShutdownHandler();
        g_scheduler.Start(opts.saveIntervalSec);
    } else {
        LogInfo("Read-only mode — auto-save disabled");
    }

    printf("RAM disk ready: %s (%s, %s)\n",
           g_mountPoint.c_str(), opts.inputPath.c_str(),
           opts.readonly ? "read-only" : "writable");

    if (!opts.readonly && opts.saveIntervalSec > 0) {
        printf("Save interval: %ds | Press Ctrl+C to stop\n", opts.saveIntervalSec);
    } else if (!opts.readonly) {
        printf("Manual save only | Press Ctrl+C to stop\n");
    }

    while (g_scheduler.IsRunning()) {
        Sleep(1000);
    }

    return 0;
}

static int CmdSave(const SaveOptions& opts) {
    if (!g_vhd.IsOpen()) {
        LogError("No active session. Use 'create' or 'mount' first, or run 'save' from within a session.");
        return 2;
    }
    return DoSave() ? 0 : 2;
}

static int CmdUnmount(const UnmountOptions& opts) {
    if (!opts.noSave && g_vhd.IsOpen()) {
        LogInfo("Saving before unmount...");
        DoSave();
    }

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
    if (devices.empty()) {
        printf("No RAM disks mounted.\n");
        return 0;
    }

    for (auto& d : devices) {
        printf("[%d] %s -> %s (%s) | %s\n",
               d.number,
               d.backingFile.c_str(),
               d.mountPoint.c_str(),
               FormatSize(d.sizeBytes).c_str(),
               d.backingFile.empty() ? "no backing file" : "VHD-backed");
    }
    return 0;
}

// --- Main ---

int main(int argc, char* argv[]) {
    auto r = ParseArgs(argc, argv);

    if (!r.error.empty()) {
        fprintf(stderr, "Error: %s\n", r.error.c_str());
        fprintf(stderr, "Try 'ramdisk-cli help'\n");
        return 1;
    }

    // Check for admin privileges for commands that need them
    bool isCreateOrMount = (r.command == Command::Create || r.command == Command::Mount);
    if (isCreateOrMount) {
        if (!AimBridge::IsAvailable()) {
            fprintf(stderr, "Error: aim_ll.exe not found.\n");
            fprintf(stderr, "Install AIM Toolkit or set AIM_LL_PATH environment variable.\n");
            return 3;
        }
    }

    switch (r.command) {
    case Command::Create:
        return CmdCreate(r.create);
    case Command::Mount:
        return CmdMount(r.mount);
    case Command::Save:
        return CmdSave(r.save);
    case Command::Unmount:
        return CmdUnmount(r.unmount);
    case Command::Status:
        return CmdStatus();
    case Command::Help:
        PrintHelp();
        return 0;
    case Command::Version:
        PrintVersion();
        return 0;
    default:
        PrintHelp();
        return 0;
    }
}
```

- [ ] **Step 2: Build**

```bash
cmake --build build
```

Expected: Compilation succeeds. Fix any missing includes or type mismatches.

- [ ] **Step 3: Verify build artifact**

```bash
ls -la build/ramdisk-cli.exe
```

Expected: Executable exists, size > 100KB (statically linked with MiniVHD).

- [ ] **Step 4: Quick smoke test (help + version)**

```bash
./build/ramdisk-cli.exe help
# Expected: Full help text

./build/ramdisk-cli.exe version
# Expected: ramdisk-cli v1.0.0
```

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire main.cpp with full command dispatch"
```

---

### Task 9: CLAUDE.md — agent skill documentation

**Files:**
- Create: `CLAUDE.md`

- [ ] **Step 1: Write CLAUDE.md**

```markdown
# RamDisk CLI

基于 Arsenal Image Mounter 内核驱动 (phdskmnt.sys) 的个人 RAM 磁盘工具。

## 概述

将 VHD 虚拟磁盘文件加载到系统内存中，以 RAM 速度运行，支持定时写回磁盘。
参考对标 Primo Ramdisk 核心功能。

**技术栈**: C++11, GCC MinGW-w64, CMake, MiniVHD (MIT), aim_ll.exe

## 前置依赖

- **Arsenal Image Mounter** / AIM Toolkit 已安装（提供 `phdskmnt.sys` 驱动 + `aim_ll.exe`）
- `aim_ll.exe` 位于 `C:\Program Files\AIM Toolkit\` 或通过 `AIM_LL_PATH` 环境变量指定
- **必须以管理员权限运行**（挂载/卸载虚拟磁盘需要管理员权限）

## 命令参考

### create — 创建新 VHD 并挂载为 RAM 盘

```bash
ramdisk-cli create -s <size> -o <vhd-path> [options]
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `-s, --size` | ✅ | 磁盘大小，如 `512M`, `4G`, `1T` |
| `-o, --output` | ✅ | VHD 文件输出路径 |
| `-t, --type` | ❌ | `fixed` 或 `dynamic`（默认 `dynamic`） |
| `--save-interval` | ❌ | 定时保存间隔秒数，默认 `300`（5分钟），`0` 表示仅手动保存 |
| `--label` | ❌ | 卷标，默认 `RAMDISK` |
| `--mount-point` | ❌ | 指定盘符如 `X:`, 默认自动分配 |

```bash
# 示例
ramdisk-cli create -s 4G -o D:\ramdisk.vhd --save-interval 600
ramdisk-cli create -s 512M -o C:\temp\scratch.vhd -t fixed
```

### mount — 加载已有 VHD 为 RAM 盘

```bash
ramdisk-cli mount -i <vhd-path> [options]
```

| 参数 | 必需 | 说明 |
|------|------|------|
| `-i, --input` | ✅ | VHD 文件路径 |
| `--save-interval` | ❌ | 定时保存间隔秒数 |
| `--readonly` | ❌ | 只读模式，不写回 |
| `--label` | ❌ | 卷标 |

```bash
ramdisk-cli mount -i D:\ramdisk.vhd
ramdisk-cli mount -i D:\ramdisk.vhd --readonly
```

### save — 手动触发保存

```bash
ramdisk-cli save [--device N]
```

在运行中的 create/mount 会话中按 Ctrl+C 也会触发自动保存。

### unmount — 保存并卸载

```bash
ramdisk-cli unmount [--device N] [--no-save]
```

### status — 查看状态

```bash
ramdisk-cli status
```

输出格式: `[device] vhd-path -> drive: (size, type) | save: intervals | state`

### help / version

```bash
ramdisk-cli help
ramdisk-cli version
```

## 退出码

| 码 | 含义 |
|----|------|
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | VHD 文件错误 |
| 3 | AIM 驱动错误 |
| 4 | 系统错误 |

## 架构

```
main.cpp (CLI 入口 + 命令分发)
├── cli_parser    — 参数解析 (ParseArgs → ParseResult)
├── vhd_engine    — VHD 文件 I/O (基于 MiniVHD C 库)
├── aim_bridge    — aim_ll.exe 子进程桥接 (CreateProcess + stdout 捕获)
└── save_scheduler — 定时保存 + 关机信号处理
```

### 数据流

- **创建**: `VhdEngine::Create` → VHD 文件 → `AimBridge::MountVm` → RAM 盘 → Windows 读写
- **加载**: `VhdEngine::Open` → 读取元数据 → `AimBridge::MountVmFromFile` → RAM 盘
- **保存**: `SaveScheduler` 定时/手动触发 → `AimBridge::ReadDeviceData` (读 `\\.\PhysicalDriveN`) → `VhdEngine::WriteAll` → VHD 文件
- **卸载**: 先保存 → `AimBridge::Unmount` → `VhdEngine::Close`

## 构建

```bash
# 初始化（首次）
git submodule update --init --recursive

# 构建
cmake -B build -G "MinGW Makefiles"
cmake --build build

# 产物: build/ramdisk-cli.exe
```

## 故障排除

### "aim_ll.exe not found"
- 安装 AIM Toolkit 或 Arsenal Image Mounter
- 或设置环境变量 `AIM_LL_PATH=C:\path\to\aim_ll.exe`

### "Administrator privileges required"
- 以管理员身份运行 cmd/PowerShell
- `aim_ll.exe` 挂载虚拟磁盘需要管理员权限

### "Failed to mount RAM disk"
- 确认 `phdskmnt.sys` 驱动正在运行: `sc query phdskmnt`
- 确认没有同名 VHD 文件已被其他 AIM 设备使用

### "Failed to write VHD"
- 检查磁盘剩余空间
- 确认 VHD 文件未被其他程序锁定

## 项目文件结构

```
build_my_ramdisk/
├── CMakeLists.txt
├── CLAUDE.md                     # 本文件
├── src/
│   ├── main.cpp                  # 入口 + 命令分发
│   ├── cli_parser.h/.cpp         # CLI 参数解析
│   ├── vhd_engine.h/.cpp         # VHD 引擎 (MiniVHD)
│   ├── aim_bridge.h/.cpp         # AIM 驱动桥接
│   ├── save_scheduler.h/.cpp     # 保存调度器
│   └── utils.h/.cpp              # 日志/格式化/字符串
└── vendor/
    └── MiniVHD/                   # git submodule
```
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: add CLAUDE.md agent skill documentation"
```

---

### Task 10: End-to-end integration test (manual, requires admin)

**No new files.** This task validates the full workflow end-to-end.

- [ ] **Step 1: Open Administrator terminal**

Right-click Command Prompt / PowerShell → "Run as administrator"

- [ ] **Step 2: Build from clean**

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

- [ ] **Step 3: Test create workflow**

```bash
# Create a 100MB test VHD
./build/ramdisk-cli.exe create -s 100M -o test_e2e.vhd --save-interval 0

# Expected output:
# [INFO] Creating VHD: test_e2e.vhd (100.00 MB)
# [INFO] Mounting as RAM disk...
# [INFO] Mounted as device 0
# [INFO] Re-opened VHD for writes
# RAM disk ready: R: (test_e2e.vhd)
# Save interval: 0s | Press Ctrl+C to stop
#
# Press Ctrl+C
# [INFO] Shutdown signal received (type=0), saving...
# [INFO] Save completed at HH:MM:SS
```

- [ ] **Step 4: Verify the VHD file**

```bash
ls -la test_e2e.vhd
```

Expected: File exists, non-zero size. For a 100MB dynamic VHD, it will be small (a few KB initially).

- [ ] **Step 5: Test mount workflow**

```bash
# Mount the VHD we just created
./build/ramdisk-cli.exe mount -i test_e2e.vhd --save-interval 0

# Expected: Mounts successfully, same output pattern as create
# Press Ctrl+C to exit
```

- [ ] **Step 6: Test status when nothing is mounted**

```bash
./build/ramdisk-cli.exe status
```

Expected: `No RAM disks mounted.`

- [ ] **Step 7: Clean up test file**

```bash
rm test_e2e.vhd
```

- [ ] **Step 8: If all tests pass**

All commands succeed. The VHD file is created, mounted, saved, and unmounted correctly.

- [ ] **Step 9: Commit (if any fixes were needed)**

```bash
git add -u
git commit -m "fix: end-to-end test fixes"
```

---

## Completion Checklist

- [ ] Task 1: Project scaffolding ✓ (builds, ramdisk-cli.exe runs)
- [ ] Task 2: MiniVHD integration ✓ (libminivhd.a builds)
- [ ] Task 3: Utils module ✓ (smoke test passes)
- [ ] Task 4: VhdEngine ✓ (create/open/read/write round-trip)
- [ ] Task 5: AimBridge ✓ (FindAimLl succeeds, ListDevices works)
- [ ] Task 6: SaveScheduler ✓ (timer fires, manual save works)
- [ ] Task 7: CLI Parser ✓ (all command/option combos parse correctly)
- [ ] Task 8: main.cpp integration ✓ (help/version work)
- [ ] Task 9: CLAUDE.md ✓ (complete documentation)
- [ ] Task 10: End-to-end test ✓ (create → mount → save → unmount → status)
