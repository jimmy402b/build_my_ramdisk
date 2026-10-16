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

### 4.11 本轮（part 11）：**在 worker 读到身份的那一刻把它记下来**

part 10 给了指纹一个**装** bundle 身份的地方，但**没有东西往里放**。本轮补上产生端，
放在**身份唯一可知的那一刻**：worker 刚刚打开并校验了 bundle，而 `register_bundle`
正拿着它读到的结果。

| 交付 | 说明 |
|---|---|
| `IWorkerFingerprintSink`（独立头文件） | **单方法接缝**，而不是 store 本身：`ServiceWorkerControl` 是命令适配器，store 是持久状态——适配器需要的**一次调用**，不是对持久层的依赖。`WorkerBindingStore` 实现它，所以生产路径就是真 store，而行为可以用 fake 测 |
| `register_bundle` 里的调用 | **尽力而为，失败不致命**（决定写在头文件里）：刚创建并校验成功的 bundle 是一块**真能用的盘**，因为一个**指纹**写不进去而拒绝它，等于拿一块能用的盘去换一个记账问题。缺身份**代价有界且可见**：该 profile 的残留变成"无法证明"，下次启动的 sweep **报出来**而不是回收 |

**这闭合了 part 9 打开的那个"没有消费者"缺口**，而且值得点出模式：**本项目已经第四次**
出现"值被产生、被测过、没有任何代码读它"。先是 `reclaimable_resources` 没有调用点，
然后是 `amend_bundle_identity` 没有调用点。**两个现在都接上了，能力是真的而不是潜在的。**

**虚拟机端到端读数（全部为真）**：真 worker、它**创建并 provision** 的真 bundle、
真 `WorkerBindingStore` 作为 sink、中间是真 `ServiceWorkerControl`。
用例先写入一条**没有** bundle 身份的"launch 式"指纹，跑一次真的 `create_and_verify_bundle`，
然后断言 worker 读到的身份**已经持久化**——并且**amend 没有扰动 pid 与 creation time**，
那两个才是让 worker 可归属的字段。
`-Repeat 3` **3/3**（请求 `3dfd843c58d347ca9158f9bc569eca9e`）。
**用 `-Repeat 3` 而不是 20**：这是新增能力用例而非对间歇缺陷的修复，
修复类读数已记在 part 8 的 sweep 上。

**我自己两个错误**（都被编译器抓到）：先把接口声明在 `service_worker_control.h` 里
又让 store 实现它，构成**循环包含**——现在它有自己的头文件；
新用例调用 `wait_for_letter_release()` **没传参**，而那个 helper **本来就带 `letter` 参数**
存在，于是我又写了一个重复的。**是"一个 helper 只做一件事"的既有约定让它浮出来的。**

### 4.12 本轮（part 12）：**把设备残留量出来**——这是 reclaimer 必须清掉的东西

T1-03 的强杀验收一直有一半没量过：**worker 在持有真实、已 provision 的卷时被强杀**。
进程那一半早有覆盖（所以它能不需要设备就写出来），而它要谈的残留——**挂载的卷与已分配的盘符**——
只有在 worker 挂载过之后才存在。

**用例与它查到的东西**：真 worker、真 create、真 provision 的卷、真分配的盘符，
然后对 **provider 自己注册表里的那个 pid** 做 `TerminateProcess`，
再像服务死亡那样丢掉 session。**事后在客机上量到**：

```
workers: 0          进程没了
disks:   1,0        AIM 盘还在
letters: C,Z        盘符还分配着
DriveLetter  FileSystemLabel
        Z    MyRamDisk
```

**进程那一半是干净的，设备那一半不是**——而这正是设计自己的预测，不是意外。
被 `TerminateProcess` 的 worker **无法**删除自己的盘符、拆卸卷或停 AIM。
job **不设 `KILL_ON_JOB_CLOSE`** 的理由是：被**请求**离开的 worker 会把这三件事都做；
而这一个没有被请求。**残留是 reclaimer 的活**，这也正是设计 §5.5 要求持久化身份的原因——
而 part 9–11 造的正是将要清掉它的那套机制。

**残留是被断言为"存在"的，这是刻意的**：只做观察的用例会让 reclaimer **悄悄**落地；
断言为存在，则实现 reclaimer 时**这个用例会变红**，直到断言也改成"已清除"。
**这就是 reclaimer 继承下来的度量。**

**两次尝试自动断言"设备"本身，都放弃了**，原因值得记：
`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` 对着挂载点返回空，
而 `Get-Disk` 在测试里调不到。**从这里做设备探测也是把客机存储栈搞退化的方式**——
本轮清理时 `Set-Disk -IsOffline` **挂住了**，需要**硬重启 VM**。
所以用例断言的是**盘符**（用户可见的那个残留），盘本身按上表**作为人工量测记录**，
**不伪装成自动化断言**。

另加 `WindowsWorkerProcessSource::launched_process_ids()`，让调用方能对本**会话自己的**
worker 动手而不是对孤儿动手。**只给 pid**：句柄保持私有，
因为能关掉句柄的调用方会留下一个"看得见却等不到"的 worker。

**读数**：干净客机上 `m5.production_execution_backend` `-Repeat 1` **1/1**
（请求 `a7fd5b35981f43be88eab47685f76af0`），残留前后各人工量测一次。
**用 `-Repeat 1`** 是因为该用例会 provision 一个真实卷、耗时约 80 秒；
修复类重复性要求已在别处满足，这是**新度量**用例。干净构建 0 error 0 warning。

**未做，且现在边界精确**：清掉残留的 reclaimer。在它存在之前，
`DeferredResourceReclaimer` 报"无法动手"，所以 sweep **报 needs-attention 而不是假装**。

### 4.13 本轮（part 13）：协议动词 + **确认它可做**（这一片不再是设计问题）

**协议**：`WorkerCommand::reclaim_leftover = 16`，**追加而非插入**
（值是上线格式，插入会静默改写它之后的每一个命令）。
operation-scoped + bare status reply，与 `freeze_writes` 同形：
拆卸卷与"移除"是同类动作，必须归属到某个 operation，
而调用方真正需要知道的就是"盘符与卷是否都没了"。

**请求里不带自己的卷身份**，这是刻意的：它需要的（bundle、挂载目标、镜像）
**已经在这台 worker 的 launch spec 里**，线上再放一份就是**第二个可能与实际动手对象不一致的东西**。

**让后续三件变成"有界工作"而不是"设计问题"的发现**：
`Win32VolumeAdapter::find_recovered_volume(disk_number, drive_letter, owner_token)`
（`windows_volume.h:187`）的注释就是
**"Recovery is attach-only: enumerate existing volumes and adopt exactly one candidate
backed by disk_number and mounted at drive_letter"**——
**这正好就是残留的状态**：**接管**一个已挂载的卷，而不是挂载它。
随后 `remove_drive_letter(owned)` 与 `ProductVolume::dismount()` 完成拆卸。
**不需要新的 M3 能力。**

**这同时纠正了上一轮我的一个错判**：我写过 `adopt_existing`"会挂载，而挂载不是拆卸"。
对**生命周期那条路**成立，对 `find_recovered_volume` **不成立**——而后者才是 reclaimer 会用的调用。
计划里的这一片描述已改正。

**读数**：开发机 `m5.worker_protocol_reclaim_leftover_is_operation_scoped`
覆盖两个方向（缺 operation id 的 **reply** 被拒、**request** 也被拒），
协议套件 **39/39**；五套 M5 逻辑测试全绿；干净构建 0 error 0 warning。

### 4.14 part 13–17：reclaimer 的四件（**三件半完成**）

| # | 件 | 状态 |
|---|---|---|
| 1 | 协议动词 `reclaim_leftover = 16` | ✅ part 13（协议 39/39） |
| 2a | `DiskRuntimeMode::reclaim`（**不分配盘符**）+ `drive_letter_present_` 修正 | ✅ part 14 |
| 2b | `WorkerVolumeMode::reclaim_existing`（**追加**）+ worker 处理器 | ✅ part 15 |
| 3a | `ActionKind` + `ServiceWorkerControl::reclaim_leftover`（**覆盖 launch 模式**）+ session client | ✅ part 16 |
| 3b | `LeftoverReclaimer`（生产 `IResourceReclaimer`） | ✅ part 17 |
| 4 | `ServiceRuntime` 接线 | **待做** |

**验收状态**：`m5_backend_a_killed_worker_leaves_no_process_mount_or_device`
**现在仍断言残留存在**（part 12 刻意如此）。接线完成后要把那两条断言改成"已清除"，
并补**否例**（身份不匹配时不清且报 needs-attention），读数**至少 `-Repeat 20`**。

**本增量反复出现的同一形状的失败（值得单独记下来）**：
**一个检查回答了与关键问题相邻的另一个问题，然后"成功地"什么也没做。**
出现 **四次**：

1. `terminate_process` 没要 `PROCESS_QUERY_LIMITED_INFORMATION`
   ⇒ **100% 的终止失败，却表现为安全检查在工作**；
2. 拆卸挂在 `drive_letter_assigned_` 上 ⇒ reclaim 会**什么都不删却报成功**；
3. `reclaim_leftover` 缺 `started_` 前置 ⇒ 会把**还挂着的残留记成已拆**；
4. dispatch `switch` 的 `default: return unrealized(...)` ⇒ 新 action kind
   **静默落进 default**，残留永不被拆，而**所有不针对它的测试照旧全绿**。

**结论**：本增量的危险不是"做错动作"，而是**看起来合理的空操作**。
因此每一处新路径的验收都要问一句"**它失败时会不会报成功**"。

