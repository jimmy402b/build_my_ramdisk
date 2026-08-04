[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [Parameter(Mandatory = $true)][string]$Executable,
    [string]$AcceptedM1Report = '',
    [string]$ExpectedDriverService = 'phdskmnt',
    [Parameter(Mandatory = $true)][string]$ExpectedDriverSha256,
    [string]$ExpectedBusType = 'Fibre Channel',
    [string]$ExpectedVendorId = 'Arsenal',
    [string]$ExpectedProductId = 'Virtual',
    [string]$TestStaticInputPath = '',
    [string]$TestAfterInventoryPath = '',
    [ValidateSet(
        'none',
        'capture-baseline',
        'construct-provider',
        'start-provider',
        'create-device',
        'wait-for-disk',
        'setup-write',
        'sample-after-write',
        'delete-retrim',
        'sample-after-retrim',
        'performance')]
    [string]$FailurePoint = 'none',
    [switch]$SafetyAcknowledged,
    [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $PSScriptRoot 'M2Harness.Common.ps1')

function Assert-NoReparsePath {
    param([string]$Path)
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        Assert-M2Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "unsafe reparse point: $($item.FullName)"
        $item = $item.Parent
    }
}

function Resolve-DriverPath {
    param([string]$Path)
    $value = $Path.Trim().Trim('"')
    if ($value.StartsWith('\SystemRoot\', [StringComparison]::OrdinalIgnoreCase)) {
        $windows = [Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)
        $value = Join-Path $windows $value.Substring(12)
    }
    return [IO.Path]::GetFullPath($value)
}

function Get-ActualStaticInput {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $requiredCommands = @(
        'Get-CimInstance',
        'Get-Disk',
        'Get-FileHash',
        'Get-Partition',
        'Get-Process',
        'Get-Volume',
        'Initialize-Disk',
        'Set-Disk',
        'New-Partition',
        'Format-Volume',
        'Add-PartitionAccessPath',
        'Remove-PartitionAccessPath',
        'Optimize-Volume',
        'fsutil.exe'
    )
    $missingCommands = @($requiredCommands | Where-Object {
        $null -eq (Get-Command $_ -ErrorAction SilentlyContinue)
    })
    $driver = if ($missingCommands -notcontains 'Get-CimInstance') {
        Get-CimInstance Win32_SystemDriver -Filter "Name='$ExpectedDriverService'" -ErrorAction SilentlyContinue
    }
    else {
        $null
    }
    $driverReadable = $null -ne $driver
    $driverPath = ''
    $driverHash = ''
    if ($driverReadable) {
        $driverPath = Resolve-DriverPath ([string]$driver.PathName)
        if ($missingCommands -notcontains 'Get-FileHash' -and [IO.File]::Exists($driverPath)) {
            $driverHash = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash
        }
    }
    $forbiddenNames = @('aim_ll', 'RamDyn', 'AIMtk', 'AIMtk-Dlg', 'AIMtk-svc', 'aim_cli', 'MountImg', 'RamDiskUI', 'deviosvc')
    $forbiddenProcesses = if ($missingCommands -notcontains 'Get-Process') {
        @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $forbiddenNames -contains $_.ProcessName } | ForEach-Object { $_.ProcessName })
    }
    else {
        @()
    }
    [pscustomobject]@{
        IsAdministrator = $isAdmin
        ExecutableExists = [IO.File]::Exists($script:ExecutableFull)
        InventoryProbeExists = [IO.File]::Exists($script:M0ProbeFull)
        Driver = [pscustomobject]@{
            Readable = $driverReadable
            ServiceName = $ExpectedDriverService
            State = if ($driverReadable) { [string]$driver.State } else { '' }
            Sha256 = $driverHash
            FilePath = $driverPath
        }
        ServiceBaselineReadable = $driverReadable
        MissingCommands = $missingCommands
        Evidence = $script:EvidenceSafety
        ForbiddenModules = @()
        ForbiddenProcesses = $forbiddenProcesses
    }
}

