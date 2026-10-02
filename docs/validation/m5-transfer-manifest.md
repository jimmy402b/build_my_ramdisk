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

### 1.10 Task 5 increment 3 — the private protocol reconciled with §2.5

§7 item 15's conflict is closed. The solution's §2.5 command set is now the
protocol's command set: eleven commands, no synonyms.

**Renamed.** `runtime_observation` → `require_runtime_observation`. The rename is
not cosmetic: the design spec §3.2 note makes the strict form a *mutation-safety*
question ("read M3 block-level dirty"), not an informational one, and
`IWorkerControl::require_runtime_observation` is documented as "no observation is
a failure, not `nullopt`". The session now enforces exactly that: `status`
reports whatever it saw, including `worker_alive == false`, while
`require_runtime_observation` refuses rather than return an observation that
cannot assert a live runtime. A caller deciding whether a checkpoint or a removal
is safe can therefore never read "could not ask" as "the runtime is clean".

**Added.** `remove_and_stop` and `shutdown`, both distinct from `cleanup`:

- `cleanup` (unchanged) is the worker's own reverse-order teardown, run once, and
  it locks out later commands.
- `remove_and_stop` removes this profile's **verified own** resources and stops.
  §5 permits a worker to clean only what it has proved it owns and the mandatory
  gate forbids a removal without proof, so the request carries a
  `WorkerRemovalAuthorization` with three separately-checked proofs (writer lock,
  matching device, retained bundle identity) plus the profile, image and the
  digest of the service's durable evidence. `require_removal_authorized` refuses
  any partial proof, a proof naming another profile, and a proof naming another
  image — the last one checked against the worker's **own** observation of the
  image it holds, not against the authorization's claim about it. A refusal
  leaves the registration and the bundle intact and reports no success. It also
  refuses while a two-phase candidate is prepared: removing the bundle would
  destroy the evidence of an operation the service has not resolved.
- `shutdown` is the service telling the worker to terminate. It dismantles
  nothing — the ordered teardown stays `cleanup`'s job, so the two are not
  synonyms and a caller can tell the worker to go away and still get a truthful
  teardown report afterwards. It refuses while a candidate is prepared (§3.5:
  an unresolved session is resolved, never abandoned), and after it only
  `shutdown` and `cleanup` are still served.

**Carried, not duplicated.** `WorkerRecoveryCorrelation` is now
`{product_operation_id, persistence::CheckpointRecoveryCorrelation}`. The
persistence type is the authority; the protocol carries it and the read-side
inspection consumes it unchanged, so the wire format and the inspection API
cannot drift into two field sets. Consequences recorded here because they are
visible:

- `src/host/m5_worker_protocol.h` now *includes* a persistence header. That is an
  include dependency only — no persistence symbol is called from the protocol or
  the session — so `myrd_m5_worker_protocol` still does not link
  `myrd_persistence`, and `myrd_management` (which links the protocol) does not
  gain a persistence link or trip `myrd_assert_execution_boundary`. The CMake
  comment was updated rather than left claiming an independence the target no
  longer has.
- `WorkerRuntimeStatus` gained `image_id`. The removal gate needs the image the
  worker actually holds, and the worker's own observation is the only honest
  source for it.
- `WorkerRecoveryOutcome` stays a distinct protocol enum — a wire enum should not
  change meaning when the persistence enum grows — but the two are now related by
  exactly one function, `worker_recovery_outcome()`, so they cannot drift in more
  than one place. A verdict the protocol has no case for maps to
  `ownership_unprovable`, which is the fail-closed direction.

