# RamDisk Daily-Use Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the existing single-process AIM VM prototype into a safe, bounded-memory, auto-starting, single-profile Windows service with durable snapshots and a pure-client CLI.

**Architecture:** A `ramdisk_core` static library owns VHD, snapshot, AIM, identity, configuration, session, IPC, and logging logic. `ramdisk-service.exe` is the only production process allowed to manipulate AIM or disks; `ramdisk-cli.exe` sends authenticated local pipe requests. The existing proxy/watchdog path remains experimental and is excluded from production targets.

**Tech Stack:** C++11, MinGW-w64 GCC 13.1+, CMake 3.16+, CTest, Win32/SCM/Named Pipes, AIM Toolkit `aim_ll.exe`, vendored nlohmann/json 3.11.3.

**Authoritative spec:** `docs/superpowers/specs/2026-07-31-ramdisk-daily-use-service-design.md`  
**Locked spec SHA-256:** `AE9DC2D06832283CB6092C9D1B83B302A74F7A7B3A5F1DE93D64B665DE8D04B9`

---

## 1. Execution protocol

1. Work in an isolated Git worktree. Do not execute this plan directly in a dirty `master` checkout.
2. Before each task, recompute the spec hash. Stop if it differs from the locked hash until the plan is reviewed.
3. Use TDD for every behavior change: add a failing test, run it, implement the smallest behavior, rerun the focused test, then run the relevant suite.
4. Do not run a state-changing AIM command, install a service, claim a drive letter, or reboot until the task explicitly reaches the G5 acceptance gate and the user approves that exact action.
5. Never use Z:, `C:\PR-Image-Z.vdf`, an existing production image, or an already occupied drive letter in tests.
6. All GitHub retrievals use `http://127.0.0.1:7890` and record provenance. Do not add an unpinned dependency.
7. At the end of every task:
   - re-read the spec requirements listed in that task;
   - run `git diff --check`;
   - record focused test output in the commit message body or implementation log;
   - confirm no unrelated user files changed;
   - commit only that task's files.
8. Do not mark a milestone gate complete from compilation alone.

## 2. Planned file structure

```text
build_my_ramdisk/
├── CMakeLists.txt
├── THIRD_PARTY.md
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── service_main.cpp
│   ├── operation_context.h
│   ├── profile_config.h/.cpp
│   ├── control_protocol.h/.cpp
│   ├── service_host.h/.cpp
│   ├── ramdisk_session.h/.cpp
│   ├── runtime_store.h/.cpp
│   ├── snapshot_engine.h/.cpp
│   ├── save_coordinator.h/.cpp
│   ├── device_identity.h/.cpp
│   ├── volume_manager.h/.cpp
│   ├── json_log.h/.cpp
│   ├── aim_bridge.h/.cpp
│   ├── vhd_engine.h/.cpp
│   └── cli_parser.h/.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── test_support.h
│   ├── test_main.cpp
│   ├── vhd_endian_tests.cpp
│   ├── vhd_engine_tests.cpp
│   ├── legacy_conversion_tests.cpp
│   ├── profile_config_tests.cpp
│   ├── control_protocol_tests.cpp
│   ├── aim_bridge_tests.cpp
│   ├── device_identity_tests.cpp
│   ├── volume_manager_tests.cpp
│   ├── snapshot_engine_tests.cpp
│   ├── runtime_store_tests.cpp
│   ├── ramdisk_session_tests.cpp
│   ├── save_coordinator_tests.cpp
│   ├── json_log_tests.cpp
│   ├── service_host_tests.cpp
│   ├── ipc_authorization_tests.cpp
│   ├── cli_tests.cpp
│   └── integration_lifecycle_tests.cpp
├── scripts/
│   └── Run-AimE2E.ps1
└── vendor/
    └── nlohmann/json.hpp
```

## 3. Milestone ordering

```text
G0 Test foundation
  → G1 Standard/bounded VHD
    → G2 Exact AIM/volume identity
      → G3 Service lifecycle
        → G4 Snapshot durability
          → G5 User-approved real AIM acceptance
```

---

### Task 1: Lock the baseline and create the CTest harness

