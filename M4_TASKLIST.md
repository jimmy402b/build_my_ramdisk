# M4 Contracted Product TASKLIST

This is the execution control file for the final MyRamDisk milestone. It is not
an independent requirement source. Every task must follow, in order:

1. `docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md`
2. `docs/superpowers/plans/2026-08-13-m4-contracted-product.md`
3. this file
4. the accepted M0-M3 contracts

Implementation base and workspace:

```text
4f9f912de56160062a007483fc56d5a02d5ff613
C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m4-contracted-product
feature/m4-contracted-product
```

The historical `feature/m4-task23-restart` branch is not an implementation
dependency and must not be merged or cherry-picked wholesale.

---

## A. Execution contract

### A1. Subagent-driven implementation

The project owner has frozen M4 to subagent-driven execution. For every R0-R8
implementation task, assign a fresh `gpt-5.6-terra` agent at medium reasoning
as implementer. After it implements the task, assign independent fresh
Terra/medium reviewers first for SPEC compliance and then for code quality.
The implementer fixes every review finding and the work is re-reviewed.

The main Sol/xhigh controller performs structural analysis with CodeGraph and
gives the implementer the needed context, maintains sequence and the one-DOING
limit, reads the diff, independently reruns verification, updates this ledger,
and creates each exact task commit. Never run multiple implementation agents
concurrently in the shared worktree. If implementation fails, the controller
first determines whether the task or plan is defective, context is missing, the
task is too large, or the work is difficult. If the plan is wrong, pause and
request or record a decision; only take over implementation when the task is
sound but Terra cannot complete it.

### A2. Before a task

- [ ] Confirm the fixed worktree, branch, HEAD and status.
- [ ] Protect every pre-existing modification and untracked path.
- [ ] Read the full SPEC sections referenced by the task.
- [ ] Read the matching PLAN task and this task card.
- [ ] Require every dependency to be `DONE` with accessible evidence.
- [ ] Ensure the table has at most one `DOING` task.
- [ ] Change the task from `PENDING` to `DOING` and fill the start record.
- [ ] Assign the fresh implementer and reserve independent fresh SPEC-compliance and code-quality reviewers.
- [ ] Have the implementer add and run the focused RED before product implementation.

### A3. Before a task is DONE

- [ ] Run the PLAN's focused Debug verification.
- [ ] Run the task's cumulative M3/M4 labels.
- [ ] Run the required Release check.
- [ ] Run `tools/Measure-M4ProductLines.ps1`.
- [ ] Run product-boundary/prohibited-string and test-seam scans.
- [ ] Run `git diff --check` and `git status --short`.
- [ ] Map the actual files to the allowed-file list.
- [ ] Obtain independent SPEC-compliance and code-quality reviews; have the implementer fix findings and obtain re-review.
- [ ] Have the main controller read the diff and independently rerun the required verification.
- [ ] Record the implementer's RED and implementation, the reviewers' findings and re-review, the main-controller verification, and remaining exclusions.
- [ ] Update status to `DONE` only with fresh evidence.
- [ ] Have the main controller stage exact paths and create the fixed task commit.

### A4. Status values

- `PENDING`: not started.
- `DOING`: active; at most one task.
- `BLOCKED`: a concrete external/environmental blocker prevents progress.
- `BLOCKED_DECISION`: R1 found a product decision that only the owner can make.
- `DONE`: implementation, tests, record and exact commit are complete.

An ordinary test failure is not `BLOCKED`; keep the task `DOING` and debug it.

### A5. Stop conditions

Stop the current task without beginning the next if a change would:

- add Service, Host, Worker, named-pipe IPC, profile or operation-store code;
- add M4 durable phase, transition, admission, evidence or revision state;
- add an interface/manager/strategy for a single concrete call;
- copy M3 checkpoint or compaction durability algorithms;
- format a recovery-path disk;
- dismount after volume-lock failure;
- clean up an orphan AIM device without reboot-bound identity;
- exceed 2500 product lines;
- weaken an accepted M0-M3 contract;
- require an M5 for product usability.

Preserve the RED, document the exact conflict and request a scoped decision.

---

## B. Build, test and evidence conventions

### B1. Standard local commands

Run inside an MSVC x64 developer environment:

```powershell
cmake --preset msvc
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

If CMake selects MinGW or `cl` reports a missing standard header, load:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
```

Do not classify that environment failure as a code RED.

### B2. M4 labels

```powershell
ctest --preset release -L m4-v0 --output-on-failure
ctest --preset release -L m4-v1 --output-on-failure
ctest --preset release -L m4-v2 --output-on-failure
ctest --preset release -L m4-v3 --output-on-failure
```

`m4-v4` is the explicit real VM route; it is not silently skipped inside local
CTest.

### B3. Evidence record format

Each task record must use:

```text
Task / status:
Executor / UTC timestamp:
Start commit / branch / end commit:
Allowed files / actual files:
Dependencies and evidence:
RED command / observed failure / exit code:
Implementation summary:
Focused Debug verification / exit code:
Cumulative and Release verification / exit code:
Line count / architecture and seam scans:
VM evidence paths and SHA-256, if applicable:
SPEC/PLAN/TASKLIST drift audit:
Open risks / explicitly excluded follow-up:
Commit subject / commit hash:
```

No old-branch result, reviewer assertion or earlier-turn output counts as fresh
task evidence.

---

## C. Overall status

| ID | Task | Status | Depends on | Primary gate | Fixed commit subject |
|---|---|---|---|---|---|
| R0 | Clean product boundary | DONE | M3 GO | `m4-v0`, product ancestry and line counter | `build(m4): establish contracted product boundary` |
| R1 | Online compaction feasibility | PENDING | R0 | GO or `BLOCKED_DECISION`; `m3-v4`, `m4-v1` | `feat(m4): prove online compaction feasibility` |
| R2 | Exact `myrd run` CLI | PENDING | R1 GO or approved SPEC amendment | CLI matrix, `m4-v0` | `feat(m4): add minimal run command` |
| R3 | Minimal M3 product runtime | PENDING | R2 | create/recover selection, stats, `m3-v2`, `m4-v1` | `feat(m4): expose minimal m3 product runtime` |
| R4 | Exact Windows volume | PENDING | R3 | anti-misformat, no-format recovery, `m4-v2` | `feat(m4): provision and recover exact windows volume` |
| R5 | Persistence maintenance loop | PENDING | R4 | checkpoint/optional compact policy, `m4-v3` | `feat(m4): schedule persistence maintenance` |
| R6 | Safe Ctrl+C shutdown | PENDING | R5 | lock/flush/dismount order, busy refusal, `m4-v3` | `feat(m4): shut down ram disk safely` |
| R7 | Real AIM/NTFS lifecycle | PENDING | R6 | create-kill-reboot-recover-shutdown VM PASS | `test(m4): prove real run lifecycle` |
| R8 | Final regression and GO | PENDING | R7 | Debug/Release full, M0-M4 labels, OFF scan, <=2500 | `docs(m4): close contracted product milestone` |

No task after R1 may start while R1 is `BLOCKED_DECISION`.

---

## D. Task cards

## R0 — Clean product boundary

**SPEC:** Sections 1, 4, 6, 13.
**PLAN:** Task R0.
**Allowed files:**

```text
src/run/main.cpp
src/CMakeLists.txt
tests/CMakeLists.txt
tests/unit/m4_product_boundary_tests.ps1
tools/Measure-M4ProductLines.ps1
M4_TASKLIST.md
```

**Acceptance:**

- [x] Branch descends from M3 GO and does not descend from Task23.
- [x] `myrd.exe` exists and exposes only the temporary contracted help shell.
- [x] Product binary has no old-M4 component tokens.
- [x] Line-count tool fails closed above a supplied maximum and reports the
  real 2500-line gate.
- [x] `unit.m4.product_boundary` is GREEN in Debug and Release.
- [x] Exact commit exists.

