# integration.m5.state_format -- the durable service state declares a schema, and a record with
# an unsupported major must be refused explicitly instead of being decoded into something that
# looks recoverable.
#
# **Guest-only.**  It installs the real service, creates the real durable store under
# C:\ProgramData\MyRamDisk and restarts the installed service against a damaged record.
#
# What this pins (R-08's "data format version"): the record header is magic(8) + major(u16) +
# minor(u16); `profile_registration_codec.cpp` refuses `major != kSchemaMajor` with
# "Record schema major is unsupported".  The case patches the major in the installed service's
# durable state, starts the service exactly as shipped, and requires a refusal that names the
# schema -- then restores the original bytes and requires the service to come back.
#
# The installed service is used on purpose: a console service that is killed never publishes
# its state record (the file appears on a graceful stop), and this case needs the record.

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
        'integration.m5.state_format installs the real service and edits its durable store; it ' +
        'may only run on the guest (AGENTS.md section 4).')
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

$workRoot = 'C:\m5\state-format-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$backup = Join-Path $workRoot 'service_state.myrs.orig'
$statePath = $null
$installed = $false

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
}

function Invoke-Script([string]$path, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $path @Arguments 2>&1 | Out-String
        return [pscustomobject]@{ output = $output; exit = $LASTEXITCODE }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Invoke-InstalledCli([string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $cli @Arguments 2>&1 | Out-String
        return [pscustomobject]@{ output = $output; exit = $LASTEXITCODE }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Get-ServiceState {
    return (& sc.exe query $ServiceName 2>&1 | Out-String)
}

function Wait-ServiceState([string]$pattern, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $state = Get-ServiceState
        if ($state -match $pattern) { return $state }
        Start-Sleep -Milliseconds 500
    }
    return (Get-ServiceState)
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text,
        '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
    if ($match.Success) { return $match.Value }
    return $null
}

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $show = Invoke-InstalledCli @('operation', 'show', $operationId)
        if ($show.exit -ne 0) { Fail "operation show failed: $($show.output)" }
        $match = [regex]::Match($show.output, ': (succeeded|completed|failed|cancelled)')
        if ($match.Success) {
            if ($match.Groups[1].Value -eq 'completed') { return 'succeeded' }
            return $match.Groups[1].Value
        }
        Start-Sleep -Milliseconds 500
    }
    return 'timeout'
}

try {
    Write-Step 'clean start, then install'
    $clean = Invoke-Script $uninstallScript @('-AcceptVolatileDataLoss', '-PurgeData')
    Write-Reading 'clean.exit' ([string]$clean.exit)
    $install = Invoke-Script $installScript @('-SourceDirectory', $source)
    Write-Reading 'install.exit' ([string]$install.exit)
    if ($install.exit -ne 0) { Fail "the install failed: $($install.output)" }
    $installed = $true
    if ((Wait-ServiceState 'RUNNING' 90) -notmatch 'RUNNING') { Fail 'the installed service never started' }

    Write-Step 'create one disk so the durable state exists, then stop the service gracefully'
    $create = Invoke-InstalledCli @('disk', 'create', '--name', 'format-check',
        '--bundle', (Join-Path $workRoot 'bundle'), '--size', '64MiB',
        '--mount', 'T:', '--checkpoint-interval', '30m', '--no-start')
    Write-Reading 'create.exit' ([string]$create.exit)
    if ($create.exit -ne 0) { Fail "the state-creating create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    $created = Wait-ForTerminal $createId 300
    Write-Reading 'create.state' $created
    if ($created -ne 'succeeded') { Fail "the state-creating create ended $created" }

    & sc.exe stop $ServiceName | Out-Null
    $stopped = Wait-ServiceState 'STOPPED' 120
    Write-Reading 'stop.state' (($stopped -replace "`n", ' ') -replace '\s+', ' ')
    if ($stopped -notmatch 'STOPPED') { Fail "the service did not stop: $stopped" }

    $found = @(Get-ChildItem -LiteralPath $dataDir -Recurse -File -Filter 'service_state.myrs' -ErrorAction SilentlyContinue)
    if ($found.Count -eq 0) { Fail 'the durable state record was not created under C:\ProgramData\MyRamDisk' }
    $statePath = $found[0].FullName
    Write-Reading 'state.path' $statePath
    $bytes = [System.IO.File]::ReadAllBytes($statePath)
    Write-Reading 'state.bytes' ([string]$bytes.Length)
    if ($bytes.Length -lt 12) { Fail "the durable state is unexpectedly small: $($bytes.Length)" }
    Copy-Item -LiteralPath $statePath -Destination $backup -Force
    $originalMajor = [int]$bytes[8] + ([int]$bytes[9] -shl 8)
    Write-Reading 'state.schema_major_before' ([string]$originalMajor)

    Write-Step 'patch the schema major to an unsupported value'
    $bytes[8] = 0xFF
    $bytes[9] = 0x7F
    [System.IO.File]::WriteAllBytes($statePath, $bytes)
    Write-Reading 'state.schema_major_patched' ([string]0x7FFF)

    Write-Step 'the shipped service must refuse the record and say why'
    & sc.exe start $ServiceName | Out-Null
    $startExit = $LASTEXITCODE
    Write-Reading 'refusal.start_exit' ([string]$startExit)
    $running = Wait-ServiceState 'RUNNING' 20
    $refused = ($startExit -ne 0) -or ($running -notmatch 'RUNNING')
    Write-Reading 'refusal.service_running' ([string]($running -match 'RUNNING'))
    if (-not $refused) {
        & sc.exe stop $ServiceName | Out-Null
        Fail 'the service accepted an unsupported durable schema'
    }
    $serviceLog = Join-Path $dataDir 'logs\service.log'
    $text = (Get-Content -LiteralPath $serviceLog -Raw -ErrorAction SilentlyContinue)
    Write-Host ('--- service log tail: ' + (($text -split "`n" | Select-Object -Last 5) -join ' | '))
    if ($text -notmatch 'schema|unsupported|Unsupported') {
        Fail "the refusal did not name the schema in $serviceLog"
    }
    Write-Reading 'refusal.names_schema' 'yes'

    Write-Step 'restore the original record and start the service again'
    Copy-Item -LiteralPath $backup -Destination $statePath -Force
    $restored = [System.IO.File]::ReadAllBytes($statePath)
    $restoredMajor = [int]$restored[8] + ([int]$restored[9] -shl 8)
    Write-Reading 'state.schema_major_restored' ([string]$restoredMajor)
    if ($restoredMajor -ne $originalMajor) { Fail 'the schema major was not restored' }
    & sc.exe start $ServiceName | Out-Null
    $back = Wait-ServiceState 'RUNNING' 90
    Write-Reading 'restored.service_running' ([string]($back -match 'RUNNING'))
    if ($back -notmatch 'RUNNING') { Fail "the service did not come back: $back" }
    $list = Invoke-InstalledCli @('disk', 'list')
    Write-Reading 'restored.list_exit' ([string]$list.exit)
    if ($list.exit -ne 0) { Fail "the restored service did not answer disk list: $($list.output)" }

    Write-Host 'PASS integration.m5.state_format: an unsupported durable schema is refused by name'
    Write-Reading 'result' 'PASS'
} finally {
    if ($installed) {
        $cleanup = Invoke-Script $uninstallScript @('-AcceptVolatileDataLoss', '-PurgeData')
        Write-Host ("cleanup: uninstall exit=" + [string]$cleanup.exit)
    }
}
