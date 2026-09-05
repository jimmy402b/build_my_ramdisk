# M4 redesign baseline: GitHub M3 freeze

Status: **ACCEPTED UPSTREAM / M4 DEVELOPMENT BASELINE**

The GitHub M3 acceptance is treated as a trusted upstream milestone. Full M3
CTest is not a prerequisite for T01 and later M4 implementation tasks; the
historical local failures remain recorded below for transparency and are not
reclassified as M4 defects.

## Source baseline

The M4 redesign worktree is based directly on the GitHub `main` commit:

```text
Remote: https://github.com/jimmy402b/build_my_ramdisk.git
Branch: main
Commit: df09c4f0ac310b00e5b53be594c4bfba6730aaec
Worktree branch: feature/m4-single-multi-disk
```

The M3 acceptance statement in `README.md` says:

> M3：VHDX + journal + 双槽 manifest 持久化里程碑已通过最终验收（证据冻结提交 `4ece675`）；Task41 真实 AIM+NTFS、Task42 崩溃矩阵、Task43 Formal 和 Task44 漂移审计均为 GO。

The README identifies these M3 evidence paths:

```text
docs/validation/m3-persistence-acceptance.md
docs/validation/m3-final-audit.md
```

The M3 E2E documentation records the accepted Formal source commit as
`4ece6756de704a4cd6a967d94aca22253063a12c`.

## M4 architecture boundary

This redesign reuses the M3 persistence/provider implementation and the
contracted single-disk runtime direction. It does **not** inherit the historical
Task23 `src/control`, `src/management`, or `src/host` service/IPC platform as the
runtime foundation.

## Historical baseline verification (pre-MSVC)

Commands were run from the clean worktree root:

```powershell
cmake --preset msvc
```

Result: **exit code 1 (blocked)**

Observed output:

```text
-- The CXX compiler identification is GNU 13.1.0
-- Check for working CXX compiler: C:/Program Files/JetBrains/CLion 2026.1/bin/mingw/bin/g++.exe - skipped
CMake Error at CMakeLists.txt:5 (message):
  M0 requires MSVC
-- Configuring incomplete, errors occurred!
```

Because configuration failed before a build tree was generated, the required
Debug/Release build and CTest commands were not run. Per the task contract,
baseline work stops here until an MSVC environment is available.

Not run:

```powershell
cmake --build --preset debug --parallel 1
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel 1
ctest --preset release --output-on-failure
```

## MSVC baseline sanity check

The Visual Studio 2022 x64 environment was then loaded explicitly with:

```powershell
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

The following command succeeded (exit code 0):

```powershell
cmake --preset msvc --fresh
```

Compiler: MSVC `19.44.35228.0` (`cl.exe` from MSVC 14.44.35207).

The full Debug build previously succeeded (exit code 0), but its full CTest run
is not required to reopen M4 work. A targeted M3 persistence library build was
used as the development sanity check below.

First, available targets were listed with:

```powershell
cmake --build --preset debug --target help
```

The target `myrd_persistence` was present. It was then built with:

```powershell
cmake --build --preset debug --target myrd_persistence --parallel 1
```

Result: **exit code 0**; `out/build/msvc/src/Debug/myrd_persistence.lib` was
produced successfully.

The prior full Debug CTest command (retained as historical evidence) was:

```powershell
ctest --preset debug --output-on-failure
```

Result: **exit code 8; 149/160 tests passed, 11 failed, 2 skipped**.

The failed tests were:

```text
21   integration.shm
26   unit.lifecycle_runner
29   unit.m3.m2_consistent_snapshot (timeout)
33   integration.m2.shm
52   unit.m2.windows_acceptance
71   integration.m3.windows_vhdx
100  integration.m3.provider
127  unit.m4.profile_store
129  unit.m4.operation_store
137  integration.m4.stop_forget
143  integration.m4.windows_worker_recovery
```

The two skipped tests were `e2e.m4.windows_service_scm` and
`e2e.m4.provisioning_gate`. Representative exact failures include:

```text
integration.shm: ch.ok()
unit.lifecycle_runner: M0Provider::start failed: code=5 operation=CreateFileMappingW
unit.m2.windows_acceptance: warm_m2_provider_runtime_for_acceptance(...).ok()
integration.m3.windows_vhdx: {"status":"BLOCKED","reason":"administrator_token_required","elevated":false,...}
integration.m3.provider: client.mapping
unit.m4.profile_store: created || error == ERROR_ALREADY_EXISTS
unit.m4.operation_store: created || error == ERROR_ALREADY_EXISTS
integration.m4.stop_forget: created || error == ERROR_ALREADY_EXISTS
integration.m4.windows_worker_recovery: created || error == ERROR_ALREADY_EXISTS
```

Its failures are not a T01 gate because upstream M3 is accepted by the project
owner. No VM, driver, or device commands were run during this baseline sanity
check.
