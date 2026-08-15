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
$requiredFunctions = @('Get-GuestFacts', 'Assert-InitialGuestFacts', 'Initialize-QualificationRoot', 'ConvertTo-WindowsCommandLine', 'Invoke-BoundedProcess', 'ConvertFrom-AimQueryJson', 'Assert-UnexpectedReset')
foreach ($name in $requiredFunctions) {
    $matches = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true))
    Need ($matches.Count -eq 1) "guest qualification controller is missing $name"
}
$guestFacts = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-GuestFacts' }, $true))[0]
Need ((@($guestFacts.Body.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath }) -join '|') -ceq 'QualificationRoot|Kit') 'Get-GuestFacts parameter contract is invalid'
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
