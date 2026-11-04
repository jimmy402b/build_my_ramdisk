# New-M5ReleasePackage.ps1 -- assembles the v1.0.0 delivery folder from the frozen evidence.
#
# The release is **unsigned** (decision 2), so the checksums are the integrity story: this tool
# copies the four binaries, the installer/uninstaller, the support pages and the frozen
# `release-manifest.json`, writes a standalone `SHA256SUMS`, and refuses to finish when a binary
# no longer matches the hash the manifest froze.
#
# Output (not committed; binaries live outside git):
#   out/release/MyRamDisk-<version>-win11-x64/
#     bin/            myrd-service.exe myrd-cli.exe myrd-gui.exe myrd-m5-worker.exe
#     install/        Install-MyRamDisk.ps1 Uninstall-MyRamDisk.ps1
#     docs/           m5-support.md m5-support-matrix.md
#     release-manifest.json
#     SHA256SUMS
#     README.txt

param(
    [string]$Version = '1.0.0',
    [string]$BinariesDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\build\msvc\src\Release'),
    [string]$EvidenceDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\m5-evidence'),
    [string]$OutputRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\release'),
    [switch]$WhatIfOnly
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$name = "MyRamDisk-$Version-win11-x64"
# **Absolute, because the sums are computed by slicing each file's full path.**  With a
# relative `-OutputRoot` (e.g. `out/release`) the substring below cut the absolute path at
# the relative string's length and wrote entries like
# `disk/.worktrees/m5-lifecycle/out/release/.../bin/myrd-cli.exe` -- valid hashes, useless
# paths.  Measured on the 1.0.1 package; 1.0.0 used the default absolute root and was fine.
$target = [System.IO.Path]::GetFullPath((Join-Path $OutputRoot $name))
$manifestPath = Join-Path $EvidenceDirectory 'release-manifest.json'

if (-not (Test-Path -LiteralPath $manifestPath)) { throw "the frozen manifest is missing: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not $manifest.frozen_at) { throw 'the manifest carries no frozen_at' }

$binaries = @($manifest.binaries | ForEach-Object { $_.name })
if ($binaries.Count -ne 4) { throw "the manifest lists $($binaries.Count) binaries, expected 4" }

# **Verify before copying.**  A package whose binaries do not match the manifest would ship a
# different product from the one that was rehearsed.
$mismatches = @()
foreach ($entry in $manifest.binaries) {
    $path = Join-Path $BinariesDirectory $entry.name
    if (-not (Test-Path -LiteralPath $path)) { $mismatches += "$($entry.name) is missing"; continue }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $entry.sha256) { $mismatches += "$($entry.name) is $hash, the manifest froze $($entry.sha256)" }
}
if ($mismatches.Count -ne 0) {
    foreach ($problem in $mismatches) { [Console]::Error.WriteLine("refused: $problem") }
    exit 1
}

if ($WhatIfOnly) {
    Write-Host "would package $name from $BinariesDirectory (4 binaries match the manifest)"
    exit 0
}

New-Item -ItemType Directory -Force -Path (Join-Path $target 'bin') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $target 'install') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $target 'docs') | Out-Null

foreach ($entry in $manifest.binaries) {
    Copy-Item -LiteralPath (Join-Path $BinariesDirectory $entry.name) -Destination (Join-Path $target 'bin') -Force
}
foreach ($script in @('Install-MyRamDisk.ps1', 'Uninstall-MyRamDisk.ps1')) {
    $source = Join-Path $repo "tools\install\$script"
    if (-not (Test-Path -LiteralPath $source)) { throw "the installer script is missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $target 'install') -Force
}
foreach ($doc in @('m5-support.md', 'm5-support-matrix.md')) {
    $source = Join-Path $repo "docs\operations\$doc"
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $target 'docs') -Force }
}
Copy-Item -LiteralPath $manifestPath -Destination $target -Force

$readme = @"
MyRamDisk $Version (Windows 11 x64, unsigned)
=============================================

What it is
  A single-disk RAM-backed disk: a service, a SYSTEM worker, an AIM virtual disk and an NTFS
  volume. Data that has been checkpointed survives a power cut; uncommitted writes may be lost
  up to the last successful checkpoint.

Supported
  Windows 11 x64 only. One MyRamDisk disk at a time. Drive letter chosen at create
  (`disk create --mount R:`) and reused on every start. NTFS, default 4 KiB cluster
  (4/8/16/32/64 KiB selectable at create). Requires phdskmnt >= 1.2.28.93 -- the installer
  CHECKs the driver and never installs, replaces or downgrades it.

Not supported
  Windows 10 / Server / ARM64; a second disk; volume labels (`--volume-label` is refused);
  in-place version rollback (reinstall only); over-commit beyond the machine's commit limit;
  Bundles on BitLocker or SMB (untested); changing the cluster size after create.

Install
  Right-click PowerShell as Administrator, then:
    powershell -ExecutionPolicy Bypass -File install\Install-MyRamDisk.ps1
  Uninstall keeps your data:
    powershell -ExecutionPolicy Bypass -File install\Uninstall-MyRamDisk.ps1

Verify the download
  The four binaries are listed in release-manifest.json (frozen) and in SHA256SUMS.
    Get-FileHash .\bin\myrd-service.exe -Algorithm SHA256
  The release is unsigned, so Windows SmartScreen may warn; the SHA-256 values are the
  integrity check.

Usage and troubleshooting
  docs\m5-support.md -- supported scope, data-loss bounds, backup/export, uninstall notes and
  where the logs are.
  Release evidence: release-manifest.json (frozen_at, binary hashes, per-run readings).
"@
Set-Content -LiteralPath (Join-Path $target 'README.txt') -Value $readme -Encoding UTF8

$lines = @()
$files = @(Get-ChildItem -LiteralPath $target -Recurse -File | Where-Object { $_.Name -ne 'SHA256SUMS' } | Sort-Object FullName)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($target.Length + 1).Replace('\', '/')
    $lines += ('{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative)
}
$lines | Set-Content -LiteralPath (Join-Path $target 'SHA256SUMS') -Encoding ASCII

Write-Host "packaged: $target"
Write-Host ("  {0} files, 4 binaries match the frozen manifest" -f ($files.Count + 1))
Write-Host ("  SHA256SUMS covers {0} files" -f $lines.Count)
