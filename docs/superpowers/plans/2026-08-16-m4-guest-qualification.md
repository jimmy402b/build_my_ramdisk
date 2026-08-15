# M4 来宾内资格测试状态机实施计划

> **供智能代理执行：** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans`，逐项执行本计划。所有步骤均使用复选框跟踪，所有行为变更均遵循 RED→GREEN TDD。

**目标：** 构建一个直接在 Windows 虚拟机内运行、可跨三次人工 VMware UI 强制重置续跑的 PowerShell 状态机，同时验证进程强杀恢复、运行中断电恢复和 missing-driver 八场景资格测试。

**架构：** 用户只运行 `tools/Invoke-M4GuestQualification.ps1`。控制器把 forward-only 状态和 append-only 事件持久化到本机 NTFS，通过现有 lifecycle runner 执行 Formal 流程，通过独立 live-powercut helper 执行运行中断电流程；现有 Final verifier 保持原语义，并新增独立 `PowerCut` tier。Formal 与 powercut 使用隔离根目录，任何失败进入不可恢复的 `Blocked`。

**技术栈：** Windows PowerShell 5.1、CMake/CTest、现有 `myrd.exe` CLI、现有 lifecycle/fixture/capture/verifier 脚本、Win32/.NET Process API、Windows System Event Log。

---

## 工作区与不变量

- 工作分支：`feature/formal13-guest-qualification`
- 起点设计提交：`70036c4a62e01f8c5fa9a7782cee63caa53b0bf3`
- 设计文档：`docs/superpowers/specs/2026-08-16-m4-guest-qualification-design.md`
- 不在实现阶段连接虚拟机；Task 1–9 全部是纯本地代码、测试、审查和资格包冻结。
- 不修改产品磁盘格式，除非新的 RED 明确证明产品缺陷且用户批准单独设计修订。
- 所有 PowerShell 文件必须通过 Windows PowerShell 5.1 AST 解析，UTF-8 无 BOM。
- 每个任务只提交列出的文件；提交前运行 `git diff --check` 和 staged allowlist。

## 文件职责

- 新建 `tools/Invoke-M4GuestQualification.ps1`：唯一用户入口、状态机、基线/重启门禁、Formal/PowerCut/Negative 编排。
- 新建 `tests/e2e/Invoke-M4LivePowerCut.ps1`：启动并监测真实产品、生成 arm/recover 证据、精确 PID 与 graceful stop。
- 修改 `tools/Invoke-M4Verification.ps1`：新增独立 `PowerCut` tier，不改变 `Final` tier。
- 新建 `tools/New-M4GuestQualificationKit.ps1`：生成 exact qualification kit 与 manifest。
- 新建 `tests/unit/test-m4-guest-qualification.ps1`：控制器状态、基线、三次重置、失败语义与静态安全契约。
- 新建 `tests/unit/test-m4-live-powercut.ps1`：live helper 的协议、证据和 ownership 契约。
- 新建 `tests/unit/test-m4-qualification-kit.ps1`：kit exact inventory、哈希、无覆盖和清理契约。
- 修改 `tests/CMakeLists.txt`：注册新的 Debug/Release `m4-v3` 测试。
- 修改 `README.md`、`RELEASE_NOTES.md`、`M4_TASKLIST.md`：使用中文记录用户操作、限制、RED/GREEN 和最终资格状态。

---

### Task 1：建立 forward-only 状态机与耐断电持久化

**文件：**

- 新建：`tools/Invoke-M4GuestQualification.ps1`
- 新建：`tests/unit/test-m4-guest-qualification.ps1`
- 修改：`tests/CMakeLists.txt`

- [ ] **Step 1：先写缺文件 RED**

在 `tests/unit/test-m4-guest-qualification.ps1` 写入最小入口测试：

```powershell
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceRoot)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$controller=Join-Path $SourceRoot 'tools\Invoke-M4GuestQualification.ps1'
if(-not(Test-Path -LiteralPath $controller -PathType Leaf)){throw 'guest qualification controller is missing'}
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $controller -SelfTest
if($LASTEXITCODE-ne0){throw "controller selftest exit $LASTEXITCODE"}
Write-Output 'M4 guest qualification contract PASS'
```

- [ ] **Step 2：运行 RED 并确认失败原因唯一**

运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .
```

预期：非零退出，唯一根因是 `guest qualification controller is missing`。

- [ ] **Step 3：实现最小参数面、状态枚举和合法转换**

在控制器中加入：