**读数**：干净构建 0 error 0 warning；**28 套件 / 5 失败，5 个全是既有基线**
（`m5.production_execution_backend` 与 `m5.startup_sweep` 为 VM-only，
后者按设计以 exit 2 走 guest guard；另加 `m5.service_operation_store`、
`m5.execution_scheduler`、`unit.m4.profile_store`）。
本改动可能影响的六套 **6/6 绿**，含 `unit.m4.disk_runtime`。

### 4.15 part 20 — 验收读数与"reclaim 卡住"的定位（**未通过，原因已收窄**）

**读数**：`m5.production_execution_backend` `-Repeat 1`，请求
`bbe52c6015344aecac5739de020e1f4e`，**0/1**（整个用例约 150 s 完成，**不是挂死**）。
四个邻居**全绿**（含 part 12 那个量残留的强杀用例），失败只有我新加的这一条：

```
myrd-test: reclaim failed: op=WorkerPipeChannel::read
           msg=pipe transfer exceeded its bounded wait native=0 code=4107
FAIL ...m5_production_execution_backend_tests.cpp:792: reclaimed.ok()
```

**已定位到的事实**（都有代码依据，非推测）：

1. **这个超时是"deadline 驱动"的**：`overlapped_io` 用
   `WaitForSingleObject(event, remaining_ms(context))` 等管道 I/O
   （`m5_worker_private_pipe.cpp:137`）——所以 4107 **就是**操作上下文的 deadline 到了；
2. **把 deadline 提到 120 s（`reclaim_context()`，part 20）之后，它依然超时**
   ⇒ **worker 在 120 s 内确实没有回包**，不是"预算给少了"；
3. 整个用例 150 s 左右结束 ⇒ 时间基本都花在这一个命令上；
4. **失败发生在 `start`**（reclaim 序列 `launch → start → reclaim_leftover` 的第一步实质动作），
   而不是拆卸那一步——`start` 在 reclaim 模式下要做"attach-only 接管已挂载卷"，
   即 M3 recovery + `find_recovered_volume` + 几何校验。

**已排除的**：

- **不是** fixture 容量错（part 19 已修，而且几何校验失败会**立刻失败**，不会超时）；
- **不是** reclaim 没被调用——`reclaim_leftover` 报的是 `WorkerPipeChannel::read` 超时，
  这说明它**已经把命令发出去并在等回包**；
- **不是**用例整体挂死（150 s 正常结束）。

**下一步要用实验区分（尚未做）**：对**被强杀的 worker 留下的残留**做 reclaim 会卡，
那对**一次"干净停止"留下的、没有 AIM 设备在用的卷**做 reclaim 会不会也卡？
这能把"reclaim 本身有问题"和"残留里的 AIM 设备状态让接管卡住"分开。
**若后者不卡，则嫌疑集中在 AIM 设备在 worker 被杀后仍存在，接管时与它冲突。**

**未验证，故不作断言**：本增量到此**没有拿到"残留被清掉"的读数**；T1-03 **不能标 PASSED**。

#### 4.15.1 挂住的那 120 s 是**系统调用阻塞**，不是死循环（客机实测）

按 3 秒粒度轮询客机上的 `myrd-m5-worker`，一轮完整读数
（请求 `85b6b9d900f448339614af53ccc3810c`，0/1）：

```
12:03:35 n=1 5012/cpu0      ← 单一 pid，CPU 基本不动
...（持续 118 s，cpu 只从 0 涨到 0.1）
12:05:33 n=0                ← 消失，正好是 120 s 预算耗尽
```

**读出两件事**：

1. **那 2 分钟里 CPU 只涨了 0.1 s** ⇒ 进程**阻塞在系统调用里**，
   **不是**忙等/死循环。这与 `overlapped_io` 的 deadline 超时完全一致：
   worker 收到命令后**再没回包**；
2. 该 pid **存活时间 = 120 s 预算**，说明**确实是超时把它结束的**，
   而不是它自己崩了。

**还没测到**：采样窗口落在 create 阶段，**没有捕捉到"第二个"（reclaim）worker 是否被拉起**。
把采样推到 t+115 s 之后的那次尝试**没有跑到 reclaim**（见下）。

#### 4.15.2 残留会**累积**，并开始破坏后续运行（重要）

紧接着的一次运行（请求 `5abe3e2d0564496eb06af3c765cd7aa3`）**只用了 2.2 s 就失败**，
失败点是 `m5_production_execution_backend_tests.cpp:544: result.ok()`——
**这是 fixture 里 create 的断言，不是我的 reclaim 用例**。此时客机状态是
`disks: 1,2,0 / letters: C,Y,Z`（前一次失败留下的两块盘、两个盘符）。

**结论**：**一个没被回收的残留会挡住后面每一次运行**。
所以：

- **每次跑这个用例之前必须确认客机干净**（`workers: 0 / disks: 0 / letters: C`）；
- 失败之后**不能直接重试**，必须先硬重启；
- 这也**从另一个角度证明 reclaimer 是必需的**：没有它，一次失败会**持续污染**后续运行。

#### 4.15.3 reclaim 到底卡在**哪一步**：候选已缩到两个（下一步的抓手）

reclaim 模式下 `DiskRuntime::start()` 的**严格顺序**（`disk_runtime.cpp` 行号为准）：

| # | 步骤 | 行 | 说明 |
|---|---|---|---|
| 1 | `acquire_bundle_lock(bundle)` | 83 | 排他写锁 |
| 2 | `recover_complete_image(virtual_bytes)` | 110 | **M3 恢复**（重开并校验 M3 镜像） |
| 3 | `start_aim()` | 115 | **启动/接管 AIM** |
| 4 | `find_recovered_volume(letter)` | 144 | attach-only 找已挂载卷 |
| 5 | `verify_volume(volume, bytes)` | 145 | 几何校验 |
| — | **不分配盘符**（reclaim 与 recover 的唯一分歧） | 174–190 | |

**测量把范围压到"1–3 之一"**：CPU 两分钟只涨 0.1 s（阻塞在系统调用）、
且进程寿命**正好等于 120 s 预算**。

**两个候选（都需要下一轮用诊断区分，本轮不作断言）**：

- **(a) `recover_complete_image`（M3 恢复）**：被强杀的 worker 可能在 bundle/journal 上留下
  未完成状态，恢复路径**等待某个永远不会到来的条件**；
- **(b) `start_aim()`**：被强杀的 worker 所建的 **AIM 设备可能仍在**，
  新的 reclaim worker 再去建/接管同一块盘的设备时**与它冲突并阻塞**。

**(b) 是我更怀疑的一个**，理由：`KILL_ON_JOB_CLOSE` **被刻意不设**，
所以 worker 被 `TerminateProcess` 后**没有任何东西替它移除 AIM 设备**——
这正是"残留"的一部分（part 12 测到 `disks: 1,0 / letters: C,Z` 就是它）。

**另外一件必须说清楚的事**：**reclaim 这条路径此前从未被"跑通过"**。
part 3 的 `LeftoverReclaimer` 用例只用了 fake 的 profile store 与 worker control
（验的是**决策**），part 12 的强杀用例**不调 reclaim**，part 18 的验收用例**至今 0/1**。
所以：**代码编译通过、接线完整，但没有任何一次成功的端到端 reclaim 行为证据**。
**T1-03 因此不能标 PASSED。**

**下一步的抓手（按代价从低到高）**：

1. 在 worker 侧给 reclaim 路径的**每一步**加一条**写到文件**的 trace
   （本项目的既有纪律：先给现象加一条能区分假设的诊断），跑一次即可定位是 2 还是 3；
2. 若定位到 `start_aim`：确认"被杀 worker 的 AIM 设备是否仍在"，
   并决定 reclaim 前是否应先移除该设备（**这会是新的设计细节，需要记录**）；
3. 若定位到 M3 恢复：转为 M3 恢复路径的问题，**优先级更高**（触及不变量 4）。

#### 4.15.4 **诊断完成：卡在 `start_aim`**（part 22，带步骤 trace 的客机读数）

抓手 1 已实现（worker 侧 `MYRD_RECLAIM_TRACE=<file>`，仅 reclaim 模式包装各步，
每步一行、**写后强制 flush**——因为要观察的进程可能**正阻塞在其后那个调用里**，
缓冲或握住句柄的 trace 恰好对最关键的那一步什么都看不到）。
读数（请求 `2552917a3f624a5eab849c7aee2b86d5`，0/1）：

```
2026-10-16T12:25:28.734Z acquire_bundle_lock: begin
2026-10-16T12:25:28.736Z   ok                     ← 2 ms
2026-10-16T12:25:28.736Z recover_complete_image: begin
2026-10-16T12:25:28.852Z   ok                     ← 116 ms，M3 恢复没问题
2026-10-16T12:25:28.854Z start_aim: begin
                                                  ← 此后无线，直到预算耗尽
```

**结论（不再是推测）**：

- **第 1、2 步都是毫秒级成功**，所以 **M3 恢复路径无责**——抓手 3 被排除，
  **不需要上升到不变量 4**；
- **阻塞点是第 3 步 `start_aim`**，即候选 (b)；**抓手 2 成立**。

**阻塞的机制已定位到代码**：`ProductionRuntimeAdapter::start()`
（`runtime_adapter.cpp:342`）会用 `factory.provider.request_timeout = std::chrono::seconds(30)`
去建并启动 `M3AimRuntime`，然后 `impl_->aim_runtime->start()`。
被杀 worker 留下的 **AIM 设备仍在**（part 12 量到的 `disks: 1,0` 就是它），
新 AIM runtime 起来时**与它冲突/等待它**，于是**永远不返回**。

