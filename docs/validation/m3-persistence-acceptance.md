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

## Task 05 ZERO/DISCARD transition acceptance — 2026-08-04

| Item | Value |
|---|---|
| Baseline | `4829c743403cb08ba2e17101d7f78c53c39315cc` |
| Branch | `feature/m3-task05` |
| Accepted commits | `14daea4`, `6440409`, `170cd51` |
| Status | `DONE` |
| Independent specification review | GO after two NO-GO repair cycles; P0=0, P1=0 |
| Independent code-quality review | GO; P0=0, P1=0, P2=0 |

### Accepted behavior

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

### Acceptance verification

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset release
# passed

cmake --build --preset debug
# passed

.\out\build\msvc\tests\Debug\myrd_m2_consistent_snapshot_tests.exe
# 55/55 passed

.\out\build\msvc\tests\Release\myrd_m2_consistent_snapshot_tests.exe
# 55/55 passed

1..20 | % { .\out\build\msvc\tests\Debug\myrd_m2_consistent_snapshot_tests.exe }
# 20/20 passed

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

ctest --preset debug --output-on-failure
# 59/59 passed

git diff --check
# passed
```

Focused executable SHA-256: Debug
`C9E4A68F855E0B36B1C97715E896BBF3E189E17AD3785DB3AE0B15555C33D059`;
Release `C4BB14591DB001AF06EEAEBBA5BD9FB06952B6697734FD4DA370728625E05298`.

### Review result and follow-up obligations

- The specification reviewer returned GO after repairs for Debug allocation
  accounting, real snapshot no-resurrection ordering, the final short block and
  deterministic reader/mutator overlap. A fresh code-quality reviewer returned
  GO with no P0, P1 or P2 findings.
- Task 06 owns public bounded-batch consumption, exact acknowledgement and the
  literal public no-resurrection proof for these consistent pre-images.
- Task 07 owns deterministic real `bad_alloc`, COW-full waiting, stall and
  automatic-abort tests.

Task 05 verdict: **DONE**. This is not an M2.1 GO or M3 GO verdict; Tasks 06–09
remain required.

## Task 06 bounded snapshot batch evidence — 2026-08-05

| Item | Value |
|---|---|
| Branch / accepted HEAD | `feature/m3-task06` / `a5b919e` |
| Independent specification review | GO after two repair cycles; P0=0, P1=0 |
| Independent code-quality review | QUALITY GO after deterministic cancellation repair; P0=0, P1=0, P2=0 |

### Accepted behavior

- Consistent batches scan in deterministic ascending order, enforce caller
  cursor/frontier equality and bounded record/byte limits, and transfer either
  the current cut member or its exact pre-image without duplicates or omission.
- A batch advances the coordinator and erases covered COW state only after the
  returned object owns its payload. Cancellation before the first record makes
  zero progress; cancellation after safe progress returns that partial batch.
- Sparse traversal is proportional to populated leaves plus emitted records.
  A 1 TiB geometry with one dirty block does not scan logical block count or
  allocate absent leaves.
- `finish`, `abort`, install and exact acknowledgement preserve the frozen
  token lifecycle, generation and dirty-epoch rules, including post-cut
  successor no-resurrection behavior.
- Cancellation and close wake every transient snapshot wait. The final seam
  pauses after the stop predicate check but before the actual condition wait,
  so the wakeup contract is tested at the only lost-notification window.

### Mutation and verification evidence

- Removing the snapshot-wait stop callback makes the deterministic
  cancellation test fail instead of depending on scheduler timing.
- Final focused Debug/Release runs passed 71/71 tests. Debug repeat passed
  20/20; Release repeat passed 100/100; the legacy `unit.m2.snapshot` seam
  regression passed 1/1.
- Fresh full Debug passed 60/60 and fresh full Release passed 61/61.
- The independent review Release focused executable SHA-256 was
  `C6435D7FD1C6DA66130B9ADB1CD699FC9525E3691BFEBF9D88FC5AA87C60CFB7`.

Task 06 verdict: **DONE**. This is not an M2.1 GO or M3 GO verdict; Tasks 07–09
remain required.

## Task 12 path, identity and writer-ownership evidence — 2026-08-05

| Item | Value |
|---|---|
| Implementation branch | `feature/m3-task12` |
| Final implementation commits | `a550638`, `9da6e3e`, `cedd4d8` |
| Independent specification review | GO; P0=0, P1=0, P2=0 |
| Independent code-quality review | GO; P0=0, P1=0, P2=0 |

### Accepted behavior

- Bundle paths are single validated UTF-8 components anchored to opened root
  and file handles. Identity, direct containment, reparse state and hard-link
  count are revalidated at every mutation boundary.
- The single writer is enforced by both an in-process manifest-identity
  registry and a cross-process byte lock. State snapshots retain strong
  ownership across concurrent `close()`.
- Pending native rename releases the writer mutex while waiting, has a finite
  deadline and bounded cancellation drain, and classifies the final native I/O
  status. A completion that beats cancellation is reported as that completion.
- An operation that cannot be proven drained remains in one of eight bounded
  process-lifetime quarantine slots and retains the complete writer authority;
  a successor writer cannot overlap potentially kernel-owned buffers.
- Production and test quarantine chains are separate. Test cleanup is compiled
  only with `BUILD_TESTING=ON`, cannot release production-provenance operations,
  and returns every operation registration independently.
- When a test fake is known to have stopped using retained buffers, releasing
  the final detached registration also releases the byte lock, process registry
  claim and authority handles. A replacement writer can then acquire normally.

### Verification

```powershell
ctest --preset debug -R '^unit\.m3\.(file_io|persistence_path)$' --output-on-failure
# 2/2 passed; Task 12 process repeat 20/20

ctest --preset release -R '^unit\.m3\.(file_io|persistence_path)$' --output-on-failure
# 2/2 passed; Task 12 process repeat 20/20

ctest --preset release --output-on-failure
# 62/62 passed

cmake -S . -B out/build/msvc-no-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build out/build/msvc-no-tests --target myrd_persistence
# passed; cleanup seam symbol absent from myrd_persistence.lib

git diff --check
# passed; final myrd-task12-* count: 0
```

### Recorded boundary

M3 detects accidental namespace changes; it does not claim atomic protection
against a malicious same-account actor racing hard-link creation. Expanding that
threat model requires immutable copy-on-write publication for every mutation or
kernel namespace arbitration, not another finite recheck.

Task 12 verdict: **DONE**. This is not an M3 GO verdict; Tasks 13–44 and the
real AIM/NTFS, native VHDX, crash and soak acceptance gates remain required.

## Task 09 M2.1 prerequisite verdict — 2026-08-05

### Verdict and acceptance identity

Final verdict: **M2.1 PASS**. Tasks 28 and 30 are authorized to consume the
M2.1 interface. This is explicitly **not M3 GO**: Tasks 13–44, native
VHDX/journal persistence, real AIM+NTFS operation, crash recovery and soak
acceptance remain open.

| Item | Accepted value |
|---|---|
| Branch | `feature/m3-task07-review-fixes` |
| Tested source commit | `c55e8330505f4d8387f69673a5903cee3e5019bd` |
| Fresh build/evidence root | `.worktrees/m3-task09-validation/out/validation/task09-c55e833-r2/` |
| OS | Windows 11 Pro, version `10.0.26200`, build `26200` |
| CPU / memory | AMD Ryzen 5 5600, 6 cores / 12 logical processors; `68359413760` bytes RAM |
| Toolchain | MSVC `19.44.35228` (`cl.exe` 14.44.35207), CMake `4.2.2`, Ninja `1.13.2` |
| Lost-wakeup defect review | Two independent read-only code reviews: final GO; P0=0, P1=0, P2=0 |
| T09.6 acceptance review | Final specification PASS and evidence GO; P0=0, P1=0, P2=0 |

The validation directory did not exist before configure. Configure and both
full builds ran from the Visual Studio x64 developer environment with these
effective commands; every command exited `0`:

```powershell
cmake -S . -B out/validation/task09-c55e833-r2 -G "Ninja Multi-Config" `
  -DCMAKE_CONFIGURATION_TYPES="Debug;Release" `
  -DCMAKE_CXX_COMPILER_LAUNCHER=tools/msvc-utf8-launcher.cmd `
  -DBUILD_TESTING=ON
cmake --build out/validation/task09-c55e833-r2 --config Debug
cmake --build out/validation/task09-c55e833-r2 --config Release
```

Raw configure, build and test output is retained under
`.worktrees/m3-task09-validation/out/validation/task09-c55e833-r2/evidence/`.
`00-preflight.log` binds the pre-test timestamp, detached worktree path, exact
HEAD, zero porcelain entries, previously absent build directory, machine and
toolchain. Each later log records its exact command and final exit code; no
earlier build log is used for this verdict.

### T09.1–T09.2 clean-build and dynamic gates

| Gate | Result | Exit |
|---|---:|---:|
| Fresh Debug full CTest | 62/62 | 0 |
| Fresh Release full CTest | 63/63 | 0 |
| Release `m3-v3`, run 1 | 4/4 | 0 |
| Release `m3-v3`, run 2 | 4/4 | 0 |
| Release `m2-v1` | 16/16 | 0 |
| Release `m2-v2` | 6/6 | 0 |
| Release `m2-v3` | 6/6 | 0 |
| Release `m2-v4` | 35/35 | 0 |
| Release `m2-v5` | 6/6 | 0 |
| Release `unit.m2.concurrency --repeat until-fail:100` | 100/100 | 0 |
| Direct Release consistent-snapshot suite | 94/94 | 0 |
| Direct Release backing-generation suite | 11/11 | 0 |
| Direct Release concurrency suite | 22/22 | 0 |

The official `m3-v3` aggregate contains
`unit.m3.m2_consistent_snapshot`,
`unit.m3.m2_consistent_snapshot_model`,
`unit.m2.backing_generation` and `unit.m2.concurrency`. The internal labels
remain separately discoverable: `m2.1-v0` 3 tests and `m2.1-v1..v5` one test
each. There were no required skips.

Task 08's frozen 100 seeds were supplemented with a fresh, externally
generated corpus. For seed index `i`, the generator took the last eight bytes
of `SHA256("c55e833|T09|i")` as a big-endian hexadecimal seed. The model ran
100 seeds × 1000 operations = 100,000 operations and passed 13/13 tests:

| Corpus item | Value |
|---|---|
| First seed | `8035c78ecf339580` |
| Last seed | `c68c264b8e312873` |
| Comma-separated corpus SHA-256 | `F31F44BD52794F13C1866778F4B9FEB60B6A1830DA93FAEF79BEC7FD0C9B4780` |
| External summary | `seeds=100 operations=100000 slowest_seed=0xf337ebafb2b22414 slowest_us=40075` |

The key accepted binary hashes are:

