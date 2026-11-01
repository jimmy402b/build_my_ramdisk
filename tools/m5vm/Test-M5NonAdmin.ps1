# Test-M5NonAdmin.ps1 -- the R-11 non-elevated e2e, driven from the host (R-11).
#
# **Why the host drives it.**  The agent runs in the guest's session 0; every mechanism tried
# there failed to launch a non-elevated process (Start-Process -Credential 0xC0000142, a
# Limited scheduled task never ran, CreateProcessAsUser access-violated the child, and `runas
# /trustlevel` works from an interactive session but not from that one).  `vmrun
# runProgramInGuest` does work: it authenticates to VMware Tools as a named guest account and
# starts the process with that account's ordinary, non-elevated token.
#
# Flow:
#   1. reset the guest and create a standard local user with a known password;
#   2. one agent case (`integration.m5.nonadmin_prepare`) starts the service and creates two
#      profiles: one owned by that user, one owned by the administrator;
#   3. this script runs the GUI's `--self-check`, `disk list`, `disk show` (own and other) and
#      a mutating `disk create` as the standard user, capturing each exit code and output;
#   4. cleanup forgets the profiles and removes the user.
#
# The assertions are the acceptance: the user reads its own disk, is refused another's and is
# refused a mutation with exit 4, and every process exits (nothing crashes or hangs).

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Vmx = $env:M5VM_VMX,
    [string]$VmPassword = $env:M5VM_PASSWORD,
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
if (-not $Vmx) { throw 'M5VM_VMX / -Vmx is required' }
if (-not $VmPassword) { throw 'M5VM_PASSWORD / -VmPassword is required for vmrun' }

$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path -LiteralPath $vmrun)) { throw "vmrun not found at $vmrun" }
if (-not (Test-Path -LiteralPath $Vmx)) { throw "the .vmx was not found: $Vmx" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'nonadmin.log'
$sshTarget = "$VmUser@$VmHost"
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
             '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR', '-i', $KeyFile)
$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
$nonadmin = 'C:\m5\nonadmin'

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

function Set-RemoteText([string]$path, [string]$text) {
    # Base64 so no quoting rule can reinterpret the content.
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $b64 = [Convert]::ToBase64String($bytes)
    Invoke-Remote ("[System.IO.File]::WriteAllBytes('$path', " +
        "[Convert]::FromBase64String('$b64')); 'ok'") | Out-Null
}

function Get-RemoteText([string]$path) {
    $out = Invoke-Remote ("if (Test-Path '$path') { Get-Content '$path' -Raw } else { 'MISSING' }")
    return (($out -join "`n"))
}

