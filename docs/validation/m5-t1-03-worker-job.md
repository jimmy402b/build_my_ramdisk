# T1-03 证据（part 3）：worker job 接线、句柄保留、观测

日期：2026-10-16
分支：`feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`
设计：`docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md` §5
增量：tasklist T1-03
本轮涵盖：**part 3**（job 接线 + 句柄保留 + 观测修正 + 工具链修正）。
part 1/2 见 `docs/superpowers/plans/2026-10-15-m5-master-plan.md` 的 T1-03 卡片。

---

## 0. 结论

| 项 | 结果 |
|---|---|
| `m5.production_execution_backend`（含 3 个新用例） | **3/3 通过**（`-Repeat 3`，请求 `16b2e9ffc0f84a72be45072429c9b8a6`） |
| 开发机纯逻辑 `m5.worker_sweep_policy` + `m5.service_host` | **全绿** |
| 干净构建 | 0 error 0 warning |
| **worker 用 `PROC_THREAD_ATTRIBUTE_JOB_LIST` 在创建时进入命名 job** | 通过（真机测量） |
| **重开的 job 句柄仍能看到活着的 worker** | 通过（真机测量，这是 sweep 能成立的前提） |
| JOB 对象内部计数 | 通过（`m5.service_host` 等，未变） |

**本轮抓到 4 个真缺陷**，其中 2 个在产品代码、2 个在测试基础设施。

---

## 1. 已交付（产品代码）

| 文件 | 变更 |
|---|---|
| `src/host/m5_worker_private_pipe.{h,cpp}` | `launch_private_worker` 增加**必需的** `worker_job` 参数；job 通过 `PROC_THREAD_ATTRIBUTE_JOB_LIST` **随创建属性一起**指派（不是创建后再 `AssignProcessToJobObject`） |
| `src/management/windows_worker_job.{h,cpp}` | job 上限改为 `JOB_OBJECT_LIMIT_BREAKAWAY_OK`；`job_handle()`；**`observe_orphans` 改用进程表 + `IsProcessInJob`** |
| `src/management/windows_worker_sessions.{h,cpp}` | provider 构造增加 `WindowsWorkerProcessSource&`；launch 后**保留句柄**（`retain_launched`）而不是丢弃 |
| `src/service/service_runtime.{h,cpp}` | 组合根先建 job 与注册表，再建 provider / worker |

**job 的三条设计义务仍然成立**：命名（重启能找到）、**不设 `KILL_ON_JOB_CLOSE`**（否则被杀的 worker 跑不到有序拆卸）、**只有按进程终止**（compile-time 断言钉住，加 job 级终止就编译失败）。

---

## 2. 四个缺陷（按发现顺序）

### 2.1 `SILENT_BREAKAWAY_OK` 让 job **不记账**（产品缺陷）

原本用 `JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK` 让 worker 自己 spawn 的 provisioning helper 不加入本 job。
真机测量发现：这样设了之后，worker 从 `JobObjectBasicProcessIdList` 里**消失**——sweep 会看不见它存在的理由。

改为 **`BREAKAWAY_OK`**：仍然允许脱离，但**必须显式请求**（`CREATE_BREAKAWAY_FROM_JOB`），而本仓库至今没有任何代码在 worker 内部 spawn 东西。**成员资格是 sweep 依赖的性质，所以成员资格优先。**

### 2.2 `QueryInformationJobObject` 枚举不到成员（产品缺陷）

换成 `BREAKAWAY_OK` 后，**`IsProcessInJob` 报 1，而同一个 job 的 `JobObjectBasicProcessIdList` 报 0 个进程**。
一个静默漏掉全部成员的枚举比一个会失败的枚举更糟：**sweep 会得出"世界是干净的"这个结论**。

`observe_orphans` 因此改为：**遍历进程表（`CreateToolhelp32Snapshot`）→ 对每个进程问 `IsProcessInJob` → 用 creation time 核对身份**。
代价写在代码注释里：这会读到本服务没启动的进程，所以"是不是我们的"由两步决定——成员资格（一个实测可用的原语）加上已记录的绑定（pid **与** creation time），由 `sweep_verdicts` 裁决。两步缺一不可，这正是 job 查询本来也需要的那种分工。