| Configuration | Binary | SHA-256 |
|---|---|---|
| Debug | `myrd_m2_consistent_snapshot_tests.exe` | `8416F97725768E801B3F3658F9E72F96E7600DE194B11EC4D18F7BB7079ED175` |
| Debug | `myrd_m2_consistent_snapshot_model_tests.exe` | `6607CC22B1C1699E6F838E85463DB9DF983538248FD6E41338EA5BF478DC3ACC` |
| Debug | `myrd_m2_backing_generation_tests.exe` | `B89A04A5FB2E40334D09171FA0D5F7BE654BA6ABC387D3C1E8D46D18E3861771` |
| Debug | `myrd_m2_concurrency_tests.exe` | `2AE238D4D89F5D36DFE4BB0B182AE62A819833F15A1071C0EE1B54614E7E4C7C` |
| Release | `myrd_m2_consistent_snapshot_tests.exe` | `8E347CA00416BCAB0904ED7BD08D0455699086EBA7CE362E2ED0D1F3AB05F8EF` |
| Release | `myrd_m2_consistent_snapshot_model_tests.exe` | `D6620045DF4A5B2C8F632B3022ACB6434FC6C91955C07649BEDF568183DE3B87` |
| Release | `myrd_m2_backing_generation_tests.exe` | `DE02FD34E089746AD734E76CB20CD6D14781B421848E3FC74DC89D30CCDD71C1` |
| Release | `myrd_m2_concurrency_tests.exe` | `2D95C2CBEF218572FAD62789280D31342F79F06E3C24CE2CD32BB1161505DE97` |

### T09.3 M2.1 diff and dependency inventory

The repository history interleaves M2.1 with Tasks 10–12, so the raw contiguous
range `a37ce86..c55e833` is intentionally not called an M2.1-only diff. The
reproducible M2.1 implementation commit set is selected from that range by
including subjects matching `(feat|fix|test)(storage):` plus the single
`test(m3): register M2.1 aggregate gate` commit, while excluding docs-only,
common-codec and persistence commits:

```powershell
$selected = git log --reverse --format='%H%x09%s' a37ce86..c55e833 |
  Where-Object {
    $_ -match "\t(feat|fix|test)\(storage\):" -or
    $_ -match "\ttest\(m3\): register M2\.1"
  }
$hashes = $selected | ForEach-Object { ($_ -split "`t")[0] }
$files = foreach ($hash in $hashes) {
  git diff-tree --no-commit-id --name-only -r $hash
}
$files | Sort-Object -Unique
```

This yields 55 commits and the following exact 38-file touched-file inventory:

```text
docs/validation/m3-persistence-acceptance.md
M3_MICROTASKS.md
M3_TASKLIST.md
src/CMakeLists.txt
src/m2/m2_report.cpp
src/storage/block_directory.cpp
src/storage/block_directory.h
src/storage/consistent_snapshot.h
src/storage/dynamic_block_store.h
src/storage/dynamic_block_store_backing.cpp
src/storage/dynamic_block_store_core.cpp
src/storage/dynamic_block_store_internal.h
src/storage/dynamic_block_store_ranges.cpp
src/storage/dynamic_block_store_read.cpp
src/storage/dynamic_block_store_snapshot.cpp
src/storage/dynamic_block_store_write.cpp
src/storage/dynamic_block_types.h
src/storage/snapshot_coordinator.cpp
src/storage/snapshot_coordinator.h
tests/CMakeLists.txt
tests/support/consistent_snapshot_model.cpp
tests/support/consistent_snapshot_model.h
tests/unit/block_directory_tests.cpp
tests/unit/block_math_tests.cpp
tests/unit/dynamic_block_store_backing_generation_tests.cpp
tests/unit/dynamic_block_store_concurrency_tests.cpp
tests/unit/dynamic_block_store_consistent_snapshot_model_tests.cpp
tests/unit/dynamic_block_store_consistent_snapshot_tests.cpp
tests/unit/dynamic_block_store_core_tests.cpp
tests/unit/dynamic_block_store_eviction_tests.cpp
tests/unit/dynamic_block_store_limit_tests.cpp
tests/unit/dynamic_block_store_model_tests.cpp
tests/unit/dynamic_block_store_range_tests.cpp
tests/unit/dynamic_block_store_read_tests.cpp
tests/unit/dynamic_block_store_snapshot_tests.cpp
tests/unit/dynamic_block_store_stress_tests.cpp
tests/unit/dynamic_block_store_write_tests.cpp
tests/unit/m2_report_tests.cpp
```

The exact selected commit list and file list are retained as machine evidence:
`12-m2.1-commit-set.txt` SHA-256
`D365032E4DC031DEF1561887B69F51AD80B3B2E593322105E62218875A39AA20`
and `13-m2.1-file-inventory.txt` SHA-256
`C15F1121861C078B687D7C77775C5668A7F2347D770ACF184F1591B78D76F6EB`.

Public contract changes are limited to:

- `ConsistentSnapshotToken`, `ConsistentSnapshotState` and
  `ConsistentSnapshotStatus`;
- fixed accounting constants `kSnapshotCowRecordBudgetBytes` and
  `kMinimumSnapshotCowBytes`;
- begin/copy-batch/finish/abort/status methods;
- bounded `snapshot_cow_max_bytes` and
  `snapshot_writer_stall_limit` options;
- stable errors `SnapshotBusy`, `SnapshotTokenInvalid`, `SnapshotAborted`,
  `SnapshotNotFullyScanned` and `SnapshotResourceExhausted`;
- option-validation errors `SnapshotCowBytesTooSmall`,
  `SnapshotCowStallNotPositive` and `SnapshotCowStallTooLarge`;
- coherent/lifetime stats for active snapshot, COW current/peak/records,
  blocked writers, stalls, auto-aborts, batches and records.

The added `src/storage` lines contain zero `CreateFile`, `ReadFile`,
`WriteFile`, `DeviceIoControl`, AIM, ImDisk or phdskmnt references.
`myrd_storage` still links only `myrd_common`; it has no persistence-format,
Win32 file, driver or proxy dependency. M2.1 adds no worker thread, native
handle or new VM API. COW ownership is ordinary bounded user memory accounted
by the coordinator. Full `m2-v4`, close/resource tests and the structural
storage-independent build all passed.

### T09.4 signed invariant audit

| Frozen requirement | Signed conclusion and direct evidence |
|---|---|
| Snapshot linearization | **PASS.** Begin closes mutation admission, drains the complete public multi-block mutation, captures backing generation and dirty epoch, publishes one token, then reopens admission. Reads continue. Deterministic cut/cancel/deadline/close tests pass; the reference model matched the frozen 100 seeds plus 100 new seeds (200,000 modeled operations total). |
| Bounded COW and stall | **PASS.** DATA is charged 64 KiB plus the fixed 256-byte coordinator record; ZERO/DISCARD still consume the record charge. One-record pressure, FIFO grant, no-barging, release-stripe-before-wait, cancellation/deadline and 250 ms auto-abort tests pass. Peak/current/record/blocked/stall/abort counters are checked, and finish/abort/close return current resources to zero without erasing lifetime peak. |
| No resurrection | **PASS.** COW is installed before mutation publication, returned payload owns its bytes, consumed blocks cannot create a later pre-image, and exact acknowledgement rechecks version/kind/generation/cutoff epoch. `ack_evict_reload_never_exposes_predecessor_bytes`, DATA resurrection, ZERO resurrection and zero/discard exact-ack tests all pass. |
| Install-before-ack | **PASS.** Before `FullyScanned`, both `install_backing_view()` and `mark_persisted()` return `SnapshotBusy`; finish returns `SnapshotNotFullyScanned`. After the full ascending scan, the accepted order is successor install → exact conditional acknowledgements → finish. A post-cut rewrite fails acknowledgement with `version_changed` and remains dirty. |
| Close and resource ownership | **PASS.** Close rejects new admission, signals lifecycle/coordinator/stripe/test barriers, drains active requests/mutations/reads, then finalizes token/COW and releases the store-owned backing. Capacity wait, preparing owner, status request, transient load, pin, pressure wait and record-staging close paths pass. Final gauges are zero, backing owners expire, VM release occurs exactly once, and the concurrency suite passed 100 consecutive Release runs. |

No SPEC §7 or PLAN §1.1–1.3 requirement was waived. No M1/AIM protocol,
persistence wire format, durability commit point or M4 boundary was changed.

### Defect evidence and independent review

The first Task09 concurrency repetition on pre-verdict commit `b3d8b19`
correctly produced a RED result: `unit.m2.concurrency` failed at
`dynamic_block_store_concurrency_tests.cpp:521` because a loading waiter did
not observe cancellation within the two-second bound. Root cause was the
notify-only stop callback window between predicate evaluation and the actual
condition-variable sleep.

Commit `c55e833` replaces those vulnerable stripe/coordinator waits with C++20
native stop-token predicate waits and includes close in every stripe predicate.
It deliberately does not allocate a per-wait combined `stop_source`; close
continues to synchronize notification by taking the predicate mutex. The same
class of wait in read, write, zero/unmap, ordinary snapshot, consistent
snapshot, Preparing and capacity paths is covered.

Independent code review found and rejected two intermediate designs before
the lost-wakeup defect was accepted: a per-wait `stop_source` could add an OOM
exception inside an active block transition, and an early revision omitted
close from the native predicate. Both were removed. Two final read-only code
reviews report P0=0, P1=0, P2=0 and GO; these code reviews are not substituted
for the separate T09.6 acceptance review.
After the final code change, Debug 62/62, Release 63/63, concurrency 100/100
and consistent-snapshot 20/20 all passed; the fresh `c55e833` validation above
then passed independently.

T09.6 round 1 produced two evidence-only NO-GO verdicts, preserved verbatim in
`14-task09-spec-review-round1.txt` and
`15-task09-evidence-review-round1.txt`. The repair:

- restores Task09 to PENDING and removes premature authorization;
- defines a reproducible 55-commit M2.1 implementation selection and its exact
  38-file touched-file inventory;
- adds the two budget constants and three option-validation errors to the
  public-change inventory;
- replaces the first evidence set with a new detached, clean-worktree run whose
  `00-preflight.log` binds HEAD/status/build-dir/machine/toolchain before
  configure;
- uses only the permitted verdict token `M2.1 PASS`.

T09.6 specification review round 2 is preserved in
`16-task09-spec-review-round2.txt`. Its sole P1 was a first-run
`slowest_us` value copied into the R2 table; the table now records the R2 raw
value `40075`. No behavioral result, seed, operation count or hash changed.

Final T09.6 records are preserved in `17-task09-spec-review-final.txt` and
`18-task09-evidence-review-final.txt`. Both reviewers independently report
P0=0, P1=0, P2=0 and authorize the status-only finalization.

Task 09 verdict: **M2.1 PASS**. This closes only the M2.1 prerequisite,
authorizes Tasks 28/30 to consume the interface, and does not claim persistence
or end-to-end M3 completion.

## Task 16 — Journal wire-format acceptance

Task 16 is accepted at implementation commit
`59fe20d91c824d12ce983c7bbb773725f2e6952f`. The accepted scope is the pure
journal wire contract only: immutable header, DATA/ZERO/DISCARD records,
leaf/internal INDEX_NODE records, CHECKPOINT_COMMIT, canonical streaming
SHA-256 and bounded stateful sequence validation. It performs no file I/O,
manifest publication, B+tree lookup or path-copy construction.

The independent frozen layout and sparse fixtures bind all offsets, declared
physical lengths, reserved-zero regions and CRC fields. The canonical sample
checkpoint digest is
`632E32DE34E3EECEEB69017A1AC6DFFCA81A20F6EA9F0F6092C0B77B37168701`.
Generation zero is the sole empty `0 -> 0` commit and retains the header
genesis digest; later generations use checked `G -> G+1` and the ordinary
checkpoint chain formula.

The strict decoders reject malformed type/version/length/alignment/range,
unknown mandatory flags, invalid identities or generations, nonzero padding,
descriptor escapes, count/root inconsistencies and CRC/digest mismatches.
DATA decode validates the full 64 KiB payload and logical-tail zeros without a
second payload allocation. Sequence validation additionally enforces exact
contiguous offsets, zero-based sequence numbers, one operation/generation,
unique block updates, block-before-index order, one terminal commit, exact
counts/root and caller committed end.

Verification after the final code change passed Debug and Release focused
tests 1/1. The full Task 16 gate passed Debug `m3-v1` 6/6, Release `m3-v1`
6/6 and full Release CTest 66/66. All-record truncation, deterministic byte
fuzz, semantic mutations with recomputed CRC and `git diff --check` passed.

Evidence SHA-256:

- `journal_format.cpp`:
  `AE9C1AAC5D71A7A86A9FB35ED401E1463E3D13DEA33D2FAC5D7E632A6249AABE`;
- frozen layout:
  `824BE2E2854FAA96F3A580C366B94758063F3CAEC2B3932FC3FB61A4A8C9C3A1`;
- digest fixture:
  `34A94F5AAAE342701527A538D6DD1793B57FF0E03CA1451259C1984DD47FC010`;
- Release journal-format test executable:
  `4F828DFEA84C72C5A03130CA2558D30B3C14F78E683D4F8D14548A2449E2545F`.

Task 16 verdict: **DONE**. This authorizes Task 17 to consume the immutable
node codec and does not claim Task 17–20 or end-to-end persistence completion.

## Task 17 — Immutable journal-index reader acceptance

Task 17 is accepted at implementation commit
`dcf5bea646696fe77d3e941eaa384d36127607e` (base implementation `ecf256c`,
review repair `dcf5bea`). The reader performs iterative
immutable B+tree lookup only. It does not build nodes, read DATA payloads,
append journal bytes or publish a manifest.

An empty root returns Absent without I/O. Nonempty lookup reads exact 4096-byte
nodes inside the caller committed length, traverses at most eight levels and
tracks visited offsets. Every root/child transition revalidates CRC,
generation, exact child level and min/max range. Strictly decoded leaf entries
produce distinct DATA, ZERO, DISCARD and Absent outcomes; corruption in any
node or descriptor that was needed for the lookup cannot become Absent.

The LRU node cache is bounded by both caller-supplied bytes and entry count.
Its key binds Windows file-object identity, journal UUID, epoch, root
generation, committed length, root descriptor and node offset. Tests prove
that different file objects, journal identities and epochs do not reuse an
entry and that eviction/clear restore the current gauges without exceeding
the configured peak.

The fixed seed `0x17A5B4C3D2E1908F` drove 1,000,000 lookups against a
`std::map` oracle. Results matched exactly; the fully warmed Release reader
performed three physical node reads and retained three nodes. Debug and
Release focused tests passed 1/1, both `m3-v1` gates passed 7/7, and full
Release CTest passed 67/67.

Evidence SHA-256:

- `journal_index.cpp`:
  `0C02133E24D041A4A142FDB402727AFDDE94676CDFCA91A229FC0C28988B4D74`;
- journal-index tests:
  `E6F4BEFDD1D56A8581613FE91DE9F7ECBD9B26BE2CF632A1179C75BF2F3A6EA0`;
- Release journal-index test executable:
  `1FA71EBCCA307EC27896872F938D391402EE7B89AAB84D84F63CB7B832C7D6E9`.

An independent malformed-input/cache-identity review issued final GO with
P0/P1/P2 = 0/0/3. It specifically rechecked that production lookup calls
the visited/depth guards, and that a real lookup waiting on the cache mutex
returns `timed_out` after its deadline. The three remaining P2 items are
additional exhaustive stress/mutation coverage and do not block Task17.

Task 17 verdict: **DONE**. This authorizes Task 18 to consume the reader and
node codec; it does not claim path-copy construction or journal durability.

## Task 18 — Copy-on-write journal-index builder acceptance

Task 18 is accepted at implementation commit
`9c6eab67d8f7059a9c206ac81d3650f582cf2b31`. The builder produces an opaque,
read-only in-memory plan before assigning any physical offsets. The plan binds
the complete immutable base identity (file object, journal UUID, epoch, root
generation, committed length and root descriptor), and planning performs no
file writes.

Each checkpoint must be exactly `G -> G+1`. Updates are sorted by block key for
deterministic output and duplicate block updates are rejected. New journal
record ranges must begin at or after the old committed end and may not overlap.
The persistent B+tree planner copies only affected root paths, caps node fanout
at 82 and depth at eight levels, and retains physical descriptors for untouched
subtrees.

Offset assignment rechecks the caller's expected immutable base view. It
validates that every planned node is reachable exactly once in strict
child-before-parent order, rejects orphan/shared/forward references, bounds all
reused records and nodes by the old committed length, and bounds all new record
ranges before the first index-node offset. Alignment, sequence, generation,
node-count, image-byte and integer-overflow checks run before encoding. The
builder still performs no writes; journal append durability remains Task 19.

Focused tests cover inserts and replacements, DATA-to-ZERO and ZERO-to-DATA,
leaf/internal/high-root splits, deterministic bytes under reordered input,
duplicate rejection, cancellation and resource limits, stale-tail/base-identity
rejection, and physical reuse of an untouched subtree below a level-2 root.
The fixed seed `0x18C0FFEE51A7B11D` drives 100,000 update events containing
random interior inserts, hot-key replacements and DATA/ZERO/DISCARD kinds.
Release compares every consecutive published root with the complete map oracle;
Debug verifies every batch plus a fixed historical sample and exhaustively
checks the final root.

Final verification passed Debug `m3-v1` 8/8 and complete Release CTest 68/68,
including Release `m3-v1` 8/8. An independent path-copy/split review returned
GO with P0/P1/P2 = 0/0/0. `git diff --check` passed.

Evidence SHA-256:

- `journal_index.cpp`:
  `2962055831A30E25B020A472E19C1DA946375C4AA3E163718AD0EC268AE6BC88`;
- builder tests:
  `1EB731605A05D7C8BC5331625C5A26ECF6E8A78C843868187DFC059164C8E892`;
- Release builder-test executable:
  `5F9D97E212FC2D746430F81CA7577A7F787A4DB05DCD443A4B34C729FF844B82`.

Task 18 verdict: **DONE**. This authorizes Task 19 to append planned records,
nodes and a commit, then flush and independently read them back. It does not
claim journal durability or manifest publication.

## Task 19 — Durable journal-checkpoint preparation acceptance

Task 19 is accepted at implementation commit
`5e0fa901b54c2692316b0e205382cc0fd61d91de`. It appends block records and
Task 18 COW index nodes only at or after the caller's manifest committed
length, writes one complete checkpoint commit, flushes exactly once, and then
independently reads back every new record before returning an unpublished
`PreparedCheckpoint`.

The writer freezes each block descriptor from its encoded bytes, streams the
canonical checkpoint digest, verifies the complete sequence and committed end,
and resolves every updated key through a separate `JournalIndexReader` rooted
at the just-written tree. Assignment node counts and final offsets are checked
against the planned layout. The returned value contains generation, root and
commit descriptors, committed length, chain digest, per-kind counts and a
bounded ack-stream descriptor; it does not publish the manifest or acknowledge
the M2 snapshot.

Production mutation requires a `ValidatedPersistenceFile`. The capability is
checked before append and again before Prepared, while every underlying write
and flush remains bound to the live bundle WriterLock and rechecks file-object
identity, hard-link count, reparse state and direct-child containment. The
generic in-memory fault file is accepted only when test seams are compiled.

Tests bind named phases and actual backend I/O in one ordered trace. They cover
short progress in every record, zero progress, disk-full and torn writes,
split-tree node failures, flush failure, record/node/commit and crosscheck-only
corruption, source-payload mutation after encoding, per-phase and per-ordinal
cancellation/deadline, finite budgets, final partial DATA blocks, retries, and
byte-for-byte preservation of both generation-zero and generation-one
committed prefixes after failure.

Final focused Debug and Release writer tests passed. A no-test-seam
`BUILD_TESTING=OFF` Release build of `myrd_persistence` passed. Release
`m3-v1` passed 9/9 and complete Release CTest passed 69/69. An independent
durability/capability review returned GO with P0/P1 = 0/0; three of its four
nonblocking test-strength suggestions were subsequently added, while a
standalone runtime production-capability probe remains optional because the
real production branch is compiled and the lower-level capability behavior is
already exercised directly.

Evidence SHA-256:

- `journal_writer.cpp`:
  `BC3301DF2F1C3FA639DC4B4A7172C6A82271B5D29DFE830D7F084EDA1A354AD8`;
- journal-writer tests:
  `FAED697968A720CE42EC5C9EEBF9BB09573CD16B4A6B7A924DF262BE2A07A718`;
- Release journal-writer test executable:
  `395FC46FDE90642FC19B0659DDFA09477EBCAA1EDF2BCCDBA8C2B44F7BA3485D`.

Task 19 verdict: **DONE**. This authorizes Task 20 to resolve immutable
committed journal blocks. It does not authorize manifest publication,
BackingView installation, snapshot acknowledgement or end-to-end checkpoint
commit.

## Task 20 — Immutable committed journal-reader acceptance

Task 20 is accepted at implementation commit
`de107626c410ff361c7ae9fdb34e501bd85d63bd`. A `JournalReader` owns one
immutable `JournalIndexView`, one image geometry and one index cache bounded by
both bytes and entries. It cannot change roots after creation and keeps the
underlying file alive until the reader's final reference is released.

The public result is deliberately three-way: `nullopt` is journal Absent,
`BlockLoadKind::Data` means a fully verified 64 KiB payload was copied, and
`BlockLoadKind::AllZero` represents either ZERO or DISCARD without modifying
the destination. Only Absent may be considered for VHDX fallback by future
Task 25 code.

Every hit is bounded by the immutable manifest committed length before I/O.
The exact record bytes are decoded and their fixed header, physical length,
record CRC, payload CRC, type, block index and record generation are compared
with the index descriptor. DATA reaches the caller buffer only after all
checks and final context validation pass. A valid descriptor whose record is
missing, truncated or inconsistent returns `BackingDataCorrupt`; index
structure damage returns `JournalCorrupt`. Ordinary native I/O failures remain
`IoError` (or `AccessDenied`), while EOF inside a committed referenced range is
classified as corruption.

Tests cover DATA/ZERO/DISCARD/Absent, image and final-partial-block boundaries,
record/header/payload/type/index/generation mutations, ZERO/DISCARD corruption,
truncated records and roots, short reads for every record kind, cached-root
post-read cancellation/deadline windows, immutable old-root tail isolation,
eight concurrent readers and exact file-handle lifetime. Destination buffers
remain unchanged on Absent, AllZero and every error.

Final Debug `m3-v1` passed 10/10 and complete Release CTest passed 70/70. A
`BUILD_TESTING=OFF` Release build of the persistence library passed. The first
independent review found two P1 issues in post-I/O cancellation and overly
broad corruption mapping; both received targeted red tests and fixes. Final
independent review returned GO with P0/P1/P2 = 0/0/0.

Evidence SHA-256:

- `journal_reader.cpp`:
  `872E5B03702052119E98BD857D6E5CDFD3982134233AD3638497825212285655`;
- journal-reader tests:
  `A41F9FE8D79E29426D0145B80AB13C78E4A578A803A307EBD4711008AC006038`;
- Release journal-reader test executable:
  `8A9600F1BD71839BFC18A976E4D626570F0E5E953FE6D31BECB3049CD1FF6ECA`.

Task 20 verdict: **DONE**. This authorizes Task 25 to compose journal hits with
future Task 22 VHDX results. It does not implement VHDX parsing, BAT lookup,
fallback, recovery selection or manifest publication.

## Task 21 — Strict VHDX structural-parser acceptance

Task 21 is accepted at implementation commit `2965425`. The parser reads only
the fixed identifier, both 4 KiB headers, both 64 KiB region tables, the
selected metadata table and its five required items. It never maps or scans
the BAT or payload and never allocates memory proportional to virtual disk
capacity.

The accepted profile is dynamic, non-differencing VHDX with a clean log,
exactly 2 MiB payload blocks, logical sectors of 512 or 4096 bytes and 4096-byte
physical sectors. Header validity and sequence selection are independent of
region-copy validity. Because region tables have no sequence field, one valid
copy is accepted, two identical valid copies are accepted, and two valid but
different copies fail closed. Unknown regions/metadata, parent locators,
fixed/differencing flags, active logs and unsupported geometry return
`UnsupportedVhdx`; malformed ranges, identities and checksums return
`InvalidFormat`.

`VhdxLayout` is a parser-only value: it has no public default constructor and
contains no file handle or writable pointer. It exposes only validated
geometry, VHDX identities, BAT/metadata/log ranges, file length, payload-block
count, chunk ratio and the dynamic interleaved BAT-entry count.

An independent fixture builder converts canonical GUID fields to Windows wire
bytes and uses its own bitwise CRC32C implementation. Tests cover every exact
read stage, both-copy fallback/conflict, the malformed field matrix, checked
overlap/overflow, post-I/O cancellation/deadline/error propagation, exact
64 TiB success with a sufficient sparse BAT, 128 deterministic truncation
samples and 128 redundant-copy single-bit checksum mutations.

Final Debug `m3-v1` passed 11/11 and complete Release CTest passed 71/71. A
`BUILD_TESTING=OFF` Release build passed. Independent malformed-input review
initially returned NO-GO twice, exposing writable-layout construction,
insufficient stage-specific truncation/mutation proof and a deadline test that
could pass early. After targeted fixes, final review returned GO with
P0/P1/P2 = 0/0/0. `git diff --check` passed.

Evidence SHA-256:

- `vhdx_format.cpp`:
  `A64BBB6812EB60831E6F615E170B5155F92BE3CBAB13856B054141EA2D1E6405`;
- VHDX format tests:
  `EE6342A7DF875C5E943D13221AF598B33DCD06972AAD84CE5B8E73861CC8C8E2`;
- Release VHDX format test executable:
  `90245C5D6944205BAD768CD991D498B8D895ECEC6980A9A0EB55564E84FC87ED`.

Task 21 verdict: **DONE**. This authorizes Task 22 to interpret BAT entries and
read payload slices lazily. It does not validate individual BAT states or
payload ranges, write VHDX files, or claim Windows native compatibility.

## Task 22 — Lazy VHDX BAT-reader acceptance

Task 22 is accepted at implementation commit `6e04578`. The reader resolves
the frozen dynamic, non-differencing VHDX profile into 64 KiB M2 blocks without
mapping or loading the complete image. States NotPresent, Undefined, Zero and
Unmapped return `AllZero` without touching the destination or payload; only a
FullyPresent entry issues the exact bounded payload read. The final logical
block is zero-padded through a scratch buffer and reaches the destination only
after exact I/O and final operation-context validation succeed.

The product constructor accepts only exclusive ownership of a
`ValidatedPersistenceFile` and derives Windows file-object identity from that
handle-anchored capability. The generic file/identity constructor is a
`BUILD_TESTING`-only seam and its symbol is absent from the production library.
Creation reparses and byte-equivalently compares the immutable `VhdxLayout`;
every operation also verifies the unchanged file length.

BAT lookup reads 4 KiB pages on demand. The LRU cache is bounded by explicit
byte and entry budgets and keyed by Windows file-object identity, VHDX
file/data-write GUIDs and page index. No cache mutex spans file I/O. Concurrent
misses may perform duplicate bounded reads, but cache insertion is race-safe;
`clear_cache()` advances a generation so an older in-flight miss cannot refill
the cleared cache. LRU insertion has rollback on allocation failure and public
paths translate allocation failure into `Result` errors.

Normal reads validate reserved/state bits, relevant interleaved sector-bitmap
entries, file/structure bounds and every touched physical payload range.
Touched aliases or BAT mutation latch corruption for all later operations.
This is deliberately incremental: proving overlap against an unseen BAT entry
would require violating lazy-open by scanning the complete BAT. Callers that
need a complete proof use `enumerate_allocated_extents()`, which scans every BAT
entry, validates all bitmap/range relationships and returns sorted unique
virtual extents without reading payload bytes. Both incremental integrity state
and enumeration output have explicit caller budgets. A complete enumeration
therefore requires `max_observed_payloads` to cover all payload blocks; later
public option sizing must document this for large sparse images.

Tests cover all supported/rejected payload states, reserved bits, structural
overlap/out-of-file/overflow, touched physical aliases and corruption latching,
2 MiB-to-64 KiB slicing, final partial blocks, 512/4096 sector bitmap
boundaries, exact independent I/O offset/length traces, bounded eviction,
clear-during-stalled-I/O concurrency, post-open length change, layout mismatch,
AccessDenied/short/zero-progress/native errors, post-BAT/post-payload
cancellation and deadline, destination atomicity, extent budgets and a
deterministic 256-payload sparse model.

The first independent review returned NO-GO with four P1 findings: caller-
forgeable file identity and mutable shared ownership, incomplete normal-read
BAT integrity checks, a cache mutex held across file I/O and exception-unsafe
LRU insertion. Targeted red tests and the ownership/cache/integrity redesign
closed all four. Final independent review returned GO with P0/P1/P2 = 0/0/5.
The five non-blocking P2 items are retained for later integration hardening:
make the random BAT oracle formula-independent; exercise product-capability
wait behavior; publish default `max_observed_payloads` sizing; extend payload,
enumeration, allocator and dual-miss fault matrices; and record that the shared
fault-file trace/forced-length seam was a justified Task 22 scope exception.

Final focused Debug and Release reader suites passed 21/21. Complete Debug
CTest passed 71/71 and complete Release CTest passed 72/72. A
`BUILD_TESTING=OFF` Release build of `myrd_persistence` passed, and `dumpbin`
confirmed that `create_for_testing` is absent. `git diff --check` passed.

Evidence SHA-256:

- `vhdx_reader.cpp`:
  `E140D5D33FC25985B311681FF72456859A811A9FAAF5F453601388C319D5F0B7`;
- VHDX reader tests:
  `5C8A5362655569BC12F7275863BBE0A8DE457EE9648CA70838595BCC8913999D`;
- Release VHDX reader test executable:
  `81286CBDCA34EAD5A172960367B15CD15047867EF41A2D5FFE5777B231DDC41D`.

## Task 23 — Sparse dynamic VHDX writer acceptance

Task 23 is accepted at implementation commit `10973da` with the Windows
compatibility correction at `c1cb72c`. The writer first builds
a deterministic, checked layout plan and only then mutates a same-directory
temporary file. It supports logical sector sizes 512 and 4096, handles BAT
chunk boundaries and final partial blocks, skips all-zero payload blocks, and
zeroes every reserved byte. Checked arithmetic rejects unrepresentable file
layouts before file creation.

Publication is ordered as ordinary-file creation, exact planned writes, flush,
close of all old handles, reopen through the product `VhdxReader`, structural
and sampled-payload verification, and handle-relative no-overwrite rename with
a post-commit volume flush. The container deliberately never receives the NTFS
sparse attribute: Windows Virtdisk rejects such VHDX files. Dynamic sparseness
is represented by absent BAT entries and by never appending absent payloads,
so a large sparse logical disk remains a short ordinary file. Once the
namespace commit may have happened, failures are classified as
`CommitIndeterminate` instead of
being returned as ordinary pre-commit errors. Temporary and final names are
validated before creation, compared case-insensitively, and reject `~` so an
NTFS 8.3 short-name alias cannot turn publication into self-replacement.

Tests cover create, extend, metadata, BAT, payload, flush, close, reopen and
rename failures. They assert that the container is not an NTFS sparse file but
remains shorter than its sparse logical geometry, verify actual I/O ordering,
and compare a fixed-seed random
sparse logical image and the reopened reader by SHA-256. No failure returns a
partially written final filename.

Three independent review rounds were performed. The first two rounds exposed
and then closed write-through, post-commit classification, cancellation,
failure-matrix, diagnostics, aliasing and oracle weaknesses. The final review
returned **GO** with P0/P1/P2 = 0/0/0, including explicit confirmation of the
8.3 alias defense. Task 24 subsequently exposed one cross-system defect that
self-read tests could not detect: `OpenVirtualDisk` returned
`ERROR_VIRTUAL_DISK_LIMITATION` for the NTFS sparse attribute. The correction
removed that attribute and added a native Windows regression gate.

Final verification evidence:

- complete Debug CTest: 73/73 passed;
- Release writer and native compatibility gates passed;
- focused path suite: 41/41 passed;
- focused writer suite: 25/25 passed;
- `BUILD_TESTING=OFF` production persistence library built successfully;
- `dumpbin /symbols` reported `NO_TEST_SEAM_SYMBOLS`;
- `git diff --check` passed.

Evidence SHA-256:

- `vhdx_writer.cpp`:
  `3EA72D7EBF4D890ECEE03634FDBCDAF90E132F4C8AEDAE74B2BC0A0A39DAEE17`;
- VHDX writer tests:
  `C9A51B71C23CB7890FE6308C126B6CF57BD4E0EF994ADDED1F7479090CC63D10`;
- Release VHDX writer test executable:
  `53A9E0C616A5E1354847C16B726D6500A1C646D314EB303245F971CAD9B86851`.

Task 23 verdict: **DONE**. This authorizes Task 24 to ask Windows native VHDX
APIs to open and attach product-written fixtures. Native compatibility is now
proven separately by the Task 24 gate below.

Task 22 verdict: **DONE**. This authorizes Task 23 to produce sparse dynamic
VHDX bases that are reopened and verified by this reader. It does not write or
publish VHDX files, compose journal fallback, select recovery candidates or
claim Windows native compatibility; those remain Tasks 23–25.

## Task 24 — Windows native VHDX compatibility acceptance

Task 24 is accepted at harness commit `31a12c7`, hardened by `0ba7028`, with
the product compatibility correction at `c1cb72c`. The first elevated native
run deliberately failed at `OpenVirtualDisk` with `0xC03A001A`
(`ERROR_VIRTUAL_DISK_LIMITATION`). The Windows SDK defines that result for
virtual-disk containers carrying unsupported sparse/compressed/encrypted file
attributes. Removing the NTFS sparse attribute made the same product fixtures
pass without weakening their BAT-level dynamic sparseness.

The gate dynamically probes `virtdisk.dll`, administrator elevation and the
required APIs. Missing capabilities return a machine-readable `BLOCKED` report
and exit 77; CTest deliberately has no skip-return override, so a mandatory
M3-V5 run cannot turn BLOCKED into a successful process. Before, immediately
around and after every attachment/raw-I/O boundary, the gate requires zero AIM
devices. It never supplies a fixture path to AIM.

Four product-generated cases cover empty and sparse data, the 512-byte-sector
BAT chunk boundary, and 4096-byte logical sectors. Each case is opened through
`OpenVirtualDisk`, queried, attached read-only without a drive letter, sampled
through its raw disk handle, and detached in finally-style cleanup. The raw
handle is bound to a canonical `\\.\PhysicalDriveN` path by an attach-time
PhysicalDrive baseline, `IOCTL_STORAGE_GET_DEVICE_NUMBER`, a second
`GetVirtualDiskPhysicalPath` query and an exact geometry check. Detach must
remove that new PhysicalDrive number.

The reverse case uses Windows `CreateVirtualDisk` with the frozen 2 MiB block,
512-byte logical-sector and 4096-byte physical-sector geometry. It attaches the
new image without a drive letter, proves the raw-handle identity and 8 MiB
length before writing, flushes and detaches, then closes every native handle.
The product `parse_vhdx_layout` and `VhdxReader` reopen that independently
created file and reproduce the complete deterministic 64 KiB payload.

Every PASS report contains exactly the five required unique cases, their fixed
sample lengths and fixed SHA-256 values, successful detach outcomes and a
successful sandbox removal. Detach or sandbox cleanup failure produces FAIL,
the native error code and a bounded recovery command.

The first independent review returned NO-GO with P0/P1/P2 = 0/4/3. It found
missing raw-disk identity closure, insufficient AIM boundary observations,
ignored sandbox cleanup, CTest skip-success semantics, weak JSON/case/hash
validation and an unaligned geometry buffer. All findings received targeted
hardening. The second review returned **GO** with P0/P1/P2 = 0/0/0.

Final verification evidence:

- the initial native red gate reported `OpenVirtualDisk:3225026586`;
- focused Debug and Release M3-V5 gates passed all five native cases;
- each case completed detach with Win32 code 0 and sandbox cleanup succeeded;
- complete Debug CTest passed 73/73;
- focused Release writer and native compatibility tests passed 2/2;
- the `BUILD_TESTING=OFF` production persistence library built successfully;
- `dumpbin /symbols` reported `NO_TEST_SEAM_SYMBOLS`;
- `git diff --check` passed.

Evidence SHA-256:

- native compatibility harness:
  `E19730E2114915A268C2CCB808E57C0E12C9108B95EDA23E900652EDF7544C8B`;
- PowerShell gate:
  `540C44266998BEE079561DEA98900E9C59AA4979C212DC73214D1B28B4FF0124`;
- Release native compatibility executable:
  `352E7C992BD859D3716AD845F845FD7C6126984315E87B8077B23A31D691EE54`.

Task 24 verdict: **DONE**. Windows accepts and attaches product-written dynamic
VHDX bases, and the product parser/reader accepts a Windows-created compatible
VHDX. This authorizes Task 25; it does not yet compose journal-first fallback,
recover manifests or create complete generation-zero image bundles.

## Task 25 — Journal-first immutable composite backing acceptance

Task 25 is accepted at implementation commit `6594907`. A separate oracle
froze the complete 4x3 journal DATA/ZERO/DISCARD/Absent by VHDX
data/zero/absent truth table before production resolution was implemented.
Journal DATA is authoritative, ZERO and DISCARD both return logical zero and
suppress older base data, and only an explicit journal Absent reaches VHDX.
Journal or base corruption, I/O failure, cancellation and deadline errors keep
their original `Error` provenance and are never reinterpreted as Absent.

Each composite owns one immutable JournalReader/VhdxReader pair and a complete
identity containing image geometry and UUID, generations, Windows and format
file identities, VHDX virtual-disk ID and length, journal epoch/committed
length/commit/root, root generation and feature flags. Old and new generation
views remain isolated under concurrent reads. Diagnostic counters are atomic
and monotonic; their multi-field sample is explicitly approximate rather than
a transactional snapshot.

The production-adapter lazy gate creates real journal and VHDX files through
the product writers and opens the product readers. VHDX open is frozen at 11
fixed structural metadata reads totaling 204,848 bytes; JournalReader open
does not read an index, and CompositeBackingStore creation performs no I/O.
One first-touch journal miss reads exactly one 4 KiB index root, one 4 KiB BAT
page and one 64 KiB payload range. BAT and payload counters are both one and
peak BAT cache use is one 4 KiB page. Real ZERO and DISCARD journal records
also prove that the production adapter never touches older VHDX payload data.

The first independent implementation review returned NO-GO with P0/P1/P2 =
0/2/2. It correctly found that the initial lazy gate used only fakes; that was
replaced by the real-reader range gate above. Its DISCARD-statistics finding
became inapplicable after inaccurate hit-kind statistics were removed: the
accepted `IBackingStore` contract exposes Data/AllZero, while the journal wire
format still preserves ZERO versus DISCARD. Identity-factory ownership and
non-transactional statistics were documented. The final independent review
returned **GO** with P0/P1/P2 = 0/0/0.

Final verification evidence:

- initial focused RED failed because `composite_backing_store.h` was absent;
- Debug and Release `unit.m3.composite_backing` passed all 11 cases;
- final complete Debug CTest passed 74/74, including native M3-V5;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built successfully;
- production `dumpbin /symbols` found zero `create_for_testing` symbols;
- `git diff --check` passed.

Evidence SHA-256:

- `composite_backing_store.h`:
  `BD9AB0F2DD155A6E25D15D74F83DA8D3EE79266C995C9233B0EE2AB2FB4D6B55`;
- `composite_backing_store.cpp`:
  `70E0A2F41C7F2552F1C6E055E9DD5CAE091324DF878C228B5FD62A3998591EBC`;
- composite tests:
  `A3E934B4D1C17EBE8B173DE230C74EBECBEA67D65630188533B070674C809A60`;
- Release composite test executable:
  `D5EF538655861DF7A7F050421C8ECC9E0BB61A8880CDE85C0BCD2011F9E49869`;
- production persistence library:
  `AA2C23A160D1C2131A8158EE9DA2C16698B74A6C0FE803DAB250A28F03DA2EE2`.

Task 25 verdict: **DONE**. This authorizes Task 26 to validate manifest
candidates and construct immutable recovery views. It does not yet select a
recoverable generation, create a generation-zero image bundle or install the
backing into M2.

## Task 15 — Highest fully valid dual-slot candidate acceptance

Task 15 is accepted at implementation commit `ea02e0a`. The selector consumes
explicit evidence for a fixed sequence of 19 checks covering slot decoding and
support, durable semantics, referenced paths, VHDX presence/open/identity/
length/profile/layout, and journal presence/open/identity/header/committed
length/commit/root/digest. A candidate is complete only when every check is
explicitly passed; missing, corruption, unsupported data, I/O, cancellation,
deadline and conflict statuses remain distinguishable and retain the original
error provenance.

Generation is trusted only after slot decode, version support and durable
semantic validation all pass. Selection chooses the highest complete trusted
generation. A lower complete generation is reported as fallback only when a
higher *trusted* candidate is rejected. Equal trusted generations with any
unequal durable identity, geometry, base/journal generation, commit, root,
digest, expected length or feature flag fail closed as split-brain, including
the asymmetric complete-versus-rejected case. Slot index and diagnostic clocks
are deliberately excluded from durable equality; equivalent ties canonicalize
to slot A.

An independent reference oracle defines its own check/status/verdict types and
compares the complete A/B decision matrix, all 19 first-failure positions, all
18 adjacent priority pairs, seven status classes symmetrically across both
slots, 21 durable-field mutations and malformed evidence shapes. Three review
rounds were performed. The final independent review returned **GO** with
P0/P1/P2 = 0/0/1. The non-blocking P2 records that the oracle groups one
embedded slot-index mismatch into the public `FatalInternalInvariant` result
instead of exposing another public subcode; all malformed shapes are still
tested to fail closed.

Final verification evidence:

- Debug and Release `unit.m3.manifest_selector` passed all 10 cases;
- current `m3-v2` selector/composite set passed 2/2;
- complete Debug CTest passed 75/75;
- the previously observed snapshot timeout did not reproduce and that test
  completed in 1.56 seconds in the final full run;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built successfully;
- `git diff --check` passed.

Evidence SHA-256:

- `manifest_selector.h`:
  `3CD1E944B15CF4E641A4B70CF32F7BB21FAB0CA2E6DCA01137EAD8352E829790`;
- `manifest_selector.cpp`:
  `C842DE030137E7099FF0CF4132FA8CD84C3FA09820D6123D302F74AD41831DA3`;
- independent reference model header/source:
  `32A22C4204D3CD57F5D4A3BE78435BE460EEA63F3718AD74A86433B11C36B92C` /
  `5D28714C981E126CB6EDB5EB985DE3F1B16A4B77C0464663E052B15F8AB36591`;
- selector tests:
  `BA299A1DABAA98D756FEF958228CF2BFEFE1E111F350BA0AE8ACC7E54F1DBF62`;
- Release selector test executable:
  `795994D59B059A29DB8104EBC8414033FCEBA7F0831393ABBAB46E00EEFE09D5`;
- production persistence library:
  `7A1650185E7944E971CD1CA9F39154D1A9C04EB90B206FA407025C7ABE0C8DC2`.

Task 15 verdict: **DONE**. Together with completed Tasks 20, 22 and 25, this
authorizes Task 26 to validate real referenced files and construct the selected
immutable recovery `BackingView`. It does not itself open files or recover an
image session.

## Task 26 — Complete image recovery acceptance

Task 26 is accepted through implementation commits `1bddc2b`, `a75ebd7`,
`104f91e`, `f818e40`, `6d8a744` and `e619a65`. Recovery validates each
manifest candidate in a frozen order, preserves operational failures instead
of reclassifying them as corruption, selects only a fully validated generation
and constructs one immutable `BackingView` backed by owned handles. The path is
read-only: it does not truncate, repair, publish, acknowledge or delete data.

VHDX recovery uses one staged parse on the validated handle. Expected identity
and length failures stop before region/metadata traversal, while successful
recovery records bounded structural reads separately from lazy BAT and payload
reads. Manifest, root and digest failures retain the first stable per-slot
provenance, and default diagnostics do not expose absolute paths or block data.

The independent oracle does not call the production writer, manifest codec,
selector, CRC or SHA implementation. Its raw strict VHDX fixture uses non-zero
base payload together with journal DATA, ZERO and DISCARD records, preventing
an incorrect ZERO/DISCARD fallback from being masked by an all-zero base.

Independent review initially returned NO-GO for operational-failure fallback,
missing independent fixtures, VHDX stage ordering and missing lazy-I/O counts.
After correction, the final review returned **GO** with P0/P1/P2 = 0/0/1; the
sole diagnostic-provenance P2 was then corrected in `e619a65`.

Final verification evidence:

- complete Debug CTest passed 77/77;
- focused Debug and Release manifest/selector/VHDX/composite/recovery/oracle
  suites passed 7/7 in each configuration;
- Debug and Release independent recovery oracle passed;
- the Windows byte-range-lock operational-failure tests passed 20/20 repeated
  runs;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built successfully and its
  symbols contained no test-seam entry points;
- `git diff --check` passed.

Task 26 verdict: **DONE**. This authorizes Task 27 to create and publish a
generation-zero bundle and prove success only by reopening it through this
production recovery path. It does not itself authorize overwriting an existing
bundle or treating an indeterminate publication as failure.

## Task 27 — Generation-zero creation acceptance

Task 27 is accepted through commits `9803ef3`, `2fdc67c`, `5eb3caf`,
`4efee3a`, `1d8f886` and `8456ad2`.
The creator first validates the request, checks the exact empty
VHDX/genesis-journal/manifest space requirement, then atomically claims a new
manifest name with `CREATE_NEW` and a writer identity recheck. Existing names
are never opened for mutation by the create API; failed attempts leave only
unreferenced, diagnosable artifacts.

The durable order is: empty dynamic VHDX publish and readback, genesis journal
header/commit write-flush-readback, exact 8192-byte manifest initialization with
slot B zeroed, slot A generation-zero write-flush-full-readback, close all
creation handles, and finally Task 26 production recovery. Success is refused
unless recovery selects slot A generation zero and boundary M2 blocks resolve
as zero. First-publication write/flush/readback uncertainty is classified as
`CommitIndeterminate`.

The phase-fault matrix injects failures before each claim, base, journal,
manifest and recovery boundary. Existing destination, path escape, low-space,
retry/non-overwrite and concurrent-creator cases are covered. Debug and Release
both passed the focused create/recovery/path/oracle set: 4/4 CTest tests and
9/9 image-creation cases. `git diff --check` passed.

Task 27 verdict: **DONE**. This authorizes Task 28 to install the recovered
immutable `BackingView` into M2's first-touch storage boundary. It does not
authorize checkpoint, compaction, cleanup or deletion of unreferenced artifacts.

## Task 28 — Recovered first-touch loading acceptance

Task 28 is accepted at `e503dcd`. The new composition adapter creates an M2
`DynamicBlockStore` in `BackingUnresident` state from a Task 26 recovery view.
It accepts only a real `CompositeBackingStore` whose embedded immutable identity
exactly equals the recovered image identity, geometry and generation. A later
view update is forwarded only through M2's existing `install_backing_view()`
lifetime and generation boundary.

The storage algorithm itself was deliberately not rewritten: Task 09/M2 already
implements per-block single-flight loading, private-buffer mutation ordering,
old-view strong references, cancellation/deadline rollback, close-in-flight,
generation retry and generation-safe eviction. The new integration tests prove
that opening performs zero backing loads, 32 concurrent first touches produce
one composite load, repeated reads stay cached, and a legal partial-sector write
materializes from the selected recovered view before becoming dirty.

Verification evidence:

- `integration.m3.lazy_backing` passed 4/4 in Debug and Release;
- focused M2 read/backing-generation/eviction/concurrency plus M3 recovery,
  oracle, create and lazy tests passed 8/8 in both configurations;
- complete Debug CTest passed 79/79 and complete Release CTest passed 80/80;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built successfully and exposed
  no `for_testing` or test-seam symbols;
- `git diff --check` passed.

Task 28 verdict: **DONE**. This authorizes Task 29 to add image-session state and
structured outcomes. It is still only an internal M3-V7 prerequisite: real AIM,
NTFS, checkpoint/reopen and long-run memory evidence remain future gates.

## Task 29 — Image-session state and structured outcome acceptance

Task 29 is accepted through commits `551c1c9` and `a039b09`. `ImageSession`
implements the exhaustive nine-state transition contract and one move-only
publish-affecting operation gate. Immutable reads may coexist with checkpoint or
compaction, while a 32-thread start race has exactly one owner. New independent
checkpoint/compaction operations reset their own disposition and first-failure
scope; resolution and reconciliation retain the original failure chain.

The operation lease records `PrePublication`, `PublishAttempted` and
`CommitProven`. `NotCommitted`, `CommitIndeterminate`, `Committed` and
`CommittedNeedsReconciliation` are accepted only at compatible boundaries. A
lease abandoned before publication returns Ready, after a publish attempt moves
to ResolvingCommit, and after the manifest commit point moves to Reconciling.
Successful manifest `FlushFileBuffers()` is the Task 31 commit point; later
readback failure cannot downgrade that durable fact.

Close is sticky and idempotent: it first rejects new operations and reads,
requests the active operation to stop without holding the session mutex, then
drains the publish lease and immutable reads. Its wait observes cancellation and
deadline. A lifecycle anchor makes an orphaned lease fail safely if its owner was
already destroyed; actual concurrent destruction remains forbidden and must be
handled by lifecycle drain/join.

Independent review initially returned NO-GO for three P1 defects: cross-operation
outcome inheritance, publication-blind RAII abandon, and lock-held synchronous
stop callbacks. All three were fixed and the same reviewer returned GO. Final
evidence on the fixed commits:

- image-session tests passed 16/16 in Debug and Release;
- Debug focused concurrency repeated 100 times without failure;
- `m3-v2` passed 6/6 in Debug and Release;
- complete Debug CTest passed 80/80 and complete Release CTest passed 81/81;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built and exposed no
  `for_testing` or test-seam symbols;
- `git diff --check` passed.

Task 29 verdict: **DONE**. This authorizes Task 30 to implement only the
pre-manifest checkpoint durability pipeline; manifest publication, backing-view
installation and dirty acknowledgment remain Tasks 31–32.

## Task 30 — Checkpoint pre-manifest durability acceptance

Task 30 is accepted at the journal `Prepared` boundary. `CheckpointCoordinator`
validates barrier/correlation/store identity, begins one M2.1 consistent cut,
streams every token member through `JournalWriter`, flushes and fully reads back
records/index/commit, then returns a move-only prepared transaction. It does not
write a manifest slot, install a successor backing view, or acknowledge dirty
state. Missing/unknown barriers fail before snapshot begin and every failure is
a valid structured `NotCommitted` outcome with operation-local correlation.

Necessary dependency closure expanded the original file list: JournalWriter now
accepts bounded synchronous batches and rejects escaped/concurrent consumers;
clean checkpoints either retain an empty root or re-home a non-empty root;
`DirtyBlockSnapshot` carries the exact dirty epoch; and M2 uses a move-only
publication pin so ordinary install/ack cannot race an active snapshot. The pin
closes admission once finish/abort starts, drains safely during close, and may
outlive its coordinator state. The prepared transaction never exposes the token
or publication lease, reserving install/ack for Task 32.

Coverage includes DATA/ZERO/DISCARD, clean and final-partial images,
overwrite-after-cut, cancellation/deadline, all 13 pre-manifest writer phases,
malformed correlation, escaped/concurrent streaming consumers, token-bound
install/ack fencing, finish/abort/close interleavings, and C++ exception unwind.
Independent reviews repeatedly returned NO-GO for structured-outcome,
consumer-lifetime, generation, snapshot-pin and correlation defects; each was
red-tested and fixed, and both final reviewers returned GO.

Final verification on the accepted source:

- focused consistent-snapshot suite passed 97/97 and a 50-run Debug repeat;
- checkpoint passed 9/9, JournalWriter 22/22 and index builder 6/6;
- complete Debug CTest passed 81/81;
- complete Release CTest passed 82/82;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built successfully and exposed
  no `for_testing`, `test_seam` or `phase_hook` symbols;
- `git diff --check` passed apart from Git's existing LF/CRLF conversion notices.

Task 30 verdict: **DONE**. This authorizes Task 31 manifest publication work;
durable commit classification, backing installation and conditional dirty
acknowledgment remain explicitly unimplemented in Task 30.

## Task 31 — Manifest publication and indeterminate-commit acceptance

Task 31 publishes exactly one prepared 4096-byte inactive manifest slot. The
slot-write attempt is the knowledge boundary: failures before it are
`NotCommitted`, write/flush failures after it are `CommitIndeterminate`, and a
successful manifest flush is the unique durable commit point. The session
records `Committed` at that flush before any post-commit readback or full normal
recovery validation; later failures can only become
`CommittedNeedsReconciliation`.

The resolver closes the publication handle before collecting evidence through
fresh normal recovery handles. Its publication identity is a move-only
capability constructed only by `PreparedCheckpointTransaction`; production
callers cannot supply old/new generations or the attempted slot. A complete new
generation is re-flushed through a fresh writer-authorized handle, a uniquely
complete old generation is accepted only when the attempted slot is
deterministically invalid, and missing/I/O/access/cancellation evidence remains
ambiguous and fails closed.

Final verification on the accepted source:

- the frozen registry generated exactly 10 distinct manifest phase tests;
- crash-clone tests recovered fresh before-write, torn-write,
  after-write/before-flush and after-flush manifest copies;
- close failure, branch A/B/C, first-failure preservation and production
  test-seam isolation were covered;
- complete Debug CTest passed 92/92 and complete Release CTest passed 93/93;
- `BUILD_TESTING=OFF` Release `myrd_persistence` built successfully; its disk
  resolver requires `FrozenPublicationIdentity`, while callback resolver and
  test-factory symbols are absent;
- two independent final reviewers returned GO and `git diff --check` passed
  apart from Git's existing LF/CRLF conversion notices.

Task 31 verdict: **DONE**. This authorizes Task 32 to install the recovered
committed view and conditionally acknowledge the exact snapshot cut. Task 31
does not install backing state or clear dirty members itself.

## Task 32 — Committed-view reconciliation acceptance

Task 32 closes the post-commit runtime gap. A transaction with durable commit
proof reopens normal recovery state, binds the complete published manifest and
journal identity, validates the exact record/index/commit layout, installs the
immutable committed backing view, replays the durable acknowledgment stream,
finishes the consistent snapshot, and only then reports ordinary `Committed`.

Acknowledgment is exact rather than generation-wide: block version, record
kind, base generation and dirty epoch must all match. DATA, ZERO and DISCARD
members modified after the cut remain dirty as `NewerDirtyPreserved`; a clean
idempotent retry is accepted only for the same complete identity. The backing
generation is rechecked inside the final state transition, closing the
generation-change TOCTOU. The replay holds at most one bounded record and
validates CRC, sequence, operation identity and checkpoint generation.

Every failure after durable commit remains
`CommittedNeedsReconciliation`. Install, acknowledgment, snapshot finish and
success-report failures are retryable; after snapshot finish, a report-only
retry does not reinstall the backing or replay acknowledgments. Empty
checkpoints, cancellation before install, transaction abandonment, mixed
identity, truncated descriptors and repeated failures are covered. COW
pre-images transfer into a bounded synchronous journal batch before their
directory record is released; pre-commit abort leaves the newer live mutation
dirty, while a committed journal becomes the durable authority.

Final verification on the accepted source:

- focused checkpoint, reconciliation and image-session tests passed 3/3;
- complete Debug CTest passed 93/93;
- complete Release CTest passed 94/94;
- `BUILD_TESTING=OFF` product build passed with test seams excluded;
- `git diff --check` passed apart from existing LF/CRLF conversion notices;
- independent persistence and storage reviewers both returned GO with zero P0
  and zero P1 findings.

Accepted P2 follow-ups are test-depth and responsiveness work: expand the
production identity and descriptor mutation matrices, inject direct exact-ack
finish/abort/close races, and propagate reconciliation cancellation/deadline
into a single exact-ack pin wait. They do not expose a known data-correctness
failure and do not block the independent Task 33 crash model.

Task 32 verdict: **DONE**. This authorizes Task 33 to validate checkpoint
atomicity from cloned durable bytes with an independent oracle.

## Task 33 — Independent checkpoint crash-model acceptance

Task 33 freezes 32 checkpoint boundaries using the exact IDs from the Task13
fault-phase registry. The independently authored oracle fixture was committed
first as `4de15e8`; `crash-matrix.json` has SHA-256
`BCCA9EABE466103610661633456B546BF95030A066FCDE1857A9E8A814EBDB4C`
and remained byte-identical during executor implementation. Five non-kill
error phases are explicitly `not_applicable` for crash recovery rather than
being assigned an invented durable outcome.

The real-file executor creates an old complete VHDX/journal/manifest bundle,
appends a new DATA/ZERO/DISCARD checkpoint, then materializes each crash clone
from durable bytes only. Normal production recovery reopens fresh handles and
must select an outcome allowed by the independent matrix. Every one of the 128
logical blocks is compared against a fixed old or new map; generation-only
checks, mixed maps and silent post-commit fallback are rejected. Physical
coverage includes representative journal short cuts around headers, payloads,
CRC boundaries and final bytes, every 512-byte-aligned manifest torn cut, tail
garbage and reordered simulated durability.

The fast independent model uses its own byte format, hash, decoder, selector
rules and fixed maps; it does not call production journal/manifest codecs or
selectors to calculate expected outcomes. The `m3-seed-v1` protocol runs the
four fixed quick seeds plus seeds derived from the low 64 bits of
`SHA256(source-code-commit | 33 | i)`. CI runs at least 10,000 schedules; a
100,000-schedule manual profile also passed. A failing case records source
commit, card ID, seed, profile, operation count, phase, cloned-byte hashes and
a greedily minimized still-failing trace without changing the expected cut.

Mutation tests prove that commit-classification changes, manifest-before-
journal flush reordering, acknowledgment before manifest commit, corrupted
post-commit fallback and a mixed-generation map all turn the oracle red.

Final verification on the accepted source:

- `model.m3.checkpoint_crash` passed 6/6 in Debug and Release;
- the 100,000-schedule Debug manual profile passed;
- complete `m3-v2` passed 20/20 in Debug and Release;
- `git diff --check` passed apart from existing LF/CRLF conversion notices;
- Task33 changed no production file under `src/`;
- the independent final reviewer returned GO with zero P0 and zero P1.

Accepted P2 follow-ups are a frozen SHA-256 seed known-answer vector and a
stricter same-stage before/after ordering assertion in the matrix validator.
They do not permit an incorrect durable generation or mixed block map.

Task 33 verdict: **DONE**. This authorizes Task 34 live-image extent
enumeration for compaction.

## Task 34 — Sparse immutable live-extent enumeration acceptance

Task 34 enumerates the exact logical DATA image from one captured immutable
`BackingView`; it never consults mutable RAM or follows a later backing
generation. Construction requires a real `CompositeBackingStore` and compares
the complete image, checkpoint, base, journal, file and commit identity before
retaining shared ownership of that view.

The first implementation was rejected because it called `load_block()` for
every 64 KiB block in the virtual capacity. The accepted design instead adds a
strict B-tree lower-bound query to the frozen journal index and a BAT-based
next-allocated query to the frozen VHDX reader. `CompositeBackingStore` merges
those two ordered sources with one lookahead each, emits an ascending
deduplicated candidate stream, and retains a far BAT candidate while earlier
journal candidates are consumed. ZERO/DISCARD candidates remain in the union
so they can suppress older base DATA; resolved zero and all-zero DATA are then
omitted from the output.

Adjacent nonzero 64 KiB blocks are merged only while logically contiguous and
within the finite caller extent bound. Each descriptor binds image UUID,
checkpoint/backing generation, offset, length, CRC32C and SHA-256. Payload
readback uses the same frozen view and must revalidate identity, CRC32C and
SHA-256 before a successful caller result. Final partial blocks are hashed and
copied only to their exact logical length. Enumerator-owned memory remains one
block scratch buffer, two hash states and one pending descriptor, independent
of emitted extent count.

Cancellation and deadlines are checked before and after candidate lookup,
payload reads and consumer callbacks, and again before successful return. Tests
cover cancellation and deadline expiry during the final DATA read, proving no
descriptor consumer call and no false success. VHDX observed physical ranges
use ordered predecessor/successor checks rather than an O(N) scan per payload;
tests cover predecessor overlap, successor overlap and exact adjacency.

The independent randomized sparse oracle builds a complete logical byte map,
derives exact extent boundaries and max-size splits, calculates CRC32C and
SHA-256, and compares the complete descriptor vector before reconstructing the
image. Additional tests cover journal DATA/ZERO/DISCARD shadowing, VHDX holes,
all-zero DATA, corruption, generation/lifetime isolation, final partial blocks,
consumer failure/exception, invalid inputs, a 1 GiB sparse virtual image, and a
far BAT lookahead retained across 64 journal candidates.

Final verification on the accepted source:

- focused journal-index, VHDX-reader, composite-backing and live-extent tests
  passed 4/4 in Debug and Release;
- complete Debug CTest passed 95/95 and complete Release CTest passed 96/96;
- `m3-v4` passed in Debug and Release;
- `BUILD_TESTING=OFF` Release product build passed with no dependency on test
  seams;
- `git diff --check` passed apart from existing LF/CRLF conversion notices;
- the second independent reviewer returned GO with zero P0 and zero P1; its
  two P2 test-depth suggestions were added before acceptance.

Task 34 verdict: **DONE**. This authorizes Task 35 to build and publish a new
base VHDX and journal epoch from the accepted immutable extent stream.

## Task 35 compaction acceptance

The accepted compaction path builds a sparse VHDX from immutable durable view
G, validates it through the real Windows `OpenVirtualDisk` API while holding a
read-only identity pin, and then reacquires the same NTFS file identity for the
write-through no-replace rename. The final name is reopened by exact identity
with an immutable anchor retained through manifest publication and runtime
reconciliation.

The post-build cut reuses the M2.1 consistent-snapshot contract. It writes the
cut-time DATA/ZERO/DISCARD delta and a G+1 commit into the new journal even for
an empty dirty set, then reuses the existing manifest publish, recovery,
backing-install and exact conditional-ack algorithms. Post-cut writes remain
dirty.

Acceptance evidence on the reviewed source:

- every named compaction phase has a one-shot failure case; failures before
  `ManifestPublishAfter` preserve the exact old manifest bytes and generation,
  while failures at or after that boundary recover G+1 and reconcile on retry;
- an independent 8 MiB full-disk oracle covers DATA/ZERO/DISCARD transitions,
  VHDX payload boundaries and the final partial payload, then compares every
  reopened block and a streaming SHA-256 digest;
- bounded-resource and immutable-source-corruption cases fail closed without
  changing the authoritative generation;
- complete Debug CTest passed 96/96 and complete Release CTest passed 97/97;
- `BUILD_TESTING=OFF` built the production `myrd_persistence` target;
- `git diff --check` passed apart from line-ending conversion notices.

The independent final reviewer returned GO with zero P0 and zero P1. One P2
about allocation exceptions during observer composition was fixed before
acceptance and the focused compaction test passed again in Debug and Release.
The remaining P2 suggestions ask for broader combined-oracle coverage; they do
not identify a correctness defect in the accepted implementation.

Task 35 verdict: **DONE**. Task 36 may now implement identity-safe retention of
obsolete immutable generation files.

## Task 36 retention acceptance

Retention accepts only explicit validated ownership records; it never scans a
directory and deletes by pattern. The pure decision accounts for both fully
valid manifest slots, live `BackingView` weak lifetimes, and prepared or
reconciling artifacts. Invalid names, reparse points, hard links and unknown
ownership fail closed.

Execution requires the new cleanup operation on the image session's single
publish-affecting gate. This excludes checkpoint, compaction, commit resolution
and reconciliation for the complete decision/delete window. A candidate that
survives the reference decision is reopened by exact Windows file identity,
its VHDX or journal format identity is revalidated while that object remains
pinned, and deletion is issued only through the writer-authorized handle.
Sharing violations, replacements, corruption and all per-file failures keep a
harmless artifact with path-redacted diagnostics.

Acceptance evidence on the reviewed source:

- `integration.m3.retention` passed 18/18 tests for both manifest slots, live
  and in-flight references, VHDX/journal deletion, hard links, reparse/path
  rejection, sharing retry, TOCTOU replacement, corruption, dry-run and bounds;
- complete Debug CTest passed 97/97;
- complete serial Release CTest passed 98/98;
- a four-way parallel Release run had one transient failure in the pre-existing
  `unit.m2.backing_generation`; that test passed immediately in isolation and
  in the complete serial run, while all Task 36 tests passed in both runs;
- `BUILD_TESTING=OFF` built the production `myrd_persistence` target;
- `git diff --check` passed apart from line-ending conversion notices.

The review findings were closed by replacing caller-attested provenance with an
opaque `BackingViewBindingState`. Each validated reader and its no-DELETE guard
must share the exact same hidden binding, and `CompositeBackingStore` owns those
guards for its complete lifetime. Checkpoint and recovery retain equivalent
guards across publication and reconciliation. The proposed journal append
reopen capability was removed after review showed that a stale generation could
otherwise overwrite a newer committed prefix; a future writable product path
must instead use a generation-fenced, revocable, session-exclusive append lease.

Final acceptance evidence:

- the independent final review returned **GO (P0=0, P1=0)**;
- complete Debug CTest passed 97/97 and complete Release CTest passed 98/98;
- the focused retention suite passed 20 consecutive Debug repetitions;
- `BUILD_TESTING=OFF` built the production `myrd_persistence` target;
- no test-only `create_for_testing` symbol was present in the product library;
- `git diff --check` passed apart from line-ending conversion notices.

Task 36 verdict: **DONE**. The retained design exposes no general journal append
API; generation-fenced writable reopening remains an explicit later wiring task.

## Task 37 inspect, verify and evidence acceptance

`inspect_image` adapts the bounded, read-only recovery path into an immutable
diagnostic snapshot. It reports both physical manifest slots, corruption
provenance, selection/fallback state and complete format/file identities without
retaining a writable capability or exposing a bundle path. Corrupt no-selection
outcomes remain inspectable, while invalid caller limits, cancellation and
operational I/O failures stay outer failures.

Structural verify performs the same bounded validation. Full verify streams the
ascending logical candidate set through one 64 KiB buffer, hashes a canonical
index/length/kind/data representation and reports exact DATA/ZERO counts. It
refuses to certify any fallback generation, including when a newer journal DATA
payload is corrupt. Tests cover DATA, ZERO, DISCARD, a 512-byte final partial
block and a fixed independent SHA-256 fixture.

The management report schema is deterministic and strict UTF-8. Absolute paths
require explicit opt-in; no API field accepts user block contents. Stable enums,
source commit, binary hash, artifact hashes, image/file identities and root/
commit descriptors fail closed in both C++ and `Verify-M3Json.ps1`. A native
fixture proves the actual C++ serializer and publisher output passes the
PowerShell verifier.

Evidence publication creates a unique same-directory temporary file, performs
an exact write and flush, and keeps the same write-through `DELETE` handle while
executing a no-replace handle rename. The handle denies competing writes and
deletes through the complete write/flush/rename window. Pre-rename faults retain
the temporary artifact; an existing final file is never overwritten.

Final acceptance evidence:

- independent review returned **GO (P0=0, P1=0)** after all five first-round
  P1 findings were closed;
- final Debug and Release `m3-v2|m3-v4` gates each passed 25/25;
- complete Debug CTest passed 99/99 and complete Release CTest passed 100/100;
- `BUILD_TESTING=OFF` built both `myrd_persistence` and
  `myrd_m3_management` without proxy, driver or M1 dependencies;
- `git diff --check` passed apart from line-ending conversion notices.

Accepted P2 boundaries: the inspector proves read-only behavior through the
production recovery boundary and byte-identical artifacts rather than adding a
second mutating backend abstraction; the generic exact-byte publisher relies on
the validated serializer/verifier chain for JSON semantics.

Task 37 verdict: **DONE**. Task 38 may expose these production APIs through the
local fake lifecycle CLI without adding format shortcuts.

## Tasks 39-41 provider and real AIM/NTFS acceptance

The accepted provider composition keeps persistence behind the existing M1
shared-memory wire contract and the M2 dynamic store. The real-host harness is
fail-closed around an exact non-system scratch identity, repository/scratch
containment, reparse points, destructive-boundary classification, and a LIFO
cleanup ledger. Acceptance-only NTFS quiescence waits for delayed filesystem
maintenance writes before defining the expected checkpoint corpus; it does not
promise persistence for writes issued after the checkpoint cut.

Formal verification used a clean Windows 11 guest clone at commit
`078f44b5427d14ac78eec0894877bd714da87eb5`. The imported Release server and
image SHA-256 values were
`91BA3D3B736515183317231A06BCAF674C795E331979BDA9F9DD2D61AEBE382E` and
`EF71152B693DAEB88B66599C382F1E0761AC9713E8B49A7E96BFDF44A823217D`.
The approved target was guest Disk 1 (64 GiB, non-Boot/non-System); guest Disk 0
remained the protected Boot/System disk.

The `RealFunctional` route passed:

- generation-zero bundle creation and real AIM PhysicalDrive publication;
- GPT/NTFS format followed by deterministic DATA, overwrite, sparse, rename,
  delete and exact-volume flush workload;
- complete generation 1 checkpoint, clean stop and generation 1 reopen without
  reformat, complete file hashes, and a bounded first-touch backing-read delta;
- complete generation 2 checkpoint, generation 3 compaction, and independent
  `verify --full` recovery of generation 3;
- Windows native read-only VHDX attach exposing exactly one NTFS volume;
- final cleanup with zero residuals and no remaining server process.

The fail-closed report verifier accepted
`m3-ntfs-report.json` with SHA-256
`C08A2B8304A4C20A58506E5E631F5EBFBF37680814F4D732E509B0ACA10D607E`.
An independently imported elevated provider integration executable passed
`9/9`, including the unchanged M1 wire contract. Its reproducible failure under
the non-elevated host token was traced to the intentional SYSTEM/Administrators
shared-memory DACL (`ERROR_ACCESS_DENIED`), not to provider behavior.

Tasks 39, 40 and 41 verdict: **DONE**. This closes M3-V6 normal-path
acceptance. The same-commit Task42 real interruption and Task43 M3-V7
soak/resource evidence are recorded below; overall M3 GO remains a separate
Task44 documentation, license, drift and elevated-regression decision.

## Historical Task 42/43 same-commit evidence (2026-08-08, superseded)

The evidence below remains valid only for commit `1ee1e35`. The current
Task43 harness adds production error-context preservation, concrete cleanup
gauges, performance observations and a 100 GiB logical/1 GiB allocated VHDX
reader profile. Fresh same-commit Task41/42/43 evidence is therefore required;
until then M3 is pending rather than GO.

The final evidence freeze is commit
`1ee1e350ced851f3b9702116dfb83ff01317d370`. The Release binaries used by all
three final routes were independently hashed as follows:

- AIM server: `5EC08F88613CBE195B5FB978E9F8B4A60479CB4585230A089684B7E7295011E7`;
- image CLI: `DD8561EF4CDA320E23566BE1A838EED87BAA3DB7746B46A997D654B524AFDCBA`;
- soak executable: `B398B10AAE7C03D9077F1A02CC823C559B5376B3D460C80D6A459E91A9C1F81A`;
- crash-model executable: `7A2BD0B63B6107C6E26BE5D396E3146C2F86CF56E6603C7F1F4FCAA108950562`.

Task 42 passed the host simulated oracle (10,000 schedules, 6/6 model tests)
and the real guest matrix: 10/10 scenarios, exactly 8 process kills and 2
controlled VMware power cuts. The Full aggregate says
`task42_complete=true` and `milestone_eligible=true`; its SHA-256 is
`82B7C7F1A4BC47D4845BF7CE56F2AA347EAF567A1E4CEED250780AC1C44225D5`.
Every scenario report is PASS, uses read-only-first recovery, verifies the
full corpus and NTFS without reformat, records a lazy-load counter delta,
selects only an old or new complete generation, has no fallback, and leaves
zero server/AIM residuals.

Task 43 Formal passed after independently revalidating the Task41 and Task42
dependency reports. It ran 100 seeds and 204,800 operations. Correctness
expected/actual SHA-256 is
`47F7C1DCC7EF0A22C3F3792AC84B6C9734E953CBA312E17C8C3E7CB5AB857697` for both;
checkpoint payload is 256 MiB; journal growth is 286,031,872 bytes against a
322,122,547-byte limit; startup reads are 0; extra committed bytes are
524,288; mutation pause and writer stall are both 0 ms; cleanup residuals are
zero. The Formal report says `task43_complete=true` and
`milestone_eligible=true`; its SHA-256 is
`10F7C74A5AA346E336CFA47F2B1E34DB097830111AB6B20A2B0837C11BBE7210`.

The same-commit Task41 report used by Formal has SHA-256
`0F92D894A98B8B20516E6817F164A12C60932D6E9214D0ABF719EBD55F7B9EDB` and
passed real AIM publication, NTFS formatting, two checkpoints, reopen without
reformat, bounded first-touch backing reads, compaction and native read-only
VHDX verification.

### Verification limitation and verdict boundary

A full non-elevated host Release CTest run passed 114/120. The six failures
are reproducible privileged-environment gates: four tests creating `Global\`
shared-memory mappings return `ERROR_ACCESS_DENIED`, one provider test has the
same mapping restriction, and native VHDX compatibility reports
`administrator_token_required`. The exact same six Release binaries were then
run in the elevated Windows 11 guest and passed 6/6 (native VHDX 5/5), so the
combined fresh coverage is 120/120. The Task43-specific CTest subset passed
5/5. These environment details are recorded rather than converted to SKIP or
hidden. The privileged supplement archive SHA-256 is
`2852F5FD282B9AA252D53CA2051D6845AC46F51CA7CA5F73645EC75FC3571804` and its
machine-readable result SHA-256 is
`DB61B53918B1FDFE7CF14768848DE3AD3E67070DB0B5571AFF087B22613F987E`.
At historical commit `1ee1e35`, Task42 and Task43 were recorded
**DONE/PASS**; that verdict is superseded. The current M3 release verdict is
**PENDING fresh same-commit evidence and Task44 closure**.
