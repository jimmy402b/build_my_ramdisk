# M5 Lifecycle Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the qualified M5 data plane: a Windows service that manages independent M4 DiskRuntime workers through typed IPC, including registration, lifecycle, recovery, shutdown, and VMware qualification.

**Architecture:** Start from clean M4 cb3df08. Import and audit typed product/control/profile contracts from feature/m5-unified-integration, but rebuild management orchestration and worker execution around DiskRuntime rather than HostCore. Extend M3 only with a two-phase checkpoint adapter; M4 foreground checkpoint behavior remains unchanged.

**Tech Stack:** C++20, CMake/MSVC, Win32 SCM and named pipes, existing M3 persistence/AIM integration, Catch2/CTest, PowerShell VMware evidence tools.

**Historical branch policy:** `feature/m5-unified-integration` is the only direct M5
contract reference. `feature/m5-supervisor-service` is a superseded JSON-profile/
DiskSupervisor experiment; it is read-only reference material, not an implementation
source. Its `src/setup/driver_manifest.*` matches the shared baseline and its profile
implementation predates the unified branch's protected-directory and provisioning-identity
changes. Nothing from that branch may be copied without an explicit contract test and review.

---

## Constraints

- The only execution path is public pipe -> ServiceCore -> per-profile worker -> DiskRuntime -> M3 -> phdskmnt.
- M4 one-shot checkpoint keeps native id checkpoint_generation + 3902, which is candidate_generation + 3901.
- M5 two-phase checkpoint uses native id candidate_generation + 3902 modulo 2^64 and rejects zero.
- M5 P4 persists service operation UUID, candidate generation, native id, binding, manifest slot, and SHA-256 of the exact PreparedManifestCandidate bytes.
- Cross-session dirty epoch comparison is forbidden. Only P5.live_epoch >= P4.cut_epoch in the same authenticated worker session is valid.
- GUI is a separate post-qualification task; it is not part of data-plane acceptance.

## File map

