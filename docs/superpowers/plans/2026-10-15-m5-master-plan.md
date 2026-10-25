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

**项目完成的定义**（**2026-10-17 用户裁定修改**）：

> **G1 + G2 + G3 全部验收通过 + T4-04 的 release manifest 冻结 + §8 遗留项清单清空**

**G4（多盘隔离）正式延后**，不在本次完成定义内。变更理由与恢复条件见 AGENTS.md §2.1，
摘要：G4 的义务不只是"多跑几块盘"，而是"必须同时多盘并消歧"——消歧要求确认卷落在我们的盘上，
实现选择了打开每个候选盘去问盘号，在设备退役窗口里会阻塞约 11 秒
（实测 11017 ms，证据 `docs/validation/m5-t1-03-worker-job.md` §4.15.57）。

**因此增量总数由 23 改为 20**：T2-01 / T2-02 / T2-03 移出必达，T1/T3/T4 不变。

---

## 2. 当前位置

**本节不再记录状态。** 唯一的状态来源是
[`2026-10-14-m5-v3-tasklist.md`](2026-10-14-m5-v3-tasklist.md)（每个增量一行），
"现在在哪、下一步做什么"在 [`docs/operations/m5-handoff.md`](../../operations/m5-handoff.md)。
本节只保留**基线读数**——它们改得很慢，而且是"还剩多少未知"的诚实答案。

**基线读数（客机）**：

| 读数 | 值 | 说明 |
|---|---|---|
| 完整套件 | **152 条 → 150 通过** | 唯二失败是客机无 Visual Studio 的两条 structural（`vswhere.exe not found`），非产品缺陷 |
| M0–M4 单独 | **134 项 × 3 = 399/402** | 失败集合三遍完全相同 ⇒ **M0–M4 无间歇性** |
| `m5.production_execution_backend` | `-Repeat 20 → 20/20` | 产品面在客机上全绿 |
| `structural.m5.read_write_separation` | `-Repeat 3 → 3/3` | 守卫用例 |

**已行使 vs 未行使**：已跑过的路径包括 worker 启停与握手、create_bundle、checkpoint 两阶段、
stop（含冻结）、job object 与崩溃回收、启动 sweep、读/写分离、public pipe + CLI 的
create / list / show / 读写 / checkpoint。**尚未跑过**：CLI 的 stop → 重启恢复 → forget、
中断提交判定、内存可见性与 UNMAP 端到端、suspend / 内存压力 checkpoint、断电恢复、GUI。

**经验规律（四次验证过）**：每首次行使一条新路径，掉出 1–2 个真缺陷。
**因此 T1-05 → T4-04 期间仍应预期若干缺陷**，这不是悲观，是已经量测到的命中率。

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
T1-01 ✅ ─→ T1-02 ✅ ─┬─→ T1-03 ✅ ─→ T1-06 ─┐
                      ├─→ T1-05 ◐ ─→ T1-04 ✅ ┼─→ T1-09 ─→ T1-10
T1-11 ✅ ────────────┘        │         │
                              └→ T1-07 ─┤
                              └→ T1-08 ─┘
