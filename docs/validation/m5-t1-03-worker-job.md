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

#### 4.15.25 **裁决 A 的延伸已实施：reclaim 也不 flush**（part 44）

**已实施**（`dc56a57`）：reclaim 模式**也跳过 flush**，理由与"不 dismount"同源，
而且这一条有**代码依据**：`remove_owned_device` 发的是 `kRemoveDevice` 给 AIM 驱动、
**从不碰卷**（`direct_aim_adapter.cpp`），所以**驱动侧移除本来就不需要卷**——
而拆卸却**在它之前的 flush 上失败**，那个失败被 `record()` 记下后
**挡住了唯一能真正清掉机器的步骤**。
flush 的意义是"把**活着的** profile 的写落到盘上"；
reclaim 不是关停一个 profile，而是**拿走一个 worker 已死的设备**，
其内容不为任何人保留。**只在 reclaim 模式跳过**（create/recover 的 flush 正是"干净停止"的定义）。
开发机：干净构建 0/0、**M4 10/11**（唯一失败是既有环境用例）。

**当时的读数（请求 `47c342b6b0e8400cbd99c18b398fbbe5`）失败点未变**，
我据此写下"残留仍占着位置"——**这个推断在 part 46 被读全 stdout 推翻**，见下节。

#### 4.15.26 **`remove_owned_device` 四步全部走通**（part 45）

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

#### 4.15.27 **读全 stdout 之后：T1-03 的两条用例都通过了；失败的是 checkpoint 用例**（part 46，本轮）

**本轮最重要的一件事，是读全 stdout 而不是只看 stderr 的第一行。** 读数（请求
`4371187ba7964c8db9933a78ebb79106`，套件共 **18** 条，跑了 16 条）：

```
PASS m5_backend_launches_and_authenticates_a_real_worker
PASS m5_backend_launch_places_the_worker_in_the_named_job
PASS m5_backend_bundle_registration_persists_the_identity_it_just_read
PASS m5_backend_a_killed_worker_leaves_no_process_mount_or_device      ← 含"自己 reclaim 掉残留"
PASS m5_backend_a_reclaim_dismantles_the_leftover_a_kill_created       ← **通过**
PASS m5_backend_a_reopened_job_still_holds_its_workers
PASS m5_backend_two_profiles_never_share_a_worker
PASS m5_backend_refuses_a_launch_it_cannot_complete
PASS m5_backend_attach_finds_nothing_and_says_so
PASS m5_backend_adapter_reaches_the_worker_it_launched
PASS m5_backend_creates_a_bundle_and_mounts_it_end_to_end
PASS m5_backend_adapter_registers_the_bundle_it_creates
PASS m5_backend_checkpoint_crosses_the_boundary_and_commits            ← **通过**
PASS m5_backend_checkpoint_boundary_withdrawal_publishes_nothing       ← **通过**
PASS m5_backend_stop_walks_quiesce_flush_commit_remove                 ← **通过**
PASS m5_backend_confirmed_removal_without_a_removal_is_refused
```

**⇒ T1-03 的两条关键用例名称都在 PASS 列表里。**
**`m5_backend_a_killed_worker_leaves_no_process_mount_or_device` 通过**，
意味着它末尾那三条新断言**全部成立**：

```cpp
REQUIRE(reclaimed.ok());              // 残留的 reclaim 成功
REQUIRE(wait_for_letter_release(letter));
REQUIRE(!is_letter_assigned(letter));  // 盘符真的被清掉了
```

**这就是 part 12 那三个残留事实（进程 / 盘符 / AIM 盘）被逐条消灭的证据**——
**T1-03 的验收目标在设备层面达成了**，只是**整个套件还因为别的用例而 0/1**。

**必须更正我在 part 43/44 里写的一句话**：我写过
"后来的那条用例仍然在它自己的 create 上失败，说明接管的那块残留仍然占着位置"。
**这个推断不成立**：失败的那条是
**`m5_backend_checkpoint_crosses_the_boundary_and_commits`（声明于 1263，guest 侧行号）**，
它调 `require_created` 于 **1292**，
而**在它之前跑过的 16 条（含两条 reclaim 用例）全部 PASS**。
**所以"残留挡住下一步"是我的误读**——真正失败的是 **checkpoint 用例**，
**在它之前有两条完整 create+mount+stop 的用例也都通过了**。

**并且失败的行号"1235"解释清楚了**：那是 **`require_created` 这个 helper 内部那行 `REQUIRE`**
（guest 侧 1226–1236），**不是调用点**——TestMain 报的是**断言行**。
**我此前按本地文件行号去反查调用点，方向就错了**（guest 是 stage 过去的副本，行号与本地不同）。

**当前该查的（与 T1-03 无关，但会让整个套件 0/1）**：
`m5_backend_checkpoint_crosses_the_boundary_and_commits` 的 create 失败
（`provision_new_volume` / 21001，PowerShell provisioning helper）。
**它是否是本增量最近几轮改动引起的，尚未验证**——
**下一轮第一件事就是把它与 HEAD 基线对比**（本项目既有的"先排除是不是自己干的"纪律），
**这也是判定 T1-03 能否收尾的关键：若那是既有失败，T1-03 的验收其实已经成立。**

**T1-03 仍不得标 PASSED（但已接近，且剩下的证据问题不在 reclaim 本身）。**

#### 4.15.28 **前后对比：修复前 16 条里只跑到 3 条，现在跑到 16 条**（part 47，本轮）

用**同一条套件的另一次客机读数**做对比（不是推测，两次都取自客机并已落到本机）：

| 运行 | 请求 | 通过的用例数 |
|---|---|---|
| **修复前**（reclaim 尚未能拆卸残留） | `9a1e7af2eaf4469883bfbfb5346108c5` | **3** —— 卡在强杀那条用例之后就停了 |
| **现在** | `4371187ba7964c8db9933a78ebb79106` | **16**（共 18 条） |

修复前那条运行的 stdout **只有 3 行**就结束，说明**它连 `m5_backend_a_killed_worker_...`
都没跑过去**；现在**它和它的 reclaim 兄弟都 PASS**。
**这是本轮拿到的、最有力的"这一段确实做对了"的证据**——
而且它是**同一台客机、同一条命令、同一组用例**下的前后对比。

**仍待归属的一件事**：第 17 条（`m5_backend_checkpoint_crosses_the_boundary_and_commits`）
在自己的 create 上失败（`provision_new_volume` 21001）。
**它是不是既有失败，本轮没有定论**，理由要说清楚：

- **代码层面**：本增量的改动**没有一处触及 provisioning**
  （改动集中在 `DiskRuntime` 的 reclaim 分支、`windows_volume.cpp` 的打开/锁/拆卸、
  以及 `m3_windows_acceptance.cpp` 的接管与 trace）；
- **环境层面**：这条用例**排在 16 条之后**，而这一套件每条都要真的建卷、
  客机存储栈跑到后段本身就可能已经吃紧（本项目已有"跑多了就退化"的记录）；
- **但我没有 HEAD 之前、且完整跑到第 17 条的读数**（修复前的运行都更早就停了），
  **所以"既有失败"这个结论我拿不出直接证据**，不宣称。

**下一轮第一件事（二选一，都有明确做法）**：
1. **把第 17 条单独跑一次**（`-Filter` 到那一条）：
   若它在**干净的客机上、单独跑也失败**，则它是**干净的既有缺陷**，
   与 T1-03 无关，**T1-03 的验收即可据此收尾**；
2. 若它单独跑**通过**，则它在整套里失败是**用例间的资源消耗/顺序问题**，
   要按 §8 的既有清单方式记录，而不是算在 T1-03 头上。

**这一步做完，T1-03 就能给出结论：通过，或精确地指出阻塞在谁身上。**

#### 4.15.29 **单跑那条用例做不到；而且我自己的改动有一个未验证的风险**（part 47 续，本轮）

**第一件事：单跑那条用例在本测试框架下做不到。** 试过 `-TestCase`（无此参数）、
以及 `-Filter 'checkpoint_crosses_the_boundary'`——
**`m5vm` 的 `-Filter` 匹配的是 `tests.json` 里的 `name`，那是"可执行文件"级别的名字**
（例如 `m5.production_execution_backend`），**不是用例名**；
所以过滤器"匹配到 0 条"并抛错。
**要单跑就必须给测试框架加一个用例选择参数**——那是**改动测试基础设施**，
本轮不做。**此路记下来，供以后需要时使用。**

**第二件事（更重要，而且是对我自己不利的一条）**：
part 41 我把 `lock` 的打开方式从 **`share = 0`（独占打开）**
改成 **共享打开 + `FSCTL_LOCK_VOLUME`**。
**这个改动改变了 M4 的加锁时序在真实争用下的行为**，
而**它在开发机上只被单元测试覆盖过**（M4 10/11），
**真实卷争用那条路径没有被验证**。

**因此第 17 条用例的失败，有可能是我的改动引起的**，具体机制是：
- 旧行为：独占打开**本身就是排他的**，拿到句柄即已排除他人；
- 新行为：共享打开**先**拿到句柄，**排他性推迟到 `FSCTL_LOCK_VOLUME`**——
- 于是**在两步之间出现了一个窗口**，**别的句柄可以在那一刻进来**。
  跑了几十条、每条都建卷拆卷之后，**这个窗口被撞上的概率并不小**，
  而表现恰好就是"后来某条用例的 provisioning 打不开/建不了卷"。

