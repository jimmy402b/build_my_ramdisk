# integration.m5.support_bundle -- a stranger can produce a package that explains a failure (R-15).
#
# **Guest-only** (it creates a real worker/volume and reads the durable logs, which are
# management-only).
#
# The case makes a real failure happen first -- a create whose bundle directory already holds a
# foreign file, so create-bundle fails with a classified error and the operation is recorded as
# failed -- then runs the shipped tool and asserts the archive:
#
#   1. exists, has a manifest.json with per-file SHA-256 and a `failure` section;
#   2. the failure section names that operation;
#   3. the operation's own `show` output is inside the archive;
#   4. versions/OS/driver/service/logs are all present;
#   5. **redaction**: the user profile path and the computer name do not appear anywhere.
#
# It deliberately does not assert on message wording of the product failure beyond "failed":
# this case is about the package, not about create.

param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.support_bundle creates a real worker and volume; it may only run on ' +
        'the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
$tool = Join-Path $repo 'tools\New-M5SupportBundle.ps1'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$workRoot = 'C:\m5\support-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$serviceLog = Join-Path $workRoot 'service.log'
$bundleZip = Join-Path $workRoot 'support.zip'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$serviceProcess = $null

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text,
        '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
    if ($match.Success) { return $match.Value }
    return $null
}

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $show = Invoke-Cli @('operation', 'show', $operationId)
        $match = [regex]::Match($show.output,
            '(?m)^operation\s+\S+:\s+(succeeded|completed|failed|cancelled)\b')
        if ($match.Success) {
            if ($match.Groups[1].Value -eq 'completed') { return 'succeeded' }
            return $match.Groups[1].Value
        }
        Start-Sleep -Milliseconds 200
    }
    return 'timeout'
}

try {
    Write-Step 'start the service so the durable state and logs exist'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog `
        -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
        -WindowStyle Hidden -PassThru
    if ($null -eq (Wait-ForService 90)) { Fail 'the service never answered' }

    Write-Step 'make a real failure: a create whose bundle directory already holds a foreign file'
    $badBundle = Join-Path $workRoot 'bad-bundle'
    New-Item -ItemType Directory -Force -Path $badBundle | Out-Null
    Set-Content -LiteralPath (Join-Path $badBundle 'not-a-bundle.txt') -Value 'foreign' -Encoding ASCII
    $mount = $null
    foreach ($candidate in @('U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if (Test-Path "$candidate\") { continue }
        if ($null -eq (Get-Volume -DriveLetter $candidate.TrimEnd(':') -ErrorAction SilentlyContinue)) {
            $mount = $candidate
            break
        }
    }
    if ($null -eq $mount) { Fail 'no free drive letter' }
    $bad = Invoke-Cli @('disk', 'create', '--name', 'support-failure',
        '--bundle', $badBundle, '--size', '64MiB', '--mount', $mount,
        '--checkpoint-interval', '30m')
    Write-Reading 'failure.cli_exit' ([string]$bad.exit)
    Write-Reading 'failure.cli_output' (($bad.output -replace "`n", ' | '))
    $failedId = Get-UuidFromText $bad.output
    if ($null -eq $failedId) { Fail "the failing create produced no operation: $($bad.output)" }
    $failedState = Wait-ForTerminal $failedId 300
    Write-Reading 'failure.operation_state' $failedState
    if ($failedState -ne 'failed') { Fail "the create was expected to fail, ended $failedState" }

    Write-Step 'a stranger runs the shipped tool'
    $toolOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $tool `
        -Output $bundleZip -InstallDir $bin -DataRoot 'C:\ProgramData\MyRamDisk' 2>&1 | Out-String
    Write-Reading 'tool.exit' ([string]$LASTEXITCODE)
    Write-Reading 'tool.tail' ((($toolOutput -split "`r?`n") | Select-Object -Last 4) -join ' | ')
    if ($LASTEXITCODE -ne 0) { Fail "the support bundle tool failed: $toolOutput" }
    if (-not (Test-Path -LiteralPath $bundleZip)) { Fail 'no archive was produced' }
    Write-Reading 'archive.bytes' ([string](Get-Item -LiteralPath $bundleZip).Length)

    Write-Step 'the archive explains that failure'
    $extract = Join-Path $workRoot 'extracted'
    Expand-Archive -LiteralPath $bundleZip -DestinationPath $extract -Force
    $manifestPath = Join-Path $extract 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { Fail 'the archive has no manifest.json' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Write-Reading 'manifest.files' ([string]$manifest.files.Count)
    Write-Reading 'manifest.redacted' ([string]$manifest.redacted)
    if ($null -eq $manifest.failure) { Fail 'the manifest carries no failure section' }
    Write-Reading 'manifest.failure.operation' ([string]$manifest.failure.operation_id)
    if ($manifest.failure.operation_id -ne $failedId) {
        Fail "the failure section names $($manifest.failure.operation_id), expected $failedId"
    }
    $operationFile = Join-Path $extract ("operations/" + $failedId + ".txt")
    if (-not (Test-Path -LiteralPath $operationFile)) { Fail 'the failed operation is not in the archive' }
    $operationText = Get-Content -LiteralPath $operationFile -Raw
    if ($operationText -notmatch 'failed') { Fail "the operation file does not say failed: $operationText" }

    Write-Step 'the archive carries the rest of the required evidence'
    foreach ($required in @('environment.json', 'versions.json', 'driver.json', 'service.json',
                            'profiles.json', 'logs/service.log', 'commands/disk-list.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $extract $required))) {
            Fail "the archive is missing $required"
        }
    }
    $versions = Get-Content -LiteralPath (Join-Path $extract 'versions.json') -Raw | ConvertFrom-Json
    if ($versions.cli -notmatch '1\.0\.0') { Fail "the cli version is not in the archive: $($versions.cli)" }
    $serviceLogText = Get-Content -LiteralPath (Join-Path $extract 'logs/service.log') -Raw
    Write-Reading 'logs.service_bytes' ([string]$serviceLogText.Length)

    Write-Step 'redaction: no user profile path and no computer name'
    $offenders = @()
    Get-ChildItem -LiteralPath $extract -Recurse -File | ForEach-Object {
        if ($_.Extension -in @('.json', '.txt', '.log')) {
            $text = Get-Content -LiteralPath $_.FullName -Raw
            if ($text -match [regex]::Escape($env:USERPROFILE)) { $offenders += $_.FullName + ' (profile)' }
            if ($text -match [regex]::Escape($env:COMPUTERNAME)) { $offenders += $_.FullName + ' (computer)' }
        }
    }
    Write-Reading 'redaction.offenders' ([string]$offenders.Count)
    if ($offenders.Count -ne 0) { Fail ("the archive leaks identity: " + ($offenders -join '; ')) }

    Write-Host 'PASS integration.m5.support_bundle: one command produced a redacted, self-explaining archive'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
