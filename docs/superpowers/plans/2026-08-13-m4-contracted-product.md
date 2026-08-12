# MyRamDisk M4 Contracted Product Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. The project owner has prohibited subagent implementation for this milestone; the primary agent performs and reviews every step inline. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one `myrd run` executable that creates or recovers one AIM-backed NTFS RAM disk, checkpoints it while running, optionally compacts it after a bounded feasibility gate, and safely writes back and removes it on Ctrl+C.

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
- `m4-v1`: M3 adapter and optional online compaction tests;
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
| `src/run/run_loop.cpp` | Checkpoint, optional compaction, shutdown and one-second loop |
| `src/run/provision_volume.ps1` | Fixed GPT/single-partition/NTFS provisioning logic |
| `src/run/run_resources.rc` | Embed provisioning script and executable version resource |
| `src/run/main.cpp` | Create/recover composition, signal flag and reverse cleanup |

### New test and tool files

| File | Responsibility |
|---|---|
| `tests/unit/m4_cli_tests.cpp` | Parser forms, defaults, bounds and exit purity |
| `tests/unit/m4_run_policy_tests.cpp` | Due predicates and commit-outcome policy |
| `tests/unit/m4_windows_volume_tests.cpp` | Pure identity/extent/drive-letter/refusal helpers |
| `tests/unit/test-m4-provision-script.ps1` | Script parameter and forbidden-command audit |
| `tests/integration/m4_product_composition_tests.cpp` | Create/recover selection and cleanup ordering without real disk mutation |
| `tests/integration/m3_online_compaction_spike_tests.cpp` | R1 feasibility test; retained only on GO |
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
tests/CMakeLists.txt
README.md
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

**Files:**

- Create on GO: `tests/integration/m3_online_compaction_spike_tests.cpp`
- Modify on GO: `src/m3/m3_provider.h`
- Modify on GO: `src/m3/m3_provider.cpp`
- Modify on GO: `src/m3/m3_windows_acceptance.h`
- Modify on GO: `src/m3/m3_windows_acceptance.cpp`
- Modify on GO: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

This is a decision task, not an open-ended refactor.

- [ ] **Step 1: Freeze the provider-level RED**

Build the test by adapting the existing provider fixture from
`tests/integration/m3_provider_integration_tests.cpp`. The new test must:

```cpp
TEST_CASE(online_provider_compaction_commits_and_preserves_live_data) {
    // Open a generation-one provider with the existing fake transport.
    // Write block A, checkpoint A, write block B, then call provider.compact().
    // Require one newer committed generation and reads of both A and B.
    // Reopen through recover_complete_image and require the same bytes.
}
```

The test may reuse existing test support but may not add a fake management
record, revision, phase or callback hierarchy.

- [ ] **Step 2: Run RED**

```powershell
cmake --build --preset debug --target myrd_m3_online_compaction_spike_tests --parallel 1
```

Expected: compile failure because `M3Provider::compact` does not exist.

- [ ] **Step 3: Attempt the bounded provider method**

The only permitted public shape is concrete:

```cpp
struct M3CompactionOutcome final {
    persistence::CommitDisposition disposition{
        persistence::CommitDisposition::NotCommitted};
    std::uint64_t generation{};
    std::optional<Error> error{};
    [[nodiscard]] bool ok() const noexcept;
};

M3CompactionOutcome M3Provider::compact(
    const OperationContext& context,
    persistence::VolumeBarrierKind barrier);
```

`M3AimRuntime` may forward the same call. Reuse the existing
`CompactionCoordinator`, journal writer, manifest publication, commit resolver
and reconciliation. Do not copy their algorithms into `src/run`.

- [ ] **Step 4: Apply the GO/NO-GO checks before polishing**

Run:

```powershell
git diff --numstat 4f9f912 -- src/m3 tests/integration/m3_online_compaction_spike_tests.cpp
rg -n 'revision|profile|operation_store|strategy|manager|callback' `
  src/m3/m3_provider.* src/m3/m3_windows_acceptance.*