**为什么这是本增量的必然结果而不是意外**：`KILL_ON_JOB_CLOSE` 是**刻意不设**的
（那是为了让"被请求离开"的 worker 走有序拆卸）。代价就是：
**worker 被强杀后，没有任何东西替它移除 AIM 设备**——而这恰好是 reclaimer 要去处理的状态。
所以 reclaimer 必须先处理"设备已存在"这件事，不能假定 AIM 是干净的。

**最后一步要做的（未做，故 T1-03 仍不得标 PASSED）**：
在 reclaim 路径上**先确认/移除遗留的 AIM 设备**，再 `start_aim`；
具体手段需要在 M3/AIM 适配层找一条"查询并清理既有设备"的路径
（**若不存在，则属于设计需要新增的能力，要记录并请示**）。

#### 4.15.5 **机制查清：那个 `DeviceIoControl` 本身没有超时**（part 23）

**AIM 启动路径**（`m3_windows_acceptance.cpp:138` `M3AimRuntime::start()` 起）：
`M3Provider::open` → `provider->start` → `discover_aim_adapter` →
`snapshot_physical_drive_numbers`（**baseline**）→
**`create_proxy_disk`（:187）** → `rescan_aim_adapter`（:201）→
`QUERY_DEVICE` 校验（:222）→ `wait_for_created_physical_drive`（:224）。

**关键代码事实**（这是"为什么会永不返回"的答案）：

- `DirectAimAdapter::create_proxy_disk`（`direct_aim_adapter.cpp:96`）确实**传了 30 秒**：
  `transport_.transact(kCreateDevice, payload, 4, 30)`；
- 但 `WindowsSrbTransport::transact`（`windows_srb_transport.cpp:26`）是
  **同步 `DeviceIoControl`**——`lpOverlapped` 传的是 **`nullptr`**，
  **也没有 `CancelIoEx`**：

  ```cpp
  BOOL ok = DeviceIoControl(
      adapter_, IOCTL_SCSI_MINIPORT,
      frame.value().data(), input_size,
      frame.value().data(), output_size,
      &bytes_returned, nullptr);      // ← 没有 overlapped，没有取消
  ```

- 那个 `30` **只是写进 SRB 帧里给驱动看的**
  （`build_srb_frame(..., timeout_seconds)`），
  **它并不约束 `DeviceIoControl` 自身**。

**结论**：**驱动若不回应，这个 `DeviceIoControl` 会永远阻塞**，
把所有上层超时（30 s provider、120 s 命令预算）**全部穿透**。
这与四次客机读数完全一致：worker 阻塞在系统调用、CPU 几乎不涨、
直到服务侧 deadline 到期才由**服务**放弃，worker 进程则被随之结束。

**为什么会触发**：被杀 worker 留下的 **proxy disk 设备仍在**（`KILL_ON_JOB_CLOSE` 刻意不设），
新的 reclaim worker 再对同一块盘做 `CREATE_DEVICE` / `RESCAN` 时，
**AIM 驱动在这个状态上没有回应**。

**因此修法有两条，且都需要记录（本轮不动手）**：

1. **让驱动调用可超时**（把 `DeviceIoControl` 改成 overlapped + `CancelIoEx`，
   让那个 30 秒真正生效）——**动的是 M0/M3 的驱动通信层**，
   属于"重写某一层"，**按 goal 规则必须停下来问**；
2. **reclaim 前先确认/移除遗留设备**（绕开冲突），但 `M3AimRuntime` 只公开
   `start`/`stop`，`remove_owned_device` 是**私有**的 ⇒
   需要**新增一个公开能力**，同样是设计新增。

**两条都不是"实现细节"，故本轮不自行选择**，已写入 plan 待裁决。

#### 4.15.6 裁决 A 已实施，但**`start_aim` 仍然卡住**——阻塞点比那个 IOCTL 更早（part 24）

**用户裁决：A（让驱动调用真的可超时）**，已实施（见提交 `9b19ec8`）：
`WindowsSrbTransport::transact` 改为 **overlapped + `CancelIoEx`**，
超时后**取消并等待完成**（否则驱动会写已经出作用域的缓冲、
miniport 也会留下无主的请求）；`timeout_seconds == 0` 从"永久等待"改为**拒绝**。
开发机：干净构建 0/0，**33 套 M3/driver/transport 用例全绿**。

**但客机读数仍是 0/1**（请求 `1607fc07ec0842829cb1d3da319d86cd`），trace 与之前**一模一样**：

```
acquire_bundle_lock: begin → ok        (2 ms)
recover_complete_image: begin → ok     (96 ms)
start_aim: begin → 此后无输出
```

**这说明阻塞点比 `create_proxy_disk` 更早**，在 `start_aim` 内部的**前几步**：
`M3Provider::open` → `provider_->start(context)` → `discover_aim_adapter` →
`snapshot_physical_drive_numbers` → 然后才是 `create_proxy_disk`。

**已经排除的**：`src/driver` 与 `src/m3` 里**只有两处** `DeviceIoControl` 调用点——
`windows_srb_transport.cpp`（**已修**）与 `physical_drive_locator.cpp`（不在这条路径上）。
所以这一次**不是同一个原因的重复**，而是**同一段代码里更早的另一个阻塞点**。

**为什么本轮查不到更细**：worker 侧的 trace 只能标出**worker 能看见的边界**
（`start_aim` 是 M4 的一个步骤），而 `M3AimRuntime::start()` 内部有 6 个以上子步骤。
要区分它们，需要**在 M3 层也写 trace**——但目前 trace 机制是 worker 层的私有函数，
**直接从 `src/m3` 调它会跨层且会造成依赖倒挂**。

**因此下一步是一件有边界的工作，而不是继续猜**：把 trace 提升为一个
**公共的、由环境变量开启的诊断设施**（放在 common 层），
然后在 `M3AimRuntime::start()` 的每一步之间打点，跑一次即可定位到具体子步骤。
**本轮到此为止**：IOCTL 的修正是真实且独立的收益（它移除了一个真实的无限阻塞），
但 **T1-03 仍未通过，仍不得标 PASSED**。

#### 4.15.7 提升 trace 后**一步就定位到了**：`rescan_aim_adapter`（part 25）

**已完成提升**：trace 变成 `src/common/diagnostic_trace.{h,cpp}`
（`MYRD_TRACE_FILE` 开启，逐行写并 flush，失败一律吞掉、绝不改变被观察行为），
`src/m3` 因此可以合法打点（它本来就依赖 `src/common`，不存在依赖倒挂）。

**客机读数**（请求 `32aff28d4dbf41f795ff95c69c1fcc61`，0/1）：

```
13:10:08.605Z start_aim: begin
13:10:08.605Z M3AimRuntime::start: provider open
13:10:08.703Z M3AimRuntime::start: provider start
13:10:08.711Z M3AimRuntime::start: discover adapter
13:10:08.713Z M3AimRuntime::start: physical baseline
13:10:08.717Z M3AimRuntime::start: create proxy disk
13:10:08.719Z M3AimRuntime::start: rescan adapter      ← 此后无输出
```

**结论（两层都清了）**：

- **`create_proxy_disk` 2 ms 就返回了** ⇒ **part 23 的 IOCTL 修正是有效且命中目标的**
  （原来它才是被怀疑的无限阻塞点）；
- **真正的阻塞点是 `rescan_aim_adapter`**。

**那个调用本身也没有超时**：`rescan_aim_adapter`
（`adapter_discovery.cpp:170`）只是

```cpp
const CONFIGRET status = CM_Reenumerate_DevNode(
    static_cast<DEVINST>(dev_inst), 0);
```

**`CM_Reenumerate_DevNode` 是同步的 PnP 重新枚举，没有超时参数**，
`0` 是 flags（不是 timeout）。**PnP 重新枚举可以被无限期阻塞**
（等待设备/驱动响应），于是它把上面所有 deadline 全部穿透——
与"worker 阻塞在系统调用、CPU 不涨"的读数完全一致。

**这是 M0/M3 驱动层的第二个无限阻塞点，和前一个不同一个调用。**

**修法（本轮不动手，因为都要裁决）**：

- **A'：不要重新枚举**。reclaim 的前提是**设备已经存在**（被强杀 worker 留下的），
  而 `start_aim` 现在**先 `create_proxy_disk` 再 rescan** ——
  对残留而言这两步**可能根本不该做**。若 AIM 已能为该 bundle 提供磁盘，
  正确做法是**接上它**而不是再造一个再重新枚举。
  **这是"reclaim 应该走一条更窄的 AIM 启动路径"的设计问题**，而不是打补丁；
- **B'：给 `CM_Reenumerate_DevNode` 加超时**：CfgMgr32 **没有**带 timeout 的版本，
  唯一办法是把整个调用**移到一个可放弃的线程**里等——
  而"放弃"一个正在 PnP 里的线程是有风险的（它可能稍后继续动设备），
  **比前一个 IOCTL 的 `CancelIoEx` 危险得多**，不该轻率实施。

**倾向 A'**：它不试图去"超时一个不该发生的操作"，而是**不发起那个操作**。
但 A' 需要设计判断（reclaim 的 AIM 启动路径到底要不要建新设备），
**故按 goal 规则停下来请示**，不自行发明。

#### 4.15.8 裁决 A' 已实施；**两次读数都无效**（part 26）

