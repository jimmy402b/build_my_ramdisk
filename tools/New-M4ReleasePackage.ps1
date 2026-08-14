[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9A-Fa-f]{40}$')][string]$SourceCommit,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ToolchainProfile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$stageCreated = $false
$zipCreated = $false
$stage = $null
$zip = $null
$temporaryZip = $null
try {
    $repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    if (-not [IO.File]::Exists($Artifact)) { throw "Artifact does not exist: $Artifact" }
    $artifactPath = (Resolve-Path -LiteralPath $Artifact -ErrorAction Stop).Path
    if ([IO.Path]::GetFileName($artifactPath) -cne 'myrd.exe') { throw 'Artifact basename must be exactly myrd.exe' }
    $readme = Join-Path $repo 'README.md'
    $license = Join-Path $repo 'LICENSE'
    $notes = Join-Path $repo 'RELEASE_NOTES.md'
    foreach ($required in @($readme, $license, $notes)) {
        if (-not [IO.File]::Exists($required)) { throw "Required release file does not exist: $required" }
    }

    $out = [IO.Path]::GetFullPath($OutputDirectory)
    $stage = Join-Path $out 'myrd-v1.0.0-windows-x64'
    $zip = Join-Path $out 'myrd-v1.0.0-windows-x64.zip'
    if ([IO.Directory]::Exists($stage)) { throw "Release stage already exists: $stage" }
    if ([IO.File]::Exists($zip)) { throw "Release ZIP already exists: $zip" }

    [IO.Directory]::CreateDirectory($out) | Out-Null
    New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
    $stageCreated = $true
    Copy-Item -LiteralPath $artifactPath -Destination (Join-Path $stage 'myrd.exe')
    Copy-Item -LiteralPath $readme -Destination (Join-Path $stage 'README.md')
    Copy-Item -LiteralPath $license -Destination (Join-Path $stage 'LICENSE')
    Copy-Item -LiteralPath $notes -Destination (Join-Path $stage 'RELEASE_NOTES.md')

    $utf8 = [Text.UTF8Encoding]::new($false)
    $newline = [Environment]::NewLine
    $hash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToUpperInvariant()
    $buildInfo = @(
        'schema=myrd.release.buildinfo.v1'
        'version=1.0.0'
        ('source_commit=' + $SourceCommit.ToLowerInvariant())
        ('artifact_sha256=' + $hash)
        ('toolchain=' + $ToolchainProfile)
    ) -join $newline
    [IO.File]::WriteAllText((Join-Path $stage 'BUILDINFO.txt'), $buildInfo + $newline, $utf8)

    $payloadNames = @('BUILDINFO.txt', 'LICENSE', 'myrd.exe', 'README.md', 'RELEASE_NOTES.md') | Sort-Object
    $sumLines = foreach ($name in $payloadNames) {
        '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $stage $name) -Algorithm SHA256).Hash.ToUpperInvariant(), $name
    }
    [IO.File]::WriteAllText((Join-Path $stage 'SHA256SUMS'), (($sumLines -join $newline) + $newline), $utf8)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $temporaryZip = $zip + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporaryZip)
    [IO.File]::Move($temporaryZip, $zip)
    $temporaryZip = $null
    $zipCreated = $true
    Write-Output ([IO.Path]::GetFullPath($zip))
}
catch {
    if ($temporaryZip -and [IO.File]::Exists($temporaryZip)) { Remove-Item -LiteralPath $temporaryZip -Force -ErrorAction SilentlyContinue }
    if ($zipCreated -and $zip -and [IO.File]::Exists($zip)) { Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue }
    if ($stageCreated -and $stage -and [IO.Directory]::Exists($stage)) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
    throw
}
