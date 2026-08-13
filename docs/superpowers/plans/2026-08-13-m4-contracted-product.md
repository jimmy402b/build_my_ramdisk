# MyRamDisk M4 Contracted Product Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to implement this plan task-by-task. For every R0-R8 implementation task, a fresh `gpt-5.6-terra` agent at medium reasoning implements the task; independent fresh Terra/medium agents then perform SPEC-compliance review and code-quality review. The implementer fixes review findings and the work is re-reviewed. The main Sol/xhigh controller uses CodeGraph for structural analysis and supplies the needed context, reads the diff, independently reruns verification, maintains task order, and makes exact commits. Do not run multiple implementation agents concurrently in the shared worktree. If implementation fails, the main controller first determines whether the task or plan is defective, context is missing, the task is too large, or implementation is difficult; pause and request or record a decision when the plan is wrong, and take over only when the task is sound but Terra cannot complete it. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one `myrd.exe` that creates or recovers one AIM-backed NTFS RAM disk with `myrd run`, checkpoints it while running, safely writes back and removes it on Ctrl+C, and compacts a stopped bundle with `myrd compact`.

**Architecture:** Start from M3 GO and compose the existing M3 provider, proxy, persistence engine and `M3AimRuntime` directly inside one foreground process. Add one pure CLI parser, one concrete Windows-volume module, one concrete run loop and one composition `main`; do not add a management process, IPC layer, durable M4 state or extensibility interfaces.

**Tech Stack:** C++20, MSVC x64, CMake/CTest, Win32 volume APIs, AIM driver, embedded Windows PowerShell 5.1 provisioning script, PowerShell E2E orchestration.

---

## 0. Execution rules

### 0.1 Fixed workspace and base

All implementation runs in:

```text
C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m4-contracted-product
```

Required branch and ancestry:

```text
feature/m4-contracted-product
4f9f912de56160062a007483fc56d5a02d5ff613
```

At the start of every task run:

```powershell
git branch --show-current
git merge-base --is-ancestor 4f9f912de56160062a007483fc56d5a02d5ff613 HEAD
git status --short
```

Expected: the contracted branch, ancestor check exit 0, and only explicitly
recorded changes. Never work in the root checkout on `master` and never edit the
historical `feature/m4-task23-restart` worktree.

### 0.2 MSVC environment

If CMake selects MinGW or `cl` cannot find standard headers, enter the existing
MSVC environment before diagnosing code:

```powershell
cmd /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc --fresh'
```

All build/test commands below assume that environment remains loaded in the
executing shell. If the agent shell cannot preserve it, prefix each command with
the same `call ... &&` sequence.

### 0.3 TDD and commit discipline

For every implementation task:

1. Add the smallest test that observes the missing behavior.
2. Run the exact target and record the real RED failure and exit code.
3. Implement only the behavior needed by that test.
4. Run focused Debug tests, then affected M3 labels, then task-specific Release
   tests.
5. Run `git diff --check` and the scope/prohibited-string scans.
6. Update the matching `M4_TASKLIST.md` execution record.
7. Stage exact paths only and commit with the task's fixed subject.

No `git add .`, broad wildcard staging, retry-to-green, assertion weakening or
timeout inflation is permitted.

### 0.4 Verification labels

New tests use these cumulative labels:

- `m4-v0`: branch/build/CLI/product-boundary tests;
- `m4-v1`: M3 product adapter, typed journal-capacity and offline compaction tests;
- `m4-v2`: Windows volume/provisioning tests;
- `m4-v3`: run-loop and shutdown tests;
- `m4-v4`: real VM product lifecycle.

M0-M3 labels retain their existing meaning and numbering.

### 0.5 Product line count

Create `tools/Measure-M4ProductLines.ps1` in R0. It counts nonblank,
non-comment physical lines in:

```text
src/run/**
net added lines in src/m3/m3_provider.h/.cpp
net added lines in src/m3/m3_windows_acceptance.h/.cpp
net added lines in src/m3/m3_aim_server_main.cpp
net added lines in src/m3/m3_lifecycle_runner.h/.cpp
net added lines in src/persistence/persistence_types.h
net added lines in src/persistence/journal_writer.h/.cpp
```

The script compares M3 GO with the worktree, prints per-file and total counts,
and exits 1 above 2500. Product PowerShell lines count. Tests, docs and purely
declarative CMake lines do not count.

---

## 1. Planned file map

### New production files

| File | Responsibility |
|---|---|
| `src/run/cli.h` | `RunOptions`, parse result and pure parser declaration |
| `src/run/cli.cpp` | Exact command grammar, defaults and bounds |
| `src/run/windows_volume.h` | Small value types and concrete volume functions |
| `src/run/windows_volume.cpp` | Exact disk/volume discovery, provisioning, mount, flush, lock and dismount |
| `src/run/run_loop.h` | Maintenance policy/state and concrete run-loop declarations |
| `src/run/run_loop.cpp` | Checkpoint, journal-capacity warning/error policy, shutdown and one-second loop |
| `src/run/provision_volume.ps1` | Fixed GPT/single-partition/NTFS provisioning logic |
| `src/run/run_resources.rc` | Embed provisioning script and executable version resource |
| `src/run/main.cpp` | Create/recover composition, offline-compact routing, signal flag and reverse cleanup |

### New test and tool files

| File | Responsibility |
|---|---|
| `tests/unit/m4_cli_tests.cpp` | Parser forms, defaults, bounds and exit purity |
| `tests/unit/m4_run_policy_tests.cpp` | Due predicates and commit-outcome policy |
| `tests/unit/m4_windows_volume_tests.cpp` | Pure identity/extent/drive-letter/refusal helpers |
| `tests/unit/test-m4-provision-script.ps1` | Script parameter and forbidden-command audit |
| `tests/integration/m4_product_composition_tests.cpp` | Create/recover/offline-compact selection and cleanup ordering without real disk mutation |
| `tests/e2e/Invoke-M4RunLifecycle.ps1` | One real AIM/NTFS create-kill-reboot-recover-shutdown lifecycle |
| `tools/Invoke-M4Verification.ps1` | Local/VM/final route with bounded inputs |
| `tools/Measure-M4ProductLines.ps1` | Product line budget gate |