**Requirements:** `BLD-001`, `BLD-002`, `BLD-003`, `BLD-004`, `TST-001`–`TST-011`

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_support.h`
- Create: `tests/test_main.cpp`

- [ ] **Step 1: Verify the authoritative spec and current baseline**

Run:

```powershell
Get-FileHash docs\superpowers\specs\2026-07-31-ramdisk-daily-use-service-design.md -Algorithm SHA256
git status --short
cmake --build build
```

Expected: hash equals the locked value; only intentional documentation changes exist; current CLI still builds.

- [ ] **Step 2: Add a dependency-free test registry**

Create `tests/test_support.h` with a registry, `TEST(name)` macro, `EXPECT_TRUE`, `EXPECT_EQ`, and exception-safe result reporting. Use this public shape:

```cpp
#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase { std::string name; std::function<void()> run; };
std::vector<TestCase>& TestRegistry();

struct TestRegistration {
    TestRegistration(const char* name, std::function<void()> run);
};

#define TEST(name) \
    static void name(); \
    static TestRegistration name##_registration(#name, name); \
    static void name()

#define EXPECT_TRUE(value) do { if (!(value)) throw std::runtime_error("EXPECT_TRUE failed: " #value); } while (0)
#define EXPECT_EQ(actual, expected) do { if (!((actual) == (expected))) throw std::runtime_error("EXPECT_EQ failed: " #actual); } while (0)
```

Create `tests/test_main.cpp` that supports `--filter <substring>`, runs matching tests, prints one PASS/FAIL line per test, and exits nonzero on any failure.

- [ ] **Step 3: Add the first harness self-test**

Add a `HarnessRunsRegisteredTest` test in `test_main.cpp` or a small companion source and verify it is discovered.

- [ ] **Step 4: Refactor CMake targets without changing runtime behavior**

In `src/CMakeLists.txt`:

- create `ramdisk_core` from reusable current sources;
- link current `ramdisk-cli` to `ramdisk_core`;
- do not yet include `storage_server.cpp`, `watchdog.cpp`, `daemon.cpp`, `dirty_bitmap.cpp`, or `sector_cache.cpp` in `ramdisk_core`;
- keep an optional `ramdisk_experimental` static library for those sources, disabled by default;
- add a placeholder `ramdisk-service` target only after `service_main.cpp` exists in Task 19.

In root `CMakeLists.txt`, call `enable_testing()` and `add_subdirectory(tests)`.

- [ ] **Step 5: Configure, build, and enumerate tests**

Run:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
```

Expected: CLI builds; at least one test is listed and passes; experimental sources are absent from the CLI link line.

- [ ] **Step 6: Alignment checkpoint and commit**

Run `git diff --check`, reread `BLD-001`–`BLD-004`, then commit:

```powershell
git add CMakeLists.txt src/CMakeLists.txt tests
git commit -m "test: establish CTest harness and core target"
```

---

### Task 2: Vendor and verify the JSON dependency

**Requirements:** `BLD-005`, `CFG-001`–`CFG-003`, `IPC-002`, `IPC-003`

**Files:**
- Create: `vendor/nlohmann/json.hpp`
- Create: `THIRD_PARTY.md`
- Modify: `src/CMakeLists.txt`
- Create: `tests/json_dependency_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Download through the required proxy**

Run from PowerShell:

```powershell
New-Item -ItemType Directory -Force vendor\nlohmann | Out-Null
Invoke-WebRequest `
  -Proxy http://127.0.0.1:7890 `
  -Uri https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp `
  -OutFile vendor\nlohmann\json.hpp
Get-FileHash vendor\nlohmann\json.hpp -Algorithm SHA256
```

Record the exact resulting SHA-256, upstream URL, version `3.11.3`, and MIT license in `THIRD_PARTY.md`.

- [ ] **Step 2: Add a failing compile/round-trip test**

Create `JsonDependencyRoundTripsUtf8` that parses and emits a JSON object containing Chinese text, a 64-bit `sizeBytes`, a GUID string, and an unknown field. The test must compare semantic values, not serialized key order.

- [ ] **Step 3: Wire the include path and pass the focused test**

Add `${PROJECT_SOURCE_DIR}/vendor` to `ramdisk_core` and tests. Run:

```powershell
cmake --build build
build\tests\ramdisk-tests.exe --filter JsonDependency
```

Expected: the UTF-8 and 64-bit values round-trip.

- [ ] **Step 4: Verify provenance and commit**

Run:

