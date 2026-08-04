[CmdletBinding()]
param(
    [ValidateSet('M2-V0', 'M2-V1', 'M2-V2', 'M2-V3', 'M2-V4', 'M2-V5', 'M2-V6', 'M2-V7')]
    [string]$Tier = 'M2-V5',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$EvidenceBase = '.\reports\m2-verification',
    [string]$Executable = '',
    [string]$AcceptedM1Report = '',
    [string]$ExpectedDriverService = 'phdskmnt',
    [Parameter(Mandatory = $true)][string]$ExpectedDriverSha256,
    [string]$ExpectedBusType = 'Fibre Channel',
    [string]$ExpectedVendorId = 'Arsenal',
    [string]$ExpectedProductId = 'Virtual',
    [string]$TestStaticInputPath = '',
    [string]$TestAfterInventoryPath = '',
    [switch]$SafetyAcknowledged,
    [switch]$WhatIf,
    [switch]$ListCommands
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'M2Verification.Common.ps1')

if ($Tier -in @('M2-V6', 'M2-V7')) {
    & (Join-Path $PSScriptRoot 'Invoke-M2Task22.ps1') @PSBoundParameters
    exit $LASTEXITCODE
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Quote-NativeArgument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

if ($ListCommands) {
    Write-Output "${Tier}: configure/build; local gates; safe dynamic-disk harness; immutable JSON verification"
    exit 0
}

Require ($Tier -eq 'M2-V5') 'unsupported M2 verification tier'
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $repoRoot "out\build\msvc\src\$Configuration\myrd-m2-e2e.exe"
}
$executableFull = [IO.Path]::GetFullPath($Executable)
Require ([IO.File]::Exists($executableFull)) "M2 executable not found: $executableFull"

$evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
Require ([IO.Directory]::Exists($evidenceBaseFull)) 'EvidenceBase must already exist'
$baseItem = Get-Item -LiteralPath $evidenceBaseFull -Force
Require (($baseItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'EvidenceBase must not be a reparse point'
$runName = '{0}-{1}-{2}' -f $Tier.ToLower(), (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff'), [Guid]::NewGuid().ToString('N')
$runDirectory = [IO.Path]::GetFullPath((Join-Path $evidenceBaseFull $runName))
$basePrefix = $evidenceBaseFull.TrimEnd('\') + '\'
Require ($runDirectory.StartsWith($basePrefix, [StringComparison]::OrdinalIgnoreCase)) 'generated evidence directory escaped EvidenceBase'
Require (-not [IO.Directory]::Exists($runDirectory)) 'generated evidence directory already exists'
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null

$harness = Join-Path $repoRoot 'tests\e2e\run-m2-dynamic-disk.ps1'
$stdoutPath = Join-Path $runDirectory 'stdout.log'
$stderrPath = Join-Path $runDirectory 'stderr.log'
$exitPath = Join-Path $runDirectory 'exit-code.txt'
$commandPath = Join-Path $runDirectory 'command.json'

$childArguments = @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', $harness,
    '-Configuration', $Configuration,
    '-EvidenceDirectory', $runDirectory,
    '-Executable', $executableFull,
    '-ExpectedDriverService', $ExpectedDriverService,
    '-ExpectedDriverSha256', $ExpectedDriverSha256,
    '-ExpectedBusType', $ExpectedBusType,
    '-ExpectedVendorId', $ExpectedVendorId,
    '-ExpectedProductId', $ExpectedProductId
)
if (-not [string]::IsNullOrWhiteSpace($AcceptedM1Report)) {
    $childArguments += @('-AcceptedM1Report', [IO.Path]::GetFullPath($AcceptedM1Report))
}
if (-not [string]::IsNullOrWhiteSpace($TestStaticInputPath)) {
    Require $WhatIf 'TestStaticInputPath is allowed only with WhatIf'
    $childArguments += @('-TestStaticInputPath', [IO.Path]::GetFullPath($TestStaticInputPath))
}
if (-not [string]::IsNullOrWhiteSpace($TestAfterInventoryPath)) {
    Require $WhatIf 'TestAfterInventoryPath is allowed only with WhatIf'
    Require (-not [string]::IsNullOrWhiteSpace($TestStaticInputPath)) 'TestAfterInventoryPath requires TestStaticInputPath'
    $childArguments += @('-TestAfterInventoryPath', [IO.Path]::GetFullPath($TestAfterInventoryPath))
}
if ($WhatIf) { $childArguments += '-WhatIf' }
if ($SafetyAcknowledged) { $childArguments += '-SafetyAcknowledged' }

[ordered]@{
    schema_version = 1
    tier = $Tier
    configuration = $Configuration
    executable = $executableFull
    executable_sha256 = (Get-FileHash -LiteralPath $executableFull -Algorithm SHA256).Hash
    arguments = $childArguments
    environment = [ordered]@{
        os_version = [Environment]::OSVersion.VersionString
        is_64_bit_process = [Environment]::Is64BitProcess
        processor_count = [Environment]::ProcessorCount
    }
    what_if = [bool]$WhatIf
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $commandPath -Encoding utf8

$argumentString = ($childArguments | ForEach-Object { Quote-NativeArgument ([string]$_) }) -join ' '
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = 'powershell.exe'
$startInfo.Arguments = $argumentString
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
$null = $process.Start()
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
$exitCode = $process.ExitCode
[IO.File]::WriteAllText($stdoutPath, $stdout)
[IO.File]::WriteAllText($stderrPath, $stderr)
[IO.File]::WriteAllText($exitPath, [string]$exitCode)
$process.Dispose()

if ($exitCode -ne 0) {
    throw "$Tier failed (exit=$exitCode): $stderr $stdout"
}
if ($WhatIf) {
    $preflightPath = Join-Path $runDirectory 'whatif-preflight.json'
    Require ([IO.File]::Exists($preflightPath)) 'WhatIf preflight evidence is missing'
    $preflight = Get-Content -Raw -LiteralPath $preflightPath -Encoding utf8 | ConvertFrom-Json
    Require ([string]$preflight.terminal_verdict -eq 'PASS') 'WhatIf preflight verdict is not PASS'
    Require (-not [bool]$preflight.exact_disk_mutation_permitted) 'WhatIf evidence permitted disk mutation'
    Write-Output "$Tier WHATIF PASS"
    exit 0
}

$verifier = Join-Path $repoRoot 'tools\Verify-M2Json.ps1'
$reportPath = Confirm-M2PublishedReport `
    -RunDirectory $runDirectory `
    -Tier $Tier `
    -Executable $executableFull `
    -Verifier $verifier

Write-Output "$Tier PASS: $reportPath"
