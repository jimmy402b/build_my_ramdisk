[CmdletBinding()]
param(
    [Parameter()]
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
    $RepositoryRoot = Split-Path -Parent $scriptDirectory
}

$microPath = Join-Path $RepositoryRoot 'M3_MICROTASKS.md'
$tasklistPath = Join-Path $RepositoryRoot 'M3_TASKLIST.md'
$planPath = Join-Path $RepositoryRoot 'docs\superpowers\plans\2026-08-04-m3-persistence-engine.md'

foreach ($path in @($microPath, $tasklistPath, $planPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required M3 planning file is missing: $path"
    }
}

$micro = Get-Content -LiteralPath $microPath -Raw -Encoding UTF8
$tasklist = Get-Content -LiteralPath $tasklistPath -Raw -Encoding UTF8
$plan = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8

$expectedTasks = @(
    7..9 | ForEach-Object { '{0:D2}' -f $_ }
) + @(
    13..44 | ForEach-Object { '{0:D2}' -f $_ }
)

$taskMatches = [regex]::Matches(
    $micro,
    '(?m)^### Task (?<id>[0-9]{2})\b')
$actualTasks = @($taskMatches | ForEach-Object { $_.Groups['id'].Value })
if (($actualTasks -join ',') -ne ($expectedTasks -join ',')) {
    throw "Unexpected remaining-task headings. Expected $($expectedTasks -join ','); actual $($actualTasks -join ',')"
}

$cardMatches = [regex]::Matches(
    $micro,
    '`(?<id>T(?<task>[0-9]{2})\.(?<card>[0-9]+)) \[(?<level>M|H|X)\]`')
if ($cardMatches.Count -eq 0) {
    throw 'No M3 microcards were discovered.'
}

$cardIds = @($cardMatches | ForEach-Object { $_.Groups['id'].Value })
$duplicates = @($cardIds | Group-Object | Where-Object Count -gt 1)
if ($duplicates.Count -ne 0) {
    throw "Duplicate microcard IDs: $($duplicates.Name -join ',')"
}

foreach ($task in $expectedTasks) {
    $count = @($cardMatches | Where-Object {
        $_.Groups['task'].Value -eq $task
    }).Count
    if ($count -lt 5) {
        throw "Task $task has only $count microcards; minimum is 5."
    }
}

foreach ($requiredText in @(
    'PAUSED_FOR_HIGH_REASONING',
    'ctest --test-dir out/build/msvc -C Release -N',
    'fault-phase-registry.json',
    'implementation_status:',
    'gate_result:'
)) {
    if (-not $micro.Contains($requiredText)) {
        throw "M3_MICROTASKS.md is missing required control text: $requiredText"
    }
}

if (-not $tasklist.Contains('M3_MICROTASKS.md')) {
    throw 'M3_TASKLIST.md does not require M3_MICROTASKS.md.'
}
if (-not $plan.Contains('M3_MICROTASKS.md')) {
    throw 'The M3 PLAN execution handoff does not require M3_MICROTASKS.md.'
}

[pscustomobject]@{
    schema = 'myrd.m3.microtask-audit.v1'
    result = 'PASS'
    task_count = $actualTasks.Count
    card_count = $cardMatches.Count
    medium_cards = @($cardMatches | Where-Object {
        $_.Groups['level'].Value -eq 'M'
    }).Count
    high_cards = @($cardMatches | Where-Object {
        $_.Groups['level'].Value -eq 'H'
    }).Count
    xhigh_cards = @($cardMatches | Where-Object {
        $_.Groups['level'].Value -eq 'X'
    }).Count
} | ConvertTo-Json -Depth 3
