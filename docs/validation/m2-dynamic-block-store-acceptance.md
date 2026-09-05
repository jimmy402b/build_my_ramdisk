# M2 Dynamic Block Store Acceptance — Task 21 Staged Evidence

Date: 2026-08-04 (Asia/Shanghai)

## Verdict

**M2-A PASS. M2-B PASS. M2-C PASS. M2 GO.** The final accepted Release
candidate is commit `ca83de4e274849b7defdd4d0ec1bbaa3840993a6`; its
`myrd-m2-e2e.exe` SHA-256 is
`86A85457D44D1B25089A6033B11FF3D221B2CEE778BA8D3A47690A175FDBBA63`.
Task 21's ordered core/concurrency/real-NTFS gates and Task 22's memory,
performance, fault, 20-cycle soak, final M2 regression, and final M1
regression all pass. The final V6 and V7 aggregate reports independently pass
`Verify-M2Json.ps1`; no failed attempt was reused as passing evidence.

## Reproducible environment facts

| Fact | Observed value |
|---|---|
| Worktree | `<local-worktree>\m2-implementation` |
| Branch | `feature/m2-dynamic-block-store` |
| Start commit | `724a00e2cf22e65de0eab18f0a6423f8194156be` |
| Initial isolated-worktree status | clean; `git status --short` produced no output before Task 00 edits |
| Parent checkout status | dirty user-owned M0/M1 diagnostics; exact protected snapshot below |
| Functional M1 commit | `3eac4f7a477c134e5dfac0fc58ff160bcd9e9f23` (ancestor, M1 acceptance source baseline) |
| Merged planning baseline | `2adf84ab16187a749bf350dee3d350cd297d52d6` (ancestor) |
| M0 acceptance | `docs/validation/m0-driver-only-acceptance.md`: final M0 GO |
| M1 acceptance | `docs/validation/m1-proxy-acceptance.md`: final M1 GO |
| Host | `<redacted-test-host>` |
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

## Task 21 — Ordered M2-A, M2-B, and M2-C functional/cleanup gates

### Candidate identity and execution policy

All Task 21 gates used commit
`e0ee6d290dcdfb99b253824d18ff6aa627063c80` and the same clean-built Release
candidate. The `myrd-m2-e2e.exe` SHA-256 is
`BAB718C1869362AC0FD5B16180B498136DEE007E4BDAF205245DD180D7B84F68`.
The checked-in MSVC/C++20 build policy was unchanged. The commands were run
once in the Plan order. The only extra V2 execution selected the two already
passing tests with `-V` so successful-test diagnostics would be retained; it
was not a retry after failure.

Local CTest and M1 logs are retained under
`reports/m2-task21/e0ee6d2/`. This directory is generated evidence and is
ignored by Git. The real-device evidence is retained in the unique immutable
run directory listed below.

### Gate results

| Gate | Command | Exit / selected tests | Canonical evidence | Result |
|---|---|---|---|---|
| M2-V1 core | `ctest --preset release -L m2-v1 --output-on-failure` | `0`; 16/16 | `reports/m2-task21/e0ee6d2/m2-v1.log` | PASS |
| M2-V2 model/VM | `ctest --preset release -L m2-v2 --output-on-failure` | `0`; 6/6 | `m2-v2.log`, `m2-v2-details.log` | PASS |
| M2-V3 run 1 | `ctest --preset release -L m2-v3 -V` | `0`; 6/6 | `m2-v3-run1.log` | PASS |
| M2-V3 run 2 | same command, immediately after run 1 | `0`; 6/6 | `m2-v3-run2.log` | PASS |
| M2-V4 local proxy/regression | `ctest --preset release -L m2-v4 --output-on-failure` | `0`; 35/35 | `m2-v4.log` | PASS |
| Full required M1 local regression | `Invoke-M1Verification.ps1 -Tier M1-V2 -Configuration Release` | `0`; 14/14 M1 units, 10,000-case fuzz, 50/50 SHM | `m1-v2.stdout.log`; empty stderr | PASS |
| M2-V5 real NTFS | `Invoke-M2Verification.ps1 -Tier M2-V5 ... -SafetyAcknowledged` | `0`; one immutable GO report and independent PASS | `out/m2-orchestrated/m2-v5-20260804-084228-9986646-392296bc65344eecba1e9ea25ec78162/` | PASS |

