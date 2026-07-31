# Fetch-AimReference.ps1
# Non-destructive AIM protocol reference fetcher.
param(
    [string]$Destination = '.\out\reference\aim'
)

$ExpectedCommit = 'd379caa027d21902e45fce8c9daf02fd116c2dda'
$Repository = 'https://github.com/ArsenalRecon/Arsenal-Image-Mounter.git'

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Destination)) {
    Write-Output "Cloning AIM reference into $Destination ..."
    git clone --no-checkout $Repository $Destination 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}

Push-Location $Destination

try {
    if (-not (Test-Path '.git')) {
        throw "Destination $Destination is not a git repository"
    }

    Write-Output "Fetching tags and commits..."
    git fetch --tags origin 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }

    Write-Output "Checking out exact commit $ExpectedCommit ..."
    git checkout --detach $ExpectedCommit 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed" }

    $actualCommit = git rev-parse HEAD
    Write-Output "Actual HEAD: $actualCommit"
    if ($actualCommit -ne $ExpectedCommit) {
        throw "Commit mismatch: expected $ExpectedCommit, got $actualCommit"
    }

    $normativeFiles = @(
        'Unmanaged Source/phdskmnt/inc/common.h',
        'Unmanaged Source/phdskmnt/inc/imdproxy.h',
        'Unmanaged Source/phdskmnt/proxy.cpp',
        'Managed Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs'
    )

    foreach ($file in $normativeFiles) {
        $fullPath = Join-Path (Get-Location) $file
        if (-not (Test-Path -LiteralPath $fullPath)) {
            throw "Normative source file missing: $file"
        }
        $hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
        Write-Output "  SHA256 $hash  $file"
    }

    Write-Output "AIM reference verified at commit $ExpectedCommit"
}
finally {
    Pop-Location
}
