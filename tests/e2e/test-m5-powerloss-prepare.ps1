# The first half of the power-cut acceptance (T1-09): leave a disk that a hard
# power-off will interrupt, with everything the second half needs written down.
#
# It deliberately leaves the **installed** service running with `start= auto`: after a
# power cut nothing else would bring it up, and "recovered without a human" is the
# claim being measured.  The host-side orchestrator (`tools/m5vm/Test-M5PowerLoss.ps1`)
# cuts the power and then runs `test-m5-powerloss-verify.ps1`.
#
# State that survives the cut is written to `C:\m5\powerloss-state.txt`: the letter, the
# profile uuid, the committed generation, and the exact bytes of the committed marker
# file.  The verify half compares against that file rather than against anything it
# remembers.

param(
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

# **An uncaught exception must say what it was, on stdout.**
#
# Measured: this script ended about a second after `sc start` with its stdout stopping at
# the service's STATE block and its stderr a run of spaces -- no `Fail:` line, so not one
# of this script's own failures, and nothing a reader could act on.  `trap` writes the
# type, the message and the stack where the harness captures them; a guest round trip is
# two minutes and guessing is not affordable.
trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

function Exit-GuestOnly {
    [Console]::Error.WriteLine(
        'integration.m5.powerloss_prepare creates a real worker, a real AIM disk and a ' +
        'real Windows volume, and may only run on the guest (AGENTS.md section 4).')
    exit 2
}
if (-not $env:MYRD_M5VM_GUEST) { Exit-GuestOnly }

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
$serviceName = 'MyRamDiskPowerLoss'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @($serviceExe, $cliExe)) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing binary: $path" }
}

$statePath = 'C:\m5\powerloss-state.txt'
$workRoot = 'C:\m5\powerloss-' + [guid]::NewGuid().ToString('N')
$bundlePath = Join-Path $workRoot 'bundle'
$profileName = 'pline-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue

Write-Step "power-loss preparation on $env:COMPUTERNAME"
Remove-ScService

$mount = Select-FreeLetter
if ($null -eq $mount) { Fail 'no free drive letter for the power-loss disk' }
Write-Host "  the disk will be mounted at $mount"

Write-Step 'install the service so that a power cut is followed by a service'
# `start= auto` is the point: the SCM brings it up at boot, and the service is what
# decides whether anything comes back.  A `demand` service would need a human, which is
# exactly what the acceptance forbids.
Invoke-Sc @('create', $serviceName, 'binPath=', $serviceExe, 'type=', 'own', 'start=', 'auto')
Invoke-Sc @('start', $serviceName)
$answered = Wait-ForService 90
if ($null -eq $answered) {
    Fail "the installed service never answered a list command.`n$((Get-ServiceState))"
}
Write-Host "  installed service is answering: $($answered.output.Trim())"

Write-Step 'create a disk with a 30 s checkpoint interval, and ask it to come back'
# The interval is what makes the committed generation move without a client command;
# `auto_start` is what tells the next boot to bring the disk up (T1-09's consumer).
$create = Invoke-Cli @('disk', 'create', '--name', $profileName, '--bundle', $bundlePath,
    '--size', '64MiB', '--mount', $mount, '--checkpoint-interval', '30s')
if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
$createId = Get-UuidFromText $create.output
if ($null -eq $createId) { Fail 'the create response carried no operation identifier' }
$createState = Wait-ForTerminal $createId 300
if ($createState -ne 'succeeded') { Fail "the create ended $createState" }

$list = Invoke-Cli @('disk', 'list')
if ($list.exit -ne 0) { Fail "disk list failed: $($list.output)" }
Write-Host "  list: $($list.output.Trim())"
$line = ($list.output -split "`n") | Where-Object { $_ -match ('\s' + [regex]::Escape($mount)) } |
    Select-Object -First 1
