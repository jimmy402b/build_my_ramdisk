# M4 Guest-Resident Qualification Design

## Purpose

Provide one user-facing PowerShell entry point that runs inside the approved Windows guest and qualifies MyRamDisk v1 against both failure modes the user cares about:

1. the `myrd.exe` process is force-terminated and the VM is then hard-reset; and
2. the VM loses power while `myrd.exe` is still running.

The same workflow also retains the frozen missing-driver negative test. The operator performs exactly three VMware UI Reset or Power Off/On operations. The script never invokes VMware, SSH, SCP, shutdown, or reboot commands.

## Chosen Approach

Use one guest-resident, resumable state machine with two isolated bundle roots:

- `formal\` preserves the existing CreateKill, RecoverShutdown, OfflineCompact, eight-case Negative, and Final evidence contracts.
- `powercut\` adds a realistic live-process power-loss scenario without changing the existing Formal evidence schema.

This is preferred over two independent scripts because there is one ordered entry point and one terminal result. It is preferred over a scheduled-task continuation because explicit operator relaunch is easier to audit and does not introduce boot-order or task-cleanup dependencies.

## User Experience

The operator copies a frozen qualification kit to the guest and runs one command from an elevated Windows PowerShell 5.1 session:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Invoke-M4GuestQualification.ps1 `
  -QualificationRoot E:\myrd-v1-qualification-<id>
```

After each forced reset, the operator runs the identical command again. The script requires the exact `QualificationRoot`; it never discovers or guesses an older run.

`-Status` is read-only. It reports the current state, required next action, completed evidence hashes, and whether a reset is still required. It performs no driver, product, disk, bundle, or state mutation.

## Qualification Kit

The kit contains exact, manifest-hashed copies of:

- `myrd.exe`;
- `Invoke-M4GuestQualification.ps1`;
- `Invoke-M4RunLifecycle.ps1`;
- `Capture-M4Child.ps1`;
- `Prepare-M4NegativeFixtures.ps1`;
- `Invoke-M4Verification.ps1`;
- `myrd_m4_aim_query.exe`; and
- `qualification-kit.json`.

The manifest records the source commit, artifact SHA-256, helper SHA-256 values, schema version, and expected filenames. The controller verifies the complete kit before its first mutation and again after each reboot. Extra or missing kit files fail closed.

After implementation, a new candidate is frozen from the controller branch. The earlier `9b78b36710ea82345ccd381357f309f840b53af8` candidate remains immutable historical evidence and is not silently reused as the new source commit.

## On-Guest Layout

The qualification root must be a previously absent directory on a persistent local NTFS volume:

```text
E:\myrd-v1-qualification-<id>\
  guest-controller-state.json
  events.jsonl
  kit\
  formal\
  powercut\
  evidence\
  logs\
  temp\
```

The RAM disk letters remain `R:` for product runs and `S:` for negative fixtures. Controller state, evidence, kit files, and logs never reside on either RAM disk.

## State Model

The controller state schema is `myrd.m4.guest-qualification.v1`. The valid forward-only states are:

```text
Initial
  -> FormalCreateReady
  -> AwaitingCrashReset
  -> CrashResetObserved
  -> FormalRecovered
  -> AwaitingLivePowerCut
  -> LivePowerCutObserved
  -> LivePowerRecovered
  -> FixturesPrepared
  -> AwaitingMissingDriverReset
  -> MissingDriverResetObserved
  -> MissingDriverCaptured
  -> DriverRestored
  -> NegativeComplete
  -> Complete
