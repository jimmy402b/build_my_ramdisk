# Host-side driver for T1-01: build on the dev machine, execute on the VM.
#
# Why this is not "just run the tests over ssh":
#
#   A Windows SSH session is a **network logon** and it is **not elevated** (an
#   administrator account gets its filtered token under UAC).  This product has
#   two hard requirements that such a session cannot satisfy:
#
#     1. `require_elevated_administrator()` (src/run/runtime_adapter.cpp:147)
#        checks TokenElevation -- device tests and product commands need an
#        elevated token;
#     2. the public control pipe's SDDL contains `(D;;GA;;;NU)`
#        (src/control/authorization.cpp:455), which **denies network logons**.
#
#   So this script uses ssh for **transport only**.  Execution happens in a
#   guest-side agent started by the scheduled task `MyRamDisk-M5-Agent`, which
#   runs with highest privileges and is not a network logon.  See
#   docs/operations/m5-vm-harness.md.
#
# No host address, user name or key is hardcoded: everything comes from
# parameters or from M5VM_HOST / M5VM_USER / M5VM_KEY (AGENTS.md section 5).

[CmdletBinding(DefaultParameterSetName = 'Probe')]
param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [int]$Port = 22,

    [string]$RemoteRoot = 'C:/m5/control',
    [string]$OutDir = 'C:\tmp\m5-vm',
    [int]$TimeoutSeconds = 1800,

    # Connectivity and environment facts.  Changes nothing on the guest.
    [Parameter(ParameterSetName = 'Probe')]
    [switch]$Probe,

    # Ship the test binary tree plus tests.json.  Does not execute anything.
    [Parameter(ParameterSetName = 'Stage')]
    [switch]$Stage,
    [Parameter(ParameterSetName = 'Stage')]
    [string]$Configuration = 'Release',

    # Trigger one agent run over the already staged tree and collect results.
    [Parameter(ParameterSetName = 'Run')]
    [switch]$Run,
    [Parameter(ParameterSetName = 'Run')]
    [string]$TaskName = 'MyRamDisk-M5-Agent',
    # Run only the cases whose name matches this regular expression.  The stage
    # is unaffected, so one staged tree can be re-run with different subsets --
    # which is what locating a single failure needs.
    [Parameter(ParameterSetName = 'Run')]
    [string]$Filter,
    # Replace every case's ctest TIMEOUT with this many seconds.  Used to tell
    # "slow on this VM" apart from "genuinely hung": several verifier cases have
    # a 30 s budget that the guest exceeds.
    [Parameter(ParameterSetName = 'Run')]
    [int]$TimeoutOverrideSeconds = 0,
    # Raise any case's ctest TIMEOUT to at least this many seconds.  A floor
    # rather than an override, so a case that legitimately asks for 300 s keeps
    # its 300 s while a 30 s verifier budget stops failing for being slow.
    [Parameter(ParameterSetName = 'Run')]
    [int]$MinTimeoutSeconds = 0,
    # Run every selected case this many times, on the guest, in one round trip.
    # This is how a reproduction rate is measured for an intermittent failure.
    [Parameter(ParameterSetName = 'Run')]
    [int]$Repeat = 1,
    # Have the test framework name each case on stderr before it runs, so a hard
    # exit still reports which case was executing.
    [Parameter(ParameterSetName = 'Run')]
    [switch]$TraceTestCases,
    # Consecutive (default) repeats each case in a row; Sweep does N whole passes
    # over the selection.  See the guest runner for why both are needed before
    # calling a case intermittently broken.
    [Parameter(ParameterSetName = 'Run')]
    [ValidateSet('Consecutive', 'Sweep')]
    [string]$RepeatMode = 'Consecutive',
    # Seconds to wait for the guest's answer.  0 means "scale with -Repeat",
    # because a sweep takes many times longer than a single pass.
    [Parameter(ParameterSetName = 'Run')]
    [int]$DeadlineSeconds = 0,
    # Wait for and collect an ALREADY SUBMITTED request instead of triggering a
    # new one.  This is the way back from a host-side timeout: the guest run was
    # never interrupted, only the waiting was.
    [Parameter(ParameterSetName = 'Run')]
    [string]$RequestId
)

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------- arguments

