[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Orchestrator
)

$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m2-orchestrator-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $staticPath = Join-Path $root 'static.json'
    [pscustomobject]@{
        IsAdministrator = $false
        InventoryProbeExists = $true
        Driver = [pscustomobject]@{
            Readable = $true
            ServiceName = 'phdskmnt'
            State = 'Running'
            Sha256 = ('A' * 64)
        }
        ServiceBaselineReadable = $true
        MissingCommands = @()
        ForbiddenModules = @()
        ForbiddenProcesses = @()
        Inventory = [pscustomobject]@{
            physical_disks = @('0', '1')
            volumes = @('system-volume')
            mount_points = @('C:\')
            m2_device_ids = @()
            service_state = 'Running'
            service_start_type = 'Automatic'
            process_handle_count = 100
            process_thread_count = 10
            process_private_bytes = 67108864
        }
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $staticPath -Encoding utf8

    $afterInventoryPath = Join-Path $root 'after-inventory.json'
    [pscustomobject]@{
        physical_disks = @('0', '1')
        volumes = @('system-volume')
        mount_points = @('C:\')
        m2_device_ids = @()
        service_state = 'Running'
        service_start_type = 'Automatic'
        process_handle_count = 107
        process_thread_count = 11
        process_private_bytes = 68157440
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $afterInventoryPath -Encoding utf8

    $evidenceBase = Join-Path $root 'evidence'
    [IO.Directory]::CreateDirectory($evidenceBase) | Out-Null
    $output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Orchestrator -Tier M2-V5 -WhatIf -EvidenceBase $evidenceBase -Executable $Executable -ExpectedDriverSha256 ('A' * 64) -TestStaticInputPath $staticPath -TestAfterInventoryPath $afterInventoryPath 2>&1)
    Require ($LASTEXITCODE -eq 0) "orchestrator WhatIf failed: $($output -join [Environment]::NewLine)"
    Require (($output -join [Environment]::NewLine) -match 'M2-V5 WHATIF PASS') 'orchestrator did not emit one PASS verdict'

    $runs = @(Get-ChildItem -LiteralPath $evidenceBase -Directory)
    Require ($runs.Count -eq 1) "expected one evidence run, found $($runs.Count)"
    $reportPath = Join-Path $runs[0].FullName 'whatif-preflight.json'
    Require ([IO.File]::Exists($reportPath)) 'WhatIf preflight report is missing'
    $report = Get-Content -Raw -LiteralPath $reportPath -Encoding utf8 | ConvertFrom-Json
    Require ([string]$report.terminal_verdict -eq 'PASS') 'WhatIf report verdict is not PASS'
    Require (-not [bool]$report.actual_is_administrator) 'test must prove non-admin WhatIf is read-only'
    Require (-not [bool]$report.exact_disk_mutation_permitted) 'WhatIf permitted disk mutation'
    Require (@($report.mutation_commands).Count -eq 0) 'WhatIf recorded a mutation command'
    Require ([bool]$report.structural_inventory_equal) 'WhatIf did not certify structural inventory equality'
    Require (-not [bool]$report.volatile_resource_counters_compared) 'WhatIf treated volatile diagnostics as structural state'
    Require ([uint64]$report.before_inventory.process_handle_count -ne [uint64]$report.after_inventory.process_handle_count) 'test did not exercise volatile resource counters'
    Require ([uint64]$report.before_inventory.process_private_bytes -ne [uint64]$report.after_inventory.process_private_bytes) 'test did not preserve volatile diagnostics'

    foreach ($name in @('stdout.log', 'stderr.log', 'command.json', 'exit-code.txt')) {
        Require ([IO.File]::Exists((Join-Path $runs[0].FullName $name))) "missing orchestrator evidence file: $name"
    }

    foreach ($tier in @('M2-V6', 'M2-V7')) {
        $task22EvidenceBase = Join-Path $root ("$tier-evidence")
        [IO.Directory]::CreateDirectory($task22EvidenceBase) | Out-Null
        $task22Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Orchestrator -Tier $tier -WhatIf -EvidenceBase $task22EvidenceBase -Executable $Executable -ExpectedDriverSha256 ('A' * 64) -TestStaticInputPath $staticPath -TestAfterInventoryPath $afterInventoryPath 2>&1)
        Require ($LASTEXITCODE -eq 0) "$tier WhatIf failed: $($task22Output -join [Environment]::NewLine)"
        Require (($task22Output -join [Environment]::NewLine) -match "$tier WHATIF PASS") "$tier WhatIf did not emit PASS"
    }

    $mutatedInventoryPath = Join-Path $root 'mutated-inventory.json'
    [pscustomobject]@{
        physical_disks = @('0', '1', '2')
        volumes = @('system-volume')
        mount_points = @('C:\')
        m2_device_ids = @()
        service_state = 'Running'
        service_start_type = 'Automatic'
        process_handle_count = 107
        process_thread_count = 11
        process_private_bytes = 68157440
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $mutatedInventoryPath -Encoding utf8
    $negativeEvidenceBase = Join-Path $root 'negative-evidence'
    [IO.Directory]::CreateDirectory($negativeEvidenceBase) | Out-Null
    $savedErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $negativeOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Orchestrator -Tier M2-V5 -WhatIf -EvidenceBase $negativeEvidenceBase -Executable $Executable -ExpectedDriverSha256 ('A' * 64) -TestStaticInputPath $staticPath -TestAfterInventoryPath $mutatedInventoryPath 2>&1)
    $negativeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorPreference
    Require ($negativeExitCode -ne 0) 'orchestrator accepted a structural disk inventory mutation'
    Require (($negativeOutput -join [Environment]::NewLine) -match 'WhatIf changed structural system inventory') 'orchestrator did not report the structural mutation'
}
finally {
    if ([IO.Directory]::Exists($root)) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

Write-Output 'M2 orchestrator WhatIf test PASS'
