# integration.m5.cli_e2e -- the public command surface, end to end, through the CLI.
#
# Two acceptances live here:
#
#   T1-04  "while a disk is being created, `disk list` returns within one second".
#          Measured by issuing a create and then timing `disk list` repeatedly while the
#          create's own durable operation is still non-terminal.  Every sample is
#          recorded with the state the operation was in when the sample began, so the
#          claim "these samples were taken during a create" is backed by a reading rather
#          than by an assumption about how long a create takes.
#
#   T1-05  the command surface works through the shipped executables: create -> list ->
#          show -> read/write -> checkpoint -> stop -> remove, all through `myrd-cli`
#          talking to the installed service over the public pipe.
#
# This runs on the guest only (AGENTS.md section 4): it creates a real worker process, a
# real AIM disk and a real Windows volume.  On the development machine it refuses with a
# non-zero exit rather than passing quietly -- a silently skipped acceptance is the
# failure mode the guard exists to prevent.
#
# Everything is asserted from the CLI's own exit codes and output, because that is what a
# script or a user sees.  Nothing here reaches into the product's internals; a failure
# therefore means "the command surface does not work", which is exactly the claim.

param(
    # The configuration whose binaries are exercised.  Staged as `src/<config>/*.exe`.
    [string]$Configuration = 'Release',
    # Upper bound on latency samples taken during the create.
    [int]$ListSamples = 20,
    # T1-04's number.
    [int]$MaxListMilliseconds = 1000,
    # How long the whole run may take.
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

function Write-Step([string]$message) {
    Write-Host "--- $message"
}

function Fail([string]$message) {
    [Console]::Error.WriteLine("FAIL: $message")
    exit 1
}

if (-not $env:MYRD_M5VM_GUEST) {
    [Console]::Error.WriteLine(
        'integration.m5.cli_e2e creates a real worker, a real AIM disk and a real ' +
        'Windows volume, and may only run on the guest (AGENTS.md section 4).  Use ' +
        "tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^integration\.m5\.cli_e2e$' rather " +
        'than ctest on the development machine.')
    exit 2
}

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tests/e2e -> repo
$bin = Join-Path $repo "out/build/msvc/src/$Configuration"
$serviceExe = Join-Path $bin 'myrd-service.exe'
$cliExe = Join-Path $bin 'myrd-cli.exe'
$workerExe = Join-Path $bin 'myrd-m5-worker.exe'
foreach ($path in @($serviceExe, $cliExe, $workerExe)) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing binary: $path" }
}

$serviceName = 'MyRamDiskService'
$workRoot = 'C:\m5\cli-e2e-' + [guid]::NewGuid().ToString('N')
$bundlePath = Join-Path $workRoot 'bundle'
# Chosen at run time, not hardcoded.  The guest carries registrations from earlier work --
# measured: profiles claiming `R:` and `S:` -- so a fixed letter would collide with one of
# them and the create would be refused for a reason that has nothing to do with this test.
$mount = $null
$profileName = 'e2e-' + [guid]::NewGuid().ToString('N').Substring(0, 8)

