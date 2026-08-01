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

# ── Preflight: administrator token ────────────────────────────
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This script requires administrator privileges.'
}

# ── Preflight: serial mutex ────────────────────────────────────
$mutex = New-Object System.Threading.Mutex($false, 'Global\myrd-m0-e2e-mutex')
if (-not $mutex.WaitOne(0)) {
    throw 'Another M0 E2E run is already in progress.'
}

try {
    # ── Preflight: services exist ──────────────────────────────
    $phdskmntSvc = Get-Service phdskmnt -ErrorAction Stop
    if ($phdskmntSvc.Status -ne 'Running') {
        throw 'phdskmnt service is not Running'
    }
    $deviosvcSvc = Get-Service deviosvc -ErrorAction Stop

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
    $reportDir = (Resolve-Path $ReportRoot -ErrorAction SilentlyContinue)
    if (-not $reportDir) {
        New-Item -ItemType Directory -Path $ReportRoot -Force | Out-Null
        $reportDir = (Resolve-Path $ReportRoot)
    }
    $runId = Get-Date -Format 'yyyyMMdd-HHmmss'

    # ── Capture deviosvc baseline ───────────────────────────────
    $devioBefore = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
    $devioOriginalState = $devioBefore.State
    $devioOriginalStartMode = $devioBefore.StartMode

    # ── Summary tracking ───────────────────────────────────────
    $attempted = 0
    $passed = 0
    $failedReports = @()
    $allReports = @()

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
        . $repoRoot\tools\Enter-MsvcEnvironment.ps1
        & $exePath query-driver --report $queryBefore
        if ($LASTEXITCODE -ne 0) {
            throw "query-driver baseline failed (exit $LASTEXITCODE)"
        }

        # Run lifecycle iterations
        $failureArg = if ($FailurePoint -ne 'none') {
            @('--inject-failure-after', $FailurePoint)
        } else { @() }

        for ($i = 1; $i -le $Iterations; $i++) {
            $attempted++
            $reportPath = Join-Path $reportDir "run-$runId-$i.json"
            $args = @('run', '--report', $reportPath) + $failureArg
            . $repoRoot\tools\Enter-MsvcEnvironment.ps1
            & $exePath @args 2>&1 | Out-Null
            $exitCode = $LASTEXITCODE
            $allReports += $reportPath

            # Validate report
            try {
                $r = Get-Content $reportPath -Raw -Encoding utf8 |
                     ConvertFrom-Json

                if ($FailurePoint -eq 'none') {
                    # Normal run assertions
                    if (-not $r.success) {
                        throw "report success=false"
                    }
                    if ($r.exit_code -ne 0) {
                        throw "exit_code=$($r.exit_code)"
                    }
                    if ($r.cleanup.exact_remove_succeeded -ne $true) {
                        throw "exact_remove_succeeded=false"
                    }
                    if ($r.cleanup.named_objects_released -ne $true) {
                        throw "named_objects_released=false"
                    }
                    if ($r.provider.fatal_protocol_error_count -ne 0) {
                        throw "fatal_protocol_errors=$($r.provider.fatal_protocol_error_count)"
                    }
                    if ($null -ne $r.raw_round_trip -and
                        $r.raw_round_trip.written_sha256 -ne
                        $r.raw_round_trip.read_sha256) {
                        throw "SHA-256 mismatch"
                    }
                } else {
                    # Fault injection assertions
                    if ($r.success) {
                        throw "injection report success=true (should be false)"
                    }
                    if ($r.cleanup.exact_remove_succeeded -ne $true) {
                        throw "injection cleanup failed: exact_remove_succeeded=false"
                    }
                }

                $passed++
                Write-Output "RUN $i/$Iterations PASS"
            } catch {
                Write-Output "RUN $i/$Iterations FAIL: $_"
                $failedReports += $reportPath
                if ($FailurePoint -eq 'none') {
                    throw # normal run fails stop the harness
                }
            }
        }

        # Query after
        $queryAfter = Join-Path $reportDir "query-after-$runId.json"
        . $repoRoot\tools\Enter-MsvcEnvironment.ps1
        & $exePath query-driver --report $queryAfter
        if ($LASTEXITCODE -ne 0) {
            Write-Output "WARNING: query-driver after run failed (exit $LASTEXITCODE)"
        }

    }
    finally {
        # Restore deviosvc
        if ($devioOriginalState -eq 'Running' -and
            (Get-Service deviosvc).Status -ne 'Running') {
            Start-Service -Name deviosvc -ErrorAction Stop
            (Get-Service deviosvc).WaitForStatus('Running',
                [TimeSpan]::FromSeconds(15))
        }
        $devioAfter = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
        $startModeUnchanged = ($devioAfter.StartMode -eq $devioOriginalStartMode)
        $stateRestored = ($devioAfter.State -eq $devioOriginalState)
        if (-not $startModeUnchanged) {
            Write-Output "ERROR: deviosvc StartMode changed!"
        }
    }

    # ── Write summary ──────────────────────────────────────────
    $summaryPath = Join-Path $reportDir "summary-$runId.json"
    $summary = @{
        run_id = $runId
        iterations = $Iterations
        attempted = $attempted
        passed = $passed
        failed_report_paths = $failedReports
        query_before = $queryBefore
        query_after = $queryAfter
        failure_point = $FailurePoint
        configuration = $Configuration
        service_state_restored = $stateRestored
        service_start_mode_unchanged = $startModeUnchanged
        original_deviosvc_state = $devioOriginalState
        original_deviosvc_start_mode = $devioOriginalStartMode
    } | ConvertTo-Json -Depth 3
    $summary | Out-File -FilePath $summaryPath -Encoding utf8
    Write-Output "SUMMARY: $summaryPath"
    Write-Output "PASSED: $passed / $attempted"

    if ($FailurePoint -eq 'none' -and $passed -ne $attempted) {
        throw "$passed/$attempted passed (normal run must be 100%)"
    }

} finally {
    $mutex.ReleaseMutex()
    $mutex.Dispose()
}