### 2.3 `-Stage` 静默丢包（工具链缺陷，**两次误导排查**）

**根因**：staging 会先清空客机上的目标树，再按条目 scp；有时上传没有真正落地，但工具仍然打印
`staged 116 test binaries` 并返回成功。

**症状**：runner 打印
`MISSING .../myrd_m5_production_execution_backend_tests.exe`，
**读起来完全像一个构建问题**——于是有一轮排查被引到了错误的方向，而"再 stage 一次"就修好了一个从未存在过的缺陷。

**修法**：staging 结束后**在客机上数**测试可执行文件，与本地数目比对，不一致就 `throw` 并明确写出
"这是 staging 没落地，不要把随后的运行失败当成产品缺陷"。

### 2.4 BOM 打断远程脚本首行（工具链缺陷）

本文件带 UTF-8 BOM，而 `powershell -Command -` **不剥 BOM**：BOM 粘在正文第一个 token 上，
`Get-ChildItem ...` 到客机变成 `\ufeffGet-ChildItem ...`，客机回答"无法将该项识别为 cmdlet"。

**修法**：`Invoke-Remote` 在正文前补一个空行吸收 BOM。这同时修好了一个**先前就存在**的失败——git 元数据解包那一步（`& 'tar.exe' ...`）此前也因同样原因报 `CommandNotFoundException`。

### 2.5 我自己在测试里犯的两个错（记录以免重犯）

- 用 `cmd.exe /c ping -n 60 ...` 当"长生"子进程：**在客机上它立刻退出**，而退出进程不可能是孤儿，
  于是用例什么都没测到；
- 先关闭 session 再观测 worker：session 一关**私有管道就 EOF，worker 是"干净退出"**的——
  把"干净退出"读成了"job 杀了它"。

两次都是**测试主体（subject）选错**，不是产品错。最终用例把 session 保持在**外层作用域**，
确保整个测量期间 worker 都活着。

---

## 3. 量测范围（重要，不含糊）

- 虚拟机：`m5.production_execution_backend` **`-Repeat 3`，3/3 通过**，真实 worker 进程。
  这是**功能验收**，不是稳定性验收；`-Repeat 20` 留给 part 3 的强杀/回收场景。
- 开发机：`m5.worker_sweep_policy`（18 用例）与 `m5.service_host` 全绿；**不创建任何进程**。
- 干净构建 0 error 0 warning（改过头文件，测量前已 `--clean-first`）。

---

## 4. 明确的已知限制（**未验证**）

### 4.1 "关闭 job 不会杀死 worker"在**本测试环境里无法干净测量**

性质本身没有疑问（不设 `KILL_ON_JOB_CLOSE` 就是为此），但**测量**在这里做不到，两条原因都实测过：

1. **`AssignProcessToJobObject` 在提权 SSH 会话里返回 `ERROR_ACCESS_DENIED`（5）**，
   所以本工具链里**每个进程都已经嵌套在一个不是我们创建的 job 里**；
   于是 `IsProcessInJob(process, nullptr, &r)` 会因为与本 job 无关的原因报 true——
   探针**分不清"还在我们的 job 里"和"在工具链的 job 里"**；
2. **关闭 job 的最后一个句柄再按名字重开，观测到的未必是同一个对象**：
   一个重开的句柄可能是一个**新的空 job**，从外面看和"worker 离开了"一模一样。

因此该用例**被删掉而不是留着空过**，性质交给 part 3 的 VM 验收（真杀服务、真孤儿），
并作为已知限制记在这里。

### 4.2 本轮新增（part 4 + part 5，`6c7346b` / `3fa2733`）