**用户裁决 A'**，已实施（提交 `789be78`）：`M3AimRuntime::start` 在创建之前
**先问驱动已经持有什么**；若某个设备的**三项身份**（大小、扇区大小、背后的文件）
与该 provider 的 bundle 全匹配，就**接管它并完全跳过重新枚举**；
匹配任一项不符就不接管（**避免误接管别的 profile 的设备**——这是这条路径唯一可能造成伤害的方式）。
接管的设备随后仍要过 create 路径**同一道**身份校验。
创建路径**逐字未动**（包括它的 rescan：新建的设备确实需要枚举）。
开发机：干净构建 0/0、**33 套 M3/driver/transport 全绿**。

**但两次客机读数都无效**，都不是"reclaim 失败"：

- 第 1 次（请求 `bd745850b0f046279e11d00f7c83fbdd`）：客机**仍是脏的**（前几次失败留下的
  `disks: 1,2,0 / letters: C,Y,Z`），套件**死在 fixture 自己的 create 断言**（`:544`），
  **根本没跑到 reclaim**；trace 文件是**上一次运行留下的陈旧内容**（时间戳 `13:10`），
  **这正是我判断"新代码没被执行"的依据**；
- 第 2 次（请求 `2790158fde704370a244de3a60349094`，客机已先硬重启确认干净）：
  仍是 0/1，但**取不到失败原因与 trace**——该次运行后**客机再次失去 SSH 响应**
  （内层 PowerShell 命令挂住），与 7.0.4 记录的现象一致。

**因此 A' 的正确性仍未验证，不作任何断言。** 客机已硬重启并确认干净。

**下一次读数前必须先做的事（重要，避免重复浪费）**：

1. **先确认客机干净**（`workers: 0 / disks: 0 / letters: C`）——脏客机连 create 都过不去；
2. **先删掉 `C:\tmp\m5-reclaim-trace.txt`**，否则会读到上一次的陈旧内容并误判；
3. 取读数**先只读 stderr 第一行**（能区分"死在 fixture create"与"死在 reclaim"），
   再决定是否读 trace。

#### 4.15.9 **A' 的读数拿到了：attach 跑了，但没匹配上**（part 27，本轮）

**本轮按新规则严格执行**：先确认客机干净（`workers: 0 / disks: 0 / letters: C`）→
**删掉陈旧 trace** → 再跑。**另发现一件对以后很重要的工具事实**：
`tools/m5vm` 会把 `results.json` 与 `response.txt` **落到本机 `C:\tmp\m5-vm\`**，
但**只记录客机端路径**；当客机 SSH 再次挂住时，
**可以用 `scp` 直接取**（本轮就是这么拿到 stderr 与 trace 的，`scp` 与挂住的 SSH 执行器是两条通道）。

**读数**（请求 `1b33bb7ada3c4408af2efc5290e2dfd8`，`-Repeat 1`，客机已确认干净）：

```
13:45:34.733Z start_aim: begin
13:45:34.857Z M3AimRuntime::start: look for an existing device   ← 新代码跑了
13:45:34.871Z M3AimRuntime::start: create proxy disk             ← 但**没接管**
13:45:34.876Z M3AimRuntime::start: rescan adapter                ← 于是仍去重新枚举 → 卡住
```

stderr：`pipe transfer exceeded its bounded wait`（4107，deadline 到期）。

**结论：attach 路径被执行了、`list_devices()` 也返回了（14 ms），但没有匹配到设备**，
于是走了原路（create + rescan）并**再次卡在 `CM_Reenumerate_DevNode`**。

**一个很可能的、需要下一轮验证的原因（本轮不作断言）**：
匹配条件里的 `native_object_path` 来自 `names.driver_path`，而它是**由 session id 派生的**：

```cpp
// src/proxy/object_names.cpp:61,69
std::wstring base = prefix + std::wstring(session_id);
names.driver_path = std::wstring(L"\\BaseNamedObjects\\") + base;
```

**如果两个 worker 派生出不同的 session id，`driver_path` 就不同，残留设备就永远匹配不上**——
那样 **A' 就按现在的身份判据不成立**，需要改成
"用**能从 durable 数据导出**的判据去认领残留设备"（这正是本增量 part 11 为 bundle 身份做过的事）。

**下一步（下一轮第一件事）**：把 create worker 与 reclaim worker 各自算出的
`session_id` / `driver_path` 打进 trace 对比一次——**一次运行即可判定**是不是这个原因。
若是，则修法是让判据不依赖 session id（或让 session id 从 profile/bundle 派生）。

#### 4.15.10 **假设被证实，而且比预想更硬**（part 28）

**读数**（请求 `3eb1fd0c95474ae7805a87ad05f28f55`）——把两侧都打进 trace 后**一次就看清了**：

```
looking for  path=\BaseNamedObjects\Global\myrd-m1-c91904d39d537e22227c225b5d8f058e
candidate 256 size=67108864 sector=512 path=...myrd-m1-7f82a121d61ef3896c547049567cb2ff
candidate 0   size=67108864 sector=512 path=...myrd-m1-d332e262792f35296e1cfc2668d66762
```

**大小相同、扇区相同、只有名字不同** ⇒ 设备一直在，**只有名字对不上**。
而名字**永远**对不上：`generate_session_id` 用 `BCryptGenRandom` 生成
（`src/proxy/object_names.cpp:19`）⇒ **每个 worker 随机**；
残留带的是**已死 worker** 的 id，而**没有任何 durable 数据记录它**。
所以原来的判据不是"没满足"，而是**不可能满足**。

**这是"把两侧都打出来"的价值**：第一版只报 found / not found，
把"设备不存在"和"身份不匹配"报成同一件事——而这是**两个方向相反、修法相反**的问题。

**已改为按几何匹配**（size + sector）。安全性**不来自名字而来自更好的地方**：
本 runtime 已持有 bundle 的**排他写锁** ⇒ 不可能有别的 worker 在服务这个 bundle；
而且匹配的这个 size 正是 AIM 创建时用的值、也正是随后 `verify_volume` 检查的值。

**接管带来的两个后果，都已处理或已记录（两者都本来是"静默"的）**：

1. **已修**：`start` 之后那道身份校验会比对 `native_object_path`，
   而接管的设备**不可能**满足它；那条失败分支会**移除设备**——
   所以第一版会**先把残留删掉、然后报失败**。现在名字比对**只对新建设备要求**，
   几何则两者都要求；
2. **未修，下一件**：`wait_for_created_physical_drive` 是靠"**与启动时 baseline 比较**"
   来找盘的，而**接管的设备其磁盘本来就在 baseline 里** ⇒ 永远看不到"新创建的"盘。
   接管路径需要**按 device id 直接定位**，而不是靠 baseline 的差集。

开发机：干净构建 0/0、**33 套 M3/driver/transport 全绿**。
**本改动的客机读数未取、不作断言。**

#### 4.15.11 剩余片的两个后果都修完后，**验收用例仍然 0/1——但这次原因完全不同**（part 29）

**已实施**：`wait_for_present_physical_drive`（attach 模式按 device 身份定位，
而不是靠 baseline 差集；`match_created_disk` 会跳过 `existed_before` 的候选，
对"接管一个本来就在的盘"恰好是错的）。开发机：干净构建 0/0、**33 套全绿**。

**顺带确认的一件既有事实**：`unit.m2.windows_acceptance` 在本机**本来就失败**
（`m2_windows_acceptance_tests.cpp:88`）。**已在 HEAD 上用"先撤改动再跑"验证过它同样失败**
⇒ **属既有开发机失败，与本改动无关**（这条不是假设，是量过的）。

**读数**（请求 `3596cc32053f411394aec6da16be6e36`，0/1）：

```
myrd-test: reclaim-case create failed: op=provision_new_volume
           msg=provisioning helper failed native=1 code=21001
