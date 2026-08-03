# M2 Dynamic Block Store Acceptance — M2-V0 and M2-B Evidence

Date: 2026-08-04 (Asia/Shanghai)

## Verdict

**M2-V0 remains PASS. The Task 14–16 concurrency/resource evidence owned by
M2-B is PASS.** The fixed-seed long stress, deterministic concurrency suite,
invariant audits, and repeated lifecycle resource checks are green without
retrying away a failure. The formal ordered M2-B stage verdict remains pending
Task 21 because Task 18 must first close the remaining real-VM portion of
M2-V2/M2-A. This document does not claim M2-C or an M2 GO verdict.

## Reproducible environment facts

| Fact | Observed value |
|---|---|
| Worktree | `C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m2-implementation` |
| Branch | `feature/m2-dynamic-block-store` |
| Start commit | `724a00e2cf22e65de0eab18f0a6423f8194156be` |
| Initial isolated-worktree status | clean; `git status --short` produced no output before Task 00 edits |
| Parent checkout status | dirty user-owned M0/M1 diagnostics; exact protected snapshot below |
| Functional M1 commit | `3eac4f7a477c134e5dfac0fc58ff160bcd9e9f23` (ancestor, M1 acceptance source baseline) |
| Merged planning baseline | `2adf84ab16187a749bf350dee3d350cd297d52d6` (ancestor) |
| M0 acceptance | `docs/validation/m0-driver-only-acceptance.md`: final M0 GO |
| M1 acceptance | `docs/validation/m1-proxy-acceptance.md`: final M1 GO |
| Host | `JIMMYLIN_IN_LAB` |
| Windows | Windows 11 Pro, `10.0.26200`, x64 |
| CPU | AMD Ryzen 5 5600, 12 logical processors |
| Physical memory | `68,359,413,760` bytes |
| PowerShell | Windows PowerShell `5.1.26100.8875`, x64 process |
| MSVC | compiler `19.44.35228.0`; tools `14.44.35207` |
| Windows SDK | `10.0.26100.0` |
| CMake | `4.2.2` |
| CTest | `4.2.2` |
| Ninja | `1.13.2` |
| Generator | checked-in `Ninja Multi-Config` preset `msvc` |
| Target architecture | x64 (`VSCMD_ARG_TGT_ARCH=x64`, 64-bit process/OS) |

The parent checkout was deliberately left untouched. Its protected
`git status --short` snapshot was:

```text
 M tasklist.md
?? .claude/
?? _diag_map.cpp
?? _diag_map.exe
?? _diag_map.obj
?? _recover_task25_orphan.exe
?? _recover_task25_orphan.obj
?? _recover_task25_signal.exe
?? _recover_task25_signal.obj
?? _test2.cpp
?? _test2.exe
?? _test2.obj
?? _test_map.cpp
?? _test_map.exe
?? _test_map.obj
?? _test_map2.ps1
?? _test_map3.ps1
?? _test_map4.ps1
?? _test_mapE.exe
?? _test_prov.cpp
?? _test_prov.exe
?? _test_prov.obj
?? _test_run.ps1
?? _test_run2.ps1
?? _v3_final.ps1
?? _v4.ps1
?? _v4b.ps1
?? _v4c.ps1
?? _v4d.ps1
?? _v4e.ps1
?? _v4f.ps1
?? _v4g.ps1
?? _v4h.ps1
?? _v5t.ps1
?? tools/_diag_scsi.ps1
?? tools/_list_scsi.ps1
?? tools/_v3_elevated.ps1
?? tools/_v3_run.ps1
```

The checked-in preset mapping is unchanged: `msvc` configures
`out/build/msvc`; `debug` and `release` select the Debug and Release
configurations. The repository root enforces C++20, MSVC, and an 8-byte
pointer size. M1 and the new shell use the same `myrd_enable_warnings` policy:
`/W4 /WX /permissive- /Zc:__cplusplus /EHsc /utf-8`. No runtime-library,
Unicode, exception, or RTTI policy difference was introduced; settings not
explicitly set by the repository continue to come from the same MSVC/CMake
defaults.

