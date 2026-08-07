[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Executable)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-SeedContract([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-Seed {
    param([string]$Profile, [string]$Seed)
    $oldProfile = $env:MYRD_M3_SOAK_PROFILE
    $oldSeed = $env:MYRD_M3_SOAK_SEED
    $oldErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $env:MYRD_M3_SOAK_PROFILE = $Profile
        $env:MYRD_M3_SOAK_SEED = $Seed
        $lines = @(& $Executable 2>&1)
        $exitCode = $LASTEXITCODE
        $nonEmpty = @($lines | ForEach-Object { [string]$_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $summary = $null
        if ($nonEmpty.Count -gt 0) {
            try { $summary = $nonEmpty[-1] | ConvertFrom-Json }
            catch { $summary = $null }
        }
        return [pscustomobject]@{
            ExitCode = $exitCode
            Lines = $nonEmpty
            Summary = $summary
        }
    }
    finally {
        $ErrorActionPreference = $oldErrorAction
        $env:MYRD_M3_SOAK_PROFILE = $oldProfile
        $env:MYRD_M3_SOAK_SEED = $oldSeed
    }
}

$quick = Invoke-Seed 'Quick' '42'
Require-SeedContract ($quick.ExitCode -eq 0) 'Quick seed process failed'
Require-SeedContract ($null -ne $quick.Summary) 'Quick final line is not JSON'
Require-SeedContract ($quick.Summary.schema -eq 'myrd.m3.soak-seed.v1') `
    'seed schema mismatch'
Require-SeedContract ([uint64]$quick.Summary.seed -eq 42) `
    'requested seed was ignored'
Require-SeedContract ($quick.Summary.profile -eq 'Quick') `
    'requested profile was ignored'
foreach ($name in @(
    'operation_count','expected_sha256','actual_sha256',
    'complete_generations_only','mixed_generation','resource','checkpoint',
    'latency','cleanup_succeeded')) {
    Require-SeedContract ($null -ne $quick.Summary.PSObject.Properties[$name]) `
        "seed summary is missing $name"
}
Require-SeedContract ($quick.Summary.expected_sha256 -eq `
    $quick.Summary.actual_sha256) 'Quick model hash mismatch'
Require-SeedContract ([bool]$quick.Summary.complete_generations_only) `
    'Quick complete-generation proof is false'
Require-SeedContract (-not [bool]$quick.Summary.mixed_generation) `
    'Quick mixed-generation proof is true'
Require-SeedContract ([uint64]$quick.Summary.resource.startup_read_bytes -le 67108864) `
    'Quick startup read threshold exceeded'
Require-SeedContract ([uint64]$quick.Summary.resource.extra_committed_bytes -le 134217728) `
    'Quick committed-memory threshold exceeded'
Require-SeedContract ([uint64]$quick.Summary.latency.mutation_pause_max_ms -le 250) `
    'Quick mutation-pause threshold exceeded'
Require-SeedContract ([uint64]$quick.Summary.latency.writer_stall_max_ms -le 250) `
    'Quick writer-stall threshold exceeded'
Require-SeedContract ([bool]$quick.Summary.cleanup_succeeded) `
    'Quick cleanup failed'

$quickAgain = Invoke-Seed 'Quick' '42'
Require-SeedContract ($quickAgain.ExitCode -eq 0) 'second Quick seed failed'
Require-SeedContract ($quickAgain.Summary.operation_count -eq `
    $quick.Summary.operation_count) 'same seed changed operation count'
Require-SeedContract ($quickAgain.Summary.expected_sha256 -eq `
    $quick.Summary.expected_sha256) 'same seed changed model hash'

$formal = Invoke-Seed 'Formal' '43'
Require-SeedContract ($formal.ExitCode -eq 0) 'Formal seed process failed'
Require-SeedContract ($formal.Summary.profile -eq 'Formal') `
    'Formal profile was ignored'
Require-SeedContract ([uint64]$formal.Summary.operation_count -gt `
    [uint64]$quick.Summary.operation_count) `
    'Formal workload is not larger than Quick'

$invalid = Invoke-Seed 'Invalid' '42'
Require-SeedContract ($invalid.ExitCode -ne 0) 'invalid profile was accepted'
Require-SeedContract ($null -eq $invalid.Summary -or
    $invalid.Summary.schema -ne 'myrd.m3.soak-seed.v1') `
    'invalid profile emitted passing seed JSON'

Write-Output 'M3 soak seed executable contract PASS'
exit 0
