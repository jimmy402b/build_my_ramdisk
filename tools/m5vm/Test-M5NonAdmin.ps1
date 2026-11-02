# Test-M5NonAdmin.ps1 -- the R-11 non-elevated e2e, driven from the host (R-11).
#
# **Why a scheduled task.**  Every other way to get a non-elevated process on this guest failed:
#   * `Start-Process -Credential` (CreateProcessWithLogonW) -- even `myrd-cli --version` dies with
#     0xC0000142 (STATUS_DLL_INIT_FAILED) in the agent's session;
#   * `vmrun runProgramInGuest` starts processes, but with a **network** logon, and the public
#     pipe's SDDL denies network logons by design (`(D;;GA;;;NU)`), so the CLI answers
#     "the service is not available" (exit 3);
#   * a Limited scheduled task would not run until the user was granted `SeBatchLogonRight`
#     (`LastTaskResult=267011`); with the right granted it runs as the standard user.
#
# A batch logon is a local logon, so the pipe accepts it.  This script:
#   1. resets the guest, creates a standard user and grants it the batch-logon right;
#   2. runs one agent case (`integration.m5.nonadmin_prepare`) that starts the service and
#      creates two profiles -- one owned by that user, one owned by the administrator;
#   3. runs the GUI's `--self-check`, `disk list`, `disk show` (own and other) and a mutating
#      `disk create` as that user through a scheduled task, capturing exit code and output;
#   4. asserts the acceptance: reads work, the other profile and the mutation are refused with
#      exit 4 and a clear message, and every process exits (nothing crashes or hangs);
#   5. cleans up (forget the profiles, stop the service, remove the user).
#
# Guest scripts are single-quoted here-strings with `__TOKEN__` placeholders, and task wrappers
# are written as base64: measured, a quoted executable path that travelled through nested
# `-EncodedCommand`/here-string layers arrived split across lines and cmd reported a usage error.

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Configuration = 'Release',
    [string]$GuestUser = 'm5limited',
    [string]$GuestPassword = 'M5Limited!2026x',
    [string]$OutDir = 'C:\tmp\m5-nonadmin',
    [switch]$SkipStage
)

$ErrorActionPreference = 'Stop'

if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'nonadmin.log'
$sshTarget = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR', '-i', $KeyFile)
$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
$nonadmin = 'C:\m5\nonadmin'
$taskName = 'm5-nonadmin-step'

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

# **Long scripts travel as files.**  Measured: a ~1.5 KiB setup script piped to `-Command -`
# produced no output and no side effect, while the same text ran when given as a file; short
# probes keep using stdin.  This is the same "write where it is read" lesson as the soak case.
function Invoke-RemoteFile([string]$script) {
    $local = Join-Path $OutDir ('remote-' + [guid]::NewGuid().ToString('N') + '.ps1')
    Set-Content -LiteralPath $local -Value $script -Encoding UTF8
    $remote = 'C:/m5/control/' + [System.IO.Path]::GetFileName($local)
    & scp.exe @sshArgs $local "$($sshTarget):$remote" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "scp of the remote script failed ($LASTEXITCODE)" }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & ssh.exe @sshArgs $sshTarget ("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + $remote) 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    Remove-Item -LiteralPath $local -Force -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "remote script failed ($code): $($output -join "`n")" }
    return $output
}

function Get-RemoteText([string]$path) {
    $out = Invoke-Remote ("if (Test-Path '$path') { Get-Content '$path' -Raw } else { 'MISSING' }")
    return (($out -join "`n"))
}

function Set-RemoteAscii([string]$path, [string]$text) {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes($text))
    Invoke-Remote ("[System.IO.File]::WriteAllBytes('$path', [Convert]::FromBase64String('$b64')); 'ok'") | Out-Null
}

