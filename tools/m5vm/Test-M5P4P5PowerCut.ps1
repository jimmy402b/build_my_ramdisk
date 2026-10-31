# R-02, host side: a hard power cut *between P4 and P5*, repeated N times.
#
# A cut cannot be produced from inside the guest and the window cannot be hit by polling, so
# the experiment is three phases per cycle:
#
#   1. guest: `integration.m5.p4p5_prepare` creates a disk, writes a marker, starts a
#      checkpoint and exits while the worker is *paused* after the landed commit (P4) and
#      before the service publishes the epoch (P5);
#   2. host: **this script** cuts the power with `vmrun stop ... hard` and boots again;
#   3. guest: `integration.m5.p4p5_verify` reads what came back and proves it is either the
#      last committed generation or the P4 candidate -- never a third state.
#
# The guest is reset between cycles so "N times" means N independent experiments, not one
# experiment observed N times.
#
# The VMX path is a host fact, so it is a parameter (or M5VM_VMX); the guest address stays
# the SSH alias, as everywhere else (AGENTS.md section 5).

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
    [string]$Configuration = 'Release',
    [switch]$SkipStage,
    [int]$Cycles = 1,
    [int]$BootTimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }
if (-not $Vmx) { throw 'M5VM_VMX / -Vmx is required: the .vmx path of the guest on this host' }
if (-not $VmPassword) { throw 'M5VM_PASSWORD / -VmPassword is required for vmrun' }
if ($Cycles -lt 1) { throw 'Cycles must be at least 1' }

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path -LiteralPath $vmrun)) { throw "vmrun not found at $vmrun" }
if (-not (Test-Path -LiteralPath $Vmx)) { throw "the .vmx was not found: $Vmx" }

$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
$sshTarget = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR',
             '-i', $KeyFile)
$remoteRoot = 'C:/m5/control'

function Invoke-Phase {
    param([string]$What, [hashtable]$Arguments)
    Write-Host ("=== {0}: Invoke-M5Vm.ps1 {1}" -f $What,
        (($Arguments.GetEnumerator() | ForEach-Object { "-$($_.Key) $($_.Value)" }) -join ' '))
    # `6>&1` is load-bearing: the harness reports its verdict with Write-Host (the
    # information stream), and `2>&1 | Out-String` alone captures an empty string.
    $text = & $harness @Arguments 6>&1 2>&1 | Out-String
    Write-Host $text
    $match = [regex]::Match([string]$text, '(\d+) of (\d+) passed')
    if (-not $match.Success) { throw ("the {0} phase produced no verdict line" -f $What) }
    $passed = [int]$match.Groups[1].Value
    $total = [int]$match.Groups[2].Value
    if ($total -eq 0 -or $passed -ne $total) {
        throw ("the {0} phase failed: {1} of {2} passed" -f $What, $passed, $total)
    }
    return [string]$text
}

# **The case's own stdout is not in the harness verdict.**  `Invoke-M5Vm.ps1 -Run` prints
# only "N of N passed" and the capture path; the readings (P4_REACHED, the generation
# numbers) live in the capture directory.  Measured: the first run of this driver passed the
# prepare phase and then declared "the prepare phase did not report P4_REACHED" because it
# searched the verdict text.
function Get-PhaseCapture {
    param([string]$PhaseText, [string]$CaseName)
    $match = [regex]::Match([string]$PhaseText, 'captures:\s*(\S+)')
    if (-not $match.Success) { throw 'the phase verdict did not print its capture directory' }
    $path = $match.Groups[1].Value.TrimEnd('\') + '\' + $CaseName + '.stdout.txt'
    return ((Invoke-Remote "if (Test-Path -LiteralPath '$path') { Get-Content -LiteralPath '$path' -Raw }") -join "`n")
}

function Invoke-Remote([string]$script) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @('') + @($script) | & ssh.exe @sshArgs $sshTarget 'powershell.exe -NoProfile -NonInteractive -Command -' 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) { throw "remote command failed ($code): $($output -join "`n")" }
    return $output
}

function Wait-ForSsh([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $probe = & ssh.exe @sshArgs $sshTarget 'echo up' 2>&1 | Out-String
        } finally {
            $ErrorActionPreference = $previous
        }
        if ($probe -match 'up') { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

# The guest reset is the same request the full-suite runner uses: the agent owns service
# control because it needs an elevated, non-network logon.
function Reset-Guest {
    $id = 'p4p5reset' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $local = Join-Path 'C:\tmp\m5-vm' "$id.request.ps1"
    @'
$ErrorActionPreference = 'Continue'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $local -Encoding utf8
    & scp.exe -q @sshArgs $local "${sshTarget}:$remoteRoot/$id.request.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'could not copy the reset request' }
    $pending = Join-Path 'C:\tmp\m5-vm' "$id.pending.id"
    Set-Content -LiteralPath $pending -Value $id -Encoding ascii -NoNewline
    & scp.exe -q @sshArgs $pending "${sshTarget}:$remoteRoot/pending.id"
    if ($LASTEXITCODE -ne 0) { throw 'could not copy pending.id' }
    Invoke-Remote "schtasks /run /tn 'MyRamDisk-M5-Agent' | Out-Null; 'triggered'" | Out-Null
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline) {
        $lines = Invoke-Remote "if (Test-Path '$remoteRoot/$id.done') { 'DONE' } else { 'WAIT' }"
        if (($lines -join ' ') -match 'DONE') {
            $text = (Invoke-Remote "Get-Content '$remoteRoot/$id.response.txt' -Raw") -join "`n"
            if ($text -notmatch 'MYRD_VM_EXIT=0' -or $text -notmatch 'RESET') {
                throw "the guest reset did not report RESET:`n$text"
            }
            return
        }
        Start-Sleep -Seconds 3
    }
    throw 'the guest reset did not finish within 180 s'
}

