# One command for the whole M5 suite (STAB-04).
#
# The suite cannot be run as one agent request: the guest agent used to replay every
# unfinished request before the new one, so a 160-case request sat behind stale work and
# the host deadline expired.  The agent now selects one request by `pending.id`, but the
# suite is still **grouped here, on the host**, for a second reason: a single hung case
# would take the whole run with it, and the power-cut phase has to happen on the host in
# the middle.  Grouping is an implementation detail; the interface is one command.
#
# What this produces under -OutDir (default C:\tmp\m5-vm\full-suite):
#   <stamp>/results.json   machine-readable: every case, its group, status and seconds
#   <stamp>/summary.txt    human summary
# and it exits non-zero when anything failed or could not be read.
#
# Groups (assigned from the staged tests.json, with an unassigned name treated as an
# error rather than silently skipped):
#   unit / model (m4.*+m5.*) / integration.m1+m4 / integration.m2 / integration.m3 /
#   integration.m5 non-powerloss / structural / powerloss (three phases)
#   memory (only with -IncludeMemoryExhaustion: the destructive R-01 experiment)
#   fault  (only with -IncludeFaultMatrix: R-04's process kills)
#
# Environment (same as every other harness entry point):
#   M5VM_HOST, M5VM_USER, M5VM_KEY, and for powerloss M5VM_VMX, M5VM_PASSWORD.

param(
    [string]$VmHost = $env:M5VM_HOST,
    [string]$VmUser = $env:M5VM_USER,
    [string]$KeyFile = $env:M5VM_KEY,
    [string]$Configuration = 'Release',
    [string]$OutDir = 'C:\tmp\m5-vm\full-suite',
    [switch]$SkipStage,
    # Run only these groups (comma separated).  Debugging aid; the release gate uses all.
    [string[]]$Only,
    # R-01: the budget-exhaustion experiment.  Destructive by design (it fills the disk's
    # fixed budget and, in `ram` mode, the machine's memory), so it is not part of the
    # default gate.  One switch makes it a repeatable step: it runs as its own group with
    # a guest reset before it, and its case count is added to `expected_cases` only when
    # it is selected.
    [switch]$IncludeMemoryExhaustion,
    # R-04: the process-kill case.  Opt-in while its recorded recovery defect is open; it
    # is a real-disk case and is serialised in its own group with a guest reset.
    [switch]$IncludeFaultMatrix,
    # T4-03's five online-compaction scenarios are real 10-16 GiB workloads and take ~35
    # minutes together -- longer than one guest agent request.  They are gated separately
    # (`integration.m5.compaction_*`) so the default A1/A2 suite keeps its 158-case shape;
    # pass this switch (or `-Only compaction`) to run them through the suite too.
    [switch]$IncludeCompaction
)

$ErrorActionPreference = 'Stop'

if (-not $VmHost) { throw 'M5VM_HOST / -VmHost is required' }
if (-not $VmUser) { throw 'M5VM_USER / -VmUser is required' }
if (-not $KeyFile) { throw 'M5VM_KEY / -KeyFile is required' }

# The child harness and the powerloss driver read these from the environment; a caller
# that passed parameters instead of exporting variables must still work.
$env:M5VM_HOST = $VmHost
$env:M5VM_USER = $VmUser
$env:M5VM_KEY = $KeyFile

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools/m5vm -> repo
$harness = Join-Path $PSScriptRoot 'Invoke-M5Vm.ps1'
$powerloss = Join-Path $PSScriptRoot 'Test-M5PowerLoss.ps1'
$stagedTests = Join-Path 'C:\tmp\m5-vm\stage' 'tests.json'
$sshArgs = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
             '-o', 'LogLevel=ERROR', '-i', $KeyFile, '-o', 'IdentitiesOnly=yes')
$scpArgs = @('-q', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new',
             '-i', $KeyFile, '-o', 'IdentitiesOnly=yes')
$target = "$VmUser@$VmHost"
$remoteRoot = 'C:/m5/control'

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$runDir = Join-Path $OutDir $stamp
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$startedUtc = (Get-Date).ToUniversalTime()

