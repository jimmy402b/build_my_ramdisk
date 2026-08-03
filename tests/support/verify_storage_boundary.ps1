[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,

    [Parameter(Mandatory = $true)]
    [string]$ProbeDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$Generator,

    [Parameter(Mandatory = $true)]
    [string]$Compiler
)

$ErrorActionPreference = 'Stop'

$sourceRoot = [System.IO.Path]::GetFullPath($SourceDirectory)
$probeRoot = [System.IO.Path]::GetFullPath($ProbeDirectory)
$expectedProbeRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $sourceRoot 'out\build\storage-boundary-probe'))

if (-not $probeRoot.Equals(
        $expectedProbeRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean unexpected storage-boundary probe path: $probeRoot"
}

if (Test-Path -LiteralPath $probeRoot) {
    Remove-Item -LiteralPath $probeRoot -Recurse -Force
}

. (Join-Path $sourceRoot 'tools\Enter-MsvcEnvironment.ps1')

$savedErrorPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$configureArguments = @(
    '-S', $sourceRoot,
    '-B', $probeRoot,
    '-G', $Generator,
    '-DBUILD_TESTING:BOOL=OFF',
    "-DCMAKE_CXX_COMPILER:FILEPATH=$Compiler"
)
$configureOutput = @(& cmake.exe @configureArguments 2>&1)
$configureExitCode = $LASTEXITCODE
$ErrorActionPreference = $savedErrorPreference
$configureOutput | ForEach-Object { Write-Output $_ }

if ($configureExitCode -ne 0) {
    throw "Storage-boundary probe configure failed with exit code $configureExitCode"
}

$ErrorActionPreference = 'Continue'
$buildArguments = @(
    '--build', $probeRoot,
    '--config', $Configuration,
    '--target', 'myrd_storage',
    '--verbose'
)
$buildOutput = @(& cmake.exe @buildArguments 2>&1)
$buildExitCode = $LASTEXITCODE
$ErrorActionPreference = $savedErrorPreference
$buildOutput | ForEach-Object { Write-Output $_ }

if ($buildExitCode -ne 0) {
    throw "Storage-boundary probe build failed with exit code $buildExitCode"
}

$renderedBuildOutput = ($buildOutput | Out-String)
if ($renderedBuildOutput -match '(?i)myrd_(proxy|driver)') {
    throw 'myrd_storage build traversed a forbidden proxy/driver target'
}

$forbiddenArtifacts = @(Get-ChildItem -LiteralPath $probeRoot -Recurse -File |
    Where-Object { $_.Name -match '(?i)^myrd_(proxy|driver)(\.|$)' })
if ($forbiddenArtifacts.Count -ne 0) {
    $paths = ($forbiddenArtifacts.FullName -join '; ')
    throw "myrd_storage produced forbidden proxy/driver artifacts: $paths"
}

$storageArtifacts = @(Get-ChildItem -LiteralPath $probeRoot -Recurse -File |
    Where-Object { $_.Name -ieq 'myrd_storage.lib' })
if ($storageArtifacts.Count -ne 1) {
    throw "Expected exactly one myrd_storage.lib, found $($storageArtifacts.Count)"
}

Write-Output 'Storage boundary probe PASS: isolated build produced only the storage dependency closure.'
