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
$requiredFunctions = @('Get-GuestFacts', 'Get-GuestBootProbe', 'Assert-InitialGuestFacts', 'Publish-QualificationRootStage', 'Initialize-QualificationRoot', 'ConvertTo-WindowsCommandLine', 'Invoke-BoundedProcess', 'ConvertFrom-AimQueryJson', 'Select-RelevantGuestProcesses', 'Assert-UnexpectedReset', 'Invoke-FormalRunnerCapture', 'Invoke-FormalCreateSequence', 'Invoke-FormalRecoverySequence', 'Invoke-LivePowerCutArm', 'Assert-LivePowerCutRecoveryEvidence', 'Invoke-LivePowerCutRecovery', 'Assert-NegativeFixtureManifest', 'Assert-NegativeInventory', 'Assert-NegativeCaseEvidence', 'Read-StrictNegativeEvidence', 'Assert-MissingDriverNegativeEvidence', 'Get-MissingDriverCaptureEvent', 'Assert-RemainingNegativeFinalEvidence', 'Invoke-MissingDriverPrepare', 'Invoke-MissingDriverRecovery', 'Invoke-DriverRestoreAndNegative', 'Invoke-QualificationRun', 'Invoke-QualificationDispatcher', 'Assert-QualificationDispatcherSeamVector', 'Invoke-MissingDriverIntegrationSelfTest', 'Invoke-QualificationDispatcherHappyFlowSelfTest', 'Invoke-NestedNegativeBaselineMutationSelfTest', 'Invoke-QualificationDispatcherFailureMatrixSelfTest', 'Invoke-QualificationDispatcherRestoreFailureMatrixSelfTest', 'Invoke-QualificationDispatcherPostGateFailureMatrixSelfTest')
foreach ($name in $requiredFunctions) {
    $matches = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true))
    Need ($matches.Count -eq 1) "guest qualification controller is missing $name"
}
$guestFacts = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-GuestFacts' }, $true))[0]
Need ((@($guestFacts.Body.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath }) -join '|') -ceq 'QualificationRoot|Kit') 'Get-GuestFacts parameter contract is invalid'
$controllerText = [IO.File]::ReadAllText($controller)
$boundedFunction = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-BoundedProcess' }, $true))[0]
$boundedArguments = @($boundedFunction.Body.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -ceq 'Arguments' })[0]
Need ($null -ne $boundedArguments) 'Invoke-BoundedProcess Arguments parameter is missing'
Need (@($boundedArguments.Attributes | Where-Object { $_.TypeName.Name -ceq 'AllowEmptyCollection' }).Count -eq 1) 'Invoke-BoundedProcess must accept an empty argument collection for zero-argument helpers'
$commandLineFunction = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'ConvertTo-WindowsCommandLine' }, $true))[0]
$commandLineArguments = @($commandLineFunction.Body.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -ceq 'Arguments' })[0]
Need ($null -ne $commandLineArguments) 'ConvertTo-WindowsCommandLine Arguments parameter is missing'
Need (@($commandLineArguments.Attributes | Where-Object { $_.TypeName.Name -ceq 'AllowEmptyCollection' }).Count -eq 1) 'ConvertTo-WindowsCommandLine must accept an empty argument collection for zero-argument helpers'
$runner = Join-Path $root 'tests\e2e\Invoke-M4RunLifecycle.ps1'
Need ([IO.File]::Exists($runner)) 'lifecycle runner is missing'
$runnerText = [IO.File]::ReadAllText($runner)
foreach ($requiredText in @('function Assert-NegativeCaseEvidence', 'Assert-NegativeCaseEvidence $record', 'Assert-NegativeCaseEvidence $x')) {
    Need ($runnerText.Contains($requiredText)) "missing-driver runner strict evidence contract is missing $requiredText"
}
Need ($runnerText.Contains('Error: run failed for')) 'missing-driver runner must bind the native refusal to its bundle path'
foreach ($requiredText in @('function Read-StrictNegativeEvidence', 'function Assert-NegativeCaseEvidence', 'Assert-ExactEvidenceProperties $Record', 'negative evidence is not canonical single JSON')) {
    Need ($controllerText.Contains($requiredText)) "controller strict negative evidence mirror is missing $requiredText"
}
foreach ($requiredText in @('negative evidence must be single-line JSON', 'negative process schema is invalid', 'negative oracle schema is invalid', 'negative lock relation is invalid', 'busy_volume relation is invalid')) {
    Need ($controllerText.Contains($requiredText)) "controller strict negative evidence mirror is missing $requiredText"
}
foreach ($requiredText in @('function Assert-NegativeRefusalProtocol', 'function Assert-BusyVolumeProtocol', 'negative refusal stderr protocol is invalid', 'busy stdout protocol is invalid')) {
    Need ($runnerText.Contains($requiredText)) "runner exact negative process protocol is missing $requiredText"
    Need ($controllerText.Contains($requiredText)) "controller exact negative process protocol mirror is missing $requiredText"
}
foreach ($requiredText in @('unknown_nonempty fixture path must remain absent', 'occupied_letter fixture path must remain absent', 'missing_driver fixture path must remain absent', 'lifecycle boot identity is not bound to baseline inventory')) {
    Need ($controllerText.Contains($requiredText)) "controller frozen fixture/lifecycle binding is missing $requiredText"
}
foreach ($requiredText in @('remaining negative final evidence schema is invalid', 'remaining negative final evidence type is invalid', 'remaining negative final evidence inventory is invalid', 'remaining negative final evidence is not canonical single JSON')) {
    Need ($controllerText.Contains($requiredText)) "controller strict final lifecycle mirror is missing $requiredText"
}
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
foreach ($requiredText in @('$script:DriverInvoker', '$script:PrepareInvoker', '$script:NegativeInvoker', 'RESET STILL REQUIRED: MissingDriver', "@('config','phdskmnt','start=','disabled')", "@('config','phdskmnt','start=','demand')", "@('start','phdskmnt')", 'm4-negative-missing_driver.v1.json')) { Need ($controllerText.Contains($requiredText)) "missing-driver controller source contract is missing $requiredText" }
foreach ($forbiddenText in @('sc.exe stop phdskmnt', 'sc.exe delete phdskmnt')) { Need (-not $controllerText.Contains($forbiddenText)) "missing-driver controller source contract contains forbidden operation $forbiddenText" }
foreach ($requiredText in @('Join-Path $formal ''m4-lifecycle.state.json.negative-fixtures.json''', 'Join-Path $formal ''m4-negative-missing_driver.v1.json''', 'Join-Path $FormalRoot "m4-negative-$case.v1.json"')) { Need ($controllerText.Contains($requiredText)) "producer formal-state path contract is missing $requiredText" }
foreach ($requiredText in @("`$stateFile=Join-Path `$formal 'm4-lifecycle.state.json'", '$StateFile.negative-fixtures.json', "Join-Path (Split-Path -Parent (`[IO.Path`]::GetFullPath(`$StateFile))) 'm4-lifecycle-Negative.v1.json'")) { Need ($controllerText.Contains($requiredText)) "controller exact state-file contract is missing $requiredText" }
Need (-not $controllerText.Contains("Join-Path `$root 'evidence\m4-negative-")) 'producer retains a legacy root evidence path'
foreach ($requiredText in @('function Assert-ScQueryLine', "'START_TYPE'", "'STATE'", "'DISABLED'", "'DEMAND_START'", "'RUNNING'", "'86400'", "'1048576'")) { Need ($controllerText.Contains($requiredText)) "producer strict protocol contract is missing $requiredText" }
foreach ($forbiddenText in @("-cmatch 'DISABLED'", "-cmatch 'MANUAL'", "-cmatch 'RUNNING'", "`$checkpoint='30'")) { Need (-not $controllerText.Contains($forbiddenText)) "producer compatibility contract retains obsolete protocol $forbiddenText" }
Need ($controllerText.Contains('$script:BootProbeProvider')) 'missing-driver same-boot gate must inject BootProbeProvider'
foreach ($requiredText in @('$script:QualificationKitProvider', '$script:ExitProvider', 'Invoke-QualificationRun -QualificationRoot')) { Need ($controllerText.Contains($requiredText)) "dispatcher fake-flow seam is missing $requiredText" }
$selfTest = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-SelfTest' }, $true))[0]
foreach ($helper in @('Invoke-MissingDriverIntegrationSelfTest', 'Invoke-QualificationDispatcherHappyFlowSelfTest', 'Invoke-QualificationDispatcherFailureMatrixSelfTest', 'Invoke-QualificationDispatcherRestoreFailureMatrixSelfTest', 'Invoke-NestedNegativeBaselineMutationSelfTest', 'Invoke-QualificationDispatcherPostGateFailureMatrixSelfTest')) {
    Need (@($selfTest.Body.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq $helper }, $true)).Count -eq 1) "controller SelfTest does not execute $helper"
}
Need (-not $controllerText.Contains('dispatcher failure seam vector is invalid')) 'controller retains obsolete dispatcher failure seam vector comment'
Need ($controllerText.Contains("kind='MissingDriverCapture'")) 'missing-driver evidence must be durably hash-anchored'
Need ($controllerText.Contains('Get-MissingDriverCaptureEvent $state')) 'driver restore must re-read the missing-driver capture anchor'
foreach ($failureRow in @('DriverDisableNonzero', 'DriverDisableTimeout', 'DriverDisableReadbackNonzero', 'DriverDisableReadbackNotDisabled', 'NewBootFactsModel', 'QueryWrongExit', 'NegativeChildExit', 'NegativeStdoutNoCrLf', 'NegativeEvidenceEmpty', 'NegativeEvidenceStderrBundlePath', 'CaptureHashTamper')) {
    Need ($controllerText.Contains("'$failureRow'")) "missing-driver failure matrix row is missing: $failureRow"
}
foreach ($failureRow in @('RestoreDemandNonzero', 'RestoreDemandTimeout', 'RestoreDemandReadbackNonzero', 'RestoreDemandReadbackNotDemand', 'RestoreStartNonzero', 'RestoreStartTimeout', 'RestoreFactsAims', 'RestoreFactsPd2', 'ActivateNonzero', 'ActivateHelperHash', 'ActivateManifestActiveCases', 'ActivateCompactInventory', 'FinalNonzero', 'FinalTimeout', 'FinalStderr', 'FinalEvidenceSchema', 'FinalEvidenceCaseHash')) {
    Need ($controllerText.Contains("'$failureRow'")) "restore/final failure matrix row is missing: $failureRow"
}
foreach ($failureRow in @('VerifierFinalExit', 'VerifierFinalTimeout', 'VerifierFinalStderr', 'VerifierFinalStdout', 'VerifierPowerCutExit', 'VerifierPowerCutTimeout', 'VerifierPowerCutStderr', 'VerifierPowerCutStdout', 'PostGateFacts', 'PostGateKit', 'PostGateFormalArtifact', 'PostGateFormalBundleInventory', 'PostGateFinalEvidenceHash', 'PostGateCaseEvidenceHash', 'PostGateArmHash', 'PostGateArmPath', 'PostGateArmSchema', 'PostGatePowerBundleInventory', 'PostGateRecoveryEvidenceMissing', 'PostGateRecoveryEvidenceHash')) {
    Need ($controllerText.Contains("'$failureRow'")) "postgate failure matrix row is missing: $failureRow"
}
Need ($controllerText.Contains('New-QualificationPostGateFixture')) 'postgate matrix must build a fresh legal dispatcher fixture'
Need ($controllerText.Contains('Invoke-QualificationDispatcher -QualificationRoot $fixture.root')) 'postgate matrix must exercise the production dispatcher'
Need ($controllerText.Contains("kind='NegativeComplete'")) 'final negative evidence must be durably anchored'
$postGate = @($controllerAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Assert-QualificationPostGate' }, $true))[0]
Need ($postGate.Body.Extent.Text.Contains('$arm=Get-LivePowerCutArmEvent $state')) 'postgate must require exactly one durable LivePowerCutArm event'
Need (-not $postGate.Body.Extent.Text.Contains("`$arms=@(`$state.events|Where-Object{`$_.kind -ceq 'LivePowerCutArm'});if(`$arms.Count-eq1)")) 'postgate must not allow a NegativeComplete state without a LivePowerCutArm event'
Need ($controllerText.Contains('preexisting remaining negative evidence is forbidden')) 'remaining negative evidence must be absent before driver restore ACK'
Need ($controllerText.Contains('Assert-NegativeCaseEvidence $record $case')) 'final evidence must validate every negative case through the strict mirror'
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

function Read-Utf8NoBomText {
    param([Parameter(Mandatory = $true)][string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    Need ($bytes.Length -lt 3 -or -not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) "$Path must be UTF-8 without BOM"
    return [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
}

function Test-OrdinalIgnoreCaseMember {
    param([Parameter(Mandatory = $true)][string[]]$Values, [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Candidate)
    return (@($Values | Where-Object { [string]::Equals($_, $Candidate, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0)
}

function Test-SafetyOffsetExcluded {
    param([Parameter(Mandatory = $true)][int]$Offset, [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ExcludedFunctions)
    return (@($ExcludedFunctions | Where-Object { $Offset -ge $_.Extent.StartOffset -and $Offset -lt $_.Extent.EndOffset }).Count -gt 0)
}

function Get-ProductionSafetyNodes {
    param([Parameter(Mandatory = $true)]$Ast, [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$ExcludedFunctionNames, [Parameter(Mandatory = $true)][string]$Label)
    $functions = @($Ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $true))
    $excluded = @()
    foreach ($name in $ExcludedFunctionNames) {
        $matches = @($functions | Where-Object { [string]::Equals($_.Name, $name, [StringComparison]::Ordinal) })
        Need ($matches.Count -eq 1) "static safety exclusion must name one audited self-test function in ${Label}: $name"
        $excluded += $matches[0]
    }
    foreach ($name in $ExcludedFunctionNames) {
        foreach ($call in @($Ast.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] -and [string]::Equals($node.GetCommandName(), $name, [StringComparison]::OrdinalIgnoreCase) }, $true))) {
            $caller = @($functions | Where-Object { $call.Extent.StartOffset -ge $_.Extent.StartOffset -and $call.Extent.StartOffset -lt $_.Extent.EndOffset } | Sort-Object { $_.Extent.EndOffset - $_.Extent.StartOffset } | Select-Object -First 1)
            if ($caller.Count -eq 1) {
                Need (Test-SafetyOffsetExcluded -Offset $caller[0].Extent.StartOffset -ExcludedFunctions $excluded) "static safety exclusion has a production caller in ${Label}: $name"
            }
        }
    }
    return [pscustomobject]@{
        Commands = @($Ast.FindAll({ param($node) $node -is [Management.Automation.Language.CommandAst] }, $true) | Where-Object { -not (Test-SafetyOffsetExcluded -Offset $_.Extent.StartOffset -ExcludedFunctions $excluded) })
        Strings = @($Ast.FindAll({ param($node) $node -is [Management.Automation.Language.StringConstantExpressionAst] -or $node -is [Management.Automation.Language.ExpandableStringExpressionAst] }, $true) | Where-Object { -not (Test-SafetyOffsetExcluded -Offset $_.Extent.StartOffset -ExcludedFunctions $excluded) })
    }
}

function Assert-ProductionSafetyAst {
    param([Parameter(Mandatory = $true)]$Ast, [Parameter(Mandatory = $true)][string]$Label, [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$ExcludedFunctionNames)
    $forbiddenCommands = @('ssh.exe', 'scp.exe', 'vmrun', 'shutdown.exe', 'Restart-Computer', 'Stop-Computer', 'Register-ScheduledTask', 'schtasks', 'Format-Volume', 'Initialize-Disk', 'New-Partition', 'remove_device')
    $removalAliases = @('Remove-Item', 'rm', 'ri', 'del', 'erase', 'rmdir')
    $nodes = Get-ProductionSafetyNodes -Ast $Ast -ExcludedFunctionNames $ExcludedFunctionNames -Label $Label
    foreach ($command in $nodes.Commands) {
        $name = [string]$command.GetCommandName()
        $literal = $null
        if ([string]$command.InvocationOperator -eq 'Ampersand' -and $command.CommandElements.Count -gt 0) {
            $first = $command.CommandElements[0]
            if ($first -is [Management.Automation.Language.StringConstantExpressionAst] -or $first -is [Management.Automation.Language.ExpandableStringExpressionAst]) { $literal = [string]$first.Value }
        }
        if ($null -ne $literal -and (Test-OrdinalIgnoreCaseMember -Values ($forbiddenCommands + $removalAliases) -Candidate $literal)) {
            throw "static safety rejects literal call-operator token $literal in $Label"
        }
        if (Test-OrdinalIgnoreCaseMember -Values $forbiddenCommands -Candidate $name) {
            throw "static safety rejects direct command $name in $Label"
        }
        if (Test-OrdinalIgnoreCaseMember -Values $removalAliases -Candidate $name) {
            $targetsRoot = @($command.FindAll({ param($node) $node -is [Management.Automation.Language.VariableExpressionAst] -and [string]::Equals($node.VariablePath.UserPath, 'QualificationRoot', [StringComparison]::OrdinalIgnoreCase) }, $true)).Count -gt 0
            if ($targetsRoot) { throw "static safety rejects qualification-root removal alias $name in $Label" }
        }
    }
    foreach ($stringNode in $nodes.Strings) {
        if ([string]$stringNode.Value -match '(?i)(password|private\s*key)') { throw "static safety rejects secret-bearing literal in $Label" }
    }
}

$readmePath = Join-Path $SourceRoot 'README.md'
$notesPath = Join-Path $SourceRoot 'RELEASE_NOTES.md'
$readme = Read-Utf8NoBomText -Path $readmePath
$notes = Read-Utf8NoBomText -Path $notesPath
function From-QualificationUtf8Base64 {
    param([Parameter(Mandatory = $true)][string]$Value)
    return [Text.UTF8Encoding]::new($false, $true).GetString([Convert]::FromBase64String($Value))
}
foreach ($phrase in @(
    (From-QualificationUtf8Base64 'djEuMCDmnaXlrr7otYTmoLzmtYvor5XvvIjpq5jnuqcv5Y+R5biD57u05oqk6ICF77yJ'),
    (From-QualificationUtf8Base64 '6LWE5qC86ISa5pys5LiN6L+b5YWlIHYxIOWFreaWh+S7tiByZWxlYXNlIFpJUA=='),
    (From-QualificationUtf8Base64 'aG9zdCDnlJ/miJDlubblhrvnu5MgcXVhbGlmaWNhdGlvbiBraXQ='),
    'approved isolated guest',
    (From-QualificationUtf8Base64 '5pys5py6IE5URlM='),
    'CRASH RESET ARMED', 'LIVE POWER-CUT ARMED', 'MISSING-DRIVER RESET ARMED',
    (From-QualificationUtf8Base64 '5ZCM5LiA6ZqP5py6IFF1YWxpZmljYXRpb25Sb290'), (From-QualificationUtf8Base64 '5rC45LmF5L+d55WZ'), (From-QualificationUtf8Base64 '5LiN5b6X6YeN6K+V'),
    '-Status', (From-QualificationUtf8Base64 '6YCA5Ye656CBIDEw'), (From-QualificationUtf8Base64 '6YCA5Ye656CBIDc3'), 'M4 v1 QUALIFICATION PASS')) {
    Need ($readme.Contains($phrase)) "README is missing Chinese qualification workflow phrase: $phrase"
}
Need ($readme.Contains(('powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Invoke-M4GuestQualification.ps1 -QualificationRoot <' + (From-QualificationUtf8Base64 '5pys5py6TlRGU+e7neWvuei3r+W+hA==') + '>'))) 'README guest qualification command is not the real controller command'
Need (-not ($readme -match '(?m)^powershell\.exe .*Invoke-M4GuestQualification\.ps1 .*\s-Run(?:\s|$)')) 'README documents the nonexistent -Run controller parameter'
foreach ($requiredText in @('New-M4GuestQualificationKit.ps1', '-SourceRoot $SourceRoot', '-Artifact $Artifact', '-Query $Query', '-OutputDirectory $OutputDirectory', "Join-Path `$SourceRoot 'out\build\msvc\src\Release\myrd.exe'", "Join-Path `$SourceRoot 'out\build\msvc\tests\Release\myrd_m4_aim_query.exe'", 'Test-Path -LiteralPath', 'Get-FileHash -LiteralPath', 'qualification-kit-')) {
    Need ($readme.Contains($requiredText)) "README host kit command contract is missing $requiredText"
}
$packagerPath = Join-Path $SourceRoot 'tools\New-M4GuestQualificationKit.ps1'
$packagerTokens = $null; $packagerErrors = $null
$packagerAst = [Management.Automation.Language.Parser]::ParseFile($packagerPath, [ref]$packagerTokens, [ref]$packagerErrors)
Need ($packagerErrors.Count -eq 0) 'qualification kit packager must parse for README parameter validation'
$packagerParameterNames = @($packagerAst.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
foreach ($parameterName in @('SourceRoot', 'Artifact', 'Query', 'OutputDirectory')) {
    Need ($packagerParameterNames -ccontains $parameterName) "qualification kit packager parameter is missing: $parameterName"
}
Need ($readme -notmatch '(?i)C:\\Users\\') 'README must not hard-code a local user path'
Need ($readme -match '(?s)Reset 1.*CreateKill.*Reset 2.*live power-cut.*Reset 3.*phdskmnt') 'README does not provide the ordered three-reset UI table'
Need ($readme -match ((From-QualificationUtf8Base64 '5LiN5b6X6aKd5aSW') + '.*reset')) 'README does not prohibit extra resets'
Need ($readme -match ((From-QualificationUtf8Base64 '5LiN5b6X') + '.*Windows.*' + (From-QualificationUtf8Base64 '5q2j5bi45YWz5py6'))) 'README does not prohibit normal Windows shutdown'
Need ($readme -match ((From-QualificationUtf8Base64 '5LiN5b6X') + '.*' + 'shutdown')) 'README does not prohibit script shutdown'
Need ($notes.Contains((From-QualificationUtf8Base64 '6LWE5qC8IGNvbnRyb2xsZXIvaGVscGVyL2tpdCDku4XkuLrpqozor4Hlt6XlhbfvvIzkuI3ov5vlhaUgdjEg5YWt5paH5Lu2IFpJUA=='))) 'release notes omit the qualification-tool ZIP boundary'
Need ($notes.Contains((From-QualificationUtf8Base64 '5LiJ5qyh55yf5a6eIFVJIOa1i+ivleWwmuW+heeUqOaIt+aYjuehruWQr+WKqA=='))) 'release notes overstate the unstarted real-VM qualification'

$paramNames = @($controllerAst.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
Need ($paramNames -ccontains 'QualificationRoot') 'controller parameter contract is missing QualificationRoot'
Need ($paramNames -ccontains 'Status') 'controller parameter contract is missing Status'
Need (-not ($paramNames -ccontains 'Run')) 'controller unexpectedly defines a Run parameter; README command contract must be reviewed'

$safetyContracts = @(
    [pscustomobject]@{ Path = $controller; Excluded = @('Invoke-QualificationKitValidatorSelfTest', 'Invoke-MissingDriverIntegrationSelfTest', 'Invoke-QualificationDispatcherHappyFlowSelfTest', 'Invoke-QualificationDispatcherFailureMatrixSelfTest', 'New-QualificationPostGateFixture', 'Invoke-QualificationDispatcherPostGateFailureMatrixSelfTest', 'Invoke-NestedNegativeBaselineMutationSelfTest', 'Invoke-QualificationDispatcherRestoreFailureMatrixSelfTest', 'Invoke-SelfTest') },
    [pscustomobject]@{ Path = (Join-Path $SourceRoot 'tests\e2e\Invoke-M4LivePowerCut.ps1'); Excluded = @('SelfTest') },
    [pscustomobject]@{ Path = (Join-Path $SourceRoot 'tools\New-M4GuestQualificationKit.ps1'); Excluded = @() })
foreach ($contract in $safetyContracts) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($contract.Path, [ref]$tokens, [ref]$errors)
    Need ($errors.Count -eq 0) "$($contract.Path) must parse for the static safety contract"
    Assert-ProductionSafetyAst -Ast $ast -Label $contract.Path -ExcludedFunctionNames $contract.Excluded
}

function Parse-SafetyFixture {
    param([Parameter(Mandatory = $true)][string]$Text)
    $fixtureTokens = $null; $fixtureErrors = $null
    $fixtureAst = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$fixtureTokens, [ref]$fixtureErrors)
    Need ($fixtureErrors.Count -eq 0) 'safety mutation fixture must parse'
    return $fixtureAst
}

foreach ($mutation in @(
    'function Production { ShUtDoWn.ExE }',
    'function Production { rm $QualificationRoot -Recurse -Force }',
    "function Production { & 'VmRuN' }")) {
    $rejected = $false
    $reason = ''
    try { Assert-ProductionSafetyAst -Ast (Parse-SafetyFixture -Text $mutation) -Label 'mutation' -ExcludedFunctionNames @() }
    catch { $rejected = $true; $reason = $_.Exception.Message }
    Need $rejected "static safety mutation was accepted: $mutation"
    Need ($reason -match '^static safety rejects ') "static safety mutation failed for the wrong reason: $reason"
}

Write-Output 'M4 guest qualification contract PASS'