| 交付 | 内容 | 读数 |
|---|---|---|
| `worker_binding_store.{h,cpp}` | 设计 §5.5 的"最后观测身份"，最小形态：每 profile 记录 worker 的 **pid + process creation time + generation**，落在 `<ProgramData>\MyRamDisk\profiles\workers`（复用安装器已建好的 ACL 受保护根，不再造第二个受保护根） | `m5.worker_binding_store` **10/10** |
| `worker_sweep.{h,cpp}` | 把 part 1/2 的决策**执行**出来：问下孤儿、只在被允许时升级、adopt 预期在服务的、并**报告**做了什么 | `m5.worker_sweep` **8/8** |
| `service_host` 的 sweep 接缝 | `IStartupSweep` + 启动序列（**recovery 之后、scheduler 之前**）+ 三种结果区分 | `m5.service_host` **14/14**（新增 6） |

**两个"答案不同"的设计点，各有独立用例**：

- **"没有记录" ≠ "记录读不出来"**：前者是 `nullopt`（不可归属），后者是失败。
  合并两者会让一个损坏的文件看起来像"这个 profile 从来没有过 worker"；
- **读不到 durable 绑定必须让 sweep 停下，而不是改道**：没有绑定时每个孤儿都像"不认识的绑定"，
  最直白的实现会把它们**全部强杀**——杀掉可能正在有序拆卸、且本来属于我们的 worker。
  枚举失败同理**不是"世界干净"**，因为那是唯一确定会终止排查的答案。

**我自己在这轮犯的错（都在解析器里，都由 round-trip 用例抓到）**：

- 字段写成 ` key=value`，所以值前面有分隔空格，`from_chars` 拒绝；
  **这份 store 写出的每一条记录都读不回来**；
- 最后一个字段后面跟着换行，`generation=7\n` 因此同样被拒。
  只修剪一端不够，而**第二个缺陷在第一个修好之前是不可见的**。

两次都是**把抓到的字节打出来**才找到的，而不是重读那段"看起来正确过三次"的代码。

**启动序列的三种结果**（`m5.service_host` 逐条钉住）：

- sweep **跑不起来** ⇒ **启动失败**，且 scheduler 从未启动（不能在没扫过的世界上准入工作）；
- sweep **发现需要人看的东西** ⇒ **默认照常启动**，但通过
  `ServiceHostDiagnostics::sweep_needs_attention` **报出来**。理由是一个选择而非疏忽：
  机器上有一个不肯死的残留 worker，其余盘仍然要被服务，拒绝启动等于把一个卡住的孤儿
  变成一个下线的服务；
- 想要"宁可不启动"的部署可以打开
  `ServiceHostOptions::fail_start_when_sweep_needs_attention`。

### 4.3 已接线（part 6，本轮）：sweep 现在会真的动手

**读数（part 7 复验，虚拟机）**：`m5.startup_sweep` **`-Repeat 20` = 20/20 通过**
（请求 `d94e45f6d6f84f77aca22de6db29e8c1`），**且在守卫生效之后**——
即开发机上这条用例会 `exit 2` 拒绝运行，只有客机才测量它。
（part 6 的首轮读数是 `-Repeat 20` 20/20，请求 `d70ee7af65d246eeb3aabd79ea7e4a61`。）

| 交付 | 内容 |
|---|---|
| provider 记录指纹 | launch 成功后写 `WorkerFingerprintRecord`（pid + creation time + session generation）。**写失败不致命**：worker 正在跑且有用，为了一个提示而拒绝一块盘是本末倒置；代价有界且可见（下次启动报 needs-attention 而不是清理） |
| `FingerprintWorkerBindings` | durable 指纹 → `ExpectedWorkerBinding`。**`recorded_creation_time` 必须来自 store**，否则 `sweep_verdicts` 对每个孤儿都答 `leave_alone`（正确地答，因为 pid 不是身份）——**这个失败在类型上看不出来**，所以写在头文件里 |
| `ProductionStartupSweep` | `run_worker_sweep` 的 `IStartupSweep` 适配器，保留报告供操作员与证据使用 |
| 组合根 | 建 store → 建 provider（带 store）→ 建 bindings（`expects_running_worker` 从 **durable profile 记录**读，不猜）→ 建 sweep → 传给 host |
| `m5.startup_sweep` | **新集成用例**：真 job 里的真孤儿 + 真 store + **真 adapter**，跑生产 sweep，断言孤儿**真的没了** |

