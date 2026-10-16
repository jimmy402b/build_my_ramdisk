# M5 总体执行计划（单一执行依据）

建立日期：2026-10-15
分支：`feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`
基线：`main` = `cb3df08`（**必须保持不动**）
设计：`docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md`（v3，取代旧文本）
逐条清单：`docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`

> **这份文档的定位**：它是"从今天到项目做完"的唯一执行依据。tasklist 回答"有哪些增量"，
> 本文回答"按什么顺序、每一步的验收是什么、什么时候必须停下来问"。
> 冲突时：**设计 > 本文 > tasklist**。进度变化只改本文与 tasklist，不改设计。

---

## 0. 怎么读这份文档

每个增量有一张卡片，五个字段：

| 字段 | 含义 |
|---|---|
| **状态** | `DONE` / `NEXT` / `TODO` / `BLOCKED`（等人裁决） |
| **依赖** | 必须先完成什么。有环即设计错误 |
| **做什么** | 范围内的东西。**范围外的东西也写**——否则一定会被重新提起 |
| **怎么验** | 开发机（纯逻辑）与虚拟机（真实设备）各自的读数要求 |
| **挂在哪个目标上** | G1（GUI）/ G2（单盘 CLI）/ G3（断电恢复）/ G4（多盘隔离）。**没有目标的增量不该存在** |

**工作量口径**：`S` ≈ 一次收尾循环；`M` ≈ 数次；`L` ≈ 需要重开一层的证据。
**这是相对量，不是工期承诺**——这个项目已经四次证明"看起来小"的增量能翻出真缺陷
（T1-11 一个"补个锁"变成三层修复）。

---

## 1. 四个目标（验收的唯一依据）

用户原话：

> M5 能够实现 GUI 界面，管理单盘/多盘；M5 最少能够实现单盘的控制管理；能够实现断电自动恢复；
> 实现多盘的时候，要求多盘无冲突，独立磁盘和单盘行为一致。

