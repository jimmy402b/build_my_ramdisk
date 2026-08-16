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

$root = [IO.Path]::GetFullPath($SourceRoot)
$controller = Join-Path $root 'tools\Invoke-M4GuestQualification.ps1'
if (-not [IO.File]::Exists($controller)) {
    throw 'guest qualification controller is missing'
}

$tokens = $null
$parseErrors = $null
$controllerAst = [Management.Automation.Language.Parser]::ParseFile(
    $controller,
    [ref]$tokens,
    [ref]$parseErrors)
Need ($parseErrors.Count -eq 0) 'guest qualification controller failed AST parsing'
$requiredFunctions = @('Get-GuestFacts', 'Assert-InitialGuestFacts', 'Publish-QualificationRootStage', 'Initialize-QualificationRoot', 'ConvertTo-WindowsCommandLine', 'Invoke-BoundedProcess', 'ConvertFrom-AimQueryJson', 'Select-RelevantGuestProcesses', 'Assert-UnexpectedReset', 'Invoke-FormalRunnerCapture', 'Invoke-FormalCreateSequence', 'Invoke-FormalRecoverySequence', 'Invoke-LivePowerCutArm', 'Assert-LivePowerCutRecoveryEvidence', 'Invoke-LivePowerCutRecovery', 'Invoke-QualificationRun')
foreach ($name in $requiredFunctions) {
    $matches = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true))
    Need ($matches.Count -eq 1) "guest qualification controller is missing $name"
}
$guestFacts = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-GuestFacts' }, $true))[0]
Need ((@($guestFacts.Body.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath }) -join '|') -ceq 'QualificationRoot|Kit') 'Get-GuestFacts parameter contract is invalid'
$controllerText = [IO.File]::ReadAllText($controller)
foreach ($requiredText in @("Name='MyRamDiskService'", "Name='phdskmnt'", 'Get-Disk', 'Win32_Process', 'CommandLine')) { Need ($controllerText.Contains($requiredText)) "Get-GuestFacts source contract is missing $requiredText" }
foreach ($forbiddenText in @('Win32_DiskDrive', "Name='myrd'")) { Need (-not $controllerText.Contains($forbiddenText)) "Get-GuestFacts source contract contains $forbiddenText" }
Need ($controllerText.Contains('[IO.Directory]::Move($Stage, $Root)')) 'qualification root publish must atomically move the stage'
Need ($controllerText.Contains('.stage-')) 'qualification root initialization must use a private stage name'
Need ($controllerText.Contains('$script:ProcessInvoker')) 'guest qualification controller must inject ProcessInvoker'
Need ($controllerText.Contains('RESET STILL REQUIRED: CrashReset')) 'guest qualification controller must require the VMware reset'
Need ($controllerText.Contains('CRASH RESET ARMED')) 'guest qualification controller must arm crash reset explicitly'
Need ($controllerText.Contains("'schema', 'qualification_id', 'state', 'sequence', 'events'")) 'qualification state schema must include events'
Need ($controllerText.Contains('$script:QualificationObserver')) 'guest qualification controller must expose durable qualification hooks'
Need ($controllerText.Contains('Assert-CreateKillEvidence')) 'guest qualification controller must validate real CreateKill evidence'
Need ($controllerText.Contains('Get-SystemResetEvents')) 'guest qualification controller must sample reset events independently'
Need ($controllerText.Contains('$script:LivePowerCutInvoker')) 'guest qualification controller must inject LivePowerCutInvoker'
Need ($controllerText.Contains('RESET STILL REQUIRED: LivePowerCut')) 'guest qualification controller must require the second VMware reset'
Need ($controllerText.Contains('LIVE POWER-CUT ARMED')) 'guest qualification controller must arm live power cut explicitly'
$tempCleanupFinally = @($controllerAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.TryStatementAst] -and
        $null -ne $node.Finally -and
        $node.Finally.Extent.Text -match '\[IO\.File\]::Delete\(\$tempPath\)'
}, $true))
Need ($tempCleanupFinally.Count -eq 1) 'qualification temp must be deleted in exactly one finally block'

$controllerOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $controller -SelfTest)
if ($LASTEXITCODE -ne 0) {
    throw "guest qualification controller selftest failed with exit code $LASTEXITCODE"
}
Need ($controllerOutput.Count -eq 1) 'guest qualification controller selftest must emit exactly one line'
Need ($controllerOutput[0] -ceq 'M4 guest qualification SelfTest PASS') 'guest qualification controller selftest output is not exact'

Write-Output 'M4 guest qualification contract PASS'
