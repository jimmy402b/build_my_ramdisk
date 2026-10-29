# Guest-side test executor for T1-01.
#
# Started by m5-agent.ps1 with highest privileges.  Reads the staged tests.json,
# runs every test **serially**, writes <RequestId>.results.json.
#
# Why strictly serial: these cases create machine-wide `PhysicalDrive` devices.
# The previous round measured the consequence (transfer manifest section 1.39):
# overlapping cases contend for one device namespace and show up as "create
# cannot find a present disk interface" or "removal timed out".  Serial is the
# only correct choice here.
#
# tests.json's command[0] was rewritten by the host into the guest's staged
# path, so it is used verbatim.

[CmdletBinding()]
param(
    [string]$ControlRoot = 'C:/m5/control',
    [Parameter(Mandatory)][string]$RequestId,
    [string]$TestsJson = 'tests.json',
    [int]$DefaultTimeoutSeconds = 300,
    # Only run cases whose name matches this regular expression.
    [string]$Filter,
    # Non-zero replaces every case's ctest TIMEOUT with this many seconds.
    [int]$TimeoutOverrideSeconds = 0,
    # Non-zero raises any case's ctest TIMEOUT to at least this many seconds.
    # A floor, not an override: the guest is slower than the machine the ctest
    # budgets were written for, but a case that legitimately asks for 300 s must
    # keep its 300 s.  Measured: four verifier cases with a 30/60 s budget pass
    # at 180 s and were not hung.
    [int]$MinTimeoutSeconds = 0,
    # Run every selected case this many times.  Repeating on the GUEST keeps a
    # reproduction-rate measurement to one round trip instead of one per run --
    # important when the case under study takes ~55 s.
    [int]$Repeat = 1,
    # Ask the test framework to name each case on stderr before it runs, so a
    # hard exit still says which case was executing.  See tests/support/test_main.cpp.
    [switch]$TraceTestCases,
    # How -Repeat orders its runs.
    #
    #   Consecutive (default) -- one case N times in a row.  Best signal for a
    #     SINGLE case's flakiness, but a case that leaves a device or a volume
    #     behind can fail its OWN next run, which reads as a flake when it is
    #     self-interference.
    #   Sweep -- a whole pass over the selected set per iteration, i.e. N genuine
    #     full passes.  Slower to gather samples per case, but it is the ordering
    #     that matches how the suite is really run, so it separates real
    #     unreliability from consecutive-run interference.
    #
    # Run both and compare before calling anything intermittently broken.
    [ValidateSet('Consecutive', 'Sweep')]
    [string]$RepeatMode = 'Consecutive'
)

$ErrorActionPreference = 'Continue'

# The scheduled task's process can carry a PATH block that was captured before
# git was installed (the Task Scheduler service holds its own environment).  The
# known install locations are therefore prepended explicitly: this makes a REAL
# installation findable, it does not substitute for one.
foreach ($toolPath in @('C:\Program Files\Git\cmd', 'C:\Program Files\Git\mingw64\bin')) {
    if (Test-Path -LiteralPath $toolPath) { $env:Path = $toolPath + ';' + $env:Path }
}

