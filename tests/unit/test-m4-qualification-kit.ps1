[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$SourceRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Need {
    param([bool]$Condition, [Parameter(Mandatory = $true)][string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-Inventory {
    param([Parameter(Mandatory = $true)][string]$Path)
    return @(
        Get-ChildItem -LiteralPath $Path -Force -Recurse | ForEach-Object {
            if ($_.PSIsContainer) { "D:$($_.FullName.Substring($Path.Length))" }
            else { "F:$($_.FullName.Substring($Path.Length)):$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)" }
        } | Sort-Object)
}

function Join-OrdinalNames {
    param([Parameter(Mandatory = $true)][string[]]$Names)
    $copy = [string[]]$Names.Clone()
    [Array]::Sort($copy, [StringComparer]::Ordinal)
    return [string]::Join('|', $copy)
}

function Invoke-Packager {
    param([string]$Root, [string]$Artifact, [string]$Query, [string]$OutputPath)
    $scriptPath = Join-Path $SourceRoot 'tools\New-M4GuestQualificationKit.ps1'
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $outputLines = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $scriptPath -SourceRoot $Root -Artifact $Artifact -Query $Query -OutputDirectory $OutputPath 2>&1 | ForEach-Object { [string]$_ })
    $ErrorActionPreference = $savedPreference
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $outputLines }
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('myrd-m4-qualification-kit-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($scratch) | Out-Null
try {
    $fakeRoot = Join-Path $scratch 'source'
    [IO.Directory]::CreateDirectory((Join-Path $fakeRoot 'tools')) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $fakeRoot 'tests\e2e')) | Out-Null
    $inputs = [ordered]@{
        'tests\e2e\Capture-M4Child.ps1' = 'capture'
        'tools\Invoke-M4GuestQualification.ps1' = 'controller'
        'tests\e2e\Invoke-M4RunLifecycle.ps1' = 'lifecycle'
        'tools\Invoke-M4Verification.ps1' = 'verification'
        'tests\e2e\Prepare-M4NegativeFixtures.ps1' = 'fixtures'
    }
    foreach ($entry in $inputs.GetEnumerator()) {
        [IO.File]::WriteAllText((Join-Path $fakeRoot $entry.Key), $entry.Value, [Text.UTF8Encoding]::new($false))
    }
    $artifact = Join-Path $scratch 'myrd.exe'
    $query = Join-Path $scratch 'myrd_m4_aim_query.exe'
    [IO.File]::WriteAllText($artifact, 'artifact', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($query, 'query', [Text.UTF8Encoding]::new($false))
    & git -C $fakeRoot init -q
    & git -C $fakeRoot config user.email 'qualification@example.invalid'
    & git -C $fakeRoot config user.name 'Qualification Test'
    & git -C $fakeRoot add .
    & git -C $fakeRoot commit -qm 'fixture'
    $kit = Join-Path $scratch 'kit'

    # RED: this first invocation must fail until the packager exists.
    $result = Invoke-Packager -Root $fakeRoot -Artifact $artifact -Query $query -OutputPath $kit
    Need ($result.ExitCode -eq 0) "qualification kit packager failed: $($result.Output -join '|')"

    $expectedNames = @(
        'Capture-M4Child.ps1', 'Invoke-M4GuestQualification.ps1',
        'Invoke-M4RunLifecycle.ps1', 'Invoke-M4Verification.ps1',
        'Prepare-M4NegativeFixtures.ps1', 'myrd.exe', 'myrd_m4_aim_query.exe',
        'qualification-kit.json')
    $actualItems = @(Get-ChildItem -LiteralPath $kit -Force)
    Need ($actualItems.Count -eq 8) 'kit root must contain exactly eight files'
    Need (-not (@($actualItems | Where-Object { $_.PSIsContainer }).Count)) 'kit must not contain a subdirectory'
    Need ((Join-OrdinalNames -Names ([string[]]$actualItems.Name)) -ceq (Join-OrdinalNames -Names $expectedNames)) 'kit root names are not exact'
    Need (@(Get-ChildItem -LiteralPath $kit -Force -Recurse -Directory).Count -eq 0) 'kit must not contain nested directories'

    $manifestPath = Join-Path $kit 'qualification-kit.json'
    $manifestBytes = [IO.File]::ReadAllBytes($manifestPath)
    Need ($manifestBytes.Length -ge 3 -and -not ($manifestBytes[0] -eq 0xEF -and $manifestBytes[1] -eq 0xBB -and $manifestBytes[2] -eq 0xBF)) 'manifest must be UTF-8 without BOM'
    $manifest = [Text.UTF8Encoding]::new($false, $true).GetString($manifestBytes) | ConvertFrom-Json
    Need ($manifest.schema -ceq 'myrd.m4.qualification-kit.v1') 'manifest schema is invalid'
    $commit = (& git -C $fakeRoot rev-parse HEAD).Trim().ToLowerInvariant()
    Need ($manifest.source_commit -ceq $commit) 'manifest source commit is invalid'
    Need ($manifest.files.Count -eq 7) 'manifest must contain exactly seven payload files'
    $payloadNames = @($expectedNames | Where-Object { $_ -cne 'qualification-kit.json' })
    Need (([string]::Join('|', @($manifest.files.name))) -ceq ([string]::Join('|', $payloadNames))) 'manifest file order is invalid'
    foreach ($entry in $manifest.files) {
        $file = Join-Path $kit $entry.name
        Need ([int64]$entry.length -eq ([IO.FileInfo]$file).Length) "manifest length is invalid for $($entry.name)"
        Need ($entry.sha256 -ceq (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToUpperInvariant()) "manifest hash is invalid for $($entry.name)"
    }

    $fakeGitDirectory = Join-Path $scratch 'fake-git'
    [IO.Directory]::CreateDirectory($fakeGitDirectory) | Out-Null
    [IO.File]::WriteAllText((Join-Path $fakeGitDirectory 'git.cmd'), "@echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa`r`n@exit /b 7`r`n", [Text.ASCIIEncoding]::new())
    $failedGitOutput = Join-Path $scratch 'failed-git-output'
    $originalPath = $env:PATH
    $env:PATH = $fakeGitDirectory + [IO.Path]::PathSeparator + $originalPath
    try { $failedGit = Invoke-Packager -Root $fakeRoot -Artifact $artifact -Query $query -OutputPath $failedGitOutput }
    finally { $env:PATH = $originalPath }
    Need ($failedGit.ExitCode -ne 0) 'nonzero git rev-parse exit must block packaging'
    Need (-not [IO.Directory]::Exists($failedGitOutput)) 'nonzero git rev-parse exit created output'
    Need (@(Get-ChildItem -LiteralPath $scratch -Force -Directory | Where-Object { $_.Name -like '.failed-git-output.stage-*' }).Count -eq 0) 'nonzero git rev-parse exit left a stage'

    $sentinelOutput = Join-Path $scratch 'existing-output'
    [IO.Directory]::CreateDirectory($sentinelOutput) | Out-Null
    [IO.File]::WriteAllText((Join-Path $sentinelOutput 'sentinel'), 'preserve')
    $beforeOutput = Get-Inventory -Path $sentinelOutput
    $collision = Invoke-Packager -Root $fakeRoot -Artifact $artifact -Query $query -OutputPath $sentinelOutput
    Need ($collision.ExitCode -ne 0) 'existing output directory must block packaging'
    Need (([string]::Join('|', (Get-Inventory -Path $sentinelOutput))) -ceq ([string]::Join('|', $beforeOutput))) 'existing output changed after collision'

    $ignoredOutput = Join-Path $scratch 'ignored-stage-output'
    $ignoredStageName = '.ignored-stage-output.stage-external'
    $ignoredStage = Join-Path $scratch $ignoredStageName
    [IO.Directory]::CreateDirectory($ignoredStage) | Out-Null
    [IO.File]::WriteAllText((Join-Path $ignoredStage 'sentinel'), 'preserve')
    $beforeIgnoredStage = Get-Inventory -Path $ignoredStage
    $env:MYRD_M4_QUALIFICATION_STAGE_NAME = $ignoredStageName
    Remove-Item Env:MYRD_M4_QUALIFICATION_TEST_MODE -ErrorAction SilentlyContinue
    try { $ignoredInjection = Invoke-Packager -Root $fakeRoot -Artifact $artifact -Query $query -OutputPath $ignoredOutput }
    finally { Remove-Item Env:MYRD_M4_QUALIFICATION_STAGE_NAME -ErrorAction SilentlyContinue }
    Need ($ignoredInjection.ExitCode -eq 0) 'ordinary packaging must ignore stage injection'
    Need ([IO.Directory]::Exists($ignoredOutput)) 'ordinary packaging did not create output'
    Need (([string]::Join('|', (Get-Inventory -Path $ignoredStage))) -ceq ([string]::Join('|', $beforeIgnoredStage))) 'ordinary stage injection changed the pre-existing stage'

    $stageOutput = Join-Path $scratch 'stage-output'
    $stageName = '.stage-output.stage-testcollision'
    $stage = Join-Path $scratch $stageName
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    [IO.File]::WriteAllText((Join-Path $stage 'sentinel'), 'preserve')
    $beforeStage = Get-Inventory -Path $stage
    $env:MYRD_M4_QUALIFICATION_STAGE_NAME = $stageName
    $env:MYRD_M4_QUALIFICATION_TEST_MODE = '1'
    try { $collision = Invoke-Packager -Root $fakeRoot -Artifact $artifact -Query $query -OutputPath $stageOutput }
    finally { Remove-Item Env:MYRD_M4_QUALIFICATION_STAGE_NAME,Env:MYRD_M4_QUALIFICATION_TEST_MODE -ErrorAction SilentlyContinue }
    Need ($collision.ExitCode -ne 0) 'stage collision must block packaging'
    Need (-not [IO.Directory]::Exists($stageOutput)) 'stage collision created output'
    Need (([string]::Join('|', (Get-Inventory -Path $stage))) -ceq ([string]::Join('|', $beforeStage))) 'pre-existing stage changed after collision'

    $controller = Join-Path $SourceRoot 'tools\Invoke-M4GuestQualification.ps1'
    $tokens = $null
    $parseErrors = $null
    $controllerAst = [Management.Automation.Language.Parser]::ParseFile($controller, [ref]$tokens, [ref]$parseErrors)
    Need ($parseErrors.Count -eq 0) 'controller must parse before validating qualification kits'
    Need (@($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-QualificationKit' }, $true)).Count -eq 1) 'controller must define Assert-QualificationKit'
    $controllerOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $controller -SelfTest)
    Need ($LASTEXITCODE -eq 0) 'controller qualification-kit validator selftest failed'
    Need ($controllerOutput -ccontains 'M4 guest qualification SelfTest PASS') 'controller validator selftest output is invalid'
    Write-Output 'M4 qualification kit PASS'
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
