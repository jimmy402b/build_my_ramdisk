# integration.m5.soak -- one round of the 24 h soak, or the check that follows a power cut.
#
# **Guest-only.**  The host orchestrator (tools/m5vm/Test-M5Soak.ps1) runs this case in a loop:
# each invocation performs `-Rounds` rounds and records handle counts, worker count, memory and
# generation in `C:\m5\soak\readings.csv`.  Every few rounds the host cuts the power with
# `vmrun stop ... hard` and then runs this case again in `verify` mode (selected by
# `C:\m5\soak\mode.txt`), which checks the guest came back without residue.
#
# A round:
#   1. create a 64 MiB disk in a fresh bundle, mount T:;
#   2. write `-PayloadMiB` of pseudo-random bytes and hash them;
#   3. checkpoint (committed), then write a second payload **without** a checkpoint;
#   4. stop, start (adopt), read the committed payload back and compare the hash;
#   5. stop, forget;
#   6. record: round, service/worker handle counts, worker count, service/worker working set,
#      generation from `disk show`, and whether the data verified.
#
# The uncommitted second write is what makes a later power cut interesting: after a cut, the
# next `start` must recover the last committed generation, and step 4's hash check is the
# oracle.  (`verify` mode does not recreate that write; it checks residue and recovers any
# profile left behind.)

param(
    [string]$Configuration = 'Release',
    [int]$Rounds = 1,
    [int]$SizeMiB = 64,
    [int]$PayloadMiB = 16,
    [string]$SoakRoot = 'C:\m5\soak'
)

$ErrorActionPreference = 'Stop'

trap {
    $message = "UNCAUGHT: $($_.Exception.GetType().FullName): $($_.Exception.Message)"
    Write-Host $message
    Write-Host ($_.ScriptStackTrace)
    # The host driver cannot see Write-Host output (it goes to the host, not the pipeline), so
    # the status and readings are written where it can read them.
    try {
        Set-Content -LiteralPath (Join-Path $SoakRoot 'last.txt') -Value ("FAIL`n" + $message) -Encoding UTF8
    } catch { }
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.soak creates real workers, AIM disks and volumes; it may only run on ' +
        'the guest (AGENTS.md section 4).')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
. (Join-Path $PSScriptRoot 'm5-e2e-common.ps1')

New-Item -ItemType Directory -Force -Path $SoakRoot | Out-Null
$modeFile = Join-Path $SoakRoot 'mode.txt'
$mode = if (Test-Path -LiteralPath $modeFile) {
    (Get-Content -LiteralPath $modeFile -Raw).Trim()
} else { 'round' }
# **Who is calling?**  The 24 h driver leaves one service running across rounds on purpose.  A
# plain ctest invocation (the release suite) must not: it would leave a console service behind
# and the next case that installs or starts a service would be running against it.  The driver
# writes `driver.txt`; anything else is standalone and cleans up what it started.
$driverFile = Join-Path $SoakRoot 'driver.txt'
$driver = if (Test-Path -LiteralPath $driverFile) {
    (Get-Content -LiteralPath $driverFile -Raw).Trim()
} else { 'standalone' }
Write-Host "READING: soak.mode=$mode"
Write-Host "READING: soak.driver=$driver"
$serviceStartedHere = $false

$workRoot = Join-Path $SoakRoot ('run-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$serviceLog = Join-Path $workRoot 'service.log'
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$serviceProcess = $null

$script:readingLines = New-Object System.Collections.Generic.List[string]
# **A failure has to leave its reason where the driver can read it.**  `Fail` from the shared
# helpers exits immediately and would skip the status file, so the soak uses its own.
function Fail-Soak([string]$message) {
    $text = "FAIL: $message"
    [Console]::Error.WriteLine($text)
    try {
        Set-Content -LiteralPath (Join-Path $SoakRoot 'last.txt') -Value ("FAIL`n" + $text + "`n" + ($script:readingLines -join "`n")) -Encoding UTF8
    } catch { }
    exit 1
}

function Write-Reading([string]$key, [string]$value) {
    $line = "READING: $key=$value"
    Write-Host $line
    $script:readingLines.Add($line)
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
        $show = Invoke-Cli @('operation', 'show', $operationId)
        $match = [regex]::Match($show.output,
            '(?m)^operation\s+\S+:\s+(succeeded|completed|failed|cancelled)\b')
        if ($match.Success) {
            if ($match.Groups[1].Value -eq 'completed') { return 'succeeded' }
            return $match.Groups[1].Value
        }
        Start-Sleep -Milliseconds 200
    }
    return 'timeout'
}

function Invoke-Operation([string]$label, [string[]]$arguments, [int]$seconds = 300) {
    $result = Invoke-Cli $arguments
    if ($result.exit -ne 0) { return @{ state = 'refused'; output = $result.output } }
    $id = Get-UuidFromText $result.output
    if ($null -eq $id) { return @{ state = 'no-id'; output = $result.output } }
    return @{ state = (Wait-ForTerminal $id $seconds); output = $result.output }
}