```

NO-GO if the solution needs coordinator public-interface changes, copied
compaction code, a new abstraction, fake revisions, or roughly more than 200
net production lines. On NO-GO:

1. save the RED and the concrete obstacle in `M4_TASKLIST.md`;
2. revert only the R1 experiment with exact patches;
3. mark R1 `BLOCKED_DECISION`;
4. stop all later tasks and request “independent M3 refactor” or “remove v1
   automatic compaction”.

- [ ] **Step 5: Verify GO if the bounds hold**

```powershell
cmake --build --preset debug --target myrd_m3_online_compaction_spike_tests --parallel 1
ctest --preset debug -R '^(integration\.m3\.online_compaction|integration\.m3\.compaction|integration\.m3\.provider)$' --output-on-failure
ctest --preset debug -L m3-v4 --output-on-failure
cmake --build --preset release --target myrd_m3_online_compaction_spike_tests --parallel 1
ctest --preset release -R '^integration\.m3\.online_compaction$' --output-on-failure
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

- [ ] **Step 6: Commit GO**

```powershell
git add src/m3/m3_provider.h src/m3/m3_provider.cpp `
  src/m3/m3_windows_acceptance.h src/m3/m3_windows_acceptance.cpp `
  tests/integration/m3_online_compaction_spike_tests.cpp tests/CMakeLists.txt `
  M4_TASKLIST.md
git commit -m "feat(m4): prove online compaction feasibility"
```

---

## Task R2: Implement the exact `myrd run` CLI

**Files:**

- Create: `src/run/cli.h`
- Create: `src/run/cli.cpp`
- Create: `tests/unit/m4_cli_tests.cpp`
- Modify: `src/run/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Write parser RED cases**

Freeze a table covering help, the default run form, every optional policy,
duplicate/missing/unknown options, size suffix boundaries, drive letters and
bundle device-path rejection. The public shape is:

```cpp
struct RunOptions final {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::chrono::seconds checkpoint_interval{600};
    std::uint64_t checkpoint_dirty_bytes{256ull * 1024 * 1024};
    std::uint64_t compact_threshold_bytes{512ull * 1024 * 1024};
    std::uint64_t ram_limit_bytes{2ull * 1024 * 1024 * 1024};
};

enum class CliAction : std::uint8_t { Run, Help, Error };
struct CliParseResult final {
    CliAction action{CliAction::Error};
    std::optional<RunOptions> options{};
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
    REQUIRE_EQ(parsed.options->virtual_bytes, 8ull * 1024 * 1024 * 1024);
    REQUIRE_EQ(parsed.options->checkpoint_interval, std::chrono::seconds(600));
    REQUIRE_EQ(parsed.options->drive_letter, L'R');
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

- [ ] **Step 4: Route `main` through the parser**

`main` must print help to stdout and return 0, print parser errors plus usage to
stderr and return 2, and temporarily print `run implementation unavailable` and
return 1 for a valid run form until R3.

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
  tests/unit/m4_cli_tests.cpp tests/CMakeLists.txt M4_TASKLIST.md
git commit -m "feat(m4): add minimal run command"
```

---

## Task R3: Expose the minimal M3 product runtime

**Files:**

- Modify: `src/m3/m3_provider.h`
- Modify: `src/m3/m3_provider.cpp`
- Modify: `src/m3/m3_windows_acceptance.h`
- Modify: `src/m3/m3_windows_acceptance.cpp`
- Modify: `src/m3/m3_aim_server_main.cpp`
- Modify: `src/run/main.cpp`
- Create: `tests/integration/m4_product_composition_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `M4_TASKLIST.md`

- [ ] **Step 1: Add RED tests for reusable production values**

Tests must require:

```cpp
const auto limits = m3::production_persistence_limits(8ull * kGiB);
const auto store = m3::production_store_options(2ull * kGiB);
REQUIRE_EQ(limits.max_virtual_bytes, 8ull * kGiB);
REQUIRE_EQ(store.soft_limit_bytes, 2ull * kGiB);
REQUIRE_EQ(store.hard_limit_bytes, 2ull * kGiB);
REQUIRE_EQ(store.eviction_low_watermark_bytes, 1ull * kGiB);
```

Add maintenance-stat tests that make one block dirty and require one storage
block of `dirty_bytes`, current generation and the selected journal committed
length.

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
M3MaintenanceStats maintenance_stats() const;
```

Use checked arithmetic. Replace the engineering AIM server's local default
constructors with calls to the shared value functions; do not change accepted
M3 default values except the explicit product RAM limit.

- [ ] **Step 4: Implement create/recover selection in `main`**

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

- [ ] **Step 5: Verify GREEN and M3 regression**

```powershell
cmake --build --preset debug --target myrd_m4_product_composition_tests myrd --parallel 1
ctest --preset debug -R '^(integration\.m4\.product_composition|integration\.m3\.(image_create|recovery|provider|windows_acceptance))$' --output-on-failure
ctest --preset debug -L m3-v2 --output-on-failure
ctest --preset debug -L m4-v1 --output-on-failure
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
```

