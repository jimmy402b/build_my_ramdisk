# M5 交接文档：新会话从这里开始

日期：2026-10-14
分支：`feature/m5-lifecycle`，HEAD `e9a4940`（工作树 `.worktrees/m5-lifecycle`）
基线：`main` = `cb3df08`，**必须保持不动**

> 可粘贴的提示词在 **§10**。

---

## 1. 一句话现状

**T1-11（stop 路径补写冻结）已实现并验证**：`m5.production_execution_backend`
`-Repeat 20` 实测 **20/20 通过**（修复前 5/20 失败）。证据与踩坑记录：
`docs/validation/m5-t1-11-stop-write-freeze.md`。
**T1-02（组合根 + 服务骨架）已通过**：`sc create`/`start`/`query`/`stop` 全链路在客机上实测，
`RUNNING` 报 `ACCEPTS_PRESHUTDOWN`、`STOPPED` 且 `SERVICE_EXIT_CODE 0`、退出后无残留。
见 `docs/validation/m5-t1-02-service-skeleton.md`。
**下一步按 tasklist 顺序是 T1-03**（job object + 启动 sweep + 资源身份持久化）。

已完成的四个实质修复：
- `067f9b3` —— 修掉一个 **50% 崩溃的 M2 测试缺陷**（测试钩子在冒充生产不变量），
  修复前 ~20/40 崩溃 → 修复后 **20/20 通过**；
- `8a310bd` —— 建立 VM 回路，并做了**首次稳定性普查**：146 用例 × 5 = 730 次运行，
  **718/730 通过，只有 2 个用例真间发**；
- T1-11 —— stop 路径补写冻结。这是 **三层**改动：先补"提交前必须冻结写"，
  再修"冻结之后所有卷操作必须复用那把锁"，最后修"无共享独占打开会撞上刚关闭的句柄"
  （实测是**瞬时竞态，不是句柄泄漏**）。`-Repeat 20` **20/20**。
  见 `docs/validation/m5-t1-11-stop-write-freeze.md`；
- `integration.m1.shm` —— `start()` 返回成功时状态还没到 `serving`（**契约违约**，
  平时 worker 线程赢下这个窗口，整套连跑压起来就输）。整套 × 3 Sweep：
  **432/438**，该用例 **3/3**（修前 1/3 失败）。见
  `docs/validation/m5-intermittent-findings.md` §4。

---

## 2. 项目是什么

`myrd`：Windows 上的 RAM disk。物理层用 AIM（Arsenal Image Mounter，`phdskmnt.sys`，
**外部安装、不得重装/替换/升级**）提供代理盘；数据用 VHDX 基础镜像 + journal +
双槽 manifest + generation/checkpoint 模型。

| 层 | 内容 |
|---|---|
| M0 / M1 | 驱动 PoC / 共享内存 proxy |
| M2 | 动态块存储（64 KiB 块，lazy backing view，CLOCK 驱逐，UNMAP→Discard） |
| M3 | 持久化（journal、checkpoint 两阶段 P4/P5、崩溃恢复） |
| M4 | 前台单进程单盘运行时（`DiskRuntime`），已在 VM 上做过真实设备验收 |
| M5 | **当前工作**：Windows 服务 + 每 profile worker + typed IPC + CLI + GUI |

架构铁律：物理分层由 CMake 强制（`myrd_assert_execution_boundary`），只有
`myrd-m5-worker` 能链接 `myrd_m4_run` / `myrd_m3_windows`。

---

## 3. 关键路径

| 用途 | 路径 |
|---|---|
| **实现工作树**（所有代码改动在这里） | `.worktrees\m5-lifecycle` |
| 主检出（只用于 `main`，不要在这里改代码） | `C:\Users\JimmyLin\Desktop\build_my_ramdisk` |
| 构建目录 | `out\build\msvc`（Ninja Multi-Config，MSVC 14.44） |
| VM 回路 | `tools\m5vm\Invoke-M5Vm.ps1` |
| 回路运维文档（三条通道、一次性配置、**11 个已修的坑**） | `docs\operations\m5-vm-harness.md` |
| 当前读数与失败分类 | `docs\validation\m5-current-state.md` |
| **两个间发缺陷的完整调查**（含"为什么前四层能过"） | `docs\validation\m5-intermittent-findings.md` |
| 增量清单与状态 | `docs\superpowers\plans\2026-10-14-m5-v3-tasklist.md` |
| **总体执行计划（顺序 / 验收 / 风险 / 何时停下问）** | `docs\superpowers\plans\2026-10-15-m5-master-plan.md` |
| 设计（v3） | `docs\superpowers\specs\2026-10-14-m5-service-cli-gui-design-v3.md` |
| T1-11 实施方案 | `docs\superpowers\plans\2026-10-14-t1-11-stop-write-freeze.md` |
| 无调试器时读崩溃转储 | `tools\m5vm\read-minidump.py` |

---

## 4. 构建与测试循环

