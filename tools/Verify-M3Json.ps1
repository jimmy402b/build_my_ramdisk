[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$ExpectedSourceCommit,
    [string]$ExpectedBinarySha256 = '',
    [string]$Executable = '',
    [switch]$AllowAbsolutePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-Property($Object, [string]$Name) {
    Require ($null -ne $Object) "object missing while checking $Name"
    Require ($null -ne $Object.PSObject.Properties[$Name]) "required property missing: $Name"
}

function Require-Hash($Value, [string]$Name) {
    Require ([string]$Value -match '^[0-9A-Fa-f]{64}$') "$Name SHA-256 is invalid"
}

function Require-Boolean($Value, [string]$Name) {
    Require ($Value -is [bool]) "$Name must be boolean"
}

function Require-UnsignedInteger($Value, [string]$Name) {
    $integral = $Value -is [byte] -or $Value -is [uint16] -or
        $Value -is [uint32] -or $Value -is [uint64] -or
        $Value -is [int16] -or $Value -is [int32] -or $Value -is [int64]
    Require ($integral) "$Name must be an integer"
    Require ([decimal]$Value -ge 0) "$Name must be nonnegative"
}

function Require-NonEmptyString($Value, [string]$Name) {
    Require ($Value -is [string]) "$Name must be a string"
    Require (-not [string]::IsNullOrWhiteSpace($Value)) "$Name is empty"
}

$fullPath = [IO.Path]::GetFullPath($Path)
Require ([IO.File]::Exists($fullPath)) 'M3 report does not exist'
Require (-not $fullPath.StartsWith('\.\', [StringComparison]::OrdinalIgnoreCase)) 'M3 report path cannot be a device path'
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$raw = [IO.File]::ReadAllText($fullPath, $strictUtf8)
Require (-not [string]::IsNullOrWhiteSpace($raw)) 'M3 report is empty'
$report = $raw | ConvertFrom-Json

foreach ($name in @(
    'schema_version','operation','status','operation_id','started_utc',
    'finished_utc','build','bundle_path_hash','bundle_path','image','slots',
    'selected','verification','cleanup_candidates','artifacts','error')) {
    Require-Property $report $name
}
Require-UnsignedInteger $report.schema_version 'schema_version'
Require ([int]$report.schema_version -eq 1) 'schema_version must be 1'
Require (@('inspect','verify_structural','verify_full') -contains [string]$report.operation) 'operation is unknown'
Require (@('passed','failed','blocked') -contains [string]$report.status) 'status is unknown'
Require ([string]$report.status -eq 'passed') 'required M3 evidence is not passed'
Require (-not [string]::IsNullOrWhiteSpace([string]$report.operation_id)) 'operation id is empty'
Require ([string]$report.started_utc -match '^\d{4}-\d{2}-\d{2}T') 'started timestamp is invalid'
Require ([string]$report.finished_utc -match '^\d{4}-\d{2}-\d{2}T') 'finished timestamp is invalid'

foreach ($name in @('source_commit','build_type','binary_sha256')) {
    Require-Property $report.build $name
}
Require ([string]$report.build.source_commit -match '^[0-9A-Fa-f]{40}$') 'source commit is invalid'
Require ($ExpectedSourceCommit -match '^[0-9A-Fa-f]{40}$') 'expected source commit is invalid'
Require ([string]$report.build.source_commit -ieq $ExpectedSourceCommit) 'source commit mismatch'
Require (-not [string]::IsNullOrWhiteSpace([string]$report.build.build_type)) 'build type is empty'
Require-Hash $report.build.binary_sha256 'binary'
if (-not [string]::IsNullOrWhiteSpace($Executable)) {
    $binaryPath = [IO.Path]::GetFullPath($Executable)
    Require ([IO.File]::Exists($binaryPath)) 'selected M3 executable does not exist'
    $actual = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    Require ($actual -ieq [string]$report.build.binary_sha256) 'binary SHA-256 mismatch'
}
elseif (-not [string]::IsNullOrWhiteSpace($ExpectedBinarySha256)) {
    Require-Hash $ExpectedBinarySha256 'expected binary'
    Require ([string]$report.build.binary_sha256 -ieq $ExpectedBinarySha256) 'binary SHA-256 mismatch'
}
else {
    throw 'an executable or expected binary SHA-256 is required'
}
Require-Hash $report.bundle_path_hash 'bundle path'
if ($null -ne $report.bundle_path) {
    Require ($AllowAbsolutePath.IsPresent) 'absolute path evidence requires explicit opt-in'
    Require ([IO.Path]::IsPathRooted([string]$report.bundle_path)) 'opted-in bundle path is not absolute'
}
Require ($null -ne $report.image) 'passing report has no image identity'
foreach ($name in @('image_uuid','virtual_bytes','logical_sector_size','physical_sector_size')) {
    Require-Property $report.image $name
}
Require ([string]$report.image.image_uuid -match '^[0-9A-Fa-f]{32}$') 'image UUID is invalid'
Require-UnsignedInteger $report.image.virtual_bytes 'image virtual_bytes'
Require-UnsignedInteger $report.image.logical_sector_size 'image logical_sector_size'
Require-UnsignedInteger $report.image.physical_sector_size 'image physical_sector_size'
Require ([uint64]$report.image.virtual_bytes -gt 0) 'image virtual size is zero'
Require (@(512,4096) -contains [int]$report.image.logical_sector_size) 'logical sector size is invalid'
Require ([int]$report.image.physical_sector_size -eq 4096) 'physical sector size is invalid'

$slots = @($report.slots)
Require ($slots.Count -eq 2) 'exactly two slot diagnostics are required'
$slotStatuses = @('empty','selected','valid_not_selected','fallback_rejected','rejected')
$checkStatuses = @('not_run','passed','missing','corrupt','unsupported','access_denied','io_error','cancelled','deadline_exceeded','conflict')
$checks = @('slot_decode','slot_support','slot_semantics','base_path','journal_path','base_presence','base_open','base_identity','base_expected_length','base_profile','base_layout','journal_presence','journal_open','journal_identity','journal_header','journal_committed_length','journal_commit','journal_root','journal_digest')
for ($index = 0; $index -lt 2; ++$index) {
    $slot = $slots[$index]
    foreach ($name in @('slot','status','generation','first_failed_check','check_status','selected')) {
        Require-Property $slot $name
    }
    $expectedSlot = if ($index -eq 0) { 'A' } else { 'B' }
    Require ([string]$slot.slot -eq $expectedSlot) 'slot order/name is invalid'
    Require ($slotStatuses -contains [string]$slot.status) 'slot status is unknown'
    Require ($checkStatuses -contains [string]$slot.check_status) 'slot check status is unknown'
    Require-Boolean $slot.selected "slot selected"
    if ($null -ne $slot.generation) { Require-UnsignedInteger $slot.generation 'slot generation' }
    if ($null -ne $slot.first_failed_check) {
        Require ($checks -contains [string]$slot.first_failed_check) 'slot failed check is unknown'
    }
}
Require (@($slots | Where-Object { [bool]$_.selected }).Count -eq 1) 'passing report must select exactly one slot'

Require ($null -ne $report.selected) 'passing report has no selected generation'
foreach ($name in @('slot_index','fallback','checkpoint_generation','base_generation','journal_epoch','base_windows_identity','base_format_identity','journal_windows_identity','journal_format_identity','journal_committed_length','root','commit')) {
    Require-Property $report.selected $name
}
Require-UnsignedInteger $report.selected.slot_index 'selected slot index'
Require ([int]$report.selected.slot_index -in 0,1) 'selected slot index is invalid'
Require-Boolean $report.selected.fallback 'selected fallback'
foreach ($name in @('checkpoint_generation','base_generation','journal_epoch','journal_committed_length')) {
    Require-UnsignedInteger $report.selected.$name "selected $name"
}
foreach ($name in @('base_windows_identity','base_format_identity','journal_windows_identity','journal_format_identity')) {
    Require-NonEmptyString $report.selected.$name "selected $name"
}
Require ($null -ne $report.selected.root) 'selected root is null'
Require ($null -ne $report.selected.commit) 'selected commit is null'
foreach ($name in @('offset','length','crc32c')) {
    Require-Property $report.selected.root $name
    Require-UnsignedInteger $report.selected.root.$name "root $name"
    Require-Property $report.selected.commit $name
    Require-UnsignedInteger $report.selected.commit.$name "commit $name"
}
Require-Property $report.selected.commit 'chain_sha256'
Require-Hash $report.selected.commit.chain_sha256 'commit chain'

if ([string]$report.operation -eq 'inspect') {
    Require ($null -eq $report.verification) 'inspect report cannot claim verification'
}
else {
    Require ($null -ne $report.verification) 'verify report has no verification section'
    foreach ($name in @('depth','verified','candidate_blocks','data_blocks','zero_blocks','data_bytes_hashed','logical_sha256')) {
        Require-Property $report.verification $name
    }
    Require (@('structural','full') -contains [string]$report.verification.depth) 'verification depth is unknown'
    $expectedDepth = if ([string]$report.operation -eq 'verify_full') { 'full' } else { 'structural' }
    Require ([string]$report.verification.depth -eq $expectedDepth) 'operation and verification depth differ'
    Require-Boolean $report.verification.verified 'verification verified'
    Require ([bool]$report.verification.verified) 'required verification did not pass'
    foreach ($name in @('candidate_blocks','data_blocks','zero_blocks','data_bytes_hashed')) {
        Require-UnsignedInteger $report.verification.$name "verification $name"
    }
    Require ([uint64]$report.verification.candidate_blocks -eq
        ([uint64]$report.verification.data_blocks + [uint64]$report.verification.zero_blocks)) 'verification block counts are inconsistent'
    if ($expectedDepth -eq 'full') {
        Require (-not [bool]$report.selected.fallback) 'full verification cannot use fallback generation'
        Require-Hash $report.verification.logical_sha256 'logical'
    }
}

$cleanupDecisions = @('Eligible','InvalidName','ReparsePoint','HardLinked','UnknownOwnership','ManifestReferenced','LiveViewReferenced','InFlightReferenced')
foreach ($candidate in @($report.cleanup_candidates)) {
    foreach ($name in @('kind','relative_name','decision','action')) { Require-Property $candidate $name }
    Require (@('base','journal') -contains [string]$candidate.kind) 'cleanup kind is unknown'
    Require ($cleanupDecisions -contains [string]$candidate.decision) 'cleanup decision is unknown'
    Require (@('Kept','Deleted') -contains [string]$candidate.action) 'cleanup action is unknown'
    $relativeName = [string]$candidate.relative_name
    Require (-not [IO.Path]::IsPathRooted($relativeName)) 'cleanup candidate leaked an absolute path'
    Require ([IO.Path]::GetFileName($relativeName) -eq $relativeName) 'cleanup candidate is not a single component'
}

$artifacts = @($report.artifacts)
Require ($artifacts.Count -gt 0) 'artifact hashes are required'
$roles = @{}
foreach ($artifact in $artifacts) {
    foreach ($name in @('role','sha256')) { Require-Property $artifact $name }
    Require (-not [string]::IsNullOrWhiteSpace([string]$artifact.role)) 'artifact role is empty'
    Require (-not $roles.ContainsKey([string]$artifact.role)) 'artifact role is duplicated'
    $roles[[string]$artifact.role] = $true
    Require-Hash $artifact.sha256 'artifact'
}
Require ($null -eq $report.error) 'passing report contains a primary error'

Write-Output "M3 JSON validation PASS: $fullPath"
exit 0
