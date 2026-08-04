# M3 Persistence Acceptance

## Baseline evidence

| Item | Value |
|---|---|
| Commit | `d06bcf3` (planning baseline) |
| Parent (M2 GO) | `a37ce86` |
| Toolchain | MSVC 14.44.35207, Windows x64 |
| Worktree | `.worktrees/m3-foundation-batch` |

## M3-V0 baseline gate (Task 00)

```powershell
cmake --preset msvc
cmake --build --preset release
ctest --preset release --output-on-failure
```

Expected and observed: **57/57 tests pass**, 0 failures.

## Gate status

| Gate | Status | Notes |
|---|---|---|
| M3-V0 | PASS | Fresh Release build + full CTest |

## Task 00/01 independent acceptance — 2026-08-04

| Item | Value |
|---|---|
| Review branch | `feature/m3-m2.1-logic` |
| Task 00 candidate | `0a399f2` (cherry-picked from `3019e0a`) |
| Task 01 candidate | `7395e42` (cherry-picked from `9e52180`) |
| Build dependency repair | `02c8eca` |
| Task 01 acceptance repair | `d5e3f5b` |
| Toolchain | MSVC 19.44 / tools 14.44.35207, Windows x64 |

### Initial Task 01 rejection evidence

- `ctest --preset release -N -L m2.1-v0` reported **0 tests**.
- `unit.m2.report` terminated with Windows access violation `0xC0000005`.
- Static review found an arbitrary `2 * 64 KiB` COW minimum, acceptance of
  negative stall durations, no active snapshot state gauge, and JSON fields
  added to serialization without matching fail-closed validation/schema tests.
- A stale Release ABI object exposed that localized MSVC `/showIncludes`
  output was not reaching Ninja (`#deps 0`). The compiler launcher now emits
  UTF-8 diagnostics; the same storage object records 16 header dependencies.

### Accepted contract and verification

- `kMinimumSnapshotCowBytes` is the exact 64 KiB payload plus the fixed
  256-byte coordinator record/index/accounting slab; `minimum + 1` is valid,
  proving that no block-alignment rule was introduced.
- Stall validation is exactly `(0 ms, 5000 ms]`.
- Token/status signatures, three state strings, five stable storage errors,
  option errors, active state and all COW lifetime counters have direct tests.
- Every M2 test fixture supplies the two new options explicitly.
- M2 JSON serialization, native validation, schema and missing-member tests
  fail closed for every new statistic and reject incoherent id/state pairs.

```powershell
ctest --preset release -L m2.1-v0 --output-on-failure
# 3/3 passed

ctest --preset release -L m2-v1 --output-on-failure
# 16/16 passed

ctest --preset release -R 'unit\.m2\.(report|json_verifier)' --output-on-failure
# 2/2 passed

ctest --preset release --output-on-failure
# 59/59 passed
```

### Drift audit

- M1 protocol and AIM driver/proxy contracts: unchanged.
- Existing M2 block, dirty epoch, backing generation and acknowledgment
  semantics: unchanged; no second epoch was introduced.
- Durability publication order and M4 scheduling/service boundary: unchanged.
- Task 00 boundary remains independent of driver/proxy; Task 01 adds only the
  frozen M2.1 management contract and evidence required before Task 02.

Task 00 and Task 01 verdict: **DONE**. Task 02 may start on the isolated logic
branch; this is not an M3 GO verdict.

## Task 02 logic-owner evidence — 2026-08-04

| Item | Value |
|---|---|
| Implementation commit | `ca1b744` |
| Logic-owner branch | `feature/m3-m2.1-logic` |
| Independent review | PASS; no remaining Task 02 correctness finding |

### Red gate

The deterministic tests were added before production admission state. The
focused target failed to compile specifically because
`DynamicBlockStoreTestAccess::{active_mutations,mutation_gate_closed,
mutation_waiters}` and the corresponding implementation did not exist. This
established the expected Task 02 red state without weakening an existing test.

### Implemented cut

- One request-independent mutation token covers the complete public
  `write()`, `zero()` or `unmap()` operation after validation and before the
  first block change. Reads never enter mutation admission.
- `begin_consistent_snapshot()` exclusively closes admission, drains all
  already-admitted mutations, captures backing generation and dirty epoch,
  activates one nonzero token, then reopens admission.
- Cancellation, deadline and close all leave through one RAII rollback that
  reopens admission and wakes queued mutations. A second begin receives the
  stable `SnapshotBusy` error.
