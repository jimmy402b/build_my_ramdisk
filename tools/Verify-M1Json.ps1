# Verify-M1Json.ps1 — Strict M1 report schema/semantic validator
param(
    [Parameter(Mandatory=$true)]
    [string]$ReportPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ReportPath)) {
    throw "Report not found: $ReportPath"
}

$r = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json

# Schema
if ($r.schema_version -ne 1) { throw "schema_version must be 1" }
if (-not $r.mode) { throw "mode is required" }
if (-not $r.run_id) { throw "run_id is required" }

# Success/exit code consistency
if ($r.success -and $r.exit_code -ne 0) {
    throw "success=true but exit_code=$($r.exit_code)"
}

# For success reports, require counters
if ($r.success) {
    if ($r.provider.info_count -lt 1) { throw "success report missing info_count" }
    if ($r.provider.read_count -lt 1) { throw "success report missing read_count" }
    if ($r.provider.write_count -lt 1) { throw "success report missing write_count" }
    if ($r.provider.zero_count -lt 1) { throw "success report missing zero_count" }
    if ($r.provider.unmap_count -lt 1) { throw "success report missing unmap_count" }
}

# Cleanup contract
if ($r.success) {
    if (-not $r.cleanup.exact_remove_succeeded) {
        throw "success report has exact_remove_succeeded=false"
    }
    if (-not $r.cleanup.provider_stopped) {
        throw "success report has provider_stopped=false"
    }
    if (-not $r.cleanup.named_objects_released) {
        throw "success report has named_objects_released=false"
    }
    if (-not $r.cleanup.aim_device_lists_match) {
        throw "success report has aim_device_lists_match=false"
    }
}

Write-Host "M1 JSON validation PASS: $ReportPath"
