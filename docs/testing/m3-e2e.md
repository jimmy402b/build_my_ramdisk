# M3 E2E harness

Task 40 provides the safety boundary for later real-host tests. It does not
format, partition, mount, attach, delete, or detach a disk. Those destructive
operations belong to Task 41 and require a separately approved scratch-disk
identity.

## Tiers

`tools/Invoke-M3Verification.ps1` exposes five explicit routes:

- `Local`: runs the configured CTest regression.
- `Preflight`: performs the read-only host/artifact/path/identity probe.
- `RealFunctional`: currently `BLOCKED` with exit code 77 until Task 41.
- `Crash`: currently `BLOCKED` with exit code 77 until Task 42.
- `Soak`: currently `BLOCKED` with exit code 77 until Task 43.

Required `SKIP` or an unavailable later tier is never converted into PASS.

## Preflight example

The evidence directory, scratch directory, and disk identity JSON must be
explicit. The identity JSON is not a disk-number hint; it must contain the
exact `Number`, `SizeBytes`, `BusType`, `UniqueId`, `SerialNumber`, and
`DeviceId`, with all boot/system/page-file/cluster flags false when present.
A non-`WhatIf` preflight also reads the host's `Get-Disk` and
`Get-PhysicalDisk` inventory and requires exactly one complete identity match;
a syntactically valid but nonexistent identity is `BLOCKED`.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  tools/Invoke-M3Verification.ps1 `
  -Tier Preflight -Configuration Release `
  -EvidenceBase C:\myrd-evidence `
  -Executable .\out\build\msvc\src\Release\myrd_m3_e2e.exe `
  -ScratchRoot C:\myrd-scratch `
  -ExpectedDiskIdentityJson C:\myrd-evidence\scratch-disk.json `
  -WhatIf
```

`-WhatIf` permits non-administrator dry-run operation and guarantees that no
format/mount/delete command is emitted. It may write JSON and summary files to
the explicitly supplied evidence directory. A real preflight requires
administrator access and only performs read-only probes.

The harness rejects wildcard paths, volume roots, the repository/workspace
root, reparse-point paths, paths outside the declared scratch root, Disk 0,
automatic disk selection, and non-unique or mismatched disk identities. Every
future cleanup action is represented in an idempotent ledger with an exact
recovery command; cleanup never derives a destructive target from a glob.

## Evidence

Preflight writes `preflight.json` and `preflight.summary.txt`. The JSON contains
the schema, route, source commit when available, executable hash, host probe,
static safety checks, explicit identity, terminal verdict, and cleanup state.
`PASS` means only that the preflight contract passed. It is not M3-V5/M3-V6
functional acceptance.
