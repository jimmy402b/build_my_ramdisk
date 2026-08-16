[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$SourceRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Need([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

$root = [IO.Path]::GetFullPath($SourceRoot)
$helper = Join-Path $root 'tests\e2e\Invoke-M4LivePowerCut.ps1'
Need ([IO.File]::Exists($helper)) 'live power-cut helper is missing'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($helper, [ref]$tokens, [ref]$errors)
Need ($errors.Count -eq 0) 'live power-cut helper failed AST parsing'
$text = [IO.File]::ReadAllText($helper)
foreach($required in @('Invoke-LivePowerCutArm','Invoke-LivePowerCutRecover','StorageInspector','WorkloadProvider','EventProvider','SignalProvider','Arm','Recover','SelfTest','Ready-GenerationText','Latest-CheckpointText','[uint64]::TryParse','(?=\r?\n|\z)','ProcessStartInfo','M4OwnedChild','GenerateConsoleCtrlEvent','myrd.m4.live-powercut-arm.v1','myrd.m4.live-powercut.v1')) { Need ($text.Contains($required)) "live power-cut helper lacks $required" }
foreach($forbidden in @('ssh.exe','scp.exe','vmrun','shutdown.exe','Restart-Computer','Stop-Computer','Get-Process -Name','Stop-Process -Name')) { Need (-not $text.Contains($forbidden)) "live power-cut helper contains forbidden $forbidden" }
$out = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $helper -Mode SelfTest 2>&1 | ForEach-Object { [string]$_ })
Need ($LASTEXITCODE -eq 0) "live power-cut selftest exit ${LASTEXITCODE}: $($out -join '|')"
Need ($out.Count -eq 1 -and $out[0] -ceq 'M4 live power-cut SelfTest PASS') 'live power-cut selftest output is not exact'
Write-Output 'M4 live power-cut contract PASS'