- Lifecycle/admission state is never held across a stripe operation. The short
  cut acquires only lifecycle/admission then state; no stripe or future
  coordinator mutex is held. Native C++20 stop-token waits remove the
  notify-only callback lost-wakeup window.

### Deterministic coverage

- A 64 KiB write starting at byte 512 crosses two blocks. Two independent
  backing gates prove that begin cannot return in the gap between its first
  and second segment.
- A post-cut write is observed in the admission waiter count, while an
  unrelated read completes with the gate closed.
- While the first begin is still draining, a second begin is forced through
  the same overlap window and returns `SnapshotBusy`; this is distinct from
  merely observing an already-published active token.
- Test access probes acquire lifecycle/admission, state and every stripe while
  begin is draining, proving that the cut wait owns none of them. Task 02 has
  no coordinator mutex; the coordinator-to-stripe reverse-order gate belongs
  to Task 03 where that mutex is introduced.
- Separate tests cover cancelled begin, timed-out begin, cancelled mutation,
  timed-out zero, close waking both cut and mutation waiters, and two
  concurrent begin calls.
- Gate, active-mutation and waiter gauges are checked back at zero on rollback
  and close paths; cancelled/timed-out begin leaves no active snapshot id.

```powershell
# Focused executable, repeated after the final implementation
1..20 | % { .\out\build\msvc\tests\Release\myrd_m2_consistent_snapshot_tests.exe }
# 20/20 passed

ctest --preset release -R '^unit\.m2\.(concurrency|write|ranges)$' --output-on-failure
# 3/3 passed

ctest --preset release -L m2-v3 --output-on-failure
# 6/6 passed

ctest --preset release --output-on-failure
# 59/59 passed
```

The independent concurrency re-review closed its three original findings:
multi-range ZERO/UNMAP token coverage, overlapping begin exclusivity, and
Task-02 lock-availability instrumentation. Residual coordinator-to-stripe and
full token-lifecycle risks are explicitly owned by Tasks 03 and 06 because no
snapshot coordinator exists in Task 02.

Task 02 verdict: **DONE**. This is not an M2.1 or M3 GO verdict; Tasks 03–09
remain required.

## Claude foundation batch independent review — 2026-08-04

| Item | Value |
|---|---|
| Reviewed branch | `feature/m3-foundation-batch` |
| Reviewed range | `d06bcf3..95d4e44` |
| Reviewed head | `95d4e44372245166f25849c780b787fc38e115dc` |
| Independent reviewers | Four disjoint reviews covering Tasks 10–12, 13–16, 17–20 and 21–24 |
| Build/test observation | Release full CTest 65/65 passed on the reviewed branch |
| Acceptance verdict | **NO-GO for every claimed Task 10–24 completion** |

The green test count only proves that the submitted thin tests pass. It does
not prove the contracts in the SPEC, PLAN and TASKLIST. The batch is retained
as reference material only and must not be merged as accepted task work.

### Blocking findings

- Task 10 buffers all SHA-256 input instead of using a streaming BCrypt hash;
  the byte codec has wraparound-prone range checks and lacks the required
  typed fixed-span helpers and independent golden-byte tests.
- Task 11 combines overlapped I/O with `FILE_FLAG_NO_BUFFERING` but treats
  `ERROR_IO_PENDING` as failure, omits checked end ranges and supplies only a
  shallow fake fault model. No native exact-I/O gate exercises this code.
- Task 12 has no handle-based canonical containment. Reparse points, hard
  links, ADS/device names and replacement races can escape the bundle root;
  writer ownership, reader sharing and file identity are not proven.
- Task 13 remains an incomplete type shell. Limits are not wired into readers
  or writers, errors/outcomes lack the required structured context and
  generation overflow still reaches a wrapping writer path.
- Task 14 accepts semantically invalid manifest slots and has no independent
  4096-byte fixture or exhaustive boundary/corruption matrix.
- Task 16 has incorrect DATA and COMMIT declared lengths, incomplete CRC
  coverage, no record decoder, and a chain digest that ignores its descriptor.
- Tasks 17–20 contain no B+tree reader/builder algorithms. The journal writer
  performs no I/O yet reports success, while the reader does not load DATA and
  collapses Absent/ZERO/DISCARD into one result.
- Task 21 checks only a few VHDX magic values and explicitly defers the
  metadata/BAT profile. Tasks 22 and 23 are success-returning stubs, and Task
  24 has no Windows native attach test at all (`m3-v5` discovers zero tests).

### Integration decision

