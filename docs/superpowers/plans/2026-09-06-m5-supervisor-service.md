# MyRamDisk M5 Supervisor and Windows Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add protected disk profiles, centralized multi-disk supervision, administrator CLI control, and a Windows service that restores enabled M4 bundles at boot without changing M4 persistence semantics.

**Architecture:** Preserve `myrd run` and `DiskRuntime` as the trusted single-disk core. `ProfileStore` owns strict metadata under `C:\ProgramData\MyRamDisk`; `DiskSupervisor` owns one isolated worker/runtime per profile; `MyRamDiskService` exposes local authenticated IPC and coordinates boot, stop, and preshutdown. Every worker owns only its profile's bundle, volume, drive letter, AIM identity, and cleanup ledger.

**Tech Stack:** C++20, MSVC x64, Win32 SCM/service APIs, named pipes, UTF-8 length-prefixed JSON, CMake/Ninja Multi-Config, CTest, Windows 11 VMware.

**Non-goals:** GUI, VSS/application-consistent snapshots, per-write synchronous persistence, remote/cloud features, encryption, driver installation/replacement, compression/replication, complex installer, and unbounded retries. Recovery remains “latest complete successful checkpoint”; later writes may be lost or retained.

---

## Task 1: Profile model, strict validation, and protected store

**Files:** create `src/m5/profile.{h,cpp}`, `src/m5/profile_store.{h,cpp}`, `tests/unit/m5_profile_tests.cpp`, `tests/unit/m5_profile_store_tests.cpp`; modify `src/CMakeLists.txt`, `tests/CMakeLists.txt`.

- [ ] Write RED tests for exact `myrd.profile.v1`: lowercase UUID v4, absolute local NTFS bundle path, 64 MiB–1 TiB size, `A:`–`Z:` letter, `auto_start`, checkpoint 5–86400 seconds, dirty threshold 1 MiB–1 TiB and ≤ size, positive revision, RFC3339 UTC timestamps. Reject unknown/duplicate fields, traversal/UNC/device/reparse/profile-root paths, duplicate IDs/bundles/letters. Tests use no real SCM/volume/filesystem.
- [ ] Run `ctest --test-dir out/build/m5 -C Debug -R '^unit\\.m5\\.(profile|profile_store)$'`; expect RED because APIs are absent.
- [ ] Implement:

```cpp
struct DiskProfile { Uuid profile_id; std::filesystem::path bundle_path; std::uint64_t size_bytes{}; wchar_t drive_letter{}; bool auto_start{}; std::chrono::seconds checkpoint_interval{}; std::uint64_t checkpoint_dirty_bytes{}; std::uint64_t revision{}; std::string created_utc; std::string updated_utc; };
Result<void> validate_profile(const DiskProfile&, std::span<const DiskProfile> existing);
Result<std::string> encode_profile_json(const DiskProfile&);
Result<DiskProfile> decode_profile_json(std::string_view);
```

- [ ] Implement `ProfileStore` under `C:\ProgramData\MyRamDisk\profiles`: SYSTEM/Administrators full control, ordinary users no write/delete/replace; same-directory random temp → flush → reopen/decode → atomic replace; optional `.bak` is never startup authority. `remove` retains bundle.
- [ ] Run focused tests, `git diff --check`, commit `feat(m5): add protected disk profile store`.

## Task 2: DiskSupervisor state machine and isolation

**Files:** create `src/m5/supervisor.{h,cpp}`, `src/m5/worker.{h,cpp}`, `tests/unit/m5_supervisor_tests.cpp`, `tests/unit/m5_worker_tests.cpp`.

- [ ] Write RED tests for Disabled/Configured/Starting/Recovering/Provisioning/Running/Checkpointing/Stopping/Stopped/Blocked/Failed; one mutating operation per profile; A failure cannot invoke B cleanup; indeterminate checkpoint is not success.
- [ ] Define injected seams:

```cpp
struct WorkerSnapshot { WorkerState state{}; std::optional<std::uint64_t> generation; std::optional<Error> last_error; };
class DiskSupervisor { public: Result<void> load_profiles(); Result<std::vector<WorkerSnapshot>> list() const; Result<void> start(std::string_view); Result<void> stop(std::string_view); Result<void> checkpoint(std::string_view); Result<void> enable(std::string_view,bool); Result<void> remove(std::string_view,bool keep_bundle); };
```

- [ ] Implement transitions and exact ownership handles; use Provisioning only for explicit create, Recovering for existing bundles, publish Running only after M4 identity/heartbeat checks, and retain Failed/Blocked diagnostics.
- [ ] Run M5 supervisor/worker tests and commit `feat(m5): add isolated disk supervisor state machine`.

## Task 3: IPC framing and authorization

**Files:** create `src/m5/m5_ipc.{h,cpp}`, `tests/unit/m5_ipc_tests.cpp`, `tests/unit/m5_authorization_tests.cpp`.

- [ ] Write RED tests for 4-byte little-endian length + UTF-8 JSON, 64 KiB cap, required schema/request_id/operation, truncation/duplicate ID/unknown operation/invalid UTF-8/oversize rejection, timeout and correlation.
- [ ] Implement `\\.\pipe\myrd-supervisor-v1`; query operations may be read by authenticated local users, mutations require impersonated local Administrators. Never trust client paths/PIDs/AIM numbers. Return exit classes 0 success, 1 product failure, 2 schema/argument, 3 service unavailable, 4 authorization, 5 timeout/unknown.
- [ ] Run IPC tests and commit `feat(m5): add authenticated supervisor ipc`.

## Task 4: Worker process and M4 bridge

**Files:** create `src/m5/worker_main.cpp`, `tests/integration/m5_worker_runtime_tests.cpp`; modify `src/run/main.cpp`, `src/run/disk_runtime.*`, `src/CMakeLists.txt`.