```powershell
# 构建（改过任何头文件就必须加 --clean-first，见 §5）
cmake --build out/build/msvc --config Release            # 或 --clean-first

# VM 回路
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
tools/m5vm/Invoke-M5Vm.ps1 -Probe                                   # 环境体检
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release            # 送进 VM
tools/m5vm/Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 180              # 跑整套
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^unit\.m2\.backing_generation$' -Repeat 20
tools/m5vm/Invoke-M5Vm.ps1 -Run -Repeat 5 -RepeatMode Sweep         # 5 遍整套
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '<x>' -TraceTestCases       # 逐用例 stderr 追踪
tools/m5vm/Invoke-M5Vm.ps1 -Run -RequestId <id>                     # 收集已提交的请求
```

- **`-Stage` 是必需的**：二进制在开发机构建，**执行只在 VM**。
- 整套一遍约 9–11 分钟；146 用例跑 5 次约 49 分钟。
- `-MinTimeoutSeconds` 是**下限**（只抬不降）：客机比 ctest 预算所假设的机器慢，
  4 个 verifier 用例的 30/60 秒预算在客机上不够。
- **host 侧等待超时不会中断 guest 的运行**：报错会给出 `-Run -RequestId <id>` 的收集命令。

---

## 5. 已确立的规则（不要重新学一遍）

| # | 规则 | 为什么 |
|---|---|---|
| 1 | **改头文件 ⇒ 测量前 `--clean-first`** | 增量构建不重编所有包含该头文件的 TU → ABI 错配 → `0xC0000005` 假崩溃。transfer manifest §7.17，**已两次现身**（我本人刚被咬一次：20/20 访问违例） |
| 2 | **单次绿是弱证据** | `unit.m2.backing_generation` 在开发机上 20/20 绿、在 VM 上 ~50% 崩溃。任何"绿"的声明**必须写明跑了几次** |
| 3 | **修复类结论至少 `-Repeat 20`** | 25% 和 70% 的缺陷，单次跑显示绿的概率分别是 75% 和 30% |
| 4 | 开发机只跑**不创建真实 worker / AIM 盘 / Windows 卷**的测试 | AGENTS.md §4；否则孤儿 worker 会占着设备，开发机存储栈会退化到重启 |
| 5 | 脚本一律传 `myrd-vm` 别名，**不硬编码地址** | 地址将来若变，脚本不用改 |
| 6 | 仓库脚本 **ASCII-only** | PowerShell 5.1 把无 BOM UTF-8 当 ANSI，会把解析搞坏 |
| 7 | 经 SSH 跑 PowerShell 用 **`-EncodedCommand`**（base64/UTF-16LE） | `ssh host "<命令>"` 走 `cmd.exe`，引号会被反复吃掉（我在这上面浪费了很多轮） |
| 8 | `@(... \| ConvertFrom-Json)` 会把 JSON 数组**嵌套成单元素** | 用显式 `foreach` 循环归一化 |
| 9 | `-join` 不能当 `Select-Object` 的参数 | 先括号收口：`($x \| Select-Object -First 5) -join ', '` |
| 10 | 硬崩溃会**吞掉全缓冲 stdout** | 已给 `test_main.cpp` 加 flush + `MYRD_TEST_TRACE` 追踪 + `set_terminate` 上报 |
| 11 | 崩溃先配 WER `LocalDumps`，再复现 | 无调试器时，转储里的 `__fastfail` **原因码**就能定性（见 `read-minidump.py`） |
| 12 | 无介质光驱上的 `std::filesystem::exists()` **会抛异常** | 客机的 `D:` 曾是光驱；`startConnected=FALSE` 会造出"无介质"状态。已整个移除该设备 |

---

## 6. 当前读数与它的含义

```
144 / 146 passed
  structural.m2.storage_independent_build   ← 客机无 Visual Studio（环境）
  structural.m5.execution_boundary          ← 同上（环境）

稳定性普查（Consecutive，730 次）：718 / 730
  从未全绿的只有 4 个：上述 2 个 + 下面 2 个
  m5.production_execution_backend   0,0,1,0,0   ← 真间发，根因已定位（T1-11）
  unit.m2.backing_generation        0,0,0,0,崩 ← 真间发，已修（067f9b3）
  其余 144 个用例 5/5 全绿
```

**未做**：`Sweep` 顺序的对照普查（连跑可能自我干扰）。若两轮结果不一致，以 Sweep 为准。

---

## 7. T1-11 已完成（记录结论，供回溯）

证据：`docs/validation/m5-t1-11-stop-write-freeze.md`。要点：

- **缺陷**：`disk stop` 实测 **25%（5/20）** 在 `remove_mount` 被拒
  （`The worker refused to remove: its runtime stands dirty`），
  因为 `quiesce_mutations` **只证明不冻结**，提交之后仍挂载的卷还能被写入。
- **修法**：stop 计划插入一步真正的写冻结，并让锁**跨命令**存活：

  ```
  quiesce → freeze_writes → flush → checkpoint → remove_mount
          → remove_exact_device → stop_runtime_and_release_lock
  ```

  锁持有在 `DiskRuntime` 里；`freeze_writes` 是 **worker 协议的新动作**
  （不是塞进 `quiesce`：一个是"证明停哪个 runtime"，一个是"让它不能再改"，失败含义相反）。