```powershell
[CmdletBinding(DefaultParameterSetName='Run')]
param(
 [Parameter(Mandatory,ParameterSetName='Run')]
 [Parameter(Mandatory,ParameterSetName='Status')][string]$QualificationRoot,
 [Parameter(Mandatory,ParameterSetName='Status')][switch]$Status,
 [Parameter(Mandatory,ParameterSetName='SelfTest')][switch]$SelfTest
)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
class M4QualificationBlocked:System.Exception{M4QualificationBlocked([string]$m):base($m){}}
function Need([bool]$ok,[string]$m){if(-not$ok){throw [M4QualificationBlocked]::new($m)}}
$script:Transitions=[ordered]@{
 Initial='FormalCreateReady';FormalCreateReady='AwaitingCrashReset';AwaitingCrashReset='CrashResetObserved'
 CrashResetObserved='FormalRecovered';FormalRecovered='AwaitingLivePowerCut';AwaitingLivePowerCut='LivePowerCutObserved'
 LivePowerCutObserved='LivePowerRecovered';LivePowerRecovered='FixturesPrepared';FixturesPrepared='AwaitingMissingDriverReset'
 AwaitingMissingDriverReset='MissingDriverResetObserved';MissingDriverResetObserved='MissingDriverCaptured'
 MissingDriverCaptured='DriverRestored';DriverRestored='NegativeComplete';NegativeComplete='Complete'
}
function Assert-Transition([string]$from,[string]$to){Need($script:Transitions.Contains($from))"unknown state: $from";Need($script:Transitions[$from]-ceq$to)"illegal state transition: $from -> $to"}
```

- [ ] **Step 4：先扩展持久化 RED**

在控制器 `SelfTest` 中创建随机 TEMP 根，断言：

```powershell
$state=[ordered]@{schema='myrd.m4.guest-qualification.v1';qualification_id='q1';state='Initial';sequence=0;events=@()}
Write-QualificationState $root $state
$loaded=Read-QualificationState $root
Need($loaded.state-ceq'Initial'-and$loaded.sequence-eq0)'state round trip failed'
Assert-Transition Initial FormalCreateReady
$illegal=$false;try{Assert-Transition Initial Complete}catch[M4QualificationBlocked]{$illegal=$true}
Need $illegal 'illegal transition was accepted'
```

重新运行上一步命令，预期 RED 为 `Write-QualificationState is not recognized`。

- [ ] **Step 5：实现 UTF-8 无 BOM、WriteThrough、Flush(true)、读回与原子替换**

加入精确实现：

```powershell
function Write-DurableUtf8([string]$path,[string]$text){
 $bytes=[Text.UTF8Encoding]::new($false).GetBytes($text)
 $s=[IO.FileStream]::new($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough)
 try{$s.Write($bytes,0,$bytes.Length);$s.Flush($true)}finally{$s.Dispose()}
}
function Write-QualificationState([string]$root,$state){
 $path=Join-Path $root 'guest-controller-state.json';$tmp="$path.$([guid]::NewGuid().ToString('N')).tmp"
 $json=$state|ConvertTo-Json -Depth 16 -Compress
 try{Write-DurableUtf8 $tmp $json;$round=[IO.File]::ReadAllText($tmp,[Text.UTF8Encoding]::new($false))|ConvertFrom-Json;Need($round.schema-ceq'myrd.m4.guest-qualification.v1')'state readback schema mismatch';if([IO.File]::Exists($path)){[IO.File]::Replace($tmp,$path,$null)}else{[IO.File]::Move($tmp,$path)}}finally{if([IO.File]::Exists($tmp)){[IO.File]::Delete($tmp)}}
}
function Read-QualificationState([string]$root){$p=Join-Path $root 'guest-controller-state.json';Need([IO.File]::Exists($p))'qualification state is absent';$x=[IO.File]::ReadAllText($p,[Text.UTF8Encoding]::new($false))|ConvertFrom-Json;Need($x.schema-ceq'myrd.m4.guest-qualification.v1')'qualification state schema mismatch';$x}
```

append-only `events.jsonl` 使用相同 `FileOptions.WriteThrough`，每条 JSON 加 CRLF 并 `Flush(true)`；测试必须重新打开文件并按序验证 `sequence`。

- [ ] **Step 6：注册并运行 GREEN**

在 `tests/CMakeLists.txt` 加入：

```cmake
add_test(NAME unit.m4.guest_qualification
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/test-m4-guest-qualification.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}")
set_tests_properties(unit.m4.guest_qualification PROPERTIES LABELS "m4-v3" TIMEOUT 30)
```

运行 direct test 和 Debug/Release focused CTest，预期全部 PASS：

```powershell
ctest.exe --preset debug -R '^unit\.m4\.guest_qualification$' --output-on-failure
ctest.exe --preset release -R '^unit\.m4\.guest_qualification$' --output-on-failure
```

- [ ] **Step 7：提交 Task 1**

```powershell
git add tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-guest-qualification.ps1 tests/CMakeLists.txt
git diff --cached --check
git commit -m "test(m4): add guest qualification state core"
```

