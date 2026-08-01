param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
$report = Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json
function require([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}
function requireProperty($object, [string]$name, [string]$message) {
    require ($null -ne $object.PSObject.Properties[$name]) $message
}

if ($report.schema_version -ne 1) { throw 'schema_version mismatch' }
require ($report.mode -in @('query-driver','run')) 'mode is invalid'
if ($report.run_id -notmatch '^[0-9a-f]{32}$') {
    throw 'run_id must be 32 lowercase hexadecimal characters'
}
if ($report.started_utc -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') {
    throw 'started_utc must be ISO-8601 UTC'
}
if ($report.finished_utc -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') {
    throw 'finished_utc must be ISO-8601 UTC'
}
if ($report.last_stage -ne 'report_written') {
    throw 'last_stage must be report_written'
}
if ($null -eq $report.cleanup) { throw 'cleanup object missing' }
if ($null -eq $report.environment) { throw 'environment object missing' }
if ($null -eq $report.dependency_audit) { throw 'dependency_audit missing' }
if ($null -eq $report.driver) { throw 'driver object missing' }
if ($null -eq $report.baseline) { throw 'baseline object missing' }
if ($null -eq $report.provider) { throw 'provider object missing' }
if ($null -eq $report.cleanup_errors) { throw 'cleanup_errors array missing' }

foreach ($field in @('windows_version','windows_build','architecture',
                      'executable_path','executable_sha256')) {
    require (-not [string]::IsNullOrWhiteSpace(
        [string]$report.environment.$field)) "environment.$field is missing"
}
requireProperty $report.environment 'elevated' 'environment.elevated is missing'
require ($report.environment.executable_sha256 -match '^[0-9A-F]{64}$') `
    'environment executable SHA-256 is invalid'

foreach ($field in @('imported_modules','loaded_modules',
                      'forbidden_loaded_modules','forbidden_processes')) {
    requireProperty $report.dependency_audit $field `
        "dependency_audit.$field is missing"
}
require (@($report.dependency_audit.loaded_modules).Count -gt 0) `
    'loaded module evidence is empty'
require (-not [string]::IsNullOrWhiteSpace(
    [string]$report.dependency_audit.deviosvc_state_during_run)) `
    'deviosvc state evidence is missing'

foreach ($field in @('service_name','service_state','file_path','file_version',
                      'sha256','authenticode_status','signer_subject',
                      'adapter_interface_path','query_version_raw')) {
    requireProperty $report.driver $field "driver.$field is missing"
}
foreach ($field in @('service_name','service_state','file_path','file_version',
                      'sha256','authenticode_status')) {
    require (-not [string]::IsNullOrWhiteSpace(
        [string]$report.driver.$field)) "driver.$field is empty"
}
require ($report.driver.sha256 -match '^[0-9A-F]{64}$') `
    'driver SHA-256 is invalid'

if ($report.mode -eq 'query-driver') {
    require ($report.success -eq $true) 'query-driver report is not successful'
    require ($report.exit_code -eq 0) 'query-driver exit_code is not zero'
    require ($null -eq $report.primary_error) `
        'query-driver report contains primary_error'
    require (@($report.cleanup_errors).Count -eq 0) `
        'query-driver cleanup_errors is not empty'
    require ($null -eq $report.created_device) `
        'query-driver report contains created_device'
    require ($null -eq $report.raw_round_trip) `
        'query-driver report contains raw_round_trip'
    require (-not [string]::IsNullOrWhiteSpace(
        [string]$report.driver.adapter_interface_path)) `
        'query-driver adapter interface path is empty'
    require (-not [string]::IsNullOrWhiteSpace(
        [string]$report.driver.query_version_raw)) `
        'query-driver version evidence is empty'
    require (@($report.baseline.physical_drive_numbers_before).Count -gt 0) `
        'query-driver PhysicalDrive baseline is empty'
    require ((@($report.baseline.aim_device_ids_before | Sort-Object) -join ',') `
        -eq (@($report.baseline.aim_device_ids_after | Sort-Object) -join ',')) `
        'query-driver AIM lists differ'
    require ((@($report.baseline.physical_drive_numbers_before | Sort-Object) -join ',') `
        -eq (@($report.baseline.physical_drive_numbers_after | Sort-Object) -join ',')) `
        'query-driver PhysicalDrive lists differ'
}
Write-Output "JSON schema valid"
