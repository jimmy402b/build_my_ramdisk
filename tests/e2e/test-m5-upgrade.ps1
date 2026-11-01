# integration.m5.upgrade -- install, overwrite, uninstall keeping data, reinstall, and a dirty
# uninstall, with the same disk surviving all of it.
#
# **Guest-only.**  It installs a real service, creates a real worker and AIM disk, and removes
# and reinstalls the service.
#
# What R-08 asks for, in the order this case proves it:
#   1. an overwrite install over a live install succeeds, the service comes back, and the disk
#      the previous install was serving is still registered and still starts;
#   2. an uninstall that keeps the data record removes the service and the files but not the
#      record, and a reinstall finds the same disk;
#   3. an uninstall while the disk is *running* takes the disk down before it removes the
#      service (no worker, no volume left), keeps the record, and a reinstall starts the disk
#      with its committed bytes;
#   4. the accepted upgrade path is reinstall, not an in-place version rollback.
#
# The marker is written before the first checkpoint, so every later start must return it.

param(
    [string]$Configuration = 'Release',
    [string]$ServiceName = 'MyRamDiskService'
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.upgrade installs a real service, worker and AIM disk, and may only ' +
        'run on the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = Join-Path $repo "out/build/msvc/src/$Configuration"
$installScript = Join-Path $repo 'tools/install/Install-MyRamDisk.ps1'
$uninstallScript = Join-Path $repo 'tools/install/Uninstall-MyRamDisk.ps1'
$installDir = 'C:\Program Files\MyRamDisk'
$dataDir = 'C:\ProgramData\MyRamDisk'
$cli = Join-Path $installDir 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @($installScript, $uninstallScript, $source)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing: $path" }
}