---

### Task 2：资格包生成、exact inventory 与冻结哈希

**文件：**

- 新建：`tools/New-M4GuestQualificationKit.ps1`
- 新建：`tests/unit/test-m4-qualification-kit.ps1`
- 修改：`tests/CMakeLists.txt`
- 修改：`tools/Invoke-M4GuestQualification.ps1`

- [ ] **Step 1：写 packager 缺失 RED**

测试在随机 scratch 中创建七个假输入文件，调用 packager 后要求 exact 八文件：

```powershell
$expected=@('Capture-M4Child.ps1','Invoke-M4GuestQualification.ps1','Invoke-M4RunLifecycle.ps1','Invoke-M4Verification.ps1','Prepare-M4NegativeFixtures.ps1','myrd.exe','myrd_m4_aim_query.exe','qualification-kit.json')
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packager -SourceRoot $fake -Artifact $artifact -Query $query -OutputDirectory $out
if($LASTEXITCODE-ne0){throw "kit packager exit $LASTEXITCODE"}
$actual=@(Get-ChildItem -LiteralPath $out -File|Sort-Object Name|ForEach-Object Name)
if((ConvertTo-Json $actual -Compress)-cne(ConvertTo-Json $expected -Compress)){throw 'qualification kit inventory mismatch'}
```

运行 direct test，预期因 `New-M4GuestQualificationKit.ps1` 缺失而 RED。

- [ ] **Step 2：实现无覆盖、同目录临时 stage 和 manifest**

packager 参数固定为：

```powershell
param([Parameter(Mandatory)][string]$SourceRoot,[Parameter(Mandatory)][string]$Artifact,[Parameter(Mandatory)][string]$Query,[Parameter(Mandatory)][string]$OutputDirectory)
```

manifest 使用：

```powershell
$manifest=[ordered]@{
 schema='myrd.m4.qualification-kit.v1'
 source_commit=((&git.exe -C $SourceRoot rev-parse HEAD)-join'').Trim().ToLowerInvariant()
 files=@($names|Sort-Object|ForEach-Object{[ordered]@{name=$_;length=[int64](Get-Item -LiteralPath (Join-Path $stage $_)).Length;sha256=(Get-FileHash -LiteralPath (Join-Path $stage $_) -Algorithm SHA256).Hash}})
}
```

要求输出目录及 `.$name.stage-*` 均预先不存在；失败时只删除本次 stage，绝不覆盖或删除既有输出。

- [ ] **Step 3：先写 controller kit 校验 RED，再实现 exact 校验**

SelfTest 依次突变 missing/extra/length/hash/source_commit，要求 `Assert-QualificationKit` 全部拒绝。实现接口：

```powershell
function Assert-QualificationKit([string]$kit,[string]$expectedCommit){
 $manifest=Get-Content -LiteralPath (Join-Path $kit 'qualification-kit.json') -Raw -Encoding UTF8|ConvertFrom-Json
 Need($manifest.schema-ceq'myrd.m4.qualification-kit.v1'-and$manifest.source_commit-ceq$expectedCommit)'kit identity mismatch'
 $expected=@('Capture-M4Child.ps1','Invoke-M4GuestQualification.ps1','Invoke-M4RunLifecycle.ps1','Invoke-M4Verification.ps1','Prepare-M4NegativeFixtures.ps1','myrd.exe','myrd_m4_aim_query.exe','qualification-kit.json')
 $actual=@(Get-ChildItem -LiteralPath $kit -File|Sort-Object Name|ForEach-Object Name)
 Need((ConvertTo-Json $actual -Compress)-ceq(ConvertTo-Json $expected -Compress))'kit exact inventory mismatch'
 $manifestNames=@($manifest.files|ForEach-Object name|Sort-Object)
 Need((ConvertTo-Json $manifestNames -Compress)-ceq(ConvertTo-Json @($expected|Where-Object{$_-cne'qualification-kit.json'}|Sort-Object) -Compress))'kit manifest file list mismatch'
 foreach($f in @($manifest.files)){ $p=Join-Path $kit $f.name;Need((Get-Item -LiteralPath $p).Length-eq$f.length)'kit length mismatch';Need((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash-ceq$f.sha256)'kit hash mismatch' }
}
```

manifest 不为自身记录哈希，测试要求 `manifest.files` 正好七个非 manifest 文件，避免自引用。

- [ ] **Step 4：验证碰撞保持与 UTF-8 无 BOM**

分别预建输出目录和同名 stage sentinel；调用必须非零，原目录完整 inventory/hash 不变。读取 JSON 首三字节，断言不是 `EF BB BF`。

- [ ] **Step 5：运行 GREEN 并提交**

注册 `unit.m4.qualification_kit` 为 `m4-v3`，执行 Debug/Release focused CTest 后提交：

