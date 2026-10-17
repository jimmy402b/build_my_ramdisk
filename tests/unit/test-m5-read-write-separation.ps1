# Assert the read/write separation the operation store promises.
#
# T1-04 requires that mutations take the operation gate exclusively and that queries take it shared or take
# nothing.  That was audited by hand once, which is worth nothing the next time somebody adds a query: a new
# read path that copies the nearest `std::scoped_lock` would silently reintroduce the queueing this change
# removed, and no behavioural test would notice, because a query that waits still returns the right answer.
#
# So the discipline is checked as a property of the source.  Two things are asserted:
#
#   1. The gate is a reader-writer one, not a plain mutex.
#   2. Every function that is a pure query takes it shared, and every function that mutates takes it exclusively.
#
# The second is the one that earns its keep.  A new query that takes the exclusive lock fails here by name,
# which is a better failure than a latency measurement on a machine that happened to be idle.

param(
    [Parameter(Mandatory = 1)]
    [string]$SourceDirectory
)

$ErrorActionPreference = 'Stop'

$source = Join-Path $SourceDirectory 'src/management/operation_store.cpp'
if (-not (Test-Path -LiteralPath $source)) { throw "operation store source not found: $source" }
$lines = Get-Content -LiteralPath $source

# Queries: they read records and change nothing.
$queries = @('load_record', 'list_records', 'find_request')
# Mutations: every one of these writes, or reads then writes, so exclusive is required rather than optional.
$mutations = @('write_record', 'admit_with_id', 'reconcile_after_restart', 'request_cancel',
               'cleanup_terminal_records')

# Walk the file once, remembering the enclosing OperationStore member for each lock statement.  A line that
# declares a member resets the current name; a lock line records what that member took.
$lockByFunction = @{}
$current = $null
foreach ($line in $lines) {
    if ($line -match 'OperationStore::([A-Za-z_][A-Za-z0-9_]*)\s*\(') {
        $current = $Matches[1]
        continue
    }
    if ($null -eq $current) { continue }
    if ($line -match 'std::shared_lock\s+lock\s*\(') {
        if (-not $lockByFunction.ContainsKey($current)) { $lockByFunction[$current] = 'shared' }
    }
    elseif ($line -match 'std::scoped_lock\s+lock\s*\(') {
        if (-not $lockByFunction.ContainsKey($current)) { $lockByFunction[$current] = 'exclusive' }
    }
    elseif ($line -match 'std::unique_lock\s+lock\s*\(') {
        if (-not $lockByFunction.ContainsKey($current)) { $lockByFunction[$current] = 'exclusive' }
    }
}

$failures = @()

# 1. The gate itself must be a reader-writer primitive.
$gateSource = ($lines | Where-Object { $_ -match 'shared_operation_gate' }) -join "`n"
if ($gateSource -notmatch 'shared_ptr<std::shared_mutex>') {
    $failures += 'the operation gate must be a std::shared_mutex so that queries can take it shared'
}

# 2. Queries shared, mutations exclusive.
foreach ($name in $queries) {
    if (-not $lockByFunction.ContainsKey($name)) {
        $failures += "${name}: no lock statement was found, so the separation cannot be confirmed"
    }
    elseif ($lockByFunction[$name] -ne 'shared') {
        $failures += "${name} is a query but takes the gate $($lockByFunction[$name]); it must be shared"
    }
}
foreach ($name in $mutations) {
    if (-not $lockByFunction.ContainsKey($name)) {
        $failures += "${name}: no lock statement was found, so the separation cannot be confirmed"
    }
    elseif ($lockByFunction[$name] -ne 'exclusive') {
        $failures += "${name} mutates but takes the gate $($lockByFunction[$name]); it must be exclusive"
    }
}

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    exit 1
}

Write-Output "read/write separation holds: $($queries.Count) queries shared, $($mutations.Count) mutations exclusive"
exit 0