At the time of this batch audit, Tasks 10–24 remained `PENDING`. Individual
commits could be cherry-picked into isolated task branches only as untrusted
scaffolding. Task 10 was subsequently repaired and independently accepted as
recorded below; Tasks 11–24 remain `PENDING`. Every task must independently
demonstrate a deterministic red test, complete implementation, focused and
cumulative gates, specification review and code-quality review before its
status can become `DONE`.

## Task 10 integrity primitives evidence — 2026-08-04

| Item | Value |
|---|---|
| Implementation branch | `feature/m3-task10` |
| Foundation commit | `1c858ac` |
| Independent specification review | GO; P0=0, P1=0 |
| Independent code-quality review | GO; P0=0, P1=0 |

### Red gates and accepted behavior

- The original SHA lifecycle test exposed an error-contract mismatch.
- A `SIZE_MAX` codec skip was accepted and advanced the cursor before checked
  offset arithmetic was implemented.
- Compile-time red tests established that typed codec errors, UUID/UTF-8 and
  alignment helpers, CRC finalization state, and the SHA error enum were absent.
- A deterministic injected hash failure established the required sticky-error
  behavior before the state machine was implemented.
- CRC32C uses the Castagnoli polynomial and published vectors. SHA-256 uses
  bounded BCrypt streaming; a partial native failure is sticky before any
  caller-visible error copy, and finalization allocates before `BCryptFinishHash`.
- The byte codec emits explicit fixed-width little-endian bytes and performs
  checked offset, length and alignment arithmetic. Truncation, overflow,
  reserved-byte, UTF-8, UUID and failure-nonmutation cases have direct tests.
- CRC and SHA lifecycle errors occupy separate stable ranges (`0x21xx` and
  `0x22xx`).

### Verification

```powershell
ctest --preset release -R '^(unit\.common|unit\.m3\.checksum_codec)$' --output-on-failure
# 2/2 passed

ctest --preset release -L m3-v1 --output-on-failure
# 1/1 passed

1..50 | % { .\out\build\msvc\tests\Release\myrd_m3_checksum_codec_tests.exe }
# 50/50 passed

1..20 | % { .\out\build\msvc\tests\Debug\myrd_m3_checksum_codec_tests.exe }
# 20/20 passed

ctest --preset release --output-on-failure
# 60/60 passed

git diff --check
# passed
```

### Recorded non-blocking debt

- Move the internal SHA state/test adapter out of the public object layout when
  an ABI boundary is frozen.
- Add a test-side direct BCrypt oracle in addition to published SHA vectors.
- Avoid the non-fail-open initialization-path `Error` copy during future cleanup.

Task 10 verdict: **DONE**. This is not an M3 GO verdict; Tasks 11–44 and the
real AIM/NTFS, crash and soak acceptance gates remain required.

## Task 03 bounded snapshot coordinator evidence — 2026-08-04

| Item | Value |
|---|---|
| Implementation commits | `cf3671f`, `7858f2e` |
| Logic-owner branch | `feature/m3-m2.1-logic` |
| Independent specification review | GO; P0=0, P1=0 |
| Independent code-quality review | GO; P0=0, P1=0 |

### Accepted behavior

- Snapshot IDs are process-wide unique nonzero values with checked CAS
  wraparound. Reservations use weak shared-state ownership, so destroying the
  coordinator cannot leave a use-after-free path.
- The ordered COW map charges exactly one 256-byte record/index/accounting slab
  per metadata pre-image. Rebind allocation, rollback and invariant checks keep
  record and byte accounting synchronized.
- Pending duplicate preservation returns `SnapshotBusy`; rollback permits a
  later retry. Invalid record kinds fail the invariant rather than entering an
  unaccounted state.
- The coordinator records cutoff membership, one pre-image per block and a
  monotonic scan frontier without a consumed-block set. Task 03 intentionally
  stores ZERO/DISCARD metadata only; DATA payload belongs to Task 04.
- Deterministic lock probes and a 10,000-record test cover lock ordering and
  ordered-index scaling.

### Verification

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset release
# passed

1..100 | % { .\out\build\msvc\tests\Release\myrd_m2_consistent_snapshot_tests.exe }
# 100/100 passed

ctest --preset release -L m2.1-v1 --output-on-failure
# 1/1 passed

ctest --preset release -L m2.1-v3 --output-on-failure
# 1/1 passed

ctest --preset release --output-on-failure
# 59/59 passed