function Get-ProcessReadings([string]$label) {
    $service = Get-Process -Name 'myrd-service' -ErrorAction SilentlyContinue | Select-Object -First 1
    $workers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    $worker = $workers | Select-Object -First 1
    $row = [ordered]@{
        utc            = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        round          = $label
        workers        = $workers.Count
        service_handle = if ($null -ne $service) { $service.HandleCount } else { -1 }
        worker_handle  = if ($null -ne $worker) { $worker.HandleCount } else { -1 }
        service_ws_mb  = if ($null -ne $service) { [math]::Round($service.WorkingSet64 / 1MB, 1) } else { -1 }
        worker_ws_mb   = if ($null -ne $worker) { [math]::Round($worker.WorkingSet64 / 1MB, 1) } else { -1 }
    }
    foreach ($key in $row.Keys) { Write-Reading "soak.$key" ([string]$row[$key]) }
    return $row
}

function Add-ReadingRow($row) {
    $csvPath = Join-Path $SoakRoot 'readings.csv'
    $exists = Test-Path -LiteralPath $csvPath
    $line = ($row.Keys | ForEach-Object { $row[$_] }) -join ','
    if (-not $exists) {
        Set-Content -LiteralPath $csvPath -Value (($row.Keys) -join ',') -Encoding ASCII
    }
    Add-Content -LiteralPath $csvPath -Value $line -Encoding ASCII
}