```powershell
Get-FileHash vendor\nlohmann\json.hpp -Algorithm SHA256
git diff --check
git add vendor\nlohmann\json.hpp THIRD_PARTY.md src/CMakeLists.txt tests
git commit -m "build: vendor pinned nlohmann json"
```

---

### Task 3: Define shared operation, result, and cancellation contracts

**Requirements:** `IO-003`, `PERF-002`, `PERF-003`, `IPC-003`, `OBS-002`

**Files:**
- Create: `src/operation_context.h`
- Create: `tests/operation_context_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing contract tests**

Test that cancellation reflects an atomic flag, progress is monotonic, operation IDs are nonzero, and `OperationResult::Failure` retains a stable code/message.

- [ ] **Step 2: Create the shared header**

Use these exact public contracts:

```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <windows.h>

struct CancellationToken {
    const std::atomic<bool>* requested = nullptr;
    bool IsCancellationRequested() const {
        return requested != nullptr && requested->load();
    }
};

struct ProgressUpdate {
    GUID operationId{};
    std::string stage;
    uint64_t completed = 0;
    uint64_t total = 0;
};

using ProgressCallback = std::function<void(const ProgressUpdate&)>;

struct OperationResult {
    bool success = false;
    GUID operationId{};
    std::string code;
    std::string message;
};
```

Add helper functions for creating GUIDs and success/failure values in the same header or a matching `.cpp`; do not duplicate them in components.

- [ ] **Step 3: Run tests and commit**

Run `build\tests\ramdisk-tests.exe --filter OperationContext`, then the complete test executable and `git diff --check`. Commit:

```powershell
git add src/operation_context.h src/CMakeLists.txt tests
git commit -m "feat: define shared operation contracts"
```

---

### Task 4: Implement profile schema, validation, and atomic storage

**Requirements:** `CFG-001`–`CFG-007`, `SAFE-002`, `SAFE-004`

**Files:**
- Create: `src/profile_config.h`
- Create: `src/profile_config.cpp`
- Create: `tests/profile_config_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Define failing schema tests**

Add named tests for:

```text
ProfileConfig_RoundTripsSchemaV1
ProfileConfig_PreservesUnknownFields
ProfileConfig_RejectsUnknownSchema
ProfileConfig_RejectsRelativeUncAndNonFixedPaths
ProfileConfig_RejectsReparseTraversal
ProfileConfig_RejectsProtectedVdfAndZDefaults
ProfileConfig_RejectsOccupiedLetter
ProfileConfig_ValidatesSizeAndIntervalBoundaries
ProfileStore_AtomicReplaceRetainsOldOnFailure
```

Use a temporary NTFS directory and injected interfaces for drive type, path resolution, reparse checks, and atomic replacement so tests do not inspect production resources.

- [ ] **Step 2: Add exact data contracts**

Create:

```cpp
struct ProfileConfig {
    int schemaVersion = 1;
    std::string profileId;
    GUID profileGuid{};
    std::wstring imagePath;
    uint64_t sizeBytes = 0;
    wchar_t driveLetter = L'\0';
    std::wstring volumeLabel;
    uint32_t saveIntervalSeconds = 0;
    bool autoStart = false;
    bool saveOnShutdown = true;
    bool keepPreviousImage = true;
    nlohmann::json unknownFields;
};

struct ValidationError { std::string code; std::string field; std::string message; };
struct ProfileValidationResult { bool valid; std::vector<ValidationError> errors; };

class ProfileStore {
public:
    explicit ProfileStore(std::wstring path);
    OperationResult Save(const ProfileConfig&);
    OperationResult Load(ProfileConfig& out) const;
};
```

- [ ] **Step 3: Implement pure validation first**

Implement schema, profile ID, GUID, size, drive-letter, label, interval, and image-path syntax checks without touching the filesystem. Preserve unknown JSON members in `unknownFields`.

- [ ] **Step 4: Implement injected machine/path validation**

Add `IPathEnvironment` with `GetDriveTypeW`, final-path resolution, filesystem name, reparse inspection, and logical-drive mask methods. The production implementation uses Unicode Win32 APIs; tests use fakes.

- [ ] **Step 5: Implement atomic store**

Serialize to `profile.json.tmp`, flush via `FlushFileBuffers`, then call `MoveFileExW` with both required flags. On failure, delete only the temp file and leave the previous profile untouched.

- [ ] **Step 6: Verify and commit**

Run:

```powershell
cmake --build build
build\tests\ramdisk-tests.exe --filter Profile
ctest --test-dir build --output-on-failure
git diff --check
```

Commit `feat: add validated atomic profile store`.

---

### Task 5: Add VHD endian primitives and golden serialization tests

**Requirements:** `VHD-001`, `VHD-002`, `VHD-005`, `TST-001`

**Files:**
- Create: `src/vhd_endian.h`
- Create: `tests/vhd_endian_tests.cpp`
- Modify: `src/vhd_engine.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write golden-byte tests before changing the engine**

Cover 16/32/64-bit host↔big-endian conversion, serialized disk type `3`, virtual size, dynamic header table offset, block size, BAT entry, and checksum byte order. Include expected byte arrays directly in the tests.

- [ ] **Step 2: Implement explicit endian helpers**

Create functions with no packed-struct reinterpretation:

```cpp
uint16_t ReadBe16(const uint8_t* p);
uint32_t ReadBe32(const uint8_t* p);
uint64_t ReadBe64(const uint8_t* p);
void WriteBe16(uint8_t* p, uint16_t value);
void WriteBe32(uint8_t* p, uint32_t value);
void WriteBe64(uint8_t* p, uint64_t value);
uint32_t OnesComplementChecksum(const uint8_t* bytes, size_t length, size_t checksumOffset);
```

- [ ] **Step 3: Replace direct on-disk struct writes**

Introduce 512-byte footer and 1024-byte dynamic-header serializers/parsers. Keep logical in-memory structs, but never `WriteFile` or `ReadFile` those structs directly.

- [ ] **Step 4: Run focused red/green tests**

Run `build\tests\ramdisk-tests.exe --filter VhdEndian` and ensure the pre-change engine fixture fails before the serializer change and passes afterward.

- [ ] **Step 5: Commit**

Commit `fix: serialize VHD metadata in big endian` after full CTest and alignment review.

---

### Task 6: Rewrite VhdEngine for validation and bounded block I/O

**Requirements:** `VHD-003`, `VHD-004`, `VHD-006`, `IO-001`, `IO-003`, `PERF-001`, `TST-002`, `TST-003`

**Files:**
- Modify: `src/vhd_engine.h`
- Modify: `src/vhd_engine.cpp`
- Create: `tests/vhd_engine_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add failing boundary/corruption tests**

Implement the exact `TST-002` cases. Add fake file-I/O instrumentation that records maximum request length and outstanding cached blocks.

- [ ] **Step 2: Replace the public interface**

Use the `VhdStatus`, `VhdValidationResult`, and `VhdEngine` surface from the spec. Remove `ReadAll` and `WriteAll` from production callers; they may remain only as test helpers with a hard size limit of 64 MiB.

- [ ] **Step 3: Implement 64-bit chunked Win32 I/O**

All calls loop on partial transfers, use `SetFilePointerEx`, split requests at 64 MiB, and treat zero-byte progress before completion as `IoError`.

- [ ] **Step 4: Bound block caching**

Replace `blockCache.resize(maxTableEntries)` data retention with at most two 2 MiB block buffers. A dirty block is serialized, flushed to its allocated VHD block, its BAT entry is written in big-endian order, and the buffer is reusable before the next block.

- [ ] **Step 5: Implement structural validation**

Validate both footers, dynamic header, checksums, table range, BAT entry range/alignment, duplicate mapped blocks, virtual size, and file length. Return the first stable code plus diagnostic message.

- [ ] **Step 6: Add a simulated 32 GiB sparse test**

Create a sparse 32 GiB logical fake device that synthesizes bytes without allocating 32 GiB. Stream across it and assert max request ≤64 MiB, cached VHD blocks ≤2, and process-side tracked buffer ≤128 MiB.

- [ ] **Step 7: Verify and commit**

Run focused VHD tests, all CTest tests, and `git diff --check`. Commit `feat: make VHD engine validated and bounded`.

---

### Task 7: Implement non-destructive legacy VHD conversion

**Requirements:** `VHD-007`, `VHD-008`, `NG-007`, `TST-002`