```powershell
git add tools/New-M4GuestQualificationKit.ps1 tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-qualification-kit.ps1 tests/CMakeLists.txt
git commit -m "build(m4): add frozen guest qualification kit"
```

---

### Task 3：来宾预检、bounded native capture 与异常重启证明

**文件：**

- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`

- [ ] **Step 1：写事实分类 RED**

SelfTest 构造一份通过样本和逐字段突变：非管理员、错误 model/MAC、非 NTFS root、服务非 Disabled/Stopped、driver 非 DEMAND/RUNNING、相关进程存在、AIM IDs 非空、R/S 存在、PD inventory 变化。调用缺失的：

```powershell
Assert-InitialGuestFacts $goodFacts
foreach($bad in $mutations){$failed=$false;try{Assert-InitialGuestFacts $bad}catch[M4QualificationBlocked]{$failed=$true};Need $failed 'bad guest facts were accepted'}
```

预期 RED：`Assert-InitialGuestFacts is not recognized`。

- [ ] **Step 2：实现纯分类函数与实际采样函数分离**

`Get-GuestFacts` 只采集；`Assert-InitialGuestFacts` 只判断。事实对象固定字段：

```powershell
[ordered]@{admin=$admin;boot_utc=$boot;model=$model;bios=$bios;mac=$mac;root_ntfs=$rootNtfs;service_start=$svcStart;service_state=$svcState;service_pid=$svcPid;driver_start=$drvStart;driver_state=$drvState;processes=@($processes);aim_ids=@($ids);R=$r;S=$s;physical_disks=@($disks)}
```

PD exact 字符串固定为：

```text
0|68719476736|GPT|4
1|68719476736|GPT|2
2|8589934592|RAW|0
```

只有 `Assert-InitialGuestFacts` 和源 kit 校验通过后，控制器才创建此前不存在的 `QualificationRoot`。使用 `[IO.Directory]::CreateDirectory()` 创建 `formal\Release`、`formal\tests\e2e`、`formal\tools`、`powercut`、`evidence`、`logs`、`temp`、`kit`；以 `[IO.File]::Copy(source,destination,$false)` 将源 kit 快照到 `QualificationRoot\kit`，再做一次 exact inventory/hash 校验。部署供既有 runner 使用的固定布局：

```text
formal\Release\myrd.exe
formal\tests\e2e\Invoke-M4RunLifecycle.ps1
formal\tools\Invoke-M4Verification.ps1
```

`formal\Release\myrd.exe` 是现有 Final verifier 的 `HostArtifact`；其他 helper 从只读 `QualificationRoot\kit` 调用。任何目标文件预先存在都失败，不使用覆盖参数。

- [ ] **Step 3：写 bounded process RED 并实现 async 双流 capture**

测试启动一个 stdout/stderr 各写一行并 exit 23 的 PowerShell child，以及一个超时 child。接口固定为：

```powershell
Invoke-BoundedProcess -FilePath $exe -Arguments $argv -TimeoutMs 30000
```

返回：

```powershell
[ordered]@{pid=$p.Id;exit=$p.ExitCode;timed_out=$false;stdout=$stdout;stderr=$stderr;started_utc=$started;ended_utc=$ended}
```

实现必须设置 `UseShellExecute=$false`、同时调用 `ReadToEndAsync()`、有界 `WaitForExit()`，超时只终止精确 PID，并在返回前等待双流任务完成。Windows 参数 quoting 函数必须覆盖空串、空格、反斜杠和双引号样本。

- [ ] **Step 4：写异常关机 RED 并实现 Event 41/6008 门禁**

测试样本必须包含：新 BootUtc、armed_utc 后的 Kernel-Power 41、EventLog 6008。分别移除 41、移除 6008、保持旧 BootUtc、把事件时间放到 armed_utc 前，均须拒绝。

接口：

```powershell
function Assert-UnexpectedReset($armed,$post){
 $before=[datetime]$armed.boot_utc;$after=[datetime]$post.boot_utc;$armedUtc=[datetime]$armed.armed_utc
 Need($after-gt$before)'boot UTC did not advance'
 Need(@($post.system_events|Where-Object{id-eq41-and[datetime]$_.time_utc-gt$armedUtc}).Count-gt0)'Kernel-Power 41 is absent'
 Need(@($post.system_events|Where-Object{id-eq6008-and[datetime]$_.time_utc-gt$armedUtc}).Count-gt0)'EventLog 6008 is absent'
}
```

- [ ] **Step 5：运行 focused GREEN 并提交**

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .
ctest.exe --preset debug -R '^unit\.m4\.guest_qualification$' --output-on-failure
ctest.exe --preset release -R '^unit\.m4\.guest_qualification$' --output-on-failure
git add tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-guest-qualification.ps1
git commit -m "feat(m4): gate guest qualification environment"
```