git diff --check
# passed
```

### Recorded follow-up obligations

- Add deterministic allocation-failure injection for the real `bad_alloc`
  catch path when Task 07 expands pressure testing.
- Add a near-`UINT64_MAX` snapshot-ID seam if ID generation receives further
  changes.
- Task 06 must freeze and test whether `Aborted` plus its reason remains
  observable through public status before coordinator state is cleared.

Task 03 verdict: **DONE**. This is not an M2.1 or M3 GO verdict; Tasks 04–09
remain required.

## Task 04 DATA pre-image evidence — 2026-08-04

| Item | Value |
|---|---|
| Implementation commits | `3e8b858`, `0ba4be5`, `e24cf89` |
| Branch | `feature/m3-task04` |
| Independent specification review | GO after one NO-GO repair cycle; P0=0, P1=0 |
| Independent code-quality review | GO after test-cleanup repair; P0=0, P1=0 |

### Accepted behavior

- The first post-cut mutation of a member DATA block preserves exactly one
  complete 64 KiB payload plus its 256-byte coordinator record before bytes,
  version or dirty identity are published.
- Resident partial/full writes, duplicate writes, a single cross-block write,
  consumed members, clean backing-at-cut load/mutate, hard-limit rejection and
  the short final logical block have deterministic reference-byte/hash tests.
- A late cross-block budget failure preserves the existing M2 non-atomic write
  contract: the completed first-block prefix has its correct pre-image, while
  the rejected second block remains unchanged and the snapshot stays complete.
- Zero dirty epochs and future backing generations fail closed as fatal
  invariants. Budget exhaustion and cancellation during DATA reservation erase
  the record and payload, restore accounting, and leave the foreground block
  unchanged.
- Concurrency tests unconditionally release barriers and join threads before
  assertions, so a failed assertion cannot terminate or hang the test process.

### Verification

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset release
# passed

1..100 | % { .\out\build\msvc\tests\Release\myrd_m2_consistent_snapshot_tests.exe }
# 100/100 passed

ctest --preset release -L m2.1-v2 --output-on-failure
# 1/1 passed

ctest --preset release -R '^unit\.m2\.(backing_store|read|limits|write|backing_generation|concurrency)$' --output-on-failure
# 6/6 passed

ctest --preset release --output-on-failure
# 60/60 passed

git diff --check
# passed
```

### Recorded follow-up obligations

- Task 05 owns the ZERO/DISCARD transition matrix.
- Task 06 owns public bounded batch consumption and token completion.
- Task 07 owns deterministic real `bad_alloc`, COW-full waiting, stripe release
  and retry, stall timeout and automatic snapshot abort.
- Copying 64 KiB while holding the coordinator mutex serializes first writes to
  distinct member blocks; benchmark and optimize only after correctness gates.

Task 04 verdict: **DONE**. This is not an M2.1 or M3 GO verdict; Tasks 05–09
remain required.

## Task 11 exact file I/O evidence — 2026-08-04

| Item | Value |
|---|---|
| Implementation branch | `feature/m3-task11` |
| Implementation commits | `700649d`, `9cb520c`, `b3cf454`, `237dd0d` |
| Independent specification review | GO; P0=0, P1=0 |
| Independent code-quality review | GO; P0=0, P1=0 |

### Red gates and accepted behavior

- Native and fake tests exposed missing `ERROR_IO_PENDING` handling, unchecked
  end offsets, short/zero-progress ambiguity, fail-open over-reporting, and an
  allocation-path handle leak before the exact-I/O wrapper was accepted.
- Windows random access uses OVERLAPPED I/O and drains pending operations after
  cancellation or deadline. Exact reads and writes retry partial transfers,
  reject backend over-reporting and zero progress, and keep stable error
  operation/domain/native-code context.
- Transfer operations share a lifetime lock; flush, truncate and close are
  exclusive. Close waits for in-flight transfers and remains idempotent.
- The sparse fault backend supports phase, occurrence and half-open byte-range
  injection for disk-full, torn-write, flush, corruption and close failures.
  State and occurrence selection are linearized by one mutex, while observers
  run after that mutex is released.
- Named-pipe tests exercise real pending read/write cancellation, deadlines,
  late-stop completion and close ordering. Failure cleanup has a four-second
  process boundary, so a `std::async` future cannot hang the test executable in
  its destructor.
- Fault injection and the allocation-failure override are linked only into the
  Task 11 test executable; the production persistence library has no driver or
  proxy dependency.

### Verification

```powershell
.\out\build\msvc\tests\Release\myrd_m3_file_io_tests.exe
# 25/25 passed

1..20 | % { .\out\build\msvc\tests\Release\myrd_m3_file_io_tests.exe }
# 20/20 processes passed

.\out\build\msvc\tests\Debug\myrd_m3_file_io_tests.exe
# 25/25 passed; an additional 10/10 process repeat passed

ctest --preset release -L m3-v1 --output-on-failure
# 2/2 passed

ctest --preset release --output-on-failure
# 61/61 passed

git diff --check
# passed
```