**为什么第二个用例被删掉**（见文件内注释）：**"无法归属的孤儿"在真实里不可构造**——
job 报出的进程**没有 profile**时，它不是"无法归属"，而是**被明确判定为不属于任何 profile**，
也就是 `terminate_forced` 那条路。要构造"无法归属"需要一个**被 job 打上 profile 标记**的孤儿，
而 job 打不了这个标记（哪个 worker 服务哪个 profile 存在 durable 记录里，job 不读它）。
所以那条路在能构造它的地方测（`m5.worker_sweep` / `m5.worker_sweep_policy` 用 fake），
**而不是留一个空过的用例**。

### 4.4 本轮抓到的 4 个缺陷（全部由"用真东西"逼出来）

**① `terminate_process` 打开了不够的权限（产品缺陷，最严重）**

`OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, ...)` 之后调 `GetProcessTimes`
——**没有 `PROCESS_QUERY_LIMITED_INFORMATION` 就读不到创建时间**，
于是 `live` 为空，而那之后的安全检查**拒绝了它本来要结束的每一个进程**。
实测：`observed=134366055718126761 live=0`，看起来像"pid 被回收"，
实际是权限没要够。**强制终止 100% 失败，而且伪装成安全检查正常工作。**

**② 指纹 store 的 leaf 目录没人建（产品缺陷）**

`open_managed_atomic_directory` 要求每一级都存在，全新安装没有指纹 ⇒ 第一次启动必然失败。
而且**继承父目录的权限不够**：子目录继承的是 ACE，自己**不是** `SE_DACL_PROTECTED`，
所以 `open_managed_atomic_directory` 正确地拒绝它。改成用**显式 descriptor 创建**，
且 ACE 必须带 `OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE`（否则报 "unexpected ACL entry"）。

**③ 手写四步原子发布被真目录拒绝（产品缺陷）**

我自己写 create/write/flush/publish，真目录回 "Temporary profile file is not verified for publication"
——那条规则正是防"半份记录"的唯一屏障。改为复用 profile 层的
`publish_current_preserving_previous`（但**它不能建文件**：第一步就是读目标，
而真目录把"文件不存在"报成错误 ⇒ 第一次写仍走显式四步，**并包含那次验证性读回**）。

**④ 我修 BOM 修出了第二个 BOM 问题（工具链，自己造成）**

第一次把"吸收 BOM 的空行"加在 `Invoke-Remote`，漏掉了直接调 `Invoke-Native` 的解包那步；
移到 `Invoke-Native` 后**没有删掉 `Invoke-Remote` 里那份**，于是空行变成两行、
**BOM 落到第二行**，把原本要修的调用弄坏了——同样的报错信息，不同的原因。
最终改为**用 .NET 静态调用数文件**（`[System.IO.Directory]::GetFiles(...).Length`），
**没有 cmdlet 名字可被污染**，把脆弱性去掉而不是绕过。

### 4.5 本轮（part 7）：资源身份投影

设计 §5.5 说"持久化最后观测身份"，做的时候发现**大部分本来就已经是持久的**——
durable `Profile` 记录里已经有 `volume_guid`、`gpt_disk_guid`、`partition_guid`、`mount_target`。
所以**没有为它们新增存储**（第二份副本只会是第二个可能和权威记录不一致的东西），
而是新增一个**纯投影** `project_observed_identity(record, bundle_file_identity, generation)`：

- 卷身份与 GPT 身份从 profile 记录读；
- `bundle_file_identity` 是参数（它在 bundle registration evidence 里，不在 profile 记录里）；
- `generation` 是参数（它属于 checkpoint 状态，不属于 profile）。