- **第二层（实测得来，最容易漏）**：`Win32VolumeAdapter::lock` 用**无共享模式**打开卷，
  所以一持锁，**同进程再开这个卷也会失败**（报成 `busy`，它分不出"别人"和"自己"）。
  第一版把冻结放在 flush 之前 → STOP **20/20 失败**（`code=21001`）。
  正解就是 M4 的顺序：**锁之后每一步都走持锁的那个句柄**。
  为此 `flush_volume` 与 `dismount_volume` 都增加了 `std::optional<LockedVolume>&` 参数，
  让"再 lock 一次"在类型上不可表达。
- **附带**：`writes_frozen` 不能由"是否还握着描述符"推导——拆卸会把描述符交给下层，
  那一刻描述符为空但卷仍冻结。故冻结状态由独立的 `writes_frozen_` 承载。
- **验收**：`-Repeat 20` → **20/20 通过**（修复前 5/20 失败）。

### 7.1 这一步踩到的两个环境事故（下次直接避开）

1. **kill host 侧的 `Invoke-M5Vm.ps1` 不会停掉客机上的运行**。客机上的测试继续跑，
   留下孤儿 worker 并占住 AIM 代理盘 → `Get-Disk` 多出 `PhysicalDrive1`、
   `diskpart rescan` 挂住、`Stop-Process` 报"拒绝访问"。
   **要停就停客机上的测试进程；恢复手段是 `vmrun ... stop <vmx> hard` 然后 `start`。**
2. **污染状态下失败会伪装成产品缺陷**：同一个二进制，污染时
   `m5_backend_creates_a_bundle_and_mounts_it_end_to_end` 失败在 `created.ok()`，
   干净重启后 13/13 通过。**设备类用例失败前先确认没有孤儿 worker / 多余物理盘。**

### 7.2 重入锁：测过、修了一个真缺陷、用例留在套件里

方案要求"先写'已持锁时再 lock'的重入测试"。**在 VM 上对真实卷测了，现在用例留在套件里**
（`m5_backend_holding_the_volume_lock_refuses_a_second_lock`），`-Repeat 20` 稳定。

**重入的结论**：本进程已持有该卷锁时，第二次获取**被拒**；方式**不是** `busy`，而是
`CreateFileW(设备路径, RW, 0 /* 无共享 */)` 返回 **`ERROR_NOT_READY`(21)**
（`lock` 只把 sharing/lock/busy 违规映射成 `busy`）。所以第二次获取**根本不会成功**——
这正是冻结必须**保留并向下传递**的原因。释放后同一个卷可以再次取锁 ⇒ 拒绝不是"卷坏了"。

**它顺带暴露的第三个真缺陷**（`docs/validation/m5-t1-11-stop-write-freeze.md` §4.4）：

- 现象：收尾间歇 `op=dismount_volume ... native=32`（`ERROR_SHARING_VIOLATION`）；
- **我先定性为"worker 句柄泄漏"——错的**。给 worker 加诊断（冲突后 250 ms 重试）后打印出
  `first=SHARING_VIOLATION after-250ms=OPENED` ⇒ **冲突句柄 250 ms 内就释放了，不是泄漏**；
- 真机制：`shutdown_clean` 先 **flush**（`flush` 打开卷→`CloseHandle`），紧接着 **dismount**
  用**无共享**模式打开同一个卷；刚关闭的句柄引用还没拆完 ⇒ `ERROR_SHARING_VIOLATION`。
  `freeze_writes` 走同一条路，所以 worker 会报 `the volume is busy`。
  **这个窗口在成品路径上真实存在**（`create → stop` 就是"flush 完立刻独占打开"），
  而它一直没被发现，是因为**失败被报成 `busy`，看起来是确定性的"盘停不下来"**；
- 修法：`Win32VolumeAdapter::lock` 在调用方已给的 `timeout_ms` 预算内**有界重试**
  冲突的独占打开（50 ms 间隔）。预算耗尽仍返回 `busy` 并带原生码——不软化任何东西。

**对照实验**（同套件、同客机）：带用例未修 9/20、2/6、5/6；**已修 20/20**。
整套三遍：144/146（终局配置与基线逐项一致）。

**保留的改进**：`VolumeLockAttempt` 增加 `native_code`，`dismount` 把它带进错误。
此前 `native=0` 把 sharing violation / `ERROR_NOT_READY` / 权限拒绝压成同一个不可区分的失败——
**它当时正在把这个竞态伪装成"确定性失败"**。

**自我纠正（两次）**：先把探针失败归因于"测试形态不可行"（错，是我自己 kill host 侧
留孤儿 worker 污染了 VM）；又归因于"worker 句柄泄漏"（也错，是瞬时竞态）。
**设备类用例间歇失败时：先排除环境污染，再给现象加一条能区分假设的诊断，最后才下结论。**

### 7.0.0 **绝不要用 `Set-Content -Encoding ASCII` 改文档**（2026-10-16，**已犯第三次**）

