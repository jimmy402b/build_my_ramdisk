[CmdletBinding()]
param(
    [ValidateSet('M2-V6', 'M2-V7')][string]$Tier,
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

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Quote-NativeArgument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Write-TextEvidence {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text)
    return [pscustomobject]@{
        path = $Path
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Invoke-ProcessCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )
    $full = [IO.Path]::GetFullPath($FilePath)
    Require ([IO.File]::Exists($full)) "process executable is missing: $full"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $full
    $startInfo.Arguments = (@($Arguments | ForEach-Object { Quote-NativeArgument ([string]$_) }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Require $process.Start() "failed to start process: $full"
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = [int]$process.ExitCode
            StandardOutput = [string]$stdoutTask.GetAwaiter().GetResult()
            StandardError = [string]$stderrTask.GetAwaiter().GetResult()
        }
    }
    finally { $process.Dispose() }
}

function Relative-EvidencePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $pathFull = [IO.Path]::GetFullPath($Path)
    Require ($pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) "evidence path escaped aggregate directory: $pathFull"
    return $pathFull.Substring($rootFull.Length)
}

function Invoke-CapturedGate {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$Directory
    )
    $result = Invoke-ProcessCapture -FilePath $FilePath -Arguments $Arguments
    $stdout = Write-TextEvidence -Path (Join-Path $Directory "$Name.stdout.log") -Text $result.StandardOutput
    $stderr = Write-TextEvidence -Path (Join-Path $Directory "$Name.stderr.log") -Text $result.StandardError
    [pscustomobject][ordered]@{
        exit_code = $result.ExitCode
        stdout_path = Relative-EvidencePath $script:RunDirectory $stdout.path
        stdout_sha256 = $stdout.sha256
        stderr_path = Relative-EvidencePath $script:RunDirectory $stderr.path
        stderr_sha256 = $stderr.sha256
        combined_output = $result.StandardOutput + [Environment]::NewLine + $result.StandardError
    }
}

function Get-ChildEntry {
    param([string]$ChildDirectory, [int]$ExpectedExitCode)
    $nativePath = Join-Path $ChildDirectory 'native-process.json'
    Require ([IO.File]::Exists($nativePath)) "native process evidence is missing: $ChildDirectory"
    $native = Get-Content -Raw -LiteralPath $nativePath -Encoding utf8 | ConvertFrom-Json
    Require ([int]$native.exit_code -eq $ExpectedExitCode) "native child exit code mismatch: expected=$ExpectedExitCode actual=$($native.exit_code)"
    $reports = @(Get-ChildItem -LiteralPath $ChildDirectory -File -Filter 'm2-report-*.json')
    Require ($reports.Count -eq 1) "expected one child M2 report, found $($reports.Count): $ChildDirectory"
    $outerPath = Join-Path $ChildDirectory 'outer-inventory.json'
    Require ([IO.File]::Exists($outerPath)) "child outer inventory is missing: $ChildDirectory"
    $report = $reports[0].FullName
    [pscustomobject][ordered]@{
        report_path = Relative-EvidencePath $script:RunDirectory $report
        report_sha256 = (Get-FileHash -LiteralPath $report -Algorithm SHA256).Hash
        process_exit_code = [int]$native.exit_code
        outer_inventory_path = Relative-EvidencePath $script:RunDirectory $outerPath
        outer_inventory_sha256 = (Get-FileHash -LiteralPath $outerPath -Algorithm SHA256).Hash
    }
}

