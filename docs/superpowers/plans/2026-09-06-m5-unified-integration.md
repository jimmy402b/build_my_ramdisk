# MyRamDisk M5 Unified Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate the existing profile, management, control, and host layers into a qualified multi-disk M5 service without introducing parallel persistence or IPC semantics.

**Architecture:** Reuse `profile.myrd`, `ServiceCore`, `ProfileRegistrationStore`, `OperationStore`, `WorkerSupervisor`, typed IPC, and `MyRamDiskHost`. Implement only missing command wiring, auto-start orchestration, status projection, logging, and qualification evidence.

**Tech Stack:** C++20, MSVC x64, CMake/Ninja, Win32 SCM, existing named-pipe protocol, Catch2/CTest, Windows 11 VMware.

---

### Task 1: Baseline audit and integration seams

**Files:** existing `src/profile/*`, `src/management/*`, `src/control/*`, `src/host/*`; add `tests/model/m5_integration_inventory_tests.cpp` if a seam needs executable coverage.

- [ ] Inventory existing command variants, ServiceCore handlers, profile registration APIs, host launch/reattach APIs, and Windows service startup/shutdown actions.
- [ ] Add focused seam tests for missing operations only; do not duplicate existing M4 tests.
- [ ] Run all existing M4 model/security/host tests and record the baseline.
- [ ] Commit `test(m5): inventory existing management integration seams`.

### Task 2: Complete profile management commands

**Files:** `src/control/command.*`, `src/control/ipc_codec.*`, `src/cli/cli_parser.*`, `src/cli/cli_renderer.*`, `src/management/service_core.*`; tests `tests/unit/m5_profile_command_tests.cpp`, `tests/integration/m5_cli_profile_tests.cpp`.

- [ ] Add or finish typed command parsing for `disk list`, `disk create`, `disk import`, `disk remove/forget`, and `config set auto_start`, using existing field validation and profile formats.
- [ ] Route create/import through existing lifecycle registration and read-only bundle inspection; route remove through existing metadata-only forget semantics.
- [ ] Ensure `auto_start` updates are atomic profile revisions and never stop a running disk.
- [ ] Test duplicate IDs/bundles/mounts, malformed profiles, authorization, `--create` boundaries, and bundle retention.
- [ ] Commit `feat(m5): complete profile management commands`.

### Task 3: Complete service command orchestration

**Files:** `src/management/service_core.*`, `src/management/state_machine.*`, `src/management/worker_supervisor.*`; tests `tests/model/m5_service_orchestration_tests.cpp`.

- [ ] Add only a read-only aggregate status projection over existing lifecycle/runtime/activity/persistence/health states.
- [ ] Enforce same-profile operation serialization and allow independent profiles to progress concurrently.
- [ ] Verify `start`, `stop`, and `checkpoint` call existing worker/operation paths and preserve all checkpoint dispositions.
- [ ] Verify A failure cannot invoke B cleanup and that indeterminate checkpoint is never rendered as success.
- [ ] Commit `feat(m5): expose unified multi-disk service orchestration`.

### Task 4: Finish CLI and service-status rendering

**Files:** `src/cli/cli_parser.*`, `src/cli/cli_renderer.*`, `src/run/main.cpp`; tests `tests/unit/m5_cli_tests.cpp`, `tests/integration/m5_cli_service_tests.cpp`.

- [ ] Implement `disk start/stop/checkpoint`, `disk enable/disable` as existing config updates, and `service status`, preserving `run` and `compact` behavior.
- [ ] Map existing IPC/service errors to stable CLI exit classes without inventing a second protocol.
- [ ] Provide text and JSON output containing profile ID, mount, state, generation, persistence disposition, and safe error category.
- [ ] Commit `feat(m5): complete disk cli and status output`.

### Task 5: Complete Windows service auto-start and shutdown integration

> **Architecture gate:** This task is blocked until the prerequisites in `docs/superpowers/specs/2026-09-11-m5-task5-architecture-resolution.md` are completed. Do not directly attach `PublicPipeServer` to the current `ProductServiceActions` until persistence authority, a production execution backend, and a real checkpoint commit boundary are unified.

**Files:** `src/management/windows_service.*`, `src/management/service_main.cpp`, `src/management/windows_worker_recovery.*`; tests `tests/integration/m5_windows_service_tests.cpp`.

- [ ] Complete prerequisite 5-A: unify `ProfileStore`/`ProfileRegistrationStore`/`OperationStore`/runtime lease truth for real M4 profiles; no empty `service_state.myrs` projection.
- [ ] Complete prerequisite 5-B: implement a production `IWorkerControl`/execution adapter that delegates the complete start/checkpoint/stop/reconciliation sequence to existing `ProductServiceActions`/`MyRamDiskHost`.
- [ ] Complete prerequisite 5-C: extend existing private Host IPC with prepare/commit/abort checkpoint control backed by real M4 commit-boundary evidence; never synthesize pre-commit evidence from post-commit replies.
- [ ] Only after 5-A--5-C pass, wire real profile discovery and `auto_start` launch through existing startup actions and host worker launch/reattach.
- [ ] Preserve driver preflight, service ordering, per-profile failure isolation, and bounded SCM restart behavior.
- [ ] Wire preshutdown/normal stop to existing best-effort checkpoint, reconciliation, and resource cleanup budgets.
- [ ] Start `PublicPipeServer` with the same production `ServiceCore` after startup gates pass; stop it before controlled service cleanup.
- [ ] Test LocalSystem registration, startup ordering, one-profile failure, timeout/degraded state, public-pipe query/mutation authorization, and no auto-format/driver mutation.
- [ ] Commit `feat(m5): complete windows service auto-start lifecycle` only after prerequisites and pipe integration pass.

### Task 6: Diagnostics and migration boundary

**Files:** existing service/operation logging locations; `README.md`, `docs/validation/m5-acceptance.md`; tests `tests/unit/m5_logging_tests.cpp`.

- [ ] Ensure UTC structured service/audit logs include request, operation, profile, worker identity, state transition, generation, error class, and cleanup outcome.
- [ ] Document that M4 `profile.myrd` import is read-only inspected before atomic registration; no JSON migration or bundle rewrite is added.
- [ ] Test log rotation/failure behavior only where the existing logger lacks coverage.
- [ ] Commit `feat(m5): complete diagnostics and import boundary`.

### Task 7: Five-stage qualification and release package

**Files:** `docs/validation/m5-acceptance.md`, `docs/validation/m5-vm-execution-report.md`, `release/m5/MANIFEST.txt`, `README.md`, `RELEASE_NOTES.md`, `tools/Verify-M5Evidence.ps1`.

- [ ] Execute M5-A single disk, M5-B dual disk isolation, M5-C boot recovery, M5-D normal shutdown/VMware power-off recovery, and M5-E release evidence.
- [ ] Record source/artifact hashes, profile/ACL/driver baseline, commands, generations, identities, residual scans, and human power boundaries.
- [ ] Require literal `M5 VM EVIDENCE PASS`; verifier remains read-only and never controls VMware/devices.
- [ ] Build Debug/Release, run M4 and M5 tests, verify version/hash/manifests, and commit `release(m5): qualify unified supervisor integration`.

## Plan self-review

- No task creates a new profile format, state machine, IPC transport, worker protocol, or checkpoint algorithm.
- Every M5 goal maps to Tasks 2–7; Task 1 prevents duplicate implementation.
- Tests are added only at missing integration seams; existing M4 tests remain the regression gate.
- No TODO/TBD placeholders or unbounded retry requirements remain.