**Files:**
- Create: `src/legacy_converter.h`
- Create: `src/legacy_converter.cpp`
- Create: `tests/legacy_conversion_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create legacy fixtures and failing tests**

Construct small images using the old host-endian layout. Test strict detection, rejection of ambiguous files, successful streamed conversion, source SHA-256 preservation, existing-output refusal, and cleanup after injected write failure.

- [ ] **Step 2: Implement strict detection**

Require all four conditions from `VHD-007`. A standard image whose creator is `RDSK` must remain standard, not legacy.

- [ ] **Step 3: Implement streamed conversion**

Open source read-only, create a new standard destination, read logical sectors in 32 MiB chunks through a legacy reader, write through the standard engine, flush/validate destination, and never rename over source.

- [ ] **Step 4: Verify source immutability and commit**

Hash source before and after success and each fault-injection point. Commit `feat: add non-destructive legacy VHD conversion`.

---

### Task 8: Define the versioned control protocol and framing

**Requirements:** `IPC-001`–`IPC-003`, `IPC-005`, `IPC-006`, `CLI-003`, `TST-008`

**Files:**
- Create: `src/control_protocol.h`
- Create: `src/control_protocol.cpp`
- Create: `tests/control_protocol_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing framing/schema tests**

Test split reads, multiple messages, zero length, payload >1 MiB, invalid UTF-8/JSON, schema mismatch, missing request ID, stable response fields, duplicate replay, and duplicate conflict.

- [ ] **Step 2: Define exact request/response records**

```cpp
enum class ControlCommand {
    Configure, Initialize, Start, Save, Stop, Status, Doctor,
    ImageValidate, ImageConvertLegacy,
    ServiceInstall, ServiceUninstall, ServiceStart, ServiceStop, ServiceStatus
};

struct ControlRequest {
    int schemaVersion = 1;
    GUID requestId{};
    ControlCommand command{};
    nlohmann::json arguments;
};

struct ControlResponse {
    GUID requestId{};
    bool success = false;
    std::string code;
    std::string message;
    GUID operationId{};
    nlohmann::json data;
};
```

- [ ] **Step 3: Implement 4-byte framing and size checks**

The prefix is unsigned little-endian payload bytes. Reads must assemble partial headers/payloads; writes must loop until complete. Reject length zero and >1,048,576 before allocating.

- [ ] **Step 4: Implement request deduplication cache**

Store canonical request SHA-256 plus response for 256 entries/10 minutes. Identical replay returns cached response; different content with same ID returns `DuplicateRequestConflict`.

- [ ] **Step 5: Run tests and commit**

Commit `feat: define versioned local control protocol` after focused and full tests.

---

### Task 9: Refactor AimBridge around injectable process execution

**Requirements:** `DEV-001`, `DEV-002`, `DEV-007`, `SAFE-002`, `TST-005`

**Files:**
- Modify: `src/aim_bridge.h`
- Modify: `src/aim_bridge.cpp`
- Create: `src/process_runner.h`
- Create: `src/process_runner.cpp`
- Create: `tests/aim_bridge_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add failing stdout/timeout/quoting tests**

Use a fake `IProcessRunner` to cover six-digit AIM device output, localized extra text, nonzero exit, timeout, cancellation, output limit, Unicode path, embedded spaces, and malformed device output.

- [ ] **Step 2: Introduce process contracts**

```cpp
struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    uint32_t exitCode = 0;
    std::string stdoutUtf8;
    std::string stderrUtf8;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() {}
    virtual ProcessResult Run(const std::wstring& executable,
                              const std::vector<std::wstring>& arguments,
                              uint32_t timeoutMilliseconds,
                              const CancellationToken&) = 0;
};
```

- [ ] **Step 3: Implement Unicode Win32 runner**

Use `CreateProcessW`, Windows argument quoting, separate stdout/stderr pipes, concurrent drain to prevent deadlock, 4 MiB per-stream limit, cancellation, and no `TerminateProcess` except for the explicitly timed-out child process.

- [ ] **Step 4: Return structured AIM data**

Change mount to return `AimMountResult`; parse the exact new AIM device number and then fetch a device list. Eliminate every production use of default device 0.

- [ ] **Step 5: Verify and commit**

Run `--filter AimBridge`, search production code for `g_deviceNumber` and default `device = 0`, then commit `refactor: return exact AIM mount identity`.

---

### Task 10: Implement exact disk, partition, and volume identity

**Requirements:** `DEV-003`–`DEV-006`, `SAFE-001`, `TST-005`

**Files:**
- Create: `src/device_identity.h`
- Create: `src/device_identity.cpp`
- Create: `tests/device_identity_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the complete reconciliation matrix**