- [ ] **Step 6: Commit**

```powershell
git add src/m3/m3_provider.h src/m3/m3_provider.cpp `
  src/m3/m3_windows_acceptance.h src/m3/m3_windows_acceptance.cpp `
  src/m3/m3_aim_server_main.cpp src/run/main.cpp `
  tests/integration/m4_product_composition_tests.cpp tests/CMakeLists.txt `
  M4_TASKLIST.md
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
enum class MaintenanceKind : std::uint8_t { Checkpoint, Compaction };
struct MaintenanceState final {
    std::chrono::steady_clock::time_point last_checkpoint{};
    std::chrono::steady_clock::time_point checkpoint_suppressed_until{};
    std::chrono::steady_clock::time_point compaction_suppressed_until{};
    bool skip_final_checkpoint{};
    bool exit_failure{};
};
```

Tests freeze exact threshold equality, no early trigger, checked dirty-byte
math, 60-second suppression, one retry for `NotCommitted`, convergence handling
and indeterminate controlled shutdown.

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
bool compact_due(const RunOptions&, const M3MaintenanceStats&,
                 const MaintenanceState&,
                 std::chrono::steady_clock::time_point) noexcept;
int run_loop(m3::M3AimRuntime&, ProductVolume&, const RunOptions&,
             std::atomic_bool& shutdown_requested);
```

Keep `do_checkpoint`, conditional `do_compact` and outcome handling private in
`run_loop.cpp`. Every persistence operation first calls `flush_volume`, which
revalidates the exact volume extent. Retry only once.

- [ ] **Step 4: Wire `main` into the loop**

After create/recover readiness, print one ready line containing drive letter and
generation, then call `run_loop`. Do not add JSON, IPC or background scheduler.

- [ ] **Step 5: Verify GREEN**

```powershell
cmake --build --preset debug --target myrd_m4_run_policy_tests myrd --parallel 1
ctest --preset debug -R '^(unit\.m4\.run_policy|integration\.m3\.(checkpoint|checkpoint_reconcile|compaction))$' --output-on-failure
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
  [ValidateSet('CreateKill','RecoverShutdown','Negative')]
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

CreateKill performs SPEC Section 12.2 steps 1-8 and exits only after killing the
exact recorded PID. RecoverShutdown runs after the operator/host reboots the VM
and performs steps 9-16. Negative runs the frozen refusal cases serially and
proves no baseline inventory or unrelated drive-letter change.

The script records line-oriented evidence under an operator-provided directory,
but product code has no evidence system.

- [ ] **Step 4: Add the verification route**

`tools/Invoke-M4Verification.ps1` supports:

```text
-Tier Local
-Tier VmCreateKill
-Tier VmRecoverShutdown
-Tier VmNegative
-Tier Final
```

`Local` runs m4 labels. VM tiers require explicit paths and safety switch.
`Final` consumes same-commit phase outputs, checks hashes and reports PASS/FAIL.

- [ ] **Step 5: Execute the real lifecycle**

Run CreateKill, reboot the VM, run RecoverShutdown, then Negative. Record exact
commands, source commit, executable SHA-256, bundle path, drive letter, before
and after inventories, file hashes and exit codes in the R7 TASKLIST record.

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
- create versus recover behavior;
- recovery never formats;
- checkpoint and conditional compaction behavior;
- Ctrl+C clean sequence;
- exact busy-volume Defender/Search guidance;
- forced-kill reboot requirement;
- v1 limitation and separately evaluated v2 identity decision point;
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
rg -n -i '\b(T'+'BD|TO'+'DO|FIX'+'ME)\b' src/run README.md `
  docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md `
  docs/superpowers/plans/2026-08-13-m4-contracted-product.md M4_TASKLIST.md
powershell -NoProfile -File tools/Measure-M4ProductLines.ps1
git diff --check
git status --short
```

Every hit must be classified. Product source must contain no prohibited old-M4
concept. Total product lines must be at most 2500.

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

M4 is GO only when R0-R8 are DONE, or when R1 is explicitly resolved by an owner
decision that amends the SPEC and removes compaction before R2 proceeds. No test
skip, previous-branch evidence, code-review assertion or nearly-complete result
substitutes for fresh command output.

The milestone ends here. There is no M5 plan.
