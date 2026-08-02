# run-m1-proxy.ps1 — Elevated M1 proxy lifecycle harness with safety boundaries
param(
    [Parameter(Mandatory=$true)]
    [string]$Executable,
    [string]$ReportDir = 'reports\m1-v4',
    [int]$Iterations = 1,
    [switch]$DebugFailurePoint
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path "$ScriptDir\..\.."

# ── Preflight ──────────────────────────────────────────

Write-Host "=== M1 Proxy Harness ==="

# Require admin
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This harness requires Administrator privileges"
}
Write-Host "[PREFLIGHT] Admin token: OK"

# Require phdskmnt running
$phdskmnt = Get-Service -Name 'phdskmnt' -ErrorAction SilentlyContinue
if (-not $phdskmnt -or $phdskmnt.Status -ne 'Running') {
    throw "phdskmnt is not running"
}
Write-Host "[PREFLIGHT] phdskmnt Running: OK"

# Require executable
if (-not (Test-Path -LiteralPath $Executable)) {
    throw "Executable not found: $Executable"
}
$ExeHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
Write-Host "[PREFLIGHT] Executable: $Executable"
Write-Host "[PREFLIGHT] SHA-256: $ExeHash"

# Create report directory
$ReportPath = Join-Path $RepoRoot $ReportDir
if (-not (Test-Path $ReportPath)) {
    New-Item -ItemType Directory -Path $ReportPath -Force | Out-Null
}
Write-Host "[PREFLIGHT] Report dir: $ReportPath"

# ── Capture baselines ──────────────────────────────────

Write-Host "=== Capturing Baselines ==="
$deviosvc = Get-Service -Name 'deviosvc' -ErrorAction SilentlyContinue
if ($deviosvc) {
    Write-Host "deviosvc: State=$($deviosvc.Status) StartMode=$($deviosvc.StartType)"
}

$beforeDevices = Get-CimInstance -ClassName Win32_DiskDrive -Filter "Model LIKE 'Arsenal%'" -ErrorAction SilentlyContinue
$beforePDs = Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Index | Sort-Object
Write-Host "Baseline AIM devices: $($beforeDevices.Count)"
Write-Host "Baseline PhysicalDrives: $($beforePDs -join ', ')"

# ── Run lifecycle ──────────────────────────────────────

$results = @()
for ($i = 0; $i -lt $Iterations; $i++) {
    $runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + "-$i"
    $childReport = Join-Path $ReportPath "m1-$runId.json"
    $childDigest = Join-Path $ReportPath "m1-$runId.digest"

    Write-Host "=== Run $i/$Iterations id=$runId ==="

    $args = @('run', '--report', $childReport)
    if ($DebugFailurePoint) { $args += '--debug-failure-point' }

    $proc = Start-Process -FilePath $Executable -ArgumentList $args `
        -NoNewWindow -Wait -PassThru
    $exitCode = $proc.ExitCode

    Write-Host "Exit code: $exitCode"

    # Validate child report
    if (Test-Path $childReport) {
        $childHash = (Get-FileHash -LiteralPath $childReport -Algorithm SHA256).Hash
        $childHash | Out-File -LiteralPath $childDigest -Encoding ASCII

        try {
            & "$ScriptDir\..\..\tools\Verify-M1Json.ps1" -ReportPath $childReport
            $results += [PSCustomObject]@{
                RunId = $runId
                ExitCode = $exitCode
                ReportHash = $childHash
                Success = ($exitCode -eq 0)
            }
        } catch {
            Write-Host "VALIDATION FAILED: $_"
            $results += [PSCustomObject]@{
                RunId = $runId
                ExitCode = $exitCode
                ReportHash = $childHash
                Success = $false
                Error = $_.Exception.Message
            }
        }
    } else {
        Write-Host "WARN: No report produced for run $runId"
        $results += [PSCustomObject]@{
            RunId = $runId
            ExitCode = $exitCode
            ReportHash = ''
            Success = $false
            Error = "No report file"
        }
    }
}

# ── Post-run baselines ─────────────────────────────────

Write-Host "=== Post-Run Baselines ==="
$afterDevices = Get-CimInstance -ClassName Win32_DiskDrive -Filter "Model LIKE 'Arsenal%'" -ErrorAction SilentlyContinue
$afterPDs = Get-CimInstance -ClassName Win32_DiskDrive -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Index | Sort-Object

$baselineMatch = ($beforeDevices.Count -eq $afterDevices.Count) -and
                 (($beforePDs -join ',') -eq ($afterPDs -join ','))
Write-Host "AIM devices match: $baselineMatch (before=$($beforeDevices.Count), after=$($afterDevices.Count))"
Write-Host "PD match: $(($beforePDs -join ',') -eq ($afterPDs -join ','))"

# ── Aggregate summary ──────────────────────────────────

$timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$summaryPath = Join-Path $ReportPath "summary-$timestamp-aggregate.json"
$passed = ($results | Where-Object { $_.Success }).Count

$summary = [PSCustomObject]@{
    schema_version = 1
    attempted = $Iterations
    passed = $passed
    executable_sha256 = $ExeHash
    baseline_aim_devices_match = $baselineMatch
    baseline_physical_drives_match = (($beforePDs -join ',') -eq ($afterPDs -join ','))
    runs = $results
}
$summary | ConvertTo-Json -Depth 4 | Out-File -LiteralPath $summaryPath -Encoding UTF8
Write-Host "=== Aggregate: $summaryPath ==="
Write-Host "=== $passed/$Iterations PASS ==="

exit ($passed -eq $Iterations ? 0 : 1)