**Evidence.** Full object rebuild (the protocol header changed), Release clean,
and the full elevated suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 139
CTEST_EXIT=0
```

139 is unchanged as a *test* count because both suites already existed; what grew
is the case count inside them: `unit.m5.worker_protocol` 20/20 (was 12) and
`unit.m5.worker_session` 18/18 (was 13). The new
protocol cases cover the correlation round-tripping field for field including the
image uuid (which has no canonical-text form on this channel), the removal
authorization round-tripping all three proofs, the removal gate refusing each
partial proof plus the wrong profile and the wrong image, and the verdict
mapping. The new session cases cover the strict-observation refusal, the removal
gate reached before the runtime is asked to do anything, removal refused across
an open two-phase boundary, shutdown refused across an open two-phase boundary,
and shutdown stopping service while leaving `cleanup` available.

### 1.11 Task 5 increment 4 — the private worker channel and its startup contract

Solution §2.2–§2.4. `src/host/m5_worker_private_pipe.{h,cpp}` plus a new
`src/host/m5_worker_bytes.h`.

**Why a shared byte header.** The launch spec is a second thing that has to cross
this boundary, and a second hand-written little-endian reader would be a second
place for the field conventions to drift. The primitives that were private to
`m5_worker_protocol.cpp` moved into `WorkerBytesWriter`/`WorkerBytesReader` in
`myrd::m5::detail`, and both TUs use them. The protocol's wire format is
unchanged by the move.

**The startup contract (§2.3).** `StartupSecret` (32 bytes from
`BCryptGenRandom`, failing closed rather than falling back to a weaker source)
and `WorkerLaunchSpec` travel entirely through an inheritable anonymous pipe
created by `create_startup_channel()`. The child's command line is to carry
*only the inherited handle value*; nothing sensitive is in it. The handshake
carries the secret, so reading the channel -- not merely being able to open the
pipe name -- is what proves the peer is the launched worker.

**The channel (§2.4).** `WorkerPrivatePipeServer` creates a per-profile,
per-session instance (`private_pipe_name()`, both axes in the name) with
`FILE_FLAG_FIRST_PIPE_INSTANCE`, so a stale or squatted name is an error rather
than something this session silently connects to. Frames are a 4-byte length plus
that many bytes, and the receiver refuses a length of zero or above
`kMaxWorkerMessageBytes` *before* allocating. `WorkerPrivatePipeClient::connect()`
and `handshake()` are deliberately separate, so a caller cannot reach a runtime
on a channel that was never authenticated.

**Deliberate departure: the pipe ACL names the service's own identity.** The
first implementation used a fixed `SYSTEM` + `Administrators` DACL, which the
tests could not exercise without elevation (`CreateFileW` returned
`ERROR_ACCESS_DENIED`). The fix is not a test seam: the DACL is built at runtime
from the creating process's token user SID, plus SYSTEM and Administrators.
That is *more* correct than the constant, because a Windows service is not
obliged to run as SYSTEM and a hardcoded DACL would leave the worker unable to
connect to its own service. It grants the interactive user nothing in
production, where the creator is the service account, and the public pipe's
descriptor is still not reused anywhere.

**Evidence.** Release build clean after a full object rebuild, and the full
elevated suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 140
CTEST_EXIT=0
```

140 = the previous 139 + `unit.m5.worker_private_pipe`. Standalone, the three
worker suites report 8/8, 20/20 and 19/19.

**A verification gap that was closed, recorded because it happened.** The first
attempt to run this suite had the elevation prompt declined twice, and the
increment was committed with the gap stated explicitly rather than papered over:
the 139/139 in §1.10 was then the newest full-suite result and did not cover
`unit.m5.worker_private_pipe`. The suite was re-run elevated afterwards and
reports 140/140 on the same tree, so the gap is closed — but the interim state is
worth keeping visible, because "committed with an outstanding full-suite run" is
a state that should be visible rather than remembered.

The handshake cases run against a real named pipe and two real threads, because
the properties under test are transport properties and a fake would assume away
exactly what can go wrong: a completed authenticated handshake followed by a real
command/reply; a peer with the right nonce and binding but the wrong startup
secret refused; a worker bound to another profile refused; and a second instance
for a live session name refused.

### 1.12 Task 5 increment 5 — the worker executable and its runtime

`src/host/m5_worker_runtime.{h,cpp}` (the production `WorkerRuntime`) and
`src/host/m5_worker_main.cpp`, in a new `myrd-m5-worker` executable target.
That target deliberately does **not** call `myrd_assert_execution_boundary`:
`myrd_m4_run` and `myrd_m3_windows` are exactly what Task 0 Step 3 says the
per-profile worker is the one target allowed to link.

**The ordering is the security property, so it is the first thing `main` does.**

1. Read the launch spec from the inherited startup handle, then close it
   immediately — holding it open would keep the secret reachable.