**投影不自己做决定**：它返回**构造出来的**身份，由 `reclaimable_resources` 判断是否完整。
理由是：一个"不完整就返回空"的投影会让**"记录不全"和"没有记录"变得无法区分**，
而这两者要导向不同的报告。

**这轮的点睛用例是 mount 形状陷阱**：

> `ObservedResourceIdentity::mount_target` 是**单个 `wchar_t`**（它授权的回收动作是"清掉这个盘符"），
> 而 profile 记录的 `mount_target` 是**字符串**，而且可以是**目录挂载点**。
> 直接取首字符对 `R:` 会**碰巧**正确，对目录挂载则会**凭空造出一个盘符**。
> 所以转换**只接受恰好 `X:` 的形状**（大小写归一），否则**留空** ⇒ `complete()` 为假 ⇒ 不回收。

读数是 `m5.worker_sweep_policy` **25/25**（新增 7 个用例：完整投影、不自己决定、
拒绝目录挂载、小写归一、畸形盘符、缺 bundle 身份、generation 0）。

**这轮同时修了一个我自己的违规**：`m5.startup_sweep` 会创建真实进程，
却在开发机 `ctest` 里跑了 60 秒。现在它自带守卫（见交接文档 §7.0），
开发机 exit 2、客机通过。

### 4.6 本轮（part 8）：强杀 worker 之后**没有无人认领的进程**

强杀验收在计划里写作两半：**"worker 自行有序拆卸（不留设备/盘符）"** 与
**"下次启动 sweep 回收，不留设备/盘符"**。其中"不留设备/盘符"那一半需要**真实 AIM 设备**，
因为那份残留是 worker 自己的挂载路径造出来的——**这一半没做，也没有宣称**。

能做而且已经做的是另一半：**被强杀的 worker 不能留下一个无人能认领的进程。**
两个新用例（都在客机，真 worker、真 job、真指纹 store）：

| 用例 | 断言 |
|---|---|
| `m5_sweep_finds_nothing_after_a_worker_is_killed` | 用生产 provider 起真 worker → `TerminateProcess` **强杀**（不是请它走）→ 关掉 session（等价服务死亡）→ 下次启动的 sweep 报 **0 个孤儿**；并且那个 pid **确实打不开**了（进程对象也被回收），不只是"没被列出来" |
| `m5_sweep_finds_nothing_after_a_worker_leaves_cleanly` | 真 worker → `shutdown` → 用**保留的句柄**等它退出 → sweep 报 0 个孤儿 |

第二个用例同时说明了**为什么 `KILL_ON_JOB_CLOSE` 不设**是可观测的：
管道关闭时 worker **自己退出**，走的是它自己的有序拆卸路径（删盘符/拆卸/停 AIM）；
如果 job 在 close 时杀成员，走的就是 `TerminateProcess`，留下的正是 sweep 反过来要清的那份残留。

**为什么"无法归属的孤儿"仍没有集成用例**：job 报出的进程**没有 profile 标记**时，
它不是"无法归属"而是**被明确判定不属于任何 profile**，也就是 `terminate_forced` 那条路。
要构造"无法归属"需要一个**被 job 打上 profile 标记**的进程，而 job 打不了这个标记
（谁服务哪个 profile 存在 durable 记录里）。那条路在能构造它的地方测（fake），
**不留空过的用例**。

### 4.7 本轮抓到的**两个"看起来像产品缺陷、实际是我测试的问题"**（都必须记下来）

**① 源码 include 把整个 backend 套件的用例带进来了。**

我为了复用 `TempBundle` / `valid_profile` / `provider` 等 fixture，
`#include "m5_production_execution_backend_tests.cpp"` —— 这是本仓库既有的复用手法。
但它**把那个文件的全部 `TEST_CASE` 一起带进同一个可执行文件**，
而那些用例会**启动真 worker，且部分 worker 会比创建它的用例活得更久**。
于是我的 sweep 用例**跑在它们之后**，看到的是**别的用例留下的孤儿**。

