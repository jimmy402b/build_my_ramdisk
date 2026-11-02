# Test-M5DriveLetter.ps1 -- host driver for integration.m5.drive_letter (R-19 follow-up).
#
# The case proves that the letter recorded at create is reused on every start, and refuses
# rather than silently re-lettering when the letter is occupied.  The reboot half cannot live
# inside a guest case, so this driver:
#   1. resets the guest and stages once;
#   2. runs the case in `stability` mode (install, create at a free letter, start, stop/start,
#      service restart, occupied-letter refusal) and reads the recorded letter;
#   3. hard-stops and starts the VM, waits for SSH, runs the case in `after-reboot` mode (start
#      the same disk, assert the same letter, then stop/forget/uninstall);
#   4. compares the two letters and prints one verdict line.

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
    [string]$Configuration = 'Release',
    [string]$OutDir = 'C:\tmp\m5-drive-letter',
    [switch]$SkipStage
)

$ErrorActionPreference = 'Stop'
if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }
if (-not $Vmx) { throw 'M5VM_VMX / -Vmx is required' }
if (-not $VmPassword) { throw 'M5VM_PASSWORD / -VmPassword is required for vmrun' }

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'drive-letter.log'
$sshTarget = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR', '-i', $KeyFile)

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

function Set-RemoteText([string]$path, [string]$text) {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($text))
    Invoke-Remote ("New-Item -ItemType Directory -Force -Path 'C:\m5\drive-letter' | Out-Null; " +
        "[System.IO.File]::WriteAllBytes('$path', [Convert]::FromBase64String('$b64')); 'ok'") | Out-Null
}

function Reset-Guest {
    Invoke-Remote ("& powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
        "'C:\m5\Reset-M5Guest.ps1' 2>&1 | Out-Null; 'reset-ok'") | Out-Null
}

function Wait-Ssh([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $probe = & ssh.exe @sshArgs $sshTarget 'cmd /c echo READY' 2>&1 | Out-String
        } finally {
            $ErrorActionPreference = $previous
        }
        if ($probe -match 'READY') { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}

function Run-Case([string]$mode) {
    Set-RemoteText 'C:\m5\drive-letter\mode.txt' $mode
    Write-Log "running the case in '$mode' mode"
    $output = & $harness -Run -Filter '^integration\.m5\.drive_letter$' `
        -MinTimeoutSeconds 1200 -DeadlineSeconds 1800 2>&1 | Out-String
    # **Verdict from the results artifact, not from the prose.**  Measured: parsing the harness
    # text reported FAIL on a run whose own line said `1 of 1 passed`; the artifact is what the
    # guest actually wrote and it carries per-case exit codes.
    $requestId = $null
    if ($output -match 'MYRD_VM_REQUEST_ID=([0-9a-f]+)') { $requestId = $matches[1] }
    $verdict = 'FAIL'
    if ($null -ne $requestId) {
        $json = (Get-RemoteText "C:\m5\control\$requestId.results.json").Trim()
        if ($json -match '^\[' -or $json -match '^\{') {
            try {
                $cases = @($json | ConvertFrom-Json)
                $bad = @($cases | Where-Object { $null -ne $_.exit_code -and [int]$_.exit_code -ne 0 })
                if ($cases.Count -ge 1 -and $bad.Count -eq 0) { $verdict = 'PASS' }
            } catch {
                $verdict = 'FAIL'
            }
        }
    }
    Write-Log ("$mode verdict: $verdict (request " + $(if ($requestId) { $requestId } else { '?' }) + ")")
    if ($verdict -ne 'PASS') {
        Write-Log (($output -split "`n" | Select-String -Pattern 'FAIL|exit=' | Select-Object -First 4) -join ' | ')
    }
    return $verdict
}

try {
    Write-Log 'resetting the guest'
    Reset-Guest
    if (-not $SkipStage) {
        Write-Log 'staging'
        & $harness -Stage -Configuration $Configuration 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'staging failed' }
    }

    $stability = Run-Case 'stability'
    $first = (Get-RemoteText 'C:\m5\drive-letter\readings.json').Trim()
    Write-Log ("stability readings: " + ($first -replace "`r?`n", ' '))
    if ($stability -ne 'PASS') { throw 'the stability half failed; the reboot half needs its disk' }

    Write-Log 'hard-resetting the VM'
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $vmrun -T ws -vp $VmPassword stop $Vmx hard 2>&1 | Out-Null
        Start-Sleep -Seconds 8
        & $vmrun -T ws -vp $VmPassword start $Vmx nogui 2>&1 | Out-Null
    } finally {
        $ErrorActionPreference = $previous
    }
    if (-not (Wait-Ssh 300)) { throw 'the guest did not come back after the hard reset' }
    Write-Log 'guest is back'

    $reboot = Run-Case 'after-reboot'
    $second = (Get-RemoteText 'C:\m5\drive-letter\readings.json').Trim()
    Write-Log ("after-reboot readings: " + ($second -replace "`r?`n", ' '))

    $letterA = $null
    $letterB = $null
    if ($first -match '"letter"\s*:\s*"([A-Z]:)"') { $letterA = $matches[1] }
    if ($second -match '"letter"\s*:\s*"([A-Z]:)"') { $letterB = $matches[1] }
    Write-Log ("letter before reboot=$letterA after reboot=$letterB")

    $failed = @()
    if ($stability -ne 'PASS') { $failed += 'stability' }
    if ($reboot -ne 'PASS') { $failed += 'after-reboot' }
    if ($null -eq $letterA -or $null -eq $letterB) { $failed += 'letter missing' }
    elseif ($letterA -ne $letterB) { $failed += "letter changed ($letterA -> $letterB)" }
    if ($failed.Count -ne 0) { throw ("drive-letter verification failed: " + ($failed -join '; ')) }

    Write-Log "PASS: the disk keeps $letterA across stop/start, a service restart and a VM reboot; an occupied letter is refused"
    Write-Host "READING: drive_letter.verdict=PASS letter=$letterA"
    exit 0
} finally {
    try {
        Write-Log 'cleanup: reset the guest'
        Reset-Guest
    } catch {
        Write-Log ("cleanup reset failed: " + $_.Exception.Message)
    }
}
