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

### 1.16 Task 5 increment 9 (unit B) — the removal reply can express its own outcome

Closes the second of the two defects §1.15a found. The abort reply (§1.15a's
other finding) is deliberately **not** in this increment: tracing it showed
`DiskRuntime::abort_checkpoint` returns `Result<void>`, so there is no failure
context anywhere upstream to carry. Fixing it needs the same data-plane work
increment 8 did for commit, and inventing a context from the bare `Error` would
name a persistence failure that never happened. Split out as unit A rather than
half-done here.

**What changed.** `WorkerRemoveAndStopReply` carries `removed`, `stopped`,
`generation` and a `WorkerRemovalRefusal`; `remove_and_stop` replies with it
instead of a bare status, in `WorkerRuntime` and `ServiceWorkerSession` alike.
`validate_shape` refuses a reply whose two halves disagree — a claimed removal
that is only half done, or a refusal alongside a completed removal — because the
barrier takes the mount, the device and the runtime in one authenticated call,
and the caller records a removal either way.

**The refusal is now a first-class answer, not an error.** This corrects
increment 8's own fix: the dirty check added there was right in effect and wrong
in reporting, returning a generic `runtime_error` where the historical protocol
had `RemoveAndStopAuthorization::runtime_dirty` as a value of its own. The
distinction it lost is the one that matters most for a caller — "the worker
refused, resolve the dirty runtime against durable truth" versus "the worker did
not answer, retry" — and a caller that cannot tell them apart retries the one
call it must never retry. The service-session test's own comment said this
before the code did: *"A removal the worker refuses is the worker's reason, not a
generic protocol failure: the caller has to act on why."* The test asserted the
wrong thing and passed, which is how the defect survived; it now asserts the
refusal arrives as an answer.

