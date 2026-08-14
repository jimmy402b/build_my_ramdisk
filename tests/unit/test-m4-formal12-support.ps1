[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Need {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-SourceInventory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $inventory = @{}
    Get-ChildItem -LiteralPath $Path -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($Path.Length).TrimStart([IO.Path]::DirectorySeparatorChar)
        $inventory[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
    return $inventory
}

function Assert-SourceInventoryUnchanged {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][hashtable]$Before,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $after = Get-SourceInventory -Path $Path
    Need ($after.Count -eq $Before.Count) "$Label source inventory count changed"
    foreach ($relative in $Before.Keys) {
        Need ($after.ContainsKey($relative)) "$Label source inventory removed $relative"
        Need ($after[$relative] -eq $Before[$relative]) "$Label source content changed $relative"
    }
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$capture = Join-Path $root 'tests\e2e\Capture-M4Child.ps1'
$prepare = Join-Path $root 'tests\e2e\Prepare-M4NegativeFixtures.ps1'
$aimQuery = Join-Path $root 'tests\e2e\m4_aim_query.cpp'
Need ([IO.File]::Exists($capture)) 'missing M4 Formal12 child-capture helper'
Need ([IO.File]::Exists($prepare)) 'missing M4 Formal12 negative-fixture helper'
Need ([IO.File]::Exists($aimQuery)) 'missing M4 Formal12 AIM query helper'

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-formal12-support-' + [Guid]::NewGuid().ToString('N'))
Need (-not [IO.Directory]::Exists($scratch)) 'Formal12 scratch root unexpectedly exists'
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $control = Join-Path $scratch 'control.ps1'
    [IO.File]::WriteAllText($control, @'
[Console]::Out.Write('control-out')
[Console]::Error.Write('control-err')
exit 77
'@, [Text.UTF8Encoding]::new($false))

    $captured = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $capture `
        -Mode Control -ChildFile $control -TimeoutMs 10000 | ConvertFrom-Json)
    Need ($captured.Count -eq 1) 'control capture must emit exactly one JSON result'
    $captured = $captured[0]
    Need ([int]$captured.child_exit -eq 77) 'control capture did not preserve child exit 77'
    Need ([string]$captured.child_stdout -eq 'control-out') 'control capture stdout is not exact'
    Need ([string]$captured.child_stderr -eq 'control-err') 'control capture stderr is not exact'

    $formal = Join-Path $scratch 'formal'
    $bundle = Join-Path $formal 'bundle'
    $gen0 = Join-Path $scratch 'gen0'
    [IO.Directory]::CreateDirectory((Join-Path $bundle 'journals')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $gen0 'journals')) | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $bundle 'disk.rdm'), [byte[]](1,2,3,4))
    [IO.File]::WriteAllBytes((Join-Path $bundle 'journals\journal.bin'), [byte[]](5,6,7,8))
    [IO.File]::WriteAllBytes((Join-Path $gen0 'disk.rdm'), [byte[]](9,10,11,12))
    [IO.File]::WriteAllBytes((Join-Path $gen0 'journals\journal.bin'), [byte[]](13,14,15,16))
    $bundleBefore = Get-SourceInventory -Path $bundle
    $gen0Before = Get-SourceInventory -Path $gen0

    $guid = '\\?\Volume{00000000-0000-0000-0000-000000000001}\'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $prepare `
        -FormalRoot $formal -GenerationZeroSource $gen0 -OccupiedLetter D -OccupiedVolumeGuid $guid
    Need ($LASTEXITCODE -eq 0) 'normal negative-fixture preparation failed'
    $manifestPath = Join-Path $formal 'm4-lifecycle.state.json.negative-fixtures.json'
    Need ([IO.File]::Exists($manifestPath)) 'normal negative-fixture manifest is missing'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Need (($manifest.active_cases -join ',') -eq 'missing_driver') 'normal fixture manifest active_cases is not exactly missing_driver'
    Assert-SourceInventoryUnchanged -Path $bundle -Before $bundleBefore -Label 'bundle'
    Assert-SourceInventoryUnchanged -Path $gen0 -Before $gen0Before -Label 'generation-zero'

    [IO.File]::WriteAllText((Join-Path $formal 'm4-negative-missing_driver.v1.json'), '{}', [Text.UTF8Encoding]::new($false))
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $prepare -FormalRoot $formal -ActivateRemaining
    Need ($LASTEXITCODE -eq 0) 'remaining negative-fixture preparation failed'
    $remaining = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Need (($remaining.active_cases -join ',') -eq 'unknown_nonempty,size_mismatch,occupied_letter,gen0,writer_lock,busy_volume,compact_lock') 'remaining fixture cases are not exact or ordered'
    Assert-SourceInventoryUnchanged -Path $bundle -Before $bundleBefore -Label 'bundle'
    Assert-SourceInventoryUnchanged -Path $gen0 -Before $gen0Before -Label 'generation-zero'

    $querySource = Get-Content -LiteralPath $aimQuery -Raw -Encoding UTF8
    foreach ($required in @('query_version\s*\(', 'list_devices\s*\(')) {
        Need ($querySource -match $required) "AIM query helper is missing call $required"
    }
    foreach ($forbidden in @('create_proxy_disk', 'remove_device', 'query_device', 'rescan_aim_adapter', 'Initialize-Disk', 'Format-Volume')) {
        Need ($querySource -notmatch [regex]::Escape($forbidden)) "AIM query helper contains forbidden capability $forbidden"
    }
}
finally {
    if ([IO.Directory]::Exists($scratch)) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
Write-Output 'M4 Formal12 support contract PASS'