function Select-Mount {
    foreach ($candidate in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if (Test-Path "$candidate\") { continue }
        if ($null -ne (Get-Volume -DriveLetter $candidate.TrimEnd(':') -ErrorAction SilentlyContinue)) { continue }
        return $candidate
    }
    return $null
}

function Stop-EveryProfile {
    foreach ($row in @((Invoke-Cli @('disk', 'list')).output)) {
        foreach ($match in [regex]::Matches($row,
                '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')) {
            $id = $match.Value
            $stop = Invoke-Operation 'stop' @('disk', 'stop', $id) 300
            [void](Invoke-Cli @('disk', 'forget', $id, '--confirmation', $id))
        }
    }
}

try {
    if ($mode -eq 'stop-service') {
        Write-Step 'stop-service: end the console service the soak left running'
        Get-Process -Name 'myrd-service' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        $remaining = @(Get-Process -Name 'myrd-service','myrd-m5-worker' -ErrorAction SilentlyContinue).Count
        Write-Reading 'soak.remaining_processes' ([string]$remaining)
        Set-Content -LiteralPath (Join-Path $SoakRoot 'last.txt') -Value (("PASS`n" + ($script:readingLines -join "`n"))) -Encoding UTF8
        exit 0
    }

    # **One service process across many rounds.**  If a service already answers, the round
    # reuses it: that is what makes handle/memory readings over a batch say something about
    # leaks.  The service is only ended by `stop-service` (or by a power cut, which is the
    # point of the exercise).
    Write-Step 'use the running service, or start one'
    $probe = Invoke-Cli @('disk', 'list')
    if ($probe.exit -eq 0) {
        Write-Reading 'soak.service' 'reused'
    } else {
        Remove-ScService
        $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
            -RedirectStandardError $serviceLog `
            -RedirectStandardOutput (Join-Path $workRoot 'service.out.log') `
            -WindowStyle Hidden -PassThru
        if ($null -eq (Wait-ForService 90)) { Fail-Soak 'the service never answered' }
        $serviceStartedHere = $true
        Write-Reading 'soak.service' 'started'
    }
    # **The soak owns the guest.**  Leftover profiles from an earlier run collide on the disk
    # name and on the mount target (measured: "The profile name is already registered to
    # another disk", and the mount target has the same problem), so they are cleared first.
    $leftovers = @(Get-ChildItem 'C:\ProgramData\MyRamDisk\profiles' -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'workers' }).Count
    Write-Reading 'soak.leftover_profiles' ([string]$leftovers)
    if ($leftovers -ne 0) { Stop-EveryProfile }

    if ($mode -eq 'verify') {
        Write-Step 'verify: the guest came back with no residue and every profile can be recovered'
        $rows = @((Invoke-Cli @('disk', 'list')).output)
        $ids = @()
        foreach ($row in $rows) {
            foreach ($match in [regex]::Matches($row,
                    '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')) {
                if ($ids -notcontains $match.Value) { $ids += $match.Value }
            }
        }
        Write-Reading 'verify.profiles' ([string]$ids.Count)
        $recovered = 0
        foreach ($id in $ids) {
            $start = Invoke-Operation 'start' @('disk', 'start', $id) 300
            if ($start.state -eq 'succeeded') { $recovered++ }
            [void](Invoke-Cli @('disk', 'stop', $id))
            Start-Sleep -Milliseconds 500
            [void](Invoke-Cli @('disk', 'stop', $id, '--force'))
        }
        Write-Reading 'verify.recovered' ([string]$recovered)
        $residue = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
        Write-Reading 'verify.workers' ([string]$residue)
        $row = Get-ProcessReadings 'verify'
        $row['data_verified'] = -1
        $row['generation'] = -1
        Add-ReadingRow $row
        Stop-EveryProfile
        Write-Host 'PASS integration.m5.soak (verify)'
        Set-Content -LiteralPath (Join-Path $SoakRoot 'last.txt') -Value (("PASS`n" + ($script:readingLines -join "`n"))) -Encoding UTF8
        exit 0
    }

    for ($round = 1; $round -le $Rounds; $round++) {
        $mount = Select-Mount
        if ($null -eq $mount) { Fail-Soak 'no free drive letter' }
        $bundle = Join-Path $workRoot ("bundle-" + $round)
        $create = Invoke-Operation 'create' @('disk', 'create', '--name', ("soak-$round-" + [guid]::NewGuid().ToString('N').Substring(0, 6)),
            '--bundle', $bundle, '--size', "$($SizeMiB)MiB", '--mount', $mount,
            '--checkpoint-interval', '1m') 300
        if ($create.state -ne 'succeeded') { Fail-Soak "round $round create ended $($create.state)" }
        $profileId = $null
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and $null -eq $profileId) {
            Start-Sleep -Milliseconds 300
            $profileId = Get-UuidFromText (Invoke-Cli @('disk', 'list')).output
        }
        if ($null -eq $profileId) { Fail-Soak "round $round produced no listed profile" }
        $letter = $mount.TrimEnd(':')

        $committed = Join-Path "$($mount)\" 'committed.bin'
        $uncommitted = Join-Path "$($mount)\" 'uncommitted.bin'
        $payload = New-Object byte[] ($PayloadMiB * 1MB)
        (New-Object Random(42 + $round)).NextBytes($payload)
        [System.IO.File]::WriteAllBytes($committed, $payload)
        $hashBefore = (Get-FileHash -LiteralPath $committed -Algorithm SHA256).Hash
        $checkpoint = Invoke-Operation 'checkpoint' @('disk', 'checkpoint', $profileId) 300
        if ($checkpoint.state -ne 'succeeded') { Fail-Soak "round $round checkpoint ended $($checkpoint.state)" }
        # Uncommitted write: it exists in RAM and must be gone after a power cut.
        [System.IO.File]::WriteAllBytes($uncommitted, $payload)
        $stop = Invoke-Operation 'stop' @('disk', 'stop', $profileId) 300
        if ($stop.state -ne 'succeeded') { Fail-Soak "round $round stop ended $($stop.state)" }

        $start = Invoke-Operation 'start' @('disk', 'start', $profileId) 300
        if ($start.state -ne 'succeeded') { Fail-Soak "round $round restart ended $($start.state)" }
        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline -and
               $null -eq (Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue)) {
            Start-Sleep -Milliseconds 500
        }
        $hashAfter = if (Test-Path -LiteralPath $committed) {
            (Get-FileHash -LiteralPath $committed -Algorithm SHA256).Hash
        } else { 'missing' }
        Write-Reading "soak.hash_match" ([string]($hashBefore -eq $hashAfter))
        $show = Invoke-Cli @('disk', 'show', $profileId)
        $generation = -1
        $genMatch = [regex]::Match($show.output, 'generation=([0-9]+)')
        if ($genMatch.Success) { $generation = [int]$genMatch.Groups[1].Value }
        Write-Reading 'soak.generation' ([string]$generation)
        Write-Reading 'soak.data_verified' ([string]($hashBefore -eq $hashAfter))

        $row = Get-ProcessReadings ([string]$round)
        $row['data_verified'] = [int]($hashBefore -eq $hashAfter)
        $row['generation'] = $generation
        Add-ReadingRow $row
        if ($hashBefore -ne $hashAfter) { Fail-Soak "round $round lost committed data" }

        $stop = Invoke-Operation 'stop' @('disk', 'stop', $profileId) 300
        if ($stop.state -ne 'succeeded') { Fail-Soak "round $round final stop ended $($stop.state)" }
        # **Forget is asynchronous.**  Measured: `disk forget` without a terminal wait returns
        # success while the operation is still proving safety; removing the bundle under it made
        # the proof fail with `image_path.open_root`, the profile survived, and the next round's
        # create hit "the mount target is already registered to another disk".  Wait first,
        # remove the bundle only afterwards.
        $forget = Invoke-Operation 'forget' @('disk', 'forget', $profileId, '--confirmation', $profileId) 300
        if ($forget.state -ne 'succeeded') { Fail-Soak "round $round forget ended $($forget.state)" }
        Remove-Item -LiteralPath (Join-Path $workRoot ("bundle-" + $round)) -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Host "PASS integration.m5.soak ($Rounds round(s))"
    Write-Reading 'soak.rounds' ([string]$Rounds)
    Write-Reading 'result' 'PASS'
    Set-Content -LiteralPath (Join-Path $SoakRoot 'last.txt') -Value (("PASS`n" + ($script:readingLines -join "`n"))) -Encoding UTF8
} finally {
    # Under the soak driver the service is deliberately left running: the next round reuses it
    # and the readings describe one long-lived process (`stop-service` ends it at the end).
    # Everywhere else, a service this case started is stopped again.
    if ($serviceStartedHere -and $driver -ne 'soak') {
        Get-Process -Name 'myrd-service' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}