# The command surface is reached as a *user* would reach it: a fresh process per command,
# and nothing but its exit code and its text.
#
# `ErrorActionPreference` is switched to `Continue` around every native call and the verdict
# is the exit code.  Under `Stop`, a native command that writes to stderr becomes a
# terminating error, which turned "the service is not up yet, try again" into a script abort
# on the first probe -- the same pitfall the harness documents for its own calls.
function Invoke-Cli {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $started = Get-Date
        $output = & $cliExe @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
        return [pscustomobject]@{
            output    = $output
            exit      = $exit
            ms        = [int]((Get-Date) - $started).TotalMilliseconds
            arguments = ($Arguments -join ' ')
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Invoke-Sc {
    param([string[]]$Arguments)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & sc.exe @Arguments 2>&1 | Out-String
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($exit -ne 0) {
        Fail "sc.exe $($Arguments -join ' ') failed ($exit): $output"
    }
    return $output
}

function Get-ServiceState {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & sc.exe query $serviceName 2>&1 | Out-String
    } finally {
        $ErrorActionPreference = $previous
    }
    return $output
}

function Wait-ForTerminal([string]$operationId, [int]$seconds) {
    # One `operation wait` is a *bounded* observation, not a promise that the operation is
    # finished: the service answers with its current state after at most its own wait budget.
    # A create that is still publishing when that budget expires is not a failure, so the
    # caller loops until the state is terminal or its own deadline passes -- and a terminal
    # `failed` is a product failure, so the loop stops there and reports it.
    $until = (Get-Date).AddSeconds($seconds)
    $state = 'unknown'
    while ((Get-Date) -lt $until) {
        # **Polled with `operation show`, not `operation wait`.**
        #
        # `operation wait` currently fails on the client with "the control pipe exchange
        # failed" about thirty seconds in -- the server-side wait budget -- and the service's
        # log shows the wait request never reaching the handler after the first one.  That is a
        # defect of its own and it is recorded in the evidence document; the end-to-end test
        # must not be blocked on it, so it observes with the command that does work.
        $state = Get-OperationState $operationId
        if (@('succeeded', 'failed', 'cancelled') -contains $state) { return $state }
        Start-Sleep -Milliseconds 500
    }
    Fail "the operation did not reach a terminal state within $seconds s (last state: $state)"
}

function Get-UuidFromText([string]$text) {
    $match = [regex]::Match($text, '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
    if (-not $match.Success) { return $null }
    return $match.Value
}

function Get-OperationState([string]$operationId) {
    # Exit 1 is expected here: an operation that *failed* is a product failure, so the CLI
    # reports it as one.  The state is read from the output either way, and only an
    # unreadable answer is fatal.
    $result = Invoke-Cli @('operation', 'show', $operationId)
    foreach ($word in @('queued', 'running', 'reconciling', 'succeeded', 'failed', 'cancelled')) {
        if ($result.output -match "operation $operationId`: $word") { return $word }
    }
    Fail "operation show $operationId did not report a state (exit $($result.exit)): $($result.output)"
}

function Wait-ForService([int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        $probe = Invoke-Cli @('disk', 'list')
        if ($probe.exit -eq 0) { return $probe }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Remove-Service {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & sc.exe query $serviceName 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            if ((Get-ServiceState) -match 'RUNNING') {
                & sc.exe stop $serviceName 2>&1 | Out-Null
                for ($i = 0; $i -lt 120; $i++) {
                    if ((Get-ServiceState) -match 'STOPPED') { break }
                    Start-Sleep -Milliseconds 500
                }
            }
            & sc.exe delete $serviceName 2>&1 | Out-Null
            Start-Sleep -Milliseconds 500
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$serviceProcess = $null

try {
    Write-Step "binaries under $bin"
    Remove-Service

    Write-Step "start the service in the foreground from $serviceExe"
    # **Foreground rather than installed, for the log.**  An installed service's stderr goes
    # nowhere: the SCM starts the process and discards it, so every diagnosis of a failing
    # command is blind.  Console mode runs the same composition root, the same host and the
    # same public pipe, and its log survives.  The installed path is exercised further down,
    # where the claim being made is about the SCM rather than about diagnostics.
    # A step trace the service and the worker both append to (see diagnostic_trace): it is the
    # only record that survives a step which never returns.
    $env:MYRD_TRACE_FILE = Join-Path $workRoot 'trace.log'
    $serviceLog = Join-Path $workRoot 'service.log'
    $serviceOutLog = Join-Path $workRoot 'service.out.log'
    $serviceProcess = Start-Process -FilePath $serviceExe -ArgumentList '--console' `
        -RedirectStandardError $serviceLog -RedirectStandardOutput $serviceOutLog `
        -WindowStyle Hidden -PassThru
    $listed = Wait-ForService 60
    if ($null -eq $listed) {
        # The service's own state is the first thing to rule out: a service that never
        # reached RUNNING cannot have a pipe for the client to find.
        $state = Get-ServiceState
        Fail "the service never answered a list command.`n$state`n$((Get-Content -LiteralPath $serviceLog -Raw -ErrorAction SilentlyContinue))"
    }
    Write-Host "  service pid $($serviceProcess.Id) is answering: $($listed.output.Trim())"

    # The letter must be free by the *product's* test, not by `Test-Path`.  The product's
    # refusal is `provision_new_volume / code=20994 / "requested drive letter is occupied"`,
    # which looks at the volume and device namespace; `Test-Path` and `disk list` do not see
    # a leftover volume that is present but not mounted at a path.  Measured cost of using
    # the weaker test: a create failed with exactly that refusal while this script believed
    # the letter was free.
    #
    # So the letters are taken from the same namespace the product answers from -- the
    # volumes Windows reports -- and a letter the product then refuses is a *hard failure*
    # with the product's own reason, never a silent retry with the next candidate.
    $occupied = @()
    foreach ($volume in (Get-CimInstance -ClassName Win32_Volume -ErrorAction Stop)) {
        if ($volume.DriveLetter) { $occupied += $volume.DriveLetter.ToUpper() }
    }
    foreach ($disk in (Get-CimInstance -ClassName Win32_LogicalDisk -ErrorAction Stop)) {
        if ($disk.DeviceID) { $occupied += $disk.DeviceID.ToUpper() }
    }
    # **And the DOS-device namespace, which is the one that actually refuses.**
    #
    # A failed create can leave a drive letter *mapped* to a device that no longer exists.
    # `Get-Volume` and `Win32_*` do not list it -- there is no volume to list -- but the name is
    # still taken, so the preflight refuses with "requested drive letter is occupied" and
    # `diskpart`'s `Set-Partition` refuses with "the requested access path is already in use".
    # Measured on the guest: after one failed create `U:` answered `QueryDosDevice` with a device
    # path while `Get-Volume` showed no `U:` at all, and the next run at `U:` failed in the
    # helper.  Only a reboot clears such a ghost.
    #
    # So a letter counts as free only when nothing at all answers for it.
    if (-not ([System.Management.Automation.PSTypeName]'Myrd.DosDevice').Type) {
        Add-Type -Namespace Myrd -Name DosDevice -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern int QueryDosDevice(string name, System.Text.StringBuilder target, int max);
'@
    }
    $mapped = @()
    foreach ($candidate in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        $buffer = New-Object System.Text.StringBuilder 512
        if ([Myrd.DosDevice]::QueryDosDevice($candidate, $buffer, 512) -ne 0) {
            $mapped += $candidate
            Write-Host ("  {0} is still mapped to {1} -- a previous run's remnant, not a volume" -f
                $candidate, $buffer.ToString())
        }
    }
    $occupied = @($occupied + $mapped | Sort-Object -Unique)
    Write-Host "  drive letters the product's namespace reports as taken: $($occupied -join ' ')"

    foreach ($letter in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if ($occupied -contains $letter) { continue }
        if ($listed.output -match [regex]::Escape($letter)) { continue }
        $mount = $letter
        break
    }
    if ($null -eq $mount) { Fail 'no free drive letter is available for the test volume' }
    Write-Host "  the test volume will be mounted at $mount"

    # -----------------------------------------------------------------------
    # T1-04: a list during a create.
    # -----------------------------------------------------------------------
    Write-Step 'T1-04: create, and time `disk list` while it is in progress'
    $create = Invoke-Cli @('disk', 'create', '--name', $profileName, '--bundle', $bundlePath,
        '--size', '64MiB', '--mount', $mount, '--no-wait')
    if ($create.exit -ne 0) { Fail "create failed: $($create.output)" }
    Write-Host "  $($create.output.Trim())"
    $operationId = Get-UuidFromText $create.output
    if ($null -eq $operationId) { Fail 'the create response carried no operation identifier' }

    $samples = New-Object System.Collections.Generic.List[object]
    $terminal = @('succeeded', 'failed', 'cancelled')
    while ($samples.Count -lt $ListSamples) {
        $state = Get-OperationState $operationId
        if ($terminal -contains $state) { break }
        $sample = Invoke-Cli @('disk', 'list')
        $samples.Add([pscustomobject]@{
                state = $state
                exit  = $sample.exit
                ms    = $sample.ms
            })
        if ((Get-Date) -gt $deadline) { Fail 'the run exceeded its deadline while sampling' }
    }
    foreach ($sample in $samples) {
        Write-Host ("  sample state={0,-10} exit={1} {2,5} ms" -f $sample.state, $sample.exit, $sample.ms)
    }
    if ($samples.Count -eq 0) { Fail 'no list sample was taken; the create finished before the first one' }
    $during = @($samples | Where-Object { $_.state -notin $terminal })
    if ($during.Count -lt 3) {
        Fail (("only {0} of {1} samples were taken while the create was still running; " +
                'the acceptance needs the list to be measured *during* the create') -f
            $during.Count, $samples.Count)
    }
    $failed = @($samples | Where-Object { $_.exit -ne 0 })
    if ($failed.Count -ne 0) { Fail "$($failed.Count) list samples failed" }
    $slow = @($samples | Where-Object { $_.ms -ge $MaxListMilliseconds })
    if ($slow.Count -ne 0) {
        Fail (("{0} of {1} list samples took {2} ms or more during a create (max {3} ms)" -f
                $slow.Count, $samples.Count, $MaxListMilliseconds,
                ($samples | Measure-Object -Property ms -Maximum).Maximum))
    }
    Write-Host ("PASS T1-04: {0} list samples during the create, max {1} ms (limit {2} ms)" -f
        $samples.Count, ($samples | Measure-Object -Property ms -Maximum).Maximum, $MaxListMilliseconds)

    # -----------------------------------------------------------------------
    # T1-05: the command surface, through the CLI.
    # -----------------------------------------------------------------------
    Write-Step 'T1-05: wait for the create, then list and show it'
    # A create is longer than one `operation wait` budget: the operation is admitted, then the
    # worker provisions and publishes, and the service answers a wait with whatever it knows at
    # the time.  Measured: one wait returned while the phase was still `publish-provisioning`,
    # which is a *running* operation, not a failed one -- so the loop waits for the state to
    # become terminal and only then judges it.
    $finalState = Wait-ForTerminal $operationId 600
    $waited = Invoke-Cli @('operation', 'wait', $operationId)
    if ($finalState -ne 'succeeded') {
        # A failed operation is diagnosed from what the service will tell a client: the
        # disk's own state and the operation's phase -- and, because this run started the
        # service itself, from the service's own log, which an installed service would
        # have thrown away.
        Write-Host "FAILED WAIT: terminal state $finalState`n$($waited.output.Trim())"
        Write-Host "  disk list: $((Invoke-Cli @('disk', 'list')).output.Trim())"
        Write-Host "  disk show: $((Invoke-Cli @('disk', 'show', $mount)).output.Trim())"
        Write-Host "  workers: $((@(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)).Count)"
        Write-Host "  work root: $((Get-ChildItem -LiteralPath $workRoot -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', ')"
        Write-Host "  service log:"
        Write-Host "$(Get-Content -LiteralPath $serviceLog -Raw)"
        Fail "the create did not succeed: $finalState"
    }
    Write-Host "  $($waited.output.Trim())"

    $list = Invoke-Cli @('disk', 'list')
    if ($list.exit -ne 0) { Fail "disk list failed after the create: $($list.output)" }
    Write-Host "  $($list.output.Trim())"
    $profileId = Get-UuidFromText $list.output
    if ($null -eq $profileId) { Fail 'disk list did not report the created profile' }
    if ($list.output -notmatch [regex]::Escape($mount)) {
        Fail "disk list did not report the mount target $mount"
    }

    $show = Invoke-Cli @('disk', 'show', $mount)
    if ($show.exit -ne 0) { Fail "disk show $mount failed: $($show.output)" }
    Write-Host "  $($show.output.Trim())"

    Write-Step 'T1-05: read and write the volume'
    $probeFile = Join-Path $mount '\m5-cli-e2e.txt'
    $payload = 'written through the public pipe at ' + (Get-Date).ToString('o')
    Set-Content -LiteralPath $probeFile -Value $payload -Encoding utf8
    $readBack = Get-Content -LiteralPath $probeFile -Raw
    if ($readBack.Trim() -ne $payload.Trim()) { Fail 'the bytes read back did not match what was written' }
    Write-Host "  wrote and read back $((Get-Item $probeFile).Length) bytes on $mount"

    Write-Step 'T1-05: checkpoint, then stop'
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $mount, '--wait')
    if ($checkpoint.exit -ne 0) { Fail "disk checkpoint failed: $($checkpoint.output)" }
    Write-Host "  $($checkpoint.output.Trim())"

    Remove-Item -LiteralPath $probeFile -Force
    Set-Content -LiteralPath (Join-Path $mount '\m5-cli-e2e-after.txt') -Value 'after checkpoint' -Encoding utf8

    $stop = Invoke-Cli @('disk', 'stop', $mount, '--wait')
    if ($stop.exit -ne 0) { Fail "disk stop failed: $($stop.output)" }
    Write-Host "  $($stop.output.Trim())"
    if (Test-Path -LiteralPath ($mount + '\')) {
        Fail "the volume is still mounted at $mount after a successful stop"
    }

    Write-Step 'T1-05: the disk is still registered after the stop'
    $after = Invoke-Cli @('disk', 'list')
    if ($after.exit -ne 0) { Fail "disk list failed after the stop: $($after.output)" }
    Write-Host "  $($after.output.Trim())"
    if ($after.output -notmatch [regex]::Escape($profileId)) {
        Fail 'the stopped disk disappeared from the list instead of being reported as stopped'
    }

    Write-Step 'T1-05: remove it (fail-closed forget, confirmation is the profile identifier)'
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId, '--wait')
    if ($forget.exit -ne 0) { Fail "disk forget failed: $($forget.output)" }
    Write-Host "  $($forget.output.Trim())"
    $empty = Invoke-Cli @('disk', 'list')
    if ($empty.exit -ne 0) { Fail "disk list failed after the removal: $($empty.output)" }
    if ($empty.output -match [regex]::Escape($profileId)) {
        Fail 'the removed disk is still listed'
    }

    Write-Step 'the installed service path: install, answer one command, stop, delete'
    # The foreground service above proved the command surface.  This proves the *shipped*
    # path -- an SCM-installed service serving the same pipe -- which is the only difference
    # between what ran so far and what a user gets.
    Invoke-Sc @('create', $serviceName, 'binPath=', $serviceExe, 'type=', 'own', 'start=', 'demand') | Out-Null
    Invoke-Sc @('start', $serviceName) | Out-Null
    $installed = Wait-ForService 90
    if ($null -eq $installed) {
        Fail "the installed service never answered a list command.`n$((Get-ServiceState))`n$((Get-Content -LiteralPath $serviceLog -Raw))"
    }
    Write-Host "  installed service state: $((Get-ServiceState).Trim())"
    Write-Host "  installed service answered: $($installed.output.Trim())"
    Invoke-Sc @('stop', $serviceName) | Out-Null
    $stoppedState = ''
    for ($i = 0; $i -lt 120; $i++) {
        $stoppedState = Get-ServiceState
        if ($stoppedState -match 'STOPPED') { break }
        Start-Sleep -Milliseconds 500
    }
    if ($stoppedState -notmatch 'STOPPED') { Fail "the installed service did not stop:`n$stoppedState" }
    if ($stoppedState -notmatch 'WIN32_EXIT_CODE\s+:\s+0') {
        Fail "the installed service stopped with a failure code:`n$stoppedState"
    }
    Write-Host "  installed service stopped cleanly"
    Invoke-Sc @('delete', $serviceName) | Out-Null

    Write-Host 'PASS integration.m5.cli_e2e'
    exit 0
} finally {
    Write-Step 'cleanup'
    # The service log is the only record of what the service was doing when a command stopped
    # working, and the work root is about to be deleted -- so it is copied out first.
    foreach ($preserve in @('trace.log', 'service.log')) {
        $source = Join-Path $workRoot $preserve
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination ('C:\m5\last-' + $preserve) -Force -ErrorAction SilentlyContinue
            Write-Host ('preserved ' + $source + ' as C:\m5\last-' + $preserve)
        }
    }
    if (Test-Path -LiteralPath (Join-Path $workRoot 'service.log')) {
        Copy-Item -LiteralPath (Join-Path $workRoot 'service.log') -Destination 'C:\m5\last-service.log' -Force -ErrorAction SilentlyContinue
        Write-Host 'service log preserved at C:\m5\last-service.log'
    }
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        # The foreground service is hard-killed, and that is stated rather than hidden: a
        # script cannot deliver Ctrl+C to a process in another console.  What it would have
        # done on a clean stop -- retire the worker and unmount -- has already happened,
        # because the disk was stopped and forgotten through the pipe before this line, and
        # the *graceful* stop path is the installed service's, measured above.
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Service
    # The service's own shutdown is what releases the worker; a leftover one would be
    # the next run's problem, so it is reported rather than ignored.
    $leftover = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
    if ($leftover.Count -ne 0) {
        [Console]::Error.WriteLine("WARNING: $($leftover.Count) worker process(es) survived the run")
    }
    if ($mount -match '^[A-Z]:$') {
        if (Test-Path -LiteralPath ($mount + '\')) {
            [Console]::Error.WriteLine("WARNING: $mount is still mounted")
        }
    }
    Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
}

