# M3 Task42 Simulated Crash Evidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the frozen Task33 checkpoint-crash oracle into reproducible, independently verified Task42 simulated-crash evidence without claiming real process-kill, AIM/NTFS, or controlled power-cut coverage.

**Architecture:** A PowerShell runner validates the frozen crash matrix and phase registry, launches the existing C++ crash-model executable as one fresh child process with at least 10,000 deterministic schedules, preserves raw logs, and atomically publishes a JSON report. A separate verifier checks the report, source/artifact hashes, fixture counts, child-process result, coverage labels, and the mandatory `task42_complete=false` limitation. The aggregate `Crash` route remains BLOCKED unless the caller explicitly selects `-CrashMode Simulated`.

**Tech Stack:** PowerShell 5.1-compatible scripts, CMake/CTest, the existing C++ `myrd_m3_checkpoint_crash_model_tests` executable, JSON and SHA-256 evidence.

---

## Frozen scope and invariants

- Production persistence, M1/M2/AIM code, Task33 model code, and both frozen JSON fixtures are read-only.
- `tests/fixtures/m3/crash-matrix.json` must contain 32 unique scenarios.
- `tests/fixtures/m3/fault-phase-registry.json` must contain 59 unique phases: 54 kill, 10 short-write, and 1 torn-write phase.
- Every crash-matrix ID must exist in the registry.
- The child model must report exactly `6/6 passed`, exit `0`, and run with `MYRD_M3_CRASH_MODEL_SCHEDULES >= 10000`.
- A simulated report must contain `scope=simulated`, `milestone_eligible=false`, `task42_complete=false`, `real_process_kill=false`, `real_aim_ntfs=false`, and `controlled_power_cut=false`.
- `Invoke-M3Verification.ps1 -Tier Crash` without explicit `-CrashMode Simulated` remains BLOCKED with exit `77`.

### Task 1: Freeze verifier behavior with a RED fixture test

**Files:**
- Create: `tests/e2e/test-m3-crash-verifier.ps1`
- Planned implementation: `tools/Verify-M3CrashJson.ps1`
- Modify later: `tests/CMakeLists.txt`

- [x] **Step 1: Write a valid simulated report fixture in a unique temporary directory.**

The fixture contains schema `myrd.m3.crash-evidence.v1`, `PASS`, simulated-only limitation flags, exact source/executable/fixture hashes, 32/59/54/10/1 counts, 10,000 schedules, child exit `0`, `6/6 passed`, raw-log SHA-256 values, and successful cleanup.

