# M0 Driver-only PoC Implementation Plan

> **Superseded:** 本计划保留为历史初稿。新的规范性 M0 SPEC 是 `docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md`，新的 Agent 可执行计划是 `docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md`。后续实现不得混用两版步骤。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and repeatedly verify a 64 MiB AIM Shared Memory Proxy disk using only our C++20 executable and the installed signed `phdskmnt.sys`, without loading or launching AIM Toolkit user-mode components.

**Architecture:** A native MSVC C++20 executable owns a zero-initialized memory block device and an IMDPROXY shared-memory server. A direct SCSI miniport adapter sends `IOCTL_SCSI_MINIPORT` requests to create, query, and remove exactly one proxy device; a test harness stops and restores `deviosvc`, performs raw-sector round trips, and records machine-readable evidence.

**Tech Stack:** C++20, MSVC 19.44+, Windows SDK 10.0.26100, CMake 4.2+, Visual Studio 17 2022 generator, CTest, Win32 SetupAPI/Configuration Manager, `DeviceIoControl`, file mappings/events/mutexes, BCrypt SHA-256, PowerShell E2E harness.

**Approved spec:** `docs/superpowers/specs/2026-07-31-aim-driver-only-clean-slate-design.md`

---

## Scope and safety gates

This plan implements only M0. It does not implement VHD, dynamic allocation, TRIM, checkpointing, a Windows Service, an installer, or Toolkit removal.

The execution must obey these safety rules:

1. Do not uninstall AIM Toolkit or any driver.
2. Do not call `aim_ll.exe`, load `aimapi.dll`, or start RamDyn.
3. Never send `IMSCSI_ALL_DEVICES`/`0x00FFFFFF` to the remove operation.
4. Remove only the device number returned by this process's successful create request.
5. Record the AIM device list before and after every real E2E cycle.
6. Stop `deviosvc` only inside a PowerShell `try/finally`; restore its original running state before the script exits.
7. If create succeeds and any later step fails, attempt exact-device cleanup before returning failure.
8. Do not write sector 0. The raw round trip uses offset 8 MiB and length 4096 bytes on the disposable 64 MiB proxy disk.
9. A real E2E command must require an elevated administrator token and refuse to run otherwise.

## File map

```text
/
├── .gitignore                         build and report exclusions
├── CMakeLists.txt                     root build definition
├── CMakePresets.json                  reproducible MSVC configure/build/test presets
├── LICENSE                            AGPL-3.0-only text
├── README.md                          M0 purpose and commands
├── THIRD_PARTY.md                     AIM source/driver provenance
├── cmake/Warnings.cmake               strict MSVC warnings
├── docs/protocol/aim-m0-contract.md    audited binary/transport contract
├── src/CMakeLists.txt
├── src/common/error.h                  typed error values
├── src/common/result.h                 success/error transport
├── src/common/unique_handle.h          Win32 HANDLE RAII
├── src/common/sha256.h/.cpp            BCrypt SHA-256
├── src/common/authenticode.h/.cpp       driver signature and signer verification
├── src/storage/block_device.h          storage boundary
├── src/storage/memory_block_device.h/.cpp
├── src/proxy/imdproxy_protocol.h        protocol layouts/constants
├── src/proxy/request_processor.h/.cpp   pure request handling
├── src/proxy/shared_memory_channel.h/.cpp
├── src/proxy/shm_proxy_server.h/.cpp
├── src/driver/aim_protocol.h            miniport layouts/constants
├── src/driver/srb_transport.h           injectable control transport
├── src/driver/srb_codec.h/.cpp           SRB buffer construction/validation
├── src/driver/windows_srb_transport.h/.cpp
├── src/driver/adapter_discovery.h/.cpp
├── src/driver/direct_aim_adapter.h/.cpp
├── src/driver/physical_drive_locator.h/.cpp
├── src/poc/json_writer.h/.cpp
├── src/poc/poc_report.h/.cpp
├── src/poc/raw_round_trip.h/.cpp
├── src/poc/main.cpp
├── tests/CMakeLists.txt
├── tests/test_framework.h
├── tests/test_main.cpp
├── tests/common_tests.cpp
├── tests/protocol_tests.cpp
├── tests/memory_block_device_tests.cpp
├── tests/request_processor_tests.cpp
├── tests/shared_memory_channel_tests.cpp
├── tests/srb_codec_tests.cpp
├── tests/direct_aim_adapter_tests.cpp
├── tests/physical_drive_locator_tests.cpp
├── tests/report_tests.cpp
└── tests/e2e/run-m0-driver-only.ps1
```

## Task 1: Bootstrap the clean MSVC C++20 repository

**Files:**
- Create: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `cmake/Warnings.cmake`
- Create: `src/CMakeLists.txt`
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_framework.h`
- Create: `tests/test_main.cpp`
- Create: `tests/common_tests.cpp`
- Create: `README.md`
- Create: `LICENSE`
- Create: `THIRD_PARTY.md`

- [ ] **Step 1: Create the build and test skeleton**

Use this root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyRamDiskM0 VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "M0 requires the MSVC x64 toolchain")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "M0 supports x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(cmake/Warnings.cmake)
add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

Use this `cmake/Warnings.cmake`:

```cmake
function(myrd_enable_warnings target)
    target_compile_options(${target} PRIVATE
        /W4 /WX /permissive- /Zc:__cplusplus /EHsc /utf-8
        /wd4324
    )
endfunction()
```

Use this initial `src/CMakeLists.txt`:

```cmake
add_library(myrd_core STATIC)
target_sources(myrd_core PRIVATE common/error.h)
target_include_directories(myrd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
myrd_enable_warnings(myrd_core)
```

Use this initial `tests/CMakeLists.txt`:

```cmake
add_executable(myrd_tests
    test_main.cpp
    common_tests.cpp
)
target_link_libraries(myrd_tests PRIVATE myrd_core)
target_include_directories(myrd_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
myrd_enable_warnings(myrd_tests)
add_test(NAME unit COMMAND myrd_tests)
```

- [ ] **Step 2: Add a dependency-free test framework**

Create `tests/test_framework.h`:

```cpp
#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {
using Function = void (*)();

struct Case {
    const char* name;
    Function function;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, Function function) {
        registry().push_back(Case{name, function});
    }
};

inline void require(bool condition, const char* expression,
                    const char* file, int line) {
    if (!condition) {
        std::ostringstream message;
        message << file << ':' << line << ": requirement failed: " << expression;
        throw std::runtime_error(message.str());
    }
}
}  // namespace test

#define TEST_CASE(name)                                                \
    static void name();                                                \
    static ::test::Registrar registrar_##name(#name, &name);          \
    static void name()

#define REQUIRE(expression)                                            \
    ::test::require(static_cast<bool>(expression), #expression,        \
                    __FILE__, __LINE__)
```

Create `tests/test_main.cpp`:

```cpp
#include "test_framework.h"

#include <exception>
#include <iostream>