$workRoot = 'C:\m5\upgrade-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$markerPath = 'T:\upgrade-marker.txt'
$payload = 'committed before the upgrade ' + [guid]::NewGuid().ToString('N')
$mount = 'T:'
$serviceLog = Join-Path $workRoot 'service.log'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Invoke-Script([string]$path, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $started = Get-Date
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $path @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
        return [pscustomobject]@{
            output = $output
            exit   = $exit
            ms     = [int]((Get-Date) - $started).TotalMilliseconds
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Invoke-Cli([string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $started = Get-Date
        $output = & $cli @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
        return [pscustomobject]@{
            output = $output
            exit   = $exit
            ms     = [int]((Get-Date) - $started).TotalMilliseconds
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Wait-ForService([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $state = & sc.exe query $ServiceName 2>&1 | Out-String
        if ($state -match 'RUNNING') {
            $list = Invoke-Cli @('disk', 'list')
            if ($list.exit -eq 0) { return $list }
        }
        Start-Sleep -Seconds 1
    }
    return $null
}

function Get-DiskLine([string]$letter) {
    $listing = Invoke-Cli @('disk', 'list')
    if ($listing.exit -ne 0) { return $null }
    return (($listing.output -split "`n") |
        Where-Object { $_ -match ('\s' + [regex]::Escape($letter)) } |
        Select-Object -First 1)
}

function Get-Field([string]$line, [string]$name) {
    if ($null -eq $line) { return $null }
    $match = [regex]::Match($line, $name + '=([^\s]+)')
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text,
        '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
    if ($match.Success) { return $match.Value }
    return $null
}

# Polls `operation show` until the operation leaves `running`.  Returns the terminal word.
function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $last = ''
    while ((Get-Date) -lt $deadline) {
        $show = Invoke-Cli @('operation', 'show', $operationId)
        if ($show.exit -ne 0) { Fail "operation show failed: $($show.output)" }
        $last = ($show.output -split "`n" | Where-Object { $_ -match ': (succeeded|failed|cancelled|running|pending)' } |
            Select-Object -Last 1)
        if ($last -match ': (succeeded|failed|cancelled)') {
            return ([regex]::Match($last, ': (\w+)')).Groups[1].Value
        }
        Start-Sleep -Milliseconds 500
    }
    Fail "the operation $operationId did not reach a terminal state within ${seconds}s: $last"
}

function Assert-ProcessesGone([string]$label) {
    $left = @(Get-Process myrd-service, myrd-m5-worker -ErrorAction SilentlyContinue)
    Write-Reading ("$label.processes_left") ([string]$left.Count)
    if ($left.Count -ne 0) { Fail "$label left $($left.Count) M5 process(es) behind" }
}

function Start-DiskAndWait([string]$profileId, [string]$label) {
    $start = Invoke-Cli @('disk', 'start', $profileId)
    if ($start.exit -ne 0) { Fail "${label}: start failed: $($start.output)" }
    $id = Get-UuidFromText $start.output
    if ($null -ne $id) {
        $state = Wait-ForTerminal $id 300
        if ($state -ne 'succeeded') { Fail "${label}: start ended $state" }
    }
    $runtime = Get-Field (Get-DiskLine $mount) 'runtime'
    Write-Reading ("$label.runtime") ([string]$runtime)
    if ($runtime -ne 'Running') { Fail "${label}: the disk did not reach Running" }
    Write-Reading ("$label.marker_intact") $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })
    if ((Get-Content -LiteralPath $markerPath -Raw) -ne $payload) { Fail "${label}: the marker bytes changed" }
}

function Stop-DiskAndWait([string]$profileId, [string]$label) {
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    if ($stop.exit -ne 0) { Fail "${label}: stop failed: $($stop.output)" }
    $id = Get-UuidFromText $stop.output
    if ($null -ne $id) {
        $state = Wait-ForTerminal $id 300
        if ($state -ne 'succeeded') { Fail "${label}: stop ended $state" }
    }
}

try {
    Write-Step 'clean start: uninstall and purge'
    $clean = Invoke-Script $uninstallScript @('-AcceptVolatileDataLoss', '-PurgeData')
    Write-Reading 'clean.exit' ([string]$clean.exit)
    if (Test-Path -LiteralPath $installDir) { Fail "the install directory survived the clean uninstall: $installDir" }
    Assert-ProcessesGone 'clean'

    Write-Step 'install #1'
    $install = Invoke-Script $installScript @('-SourceDirectory', $source)
    Write-Reading 'install1.exit' ([string]$install.exit)
    if ($install.exit -ne 0) { Fail "install #1 failed: $($install.output)" }
    if ($install.output -notmatch 'phdskmnt .* present and running') {
        Fail "the install did not report the driver check: $($install.output)"
    }
    if ($null -eq (Wait-ForService 90)) { Fail 'the installed service never answered' }

    Write-Step "create a 64 MiB disk and commit a marker"
    $create = Invoke-Cli @('disk', 'create', '--name', 'upgrade',
        '--bundle', $bundlePath, '--size', '64MiB', '--mount', $mount,
        '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    if ((Wait-ForTerminal $createId 300) -ne 'succeeded') { Fail 'the create did not succeed' }
    $line = Get-DiskLine $mount
    $profileId = Get-UuidFromText $line
    Write-Reading 'profile_id' $profileId

    [System.IO.File]::WriteAllText($markerPath, $payload, [System.Text.Encoding]::UTF8)
    $stream = [System.IO.File]::Open($markerPath, 'Open', 'ReadWrite', 'Read')
    try { $stream.Flush($true) } finally { $stream.Dispose() }
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId)
    if ($checkpoint.exit -ne 0) { Fail "checkpoint failed: $($checkpoint.output)" }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -ne $checkpointId) {
        if ((Wait-ForTerminal $checkpointId 300) -ne 'succeeded') { Fail 'the checkpoint did not succeed' }
    }
    Stop-DiskAndWait $profileId 'before-upgrade'

    Write-Step 'overwrite install over a live install'
    $over = Invoke-Script $installScript @('-SourceDirectory', $source)
    Write-Reading 'overwrite.exit' ([string]$over.exit)
    if ($over.exit -ne 0) { Fail "the overwrite install failed: $($over.output)" }
    if ($null -eq (Wait-ForService 90)) { Fail 'the service did not come back after the overwrite install' }
    Write-Reading 'overwrite.profile_still_listed' $(if ((Get-DiskLine $mount) -match [regex]::Escape($profileId)) { 'yes' } else { 'NO' })
    if ((Get-DiskLine $mount) -notmatch [regex]::Escape($profileId)) {
        Fail 'the overwrite install lost the registered profile'
    }
    Start-DiskAndWait $profileId 'after-overwrite'
    Stop-DiskAndWait $profileId 'after-overwrite'

    Write-Step 'uninstall keeping the data record'
    $keep = Invoke-Script $uninstallScript @('-AcceptVolatileDataLoss')
    Write-Reading 'keep.exit' ([string]$keep.exit)
    if ($keep.exit -ne 0) { Fail "the data-keeping uninstall failed: $($keep.output)" }
    Assert-ProcessesGone 'keep'
    Write-Reading 'keep.install_dir_exists' ([string](Test-Path -LiteralPath $installDir))
    Write-Reading 'keep.data_dir_exists' ([string](Test-Path -LiteralPath $dataDir))
    if (Test-Path -LiteralPath $installDir) { Fail 'the install directory survived the uninstall' }
    if (-not (Test-Path -LiteralPath $dataDir)) { Fail 'the uninstall removed the data record' }

    Write-Step 'reinstall and start the same disk again'
    $again = Invoke-Script $installScript @('-SourceDirectory', $source)
    Write-Reading 'reinstall.exit' ([string]$again.exit)
    if ($again.exit -ne 0) { Fail "the reinstall failed: $($again.output)" }
    if ($null -eq (Wait-ForService 90)) { Fail 'the reinstalled service never answered' }
    if ((Get-DiskLine $mount) -notmatch [regex]::Escape($profileId)) {
        Fail 'the reinstall did not find the registered profile'
    }
    Start-DiskAndWait $profileId 'after-reinstall'

    Write-Step 'dirty uninstall: remove the product while the disk is running'
    $dirty = Invoke-Script $uninstallScript @('-AcceptVolatileDataLoss')
    Write-Reading 'dirty.exit' ([string]$dirty.exit)
    if ($dirty.exit -ne 0) { Fail "the dirty uninstall failed: $($dirty.output)" }
    Assert-ProcessesGone 'dirty'
    Write-Reading 'dirty.volume_gone' ([string](-not (Test-Path -LiteralPath 'T:\')))
    if (Test-Path -LiteralPath 'T:\') { Fail 'the dirty uninstall left the volume mounted' }
    Write-Reading 'dirty.data_dir_exists' ([string](Test-Path -LiteralPath $dataDir))
    if (-not (Test-Path -LiteralPath $dataDir)) { Fail 'the dirty uninstall removed the data record' }

    Write-Step 'reinstall after the dirty uninstall and read the committed bytes'
    $final = Invoke-Script $installScript @('-SourceDirectory', $source)
    Write-Reading 'final.exit' ([string]$final.exit)
    if ($final.exit -ne 0) { Fail "the final reinstall failed: $($final.output)" }
    if ($null -eq (Wait-ForService 90)) { Fail 'the final install never answered' }
    Start-DiskAndWait $profileId 'after-dirty'
    Stop-DiskAndWait $profileId 'after-dirty'
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.upgrade: overwrite, keep-data uninstall, reinstall, dirty uninstall'
    Write-Reading 'result' 'PASS'
} finally {
    # Leave no install behind for the next case, but keep the record when the run failed so
    # the evidence stays inspectable.
    if ($null -eq $script:keepDataOnExit) {
        $cleanup = Invoke-Script $uninstallScript @('-AcceptVolatileDataLoss', '-PurgeData')
        Write-Host ("cleanup: uninstall exit=" + [string]$cleanup.exit)
    }
}