function Invoke-AsStandardUser([string]$label, [string]$exe, [string[]]$arguments) {
    $out = "$nonadmin\$label.out.txt"
    $err = "$nonadmin\$label.err.txt"
    $exitFile = "$nonadmin\$label.exit.txt"
    $wrapper = "$nonadmin\$label.cmd"
    $quoted = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $ladder = 'if errorlevel 5 (echo 5 > ' + $exitFile + ') else if errorlevel 4 (echo 4 > ' +
        $exitFile + ') else if errorlevel 3 (echo 3 > ' + $exitFile + ') else if errorlevel 2 ' +
        '(echo 2 > ' + $exitFile + ') else if errorlevel 1 (echo 1 > ' + $exitFile + ') else ' +
        '(echo 0 > ' + $exitFile + ')'
    $text = "@echo off`r`n" +
        '"' + $exe + '" ' + $quoted + ' > "' + $out + '" 2> "' + $err + '"' + "`r`n" + $ladder
    Set-RemoteAscii $wrapper $text
    Invoke-Remote ("Remove-Item -LiteralPath '$out','$err','$exitFile' -Force " +
        "-ErrorAction SilentlyContinue; 'ok'") | Out-Null

    $registerTemplate = @'
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/c __WRAPPER__'
Register-ScheduledTask -TaskName '__TASK__' -Action $action -User '__USER__' -Password '__PASS__' -RunLevel Limited -Force | Out-Null
Start-ScheduledTask -TaskName '__TASK__'
'ok'
'@
    $register = $registerTemplate.Replace('__WRAPPER__', $wrapper).
        Replace('__TASK__', $taskName).Replace('__USER__', $GuestUser).
        Replace('__PASS__', $GuestPassword)
    Invoke-Remote $register | Out-Null

    $until = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $until) {
        $probe = Invoke-Remote ("if (Test-Path '$exitFile') { 'yes' } else { 'no' }")
        if (($probe -join ' ') -match 'yes') { break }
        Start-Sleep -Seconds 1
    }
    $info = Invoke-Remote ("if (Test-Path '$exitFile') { 'exit=' + (Get-Content '$exitFile' -Raw).Trim() } else { 'MISSING' }")
    $joined = ($info -join "`n")
    $match = [regex]::Match($joined, 'exit=([0-9]+)')
    Invoke-Remote ("Unregister-ScheduledTask -TaskName '$taskName' -Confirm:`$false -ErrorAction SilentlyContinue; 'ok'") | Out-Null
    if (-not $match.Success) { throw "the standard-user run '$label' never produced an exit code" }
    $exitCode = [int]$match.Groups[1].Value
    $outText = Get-RemoteText $out
    $errText = Get-RemoteText $err
    Write-Log ("{0}: exit={1}" -f $label, $exitCode)
    $brief = (($outText + "`n" + $errText) -replace "`r?`n", ' | ')
    if ($brief.Length -gt 400) { $brief = $brief.Substring(0, 400) + '...' }
    Write-Log ("  " + $brief)
    return [pscustomobject]@{ exit = $exitCode; output = ($outText + "`n" + $errText) }
}

$setupTemplate = @'
$ErrorActionPreference='Stop'
if (Get-LocalUser -Name '__USER__' -ErrorAction SilentlyContinue) { Remove-LocalUser -Name '__USER__' }
$secure = ConvertTo-SecureString '__PASS__' -AsPlainText -Force
New-LocalUser -Name '__USER__' -Password $secure -PasswordNeverExpires -AccountNeverExpires | Out-Null
New-Item -ItemType Directory -Force -Path '__DIR__' | Out-Null
& icacls.exe '__DIR__' /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
$sid = (Get-LocalUser -Name '__USER__').SID.Value
& secedit.exe /export /cfg '__DIR__\before.inf' /areas USER_RIGHTS | Out-Null
$text = Get-Content '__DIR__\before.inf' -Raw
if ($text -notmatch [regex]::Escape($sid)) {
    $m = [regex]::Match($text, '(?m)^SeBatchLogonRight\s*=\s*(.*)$')
    $current = if ($m.Success) { $m.Groups[1].Value.Trim() } else { '' }
    $new = 'SeBatchLogonRight = ' + $(if ($current) { $current + ',' } else { '' }) + '*' + $sid
    if ($m.Success) { $text = $text.Replace($m.Value, $new) } else { $text = $text + "`r`n" + $new }
    [System.IO.File]::WriteAllText('__DIR__\after.inf', $text, [System.Text.Encoding]::Unicode)
    & secedit.exe /configure /db '__DIR__\sepol.sdb' /cfg '__DIR__\after.inf' /areas USER_RIGHTS | Out-Null
}
Set-Content -LiteralPath '__DIR__\sid.txt' -Value $sid -Encoding ASCII
$sid
'@

$cleanupTemplate = @'
Get-ScheduledTask -TaskName '__TASK__' -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false
if (Get-LocalUser -Name '__USER__' -ErrorAction SilentlyContinue) { Remove-LocalUser -Name '__USER__' }
Remove-Item -LiteralPath '__DIR__' -Recurse -Force -ErrorAction SilentlyContinue
'ok'
'@

