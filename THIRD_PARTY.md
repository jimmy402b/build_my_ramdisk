# Third-Party Notices

## Arsenal Image Mounter

- **Repository**: https://github.com/ArsenalRecon/Arsenal-Image-Mounter
- **Fixed Commit**: `d379caa027d21902e45fce8c9daf02fd116c2dda`
- **License**: GNU Affero General Public License v3.0 (AGPL-3.0)
- **Usage in M0**: Protocol definitions and ABI constants from the following
  normative source files are referenced for interoperability with the installed
  signed `phdskmnt.sys` driver:

  - `Unmanaged Source/phdskmnt/inc/common.h`
  - `Unmanaged Source/phdskmnt/inc/imdproxy.h`
  - `Unmanaged Source/phdskmnt/proxy.cpp`
  - `Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs`

  No AIM Toolkit source code, binaries, or user-mode libraries (aimapi.dll,
  aim_ll.exe, RamDyn.exe, etc.) are distributed, linked, or loaded by M0.

## Windows SDK

M0 links against standard Windows SDK system libraries only. No third-party
C++ libraries are included.