### Existing files allowed to change

```text
CMakeLists.txt
src/CMakeLists.txt
src/m3/m3_provider.h
src/m3/m3_provider.cpp
src/m3/m3_windows_acceptance.h
src/m3/m3_windows_acceptance.cpp
src/m3/m3_aim_server_main.cpp
src/m3/m3_lifecycle_runner.h
src/m3/m3_lifecycle_runner.cpp
src/persistence/persistence_types.h
src/persistence/journal_writer.h
src/persistence/journal_writer.cpp
tests/CMakeLists.txt
tests/unit/m3_journal_writer_tests.cpp
tests/unit/m3_persistence_types_tests.cpp
tests/unit/m4_product_boundary_tests.ps1
README.md
docs/deferred/online-compaction.md
docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md
docs/superpowers/plans/2026-08-13-m4-contracted-product.md
M4_TASKLIST.md
```

Any additional production file requires a RED showing why one of the listed
single-purpose files cannot own the behavior, followed by a SPEC amendment
before code is changed.

---

## Task R0: Establish the clean product boundary

**Files:**

- Create: `src/run/main.cpp`
- Create: `tools/Measure-M4ProductLines.ps1`
- Create: `tests/unit/m4_product_boundary_tests.ps1`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Record the clean-base evidence**

Run:

```powershell
git rev-parse HEAD
git log --oneline --decorate -3
git merge-base --is-ancestor 5acc2896fec22f7e78245ab8c5dcdff9469a0eba HEAD
```

Expected: HEAD descends from M3 GO; the last ancestor command exits nonzero,
proving Task23 is not in product ancestry. Record both exit codes.

- [ ] **Step 2: Write the boundary RED test**

Create `tests/unit/m4_product_boundary_tests.ps1` with this frozen input/output
contract:

```powershell
param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$help = & $Executable '--help' 2>&1
if ($LASTEXITCODE -ne 0) { throw 'myrd --help must exit 0' }
if (($help -join "`n") -notmatch '^Usage: myrd run ') {
    throw 'contracted product usage is missing'
}
$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Executable))
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
$forbidden = @('MyRamDiskService','MyRamDiskHost','operation_store',
    'profile_registration','public_pipe_server','worker_supervisor')
foreach ($token in $forbidden) {
    if ($ascii.Contains($token)) { throw "forbidden product token: $token" }
}
Write-Output 'M4 product boundary PASS'
```

Register target `myrd_m4_product_boundary_tests` and CTest
`unit.m4.product_boundary` with label `m4-v0`.

- [ ] **Step 3: Run RED**

```powershell
cmake --preset msvc
cmake --build --preset debug --target myrd_m4_product_boundary_tests --parallel 1
```

Expected: FAIL because target `myrd` or its help contract does not exist.

- [ ] **Step 4: Add the minimal executable**

Create `src/run/main.cpp` as a temporary R0 shell:

```cpp
#include <string_view>
#include <iostream>

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::wcout << L"Usage: myrd run --bundle <path> --size <N> "
                      L"--drive-letter <R:> [options]\n";
        return 0;
    }
    std::wcerr << L"Usage: myrd run --bundle <path> --size <N> "
                  L"--drive-letter <R:> [options]\n";
    return 2;
}
```

Add `myrd` to `src/CMakeLists.txt`, link only accepted M0-M3 libraries and
Windows libraries already needed by `M3AimRuntime`, and enable warnings.

- [ ] **Step 5: Add the line-budget script**

`tools/Measure-M4ProductLines.ps1` must accept:

```powershell
param(
  [string]$Base = '4f9f912de56160062a007483fc56d5a02d5ff613',
  [int]$Maximum = 2500
)
```

It must enumerate the fixed paths in Section 0.5, count added nonblank physical
lines, print `path=count`, print `TOTAL=<N> MAXIMUM=2500`, and exit 1 when total
exceeds the maximum. Add a self-test that invokes `-Maximum 0` and requires
failure.

- [ ] **Step 6: Verify GREEN**

```powershell
cmake --build --preset debug --target myrd_m4_product_boundary_tests --parallel 1
ctest --preset debug -R '^unit\.m4\.product_boundary$' --output-on-failure
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

Expected: boundary PASS, line count below 2500, diff check exit 0.

- [ ] **Step 7: Commit exact files**

```powershell
git add src/run/main.cpp src/CMakeLists.txt tests/CMakeLists.txt `
  tests/unit/m4_product_boundary_tests.ps1 tools/Measure-M4ProductLines.ps1 `
  M4_TASKLIST.md
git commit -m "build(m4): establish contracted product boundary"
```

---

## Task R1: Decide online compaction feasibility

**Result:** NO-GO, experiment removed, owner decision resolved.

The historical RED and bounded experiment are preserved in `M4_TASKLIST.md`.
The spike proved that live-provider compaction needs either a duplicate M3
publication/resolution/reconciliation implementation or an independently
designed shared transaction-finalization refactor. No experimental production
or test code remains.

The owner approved the SPEC amendment on 2026-08-13:

1. M4 v1 has no online or automatic compaction.
2. R2 and R3 add the supported offline product command
   `myrd compact --bundle <path>` by reusing `run_m3_lifecycle`.
3. R5 warns once at 80% of the 2048 MiB journal ceiling and stops checkpoint
   retries after a typed journal-capacity failure.
4. `docs/deferred/online-compaction.md` records the independent future work and
   R1 root cause without adding a v1 placeholder.

This decision completes R1 and unblocks R2. It does not authorize an online
method, coordinator API change or M3 shared refactor inside M4 v1.

---

## Task R2: Implement the exact product CLI

**Files:**

- Create: `src/run/cli.h`
- Create: `src/run/cli.cpp`
- Create: `tests/unit/m4_cli_tests.cpp`
- Modify: `src/run/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/m4_product_boundary_tests.ps1`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Write parser RED cases**

Freeze a table covering help, the default run form, the exact offline compact
form, every run policy, duplicate/missing/unknown options, size suffix
boundaries, drive letters and bundle device-path rejection. The public shape is:

```cpp
struct RunOptions final {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::chrono::seconds checkpoint_interval{600};
    std::uint64_t checkpoint_dirty_bytes{256ull * 1024 * 1024};
    std::uint64_t ram_limit_bytes{2ull * 1024 * 1024 * 1024};
};

struct CompactOptions final {
    std::filesystem::path bundle;
};

enum class CliAction : std::uint8_t { Run, Compact, Help, Error };
struct CliParseResult final {
    CliAction action{CliAction::Error};
    std::optional<RunOptions> run{};
    std::optional<CompactOptions> compact{};
    std::wstring message;
};

CliParseResult parse_cli(std::span<const std::wstring_view> args);
```

Representative frozen tests:

```cpp
TEST_CASE(cli_default_run_contract) {
    const auto parsed = parse({L"run", L"--bundle", L"C:\\disk",
        L"--size", L"8G", L"--drive-letter", L"R:"});
    REQUIRE_EQ(parsed.action, CliAction::Run);
    REQUIRE_EQ(parsed.run->virtual_bytes, 8ull * 1024 * 1024 * 1024);
    REQUIRE_EQ(parsed.run->checkpoint_interval, std::chrono::seconds(600));
    REQUIRE_EQ(parsed.run->drive_letter, L'R');
}

TEST_CASE(cli_offline_compact_contract) {
    const auto parsed = parse({L"compact", L"--bundle", L"C:\\disk"});
    REQUIRE_EQ(parsed.action, CliAction::Compact);
    REQUIRE_EQ(parsed.compact->bundle, std::filesystem::path(L"C:\\disk"));
    REQUIRE_EQ(parse(compact_with_run_option).action, CliAction::Error);
}

TEST_CASE(cli_rejects_duplicate_and_device_paths) {
    REQUIRE_EQ(parse(duplicate_bundle).action, CliAction::Error);
    REQUIRE_EQ(parse(globalroot_bundle).action, CliAction::Error);
    REQUIRE_EQ(parse(ads_bundle).action, CliAction::Error);
}
```

- [ ] **Step 2: Run RED**

```powershell
cmake --build --preset debug --target myrd_m4_cli_tests --parallel 1
```

Expected: compile failure because `src/run/cli.h` does not exist.

- [ ] **Step 3: Implement the pure parser**

Use `std::from_chars` for numbers, checked multiplication for suffixes and exact
duplicate tracking. Command and option names are exact lowercase ASCII and
case-sensitive; drive letters accept either ASCII case and normalize to
uppercase. Do not access the filesystem, registry, driver or volume APIs.
Return only actionable usage text; do not add error classes or JSON.

Update the R0 boundary test's exact help/error contract to the final product
grammar. Global help lists exactly the `run` and `compact` forms; `run --help`
and `compact --help` each print only their exact subcommand usage. No-argument
invocation remains exit 2, stdout empty, with the exact global usage on stderr.
Freeze the exact global text from SPEC Section 3.1, including its two-space
indentation, line breaks and one trailing platform newline.

- [ ] **Step 4: Route `main` through the parser**

`main` must print help to stdout and return 0, print parser errors plus usage to
stderr and return 2, and temporarily print `run implementation unavailable` or
`compact implementation unavailable` and return 1 for the corresponding valid
form until R3.

- [ ] **Step 5: Verify GREEN and pre-mutation purity**

```powershell
cmake --build --preset debug --target myrd_m4_cli_tests myrd --parallel 1
ctest --preset debug -R '^(unit\.m4\.cli|unit\.m4\.product_boundary)$' --output-on-failure
& .\out\build\msvc\src\Debug\myrd.exe run --bundle C:\x --size bad --drive-letter R:
if ($LASTEXITCODE -ne 2) { throw 'expected usage exit 2' }
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

- [ ] **Step 6: Commit**

```powershell
git add src/run/cli.h src/run/cli.cpp src/run/main.cpp src/CMakeLists.txt `
  tests/unit/m4_cli_tests.cpp tests/unit/m4_product_boundary_tests.ps1 `
  tests/CMakeLists.txt M4_TASKLIST.md
git commit -m "feat(m4): add minimal product commands"
```

---

## Task R3: Expose the minimal M3 product runtime

**Files:**

- Modify: `src/m3/m3_provider.h`
- Modify: `src/m3/m3_provider.cpp`
- Modify: `src/m3/m3_windows_acceptance.h`
- Modify: `src/m3/m3_windows_acceptance.cpp`
- Modify: `src/m3/m3_aim_server_main.cpp`
- Modify: `src/m3/m3_lifecycle_runner.h`
- Modify: `src/m3/m3_lifecycle_runner.cpp`
- Modify: `src/persistence/persistence_types.h`
- Modify: `src/persistence/journal_writer.h`
- Modify: `src/persistence/journal_writer.cpp`
- Modify: `src/run/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/m3_journal_writer_tests.cpp`
- Modify: `tests/unit/m3_persistence_types_tests.cpp`
- Modify: `tests/unit/m4_product_boundary_tests.ps1`
- Create: `tests/integration/m4_product_composition_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tools/Measure-M4ProductLines.ps1`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Add RED tests for reusable production values**

Tests must require:

```cpp
const auto limits = m3::production_persistence_limits(8ull * kGiB);
const auto store = m3::production_store_options(2ull * kGiB);
REQUIRE_EQ(limits.max_virtual_bytes, 8ull * kGiB);
REQUIRE_EQ(store.soft_limit_bytes, 1ull * kGiB);
REQUIRE_EQ(store.hard_limit_bytes, 2ull * kGiB);
REQUIRE_EQ(store.eviction_low_watermark_bytes, 512ull * kMiB);
REQUIRE(validate_dynamic_block_store_options(store).ok());
REQUIRE(store.soft_limit_bytes < store.hard_limit_bytes);
```

The same test matrix must freeze the complete product RAM contract, including
the accepted M2 ordering invariant:

```cpp
const auto minimum = m3::production_store_options(64ull * kMiB);
REQUIRE_EQ(minimum.hard_limit_bytes, 64ull * kMiB);
REQUIRE_EQ(minimum.soft_limit_bytes, 32ull * kMiB);
REQUIRE_EQ(minimum.eviction_low_watermark_bytes, 16ull * kMiB);
REQUIRE(validate_dynamic_block_store_options(minimum).ok());
REQUIRE(minimum.soft_limit_bytes < minimum.hard_limit_bytes);