本轮我用 `Set-Content -Path <plan> -Value $c -NoNewline -Encoding ASCII` 改一份**中文文档**，
把**整份 master-plan 的中文全部变成 `???`**（368 处），
`git --no-pager diff --stat` 立刻显示 `409 insertions / 658 deletions` —— 一份正常的
小改动不可能有这种比例。**已用 `git checkout --` 恢复。**

**为什么会出现这个行为**：本会话的 PowerShell 是 **PowerShell 7**（提示符 `?>`），
而 PS 7 的 `-Encoding ASCII` **不再静默升级为 UTF-8**——它真的按 ASCII 写，
**每个非 ASCII 字符都变成 `?`**。在 Windows PowerShell 5.1 上同样的命令不会这样破坏。

**规则（硬性）**：

- **改文本文件一律用 `edit` 工具**，不要用 `Set-Content`；
- 确实需要写文件时，**只能用 `-Encoding utf8NoBOM`**（PS 7）或 `utf8`（5.1），
  **永远不要 `ASCII`**；
- **改完中文文件后先查 `???`**：`(Select-String -Path <f> -Pattern "\?\?\?" | Measure-Object).Count`
  必须为 0；再查 `git diff --stat` 的比例是否合理；
- 这个坑本次会话在**测试文件**上犯过一次（已记录），这是**第三次**——
  所以它是纪律问题，不是偶发。

### 7.0.1 会创建真实设备的用例的两个环境事实（2026-10-16）

- **`Set-Disk -IsOffline` 会在客机上挂住**：本轮清理强杀残留时它卡死，
  需要 `vmrun stop <vmx> hard` + `start <vmx> nogui` 硬重启（这是让客机存储栈恢复的唯一可靠手段，
  重启后 `disks: 0`）。
  **规则**：不要用 `Set-Disk` / `diskpart` 去清残留；**硬重启 VM**，它更快也更可靠。
- **留残留是合法的中间状态，但要有意为之**：`m5.production_execution_backend` 的强杀用例
  会**故意留下** `disks: 1,0 / letters: C,Z`，因为那正是 reclaimer 必须清掉的东西，
  而且该用例**断言它存在**。跑完它之后客机**不是干净的**，
  **下一个依赖干净设备的用例之前必须硬重启**。

### 7.0.2 `-Stage` 可能**先删后用**，失败后客机既没有 stage 也没有构建树（2026-10-16 新）

**现象**：`-Stage` 报 `remote command failed (exit 1)`，客机侧消息是
`GetFiles('...\out\build\msvc\tests\Release')` 抛 `DirectoryNotFoundException`，
并且 `C:\tmp\m5-vm\stage` **不存在**。本地 `out/build/msvc/tests/Release` 有 **119** 个 exe，
**本地构建没问题**。

**本轮取证到的客机状态（stage 失败之后）**：

```
workers: 1          ← 有孤儿 worker
disks: 1,2,0        ← 两块 AIM 盘
letters: C,Y,Z      ← 两个残留盘符
guest tests\Release exists: False
no stage dir
```

**两点判断**：

- **失败的 stage 会留下脏客机**：孤儿 worker + 两块盘 + 两个盘符。
  **所以 stage 失败之后不能直接重试**，必须先硬重启；
- 这**同时是一次意外但有价值的观测**：它说明"残留"不需要靠强杀用例制造，
  **一次中途失败的 stage/run 就会留下它**——这正是 reclaimer 要处理的东西。

**下一次遇到 stage 失败的第一动作**：`vmrun stop <vmx> hard` + `start <vmx> nogui`，
然后确认 `workers: 0 / disks: 0 / letters: C` 再重试。**不要**用 `Set-Disk` 清（会挂，见 7.0.1）。

### 7.0.3 断言失败时**必须打印 error 消息**（2026-10-16，本轮付了两次学费）

本轮 `m5_backend_a_reclaim_dismantles_the_leftover_a_kill_created` **连续两轮 0/20、0/3 全红**，
而 stderr 只有一行 `FAIL ...: reclaimed.ok()`——
**看不出是 adopt 失败、verify 失败还是拆卸失败**，于是**每轮都白跑一整次客机循环**。

**规则**：凡是断言一个 `Result`，就用本仓库既有写法把错误打出来：

```cpp
if (!x.ok()) {
    std::fprintf(stderr, "myrd-test: <what> failed: op=%ls msg=%ls native=%u code=%u",
                 x.error().operation.c_str(), x.error().message.c_str(),
                 x.error().native_code, x.error().code);
    std::fputc(10, stderr);
}
REQUIRE(x.ok());
```

**"先给现象加一条能区分假设的诊断"这条纪律，对 `Result` 断言同样适用**，
而且在这里代价特别高：一次客机往返约 5–30 分钟。
**这条规则在加上去的当轮就回本了**——它把之前两轮的盲猜变成了一行确切诊断。

### 7.0.5 **耗时是判断"客机上跑的到底是哪个二进制"的最省事信号**（2026-10-16，本轮踩坑）

**两个坑，一个是构建系统的，一个是纪律的。**