2. Connect to the private pipe the *spec* names and complete the authenticated
   handshake. (`DiskWorkerRuntime::open()` deliberately does not build a runtime;
   it only validates the config, so "construct the runtime" cannot happen before
   the handshake by accident.)
3. Only then construct the runtime and open the bundle.

The command line carries one hex handle value and nothing else, so no secret can
reach it. That value is unverified input, so it is parsed strictly (hex only, no
sign, no prefix, every character consumed, non-zero) and the handle is
type-checked as a pipe before use.

**`prove_recovery` is callable before `start()`**, which is the point of it: a
fresh worker must decide what the bundle proves *before* it takes the writer
lock. It forwards the correlation's persistence struct straight to
`M3Provider::inspect_checkpoint` (static, no lock, no store, no runtime) and maps
the verdict through the one `worker_recovery_outcome()` function.

**`start()` verifies rather than trusts.** `WorkerLaunchSpec` now carries the
image uuid the service recorded, and `start()` re-reads the bundle's own uuid via
`persistence::inspect_image` and refuses to run when they disagree. That is why
`observe().image_id` — which the removal gate checks the authorization against —
is a checked value rather than a restatement of what the service claimed.

`remove_and_stop` re-checks the ownership proof itself before dismantling, then
runs the same ordered teardown as `cleanup`. `shutdown` dismantles nothing.

**Evidence, and what is missing from it.** Release build clean after a full
object rebuild; `myrd-m5-worker.exe` reports its usage and exits 1 when launched
without a launch handle (fail-closed, checked by hand);
`unit.m5.worker_private_pipe` 8/8.

**The full elevated suite.** It was not run when this increment was first
committed — the elevation prompt was declined — so the commit named the gap
rather than papering over it, and predicted 140 because the increment adds an
executable, not a test. The suite was re-run elevated afterwards:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 140
CTEST_EXIT=0
```

140 is 140, so the prediction held. Recording the prediction *and* its
confirmation separately is the point: a prediction that is written down as a
prediction is checkable, and one recorded as a result is not.

**A stale-object trap hit again, immediately.** Adding `image_id` to
`WorkerLaunchSpec` changed a struct another translation unit already used, and
the incremental build left the pipe test binary reading the old layout: a
segmentation fault with no output. Deleting every `.obj` and rebuilding fixed it
with no source change. This is §7 item 17 happening a second time; the rule there
is not advisory.

### 1.13 Task 5 increment 6 — the service's end of the private channel

`src/management/service_worker_session.{h,cpp}`, in `myrd_management`. This is a
rebuild: the historical file of this name drove `host::HostMessage` over
HostCore, and both are prohibited imports. What it keeps is the *shape* — one
method per private command, the request and the accepted reply stated in the same
place so they cannot disagree — and the reason for it, which is that a reply can
be wrong in three ways that have each been a real defect here: a different
command, a different operation id, a mismatched binding. A reply carrying an
error is returned as a failure, never as a payload, because reading a success
payload past a reported failure is how a refusal becomes a recorded success.

`myrd_management` now links `myrd_m5_worker_protocol`. That is not a forbidden
execution dependency — the protocol reaches no runtime, no driver and no bundle —
so `myrd_assert_execution_boundary` still passes, and the service speaks to a
worker without regaining the execution coupling the check exists to prevent.

**Four defects this increment found, all in code that was already committed.**

1. **The shape table required a status payload on the *request*.** `status`,
   `require_runtime_observation` and `checkpoint_abort` all had `kHasStatus` in
   their *required* set, so every such request was unbuildable. It only surfaced
   when the service side was written, because the worker side had always been
   driven by hand-built messages. Payload now belongs on the reply; the request
   carries none. `checkpoint_abort` is one of them: the worker aborts the
   candidate it holds, so naming one in the request could only be a way to
   release someone else's prepare.
2. **The authentication gate required an operation id of every command but the
   handshake.** A status query belongs to no operation, so it could not be
   answered at all. The gate now requires the operation id exactly where the
   command's own shape requires it, so "operation-scoped" is stated once.
3. **`validate_shape` treated the operation id as a success payload** on error
   replies, which made every error reply to an operation-scoped command
   unbuildable. The binding and the operation id are *envelope*; only the rest is
   payload.
4. **The sequence number was consumed on reply-accept, not on send.**
   `ServiceWorkerSession::exchange` advanced only after accepting a reply, so a
   command that was *refused* left the next request on the same number — which
   the worker, having taken its high-water mark the moment the request
   authenticated, then refused as a replay. The sequence advances when the
   request is written.

**One claim of mine was wrong and is corrected here.** The previous increment
said `cleanup` "is still available after shutdown". That is true inside the
process and false on the wire: `m5_worker_main` breaks its loop as soon as
`shutdown` is requested, so no later `cleanup` is ever read. The two commands are
distinct in the other direction — `cleanup` is the explicit teardown the service
can ask for *instead of* shutdown, and it dismantles while the worker stays alive
to be asked for more. The test now asserts that, and that a dismantled worker
refuses a later shutdown rather than serving it.

**Evidence.** Release build clean after a full object rebuild, and the full
elevated suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 141
CTEST_EXIT=0
```

