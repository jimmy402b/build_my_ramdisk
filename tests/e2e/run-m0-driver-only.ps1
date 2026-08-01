param(
    [ValidateRange(1,20)][int]$Iterations = 1,
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [string]$ReportRoot = '.\reports\m0-e2e',
    [ValidateSet('none','create_returned','device_confirmed',
                 'physical_drive_confirmed','round_trip_verified')]
    [string]$FailurePoint = 'none'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $root
Set-Location $repoRoot

function Assert-SameStringSet($left, $right, [string]$label) {
    $a = @($left | ForEach-Object { [string]$_ } | Sort-Object)
    $b = @($right | ForEach-Object { [string]$_ } | Sort-Object)
    if (($a -join "`n") -ne ($b -join "`n")) {
        throw "$label changed: before=[$($a -join ',')] after=[$($b -join ',')]"
    }
}

function Assert-NoReparsePointToRoot([string]$ExistingPath,
                                     [string]$RequiredRoot) {
    $rootFullPath = [IO.Path]::GetFullPath($RequiredRoot).TrimEnd('\')
    $pathItem = Get-Item -LiteralPath $ExistingPath -Force
    while ($null -ne $pathItem) {
        if (($pathItem.Attributes -band
                [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "ReportRoot must not traverse a reparse point: $($pathItem.FullName)"
        }
        if ($pathItem.FullName.TrimEnd('\') -eq $rootFullPath) {
            return
        }
        $pathItem = $pathItem.Parent
    }
    throw 'ReportRoot ancestry could not be proven below reports'
}

$outerRunId = Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'
$preflightSummaryDir = Join-Path $repoRoot 'reports\m0-harness-preflight'
$summaryWasWritten = $false
$mutex = $null
$mutexOwned = $false

try {
    # ── Preflight: administrator token ────────────────────────
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $currentPrincipal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'This script requires administrator privileges.'
    }

    # ── Preflight: serial mutex ────────────────────────────────
    $mutex = New-Object System.Threading.Mutex(
        $false, 'Global\myrd-m0-e2e-mutex')
    $mutexAcquired = $false
    try {
        $mutexAcquired = $mutex.WaitOne(0)
    } catch [Threading.AbandonedMutexException] {
        # WaitOne grants ownership before reporting abandonment. Continue with
        # the recovered lock and release it in the outer finally block.
        $mutexAcquired = $true
    }
    if (-not $mutexAcquired) {
        throw 'Another M0 E2E run is already in progress.'
    }
    $mutexOwned = $true

    # ── Preflight: services exist ──────────────────────────────
    $phdskmntSvc = Get-Service phdskmnt -ErrorAction Stop
    if ($phdskmntSvc.Status -ne 'Running') {
        throw 'phdskmnt service is not Running'
    }
    $deviosvcSvc = Get-Service deviosvc -ErrorAction Stop

    $forbiddenNames = @('aim_ll','RamDyn','AIMtk','AIMtk-Dlg','AIMtk-svc','aim_cli','MountImg','RamDiskUI')
    $forbiddenRunning = @(Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $forbiddenNames -contains $_.ProcessName })
    if ($forbiddenRunning.Count -ne 0) {
        throw "Forbidden AIM process is running: $($forbiddenRunning.ProcessName -join ',')"
    }

    # ── Preflight: executable exists ───────────────────────────
    $configLower = $Configuration.ToLower()
    $exeDir = "out\build\msvc\src\$Configuration"
    $exePath = "$exeDir\myrd-m0-poc.exe"
    if (-not (Test-Path -LiteralPath $exePath)) {
        throw "Executable not found: $exePath"
    }

    # ── Preflight: import audit for Release ────────────────────
    if ($Configuration -eq 'Release') {
        . .\tools\Enter-MsvcEnvironment.ps1
        $audit = & .\tools\Audit-M0Dependencies.ps1 -Executable $exePath
        if ($LASTEXITCODE -ne 0) { throw 'Release import audit failed' }
    }

    # ── Preflight: FailurePoint only in Debug ──────────────────
    if ($FailurePoint -ne 'none' -and $Configuration -ne 'Debug') {
        throw 'Failure injection requires Debug configuration'
    }

    # ── Preflight: ReportRoot ──────────────────────────────────
    $reportsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'reports'))
    $reportFullPath = if ([IO.Path]::IsPathRooted($ReportRoot)) {
        [IO.Path]::GetFullPath($ReportRoot)
    } else {
        [IO.Path]::GetFullPath((Join-Path (Get-Location) $ReportRoot))
    }
    $reportsPrefix = $reportsRoot.TrimEnd('\') + '\'
    if (-not $reportFullPath.StartsWith(
            $reportsPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ReportRoot must resolve below the repository reports directory'
    }

    # Check the nearest existing ancestor before creating anything. Otherwise
    # New-Item could follow an existing junction and write outside reports.
    $existingAncestor = $reportFullPath
    while (-not (Test-Path -LiteralPath $existingAncestor)) {
        $parent = [IO.Directory]::GetParent($existingAncestor)
        if ($null -eq $parent) {
            throw 'ReportRoot existing ancestry could not be determined'
        }
        $existingAncestor = $parent.FullName
    }
    Assert-NoReparsePointToRoot $existingAncestor $reportsRoot

    if (-not (Test-Path -LiteralPath $reportFullPath)) {
        New-Item -ItemType Directory -Path $reportFullPath -Force | Out-Null
    }
    $reportDir = (Resolve-Path -LiteralPath $reportFullPath).Path
    Assert-NoReparsePointToRoot $reportDir $reportsRoot
    $preflightSummaryDir = $reportDir
    $runId = (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff')

    # ── Capture deviosvc baseline ───────────────────────────────
    $devioBefore = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
    $devioOriginalState = $devioBefore.State
    $devioOriginalStartMode = $devioBefore.StartMode
    $devioOriginalProcessId = $devioBefore.ProcessId
    $devioOriginalPath = $devioBefore.PathName

    # ── Summary tracking ───────────────────────────────────────
    $attempted = 0
    $passed = 0
    $failedReports = @()
    $allReports = @()
    $childExitCodes = @{}
    $reportSha256 = @{}
    $querySha256 = @{}
    $queryBefore = $null
    $queryAfter = $null
    $harnessError = $null
    $restoreError = $null

    # ── try/finally block ──────────────────────────────────────
    try {
        # Stop deviosvc
        if ($devioOriginalState -eq 'Running') {
            Stop-Service -Name deviosvc -Force -ErrorAction Stop
            (Get-Service deviosvc).WaitForStatus('Stopped',
                [TimeSpan]::FromSeconds(15))
        }
        $during = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
        if ($during.State -ne 'Stopped') {
            throw 'deviosvc did not stop (state=' + $during.State + ')'
        }

        # Run query-driver baselines
        $queryBefore = Join-Path $reportDir "query-before-$runId.json"
        & $exePath query-driver --report $queryBefore
        if ($LASTEXITCODE -ne 0) {
            throw "query-driver baseline failed (exit $LASTEXITCODE)"
        }
        & $repoRoot\tools\Verify-M0Json.ps1 -Path $queryBefore | Out-Null
        $querySha256[$queryBefore] =
            (Get-FileHash -LiteralPath $queryBefore -Algorithm SHA256).Hash

        # Run lifecycle iterations
        $failureArg = if ($FailurePoint -ne 'none') {
            @('--inject-failure-after', $FailurePoint)
        } else { @() }

        for ($i = 1; $i -le $Iterations; $i++) {
            $attempted++
            $reportPath = Join-Path $reportDir "run-$runId-$i.json"
            if (Test-Path -LiteralPath $reportPath) {
                throw "Refusing to overwrite existing report: $reportPath"
            }
            $args = @('run', '--report', $reportPath) + $failureArg
            & $exePath @args 2>&1 | Out-Null
            $exitCode = $LASTEXITCODE
            $allReports += $reportPath
            $childExitCodes[$reportPath] = $exitCode

            # Validate report
            try {
                & $repoRoot\tools\Assert-M0RunReport.ps1 `
                    -Path $reportPath -ChildExitCode $exitCode `
                    -FailurePoint $FailurePoint | Out-Null
                $reportSha256[$reportPath] =
                    (Get-FileHash -LiteralPath $reportPath `
                        -Algorithm SHA256).Hash

                $passed++
                Write-Output "RUN $i/$Iterations PASS"
            } catch {
                Write-Output "RUN $i/$Iterations FAIL: $_"
                $failedReports += $reportPath
                throw
            }
        }

        # Query after
        $queryAfter = Join-Path $reportDir "query-after-$runId.json"
        & $exePath query-driver --report $queryAfter
        if ($LASTEXITCODE -ne 0) {
            throw "query-driver after run failed (exit $LASTEXITCODE)"
        }

        & $repoRoot\tools\Verify-M0Json.ps1 -Path $queryAfter | Out-Null
        $querySha256[$queryAfter] =
            (Get-FileHash -LiteralPath $queryAfter -Algorithm SHA256).Hash
        $beforeReport = Get-Content -LiteralPath $queryBefore -Raw -Encoding utf8 |
            ConvertFrom-Json
        $afterReport = Get-Content -LiteralPath $queryAfter -Raw -Encoding utf8 |
            ConvertFrom-Json
        if ($beforeReport.mode -ne 'query-driver' -or
            $afterReport.mode -ne 'query-driver' -or
            -not $beforeReport.success -or -not $afterReport.success) {
            throw 'query baseline report is invalid'
        }
        if ($null -ne $beforeReport.created_device -or
            $null -ne $afterReport.created_device -or
            $null -ne $beforeReport.raw_round_trip -or
            $null -ne $afterReport.raw_round_trip) {
            throw 'query baseline mutated device state'
        }
        if ($beforeReport.dependency_audit.deviosvc_state_during_run -ne
                'Stopped' -or
            $afterReport.dependency_audit.deviosvc_state_during_run -ne
                'Stopped') {
            throw 'deviosvc was not Stopped for query baselines'
        }
        Assert-SameStringSet $beforeReport.baseline.aim_device_ids_before `
            $afterReport.baseline.aim_device_ids_after 'AIM device baseline'
        Assert-SameStringSet $beforeReport.baseline.physical_drive_numbers_before `
            $afterReport.baseline.physical_drive_numbers_after `
            'PhysicalDrive baseline'

    } catch {
        $harnessError = $_
    }
    finally {
        try {
            # Restore deviosvc to the exact original running/stopped state.
            $currentState = (Get-CimInstance Win32_Service `
                -Filter "Name='deviosvc'").State
            if ($devioOriginalState -eq 'Running' -and
                $currentState -ne 'Running') {
                Start-Service -Name deviosvc -ErrorAction Stop
                (Get-Service deviosvc).WaitForStatus('Running',
                    [TimeSpan]::FromSeconds(15))
            } elseif ($devioOriginalState -eq 'Stopped' -and
                      $currentState -ne 'Stopped') {
                Stop-Service -Name deviosvc -Force -ErrorAction Stop
                (Get-Service deviosvc).WaitForStatus('Stopped',
                    [TimeSpan]::FromSeconds(15))
            }
            $devioAfter = Get-CimInstance Win32_Service `
                -Filter "Name='deviosvc'"
            $startModeUnchanged =
                ($devioAfter.StartMode -eq $devioOriginalStartMode)
            $stateRestored = ($devioAfter.State -eq $devioOriginalState)
        } catch {
            $restoreError = $_
            $startModeUnchanged = $false
            $stateRestored = $false
        }
    }

    # Child reports are immutable evidence published atomically by the C++
    # process. Service restoration is recorded by the harness summary instead
    # of rewriting those reports after publication.
    foreach ($reportPath in $allReports) {
        try {
            $currentHash = (Get-FileHash -LiteralPath $reportPath `
                -Algorithm SHA256).Hash
            if ($currentHash -ne $reportSha256[$reportPath]) {
                throw "published child report changed after validation: $reportPath"
            }
            & $repoRoot\tools\Assert-M0RunReport.ps1 `
                -Path $reportPath `
                -ChildExitCode ([int]$childExitCodes[$reportPath]) `
                -FailurePoint $FailurePoint | Out-Null
        } catch {
            if ($failedReports -notcontains $reportPath) {
                $failedReports += $reportPath
            }
            if ($null -eq $harnessError) { $harnessError = $_ }
        }
    }

    foreach ($queryPath in @($queryBefore, $queryAfter)) {
        if ([string]::IsNullOrWhiteSpace([string]$queryPath)) { continue }
        try {
            $expectedHash = $querySha256[$queryPath]
            if ([string]::IsNullOrWhiteSpace([string]$expectedHash)) {
                throw "missing validated query report hash: $queryPath"
            }
            $currentHash = (Get-FileHash -LiteralPath $queryPath `
                -Algorithm SHA256).Hash
            if ($currentHash -ne $expectedHash) {
                throw "published query report changed after validation: $queryPath"
            }
            & $repoRoot\tools\Verify-M0Json.ps1 -Path $queryPath | Out-Null
        } catch {
            if ($null -eq $harnessError) { $harnessError = $_ }
        }
    }

    if (-not $stateRestored -or -not $startModeUnchanged) {
        if ($null -eq $harnessError) {
            $harnessError = 'deviosvc state or StartMode was not restored'
        }
    }

    # ── Write summary ──────────────────────────────────────────
    $failedReports = @($failedReports | Select-Object -Unique)
    $passed = $attempted - $failedReports.Count
    $summaryPath = Join-Path $reportDir "summary-$runId.json"
    if (Test-Path -LiteralPath $summaryPath) {
        throw "Refusing to overwrite existing summary: $summaryPath"
    }
    $summary = @{
        run_id = $runId
        iterations = $Iterations
        attempted = $attempted
        passed = $passed
        failed_report_paths = $failedReports
        report_paths = $allReports
        child_exit_codes = $childExitCodes
        report_sha256 = $reportSha256
        query_sha256 = $querySha256
        query_before = $queryBefore
        query_after = $queryAfter
        failure_point = $FailurePoint
        configuration = $Configuration
        service_state_restored = $stateRestored
        service_start_mode_unchanged = $startModeUnchanged
        original_deviosvc_state = $devioOriginalState
        original_deviosvc_start_mode = $devioOriginalStartMode
        original_deviosvc_process_id = $devioOriginalProcessId
        original_deviosvc_path = $devioOriginalPath
        restore_error = if ($null -eq $restoreError) { $null } else {
            [string]$restoreError
        }
        harness_error = if ($null -eq $harnessError) { $null } else {
            [string]$harnessError
        }
    } | ConvertTo-Json -Depth 6
    $summary | Out-File -FilePath $summaryPath -Encoding utf8
    $null = Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8 |
        ConvertFrom-Json
    $summaryWasWritten = $true
    Write-Output "SUMMARY: $summaryPath"
    Write-Output "PASSED: $passed / $attempted"

    if ($null -ne $harnessError -or $passed -ne $attempted -or
        $failedReports.Count -ne 0) {
        throw "Harness contract failed: $passed/$attempted passed; $harnessError"
    }

} catch {
    if (-not $summaryWasWritten) {
        New-Item -ItemType Directory -Path $preflightSummaryDir `
            -Force | Out-Null
        $fallbackSummaryPath = Join-Path $preflightSummaryDir `
            "summary-preflight-$outerRunId.json"
        if (Test-Path -LiteralPath $fallbackSummaryPath) {
            throw "Refusing to overwrite fallback summary: $fallbackSummaryPath"
        }
        [ordered]@{
            run_id = $outerRunId
            iterations = $Iterations
            attempted = 0
            passed = 0
            failed_report_paths = @()
            report_paths = @()
            child_exit_codes = @{}
            report_sha256 = @{}
            query_sha256 = @{}
            query_before = $null
            query_after = $null
            failure_point = $FailurePoint
            configuration = $Configuration
            service_state_restored = $null
            service_start_mode_unchanged = $null
            harness_error = [string]$_
        } | ConvertTo-Json -Depth 6 |
            Out-File -LiteralPath $fallbackSummaryPath -Encoding utf8
        $null = Get-Content -LiteralPath $fallbackSummaryPath `
            -Raw -Encoding utf8 | ConvertFrom-Json
        Write-Output "PREFLIGHT SUMMARY: $fallbackSummaryPath"
    }
    throw
} finally {
    if ($mutexOwned) { $mutex.ReleaseMutex() }
    if ($null -ne $mutex) { $mutex.Dispose() }
}