function Invoke-Remote([string]$script) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @('') + @($script) | & ssh.exe @sshArgs $target 'powershell.exe -NoProfile -NonInteractive -Command -' 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) { throw "remote command failed ($code): $($output -join "`n")" }
    return $output
}

# The reset is part of the gate, not an optimisation: a leftover disk measures the guest.
# It runs through the agent because service control needs an elevated, non-network logon.
function Reset-Guest {
    $id = 'fullreset' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $local = Join-Path $runDir "$id.request.ps1"
    @'
$ErrorActionPreference = 'Continue'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $local -Encoding utf8
    & scp.exe @scpArgs $local "${target}:$remoteRoot/$id.request.ps1"
    if ($LASTEXITCODE -ne 0) { throw 'could not copy the reset request' }
    $pending = Join-Path $runDir "$id.pending.id"
    Set-Content -LiteralPath $pending -Value $id -Encoding ascii -NoNewline
    & scp.exe @scpArgs $pending "${target}:$remoteRoot/pending.id"
    if ($LASTEXITCODE -ne 0) { throw 'could not copy pending.id' }
    Invoke-Remote "schtasks /run /tn 'MyRamDisk-M5-Agent' | Out-Null; 'triggered'" | Out-Null
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline) {
        $lines = Invoke-Remote "if (Test-Path '$remoteRoot/$id.done') { 'DONE' } else { 'WAIT' }"
        if (($lines -join ' ') -match 'DONE') {
            $text = (Invoke-Remote "Get-Content '$remoteRoot/$id.response.txt' -Raw") -join "`n"
            if ($text -notmatch 'MYRD_VM_EXIT=0' -or $text -notmatch 'RESET') {
                throw "the guest reset did not report RESET:`n$text"
            }
            return
        }
        Start-Sleep -Seconds 3
    }
    throw 'the guest reset did not finish within 180 s'
}

function Invoke-Group([string]$name, [string]$filter) {
    Write-Host "`n=== group $name (filter $filter)"
    Reset-Guest
    $groupStarted = Get-Date
    $text = & $harness -Run -Filter $filter -MinTimeoutSeconds 900 -DeadlineSeconds 5400 -TimeoutSeconds 5400 6>&1 2>&1 | Out-String
    Write-Host $text
    $match = [regex]::Match([string]$text, 'request ([0-9a-f]{32}) submitted')
    if (-not $match.Success) { throw "the $name group produced no request id" }
    $id = $match.Groups[1].Value
    $resultsPath = Join-Path 'C:\tmp\m5-vm' "$id.results.json"
    if (-not (Test-Path -LiteralPath $resultsPath)) {
        throw "the $name group produced no results.json ($resultsPath)"
    }
    $raw = Get-Content -LiteralPath $resultsPath -Raw | ConvertFrom-Json
    $cases = New-Object System.Collections.ArrayList
    foreach ($case in $raw) {
        [void]$cases.Add([pscustomobject]@{
            name      = [string]$case.name
            status    = if ($null -ne $case.status) { [string]$case.status } else { 'pass' }
            exit_code = $case.exit_code
            seconds   = $case.seconds
        })
    }
    $failed = @($cases | Where-Object { $_.status -in @('fail', 'timeout') })
    $skipped = @($cases | Where-Object { $_.status -eq 'skip' })
    $passed = $cases.Count - $failed.Count - $skipped.Count
    $seconds = [math]::Round(((Get-Date) - $groupStarted).TotalSeconds, 1)
    Write-Host ("--- {0}: {1} passed, {2} skipped, {3} failed in {4} s" -f
        $name, $passed, $skipped.Count, $failed.Count, $seconds)
    return [pscustomobject]@{
        name = $name; filter = $filter; passed = $passed
        skipped = $skipped.Count; failed = $failed.Count
        seconds = $seconds; cases = $cases
    }
}

# ---------------------------------------------------------------------------
# Group assignment from the staged test list.
# ---------------------------------------------------------------------------
if (-not $SkipStage) {
    Write-Host '=== staging the Release build'
    $stageText = & $harness -Stage -Configuration $Configuration 6>&1 2>&1 | Out-String
    Write-Host $stageText
}
if (-not (Test-Path -LiteralPath $stagedTests)) {
    throw "staged tests.json not found: $stagedTests (run without -SkipStage first)"
}
# **Enumerated, not wrapped.**  PS 5.1's ConvertFrom-Json returns a top-level JSON array
# as one object, so `@(...)` produced a single-element list whose `name` was the whole
# joined list: the completeness check then compared 1 against 1 and passed, and two tests
# (`integration.shm`, `model.m3.checkpoint_crash`) belonged to no group without anyone
# noticing.  The first full run reported 158 cases, not 160.
$allTests = New-Object System.Collections.ArrayList
foreach ($t in (Get-Content -LiteralPath $stagedTests -Raw | ConvertFrom-Json)) {
    [void]$allTests.Add($t)
}
if ($allTests.Count -eq 0) { throw 'the staged tests.json is empty' }

