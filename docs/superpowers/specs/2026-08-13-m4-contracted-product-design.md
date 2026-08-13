# MyRamDisk M4 Contracted Product Design

> Status: APPROVED FOR IMPLEMENTATION
> Date: 2026-08-13
> Implementation base: `4f9f912de56160062a007483fc56d5a02d5ff613` (`docs(m3): close persistence milestone`)
> Authoritative companions: `docs/superpowers/plans/2026-08-13-m4-contracted-product.md`, `M4_TASKLIST.md`

## 1. Product decision

M4 is the final product milestone. It is not a continuation of the failed
Service/Host/Worker management platform and it does not create an M5.

The product has one foreground runtime command and one offline maintenance
command in the same executable:

```text
myrd run --bundle <path> --size <N> --drive-letter <R:>
         [--checkpoint-seconds <N>]
         [--checkpoint-dirty-mb <N>]
         [--ram-limit <N>]

myrd compact --bundle <path>
```

`myrd run` creates or recovers one persistent RAM disk, keeps it mounted while
the process runs, periodically persists it, and performs a safe final writeback
when Ctrl+C requests shutdown. `myrd compact` operates only on a stopped bundle
and reuses the accepted offline M3 lifecycle; it never creates an AIM device or
mounts a volume.

The product is deliberately narrow:

- one executable, `myrd.exe`;
- one resident MyRamDisk process;
- one disk per process;
- one human-readable output mode;
- direct use of the accepted M0-M3 storage, proxy, persistence and AIM code;
- no management framework around those components.

## 2. Goals and completion definition

M4 is complete only when all of the following are true on a clean Windows 11
x64 VM with the real AIM driver and NTFS:

1. `myrd run` creates a missing bundle, creates the exact AIM disk, provisions
   one GPT/NTFS volume, assigns the requested drive letter and becomes ready.
2. A file written to the volume survives a committed checkpoint, forced
   termination of the exact `myrd.exe` PID, Windows reboot, and recovery with
   the same command.
3. The recovery path does not call `Initialize-Disk`, `New-Partition` or
   `Format-Volume`.
4. Periodic checkpoints flush the exact volume before invoking M3 checkpoint.
5. v1 performs no online or automatic compaction. It warns once when the journal
   reaches 80% of its 2048 MiB ceiling and provides the supported offline
   `myrd compact --bundle <path>` recovery path.
6. Ctrl+C performs `lock -> flush -> final checkpoint -> dismount -> remove
   drive letter -> remove exact AIM device`, and only then exits 0.
7. The shipped product contains none of the prohibited old-M4 components in
   Section 4.
8. Net new product implementation is at most 2500 physical source lines,
   including product provisioning script lines. The target is 1750 lines.
9. Debug and Release full CTest pass, the M0-M3 formal labels pass, the M4 VM
   lifecycle passes, and product seam/unfinished-marker/boundary scans are clean.

## 3. User-visible contract

### 3.1 Commands

The only accepted product forms are:

```text
myrd --help
myrd run --help
myrd run --bundle <path> --size <N> --drive-letter <R:> [policy options]
myrd compact --help
myrd compact --bundle <path>
```

Every other subcommand or option is a usage error. There is no JSON output,
configuration file, daemon command, runtime management command or cleanup
command. The offline compact form accepts only `--bundle`; it does not expose
M3 barrier, geometry, persistence-limit or store-limit options.
Command names and option names are exact lowercase ASCII and case-sensitive.
Drive letters accept either ASCII case and are normalized to uppercase.

The exact global usage text is:

```text
Usage:
  myrd run --bundle <path> --size <N> --drive-letter <R:> [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]
  myrd compact --bundle <path>
```

`myrd run --help` prints only the `run` usage line with the `Usage:` heading;
`myrd compact --help` prints only the `compact` usage line with that heading.
No-argument invocation exits 2, writes nothing to stdout and writes the global
usage text to stderr.

### 3.2 Defaults

```text
--checkpoint-seconds     600
--checkpoint-dirty-mb    256
--ram-limit              2G
```

### 3.3 Exit codes

| Code | Meaning |
|---:|---|
| 0 | Help completed, offline compaction committed, or Ctrl+C shutdown completed with all cleanup confirmed |
| 2 | Command-line usage error; no bundle, AIM or volume mutation occurred |
| 1 | Every other startup, runtime, persistence, provisioning or cleanup failure |

