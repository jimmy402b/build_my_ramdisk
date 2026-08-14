[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FormalRoot,
    [string]$GenerationZeroSource,
    [ValidatePattern('^[A-Za-z]$')][string]$OccupiedLetter,
    [string]$OccupiedVolumeGuid,
    [switch]$ActivateRemaining
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Need { param([bool]$Condition, [string]$Message) if (-not $Condition) { throw $Message } }
function Inventory([string]$Path) {
    @(Get-ChildItem -LiteralPath $Path -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($Path.Length).TrimStart('\'); length = [int64]$_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    } | ConvertTo-Json -Compress -Depth 4)
}
function Write-Utf8NoBomAtomic([string]$Path, [string]$Text) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($Path)) {
        $backup = "$Path.$([Guid]::NewGuid().ToString('N')).bak"
        [IO.File]::Replace($temporary, $Path, $backup)
        [IO.File]::Delete($backup)
    }
    else {
        [IO.File]::Move($temporary, $Path)
    }
}
function Assert-InitialManifest($Manifest, [string]$Root) {
    $cases = @('unknown_nonempty', 'size_mismatch', 'occupied_letter', 'gen0', 'writer_lock', 'missing_driver', 'busy_volume', 'compact_lock')
    $entryFields = @{
        unknown_nonempty = @('path', 'letter')
        size_mismatch = @('path', 'letter', 'expected_size')
        occupied_letter = @('path', 'letter', 'volume_guid')
        gen0 = @('path', 'letter', 'expected_size')
        writer_lock = @('path', 'letter', 'expected_size')
        missing_driver = @('path', 'letter')
        busy_volume = @('path', 'letter', 'expected_size', 'holder_relative_path')
        compact_lock = @('path', 'letter', 'expected_size')
    }
    Need ($Manifest.schema -ceq 'myrd.m4.negative-fixtures.v1') 'negative fixture schema mismatch'
    Need ((@($Manifest.PSObject.Properties.Name) -join ',') -ceq (('schema', 'active_cases') + $cases -join ',')) 'negative fixture fields are not exact'
    Need ((@($Manifest.active_cases) -join ',') -ceq 'missing_driver') 'negative fixture active cases are not initial'
    foreach ($case in $cases) {
        $entry = $Manifest.$case
        Need ($null -ne $entry) "negative fixture entry is absent: $case"
        Need ((@($entry.PSObject.Properties.Name | Sort-Object) -join ',') -ceq (@($entryFields[$case] | Sort-Object) -join ',')) "negative fixture fields are not exact: $case"
        $expectedPath = Join-Path $Root $case
        Need ([IO.Path]::GetFullPath([string]$entry.path) -ceq $expectedPath) "negative fixture path is invalid: $case"
    }
    foreach ($case in @('unknown_nonempty', 'size_mismatch', 'gen0', 'writer_lock', 'missing_driver', 'busy_volume', 'compact_lock')) {
        Need ([string]$Manifest.$case.letter -ceq 'S') "negative fixture letter is invalid: $case"
    }
    foreach ($case in @('size_mismatch', 'gen0', 'writer_lock', 'busy_volume', 'compact_lock')) {
        Need ([string]$Manifest.$case.expected_size -ceq '64M') "negative fixture size is invalid: $case"
    }
    Need ([string]$Manifest.busy_volume.holder_relative_path -ceq 'holder.bin') 'negative fixture busy holder is invalid'
    Need ([string]$Manifest.occupied_letter.letter -match '^[A-Z]$') 'negative fixture occupied letter is invalid'
    Need ([string]$Manifest.occupied_letter.volume_guid -match '^\\\\\?\\Volume\{[0-9A-Fa-f-]+\}\\$') 'negative fixture occupied GUID is invalid'
    foreach ($case in @('unknown_nonempty', 'occupied_letter', 'missing_driver')) { Need (-not (Test-Path -LiteralPath (Join-Path $Root $case))) "negative fixture absent path exists: $case" }
    foreach ($case in @('size_mismatch', 'gen0', 'writer_lock', 'busy_volume', 'compact_lock')) { Need ([IO.File]::Exists((Join-Path $Root "$case\disk.rdm"))) "negative fixture disk is absent: $case" }
}

$formal = [IO.Path]::GetFullPath($FormalRoot).TrimEnd('\')
$bundle = Join-Path $formal 'bundle'
$negativeRoot = [IO.Path]::GetFullPath("$bundle.r7-negative")
$manifestPath = Join-Path $formal 'm4-lifecycle.state.json.negative-fixtures.json'

if ($ActivateRemaining) {
    Need ([IO.File]::Exists($manifestPath)) 'negative fixture manifest is absent'
    Need ([IO.File]::Exists((Join-Path $formal 'm4-negative-missing_driver.v1.json'))) 'immutable missing-driver evidence is absent'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-InitialManifest $manifest $negativeRoot
    $manifest.active_cases = @('unknown_nonempty', 'size_mismatch', 'occupied_letter', 'gen0', 'writer_lock', 'busy_volume', 'compact_lock')
    Write-Utf8NoBomAtomic $manifestPath (($manifest | ConvertTo-Json -Compress -Depth 8) + [Environment]::NewLine)
    exit 0
}

Need (-not (Test-Path -LiteralPath $negativeRoot)) 'negative root must be absent'
Need (-not (Test-Path -LiteralPath $manifestPath)) 'negative fixture manifest must be absent'
Need ([IO.File]::Exists((Join-Path $bundle 'disk.rdm'))) 'current compacted source disk.rdm is absent'
Need (-not [string]::IsNullOrWhiteSpace($GenerationZeroSource)) 'generation-zero source is required'
Need (-not [string]::IsNullOrWhiteSpace($OccupiedLetter)) 'occupied letter is required'
Need (-not [string]::IsNullOrWhiteSpace($OccupiedVolumeGuid)) 'occupied volume GUID is required'
$generationZero = [IO.Path]::GetFullPath($GenerationZeroSource).TrimEnd('\')
Need ([IO.File]::Exists((Join-Path $generationZero 'disk.rdm'))) 'generation-zero source disk.rdm is absent'
$bundleBefore = Inventory $bundle
$gen0Before = Inventory $generationZero
[void][IO.Directory]::CreateDirectory($negativeRoot)
foreach ($case in @('size_mismatch', 'writer_lock', 'busy_volume', 'compact_lock')) { Copy-Item -LiteralPath $bundle -Destination (Join-Path $negativeRoot $case) -Recurse -ErrorAction Stop }
Copy-Item -LiteralPath $generationZero -Destination (Join-Path $negativeRoot 'gen0') -Recurse -ErrorAction Stop
Need ((Inventory $bundle) -ceq $bundleBefore) 'current compacted source inventory changed'
Need ((Inventory $generationZero) -ceq $gen0Before) 'generation-zero source inventory changed'
foreach ($case in @('unknown_nonempty', 'occupied_letter', 'missing_driver')) { Need (-not (Test-Path -LiteralPath (Join-Path $negativeRoot $case))) "fixture path must be absent: $case" }

$letter = $OccupiedLetter.ToUpperInvariant()
$manifest = [ordered]@{
    schema = 'myrd.m4.negative-fixtures.v1'
    active_cases = @('missing_driver')
    unknown_nonempty = [ordered]@{path = (Join-Path $negativeRoot 'unknown_nonempty'); letter = 'S'}
    size_mismatch = [ordered]@{path = (Join-Path $negativeRoot 'size_mismatch'); letter = 'S'; expected_size = '64M'}
    occupied_letter = [ordered]@{path = (Join-Path $negativeRoot 'occupied_letter'); letter = $letter; volume_guid = $OccupiedVolumeGuid}
    gen0 = [ordered]@{path = (Join-Path $negativeRoot 'gen0'); letter = 'S'; expected_size = '64M'}
    writer_lock = [ordered]@{path = (Join-Path $negativeRoot 'writer_lock'); letter = 'S'; expected_size = '64M'}
    missing_driver = [ordered]@{path = (Join-Path $negativeRoot 'missing_driver'); letter = 'S'}
    busy_volume = [ordered]@{path = (Join-Path $negativeRoot 'busy_volume'); letter = 'S'; expected_size = '64M'; holder_relative_path = 'holder.bin'}
    compact_lock = [ordered]@{path = (Join-Path $negativeRoot 'compact_lock'); letter = 'S'; expected_size = '64M'}
}
Write-Utf8NoBomAtomic $manifestPath (($manifest | ConvertTo-Json -Compress -Depth 8) + [Environment]::NewLine)