**（1）`Move-Item` 还原备份会保留备份的旧时间戳 ⇒ CMake 静默不重编。**
本轮做源码级隔离实验（把其它用例包进 `#if 0`）后还原文件，
**因为没有重编，我看到的是"隔离版"的旧二进制**，
于是把 **`1/1 passed` 误读成"完整套件通过了"**。
**是耗时露的马脚**：完整套件约 **50–75 秒**，那次只用了 **9–10 秒**。

**规则**：
- **还原文件后必须 `touch` 它**（或 `--clean-first`），否则构建系统会跳过；
- **`m5.production_execution_backend` 的耗时是可靠信号**：
  **完整 ≈ 50–75 秒**；**若只有 ~10 秒，几乎肯定跑的是被裁剪过的二进制**。

**（2）不要靠"跑一遍"去核对二进制内容——那会真的建 worker 与卷。**
本轮核对时在**开发机上**直接跑了该测试二进制，
**它真的创建了 worker 进程与卷**（随后已杀掉并确认归零）。
**这违反 §4 的硬门**，故记为警示：
**核对二进制用间接信号（耗时、PASS 行数），不要运行它。**

**（3）套件本身目前不稳定（记入 §8，与本增量无关）**：
`m5.production_execution_backend` 在**同一条命令、干净客机**下，
三次运行分别得到 **16 / 16 / 12** 条通过，
**失败点与原因都会变**（`provision_new_volume` 21001 ↔
`generation-zero checkpoint was not committed`）。
**已用两个实验排除**：① 加锁语义（回退前后完全相同）；
② 失败那条用例本身（隔离跑 1/1 通过）。
**因此这是套件级的资源/时序问题**，**不要**据此判断 T1-03 的成败。

### 7.0.6 **给一个无界的同步调用加"界"时：`std::async` 的 future 不能用来放弃**（2026-10-16，本轮踩坑）

**背景**：`enumerate_disk_candidates()`（`src/driver/physical_drive_locator.cpp`）
是**同步、无 overlapped 形式、不可取消**的 SetupAPI + IOCTL 序列，
实测会在客机上**挂死 80+ 秒**。给它加界的**唯一**办法是**把调用放出去、自己不等**。

**踩的坑**：第一版写成

```cpp
auto work = std::async(std::launch::async, []{ return enumerate_disk_candidates(); });
if (work.wait_for(timeout) != std::future_status::ready) { return timeout_error; }   // ← 没用
```

**`std::async` 返回的 future 在析构时会等待任务完成**，
所以"等超时 → 返回"**仍然会等那个要放弃的东西**：
**界读起来是对的，实际什么都没改变**（`-Repeat 5` 与改动前完全一样，
仍是那个不透明的 4107）。**这是最危险的一类"看起来生效"的改动。**

**正确做法**：**detached 线程 + 堆上的共享状态**（`std::make_shared`，线程持有其副本），
超时即返回错误、**不 join**。

**另有一个我自己在同一处犯的 UB**：第一版修正里**移动了一个仍被锁住的 `std::mutex`**。
**规则**：要跨作用域共享同步原语，就**一开始放在堆上**，
不要在返回路径上"搬"它。

**适用条件（写清楚，免得被误用到不该用的地方）**：
**放弃一个线程只在"被放弃的工作是只读的"时候才安全**（这里是列接口、查地址、查长度）。
**放弃一个正在拆卸/写入的调用是不安全的**——
那种情况要用 `CancelIoEx` 之类**真正取消**（本项目的 part 23/36/39 就是这么做的），
**不是**丢下不管。

### 7.0.8 **`dumpbin /directives` 对已链接的 exe 无效；要查 CRT/STL 链接请用 `/dependents`**（2026-10-16）

**计划 §8 第 5 项原来建议**"`dumpbin /directives` 对比 `myrd_storage.lib` 与测试 exe 的 `/DEFAULTLIB`"。
**实测：那条命令对 exe 不产出任何 `DEFAULTLIB`**——
该开关面向 `.obj` / `.lib`，**不是已链接的映像**。
**照它做会得到"exe 没有任何 DEFAULTLIB"这个无意义的结论**，
很容易被误读成"链接配置缺失"。

**正确做法**：**查 exe 用 `/dependents`**（看它实际绑定哪个 CRT/STL DLL），
**查 lib 用 `/directives`**（看它要求链接什么）。

**本项目的实际读数（2026-10-16，供以后对照）**：

| 对象 | 命令 | 结果 |
|---|---|---|
| `myrd_storage.lib` | `/directives` | `msvcprt` / `MSVCRT` / `OLDNAMES` / `uuid.lib` ⇒ **动态 STL** |
| 测试 exe | `/dependents` | `MSVCP140.dll` / `VCRUNTIME140.dll` ⇒ **动态** |
| 全树 `*.lib` | `/directives` 扫 `LIBCMT`/`libcpmt` | **一个都没有** ⇒ **无静态 STL** |

⇒ **本项目没有静态/动态 STL 混链**（§8 第 5 项据此关闭）。