The V1 selection includes block math, immutable backing, sparse directory,
store state, read/write/ranges, ZERO/UNMAP, limits, snapshot, generation, and
eviction tests. No required test was skipped and CTest did not report an empty
selection.

V2's fixed model completed exactly 100,000 operations with seed
`0x4D5952444D324131` and digest `0xDD89B58C508C5E36`. The real 64 GiB VM flow
reserved 68,719,476,736 bytes with zero initial committed store bytes, wrote
and verified 16,384 blocks / 1,073,741,824 bytes, and reclaimed
1,076,412,416 bytes versus the 966,367,641-byte minimum after UNMAP. The 1 TiB
flow reserved 1,099,511,627,776 bytes, committed only 196,608 bytes for three
distant blocks, allocated three sparse leaves, and returned handles/threads
exactly to baseline after destruction.

Both V3 runs passed on their first execution. Run 1 used seed
`0x4d59524453545231`, completed 1,152 reads, 948 writes, 576 ZERO and 576
UNMAP operations, and observed 10 real pressure evictions. Run 2 completed
1,152 reads, 962 writes, 576 ZERO and 576 UNMAP operations, and observed five
real pressure evictions. Each resource audit completed 20 store lifecycles,
returned handles `67 → 67`, threads `4 → 4`, private growth `0`, and VM
reservations/releases `21/21`. The 22-case deterministic concurrency suite,
cancellation/deadline paths, pressure, shutdown, generation acknowledgement,
invariants, and resource checks all ran in each V3 gate.

### Canonical real NTFS evidence

The Task 21 V5 report is
`m2-report-0aeeb02e4414e79cc879bef42ed1dd42-36948372f1545719273abb1ad5106018.json`
with SHA-256
`BEC7A6D9CF9497CB8BB87797CA4B15961AB3F2F7B10F388485326181F6470FBC`.
Its embedded executable hash exactly matches the candidate above. Both native
semantic validation and the independent `Verify-M2Json.ps1` pass.

The exact 64 GiB AIM device (raw device ID `0`, `PhysicalDrive1`) was confirmed
by QUERY_DEVICE and independent physical-drive identity before mutation. GPT
initialization, one NTFS partition, formatting, run-unique directory mount,
deterministic writes, modification, file/directory rename, flush/close,
readback and hash verification all passed while the provider remained alive.
Written/read bytes are each `4,294,967,296`; deleted bytes are
`2,147,483,648`; the aggregate write/read SHA-256 is
`656A5FAB03096E885C879D2A19775398EEF1421A0E62348EF8EEF2BD886CD6BD`.
NTFS DeleteNotify was explicitly enabled and ReTrim succeeded.

Store committed bytes moved `0 → 4,365,287,424 → 2,217,934,848` at READY,
AFTER-WRITE and AFTER-RETRIM. Cumulative UNMAP bytes increased from
`68,770,332,672` after write to `73,355,886,592` after Delete/ReTrim. The
sequential/random ratios were 11,215/8,466 basis points against the V5
7,000/6,000 thresholds; Task 22 still owns the separate V6 performance and
1-GiB memory proof and these V5 values do not waive it.

Cleanup removed only the exact owned mount and AIM device. Native and outer
before/after inventories both report MATCH, AIM device lists returned empty,
only physical Disk 0 remained, and `phdskmnt` returned to Running/Automatic.
No Task 21 attempt failed, no evidence directory was reused, and no threshold,
JSON report, cleanup target, or test selection was changed during execution.

### Task 21 checkpoint verdicts and remaining boundary