**Execution record:**

```text
Task / status: R0 / DONE
Executor / UTC timestamp: fresh gpt-5.6-terra medium implementer / 2026-08-12T20:07:15Z
Start commit / branch / end commit: b66b812a42b25581335787975abc2b8cef7a408a / feature/m4-contracted-product / this R0 commit at Git HEAD
Allowed files / actual files: R0 frozen allowlist / src/run/main.cpp; src/CMakeLists.txt; tests/CMakeLists.txt; tests/unit/m4_product_boundary_tests.ps1; tools/Measure-M4ProductLines.ps1; M4_TASKLIST.md
Dependencies and evidence: M3 GO 4f9f912 is ancestor (exit 0); Task23 5acc289 is not ancestor (exit 1); start worktree clean before R0 ledger update
RED command / observed failure / exit code: `cmd /d /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --preset msvc && cmake --build --preset debug --target myrd_m4_product_boundary_tests --parallel 1'`; CMake generator expression `$<TARGET_FILE:myrd>` failed because target `myrd` did not exist; exit 1
Implementation summary: added the temporary `myrd --help` usage shell, focused PowerShell product-boundary assertion (bounded process, exact stdout/empty stderr, timeout cleanup, ASCII/UTF-16LE forbidden-token scan with self-audit, stderr-pollution and default/max-zero line-count self-checks), CMake custom target/CTest registration, and fail-closed M4 product-line counter; review repairs added language-aware comment handling, UTF-8/NUL/path guards, exact Git hunk counting without base-text reconstruction, cross-line C++ string/raw-string and PowerShell block/here-string handling, process cleanup hardening, and isolated regression fixtures including a five-file binary-base/no-hunk fail-closed proof
Focused Debug verification / exit code: MSVC Debug `myrd_m4_product_boundary_tests` target and `ctest --preset debug -R '^unit\.m4\.product_boundary$' --output-on-failure`; both exit 0
Cumulative and Release verification / exit code: MSVC Release `myrd_m4_product_boundary_tests` target and `ctest --preset release -R '^unit\.m4\.product_boundary$' --output-on-failure`; both exit 0; no full M3/VM route run per R0 exclusion
Line count / architecture and seam scans: main-controller fresh counter reported `src/run/main.cpp=12`, `TOTAL=12 MAXIMUM=2500` exit 0; `-Maximum 0` exit 1; invalid Base exit 1; Debug/Release product binary scan for old-M4 and representative test-seam tokens had no match; exact six-file allowlist and `git diff --check` passed
VM evidence paths and SHA-256, if applicable: not applicable to R0
SPEC/PLAN/TASKLIST drift audit: execution contract aligned by b66b812 and R0 sample include corrected; all SPEC findings and review-driven regression gaps were repaired and re-reviewed; final independent SPEC review APPROVED and final independent code-quality review APPROVED with no Critical/Important/Minor; main controller read the full diff and reran the required verification at 2026-08-13T04:12:21Z; no R1 or old-M4 content entered R0
Open risks / explicitly excluded follow-up: R0 is only a boundary/help shell; no M3 VM rerun and no R1 work; `vswhere.exe` emits a non-fatal vcvars warning after successful commands
Commit subject / commit hash: build(m4): establish contracted product boundary / this R0 commit at Git HEAD
```

## R1 — Online compaction feasibility

**SPEC:** Sections 7, 10.2.
**PLAN:** Task R1.
**Allowed files on GO:**

```text
src/m3/m3_provider.h
src/m3/m3_provider.cpp
src/m3/m3_windows_acceptance.h
src/m3/m3_windows_acceptance.cpp
tests/integration/m3_online_compaction_spike_tests.cpp
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] RED proves live provider lacks online compaction.
- [ ] One concrete provider/runtime call reuses existing M3 coordinator,
  publication, resolver and reconciliation.