### 7.0.9 **⚠ 更正："枚举阻塞"是错的根因；真凶是陈旧二进制害我追错了好几轮**（2026-10-16，**先读这条**）

**若你正要接着修 T1-03，请先读这条，它比 7.0.7 更重要。**

**7.0.7 里"枚举在负载下阻塞、拖垮 reclaim"这个结论已被实测撤回。**
同一条命令、同一台客机上：

| 读数 | 次 | 逐次通过条数 | 失败原因 |
|---|---|---|---|
| `270d458a`（**重启客机后**） | 2 | **16 / 16** | `provision_new_volume` 21001 |
| `23f2679f`（逐步打点版） | 2 | **16 / 16** | 同上 |
| `95c40c81` | 5 | 3 / 3 / 3 / 3 / 3 | **枚举界超时** |

**逐步打点的 trace 显示**：
`enumerate_disk_candidates` 每一步都返回、单次约 **14 ms**、
等待循环**从未超过 `turn 1`**、trace 里**没有任何 `bound fired`**。
⇒ **枚举既没慢也没卡。那个 3 条读数来自一份与源码不匹配的二进制。**

**因此**：
- ❌ 不要再为"枚举会卡"做任何改动（已有的界可以留着当防御，**但它不是答案**）；
- ❌ 不要再相信"客机越跑越退化"（唯一依据就是那个不可信读数）；
- ✅ **唯一稳定的失败是第 17 条用例的建卷**：
  `op=provision_new_volume msg=provisioning helper failed native=1 code=21001`
  ——**那是 PowerShell provisioning helper（外部进程），与枚举/reclaim 无关**。
  **要查的是这个 helper 的实际输出与退出码。**
- ✅ **T1-03 的两条用例在每一个可信读数里都 PASS**（最近 4 次运行全通过）。

**教训（本项目的第四次）**：**"陈旧/不匹配的二进制"会伪造出"退化""偶发""阻塞"这类结论。**
已有的相关坑：`Move-Item` 还原保留旧时间戳 ⇒ CMake 静默不重编（7.0.5）；
part 64 因此引入 `CMAKE_DEPENDS_USE_COMPILER`。
**规则**：**任何"性能变差/偶发/卡住"的结论，先确认客机上跑的二进制确实来自当前源码**——
最省事的判据仍是**耗时**（完整套件 50–75 s，被裁剪过的约 10 s）。

### 7.0.7 **~~设计缺口：设计文档从未设计"如何找到那块物理盘"~~（2026-10-16）——**结论部分撤回，见 7.0.9**

> **读之前先看 7.0.9**：本节把"如何找到那块物理盘"称为 T1-03 的真正阻塞，
> **但后来证明枚举一直正常，那个阻塞不成立**。
> **仍然成立的部分**：设计文档**确实没有设计发现机制**
> （`SetupDi` / `DeviceInterface` / `SCSI` / `PhysicalDrive` 在 434 行里出现 0 次，
> 而 `盘符` 出现 10 次）——**这是一个真实的设计缺口，值得在 T4 前补**。
> **已撤回的部分**：它**不是**当前失败的原因。


**若下一位接手 T1-03，先读这条，它比 trace 更重要。**

设计文档 `docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md`（434 行）
对 **`盘符`** 有 10 次交代，但对
**`SetupDi` / `DeviceInterface` / `dev_inst` / `SCSI` / `PhysicalDrive` 全部 0 次**——
**即"把 AIM 代理盘对应到 Windows 物理盘号"这一步，设计里根本没有。**

而实现必须有这一步（要对那块盘 `remove`/`restore`），
于是 M0/M3 **自行选择了"全机器枚举磁盘接口"**（`enumerate_disk_candidates`，
`SetupAPI` + 每块盘查 `IOCTL_SCSI_GET_ADDRESS` / `IOCTL_STORAGE_GET_DEVICE_NUMBER`）。

**这个自选机制在本项目客机上不可靠**：实测**卡住 80+ 秒不返回**，
且**每次 worker 启动都会跑**，于是套件里越跑越糟
（`-Repeat 20` 有 18 次连第 3 条用例都过不去）。

**已做（有用但不解决它）**：给枚举加了界（10 s，detached 线程），
并**下沉到函数本身**使 M1/M2/POC 全部受益（`174799b`）。
**效果是"挂死 80 s + 不透明 4107"变成"10 s 内失败并指名 `enumerate_disk_candidates`"**。
**但它不会让 reclaim 成功**。

**未决（需用户/设计裁决）**：三条路——
① 改按 **SCSI 地址**（`DeviceId` 里的 `path/target/lun`）直接构造设备路径打开，不枚举；
② 换成**有界/定向**的发现（只查 AIM 适配器 `dev_inst` 下的子设备，或换能按身份查询的接口）；
③ **扩展 AIM 查询**让驱动报盘号（**已知当前报的是 AIM 自己的设备号，故须改协议/驱动**）。

**注意**：**不要**把"枚举加界"当成已经修好——它只是让失败可见。

### 7.0.4 reclaim 用例首次运行**挂住**：多于 30 分钟无结果且 SSH 停止响应（2026-10-16）