141 = the previous 140 + `unit.m5.service_worker_session`. That suite is 6/6,
driven against the worker's own session over a real named pipe and two threads;
standalone, `unit.m5.worker_protocol` is 20/20, `unit.m5.worker_session` 19/19
and `unit.m5.worker_private_pipe` 8/8.

### 1.14 Task 5 increment 7 — the volume identity producer

This increment is not in the plan's Task 5 file list. It exists because
`service_worker_control` cannot be written honestly without it, and the reason
is worth stating plainly as §7 item 18.

**What was missing.** The action plan's `record_volume_identity` step must
*return* `VolumeRegistrationEvidence{gpt_disk_id, partition_id, volume_id}` — three
`product::Uuid`s — for ServiceCore to persist into the durable profile
(`service_core.cpp:6120`). The historical adapter read them from
`host::WorkerRuntimeStatus.device`. Tracing the new stack, nothing produces that
triple at all: `ProductVolume` carried a `volume_guid` **wstring** plus
`VolumeExtent{disk_number, starting_offset, length_bytes}`, and
`VolumeRegistrationEvidence` appeared in the tree only as a stored, coded and
validated management type. The producer was part of the HostCore device identity
that M5 replaces, so re-seating the adapter on the private protocol would have
left `record_volume_identity` with no honest source — and inventing one would be
exactly the fabrication the adapter exists to prevent.

**What was added.** `src/run/windows_volume.{h,cpp}`:

- `VolumePersistentIdentity` (canonical lowercase GUID text) plus
  `ProductVolume::identity`, declared **last** so every existing aggregate
  initializer keeps its meaning — a fixture that does not name it gets an empty
  identity, which `complete()` reports as "not established" rather than as a real
  one.
- `canonical_guid_text()`, a pure function over the sixteen bytes of a Win32
  GUID. Data1/Data2/Data3 are little-endian in memory and Data4 is not, so a
  byte-wise hex dump would produce a GUID that looks right, parses right, and
  never matches the one Windows reports. That is why it is a named function with
  its own tests.
- `fill_persistent_identity()`, reading the partition GUID
  (`IOCTL_DISK_GET_PARTITION_INFO_EX`) and the GPT disk GUID
  (`IOCTL_DISK_GET_DRIVE_LAYOUT_EX` on the physical drive the validated extent
  names). **Best effort by design**: an unreadable identity leaves the triple
  empty rather than substituting a zero GUID, because a zero GUID is a value that
  compares equal to something rather than to nothing.

`DiskRuntime::adopted_volume()` exposes the volume the runtime actually adopted;
`m5::WorkerRuntimeStatus` gained `std::optional<WorkerVolumeIdentity> volume`;
`DiskWorkerRuntime::observe()` fills it only when all three were read. Canonical
text is the wire form deliberately — `product::Uuid` has no byte-level factory,
so the one canonical parser is `Uuid::parse`, and the adapter parses there.

**The completeness gate is in `validate_shape`.** Both the encoder and the
decoder pass through it, so a present-but-incomplete triple is refused once
rather than checked twice. A partial identity is not a weaker proof: it is one the
service cannot compare against a durable profile at all, and admitting it would
let a profile be recorded that no later start could ever verify.