T1-09 ─→ T3-01 ─→ T3-02 ─→ T3-03 ─→ T3-04 ─→ T3-05 ─→ T4-01
T1-09 ─→ T4-02 ─→ T4-03 ─→ T4-04
```

`T2-01 ─→ T2-02 ─→ T2-03` **⏸ 整章延后（2026-10-17 裁定）**，因此不出现在图中。

**关键路径**：`T1-05 → T1-06 → T1-09 → T3-01…T3-05 → T4-01 → … → T4-04`。
`T1-05` 是当前唯一未收尾的单盘增量；**`T1-06` 是关键路径上最容易低估的一步**
（设计 §14.3 有七条用例，且现在 `reconcile_checkpoint` 正面路径零测试）。

**T3 的依赖已改（2026-10-25）**：原文 T3-01 依赖 `T2-02`。G4 整章延后而 G1 仍在完成口径内
⇒ T3-01 改依赖 `T1-09`。这是从既有裁定推导的，不是新裁定；恢复 G4 时不必回改
（多盘只是 GUI 的一个视图，不是前置）。

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
| **状态** | **✅ PASSED**（part 1–96；客机读数与证据见 §13 与 `docs/validation/m5-t1-03-worker-job.md`） |
| **依赖** | T1-02 |
| **工作量** | L |
| **目标** | G3、G4 |

**已完成。** 这是"新路径首跑必掉缺陷"这条规律最典型的一次：决策核心、job object、
启动 sweep、残留回收四层，每一层第一次拿真东西跑都掉出了产品缺陷。

三条最终成立的性质（客机读数见 §13）：

1. **pid 不是身份** —— creation time 不匹配即判 `pid_recycled`；身份不完整 ⇒ **不回收**
   （`nullopt`）；"判不了"不是许可（`indeterminate`）。
2. **命名 job `Global\MyRamDisk.M5.Workers`，不设 `KILL_ON_JOB_CLOSE`** —— 设了等于
   "服务死则 worker 死"，worker 跑不到有序拆卸，会留下设备与盘符。强制终止**按进程**
   （`TerminateProcess`），绝不 `TerminateJobObject` 连坐。
3. **reclaim 只按"已证明的身份"动手** —— 卷 GUID + 盘符 + bundle 身份；无法证明归属时
   **不清理**、报 `needs-attention`；按进程名 / AIM 编号 / 显示名清理是**禁止**的。

三个代价最高的认知（细节见 `m5-lessons.md`）：

- 设计里从未设计"如何找到那块物理盘"（`SetupDi` / `PhysicalDrive` 在设计文档中出现 0 次），
  实现自选的"枚举 + 打开候选盘"在设备退役窗口内**阻塞约 11 秒**（实测 11017 ms）。
  最终按裁定 (a) 改为 **attach-only 不取盘号**，卷身份改用 durable GUID + 挂载优先。
- **create 会"采纳"另一个 profile 的同尺寸设备**（采纳路径跳过 backing name 校验，其正当性
  论证只覆盖本 bundle 的并发持有者）；六个"环境里有什么"的假设全部无效，真因是
  **代码走了另一条分支**。
- 一份**与源码不匹配的二进制**造出过"枚举退化 / 客机越跑越糟"的结论，白追了几轮。

完整过程、每个假设的读数与推翻方式见
`docs/validation/m5-t1-03-worker-job.md` 与 `docs/validation/m5-resolved-create-disk-adoption.md`。
**那两份是证据记录，不是现状描述。**

---

### T1-04 — 读/写分离

| 字段 | 内容 |
|---|---|
| **状态** | **✅ PASSED**（客机 `integration.m5.cli_e2e`：60/60 采样落在 create 进行中，最大时延 111 / 35 / 26 ms；证据 `m5-t1-05-cli.md` §5.1） |
| **依赖** | T1-02（实现）、T1-05（验收需要 `disk list`） |
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
| **状态** | **◐ PARTIAL**（create / list / show / 读写 / checkpoint 已在客机跑通；只剩 `stop` 完成判定 → 重启恢复 → `forget`） |
| **依赖** | T1-02 |
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

## 6. T2 —— 多盘（G4）—— **⏸ 本章整章延后（2026-10-17 用户裁定）**

**T2-01 / T2-02 / T2-03 不计入必达增量。** 本章内容保留以便将来恢复，但**现在不执行**。

**恢复前置条件**（将来做多盘时必须先满足）：

1. **不再依赖"打开候选盘取盘号"**——即先完成 §8 里那条设计决定：
   卷↔盘的对应要么用卷侧 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`（已能给出盘号），
   要么用不打开设备的属性查询；**不允许**再让拆卸路径去逐个 `CreateFileW` 磁盘设备。
2. **考虑 `DiskRuntime` 的职责拆分**（锁 / 生命周期 / 模式分发）——
   本次延后时它已被 4 处"曾经的真实缺陷"打磨过，多盘会再加义务。
3. **单盘路径必须先稳定**（T1 收尾 + G2/G3 验收通过），多盘不得建立在未稳的单盘之上。

**本次范围内保留的多盘"接入接口"**（不拆，因为成本已付且它们不是消歧机器）：
per-profile worker、按 profile 键的存储、命名 job object、worker 身份持久化（指纹）。