int main() {
    int failed = 0;
    for (const auto& item : test::registry()) {
        try {
            item.function();
            std::cout << "PASS " << item.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << item.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (test::registry().size() - failed) << "/"
              << test::registry().size() << " passed\n";
    return failed == 0 ? 0 : 1;
}
```

Create `tests/common_tests.cpp`:

```cpp
#include "test_framework.h"

TEST_CASE(repository_bootstrap_smoke) {
    REQUIRE(true);
}
```

- [ ] **Step 3: Add reproducible MSVC presets**

Create `CMakePresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "vs2022-debug",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/out/build/vs2022-debug",
      "cacheVariables": {
        "CMAKE_CONFIGURATION_TYPES": "Debug;Release",
        "BUILD_TESTING": "ON"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "vs2022-debug",
      "configurePreset": "vs2022-debug",
      "configuration": "Debug"
    }
  ],
  "testPresets": [
    {
      "name": "vs2022-debug",
      "configurePreset": "vs2022-debug",
      "configuration": "Debug",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

- [ ] **Step 4: Add repository metadata**

Create `.gitignore`:

```gitignore
/out/
/reports/
*.user
*.suo
*.VC.db
*.VC.VC.opendb
```

Create `README.md` with these exact M0 commands:

````markdown
# MyRamDisk vNext

This branch contains the M0 Driver-only proof of concept defined in
`docs/superpowers/specs/2026-07-31-aim-driver-only-clean-slate-design.md`.

M0 does not uninstall AIM Toolkit and does not implement a production RAM disk.

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```
````

Download the canonical AGPL v3 text without changing it:

```powershell
Invoke-WebRequest https://www.gnu.org/licenses/agpl-3.0.txt -OutFile LICENSE
```

Create `THIRD_PARTY.md`:

```markdown
# Third-party provenance

## Arsenal Image Mounter

- Upstream: https://github.com/ArsenalRecon/Arsenal-Image-Mounter
- Audited commit: `d379caa027d21902e45fce8c9daf02fd116c2dda`
- Protocol reference: commit
  `d379caa027d21902e45fce8c9daf02fd116c2dda` (upstream API 1.2.29.95).
- `common.h`, `proxy.cpp`, and `DevioShmService.cs` state Arsenal Recon
  AGPL-3.0/open-source terms. `imdproxy.h` is a separate ImDisk declaration
  by Olof Lagerkvist under its MIT-style notice; do not classify it as AGPL.
- Accepted runtime driver source package: commit
  `cb7a2347e9595b12e2bd178ce803090a7a674037`,
  `DriverSetup/DriverFiles.zip` SHA-256
  `B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324`;
  member `Win10/x64/phdskmnt.sys`, version 1.2.28.93, SHA-256
  `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9`.
- The local ABI/protocol declarations adapt the published declarations; no AIM
  user-mode implementation body is copied.

No AIM user-mode binary or `phdskmnt.sys` is linked, loaded from the project,
or redistributed by M0.
```

- [ ] **Step 5: Configure, build, and run the smoke test**

Run:

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

Expected: configure and build exit 0; CTest reports `100% tests passed, 0 tests failed`.

- [ ] **Step 6: Commit the bootstrap**

```powershell
git add .gitignore CMakeLists.txt CMakePresets.json LICENSE README.md THIRD_PARTY.md cmake src tests
git commit -m "build: bootstrap MSVC C++20 M0 project"
```

## Task 2: Add typed errors, results, and HANDLE ownership

**Files:**
- Create: `src/common/error.h`
- Create: `src/common/result.h`
- Create: `src/common/unique_handle.h`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/common_tests.cpp`

- [ ] **Step 1: Write failing ownership and result tests**

Append to `tests/common_tests.cpp`:

```cpp
#include "common/result.h"
#include "common/unique_handle.h"

#include <windows.h>

TEST_CASE(result_carries_success_and_error) {
    auto success = myrd::Result<int>::success(42);
    REQUIRE(success.ok());
    REQUIRE(success.value() == 42);

    auto failure = myrd::Result<int>::failure(
        {myrd::ErrorCode::invalid_argument, 0, L"bad input"});
    REQUIRE(!failure.ok());
    REQUIRE(failure.error().code == myrd::ErrorCode::invalid_argument);
}

TEST_CASE(unique_handle_closes_owned_event) {
    HANDLE raw = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(raw != nullptr);
    {
        myrd::UniqueHandle owned(raw);
        REQUIRE(owned.get() == raw);
    }
    REQUIRE(SetEvent(raw) == FALSE);
    REQUIRE(GetLastError() == ERROR_INVALID_HANDLE);
}
```

- [ ] **Step 2: Run the test and verify the missing headers fail the build**

Run:

```powershell
cmake --build --preset vs2022-debug
```

Expected: FAIL because `common/result.h` and `common/unique_handle.h` do not exist.

- [ ] **Step 3: Implement the common primitives**

Create `src/common/error.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace myrd {
enum class ErrorCode {
    invalid_argument,
    not_found,
    access_denied,
    timeout,
    protocol_error,
    win32_error,
    ntstatus_error,
    io_error,
    resource_limit,
    internal_error
};

struct Error {
    ErrorCode code;
    std::uint32_t native_code;
    std::wstring message;
};
}  // namespace myrd
```

Create `src/common/result.h`:

```cpp
#pragma once

#include "common/error.h"

#include <utility>
#include <variant>

namespace myrd {
template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
    T& value() { return std::get<T>(storage_); }
    const T& value() const { return std::get<T>(storage_); }
    Error& error() { return std::get<Error>(storage_); }
    const Error& error() const { return std::get<Error>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(); }
    static Result failure(Error error) { return Result(std::move(error)); }
    bool ok() const noexcept { return !error_.has_value(); }
    const Error& error() const { return error_.value(); }

private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}
    std::optional<Error> error_;
};
}  // namespace myrd
```

Add `#include <optional>` to `result.h`.

Create `src/common/unique_handle.h`:

```cpp
#pragma once

#include <windows.h>

#include <utility>

namespace myrd {
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};
}  // namespace myrd
```

- [ ] **Step 4: Build and run unit tests**

Run:

```powershell
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

Expected: all tests pass.

- [ ] **Step 5: Commit the common layer**

```powershell
git add src/common tests/common_tests.cpp src/CMakeLists.txt
git commit -m "feat: add typed errors and Win32 handle ownership"
```

## Task 3: Freeze AIM and IMDPROXY binary contracts

**Files:**
- Create: `src/driver/aim_protocol.h`
- Create: `src/proxy/imdproxy_protocol.h`
- Create: `tests/protocol_tests.cpp`
- Create: `docs/protocol/aim-m0-contract.md`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing protocol layout tests**

Create `tests/protocol_tests.cpp`:

```cpp
#include "driver/aim_protocol.h"
#include "proxy/imdproxy_protocol.h"
#include "test_framework.h"

#include <cstddef>

TEST_CASE(aim_layout_matches_upstream) {
    REQUIRE(sizeof(SRB_IO_CONTROL) == 28);
    REQUIRE(sizeof(myrd::aim::DeviceNumber) == 4);
    REQUIRE(sizeof(myrd::aim::DeviceConfiguration) == 36);
    REQUIRE(offsetof(myrd::aim::DeviceConfiguration, disk_size) == 4);
    REQUIRE(offsetof(myrd::aim::DeviceConfiguration, image_offset) == 20);
    REQUIRE(offsetof(myrd::aim::DeviceConfiguration, file_name_length) == 32);
    REQUIRE(myrd::aim::kCreateDevice == 0x83730801u);
    REQUIRE(myrd::aim::kRemoveDevice == 0x83730806u);
}

TEST_CASE(imdproxy_layout_matches_upstream) {
    REQUIRE(myrd::proxy::kHeaderSize == 4096u);
    REQUIRE(sizeof(myrd::proxy::InfoResponse) == 24);
    REQUIRE(sizeof(myrd::proxy::ReadRequest) == 24);
    REQUIRE(sizeof(myrd::proxy::ReadResponse) == 16);
    REQUIRE(sizeof(myrd::proxy::WriteRequest) == 24);
    REQUIRE(sizeof(myrd::proxy::WriteResponse) == 16);
}
```

- [ ] **Step 2: Run the build and verify it fails on missing protocol headers**

Run `cmake --build --preset vs2022-debug`.

Expected: FAIL because the protocol headers do not exist.

- [ ] **Step 3: Implement the AIM contract**

Create `src/driver/aim_protocol.h` with `#include <windows.h>` and `#include <ntddscsi.h>`, then define:

```cpp
#pragma once

#include <windows.h>
#include <ntddscsi.h>

#include <cstddef>
#include <cstdint>

namespace myrd::aim {
constexpr std::uint32_t kAutoDeviceNumber = 0x00FFFFFFu;
constexpr std::uint32_t kTypeProxy = 0x00000300u;
constexpr std::uint32_t kProxyTypeSharedMemory = 0x00003000u;
constexpr std::uint32_t kDeviceTypeHd = 0x00000010u;
constexpr std::uint32_t kProxyDiskFlags =
    kTypeProxy | kProxyTypeSharedMemory | kDeviceTypeHd;

constexpr std::uint32_t kControlBase = 0x83730000u;
constexpr std::uint32_t kQueryVersion = kControlBase | 0x800u;
constexpr std::uint32_t kCreateDevice = kControlBase | 0x801u;
constexpr std::uint32_t kQueryDevice = kControlBase | 0x802u;
constexpr std::uint32_t kQueryAdapter = kControlBase | 0x803u;
constexpr std::uint32_t kRemoveDevice = kControlBase | 0x806u;

union DeviceNumber {
    struct {
        std::uint8_t path_id;
        std::uint8_t target_id;
        std::uint8_t lun;
        std::uint8_t padding;
    } parts;
    std::uint32_t value;
};

#pragma pack(push, 4)
struct DeviceConfiguration {
    DeviceNumber device_number;
    std::int64_t disk_size;
    std::uint32_t bytes_per_sector;
    std::uint32_t reserved;
    std::int64_t image_offset;
    std::uint32_t flags;
    std::uint16_t file_name_length;
};
#pragma pack(pop)

static_assert(sizeof(SRB_IO_CONTROL) == 28);
static_assert(sizeof(DeviceNumber) == 4);
static_assert(sizeof(DeviceConfiguration) == 36);
static_assert(offsetof(DeviceConfiguration, disk_size) == 4);
static_assert(offsetof(DeviceConfiguration, image_offset) == 20);
static_assert(offsetof(DeviceConfiguration, file_name_length) == 32);
}  // namespace myrd::aim
```

- [ ] **Step 4: Implement the IMDPROXY contract**

Create `src/proxy/imdproxy_protocol.h`:

```cpp
#pragma once

#include <cstdint>

namespace myrd::proxy {
constexpr std::uint64_t kHeaderSize = 4096;
constexpr std::uint64_t kDefaultDataSize = 8ull << 20;
constexpr std::uint64_t kDefaultMappingSize = kHeaderSize + kDefaultDataSize;

enum class RequestCode : std::uint64_t {
    null_request = 0,
    info = 1,
    read = 2,
    write = 3,
    connect = 4,
    close = 5,
    unmap = 6,
    zero = 7,
    scsi = 8,
    shared = 9
};

struct InfoResponse { std::uint64_t file_size, request_alignment, flags; };
struct ReadRequest { std::uint64_t request_code, offset, length; };
struct ReadResponse { std::uint64_t error_number, length; };
struct WriteRequest { std::uint64_t request_code, offset, length; };
struct WriteResponse { std::uint64_t error_number, length; };
struct CloseRequest { std::uint64_t request_code; };

static_assert(sizeof(InfoResponse) == 24);
static_assert(sizeof(ReadRequest) == 24);
static_assert(sizeof(ReadResponse) == 16);
static_assert(sizeof(WriteRequest) == 24);
static_assert(sizeof(WriteResponse) == 16);
static_assert(sizeof(CloseRequest) == 8);
}  // namespace myrd::proxy
```

- [ ] **Step 5: Document provenance and unsupported requests**

Create `docs/protocol/aim-m0-contract.md` recording:

```markdown
# AIM M0 binary contract

Source baseline: Arsenal Image Mounter commit
`d379caa027d21902e45fce8c9daf02fd116c2dda`.

Audited files:

- `Unmanaged Source/phdskmnt/inc/common.h`
- `Unmanaged Source/phdskmnt/inc/imdproxy.h`
- `Unmanaged Source/phdskmnt/proxy.cpp`
- `Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs`

M0 advertises no optional IMDPROXY flags and accepts only INFO, READ, WRITE,
and CLOSE. CONNECT, UNMAP, ZERO, SCSI, and SHARED are protocol errors in M0.

The create filename is the native path
`\BaseNamedObjects\Global\<session-id>` and the user-mode objects are
`Global\<session-id>`, `Global\<session-id>_Request`,
`Global\<session-id>_Response`, and `Global\<session-id>_Server`.
```

- [ ] **Step 6: Run tests and commit**

Run `cmake --build --preset vs2022-debug` and `ctest --preset vs2022-debug`.

Expected: all tests pass.

```powershell
git add src/driver src/proxy tests/protocol_tests.cpp tests/CMakeLists.txt docs/protocol
git commit -m "feat: freeze AIM and IMDPROXY M0 contracts"
```

## Task 4: Implement the 64 MiB memory block device

**Files:**
- Create: `src/storage/block_device.h`
- Create: `src/storage/memory_block_device.h`
- Create: `src/storage/memory_block_device.cpp`
- Create: `tests/memory_block_device_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing block-device tests**

Create tests for zero initialization, a 4096-byte round trip at offset 8 MiB,
unaligned access rejection, and out-of-range rejection:

```cpp
#include "storage/memory_block_device.h"
#include "test_framework.h"

#include <array>
#include <cstddef>

TEST_CASE(memory_device_is_zero_initialized) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    std::array<std::byte, 512> bytes{};
    auto result = device.read(0, bytes);
    REQUIRE(result.ok());
    for (auto value : bytes) REQUIRE(value == std::byte{0});
}

TEST_CASE(memory_device_round_trip) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    std::array<std::byte, 4096> written{};
    std::array<std::byte, 4096> read{};
    for (std::size_t i = 0; i < written.size(); ++i) {
        written[i] = static_cast<std::byte>((i * 131u) & 0xFFu);
    }
    REQUIRE(device.write(8ull << 20, written).ok());
    REQUIRE(device.read(8ull << 20, read).ok());
    REQUIRE(written == read);
}

TEST_CASE(memory_device_rejects_invalid_ranges) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    std::array<std::byte, 513> bad{};
    REQUIRE(!device.write(0, bad).ok());
    std::array<std::byte, 512> sector{};
    REQUIRE(!device.read(64ull << 20, sector).ok());
}
```

- [ ] **Step 2: Verify the build fails before implementation**

Run `cmake --build --preset vs2022-debug`.

Expected: FAIL because the storage headers do not exist.

- [ ] **Step 3: Implement the storage boundary and memory backend**

Create `src/storage/block_device.h`:

```cpp
#pragma once