**Evidence.** Release build clean after a full object rebuild; full elevated
suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 141
CTEST_EXIT=0
```

141 unchanged — cases, not executables. Standalone: `unit.m5.worker_protocol`
25/25 → **27/27** (a refusal round-trips and is distinguishable from a completed
removal; a half-removed reply is refused in both directions),
`unit.m5.worker_session` 20/20 → **21/21** (the session relays a refusal as an
answer), `unit.m5.service_worker_session` 6/6.

**A logic error caught by its own test, recorded because the test was right and
the code was not.** The disagreement check was first written as
`refused() == (!removed || !stopped)` — refusing exactly the replies that *agree*
with themselves and accepting the contradictory ones. Two suites failed on it at
once, which is what a rule stated in one place buys.

### 1.30 Task 5 increment 15 (unit D) — the end-to-end case, and the two defects it found

§1.25's case is back, and it passes. It is the only case in this tree that drives
a whole lifecycle through the real objects: a real `myrd-m5-worker` process, a
real generation-zero image, a real AIM proxy disk, a real provisioned and
formatted 64 MiB volume, a real drive letter assigned and released.

**It found two defects on its first run, and neither was the mode §1.25
blamed.** With the mode fixed, `start()` still could not come up:

1. **The worker passed a zero RAM budget.** `DiskWorkerRuntime` built its
   `ProductionRuntimeAdapter` with `ram_limit_bytes = 0`, and
   `store_options_for(0)` set `hard_limit_bytes`, `soft_limit_bytes` and
   `eviction_low_watermark_bytes` to zero. Every one of those is a hard limit
   the M3 store requires to be non-zero and strictly ordered, so
   `M3Provider::open` refused with *"provider limits are invalid"* -- before a
   bundle was opened. Zero now means "the caller has no budget to impose", and
   the production defaults stand. That is the fix rather than a magic number in
   the worker, because the worker's launch spec does not carry the profile's
   memory policy at all (see below) and 0 could never be a valid budget.
2. **`myrd-m5-worker` had no embedded provisioning script.**
   `run_provisioning_helper` resolves the script from `GetModuleHandleW(nullptr)`
   -- the *executable* module -- and the `.rc` was attached to `myrd_m4_run`
   (static) and `myrd` only. The worker therefore created generation zero,
   started AIM, and died in `provision_new_volume` with
   `ERROR_RESOURCE_TYPE_NOT_FOUND` (1813). The target now carries the resource,
   for the same reason `myrd` does.

So §1.22's functional gap #3 -- "the worker's `start()` has never succeeded
anywhere" -- had three causes, not one. §1.25 found the mode; these two were
behind it, and both are the kind that only a run can find: a value that is
unset-but-treated-as-set, and a resource that lives in the wrong module. They
are recorded as §7 item 25.

**Evidence.** Release build clean after a full object rebuild; no compiler
warnings. Full suite **143/144**, the one failure the pre-existing
`integration.m3.recovery` (§1.18). `m5.production_execution_backend` 5/5 →
**6/6**, and the whole suite -- including the real lifecycle -- runs in **8.4 s**.
The suite's timeout went from 120 s to 300 s to allow for a provisioning
round-trip; the observed cost says that was generous rather than necessary.

**The side effects, verified rather than assumed.** The case claims a drive
letter it has confirmed is free (scanned from `Z:` downwards) and releases it on
every path: `cleanup` removes the letter and dismounts, and a worker whose
channel closes without one runs the same teardown as it exits. Checked after the
run: no extra drive letter, no extra disk, no `myrd-m5-worker` process.

**The probe is gone.** Locating either defect meant a temporary `fprintf` of the
error, because the framework's `REQUIRE` exits at the first failure and prints
only a line number. The case's comment says so, so the next person does not
rediscover why a bare `created.ok()` tells them nothing.

#### What this unit does not settle

**The launch still does not carry the profile's memory policy.** A profile has
one (`MemoryPolicy::hard_limit_bytes` and its two companions), the create intent
carries the inputs it is materialized from, and the worker gets none of it -- so
a worker runs with the production store defaults rather than the limits the
profile was configured with. That is a real functional gap in M5 and it is
unchanged by this round; what changed is that it is no longer *fatal*. Carrying
it means widening the launch spec, and the create path would have to materialize
the policy before the profile is published, which is the same boundary §1.26 was
about. Recorded rather than decided.

### 1.29 Task 5 increment 15 (unit C) — the two bundle actions are realized

`create` and `import` can now complete. Both are answered by launching a worker
*from the operation's lifecycle intent* and asking it for the bundle's evidence.

**The launch had to stop taking a durable record.** Both actions run before their
profile is published, so at that moment there is no record for `bind_worker` to
launch from -- and §1.26's point is that the intent carries everything a launch
needs anyway. `IWorkerSessionProvider::launch` therefore takes a
`WorkerLaunchRequest`, and two translations produce one: from a durable record,
and from an intent. The translation is where the product's rule about mount
targets is enforced, once for both producers and before any process exists --
the profile path additionally has to name a drive-letter *mount kind*, which the
provider-side check never looked at.

**The volume mode comes from the same criterion in both.** A profile whose
lifecycle is still `provisioning` has a bundle and no volume, so the launch asks
for `provision_new`; every other profile adopts the volume it has. That is
`append_start_actions`' own branch restated, not a second rule invented here.

**`create` holds the session and `import` does not.** After a create the plan
continues: `create_runtime_and_worker` finds this session instead of launching a
second worker for one bundle, and the provisioning-family steps read the runtime
this command started. An inspection leaves nothing to command, so its session is
dropped -- the channel closes, the worker exits, and no process is kept alive for
a profile whose operation has nothing left to do.

#### The departure from §1.26, and why it is a correction rather than a preference

§1.26 says `inspect_bundle_read_only` "follows the same path for import, where
there is no provisioning to do", and that the shape table "gains one bit rather
than a second evidence shape". **The second half is right and the first half is
not**, and the reason is one fact §1.26 did not have: in this runtime the only
way to open a bundle and produce evidence is `DiskRuntime::recover` plus
`start()`, and **recovery assigns a drive letter.** Following `create_bundle`'s
path would therefore mount a volume and leave a worker running for an imported
profile that the plan records as `ready` and never starts -- `service_core.cpp`
clears every action after `publish_imported_ready_profile` for an import
(`:3529`). The action is named *read-only*; mounting a disk is not.

Put to the user, who ruled for a separate command. So `inspect_bundle` joins the
protocol: operation-scoped, answered with the same bare status, and running no
lifecycle at all. Its reply is distinguishable from a start's by the fact that
matters -- `worker_alive` is false -- and the session deliberately does not set
`started_`, so a later status read is refused, which is the truth about a worker
that started nothing.

The evidence import needs was always obtainable without a runtime: the image id
and geometry come from the same `inspect_image` a start uses, and the file
identity from one read-attribute handle. §1.18's capture is now the one function
behind both -- `record_bundle_identity()` -- so an inspection cannot verify
something a start would have refused, or report it differently.

**The worker's status is now the *observed* image, not the claimed one.**
`observe()` reported `config.image_id` and `remove_and_stop` compared a removal
authorization against it. Both use the identity the worker read and verified out
of the bundle now, which is the fact a removal authorization is supposed to be
checked against. That moved in unit B and is restated here because this unit is
where a launch can be asked for no image at all.

**Evidence.** Release build clean after a full object rebuild; no compiler
warnings. Full suite **143/144**, the one failure the pre-existing
`integration.m3.recovery` (§1.18). `unit.m5.worker_protocol` 33/33 → **35/35**,
`unit.m5.worker_session` 23/23 → **25/25**, `unit.m5.service_worker_control`
8/8 → **13/13**, `m5.production_execution_backend` 5/5 (unchanged; its create
case arrives with unit D).

**A hazard this unit removed rather than introduced.**
`m5_backend_adapter_reaches_the_worker_it_launched` used a `provisioning` profile,
which was harmless while `start()` was hardcoded to recover -- it failed on the
bundle kind and touched nothing. With the mode now following the lifecycle, the
same case runs the **create** lifecycle: it would provision a real volume and
claim a real drive letter as a side effect of a case whose only assertion is that
the worker was reached. The fixture's lifecycle now defaults to `ready` so the
case adopts and fails on the bundle kind exactly as before. Checked afterwards
that the run left no drive letter and no AIM disk behind. The mount-target case
moved out of this suite entirely: the rule lives in the translation now, and
`unit.m5.service_worker_control` covers both producers plus the mount-kind case
the old one never checked.

#### What this unit does not settle, recorded so it is not discovered later

**A resumed create would launch in `provision_new` and fail on its own bundle.**
The mode follows the durable lifecycle, and a create interrupted between
`publish_provisioning_profile` and `publish_ready_profile` leaves a profile that
is still `provisioning` -- while the bundle it already created has generation
zero, and `DiskRuntime::create` requires an *empty* bundle. It fails closed (the
runtime refuses the mode/kind mismatch rather than provisioning over the
existing generation), so nothing is destroyed, but the resume does not complete.
Choosing the criterion that fixes it -- the durable lifecycle, or what the bundle
actually holds -- is an ordering-table question, and §7 item 22 already assigns
that to Task 6, which owns `service_core.cpp`.

### 1.28 Task 5 increment 15 (unit B) — the launch decides which lifecycle runs

`DiskWorkerRuntime::start()` was hardcoded to `DiskRuntime::recover`, which can
only adopt a bundle a previous run already provisioned. That is §1.25's defect,
and this is the half of it that lives in the worker.

**The mode travels in the launch spec, and its criterion is copied rather than
invented.** `WorkerVolumeMode` has two values, `adopt_existing` (the zero value)
and `provision_new`, and the line between them is the profile's own lifecycle --
which is exactly what `append_start_actions` branches on, where `provisioning`
means "this bundle has no volume yet". `adopt_existing` is the default
deliberately: a launch that failed to carry a mode must not be the kind that
creates anything.

**The mode is not the only thing that decides.** `DiskRuntime::start()` already
classifies the bundle and requires `empty` for a create and `manifest` for a
recover, so a mode that disagrees with what is actually on disk fails closed
inside the runtime rather than quietly taking the other path. The worker chooses
which call to make; the runtime is what refuses a choice reality contradicts.

**The expected image had to become optional, and the reason is not
convenience.** `BundleEvidenceSource::read_only_inspection` means "record what
the bundle turns out to hold"; every other path means "this bundle must hold
this image". A zero uuid used as a sentinel would make those one message, so the
launch spec carries the uuid behind a presence bit and the worker verifies the
claim *or* adopts what it finds. It is refused when a create carries no image at
all, because a create has no bundle to inspect and would otherwise invent the
durable identity the service is about to record.

**The image id now reaches generation zero, which is what made the create path
possible at all.** `M4PersistenceAdapter` generated a random image uuid, so a
worker-created bundle could never have carried the id the service derived from
the create request -- and `valid_bundle_evidence` (`service_core.cpp:419`)
requires exactly that equality. `M4PersistenceOptions` therefore regained the
optional image uuid the historical adapter had, and it is threaded from the
launch spec through `ProductionRuntimeAdapter::Config` down to the
`GenerationZeroCreateRequest`. This reaches into Task 4's `src/run`; §7 item 23
records it as a deviation.

**One more thing moved, because it was the wrong value.** `observe()` reported
`config.image_id` -- the image the service *claimed* -- and `remove_and_stop`
compared a removal authorization against it. Both now use the identity the
worker actually read and verified out of the bundle, which is the fact a removal
authorization is supposed to be checked against.

**Evidence.** Release build clean after a full object rebuild; no compiler
warnings. Full suite **143/144**, the one failure the pre-existing
`integration.m3.recovery` (§1.18).

**New coverage.** `unit.m5.worker_private_pipe` 8/8 → **11/11**: both modes
round-trip as themselves, an absent image stays distinguishable from a zero one,
and a mode this protocol does not define is refused *on the way out* as well as
on the way in. One branch is deliberately unreachable from a test and is named
here rather than left implicit: the decoder's own mode check exists for a peer
that disagrees with this build, and `encode_launch_spec` now refuses the same
value first, so no case in this tree can hand the decoder one.

`unit.m4.persistence_adapter` 3/3 → **4/4**: a generation-zero created with a
named image uuid reports that uuid, and recovery re-reads the same identity from
the bundle's own bytes rather than from anything the caller remembered. This is
the first test of the new plumbing, and it is where the "free space could not be
queried" failure was found -- `M4PersistenceAdapter` claims a manifest inside a
bundle directory it does not create, so the test has to make one, exactly as
`ProductionRuntimeAdapter::create_generation_zero` does.

### 1.27 Task 5 increment 15 (unit A) — the `create_bundle` command exists

Lands the first of §1.26's five changes: the private command itself, its shape,
the worker's answer to it, and the service's client for it. The launch-spec
mode, the worker runtime's create/recover choice and the adapter's two bundle
actions are units B and C; the end-to-end case is unit D.

**A command whose reply is a status, so no payload type was added.**
`WorkerRuntimeStatus` already carries `WorkerBundleEvidence` (§1.18), so
`create_bundle` is operation-scoped with a bare status reply, exactly as `start`
is — the shape table gained a command, not a second evidence shape that could
drift from the first.

**`create_bundle` and `start` reach the same runtime call, and that is stated
rather than hidden.** The runtime offers one granularity: `DiskRuntime` decides
create-or-recover from the bundle's own kind and the mode it was constructed
with, and a bundle with no volume checkpoint cannot be recovered at all (§1.25).
What makes this a distinct command rather than a synonym is the promise it
carries and `start` does not: **that the bundle's identity was recorded.** The
worker refuses to answer `create_bundle` with a status whose evidence is absent
or incomplete, and the service's client refuses such a reply again on receipt.
A success here means the service may record a registration; a success from
`start` means only that a runtime came up.

**A started-but-unrecorded runtime is reported as an error, not as a state that
did not happen.** The session sets `started_` as soon as the runtime is up and
*then* checks the evidence, so the refusal does not claim the worker is
unstarted while a volume is mounted. The teardown path depends on knowing which
it is, and a session that lied about its own state to make a reply tidier would
be the worst kind of correctness.

**Evidence.** Release build clean after a full object rebuild (every `.obj`
deleted first, per §7 item 17). Full suite **143/144**, the one failure the
pre-existing `integration.m3.recovery` (§1.18). Standalone: `unit.m5.worker_protocol`
31/31 → **33/33**, `unit.m5.worker_session` 21/21 → **23/23**.

**What the new cases pin, and one thing they deliberately cannot.** The protocol
suite checks that the reply round-trips with its evidence and that the command is
operation-scoped — a request without an operation id is refused by the
authentication gate, and one carrying a status is refused as a stray field. The
session suite checks that a runtime with evidence is answered with it, and that a
runtime *without* evidence is refused while still being reported as started.
What no case here covers is the command over a real channel: `ServiceWorkerSession`
is concrete over an authenticated pipe and cannot be faked, so that arrives with
unit D's end-to-end run.

### 1.26 The create path: a `create_bundle` private command, designed

Ruled by the user: add a private protocol command so `create_and_verify_bundle`
becomes realizable. The design below is settled; the code is not written yet, and
this is recorded so it is not re-derived.

**The interface fact that made it possible.** `create_and_verify_bundle` runs
*before* `publish_provisioning_profile`, so at that moment no durable profile
exists -- and `bind_worker` requires one to launch. But `WorkerActionRequest`
also carries `lifecycle_intent`, and `LifecycleOperationIntent` holds
`profile_id`, `expected_image_id`, `bundle_path`, `mount_target` and
`requested_capacity_bytes`. That is everything a `WorkerLaunchSpec` needs. So the
adapter can launch a worker for a profile that is not durable yet, and the
ordering the plan already has does not have to move.

**The command.** `create_bundle`, operation-scoped like the other mutations. Its
reply needs no new payload type: `WorkerRuntimeStatus` already carries
`WorkerBundleEvidence` (§1.18), so the reply is a status with the bundle
engaged, and the shape table gains one bit rather than a second evidence shape
that could drift from the first.

> **CORRECTED by §1.29.** The no-second-evidence-shape half holds. The claim
> below that `inspect_bundle_read_only` "follows the same path for import, where
> there is no provisioning to do" does not: opening a bundle in this runtime
> means recovering it, and recovery **assigns a drive letter**. A read-only
> action realized that way would mount a volume the import plan never asks to
> mount. On the user's ruling the protocol gained a second command,
> `inspect_bundle`, which runs no lifecycle at all. §1.29 has the reasoning.

**What the worker does for it.** `DiskRuntime::create` plus `start()` performs
the whole sequence -- generation-zero, AIM start, provision, verify, flush,
checkpoint -- in one call, because that is the only granularity the runtime
offers. So `create_bundle` realizes **both** the bundle's creation and its
provisioning, and the plan's later provisioning-family steps keep their existing
mapping: `verify_startup_step` reads a status that is now a started runtime's,
which is the same "proven by the authenticated start" boundary the startup-step
ruling already established (§7 item 19).

**Why this is the right slice rather than a convenient one.** The alternative --
having the worker create only generation-zero and letting the later `start`
provision -- does not work: a bundle with no volume checkpoint cannot be
`recover`ed, which is exactly the defect §1.25 found. Create and provision are
one operation in this runtime, and the protocol should say one operation.

**What changes, in order.** The launch spec gains what selects the mode (the
profile's lifecycle already distinguishes a fresh disk from an adopted one, and
`append_start_actions` in `state_machine.cpp` branches on precisely that). The
worker's runtime keeps the mode and `start()` picks `DiskRuntime::create` or
`::recover` accordingly. `create_bundle` is added to the worker's command loop.
The adapter's `create_and_verify_bundle` becomes: launch from the intent, send
`create_bundle`, map the returned evidence into `BundleRegistrationEvidence` with
the source the evidence actually came from. `inspect_bundle_read_only` follows
the same path for import, where there is no provisioning to do.

**The end-to-end case from §1.25 comes back when this lands**, and is the thing
that proves it: create a bundle, launch, `start`, observe, `cleanup`.

### 1.25 The end-to-end attempt, and the defect it found

§1.24 established that the lifecycle can be driven here. This is the attempt, and
it did not reach a mounted volume -- but it converted a vague gap into a named
defect, which is the useful outcome.

**What was built.** A case in the backend suite that creates a real
generation-zero bundle, claims a free drive letter, launches the worker through
`WindowsWorkerSessionProvider`, and calls `start`. Two harness mistakes were
found and fixed on the way, both worth keeping because both hid themselves well:
`std::wstring mount{1, letter}` picks the **`initializer_list` constructor** and
builds a two-character string (`0x01` then the letter), which then fails the
mount-target check for a reason that looks nothing like the mistake; and the
constants the bundle fixture needs are split across `myrd`, `myrd::persistence`
and `myrd::storage`, so an unqualified name resolves or does not depending on
which `using` is in scope.

**The defect.** With both fixed, the worker launched, authenticated, and `start`
failed with:

```
op=recover_complete_image
msg=generation-zero bundle has no durable Windows volume checkpoint
```

`DiskWorkerRuntime::start()` calls **`DiskRuntime::recover`**. It can only adopt a
bundle that a previous run already provisioned and mounted. **Nothing in M5
provisions one**: there is no create path in the worker, and the plan's
`create_and_verify_bundle` -- which is where provisioning belongs -- is
fail-closed in the adapter pending Task 6's ordering decision.

So §1.22's functional gap #3 is not a configuration problem or a missing test
seam. It is that **the operation which turns a fresh bundle into a startable one
has no implementation anywhere**, and the ordering argument about
`create_and_verify_bundle` has been about a step nobody wrote. That is the
concrete thing Task 6 now has to settle, with the failing call site as the
evidence.

**The case was removed rather than committed failing.** It is not in the tree:
the framework has no skip, and asserting today's failure would enshrine a defect
as the expectation -- the same mistake §1.17 caught in an older assertion. It
comes back when the create path exists, and its shape is recorded here so it does
not have to be rediscovered: generation-zero bundle, free drive letter, launch,
`start`, `require_runtime_observation`, `cleanup`.

### 1.24 The end-to-end run belongs on this host, not the VM

Checked before planning the work, because it changes where it happens.
`integration.m3.windows_acceptance` is **not** behind an environment gate, it
runs in the normal suite, and it passes. Its `m3_aim_runtime_checks_context_before_driver_discovery`
case calls `M3AimRuntime::start()` and asserts success -- and driver discovery
happens before that point, not after. Independent confirmation:
`sc query phdskmnt` reports `RUNNING` on this host.

So the full lifecycle -- AIM proxy disk, real volume, mount, flush, checkpoint --
can be driven **here**. The acceptance VM ([[m5-vm-access-method]]) is for
driving the installed *service* through the public pipe, which is a different
question: this is the execution path below it.

**What does not exist yet, and is therefore the work.** No suite drives a full
lifecycle: `m3_provider_integration_tests` uses a `CapturingTransport` fake,
`m4_multi_instance_tests` and `m4_run_loop_runtime_tests` build `ProductVolume`
values by hand, and `m3_windows_acceptance_tests` exercises the AIM runtime's
validation and metrics rather than a volume. The closest thing to a bundle-
creation harness is `m5_checkpoint_inspection_tests`' `TempBundle` +
`create_generation_zero_image`, which is where the end-to-end case starts from.

**Side effects that must be stated before running it, not discovered after.** A
real lifecycle creates an AIM proxy disk backed by a temporary VHDX, provisions
and formats a volume, and assigns it a drive letter. It is a virtual disk, not a
physical one -- but it is a real volume with a real letter on this machine, and a
failure part-way can leave a residual device (the failure mode §1.15a's
`IResidualDeviceControl` exists to clean up). The case must therefore claim a
letter it has verified is free and release it on every path.

### 1.23 Task 5 increment 14 — the durable correlation can carry a recovery proof

Lands the widening §1.22 decided, and with it the last fail-closed action in
`IWorkerControl`.

`CheckpointCommitBoundaryEvidence` gains a manifest slot and a bytes digest,
appended so every existing aggregate initializer keeps its meaning.
`recovery_recoverable()` is the gate: a zero digest is the *absence* of the fact
a recovery proof is made of, not a digest that happens to be zero, and the
adapter refuses on it rather than comparing it.

**The migration, and why the version gate is not where the safety lives.**
`profile_registration_codec.cpp` required an exact schema minor match, so the
minor is bumped to 1 and the reader accepts `[0, 1]`. The payload is not
self-describing, so `unwrap` now carries the minor out to the `Reader`, which
`read_checkpoint_correlation` consults to decide whether the two fields are
present. A record from the previous minor decodes with them absent. **That is the
whole migration, and rejecting those records would have been data loss.**

Where the safety actually lives is `recovery_recoverable()` plus the adapter's
refusal, not the version gate. A version gate can only say "these bytes are not
the shape I expect"; it cannot say "the fact you need is not in them". A legacy
correlation loads, is usable for everything it does carry, and *cannot* be
mistaken for a proved one.

**`reconcile_checkpoint` is realized**, through `recovery_proof`. The worker
judges the bundle; the adapter relays which of the three answers came back and
does not decide what it implies -- promotion, rollback or parking is ServiceCore's
table, and inventing one here would be deciding it in the wrong layer. A proof
that could not be *taken* is a failure, not `ownership_unprovable`: "could not
ask" must not become a finding about the bundle.

**A process error of mine, caught by the suite.** After the header change the
full run showed `m5.production_execution_backend` dying with `0xC0000409` -- the
CRT abort that §7 item 17 names as the stale-object signature. I had edited
`service_core.h` and rebuilt incrementally **without deleting the objects
first**, which is the exact rule that item exists for and the third time it has
been violated in this work. Deleting `out/build/msvc/**/*.obj` and rebuilding
fixed it with no source change. The rule is not advisory and this is the
recorded instance.

**Evidence.** Release build clean **after a full object rebuild**; full suite
**143/144**, the one failure being the pre-existing `integration.m3.recovery`
(§1.18). `unit.m5.registration_codec` is 3/3 and
`unit.m5.service_worker_control` is 8/8.

**New coverage where there was none.** Nothing in the suite included the
registration codec header before this increment -- the durable format had no
direct test at all, and the migration would have been its first. It now has
three cases: the new fields round-trip; a record rewritten as the *previous*
minor decodes with the fields absent rather than invented (the assertion the
migration turns on, and built by locating the appended bytes by value because the
correlation is not last in the payload); and a minor from the future is refused,
because a payload read as if it ended where this build expects would decode
trailing bytes into whichever field came next.

### 1.22 Task 5's scope closes — and the three rulings that close it

Task 5's file list in the plan names one file that is not built here and one it
no longer needs. Both were put to the user with the caller evidence, and both
were ruled on rather than assumed.

**`windows_worker_recovery.{h,cpp}` moves to Task 7 whole.** The evidence is the
caller set: every one of its five exports is called only by
`windows_service.{h,cpp}`, `worker_supervisor.{h,cpp}` and
`service_composition.h` -- and the plan's own ownership table assigns all three
of those to Task 7. Task 5 has **no caller for any of it**. Building it here
would have meant writing five functions nothing in this task uses, two of which
(`SystemServiceClock`, `DurableWorkerRestartHistoryStore`) are Task 7 adapters --
settling Task 7 questions inside Task 5 files.

**`load_persisted_worker_status` is deleted, not ported.** It read a *persisted
worker status file*, and the M5 stack has no such artifact: a search for
`worker_status`, `WorkerStatus` and `runtime_snapshot` outside the worker's own
files returns nothing. In M5 the worker's status is live over the authenticated
channel. The function had no counterpart to rebuild against, so it is recorded as
a HostCore artifact M5 does not have rather than translated into something that
looks equivalent.

**The durable correlation is widened.** `WorkerRecoveryCorrelation` needs a
manifest slot and a bytes digest; `CheckpointCommitBoundaryEvidence` carries
neither, and `CheckpointDurableState` persists that same type -- so the
correlation cannot be reconstructed even from the durable record, which is
precisely when recovery matters. `reconcile_checkpoint` therefore stays
fail-closed until this lands, and §7 item 19's correction stands as the record of
why.

**That widening is a durable-format migration, and the shape matters.**
`profile_registration_codec.cpp:646` requires an exact schema minor match
(currently 1/0), so the two new fields cannot simply be appended: the minor is
bumped and the reader accepts **both** the old minor (fields absent, left at
their defaults) and the new one. The honest cost, stated rather than discovered
later: a correlation recorded before the upgrade carries no slot and no digest,
so an operation left in flight by the older build cannot have its ownership
proven afterwards. For M5 that means test artifacts, but the decoder must not
mis-read them silently, and a legacy correlation must fail closed rather than
compare as a match against zeros.

**Task 5's *scope* is now closed. Its outputs are not finished, and "complete"
would be the wrong word for them.** The distinction matters because the two were
conflated when this section was first written, and the correction is the useful
part:

*What is closed:* every file the plan listed for Task 5 either exists or has been
explicitly re-homed, and no file is left un-owned. That is a statement about
ownership, not about working software.

*What is open, and how open:*

1. **Three real functional gaps.** `create_and_verify_bundle` and
   `inspect_bundle_read_only` are fail-closed pending Task 6's ordering decision,
   so **M5 `create` and `import` do not work**. `attach` returns `nullopt`
   pending Task 7's live-worker registry, so `recover_bundle` does not work. And
   the worker's `start()` has never succeeded anywhere, so
   `create_runtime_and_worker` does not work either.

   > **RESOLVED for `create`, by §1.29–§1.30.** `create` works end to end and is
   > proved by a real lifecycle. It needed three fixes, not the one §1.25 named:
   > the disk mode (§1.28), a zero RAM budget that killed every start before a
   > bundle was opened, and a provisioning resource missing from the worker
   > module (§7 item 25). `import` works too, through a read-only inspection
   > rather than the mounting path §1.26 assumed (§1.29). Two gaps remain from
   > this list: `attach` still returns `nullopt` (Task 7's live-worker registry),
   > and `recover_bundle` with it. The launch also still does not carry the
   > profile's memory policy.
2. **The adapter's positive paths have never executed.** `quiesce_mutations`,
   `flush_exact_volume`, `checkpoint_across_boundary`, `remove_mount`,
   `confirm_removal`, `verify_startup_step`, `record_volume_identity`,
   `resume_mutations`, `bind_worker`'s success branch and `reconcile_checkpoint`'s
   success branch are all exercised by nothing. The model tests drive their own
   fake `IWorkerControl` (`m4_checkpoint_operation_tests.cpp:719`), and the
   backend suite asserts `create_runtime_and_worker` *fails*. The production
   data-plane pieces are in the same state: `capture_bundle_evidence`,
   `fill_persistent_identity`, `DiskRuntime::adopted_volume`.
3. **One planned file is not here.** `windows_worker_recovery.{h,cpp}` was in
   Task 5's list and moved to Task 7 by ruling. The plan's list is not satisfied;
   it is amended.

So the honest status is: **Task 5's scope is settled, three of its capabilities
do not work yet, and most of its code has never run.** The remaining work is
Task 6 for the first gap, Task 7 for the second and third, and the same backend
suite extended with a real bundle and volume for the verification.

### 1.21 Task 5 increment 13 — the backend suite that executes increments 10-12

`tests/integration/m5_production_execution_backend_tests.cpp`, the suite §1.20
called out as owed. It starts a **real `myrd-m5-worker` process** and drives it
over the private channel, so the launcher, the session provider and the adapter
stop being reviewed-but-unexecuted code.

**The scope boundary is the deliberate part.** The worker's `start` command runs
`DiskRuntime::start()`, which needs an AIM proxy disk and a real volume -- the
acceptance environment, not a unit harness. So the cases drive the worker up to
and including the authenticated session and stop there. What did not exist before
this file is the service-to-worker hop: process launch, inherited startup
channel, pipe ACL, handshake, binding. That hop is now fully exercised.

**The assertion that makes the evidence real rather than plausible.** The first
draft asserted `!status.ok()` for a worker whose runtime was never started, and
that is satisfied by *any* failure -- including one that never reached a worker.
The case now asserts `status.error().operation == L"observe"`: `observe` is the
operation name the worker's own command loop uses when it refuses a status for a
runtime it has not started, so an error carrying it can only have come from that
loop, over the authenticated channel. A launch that never happened, or a
handshake that silently no-opped, fails on that line instead of passing on a
bare `!ok()`. The initial run passed in 0.14 s, which is exactly the kind of
result that deserves a second look rather than a commit.

**Six cases, 6/6:** a real launch authenticates and reports the child's own pid
and creation time; two profiles get two distinct processes and two distinct
bindings; a launch with no executable is refused; a durable mount target that is
not a drive letter is refused *before* a process exists; `attach` reports "none
running" rather than pretending; and the adapter reaches the worker it launched
-- asserted as "the failure is the runtime's, not `execution_not_bound`", which
is the only thing that separates a wired-up adapter from an unwired one when both
fail.

**One build detail worth recording.** The worker's path cannot be baked into a
C++ string literal: an absolute Windows path has backslashes, and `\U` in
`C:\Users\...` is not an escape sequence -- C4129, promoted to an error by
`myrd_enable_warnings`. `$<SHELL_PATH:...>` inside `target_compile_definitions`
did not convert it either. The path is therefore passed *relative* with forward
slashes via `$<CONFIG>`, resolved at run time against this module's own
directory, and asserted to exist so a layout change fails with the path it tried.

**Evidence.** Release build clean after a full object rebuild; full suite
**142/143**, the one failure being `integration.m3.recovery` (§1.18).
`m5.production_execution_backend` is 6/6.

### 1.20 Task 5 increment 12 — the worker launcher and its provider

Two pieces, split by what each one knows.

`launch_private_worker` lives in `src/host/m5_worker_private_pipe.{h,cpp}`
because it is the thing that ties that file's three existing primitives together
and nothing else: it creates the startup channel, creates the pipe server,
starts the process, writes the spec, and does not return until the handshake has
authenticated the peer.  It knows nothing about profiles or durable state, so the
protocol target stays free of `src/management`.

`WindowsWorkerSessionProvider` lives in
`src/management/windows_worker_sessions.{h,cpp}` and is the translation the other
piece deliberately omits: `profile::ProfileRecord` → `m5::WorkerLaunchSpec`.  It
is the only part that knows about profiles, so it is the only part that has to be
tested against them.

**Three decisions inside the launcher, each with a reason.**

Inheritance is restricted to the one handle the worker needs, via
`STARTUPINFOEX` and `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.  A plain
`CreateProcessW(bInheritHandles = TRUE)` hands the child every inheritable handle
the service holds, and each of those is a thing the child could then be made to
act on.