FAIL ...m5_production_execution_backend_tests.cpp:744: created.ok()
```

**这一次失败在"我这条用例自己的第一阶段（create）"**，而不是 reclaim；
而且**trace 文件不存在** ⇒ **reclaim 代码根本没被执行**。
客机当时是**干净的**（`disks: 0 / letters: C`），四个邻居全绿。

**根因是一个用例间级联**：紧邻的 `m5_backend_a_killed_worker_leaves_no_process_mount_or_device`
**按设计会留下残留**（它断言残留存在），而**两条用例的虚拟大小相同（64 MiB）**；
于是下一条用例的 create 在 provision 阶段失败（`21001`）。

**因此有一件事必须写清楚**：**目前这两条设备类用例不能在同一个客机会话里相继运行**，
除非中间清场。这正是 part 12 记录过的"残留会累积并破坏后续运行"，
现在确认它**就在同一套件内部发生**，不只是跨运行。

**要做的修法（不是打补丁，需按项目规则处理）**：
让这两条用例**各自在自己的会话里自清**
（例如断言残留之后**自己把它 reclaim 掉**再结束），
而不是依赖"跑之前硬重启客机"。这条改动**同时验证 reclaimer 本身**，
是最值得做的下一步——但**它超出本轮的预算**，故记录在此。

**另一条应当固化的纪律**：`REQUIRE(result.ok())` 一律**先打印 error 再断言**
（本轮又是这样才拿到 `op=provision_new_volume`）。**同一用例里的下一个断言也要一起加**，
否则只是把下一次盲猜往后挪一格——本轮正是如此。

#### 4.15.12 级联已从源头修掉；**cleanup 那条 reclaim 仍然超时**（part 30，本轮）

**已实施**：`m5_backend_a_killed_worker_leaves_no_process_mount_or_device`
**先断言残留存在、然后自己把它 reclaim 掉**（`394d8d0`）。这从源头去掉了
"两条设备用例必须靠重启客机才能相继运行"的环境依赖，
并且**让这条用例成为"reclaimer 能处理强杀留下的残留"的证明**——
顺序也是唯一诚实的那种：先断言它在，再断言它没了。

**读数（两条，都是 0/1，但都不是"create 失败"了）**：

| 请求 | 失败点 | 现象 |
|---|---|---|
| `b78559db100d4f56915198c0c1ec30fe` | `:736 reclaimed.ok()`（**cleanup 的 reclaim**） | `pipe transfer exceeded its bounded wait`（4107） |
| 第二轮（cleanup 已加 trace 后） | 未取到 | **超过 30 分钟无结果**，且**trace 文件根本没被创建**，`scp` 取不到；已终止并硬重启客机 |

**关键结论（本轮最有价值的一点）**：**级联确实修好了**——
失败点从"下一条用例的 create（`provision_new_volume` 21001）"
变成了"**这条用例自己的 cleanup reclaim 超时**"。
也就是说：**现在卡住的正是 reclaimer 本身，而不是测试环境。**

**trace 文件没被创建这一点本身也是信息**：说明
`M3AimRuntime::start` 的**第一个打点**（`provider open`）都没到——
即 worker 在 `start_aim` **之前**就没有回应，
或者这次运行的 worker 根本没能进到 `M3AimRuntime::start`。
这与"cleanup 用的 durable record 让 reclaim 走了另一条路"一致，**但本轮无法判定**。

**下一步（下一轮第一件事）**：用**单次、带 trace、并在跑之前确认 trace 文件不存在**的方式跑，
并在**运行中**（而不是结束后）用 `scp` 抓一次 trace——
本轮是在"结束之后"抓，而客机那时已经不稳，两次都取不到。
**先把"能不能在运行中抓到 trace"解决掉**，否则下一轮还会卡在同一处。

#### 4.15.13 **证据拿到手了：cleanup 的 reclaim worker 卡在 `M3AimRuntime::start` 之前**（part 31）

**本轮把取数问题解决了**：跑之前确认 trace 不存在、运行中每 20 秒 `scp` 一次。
读数（请求 `9a941ec229454b32bfd5e3cf1bb79712`，0/1，约 140 s）：

- stderr：`residue cleanup failed: op=WorkerPipeChannel::read ... bounded wait`（4107），
  失败点 `:736`（**cleanup 的 reclaim**，不是 create）；
- **`C:\tmp\m5-reclaim-trace.txt` 在整个运行期间从未出现**（运行中抓不到、结束后 `dir` 也不存在）；
- **运行结束后客机上仍留着一个 worker**：`pid 3212`，启动时刻与 reclaim 同时，
  **CPU 只有 0.28 s**（与"阻塞在系统调用"一致）——**这是本次运行留下的孤儿**，
  也说明**测试超时后 worker 并不会自己退出**（服务侧放弃，进程留着）。

**这一条把范围显著收窄，而且是新信息**：

| 用例 | worker 是否到达 `M3AimRuntime::start` |
|---|---|
| reclaim 用例（part 25/27 的读数） | **是**（trace 里有 `provider open` 等每一步） |
| **cleanup（本轮）** | **否**（**连第一个打点都没有**） |

⇒ **cleanup 的 reclaim worker 卡在更早的地方**：
在 `M3AimRuntime::start()` **被调用之前**——即 worker 自身启动 / runtime 打开 /
或**连接私有管道**这些环节之一。

**为什么会不一样（假设，未验证）**：cleanup 紧跟在**强杀**之后；
而 reclaim 用例是**新 phase、新 job、新 control**，间隔更久。
**最可疑的是"上一个 worker 刚被杀、其私有管道/名字对象的清理还没完成"**，
于是新 worker 在**连管道**时等待一个不会再被满足的条件。

**下一轮第一件事（一次运行即可判定）**：在**更早**的位置打点——
worker 主流程的每一阶段（进程起来、读到 launch spec、打开 runtime、连接管道、发首帧），
而不是只从 `M3AimRuntime::start` 开始。**现有 trace 的第一个点太靠后**，这正是它这次一无所获的原因。

#### 4.15.14 **A' 成功了——接管、定位、找卷、校验全部通过**（part 32，本轮）

**先更正上一轮的一个误判**：上一轮"trace 文件从未出现"**不是**"worker 没到达"，
而是**那次运行的 trace 没有生效**（`_wputenv_s` 没传到那个 worker，或逐次开关文件不可靠）。
本轮在 worker 启动各阶段加打点后，**同一个 cleanup 路径给出了完整 trace** ——
所以"阻塞在 `M3AimRuntime::start` 之前"这个结论**作废**，撤回。

**读数**（请求 `5a3542185eb7464a8b022ef43d5219c3`，0/1）：

```
15:27:42.002Z M3AimRuntime::start: look for an existing device
15:27:42.003Z   candidate id=0 size=67108864 sector=512 path=...myrd-m1-39f95bee...
15:27:42.003Z M3AimRuntime::start: ADOPTING the existing device    ← A' 生效
15:27:42.003Z M3AimRuntime::start: query device
15:27:42.003Z M3AimRuntime::start: wait for physical drive
15:27:42.119Z   ok                                                  ← present-wait 生效
15:27:42.119Z find_recovered_volume: begin
15:27:42.121Z   ok
15:27:42.121Z verify_volume: begin
15:27:42.121Z   ok
15:27:42.214Z worker: command received                               ← 随后是 reclaim 的拆卸
```

**这一轮换来的确定结论（多且都是正的）**：

1. **A' 的设计是对的**：设备**名字不同、几何相同**，被判据**正确接管**；
2. **`find_existing_device` 的几何匹配生效**；
3. **`wait_for_present_physical_drive`（part 29 新增）生效**——接管的盘**确实被找到**，
   这正是"靠 baseline 差集找盘"永远做不到的；
4. **`CM_Reenumerate_DevNode` 被绕开了**：走的是 ADOPTING 分支，**没有 rescan**；
5. 接管后的**身份校验也没再误删设备**（part 28 修的那点确实必要）。

**剩下的是最后一段**：trace 停在 `verify_volume: ok` 之后、
`worker: command received` 之前的那一小段——
即 **`DiskRuntime::start()` 已经成功返回，接下来执行 `reclaim_leftover` 的拆卸**
（`cleanup` → `shutdown_clean`：删盘符 / 拆卸卷 / **停 AIM** / 释放 bundle 锁），
**其中某一步阻塞**（stderr 仍是 4107 超时）。

**下一轮第一件事（已定位到很小的范围）**：给**拆卸的每一步**打点
（`remove_drive_letter` / `dismount_volume` / `stop_aim` / `release_bundle_lock`），
**一次运行即可指出是哪一步**。**注意**：`stop_aim` 用的是同一个
`M3AimRuntime::stop()`，而 part 23 只让 **SRB 传输**可超时——
若 `stop` 走的是别的系统调用（或 PnP），**可能还有第三个无限阻塞点**。

#### 4.15.15 **拆卸 trace 一步定位：卡在 `flush_volume`**（part 33，本轮）

**读数**（请求 `58bbafd11752431a9549036c2082cb39`，0/1）：

```
15:36:12.902Z find_recovered_volume: begin → ok
15:36:12.909Z verify_volume: begin → ok
15:36:13.023Z worker: command received
15:36:13.023Z flush_volume: begin        ← 此后没有任何输出
```

**结论：卡在拆卸的**第一步 `flush_volume`**，不是 `dismount` / `stop_aim` / 释放锁。**

**为什么拆卸里会有 flush**：`shutdown_clean_locked` 在 `started_` 为真时
**先 flush 再删盘符**（`disk_runtime.cpp:395-404`），而采用接管路径的 reclaim
启动完成后 `started_` 就是真 ⇒ **它会对"接管的那个卷"做一次 flush**。

**这一步没有任何超时**：`flush_volume` 的实现经 `Win32VolumeAdapter::flush`
（走卷句柄的 `DeviceIoControl` 类调用 / 卷缓存刷写），
而 part 23 只让 **SRB 传输**（`windows_srb_transport.cpp`）可超时。
**所以这很可能是第三个无限阻塞点，且和第二个同类**（同步、无超时）。

**下一步（范围已经很小）**：
给 `Win32VolumeAdapter::flush` 内部打点/加界，
或者先确认它到底阻塞在哪个系统调用上（卷锁？`FlushFileBuffers`？`FSCTL`？）。
**若确认同样缺少超时，那就和 part 23 是同一个修法**——
把该调用也变成有界可取消的，而不是再绕开它。

#### 4.15.16 **flush 也被清白了；真正的阻塞点是 `dismount_volume`**（part 34，本轮）

**读数**（请求 `72fe36db65bb49eea3a8c9da833e6ea5`，0/1）——把 flush 内部两个调用都打点之后：

```
15:44:19.973Z flush_volume: begin
15:44:19.973Z   Win32VolumeAdapter::flush: opening the volume
15:44:19.973Z   Win32VolumeAdapter::flush: volume opened, flushing buffers
15:44:19.975Z   Win32VolumeAdapter::flush: done          ← 2 ms，清白
15:44:19.975Z   ok
15:44:19.975Z remove_drive_letter: begin
15:44:19.977Z   ok                                       ← 2 ms，清白
15:44:19.982Z dismount_volume: begin
                                                          ← 此后无输出