- [ ] No algorithm copy, fake revision, durable M4 record or new abstraction.
- [ ] Production delta remains within the bounded spike.
- [ ] Online reopen test, M3 provider/compaction tests and `m3-v4` pass.
- [ ] If any bound fails, experiment is removed, evidence is recorded, task is
  `BLOCKED_DECISION`, and no later task starts.
- [ ] GO has the exact commit.

**Execution record:** Not started.

## R2 — Exact `myrd run` CLI

**SPEC:** Section 3.
**PLAN:** Task R2.
**Allowed files:**

```text
src/run/cli.h
src/run/cli.cpp
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m4_cli_tests.cpp
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] Only `myrd --help`, `myrd run --help` and the exact run form parse.
- [ ] Defaults and every numeric/path/drive-letter bound are frozen by tests.
- [ ] Duplicate, unknown, incomplete and device-path inputs exit 2 before
  filesystem, AIM or volume access.
- [ ] Output is human-only and stderr/stdout purity is tested.
- [ ] Debug/Release CLI and `m4-v0` pass.
- [ ] Exact commit exists.

**Execution record:** Not started.

## R3 — Minimal M3 product runtime

**SPEC:** Sections 8 and 9.1.
**PLAN:** Task R3.
**Allowed files:**

```text
src/m3/m3_provider.h
src/m3/m3_provider.cpp
src/m3/m3_windows_acceptance.h
src/m3/m3_windows_acceptance.cpp
src/m3/m3_aim_server_main.cpp
src/run/main.cpp
tests/integration/m4_product_composition_tests.cpp
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] Shared production persistence/store values preserve accepted M3 limits.
- [ ] Generation-zero create uses existing creator and independently reopens.
- [ ] Internal new-image generation zero bypasses only the existing-bundle
  product gate; an existing generation-zero bundle is rejected.
- [ ] Recover requires exact geometry.
- [ ] Maintenance stats use checked dirty-byte math and current journal truth.
- [ ] M3 `m3-v2`, provider, recovery and Windows acceptance pass.
- [ ] Exact commit exists.

**Execution record:** Not started.

## R4 — Exact Windows volume

**SPEC:** Sections 5, 9 and 11.2.
**PLAN:** Task R4.
**Allowed files:**

```text
src/run/windows_volume.h
src/run/windows_volume.cpp
src/run/provision_volume.ps1
src/run/run_resources.rc
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m4_windows_volume_tests.cpp
tests/unit/test-m4-provision-script.ps1
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] All seven pre-provision anti-misformat checks fail closed.
- [ ] Provision helper is fixed, embedded, bounded and accepts only disk number
  and drive letter.
- [ ] Create produces exactly one GPT basic-data partition and NTFS volume on
  the exact runtime PhysicalDrive.
- [ ] First volume flush precedes committed generation-one checkpoint.
- [ ] Recover locates exact volume by disk extents and never calls provisioning.
- [ ] Occupied drive letters and ambiguous/mixed volumes are rejected.
- [ ] `m4-v2`, M3 Windows acceptance and product composition pass.
- [ ] Exact commit exists.

**Execution record:** Not started.

## R5 — Persistence maintenance loop

**SPEC:** Section 10.
**PLAN:** Task R5.
**Allowed files:**

```text
src/run/run_loop.h
src/run/run_loop.cpp
src/run/main.cpp
src/CMakeLists.txt
tests/unit/m4_run_policy_tests.cpp
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] One-second loop has no scheduler interface, queue or thread pool.
- [ ] Time and dirty threshold equality trigger checkpoint exactly.
- [ ] Journal threshold triggers compaction only after R1 GO.
- [ ] Every operation revalidates and flushes the exact volume first.
- [ ] `NotCommitted` retries once and suppresses 60 seconds after second failure.
- [ ] Reconciliation converges through existing M3 ownership.
- [ ] Indeterminate commit requests controlled failure shutdown and suppresses
  final checkpoint.
- [ ] `m4-v3` and affected M3 checkpoint/compaction tests pass.
- [ ] Exact commit exists.

**Execution record:** Not started.

## R6 — Safe Ctrl+C shutdown