**必须做的验证（下一轮第一件事）**：**`integration.m4.multi_instance`** ——
它正是"真实卷争用"的那条集成测试，
**而且它只能在客机上跑**（要真实设备），所以**开发机无法替代**。
**若它通过，我这一改动的风险大幅下降**；**若它失败，则第 17 条的失败很可能就是我造成的，
必须回退或改正加锁方式**。

**这条按项目纪律写下来：在把第 17 条归因于"既有失败"之前，先排除是不是我干的。**
**T1-03 仍不得标 PASSED。**

#### 4.15.30 **我自己那条嫌疑被排除了：回退后仍然是 16/18、同一个失败点**（part 48，本轮）

**两件事，都是本轮做的，且第二件是决定性的。**

**（1）`integration.m4.multi_instance` 不是我以为的那种验证。**
它在客机上跑出 **5/5**（含 `m4_multi_instance_rejects_second_runtime_for_same_bundle_lock`），
**但读它的实现后发现：它的锁是注入的 fake**——
`ops.acquire_bundle_lock` 是一个 `std::function`，读写一个内存里的 `bool locked`，
**它从头到尾不碰真实卷**。
⇒ **它的通过并不能验证我改的那个真实加锁行为**。
**我上一轮把它当作"能验证"的测试，这个判断是错的**，必须更正。

**（2）于是直接做回退实验，结论是决定性的**：
把 part 41 的 **共享打开 + `FSCTL_LOCK_VOLUME`** 回退成原来的 **`share = 0` 独占打开**
（保留 `FILE_FLAG_OVERLAPPED`，它不影响锁语义、且是拆卸可取消的前提），
再跑同一条套件：

| 版本 | 通过的用例数 | 失败点 |
|---|---|---|
| 改动版（共享打开） | **16** | 第 17 条 create（21001） |
| **回退版（独占打开）** | **16** | **第 17 条 create（21001），一模一样** |

⇒ **第 17 条的失败与我的加锁改动无关**：
**独占打开与共享打开两种语义下，它都以完全相同的方式失败**。
**我自己那条嫌疑被排除了**——这正是项目纪律"先排除是不是自己干的"要得到的结果，
而且**这次是靠一次真正的对照实验，不是靠推理**。

**回退保留的部分也是有意为之**：只回退**没有验证**的那一点（share mode），
保留**已经被证明**的那些（`FILE_FLAG_OVERLAPPED`、有界的 `FSCTL_DISMOUNT_VOLUME`
与 `FSCTL_LOCK_VOLUME`）。开发机：干净构建 0/0、**M4 10/11**（唯一失败是既有环境用例）。

**因此第 17 条现在可以合理地记为"与 T1-03 无关的独立失败"**（但仍**未**证明是既有缺陷；
要证明需要在**没有本增量改动**的基线上跑，而本增量改动已经全部提交，
那需要另开一个干净 worktree——**成本较高，故记为待办而不是本轮做**）。

**T1-03 的验收本身已经成立**：它的两条用例在客机上 PASS，
而且这一点在**两个不同的加锁语义下都一样**（本轮与上一轮各测一次）。

#### 4.15.31 **隔离实验：那条用例单跑 1/1 通过 ⇒ 它是"用例间资源交互"，不是缺陷**（part 49，本轮）

**做法（值得记下来，因为这是本项目第一次这么做）**：既然测试框架**不支持选单个用例**，
就用**源码级隔离**：把 17 条其它用例各包进 `#if 0 ... #endif`，**只留目标那一条**，
只重编那一个测试目标，然后照常 stage/run。**改动只在工作区、跑完立即用备份还原**
（还原后与 HEAD 无 diff、花括号 171/171、用例数 18、无编码损坏——都已核对）。

**读数**（请求 `127ca622d7124964b01789733c309521`，客机干净）：

```
1 of 1 passed        ← m5_backend_checkpoint_crosses_the_boundary_and_commits 单独跑，通过
```

**⇒ 结论明确，而且把归因彻底换了方向**：

| 观察 | 含义 |
|---|---|
| 整条套件里：**16/18**，第 17 条在 create 失败（21001） | 失败**可复现**（两次干净客机都一样） |
| **单独跑：1/1 通过** | **那条用例本身没有缺陷** |
| 我的加锁改动：**回退前后完全相同**（part 48） | **也不是我改的加锁语义** |

**三条合起来只有一个解释**：**这是一个用例间的资源交互**——
跑到第 17 条时，前面 16 条（每条都真的建卷/拆卷、其中两条还**故意强杀 worker 并 reclaim**）
在客机上**累积下了某个状态**，使 `provision_new_volume` 的 PowerShell helper 打不开/建不了卷。
**这一点与 part 12、part 15 记录过的"残留会累积"是同一类现象**，只是这次发生在**同一套件内部**。

**因此对 T1-03 的定位**：

- **T1-03 的验收不依赖第 17 条**：它的两条用例**都在 PASS 列表里**，
  而且**在隔离条件下目标用例也通过**（说明套件里的失败不是"reclaim 弄坏了后面的用例"的必然结果，
  但也**不能说完全无关**——见下）；
- **但要诚实**：**我无法排除"reclaim 留下的某个状态"是那个累积的一部分**。
  我能排除的是**加锁语义**（对照实验）与**该用例自身**（隔离实验），
  **排除不了"reclaim 之后客机少了一点可用空间/句柄/盘符"**。

**下一步（方向明确，且是收尾 T1-03 与清理 §8 都要的）**：
**在第 16 条跑完、第 17 条开始之前取一次客机现场**
（`disks` / `letters` / `Get-Volume` / `myrd-m5-worker` 数），
**与"第 17 条单跑前"的现场对比**——**一次读数就能看出到底多/少了什么**。
**这比继续加打点更直接**，而且正是 part 46 记录过、当时没做的那件事。

**T1-03 仍不得标 PASSED**（但它的两条用例已被两种方式证明通过；
剩下的问题属于**套件级资源卫生**，与 §8 的既有清单同类）。

#### 4.15.32 **失败模式是"变动"的：换了地方、也换了原因**（part 50，本轮）

**本轮又跑了三次，结果如下（全部在干净客机上）**：

| 请求 | 通过的用例数 | 失败原因 |
|---|---|---|
| `80471885`（part 48，回退版） | 16 | `create failed: op=provision_new_volume ... 21001` |
| `4371187b`（part 46） | 16 | 同上 |
| **`d4fe32f78c624d239cacaf8782f5049a`（本轮）** | **12** | **`create failed: op=checkpoint msg=generation-zero checkpoint was not committed`** |

**⇒ 这是本轮最重要的信息：失败不是稳定的那一个点，而是"随运行变化"的**——
**用例数 16 → 16 → 12**，**失败原因从 provisioning 变成 generation-zero checkpoint**。
**而我自己的加锁改动、以及那条用例本身，都已分别被对照实验与隔离实验排除**（parts 48、49）。

**三条合起来的结论**：**这是套件级的资源/时序不稳定**，
**不是某一个确定的缺陷**——而**"generation-zero checkpoint was not committed"**
是在**建卷过程中**就失败了，说明**客机在那个时点已经不能正常完成建卷**。

**因此对 T1-03 的定位保持为**：
- **它的两条用例在多次运行里都在 PASS 列表里**（本节这次 12 条里也包含它们，
  即 `m5_backend_a_killed_worker_leaves_no_process_mount_or_device` 与
  `m5_backend_a_reclaim_dismantles_the_leftover_a_kill_created` 都 PASS）；
- **它不能把整个套件拖绿**，但**套件不绿的原因不是它**（隔离实验 + 对照实验）。

**§8 待办（本轮新增，且必须写清楚）**：
**"整套 `m5.production_execution_backend` 在多条用例连续跑时会不稳定（12–16/18，
失败点与原因随运行变化）"**——这是**独立于 T1-03 的套件级问题**，
按 §8 的既有方式记录，而**不是**算在 T1-03 上。

#### 4.15.33 **一条必须写进 handoff 的操作陷阱（本轮踩了，浪费一整轮）**

**隔离实验的正确做法**（part 49 用的）是"源码级隔离"，
但**本轮重复它时踩了一个坑，必须记下来**：

> **`Move-Item` 还原备份会保留备份文件的旧时间戳**，
> 于是 **CMake 认为源文件没变、不重新编译**，
> **我看到的"1/1 passed"其实是上一次"隔离版"的旧二进制的读数**，
> 而不是恢复后完整套件的读数。

**我第一次因此得出了错误的"套件 1/1 通过"**，直到**注意到运行只花 9–10 秒**
（完整套件要 50–75 秒）才起疑，进而核对才发现二进制没重编。

**规则**：
- **还原文件后必须 `touch` 它**（或直接 `--clean-first`），否则构建系统会静默跳过；
- **判断"这次跑的是不是完整套件"最省事的信号是耗时**：
  `m5.production_execution_backend` 完整跑约 **50–75 秒**，
  若只有 **10 秒左右**，**几乎肯定是跑到了被裁剪过的二进制**。

**另外一条（同轮发现，同样重要）**：
**在开发机上直接运行这个测试二进制会真的创建 worker 与卷**——
本轮我在核对二进制内容时误跑了一次，
**立刻杀掉了残留 worker 并确认归零**。
**这违反了 §4 的硬门**，记在这里作为警示：
**核对二进制内容不要用"跑一遍"的方式**，用**耗时/输出条数**等间接信号判断。

#### 4.15.34 **`-Repeat 20` 的定量读数（本轮完成）——0/20，但原因分层**（part 51）