Tests cover: exactly one new device; zero/two new devices; stdout/list mismatch; disk number mismatch; volume extent mismatch; volume serial mismatch; MBR/GPT mismatch; drive-letter mismatch; size mismatch; and exact restart adoption.

- [ ] **Step 2: Define identity/provider interfaces**

Use `MountIdentity` from the spec plus an injected `IDeviceInventory` that returns AIM devices, Windows disks, layouts, volume GUIDs, extents, serials, and mount paths.

- [ ] **Step 3: Implement before/after resolution**

The resolver first matches the AIM-reported device number, then maps its physical drive to a Windows disk, then maps extents to one volume. Capacity is a final consistency check only.

- [ ] **Step 4: Implement live identity verification**

Return `Exact`, `Missing`, or `Mismatch` with field-level diagnostics. No mismatch path calls unmount or mount-point removal.

- [ ] **Step 5: Remove unsafe matching**

Delete/retire capacity-based selection in `drive_letter.cpp`. Search for equal-size disk selection and device-list index selection; production results must be empty.

- [ ] **Step 6: Verify and commit**

Commit `feat: prove exact AIM disk ownership` after matrix tests and search evidence.

---

### Task 11: Implement verified volume management and provisioning

**Requirements:** `PRV-001`–`PRV-006`, `CFG-005`, `DEV-004`, `SAFE-003`

**Files:**
- Create: `src/volume_manager.h`
- Create: `src/volume_manager.cpp`
- Create: `tests/volume_manager_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write destructive-precondition tests**

Test refusal for an existing image, occupied letter, pre-existing disk, ambiguous new disk, identity mismatch, nonempty partition table, DiskPart timeout/error, and failed post-verification.

- [ ] **Step 2: Define the injected interface**

```cpp
class IVolumeManager {
public:
    virtual ~IVolumeManager() {}
    virtual OperationResult InitializeNtfs(const MountIdentity&, const ProfileConfig&,
                                           const CancellationToken&) = 0;
    virtual OperationResult AssignDriveLetter(const MountIdentity&, wchar_t letter) = 0;
    virtual OperationResult SetLabel(const MountIdentity&, const std::wstring& label) = 0;
    virtual OperationResult FlushTargetVolume(const MountIdentity&) = 0;
};
```

- [ ] **Step 3: Implement exact DiskPart provisioning**

Generate a unique script selecting `windowsDiskNumber`, then `clean`, `convert mbr`, `create partition primary align=1024`, `format fs=ntfs quick label="..."`, and `assign letter=X`. Execute with the process runner and 300-second timeout.

- [ ] **Step 4: Post-verify independently**

Re-enumerate layout, volume extent, filesystem `NTFS`, label, and mount path. Report success only if all match the same identity.

- [ ] **Step 5: Verify and commit**

Run fake tests only; do not execute real DiskPart. Commit `feat: add verified NTFS provisioning`.

---

### Task 12: Implement sparse VHD expansion to raw staging

**Requirements:** `IO-001`–`IO-004`, `PERF-001`–`PERF-003`

**Files:**
- Create: `src/snapshot_engine.h`
- Create: `src/snapshot_engine.cpp`
- Create: `tests/snapshot_engine_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add failing expand tests**

Test deterministic data across >4 GiB logical offsets, sparse zero regions, partial writes, cancellation, progress cadence, raw filename uniqueness, cleanup retry, and max buffer tracking.

- [ ] **Step 2: Define SnapshotEngine dependencies**

Inject `VhdEngine`, file I/O, clock, GUID provider, progress callback, and cleanup registry. Do not directly use globals.

- [ ] **Step 3: Implement sparse raw creation**

Create under `%ProgramData%\MyRamDisk\staging\<profileGuid>-<operationGuid>.raw`, call `FSCTL_SET_SPARSE`, set logical EOF to profile size, and write nonzero VHD chunks through one 32 MiB buffer.

- [ ] **Step 4: Implement cancellation/progress**

Check cancellation between chunks and publish progress at least every 5 seconds and at completion. Cancellation closes and deletes the staging file when possible and registers it for safe cleanup otherwise.

- [ ] **Step 5: Verify memory bounds and commit**

Run simulated 32 GiB tests and assert one 32 MiB buffer plus VHD's two-block cache. Commit `feat: stream VHD into sparse AIM staging image`.