### Recorded non-blocking debt

- `FlushFileBuffers` is synchronous on Windows; Task 11 can only honor a
  cancellation/deadline observed before entering that native call.
- Bundle containment, file identity and single-writer ownership deliberately
  remain Task 12 scope.

Task 11 verdict: **DONE**. This is not an M3 GO verdict; Tasks 12–44 and the
real AIM/NTFS, native VHDX, crash and soak acceptance gates remain required.

## Task 05 ZERO/DISCARD transition candidate evidence — 2026-08-04

| Item | Value |
|---|---|
| Baseline | `4829c743403cb08ba2e17101d7f78c53c39315cc` |
| Branch | `feature/m3-task05` |
| Candidate status | `READY_FOR_REVIEW`; deliberately not `DONE` |
| Independent review | Not completed: Claude read-only review failed with API `ConnectionRefused` |

### Candidate behavior

- The deterministic truth table covers DATA→ZERO, DATA→DISCARD,
  ZERO→DATA, DISCARD→DATA, ZERO→DISCARD and DISCARD→ZERO.
- DATA pre-images retain the Task 04 exact 64 KiB path. ZERO/DISCARD
  pre-images retain exact logical kind, version, base generation, dirty epoch
  and acknowledgement identity without allocating payload; each metadata record
  is charged exactly 256 bytes.
- The first eligible pre-cut mutation preserves exactly once. Post-cut clean
  blocks, consumed members and already-preserved members do not allocate a new
  record. Invalid kind, epoch and generation identities fail closed.
- Partial ZERO/UNMAP over DATA is normalized to DATA and preserves the complete
  old DATA block. Partial ZERO/UNMAP over an already-zero dirty block remains a
  no-op while still validating active-token identity.
- Reservation and publication precede mutation publication. Budget exhaustion,
  cancellation and backing-read failure leave no ghost record and do not change
  the rejected block. A cross-range late failure retains the M2 non-atomic
  prefix contract, with a pre-image for every committed prefix block.
- The current narrow M2 install/exact-ack/ZeroClean read path proves that neither
  a fake successor nor an acknowledged predecessor is resurrected. Public
  consumption and acknowledgement of a Task 05 consistent pre-image is
  intentionally deferred to Task 06; no Task 06 API was fabricated.

### Red evidence

- The first Release focused run failed at the transition-matrix
  `snapshot_preimage` assertion (exit 1), exposing missing DATA→ZERO/DISCARD COW.
- A separate partial ZeroDirty invalid-identity no-op test failed at
  `!rejected.ok()` (exit 1), exposing an unchecked early-return path.
- The first minimal fix caused two legacy M2 write/range regressions by checking
  test-seeded epoch zero without an active token. Restricting that validation to
  the active COW path restored the frozen M2 behavior.

### Candidate verification

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset release
# passed

1..100 | % { .\out\build\msvc\tests\Release\myrd_m2_consistent_snapshot_tests.exe }
# 100/100 passed

ctest --preset release -L m2.1-v2 --output-on-failure
# 1/1 passed

ctest --preset release -R '^unit\.m2\.(ranges|snapshot|backing_generation|write)$' --output-on-failure
# 4/4 passed

ctest --preset release -L m2-v3 --output-on-failure
# 6/6 passed

ctest --preset release --output-on-failure
# 60/60 passed

git diff --check
# passed
```

Focused executable SHA-256:
`67F805B9436B59B01D80DBB8140FDC1B473106C8EC218AA13E7C59BEAE896B09`.

### Review and follow-up obligations

- Independent specification and code-quality review remain mandatory before
  changing Task 05 to `DONE` or beginning Task 06.
- Task 06 owns public bounded-batch consumption, exact acknowledgement and the
  literal public no-resurrection proof for these consistent pre-images.
- Task 07 owns deterministic real `bad_alloc`, COW-full waiting, stall and
  automatic-abort tests.
- A voluntary Debug probe exposed a pre-existing Task 03 issue: the MSVC Debug
  `std::map` container-proxy allocation is not included in the coordinator's
  initial budget accounting. It is outside Task 05 and must be fixed and
  accepted separately before the final M3 Debug GO.

Task 05 verdict: **READY_FOR_REVIEW** only. This is not a Task 05 `DONE`, M2.1
GO or M3 GO verdict; Tasks 06–09 remain required.