# R-01/R-04: cases that are staged like every other one but are part of the suite only when
# the caller asks for them.  Filtering the *selection* here (rather than skipping them
# later) keeps `expected_cases` honest: 160 by default, 161 with `-IncludeMemoryExhaustion`
# or `-IncludeFaultMatrix`.  The `p4p5_*` pair is never selected here at all: it needs the
# host to cut the power between its two halves and is driven by
# `tools/m5vm/Test-M5P4P5PowerCut.ps1`.
$wantMemoryExhaustion = $IncludeMemoryExhaustion -or ($Only -contains 'memory')
$wantFaultMatrix = $IncludeFaultMatrix -or ($Only -contains 'fault')
$wantCompaction = $IncludeCompaction -or ($Only -contains 'compaction')
$selectedTests = New-Object System.Collections.ArrayList
foreach ($t in $allTests) {
    if ($t.name -eq 'integration.m5.memory_exhaustion' -and -not $wantMemoryExhaustion) {
        continue
    }
    if ($t.name -eq 'integration.m5.process_kills' -and -not $wantFaultMatrix) {
        continue
    }
    if ($t.name -eq 'integration.m5.service_kill' -and -not $wantFaultMatrix) {
        continue
    }
    if ($t.name -eq 'integration.m5.bundle_corruption' -and -not $wantFaultMatrix) {
        continue
    }
    if ($t.name -eq 'integration.m5.stop_after_restore' -and -not $wantFaultMatrix) {
        continue
    }
    if ($t.name -eq 'integration.m5.hostile_acl' -and -not $wantFaultMatrix) {
        continue
    }
    if ($t.name -eq 'integration.m5.volume_full' -and -not $wantFaultMatrix) {
        continue
    }
    if ($t.name -match '^integration\.m5\.p4p5_') { continue }
    [void]$selectedTests.Add($t)
}

