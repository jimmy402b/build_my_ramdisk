[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Arm','Recover','SelfTest')][string]$Mode,
    [string]$Executable,
    [string]$Bundle,
    [string]$DriveLetter='R',
    [string]$EvidenceDirectory,
    [string]$SourceCommit,
    [string]$ArmEvidence,
    [int]$ExpectedArmedPid=0,
    [string]$ExpectedArmEvidenceSha256,
    [ValidateRange(1000,600000)][int]$TimeoutMs=120000
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# External effects are named seams.  Production binds these to the real
# Windows implementations below; SelfTest may replace only these providers.
$script:StorageInspector=$null
$script:WorkloadProvider=$null
$script:EventProvider=$null
$script:SignalProvider=$null
$script:FailureInjector=$null

class M4LivePowerCutBlocked : System.Exception { M4LivePowerCutBlocked([string]$Message) : base($Message) {} }
function Block([string]$Message) { throw [M4LivePowerCutBlocked]::new($Message) }
function Need([bool]$Condition,[string]$Message) { if(-not $Condition){ Block $Message } }
function Json($Value) { $Value | ConvertTo-Json -Depth 16 -Compress }
function String-Sha256([string]$Text) { $sha=[Security.Cryptography.SHA256]::Create();try{([BitConverter]::ToString($sha.ComputeHash([Text.UTF8Encoding]::new($false).GetBytes($Text)))).Replace('-','')}finally{$sha.Dispose()} }
function Arm-IdentityHash($Arm) { $value=[ordered]@{};foreach($property in $Arm.psobject.Properties){if($property.Name -cne 'identity_sha256'){$value[$property.Name]=$property.Value}};String-Sha256 (Json $value) }
function Same($Left,$Right) { (Json $Left) -ceq (Json $Right) }
function Write-New([string]$Path,[string]$Text) {
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.Write($Text); $writer.Flush(); $stream=$null; $writer.Dispose() }
    finally { if($null -ne $stream){$stream.Dispose()} }
}
function Write-DurableAtomic([string]$Path,$Value) {
    $tmp="$Path.$([guid]::NewGuid().ToString('N')).tmp"
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes((Json $Value)+[Environment]::NewLine)
    $stream=[IO.FileStream]::new($tmp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough)
    try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
    try {[IO.File]::Move($tmp,$Path)} catch {if([IO.File]::Exists($tmp)){Remove-Item -LiteralPath $tmp -Force};throw}
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
    if($null -ne $script:StorageInspector){ return & $script:StorageInspector 'Inventory' }
    $disks=@(Get-CimInstance Win32_DiskDrive -ErrorAction Stop)
    $volumes=@(Get-Volume -ErrorAction Stop | Where-Object {$_.DriveLetter} | ForEach-Object {
        $letter=[string]$_.DriveLetter; $parts=@(Get-Partition -DriveLetter $letter -ErrorAction SilentlyContinue)
        "$($letter.ToUpperInvariant()):|$($_.UniqueId)|$((@($parts|ForEach-Object{"$($_.DiskNumber):$($_.PartitionNumber)"}|Sort-Object)-join ','))"
    } | Sort-Object)
    [ordered]@{stable_identities=@($disks|ForEach-Object{"$($_.PNPDeviceID)|$($_.SerialNumber)|$($_.Model)|$($_.Size)"}|Sort-Object);drive_mappings=$volumes}
}
function StableInventory($Value) { [ordered]@{stable_identities=@($Value.stable_identities);drive_mappings=@($Value.drive_mappings)} }
function Recovery-Events([datetime]$ArmedBoot) {
    if($null -ne $script:EventProvider){return & $script:EventProvider $ArmedBoot}
    $recovered=([datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime).ToUniversalTime()
    $events=@(Get-WinEvent -FilterHashtable @{LogName='System'; Id=@(41,6008); StartTime=$ArmedBoot} -ErrorAction Stop | ForEach-Object {[pscustomobject]@{id=[int]$_.Id;time_utc=$_.TimeCreated.ToUniversalTime().ToString('o')}})
    [pscustomobject]@{boot_utc=$recovered.ToString('o');events=$events}
}
function Bundle-Inventory([string]$Root) {
    Need (Test-Path -LiteralPath $Root -PathType Container) 'bundle does not exist'
    @((Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Sort-Object FullName | ForEach-Object {
        "$($_.FullName.Substring($Root.Length))|$($_.Length)|$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
    }))
}
function Volume-Identity([string]$Letter) {
    if($null -ne $script:StorageInspector){ return & $script:StorageInspector 'Volume' $Letter }
    $volume=Get-Volume -DriveLetter $Letter -ErrorAction Stop
    $part=Get-Partition -DriveLetter $Letter -ErrorAction Stop
    $disk=Get-Disk -Number $part.DiskNumber -ErrorAction Stop
    Need ($volume.FileSystem -ceq 'NTFS') 'Ready volume was not NTFS'
    Need (([string]$disk.PartitionStyle) -ceq 'GPT') 'Ready volume was not GPT'
    Need ($disk.Size -eq 64MB) 'Ready disk was not 64MiB'
    Need (("$($disk.FriendlyName)|$($disk.Model)|$($disk.BusType)" -match 'Arsenal|Virtual')) 'Ready disk was not the expected virtual disk'
    [ordered]@{volume_guid=[string]$volume.UniqueId;filesystem='NTFS';partition_style='GPT';disk_number=[int]$disk.Number;partition_number=[int]$part.PartitionNumber;expected_physical_identity="$($disk.UniqueId)|$($disk.SerialNumber)|$($disk.Location)|$($disk.FriendlyName)";validated_expected=$true}
}
function Workload-Hashes([string]$Letter,[string[]]$Names) {
    if($null -ne $script:WorkloadProvider){ return & $script:WorkloadProvider 'Hashes' $Letter ([pscustomobject]@{names=@($Names)}) }
    $hashes=[ordered]@{}; foreach($name in $Names){$path=Join-Path "$Letter`:" $name;Need([IO.File]::Exists($path))"missing workload $name";$hashes[$name]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash};$hashes
}
function Write-Workload([string]$Letter,[string]$Name,[string]$Text) {
    if($null -ne $script:WorkloadProvider){ & $script:WorkloadProvider 'Write' $Letter ([pscustomobject]@{name=$Name;text=$Text}); return }
    [IO.File]::WriteAllText((Join-Path "$Letter`:" $Name),$Text,[Text.UTF8Encoding]::new($false))
}
function Run-Argv([string]$Root,[string]$Letter) { $provider=Get-Variable -Name RunArgvProvider -Scope Script -ValueOnly -ErrorAction SilentlyContinue;if($null -ne $provider){return @(& $provider $Root $Letter)};@('run','--bundle',$Root,'--size','64M','--drive-letter',"$Letter`:",'--checkpoint-seconds','5','--checkpoint-dirty-mb','1','--ram-limit','64M') }
function Quote-WindowsArg([string]$Value) { '"'+($Value -replace '(\\*)"','$1$1\"' -replace '(\\*)$','$1$1')+'"' }
function Start-ExactChild([string]$File,[string[]]$Argv) {
    $configuredPrefix=Get-Variable -Name LiveLogPrefix -Scope Script -ValueOnly -ErrorAction SilentlyContinue;$prefix=if($configuredPrefix){$configuredPrefix}else{'powercut'}
    $stdout=Join-Path $EvidenceDirectory "$prefix-owned-stdout.log";$stderr=Join-Path $EvidenceDirectory "$prefix-owned-stderr.log"
    $args=(@($Argv|ForEach-Object{Quote-WindowsArg ([string]$_)}) -join ' ')
    $owned=[M4NativeChild]::Start($File,$args,$stdout,$stderr)
    [pscustomobject]@{Process=$owned;argv=@($Argv);stdout_path=$stdout;stderr_path=$stderr;started_utc=$owned.StartedUtc;creation_utc=$owned.CreationUtc;process_group=$owned.ProcessId}
}
function Read-OwnedLog([string]$Path) { if(-not [IO.File]::Exists($Path)){return ''};$share=[IO.FileShare]([int]([IO.FileShare]::ReadWrite) -bor [int]([IO.FileShare]::Delete));$stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,$share);try{[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false),$false).ReadToEnd()}finally{$stream.Dispose()} }
function Child-Stdout($Child) { Read-OwnedLog $Child.stdout_path }
function Child-Stderr($Child) { Read-OwnedLog $Child.stderr_path }
function Wait-Ready($Child,[string]$Letter,[int]$Milliseconds) {
    $deadline=[datetime]::UtcNow.AddMilliseconds($Milliseconds)
    while([datetime]::UtcNow -lt $deadline){$generation=Ready-GenerationText (Child-Stdout $Child) $Letter;if($null -ne $generation){return $generation};if($Child.Process.HasExited){Block "child exited before Ready: $($Child.Process.ExitCode); stderr=$(Child-Stderr $Child)"};Start-Sleep -Milliseconds 25}
    $captured=Child-Stdout $Child;$codes=(@($captured.ToCharArray()|ForEach-Object{[int][char]$_}) -join ',')
    Block "Ready timeout; stdout=$captured; stdout_length=$($captured.Length); stdout_codepoints=$codes; stderr=$(Child-Stderr $Child)"
}
function Wait-SuccessorCheckpoint($Child,[uint64]$Generation,[int]$Milliseconds) {
    $deadline=[datetime]::UtcNow.AddMilliseconds($Milliseconds)
    while([datetime]::UtcNow -lt $deadline){$checkpoint=Latest-CheckpointText (Child-Stdout $Child);if($null -ne $checkpoint -and $checkpoint.generation -gt $Generation){return $checkpoint};if($Child.Process.HasExited){Block "child exited before successor checkpoint: $($Child.Process.ExitCode)"};Start-Sleep -Milliseconds 25}
    Block 'successor checkpoint timeout'
}
function Wait-FinalSuccessorCheckpoint($Child,[uint64]$Generation,[int]$Milliseconds,[int]$Offset) {
    $deadline=[datetime]::UtcNow.AddMilliseconds($Milliseconds)
    while([datetime]::UtcNow -lt $deadline){$text=Child-Stdout $Child;if($text.Length -ge $Offset){$checkpoint=Latest-CheckpointText $text.Substring($Offset);if($null -ne $checkpoint -and $checkpoint.generation -gt $Generation){return $checkpoint}};if($Child.Process.HasExited){Block "child exited before final successor checkpoint: $($Child.Process.ExitCode)"};Start-Sleep -Milliseconds 25}
    Block 'final successor checkpoint timeout'
}
Add-Type @'
using System;
using System.IO;
using System.Runtime.InteropServices;
public sealed class M4NativeChild : IDisposable {
 const uint GENERIC_WRITE=0x40000000, FILE_SHARE_READ=1, FILE_SHARE_WRITE=2, FILE_SHARE_DELETE=4, CREATE_NEW=1, STARTF_USESTDHANDLES=0x100, CREATE_NEW_PROCESS_GROUP=0x200, EXTENDED_STARTUPINFO_PRESENT=0x00080000, PROC_THREAD_ATTRIBUTE_HANDLE_LIST=0x00020002, STILL_ACTIVE=259;
 [StructLayout(LayoutKind.Sequential)] struct SA { public int nLength; public IntPtr lpSecurityDescriptor; [MarshalAs(UnmanagedType.Bool)] public bool bInheritHandle; }
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct SI { public int cb; public IntPtr lpReserved,lpDesktop,lpTitle; public int dwX,dwY,dwXSize,dwYSize,dwXCountChars,dwYCountChars,dwFillAttribute,dwFlags; public short wShowWindow,cbReserved2; public IntPtr lpReserved2,hStdInput,hStdOutput,hStdError; }
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct STARTUPINFOEX { public SI StartupInfo; public IntPtr lpAttributeList; }
 [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr hProcess,hThread; public int dwProcessId,dwThreadId; }
 [DllImport("kernel32",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFileW(string n,uint a,uint s,ref SA sa,uint d,uint f,IntPtr t);
 [DllImport("kernel32",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateProcessW(string a,System.Text.StringBuilder c,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string dir,ref STARTUPINFOEX si,out PI pi);
 [DllImport("kernel32",SetLastError=true)] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32",SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr h,out uint c);
 [DllImport("kernel32",SetLastError=true)] static extern uint WaitForSingleObject(IntPtr h,uint ms);
 [DllImport("kernel32",SetLastError=true)] static extern bool TerminateProcess(IntPtr h,uint code);
 [DllImport("kernel32",SetLastError=true)] static extern bool GetProcessTimes(IntPtr h,out long creation,out long exit,out long kernel,out long user);
 [DllImport("kernel32",SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list,int count,int flags,ref IntPtr size);
 [DllImport("kernel32",SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,IntPtr attribute,IntPtr value,IntPtr size,IntPtr previous,IntPtr returned);
 [DllImport("kernel32")] static extern void DeleteProcThreadAttributeList(IntPtr list);
 static readonly IntPtr Invalid=new IntPtr(-1); IntPtr processHandle; public static bool ForceGetTimesFailure; public static int LastEmergencyExitCode; public int ProcessId {get;private set;} public string StartedUtc {get;private set;} public string CreationUtc {get;private set;}
 M4NativeChild(IntPtr handle,int pid){processHandle=handle;ProcessId=pid;StartedUtc=DateTime.UtcNow.ToString("o");long creation,exit,kernel,user;if(ForceGetTimesFailure||!GetProcessTimes(handle,out creation,out exit,out kernel,out user))throw new IOException("GetProcessTimes",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));CreationUtc=DateTime.FromFileTimeUtc(creation).ToString("o");}
 static IntPtr Open(string path,ref SA sa){var h=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,ref sa,CREATE_NEW,0,IntPtr.Zero);if(h==Invalid)throw new IOException("CreateFileW log",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));return h;}
public static M4NativeChild Start(string file,string arguments,string stdout,string stderr){var sa=new SA{nLength=Marshal.SizeOf(typeof(SA)),bInheritHandle=true};IntPtr o=IntPtr.Zero,e=IntPtr.Zero,list=IntPtr.Zero,handles=IntPtr.Zero,size=IntPtr.Zero;PI pi=new PI();try{o=Open(stdout,ref sa);e=Open(stderr,ref sa);InitializeProcThreadAttributeList(IntPtr.Zero,1,0,ref size);list=Marshal.AllocHGlobal(size);if(!InitializeProcThreadAttributeList(list,1,0,ref size))throw new IOException("InitializeProcThreadAttributeList",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));handles=Marshal.AllocHGlobal(IntPtr.Size*2);Marshal.WriteIntPtr(handles,0,o);Marshal.WriteIntPtr(handles,IntPtr.Size,e);if(!UpdateProcThreadAttribute(list,0,new IntPtr(PROC_THREAD_ATTRIBUTE_HANDLE_LIST),handles,new IntPtr(IntPtr.Size*2),IntPtr.Zero,IntPtr.Zero))throw new IOException("UpdateProcThreadAttribute",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));var si=new STARTUPINFOEX{StartupInfo=new SI{cb=Marshal.SizeOf(typeof(STARTUPINFOEX)),dwFlags=(int)STARTF_USESTDHANDLES,hStdOutput=o,hStdError=e,hStdInput=IntPtr.Zero},lpAttributeList=list};var cmd=new System.Text.StringBuilder("\""+file+"\" "+arguments);uint flags=CREATE_NEW_PROCESS_GROUP|EXTENDED_STARTUPINFO_PRESENT;if(!CreateProcessW(file,cmd,IntPtr.Zero,IntPtr.Zero,true,flags,IntPtr.Zero,null,ref si,out pi))throw new InvalidOperationException("CreateProcessW",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));if(!CloseHandle(pi.hThread))throw new IOException("CloseHandle thread",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));pi.hThread=IntPtr.Zero;try{return new M4NativeChild(pi.hProcess,pi.dwProcessId);}catch{if(pi.hProcess!=IntPtr.Zero){var failedProcess=pi.hProcess;pi.hProcess=IntPtr.Zero;TerminateProcess(failedProcess,1);WaitForSingleObject(failedProcess,5000);CloseHandle(failedProcess);}throw;}}finally{if(pi.hThread!=IntPtr.Zero)CloseHandle(pi.hThread);if(list!=IntPtr.Zero){DeleteProcThreadAttributeList(list);Marshal.FreeHGlobal(list);}if(handles!=IntPtr.Zero)Marshal.FreeHGlobal(handles);if(o!=IntPtr.Zero)CloseHandle(o);if(e!=IntPtr.Zero)CloseHandle(e);}}
 public bool HasExited {get{uint c;if(!GetExitCodeProcess(processHandle,out c))throw new IOException("GetExitCodeProcess",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));return c!=STILL_ACTIVE;}} public int ExitCode {get{uint c;if(!GetExitCodeProcess(processHandle,out c))throw new IOException("GetExitCodeProcess",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));return unchecked((int)c);}} public bool WaitForExit(int ms){var result=WaitForSingleObject(processHandle,(uint)ms);if(result==0)return true;if(result==0x102)return false;throw new IOException("WaitForSingleObject",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));} public void Kill(){if(!TerminateProcess(processHandle,1))throw new IOException("TerminateProcess",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));} public void EmergencyTerminateAndWait(int ms){if(processHandle==IntPtr.Zero)throw new ObjectDisposedException("M4NativeChild");uint before;if(!GetExitCodeProcess(processHandle,out before))throw new IOException("GetExitCodeProcess",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));if(before==STILL_ACTIVE&&!TerminateProcess(processHandle,1))throw new IOException("TerminateProcess",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));if(!WaitForExit(ms))throw new TimeoutException("owned child emergency exit timeout");uint after;if(!GetExitCodeProcess(processHandle,out after))throw new IOException("GetExitCodeProcess",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));if(after!=1)throw new IOException("owned child emergency exit was not 1");LastEmergencyExitCode=unchecked((int)after);} public void Dispose(){if(processHandle!=IntPtr.Zero){if(!CloseHandle(processHandle))throw new IOException("CloseHandle process",Marshal.GetExceptionForHR(Marshal.GetHRForLastWin32Error()));processHandle=IntPtr.Zero;}}
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
function Exact-CtrlBreak($Child) { [M4LiveCtrlBreak]::Exact([uint32]$Child.Process.ProcessId) }
function Wait-Exit0($Child,[int]$Milliseconds) { Need($Child.Process.WaitForExit($Milliseconds))'exact child exit timeout';$exit=[int]$Child.Process.ExitCode;Need($exit -eq 0)"exact child exit $exit" }
function Complete-OwnedChildFailure($Child,$Primary) {
    $cleanupErrors = [Collections.Generic.List[string]]::new()
    if ($null -ne $Child) {
        try { $Child.Process.EmergencyTerminateAndWait(5000) } catch { $cleanupErrors.Add("emergency terminate: $($_.Exception.Message)") }
        try { $Child.Process.Dispose() } catch { $cleanupErrors.Add("dispose: $($_.Exception.Message)") }
    }
    if ($cleanupErrors.Count -ne 0) { Block "$($Primary.Message); owned child cleanup failed: $($cleanupErrors -join '; ')" }
    throw $Primary
}
function Complete-OwnedChildSuccess($Child) {
    if ($null -eq $Child) { return }
    try { $Child.Process.Dispose() } catch { Block "owned child dispose failed: $($_.Exception.Message)" }
}
function Reject-ChildStderr($Child,[string]$Stage) { Need([string]::IsNullOrEmpty((Child-Stderr $Child))) "$Stage child stderr is not empty" }
function Invoke-FailurePoint([string]$Name) { if($null -ne $script:FailureInjector){ & $script:FailureInjector $Name } }
function Arm-EvidencePath([string]$Directory) { Join-Path $Directory 'myrd.m4.live-powercut-arm.v1.json' }
function Load-Arm([string]$Path) { Need([IO.File]::Exists($Path))'arm evidence is missing';Get-Content -LiteralPath $Path -Raw -Encoding UTF8|ConvertFrom-Json }
function Assert-Identity($Arm) {
    Need($Arm.schema -ceq 'myrd.m4.live-powercut-arm.v1')'arm evidence schema invalid'
    Need($Arm.source_commit -ceq $SourceCommit)'arm source identity changed'
    Need($Arm.executable_sha256 -ceq (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash)'arm artifact identity changed'
    Need((Json @($Arm.bundle_inventory)) -ceq (Json @(Bundle-Inventory $Bundle)))'arm bundle inventory changed'
    Need($Arm.identity_sha256 -ceq (Arm-IdentityHash $Arm))'arm evidence identity hash invalid'
    Need (([IO.Path]::GetFullPath([string]$Arm.bundle_path) -ceq [IO.Path]::GetFullPath($Bundle))) 'arm bundle path changed'
    Need($Arm.old_pid -gt 0 -and $Arm.pid_identity -and $Arm.pid_identity.pid -eq $Arm.old_pid -and $Arm.pid_identity.process_group -eq $Arm.old_pid)'arm PID identity invalid'
    Need($Arm.pid_identity.commandline -ceq (Json @($Arm.argv)))'arm command-line identity changed'
}
function Invoke-LivePowerCutArm {
    Need($DriveLetter -match '^[A-Za-z]$')'DriveLetter must be one ASCII letter';Need([IO.File]::Exists($Executable))'Executable is missing';Need($SourceCommit -match '^[0-9A-Fa-f]{40}$')'SourceCommit must be exact 40 hex';Need([IO.Directory]::Exists($EvidenceDirectory))'EvidenceDirectory is missing';Need(-not(Test-Path "$DriveLetter`:"))'Arm requires a free drive letter';Need(-not(Test-Path -LiteralPath $Bundle))'Arm requires an absent bundle'
    $base=Inventory;$child=$null;$success=$false
    try {
        $argv=Run-Argv $Bundle $DriveLetter;$child=Start-ExactChild $Executable $argv;Invoke-FailurePoint 'Arm.PostStart';Reject-ChildStderr $child 'Arm';$generation=Wait-Ready $child $DriveLetter $TimeoutMs;Reject-ChildStderr $child 'Arm';Invoke-FailurePoint 'Arm.Volume';$volume=Volume-Identity $DriveLetter;Need($volume.validated_expected -eq $true)'Ready volume did not validate the expected physical disk'
        Invoke-FailurePoint 'Arm.Workload';Write-Workload $DriveLetter 'A.txt' 'live-powercut-A';Write-Workload $DriveLetter 'D.txt' 'live-powercut-D';$hashes=Workload-Hashes $DriveLetter @('A.txt','D.txt');Invoke-FailurePoint 'Arm.SuccessorTimeout';$successor=Wait-SuccessorCheckpoint $child $generation $TimeoutMs
        Reject-ChildStderr $child 'Arm';Need(-not $child.Process.HasExited)'exact armed child exited before evidence commit'
        $pidIdentity=[ordered]@{pid=[int]$child.Process.ProcessId;creation_utc=$child.creation_utc;started_utc=$child.started_utc;commandline=(Json @($argv));process_group=[int]$child.process_group}
        $evidence=[ordered]@{schema='myrd.m4.live-powercut-arm.v1';status='ARMED';source_commit=$SourceCommit.ToLowerInvariant();executable_path=[IO.Path]::GetFullPath($Executable);executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;bundle_path=[IO.Path]::GetFullPath($Bundle);bundle_inventory=@(Bundle-Inventory $Bundle);argv=@($argv);armed_boot_utc=([datetime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime).ToUniversalTime().ToString('o');old_pid=$pidIdentity.pid;pid_identity=$pidIdentity;armed_generation=$successor.generation;ready_generation=$generation;volume=$volume;hashes_before=$hashes;baseline_inventory=$base;stdout_path=$child.stdout_path;stderr_path=$child.stderr_path}
        $evidence.identity_sha256=String-Sha256 (Json $evidence);$path=Arm-EvidencePath $EvidenceDirectory;Invoke-FailurePoint 'Arm.EvidenceWriteReadback';Write-DurableAtomic $path $evidence;Need(-not $child.Process.HasExited)'exact armed child exited after evidence commit';Reject-ChildStderr $child 'Arm'
        $result=[pscustomobject]@{pid=$child.Process.ProcessId;generation=$successor.generation;arm_evidence=$path;bundle_inventory=$evidence.bundle_inventory;stdout_path=$child.stdout_path;stderr_path=$child.stderr_path;process_alive=$true};$success=$true;return $result
    } catch { Complete-OwnedChildFailure $child $_.Exception
    } finally { if($success){Complete-OwnedChildSuccess $child} }
}
function Invoke-LivePowerCutRecover {
    Need($DriveLetter -match '^[A-Za-z]$')'DriveLetter must be one ASCII letter';Need([IO.File]::Exists($Executable))'Executable is missing';Need($SourceCommit -match '^[0-9A-Fa-f]{40}$')'SourceCommit must be exact 40 hex';$path=$(if($ArmEvidence){$ArmEvidence}else{Arm-EvidencePath $EvidenceDirectory});$arm=Load-Arm $path;Assert-Identity $arm
    if($ExpectedArmedPid -ne 0 -or -not [string]::IsNullOrWhiteSpace($ExpectedArmEvidenceSha256)){Need($ExpectedArmedPid -gt 0)'ExpectedArmedPid is required';Need($ExpectedArmEvidenceSha256 -match '^[0-9A-Fa-f]{64}$')'ExpectedArmEvidenceSha256 is required';Need((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $ExpectedArmEvidenceSha256)'arm evidence SHA changed';Need($arm.old_pid -eq $ExpectedArmedPid -and $arm.pid_identity.pid -eq $ExpectedArmedPid)'arm expected PID changed'}
    $eventRecord=Recovery-Events ([datetime]$arm.armed_boot_utc);$recoveredBoot=[datetime]$eventRecord.boot_utc;Need($recoveredBoot -gt [datetime]$arm.armed_boot_utc)'recovered boot did not advance';$shutdownEvents=@($eventRecord.events);Need(@($shutdownEvents|Where-Object {$_.id -eq 41}).Count -gt 0)'unexpected shutdown event 41 missing';Need(@($shutdownEvents|Where-Object {$_.id -eq 6008}).Count -gt 0)'unexpected shutdown event 6008 missing';foreach($event in $shutdownEvents){Need($event.id -in @(41,6008) -and ([datetime]$event.time_utc) -gt ([datetime]$arm.armed_boot_utc) -and ([datetime]$event.time_utc) -le $recoveredBoot)'shutdown event time/type invalid'}
    $base=Inventory;Need((Json (StableInventory $base)) -ceq (Json (StableInventory $arm.baseline_inventory)))'recovery inventory is not armed baseline';Need(-not(Test-Path "$DriveLetter`:"))'Recover requires absent drive letter';Need($arm.old_pid -gt 0 -and $arm.pid_identity -and $arm.pid_identity.pid -eq $arm.old_pid)'armed PID identity invalid';Need($null -eq (Get-Process -Id ([int]$arm.old_pid) -ErrorAction SilentlyContinue))'armed PID is still present'
    if([string]::IsNullOrWhiteSpace($script:LiveLogPrefix)){$script:LiveLogPrefix='powercut-recover'};$child=$null;$success=$false;try {$child=Start-ExactChild $Executable (Run-Argv $Bundle $DriveLetter);Reject-ChildStderr $child 'Recover';$ready=Wait-Ready $child $DriveLetter $TimeoutMs;Reject-ChildStderr $child 'Recover';$recoveredVolume=Volume-Identity $DriveLetter;Need((Json $recoveredVolume) -ceq (Json $arm.volume))'recovered volume identity changed';Need($ready -gt [uint64]$arm.armed_generation)'recovered generation did not advance';$pre=Workload-Hashes $DriveLetter @('A.txt','D.txt');Need($pre.'A.txt' -ceq $arm.hashes_before.'A.txt' -and $pre.'D.txt' -ceq $arm.hashes_before.'D.txt')'pre-recovery workload hash changed';Write-Workload $DriveLetter 'post-recovery.txt' 'post-recovery';$post=Workload-Hashes $DriveLetter @('A.txt','D.txt','post-recovery.txt');$beforeSignal=(Child-Stdout $child).Length;Invoke-FailurePoint 'Recover.Signal';if($null -ne $script:SignalProvider){& $script:SignalProvider $child}else{Exact-CtrlBreak $child};Invoke-FailurePoint 'Recover.FinalTimeout';$successor=Wait-FinalSuccessorCheckpoint $child $ready $TimeoutMs $beforeSignal;Wait-Exit0 $child $TimeoutMs;Reject-ChildStderr $child 'Recover';Invoke-FailurePoint 'Recover.CleanInventory';$after=Inventory;Need((Json (StableInventory $after)) -ceq (Json (StableInventory $base)))'clean inventory was not restored'
        $result=[ordered]@{schema='myrd.m4.live-powercut.v1';status='PASS';source_commit=$SourceCommit.ToLowerInvariant();executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;armed_boot_utc=$arm.armed_boot_utc;recovered_boot_utc=$recoveredBoot.ToString('o');shutdown_events=@($shutdownEvents);old_pid=[int]$arm.old_pid;old_pid_absent=$true;armed_generation=[uint64]$arm.armed_generation;recovered_generation=$successor.generation;baseline_inventory=$arm.baseline_inventory;inventory_after=$after;volume=$arm.volume;hashes_before=$arm.hashes_before;hashes_after=$post;bundle_inventory_before=$arm.bundle_inventory;bundle_inventory_after=@(Bundle-Inventory $Bundle);graceful_exit=0};Write-DurableAtomic (Join-Path $EvidenceDirectory 'myrd.m4.live-powercut.v1.json') $result;$success=$true;return $result
    } catch { Complete-OwnedChildFailure $child $_.Exception
    } finally {if($success){Complete-OwnedChildSuccess $child}}
}
function Arm { Invoke-LivePowerCutArm }
function Recover { Invoke-LivePowerCutRecover }
function SelfTest {
    Need((Ready-GenerationText "Ready: R: generation 1.`r`n" 'R') -eq 1)'Ready CRLF rejected';Need((Ready-GenerationText "Ready: R: generation 2.`n" 'R') -eq 2)'Ready LF rejected';Need((Ready-GenerationText 'Ready: R: generation 3.' 'R') -eq 3)'Ready EOF rejected';foreach($bad in @("Ready: R: generation 0.`r`n","Ready: S: generation 1.`r`n","Ready: R: generation 1. `r`n","Ready: R: generation 1.`r","Ready: R: generation 18446744073709551616.`n")){Need($null -eq (Ready-GenerationText $bad 'R'))'Ready parser accepted invalid input'}
    Need((Latest-CheckpointText "Checkpoint committed generation 2.`r`n").generation -eq 2)'checkpoint CRLF rejected';Need((Latest-CheckpointText "Checkpoint committed generation 3.`n").generation -eq 3)'checkpoint LF rejected';Need((Latest-CheckpointText 'Checkpoint committed generation 4.').generation -eq 4)'checkpoint EOF rejected';foreach($bad in @("Checkpoint committed generation 0.`n","Checkpoint committed generation 1. `n","Checkpoint committed generation 1.`r","Checkpoint committed generation 18446744073709551616.`n")){Need($null -eq (Latest-CheckpointText $bad))'checkpoint parser accepted invalid input'}
    $temp=Join-Path ([IO.Path]::GetTempPath()) ('m4-live-powercut-'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($temp)|Out-Null; $child=$null;$originalEvidenceDirectory=$EvidenceDirectory;$EvidenceDirectory=$temp
    try {
        $script=Join-Path $temp 'child.ps1'
        [IO.File]::WriteAllText($script,'[Console]::Out.WriteLine("Ready: R: generation 1."); [Console]::Error.WriteLine("owned-stderr"); [Console]::Out.WriteLine("Checkpoint committed generation 2."); Start-Sleep -Seconds 20',[Text.UTF8Encoding]::new($false))
        $script:LiveLogPrefix='parser';$child=Start-ExactChild (Get-Command powershell.exe).Source @('-NoProfile','-File',$script)
        $ready=Wait-Ready $child 'R' 10000;$checkpoint=Wait-SuccessorCheckpoint $child $ready 10000
        Need($ready -eq 1 -and $checkpoint.generation -eq 2 -and -not $child.Process.HasExited -and (Child-Stderr $child) -match 'owned-stderr')'harmless child real Arm-path contract failed'
        $script:LiveLogPrefix='early';$early=Start-ExactChild (Get-Command powershell.exe).Source @('-NoProfile','-Command','exit 9');try{Need($early.Process.WaitForExit(5000))'early exit fixture failed'}finally{$early.Process.Dispose()}
        $noSuccessor='Ready: R: generation 1.';Need($null -eq (Latest-CheckpointText $noSuccessor))'missing successor accepted'
        $wrong=$child.Process.ProcessId+1000000;Need($null -eq (Get-Process -Id $wrong -ErrorAction SilentlyContinue))'wrong PID fixture was unexpectedly live'
        $script:LiveLogPrefix='constructor-failure';$beforeConstructorFailure=@(Get-Process | ForEach-Object Id);$constructorRejected=$false
        try {[M4NativeChild]::ForceGetTimesFailure=$true;Start-ExactChild (Get-Command powershell.exe).Source @('-NoProfile','-Command','Start-Sleep -Seconds 20')|Out-Null}catch{$constructorRejected=$true}finally{[M4NativeChild]::ForceGetTimesFailure=$false}
        Start-Sleep -Milliseconds 100
        Need($constructorRejected -and @((Get-Process|ForEach-Object Id)|Where-Object{$beforeConstructorFailure-notcontains$_}).Count-eq0)'constructor failure left a native child alive'
    } finally {if($null -ne $child){if(-not $child.Process.HasExited){$child.Process.Kill();$child.Process.WaitForExit(5000)|Out-Null};$child.Process.Dispose()};$EvidenceDirectory=$originalEvidenceDirectory;Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue}
    # The complete orchestration is exercised with a real harmless PowerShell child;
    # only Windows storage/workload/signal observations are substituted.
    $old=@{Executable=$Executable;Bundle=$Bundle;DriveLetter=$DriveLetter;EvidenceDirectory=$EvidenceDirectory;SourceCommit=$SourceCommit;TimeoutMs=$TimeoutMs;Storage=$script:StorageInspector;Workload=$script:WorkloadProvider;Event=$script:EventProvider;Signal=$script:SignalProvider;Failure=$script:FailureInjector;Argv=(Get-Variable -Name RunArgvProvider -Scope Script -ValueOnly -ErrorAction SilentlyContinue)}
    $root=Join-Path ([IO.Path]::GetTempPath()) ('m4-live-orchestration-'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($root)|Out-Null
    try {
        $fixture=Join-Path $root 'harmless-child.ps1'
        [IO.File]::WriteAllText($fixture,'param([string]$Marker,[int]$Generation) [Console]::Out.WriteLine("Ready: R: generation $Generation."); [Console]::Out.WriteLine("Checkpoint committed generation $($Generation + 1)."); while(-not (Test-Path -LiteralPath $Marker)){ Start-Sleep -Milliseconds 25 }; [Console]::Out.WriteLine("Checkpoint committed generation $($Generation + 2)."); exit 0',[Text.UTF8Encoding]::new($false))
        $fixtureRoot=$root;$Executable=(Get-Command powershell.exe).Source;$Bundle=Join-Path $root 'bundle';$DriveLetter='R';$EvidenceDirectory=$root;$SourceCommit=('a'*40);$TimeoutMs=3000;$script:LiveLogPrefix='powercut-arm';$script:RunArgvProvider={param($b,$l)$g=if($env:M4_LIVE_RECOVER -eq '1'){'3'}else{'1'};@('-NoProfile','-ExecutionPolicy','Bypass','-File',$fixture,'-Marker',(Join-Path $fixtureRoot 'continue.txt'),'-Generation',$g)}
        $baseline=[ordered]@{stable_identities=@('baseline');drive_mappings=@('C:|baseline')}
        $script:StorageInspector={param($op,$letter) if($op -eq 'Inventory'){return $baseline};if($op -eq 'Volume'){if(-not(Test-Path 'R:')){New-PSDrive -Name R -PSProvider FileSystem -Root $root -Scope Global|Out-Null};[IO.Directory]::CreateDirectory($Bundle)|Out-Null;return [ordered]@{volume_guid='selftest-volume';filesystem='NTFS';partition_style='GPT';disk_number=7;partition_number=1;expected_physical_identity='selftest-physical';validated_expected=$true}}}
        $script:WorkloadProvider={param($op,$letter,$payload) if($op -eq 'Write'){[IO.File]::WriteAllText((Join-Path $root $payload.name),$payload.text,[Text.UTF8Encoding]::new($false));return};$h=[ordered]@{};foreach($n in $payload.names){$h[$n]=(Get-FileHash -LiteralPath (Join-Path $root $n) -Algorithm SHA256).Hash};return $h}
        $arm=Invoke-LivePowerCutArm;Need($arm.process_alive -and $null -ne (Get-Process -Id $arm.pid -ErrorAction SilentlyContinue))'actual Arm did not return a live exact PID';$armRaw=[IO.File]::ReadAllBytes($arm.arm_evidence);Need($armRaw.Length -gt 3 -and -not($armRaw[0]-eq239-and$armRaw[1]-eq187-and$armRaw[2]-eq191))'arm evidence is not UTF8 without BOM';$armJson=Get-Content -LiteralPath $arm.arm_evidence -Raw -Encoding UTF8|ConvertFrom-Json;Need($armJson.schema -ceq 'myrd.m4.live-powercut-arm.v1' -and $armJson.old_pid -eq $arm.pid -and $armJson.armed_generation -eq 2 -and $armJson.pid_identity.creation_utc -and ((Read-OwnedLog $arm.stdout_path) -match 'Ready: R: generation'))'actual Arm evidence invalid'
        $armedProcess = Get-Process -Id $arm.pid -ErrorAction Stop
        $armedProcess.Kill()
        $armedProcess.WaitForExit()
        Remove-PSDrive -Name R -Force
        $env:M4_LIVE_RECOVER='1';$script:EventProvider={param($armedBoot)$t=([datetime]$armedBoot).AddMilliseconds(1).ToUniversalTime().ToString('o');[pscustomobject]@{boot_utc=([datetime]$armedBoot).AddSeconds(1).ToUniversalTime().ToString('o');events=@([pscustomobject]@{id=[int]41;time_utc=$t},[pscustomobject]@{id=[int]6008;time_utc=$t})}};$script:SignalProvider={param($child) Need($child.Process.ProcessId -gt 0)'signal exact PID missing';[IO.File]::WriteAllText((Join-Path $root 'continue.txt'),'go',[Text.UTF8Encoding]::new($false))}
        $ArmEvidence=$arm.arm_evidence;$ExpectedArmedPid=$arm.pid;$ExpectedArmEvidenceSha256=(Get-FileHash -LiteralPath $ArmEvidence -Algorithm SHA256).Hash;$script:LiveLogPrefix='powercut-recover';$recovered=Invoke-LivePowerCutRecover
        Need($recovered.schema -ceq 'myrd.m4.live-powercut.v1' -and $recovered.status -ceq 'PASS' -and $recovered.recovered_generation -eq 5 -and $recovered.graceful_exit -eq 0)'actual Recover evidence invalid'
        # Each failure point runs the production Arm/Recover function with a real
        # harmless child.  The wrapper only records ownership; it never simulates
        # process creation or cleanup.
        foreach($point in @('Arm.PostStart','Arm.Volume','Arm.Workload','Arm.SuccessorTimeout','Arm.EvidenceWriteReadback','Recover.Signal','Recover.FinalTimeout','Recover.CleanInventory')) {
            $savedStart=${function:Start-ExactChild};$started=$null;$startCount=0;$savedFailure=$script:FailureInjector;$savedPrefix=$script:LiveLogPrefix;$savedBundle=$Bundle;$savedEvidence=$EvidenceDirectory;$savedArm=$ArmEvidence;$savedStorage=$script:StorageInspector;$savedWorkload=$script:WorkloadProvider;$savedSignal=$script:SignalProvider;$caseRoot=Join-Path $root ('failure-'+$point.Replace('.','-')+'-'+[guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($caseRoot)|Out-Null;$before=@(Get-Process|ForEach-Object Id);$typed=$false;$message=''
            try {
                Set-Item function:script:Start-ExactChild {param($File,$Argv)$script:FailureStartCount++; $made=& $savedStart $File $Argv; $script:FailureStarted=$made; return $made};$script:FailureStartCount=0;$script:FailureStarted=$null;$script:LiveLogPrefix=('failure-'+[IO.Path]::GetFileName($caseRoot))
                $script:FailurePoint=$point;$script:FailureInjector={param($name)if($name-ceq$script:FailurePoint){Block "injected $name"}}
                if($point-like'Arm.*') {
                    if(Get-PSDrive R -ErrorAction SilentlyContinue){Remove-PSDrive R -Force}
                    Remove-Item (Join-Path $root 'continue.txt') -Force -ErrorAction SilentlyContinue
                    $Bundle=Join-Path $caseRoot 'bundle';$EvidenceDirectory=$caseRoot;$script:StorageInspector={param($op,$letter)if($op-eq'Inventory'){return $baseline};if($op-eq'Volume'){if(-not(Test-Path 'R:')){New-PSDrive -Name R -PSProvider FileSystem -Root $caseRoot -Scope Global|Out-Null};[IO.Directory]::CreateDirectory($Bundle)|Out-Null;[ordered]@{volume_guid='failure-volume';filesystem='NTFS';partition_style='GPT';disk_number=7;partition_number=1;expected_physical_identity='failure-physical';validated_expected=$true}}};$script:WorkloadProvider={param($op,$letter,$payload)if($op-eq'Write'){[IO.File]::WriteAllText((Join-Path $caseRoot $payload.name),$payload.text);return};$h=[ordered]@{};foreach($n in $payload.names){$h[$n]=(Get-FileHash (Join-Path $caseRoot $n) -Algorithm SHA256).Hash};$h};Invoke-LivePowerCutArm|Out-Null
                } else {
                    $Bundle=$savedBundle;$EvidenceDirectory=$root;$ArmEvidence=$arm.arm_evidence;Remove-Item (Join-Path $root 'continue.txt') -Force -ErrorAction SilentlyContinue;if($point-ceq'Recover.FinalTimeout'){$script:SignalProvider={param($child)}};Invoke-LivePowerCutRecover|Out-Null
                }
            } catch [M4LivePowerCutBlocked] {$typed=$true;$message=$_.Exception.Message
            } finally {if(Get-PSDrive R -ErrorAction SilentlyContinue){Remove-PSDrive R -Force};Set-Item function:script:Start-ExactChild $savedStart;$script:FailureInjector=$savedFailure;$script:LiveLogPrefix=$savedPrefix;$script:StorageInspector=$savedStorage;$script:WorkloadProvider=$savedWorkload;$script:SignalProvider=$savedSignal;$Bundle=$savedBundle;$EvidenceDirectory=$savedEvidence;$ArmEvidence=$savedArm}
            $started=$script:FailureStarted;Need($typed-and$message-match'injected' -and$script:FailureStartCount-eq1-and$null-ne$started)"failure injection $point was not typed after one child start: typed=$typed message=$message starts=$($script:FailureStartCount) started=$($null-ne$started)";Need([M4NativeChild]::LastEmergencyExitCode-eq1)"failure injection $point did not emergency-exit 1";Need($null-eq(Get-Process -Id $started.Process.ProcessId -ErrorAction SilentlyContinue))"failure injection $point left its exact child";Need(@((Get-Process|ForEach-Object Id)|Where-Object{$before-notcontains$_}).Count-eq0)"failure injection $point left a child";Remove-Item $caseRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        foreach ($case in @('source', 'artifact', 'bundle', 'missing41', 'missing6008', 'sameboot', 'wrongpid', 'wrongsha')) {
            $caseRoot = Join-Path $root ("mutation-$case-" + [guid]::NewGuid().ToString('N'))
            $caseBundle = Join-Path $caseRoot 'bundle'
            $savedBundle = $Bundle
            $savedArm = $ArmEvidence
            $savedEvent = $script:EventProvider
            $savedStorage = $script:StorageInspector
            $savedWorkload = $script:WorkloadProvider
            $savedStart = ${function:Start-ExactChild}
            $expectedPid = [int]$arm.pid
            $script:MutationChildStartCount = 0
            $rejected = $false
            $reason = ''
            [IO.Directory]::CreateDirectory($caseRoot) | Out-Null
            Copy-Item -LiteralPath $savedBundle -Destination $caseBundle -Recurse
            try {
                if (Get-PSDrive R -ErrorAction SilentlyContinue) { Remove-PSDrive R -Force }
                $Bundle = $caseBundle
                $script:StorageInspector = {
                    param($op, $letter)
                    if ($op -eq 'Inventory') { return $baseline }
                    if ($op -eq 'Volume') {
                        if (-not (Test-Path 'R:')) { New-PSDrive -Name R -PSProvider FileSystem -Root $caseRoot -Scope Global | Out-Null }
                        return [ordered]@{ volume_guid='case-volume'; filesystem='NTFS'; partition_style='GPT'; disk_number=7; partition_number=1; expected_physical_identity='case-physical'; validated_expected=$true }
                    }
                }
                $script:WorkloadProvider = { param($op, $letter, $payload) Block 'mutation must not reach workload' }
                Set-Item -Path function:script:Start-ExactChild -Value {
                    param($File, $Argv)
                    $script:MutationChildStartCount++
                    Block 'mutation reached child start'
                }

                $copy = Join-Path $caseRoot 'arm.json'
                $bad = Get-Content -LiteralPath $arm.arm_evidence -Raw -Encoding UTF8 | ConvertFrom-Json
                $bad.bundle_path = [IO.Path]::GetFullPath($caseBundle)
                $bad.bundle_inventory = @(Bundle-Inventory $caseBundle)
                switch ($case) {
                    'source' { $bad.source_commit = ('b' * 40) }
                    'artifact' { $bad.executable_sha256 = ('0' * 64) }
                    'bundle' { $bad.bundle_path = 'C:\bad' }
                    'wrongpid' {
                        $bad.old_pid = 999999
                        $bad.pid_identity.pid = 999999
                        $bad.pid_identity.process_group = 999999
                    }
                }
                $bad.identity_sha256 = Arm-IdentityHash $bad
                [IO.File]::WriteAllText($copy, ($bad | ConvertTo-Json -Depth 16), [Text.UTF8Encoding]::new($false))
                $ArmEvidence = $copy
                $ExpectedArmedPid = $expectedPid
                $ExpectedArmEvidenceSha256 = if ($case -eq 'wrongsha') { '0' * 64 } else { (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash }

                if ($case -eq 'missing41') {
                    $script:EventProvider = { param($x) [pscustomobject]@{ boot_utc=([datetime]$x).AddSeconds(1).ToString('o'); events=@([pscustomobject]@{ id=6008; time_utc=([datetime]$x).AddMilliseconds(1).ToString('o') }) } }
                }
                if ($case -eq 'missing6008') {
                    $script:EventProvider = { param($x) [pscustomobject]@{ boot_utc=([datetime]$x).AddSeconds(1).ToString('o'); events=@([pscustomobject]@{ id=41; time_utc=([datetime]$x).AddMilliseconds(1).ToString('o') }) } }
                }
                if ($case -eq 'sameboot') {
                    $script:EventProvider = { param($x) [pscustomobject]@{ boot_utc=$x.ToString('o'); events=@() } }
                }

                Invoke-LivePowerCutRecover | Out-Null
            }
            catch [M4LivePowerCutBlocked] {
                $rejected = $true
                $reason = $_.Exception.Message
            }
            finally {
                if (Get-PSDrive R -ErrorAction SilentlyContinue) { Remove-PSDrive R -Force }
                Set-Item -Path function:script:Start-ExactChild -Value $savedStart
                $script:StorageInspector = $savedStorage
                $script:WorkloadProvider = $savedWorkload
                $script:EventProvider = $savedEvent
                $ArmEvidence = $savedArm
                $Bundle = $savedBundle
                $ExpectedArmedPid = 0
                $ExpectedArmEvidenceSha256 = ''
                Remove-Item -LiteralPath $caseRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
            $expected = @{
                source = 'source identity changed'
                artifact = 'artifact identity changed'
                bundle = 'bundle path changed'
                missing41 = 'event 41'
                missing6008 = 'event 6008'
                sameboot = 'boot did not advance'
                wrongpid = 'arm expected PID changed'
                wrongsha = 'arm evidence SHA changed'
            }[$case]
            Need ($rejected -and $reason -match [regex]::Escape($expected) -and $script:MutationChildStartCount -eq 0 -and -not (Test-Path -LiteralPath $caseRoot)) "mutation $case accepted, wrong reason, or started a child"
        }
    } finally {Remove-Item Env:M4_LIVE_RECOVER -ErrorAction SilentlyContinue;if(Get-PSDrive R -ErrorAction SilentlyContinue){Remove-PSDrive R -Force};$Executable=$old.Executable;$Bundle=$old.Bundle;$DriveLetter=$old.DriveLetter;$EvidenceDirectory=$old.EvidenceDirectory;$SourceCommit=$old.SourceCommit;$TimeoutMs=$old.TimeoutMs;$script:StorageInspector=$old.Storage;$script:WorkloadProvider=$old.Workload;$script:EventProvider=$old.Event;$script:SignalProvider=$old.Signal;$script:FailureInjector=$old.Failure;$script:RunArgvProvider=$old.Argv;$script:LiveLogPrefix=$null;Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue}
    Write-Output 'M4 live power-cut SelfTest PASS'
}
try {
    switch ($Mode) {
        'SelfTest' { SelfTest; $global:LASTEXITCODE=0 }
        'Arm' { Arm }
        'Recover' { Recover }
    }
} catch [M4LivePowerCutBlocked] {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 77
} catch {
    [Console]::Error.WriteLine($_.Exception.ToString())
    exit 2
}
exit 0