const auto maximum = m3::production_store_options(1ull * kTiB);
REQUIRE_EQ(maximum.hard_limit_bytes, 1ull * kTiB);
REQUIRE_EQ(maximum.soft_limit_bytes, 512ull * kGiB);
REQUIRE_EQ(maximum.eviction_low_watermark_bytes, 256ull * kGiB);
REQUIRE(validate_dynamic_block_store_options(maximum).ok());
REQUIRE(maximum.soft_limit_bytes < maximum.hard_limit_bytes);

const auto unaligned = m3::production_store_options(65537ull * kKiB);
REQUIRE_EQ(unaligned.hard_limit_bytes, 64ull * kMiB);
REQUIRE_EQ(unaligned.soft_limit_bytes, 32ull * kMiB);
REQUIRE_EQ(unaligned.eviction_low_watermark_bytes, 16ull * kMiB);
REQUIRE(validate_dynamic_block_store_options(unaligned).ok());
REQUIRE(unaligned.soft_limit_bytes < unaligned.hard_limit_bytes);
```

For the default `2ull * kGiB`, the assertions above therefore require hard
2 GiB, soft 1 GiB and low watermark 512 MiB. `ram_limit` always names the
requested upper bound: the actual hard ceiling is its largest storage-block
aligned value no greater than the request. It must never be copied into both
M2 limits, and valid CLI K/M/G/T values are not rejected solely for lacking
storage-block alignment.

Add maintenance-stat tests that make one block dirty and require one storage
block of `dirty_bytes`, current generation and the selected journal committed
length. Add journal-writer tests with an optional absolute committed-end ceiling:
exact equality succeeds, crossing by one aligned record returns typed
`JournalCapacityExhausted`, and leaving the ceiling unset preserves current M3
behavior. Existing per-checkpoint byte/record work limits, allocation, snapshot
batch and host disk failures must retain their existing distinct codes.
Add a provider checkpoint regression that crosses a small configured absolute
ceiling and proves `M3CheckpointOutcome.error_context->code` remains exactly
`JournalCapacityExhausted`, with no generation or selected committed-length
advance.

Add offline product-compaction tests that require a generation-one bundle,
advance one committed generation, reopen with identical bytes, reject a
generation-zero bundle and fail without mutation while the M3 writer lock is
held. Freeze the exact success and writer-conflict messages from SPEC Section
9.5 and prove no output recommends the engineering `myrd_m3_e2e` executable.

- [ ] **Step 2: Run RED**

```powershell
cmake --build --preset debug --target myrd_m4_product_composition_tests --parallel 1
```

Expected: missing product default and maintenance-stat APIs.

- [ ] **Step 3: Add the concrete M3 values**

Add to `m3_provider.h`:

```cpp
persistence::PersistenceLimits production_persistence_limits(
    std::uint64_t virtual_bytes);
DynamicBlockStoreOptions production_store_options(std::uint64_t ram_limit);
struct M3MaintenanceStats final {
    std::uint64_t generation{};
    std::uint64_t dirty_bytes{};
    std::uint64_t journal_committed_bytes{};
};
Result<M3MaintenanceStats> maintenance_stats() const;
```

Use checked arithmetic. Replace the engineering AIM server's local default
constructors with calls to the shared value functions; do not change accepted
M3 default values except the explicit product RAM policy: hard equals the
largest storage-block-aligned value no greater than the explicit product RAM
request, soft is half that aligned hard value rounded down to a block, and low
watermark is one quarter rounded down to a block.

Add the same concrete `Result<M3MaintenanceStats> maintenance_stats() const`
forwarder to `M3AimRuntime`; it must call the owned provider and add no cache,
observer or interface. Add this field to `M3ProviderConfig`:

```cpp
std::optional<std::uint64_t> journal_committed_end_ceiling{};
```

Only product `main` sets it, to `2147483648`; engineering M3 composition leaves
the default unset. Tests may set a smaller finite value to exercise the exact
boundary without creating a 2 GiB fixture.

Update `Measure-M4ProductLines.ps1` and its isolated boundary fixture so every
newly allowed M3/persistence adaptation file is counted relative to M3 GO. The
counter must continue to fail closed when a fixed file is missing or binary.

Add `PersistenceErrorCode::JournalCapacityExhausted = 20` and its exact
`to_string` mapping. Add an optional absolute committed-end ceiling to
`JournalWriterOptions` and set it only from the optional provider field. Return
the new code only when the candidate checkpoint's final committed end would
cross that absolute ceiling. Equality succeeds. A rejected checkpoint may
leave only an uncommitted tail; it must not advance the selected manifest,
generation or durable committed length. Do not rename or reuse
`SnapshotResourceExhausted`; the existing per-operation `max_checkpoint_bytes`,
record count, memory, snapshot and bounded-workspace failures retain their
current semantics.

- [ ] **Step 4: Add the offline product-compaction adapter**

Add one concrete function to `m3_lifecycle_runner.h/.cpp`:

```cpp
M3LifecycleOutcome run_product_offline_compaction(
    const OperationContext& context,
    const std::filesystem::path& bundle_root);
```

It fixes manifest `disk.rdm`, first recovers the geometry, rejects generation
zero, derives `production_persistence_limits(virtual_bytes)` and finite product
store options by calling `production_store_options(2ull * kGiB)`, sets the
existing offline lifecycle's compact command and `RawTest` barrier internally,
and calls `run_m3_lifecycle`. The word `RawTest` does not appear in product help
or user input; it is an internal barrier for an already-stopped durable view
with no mounted filesystem to flush. Require `Committed`, cleanup success and a
successor generation. Do not copy any compaction, publication, resolver or
reconciliation code.

- [ ] **Step 5: Implement create/recover/compact selection in `main`**

Add concrete local helpers, not interfaces:

```cpp
enum class BundleMode { Create, Recover };
Result<BundleMode> classify_bundle(const std::filesystem::path& root);
Result<persistence::RecoveredImage> create_product_image(
    const OperationContext&, const RunOptions&);
