# M2 Dynamic Block Store Acceptance — Task 00 / M2-V0

Date: 2026-08-03 (Asia/Shanghai)

## Verdict

**M2-V0 PASS for the build shell and baseline only.** M1 is demonstrably GO on
this worktree, the Release build is green, and `myrd-m2-e2e` is present and
buildable. M2 product implementation has not started; this document does not
claim M2-V1, M2-V2, M2-V3, M2-V4 behavior, M2-A/B/C, or an M2 GO verdict.

## Reproducible environment facts

| Fact | Observed value |
|---|---|
| Worktree | `C:\Users\JimmyLin\Desktop\build_my_ramdisk\.worktrees\m2-implementation` |
| Branch | `feature/m2-dynamic-block-store` |
| Start commit | `724a00e2cf22e65de0eab18f0a6423f8194156be` |
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
| Post-change full local regression | `ctest --preset release --output-on-failure` | `0`, 26/26 passed; no `No tests were found` or skipped test |
| Fresh M1 local gate after shell | `& .\tools\Invoke-M1Verification.ps1 -Tier M1-V2 -Configuration Debug` | `0`, M1-V2 PASS: 14/14 M1 unit tests, 1/1 fuzz, 50/50 local SHM |

The post-change Release CTest label summary was deterministic:

| Label | Selected real tests | Meaning at Task 00 |
|---|---:|---|
| `m2-v1` | 3 | Existing operation-context, block-device-contract, and storage prerequisite tests |
| `m2-v2` | 3 | Same real prerequisite slice reserved until model/VM tests exist |
| `m2-v3` | 3 | Same real prerequisite slice reserved until concurrency/resource tests exist |
| `m2-v4` | 26 | Complete current local regression baseline, including all M1 and legacy local tests |

These are CTest labels on existing deterministic tests, not artificial tests
created to make a label pass. Future M2 tasks must add their actual tests to
the appropriate labels before claiming a tier.

The Release Ninja dependency query was:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
ninja.exe -C out/build/msvc -f build-Release.ninja -t query src/Release/myrd_storage.lib
```

Its build inputs were `memory_block_device.cpp.obj` and order-only
`src/Release/myrd_common.lib`; `myrd_proxy` and `myrd_driver` appeared only as
downstream outputs/consumers. This proves `myrd_storage` remains independently
buildable without proxy or driver implementation dependencies.

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

M2 product implementation has not started. The next task may begin only after
this M2-V0 build-shell evidence and the existing M1 GO baseline remain intact.