#include "common/result.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace myrd {
class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    virtual std::uint64_t size_bytes() const noexcept = 0;
    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual Result<std::size_t> read(
        std::uint64_t offset, std::span<std::byte> output) = 0;
    virtual Result<std::size_t> write(
        std::uint64_t offset, std::span<const std::byte> input) = 0;
};
}  // namespace myrd
```

Implement `MemoryBlockDevice` with a fixed `std::vector<std::byte>`. Both methods
must reject zero-length, unaligned, overflowed, and out-of-range requests before
calling `std::memcpy`.

Use this validation expression in both methods:

```cpp
const bool aligned = offset % sector_size_ == 0 && data.size() % sector_size_ == 0;
const bool in_range = offset <= bytes_.size() &&
    data.size() <= bytes_.size() - static_cast<std::size_t>(offset);
if (data.empty() || !aligned || !in_range) {
    return Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, 0, L"invalid block I/O range"});
}
```

- [ ] **Step 4: Run tests and commit**

Run `cmake --build --preset vs2022-debug` and `ctest --preset vs2022-debug`.

Expected: all tests pass.

```powershell
git add src/storage tests/memory_block_device_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add fixed memory block device for M0"
```

## Task 5: Implement the pure IMDPROXY request processor

**Files:**
- Create: `src/proxy/request_processor.h`
- Create: `src/proxy/request_processor.cpp`
- Create: `tests/request_processor_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing INFO/READ/WRITE/CLOSE tests**