if ([string]::IsNullOrWhiteSpace($VmHost)) { throw 'VmHost is required (or set M5VM_HOST).' }
if ([string]::IsNullOrWhiteSpace($VmUser)) { throw 'VmUser is required (or set M5VM_USER).' }

$sshArgs = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
             '-o', 'LogLevel=ERROR', '-p', "$Port")
$scpArgs = @('-q', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
             '-P', "$Port")
if (-not [string]::IsNullOrWhiteSpace($KeyFile)) {
    # IdentitiesOnly keeps this from offering unrelated keys and tripping the
    # guest's MaxAuthTries before our own key is tried.
    $sshArgs += @('-i', $KeyFile, '-o', 'IdentitiesOnly=yes')
    $scpArgs += @('-i', $KeyFile, '-o', 'IdentitiesOnly=yes')
}
$target = "$VmUser@$VmHost"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ------------------------------------------------------------- transport

# Native commands write ordinary diagnostics -- and any remote stderr -- to
# stderr.  Under ErrorActionPreference = 'Stop' PowerShell turns that into a
# terminating error, so a successful call looks like a failure and the exit
# code that should decide is never reached.  Every native call goes through
# here and the exit code is what decides.
function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$Arguments,
        [string[]]$InputLines
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($PSBoundParameters.ContainsKey('InputLines')) {
            # A leading empty line is prepended here, at the single place every
            # line-fed call goes through, because this file is saved with a UTF-8
            # BOM and PowerShell does not strip it from the *first* line it pipes:
            # the BOM arrives glued to that line's first token, so
            # `Get-ChildItem ...` reaches the guest as `\ufeffGet-ChildItem ...` and
            # the guest answers "The term ... is not recognized as the name of a
            # cmdlet".  The empty first line absorbs it.
            #
            # Doing this here rather than at each call site is the correction: it
            # was first added to `Invoke-Remote` alone, which left the direct
            # `Invoke-Native` call that extracts git metadata still broken -- and
            # that call had been failing this way since before this session.
            $output = @('') + $InputLines | & $FilePath @Arguments 2>&1
        } else {
            $output = & $FilePath @Arguments 2>&1
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{ ExitCode = $code; Output = @($output) }
}