Errors go to stderr as short actionable text. Normal progress goes to stdout.
No product error-code taxonomy is introduced.

### 3.4 `--bundle`

- The value is a normal filesystem directory path.
- Win32 device paths, `GLOBALROOT`, volume-device paths, NUL and alternate data
  stream syntax are rejected.
- For `myrd run`, a missing directory or empty directory means create.
- For `myrd run`, an existing fixed manifest `disk.rdm` means recover.
- For `myrd compact`, the directory and `disk.rdm` must already exist; missing
  or empty input is rejected without creating anything.
- A non-empty directory without `disk.rdm` is rejected without overwriting it
  by both commands.
- A regular file or other non-directory object is rejected.

The `myrd run` bundle classification is exactly:

```text
manifest exists       -> recover
directory absent/empty -> create
otherwise             -> fail
```

### 3.5 `--size`

- Unsigned integer with binary `K`, `M`, `G` or `T` suffix.
- No decimal, sign, whitespace suffix, overflow or trailing text.
- Range: 64 MiB through 1 TiB.
- New image geometry uses this value.
- Recovered manifest geometry must match it exactly.
- The M3 generation-zero/VHDX image geometry remains logical sector 512 and
  physical sector 4096. The Windows AIM disk must report logical sector 512;
  its `StorageAccessAlignmentProperty.BytesPerPhysicalSector` is accepted only
  when it is 512 or 4096. Every other reported physical-sector value fails
  closed.

### 3.6 `--drive-letter`

- Required; exactly one ASCII letter followed by `:`.
- The letter must be free, or already map to the exact recovered volume.
- A letter owned by any other volume is rejected.
- The product never steals or rewrites another volume's mount point.

### 3.7 Policy bounds

- `checkpoint-seconds`: 5 through 86400.
- `checkpoint-dirty-mb`: 1 through 1048576.
- `ram-limit`: 64 MiB through 1 TiB.
- `ram-limit` is a requested resident-memory upper bound, not a parser
  alignment requirement. The product derives `aligned_hard` as the largest
  storage-block-aligned value no greater than `ram-limit`:
  `floor(ram-limit / kM2BlockSize) * kM2BlockSize`. It derives soft as
  `floor((aligned_hard / 2) / kM2BlockSize) * kM2BlockSize` and the eviction
  low watermark as `floor((aligned_hard / 4) / kM2BlockSize) * kM2BlockSize`.
  Thus the actual M2 hard resident-memory ceiling is the largest valid ceiling
  not exceeding the request. This preserves the accepted M2 invariant
  `soft_limit_bytes < hard_limit_bytes` without rejecting otherwise valid CLI
  K/M/G/T values merely because they are not block-aligned.

The required RAM-policy validity matrix is:

| `--ram-limit` | hard limit | soft limit | eviction low watermark |
|---:|---:|---:|---:|
| 64 MiB | 64 MiB | 32 MiB | 16 MiB |
| 2 GiB (default) | 2 GiB | 1 GiB | 512 MiB |
| 1 TiB | 1 TiB | 512 GiB | 256 GiB |
| 65537 KiB | 64 MiB | 32 MiB | 16 MiB |

## 4. Explicit non-goals and prohibited product content

The new branch must not contain or reintroduce:

- MyRamDisk Windows Service;
- Host or Worker product processes;
- service/worker supervision, reattach or crash recovery;
- named-pipe control plane;
- public IPC frame/client/server stack;
- profile codec, profile registration or profile store;
- operation store or request replay store;
- M4 durable phase, transition, admission, revision or evidence records;
- scheduler interfaces, thread pools, queues or persistent timers;
- runtime management commands such as checkpoint, stop, status or cleanup;
- JSON product configuration or JSON CLI rendering;
- automatic drive-letter selection;
- generation-zero provisioning retry;
- bundle import/export/delete/rename;
- multi-disk coordination or global memory budgets;
- GUI, tray, MSI, Agent Skill or diagnostic export;
- M4 model tests, crash models, fuzz models or randomized soak models;
- adoption or removal of an orphan AIM device without a Windows reboot.

No interface, type, empty directory or compatibility seam may be added for
later use for any item in this list.