Every failure path terminates and waits for the child before returning, through a
guard that owns it until the caller takes it.  A half-launched worker still holds
the pipe name and, once it has read the spec, the bundle -- and a process nobody
kept a handle to is a process nobody can stop.

The budget is the context's deadline rather than a separate `timeout_ms`
parameter.  That parameter existed in the first draft and the compiler was right
to reject it as unused: two bounds on one wait can disagree, and only one of them
was ever consulted.

**One thing built and then removed, recorded because removing it was the point.**
`wait_for_worker_exit` was written alongside the launcher, mirroring the
historical adapter's use of it to prove a removal against a *dead process*.  The
M5 adapter does not do that -- a removal is proven by the worker's own
authenticated answer -- so nothing would have called it.  It was deleted rather
than shipped unused, with the note that it returns with the live-worker registry
that needs it.

**`attach` returns `nullopt`, and that is a scope statement rather than a stub.**
Adopting a worker this provider did not launch needs a registry of live workers,
which belongs to the service composition: the adapter already holds its own
sessions, and this provider owns none.  A successful `nullopt` means "none
running", which the adapter turns into an explicit `execution_not_bound` refusal.
`recover_bundle` therefore fails today, and will keep failing until Task 7
supplies the registry.  Saying so is better than a provider that appears to
adopt and does not.