**The paired decision, recorded because it narrows a check.** The six startup
steps (`start_proxy`, `create_exact_device`, `locate_exact_physical_drive`,
`provision_gpt_ntfs`, `assign_exact_mount`, `readiness_io`) were historically each
checked against `status.device.length_bytes`. The new `WorkerRuntimeStatus`
carries no device geometry, so they are proven by the authenticated, verified
`start()` that produced the status — the worker genuinely ran them inside
`DiskWorkerRuntime::start()` before answering, and the reply is authenticated and
image-verified, so this is a coarser proof boundary rather than a fabricated one.
Both this and the identity producer were put to the user as architecture/scope
decisions before any code was written; both were confirmed as described here.

**Evidence.** Release build clean after a full object rebuild (the `.obj` tree was
already empty, so this run was a from-scratch compile — §7 item 17 is satisfied by
construction), and the full elevated suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 141
CTEST_EXIT=0
```

141 is unchanged because the increment adds no new *executable*: the seven new
cases live inside three existing suites. Standalone counts moved
`unit.m5.worker_protocol` 20/20 → **22/22**, `unit.m4.disk_runtime` 27/27 →
**29/29**, `unit.m4.windows_volume` 16/16 → **19/19**.

**A compile error worth keeping.** The encode-side completeness check was first
written as an early `return` inside `write_body`, which returns `void`. Moving the
gate to `validate_shape` was not only the fix but the better design: the
invariant is now stated once, where both directions of the codec already
converge.

### 1.15 Task 5 increment 8 — the commit reply can express its own outcome

Closes §7 item 20, and carries the two decisions recorded in §7 item 21.

**The defect, and the second loss point the fix uncovered.** §7 item 20 named
`DiskWorkerRuntime::commit_checkpoint` as the place the disposition was lost.
Writing the fix found a second, earlier one: `map_disposition` in
`runtime_adapter.cpp` folded `CommittedNeedsReconciliation` into
`commit_indeterminate`, because `DiskCheckpointDisposition` had no case for it.
So the collapse happened twice, in two layers, and fixing only the worker's one
would have left the distinction still destroyed upstream.

**What changed.**

- `DiskCheckpointDisposition` gained `committed_needs_reconciliation`, appended
  rather than folded, and `map_disposition` now maps it faithfully.
- `DiskCheckpointResult` gained `std::optional<DiskCheckpointErrorContext>`, a
  mirror of `persistence::PersistenceErrorContext` built by the same
  mirror-don't-include convention the disposition already used. The adapter had
  been letting `M3CheckpointOutcome::error_context` fall off the floor, so a
  non-landed commit arrived with no evidence for ServiceCore to validate its own
  record against.
- The private protocol gained `WorkerCommitDisposition`,
  `WorkerCommitFailure` and `WorkerCheckpointCommitReply`, a new
  `kHasCommitReply` presence bit, and a codec for all of it. `checkpoint_commit`
  replies now carry the reply payload instead of a bare status.
- `WorkerRuntime::commit_checkpoint` returns the reply rather than a status, so
  the disposition cannot be dropped between the runtime and the session.
- `ServiceWorkerSession::commit_checkpoint` returns the reply, for the same
  reason on the service side.
- `remove_and_stop` now observes before dismantling and refuses a dirty runtime
  (see §7 item 21 for the decision and why the check lives in the worker).

**Two design choices worth stating.**

The status encoder and decoder were extracted into `write_status` / `read_status`
so the bare `status` payload and the status inside a commit reply cannot drift
into two field orders. A drifted field order does not fail to compile; it decodes
as a plausible status with the wrong numbers in it, which is the worst available
failure mode for a channel whose whole purpose is to be trustworthy. For the same
reason the decoder reads the commit reply *before* the bare status, matching
write order — the reader is a cursor, and a block read out of order would not
fail, it would decode the wrong bytes into the right fields.

The reply is refused when it is self-contradictory, in `validate_shape` where
both directions already converge: a landed commit must not also carry failure
evidence, and a commit that did not land must carry the failure that decided it.
The second half is the load-bearing one — without it a caller is handed a
disposition it cannot justify.

**Evidence.** Release build clean after a full object rebuild. Full elevated
suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 141
CTEST_EXIT=0
```