按 §9 的修复类质量门（"修复类结论至少 `-Repeat 20`"）跑了 **20 次**，
并把**每一次**的 stdout/stderr 都取回来逐条看（请求 `dd250ebdffc24308ac8b862daaaec4be`，耗时约 39 分钟）：

| 情形 | 次数 | 走到哪 | 失败点 |
|---|---|---|---|
| **A. 2 条 PASS 后就停** | **18** | 前两条用例之后 | **`:544` `result.ok()`**（fixture 层，**在 reclaim 用例之前**） |
| **B. 走到 16 条 PASS** | **2** | 第 16 条之后 | `:1235`（`require_created`），原因 `provision_new_volume` 21001 |

**由此得到的结论（比"0/20"这个数字本身重要得多）**：

1. **绝大多数迭代（18/20）根本没跑到 T1-03 的用例**——
   它们在 **`:544`** 就失败了，而那是**fixture 层的建卷**，
   **位置在前两条用例之后、reclaim 用例（`:595` / `:750`）之前**。
   ⇒ **关于 T1-03，那 18 次是"无信息"，不是"失败"**；
2. **只有 2 次真正跑到了 T1-03 的用例，而那 2 次里两条都 PASS**；
3. **`-Repeat 20` 因此对本套件不成立**：不是"跑了 20 次都没过"，
   而是**这台客机在第 2 条与第 3 条用例之间就已经不能建卷了**——
   **这反过来把问题定到了"建卷能力随运行次数退化"上**，
   而它**发生在 reclaim 用例之前**，**所以与 T1-03 无关**。

**这也是对"`-Repeat` 用于崩溃回收类行为"这条质量门的一个重要限定**：
**当迭代之间的环境本身在退化时，`-Repeat N` 的读数会被"后面的迭代根本没跑到"污染**，
必须**逐次看走到了哪**，而不是只看 `x of N passed`。
**本项目的 `-Repeat` 用法应据此加一句**（已记入 §8 第 6 条）。

**T1-03 的验收状态不变**：两条用例在**能跑到它们的那 2 次里都 PASS**，
另有隔离实验（单跑 1/1）与对照实验（回退加锁后失败方式不变）支持。

**下一步（与 T1-03 无关，但挡住整个套件）**：
**`:544` 的 fixture 建卷为什么在第 2 条用例之后就失败**——
这是**本套件最大的可用性问题**，**建议在 T1-05/T1-06 之前先解决**（已记入 §8 第 6 条）。

#### 4.15.35 **`-Repeat 3` 抓到了 T1-03 自己的间歇性失败：reclaim 偶尔超时**（part 52，本轮）

**先修掉一个"没有诊断"的断言**（`:544` 那条 `REQUIRE(result.ok())` 之前**不打印任何错误**，
而别处的失败都打印）。补上之后，`-Repeat 3`（请求 `59e8aca35b614569b56d2147406d07d5`）读数如下：

| 迭代 | 通过条数 | 失败点与原因 |
|---|---|---|
| 1 | **16** | `:1248` `provision_new_volume` 21001（两条 T1-03 用例**都 PASS**） |
| 2 | **16** | 同上（两条 T1-03 用例**都 PASS**） |
| 3 | **3** | **`:756 reclaimed.ok()` —— residue cleanup 的 reclaim，`pipe transfer exceeded its bounded wait`（4107）** |

**这是本轮最重要的发现，而且它落在 T1-03 自己身上**：

- 迭代 1、2 走到 16 条，**T1-03 的两条用例都通过**；
- **迭代 3 只走到 3 条，卡在 `m5_backend_a_killed_worker_leaves_no_process_mount_or_device`
  结尾那段 cleanup 的 reclaim 上**——即 **T1-03 自己的路径**，
  而且**失败原因是超时（4107），不是被我修掉的那些无界调用之一**（那些都已加界）。

⇒ **T1-03 的 reclaim 是间歇性的（约 1/3 在这一轮里）**，
**所以 §9 的"修复类结论至少 `-Repeat 20`"这条质量门对 T1-03 是适用的**，
**而我此前用它换来的两次"通过"不足以作为收尾证据**。
**这一条必须如实写下来：T1-03 不能标 PASSED。**

**下一步（下一轮第一件事，范围明确）**：
**在 reclaim 的拆卸路径上再抓一次 4107**——
迭代 3 只走到 3 条，说明它**很早就卡住**，
而且我上一轮为 reclaim 加的"跳过 flush / 跳过 dismount"两道闸
**如果 `mode_` 判定没有生效，reclaim 仍会去取卷锁**，那正是会卡的地方。
**因此先确认那两道闸在真实路径上是否真的生效**（trace 里应出现"没有 flush_volume/flush 行"），
再决定是闸没生效、还是另有第三个阻塞点。

#### 4.15.36 **找到了 T1-03 间歇性失败的根因：`SetupDiGetClassDevsW` 无界枚举**（part 53，本轮）

**读法（很便宜，且立刻定位）**：上一次 `-Repeat 3` 里**迭代 3 只走到 3 条**，
说明它**卡在强杀那条用例的 cleanup reclaim**；
而该迭代**确实跑到过 reclaim**，所以客机上的 trace 就是现场。
把 `C:\tmp\m5-reclaim-trace.txt` 取回来的**最后一段**是：

```
19:05:04.850Z worker: launch spec read, connecting
19:05:04.852Z worker: pipe connected, authenticating
19:05:04.852Z worker: handshaken, opening the bundle
19:05:04.854Z worker: runtime open, serving
19:05:04.856Z acquire_bundle_lock: begin → ok
19:05:04.856Z recover_complete_image: begin → 19:05:04.952Z ok
19:05:04.952Z start_aim: begin
19:05:04.954Z M3AimRuntime::start: provider open
19:05:05.050Z M3AimRuntime::start: provider start
19:05:05.052Z M3AimRuntime::start: discover adapter
19:05:05.057Z M3AimRuntime::start: physical baseline      ← 此后没有任何输出
```

⇒ **卡在 `snapshot_physical_drive_numbers()`，也就是 `physical baseline` 那一步**，
**在 `look for an existing device` 之前**（我此前只给后面那步加了点，所以看不到）。

**它为什么无界**：`snapshot_physical_drive_numbers` → `enumerate_disk_candidates`
（`physical_drive_locator.cpp:144`）第一句就是

```cpp
const HDEVINFO raw_device_info = SetupDiGetClassDevsW(
    &kDiskGuid, nullptr, nullptr,
    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
```

**`SetupDiGetClassDevsW` 是同步的 PnP 设备枚举，没有超时参数**——
**这是本增量的第六个无界阻塞点**，而且它**不在 AIM 或卷的路径上，而在"数一下机器上有哪些盘"这件小事上**。

**为什么这解释了一切（包括间歇性与套件不稳定）**：
`start_aim` 每次都要先取 baseline ⇒ **每一次 worker 启动都会枚举一遍全套磁盘接口**；
而这一套件**每条用例都真的建卷拆卷**，
**设备接口树里越积越多**，枚举所需的 PnP 工作随运行次数增长，
**于是从"很快"变成"卡住"**——这与观测完全一致：
**迭代 1、2 走到 16 条，迭代 3 停在 3 条；以及 18/20 次连第 3 条用例都过不去**。

**当前六个无界阻塞点的完整清单**：

| # | 位置 | 状态 |
|---|---|---|
| 1 | SRB 传输 `DeviceIoControl` | ✅ part 23 已修（overlapped + `CancelIoEx`） |
| 2 | `CM_Reenumerate_DevNode` | 已被 A' 绕开 |
| 3 | `Win32VolumeAdapter::flush` | 实测 2 ms，清白 |
| 4 | `FSCTL_DISMOUNT_VOLUME` | ✅ part 36 已修 |
| 5 | `FSCTL_LOCK_VOLUME` | ✅ part 39 已修（有界 + 返回 `busy`） |
| **6** | **`SetupDiGetClassDevsW`（磁盘接口枚举）** | **本轮定位，未修** |

**修法方向（下一轮第一件事，且要请示取舍）**：
这是**第六个同形状**的问题，而**前两个已证明"overlapped + 取消"可行**——
但 **`SetupDiGetClassDevsW` 是 CfgMgr32/SetupAPI 的调用，没有 overlapped 形式**
（与 part 23/36/39 不同），所以要么：
1. **把整个枚举丢进可放弃的线程并带上限**（较脏，但可行）；
2. **换一个可加界的枚举方式**（例如直接用 `CM_Get_Device_Interface_List` 或
   只枚举**已知的** AIM 盘号区间，而不是全机器枚举）；
3. **干脆不要每次启动都取 baseline**：baseline 只用于"找出**新出现**的盘"，
   而**接管（A'）路径与新建路径都不该依赖全机器枚举**——
   这一条最符合本增量已经确立的方向（**不要做不需要做的事**）。

**倾向 3**，因为它同时减少**每次 worker 启动**的 PnP 负担，
而这正是间歇性随运行次数恶化的原因；**但它属于设计取舍，故记下来请示，不自行发明。**

**T1-03 仍不得标 PASSED。**

#### 4.15.37 **裁决：不取全机器 baseline（选项 A）——实施要点与依赖**（part 54，本轮）

**用户裁决 A**：**不要每次启动都取全机器 baseline**。
这与本增量已经确立两次的方向一致（**不做不需要做的事**：
reclaim 不做 flush、不做 dismount）。**本轮记录下来并说清它牵动什么**，因为这不是一行改动：