**Evidence.** Release build clean after a full object rebuild; full suite
**141/142**, the one failure being `integration.m3.recovery` (§1.18).
`structural.m5.execution_boundary` passes, so neither new piece reached a
forbidden target.

**Not covered, and the reason is structural rather than an omission.** Both
pieces are unexecuted: the launcher needs a real worker process and a real
bundle, and the provider needs a durable profile.  Neither can be exercised
without the production backend suite, which is increment 14 -- the same suite
that covers the adapter's action mapping.  Until then they are
reviewed-but-unexecuted code, and this is the third increment in a row with that
property; it is worth stating plainly rather than letting the pattern pass
unnoticed.

### 1.19 Task 5 increment 11 — ServiceWorkerControl, the Decision A seam

`src/management/service_worker_control.{h,cpp}` and
`src/management/windows_worker_observation.h`, in `myrd_management`. The sole
`IWorkerControl` implementation, rebuilt on `ServiceWorkerSession` over the
private protocol. `structural.m5.execution_boundary` passes, so the adapter
reaches a worker without regaining the execution coupling the check exists to
prevent.

**Two things the durable boundary evidence does not carry, found while writing
it.** `CheckpointCommitBoundaryEvidence` is what the durable layer persists as
P4, and it has **no manifest slot and no bytes digest** -- but the worker refuses
a commit that does not repeat exactly the candidate `prepare` froze. So the
adapter keeps the whole `WorkerPreparedCandidate` alongside the evidence in its
held candidate; without it the commit could never be issued at all. The same
absence is why `reconcile_checkpoint` stays fail-closed (§7 item 19 is corrected
below): building a recovery correlation needs the slot and the digest, and there
is nothing to build one from.