**实测**：`-Repeat 20` → **18/20**，两次失败都在
`second.last_report()->orphans_observed == 0`，
只是那两次恰好有更多"掉队者"。**这不是产品缺陷，差一点就被当成产品缺陷报上去。**

修法：**不再 include 源文件**，把这几个 helper 本地写出来。
重复是这里较小的代价——真正正确的修法是抽一个共享 **header**（已记为后续项），
而"一个会悄悄放大测试世界的源文件 include"**不是** fixture 机制。

**② 一次 sweep 只是"一个瞬间"，而 job 是共享的。**

去掉 include 之后仍有 **1/20** 失败，同一条断言。
原因：job 是**全服务共用的命名 job**，同一进程里别的用例的 worker **可能还在退出过程中**，
而我的用例只读**一次**。sweep 报出一个"正在消失的进程"是**正确行为**——
真实启动也会看到（上个实例的 worker 在父进程死后需要一点时间才走）。

修法：断言**收敛**而不是断言某一个瞬间——`sweep_until_clean` 在 30 秒内反复 sweep，
直到 job 报 0 个孤儿。**这才是服务真正依赖的性质。**

**两个坑的共同教训**：`-Repeat 20` 又一次是唯一发现它们的手段。
两次都不是产品错，但**两次都会被我写成"产品缺陷"**——所以"先怀疑自己的测试"
和"用重复读数确认"是同一件事的两面。

**最终读数**：`m5.startup_sweep` **`-Repeat 20` = 20/20**
（请求 `c2a2c6bf0bbd43baafdb84d3426f82dd`）。

### 4.8 本轮（part 9）：给"能不能回收"这个判断**接上消费者**

**发现**：`reclaimable_resources` 与 `project_observed_identity` 在 `src/` 里
**一个调用点都没有**——实现了、测过了、**没有任何代码问它们**。
这正是设计 §7.3 警告过、而且**本项目已经栽过三次**的"没有生产者/没有消费者"缺陷，
而且它的恶劣之处是**所有测试照旧全绿**。

**修法**：`run_worker_sweep` 增加资源半边，并且是**唯一**的调用者：

- 新增 `IObservedResourceIdentitySource`（durable 记录 → 身份）
  与 `IResourceReclaimer`（证明归属之后**动手**）两个接缝；
- 资源半边**排在进程半边之后**——先终止 worker 再回收它留下的东西，
  这个顺序不会去回收一个**活着的 worker 正在用**的设备；反过来则会和它抢自己的卷；
- 报告新增两个计数：`reclaimable_profiles`（**能证明**归属）与
  `unprovable_profiles`（**不能证明**）。
  **`unprovable_profiles` 一旦非零就计入 needs-attention**——设计 §5.5 要求
  "无法证明归属 → 报告而不是动手"，而一个**默默什么都不回收**的 sweep
  与一个**真的没什么可做**的 sweep 从外面看是一样的；
- **读不到身份 = 致命**（和读不到绑定同理）：没有记录时每一份残留都"无法证明"，
  于是每一次启动都会把所有盘报成 needs-attention；而如果调用方把"无法证明"
  读成"无事可做"，就会**悄悄什么都不做**。

**生产接线，以及它现在能做什么、不能做什么**（都写进代码注释，不靠记忆）：

- 卷的那一半来自 durable profile 记录（`volume_guid` / `mount_target` / GPT 身份），**是持久的**；
- **bundle 的那一半拿不到**：需要 bundle 目录的 **file identity**，而它**不在** durable profile
  记录里——它跟着 bundle registration evidence，今天唯一被持久化的地方是**删除墓碑**。
  所以普通 profile 的身份**必然不完整** ⇒ sweep 报 `unprovable` 而**不回收**。

**这是正确结果而不是占位符**：拿一个谁都没确立过的 bundle 身份去回收一个卷，
就是**凭猜测动手**。生产里的 reclaimer 因此是 `DeferredResourceReclaimer`，
`can_reclaim()` 返回 false ⇒ **已证明的残留仍然计入 needs-attention，而不是被报成已清理**。

