[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [Parameter(Mandatory = $true)][string]$ExpectedServerSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedImageSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-M3NtfsEvidence {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Require-M3NtfsHash {
    param($Value, [string]$Name)
    Require-M3NtfsEvidence ([string]$Value -match '^[0-9A-Fa-f]{64}$') "$Name is not SHA-256"
}

function Require-M3NtfsProperty {
    param($Object, [string]$Name)
    Require-M3NtfsEvidence ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) `
        "required evidence property is missing: $Name"
}

function Require-M3NtfsCounter {
    param($Object, [string]$Name)
    Require-M3NtfsProperty $Object $Name
    $value = [uint64]$Object.$Name
    return $value
}

$fullPath = [IO.Path]::GetFullPath($Path)
$status = 'FAIL'
try {
    Require-M3NtfsEvidence ([IO.File]::Exists($fullPath)) 'NTFS evidence report is missing'
    $raw = [IO.File]::ReadAllText($fullPath, [Text.UTF8Encoding]::new($false, $true))
    $report = $raw | ConvertFrom-Json
    foreach ($name in @('schema', 'phase', 'status', 'terminal_verdict', 'what_if',
                        'server_started', 'destructive_operations_emitted',
                        'source_commit', 'executable_sha256', 'cleanup', 'workload')) {
        Require-M3NtfsProperty $report $name
    }
    Require-M3NtfsEvidence ([string]$report.schema -eq 'myrd.m3.ntfs-workload.v1') 'NTFS evidence schema is invalid'
    Require-M3NtfsEvidence ([string]$report.phase -eq 'All') 'evidence is not a complete All run'
    Require-M3NtfsEvidence ([string]$report.status -eq 'PASS' -and [string]$report.terminal_verdict -eq 'PASS') `
        'evidence terminal verdict is not PASS'
    Require-M3NtfsEvidence (-not [bool]$report.what_if) 'WhatIf evidence cannot be functional acceptance'
    Require-M3NtfsEvidence ([bool]$report.server_started) 'AIM server was never started'
    Require-M3NtfsEvidence ([bool]$report.destructive_operations_emitted) 'functional workload emitted no operations'
    Require-M3NtfsEvidence ([string]$report.source_commit -match '^[0-9A-Fa-f]{40}$') 'source commit is invalid'
    Require-M3NtfsEvidence ([string]$report.source_commit -ieq $ExpectedSourceCommit) 'source commit mismatch'
    Require-M3NtfsHash $ExpectedServerSha256 'expected server'
    Require-M3NtfsHash $ExpectedImageSha256 'expected image'
    Require-M3NtfsHash $report.executable_sha256.server 'server executable'
    Require-M3NtfsHash $report.executable_sha256.image 'image executable'
    Require-M3NtfsEvidence ([string]$report.executable_sha256.server -ieq $ExpectedServerSha256) 'server hash mismatch'
    Require-M3NtfsEvidence ([string]$report.executable_sha256.image -ieq $ExpectedImageSha256) 'image hash mismatch'

    Require-M3NtfsProperty $report.cleanup 'succeeded'
    Require-M3NtfsEvidence ([bool]$report.cleanup.succeeded) 'cleanup did not succeed'
    Require-M3NtfsEvidence (@($report.cleanup.residuals).Count -eq 0) 'cleanup left residuals'

    $workload = $report.workload
    foreach ($name in @('first_checkpoint_generation', 'second_checkpoint_generation',
                        'compact_generation', 'server_reopen_verified', 'workload',
                        'lazy_evidence', 'native_vhdx')) {
        Require-M3NtfsProperty $workload $name
    }
    Require-M3NtfsEvidence ([uint64]$workload.second_checkpoint_generation -gt
        [uint64]$workload.first_checkpoint_generation) 'reopen did not publish a successor generation'
    Require-M3NtfsEvidence ([bool]$workload.server_reopen_verified) 'server reopen was not verified'
    Require-M3NtfsEvidence ([string]$workload.native_vhdx.filesystem -eq 'NTFS') 'native VHDX is not NTFS'
    Require-M3NtfsProperty $workload.native_vhdx 'disk_number'
    Require-M3NtfsEvidence ([int]$workload.native_vhdx.disk_number -gt 0) 'native VHDX disk number is unsafe'

    $lazy = $workload.lazy_evidence
    foreach ($name in @('bounded_sample', 'full_corpus_verified', 'startup_status', 'sample_status')) {
        Require-M3NtfsProperty $lazy $name
    }
    Require-M3NtfsEvidence ([bool]$lazy.bounded_sample -and [bool]$lazy.full_corpus_verified) `
        'lazy-load sample/full verification is incomplete'
    $startupReads = Require-M3NtfsCounter $lazy.startup_status.storage 'backing_reads_total'
    $sampleReads = Require-M3NtfsCounter $lazy.sample_status.storage 'backing_reads_total'
    $startupUncached = Require-M3NtfsCounter $lazy.startup_status.storage 'uncached_reads_total'
    $sampleUncached = Require-M3NtfsCounter $lazy.sample_status.storage 'uncached_reads_total'
    Require-M3NtfsEvidence ($sampleReads -gt $startupReads -or $sampleUncached -gt $startupUncached) `
        'bounded sample did not prove a backing read'

    $files = @($workload.workload.files)
    Require-M3NtfsEvidence ($files.Count -gt 0) 'durable file corpus is empty'
    foreach ($file in $files) {
        foreach ($name in @('path', 'bytes', 'sha256')) { Require-M3NtfsProperty $file $name }
        Require-M3NtfsHash $file.sha256 'durable file'
        Require-M3NtfsEvidence ([uint64]$file.bytes -gt 0) 'durable file length is zero'
    }
    Require-M3NtfsProperty $workload.workload 'sparse'
    Require-M3NtfsHash $workload.workload.sparse.sha256 'sparse file'

    Write-Output "M3 NTFS JSON validation PASS: $fullPath"
    $status = 'PASS'
    exit 0
}
catch {
    $message = [string]$_.Exception.Message
    [Console]::Error.WriteLine("M3 NTFS JSON validation ${status}: $message")
    exit 1
}