The other: the worker reports its image as `persistence::ImageUuid`, which is raw
bytes, while the durable boundary evidence and the profile hold `product::Uuid`.
The comparison goes through one conversion function, and it goes through
*canonical text* -- `Uuid` has no byte-level factory, so text via `Uuid::parse`
is the only bridge that cannot disagree with the one canonical form.

**Two scope decisions, both stated in the header rather than left implicit.**
`create_and_verify_bundle` and `inspect_bundle_read_only` are **fail-closed**
here. The worker now produces the evidence they must return (§1.18), so the data
exists; what is unsettled is *when*, because the plan places both before
`create_runtime_and_worker` while in M5 the bundle is created by the worker's own
`start()`. That is orchestration, and Task 6 owns the ordering table. Choosing
one option here would settle a Task 6 question in a Task 5 file.
`reconcile_checkpoint` is likewise not overridden.

**§7 item 19 corrected.** It claimed `reconcile_checkpoint` becomes realizable
through `recovery_proof`. It does not: the correlation needs the manifest slot
and the bytes digest, and the evidence the method is handed carries neither. The
claim was written from the protocol's capabilities without checking what the
caller actually receives, which is the same mistake the reconciliation pass was
written to stop making.

**Evidence.** Release build clean after a full object rebuild. Full suite:

```
ELEVATED=True
141 tests passed, 1 tests failed out of 142
```

142 = 141 + `unit.m5.service_worker_control`. The one failure remains
`integration.m3.recovery` (§1.18).

**What the new suite covers, and what it deliberately does not.**
`unit.m5.service_worker_control` is 7/7: the whole fail-closed action set
(`compact`, the config publishes, the rebind pair, `forget_metadata`, and the two
bundle actions) is asserted to fail *without contacting a worker*;
`reconcile_checkpoint` is asserted fail-closed; `prove_new_uninitialized` refuses
without a durable profile; `observe_runtime` reports an unbound profile as
*observed gone* rather than as "not observed"; strict observation refuses when
unbound; and the observation mapping is pinned in all three liveness
combinations.

It does **not** cover the action mapping, the checkpoint boundary or the removal
proof, because `ServiceWorkerSession` is a concrete type over an authenticated
channel and cannot be faked -- and faking it would mean testing a different
object than the one that runs. Those paths need a real worker and belong to
`m5_production_execution_backend_tests`, which is increment 14. The suite's own
header says so, so the gap is visible from the test rather than only from this
document.

### 1.18 Task 5 increment 10 — the worker produces the bundle evidence

Implements the ruling recorded in §7 item 22. ServiceCore requires a
`BundleRegistrationEvidence` from `create_and_verify_bundle` and
`inspect_bundle_read_only` and fails the operation without one; the management
layer may not open a bundle, and the private protocol had no way to ask. The
worker opens the bundle anyway — `start()` already re-reads its image uuid — so
the evidence travels in the status, exactly as the volume identity does.

**What is on the wire, and what deliberately is not.** `WorkerBundleEvidence`
carries the image uuid, the three geometry fields and the bundle directory's own
identity (volume serial and 16-byte file id). It does **not** carry the manifest
name or the payload block size: those are product constants the service already
holds, and a second copy on the wire would be a second thing to keep in step.
The `source` is likewise the service's to decide — `created_and_verified` when
the worker provisioned the bundle in this session, `read_only_inspection` when
it adopted one that already existed.

**Where it is captured.** In `start()`, immediately after the inspection that
verifies the image uuid — the one moment the bundle is known to be the one the
service named. The geometry and the uuid come free with that inspection; the file
identity needs a second handle with `FILE_READ_ATTRIBUTES` and full sharing,
because the runtime already holds the writer lock through its own handle and
asking for anything stronger would deadlock the worker against itself. The flags
match the management layer's `inspect_bundle_identity`, which reads the same
identity for the same purpose, so there is one convention rather than two.

A failure to capture is a failure to start, not a start with no evidence: a
worker running without a recordable identity could be asked to remove resources
another profile owns. `complete()` is the gate, enforced in `validate_shape` as
well as at capture — a zero geometry or a zero volume serial is not a weaker
registration but one no later start could re-verify.

`observe()` reports it whenever it was captured, including after a teardown: the
evidence describes the file object this worker opened and verified, which is
still the same object after the runtime is gone, and the service has to be able
to record a registration for a profile it has since stopped.

**Evidence.** Release build clean after a full object rebuild. Full suite:

```
ELEVATED=True
99% tests passed, 1 tests failed out of 141
CTEST_EXIT=8
```

The one failure is `integration.m3.recovery`, the pre-existing failure recorded
in §7 item 5 and known independently of M5. It is **provably not** this
increment's: `myrd_m3_recovery_tests` links only `myrd_test_support` and
`myrd_persistence`; `myrd_test_support` contains `test_main.cpp` and nothing
else; and `myrd_persistence` links only `myrd_storage`, `myrd_common` and
`bcrypt`. No translation unit this work touched is reachable from that binary,
so the check is a link-graph argument rather than a re-run. Its mechanism, now
identified: the case opens a `.vhdx` with a share mode of zero to make it busy,
then requires recovery to report an operational error — and an elevated process
holding `SeBackupPrivilege` can open past a share-mode-zero holder, so recovery
succeeds and the assertion fails. It is an elevated-fails test in a suite that
has been run elevated.

**Standalone counts.** `unit.m5.worker_protocol` 29/29 → **31/31** (the bundle
evidence round-trips byte for byte, including the raw file id; incomplete
evidence is refused).

**The 141-then-140 discrepancy, resolved — and it was environmental, not a code
change.** Two full-suite runs earlier in the session reported 141/141 with
`CTEST_EXIT=0`; later runs of the same script reported 140/141, with no reachable
change in between. The cause is the privilege state of the shell, and the chain
is short enough to verify end to end:

- The failing case holds `busy.vhdx` with share mode zero to make it busy, then
  requires recovery to report an operational error.
- The recovery path opens files with `FILE_FLAG_BACKUP_SEMANTICS`
  (`image_paths.cpp:549`, `:613`, `:1495`).
- **An opener holding `SeBackupPrivilege` *enabled* bypasses the share-mode
  check.** `whoami /priv` now reports it 已启用 (enabled); an administrator token
  holds that privilege but leaves it disabled by default, which is the state the
  earlier runs were in.

So the test's premise -- that share mode zero makes a file unopenable -- is false
for exactly the opener the production code is. The earlier 141/141 readings were
correct for their environment, and the later 140/141 reading is correct for this
one; neither was wrong and nothing regressed. The test is a **pre-existing test
defect** that a privilege change exposed, not a code defect.

Current baseline: **141/142** once §1.19's new suite is counted, with
`integration.m3.recovery` as the single failure.

Closes the first of the two defects §1.15a found, and the last of the three
replies that could not report what they did. `abort` returned a bare
`WorkerRuntimeStatus`, so `aborted == false` covered both "there was no candidate
to cancel" — which leaves the bundle untouched — and "the candidate existed and
could not be cancelled", which leaves its state unknown and forces a
reconciliation. The historical reply's own comment says why that matters: the
safe handling for the first is the unsafe handling for the second.

**The reply carries a reason, not a failure context — and that is a deliberate
departure from the historical shape.** This was the unit that stopped an earlier
attempt, and the reason is worth keeping. The historical
`WorkerCheckpointAbortReply` carried a `WorkerCheckpointFailureContext`;
`M3Provider::abort_checkpoint` and the transaction abort beneath it **both return
`Result<void>`**, so there is no context to relay. The only way to fill the field
would be to compose one from the bare `Error` — naming a persistence failure
that never happened, which is the exact fabrication this whole seam exists to
prevent. So the reply carries `WorkerAbortRefusal` (`no_candidate` /
`abort_failed`) instead. What the caller needs is the *distinction*, not the
detail, and the distinction is what it now gets. The detail becomes available
when the layer below can supply a real context; until then its absence is
recorded rather than papered over.

`generation` travels on both paths, because the caller checks that a successful
abort left the committed generation where it was — a caller that could not read
it could not make that check at all.

**Evidence.** Release build clean after a full object rebuild; full suite:

```
ELEVATED=True
100% tests passed, 0 tests failed out of 141
CTEST_EXIT=0
```

Standalone: `unit.m5.worker_protocol` 27/27 → **29/29** (both abort refusals
round-trip and stay distinguishable; a self-contradictory abort reply is refused
in both directions), `unit.m5.worker_session` **21/21** (a failed cancel is
relayed as an answer, not as a transport error),
`unit.m5.service_worker_session` **6/6**.

