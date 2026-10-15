# M5 虚拟机测试回路（T1-01）

状态：**已在真实客机上验证（2026-10-14）**，见 §8 的实测结果与踩到的坑
位置：`tools/m5vm/`
对应：设计 v3 §13 T1-01；`AGENTS.md` §4（测试规则）、§5（虚拟机接入）

---

## 0. 实测结论：三条通道，各管一段

2026-10-14 在这台客机（`Windows 11 x64`，MAC `00:0C:29:02:90:C1`，账户 `v\usr`）上实测：

| 通道 | 提权 | 令牌里有 NETWORK SID (S-1-5-2) | 用途 |
|---|---|---|---|
| **OpenSSH** | ✅ High Mandatory Level | ❌ **有** | 传输、探测、编排；**不能**碰产品自己的控制管道 |
| **计划任务**（vmrun 创建，SSH 触发） | ✅ High Mandatory Level | ✅ **无**（是 `NT AUTHORITY\BATCH`，类型 4） | **全套测试 + 产品命令** |
| **vmrun** 客机内执行 | ✅（经 Tools 服务代理，绕过 UAC） | ✅ 无 | 客机一次性配置、电源、快照、截屏 |

**纠正一条流传下来的判断**：§1 说"SSH 会话不提权"是**错的**——实测 SSH 拿到的是完整令牌
（`elevated: True`）。SSH 真正的问题是**登录类型**：令牌含 `S-1-5-2`，所以公共管道 SDDL 里的
`(D;;GA;;;NU)` 会拒绝它。这解释了 handoff 里"`myrd.exe` 经 SSH 跑永远答 supervisor service
is unreachable"——原因不是权限，是登录类型。

---

## 1. 为什么不能"ssh 直接跑测试"

两条硬要求，一个 SSH 会话无法满足：

| 要求 | 证据 |
|---|---|
| 产品命令与设备测试需要**提权令牌** | `src/run/runtime_adapter.cpp:147` 的 `require_elevated_administrator()` 检查 `TokenElevation`；`src/run/main.cpp:82` 在入口处就调用它。**SSH 满足这一条** |
| 公共控制管道**拒绝 Network 登录** | `src/control/authorization.cpp:455` 的 SDDL 含 `(D;;GA;;;NU)`。**SSH 不满足这一条** |

**所以本回路把 SSH 只当传输用，执行交给客机上以最高权限、非网络登录方式运行的 agent。**

---

## 2. 架构

```
开发机                                     客户机
──────                                     ──────
构建（Release/Debug）
  │
  ├─ ctest --show-only=json-v1  → tests.json（唯一权威的测试清单）
  │                                 并把 command[0] 改写成客机 staged 路径
  │
  ├─ scp  ──────────────────────────▶  C:\m5\control\{tests.json, tests\, src\}
  │                                     （目录布局必须保持：测试二进制按相对路径找 worker）
  │
  └─ scp  <id>.request.ps1  ────────▶  C:\m5\control\
                                          │
                        ssh: schtasks /run /tn MyRamDisk-M5-Agent
                                          │
                              ┌───────────▼────────────┐
                              │ 计划任务（最高权限、    │
                              │ 非交互、非网络登录）    │
                              │  → m5-agent.ps1 -Drain  │
                              │     → Invoke-M5Tests.ps1│
                              │       串行执行每个测试  │
                              └───────────┬────────────┘
                                          │
      scp  <id>.response.txt  ◀───────────┤  C:\m5\control\<id>.done（最后写）
      scp  <id>.results.json  ◀───────────┘  + run-<id>\*.stdout/stderr.txt
```

**为什么必须串行**：这些用例会创建机器全局的 `PhysicalDrive` 设备。上一轮量过（transfer
manifest §1.39）：重叠执行会让同一台机器上的多个用例争同一个设备命名空间，表现为
"create 找不到磁盘接口"或"移除超时"。

---

## 3. 一次性客机准备

以下步骤需要**在客机上以管理员身份**做一次（可以走客机控制台，或一次性提权会话）。

### 3.1 安装公钥（让 ssh 非交互可用）