**SPEC:** Section 11.
**PLAN:** Task R6.
**Allowed files:**

```text
src/run/windows_volume.h
src/run/windows_volume.cpp
src/run/run_loop.h
src/run/run_loop.cpp
src/run/main.cpp
tests/unit/m4_windows_volume_tests.cpp
tests/unit/m4_run_policy_tests.cpp
tests/integration/m4_product_composition_tests.cpp
M4_TASKLIST.md
```

**Acceptance:**

- [ ] Console handler only stores an atomic flag.
- [ ] Clean order is lock, extent check, flush, final checkpoint, dismount,
  mount removal and runtime stop.
- [ ] Dismount is unreachable without a held volume lock.
- [ ] Busy lock performs no cleanup mutation, prints Defender/Search guidance,
  clears the request and resumes running.
- [ ] Forced termination is documented as requiring Windows reboot.
- [ ] Only complete confirmed cleanup exits 0.
- [ ] Debug/Release `m4-v3` pass and exact commit exists.

**Execution record:** Not started.

## R7 — Real AIM/NTFS lifecycle

**SPEC:** Section 12.2.
**PLAN:** Task R7.
**Allowed files:**

```text
tests/e2e/Invoke-M4RunLifecycle.ps1
tools/Invoke-M4Verification.ps1
tests/CMakeLists.txt
M4_TASKLIST.md
```

**Acceptance:**

- [ ] Host-built Release artifacts and hashes are recorded.
- [ ] Create phase provisions exact GPT/NTFS and commits file A.
- [ ] Exact PID is force-killed and Windows is rebooted.
- [ ] Recover phase never invokes provisioning and verifies A.
- [ ] Ctrl+C commits D, safely dismounts and returns inventory to baseline.
- [ ] Third run verifies D.
- [ ] Negative matrix proves no overwrite, no letter theft, no gen-zero retry,
  no ambiguous recovery format, writer lock, driver preflight and busy refusal.
- [ ] Evidence contains no password, private key or secret.
- [ ] Final route says PASS and exact commit exists.

**Execution record:** Not started.

## R8 — Final regression and GO

**SPEC:** All sections.
**PLAN:** Task R8.
**Allowed files:**

```text
README.md
M4_TASKLIST.md
docs/superpowers/specs/2026-08-13-m4-contracted-product-design.md
docs/superpowers/plans/2026-08-13-m4-contracted-product.md
```

SPEC/PLAN edits are allowed only for factual drift discovered during final
audit, not for adding features.

**Acceptance:**

- [ ] README contains exact command, defaults, recovery no-format guarantee,
  busy-volume guidance, forced-kill reboot limitation and exit codes.
- [ ] Fresh Debug full CTest passes.
- [ ] Fresh Release full CTest passes.
- [ ] Formal M0-M3 labels and all local M4 labels pass.
- [ ] R7 same-commit VM final evidence passes.
- [ ] `BUILD_TESTING=OFF` product builds and contains no test seam.
- [ ] Architecture and unfinished-marker scans are clean.
- [ ] Product line count is at most 2500.
- [ ] No requirement is deferred to M5; no M5 is planned.
- [ ] Drift audit maps every SPEC requirement and changed product file.
- [ ] Exact final commit exists and M4 status is GO.

**Execution record:** Not started.

---

## E. Final GO checklist

- [ ] R0-R8 are DONE, including any explicit R1 owner decision and amended
  documents.
- [ ] One product executable and one resident MyRamDisk process are shipped.
- [ ] Create, periodic persistence, forced-kill/reboot recovery and Ctrl+C
  shutdown pass on real AIM/NTFS.
- [ ] Recovery never formats.
- [ ] Busy volume never dismounts.
- [ ] Old M4 management architecture is absent from product source and binary.
- [ ] Product implementation is at most 2500 lines.
- [ ] Full local regression and same-commit VM evidence are fresh.
- [ ] README states every v1 limitation.
- [ ] M4 is declared the final milestone; no M5 tasklist or plan exists.
