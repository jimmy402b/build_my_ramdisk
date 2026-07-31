# M0 Driver-only PoC Agent-Executable Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce auditable evidence that our C++20 process can use the installed signed `phdskmnt.sys` directly, implement the AIM Shared Memory Proxy itself, expose a disposable 64 MiB Windows disk, complete an exact raw-sector round trip, and remove only the device it created without loading AIM Toolkit user-mode components.

**Architecture:** A single native executable composes four independently testable boundaries: a fixed memory block device, a Shared Memory Proxy provider, a direct SCSI miniport adapter, and a lifecycle/evidence runner. All real-driver mutation is delayed until unit tests, local shared-memory integration, protocol provenance, query-only driver access, identity checks, and cleanup guards have passed.

**Tech Stack:** C++20, MSVC x64, CMake 3.25+, Ninja Multi-Config, CTest, Win32 SetupAPI/Configuration Manager, `IOCTL_SCSI_MINIPORT`, file mappings/events/mutexes, BCrypt, WinVerifyTrust, PowerShell 5.1+.

**Normative SPEC:** `docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md`

---

## 1. Execution contract for every Agent

An Agent starting with zero chat context must do this before changing files:

1. Read the normative SPEC completely.
2. Read this plan completely through the current task.
3. Run `git status --short --branch` and preserve unrelated user changes.
4. Execute tasks in numeric order; do not skip verification tiers.
5. Check off a step only after its command produced the stated evidence.
6. Make one commit per task using the specified message.
7. Never rewrite or squash earlier task commits during M0 execution.

When a command fails:

- Preserve the full command and output in `reports/diagnostics/<task>-<timestamp>.txt`.
- Classify the failure as implementation defect, environment blocker, protocol-source blocker, or real-driver NO-GO evidence.
- Fix only the earliest failing boundary.
- Re-run the failing command, then the task's full pass command.
- Do not make speculative changes in two layers at once.

Real-driver safety rules apply even if a later task text accidentally omits them:

- Do not uninstall AIM Toolkit or `phdskmnt.sys`.
- Do not launch AIM Toolkit executables or load `aimapi.dll`.
- Never remove the all-devices/automatic-allocation sentinel.
- Only remove the exact `DeviceId` returned by this process.
- Never write sector 0; use offset 8 MiB and length 4096 bytes.
- Stop and restore `deviosvc` only in the E2E harness `try/finally`.
- Never run two real M0 lifecycles concurrently.

## 2. Verification ladder

Every task ends at one of these tiers:

| Tier | Command family | Meaning |
|---|---|---|
| V0 | scripts/docs/source checks | No implementation assumption is unverified |
| V1 | build + unit CTest | Pure components compile and behave correctly |
| V2 | local integration CTest | Mapping/events/thread exchange works without driver |
| V3 | `query-driver` | Read-only real-driver path works and changes no state |
| V4 | one guarded `run` | One complete create/read/write/remove lifecycle works |
| V5 | guarded fault injection | Every post-create failure still performs exact cleanup |
| V6 | 20 guarded runs | Go/No-go evidence is repeatable |

The Agent must not run V4 before V3 passes, and must not run V6 before V4 and V5 pass.

## 3. Standard command environment

All commands are run from the repository root in x64 PowerShell.

After Task 1, initialize the MSVC environment once per shell:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cl 2>&1 | Select-Object -First 3
cmake --version
ninja --version
```

Expected:

- `cl` identifies an x64 Microsoft C/C++ compiler.
- CMake version is at least 3.25.
- Ninja exits 0.

Canonical build commands after Task 2:

```powershell
cmake --preset msvc
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
```

## 4. Canonical repository map

```text
/
├── .gitignore
├── CMakeLists.txt
├── CMakePresets.json
├── LICENSE
├── README.md
├── THIRD_PARTY.md
├── cmake/
│   └── Warnings.cmake
├── docs/
│   ├── protocol/aim-m0-contract.md
│   ├── validation/m0-environment.md
│   └── validation/m0-driver-only-acceptance.md
├── tools/
│   ├── Enter-MsvcEnvironment.ps1
│   ├── Fetch-AimReference.ps1
│   ├── Audit-M0Dependencies.ps1
│   ├── Verify-M0Json.ps1
│   └── Invoke-M0Verification.ps1
├── src/
│   ├── CMakeLists.txt
│   ├── common/
│   │   ├── error.h
│   │   ├── result.h
│   │   ├── unique_handle.h
│   │   ├── unique_view.h
│   │   ├── unique_local.h
│   │   ├── scope_exit.h
│   │   ├── checked_cast.h
│   │   ├── sha256.h/.cpp
│   │   ├── authenticode.h/.cpp
│   │   └── process_snapshot.h/.cpp
│   ├── storage/
│   │   ├── block_device.h
│   │   └── memory_block_device.h/.cpp
│   ├── proxy/
│   │   ├── imdproxy_protocol.h
│   │   ├── request_processor.h/.cpp
│   │   ├── object_names.h/.cpp
│   │   ├── shared_memory_channel.h/.cpp
│   │   └── shm_proxy_server.h/.cpp
│   ├── driver/
│   │   ├── aim_protocol.h
│   │   ├── srb_transport.h
│   │   ├── srb_codec.h/.cpp
│   │   ├── direct_aim_adapter.h/.cpp
│   │   ├── adapter_discovery.h/.cpp
│   │   ├── windows_srb_transport.h/.cpp
│   │   └── physical_drive_locator.h/.cpp
│   └── poc/
│       ├── cli.h/.cpp
│       ├── poc_report.h/.cpp
│       ├── json_writer.h/.cpp
│       ├── raw_round_trip.h/.cpp
│       ├── m0_provider.h/.cpp
│       ├── lifecycle_runner.h/.cpp
│       └── main.cpp
└── tests/
    ├── CMakeLists.txt
    ├── support/test_framework.h
    ├── support/test_main.cpp
    ├── unit/common_tests.cpp
    ├── unit/protocol_tests.cpp
    ├── unit/storage_tests.cpp
    ├── unit/request_processor_tests.cpp
    ├── unit/object_names_tests.cpp
    ├── unit/srb_codec_tests.cpp
    ├── unit/direct_aim_adapter_tests.cpp
    ├── unit/physical_drive_locator_tests.cpp
    ├── unit/report_tests.cpp
    ├── unit/lifecycle_runner_tests.cpp
    ├── integration/shm_proxy_server_tests.cpp
    └── e2e/run-m0-driver-only.ps1
```

Do not combine unrelated responsibilities into `main.cpp` or a single utility file.

## Task 0: Freeze workspace state and create the execution ledger

**Files:**

- Create: `docs/validation/m0-environment.md`
- Create: `reports/diagnostics/` at execution time; keep it ignored

- [ ] **Step 0.1: Confirm branch and clean starting point**

Run:

```powershell
git status --short --branch
git log -3 --oneline
```

Expected: branch `rewrite/driver-only`; only changes intentionally made for the current task are present. If unrelated changes exist, stop and report them rather than resetting them.

- [ ] **Step 0.2: Record immutable design inputs**

Write into `docs/validation/m0-environment.md`:

```markdown
# M0 Environment Ledger

- Normative SPEC: `docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md`
- Agent plan: `docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md`
- AIM source commit: `d379caa027d21902e45fce8c9daf02fd116c2dda`
- Working branch: `rewrite/driver-only`
- Real-driver tests authorized only after V0-V3 pass: yes
- Toolkit uninstall authorized: no
```

- [ ] **Step 0.3: Capture current non-mutating machine facts**

Run and paste the output into the environment ledger:

```powershell
cmake --version
ninja --version
& 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' `
  -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory |
  Sort-Object Name -Descending | Select-Object -First 3 -ExpandProperty Name
Get-Service phdskmnt,deviosvc | Select-Object Name,Status,StartType
(Get-Item 'C:\Windows\System32\drivers\phdskmnt.sys').VersionInfo |
  Select-Object FileVersion,ProductVersion,CompanyName
```

Expected: every command succeeds. A missing compiler, SDK, driver, or service is an environment blocker; do not create source files beyond the ledger until resolved.

- [ ] **Step 0.4: Confirm no implementation files exist yet**

Run:

```powershell
$implementation = @(rg --files src tests tools 2>$null)
if ($implementation.Count -ne 0) { throw "Unexpected implementation files already exist" }
```

Expected: exit 0.

- [ ] **Step 0.5: Commit the ledger**

```powershell
git add docs/validation/m0-environment.md
git commit -m "docs: record M0 execution environment"
```

Expected: one commit containing only the environment ledger.

## Task 1: Add deterministic MSVC/Ninja environment entry

**Files:**

- Create: `tools/Enter-MsvcEnvironment.ps1`
- Modify: `docs/validation/m0-environment.md`

- [ ] **Step 1.1: Write the environment script contract**

The script must:

```powershell
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found' }
$installationPath = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if ([string]::IsNullOrWhiteSpace($installationPath)) { throw 'MSVC x64 tools not found' }
$module = Join-Path $installationPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $module -ErrorAction Stop
Enter-VsDevShell -VsInstallPath $installationPath -SkipAutomaticLocation `
  -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe unavailable' }
if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) { throw 'ninja.exe unavailable' }
```

The file is dot-sourced; it must not call `exit`.

- [ ] **Step 1.2: Run from a fresh PowerShell process**

```powershell
powershell -NoProfile -Command ". .\tools\Enter-MsvcEnvironment.ps1; cl 2>&1 | Select-Object -First 1; ninja --version"
```

Expected: exit 0, one MSVC banner line, one Ninja version line.

- [ ] **Step 1.3: Verify wrong architecture is not selected**

```powershell
powershell -NoProfile -Command ". .\tools\Enter-MsvcEnvironment.ps1; if (`$env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'not x64' }"
```

Expected: exit 0.

- [ ] **Step 1.4: Record tool versions in the ledger**

Append the exact `cl`, CMake, Ninja and latest Windows SDK versions. Do not write ranges such as “recent”.

- [ ] **Step 1.5: Verify diff and commit**

```powershell
git diff --check
git add tools/Enter-MsvcEnvironment.ps1 docs/validation/m0-environment.md
git commit -m "build: add deterministic MSVC environment entry"
```

## Task 2: Bootstrap the C++20 build and named CTest suites

**Files:**

- Create: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `cmake/Warnings.cmake`
- Create: `src/CMakeLists.txt`
- Create: `tests/CMakeLists.txt`
- Create: `tests/support/test_framework.h`
- Create: `tests/support/test_main.cpp`
- Create: `tests/unit/common_tests.cpp`
- Create: `README.md`
- Create: `LICENSE`
- Create: `THIRD_PARTY.md`

- [ ] **Step 2.1: Write a failing configure smoke before adding CMake files**

Run:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
```

Expected: FAIL because `CMakePresets.json` does not exist. Save the output as red-phase evidence in `reports/diagnostics/task-02-red.txt`.

- [ ] **Step 2.2: Add root build constraints**

Use this complete root contract:

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyRamDiskM0 VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "M0 requires MSVC")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "M0 requires x64")
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

`cmake/Warnings.cmake` must expose `myrd_enable_warnings(target)` and add `/W4 /WX /permissive- /Zc:__cplusplus /EHsc /utf-8`.

- [ ] **Step 2.3: Add Ninja Multi-Config presets**