**本次范围内要移除的"消歧机器"**（见 §8 item 9）：
`ProductionRuntimeAdapter::find()` 里那句 `aim_disk_number()` 及其盘号过滤依赖。

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
| **依赖** | T1-09（原文 T2-02；G4 延后后由此推导，见 §4） |
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

1. **`m5.execution_scheduler`** —— **✅ 已关闭**（part 63 复核绿；part 98 全量客机再确认通过）。
   **它不再是 T1-05/T1-06 的前置。**

   **读数（明确写明次数与范围）**：直接运行测试 exe **连续 8 次，每次都是 9/9 passed**；
   用 **ctest 单独跑也是 Passed**；在 `-j 8` 的整套运行里同样通过。
   ⇒ **原先"基线即失败"的记录已过时**，**不能再据此认为它落在 T1-05/T1-06 的验收面上挡路**。

   **为什么当初是红的（本轮查明的解释，而不是猜测）**：
   该套件在**开发机**上曾于 `m5_execution_scheduler_tests.cpp:455` 失败，
   而**本轮在干净重编后无法复现**。最可能的原因是**当时那个二进制是陈旧/半链接的产物**——
   本项目本来就有一条明确纪律：**头文件改动后必须 `--clean-first`**，
   否则增量构建会留下不一致的 TU。**这与 §8 第 4 项（ABI 陷阱）是同一个根因**，
   也解释了为什么它"时红时绿"。

   **顺带核对（同轮）**：开发机上其余失败项**全部可解释**，没有一项是新缺陷：

   | 套件 | 开发机失败原因 |
   |---|---|
   | `integration.shm` / `integration.m2.shm` | 既有（§8 第 2 项记的跨用例干扰 + 命名管道限制） |
   | `unit.m2.windows_acceptance` | 既有环境用例（早前已基线核实） |
   | `unit.m4.profile_store` | 既有环境用例（早前已基线核实） |
   | `m5.service_operation_store` | **需要提权**：用例用 SYSTEM+Administrators 的 DACL 调 `CreateDirectoryW`；**本开发机 `IsInRole(Administrator)` = False**，故必然失败。**客机上由 scheduled task 以最高权限运行**（harness 注释里写明"SSH 是网络登录、不提权"），故它属于"客机可测"。 |
   | `m5.production_execution_backend` / `m5.startup_sweep` | 客机专用（开发机由守卫提前退出） |
2. **整套 Sweep 顺序普查**（T1-09 之前做）：T1-01 的普查用的是 Consecutive 顺序，
   按构造抓不到"跨用例干扰型"间发项（`integration.m1.shm` 就是这一类）。
   **M5-A 宣布通过之前必须做一次 Sweep 顺序的整套重复普查。**
3. **测试硬编码开发机绝对路径**（`MYRD_M3_*_FIXTURE_PATH`、`MYRD_PRODUCT_EXE_PATH`）——
   **本轮（part 62）已核实，此项比计划设想的轻得多，且不是 T4-01 的前置**：

   **核实结果**：
   - **`MYRD_PRODUCT_EXE_PATH` 不是硬编码**：它在 `tests/CMakeLists.txt:299` 定义为
     **`$<TARGET_FILE:myrd>`**，即**生成器表达式**，**会跟着构建位置走**。
     计划担心的"装到别的路径就全错"**对产品不成立**——
     **产品二进制从不使用这个宏**（它只出现在测试目标里）。
   - **`MYRD_M3_*_FIXTURE_PATH` 也不是字面硬编码**：它们由
     **`${CMAKE_SOURCE_DIR}/tests/fixtures/m3/...`** 在 **配置期**展开成绝对路径，
     指向**源码树里的测试夹具**。**它们是测试夹具路径，不是产品路径**；
     **产品安装不会带上它们**（除非把测试也装出去）。
   - **客机侧已被正确处理**：`tools/m5vm/Invoke-M5Vm.ps1:255` 起有一段明确说明——
     **staged 树刻意镜像开发机布局**，理由正是"多个测试在配置期把绝对路径烘进二进制，
     而二进制的常量无法在送往客机时重写"。**harness 文档里也已记下
     "改写路径并给测试打补丁"是更好的长期修法**。

   **结论**：**T4-01 的安装器不受此项影响**（产品不依赖这些宏）。
   **真正受影响的只有"把测试二进制拿去别的布局运行"这一种场景**，
   而 harness 已用镜像布局覆盖。
   **故此项从"T4-01 之前必须改"降级为"改进项"**：
   若要修，正确做法是 harness 文档里已写的那个——
   **让测试从环境变量/相对路径取夹具，而不是配置期烘入绝对路径**。
   **在 T4 之前不必做**；但**若将来要在任意机器上跑测试，则必须先做**。