The pinned AIM reference is available in ignored
`out/reference/aim` at
`d379caa027d21902e45fce8c9daf02fd116c2dda`. Its four normative files were
hash-verified after a local clone from the parent checkout because direct
GitHub access was unavailable through the configured proxy.

## Public M1 API and target mapping inspected

| Contract or target | Actual path/name and role |
|---|---|
| `IBlockDevice`, `DataRange`, `DeviceFeatures` | `src/storage/block_device.h` |
| `OperationContext`, cancellation/deadline check | `src/common/operation_context.h` |
| `Result<T>` / `Result<void>` | `src/common/result.h` |
| `ErrorDomain`, `OperationErrorCode`, `Error` | `src/common/error.h` |
| M1 provider lifecycle | `src/m1/m1_provider.h` / `src/m1/m1_provider.cpp`; `start`, `stop`, snapshots and named-object ownership |
| M1 lifecycle and exact cleanup seams | `src/m1/m1_lifecycle_runner.h` / `src/m1/m1_lifecycle_runner.cpp`; injectable kernel/device locator interfaces and `stop_and_release` |
| M1 structured report | `src/m1/m1_report.h` / `src/m1/m1_report.cpp`; driver, provider, raw-I/O and cleanup evidence |
| Existing storage library | `myrd_storage` in `src/CMakeLists.txt`; `MemoryBlockDevice` plus `block_device.h`, linked publicly only to `myrd_common` |
| New shell executable | `myrd-m2-e2e` in `src/CMakeLists.txt`, source `src/m2/main.cpp`, linked only to existing `myrd_storage` |
| Existing M1 executable | `myrd-m1-e2e` |
| Existing driver/proxy targets | `myrd_driver`, `myrd_proxy`; not dependencies of `myrd_storage` |
| Test framework | checked-in `tests/support/test_framework.h`; no new framework or fake pass test was added |

The target graph was extended by consuming the existing storage target. No
second storage library was created and no storage, proxy, driver, provider,
report, or cleanup implementation source was changed.

## TDD red/green evidence

Before the build-shell edit, after a clean baseline configure/build:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build --preset release --target myrd-m2-e2e
```

Exit code: `1`.

Observed failure:

```text
ninja: error: unknown target 'myrd-m2-e2e', did you mean 'myrd-m1-e2e'?
```

After adding only `src/m2/main.cpp` and the target declarations:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset release --target myrd-m2-e2e
```

Exit code: `0`; the target compiled and linked in 2/2 steps. The entry point
returns immediately and performs no storage behavior.

## Fresh baseline and post-change verification

All commands below were run from the worktree above.

| Phase | Command | Exit/result |
|---|---|---|
| Pre-change configure | `cmake --preset msvc --fresh` | `0`, configure/generate succeeded |
| Pre-change Release build | `cmake --build --preset release --clean-first` | `0`, 92/92 build steps |
| Pre-change full local regression | `ctest --preset release --output-on-failure` | `0`, 26/26 passed |
| Fresh M1 local gate before shell | `& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -Configuration Debug` | `0`, M1-V2 PASS: 14/14 M1 unit tests, 1/1 fuzz, 50/50 local SHM |
| Post-change independent storage build | `cmake --build --preset release --target myrd_storage --clean-first` | `0`, 6/6 steps; only `myrd_common` and `myrd_storage` were built |
| Post-change configure | `cmake --preset msvc --fresh` | `0`, configure/generate succeeded |
| Post-change Release build | `cmake --build --preset release --clean-first` | `0`, 94/94 build steps |
| Post-change full local regression | `ctest --preset release --output-on-failure` | `0`, 27/27 passed; no `No tests were found` or skipped test |
| Fresh M1 local gate after shell | `& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -Configuration Debug` | `0`, M1-V2 PASS: 14/14 M1 unit tests, 1/1 fuzz, 50/50 local SHM |

The post-change Release CTest label summary was deterministic:

| Label | Selected real tests | Meaning at Task 00 |
|---|---:|---|
| `m2-v1` | 4 | Existing operation-context, block-device-contract, and storage prerequisite tests plus the independent-storage build gate |
| `m2-v2` | 4 | Same real prerequisite slice reserved until model/VM tests exist |
| `m2-v3` | 4 | Same real prerequisite slice reserved until concurrency/resource tests exist |
| `m2-v4` | 27 | Complete current local regression baseline plus the independent-storage build gate |