function Invoke-Remote {
    param([Parameter(Mandatory)][string]$Script)
    # The body goes over stdin so no quoting rule can rewrite it.
    #
    # Nothing is prepended here: `Invoke-Native` already adds the empty first line
    # that absorbs this file's UTF-8 BOM, and doing it in both places put the BOM on
    # the *second* line -- which broke the call it was meant to fix, in the same way
    # and with the same unhelpful message.
    $a = $sshArgs + @($target, 'powershell.exe -NoProfile -NonInteractive -Command -')
    $r = Invoke-Native -FilePath 'ssh.exe' -Arguments $a -InputLines @($Script)
    if ($r.ExitCode -ne 0) {
        throw "remote command failed (exit $($r.ExitCode)):`n$($r.Output -join "`n")"
    }
    return $r.Output
}

function Copy-ToRemote {
    param([Parameter(Mandatory)][string]$Local, [Parameter(Mandatory)][string]$Remote)
    $a = $scpArgs + @('-r', $Local, "${target}:$Remote")
    $r = Invoke-Native -FilePath 'scp.exe' -Arguments $a
    if ($r.ExitCode -ne 0) { throw "scp failed: $Local -> $Remote ($($r.Output -join ' '))" }
}

function Copy-FromRemote {
    param([Parameter(Mandatory)][string]$Remote, [Parameter(Mandatory)][string]$Local)
    $a = $scpArgs + @('-r', "${target}:$Remote", $Local)
    $r = Invoke-Native -FilePath 'scp.exe' -Arguments $a
    if ($r.ExitCode -ne 0) { throw "scp failed: $Remote -> $Local ($($r.Output -join ' '))" }
}

function Wait-RemoteFile {
    param([Parameter(Mandatory)][string]$Remote, [Parameter(Mandatory)][string]$Local,
          [int]$DeadlineSeconds = $TimeoutSeconds)
    $deadline = [DateTime]::UtcNow.AddSeconds($DeadlineSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 1500
        Remove-Item -LiteralPath $Local -Force -ErrorAction SilentlyContinue
        $a = $scpArgs + @("${target}:$Remote", $Local)
        [void](Invoke-Native -FilePath 'scp.exe' -Arguments $a)
        if ((Test-Path -LiteralPath $Local) -and (Get-Item -LiteralPath $Local).Length -gt 0) { return }
    }
    throw "timed out after $DeadlineSeconds s waiting for $Remote"
}

# ------------------------------------------------------------- probe

function New-ProbeScript {
    # Reports facts only, never mutates.  Every device query is bounded: the
    # guest is disposable, but a probe must not be what wedges it.
    return @'
$ErrorActionPreference = 'Continue'
function Fact($k, $v) { "{0,-22} {1}" -f $k, $v }

$id        = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($id)
Fact 'computer'     $env:COMPUTERNAME
Fact 'user'         $id.Name
Fact 'elevated'     $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Fact 'logon_type'   $id.AuthenticationType
Fact 'session_id'   (Get-Process -Id $PID).SessionId
Fact 'os'           (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption
Fact 'os_build'     ([Environment]::OSVersion.Version).ToString()
Fact 'powershell'   $PSVersionTable.PSVersion.ToString()
Fact 'control_root' (Test-Path 'C:\m5\control')
Fact 'agent_script' (Test-Path 'C:\m5\agent\m5-agent.ps1')

$task = schtasks /query /tn 'MyRamDisk-M5-Agent' 2>$null | Out-String
Fact 'agent_task'   ($task.Trim().Length -gt 0)

$svc = Get-Service -Name phdskmnt -ErrorAction SilentlyContinue
if ($svc) {
    $start = (Get-CimInstance Win32_Service -Filter "Name='phdskmnt'" -ErrorAction SilentlyContinue).StartMode
    Fact 'phdskmnt' ("status={0} start={1}" -f $svc.Status, $start)
} else {
    Fact 'phdskmnt' 'ABSENT'
}

$job = Start-Job { (Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue | Measure-Object).Count }
if (Wait-Job $job -Timeout 60) { Fact 'physical_drives' (Receive-Job $job) } else { Fact 'physical_drives' 'TIMEOUT' }
Remove-Job $job -Force -ErrorAction SilentlyContinue

$c = Get-PSDrive C -ErrorAction SilentlyContinue
if ($c) { Fact 'c_free_gb' ([math]::Round($c.Free / 1GB, 1)) }

Fact 'R_present' (Test-Path 'R:\')
Fact 'S_present' (Test-Path 'S:\')
'@
}

if ($Probe) {
    Write-Host "probing $target ..." -ForegroundColor Cyan
    $report = Invoke-Remote -Script (New-ProbeScript)
    $report | ForEach-Object { Write-Host $_ }
    $report | Set-Content -LiteralPath (Join-Path $OutDir 'probe.txt') -Encoding utf8
    Write-Host "`nsaved: $(Join-Path $OutDir 'probe.txt')"
    return
}

# ------------------------------------------------------------- stage

if ($Stage) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools/m5vm -> repo root
    $build = Join-Path $repo 'out/build/msvc'
    if (-not (Test-Path $build)) { throw "build tree not found: $build" }

    # The staged tree mirrors the DEV-MACHINE layout, because several tests bake
    # absolute paths into the binary at configure time and a binary's constants
    # cannot be rewritten on the way to the guest:
    #
    #   MYRD_M3_*_FIXTURE_PATH   = ${CMAKE_SOURCE_DIR}/tests/fixtures/m3/...
    #   MYRD_M3_*_MATRIX_PATH    = ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/m3/...
    #   MYRD_PRODUCT_EXE_PATH    = $<TARGET_FILE:myrd>   (an absolute build path)
    #
    # Mirroring the layout makes every one of those resolve, and it means the
    # command lines travel VERBATIM -- no rewriting, so no rewriting bugs.
    # The alternative (rewriting paths and patching the tests) is recorded as a
    # follow-up in the harness document; it is the better long-term fix.
    $stageDir = Join-Path $OutDir 'stage'
    Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue

    $testsDest = Join-Path $stageDir "out/build/msvc/tests/$Configuration"
    $srcDest = Join-Path $stageDir "out/build/msvc/src/$Configuration"
    New-Item -ItemType Directory -Force -Path $testsDest, $srcDest | Out-Null

    # Destination directories are created first and SOURCE CONTENTS are copied
    # into them.  `Copy-Item -Recurse <srcDir> <missingDest>` would create <dest>
    # holding srcDir's contents and silently drop the CONFIG level, which breaks
    # MYRD_M5_WORKER_RELPATH = ../../src/<CONFIG>/myrd-m5-worker.exe.
    Copy-Item -Recurse -Force (Join-Path $build "tests/$Configuration/*") $testsDest
    # verify_storage_boundary_<Config>.ps1 sits one level ABOVE tests/<Config>
    # and is named as an argument by the structural case, so it travels too.
    Copy-Item -Force (Join-Path $build 'tests/*.ps1') (Join-Path $stageDir 'out/build/msvc/tests') -ErrorAction SilentlyContinue
    # Every executable the commands reference: myrd-m2-e2e.exe and
    # myrd-m3-aim-server.exe are named as arguments by their own cases, and a
    # missing binary shows up as a test failure rather than as a staging error.
    Copy-Item -Force (Join-Path $build "src/$Configuration/*.exe") $srcDest

    $stagedExes = (Get-ChildItem -LiteralPath $testsDest -Filter *.exe).Count
    if ($stagedExes -eq 0) { throw "no test executables staged from $build/tests/$Configuration" }
    if (-not (Test-Path (Join-Path $srcDest 'myrd-m5-worker.exe'))) {
        throw "the worker was not staged from $build/src/$Configuration"
    }
    Write-Host "staged $stagedExes test binaries + $((Get-ChildItem -LiteralPath $srcDest -Filter *.exe).Count) product binaries for $Configuration"

    # The source side: fixtures, the .ps1 cases, the tools they call, the source
    # files those tools read, and the CMake modules the structural cases load.
    # `src` and `cmake` were missing at first, which failed three cases with
    # "Missing provisioning script", "cannot find ...main.cpp" and "Missing
    # execution-boundary cmake module" -- i.e. the tests read the SOURCE tree,
    # not just the build tree.
    foreach ($sub in @('tests', 'tools', 'src', 'cmake')) {
        $from = Join-Path $repo $sub
        if (Test-Path $from) {
            $to = Join-Path $stageDir $sub
            New-Item -ItemType Directory -Force -Path $to | Out-Null
            Copy-Item -Recurse -Force (Join-Path $from '*') $to
        }
    }
    # A few root files the .ps1 cases read.
    foreach ($f in @('CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'NOTICE', 'README.md', 'THIRD_PARTY.md')) {
        $from = Join-Path $repo $f
        if (Test-Path $from) { Copy-Item -Force $from (Join-Path $stageDir $f) }
    }

    # git metadata.  Several cases record provenance with
    # `git -C <repo> rev-parse HEAD`, so installing the git BINARY is not enough:
    # the mirrored tree needs the repository metadata too, at the paths the
    # worktree's .git pointer names.  A worktree's .git is a FILE pointing at
    # <mainRepo>/.git/worktrees/<name>, so both halves have to travel.
    $mainRepo = Split-Path (Split-Path $repo -Parent) -Parent
    $worktreeGitPointer = Join-Path $repo '.git'
    $mainGit = Join-Path $mainRepo '.git'
    if ((Test-Path $worktreeGitPointer) -and (Test-Path $mainGit)) {
        Copy-Item -Force $worktreeGitPointer (Join-Path $stageDir '.git')
        $gitMetaTar = Join-Path $OutDir 'git-meta.tgz'
        Remove-Item $gitMetaTar -Force -ErrorAction SilentlyContinue
        & tar.exe -czf $gitMetaTar -C $mainRepo '.git'
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $gitMetaTar)) {
            throw "failed to archive $mainGit for the guest"
        }
        Write-Host ("git metadata: .git pointer + {0} MB archive" -f [math]::Round((Get-Item $gitMetaTar).Length / 1MB, 1))
    } else {
        # Not fatal: the cases that need provenance will report it themselves.
        Write-Host 'git metadata: not staged (no .git pointer or no main .git)' -ForegroundColor Yellow
    }
    # The structural probes live under out/build/ (outside msvc/) and are written
    # by the tests, so the directories have to exist.
    #
    # The loop variable is NOT called $probe: PowerShell variable names are
    # case-insensitive, so that would assign to this script's own [switch]$Probe
    # parameter and fail type conversion.
    foreach ($probeDir in @('execution-boundary-probes', 'storage-boundary-probes')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $stageDir "out/build/$probeDir") | Out-Null
    }

    # ctest's own JSON report is the single authoritative test list.  The lines
    # are joined before parsing: ConvertFrom-Json takes one string, and an array
    # of lines is not that.
    $json = & ctest --test-dir $build -C $Configuration --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { throw 'ctest --show-only=json-v1 failed' }
    $spec = ($json -join "`n") | ConvertFrom-Json

    # Commands travel verbatim: the mirrored layout makes the baked paths valid.
    #
    # WORKING_DIRECTORY travels too.  It was missing at first, so the runner used
    # the executable's own directory -- a different launch context than CTest's
    # (for every C++ case CTest uses `.../msvc/tests`, the PARENT of `Release`).
    # Measured on the M2 backing-generation crash: the working directory is not
    # what makes it crash (4/10 vs 2/10 across the two contexts), but a test that
    # reads a fixture relative to its working directory would behave differently,
    # so the reading is not trustworthy without this.
    $tests = foreach ($t in $spec.tests) {
        $props = $t.properties
        [pscustomobject]@{
            name             = $t.name
            command          = @($t.command)
            timeout          = if ($props) { ($props | Where-Object { $_.name -eq 'TIMEOUT' }).value } else { $null }
            workingDirectory = if ($props) { ($props | Where-Object { $_.name -eq 'WORKING_DIRECTORY' }).value } else { $null }
        }
    }
    $tests | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stageDir 'tests.json') -Encoding utf8
    Write-Host "staged $($tests.Count) tests -> $stageDir"

    Invoke-Remote -Script "New-Item -ItemType Directory -Force -Path '$RemoteRoot' | Out-Null; 'ok'" | Out-Null

    # The tree lands at the DEV path on the guest, because that is what the
    # baked-in constants point at.
    $guestRepoWin = $repo
    # Refuse to aim a recursive delete at anything that is not this project's
    # checkout path.  This runs on the GUEST over ssh, but a wrong value here
    # would still be destructive, so it is checked rather than assumed.
    if ([string]::IsNullOrWhiteSpace($guestRepoWin) -or
        $guestRepoWin -notmatch 'build_my_ramdisk' -or
        $guestRepoWin -notmatch 'm5-lifecycle\s*$') {
        throw "refusing to clean an unexpected guest path: '$guestRepoWin'"
    }
    if ($guestRepoWin -notmatch '^[A-Za-z]:\\') {
        throw "guest repo path must be an absolute Windows path: '$guestRepoWin'"
    }

    # Remove the previous tree first.  A layout change would otherwise leave
    # stale binaries that look like this run's and never get overwritten, which
    # is exactly what happened the first time this ran.
    #
    # The remote shell for an ssh command is cmd.exe, so this is plain cmd: it
    # deletes a tree far more simply than a multi-line PowerShell payload over
    # stdin.
    $clean = 'rmdir /s /q "' + $guestRepoWin + '" 2>nul & echo cleaned'
    $cleaned = Invoke-Native -FilePath 'ssh.exe' -Arguments ($sshArgs + @($target, $clean))
    if ($cleaned.ExitCode -ne 0) {
        throw "remote cleanup failed (exit $($cleaned.ExitCode)): $($cleaned.Output -join ' ')"
    }
    # Then recreate the path (with its parents) before copying into it.
    Invoke-Remote -Script "New-Item -ItemType Directory -Force -Path '$($guestRepoWin -replace '\\','/')' | Out-Null; 'ok'" | Out-Null

    # Copied one item at a time.  A wildcard would not be expanded on the way
    # from PowerShell to a native program, and letting scp glob it on its own
    # produces a different result than intended.
    #
    # The stage tree is ENUMERATED rather than listed: a hardcoded list silently
    # stopped copying `src`, `cmake`, the root files and `.git` when each was
    # added, and the symptom was a test failure on the guest rather than a
    # staging error.
    $guestRepoScp = $guestRepoWin -replace '\\', '/'
    Copy-ToRemote -Local (Join-Path $stageDir 'tests.json') -Remote "$RemoteRoot/tests.json"
    foreach ($entry in (Get-ChildItem -LiteralPath $stageDir -Force | Where-Object { $_.Name -ne 'tests.json' })) {
        Copy-ToRemote -Local $entry.FullName -Remote "$guestRepoScp/$($entry.Name)"
    }

    # The main repository's git metadata goes to its own mirrored path, because
    # that is where the worktree's .git pointer sends git.
    $gitMetaTar = Join-Path $OutDir 'git-meta.tgz'
    if (Test-Path $gitMetaTar) {
        $mainRepoScp = $mainRepo -replace '\\', '/'
        Invoke-Remote -Script "New-Item -ItemType Directory -Force -Path '$mainRepoScp' | Out-Null; 'ok'" | Out-Null
        Copy-ToRemote -Local $gitMetaTar -Remote "$mainRepoScp/.m5-git-meta.tgz"
        $extract = "& 'C:\Windows\system32\tar.exe' -xzf '$mainRepoScp/.m5-git-meta.tgz' -C '$mainRepoScp'; " +
                   "Remove-Item '$mainRepoScp/.m5-git-meta.tgz' -Force -ErrorAction SilentlyContinue; " +
                   "'git metadata extracted to $mainRepoScp/.git'"
        Invoke-Remote -Script $extract | ForEach-Object { Write-Host "  $_" }
    }

    # Verify the payload actually landed, and fail loudly if it did not.
    #
    # This exists because staging reported "staged 116 test binaries" and then ran
    # against a guest whose test directory was EMPTY, twice in one session.  The
    # symptom was the runner printing
    # `MISSING .../myrd_m5_production_execution_backend_tests.exe`, which reads
    # exactly like a build problem -- so the failure sent the investigation to the
    # wrong place, and one re-stage "fixed" a defect that had never existed.
    #
    # Counting on the guest is the only statement that cannot be satisfied by a
    # successful `scp` exit code, and the number is the same one printed above, so
    # a mismatch is unambiguous.
    $guestExeDir = "$guestRepoWin\out\build\msvc\tests\$Configuration"
    # A .NET call rather than a cmdlet, and that is the second correction here.  The
    # first version used `Get-ChildItem` and was defeated by this file's UTF-8 BOM
    # being glued to the cmdlet name; adding a blank line to absorb it then put the
    # BOM on the *second* line of another call and broke that one instead.  A static
    # .NET call has no cmdlet name to corrupt, so the fragility is removed rather
    # than worked around.
    $countScript = "[System.IO.Directory]::GetFiles('$guestExeDir', '*.exe').Length"
    $guestExeLines = @(Invoke-Remote -Script $countScript)
    $guestExeCount = -1
    foreach ($line in $guestExeLines) {
        $parsed = 0
        if ([int]::TryParse(([string]$line).Trim(), [ref]$parsed)) { $guestExeCount = $parsed }
    }
    if ($guestExeCount -ne $stagedExes) {
        throw ("staging did not land: the guest has $guestExeCount test executables in " +
               "$guestExeDir but $stagedExes were staged.  Re-run -Stage; do not treat the " +
               "run failure that follows as a product defect.")
    }

    Write-Host "staged to ${target}:$guestRepoScp  (tests.json in $RemoteRoot)"
    return
}

