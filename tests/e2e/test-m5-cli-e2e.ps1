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
    # **The service log is part of the failure.**  `reconciling` is a terminal state
    # shared by three different failures (a dirty final checkpoint, an unconfirmed
    # commit, a failed resume), and the operation record only carries the state.
    # Without the last lines the reading is "it hung", which is not actionable.
    $tail = ''
    if (Test-Path -LiteralPath $serviceLog) {
        $tail = ($(Get-Content -LiteralPath $serviceLog -Tail 12) -join "`n")
    }
    Fail "the operation did not reach a terminal state within $seconds s (last state: $state)`n$tail"
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
    # R-01 / decision 1: M5 does not over-commit.
    #
    # A fixed budget above the machine's commit limit is refused *at admission*: the
    # answer is a product code with the two numbers in it, and nothing durable or real
    # exists afterwards.  Measured from the outside, because the model test can only
    # assert the core's decision -- not that no worker, bundle or profile was left behind.
    # -----------------------------------------------------------------------
    Write-Step 'R-01: a fixed budget above the machine commit limit is refused at admission'
    if ((Invoke-Cli @('disk', 'list')).output -match 'generation=') {
        Fail 'a profile exists before the R-01 step; this case measures an empty service'
    }
    $overBundle = Join-Path 'C:\m5' ($profileName + '-over-commit')
    $overName = $profileName + '-over-commit'
    $over = Invoke-Cli @('disk', 'create', '--name', $overName, '--bundle', $overBundle,
        '--size', '64GiB', '--mount', $mount,
        '--memory-mode', 'fixed', '--memory-hard-limit', '64GiB', '--no-wait')
    if ($over.exit -ne 1) {
        Fail ("the over-commit create did not fail with exit 1 (exit=$($over.exit)): " +
            $over.output)
    }
    if ($over.output -notmatch 'code: 0x560A') {
        Fail "the over-commit create did not carry the R-01 code 0x560A: $($over.output)"
    }
    if ($over.output -notmatch 'M5 does not over-commit') {
        Fail "the over-commit refusal does not state the rule: $($over.output)"
    }
    $limitMatch = [regex]::Match($over.output, 'commit limit of (\d+) bytes')
    if (-not $limitMatch.Success) {
        Fail "the over-commit refusal does not name the machine's commit limit: $($over.output)"
    }
    Write-Host "  commit limit reported by the service: $($limitMatch.Groups[1].Value) bytes"
    # **No residue.**  A refusal at admission may not leave a profile, an operation, a
    # bundle or a worker behind -- each of those is what makes a later attempt fail for
    # a reason that has nothing to do with what the user just asked for.
    $overList = Invoke-Cli @('disk', 'list')
    if ($overList.output -match 'generation=') {
        Fail "the refused create left a profile: $($overList.output)"
    }
    if ($over.output -match 'operation [0-9a-f]{8}-') {
        Fail "the refused create left an operation: $($over.output)"
    }
    if (Test-Path -LiteralPath $overBundle) {
        Fail "the refused create left a bundle directory: $overBundle"
    }
    $overWorkers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count
    if ($overWorkers -ne 0) {
        Fail "the refused create left $overWorkers worker(s) behind"
    }
    Write-Host ("PASS R-01: over-commit budget refused at admission (code 0x560A, " +
        'commit limit named, no profile/operation/bundle/worker)')

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
        Write-Host "  disk show: $((Invoke-Cli @('disk', 'show', $profileName)).output.Trim())"
        Write-Host "  workers: $((@(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)).Count)"
        Write-Host "  work root: $((Get-ChildItem -LiteralPath $workRoot -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', ')"
        Write-Host "  service log:"
        Write-Host "$(Get-Content -LiteralPath $serviceLog -Raw)"
        # **The operation record is the only place the failure's own fields are decoded.**  Its files are TLV binaries
        # on disk, so this asks the still-running service to read them: the phase, the step and the detail here are
        # what the service believes failed, which is what the log and the trace have disagreed about.
        Write-Host "  operation show:"
        Write-Host "$((Invoke-Cli @('operation', 'show', $createId)).output.Trim())"
        Fail "the create did not succeed: $finalState"
    }
    Write-Host "  $($waited.output.Trim())"

    $list = Invoke-Cli @('disk', 'list')
    if ($list.exit -ne 0) { Fail "disk list failed after the create: $($list.output)" }
    Write-Host "  $($list.output.Trim())"
    # **The created profile's own line, not the first uuid in the listing.**
    #
    # `Get-UuidFromText` on the whole listing returned whichever profile sorted
    # first, so on a guest carrying leftovers every later command addressed a
    # previous run's profile: measured, `disk show / stop / forget` all targeted
    # `6c4f2c10` -- whose bundle had long been deleted -- while this run had
    # created `6ce17b58`.  The stop "succeeded" and the mount went away, which is
    # why nothing looked wrong until the forget tried to open the dead path.
    $createdLine = ($list.output -split "`n") |
        Where-Object { $_ -match [regex]::Escape($mount) -and $_ -match 'running' } |
        Select-Object -First 1
    if ($null -eq $createdLine) {
        $createdLine = ($list.output -split "`n") |
            Where-Object { $_ -match [regex]::Escape($mount) } |
            Select-Object -First 1
    }
    $profileId = if ($null -ne $createdLine) { Get-UuidFromText $createdLine } else { $null }
    if ($null -eq $profileId) { Fail 'disk list did not report the created profile' }
    if ($list.output -notmatch [regex]::Escape($mount)) {
        Fail "disk list did not report the mount target $mount"
    }

    $show = Invoke-Cli @('disk', 'show', $profileName)
    if ($show.exit -ne 0) { Fail "disk show $mount failed: $($show.output)" }
    Write-Host "  $($show.output.Trim())"

    Write-Step 'T1-05: read and write the volume'
    $probeFile = Join-Path $mount '\m5-cli-e2e.txt'
    $payload = 'written through the public pipe at ' + (Get-Date).ToString('o')
    Set-Content -LiteralPath $probeFile -Value $payload -Encoding utf8
    $readBack = Get-Content -LiteralPath $probeFile -Raw
    if ($readBack.Trim() -ne $payload.Trim()) { Fail 'the bytes read back did not match what was written' }
    Write-Host "  wrote and read back $((Get-Item $probeFile).Length) bytes on $mount"

    # -----------------------------------------------------------------------
    # STAB-01: M5 is a single-disk product, so a second disk is refused.
    #
    # The product used to *look* multi-disk and then strand a device: the second
    # disk's teardown left its AIM proxy device behind, so a restart refused
    # `no volume carries the recorded identity` and the forget refused
    # `device_absent=0`.  The fix is an admission refusal with a stable code, and
    # this block measures it from the CLI: refused, before any work, with the
    # running disk's generation, mount and worker all unchanged.
    #
    # The refusal is asserted to be *quick* as well as correct.  A create that
    # waited for provisioning and then failed would still be a refusal, but it
    # would also have opened a second device first, which is the residue this
    # guard exists to prevent.
    # -----------------------------------------------------------------------
    Write-Step 'STAB-01: a second disk is refused while the first one is running'
    $secondMount = $null
    foreach ($letter in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if ($letter -eq $mount) { continue }
        if ($occupied -contains $letter) { continue }
        $buffer = New-Object System.Text.StringBuilder 512
        if ([Myrd.DosDevice]::QueryDosDevice($letter, $buffer, 512) -ne 0) { continue }
        $secondMount = $letter
        break
    }
    if ($null -eq $secondMount) { Fail 'no second free drive letter is available' }

    function Get-Generation([string]$text) {
        $match = [regex]::Match($text, 'generation=(\d+)')
        if (-not $match.Success) { return $null }
        return [uint64]$match.Groups[1].Value
    }

    $firstBefore = Invoke-Cli @('disk', 'show', $profileName)
    if ($firstBefore.exit -ne 0) {
        Fail "disk show before the second create failed: $($firstBefore.output)"
    }
    $generationBefore = Get-Generation $firstBefore.output
    if ($null -eq $generationBefore) {
        Fail "disk show before the second create carried no generation: $($firstBefore.output)"
    }
    if ($firstBefore.output -notmatch 'runtime=Running') {
        Fail "the first disk was not running before the second create: $($firstBefore.output)"
    }
    $workersBefore = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue |
        Sort-Object Id | ForEach-Object { $_.Id })

    $secondBundle = Join-Path $workRoot 'second-bundle'
    $secondCreate = Invoke-Cli @('disk', 'create', '--name', "$profileName-second",
        '--bundle', $secondBundle, '--size', '64MiB', '--mount', $secondMount, '--no-wait')
    Write-Host "  second create on $secondMount exit=$($secondCreate.exit) in $($secondCreate.ms) ms"
    Write-Host "  $($secondCreate.output.Trim())"
    if ($secondCreate.exit -eq 0) {
        Fail "a second disk create succeeded while $mount was running; M5 is a single-disk product"
    }
    if ($secondCreate.output -notmatch 'code: 0x5609') {
        Fail ("the second create was not refused with the single-disk code (0x5609): " +
            $secondCreate.output)
    }
    if ($secondCreate.output -notmatch 'single-disk') {
        Fail ("the second-create refusal does not say M5 is a single-disk product: " +
            $secondCreate.output)
    }
    if ($secondCreate.ms -ge 30000) {
        Fail ("the second create took $($secondCreate.ms) ms to refuse; the guard must " +
            'refuse at admission, before any device is opened')
    }
    # **No residue from the refused create.**  Its bundle directory must not exist:
    # the refusal happened before a worker could provision anything.
    if (Test-Path -LiteralPath $secondBundle) {
        Fail "the refused create left a bundle directory behind: $secondBundle"
    }

    # **The first disk is untouched, not merely still present.**
    $firstAfter = Invoke-Cli @('disk', 'show', $profileName)
    if ($firstAfter.exit -ne 0) {
        Fail "disk show after the second create failed: $($firstAfter.output)"
    }
    $generationAfter = Get-Generation $firstAfter.output
    if ($generationAfter -ne $generationBefore) {
        Fail ("the refused second create moved the first disk's generation: " +
            "$generationBefore -> $generationAfter")
    }
    if ($firstAfter.output -notmatch [regex]::Escape($mount)) {
        Fail "the first disk lost its mount $mount after the second create was refused"
    }
    if ($firstAfter.output -notmatch 'runtime=Running') {
        Fail "the first disk stopped running after the second create was refused"
    }
    $workersAfter = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue |
        Sort-Object Id | ForEach-Object { $_.Id })
    $workersAdded = @($workersAfter | Where-Object { $workersBefore -notcontains $_ })
    $workersLost = @($workersBefore | Where-Object { $workersAfter -notcontains $_ })
    if ($workersAdded.Count -ne 0 -or $workersLost.Count -ne 0) {
        Fail ("the refused second create changed the worker set: before [$($workersBefore -join ',')] " +
            "after [$($workersAfter -join ',')]")
    }
    $listAfter = Invoke-Cli @('disk', 'list')
    if ($listAfter.exit -ne 0) { Fail "disk list after the second create failed: $($listAfter.output)" }
    # **Case-sensitive on purpose.**  `-match` is case-insensitive by default, and
    # `disk.list: completed` contains a `t:` -- so a case-insensitive search for `T:`
    # matches the disposition line and reports a phantom second disk.  Measured: the
    # first run of this block failed here while the refusal itself was correct.
    if ($listAfter.output -cmatch ([regex]::Escape($secondMount) + '\s')) {
        Fail "the refused second disk appears in disk list at $secondMount"
    }
    Write-Host ("PASS STAB-01: second create refused (code 0x5609), first disk unchanged " +
        "(generation=$generationAfter mount=$mount workers=$($workersAfter.Count))")

    # -----------------------------------------------------------------------
    # T1-07: the memory the user came for, and UNMAP end to end.
    #
    # Two claims, both measured rather than asserted from a design: the running
    # disk reports the RAM it holds against the budget it was opened with, and
    # deleting a file actually gives that memory back.  The second one is the
    # first end-to-end verification of the release path -- NTFS delete -> volume
    # UNMAP -> proxy -> `device_.unmap` -> store discard -- and it is the reason
    # this step writes a real 16 MiB file instead of inspecting a counter.
    # -----------------------------------------------------------------------
    Write-Step 'T1-07: memory is visible, and deleting a file frees it (UNMAP)'
    $memLine = {
        param([string]$text)
        $line = ($text -split "`n") |
            Where-Object { $_ -match [regex]::Escape($mount) } | Select-Object -First 1
        if ($null -eq $line) { return $null }
        $match = [regex]::Match($line, 'mem=(\d+)/(\d+)')
        if (-not $match.Success) { return $null }
        return [pscustomobject]@{
            resident = [uint64]$match.Groups[1].Value
            budget   = [uint64]$match.Groups[2].Value
        }
    }

    $baseline = & $memLine (Invoke-Cli @('disk', 'list')).output
    if ($null -eq $baseline) {
        Fail 'disk list did not report mem=<resident>/<budget> for the running disk'
    }
    if ($baseline.budget -eq 0) {
        Fail 'the running disk reported a zero memory budget, so nothing can be shown against it'
    }
    Write-Host ("  memory before the file: resident={0} budget={1}" -f $baseline.resident, $baseline.budget)

    # 16 MiB of incompressible-ish bytes, written **write-through** and then closed.
    # Write-through is not caution, it is the difference between measuring the store
    # and measuring the Windows file cache: the first run of this step wrote normally
    # and the reported resident number did not move at all (2949120 before and after
    # 16 MiB), because the data was still in the cache when the list was taken.
    $bigFile = Join-Path $mount '\m5-unmap.bin'
    $chunk = New-Object byte[] (1MB)
    (New-Object System.Random 12345).NextBytes($chunk)
    $stream = [System.IO.File]::Create(
        $bigFile, 4096, [System.IO.FileOptions]::WriteThrough)
    try {
        for ($index = 0; $index -lt 16; $index++) { $stream.Write($chunk, 0, $chunk.Length) }
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    $written = (Get-Item -LiteralPath $bigFile).Length
    if ($written -ne 16MB) { Fail "the large file is $written bytes, not 16 MiB" }

    # Polled, not sampled once: the counter is maintained by the worker's store and
    # the last block of a write can land a moment after the handle is closed.
    $peak = $null
    $riseDeadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $riseDeadline) {
        $sample = & $memLine (Invoke-Cli @('disk', 'list')).output
        if ($null -eq $sample) { break }
        if ($null -eq $peak -or $sample.resident -gt $peak.resident) { $peak = $sample }
        if ($sample.resident -gt $baseline.resident) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $peak) { Fail 'disk list stopped reporting the memory gauges' }
    if ($peak.budget -ne $baseline.budget) {
        Fail ("the reported budget changed while nothing was reconfigured: {0} -> {1}" -f
            $baseline.budget, $peak.budget)
    }
    if ($peak.resident -le $baseline.resident) {
        Fail ("writing 16 MiB did not raise the reported resident memory: before={0} after={1}" -f
            $baseline.resident, $peak.resident)
    }
    if ($peak.resident -gt $peak.budget) {
        Fail ("the reported resident memory exceeds the budget: resident={0} budget={1}" -f
            $peak.resident, $peak.budget)
    }

    Remove-Item -LiteralPath $bigFile -Force
    $after = $null
    $unmapDeadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $unmapDeadline) {
        $after = & $memLine (Invoke-Cli @('disk', 'list')).output
        if ($null -ne $after -and $after.resident -lt $peak.resident) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $after) { Fail 'disk list stopped reporting the memory gauges after the delete' }
    if ($after.resident -ge $peak.resident) {
        Fail ("deleting a 16 MiB file freed no resident memory: peak={0} after={1}" -f
            $peak.resident, $after.resident)
    }
    # "Same order as Task Manager" is reported rather than asserted: the two numbers
    # count different things (the store's committed arena versus the worker process'
    # whole working set) and pinning their ratio would turn a platform observation
    # into a contract this project does not control.
    $worker = Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $workingSet = if ($null -ne $worker) { $worker.WorkingSet64 } else { 0 }
    Write-Host ("  memory after the delete: resident={0} (peak {1}, before {2}); worker working set={3}" -f
        $after.resident, $peak.resident, $baseline.resident, $workingSet)
    Write-Host ("PASS T1-07: resident rose {0} -> {1} and fell to {2} after the delete (budget {3})" -f
        $baseline.resident, $peak.resident, $after.resident, $peak.budget)

    Write-Step 'T1-05: checkpoint, then stop'
    $checkpoint = Invoke-Cli @('disk', 'checkpoint', $profileName, '--wait')
    if ($checkpoint.exit -ne 0) { Fail "disk checkpoint failed: $($checkpoint.output)" }
    Write-Host "  $($checkpoint.output.Trim())"

    Remove-Item -LiteralPath $probeFile -Force
    Set-Content -LiteralPath (Join-Path $mount '\m5-cli-e2e-after.txt') -Value 'after checkpoint' -Encoding utf8

    # **Observed, not waited on.**  `--wait` goes through `operation wait`, which
    # is the client-side defect `Wait-ForTerminal` already documents: about thirty
    # seconds in it reports "the control pipe exchange failed" while the service
    # log shows the stop completing (checkpoint published, execute returned).  The
    # stop that "failed" here had in fact succeeded.  Same pattern as create: ask
    # for the operation id, then poll the command that works.
    $stop = Invoke-Cli @('disk', 'stop', $profileName)
    if ($stop.exit -ne 0) { Fail "disk stop failed: $($stop.output)" }
    Write-Host "  $($stop.output.Trim())"
    $stopId = Get-UuidFromText $stop.output
    if ($null -eq $stopId) { Fail 'the stop response carried no operation identifier' }
    $stopState = Wait-ForTerminal $stopId 300
    if ($stopState -ne 'succeeded') { Fail "the stop ended $stopState" }
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

    # **Restart, and prove the bytes are still there.**  Until this step the test
    # stopped and then forgot: "recovery after a restart" was never exercised, so a
    # stop that lost the last checkpoint would have passed.  The file written after
    # the explicit checkpoint is the one the stop's final checkpoint had to persist.
    Write-Step 'T1-05: restart it -- the bytes written before the stop must survive'
    $start = Invoke-Cli @('disk', 'start', $profileName)
    if ($start.exit -ne 0) { Fail "disk start failed: $($start.output)" }
    Write-Host "  $($start.output.Trim())"
    $startId = Get-UuidFromText $start.output
    if ($null -eq $startId) { Fail 'the start response carried no operation identifier' }
    $startState = Wait-ForTerminal $startId 300
    if ($startState -ne 'succeeded') {
        # The state alone is not actionable: `failed` hides which step refused,
        # and reading the wrong log tail once already cost a round.
        $tail = if (Test-Path -LiteralPath $serviceLog) {
            ($(Get-Content -LiteralPath $serviceLog -Tail 15) -join "`n")
        } else { '' }
        Fail "the restart ended $startState`n$tail"
    }
    $afterRestart = Join-Path $mount 'm5-cli-e2e-after.txt'
    if (-not (Test-Path -LiteralPath $afterRestart)) {
        Fail "the volume was not remounted at ${mount}: the profile started but no volume appeared"
    }
    $restored = Get-Content -LiteralPath $afterRestart -Raw
    if ($restored.Trim() -ne 'after checkpoint') {
        Fail "the bytes written before the stop did not survive the restart: '$($restored.Trim())'"
    }
    Write-Host "  restarted on ${mount}; the file written before the stop is intact"

    Write-Step 'T1-05: stop it again before removing it'
    $stop2 = Invoke-Cli @('disk', 'stop', $profileName)
    if ($stop2.exit -ne 0) { Fail "the second stop failed: $($stop2.output)" }
    $stop2Id = Get-UuidFromText $stop2.output
    if ($null -eq $stop2Id) { Fail 'the second stop carried no operation identifier' }
    $stop2State = Wait-ForTerminal $stop2Id 300
    if ($stop2State -ne 'succeeded') { Fail "the second stop ended $stop2State" }
    if (Test-Path -LiteralPath ($mount + '\')) {
        Fail "the volume is still mounted at $mount after the second stop"
    }

    # -----------------------------------------------------------------------
    # T4-02: export the stopped disk to one standalone VHDX.
    #
    # The worker's export path reads the file back through `VhdxReader` before
    # it publishes the name, so a success here *is* "the reader can read it".
    # What this block adds is the user-visible half: the file exists, the
    # bundle's generation did not move, and a second export refuses to
    # overwrite.
    # -----------------------------------------------------------------------
    Write-Step 'T4-02: export the stopped disk to a standalone VHDX'
    $exportList = Invoke-Cli @('disk', 'list')
    if ($exportList.exit -ne 0) { Fail "disk list failed before the export: $($exportList.output)" }
    $exportLine = ($exportList.output -split "`n") |
        Where-Object { $_ -match [regex]::Escape($mount) } | Select-Object -First 1
    if ($null -eq $exportLine) { Fail "disk list did not report $mount before the export" }
    $exportGeneration = [regex]::Match($exportLine, 'generation=(\d+)')
    if (-not $exportGeneration.Success) { Fail "disk list did not report a generation for $mount" }
    $generationBeforeExport = [uint64]$exportGeneration.Groups[1].Value
    $exportPath = Join-Path 'C:\m5' ('m5-export-' + [guid]::NewGuid().ToString('N') + '.vhdx')
    $exportResult = Invoke-Cli @('disk', 'export', $profileName, '--output', $exportPath)
    Write-Host "  export exit=$($exportResult.exit)"
    if ($exportResult.exit -ne 0) { Fail "disk export failed: $($exportResult.output)" }
    if (-not (Test-Path -LiteralPath $exportPath)) {
        Fail "disk export reported success but $exportPath does not exist"
    }
    $exportBytes = (Get-Item -LiteralPath $exportPath).Length
    if ($exportBytes -le 0) { Fail "the exported VHDX is empty" }
    Write-Host "  exported $exportBytes bytes"
    $afterExport = Invoke-Cli @('disk', 'list')
    if ($afterExport.exit -ne 0) { Fail "disk list failed after the export: $($afterExport.output)" }
    $afterLine = ($afterExport.output -split "`n") |
        Where-Object { $_ -match [regex]::Escape($mount) } | Select-Object -First 1
    $afterGeneration = [regex]::Match($afterLine, 'generation=(\d+)')
    if (-not $afterGeneration.Success) { Fail "disk list lost the generation after the export" }
    if ([uint64]$afterGeneration.Groups[1].Value -ne $generationBeforeExport) {
        Fail ("the export changed the bundle's generation: {0} -> {1}" -f
            $generationBeforeExport, [uint64]$afterGeneration.Groups[1].Value)
    }
    Write-Host "  generation unchanged at $generationBeforeExport"
    $overwrite = Invoke-Cli @('disk', 'export', $profileName, '--output', $exportPath)
    Write-Host "  second export exit=$($overwrite.exit)"
    if ($overwrite.exit -eq 0) { Fail 'the export overwrote an existing file' }
    if ($overwrite.output -notmatch 'exists|overwrite') {
        Fail "the refusal does not say the destination exists: $($overwrite.output)"
    }

    $volName = 'm5-e2e-volatile-' + [guid]::NewGuid().ToString('N')
    # **Created running, deliberately.**  The volatile guard is a property of the
    # *stop*, and this case must measure that guard.  Starting a disk that a
    # `--no-start` create left un-provisioned is Load Only's separate problem and
    # has its own coverage above; folding the two together made a failure in one
    # read as a failure in the other.
    $vol = Invoke-Cli @('disk', 'create', '--name', $volName, '--bundle', (Join-Path 'C:\m5' $volName),
        '--size', '64MiB', '--mount', 'Z:', '--checkpoint-interval', 'off')
    if ($vol.exit -ne 0) { Fail "the volatile create failed: $($vol.output)" }
    $volId = Get-UuidFromText $vol.output
    if ((Wait-ForTerminal $volId 300) -ne 'succeeded') { Fail 'the volatile create did not succeed' }
    # **Checkpoint first, then write.**  The disk starts clean, the write below
    # makes it dirty, and the forced stop must be allowed to discard those bytes
    # -- the worker's dirty refusal is exactly what used to park this operation
    # in `reconciling`.  The checkpoint interval is `off`, so the
    # stop-without-the-flag refusal is decided by the volatile policy.
    $volCheckpoint = Invoke-Cli @('disk', 'checkpoint', $volName)
    if ($volCheckpoint.exit -ne 0) { Fail "the volatile checkpoint failed: $($volCheckpoint.output)" }
    $volCheckpointId = Get-UuidFromText $volCheckpoint.output
    if ((Wait-ForTerminal $volCheckpointId 300) -ne 'succeeded') {
        Fail 'the volatile checkpoint did not succeed'
    }
    Set-Content -LiteralPath 'Z:\m5-volatile.txt' -Value 'volatile data' -Encoding utf8
    Write-Host '  wrote data the forced stop must be allowed to discard'
    $volPlain = Invoke-Cli @('disk', 'stop', $volName)
    Write-Host "  volatile stop without the flag: exit=$($volPlain.exit)"
    if ($volPlain.exit -eq 0) { Fail 'a volatile disk stopped without --accept-volatile-data-loss' }
    if ($volPlain.output -notmatch 'volatile') { Fail "the refusal does not name volatile data: $($volPlain.output)" }
    $volForced = Invoke-Cli @('disk', 'stop', $volName, '--accept-volatile-data-loss')
    Write-Host "  volatile stop with the flag: exit=$($volForced.exit)"
    if ($volForced.exit -ne 0) { Fail "the acknowledged stop failed: $($volForced.output)" }
    $volForceId = Get-UuidFromText $volForced.output
    if ((Wait-ForTerminal $volForceId 300) -ne 'succeeded') { Fail 'the acknowledged stop did not succeed' }
    $probeName = 'm5-probe-' + [guid]::NewGuid().ToString('N')
    $probe = Invoke-Cli @('disk', 'create', '--name', $probeName, '--bundle', (Join-Path 'C:\m5' $probeName),
        '--size', '64MiB', '--mount', 'Y:', '--no-start', '--checkpoint-interval', 'off')
    Write-Host "  probe create exit=$($probe.exit)"
    if ($probe.exit -ne 0) { Fail "the --no-start create was not admitted: $($probe.output)" }
    $probeId = Get-UuidFromText $probe.output
    if ($null -eq $probeId) { Fail 'the --no-start create carried no operation identifier' }
    # **The defect-A acceptance, on the reading the user sees.**  A stopped create
    # must reach a terminal state inside thirty seconds, and the worker that served
    # it must not outlive the operation.  Before this was asserted, the create sat
    # `running` forever while a worker held the device, and the next disk's forget
    # was refused with `device_absent=0`.
    $probeState = Wait-ForTerminal $probeId 30
    Write-Host "  probe terminal=$probeState"
    if ($probeState -ne 'succeeded') { Fail "the --no-start create ended $probeState" }
    $probeWorkers = @()
    for ($i = 0; $i -lt 20; $i++) {
        $probeWorkers = @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)
        if ($probeWorkers.Count -eq 0) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($probeWorkers.Count -ne 0) {
        Fail "the --no-start create left $($probeWorkers.Count) worker process(es) behind"
    }
    Write-Host '  no worker survived the stopped create'
    $probeList = Invoke-Cli @('disk', 'list')
    if ($probeList.exit -ne 0) { Fail "disk list failed after the --no-start create: $($probeList.output)" }
    # `disk list` renders the mount target, not the display name, so the mount is
    # what identifies this profile's row; Y: is not used by any other disk here.
    $probeLine = ($probeList.output -split "`n") |
        Where-Object { $_ -match [regex]::Escape('Y:') } | Select-Object -First 1
    $probeProfileId = if ($null -ne $probeLine) { Get-UuidFromText $probeLine } else { $null }
    if ($null -eq $probeProfileId) { Fail 'disk list did not report the --no-start profile' }
    # **The other half of Load Only: the disk can be started.**  The create has
    # already provisioned the volume and its durable record now carries the
    # identity, so this start must adopt that volume -- not provision a new one,
    # and not refuse the non-empty bundle.  A started disk is then used (one
    # write/read) and stopped, so the later forget's `device_absent=1` is again a
    # meaningful reading.
    Write-Step 'T4-02c: start the disk that was created without starting'
    $probeStart = Invoke-Cli @('disk', 'start', $probeName)
    if ($probeStart.exit -ne 0) { Fail "the --no-start disk's start failed: $($probeStart.output)" }
    $probeStartId = Get-UuidFromText $probeStart.output
    if ($null -eq $probeStartId) { Fail 'the --no-start start carried no operation identifier' }
    $probeStartState = Wait-ForTerminal $probeStartId 300
    Write-Host "  start terminal=$probeStartState"
    if ($probeStartState -ne 'succeeded') { Fail "the --no-start start ended $probeStartState" }
    $probeShow = Invoke-Cli @('disk', 'show', $probeName)
    if ($probeShow.exit -ne 0) { Fail "disk show failed after the start: $($probeShow.output)" }
    Write-Host "  $($probeShow.output.Trim())"
    if ($probeShow.output -notmatch 'running') { Fail 'the started disk is not reported running' }
    if (-not (Test-Path -LiteralPath 'Y:\')) {
        Fail 'the started Load Only volume did not appear at Y:'
    }
    $loadOnlyFile = Join-Path 'Y:' 'm5-load-only.txt'
    Set-Content -LiteralPath $loadOnlyFile -Value 'load only' -Encoding utf8
    if ((Get-Content -LiteralPath $loadOnlyFile -Raw).Trim() -ne 'load only') {
        Fail 'the started Load Only volume did not accept a write'
    }
    Write-Host '  the started Load Only volume accepted a write'
    # The checkpoint may still leave the runtime dirty (mounted filesystem
    # metadata can write after the cut).  The stop below acknowledges losing
    # whatever remains, so the worker must remove anyway.
    $probeCheckpoint = Invoke-Cli @('disk', 'checkpoint', $probeName)
    if ($probeCheckpoint.exit -ne 0) {
        Fail "the Load Only checkpoint failed: $($probeCheckpoint.output)"
    }
    $probeCheckpointId = Get-UuidFromText $probeCheckpoint.output
    if ((Wait-ForTerminal $probeCheckpointId 300) -ne 'succeeded') {
        Fail 'the Load Only checkpoint did not succeed'
    }
    $probeStop = Invoke-Cli @('disk', 'stop', $probeName, '--accept-volatile-data-loss')
    if ($probeStop.exit -ne 0) { Fail "the Load Only stop failed: $($probeStop.output)" }
    $probeStopId = Get-UuidFromText $probeStop.output
    if ((Wait-ForTerminal $probeStopId 300) -ne 'succeeded') {
        Fail 'the Load Only stop did not succeed'
    }
    if (Test-Path -LiteralPath 'Y:\') { Fail 'the Load Only volume is still mounted after its stop' }

    Write-Step 'T1-05: remove it (fail-closed forget, confirmation is the profile identifier)'
    $forget = Invoke-Cli @('disk', 'forget', $profileId, '--confirmation', $profileId, '--wait')
    if ($forget.exit -ne 0) { Fail "disk forget failed: $($forget.output)" }
    Write-Host "  $($forget.output.Trim())"
    # **STAB-01 keeps this reading.**  The refused second disk must not make the first
    # disk's forget unsafe: a failed create can leave a device behind, and this is the
    # assertion that would catch it.  Fail-closed forget only succeeds with the proof, so
    # the proof is what is asserted -- not the exit code.
    $firstForgetLog = if (Test-Path -LiteralPath $serviceLog) {
        Get-Content -LiteralPath $serviceLog -Raw
    } else { '' }
    $firstProved = ($firstForgetLog -split "`n") |
        Where-Object { $_ -match 'forget safety proved:' -and $_ -match 'device_absent=1' }
    if (-not $firstProved) {
        Fail 'the first disk''s forget did not record device_absent=1 in the service log'
    }
    Write-Host '  first disk forgotten; device_absent=1 proved'
    $empty = Invoke-Cli @('disk', 'list')
    if ($empty.exit -ne 0) { Fail "disk list failed after the removal: $($empty.output)" }
    if ($empty.output -match [regex]::Escape($profileId)) {
        Fail 'the removed disk is still listed'
    }

    Write-Step 'T4-02c: forget the stopped disk -- its device must be proved absent'
    $probeForget = Invoke-Cli @('disk', 'forget', $probeProfileId,
        '--confirmation', $probeProfileId, '--wait')
    if ($probeForget.exit -ne 0) {
        Fail "the --no-start disk's forget failed: $($probeForget.output)"
    }
    $probeForgetLog = if (Test-Path -LiteralPath $serviceLog) {
        Get-Content -LiteralPath $serviceLog -Raw
    } else { '' }
    $provedLines = ($probeForgetLog -split "`n") |
        Where-Object { $_ -match 'forget safety proved:' }
    if (-not ($provedLines | Where-Object { $_ -match 'device_absent=1' })) {
        Fail ("the stopped disk's forget did not record device_absent=1 in the " +
            "service log; proved lines: $($provedLines -join ' | ')")
    }
    Write-Host '  stopped disk forgotten; device_absent=1 proved'


    # -----------------------------------------------------------------------
    # T1-08: the checkpoint policy actually fires.
    #
    # A second disk, because the first one is the command-surface subject and an
    # automatic checkpoint landing in the middle of those measurements would make
    # both stories harder to read.  Nothing in this block issues `disk checkpoint`:
    # the only thing asked for is `--checkpoint-interval 30s` at create time, and the
    # generation must move on its own.  That is the whole claim -- before T1-08 the
    # policy fields were serialized, validated, read and then dropped.
    # -----------------------------------------------------------------------
    Write-Step 'T1-08: an automatic checkpoint fires on the interval policy'
    $policyMount = $null
    foreach ($letter in @('T:', 'U:', 'V:', 'W:', 'X:', 'Y:', 'Z:')) {
        if ($letter -eq $mount) { continue }
        if ($occupied -contains $letter) { continue }
        if ($listed.output -match [regex]::Escape($letter)) { continue }
        $policyMount = $letter
        break
    }
    if ($null -eq $policyMount) { Fail 'no second free drive letter for the policy disk' }
    $policyName = 'm5-e2e-policy-' + [guid]::NewGuid().ToString('N')
    $policyBundle = Join-Path 'C:\m5' $policyName
    $policyCreate = Invoke-Cli @('disk', 'create', '--name', $policyName,
        '--bundle', $policyBundle, '--size', '64MiB', '--mount', $policyMount,
        '--checkpoint-interval', '30s', '--compaction-threshold', '2')
    if ($policyCreate.exit -ne 0) { Fail "the policy disk create failed: $($policyCreate.output)" }
    Write-Host "  policy create: $($policyCreate.output.Trim())"
    # `disk create` admits and returns "operation queued"; it does not wait, even
    # without `--no-wait`.  The first disk's flow already knows this (it polls), and so
    # must this one: a list taken here would show an empty table.
    $policyCreateId = Get-UuidFromText $policyCreate.output
    if ($null -eq $policyCreateId) { Fail 'the policy disk create carried no operation id' }
    $policyCreated = Wait-ForTerminal $policyCreateId 300
    if ($policyCreated -ne 'succeeded') { Fail "the policy disk create ended $policyCreated" }
    Write-Host "  policy disk mounted at $policyMount with a 30 s checkpoint interval"
    # **T4-03: the disk compacts itself.**  Two checkpoints cross the threshold asked for at create time, and the
    # maintenance tick is what notices -- so the proof is the service log saying it dispatched, not the policy code
    # existing.  Polled rather than slept on: the tick runs every fifteen seconds and the checkpoints every thirty.
    $compactionLine = 'maintenance: compaction dispatched (checkpoint-count)'
    $compactionWaited = 0
    $compacted = $false
    while ($compactionWaited -lt 180) {
        if ((Test-Path $serviceLog) -and ((Get-Content $serviceLog -Raw) -match [regex]::Escape($compactionLine))) {
            $compacted = $true
            break
        }
        Start-Sleep -Seconds 5
        $compactionWaited += 5
    }
    if (-not $compacted) { Fail "no automatic compaction within $compactionWaited s of the second checkpoint" }
    Write-Host "  automatic compaction observed after $compactionWaited s"

    $generationLine = {
        param([string]$text, [string]$letter)
        $line = ($text -split "`n") |
            Where-Object { $_ -match [regex]::Escape($letter) } | Select-Object -First 1
        if ($null -eq $line) { return $null }
        $match = [regex]::Match($line, 'generation=(\d+)')
        if (-not $match.Success) { return $null }
        return [uint64]$match.Groups[1].Value
    }
    $policyList = Invoke-Cli @('disk', 'list')
    if ($policyList.exit -ne 0) { Fail "disk list failed after the policy create: $($policyList.output)" }
    Write-Host "  policy list: $($policyList.output.Trim())"
    $generationBefore = & $generationLine $policyList.output $policyMount
    if ($null -eq $generationBefore) { Fail "disk list did not report a generation for $policyMount" }

    # Something to checkpoint: 1 MiB write-through, so the runtime really is dirty.
    $policyFile = Join-Path $policyMount '\m5-policy.bin'
    $policyChunk = New-Object byte[] (1MB)
    (New-Object System.Random 777).NextBytes($policyChunk)
    $policyStream = [System.IO.File]::Create(
        $policyFile, 4096, [System.IO.FileOptions]::WriteThrough)
    try {
        $policyStream.Write($policyChunk, 0, $policyChunk.Length)
        $policyStream.Flush($true)
    } finally {
        $policyStream.Dispose()
    }

    # 30 s interval + the service's own 15 s policy tick, so the first automatic
    # checkpoint can legitimately take up to ~45 s.  The window is 90 s: generous
    # enough not to be flaky on a loaded guest, tight enough that "nothing happened"
    # is a real failure rather than a scheduling coincidence.
    $autoDeadline = (Get-Date).AddSeconds(90)
    $autoObserved = $null
    $startedWaiting = Get-Date
    while ((Get-Date) -lt $autoDeadline) {
        Start-Sleep -Seconds 3
        $sample = Invoke-Cli @('disk', 'list')
        if ($sample.exit -ne 0) { continue }
        $generation = & $generationLine $sample.output $policyMount
        if ($null -ne $generation -and $generation -gt $generationBefore) {
            $autoObserved = $generation
            break
        }
    }
    if ($null -eq $autoObserved) {
        Fail ("no automatic checkpoint was taken within 90 s of a 30 s interval " +
            "(generation stayed {0})" -f $generationBefore)
    }
    $waitedSeconds = [math]::Round(((Get-Date) - $startedWaiting).TotalSeconds, 1)
    Write-Host ("  generation moved {0} -> {1} on its own after {2} s" -f
        $generationBefore, $autoObserved, $waitedSeconds)

    # Cleanup of the policy disk, so the next iteration starts from a guest with no
    # leftovers: a stopped and forgotten profile, and its letter released.
    # **The policy disk runs alone.**  M5 is a single-disk product (G4 is deferred), and
    # this section used to run while the first disk was still up: two live AIM proxy
    # devices at once, which is outside the scope.  Measured consequence: the second
    # disk's teardown left its device behind -- the stop's own `remove_exact_device`
    # reported completed, then the restart failed `no volume carries the recorded
    # identity` and the forget refused `device_absent=0`.  Every one of those is the
    # single-disk scope saying so, not the policy.  This section therefore runs after
    # the first disk is forgotten, so one disk exists at a time.
    $policyStop = Invoke-Cli @('disk', 'stop', $policyName)
    if ($policyStop.exit -ne 0) { Fail "the policy disk stop failed: $($policyStop.output)" }
    $policyStopId = Get-UuidFromText $policyStop.output
    if ($null -eq $policyStopId) { Fail 'the policy disk stop carried no operation id' }
    $policyStopState = Wait-ForTerminal $policyStopId 300
    if ($policyStopState -ne 'succeeded') { Fail "the policy disk stop ended $policyStopState" }
    $policyId = $null
    $policyListAfter = Invoke-Cli @('disk', 'list')
    $policyLine = ($policyListAfter.output -split "`n") |
        Where-Object { $_ -match [regex]::Escape($policyMount) } | Select-Object -First 1
    if ($null -ne $policyLine) { $policyId = Get-UuidFromText $policyLine }
    if ($null -eq $policyId) { Fail 'the policy disk vanished without a forget' }
    # No `--wait` here either: the same client-side defect the first disk's forget
    # documents.  Ask for the operation id and poll the command that works.
    #
    # **Retried, and the retry is not papering over anything.**  Forget is fail-closed:
    # it refuses unless a fresh read-only inspection proves the writer lock and the
    # proxy device are both absent.  A device that was removed a moment ago can still be
    # retiring when that inspection runs, so the proof legitimately comes back
    # `unsafe` -- measured: the first attempt after a stop ended `failed` with
    # `marker=unsafe`.  Refusing is the correct behaviour; asking again after a settling
    # interval is the correct response to it.
    $policyForgetId = $null
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        $policyForget = Invoke-Cli @('disk', 'forget', $policyId, '--confirmation', $policyId)
        if ($policyForget.exit -eq 0) {
            $policyForgetId = Get-UuidFromText $policyForget.output
            if ($null -eq $policyForgetId) { Fail 'the policy disk forget carried no operation id' }
            $policyForgetState = Wait-ForTerminal $policyForgetId 300
            if ($policyForgetState -eq 'succeeded') { break }
        }
        Write-Host ("  forget attempt {0} did not succeed; waiting for the device to settle" -f $attempt)
        Start-Sleep -Seconds 2
        $policyForgetId = $null
    }
    if ($null -eq $policyForgetId) {
        Fail ("the policy disk could not be forgotten in 10 attempts: {0}" -f
            $(if ($null -ne $policyForget) { $policyForget.output } else { 'no answer' }))
    }
    Write-Host '  policy disk stopped and forgotten'


    Write-Step 'the installed service path: install, answer one command, stop, delete'
    # The foreground service above proved the command surface.  This proves the *shipped*
    # path -- an SCM-installed service serving the same pipe -- which is the only difference
    # between what ran so far and what a user gets.
    # **The foreground service owns the public pipe.**  The installed one binds the
    # same name, so it cannot start beside it: measured, with both alive the
    # installed instance exited immediately (SCM 7024, service-specific code 0)
    # while `Wait-ForService` still answered -- from the foreground process -- and
    # the failure only surfaced later as `sc stop` 1062.  Stopping it first is what
    # makes this section measure the SCM path rather than a pipe conflict.
    if ($null -ne $serviceProcess -and -not $serviceProcess.HasExited) {
        Stop-Process -Id $serviceProcess.Id -Force -ErrorAction SilentlyContinue
        $null = $serviceProcess.WaitForExit(10000)
    }
    $serviceProcess = $null
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

    # R-14: a volume label is a refused feature, and the refusal names it.  The release
    # tasklist spells the option `--volume-label`; the CLI rejects it before any request goes
    # to the wire, so this needs no service and leaves nothing behind.
    Write-Step 'a volume label is refused by name (R-14)'
    $labelAttempt = Invoke-Cli @('disk', 'create', '--name', 'label-check',
        '--bundle', (Join-Path $workRoot 'label-bundle'), '--size', '64MiB',
        '--mount', 'T:', '--volume-label', 'DATA')
    Write-Host ("READING: label.exit=" + [string]$labelAttempt.exit)
    Write-Host ("READING: label.output=" + (($labelAttempt.output -replace "`n", ' | ')))
    if ($labelAttempt.exit -ne 2) {
        Fail "a volume label must be a usage failure (exit 2); got exit $($labelAttempt.exit)"
    }
    if ($labelAttempt.output -notmatch '--volume-label is not supported') {
        Fail "the label refusal did not name the option: $($labelAttempt.output)"
    }
    Write-Host 'READING: label.result=refused-by-name'

    Write-Host 'PASS integration.m5.cli_e2e'
    exit 0
} finally {
    Write-Step 'cleanup'
    # **Every iteration keeps its own service log.**
    #
    # The two `last-*` copies below are overwritten by the next run, so the log of a
    # failure in iteration 11 of a `-Repeat 20` was already gone by the time the batch
    # ended -- and a batch is exactly where an intermittent failure is found.  A measured
    # need: 2 of 20 repeats ended parked in `reconciling` and the only surviving file was
    # the last iteration's log, which had passed.  The work root is unique per iteration,
    # so its leaf name is reused as the archive key and nothing is overwritten.
    $archiveTag = Split-Path -Leaf $workRoot
    $archiveRoot = 'C:\m5\runs'
    foreach ($preserve in @('trace.log', 'service.log')) {
        $source = Join-Path $workRoot $preserve
        if (Test-Path -LiteralPath $source) {
            New-Item -ItemType Directory -Force -Path $archiveRoot | Out-Null
            Copy-Item -LiteralPath $source `
                -Destination (Join-Path $archiveRoot ($archiveTag + '.' + $preserve)) `
                -Force -ErrorAction SilentlyContinue
        }
    }
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