```

**结论：拆卸里真正卡住的是 `dismount_volume`。**
`flush_volume` 与 `remove_drive_letter` **都已证明是好的**（各 2 ms）。
这也再次说明**逐步打点是对的**：上一步只知道"卡在 flush"，
而这一次把 flush 内部也打点后，才看到**它早就过了 flush**。

**当前完整的拆卸阻塞点清单（每一步都有实测）**：

| 步骤 | 结果 |
|---|---|
| `flush_volume`（含内部 open / `FlushFileBuffers`） | ✅ 2 ms |
| `remove_drive_letter` | ✅ 2 ms |
| **`dismount_volume`** | **❌ 阻塞（4107 超时）** |
| `stop_aim` | 未到达 |
| `release_bundle_lock` | 未到达 |

**下一步（下一轮第一件事）**：查 `dismount_volume` 的实现
（`Win32VolumeAdapter::dismount` / `ProductVolume::dismount`，
很可能还有卷锁释放 / `FSCTL_DISMOUNT_VOLUME` / `IOCTL` 这类**同步且无超时**的调用），
**极可能是第三个（或第四个）同类无限阻塞点**——与 part 23 是同一个修法方向：
**给调用加界并使其可取消**，而不是继续绕。

#### 4.15.17 **`dismount` 是第四个同类无限阻塞点，而且这已经是系统性问题**（part 35，本轮）

`LockedVolume::dismount()`（`windows_volume.cpp:641`）就是那个调用：

```cpp
if (!DeviceIoControl(handle_, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr))
```

**同步 `DeviceIoControl`、`lpOverlapped = nullptr`、没有任何界**——
**与 part 23 在 SRB 传输里修掉的是同一个缺陷形状。**

**本增量至今遇到的四个无限阻塞点，都是同一类**：

| # | 位置 | 形状 | 状态 |
|---|---|---|---|
| 1 | `windows_srb_transport.cpp` | 同步 `DeviceIoControl`，`timeout` 只写进 SRB 帧 | **✅ part 23 已修**（overlapped + `CancelIoEx`） |
| 2 | `CM_Reenumerate_DevNode`（PnP） | 同步、API 无 timeout 形参 | 已被 A' **绕开**（不重新枚举） |
| 3 | `Win32VolumeAdapter::flush` | 曾怀疑；**实测 2 ms，清白** | 不需要修 |
| **4** | **`FSCTL_DISMOUNT_VOLUME`** | **同步 `DeviceIoControl`、无界** | **本轮定位，未修** |

**这是一条值得写进结论的系统性事实**（而不只是四个 bug）：

> **本项目的设备路径大量使用同步、无界的 `DeviceIoControl` / PnP 调用。
> 只要驱动或 PnP 在一个不该慢的状态下不回应，调用方就没有任何办法放弃。**
> part 23 已经把其中一个变成有界可取消的；**剩下的是同一件事再做一些**，
> 而不是"再多绕一个"。

**修法方向（与 part 23 一致，且已证明可行）**：把 `FSCTL_DISMOUNT_VOLUME`
改成 **overlapped + `CancelIoEx`**，超时后取消并等待完成。
**前提**：`LockedVolume` 的句柄必须是以 `FILE_FLAG_OVERLAPPED` 打开的——
这一点**未确认**，是本改动第一步要查的（若没有，需要在取锁时就带上）。

**本轮到此为止**（上下文预算见底）：**定位完成，修法方向明确且与已成功的 part 23 同型**，
**但未实施、未取读数，故不作任何"已修好"的声明，T1-03 仍不得标 PASSED。**

**并且本轮把那个前提查清了，结论不好但重要**：
**`src/run/windows_volume.cpp` 里完全没有 `FILE_FLAG_OVERLAPPED`** ——
也就是说**被锁定的卷句柄是同步打开的，因此 `CancelIoEx` 对它根本用不了**。

⇒ **part 23 的成功经验不能照搬**：
那次 SRB 传输的句柄本来就支持 overlapped（管道/设备句柄是那样打开的），
而这次**必须先改"取锁时以 overlapped 打开句柄"**，
才谈得上把 `FSCTL_DISMOUNT_VOLUME` 变成可取消的。
**这是一处更大的改动**（触及 M4 取卷锁的路径），
**因此下一轮的第一件事不是直接改代码，而是先判定**：
"以 `FILE_FLAG_OVERLAPPED` 打开被锁定的卷"是否会影响 M4 既有的锁语义与证据——
**若是，则又一次属于"重写某一层并重挣 M3/M4 证据"，需要请示后再动。**

#### 4.15.18 `dismount` 已加界（part 36），**但读数显示卡点还在 `dismount` 内部更早处**

**裁决的前提问题本轮自己查清了，答案是"不用担心"**：
独占性是 **share mode = 0**，而 `FILE_FLAG_OVERLAPPED` **只影响 I/O 完成的通知方式**，
**不改锁与共享语义**；不用 `OVERLAPPED` 的调用方行为与之前**逐字相同**。
同句柄上另两个调用（`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`、`FlushFileBuffers`）
在 overlapped 句柄上都是安全且文档允许的。
**所以这不是"重写某一层"，只是加一个 flag**——本轮据此直接实施，未再请示。

**已实施**（`407d259`）：`LockedVolume::dismount` 改为
**overlapped + `CancelIoEx` + 10 s 界**，超时**取消并等待完成**，
失败按**拒绝**返回（引擎对"未能完成的拆卸"本来就是保留卷并报错，
**而无界等待永远到不了这个结果**）。
开发机：干净构建 0/0、**M4/M3/volume 43/44 绿**（唯一失败是既有的 `unit.m4.profile_store`）
——对一次触及 M4 锁路径的改动，这就是需要的证据。

**但读数（请求 `9a1e7af2eaf4469883bfbfb5346108c5`，0/1）显示卡点更早**：

```
15:55:24.216Z remove_drive_letter: begin → ok
15:55:24.223Z dismount_volume: begin
                                      ← 无输出；**我新加的那条 `LockedVolume::dismount` 打点根本没出现**
```

⇒ **阻塞发生在 `dismount_volume` 这一步内部、`LockedVolume::dismount()` 之前。**

**原因已经定位到代码**：`ProductionRuntimeAdapter::dismount`（`runtime_adapter.cpp`）
**有两个分支**：

- `held.has_value()` ⇒ **`held->dismount()`**（我加界的那条）；
- 否则 ⇒ **`impl_->volume_adapter.lock(volume, 30000)`**，
  **这条路径会重新取锁**，而且它的循环体是 `CreateFileW(..., 0 sharing, ..., FILE_FLAG_OVERLAPPED)`。

而本次运行的 trace 里 **`flush` 走的是 `Win32VolumeAdapter::flush`**
（不是 `LockedVolume::flush`）⇒ **reclaim 的 runtime 没有持有锁（`held` 为空）**
⇒ **走的是重新取锁那条分支**。

**下一轮第一件事（范围极小）**：给 `ProductionRuntimeAdapter::dismount` 的
**两个分支各打一条点**，以及 `Win32VolumeAdapter::lock` 的**重试循环每次打一条**——
**一次运行即可看出是"取锁一直在被拒"还是"取锁之后就卡住"。**
**注意**：`lock` 有 30 s 预算，而现象是**超过 120 s 都没回包**，
所以要么重试循环比预算更久，要么阻塞在预算管不到的地方——
**这正是下一条打点要分清的两件事。**

#### 4.15.19 **分清了：卡在 `lock` 里那句 `CreateFileW`，连一次重试都没发生**（part 37，本轮）

**读数**（请求 `fc741c8ce1a84a5ab85114c7204aa0a6`，0/1，trace 共 36 行）：

```
16:04:01.267Z remove_drive_letter: begin → ok
16:04:01.277Z dismount_volume: begin
16:04:01.280Z dismount: re-acquiring the lock      ← 分支确认（不是 held 那条）
                                                    ← 之后什么都没有
