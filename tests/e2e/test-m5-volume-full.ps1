# integration.m5.volume_full -- a bundle volume without room must be refused explicitly, and
# freeing space must make the same request succeed.
#
# **Guest-only.**  It creates a real VHD, a real NTFS volume, a real worker and a real AIM
# disk.
#
# The shape: attach a 64 MiB VHD as V:, fill it until ~4 MiB are free, and ask the product to
# create a disk whose bundle lives there.  That request cannot fit (the bundle alone wants a
# 4 MiB base image plus an 8 MiB journal), so it must fail explicitly -- an exit 1 with an
# error, or an admitted operation that reaches `failed` -- and it must not leave a registered
# profile or a worker behind.  Then the filler is deleted and the same request, against a
# fresh bundle path, must succeed, checkpoint and stop.
#
# The second bundle path is deliberate: reusing the first one would hide whether a failed
# create left residue (the case records the residue separately instead of deleting it).

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
        'integration.m5.volume_full creates a real VHD, volume, worker and AIM disk; it may ' +
        'only run on the guest (AGENTS.md section 4).')
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

$workRoot = 'C:\m5\volume-full-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$vhdPath = Join-Path $workRoot 'bundle-volume.vhdx'
# **On V:, not in the work root.**  Measured once: the filler was created under the C:
# work root, so V:'s free space never moved and the loop wrote 120 MB to the system disk.
$fillerPath = 'V:\filler.bin'
$bundleFirst = 'V:\bundle-first'
$bundleSecond = 'V:\bundle-second'
$serviceLog = Join-Path $workRoot 'service.log'
$serviceOut = Join-Path $workRoot 'service.out.log'
$traceLog = Join-Path $workRoot 'trace.log'
$serviceProcess = $null
$vhdAttached = $false
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

function Invoke-Diskpart([string[]]$lines, [string]$label) {
    $scriptPath = Join-Path $workRoot ("diskpart-" + $label + ".txt")
    Set-Content -LiteralPath $scriptPath -Value $lines -Encoding ASCII
    $out = & diskpart.exe /s $scriptPath 2>&1
    Write-Reading ("${label}.diskpart_output") (($out -join ' | ') -replace "\s+", ' ')
    if ($LASTEXITCODE -ne 0) { Fail "${label}: diskpart exited $LASTEXITCODE" }
    return ($out -join "`n")
}

function Get-FreeBytes([string]$letter) {
    # `[System.IO.DriveInfo]` reads the volume's current state; a PSDrive object is not
    # guaranteed to refresh while the loop is writing.
    try {
        return [int64]([System.IO.DriveInfo]::new($letter + ':')).AvailableFreeSpace
    } catch {
        return -1
    }
}