if ($null -eq $line) { Fail "disk list did not report ${mount}: $($list.output)" }
# Parsed here, not through the shared helper: the helper returned null on a line that
# plainly carries a uuid (measured: `e68f6af3-... T: ready running ...`), and a failure
# that prints the line it could not read is worth more than a shared function whose
# behaviour has to be debugged on the guest.
$uuidMatch = [regex]::Match([string]$line,
    '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
if (-not $uuidMatch.Success) { Fail "disk list line carries no profile uuid: [$line]" }
$profileId = $uuidMatch.Value
Write-Host "  profile $profileId"

$autoStart = Invoke-Cli @('disk', 'config', 'set', 'auto_start', 'true', $profileId)
if ($autoStart.exit -ne 0) { Fail "config set auto_start failed: $($autoStart.output)" }
Write-Host "  auto_start enabled"

Write-Step 'write the marker, then wait for a committed generation that contains it'
$marker = Join-Path $mount '\powerloss-marker.txt'
$payload = 'written before the power cut at ' + (Get-Date).ToString('o')
Set-Content -LiteralPath $marker -Value $payload -Encoding utf8 -NoNewline
if ((Get-Content -LiteralPath $marker -Raw) -ne $payload) { Fail 'the marker did not read back' }

function Read-Generation {
    param([string]$letter)
    $listing = Invoke-Cli @('disk', 'list')
    if ($listing.exit -ne 0) { return $null }
    $row = ($listing.output -split "`n") | Where-Object { $_ -match ('\s' + [regex]::Escape($letter)) } |
        Select-Object -First 1
    if ($null -eq $row) { return $null }
    $match = [regex]::Match($row, 'generation=(\d+)')
    if (-not $match.Success) { return $null }
    return [uint64]$match.Groups[1].Value
}

# The marker must be inside a *committed* generation, so at least one automatic
# checkpoint has to land after the write.  30 s interval + the service's 15 s policy
# tick, so the window is generous; the generation moving is the evidence.
$generationBefore = Read-Generation $mount
if ($null -eq $generationBefore) { Fail 'disk list did not report a generation' }
Write-Host "  generation after the create: $generationBefore"
$commitDeadline = (Get-Date).AddSeconds(120)
$committedGeneration = $null
while ((Get-Date) -lt $commitDeadline) {
    Start-Sleep -Seconds 3
    $generation = Read-Generation $mount
    if ($null -ne $generation -and $generation -gt $generationBefore) {
        $committedGeneration = $generation
        break
    }
}
if ($null -eq $committedGeneration) {
    Fail ("no automatic checkpoint committed the marker within 120 s " +
        "(generation stayed $generationBefore)")
}
Write-Host "  committed generation: $committedGeneration (marker is inside it)"

# **What the cut may cost, written down rather than assumed.**  This file is written
# after the commit and is *not* waited on: it is the data the acceptance allows to be
# lost (at most one interval), and its absence after the cut is not a failure.
$uncommitted = Join-Path $mount '\powerloss-uncommitted.txt'
Set-Content -LiteralPath $uncommitted -Value 'written after the last checkpoint' -Encoding utf8

# **JSON, not `key=value`.**  Windows PowerShell's `Set-Content -Encoding utf8` writes a BOM,
# and the hand-written parser read the first key as "\uFEFFprofile_id" -- measured as "the
# power-loss state has no profile_id" over a file that plainly had one.  JSON removes the
# hand parsing along with the encoding question.
$json = @{ profile_id = $profileId
           profile_name = $profileName
           mount = $mount
           bundle = $bundlePath
           generation = $committedGeneration
           marker_path = 'powerloss-marker.txt'
           marker_payload = $payload
           service_name = $serviceName } | ConvertTo-Json
# **Flushed to the disk, not just to the file cache.**
#
# Measured: the state file came back after the cut with the right length and no content, so
# the verify half read an empty file and reported it unreadable -- correctly, because a file
# the power cut emptied is exactly what it saw.  `Set-Content` returns once the bytes are in
# the cache; `Flush($true)` is what makes them survive the machine losing power.
$bytes = [System.Text.Encoding]::ASCII.GetBytes($json)
$stream = [System.IO.FileStream]::new(
    [System.IO.Path]::GetFullPath($statePath), 'Create', 'Write', 'None')
try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
finally { $stream.Dispose() }
Write-Host "  state written to $statePath"
Write-Host 'PREPARED'
exit 0