- [ ] Write RED integration tests proving one worker calls only M4 recovery for an existing bundle, reports PID/generation/volume identity/heartbeat, handles checkpoint/stop, and A/B failures are isolated.
- [ ] Implement private worker entry with validated profile and private control endpoint; construct existing M4 `RunOptions`; accept only supervisor checkpoint/stop; use a Job Object/exact process handle for bounded cleanup, never image-name killing.
- [ ] Run integration tests and commit `feat(m5): bridge supervised workers to m4 runtime`.

## Task 5: `myrd disk` CLI

**Files:** modify `src/cli/cli_parser.*`, `src/cli/cli_renderer.*`, `src/run/main.cpp`; create `tests/unit/m5_cli_tests.cpp`, `tests/integration/m5_cli_service_tests.cpp`.

- [ ] Write RED tests for `disk list`, `add --id --bundle --size --drive-letter [--auto-start] [--create]`, `remove [--keep-bundle]`, `enable`, `disable`, `start`, `stop`, `checkpoint`, and `service status`; preserve existing `run`/`compact` behavior.
- [ ] Map commands to framed IPC, render `myrd.disk.list.v1` and `myrd.service.status.v1`; only `--create` may create a bundle; remove retains bundle; enable/disable never stops a running disk.
- [ ] Run CLI tests and commit `feat(m5): add disk management cli`.

## Task 6: Windows Service lifecycle and boot recovery

**Files:** create `src/m5/service_main.cpp`, `src/m5/service_controller.{h,cpp}`, `tests/integration/m5_service_lifecycle_tests.cpp`.

- [ ] Write RED SCM-seam tests for LocalSystem registration, `phdskmnt` dependency check, profile ordering, 60-second start and 120-second preshutdown budgets, degraded state, no auto-format/repair, and continued cleanup after one profile fails.
- [ ] Implement service startup, IPC, auto_start recovery, `SERVICE_CONTROL_PRESHUTDOWN`, and one bounded SCM service restart. Do not install/modify/remove `phdskmnt` or auto-restart workers indefinitely.
- [ ] Run service tests and commit `feat(m5): add windows service lifecycle`.

## Task 7: Logging, migration, and rollback

**Files:** create `src/m5/m5_logging.{h,cpp}`, `tests/unit/m5_logging_tests.cpp`, `tests/integration/m5_migration_tests.cpp`.

- [ ] Write RED tests requiring UTC, service version, event, profile ID, state transition, generation, letter, error class, request ID and identity; test 10 MiB rotation/five files and LogWriteFailed; test valid M4 import, malformed profile/bundle rejection, remove/keep-bundle and re-import without bundle rewrite.
- [ ] Implement `C:\ProgramData\MyRamDisk\logs\service.jsonl` and `audit.jsonl`; logs are diagnostic only. Import validates M4 read-only recovery/geometry before atomic profile publication.
- [ ] Run tests and commit `feat(m5): add diagnostics and profile migration`.

## Task 8: Fault matrix, evidence verifier, and local gate

**Files:** create `tests/integration/m5_fault_matrix_tests.cpp`, `tests/e2e/m5_service_harness.ps1`, `tools/Verify-M5Evidence.ps1`; modify `README.md`, `docs/validation/m5-acceptance.md`, CMake files.

- [ ] Write RED tests for profile corruption, duplicate resources, occupied letter, driver unavailable, worker start/exit, all checkpoint dispositions, busy volume, IPC timeout, unauthorized mutation, and A-failure/B-running isolation; assert no cross-profile cleanup or bundle deletion.
- [ ] Implement read-only observer and offline verifier. Verifier requires M5-1…M5-9 PASS, source/artifact/profile/service hashes and zero unexplained residuals; it never controls VMware or devices.
- [ ] Run Debug/Release M5-labeled CTest and verifier tests; commit `test(m5): add fault matrix and evidence verifier`.

## Task 9: VMware qualification and release package

**Files:** modify `docs/validation/m5-acceptance.md`; create `docs/validation/m5-vm-execution-report.md`, `release/m5/MANIFEST.txt`; modify `README.md`, `RELEASE_NOTES.md`.

- [ ] Freeze final commit/artifact hashes and VM/driver/profile ACL baseline on persistent NTFS.
- [ ] Execute M5-1 profile registration/list, M5-2 explicit start/checkpoint/stop, M5-3 boot auto-recovery, M5-4 one-profile failure isolation, M5-5 query/mutation authorization, M5-6 preshutdown/normal restart, M5-7 worker failure/explicit restart, M5-8 manual VMware Power Off→Power On multi-disk recovery, and M5-9 corruption/migration rollback. Human power boundaries must be recorded.
- [ ] Assemble canonical evidence and require literal `M5 VM EVIDENCE PASS`; commit `docs(m5): record vm service qualification`.
- [ ] Build final Debug/Release, verify version remains `1.0.0`, package binaries/service registration/profile schema/driver prerequisite/evidence manifest, run smoke commands, verify hashes, and commit `release(m5): package supervised ramdisk service`.

## Plan self-review

- Profile, ACL, atomic update and migration: Tasks 1 and 7.
- Supervisor, worker ownership/isolation and M4 bridge: Tasks 2 and 4.
- IPC, authorization and CLI: Tasks 3 and 5.
- Service, boot, preshutdown and bounded cleanup: Task 6.
- Fault matrix, evidence and VM M5-1…M5-9: Tasks 8 and 9.
- Release packaging and version/hash handoff: Task 9.
- No TODO/TBD placeholders; every task has files, tests, commands/outcomes and commit boundary.


> Superseded by 2026-09-06-m5-unified-integration.md; do not implement this plan.