# Windows command-line quoting, so an argument that contains a space (for
# example the cl.exe path under "Program Files (x86)") survives the trip to the
# child's argv intact.  Joining with spaces without this silently splits it.
function Quote-Arg([string]$value) {
    if ([string]::IsNullOrEmpty($value)) { return '""' }
    if ($value -notmatch '[\s"]') { return $value }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $backslashes = 0
    foreach ($ch in $value.ToCharArray()) {
        if ($ch -eq '\') { $backslashes++; continue }
        if ($ch -eq '"') {
            [void]$sb.Append('\' * ($backslashes * 2 + 1)); [void]$sb.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) { [void]$sb.Append('\' * $backslashes); $backslashes = 0 }
        [void]$sb.Append($ch)
    }
    if ($backslashes -gt 0) { [void]$sb.Append('\' * ($backslashes * 2)) }
    [void]$sb.Append('"')
    return $sb.ToString()
}

$specPath = Join-Path $ControlRoot $TestsJson
if (-not (Test-Path -LiteralPath $specPath)) { throw "tests.json not found: $specPath" }

# Normalised with an explicit loop, not `@(... | ConvertFrom-Json)`.
# ConvertFrom-Json returns a JSON array as ONE object, so wrapping it in @()
# produces a single-element array whose element is the array -- the filter then
# matches one "case" whose name is "System.Object[]".  foreach enumerates the
# array properly, which is what makes this unambiguous.
$raw = Get-Content -LiteralPath $specPath -Raw | ConvertFrom-Json
$tests = New-Object System.Collections.ArrayList
foreach ($t in $raw) { [void]$tests.Add($t) }
if ($tests.Count -eq 0) { throw "no cases in $specPath" }

if (-not [string]::IsNullOrWhiteSpace($Filter)) {
    $selected = New-Object System.Collections.ArrayList
    foreach ($t in $tests) { if ($t.name -match $Filter) { [void]$selected.Add($t) } }
    Write-Host ("filter '{0}': {1} of {2} cases selected" -f $Filter, $selected.Count, $tests.Count)
    if ($selected.Count -eq 0) { throw "the filter matched no case: $Filter" }
    $tests = $selected
}

$work = Join-Path $ControlRoot "run-$RequestId"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Expand each selected case into $Repeat runs, each with its own label and its
# own captures.  A repeat count of 1 leaves the names exactly as before.
#
# The pair order is the whole point: Consecutive walks the iterations inside a
# case, Sweep walks the cases inside an iteration.  See -RepeatMode.
$runs = New-Object System.Collections.ArrayList
if ($RepeatMode -eq 'Sweep') {
    for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
        foreach ($t in $tests) {
            $label = if ($Repeat -gt 1) { '{0}#{1}' -f $t.name, $iteration } else { $t.name }
            [void]$runs.Add([pscustomobject]@{
                name             = $label
                command          = $t.command
                timeout          = $t.timeout
                workingDirectory = $t.workingDirectory
            })
        }
    }
} else {
    foreach ($t in $tests) {
        for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
            $label = if ($Repeat -gt 1) { '{0}#{1}' -f $t.name, $iteration } else { $t.name }
            [void]$runs.Add([pscustomobject]@{
                name             = $label
                command          = $t.command
                timeout          = $t.timeout
                workingDirectory = $t.workingDirectory
            })
        }
    }
}

$results = New-Object System.Collections.ArrayList
$index = 0

foreach ($t in $runs) {
    $index++
    $name = $t.name
    $command = @($t.command)
    if ($command.Count -eq 0) { continue }

    $exe = [string]$command[0]
    $argv = if ($command.Count -gt 1) { @($command[1..($command.Count - 1)]) } else { @() }
    # The override exists to separate "slow on this VM" from "genuinely hung":
    # several verifier cases carry a 30 s ctest budget that the guest exceeds.
    $timeout = if ($TimeoutOverrideSeconds -gt 0) {
        $TimeoutOverrideSeconds
    } elseif ($t.timeout) {
        [int]$t.timeout
    } else {
        $DefaultTimeoutSeconds
    }
    if ($MinTimeoutSeconds -gt 0 -and $timeout -lt $MinTimeoutSeconds) {
        $timeout = $MinTimeoutSeconds
    }

    $safe = ($name -replace '[^A-Za-z0-9._-]', '_')
    $outFile = Join-Path $work "$safe.stdout.txt"
    $errFile = Join-Path $work "$safe.stderr.txt"

    Write-Host ("[{0}/{1}] {2}" -f $index, $runs.Count, $name)

    if (-not (Test-Path -LiteralPath $exe)) {
        [void]$results.Add([pscustomobject]@{
            name = $name; status = 'fail'; exit_code = -1; seconds = 0.0
            stdout = $outFile; stderr = $errFile; note = "executable not staged: $exe"
        })
        Write-Host "        MISSING $exe" -ForegroundColor Red
        continue
    }

    $started = Get-Date
    $proc = $null
    try {
        # The child's stdout/stderr go to FILES, not pipes.
        #
        # A piped read only completes when every writer closes the pipe, and a
        # test that spawns a worker passes that pipe to the worker.  Measured: a
        # test killed on timeout left an orphan myrd-m5-worker holding the pipe,
        # the async read never finished, and the whole run wedged for 30 minutes
        # with a live AIM device still claimed.  Files cannot be held open that
        # way, and they survive the process, so a crash still leaves evidence.
        #
        # cmd is used because ProcessStartInfo cannot redirect to a file.  The
        # `/c ""...""` form is the one that survives a quoted program path.
        #
        # The guest marker is set *inside* this cmd shell rather than through
        # `ProcessStartInfo.EnvironmentVariables`, and that is a correction: the
        # environment form was measured not to reach the test through the `cmd /c`
        # wrapper, so a case that refuses to run off the guest refused on the guest
        # too and the acceptance became unmeasurable.  Setting it in the shell that
        # spawns the test is inheritance the wrapper cannot lose.
        $inner = 'set MYRD_M5VM_GUEST=1&& ' + (Quote-Arg $exe)
        if ($argv.Count -gt 0) {
            $inner += ' ' + (($argv | ForEach-Object { Quote-Arg ([string]$_) }) -join ' ')
        }
        $inner += ' > ' + (Quote-Arg $outFile) + ' 2> ' + (Quote-Arg $errFile)

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $env:ComSpec
        $psi.Arguments = '/c "' + $inner + '"'
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        # CTest's own working directory, not the executable's.  See the staging
        # side for why the difference matters.
        $runCwd = if ($t.workingDirectory) { [string]$t.workingDirectory } else { Split-Path -Parent $exe }
        $psi.WorkingDirectory = $runCwd
        if ($TraceTestCases) { $psi.EnvironmentVariables['MYRD_TEST_TRACE'] = '1' }
        # Tells a test that creates real processes that it is on the guest, which is
        # the only place it may run (`AGENTS.md` section 4).  Cases that need real
        # processes refuse to start without it, deliberately loudly rather than by
        # skipping -- a silently skipped case leaves an acceptance unmeasured while
        # the suite reports success.
        $psi.EnvironmentVariables['MYRD_M5VM_GUEST'] = '1'

        $proc = [System.Diagnostics.Process]::Start($psi)

        if (-not $proc.WaitForExit($timeout * 1000)) {
            # Kill the TREE, not just the shell: Process.Kill() leaves the test's
            # own children behind, and those are what hold devices.  taskkill /T
            # takes the whole tree.
            & taskkill.exe /T /F /PID $proc.Id 2>&1 | Out-Null
            [void]$proc.WaitForExit(30000)
            # A device-holding orphan is not acceptable collateral, so sweep any
            # myrd process the killed tree left behind.
            foreach ($orphan in @(Get-Process -ErrorAction SilentlyContinue |
                                  Where-Object { $_.ProcessName -like 'myrd*' })) {
                try { $orphan.Kill() } catch { }
            }
            [void]$results.Add([pscustomobject]@{
                name = $name; status = 'timeout'; exit_code = 'TIMEOUT'
                seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
                stdout = $outFile; stderr = $errFile; note = "exceeded ${timeout}s"
            })
            Write-Host "        TIMEOUT after ${timeout}s" -ForegroundColor Yellow
            continue
        }

        $code = $proc.ExitCode
        # cmd returns the child's code, but a test that spawned a survivor can
        # still leave one behind on a NORMAL exit; sweep those too.
        foreach ($orphan in @(Get-Process -ErrorAction SilentlyContinue |
                              Where-Object { $_.ProcessName -like 'myrd*' })) {
            try { $orphan.Kill() } catch { }
        }
    } catch {
        $code = -2
        Add-Content -LiteralPath $errFile -Value "runner: $_" -Encoding utf8
    } finally {
        if ($proc) { $proc.Dispose() }
    }

    # **77 is the suite's SKIP convention.**  Two structural cases need Visual Studio
    # (vswhere) and a guest without build tools used to look like a product regression;
    # they now exit 77 and are counted separately instead of failing the run.
    $status = if ($code -eq 77) { 'skip' } elseif ($code -ne 0) { 'fail' } else { 'pass' }
    [void]$results.Add([pscustomobject]@{
        name = $name; status = $status; exit_code = $code
        seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
        stdout = $outFile; stderr = $errFile; note = ''
    })
    if ($status -eq 'skip') { Write-Host "        SKIP exit=$code" -ForegroundColor Yellow }
    elseif ($status -ne 'pass') { Write-Host "        FAIL exit=$code" -ForegroundColor Red }
}

# **Atomic, and validated.**  Measured failure mode: a `results.json` written by a
# single `Set-Content` was once left as 357 spaces, and the host read it as an empty
# result set instead of noticing the write had gone wrong.  Write a temporary file,
# rename it into place (a rename is atomic on one volume), then read it back and
# refuse to claim a run finished if it does not parse.
$resultsPath = Join-Path $ControlRoot "$RequestId.results.json"
$temporaryPath = "$resultsPath.tmp"
$json = $results | ConvertTo-Json -Depth 4
Set-Content -LiteralPath $temporaryPath -Value $json -Encoding utf8
Move-Item -LiteralPath $temporaryPath -Destination $resultsPath -Force
# **Enumerated, not wrapped.**  PS 5.1's ConvertFrom-Json returns a top-level JSON
# array as one object, so `@(...)` around it counts 1 no matter how many entries it
# holds; the first version of this check therefore reported "wrote 96, read back 1"
# on a perfectly good file.  `foreach` is what enumerates the array.
$parsedBack = Get-Content -LiteralPath $resultsPath -Raw | ConvertFrom-Json
$written = @(foreach ($entryBack in $parsedBack) { $entryBack })
if ($written.Count -ne $results.Count) {
    throw ("results.json did not round-trip: wrote {0} entries, read back {1}" -f
        $results.Count, $written.Count)
}
foreach ($entry in @($written)) {
    if ([string]::IsNullOrWhiteSpace([string]$entry.name)) {
        throw 'results.json contains an entry without a name'
    }
}

$failed = @($results | Where-Object { $_.status -in @('fail', 'timeout') })
$skipped = @($results | Where-Object { $_.status -eq 'skip' })
Write-Host ''
Write-Host ("{0} of {1} passed" -f ($results.Count - $failed.Count - $skipped.Count), $results.Count)
if ($skipped.Count -ne 0) {
    Write-Host ("{0} skipped" -f $skipped.Count) -ForegroundColor Yellow
    $skipped | ForEach-Object { Write-Host ("  SKIP {0}" -f $_.name) -ForegroundColor Yellow }
}
$failed | ForEach-Object { Write-Host ("  FAIL {0} (exit {1})" -f $_.name, $_.exit_code) -ForegroundColor Red }
Write-Host "results: $resultsPath"
Write-Host "captures: $work"