---

### Task 4：编排现有 Formal CreateKill、Reset1、Recover 与 Offline

**文件：**

- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`

- [ ] **Step 1：写 no-ack 与 CreateKill 顺序 RED**

给控制器提供注入式 `$script:ProcessInvoker`，SelfTest 使用 fake 记录 argv。断言精确顺序：四次 no-ack（20,000ms）后才允许一次 ACK CreateKill（420,000ms）。四个 no-ack 必须逐个验证：exit 77、精确单行 CRLF、stderr 空、状态/根/inventory/AIM 不变。

预期 RED：`Invoke-FormalCreateSequence is not recognized`。

- [ ] **Step 2：实现 runner capture 适配器**

构造参数必须直接调用 kit 中 `Capture-M4Child.ps1 -Mode Runner`，并传：

```powershell
@('-NoProfile','-ExecutionPolicy','Bypass','-File',$capture,'-Mode','Runner','-ChildFile',$runner,'-SourceCommit',$commit,'-Phase',$phase,'-Executable',$artifact,'-Bundle',$formalBundle,'-DriveLetter','R','-StateFile',$formalState,'-TimeoutMs',[string]$timeout)
```

ACK 时只追加 `-SafetyAcknowledged`。禁止调用 `Invoke-Expression` 或字符串命令行。

- [ ] **Step 3：实现 Reset1 arm 的 flush-before-message 契约**

CreateKill 通过后采集并持久化：pre boot、armed UTC、evidence SHA、exact killed PID、formal bundle inventory、residual owned AIM identity。顺序测试使用事件钩子，必须为：

```text
WriteState -> FlushEvent -> ReReadState -> VerifyEvidenceHash -> PrintCrashResetArmed
```

打印前状态必须已经是 `AwaitingCrashReset`。SelfTest 把每个前置钩子改为失败，断言永不出现 `CRASH RESET ARMED`。

- [ ] **Step 4：实现重启后 exact postgate、Recover 和 Offline**

若 BootUtc 未增加，返回：

```text
RESET STILL REQUIRED: CrashReset
```

退出 10，且状态文件 SHA 不变。BootUtc 增加时调用 `Assert-UnexpectedReset`，验证 Create evidence SHA、kit、PID、AIM、R/S、PD，然后执行 ACK RecoverShutdown 与 OfflineCompact，各一次。验证现有 evidence schema、状态 PASS、generation 后继、A/D hash 和 clean inventory，再转到 `FormalRecovered`。

- [ ] **Step 5：运行 GREEN、完整 m4-v3 并提交**

```powershell
ctest.exe --preset debug -L m4-v3 --output-on-failure
ctest.exe --preset release -L m4-v3 --output-on-failure
git add tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-guest-qualification.ps1
git commit -m "feat(m4): orchestrate crash recovery lifecycle"
```

---

### Task 5：实现运行中断电 arm/recover helper

**文件：**

- 新建：`tests/e2e/Invoke-M4LivePowerCut.ps1`
- 新建：`tests/unit/test-m4-live-powercut.ps1`
- 修改：`tests/CMakeLists.txt`

- [ ] **Step 1：写 helper 缺失 RED 与参数契约**

测试要求脚本参数：

```powershell
param(
 [Parameter(Mandatory)][ValidateSet('Arm','Recover','SelfTest')][string]$Mode,
 [string]$Executable,[string]$Bundle,[string]$DriveLetter='R',[string]$EvidenceDirectory,
 [string]$SourceCommit,[string]$ArmEvidence,[ValidateRange(1000,600000)][int]$TimeoutMs=120000
)
```

运行 `-Mode SelfTest`，预期最初因文件缺失 RED。

- [ ] **Step 2：先写协议 parser RED，再实现 CRLF/LF/EOF 严格解析**

覆盖：

```text
Ready: R: generation 1.
Checkpoint committed generation 2.
```

接受 CRLF/LF/EOF，拒绝 generation 0、错误盘符、尾空格、垃圾、bare CR、UInt64 overflow。实现使用 `[uint64]::TryParse` 和 `(?=\r?\n|\z)`，不能依赖 `$Matches` 全局状态。

- [ ] **Step 3：实现 Arm 真实 child 与证据**

Arm 使用 `System.Diagnostics.ProcessStartInfo` 直接启动 `myrd.exe run`，stdout/stderr 重定向到 powercut root 的固定 owned files。argv 精确为：

```powershell
@('run','--bundle',$Bundle,'--size','64M','--drive-letter','R:','--checkpoint-seconds','5','--checkpoint-dirty-mb','1','--ram-limit','64M')
```

等待 Ready、NTFS/GPT/volume identity、写入并回读 A/D、等待 successor checkpoint。写 `myrd.m4.live-powercut-arm.v1`，返回 PID、generation、bundle inventory 和日志路径，但不终止 child。

SelfTest 使用 harmless PowerShell child 模拟 Ready/checkpoint，并断言 helper 返回时 exact PID 仍 alive；stderr、早退、无 successor、错误 PID 均拒绝。

- [ ] **Step 4：实现 Recover 与 graceful CtrlBreak**

Recover 读取 arm evidence，要求同 bundle/source/artifact。启动 exact child，Ready generation 必须大于 armed generation；验证 A/D，新增并验证 post-recovery workload，发送精确 process-group CtrlBreak，等待 final successor checkpoint 和 exit 0，恢复 clean inventory。

复用 runner 中已审计的 `M4Child`/`GenerateConsoleCtrlEvent` 模式，不 broad broadcast，不以进程名杀进程。输出 `myrd.m4.live-powercut.v1`，schema 字段固定为设计文档列出的 armed/recovered boot、events、PID、generation、inventory、volume、hash、exit 与 clean inventory。

- [ ] **Step 5：测试 arm 后进程早退与异常 shutdown**

测试必须证明：arm 后 child 退出会使 controller 后续判定 Blocked；没有 event 41/6008 的 Recover evidence 不能成为 PASS；bundle/source/artifact 任一变化被拒绝。

- [ ] **Step 6：注册、运行 GREEN 并提交**

```cmake
add_test(NAME unit.m4.live_powercut
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/unit/test-m4-live-powercut.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}")
set_tests_properties(unit.m4.live_powercut PROPERTIES LABELS "m4-v3" TIMEOUT 60)
```

运行 Debug/Release focused 后提交：

```powershell
git add tests/e2e/Invoke-M4LivePowerCut.ps1 tests/unit/test-m4-live-powercut.ps1 tests/CMakeLists.txt
git commit -m "test(m4): add live power-cut lifecycle"
```

---

### Task 6：把 live powercut 接入控制器并冻结 Reset2

**文件：**

- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`

