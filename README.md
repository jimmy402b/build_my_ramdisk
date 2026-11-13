# MyRamDisk

**1.0.4 (2026-11-13)** — a single-disk RAM disk for Windows 11 x64. Data lives in RAM and is
checkpointed to a bundle on disk; after a reboot or a power cut the disk comes back from its
last committed checkpoint. 1.0.4 fixes auto-start being consumed inside Windows' shutdown
window; 1.0.3 added **online compaction**. The release is **unsigned** and ships SHA-256 checksums.

## What it is

- `myrd-service` — a LocalSystem service that owns the disk lifecycle;
- `myrd-m5-worker` — a SYSTEM worker that opens an AIM (`phdskmnt`) virtual disk and an NTFS volume;
- `myrd-cli` / `myrd-gui` — clients (the GUI is read-only for non-administrators);
- **one disk at a time**: a second `disk create`/`disk start` is refused with the stable code
  `0x5609`.

## Supported

| | |
|---|---|
| OS | Windows 11 x64 (measured on build 26200; other Win11 builds are not individually verified) |
| Driver | `phdskmnt` **>= 1.2.28.93**, installed and RUNNING. The installer only checks it; it never installs, replaces or downgrades the driver |
| Disk | one disk; NTFS; cluster size 4/8/16/32/64 KiB at create (default 4 KiB) |
| Memory | `--memory-mode automatic` = `min(physical RAM / 4, capacity)`; `fixed` must be <= capacity and <= the machine's commit limit (`0x560A` when it exceeds it) |
| Commands | `disk list/show/create/import/start/stop/checkpoint/compact/export/forget/repair-profile/rebind/config`; `operation show/wait/cancel` |
| Persistence | interval / dirty-threshold / memory-pressure / suspend checkpoints; automatic recovery after a hard power cut |
| Compaction | **online** `disk compact` (and automatic at the profile's checkpoint-count threshold): folds the journal into a new base/journal without stopping the disk, then discards the retired pair so the journal stays bounded |

**Compaction boundary**: the disk stays mounted and writable. A compaction takes a consistent
snapshot, prepares the new artifacts, publishes a new manifest generation and switches the
backing. A crash before that publish leaves the source generation authoritative; a crash after it
is resolved by a read-only inspection. A successful compaction intentionally discards the
**previous** generation pair, so only the latest committed generation is guaranteed afterwards
(there is no rollback to the generation before it). Peak free space is proportional to the durable
source plus the store's dirty bound; a low-space attempt ends `DiskFull`/deferred rather than
publishing partially.

**Not supported**: Windows 10 / Server / ARM64; multiple disks (deferred); volume labels; in-place
version rollback (reinstall only); over-commit beyond the commit limit; BitLocker or SMB bundle
locations (untested); 4Kn media (untested).

## Install and first disk

The release package is `MyRamDisk-<version>-win11-x64.zip`; extract it and install from an
elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File install\Install-MyRamDisk.ps1
& 'C:\Program Files\MyRamDisk\myrd-cli.exe' disk create --name ram --bundle C:\MyRamDisk `
    --size 16GiB --memory-mode automatic --mount Z: --wait
```

Uninstall keeps `C:\ProgramData\MyRamDisk` (profiles, operations, logs) unless `-PurgeData` is
passed; reinstalling is the supported upgrade path. Details:
[`docs/operations/m5-support.md`](docs/operations/m5-support.md).

## Data-loss bound

Only data in a **completed checkpoint** survives. After a power cut the disk returns at the last
committed generation; writes made after that checkpoint are lost — at most one checkpoint
interval, 60 s by default (`--checkpoint-interval`, or `off` for a volatile disk).

## Documentation

- support scope, driver requirement, uninstall and logs: [`docs/operations/m5-support.md`](docs/operations/m5-support.md)
- 1.0.4 release notes, auto-start fix and scoped gate: [`docs/operations/m5-release-1.0.4.md`](docs/operations/m5-release-1.0.4.md)
- 1.0.3 release notes, online-compaction acceptance readings and gate status: [`docs/operations/m5-release-1.0.3.md`](docs/operations/m5-release-1.0.3.md)
- 1.0.1 release notes, acceptance readings and gate results: [`docs/operations/m5-release-1.0.1.md`](docs/operations/m5-release-1.0.1.md)
- the ≥8 GiB / large-journal defect and its fix record: [`docs/operations/m5-known-issue-2026-11-04-large-disk-create.md`](docs/operations/m5-known-issue-2026-11-04-large-disk-create.md)
- version history: [`CHANGELOG.md`](CHANGELOG.md)
- support matrix: [`docs/operations/m5-support-matrix.md`](docs/operations/m5-support-matrix.md)