```

`create_product_image` builds `GenerationZeroCreateRequest`, uses
`BCryptGenRandom`, manifest `disk.rdm`, fixed safe initial names and finite
limits, then requires returned generation zero. Recover calls
`recover_complete_image`, rejects generation zero and exact-size mismatch. Both
construct the same `M3AimProviderFactoryConfig` and `M3AimRuntime`.

Until R4, stop after the runtime reports its exact PhysicalDrive, call
`runtime.stop()` and return 1 with `Windows volume setup unavailable`.
For `CliAction::Compact`, call only `run_product_offline_compaction` with a
finite deadline, print one human success line containing the new generation,
print an actionable stderr failure otherwise, and never construct
`M3AimRuntime`.

Link the product executable to `myrd_m3_windows`; that existing library already
brings the M3 management/lifecycle implementation. Do not link the engineering
`myrd_m3_e2e` executable or route product input through its parser.

- [ ] **Step 6: Verify GREEN and M3 regression**

```powershell
cmake --build --preset debug --target myrd_m4_product_composition_tests myrd --parallel 1
ctest --preset debug -R '^(integration\.m4\.product_composition|unit\.m3\.journal_writer|unit\.m3\.persistence_types|integration\.m3\.(image_create|recovery|provider|compaction|windows_acceptance))$' --output-on-failure
ctest --preset debug -L m3-v2 --output-on-failure
ctest --preset debug -L m4-v1 --output-on-failure
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

- [ ] **Step 7: Commit**

```powershell
git add src/m3/m3_provider.h src/m3/m3_provider.cpp `
  src/m3/m3_windows_acceptance.h src/m3/m3_windows_acceptance.cpp `
  src/m3/m3_aim_server_main.cpp src/m3/m3_lifecycle_runner.h `
  src/m3/m3_lifecycle_runner.cpp src/persistence/persistence_types.h `
  src/persistence/journal_writer.h src/persistence/journal_writer.cpp `
  src/run/main.cpp src/CMakeLists.txt tests/unit/m3_journal_writer_tests.cpp `
  tests/unit/m3_persistence_types_tests.cpp `
  tests/unit/m4_product_boundary_tests.ps1 `
  tests/integration/m4_product_composition_tests.cpp tests/CMakeLists.txt `
  tools/Measure-M4ProductLines.ps1 M4_TASKLIST.md
git commit -m "feat(m4): expose minimal m3 product runtime"
```

---

## Task R4: Provision and recover the exact Windows volume

**Files:**

- Create: `src/run/windows_volume.h`
- Create: `src/run/windows_volume.cpp`
- Create: `src/run/provision_volume.ps1`
- Create: `src/run/run_resources.rc`
- Create: `tests/unit/m4_windows_volume_tests.cpp`
- Create: `tests/unit/test-m4-provision-script.ps1`
- Modify: `src/run/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Freeze identity/refusal RED cases**

Define only these public values/functions:

```cpp
struct ProductVolume final {
    std::wstring volume_guid_path;
    std::uint32_t physical_drive_number{};
    wchar_t drive_letter{};
};

Result<ProductVolume> provision_new_volume(
    const m3::M3AimRuntimeInfo&, wchar_t drive_letter,
    std::chrono::milliseconds timeout);
Result<ProductVolume> find_recovered_volume(
    const m3::M3AimRuntimeInfo&, wchar_t drive_letter,
    std::chrono::milliseconds timeout);
Result<void> flush_volume(const ProductVolume&);
Result<void> assign_drive_letter(ProductVolume&);
Result<void> remove_drive_letter(const ProductVolume&);
```

Unit tables must reject sentinel/wrong DeviceId evidence, wrong size/sector,
pre-existing/non-RAW partitions, boot/system disk, zero/multiple/mixed disk
extents and occupied drive letters. They must accept exactly one matching extent
and an already-correct mount point.

- [ ] **Step 2: Run RED**

```powershell
cmake --build --preset debug --target myrd_m4_windows_volume_tests --parallel 1
```

Expected: missing module/functions.

- [ ] **Step 3: Implement read-only native discovery first**

Use `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`, `IOCTL_DISK_GET_DRIVE_LAYOUT_EX`,
`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`, `GetVolumeInformationW`,
`GetVolumePathNamesForVolumeNameW` and the exact runtime PhysicalDrive number.
Every two-call Win32 sizing pattern has a finite cap. Ambiguity fails closed.

Run focused tests before adding mutation.

- [ ] **Step 4: Freeze and implement the provisioning script**

The embedded script accepts only:

```powershell
param([UInt32]$DiskNumber, [char]$DriveLetter)
$disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
if ($disk.PartitionStyle -ne 'RAW') { throw 'disk is not RAW' }
if ($disk.IsBoot -or $disk.IsSystem) { throw 'refusing boot/system disk' }
Set-Disk -Number $DiskNumber -IsOffline $false -ErrorAction Stop
Set-Disk -Number $DiskNumber -IsReadOnly $false -ErrorAction Stop
Initialize-Disk -Number $DiskNumber -PartitionStyle GPT -ErrorAction Stop
$partition = New-Partition -DiskNumber $DiskNumber -UseMaximumSize `
    -DriveLetter $DriveLetter -ErrorAction Stop
Format-Volume -Partition $partition -FileSystem NTFS `
    -NewFileSystemLabel 'MyRamDisk' -Confirm:$false -Force -ErrorAction Stop
```

The script test must parse it, require the exact cmdlet allowlist and reject
`Invoke-Expression`, `Start-Process`, network commands, path parameters,
partition deletion and disk clearing.

- [ ] **Step 5: Embed and invoke it safely**

