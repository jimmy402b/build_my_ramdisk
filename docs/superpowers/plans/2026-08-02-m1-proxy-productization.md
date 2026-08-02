# M1 Shared Memory Proxy Productization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Productize the M0 shared-memory proxy into a safe, diagnosable M1 library that supports INFO, READ, WRITE, UNMAP, ZERO, CLOSE, cancellation, timeout, exact cleanup, and a real `phdskmnt.sys` end-to-end gate without AIM user-mode components.

**Architecture:** Keep the proven M0 driver control path as the kernel-facing lifecycle layer. Evolve the common block-device contract and protocol decoder, add a policy-driven named-object channel and a new `ShmProxySession`, then compose them in an M1-only verification executable. M1 deliberately uses the fixed-size `MemoryBlockDevice`; dynamic allocation, backing files, dirty snapshots, persistence, services, and product CLI belong to M2-M4.

**Tech Stack:** Windows x64, MSVC C++20, CMake 3.25+, Ninja Multi-Config, Win32 named mappings/events/DACLs, `phdskmnt.sys` AIM SHM protocol, PowerShell acceptance harness, the repository's lightweight C++ test framework.

---

## 1. Authority, baseline, and scope

The authoritative design is `docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md`. This plan implements only its M1 scope and gates M1-A, M1-B, and M1-C.

The implementation baseline is the accepted M0 closure:

- M0 merge: `a03a49e`
- named-object release fix: `bc9b852`
- final closure: `0e2b3b3`
- accepted Release V6 aggregate: `reports/m0-v6/summary-20260802-005337-3234647-aggregate.json`

M0 is a frozen safety baseline. Do not delete or weaken `myrd-m0-poc`, `tests/e2e/run-m0-driver-only.ps1`, `tools/Invoke-M0Verification.ps1`, or their report validators while executing M1.

M1 includes:

- exact wire ABI for INFO, READ, WRITE, UNMAP, ZERO, and CLOSE;
- checked request validation and stable Win32 `errorno` mapping;
- `IBlockDevice` translation with cancellation and deadline propagation;
- cryptographically random session IDs, policy-built kernel-visible names, explicit DACLs, and duplicate-name rejection;
- session states, ready barrier, request/cancel waits, response ordering, fatal behavior, statistics, and cleanup;
- deterministic protocol fuzzing, local SHM integration, initialization failure injection, and real driver E2E;
- structured evidence proving no AIM user-mode dependency and exact device/object cleanup.

M1 excludes:

- `VirtualAlloc` reserve/commit/decommit and 64 KiB dynamic blocks;
- eviction, backing stores, dirty versions, and snapshot APIs;
- VHD/VHDX, journal, checkpoint, timed write-back, and power-loss durability;
- Windows Service, profile storage, startup recovery, product CLI, and GUI;
- M2's 64 GiB NTFS and memory-efficiency/performance gates.

## 2. Mandatory execution protocol

The execution ledger is the repository-root `M1_TASKLIST.md`. Agents update progress, evidence, implementation commits, and alignment-review commits there; they do not use the completed M0 `tasklist.md` and do not mark this PLAN's checkboxes directly.

Every agent executing a task must do the following in order:

1. Read this plan, the M1 sections of the design specification, and the previous task's commit.
2. Run `git status --short`; preserve the user's existing untracked diagnostics and unrelated changes.
3. Confirm all prerequisite task tests pass before editing.
4. Follow red-green-refactor: add the named failing test, observe the specified failure, add only the implementation required by the task, and rerun the focused test.
5. Run `git diff --check` and the task's regression command.
6. Compare the diff to the M1 include/exclude lists above. Remove M2-M4 work and unrelated refactors.
7. Commit only the task's files with the exact commit intent given in the task.
8. Re-read this plan before moving to the next task; do not skip a failed gate.

Execution should occur in an isolated worktree created with `using-git-worktrees`. The dirty diagnostics in the current workspace are not M1 inputs and must not be copied into commits.

## 3. Locked file map

### Existing files to modify

- `src/CMakeLists.txt`: add M1 protocol/session/support targets and the M1 verification executable.
- `tests/CMakeLists.txt`: register focused M1 unit, fuzz, integration, and harness tests.
- `src/common/error.h`: add stable M1 internal error identifiers and a separate native Win32 field without breaking M0 aggregate initialization.
- `src/storage/block_device.h`: publish `OperationContext`, `DataRange`, `DeviceFeatures`, and READ/WRITE/UNMAP/ZERO operations.
- `src/storage/memory_block_device.h`, `src/storage/memory_block_device.cpp`: remain a fixed-size test/M1 backing device and implement the new contract.
- `src/proxy/imdproxy_protocol.h`: add audited flags, UNMAP/ZERO ABI, and complete layout assertions.
- `src/proxy/object_names.h`, `src/proxy/object_names.cpp`: add namespace policy and Cancel Event names while preserving `ObjectNames::from_session()` for M0.
- `src/proxy/request_processor.h`, `src/proxy/request_processor.cpp`: implement all six M1 operations, validation, cancellation, timeout, diagnostics, and stable responses.
- `src/proxy/shared_memory_channel.h`, `src/proxy/shared_memory_channel.cpp`: add Cancel Event ownership, explicit stage hooks, zero-initialization, and safe accessors.

### New production/support files

- `src/common/operation_context.h`: cancellation/deadline value object and check helper.
- `src/proxy/proxy_error.h`, `src/proxy/proxy_error.cpp`: internal-to-Win32 `errorno` mapping and error classification.
- `src/proxy/range_normalizer.h`, `src/proxy/range_normalizer.cpp`: decode, validate, sort, and merge UNMAP/ZERO ranges.
- `src/proxy/shm_proxy_session.h`, `src/proxy/shm_proxy_session.cpp`: M1 session state machine and worker lifecycle.
- `src/m1/m1_provider.h`, `src/m1/m1_provider.cpp`: M1 composition root using `MemoryBlockDevice`.
- `src/m1/m1_raw_protocol_io.h`, `src/m1/m1_raw_protocol_io.cpp`: real READ/WRITE/ZERO/UNMAP verification through `PhysicalDrive`.
- `src/m1/m1_cli.h`, `src/m1/m1_cli.cpp`: parse the narrow M1 verification command line.
- `src/m1/m1_lifecycle_runner.h`, `src/m1/m1_lifecycle_runner.cpp`: injectable exact-device lifecycle orchestration and cleanup.
- `src/m1/m1_report.h`, `src/m1/m1_report.cpp`: structured M1 evidence.
- `src/m1/main.cpp`: runtime composition and M1-C verification entry point; it contains no lifecycle state machine.
- `tools/Invoke-M1Verification.ps1`: tiered V0-V6 orchestrator.
- `tools/Verify-M1Json.ps1`: strict report schema/semantic validator.
- `tests/e2e/run-m1-proxy.ps1`: elevated single/loop harness with baseline and service restoration.
- `tests/e2e/test-m1-harness-preflight.ps1`: non-destructive harness safety tests.
- `docs/validation/m1-proxy-acceptance.md`: final evidence and GO/NO-GO record.

### New test files

- `tests/unit/operation_context_tests.cpp`
- `tests/unit/block_device_contract_tests.cpp`
- `tests/unit/m1_protocol_tests.cpp`
- `tests/unit/proxy_error_tests.cpp`
- `tests/unit/range_normalizer_tests.cpp`
- `tests/support/fake_block_device.h`
- `tests/unit/m1_request_processor_tests.cpp`
- `tests/unit/m1_protocol_fuzz_tests.cpp`
- `tests/unit/namespace_policy_tests.cpp`
- `tests/integration/m1_shm_session_tests.cpp`
- `tests/unit/m1_provider_tests.cpp`
- `tests/unit/m1_raw_protocol_io_tests.cpp`
- `tests/unit/m1_cli_tests.cpp`
- `tests/unit/m1_lifecycle_runner_tests.cpp`
- `tests/unit/m1_report_tests.cpp`
- `tests/unit/m1_verification_script_tests.ps1`

Do not create M2 storage files during this plan.

## 4. Verification tiers

| Tier | Required evidence |
|---|---|
| M1-V0 | pinned AIM source, SPEC/PLAN presence, M0 closure commit reachable, clean diff checks |
| M1-V1 | Debug build and all `unit.m1.*` tests |
| M1-V2 | local `integration.m1.shm` 50 consecutive passes plus deterministic 10,000-case fuzz |
| M1-V3 | M0 read-only driver query and dependency audit still pass |
| M1-V4 | one elevated Debug real-driver M1 lifecycle with READ/WRITE/ZERO/UNMAP |
| M1-V5 | each M1 lifecycle failure point cleans up, followed by one normal run |
| M1-V6 | Release import audit and 20 consecutive real-driver lifecycles |

M1 GO requires all tiers. A unit-only or local-SHM-only result is not M1 GO.

---

### Task 0: Establish the isolated M1 execution baseline

**Files:**

- Read: `docs/superpowers/specs/2026-08-01-m1-m2-proxy-dynamic-block-store-design.md`
- Read: `docs/superpowers/plans/2026-08-02-m1-proxy-productization.md`
- Read: `docs/validation/m0-driver-only-acceptance.md`
- No repository files changed

- [ ] **Step 1: Create an isolated worktree**

Use `using-git-worktrees`. The resulting branch name must be `feature/m1-proxy-productization` and its base must contain `0e2b3b3`.

Run:

```powershell
git merge-base --is-ancestor 0e2b3b3 HEAD
if ($LASTEXITCODE -ne 0) { throw 'M0 closure is not an ancestor' }
git status --short
```

Expected: the ancestor check exits 0; the new worktree has no tracked modifications.

- [ ] **Step 2: Verify the pinned upstream contract**

Run:

