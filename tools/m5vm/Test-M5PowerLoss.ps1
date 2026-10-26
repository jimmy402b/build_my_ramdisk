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
if (-not $Vmx) {
    throw 'M5VM_VMX / -Vmx is required: the .vmx path of the guest on this host'
}
if (-not $VmPassword) {
    throw 'M5VM_PASSWORD / -VmPassword is required: the encrypted-VM password for vmrun'
}

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path -LiteralPath $vmrun)) { throw "vmrun not found at $vmrun" }
if (-not (Test-Path -LiteralPath $Vmx)) { throw "the .vmx was not found: $Vmx" }

function Invoke-Harness {
    # A **hashtable** splat, not an array one: an array splat passes positional
    # arguments, so `-Run` arrives as a string and the harness asks for a positional
    # parameter that does not exist.  Measured.
    param([hashtable]$Arguments)
    Write-Host ("=== Invoke-M5Vm.ps1 {0}" -f (($Arguments.GetEnumerator() | ForEach-Object { "-$($_.Key) $($_.Value)" }) -join ' '))
    $output = & (Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1') @Arguments 2>&1 | Out-String
    Write-Host $output
    return [pscustomobject]@{ exit = $LASTEXITCODE; output = $output }
}

# **The harness exits 0 even when a case fails.**  Its exit code reports the request,
# not the verdict -- measured: two failing cases and `MYRD_VM_EXIT=0`.  So the verdict is
# read from its own summary line, and a summary that cannot be found is a failure too:
# "could not tell" must never read as "passed".
function Assert-CasePassed {
    param($Result, [string]$What)
    $match = [regex]::Match($Result.output, '(\d+) of (\d+) passed')
    if (-not $match.Success) {
        throw ("the {0} half produced no verdict line (exit {1})" -f $What, $Result.exit)
    }
    $passed = [int]$match.Groups[1].Value
    $total = [int]$match.Groups[2].Value
    if ($total -eq 0 -or $passed -ne $total) {
        throw ("the {0} half failed: {1} of {2} passed" -f $What, $passed, $total)
    }
    Write-Host ("  {0} half: {1} of {2} passed" -f $What, $passed, $total)
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

if (-not $SkipStage) {
    $stage = Invoke-Harness -Arguments @{ Stage = $true; Configuration = $Configuration }
    if ($stage.exit -ne 0) { throw "staging failed ($($stage.exit))" }
}

# ---- phase 1: leave a disk that the cut will interrupt -------------------------
$prepare = Invoke-Harness -Arguments @{ Run = $true;
    Filter = '^integration\.m5\.powerloss_prepare$';
    MinTimeoutSeconds = 600; TimeoutSeconds = 1200 }
try {
    Assert-CasePassed -Result $prepare -What 'prepare'
} catch {
    throw "$($_.Exception.Message); the power cut was not attempted"
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
# The service is started by the SCM partway through the boot, and the verify half waits
# for it, so this is only to let the network and the SCM report settle.
Start-Sleep -Seconds 20

# ---- phase 3: look, do not touch ----------------------------------------------
$verify = Invoke-Harness -Arguments @{ Run = $true;
    Filter = '^integration\.m5\.powerloss_verify$';
    MinTimeoutSeconds = 900; TimeoutSeconds = 1800 }
Assert-CasePassed -Result $verify -What 'verify'

Write-Host 'PASS integration.m5.powerloss: the disk came back by itself after a hard power cut'
exit 0
