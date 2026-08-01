param(
    [Parameter(Mandatory)][string]$SummaryPath,
    [ValidateRange(1,20)][int]$ExpectedIterations = 20,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$summary = Get-Content -LiteralPath $SummaryPath -Raw -Encoding utf8 |
    ConvertFrom-Json

function require([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

require ($summary.configuration -eq 'Release') `
    'V6 summary configuration must be Release'
require ($summary.failure_point -eq 'none') `
    'V6 summary must be a normal run'
require ($summary.attempted -eq $ExpectedIterations) `
    'V6 attempted count mismatch'
require ($summary.passed -eq $ExpectedIterations) `
    'V6 passed count mismatch'
require (@($summary.failed_report_paths).Count -eq 0) `
    'V6 summary contains failed reports'
require ($summary.service_state_restored -eq $true) `
    'deviosvc state was not restored'
require ($summary.service_start_mode_unchanged -eq $true) `
    'deviosvc StartMode changed'

$reportPaths = @($summary.report_paths)
require ($reportPaths.Count -eq $ExpectedIterations) `
    'V6 report path count mismatch'

$reports = @()
foreach ($path in $reportPaths) {
    require (Test-Path -LiteralPath $path) "missing report: $path"
    $exitProperty = $summary.child_exit_codes.PSObject.Properties[$path]
    require ($null -ne $exitProperty) "missing child exit code: $path"
    $childExitCode = [int]$exitProperty.Value
    $hashProperty = $summary.report_sha256.PSObject.Properties[$path]
    require ($null -ne $hashProperty) "missing immutable report hash: $path"
    require ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq
             [string]$hashProperty.Value) "published report changed: $path"
    & "$PSScriptRoot\Assert-M0RunReport.ps1" `
        -Path $path -ChildExitCode $childExitCode `
        -FailurePoint none | Out-Null
    $reports += Get-Content -LiteralPath $path -Raw -Encoding utf8 |
        ConvertFrom-Json
}

$runIds = @($reports | ForEach-Object { $_.run_id })
$sessionIds = @($reports | ForEach-Object { $_.provider.session_id })
$objectPaths = @($reports | ForEach-Object {
    $_.provider.native_object_path
})
$executableHashes = @($reports | ForEach-Object {
    $_.environment.executable_sha256
})
$driverHashes = @($reports | ForEach-Object { $_.driver.sha256 })

require (@($runIds | Select-Object -Unique).Count -eq $ExpectedIterations) `
    'run ids are not unique'
require (@($sessionIds | Select-Object -Unique).Count -eq
         $ExpectedIterations) 'session ids are not unique'
require (@($objectPaths | Select-Object -Unique).Count -eq
         $ExpectedIterations) 'provider object paths are not unique'
require (@($executableHashes | Select-Object -Unique).Count -eq 1) `
    'Release executable hash changed during V6'
require (@($driverHashes | Select-Object -Unique).Count -eq 1) `
    'driver hash changed during V6'

$queryBefore = Get-Content -LiteralPath $summary.query_before `
    -Raw -Encoding utf8 | ConvertFrom-Json
$queryAfter = Get-Content -LiteralPath $summary.query_after `
    -Raw -Encoding utf8 | ConvertFrom-Json
require ($queryBefore.success -and $queryAfter.success) `
    'independent query baseline failed'
require ((@($queryBefore.baseline.aim_device_ids_before | Sort-Object) -join ',') `
    -eq (@($queryAfter.baseline.aim_device_ids_after | Sort-Object) -join ',')) `
    'final AIM device list differs from baseline'
require ((@($queryBefore.baseline.physical_drive_numbers_before | Sort-Object) -join ',') `
    -eq (@($queryAfter.baseline.physical_drive_numbers_after | Sort-Object) -join ',')) `
    'final PhysicalDrive list differs from baseline'

# Run a new read-only query after the harness has restored deviosvc. This is
# independent of the harness's query-after, which intentionally runs while
# deviosvc is still stopped.
$summaryBaseName = [IO.Path]::GetFileNameWithoutExtension($SummaryPath)
$finalQueryPath = Join-Path (Split-Path -Parent $SummaryPath) `
    ($summaryBaseName + '-final-query.json')
require (-not (Test-Path -LiteralPath $finalQueryPath)) `
    "refusing to overwrite final query evidence: $finalQueryPath"
$executablePath = [string]$reports[0].environment.executable_path
require (Test-Path -LiteralPath $executablePath) `
    "Release executable is missing: $executablePath"
& $executablePath query-driver --report $finalQueryPath | Out-Null
require ($LASTEXITCODE -eq 0) `
    "post-restore query-driver failed (exit $LASTEXITCODE)"
& "$PSScriptRoot\Verify-M0Json.ps1" -Path $finalQueryPath | Out-Null
$finalQuery = Get-Content -LiteralPath $finalQueryPath `
    -Raw -Encoding utf8 | ConvertFrom-Json
require ($finalQuery.environment.executable_sha256 -eq $executableHashes[0]) `
    'post-restore query used a different executable'
require ($finalQuery.driver.sha256 -eq $driverHashes[0]) `
    'post-restore query observed a different driver'
require ((@($queryBefore.baseline.aim_device_ids_before | Sort-Object) -join ',') `
    -eq (@($finalQuery.baseline.aim_device_ids_after | Sort-Object) -join ',')) `
    'post-restore AIM device list differs from baseline'
require ((@($queryBefore.baseline.physical_drive_numbers_before | Sort-Object) -join ',') `
    -eq (@($finalQuery.baseline.physical_drive_numbers_after | Sort-Object) -join ',')) `
    'post-restore PhysicalDrive list differs from baseline'

foreach ($sessionId in $sessionIds) {
    $baseName = "Global\myrd-m0-$sessionId"
    try {
        $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            $baseName)
        $mapping.Dispose()
        throw "mapping still exists: $baseName"
    } catch [IO.FileNotFoundException] {
    }
    foreach ($eventName in @("${baseName}_Request", "${baseName}_Response")) {
        try {
            $event = [Threading.EventWaitHandle]::OpenExisting($eventName)
            $event.Dispose()
            throw "event still exists: $eventName"
        } catch [Threading.WaitHandleCannotBeOpenedException] {
        }
    }
    try {
        $mutex = [Threading.Mutex]::OpenExisting("${baseName}_Server")
        $mutex.Dispose()
        throw "server mutex still exists: ${baseName}_Server"
    } catch [Threading.WaitHandleCannotBeOpenedException] {
    }
}

$phdskmntService = Get-CimInstance Win32_SystemDriver `
    -Filter "Name='phdskmnt'"
$deviosvcService = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
require ($null -ne $phdskmntService) `
    'phdskmnt system driver is missing after V6'
require ($null -ne $deviosvcService) `
    'deviosvc service is missing after V6'
require ($phdskmntService.State -eq 'Running') `
    'phdskmnt is not Running after V6'
require ($deviosvcService.State -eq $summary.original_deviosvc_state) `
    'deviosvc final state differs from baseline'
require ($deviosvcService.StartMode -eq
         $summary.original_deviosvc_start_mode) `
    'deviosvc final StartMode differs from baseline'

$aggregate = [ordered]@{
    schema_version = 1
    summary_path = (Resolve-Path -LiteralPath $SummaryPath).Path
    expected_iterations = $ExpectedIterations
    validated_reports = $reportPaths
    distinct_run_ids = @($runIds | Select-Object -Unique).Count
    distinct_session_ids = @($sessionIds | Select-Object -Unique).Count
    executable_sha256 = $executableHashes[0]
    driver_sha256 = $driverHashes[0]
    query_before = $summary.query_before
    query_after = $summary.query_after
    final_query_after_service_restore = $finalQueryPath
    named_objects_reopen_failed_for_all = $true
    service_state_restored = $true
    service_start_mode_unchanged = $true
    success = $true
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path (Split-Path -Parent $SummaryPath) `
        (([IO.Path]::GetFileNameWithoutExtension($SummaryPath)) +
         '-aggregate.json')
}
require (-not (Test-Path -LiteralPath $OutputPath)) `
    "refusing to overwrite aggregate evidence: $OutputPath"
$aggregate | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $OutputPath -Encoding utf8
$null = Get-Content -LiteralPath $OutputPath -Raw -Encoding utf8 |
    ConvertFrom-Json
Write-Output "V6 aggregate PASS: $OutputPath"