The supported offline `myrd compact` command is not a runtime management
command. It is the only v1 bundle-maintenance command and cannot communicate
with, control or coexist with a running `myrd run` process for the same bundle.

## 5. Process and component shape

The steady-state process tree is:

```text
myrd.exe
  -> M3 provider and SHM proxy in process
  -> AIM kernel driver device
```

There is no MyRamDisk child process in steady state.

For first-time provisioning only, `myrd.exe` may launch the system executable:

```text
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe
```

The provisioning script is a fixed source asset embedded in `myrd.exe` as a
Windows resource. At runtime it is written to a newly created per-process
temporary file with restrictive sharing, invoked using `-NoLogo -NoProfile
-NonInteractive -ExecutionPolicy Bypass -File`, and deleted after the bounded
process exits. Only a validated numeric disk number and one validated drive
letter are passed as script parameters. Bundle paths never enter PowerShell.
Stdout and stderr are bounded, the process has a fixed timeout, and the helper
does not participate in checkpoint, compaction or shutdown.

The new source area is intentionally small:

```text
src/run/
  cli.h
  cli.cpp
  windows_volume.h
  windows_volume.cpp
  run_loop.h
  run_loop.cpp
  provision_volume.ps1
  run_resources.rc
  main.cpp
```

Responsibilities:

- `cli.*`: pure parsing, defaults and bounds; no disk or AIM access.
- `windows_volume.*`: concrete Win32 volume discovery, exact extent checks,
  provisioning helper, flush, lock, dismount and drive-letter functions.
- `run_loop.*`: concrete checkpoint, journal-capacity policy, shutdown and the
  one-second loop; no interfaces or queues.
- `main.cpp`: composition, offline compact routing, Ctrl+C flag,
  create/recover startup and reverse-order cleanup.
- `provision_volume.ps1`: fixed GPT/single-partition/NTFS provisioning script.
- `run_resources.rc`: embeds that script; it has no behavior of its own.

Small changes are allowed in:

```text
src/m3/m3_provider.h/.cpp
src/m3/m3_windows_acceptance.h/.cpp
src/m3/m3_aim_server_main.cpp
src/m3/m3_lifecycle_runner.h/.cpp
src/persistence/persistence_types.h
src/persistence/journal_writer.h
src/persistence/journal_writer.cpp
```

They may expose only the product facts and operations that M3 already owns.
They must not introduce a new abstraction layer. The persistence changes are
limited to a distinct journal-capacity error value and the exact writer-limit
return sites needed by Section 10.2.

## 6. Git and history boundary

Implementation starts from the M3 GO commit:

```text
4f9f912de56160062a007483fc56d5a02d5ff613
```

The implementation branch is `feature/m4-contracted-product` in the isolated
worktree `.worktrees/m4-contracted-product`.

The old `feature/m4-task23-restart` branch is historical reference only and is
not in the new branch ancestry. Whole old-M4 commits must not be cherry-picked.
A small Windows detail may be manually re-derived only after a RED test proves
the need; the resulting code must use the concrete functions in Section 5 and
must not bring back old management types.

## 7. Compaction feasibility gate

### 7.1 Codebase fact

At M3 GO there is no online `M3Provider::compact()` method. Compaction is
composed by the offline M3 lifecycle runner using `ImageSession`,
`DynamicBlockStore`, `CompactionCoordinator`, journal preparation, manifest
publication and reconciliation. The online `M3Provider` owns those objects but
does not expose the operation.

### 7.2 R1 question

Before CLI or Windows volume product work, a bounded spike must answer:

> Can the existing M3 compaction algorithm be invoked by the live provider
> through one small concrete method, without copying the algorithm, weakening
> its cut/manifest/reconciliation semantics, adding durable M4 records, or
> changing the public interfaces of the persistence coordinators?

The spike may:

- add one concrete provider/runtime compaction method and one outcome value;
- move a small shared fragment out of the offline lifecycle runner if that
  directly eliminates duplication;
- add deterministic focused tests;
- add no more than about 200 net production lines.

The spike may not:

- copy the compaction coordinator algorithm into `src/run` or another M3 file;
- invent operation/profile revisions;
- introduce a strategy, manager, scheduler or callback hierarchy;
- change the durability meaning of M3 commits;
- turn into a general M3 refactor.

### 7.3 Decision