function Get-ActualInventory {
    param([ValidateSet('before', 'after')][string]$Label)

    $aimReportPath = Join-Path $script:EvidenceFull "outer-aim-$Label.json"
    Assert-M2Condition (-not [IO.File]::Exists($aimReportPath)) "refusing to overwrite outer AIM $Label inventory"
    $aimProbe = Invoke-M2NativeProcess -FilePath $script:M0ProbeFull -Arguments @(
        'query-driver', '--report', $aimReportPath)
    Assert-M2Condition ([int]$aimProbe.ExitCode -eq 0) "read-only AIM $Label inventory failed: $($aimProbe.StandardError)$($aimProbe.StandardOutput)"
    Assert-M2Condition ([IO.File]::Exists($aimReportPath)) "read-only AIM $Label inventory report is missing"
    $aimReport = Get-Content -Raw -LiteralPath $aimReportPath -Encoding utf8 | ConvertFrom-Json
    Assert-M2Condition ([bool]$aimReport.success -and [int]$aimReport.exit_code -eq 0) "read-only AIM $Label inventory report did not succeed"
    $aimDeviceIds = @($aimReport.baseline.aim_device_ids_before | ForEach-Object { [string]$_ } | Sort-Object)
    $aimDeviceIdsAfter = @($aimReport.baseline.aim_device_ids_after | ForEach-Object { [string]$_ } | Sort-Object)
    Assert-M2Condition (($aimDeviceIds -join ',') -eq ($aimDeviceIdsAfter -join ',')) "read-only AIM $Label probe changed the device list"

    $service = Get-CimInstance Win32_SystemDriver -Filter "Name='$ExpectedDriverService'" -ErrorAction Stop
    Assert-M2Condition ($null -ne $service) 'driver service disappeared while capturing inventory'
    $disks = @(Get-Disk -ErrorAction Stop)
    $volumes = @(Get-Volume -ErrorAction Stop)
    Assert-M2Condition ($disks.Count -gt 0) 'physical disk inventory is empty'
    Assert-M2Condition ($volumes.Count -gt 0) 'volume inventory is empty'
    $diskNumbers = @($disks | Sort-Object Number | ForEach-Object { [string]$_.Number })
    $probeDiskNumbers = @($aimReport.baseline.physical_drive_numbers_before | ForEach-Object { [string]$_ } | Sort-Object)
    Assert-M2Condition (($diskNumbers -join ',') -eq ($probeDiskNumbers -join ',')) "Get-Disk and read-only AIM $Label physical-drive inventories differ"
    $process = Get-Process -Id $PID
    [ordered]@{
        physical_disks = $diskNumbers
        volumes = @($volumes | Sort-Object UniqueId | ForEach-Object { [string]$_.UniqueId })
        mount_points = @($volumes | Where-Object { $null -ne $_.Path } | Sort-Object Path | ForEach-Object { [string]$_.Path })
        m2_device_ids = $aimDeviceIds
        service_state = if ($null -eq $service) { '' } else { [string]$service.State }
        service_start_type = if ($null -eq $service) { '' } else { [string]$service.StartMode }
        process_handle_count = [uint64]$process.HandleCount
        process_thread_count = [uint64]$process.Threads.Count
        process_private_bytes = [uint64]$process.PrivateMemorySize64
    }
}

function ConvertTo-StructuralInventory {
    param($Inventory)
    [ordered]@{
        physical_disks = @($Inventory.physical_disks | ForEach-Object { [string]$_ } | Sort-Object)
        volumes = @($Inventory.volumes | ForEach-Object { [string]$_ } | Sort-Object)
        mount_points = @($Inventory.mount_points | ForEach-Object { [string]$_ } | Sort-Object)
        m2_device_ids = @($Inventory.m2_device_ids | ForEach-Object { [string]$_ } | Sort-Object)
        service_state = [string]$Inventory.service_state
        service_start_type = [string]$Inventory.service_start_type
    }
}

