# M0 Named-object Release Race Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the transient named-object cleanup race without weakening M0's per-cycle zero-residue gate.

**Architecture:** `M0Provider` remains the source of one exact probe. `LifecycleRunner` turns that probe into a bounded condition wait after provider shutdown, shared by normal and error cleanup paths. Unexpected probe errors fail immediately; persistent object presence fails at the deadline.

**Tech Stack:** C++20, Win32 named objects, MSVC/Ninja, CTest, PowerShell V6 harness.

---

### Task 1: Make the release probe distinguish presence from probe errors

**Files:**
- Modify: `src/poc/m0_provider.cpp`
- Test: `tests/unit/lifecycle_runner_tests.cpp`

- [x] **Step 1: Preserve the existing failing regression test**

Keep `lifecycle_retries_transient_named_object_release`, whose provider returns
false once and true on the next probe.

- [x] **Step 2: Verify the regression test is red**

Run:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset debug --target myrd_lifecycle_runner_tests
ctest --preset debug -R '^unit\.lifecycle_runner$' --output-on-failure
```

Expected: failure at `REQUIRE(r.success)` in
`lifecycle_retries_transient_named_object_release`.

- [x] **Step 3: Return unexpected Win32 errors from the provider probe**

For each mapping/event/mutex probe, return `success(false)` when the object can
be opened, continue when `GetLastError() == ERROR_FILE_NOT_FOUND`, and otherwise
return a Win32-domain failure carrying the operation and error code.

### Task 2: Add bounded condition waiting to lifecycle cleanup

**Files:**
- Modify: `src/poc/lifecycle_runner.cpp`
- Test: `tests/unit/lifecycle_runner_tests.cpp`

- [x] **Step 1: Add one shared wait lambda**

Poll `provider.named_objects_released()` every 100 milliseconds for at most 15
seconds. Set `report.cleanup.named_objects_released=true` only after a true
probe. On an unexpected probe error, append its exact code to `cleanup_errors`.
On timeout, append `WAIT_TIMEOUT` with operation
`wait_for_named_objects_released`.

- [x] **Step 2: Use the wait in both cleanup paths**

Replace both immediate probes: the cleanup lambda used after primary failures
and the normal path after `stop_and_release`.

- [x] **Step 3: Verify targeted and complete local tests**

Run Debug and Release lifecycle tests, then Debug and Release V2. Expected:
all selected tests pass and each V2 SHM integration completes 50/50.

### Task 3: Prove the real-driver race is resolved

**Files:**
- Modify after success: `docs/validation/m0-v6-20-cycle.md`
- Modify after success: `docs/validation/m0-driver-only-acceptance.md`

- [x] **Step 1: Run full V6 on master**

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
.\tools\Invoke-M0Verification.ps1 -Tier V6 -Configuration Release
```

Expected: V4 1/1, all V5 groups 1/1 and V6 20/20 with aggregate PASS.

- [x] **Step 2: Record only the new accepted evidence**

Update the validation documents with the new V6 summary and aggregate paths.
Do not cite the failed 8/9 run as accepted evidence; retain it under `reports/`
as diagnostic history.

- [ ] **Step 3: Commit the verified fix**

Commit source, regression test, design/plan and updated validation documents.
Do not add generated `out/`, `reports/` or pre-existing diagnostic files.
