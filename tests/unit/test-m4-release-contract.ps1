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

function Read-StrictUtf8WithoutBom {
    param([Parameter(Mandatory = $true)][string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Need (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) "UTF-8 BOM is forbidden in $Path"
    try {
        return [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    }
    catch [Text.DecoderFallbackException] {
        throw "invalid UTF-8 in $Path"
    }
}

$root = [IO.Path]::GetFullPath($SourceRoot)
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw -Encoding UTF8
Need ($cmake -match '(?m)^project\(MyRamDiskM0 VERSION 1\.0\.0 LANGUAGES CXX\)\s*$') 'root CMake project version must be exactly 1.0.0'

$resource = Get-Content -LiteralPath (Join-Path $root 'src\run\run_resources.rc') -Raw -Encoding UTF8
Need ($resource -match '(?m)^\s*FILEVERSION\s+1,0,0,0\s*$') 'resource FILEVERSION must be 1,0,0,0'
Need ($resource -match '(?m)^\s*PRODUCTVERSION\s+1,0,0,0\s*$') 'resource PRODUCTVERSION must be 1,0,0,0'
Need ($resource -match 'VALUE\s+"FileVersion",\s*"1\.0\.0"') 'resource FileVersion must be 1.0.0'
Need ($resource -match 'VALUE\s+"ProductVersion",\s*"1\.0\.0"') 'resource ProductVersion must be 1.0.0'
Need ($resource -match 'VALUE\s+"ProductName",\s*"MyRamDisk"') 'resource ProductName must be MyRamDisk'
Need ($resource -match '(?m)^\s*1\s+RCDATA\s+"provision_volume\.ps1"\s*$') 'resource must retain exactly the existing 1 RCDATA provision_volume.ps1'

$readme = Get-Content -LiteralPath (Join-Path $root 'README.md') -Raw -Encoding UTF8
$headings = @(
    (([string][char]0x652F) + [char]0x6301 + [char]0x8303 + [char]0x56F4),
    (([string][char]0x5FEB) + [char]0x901F + [char]0x5F00 + [char]0x59CB),
    (([string][char]0x521B) + [char]0x5EFA + [char]0x4E0E + [char]0x6062 + [char]0x590D),
    ('Checkpoint ' + [char]0x4E0E + ' journal'),
    (([string][char]0x79BB) + [char]0x7EBF + [char]0x538B + [char]0x7F29),
    (([string][char]0x5B89) + [char]0x5168 + [char]0x5173 + [char]0x95ED + [char]0x4E0E + [char]0x5F3A + [char]0x5236 + [char]0x7EC8 + [char]0x6B62),
    (([string][char]0x9000) + [char]0x51FA + [char]0x7801),
    (([string][char]0x5DF2) + [char]0x77E5 + [char]0x9650 + [char]0x5236)
)
foreach ($heading in $headings) {
    Need ($readme -match ('(?m)^##\s*' + [regex]::Escape($heading) + '\s*$')) "README is missing heading $heading"
}
foreach ($command in @('myrd.exe run --bundle', 'myrd.exe compact --bundle')) {
    Need ($readme -match [regex]::Escape($command)) "README is missing command $command"
}
$localNtfsRequirement = 'bundle ' + [string]::new([char[]]@(0x76EE,0x5F55,0x5FC5,0x987B,0x4F4D,0x4E8E,0x672C,0x673A)) + ' NTFS ' + [char]0x5377
$freeLetterRequirement = [string]::new([char[]]@(0x521B,0x5EFA,0x65F6,0x6307,0x5B9A,0x7684,0x76D8,0x7B26,0x5FC5,0x987B,0x5F53,0x524D,0x7A7A,0x95F2))
foreach ($requirement in @($localNtfsRequirement, $freeLetterRequirement)) {
    Need ($readme.Contains($requirement)) "README is missing support requirement: $requirement"
}

$notes = Join-Path $root 'RELEASE_NOTES.md'
$packager = Join-Path $root 'tools\New-M4ReleasePackage.ps1'
Need ([IO.File]::Exists($notes)) 'missing RELEASE_NOTES.md'
Need ([IO.File]::Exists($packager)) 'missing M4 release packager'
$notesText = Get-Content -LiteralPath $notes -Raw -Encoding UTF8
foreach ($requirement in @($localNtfsRequirement, $freeLetterRequirement)) {
    Need ($notesText.Contains($requirement)) "RELEASE_NOTES is missing supported-profile requirement: $requirement"
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-release-contract-' + [Guid]::NewGuid().ToString('N'))
Need (-not [IO.Directory]::Exists($scratch)) 'release-contract scratch root unexpectedly exists'
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $release = Join-Path $scratch 'Release'
    [IO.Directory]::CreateDirectory($release) | Out-Null
    $fake = Join-Path $release 'myrd.exe'
    [IO.File]::WriteAllBytes($fake, [byte[]](1,2,3,4,5))
    $out = Join-Path $scratch 'package'
    $commit = '0123456789abcdef0123456789abcdef01234567'
    $zipOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packager `
        -Artifact $fake -OutputDirectory $out -SourceCommit $commit -ToolchainProfile 'test-toolchain')
    Need ($LASTEXITCODE -eq 0) 'release packager failed for its first invocation'
    Need ($zipOutput.Count -eq 1) 'release packager must emit exactly one ZIP path'
    $zipPath = [IO.Path]::GetFullPath([string]$zipOutput[0])
    Need ([IO.File]::Exists($zipPath)) 'release packager ZIP path does not exist'
    Need ([IO.Path]::GetDirectoryName($zipPath) -eq [IO.Path]::GetFullPath($out)) 'release packager ZIP is outside the supplied output directory'
    Need ([IO.Path]::GetFileName($zipPath) -eq 'myrd-v1.0.0-windows-x64.zip') 'release packager ZIP basename is invalid'
    Need ([IO.Path]::GetExtension($zipPath) -eq '.zip') 'release packager output is not a ZIP path'

    $expanded = Join-Path $scratch 'expanded'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $expanded)
    $directories = @(Get-ChildItem -LiteralPath $expanded -Recurse -Directory)
    Need ($directories.Count -eq 0) 'release ZIP must not contain directories'
    $names = @(Get-ChildItem -LiteralPath $expanded -Recurse -File | ForEach-Object {
        $_.FullName.Substring($expanded.Length).TrimStart([IO.Path]::DirectorySeparatorChar)
    } | Sort-Object)
    Need (($names -join ',') -eq 'BUILDINFO.txt,LICENSE,myrd.exe,README.md,RELEASE_NOTES.md,SHA256SUMS') 'release ZIP has an unexpected file set'
    $sumsText = Read-StrictUtf8WithoutBom -Path (Join-Path $expanded 'SHA256SUMS')
    $sums = @($sumsText -split "`r?`n")
    if ($sums.Count -gt 0 -and $sums[-1] -eq '') { $sums = @($sums[0..($sums.Count - 2)]) }
    Need ($sums.Count -eq 5) 'SHA256SUMS must have five lines'
    $payloadNames = @('BUILDINFO.txt', 'LICENSE', 'myrd.exe', 'README.md', 'RELEASE_NOTES.md')
    $checksumPattern = '^([A-F0-9]{64})  ([^\r\n]+)$'
    foreach ($name in $payloadNames) {
        $matches = @($sums | Where-Object {
            $_ -match $checksumPattern -and $Matches[2] -eq $name
        })
        Need ($matches.Count -eq 1) "SHA256SUMS must contain exactly one valid line for $name"
        $expectedHash = (Get-FileHash -LiteralPath (Join-Path $expanded $name) -Algorithm SHA256).Hash.ToUpperInvariant()
        Need ($matches[0].Substring(0, 64) -eq $expectedHash) "SHA256SUMS hash is incorrect for $name"
    }
    $expectedSumLines = @($payloadNames | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $expanded $_) -Algorithm SHA256).Hash.ToUpperInvariant(), $_
    })
    Need (($sums -join "`n") -eq ($expectedSumLines -join "`n")) 'SHA256SUMS lines are not in the frozen sorted order'
    $buildInfo = Read-StrictUtf8WithoutBom -Path (Join-Path $expanded 'BUILDINFO.txt')
    Need ($buildInfo -match '(?m)^schema=myrd\.release\.buildinfo\.v1\r?$') 'BUILDINFO schema is invalid'
    Need ($buildInfo -match '(?m)^version=1\.0\.0\r?$') 'BUILDINFO version is invalid'
    Need ($buildInfo -match ('(?m)^source_commit=' + $commit + '\r?$')) 'BUILDINFO source commit is invalid'
    Need ($buildInfo -match '(?m)^toolchain=test-toolchain\r?$') 'BUILDINFO toolchain is invalid'
    $fakeHash = (Get-FileHash -LiteralPath $fake -Algorithm SHA256).Hash.ToUpperInvariant()
    Need ($buildInfo -match ('(?m)^artifact_sha256=' + $fakeHash + '\r?$')) 'BUILDINFO artifact hash is invalid'

    $stage = Join-Path $out 'myrd-v1.0.0-windows-x64'
    if ([IO.Directory]::Exists($stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
    Need (-not [IO.Directory]::Exists($stage)) 'ZIP-only collision probe stage is not absent'
    $priorErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $zipOnlyOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packager `
            -Artifact $fake -OutputDirectory $out -SourceCommit $commit -ToolchainProfile 'test-toolchain' 2>&1)
    }
    finally { $ErrorActionPreference = $priorErrorAction }
    Need ($LASTEXITCODE -ne 0) 'release packager overwrote an existing ZIP'

    Remove-Item -LiteralPath $zipPath -Force
    Need (-not [IO.File]::Exists($zipPath)) 'stage-only collision probe ZIP is not absent'
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $priorErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $stageOnlyOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packager `
            -Artifact $fake -OutputDirectory $out -SourceCommit $commit -ToolchainProfile 'test-toolchain' 2>&1)
    }
    finally { $ErrorActionPreference = $priorErrorAction }
    Need ($LASTEXITCODE -ne 0) 'release packager overwrote an existing stage'
}
finally {
    if ([IO.Directory]::Exists($scratch)) { Remove-Item -LiteralPath $scratch -Recurse -Force }
}
Write-Output 'M4 release contract PASS'