- [x] **Step 2: Invoke the absent verifier and observe RED.**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/e2e/test-m3-crash-verifier.ps1
```

Expected: nonzero because `tools/Verify-M3CrashJson.ps1` does not exist.

- [x] **Step 3: Add negative fixture mutations.**

The test must reject at least: `task42_complete=true`, `milestone_eligible=true`, `real_process_kill=true`, schedule count below 10,000, scenario count mismatch, child exit nonzero, missing `6/6 passed`, source mismatch, executable hash mismatch, fixture hash mismatch, raw-log hash mismatch, and failed cleanup.

### Task 2: Implement the independent JSON verifier

**Files:**
- Create: `tools/Verify-M3CrashJson.ps1`
- Test: `tests/e2e/test-m3-crash-verifier.ps1`

- [x] **Step 1: Implement fail-closed property, hash, count, and boolean helpers.**

The verifier accepts `Path`, `ExpectedSourceCommit`, `ExpectedExecutableSha256`, `ExpectedMatrixSha256`, `ExpectedRegistrySha256`, `StdoutPath`, and `StderrPath`. Every required property is checked explicitly; unknown extra properties are tolerated for forward-compatible diagnostics.

- [x] **Step 2: Enforce simulated-only semantics.**

Require `status=PASS`, `terminal_verdict=PASS`, `scope=simulated`, `milestone_eligible=false`, `task42_complete=false`, `simulated_file_clone=true`, `fresh_process=true`, and all three real-environment flags false.

- [x] **Step 3: Enforce executable result and raw-log provenance.**

Require 10,000 or more schedules, exactly 32/59/54/10/1 fixture counts, child exit `0`, process-exited cleanup, summary `6/6 passed`, and SHA-256 matches for both raw-log files.

- [x] **Step 4: Run the verifier test and observe GREEN.**

Expected: all valid and negative cases pass the test harness, which exits `0`.

### Task 3: Freeze runner validation and execution with RED tests

**Files:**
- Create: `tests/e2e/test-m3-crash-simulated.ps1`
- Planned implementation: `tests/e2e/Invoke-M3CrashMatrix.ps1`

- [x] **Step 1: Add a test accepting `Configuration` and exact crash-model executable path.**

The test creates a unique evidence directory, runs `-Phase Validate`, checks no child process was started, then runs `-Phase Run -ScheduleCount 10000`, invokes the independent verifier, and removes only its exact unique temporary directory in `finally`.

- [x] **Step 2: Observe RED because the runner is absent.**

Run the script with the current Debug crash-model executable. Expected: nonzero because `Invoke-M3CrashMatrix.ps1` does not exist.

- [x] **Step 3: Add fail-closed fixture tests.**

Copy fixtures into the unique temporary directory and mutate duplicate/missing IDs or count fields. `-Phase Validate` must return nonzero and must not start the crash-model executable.

### Task 4: Implement the simulated crash runner

**Files:**
- Create: `tests/e2e/Invoke-M3CrashMatrix.ps1`
- Test: `tests/e2e/test-m3-crash-simulated.ps1`

- [x] **Step 1: Validate all input paths and frozen fixtures before process start.**

Require existing executable/file/evidence paths, evidence outside the repository, SHA-256-compatible files, schema/count/unique-ID invariants, and every matrix ID in the registry. `Phase Validate` publishes `myrd.m3.crash-validation.v1` and exits without starting a child.

- [x] **Step 2: Launch one exact fresh child process with bounded timeout.**

Use `System.Diagnostics.ProcessStartInfo`, redirect stdout/stderr, set `MYRD_M3_CRASH_MODEL_SCHEDULES`, record PID/start/finish, wait no more than 180 seconds, and kill only that exact child on timeout. No shell interpolation or wildcard target is permitted.

- [x] **Step 3: Preserve raw output and publish report atomically.**

Write `m3-crash-model.stdout.txt`, `m3-crash-model.stderr.txt`, and `m3-crash-report.json` through same-directory unique temporary files followed by exact replace/rename. Report cleanup failure as FAIL.

- [x] **Step 4: Label coverage honestly.**

Set simulated-file-clone and fresh-process true only after a successful model run. Set real-process-kill, real-AIM/NTFS, controlled-power-cut, milestone eligibility, and Task42 completion false unconditionally in this runner.

- [x] **Step 5: Run Debug and Release focused tests.**

Expected: runner test passes for each configuration and leaves no `myrd-m3-crash-*` temporary directories.

### Task 5: Register tests and add explicit verification routing

**Files:**
- Modify: `tests/CMakeLists.txt`
- Modify: `tools/Invoke-M3Verification.ps1`
- Modify: `tests/e2e/test-m3-crash-simulated.ps1`

- [x] **Step 1: Register verifier and runner tests.**

Add `unit.m3.crash_verifier` and `integration.m3.crash_simulated_evidence`, pass `$<CONFIG>` and `$<TARGET_FILE:myrd_m3_checkpoint_crash_model_tests>`, label both `m3-v2`, and use a 180-second timeout for the runner.

- [x] **Step 2: Add `CrashMode` and `CrashExecutable` route arguments.**

`CrashMode` is `Full` or `Simulated`, default `Full`. `Tier Crash` with `Full` emits machine-readable BLOCKED/77. `Simulated` requires an existing evidence base and executable, invokes runner `Phase Run`, invokes the independent verifier with current commit and hashes, and prints `M3 Crash Simulated PASS; Task42 remains incomplete` on exit `0`.

- [x] **Step 3: Extend route tests.**

Assert default/full Crash remains BLOCKED/77 and no evidence run is created; simulated mode passes with a real model executable and leaves a verified report.

### Task 6: Document and record partial Task42 progress

**Files:**
- Modify: `docs/testing/m3-e2e.md`
- Modify: `M3_MICROTASKS.md`

- [x] **Step 1: Document commands and limitations.**

Explain the difference among simulated file-clone, real process-kill, real AIM/NTFS, and controlled power-cut. State that only the first is implemented and passing.

- [x] **Step 2: Mark only T42.1/T42.2 simulated subset evidence complete.**

Do not mark Task42, T42.3–T42.7, M3-V6, M3-V7, or final M3 GO complete.

### Task 7: Verification and commits

**Files:** all files above; production files remain unchanged.

- [x] **Step 1: Run syntax and focused tests.**

Parse every modified PowerShell file, run both new scripts directly, and run both CTest entries in Debug and Release.

- [x] **Step 2: Run the existing crash oracle independently.**

Run `model.m3.checkpoint_crash` in Debug and Release and confirm `6/6 passed` with at least 10,000 schedules.

- [x] **Step 3: Run full Debug and Release CTest.**

Require zero failures. Preserve the two pre-existing line-ending-only files unstaged.

- [x] **Step 4: Run `git diff --check` and the M3 microtask audit.**

Require both to exit `0`.

- [ ] **Step 5: Commit in reviewable units.**

Commit verifier tests/verifier, runner tests/runner/routing, and documentation separately. Never stage `src/persistence/file_io.h` or `tests/support/test_main.cpp`.