Create a mapping-sized byte vector. Write request headers at offset 0 and data at
offset 4096. Verify:

```cpp
TEST_CASE(proxy_info_reports_exact_m0_capabilities) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    myrd::proxy::RequestProcessor processor(device);
    std::vector<std::byte> mapping(myrd::proxy::kDefaultMappingSize);
    *reinterpret_cast<std::uint64_t*>(mapping.data()) = 1;
    auto action = processor.process(mapping);
    REQUIRE(action.ok());
    REQUIRE(action.value() == myrd::proxy::Action::respond);
    const auto& response = *reinterpret_cast<const myrd::proxy::InfoResponse*>(mapping.data());
    REQUIRE(response.file_size == (64ull << 20));
    REQUIRE(response.request_alignment == 512);
    REQUIRE(response.flags == 0);
}
```

Append these concrete cases:

```cpp
TEST_CASE(proxy_read_copies_backend_data_to_shared_area) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    std::array<std::byte, 512> source{};
    source[0] = std::byte{0x5A};
    REQUIRE(device.write(4096, source).ok());

    myrd::proxy::RequestProcessor processor(device);
    std::vector<std::byte> mapping(myrd::proxy::kDefaultMappingSize);
    const myrd::proxy::ReadRequest request{2, 4096, 512};
    std::memcpy(mapping.data(), &request, sizeof(request));

    auto action = processor.process(mapping);
    REQUIRE(action.ok());
    REQUIRE(mapping[myrd::proxy::kHeaderSize] == std::byte{0x5A});
    myrd::proxy::ReadResponse response{};
    std::memcpy(&response, mapping.data(), sizeof(response));
    REQUIRE(response.error_number == 0);
    REQUIRE(response.length == 512);
}

TEST_CASE(proxy_write_copies_shared_data_to_backend) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    myrd::proxy::RequestProcessor processor(device);
    std::vector<std::byte> mapping(myrd::proxy::kDefaultMappingSize);
    const myrd::proxy::WriteRequest request{3, 8192, 512};
    std::memcpy(mapping.data(), &request, sizeof(request));
    mapping[myrd::proxy::kHeaderSize] = std::byte{0xA5};

    REQUIRE(processor.process(mapping).ok());
    std::array<std::byte, 512> output{};
    REQUIRE(device.read(8192, output).ok());
    REQUIRE(output[0] == std::byte{0xA5});
}

TEST_CASE(proxy_close_requests_server_shutdown) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    myrd::proxy::RequestProcessor processor(device);
    std::vector<std::byte> mapping(myrd::proxy::kDefaultMappingSize);
    const myrd::proxy::CloseRequest request{5};
    std::memcpy(mapping.data(), &request, sizeof(request));
    auto action = processor.process(mapping);
    REQUIRE(action.ok());
    REQUIRE(action.value() == myrd::proxy::Action::close);
}

TEST_CASE(proxy_rejects_unsupported_and_out_of_range_requests) {
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    myrd::proxy::RequestProcessor processor(device);
    std::vector<std::byte> mapping(myrd::proxy::kDefaultMappingSize);
    const myrd::proxy::ReadRequest unsupported{6, 0, 512};
    std::memcpy(mapping.data(), &unsupported, sizeof(unsupported));
    REQUIRE(!processor.process(mapping).ok());

    const myrd::proxy::ReadRequest outside{2, 64ull << 20, 512};
    std::memcpy(mapping.data(), &outside, sizeof(outside));
    REQUIRE(!processor.process(mapping).ok());
}
```

- [ ] **Step 2: Run the test and confirm the missing processor fails the build**

Run `cmake --build --preset vs2022-debug`.

Expected: FAIL because `request_processor.h` does not exist.

- [ ] **Step 3: Implement the processor**

Use this interface:

```cpp
namespace myrd::proxy {
enum class Action { respond, close };

class RequestProcessor {
public:
    explicit RequestProcessor(IBlockDevice& device) : device_(device) {}
    Result<Action> process(std::span<std::byte> mapping);

private:
    IBlockDevice& device_;
};
}
```

Implementation rules:

- Require `mapping.size() >= kDefaultMappingSize`.
- Read request structs with `std::memcpy`, not unaligned pointer dereferences.
- Require offset and length to be 512-byte aligned.
- Require `length <= mapping.size() - kHeaderSize`.
- INFO writes exactly `InfoResponse{size, sector_size, 0}`.
- READ writes backend bytes at `kHeaderSize`, then overwrites offset 0 with
  `ReadResponse{0, length}`.
- WRITE reads input bytes at `kHeaderSize`, calls the backend, then writes
  `WriteResponse{0, length}`.
- CLOSE returns `Action::close` without signaling another request cycle.
- Unsupported codes return `protocol_error`; do not advertise optional flags.

- [ ] **Step 4: Run tests and commit**

Run build and CTest; expected all tests pass.

```powershell
git add src/proxy/request_processor.* tests/request_processor_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: process M0 IMDPROXY requests"
```

## Task 6: Implement Shared Memory channel ownership and server loop

**Files:**
- Create: `src/proxy/shared_memory_channel.h`
- Create: `src/proxy/shared_memory_channel.cpp`
- Create: `src/proxy/shm_proxy_server.h`
- Create: `src/proxy/shm_proxy_server.cpp`
- Create: `tests/shared_memory_channel_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing channel tests**

Test deterministic object names, duplicate-name exclusion, mapping capacity, and
a local request/response exchange:

```cpp
TEST_CASE(channel_builds_driver_and_win32_names) {
    const auto names = myrd::proxy::ObjectNames::from_session(L"m0-test-123");
    REQUIRE(names.mapping == L"Global\\m0-test-123");
    REQUIRE(names.request == L"Global\\m0-test-123_Request");
    REQUIRE(names.response == L"Global\\m0-test-123_Response");
    REQUIRE(names.server == L"Global\\m0-test-123_Server");
    REQUIRE(names.driver_path == L"\\BaseNamedObjects\\Global\\m0-test-123");
}
```

Add a local exchange test using the public server API:

```cpp
TEST_CASE(channel_and_server_complete_local_info_exchange) {
    auto names = myrd::proxy::ObjectNames::from_session(L"m0-local-exchange");
    auto channel = myrd::proxy::SharedMemoryChannel::create(names);
    REQUIRE(channel.ok());
    myrd::MemoryBlockDevice device(64ull << 20, 512);
    myrd::proxy::RequestProcessor processor(device);
    myrd::proxy::ShmProxyServer server(std::move(channel.value()), processor);
    REQUIRE(server.start().ok());

    myrd::UniqueHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                                names.mapping.c_str()));
    myrd::UniqueHandle request(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                          names.request.c_str()));
    myrd::UniqueHandle response(OpenEventW(SYNCHRONIZE, FALSE,
                                           names.response.c_str()));
    REQUIRE(mapping && request && response);
    void* view = MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0,
                               myrd::proxy::kDefaultMappingSize);
    REQUIRE(view != nullptr);
    const std::uint64_t code = 1;
    std::memcpy(view, &code, sizeof(code));
    REQUIRE(SetEvent(request.get()) != FALSE);
    REQUIRE(WaitForSingleObject(response.get(), 5000) == WAIT_OBJECT_0);
    myrd::proxy::InfoResponse info{};
    std::memcpy(&info, view, sizeof(info));
    REQUIRE(info.file_size == (64ull << 20));
    UnmapViewOfFile(view);
    REQUIRE(server.stop().ok());
}
```

- [ ] **Step 2: Verify the tests fail before implementation**

Run the build; expected missing channel/server headers.

- [ ] **Step 3: Implement `SharedMemoryChannel`**

Create the mapping with `CreateFileMappingW(INVALID_HANDLE_VALUE, ..., PAGE_READWRITE)`
and exactly `kDefaultMappingSize`. Create request/response events with
`bManualReset=FALSE`, create the server mutex, and acquire it with a zero timeout.
Treat `ERROR_ALREADY_EXISTS` or an unacquired server mutex as a name collision.

Expose only:

```cpp
class SharedMemoryChannel {
public:
    static Result<SharedMemoryChannel> create(const ObjectNames& names);
    std::span<std::byte> mapping() noexcept;
    HANDLE request_event() const noexcept;
    HANDLE response_event() const noexcept;
    const ObjectNames& names() const noexcept;
};
```

All mapping, view, event, and mutex resources must be owned by RAII objects.

- [ ] **Step 4: Implement `ShmProxyServer`**

Use one worker thread and this lifecycle:

```cpp
while (!stop_requested) {
    const DWORD wait = WaitForSingleObject(channel.request_event(), 1000);
    if (wait == WAIT_TIMEOUT) continue;
    if (wait != WAIT_OBJECT_0) return wait_failure();

    auto action = processor.process(channel.mapping());
    if (!action.ok()) return action.error();
    if (action.value() == Action::close) return Result<void>::success();
    if (!SetEvent(channel.response_event())) return set_event_failure();
}
```

`start()` must not return success until all named objects exist. `stop()` signals a
private manual-reset stop event and joins the thread. Destruction must be safe even
if the driver never connects.

- [ ] **Step 5: Run local integration tests and commit**

Run build and CTest; expected all tests pass and no named object remains open after
the test process exits.

```powershell
git add src/proxy tests/shared_memory_channel_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add shared memory proxy server"
```

## Task 7: Implement SRB request construction and a mockable Direct AIM adapter

**Files:**
- Create: `src/driver/srb_transport.h`
- Create: `src/driver/srb_codec.h`
- Create: `src/driver/srb_codec.cpp`
- Create: `src/driver/direct_aim_adapter.h`
- Create: `src/driver/direct_aim_adapter.cpp`
- Create: `tests/srb_codec_tests.cpp`
- Create: `tests/direct_aim_adapter_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing SRB codec tests**

Verify header length, eight-byte `PhDskMnt` signature, control code, payload length,
timeout, and native UTF-16 filename bytes. The create test must expect:

```cpp
const myrd::aim::ProxyDiskSpec spec{
    .size_bytes = 64ull << 20,
    .sector_size = 512,
    .native_object_path = L"\\BaseNamedObjects\\Global\\m0-test"
};
const auto request = myrd::aim::build_create_request(spec);
REQUIRE(request.ok());
REQUIRE(request.value().configuration().device_number.value ==
        myrd::aim::kAutoDeviceNumber);
REQUIRE(request.value().configuration().flags == myrd::aim::kProxyDiskFlags);
REQUIRE(request.value().configuration().file_name_length ==
        spec.native_object_path.size() * sizeof(wchar_t));
```

Add these concrete rejection checks:

```cpp
TEST_CASE(create_codec_rejects_invalid_spec) {
    myrd::aim::ProxyDiskSpec unaligned{513, 512, L"\\BaseNamedObjects\\Global\\x"};
    REQUIRE(!myrd::aim::build_create_request(unaligned).ok());

    myrd::aim::ProxyDiskSpec too_long{64ull << 20, 512,
        std::wstring(40000, L'x')};
    REQUIRE(!myrd::aim::build_create_request(too_long).ok());
}

TEST_CASE(srb_codec_rejects_truncated_response) {
    std::vector<std::byte> response(sizeof(SRB_IO_CONTROL) - 1);
    REQUIRE(!myrd::aim::decode_srb_response(response, 0x83730800u).ok());
}
```

- [ ] **Step 2: Write failing adapter behavior tests using `FakeSrbTransport`**

The fake captures the control code and request bytes and returns prepared response
bytes. Add one success-path test that prepares a create response containing
`DeviceId{0x00030201}` and verifies the adapter returns that exact value, then add
this mandatory safety test:

```cpp
TEST_CASE(adapter_refuses_remove_all_sentinel) {
    FakeSrbTransport transport;
    myrd::aim::DirectAimAdapter adapter(transport);
    auto result = adapter.remove_device({myrd::aim::kAutoDeviceNumber});
    REQUIRE(!result.ok());
    REQUIRE(transport.send_count() == 0);
}

TEST_CASE(adapter_returns_exact_created_device_id) {
    FakeSrbTransport transport;
    transport.queue_create_response(0x00030201u, 64ull << 20,
                                    myrd::aim::kProxyDiskFlags);
    myrd::aim::DirectAimAdapter adapter(transport);
    auto created = adapter.create_proxy_disk(
        {64ull << 20, 512, L"\\BaseNamedObjects\\Global\\m0-test"});
    REQUIRE(created.ok());
    REQUIRE(created.value().id.value == 0x00030201u);
    REQUIRE(transport.last_control_code() == myrd::aim::kCreateDevice);
}
```

- [ ] **Step 3: Implement SRB encoding and validation**

`SrbTransport` exposes:

```cpp
class SrbTransport {
public:
    virtual ~SrbTransport() = default;
    virtual Result<std::vector<std::byte>> transact(
        std::uint32_t control_code,
        std::span<const std::byte> payload,
        std::size_t response_capacity,
        std::uint32_t timeout_seconds) = 0;
};
```

`srb_codec.cpp` constructs `SRB_IO_CONTROL` with:

```cpp
header.HeaderLength = sizeof(SRB_IO_CONTROL);
std::memcpy(header.Signature, "PhDskMnt", 8);
header.Timeout = timeout_seconds;
header.ControlCode = control_code;
header.ReturnCode = 0;
header.Length = static_cast<ULONG>(payload.size());
```

Reject every response where signature, header length, returned length, or total
buffer bounds are inconsistent.

- [ ] **Step 4: Implement `DirectAimAdapter`**

Expose strongly typed `DeviceId`, `DriverVersion`, `ProxyDiskSpec`, and
`CreatedDisk`. `create_proxy_disk` must return the exact `DeviceId` supplied by the
driver response. `remove_device` must reject `0x00FFFFFF` before transport access.

No method may accept a disk capacity or PhysicalDrive number as a substitute for
`DeviceId`.

- [ ] **Step 5: Run tests and commit**

Run build and CTest; expected all tests pass.

```powershell
git add src/driver tests/srb_codec_tests.cpp tests/direct_aim_adapter_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add direct AIM control codec and adapter"
```

## Task 8: Implement Windows adapter discovery, IOCTL transport, and PhysicalDrive mapping

**Files:**
- Create: `src/driver/adapter_discovery.h`
- Create: `src/driver/adapter_discovery.cpp`
- Create: `src/driver/windows_srb_transport.h`
- Create: `src/driver/windows_srb_transport.cpp`
- Create: `src/driver/physical_drive_locator.h`
- Create: `src/driver/physical_drive_locator.cpp`
- Create: `tests/physical_drive_locator_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing pure matching tests**

Separate Windows enumeration from matching. Given candidates containing SCSI
addresses and storage device numbers, verify exact PathId/TargetId/Lun matching:

```cpp
TEST_CASE(locator_matches_exact_scsi_address) {
    const myrd::aim::DeviceId expected{0x00030201u};
    const std::vector<myrd::aim::DiskCandidate> candidates{
        {1, 2, 2, 7},
        {1, 2, 3, 9}
    };
    auto result = myrd::aim::match_physical_drive(expected, candidates);
    REQUIRE(result.ok());
    REQUIRE(result.value().physical_drive_number == 9);
    REQUIRE(result.value().path == L"\\\\.\\PhysicalDrive9");
}
```

Add explicit no-match and duplicate-match cases:

```cpp
TEST_CASE(locator_rejects_missing_match) {
    const std::vector<myrd::aim::DiskCandidate> candidates{{1, 2, 2, 7}};
    REQUIRE(!myrd::aim::match_physical_drive({0x00030201u}, candidates).ok());
}

TEST_CASE(locator_rejects_duplicate_matches) {
    const std::vector<myrd::aim::DiskCandidate> candidates{
        {1, 2, 3, 7}, {1, 2, 3, 9}
    };
    REQUIRE(!myrd::aim::match_physical_drive({0x00030201u}, candidates).ok());
}
```

- [ ] **Step 2: Implement adapter discovery**

Enumerate `GUID_DEVINTERFACE_STORAGEPORT` with SetupAPI. For each present device
interface, query `DEVPKEY_Device_Service`; retain only service name `phdskmnt`.
Open the interface path with `CreateFileW` using read/write access and shared
read/write. Return an error unless exactly one compatible adapter responds to
`SMP_IMSCSI_QUERY_VERSION`.

Do not scan arbitrary SCSI ports without verifying the AIM query-version response.

- [ ] **Step 3: Implement `WindowsSrbTransport`**

Construct one contiguous in/out buffer containing `SRB_IO_CONTROL + payload` and
call:

```cpp
DeviceIoControl(adapter_handle,
                IOCTL_SCSI_MINIPORT,
                buffer.data(), static_cast<DWORD>(buffer.size()),
                buffer.data(), static_cast<DWORD>(buffer.size()),
                &bytes_returned, nullptr);
```

Copy the returned bytes into a fresh vector before validating. Preserve both
`GetLastError()` and `SRB_IO_CONTROL::ReturnCode` in typed errors.

- [ ] **Step 4: Implement disk-interface enumeration**

Enumerate `GUID_DEVINTERFACE_DISK`. For every openable disk interface:

- Call `IOCTL_SCSI_GET_ADDRESS` and retain PathId, TargetId, and Lun.
- Call `IOCTL_STORAGE_GET_DEVICE_NUMBER` and retain `DeviceNumber`.
- Compare the three SCSI address bytes with the AIM `DeviceId`.
- Return exactly one `\\.\PhysicalDriveN` path.

Retry discovery every 250 ms for up to 15 seconds after create. A timeout is an
error and triggers exact-device cleanup by the caller.

- [ ] **Step 5: Run unit tests and compile the Windows implementation**

Link `Setupapi`, `Cfgmgr32`, and `Advapi32` in `src/CMakeLists.txt`.

Run build and CTest. Expected: all unit tests pass; no real device is created yet.

- [ ] **Step 6: Commit**

```powershell
git add src/driver tests/physical_drive_locator_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: discover AIM adapter and exact physical drive"
```

## Task 9: Add SHA-256, raw-sector round trip, and strict JSON evidence

**Files:**
- Create: `src/common/sha256.h`
- Create: `src/common/sha256.cpp`
- Create: `src/common/authenticode.h`
- Create: `src/common/authenticode.cpp`
- Create: `src/poc/json_writer.h`
- Create: `src/poc/json_writer.cpp`
- Create: `src/poc/poc_report.h`
- Create: `src/poc/poc_report.cpp`
- Create: `src/poc/raw_round_trip.h`
- Create: `src/poc/raw_round_trip.cpp`
- Create: `tests/report_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing SHA and JSON tests**

Use the standard SHA-256 vector for ASCII `abc`:

```cpp
TEST_CASE(sha256_matches_standard_vector) {
    const std::array<std::byte, 3> input{
        std::byte{0x61}, std::byte{0x62}, std::byte{0x63}
    };
    auto digest = myrd::sha256_hex(input);
    REQUIRE(digest.ok());
    REQUIRE(digest.value() ==
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
}

TEST_CASE(json_writer_escapes_and_emits_valid_types) {
    myrd::JsonWriter writer;
    writer.begin_object();
    writer.member("text", "quote: \" slash: \\ newline:\n");
    writer.member("ok", true);
    writer.member("count", static_cast<std::uint64_t>(7));
    writer.member_array("names", std::vector<std::string>{"alpha", "beta"});
    writer.end_object();
    REQUIRE(writer.str() ==
        "{\"text\":\"quote: \\\" slash: \\\\ newline:\\n\","
        "\"ok\":true,\"count\":7,\"names\":[\"alpha\",\"beta\"]}");
}
```

Write the generated fixture to the test binary's temporary directory and add a CTest
PowerShell step that runs `Get-Content -Raw | ConvertFrom-Json`. Expected: exit 0,
so JSON validity is checked by an independent parser.

- [ ] **Step 2: Implement SHA-256 with BCrypt**

Use `BCryptOpenAlgorithmProvider(BCRYPT_SHA256_ALGORITHM)`, query
`BCRYPT_OBJECT_LENGTH` and `BCRYPT_HASH_LENGTH`, create/update/finish the hash, and
close all BCrypt handles with RAII. Link `Bcrypt.lib`.

- [ ] **Step 3: Implement offline Authenticode verification**

Implement `verify_authenticode(path)` with `WinVerifyTrust` and
`WINTRUST_ACTION_GENERIC_VERIFY_V2`. Use `WTD_REVOKE_NONE` and
`WTD_CACHE_ONLY_URL_RETRIEVAL` so M0 does not depend on network access. Extract the
signer subject with `CryptQueryObject`, `CryptMsgGetParam(CMSG_SIGNER_INFO_PARAM)`,
`CertFindCertificateInStore`, and `CertGetNameStringW`. Link `Wintrust.lib` and
`Crypt32.lib`. Return both the trust status and signer string.

- [ ] **Step 4: Implement fixed-schema JSON reporting**

`PocReport` must contain:

```cpp
struct PocReport {
    std::string mode;
    std::wstring timestamp_utc;
    std::wstring driver_path;
    std::wstring driver_version;
    std::wstring driver_signer;
    bool driver_signature_valid;
    std::wstring adapter_path;
    std::uint32_t create_flags;
    std::uint32_t aim_device_id;
    std::uint32_t physical_drive_number;
    std::uint64_t info_count;
    std::uint64_t read_count;
    std::uint64_t write_count;
    std::uint64_t close_count;
    std::uint64_t test_offset;
    std::uint64_t test_length;
    std::string written_sha256;
    std::string read_sha256;
    bool exact_remove_succeeded;
    bool device_lists_match_after_cleanup;
    bool named_objects_released;
    std::vector<std::uint32_t> device_ids_before;
    std::vector<std::uint32_t> device_ids_after;
    std::vector<std::wstring> loaded_modules;
    std::vector<std::wstring> aim_user_processes;
};
```

The writer must emit UTF-8 JSON and never include raw test-sector contents.

- [ ] **Step 5: Implement raw round trip**

Open only the PhysicalDrive path returned by exact locator matching. Use offset
`8ull << 20`, length 4096, and a deterministic pattern derived from a random session
GUID. Call `FlushFileBuffers`, seek back, read 4096 bytes, and compare SHA-256.

Reject a PhysicalDrive path that does not match the exact pattern
`\\.\PhysicalDrive<decimal>` or whose device number differs from the locator result.

- [ ] **Step 6: Run tests and commit**

Run build and CTest; expected all tests pass.

```powershell
git add src/common/sha256.* src/common/authenticode.* src/poc tests/report_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add M0 evidence report and raw round trip"
```

## Task 10: Assemble the `myrd-m0-poc.exe` lifecycle

**Files:**
- Create: `src/poc/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `README.md`

- [ ] **Step 1: Add a query-only command before enabling device creation**

Support:

```text
myrd-m0-poc.exe query-driver --report <path>
```

The command discovers the adapter, sends query version/list requests, inspects the
installed `C:\Windows\System32\drivers\phdskmnt.sys` version/signature, enumerates
loaded modules and AIM-related processes, writes JSON, and exits without creating a
disk.

- [ ] **Step 2: Build and run query-only mode**

Run from an elevated PowerShell:

```powershell
cmake --build --preset vs2022-debug
New-Item -ItemType Directory -Path reports -Force | Out-Null
& .\out\build\vs2022-debug\src\Debug\myrd-m0-poc.exe query-driver --report .\reports\query-driver.json
$LASTEXITCODE
Get-Content .\reports\query-driver.json -Raw | ConvertFrom-Json | Format-List
```

Expected: exit 0; JSON identifies `phdskmnt.sys`; loaded modules do not contain
`aimapi.dll`; no device is added or removed.

- [ ] **Step 3: Implement the real `run` lifecycle**

Support:

```text
myrd-m0-poc.exe run --report <path>
```

Use this exact order in `main.cpp`:

```cpp
discover and open AIM adapter
snapshot adapter device list
create MemoryBlockDevice(64 MiB, 512)
generate cryptographically random session id
create SharedMemoryChannel
start ShmProxyServer and wait for ready
create proxy device with native shared-memory path
record returned DeviceId immediately
locate exact PhysicalDrive by SCSI address
perform 4096-byte raw round trip at 8 MiB
remove exact returned DeviceId
wait for CLOSE or bounded server shutdown
verify post-cleanup adapter list equals pre-create list
write report
```

Implement cleanup with a scope guard whose only removable id is initially empty and
is assigned immediately after a successful create response. The scope guard must
never call remove with `0x00FFFFFF`.

- [ ] **Step 4: Add protocol counters**

`RequestProcessor` increments atomic INFO, READ, WRITE, and CLOSE counters. Include
them in the report. The run is successful only if INFO, READ, and WRITE are all at
least one; CLOSE may be zero only when the driver was already removed and the server
was cleanly stopped after the bounded wait.

- [ ] **Step 5: Verify import and process independence**

Run:

```powershell
dumpbin /dependents .\out\build\vs2022-debug\src\Debug\myrd-m0-poc.exe
```

Expected: Windows system DLLs only; no `aimapi.dll`.

At runtime, enumerate the current process modules with Toolhelp32 and record them in
the report. Enumerate process names and record any matching
`aim_ll`, `RamDyn`, `AIMtk`, `aim_cli`, `MountImg`, or `deviosvc`.

- [ ] **Step 6: Commit the executable**

```powershell
git add src/poc/main.cpp src/CMakeLists.txt README.md
git commit -m "feat: assemble Driver-only M0 lifecycle"
```

## Task 11: Add the guarded real-driver E2E harness

**Files:**
- Create: `tests/e2e/run-m0-driver-only.ps1`
- Modify: `README.md`

- [ ] **Step 1: Implement administrator and environment preflight**

The script accepts:

```powershell
param(
    [int]$Iterations = 1,
    [string]$Configuration = 'Debug',
    [string]$ReportRoot = '.\reports\m0'
)
```

It must fail before state changes unless:

- The current token is elevated.
- `phdskmnt` exists and is running.
- `myrd-m0-poc.exe` exists.
- No forbidden AIM user process except `deviosvc` is running.
- `$Iterations` is between 1 and 20.

- [ ] **Step 2: Capture and restore `deviosvc` in `try/finally`**

Use this control shape:

```powershell
$devio = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
$originalState = $devio.State
$originalStartMode = $devio.StartMode

try {
    if ($originalState -eq 'Running') {
        Stop-Service -Name deviosvc -Force
        (Get-Service deviosvc).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
    }
    # Run query and lifecycle iterations here.
}
finally {
    if ($originalState -eq 'Running' -and
        (Get-Service deviosvc).Status -ne 'Running') {
        Start-Service -Name deviosvc
        (Get-Service deviosvc).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
    }
    $restored = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
    if ($restored.StartMode -ne $originalStartMode) {
        throw "deviosvc start mode changed unexpectedly"
    }
}
```

- [ ] **Step 3: Run one guarded lifecycle and validate JSON**

For each iteration, execute `myrd-m0-poc.exe run --report <unique-path>`, parse the
JSON with `ConvertFrom-Json`, and assert:

```powershell
if ($report.written_sha256 -ne $report.read_sha256) { throw 'SHA mismatch' }
if (-not $report.exact_remove_succeeded) { throw 'exact remove failed' }
if (-not $report.device_lists_match_after_cleanup) { throw 'device list leaked' }
if (-not $report.named_objects_released) { throw 'named object leaked' }
if ($report.info_count -lt 1 -or $report.read_count -lt 1 -or $report.write_count -lt 1) {
    throw 'driver did not exercise INFO/READ/WRITE'
}
if ($report.loaded_modules -match 'aimapi\.dll') { throw 'aimapi.dll was loaded' }
if ($report.aim_user_processes.Count -ne 0) { throw 'AIM user process detected' }
```

The harness must also run `query-driver` before the first iteration and after the
last iteration and compare adapter device lists.

- [ ] **Step 4: Run the first real E2E cycle**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\e2e\run-m0-driver-only.ps1 -Iterations 1
```

Expected: exit 0, one valid report, `deviosvc` restored, and no AIM device delta.

If this fails, stop implementation changes and use systematic debugging to identify
the failing boundary. Do not broaden protocol flags or try multiple speculative
fixes in one change.

- [ ] **Step 5: Commit the E2E harness**

```powershell
git add tests/e2e README.md
git commit -m "test: add guarded Driver-only E2E harness"
```

## Task 12: Execute the 20-cycle Go/No-go acceptance

**Files:**
- Create: `docs/validation/2026-07-31-m0-driver-only-report.md`
- Modify only if evidence exposes a defect: the exact source/test files associated with that root cause

- [ ] **Step 1: Run all unit and local integration tests fresh**

```powershell
cmake --preset vs2022-debug
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

Expected: configure/build exit 0 and CTest reports zero failures.

- [ ] **Step 2: Run the full guarded acceptance**

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\e2e\run-m0-driver-only.ps1 -Iterations 20
```

Expected:

- 20/20 lifecycle reports pass.
- Each report contains INFO, READ, and WRITE requests.
- Every write/read SHA pair matches.
- Every exact remove succeeds.
- Pre/post device lists match for every iteration and for the full run.
- `deviosvc` is stopped during each run and restored afterward.
- No report lists `aimapi.dll` or an AIM user-mode process.
- No shared mapping, event, or server mutex name can be reopened after its cycle.

- [ ] **Step 3: Capture independent system evidence**

Record:

```powershell
Get-AuthenticodeSignature C:\Windows\System32\drivers\phdskmnt.sys
(Get-Item C:\Windows\System32\drivers\phdskmnt.sys).VersionInfo
Get-CimInstance Win32_SystemDriver -Filter "Name='phdskmnt'"
Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
dumpbin /dependents .\out\build\vs2022-debug\src\Release\myrd-m0-poc.exe
```

Build Release before the final `dumpbin` check:

```powershell
cmake --build .\out\build\vs2022-debug --config Release
```

- [ ] **Step 4: Write the acceptance report**

`docs/validation/2026-07-31-m0-driver-only-report.md` must contain:

```markdown
# M0 Driver-only Acceptance Report

## Verdict

`GO` only if every mandatory condition passed; otherwise `NO-GO`.

## Environment

- Windows version and build
- CPU architecture
- MSVC version
- Windows SDK version
- `phdskmnt.sys` file/product version
- Authenticode status and signer
- Official AIM source commit and DriverFiles.zip SHA-256

## Dependency evidence

- Import-table result
- Loaded-module result
- AIM user-process result
- `deviosvc` stopped/restored evidence

## Lifecycle evidence

- Iterations attempted/passed
- Device IDs created and removed
- PhysicalDrive numbers
- INFO/READ/WRITE/CLOSE totals
- Written/read SHA-256 pairs
- Pre/post device-list comparison
- Named-object cleanup result

## Failures

For `GO`, state `None`. For `NO-GO`, list the exact failed condition, native error,
protocol request, cleanup result, and the retained report path.
```

- [ ] **Step 5: Make the gate decision**

Set `GO` only when all seven spec conditions pass. A successful compile, successful
query-version call, or a single successful mount is not enough.

For `NO-GO`, do not start M1. Commit the evidence and open a root-cause investigation.

- [ ] **Step 6: Verify repository state and commit the report**

Run:

```powershell
git diff --check
git status --short
```

Expected: only intended source, test, documentation, and report files are present;
`reports/` and build outputs remain ignored.

```powershell
git add docs/validation
git commit -m "docs: record M0 Driver-only acceptance"
```

## Plan completion criteria

The plan is complete only when one of these terminal states is recorded:

- **GO:** 20/20 real-driver cycles pass and the acceptance report proves the process used only our executable plus `phdskmnt.sys`. Proceed by writing the separate M1 plan.
- **NO-GO:** The acceptance report records reproducible evidence, exact cleanup state, and a root-cause boundary. Do not write or execute M1 until the blocker is resolved and M0 is rerun.

No M0 result authorizes uninstalling AIM Toolkit on the current physical machine. Standalone driver installation and eventual Toolkit removal belong to M5 and must first pass in a disposable Windows VM.