R1 was NO-GO. The experiment proved that a live-provider implementation would
either copy the M3 manifest publication, commit resolution and reconciliation
algorithm or first extract a shared transaction-finalization path from M3. The
experiment was removed and the final R1 production delta is zero.

The owner approved the following v1 amendment on 2026-08-13:

- remove online and automatic compaction from M4 v1;
- ship `myrd compact --bundle <path>` as a supported offline product command;
- monitor the fixed 2048 MiB journal ceiling during `myrd run`;
- record online compaction as explicit work outside v1 in
  `docs/deferred/online-compaction.md`.

The deferred item is not an M5, does not block M4 v1 GO and must not add a
placeholder interface to v1. No later task may reintroduce online compaction
without a separately approved M3 design and plan.

## 8. M3 product adaptation

### 8.1 Generation-zero creation

The create path calls existing `create_generation_zero_image()` with:

- manifest `disk.rdm`;
- fixed safe initial base and journal names;
- CLI geometry;
- finite production persistence limits;
- random image, VHDX and journal identities produced by `BCryptGenRandom`;
- a finite operation deadline.

The function already closes creation handles and independently reopens the
generation-zero image through M3 recovery. This internal reopen verifies the
newly created manifest and identities; it does not pass through the existing
bundle's `generation >= 1` product gate. Generation zero is expected here. The
create path immediately provisions the volume and performs the first successful
checkpoint, which advances the bundle to generation one.

When the user supplies an already existing bundle whose selected generation is
zero, startup rejects it. v1 does not retry provisioning, delete it or format
it. The error tells the user to verify the path, manually remove the incomplete
bundle and run again.

### 8.2 Product defaults

M3 production-limit construction and store-option construction are moved from
the engineering AIM server into named value-returning functions reusable by
`myrd`, without changing their accepted numeric values.

The fixed M4 product journal ceiling is 2048 MiB of cumulative committed-end
length. This is separate from M3's existing finite per-checkpoint byte and
record work limits. The product provider supplies an optional absolute
committed-end ceiling through `M3ProviderConfig` to `JournalWriter`; `myrd run`
sets it to exactly 2147483648 bytes and existing M3 callers leave it unset.
Crossing that absolute ceiling reports a distinct typed
`JournalCapacityExhausted` persistence error with frozen numeric value 20. The
candidate checkpoint must not advance the manifest or durable committed length.
Per-checkpoint record/byte work, snapshot-memory, batch and host-filesystem
failures retain their existing codes and must not be classified as a full
journal by parsing human error text.

### 8.3 Maintenance stats

The runtime exposes one transient value:

```cpp
struct M3MaintenanceStats final {
    std::uint64_t generation{};
    std::uint64_t dirty_bytes{};
    std::uint64_t journal_committed_bytes{};
};
```

`M3Provider::maintenance_stats()` and the concrete `M3AimRuntime` forwarding
method return `Result<M3MaintenanceStats>`. They read under the provider's
existing operation lock. Dirty bytes are the checked sum of resident-dirty and
zero-dirty blocks multiplied by the fixed storage block size. Journal committed
length comes from the current recovered image identity. Arithmetic or fact
failure is returned instead of truncating. The value is not persisted and is
not evidence or a phase.

### 8.4 Runtime cleanup order

`M3AimRuntime::stop()` continues to remove the exact owned AIM device and wait
for PhysicalDrive inventory to return to its captured baseline before stopping
the provider. M4 must dismount and remove the drive letter before calling it.

## 9. Startup flow

### 9.1 `myrd run` preflight

Before `myrd run` mutation:

1. Require an elevated administrator token.
2. Parse the complete CLI and bounds.
3. Classify the bundle without modifying non-empty unknown directories.
4. Acquire the existing M3 bundle writer lock through provider startup.
5. Verify the AIM driver can be discovered before any Windows disk mutation.
6. Verify the requested drive letter is free or already belongs to the exact
   recovered volume.

These AIM, elevation and drive-letter checks do not apply to `myrd compact`.
The offline command requires normal read/write/delete access to the bundle and
the exclusive M3 writer lock, but it discovers or starts no driver, device,
proxy, disk or volume.

### 9.2 Create path