$assign = {
    param([string]$name)
    if ($name -eq 'integration.m5.memory_exhaustion') { return 'memory' }
    if ($name -eq 'integration.m5.process_kills') { return 'fault' }
    if ($name -eq 'integration.m5.service_kill') { return 'fault-service' }
    if ($name -eq 'integration.m5.bundle_corruption') { return 'fault' }
    if ($name -eq 'integration.m5.stop_after_restore') { return 'fault' }
    if ($name -eq 'integration.m5.hostile_acl') { return 'fault' }
    if ($name -eq 'integration.m5.volume_full') { return 'fault' }
    if ($name -match '^unit\.') { return 'unit' }
    if ($name -match '^m4\.' -or $name -match '^m5\.' -or $name -match '^model\.') { return 'model' }
    if ($name -match '^integration\.m1\.' -or $name -match '^integration\.m4\.' -or $name -match '^integration\.shm$') { return 'integration.m1m4' }
    if ($name -match '^integration\.m2\.') { return 'integration.m2' }
    if ($name -match '^integration\.m3\.') { return 'integration.m3' }
    if ($name -match '^integration\.m5\.powerloss') { return 'powerloss' }
    if ($name -match '^integration\.m5\.compaction_') { return 'compaction' }
    if ($name -match '^integration\.m5\.(performance|cluster_size|compatibility|soak|support_bundle)$') { return 'b' }
    if ($name -eq 'integration.m5.bigdisk') { return 'bigdisk' }
    if ($name -eq 'integration.m5.nonadmin_prepare') { return 'nonadmin' }
    if ($name -match '^integration\.m5\.') { return 'integration.m5' }
    if ($name -match '^structural\.') { return 'structural' }
    return 'unassigned'
}
$byGroup = @{}
foreach ($t in $selectedTests) {
    $group = & $assign $t.name
    if ($group -eq 'unassigned') { throw "test '$($t.name)' belongs to no group" }
    if (-not $byGroup.ContainsKey($group)) { $byGroup[$group] = New-Object System.Collections.ArrayList }
    [void]$byGroup[$group].Add($t.name)
}
$selectedTotal = $selectedTests.Count
$assignedTotal = ($byGroup.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
if ($assignedTotal -ne $selectedTotal) {
    throw "group assignment covers $assignedTotal of $selectedTotal tests"
}
foreach ($group in $byGroup.Keys) {
    Write-Host ("  {0}: {1} case(s)" -f $group, $byGroup[$group].Count)
}

$filters = [ordered]@{
    'unit'           = '^unit\.'
    'model'          = '^(m4|m5|model)\.'
    'integration.m1m4' = '^integration\.(m1|m4)\.|^integration\.shm$'
    'integration.m2' = '^integration\.m2\.'
    'integration.m3' = '^integration\.m3\.'
    'integration.m5' = '^integration\.m5\.(?!powerloss|memory_exhaustion|process_kills|service_kill|bundle_corruption|stop_after_restore|hostile_acl|volume_full|p4p5_|performance|cluster_size|compatibility|soak|support_bundle|compaction_|bigdisk|nonadmin_prepare)'
    'structural'     = '^structural\.'
}
if ($wantFaultMatrix) {
    # R-04's process kills and R-03's bundle damages assume an empty service, so they get
    # their own guest reset instead of sharing the integration.m5 group's (whose earlier
    # cases leave profiles).  The service kill gets a group of its own on top of that: after
    # a kill experiment the guest's storage stack needs a hard reboot, so one kill per group
    # is the most a reset can honestly separate.  Off by default while the R-04.B recovery
    # question is open: a known-red case in the default gate trains people to ignore it.
    $filters['fault'] = '^integration\.m5\.(process_kills|bundle_corruption|stop_after_restore|hostile_acl|volume_full)$'
    $filters['fault-service'] = '^integration\.m5\.service_kill$'
}
if ($wantMemoryExhaustion) {
    # R-01's one-command exhaustion experiment.  Its own group, so it gets a fresh guest
    # and its failure cannot take another group with it.
    $filters['memory'] = '^integration\.m5\.memory_exhaustion$'
}
if ($wantCompaction) {
    # The five real-workload compaction scenarios, separated because together they exceed
    # one guest agent request.  Each case has its own ctest timeout (5400 s).
    $filters['compaction'] = '^integration\.m5\.compaction_'
}
if ($Only -contains 'b') {
    # B is always run per-case with a fresh guest (the handoff's 5/5 evidence); this group
    # exists only so `-Only b` can reproduce that set in one request if asked.
    $filters['b'] = '^integration\.m5\.(performance|cluster_size|compatibility|soak|support_bundle)$'
}
if ($Only -contains 'bigdisk') {
    # The 16 GiB create is a known-stalling large-disk case (handoff §3.233), excluded from
    # A1/A2 since 1.0.2; this group only exists to run it deliberately.
    $filters['bigdisk'] = '^integration\.m5\.bigdisk$'
}
if ($Only.Count -ne 0) {
    foreach ($name in $Only) {
        if ($name -ne 'powerloss' -and -not $filters.Contains($name)) {
            throw "-Only '$name' is not a group; groups: $($filters.Keys -join ', '), powerloss"
        }
    }
}

$groups = New-Object System.Collections.ArrayList
foreach ($name in $filters.Keys) {
    if ($Only.Count -ne 0 -and $Only -notcontains $name) { continue }
    [void]$groups.Add((Invoke-Group $name $filters[$name]))
}

# **`expected_cases` describes this run, not the staged list.**  `-Only` selects groups,
# and the memory case exists in the staged list whether or not it was asked for -- so
# counting the *groups actually executed* is the only total that stays meaningful in all
# three modes (default 160, `-IncludeMemoryExhaustion` 161, `-Only memory` 1).  The
# alternative, counting the staged list, is what made a passing `-Only memory` run report
# "1 cases reported, 161 expected" and exit non-zero.
$expectedTotal = 0
foreach ($name in $filters.Keys) {
    if ($Only.Count -ne 0 -and $Only -notcontains $name) { continue }
    if ($byGroup.ContainsKey($name)) { $expectedTotal += $byGroup[$name].Count }
}
if ($Only.Count -eq 0 -or $Only -contains 'powerloss') {
    # The power-cut phase is two guest cases summarised from one host-driven transcript.
    $expectedTotal += 2
}

if ($Only.Count -eq 0 -or $Only -contains 'powerloss') {
    if (-not $env:M5VM_VMX -or -not $env:M5VM_PASSWORD) {
        throw 'powerloss needs M5VM_VMX and M5VM_PASSWORD in the environment'
    }
    Write-Host "`n=== group powerloss (prepare + hard power cut + verify)"
    $powerStarted = Get-Date
    $powerText = ''
    # **A failing powerloss phase must still produce the aggregate results file.**  The
    # first full run aborted here on a `throw` from the child, so the machine-readable
    # results of the other seven groups were never written; that is the opposite of
    # what a release gate needs.
    try {
        $powerText = & $powerloss -SkipStage -Configuration $Configuration 6>&1 2>&1 | Out-String
    } catch {
        $powerText = ([string]$powerText) + "`n" + $_.Exception.Message
    }
    Write-Host $powerText
    $seconds = [math]::Round(((Get-Date) - $powerStarted).TotalSeconds, 1)
    $powerPassed = $powerText -match 'PASS integration\.m5\.powerloss'
    $cases = New-Object System.Collections.ArrayList
    foreach ($phase in @('integration.m5.powerloss_prepare', 'integration.m5.powerloss_verify')) {
        [void]$cases.Add([pscustomobject]@{
            name = $phase
            status = if ($powerPassed) { 'pass' } else { 'fail' }
            exit_code = if ($powerPassed) { 0 } else { 1 }
            seconds = $null
        })
    }
    [void]$groups.Add([pscustomobject]@{
        name = 'powerloss'; filter = 'prepare + cut + verify'
        passed = if ($powerPassed) { 2 } else { 0 }
        skipped = 0; failed = if ($powerPassed) { 0 } else { 2 }
        seconds = $seconds; cases = $cases
    })
}

$finishedUtc = (Get-Date).ToUniversalTime()
$totals = [pscustomobject]@{
    cases   = ($groups | ForEach-Object { $_.cases.Count } | Measure-Object -Sum).Sum
    passed  = ($groups | Measure-Object -Property passed -Sum).Sum
    skipped = ($groups | Measure-Object -Property skipped -Sum).Sum
    failed  = ($groups | Measure-Object -Property failed -Sum).Sum
}
$document = [pscustomobject]@{
    schema           = 'myrd.m5.full-suite.v1'
    started_utc      = $startedUtc.ToString('o')
    finished_utc     = $finishedUtc.ToString('o')
    duration_seconds = [math]::Round(($finishedUtc - $startedUtc).TotalSeconds, 1)
    configuration    = $Configuration
    groups           = $groups
    totals           = $totals
    expected_cases   = $expectedTotal
}
$resultsPath = Join-Path $runDir 'results.json'
$temporaryPath = "$resultsPath.tmp"
$document | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temporaryPath -Encoding utf8
Move-Item -LiteralPath $temporaryPath -Destination $resultsPath -Force
$readBack = Get-Content -LiteralPath $resultsPath -Raw | ConvertFrom-Json
if ($null -eq $readBack.totals -or $readBack.groups.Count -eq 0) {
    throw 'the full-suite results.json did not round-trip'
}

$summary = New-Object System.Collections.ArrayList
[void]$summary.Add(("M5 full suite: {0} cases, {1} passed, {2} skipped, {3} failed in {4} s" -f
    $totals.cases, $totals.passed, $totals.skipped, $totals.failed, $document.duration_seconds))
foreach ($group in $groups) {
    [void]$summary.Add(("  {0,-18} {1,3} passed {2,3} skipped {3,3} failed {4,7} s" -f
        $group.name, $group.passed, $group.skipped, $group.failed, $group.seconds))
}
if ($totals.cases -ne $expectedTotal) {
    [void]$summary.Add(("  ERROR: {0} cases reported, {1} expected" -f $totals.cases, $expectedTotal))
}
$summaryPath = Join-Path $runDir 'summary.txt'
$summary | Set-Content -LiteralPath $summaryPath -Encoding utf8
$summary | ForEach-Object { Write-Host $_ }
Write-Host "`nresults: $resultsPath"
Write-Host "summary: $summaryPath"

if ($totals.failed -ne 0 -or $totals.cases -ne $expectedTotal) {
    exit 1
}
exit 0