try {
    Write-Log 'resetting the guest'
    Invoke-Remote ("& powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
        "'C:\m5\Reset-M5Guest.ps1' 2>&1 | Select-Object -Last 2") | ForEach-Object { Write-Log ("  " + $_.Trim()) }

    Write-Log "creating $GuestUser and granting SeBatchLogonRight"
    $setup = $setupTemplate.Replace('__USER__', $GuestUser).
        Replace('__PASS__', $GuestPassword).Replace('__DIR__', $nonadmin)
    $setupOutput = (Invoke-RemoteFile $setup) -join "`n"
    $sidText = (Get-RemoteText "$nonadmin\sid.txt").Trim()
    $sidMatch = [regex]::Match($sidText, 'S-1-5-21-[0-9-]+')
    if (-not $sidMatch.Success) { throw "the setup wrote no SID: '$sidText' (output: $setupOutput)" }
    $sid = $sidMatch.Value
    Write-Log ("  sid=" + $sid)

    if (-not $SkipStage) {
        Write-Log 'staging'
        & $harness -Stage -Configuration $Configuration 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'staging failed' }
    }

    Write-Log 'preparing two profiles (agent)'
    Set-RemoteAscii "$nonadmin\mode.txt" 'prepare'
    & $harness -Run -Filter '^integration\.m5\.nonadmin_prepare$' -TimeoutOverrideSeconds 1800 2>&1 | Out-Null
    $profiles = (Get-RemoteText "$nonadmin\profiles.json") | ConvertFrom-Json
    Write-Log ("  owned=" + $profiles.owned + " admin=" + $profiles.admin)

    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $binDir = Join-Path $repoRoot "out\build\msvc\src\$Configuration"
    $cli = Join-Path $binDir 'myrd-cli.exe'
    $gui = Join-Path $binDir 'myrd-gui.exe'

    $results = @{}
    $results['gui-selfcheck'] = Invoke-AsStandardUser 'gui-selfcheck' $gui @('--self-check')
    $results['cli-list'] = Invoke-AsStandardUser 'cli-list' $cli @('disk', 'list')
    $results['cli-show-own'] = Invoke-AsStandardUser 'cli-show-own' $cli @('disk', 'show', [string]$profiles.owned)
    $results['cli-show-other'] = Invoke-AsStandardUser 'cli-show-other' $cli @('disk', 'show', [string]$profiles.admin)
    $results['cli-create'] = Invoke-AsStandardUser 'cli-create' $cli @('disk', 'create', '--name', 'nope',
        '--bundle', "$nonadmin\bundle-nope", '--size', '64MiB', '--mount', 'U:')

    $failed = @()
    if ($results['gui-selfcheck'].exit -ne 0) { $failed += 'gui-selfcheck exit' }
    if ($results['gui-selfcheck'].output -notmatch 'self-check:') { $failed += 'gui-selfcheck output' }
    if ($results['cli-list'].exit -ne 0) { $failed += 'cli-list exit' }
    if ($results['cli-list'].output -notmatch [regex]::Escape($profiles.owned)) { $failed += 'cli-list owned' }
    if ($results['cli-list'].output -match [regex]::Escape($profiles.admin)) { $failed += 'cli-list leaked admin' }
    if ($results['cli-show-own'].exit -ne 0) { $failed += 'cli-show-own exit' }
    if ($results['cli-show-other'].exit -ne 4) { $failed += 'cli-show-other exit' }
    if ($results['cli-show-other'].output -notmatch 'not authorized|another user|permission|denied') {
        $failed += 'cli-show-other message'
    }
    if ($results['cli-create'].exit -ne 4) { $failed += 'cli-create exit' }
    if ($results['cli-create'].output -notmatch 'administrator|permission|not authorized|denied') {
        $failed += 'cli-create message'
    }
    if ($failed.Count -ne 0) { throw ("non-elevated assertions failed: " + ($failed -join '; ')) }

    Write-Log 'PASS: the standard user reads its own disk, is refused elsewhere, and nothing crashed'
    Write-Host 'READING: nonadmin.result=PASS'
    exit 0
} finally {
    try {
        Write-Log 'cleanup: forget the profiles and stop the service (agent)'
        Set-RemoteAscii "$nonadmin\mode.txt" 'cleanup'
        & $harness -Run -Filter '^integration\.m5\.nonadmin_prepare$' -TimeoutOverrideSeconds 1800 2>&1 | Out-Null
    } catch {
        Write-Log ("cleanup case failed: " + $_.Exception.Message)
    }
    try {
        Write-Log 'cleanup: remove the standard user and the scratch directory'
        $cleanup = $cleanupTemplate.Replace('__TASK__', $taskName).
            Replace('__USER__', $GuestUser).Replace('__DIR__', $nonadmin)
        Invoke-RemoteFile $cleanup | Out-Null
    } catch {
        Write-Log ("cleanup failed: " + $_.Exception.Message)
    }
}
