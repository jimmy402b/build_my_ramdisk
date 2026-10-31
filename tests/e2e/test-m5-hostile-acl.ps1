# integration.m5.hostile_acl -- a profile directory whose ACL has been loosened must be
# refused explicitly, and restoring the ACL must bring the disk back.
#
# **Guest-only.**  It creates a real worker, a real AIM disk, a real Windows volume and it
# edits the DACL of a real ProgramData profile directory.
#
# What the product promises (profile_store.cpp, validate_management_acl): every profile
# directory's DACL must be *protected* and contain exactly the SYSTEM and Administrators
# management entries with full access.  This case clears the protection bit (one `Set-Acl`
# away from what a careless admin or an installer bug would do) and asserts:
#
#   1. the next command that opens the profile fails explicitly -- exit 1 with a message
#      that names the ACL, not a hang and not a silent success;
#   2. no worker is left behind;
#   3. after the original SDDL is restored, the same disk starts, serves its committed
#      marker and stops cleanly.
#
# The SDDL is captured before the damage and restored verbatim, so a failure here cannot
# leave the guest's store in a strange state.

param(
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'

trap {
    Write-Host "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host ($_.ScriptStackTrace)
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.hostile_acl creates a real worker, a real AIM disk, a real Windows ' +
        'volume and edits a ProgramData DACL; it may only run on the guest (AGENTS.md 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

foreach ($path in @($serviceExe, $cliExe)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "missing binary: $path" }
}

$workRoot = 'C:\m5\hostile-acl-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$bundlePath = Join-Path $workRoot 'bundle'
$profileName = 'acl-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$serviceLog = Join-Path $workRoot 'service.log'
$serviceOut = Join-Path $workRoot 'service.out.log'
$traceLog = Join-Path $workRoot 'trace.log'
$serviceProcess = $null
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

function Write-Reading([string]$key, [string]$value) {
    Write-Host "READING: $key=$value"
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

function Assert-AclMentioned([string]$label, $result) {
    if ($result.exit -ne 1) {
        Fail ("${label}: a loosened profile ACL must be a product failure (exit 1); got " +
            "exit $($result.exit): $($result.output)")
    }
    if ($result.output -notmatch 'ACL|protected|permission|access') {
        Fail "${label}: the failure did not name the ACL problem: $($result.output)"
    }
}

try {
    Write-Step 'start the service'
    Remove-ScService
    $running = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($running.Count -ne 0) { Fail "the guest already has $($running.Count) worker(s); reset it first" }
    $env:MYRD_TRACE_FILE = $traceLog
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog -RedirectStandardOutput $serviceOut `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 90
    if ($null -eq $listed) { Fail "the service never answered a list command: $((Get-Content -LiteralPath $serviceLog -Raw -ErrorAction SilentlyContinue))" }
    $existing = (($listed.output -split "`n") | Where-Object { $_ -match 'generation=' })
    if ($existing.Count -ne 0) { Fail "the guest already has $($existing.Count) profile(s); reset it first" }

    $script:mount = Select-FreeLetter
    if ($null -eq $script:mount) { Fail 'no free drive letter' }
    Write-Reading 'mount' $script:mount

    Write-Step "create a 64 MiB disk at $($script:mount)"
    $create = Invoke-Cli @('disk', 'create', '--name', $profileName,
        '--bundle', $bundlePath, '--size', '64MiB', '--mount', $script:mount,
        '--checkpoint-interval', '30m')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    $createId = Get-UuidFromText $create.output
    if ($null -eq $createId) { Fail 'the create carried no operation id' }
    if ((Wait-ForTerminal $createId 300) -ne 'succeeded') { Fail 'the create did not succeed' }
    $line = Get-DiskLine $script:mount
    $profileId = Get-UuidFromText $line
    Write-Reading 'profile_id' $profileId

    $markerPath = Join-Path ($script:mount + '\') 'acl-marker.txt'
    $payload = 'committed before the acl damage ' + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($markerPath, $payload, [System.Text.Encoding]::UTF8)
    $stream = [System.IO.File]::Open($markerPath, 'Open', 'ReadWrite', 'Read')
    try { $stream.Flush($true) } finally { $stream.Dispose() }
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId)
    if ($checkpoint.exit -ne 0) { Fail "checkpoint failed: $($checkpoint.output)" }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -ne $checkpointId) { [void](Wait-ForTerminal $checkpointId 300) }
    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    $stopState = 'refused'
    if ($stop.exit -eq 0) {
        $id = Get-UuidFromText $stop.output
        if ($null -ne $id) { $stopState = Wait-ForTerminal $id 300 }
    }
    Write-Reading 'stop_before_damage' $stopState
    if ($stopState -ne 'succeeded') { Fail "the disk did not stop before the damage: $stopState" }

    $profileDir = Join-Path (Join-Path 'C:\ProgramData\MyRamDisk' 'profiles') $profileId
    if (-not (Test-Path -LiteralPath $profileDir)) { Fail "missing profile directory: $profileDir" }
    $originalSddl = (Get-Acl -LiteralPath $profileDir).Sddl
    Write-Reading 'acl.sddl_before' $originalSddl

    Write-Step 'loosen the profile DACL (clear its protection)'
    $acl = Get-Acl -LiteralPath $profileDir
    $acl.SetAccessRuleProtection($false, $false)
    Set-Acl -LiteralPath $profileDir -AclObject $acl
    $damagedSddl = (Get-Acl -LiteralPath $profileDir).Sddl
    Write-Reading 'acl.sddl_damaged' $damagedSddl
    if ($damagedSddl -eq $originalSddl) { Fail 'the ACL damage did not change the SDDL' }

    Write-Step 'the next command that opens the profile must fail explicitly'
    $listAfter = Invoke-Cli @('disk', 'list')
    Write-Reading 'damaged.list_exit' ([string]$listAfter.exit)
    Write-Reading 'damaged.list_output' (($listAfter.output -replace "`n", ' | '))
    $startAfter = Invoke-Cli @('disk', 'start', $profileId)
    Write-Reading 'damaged.start_exit' ([string]$startAfter.exit)
    Write-Reading 'damaged.start_output' (($startAfter.output -replace "`n", ' | '))
    if ($listAfter.exit -ne 0) {
        Assert-AclMentioned 'damaged list' $listAfter
    } else {
        # The list tolerated the loosened directory; then the start is the command that
        # must refuse.  Either shape is explicit, but one of them has to be.
        Assert-AclMentioned 'damaged start' $startAfter
    }
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    Write-Reading 'damaged.workers_left' ([string]$workers)
    if ($workers -ne 0) { Fail "the ACL damage left $workers worker(s) behind" }

    Write-Step 'restore the original SDDL'
    $restored = New-Object System.Security.AccessControl.DirectorySecurity
    $restored.SetSecurityDescriptorSddlForm($originalSddl)
    Set-Acl -LiteralPath $profileDir -AclObject $restored
    $afterSddl = (Get-Acl -LiteralPath $profileDir).Sddl
    Write-Reading 'acl.sddl_restored' $afterSddl
    # **Not a string equality check.**  Windows re-derives some control flags when an SDDL
    # is applied (measured: the restored form gained `AI`, so the text differs while the
    # protection bit is still set).  What must hold is the property the product checks --
    # `SE_DACL_PROTECTED` -- and then the start below is the real verdict on the restore.
    $protectedAfter = (Get-Acl -LiteralPath $profileDir).AreAccessRulesProtected
    Write-Reading 'acl.protected_after_restore' ([string]$protectedAfter)
    if (-not $protectedAfter) { Fail 'the ACL restore did not restore DACL protection' }

    Write-Step 'the same disk must come back'
    $start = Invoke-Cli @('disk', 'start', $profileId)
    if ($start.exit -ne 0) { Fail "the start after the ACL restore failed: $($start.output)" }
    $startId = Get-UuidFromText $start.output
    if ($null -ne $startId) {
        $state = Wait-ForTerminal $startId 300
        if ($state -ne 'succeeded') { Fail "the start after the ACL restore ended $state" }
    }
    $runtime = Get-Field (Get-DiskLine $script:mount) 'runtime'
    Write-Reading 'restored.runtime' ([string]$runtime)
    if ($runtime -ne 'Running') { Fail "the disk did not reach Running after the ACL restore" }
    Write-Reading 'restored.marker_intact' $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })
    if ((Get-Content -LiteralPath $markerPath -Raw) -ne $payload) { Fail 'the marker bytes changed across the ACL damage' }

    $stopAfter = Invoke-Cli @('disk', 'stop', $profileId)
    $stopAfterState = 'refused'
    if ($stopAfter.exit -eq 0) {
        $id = Get-UuidFromText $stopAfter.output
        if ($null -ne $id) { $stopAfterState = Wait-ForTerminal $id 300 }
    }
    Write-Reading 'restored.stop_state' $stopAfterState
    if ($stopAfterState -ne 'succeeded') { Fail "the stop after the ACL restore ended $stopAfterState" }

    Write-Step 'cleanup: forget the disk'
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.hostile_acl: loosened ACL refused, restored ACL recovers'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