- [ ] **Step 1：写双根隔离与 arm 顺序 RED**

SelfTest 断言 Formal path 与 PowerCut path 无父子/相等混淆；Formal inventory 在整个 live 场景前后完全相同。fake helper 返回 live PID 后，控制器顺序必须是：

```text
Arm helper PASS -> Write AwaitingLivePowerCut -> Flush event -> Re-read -> Verify PID alive -> Print LIVE POWER-CUT ARMED -> monitor PID
```

预期 RED：`Invoke-LivePowerCutArm is not recognized`。

- [ ] **Step 2：实现 armed wait 与误运行语义**

控制器每十秒检查 exact PID/start time，打印同一简短 armed 状态。PID 提前退出立即写 `Blocked`。在 boot 未增加时再次运行控制器，只打印 `RESET STILL REQUIRED: LivePowerCut`，退出 10，绝不启动新产品。

- [ ] **Step 3：实现重启后 Recover 接线**

BootUtc 增加后先执行 event 41/6008、kit、AIM、R/S、PD、old PID、arm evidence 和 bundle identity 门禁，再调用 helper `-Mode Recover`。验证 `myrd.m4.live-powercut.v1` 后转到 `LivePowerRecovered`。

- [ ] **Step 4：运行 focused 与完整 m4-v3 后提交**

```powershell
ctest.exe --preset debug -R '^unit\.m4\.(guest_qualification|live_powercut)$' --output-on-failure
ctest.exe --preset release -R '^unit\.m4\.(guest_qualification|live_powercut)$' --output-on-failure
git add tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-guest-qualification.ps1
git commit -m "feat(m4): resume live power-cut qualification"
```

---

### Task 7：编排 missing-driver Reset3 与剩余七场景

**文件：**

- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`

- [ ] **Step 1：写 exact fixture 与 driver sequence RED**

SelfTest fake invoker 冻结顺序：clean baseline → Prepare normal → exact manifest/inventories → `sc.exe config phdskmnt start= disabled` 一次 → readback Disabled → flush AwaitingMissingDriverReset → print reset3。禁止 `sc stop`、driver delete、第二次 config disabled。

- [ ] **Step 2：实现 fixture 校验**

复用 Prepare 的 exact case 顺序：

```powershell
@('unknown_nonempty','size_mismatch','occupied_letter','gen0','writer_lock','missing_driver','busy_volume','compact_lock')
```

校验 exact property sets、derived paths、S/D letters、`holder.bin`、64M sizes、四份 compact copy 完整 inventory 和 gen0 copy 完整 inventory。Prepare 前后 main/gen0 source inventory 必须不变。

- [ ] **Step 3：实现 Reset3 postgate 和 partial missing_driver**

重启后要求 BootUtc/event 41/6008、driver Disabled/Stopped、query exact exit 1/stdout empty/stderr `discover_aim_adapter failed`、R/S absent、PD exact。调用一次 ACK Negative；预期 exit 77 和精确 remaining-cases 行，并保存 `m4-negative-missing_driver.v1.json`。

- [ ] **Step 4：实现一次 restore 与七场景**

精确调用：

```text
sc.exe config phdskmnt start= demand
sc.exe start phdskmnt
```

验证 DEMAND_START/Running、AIM IDs 空、R/S absent、PD exact。重新哈希 Prepare helper，调用 `-ActivateRemaining`，验证 active_cases 七项顺序，执行一次 ACK Negative exit 0。restore 失败立即 Blocked，绝不执行第四次 reset。

- [ ] **Step 5：运行 GREEN 并提交**

```powershell
ctest.exe --preset debug -L m4-v3 --output-on-failure
ctest.exe --preset release -L m4-v3 --output-on-failure
git add tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-guest-qualification.ps1
git commit -m "feat(m4): qualify missing-driver recovery"
```

---

### Task 8：新增独立 PowerCut verifier tier 与总 PASS 门禁

**文件：**

- 修改：`tools/Invoke-M4Verification.ps1`
- 修改：`tools/Invoke-M4GuestQualification.ps1`
- 修改：`tests/unit/test-m4-live-powercut.ps1`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`
- 修改：`tests/CMakeLists.txt`

- [ ] **Step 1：写 verifier Tier 缺失 RED**

把测试生成的完整 `myrd.m4.live-powercut.v1` 传入，并显式使用 kit manifest 的 source commit：

```powershell
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $verifier -Tier PowerCut -EvidenceDirectory $evidence -HostArtifact $artifact -ExpectedSourceCommit $commit
```

预期 RED：ValidateSet 拒绝 `PowerCut`。

- [ ] **Step 2：实现 `Verify-PowerCut`，不改 `Verify-Final`**

扩展 Tier ValidateSet 为 `...,Final,PowerCut`，并新增：

```powershell
[ValidatePattern('^$|^[0-9A-Fa-f]{40}$')][string]$ExpectedSourceCommit=''
```

加入 `Resolve-ExpectedCommit`：显式值存在时返回其 lowercase；为空时维持现有行为，从 `$root` 的 git HEAD 读取。控制器在来宾内对 `Final` 和 `PowerCut` 都必须传 kit manifest 的 exact commit；原有本地/host 调用不传参数，行为不变。mutation test 必须证明空值继续绑定 git HEAD、错误 commit 被拒绝、显式 exact commit 可在无 `.git` 的合成根中通过。

`Verify-PowerCut` 必须验证：

```powershell
Need($x.schema-eq'myrd.m4.live-powercut.v1'-and$x.status-eq'PASS')'PowerCut schema/status invalid'
Need($x.source_commit-eq$commit-and$x.executable_sha256-eq(Get-FileHash $HostArtifact -Algorithm SHA256).Hash)'PowerCut identity invalid'
Need([datetime]$x.recovered_boot_utc-gt[datetime]$x.armed_boot_utc)'PowerCut boot did not advance'
Need(@($x.shutdown_events|? id-eq41).Count-gt0-and@($x.shutdown_events|? id-eq6008).Count-gt0)'PowerCut unexpected-shutdown evidence missing'
Need([uint64]$x.recovered_generation-gt[uint64]$x.armed_generation)'PowerCut generation did not advance'
Need($x.old_pid-gt0-and$x.old_pid_absent-and$x.graceful_exit-eq0)'PowerCut process evidence invalid'
Need($x.hashes_before.'A.txt'-eq$x.hashes_after.'A.txt'-and$x.hashes_before.'D.txt'-eq$x.hashes_after.'D.txt')'PowerCut workload hash changed'
Need((Stable $x.inventory_after)-eq(Stable $x.baseline_inventory))'PowerCut baseline was not restored'
```

- [ ] **Step 3：添加 mutation matrix RED/GREEN**

逐项突变 schema/status/source/hash/boot/events/generation/PID/hash/inventory，必须全部失败。移除 powercut evidence、添加额外 evidence 也必须失败。

- [ ] **Step 4：实现控制器总门禁**

控制器先调用现有 `-Tier Final -ExpectedSourceCommit $commit`，再调用 `-Tier PowerCut -ExpectedSourceCommit $commit`。两者的 `HostArtifact` 都是 `formal\Release\myrd.exe`。两者及 final clean baseline 全部通过后，原子写 `Complete` 并且只输出：

```text
M4 v1 QUALIFICATION PASS
```

任一 verifier 非零必须写 Blocked 且不打印 PASS。

- [ ] **Step 5：运行 GREEN 并提交**