```text
create generation-zero bundle
-> create and start M3AimRuntime
-> obtain exact AIM DeviceId and new PhysicalDrive number
-> prove size, sector geometry, RAW state, no partition and not boot/system
-> run bounded embedded PowerShell provisioning
-> rediscover one GPT basic-data partition and one NTFS volume on exact disk
-> prove requested drive letter maps to that exact volume
-> FlushFileBuffers(exact volume)
-> checkpoint(WindowsVolumeFlushed)
-> require Committed generation 1
-> enter run loop
```

Provisioning failure does not trigger a second format attempt. Cleanup removes
only the drive letter and exact AIM device owned by the current runtime. The
incomplete generation-zero bundle remains for explicit user removal.

### 9.3 Recover path

```text
recover complete M3 image
-> reject selected generation 0
-> require CLI size == manifest geometry
-> create and start M3AimRuntime
-> locate volumes whose disk extents refer only to the exact PhysicalDrive
-> require exactly one GPT basic-data partition and one NTFS volume
-> assign or confirm the requested drive letter
-> enter run loop
```

The recover path must not execute or contain a fallback to `Initialize-Disk`,
`New-Partition`, `Format-Volume`, partition-table clearing, RAW repair or
generation-zero provisioning.

### 9.4 Minimum anti-misformat checks

Before provisioning, all seven checks must pass:

1. The AIM adapter returned the exact non-sentinel DeviceId created by this
   runtime.
2. AIM `QUERY_DEVICE` reports the expected size, sector size and proxy path.
3. The PhysicalDrive appeared after the captured baseline and maps to that
   DeviceId.
4. Native geometry reports the exact requested size within the accepted device
   alignment semantics.
5. The disk is RAW and contains no partition.
6. Windows does not mark it as boot or system disk.
7. The requested drive letter is not owned by another volume.

If any check is ambiguous or fails, no provisioning command runs.

### 9.5 Offline compaction

`myrd compact --bundle <path>` is valid only after `myrd run` for that bundle
has stopped. It:

1. requires an existing normal directory containing the fixed `disk.rdm`;
2. recovers and validates the newest complete generation and rejects generation
   zero;
3. derives finite product persistence limits from the recovered virtual size
   and uses the fixed product store options for the default 2 GiB hard RAM
   limit (soft 1 GiB, low watermark 512 MiB); the user cannot supply
   engineering limits or `raw-test` options;
4. acquires the existing M3 bundle writer lock and fails without mutation if a
   runtime or another maintenance process owns the bundle;
5. calls the existing offline M3 lifecycle compaction and requires a committed,
   reconciled successor generation;
6. closes every store, session and writer capability before exiting 0.

The command compacts only the last committed durable view. It launches no AIM
runtime, proxy, provisioning script or volume operation, and it cannot preserve
RAM changes that never reached a successful checkpoint. After a forced kill,
the required Windows reboot still precedes offline compaction.

Successful completion prints exactly:

```text
Compacted bundle to generation <N>.
```

If the bundle writer lock is owned, the command does not mutate the bundle and
prints exactly:

```text
Error: bundle is in use. Stop myrd run for this bundle, then retry: myrd compact --bundle "<the same bundle path>"
```

Other failures print one short error that includes the failing bundle path and
the existing M3 diagnostic, then exit 1. The product never tells an ordinary
user to invoke `myrd_m3_e2e` or provide an internal barrier/store option.

## 10. Maintenance loop

The loop has no scheduler abstraction:

```cpp
while (!shutdown_requested.load(std::memory_order_relaxed)) {
    wait_up_to_one_second();
    auto stats = runtime.maintenance_stats();
    if (!stats.ok()) {
        request_failure_shutdown(stats.error());
        continue;
    }
    warn_once_if_journal_high(stats.value());
    if (checkpoint_due(stats.value(), now)) do_checkpoint(runtime, volume);
}
```

A maintenance-stat failure prints one error and requests controlled failure
shutdown. The normal final checkpoint is still attempted unless an earlier
commit outcome already prohibited it; cleanup ultimately exits 1.

### 10.1 Checkpoint trigger

Checkpoint is due when either:

- elapsed time since the last successful checkpoint reaches
  `checkpoint-seconds`; or
- dirty bytes reaches `checkpoint-dirty-mb`.

The action is:

```text
open exact volume GUID
-> verify its disk extents still refer only to runtime PhysicalDrive
-> FlushFileBuffers
-> runtime.checkpoint(WindowsVolumeFlushed)
```

### 10.2 Journal capacity policy

The production ceiling is exactly 2048 MiB. The warning threshold is the first
integer byte at or above 80%, computed without floating point as:

```text
warning_bytes = ceiling_bytes - floor(ceiling_bytes / 5)
              = 1717986919
```

On the first maintenance observation at or above that threshold, `myrd run`
prints one warning for the process lifetime. Its exact form is:

```text
Warning: journal is <current> of 2147483648 bytes. Press Ctrl+C for a clean shutdown, then run: myrd compact --bundle "<the same bundle path>"
```

The warning is not repeated on every one-second poll or checkpoint.

### 10.3 Maintenance error policy

M4 preserves M3 commit meaning but adds no persistent state:

- `Committed`: print the generation, update in-memory last-success time, keep
  running.
- `NotCommitted`: retry once immediately. On a second failure, warn and suppress
  checkpoint attempts for 60 seconds while the disk remains available.
- typed `JournalCapacityExhausted`: do not perform the ordinary immediate retry
  and do not schedule more checkpoints in this process. Print one actionable
  error in this exact form, and keep the mounted disk available until the
  operator requests shutdown:

  ```text
  Error: journal capacity is exhausted; new changes are not durable. Press Ctrl+C, then run: myrd compact --bundle "<the same bundle path>". Restart with the original myrd run command.
  ```
- `CommittedNeedsReconciliation`: invoke the existing provider retry/resume path
  once. If it converges to `Committed`, continue; otherwise request controlled
  shutdown.
- `CommitIndeterminate`: stop new maintenance, request controlled shutdown, do
  not issue a final checkpoint, let next-run M3 recovery select the last complete
  generation, and ultimately exit 1.

Only in-memory timestamps and one shutdown-reason enum are allowed. They are not
serialized and do not form a state machine contract.

After `JournalCapacityExhausted`, Ctrl+C skips the known-impossible final
checkpoint, continues safe dismount/device cleanup, and exits 1. Changes after
the last successful generation may be lost; offline compaction then operates on
that last successful generation.

## 11. Ctrl+C and shutdown

`SetConsoleCtrlHandler` handles Ctrl+C and console-close signals. The handler
does only:

```cpp
shutdown_requested.store(true, std::memory_order_relaxed);
```

It performs no allocation, output, locking or I/O.

### 11.1 Clean sequence

```text
open and FSCTL_LOCK_VOLUME on exact volume
-> verify exact disk extent again
-> FlushFileBuffers
-> final checkpoint unless prior commit is indeterminate
-> FSCTL_DISMOUNT_VOLUME
-> remove only the requested drive-letter mount point
-> M3AimRuntime::stop()
-> confirm exact AIM device absent and PhysicalDrive inventory at baseline
-> exit 0 only if every required step succeeded
```

Final checkpoint `NotCommitted` is retried once. If it still fails, cleanup
continues but the process exits 1. An indeterminate final checkpoint is not
retried; cleanup continues and exit is 1.

### 11.2 Busy volume

If `FSCTL_LOCK_VOLUME` fails because handles remain open:

- do not dismount;
- do not remove the drive letter;
- do not stop the proxy;
- do not remove the AIM device;
- clear the shutdown request and return to the run loop.

The runtime message and README must say:

> Close applications using the RAM disk and press Ctrl+C again. If the volume
> remains busy, temporarily pause or exclude Windows Defender real-time scanning
> and Windows Search indexing for this drive, then retry. Closing the terminal
> or killing `myrd.exe` is an unclean termination and requires a Windows reboot
> before the same bundle is used again.

The product never calls dismount after a failed lock.

### 11.3 Forced termination

A forced process termination can leave the AIM device, PhysicalDrive and mount
alive because `M3AimRuntime::stop()` did not run. v1 therefore requires a
Windows reboot before reusing the same bundle. It does not implement `myrd
cleanup` because the new process has no persisted proof binding an orphan device
to a bundle.

This is a documented product limitation. M4 does not promise a cleanup command,
lease or later identity phase. Any proposal to remove the reboot requirement
would require a new owner-approved scope decision outside this final milestone;
none is planned here.

## 12. Testing strategy

### 12.1 Unit and integration tests