**现象**：`-Run -Filter '^m5\.production_execution_backend$' -Repeat 1 -MinTimeoutSeconds 900`
**staged 成功、run 启动**，但**超过 30 分钟没有结果文件**，同时**新的 SSH 命令不再被服务**
（`echo ALIVE` 还会答，但任何带内层 PowerShell 的命令挂住）。
杀掉本地 job + `vmrun stop hard` / `start nogui` 后客机恢复干净。

**已排除 / 已修的**（见 part 19–20）：

- 早先 `0/20`、`0/3` 全红是**我自己的 fixture 容量写错**（8 MiB vs 64 MiB），
  被 `reclaim` 正确地以几何校验拒绝；
- 随后可见的错误是 **IPC 超时**（`pipe transfer exceeded its bounded wait`，code 4107），
  已用 `reclaim_context()`（取 session 最大值 120 s）修掉。

**仍未判定**：修掉超时后到底是**成功**、**仍然超时**还是**真的挂住**——
**本轮没有读数，故不作任何断言**。区分"会挂"与"会超时"是下一步第一件事。

**下一次跑这个用例的建议**：

- 用**单次** `-Repeat 1`，并**先**确认 `workers: 0 / disks: 0 / letters: C`；
- 挂住超过 ~10 分钟就**硬重启**，不要等 30 分钟；
- 若复现，**先取现场再重启**：`myrd-m5-worker` 进程数与启动时间、`Get-Disk`、盘符、
  以及 `C:\m5\control\run-<id>\*` 是否已有 stdout——
  用于区分"**reclaim 没被调用**"、"**调用后卡在 adopt**"、"**调用后卡在拆卸**"。

### 7.0 会创建真实进程的用例必须自带守卫（2026-10-16 新增，**已犯过一次**）

**我写了一个会创建真实进程的用例（`m5.startup_sweep`），却忘了它只能上客机。**
在开发机上跑 `ctest -R "m5\."` 时，它**真的跑了**——60 秒里反复启动并杀掉 `cmd.exe` 子进程。
这次没留下残留，但那是运气：同一个错误换成 AIM 盘就不会是运气。

**现在它自带守卫**：不在客机上就直接 `_Exit(2)` 并打印该怎么办，
**故意"响亮地拒绝"而不是静默跳过**——静默跳过会让一个验收"没测量"却被报成"通过"。

**守卫用环境变量，而且踩了两个坑才定下来：**

| 尝试 | 结果 |
|---|---|
| `ProcessStartInfo.EnvironmentVariables['MYRD_M5VM_GUEST']` | **客机上也没生效**（变量没穿过 runner 包的那层 `cmd /c`） |
| 检测 `C:\tmp\m5-vm\stage` 存在 | **开发机上也存在**（staging 就是镜像开发机布局的，这正是它绕过守卫的原因） |
| 在 runner 里 `set MYRD_M5VM_GUEST=1&& <exe>`，写进 `cmd` 行 | ✅ 两侧都对：开发机 exit 2，客机 5/5 |