**baseline 目前的作用**：`snapshot_physical_drive_numbers()` 取"启动时机器上有哪些盘号"，
`wait_for_created_physical_drive(device, baseline, size, timeout)` 靠**与它做差集**来认出
**"刚被创建的那块盘"**（`match_created_disk` 会**跳过标记为 `existed_before` 的候选**）。

**因此去掉 baseline 就必须给出替代判据**，而 `find_created_disk` 的匹配本来就已经用了
**更强的身份**：`DeviceId`（SCSI 目标地址）+ **期望长度**。
也就是说 —— **这两项已经足以定位那块盘**，baseline 只是**额外的**一层过滤；
而 part 29 为"接管"路径新增的 `wait_for_present_physical_drive`
**正是同一个匹配、传空 baseline**（`std::span<const std::uint32_t>{}`），
**并且已在客机上实测可用**（parts 32/34 的 trace 走的就是它）。

⇒ **实施方向已经清楚且风险可控**：
**让"新建"路径也用同一个"按身份匹配"的等待**（即 `wait_for_present_physical_drive`），
**从而不再需要 baseline**；`snapshot_physical_drive_numbers` 与其调用点一并去掉。
**保留 `wait_for_physical_drive_retired`（拆卸侧）不变**，它本来就不看 baseline。

**下一步（下一轮第一件事）**：
1. 在 `M3AimRuntime::start` 里**去掉 baseline 的获取**，
   把新建路径的等待换成按身份匹配的那一个；
2. **确认 `match_created_disk` 在"不标记 existed_before"时仍能正确拒绝异己盘**
   （它已有 `expected_size_bytes` 这一重身份；**需要读一遍它的实现确认**，
   因为这正是 part 28 我为接管路径写过的同一条安全论证）；
3. 开发机干净构建 + M3/driver 套件；
4. 客机 `-Repeat 3` → 若不再卡在 `physical baseline`，再上 `-Repeat 20` 取 T1-03 的收尾读数。

**预期收益（应写进读数）**：若根因判断正确，
**`-Repeat 20` 应从"18/20 连第 3 条都过不去"变成大部分迭代能跑到 T1-03 的用例**——
**这同时是对"根因是它"这个判断本身的检验**：
**若换掉 baseline 后套件稳定了，则根因确认；若仍不稳定，则另有原因。**

**T1-03 仍不得标 PASSED（且本轮未改代码，只记录裁决与实施路径）。**

#### 4.15.38 **预测被证伪：去掉 baseline 之后，间歇失败一模一样**（part 55，本轮）

**已实施裁决 A**（`9b539ad`）：`M3AimRuntime::start` **不再调用
`snapshot_physical_drive_numbers()`**，`physical_baseline_` 成员一并删除；
两条等待路径**合并为按身份匹配的 `wait_for_present_physical_drive`**
（它们在语义上**只差"是否跳过已存在的候选"**，而那正是 baseline 提供的、且**不是身份判据**
——`match_created_disk` 比的是 **SCSI path/target/LUN + 期望长度**）。
开发机：干净构建 0/0、**43/44 绿**（唯一失败是既有环境用例）。

**然后按上一节写下的预测去检验**（请求 `96cc6ab1e12b40abb3dfd5864893f562`，`-Repeat 3`）：

| 迭代 | 通过条数 | 失败点 | 与"未改动时"对比 |
|---|---|---|---|
| 1 | **16** | `:1248` provisioning | **相同** |
| 2 | **16** | `:1248` provisioning | **相同** |
| 3 | **3** | **`:756 reclaimed.ok()`，4107** | **相同** |

⇒ **预测被证伪。** 我在上一节明确写下：
"**若换掉 baseline 后套件稳定了，则根因确认；若仍不稳定，则另有原因**"——
**现在就是"另有原因"**：去掉全机器枚举**并没有**消除那个间歇超时，
**迭代 3 仍然只走到 3 条，失败点与原因逐字相同**。

**因此必须更正我上一节的结论**：`SetupDiGetClassDevsW` 的定位**仍然成立**
（它确实是无界的、也确实每次启动都跑），
**但它不是那个 4107 的原因**。
当时我把"trace 停在 `physical baseline`"当成了因果——**那只是一个相关性**：
trace 文件**跨迭代累积**，所以我读到的那一段**可能属于另一次迭代**，
而我**没有把迭代边界与 trace 时间戳对齐**就下了结论。**这是本轮的教训**。

**去掉 baseline 这件事本身仍然保留**，理由与预测无关：
**它移除了一个真实的、每次启动都会跑的无界 PnP 枚举**（第六个），
而且**不减损任何身份判据**（有代码依据）。**但它不是那个 4107 的答案。**

**下一步（重新定位，且要针对"4107 到底发生在哪一步"）**：
**必须按迭代分离 trace**（每次运行前删文件、运行后立刻取，
并用**该迭代的起止时间**裁剪），再定位那个间歇超时；
**在此之前不再对 4107 的成因下任何结论。**

**T1-03 仍不得标 PASSED。**

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

**T1-03 仍不得标 PASSED。**

### 4.16 强杀验收的边界（写清楚，避免误读）

**强杀验收**：强杀**服务** → worker 自行有序拆卸；强杀 **worker** → 下次启动 sweep 回收不留设备/盘符。
本轮验的是"sweep 会终结一个真孤儿"，**不是**"断电后设备与盘符被回收"——
后者要等 `reclaimable_resources` 的消费者接上（设计 §5.5 的资源回收），
而那需要 worker 报告 bundle/volume 身份并被持久化。**这部分没做，也没有宣称。**

#### 4.15.39 **按迭代分离 trace 抓住了现场：卡在 `wait_for_present_physical_drive`，而超时打不响**（part 56，本轮）

**这一次按上一节定下的方法做**：先删掉客机上**所有**陈旧 trace，再跑，
**并在运行中**（而不是跑完）用 `scp` 取一次。结果**当场抓到一个正在挂住的 worker**：

```
19:32:01.434Z M3AimRuntime::start: look for an existing device
19:32:01.436Z M3AimRuntime::start: ADOPTING the existing device
19:32:01.436Z M3AimRuntime::start: query device
19:32:01.438Z M3AimRuntime::start: wait for physical drive     ← 就停在这里
```

**同时读到该 worker 的进程状态**：`pid 580`、**CPU 0.23 s**、启动于 `03:32:01`，
到 `03:33:25` **已 84 秒无进展**——**又是"阻塞在系统调用"的形态**。

**为什么它有 30 秒预算却不生效**（本轮关键，有代码依据）：
`wait_for_created_physical_drive` 的循环形如

```cpp
while (std::chrono::steady_clock::now() < deadline) {
    auto cand = enumerate_disk_candidates();   // ← 这里没有任何界
```

**deadline 只在进入循环体之前检查一次**，
而 **`enumerate_disk_candidates()` 内部全是同步、无界的 PnP 与 IOCTL 调用**
（`SetupDiGetClassDevsW` / `SetupDiEnumDeviceInterfaces` /
`DeviceIoControl(IOCTL_SCSI_GET_ADDRESS)` / `IOCTL_STORAGE_GET_DEVICE_NUMBER`）。
⇒ **只要其中任何一次不返回，那个 30 秒预算就永远不会被重新检查**——
**预算管的是"循环转几圈"，管不住"单圈要多久"。**

**这把前两轮的矛盾解释清楚了**：

- part 53 定位到 `SetupDiGetClassDevsW` **且它确实无界**——但那不是全部；
- part 55 我删掉了**启动 baseline 的那一次**枚举，**预测失败是对的**：
  **同一个枚举还在等待循环里**，而等待循环**每次 worker 启动都会跑**；
- 所以 4107 一直在，**只是位置始终在同一个函数里**；
  我第一次读到的 trace 是**跨迭代累积**的，第二次按迭代分离才看清。

**根因陈述（比之前精确）**：**`enumerate_disk_candidates` 本身无界，
而它在等待循环内被反复调用；预算是"循环级"的，管不住"单次调用级"的阻塞。**
**两个缺陷叠加**才产生那个间歇的 4107：
机器上磁盘接口越多/越坏，单次枚举越慢，最终某一次不返回。

**修法方向（与裁决 A 同源，但更深一层）**：
**不要靠"枚举机器上的盘"来找我们自己的盘**——
**那块盘是 AIM 驱动按我们的设备创建的，驱动知道它对应哪个 `PhysicalDrive`。**
`DiskInfo` 已有 `size_bytes` / `sector_size` / `native_object_path`；
**若能把"这个设备对应的盘号"从驱动问出来**（`QUERY_DEVICE` 扩展，
或用 `created` 的 SCSI 地址直接拼出设备路径去打开），
**等待循环就完全不需要 `enumerate_disk_candidates`**，
**这个无界调用在启动路径上就消失了**——与"不取 baseline"同一方向，
只是**这次要连等待循环里的那次也去掉**。

**这是设计取舍（要不要扩展 AIM 的查询能力），故记下来请示，不自行发明。**
**另有一件应同时做的**：**给 `enumerate_disk_candidates` 的每次调用本身加界**
（它既无 overlapped 形式也不可放弃），
**否则任何一处遗留的枚举仍能把整个 worker 挂死。**

**T1-03 仍不得标 PASSED。**

#### 4.15.40 **推进修法前的一个阻塞事实：驱动报不出盘号**（part 57，本轮）

上一节把修法方向定为"**不问机器、直接问驱动那块盘是几号**"。
**本轮去核对它是否可行，结论是：`QUERY_DEVICE` 报不出盘号。**