`CMakePresets.json` must define:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "msvc",
      "generator": "Ninja Multi-Config",
      "binaryDir": "${sourceDir}/out/build/msvc",
      "cacheVariables": {
        "CMAKE_CONFIGURATION_TYPES": "Debug;Release",
        "BUILD_TESTING": "ON"
      }
    }
  ],
  "buildPresets": [
    { "name": "debug", "configurePreset": "msvc", "configuration": "Debug" },
    { "name": "release", "configurePreset": "msvc", "configuration": "Release" }
  ],
  "testPresets": [
    {
      "name": "debug",
      "configurePreset": "msvc",
      "configuration": "Debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "release",
      "configurePreset": "msvc",
      "configuration": "Release",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

- [ ] **Step 2.4: Add a dependency-free test runner**

The runner must register named functions, print `PASS <name>` or `FAIL <name>: <reason>`, print `<passed>/<total> passed`, and return nonzero if any case fails. Define `TEST_CASE`, `REQUIRE`, and `REQUIRE_EQ` macros. The initial case is:

```cpp
TEST_CASE(repository_bootstrap_smoke) {
    REQUIRE(true);
    REQUIRE_EQ(sizeof(void*), 8u);
}
```

Register the executable in CTest as `unit.common`.

- [ ] **Step 2.5: Add repository metadata**

`.gitignore` must include `/out/`, `/reports/`, IDE state, and generated report JSON. `README.md` must state that M0 is not a production RAM disk and must not uninstall Toolkit. `THIRD_PARTY.md` must record the fixed AIM commit and AGPL source. `LICENSE` must contain the unmodified AGPL-3.0 text.

Acquire the license from the canonical GNU endpoint:

```powershell
Invoke-WebRequest 'https://www.gnu.org/licenses/agpl-3.0.txt' -OutFile .\LICENSE
if ((Get-Item .\LICENSE).Length -lt 30000) { throw 'AGPL license download is incomplete' }
```

If the endpoint is unavailable, stop Task 2 as an external documentation blocker rather than inventing or truncating license text.

- [ ] **Step 2.6: Configure, build, and test Debug**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset debug
ctest --preset debug -R '^unit\.common$'
```

Expected: all commands exit 0; CTest says `100% tests passed`.

- [ ] **Step 2.7: Build and test Release**

```powershell
cmake --build --preset release
ctest --preset release -R '^unit\.common$'
```

Expected: exit 0 with the same test result.

- [ ] **Step 2.8: Verify generated files stay ignored and commit**

```powershell
git status --short
git check-ignore out reports
git diff --check
git add .gitignore CMakeLists.txt CMakePresets.json cmake src tests README.md LICENSE THIRD_PARTY.md
git commit -m "build: bootstrap C++20 Ninja M0 project"
```

## Task 3: Acquire and prove the fixed AIM protocol source

**Files:**

- Create: `tools/Fetch-AimReference.ps1`
- Create: `docs/protocol/aim-m0-contract.md`
- Modify: `THIRD_PARTY.md`

- [ ] **Step 3.1: Implement a non-destructive reference fetcher**

The script accepts `-Destination`, defaults below `out/reference/aim`, and uses this commit:

```powershell
$ExpectedCommit = 'd379caa027d21902e45fce8c9daf02fd116c2dda'
$Repository = 'https://github.com/ArsenalRecon/Arsenal-Image-Mounter.git'
```

Rules:

- If destination does not exist, clone without modifying the project tree.
- If destination exists, require it to be a Git repository.
- Fetch and detach-checkout the exact commit.
- Run `git rev-parse HEAD` and compare case-insensitively with `$ExpectedCommit`.
- Verify all four normative source files exist.
- Print their absolute paths and SHA-256 values.
- Never delete or reset an arbitrary destination.

- [ ] **Step 3.2: Run the source gate**

```powershell
.\tools\Fetch-AimReference.ps1 -Destination .\out\reference\aim
git -C .\out\reference\aim rev-parse HEAD
```

Expected: exact fixed commit. If network access or checkout fails, record `BLOCKED_PROTOCOL_SOURCE` and stop M0 implementation; do not use remembered constants.

- [ ] **Step 3.3: Extract the authoritative symbol list**

Run:

```powershell
$aim = Resolve-Path .\out\reference\aim
rg -n 'SMP_IMSCSI_(QUERY_VERSION|CREATE_DEVICE|QUERY_DEVICE|QUERY_ADAPTER|REMOVE_DEVICE)' $aim
rg -n 'IMSCSI_(AUTO_DEVICE_NUMBER|ALL_DEVICES|TYPE_PROXY|PROXY_TYPE_SHM|DEVICE_TYPE_HD)' $aim
rg -n 'IMDPROXY_REQ_(INFO|READ|WRITE|CLOSE)|IMDPROXY_FLAGS' $aim
rg -n '_Request|_Response|_Server|BaseNamedObjects' $aim
```

Expected: every required symbol has at least one match in a normative source file.

- [ ] **Step 3.4: Write the protocol contract with no guessed values**

For every required constant and structure, record:

```markdown
| Local symbol | Upstream symbol | Exact value/layout | Source file:line | Notes |
|---|---|---|---|---|
```

Also record filename length unit/NUL rule, packing, request/response byte counts, SRB ReturnCode semantics, and object naming. Every row must contain an exact value or field layout; no blank cells or prose such as “standard value”.

- [ ] **Step 3.5: Cross-review the contract against all four sources**

Run one `rg` command per table row and append the command transcript under `## Audit transcript`. If managed and unmanaged implementations differ, stop and record the discrepancy.

- [ ] **Step 3.6: Run V0 provenance checks**

```powershell
$text = Get-Content .\docs\protocol\aim-m0-contract.md -Raw
if ($text -notmatch 'd379caa027d21902e45fce8c9daf02fd116c2dda') { throw 'commit missing' }
if ($text -match '\|\s*\|') { throw 'empty contract cell' }
git diff --check
```

Expected: exit 0.

- [ ] **Step 3.7: Commit the source gate and contract**

```powershell
git add tools/Fetch-AimReference.ps1 docs/protocol/aim-m0-contract.md THIRD_PARTY.md
git commit -m "docs: freeze audited AIM M0 protocol contract"
```

## Task 4: Implement common Result, ownership, and checked conversion primitives

**Files:**

- Create: `src/common/error.h`
- Create: `src/common/result.h`
- Create: `src/common/unique_handle.h`
- Create: `src/common/unique_view.h`
- Create: `src/common/unique_local.h`
- Create: `src/common/scope_exit.h`
- Create: `src/common/checked_cast.h`
- Modify: `tests/unit/common_tests.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 4.1: Add concrete failing tests**

Tests must cover these exact names and assertions:

```text
result_preserves_success_value
result_preserves_error_domain_code_and_operation
unique_handle_move_transfers_single_close
unique_handle_release_stops_ownership
unique_view_unmaps_once
scope_exit_runs_on_early_return
scope_exit_can_be_released
checked_cast_accepts_dword_max
checked_cast_rejects_value_above_dword_max
```

Use a real event for handle ownership, a real anonymous mapping for view ownership, and `std::numeric_limits<std::size_t>::max()` for overflow.

- [ ] **Step 4.2: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_common_tests
```

Expected: FAIL because the common headers do not exist.

- [ ] **Step 4.3: Implement the exact public types**

```cpp
namespace myrd {
enum class ErrorDomain : std::uint8_t {
    usage, preflight, protocol, win32, ntstatus,
    storage, cleanup, evidence, internal
};

struct Error {
    ErrorDomain domain;
    std::uint32_t code;
    std::wstring operation;
    std::wstring message;
};

template<class T> class Result;
template<> class Result<void>;

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept;
    ~UniqueHandle() noexcept;
    UniqueHandle(UniqueHandle&&) noexcept;
    UniqueHandle& operator=(UniqueHandle&&) noexcept;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const noexcept;
    HANDLE release() noexcept;
    void reset(HANDLE replacement = nullptr) noexcept;
    explicit operator bool() const noexcept;
};
}
```

`UniqueView` owns one `MapViewOfFile` pointer and calls `UnmapViewOfFile`. `ScopeExit<F>` executes once unless released. `checked_numeric_cast<T>(U)` returns `Result<T>` and validates both signedness and upper/lower bounds.

`UniqueLocal` owns memory returned by `LocalAlloc` or `ConvertStringSecurityDescriptorToSecurityDescriptorW` and calls `LocalFree` exactly once.

- [ ] **Step 4.4: Add compile-time ownership checks**

Add assertions:

```cpp
static_assert(!std::is_copy_constructible_v<myrd::UniqueHandle>);
static_assert(std::is_nothrow_move_constructible_v<myrd::UniqueHandle>);
static_assert(!std::is_copy_constructible_v<myrd::UniqueView>);
static_assert(!std::is_copy_constructible_v<myrd::UniqueLocal>);
```

- [ ] **Step 4.5: Run focused and full V1 tests**

```powershell
cmake --build --preset debug --target myrd_common_tests
ctest --preset debug -R '^unit\.common$'
cmake --build --preset release --target myrd_common_tests
ctest --preset release -R '^unit\.common$'
```

Expected: both configurations pass.

- [ ] **Step 4.6: Commit**

```powershell
git add src/common src/CMakeLists.txt tests/unit/common_tests.cpp
git commit -m "feat: add M0 error and resource ownership primitives"
```

## Task 5: Encode AIM and IMDPROXY ABI from the audited contract

**Files:**

- Create: `src/driver/aim_protocol.h`
- Create: `src/proxy/imdproxy_protocol.h`
- Create: `tests/unit/protocol_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 5.1: Write layout tests before local declarations**

For every structure in `docs/protocol/aim-m0-contract.md`, add:

```cpp
static_assert(sizeof(LocalType) == audited_size);
static_assert(alignof(LocalType) == audited_alignment);
static_assert(offsetof(LocalType, audited_field) == audited_offset);
```

For every operation constant, add runtime equality assertions against the exact audited numeric value. The test names must include:

```text
aim_control_codes_match_audited_contract
aim_create_layout_matches_audited_contract
aim_query_layouts_match_audited_contract
aim_remove_layout_matches_audited_contract
imdproxy_request_codes_match_audited_contract
imdproxy_layouts_match_audited_contract
```

- [ ] **Step 5.2: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_protocol_tests
```

Expected: FAIL because both protocol headers are absent.

- [ ] **Step 5.3: Implement only the M0 protocol surface**

The local headers must define strong wrappers and fixed parameters:

```cpp
namespace myrd::aim {
struct DeviceId { std::uint32_t value; };
struct DeviceAddress {
    std::uint8_t path_id;
    std::uint8_t target_id;
    std::uint8_t lun;
};
DeviceAddress decode_device_address(DeviceId id) noexcept;
struct ProxyDiskSpec {
    std::uint64_t size_bytes;
    std::uint32_t sector_size;
    std::wstring native_object_path;
};
constexpr std::uint64_t kM0DiskSize = 64ull * 1024ull * 1024ull;
constexpr std::uint32_t kM0SectorSize = 512;
}

namespace myrd::proxy {
constexpr std::size_t kHeaderSize = 4096;
constexpr std::size_t kDataAreaSize = 8ull * 1024ull * 1024ull;
constexpr std::size_t kMappingSize = kHeaderSize + kDataAreaSize;
}
```

Copy exact operation constants and wire structures from the audited contract. Do not add optional protocol operations beyond INFO, READ, WRITE and CLOSE.

`decode_device_address` follows the audited upstream union byte order. Add a test with a non-symmetric raw id so PathId, TargetId and Lun cannot accidentally be swapped.

- [ ] **Step 5.4: Add safe wire helpers**

Provide complete templates:

```cpp
template<class T>
Result<T> read_wire(std::span<const std::byte> bytes, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return Result<T>::failure({ErrorDomain::protocol, 0,
            L"read_wire", L"wire buffer is truncated"});
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return Result<T>::success(value);
}

template<class T>
Result<void> write_wire(std::span<std::byte> bytes, std::size_t offset,
                        const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"write_wire", L"wire buffer is too small"});
    }
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
    return Result<void>::success();
}
```

- [ ] **Step 5.5: Add truncation and sentinel tests**

Verify `read_wire` rejects every buffer size from 0 through `sizeof(T)-1`. Verify the automatic/all-devices sentinel is represented by a named constant and cannot be confused with a valid owned `DeviceId`.

- [ ] **Step 5.6: Run focused tests and inspect compiler packing output**

```powershell
cmake --build --preset debug --target myrd_protocol_tests
ctest --preset debug -R '^unit\.protocol$'
cmake --build --preset release --target myrd_protocol_tests
ctest --preset release -R '^unit\.protocol$'
```

Expected: all ABI assertions pass in both configurations.

- [ ] **Step 5.7: Commit**

```powershell
git add src/driver/aim_protocol.h src/proxy/imdproxy_protocol.h `
  tests/unit/protocol_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: encode audited AIM and IMDPROXY M0 ABI"
```

## Task 6: Implement the fixed 64 MiB memory block device

**Files:**

- Create: `src/storage/block_device.h`
- Create: `src/storage/memory_block_device.h`
- Create: `src/storage/memory_block_device.cpp`
- Create: `tests/unit/storage_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 6.1: Define the storage boundary and concrete cases**

Use this exact interface:

```cpp
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
}
```

Add these named cases:

```text
memory_device_reports_fixed_geometry
memory_device_starts_zeroed
memory_device_round_trips_4096_bytes_at_8_mib
memory_device_reads_last_sector
memory_device_rejects_zero_length
memory_device_rejects_unaligned_offset
memory_device_rejects_unaligned_length
memory_device_rejects_first_byte_past_end
memory_device_rejects_overflowed_range
```

- [ ] **Step 6.2: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_storage_tests
```

Expected: FAIL because the storage headers or target do not exist.

- [ ] **Step 6.3: Implement geometry and one shared validator**

Use subtraction-based bounds checking:

```cpp
Result<void> MemoryBlockDevice::validate(
    std::uint64_t offset, std::size_t length) const {
    if (length == 0) {
        return Result<void>::failure({ErrorDomain::storage, ERROR_INVALID_PARAMETER,
            L"MemoryBlockDevice::validate", L"zero-length I/O"});
    }
    if ((offset % sector_size_) != 0 || (length % sector_size_) != 0) {
        return Result<void>::failure({ErrorDomain::storage, ERROR_INVALID_PARAMETER,
            L"MemoryBlockDevice::validate", L"unaligned I/O"});
    }
    if (offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
        return Result<void>::failure({ErrorDomain::storage, ERROR_INVALID_PARAMETER,
            L"MemoryBlockDevice::validate", L"I/O outside device"});
    }
    return Result<void>::success();
}
```

The constructor rejects zero capacity, zero sector size, and capacity not divisible by sector size.

Use this concrete class boundary:

```cpp
class MemoryBlockDevice final : public IBlockDevice {
public:
    MemoryBlockDevice(std::uint64_t size_bytes, std::uint32_t sector_size);
    std::uint64_t size_bytes() const noexcept override;
    std::uint32_t sector_size() const noexcept override;
    Result<std::size_t> read(
        std::uint64_t offset, std::span<std::byte> output) override;
    Result<std::size_t> write(
        std::uint64_t offset, std::span<const std::byte> input) override;
private:
    Result<void> validate(std::uint64_t offset, std::size_t length) const;
    std::vector<std::byte> bytes_;
    std::uint32_t sector_size_;
};
```

- [ ] **Step 6.4: Implement full-length READ and WRITE**

On success, copy exactly the requested byte count and return that count. M0 never returns a short successful operation.

- [ ] **Step 6.5: Run focused Debug and Release tests**

```powershell
cmake --build --preset debug --target myrd_storage_tests
ctest --preset debug -R '^unit\.storage$'
cmake --build --preset release --target myrd_storage_tests
ctest --preset release -R '^unit\.storage$'
```

Expected: all nine cases pass in both configurations.

- [ ] **Step 6.6: Commit**

```powershell
git add src/storage tests/unit/storage_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add fixed M0 memory block device"
```

## Task 7: Implement pure INFO request processing

**Files:**

- Create: `src/proxy/request_processor.h`
- Create: `src/proxy/request_processor.cpp`
- Create: `tests/unit/request_processor_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 7.1: Define the processor API and counters**

```cpp
namespace myrd::proxy {
enum class ProcessorAction : std::uint8_t { respond, close, fatal };
struct RequestCounters {
    std::uint64_t info = 0;
    std::uint64_t read = 0;
    std::uint64_t write = 0;
    std::uint64_t close = 0;
    std::uint64_t fatal_protocol_errors = 0;
};
struct ProcessOutcome {
    ProcessorAction action;
    std::optional<Error> error;
};
class RequestProcessor final {
public:
    explicit RequestProcessor(IBlockDevice& device) noexcept;
    ProcessOutcome process(std::span<std::byte> mapping) noexcept;
    RequestCounters counters() const noexcept;
private:
    IBlockDevice& device_;
    RequestCounters counters_{};
};
}
```

- [ ] **Step 7.2: Write concrete failing INFO tests**

```text
processor_rejects_mapping_smaller_than_protocol_header
processor_info_reports_64_mib
processor_info_reports_512_alignment
processor_info_advertises_zero_optional_flags
processor_info_increments_only_info_counter
processor_info_does_not_touch_data_area
```

Fill the data area with `0xA5`; after INFO, assert every data byte remains unchanged.

- [ ] **Step 7.3: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_request_processor_tests
```

Expected: missing processor implementation.

- [ ] **Step 7.4: Implement INFO using wire helpers**

```text
Require mapping size == kMappingSize.
Read request code at offset 0 with read_wire.
For INFO, write InfoResponse{device size, sector size, zero flags} at offset 0.
Increment info only after the response write succeeds.
Return ProcessorAction::respond.
```

Do not clear the mapping and do not write beyond the response structure.

- [ ] **Step 7.5: Run focused tests**

```powershell
cmake --build --preset debug --target myrd_request_processor_tests
ctest --preset debug -R '^unit\.request_processor$'
```

Expected: all INFO cases pass.

- [ ] **Step 7.6: Commit**

```powershell
git add src/proxy/request_processor.* tests/unit/request_processor_tests.cpp `
  src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: process M0 proxy INFO requests"
```

## Task 8: Add READ, WRITE, CLOSE, and fatal request behavior

**Files:**

- Modify: `src/proxy/request_processor.h`
- Modify: `src/proxy/request_processor.cpp`
- Modify: `tests/unit/request_processor_tests.cpp`

- [ ] **Step 8.1: Add concrete READ tests**

```text
processor_read_copies_backend_bytes_to_data_area
processor_read_writes_success_response_after_data
processor_read_rejects_length_larger_than_data_area
processor_read_returns_error_response_for_unaligned_range
processor_read_returns_error_response_for_backend_range_failure
processor_read_does_not_increment_on_fatal_truncation
```

Use a recording fake block device to assert exact offset, length and call count.

- [ ] **Step 8.2: Add concrete WRITE tests**

```text
processor_write_copies_data_area_to_backend
processor_write_uses_private_copy_before_backend_call
processor_write_writes_success_response
processor_write_rejects_length_larger_than_data_area
processor_write_returns_error_response_for_unaligned_range
processor_write_returns_error_response_for_backend_failure
```

The private-copy test makes the fake backend mutate the shared area during its call and verifies the backend received the original bytes.

- [ ] **Step 8.3: Add CLOSE and unsupported-code tests**

```text
processor_close_returns_close_and_increments_counter
processor_unknown_code_returns_fatal
processor_unknown_code_increments_fatal_counter
processor_never_advertises_unimplemented_capabilities
```

- [ ] **Step 8.4: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_request_processor_tests
ctest --preset debug -R '^unit\.request_processor$'
```

Expected: new cases fail while INFO cases remain green.

- [ ] **Step 8.5: Implement one validation path before backend access**

Validate all six conditions:

```text
length is nonzero
length is not larger than kDataAreaSize
offset is aligned to device sector size
length is aligned to device sector size
offset is not larger than device size
length is not larger than device size minus offset
```

For a valid READ/WRITE request with backend failure, encode the protocol-defined nonzero error and zero completed length, signal-able as `respond`. An unknown code is `fatal` because M0 never advertised it.

- [ ] **Step 8.6: Verify the full processor suite**

```powershell
cmake --build --preset debug --target myrd_request_processor_tests
ctest --preset debug -R '^unit\.request_processor$'
cmake --build --preset release --target myrd_request_processor_tests
ctest --preset release -R '^unit\.request_processor$'
```

Expected: zero failures and exact counters.

- [ ] **Step 8.7: Commit**

```powershell
git add src/proxy/request_processor.* tests/unit/request_processor_tests.cpp
git commit -m "feat: process M0 proxy read write and close requests"
```

## Task 9: Validate session IDs, object names, and security descriptors

**Files:**

- Create: `src/proxy/object_names.h`
- Create: `src/proxy/object_names.cpp`
- Create: `tests/unit/object_names_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 9.1: Write exact name tests**

```text
object_names_accept_32_lowercase_hex_session
object_names_build_exact_win32_names
object_names_build_exact_native_path
object_names_reject_uppercase
object_names_reject_wrong_length
object_names_reject_separator
object_names_reject_non_hex
session_generator_produces_128_bits_and_unique_values
```

Expected names for session `00112233445566778899aabbccddeeff`:

```text
Global\myrd-m0-00112233445566778899aabbccddeeff
Global\myrd-m0-00112233445566778899aabbccddeeff_Request
Global\myrd-m0-00112233445566778899aabbccddeeff_Response
Global\myrd-m0-00112233445566778899aabbccddeeff_Server
\BaseNamedObjects\Global\myrd-m0-00112233445566778899aabbccddeeff
```

Use this public structure:

```cpp
struct ObjectNames {
    std::wstring mapping;
    std::wstring request;
    std::wstring response;
    std::wstring server;
    std::wstring driver_path;
    static Result<ObjectNames> from_session(std::wstring_view session_id);
};
Result<std::wstring> generate_session_id();
```

- [ ] **Step 9.2: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_object_names_tests
```

Expected: missing object-name implementation.

- [ ] **Step 9.3: Implement CSPRNG session generation**

Call `BCryptGenRandom(nullptr, bytes.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG)` and encode each byte as exactly two lowercase hexadecimal characters. Preserve a BCrypt failure code.

- [ ] **Step 9.4: Implement explicit object security**

Convert this SDDL into a self-relative descriptor owned by LocalFree RAII:

```text
D:P(A;;GA;;;SY)(A;;GA;;;BA)
```

The returned `SECURITY_ATTRIBUTES` remains valid only while its descriptor owner lives. Test with `GetSecurityDescriptorDacl` and require a present non-null DACL.

- [ ] **Step 9.5: Run focused tests and commit**

```powershell
cmake --build --preset debug --target myrd_object_names_tests
ctest --preset debug -R '^unit\.object_names$'
git add src/proxy/object_names.* tests/unit/object_names_tests.cpp `
  src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add secure M0 proxy object naming"
```

## Task 10: Own the shared mapping, events, and server mutex

**Files:**

- Create: `src/proxy/shared_memory_channel.h`
- Create: `src/proxy/shared_memory_channel.cpp`
- Create: `tests/integration/shm_proxy_server_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 10.1: Define the move-only channel API**

```cpp
namespace myrd::proxy {
class SharedMemoryChannel final {
public:
    static Result<SharedMemoryChannel> create(const ObjectNames& names);
    SharedMemoryChannel(SharedMemoryChannel&&) noexcept;
    SharedMemoryChannel& operator=(SharedMemoryChannel&&) noexcept;
    SharedMemoryChannel(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel& operator=(const SharedMemoryChannel&) = delete;
    std::span<std::byte> mapping() noexcept;
    HANDLE request_event() const noexcept;
    HANDLE response_event() const noexcept;
    const ObjectNames& names() const noexcept;
};
}
```

- [ ] **Step 10.2: Write failing resource tests**

```text
channel_creates_exact_mapping_capacity
channel_creates_auto_reset_request_event
channel_creates_auto_reset_response_event
channel_exposes_mapping_to_second_handle
channel_rejects_duplicate_session
channel_move_preserves_ownership
channel_destruction_releases_all_named_objects
```

For each auto-reset event, signal once and verify the first zero-time wait succeeds and the second times out.

- [ ] **Step 10.3: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_shm_proxy_server_tests
```

Expected: missing channel implementation.

- [ ] **Step 10.4: Create objects in rollback-safe order**

Use the explicit security attributes from Task 9 and this order:

```text
create server mutex and reject an existing name
create file mapping and reject ERROR_ALREADY_EXISTS
map the complete view
create Request auto-reset event and reject an existing name
create Response auto-reset event and reject an existing name
move all local owners into the returned channel
```

If any operation fails, local RAII variables release every earlier resource before returning.

- [ ] **Step 10.5: Verify capacity and cross-handle visibility**

Write the first and last valid mapping bytes and confirm a second view observes them. Do not probe outside the mapping.

- [ ] **Step 10.6: Verify post-destruction release**

After the channel leaves scope, `OpenFileMappingW`, `OpenEventW` and `OpenMutexW` for all names must fail with `ERROR_FILE_NOT_FOUND`.

- [ ] **Step 10.7: Run V2 channel tests and commit**

```powershell
cmake --build --preset debug --target myrd_shm_proxy_server_tests
ctest --preset debug -R '^integration\.shm$'
git add src/proxy/shared_memory_channel.* tests/integration/shm_proxy_server_tests.cpp `
  src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: own M0 shared memory channel resources"
```

## Task 11: Implement an event-driven Shared Memory server lifecycle

**Files:**

- Create: `src/proxy/shm_proxy_server.h`
- Create: `src/proxy/shm_proxy_server.cpp`
- Modify: `tests/integration/shm_proxy_server_tests.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 11.1: Define explicit server states**

```cpp
namespace myrd::proxy {
enum class ServerState : std::uint8_t {
    constructed, ready, running, close_seen, stopped, failed
};
class ShmProxyServer final {
public:
    ShmProxyServer(SharedMemoryChannel channel, RequestProcessor& processor);
    ~ShmProxyServer() noexcept;
    Result<void> start();
    Result<void> stop();
    ServerState state() const noexcept;
    std::optional<Error> worker_error() const;
};
}
```

- [ ] **Step 11.2: Write failing state and exchange tests**

```text
server_start_publishes_ready_before_return
server_completes_local_info_exchange
server_completes_local_write_then_read_exchange
server_close_transitions_to_close_seen
server_stop_before_start_is_safe
server_stop_while_waiting_is_bounded
server_stop_is_idempotent
server_unknown_request_transitions_to_failed
server_destructor_joins_worker
```

- [ ] **Step 11.3: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_shm_proxy_server_tests
ctest --preset debug -R '^integration\.shm$'
```

Expected: the new server cases fail.

- [ ] **Step 11.4: Wait on Request and Stop events together**

The worker uses:

```cpp
HANDLE waits[2]{channel_.request_event(), stop_event_.get()};
const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
```

Index 0 processes one request. Index 1 exits normally. `WAIT_FAILED` stores `GetLastError()`. A `respond` outcome signals Response once; `close` exits; `fatal` stores the processor error and exits failed.

- [ ] **Step 11.5: Make start and stop bounded and idempotent**

`start()` rejects a second start. `stop()` signals a private manual-reset event and joins once. A test measures stop-while-waiting and requires completion under 5 seconds.

- [ ] **Step 11.6: Repeat the local V2 suite 50 times**

```powershell
1..50 | ForEach-Object {
  ctest --preset debug -R '^integration\.shm$' | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "local SHM iteration $_ failed" }
}
```

Expected: 50/50 passes with no name collision or teardown race.

- [ ] **Step 11.7: Commit**

```powershell
git add src/proxy/shm_proxy_server.* tests/integration/shm_proxy_server_tests.cpp src/CMakeLists.txt
git commit -m "feat: run event-driven M0 shared memory server"
```

## Task 12: Build and validate generic SRB frames

**Files:**

- Create: `src/driver/srb_transport.h`
- Create: `src/driver/srb_codec.h`
- Create: `src/driver/srb_codec.cpp`
- Create: `tests/unit/srb_codec_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 12.1: Define the injectable transport boundary**

```cpp
namespace myrd::aim {
class ISrbTransport {
public:
    virtual ~ISrbTransport() = default;
    virtual Result<std::vector<std::byte>> transact(
        std::uint32_t control_code,
        std::span<const std::byte> request_payload,
        std::size_t response_payload_capacity,
        std::uint32_t timeout_seconds) = 0;
};
}
```

- [ ] **Step 12.2: Write failing frame-construction tests**

```text
srb_frame_uses_exact_header_length
srb_frame_uses_exact_eight_byte_signature
srb_frame_preserves_control_code
srb_frame_preserves_timeout
srb_frame_sets_request_payload_length
srb_frame_zeroes_return_code_before_call
srb_frame_rejects_payload_above_ulong
```

- [ ] **Step 12.3: Write failing response-validation tests**

```text
srb_response_rejects_short_header
srb_response_rejects_wrong_signature
srb_response_rejects_wrong_header_length
srb_response_rejects_wrong_control_code
srb_response_rejects_length_beyond_bytes_returned
srb_response_preserves_nonzero_return_code
srb_response_returns_exact_payload_slice
```

- [ ] **Step 12.4: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_srb_codec_tests
```

Expected: missing codec.

- [ ] **Step 12.5: Implement frame construction with checked casts**

Construct one zero-initialized vector large enough for the header plus the larger of request size and response capacity. Fill every field from the audited protocol contract. Validate every conversion to `ULONG` and `DWORD` before allocation or copy.

- [ ] **Step 12.6: Parse responses as untrusted input**

Validate in this order: total header size, copied header, signature, HeaderLength, ControlCode, declared Length, `bytes_returned`, ReturnCode. Return a payload span only after structural checks pass.

- [ ] **Step 12.7: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_srb_codec_tests
ctest --preset debug -R '^unit\.srb_codec$'
cmake --build --preset release --target myrd_srb_codec_tests
ctest --preset release -R '^unit\.srb_codec$'
git add src/driver/srb_* tests/unit/srb_codec_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add validated AIM SRB frame codec"
```

## Task 13: Encode AIM operations and implement DirectAimAdapter with a fake transport

**Files:**

- Create: `src/driver/direct_aim_adapter.h`
- Create: `src/driver/direct_aim_adapter.cpp`
- Create: `tests/unit/direct_aim_adapter_tests.cpp`
- Modify: `src/driver/srb_codec.h`
- Modify: `src/driver/srb_codec.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 13.1: Define public result types**

```cpp
namespace myrd::aim {
struct DriverVersion { std::uint32_t raw; };
struct DiskInfo {
    DeviceId id;
    std::uint64_t size_bytes;
    std::uint32_t sector_size;
    std::uint32_t flags;
    std::wstring native_object_path;
};
class DirectAimAdapter final {
public:
    explicit DirectAimAdapter(ISrbTransport& transport) noexcept;
    Result<DriverVersion> query_version();
    Result<std::vector<DeviceId>> list_devices();
    Result<DeviceId> create_proxy_disk(const ProxyDiskSpec& spec);
    Result<DiskInfo> query_device(DeviceId id);
    Result<void> remove_device(DeviceId id);
};
}
```

- [ ] **Step 13.2: Implement a byte-accurate FakeSrbTransport in tests**

The fake records every control code, timeout, request payload and response capacity. It queues exact byte responses, injects transport errors, and exposes `send_count()` plus `calls()`.

- [ ] **Step 13.3: Write QUERY tests**

```text
adapter_query_version_decodes_exact_value
adapter_query_version_rejects_truncated_payload
adapter_list_devices_decodes_zero_one_and_many
adapter_list_devices_rejects_count_length_mismatch
adapter_query_device_requires_returned_id_to_match_requested_id
```

- [ ] **Step 13.4: Write CREATE tests**

```text
adapter_create_encodes_64_mib_512_and_shared_memory_flags
adapter_create_encodes_exact_native_object_path_length
adapter_create_rejects_empty_path
adapter_create_rejects_unaligned_size
adapter_create_returns_exact_driver_device_id
adapter_create_does_not_hide_returned_id_behind_query
adapter_query_device_decodes_exact_path_flags_and_geometry
```

- [ ] **Step 13.5: Write mandatory REMOVE safety tests**

```text
adapter_remove_rejects_all_devices_sentinel_without_transport
adapter_remove_sends_exact_owned_device_id
adapter_remove_rejects_response_for_different_device
adapter_remove_propagates_driver_return_code
```

The sentinel case must assert `send_count() == 0`.

- [ ] **Step 13.6: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_direct_aim_adapter_tests
```

Expected: undefined adapter or operation codecs.

- [ ] **Step 13.7: Implement pure encoders and decoders**

Give QUERY_VERSION, QUERY_ADAPTER, CREATE_DEVICE, QUERY_DEVICE and REMOVE_DEVICE separate encode/decode functions. String encoding, NUL behavior and byte lengths must match the audited contract exactly. `DirectAimAdapter` must not manipulate byte offsets itself.

- [ ] **Step 13.8: Return the CREATE id immediately and guard REMOVE**

`create_proxy_disk` returns the validated CREATE response id immediately and performs no hidden QUERY. This lets the lifecycle own the exact id before any later failure. `query_device` remains a separate call. Reject the sentinel before calling an encoder or transport.

- [ ] **Step 13.9: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_direct_aim_adapter_tests
ctest --preset debug -R '^unit\.direct_aim_adapter$'
cmake --build --preset release --target myrd_direct_aim_adapter_tests
ctest --preset release -R '^unit\.direct_aim_adapter$'
git add src/driver tests/unit/direct_aim_adapter_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add fake-tested direct AIM adapter"
```

## Task 14: Discover the AIM adapter and transact with IOCTL_SCSI_MINIPORT

**Files:**

- Create: `src/driver/adapter_discovery.h`
- Create: `src/driver/adapter_discovery.cpp`
- Create: `src/driver/windows_srb_transport.h`
- Create: `src/driver/windows_srb_transport.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 14.1: Define discovery output and transport ownership**

```cpp
namespace myrd::aim {
struct AdapterHandle {
    std::wstring interface_path;
    UniqueHandle handle;
};
Result<AdapterHandle> discover_aim_adapter();
class WindowsSrbTransport final : public ISrbTransport {
public:
    explicit WindowsSrbTransport(HANDLE adapter) noexcept;
    Result<std::vector<std::byte>> transact(
        std::uint32_t, std::span<const std::byte>,
        std::size_t, std::uint32_t) override;
private:
    HANDLE adapter_;
};
}
```

The transport borrows the handle; `AdapterHandle` owns it.

- [ ] **Step 14.2: Add a non-real invalid-handle test**

Instantiate `WindowsSrbTransport` with `INVALID_HANDLE_VALUE`, call a small transaction, and require a typed Win32 error without crash. The test must not enumerate or contact the driver.

- [ ] **Step 14.3: Implement SetupAPI enumeration**

For `GUID_DEVINTERFACE_STORAGEPORT`:

1. Enumerate present device interfaces.
2. Obtain detail paths through a two-call size query.
3. Obtain `DEVPKEY_Device_Service` through a two-call property query.
4. Retain service `phdskmnt`, case-insensitive.
5. Open read/write with shared read/write.
6. Probe QUERY_VERSION using a temporary transport.
7. Require exactly one protocol-valid adapter.

Own every SetupAPI list with RAII calling `SetupDiDestroyDeviceInfoList`.

- [ ] **Step 14.4: Implement the IOCTL call**

```cpp
BOOL ok = DeviceIoControl(
    adapter_, IOCTL_SCSI_MINIPORT,
    buffer.data(), input_size,
    buffer.data(), output_size,
    &bytes_returned, nullptr);
```

Save `GetLastError()` immediately on failure. On success, resize to `bytes_returned` only after range validation, then pass bytes through `SrbCodec`.

- [ ] **Step 14.5: Link only Windows SDK libraries**

Add `Setupapi` and `Cfgmgr32` plus required system libraries. Never add an AIM Toolkit directory or import library.

- [ ] **Step 14.6: Build without real discovery**

```powershell
cmake --build --preset debug
ctest --preset debug -E '^real\.'
cmake --build --preset release
ctest --preset release -E '^real\.'
```

Expected: all non-real tests pass.

- [ ] **Step 14.7: Inspect link inputs and commit**

```powershell
$matches = @(rg -i 'aimapi|aim_ll|ramdyn|aim toolkit' out/build/msvc -g '*.rsp' -g '*.ninja')
if ($matches.Count -ne 0) { $matches; throw 'Forbidden AIM link input' }
git add src/driver/adapter_discovery.* src/driver/windows_srb_transport.* src/CMakeLists.txt
git commit -m "feat: discover AIM storage port and send miniport IOCTLs"
```

## Task 15: Enumerate disks and prove exact PhysicalDrive identity

**Files:**

- Create: `src/driver/physical_drive_locator.h`
- Create: `src/driver/physical_drive_locator.cpp`
- Create: `tests/unit/physical_drive_locator_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 15.1: Separate pure matching from Windows enumeration**

```cpp
namespace myrd::aim {
struct DiskCandidate {
    std::uint8_t path_id;
    std::uint8_t target_id;
    std::uint8_t lun;
    std::uint32_t physical_drive_number;
    std::uint64_t length_bytes;
    bool existed_before;
};
struct LocatedDisk {
    std::uint32_t physical_drive_number;
    std::wstring path;
    std::uint64_t length_bytes;
};
Result<LocatedDisk> match_created_disk(
    DeviceId created, std::span<const DiskCandidate> candidates);
Result<std::vector<std::uint32_t>> snapshot_physical_drive_numbers();
Result<LocatedDisk> wait_for_created_physical_drive(
    DeviceId created, std::span<const std::uint32_t> baseline,
    std::chrono::milliseconds timeout);
}
```

- [ ] **Step 15.2: Write pure identity cases**

```text
locator_matches_exact_path_target_lun
locator_rejects_zero_matches
locator_rejects_two_exact_matches
locator_rejects_candidate_that_existed_before
locator_rejects_length_not_64_mib
locator_builds_exact_decimal_physicaldrive_path
locator_does_not_match_capacity_without_scsi_address
```

- [ ] **Step 15.3: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_physical_drive_locator_tests
```

Expected: missing locator implementation.

- [ ] **Step 15.4: Implement disk-interface enumeration**

Enumerate `GUID_DEVINTERFACE_DISK`. For each openable interface, query `IOCTL_SCSI_GET_ADDRESS`, `IOCTL_STORAGE_GET_DEVICE_NUMBER` and `IOCTL_DISK_GET_LENGTH_INFO`. Enumeration opens no write access.

- [ ] **Step 15.5: Implement bounded matching**

Every 250 ms, enumerate and run the pure matcher. Zero matches retries until 15 seconds. Ambiguity or identity conflict fails immediately. Re-query a success after 100 ms; both observations must agree.

- [ ] **Step 15.6: Validate the PhysicalDrive path parser**

Accept only `\\.\PhysicalDrive` followed by decimal digits and no suffix. Parse with overflow checks. Before any write-capable open, compare the parsed number with `IOCTL_STORAGE_GET_DEVICE_NUMBER`.

- [ ] **Step 15.7: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_physical_drive_locator_tests
ctest --preset debug -R '^unit\.physical_drive_locator$'
cmake --build --preset release --target myrd_physical_drive_locator_tests
ctest --preset release -R '^unit\.physical_drive_locator$'
git add src/driver/physical_drive_locator.* tests/unit/physical_drive_locator_tests.cpp `
  src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: prove exact M0 PhysicalDrive identity"
```

## Task 16: Add SHA-256, Authenticode, module, and process evidence

**Files:**

- Create: `src/common/sha256.h`
- Create: `src/common/sha256.cpp`
- Create: `src/common/authenticode.h`
- Create: `src/common/authenticode.cpp`
- Create: `src/common/process_snapshot.h`
- Create: `src/common/process_snapshot.cpp`
- Modify: `tests/unit/common_tests.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 16.1: Add SHA-256 known-vector tests**

Test empty input and ASCII `abc`:

```text
E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855
BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD
```

Also hash a 4096-byte deterministic buffer in one call and in 512-byte chunks; digests must match.

- [ ] **Step 16.2: Add Authenticode error-path tests**

```text
authenticode_rejects_missing_file
authenticode_returns_status_and_signer_fields
authenticode_closes_provider_state_on_every_path
```

The signed-file case may use a Windows system binary, but must not assert a specific publisher string.

- [ ] **Step 16.3: Add process snapshot tests**

Verify the current executable appears in the current-process module list and the current process appears in the process snapshot. Forbidden-name matching is case-insensitive and compares basename only.

- [ ] **Step 16.4: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_common_tests
ctest --preset debug -R '^unit\.common$'
```

Expected: new evidence cases fail.

- [ ] **Step 16.5: Implement BCrypt SHA-256 with RAII**

Open `BCRYPT_SHA256_ALGORITHM`, query object/hash lengths, allocate exact buffers, create/update/finish the hash, and close algorithm/hash handles on every exit. Output uppercase hex.

- [ ] **Step 16.6: Implement offline Authenticode verification**

Use `WinVerifyTrust` with `WINTRUST_ACTION_GENERIC_VERIFY_V2`, `WTD_REVOKE_NONE`, and `WTD_CACHE_ONLY_URL_RETRIEVAL`. Extract the signer through `CryptQueryObject`, `CryptMsgGetParam`, `CertFindCertificateInStore`, and `CertGetNameStringW`. Always call the close-state action.

- [ ] **Step 16.7: Implement Toolhelp snapshots**

Use `TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32` for current-process modules and `TH32CS_SNAPPROCESS` for process names. Treat `ERROR_NO_MORE_FILES` as normal end-of-enumeration.

- [ ] **Step 16.8: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_common_tests
ctest --preset debug -R '^unit\.common$'
cmake --build --preset release --target myrd_common_tests
ctest --preset release -R '^unit\.common$'
git add src/common tests/unit/common_tests.cpp src/CMakeLists.txt
git commit -m "feat: collect cryptographic and dependency evidence"
```

## Task 17: Implement the exact JSON report schema and atomic writer

**Files:**

- Create: `src/poc/json_writer.h`
- Create: `src/poc/json_writer.cpp`
- Create: `src/poc/poc_report.h`
- Create: `src/poc/poc_report.cpp`
- Create: `tests/unit/report_tests.cpp`
- Create: `tools/Verify-M0Json.ps1`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 17.1: Encode every SPEC field as a typed C++ member**

Use nested structures corresponding one-to-one with SPEC section 17. Required scalar fields must not be stored in a generic string map.

```cpp
struct PocReport {
    std::uint32_t schema_version = 1;
    std::string mode;
    std::string run_id;
    std::string started_utc;
    std::string finished_utc;
    bool success = false;
    int exit_code = 12;
    std::string last_stage = "started";
    EnvironmentEvidence environment;
    DependencyEvidence dependency_audit;
    DriverEvidence driver;
    BaselineEvidence baseline;
    ProviderEvidence provider;
    std::optional<CreatedDeviceEvidence> created_device;
    std::optional<RawRoundTripEvidence> raw_round_trip;
    CleanupEvidence cleanup;
    std::optional<ErrorEvidence> primary_error;
    std::vector<ErrorEvidence> cleanup_errors;
};
```

- [ ] **Step 17.2: Write failing JSON and schema tests**

```text
json_writer_escapes_quote_backslash_and_controls
json_writer_emits_booleans_as_json_booleans
json_writer_emits_integers_without_quotes
json_writer_emits_null_optional_sections
report_contains_every_required_top_level_key
report_preserves_cleanup_and_primary_error_together
report_utf8_encodes_non_ascii_signer
```

- [ ] **Step 17.3: Add an independent PowerShell parser**

`tools/Verify-M0Json.ps1`:

```powershell
param([Parameter(Mandatory)][string]$Path)
$report = Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json
if ($report.schema_version -ne 1) { throw 'schema_version mismatch' }
if ($null -eq $report.cleanup) { throw 'cleanup object missing' }
if ($null -eq $report.environment) { throw 'environment object missing' }
```

CTest runs it against a fixture emitted by the C++ report tests.

- [ ] **Step 17.4: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_report_tests
ctest --preset debug -R '^unit\.report'
```

Expected: report implementation is absent.

- [ ] **Step 17.5: Implement deterministic UTF-8 JSON**

Emit fields in SPEC order. Escape U+0000 through U+001F, quote and backslash. Convert UTF-16 with `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)`; invalid UTF-16 is an evidence error.

- [ ] **Step 17.6: Implement no-overwrite atomic publication**

Create parent directories, reject an existing final path, write a unique sibling temporary file, flush it, close it, then rename without replace-existing. If publication fails, print the retained temporary path to stderr.

- [ ] **Step 17.7: Run C++ and independent parser tests**

```powershell
cmake --build --preset debug --target myrd_report_tests
ctest --preset debug -R '^unit\.report'
cmake --build --preset release --target myrd_report_tests
ctest --preset release -R '^unit\.report'
```

Expected: all report and parser tests pass.

- [ ] **Step 17.8: Commit**

```powershell
git add src/poc/json_writer.* src/poc/poc_report.* tests/unit/report_tests.cpp `
  tools/Verify-M0Json.ps1 src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: emit strict atomic M0 evidence reports"
```

## Task 18: Parse CLI commands without side effects

**Files:**

- Create: `src/poc/cli.h`
- Create: `src/poc/cli.cpp`
- Modify: `tests/unit/report_tests.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 18.1: Define the parsed command**

```cpp
namespace myrd::poc {
enum class CommandMode : std::uint8_t { query_driver, run };
enum class FailurePoint : std::uint8_t {
    none, create_returned, device_confirmed,
    physical_drive_confirmed, round_trip_verified
};
struct CommandLine {
    CommandMode mode;
    std::filesystem::path report_path;
    FailurePoint injected_failure = FailurePoint::none;
};
Result<CommandLine> parse_command_line(
    std::span<const std::wstring_view> args, bool debug_build);
}
```

- [ ] **Step 18.2: Write valid and invalid argument cases**

```text
cli_parses_query_driver_report
cli_parses_run_report
cli_rejects_unknown_command
cli_rejects_missing_report
cli_rejects_duplicate_report
cli_rejects_unknown_option
cli_rejects_existing_report_path
cli_accepts_failure_injection_only_in_debug
cli_rejects_failure_injection_for_query_driver
```

- [ ] **Step 18.3: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_report_tests
ctest --preset debug -R '^unit\.report$'
```

Expected: CLI cases fail.

- [ ] **Step 18.4: Implement a strict single-pass parser**

Reject all extra or duplicate arguments. Normalize the report path to an absolute path without requiring the final file to exist. Reject device-namespace and named-pipe paths.

- [ ] **Step 18.5: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_report_tests
ctest --preset debug -R '^unit\.report$'
git add src/poc/cli.* tests/unit/report_tests.cpp src/CMakeLists.txt
git commit -m "feat: parse side-effect-free M0 commands"
```

## Task 19: Automate build, test, import, and JSON verification tiers

**Files:**

- Create: `tools/Audit-M0Dependencies.ps1`
- Create: `tools/Invoke-M0Verification.ps1`
- Modify: `README.md`

- [ ] **Step 19.1: Implement the Release import-table audit**

`Audit-M0Dependencies.ps1` accepts `-Executable`, finds `dumpbin.exe` through the active MSVC environment, runs `/dependents`, prints the complete list, and fails on any forbidden basename:

```text
aimapi.dll
aim_ll.exe
RamDyn.exe
AIMtk-Dlg.exe
AIMtk-svc.exe
aim_cli.exe
MountImg.exe
RamDiskUI.exe
deviosvc.exe
```

Matching is case-insensitive.

- [ ] **Step 19.2: Define verification orchestrator parameters**

```powershell
param(
  [ValidateSet('V0','V1','V2','V3','V4','V5','V6')]
  [string]$Tier = 'V2',
  [ValidateSet('Debug','Release')]
  [string]$Configuration = 'Debug',
  [switch]$ListCommands
)
```

- [ ] **Step 19.3: Implement V0 through V2**

```text
V0 verifies fixed source commit, complete protocol contract and git diff --check.
V1 runs configure, selected build and every unit.* CTest.
V2 runs V1, then integration.shm 50 times.
```

The script stops at the first nonzero command and prints the exact failing tier and command.

- [ ] **Step 19.4: Test command composition without real execution**

`-ListCommands` prints but does not execute. Verify V2 contains V0 and V1 commands, while V1 contains no `query-driver`, `run`, service-control or PhysicalDrive command.

- [ ] **Step 19.5: Run V0-V2 through one command**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
.\tools\Invoke-M0Verification.ps1 -Tier V2 -Configuration Debug
```

Expected: exit 0 and final line `V2 PASS`.

- [ ] **Step 19.6: Commit**

```powershell
git add tools/Audit-M0Dependencies.ps1 tools/Invoke-M0Verification.ps1 README.md
git commit -m "test: automate M0 verification tiers"
```

## Task 20: Implement the cleanup-first lifecycle runner against fakes

**Files:**

- Create: `src/poc/lifecycle_runner.h`
- Create: `src/poc/lifecycle_runner.cpp`
- Create: `tests/unit/lifecycle_runner_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 20.1: Define narrow injectable collaborators**

```cpp
namespace myrd::poc {
class IProviderLifecycle {
public:
    virtual ~IProviderLifecycle() = default;
    virtual Result<void> start() = 0;
    virtual Result<void> stop_and_release() = 0;
    virtual std::wstring native_object_path() const = 0;
    virtual proxy::RequestCounters counters() const = 0;
    virtual Result<bool> named_objects_released() const = 0;
};
class IKernelDiskAdapter {
public:
    virtual ~IKernelDiskAdapter() = default;
    virtual Result<std::vector<aim::DeviceId>> list_devices() = 0;
    virtual Result<aim::DeviceId> create_proxy_disk(
        const aim::ProxyDiskSpec&) = 0;
    virtual Result<aim::DiskInfo> query_device(aim::DeviceId) = 0;
    virtual Result<void> remove_device(aim::DeviceId) = 0;
    virtual Result<void> wait_until_absent(
        aim::DeviceId, std::chrono::milliseconds) = 0;
};
class IDiskLocator {
public:
    virtual ~IDiskLocator() = default;
    virtual Result<std::vector<std::uint32_t>> snapshot_numbers() = 0;
    virtual Result<aim::LocatedDisk> wait_for(
        aim::DeviceId, std::span<const std::uint32_t>) = 0;
};
class IRoundTrip {
public:
    virtual ~IRoundTrip() = default;
    virtual Result<RawRoundTripEvidence> run(const aim::LocatedDisk&) = 0;
};
}
```

`DirectAimAdapter` is wrapped by this interface in production; tests provide a fake.

- [ ] **Step 20.2: Define the exact lifecycle stages**

```cpp
enum class LifecycleStage : std::uint8_t {
    started, preflight_passed, baseline_captured, provider_ready,
    create_returned, device_confirmed, physical_drive_confirmed,
    round_trip_verified, device_removed, provider_stopped,
    cleanup_verified, report_written
};
```

- [ ] **Step 20.3: Write the success-order test**

The fake call log must exactly equal:

```text
list_devices_before
snapshot_physical_drives_before
provider_start
create_proxy_disk
query_created_device
locate_physical_drive
raw_round_trip
remove_exact_device
wait_device_absent
provider_stop_and_release
check_named_objects
list_devices_after
snapshot_physical_drives_after
```

- [ ] **Step 20.4: Write one failure test for every resource stage**

```text
lifecycle_provider_start_failure_does_not_remove
lifecycle_create_failure_does_not_remove_without_owned_id
lifecycle_failure_after_create_removes_exact_id
lifecycle_query_confirmation_failure_removes_exact_id
lifecycle_query_confirmation_path_mismatch_removes_exact_id
lifecycle_query_confirmation_geometry_mismatch_removes_exact_id
lifecycle_locator_failure_removes_exact_id
lifecycle_round_trip_failure_removes_exact_id
lifecycle_remove_failure_is_reported
lifecycle_provider_stop_and_release_failure_is_reported
lifecycle_preserves_primary_error_and_cleanup_errors
lifecycle_never_removes_sentinel
```

- [ ] **Step 20.5: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_lifecycle_runner_tests
ctest --preset debug -R '^unit\.lifecycle_runner$'
```

Expected: lifecycle implementation is absent.

- [ ] **Step 20.6: Implement the optional owned DeviceId slot**

The slot starts empty and is assigned immediately after a validated CREATE response, before QUERY_DEVICE. Its setter rejects the sentinel. The lifecycle then calls QUERY_DEVICE and compares id, native path, flags and geometry with the request. Cleanup reads only the owned slot. Cleanup is called from success and every error path and is protected by a final scope guard.

- [ ] **Step 20.7: Preserve primary and cleanup errors independently**

The first non-cleanup failure becomes `primary_error`. Every cleanup failure appends to `cleanup_errors`. A successful data path with failed cleanup is overall failure with exit code 10.

- [ ] **Step 20.8: Add deterministic Debug failure injection**

After each supported stage, compare the requested `FailurePoint`. On match, create an internal injected error and execute normal cleanup. Compile the injection branch only for `_DEBUG`; Release argument parsing rejects it.

- [ ] **Step 20.9: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_lifecycle_runner_tests
ctest --preset debug -R '^unit\.lifecycle_runner$'
cmake --build --preset release --target myrd_lifecycle_runner_tests
ctest --preset release -R '^unit\.lifecycle_runner$'
git add src/poc/lifecycle_runner.* tests/unit/lifecycle_runner_tests.cpp `
  src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add exact-cleanup M0 lifecycle state machine"
```

## Task 21: Implement raw round trip behind a fakeable I/O boundary

**Files:**

- Create: `src/poc/raw_round_trip.h`
- Create: `src/poc/raw_round_trip.cpp`
- Modify: `tests/unit/lifecycle_runner_tests.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 21.1: Define raw I/O ownership and operation order**

```cpp
namespace myrd::poc {
class IRawDiskIo {
public:
    virtual ~IRawDiskIo() = default;
    virtual Result<void> verify_identity(const aim::LocatedDisk&) = 0;
    virtual Result<void> seek(std::uint64_t offset) = 0;
    virtual Result<void> write_exact(std::span<const std::byte>) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> read_exact(std::span<std::byte>) = 0;
};
class RawRoundTrip final : public IRoundTrip {
public:
    explicit RawRoundTrip(IRawDiskIo& io) noexcept;
    Result<RawRoundTripEvidence> run(const aim::LocatedDisk&) override;
private:
    IRawDiskIo& io_;
};
}
```

- [ ] **Step 21.2: Write exact-order tests**

```text
round_trip_verifies_identity_before_write_open
round_trip_uses_offset_8_mib_and_length_4096
round_trip_flushes_between_write_and_read
round_trip_reseeks_before_read
round_trip_rejects_short_write
round_trip_rejects_short_read
round_trip_rejects_hash_mismatch
round_trip_report_contains_no_raw_bytes
```

- [ ] **Step 21.3: Run the red phase**

```powershell
cmake --build --preset debug --target myrd_lifecycle_runner_tests
ctest --preset debug -R '^unit\.lifecycle_runner$'
```

Expected: round-trip cases fail.

- [ ] **Step 21.4: Generate and hash exactly 4096 random bytes**

Use `BCryptGenRandom` and compute the written digest before opening the disk for write. Never print or serialize the buffer.

- [ ] **Step 21.5: Implement Windows raw identity recheck**

Before returning a write-capable handle:

1. Parse the exact PhysicalDrive path.
2. Open read-only and re-query device number plus length.
3. Compare with `LocatedDisk` and 64 MiB.
4. Close the read-only handle.
5. Open GENERIC_READ | GENERIC_WRITE with shared read/write.
6. Query identity again on the write handle.

Only after step 6 may seek or write occur.

- [ ] **Step 21.6: Run tests and commit**

```powershell
cmake --build --preset debug --target myrd_lifecycle_runner_tests
ctest --preset debug -R '^unit\.lifecycle_runner$'
git add src/poc/raw_round_trip.* tests/unit/lifecycle_runner_tests.cpp src/CMakeLists.txt
git commit -m "feat: add identity-guarded M0 raw round trip"
```

## Task 22: Assemble query-driver and run modes in the executable

**Files:**

- Create: `src/poc/main.cpp`
- Create: `src/poc/m0_provider.h`
- Create: `src/poc/m0_provider.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `README.md`

- [ ] **Step 22.1: Create the executable target with system libraries only**

Target name: `myrd-m0-poc`. Link internal libraries and Windows SDK libraries only. Do not use delay-load or `LoadLibrary` for AIM components.

- [ ] **Step 22.2: Implement one top-level exception boundary**

`wmain` performs this exact sequence:

```text
initialize report with start time and run id
parse arguments
collect environment and dependency evidence
dispatch query-driver or run
catch std::exception and unknown exceptions
set final success, exit code, last stage and finish time
publish report exactly once
return report exit code
```

- [ ] **Step 22.3: Implement query-driver as read-only**

Discover the adapter, query version/list, inspect driver path/version/signature/hash, record modules/processes, and take before/after AIM and PhysicalDrive snapshots. Do not construct object names, a channel, memory block device, or call CREATE/REMOVE.

Process classification is mode-aware: `query-driver` records a pre-existing `deviosvc` but does not classify it as forbidden; `run` classifies any `deviosvc` process during the Provider lifecycle as a failure because the harness must have stopped it.

- [ ] **Step 22.4: Compose run mode without executing it**

Run requires elevation, acquires a global M0-run mutex, constructs concrete provider/adapter/locator/round-trip objects, and invokes `LifecycleRunner`. `M0Provider` owns MemoryBlockDevice, ObjectNames, SharedMemoryChannel, RequestProcessor and ShmProxyServer. Its `stop_and_release()` stops the worker, snapshots counters, destroys server/channel handles, and retains only names plus counters for reopen checks. Do not manipulate `deviosvc` from C++.

- [ ] **Step 22.5: Finalize protocol and cleanup evidence**

Copy INFO/READ/WRITE/CLOSE/fatal counters. Release all named-object owners, attempt reopen checks, and record the result. Always take final device snapshots before publishing the report.

- [ ] **Step 22.6: Build Debug and Release**

```powershell
cmake --build --preset debug --target myrd-m0-poc
cmake --build --preset release --target myrd-m0-poc
```

Expected: both executables link.

- [ ] **Step 22.7: Verify usage paths without driver mutation**

```powershell
& .\out\build\msvc\src\Debug\myrd-m0-poc.exe
if ($LASTEXITCODE -ne 2) { throw 'usage exit code mismatch' }
& .\out\build\msvc\src\Debug\myrd-m0-poc.exe invalid --report .\reports\invalid.json
if ($LASTEXITCODE -ne 2) { throw 'invalid command exit code mismatch' }
```

Expected: exit code 2 both times and no device change.

- [ ] **Step 22.8: Commit**

```powershell
git add src/poc/main.cpp src/poc/m0_provider.* src/CMakeLists.txt README.md
git commit -m "feat: assemble M0 query and lifecycle executable"
```

## Task 23: Pass the read-only V3 real-driver gate

**Files:**

- Create: `docs/validation/m0-v3-query-driver.md`
- Modify: `tools/Invoke-M0Verification.ps1`

- [ ] **Step 23.1: Open an elevated x64 PowerShell and rebuild Release**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset release --target myrd-m0-poc
ctest --preset release -E '^real\.'
```

Expected: all non-real tests pass before driver access.

- [ ] **Step 23.2: Audit the Release import table before execution**

```powershell
.\tools\Audit-M0Dependencies.ps1 `
  -Executable .\out\build\msvc\src\Release\myrd-m0-poc.exe
```

Expected: exit 0, no AIM user-mode dependency.

- [ ] **Step 23.3: Capture an independent pre-query snapshot**

```powershell
$beforeServices = Get-Service phdskmnt,deviosvc |
  Select-Object Name,Status,StartType
$beforeDisks = Get-CimInstance Win32_DiskDrive |
  Select-Object Index,DeviceID,Size,PNPDeviceID
$beforeProcesses = Get-Process |
  Select-Object Id,ProcessName,Path
```

Do not stop a service and do not open a PhysicalDrive for write.

- [ ] **Step 23.4: Run query-driver exactly once**

```powershell
New-Item -ItemType Directory -Path .\reports\m0-v3 -Force | Out-Null
$reportPath = Join-Path (Resolve-Path .\reports\m0-v3) `
  ('query-' + [guid]::NewGuid().ToString('N') + '.json')
& .\out\build\msvc\src\Release\myrd-m0-poc.exe `
  query-driver --report $reportPath
if ($LASTEXITCODE -ne 0) { throw "query-driver failed with $LASTEXITCODE" }
.\tools\Verify-M0Json.ps1 -Path $reportPath
```

Expected: exit 0 and valid JSON.

- [ ] **Step 23.5: Assert query mode made no state change**

Parse the report and require:

```powershell
$r = Get-Content -LiteralPath $reportPath -Raw -Encoding utf8 | ConvertFrom-Json
if ($r.mode -ne 'query-driver' -or -not $r.success) { throw 'query mode failed' }
if ($null -ne $r.created_device) { throw 'query mode created a device' }
if ($null -ne $r.raw_round_trip) { throw 'query mode performed raw I/O' }
if ((@($r.baseline.aim_device_ids_before) -join ',') -ne
    (@($r.baseline.aim_device_ids_after) -join ',')) { throw 'AIM device list changed' }
if ((@($r.baseline.physical_drive_numbers_before) -join ',') -ne
    (@($r.baseline.physical_drive_numbers_after) -join ',')) { throw 'disk list changed' }
if (@($r.dependency_audit.forbidden_loaded_modules).Count -ne 0) { throw 'forbidden module' }
```

- [ ] **Step 23.6: Compare independent post-query observations**

Re-run service and disk snapshots. Services must have identical Status/StartType and disk sets must be identical to pre-query.

- [ ] **Step 23.7: Add V3 to the orchestrator**

V3 must run V2 Release, dependency audit, one query-driver command, JSON verification and no-state-change assertions. It must refuse a non-elevated shell before running query-driver.

- [ ] **Step 23.8: Record and commit V3 evidence**

The markdown report includes exact executable SHA-256, driver file version/hash/signature, adapter path, query version, before/after lists and report path.

```powershell
git add tools/Invoke-M0Verification.ps1 docs/validation/m0-v3-query-driver.md
git commit -m "docs: record read-only M0 V3 driver gate"
```

If V3 fails, stop. Do not create a real proxy device.

## Task 24: Implement the guarded real-driver E2E harness

**Files:**

- Create: `tests/e2e/run-m0-driver-only.ps1`
- Modify: `tools/Invoke-M0Verification.ps1`
- Modify: `README.md`

- [ ] **Step 24.1: Define harness parameters and serial lock**

```powershell
param(
  [ValidateRange(1,20)][int]$Iterations = 1,
  [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
  [string]$ReportRoot = '.\reports\m0-e2e',
  [ValidateSet('none','create_returned','device_confirmed',
               'physical_drive_confirmed','round_trip_verified')]
  [string]$FailurePoint = 'none'
)
```

Acquire a named system mutex for the whole script. If it is already owned, fail before any service change.

- [ ] **Step 24.2: Implement all preflight checks before `try` mutations**

Require:

```text
administrator token
phdskmnt service exists and is Running
deviosvc service exists
selected executable exists
Release executable passes import audit when Configuration is Release
no forbidden AIM process except deviosvc is running
FailurePoint is none for Release
ReportRoot resolves below the repository reports directory
```

- [ ] **Step 24.3: Capture complete `deviosvc` baseline**

Store `State`, `StartMode`, `ProcessId` and service path from CIM. The script must never call `Set-Service` or `sc.exe config`.

- [ ] **Step 24.4: Stop and restore only in `try/finally`**

Use this shape:

```powershell
$devioBefore = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
try {
  if ($devioBefore.State -eq 'Running') {
    Stop-Service -Name deviosvc -Force -ErrorAction Stop
    (Get-Service deviosvc).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
  }
  $during = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
  if ($during.State -ne 'Stopped') { throw 'deviosvc did not stop' }
  # query baseline and lifecycle iterations
}
finally {
  if ($devioBefore.State -eq 'Running' -and
      (Get-Service deviosvc).Status -ne 'Running') {
    Start-Service -Name deviosvc -ErrorAction Stop
    (Get-Service deviosvc).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
  }
  $devioAfter = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
  if ($devioAfter.StartMode -ne $devioBefore.StartMode) {
    throw 'deviosvc StartMode changed'
  }
}
```

- [ ] **Step 24.5: Run query baselines around all lifecycle iterations**

Run `query-driver` once after `deviosvc` stops and once before it is restored. Compare AIM device ids and PhysicalDrive numbers from both reports.

- [ ] **Step 24.6: Validate every normal run report**

Require:

```text
success true and exit_code 0
last_stage report_written
INFO, READ and WRITE counts each at least one
fatal protocol error count zero
written and read SHA-256 equal
exact remove attempted and succeeded
removed id equals created id
device disappeared
provider stopped
named objects released
AIM and PhysicalDrive lists match baseline
forbidden loaded modules and processes are empty
deviosvc state during run is Stopped
```

- [ ] **Step 24.7: Validate injected-failure reports differently**

For a non-`none` FailurePoint, require executable nonzero, `success` false, `primary_error.operation` identifies failure injection, and every cleanup assertion remains true. A cleanup failure causes the harness to fail even though the main operation was intentionally failed.

- [ ] **Step 24.8: Always emit a harness summary**

Write `summary.json` containing iteration count, passed count, failed report paths, baseline query paths, service restoration result and start-mode result. Parse it with `ConvertFrom-Json` before exiting.

- [ ] **Step 24.9: Add V4-V6 command mapping to the orchestrator**

```text
V4 runs V3 then one normal Debug E2E iteration.
V5 runs V4 then each Debug failure point once and another normal iteration.
V6 runs V5 then Release dependency audit and 20 normal Release iterations.
```

- [ ] **Step 24.10: Run script syntax validation without executing E2E**

```powershell
$null = [scriptblock]::Create(
  (Get-Content .\tests\e2e\run-m0-driver-only.ps1 -Raw -Encoding utf8))
.\tools\Invoke-M0Verification.ps1 -Tier V6 -ListCommands
```

Expected: syntax succeeds; listed commands show V0 through V6 in order.

- [ ] **Step 24.11: Commit the harness before first mutation**

```powershell
git add tests/e2e/run-m0-driver-only.ps1 tools/Invoke-M0Verification.ps1 README.md
git commit -m "test: add guarded M0 real-driver harness"
```

## Task 25: Pass one complete V4 lifecycle

**Files:**

- Create: `docs/validation/m0-v4-single-cycle.md`
- Modify only on a proven defect: the source and test for the earliest failing boundary

- [ ] **Step 25.1: Re-run V0-V3 fresh**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
.\tools\Invoke-M0Verification.ps1 -Tier V3 -Configuration Debug
```

Expected: `V3 PASS` in the current elevated shell.

- [ ] **Step 25.2: Run exactly one guarded Debug lifecycle**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\e2e\run-m0-driver-only.ps1 `
  -Iterations 1 -Configuration Debug `
  -ReportRoot .\reports\m0-v4
```

Expected: exit 0 and summary reports 1 attempted, 1 passed.

- [ ] **Step 25.3: Independently inspect the lifecycle report**

Open the JSON and manually compare:

```text
created DeviceId == removed DeviceId
created SCSI address == located candidate address
disk length == 67108864
raw offset == 8388608
raw length == 4096
written digest == read digest
before/after AIM ids equal
before/after PhysicalDrive numbers equal
```

- [ ] **Step 25.4: Check service and forbidden processes after harness exit**

```powershell
Get-Service phdskmnt,deviosvc | Select-Object Name,Status,StartType
Get-Process -ErrorAction SilentlyContinue |
  Where-Object ProcessName -Match 'aim_ll|RamDyn|AIMtk|aim_cli|MountImg|RamDiskUI'
```

Expected: service state/start type matches preflight and no forbidden process was introduced.

- [ ] **Step 25.5: If V4 fails, diagnose only the earliest boundary**

Use the failure decision tree later in this document. Preserve report and harness summary. Do not run a second lifecycle until the first failure has a test that reproduces its boundary.

- [ ] **Step 25.6: Record and commit V4 evidence**

Document exact commands, report paths, created/removed id, hashes, counters, cleanup and service restoration.

```powershell
git add docs/validation/m0-v4-single-cycle.md
git commit -m "docs: record M0 V4 single-cycle evidence"
```

## Task 26: Pass V5 post-create fault-injection cleanup

**Files:**

- Create: `docs/validation/m0-v5-fault-cleanup.md`
- Modify only on a proven defect: the smallest source/test pair that owns the boundary

- [ ] **Step 26.1: Run failure immediately after CREATE returns**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\e2e\run-m0-driver-only.ps1 `
  -Iterations 1 -Configuration Debug `
  -FailurePoint create_returned `
  -ReportRoot .\reports\m0-v5-create
```

Expected: intentional main failure, exact remove succeeds, all lists and named objects return to baseline, harness exits 0 because the expected-failure contract passed.

- [ ] **Step 26.2: Run failure after QUERY_DEVICE confirmation**

Repeat with `-FailurePoint device_confirmed`. Expected cleanup contract is identical.

- [ ] **Step 26.3: Run failure after PhysicalDrive identity confirmation**

Repeat with `-FailurePoint physical_drive_confirmed`. Expected: no raw write occurred, exact cleanup passed.

- [ ] **Step 26.4: Run failure after successful raw round trip**

Repeat with `-FailurePoint round_trip_verified`. Expected: hashes match and exact cleanup still passes.

- [ ] **Step 26.5: Run one normal lifecycle after all injected failures**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\e2e\run-m0-driver-only.ps1 `
  -Iterations 1 -Configuration Debug `
  -ReportRoot .\reports\m0-v5-postcheck
```

Expected: normal success proves failure tests left no latent state.

- [ ] **Step 26.6: Record and commit V5 evidence**

For every failure point, list primary error stage, created/removed id, cleanup fields and before/after sets.

```powershell
git add docs/validation/m0-v5-fault-cleanup.md
git commit -m "docs: record M0 V5 cleanup evidence"
```

## Task 27: Execute the 20-cycle Release V6 acceptance

**Files:**

- Create: `docs/validation/m0-v6-20-cycle.md`

- [ ] **Step 27.1: Build and test Release fresh**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset release
ctest --preset release
.\tools\Audit-M0Dependencies.ps1 `
  -Executable .\out\build\msvc\src\Release\myrd-m0-poc.exe
```

Expected: zero failures and no forbidden import.

- [ ] **Step 27.2: Record Release executable hash**

```powershell
Get-FileHash .\out\build\msvc\src\Release\myrd-m0-poc.exe -Algorithm SHA256
```

Use this exact hash in the V6 and final acceptance reports.

- [ ] **Step 27.3: Run 20 guarded iterations**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tests\e2e\run-m0-driver-only.ps1 `
  -Iterations 20 -Configuration Release `
  -ReportRoot .\reports\m0-v6
```

Expected: exit 0 and summary says 20 attempted, 20 passed.

- [ ] **Step 27.4: Aggregate independent invariants over all reports**

PowerShell must parse all 20 lifecycle JSON files and assert:

```text
20 distinct run ids
20 distinct session object names
20 success=true reports
20 exact remove successes
20 matching digest pairs
20 zero fatal-protocol counts
20 named-object release successes
20 AIM list matches
20 PhysicalDrive list matches
every removed id equals that report's created id
no forbidden loaded module or process in any report
```

- [ ] **Step 27.5: Verify final independent system state**

Run a new query-driver report, service snapshot and disk snapshot. Compare with the baseline query from the harness. Attempting to reopen each report's object names must return not found.

- [ ] **Step 27.6: Record V6 evidence**

Include executable hash, driver hash/signature/version, summary path, 20 report paths, all ids, counters, digests and cleanup aggregate.

- [ ] **Step 27.7: Commit V6 evidence**

```powershell
git add docs/validation/m0-v6-20-cycle.md
git commit -m "docs: record M0 V6 twenty-cycle evidence"
```

## Task 28: Write the formal M0 Go/No-go report

**Files:**

- Create: `docs/validation/m0-driver-only-acceptance.md`

- [ ] **Step 28.1: Re-read every MUST in the operational SPEC**

Create a requirement table:

```markdown
| SPEC section | Requirement | Evidence command/report | Result |
|---|---|---|---|
```

Every safety invariant, protocol gate, V0-V6 tier and GO condition gets one row.

- [ ] **Step 28.2: Separate facts from inference**

The report must distinguish:

```text
Observed: command/report value directly recorded.
Derived: comparison or aggregate computed from observed values.
Conclusion: GO, NO-GO, or BLOCKED.
```

- [ ] **Step 28.3: Apply the verdict mechanically**

Set `GO` only if all SPEC section 20 conditions are PASS. One FAIL produces `NO-GO`. An unmet external prerequisite before driver mutation produces `BLOCKED`, not GO.

- [ ] **Step 28.4: State Toolkit consequences precisely**

Even for GO, write: “Runtime Driver-only feasibility is proven on this machine. Toolkit removal and standalone driver installation remain unapproved until M5 disposable-VM validation.”

- [ ] **Step 28.5: Validate report completeness**

```powershell
$p = '.\docs\validation\m0-driver-only-acceptance.md'
$t = Get-Content -LiteralPath $p -Raw -Encoding utf8
foreach ($required in @('Verdict','V0','V1','V2','V3','V4','V5','V6',
                         'phdskmnt.sys','deviosvc','aimapi.dll','20/20',
                         'exact DeviceId','named objects')) {
  if (-not $t.Contains($required)) { throw "Acceptance report missing: $required" }
}
```

Expected: exit 0 with no missing acceptance field.

- [ ] **Step 28.6: Commit the verdict**

```powershell
git add docs/validation/m0-driver-only-acceptance.md
git commit -m "docs: publish M0 driver-only verdict"
```

## Task 29: Final repository and reproducibility audit

**Files:**

- Modify: `README.md`
- Modify: `docs/validation/m0-environment.md`

- [ ] **Step 29.1: Run every non-real verification fresh**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
.\tools\Invoke-M0Verification.ps1 -Tier V2 -Configuration Debug
.\tools\Invoke-M0Verification.ps1 -Tier V2 -Configuration Release
```

Expected: both end in `V2 PASS`.

- [ ] **Step 29.2: Re-run the highest authorized real tier**

- For an accepted GO, run `Invoke-M0Verification.ps1 -Tier V6 -Configuration Release`.
- For NO-GO, re-run only through the failing tier and preserve its evidence.
- For BLOCKED, run only tiers not requiring the missing prerequisite.

Do not claim a higher tier than the fresh command proves.

- [ ] **Step 29.3: Scan source and build metadata for forbidden dependencies**

```powershell
$forbidden = @(rg -n -i 'aimapi\.dll|aim_ll\.exe|RamDyn\.exe|AIMtk|aim_cli\.exe|MountImg\.exe|RamDiskUI\.exe' `
  src CMakeLists.txt cmake tests tools)
$allowed = @($forbidden | Where-Object { $_ -match 'Audit-M0Dependencies|forbidden|process' })
if ($forbidden.Count -ne $allowed.Count) { $forbidden; throw 'Unexpected AIM dependency reference' }
```

Expected: references exist only in auditing/forbidden-name logic, never link or launch logic.

- [ ] **Step 29.4: Verify repository hygiene**

```powershell
git diff --check
git status --short
git ls-files | Select-String -Pattern '^out/|^reports/'
```

Expected: no whitespace errors; only intended README/ledger edits before final commit; no build or report artifacts tracked.

- [ ] **Step 29.5: Update README with exact reproduction commands**

Document V0-V6 commands, admin requirement, service restoration, expected report locations, and the warning that M0 does not authorize Toolkit removal.

- [ ] **Step 29.6: Commit the reproducibility handoff**

```powershell
git add README.md docs/validation/m0-environment.md
git commit -m "docs: finalize reproducible M0 handoff"
```

## 5. SPEC-to-task coverage map

| Operational SPEC area | Implementing tasks | Proving tier |
|---|---|---|
| Runtime dependency boundary | 16, 19, 22, 23, 24 | V3-V6 |
| Safety invariants and exact removal | 13, 15, 20, 24, 26 | V1, V4, V5 |
| Protocol source and ABI | 3, 5, 12, 13 | V0, V1 |
| Fixed memory block device | 6 | V1 |
| Shared Memory Provider | 7, 8, 9, 10, 11, 22 | V1, V2, V4 |
| AIM adapter and IOCTL transport | 12, 13, 14 | V1, V3 |
| PhysicalDrive identity | 15, 21 | V1, V4 |
| Lifecycle and cleanup state machine | 20, 22, 24, 26 | V1, V4, V5 |
| Raw round trip | 21, 25 | V1, V4 |
| CLI and exit codes | 18, 22 | V1, V3 |
| JSON evidence schema | 16, 17, 22, 24 | V1, V3-V6 |
| Verification ladder | 19, 23, 24, 25, 26, 27 | V0-V6 |
| Go/No-go decision | 27, 28, 29 | V6 |

Every normative area has both an implementation task and a proving tier. A passing lower-level test cannot substitute for the listed real-driver tier.

## 6. Failure diagnosis decision tree

Use only after the corresponding test or real command fails.

### Protocol source cannot be fetched

```text
Classify BLOCKED_PROTOCOL_SOURCE.
Record Git command and network/proxy error.
Do not edit protocol values.
Do not proceed beyond Task 3.
```

### ABI/static assertions fail

```text
Compare local packing, field type and offset with audited source.
Fix the protocol header and its exact test together.
Do not change driver transport or retry logic.
```

### Local SHM integration fails

```text
Inspect object creation error, auto-reset behavior, wait result and server state.
Reproduce in integration.shm without driver.
Do not run query-driver or create a real device.
```

### QUERY_VERSION fails

```text
Check elevation, adapter service property, open error, IOCTL bytes returned,
SRB signature/header/control code/ReturnCode and payload length in that order.
Do not change Shared Memory code.
```

### CREATE fails before returning an id

```text
No owned id exists, so do not call REMOVE.
Compare create payload bytes with the audited contract.
Verify provider objects remain alive and native path is exact.
Record adapter list after failure.
```

### CREATE response returns an id but QUERY_DEVICE fails

```text
Store the returned id immediately.
Attempt REMOVE of that exact id during cleanup.
Compare query response id/path/flags/geometry with the request.
Do not locate or write a PhysicalDrive.
```

### PhysicalDrive cannot be located

```text
Preserve candidate SCSI addresses, lengths and before/after membership.
Remove exact owned id.
Do not select by size, order, drive letter or newest timestamp.
```

### Raw write/read fails

```text
Record identity checks, offset, requested/transferred byte counts,
FlushFileBuffers result and both hashes.
Remove exact id and verify all cleanup.
Do not retry a write until the boundary has a reproducing test or clear external cause.
```

### REMOVE or cleanup fails

```text
Keep the original primary error.
Append cleanup errors.
Query the exact id and adapter list.
Do not send an all-devices removal.
Mark NO-GO and retain all report files.
```

## 7. Plan completion criteria

This plan reaches exactly one terminal state:

- **GO:** Tasks 0-29 completed, fresh V6 is 20/20, and the formal report proves all operational SPEC conditions.
- **NO-GO:** Real-driver prerequisites were present, but a mandatory behavior failed; exact evidence and cleanup state are committed.
- **BLOCKED:** A required external condition was missing before a conclusive real-driver result; the last passing tier and blocker are committed.

No terminal state in M0 authorizes Toolkit uninstallation on the current physical machine.