These are CTest labels on existing deterministic tests, not artificial tests
created to make a label pass. Future M2 tasks must add their actual tests to
the appropriate labels before claiming a tier. The M2-V4 baseline is captured
from CMake's complete registered-test directory property after all accepted
M1/legacy `add_test` calls, rather than duplicated as a hand-maintained name
list; the structural probe is then appended explicitly.

The storage boundary is also enforced continuously by
`structural.m2.storage_independent_build`. CMake configuration fails if
`myrd_storage` properties mention `myrd_proxy` or `myrd_driver`, including
inside generator expressions or manually added dependencies. The CTest gate
creates a PID/GUID-unique child below the exact ignored
`out/build/storage-boundary-probes` base, configures a fresh isolated build
with tests disabled, and builds only `myrd_storage`. It rejects the resolved
build whenever target-specific build paths or produced artifacts contain the
proxy or driver implementation targets, so intermediate targets, generator
expressions, and `add_dependencies()` edges are covered by the actual
generator-resolved dependency closure. Each run removes only its own verified
non-reparse-point child in `finally`; two simultaneous CTest processes were
run successfully and left the probe base empty. Its PowerShell body is
generated into the build tree by `tests/CMakeLists.txt`, keeping Task 00
inside its exact source-file allowlist. The probe was fault-tested by
temporarily injecting `add_dependencies(myrd_storage myrd_driver)`: the old
gate incorrectly passed, while the isolated gate failed after observing the
driver build. A Debug-only generator-expression dependency was also rejected
at configure time. Both injections were removed before the canonical green
run.

The Release Ninja dependency query used for the initial audit was:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
ninja.exe -C out/build/msvc -f build-Release.ninja -t query src/Release/myrd_storage.lib
```

Its build inputs were `memory_block_device.cpp.obj` and order-only
`src/Release/myrd_common.lib`; `myrd_proxy` and `myrd_driver` appeared only as
downstream outputs/consumers. Together with the isolated CTest probe, this
proves `myrd_storage` remains independently buildable without proxy or driver
implementation dependencies.

## Bootstrap attempts retained as audit facts

The first M1-V2 invocation exited `1` before tests because
`out/reference/aim` was absent. The checked-in fetch script then exited `1`
under Windows PowerShell 5.1 when Git clone progress was promoted from its
`2>&1 | Out-Host` pipeline to `NativeCommandError`. A direct GitHub clone also
exited `1` because the configured `127.0.0.1` proxy could not connect to
GitHub. No product files were changed by these attempts. The existing pinned
reference in the parent checkout supplied the exact commit locally, after
which the unchanged verifier passed and its four normative files were hashed.

## Drift audit and scope boundary

- M0 acceptance, M1 acceptance, functional commit `3eac4f7`, and planning baseline `2adf84a` were inspected; both commits are ancestors of the starting worktree.
- The only tracked product/docs changes for Task 00 are `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `src/m2/main.cpp`, this acceptance document, and the Task 00 status/execution record in `M2_TASKLIST.md`.
- No M1 behavior source changed. `myrd_storage` was not duplicated and its existing implementation was not changed.
- No Task 01 dynamic block-store source, allocation, backing, eviction, generation, snapshot, proxy injection, driver, NTFS, persistence, or performance behavior was added.
- The CTest labels are reservations with real prerequisite coverage; their passing counts do not claim M2 behavior.
- No CodeGraph initialization was performed.

The preceding statements describe the Task 00 baseline at the time it was
captured. They are retained as historical evidence; the current implementation
and gate status are recorded below.

## Task 16 — M2-B concurrency and resource evidence

Task 16 adds `unit.m2.stress` beside the 22-case deterministic
`unit.m2.concurrency` suite. It changes no product implementation. The new
target has a 120-second outer CTest timeout; each storage operation uses a
five-second deadline, and the source contains no timing sleep.

### Fixed-seed multi-role stress