- `query_device` 发 `kQueryDevice`，返回体按 **`DeviceConfiguration`** 解析
  （`direct_aim_adapter.cpp`，容量 `sizeof(DeviceConfiguration) + 260`）；
- `DeviceConfiguration` 的字段是
  `DeviceNumber / DiskSize / BytesPerSector / ImageOffset / Flags / FileNameLength / 文件名`
  （见 `create_proxy_disk` 对同一结构体的构造，`direct_aim_adapter.cpp:85-92`）；
- **`DeviceNumber` 是 AIM 自己的设备号**（创建时用的是 `kAutoDeviceNumber`），
  **不是 Windows 的 `PhysicalDrive` 号**——
  后者是 **Windows 枚举该 SCSI 设备时分配的**，
  而 AIM 是无盘设备（SHM 代理），**它并不知道 Windows 给它编了几号**。

⇒ **"直接问驱动"这条路在当前 `QUERY_DEVICE` 能力下走不通**，
除非**扩展协议/驱动**（那是改 M0/M3 的接口，属于要请示的范围）。

**因此"去掉枚举"只剩两种落法，且都需先判定**：

1. **用 `created` 的 SCSI 地址自己拼出设备接口路径**（`\\?\scsi#disk&...`）直接打开那块盘——
   **不枚举**。可行性**未验证**：要确认 Windows 对 AIM 代理盘生成的接口路径格式，
   以及实例哈希能否由 `path/target/lun` 唯一决定；
2. **保留枚举，但把每次枚举放进可放弃的执行体并加强制上限**（见下）。

**而无论选哪一种，有一件事本身就应当做**（与本增量其余五个修法同型）：

> **`enumerate_disk_candidates()` 必须有一个"最坏情况上限"。**
> 它现在**同步、无 overlapped 形式、也不可取消**；
> **任何一处遗留的枚举都能把整个 worker 挂死**——这正是 4107 的机制
> （预算在循环外，管不住单次调用）。

**具体做法（本轮判定为可自行实施的范围内，下一轮做）**：
把 `enumerate_disk_candidates()` 的调用**放进可放弃的执行体**
（例如 `std::async` + `wait_for(超时)`，超时即返回超时错误并**不再等待**），
**于是"单次枚举"有了上限，循环级预算重新变得有意义**。
**与 part 23/36/39 同型**（都是"让一次可能不返回的调用变得可以放弃"），
只是这次没有 overlapped 可用，所以**用执行体而不是取消 I/O**。

**风险写明（作为选这条路的理由）**：被放弃的执行体**仍可能在内核里**，
但它是**只读枚举**（列接口、查地址、查长度），**不改变设备状态**——
**放弃它比放弃一个正在拆卸的调用安全得多。**

**本轮未改代码**（只完成可行性判定与方法确定）。**T1-03 仍不得标 PASSED。**

#### 4.15.41 **界生效了，而且把"挂死"变成了"有诊断的失败"**（part 58，本轮）

**已实施**：`enumerate_disk_candidates_within(10 s)`，
把枚举放到**detached 线程**里等上限，超时即放弃并返回
`enumerate_disk_candidates ... did not finish within its bound`（`WAIT_TIMEOUT` = 258）。

**中途一个重要更正（写进了代码注释）**：第一版用 `std::async` + `wait_for`，
**`-Repeat 5` 显示与改动前完全一样的 4107 超时**——
原因是 **`std::async` 的 future 在析构时会等待任务结束**，
所以"等超时然后让它出作用域"**仍然等待那个要放弃的东西**：
**界看起来生效了，实际什么也没改变**。
**换成 detached 线程才是真正放弃。**
（另外我第一版修正里还犯了个 UB：**移动一个仍被锁住的 mutex**；
最终改为**堆上的共享状态**，简单且正确。）

**读数（请求 `edf63eb825504d879156a9101785c4fa`，`-Repeat 5`）——这是本增量的一次实质性转折**：

| 迭代 | 通过条数 | 失败原因 |
|---|---|---|
| 1 | **16** | `:1248` provisioning（两条 T1-03 用例**都 PASS**） |
| 2 | **16** | 同上 |
| 3 | **3** | **`op=enumerate_disk_candidates ... did not finish within its bound`（258）** |
| 4 | **3** | 同上 |
| 5 | **3** | 同上 |

**对比改动前**（同为 5 次、同为干净客机）：

| | 迭代 3–5 的表现 |
|---|---|
| **改动前** | worker **挂死 80+ 秒**，reclaim 以**不透明的 4107** 失败 |
| **改动后** | **10 秒内失败**，错误**直接指名列举**（`enumerate_disk_candidates`，258） |

⇒ **界是真生效的，而且它把"卡死"变成了"可诊断的失败"**——
**这正是 quality gate 意义上的一次实质改进**：
失败不再需要靠 trace 反推，**错误自己说出了原因**。
（同时说明 **4107 只是"worker 没回包"的表象**，真正的阻塞一直是这个枚举。）

**但这不等于 T1-03 通过**：迭代 3–5 **仍然失败**，
只是**失败原因变清楚了**：**这台客机上"枚举磁盘接口"确实会卡住**，
而 reclaim 路径依赖它（`wait_for_present_physical_drive` 仍要枚举）。

**因此下一步就是上一节列出的那件事的"真正落法"**（而不是再加界）：

> **不能靠枚举机器上的盘来找我们自己的盘。**
> 界只是让失败可见；**要让 reclaim 成功，必须让它在不枚举的前提下找到那块盘。**

上一节已查明**驱动报不出盘号**（`DeviceConfiguration.DeviceNumber` 是 AIM 自己的设备号，
不是 Windows 的 `PhysicalDrive` 号），所以候选仍是：
**用 `created` 的 SCSI 地址自己拼出设备接口路径直接打开**（不枚举），
**或扩展 AIM 的查询能力**。
**两者都需要设计判断，故记下来请示。**

**T1-03 仍不得标 PASSED（但"为什么失败"现在是一个明确的、可修的问题了）。**

#### 4.15.42 **这不是 T1-03 的局部问题：枚举阻塞影响整个项目**（part 59，本轮）

**本轮先量了一下影响面**（`git grep` 该枚举的全部调用点），结论值得单独记：

| 调用点 | 属于 |
|---|---|
| `snapshot_physical_drive_numbers` | **`src/m1/main.cpp`**、**`src/poc/main.cpp`** |
| `snapshot_physical_drive_numbers` × 5 处 | **`src/m2/m2_windows_acceptance.cpp`**（:888/:1036/:1378/:2034/:2068） |
| `enumerate_disk_candidates` | 本文件内的等待循环（**已加界**，part 58）、`snapshot_*`、`enumerate_present_*` |

⇒ **"枚举磁盘接口会卡住"不是 M3 或 reclaim 的问题**：
**M2 的 Windows 验收、M1 与 POC 的驱动路径都用同一个枚举**。
**因此它应当被当作 §8 层面的、项目级的问题来处理，而不是继续算在 T1-03 里。**

**而且修法是统一的，只有一个**：

> **把界下沉到 `enumerate_disk_candidates()` 本身**，让**任何**调用者都不可能被它挂住。

理由（本轮确定，不再是猜测）：

- part 58 已经**实测证明这种加界方式有效**（detached 线程 + 堆上共享状态）；
- 它**不改变语义**：超时返回 `WAIT_TIMEOUT`，调用方要么报错、要么重试，
  **都是它们本来就要处理的情形**；
- 它是**一次性**的：加在函数里，**所有**调用点（含 M1/M2/POC）同时受益，
  而**不必**在五处 m2 的调用点各改一遍——**这正是"能放在一处就不要放五处"的判断**；
- 对 reclaim 来说它**不解决根本问题**（枚举卡住仍然拿不到盘），
  但它把**整个项目的同类挂死**都变成**有诊断的失败**，
  这对 M5 其余增量的可测性有直接价值。

**待裁决事项因此更新为两个，且第一个更宽**：

1. **（本轮新增，建议先做）把界下沉到 `enumerate_disk_candidates()` 本身**——
   这属于我已经验证过的修法在**同一函数名下**的推广，
   但我**没有**在整层推广的授权，且它会触及 M1/M2 共用的驱动层，
   故**记下来请示**；
2. **让 reclaim 不依赖枚举**（用 `created` 的 SCSI 地址拼设备路径，或扩展 AIM 查询）——
   这是上一节列的，仍是设计判断。

**T1-03 仍不得标 PASSED。****本轮的产出是"影响面与统一修法"的判定，未改代码。**

#### 4.15.43 **这不是实现缺陷，是设计缺口：设计文档从未设计"如何找到那块物理盘"**（part 61，本轮）

**本轮去查设计文档有没有交代过这件事，结论是没有。**

对 `docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md`（434 行）逐词检索：

| 关键词 | 出现次数 |
|---|---|
| `SetupDi` / `DeviceInterface` / `dev_inst` | **0 / 0 / 0** |
| `SCSI` / `Scsi` | **0 / 0** |
| `PhysicalDrive` | **0** |
| `盘号` | **0** |
| **`盘符`** | **10** |

⇒ **设计文档对"盘符"有充分交代（10 次），但对"如何把 AIM 代理盘对应到 Windows 的物理盘"一次都没提。**
而 **M0/M3 的实现里必须有这一步**（拿到 `PhysicalDrive` 号才能对它 `remove`/`restore`），
于是实现自行选择了**全机器枚举磁盘接口**——

**这条路现在被实测证明在本项目的客机上不可靠**（会卡住 80+ 秒，见 parts 56–58）。