```

**两条新打点给出了一个干净的二选一结果**：

- **`open refused, retrying` 出现 0 次**；
- **`opened exclusively` / `gave up opening` 两条也都没出现**。

⇒ **它既没有被拒绝、也没有打开成功、更没有耗尽 30 s 预算**，
而是**卡在 `lock` 里那句 `CreateFileW` 上**（`windows_volume.cpp:880`）——
**在它返回之前，循环体一次都没走完**。

**这就是"预算管不到的地方"**：`timeout_ms` 只约束**重试循环的总时长**，
而**单次 `CreateFileW` 本身没有任何界**。
所以一个卡住的独占打开会**穿透那 30 s**，也就解释了"现象超过 120 s"。

**为什么独占打开会卡、而 flush 的打开不卡**：两者差在**共享模式**——
- flush：`GENERIC_READ|GENERIC_WRITE` + **share read/write** ⇒ 成功（实测 2 ms）；
- lock：`GENERIC_READ|GENERIC_WRITE` + **share 0（独占）** ⇒ **卡住**。

即：**这个卷上还存在着另一个打开**（被强杀的 worker 留下的），
**拒绝共享的打开因此等待**，而它等的东西永远不会来。

**这正是"残留"的另一面**：part 12 量到的残留在 UI 上是"盘符还在"，
**在这里是"卷上还挂着一个没人会关的句柄"**。

**下一轮的下一步（方向已明确）**：让这次独占打开**可放弃**。
`CreateFileW` 没有超时参数，但卷上的独占打开等待的是卷的强制锁，
可由 **`CancelSynchronousIo`（对发起线程）或改成 overlapped 打开**来放弃。
**注意**：`lock` 的句柄**已经是 `FILE_FLAG_OVERLAPPED`**（part 36 加的），
所以这里**很可能可以直接用"overlapped 打开 + 超时 + `CancelIoEx`"**——
**但这一点必须先确认（未经实测，不得当作结论）**。

#### 4.15.20 **真正的那个调用是 `FSCTL_LOCK_VOLUME`——第五个无界调用**（part 38，本轮）

上一节把范围压到 `lock` 内部之后，**直接读代码就找到了那一句**（`windows_volume.cpp:909`）：

```cpp
DWORD ignored = 0;
const BOOL ok = DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr);
```

**同步 `DeviceIoControl`、`lpOverlapped = nullptr`、没有任何界** ——
**与 part 23（SRB 传输）、part 36（`FSCTL_DISMOUNT_VOLUME`）是同一个缺陷形状。**

**语义上也完全对得上**：`FSCTL_LOCK_VOLUME` 就是"把这卷锁起来"，
**当卷上已经有一个别人打开的句柄时，它必须等待**——
而那个句柄属于**被强杀的 worker**，**永远不会关**。
所以它**永远等下去**，`timeout_ms` 那 30 s 只约束重试循环、管不到这一句。

**这与本轮读数完全一致**：`open refused, retrying` **0 次**（说明 `CreateFileW` 成功了，
独占打开**没有**被拒），随后 `opened exclusively` 之后就该是这一句，
**而它没有返回**。

**五个无界阻塞点的完整清单（全部有实测或代码依据）**：

| # | 位置 | 状态 |
|---|---|---|
| 1 | `windows_srb_transport.cpp` 的 `DeviceIoControl` | **✅ part 23 已修**（overlapped + `CancelIoEx`） |
| 2 | `CM_Reenumerate_DevNode`（PnP） | 已被 A' **绕开** |
| 3 | `Win32VolumeAdapter::flush` | 实测 2 ms，**清白** |
| 4 | `FSCTL_DISMOUNT_VOLUME` | **✅ part 36 已修** |
| **5** | **`FSCTL_LOCK_VOLUME`** | **本轮定位，未修** |

**修法**：与 part 36 **完全同型**，而且**前提已经满足**——
`lock` 拿到的句柄**已经是 `FILE_FLAG_OVERLAPPED`**（part 36 加的那个 flag 正好是这里需要的）：
把它改成 **overlapped + `CancelIoEx` + 界**，超时即**返回 `busy`**
（`busy` 在既有语义里就是"预期中的观察，调用方可以保留 runtime 并重试"，
**比"永久挂住"好得多**）。

**本轮到此为止**（上下文预算见底）：**定位完成、修法明确、前提已具备，
但未实施、未取读数——故不作任何"已修好"的声明，T1-03 仍不得标 PASSED。**
**下一轮第一件事**：按 part 36 的成例把 `FSCTL_LOCK_VOLUME` 变成有界可取消的。

#### 4.15.21 **更正：卡的不是 `FSCTL_LOCK_VOLUME`，而是那句独占 `CreateFileW`**（part 39–40）

**`FSCTL_LOCK_VOLUME` 已按 part 36 的成例加界**（`1827150`：overlapped + `CancelIoEx` +
调用方预算，超时返回 **`busy`**——该函数的契约本来就把 `busy` 定义为
"预期中的观察，调用方可保留 runtime 并重试"）。开发机：干净构建 0/0、
**M4/M3/volume 43/44 绿**（唯一失败是既有的 `unit.m4.profile_store`）。

**但读数（请求 `ba48cd78e6f64d1380e0dfe5e952aee3`，0/1）说明我上一轮的推断错了**：

```
16:15:20.732Z dismount_volume: begin
16:15:20.734Z dismount: re-acquiring the lock
              ← 没有出现 "Win32VolumeAdapter::lock: opened exclusively"
```

**我新加的那条"打开结果"打点就在 `CreateFileW` 之后第一行，它没出现**
⇒ **阻塞的是 `CreateFileW` 本身**，不是它后面的 `FSCTL_LOCK_VOLUME`。

**必须更正的我自己的推断**：part 38 我写"`open refused, retrying` 0 次 ⇒ 说明独占打开成功了"。
**这是错的**：重试打点只在 `CreateFileW` **失败且错误是 busy 类**时才打，
**"0 次"只说明它没有失败，不能说明它返回了**。**一条不出现的打点被当成了"成功"的证据**——
这正好是本增量反复出现的那个形状（**把"没有报错"读成"做成了"**），
只不过这次是我在**读证据**时犯的，而不是在写代码时。

**因此当前事实是**：`lock` 里的 **`CreateFileW(..., share 0, ...)` 会无限期阻塞**
（卷上还挂着被强杀 worker 留下的句柄），而 `CreateFileW` **没有超时参数**，
`timeout_ms` 约束的是它**之后**的重试循环。

**这让修法变难，必须如实说明**：

- 句柄虽然已经是 `FILE_FLAG_OVERLAPPED`，但**独占打开本身是同步的**，
  `CancelIoEx` 针对的是**该句柄上已发出的 I/O**，**不是一个尚未返回的 `CreateFile`**；
- 可行的方向（都未实测，需下一轮判定）：
  1. **改申请顺序**：以**共享打开**（像 `flush` 那样，实测 2 ms 成功）拿到句柄，
     再用**已加界的 `FSCTL_LOCK_VOLUME`** 去锁——**这条最有希望**，
     因为 part 39 刚让那个 IOCTL 可放弃了；
  2. 用 `CancelSynchronousIo`（对发起线程）去取消**尚未完成**的打开；
  3. 在打开前**探测**卷是否已被独占（共享打开 + 查询），
     代价是要接受探测与打开之间的竞态。

**倾向方向 1**：它把一个**无界的同步打开**换成一个**有界的可取消 IOCTL**，
而且两者需要的语义（拿到独占锁）完全相同。**但未实测，故只作记录。**

**T1-03 仍不得标 PASSED。**

#### 4.15.22 **方向 1 已实施：那个无界打开确实被换掉了，但 `FSCTL_LOCK_VOLUME` 仍然不返回**（part 41，本轮）

**已实施并验证语义**（`aeee7aa`）：`lock` 改为
**`FILE_SHARE_READ | FILE_SHARE_WRITE` 打开 + 紧接 `FSCTL_LOCK_VOLUME` 取独占**
（后者 part 39 已加界）。独占性没有丢——**仍由真正持有卷的那个调用取得**，
只是换成一个调用方**可以放弃**的调用。
开发机：干净构建 0/0、**M4/M3/volume 43/44 绿**（唯一失败是既有的 `unit.m4.profile_store`），
**说明 M4 的锁语义未被破坏**。

**读数（请求 `d07221be7c474f7ebfa6e9023929ec2c`，0/1）证明方向 1 解决了一半**：

```
16:25:32.017Z dismount_volume: begin
16:25:32.019Z dismount: re-acquiring the lock
16:25:32.019Z Win32VolumeAdapter::lock: handle acquired     ← 打开不再阻塞 ✅
16:25:32.021Z Win32VolumeAdapter::lock: FSCTL_LOCK_VOLUME
                                                             ← 之后什么都没有
```

**并且运行结束后客机上仍挂着 worker**（`pid 2208`、CPU 0.3 s、已超过 2 分钟）
⇒ **`FSCTL_LOCK_VOLUME` 在我们自己设的 30 s 预算之后仍未返回**。

**必须如实指出这说明什么**：**overlapped + `WaitForSingleObject` 的界在这个调用上没有生效。**
最可能的原因是**卷管理器对 `FSCTL_LOCK_VOLUME` 是按同步方式实现的**，
于是它**在内核里阻塞**，而内核里的阻塞**不会**被 `CancelIoEx` 或等待超时打断
（`CancelIoEx` 只能取消**尚未开始处理**的请求）。

**这是一个关于本机制可行性的实质问题，而不只是一处实现细节**：

> reclaim 需要**独占锁**才能删盘符/拆卸；
> 而独占锁**恰恰要求卷上不能有别人的句柄**——
> 那个句柄属于**被强杀、永远不会关闭**的 worker。

⇒ **"通过取锁来拆卸一个被强杀 worker 留下的卷"可能与取锁的前提相冲突。**
**这个矛盾需要在设计层面回答**，选项至少包括：

1. **不强求独占锁**：reclaim 直接删盘符 + 对残留设备做 `REMOVE_DEVICE`
   （AIM 侧移除设备，不强占卷），**可能根本不需要卷锁**；
2. 先**从驱动侧移除设备**，卷随之消失，**锁的问题自动消失**；
3. 接受"**需要一次重启**"作为残留的清理方式（这是设计 §5.5 想避免的，但可能无法避免）。

**本轮到此为止**（上下文预算见底）：**实现已提交、语义已验证未破坏，
但最后的读数表明这是一个设计层面的问题，故不再用实现细节去试，
而是把它作为需要裁决的选项记录下来。T1-03 仍不得标 PASSED。**

#### 4.15.23 **裁决 A 已实施：residue cleanup 不再挂住了**（part 42，本轮）

**用户裁决 A（从驱动侧移除设备，不取卷锁）**，已实施（`c218294`）：
**reclaim 模式跳过拆卸**——因为它**做不到**（取独占锁的前提被残留句柄否掉）
且**不需要**（随后 `stop_aim` 会**从驱动侧移除设备，卷随之消失**）。
**只在 `reclaim` 模式跳过**：create / recover 拆的是自己拥有的、没有竞争句柄的卷，
在那里跳过会把卷留在挂载状态——**那是另一个 bug**，所以按模式判断而不是一律跳过。
开发机：干净构建 0/0、**M4 10/11**（唯一失败是既有的 `unit.m4.profile_store`）。

**读数（请求 `94a22cb16f444d10a2deaa6dc9211f25`）——这是本轮最重要的进展**：

- **之前**：失败在 `:743`，即 **residue cleanup 的 reclaim 超时**；
- **现在**：失败在 **`:1235`（另一条、更靠后的用例）**，
  而那一条失败在**它自己的 create**（`provision_new_volume`，21001）。

⇒ **`:743` 不再失败**：**那条一直挂住的 cleanup reclaim 通过了**，
**裁决 A 解决了它**。（`:736` 那两条"断言残留被清掉"的检查也随之通过。）

**但残留并没有真正清干净**，这一点必须说清楚。trace 的同一段里出现了：

```
17:40:20.956Z M3AimRuntime::start: ADOPTING the existing device
17:40:22.560Z Win32VolumeAdapter::flush: opening the volume
17:40:22.562Z dismount: re-acquiring the lock
17:40:22.562Z Win32VolumeAdapter::lock: gave up opening
17:40:22.562Z Win32VolumeAdapter::flush: opening the volume
17:40:22.564Z dismount: re-acquiring the lock
17:40:22.564Z Win32VolumeAdapter::lock: gave up opening
```

即**同一条 worker 在反复"flush → 取锁失败"**（注意 `wait for physical drive` 到下一行之间
隔了 **1.6 s**），而 worker 自己的 stderr 也说了：

```
myrd-m5-worker: the worker's resources could not be released
                (op=flush msg=opening volume failed native=0 code=21001)
