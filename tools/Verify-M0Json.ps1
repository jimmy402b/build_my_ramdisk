param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
$report = Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json
if ($report.schema_version -ne 1) { throw 'schema_version mismatch' }
if ($null -eq $report.cleanup) { throw 'cleanup object missing' }
if ($null -eq $report.environment) { throw 'environment object missing' }
if ($null -eq $report.dependency_audit) { throw 'dependency_audit missing' }
if ($null -eq $report.driver) { throw 'driver object missing' }
if ($null -eq $report.baseline) { throw 'baseline object missing' }
if ($null -eq $report.provider) { throw 'provider object missing' }
if ($null -eq $report.cleanup_errors) { throw 'cleanup_errors array missing' }
Write-Output "JSON schema valid"