- **M2-A PASS:** V1 core and V2 model/real-VM evidence are complete.
- **M2-B PASS:** both V3 runs, V4, and the full required M1 local regression
  are complete without retry-to-pass or resource drift.
- **M2-C functional/cleanup PASS:** the canonical V5 real NTFS run is complete
  and exact cleanup is proven.
- **At the Task 21 checkpoint, full M2-C remained pending Task 22 and `M2 GO`
  was not issued.** V6 still had to prove the dedicated
  memory/performance/fault gates, and V7 still had to complete 20 real Release
  lifecycles followed by final regression and report validation. The final
  Task 22 section below closes that historical boundary.

M2 still does not implement or claim durable filesystem backing, a journal,
checkpoint/recovery, timed writeback, crash recovery, M3 persistence, or M4.

## Task 22 — Final memory, performance, fault, and soak acceptance

### Final candidate and commands

All final Task 22 evidence is bound to commit
`ca83de4e274849b7defdd4d0ec1bbaa3840993a6` and executable SHA-256
`86A85457D44D1B25089A6033B11FF3D221B2CEE778BA8D3A47690A175FDBBA63`.
The pinned `phdskmnt.sys` SHA-256 remained
`3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9`.

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --build .\out\build\msvc --config Release --clean-first
ctest --test-dir .\out\build\msvc -C Release --output-on-failure

powershell -File .\tools\Invoke-M2Verification.ps1 -Tier M2-V6 `
  -Configuration Release -EvidenceBase .\out\m2-orchestrated `
  -Executable .\out\build\msvc\src\Release\myrd-m2-e2e.exe `
  -AcceptedM1Report 'C:\path\to\accepted-m1-report.json' `
  -ExpectedDriverSha256 3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9 `
  -SafetyAcknowledged

# The same command was then run with -Tier M2-V7.
powershell -File .\tools\Verify-M2Json.ps1 -Tier M2-V6 `
  -Path .\out\m2-orchestrated\m2-v6-20260804-100956-df15869849e7\m2-v6-aggregate.json `
  -Executable .\out\build\msvc\src\Release\myrd-m2-e2e.exe
powershell -File .\tools\Verify-M2Json.ps1 -Tier M2-V7 `
  -Path .\out\m2-orchestrated\m2-v7-20260804-101335-0ff845aa6db8\m2-v7-aggregate.json `
  -Executable .\out\build\msvc\src\Release\myrd-m2-e2e.exe
```

The clean Release build and full CTest exited `0`; all `57/57` tests passed.
Both final orchestrated commands and both independent aggregate-verifier
commands exited `0`.

### M2-V6 results

Canonical evidence:
`out/m2-orchestrated/m2-v6-20260804-100956-df15869849e7/`.
The aggregate report is `m2-v6-aggregate.json`, SHA-256
`A0DA6F51FDDFE0975E0234DC11F56BA70E76A9961D3AF53424D96C1C702E67EF`.

| Gate | Observed | Required threshold | Result |
|---|---:|---:|---|
| Empty 64 GiB Ready/fixed metadata | 102,400 bytes | <= 134,217,728 bytes | PASS |
| 1 GiB unique-write total growth | 1,079,177,216 bytes | <= 1,181,218,406 bytes including fixed metadata | PASS |
| Store committed after write | 1,073,741,824 bytes | exact 1 GiB | PASS |
| Reclaimed after UNMAP | 1,076,420,608 bytes in 30 ms | >= 966,367,641 bytes within 10,000 ms | PASS |
| Request-path private growth | 0 bytes; constant-space descriptor; 8 MiB maximum request allocation | no unbounded per-request growth | PASS |
| Sequential QD1 ratio | 11,605 basis points | >= 7,000 | PASS |
| Random QD1 ratio | 7,844 basis points | >= 6,000 | PASS |
| Real lifecycle fault boundaries | 10/10 | every declared boundary with exact cleanup | PASS |

