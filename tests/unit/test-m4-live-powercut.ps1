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

# Task 8 verifier contract: this intentionally uses a complete producer-shaped
# recovery record.  It is local-only and never invokes the live helper.
$verifier = Join-Path $root 'tools\Invoke-M4Verification.ps1'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('m4-powercut-verifier-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    $evidence = Join-Path $fixture 'evidence'; New-Item -ItemType Directory -Path $evidence | Out-Null
    $artifact = Join-Path $fixture 'Release\myrd.exe'; New-Item -ItemType Directory -Path (Split-Path $artifact -Parent) | Out-Null
    [IO.File]::WriteAllText($artifact, 'fixture', [Text.UTF8Encoding]::new($false))
    $bundle = Join-Path $fixture 'bundle'; New-Item -ItemType Directory -Path $bundle | Out-Null
    [IO.File]::WriteAllText((Join-Path $bundle 'A.txt'), 'A', [Text.UTF8Encoding]::new($false)); [IO.File]::WriteAllText((Join-Path $bundle 'D.txt'), 'D', [Text.UTF8Encoding]::new($false))
    $sha = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToUpperInvariant(); $hashA=(Get-FileHash (Join-Path $bundle 'A.txt') -Algorithm SHA256).Hash.ToUpperInvariant(); $hashD=(Get-FileHash (Join-Path $bundle 'D.txt') -Algorithm SHA256).Hash.ToUpperInvariant(); $commit='a'*40
    $inv=[ordered]@{stable_identities=@('disk|fixture');drive_mappings=@('C:|fixture')}; $volume=[ordered]@{volume_guid='V';filesystem='NTFS';partition_style='GPT';disk_number=[int64]3;partition_number=[int64]1;expected_physical_identity='fixture';validated_expected=$true}
    $doc=[ordered]@{schema='myrd.m4.live-powercut.v1';status='PASS';source_commit=$commit;executable_sha256=$sha;armed_boot_utc='2026-08-16T00:00:00.0000000Z';recovered_boot_utc='2026-08-16T00:02:00.0000000Z';shutdown_events=@([ordered]@{id=[int64]41;time_utc='2026-08-16T00:01:00.0000000Z'},[ordered]@{id=[int64]6008;time_utc='2026-08-16T00:01:01.0000000Z'},[ordered]@{id=[int64]41;time_utc='2026-08-16T00:01:02.0000000Z'});old_pid=[int64]42;old_pid_absent=$true;armed_generation=[int64]7;recovered_generation=[int64]8;baseline_inventory=$inv;inventory_after=$inv;volume=$volume;hashes_before=[ordered]@{'A.txt'=$hashA;'D.txt'=$hashD};hashes_after=[ordered]@{'A.txt'=$hashA;'D.txt'=$hashD;'post-recovery.txt'=('B'*64)};bundle_inventory_before=@();bundle_inventory_after=@();graceful_exit=[int64]0}
    [IO.File]::WriteAllText((Join-Path $evidence 'myrd.m4.live-powercut.v1.json'), ($doc|ConvertTo-Json -Compress -Depth 10)+"`r`n", [Text.UTF8Encoding]::new($false))
    $out=@(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier -Tier PowerCut -EvidenceDirectory $evidence -HostArtifact $artifact -ExpectedSourceCommit $commit 2>&1 | ForEach-Object {[string]$_})
    Need ($LASTEXITCODE -eq 0) "PowerCut verifier rejected producer-shaped fixture: $($out -join '|')"
    foreach($mutation in @('schema','status','source','artifact','boot','events','bad-id','bad-type','out-window','generation','pid','hash','inventory','extra','missing')) {
        $copy=$doc|ConvertTo-Json -Depth 12|ConvertFrom-Json
        switch($mutation){
            'schema'{$copy.schema='forged'};'status'{$copy.status='FAIL'};'source'{$copy.source_commit='b'*40};'artifact'{$copy.executable_sha256='0'*64};'boot'{$copy.recovered_boot_utc=$copy.armed_boot_utc};'events'{$copy.shutdown_events=@([ordered]@{id=[int64]41;time_utc='2026-08-16T00:01:00.0000000Z'})};'bad-id'{$copy.shutdown_events[0].id=[int64]42};'bad-type'{$copy.shutdown_events[0].id='41'};'out-window'{$copy.shutdown_events[0].time_utc='2026-08-16T00:02:00.0000001Z'};'generation'{$copy.recovered_generation=$copy.armed_generation};'pid'{$copy.old_pid=0};'hash'{$copy.hashes_after.'A.txt'='0'*64};'inventory'{$copy.inventory_after.stable_identities=@('forged')};'extra'{$copy|Add-Member forged $true};'missing'{$copy.PSObject.Properties.Remove('volume')}
        }
        [IO.File]::WriteAllText((Join-Path $evidence 'myrd.m4.live-powercut.v1.json'), ($copy|ConvertTo-Json -Compress -Depth 12)+"`r`n", [Text.UTF8Encoding]::new($false))
        $err=Join-Path $fixture ('reject-' + $mutation + '.stderr');$p=Start-Process -FilePath powershell.exe -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$verifier,'-Tier','PowerCut','-EvidenceDirectory',$evidence,'-HostArtifact',$artifact,'-ExpectedSourceCommit',$commit) -RedirectStandardError $err -NoNewWindow -Wait -PassThru
        Need ($p.ExitCode -ne 0) "PowerCut verifier accepted $mutation mutation"
    }
} finally { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue }
