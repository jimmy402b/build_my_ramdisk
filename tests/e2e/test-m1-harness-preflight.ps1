# Non-destructive preflight tests for run-m1-proxy.ps1.
$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
$harness = Join-Path $scriptDir 'run-m1-proxy.ps1'
Set-Location $repoRoot

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Snapshot-State {
    $service = Get-CimInstance Win32_Service -Filter "Name='deviosvc'"
    $drives = @(Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Index | Sort-Object)
    $aim = @(Get-CimInstance Win32_DiskDrive `
        -Filter "Model LIKE 'Arsenal%'" -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty DeviceID | Sort-Object)
    return [ordered]@{
        service_state = if ($null -eq $service) { '' } else { $service.State }
        service_start_mode = if ($null -eq $service) { '' } else { $service.StartMode }
        service_path = if ($null -eq $service) { '' } else { $service.PathName }
        drives = $drives -join ','
        aim = $aim -join ','
    }
}

function Assert-Same-State($Before, $After, [string]$Label) {
    foreach ($key in $Before.Keys) {
        Require ($Before[$key] -eq $After[$key]) `
            "$Label changed ${key}: before=$($Before[$key]) after=$($After[$key])"
    }
}

function Invoke-ExpectedFailure([string[]]$Arguments, [string]$Expected) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File $harness @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    Require ($exitCode -ne 0) "harness unexpectedly succeeded: $($output -join "`n")"
    Require (($output -join "`n") -match [regex]::Escape($Expected)) `
        "unexpected failure (exit=$exitCode): $($output -join "`n")"
}

function New-TestReportRoot([string]$Id) {
    $path = Join-Path $repoRoot "reports\m1-preflight-$Id"
    if (Test-Path -LiteralPath $path) {
        throw "test report root unexpectedly exists: $path"
    }
    return $path
}

$id = [guid]::NewGuid().ToString('N')
$before = Snapshot-State
$created = @()
$keeper = $null
$keeperProcess = $null
$junction = $null
$outside = $null
try {
    # 1. Explicit non-admin branch; it must fail before any service touch.
    $root = New-TestReportRoot $id
    Invoke-ExpectedFailure @('-ReportRoot', $root, '-TestForceNonAdmin') `
        'Administrator privileges'
    $created += $root
    Write-Output '[1/8] non-admin rejection PASS'

    # 2. Missing executable.
    $root = New-TestReportRoot ($id + '-missing')
    Invoke-ExpectedFailure @('-ReportRoot', $root, '-Executable',
        'out\build\msvc\src\Debug\does-not-exist.exe') 'Executable not found'
    $created += $root
    Write-Output '[2/8] missing executable PASS'

    # 3. Report root outside repository reports.
    $outsideRoot = Join-Path $env:TEMP ('myrd-m1-outside-' + $id)
    Invoke-ExpectedFailure @('-ReportRoot', $outsideRoot) 'below the repository reports'
    Write-Output '[3/8] outside report root PASS'

    # 4. Existing junction below reports is rejected before directory creation.
    $outside = Join-Path $env:TEMP ('myrd-m1-reparse-' + $id)
    $junction = Join-Path $repoRoot ('reports\m1-preflight-junction-' + $id)
    New-Item -ItemType Directory -Path $outside | Out-Null
    New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
    Invoke-ExpectedFailure @('-ReportRoot',
        (Join-Path $junction 'child')) 'reparse point'
    Require (-not (Test-Path -LiteralPath (Join-Path $outside 'child'))) `
        'harness followed the preflight junction'
    Write-Output '[4/8] reparse rejection PASS'

    # 5. Existing child report is rejected without overwrite.
    $existingRoot = Join-Path $repoRoot ('reports\m1-preflight-existing-' + $id)
    New-Item -ItemType Directory -Path $existingRoot | Out-Null
    $existing = Join-Path $existingRoot "run-$id-1.json"
    [IO.File]::WriteAllText($existing, '{"sentinel":true}')
    Invoke-ExpectedFailure @('-ReportRoot', $existingRoot, '-RunId', $id) `
        'Refusing to overwrite existing report'
    Require ((Get-Content -Raw -LiteralPath $existing) -eq '{"sentinel":true}') `
        'existing report was modified'
    $created += $existingRoot
    Write-Output '[5/8] overwrite rejection PASS'

    # 6. Non-blocking harness mutex.
    $mutexName = 'Global\myrd-m1-e2e-harness-mutex'
    $childSource = @"
`$mutex = New-Object Threading.Mutex(`$false, '$mutexName')
`$null = `$mutex.WaitOne()
Start-Sleep -Seconds 15
"@
    $encoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($childSource))
    $keeperProcess = Start-Process powershell.exe -ArgumentList @(
        '-NoProfile','-EncodedCommand',$encoded) -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 500
    $root = New-TestReportRoot ($id + '-mutex')
    Invoke-ExpectedFailure @('-ReportRoot', $root) 'already in progress'
    $created += $root
    if (-not $keeperProcess.HasExited) {
        Stop-Process -Id $keeperProcess.Id -Force
        $keeperProcess.WaitForExit()
    }
    Write-Output '[6/8] mutex rejection PASS'

    # 7. Driver-not-running branch uses a nonexistent service name, so no
    # service state is changed.
    $root = New-TestReportRoot ($id + '-driver')
    Invoke-ExpectedFailure @('-ReportRoot', $root,
        '-DriverServiceName', ('__missing_phdskmnt_' + $id)) 'not running'
    $created += $root
    Write-Output '[7/8] driver state rejection PASS'

    # 8. Release must reject failure injection before looking for/building an
    # executable or touching a service.
    $root = New-TestReportRoot ($id + '-release')
    Invoke-ExpectedFailure @('-Configuration','Release', '-ReportRoot', $root,
        '-FailurePoint','provider_ready') 'requires Debug'
    $created += $root
    Write-Output '[8/8] Release failure-injection rejection PASS'
} finally {
    if ($null -ne $keeperProcess -and -not $keeperProcess.HasExited) {
        Stop-Process -Id $keeperProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $junction -and (Test-Path -LiteralPath $junction)) {
        Remove-Item -LiteralPath $junction -Force
    }
    if ($null -ne $outside -and (Test-Path -LiteralPath $outside)) {
        Remove-Item -LiteralPath $outside -Recurse -Force
    }
    foreach ($path in $created) {
        if ([IO.Path]::GetFullPath($path).StartsWith(
                [IO.Path]::GetFullPath((Join-Path $repoRoot 'reports')),
                [StringComparison]::OrdinalIgnoreCase) -and
            (Test-Path -LiteralPath $path)) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
}
$after = Snapshot-State
Assert-Same-State $before $after 'preflight test'
Write-Output 'M1 harness preflight: 8/8 PASS; system state unchanged'