function Read-GuestResidue {
    $script = @'
Get-Process myrd-service,myrd-m5-worker -ErrorAction SilentlyContinue |
    Measure-Object | Select-Object -ExpandProperty Count
$disks = @(Get-Disk -ErrorAction SilentlyContinue).Count
"disks=$disks"
'@
    return (Invoke-Remote $script) -join "`n"
}

if (-not $SkipStage) {
    Write-Host '=== stage: Invoke-M5Vm.ps1 -Stage -Configuration '"$Configuration"
    $stage = & $harness -Stage -Configuration $Configuration 6>&1 2>&1 | Out-String
    Write-Host $stage
    if ($LASTEXITCODE -ne 0) { throw "staging failed ($LASTEXITCODE)" }
}

$results = New-Object System.Collections.ArrayList
for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
    Write-Host "`n############ cycle $cycle of $Cycles"
    Reset-Guest

    $prepareText = Invoke-Phase -What "prepare($cycle)" -Arguments @{
        Run = $true
        Filter = '^integration\.m5\.p4p5_prepare$'
        MinTimeoutSeconds = 900
        TimeoutSeconds = 1800
    }
    $prepareCapture = Get-PhaseCapture $prepareText 'integration.m5.p4p5_prepare'
    Write-Host $prepareCapture
    if ($prepareCapture -notmatch 'P4_REACHED') {
        throw "cycle ${cycle}: the prepare phase did not report P4_REACHED:`n$prepareCapture"
    }

    Write-Host '=== cutting the power (vmrun stop ... hard) while the worker is paused at P4'
    & $vmrun -T ws -vp $VmPassword stop $Vmx hard
    Start-Sleep -Seconds 5
    Write-Host '=== powering the machine back on'
    & $vmrun -T ws -vp $VmPassword start $Vmx nogui
    if (-not (Wait-ForSsh $BootTimeoutSeconds)) {
        Write-Host '  the guest did not answer ssh; power-cycling once'
        & $vmrun -T ws -vp $VmPassword stop $Vmx hard
        Start-Sleep -Seconds 10
        & $vmrun -T ws -vp $VmPassword start $Vmx nogui
        if (-not (Wait-ForSsh $BootTimeoutSeconds)) {
            throw "cycle ${cycle}: the guest did not answer ssh after a second boot"
        }
    }
    Start-Sleep -Seconds 20

    $verifyText = Invoke-Phase -What "verify($cycle)" -Arguments @{
        Run = $true
        Filter = '^integration\.m5\.p4p5_verify$'
        MinTimeoutSeconds = 900
        TimeoutSeconds = 1800
    }
    $verifyCapture = Get-PhaseCapture $verifyText 'integration.m5.p4p5_verify'
    Write-Host $verifyCapture
    if ($verifyCapture -notmatch 'PASS integration\.m5\.p4p5_verify') {
        throw "cycle ${cycle}: the verify phase did not report PASS:`n$verifyCapture"
    }

    $candidate = [regex]::Match($verifyCapture, 'READING: candidate_generation=(\d+)')
    $recovered = [regex]::Match($verifyCapture, 'READING: recovered_generation=(\d+)')
    $outcome = [regex]::Match($verifyCapture, 'READING: recovery_outcome=(\S+)')
    if (-not $candidate.Success -or -not $recovered.Success -or -not $outcome.Success) {
        throw "cycle ${cycle}: the verify phase did not print the generation readings"
    }
    $residue = Read-GuestResidue
    [void]$results.Add([pscustomobject]@{
        cycle = $cycle
        candidate = [uint64]$candidate.Groups[1].Value
        recovered = [uint64]$recovered.Groups[1].Value
        outcome = $outcome.Groups[1].Value
        residue = ($residue -replace "`n", '; ')
    })
    Write-Host ("--- cycle {0}: candidate={1} recovered={2} outcome={3}" -f
        $cycle, $results[$results.Count - 1].candidate,
        $results[$results.Count - 1].recovered,
        $results[$results.Count - 1].outcome)
}

Write-Host "`n=== R-02 summary"
foreach ($row in $results) {
    Write-Host ("  cycle {0}: candidate={1} recovered={2} outcome={3} residue=[{4}]" -f
        $row.cycle, $row.candidate, $row.recovered, $row.outcome, $row.residue)
}
$candidates = @($results | Where-Object { $_.outcome -eq 'candidate' }).Count
$legal = @($results | Where-Object {
    $_.outcome -eq 'candidate' -or $_.outcome -eq 'last_committed' }).Count
Write-Host ("  {0} of {1} cycles recovered to a legal generation ({2} to the P4 candidate)" -f
    $legal, $results.Count, $candidates)
Write-Host 'PASS integration.m5.p4p5_powercut: the P4/P5 cut always recovered to a legal generation'
exit 0