The normal V6 real run also performed GPT initialization, NTFS formatting,
4,294,967,296-byte write/hash readback, 2,147,483,648-byte delete/ReTrim,
mount removal, exact AIM device removal, provider stop, and service/baseline
restoration. Fault runs covered `capture-baseline`, `construct-provider`,
`start-provider`, `create-device`, `wait-for-disk`, `setup-write`,
`sample-after-write`, `delete-retrim`, `sample-after-retrim`, and
`performance`. This includes both pre-format raw-disk and formatted-volume
failure boundaries; formatting is a normal filesystem step, not a driver
availability workaround.

### M2-V7 results and final regressions

Canonical evidence:
`out/m2-orchestrated/m2-v7-20260804-101335-0ff845aa6db8/`.
The aggregate report is `m2-v7-aggregate.json`, SHA-256
`B9DCC2E54CCD726597E494C5DBF406C1CD972D9515CA57D0094867BC9F627B04`.

All `20/20` independent Release processes returned GO and exact outer
inventory cleanup. Every cycle ran the full 4 GiB V5 workload rather than the
permitted smaller soak region. Across the soak, 85,899,345,920 bytes were
written and hash-read, and 42,949,672,960 bytes were deleted/ReTrimmed.
Sequential ratios ranged from 9,238 to 15,420 basis points and random ratios
from 7,260 to 9,833; all remained above 7,000/6,000. No cycle had a structural
baseline mismatch or a process-resource growth failure.

After cycle 20, the same orchestrator freshly ran M2-V1, M2-V2, M2-V3 twice,
M2-V4, and M1-V2 Release. Every recorded exit code is `0`; the aggregate
stores hashes for each stdout log. Independent V6 and V7 JSON verification
then passed again against the final executable.

### Failed-attempt and defect history

No failed attempt was deleted or counted toward the final 20 cycles.

- An initial V7 invocation used a mistyped expected driver hash and failed in
  preflight before disk mutation.
- `m2-v7-20260804-093800-da3d99c8f563` stopped at cycle 12 after a valid NTFS
  workload because two runtime threads were released (`4 -> 2`). Commit
  `6b76170` unified native and JSON resource policy as a non-growth leak gate
  and made resource-only NO-GO reports valid and auditable.
- `m2-v7-20260804-100050-e5bfd31c0cb5` stopped at cycle 7 with a transient
  final handle sample (`119 -> 120`). Commit `ca83de4` added a bounded 20 x
  50 ms post-cleanup stabilization window; persistent growth still fails.
- Earlier Task 22 implementation defects were fixed in `d479906`, `587871d`,
  `4a8b322`, `211fe14`, and `ef48f30`: real fault/soak gates, Win32-safe
  evidence paths, exact fault schema fields, strictly ordered fast-fault
  samples, and confirmed raw-disk fault evidence.

After each executable change, the Release suite and V6 were rerun and V7 was
restarted at cycle 1. Thresholds, cleanup identities, and failed reports were
not edited to obtain PASS.

### M3 handoff and scope boundary

M2 hands M3 a stable `IBlockDevice` path, immutable `IBackingStore` and
`BackingView` contracts, dirty snapshot/cursor/base-generation evidence,
install-before-ack ordering, exact version/kind/generation acknowledgement,
and observable stats and limits. The accepted tests prove that acknowledged
data/zero state cannot resurrect from an older generation after eviction and
reload.

M2 deliberately does **not** implement durable file backing, a journal,
checkpoint/recovery, timed writeback, crash consistency, or power-loss
recovery. Those remain M3/M4 work and are not implied by `M2 GO`.

The full Plan Definition of done was reread after the final run. M1 regression,
large sparse reservation, model/proxy/NTFS correctness, lazy/single-flight
backing behavior, eviction and generation safety, limits/fault/shutdown,
100,000-operation model, concurrency/resources, 4 GiB NTFS, V6, V7, exact
cleanup, and machine-readable evidence are all covered by the final candidate.
No M3/M4 behavior or durability claim was added. Final verdict: **M2 GO**.