**因此这条应当被记录为设计缺口，而不是"T1-03 的实现 bug"**：
**设计没有规定发现机制 ⇒ 实现选了一个可行但脆弱的机制 ⇒ 在真实设备上暴露。**
这也解释了为什么它如此难修：**我在修一个没有设计依据的选择**，
而**正确的做法是先补上这个设计决定**，再按它改实现。

**待裁决（本轮升级为设计层，措辞尽量具体，便于决定）**：

> **myrd 应当如何把一块 AIM 代理盘对应到 Windows 的物理盘号？**
>
> 三条已知的路：
> 1. **不再依赖盘号**：改成按 **SCSI 地址**（`path/target/lun`，`DeviceId` 里已有）
>    直接构造设备接口路径并打开——**不枚举**。需确认 Windows 对 AIM 代理盘生成的路径格式；
> 2. **把枚举换成有界/定向的发现**：例如只查 AIM 适配器 `dev_inst` 下的子设备，
>    或换用挂载管理器等**能按身份查询**的接口；
> 3. **扩展 AIM 查询**：让驱动报告盘号（**已知当前 `DeviceConfiguration` 报的是 AIM 自己的设备号，
>    故需扩展协议/驱动**）。

**本轮已完成、且与上面选择无关的部分**（已提交，有干净构建与无回归证据）：
**给枚举加了界并下沉到函数本身**（`174799b`，part 60），
让**任何**调用者（含 M1/M2/POC）都**不可能再被它挂死**，
但**这只会把挂死变成快速失败，不会让 reclaim 成功**。

**T1-03 不得标 PASSED；且本轮确认它被一个设计决定挡住，不是被实现细节挡住。**

#### 4.15.44 **真正的原因找到了：拆卸不干净，累积出 22 个陈旧 AIM 盘条目，拖垮枚举**（part 65，本轮）

**本轮换了查法：不看 trace，直接查客机的设备实例表。结果一眼就清楚了。**

```
客机 AKIM 盘枚举条目：22 个
  HKLM\SYSTEM\CurrentControlSet\Enum\SCSI\Disk&Ven_Arsenal&Prod_Virtual\
    1&2afd7d61&0&000000
    1&2afd7d61&0&000001
    1&2afd7d61&0&000002
    1&2afd7d61&0&000100      ← 每跑一轮就对出一组
    ... 一直到 000701
而实际存在的物理盘：Get-Disk = 1，Win32_DiskDrive = 1
```

⇒ **客机的 SCSI 设备实例表里堆着 22 个 AIM 代理盘的残骸，而真实盘只有 1 个。**

**这就把整条因果链闭合了，而且每一条都能对上**：

1. `enumerate_disk_candidates` 走的是 **`SetupDiGetClassDevsW` 枚举磁盘接口**——
   **它枚举的正是这些实例**，**22 个里面 21 个是死的**；
2. 对每个候选，代码会 **`CreateFileW` 打开它、发 `IOCTL_SCSI_GET_ADDRESS` /
   `IOCTL_STORAGE_GET_DEVICE_NUMBER`**——
   **对一个已经不存在的设备发这两个 IOCTL，正是会慢甚至不返回的地方**；
3. 所以**枚举的工作量随运行次数单调增长**，
   **到第 3 次左右就慢到超过任何预算**：
   `-Repeat 20` 里 18 次连第 3 条用例都过不去、part 56 量到的 80+ 秒挂死、
   part 58 的 10 秒超时——**全部是同一个原因的不同表现**。

**而"22 个残骸"本身就是 T1-03 要解决的问题**：
**那正是"拆卸不干净"的痕迹**（每个 AIM 设备拆卸后应在设备实例表里被移除）。
**换句话说：我一直在追的"枚举为什么会卡"，
答案是"这个增量自己留下的垃圾把它拖垮了"。**

**这改变了修法的优先级**（与上一节的"设计缺口"并不矛盾，而是补上了它）：

| 层次 | 问题 | 说明 |
|---|---|---|
| **A（真正要修的）** | **拆卸没有清掉设备实例，残骸累积** | **这是 T1-03 的实质缺陷**，也是残骸的来源。修好它，枚举的工作量就不会增长 |
| B（防御） | 枚举无界 | **已加界**（part 58/60），让"卡死"变成"可诊断的失败" |
| C（设计缺口） | 设计没规定"如何按身份找到那块盘" | 仍值得补，但**不再是唯一的出路**：A 修好后枚举可能重新变得可用 |

**因此下一步（比"发明发现机制"更直接、更符合现有设计）**：
**查清"为什么拆卸之后设备实例还在"**——
`remove_owned_device` 走的是 `kRemoveDevice`（驱动侧移除，A' 裁决），
而**驱动侧移除可能只删掉设备对象、不删设备实例**，
于是实例表里留下"有实例、无设备"的条目。
**要查的是：**
1. **这些残骸条目是否对应真实存在的设备**（`CM_Locate_DevNode` 能否打开）；
2. **它们是在哪一步被留下的**：
   `kRemoveDevice` 之后、还是 worker 被杀导致 `kRemoveDevice` 根本没发；
3. **客机重启后它们是否消失**（若消失 ⇒ 是"活残骸"，运行期累积；
   若仍在 ⇒ 是持久化到注册表的陈旧条目）。
**这三条一次读数即可回答，且不需要任何设计裁决。**

**T1-03 不得标 PASSED。****本轮未改代码**（只做了客机设备实例表的诊断）。

#### 4.15.45 **残骸是"幽灵设备"且声称自己存在——而 `DIGCF_PRESENT` 正是按这个标志过滤的**（part 66，本轮）

上一节发现 22 个残骸条目，本轮做三件判定，其中两件已确定、一件仍待验证：

**（1）它们重启后仍在 ⇒ 是持久化的陈旧条目，不是"活残骸"。**
重启客机后立即查：**AIM 条目仍是 22**，worker 0、真实盘 1。
⇒ **它们写进了注册表且不会自己消失**（`HKLM\SYSTEM\CurrentControlSet\Enum\...`）。

**（2）它们是"幽灵设备"，而且标志位显示它们声称自己存在。**
逐条读 `ConfigFlags`：**22 个全部为 `0`**；`CSConfigFlags` 为空。
而 `Get-PnpDevice -Class DiskDrive` 的读数是：

| 项 | 读数 |
|---|---|
| 磁盘类设备总数 | **27** |
| 其中真实盘（`VMware Virtual NVMe Disk`） | **1**，`Status = OK` |
| 其中 `Arsenal Virtual SCSI Disk Device` | **26**，`Status = Unknown` |

⇒ **26 个 AIM 幽灵盘，Status 为 `Unknown`**，而**它们的 `ConfigFlags` 并没有置上
"已移除"那一位（`CONFIGFLAG_REMOVED` = 0x1）**。

**（3）因此"`DIGCF_PRESENT` 会不会把它们过滤掉"这个问题，答案倾向于"不会"，但尚未直接验证。**
`SetupDiGetClassDevsW` 是按**存在性**过滤的，而**存在性由那个标志/设备状态决定**；
**这些条目 `ConfigFlags = 0` 且实例仍在注册表里**，
**所以它们很可能仍然被 `DIGCF_PRESENT` 返回**——
**若如此，枚举就会对这些"无设备可打开"的幽灵逐个 `CreateFileW` 并发 IOCTL，
那正是会慢到 80 秒的地方。**

**必须诚实标注**：**（3）是推断，不是实测。**
**直接验证只要一个很小的程序**（或一次现有的 `enumerate_*` 调用加计数）：
**打印 `SetupDiGetClassDevsW(DIGCF_PRESENT)` 返回的候选数**——
**若 ≈27 则推断成立**；**若 ≈1 则我的整条机制解释错了**，
那就得回到"枚举为何会卡"重新查。
**这是下一轮第一件该做的事**，因为它决定后面所有修法方向。

**（4）这仍然指向同一个实质缺陷：拆卸留下了幽灵设备。**
无论（3）结论如何，**"跑一轮多几个幽灵、永不消失"本身就是要修的**：
每次建卷/拆卸都应在设备实例层留下**零**新增条目。
**而它与上一节的"设计缺口"是互补的**：
发现机制的设计缺口（C）值得补，**但如果幽灵会持续累积，
那么任何"枚举全机器"的实现迟早都会被拖垮**——
**所以先修"不再留幽灵"，再谈发现机制怎么设计。**

**下一步（两条，且第一条能证伪我自己的解释）**：
1. **数一次 `DIGCF_PRESENT` 的返回条目数**（≈27 还是 ≈1）——**决定机制解释成立与否**；
2. 若成立，**查幽灵是在哪一步留下的**：
   `kRemoveDevice` 之后仍留实例？还是 worker 被杀导致 `kRemoveDevice` 根本没发出？

**T1-03 不得标 PASSED。**

#### 4.15.46 **两个机制解释都被实测证伪；真实原因仍未知**（part 67，本轮）

**本轮写了两个一次性探针**（`tools/probe/disk_presence_probe.cpp`、
`tools/probe/disk_open_probe.cpp`，**只读查询、不创建不改状态**），在客机上直接量。
**结果把我前两轮的机制解释都推翻了。**

**探针一：`DIGCF_PRESENT` 会不会返回那 22 个幽灵盘？**

```
disk interfaces with DIGCF_PRESENT : 1        ← 只有真实的 1 个
disk interfaces without it         : 27       ← 不设 PRESENT 时才是 27
```