开发机的公钥：`%USERPROFILE%\.ssh\id_ed25519.pub`。

在**客机**上执行（把 `<pubkey>` 换成公钥内容）：

```powershell
$dir = "$env:USERPROFILE\.ssh"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Add-Content -Path "$dir\authorized_keys" -Value '<pubkey>' -Encoding ascii
# 只有本人可读，sshd 会检查这一点
icacls "$dir\authorized_keys" /inheritance:r /grant:r "$($env:USERNAME):(R)" | Out-Null
```

> 不用 `ssh-copy-id`（Windows OpenSSH 没有），也不在脚本里放明文密码（`AGENTS.md` §5）。

### 3.2 放置 agent 脚本

```powershell
New-Item -ItemType Directory -Force -Path 'C:\m5\agent', 'C:\m5\control' | Out-Null
```

把 `tools/m5vm/guest/m5-agent.ps1` 与 `tools/m5vm/guest/Invoke-M5Tests.ps1` 复制到
`C:\m5\agent\`。

### 3.3 建计划任务

用**管理员用户**（不要用 SYSTEM）——产品的授权模型按 administrator/owner SID 判定，
SYSTEM 是另一个主体，会让授权类用例的行为与真实使用不一致。

```powershell
schtasks /create /tn MyRamDisk-M5-Agent /f `
  /tr "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\m5\agent\m5-agent.ps1 -Drain" `
  /sc once /st 00:00 `
  /ru <管理员用户名> /rp <该用户密码> /rl highest
```

- `/rl highest` 给提权令牌；`/rp` 使任务"无论用户是否登录都运行"，因此它**不是** Network
  登录，不受 `(D;;GA;;;NU)` 影响。
- 任务命令行固定，所以用 `-Drain`（处理所有未完成请求后退出）而不是传请求 id。

**备选路径**（如果任务方式在授权类用例上出现意外差异）：沿用上一轮的做法——在客机
交互式提权桌面上常驻运行 `m5-agent.ps1`（不带参数，轮询模式），SSH 只做传输。

### 3.4 前置条件

- `phdskmnt` 已安装并在运行（本回路**只检查不安装**，见设计 v3 §12）。
- 客机 `C:` 至少有装下 staged 测试树的空间（约几十 MB）。

---

## 4. 日常用法

```powershell
# 0) 一次性：把地址/用户/密钥放到环境变量，或每次传参
$env:M5VM_HOST = '<地址或别名>'
$env:M5VM_USER = '<用户名>'
$env:M5VM_KEY  = "$env:USERPROFILE\.ssh\id_ed25519"

# 1) 连通性与环境探测（不改客机任何状态）
.\tools\m5vm\Invoke-M5Vm.ps1 -Probe

# 2) 构建后把测试树送进客机
cmake --build out/build/msvc --config Release
.\tools\m5vm\Invoke-M5Vm.ps1 -Stage -Configuration Release

# 3) 触发执行并取回结果
.\tools\m5vm\Invoke-M5Vm.ps1 -Run
```

产物落在 `C:\tmp\m5-vm\`：`probe.txt`、`<id>.response.txt`、`<id>.results.json`。

`-Probe` 会报告：计算机名、用户、**是否提权**、登录类型、会话号、OS 版本、PowerShell
版本、`phdskmnt` 状态、物理盘数量（60 秒上限）、`C:` 剩余空间、`R:`/`S:` 是否存在。
**第一件要看的就是 `elevated True`** —— 它同时验证了计划任务配好了。

---

## 5. T1-01 的退出标准与对应

| 退出标准 | 由什么满足 |
|---|---|
| 回路可用：构建在开发机、执行在客机 | `-Stage` + `-Run` |
| 当前 HEAD 的真实全套读数写入 `docs/validation/m5-current-state.md` | `-Run` 的 `results.json` 按测试名给出逐条结论；把它整理成该文档 |
| 明确区分"开发机可跑"与"只能上 VM" | `AGENTS.md` §4 的判定标准；本轮实测会给出真实分类 |

---

## 6. 客机 IP 固化：已完成（2026-10-14）

**地址：`192.168.180.129`**，主机别名 **`myrd-vm`**。此后脚本参数一律传 `myrd-vm`。

### 为什么之前会漂

`.vmx` 里**没有** `ethernet0.connectionType`，而客机当时实际是**桥接**：地址从宿主机所在的
物理网络取（实测拿到 `10.0.0.226`，网关 10.0.0.1）。宿主机换网络，客机地址就跟着变。

（历史 DHCP 租约里 `192.168.180.129` 出现在 VMnet8 的池里，所以这台机器以前也在 NAT 上待过；
`connectionType` 缺失时 Workstation 取默认值，而默认值在此版本上是桥接。）

### 做法（三步，需要管理员；改完要关一次机）

```powershell
$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
$vmx   = 'C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx'