Embed as `RCDATA`. Extract to a new temporary file opened with exclusive sharing,
launch the fixed system PowerShell path with `CreateProcessW`, redirect stdout and
stderr into bounded readers, enforce 120-second timeout, terminate only that
helper on timeout, and delete the temporary file. Do not pass bundle paths.

- [ ] **Step 6: Wire create and recover paths**

Create path calls `provision_new_volume`, re-discovers and revalidates the exact
volume, flushes it and requires the first M3 checkpoint to commit generation 1.
Recover path calls only `find_recovered_volume` and `assign_drive_letter`; no
code path from recover may invoke the provisioning resource.

- [ ] **Step 7: Verify GREEN and boundary scans**

```powershell
cmake --build --preset debug --target myrd_m4_windows_volume_tests myrd --parallel 1
ctest --preset debug -R '^(unit\.m4\.windows_volume|unit\.m4\.provision_script|integration\.m4\.product_composition)$' --output-on-failure
ctest --preset debug -L m4-v2 --output-on-failure
rg -n 'Initialize-Disk|New-Partition|Format-Volume' src/run
rg -n 'provision_new_volume' src/run/main.cpp
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

The manual call graph audit must show the provisioning call only under
`BundleMode::Create`.

- [ ] **Step 8: Commit**

```powershell
git add src/run/windows_volume.h src/run/windows_volume.cpp `
  src/run/provision_volume.ps1 src/run/run_resources.rc src/run/main.cpp `
  src/CMakeLists.txt tests/unit/m4_windows_volume_tests.cpp `
  tests/unit/test-m4-provision-script.ps1 tests/CMakeLists.txt M4_TASKLIST.md
git commit -m "feat(m4): provision and recover exact windows volume"
```

---

## Task R5: Run periodic persistence maintenance

**Files:**

- Create: `src/run/run_loop.h`
- Create: `src/run/run_loop.cpp`
- Create: `tests/unit/m4_run_policy_tests.cpp`
- Modify: `src/run/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Write due-policy and outcome RED tests**

Use one transient value only:

```cpp
struct MaintenanceState final {
    std::chrono::steady_clock::time_point last_checkpoint{};
    std::chrono::steady_clock::time_point checkpoint_suppressed_until{};
    bool journal_warning_printed{};
    bool checkpoints_disabled_for_full_journal{};
    bool skip_final_checkpoint{};
    bool exit_failure{};
};
```

Tests freeze exact threshold equality, no early trigger, checked dirty-byte
math, 60-second suppression, one retry for ordinary `NotCommitted`, convergence
handling and indeterminate controlled shutdown. They also freeze:

- ceiling `2147483648` and warning threshold `1717986919` bytes;
- no warning below the threshold and one process-lifetime warning at/above it;
- warning text contains the exact stopped-bundle command;
- `JournalCapacityExhausted` performs no retry, disables later checkpoints,
  marks final checkpoint skipped and requests failure exit after cleanup;
- other `SnapshotResourceExhausted` errors retain the ordinary retry path.
- maintenance-stat failure requests controlled failure shutdown, still permits
  the normal final checkpoint when commit state permits it, and exits 1 after
  cleanup.

- [ ] **Step 2: Run RED**

```powershell
cmake --build --preset debug --target myrd_m4_run_policy_tests --parallel 1
```

Expected: missing run-loop module.

- [ ] **Step 3: Implement pure policy and concrete operations**

Expose only:

```cpp
bool checkpoint_due(const RunOptions&, const M3MaintenanceStats&,
                    const MaintenanceState&,
                    std::chrono::steady_clock::time_point) noexcept;
bool apply_checkpoint_outcome(MaintenanceState&,
                              const m3::M3CheckpointOutcome&,
                              std::chrono::steady_clock::time_point,
                              bool retry_already_used) noexcept;
void mark_maintenance_stats_failure(MaintenanceState&) noexcept;
int run_loop(m3::M3AimRuntime&, ProductVolume&, const RunOptions&,
             std::atomic_bool& shutdown_requested);
```

Keep `do_checkpoint`, journal warning emission and outcome handling private in
`run_loop.cpp`; the two small pure policy functions only mutate transient
`MaintenanceState` and add no interface or persistent record. Every checkpoint
first calls `flush_volume`, which revalidates the exact volume extent. Retry an
ordinary `NotCommitted` only once. A typed journal-capacity result must bypass
that retry and every later checkpoint. `apply_checkpoint_outcome` returns true
only when the caller must make the single immediate ordinary retry;
`mark_maintenance_stats_failure` sets `exit_failure` without setting
`skip_final_checkpoint`, after which `run_loop` requests shutdown.

- [ ] **Step 4: Wire `main` into the loop**

After create/recover readiness, print one ready line containing drive letter and
generation, then call `run_loop`. Do not add JSON, IPC or background scheduler.

- [ ] **Step 5: Verify GREEN**

```powershell
cmake --build --preset debug --target myrd_m4_run_policy_tests myrd --parallel 1
ctest --preset debug -R '^(unit\.m4\.run_policy|unit\.m3\.(journal_writer|persistence_types)|integration\.m3\.(checkpoint|checkpoint_reconcile))$' --output-on-failure
ctest --preset debug -L m4-v3 --output-on-failure
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
rg -n 'scheduler|queue|thread_pool|operation_store|phase|evidence' src/run
git diff --check
```

- [ ] **Step 6: Commit**

```powershell
git add src/run/run_loop.h src/run/run_loop.cpp src/run/main.cpp `
  src/CMakeLists.txt tests/unit/m4_run_policy_tests.cpp tests/CMakeLists.txt `
  M4_TASKLIST.md