M4 adds only:

1. CLI parsing/default/bound tests.
2. Pure due-policy and commit-outcome tests.
3. Windows volume identity/extent/refusal tests using bounded fakes or temporary
   handles where possible.
4. One concrete composition integration test around create/recover selection and
   cleanup order.
5. One offline product-compaction integration test proving writer-lock refusal,
   committed generation advance and reopen with identical data.

No M4 model, crash-model, randomized soak or fake management composition is
allowed.

### 12.2 Real VM lifecycle

The single product E2E runs serially:

1. Capture AIM device and PhysicalDrive baseline.
2. Ensure bundle is absent and requested drive letter is free.
3. Run `myrd run` and require GPT/NTFS readiness.
4. Write file A and record SHA-256.
5. Wait for a committed checkpoint.
6. Write file C that is not promised durable.
7. Force-kill only the exact `myrd.exe` PID.
8. Reboot Windows and require no residual AIM disk.
9. Run the same command and require the recover path.
10. Prove provisioning commands were not executed.
11. Verify file A; do not require C.
12. Write file D.
13. Close test handles and send Ctrl+C.
14. Require clean shutdown order, exit 0 and inventory baseline.
15. Run `myrd compact --bundle <path>`, require one committed successor and no
    AIM/PhysicalDrive inventory change.
16. Run a third time, verify file D, Ctrl+C again and require baseline.

Minimum negative VM checks are:

- unknown non-empty directory is untouched;
- recover size mismatch fails;
- occupied drive letter is not stolen;
- generation-zero existing bundle is not reprovisioned;
- recovery volume ambiguity is not formatted;
- second process is rejected by the M3 writer lock;
- missing AIM driver fails before disk mutation;
- busy volume is not dismounted or removed and can exit after the holder closes.
- offline compact is rejected while `myrd run` owns the bundle.

## 13. Code budget and architecture scans

Physical source lines are counted relative to M3 GO for:

- `src/run/**` including `provision_volume.ps1`;
- net additions in every allowed M3/persistence product-adaptation file;
- any additional product CMake/resource logic that contains runtime behavior.

Tests, documentation and declarative CMake target lists are excluded. Generated
code, macros, long single lines or placing product behavior in tests may not be
used to evade the count.

| Area | Target | Maximum |
|---|---:|---:|
| CLI/defaults | 250 | 350 |
| M3 create/stats/offline compact adaptation | 300 | 450 |
| Windows provisioning and volume identity | 650 | 900 |
| Flush/mount/dismount | 250 | 350 |
| Run loop/shutdown/main | 300 | 450 |
| **Total** | **1750** | **2500** |

If the projected total exceeds 2500, implementation stops and identifies the
specific RED that caused growth. The supported offline journal-recovery path,
anti-misformat checks, exact-volume flush and locked shutdown are not removed to
satisfy the budget.

Product builds are scanned for the prohibited names and representative strings
from Section 4. No test seam may appear in `BUILD_TESTING=OFF` objects or
`myrd.exe`.

## 14. Implementation order and gates

The only allowed order is:

```text
R0 clean boundary
-> R1 online compaction NO-GO plus owner-approved v1 amendment
-> R2 CLI
-> R3 M3 product adaptation
-> R4 Windows volume create/recover
-> R5 maintenance loop
-> R6 Ctrl+C shutdown
-> R7 real VM lifecycle
-> R8 regression, documentation and final GO
```

R1 is resolved by the owner-approved amendment in Section 7.3. Every other task
requires its predecessor to be DONE. No later task may be implemented because
it appears easier.

## 15. Stop conditions

Stop immediately if a proposed change:

- introduces service, host, worker, IPC, profile or operation-store code;
- introduces a durable M4 phase, transition, evidence or revision;
- adds an interface for a single concrete call;
- copies checkpoint or compaction durability algorithms out of M3;
- formats any recovery-path disk;
- dismounts a volume after lock failure;
- adopts or deletes an orphan AIM device after forced termination;
- raises the product budget above 2500 lines;
- weakens an accepted M0-M3 protocol, storage or persistence contract;
- requires an M5 to make the single-disk product usable.

The correct response is to preserve the RED evidence, mark the current task
BLOCKED and request a scoped product decision. Difficulty is not permission to
expand the architecture.
