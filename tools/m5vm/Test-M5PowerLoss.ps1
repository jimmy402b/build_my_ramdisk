# The power-cut acceptance (T1-09), host side.
#
# A hard power cut cannot be produced from inside the guest and cannot be observed from
# outside it, so the acceptance is three phases:
#
#   1. guest: `integration.m5.powerloss_prepare` installs the service with `start= auto`,
#      creates a disk with a 30 s checkpoint interval and `auto_start`, writes a marker,
#      waits for a commit that contains it, and records what must survive;
#   2. host: **this script** cuts the power with `vmrun stop ... hard` and boots the
#      machine again;
#   3. guest: `integration.m5.powerloss_verify` only looks -- it never starts the service
#      or the disk, because the claim is that the OS and the service did.
#
# The VMX path is a host fact, so it is a parameter (or M5VM_VMX) rather than a literal
# inside the loop; the guest address stays the SSH alias, as everywhere else
# (AGENTS.md section 5).

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
    [string]$Configuration = 'Release',
    [switch]$SkipStage,
    [int]$BootTimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }
if (-not $Vmx) { throw 'M5VM_VMX / -Vmx is required: the .vmx path of the guest on this host' }
if (-not $VmPassword) {
    throw 'M5VM_PASSWORD / -VmPassword is required: the encrypted-VM password for vmrun'
}

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path -LiteralPath $vmrun)) { throw "vmrun not found at $vmrun" }
if (-not (Test-Path -LiteralPath $Vmx)) { throw "the .vmx was not found: $Vmx" }

$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'

# **Run one harness phase and judge it by its own summary line.**
#
# The harness exits 0 even when the case fails -- measured twice, `MYRD_VM_EXIT=0` over two
# failing cases -- so its exit code says the request was delivered, not that anything
# passed.  The verdict is read from `N of M passed`, and a summary that cannot be found is
# a failure: "could not tell" must never read as "passed".  The call is inline rather than
# through a helper because a helper returning the captured text kept arriving empty here,
# and a two-minute guest round trip is not the place to debug a wrapper.
function Invoke-Phase {
    param([string]$What, [hashtable]$Arguments)
    Write-Host ("=== {0}: Invoke-M5Vm.ps1 {1}" -f $What,
        (($Arguments.GetEnumerator() | ForEach-Object { "-$($_.Key) $($_.Value)" }) -join ' '))
    # **`6>&1` is load-bearing.**  The harness reports its verdict with `Write-Host`, which goes to
    # the *information* stream -- `2>&1 | Out-String` captures only success output and stderr,
    # so the first version of this function saw an empty string and declared "no verdict line"
    # over a run whose verdict was printed right there on the console.  Measured twice.
    $text = & $harness @Arguments 6>&1 2>&1 | Out-String
    Write-Host $text
    $match = [regex]::Match([string]$text, '(\d+) of (\d+) passed')
    if (-not $match.Success) {
        throw ("the {0} phase produced no verdict line" -f $What)
    }
    $passed = [int]$match.Groups[1].Value
    $total = [int]$match.Groups[2].Value
    if ($total -eq 0 -or $passed -ne $total) {
        throw ("the {0} phase failed: {1} of {2} passed" -f $What, $passed, $total)
    }
    Write-Host ("  {0} phase: {1} of {2} passed" -f $What, $passed, $total)
}

function Wait-ForSsh([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $probe = & ssh -o BatchMode=yes -o ConnectTimeout=5 -i $KeyFile `
                "$VmUser@$VmHost" 'echo up' 2>&1 | Out-String
        } finally {
            $ErrorActionPreference = $previous
        }
        if ($probe -match 'up') { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

# Staging prints a count, not a verdict, so it is judged by its exit code alone -- the
# verdict rule below belongs to `-Run`, and applying it here declared "no verdict line"
# over a stage that had just succeeded (measured).
if (-not $SkipStage) {
    Write-Host '=== stage: Invoke-M5Vm.ps1 -Stage -Configuration '"$Configuration"
    $stage = & $harness -Stage -Configuration $Configuration 6>&1 2>&1 | Out-String
    Write-Host $stage
    if ($LASTEXITCODE -ne 0) { throw "staging failed ($LASTEXITCODE)" }
}

# ---- phase 1: leave a disk that the cut will interrupt -------------------------
Invoke-Phase -What 'prepare' -Arguments @{
    Run = $true
    Filter = '^integration\.m5\.powerloss_prepare$'
    MinTimeoutSeconds = 600
    TimeoutSeconds = 1200
}

# ---- phase 2: the cut ---------------------------------------------------------
# `stop hard` is the power cut: no shutdown, no preshutdown, no flush -- the machine
# simply stops.  That is the failure the recovery path exists for, and a graceful stop
# would prove nothing.
Write-Host '=== cutting the power (vmrun stop ... hard)'
& $vmrun -T ws -vp $VmPassword stop $Vmx hard
Start-Sleep -Seconds 5
Write-Host '=== powering the machine back on'
& $vmrun -T ws -vp $VmPassword start $Vmx nogui
if (-not (Wait-ForSsh $BootTimeoutSeconds)) {
    throw "the guest did not answer ssh within $BootTimeoutSeconds s of the reboot"
}
Write-Host '  ssh is up; waiting for the boot to settle'
# The service is started by the SCM partway through the boot and the verify phase waits for
# it, so this is only to let the network and the SCM report settle.
Start-Sleep -Seconds 20

# ---- phase 3: look, do not touch ----------------------------------------------
Invoke-Phase -What 'verify' -Arguments @{
    Run = $true
    Filter = '^integration\.m5\.powerloss_verify$'
    MinTimeoutSeconds = 900
    TimeoutSeconds = 1800
}

Write-Host 'PASS integration.m5.powerloss: the disk came back by itself after a hard power cut'
exit 0
