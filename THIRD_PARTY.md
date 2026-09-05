# Third-Party Notices and Provenance Inventory

This file records the source, license status, and redistribution boundary of
the declarations, specifications, runtime prerequisites, build tools, and test
fixtures used through M3. It is an engineering compliance inventory, not legal
advice.

Unless a section explicitly says otherwise, third-party source and binaries
are neither vendored nor redistributed by this repository. The repository's
`LICENSE` contains the GNU Affero General Public License version 3 text. The
M0 project plan freezes the project license as `AGPL-3.0-only`; this inventory
therefore uses that SPDX expression and does not add an `or later` grant. The
project copyright notice is recorded in `NOTICE` as:
`Copyright (C) 2026 jimmy402b and contributors`.

## Arsenal Image Mounter and ImDisk declaration provenance

The protocol reference is the Arsenal Image Mounter repository at pinned
commit `d379caa027d21902e45fce8c9daf02fd116c2dda`. That source reports upstream
driver/API version **1.2.29.95**. Repository:
<https://github.com/ArsenalRecon/Arsenal-Image-Mounter>.

The local files adapt published ABI/protocol declarations. They are not a
clean-room claim and are not copies of an upstream implementation body:

| Local material | Pinned upstream declaration/reference | Upstream notice | Local change and use |
|---|---|---|---|
| `src/driver/aim_protocol.h` | `Unmanaged Source/phdskmnt/inc/common.h` at `d379caa...` | Arsenal Recon; the upstream file states AGPL-3.0/open-source terms, while the repository also describes proprietary licensing options | Required constants and packed ABI layouts were adapted to namespaced C++20 fixed-width declarations, with local static assertions and project-specific constants. |
| `src/proxy/imdproxy_protocol.h` | `Unmanaged Source/phdskmnt/inc/imdproxy.h` at `d379caa...` | Copyright (C) 2005-2007 Olof Lagerkvist; MIT-style notice reproduced below | The supported request constants and wire layouts were adapted to namespaced C++20 declarations. Safe read/write helpers and validation are independently authored. |
| Local AIM/IMDPROXY behavior | `proxy.cpp` and `Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs` at `d379caa...` | Arsenal Recon; AGPL-3.0/open-source terms stated in those files | Used as behavioral interoperability references. No function or service implementation body is vendored or compiled. |

`tools/Fetch-AimReference.ps1` fetches the pinned files only into ignored build
output for comparison. No AIM user-mode binary (`aimapi.dll`, `aim_ll.exe`,
`RamDyn.exe`, or `deviosvc`) is linked, loaded from the project, or
redistributed.

### Complete `imdproxy.h` MIT-style notice

The following notice is reproduced from the pinned `imdproxy.h` because local
protocol declarations are adapted from that header:

```text
ImDisk Proxy Services.

Copyright (C) 2005-2007 Olof Lagerkvist.

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

## Installed `phdskmnt.sys` prerequisite

The accepted runtime driver is deliberately tracked separately from the newer
protocol reference:

| Item | Accepted provenance |
|---|---|
| AIM source/package commit | `cb7a2347e9595b12e2bd178ce803090a7a674037` |
| Package member | `DriverSetup/DriverFiles.zip` |
| Package SHA-256 | `B889A4BAAE15CE4F6DDD94327BCC56C9B1D0FC442DDB3E0A1FD233F8D12BD324` |
| Driver member | `Win10/x64/phdskmnt.sys` |
| Driver SHA-256 | `3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9` |
| Accepted file/product version | `1.2.28.93` |

M0-M3 real-host tests interoperate with this already installed, signed driver.
The driver archive did not provide the project with an unambiguous independent
binary redistribution grant. Therefore `phdskmnt.sys`, the archive, AIM Toolkit,
and AIM user-mode binaries are prerequisites obtained and installed by the user
under upstream terms; they are not part of this repository, build, or release.
M3 acceptance does not authorize uninstalling the Toolkit or replacing the
accepted driver.

## Microsoft specifications, SDK, APIs, and runtime components

### MS-VHDX and Virtual Disk API

The strict VHDX profile and project-authored golden layout are derived from
Microsoft Open Specification **[MS-VHDX] revision 8.0, published
2024-04-23**: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/>.
The specification remains Microsoft copyrighted material; its patent/IP status
is described by the **Intellectual Property Rights Notice for Open
Specifications Documentation** on the Microsoft specification page:
<https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/83e061f8-f6e2-4de1-91bd-5d518a43d477>.
The project uses public format facts only. It does not vendor Microsoft
specification text, sample source, or a Microsoft binary VHDX image.

Native compatibility tests dynamically load the Windows-provided
`virtdisk.dll` and resolve all six functions below. They are test-only dynamic
dependencies, not normal project import-library links:

- `OpenVirtualDisk`
- `AttachVirtualDisk`
- `DetachVirtualDisk`
- `GetVirtualDiskPhysicalPath`
- `GetVirtualDiskInformation`
- `CreateVirtualDisk`

`virtdisk.dll` is supplied by Windows and must not be bundled as a project
binary.

### Windows SDK and system API inventory

Observed Windows SDK baseline: **10.0.26100.0**. Windows SDK and operating-
system components are governed by Microsoft terms and are build/runtime
prerequisites, not project source.

- Windows/SDK headers used by product or tests: `windows.h`, `bcrypt.h`,
  `cfgmgr32.h`, `malloc.h`, `ntddscsi.h`, `psapi.h`, `sddl.h`,
  `securitybaseapi.h`, `setupapi.h`, `softpub.h`, `tlhelp32.h`, `virtdisk.h`,
  `wincrypt.h`, `winioctl.h`, `winsvc.h`, `wintrust.h`, and `winver.h`.
- Windows import libraries linked by CMake: `bcrypt`, `wintrust`, `crypt32`,
  `Setupapi`, `Cfgmgr32`, `Psapi`, `Version`, and `Advapi32`.
- Native `ntdll.dll` entry points resolved dynamically: `NtSetInformationFile`,
  `RtlNtStatusToDosError`, and `RtlGetVersion`. These native interfaces are
  OS-supplied and may have a narrower compatibility contract than documented
  Win32 APIs.
- SHA-256 uses Windows CNG/BCrypt. CRC32C is project code. The project does not
  include or link OpenSSL.

System DLLs and SDK files are supplied by Windows or the installed toolchain;
none may be copied into a project release merely because the project imports
them.

### Visual C++ runtime deployment

The project does not set `CMAKE_MSVC_RUNTIME_LIBRARY` and inspected Release
binaries dynamically import the Visual C++ v14 runtime (`MSVCP140.dll`,
`MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`, and `VCRUNTIME140_1.dll`) plus
UCRT components. A released x64 binary therefore requires an installed x64
Visual C++ v14 Redistributable at least as recent as the toolset used to build
it, unless a license-compliant app-local deployment is deliberately selected.
The UCRT is an operating-system component on the supported Windows 11 baseline.
Microsoft's deployment terms are documented at
<https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files>.

Debug runtime DLLs are not redistributable. No Microsoft runtime DLL, Debug or
Release, is tracked by this repository.

## Build, test, and evidence tools

Observed versions are from the 2026-08-08 audit environment; “required” values
come from the repository build configuration. Tool versions are properties of
that environment, not of a source commit. These tools remain externally
installed.

| Component | Source | Required/observed version | License/status | Redistribution policy | Role |
|---|---|---|---|---|---|
| Visual Studio 2022 Build Tools: `cl.exe`, `link.exe`, `dumpbin.exe`, DevShell | Microsoft Visual Studio | MSVC 19.44.35228; linker/dumpbin 14.44.35228; toolset family 14.44 | Microsoft Visual Studio license terms | Do not redistribute tool binaries; only separately authorized redistributables may be deployed | x64 C++20 compile/link and dependency audit |
| `vswhere.exe` | <https://github.com/microsoft/vswhere> | 3.1.7 | MIT | External Visual Studio Installer component; not shipped by this project | Locate Visual Studio for `Enter-MsvcEnvironment.ps1` |
| Windows SDK | Microsoft Windows SDK | 10.0.26100.0 | Microsoft SDK terms | Build prerequisite; headers/libs are not shipped | Windows declarations and import libraries |
| CMake and CTest | <https://cmake.org/licensing/> | required >= 3.25; observed 4.2.2 | BSD 3-Clause | External build/test tools; not shipped | Configure, generate, build orchestration, test registration/execution |
| Ninja | <https://github.com/ninja-build/ninja> | required >= 1.11; observed 1.13.2 | Apache-2.0 | External build tool; not shipped | Ninja Multi-Config executor |
| Git for Windows | <https://github.com/git-for-windows/git> | 2.53.0.windows.1 | Git core GPL-2.0; bundled components have their own notices | External provenance tool; not shipped | source revision and evidence provenance |
| Windows 11 inbox tools and cmdlets | Microsoft Windows | host 10.0.26200.8875; guest Windows 11 test baseline | Microsoft Windows terms | OS-supplied; not shipped | `cmd`/`chcp`, `fsutil`, `powercfg`, services, CIM, Storage, process, firewall, and file/hash operations |
| Windows PowerShell / .NET Framework | Microsoft Windows / .NET Framework | PowerShell 5.1.26100.8875; CLR 4.0.30319.42000 | Microsoft Windows/.NET terms | OS-supplied; not shipped | JSON, SHA-256, orchestration, verification scripts |
| VMware Workstation and `vmrun.exe` | VMware Workstation | 17.6.4 build-24832109 | VMware proprietary EULA | External test infrastructure; not shipped | isolated Windows 11 guest and controlled power actions |
| OpenSSH for Windows | <https://github.com/PowerShell/Win32-OpenSSH> | client 9.5p2; guest server is an optional Windows capability | OpenSSH portable BSD-family notices; Windows packaging under Microsoft terms | OS/feature prerequisite; binaries are not shipped | host-to-guest test transport |
| LibreSSL used by OpenSSH build | <https://www.libressl.org/> | OpenSSH client reports LibreSSL 3.8.2 | ISC/BSD-style component notices | Transitive part of the installed OpenSSH build; not shipped separately | SSH cryptographic transport |

The repository uses a project-authored minimal C++ test framework at
`tests/support/test_framework.h`. It does not vendor Catch2, GoogleTest,
doctest, Boost, fmt, zlib, libewf, or another third-party C++ test/library
package. PowerShell JSON and cryptographic operations use Windows/.NET
components described above.

## Project-authored M3 fixture manifest

All 14 tracked files below are project-authored test data. Their hashes were
recomputed from the M3 audit worktree on 2026-08-08 and are source-content
identities rather than claims about a later sign-off commit. “Repository
license” means `AGPL-3.0-only` as recorded above. They may be redistributed only
with the project and its applicable notices; none is an upstream binary.

| Fixture | Verified SHA-256 | Origin/producer | License and redistribution |
|---|---|---|---|
| `tests/fixtures/m3/crash-matrix.json` | `BCCA9EABE466103610661633456B546BF95030A066FCDE1857A9E8A814EBDB4C` | Project crash oracle, introduced by `4de15e8d85c2b184a68ef3d91057224681cffa9c` | Repository license; project redistribution only |
| `tests/fixtures/m3/fault-phase-registry.json` | `A47C3E147A7CD8F9A275F0604AB92C0FCB087AABF6334A23E05E6BA6832BA1D2` | Project fault-phase contract, introduced by `c2cf23cb166ec27106932d9d75bf835897bb014f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-checkpoint-digest-v1.hex` | `34A94F5AAAE342701527A538D6DD1793B57FF0E03CA1451259C1984DD47FC010` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-commit-v1.hex` | `376C7D1BC9ED119674CB095C0FBBD76A57FEAA3CC84430CE0DE1FFB6ED5293D7` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-data-v1.hex` | `32E68B738C22D58F3352FA6C68295CD5E2B4D8504BA5E6100A968EE57D7DA3E5` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-discard-v1.hex` | `AB64904602C0B4A63842FC8CA8B3F09C75ACDE67B9FD311C0A0F7C7695F302EA` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-header-v1.hex` | `3912FDA436C1E08667CC43B0D2C5BC1F813C95FF2087421C46A69C2C4F9AF4F1` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-index-v1.hex` | `D8E7D1243DC5801E29831336574C8693AD430537C15D79D260F4C45E0DB97408` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-v1-layout.md` | `824BE2E2854FAA96F3A580C366B94758063F3CAEC2B3932FC3FB61A4A8C9C3A1` | Project journal v1 wire-layout contract, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/journal-zero-v1.hex` | `D58BB6EC5425AA6B7373C92706C68915F66D6A34991359C163E9369C8D9CC8FB` | Project journal v1 codec fixture, introduced by `59fe20d91c824d12ce983c7bbb773725f2e6952f` | Repository license; project redistribution only |
| `tests/fixtures/m3/manifest-v1.hex` | `CB327E553B95E416918693F077E957E3975869647CE29E37B9EF35A5F086C463` | Project manifest v1 codec fixture, introduced by `a8ee63c710369de9c6b9d65a1edaad0ecb29939b` | Repository license; project redistribution only |
| `tests/fixtures/m3/manifest-v1-layout.md` | `EF517A6D75308BA6902465C0E582CFE89E8BE1B6542B03295408DB0AC07B5AAC` | Project manifest v1 wire-layout contract, introduced by `a8ee63c710369de9c6b9d65a1edaad0ecb29939b` | Repository license; project redistribution only |
| `tests/fixtures/m3/real-crash-matrix.json` | `ECEA3CAFC534085D8B23CD474AC9407AE2F9C4A1490813EE5934F2EB18C68FEC` | Project real interruption oracle, introduced by `45db75f28670b0f5e1eff2a8e69d18d1003d76a7` | Repository license; project redistribution only |
| `tests/fixtures/m3/vhdx/strict-dynamic-v1-layout.md` | `44D6D4E1279ED00F58A281748D74D13036B8FAEAF103302B6E0CCC23CD745499` | Project-authored golden layout derived from MS-VHDX revision 8.0 field definitions; introduced by `29654253355e2f936413f43aa95a12a29ea82431` | Repository license for local expression; retain this MS-VHDX derivation notice; no Microsoft text/image redistributed |

Mutable VHDX, NTFS, crash, and soak evidence is generated outside the tracked
repository and referenced by hash. No tracked `.sys`, `.dll`, `.exe`, `.lib`,
`.vhd`, `.vhdx`, archive, installer, or third-party package is present.

## Published checksum validation vectors

The checksum tests use public algorithm test values, not third-party source or
binary code:

- CRC32C of the ASCII string `123456789` is `E3069283`. This is the CRC-32C
  example in IETF RFC 3720, Appendix B.4:
  <https://www.rfc-editor.org/rfc/rfc3720#appendix-B.4>.
- SHA-256 of the ASCII string `abc` is
  `BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD`.
  This is the SHA-256 example in NIST FIPS 180-4:
  <https://csrc.nist.gov/pubs/fips/180-4/upd1/final>.
- SHA-256 of the empty message is the standard digest
  `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`.
  It is retained as a second known-answer check for the Windows CNG-backed
  implementation.

Only the input and expected numeric digest values appear in tests. No RFC or
NIST prose, reference implementation, or test file is copied into the project.

## Release notice status

- The project copyright holder is recorded in `NOTICE`.
- This engineering inventory does not replace a legal review of distributing a
  future installer or binary bundle.