**另一条务必记住的运维事实**：客机上的 runner 脚本**不在 `-Stage` 的范围内**，要手工 scp，
而且**agent 实际运行的是 `C:\m5\agent\Invoke-M5Tests.ps1`，不是 `C:\m5\control\` 下那份**
（`C:\m5\control\` 有同样的文件，改了它没有任何效果——我就这样白等了两轮）。
改了 guest 脚本之后：**scp 到 `C:\m5\agent\`**，再重跑。

### 7.1 工具链的两个静默陷阱（2026-10-16 实测并已修）

- **`-Stage` 会静默丢包**：它先清空客机目标树再逐条 scp，有时上传没落地却仍然打印
  `staged N test binaries` 并成功返回。症状是 runner 打印
  `MISSING .../<test>.exe`，**读起来完全像构建问题**——已有一轮排查因此走错方向，
  而"再 stage 一次"修好了一个从未存在的缺陷。
  **现已加校验**：staging 结束后在客机上数测试可执行文件并与本地比对，不符即报错并写明
  "这是 staging 没落地，不要把随后的运行失败当成产品缺陷"。
  **仍要记住**：看到 `MISSING` 先怀疑 staging，不要先怀疑构建。
- **UTF-8 BOM 会打断远程脚本首行**：`Invoke-M5Vm.ps1` 带 BOM，而 `powershell -Command -` 不剥 BOM，
  BOM 粘在正文第一个 token 上 ⇒ 客机报"无法将该项识别为 cmdlet"。
  修法**两次才修对**：第一次加在 `Invoke-Remote`（漏了直接调 `Invoke-Native` 的解包步骤），
  第二次移到 `Invoke-Native` 却**没删掉第一份** ⇒ 空行变两行、**BOM 落到第二行**，
  把原本要修的调用弄坏，**同样的报错、不同的原因**。
  最终把 staging 自检里的计数改成 **.NET 静态调用**（`[System.IO.Directory]::GetFiles(...).Length`），
  **没有 cmdlet 名字可被污染**——把脆弱性去掉，而不是继续绕过。
  **教训**：这个工具链上"报错信息相同"不代表"原因相同"，改工具时要重跑**所有**受影响的调用。

---

## 8. 未解 / 待查（不要当成已解释）

1. **`set_terminate` 处理器没有触发**：那个守卫调的是 `std::terminate()`，而我的处理器
   从未打印。修复后守卫不再触发，所以不影响结论；但可能意味着**静态/动态 STL 混链**
   （两套 terminate 指针）。检查很便宜：`dumpbin /directives` 对比 `myrd_storage.lib`
   与测试 exe 的 `/DEFAULTLIB`。
2. **测试硬编码开发机绝对路径**（`MYRD_M3_*_FIXTURE_PATH`、`MYRD_PRODUCT_EXE_PATH`）：
   现靠"在客机上镜像开发机路径布局"绕过。长期正解是改测试从环境变量取。
3. **两个 structural 用例需要 Visual Studio**：已归类为环境受限并接受；
   真要 146/146 就得给客机装 Build Tools，性价比不划算。
4. **设计未决项**（需要用户裁决）：Primo 的 "Mini Tools" 定义；T4-02 单文件 VHDX 导出是否保留。
5. **§7.17 那个 ABI 陷阱**仍未被根治：项目没有可靠的头文件依赖跟踪，只靠"记得加
   `--clean-first`"这条纪律。
6. **【未修】`m5.execution_scheduler` 在开发机上本来就是失败的**：T1-02 期间发现，
   用 `git stash` 回到未改动的 HEAD 复核过——**同一个用例在基线代码上同样失败**
   （`m5_execution_scheduler_tests.cpp:455`，`!record_is_succeeded(fixture, operation_id)`），
   与本轮改动无关。它落在 **T1-05/T1-06 的验收面**上，到那两个增量时一并处理。
   **在此之前它是"开发机上一个已知红灯"**，不是可忽略项。

---

## 9. 本地一次性测试凭据（仓库若要共享，请先删除本节）

| 项 | 值 |
|---|---|
| 虚拟机 | VMware Workstation，`Windows 11 x64`，MAC `00:0C:29:02:90:C1` |
| VMX | `C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx` |
| **VM 加密密码**（每条 vmrun 都要 `-vp`） | `12345678`（`123` / `111` 都不对） |
| 客机账户 | `usr` / `123` |
| SSH 密钥 | `~\.ssh\id_ed25519`，别名 `myrd-vm` → `192.168.180.129` |
| 客机主机密钥指纹 | `SHA256:VnBT/Zzkxj+hxwIBUsdgFOyS0oKkZD/MrRZ7P3axKIE` |
| 驱动 | `phdskmnt` 必须 Running |

VM 是**加密的**（vTPM，Win11 必需），所以 vmrun 必须带 `-vp`；且 **`-T ws` 必须是
vmrun 的第一个参数**，否则 `-gu/-gp` 被忽略。

---

## 10. 提示词（新会话直接粘贴）

```
继续 myrd 项目的 M5 实现。

工作目录 C:\Users\JimmyLin\Desktop\build_my_ramdisk
实现在工作树 .worktrees\m5-lifecycle（HEAD e9a4940）；main 必须保持在 cb3df08 不动。

动手之前先读这四份：
1. docs\operations\m5-handoff.md            —— 交接文档（环境、规则、待办）
2. docs\superpowers\plans\2026-10-14-m5-v3-tasklist.md        —— 增量清单与状态
3. docs\validation\m5-intermittent-findings.md                —— 两个间发缺陷的完整调查
4. docs\superpowers\plans\2026-10-14-t1-11-stop-write-freeze.md  —— 当前任务的实施方案

当前任务：T1-11（stop 路径补写冻结）。严格按方案第 2 节执行，并且
第一步是先写"已持锁时再 lock"的重入测试，通过之后才动 dismount()。
验收用 tools\m5vm\Invoke-M5Vm.ps1 -Run -Filter '^m5\.production_execution_backend$'
-Repeat 20，要求 20/20（修复前实测 5/20 失败）。

硬性规则：
- 改任何头文件之后、测量之前必须
  cmake --build out/build/msvc --config Release --clean-first
  （否则会得到 0xC0000005 的假崩溃，已咬过两次）。
- 开发机上只跑不创建真实 worker / AIM 盘 / Windows 卷的测试；其余一律在 VM 上跑。
- 不接受单次绿：修复类结论至少 -Repeat 20，任何"绿"的声明必须写明跑了几次。
- 每个增量收尾：干净构建 → VM 端到端 → 证据写入 docs\validation\ → 提交到分支。
- VM 用 ssh 别名 myrd-vm 与 tools\m5vm\Invoke-M5Vm.ps1 驱动，不硬编码地址；
  经 SSH 跑 PowerShell 用 -EncodedCommand，避免引号被 cmd 吃掉。
- 只在设计未决项、验收声明变更、或需要重写某一层并重挣 M3/M4 证据时停下来问我。

T1-11 完成后按 tasklist 顺序继续 T1-02 → T4-04。
```