**⇒ 幽灵盘被 `DIGCF_PRESENT` 正确滤掉了。**
**所以 part 65/66 写的"枚举在 26 个幽灵上打转"是错的**——枚举根本看不到它们
（连跑 5 次结果一致，不是偶发）。

**探针二：枚举的每一步各要多久？**（把 `enumerate_disk_candidates` 拆开计时）

```
SetupDiGetClassDevsW              :    1.5 ms
SetupDiEnumDeviceInterfaces       :    0.0 ms   (got=1)
SetupDiGetDeviceInterfaceDetailW  :    0.0 ms
CreateFileW (与定位器同参数)       :    0.8 ms   (handle=ok)
IOCTL_SCSI_GET_ADDRESS            :    0.0 ms   (ok=1)
IOCTL_STORAGE_GET_DEVICE_NUMBER   :    0.0 ms   (ok=1, disk=0)
第二次 CreateFileW（持有句柄时）   :    0.0 ms   (handle=ok)
```

**⇒ 整条枚举路径（含打开与两个 IOCTL）在客机上只要约 2.5 毫秒，而且全部成功。**
part 66 猜测的"打开/IOCTL 会卡"**同样错误**，包括"持锁时会阻塞"那一条。

**因此必须更正**：**连续两轮的机制解释都不成立**，且**都是被直接量测推翻的**。
**`enumerate_disk_candidates` 在当前状态下又快又稳。**

**那"80 秒挂死 / 10 秒超时"是什么？现在只能说到这一步**：

- **挂死确实发生在 `wait_for_present_physical_drive` 里**（part 56 的 trace 是现场证据：
  当时 worker CPU 0.23 s、84 秒无进展）；
- **但它的组成部分在空闲状态下都是毫秒级**；
- ⇒ **触发条件不是"这台机器现在慢"，而是"某个特定状态下才慢/卡"**，
  最可能是**那块盘/卷正被 worker 持有、或 AIM 设备处于过渡态时**某一步才阻塞；
- **两个探针都是在"没有 worker、没有挂载卷"的空闲客机上跑的**，
  **测的是健康状态，不是出事时的状态**——**这正是它们没能复现的原因**。

**下一步（方法必须换，否则还会得到同样的证伪）**：
**在"出事的那个状态"下量**。可行做法：**把每步耗时埋进
`wait_for_present_physical_drive` 内部**（每次枚举/打开前后各一条 trace），
**跑 `-Repeat 3`，在失败那次读出逐步耗时**。
**这比"卡住时从旁边探"可靠**，因为它不依赖时机。**建议下一轮做这个。**

**T1-03 不得标 PASSED。**

#### 4.15.48 **读数指向一个不需要新裁决的修法：reclaim 可能根本不需要这次等待**（part 69，本轮）

上一节的读数让"这次等待是否必需"变成可查的问题，本轮查了，**证据倾向"不必需"**：

**（1）拆卸不使用盘号。** `remove_owned_device`（`m3_windows_acceptance.cpp`）**只用
`owned_device_`（AIM 设备号）与 `adapter_`**：
它的四步是 `remove_device(*owned_device_)` → 轮询 `list_devices()` →
PnP rescan → 等 PhysicalDrive 消失，
**全程不读 `info_.disk`，也不使用 `physical_drive_number`**。

**（2）`info_.disk` 的唯一读者是状态打印。**
全仓库检索 `info_.disk`：**只有 `src/m3/m3_aim_server_main.cpp:113/129` 在读它，
而那是把物理盘号打进状态输出**；此外就是 `m3_windows_acceptance.cpp:379` 的赋值本身。
⇒ **被这次等待"生产"出来的那个盘号，在拆卸链路上无人消费。**

**（3）因此这条因果链是成立的**：
`start_aim` → `wait_for_present_physical_drive`（**负载下会阻塞 >10 s**）
→ **而它产出的东西对 reclaim 没用** ⇒ **reclaim 为一件不需要的信息付出了阻塞的代价**。
**这正好是"不做不需要做的事"**（本增量已用过两次：reclaim 不 flush、不 dismount）。

**但要诚实标注三件事，它们决定能否直接改**：

1. **`wait_for_present_physical_drive` 可能还有"证明归属"的作用**：
   它在 `find_existing_device` 之后运行，而 part 30 的裁决要求
   **"接管前必须证明归属"**。若那一步的归属证明**只由 `find_existing_device`
   （按长度+扇区匹配）提供**，则等待确实只是"定位+把盘号填进 info_"，可以省；
   **但若归属证明也依赖这次等待，就不能简单删掉**——**这一点本轮未核实，必须查清**；
2. **`info_.disk` 为空会不会影响后续步骤**（例如 `wait_for_physical_drive_retired`
   是否需要盘号）——**同样未核实**；
3. **省掉它会改变 `M3AimRuntimeInfo` 的内容**，
   若有测试或调用方依赖 `info.disk` 有值，会破坏它们（目前只看到状态打印，**倾向安全，但需确认**）。

**因此在改之前必须先核实上面三条**（都是读代码即可，不需要上客机）。
**若三条都通过，就把这次等待从 reclaim 路径移除，并按质量门用 `-Repeat 20` 在客机上验证。**

**这仍然比"发明新的发现机制"更可取**：
**它绕开的是阻塞本身，而不是给阻塞加界。**

**T1-03 不得标 PASSED。****本轮未改产品代码。**

#### 4.15.49 **三项前置检查全部通过；可以移除，且只剩一个显示层注意点**（part 70，本轮）

上一节列的三个待核实项，本轮逐条查完（**全部是读代码，未上客机**）：

**检查 1：归属是否由别处证明？——是，而且这次等待发生在所有校验之后。**
`start` 的顺序是：
1. `find_existing_device(spec)`（**接管**路径）或 `create_proxy_disk(spec)`（**新建**路径）
   ⇒ **这一步就确立了"这块盘是我们的"**，并随即赋给 `owned_device_`；
2. `query_device(*owned_device_)` ⇒ **身份校验**：
   `id == owned_device_` **且** `size_bytes == geometry.virtual_bytes`
   **且** `sector_size == geometry.logical_sector_size`
   **且**（接管时豁免 `native_object_path`，因为那是**死 worker 的随机会话名**；
   新建时要求 `native_object_path == names.driver_path`）；
3. **然后才是** `wait_for_present_physical_drive`。

⇒ **"证明归属"与"身份校验"都在第 1、2 步完成，且都在这次等待之前。**
**这次等待不是归属证明**，它只是"把那块盘定位出来、填进 `info_.disk`"。
**这一点是上一节最大的疑点，现已排除。**

**检查 2：后续步骤是否需要盘号？——不需要。**
`wait_for_physical_drive_retired` 的签名是
**`(DeviceId owned, std::uint64_t expected_size_bytes, timeout)`**——
**它按 SCSI 地址与期望长度判断"盘已消失"，根本没有盘号参数**。
（`remove_owned_device` 同理，只用 `owned_device_` 与 `adapter_`。）

**检查 3：谁真正读那个被生产出来的盘号？——只有 m1 与状态输出。**
全仓库检索 `physical_drive_number` 的读者：

| 位置 | 用途 | 是否受影响 |
|---|---|---|
| `src/m1/m1_raw_protocol_io.cpp:121/141/377` | **M1 自己的原始协议 I/O**（不是 M3 链路） | **不受影响** |
| `src/m3/m3_aim_server_main.cpp:113/115/129` | **把盘号打进状态输出**（`info.disk...`） | **只影响"显示"** |

⇒ **M3 的拆卸链路一处都不读它。**

**唯一需要在实现时处理的注意点（本轮发现，必须写明）**：
移除之后，**reclaim 路径下 `info_.disk` 会是空**，
于是 **`m3_aim_server_main.cpp` 里那两处状态输出会打印一个空的盘号**。
**这不是正确性问题**（拆卸不依赖它），**但是"对外可见的行为变化"**：
- 若 M5 的服务/CLI **会把这个状态读给用户看**，那**reclaim 之后该字段会是空的**，
  需要**明确它是"未知/不适用"，而不是静默留空**；
- **实现时应当让该字段在 reclaim 下显式表达"不适用"**，
  并在证据里记下这个行为变化。

**结论**：**三检查通过 ⇒ 把 `wait_for_present_physical_drive` 从 reclaim 路径移除是安全的**，
代价只有上面那一处显示层注意点。**按质量门，改完必须在客机上用 `-Repeat 20` 验证**，
并且**要检验的正是"移除后 reclaim 是否不再卡"**——
**若仍卡，则说明这次等待不是阻塞源，必须回到"细分枚举内部各步"那条路。**

**T1-03 不得标 PASSED。****本轮未改产品代码**（本轮的价值是把"可以改"变成"已核实可以改"）。

#### 4.15.50 **实施前的最后一个障碍：M3 目前不知道自己是 reclaim**（part 70 续，本轮）

开始实施时发现**真正的改动点不在 `m3_windows_acceptance.cpp` 那几行，而在"模式如何传到 M3"**，
**这必须在动手前说清楚，否则会做成半截改动**：

**事实**：
- `M3AimRuntime::create(config, device_timeout)` 与 `M3AimProviderFactoryConfig`
  **都没有"模式"这个概念**——`config` 的字段只有
  `OperationContext context`、`M3ProviderConfig provider`，以及测试缝 `M3ProviderFactories`；
- **模式只存在于上层**：`WorkerVolumeMode::reclaim_existing`（host）
  → `DiskRuntimeMode::reclaim`（`src/run/disk_runtime.{h,cpp}`）；