4. **ABI 陷阱未根治** —— **✅ 已关闭**（part 64 用机制修掉：`CMAKE_DEPENDS_USE_COMPILER TRUE`，
   改为由 MSVC `/showIncludes` 提供依赖，不再依赖"记得加 `--clean-first`"这条纪律）。
   原先只靠"记得加 `--clean-first`"这条纪律。
   **已实施**：在 `CMakeLists.txt` 里设 **`CMAKE_DEPENDS_USE_COMPILER TRUE`**
   （位置在 `project()` 之后、配置之前），让 **MSVC 输出 `/showIncludes`、
   CMake 据此记录每个 obj 实际包含了什么**，**构建图由构造保证正确**。

   **实测（不是推断）**：干净构建后，`touch src/common/diagnostic_trace.h` 再增量构建，
   **重编了 133 个目标**，其中包括
   `m3_windows_acceptance`、`m5_worker_runtime`、`windows_volume`、`m5_worker_main`——
   **而在缺这个设置时，只会重编该头文件自己的那一个 TU**，
   其余按旧头文件继续用，这正是此前那个"不是编译错误、而是偶发崩溃/偶发测试红"的来源。
   **干净构建 0 error 0 warning。**

   **因此"记得 `--clean-first`"不再是承重纪律**（头文件改动后仍可安全地全量构建，
   但**不再必需**）。**§8 第 1 项（`execution_scheduler` 时红时绿）已查明同源**，一并受益。
5. **`set_terminate` 处理器从未触发** —— **本轮（part 62）已查，不是静态/动态 STL 混链，此项可关**。
   计划里原来建议的命令是 `dumpbin /directives` 对比 `.lib` 与 exe 的 `/DEFAULTLIB`；
   **实测发现那条命令对 exe 无效**（**它的 `/directives` 不产出任何 `DEFAULTLIB`**，
   因为该开关面向 `.obj`/`.lib`，不是已链接的映像）。**正确的查法是 `/dependents`。**

   **读数（干净且一致）**：
   - `myrd_storage.lib` 的 `/DEFAULTLIB`：`msvcprt` / `MSVCRT` / `OLDNAMES` / `uuid.lib`
     —— **动态 STL（`msvcprt`）**；
   - 测试 exe 的 `/dependents`：**`MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`**
     —— **同样动态**；
   - **全构建树的 `*.lib` 中没有任何一个引入 `LIBCMT` 或 `libcpmt`**（即静态 STL）；
   - 抽查三个测试 exe **全部只依赖 `MSVCP140.dll`**。

   ⇒ **没有静态/动态 STL 混链**，"从未触发"因此**不是**混链的症状，**第 5 项可以关掉**。
   **剩下的解释是良性的**：测试进程**根本就没有走到 `std::terminate`**
   （常见形状的测试跑在断言上，而断言走的是框架的失败路径，不是未处理异常/纯虚调用）。
   若要把这条**坐实**，只需一个**故意触发 terminate 的用例**
   （例如投掷一个逃出 `main` 的异常）并断言处理器确实被调用——
   **这是"证明处理器接线正确"，而不是"解释它为何没响"**，故列为可选，不再作为 T4 前置。