```powershell
if (-not (Test-Path -LiteralPath 'out/reference/aim/.git')) {
    & .\tools\Fetch-AimReference.ps1
}
git -C out/reference/aim rev-parse HEAD
rg -n "IMDPROXY_REQ_UNMAP|IMDPROXY_REQ_ZERO|IMDPROXY_FLAG_SUPPORTS_UNMAP|IMDPROXY_FLAG_SUPPORTS_ZERO" "out/reference/aim/Unmanaged Source/phdskmnt/inc/imdproxy.h"
```

Expected: commit `d379caa027d21902e45fce8c9daf02fd116c2dda`; request codes 6 and 7 and flags `0x02` and `0x04` are present.

- [ ] **Step 3: Run the non-destructive baseline**

Run:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset debug
ctest --preset debug -R "^(unit\.|integration\.shm$)"
```

Expected: configure/build succeed and the current M0 test set passes. Do not proceed if the baseline fails.

- [ ] **Step 4: Confirm scope alignment**

Run:

```powershell
git status --short
git diff --check
```

Expected: no tracked changes. Task 0 has no commit.

---

### Task 1: Add stable operation errors and cancellation/deadline context

**Files:**

- Modify: `src/common/error.h`
- Create: `src/common/operation_context.h`
- Create: `tests/unit/operation_context_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing context tests**

Create `tests/unit/operation_context_tests.cpp`:

```cpp
#include "test_framework.h"

#include "src/common/operation_context.h"

#include <chrono>
#include <stop_token>

TEST_CASE(m1_operation_context_accepts_live_operation) {
    myrd::OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(1)};
    REQUIRE(myrd::check_operation_context(context).ok());
}

TEST_CASE(m1_operation_context_reports_cancellation_before_timeout) {
    std::stop_source source;
    source.request_stop();
    myrd::OperationContext context{
        source.get_token(), std::chrono::steady_clock::now() -
                                std::chrono::seconds(1)};
    auto result = myrd::check_operation_context(context);
    REQUIRE(result.has_error());
    REQUIRE_EQ(result.error().code,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::cancelled));
}

TEST_CASE(m1_operation_context_reports_expired_deadline) {
    myrd::OperationContext context{
        {}, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)};
    auto result = myrd::check_operation_context(context);
    REQUIRE(result.has_error());
    REQUIRE_EQ(result.error().code,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::timed_out));
}
```

Register `unit.m1.operation_context` as a standalone executable linked to `myrd_test_support` and `myrd_common`.

- [ ] **Step 2: Run the test and observe the missing-header failure**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_operation_context_tests
```

Expected: compilation fails because `src/common/operation_context.h` does not exist.

- [ ] **Step 3: Extend the error model without breaking M0**

Append the following enum before `struct Error` and append `native_code` as the last field so all existing four-field aggregate initializers retain their meaning:

```cpp
enum class OperationErrorCode : std::uint32_t {
    protocol_malformed = 0x1001,
    unknown_request = 0x1002,
    invalid_range = 0x1003,
    misaligned = 0x1004,
    read_only = 0x1005,
    backing_read_failed = 0x1006,
    memory_hard_limit = 0x1007,
    commit_failed = 0x1008,
    decommit_failed = 0x1009,
    cancelled = 0x100A,
    timed_out = 0x100B,
    session_closed = 0x100C,
    version_changed = 0x100D,
    internal_invariant = 0x100E
};

struct Error {
    ErrorDomain domain;
    std::uint32_t code;
    std::wstring operation;
    std::wstring message;
    std::uint32_t native_code = 0;
};
```

- [ ] **Step 4: Implement the header-only context helper**

Create `src/common/operation_context.h`:

```cpp
#pragma once

#include "result.h"

#include <chrono>
#include <stop_token>

namespace myrd {

struct OperationContext {
    std::stop_token stop_token{};
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
};

inline Result<void> check_operation_context(
    const OperationContext& context) {
    if (context.stop_token.stop_requested()) {
        return Result<void>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::cancelled),
            L"check_operation_context", L"operation cancelled"});
    }
    if (std::chrono::steady_clock::now() >= context.deadline) {
        return Result<void>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::timed_out),
            L"check_operation_context", L"operation deadline expired"});
    }
    return Result<void>::success();
}

} // namespace myrd
```

- [ ] **Step 5: Build and run focused tests**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_operation_context_tests
ctest --preset debug -R "^unit\.m1\.operation_context$"
```

Expected: one test executable passes all three cases.

- [ ] **Step 6: Run M0 regression and commit**

Run:

```powershell
ctest --preset debug -R "^unit\."
git diff --check
git add src/common/error.h src/common/operation_context.h tests/unit/operation_context_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add cancellable operation context"
```

Expected: all unit tests pass; commit contains no proxy or storage implementation.

---

### Task 2: Publish the M1 block-device contract and adapt the fixed-memory device

**Files:**

- Modify: `src/storage/block_device.h`
- Modify: `src/storage/memory_block_device.h`
- Modify: `src/storage/memory_block_device.cpp`
- Modify: `src/proxy/request_processor.cpp` (mechanical call-signature adaptation only)
- Create: `tests/unit/block_device_contract_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/storage_tests.cpp`
- Modify: `tests/unit/request_processor_tests.cpp`

- [ ] **Step 1: Write failing contract tests**

The new test must construct a 1 MiB device, assert features, write/read one sector, zero one sector, unmap one sector, reject misalignment, reject overflow, reject writes to a read-only device, and reject a cancelled operation. Use this exact feature assertion and operation shape:

```cpp
myrd::MemoryBlockDevice device(1024 * 1024, 512, false);
const auto features = device.features();
REQUIRE(!features.read_only);
REQUIRE(features.supports_unmap);
REQUIRE(features.supports_zero);

myrd::OperationContext context{};
std::array<std::byte, 512> source{};
source.fill(std::byte{0x5A});
REQUIRE(device.write(context, 0, source).ok());

std::array<std::byte, 512> destination{};
REQUIRE(device.read(context, 0, destination).ok());
REQUIRE(destination == source);

const myrd::DataRange range{0, 512};
REQUIRE(device.zero(context, std::span<const myrd::DataRange>(&range, 1)).ok());
REQUIRE(device.read(context, 0, destination).ok());
REQUIRE(destination[0] == std::byte{0});
```

The unmap case must rewrite `0x5A`, call `unmap()`, and verify a subsequent read returns zero.

- [ ] **Step 2: Observe compile failures from the old interface**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_block_device_contract_tests
```

Expected: compilation fails because `DeviceFeatures`, `DataRange`, context-aware operations, and UNMAP/ZERO are absent.

- [ ] **Step 3: Replace `IBlockDevice` with the approved interface**

`src/storage/block_device.h` must expose exactly:

```cpp
#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"

#include <cstdint>
#include <span>

namespace myrd {

struct DataRange {
    std::uint64_t offset;
    std::uint64_t length;
};

struct DeviceFeatures {
    bool read_only;
    bool supports_unmap;
    bool supports_zero;
};

class IBlockDevice {
public:
    virtual ~IBlockDevice() = default;
    virtual std::uint64_t size_bytes() const noexcept = 0;
    virtual std::uint32_t sector_size() const noexcept = 0;
    virtual DeviceFeatures features() const noexcept = 0;
    virtual Result<void> read(const OperationContext& context,
        std::uint64_t offset, std::span<std::byte> output) = 0;
    virtual Result<void> write(const OperationContext& context,
        std::uint64_t offset, std::span<const std::byte> input) = 0;
    virtual Result<void> unmap(const OperationContext& context,
        std::span<const DataRange> ranges) = 0;
    virtual Result<void> zero(const OperationContext& context,
        std::span<const DataRange> ranges) = 0;
};

} // namespace myrd
```

- [ ] **Step 4: Adapt `MemoryBlockDevice`**

Add a `bool read_only = false` constructor argument, a `std::mutex`, `features()`, and the four context-aware operations. The shared range helper must validate every range before modifying any byte so a malformed multi-range operation cannot partly mutate the device:

```cpp
Result<void> MemoryBlockDevice::apply_zero_ranges(
    const OperationContext& context, std::span<const DataRange> ranges) {
    auto live = check_operation_context(context);
    if (!live) return live;
    for (const auto& range : ranges) {
        auto valid = validate(range.offset, range.length);
        if (!valid) return valid;
    }
    if (read_only_) {
        return Result<void>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::read_only),
            L"MemoryBlockDevice::apply_zero_ranges", L"device is read-only"});
    }
    std::scoped_lock lock(mutex_);
    for (const auto& range : ranges) {
        std::fill_n(bytes_.begin() + static_cast<std::size_t>(range.offset),
                    static_cast<std::size_t>(range.length), std::byte{0});
    }
    return Result<void>::success();
}
```

Both `unmap()` and `zero()` call this helper. `read()` and `write()` must call `check_operation_context()`, validate outside-device and alignment using subtraction instead of unchecked `offset + length`, then lock around the copy. `write()` must return the stable `read_only` error before mutation.

- [ ] **Step 5: Mechanically update M0 callers**

Replace calls shaped as:

```cpp
device.read(offset, output);
device.write(offset, input);
```

with:

```cpp
device.read(myrd::OperationContext{}, offset, output);
device.write(myrd::OperationContext{}, offset, input);
```

Replace assertions against returned byte counts with `.ok()` assertions. Do not change M0 test intent.

- [ ] **Step 6: Run focused and full unit tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^unit\.m1\.block_device_contract$"
ctest --preset debug -R "^unit\."
```

Expected: contract and all pre-existing unit tests pass.

- [ ] **Step 7: Alignment review and commit**

Confirm `memory_block_device.cpp` contains no `VirtualAlloc`, backing-file I/O, dirty metadata, or eviction logic.

Run:

```powershell
rg -n "VirtualAlloc|MEM_COMMIT|evict|dirty|backing" src/storage/memory_block_device.*
git diff --check
git add src/storage/block_device.h src/storage/memory_block_device.h src/storage/memory_block_device.cpp src/proxy/request_processor.cpp tests/unit/block_device_contract_tests.cpp tests/unit/storage_tests.cpp tests/unit/request_processor_tests.cpp tests/CMakeLists.txt
git commit -m "feat: publish M1 block device contract"
```

Expected: the search returns no implementation matches; commit preserves M0 behavior.

---

### Task 3: Complete and lock the M1 wire ABI

**Files:**

- Modify: `src/proxy/imdproxy_protocol.h`
- Create: `tests/unit/m1_protocol_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing ABI tests**

Tests must assert exact request values, flag values, sizes, and offsets against both the pinned contract and the Windows SDK:

```cpp
#include "test_framework.h"
#include "src/proxy/imdproxy_protocol.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

TEST_CASE(m1_protocol_codes_and_flags_match_pinned_aim) {
    REQUIRE_EQ(myrd::proxy::kReqInfo, 1u);
    REQUIRE_EQ(myrd::proxy::kReqRead, 2u);
    REQUIRE_EQ(myrd::proxy::kReqWrite, 3u);
    REQUIRE_EQ(myrd::proxy::kReqClose, 5u);
    REQUIRE_EQ(myrd::proxy::kReqUnmap, 6u);
    REQUIRE_EQ(myrd::proxy::kReqZero, 7u);
    REQUIRE_EQ(myrd::proxy::kFlagReadOnly, 0x01u);
    REQUIRE_EQ(myrd::proxy::kFlagSupportsUnmap, 0x02u);
    REQUIRE_EQ(myrd::proxy::kFlagSupportsZero, 0x04u);
}

TEST_CASE(m1_protocol_unmap_zero_layout_matches_pinned_aim) {
    REQUIRE_EQ(sizeof(myrd::proxy::UnmapRequest), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::UnmapResponse), 8u);
    REQUIRE_EQ(sizeof(myrd::proxy::ZeroRequest), 16u);
    REQUIRE_EQ(sizeof(myrd::proxy::ZeroResponse), 8u);
    REQUIRE_EQ(sizeof(myrd::proxy::DataSetRangeWire),
               sizeof(DEVICE_DATA_SET_RANGE));
    REQUIRE_EQ(offsetof(myrd::proxy::DataSetRangeWire, offset),
               offsetof(DEVICE_DATA_SET_RANGE, StartingOffset));
    REQUIRE_EQ(offsetof(myrd::proxy::DataSetRangeWire, length),
               offsetof(DEVICE_DATA_SET_RANGE, LengthInBytes));
}
```

- [ ] **Step 2: Observe missing-symbol failures**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_protocol_tests
```

Expected: compile fails for UNMAP/ZERO constants and structures.

- [ ] **Step 3: Add audited constants and structures**

Add to `imdproxy_protocol.h`:

```cpp
constexpr std::uint64_t kReqUnmap = 6;
constexpr std::uint64_t kReqZero = 7;

constexpr std::uint64_t kFlagReadOnly = 0x01;
constexpr std::uint64_t kFlagSupportsUnmap = 0x02;
constexpr std::uint64_t kFlagSupportsZero = 0x04;

struct UnmapRequest {
    std::uint64_t request_code;
    std::uint64_t length;
};
struct UnmapResponse { std::uint64_t errorno; };
struct ZeroRequest {
    std::uint64_t request_code;
    std::uint64_t length;
};
struct ZeroResponse { std::uint64_t errorno; };
struct DataSetRangeWire {
    std::uint64_t offset;
    std::uint64_t length;
};

static_assert(sizeof(UnmapRequest) == 16);
static_assert(sizeof(UnmapResponse) == 8);
static_assert(sizeof(ZeroRequest) == 16);
static_assert(sizeof(ZeroResponse) == 8);
static_assert(sizeof(DataSetRangeWire) == 16);
```

Also add missing standard headers used by wire helpers: `<cstring>` and `<type_traits>`.

- [ ] **Step 4: Add truncation and round-trip cases**

Add tests that `read_wire<UnmapRequest>` rejects 15 bytes, accepts 16 bytes, preserves a 64-bit length, and `write_wire<ZeroResponse>` writes exactly 8 response bytes without touching the following sentinel byte.

- [ ] **Step 5: Run ABI tests and regressions**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^(unit\.m1\.protocol|unit\.protocol)$"
git diff --check
```

Expected: both old and new protocol tests pass.

- [ ] **Step 6: Commit**

```powershell
git add src/proxy/imdproxy_protocol.h tests/unit/m1_protocol_tests.cpp tests/CMakeLists.txt
git commit -m "feat: define complete M1 proxy ABI"
```

---

### Task 4: Implement stable proxy error mapping

**Files:**

- Create: `src/proxy/proxy_error.h`
- Create: `src/proxy/proxy_error.cpp`
- Create: `tests/unit/proxy_error_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the complete mapping table test**

Use a table-driven test covering every M1 mapping:

```cpp
struct MappingCase {
    myrd::OperationErrorCode code;
    std::uint32_t native_code;
    std::uint64_t expected;
};

const MappingCase cases[] = {
    {myrd::OperationErrorCode::invalid_range, 0, ERROR_INVALID_PARAMETER},
    {myrd::OperationErrorCode::misaligned, 0, ERROR_INVALID_PARAMETER},
    {myrd::OperationErrorCode::read_only, 0, ERROR_WRITE_PROTECT},
    {myrd::OperationErrorCode::backing_read_failed, 0, ERROR_READ_FAULT},
    {myrd::OperationErrorCode::memory_hard_limit, 0, ERROR_NOT_ENOUGH_MEMORY},
    {myrd::OperationErrorCode::commit_failed, ERROR_COMMITMENT_LIMIT,
        ERROR_COMMITMENT_LIMIT},
    {myrd::OperationErrorCode::decommit_failed, 0, ERROR_NOT_ENOUGH_MEMORY},
    {myrd::OperationErrorCode::cancelled, 0, ERROR_OPERATION_ABORTED},
    {myrd::OperationErrorCode::timed_out, 0, ERROR_TIMEOUT},
    {myrd::OperationErrorCode::session_closed, 0, ERROR_BROKEN_PIPE},
    {myrd::OperationErrorCode::internal_invariant, 0, ERROR_INTERNAL_ERROR}
};
```

For each row, construct an `Error` with `code` and `native_code`, then assert `to_proxy_errorno()`.

- [ ] **Step 2: Observe missing API failure**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_proxy_error_tests
```

Expected: compile fails because `proxy_error.h` is absent.

- [ ] **Step 3: Implement mapping and classification**

Expose:

```cpp
namespace myrd::proxy {

enum class ErrorDisposition : std::uint8_t { request, session_fatal };

std::uint64_t to_proxy_errorno(const Error& error) noexcept;
ErrorDisposition classify_error(const Error& error) noexcept;

} // namespace myrd::proxy
```

`to_proxy_errorno()` must switch on `static_cast<OperationErrorCode>(error.code)`. Commit/decommit use `native_code` when nonzero. Unknown/internal errors map to `ERROR_INTERNAL_ERROR`.

`classify_error()` returns `session_fatal` only for `protocol_malformed`, `unknown_request`, `internal_invariant`, and unknown codes. All range, alignment, device, cancellation, and timeout errors are request-level.

- [ ] **Step 4: Run focused tests and commit**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_proxy_error_tests
ctest --preset debug -R "^unit\.m1\.proxy_error$"
git diff --check
git add src/proxy/proxy_error.h src/proxy/proxy_error.cpp tests/unit/proxy_error_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: map M1 errors to stable proxy responses"
```

---

### Task 5: Decode and normalize UNMAP/ZERO ranges

**Files:**

- Create: `src/proxy/range_normalizer.h`
- Create: `src/proxy/range_normalizer.cpp`
- Create: `tests/unit/range_normalizer_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing normalization tests**

Tests must cover: empty list, byte length not divisible by 16, length beyond SHM data area, zero-length entries ignored, misaligned offset/length, `offset + length` overflow, device-end overflow, unsorted input, overlap, adjacency, and separated ranges.

Use this success case:

```cpp
const myrd::proxy::DataSetRangeWire wire[] = {
    {4096, 4096}, {0, 4096}, {2048, 4096}, {16384, 0}
};
auto bytes = std::as_bytes(std::span(wire));
auto result = myrd::proxy::decode_and_normalize_ranges(
    bytes, bytes.size(), 64 * 1024, 512);
REQUIRE(result.ok());
REQUIRE_EQ(result.value().size(), 1u);
REQUIRE_EQ(result.value()[0].offset, 0u);
REQUIRE_EQ(result.value()[0].length, 8192u);
```

- [ ] **Step 2: Observe missing normalizer failure**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_range_normalizer_tests
```

Expected: compile fails because the normalizer is absent.

- [ ] **Step 3: Implement the exact API**

`range_normalizer.h` must declare:

```cpp
Result<std::vector<DataRange>> decode_and_normalize_ranges(
    std::span<const std::byte> data_area,
    std::uint64_t encoded_length,
    std::uint64_t device_size,
    std::uint32_t sector_size);
```

Implementation order is mandatory:

1. reject `encoded_length > data_area.size()`;
2. reject non-multiple of `sizeof(DataSetRangeWire)`;
3. copy each wire entry with `read_wire` rather than reinterpret-casting untrusted bytes;
4. ignore zero-length entries;
5. validate sector alignment;
6. reject `offset > device_size` or `length > device_size - offset`;
7. sort by `(offset, length)`;
8. merge overlap/adjacency using subtraction-safe comparisons.

Use stable `invalid_range` or `misaligned` internal codes for every failure.

- [ ] **Step 4: Add the boundary cases and run tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^unit\.m1\.range_normalizer$"
git diff --check
```

Expected: all listed cases pass and no test allocates from an attacker-controlled count before checking the data-area bound.

- [ ] **Step 5: Commit**

```powershell
git add src/proxy/range_normalizer.h src/proxy/range_normalizer.cpp tests/unit/range_normalizer_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: normalize M1 data set ranges"
```

---

### Task 6: Add a strict fake block device for processor tests

**Files:**

- Create: `tests/support/fake_block_device.h`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create a compile-only consumer test**

At the top of the forthcoming `tests/unit/m1_request_processor_tests.cpp`, instantiate `FakeBlockDevice` and assert its initial call counters are zero. Register the test target before creating the fake header.

- [ ] **Step 2: Observe the missing fake failure**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_request_processor_tests
```

Expected: compile fails because `fake_block_device.h` is absent.

- [ ] **Step 3: Implement the strict fake**

The fake must implement `IBlockDevice` and expose:

```cpp
struct FakeBlockDevice final : myrd::IBlockDevice {
    std::uint64_t size = 64ull * 1024ull * 1024ull;
    std::uint32_t sector = 512;
    myrd::DeviceFeatures advertised{false, true, true};
    myrd::Result<void> next_result = myrd::Result<void>::success();
    std::vector<std::byte> bytes =
        std::vector<std::byte>(static_cast<std::size_t>(size));
    std::uint64_t read_calls = 0;
    std::uint64_t write_calls = 0;
    std::uint64_t unmap_calls = 0;
    std::uint64_t zero_calls = 0;
    std::vector<myrd::DataRange> last_ranges;

    std::uint64_t size_bytes() const noexcept override { return size; }
    std::uint32_t sector_size() const noexcept override { return sector; }
    myrd::DeviceFeatures features() const noexcept override {
        return advertised;
    }
```

Each operation must increment only its own counter, copy its input/ranges for later assertion, and return `next_result` before touching bytes when it is a failure. The fake itself must bounds-check so an invalid processor call fails the test immediately with `REQUIRE`.

- [ ] **Step 4: Build the consumer and commit support**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_request_processor_tests
git diff --check
git add tests/support/fake_block_device.h tests/unit/m1_request_processor_tests.cpp tests/CMakeLists.txt
git commit -m "test: add strict M1 block device fake"
```

Expected: the initial consumer test passes; protocol behavior is added in later tasks.

---

### Task 7: Productize INFO and READ request processing

**Files:**

- Modify: `src/proxy/request_processor.h`
- Modify: `src/proxy/request_processor.cpp`
- Modify: `tests/unit/m1_request_processor_tests.cpp`
- Modify: existing RequestProcessor callers/tests for the context overload

- [ ] **Step 1: Add failing INFO/READ tests**

Cover:

- INFO exact capacity/alignment and flags from `DeviceFeatures`;
- read-only INFO flag;
- READ data copied before success header publication;
- zero length, data-area overflow, offset/length misalignment, addition overflow, and device-end overflow;
- backend failure mapped by `to_proxy_errorno()` with response length zero;
- cancelled and expired context do not call the fake;
- exact per-type and per-error counters.

All tests invoke:

```cpp
auto outcome = processor.process(context, mapping);
```

- [ ] **Step 2: Observe signature and behavior failures**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_request_processor_tests
ctest --preset debug -R "^unit\.m1\.request_processor$"
```

Expected: compile or assertions fail because the context overload and feature flags are not implemented.

- [ ] **Step 3: Publish the M1 processor surface**

Use these public types:

```cpp
enum class ProcessorAction : std::uint8_t { respond, close, fatal };

struct RequestCounters {
    std::uint64_t info = 0;
    std::uint64_t read = 0;
    std::uint64_t write = 0;
    std::uint64_t unmap = 0;
    std::uint64_t zero = 0;
    std::uint64_t close = 0;
    std::uint64_t info_errors = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t unmap_errors = 0;
    std::uint64_t zero_errors = 0;
    std::uint64_t fatal_protocol_errors = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t timed_out = 0;
};

struct RequestMetadata {
    std::uint64_t request_code = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct ProcessOutcome {
    ProcessorAction action;
    std::optional<Error> error;
    RequestMetadata request;
};

class RequestProcessor final {
public:
    explicit RequestProcessor(IBlockDevice& device) noexcept;
    ProcessOutcome process(const OperationContext& context,
                           std::span<std::byte> mapping) noexcept;
    ProcessOutcome process(std::span<std::byte> mapping) noexcept;
    RequestCounters counters() const noexcept;
};
```

The context-free overload is an M0 compatibility shim that calls the context-aware overload with an unlimited context. Every request-level or fatal failure returns its `Error` and decoded `RequestMetadata` in `ProcessOutcome`; normal requests return no error. This gives the session structured diagnostics without logging disk data.

- [ ] **Step 4: Implement shared validation and response helpers**

Use a helper returning stable internal errors:

```cpp
Result<void> validate_io(std::uint64_t offset, std::uint64_t length,
                         std::uint64_t device_size,
                         std::uint32_t sector_size) {
    if (length == 0 || length > kDataAreaSize || offset > device_size ||
        length > device_size - offset) {
        return Result<void>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"validate_io", L"request range is invalid"});
    }
    if ((offset % sector_size) != 0 || (length % sector_size) != 0) {
        return Result<void>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::misaligned),
            L"validate_io", L"request range is not sector aligned"});
    }
    return Result<void>::success();
}
```

Implement one response helper per response shape. Every ignored `write_wire()` result is a fatal invariant bug; return `ProcessorAction::fatal` rather than continuing.

- [ ] **Step 5: Implement INFO and READ**

INFO flags must be derived only from `device_.features()`:

```cpp
const auto features = device_.features();
InfoResponse response{
    device_.size_bytes(),
    device_.sector_size(),
    (features.read_only ? kFlagReadOnly : 0ull) |
    (features.supports_unmap ? kFlagSupportsUnmap : 0ull) |
    (features.supports_zero ? kFlagSupportsZero : 0ull)};
```

READ must validate the header and range, check context, call the device into the SHM data area, then write `{0, requested_length}`. On request-level failure write `{mapped_error, 0}`. A malformed/unknown request that cannot be safely interpreted is fatal.

- [ ] **Step 6: Catch exceptions at the noexcept boundary**

Wrap dispatch so `std::bad_alloc` becomes request-level `memory_hard_limit` when a response layout is known; all other exceptions become fatal `internal_invariant`. No C++ exception may escape `process()`.

- [ ] **Step 7: Run tests and M0 regressions**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^(unit\.m1\.request_processor|unit\.request_processor|integration\.shm)$"
git diff --check
```

Expected: INFO/READ M1 cases and all old processor/session tests pass.

- [ ] **Step 8: Commit**

```powershell
git add src/proxy/request_processor.h src/proxy/request_processor.cpp tests/unit/m1_request_processor_tests.cpp tests/unit/request_processor_tests.cpp tests/integration/shm_proxy_server_tests.cpp
git commit -m "feat: productize M1 info and read requests"
```

---

### Task 8: Productize WRITE and exact request-level failures

**Files:**

- Modify: `src/proxy/request_processor.cpp`
- Modify: `tests/unit/m1_request_processor_tests.cpp`

- [ ] **Step 1: Add failing WRITE tests**

Cover successful private-copy behavior, all validation boundaries from READ, read-only mapping to `ERROR_WRITE_PROTECT`, injected backend error mapping, cancelled/expired context, response `{errorno, 0}` on failure, response `{0, length}` on success, and exactly one WRITE counter increment per decoded WRITE request.

The private-copy fake must mutate the SHM data area during `write()` and assert the source span still contains the original bytes.

- [ ] **Step 2: Observe failures**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_request_processor_tests
ctest --preset debug -R "^unit\.m1\.request_processor$"
```

Expected: new WRITE cases fail against the old implementation.

- [ ] **Step 3: Implement WRITE**

The dispatch branch must perform this order:

```cpp
auto request = read_wire<WriteRequest>(header, 0);
if (!request) return fatal(request.error());
auto valid = validate_io(request.value().offset, request.value().length,
                         device_.size_bytes(), device_.sector_size());
if (!valid) return write_error_response<WriteResponse>(mapping, valid.error());
auto live = check_operation_context(context);
if (!live) return write_error_response<WriteResponse>(mapping, live.error());

const auto length = static_cast<std::size_t>(request.value().length);
std::vector<std::byte> private_copy(length);
std::memcpy(private_copy.data(), mapping.data() + kHeaderSize, length);
auto result = device_.write(context, request.value().offset, private_copy);
if (!result) return write_error_response<WriteResponse>(mapping, result.error());
return write_success_response(mapping, WriteResponse{0, request.value().length});
```

Increment counters in one exit helper so errors cannot double count. Increment the matching per-type error field for every request-level WRITE failure.

- [ ] **Step 4: Run focused, protocol, and M0 regression tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^(unit\.m1\.request_processor|unit\.request_processor|integration\.shm)$"
git diff --check
```

Expected: all pass.

- [ ] **Step 5: Commit**

```powershell
git add src/proxy/request_processor.cpp tests/unit/m1_request_processor_tests.cpp
git commit -m "feat: productize M1 write requests"
```

---

### Task 9: Implement UNMAP, ZERO, CLOSE, and fatal protocol behavior

**Files:**

- Modify: `src/proxy/request_processor.cpp`
- Modify: `tests/unit/m1_request_processor_tests.cpp`

- [ ] **Step 1: Add failing UNMAP/ZERO tests**

Cover exact request/response layouts, empty range arrays, malformed byte lengths, maximum data-area boundary, zero-length range removal, sort/merge forwarding, alignment, arithmetic overflow, capacity overflow, backend errors, cancellation, timeout, and separate counters.

For the success case, encode ranges into the data-area span beginning at `mapping.data() + kHeaderSize`, invoke the processor, assert `UnmapResponse{0}` or `ZeroResponse{0}`, and compare the fake's `last_ranges` to normalized values.

- [ ] **Step 2: Add failing CLOSE/fatal tests**

Cover:

- exact CLOSE returns `ProcessorAction::close` and no response;
- a truncated mapping returns fatal;
- unknown request code returns fatal `unknown_request`;
- response write failure is fatal;
- no backend operation occurs after any fatal decode;
- after a session consumes CLOSE it will not consume a second queued request; the latter belongs to the session integration task.

- [ ] **Step 3: Observe failures**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_request_processor_tests
ctest --preset debug -R "^unit\.m1\.request_processor$"
```

