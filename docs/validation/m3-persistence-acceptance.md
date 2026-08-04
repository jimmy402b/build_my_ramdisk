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
