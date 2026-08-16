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
foreach($required in @('Invoke-LivePowerCutArm','Invoke-LivePowerCutRecover','StorageInspector','WorkloadProvider','EventProvider','SignalProvider','Arm','Recover','SelfTest','Ready-GenerationText','Latest-CheckpointText','[uint64]::TryParse','(?=\r?\n|\z)','CreateProcessW','CREATE_NEW_PROCESS_GROUP','GenerateConsoleCtrlEvent','owned-stdout.log','owned-stderr.log','Flush($true)','myrd.m4.live-powercut-arm.v1','myrd.m4.live-powercut.v1')) { Need ($text.Contains($required)) "live power-cut helper lacks $required" }
foreach($required in @('New-LivePowerCutDeadline','Get-LivePowerCutRemainingMilliseconds','$script:ClockProvider','$script:WaitSleeper','EmergencyTerminateAndWait(5000)')) { Need ($text.Contains($required)) "live power-cut helper lacks shared deadline hardening $required" }
foreach($forbiddenOwned in @('RedirectStandardOutput=$true','RedirectStandardError=$true','new Thread(','M4OwnedChild')) { Need (-not $text.Contains($forbiddenOwned)) "live power-cut helper retains unsafe owned stream handling $forbiddenOwned" }
Need ($text.Contains('public short wShowWindow,cbReserved2')) 'native STARTUPINFO layout does not preserve WORD fields'
foreach($requiredNative in @('STARTUPINFOEX','PROC_THREAD_ATTRIBUTE_HANDLE_LIST','InitializeProcThreadAttributeList','UpdateProcThreadAttribute','GetProcessTimes','TerminateProcess','WaitForSingleObject','CloseHandle(processHandle)','GenerateConsoleCtrlEvent(1,group)')) { Need ($text.Contains($requiredNative)) "live power-cut helper lacks native ownership hardening $requiredNative" }
foreach($forbiddenNative in @('Process.GetProcessById','CloseHandle(pi.hProcess)','System.Diagnostics.Process')) { Need (-not $text.Contains($forbiddenNative)) "live power-cut helper retains unsafe process ownership $forbiddenNative" }
foreach($forbidden in @('ssh.exe','scp.exe','vmrun','shutdown.exe','Restart-Computer','Stop-Computer','Get-Process -Name','Stop-Process -Name')) { Need (-not $text.Contains($forbidden)) "live power-cut helper contains forbidden $forbidden" }
$out = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $helper -Mode SelfTest 2>&1 | ForEach-Object { [string]$_ })
Need ($LASTEXITCODE -eq 0) "live power-cut selftest exit ${LASTEXITCODE}: $($out -join '|')"
Need ($out.Count -eq 1 -and $out[0] -ceq 'M4 live power-cut SelfTest PASS') 'live power-cut selftest output is not exact'
Write-Output 'M4 live power-cut contract PASS'