function Publish-WhatIfReport {
    param($Before, $After, [bool]$ActualIsAdmin)
    $path = Join-Path $script:EvidenceFull 'whatif-preflight.json'
    Assert-M2Condition (-not [IO.File]::Exists($path)) 'refusing to overwrite WhatIf evidence'
    $report = [ordered]@{
        schema_version = 1
        mode = 'WHATIF'
        actual_is_administrator = $ActualIsAdmin
        static_preflight_passed = $true
        exact_disk_mutation_permitted = $false
        mutation_commands = @()
        before_inventory = $Before
        after_inventory = $After
        structural_inventory_equal = $true
        volatile_resource_counters_compared = $false
        terminal_verdict = 'PASS'
    }
    $temporary = $path + '.tmp-' + [Guid]::NewGuid().ToString('N')
    try {
        $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
        $null = Get-Content -Raw -LiteralPath $temporary -Encoding utf8 | ConvertFrom-Json
        Move-Item -LiteralPath $temporary -Destination $path
    }
    finally {
        if ([IO.File]::Exists($temporary)) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

$script:EvidenceFull = [IO.Path]::GetFullPath($EvidenceDirectory)
$script:ExecutableFull = [IO.Path]::GetFullPath($Executable)
$script:M0ProbeFull = Join-Path ([IO.Path]::GetDirectoryName($script:ExecutableFull)) 'myrd-m0-poc.exe'
Assert-M2Condition ([IO.Directory]::Exists($script:EvidenceFull)) 'evidence directory must already exist'
Assert-NoReparsePath $script:EvidenceFull
$probePath = Join-Path $script:EvidenceFull ('.write-probe-' + [Guid]::NewGuid().ToString('N'))
try {
    [IO.File]::WriteAllText($probePath, 'probe')
}
finally {
    if ([IO.File]::Exists($probePath)) {
        Remove-Item -LiteralPath $probePath -Force
    }
}
$script:EvidenceSafety = [pscustomobject]@{
    Exists = $true
    IsDirectory = $true
    Writable = $true
    WouldOverwrite = [IO.File]::Exists((Join-Path $script:EvidenceFull 'whatif-preflight.json'))
    HasReparsePoint = $false
}

if (-not [string]::IsNullOrWhiteSpace($TestStaticInputPath)) {
    Assert-M2Condition $WhatIf 'TestStaticInputPath is allowed only with WhatIf'
    $static = Get-Content -Raw -LiteralPath $TestStaticInputPath -Encoding utf8 | ConvertFrom-Json
    $static | Add-Member -NotePropertyName ExecutableExists -NotePropertyValue ([IO.File]::Exists($script:ExecutableFull)) -Force
    $static | Add-Member -NotePropertyName Evidence -NotePropertyValue $script:EvidenceSafety -Force
    $beforeInventory = $static.Inventory
}
else {
    Assert-M2Condition ([string]::IsNullOrWhiteSpace($TestAfterInventoryPath)) 'TestAfterInventoryPath requires TestStaticInputPath'
    $static = Get-ActualStaticInput
    $beforeInventory = $null
}

Test-M2StaticPreflight -InputObject $static -ExpectedDriverService $ExpectedDriverService -ExpectedDriverSha256 $ExpectedDriverSha256 -AllowNonAdministratorForWhatIf:$WhatIf
if ($null -eq $beforeInventory) {
    $beforeInventory = Get-ActualInventory -Label 'before'
}

if ($WhatIf) {
    if (-not [string]::IsNullOrWhiteSpace($TestAfterInventoryPath)) {
        Assert-M2Condition (-not [string]::IsNullOrWhiteSpace($TestStaticInputPath)) 'TestAfterInventoryPath requires TestStaticInputPath'
        $afterInventory = Get-Content -Raw -LiteralPath $TestAfterInventoryPath -Encoding utf8 | ConvertFrom-Json
    }
    else {
        $afterInventory = if (-not [string]::IsNullOrWhiteSpace($TestStaticInputPath)) { $static.Inventory } else { Get-ActualInventory -Label 'after' }
    }
    $beforeStructuralJson = ConvertTo-StructuralInventory $beforeInventory | ConvertTo-Json -Depth 12 -Compress
    $afterStructuralJson = ConvertTo-StructuralInventory $afterInventory | ConvertTo-Json -Depth 12 -Compress
    Assert-M2Condition ($beforeStructuralJson -eq $afterStructuralJson) 'WhatIf changed structural system inventory'
    Publish-WhatIfReport -Before $beforeInventory -After $afterInventory -ActualIsAdmin ([bool]$static.IsAdministrator)
    Write-Output 'M2 dynamic-disk WHATIF PASS; no disk, volume, mount, provider, device, or service mutation executed'
    exit 0
}

Assert-M2Condition $SafetyAcknowledged 'real M2 requires destructive safety acknowledgement'
Assert-M2Condition (-not [string]::IsNullOrWhiteSpace($AcceptedM1Report) -and [IO.File]::Exists([IO.Path]::GetFullPath($AcceptedM1Report))) 'accepted M1 report is missing'
& (Join-Path $repoRoot 'tools\Verify-M1Json.ps1') -ReportPath ([IO.Path]::GetFullPath($AcceptedM1Report)) | Out-Null

$arguments = @(
    '--real',
    '--evidence-dir', $script:EvidenceFull,
    '--accepted-m1-report', [IO.Path]::GetFullPath($AcceptedM1Report),
    '--expected-driver-service', $ExpectedDriverService,
    '--expected-driver-sha256', $ExpectedDriverSha256,
    '--expected-bus-type', $ExpectedBusType,
    '--expected-vendor-id', $ExpectedVendorId,
    '--expected-product-id', $ExpectedProductId,
    '--disk-size-bytes', '68719476736',
    '--test-policy',
    '--max-snapshot-batch-bytes', '67108864',
    '--max-snapshot-batch-records', '1024',
    '--lock-stripe-count', '64',
    '--i-understand-this-test-initializes-a-new-disk'
)
if ($FailurePoint -ne 'none') {
    $arguments += @('--inject-failure-after', $FailurePoint)
}
$child = Invoke-M2NativeProcess -FilePath $script:ExecutableFull -Arguments $arguments
[ordered]@{
    schema_version = 1
    failure_point = $FailurePoint
    exit_code = [int]$child.ExitCode
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $script:EvidenceFull 'native-process.json') -Encoding utf8
$afterInventory = Get-ActualInventory -Label 'after'
$beforeStructuralJson = ConvertTo-StructuralInventory $beforeInventory | ConvertTo-Json -Depth 12 -Compress
$afterStructuralJson = ConvertTo-StructuralInventory $afterInventory | ConvertTo-Json -Depth 12 -Compress
$structuralEqual = $beforeStructuralJson -eq $afterStructuralJson
[ordered]@{
    schema_version = 1
    before = $beforeInventory
    after = $afterInventory
    structural_equal = $structuralEqual
} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $script:EvidenceFull 'outer-inventory.json') -Encoding utf8
Assert-M2Condition $structuralEqual 'real M2 changed structural system inventory'
$expectedExitCode = if ($FailurePoint -eq 'none') { 0 } else { 8 }
Assert-M2Condition ([int]$child.ExitCode -eq $expectedExitCode) "real M2 process returned unexpected exit code: expected=$expectedExitCode actual=$($child.ExitCode) stderr=$($child.StandardError) stdout=$($child.StandardOutput)"
$match = [regex]::Match([string]$child.StandardOutput, 'M2 real report:\s*(.+\.json)\s*$')
Assert-M2Condition $match.Success 'real M2 process did not print a report path'
$reportPath = [IO.Path]::GetFullPath($match.Groups[1].Value.Trim())
Assert-M2Condition ([IO.File]::Exists($reportPath)) 'real M2 report is missing'

if ($FailurePoint -eq 'none') {
    & (Join-Path $repoRoot 'tools\Verify-M2Json.ps1') -Path $reportPath -Tier M2-V5 -Executable $script:ExecutableFull
    Write-Output "M2 dynamic-disk REAL PASS: $reportPath"
    exit 0
}

$nativeValidation = Invoke-M2NativeProcess -FilePath $script:ExecutableFull -Arguments @('--validate-report', $reportPath)
Assert-M2Condition ([int]$nativeValidation.ExitCode -eq 0) "native validation rejected the injected-failure report: $($nativeValidation.StandardError)$($nativeValidation.StandardOutput)"
$faultReport = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
$expectedPhase = $FailurePoint.ToUpperInvariant()
Assert-M2Condition ([string]$faultReport.verdict -eq 'NO-GO') 'injected failure report did not fail closed'
$matchingFailures = @($faultReport.failures | Where-Object {
    [string]$_.phase -eq $expectedPhase -and
    [string]$_.message -match 'injected acceptance failure'
})
Assert-M2Condition ($matchingFailures.Count -eq 1) 'injected failure was not observed at the requested phase'
$matchingPhase = @($faultReport.phases | Where-Object {
    [string]$_.phase_id -eq $expectedPhase -and
    [string]$_.phase_status -eq 'FAILED'
})
Assert-M2Condition ($matchingPhase.Count -eq 1) 'requested injected phase was not recorded as FAILED'

$requiredCleanup = switch ($FailurePoint) {
    'capture-baseline' { @('RESTORE-SERVICE') }
    'construct-provider' { @('STOP-PROVIDER', 'RESTORE-SERVICE') }
    'start-provider' { @('STOP-PROVIDER', 'RESTORE-SERVICE') }
    'create-device' { @('REMOVE-DEVICE', 'STOP-PROVIDER', 'RESTORE-SERVICE') }
    'wait-for-disk' { @('REMOVE-DEVICE', 'STOP-PROVIDER', 'RESTORE-SERVICE') }
    default { @('REMOVE-MOUNTS', 'REMOVE-DEVICE', 'STOP-PROVIDER', 'RESTORE-SERVICE') }
}
foreach ($action in @($faultReport.cleanup_actions)) {
    $isRequired = $requiredCleanup -contains [string]$action.cleanup_action_id
    if ($isRequired) {
        Assert-M2Condition ([bool]$action.cleanup_attempted) "required fault cleanup was not attempted: $($action.cleanup_action_id)"
        Assert-M2Condition ([string]$action.cleanup_result -eq 'SUCCEEDED') "required fault cleanup failed: $($action.cleanup_action_id)"
    }
    else {
        Assert-M2Condition (-not [bool]$action.cleanup_attempted) "unowned resource cleanup was unexpectedly attempted: $($action.cleanup_action_id)"
        Assert-M2Condition ([string]$action.cleanup_result -eq 'NOT-ATTEMPTED') "unowned resource cleanup has an invalid result: $($action.cleanup_action_id)"
    }
}
Assert-M2Condition ([string]$faultReport.final_baseline_comparison.comparison -eq 'MATCH') 'fault run baseline comparison is not MATCH'
Assert-M2Condition ([bool]$faultReport.final_baseline_comparison.exact_match) 'fault run final inventory is not an exact match'
Assert-M2Condition ([bool]$faultReport.final_baseline_comparison.resources_within_tolerance) 'fault run resource counters exceed tolerance'
Write-Output "M2 dynamic-disk EXPECTED FAULT PASS [$FailurePoint]: $reportPath"