| Area | Files |
| --- | --- |
| Contracts/profile | src/product/*, src/control/*, src/profile/*, src/CMakeLists.txt |
| Durable authority | src/management/service_operation_store.*, profile_registration_*, state_machine.*, service_core.* |
| Two-phase M3 | src/m3/m3_provider.*, src/persistence/checkpoint_coordinator.*, commit_resolver.* |
| Shared runtime | src/run/runtime_adapter.* (new), src/run/main.cpp, src/run/disk_runtime.* |
| Worker | src/host/m5_worker_protocol.* (new), m5_worker_session.* (new), m5_worker_main.cpp (new) |
| Service | src/management/execution_scheduler.*, service_composition.*, windows_service.*, service_main.cpp |
| CLI/pipe | src/cli/*, src/control/public_pipe_*, src/management/service_response_wire.* |
| Tests | tests/unit/m5_*, tests/model/m5_*, tests/integration/m5_*, tests/e2e/m5_* |

## Management layer file ownership

`src/management/` and `src/cli/` do not exist at `cb3df08`. Every file below is
**created** by the task named — ported from `feature/m5-unified-integration` or
rebuilt. No file is "modified into existence".

The split is measured, not assumed. The layer is 30 files / 26,310 lines. Its
entire execution coupling is **9 files** that `#include "src/host/*"` or
`"src/run/*"` (306 symbol references). Every other file has zero M3/driver/host
includes, and `service_core.cpp` contains no `host::`, `HostCore`, `host_main` or
`M3HostProvider` reference at all. ServiceCore already drives execution through
the `IWorkerControl` interface, whose **only** implementer is
`service_worker_control.h`. Decision A is therefore already this code's shape:
the duplicate HostCore engine is confined behind a single seam, and replacing
that seam removes it.

### Ported verbatim (19,228 lines) — execution-agnostic

| File | Lines | Owning task |
| --- | --- | --- |
| `operation_store.{h,cpp}` | 1,846 | Task 2 |
| `service_operation_store.{h,cpp}` | 212 | Task 2 |
| `profile_registration_codec.{h,cpp}` | 1,094 | Task 2 |
| `profile_registration_store.{h,cpp}` | 2,121 | Task 2 |
| `state_machine.{h,cpp}` | 1,276 | Task 2 |
| `service_core.{h,cpp}` | 8,806 | Task 2 port, Task 6 P4/P5 + dirty-truth edits |
| `worker_supervisor.{h,cpp}` | 1,318 | Task 7 |
| `service_response_wire.{h,cpp}` | 944 | Task 7 |
| `service_composition.{h,cpp}` | 466 | Task 7 |
| `execution_scheduler.{h,cpp}` | 315 | Task 7 |
| `windows_service_adapters.{h,cpp}` | 152 | Task 7 |
| `service_main.cpp` | 5 | Task 7 |
| `src/cli/{cli_parser,cli_renderer,management_dispatch}.{h,cpp}` | 673 | Task 7 |

`state_machine` is a pure allocation-free reducer contract (it includes only
`product_state.h` and `profile_types.h`); porting it brings the Blocker A fix
(`a45d5ce`) with it.

### Rebuilt (7,755 lines) — host-coupled, re-seated on DiskRuntime

These are the Decision A boundary. Each keeps its frozen contract (the
`IWorkerControl` interface, the durable phase/disposition types in
`service_core.h`, the private binding identity) and changes only its execution
implementation.

| File | Lines | HostCore refs | Owning task |
| --- | --- | --- | --- |
| `service_worker_control.{h,cpp}` | 1,532 | 10 | Task 5 |
| `service_worker_session.{h,cpp}` | 648 | 116 | Task 5 |
| `windows_worker_observation.h` | 46 | 3 | Task 5 |
| `windows_worker_recovery.{h,cpp}` | 875 | 45 | Task 5 |
| `windows_service.{h,cpp}` | 4,645 | 132 | Task 7 |

`service_worker_control` is the load-bearing one: it is the sole `IWorkerControl`
implementation, and it is where a DiskRuntime-backed worker replaces HostCore.
`windows_service` is listed as rebuilt rather than ported only because it reaches
`host_ipc.h` for worker launch; most of its 4,645 lines (SCM wiring, auto-start,
preshutdown budget) are host-agnostic and are expected to port with the launch
seam replaced — Task 7 confirms the final split and records it.

### Departure from the scope document

`docs/m4-m5-scope.md` §4 lists `ServiceCore` as "替换实现（冻结契约）". This plan
**ports `service_core.{h,cpp}` verbatim** and edits it only where the design spec
requires (Task 6: two-phase P4/P5, the §3.6 recovery table, dirty-truth stop).

Reason: the scope line treats ServiceCore as part of the duplicate engine, but
the measurement above shows it is not — it carries no execution coupling and
already dispatches through `IWorkerControl`. Porting it keeps the frozen durable
contract and inherits the F10/F11/F12 and Blocker A–D fixes that already landed
in it (`65d0a0d`, `d926354`, `8feb030`, `36b9707`, `a45d5ce`, `f2ba838`), which a
rewrite would have to re-derive. This is flagged here so the departure is visible
rather than silent.

## Task 0: Establish the clean M5 integration boundary

**Files:**
- Create: docs/validation/m5-transfer-manifest.md
- Modify: src/CMakeLists.txt, tests/CMakeLists.txt

- [ ] **Step 1: Create an isolated implementation worktree**

Run:

~~~
git worktree add .worktrees/m5-lifecycle -b feature/m5-lifecycle cb3df08
~~~

Expected: the worktree is clean and HEAD is cb3df08.

- [ ] **Step 2: Record the transfer policy**

Write docs/validation/m5-transfer-manifest.md. It must name feature/m5-unified-integration as source, allow only src/product, src/control, and src/profile for direct transfer, and explicitly prohibit importing HostCore, host_main, old ServiceWorkerControl, or old Windows-service composition.

- [ ] **Step 3: Add dependency-boundary checks**

In src/CMakeLists.txt, make management and CLI targets independent of M3/AIM/driver targets. Only the per-profile worker executable may link myrd_m4_run and myrd_m3_windows.

- [ ] **Step 4: Record the M4 baseline**

Run:

~~~
cmake --preset release
cmake --build out/build/msvc --config Release
ctest --test-dir out/build/msvc -C Release --output-on-failure
~~~

Record the exact result in the transfer manifest before changing source.

- [ ] **Step 5: Commit**

~~~
git add docs/validation/m5-transfer-manifest.md src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "chore(m5): establish lifecycle implementation boundary"
~~~

## Task 1: Transfer typed contracts and durable profile storage

**Files:**
- Create: audited src/product/*, src/control/*, src/profile/* from feature/m5-unified-integration
- Modify: src/CMakeLists.txt, tests/CMakeLists.txt
- Test: tests/unit/m4_profile_codec_tests.cpp, tests/unit/m4_profile_store_tests.cpp
- Deferred to Task 7: tests/unit/m5_profile_command_tests.cpp (needs `src/cli/cli_parser.h`), tests/unit/m5_service_response_codec_tests.cpp (needs `src/management/service_response_wire.h`)

- [ ] **Step 1: Add red compatibility tests**

Test valid MYRDPRF decode/round-trip, unsupported schema Blocked behavior without file mutation, malformed typed command rejection, and malformed public-pipe frame rejection.

- [ ] **Step 2: Run the red tests**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m4\.profile_(codec|store)|m5\.(profile_commands|service_response_codec)"
~~~

Expected: new M5 targets do not exist until the audited modules are added.

- [ ] **Step 3: Transfer contracts only**

Add the audited product, control, and profile sources; define myrd_product_contracts, myrd_product_ipc, myrd_profile, and public-pipe CMake targets. Do not add management or worker execution code.

- [ ] **Step 4: Diff both historical profile implementations before finalizing the transfer**

Run:

~~~powershell
git diff feature/m5-supervisor-service..feature/m5-unified-integration -- src/profile src/setup
~~~

Expected: no `src/setup` driver-manifest difference; only the unified branch's protected profile-directory API and provisioning identity validation differences require review. The supervisor branch's JSON profile and DiskSupervisor contracts must not enter this implementation.

- [ ] **Step 5: Verify fail-closed profile semantics**

Preserve atomic current/previous profile publication, protected ProgramData roots, and strict profile validation. Unknown schema must retain original bytes and report Blocked.

- [ ] **Step 6: Run focused tests and commit**

Run the Step 2 command; expected result is all selected tests pass. Then:

~~~
git add src/product src/control src/profile src/CMakeLists.txt tests
git commit -m "feat(m5): import typed contracts and durable profile storage"
~~~

## Task 2: Implement durable operation and registration authority

**Files:**
- Create (port): src/management/operation_store.{h,cpp}, service_operation_store.{h,cpp}, profile_registration_codec.{h,cpp}, profile_registration_store.{h,cpp}, state_machine.{h,cpp}, service_core.{h,cpp}
- Modify: src/CMakeLists.txt
- Test: tests/integration/m5_service_operation_store_tests.cpp, tests/integration/m5_profile_registration_store_tests.cpp, tests/model/m5_service_orchestration_tests.cpp

`service_core.{h,cpp}` ports here as the frozen durable contract, not as
execution code: the sole `IWorkerControl` implementation does not exist until
Task 5, so Task 2 tests ServiceCore's planning, gate and durable-state reducers
against a fake worker control.

- [ ] **Step 1: Write red store tests**

Cover stale CAS rejection, request-id replay, operation-gate ownership, profile-registration atomicity, and failed import retaining both bundle and missing registration.

- [ ] **Step 2: Define frozen checkpoint durable state**

Use:
~~~cpp
enum class CheckpointDurablePhase : std::uint8_t {
    stable, admitted, commit_boundary, evidence_recorded,
    committed_pending_epoch_confirmation, resolving,
};
~~~

Make P4 correlation persist operation UUID, image UUID, prior/candidate generation, native id, manifest slot, candidate digest, worker binding, and cut epoch.

- [ ] **Step 3: Implement stores**

Use one service-owned CAS publication path for .myop/.prev, service_state.myrs, and profile registration. Every transition loads the expected revision and rejects stale state.

- [ ] **Step 4: Implement import reservation**

The import worker retains an exclusive writer lease across validation and atomic registration publication, then hands it to the managed worker or releases it. Lease loss publishes no registration.

- [ ] **Step 5: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m5\.(service_operation_store|profile_registration_store|service_orchestration)"
~~~

Then:

~~~
git add src/management tests/integration/m5_service_operation_store_tests.cpp tests/integration/m5_profile_registration_store_tests.cpp tests/model/m5_service_orchestration_tests.cpp
git commit -m "feat(m5): add durable operation and registration authority"
~~~

## Task 3: Expose the two-phase M3 checkpoint adapter

**Files:**
- Modify: src/m3/m3_provider.{h,cpp}, src/persistence/checkpoint_coordinator.{h,cpp}, src/persistence/commit_resolver.{h,cpp}
- Test: tests/integration/m3_provider_integration_tests.cpp, tests/integration/m3_checkpoint_tests.cpp

- [ ] **Step 1: Write red two-phase tests**

Assert prepare changes no selected manifest slot, abort preserves the prior generation, commit publishes exactly the P4-recorded bytes, zero native id is rejected, and legacy M3Provider::checkpoint still uses checkpoint_generation + 3902 (candidate_generation + 3901).

- [ ] **Step 2: Add explicit M3 two-phase types**

Add request/reply interfaces equivalent to:

~~~cpp
struct M3CheckpointPrepareRequest {
    OperationContext context;
    VolumeBarrierKind barrier;
    std::uint64_t native_operation_id;
};
struct M3CheckpointPrepared {
    std::uint64_t prior_generation;
    std::uint64_t candidate_generation;
    std::uint8_t manifest_slot;
    Sha256Digest candidate_bytes_digest;
};
struct M3CheckpointCommitRequest {
    std::uint64_t native_operation_id;
    std::uint8_t manifest_slot;
    Sha256Digest candidate_bytes_digest;
};
Result<M3CheckpointPrepared> prepare_checkpoint(
    const M3CheckpointPrepareRequest&);
Result<M3CheckpointOutcome> commit_checkpoint(
    const M3CheckpointCommitRequest&);
Result<void> abort_checkpoint(std::uint64_t native_operation_id);
~~~

- [ ] **Step 3: Freeze the candidate before P4**

During prepare, validate the active manifest, construct PreparedManifestCandidate with committed time image_.checkpoint_generation + 2, retain the exact bytes, and return SHA-256(bytes). Commit compares the request to the held candidate before publish_manifest.

- [ ] **Step 4: Define shutdown handling**

An unpublished prepared transaction aborts on runtime/M3 destruction. A transaction whose manifest publication began reconciles and never becomes a new one-shot commit.

- [ ] **Step 5: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "integration\.m3\.(checkpoint|provider)"
~~~

Then:

~~~
git add src/m3 src/persistence tests/integration/m3_provider_integration_tests.cpp tests/integration/m3_checkpoint_tests.cpp
git commit -m "feat(m5): expose fail-closed checkpoint prepare commit abort"
~~~

## Task 4: Extract the shared DiskRuntime production adapter

**Files:**
- Create: src/run/runtime_adapter.{h,cpp}
- Modify: src/run/main.cpp, src/run/disk_runtime.{h,cpp}, src/m3/m3_windows_acceptance.{h,cpp}, src/CMakeLists.txt
- Test: tests/unit/m4_disk_runtime_tests.cpp, tests/integration/m4_run_loop_runtime_tests.cpp, tests/integration/m5_production_execution_backend_tests.cpp

- [ ] **Step 1: Write adapter-parity tests**

Using fake dependencies, assert foreground and worker adapter construction issues identical create/recover/flush/dismount/stop calls for identical lifecycle inputs.

- [ ] **Step 2: Extract production composition**

Define:

~~~cpp
struct WindowsVolumeOperations {
    Result<ProductVolume> provision(std::uint64_t bytes, wchar_t letter);
    Result<ProductVolume> find(wchar_t letter);
    Result<void> verify(const ProductVolume&, std::uint64_t bytes);
    Result<void> flush(const ProductVolume&);
    Result<void> assign(const ProductVolume&, wchar_t letter);
    Result<void> remove_letter(const ProductVolume&);
    Result<void> dismount(const ProductVolume&);
};
struct M3RuntimeOperations {
    Result<void> create_generation_zero(std::uint64_t bytes);
    Result<void> recover(std::uint64_t bytes);
    Result<void> start();
    Result<void> stop();
    Result<DiskCheckpointResult> checkpoint();
    DiskMaintenanceStats maintenance_stats() const;
};
struct RuntimeAdapterDependencies {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes;
    wchar_t drive_letter;
    WindowsVolumeOperations& volumes;
    M3RuntimeOperations& m3;
    HANDLE owner_token;
};
Result<DiskRuntimeOperations> make_runtime_operations(
    RuntimeAdapterDependencies&);
~~~

Move ForegroundRuntime operation construction into this factory. Keep main.cpp responsible only for foreground CLI, signal handling, and factory composition.

- [ ] **Step 3: Add worker-only two-phase delegation**

Add prepare, commit, abort, and recovery methods to DiskRuntime behind one runtime mutex. Retain the old public checkpoint() path as a one-shot M3 checkpoint.

- [ ] **Step 4: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m4\.(disk_runtime|run_loop_runtime)|m5\.production_execution_backend"
~~~

Then:

~~~
git add src/run src/m3/m3_windows_acceptance.* src/CMakeLists.txt tests/unit/m4_disk_runtime_tests.cpp tests/integration/m4_run_loop_runtime_tests.cpp tests/integration/m5_production_execution_backend_tests.cpp
git commit -m "refactor(m4): share DiskRuntime production adapter with M5 worker"
~~~

## Task 5: Build authenticated per-profile workers

**Files:**
- Create: src/host/m5_worker_protocol.{h,cpp}, src/host/m5_worker_session.{h,cpp}, src/host/m5_worker_main.cpp
- Create (rebuild): src/management/service_worker_control.{h,cpp}, service_worker_session.{h,cpp}, windows_worker_observation.h, windows_worker_recovery.{h,cpp}
- Modify: src/CMakeLists.txt
- Test: tests/unit/m5_service_worker_session_tests.cpp, tests/integration/m5_production_execution_backend_tests.cpp

This is the Decision A seam. `service_worker_control` stays the only
`IWorkerControl` implementation and keeps that interface frozen; what changes is
that it drives an `m5_worker_protocol` session over DiskRuntime instead of the
HostCore private protocol. `service_worker_session` and
`windows_worker_recovery` carry 161 HostCore references between them and are
re-seated on the same protocol; the private binding identity (profile id, image
id, worker pid, process creation time, session generation, profile revision) is
preserved exactly.

- [ ] **Step 1: Write private-protocol red tests**

Cover handshake authentication, replay rejection, binding mismatch, all four dispositions, candidate-digest mismatch, strict runtime observation, and recovery proof.

- [ ] **Step 2: Define typed private messages**

Prepare reply includes WorkerBinding, prior/candidate generations, native id, cut epoch, manifest slot, and candidate digest. Define commit, abort, status, runtime-observation, and recovery-proof requests; every request carries authenticated binding and product operation id.

- [ ] **Step 3: Implement a DiskRuntime-only worker**

Construct DiskRuntime through make_runtime_operations. Do not link or copy HostCore. Route private commands to runtime start/recover, flush, two-phase checkpoint, worker-owned cleanup, and status.

- [ ] **Step 4: Implement recovery proof**

A fresh worker returns exactly prior generation proved, this M5 candidate proved, or ownership unprovable. It validates image UUID, prior/candidate generation, native id, manifest slot, and candidate digest.

- [ ] **Step 5: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m5\.(service_worker_session|production_execution_backend)"
~~~

Then:

~~~
git add src/host src/management/service_worker_session.* src/CMakeLists.txt tests/unit/m5_service_worker_session_tests.cpp tests/integration/m5_production_execution_backend_tests.cpp
git commit -m "feat(m5): run authenticated workers through DiskRuntime"
~~~

## Task 6: Rebuild ServiceCore checkpoint and stop orchestration

**Files:**
- Modify: src/management/service_core.{h,cpp}, service_worker_control.{h,cpp} (both created in Tasks 2 and 5)
- Test: tests/model/m5_service_orchestration_tests.cpp, tests/model/m4_checkpoint_operation_tests.cpp, tests/model/m4_recovery_commands_tests.cpp, tests/integration/m5_checkpoint_boundary_tests.cpp, tests/unit/m5_service_worker_control_tests.cpp

This task edits the ported `service_core.cpp` rather than creating it: the
two-phase boundaries, the §3.6 recovery table and the dirty-truth stop rules are
deliberate changes to code that Task 2 brought in verbatim. `state_machine` is
edited only if a phase/gate contract needs extending.

- [ ] **Step 1: Write state-transition tests**

Cover every phase, disposition, and worker-liveness combination: admitted before P4, P4 after worker replacement, P5 with lower new-session epoch, abort acknowledgement loss, native-id mismatch, and all recovery-proof outcomes.

- [ ] **Step 2: Implement P4**

After prepare, atomically publish commit_boundary before private commit. Reject mismatched profile revision, binding, prior generation, native id, manifest slot, candidate digest, or durable operation revision.

- [ ] **Step 3: Implement P5 and recovery**

Implement:

~~~text
prior proof -> terminal not_committed, stable, release gate
matching candidate proof -> committed-P5 flow
ownership unprovable -> resolving/needs_attention, retain gate
P5 committed -> committed_pending_epoch_confirmation
P5 not_committed -> restore prior durable truth
P5 indeterminate/reconcile -> resolving
~~~

- [ ] **Step 4: Implement normal stop policy**

checkpoint_required always checkpoints after quiesce and flush. require_clean uses strict block-dirty observation; dirty, mismatch, or no answer resumes and never reaches removal. Compare epoch only within the P4/P5 session.

- [ ] **Step 5: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m5\.(service_orchestration|checkpoint_boundary|service_worker_control)|m4\.(checkpoint_operation|recovery_commands)"
~~~

Then:

~~~
git add src/management/service_core.* src/management/state_machine.* src/management/service_worker_control.* tests/model tests/integration/m5_checkpoint_boundary_tests.cpp tests/unit/m5_service_worker_control_tests.cpp
git commit -m "feat(m5): coordinate checkpoint and stop from durable evidence"
~~~

## Task 7: Compose service, scheduler, public pipe, and CLI

**Files:**
- Create (port): src/management/execution_scheduler.{h,cpp}, service_composition.{h,cpp}, service_response_wire.{h,cpp}, worker_supervisor.{h,cpp}, windows_service_adapters.{h,cpp}, service_main.cpp, src/cli/{cli_parser,cli_renderer,management_dispatch}.{h,cpp}
- Create (rebuild): src/management/windows_service.{h,cpp}
- Create (deferred from Task 1): src/control/service_response_codec.h
- Modify: src/control/public_pipe_{client,server}.{h,cpp} (only if the wire seam changes), src/CMakeLists.txt
- Test: tests/model/m5_execution_scheduler_tests.cpp, tests/integration/m5_{service_composition,public_pipe_round_trip,authorization_round_trip,mutation_round_trip,windows_service_production}_tests.cpp, tests/unit/m5_{cli,service_response_codec}_tests.cpp, tests/unit/m5_profile_command_tests.cpp

This task also clears the two Task 1 deferrals: `src/control/service_response_codec.h`,
`tests/unit/m5_profile_command_tests.cpp` and
`tests/unit/m5_service_response_codec_tests.cpp` all needed artifacts created here.
`windows_service` rebuilds only its worker-launch seam on `m5_worker_protocol`;
the SCM, auto-start and preshutdown-budget logic ports.

- [ ] **Step 1: Write scheduler/admission tests**

Assert durable operation admission precedes execution, only the scheduler invokes execute_operation, reads remain available after mutation admission closes, and client timeout never cancels a safe operation.

- [ ] **Step 2: Create one production composition root**

ServiceComposition owns profile store, operation store, ServiceCore, scheduler, worker supervisor, and public pipe. Serialize mutations per profile; unrelated profiles remain independent.

- [ ] **Step 3: Connect public pipe and CLI**

Support create, import, list, start, stop, checkpoint, auto-start configuration, and fail-closed forget. Render only typed service responses and map them to exit codes 0 through 5.

- [ ] **Step 4: Implement service lifecycle**

Auto-start profiles are scheduled independently. Preshutdown closes mutation admission, uses one SCM-bounded global deadline split among remaining profiles, and records indeterminate/needs-attention rather than claiming timed-out success.

- [ ] **Step 5: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m5\.(execution_scheduler|service_composition|public_pipe_round_trip|authorization_round_trip|mutation_round_trip|windows_service_production|cli|service_response_codec)"
~~~

Then:

~~~
git add src/management src/cli src/control src/CMakeLists.txt tests/model/m5_execution_scheduler_tests.cpp tests/integration/m5_* tests/unit/m5_*
git commit -m "feat(m5): host lifecycle authority in Windows Service"
~~~

## Task 8: Prove isolation and restart recovery

**Files:**
- Create: tests/integration/m5_restart_reconciliation_tests.cpp
- Modify: tests/integration/m5_mutation_round_trip_tests.cpp, m5_windows_service_production_tests.cpp, m5_production_execution_backend_tests.cpp

- [ ] **Step 1: Add two-profile isolation cases**

Fail profile A during create, P4/P5, removal, and preshutdown. Assert B's generation, mount, worker, operation gate, and writer lock remain unchanged.

- [ ] **Step 2: Add restart cases**

Drive admitted, commit_boundary, evidence_recorded with all four dispositions, and committed_pending_epoch_confirmation through service and worker replacement. Assert the exact designed recovery action.

- [ ] **Step 3: Add dirty-truth cases**

Assert restart followed by checkpoint-required stop checkpoints and removes; explicit checkpoint accepts lower new-session epoch; require-clean rejects block-dirty removal; and P4 worker replacement never removes.

- [ ] **Step 4: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m5\.(mutation_round_trip|windows_service_production|production_execution_backend|restart_reconciliation)"
~~~

Then:

~~~
git add tests/integration/m5_*
git commit -m "test(m5): prove lifecycle recovery and profile isolation"
~~~

## Task 9: Qualify local builds and evidence tooling

**Files:**
- Create: docs/validation/m5-current-state.md, m5-acceptance.md, m5-vm-execution-report.md, release/m5/MANIFEST.txt
- Create: tools/Assemble-M5Evidence.ps1, tools/Verify-M5Evidence.ps1

- [ ] **Step 1: Build and test clean Release**

Run:

~~~
cmake --preset release
cmake --build out/build/msvc --config Release --clean-first
ctest --test-dir out/build/msvc -C Release --output-on-failure
~~~

Record exact passed/failed results and artifact hashes. A relevant M5 failure prevents qualification.

- [ ] **Step 2: Build and test clean Debug**

Run:

~~~
cmake --preset debug
cmake --build out/build/msvc --config Debug --clean-first
ctest --test-dir out/build/msvc -C Debug --output-on-failure
~~~

Record Debug independently and distinguish M4 baseline failures from regressions.

- [ ] **Step 3: Implement fail-closed evidence tools**

Assembler accepts only a validated evidence root, hashes captures and frozen binaries, and rejects mixed runs. Verifier prints M5 VM EVIDENCE PASS only for a complete matching manifest.

- [ ] **Step 4: Test evidence tool negatives**

Test altered binary hash, missing case, duplicate case id, capture outside root, stale manifest, and malformed result input. Every negative exits nonzero and never prints the PASS literal.

- [ ] **Step 5: Commit**

~~~
git add docs/validation release/m5 tools/Assemble-M5Evidence.ps1 tools/Verify-M5Evidence.ps1
git commit -m "test(m5): add reproducible qualification evidence pipeline"
~~~

## Task 10: Execute VMware qualification

**Files:**
- Create: tests/e2e/m5_vm_lifecycle.ps1, tests/e2e/m5_vm_recovery.ps1
- Modify: docs/validation/m5-acceptance.md, m5-vm-execution-report.md, m5-current-state.md

- [ ] **Step 1: Run M5-A single-profile lifecycle**

Record create/import, start, checkpoint, stop, and exact generation transitions against frozen Release binaries.

- [ ] **Step 2: Run M5-B dual-profile isolation**

Induce a fault in A and prove B stays mounted, writable, at its original generation, and owned by its original worker.

- [ ] **Step 3: Run M5-C service restart and auto-start**

Exercise every dirty-truth case, then reboot and prove each auto-start profile recovers independently.

- [ ] **Step 4: Run M5-D preshutdown and VMware power-off recovery**

Capture graceful shutdown and Power Off -> Power On evidence. Verify recovery to each bundle's last complete generation and absence of synthetic success.

- [ ] **Step 5: Assemble and verify evidence**

Run:

~~~
.\tools\Assemble-M5Evidence.ps1 -EvidenceRoot C:\m5-evidence -ReleaseBinaries C:\m5-frozen-release
.\tools\Verify-M5Evidence.ps1 -EvidenceRoot C:\m5-evidence -Manifest .\release\m5\MANIFEST.txt
~~~

Expected: M5 VM EVIDENCE PASS and exit code 0 only for complete evidence.

- [ ] **Step 6: Commit factual evidence only**

~~~
git add docs/validation release/m5 tests/e2e/m5_*.ps1
git commit -m "docs(m5): record VMware lifecycle qualification"
~~~

## Task 11: Build the post-qualification GUI

**Files:**
- Create: src/gui/main.cpp, src/gui/tray_window.{h,cpp}, src/gui/service_client.{h,cpp}
- Modify: src/CMakeLists.txt, tests/CMakeLists.txt
- Test: tests/integration/m5_gui_pipe_tests.cpp

- [ ] **Step 1: Check the qualification gate**

Proceed only after Task 10 evidence verification emits M5 VM EVIDENCE PASS for frozen Release artifacts.

- [ ] **Step 2: Write thin-client tests**

Assert the GUI uses only PublicPipeClient and cannot open bundle, volume, device, or driver APIs.

- [ ] **Step 3: Implement Win32 tray and window**

Keep GUI state ephemeral; service remains authority for projections, progress, and mutations.

- [ ] **Step 4: Verify and commit**

Run:

~~~
ctest --test-dir out/build/msvc -C Release --output-on-failure -R "m5\.gui_pipe"
~~~

Then:

~~~
git add src/gui src/CMakeLists.txt tests/integration/m5_gui_pipe_tests.cpp tests/CMakeLists.txt
git commit -m "feat(gui): add post-qualification M5 thin client"
~~~

## Plan self-review

- Tasks 1 through 8 cover the M5 data plane; Tasks 9 and 10 cover local and VMware qualification; Task 11 isolates GUI after qualification.
- Tasks 3 and 4 preserve and regression-test M4 foreground checkpoint semantics while adding M5-only adaptation.
- Tasks 2, 3, 5, 6, and 8 cover CAS, import ownership, P4/P5, worker replacement, reconciliation, dirty truth, and cross-profile cleanup.
- The plan does not include VSS, remote management, encryption, driver installation, or new bundle/profile/checkpoint disk formats.
