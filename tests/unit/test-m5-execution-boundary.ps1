# Proves the M5 execution-boundary assertion actually fires.
#
# The gate is only worth having if it rejects a real violation and accepts a
# clean target, so each probe below configures a throwaway project that includes
# the real cmake/ExecutionBoundary.cmake and asserts the exact expected outcome.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$ProbeBaseDirectory
)

$ErrorActionPreference = 'Stop'

# **A guest without Visual Studio is an environment fact, not a boundary violation.**
# `Enter-MsvcEnvironment.ps1` needs vswhere.exe (installed with the VS installer); the
# two structural cases used to FAIL with `vswhere.exe not found`, which reads exactly
# like a product regression.  Exit 77 is the suite's SKIP convention (CTest's
# SKIP_RETURN_CODE and the guest runner's skip status).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    Write-Output 'SKIP: vswhere.exe not found (Visual Studio build tools are not installed)'
    exit 77
}

$sourceRoot = [System.IO.Path]::GetFullPath($SourceDirectory)
$probeBaseRoot = [System.IO.Path]::GetFullPath($ProbeBaseDirectory)
$expectedProbeBaseRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'out\build\execution-boundary-probes'))

if (-not $probeBaseRoot.Equals(
        $expectedProbeBaseRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use unexpected execution-boundary probe base: $probeBaseRoot"
}

if (-not (Test-Path -LiteralPath $probeBaseRoot)) {
    New-Item -ItemType Directory -Path $probeBaseRoot -Force | Out-Null
}

$probeBaseItem = Get-Item -LiteralPath $probeBaseRoot -Force
if (($probeBaseItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing to use reparse-point execution-boundary probe base: $probeBaseRoot"
}

$boundaryRelative = 'cmake/ExecutionBoundary.cmake'
$boundaryPath = Join-Path $sourceRoot ($boundaryRelative -replace '/', '\')
if (-not (Test-Path -LiteralPath $boundaryPath)) {
    throw "Missing execution-boundary cmake module: $boundaryPath"
}
$boundaryIncludePath = $boundaryPath -replace '\\', '/'

$probeRunName = 'run-{0}-{1}' -f $PID, ([System.Guid]::NewGuid().ToString('N'))
$probeRoot = [System.IO.Path]::GetFullPath((Join-Path $probeBaseRoot $probeRunName))
$probePrefix = $probeBaseRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $probeRoot.StartsWith(
        $probePrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Generated execution-boundary probe path escaped its base: $probeRoot"
}

New-Item -ItemType Directory -Path $probeRoot | Out-Null

function Invoke-BoundaryProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$TargetBody,
        [Parameter(Mandatory = $true)][bool]$ExpectConfigureFailure,
        [string]$ExpectedMessageFragment = ''
    )

    $caseRoot = Join-Path $probeRoot $Name
    New-Item -ItemType Directory -Path $caseRoot | Out-Null

    $cmakeLists = @(
        'cmake_minimum_required(VERSION 3.25)',
        'project(m5executionboundary NONE)',
        ('include("{0}")' -f $boundaryIncludePath),
        $TargetBody
    ) -join [Environment]::NewLine
    Set-Content -LiteralPath (Join-Path $caseRoot 'CMakeLists.txt') `
        -Value $cmakeLists -Encoding Ascii

    $savedErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    # Ninja explicitly: the default generator on this host resolves to Visual
    # Studio and fails on a missing v145 toolset before the assertion is reached.
    $output = @(& cmake.exe -G Ninja -S $caseRoot -B (Join-Path $caseRoot 'build') 2>&1)
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorPreference
    $rendered = ($output | Out-String)
    # CMake hard-wraps message() text at ~76 columns, so a multi-word fragment
    # is split across lines. Collapse whitespace before matching.
    $normalized = ($rendered -replace '\s+', ' ')

    if ($ExpectConfigureFailure) {
        if ($exitCode -eq 0) {
            Write-Output $rendered
            throw "Probe '$Name' configured successfully but a violation was expected"
        }
        if ($ExpectedMessageFragment -and ($normalized -notmatch [regex]::Escape($ExpectedMessageFragment))) {
            Write-Output $rendered
            throw "Probe '$Name' failed without reporting '$ExpectedMessageFragment'"
        }
        Write-Output "  probe ${Name}: rejected as expected"
    }
    else {
        if ($exitCode -ne 0) {
            Write-Output $rendered
            throw "Probe '$Name' failed to configure but it is a legal target"
        }
        Write-Output "  probe ${Name}: accepted as expected"
    }
}

try {
    . (Join-Path $sourceRoot 'tools\Enter-MsvcEnvironment.ps1')

    # Direct execution-authority link — the canonical violation.
    Invoke-BoundaryProbe -Name 'reject_direct_m4_run' -ExpectConfigureFailure $true `
        -ExpectedMessageFragment 'forbidden execution dependency myrd_m4_run' -TargetBody @'
add_library(boundary_victim INTERFACE)
target_link_libraries(boundary_victim INTERFACE myrd_m4_run)
myrd_assert_execution_boundary(boundary_victim)
'@

    # Driver/AIM reach-through.
    Invoke-BoundaryProbe -Name 'reject_driver' -ExpectConfigureFailure $true `
        -ExpectedMessageFragment 'forbidden execution dependency myrd_driver' -TargetBody @'
add_library(boundary_victim INTERFACE)
target_link_libraries(boundary_victim INTERFACE myrd_common myrd_driver)
myrd_assert_execution_boundary(boundary_victim)
'@

    # M3 management reach-through.
    Invoke-BoundaryProbe -Name 'reject_m3_management' -ExpectConfigureFailure $true `
        -ExpectedMessageFragment 'forbidden execution dependency myrd_m3_management' -TargetBody @'
add_library(boundary_victim INTERFACE)
target_link_libraries(boundary_victim INTERFACE myrd_m3_management)
myrd_assert_execution_boundary(boundary_victim)
'@

    # Propagated through the INTERFACE_LINK_LIBRARIES_DIRECT seam.
    Invoke-BoundaryProbe -Name 'reject_interface_direct' -ExpectConfigureFailure $true `
        -ExpectedMessageFragment 'forbidden execution dependency myrd_m3_windows' -TargetBody @'
add_library(boundary_victim INTERFACE)
set_property(TARGET boundary_victim PROPERTY INTERFACE_LINK_LIBRARIES_DIRECT myrd_m3_windows)
myrd_assert_execution_boundary(boundary_victim)
'@

    # A contract-only target is exactly what the boundary is meant to permit.
    Invoke-BoundaryProbe -Name 'accept_contract_only' -ExpectConfigureFailure $false -TargetBody @'
add_library(boundary_victim INTERFACE)
target_link_libraries(boundary_victim INTERFACE myrd_common bcrypt)
myrd_assert_execution_boundary(boundary_victim)
'@

    # Guard against a silently vacuous check.
    Invoke-BoundaryProbe -Name 'reject_unknown_target' -ExpectConfigureFailure $true `
        -ExpectedMessageFragment 'unknown target' -TargetBody @'
myrd_assert_execution_boundary(boundary_target_that_does_not_exist)
'@

    Write-Output 'Execution boundary probe PASS: violations rejected, contract-only targets accepted.'
}
finally {
    if (Test-Path -LiteralPath $probeRoot) {
        $probeItem = Get-Item -LiteralPath $probeRoot -Force
        if (($probeItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to recursively remove reparse-point probe run: $probeRoot"
        }
        Remove-Item -LiteralPath $probeRoot -Recurse -Force
    }
}