# ------------------------------------------------------------- run

if ($Run) {
    # Collecting an already submitted request is how a host-side timeout is
    # recovered: the guest run was never interrupted, only the waiting was.  A
    # 5x sweep of the full suite takes ~50 minutes and blew through the single
    # run default, which then looked like a failed run when it was not.
    $collecting = -not [string]::IsNullOrWhiteSpace($RequestId)
    if ($collecting) {
        $id = $RequestId
        Write-Host "collecting already submitted request $id (not triggering a run)"
    } else {
        $id = [guid]::NewGuid().ToString('N')
    }
    $requestLocal = Join-Path $OutDir "$id.request.ps1"
    # The request body does one thing: call the guest's test executor.  The
    # agent owns bounded execution and the response envelope; how tests run is
    # Invoke-M5Tests.ps1's business.
    $call = "& 'C:/m5/agent/Invoke-M5Tests.ps1' -ControlRoot '$RemoteRoot' -RequestId '$id' -TestsJson 'tests.json'"
    if (-not [string]::IsNullOrWhiteSpace($Filter)) {
        # Single quotes are the safe delimiter here: the filter is a regex the
        # guest's PowerShell will parse, and only an embedded quote could end it.
        if ($Filter.Contains("'")) { throw "Filter may not contain a single quote: $Filter" }
        $call += " -Filter '$Filter'"
    }
    if ($TimeoutOverrideSeconds -gt 0) {
        $call += " -TimeoutOverrideSeconds $TimeoutOverrideSeconds"
    }
    if ($MinTimeoutSeconds -gt 0) {
        $call += " -MinTimeoutSeconds $MinTimeoutSeconds"
    }
    if ($Repeat -gt 1) {
        $call += " -Repeat $Repeat"
    }
    if ($TraceTestCases) {
        $call += " -TraceTestCases"
    }
    if ($RepeatMode -ne 'Consecutive') {
        $call += " -RepeatMode $RepeatMode"
    }
    @(
        '$ErrorActionPreference = ''Stop'''
        $call
    ) | Set-Content -LiteralPath $requestLocal -Encoding utf8
    if (-not [string]::IsNullOrWhiteSpace($Filter)) { Write-Host "filter: $Filter" }
    if ($TimeoutOverrideSeconds -gt 0) { Write-Host "timeout override: ${TimeoutOverrideSeconds}s" }

    if ($collecting) {
        Write-Host "request $id was submitted earlier; waiting for its response ..."
    } else {
        Copy-ToRemote -Local $requestLocal -Remote "$RemoteRoot/$id.request.ps1"
        Invoke-Remote -Script "schtasks /run /tn '$TaskName' | Out-Null; 'triggered'" | Out-Null
        Write-Host "request $id submitted; waiting for response ..."
    }

    # Scale the wait with the work: one pass of the full suite runs ~10 minutes,
    # so a sweep of N passes cannot be waited for with the single-run default.
    $waitSeconds = if ($DeadlineSeconds -gt 0) {
        $DeadlineSeconds
    } elseif ($Repeat -gt 1) {
        $TimeoutSeconds * $Repeat
    } else {
        $TimeoutSeconds
    }
    Write-Host "deadline: ${waitSeconds}s"
    $responseLocal = Join-Path $OutDir "$id.response.txt"
    try {
        Wait-RemoteFile -Remote "$RemoteRoot/$id.response.txt" -Local $responseLocal `
                        -DeadlineSeconds $waitSeconds
    } catch {
        # The guest keeps working; only this wait gave up.  Say so, with the way
        # back in, instead of reporting a failure the run did not have.
        throw ("$($_.Exception.Message)`n" +
               "the guest run is NOT interrupted by this. Collect it later with:`n" +
               "  -Run -RequestId $id`n" +
               "or raise -DeadlineSeconds (currently ${waitSeconds}s).")
    }
    Get-Content -LiteralPath $responseLocal | ForEach-Object { Write-Host $_ }

    $resultsLocal = Join-Path $OutDir "$id.results.json"
    try {
        Copy-FromRemote -Remote "$RemoteRoot/$id.results.json" -Local $resultsLocal
        # Normalised with an explicit loop for the same reason as on the guest:
        # ConvertFrom-Json writes a JSON array as ONE object, so a single-case
        # run would leave .Count null and print "0 of  passed".
        $rawResults = Get-Content -LiteralPath $resultsLocal -Raw | ConvertFrom-Json
        $results = New-Object System.Collections.ArrayList
        foreach ($x in $rawResults) { [void]$results.Add($x) }
        $failed = New-Object System.Collections.ArrayList
        foreach ($x in $results) { if ($x.exit_code -ne 0) { [void]$failed.Add($x) } }
        Write-Host ''
        Write-Host ("{0} of {1} passed" -f ($results.Count - $failed.Count), $results.Count)
        foreach ($x in $failed) { Write-Host ("  FAIL {0} (exit {1})" -f $x.name, $x.exit_code) -ForegroundColor Red }
    } catch {
        Write-Host "no results.json collected: $_" -ForegroundColor Yellow
    }
    Write-Host "`nartifacts in $OutDir"
    return
}

Write-Host 'nothing to do: pass -Probe, -Stage or -Run'