| 目标 | 可验收的措辞 |
|---|---|
| **G1** GUI | Win32 薄客户端：列出全部盘（名称/盘符/状态/generation/**内存占用**）、新建/导入/启动/停止/checkpoint、看得见进度与失败原因、**长操作期间界面不假死**、未提权可只读 |
| **G2** 单盘 CLI | 一个盘端到端：注册 → 启动 → 读写 → checkpoint → 停止 → 重启恢复 → 移除，全走 CLI；只有 confirmed committed 才报成功；退出码 0–5 稳定 |
| **G3** 断电恢复 | VMware 强制断电重启后，所有 `auto_start=true` 的盘**无人工介入**恢复到自身最后完整已提交 generation；**包括断电正好落在 P4/P5 之间**；绝不重新分区/格式化；判不了就报 needs-attention |
| **G4** 多盘隔离 | N 盘同时跑；A 在任一环节失败时 B 的 generation / 挂载 / worker / operation gate / writer lock **全部不变**；**且 A 的长操作不得让 B 排队或超时**（延迟也是行为） |

**项目完成的定义**：G1–G4 全部验收通过 + T4-04 的 release manifest 冻结。

---

## 2. 当前位置（截至本文建立）

| 层 | 状态 |
|---|---|
| M0 / M1 / M2 / M3 | 已验收（**证据取自开发机**；VM 上只有间接覆盖） |
| M4 | 已验收（VM 战役只跑了**聚焦 9 项**，不是完整套件） |
| M5 | T1-01 与 T1-11 完成；T1-02 完成；其余未开始 |

**已完成的实质修复（全部有量测）**：

| 修复 | 读数 |
|---|---|
| M2 `backing_generation` 测试钩子冒充生产不变量 | 修前 ~50% 崩溃 → **20/20** |
| T1-11 stop 路径补写冻结（三层） | `-Repeat 20` **20/20**（修前 5/20 失败） |
| `integration.m1.shm` 契约违约 | 整套 ×3 Sweep **432/438**，该用例 **3/3**（修前 1/3 失败） |
| T1-02 组合根 + SCM 骨架 | `sc create`/`start`/`query`/`stop` 全通，无残留 |

**当前整套基线**：VM 上 **144/146**，连跑三遍逐项一致；唯二失败是客机无 Visual Studio 的两个
structural 用例（已知环境受限）。

**已行使 vs 未行使（这是"还剩多少未知"的诚实答案）**：

- 已真正跑过的路径：worker 启停与握手、create_bundle、checkpoint 两阶段、stop（含冻结）、
  多盘同时存活与单盘停止、服务 SCM 生命周期。
- **从未跑过的路径**：job object 与崩溃回收、启动 sweep、读/写分离、public pipe 与 CLI 全命令面、
  中断提交判定（P4/P5 之间的崩溃恢复）、内存可见性与 UNMAP 端到端、
  suspend/内存压力 checkpoint、断电恢复、GUI。
- **经验规律（四次验证过）**：每首次行使一条新路径，掉出 1–2 个真缺陷。
  **因此 T1-03 → T4-04 期间仍应预期若干缺陷**，这不是悲观，是已经量测到的命中率。

---

## 3. 不变量与硬门（任何增量都不得违反）

来自设计 §2 与 `AGENTS.md`，逐条都会在收尾时核对：

1. **唯一执行链路**：public pipe → ServiceCore → per-profile worker → DiskRuntime → M3 → phdskmnt。
2. `myrd_assert_execution_boundary` 覆盖 `myrd_management` / `myrd_product_ipc` / `myrd_profile` /
   `myrd_m5_worker_protocol`，并有负例测试。**新目标也要挂上这个检查。**
3. 不新增第二套状态机、JSON IPC、profile 格式或 host 执行核心。
4. **不改 M3 的 bundle / journal / manifest 布局**，不改 M4 前台单阶段 checkpoint 行为。
5. `dirty_epoch` 绝不跨会话比较；"是否 dirty"以 worker 报告的 M3 块级 dirty 为准，
   答不出来即 unclean。
6. **任何任务不得安装 / 替换 / 升级 / 卸载 `phdskmnt`**；也不得分发 AIM 二进制。
7. `disk remove` / `forget` 无法证明 writer lock / 匹配设备 / bundle identity 安全时**拒绝**，
   保留 registration 与 bundle，**绝不伪报成功**。
8. worker replacement 不得触发 removal 或 synthetic checkpoint success。
9. 每次变更都由 scheduler 拥有；客户端超时永不取消一个已安全运行的 operation。
10. 单盘故障只清理该盘**已验证拥有**的资源；强制终止按 profile 粒度，
    **绝不 `TerminateJobObject` 连坐**。
11. 中断提交判定路径**只读**：不建 runtime、不持 writer transaction、不改 bundle。
12. `ManagedProfileSnapshot` 不得进任何客户端投影（不得泄漏路径）。
13. 每个增量都在虚拟机取得读数；开发机只跑**不创建真实 worker / AIM 盘 / Windows 卷**的测试。

**流程门（每个增量收尾都必须过）**：

- 改了**任何头文件** ⇒ 测量前 `cmake --build out/build/msvc --config Release --clean-first`
  （否则得到 `0xC0000005` 假崩溃，已被咬过两次）；
- 干净构建 0 error 0 warning；
- 修复类结论**至少 `-Repeat 20`**；任何"绿"的声明必须写明跑了几次；
- 证据写入 `docs/validation/`，提交到 `feature/m5-lifecycle`，更新本文与 tasklist 状态。

---

## 4. 执行顺序（依赖图）

```text
T1-01 ✅ ─→ T1-02 ✅ ─┬─→ T1-03 ─→ T1-06 ─┐
                     ├─→ T1-04 ─→ T1-05 ─┼─→ T1-09 ─→ T1-10
T1-11 ✅ ────────────┘        │         │
                              └→ T1-07 ─┤
                              └→ T1-08 ─┘
T1-09 ─→ T2-01 ─→ T2-02 ─→ T2-03
T2-02 ─→ T3-01 ─→ T3-02 ─→ T3-03 ─→ T3-04 ─→ T3-05
T3-05 ─→ T4-01
T1-09 ─→ T4-02 ─→ T4-03 ─→ T4-04
```

**关键路径**：`T1-03 → T1-06 → T1-09 → T2-01 → T2-02 → T3-01…T3-05 → T4-01 → … → T4-04`。
`T1-05` 体量最大但不在关键路径上；**`T1-06` 是关键路径上最容易低估的一步**
（设计 §14.3 有七条用例，且现在 `reconcile_checkpoint` 正面路径零测试）。

---

## 5. T1 —— 单盘可用（G2 + G3）

### T1-01 ✅ DONE — VM 测试回路

读数 **144/146**。遗留：两个间发问题（均由后续增量修掉）、测试硬编码开发机路径（见 §8）。

### T1-02 ✅ DONE — 组合根 + 服务骨架

`sc create`/`start`/`query`/`stop` 全通；`RUNNING` 报 `ACCEPTS_PRESHUTDOWN`；`STOPPED` 且
`SERVICE_EXIT_CODE 0`；退出后无 service/worker/disk 残留；无盘干净退出；ProgramData 根 **fail-closed**
（由 T4-01 安装器创建）。证据：`docs/validation/m5-t1-02-service-skeleton.md`。

**范围外**：逐盘关停（归 T1-03/T1-05）；OS 真的发起 preshutdown（归 T1-09）。

---

### T1-03 — job object + 启动 sweep + 资源身份持久化

| 字段 | 内容 |
|---|---|
| **状态** | **IN_PROGRESS**（part 1 决策核心 + part 2 job object/verdict，合计 **18 用例通过**） |
| **依赖** | T1-02 |
| **工作量** | L |
| **目标** | G3、G4 |

**已完成 part 1（`8bb2b36`）**：`worker_sweep_policy.{h,cpp}`——两个纯函数决策：
**pid 不是身份**（creation time 不匹配 ⇒ `pid_recycled`）、**身份不完整 ⇒ 不回收**（`nullopt`）、
**"判不了"不是许可**（零 pid / 缺 creation time ⇒ `indeterminate`）、generation 0 是真实 generation。

**已完成 part 2（`2daeae5`）**：`windows_worker_job.{h,cpp}` + `worker_process_source.{h,cpp}`：

- **命名 job** `Global\MyRamDisk.M5.Workers`（`CreateJobObjectW` 会打开已存在的 job ⇒ 重启能找到前任的 worker）；
- **不设 `KILL_ON_JOB_CLOSE`**（设了 = 服务死则 worker 死 ⇒ 跑不到有序拆卸）；
  `SILENT_BREAKAWAY_OK` 让 worker 自己 spawn 的 provisioning helper 不加入本 job；
- **只有按进程终止**：`IWorkerProcessSource` 没有 job 级对应物，
  并用 **compile-time 断言**钉住"加了 job 级终止就编译失败"（比等到两块盘才暴露早得多）；
- **`sweep_verdicts`**（纯函数）：`adopt` / `terminate_cooperatively` / `terminate_forced` / `leave_alone`，
  输入是"job 报的进程" + "durable 记录里的绑定"——job 知道哪些进程在里面，**不可能知道各自服务哪个 profile**；
- `terminate_process` 在**终止前重读 creation time**，pid 已不再指向观测到的进程就拒绝
  （verdict 基于的观测到执行时可能已经过期）。

**诚实记录**：对**上一个服务会话**的孤儿 worker，**协作退出无法送达**——私有通道需要那个会话的
startup secret，而它随父进程死了。所以这条路径报明确失败而不是伪造成功；孤儿能被执行的是"被终止"，
它留下的 durable 记录由恢复路径对账（**那是 T1-06 的题目**）。

**已完成 part 3（`7349efd`）**——job 接线 + 句柄保留 + 观测修正，证据：
`docs/validation/m5-t1-03-worker-job.md`：

- `launch_private_worker` 增加**必需**的 job 参数，用 **`PROC_THREAD_ATTRIBUTE_JOB_LIST`** 随创建属性指派
  （不是创建后再 `AssignProcessToJobObject`——那会留一个"worker 在所有 job 之外运行"的窗口）；
- provider **保留句柄**（不再由 `close_launched_worker` 丢弃），且**在构造 session 之前**保留，
  这样 session 构造失败也不会留下"在 job 里但够不到"的 worker；
- **抓到 2 个产品缺陷**：① `SILENT_BREAKAWAY_OK` 让 job **不再记账**其成员（改为 `BREAKAWAY_OK`）；
  ② `JobObjectBasicProcessIdList` 对一个**确实含进程**的 job 报 0 个成员
  （`IsProcessInJob` 报 1）⇒ 改为**遍历进程表 + 逐进程问成员资格**；
- **修正 2 个工具链缺陷**：③ `-Stage` 静默丢包（现在**在客机上数**可执行文件，不符即报错并写明
  "不要把随后的运行失败当成产品缺陷"）；④ 本文件带 BOM 而 `powershell -Command -` 不剥 BOM，
  导致远程脚本首行失败（`Invoke-Remote` 补空行吸收；顺带修好了先前就存在的 git 元数据解包失败）；
- **验证**：`m5.production_execution_backend` **3/3**（`-Repeat 3`），含
  "**重开的 job 句柄仍能看到活着的 worker**"——**这是整个 sweep 成立的前提**。

**已知限制（写清楚，未验证）**：`AssignProcessToJobObject` 在提权 SSH 会话里返回
`ERROR_ACCESS_DENIED(5)`，所以工具链里每个进程都已在**别人创建的 job** 中，
`IsProcessInJob(.., nullptr, ..)` 因此会因无关原因报 true；加上"关掉最后一个句柄再按名字重开未必是同一对象"，
**"关闭 job 不杀 worker"这条性质在本测试环境里无法干净测量**。该用例**删除而非空过**，
交给 part 3 的"真杀服务"验收。

**剩余（part 3 收尾，下一步）**：

1. **持久化"最后观测身份"**（喂给 part 1 的 `reclaimable_resources`）；
2. **服务启动时跑 sweep**（读 durable 绑定 → `sweep_verdicts` → 执行）；
3. **强杀验收**：强杀服务 → worker 自行有序拆卸；强杀 worker → 下次启动 sweep 回收不留设备/盘符；
   无法证明归属 → 不清且报 needs-attention。**至少 `-Repeat 20`**。

**做什么**（设计 §5）：

1. **命名 job object** `Global\MyRamDisk.M5.Workers`；**不设** `KILL_ON_JOB_CLOSE`
   （设了等于"服务死 → worker 被杀" → worker 跑不到有序拆卸 → 留设备与盘符）；
   **保留进程句柄**（现在被 `close_launched_worker` 丢弃；没有句柄就没有单盘粒度的强制终止）。
2. **协作优先 + 有界升级**：发私有 `shutdown` → 等进程退出（用保留的句柄）→
   超时则 `TerminateProcess`（**按 profile 粒度，绝不 `TerminateJobObject`**）→ 记 needs-attention。
3. **启动 sweep**：建/开 job → `JobObjectBasicProcessIdList` 枚举 →
   对每个 PID 读 creation time 与**持久化的 worker 绑定**核对（防 PID 回收）→
   不属于本服务的协作终止。
4. **服务侧持久化"最后观测身份"**：bundle file identity、AIM 设备身份、卷 GUID + extents、
   盘符、generation。来源是 worker status 已经携带的 `WorkerBundleEvidence` + volume identity。
5. **残余回收**：按身份匹配回收**只属于本 profile** 的 AIM 设备 / 卷 / 盘符。
   **无法证明归属 → 不清理**，报 needs-attention，保留证据。**绝不按进程名 / AIM 编号 / 显示名清理。**

**范围外**：`windows_worker_recovery.{h,cpp}` **不重建**（设计 §15：本期不做 worker 生存，
该组件无消费者）；运行期租约不存在（§3.4）。

**怎么验**：

- 开发机（纯逻辑）：job/sweep/升级决策的状态机 + 句柄生命周期，全部用 fake，不建真实进程。
- **虚拟机（真实设备）**：
  - 强杀**服务** → worker 自行有序拆卸（不留设备/盘符）；
  - 强杀**worker** → 下次启动 sweep 回收，不留设备/盘符；
  - **无法证明归属时不清且报 needs-attention**（这条必须有负例）。
- 每个结论**至少 `-Repeat 20`**：这是崩溃回收类行为，单次绿是弱证据。

---

### T1-04 — 读/写分离

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-02 |
| **工作量** | M |
| **目标** | G1（解 GUI 假死）、G4（延迟隔离的一半） |

**做什么**（设计 §6.2 T1）：变更命令取**独占** gate；查询命令
（`disk list` / `show` / `operation list` / `status`）取**共享** gate，或按逐条审计结论完全不加锁。

**依据**：`ServiceCore` 的全部可变状态只有 `pending_mutex_` + `pending_`（其余全是注入引用），
且 `pending_` 自带锁 ⇒ **风险集中在注入的存储上，必须逐条审计而不是一刀切**。

**怎么验**：

- 开发机：锁语义与审计结论的纯逻辑测试。
- **虚拟机**：一个盘正在 create 时，`disk list` **1 秒内返回**；且该读数要**写明跑了几次**。

**风险**：这里的"逐条审计"是这层最容易出错的地方——**不能只测 `disk list` 快就宣布完成**，
要对每一条查询命令给出"为什么不加锁是安全的"的依据。

---

### T1-05 — public pipe 全命令面 + CLI

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-04 |
| **工作量** | L |
| **目标** | G2 |

**做什么**：public pipe 全命令面 + CLI：`create` / `import` / `list` / `show` / `start` / `stop` /
`checkpoint` / `config set auto_start` / `remove` / `forget` / `release-memory`；
**只渲染 typed response**；**退出码 0–5**。

**范围外**：GUI 不消费 CLI 的文本 parse/render（设计 §10.2 明确）。

**怎么验**：

- 开发机：命令解析、typed response 渲染、退出码映射的纯逻辑测试。
- **虚拟机**：单盘端到端 `create → start → 读写 → checkpoint → stop → 重启恢复 → remove`（G2 原话）。
- 另外验设计 §8 的 data flow 分支：`start` 的模式由 **durable lifecycle** 决定（绝不猜 bundle 状态）；
  `import` 的 writer lease 必须覆盖"校验 → registration 发布"全程。

---

### T1-06 — 中断提交判定可达（关键路径上最容易低估的一步）

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-03, T1-05 |
| **工作量** | L |
| **目标** | **G3（硬要求）** |

**做什么**（设计 §4.3 / §14.3）：从 P4 durable correlation 造 launch 启**只读**检查 worker →
`recovery_proof` 三态映射 → **§14.3 的七条用例全绿** → **检查路径只读**（bundle 哈希不变）。

**现状**：`reconcile_checkpoint` 只有**两条拒绝用例，正面路径零测试**——这是本节必须关掉的第一笔账。

**怎么验**：

- 开发机：三态决策与 correlation 校验的纯逻辑测试。
- **虚拟机**：在 P4/P5 之间制造服务崩溃 → 重启后盘能自行恢复或明确报 needs-attention，
  **不卡死**；且检查前后 **bundle 的 generation 与文件哈希不变**。
- **必须 `-Repeat 20`**：这是崩溃点注入类测试，单次绿没有意义。

---

### T1-07 — 内存可见性

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-05 |
| **工作量** | M |
| **目标** | G1（GUI 核心列）、G2 |

**做什么**（设计 §7）：新增贯穿各层的事实 `resident_bytes` / `budget_bytes`：

```
storage → persistence/M3 → run(DiskMaintenanceStats) → host(WorkerRuntimeStatus)
        → management(ServiceResourceProjection，不得含路径) → CLI/GUI
```

并**首次端到端验证 UNMAP**：删文件后常驻内存下降。

**⚠ 实现纪律（设计 §7.3 的风险提示，本项目已栽过三次）**：跨层新增一个事实，最容易在中间断掉。
历史案例：manifest §7 item 18 的 volume identity 无生产者、item 22 的 `BundleRegistrationEvidence`
无生产者、item 23 的 image uuid 被随机生成。
**每加一层都要有单测钉住"这个值从哪来"，而不是只问"它到哪去"。**

**怎么验**：开发机验贯通；**虚拟机**验 CLI 报出的 resident/budget/dirty 与任务管理器同量级 +
删文件后常驻内存下降。

---

### T1-08 — suspend / hibernate 与内存压力的有界 checkpoint

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-05 |
| **工作量** | M |
| **目标** | G3（旁支） |

**做什么**（设计 §9）：profile 里 `checkpoint.on_memory_pressure{true}` 与 `on_suspend{best_effort}`
**已有字段但没有运行期消费者**（M4 run loop 只处理 interval 与 dirty 阈值）。本期接上消费者，
并接 `SERVICE_CONTROL_POWEREVENT` / `PBT_APMSUSPEND`。

**怎么验**：开发机验策略与预算分摊；**虚拟机**验休眠/恢复后 generation 前进且盘可用、
内存压力下触发 checkpoint。

---

### T1-09 — M5-A 单盘验收 + 断电恢复（第一个大验收门）

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-06, T1-07, T1-08, **T1-11 ✅** |
| **工作量** | L |
| **目标** | **G3 全部** |

**做什么**：VMware 强制断电 → 开机自动恢复到最后完整 generation，**无人工介入、无残留**。

**怎么验**（这是验收门，标准要写死）：

- 断电点必须覆盖**三种**：正常运行时、checkpoint 提交前后、**正好落在 P4/P5 之间**；
- 每次断电后：所有 `auto_start=true` 的盘各自恢复到**自身**最后完整已提交 generation；
- **绝不重新分区/格式化**；判不了 → needs-attention（不猜）；
- 无残留：无孤儿 worker、无多余 AIM 盘、无残留盘符；
- **前置条件**：先做一次**整套 Sweep 顺序普查**（见 §8 遗留 3），确认基线可信再宣布 M5-A 通过。

**风险**：这一节会第一次真正行使"断电 + 恢复"整条链路，**应预期掉出缺陷**。

---

### T1-10 — 默认策略收紧

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-09 |
| **工作量** | S |
| **目标** | G3（"崩溃最多丢一个间隔"要能被用户读懂） |

**做什么**：默认 interval 60 s、dirty 阈值默认开启；把"崩溃最多丢一个间隔"写进**使用文档**
（不是设计文档）。

---

## 6. T2 —— 多盘（G4）

### T2-01 — per-profile 串行

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-09 |
| **工作量** | L |
| **目标** | **G4 核心** |

**做什么**（设计 §6.2 T2）：存储层 gate 的键从"store 路径"改为**按 profile 键**；
共享的 `request_index` 等容器另有自己的互斥；组合根按 profile 分锁；
`ServiceWorkerControl` 的 session 容器加锁（其头部已声明"不是并发对象"，
需要它本身安全或由 per-profile 锁保证）。

**⚠ 锁序必须写死并有测试**（profile gate → store 内部锁），避免死锁。

**怎么验**：开发机验锁序；**虚拟机**验"A 正在 create 时 B 的 start 不被阻塞" +
**并发 100 次无死锁**。

---

### T2-02 — 多盘隔离矩阵

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T2-01 |
| **工作量** | M |
| **目标** | **G4 验收面** |

**做什么**：矩阵——A 在 create / checkpoint / stop / removal / preshutdown **失败**时，
B 的 generation / 挂载点 / worker / operation gate / writer lock **全部不变**。

**怎么验**：**虚拟机**，真实双盘，每个失败点单独验；`-Repeat 20`（失败注入类）。

---

### T2-03 — 双盘断电矩阵

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T2-02 |
| **工作量** | M |
| **目标** | G4 + G3 交叉（M5-B / M5-D） |

**做什么**：断电后 A/B 各自恢复到最后完整 generation，**不混 generation、无残留**。

---

## 7. T3 —— GUI（G1）

### T3-01 — GUI client 库（物理隔离）

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T2-02 |
| **工作量** | M |
| **目标** | G1 |

**做什么**（设计 §10.2 / §10.6）：GUI 只链 `myrd_product_contracts` + `myrd_product_ipc` +
一个纯 client 库；用 `src/control`（`Command` / `ipc_frame` / `PublicPipeClient` / 鉴权）与
`service_response_wire.h` 的 `decode_service_response`；**不用** `src/cli/*`。

**落地手法是 CMake 层面的**——**物理上拿不到** persistence / AIM / driver 符号，
再加一条**负例测试**证明二进制不含这些符号。这与 `myrd_assert_execution_boundary` 同一手法，
比"约定不要调用"强得多。

**怎么验**：开发机即可（负例测试 + 符号检查）。

---

### T3-02 — 托盘 + 主窗口 + 磁盘列表 + 内存列

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T3-01 |
| **工作量** | M |
| **目标** | G1 |

**做什么**：托盘 + 主窗口 + 磁盘列表 + 内存列。

**两条硬规矩**（设计 §10.3）：

1. **`aggregate` 才是 verdict**。单读 `runtime` 会看到记录值而可能骗人，
   且 synthesized snapshot 的 `confirmed_generation` 是 0（没有 "unknown" 编码可用）。
   GUI 的列与颜色**以 `aggregate` 为准**。
2. **`ManagedProfileSnapshot` 不得直接进 UI 层**（含 `record.bundle_path`）。只消费 wire 投影。

**怎么验**：**虚拟机**（多盘正确显示）。

---

### T3-03 — 新建 / 导入 / 启动 / 停止 / checkpoint + 进度

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T3-02 |
| **工作量** | L |
| **目标** | G1 |

**做什么**：五个变更动作 + 进度；**长操作期间界面不假死**。
手法（设计 §10.5）：单一后台管道线程 + 慢轮询（1–2 s）+ `OperationWait` 阻塞等待；
**UI 线程永不碰 pipe**；"服务忙"渲染成正常状态而不是错误弹窗。**依赖 T1-04 的读/写分离。**

**怎么验**：**虚拟机**验长操作期间不假死、进度与终态一致。

---

### T3-04 — 权限与错误呈现

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T3-03 |
| **工作量** | M |
| **目标** | G1 |

**做什么**：未提权只读可用、变更引导提权（退出码 4 = 权限）；`force` /
`accept_volatile_data_loss` / `confirmation_id` **三条确认流分别设计，不能共用一个 MessageBox**。

**怎么验**：**虚拟机**验未提权只读 + 变更按钮禁用 + 提升入口。

---

### T3-05 — GUI 薄客户端工作流验收

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T3-04 |
| **工作量** | M |
| **目标** | **G1 验收** |

**做什么**：数据面资格通过之后的独立薄客户端工作流验收。

---

## 8. T4 —— 产品化

### 前置：本节开始前必须清掉的遗留

1. **`m5.execution_scheduler` 在开发机上是红的**（已复核为**基线即失败**，
   `m5_execution_scheduler_tests.cpp:455`）。它落在 T1-05/T1-06 的验收面上，
   **必须在 T1-05/T1-06 期间修掉**，不能带进 T4。
2. **整套 Sweep 顺序普查**（T1-09 之前做）：T1-01 的普查用的是 Consecutive 顺序，
   按构造抓不到"跨用例干扰型"间发项（`integration.m1.shm` 就是这一类）。
   **M5-A 宣布通过之前必须做一次 Sweep 顺序的整套重复普查。**
3. **测试硬编码开发机绝对路径**（`MYRD_M3_*_FIXTURE_PATH`、`MYRD_PRODUCT_EXE_PATH`）：
   现在靠"在客机上镜像开发机路径布局"绕过。**T4-01 装到真实机器上之前必须改成从环境变量取**，
   否则安装到别的路径就全错。
4. **ABI 陷阱未根治**：项目没有可靠的头文件依赖跟踪，只靠"记得加 `--clean-first`"这条纪律。
   T4-04 的干净构建门是可接受的兜底，但**这条纪律要在 T4 期间写进构建脚本**（能自动就别靠记）。
5. **`set_terminate` 处理器从未触发**：可能意味着静态/动态 STL 混链。
   `dumpbin /directives` 对比 `myrd_storage.lib` 与测试 exe 的 `/DEFAULTLIB`，很便宜。

### T4-01 — 安装器

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T3-05 |
| **工作量** | L |
| **目标** | 可交付 |

**做什么**：打包 / SCM 注册 / **ProgramData ACL（并创建 durable 根——T1-02 的 fail-closed 就是等它）** /
`phdskmnt` 前置检查（**只检查不安装**）/ 卸载**必须安全下盘**。

**⚠ 硬门**：安装器**不得**安装、替换、升级或卸载 `phdskmnt`；不得分发 AIM 二进制
（`THIRD_PARTY.md` 的再分发边界）。

---

### T4-02 — Load Only / 单文件 VHDX / 易失盘 / 卷参数

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T1-09 |
| **工作量** | L |
| **目标** | 产品化 |

**做什么**：Load Only 模式、**导出为单文件 VHDX**（复用 compaction 的 fold writer）、
易失盘模式、卷标 / 簇大小 / 卷序列号。

**裁决记录（设计 §16 #4）**：单文件 VHDX 导出**保留**（价值高、成本中低）。

---

### T4-03 — 在线 compaction

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T4-02 |
| **工作量** | L |
| **目标** | 产品化 |

**做什么**：第二套 durable phase + reconciliation + 自动触发策略；
**journal 增长必须有界并可见**。

**⚠ 硬门**：**不得改 M3 的 bundle / journal / manifest 布局**。
新 phase 必须落在既有布局之上。

---

### T4-04 — M5-E 发布证据

| 字段 | 内容 |
|---|---|
| **状态** | TODO |
| **依赖** | T4-03 |
| **工作量** | M |
| **目标** | **项目完成** |

**做什么**：干净 Debug/Release、M4 回归、冻结哈希、证据组装/校验工具
（**负例非零退出且永不打印 PASS**）、release manifest。

---

## 9. 每个增量的固定收尾循环（不要临时决定）

```
1. 读设计对应章节 + 本文卡片          ← 不读就写，等于重新设计
2. 先写针对性测试（纯逻辑），跑绿      ← 失败先于修复
3. 实现（改头文件 ⇒ 立刻 --clean-first）
4. 开发机：纯逻辑测试全绿
5. 干净构建：0 error 0 warning
6. 虚拟机：Stage → Run（真实设备；修复类 ≥ -Repeat 20）
7. 证据写入 docs/validation/<增量>.md：现象、根因、修法、读数、量测范围（跑了几次）
8. 提交到 feature/m5-lifecycle；更新本文与 tasklist 状态
9. 下一增量
```

**证据文档的最低要求**（本项目已经因为缺这几项返工过）：

- **读数必须写明跑了几次**；
- **量测范围必须写明**（开发机还是虚拟机、单次还是重复）；
- **未验证的东西必须写出来**，不能只写通过的部分；
- 结论与代码注释不一致时，以**代码**为准并立刻改注释。

---

## 10. 什么时候必须停下来问（其余一律自主推进）

| 情形 | 为什么必须问 |
|---|---|
| **设计未决项** | 例：Primo "Mini Tools" 的定义（设计 §16 #3，未定义则**不列入范围**） |
| **验收声明变更** | 降低某个 G 的验收标准、或把某个增量拆/并，会改变"做完"的定义 |
| **需要重写某一层并重挣 M3/M4 证据** | 撞不变量 4（不改 M3 布局、不改 M4 前台行为） |
| **要违反本文 §3 的任一硬门** | 只能由人裁决 |
| **发现设计本身自相矛盾** | 以设计为准会导致错误实现时 |

**不属于"需要问"的**：常规编译错误、单步测试失败、实现细节反复、
某条路径第一次行使时掉出缺陷（**那是预期内的**，修掉并记录即可）。

---

## 11. 风险登记（按"最可能咬人"排序）

| # | 风险 | 证据/理由 | 处置 |
|---|---|---|---|
| 1 | **T1-06 被低估** | 关键路径上、`reconcile_checkpoint` 正面路径零测试、七条用例 | 给它独立的时间预算；**先写用例再写实现** |
| 2 | **新路径首跑掉缺陷**（已成规律） | 四次验证：M2 崩溃、T1-11 三层、m1.shm、SCM context | 每个增量的收尾循环里**预留修复时间**，不把"第一次就绿"当计划 |
| 3 | **跨层新增事实在中间断掉** | 已栽三次（volume identity / bundle evidence / image uuid） | T1-07 起，每加一层**单测钉住值从哪来** |
| 4 | **锁序引入死锁** | T2-01 要把一把粗锁拆成 per-profile 锁 | 锁序**写死并有测试**；并发 100 次无死锁 |
| 5 | **CMake 物理隔离被绕过** | T3-01 的 GUI 只允许链两个库 | **负例测试 + 符号检查**，不靠约定 |
| 6 | **头文件改动的 ABI 假崩溃** | 已咬两次 | 纪律 + T4 期间自动化 |
| 7 | **VM 环境污染伪装成产品缺陷** | 已咬多次（孤儿 worker、多余物理盘） | 设备类用例失败**前**先查 `Get-Process myrd-m5-worker` 与 `Get-Disk` |
| 8 | **`-Stage` 静默跳过正在运行的 exe** | T1-02 实测 | 测服务前先停服务；判成功看输出 `staged to`，不看 `$LASTEXITCODE` |
| 9 | **单次绿被当成结论** | `unit.m2.backing_generation` 在开发机 20/20 绿、VM 上 ~50% 崩 | 修复类结论**至少 20 次** |

---

## 12. 明确的非目标（不要重新提起）

来自设计 §12，理由留在设计里。摘要：跨服务重启不中断磁盘、驱动分发/安装、
页面文件支持、Direct-IO 呈现模式、压缩镜像/改基准格式、VSS、远程管理、加密、卷扩容、
混合盘、快照回滚到更早 generation、多语言 UI、任何新的 bundle/profile/checkpoint/journal 格式。

---

## 13. 进度追踪

状态只改这里（并同步 tasklist）：

| 增量 | 状态 | 证据 |
|---|---|---|
| T1-01 | ✅ DONE | `docs/validation/m5-current-state.md` |
| T1-02 | ✅ DONE | `docs/validation/m5-t1-02-service-skeleton.md`（+ `-service-host.md`） |
| T1-11 | ✅ DONE | `docs/validation/m5-t1-11-stop-write-freeze.md` |
| T1-03 | **IN_PROGRESS** | part 1–9：逻辑 `m5.worker_sweep_policy` 25/25、`m5.worker_binding_store` 10/10、`m5.worker_sweep` **14/14**、`m5.service_host` 14/14（开发机）；VM `m5.startup_sweep` **`-Repeat 20` 20/20**（part 8）、`-Repeat 5` 5/5（part 9，请求 `df1f78bc01114221b2a02db96363d1a5`）。证据 `docs/validation/m5-t1-03-worker-job.md`。**剩余：把 bundle file identity 持久化（否则回收永远"无法证明"）+ 设备/盘符强杀验收**。part 9 修掉了"决策没有消费者"——`reclaimable_resources` 此前在 `src/` **零调用点** |
| T1-04 | TODO | — |
| T1-05 | TODO | — |
| T1-06 | TODO | — |
| T1-07 | TODO | — |
| T1-08 | TODO | — |
| T1-09 | TODO | — |
| T1-10 | TODO | — |
| T2-01 | TODO | — |
| T2-02 | TODO | — |
| T2-03 | TODO | — |
| T3-01 | TODO | — |
| T3-02 | TODO | — |
| T3-03 | TODO | — |
| T3-04 | TODO | — |
| T3-05 | TODO | — |
| T4-01 | TODO | — |
| T4-02 | TODO | — |
| T4-03 | TODO | — |
| T4-04 | TODO | — |

**已完成增量**：3 / 23。

---

## 14. 需要你现在裁决的事项

| # | 事项 | 我的建议 |
|---|---|---|
| 1 | **Primo "Mini Tools" 的定义** | 未定义 ⇒ **不列入范围**。定义清楚了才发现它可能是"加几个 CLI 子命令"这种小事，或者是一整块新功能 |
| 2 | **T4-02 单文件 VHDX 导出** | 设计 §16 #4 已裁决**保留**，我按保留执行。若你要改，现在说 |
| 3 | **完成口径** | 我按"G1–G4 全通过 + T4-04 manifest 冻结"当项目完成。若你希望更早收（例如只到 G2+G3，GUI 后置），现在说，因为那会**改变关键路径和 T3 的取舍** |
| 4 | **VM 是否长期可用** | T1-03 起到 T4 每一步都依赖它。若它有时不可用，我需要把"可开发机完成的纯逻辑部分"和"必须上 VM 的验收"分开排，避免再次出现"对着污染状态测半天" |
