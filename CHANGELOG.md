# Changelog

All notable changes to MyRamDisk are recorded here. The project follows
[Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

**This file is part of the release contract (R-07).** Every shipped version names the
commits its behaviour came from, so "what is in 1.0.0" can be answered without reading the

## Release status (2026-11-02)

**R-19 passed; R-01 … R-19 are all PASSED.** The release was rehearsed from one build
(`41ed3de`, followed only by documentation commits up to `26c0578`):

| Run | Cases | Result |
|---|---|---|
| A1 (main suite) | 158 | 158/158 passed |
| A2 (main suite, rerun) | 158 | 158/158 passed |
| B (performance, cluster size, compatibility, soak, support bundle) | 5 | 5/5 passed |
| C (fault matrix, one case per clean reset) | 6 | 6/6 passed |

Frozen evidence: `out/m5-evidence/release-manifest.json`, `frozen_at=2026-11-02T10:21:08Z`,
four binaries and nine readings; `tools/Verify-M5Evidence.ps1` answers **accepted**. Hashes:

```text
myrd-service.exe    69d9166aa9a2a325b5120f7f11f92942751948a2b01b67acc6156642a4adcc73
myrd-cli.exe        a00678e1318e4080ffcd451a5a7b765188b9abd2178ed340dc0dd93cdff40c35
myrd-gui.exe        f54a12fcc5cad4726b0b0bae76647860c36d455eaff583968fef0eb2377ab3cb
myrd-m5-worker.exe  7b689347e79f30afbe8a9fb33b0528d05b85948c308b4fddac0e3d8b3c63fb69
```

Known limits recorded with the release: the soak ran 19 h 14 min (the tasklist asks for 24 h;
the user closed it early), the service working set drifts up 1–2 MB within one service lifetime
and returns to baseline on restart, `integration.m5.startup_sweep` and `unit.m3.vhdx_reader` are
intermittent (green on their own and on full reruns), and `integration.m5.bundle_corruption`
must run on a cleanly reset guest or it hangs. See `docs/operations/m5-release-rehearsal.md` §9.

whole history. Dates are the dates the change landed on `feature/m5-lifecycle`.

## [1.0.3] - 2026-11-06

### Added

- **Online compaction.** `disk compact` and the automatic checkpoint-count trigger now
  actually fold the journal into a new base/journal on the running disk, publish a new
  manifest generation, switch the backing and delete the retired generation pair. The
  worker gained `compaction_prepare/commit/abort/inspect_compaction`; the service realizes
  the durable mutation-boundary/cut/commit/epoch phases and the read-only recovery
  inspection.
- Guest acceptance in `integration.m5.compaction_{manual,auto,churn,regression,kill}`:
  2.26 GB journal -> 12 KB on a 16 GiB disk, 213 generations / 6 automatic compactions,
  3x10 GiB churn, the 1.0.2 regression, and a kill after the compaction publish.
- B 5/5 and fault 6/6 gates with a fresh guest per case.

### Status

Published 2026-11-06 as tag `v1.0.3`. A1/A2, B 5/5, fault 6/6, compaction 5/5
and powerloss 2/2 are green on the guest; two `--clean-first` builds produced
identical binary hashes. The manifest is `frozen_at=2026-11-06T08:45:27Z`
(22 readings), `Verify-M5Evidence.ps1` answers **accepted**, and the package
`out/release/MyRamDisk-1.0.3-win11-x64` has a 10-entry `SHA256SUMS` with 0
mismatch. See `docs/operations/m5-release-1.0.3.md` §4 and §6.

## [1.0.2] - 2026-11-05

Patch release for a checkpoint-resilience defect found by the host write/delete/write test:
write 10 GiB, delete it before the next checkpoint, write 1 GiB, then checkpoint.

### Fixed

- **An aborted checkpoint left an uncommitted journal tail, and the disk could never
  checkpoint again.**  The journal writer appends records while it cuts the snapshot, so a
  checkpoint that aborts part-way (measured: `SnapshotResourceExhausted`, "snapshot was
  aborted", after a 10 GiB delete's discard backlog) had already grown the journal by
  490 MB.  No manifest named those bytes, but the next open compared the whole Windows
  file identity (size included) against the last committed generation, so every retry
  failed with `opened bundle file identity differs from the expected identity`; after a
  restart it reached publish and failed with `reopened journal identity changed before
  publish`, leaving the profile `blocked / needs-attention / commit_indeterminate`.  The
  writes accepted before the abort were only in RAM and were lost on restart (measured:
  the 1 GiB file was gone after a service restart).  A failed preparation now truncates
  the journal back to the last committed end, an aborted prepared transaction does the
  same, and a checkpoint that opens a journal longer than its committed length trims the
  tail before writing.

### Measured after the fix (host, 16 GiB / automatic 15.92 GiB budget)

- write 10 GiB write-through 14 s; delete instant; write 1 GiB 2 s;
- the automatic checkpoint aborted twice with `SnapshotResourceExhausted` (the delete's
  discard backlog), then committed generation 3 (`post_commit_dirty=1`), and interval
  checkpoints reached `Idle/Clean` at generation 5;
- journal 1,117,143,040 → 2,944,442,368 B, committed;
- `disk stop` + `disk start` (new worker, recovered from the bundle) → generation 6,
  `Z:\one.bin` 1,073,741,824 B and SHA-256
  `DD11089A99D4DEDAE1CABF0E8D55229ECBC9DECA02888BFE4DDAF9640E7221B9` unchanged, the
  10 GiB file absent.

The snapshot auto-abort itself is still a retry, not a success: a large delete can cost
one or two aborted checkpoint attempts before the discard backlog drains.  The checkpoint
now recovers from them instead of wedging.

## [1.0.1] - 2026-11-04

Patch release for the large-disk defects found in real use on 1.0.0. **No behaviour change
for disks that already worked**; the fixes are on the ≥8 GiB create path and on checkpoints
whose dirty set is larger than 2 GiB.

### Fixed

- **≥8 GiB `disk create` deadlocked** (`7a1d4d1`). Three independent causes, each with a
  guest reading: the automatic budget was not block-aligned (`M3Provider::open provider
  limits are invalid`); a create's generation-zero checkpoint was bounded by the ordinary
  120 s worker command even though its cost grows with the disk (`4107
  WorkerPipeChannel::read`); and the service's `operation wait` ceiling collided with the
  CLI's own 30 s exchange budget. Measured after the fix: 8 GiB automatic 125 s, 16 GiB
  fixed:12GiB 314 s, 16 GiB automatic 327 s, all `disk create --wait` exit 0.
- **A checkpoint that committed was still classified dirty** (`disposition=committed`
  fourteen times with generation 2→15, `persistence=Dirty`, maintenance checkpointing
  forever, 9,112,297,472 B journal). Cleanliness is now the worker's block-level answer
  (`post_commit_dirty`), not a comparison of a per-process epoch the worker never reports.
- **The M4 recovery adapter read every bundle with a fixed 2 GiB journal bound**, so a
  committed 9.1 GiB journal made the next `disk start` refuse its own bundle
  (`no fully valid manifest generation exists`). Every reader now shares
  `recovery_persistence_limits()`: twice the dirty set plus overhead, unbudgeted profiles
  unchanged.
- **A power cut inside a start's `Starting/Recovering` window wedged the profile.** The
  startup sweep now retires `starting` as well as `running`, and an interrupted
  `disk.start` record is terminalized with its operation gate released; before that, every
  later start was refused with `0x5605` while `disk show` still said `health=Healthy`.

### Guest acceptance (16 GiB, fixed 12 GiB budget, 7 GiB write-through)

- create exit 0, volume 17,162,039,296 B NTFS, checkpoint generation 2;
- write 12 s, 7,516,192,768 B, SHA-256 `AF76B0B8…E528554`;
- checkpoints generation 3 (`post_commit_dirty=1`, writes landed after the cut) → 4
  (`post_commit_dirty=0`, clean) → 5 (`Idle`/`Clean`), journal 9,112,285,184 B committed;
- hard power cut, next boot: interrupted start repaired, `disk start` succeeded, file
  length and digest matched, second disk still refused with `0x5609`.

Full gate rerun, frozen manifest and the packaged hashes are recorded in
`docs/operations/m5-release-1.0.1.md`.

## [1.0.0] - 2026-10-30 (release candidate)

First release. **Supported platform: Windows 11 only** (Win10 / Server are not supported).
**Unsigned**: there is no code-signing certificate, so Windows SmartScreen may warn; the
release ships SHA-256 checksums and these notes state the consequence. **No version
rollback**: reinstalling is the supported way back; the installer and the docs say so.

### Single-disk lifecycle (M5)

- One disk at a time: a second `disk create` / `disk start` is refused at admission with the
  stable code `0x5609` (`single_disk_violation`). (`2a1f125`, evidence
  `docs/validation/m5-stab-01-single-disk.md`)
- Create / start / stop / checkpoint / compact / forget, Load Only (`--no-start` + `disk
  start`), volatile-disk loss protection with `--accept-volatile-data-loss`, single-file
  VHDX export, and automatic recovery after a hard power cut.
- `disk show` reports the worker's own memory gauges (`mem=resident/budget`) and the
  durable compaction counters.

### Memory budget (M5 §7, decision 1)

- A fixed `--memory-hard-limit` above the machine's commit limit is **refused at create
  admission** with `0x560A` and the message "M5 does not over-commit"; nothing is left
  behind (no operation, reservation, bundle or worker). (`c8b3d63`, evidence
  `docs/validation/m5-stab-02b-memory-limits.md`)
- The store's `hard_limit_rejections` count travels to `disk show` as
  `budget_rejections=`, so a failed write can be attributed to the budget locally.
- The budget-exhaustion experiment is a repeatable gate step:
  `tools/m5vm/Invoke-M5FullSuite.ps1 -Only memory`.

### Stability work that landed with 1.0.0

- One-command full suite over the guest VM (`tools/m5vm/Invoke-M5FullSuite.ps1`): grouped,
  reset between groups, machine-readable `results.json`, two consecutive runs compared case
  by case. (`3a13308`, evidence `docs/validation/m5-stab-04-full-suite.md`)
- Recovery from a checkpoint interrupted between P4 (commit durable) and P5 (epoch
  published) no longer leaves the operation `running` forever: the epoch is published from
  the recovery inspection's proof. (`6393a89`, evidence
  `docs/validation/m5-stab-03c-process-kills.md`)

### Release engineering, support and newer scope (R-09 … R-19)

- **Security audit (R-09)**: seven blocking findings fixed -- the worker and the client verify
  the private pipe's server identity, the profile store checks the directory owner, `disk
  export` is administrator-only, the log directory is management-only, the job handle asks only
  for `JOB_OBJECT_QUERY`, the launch spec is read to EOF with a 128 KiB cap, and the client
  verifies the public pipe's server. (`6f93816`, evidence
  `docs/validation/m5-stab-07-security-audit.md`)
- **Reproducible builds (R-10)**: `/Brepro` on compile and link; two clean builds of the same
  commit produce byte-identical binaries. (`0ff3b03`)
- **Non-elevated read path (R-11)**: `disk show` is caller-filtered and the service compares the
  profile's owner with the caller, so a user can read its own disk without elevation while
  another user's profile is refused. (`edd562b`)
- **Cluster size (R-17a)**: `disk create --cluster-size` accepts 4/8/16/32/64 KiB as powers of
  two, create-only; absent keeps the Windows default. (`bfdaff1`, evidence
  `docs/validation/m5-stab-15-cluster-size.md`)
- **Support bundle (R-15)**: `tools/New-M5SupportBundle.ps1` writes one redacted archive with
  versions, OS, driver, service state, disks, recent operations, profile manifests and log
  tails, plus a `failure.json` naming the most recent failed operation. (`3bdd01e`, evidence
  `docs/validation/m5-stab-13-support-bundle.md`)
- **One support page (R-16)**: `docs/operations/m5-support.md` states the supported scope, the
  known limits (each with a test or an explicit "not measured"), the data-loss bound,
  backup/export, the driver requirement and uninstall notes; installer refusals and the
  product's `driver_unavailable` message point at it. (`73efbc4`)
- **Compatibility (R-18)**: a compressed bundle directory, an EFS-encrypted one, Defender with
  real-time protection on, and a non-system AIM disk with 512/512 sectors all measured with a
  full create/write/checkpoint/stop cycle. (`310247d`, evidence
  `docs/validation/m5-stab-16-compatibility.md`)
- **Sustained operation (R-13)**: `integration.m5.soak` plus `tools/m5vm/Test-M5Soak.ps1` run a
  24 h cycle with periodic hard power cuts, measuring handles, worker count, working set and the
  committed-data hash every round. (`b51973d`, `a8c1da5`, `fc489ec`)
- **Non-mirror test layout (R-17b)**: configure-time test paths are rebased at run time through
  `MYRD_TEST_ROOT`, so the suite runs from a checkout that is not at the dev path. (`2646539`)
- **Release rehearsal (R-19)**: `docs/operations/m5-release-rehearsal.md` is the runbook for one
  commit, a frozen manifest, a clean VM, two consecutive full runs and the eight release gates.
  (`db90b4c`)
### Known limits in 1.0.0

- Volume labels are **not supported**: AIM volumes do not complete `SetVolumeLabel`; the
  entry point refuses and explains (decision 3).  The refusal names the option:
  `--volume-label is not supported: this release does not set volume labels` (the short
  `--label` is refused with the same message).
- Cluster size is a **create-only** choice (4 KiB..64 KiB); a formatted volume keeps the size
  it was created with and there is no re-format command.
- One disk at a time (G4 multi-disk is deferred).
- Physical RAM exhaustion is **not** a supported path: budgets above the machine's commit
  limit are refused at creation (decision 1).
- Upgrades are supported by reinstall only; there is no downgrade or rollback path in the
  code (decision 5).
- The driver (`phdskmnt`) is checked, never installed or replaced.

## Commit map

| Area | Commit |
|---|---|
| Single-disk admission guard | `2a1f125` |
| Memory budget enforced, >64 MiB disks creatable | `fcd7da1`, `1dbc476` |
| One-command full suite | `46308ec`, `caa92b3`, `369e38d`, `3a13308` |
| Six release decisions (docs) | `ae915d0` |
| Over-commit refusal + budget rejections (R-01) | `c8b3d63` |
| Deterministic P4 window (R-04 apparatus) | `b12b449` |
| P5 from the recovery inspection's proof | `6393a89` |
| Security audit, seven blocking findings (R-09) | `6f93816` |
| Reproducible builds with /Brepro (R-10) | `0ff3b03` |
| Volume-label refusal by name (R-14) | `cf976f8` |
| Create-only cluster size (R-17a) | `bfdaff1` |
| Support bundle (R-15) | `3bdd01e` |
| One support page and error pointers (R-16) | `73efbc4` |
| Compatibility measurements (R-18) | `310247d` |
| Non-elevated `disk show` with owner check (R-11) | `edd562b` |
| 24 h soak apparatus (R-13) | `b51973d`, `fc489ec` |
| Non-mirror test layout (R-17b) | `2646539` |
| Release rehearsal runbook (R-19) | `db90b4c` |

`[1.0.0]` is a release candidate until the R-19 rehearsal completes; the manifest and the
checksums are produced by `tools/New-M5ReleaseManifest.ps1` and checked by
`tools/Verify-M5Evidence.ps1`.