try {
    Write-Step 'attach a 64 MiB VHD as V:'
    $out = Invoke-Diskpart @(
        "create vdisk file=`"$vhdPath`" maximum=64 type=fixed",
        "select vdisk file=`"$vhdPath`"",
        'attach vdisk',
        'create partition primary',
        'select partition 1',
        'format fs=ntfs quick label=M5BUNDLE',
        'assign letter=V'
    ) 'vhd'
    $vhdAttached = $true
    if (-not (Test-Path -LiteralPath 'V:\')) { Fail "V: did not appear after diskpart: $out" }
    $free = Get-FreeBytes 'V'
    Write-Reading 'volume.free_before_fill' ([string]$free)
    if ($free -lt 40MB) { Fail "the fresh volume is unexpectedly small: $free bytes free" }

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

    Write-Step 'fill the volume, leaving about 4 MiB free'
    if ((Get-FreeBytes 'V') -lt 0) { Fail 'V: disappeared before the fill' }
    $stream = [System.IO.File]::Open($fillerPath, 'Create', 'Write', 'None')
    try {
        $chunk = New-Object byte[] (1MB)
        $written = 0L
        while ((Get-FreeBytes 'V') -gt 6MB) {
            $stream.Write($chunk, 0, $chunk.Length)
            $written += $chunk.Length
            if ($written -gt 120MB) { Fail 'the filler loop did not reduce free space' }
        }
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    $free = Get-FreeBytes 'V'
    Write-Reading 'volume.filler_bytes' ([string]$written)
    Write-Reading 'volume.free_after_fill' ([string]$free)
    if ($free -gt 6MB) { Fail "the volume is still too empty: $free bytes free" }

    Write-Step 'create on the full volume must fail explicitly'
    $create = Invoke-Cli @('disk', 'create', '--name', 'full-first',
        '--bundle', $bundleFirst, '--size', '64MiB', '--mount', 'T:',
        '--checkpoint-interval', '30m')
    Write-Reading 'full.create_exit' ([string]$create.exit)
    Write-Reading 'full.create_output' (($create.output -replace "`n", ' | '))
    $state = 'refused'
    if ($create.exit -eq 0) {
        $id = Get-UuidFromText $create.output
        if ($null -eq $id) { Fail "the create was admitted without an operation id: $($create.output)" }
        $state = Wait-ForTerminal $id 300
    } elseif ($create.exit -eq 1) {
        if ($create.output -notmatch 'error:') {
            Fail "the refusal carried no error line: $($create.output)"
        }
    } else {
        Fail "a full bundle volume must be a product failure; got exit $($create.exit)"
    }
    Write-Reading 'full.create_state' $state
    if ($state -eq 'succeeded') { Fail 'a full volume accepted the create' }
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    Write-Reading 'full.workers_left' ([string]$workers)
    if ($workers -ne 0) { Fail "the failed create left $workers worker(s) behind" }
    $afterList = Invoke-Cli @('disk', 'list')
    Write-Reading 'full.list_exit' ([string]$afterList.exit)
    $stray = (($afterList.output -split "`n") | Where-Object { $_ -match 'generation=' })
    Write-Reading 'full.profiles_left' ([string]$stray.Count)
    if ($stray.Count -ne 0) { Fail "the failed create left a registered profile: $($afterList.output)" }
    Write-Reading 'full.bundle_dir_exists' ([string](Test-Path -LiteralPath $bundleFirst))

    Write-Step 'free the space and create again'
    Remove-Item -LiteralPath $fillerPath -Force
    $free = Get-FreeBytes 'V'
    Write-Reading 'volume.free_after_delete' ([string]$free)
    $create2 = Invoke-Cli @('disk', 'create', '--name', 'full-second',
        '--bundle', $bundleSecond, '--size', '64MiB', '--mount', 'T:',
        '--checkpoint-interval', '30m')
    if ($create2.exit -ne 0) { Fail "the create after freeing space failed: $($create2.output)" }
    $create2Id = Get-UuidFromText $create2.output
    if ($null -eq $create2Id) { Fail 'the second create carried no operation id' }
    if ((Wait-ForTerminal $create2Id 300) -ne 'succeeded') { Fail 'the second create did not succeed' }
    $line = Get-DiskLine 'T:'
    $profileId = Get-UuidFromText $line
    Write-Reading 'second.profile_id' $profileId

    $markerPath = 'T:\volume-full-marker.txt'
    $payload = 'committed after freeing the volume ' + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($markerPath, $payload, [System.Text.Encoding]::UTF8)
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileId)
    if ($checkpoint.exit -ne 0) { Fail "the checkpoint on the refilled volume failed: $($checkpoint.output)" }
    $checkpointId = Get-UuidFromText $checkpoint.output
    if ($null -ne $checkpointId) {
        $checkpointState = Wait-ForTerminal $checkpointId 300
        Write-Reading 'second.checkpoint_state' $checkpointState
        if ($checkpointState -ne 'succeeded') { Fail "the checkpoint ended $checkpointState" }
    }
    Write-Reading 'second.marker_intact' $(if ((Get-Content -LiteralPath $markerPath -Raw) -eq $payload) { 'yes' } else { 'NO' })

    $stop = Invoke-Cli @('disk', 'stop', $profileId)
    $stopState = 'refused'
    if ($stop.exit -eq 0) {
        $id = Get-UuidFromText $stop.output
        if ($null -ne $id) { $stopState = Wait-ForTerminal $id 300 }
    }
    Write-Reading 'second.stop_state' $stopState
    if ($stopState -ne 'succeeded') { Fail "the stop after the recovery ended $stopState" }
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId)
    Write-Reading 'cleanup_forget' ([string]$forget.exit)

    Write-Host 'PASS integration.m5.volume_full: full volume refused, freed volume recovers'
    Write-Reading 'result' 'PASS'
} finally {
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if ($vhdAttached) {
        # Not through `Invoke-Diskpart`: that helper fails the script, and this is a cleanup
        # path where a failure must be reported, not turned into a second abort.
        try {
            $scriptPath = Join-Path $workRoot 'diskpart-detach.txt'
            Set-Content -LiteralPath $scriptPath -Encoding ASCII -Value @(
                "select vdisk file=`"$vhdPath`"", 'detach vdisk')
            $out = & diskpart.exe /s $scriptPath 2>&1
            if ($LASTEXITCODE -ne 0) { Write-Host "WARN: detach exited ${LASTEXITCODE}: $out" }
        } catch {
            Write-Host "WARN: detaching the VHD failed: $($_.Exception.Message)"
        }
        if (Test-Path -LiteralPath $vhdPath) {
            Remove-Item -LiteralPath $vhdPath -Force -ErrorAction SilentlyContinue
        }
    }
}
