[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$Query,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Need {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

$stage = $null
$stageCreated = $false
try {
    $root = [IO.Path]::GetFullPath($SourceRoot)
    $destinationDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    $parent = [IO.Path]::GetDirectoryName($destinationDirectory)
    Need ([IO.Directory]::Exists($root)) "SourceRoot does not exist: $root"
    Need (-not [IO.Directory]::Exists($destinationDirectory) -and -not [IO.File]::Exists($destinationDirectory)) "OutputDirectory already exists: $destinationDirectory"
    Need ([IO.Directory]::Exists($parent)) "OutputDirectory parent does not exist: $parent"

    $files = @(
        [pscustomobject]@{ Source = (Join-Path $root 'tests\e2e\Capture-M4Child.ps1'); Name = 'Capture-M4Child.ps1' }
        [pscustomobject]@{ Source = (Join-Path $root 'tools\Invoke-M4GuestQualification.ps1'); Name = 'Invoke-M4GuestQualification.ps1' }
        [pscustomobject]@{ Source = (Join-Path $root 'tests\e2e\Invoke-M4RunLifecycle.ps1'); Name = 'Invoke-M4RunLifecycle.ps1' }
        [pscustomobject]@{ Source = (Join-Path $root 'tools\Invoke-M4Verification.ps1'); Name = 'Invoke-M4Verification.ps1' }
        [pscustomobject]@{ Source = (Join-Path $root 'tests\e2e\Prepare-M4NegativeFixtures.ps1'); Name = 'Prepare-M4NegativeFixtures.ps1' }
        [pscustomobject]@{ Source = [IO.Path]::GetFullPath($Artifact); Name = 'myrd.exe' }
        [pscustomobject]@{ Source = [IO.Path]::GetFullPath($Query); Name = 'myrd_m4_aim_query.exe' }
    )
    foreach ($file in $files) { Need ([IO.File]::Exists($file.Source)) "Required kit file does not exist: $($file.Source)" }
    Need (([IO.Path]::GetFileName($files[5].Source) -ceq 'myrd.exe')) 'Artifact basename must be exactly myrd.exe'
    Need (([IO.Path]::GetFileName($files[6].Source) -ceq 'myrd_m4_aim_query.exe')) 'Query basename must be exactly myrd_m4_aim_query.exe'

    $name = [IO.Path]::GetFileName($destinationDirectory)
    $injected = [Environment]::GetEnvironmentVariable('MYRD_M4_QUALIFICATION_STAGE_NAME')
    if ([string]::IsNullOrEmpty($injected)) {
        $stageName = '.' + $name + '.stage-' + [Guid]::NewGuid().ToString('N')
    }
    else {
        Need ($injected -cmatch ('^\.' + [Regex]::Escape($name) + '\.stage-[A-Za-z0-9-]+$')) 'Injected qualification stage name is invalid'
        $stageName = $injected
    }
    $stage = Join-Path $parent $stageName
    Need (-not [IO.Directory]::Exists($stage) -and -not [IO.File]::Exists($stage)) "Qualification stage already exists: $stage"
    New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
    $stageCreated = $true

    foreach ($file in $files) { [IO.File]::Copy($file.Source, (Join-Path $stage $file.Name), $false) }
    $commit = (& git -C $root rev-parse HEAD).Trim().ToLowerInvariant()
    Need ($commit -cmatch '^[0-9a-f]{40}$') 'SourceRoot git commit is invalid'
    $manifestFiles = @($files | ForEach-Object {
        $destination = Join-Path $stage $_.Name
        [ordered]@{
            name = $_.Name
            length = [int64]([IO.FileInfo]$destination).Length
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToUpperInvariant()
        }
    })
    $manifest = [ordered]@{
        schema = 'myrd.m4.qualification-kit.v1'
        source_commit = $commit
        files = $manifestFiles
    }
    $manifestPath = Join-Path $stage 'qualification-kit.json'
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    [IO.Directory]::Move($stage, $destinationDirectory)
    $stageCreated = $false
    Write-Output $destinationDirectory
}
finally {
    if ($stageCreated -and $stage -and [IO.Directory]::Exists($stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
