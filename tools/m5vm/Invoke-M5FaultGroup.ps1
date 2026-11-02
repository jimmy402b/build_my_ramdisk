# Invoke-M5FaultGroup.ps1 -- runs the six fault cases as **one command**, with a clean guest
# between cases (R-04 delivery, plan B).
#
# The cases themselves refuse to run on a guest that still has a profile
# (`the guest already has 1 profile(s); reset it first`), and running them back to back in the
# general suite was measured to wedge the guest: `bundle_corruption` after a dirty guest ran
# for more than half an hour and took SSH with it.  Instead of teaching every case to reset the
# machine, this runner owns the reset:
#
#   for each case:  reset the guest -> run the case -> read its results artifact;
#                   if the case failed or the guest stopped answering, hard-reset the VM once
#                   and retry the case, then move on either way.
#
# Output: <OutDir>/fault-group.json (per-case exit codes and request ids) and
#         <OutDir>/fault-group.summary.txt.  Exit code 0 only when all six cases passed.
#
# **The reset is not the product under test.**  It removes profiles/operations and stops
# leftover processes so the *next* case starts from the same place the release rehearsal
# describes; the failure it prevents is a harness-state failure, not a product failure.

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
    [string]$Configuration = 'Release',
    [string]$OutDir = 'C:\tmp\m5-fault-group',
    [switch]$SkipStage,
    [string[]]$Only
)

$ErrorActionPreference = 'Stop'
if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'fault-group.log'
$jsonPath = Join-Path $OutDir 'fault-group.json'
$summaryPath = Join-Path $OutDir 'fault-group.summary.txt'
$sshTarget = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR', '-i', $KeyFile)

$cases = @('process_kills', 'service_kill', 'bundle_corruption',
           'stop_after_restore', 'hostile_acl', 'volume_full')
if ($Only) { $cases = @($cases | Where-Object { $Only -contains $_ }) }
if ($cases.Count -eq 0) { throw 'no fault case selected' }

function Write-Log([string]$line) {
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    Add-Content -LiteralPath $log -Value "[$stamp] $line" -Encoding UTF8
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

function Get-RemoteText([string]$path) {
    $out = Invoke-Remote ("if (Test-Path '$path') { Get-Content '$path' -Raw } else { 'MISSING' }")
    return (($out -join "`n"))
}

function Test-Ssh {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $probe = & ssh.exe @sshArgs $sshTarget 'cmd /c echo READY' 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $previous
    }
    return ($probe -match 'READY')
}

function Wait-Ssh([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        # Two consecutive answers: the first one can arrive while the boot is still settling.
        if (Test-Ssh) {
            Start-Sleep -Seconds 5
            if (Test-Ssh) { return $true }
        }
        Start-Sleep -Seconds 5
    }
    return $false
}

# Runs one guest script with a hard bound.  Start-Job gives the caller a way to abandon a wedged
# guest; ssh keepalives (in $sshArgs) make the job end by itself when the connection dies.
function Invoke-RemoteBounded([string]$script, [int]$seconds) {
    $job = Start-Job -ScriptBlock {
        param($text, $target, $arguments)
        @('') + @($text) | & ssh.exe @arguments $target 'powershell.exe -NoProfile -NonInteractive -Command -' 2>&1
    } -ArgumentList $script, $sshTarget, $sshArgs
    if (-not (Wait-Job $job -Timeout $seconds)) {
        Stop-Job $job -ErrorAction SilentlyContinue
        Remove-Job $job -Force -ErrorAction SilentlyContinue
        return $null
    }
    $output = Receive-Job $job
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    return $output
}

function Reset-Guest([string]$reason) {
    # **Blocking but bounded.**  Measured: `start /b` children die with the SSH session, so a
    # detached reset never ran; a plain blocking call hangs for ever on a wedged guest.  The job
    # + wait gives the reset 120 s and then lets the caller hard-reset the VM.
    Write-Log "reset ($reason)"
    $script = "& powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1 2>&1 | Select-Object -Last 1"
    $output = Invoke-RemoteBounded $script 120
    if ($null -eq $output) { Write-Log '  reset did not finish within 120 s'; return $false }
    foreach ($line in @($output)) { if ("$line".Trim()) { Write-Log ("  " + "$line".Trim()) } }
    return $true
}
function HardReset-Vm([string]$reason) {
    Write-Log "hard-resetting the VM ($reason)"
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $vmrun -T ws -vp $VmPassword stop $Vmx hard 2>&1 | Out-Null
        Start-Sleep -Seconds 8
        & $vmrun -T ws -vp $VmPassword start $Vmx nogui 2>&1 | Out-Null
    } finally {
        $ErrorActionPreference = $previous
    }
    return (Wait-Ssh 300)
}

