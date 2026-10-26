# The second half of the power-cut acceptance (T1-09): after the machine was cut off
# and booted again, prove that the disk came back **on its own**, at the last committed
# generation, with the committed bytes intact and nothing left behind.
#
# Nothing here starts the service or the disk: the whole claim is that the OS brought
# the service up and the service brought the disk up.  This script only looks.

param(
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.powerloss_verify may only run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$cliExe = Join-Path $bin 'myrd-cli.exe'
$serviceName = 'MyRamDiskPowerLoss'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

$statePath = 'C:\m5\powerloss-state.txt'
if (-not (Test-Path -LiteralPath $statePath)) {
    Fail "no power-loss state at $statePath -- the prepare half did not run"
}
$state = @{}
foreach ($line in Get-Content -LiteralPath $statePath) {
    $at = $line.IndexOf('=')
    if ($at -gt 0) { $state[$line.Substring(0, $at)] = $line.Substring($at + 1) }
}
foreach ($key in @('profile_id', 'mount', 'generation', 'marker_payload')) {
    if (-not $state.ContainsKey($key)) { Fail "the power-loss state has no $key" }
}
$mount = $state['mount']
$expectedGeneration = [uint64]$state['generation']

Write-Step 'the service came up without being started by this script'
$answered = Wait-ForService 180
if ($null -eq $answered) {
    Fail ("the installed service did not answer within 180 s of the boot.`n" +
        "$((Get-ServiceState))`n$((Get-Content -LiteralPath 'C:\m5\last-service.log' -Raw -ErrorAction SilentlyContinue))")
}
Write-Host "  service state: $((Get-ServiceState).Trim())"

Write-Step 'the disk came back on its own'
# **Auto-start is the claim.**  The durable record said running when the power went, and
# the policy said come back; a `stopped` disk here means the bytes were recovered and
# nobody could use them.
$list = Invoke-Cli @('disk', 'list')
if ($list.exit -ne 0) { Fail "disk list failed: $($list.output)" }
Write-Host "  $($list.output.Trim())"
$line = ($list.output -split "`n") | Where-Object { $_ -match [regex]::Escape($mount) } |
    Select-Object -First 1
if ($null -eq $line) { Fail "the disk is absent after the power cut: $($list.output)" }
if ($line -notmatch 'running') {
    Fail "the disk did not come back running: $line"
}
$generationMatch = [regex]::Match($line, 'generation=(\d+)')
if (-not $generationMatch.Success) { Fail "disk list reported no generation: $line" }
$generation = [uint64]$generationMatch.Groups[1].Value
if ($generation -lt $expectedGeneration) {
    Fail ("the recovered generation ($generation) is older than the last committed one " +
        "($expectedGeneration): the cut cost a committed checkpoint")
}
Write-Host "  recovered generation: $generation (last committed before the cut: $expectedGeneration)"

Write-Step 'the committed bytes are intact'
$marker = Join-Path $mount ($state['marker_path'])
if (-not (Test-Path -LiteralPath $marker)) {
    Fail "the committed marker file is gone: $marker"
}
$payload = Get-Content -LiteralPath $marker -Raw
if ($payload -ne $state['marker_payload']) {
    Fail ("the committed marker contents changed:`n  expected: $($state['marker_payload'])`n  found:    $payload")
}
Write-Host "  marker bytes match the pre-cut record ($($payload.Length) chars)"

# What the cut may legitimately have cost: the file written after the last checkpoint.
# Its presence is not required; reporting it is what makes "at most one interval" a
# reading rather than a slogan.
$uncommitted = Join-Path $mount 'powerloss-uncommitted.txt'
if (Test-Path -LiteralPath $uncommitted) {
    Write-Host '  the post-checkpoint file survived (the cut landed after a later commit)'
} else {
    Write-Host '  the post-checkpoint file is gone (the cut cost the writes since the last interval)'
}

Write-Step 'nothing was left behind'
$workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
if ($workers.Count -ne 1) {
    Fail ("expected exactly one worker for one running disk, found $($workers.Count)")
}
$disks = @(Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction Stop |
    Where-Object { $_.Model -match 'AIM|MyRamDisk' })
Write-Host "  workers: $($workers.Count); aim-backed disks: $($disks.Count)"

Write-Step 'cleanup: stop and forget the disk, remove the service'
$stop = Invoke-Cli @('disk', 'stop', $state['profile_id'])
if ($stop.exit -ne 0) { Fail "disk stop failed: $($stop.output)" }
$stopId = Get-UuidFromText $stop.output
if ($null -eq $stopId) { Fail 'the stop carried no operation id' }
$stopState = Wait-ForTerminal $stopId 300
if ($stopState -ne 'succeeded') { Fail "the stop ended $stopState" }

$profileId = $state['profile_id']
$forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
if ($forget.exit -ne 0) { Fail "disk forget failed: $($forget.output)" }
$forgetId = Get-UuidFromText $forget.output
if ($null -eq $forgetId) { Fail 'the forget carried no operation id' }
$forgetState = Wait-ForTerminal $forgetId 300
if ($forgetState -ne 'succeeded') { Fail "the forget ended $forgetState" }
Remove-ScService
Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $state['bundle'] -Recurse -Force -ErrorAction SilentlyContinue

Write-Host 'PASS T1-09: the disk came back by itself at the committed generation'
exit 0
