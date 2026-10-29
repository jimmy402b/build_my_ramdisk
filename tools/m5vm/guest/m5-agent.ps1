# Guest-side agent for T1-01.
#
# This script MUST run with highest privileges and NOT as a network logon.  It
# is started by the scheduled task `MyRamDisk-M5-Agent`; see
# docs/operations/m5-vm-harness.md and the header of Invoke-M5Vm.ps1 for why an
# ssh session cannot do this itself.
#
# Its job is deliberately small: take a request, run it, drop a response.  How
# tests are executed is Invoke-M5Tests.ps1's business.
#
#   -Drain                  process every unfinished request once, then exit
#                           (this is what the scheduled task runs; the task's
#                           command line is fixed, so the request id cannot be
#                           passed through it)
#   -Once -RequestId <id>   run exactly that request and exit (manual use)
#   (no arguments)          poll mode: keep watching for new requests
#
# Convention, matching the request/response convention this project already
# established for the VM:
#   <id>.request.ps1   input
#   <id>.response.txt  output, framed with MYRD_VM_* markers
#   <id>.done          completion marker, written LAST; the host waits on it

[CmdletBinding()]
param(
    [string]$ControlRoot = 'C:\m5\control',
    [switch]$Drain,
    [switch]$Once,
    [string]$RequestId,
    [int]$TimeoutSeconds = 3600,
    [int]$PollSeconds = 3
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $ControlRoot | Out-Null
$logPath = Join-Path $ControlRoot 'm5-agent.log'

function Write-Log([string]$message) {
    $line = '{0} {1}' -f ([DateTime]::UtcNow.ToString('s')), $message
    Add-Content -LiteralPath $logPath -Value $line -Encoding utf8
}

function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-OneRequest([string]$id) {
    $request = Join-Path $ControlRoot "$id.request.ps1"
    if (-not (Test-Path -LiteralPath $request)) { Write-Log "no request for $id"; return }

    $response = Join-Path $ControlRoot "$id.response.txt"
    $done = Join-Path $ControlRoot "$id.done"
    Remove-Item -LiteralPath $done -Force -ErrorAction SilentlyContinue

    $started = [DateTime]::UtcNow
    $exit = 'UNKNOWN'
    $stdout = ''
    $stderr = ''

    # Run the request in a child job so a wedged request cannot hold the agent
    # forever.  Bounded execution is the whole reason this indirection exists.
    try {
        $job = Start-Job -ScriptBlock {
            param($script)
            $ErrorActionPreference = 'Continue'
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script 2>&1 | Out-String
        } -ArgumentList $request

        if (Wait-Job $job -Timeout $TimeoutSeconds) {
            $stdout = [string](Receive-Job $job)
            $exit = 0
        } else {
            Stop-Job $job -ErrorAction SilentlyContinue
            $stderr = "agent: request exceeded $TimeoutSeconds s and was stopped"
            $exit = 'TIMEOUT'
        }
        Remove-Job $job -Force -ErrorAction SilentlyContinue
    } catch {
        $stderr = "agent: $_"
        $exit = 'AGENT_ERROR'
    }

    $finished = [DateTime]::UtcNow
    $body = @(
        "MYRD_VM_REQUEST_ID=$id"
        "MYRD_VM_STARTED_UTC=$($started.ToString('o'))"
        "MYRD_VM_FINISHED_UTC=$($finished.ToString('o'))"
        "MYRD_VM_EXIT=$exit"
        "MYRD_VM_ELEVATED=$(Test-Elevated)"
        "MYRD_VM_SESSION_ID=$((Get-Process -Id $PID).SessionId)"
        '--- STDOUT ---'
        $stdout
        '--- STDERR ---'
        $stderr
    ) -join "`r`n"

    Set-Content -LiteralPath $response -Value $body -Encoding utf8
    Set-Content -LiteralPath $done -Value 'done' -Encoding ascii
    Write-Log "request $id finished exit=$exit elevated=$(Test-Elevated)"
}

function Invoke-PendingRequests {
    # **One request per trigger, chosen by the host.**  The old drain ran every
    # unfinished `*.request.ps1` in the queue, so a request left behind by an earlier
    # run was replayed before the new one: measured, a full-suite request sat behind
    # old requests and the host's 40-minute deadline expired while the guest was
    # still working through them.  The host now writes `pending.id` before it
    # triggers the task, and the agent processes only that id.  The fallback (no
    # pending.id) keeps manual drain useful.
    $only = $null
    $pendingIdFile = Join-Path $ControlRoot 'pending.id'
    if (Test-Path -LiteralPath $pendingIdFile) {
        $only = ([string](Get-Content -LiteralPath $pendingIdFile -Raw)).Trim()
        Remove-Item -LiteralPath $pendingIdFile -Force -ErrorAction SilentlyContinue
        Write-Log "pending.id selects $only"
    }
    $pending = Get-ChildItem -LiteralPath $ControlRoot -Filter '*.request.ps1' -ErrorAction SilentlyContinue
    $ran = 0
    foreach ($file in $pending) {
        $id = $file.Name -replace '\.request\.ps1$', ''
        if ($null -ne $only -and $only -ne '' -and $id -ne $only) {
            Write-Log "skipping $id (not the current request)"
            continue
        }
        if (Test-Path -LiteralPath (Join-Path $ControlRoot "$id.done")) { continue }
        Invoke-OneRequest -id $id
        $ran++
    }
    return $ran
}

Write-Log ('agent start elevated={0} drain={1} once={2} request={3}' -f (Test-Elevated), $Drain, $Once, $RequestId)

if ($Drain) {
    # The scheduled task's one job.  Requests may have been dropped before the
    # task was triggered, so drain whatever is unfinished and exit.
    $ran = Invoke-PendingRequests
    Write-Log "drain complete, $ran request(s) processed"
    exit 0
}

if ($Once) {
    if ([string]::IsNullOrWhiteSpace($RequestId)) { Write-Log 'Once requires -RequestId'; exit 2 }
    Invoke-OneRequest -id $RequestId
    exit 0
}

Write-Log "polling $ControlRoot every $PollSeconds s"
while ($true) {
    [void](Invoke-PendingRequests)
    Start-Sleep -Seconds $PollSeconds
}