6. **`m5.production_execution_backend` 整套在客机上不稳定** —— **✅ 已关闭（part 95–96）**。
   **根因不是"不稳定"，是一个确定的功能缺陷**：create 路径会"采纳"另一个 profile 的同尺寸设备
   （采纳时跳过 backing name 校验，其正当性论证只覆盖本 bundle 的并发持有者）。
   **修复后 `-Repeat 20` → 20 / 20 全绿**（请求 `826c5f3567c14e13ba9a7ff40f8cf621`），
   全量 152 条 → **150 通过**，唯二失败是客机缺 Visual Studio 的两条结构性测试。
   **⇒ 原先"间歇性/客机退化"这两个说法都不成立，本条按"已解决"关闭。**
   完整排查链（六个被实测推翻的假设）见
   [`docs/validation/m5-resolved-create-disk-adoption.md`](../../validation/m5-resolved-create-disk-adoption.md)。
   **下面是 part 75 当时的更正记录，保留作为过程证据：**

   **撤回的理由（有读数支撑）**：在**同一条命令、同一台客机**上：

   | 读数 | 次 | 逐次通过条数 | 失败原因 |
   |---|---|---|---|
   | `270d458a`（重启客机后） | 2 | **16 / 16** | `provision_new_volume` 21001 |
   | `23f2679f`（逐步打点版） | 2 | **16 / 16** | 同上 |
   | `95c40c81`（回退后那一次） | 5 | 3 / 3 / 3 / 3 / 3 | **枚举界超时** |

   **16 条 PASS 在重启前后、打点前后都可复现**；而逐步打点的 trace 显示
   **枚举每一步都返回、单次约 14 ms、`turn` 从未超过 1、也没有任何 `bound fired`**。
   ⇒ **"枚举在负载下阻塞"与"客机随运行次数退化"都不成立**
   （后者的唯一依据是那个不可信的 3 条读数）。

   **唯一稳定的失败是**：
   ```
   create failed: op=provision_new_volume msg=provisioning helper failed native=1 code=21001
   FAIL ...m5_production_execution_backend_tests.cpp:1248: created.ok()
   ```
   **即第 17 条用例建卷失败，原因是 PowerShell provisioning helper 失败**——
   **与枚举、reclaim 均无关**。**这才是本项要查的东西**（下一轮第一件事）。

   **教训（已记入 handoff）**：**"陈旧/不匹配的二进制"是本项目反复踩的坑**
   （`Move-Item` 保留旧时间戳使 CMake 跳过重编；part 64 因此引入
   `CMAKE_DEPENDS_USE_COMPILER`）。**本轮它第四次现身，并导致我连续几轮追错了根因。**
   **任何"退化""偶发""阻塞"类结论，都必须先确认客机上跑的二进制与当前源码一致。**

8. **`enumerate_disk_candidates()` 无界** —— **⚠ 本条在 part 75 被大幅更正。**

   **仍然成立的部分**：它**确实没有自己的界**，由 part 58 在调用侧加了界
   （detached 线程 + 10 s），part 60 把界下沉到函数本身，使 M1/M2/POC 一并受益。
   **这部分保留**（是合理的防御）。

   **已撤回的部分**：**它并不是 T1-03 那个失败的根因**。
   逐步打点显示**每一次枚举都很快（~14 ms）且每个步骤都返回**，
   等待循环**从未超过 `turn 1`**，trace 里**没有任何 `bound fired`**。
   同一条命令在重启前后、打点前后都能稳定跑到 **16/18**。
   ⇒ **"枚举阻塞导致 reclaim 卡住"这个因果链不成立。**

   **那为什么保留界**：它仍是正确的防御（一个无界调用不该存在），
   且它让**任何**调用者都不可能被挂死。**但不要把它当成本增量失败的答案。**
7. **测试框架不支持按用例筛选**（T1-03 收尾时发现）：`m5vm` 的 `-Filter` 匹配的是
   `tests.json` 里的**可执行文件名**（如 `m5.production_execution_backend`），
   **不是用例名**；因此"单跑一条用例"只能靠**源码级隔离**（把其它用例包进 `#if 0`）。
   **做隔离实验时务必 `touch` 还原后的文件**，否则 CMake 会因时间戳未变而**静默跳过重编**
   （已踩过：会把"隔离版的旧二进制"误读成"完整套件通过"）。
   **可靠的判据是耗时**：完整套件约 **50–75 秒**，被裁剪过的约 **10 秒**。