```

**⇒ 接管的那块残留设备没有被成功移除**，于是它挡住了**后面那条用例**的 create。
**"cleanup 不挂住"与"残留真被清掉"是两件事**，本轮只证明了**前者**。

**下一步（下一轮第一件事）**：查为什么接管后的设备**没有在拆卸里被移除**——
`stop_aim` 走 `M3AimRuntime::stop()` ⇒ `remove_owned_device()`，
而 `remove_owned_device` 大概也会走 `wait_for_physical_drive_retired` 之类的等待，
**需要打点确认它是不是又卡在某个无界调用上**（这将是第六个候选）。
**注意**：`remove_owned_device` 的 `REMOVE_DEVICE` 本身也可能需要驱动回应，
**若同样无界，那就是同一个系统性问题再一处。**

**T1-03 仍不得标 PASSED。**

#### 4.15.24 **定位更新（part 43）：不是第六个无界调用，而是一个"有界但被拒绝"** 

上一节我猜"`REMOVE_DEVICE` 可能也无界"。**读代码后这个猜测不成立，必须更正**：

`M3AimRuntime::remove_owned_device()`（`m3_windows_acceptance.cpp`）是**有界的**：

- `adapter_->remove_device(*owned_device_)` 走 **AIM 的 SRB 传输**，
  而 **part 23 已让那条传输可超时**；
- 其后的"确认设备真的消失"轮询有一个**明确的 `deadline = now + device_timeout_`**，
  超时返回 `cleanup` 域的错误（**不是无限等**）。

⇒ **它不会挂住；它会"失败"**。这与本轮读数一致：
worker 的 stderr 是 `op=flush msg=opening volume failed`，
而 **`flush` 的打开失败**（`CreateFileW` 返回 `INVALID_HANDLE_VALUE`）是**立即失败的**，
不是超时——**这正是"被拒绝"而不是"被挂住"的样子**。

**这个区分对结论很重要**：本增量至今**五个**无界阻塞点都已处理（三修、一绕开、一实测清白），
**而这一步不是第六个**——它是**正确的失败**：
**卷还被残留句柄占着，所以 `flush` 打不开、设备也就拆不掉。**

**因此现在的真实状态是**：**reclaim 不再挂住（裁决 A 生效），
但它在一个"卷被占用"的前提下被正确地拒绝了。**
**残留之所以还在，是因为要移除设备就需要能操作那个卷，
而卷被那个永远不会关闭的句柄占着**——**与 4.15.22 记下的那个设计矛盾是同一个根**。

**下一轮该做的（不再是猜，而是二选一）**：

1. 查 **`REMOVE_DEVICE` 是否真的要求卷可打开**：
   若 **AIM 的 `REMOVE_DEVICE` 可以直接移除设备（不碰卷）**，
   那失败就出在**我们自己先 flush 的那一步**，**修法是"接管路径下不要先 flush"**，
   这与裁决 A"不取卷锁"是同一思路的自然延伸；
2. 若 `REMOVE_DEVICE` 本身也要求卷可用，那么**驱动侧移除也需要卷可操作**，
   则**裁决 A 不足以完成清理**，需要回到用户那里再定（重启 / 驱动改动）。

**判定手段**：给 `remove_owned_device` 的每一步打点
（`remove_device` 之前/之后、每次轮询、以及 `physical_drive_restored_` 那一段），
**一次运行即可看出它是在 `remove_device` 还是在其后的等待里失败**。

**T1-03 仍不得标 PASSED。**

#### 4.15.25 **裁决 A 的延伸已实施**（part 44）

**读数**（请求 `4371187ba7964c8db9933a78ebb79106`）——新加的四条打点**全部出现**，
而且**同一段里完整跑了两遍**：

```
17:54:49.587Z remove_owned_device: remove_device
17:54:49.590Z remove_owned_device: polling the inventory
17:54:49.604Z remove_owned_device: device gone from the inventory   ← kRemoveDevice 确实移除了它
17:54:49.609Z remove_owned_device: rescanning the adapter
17:54:49.616Z remove_owned_device: PhysicalDrive retired
17:54:49.625Z remove_owned_device: remove_device                    ← 又开始第二遍
```

**结论（这一条是明确的进展，且是正面的）**：

- `kRemoveDevice` **确实移除了设备**（`device gone from the inventory`）；
- 随后的 **adapter rescan 也完成**、**PhysicalDrive 也 retired**；
- **四步全部返回成功**，**没有超时、没有被拒绝**。

⇒ **裁决 A 的完整链条（接管 → 不 flush → 不 dismount → 驱动侧移除）在设备层面是通的**：
**"从驱动侧移除设备"这条设计路径被实测证明可行**，
而 section 4.15.24 里那个"`remove_device` 是否需要卷"的疑问
**已被代码与实测一起回答：不需要。**

**但测试仍然失败，且失败点没变**（`:1235`，那条用例自己的 `provision_new_volume` 21001），
worker 的 stderr 也仍是 `op=flush msg=opening volume failed`。
**因此必须区分两件事**：

| 主张 | 状态 |
|---|---|
| reclaim 的拆卸链路（含驱动侧移除）**能走通** | **✅ 本轮实测** |
| **残留被彻底清掉、后续用例不受影响** | **❌ 仍未成立** |

**下一轮要查的就是这中间的那一段**：设备已移除、盘已 retired，
**但机器上似乎仍留着能挡住 `provision_new_volume` 的东西**
（part 12 量到残留有三个事实：进程、盘符、AIM 盘；本轮证明了**AIM 设备**那条已通，
所以嫌疑落在**盘符/卷**那一条）。**下一次读数应在 reclaim 之后立即查
`disks` / `letters` / `Get-Volume`**，而不是只看 trace——
**这比再加打点更直接**。

**T1-03 仍不得标 PASSED。**

#### 4.15.26 **`remove_owned_device` 四步全部走通**（part 45）

**已实施**（`dc56a57`）：**reclaim 也不 flush**。理由与"不 dismount"同源，
而且这一条有**代码依据**：`remove_owned_device` 发的是 `kRemoveDevice` 给 AIM 驱动、
**从不碰卷**（`direct_aim_adapter.cpp`），所以**驱动侧移除本来就不需要卷**——
而拆卸却**在它之前的 flush 上失败**，那个失败被 `record()` 记下后
**挡住了唯一能真正清掉机器的步骤**。
flush 存在的意义是"把**活着的** profile 的写落到盘上"；
reclaim 不是关停一个 profile，而是**拿走一个 worker 已死的设备**，
其内容不为任何人保留。开发机：干净构建 0/0、**M4 10/11**（唯一失败是既有环境用例）。

**但读数（请求 `47c342b6b0e8400cbd99c18b398fbbe5`）显示失败点没有变化**：

```
myrd-test: create failed: op=provision_new_volume msg=provisioning helper failed native=1 code=21001
FAIL ...:1235: created.ok()
myrd-m5-worker: the worker's resources could not be released (op=flush msg=opening volume failed ...)
```

**⇒ 后来的那条用例仍然在它自己的 create 上失败，说明接管的那块残留仍然占着位置。**

**一处必须点明的观察**：这一段 trace 里**没有 `dismount_volume: begin` / `flush_volume: begin`**
（那两条是 `DiskRuntime` 打的），
**而 `flush: opening the volume` 与 `dismount: re-acquiring the lock` 出现了两次**——
说明**这些 flush/dismount 来自另一条路径**（很可能是**那条后来用例自己的清理过程**），
**不是我们这条 reclaim 的拆卸**。
所以**不能把它们当作"reclaim 又失败"的证据**——**这一点本轮只做到"指出不属于同一路径"，
未能进一步定位。**

**下一轮第一件事（已缩小到一件事）**：给 **`remove_owned_device` 内部逐步打点**——
`remove_device()` 之前/之后、确认轮询的每次迭代、以及 `physical_drive_restored_` 那一段。
**一次运行即可回答**：设备到底**有没有**被 `kRemoveDevice` 移除、
还是移除后卡在"等待 PhysicalDrive 消失"的那段有界轮询里（后者会返回超时错误）。
**这是本轮之后唯一还没被排除的可能。**

**T1-03 仍不得标 PASSED。****强杀验收**：强杀**服务** → worker 自行有序拆卸；强杀 **worker** → 下次启动 sweep 回收不留设备/盘符。
本轮验的是"sweep 会终结一个真孤儿"，**不是**"断电后设备与盘符被回收"——
后者要等 `reclaimable_resources` 的消费者接上（设计 §5.5 的资源回收），
而那需要 worker 报告 bundle/volume 身份并被持久化。**这部分没做，也没有宣称。**
