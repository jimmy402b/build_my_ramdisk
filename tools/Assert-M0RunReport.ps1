param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][int]$ChildExitCode,
    [ValidateSet('none','create_returned','device_confirmed',
                 'physical_drive_confirmed','round_trip_verified')]
    [string]$FailurePoint = 'none',
    [switch]$RequireServiceRestoration
)

$ErrorActionPreference = 'Stop'

& "$PSScriptRoot\Verify-M0Json.ps1" -Path $Path | Out-Null
$report = Get-Content -LiteralPath $Path -Raw -Encoding utf8 |
    ConvertFrom-Json

function require([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

require ($report.mode -eq 'run') 'mode must be run'
require ($report.exit_code -eq $ChildExitCode) `
    "report exit_code does not match child exit code"
require ($report.environment.elevated -eq $true) 'run was not elevated'
require ($report.environment.architecture -eq 'x64') 'run was not x64'
require ($report.environment.executable_sha256 -match '^[0-9A-F]{64}$') `
    'missing executable SHA-256'
require ($report.driver.service_name -eq 'phdskmnt') `
    'driver service name mismatch'
require ($report.driver.service_state -eq 'Running') `
    'phdskmnt was not Running'
require ($report.driver.file_path -match '(?i)phdskmnt\.sys$') `
    'driver file path missing'
require (-not [string]::IsNullOrWhiteSpace(
        [string]$report.driver.file_version)) 'driver file version missing'
require ($report.driver.sha256 -match '^[0-9A-F]{64}$') `
    'driver SHA-256 missing'
require ($report.provider.session_id -match '^[0-9a-f]{32}$') `
    'provider session id invalid'
require ($report.provider.native_object_path.EndsWith(
        $report.provider.session_id)) 'provider path/session mismatch'
require ($report.provider.info_count -ge 1) 'INFO count must be at least one'
require ($report.provider.fatal_protocol_error_count -eq 0) `
    'fatal protocol error count must be zero'
require (@($report.dependency_audit.forbidden_loaded_modules).Count -eq 0) `
    'forbidden loaded module detected'
require (@($report.dependency_audit.forbidden_processes).Count -eq 0) `
    'forbidden process detected'
require ($report.dependency_audit.deviosvc_state_during_run -eq 'Stopped') `
    'deviosvc was not Stopped during run'
require (@($report.baseline.physical_drive_numbers_before).Count -gt 0) `
    'PhysicalDrive baseline is empty'

require ($null -ne $report.created_device) 'created_device is missing'
if ($report.created_device.confirmed_by_query_device -eq $true) {
    require ($report.created_device.create_flags -eq 0x00000310) `
        'QUERY_DEVICE flags do not equal normalized M0 HD/proxy flags'
}
require ($report.cleanup.exact_remove_attempted -eq $true) `
    'exact remove was not attempted'
require ($report.cleanup.exact_remove_succeeded -eq $true) `
    'exact remove did not succeed'
require ($report.cleanup.removed_device_id -eq
         $report.created_device.device_id_raw) `
    'removed DeviceId differs from created DeviceId'
require ($report.cleanup.device_disappeared -eq $true) `
    'created device did not disappear'
require ($report.cleanup.provider_stopped -eq $true) `
    'provider did not stop'
require ($report.cleanup.named_objects_released -eq $true) `
    'named objects were not released'
require ($report.cleanup.aim_device_lists_match -eq $true) `
    'AIM device list did not return to baseline'
require ($report.cleanup.physical_drive_lists_match -eq $true) `
    'PhysicalDrive list did not return to baseline'
require (@($report.cleanup_errors).Count -eq 0) 'cleanup_errors is not empty'

if ($RequireServiceRestoration) {
    require ($report.cleanup.deviosvc_restored -eq $true) `
        'deviosvc state was not restored'
    require ($report.cleanup.deviosvc_start_mode_unchanged -eq $true) `
        'deviosvc StartMode changed'
}

if ($FailurePoint -eq 'none') {
    require ($ChildExitCode -eq 0) 'normal child exit code must be zero'
    require ($report.success -eq $true) 'normal report success must be true'
    require ($null -eq $report.primary_error) `
        'normal report contains a primary error'
    require ($report.created_device.confirmed_by_query_device -eq $true) `
        'QUERY_DEVICE confirmation missing'
    require ($report.created_device.disk_length -eq 67108864) `
        'disk length mismatch'
    require ($report.provider.read_count -ge 1) 'READ count must be at least one'
    require ($report.provider.write_count -ge 1) 'WRITE count must be at least one'
    require ($null -ne $report.raw_round_trip) 'raw round trip is missing'
    require ($report.raw_round_trip.offset -eq 8388608) `
        'raw round-trip offset mismatch'
    require ($report.raw_round_trip.length -eq 4096) `
        'raw round-trip length mismatch'
    require ($report.raw_round_trip.flush_succeeded -eq $true) `
        'FlushFileBuffers did not succeed'
    require ($report.raw_round_trip.written_sha256 -eq
             $report.raw_round_trip.read_sha256) 'SHA-256 mismatch'
} else {
    require ($ChildExitCode -ne 0) 'injected child exit code must be nonzero'
    require ($report.success -eq $false) `
        'injected report success must be false'
    require ($null -ne $report.primary_error) `
        'injected report primary_error is missing'
    require ($report.primary_error.operation -eq 'injected_failure') `
        'failure was not produced by the requested injection point'

    switch ($FailurePoint) {
        'create_returned' {
            require ($report.created_device.confirmed_by_query_device -eq $false) `
                'create_returned injection advanced past QUERY_DEVICE'
            require ($null -eq $report.raw_round_trip) `
                'create_returned injection performed raw I/O'
            require ($report.provider.write_count -eq 0) `
                'create_returned injection issued WRITE'
        }
        'device_confirmed' {
            require ($report.created_device.confirmed_by_query_device -eq $true) `
                'device_confirmed injection lacks QUERY_DEVICE evidence'
            require ($report.created_device.disk_length -eq 67108864) `
                'disk length mismatch'
            require ($report.created_device.physical_drive_number -lt 0) `
                'device_confirmed injection advanced to PhysicalDrive'
            require ($null -eq $report.raw_round_trip) `
                'device_confirmed injection performed raw I/O'
            require ($report.provider.write_count -eq 0) `
                'device_confirmed injection issued WRITE'
        }
        'physical_drive_confirmed' {
            require ($report.created_device.physical_drive_number -ge 0) `
                'physical_drive_confirmed evidence missing'
            require ($report.created_device.disk_length -eq 67108864) `
                'disk length mismatch'
            require ($null -eq $report.raw_round_trip) `
                'physical_drive_confirmed injection performed raw I/O'
            require ($report.provider.write_count -eq 0) `
                'physical_drive_confirmed injection issued WRITE'
        }
        'round_trip_verified' {
            require ($null -ne $report.raw_round_trip) `
                'round_trip_verified evidence missing'
            require ($report.created_device.disk_length -eq 67108864) `
                'disk length mismatch'
            require ($report.provider.write_count -ge 1) `
                'round_trip_verified injection lacks WRITE'
            require ($report.raw_round_trip.offset -eq 8388608) `
                'round_trip_verified offset mismatch'
            require ($report.raw_round_trip.length -eq 4096) `
                'round_trip_verified length mismatch'
            require ($report.raw_round_trip.flush_succeeded -eq $true) `
                'round_trip_verified flush did not succeed'
            require ($report.raw_round_trip.written_sha256 -eq
                     $report.raw_round_trip.read_sha256) 'SHA-256 mismatch'
        }
    }
}

Write-Output "Run report valid: $Path"