# 1) 优雅关机
& $vmrun -T ws -vp <VM密码> stop $vmx soft

# 2) 在 .vmx 里显式写死 NAT（VM 必须关机）
#    在 ethernet0.present = "TRUE" 之后插入两行：
#      ethernet0.connectionType = "nat"
#      ethernet0.startConnected = "TRUE"

# 3) 在 C:\ProgramData\VMware\vmnetdhcp.conf 的 `subnet 192.168.180.0` 块**内部**加保留，
#    插在该块的闭合花括号之前：
#      host MyRamDiskVM {
#          hardware ethernet 00:0C:29:02:90:C1;
#          fixed-address 192.168.180.129;
#      }
Restart-Service VMnetDHCP -Force

# 4) 开机并确认
& $vmrun -T ws -vp <VM密码> start $vmx nogui
& $vmrun -T ws -vp <VM密码> getGuestIPAddress $vmx        # 应为 192.168.180.129
```

### 两个必须注意的坑

- **改 `vmnetdhcp.conf` 前先备份，改完自检花括号平衡。** 我第一次用"锚点字符串替换"插入，
  结果多插了一个 `}` 和一个 `{`，把 subnet 块结构破坏了（`host VMnet8` 其实是**顶层块**，
  不在 subnet 块里——按行看才看得出来）。正确做法是**按行号定位 subnet 块的闭合花括号，
  在它之前插入**，然后断言全文件 `{` 与 `}` 数量相等。
- **known_hosts 里该地址可能已有旧占用者的密钥。** 本例第 25–27 行有 ed25519/rsa/ecdsa
  三条记录，ssh 因 ECDSA 那条不一致而拒绝（`REMOTE HOST IDENTIFICATION HAS CHANGED`）。
  处理方式：**先用 `ssh-keyscan` 核对 ed25519 指纹与已知值一致**（本例一致：
  `SHA256:VnBT/...`），确认是同一台机器，再删掉该地址的旧记录、由 `accept-new` 重新写入。
  顺序不能颠倒——先信后删，不是先删后信。

### 主机侧别名

`~/.ssh/config` 追加（**追加前备份**：该文件里还有使用者自己的 GPU 集群配置）：

```
Host myrd-vm
    HostName 192.168.180.129
    User usr
    IdentityFile ~/.ssh/id_ed25519
    IdentitiesOnly yes
    StrictHostKeyChecking accept-new
    ServerAliveInterval 30
```

### 切换后的瞬态

切到 NAT 之后的一两分钟里，`scp` 出现过一次 `Connection closed` / `Connection refused`，
随后自行恢复（Windows 网络位置识别与防火墙配置随网络类型变化而重建）。**遇到时先确认
22 端口是否已通、旧地址是否已消失**，不要当成配置错误去回滚。

---

## 7. 一次性客机配置：实际做过的步骤（2026-10-14 可复现）

客户机是 VMware Workstation 的 **`Windows 11 x64`**（`C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx`，MAC `00:0C:29:02:90:C1`，账户 `v\usr`）。

**该 VM 是加密的**（`vmx.encryptionType = "partial"` + `vtpmpresent`，Win11 需要 vTPM），
所以**每条 vmrun 命令都要 `-vp <VM密码>`**。密码已确认。

vmrun 的**参数顺序有硬要求：`-T ws` 必须最前**，否则 `-gu/-gp` 会被忽略，vmrun 转而交互式
索要 guest 用户名（表现为 `Error: Command requires valid user name and password`）。

```powershell
$vmrun = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
$vmx   = 'C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx'

# 1) 上传引导脚本与公钥（客机文件操作同样需要 -gu/-gp）
& $vmrun -T ws -vp <VM密码> -gu usr -gp <密码> copyFileFromHostToGuest $vmx `
      bootstrap-guest.ps1 C:\Users\usr\m5-bootstrap.ps1
& $vmrun -T ws -vp <VM密码> -gu usr -gp <密码> copyFileFromHostToGuest $vmx `
      "$env:USERPROFILE\.ssh\id_ed25519.pub" C:\Users\usr\m5-key.pub

# 2) 以提权身份执行（经 Tools 服务代理，绕过 UAC —— 这是能建"最高权限计划任务"的关键）
& $vmrun -T ws -vp <VM密码> -gu usr -gp <密码> runProgramInGuest $vmx `
      C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe `
      -NoProfile -ExecutionPolicy Bypass -File C:\Users\usr\m5-bootstrap.ps1
```

引导脚本做四件事：建 `C:\m5\{agent,control}`、装公钥（**见下面的坑 1**）、建计划任务
`MyRamDisk-M5-Agent`、把状态报告写到 `C:\m5\bootstrap.log`（用 `copyFileFromGuestToHost`
取回，**别只看退出码**）。

计划任务：

```powershell
schtasks /create /tn MyRamDisk-M5-Agent /f `
  /tr "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\m5\agent\m5-agent.ps1 -Drain" `
  /sc once /st 00:00 /ru usr /rp <密码> /rl highest
```

用 `-Drain`（处理所有未完成请求后退出）而不是传请求 id：**计划任务的命令行是固定的**。

---

## 8. 实测踩到的坑（都修好了，别再踩）

| # | 坑 | 表现 | 修法 |
|---|---|---|---|
| 1 | **管理员账号的 sshd 读的不是用户自己的 authorized_keys** | `Permission denied (publickey)`，即使文件装好了、ACL 也对了 | `sshd_config` 末尾的 `Match Group administrators` 把 `AuthorizedKeysFile` 指向 `__PROGRAMDATA__/ssh/administrators_authorized_keys`。管理员用户必须装到那个文件，并设 ACL 为 Administrators + SYSTEM（`icacls <file> /inheritance:r /grant '*S-1-5-32-544:F' /grant '*S-1-5-18:F'`） |
| 2 | **远端默认 shell 是 cmd.exe** | 临时脚本里的 PowerShell 语法报 `'$r' 不是内部或外部命令` | `ssh host <cmd>` 走 cmd；任何 PowerShell 脚本都要显式包一层：`ssh host 'powershell.exe -NoProfile -NonInteractive -Command -'` 并从 stdin 灌入 |
| 3 | **`ErrorActionPreference='Stop'` 会把原生命令的 stderr 变成终止错误** | 一次成功的 ssh/scp 被当成失败，真实的 `$LASTEXITCODE` 永远看不到 | 所有原生命令走一个 `Invoke-Native` 包装：临时切 `Continue`，以退出码判定成败 |
| 4 | **`Start-Process -ArgumentList @()` 参数校验失败** | 每个无参测试都记成 `exit=-2`（runner 自己的码，不是测试的） | 只在有参数时才加 `ArgumentList`（用哈希表 splat） |
| 5 | **`$probe` 与脚本参数 `[switch]$Probe` 是同一变量** | `无法将"System.String"转换为 SwitchParameter` | PowerShell 变量名大小写不敏感；循环变量不要与脚本参数同名（改名 `$probeDir`） |
| 6 | **`Copy-Item -Recurse <srcDir> <missingDest>` 会拍平一层** | `tests\` 里直接是 exe，`tests\Release\` 空 → `../../src/Release/myrd-m5-worker.exe` 解析失败 | 先建目标目录，再复制**源内容**（`<src>/*`）；并在远端重传前先删旧的二进制树，否则旧文件会留下 |
| 7 | **`Start-Process -PassThru` 不带 `-Wait` 时 `ExitCode` 读不到** | 每个测试都记成 `exit=-3`（"读不到退出码"） | 改用 `System.Diagnostics.Process` + `ReadToEndAsync()` + `WaitForExit(timeout)`：**既有真实退出码又有超时**（对照实验：A 无 `-Wait` → null；B 有 `-Wait` → 正确但无超时；C 两者兼得）。参数按 Windows 规则自行引用，否则 `Program Files (x86)` 这类路径会被拆断 |
| 8 | **`@(... | ConvertFrom-Json)` 会把 JSON 数组嵌套成单元素** | 过滤器只选中 1 项、用例名打印成 `System.Object[]`；单结果时汇总打印 `0 of  passed` | `ConvertFrom-Json` 把 JSON 数组当作**一个对象**输出，`@()` 于是包成"1 个元素（内容是数组）"。用**显式 `foreach` 循环**归一化，不要用 `@()` |
| 9 | **无介质光驱上的 `std::filesystem::exists()` 会抛异常** | `unit.m4.profile_codec` 以 `exception` 失败（用例断言 `!exists(L"D:\...")`） | 客机的 `D:` 是光驱。设 `sata0:1.startConnected="FALSE"` 会让它变"无介质"，而 `exists()` 这时抛 `ERROR_NOT_READY` 而不是返回 false。正解是**整个移除设备**（`sata0:1.present="FALSE"`） |
| 10 | **两个 structural 用例需要 Visual Studio** | `vswhere.exe not found`（`tools/Enter-MsvcEnvironment.ps1:6`） | 客机没装 VS。**环境受阻，不是产品失败**——除非给客机装 Build Tools，否则这两个用例在 VM 上永远不会绿 |
| 11 | **`-join` 不能作为 `Select-Object` 的参数** | `找不到与参数名称"join"匹配的参数` | `($x | Select-Object -First 5) -join ', '` —— 括号先收口再 `-join`。这个错误在本轮犯了两次 |

另外两条与 harness 无关、但会影响读数：

- **两个 structural 用例在客机上注定失败**：`structural.m2.storage_independent_build` 与
  `structural.m5.execution_boundary` 都要 Visual Studio 环境
  （`tools/Enter-MsvcEnvironment.ps1` → `vswhere.exe`）。这是环境限制，**不是产品失败**，
  读数里单独归类。其余 144 个不依赖编译器。
- 若真要 146/146，必须在客机上装 Visual Studio Build Tools——**为了两个环境用例装一套 VS，
  不划算**；先如实记为环境限制。

## 9. 尚未完成

- **重复运行刻画两个已知间发问题**：`m5.production_execution_backend`（§1.39 的
  "runtime stands dirty" 签名）与 `unit.m2.backing_generation`（`0xC0000409` 硬崩溃）。
  两者都在 3 次全套里各出现 1 次。用 `-Filter` 各跑 10 次统计复现率，复现时保留现场。
  见 `docs/validation/m5-current-state.md` §4。
- **测试硬编码开发机绝对路径**（`MYRD_M3_*_FIXTURE_PATH`、`MYRD_PRODUCT_EXE_PATH`）。
  目前靠在客机上镜像开发机路径布局绕过；长期正解是让测试从环境变量或相对路径取。
- ~~客机 IP 固化~~：**已完成**，见 §6（`192.168.180.129` / 别名 `myrd-vm`）。
- ~~stage `src/` 与 `cmake/`~~：**已完成**（Stage 的复制列表已改为枚举，避免再漏）。
- ~~客机缺 git~~：**已完成**（Git 2.53 宿主机直传 + 镜像 git 元数据；客机 `rev-parse HEAD`
  与宿主机返回同一提交）。
- ~~4 个 TIMEOUT 未查~~：**已查明是预算过紧，不是挂住**——它们在 180 秒下通过；
  已加 `-MinTimeoutSeconds`（下限）。