git commit -m "feat(m4): schedule persistence maintenance"
```

---

## Task R6: Implement safe Ctrl+C shutdown

**Files:**

- Modify: `src/run/windows_volume.h`
- Modify: `src/run/windows_volume.cpp`
- Modify: `src/run/run_loop.h`
- Modify: `src/run/run_loop.cpp`
- Modify: `src/run/main.cpp`
- Modify: `tests/unit/m4_windows_volume_tests.cpp`
- Modify: `tests/unit/m4_run_policy_tests.cpp`
- Modify: `tests/integration/m4_product_composition_tests.cpp`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Add shutdown-order RED tests**

Freeze this observable sequence in the composition test:

```text
lock
verify_extent
flush
final_checkpoint
dismount
remove_drive_letter
runtime_stop
```

Freeze busy behavior: lock failure produces no later action, clears the shutdown
request and returns to running. Freeze indeterminate behavior: skip final
checkpoint but continue locked dismount/cleanup and return failure.

- [ ] **Step 2: Run RED**

```powershell
cmake --build --preset debug --target myrd_m4_product_composition_tests --parallel 1
```

Expected: missing lock/dismount and shutdown behavior.

- [ ] **Step 3: Add concrete volume shutdown calls**

Add one concrete move-only resource value; this is ownership, not an
extensibility interface:

```cpp
class LockedVolume final {
public:
    LockedVolume(LockedVolume&&) noexcept;
    LockedVolume& operator=(LockedVolume&&) noexcept;
    ~LockedVolume() noexcept;
    Result<void> flush();
    Result<void> dismount();
private:
    // Owns the already-locked exact-volume UniqueHandle and identity.
};
struct VolumeLockAttempt final {
    bool busy{};
    std::optional<LockedVolume> locked{};
};
Result<VolumeLockAttempt> lock_volume(const ProductVolume&);
```

The implementation opens the exact volume GUID without sharing, calls
`FSCTL_LOCK_VOLUME`, verifies extents, flushes the same handle and later calls
`FSCTL_DISMOUNT_VOLUME`. A busy sharing/lock result maps only to `busy`; all
other errors fail. No code calls dismount without the held locked handle.

- [ ] **Step 4: Install the minimal console handler**

`main.cpp` owns one global `std::atomic_bool*` valid for the run-loop lifetime.
The `SetConsoleCtrlHandler` callback accepts Ctrl+C, Ctrl+Break, close, logoff and
shutdown events, performs only `store(true)`, and returns `TRUE`. It performs no
I/O or allocation.

- [ ] **Step 5: Implement `do_shutdown`**

On lock busy, print the exact actionable Defender/Search guidance, clear the
flag and resume the loop. On a locked volume, enforce the frozen order. Only all
success returns 0 and prints `Clean shutdown. Goodbye.`. Any required failure
continues safe cleanup where possible and returns 1.

- [ ] **Step 6: Verify GREEN**

```powershell
cmake --build --preset debug --target myrd_m4_product_composition_tests myrd --parallel 1
ctest --preset debug -R '^(unit\.m4\.(windows_volume|run_policy)|integration\.m4\.product_composition)$' --output-on-failure
ctest --preset debug -L m4-v3 --output-on-failure
rg -n -C 4 'FSCTL_DISMOUNT_VOLUME' src/run/windows_volume.cpp
rg -n 'TerminateProcess|DeleteFile.*bundle|cleanup command' src/run
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

- [ ] **Step 7: Commit**

```powershell
git add src/run/windows_volume.h src/run/windows_volume.cpp `
  src/run/run_loop.h src/run/run_loop.cpp src/run/main.cpp `
  tests/unit/m4_windows_volume_tests.cpp tests/unit/m4_run_policy_tests.cpp `
  tests/integration/m4_product_composition_tests.cpp M4_TASKLIST.md
git commit -m "feat(m4): shut down ram disk safely"
```

---

## Task R7: Prove the real product lifecycle

**Files:**

- Create: `tests/e2e/Invoke-M4RunLifecycle.ps1`
- Create: `tools/Invoke-M4Verification.ps1`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

This task uses the existing approved VM and real AIM driver. Build on the host,
copy only required artifacts, and do not recreate a compiler environment in the
VM.

- [ ] **Step 1: Write the E2E preflight RED**

`Invoke-M4RunLifecycle.ps1` parameters are explicit:

```powershell
param(
  [ValidateSet('CreateKill','RecoverShutdown','OfflineCompact','Negative')]
  [string]$Phase,
  [Parameter(Mandatory)][string]$Executable,
  [Parameter(Mandatory)][string]$Bundle,
  [Parameter(Mandatory)][string]$DriveLetter,
  [Parameter(Mandatory)][string]$StateFile,
  [switch]$SafetyAcknowledged
)
```

Without `SafetyAcknowledged`, every phase exits 77 before mutation. The state
file contains only test paths, hashes, PID, expected generation and inventory;
it contains no password or private key.

- [ ] **Step 2: Run RED on the VM**

Run the CreateKill phase with the product binary. Expected initial failure must
be the first real missing behavior in product/Windows composition, not a missing
artifact, password or harness path.

- [ ] **Step 3: Implement the three bounded phases**

CreateKill performs SPEC Section 12.2 steps 1-7 and exits only after killing the
exact recorded PID. RecoverShutdown runs after the operator/host reboots the VM
and performs steps 8-14. OfflineCompact runs only after clean shutdown, calls
the same Release `myrd.exe compact --bundle <path>`, requires a successor
generation with identical file hashes, and proves AIM/PhysicalDrive inventory
does not change. Negative runs the frozen refusal cases serially, including
writer-lock rejection of offline compact while `myrd run` owns the bundle, and
proves no baseline inventory or unrelated drive-letter change.

The script records line-oriented evidence under an operator-provided directory,
but product code has no evidence system.

- [ ] **Step 4: Add the verification route**

`tools/Invoke-M4Verification.ps1` supports:

```text
-Tier Local
-Tier VmCreateKill
-Tier VmRecoverShutdown
-Tier VmOfflineCompact
-Tier VmNegative
-Tier Final
```

`Local` runs m4 labels. VM tiers require explicit paths and safety switch.
`Final` consumes same-commit phase outputs, checks hashes and reports PASS/FAIL.

- [ ] **Step 5: Execute the real lifecycle**

Run CreateKill, reboot the VM, run RecoverShutdown, OfflineCompact, then
Negative. Record exact commands, source commit, executable SHA-256, bundle
path, drive letter, before and after inventories, file hashes and exit codes in
the R7 TASKLIST record.

- [ ] **Step 6: Verify local regressions after VM fixes**

```powershell
ctest --preset debug -L m4-v0 --output-on-failure
ctest --preset debug -L m4-v1 --output-on-failure
ctest --preset debug -L m4-v2 --output-on-failure
ctest --preset debug -L m4-v3 --output-on-failure
cmake --build --preset release --target myrd --parallel 1
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

