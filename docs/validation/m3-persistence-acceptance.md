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
