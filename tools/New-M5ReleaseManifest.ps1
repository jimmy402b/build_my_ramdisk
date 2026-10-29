# Writes the release manifest: the four binaries, their hashes, and the readings they were accepted on.
#
# **This is the tool, not the freeze.**  Running it produces a manifest for whatever is on disk *now*;
# the freeze is the act of running it once, at the end, after the last change -- which is why it is a
# separate step from every build.  Freezing before T4-02 and T4-03 are finished would freeze a release
# that is not the release.
#
# It refuses rather than guesses: a missing binary, an unreadable directory, or a hash that cannot be
# computed stops it.  A manifest with a hole in it is worse than no manifest, because it looks complete.

[CmdletBinding()]
param(
    [string]$BinariesDirectory = '',
    [string]$EvidenceDirectory = '',
    [string]$OutputPath = '',
    [string]$Configuration = 'Release',
    [switch]$Signed
)

$ErrorActionPreference = 'Stop'

# Resolved in the body rather than in the parameter defaults, because a default that reads
# `$PSScriptRoot` is evaluated before the script's own directory is always known -- measured as
# 'Cannot bind argument to parameter Path because it is an empty string'.
$scriptDirectory = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$repoRoot = Split-Path -Parent $scriptDirectory
if (-not $BinariesDirectory) { $BinariesDirectory = Join-Path $repoRoot 'out/build/msvc/src/Release' }
if (-not $EvidenceDirectory) { $EvidenceDirectory = Join-Path $repoRoot 'out/m5-evidence' }
if (-not $OutputPath) { $OutputPath = Join-Path $EvidenceDirectory 'release-manifest.json' }

function Fail([string]$message) {
    [Console]::Error.WriteLine("manifest: $message")
    exit 1
}

$binaries = @('myrd-service.exe', 'myrd-cli.exe', 'myrd-gui.exe', 'myrd-m5-worker.exe')
foreach ($name in $binaries) {
    $path = Join-Path $BinariesDirectory $name
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing binary: $path" }
}

$files = foreach ($name in $binaries) {
    $path = Join-Path $BinariesDirectory $name
    $hash = Get-FileHash -LiteralPath $path -Algorithm SHA256
    if (-not $hash.Hash) { Fail "no hash for $path" }
    [ordered]@{
        name   = $name
        bytes  = (Get-Item -LiteralPath $path).Length
        sha256 = $hash.Hash.ToLowerInvariant()
    }
}

# The readings the release is accepted on, taken from the evidence directory's results files.  They
# are summarised rather than copied: the manifest says which run each line came from, and the results
# files themselves are the evidence.
$readings = @()
if (Test-Path -LiteralPath $EvidenceDirectory) {
    foreach ($file in (Get-ChildItem -LiteralPath $EvidenceDirectory -Filter '*.results.json' -ErrorAction SilentlyContinue)) {
        try {
            $parsed = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        } catch {
            Fail "$($file.Name) is not JSON: $($_.Exception.Message)"
        }
        # The runner writes an array of per-case results; taking `[int]$parsed.exit_code` on that is what
        # produced 'Cannot convert System.Object[] to System.Int32'.  A reading is: how many cases, how many
        # failed, and the worst exit code.
        $cases = @($parsed)
        # **A SKIP is not a failure.**  The structural cases exit 77 when the guest has
        # no Visual Studio (vswhere); counting that as `failed` put "structural: 2
        # failed" into the release manifest while the guest report called it SKIP.  The
        # runner's `status` field is authoritative when present; older readings predate
        # it and keep the exit-code rule.
        $skipped = @($cases | Where-Object {
            $null -ne $_.status -and [string]$_.status -eq 'skip' })
        $failed = @($cases | Where-Object {
            ($null -eq $_.status -or [string]$_.status -ne 'skip') -and
            $null -ne $_.exit_code -and [int]$_.exit_code -ne 0 })
        $readings += [ordered]@{
            artifact    = $file.Name
            cases       = $cases.Count
            skipped     = $skipped.Count
            failed      = $failed.Count
            worst_exit  = if ($failed.Count -eq 0) { 0 } else { 1 }
            failing     = @($failed | ForEach-Object { $_.name })
        }
    }
}

$manifest = [ordered]@{
    product      = 'MyRamDisk'
    milestone    = 'M5'
    configuration = $Configuration
    frozen_at    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    signed       = [bool]$Signed
    binaries     = $files
    readings     = $readings
    # What this manifest does *not* say, written down so nobody reads more into it than it carries.
    limits       = @(
        'The hashes describe the binaries in the directory named at generation time.',
        'The readings are the results files present in the evidence directory at that time.',
        'Nothing here asserts that a guest run happened; the results files carry their own request ids.'
    )
}

$directory = Split-Path -Parent $OutputPath
if ($directory -and -not (Test-Path -LiteralPath $directory)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding ASCII
Write-Host "manifest: wrote $OutputPath ($($files.Count) binaries, $($readings.Count) readings)"
exit 0