```

`Blocked` is terminal. There is no automatic retry and no transition out of `Blocked`.

Every transition records:

- qualification ID and source commit;
- previous and next state;
- UTC timestamp and current boot UTC;
- artifact and helper hashes;
- relevant process identity;
- expected generation and bundle inventory;
- evidence path and evidence SHA-256 values; and
- driver start/state facts when relevant.

The JSON state file is written to a new file, flushed to disk, parsed and hashed back, then atomically renamed over the prior state. A matching append-only JSONL event is flushed before the user is told that power may be removed. If either flush or readback fails, the reset is not armed.

## Common Preflight

Before any first-run mutation, require:

- elevated administrator token;
- Windows PowerShell 5.1;
- exact approved VMware guest identity and MAC address;
- local NTFS qualification root parent;
- a previously absent qualification root;
- exact kit manifest and hashes;
- `MyRamDiskService` Disabled and Stopped with no host process;
- `phdskmnt` DEMAND_START and Running;
- no `myrd.exe`, harness, query, or owned helper processes;
- AIM API reports an empty device list;
- `R:` and `S:` are absent; and
- PD0/PD1 are the fixed GPT disks and PD2 is the fixed RAW test disk, with exact sizes and partition counts.

Any mismatch stops before root creation or product start.

## Scenario 1: Process Crash Followed by Hard Reset

The controller first runs the four existing no-ack gates. Each must return the exact safety block with exit 77 and no mutation.

It then invokes the existing acknowledged CreateKill lifecycle once. CreateKill must:

- reach Ready on `R:`;
- create and verify workload A;
- observe the successor checkpoint;
- force-terminate the exact owned `myrd.exe` PID;
- record the exact nonzero child exit; and
- save immutable CreateKill evidence.

The controller records the current boot UTC, exact evidence hash, residual owned-device facts, and state `AwaitingCrashReset`. Only after the state and event are flushed does it print:

```text
CRASH RESET ARMED
myrd.exe has been force-terminated and the evidence is durable.
Use VMware UI to perform exactly one Reset or Power Off/On now.
After Windows starts, run the same qualification command again.
```

The controller remains idle; it does not initiate or simulate the reset.

On relaunch, the controller requires:

- boot UTC strictly greater than the armed boot UTC;
- Windows System event evidence of an unexpected shutdown after the armed timestamp, including Kernel-Power event 41 and EventLog event 6008;
- old owned PIDs absent;
- CreateKill evidence hash unchanged;
- kit hashes unchanged;
- AIM API empty, `R:`/`S:` absent, and PD0/PD1/PD2 exact.

It then runs RecoverShutdown and OfflineCompact once and verifies their existing evidence contracts and workload hashes.

## Scenario 2: Live Product Power Loss

This scenario uses `powercut\` and never modifies the Formal bundle or Formal evidence.

The controller starts `myrd.exe` against the powercut bundle and keeps an exact process handle. Before arming power loss, it requires:

- Ready on `R:` with NTFS/GPT and the expected physical disk identity;
- workload data written and read back with frozen A and D hashes;
- at least one durable successor checkpoint;
- a captured generation, PID, command line, artifact hash, volume GUID, and complete bundle inventory; and
- `myrd.exe` still alive after the state/event flush and readback.

The controller enters `AwaitingLivePowerCut`, continues monitoring the exact process handle, and prints a periodic message:

```text
LIVE POWER-CUT ARMED
myrd.exe is still running and the durable checkpoint is verified.
You may use VMware UI to Reset or Power Off at any time.
Do not shut down Windows normally.
After Windows starts, run the same qualification command again.
```

If the product exits before power is removed, the run becomes `Blocked`. A normal controller exit, Ctrl+C, or normal Windows shutdown is not accepted as a power-cut pass.

On relaunch, the controller requires a newer boot UTC, the same unexpected-shutdown event evidence, the old PID absent, unchanged kit hashes, unchanged armed bundle identity, an empty AIM baseline, absent `R:`/`S:`, and exact PD0/PD1/PD2.

It then starts recovery against the same powercut bundle, requires a successor generation, verifies the pre-cut workload hashes, writes and verifies a post-recovery workload, performs a graceful stop, and emits `myrd.m4.live-powercut.v1` evidence.

The power-cut evidence records source/artifact identity, armed and recovered boot UTC values, unexpected-shutdown event records, exact PID, armed and recovered generations, pre/post bundle inventories, volume identity, workload hashes, and final clean inventory. A dedicated verifier path validates this evidence; the existing Formal Final schema is unchanged.

## Scenario 3: Missing Driver

After the live power-cut scenario has fully recovered and returned to a clean baseline, the controller prepares the frozen eight negative fixtures and validates their exact manifest and inventories.

It requires AIM empty, `R:`/`S:` absent, exact PD0/PD1/PD2, and no relevant process. It changes `phdskmnt` from DEMAND_START to Disabled exactly once, reads the configuration back, flushes state `AwaitingMissingDriverReset`, and prints the third reset instruction.

On relaunch, it requires a newer boot UTC and unexpected-shutdown events, proves `phdskmnt` is Disabled and not Running, and proves the query helper returns the exact absent-adapter failure. It runs only `missing_driver` and saves the partial evidence.

The controller then restores DEMAND_START, starts `phdskmnt` once, proves AIM empty and the disk/letter baseline clean, activates the remaining seven cases, and runs one final Negative aggregate. Failure to restore the driver is terminal and never triggers another reset.

## Final Qualification

The existing Formal verifier must accept exactly:

- CreateKill;
- RecoverShutdown;
- OfflineCompact;
- eight individual Negative case files; and
- the Negative aggregate.

The live power-cut verifier must separately accept `myrd.m4.live-powercut.v1`. Only when both verifiers pass, all kit hashes remain exact, the driver is restored, AIM is empty, `R:`/`S:` are absent, and PD0/PD1/PD2 remain exact does the controller write `Complete` and print:

```text
M4 v1 QUALIFICATION PASS
```

The qualification root and all evidence are retained. Only exact controller-owned temporary files are removed.

## Relaunch and Error Semantics

- Relaunch at an `Awaiting*Reset` state before boot UTC advances is read-only and prints `RESET STILL REQUIRED`; it does not invalidate or mutate the run.
- Relaunch after the required boot advances performs the matching post-reset gate exactly once.
- Relaunch in a completed intermediate state advances only the next transition.
- A state hash mismatch, illegal transition, corrupt JSON, changed kit, ambiguous process/device identity, unexpected evidence, or failed command writes `Blocked`, emits one concise error, and exits 77.
- Completed product/driver phases are never retried.
- The controller never deletes a Formal or powercut root, never formats or partitions storage, never removes a driver, and never deletes an unknown AIM device.

## Security and Scope

The controller contains no SSH key path, guest credential, VMware credential, `vmrun`, scheduled-task registration, reboot, shutdown, or host-control API. The only VMware operation is the user's manual UI action.

All dangerous operations remain outside scope: no disk formatting beyond the existing product-provisioning contract, no manual partition changes, no driver uninstall, no unknown device cleanup, and no automatic reset.

## Implementation Boundaries

The planned implementation is split by responsibility:

- `tools/Invoke-M4GuestQualification.ps1`: user-facing state machine, pre/post gates, atomic state/event persistence, and interaction.
- `tests/e2e/Invoke-M4LivePowerCut.ps1`: exact child lifecycle, Ready/checkpoint/workload observation, armed monitoring, and recovery evidence generation.
- `tools/Invoke-M4Verification.ps1`: add a separate `PowerCut` verification tier while leaving `Final` behavior unchanged.
- `tests/unit/test-m4-guest-qualification.ps1`: state transitions, flush-before-arm, reset evidence, fail-closed behavior, and static safety contracts.
- `tests/unit/test-m4-live-powercut.ps1`: live child protocol, exact PID ownership, power-cut evidence schema, recovery, and negative cases.
- `tests/CMakeLists.txt`: register bounded Debug/Release local tests.
- release/qualification documentation: explain the three manual UI reset points and exact relaunch command.

The product executable and on-disk bundle format are not changed unless TDD proves an unavoidable product defect. Any such defect requires a separate design amendment rather than being hidden inside controller work.

## Test Strategy

Implementation follows strict RED-GREEN TDD and remains local until the controller and helper contracts pass.

Pure/local tests cover:

- every legal and illegal state transition;
- atomic write, flush, readback, and corrupt-state refusal;
- exact kit inventory and hashes;
- all four no-ack gates;
- exactly three ordered reset arms;
- reset arm occurring only after durable state/event writes;
- unchanged boot producing `RESET STILL REQUIRED` without mutation;
- newer boot plus Kernel-Power 41 and EventLog 6008 acceptance;
- missing or normal-shutdown event evidence rejection;
- live power-cut arm only while the exact product handle is alive;
- premature product exit rejection;
- CreateKill evidence immutability;
- generation and workload-hash recovery rules;
- exact negative fixture and evidence inventories;
- driver disable/restore behavior through injected command results;
- no retry after a completed mutation;
- exact terminal output and exit codes; and
- static absence of SSH, SCP, VMware, password, scheduled-task, reboot/shutdown, format, partition, uninstall, and unknown-device-removal commands.

Debug and Release M4 local suites must remain green. A guest execution is authorized only after the script, helper, verifier tier, documentation, and qualification kit receive independent specification and quality reviews.