Expected: new operation tests fail.

- [ ] **Step 4: Implement UNMAP/ZERO with one shared dispatcher**

Use a private operation enum and route only after decoding the matching 16-byte header:

```cpp
enum class RangeOperation : std::uint8_t { unmap, zero };

auto ranges = decode_and_normalize_ranges(
    data_area, request.length, device_.size_bytes(), device_.sector_size());
if (!ranges) return write_range_error_response(mapping, operation,
                                                ranges.error());
auto result = operation == RangeOperation::unmap
    ? device_.unmap(context, ranges.value())
    : device_.zero(context, ranges.value());
if (!result) return write_range_error_response(mapping, operation,
                                                result.error());
return write_range_success_response(mapping, operation);
```

Do not advertise or implement CONNECT, SCSI, or SHARED request codes.

- [ ] **Step 5: Implement CLOSE and fatal branches**

CLOSE increments its counter and returns `close`. Unknown codes create `unknown_request`, increment fatal count, and return `fatal`. A header too small to identify a safe response creates `protocol_malformed` and is fatal.

- [ ] **Step 6: Run all M1-A unit tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^unit\.m1\."
ctest --preset debug -R "^(unit\.request_processor|integration\.shm)$"
git diff --check
```

Expected: all pass.

- [ ] **Step 7: Commit**

```powershell
git add src/proxy/request_processor.cpp tests/unit/m1_request_processor_tests.cpp
git commit -m "feat: support M1 range and close requests"
```

---

### Task 10: Add deterministic 10,000-case protocol fuzzing

**Files:**

- Create: `tests/unit/m1_protocol_fuzz_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the deterministic fuzz driver**

Use `std::mt19937_64` with fixed seed `0x4D3150524F585955ull`. Generate exactly 10,000 mappings covering all six valid codes, unknown codes, truncated logical views, arbitrary 64-bit offsets/lengths, arbitrary range-array lengths, aligned valid requests, and injected backend errors.

Each iteration must:

```cpp
const auto before = fake.total_calls();
const auto outcome = processor.process(context, logical_mapping);
REQUIRE(outcome.action == myrd::proxy::ProcessorAction::respond ||
        outcome.action == myrd::proxy::ProcessorAction::close ||
        outcome.action == myrd::proxy::ProcessorAction::fatal);
REQUIRE(fake.invalid_call_count == 0);
REQUIRE(fake.total_calls() >= before);
REQUIRE(fake.total_calls() <= before + 1);
```

On failure, print seed and iteration before `REQUIRE` exits. A valid normalized UNMAP/ZERO call counts as one backend call regardless of range count.

- [ ] **Step 2: Run and observe any processor defects**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_protocol_fuzz_tests
ctest --preset debug -R "^unit\.m1\.protocol_fuzz$" --output-on-failure
```

Expected before hardening: the test may expose a boundary/counter defect; retain the printed seed/iteration as a regression case.

- [ ] **Step 3: Fix only reproducible processor defects**

For each discovered input, add a named deterministic case to `m1_request_processor_tests.cpp` before changing production code. Keep the fixed-seed loop unchanged.

- [ ] **Step 4: Prove deterministic stability**

Run:

```powershell
1..20 | ForEach-Object {
    ctest --preset debug -R "^unit\.m1\.protocol_fuzz$" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "fuzz repetition $_ failed" }
}
git diff --check
```

Expected: all 20 invocations pass the same 10,000-case sequence.

- [ ] **Step 5: Commit and record M1-A candidate**

```powershell
git add tests/unit/m1_protocol_fuzz_tests.cpp tests/unit/m1_request_processor_tests.cpp tests/support/fake_block_device.h tests/CMakeLists.txt src/proxy
git commit -m "test: fuzz M1 proxy protocol deterministically"
```

M1-A is a candidate only after Tasks 1-10 pass together; it is not yet M1 GO.

---

### Task 11: Add namespace policies and complete named-object identities

**Files:**

- Modify: `src/proxy/object_names.h`
- Modify: `src/proxy/object_names.cpp`
- Create: `tests/unit/namespace_policy_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/unit/object_names_tests.cpp` (additive Cancel Event assertions only)

- [ ] **Step 1: Write failing policy tests**

Cover random session ID format, invalid IDs, M0 compatibility, local policy names, kernel-visible M1 names, cancel/owner names, and exact driver path. The M1 expected shape is:

```text
mapping:     Global\myrd-m1-<32 lowercase hex>
request:     Global\myrd-m1-<id>_Request
response:    Global\myrd-m1-<id>_Response
cancel:      Global\myrd-m1-<id>_Cancel
owner:       Global\myrd-m1-<id>_Server
driver_path: \BaseNamedObjects\Global\myrd-m1-<id>
```

- [ ] **Step 2: Observe missing policy failures**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_namespace_policy_tests
```

Expected: compile fails because `IProxyNamespacePolicy` and new fields do not exist.

- [ ] **Step 3: Publish compatible naming types**

Use:

```cpp
struct ObjectNames {
    std::wstring mapping;
    std::wstring request;
    std::wstring response;
    std::wstring cancel;
    std::wstring server;
    std::wstring driver_path;
    static Result<ObjectNames> from_session(
        std::wstring_view session_id);
};

using ProxyObjectNames = ObjectNames;

class IProxyNamespacePolicy {
public:
    virtual ~IProxyNamespacePolicy() = default;
    virtual Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const = 0;
};

class KernelVisibleNamespacePolicy final : public IProxyNamespacePolicy {
public:
    Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const override;
};

class LocalNamespacePolicy final : public IProxyNamespacePolicy {
public:
    Result<ProxyObjectNames> build_names(
        std::wstring_view session_id) const override;
};
```

`ObjectNames::from_session()` remains the M0 compatibility factory and continues to produce the proven `Global\myrd-m0-<id>` names. `KernelVisibleNamespacePolicy` produces the new M1 prefix. There is no free factory, fallback, or M0 caller migration in this task.

`LocalNamespacePolicy` produces `Local\myrd-m1-test-<id>` plus the same `_Request`, `_Response`, `_Cancel`, and `_Server` suffixes; its `driver_path` is empty because local-policy names must never be passed to `phdskmnt.sys`.

- [ ] **Step 4: Implement one strict ID validator**

All policies must call one validator requiring exactly 32 lowercase hex characters. There is no fallback sequence and no retry with alternate namespaces.

- [ ] **Step 5: Run policy, M0 name, and full unit tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^(unit\.m1\.namespace_policy|unit\.object_names)$"
ctest --preset debug -R "^unit\."
git diff --check
```

Expected: M1 and M0 exact-name assertions pass.

- [ ] **Step 6: Commit**

```powershell
git add src/proxy/object_names.h src/proxy/object_names.cpp tests/unit/namespace_policy_tests.cpp tests/unit/object_names_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add explicit M1 namespace policies"
```

---

### Task 12: Harden the shared-memory channel and add Cancel Event

**Files:**

- Modify: `src/proxy/shared_memory_channel.h`
- Modify: `src/proxy/shared_memory_channel.cpp`
- Modify: `tests/integration/m1_shm_session_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: old SHM tests only for additive behavior

- [ ] **Step 1: Add failing channel lifecycle tests**

Cover creation/access of Mapping, Request, Response, Cancel, and owner mutex; duplicate rejection for each public object identity; zero-filled mapping; explicit DACL denying an ordinary unprivileged test SID while allowing LocalSystem and Builtin Administrators; and all handles becoming unopenable after destruction.

- [ ] **Step 2: Add initialization-stage failure tests**

Define stages:

```cpp
enum class ChannelCreateStage : std::uint8_t {
    owner_mutex, mapping, map_view, request_event,
    response_event, cancel_event
};

using ChannelCreateHook =
    std::function<Result<void>(ChannelCreateStage)>;
```

For every stage, inject a failure before the Win32 operation and assert all earlier objects are released after `create()` returns an error.

- [ ] **Step 3: Observe failures**

Run:

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m1_shm_session_tests
ctest --preset debug -R "^integration\.m1\.shm$"
```

Expected: compile or assertions fail because Cancel Event and hooks are absent.

- [ ] **Step 4: Extend channel ownership**

Add:

```cpp
static Result<SharedMemoryChannel> create(
    const ProxyObjectNames& names,
    ChannelCreateHook hook = {});
HANDLE cancel_event() const noexcept;
```

Creation order must be owner mutex, mapping, map view, request, response, cancel. Request and Response Events are auto-reset; Cancel Event is manual-reset so the request worker and cancel monitor can both observe shutdown. After mapping, call `std::memset(view, 0, kMappingSize)` before returning success. Every named object uses the same explicit non-inheritable DACL.

- [ ] **Step 5: Keep the DACL narrow**

The M1 default SDDL is:

```text
D:P(A;;GA;;;SY)(A;;GA;;;BA)
```

M1 has no installed service SID yet; M4 may add its concrete service SID. Do not grant Everyone, Authenticated Users, or interactive users.

- [ ] **Step 6: Run SHM tests 50 times and M0 regression**

Run:

```powershell
cmake --build --preset debug
1..50 | ForEach-Object {
    ctest --preset debug -R "^integration\.m1\.shm$" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "M1 SHM repetition $_ failed" }
}
ctest --preset debug -R "^integration\.shm$"
git diff --check
```

Expected: all repetitions and old integration test pass.

- [ ] **Step 7: Commit**

```powershell
git add src/proxy/shared_memory_channel.h src/proxy/shared_memory_channel.cpp tests/integration/m1_shm_session_tests.cpp tests/integration/shm_proxy_server_tests.cpp tests/CMakeLists.txt
git commit -m "feat: harden M1 shared memory channel"
```

---

### Task 13: Implement the `ShmProxySession` state machine

**Files:**

- Create: `src/proxy/shm_proxy_session.h`
- Create: `src/proxy/shm_proxy_session.cpp`
- Modify: `tests/integration/m1_shm_session_tests.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add failing state/ready tests**

Cover the exact stable states `constructed`, `objects_created`, `ready`, `serving`, `closing`, `closed`, and `failed`. Assert `start()` does not return until the worker has published Ready, a second `start()` fails, and destruction/`stop()` before start is safe and idempotent.

- [ ] **Step 2: Add failing request-order tests**

For each request, write the request/data, set Request Event, wait Response Event, and verify the response. Instrument the fake so the test proves Response Event is not signaled before backend completion and response bytes are complete when the wait returns.

- [ ] **Step 3: Add failing cancellation/fatal tests**

Cover Cancel Event while idle, cancellation during delayed backend work, request deadline expiry, CLOSE, SetEvent(Response) failure injection, unknown request fatal behavior, no request after fatal/CLOSE, worker join, and complete handle/thread cleanup.

- [ ] **Step 4: Observe failures**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_shm_session_tests
ctest --preset debug -R "^integration\.m1\.shm$" --output-on-failure
```

Expected: compile fails because `ShmProxySession` is absent.

- [ ] **Step 5: Publish the session API**

Use:

```cpp
enum class SessionState : std::uint8_t {
    constructed, objects_created, ready, serving,
    closing, closed, failed
};

struct ShmProxySessionOptions {
    std::chrono::milliseconds request_timeout{30000};
    std::size_t max_recent_diagnostics = 64;
};

enum class SessionStartStage : std::uint8_t {
    cancel_monitor, request_worker, ready_publish
};

struct ShmProxySessionHooks {
    std::function<Result<void>(SessionStartStage)> before_start_stage;
    std::function<Result<void>(HANDLE, std::wstring_view)> signal_event;
};

struct SessionDiagnostic {
    std::uint64_t sequence;
    RequestMetadata request;
    Error error;
};

struct SessionSnapshot {
    SessionState state;
    RequestCounters requests;
    std::optional<Error> last_error;
    std::vector<SessionDiagnostic> recent_diagnostics;
};

class ShmProxySession final {
public:
    ShmProxySession(SharedMemoryChannel channel,
                    RequestProcessor& processor,
                    ShmProxySessionOptions options = {},
                    ShmProxySessionHooks hooks = {});
    ~ShmProxySession() noexcept;
    Result<void> start();
    Result<void> cancel();
    Result<void> join();
    Result<void> stop();
    SessionSnapshot snapshot() const;
    const ProxyObjectNames& names() const noexcept;
};
```

- [ ] **Step 6: Implement Ready and worker ownership**

`start()` changes `constructed -> objects_created`, invokes the matching start-stage hook, launches a request `std::jthread` and a cancel-monitor `std::jthread`, then waits on a `std::promise<Result<void>>` future. The cancel monitor waits on the named Cancel Event and calls `stop_source_.request_stop()` so cancellation can reach a backend operation while the request worker is blocked inside it. After both threads are valid, the request worker publishes `ready`, fulfills the promise, changes to `serving`, then waits on Request Event and Cancel Event. Hook/thread/promise failures transition to `failed`, signal Cancel Event, and join both threads safely.

- [ ] **Step 7: Implement the request loop and response ordering**

Each Request Event creates:

```cpp
OperationContext context{
    stop_source_.get_token(),
    std::chrono::steady_clock::now() + options_.request_timeout};
auto outcome = processor_.process(context, channel_.mapping());
```

For `respond`, call the signal hook for Response Event only after `process()` returns and all response writes are done. The production hook wraps `SetEvent()` and preserves `GetLastError()`; hook/SetEvent failure is fatal. For `close`, transition to closing and exit without waiting for another request. For `fatal`, store the exact error, transition to failed, and exit.

When `ProcessOutcome.error` is present, append a `SessionDiagnostic` to a bounded FIFO before signaling a request-level response or entering failed state. Drop the oldest diagnostic when the configured capacity is reached. Do not record successful I/O and do not copy SHM data into diagnostics.

- [ ] **Step 8: Implement bounded stop/join**

`cancel()` requests the stop token and sets named Cancel Event. `join()` sets Cancel Event if needed, then joins both threads without holding the state/error mutex. `stop()` is idempotent and performs cancel then join; a failed session remains `failed`, while normal cancellation/CLOSE becomes `closed`.

- [ ] **Step 9: Remove data races**

Protect state and last error with one mutex or atomics plus a mutex for the optional error. Tests must query `snapshot()` concurrently during a delayed request under Thread Sanitizer-equivalent reasoning; MSVC has no required sanitizer gate, so repeated stress and lock review are mandatory.

- [ ] **Step 10: Run local M1-B tests 50 times**

Run:

```powershell
cmake --build --preset debug
1..50 | ForEach-Object {
    ctest --preset debug -R "^integration\.m1\.shm$" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "M1 session repetition $_ failed" }
}
ctest --preset debug -R "^(unit\.m1\.|integration\.shm$)"
git diff --check
```

Expected: all 50 passes; no stuck worker or open named object.

- [ ] **Step 11: Commit and record M1-B candidate**

```powershell
git add src/proxy/shm_proxy_session.h src/proxy/shm_proxy_session.cpp tests/integration/m1_shm_session_tests.cpp src/CMakeLists.txt
git commit -m "feat: add productized M1 proxy session"
```

M1-B is a candidate after Tasks 11-13 pass; real driver work has not yet passed.

---

### Task 14: Compose the M1 provider without changing M0

**Files:**

- Create: `src/m1/m1_provider.h`
- Create: `src/m1/m1_provider.cpp`
- Create: `tests/unit/m1_provider_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing composition tests**

Tests must assert one provider instance owns one random session, a 64 MiB/512-byte fixed `MemoryBlockDevice`, a kernel-visible M1 path, a RequestProcessor, and one ShmProxySession. Start must publish Ready; stop must release all names; start failure must release partially created objects.

- [ ] **Step 2: Observe missing provider failure**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_provider_tests
```

Expected: compile fails because `M1Provider` is absent.

- [ ] **Step 3: Publish the provider surface**

Use:

```cpp
struct M1ProviderConfig {
    std::uint64_t size_bytes = 64ull * 1024ull * 1024ull;
    std::uint32_t sector_size = 512;
    std::chrono::milliseconds request_timeout{30000};
};

class M1Provider final {
public:
    static Result<std::unique_ptr<M1Provider>> create(
        M1ProviderConfig config = {});
    static Result<std::unique_ptr<M1Provider>> create_for_test(
        const IProxyNamespacePolicy& namespace_policy,
        M1ProviderConfig config = {});
    ~M1Provider() noexcept;
    Result<void> start();
    Result<void> stop();
    std::wstring native_object_path() const;
    ProxyObjectNames names() const;
    SessionSnapshot snapshot() const;
};
```

Use a factory because RNG, name policy, channel creation, and memory allocation can fail. The constructor itself must not silently leave an invalid object. Unit tests call `create_for_test()` with `LocalNamespacePolicy`; real-driver code calls `create()` and cannot override the kernel-visible policy.

- [ ] **Step 4: Implement strict composition**

Creation order is session ID, the selected namespace policy, fixed `MemoryBlockDevice`, RequestProcessor, SharedMemoryChannel, then ShmProxySession. The production factory always selects `KernelVisibleNamespacePolicy`. `start()` only starts the session. Device creation remains outside the provider. `stop()` only stops/releases session objects; it never removes a kernel disk.

- [ ] **Step 5: Run provider, local SHM, and M0 tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^(unit\.m1\.provider|integration\.m1\.shm|unit\.lifecycle_runner)$"
git diff --check
```

Expected: all pass.

- [ ] **Step 6: Commit**

```powershell
git add src/m1/m1_provider.h src/m1/m1_provider.cpp tests/unit/m1_provider_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: compose the M1 proxy provider"
```

---

### Task 15: Implement real-driver READ/WRITE/ZERO/UNMAP verification

**Files:**

- Create: `src/m1/m1_raw_protocol_io.h`
- Create: `src/m1/m1_raw_protocol_io.cpp`
- Create: `tests/unit/m1_raw_protocol_io_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Define an injectable raw-disk API and failing tests**

Expose an `IM1RawDiskApi` with open, aligned read, aligned write, flush, and trim-range methods so unit tests never touch a real disk. Tests must assert this sequence:

1. write deterministic nonzero bytes and read/hash them;
2. write an all-zero buffer to the same range and verify zeros;
3. rewrite deterministic nonzero bytes;
4. issue one TRIM range and verify zeros;
5. reject an unexpected read, short I/O, flush failure, or TRIM failure.

- [ ] **Step 2: Observe missing verifier failure**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_raw_protocol_io_tests
```

Expected: compile fails because the verifier is absent.

- [ ] **Step 3: Publish result evidence**

Use:

```cpp
struct M1RawProtocolEvidence {
    std::uint64_t offset;
    std::uint64_t length;
    std::string write_sha256;
    std::string read_sha256;
    bool zero_write_verified;
    bool unmap_verified;
    bool flush_succeeded;
};

Result<M1RawProtocolEvidence> verify_m1_raw_protocol(
    IM1RawDiskApi& api,
    const aim::LocatedDisk& disk,
    const proxy::RequestCounters& before,
    const std::function<proxy::RequestCounters()>& counters);
```

Use an offset of 8 MiB and a length of 1 MiB, both divisible by 4096 and below the 64 MiB device end.

- [ ] **Step 4: Implement real all-zero WRITE verification**

Write the 1 MiB zero buffer through `WriteFile`. Because INFO advertises `SUPPORTS_ZERO`, pinned `phdskmnt.sys` detects an all-zero write and sends request code 7. After completion, poll provider counters for up to 5 seconds and require `zero > before.zero`, then read and verify all bytes are zero.

- [ ] **Step 5: Implement real UNMAP verification**

`WindowsM1RawDiskApi::trim_range()` must call `DeviceIoControl(IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES)` using one `DEVICE_DATA_SET_RANGE` and `DeviceDsmAction_Trim`:

```cpp
struct TrimInput {
    DEVICE_MANAGE_DATA_SET_ATTRIBUTES attributes;
    DEVICE_DATA_SET_RANGE range;
};

TrimInput input{};
input.attributes.Size = sizeof(input.attributes);
input.attributes.Action = DeviceDsmAction_Trim;
input.attributes.DataSetRangesOffset =
    offsetof(TrimInput, range);
input.attributes.DataSetRangesLength = sizeof(input.range);
input.range.StartingOffset = offset;
input.range.LengthInBytes = length;
```

After DeviceIoControl succeeds, require `unmap > before.unmap` within 5 seconds and read back zeros. A successful IOCTL without an observed real provider UNMAP is a test failure.

- [ ] **Step 6: Run unit tests only**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^unit\.m1\.raw_protocol_io$"
git diff --check
```

Expected: fake API sequence and all failures pass without elevation or a real disk.

- [ ] **Step 7: Commit**

```powershell
git add src/m1/m1_raw_protocol_io.h src/m1/m1_raw_protocol_io.cpp tests/unit/m1_raw_protocol_io_tests.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: verify real M1 range operations"
```

---

### Task 16: Build the M1-C lifecycle executable and structured report

**Files:**

- Create: `src/m1/m1_cli.h`
- Create: `src/m1/m1_cli.cpp`
- Create: `src/m1/m1_lifecycle_runner.h`
- Create: `src/m1/m1_lifecycle_runner.cpp`
- Create: `src/m1/m1_report.h`
- Create: `src/m1/m1_report.cpp`
- Create: `src/m1/main.cpp`
- Create: `tests/unit/m1_cli_tests.cpp`
- Create: `tests/unit/m1_lifecycle_runner_tests.cpp`
- Create: `tests/unit/m1_report_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing report tests**

Require schema version 1 and exact top-level fields:

```text
schema_version, mode, run_id, started_utc, finished_utc, success,
exit_code, last_stage, driver, baseline, provider, created_device,
raw_protocol, cleanup, primary_error, cleanup_errors
```

Provider evidence must include all six request counters, per-type request-error counters, fatal errors, cancellation, timeout, final state, bounded structured diagnostics, and native object path. Cleanup must include exact remove attempted/succeeded, device absent, provider stopped, names released, AIM baseline match, PhysicalDrive baseline match, and service restoration status.

- [ ] **Step 2: Observe missing report failure**

Run:

```powershell
cmake --build --preset debug --target myrd_m1_report_tests
```

Expected: compile fails because M1 report types are absent.

- [ ] **Step 3: Implement report serialization and atomic publication**

Reuse `src/poc/json_writer.h` and `src/poc/report_publisher.h` by linking only the M1 E2E/report target to `myrd_poc`. `myrd_proxy`, `myrd_storage`, and the reusable `myrd_m1` provider target must not depend on `myrd_poc`. Do not move or duplicate the JSON/publisher implementation in M1.

The M1 report must preserve both internal `Error.code` and `Error.native_code`. Each diagnostic contains only sequence, request code, offset, length, operation, and error fields. Serialization must never include disk data or the user-supplied report path.

- [ ] **Step 4: Publish injectable lifecycle boundaries**

`m1_lifecycle_runner.h` defines narrow interfaces for provider creation/start/stop, kernel list/create/query/remove/wait, PhysicalDrive snapshot/locate, and raw protocol verification. The concrete wrappers live in `main.cpp`; tests use fakes. The runner accepts a `M1RunConfig`, returns an in-memory `M1Report`, and never writes files or changes service state.

- [ ] **Step 5: Implement the exact lifecycle in `m1_lifecycle_runner.cpp`**

`run_m1_lifecycle()` performs:

1. require elevation and acquire `Global\myrd-m1-e2e-mutex` without waiting;
2. capture AIM device IDs and PhysicalDrive numbers;
3. discover/query `phdskmnt.sys` directly;
4. create/start `M1Provider` and wait Ready;
5. call `DirectAimAdapter::create_proxy_disk()` with the provider native path, 64 MiB, 512-byte sectors, and SHM proxy flags;
6. record the exact returned DeviceId and confirm it by query;
7. rescan and locate only the newly owned PhysicalDrive;
8. call `verify_m1_raw_protocol()` and require READ, WRITE, ZERO, and UNMAP counters;
9. remove only the exact returned DeviceId and wait absent;
10. stop provider, poll named-object absence using the M0 proven delayed-release strategy, and capture final counters;
11. recapture baselines and require equality;
12. return a success report only when all gates pass.

- [ ] **Step 6: Implement failure cleanup with ownership proof**

All exits after CREATE must enter one cleanup path. Store `std::optional<DeviceId> owned_id`; reject `kAllDevices` and `kAutoDeviceNumber`; remove only `owned_id`; stop provider after device disappearance/CLOSE; collect cleanup failures without hiding the primary failure.

Debug-only failure points are:

```text
provider_ready
create_returned
device_confirmed
physical_drive_confirmed
write_verified
zero_verified
unmap_verified
device_removed
```

Release builds reject failure injection arguments.

- [ ] **Step 7: Implement and test the narrow CLI**

Cover missing command/report, duplicate options, existing report refusal, relative path below `reports`, device namespace rejection, Debug failure-point acceptance, and Release rejection.

`main.cpp` parses `run --report <new-path>`, gathers runtime/dependency evidence, creates concrete adapters, invokes `run_m1_lifecycle()`, atomically publishes exactly one report, and maps report exit code to process exit code. It must refuse to overwrite an existing report.

- [ ] **Step 8: Run all non-elevated tests**

Run:

```powershell
cmake --build --preset debug
ctest --preset debug -R "^(unit\.m1\.|unit\.report|unit\.lifecycle_runner)$"
git diff --check
```

Expected: all pass; `myrd-m1-e2e.exe` is built but not yet run against the driver.

- [ ] **Step 9: Commit**

```powershell
git add src/m1/m1_cli.h src/m1/m1_cli.cpp src/m1/m1_lifecycle_runner.h src/m1/m1_lifecycle_runner.cpp src/m1/m1_report.h src/m1/m1_report.cpp src/m1/main.cpp src/CMakeLists.txt tests/unit/m1_cli_tests.cpp tests/unit/m1_lifecycle_runner_tests.cpp tests/unit/m1_report_tests.cpp tests/CMakeLists.txt
git commit -m "feat: add M1 driver lifecycle verifier"
```

---

### Task 17: Add strict M1 JSON validation and elevated harness safety

**Files:**

- Create: `tools/Verify-M1Json.ps1`
- Create: `tests/e2e/run-m1-proxy.ps1`
- Create: `tests/e2e/test-m1-harness-preflight.ps1`

- [ ] **Step 1: Write preflight tests before the harness**

The preflight test invokes the harness in child PowerShell processes and requires failure before mutation for: non-admin token, missing executable, report root outside `reports`, report-root traversal through a reparse point, existing report overwrite, concurrent mutex ownership, `phdskmnt` not running, and Release failure injection.

Each child test captures service state, AIM device list, and PhysicalDrive list before/after and requires equality.

- [ ] **Step 2: Observe missing-script failures**

Run:

```powershell
& .\tests\e2e\test-m1-harness-preflight.ps1
```

Expected: failure because the M1 harness does not exist.

- [ ] **Step 3: Implement strict report validation**

`Verify-M1Json.ps1` must reject malformed JSON, missing/extra semantic evidence, schema/version/mode mismatch, inconsistent success/exit code, missing exact device ownership, missing READ/WRITE/ZERO/UNMAP counters, cleanup false on a success report, forbidden AIM modules/processes, unchanged-report hash mismatch, and baseline mismatch.

For a successful normal report require:

```powershell
if (-not $r.success -or $r.exit_code -ne 0) { throw 'run failed' }
if ($r.provider.info_count -lt 1 -or
    $r.provider.read_count -lt 1 -or
    $r.provider.write_count -lt 1 -or
    $r.provider.zero_count -lt 1 -or
    $r.provider.unmap_count -lt 1) {
    throw 'all real proxy operations were not observed'
}
if (-not $r.cleanup.exact_remove_succeeded -or
    -not $r.cleanup.device_disappeared -or
    -not $r.cleanup.provider_stopped -or
    -not $r.cleanup.named_objects_released -or
    -not $r.cleanup.aim_device_lists_match -or
    -not $r.cleanup.physical_drive_lists_match) {
    throw 'cleanup contract failed'
}
```

- [ ] **Step 4: Implement the elevated harness**

Mirror M0's proven safety boundaries:

