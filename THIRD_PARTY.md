# Third-Party Notices and Tool Inventory

This inventory distinguishes source/protocol references from runtime
prerequisites. Unless a section explicitly says otherwise, third-party source
and binaries are not vendored or redistributed by this repository.

## Arsenal Image Mounter protocol reference

- **Upstream repository:** `https://github.com/ArsenalRecon/Arsenal-Image-Mounter`
- **Fixed reference commit:**
  `d379caa027d21902e45fce8c9daf02fd116c2dda`
- **Upstream notice:** the pinned `LICENSE.md` describes dual licensing. The
  listed Arsenal source files state that the source/API is available under GNU
  Affero General Public License v3 with proprietary exceptions available from
  Arsenal Recon.
- **Local compatibility:** MyRamDisk is distributed under AGPL-3.0. No AIM
  source file is compiled or vendored. Local ABI declarations and protocol
  behavior are an interoperability reimplementation tied to the audited
  contract in `docs/protocol/aim-m0-contract.md`.

Normative reference files:

- `Unmanaged Source/phdskmnt/inc/common.h` — Arsenal Recon, AGPL-3.0/open-source
  terms stated in the upstream file;
- `Unmanaged Source/phdskmnt/proxy.cpp` — Arsenal Recon, AGPL-3.0/open-source
  terms stated in the upstream file;
- `Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs`
  — Arsenal Recon, AGPL-3.0/open-source terms stated in the upstream file;
- `Unmanaged Source/phdskmnt/inc/imdproxy.h` — ImDisk Proxy Services,
  Copyright 2005–2007 Olof Lagerkvist, MIT-style permission notice reproduced
  in that upstream file.

Local files `src/driver/aim_protocol.h` and
`src/proxy/imdproxy_protocol.h` contain the audited constants, packed wire
layouts and independent safe codecs. `tools/Fetch-AimReference.ps1` fetches the
pinned repository only into ignored build output for comparison; it does not
copy upstream files into the source tree.

## Installed `phdskmnt.sys` runtime prerequisite

M0–M3 real-host tests interoperate with an already installed, signed
`phdskmnt.sys`. The accepted machine used version `1.2.28.93`, SHA-256
`3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9`.

The driver binary and AIM Toolkit are not part of this repository, build or
release artifact. This project does not claim a right to redistribute the
driver, `aimapi.dll`, `aim_ll.exe`, `RamDyn.exe`, `deviosvc` or any independent
AIM installer. A user must obtain and install any required signed driver under
the upstream supplier's terms. M3 acceptance does not authorize uninstalling
the Toolkit or replacing the installed driver.

## Microsoft specifications, SDK and system components

- The strict VHDX profile and project-authored golden fixtures are derived from
  Microsoft Open Specification **[MS-VHDX]**, reviewed at published revision
  `v20240423`. No Microsoft specification text, sample source or binary fixture
  is vendored.
- Native compatibility tests call the documented Windows Virtual Disk API
  (`OpenVirtualDisk`, `AttachVirtualDisk`, `DetachVirtualDisk`) through the
  operating-system `virtdisk.dll`; that DLL is not redistributed.
- Product and test binaries use Windows SDK headers and system libraries,
  including `bcrypt`, `wintrust`, `crypt32`, `version`, `setupapi`,
  `cfgmgr32`, `virtdisk` and standard Win32/NT APIs. These are operating-system
  or toolchain components and are not copied into the repository or release.
- SHA-256 uses Windows CNG/BCrypt. The project does not include or link OpenSSL.

## Build and test tools

These externally installed tools are not linked into or redistributed with the
product:

| Tool | Accepted baseline/current evidence | Role |
|---|---|---|
| Visual Studio 2022 Build Tools / MSVC x64 | toolset 14.44.35207, Windows SDK 10.0.26100.0 | compiler, linker and SDK |
| CMake | required >= 3.25; final Task44 host 4.2.2 | configure/build orchestration |
| Ninja | required >= 1.11; final Task44 host 1.13.2 | build executor |
| Windows PowerShell | 5.1; final Task44 host 5.1.26100.8875 | verification harness |
| Git for Windows | final Task44 host 2.53.0.windows.1 | source/evidence provenance |
| VMware Workstation | 17.6.4 build-24832109 | isolated Windows 11 power-cut testing |
| OpenSSH Server/Client | Windows client 9.5p2; guest Windows optional capability | host-to-guest test transport |

The repository uses a project-authored minimal C++ test framework in
`tests/support/test_framework.h`; it does not vendor Catch2, GoogleTest, Boost, fmt,
zlib, libewf or another third-party C++ library. PowerShell JSON handling uses
the system runtime.

## Project-authored fixtures and generated evidence

Files under `tests/fixtures/m3/` are project-authored JSON, Markdown and
hexadecimal golden data. Format fixtures were independently generated from the
frozen project format or public MS-VHDX field definitions; they do not contain
an upstream VHDX image or AIM binary. Mutable VHDX, NTFS, crash and soak evidence
is generated outside the tracked repository and is referenced by SHA-256.

No tracked `.sys`, `.dll`, `.exe`, `.lib`, `.vhdx`, archive or third-party
binary is present in the repository.
