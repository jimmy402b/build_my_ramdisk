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
4. **`unit.m4.profile_store` is elevation-gated.** It builds a protected
   ProgramData layout whose intermediate directories carry a SYSTEM +
   Administrators-only DACL; creating a child inside such a directory needs
   `FILE_ADD_SUBDIRECTORY`, which directory ownership does not confer. It passes
   from an elevated shell and fails with `ERROR_ACCESS_DENIED` otherwise — the same
   class as the §1.1 baseline set, and not a defect in the transmitted code.