- require administrator token;
- acquire `Global\myrd-m1-e2e-harness-mutex` non-blocking;
- verify `phdskmnt` running;
- capture exact `deviosvc` state/start mode/path/PID;
- reject forbidden AIM processes;
- stop `deviosvc` only for the test window;
- run M0 query-driver before and after as a read-only baseline;
- run the M1 executable for requested iterations;
- validate and hash each child report immediately;
- restore `deviosvc` in `finally`;
- verify child hashes again after restoration;
- publish one aggregate summary without rewriting child reports.

- [ ] **Step 5: Make preflight tests pass**

Run from a normal non-elevated shell:

```powershell
& .\tests\e2e\test-m1-harness-preflight.ps1
```

Expected: every safety scenario passes and no device/service state changes.

- [ ] **Step 6: Run local regressions**

Run:

```powershell
ctest --preset debug -R "^(unit\.m1\.|integration\.m1\.shm)$"
git diff --check
```

Expected: pass.

- [ ] **Step 7: Commit**

```powershell
git add tools/Verify-M1Json.ps1 tests/e2e/run-m1-proxy.ps1 tests/e2e/test-m1-harness-preflight.ps1
git commit -m "test: add safe M1 driver harness"
```

---

### Task 18: Add the tiered M1 verifier and dependency audit

**Files:**

- Create: `tools/Invoke-M1Verification.ps1`
- Create: `tests/unit/m1_verification_script_tests.ps1`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write command-list tests**

`Invoke-M1Verification.ps1 -Tier M1-V6 -ListCommands` must list every lower tier in order and include a non-empty CTest selection assertion. Tests must fail if a regex selects zero tests.

- [ ] **Step 2: Implement V0-V3**

V0 verifies pinned AIM commit, design/plan presence, M0 closure ancestry, and `git diff --check`. V1 configures/builds and runs every `unit.m1.*`. V2 runs protocol fuzz and local SHM 50 times. V3 runs the Release import audit and the existing M0 read-only query-driver without changing service state; the M1 V4-V6 harness separately proves operation with `deviosvc` stopped and restored.

- [ ] **Step 3: Implement V4-V6**

V4 invokes one Debug M1 lifecycle. V5 invokes every Debug failure point, validates cleanup for the expected failure, then performs one normal Debug lifecycle. V6 builds Release, audits imports, and invokes 20 Release lifecycles with exactly one newly created aggregate summary.

The Release import audit must reject `aimapi.dll`, `aim_ll`, RamDyn, AIM GUI binaries, `devio.dll`, and any unapproved binary from AIM Toolkit. Windows system libraries and this project's static code are allowed.

- [ ] **Step 4: Run non-elevated script tests and list commands**

Run:

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -ListCommands
& .\tests\unit\m1_verification_script_tests.ps1
```

Expected: complete command list and script tests pass without mutation.

- [ ] **Step 5: Run M1-V2 locally**

Run:

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -Configuration Debug
```

Expected: Debug build, all M1 unit tests, 10,000-case fuzz, and 50 local SHM repetitions pass.

- [ ] **Step 6: Commit**

```powershell
git diff --check
git add tools/Invoke-M1Verification.ps1 tests/unit/m1_verification_script_tests.ps1 tests/CMakeLists.txt
git commit -m "test: orchestrate M1 verification tiers"
```

---

### Task 19: Execute M1-C real-driver gates

**Files:**

- Evidence only under `reports/m1-*` (ignored or untracked)
- No source changes unless a gate exposes a reproducible defect

- [ ] **Step 1: Capture service baseline**

In an elevated PowerShell:

```powershell
Get-CimInstance Win32_Service -Filter "Name='phdskmnt' OR Name='deviosvc'" |
    Select-Object Name,State,StartMode,PathName,ProcessId
```

Expected: `phdskmnt` is Running. Record `deviosvc` state/start mode before the harness.

- [ ] **Step 2: Run M1-V3**

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V3 -Configuration Release
```

Expected: pinned contract, build/tests, Release dependency audit, and read-only driver query pass.

- [ ] **Step 3: Re-run one complete M0 lifecycle against the changed common libraries**

```powershell
& .\tools\Invoke-M0Verification.ps1 -Tier V4 -Configuration Debug
```

Expected: the frozen M0 provider still creates, serves, verifies, removes, and releases one real device after M1's shared interface changes.

- [ ] **Step 4: Run one Debug M1 lifecycle**

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V4 -Configuration Debug
```

Expected: one report proves INFO, READ, WRITE, ZERO, UNMAP, exact remove, object release, baseline equality, and service restoration.

- [ ] **Step 5: Run all cleanup failure points**

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V5 -Configuration Debug
```

Expected: every injected run fails at its named point while cleanup succeeds, followed by one normal pass.

- [ ] **Step 6: Run Release V6**

```powershell
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V6 -Configuration Release
```

Expected: exactly 20/20 successful Release lifecycles, no device/PhysicalDrive/name/thread/handle residue, no forbidden dependency, and service baseline restored.

- [ ] **Step 7: Handle any failure with a regression-first loop**

If a gate fails, stop the tier, preserve the report, reproduce at the lowest tier, add a deterministic unit/integration regression, implement the minimal fix, rerun M1-V2, then resume the failed real-driver tier. Never patch evidence or relax validators to convert a failure into a pass.

- [ ] **Step 8: Commit only reproducible fixes**

Each defect receives its own commit. If all gates pass without source changes, Task 19 has no commit.

---

### Task 20: Record M1 acceptance and final regression

**Files:**

- Create: `docs/validation/m1-proxy-acceptance.md`
- Modify: `README.md`

- [ ] **Step 1: Write the acceptance document from immutable evidence**

Record:

- tested commit and executable SHA-256;
- Windows build, CPU, RAM, driver version/hash/signature;
- pinned AIM source commit;
- M1-V0 through M1-V6 command results;
- exact 20-cycle aggregate path and hash;
- unit count, fuzz seed/case count, local SHM repetitions;
- real INFO/READ/WRITE/ZERO/UNMAP counters;
- before/after device and PhysicalDrive sets;
- named-object release and service restoration evidence;
- forbidden module/process audit;
- known M1 exclusions and the fact that data remains volatile;
- explicit `M1 GO` or `M1 NO-GO` conclusion.

- [ ] **Step 2: Update README without claiming M2-M4 features**

README must state that M1 is a volatile fixed-memory proxy productization milestone. It must not claim dynamic physical-memory commit, image loading, timed write-back, crash recovery, persistence, service management, or GUI.

- [ ] **Step 3: Run complete source verification one final time**

Run:

```powershell
& .\tools\Invoke-M0Verification.ps1 -Tier V2 -Configuration Debug
& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -Configuration Debug
cmake --build --preset release
ctest --preset release -R "^(unit\.|integration\.)"
git diff --check
```

Expected: M0 local regression, M1 local gate, and all Release tests pass.

- [ ] **Step 4: Audit the final diff against scope**

Run:

```powershell
git diff --stat 0e2b3b3...HEAD
rg -n "VirtualAlloc|MEM_RESERVE|MEM_COMMIT|VHD|checkpoint|journal|write.back|eviction" src/m1 src/proxy src/storage
```

Expected: no M2-M4 implementation. Mentions in comments describing exclusions are acceptable; executable behavior is not.

- [ ] **Step 5: Commit acceptance**

```powershell
git add docs/validation/m1-proxy-acceptance.md README.md
git commit -m "docs: record M1 proxy acceptance"
```

- [ ] **Step 6: Final plan alignment review**

Re-read the design sections 3.1, 4, 6.1-6.2, 7-8, 18-22, 23.1, 24.1, 25.1, 27 M1-A/B/C, and 28 M1 GO. For each requirement, cite a test, report field, or acceptance evidence line. If any requirement has no evidence, M1 remains NO-GO and this task is not complete.

---

## 5. Task-to-SPEC traceability

| SPEC requirement | Plan tasks |
|---|---|
| `IBlockDevice`, context, features, ranges | 1-2 |
| INFO/READ/WRITE/CLOSE ABI | 3, 7-9 |
| UNMAP/ZERO ABI and normalization | 3, 5, 9 |
| stable `errorno` and fatal/request split | 4, 7-9 |
| 10,000-case deterministic fuzz | 10 |
| random IDs and namespace policy | 11 |
| explicit DACL and object lifecycle | 11-12 |
| session Ready/Serving/Closing/Failed states | 13 |
| Cancel Event, timeout, response ordering | 12-13 |
| structured diagnostics and counters | 7-9, 13, 16-17 |
| local SHM and failure injection | 12-13, 17-18 |
| M0 GO prerequisite and no AIM user mode | 0, 17-19 |
| real driver INFO/READ/WRITE/ZERO/UNMAP | 15-19 |
| exact device and named-object cleanup | 16-19 |
| M1-A/M1-B/M1-C and final GO | 10, 13, 19-20 |

## 6. Stop conditions

Stop and mark the current gate failed when any of the following occurs:

- the pinned AIM wire ABI differs from the local definitions;
- M0 V2 regression no longer passes;
- unknown/truncated requests can call the backend or escape as exceptions;
- request validation permits overflow, misalignment, or out-of-device access;
- Response Event can be observed before complete response publication;
- cancellation, CLOSE, fatal error, or initialization failure leaves a thread/handle/name alive;
- M1 code removes an unowned or sentinel device ID;
- real all-zero writes do not produce request code 7, or real trim does not produce request code 6;
- a success report lacks any INFO/READ/WRITE/ZERO/UNMAP evidence;
- AIM Toolkit user-mode code is imported, loaded, or required;
- `deviosvc`, AIM devices, PhysicalDrives, or named objects differ from baseline after cleanup;
- any M2-M4 feature is added to make M1 pass.

Performance shortfalls alone do not authorize weakening correctness checks. M1 has no persistence; data loss after device removal or process termination is expected and must be stated plainly.