```powershell
ctest.exe --preset debug -R '^unit\.m4\.(lifecycle_final_verifier|live_powercut|guest_qualification)$' --output-on-failure
ctest.exe --preset release -R '^unit\.m4\.(lifecycle_final_verifier|live_powercut|guest_qualification)$' --output-on-failure
git add tools/Invoke-M4Verification.ps1 tools/Invoke-M4GuestQualification.ps1 tests/unit/test-m4-live-powercut.ps1 tests/unit/test-m4-guest-qualification.ps1 tests/CMakeLists.txt
git commit -m "feat(m4): verify live power-cut evidence"
```

---

### Task 9：中文文档、静态安全契约与本地冻结

**文件：**

- 修改：`README.md`
- 修改：`RELEASE_NOTES.md`
- 修改：`M4_TASKLIST.md`
- 修改：`tests/unit/test-m4-guest-qualification.ps1`
- 修改：`tests/unit/test-m4-qualification-kit.ps1`

- [ ] **Step 1：先写中文文档缺失 RED**

契约测试要求 README/notes 包含：来宾管理员 PowerShell 命令、同一 `QualificationRoot` 重运行、三次且仅三次 UI Reset/Power Off、三个 armed 文案、禁止正常 shutdown、`-Status`、退出 10/77/0、root/evidence 保留。

运行 direct test，预期对第一个缺失中文短语 RED。

- [ ] **Step 2：补齐中文用户文档与账本**

README 给出单一复制运行命令和三次人工操作表。RELEASE_NOTES 明确该脚本是资格测试工具，不进入 v1 六文件发行 ZIP。M4_TASKLIST 记录每个 genuine RED/GREEN、提交、测试和“真实 VM 尚未启动”的状态。

- [ ] **Step 3：加入静态禁止项**

测试递归扫描控制器、live helper、kit packager，拒绝以下 token/AST 行为：

```text
ssh.exe
scp.exe
vmrun
shutdown.exe
Restart-Computer
Stop-Computer
Register-ScheduledTask
schtasks
Format-Volume
Initialize-Disk
New-Partition
remove_device
Remove-Item $QualificationRoot
password
private key
```

允许文档在“禁止”语境出现这些词；代码扫描必须零命中。

- [ ] **Step 4：运行完整 Debug/Release 本地验证**

```powershell
cmake --preset msvc
cmake --build --preset debug --parallel 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\Invoke-M4Verification.ps1 -Tier Local -Configuration Debug
cmake --build --preset release --parallel 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\Invoke-M4Verification.ps1 -Tier Local -Configuration Release
```

预期两次精确输出：

```text
M4 Local PASS: Debug m4-v0..v3
M4 Local PASS: Release m4-v0..v3
```

- [ ] **Step 5：独立 SPEC 与 QUALITY review**

SPEC reviewer 对照设计逐条检查三次状态转换、双根隔离、证据 schema、Final 不弱化。QUALITY reviewer 检查进程 ownership、Flush(true)、事件日志窗口、Ctrl+C、timeout、无重试、secret/format/driver-delete 静态边界。任一 Critical/Important 必须先 TDD 修复再复审。

- [ ] **Step 6：提交文档与契约**

```powershell
git add README.md RELEASE_NOTES.md M4_TASKLIST.md tests/unit/test-m4-guest-qualification.ps1 tests/unit/test-m4-qualification-kit.ps1
git diff --cached --check
git commit -m "docs(m4): document guest qualification workflow"
```

- [ ] **Step 7：冻结新的本地资格候选，不运行虚拟机**

在 tracked worktree clean 后运行 packager，输出到：

```text
out\qualified\<new-head>\guest-qualification-kit\
```

重新解析 `qualification-kit.json`，逐文件复核 length/SHA、UTF-8 无 BOM、exact inventory；记录 kit manifest SHA 和 `myrd.exe` SHA。确认 `git status --porcelain=v1` 仍为空。

- [ ] **Step 8：停止在真实 VM 门前并交给用户**

报告新 HEAD、kit 路径、manifest SHA、完整本地测试与双 review 结果。明确尚未执行来宾脚本。只有用户再次确认后，才把 kit 复制到虚拟机并开始三次人工 UI 强制重置的真实资格测试。

---

## 计划自审结果

- 设计中的两个根、三个 reset、forward-only state、event 41/6008、PowerCut 独立 verifier、missing-driver restore、Final 和本地 freeze 均有对应 Task。
- 所有生产行为在实现前都有可观察 RED；没有以静态 regex 代替核心状态机或 evidence mutation tests。
- `Invoke-M4GuestQualification.ps1` 是唯一用户入口；live helper、packager 和 verifier 均是单一职责。
- Task 1–9 不授权 SSH、虚拟机操作或实际设备变更；真实 VM 执行是单独的用户确认门。
- 文件名、参数名、state 名和 evidence schema 在所有任务中保持一致。