- [ ] **Step 7: Commit**

```powershell
git add tests/e2e/Invoke-M4RunLifecycle.ps1 tools/Invoke-M4Verification.ps1 `
  tests/CMakeLists.txt M4_TASKLIST.md
git commit -m "test(m4): prove real run lifecycle"
```

---

## Task R8: Final regression, documentation and GO

**Files:**

- Modify: `README.md`
- Modify: `M4_TASKLIST.md`
- Modify if required for drift only: the SPEC and this PLAN

- [ ] **Step 1: Write user documentation**

README must contain:

- exact command and defaults;
- administrator/AIM/Windows requirements;
- clarification that offline compact does not discover or start AIM and needs
  only bundle filesystem access plus the exclusive writer lock;
- create versus recover behavior;
- recovery never formats;
- checkpoint behavior, 80% journal warning, full-journal failure behavior and
  the exact stopped-bundle `myrd compact --bundle <path>` recovery command;
- the ordinary clean route: close applications, press Ctrl+C, wait for `myrd`
  to exit, run `myrd compact --bundle "<the same bundle path>"`, then restart
  the original `myrd run` command;
- the full-journal route: press Ctrl+C, allow safe cleanup to finish with exit
  1, run that same compact command, then restart the original run command;
- the forced-kill route: reboot Windows first, then run the compact command and
  restart the original run command; never direct users to `myrd_m3_e2e`;
- an explicit statement that online compaction is deferred outside v1, with a
  link to `docs/deferred/online-compaction.md`;
- Ctrl+C clean sequence;
- exact busy-volume Defender/Search guidance;
- forced-kill reboot as a product limitation with no promised cleanup/identity
  follow-up in this final milestone;
- only exit codes 0, 1 and 2.

- [ ] **Step 2: Build Debug and Release from a fresh configure**

```powershell
cmake --preset msvc --fresh
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

Expected: all tests PASS. An environment-gated real VM test may be absent from
ordinary CTest, but R7 final evidence must independently PASS.

- [ ] **Step 3: Run formal cumulative labels**

```powershell
ctest --preset release -L m2.1-v0 --output-on-failure
ctest --preset release -L m2.1-v1 --output-on-failure
ctest --preset release -L m2.1-v2 --output-on-failure
ctest --preset release -L m2.1-v3 --output-on-failure
ctest --preset release -L m2.1-v4 --output-on-failure
ctest --preset release -L m2.1-v5 --output-on-failure
ctest --preset release -L m3-v1 --output-on-failure
ctest --preset release -L m3-v2 --output-on-failure
ctest --preset release -L m3-v3 --output-on-failure
ctest --preset release -L m3-v4 --output-on-failure
ctest --preset release -L m3-v5 --output-on-failure
ctest --preset release -L m3-v6 --output-on-failure
ctest --preset release -L m3-v7 --output-on-failure
ctest --preset release -L m4-v0 --output-on-failure
ctest --preset release -L m4-v1 --output-on-failure
ctest --preset release -L m4-v2 --output-on-failure
ctest --preset release -L m4-v3 --output-on-failure
```

Do not rerun historical destructive M3 VM scenarios; the complete local M0-M3
regression and R7's new product VM lifecycle are the final evidence required.

- [ ] **Step 4: Build product-only and scan seams**

```powershell
cmake -S . -B out/build/m4-off -G "Ninja Multi-Config" -DBUILD_TESTING=OFF
cmake --build out/build/m4-off --config Release --target myrd --parallel 1
rg -a -n 'MYRD_ENABLE_TEST_SEAMS|open_for_test|fixture|fault.phase|crash.model' `
  out/build/m4-off/src/Release/myrd.exe
```

Expected: build PASS and `rg` has no match.

- [ ] **Step 5: Run architecture, unfinished-marker and budget scans**

```powershell
rg -n -i 'service|host|worker|named.pipe|ipc|profile|operation.store|durable.phase|admission|revision|evidence' src/run
rg -n -i 'compact|compaction' src/run/run_loop.h src/run/run_loop.cpp
rg -n -i '\b(T'+'BD|TO'+'DO|FIX'+'ME)\b' src/run README.md `
  docs/deferred/online-compaction.md `
  docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md `
  docs/superpowers/plans/2026-08-13-m4-contracted-product.md M4_TASKLIST.md
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
git status --short
```

Every hit must be classified. Product source must contain no prohibited old-M4
concept. The run-loop compaction scan must have no match; offline compaction is
routed only by `main`. Total product lines must be at most 2500.

- [ ] **Step 6: Run final VM evidence verifier**

```powershell
powershell -NoProfile -File tools/Invoke-M4Verification.ps1 -Tier Final `
  -EvidenceDirectory $m4EvidenceDirectory
```

Before running, set `$m4EvidenceDirectory` to the exact already-created R7
evidence directory and record its resolved absolute path. Expected:
`M4 FINAL PASS` and exit 0.

- [ ] **Step 7: Complete the drift audit**

Map every SPEC requirement to R0-R8 and every changed production file to one
allowed responsibility. Confirm there is no planned M5 and no deferred feature
required for v1 usability. Mark all TASKLIST items DONE only with same-turn fresh
evidence.

- [ ] **Step 8: Commit final closure**

```powershell
git add README.md M4_TASKLIST.md
git commit -m "docs(m4): close contracted product milestone"
```

If SPEC or PLAN required factual corrections, stage those exact files too and
describe the drift correction in the R8 record. Do not amend earlier commits.

---

## 2. Final acceptance summary

M4 is GO only when R0-R8 are DONE. R1 is resolved by the approved v1 amendment:
no online compaction, supported offline product compaction, typed journal
capacity handling and the explicit external defer record. No test skip,
previous-branch evidence, code-review assertion or nearly-complete result
substitutes for fresh command output.

The milestone ends here. There is no M5 plan.