function Invoke-GuestOp([string]$program, [string[]]$arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $vmrun -T ws -vp $VmPassword -gu $GuestUser -gp $GuestPassword `
            runProgramInGuest $Vmx -noWait $program @arguments 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) { throw "vmrun runProgramInGuest failed ($code): $output" }
}

function Wait-RemoteFile([string]$path, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $exists = Invoke-Remote "if (Test-Path '$path') { 'yes' } else { 'no' }"
        if (($exists -join ' ') -match 'yes') { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

function Get-RemoteJson([string]$path) {
    $text = Get-RemoteText $path
    if ($text -match 'MISSING') { throw "$path is missing" }
    return ($text | ConvertFrom-Json)
}

try {
    # ---- 1. reset, create the standard user ----------------------------------
    Write-Log 'resetting the guest'
    Invoke-Remote ("& powershell.exe -NoProfile -ExecutionPolicy Bypass -File " +
        "'C:\m5\Reset-M5Guest.ps1' 2>&1 | Select-Object -Last 3") | ForEach-Object { Write-Log ("  " + $_.Trim()) }

    Write-Log "creating the standard user $GuestUser"
    $create = Invoke-Remote @"
`$ErrorActionPreference='Stop'
if (Get-LocalUser -Name '$GuestUser' -ErrorAction SilentlyContinue) { Remove-LocalUser -Name '$GuestUser' }
`$secure = ConvertTo-SecureString '$GuestPassword' -AsPlainText -Force
New-LocalUser -Name '$GuestUser' -Password `$secure -PasswordNeverExpires -AccountNeverExpires | Out-Null
New-Item -ItemType Directory -Force -Path '$nonadmin' | Out-Null
& icacls.exe '$nonadmin' /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
Set-Content -LiteralPath '$nonadmin\sid.txt' -Value (Get-LocalUser -Name '$GuestUser').SID.Value -Encoding ASCII
'sid=' + (Get-Content '$nonadmin\sid.txt' -Raw).Trim()
"@
    Write-Log ("  " + ($create -join ' '))

    if (-not $SkipStage) {
        Write-Log 'staging'
        & $harness -Stage -Configuration $Configuration 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'staging failed' }
    }

    # ---- 2. agent prepares the two profiles ---------------------------------
    Write-Log 'preparing two profiles (agent)'
    Set-RemoteText "$nonadmin\mode.txt" 'prepare'
    $prepare = & $harness -Run -Filter '^integration\.m5\.nonadmin_prepare$' `
        -TimeoutOverrideSeconds 1800 2>&1 | Out-String
    $profiles = Get-RemoteJson "$nonadmin\profiles.json"
    Write-Log ("  owned=" + $profiles.owned + " admin=" + $profiles.admin)
    $ownerFile = Get-RemoteText "$nonadmin\sid.txt"
    Write-Log ("  sid=" + $ownerFile.Trim())

    # ---- 3. run the GUI and CLI as the standard user ------------------------
    # The staged tree sits at the same path on the guest, so one repo-relative path serves
    # both sides; a hard-coded checkout path would break the non-mirror layout (R-17b).
    $repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $binDir = Join-Path $repoRoot "out\build\msvc\src\$Configuration"
    $cli = Join-Path $binDir 'myrd-cli.exe'
    $gui = Join-Path $binDir 'myrd-gui.exe'
    $steps = [ordered]@{
        'gui-selfcheck' = @{ exe = $gui; args = @('--self-check') }
        'cli-list'      = @{ exe = $cli; args = @('disk', 'list') }
        'cli-show-own'  = @{ exe = $cli; args = @('disk', 'show', [string]$profiles.owned) }
        'cli-show-other' = @{ exe = $cli; args = @('disk', 'show', [string]$profiles.admin) }
        'cli-create'    = @{ exe = $cli; args = @('disk', 'create', '--name', 'nope',
                              '--bundle', "$nonadmin\bundle-nope", '--size', '64MiB', '--mount', 'U:') }
    }
    $results = @{}
    foreach ($label in $steps.Keys) {
        $step = $steps[$label]
        $quoted = ($step.args | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join ' '
        $script = "& '" + $step.exe + "' " + $quoted + " > '$nonadmin\$label.out' 2> '$nonadmin\$label.err'`r`n" +
            "Set-Content -LiteralPath '$nonadmin\$label.exit' -Value `$LASTEXITCODE -Encoding ASCII"
        Set-RemoteText "$nonadmin\$label.ps1" $script
        Invoke-Remote ("Remove-Item -LiteralPath '$nonadmin\$label.exit','$nonadmin\$label.out','$nonadmin\$label.err' " +
            "-Force -ErrorAction SilentlyContinue; 'ok'") | Out-Null
        Invoke-GuestOp 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$nonadmin\$label.ps1")
        if (-not (Wait-RemoteFile "$nonadmin\$label.exit" 120)) {
            throw "the guest run '$label' never finished (no exit file)"
        }
        $exitText = (Get-RemoteText "$nonadmin\$label.exit").Trim()
        $outText = Get-RemoteText "$nonadmin\$label.out"
        $errText = Get-RemoteText "$nonadmin\$label.err"
        $results[$label] = [pscustomobject]@{
            exit = [int]$exitText
            output = ($outText + "`n" + $errText)
        }
        Write-Log ("{0}: exit={1}" -f $label, $exitText)
        Write-Log ("  " + (($results[$label].output -replace "`r?`n", ' | ')))
    }

    # ---- 4. assertions ------------------------------------------------------
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
    # ---- 5. cleanup --------------------------------------------------------
    try {
        Write-Log 'cleanup: forget the profiles and stop the service (agent)'
        Set-RemoteText "$nonadmin\mode.txt" 'cleanup'
        & $harness -Run -Filter '^integration\.m5\.nonadmin_prepare$' `
            -TimeoutOverrideSeconds 1800 2>&1 | Out-Null
    } catch {
        Write-Log ("cleanup case failed: " + $_.Exception.Message)
    }
    try {
        Write-Log 'cleanup: remove the standard user'
        Invoke-Remote ("if (Get-LocalUser -Name '$GuestUser' -ErrorAction SilentlyContinue) { " +
            "Remove-LocalUser -Name '$GuestUser' }; Remove-Item -LiteralPath '$nonadmin' -Recurse -Force " +
            "-ErrorAction SilentlyContinue; 'ok'") | Out-Null
    } catch {
        Write-Log ("cleanup user failed: " + $_.Exception.Message)
    }
}