**One assertion had to be inverted, and that is the point.** The existing case
for a failed abort asserted `reply.error.has_value()` — it encoded the defect as
the expectation. It now asserts the refusal arrives as a reply, with the
distinction intact. A test that asserts the collapsed answer will keep passing
after the collapse is fixed unless someone reads it.

**The elevation gate is gone in this environment.** Three suites that
§7 item 5 lists as unelevated failures — `m5.service_operation_store`,
`unit.m4.profile_store`, `integration.m3.provider` — now pass when run directly,
and the whole suite runs without the elevated launcher. The eight-test unelevated
set is therefore historical, not a property of the tree; it described the shell,
not the code.

### 1.15a The reconciliation pass — every host fact the rebuilt files consume

Written because increments 7 and 8 both discovered a gap that a single upfront
diff would have found, and because the plan never asked the question. The plan
measured the *coupling* ("9 files, 306 host references") and derived its
port/rebuild split correctly from it, but the historical adapter is also a
**specification**: the host fields it reads are exactly the facts a worker must
be able to supply. Nothing diffed that specification against what the new stack
provides, so each gap surfaced one at a time, each costing a build and an
elevated suite.

This pass took the three rebuilt files (`service_worker_control.{h,cpp}`,
`service_worker_session.{h,cpp}`, `windows_worker_recovery.{h,cpp}`, plus
`windows_worker_observation.h`), extracted every `host::` symbol and every field
access mechanically, and compared each against `m5::` equivalents. The result:

**Realized, confirmed equivalent.** `prepared` / prior / candidate / native id /
cut epoch (`m5::WorkerPreparedCandidate` adds manifest slot and candidate digest,
which is strictly more); `commit_reply.disposition` / `generation` / `failure`
(§1.15); `committed_generation`, `dirty`, `dirty_epoch`, `active_operation_id`,
`active_operation_phase`; the binding's `worker_pid`,
`process_creation_time`, `session_generation`; and `image_id`, which moved from
the binding to the status and is now *verified* rather than asserted (§1.12).

**Deliberately dropped by decision.** The device geometry family —
`device.length_bytes`, `physical_drive_number`, `port_number`, `target_id`,
`path_id`, `lun`, plus `aim_device_id` and `proxy_native_path`. These were
per-field checks on the startup steps; §7 item 19 records the ruling that the
authenticated, image-verified `start()` is the proof instead. The identity
triple from the same struct is *not* dropped — §1.14 produces it.

**Two new defects, both siblings of the one §1.15 fixed.** The commit reply was
not the only reply that could not express its own outcome:

- `abort` returns a bare `WorkerRuntimeStatus`. The historical
  `WorkerCheckpointAbortReply` carried `aborted`, `generation` and a failure
  context, and its comment says exactly why: "an abort that failed is not one
  failure but two, and the caller has to be able to tell 'there was no candidate
  to cancel' from 'the candidate existed and could not be cancelled'. The first
  leaves the bundle untouched; the second leaves its state unknown and is what
  forces a reconciliation." The M5 shape collapses them, so
  `abort_held_candidate` cannot distinguish a benign no-op from a forced
  reconciliation — the same class of error, in the path next door.
- `remove_and_stop` returns a bare `WorkerRuntimeStatus`. The historical reply
  carried `removed`, `stopped` and `generation`. Without them the adapter cannot
  perform its own `removal_generation_mismatch` check, cannot enforce that a
  reply claiming removal also claims the stop (the historical code refuses
  `removed-without-stopped` on the wire), and cannot record the generation the
  removal was proven at. Separately, the dirty refusal added in §1.15 reports a
  generic `runtime_error`; the historical protocol had
  `RemoveAndStopAuthorization::runtime_dirty` as a **first-class answer**. That
  makes the refusal correct in effect but opaque in reporting — the very
  collapsing this section is about, committed by this section's own fix.

**One capability to re-home.** `host::IResidualDeviceControl`
(`enumerate_devices`, `remove_if_still_exact`, with `ResidualDeviceIdentity` =
ownership marker + physical disk identity, and a six-value
`ResidualDeviceDisposition`) is a whole reconciliation subsystem inside
`windows_worker_recovery`. The new stack has the primitive underneath —
`src/driver/direct_aim_adapter.h` offers `list_devices`, `query_device` and
`remove_device` — but `myrd_management` may not link `myrd_driver`, so as with
bundle evidence this belongs to the worker side. Note the historical contract's
own demand: "An ID-only implementation does not satisfy this contract", so
whatever realizes it must revalidate owner *and* physical identity immediately
before the single-device remove, not just match a device id.

**Minor.** `host::kCheckpointSentinelGeneration` (`~0`) had no M5 equivalent;
the historical adapter used it to reject a generation that cannot be advanced.
Either a shared constant or the check goes; decided with the adapter.

**Evidence.** This was a read-only pass; no build, no suite. Recorded so the
list is complete before the adapter is written against it, which is the whole
point — the previous two rounds each discovered one entry at the cost of a build
and an elevated suite.

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

23. **The create path reached into Task 4's `src/run`, and it had to.** Same
    shape as item 18 and recorded for the same reason: the change is additive
    and behaviour-preserving, and it was necessary rather than convenient.
    `M4PersistenceAdapter` generated a random image uuid, so a bundle the worker
    created could never have carried the id ServiceCore derives from the create
    request -- and `valid_bundle_evidence` refuses evidence whose image id is not
    that one, so the whole create path would have failed validation with no
    local explanation. `M4PersistenceOptions` regained the optional image uuid
    the historical adapter carried (`feature/m5-unified-integration`), and
    `ProductionRuntimeAdapter::Config` gained it as a last field so every
    existing aggregate initializer keeps its meaning. Absent means "generate
    one", which is exactly what the foreground M4 path does today, so no
    existing behaviour moved. `unit.m4.persistence_adapter` gained the case that
    pins it.

    The type is the persistence layer's `ImageUuid` rather than `product::Uuid`
    as the historical version had it. It is the `GenerationZeroCreateRequest`
    field verbatim, so the product type would only have added a conversion
    between two sixteen-byte encodings -- and the management layer already has
    the one conversion it needs, in `WindowsWorkerSessionProvider`.

24. **§1.26's single command became two: `create_bundle` and `inspect_bundle`.**
    Ruled by the user, and the reasoning is §1.29's. In short: §1.26 assumed
    `inspect_bundle_read_only` could ride `create_bundle`'s path "for import,
    where there is no provisioning to do", but the only way this runtime opens a
    bundle is `DiskRuntime::recover` + `start()`, and recovery assigns a drive
    letter. Taking that path would mount a volume for a profile the import plan
    records as `ready` and never starts, in an action whose own name says
    read-only. `inspect_bundle` therefore exists, is operation-scoped, answers
    with the same bare status, and runs no lifecycle. The shape table gains two
    entries rather than one; no payload type was added either way.

25. **Two defects the end-to-end case found, both outside Task 5's files and
    both fatal to the create path.** Recorded here because they are fixes to
    code this task did not own, and because the *shape* of each is the reason
    they survived: neither is visible without running the whole path.

    **(a) A zero RAM budget, read as a budget.** `DiskWorkerRuntime` passed
    `ram_limit_bytes = 0` to `ProductionRuntimeAdapter`, and
    `store_options_for` wrote it straight into three store limits that must be
    non-zero and strictly ordered -- so `M3Provider::open` refused every start
    with *"provider limits are invalid"*, before any bundle was opened. By the
    time the create path ran for real this was the *first* thing that happened.
    The fix is in `src/run/runtime_adapter.cpp`: zero now means "no budget to
    impose" and the production defaults stand, which is the only reading a
    literal zero can have for a field that is invalid at zero. The worker
    carries no budget because the launch spec does not carry the profile's
    memory policy -- a separate, still-open gap (§1.30).

    **(b) A process resource in the wrong module.** `run_provisioning_helper`
    reads `provision_volume.ps1` from `GetModuleHandleW(nullptr)`, the
    *executable*. `run_resources.rc` was attached to `myrd_m4_run` (a static
    library) and to `myrd`, so `myrd` had it and `myrd-m5-worker` did not: the
    worker created generation zero, started AIM, and died in
    `provision_new_volume` with `ERROR_RESOURCE_TYPE_NOT_FOUND` (1813).
    `src/CMakeLists.txt` now attaches the same `.rc` to `myrd-m5-worker`, which
    is what makes the worker a first-class driver of the same engine rather
    than a target that happens to link it.

