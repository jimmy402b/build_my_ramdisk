[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Verifier,
    [Parameter(Mandatory = $true)][string]$FixtureExecutable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Write-Report($Report, [string]$Path) {
    $Report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Invoke-Verify([string]$Path) {
    return @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Verifier `
        -Path $Path -ExpectedSourceCommit '0123456789abcdef0123456789abcdef01234567' `
        -ExpectedBinarySha256 ('A' * 64) 2>&1)
}

function Expect-Failure($Report, [string]$Name, [string]$Expected) {
    $path = Join-Path $script:root ($Name + '.json')
    Write-Report $Report $path
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(Invoke-Verify $path)
        $code = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $saved }
    Require ($code -ne 0) "invalid fixture passed: $Name"
    Require (($output -join [Environment]::NewLine) -match [regex]::Escape($Expected)) `
        "unexpected rejection for $Name`: $($output -join [Environment]::NewLine)"
}

$script:root = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m3-json-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($script:root) | Out-Null
try {
    $hashA = 'A' * 64
    $hashB = 'B' * 64
    $valid = [ordered]@{
        schema_version = 1
        operation = 'verify_full'
        status = 'passed'
        operation_id = 'op-1'
        started_utc = '2026-08-06T00:00:00.000Z'
        finished_utc = '2026-08-06T00:00:01.000Z'
        build = [ordered]@{
            source_commit = '0123456789abcdef0123456789abcdef01234567'
            build_type = 'Debug'
            binary_sha256 = $hashA
        }
        bundle_path_hash = $hashB
        bundle_path = $null
        image = [ordered]@{
            image_uuid = '00112233445566778899AABBCCDDEEFF'
            virtual_bytes = 67108864
            logical_sector_size = 512
            physical_sector_size = 4096
        }
        slots = @(
            [ordered]@{ slot='A'; status='selected'; generation=7; first_failed_check=$null; check_status='passed'; selected=$true },
            [ordered]@{ slot='B'; status='empty'; generation=$null; first_failed_check='slot_decode'; check_status='missing'; selected=$false }
        )
        selected = [ordered]@{ slot_index=0; fallback=$false; checkpoint_generation=7; base_generation=3; journal_epoch=4; base_windows_identity='bwi'; base_format_identity='bfi'; journal_windows_identity='jwi'; journal_format_identity='jfi'; journal_committed_length=12288; root=[ordered]@{offset=4096;length=4096;crc32c=1}; commit=[ordered]@{offset=8192;length=4096;crc32c=2;chain_sha256=$hashA} }
        verification = [ordered]@{ depth='full'; verified=$true; candidate_blocks=0; data_blocks=0; zero_blocks=0; data_bytes_hashed=0; logical_sha256=$hashB }
        cleanup_candidates = @()
        artifacts = @([ordered]@{role='manifest';sha256=$hashA})
        error = $null
    }
    $validPath = Join-Path $script:root 'valid.json'
    Write-Report $valid $validPath
    $output = @(Invoke-Verify $validPath)
    Require ($LASTEXITCODE -eq 0) "valid fixture failed: $($output -join [Environment]::NewLine)"
    Require (($output -join [Environment]::NewLine) -match 'M3 JSON validation PASS') 'PASS marker missing'

    $nativePath = Join-Path $script:root 'native-valid.json'
    & $FixtureExecutable $nativePath
    Require ($LASTEXITCODE -eq 0) 'native serializer/publisher fixture failed'
    $nativeOutput = @(Invoke-Verify $nativePath)
    Require ($LASTEXITCODE -eq 0) "native JSON failed verifier: $($nativeOutput -join [Environment]::NewLine)"

    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.schema_version = 2
    Expect-Failure $copy 'schema' 'schema_version must be 1'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.operation = 'unknown'
    Expect-Failure $copy 'operation' 'operation is unknown'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.build.source_commit = '0' * 40
    Expect-Failure $copy 'source' 'source commit mismatch'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.build.binary_sha256 = 'short'
    Expect-Failure $copy 'binary-hash' 'binary SHA-256 is invalid'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.bundle_path = 'C:\\secret\\disk.myrd'
    Expect-Failure $copy 'path-opt-in' 'absolute path evidence requires explicit opt-in'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.slots[1].check_status = 'SKIP'
    Expect-Failure $copy 'skip' 'slot check status is unknown'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.verification.depth = 'mystery'
    Expect-Failure $copy 'depth' 'verification depth is unknown'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.verification.verified = 'false'
    Expect-Failure $copy 'verified-type' 'verification verified must be boolean'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.selected.root = $null
    Expect-Failure $copy 'null-root' 'selected root is null'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.selected.base_windows_identity = ''
    Expect-Failure $copy 'empty-identity' 'selected base_windows_identity is empty'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.selected.commit.PSObject.Properties.Remove('length')
    Expect-Failure $copy 'missing-descriptor' 'required property missing: length'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.selected.fallback = $true
    Expect-Failure $copy 'full-fallback' 'full verification cannot use fallback generation'
    $copy = $valid | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $copy.artifacts = @()
    Expect-Failure $copy 'artifact' 'artifact hashes are required'

    $invalidUtf8Path = Join-Path $script:root 'invalid-utf8.json'
    [IO.File]::WriteAllBytes($invalidUtf8Path, [byte[]](0x7B,0x22,0x78,0x22,0x3A,0x22,0xC3,0x28,0x22,0x7D))
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $invalidOutput = @(Invoke-Verify $invalidUtf8Path)
        $invalidCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $saved }
    Require ($invalidCode -ne 0) 'invalid UTF-8 fixture passed'
}
finally {
    if ([IO.Directory]::Exists($script:root)) {
        Remove-Item -LiteralPath $script:root -Recurse -Force
    }
}

Write-Output 'M3 JSON verifier tests PASS'