141 is unchanged again — this increment adds cases, not executables. Standalone:
`unit.m5.worker_protocol` 22/22 → **25/25** (four dispositions round-trip,
self-contradictory reply refused, undefined disposition byte refused),
`unit.m5.worker_session` 19/19 → **20/20** (the disposition-preservation case),
`unit.m4.disk_runtime` 29/29 → **30/30** (needs-reconciliation stays distinct).
`unit.m5.service_worker_session` is 6/6.

**What is not covered here, stated rather than implied.** The two mappings that
sit inside production objects — `ProductionRuntimeAdapter`'s
`map_disposition`/`map_error_context` and `DiskWorkerRuntime`'s
`protocol_disposition` and its dirty-refusal — have no unit test, because both
need a real M3 runtime and a real bundle. They are exactly what the deferred
`m5_production_execution_backend_tests.cpp` (Task 4 item 6) exercises, which is
Task 5's next remaining item, so the gap closes there rather than being left
open. Until that lands, those four functions are reviewed-but-unexecuted code.

**The undefined-disposition-byte test locates its byte by diffing.** Two replies
that differ only in their disposition are encoded and the first differing byte is
taken as the disposition. A hard-coded offset into the status would silently
point somewhere else the moment the status gains a field — which it just did.

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
    type should carry it rather than duplicate its fields. **Reconciled by §1.10**
    (Task 5 increment 3): the rename and the two new commands landed as written
    above, the protocol carries `persistence::CheckpointRecoveryCorrelation`
    instead of its own field set, and the three commands are verified distinct
    rather than synonymous.
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

18. **Task 5 gained a volume-identity producer, which reaches into Task 4's
    `src/run`.** The plan's Task 5 file list does not include `src/run/*`, but
    `service_worker_control` cannot realize `record_volume_identity` without a
    source for `VolumeRegistrationEvidence`, and the new stack had none (§1.14).
    The change is additive and behavior-preserving for existing callers: the new
    `ProductVolume::identity` is declared last so every aggregate initializer
    keeps its meaning, `same_identity()` is hand-written and still compares the
    same six fields, and no existing test changed. It also deliberately leaves
    `Win32VolumeAdapter::observe()` not re-reading the identity — nothing consumes
    it on that path yet, and filling it there without a consumer would be
    untested code. Recorded rather than done silently, and confirmed with the
    user as an architecture decision before implementation.

