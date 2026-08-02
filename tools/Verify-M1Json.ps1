param(
    [Parameter(Mandatory=$true)][string]$ReportPath,
    [int]$ChildExitCode = [int]::MinValue,
    [ValidateSet('none','provider_ready','create_returned','device_confirmed',
                 'physical_drive_confirmed','write_verified','zero_verified',
                 'unmap_verified','device_removed')]
    [string]$ExpectedFailurePoint = 'none'
)

$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-Property($Object, [string]$Name) {
    Require ($null -ne $Object) "object missing while checking $Name"
    Require ($null -ne $Object.PSObject.Properties[$Name]) `
        "required property missing: $Name"
}

function Same-Set($Left, $Right) {
    $a = @($Left | ForEach-Object { [string]$_ } | Sort-Object)
    $b = @($Right | ForEach-Object { [string]$_ } | Sort-Object)
    return (($a -join "`n") -eq ($b -join "`n"))
}

Require (Test-Path -LiteralPath $ReportPath -PathType Leaf) `
    "Report not found: $ReportPath"
$raw = Get-Content -Raw -LiteralPath $ReportPath -Encoding utf8
Require (-not [string]::IsNullOrWhiteSpace($raw)) 'report is empty'
$report = $raw | ConvertFrom-Json

$expectedTop = @(
    'schema_version','mode','run_id','started_utc','finished_utc','success',
    'exit_code','last_stage','driver','baseline','provider','created_device',
    'raw_protocol','cleanup','primary_error','cleanup_errors') | Sort-Object
$actualTop = @($report.PSObject.Properties.Name | Sort-Object)
Require (($expectedTop -join "`n") -eq ($actualTop -join "`n")) `
    "top-level schema mismatch: $($actualTop -join ',')"

Require ($report.schema_version -eq 1) 'schema_version must be 1'
Require ($report.mode -eq 'run') 'mode must be run'
Require ([string]$report.run_id -match '^[0-9a-f]{32}$') `
    'run_id must be 32 lowercase hexadecimal characters'
Require ([string]$report.started_utc -match `
    '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') `
    'started_utc must be ISO-8601 UTC'
Require ([string]$report.finished_utc -match `
    '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') `
    'finished_utc must be ISO-8601 UTC'
Require (-not [string]::IsNullOrWhiteSpace([string]$report.last_stage)) `
    'last_stage is required'

if ($ChildExitCode -ne [int]::MinValue) {
    Require ($report.exit_code -eq $ChildExitCode) `
        "child exit code mismatch: report=$($report.exit_code) child=$ChildExitCode"
}
Require (($report.success -and $report.exit_code -eq 0) -or
         (-not $report.success -and $report.exit_code -ne 0)) `
    'success and exit_code are inconsistent'

foreach ($name in @('service_name','service_state','file_path','file_version',
                     'sha256','authenticode_status','signer_subject',
                     'adapter_interface_path','api_version','sub_version',
                     'query_version_raw','imported_modules','loaded_modules',
                     'forbidden_loaded_modules','forbidden_processes')) {
    Require-Property $report.driver $name
}
Require ($report.driver.service_name -eq 'phdskmnt') `
    'driver service_name must be phdskmnt'
Require ($report.driver.service_state -eq 'Running') `
    'phdskmnt was not Running during the child lifecycle'
Require (-not [string]::IsNullOrWhiteSpace(
    [string]$report.driver.file_path)) 'driver file_path is empty'
Require ([string]$report.driver.file_version -match
    '^\d+\.\d+\.\d+\.\d+$') 'driver file_version is invalid'
Require ($report.driver.sha256 -match '^[0-9A-F]{64}$') `
    'driver SHA-256 is invalid'
Require ([string]$report.driver.authenticode_status -match '^0x[0-9A-F]+$') `
    'driver Authenticode status is invalid'
Require (-not [string]::IsNullOrWhiteSpace(
    [string]$report.driver.adapter_interface_path)) `
    'adapter interface path is empty'
Require (-not [string]::IsNullOrWhiteSpace(
    [string]$report.driver.query_version_raw)) `
    'driver query version evidence is empty'
Require (@($report.driver.loaded_modules).Count -gt 0) `
    'loaded module evidence is empty'
Require (@($report.driver.forbidden_loaded_modules).Count -eq 0) `
    'forbidden AIM user-mode module was loaded'
Require (@($report.driver.forbidden_processes).Count -eq 0) `
    'forbidden AIM/devio process was running'

foreach ($name in @('info_count','read_count','write_count','unmap_count',
                     'zero_count','close_count','info_error_count',
                     'read_error_count','write_error_count',
                     'unmap_error_count','zero_error_count',
                     'fatal_protocol_error_count','cancelled','timed_out',
                     'native_object_path','final_state','last_error',
                     'diagnostics')) {
    Require-Property $report.provider $name
}
Require ([string]$report.provider.native_object_path -match
    '^\\BaseNamedObjects\\Global\\myrd-m1-[0-9a-f]{32}$') `
    'provider native object path is invalid'

foreach ($name in @('exact_remove_attempted','exact_remove_succeeded',
                     'removed_device_id','device_disappeared',
                     'provider_stopped','named_objects_released',
                     'aim_device_lists_match','physical_drive_lists_match',
                     'deviosvc_restored','deviosvc_start_mode_unchanged')) {
    Require-Property $report.cleanup $name
}
Require $report.cleanup.provider_stopped 'provider was not stopped'
Require $report.cleanup.named_objects_released 'named objects were not released'
Require $report.cleanup.aim_device_lists_match 'AIM device baseline changed'
Require $report.cleanup.physical_drive_lists_match `
    'PhysicalDrive baseline changed'
Require (Same-Set $report.baseline.aim_device_ids_before `
                  $report.baseline.aim_device_ids_after) `
    'AIM baseline arrays differ'
Require (Same-Set $report.baseline.physical_drive_numbers_before `
                  $report.baseline.physical_drive_numbers_after) `
    'PhysicalDrive baseline arrays differ'

if ($ExpectedFailurePoint -eq 'none') {
    Require $report.success 'normal M1 lifecycle failed'
    Require ($report.provider.final_state -eq 'closed') `
        'successful provider final_state must be closed'
    Require ($null -eq $report.primary_error) `
        'successful report contains primary_error'
    Require (@($report.cleanup_errors).Count -eq 0) `
        'successful report contains cleanup_errors'
    Require ($null -ne $report.created_device) 'created_device is missing'
    Require ($report.created_device.device_id_raw -match '^\d+$') `
        'created DeviceId is invalid'
    Require ($report.created_device.device_id_raw -ne '16777215') `
        'created DeviceId is the all-device sentinel'
    Require $report.created_device.confirmed_by_query_device `
        'created device was not confirmed by QUERY_DEVICE'
    Require ($null -ne $report.raw_protocol) 'raw_protocol evidence is missing'
    Require ($report.raw_protocol.write_sha256 -match '^[0-9A-F]{64}$') `
        'write SHA-256 is invalid'
    Require ($report.raw_protocol.write_sha256 -eq
             $report.raw_protocol.read_sha256) 'raw readback hash differs'
    Require $report.raw_protocol.flush_succeeded 'raw flush was not successful'
    Require $report.raw_protocol.zero_write_verified `
        'real ZERO request was not verified'
    Require $report.raw_protocol.unmap_verified `
        'real UNMAP request was not verified'
    Require ($report.provider.info_count -ge 1 -and
             $report.provider.read_count -ge 1 -and
             $report.provider.write_count -ge 1 -and
             $report.provider.zero_count -ge 1 -and
             $report.provider.unmap_count -ge 1) `
        'all real proxy operations were not observed'
    Require $report.cleanup.exact_remove_attempted `
        'exact remove was not attempted'
    Require $report.cleanup.exact_remove_succeeded `
        'exact remove did not succeed'
    Require $report.cleanup.device_disappeared `
        'created device did not disappear'
} else {
    Require (-not $report.success) 'injected failure unexpectedly succeeded'
    Require ($null -ne $report.primary_error) `
        'injected failure report has no primary_error'
    Require ($report.primary_error.operation -eq 'injected_failure') `
        'failure report does not contain the injected primary error'
    if ($ExpectedFailurePoint -ne 'provider_ready') {
        Require $report.cleanup.exact_remove_attempted `
            'post-CREATE failure did not attempt exact removal'
        Require $report.cleanup.exact_remove_succeeded `
            'post-CREATE exact removal failed'
        Require $report.cleanup.device_disappeared `
            'post-CREATE device did not disappear'
    }
    Require (@($report.cleanup_errors).Count -eq 0) `
        'failure injection cleanup contains errors'
}

Write-Output "M1 JSON validation PASS: $ReportPath"