function Invoke-M2Child {
    param(
        [string]$ChildDirectory,
        [string]$FailurePoint = 'none'
    )
    [IO.Directory]::CreateDirectory($ChildDirectory) | Out-Null
    $harness = Join-Path $repoRoot 'tests\e2e\run-m2-dynamic-disk.ps1'
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $harness,
        '-Configuration', $Configuration,
        '-EvidenceDirectory', $ChildDirectory,
        '-Executable', $script:ExecutableFull,
        '-ExpectedDriverService', $ExpectedDriverService,
        '-ExpectedDriverSha256', $ExpectedDriverSha256,
        '-ExpectedBusType', $ExpectedBusType,
        '-ExpectedVendorId', $ExpectedVendorId,
        '-ExpectedProductId', $ExpectedProductId,
        '-AcceptedM1Report', [IO.Path]::GetFullPath($AcceptedM1Report),
        '-SafetyAcknowledged'
    )
    if ($FailurePoint -ne 'none') { $arguments += @('-FailurePoint', $FailurePoint) }
    $result = Invoke-ProcessCapture -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Arguments $arguments
    Write-TextEvidence -Path (Join-Path $ChildDirectory 'harness.stdout.log') -Text $result.StandardOutput | Out-Null
    Write-TextEvidence -Path (Join-Path $ChildDirectory 'harness.stderr.log') -Text $result.StandardError | Out-Null
    Require ($result.ExitCode -eq 0) "M2 child harness failed: point=$FailurePoint exit=$($result.ExitCode) stdout=$($result.StandardOutput) stderr=$($result.StandardError)"
    return Get-ChildEntry -ChildDirectory $ChildDirectory -ExpectedExitCode $(if ($FailurePoint -eq 'none') { 0 } else { 8 })
}

function Parse-GateNumber {
    param([string]$Text, [string]$Pattern, [string]$Name)
    $match = [regex]::Match($Text, $Pattern)
    Require $match.Success "M2-V6 output is missing $Name"
    return [uint64]$match.Groups[1].Value
}

if ($ListCommands) {
    Write-Output "${Tier}: virtual-memory gate, bounded request allocation gate, real fault matrix, and Release soak/regression aggregate"
    exit 0
}

if ($WhatIf) {
    Require (-not [string]::IsNullOrWhiteSpace($Executable)) 'WhatIf requires an executable path'
    $old = Join-Path $PSScriptRoot 'Invoke-M2Verification.ps1'
    $arguments = @('-Tier', 'M2-V5', '-WhatIf', '-EvidenceBase', $EvidenceBase,
        '-Executable', $Executable, '-ExpectedDriverSha256', $ExpectedDriverSha256,
        '-ExpectedDriverService', $ExpectedDriverService,
        '-ExpectedBusType', $ExpectedBusType, '-ExpectedVendorId', $ExpectedVendorId,
        '-ExpectedProductId', $ExpectedProductId)
    if (-not [string]::IsNullOrWhiteSpace($TestStaticInputPath)) {
        $arguments += @('-TestStaticInputPath', $TestStaticInputPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($TestAfterInventoryPath)) {
        $arguments += @('-TestAfterInventoryPath', $TestAfterInventoryPath)
    }
    $result = Invoke-ProcessCapture -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Arguments (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $old) + $arguments)
    Require ($result.ExitCode -eq 0) "M2 WhatIf preflight failed: $($result.StandardError)$($result.StandardOutput)"
    Write-Output "$Tier WHATIF PASS"
    exit 0
}