- 而 `M3AimRuntime` 是**由 host 通过注入的 `operations_.start_aim` 回调**构造的
  （`m5_worker_runtime.cpp`）——**也就是说，模式在 host 那一侧是已知的，
  只是没有被传进 M3。**

**因此实施路径是清楚的，但要动三层**：

1. **`M3AimProviderFactoryConfig` 增加一个模式字段**
   （例如 `bool reclaim_only{false}` 或一个小型枚举）；
2. **host 在构造 M3 时把模式填进去**（`m5_worker_runtime.cpp` 里
   由 `volume_mode` 决定，那里已经有 `WorkerVolumeMode::reclaim_existing` 的判断）；
3. **`M3AimRuntime::start` 在该模式下跳过 `wait_for_present_physical_drive`**，
   并把 `info_.disk` 置为**显式的"不适用"**（见 part 70 的显示层注意点）。

**另外两件要在同一改动里落实的**（否则会留下不一致）：
- **`info_.disk` 的可选性**：若它变成"reclaim 下为空"，那么
  `m3_aim_server_main.cpp` 的两处状态输出必须能处理空值
  （**显式表达"不适用"**，而不是打印空盘号）；
- **`M3AimRuntimeInfo` 的既有消费者**：目前只看到那两处状态输出，
  但**改完后要用 `git grep` 复核一次**（含 tests），确认没有别的读取者。

**为什么本轮不直接改**：它跨 3 个文件、涉及一个**对外可见的字段语义变化**，
而**本轮剩余的上下文不足以"改 + 干净构建 + 客机 `-Repeat 20` + 证据"走完整个收尾循环**。
**按本项目的纪律，宁可留下"已核实可以改 + 精确的改动清单"，也不留下半截改动。**

**下一轮的完整清单（可直接执行）**：
1. 加 `M3AimProviderFactoryConfig::reclaim_only`；
2. host 侧填值；`start()` 在该模式下跳过物理盘等待、`info_.disk` 置"不适用"；
3. 处理状态输出的空值；
4. 干净构建 0 error 0 warning；
5. **客机 `-Repeat 20`**：**要看的正是"reclaim 是否不再卡"**；
   - **若通过** ⇒ T1-03 的阻塞解除，可进入收尾；
   - **若仍卡** ⇒ **这次等待不是阻塞源**，回到"细分枚举内部各步耗时"那条路
     （见 part 68 末尾）。

**T1-03 不得标 PASSED。**

#### 4.15.47 **内侧仪表给出明确读数：负载下枚举第 1 圈就吃掉整个 10 秒预算**（part 68，本轮）

按上一节定下的方法，把仪表**埋进 `wait_for_physical_drive` 内部**后跑 `-Repeat 3`
（请求 `1d5c110342a449fdadfa73f236bbecb7`），**trace 里出现的就是现场**：

```
20:42:45.944Z wait_for_physical_drive: enumeration failed on turn 1 after 10011 ms
              (total 10011 ms) -- the bound fired
```

**这一条读数同时确定了两件事，而且它们只有合起来才有意义**：

| 量测 | 条件 | 结果 |
|---|---|---|
| `tools/probe` 的逐步计时（part 67） | **空闲客机**（无 worker、无挂载卷） | **整条路径 ~2.5 ms，全部成功** |
| 内侧仪表（本轮） | **套件运行中**（正在建/拆卷） | **第 1 圈就 >10 s，触发界** |

⇒ **同一个函数、同一台机器：空闲时 2.5 毫秒，负载下超过 10 秒。**
**"负载"是唯一变量。** 这解释了为什么两个外侧探针必然失败——
它们测的是健康状态；**也解释了为何前两个机制猜测都不成立**——
**幽灵盘、打开/IOCTL 都不是原因，真正的原因是"在卷被持有/正在拆卸时，
这条枚举路径会阻塞"。**

**诚实边界（本轮到此为止）**：**内侧仪表只报"这一圈花了 >10 s"，
没有细分到"枚举 / 获取接口细节 / 打开 / 两个 IOCTL"中的哪一步**——
因为被放弃的那个 detached 线程仍在内核里，**拿不到它的分步耗时**。
**要拿到那一步，需要把计时再下沉一层**（在 `enumerate_disk_candidates_unbounded`
内部逐步打点，并让那些打点**在超时后仍能被读到**）。

**更重要的是：本轮的读数让"是否还需要枚举"这个问题更有价值了。**
`reclaim` 路径**本来就知道自己的 `DeviceId` 和盘符**（残留设备是它自己之前创建的，
盘符也记录在 profile 里）。**若拆卸时不必"找到盘号"**，
那么**这条会阻塞的枚举就可以完全不进 reclaim 路径**——
**这正是"不做不需要做的事"，而且它绕开的是阻塞本身，不只是给它加界。**

**因此建议下一轮的方向**（技术上可直接做，不需新裁决）：
1. **查清 reclaim 路径是否真的需要 `wait_for_present_physical_drive`**——
   它需要物理盘号做什么？（`remove_owned_device` 用的是 `kRemoveDevice`（驱动侧），
   **而 part 34 的读数显示那四步不需要盘号即可完成**）；
2. **若确实不需要，就把该等待从 reclaim 路径移除**，
   并**在客机上用 `-Repeat 20` 验证 reclaim 是否稳定**；
3. **与此同时保留界**，让其他调用者仍不会被挂死。

**T1-03 不得标 PASSED。**

#### 4.15.51 **"移除这次等待"试做后撤回：`disk` 字段不是可选的，而且有验收在用它**（part 71，本轮）

**按上一节的清单开始实施，做到第 5 步发现一个未预见的前提，因此撤回、不留半截改动。**

**已做过的改动（现全部 `git checkout` 还原；树干净、构建 0/0）**：
1. `M3AimProviderFactoryConfig` 加 `reclaim_only`；
2. `ProductionRuntimeAdapter::Config` 加同名字段——**声明在最后**，与 `image_uuid` 同一手法，
   使既有聚合初始化器不变义——host 侧由 `volume_mode == reclaim_existing` 填值；
3. `runtime_adapter.cpp` 把它转交给 M3 工厂；
4. `M3AimRuntime::start` 在 `reclaim_only` 时跳过 `wait_for_present_physical_drive`；
5. `m3_aim_server_main.cpp` 两处状态输出改为打印 `null`（而不是默认的 `0`）。

**编译器立刻否掉了第 5 步的前提**：

```
m3_aim_server_main.cpp(116): error C2039: "has_value" 不是 "myrd::aim::LocatedDisk" 的成员
m3_aim_server_main.cpp(117): error C2819: "LocatedDisk" 没有重载 "operator ->"
```

⇒ **`M3AimRuntimeInfo::disk` 不是 `std::optional<LocatedDisk>`，而是普通的
`aim::LocatedDisk`**（默认构造就是"0 号盘"）。
**所以"reclaim 下把它留空"在类型上不成立**，必须**先把字段改成 optional**。

**而改成 optional 引出了真正的阻塞点（本轮关键发现）**：
`git grep` 找到 **`src/run/runtime_adapter.cpp` 有三处读 `disk.physical_drive_number`**，
其中 **`:421` 是 `verify_volume` 的验收条件**：

```cpp
actual.extents.front().disk_number !=
    impl_->aim_runtime->info().disk.physical_drive_number ||   // 用盘号核对"这个卷是不是我们的"
```

⇒ **盘号不只是显示，它还参与"卷归属"的核对。**
**因此把 `disk` 变成 optional 会一路牵到 `verify_volume`**，
而**"reclaim 时 `verify_volume` 是否需要盘号、那时它会不会被调用"本轮没有查清**。

**撤回的理由（按本项目纪律）**：
- 这**不是实现细节，而是字段语义变更**（`info.disk` 由"总有值"变成"可能没有"），
  会波及所有读者，**包括一个验收条件**；
- **在没查清 `verify_volume` 是否在 reclaim 路径上被调用之前就改，
  等于把风险从"reclaim 卡住"换成"卷归属核对失效"**——
  后者更危险，正是"按身份核对"这条硬门要防的事；
- 本轮剩余上下文不足以"改 + 干净构建 + 客机 `-Repeat 20` + 证据"走完整循环。

**因此本轮以"撤回 + 记录"结束，净改动为零**
（树干净、构建 0/0、`wait_for_present_physical_drive` 仍在原处未动）。
**这是有意为之：宁可零改动，也不留一个改了一半的跨层改动。**

**下一步必须先查清的两件事（都是读代码）**：
1. **`verify_volume` 在 reclaim 路径上是否会被调用**——
   若**不会**，"reclaim 下 `disk` 为空"就碰不到它，风险立刻大幅下降；
   若**会**，则必须给 reclaim 一条不依赖盘号的核对方式
   （`verify_volume` 本来也在比 `volume_guid`，这是现成的替代身份）；
2. **另两处读者（`:310`、`:426`）的语义**——
   `:426` 只是诊断打印（可像状态输出那样处理 `null`）；
   `:310` 是 `aim_disk_number()` 的返回值，**要看调用方能否接受"没有盘号"**。

**另有一条更保守的替代路（本轮想到，同样待评估）**：
**保留这次等待，但只跳过"把盘号写进 `info_`"**，
即**不动字段类型**，从而不牵动 `verify_volume`。
**需确认 `wait_for_present_physical_drive` 能否在"不返回 `LocatedDisk`"的前提下
仍完成它必须做的事**——它现在就是为返回这个而存在的。

**T1-03 不得标 PASSED。****本轮净改动为零**（试做后按纪律撤回）。
