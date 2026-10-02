# M5 Transfer Manifest

Status: Task 0 boundary record.
Branch: `feature/m5-lifecycle` (worktree `.worktrees/m5-lifecycle`)
Date: 2026-10-01

This file fixes what may be carried into the M5 lifecycle implementation, from
where, and on what evidence. It is written before any M5 source is added.

## 1. Baseline

`feature/m5-lifecycle` starts at `cb3df08` ("refactor: strip M5 scaffolding to a
clean M4 baseline"), the stripped M4 baseline that `main` also points at.

`cb3df08` contains only the qualified foreground runtime `myrd run`
(`src/run/*`) over M0–M3
(`common`/`storage`/`proxy`/`driver`/`poc`/`m1`/`m2`/`persistence`/`m3`).
No service, no profile store, no IPC, no GUI.

### 1.1 Recorded baseline build (pristine, before any M5 source)

Recorded from the pre-change tree, as required by Task 0 Step 4.

Recorded from the pre-change tree, as required by Task 0 Step 4.

```
cmake --preset msvc
cmake --build out/build/msvc --config Release
ctest --test-dir out/build/msvc -C Release --output-on-failure
```

Result: **129 tests, 123 passed, 6 failed**, build clean.

The 6 failures are environmental, not regressions: **every one is gated on an
elevated administrator token.**

| Test | Labels | Observed evidence unelevated |
| --- | --- | --- |
| `integration.shm` | m2-v4 | `CreateFileMappingW` failed |
| `integration.m2.shm` | m2-v4 | shared-memory mapping absent |
| `integration.m3.provider` | m3-v2;m3-v4 | shared-memory mapping absent |
| `unit.lifecycle_runner` | m2-v4 | `M0Provider::start failed: code=5 operation=CreateFileMappingW` |
| `unit.m2.windows_acceptance` | m2-v5 | `warm_m2_provider_runtime_for_acceptance` fails to start the provider |
| `integration.m3.windows_vhdx` | m3-v5 | self-reports `{"status":"BLOCKED","reason":"administrator_token_required","elevated":false,"aim_devices_online":-1}` |

Win32 code 5 is `ERROR_ACCESS_DENIED`.

**Diagnosis corrected by re-running the same set from an elevated shell:** all 6
pass, i.e. the AIM device and `phdskmnt` *are* present on this host and the
shared-memory proxy works. The gate is the token, not a missing driver. The
earlier reading in this note ("no AIM device and no driver loaded") was wrong.

```
Start-Process powershell -Verb RunAs ... -> ELEVATED=True
integration.shm            Passed   0.41 sec
unit.lifecycle_runner      Passed   0.41 sec
integration.m2.shm         Passed   0.25 sec
unit.m2.windows_acceptance Passed   0.04 sec
integration.m3.windows_vhdx Passed  1.39 sec
integration.m3.provider    Passed   2.16 sec
100% tests passed, 0 tests failed out of 6
```

**Consequence for routine work:** run the local suite from an elevated shell, or
these 6 (plus `unit.m4.profile_store`, see §5) fail for reasons unrelated to the
change under test. A non-elevated run is still useful, but its failure set must be
compared against this exact list before being called a regression.

Note: `integration.m3.recovery` passed here, unlike on the earlier M5 campaign
host, so that historical "known failure" does not apply to this machine.

### 1.2 Task 1 verification

After the contract transfer (Task 1), the full Release suite run elevated:

```
100% tests passed, 0 tests failed out of 132
```

132 = the 129 baseline tests + `unit.m4.profile_codec` + `unit.m4.profile_store`
+ `structural.m5.execution_boundary`. The transferred contracts therefore break
nothing in M0–M4, and the execution-boundary gate is live in the same run.

### 1.3 Task 2 verification

After porting the durable authority (15,355 lines: `operation_store`,
`service_operation_store`, `profile_registration_codec`, `profile_registration_store`,
`state_machine`, `service_core`), the full Release suite run elevated:

```
100% tests passed, 0 tests failed out of 135
```

135 = the previous 132 + `m5.service_operation_store` + `m5.service_orchestration`
+ `m4.checkpoint_operation`. The ported ServiceCore therefore satisfies the same
19 orchestration cases and the checkpoint-operation model it did on the source
branch, and `myrd_management` passes the execution-boundary assertion — confirming
the measurement in the plan's ownership table that this layer links no execution
target.


### 1.4 Task 3 verification

`M3Provider` gained the explicit two-phase boundary:
`prepare_checkpoint` / `commit_checkpoint` / `abort_checkpoint`, plus
`M3CheckpointPrepareRequest`, `M3CheckpointPrepared` and
`M3CheckpointCommitRequest`.

What was done, and what was deliberately not done:

- `checkpoint_impl` was split into `checkpoint_prepare_impl` (an explicit native
  operation id) and `publish_prepared_checkpoint`. The M4 one-shot
  `checkpoint()` is now those two calls in sequence with its native id unchanged
  at `checkpoint_generation + 3902`, so M4 behaviour is byte-for-byte the same
  path rather than a parallel one.
- The candidate is frozen during prepare: `prepare_manifest_candidate` is called
  there, and the slot plus SHA-256 of the exact candidate bytes are returned and
  required again at commit.
- `pending_two_phase_` is engaged exactly between prepare and publish. That
  invariant is what lets `stop` tell "prepared, unpublished" from "published,
  awaiting reconcile".
- §3.5's stop rule is implemented at the `stop()` pending-checkpoint branch: an
  unpublished prepare is **aborted** (releasing the snapshot and publish lease)
  instead of the previous unconditional `resume_pending_checkpoint`, which would
  have committed a candidate no caller authorised. A published transaction still
  resumes.
- A zero native operation id is refused at prepare, before any state changes.

Evidence: the Release build is clean, and the full elevated suite reports

```
100% tests passed, 0 tests failed out of 135
```

with `integration.m3.provider` passing. That run was executed **after** rebuilding
the provider test target with the five new cases, and all five case names plus the
three new API symbols are present in the test binary, so the cases ran rather than
being absent.

**Caveat, recorded honestly:** a follow-up attempt to print the per-case PASS list
was blocked because the elevation prompt was declined, so I have the suite-level
result and the compiled-in case names, not a fresh per-case listing. Re-run
`myrd_m3_provider_integration_tests.exe` from an elevated shell to print the five
`m3_two_phase_*` cases individually.

**Observability limit:** the M5 native-id rule
(`candidate_generation + kM5CheckpointOperationOffset`) is exercised by these
tests, but the constant itself is not yet shared with the service — Task 5/6 must
define it in one place so the worker and M3 cannot drift.

### 1.5 Task 4 verification

The production composition was extracted from `run/main.cpp` into
`src/run/runtime_adapter.{h,cpp}`: a `WindowsVolumeOperations` boundary, an
`M3RuntimeOperations` boundary, the four environment gates (`preflight`,
`classify_bundle`, `acquire/release_bundle_lock`), and
`make_runtime_operations()` which turns them into the single
`DiskRuntimeOperations` both the foreground CLI and the per-profile worker
compose through. `main.cpp` now keeps only CLI parsing, the test seams, signal
handling and this composition.

`DiskRuntime` gained the M5-only two-phase boundary — `prepare_checkpoint`,
`commit_checkpoint`, `abort_checkpoint`, `checkpoint_prepared()` — behind one
runtime mutex shared with the M4 one-shot `checkpoint()` and with
`shutdown_clean()`. `M3AimRuntime` gained matching pass-throughs to the Task 3
provider API.

Evidence: the Release build is clean and the full elevated suite reports

```
100% tests passed, 0 tests failed out of 136
```

136 = the previous 135 + `unit.m4.runtime_adapter`. The new `unit.m4.disk_runtime`
cases (7) and `unit.m4.runtime_adapter` cases (5) are the ones added here; the
other 124 are the pre-existing suites, unchanged, which is the evidence that the
extraction did not alter M4 behaviour.

**Unelevated comparison.** The same tree run unelevated fails 8, not 7: the
seven §1.1/§7.5 gates plus `m5.service_operation_store`, which fails at its own
`create_secure_directory` (`tests/integration/m5_service_operation_store_tests.cpp:92`)
— the identical SYSTEM+Administrators-DACL class. It is recorded in §7.5 so the
unelevated failure set is complete rather than looking like a Task 4 regression.
Task 4 touched no `src/management/` file.

### 1.6 Task 5 increment 1 — the private protocol (Steps 1–2)

Task 5 is in progress. This increment delivers the typed private service <->
worker protocol and its strict codec, plus the single definition of the M5
native operation id rule; the worker session, the worker executable and the
service-side `service_worker_control` rebuild are the remainder.

- `src/common/m5_native_operation_id.h` — `kM5CheckpointOperationOffset = 3902`
  and `m5_native_operation_id()`. Before this it existed **only** inside
  `tests/integration/m3_provider_integration_tests.cpp`; that test now includes
  the shared header instead of repeating the literal, so the M3 journal rule,
  the worker and the service's P4 record cannot drift (see §7 item 13).
- `src/host/m5_worker_protocol.{h,cpp}` — `WorkerBinding`, the handshake, the
  frozen `WorkerPreparedCandidate`, `WorkerCheckpointCommit`,
  `WorkerRuntimeStatus`, `WorkerRecoveryProof`, and the nine-command set; a
  strict binary codec that refuses a message without a binding, a stray payload
  for its command, a version or magic mismatch, a truncated body and trailing
  bytes; and the two gates — `authenticate_worker_message` (binding equality,
  strictly-increasing sequence, required product operation id) and
  `require_matching_candidate` (native id, slot, digest).
- `tests/unit/m5_worker_protocol_tests.cpp` (`unit.m5.worker_protocol`, 12
  cases) covers Step 1's list: handshake authentication, replay rejection,
  binding mismatch, candidate-digest mismatch, strict runtime observation, and
  every recovery-proof outcome, plus the fail-closed decoder negatives.

Nothing in this increment is copied from `host_ipc`/`host_protocol` (§3): the
message set, the framing and the gates are written against the frozen
`IWorkerControl` contract in `service_core.h`.

Evidence: Release build clean, full elevated suite

```
100% tests passed, 0 tests failed out of 137
```

137 = the previous 136 + `unit.m5.worker_protocol`.

### 1.7 Task 5 increment 2 — the worker command loop (Step 3)

`src/host/m5_worker_session.{h,cpp}` is the worker's command loop. It is
transport-free: it takes one decoded `WorkerMessage` and returns the reply, so
`m5_worker_main` later owns the pipe and this code stays testable. It drives
`WorkerRuntime`, an interface the production side implements over `DiskRuntime`
built by the shared adapter — the router the plan's Step 3 asks for.

What it enforces:

- **Authenticate before execute.** `authenticate_worker_message` runs first; a
  refused request is answered with an error and reaches the runtime not at all.
  The sequence high-water mark is consumed only on acceptance, so a replay of an
  accepted request stays refused.
- **The handshake nonce.** A handshake carrying any nonce but this session's is
  refused, so an echo from a previous session cannot re-establish the channel.
- **The candidate gate.** `checkpoint_prepare` derives
  `native_id = candidate_generation + kM5CheckpointOperationOffset` from the
  observed generation, then **re-derives it from M3's reply** and aborts if the
  two disagree. A candidate the service could not re-derive is never reported as
  P4 evidence. Commit repeats the frozen identity; a mismatch leaves the
  candidate prepared; a successful abort releases it; a *failed* abort keeps it
  prepared so the service can retry rather than assume release.
- **Worker-owned cleanup** runs once, and any later command is refused rather
  than served against a dismantled runtime.

`WorkerRecoveryCorrelation` was added to the protocol: a recovery-proof request
must carry the P4 correlation the service durably recorded, because that is the
only thing a fresh worker may judge ownership against.

`tests/unit/m5_worker_session_tests.cpp` (`unit.m5.worker_session`, 13 cases)
covers all of the above.

Evidence: Release build clean, full elevated suite

```
100% tests passed, 0 tests failed out of 138
```

138 = the previous 137 + `unit.m5.worker_session`.

### 1.8 The 2026-10-02 solution: recovery inspection, private IPC, Task 5/6 cut

`docs/superpowers/plans/2026-10-02-m5-inspection-private-ipc-solution.md` closes
three gaps that Task 4/5 hit and that no earlier task owned. It is carried onto
this branch for the same reason as the other design docs (§7 item 1).

**What it decides:**

1. **Recovery inspection belongs to Task 3, which reopens.** A new read-side
   `CheckpointRecoveryInspection inspect_checkpoint(context, expected)` lives in
   `src/persistence/image_inspector.{h,cpp}` + `recovery_engine.{h,cpp}`, with
   `M3Provider` exposing only a thin wrapper so the worker never reads a bundle
   directly. Test: `tests/integration/m5_checkpoint_inspection_tests.cpp`.
   §1.3 forbids using the write-side `JournalIndexBuildPlan` as recovery
   evidence — the inspection must read the committed manifest, the journal index
   and `JournalRecordCommon::operation_id` from the recovery side, re-read the
   selected manifest slot and hash its exact bytes.
2. **Private IPC belongs wholly to Task 5**, including
   `src/host/m5_worker_private_pipe.{h,cpp}`, `WorkerLaunchSpec`, process launch,
   handshake and worker-owned cleanup. The startup secret must travel through an
   inherited anonymous pipe or a protected inherited handle, never on the
   command line.
3. **Task 5/6 boundary.** Task 5 owns `service_worker_control.{h,cpp}`,
   `service_worker_session.{h,cpp}`, `windows_worker_observation.h` and
   `windows_worker_recovery.{h,cpp}`. Task 6 owns orchestration only and may not
   launch processes, frame the private pipe, or read a bundle/journal.

**Why Task 3 had to reopen (verified, not assumed).** The recovery proof needs
the journal's native operation id. It is durably written
(`JournalRecordCommon::operation_id`, `journal_format.h`), but nothing on the
read side surfaces it: `M3Provider`'s public API has only
`state/ready/generation/geometry/storage_stats/transport_snapshot/names`;
`persistence::RecoveredImage` (`recovery_engine.h`) carries geometry, image_uuid,
checkpoint_generation, base_generation, journal_epoch, selected_slot,
selected_manifest — no operation id; `SelectedImageInspection`
(`image_inspector.h`) likewise; and the read-side `JournalIndexView`
(`journal_index.h`) carries epoch, root_generation, committed_length and root,
but no operation id. The only accessor that returns one,
`JournalIndexBuildPlan::checkpoint_operation_id()`, is the **write-side** plan
that §1.3 explicitly excludes. So Task 5 Step 4 was not implementable on the
existing surface; this is exactly the gap the solution fills.

**Where the id actually is (the question this section left open, now answered).**
The read side already decodes it; it was simply never returned. Recovery's
per-candidate validation reads the manifest's own journal-commit descriptor and
calls `decode_checkpoint_commit` on those bytes
(`recovery_engine.cpp`, the `ManifestCandidateCheck::JournalCommit` check). The
decoded `CheckpointCommitRecord` is kept in `CandidateResources::journal_commit`
and carries `common.operation_id`. That is a read of the committed journal
record — the same `JournalRecordCommon::operation_id` the section names — and it
is **not** `JournalIndexBuildPlan`. It is the only read-side source, and it is
sufficient: adding one field to `RecoveredImage` surfaces it without any new
journal parsing. The alternative the section considered — hashing or parsing
journal bytes a second time — was unnecessary.

**Recorded conflict with already-committed code (§7 item 15).** Task 5
increments 1–2 were written before this solution existed and do not match §2.5.

### 1.9 Task 3 addendum — the read-only recovery inspection

Reopening Task 3 as §1.8 decides. What landed:

- `recovery_engine.h`: `RecoveredImage::journal_operation_id`, populated in
  `recover_complete_image` from the already-decoded
  `CandidateResources::journal_commit.common.operation_id`; and
  `read_manifest_slot_bytes()`, which re-reads one committed manifest slot image
  through the same inspection open path recovery uses.
- `image_inspector.{h,cpp}`: `CheckpointRecoveryCorrelation` (image uuid, prior
  and candidate generation, native operation id, manifest slot, candidate bytes
  digest), `CheckpointRecoveryInspection` (the six observed fields plus a
  `verdict` and a `candidate_mismatch_mask`), and
  `inspect_checkpoint(context, bundle_root, options, expected)`.
- `m3_provider.{h,cpp}`: a **static** `M3Provider::inspect_checkpoint`. Static
  because the caller is a fresh worker that has not opened the bundle yet; the
  inspection exists to decide what may be opened, so taking the writer lock to
  answer it would invert the ordering it enforces. It adds no policy — it builds
  `ImageInspectOptions` and forwards.

Read-only, concretely: the inspection creates no `M3Provider`, no
`VolumeRuntime`, no store, no transport and no writer lock. It calls
`recover_complete_image`, which opens the bundle for inspection and builds a
composite over existing bytes — the same path `inspect_image` has always used.

**Verdicts and the fail-closed rules.** `CandidateGenerationProved` requires all
six conditions (§2.6's five plus the prior generation) and is the *only* verdict
that permits confirming the operation. `PriorGenerationProved` requires the
committed generation to be the candidate's prior generation *and* the journal's
native id to be someone else's. Everything else — including a bundle that is
readable but not recoverable, and a candidate superseded by a later checkpoint —
is `OwnershipUnprovable`. A readable bundle that disagrees with the correlation
is a *successful* inspection with an unprovable verdict; only an operational or
format failure is an outer `Result` failure. Either way nothing is confirmed.

`candidate_mismatch_mask` carries one bit per failed condition, all six when the
bundle could not be read far enough to check any. It exists so a refusal can say
which conditions held; it is never zero unless the candidate is proved.

**Evidence.** Release build clean after a **full object rebuild** (see §7 item
17), `9/9 passed` from `myrd_m5_checkpoint_inspection_tests.exe`, and the full
elevated suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 139
CTEST_EXIT=0
```

139 = the previous 138 + `integration.m5.checkpoint_inspection`. Because the
whole object tree was deleted and recompiled first (§7 item 17), this count — not
the §1.4–§1.7 ones — is the first one measured on a tree that is guaranteed
internally consistent.

The nine cases are: a committed candidate proved (including that the reported id
is the candidate's 3903, not generation zero's 3901); the prior generation proved
after an abort; a forged manifest digest; a mismatched native operation id; a
mismatched image uuid; a mismatched manifest slot; a superseded candidate; a
bundle with no manifest; and that the inspection writes nothing, proved by
fingerprinting every file in the bundle (names, sizes, SHA-256) before and after.

**One deliberate non-obviousness in the mask.** The digest is taken over the slot
the bundle *selected*, so a correlation naming the wrong slot fails only the slot
condition (the bytes still hash to the prepared digest). Both conditions are kept
because they catch different things: the slot field catches a P4 record that
named a different slot, and the digest catches byte-level drift. The test asserts
the single-bit mask so this cannot quietly change.

## 2. Direct-transfer sources

**Only** these directories may be transferred from `feature/m5-unified-integration`:

| Path | What it carries |
| --- | --- |
| `src/product/*` | uuid / error / state / quantity / json contracts |
| `src/control/*` | typed command / IPC framing / pipe transport / authorization |
| `src/profile/*` | binary `profile.myrd` codec, strict validation, atomic store |

Nothing outside those three directories is transferred. The remaining M5
directories (`management`, `host`, `cli`, `setup`) are **rebuilt**, not copied.

## 3. Prohibited imports

The following must not be imported, linked, or copied, in any form:

- `HostCore` and its `M3HostProviderAdapter` execution core
- `host_main` (the old host process entry point)
- the old `ServiceWorkerControl`
- the old Windows-service composition
- any second state machine, JSON IPC, profile format, or host execution core

Rationale: the stripped M5 attempts already carried a duplicate execution engine
next to `DiskRuntime`. That duplication is where the `dirty_epoch` / block-dirty
defects lived. M5 reuses M4's `DiskRuntime` as the only per-disk engine.

## 4. Historical branch policy

- `feature/m5-unified-integration` is the only direct contract reference.
- `feature/m5-supervisor-service` is a superseded JSON-profile / `DiskSupervisor`
  experiment. It is **read-only reference material**, never an implementation
  source. Its `src/setup/driver_manifest.*` matches the shared baseline; its
  profile implementation predates the unified branch's protected-directory and
  provisioning-identity changes. Nothing from it may be copied without an
  explicit contract test and review.

Task 1 Step 4 re-runs the differential check
(`git diff feature/m5-supervisor-service..feature/m5-unified-integration -- src/profile src/setup`)
at transfer time.

## 5. Execution boundary (Task 0 Step 3)

The only execution path is:

```text
public pipe -> ServiceCore -> per-profile worker -> DiskRuntime -> M3 -> phdskmnt
```

No management, CLI, or GUI target may link the execution-authority libraries in
process. Enforcement is a configure-time assertion,
`myrd_assert_execution_boundary(<target>)`, defined in
`cmake/ExecutionBoundary.cmake`. It rejects `myrd_m4_run`, `myrd_m3_windows`,
`myrd_m3_management`, `myrd_m1`, `myrd_driver` and `myrd_proxy` on the target's
`LINK_LIBRARIES`, `INTERFACE_LINK_LIBRARIES`, `INTERFACE_LINK_LIBRARIES_DIRECT`,
`INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE` and `MANUALLY_ADDED_DEPENDENCIES`
properties, and fails configuration.

This mirrors the existing storage-boundary gate in `tests/CMakeLists.txt`
(`myrd_storage` must not reach `myrd_proxy`/`myrd_driver`).

Only the M4 foreground executable `myrd` and the M5 per-profile worker executable
(the Task 5 deliverable) are permitted to link the execution-authority libraries.

It is applied to **product** targets only (`myrd_product_ipc` and `myrd_profile`
call it). Test executables are deliberately exempt: a test legitimately links
whatever code it exercises, including `myrd_m4_run` and `myrd_m3_management`, so
asserting the boundary on them would be false and would force the gate to be
weakened later.

The assertion is itself verified by `structural.m5.execution_boundary`
(`tests/unit/test-m5-execution-boundary.ps1`), which proves it rejects each
forbidden dependency and accepts a contract-only target.

## 6. M4/M3 invariants preserved

- M3 bundle, journal, manifest and profile on-disk formats are unchanged.
- No task installs, replaces, upgrades, or uninstalls `phdskmnt`; it remains an
  externally supplied prerequisite.
- `disk remove/forget` fails closed: without proof that the writer lock, matching
  device and retained bundle identity are safe, it refuses and retains both the
  registration and the bundle, and never reports success.
- M4 foreground one-shot checkpoint semantics are unchanged; the two-phase
  prepare/commit/abort path is M5-only.

## 7. Deviations from the plan text

Recorded so the plan and the tree do not silently diverge.

1. **Design docs carried onto the branch.** The plan, task list and design spec
   were untracked on `main`, so a `cb3df08` checkout does not contain them. They
   are committed on `feature/m5-lifecycle` so the branch is self-contained.
   `main` is left at `cb3df08` and stays a clean M4 baseline.
2. **Task 0 Step 3 shape.** At `cb3df08` no management or CLI target exists yet,
   so the step cannot literally constrain them. It is implemented as the
   enforcement mechanism plus its negative test; Tasks 1, 5 and 7 call
   `myrd_assert_execution_boundary` on the targets they create.
3. **Task 1 scope narrowed to what is self-contained.** Task 1's test list names
   four tests, but two of them cannot compile until Task 7 delivers their
   dependencies:

   | Deferred item | Requires | Delivered by |
   | --- | --- | --- |
   | `tests/unit/m5_profile_command_tests.cpp` | `src/cli/cli_parser.h` | Task 7 |
   | `src/control/service_response_codec.h` | `src/management/service_core.h`, `src/management/service_response_wire.h` | Task 7 |
   | `tests/unit/m5_service_response_codec_tests.cpp` | `src/management/service_response_wire.h` | Task 7 |

   Task 1 therefore transfers `src/product/*`, `src/profile/*`, and `src/control/*`
   **except** `service_response_codec.h`, and runs the two self-contained tests
   (`unit.m4.profile_codec`, `unit.m4.profile_store`). This follows Step 3's own
   instruction not to add management or worker execution code. The deferred header
   and tests move to Task 7, which creates their dependencies.

   Task 2 has the same shape: `tests/integration/m5_profile_registration_store_tests.cpp`
   includes `src/cli/cli_renderer.h` and `src/management/service_response_wire.h`,
   both Task 7 artifacts, so it defers there too. Task 2 runs
   `m5.service_operation_store`, `m5.service_orchestration` and
   `m4.checkpoint_operation`.

   The general rule this exposes: **a plan task's test list may name tests whose
   dependencies a later task creates.** Each such test moves to the task that
   creates its dependency, and the move is recorded here rather than silently
   dropped.
4. **Management-layer ownership re-cut (2026-10-01).** Tasks 2/5/6/7 previously
   said "Modify" files that do not exist at `cb3df08`; the layer is 30 files /
   26,310 lines and had no owning task. The plan now carries an explicit ownership
   table. The port/rebuild split was measured, not assumed: the layer's entire
   execution coupling is 9 files (306 host references), and `service_core.cpp`
   has none. See the plan's "Management layer file ownership" section, including
   the recorded departure from `docs/m4-m5-scope.md` §4 over porting ServiceCore.

5. **`unit.m4.profile_store` is elevation-gated.** It builds a protected
   ProgramData layout whose intermediate directories carry a SYSTEM +
   Administrators-only DACL; creating a child inside such a directory needs
   `FILE_ADD_SUBDIRECTORY`, which directory ownership does not confer. It passes
   from an elevated shell and fails with `ERROR_ACCESS_DENIED` otherwise — the same
   class as the §1.1 baseline set, and not a defect in the transmitted code.
   `m5.service_operation_store` is the same case, at
   `create_secure_directory` in `m5_service_operation_store_tests.cpp`; it is
   listed here so the **unelevated** failure set is exactly
   {integration.shm, integration.m2.shm, integration.m3.provider,
   integration.m3.windows_vhdx, unit.lifecycle_runner, unit.m2.windows_acceptance,
   unit.m4.profile_store, m5.service_operation_store} = 8 tests. Any other
   unelevated failure is a real regression.
6. **Task 4: `m5_production_execution_backend_tests.cpp` deferred to Task 5.**
   Task 4's test list names it and Task 5's names it again. At `cb3df08` it does
   not exist, and on `feature/m5-unified-integration` it includes
   `src/host/host_ipc.h` and `src/host/host_protocol.h` (HostCore — a §3
   prohibited import) as well as `src/management/service_composition.h` (Task 7)
   and `src/management/service_worker_session.h` (Task 5). It therefore cannot
   compile, let alone run, before Tasks 5 and 7 create those. Deferring follows
   the general rule in item 3 above; the test is rewritten there against
   `DiskRuntime`, not copied.
7. **Task 4 Step 1's parity tests live in a new file.** The plan asks for
   "adapter-parity tests" but names no file for them; Task 4's list names only the
   M4 suites. They are `tests/unit/m4_runtime_adapter_tests.cpp`
   (`unit.m4.runtime_adapter`), which drives foreground and worker compositions
   with identical fake dependencies and asserts byte-identical call sequences,
   including the M5 two-phase boundary and the shutdown cleanup order.
8. **Task 4 Step 3 split: prepare/commit/abort here, recovery in Task 5.** Step 3
   says "Add prepare, commit, abort, and recovery methods to DiskRuntime". The
   first three are implemented and covered in `unit.m4.disk_runtime` in Task 4.
   The **recovery** method (the §3.5 read-only reconciliation command that returns
   prior-generation-proved / candidate-proved / ownership-unprovable) is deferred
   to Task 5: its input is the P4 durable correlation that Task 2's stores carry
   and the worker exchanges, and its only test is the deferred backend suite from
   item 6. Implementing it here would be an untested guess at Task 5's protocol.
9. **`RuntimeAdapterDependencies::owner_token` is `std::wstring`, not `HANDLE`.**
   The plan's struct sketch shows `HANDLE owner_token`. `Win32VolumeAdapter`
   consumes a `std::wstring` token, and the foreground value is
   `L"myrd:" + pid`, so keeping the string type is what makes the extraction
   behaviour-preserving. The sketch's `HANDLE` would have required inventing a
   conversion with no consumer.
10. **`M3RuntimeOperations` also carries prepare/commit/abort.** The plan's Step 2
    sketch lists only create/recover/start/stop/checkpoint/maintenance_stats, but
    design spec §4 states two-phase is the addition *above* the adapter and that
    the worker reuses the adapter. Task 5 must therefore reach the two-phase
    boundary through the same shared adapter; a second private path would
    reintroduce the duplication the extraction exists to remove.
11. **DiskRuntime mirrors the M3 two-phase types instead of including them.**
    `DiskCheckpointPrepareRequest/Prepared/CommitRequest` are declared in
    `disk_runtime.h` in terms of `src/common/sha256.h`, exactly as the existing
    `DiskCheckpointResult` mirrors `persistence::CommitDisposition`. This keeps
    the engine header free of M3 headers; `ProductionRuntimeAdapter` performs the
    translation.
12. **Shared environment helpers moved out of `main.cpp`.** `classify_bundle` and
    `require_elevated_administrator` moved into `runtime_adapter.{h,cpp}`, and
    `make_production_environment()` supplies the four gates for both callers.
    `main.cpp`'s early preflight/classify (needed for the `MYRD_M4_TEST_*` seams
    and for failing before any bundle access) still runs, so the driver gate is
    evaluated twice on the foreground path — exactly as it was before.
13. **Task 5: the native-id constant lives in `src/common/`, not the worker.**
    `kM5CheckpointOperationOffset` is read by the M3 journal rule, the worker and
    the service's P4 record, and `myrd_management` may not link `src/host`.
    Putting it in `src/host/m5_worker_protocol.h` would have forced either a
    layering violation or a second copy, so it is
    `src/common/m5_native_operation_id.h`. The M3 integration test's local copy
    is deleted in favour of the shared header.
14. **Task 5: the private protocol is a new type set, not a reuse of
    `ipc_frame`.** The public frame carries a schema-driven command payload and
    is deliberately reachable by an unprivileged client; the private protocol
    carries an authenticated `WorkerBinding` on every message and a
    strictly-increasing sequence. Folding them together would have meant the
    public codec learning worker binding semantics, so the private messages reuse
    only the canonical-text UUID convention (and the byte-identical formatter
    from `ipc_frame.cpp`).
15. **Task 5 increments 1–2 predate the 2026-10-02 solution and do not match its
    §2.5 command set.** Recorded rather than silently fixed, so the delta is
    visible. The committed protocol (`7c6a3e0`) has nine commands:
    `handshake, start, status, checkpoint_prepare, checkpoint_commit,
    checkpoint_abort, runtime_observation, recovery_proof, cleanup`. §2.5
    requires a set that instead names `require_runtime_observation` and adds both
    `remove_and_stop` and `shutdown` (distinct from the existing `cleanup`).
    Reconciling means: renaming `runtime_observation`, and adding the two
    commands with their payload shapes and gates — **not** adding synonyms
    alongside the old names, which would leave two ways to ask for the same
    thing. `WorkerRecoveryCorrelation` (protocol) also needs to reconcile with
    the solution's persistence-level `CheckpointRecoveryCorrelation`; the
    inspection API's parameter type should be the authority and the protocol
    type should carry it rather than duplicate its fields.
16. **Task 3 addendum: `inspect_checkpoint` takes the bundle location and
    inspection options.** §1.2 writes the signature as
    `inspect_checkpoint(context, expected)`, which omits where the bundle is and
    what the recovery limits are. The implementation is
    `inspect_checkpoint(context, bundle_root, options, expected)`, mirroring the
    existing `inspect_image`/`verify_image`: `ImageInspectOptions` already
    carries the manifest name and the bounded recovery limits. `M3Provider`'s
    static wrapper is the shorter shape §1.2 describes, because
    `M3ProviderConfig` already holds both.
17. **The build directory does not track header dependencies — so every earlier
    "build clean" claim was made against a possibly stale tree.** `ninja -t deps`
    reports **no recorded dependencies** for the object files in
    `out/build/msvc` (checked on
    `src/CMakeFiles/myrd_persistence.dir/Release/persistence/image_creator.cpp.obj`);
    that object's mtime was a day older than a header it includes. A normal
    `cmake --build` therefore recompiles a changed header's dependents only if
    their `.cpp` also changed. This is not cosmetic: adding a field to
    `RecoveredImage` left `image_creator.cpp.obj` stale, and because
    `create_generation_zero_image` returns `Result<RecoveredImage>` **by value**,
    the test binary read a return object laid out for the old size — a 0xC0000409
    CRT abort with a garbage `ok()`. **Rule from here on: after any header edit,
    delete the stale objects (`find out/build/msvc -name '*.obj' -delete`) and
    rebuild, or the verification result is not trustworthy.** The §1.9 evidence
    was produced that way, and the Task 3 addendum suite run re-establishes a
    full-rebuild count for the current tree. The counts in §1.4–§1.7 were taken
    before this was understood: they were real runs of a built tree, but the tree
    may have mixed objects from before and after a header change. Re-running them
    from a full rebuild is a Task 9 obligation.
