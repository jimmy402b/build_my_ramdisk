[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Arm','Recover','SelfTest')][string]$Mode,
    [string]$Executable,
    [string]$Bundle,
    [string]$DriveLetter='R',
    [string]$EvidenceDirectory,
    [string]$SourceCommit,
    [string]$ArmEvidence,
    [ValidateRange(1000,600000)][int]$TimeoutMs=120000
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

class M4LivePowerCutBlocked : System.Exception { M4LivePowerCutBlocked([string]$Message) : base($Message) {} }
function Block([string]$Message) { throw [M4LivePowerCutBlocked]::new($Message) }
function Need([bool]$Condition,[string]$Message) { if(-not $Condition){ Block $Message } }
function Json($Value) { $Value | ConvertTo-Json -Depth 16 -Compress }
function Same($Left,$Right) { (Json $Left) -ceq (Json $Right) }
function Write-New([string]$Path,[string]$Text) {
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.Write($Text); $writer.Flush(); $stream=$null; $writer.Dispose() }
    finally { if($null -ne $stream){$stream.Dispose()} }
}
function UInt64-Strict([string]$Text) { $value=[uint64]0; if(-not [uint64]::TryParse($Text,[ref]$value)){return $null}; if($value -eq 0){return $null}; return $value }
function Ready-GenerationText([string]$Text,[string]$Letter) {
    $escaped=[regex]::Escape($Letter.ToUpperInvariant())
    $matches=[regex]::Matches($Text,"(?m)^Ready: $escaped`: generation ([0-9]+)\.(?=\r?\n|\z)")
    foreach($match in $matches){$value=UInt64-Strict $match.Groups[1].Value;if($null -ne $value){return $value}}
    return $null
}
function Latest-CheckpointText([string]$Text) {
    $last=$null
    foreach($match in [regex]::Matches($Text,'(?m)^Checkpoint committed generation ([0-9]+)\.(?=\r?\n|\z)')) {
        $value=UInt64-Strict $match.Groups[1].Value
        if($null -ne $value){$last=[pscustomobject]@{generation=$value;line=$match.Value}}
    }
    return $last
}
function Inventory {
    $disks=@(Get-CimInstance Win32_DiskDrive -ErrorAction Stop)
    $volumes=@(Get-Volume -ErrorAction Stop | Where-Object {$_.DriveLetter} | ForEach-Object {
        $letter=[string]$_.DriveLetter; $parts=@(Get-Partition -DriveLetter $letter -ErrorAction SilentlyContinue)
        "$($letter.ToUpperInvariant()):|$($_.UniqueId)|$((@($parts|ForEach-Object{"$($_.DiskNumber):$($_.PartitionNumber)"}|Sort-Object)-join ','))"
    } | Sort-Object)
    [ordered]@{stable_identities=@($disks|ForEach-Object{"$($_.PNPDeviceID)|$($_.SerialNumber)|$($_.Model)|$($_.Size)"}|Sort-Object);drive_mappings=$volumes}
}
function StableInventory($Value) { [ordered]@{stable_identities=@($Value.stable_identities);drive_mappings=@($Value.drive_mappings)} }
function Bundle-Inventory([string]$Root) {
    Need (Test-Path -LiteralPath $Root -PathType Container) 'bundle does not exist'
    @((Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
        "$($_.FullName.Substring($Root.Length))|$($_.Length)|$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }))
}
function Volume-Identity([string]$Letter) {
    $volume=Get-Volume -DriveLetter $Letter -ErrorAction Stop
    $part=Get-Partition -DriveLetter $Letter -ErrorAction Stop
    $disk=Get-Disk -Number $part.DiskNumber -ErrorAction Stop
    Need ($volume.FileSystem -ceq 'NTFS') 'Ready volume was not NTFS'
    Need (([string]$disk.PartitionStyle) -ceq 'GPT') 'Ready volume was not GPT'
    [ordered]@{volume_guid=[string]$volume.UniqueId;filesystem='NTFS';partition_style='GPT';disk_number=[int]$disk.Number;partition_number=[int]$part.PartitionNumber}
}
function Workload-Hashes([string]$Letter,[string[]]$Names) {
    $hashes=[ordered]@{}; foreach($name in $Names){$path=Join-Path "$Letter`:" $name;Need([IO.File]::Exists($path))"missing workload $name";$hashes[$name]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash};$hashes
}
function Run-Argv([string]$Root,[string]$Letter) { @('run','--bundle',$Root,'--size','64M','--drive-letter',"$Letter`:",'--checkpoint-seconds','5','--checkpoint-dirty-mb','1','--ram-limit','64M') }
function Quote-WindowsArg([string]$Value) { '"'+($Value -replace '(\\*)"','$1$1\"' -replace '(\\*)$','$1$1')+'"' }
function Start-ExactChild([string]$File,[string[]]$Argv) {
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$File;$info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.CreateNoWindow=$true
    # PowerShell 5.1 lacks ArgumentList; quote each frozen argument explicitly.
    $info.Arguments=(@($Argv|ForEach-Object{Quote-WindowsArg ([string]$_)}) -join ' ')
    $owned=[M4OwnedChild]::Start($File,$info.Arguments)
    [pscustomobject]@{Process=$owned.Process;Owned=$owned;argv=@($Argv);stdout=$owned.Stdout;stderr=$owned.Stderr;started_utc=[datetime]::UtcNow.ToString('o')}
}
function Wait-Ready($Child,[string]$Letter,[int]$Milliseconds) {
    $deadline=[datetime]::UtcNow.AddMilliseconds($Milliseconds)
    while([datetime]::UtcNow -lt $deadline){$generation=Ready-GenerationText $Child.stdout.ToString() $Letter;if($null -ne $generation){return $generation};if($Child.Process.HasExited){Block "child exited before Ready: $($Child.Process.ExitCode); stderr=$($Child.stderr.ToString())"};Start-Sleep -Milliseconds 25}
    Block "Ready timeout; stdout=$($Child.stdout.ToString()); stderr=$($Child.stderr.ToString())"
}
function Wait-SuccessorCheckpoint($Child,[uint64]$Generation,[int]$Milliseconds) {
    $deadline=[datetime]::UtcNow.AddMilliseconds($Milliseconds)
    while([datetime]::UtcNow -lt $deadline){$checkpoint=Latest-CheckpointText $Child.stdout.ToString();if($null -ne $checkpoint -and $checkpoint.generation -gt $Generation){return $checkpoint};if($Child.Process.HasExited){Block "child exited before successor checkpoint: $($Child.Process.ExitCode)"};Start-Sleep -Milliseconds 25}
    Block 'successor checkpoint timeout'
}
Add-Type @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
public sealed class M4OwnedChild : IDisposable {
 public readonly Process Process; public readonly StringBuilder Stdout=new StringBuilder(), Stderr=new StringBuilder();
 readonly Thread outputThread,errorThread; bool outputFailed,errorFailed;
 M4OwnedChild(Process process) { Process=process; outputThread=new Thread(()=>Drain(process.StandardOutput,Stdout,true)); errorThread=new Thread(()=>Drain(process.StandardError,Stderr,false)); outputThread.IsBackground=true; errorThread.IsBackground=true; outputThread.Start(); errorThread.Start(); }
 void Drain(StreamReader reader,StringBuilder target,bool output) { try { char[] buffer=new char[1024]; int count; while((count=reader.Read(buffer,0,buffer.Length))>0) lock(target) target.Append(buffer,0,count); } catch { if(output) outputFailed=true; else errorFailed=true; } }
 public static M4OwnedChild Start(string file,string arguments) { var info=new ProcessStartInfo(); info.FileName=file; info.Arguments=arguments; info.UseShellExecute=false; info.RedirectStandardOutput=true; info.RedirectStandardError=true; info.CreateNoWindow=true; var process=new Process(); process.StartInfo=info; if(!process.Start()) throw new InvalidOperationException("ProcessStartInfo failed to start child"); return new M4OwnedChild(process); }
 public void FinishDrains() { if(!outputThread.Join(5000)||!errorThread.Join(5000)) throw new IOException("owned stdout/stderr drain timeout"); if(outputFailed||errorFailed) throw new IOException("owned stdout/stderr drain failed"); }
 public void Dispose() { Process.Dispose(); }
}
'@ -ErrorAction Stop
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class M4LiveCtrlBreak {
 [DllImport("kernel32",SetLastError=true)] static extern bool SetConsoleCtrlHandler(IntPtr handler,bool add);
 [DllImport("kernel32",SetLastError=true)] static extern bool GenerateConsoleCtrlEvent(uint type,uint group);
 public static void Exact(uint group) { if(!SetConsoleCtrlHandler(IntPtr.Zero,true)) throw new InvalidOperationException("SetConsoleCtrlHandler"); try { if(!GenerateConsoleCtrlEvent(1,group)) throw new InvalidOperationException("GenerateConsoleCtrlEvent"); } finally { if(!SetConsoleCtrlHandler(IntPtr.Zero,false)) throw new InvalidOperationException("SetConsoleCtrlHandler restore"); } }
}
'@ -ErrorAction Stop
function Exact-CtrlBreak($Child) { [M4LiveCtrlBreak]::Exact([uint32]$Child.Process.Id) }
function Wait-Exit0($Child,[int]$Milliseconds) { Need($Child.Process.WaitForExit($Milliseconds))'exact child exit timeout';Need($Child.Process.ExitCode -eq 0)"exact child exit $($Child.Process.ExitCode)" }
function Arm-EvidencePath([string]$Directory) { Join-Path $Directory 'myrd.m4.live-powercut-arm.v1.json' }
function Load-Arm([string]$Path) { Need([IO.File]::Exists($Path))'arm evidence is missing';Get-Content -LiteralPath $Path -Raw -Encoding UTF8|ConvertFrom-Json }
function Assert-Identity($Arm) {
    Need($Arm.schema -ceq 'myrd.m4.live-powercut-arm.v1')'arm evidence schema invalid'
    Need($Arm.source_commit -ceq $SourceCommit)'arm source identity changed'
    Need($Arm.executable_sha256 -ceq (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash)'arm artifact identity changed'
    Need((Json @($Arm.bundle_inventory)) -ceq (Json @(Bundle-Inventory $Bundle)))'arm bundle inventory changed'
}
function Arm {
    Need($DriveLetter -match '^[A-Za-z]$')'DriveLetter must be one ASCII letter';Need([IO.File]::Exists($Executable))'Executable is missing';Need($SourceCommit -match '^[0-9A-Fa-f]{40}$')'SourceCommit must be exact 40 hex';Need([IO.Directory]::Exists($EvidenceDirectory))'EvidenceDirectory is missing';Need(-not(Test-Path "$DriveLetter`:"))'Arm requires a free drive letter';Need(-not(Test-Path -LiteralPath $Bundle))'Arm requires an absent bundle'
    $base=Inventory;$child=$null
    try {
        $argv=Run-Argv $Bundle $DriveLetter;$child=Start-ExactChild $Executable $argv;$generation=Wait-Ready $child $DriveLetter $TimeoutMs;$volume=Volume-Identity $DriveLetter
        [IO.File]::WriteAllText((Join-Path "$DriveLetter`:" 'A.txt'),'live-powercut-A',[Text.UTF8Encoding]::new($false));[IO.File]::WriteAllText((Join-Path "$DriveLetter`:" 'D.txt'),'live-powercut-D',[Text.UTF8Encoding]::new($false));$hashes=Workload-Hashes $DriveLetter @('A.txt','D.txt');$successor=Wait-SuccessorCheckpoint $child $generation $TimeoutMs
        Need(-not $child.Process.HasExited)'exact armed child exited before evidence commit'
        $evidence=[ordered]@{schema='myrd.m4.live-powercut-arm.v1';status='ARMED';source_commit=$SourceCommit.ToLowerInvariant();executable_path=[IO.Path]::GetFullPath($Executable);executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;bundle_path=[IO.Path]::GetFullPath($Bundle);bundle_inventory=@(Bundle-Inventory $Bundle);argv=@($argv);armed_boot_utc=([datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime).ToUniversalTime().ToString('o');old_pid=$child.Process.Id;armed_generation=$successor.generation;ready_generation=$generation;volume=$volume;hashes_before=$hashes;baseline_inventory=$base;stdout_path='owned-stdout-memory';stderr_path='owned-stderr-memory'}
        $path=Arm-EvidencePath $EvidenceDirectory;Write-New $path ((Json $evidence)+[Environment]::NewLine);Need(-not $child.Process.HasExited)'exact armed child exited after evidence commit'
        [pscustomobject]@{pid=$child.Process.Id;generation=$successor.generation;arm_evidence=$path;bundle_inventory=$evidence.bundle_inventory;stdout=$child.stdout.ToString();stderr=$child.stderr.ToString();process_alive=$true}
    } finally { if($null -ne $child){$child.Process.Dispose()} }
}
function Recover {
    Need($DriveLetter -match '^[A-Za-z]$')'DriveLetter must be one ASCII letter';Need([IO.File]::Exists($Executable))'Executable is missing';Need($SourceCommit -match '^[0-9A-Fa-f]{40}$')'SourceCommit must be exact 40 hex';$path=$(if($ArmEvidence){$ArmEvidence}else{Arm-EvidencePath $EvidenceDirectory});$arm=Load-Arm $path;Assert-Identity $arm
    $base=Inventory;Need((Json (StableInventory $base)) -ceq (Json (StableInventory $arm.baseline_inventory)))'recovery inventory is not armed baseline';Need(-not(Test-Path "$DriveLetter`:"))'Recover requires absent drive letter';Need($null -eq (Get-Process -Id ([int]$arm.old_pid) -ErrorAction SilentlyContinue))'armed PID is still present'
    $child=$null;try {$child=Start-ExactChild $Executable (Run-Argv $Bundle $DriveLetter);$ready=Wait-Ready $child $DriveLetter $TimeoutMs;Need($ready -gt [uint64]$arm.armed_generation)'recovered generation did not advance';$pre=Workload-Hashes $DriveLetter @('A.txt','D.txt');Need($pre.'A.txt' -ceq $arm.hashes_before.'A.txt' -and $pre.'D.txt' -ceq $arm.hashes_before.'D.txt')'pre-recovery workload hash changed';[IO.File]::WriteAllText((Join-Path "$DriveLetter`:" 'post-recovery.txt'),'post-recovery',[Text.UTF8Encoding]::new($false));$post=Workload-Hashes $DriveLetter @('A.txt','D.txt','post-recovery.txt');$successor=Wait-SuccessorCheckpoint $child $ready $TimeoutMs;Exact-CtrlBreak $child;Wait-Exit0 $child $TimeoutMs;$after=Inventory;Need((Json (StableInventory $after)) -ceq (Json (StableInventory $base)))'clean inventory was not restored'
        [pscustomobject]@{schema='myrd.m4.live-powercut.v1';status='PASS';source_commit=$SourceCommit.ToLowerInvariant();executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;armed_boot_utc=$arm.armed_boot_utc;recovered_boot_utc=([datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime).ToUniversalTime().ToString('o');shutdown_events=@();old_pid=[int]$arm.old_pid;old_pid_absent=$true;armed_generation=[uint64]$arm.armed_generation;recovered_generation=$successor.generation;baseline_inventory=$arm.baseline_inventory;inventory_after=$after;volume=$arm.volume;hashes_before=$arm.hashes_before;hashes_after=$post;bundle_inventory_before=$arm.bundle_inventory;bundle_inventory_after=@(Bundle-Inventory $Bundle);graceful_exit=0}
    } finally {if($null -ne $child){$child.Process.Dispose()}}
}
function SelfTest {
    Need((Ready-GenerationText "Ready: R: generation 1.`r`n" 'R') -eq 1)'Ready CRLF rejected';Need((Ready-GenerationText "Ready: R: generation 2.`n" 'R') -eq 2)'Ready LF rejected';Need((Ready-GenerationText 'Ready: R: generation 3.' 'R') -eq 3)'Ready EOF rejected';foreach($bad in @("Ready: R: generation 0.`r`n","Ready: S: generation 1.`r`n","Ready: R: generation 1. `r`n","Ready: R: generation 1.`r","Ready: R: generation 18446744073709551616.`n")){Need($null -eq (Ready-GenerationText $bad 'R'))'Ready parser accepted invalid input'}
    Need((Latest-CheckpointText "Checkpoint committed generation 2.`r`n").generation -eq 2)'checkpoint CRLF rejected';Need((Latest-CheckpointText "Checkpoint committed generation 3.`n").generation -eq 3)'checkpoint LF rejected';Need((Latest-CheckpointText 'Checkpoint committed generation 4.').generation -eq 4)'checkpoint EOF rejected';foreach($bad in @("Checkpoint committed generation 0.`n","Checkpoint committed generation 1. `n","Checkpoint committed generation 1.`r","Checkpoint committed generation 18446744073709551616.`n")){Need($null -eq (Latest-CheckpointText $bad))'checkpoint parser accepted invalid input'}
    $temp=Join-Path ([IO.Path]::GetTempPath()) ('m4-live-powercut-'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($temp)|Out-Null; $child=$null
    try {
        $script=Join-Path $temp 'child.ps1'
        [IO.File]::WriteAllText($script,'[Console]::Out.WriteLine("Ready: R: generation 1."); [Console]::Error.WriteLine("owned-stderr"); [Console]::Out.WriteLine("Checkpoint committed generation 2."); Start-Sleep -Seconds 20',[Text.UTF8Encoding]::new($false))
        $child=Start-ExactChild (Get-Command powershell.exe).Source @('-NoProfile','-File',$script)
        $ready=Wait-Ready $child 'R' 10000;$checkpoint=Wait-SuccessorCheckpoint $child $ready 10000
        Need($ready -eq 1 -and $checkpoint.generation -eq 2 -and -not $child.Process.HasExited -and $child.stderr.ToString() -match 'owned-stderr')'harmless child real Arm-path contract failed'
        $child.Process.Kill(); Need($child.Process.WaitForExit(5000))'harmless child did not stop';$child.Owned.FinishDrains()
        $early=Start-ExactChild (Get-Command powershell.exe).Source @('-NoProfile','-Command','exit 9');try{Need($early.Process.WaitForExit(5000) -and $early.Process.ExitCode -eq 9)'early exit fixture failed'}finally{$early.Process.Dispose()}
        $noSuccessor='Ready: R: generation 1.';Need($null -eq (Latest-CheckpointText $noSuccessor))'missing successor accepted'
        $wrong=$child.Process.Id+1000000;Need($null -eq (Get-Process -Id $wrong -ErrorAction SilentlyContinue))'wrong PID fixture was unexpectedly live'
    } finally {if($null -ne $child){$child.Process.Dispose()};Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue}
    Write-Output 'M4 live power-cut SelfTest PASS'
}
try {
    switch ($Mode) {
        'SelfTest' { SelfTest; $global:LASTEXITCODE=0 }
        'Arm' { Arm }
        'Recover' { Recover }
    }
} catch [M4LivePowerCutBlocked] {
    Write-Error $_.Exception.Message
    exit 77
} catch {
    [Console]::Error.WriteLine($_.Exception.ToString())
    exit 2
}
exit 0