function Invoke-FaultCase([string]$name) {
    # **The harness prints its request id with Write-Host**, which a normal pipeline capture
    # does not see (measured: `$output` held no request id while the console showed it).  The
    # results artifact is the authoritative output, so the newest `.results.json` written after
    # this run started is read and must name this case.
    $started = (Get-Date).ToUniversalTime().AddSeconds(-10)
    $output = & $harness -Run -Filter "^integration\.m5\.$name$" `
        -MinTimeoutSeconds 1200 -DeadlineSeconds 1800 *>&1 | Out-String
    $passed = $false
    $exits = @()
    $artifact = $null
    try {
        $listing = Invoke-Remote ("Get-ChildItem 'C:\m5\control' -Filter '*.results.json' | " +
            "Where-Object { `$_.LastWriteTimeUtc -ge [datetime]'$($started.ToString('o'))' } | " +
            "Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName")
        $artifact = ($listing | Where-Object { $_ -match '\.results\.json$' } | Select-Object -First 1)
    } catch {
        $artifact = $null
    }
    if ($artifact) {
        $json = (Get-RemoteText ($artifact.Trim())).Trim()
        if ($json -match '^\[|^\{') {
            try {
                $cases = @($json | ConvertFrom-Json)
                $exits = @($cases | ForEach-Object { $_.exit_code })
                $names = @($cases | ForEach-Object { $_.name })
                $bad = @($cases | Where-Object { $null -ne $_.exit_code -and [int]$_.exit_code -ne 0 })
                $passed = $cases.Count -ge 1 -and $bad.Count -eq 0 -and
                    ($names -contains "integration.m5.$name")
            } catch {
                $passed = $false
            }
        }
    }
    if (-not $passed -and $output -match '1 of 1 passed') { $passed = $true }
    return [pscustomobject]@{ passed = $passed; request = $artifact; exits = $exits }
}

try {
    if (-not $SkipStage) {
        Write-Log 'staging'
        & $harness -Stage -Configuration $Configuration 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'staging failed' }
    }

    $results = @()
    foreach ($name in $cases) {
        Write-Log "case $name"
        if (-not (Reset-Guest "before $name")) {
            if (-not (HardReset-Vm "the guest stopped answering before $name")) {
                $results += [pscustomobject]@{ case = $name; passed = $false; attempts = 0; request = $null; exits = @(); note = 'guest unreachable' }
                continue
            }
            [void](Reset-Guest "after the hard reset before $name")
        }
        $attempt = Invoke-FaultCase $name
        $attempts = 1
        if (-not $attempt.passed) {
            # One retry after a hard reset: a wedged guest is a harness-state failure, and the
            # retry is what tells the two apart.
            if (HardReset-Vm "retry $name after a failed attempt") {
                [void](Reset-Guest "retry $name")
                $attempt = Invoke-FaultCase $name
                $attempts = 2
            }
        }
        Write-Log ("  {0}: passed={1} attempts={2} exits={3}" -f
            $name, $attempt.passed, $attempts, (($attempt.exits) -join ','))
        $results += [pscustomobject]@{ case = $name; passed = $attempt.passed; attempts = $attempts
            request = $attempt.request; exits = $attempt.exits; note = '' }
    }

    $failed = @($results | Where-Object { -not $_.passed })
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    [pscustomobject]@{ finished_at = $stamp; cases = $results
        passed = $results.Count - $failed.Count; failed = $failed.Count } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding UTF8

    $lines = @("fault group: $($results.Count - $failed.Count) of $($results.Count) passed ($stamp)")
    foreach ($result in $results) {
        $lines += ("  {0,-20} {1} (attempts {2})" -f $result.case, $(if ($result.passed) { 'PASS' } else { 'FAIL' }), $result.attempts)
    }
    $lines | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    foreach ($line in $lines) { Write-Host $line }
    if ($failed.Count -ne 0) { throw ("fault group failed: " + (($failed | ForEach-Object { $_.case }) -join ', ')) }
    Write-Host 'READING: fault_group.result=PASS'
    exit 0
} finally {
    try { [void](Reset-Guest 'after the fault group') } catch { }
}