**读数**：开发机 `m5.worker_sweep` **14/14**（新增 6 个资源半边的用例，
含"能证明但回收不了必须报 needs-attention"这一条）；
VM `m5.startup_sweep` `-Repeat 5` **5/5**（请求 `df1f78bc01114221b2a02db96363d1a5`）——
本机没有 profile，所以资源半边不动，进程半边照旧。

### 4.10 本轮（part 10）：指纹现在**能装下** bundle 身份——回收缺的就是它

part 9 把回收判断接上了消费者，但**每个 profile 都只能报 `unprovable`**，
因为 bundle 目录的身份**在服务读得到的地方都不持久**。本轮补上这个**能力**。

**为什么这是 store 的改动而不是协议的改动**：worker 的 **status 本来就带
`WorkerBundleEvidence`**（它打开并校验 bundle 时读到的身份）。所以服务**拿得到**它，
只是**留不住**它。

| 交付 | 说明 |
|---|---|
| `WorkerFingerprintRecord::bundle_file_identity` | **追加在最后**，位置是刻意的：这个结构在测试里到处被**按位置**聚合初始化，中间插字段会**静默改写每一个既有初始化式的含义** |
| `amend_bundle_identity` | 与 `record` 分开，因为两件事**到达时间不同**：进程在 launch 时，身份要等 worker 打开 bundle。它**只改不换**，所以 launch 之后才到的 status **不会抹掉**让进程可归属的 pid；而且**没有记录可改时如实报告**，不去**凭空造一条**——一条没有进程身份的指纹**什么都归属不了，却看起来像归属了** |

**本轮四个缺陷，全部是我自己的**（这个文件的解析器**第四次**把我绊倒）：

1. 我把**缺失**的 bundle 字段当成畸形 → 这会让我**launch 时写出的每一条记录都读不回来**
   （launch 时本来就没有身份可写）。两种写法含义相同，现在缺失即"未观测"；
2. `field()` 用**子串**搜 `key=`，于是 bundle 字段缺失时 **`bundle` 匹配到了 `generation=` 里面**
   —— 又是同一个最常见的情形。改为**分词 + 精确匹配键**；
3. 缺失标记 `-` 会带上**行尾换行**，"没有身份"因此被读成"身份畸形"。
   标记改为 `none`，且分词器剥掉结束符；
4. **三个测试用例**把文件写在**带连字符**的名字下，而 store 的键**不带连字符**——
   **这个文件自己的注释里已经警告过同一个坑，我又犯了一次**。

**还有一条值得记**：把字段插在中间**只因为测试会编译**才被发现；
如果这是**线格式或文件格式**里插在中间，就不会有人提醒。

**读数**：开发机 `m5.worker_binding_store` **17/17**（新增 7 个用例：round trip、
缺失 vs 畸形、amend、无记录可 amend、缺字段记录、两种拒绝形状）；
四套逻辑测试全绿；干净构建 0 error 0 warning。
VM `m5.startup_sweep` `-Repeat 5` **5/5**（请求 `9b7855d08e2c4ac6a47b0e60630f4c11`）——
与预期一致：本机没有 profile，新字段**无处生效**。

**仍然开着，而且现在很精确**：**在 worker status 报出 bundle 时调用 amend** 的那一步。
在那之前，这个能力**存在但没人用**——正是 part 9 谈的"没有消费者"模式，
**写在这里以免以后当成意外发现**。之后才是需要它的设备/盘符强杀验收。

### 4.11 尚未接线（T1-03 最后一步）

**强杀验收**：强杀**服务** → worker 自行有序拆卸；强杀 **worker** → 下次启动 sweep 回收不留设备/盘符。
本轮验的是"sweep 会终结一个真孤儿"，**不是**"断电后设备与盘符被回收"——
后者要等 `reclaimable_resources` 的消费者接上（设计 §5.5 的资源回收），
而那需要 worker 报告 bundle/volume 身份并被持久化。**这部分没做，也没有宣称。**