The canonical seed is `0x4D59524453545231`. Three rounds each launch two
writers, one ZERO caller, one UNMAP caller, two readers, one dirty
snapshot/successor/install/exact-ack role, and one forced clean-pressure role.
Each data role executes exactly 192 operations across 20 selected blocks in
three sparse metadata leaves of a 2,050-block store. Mutations to one block are
sequenced only by an independent shadow operation lock; readers overlap them
and accept only a complete historical version, never torn bytes. Operations
on different blocks, snapshot/install/ack, the eviction worker, and explicit
pressure remain concurrent.

The shadow owns its bytes, versions, dirty kinds, per-version digests, and the
generation interval observed around each mutation. It does not read product
data to construct expected bytes. Snapshot records must match the shadow's
version, kind, payload digest, and dirty-base interval before they may be
applied to an immutable fake successor. Generation-bound acknowledgements are
checked as exact success or `VersionChanged` according to the independently
serialized shadow state.

Every round has two quiescent audits. The pre-drain audit proves active dirty
state and dirty-base generation intervals before acknowledgement can hide a
discrepancy. The post-drain audit performs clean pressure, full byte-by-byte
comparison, and reload validation. Both audits reconcile stable state,
`prior_state`, owner/transition epoch, pins, version, dirty kind, clean/dirty
generation bounds, all state gauges, commit reservations, arena committed
bytes, the installed generation, and the three allocated sparse leaves.

One submitted-state Release evidence run reported:

```text
M2_STRESS_SUMMARY seed=0x4d59524453545231 reads=1152 writes=951
zeroes=576 unmaps=576 pressure_rejections=201 pressure_passes=768
pressure_scanned=3138 pressure_evicted=7 installs=82 exact_acks=163
stale_acks=270 generation=83
```

The exact interleaving-dependent counters may vary, but the seed, role counts,
operation bounds, invariants, byte comparisons, and non-zero coverage gates do
not. On failure the test prints the seed, first failure, the last 16 operations,
all role/pressure counters, and persistence counters.

### Resource accounting

The resource test warms up once, captures five process baseline samples, then
creates, exercises, closes, and destroys 20 stores. A real Win32 VM wrapper
tracks every reservation, commit, decommit, and release. Each cycle separately
requires zero active reservation, zero committed arena bytes, zero backing
objects, zero cleanup release failures, and a stopped/joined store worker.
Process handle and thread counts must return exactly to baseline. Private bytes
must remain within measured baseline noise plus a documented 1 MiB allocator
allowance; exact arena accounting remains the correctness gate.

The submitted-state Release sample was:

```text
M2_RESOURCE_SUMMARY cycles=20 handles_before=61 handles_after=61
threads_before=4 threads_after=4 private_before=1929216
private_after=1929216 private_growth=0 baseline_noise=0
private_tolerance=1048576 reserves=21 releases=21
```

### Reproducible verification

| Gate | Result |
|---|---|
| Debug exact `unit.m2.stress` repeat-until-fail 20 | 20/20, exit 0 |
| Release exact `unit.m2.stress` repeat-until-fail 20 | 20/20, exit 0 |
| Clean-first Debug build and full CTest | build 134/134; tests 42/42 |
| Clean-first Release build and full CTest | build 134/134; tests 42/42 |
| Release M2-V1 cumulative label | 16/16 in the final full run |
| Release M2-V2 cumulative label | 5/5 in the final full run |
| Release M2-V3 consecutive run 1 | 6/6, exit 0 |
| Release M2-V3 consecutive run 2 | 6/6, exit 0 |

An independent strict read-only review initially found five Important issues
and one Minor diagnostic gap: shadow locks covering store calls, no active-dirty
pre-drain audit, weak pressure evidence, unhandled helper-thread exceptions, an
overly broad private-byte tolerance, and insufficient failure history. All six
were corrected. The final independent re-review returned `APPROVED` with no
remaining Critical, Important, or Minor finding.

### Gate boundary

The M2-B-owned concurrency/resource evidence is complete. Formal ordered M2-B
acceptance is intentionally deferred to Task 21 until Task 18 completes the
real 64 GiB/1 TiB VM integration required by the preceding M2-A/M2-V2 gate.
Proxy injection, real driver/NTFS behavior, persistence, performance, M2-C, and
the overall M2 GO verdict are outside Task 16 and remain unclaimed.
