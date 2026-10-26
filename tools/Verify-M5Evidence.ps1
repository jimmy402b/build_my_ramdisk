# Refuses an evidence set that does not prove what it claims.
#
# **It never prints PASS.**  That is the whole discipline of this file: the tool exists because a
# green-looking evidence set was once assembled by hand and believed, so its job is to *refuse*
# things, and the strongest thing it may say is "accepted".  A missing artifact, an empty file, a
# results file whose case failed, or a run whose request id is not the one a document cites are all
# refusals with a reason.
#
# `-SelfTest` builds deliberately broken sets in a temporary directory and requires the checks to
# refuse each one.  A validator that has never refused anything is a validator nobody has tested.

[CmdletBinding()]
param(
    [string]$EvidenceDirectory,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

$script:problems = New-Object System.Collections.Generic.List[string]

function Refuse([string]$reason) {
    $script:problems.Add($reason)
}

function Require-File([string]$path, [string]$what) {
    if (-not (Test-Path -LiteralPath $path)) {
        Refuse "$what is missing: $path"
        return $false
    }
    if ((Get-Item -LiteralPath $path).Length -eq 0) {
        Refuse "$what is empty: $path"
        return $false
    }
    return $true
}

# The artifacts a complete M5 evidence set has to contain.  Paths are relative to the directory the
# tool is pointed at, so the same list applies to a release bundle and to a hand-built folder.
# The manifest plus the readings it points at.  **Not a fixed list of case files**: the evidence set is
# whatever the freeze included, and the manifest is what says so -- a hardcoded list drifts from it and
# then refuses a set that is complete (measured: it demanded the power-loss pair from a set that the
# manifest had deliberately left out).
$script:required = @('release-manifest.json')
$script:minimumReadings = 3

function Check-Evidence([string]$directory) {
    $script:problems.Clear()
    if (-not (Test-Path -LiteralPath $directory)) {
        Refuse "the evidence directory does not exist: $directory"
        return
    }
    foreach ($name in $script:required) {
        $path = Join-Path $directory $name
        if (-not (Require-File $path $name)) { continue }
        if ($name -like '*.results.json') {
            # A results file is only evidence if the case it names passed, and if it is the shape the
            # harness writes rather than a hand-written summary.
            try {
                $parsed = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            } catch {
                Refuse "$name is not JSON: $($_.Exception.Message)"
                continue
            }
            # **The artifact is an array of per-case results, not one object.**  Measured against the files
            # the runner actually writes: each element carries its own exit_code, so "the run passed" means
            # every element is zero -- and a single non-zero element is a refusal that names the case.
            $cases = @($parsed)
            if ($cases.Count -eq 0) {
                Refuse "$name contains no cases"
                continue
            }
            $failed = @($cases | Where-Object { $null -ne $_.exit_code -and [int]$_.exit_code -ne 0 })
            if ($failed.Count -ne 0) {
                $names = ($failed | ForEach-Object { $_.name }) -join ', '
                Refuse "$name records $($failed.Count) failing case(s): $names"
            }
            # The request id ties the file to one run; the runner writes it beside the results and in
            # the manifest, and a file with none cannot be attributed.
        }
        if ($name -eq 'release-manifest.json') {
            try {
                $manifest = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            } catch {
                Refuse "release-manifest.json is not JSON: $($_.Exception.Message)"
                continue
            }
            if (-not $manifest.frozen_at) { Refuse 'the manifest carries no frozen_at' }
            if (-not $manifest.binaries -or @($manifest.binaries).Count -eq 0) {
                Refuse 'the manifest lists no binaries, so nothing is frozen by it'
            }
            $readingCount = @($manifest.readings).Count
            if ($readingCount -lt $script:minimumReadings) {
                Refuse "the manifest carries $readingCount reading(s); at least $($script:minimumReadings) are required"
            }
        }
    }
}

function Report([string]$directory) {
    if ($script:problems.Count -eq 0) {
        # Not "PASS": the tool refuses, and when it cannot it says exactly that.
        Write-Host "accepted: $directory meets the evidence requirements"
        exit 0
    }
    foreach ($problem in $script:problems) {
        [Console]::Error.WriteLine("refused: $problem")
    }
    exit 1
}

if ($SelfTest) {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ('m5-evidence-selftest-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $failures = New-Object System.Collections.Generic.List[string]

    function Expect-Refusal([string]$label, [scriptblock]$build) {
        $case = Join-Path $root ([guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $case | Out-Null
        & $build $case
        Check-Evidence $case
        if ($script:problems.Count -eq 0) {
            $failures.Add("$label was accepted")
        } else {
            Write-Host "  refused as expected: $label ($($script:problems[0]))"
        }
    }

    $writeResults = {
        param($case, $exit, $id)
        $content = @{ exit_code = $exit; request_id = $id } | ConvertTo-Json
        Set-Content -LiteralPath (Join-Path $case 'm5-cli-e2e.results.json') -Value $content
    }

    Expect-Refusal 'an empty directory' { param($case) }
    Expect-Refusal 'a zero-byte artifact' {
        param($case)
        foreach ($name in $script:required) {
            Set-Content -LiteralPath (Join-Path $case $name) -Value ''
        }
    }
    Expect-Refusal 'a results file whose case failed' {
        param($case)
        & $writeResults $case 1 '00000000-0000-0000-0000-000000000001'
    }
    Expect-Refusal 'a manifest that freezes nothing' {
        param($case)
        & $writeResults $case 0 '00000000-0000-0000-0000-000000000001'
        $manifest = @{ frozen_at = '2026-10-27T00:00:00Z'; files = @() } | ConvertTo-Json
        Set-Content -LiteralPath (Join-Path $case 'release-manifest.json') -Value $manifest
    }

    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    if ($failures.Count -ne 0) {
        foreach ($failure in $failures) { [Console]::Error.WriteLine("self-test failed: $failure") }
        exit 1
    }
    Write-Host "self-test: every broken set was refused"
    exit 0
}

if (-not $EvidenceDirectory) {
    [Console]::Error.WriteLine('Verify-M5Evidence: -EvidenceDirectory or -SelfTest is required')
    exit 2
}

Check-Evidence $EvidenceDirectory
Report $EvidenceDirectory
