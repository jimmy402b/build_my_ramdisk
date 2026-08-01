# MyRamDisk M0 Driver-only PoC

> **M0 is NOT a production RAM disk.** It does not uninstall AIM Toolkit.
> It produces auditable technical evidence for the driver-only approach only.

## Status

M0 has a machine-scoped **GO** verdict on the `rewrite/driver-only` branch.
Tasks 24-29 and the fresh V6 acceptance are recorded in `tasklist.md` and
`docs/validation/m0-driver-only-acceptance.md`.

## Requirements

- Windows 11 x64
- Visual Studio 2022 Build Tools with MSVC x64
- CMake ≥ 3.25
- Ninja ≥ 1.11
- Signed `phdskmnt.sys` driver (version 1.2.28.93) installed
- Administrator elevation for real-driver tests (V3+)

## Build

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
cmake --preset msvc
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
```

## Verification Tiers

| Tier | Requires Admin | Touches Driver | Goal |
|---|---|---|---|
| V0 | No | No | Docs, format, protocol source, forbidden dependency scan |
| V1 | No | No | Compile, unit tests, static ABI |
| V2 | No | No | Local mapping/event/server integration |
| V3 | Yes | Query only | query-driver, no state change |
| V4 | Yes | Yes | Single create/read/write/remove lifecycle |
| V5 | Yes | Yes | Fault injection and cleanup proof |
| V6 | Yes | Yes | 20-cycle final acceptance |

## Exact reproduction

Run non-real tiers in any x64 PowerShell with the MSVC environment loaded:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
.\tools\Invoke-M0Verification.ps1 -Tier V0 -Configuration Debug
.\tools\Invoke-M0Verification.ps1 -Tier V1 -Configuration Debug
.\tools\Invoke-M0Verification.ps1 -Tier V2 -Configuration Debug
.\tools\Invoke-M0Verification.ps1 -Tier V2 -Configuration Release
```

Run real-driver tiers only from an elevated x64 PowerShell, in order:

```powershell
. .\tools\Enter-MsvcEnvironment.ps1
.\tools\Invoke-M0Verification.ps1 -Tier V3 -Configuration Release
.\tools\Invoke-M0Verification.ps1 -Tier V4 -Configuration Release
.\tools\Invoke-M0Verification.ps1 -Tier V5 -Configuration Release
.\tools\Invoke-M0Verification.ps1 -Tier V6 -Configuration Release
```

The E2E harness serializes all real runs, stops `deviosvc` only inside a
`try/finally`, restores its original state and StartMode, and writes reports
below `reports/`. Normal and injected reports are checked by
`tools/Assert-M0RunReport.ps1`; V6 is independently checked by
`tools/Assert-M0V6Aggregate.ps1`, including a new read-only query after service
restoration. Child reports are atomically published and the harness verifies
that their SHA-256 hashes remain unchanged.

Expected evidence documents are under `docs/validation/` and generated JSON is
under `reports/m0-v4*`, `reports/m0-v5*` and `reports/m0-v6*`.

## Authorization warning

M0 GO proves runtime driver-only feasibility on this machine only. It does not
authorize uninstalling AIM Toolkit, deleting `deviosvc`, replacing the signed
driver, or distributing a standalone driver package. Toolkit removal and a
driver-only installation procedure require M5 disposable-VM validation.

## License

This project is licensed under the GNU Affero General Public License v3.0.
See `LICENSE` for the full text.

The fixed AIM protocol reference is used under the terms of the AGPL-3.0
license of Arsenal-Image-Mounter (commit `d379caa027d21902e45fce8c9daf02fd116c2dda`).
