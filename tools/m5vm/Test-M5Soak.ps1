# Test-M5Soak.ps1 -- the 24 h soak driver (R-13).
#
# **Host side.**  It loops the guest case `integration.m5.soak` (one round per invocation),
# records every round's output, and every `-CutEveryRounds` rounds cuts the power with
# `vmrun stop ... hard`, boots the guest again, and runs the case in `verify` mode.
#
# The guest case owns the measurement (handle counts, worker count, working set, generation,
# committed-data hash); this script owns the wall clock and the power cuts.  Results:
#
#   <OutDir>\soak.log          one line per harness outcome plus each round's output
#   <OutDir>\failures.log      the first failure with the guest capture path (evidence)
#   <OutDir>\summary.txt       totals, duration, first/last handle and memory readings
#
# It never SSHes during a guest test: the guest case is a harness submission and the only
# out-of-band action is the power cut, which is the point of the exercise.

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
    [string]$Configuration = 'Release',
    [double]$Hours = 24,
    [int]$CutEveryRounds = 10,
    [string]$OutDir = 'C:\tmp\m5-soak',
    [switch]$SkipStage,
    [int]$BootTimeoutSeconds = 900,
    [int]$MaxRounds = 0
)

$ErrorActionPreference = 'Stop'

if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }
if (-not $Vmx) { throw 'M5VM_VMX / -Vmx is required' }
if (-not $VmPassword) { throw 'M5VM_PASSWORD / -VmPassword is required for vmrun' }
if ($Hours -le 0) { throw 'Hours must be positive' }
if ($CutEveryRounds -lt 1) { throw 'CutEveryRounds must be at least 1' }

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path -LiteralPath $vmrun)) { throw "vmrun not found at $vmrun" }
if (-not (Test-Path -LiteralPath $Vmx)) { throw "the .vmx was not found: $Vmx" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$soakLog = Join-Path $OutDir 'soak.log'
$failureLog = Join-Path $OutDir 'failures.log'
$summary = Join-Path $OutDir 'summary.txt'
$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'

$sshTarget = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR',
             '-i', $KeyFile)

function Write-Log([string]$line) {
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    Add-Content -LiteralPath $soakLog -Value "[$stamp] $line" -Encoding UTF8
    Write-Host "[$stamp] $line"
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
        try { $probe = & ssh.exe @sshArgs $sshTarget 'echo up' 2>&1 | Out-String } finally { $ErrorActionPreference = $previous }
        if ($probe -match 'up') { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Wait-ForScp([int]$seconds) {
    $probe = Join-Path 'C:\tmp\m5-vm' 'soak-scp-probe.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $probe) | Out-Null
    Set-Content -LiteralPath $probe -Value 'probe' -Encoding ASCII
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & scp.exe @sshArgs $probe "$($sshTarget):C:/m5/soak/scp-probe.txt" 2>&1 | Out-Null
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previous
        }
        if ($code -eq 0) { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Set-SoakDriver([string]$value) {
    # Tells the guest case that the 24 h driver is calling: it then keeps one service alive
    # across rounds instead of stopping what it started.
    Invoke-Remote ("New-Item -ItemType Directory -Force -Path 'C:\m5\soak' | Out-Null; " +
        "Set-Content -LiteralPath 'C:\m5\soak\driver.txt' -Value '$value' -Encoding ASCII; 'ok'") | Out-Null
}

function Set-SoakMode([string]$mode) {
    Invoke-Remote ("New-Item -ItemType Directory -Force -Path 'C:\m5\soak' | Out-Null; " +
        "Set-Content -LiteralPath 'C:\m5\soak\mode.txt' -Value '$mode' -Encoding ASCII; 'ok'") | Out-Null
}

function Invoke-SoakRound([string]$label) {
    # **`Write-Host` output is not capturable through the pipeline**, so the harness's printed
    # outcome is not what this driver reads: the guest case writes `C:\m5\soak\last.txt` with
    # `PASS`/`FAIL` plus its readings, and that file is the source of truth.
    Invoke-Remote ("Remove-Item -LiteralPath 'C:\m5\soak\last.txt' -Force -ErrorAction SilentlyContinue; 'ok'") | Out-Null
    $runOutput = & $harness -Run -Filter '^integration\.m5\.soak$' `
        -TimeoutOverrideSeconds 1800 2>&1 | Out-String
    $exit = $LASTEXITCODE
    $status = Invoke-Remote ("if (Test-Path 'C:\m5\soak\last.txt') { Get-Content 'C:\m5\soak\last.txt' -Raw } else { 'MISSING' }")
    $text = ($status -join "`n")
    $passed = $text -match '(?m)^PASS'
    Write-Log ("{0}: harness_exit={1} passed={2}" -f $label, $exit, $passed)
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match 'READING: soak\.') { Add-Content -LiteralPath $soakLog -Value ("    " + $line.Trim()) -Encoding UTF8 }
    }
    if (-not $passed) {
        Add-Content -LiteralPath $failureLog -Value ("[$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))] $label failed:`n$runOutput`n--- guest last.txt ---`n$text") -Encoding UTF8
        return $null
    }
    return $text
}

function Get-Metric([string]$text, [string]$name) {
    $match = [regex]::Match($text, "READING: soak\.$name=([^\r\n]+)")
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return $null
}

try {
    if (-not $SkipStage) {
        Write-Log 'staging the build once'
        & $harness -Stage -Configuration $Configuration 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'staging failed' }
    }
    & $vmrun -T ws -vp $VmPassword start $Vmx nogui 2>&1 | Out-Null
    if (-not (Wait-ForSsh $BootTimeoutSeconds)) { throw 'the guest never answered ssh' }
    # The scp probe writes into the soak directory, so it has to exist first: without this the
    # probe failed for fifteen minutes and the driver reported "never answered scp" while the
    # guest was perfectly healthy.
    Invoke-Remote ("New-Item -ItemType Directory -Force -Path 'C:\m5\soak' | Out-Null; 'ok'") | Out-Null
    if (-not (Wait-ForScp $BootTimeoutSeconds)) { throw 'the guest never answered scp' }
    # **Start from a clean guest.**  A profile left behind by an earlier failure holds its
    # mount target and makes the next create fail with "the mount target is already registered
    # to another disk" (measured); the case cannot always clear it, because a profile from a
    # failed create is not necessarily forgettable.  The reset script is the supported way.
    Write-Log 'resetting the guest before the soak'
    Invoke-Remote ("& powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
        "'C:\m5\Reset-M5Guest.ps1' 2>&1 | Out-String") | ForEach-Object { Write-Log ("  reset: " + $_.Trim()) }
    Set-SoakDriver 'soak'
    Set-SoakMode 'round'

    $deadline = (Get-Date).AddHours($Hours)
    $round = 0
    $cuts = 0
    $failures = 0
    $first = $null
    $last = $null
    while ((Get-Date) -lt $deadline) {
        if ($MaxRounds -gt 0 -and $round -ge $MaxRounds) { break }
        $round++
        $text = Invoke-SoakRound ("round {0}" -f $round)
        if ($null -eq $text) {
            $failures++
            Write-Log "stopping: round $round failed; evidence in $failureLog"
            break
        }
        $sample = [pscustomobject]@{
            round          = $round
            workers        = Get-Metric $text 'workers'
            service_handle = Get-Metric $text 'service_handle'
            worker_handle  = Get-Metric $text 'worker_handle'
            service_ws_mb  = Get-Metric $text 'service_ws_mb'
            worker_ws_mb   = Get-Metric $text 'worker_ws_mb'
            generation     = Get-Metric $text 'generation'
            verified       = Get-Metric $text 'data_verified'
        }
        if ($null -eq $first) { $first = $sample }
        $last = $sample
        Write-Log ("  workers={0} svc_handle={1} wrk_handle={2} svc_ws={3}MB wrk_ws={4}MB generation={5} verified={6}" -f
            $sample.workers, $sample.service_handle, $sample.worker_handle, $sample.service_ws_mb,
            $sample.worker_ws_mb, $sample.generation, $sample.verified)

        if ($round % $CutEveryRounds -eq 0) {
            $cuts++
            Write-Log "cutting the power (cut $cuts) after round $round"
            & $vmrun -T ws -vp $VmPassword stop $Vmx hard 2>&1 | Out-Null
            Start-Sleep -Seconds 5
            & $vmrun -T ws -vp $VmPassword start $Vmx nogui 2>&1 | Out-Null
            if (-not (Wait-ForSsh $BootTimeoutSeconds)) { throw "the guest did not come back after cut $cuts" }
            if (-not (Wait-ForScp $BootTimeoutSeconds)) { throw "scp did not come back after cut $cuts" }
            Set-SoakMode 'verify'
            $verify = Invoke-SoakRound ("verify {0}" -f $cuts)
            if ($null -eq $verify) {
                $failures++
                Write-Log "stopping: post-cut verify $cuts failed; evidence in $failureLog"
                break
            }
            $verifyWorkers = Get-Metric $verify 'workers'
            Write-Log "  post-cut workers=$verifyWorkers"
            Set-SoakMode 'round'
        }
    }

    # End the long-lived console service the rounds reused, and record that it is gone.
    Set-SoakMode 'stop-service'
    $stopped = Invoke-SoakRound 'stop-service'
    if ($null -eq $stopped) { $failures++; Write-Log 'stopping: the service did not stop cleanly' }
    # Leave no stale mode behind: a later ctest invocation of the case must default to a round
    # and clean up after itself, not re-run `stop-service` because a file survived the soak.
    Invoke-Remote ("Remove-Item -LiteralPath 'C:\m5\soak\mode.txt','C:\m5\soak\driver.txt' " +
        "-Force -ErrorAction SilentlyContinue; 'ok'") | Out-Null
    $duration = if ($null -ne $first) { $round } else { 0 }
    $lines = @(
        "soak finished: rounds=$round cuts=$cuts failures=$failures",
        "duration_hours=$([math]::Round($Hours, 2)) max_rounds=$MaxRounds",
        "first: $($first | ConvertTo-Json -Compress)",
        "last:  $($last | ConvertTo-Json -Compress)"
    )
    Set-Content -LiteralPath $summary -Value ($lines -join "`n") -Encoding UTF8
    $lines | ForEach-Object { Write-Log $_ }
    if ($failures -ne 0) { exit 1 }
    exit 0
} catch {
    $detail = $_.Exception.Message
    if ($null -ne $_.InvocationInfo) {
        $detail += " || at " + $_.InvocationInfo.PositionMessage
    }
    Add-Content -LiteralPath $failureLog -Value ("[$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))] driver error: $detail") -Encoding UTF8
    Write-Log "driver error: $detail"
    exit 1
}
