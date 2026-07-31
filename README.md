# MyRamDisk M0 Driver-only PoC

> **M0 is NOT a production RAM disk.** It does not uninstall AIM Toolkit.
> It produces auditable technical evidence for the driver-only approach only.

## Status

M0 is a milestone within the `rewrite/driver-only` branch.
Execution progress is tracked in `tasklist.md`.

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

Run with: `.\tools\Invoke-M0Verification.ps1 -Tier <V0-V6> -Configuration <Debug|Release>`

## License

This project is licensed under the GNU Affero General Public License v3.0.
See `LICENSE` for the full text.

The fixed AIM protocol reference is used under the terms of the AGPL-3.0
license of Arsenal-Image-Mounter (commit `d379caa027d21902e45fce8c9daf02fd116c2dda`).