Require ($Configuration -eq 'Release') "$Tier requires Release configuration"
Require $SafetyAcknowledged 'M2-V6/V7 requires destructive safety acknowledgement'
Require (-not [string]::IsNullOrWhiteSpace($AcceptedM1Report)) 'M2-V6/V7 requires an accepted M1 report'
Require ([IO.File]::Exists([IO.Path]::GetFullPath($AcceptedM1Report))) 'accepted M1 report is missing'
$script:ExecutableFull = if ([string]::IsNullOrWhiteSpace($Executable)) {
    Join-Path $repoRoot 'out\build\msvc\src\Release\myrd-m2-e2e.exe'
} else { [IO.Path]::GetFullPath($Executable) }
Require ([IO.File]::Exists($script:ExecutableFull)) "M2 executable not found: $script:ExecutableFull"
$evidenceBaseFull = [IO.Path]::GetFullPath($EvidenceBase)
Require ([IO.Directory]::Exists($evidenceBaseFull)) 'EvidenceBase must already exist'
Require (((Get-Item -LiteralPath $evidenceBaseFull -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'EvidenceBase must not be a reparse point'
$script:RunDirectory = [IO.Path]::GetFullPath((Join-Path $evidenceBaseFull ("$($Tier.ToLower())-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') + '-' + [Guid]::NewGuid().ToString('N'))))
[IO.Directory]::CreateDirectory($script:RunDirectory) | Out-Null
$candidateHash = (Get-FileHash -LiteralPath $script:ExecutableFull -Algorithm SHA256).Hash
$gitCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
$ctest = (Get-Command ctest.exe -ErrorAction Stop).Source

if ($Tier -eq 'M2-V6') {
    $memoryGate = Invoke-CapturedGate -Name 'memory-gate' -FilePath $ctest -Arguments @('--test-dir', (Join-Path $repoRoot 'out\build\msvc'), '-C', 'Release', '-R', '^integration\.m2\.virtual_memory$', '-V', '--output-on-failure') -Directory $script:RunDirectory
    Require ([int]$memoryGate.exit_code -eq 0) 'M2-V6 virtual-memory integration did not pass'
    $memoryText = [string]$memoryGate.combined_output
    $fixedMetadata = Parse-GateNumber $memoryText 'VM_EMPTY_GATE observed_delta=(\d+)' 'VM_EMPTY_GATE'
    $storeCommitted = Parse-GateNumber $memoryText 'VM_WRITE_GATE store_committed=(\d+)' 'VM_WRITE_GATE store_committed'
    $observedGrowth = Parse-GateNumber $memoryText 'VM_WRITE_GATE store_committed=\d+ observed_total_delta=(\d+)' 'VM_WRITE_GATE observed_total_delta'
    $reclaimed = Parse-GateNumber $memoryText 'VM_RECLAIM_GATE reclaimed=(\d+)' 'VM_RECLAIM_GATE reclaimed'
    $reclaimElapsed = Parse-GateNumber $memoryText 'VM_RECLAIM_GATE reclaimed=\d+ threshold=\d+ deadline_ms=\d+ actual_elapsed_ms=(\d+)' 'VM_RECLAIM_GATE actual_elapsed_ms'
    $memory = [ordered]@{
        integration_exit_code = [int]$memoryGate.exit_code
        log_path = $memoryGate.stdout_path
        log_sha256 = $memoryGate.stdout_sha256
        ready_committed_bytes = $fixedMetadata
        fixed_metadata_bytes = $fixedMetadata
        write_bytes = [uint64]1073741824
        store_committed_after_write_bytes = $storeCommitted
        observed_total_growth_bytes = $observedGrowth
        reclaimed_bytes = $reclaimed
        reclaim_elapsed_ms = $reclaimElapsed
    }
    $requestGate = Invoke-CapturedGate -Name 'request-allocation-gate' -FilePath $ctest -Arguments @('--test-dir', (Join-Path $repoRoot 'out\build\msvc'), '-C', 'Release', '-R', '^(unit\.m2\.block_math|unit\.m1\.request_processor|unit\.m2\.stress)$', '-V', '--output-on-failure') -Directory $script:RunDirectory
    Require ([int]$requestGate.exit_code -eq 0) 'M2-V6 request-allocation test set did not pass'
    $requestText = [string]$requestGate.combined_output
    $growthMatches = [regex]::Matches($requestText, 'M2_RESOURCE_SUMMARY[^\r\n]*private_growth=(\d+)')
    Require ($growthMatches.Count -gt 0) 'M2-V6 request-allocation resource summary is missing'
    $maxGrowth = [uint64]0
    foreach ($match in $growthMatches) { $maxGrowth = [Math]::Max($maxGrowth, [uint64]$match.Groups[1].Value) }
    $request = [ordered]@{
        test_exit_code = [int]$requestGate.exit_code
        log_path = $requestGate.stdout_path
        log_sha256 = $requestGate.stdout_sha256
        constant_space_range_descriptor = $true
        maximum_request_allocation_bytes = [uint64]8388608
        measured_private_growth_bytes = $maxGrowth
    }
    $normalDirectory = Join-Path $script:RunDirectory 'normal'
    $normal = Invoke-M2Child -ChildDirectory $normalDirectory
    $faults = @()
    $faultPoints = @('capture-baseline', 'construct-provider', 'start-provider', 'create-device', 'wait-for-disk', 'setup-write', 'sample-after-write', 'delete-retrim', 'sample-after-retrim', 'performance')
    $index = 0
    foreach ($point in $faultPoints) {
        $index++
        $childDirectory = Join-Path $script:RunDirectory ('fault-{0:D2}-{1}' -f $index, $point)
        $entry = Invoke-M2Child -ChildDirectory $childDirectory -FailurePoint $point
        $entry | Add-Member -NotePropertyName failure_point -NotePropertyValue $point
        $faults += $entry
    }
    $aggregate = [ordered]@{
        schema_version = 1
        evidence_kind = 'M2-V6-AGGREGATE'
        tier = 'M2-V6'
        verdict = 'PASS'
        configuration = 'Release'
        candidate = [ordered]@{ executable_path = $script:ExecutableFull; executable_sha256 = $candidateHash; git_commit = $gitCommit }
        memory = $memory
        request_allocation = $request
        normal_run = $normal
        fault_runs = $faults
    }
    $aggregatePath = Join-Path $script:RunDirectory 'm2-v6-aggregate.json'
    $aggregate | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $aggregatePath -Encoding utf8
    & (Join-Path $repoRoot 'tools\Verify-M2Json.ps1') -Path $aggregatePath -Tier M2-V6 -Executable $script:ExecutableFull
    Write-Output "M2-V6 PASS: $aggregatePath"
    exit 0
}

$cycles = @()
for ($index = 1; $index -le 20; $index++) {
    Write-Output "--- M2-V7 cycle $index/20 ---"
    $childDirectory = Join-Path $script:RunDirectory ('cycle-{0:D2}' -f $index)
    $entry = Invoke-M2Child -ChildDirectory $childDirectory
    $entry | Add-Member -NotePropertyName cycle_index -NotePropertyValue $index
    $cycles += $entry
}

$regressions = [ordered]@{}
foreach ($gate in @(
    @{ name = 'm2_v1'; args = @('-L', 'm2-v1') },
    @{ name = 'm2_v2'; args = @('-L', 'm2-v2') },
    @{ name = 'm2_v3_first'; args = @('-L', 'm2-v3') },
    @{ name = 'm2_v3_second'; args = @('-L', 'm2-v3') },
    @{ name = 'm2_v4'; args = @('-L', 'm2-v4') }
)) {
    $result = Invoke-CapturedGate -Name $gate.name -FilePath $ctest -Arguments (@('--test-dir', (Join-Path $repoRoot 'out\build\msvc'), '-C', 'Release', '--output-on-failure') + $gate.args) -Directory $script:RunDirectory
    Require ([int]$result.exit_code -eq 0) "final regression failed: $($gate.name)"
    $regressions[$gate.name] = [ordered]@{ exit_code = [int]$result.exit_code; log_path = $result.stdout_path; log_sha256 = $result.stdout_sha256 }
}
$m1 = Invoke-CapturedGate -Name 'm1-v2-final' -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repoRoot 'tools\Invoke-M1Verification.ps1'), '-Tier', 'M1-V2', '-Configuration', 'Release') -Directory $script:RunDirectory
Require ([int]$m1.exit_code -eq 0) 'final M1-V2 regression failed'
$regressions['m1_v2'] = [ordered]@{ exit_code = [int]$m1.exit_code; log_path = $m1.stdout_path; log_sha256 = $m1.stdout_sha256 }

$aggregate = [ordered]@{
    schema_version = 1
    evidence_kind = 'M2-V7-AGGREGATE'
    tier = 'M2-V7'
    verdict = 'PASS'
    configuration = 'Release'
    candidate = [ordered]@{ executable_path = $script:ExecutableFull; executable_sha256 = $candidateHash; git_commit = $gitCommit }
    cycle_count = 20
    cycles = $cycles
    regressions = $regressions
}
$aggregatePath = Join-Path $script:RunDirectory 'm2-v7-aggregate.json'
$aggregate | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $aggregatePath -Encoding utf8
& (Join-Path $repoRoot 'tools\Verify-M2Json.ps1') -Path $aggregatePath -Tier M2-V7 -Executable $script:ExecutableFull
Write-Output "M2-V7 PASS: $aggregatePath"
