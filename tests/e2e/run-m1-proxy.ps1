param(
    [ValidateRange(1,20)][int]$Iterations = 1,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [string]$ReportRoot = '.\reports\m1-e2e',
    [ValidateSet('none','provider_ready','create_returned','device_confirmed',
                 'physical_drive_confirmed','write_verified','zero_verified',
                 'unmap_verified','device_removed')]
    [string]$FailurePoint = 'none',
    [string]$Executable = '',
    [string]$RunId = '',
    [string]$DriverServiceName = 'phdskmnt',
    [switch]$TestForceNonAdmin
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repoRoot

$mutex = $null
$mutexOwned = $false
$serviceMutationStarted = $false
$devioOriginal = $null
$restoreError = $null

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Same-Set($Left, $Right) {
    $a = @($Left | ForEach-Object { [string]$_ } | Sort-Object)
    $b = @($Right | ForEach-Object { [string]$_ } | Sort-Object)
    return (($a -join "`n") -eq ($b -join "`n"))
}

function Assert-NoReparsePointToRoot([string]$ExistingPath,
                                     [string]$RequiredRoot) {
    $rootFull = [IO.Path]::GetFullPath($RequiredRoot).TrimEnd('\')
    $item = Get-Item -LiteralPath $ExistingPath -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "ReportRoot must not traverse a reparse point: $($item.FullName)"
        }
        if ($item.FullName.TrimEnd('\') -eq $rootFull) { return }
        $item = $item.Parent
    }
    throw 'ReportRoot ancestry could not be proven below reports'
}

try {
    # All checks before this script stops/starts a service.
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    $isAdmin = $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($TestForceNonAdmin) { $isAdmin = $false }
    Require $isAdmin 'This harness requires Administrator privileges'

    if ($FailurePoint -ne 'none' -and $Configuration -ne 'Debug') {
        throw 'Failure injection requires Debug configuration'
    }
    if ([string]::IsNullOrWhiteSpace($RunId)) {
        $RunId = Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'
    }
    Require ($RunId -match '^[A-Za-z0-9._-]+$') 'RunId contains unsafe characters'

    if ([string]::IsNullOrWhiteSpace($Executable)) {
        $Executable = "out\build\msvc\src\$Configuration\myrd-m1-e2e.exe"
    }
    if ([IO.Path]::IsPathRooted($Executable)) {
        $m1Exe = [IO.Path]::GetFullPath($Executable)
    } else {
        $m1Exe = [IO.Path]::GetFullPath((Join-Path $repoRoot $Executable))
    }
    Require (Test-Path -LiteralPath $m1Exe -PathType Leaf) `
        "Executable not found: $m1Exe"
    $m0Exe = Join-Path $repoRoot `
        "out\build\msvc\src\$Configuration\myrd-m0-poc.exe"
    Require (Test-Path -LiteralPath $m0Exe -PathType Leaf) `
        "M0 query executable not found: $m0Exe"

    $reportsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'reports'))
    $reportFull = if ([IO.Path]::IsPathRooted($ReportRoot)) {
        [IO.Path]::GetFullPath($ReportRoot)
    } else {
        [IO.Path]::GetFullPath((Join-Path $repoRoot $ReportRoot))
    }
    $reportsPrefix = $reportsRoot.TrimEnd('\') + '\'
    Require ($reportFull.StartsWith(
        $reportsPrefix, [StringComparison]::OrdinalIgnoreCase)) `
        'ReportRoot must resolve below the repository reports directory'

    $existingAncestor = $reportFull
    while (-not (Test-Path -LiteralPath $existingAncestor)) {
        $parent = [IO.Directory]::GetParent($existingAncestor)
        Require ($null -ne $parent) `
            'ReportRoot existing ancestry could not be determined'
        $existingAncestor = $parent.FullName
    }
    Assert-NoReparsePointToRoot $existingAncestor $reportsRoot

    $plannedReports = @()
    for ($index = 1; $index -le $Iterations; $index++) {
        $plannedReports += Join-Path $reportFull "run-$RunId-$index.json"
    }
    $summaryPath = Join-Path $reportFull "summary-$RunId.json"
    foreach ($path in @($plannedReports) + @($summaryPath)) {
        Require (-not (Test-Path -LiteralPath $path)) `
            "Refusing to overwrite existing report: $path"
    }

    $mutex = New-Object Threading.Mutex($false, 'Global\myrd-m1-e2e-harness-mutex')
    try {
        $mutexOwned = $mutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        $mutexOwned = $true
    }
    Require $mutexOwned 'Another M1 E2E harness is already in progress'

    $driver = Get-Service -Name $DriverServiceName -ErrorAction SilentlyContinue
    Require ($null -ne $driver -and $driver.Status -eq 'Running') `
        "$DriverServiceName is not running"
    $devioService = Get-Service -Name deviosvc -ErrorAction Stop

    $forbiddenNames = @('aim_ll','RamDyn','AIMtk','AIMtk-Dlg','AIMtk-svc',
                        'aim_cli','MountImg','RamDiskUI')
    $forbidden = @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $forbiddenNames -contains $_.ProcessName })
    Require ($forbidden.Count -eq 0) `
        "Forbidden AIM process is running: $($forbidden.ProcessName -join ',')"

    if ($Configuration -eq 'Release') {
        & "$repoRoot\tools\Audit-M0Dependencies.ps1" -Executable $m1Exe
        if ($LASTEXITCODE -ne 0) { throw 'Release import audit failed' }
    }

    if (-not (Test-Path -LiteralPath $reportFull)) {
        New-Item -ItemType Directory -Path $reportFull -Force | Out-Null
    }
    $reportDir = (Resolve-Path -LiteralPath $reportFull).Path
    Assert-NoReparsePointToRoot $reportDir $reportsRoot

    $devioOriginal = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
    $originalState = $devioOriginal.State
    $originalStartMode = $devioOriginal.StartMode
    $originalPath = $devioOriginal.PathName
    $originalPid = $devioOriginal.ProcessId

    $childReports = @()
    $childHashes = @{}
    $childExitCodes = @{}
    $queryHashes = @{}
    $passed = 0
    $harnessError = $null
    $queryBefore = Join-Path $reportDir "query-before-$RunId.json"
    $queryAfter = Join-Path $reportDir "query-after-$RunId.json"

    try {
        $serviceMutationStarted = $true
        if ($originalState -eq 'Running') {
            Stop-Service -Name deviosvc -Force -ErrorAction Stop
            (Get-Service deviosvc).WaitForStatus(
                'Stopped', [TimeSpan]::FromSeconds(15))
        }
        Require ((Get-Service deviosvc).Status -eq 'Stopped') `
            'deviosvc did not stop for the isolated test window'

        & $m0Exe query-driver --report $queryBefore | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "M0 query-before failed (exit $LASTEXITCODE)"
        }
        & "$repoRoot\tools\Verify-M0Json.ps1" -Path $queryBefore | Out-Null
        $queryHashes[$queryBefore] = (Get-FileHash -LiteralPath $queryBefore `
            -Algorithm SHA256).Hash

        for ($index = 1; $index -le $Iterations; $index++) {
            $reportPath = $plannedReports[$index - 1]
            $childArgs = @('run', '--report', $reportPath)
            if ($FailurePoint -ne 'none') {
                $childArgs += @('--inject-failure-after', $FailurePoint)
            }
            & $m1Exe @childArgs | Out-Null
            $exitCode = $LASTEXITCODE
            $childReports += $reportPath
            $childExitCodes[$reportPath] = $exitCode
            & "$repoRoot\tools\Verify-M1Json.ps1" `
                -ReportPath $reportPath -ChildExitCode $exitCode `
                -ExpectedFailurePoint $FailurePoint | Out-Null
            $childHashes[$reportPath] = (Get-FileHash -LiteralPath $reportPath `
                -Algorithm SHA256).Hash
            $passed++
            Write-Output "M1 RUN $index/$Iterations PASS"
        }

        & $m0Exe query-driver --report $queryAfter | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "M0 query-after failed (exit $LASTEXITCODE)"
        }
        & "$repoRoot\tools\Verify-M0Json.ps1" -Path $queryAfter | Out-Null
        $queryHashes[$queryAfter] = (Get-FileHash -LiteralPath $queryAfter `
            -Algorithm SHA256).Hash
        $before = Get-Content -Raw -LiteralPath $queryBefore -Encoding utf8 |
            ConvertFrom-Json
        $after = Get-Content -Raw -LiteralPath $queryAfter -Encoding utf8 |
            ConvertFrom-Json
        Require (Same-Set $before.baseline.aim_device_ids_before `
                          $after.baseline.aim_device_ids_after) `
            'M0 query AIM baseline changed across M1 run'
        Require (Same-Set $before.baseline.physical_drive_numbers_before `
                          $after.baseline.physical_drive_numbers_after) `
            'M0 query PhysicalDrive baseline changed across M1 run'
    } catch {
        $harnessError = $_
    } finally {
        try {
            $current = (Get-CimInstance Win32_Service `
                -Filter "Name='deviosvc'").State
            if ($originalState -eq 'Running' -and $current -ne 'Running') {
                Start-Service -Name deviosvc -ErrorAction Stop
                (Get-Service deviosvc).WaitForStatus(
                    'Running', [TimeSpan]::FromSeconds(15))
            } elseif ($originalState -eq 'Stopped' -and $current -ne 'Stopped') {
                Stop-Service -Name deviosvc -Force -ErrorAction Stop
                (Get-Service deviosvc).WaitForStatus(
                    'Stopped', [TimeSpan]::FromSeconds(15))
            }
        } catch {
            $restoreError = $_
        }
    }

    $devioAfter = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
    $stateRestored = ($devioAfter.State -eq $originalState)
    $startModeUnchanged = ($devioAfter.StartMode -eq $originalStartMode)
    Require $stateRestored 'deviosvc state was not restored'
    Require $startModeUnchanged 'deviosvc StartMode changed'
    if ($null -ne $restoreError) { throw $restoreError }

    foreach ($reportPath in $childReports) {
        if (-not $childHashes.ContainsKey($reportPath)) { continue }
        $hash = (Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash
        Require ($hash -eq $childHashes[$reportPath]) `
            "immutable child report changed: $reportPath"
        & "$repoRoot\tools\Verify-M1Json.ps1" `
            -ReportPath $reportPath `
            -ChildExitCode ([int]$childExitCodes[$reportPath]) `
            -ExpectedFailurePoint $FailurePoint | Out-Null
    }
    foreach ($queryPath in @($queryBefore, $queryAfter)) {
        if (-not $queryHashes.ContainsKey($queryPath)) { continue }
        $hash = (Get-FileHash -LiteralPath $queryPath -Algorithm SHA256).Hash
        Require ($hash -eq $queryHashes[$queryPath]) `
            "immutable query report changed: $queryPath"
    }

    $summary = [ordered]@{
        schema_version = 1
        run_id = $RunId
        attempted = $Iterations
        passed = $passed
        configuration = $Configuration
        failure_point = $FailurePoint
        executable_sha256 = (Get-FileHash -LiteralPath $m1Exe `
            -Algorithm SHA256).Hash
        report_paths = $childReports
        child_exit_codes = $childExitCodes
        report_sha256 = $childHashes
        query_before = $queryBefore
        query_after = $queryAfter
        query_sha256 = $queryHashes
        service_state_restored = $stateRestored
        service_start_mode_unchanged = $startModeUnchanged
        original_deviosvc_state = $originalState
        original_deviosvc_start_mode = $originalStartMode
        original_deviosvc_path = $originalPath
        original_deviosvc_process_id = $originalPid
        harness_error = if ($null -eq $harnessError) { $null } else {
            [string]$harnessError
        }
    }
    $summary | ConvertTo-Json -Depth 8 |
        Out-File -LiteralPath $summaryPath -Encoding utf8
    $null = Get-Content -Raw -LiteralPath $summaryPath -Encoding utf8 |
        ConvertFrom-Json
    if ($null -ne $harnessError) { throw $harnessError }
    Require ($passed -eq $Iterations) `
        "M1 harness passed $passed of $Iterations runs"
    Write-Output "SUMMARY: $summaryPath"
    Write-Output "PASSED: $passed / $Iterations"
} finally {
    if ($serviceMutationStarted -and $null -ne $devioOriginal -and
        $null -ne $restoreError) {
        Write-Error "deviosvc restoration failed: $restoreError"
    }
    if ($mutexOwned) { $mutex.ReleaseMutex() }
    if ($null -ne $mutex) { $mutex.Dispose() }
    Pop-Location
}