19. **The M5 `IWorkerControl` realization table, decided before implementation.**
    Recorded here so the adapter's fail-closed set is a decision rather than an
    accident of what happened to get written. Realized over the private protocol:
    `create_runtime_and_worker`, `recover_bundle`, `quiesce_mutations`,
    `flush_exact_volume`, `checkpoint`/`checkpoint_if_required`,
    `resume_mutations`, `remove_mount`,
    `remove_exact_device`/`stop_runtime_and_release_lock` (proven-by-removal
    no-ops), and the dirty-epoch/observation trio. Realized service-side with no
    worker: `create_and_verify_bundle`, `prove_new_uninitialized`. Fail closed
    with `unrealized_action`: the compaction family, `publish_live_config`,
    `publish_stopped_config`, `verify_rebind_bundle`, `publish_rebind`,
    `forget_metadata`. Two actions the historical adapter could *not* realize
    become realizable here and are noted as improvements rather than carried
    over as gaps: `inspect_bundle_read_only` (the old status carried no bundle
    identity; `WorkerRuntimeStatus::image_id` is now a verified value) and
    `reconcile_checkpoint` (the old comment cites "no worker command exposes the
    persistence failure context"; `recovery_proof` now does). `prove_forget_safe`
    and the compaction boundary methods stay on their inherited fail-closed
    defaults, which is the honest answer — the private protocol cannot prove a
    writer lock, a matching device or a bundle identity is absent, and a forget
    reported safe without that proof is what forget exists to prevent.

20. **CLOSED — the commit reply dropped the commit disposition.** Fixed by §1.15,
    which also found a second loss point (§7 item 20a). Kept rather than deleted,
    because the shape of the mistake is the useful part: the defect survived two
    increments because both were written from one side of the channel, and it was
    only visible to the first caller that had to *use* the reply. Originally
    recorded as:

    Found while
    writing `service_worker_control`, in the same way §1.13's four defects were
    found: the service side is the first caller that has to *use* what the worker
    sends. `DiskWorkerRuntime::commit_checkpoint` calls `checkpoint_ok()`, which
    is true only for `committed`, and turns every other outcome into one generic
    `internal_invariant` error — while the comment on the line directly above
    says "the disposition travels verbatim, including the indeterminate one". The
    private protocol's commit reply carries only `WorkerRuntimeStatus`, so
    `not_committed`, `committed_needs_reconciliation` and `commit_indeterminate`
    are indistinguishable on the wire.

    This is not cosmetic. `WorkerCheckpointEvidence::disposition` is what drives
    Task 6's P5 table: `committed` → committed-pending-epoch-confirmation,
    `not_committed` → restore prior durable truth, `indeterminate`/reconcile →
    resolving. Collapsing the three into one error means the service cannot tell
    "nothing was published, restore the prior record" from "the bundle's fate is
    unknown, park the operation" — and the safe answer for one is the unsafe
    answer for the other.

    **Traced to its source, and it is shallower than it first looked.** The
    disposition survives all the way up: `M3CheckpointOutcome` carries
    `disposition`, `generation`, `error` and `error_context`, and
    `ProductionRuntimeAdapter::commit_checkpoint` maps the disposition into
    `DiskCheckpointResult`. The loss is at exactly one place —
    `DiskWorkerRuntime::commit_checkpoint`, which tests `checkpoint_ok()` and
    discards the disposition it was just handed. `DiskCheckpointResult` does
    carry `disposition` and `generation`, so that much needs no new plumbing at
    all; the persistence failure context is the only genuinely absent piece,
    because the adapter maps only those two fields into `DiskCheckpointResult`
    and lets `M3CheckpointOutcome::error_context` fall off there.

    Fix shape, not yet implemented: widen `DiskCheckpointResult` to carry the
    error context (the upstream type is `persistence::PersistenceErrorContext`,
    which `WorkerCheckpointEvidence::error_context` already mirrors), add a
    commit-reply payload to the private protocol carrying the disposition, the
    generation and that context, have the worker relay
    `DiskCheckpointDisposition` verbatim instead of folding it, and extend the
    protocol round-trip tests to cover all four dispositions.
    `execute_checkpoint`'s boundary logic must not be written against a reply
    that cannot express its own outcome.

20a. **The defect had a second loss point, upstream.** `map_disposition` in
    `runtime_adapter.cpp` folded `persistence::CommitDisposition::
    CommittedNeedsReconciliation` into `commit_indeterminate`, because
    `DiskCheckpointDisposition` had no case for it. So the collapse happened in
    two layers, and fixing only `DiskWorkerRuntime` — which is where the trail
    originally led — would have left the distinction destroyed before it ever
    reached the worker. Found by fixing item 20 and asking where the value came
    from rather than only where it stopped. Fixed in §1.15.

21. **Two removal-path decisions, put to the user before implementation.**
    Recorded so the semantics are a decision rather than an accident.

    **(a) The worker refuses a dirty removal.** The historical adapter's
    `remove_mount` propagated a refusal from the worker, which declined to remove
    a runtime holding uncommitted writes because "removed anyway" would be
    indistinguishable from data loss. The M5 worker did not: `remove_and_stop`
    re-checked the authorization and then ran `cleanup()` regardless. The check
    was put back in `DiskWorkerRuntime::remove_and_stop`, before `cleanup()`
    (teardown is what would flush the very writes being refused), and the user's
    ruling was that it belongs there rather than in the adapter: a guard in the
    caller protects only that caller, and every other path to the call — the
    service composition, a retry, a future executor — would silently bypass it.

    **(b) The three removal-authorization proofs come from the worker's own
    verified start.** `require_removal_authorized` demands `writer_lock_proved`,
    `device_match_proved` and `bundle_identity_proved` before a removal may be
    requested. On the stop path those are facts the worker established when its
    `start()` took the writer lock, matched the AIM device and re-read the
    bundle's uuid against the image the service recorded — and the private
    channel authenticates that this is that worker. `remove_and_stop`
    independently re-checks that the authorization covers its own image. The
    alternative — building separate service-side proofs for each — was rejected
    as duplicating facts the worker already holds. `prove_forget_safe` stays
    fail-closed for actual *forget*, which is the case the hard constraint is
    about; the adapter must document per-flag what each boolean means on the stop
    path so the two are never confused.

22. **OPEN GAP — nothing in M5 can produce `BundleRegistrationEvidence`, so
    `create` and `import` cannot complete.** Found while preparing the adapter,
    and it corrects §7 item 19's realization table.

    ServiceCore requires `worker_result.lifecycle.bundle` to hold a
    `BundleRegistrationEvidence` for both `create_and_verify_bundle` and
    `inspect_bundle_read_only`; when it is absent it fails the operation with
    `needs_attention_failure` and terminal result `"bundle"`
    (`service_core.cpp:5903`). That struct carries an image id, a manifest name,
    the logical and sector geometry, the payload block size, and a
    `BundleFileIdentity` — a volume serial plus a 16-byte file id, which is
    `GetFileInformationByHandle` on the bundle file.

    Neither layer can supply it. `myrd_management` links only `myrd_profile`,
    `myrd_product_ipc`, `myrd_product_contracts`, `myrd_m5_worker_protocol` and
    `myrd_common`, and `myrd_assert_execution_boundary` keeps it that way — so
    the adapter cannot open a bundle, and `create_and_verify_bundle` must also
    *create* the generation-zero image. And the private protocol has no
    bundle-create or bundle-inspect command, so the worker cannot be asked
    either. Item 19's claim that `inspect_bundle_read_only` is "realized from the
    worker's own verified image identity" was wrong on two counts: the required
    evidence is a full struct rather than an image id, and it is required from an
    action that runs *before* the worker exists.

    There is a related ordering fact. In M5 the worker's `start()` is what
    creates the generation-zero bundle — `DiskRuntime::create` runs the
    `generation-zero` step — but the action plan places `create_and_verify_bundle`
    *before* `create_runtime_and_worker`. So the plan's ordering and the M5
    execution model disagree about who creates the bundle and when.

    **Corrected before any code was written, by asking where the value comes
    from rather than where it stops — the same check that found §7 item 20a.**
    Two of the three claims above are wrong. `BundleFileIdentity` *does* have a
    producer, and it is in the management layer already:
    `inspect_bundle_identity(std::wstring_view path)` in
    `profile_registration_store.cpp` reads it with
    `GetFileInformationByHandleEx(..., FileIdInfo, ...)`. And
    `BundleEvidenceSource` has two values, `created_and_verified` and
    `read_only_inspection` — the enum was built for exactly the two paths.
    So reading a bundle's *identity* is not what the execution boundary
    forbids; the boundary forbids the execution coupling, and `ServiceCore`
    parsing journal bytes. What is genuinely absent is only the **creation** of
    the generation-zero image, which needs persistence.

    **Chosen: the worker produces the evidence through its status.** Confirmed
    with the user. It follows the volume-identity precedent (§1.14) exactly, and
    it matches where bundle creation actually happens in M5 — the worker's
    `start()` — rather than leaving `create_and_verify_bundle` as a plan step
    ordered before the worker that performs it. Concretely:

    - `DiskWorkerRuntime::observe()` fills the bundle evidence. Its sources are
      already at hand or already exist: `image_id` is `impl.config.image_id`,
      which `start()` re-read against the bundle; the manifest name and geometry
      come from `impl.config` plus the product constants; and the file identity
      has an existing producer in `persistence/file_identity.cpp` to reuse rather
      than a second `GetFileInformationByHandleEx` written by hand.
    - `WorkerRuntimeStatus` carries it, as canonical fields beside the volume
      identity, and `ServiceWorkerControl` maps it into
      `BundleRegistrationEvidence` with the source the evidence actually came
      from — `created_and_verified` when the worker provisioned the bundle in
      this session, `read_only_inspection` when it adopted one that already
      existed.
    - The ordering disagreement the plan carries — `create_and_verify_bundle`
      before `create_runtime_and_worker`, while the worker's `start()` is what
      creates the image — is Task 6's to settle, since Task 6 owns
      `service_core.cpp` and the orchestration table.

    Recorded as still open because the code does not exist yet; the shape is
    settled and the remaining work is Task 5's adapter plus Task 6's ordering.