26. **The scope question was put to the user and answered: multi-disk stays.**
    Recorded so it is not re-derived, and because the analysis is the useful
    part. The question was whether dropping multi-profile support ("one RAM disk
    per machine") would make M5 much faster to finish.

    It would make M5 *smaller* but not much *faster*, and the reason is where
    the remaining work is. From `docs/superpowers/plans/2026-09-30-m5-lifecycle-tasklist.md`:
    single-profile deletes **M5-08** ("two-profile isolation and service/worker
    restart suites") and the **M5-B** acceptance stage -- one task of twelve and
    one acceptance stage of four. It does not touch **M5-06** (the next task:
    P4/P5 core, reconciliation, dirty-truth stop), **M5-07** (scheduler, public
    pipe, CLI, service lifecycle, auto-start), **M5-C** (service restart and
    auto-start recovery) or **M5-D** (preshutdown and power-off recovery). Those
    are the durability-across-restart semantics, and they are independent of how
    many disks exist -- §3.5's "worker 退出（崩溃/被杀）" branch is a whole
    paragraph about one worker dying mid-transaction, not about N of them. Both
    of §7 item 25's defects would be hit identically by a single-profile M5.

    The multi-disk-specific machinery that does exist (the profile-keyed session
    map, the live-worker registry Task 7 owes, `materialize_memory_policy`'s
    arbitration between profiles) is mostly *already written* -- the management
    layer was ported in Tasks 1-3 and runs. Cancelling the feature does not
    refund it.

    The larger lever was put on the record too and declined: dropping the
    separate worker *process* and running execution inside the service would
    delete the whole private protocol (roughly half of Task 5's increments). It
    is a safety decision rather than a scope one -- the design treats a killed
    worker as a normal branch the service reconciles from durable state, and
    `myrd_assert_execution_boundary` would have to go -- and it is independent
    of disk count.

### 1.31 Task 5 increment 15 (unit E) — the adapter's own registration path runs

Unit D drove `ServiceWorkerSession` directly, and the unit suite drives
`ServiceWorkerControl` with a provider that can never launch. Between them,
`register_bundle`'s **success** branch had never executed: mapping the worker's
evidence into `BundleRegistrationEvidence`, deciding whether to hold the
session, converting the image id. That is this round's own code, and it was
"reviewed but unexecuted" for exactly as long as it took to look.

**`m5_backend_adapter_registers_the_bundle_it_creates`** closes it through the
adapter the service actually calls, and closes a second item from §1.22's
unexecuted list with it: `record_volume_identity`. Both run against a real
worker, a real 64 MiB volume and a real drive letter.

**What the new case proves beyond "it returns ok".** The evidence's fields are
each traceable: the geometry and the uuid are the worker's authenticated reading
of the bundle, the file identity names the file object, and the manifest name and
payload block size are the product constants the service already holds -- so a
field silently invented by the adapter would show up as a mismatch. The session
is *held* (the plan's later `create_runtime_and_worker` finds it rather than
launching a second worker for one bundle), and `record_volume_identity`'s success
proves the worker really provisioned and adopted a volume rather than merely
answering.

**It also covers the branch nobody had run: a worker killed by a closed
channel.** The case drops the adapter -- and with it the session -- without
calling `cleanup`, which is what a service crash does. The worker notices, runs
its ordered teardown, and releases the drive letter. The case waits for that
(bounded, not a fixed sleep) instead of assuming it.

**A worker that dies now says why.** `m5_worker_main`'s failure path printed only
a fixed sentence, so "the worker's resources could not be released" named the
consequence and threw the cause away -- which is the whole record when the
channel closed rather than a command failed. It now prints the error's operation,
message and codes.

#### An observation that is recorded, not resolved

On the first run of unit E's case, the worker printed that teardown failure.
After a full object rebuild it did not reappear, and **five consecutive runs of
the suite were clean**. So: observed once, not reproduced.

It is written down rather than dismissed because it is on the path the design
cares about most -- §3.5's "worker 退出（崩溃/被杀）" branch, where the only
recoverable answer is the durable record. What is known: the failure was in
`DiskWorkerRuntime::cleanup()` after a channel close, the drive letter *was*
still released (the case's wait succeeded), so the failure is in a step after the
mount removal. What is not known: which step, or whether it was a real teardown
defect or a stale-object artifact -- the same signature §7 item 17 gives a
different cause to. The next occurrence will name it, because the diagnostic is
now there.

**Evidence.** Release build clean after a full object rebuild; no compiler
warnings. Full suite **143/144**, the one failure the pre-existing
`integration.m3.recovery` (§1.18). `m5.production_execution_backend` 6/6 →
**7/7**, five consecutive clean runs.

### 1.32 Task 6 begins — §3.6's disposition table is implemented, and now pinned

The first Task 6 increment is a survey before any code, because the plan's premise
needed checking: its Step 2-4 say "Implement P4", "Implement P5 and recovery",
"Implement normal stop policy", and the ported `service_core.cpp` already
appears to contain all three.

**What the survey found, and the limit on it.** Sampled, not read end to end:
all six `CheckpointDurablePhase` values are written (admitted ×6, commit_boundary
×1, evidence_recorded ×2, committed_pending_epoch_confirmation ×1, resolving ×4,
back to stable ×4); P4's `commit_boundary` publish is at `:4369`; the §3.6
disposition table is at `:5168-5340`; the restart gate-restoration and the
operation-first crash window are at `:7167-7270`; `recover_after_restart` is
complete at `:6555`; the dirty-truth stop branches are at `:2618`, `:3976` and
`:4004`. A grep shows code *exists*; it does not show it is right or that it has
ever run. So the first increment is a test that turns the inference into
evidence.

**`m5_checkpoint_evidence_records_the_durable_phase_section_3_6_names`** drives
`ServiceCore` with the model's fake `IWorkerControl` (the harness
`m4_checkpoint_operation_tests.cpp` already provides) through all four
dispositions and asserts the durable phase and the recorded disposition. It
passes:

| disposition | durable phase after the operation |
| --- | --- |
| `committed` | `stable`, disposition `committed`, generation advanced |
| `not_committed` | `stable`, disposition `not_committed` |
| `commit_indeterminate` | `resolving` |
| `committed_needs_reconciliation` | `resolving` |

So §3.6's second table **is** implemented, and the suite already had a case that
would have passed with any of four wrong answers: it asserts only that an
uncertain disposition never reports success. That is the coarsest consequence;
these are the durable facts a restart branches on.

**The first version of the test was wrong, and the reason is the useful part.**
It expected `committed` to leave `committed_pending_epoch_confirmation`. It does
not: the phase is written at `:5491`, the epoch is published in the same session
at `:5507`, and a landed confirmation moves it to `stable` at `:5548`. So
`committed_pending_epoch_confirmation` is a **transient**, and it survives a
restart only when the epoch confirmation did not land -- which needs an epoch
publication that fails. That is a case worth having (it is the row §3.6's first
table names), and it is not the same case as "the commit was committed".

**Evidence.** Release build clean; full suite **143/144**, the one failure the
pre-existing `integration.m3.recovery` (§1.18). `m5.service_orchestration`
19/19 → **20/20**.