9. **`m5.production_execution_backend` 第 17 条在建卷时失败** —— **✅ 已解决（part 95）**。
   完整记录见 [`docs/validation/m5-resolved-create-disk-adoption.md`](../../../validation/m5-resolved-create-disk-adoption.md)，
   其中保留**六个被实测排除的假设**与"六个方向为什么全错"。
   **根因**：**失败的那次 create 走的是"采纳"路径，采纳了另一个 profile 的设备**。
   采纳的匹配在 `adopting_existing` 为真时**跳过了 backing name 比较，只比大小与扇区大小**；
   其正当性论证是"本运行时持有 bundle 的独占写锁"，
   **而这句话只覆盖"本 bundle 的并发持有者"，覆盖不了"另一个 bundle 的持有者"** ——
   两个 profile 的盘都是 64 MiB，于是 B 采纳了 A 的设备，随后定位到 A 的盘（已格式化，非空）。
   **修复**：**create 路径不再采纳**（创建就是造新设备；本 bundle 的残留归 sweep/reclaim 管），
   **attach-only 路径仍然采纳**（`recover`/`reclaim` 必须找到已有设备）。
   **读数**：`-Repeat 3` → **3/3 全绿、无一条 FAIL**；`-Repeat 20` 结果见证据文档。
   **教训**：**"这次操作走了哪条分支"是最便宜也最常被跳过的一问**——
   六个假设都在追问"环境里有什么"，而答案是"代码走了另一条路"。
10. **改 `src/run/provision_volume.ps1` 后必须先在开发机跑 `unit.m4.provision_script`** ——
   它是那个脚本唯一守卫（AST 白名单，只允许固定的存储命令集），
   而且不需要真实设备。**已因漏跑而红过两次**（`ForEach-Object`、`New-Object` 都不在白名单里）。
   另：该脚本通过 `src/run/run_resources.rc` 以 `RCDATA` 嵌入，
   **改 `.ps1` 不会触发重编，必须 `touch` 那个 `.rc`**，否则客机上跑的还是旧脚本。

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

状态只改 tasklist；下表是同步副本，只用于从本文件一眼看到顺序与完成度。

| 增量 | 状态 | 证据 |
|---|---|---|
| T1-01 | ✅ PASSED | 144/146 + 稳定性普查 718/730（`docs/validation/m5-m0-m4-vm-baseline.md`） |
| T1-02 | ✅ PASSED | `docs/validation/m5-t1-02-service-skeleton.md`（+ `-service-host.md`） |
| T1-03 | ✅ PASSED | `docs/validation/m5-t1-03-worker-job.md`、`m5-resolved-create-disk-adoption.md` |
| T1-04 | ✅ PASSED | `docs/validation/m5-t1-05-cli.md` §5.1（60/60 采样，111 / 35 / 26 ms） |
| T1-05 | ◐ PARTIAL | `docs/validation/m5-t1-05-cli.md` §10.2（create / checkpoint 通，stop 卡在完成判定） |
| T1-11 | ✅ PASSED | `docs/validation/m5-t1-11-stop-write-freeze.md`（20/20） |
| T1-06 … T1-10 | TODO | 见 tasklist |
| T2-01 … T2-03 | ⏸ 延后（G4 暂不做） | 2026-10-17 裁定 |
| T3-01 … T3-05 | TODO | — |
| T4-01 … T4-04 | TODO | — |

**已完成 5 / 20**（另 T1-05 部分完成）。20 = T1(11) + T3(5) + T4(4)；T2 三项延后不计。

---

## 14. 需要你现在裁决的事项

| # | 事项 | 我的建议 |
|---|---|---|
| 1 | **Primo "Mini Tools" 的定义** | 未定义 ⇒ **不列入范围**。定义清楚了才发现它可能是"加几个 CLI 子命令"这种小事，或者是一整块新功能 |
| 2 | **T4-02 单文件 VHDX 导出** | 设计 §16 #4 已裁决**保留**，我按保留执行。若你要改，现在说 |
| 3 | ~~完成口径~~ | **已于 2026-10-17 裁定**：G1 + G2 + G3 全部验收通过 + T4-04 manifest 冻结 + §8 遗留清空；G4 延后。不再询问 |
| 4 | **VM 是否长期可用** | T1-03 起到 T4 每一步都依赖它。若它有时不可用，我需要把"可开发机完成的纯逻辑部分"和"必须上 VM 的验收"分开排，避免再次出现"对着污染状态测半天" |
