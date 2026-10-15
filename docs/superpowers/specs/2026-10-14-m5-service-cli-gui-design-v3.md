# MyRamDisk M5 生命周期管理设计 v3（服务 + CLI + GUI）

状态：待评审
日期：2026-10-14
基线：`main` = `cb3df08`（干净 M4）；M5 实现在 `feature/m5-lifecycle`
取代：`2026-09-30-m5-lifecycle-gui-design.md`（v2）
配套：`docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`、`docs/validation/m5-transfer-manifest.md`

---

## 0. v3 相对 v2 的变更摘要

这是本文档的审阅入口。v3 不是 v2 的润色，是**一次目标重述 + 一次范围收缩 + 三条必需新增**。

| 变更 | 类型 | 理由 |
|---|---|---|
| 目标重述为 G1–G4（见 §1） | 重述 | v2 的目标是"把 M4 包成服务"，缺一条产品语义；本次按用户陈述的目标重写 |
| **移除**：worker 跨服务重启生存（v2 隐含、且 `feature/m5-unified-integration` 实现过） | 范围收缩 | G1–G4 无一条需要它（§3）。它要先付出通道反转 + 运行时租约 + `session_generation` 迁移 + P4/P5 fence 重锚 + 重连循环，是本项目最大的一笔新增 |
| **移除**：未托管期 checkpoint 策略 | 范围收缩 | 是上一条的推论；worker 不跨服务生存，就不存在"服务不在而 worker 在"的窗口 |
| **新版 §3.6 恢复表**：删去"原 worker 可重连"两行 | 修正 | 与裁决冲突；v2 保留该行但机制已被删，tasklist 已自行标注"结构上不可能" |
| **新增**：中断提交判定必须可达（§4.3） | 必需 | G3 直接要求。断电可能落在 P4/P5 之间，此时 `start` 被状态机拒绝；判定入口现在是 `attach`，而它恒为空 |
| **新增**：进程容器与孤儿回收（§5） | 必需 | 用户要求；且"不留残余"是 M4 验收基线 |
| **新增**：per-profile 串行 + 读/写分离（§6） | 必需 | G4 要求"独立磁盘与单盘行为一致"；当前全局 gate 让 A 的长操作阻塞 B 与所有读 |
| **新增**：内存可见性（§7） | 必需 | 承诺"内存可控"的信任基础；GUI 的核心列 |
| **明示非目标**：驱动分发、页面文件、Direct-IO、压缩镜像（§12） | 明确 | 两项架构不可为、一项既有裁决、一项撞冻结格式；不写下来一定会被重新提 |
| 保留不动 | — | 决策 A（worker 复用 `DiskRuntime`）、`myrd_assert_execution_boundary` 物理边界、两阶段 P4/P5、M3/M4 磁盘格式、fail-closed 原则 |

**一个顺带的好消息**：移除 worker 生存**修好了**审计发现的一个缺陷。v2 的 P4/P5 规则写的是"只验证同一 authenticated worker session"，而 `session_generation` 由服务进程铸造（`WindowsWorkerSessionProvider::next_session_generation_{1}`，每实例重置）。在 worker 随服务而死的世界里，"worker session ≡ 服务 session"，这条规则成立；一旦 worker 活过服务，该字段归属就错了。**v3 的裁决让这条规则重新成立**，不需要迁移。

---

## 1. 目标与验收标准

用户陈述的四个目标（原文）：

> M5 能够实现 GUI 界面，管理单盘/多盘；M5 最少能够实现单盘的控制管理；能够实现断电自动恢复；实现多盘的时候，要求多盘无冲突，独立磁盘和单盘行为一致。

拆成可验收的目标：

### G1 — GUI 管理单盘/多盘

**验收**：一个 Win32 薄客户端，能列出全部盘（名称、盘符、状态、generation、**内存占用**），能新建/导入/启动/停止/checkpoint，能看到操作进度与失败原因；**在任一盘执行长操作期间，界面不假死**；未提权时能以只读方式工作并明确提示。

### G2 — 最少实现单盘的控制管理

**验收**：一个盘可端到端完成 注册 → 启动 → 读写 → checkpoint → 停止 → 重启恢复 → 移除，全部通过 CLI；只有 confirmed committed 才报成功；失败分类稳定（退出码 0–5）。

### G3 — 断电自动恢复

**验收**：VMware 强制断电后重新开机，所有 `auto_start=true` 的盘**无需人工介入**各自恢复到自身最后一个完整已提交 generation；**包括断电正好发生在 checkpoint 提交边界（P4/P5 之间）的情况**；恢复过程中绝不重新分区/格式化；无法判定时明确报 needs-attention 而不是猜。

### G4 — 多盘无冲突，独立磁盘与单盘行为一致

**验收**：N 个盘同时运行，A 在 create/checkpoint/stop/移除/关机任一环节失败时，B 的 generation、挂载点、worker、operation gate、writer lock **全部不变**；且 **A 的长操作不得让 B 的操作排队或超时**（延迟也是行为）；每个盘在系统中的表现与它单独存在时相同。

> 延迟隔离是 G4 的一部分，不是 G1 的附加要求。理由是用户的措辞是"行为一致"，而"B 要等 A 三十秒"就是行为不一致。

---

## 2. 基线与不变量

**基线**：`main` = `cb3df08`（干净 M4）。M5 在 `feature/m5-lifecycle` 上实现。

**不变量**（违反即为缺陷）：

1. **决策 A**：worker 只驱动 M4 的 `DiskRuntime`，不重走 `HostCore → M3`。两阶段 checkpoint 是 adapter 之上唯一新增的执行侧能力。
2. **单一权威**：`public pipe → ServiceCore → worker → DiskRuntime → M3 → phdskmnt` 是唯一执行链路。不新增第二套状态机、JSON IPC、profile 格式或 host 执行核心。
3. **物理边界**：`myrd_management` / `myrd_product_ipc` / `myrd_profile` / `myrd_m5_worker_protocol` 不得链接 `myrd_m4_run` / `myrd_m3_windows` / `myrd_m3_management` / `myrd_m1` / `myrd_driver` / `myrd_proxy`。由 `myrd_assert_execution_boundary` 在 CMake 层强制并有负例测试。
4. **fail-closed**：profile 损坏、资源冲突、驱动不可用、worker 身份不符、checkpoint 未确定 —— 一律拒绝。绝不自动格式化、修复为零、删除 bundle，或把"没有答案"当成 clean。
5. **不改磁盘格式**：M3 的 bundle / journal / manifest / 双槽布局，以及 M4 前台单阶段 checkpoint 的行为，均不变。
6. **`dirty_epoch` 规则**：per-worker-session 的 mutation 计数，**绝不**作为跨会话的 durable 单调时钟；"是否 dirty"以 worker 报告的 M3 块级 dirty 为准；P4/P5 只校验同一 authenticated worker session 的身份（profile id / image id / PID / process creation time / session generation），不比较 epoch 数值。
   - 由 §3 的裁决，worker session 与服务 session 同寿，本规则天然成立。
7. **不装驱动**：`phdskmnt` 只作为外部前置依赖检查，M5 不安装、替换、升级或卸载它。

---

## 3. worker 生命周期裁决

### 3.1 裁决

**worker 的生命周期绑定于服务进程。** 私有通道关闭（服务退出、被杀、崩溃）即为该 worker 生命的终点：它完成有序拆卸后退出，服务下次启动时启动新 worker。

`src/host/m5_worker_main.cpp:174-187` 已经这样实现，且注释写明了意图：

> A closed or failed channel is the end of this worker's life, not an error to retry against: the service will launch a replacement.

### 3.2 后果（必须文档化，不是缺陷）

| 事件 | 行为 |
|---|---|
| 服务正常停止 / 重启 / 升级 | 所有盘按有界预算 checkpoint 并下线；服务回来后按 `auto_start` 自动拉起。**盘会闪断**，未提交写入丢失（最多一个 checkpoint 间隔） |
| 服务崩溃 / 被杀 | worker 在通道关闭后有序拆卸并退出；下次服务启动做残余清扫（§5）后按 `auto_start` 恢复 |
| OS 关机 | 同"服务正常停止"，在 SCM preshutdown 预算内完成 |
| 机器断电 | 全部消失，走 G3 的恢复路径 |

**"service stop ≠ disk stop"不成立**：停服务就是停盘。用户界面与文档必须这样表达。

### 3.3 为什么这样裁决

1. G1–G4 无一条需要 worker 跨服务生存（断电时所有 worker 都死，恢复路径与是否生存无关）。
2. **鉴权面小得多**：worker 活过服务，就必须解决"是我的服务重启了"与"有冒充者在连我"的区分 —— 那需要持久化的运行时租约 + 服务自证握手 + 凭据轮换的两阶段原子性。这是一整个新协议面。
3. **状态简单**：重启后一切都是"已停止"，恢复路径唯一（launch + `DiskRuntime::recover`），不需要活 worker 注册表、不需要"半死不活"的中间态处理。
4. 符合 M4 验收基线（"无残留进程 / 无目标盘符 / 无虚拟盘"）。

### 3.4 保留的缝

如果将来要支持跨服务重启生存，唯一需要在今天写下的约束是：**私有协议的信封与绑定必须能承载一个重挂接握手**（即消息的认证绑定不假设"本连接建立于本进程启动时"）。这是一句设计约束，不是工作量。

---

## 4. 提交边界与恢复

### 4.1 P4/P5 两阶段（已实现，不变）

```
prepare_checkpoint  → 返回（不提交）：prior/candidate generation、dirty_epoch、
                      worker session identity、manifest slot、candidate bytes digest
commit_checkpoint   → 四类 disposition：committed / not_committed /
                      commit_indeterminate / committed_needs_reconciliation
abort_checkpoint    → 在 commit 前取消 candidate
```

`committed_needs_reconciliation` **不得**折叠成成功或普通 indeterminate；四类 disposition 必须在私有 IPC、worker 返回值、durable evidence、测试矩阵中一致出现。

### 4.2 durable phase

```
stable / admitted / commit_boundary / evidence_recorded /
committed_pending_epoch_confirmation / resolving
```

P4 = `commit_boundary`；P5 = `evidence_recorded` + `committed_pending_epoch_confirmation`。

### 4.3 中断提交判定必须可达（v3 新增，G3 的硬要求）

**问题**：断电/崩溃可能落在 P4 与 P5 之间。此时 durable phase 是 `commit_boundary` / `evidence_recorded` / `resolving`，而 `start`（及 `force_stop`/`checkpoint`/`compact`/`rebind`）在状态机里要求 `S::not_commit_indeterminate`（`state_machine.cpp:27-38`）。唯一能从该状态出去的命令是 `resolve_commit`，而它走 `reconcile_checkpoint`。

`ServiceWorkerControl::reconcile_checkpoint`（`service_worker_control.cpp:1130`）通过 `require_session(profile_id, /*attach_if_absent=*/true)` 取 worker，而 `WindowsWorkerSessionProvider::attach` 恒返回 `success(nullptr)`（`windows_worker_sessions.cpp:106`）。**结果：一次落在提交边界上的断电 + 重启，会让那块盘停在 needs-attention，起不来。**

**修法**：判定不需要"找到已经在跑的 worker"，而是需要"**一个能只读检查 bundle 的 worker**"。所以把入口从 `attach` 换成**从 durable correlation 造 launch**：

```text
ServiceCore 发现 durable phase 已过 prepare（recovering_checkpoint_from_m3 = true）
  → 从 P4 durable correlation 构造 WorkerLaunchRequest
      （bundle 路径 + image id 来自 correlation；volume_mode = inspect_only）
  → launch 一个检查用 worker（不挂卷、不分配盘符）
  → 发 recovery_proof(correlation)
  → 三态映射：
        prior_generation_proved   → 终态 not_committed，恢复 stable / 释放 gate
        candidate_generation_proved → 进入 committed-P5 路径
        ownership_unprovable      → resolving / needs-attention（保留 gate，不删不移）
  → 该 worker 结束
```

这个形状**已经在 `register_bundle` 里跑通过**（`create_bundle` / `inspect_bundle` 就是"launch 一个只检查、答完即退的 worker"），所以这是复用而不是新机制。`M3Provider::inspect_checkpoint` 本来就是 static、为"还没打开 bundle 的新 worker"设计的。

**只读要求**：检查路径不得创建 runtime、不得持有 writer transaction、不得修改 bundle。由 `m5_checkpoint_inspection_tests.cpp` 的 `writes_nothing` 用例钉住。

**三态判定的字段要求**：correlation 必须携带 image UUID、prior/candidate generation、journal native operation id、manifest slot、candidate bytes digest。缺任一 → **拒绝回答**（`recovery_not_provable`），不得当作"匹配"。零 digest 是"缺失"而不是"匹配零"。

### 4.4 新版恢复决策表（取代 v2 §3.6 的两张表）

v2 的表按"原 worker 可重连 / 原 worker 不存在"分叉；§3 的裁决使第一列**不存在**，故合并为一列。

| durable phase | 重启后动作 |
|---|---|
| `stable` | 无 |
| `admitted` | 新 worker 只读 recovery proof；未拿到 proof → 保持 `resolving` |
| `commit_boundary` | 同上（判断 manifest 是否 publish） |
| `evidence_recorded` | 见 disposition 表 |
| `committed_pending_epoch_confirmation` | 同 session 确认 epoch，或 reconcile |
| `resolving` | 继续 reconciliation，阻止 stop/remove |

`evidence_recorded` 与 `CheckpointCommitDisposition` 联合解释：

| disposition | 重启后动作 |
|---|---|
| `committed` | 推进 confirmed generation → `committed_pending_epoch_confirmation` |
| `not_committed` | 回退到 prior generation |
| `commit_indeterminate` / `committed_needs_reconciliation` | `resolving` |

**硬规则**：`admitted` / `commit_boundary` 阶段，**未拿到 recovery proof 时保持 `resolving`，禁止重放 start / checkpoint / stop**。理由：`admitted` 不保证"尚未 prepare" —— 服务可能已向 worker 发出 prepare，而 worker 已随服务死亡。关键序列（prepare 已发 → 服务在写 `commit_boundary` 前崩溃）必须按此处理，不得盲目 re-plan。

**删除的行**："原 worker 可重连 → 查询其 pending correlation"。理由：worker 随服务而死，该分支不可达。tasklist 已自行标注"结构上不可能"，v3 把它正式删掉并记录理由，而不是留着一个永远不执行的行。

**Tier 1（向活 worker 查询 pending candidate）在本期不存在**。它是"worker 跨服务生存"的产物；若将来采纳 §3.4 的缝，再连同该能力一起引入。

### 4.5 未托管模式：不存在

v2 的设计讨论里出现过"服务不在时 worker 按策略自检点"。由 §3 的裁决，该窗口不存在，故删除该设计。服务在 = 盘在；服务不在 = 盘已下线。

---

## 5. 进程容器与孤儿回收（新增）

### 5.1 目标

把"不留残余"从愿望变成**受检属性**，并让一个卡死的 worker 有界可回收 —— 而不违反单盘隔离。

### 5.2 job object 设计

| 项 | 决定 | 理由 |
|---|---|---|
| 类型 | **命名 job**（`Global\MyRamDisk.M5.Workers`） | 重启后的新服务实例要能 `OpenJobObject` 枚举上次留下的进程 |
| `KILL_ON_JOB_CLOSE` | **不设** | 设了就等于"服务死 → worker 被杀"。worker 被 `TerminateProcess` 会**跑不到有序拆卸**（dismount / 删盘符 / 停 AIM），留下设备与盘符残余 |
| 进程句柄 | **保留**（现在被 `close_launched_worker` 丢弃） | 没有句柄就没有单盘粒度的强制终止；也无法等待退出 |
| 强制终止 | **按 profile 粒度** `TerminateProcess`，**绝不** `TerminateJobObject` | G4 的单盘隔离；一个盘卡住不能连坐 |

### 5.3 协作优先 + 有界升级

```text
服务要下掉一个盘（stop / preshutdown / 强制回收）：
  1. 协作路径：发私有 shutdown 命令 → worker 有序拆卸 → 退出（首选）
  2. 有界等待：deadline 内等进程退出（用保留的句柄 WaitForSingleObject）
  3. 升级：超时 → TerminateProcess（有界等待）→ 记 needs-attention
  4. 绝不宣称成功：被杀掉的 worker 的资源由 §5.4 的清扫接手
```

### 5.4 启动清扫（sweep）

```text
服务启动：
  1. 建/开命名 job
  2. 枚举 job 内进程（JobObjectBasicProcessIdList）
  3. 对每个 PID 读 process creation time，与本服务持久化的 worker 绑定核对（防 PID 回收）
  4. 不属于本服务的 / 无对应 profile 的 → 协作终止，超时则升级
  5. 逐盘检查上次崩溃留下的设备与盘符残余（见下）
```

### 5.5 残余资源的回收需要持久化的"最后观测身份"（新增设计项）

上一进程被杀时可能留下：AIM 设备、卷、盘符。要安全回收它们，服务必须能**证明归属**。因此：

> **服务侧持久化每个 profile 最后观测到的资源身份**：bundle file identity、AIM 设备身份、卷 GUID + extents、盘符、generation。

- 来源：worker 的 status 已经携带 `WorkerBundleEvidence` + volume identity（Task 5 的 `record_volume_identity` / `register_bundle` 路径已经在生产这组事实）。
- 用途：启动 sweep 时按身份匹配并回收**只属于本 profile** 的残余。
- **无法证明归属 → 不清理**，报 `needs_attention`，保留证据等人工介入。绝不按进程名 / AIM 编号 / 显示名清理。
- 这条与 M4 的"清理只清本实例取得过的资源"是同一原则，只是把"实例"扩展到"跨服务实例的同一 profile"。

---

## 6. 并发模型：per-profile 串行 + 读/写分离（新增）

### 6.1 现状（缺陷）

四处全局串行：

| # | 位置 | 证据 |
|---|---|---|
| 1 | `serialize_pipe_handler` 一把全局 mutex 管所有命令 | `service_response_wire.h` 的 `serialize_pipe_handler(handler, gate)` |
| 2 | scheduler 在**同一把**锁里 claim + execute | `execution_scheduler.cpp:160` `std::scoped_lock lock(execution_gate_)` 包住 claim 与 `execute_claimed_operation` |
| 3 | `OperationStore` 一把 gate 管全部操作 | gate key = store 路径（`service_operation_store.cpp:32` → `operation_store.cpp:869`） |
| 4 | `ProfileRegistrationStore` 同样 | `profile_registration_store.cpp:821`（key = `managed.value().path`） |

后果：A 的一次 `disk create`（几十秒）阻塞 B 的所有命令、B 的执行、以及 B 的持久化写入。GUI 在此期间**必然假死**。

### 6.2 分两步修

**T1（读/写分离，解决 GUI 假死）**

- 变更命令取**独占** gate；查询命令（`disk list` / `show` / `operation list` / `status`）取**共享** gate，或按审计结论完全不加锁。
- 依据：`ServiceCore` 的**全部可变状态只有 `pending_mutex_` + `pending_`**（`service_core.h:1116-1125`，其余全是注入引用），且 `pending_` 已自带锁。所以读与写并发的风险集中在注入的存储上，需要逐条审计而不是一刀切。
- 验收：一个盘正在 create 时，`disk list` 在 1 秒内返回。

**T2（per-profile 键，满足 G4 的延迟隔离）**

- 存储层 gate 的键从"store 路径"改为**按 profile 键**；共享的 `request_index` 等容器另有自己的互斥。
- 组合根按 profile 分锁；`ServiceWorkerControl` 的 session 容器加锁（其头部已声明"不是并发对象"，需要它变成本身安全或由 per-profile 锁保证）。
- **锁序必须写死并测试**（profile gate → store 内部锁），避免死锁。
- 验收：A 正在 create 时，B 的 start 不被阻塞；A/B 并发 100 次无死锁。

---

## 7. 内存可见性（新增）

### 7.1 为什么是必需项

"内存可控"是最小可用 RAM 盘的第一条承诺。用户看不到内存去哪了，就不敢把工作放进去。它同时是 GUI 的核心列。

### 7.2 数据现状

| 事实 | 现状 |
|---|---|
| `dirty_bytes` / `journal_bytes` / `generation` | 已有（`DiskMaintenanceStats`，`disk_runtime.h:72-76`） |
| RAM 上限 / 软上限 / 低水位 | 已有（由 `--ram-limit` 推出 hard / soft(½) / low(¼)） |
| **常驻字节数（resident bytes）** | **没有公开访问器**。存储内部有常驻块状态（`BlockState::Unresident`、`try_decommit_resident_clean`），但没有对外汇报 |
| 删除释放（UNMAP） | 机制已在：`imdproxy_protocol.h` 的 `kFlagSupportsUnmap` + `kReqUnmap=6` → `request_processor.cpp:106/202` → `device_.unmap(...)`；`DynamicBlockStore::features()` 返回 `{false,true,true}`；释放的 extent 以 `JournalRecordType::Discard` 记入 journal |
| 检查点后回收 | 机制已在：`EvictionManager` 只驱逐"当前不可变 backing view 能重建"的块（`skipped_dirty_blocks` 明确跳过脏块）；checkpoint 后 `install_backing_view` 推进基线使其可驱逐 |
| 手动释放 | `EvictionManager::reclaim_to(target)` 已在，缺一条调用它的命令 |

### 7.3 设计

新增一条贯穿各层的事实 **`resident_bytes` / `budget_bytes`**：

```
storage   : DynamicBlockStore 暴露常驻字节计数
persistence/M3 : provider 把计数并入 maintenance stats
run       : DiskMaintenanceStats 增加字段
host      : WorkerRuntimeStatus 携带
management: ServiceResourceProjection 暴露（不得含路径）
CLI/GUI   : 显示
```

**风险提示（写进实现纪律）**：跨层新增一个事实，最容易在中间断掉 —— 本项目已经栽过三次（manifest §7 item 18 的 volume identity 无生产者、item 22 的 `BundleRegistrationEvidence` 无生产者、item 23 的 image uuid 被随机生成）。**每加一层都要有单测钉住"这个值从哪来"，而不是只问"它到哪去"。**

### 7.4 手动释放与内存压力

- 新增 `disk release-memory <profile>`：调用 `reclaim_to` 释放可回收的常驻块。低工作量。
- profile 已有 `checkpoint.on_memory_pressure{true}` 与 `on_shutdown` / `on_suspend{best_effort}` 策略字段，但**运行期没有消费者**（M4 run loop 只处理 interval 与 dirty 阈值）。本期把 memory pressure 与 suspend 接上（§9）。

---

## 8. 数据流

### 8.1 `disk create` / `disk import`

```
ServiceCore：授权 → durable 建 operation → 按 lifecycle 选 create/import
  → create：create_and_verify_bundle（从 intent 造 launch → create_bundle → 取 bundle 证据）
             → publish_provisioning_profile → create_runtime_and_worker（launch + start）
             → verify/record volume identity → flush → checkpoint（P4/P5）→ publish_ready_profile
             → assign mount → readiness
  → import：inspect_bundle（只读、不挂载）→ publish_imported_ready_profile
```

`disk import` 的 writer lease 必须覆盖"校验 → registration 发布"全程；无法证明独占 → 拒绝注册，不迁移、不改写 bundle。

### 8.2 `disk start`

启动模式由 **durable lifecycle** 决定，绝不猜 bundle 状态：`ready` → 只能 recover；服务自己刚创建、`provisioning` → 才允许 create。bundle 缺失/空、identity 不符、状态与 lifecycle 不符 → fail-closed。

### 8.3 `disk checkpoint`

`Checkpointing` → worker：flush 卷 → prepare（返回边界证据，不提交）→ ServiceCore 校验 P4 → commit → P5 同 session 确认 → 发布 checkpoint durable state → `Running`。

### 8.4 `disk stop`

```
Stopping → quiesce → flush
  → checkpoint_required：走 P4/P5 边界后再删除
  → require_clean：读 worker 块级 dirty；dirty / 不符 / 无答案 → 拒绝并 resume，绝不进入删除
  → 删盘符 → dismount → 停 AIM → Stopped
```

### 8.5 `disk remove` / `forget`

**fail-closed**：无法证明 writer lock、匹配设备、保留的 bundle identity 三者安全时**拒绝**，保留 registration 与 bundle，**绝不伪报成功**。worker replacement 不得触发 removal 或 synthetic checkpoint success。

---

## 9. 服务生命周期

| 事件 | 行为 |
|---|---|
| 服务启动 | 建/开 job → 加载 durable 状态 → §5.4 sweep → `recover_after_restart()` → 按 `auto_start` **逐盘**拉起（一盘失败不阻塞其它盘、不卸载、不清理其它盘） |
| `SERVICE_CONTROL_STOP`（手动停/重启） | 关闭变更准入 → 摘掉 public pipe → 停 scheduler → 按预算逐盘 checkpoint + 拆卸 → 退出 |
| `SERVICE_CONTROL_PRESHUTDOWN`（OS 关机） | 同上，在 SCM preshutdown 预算内执行 |
| `SERVICE_CONTROL_POWEREVENT` / `PBT_APMSUSPEND` | **新增**：按 profile 的 `on_suspend` 做有界 best-effort checkpoint |
| 内存压力 | **新增**：按 `on_memory_pressure` 触发有界 checkpoint；已在 profile 里，缺消费者 |

**预算规则**：全局 deadline（不超过 SCM 期限），逐盘按剩余预算分摊（全局剩余 ÷ 未处理盘数）。超时后：**不宣称成功**，durable operation 标 `indeterminate` / needs-attention，保留 worker 让系统终止或等其自行退出（不盲 kill）。

---

## 10. GUI（薄客户端）

### 10.1 定位

`myrd-gui`：Win32 托盘图标 + 主窗口。**只走 public pipe**，不直接碰 bundle / 卷 / 驱动。GUI 状态是**可丢弃的**；service 永远是投影、进度和变更的唯一权威。

### 10.2 复用哪一层（关键取舍）

| 层 | 用不用 | 理由 |
|---|---|---|
| `src/control`（`Command` / `ipc_frame` / `PublicPipeClient` / 鉴权） | ✅ | 类型化命令与通道，GUI 不需要解析文本 |
| `src/management/service_response_wire.h`（`decode_service_response`） | ✅ | 服务端/客户端共享编解码 |
| `src/cli/*`（`cli_parser` / `cli_renderer`） | ❌ | 文本 parse/render 是 CLI 专属；GUI 走它只多一层字符串往返 |

三个消费者共用同一个 typed seam：CLI（文本渲染）、GUI（窗口）、测试（直连）。

### 10.3 数据模型

- `ServiceProfileProjection`：`profile_id`、`lifecycle`、`runtime`、`activity`、`persistence`、`health`、**`aggregate`**、`confirmed_generation`
- `ServiceOperationProjection`：`operation_id`、`state`、`phase`、时间戳、`cancellable`、`cancel_requested`
- `mount_target`（**安全投影，永不含 bundle 路径**）
- §7 的内存三元组（resident / budget / dirty）
- `error` / `disposition` / `warnings`

两条硬规矩：

1. **`aggregate` 才是 verdict**。`ServiceCore` 的 `reported_profile_state()` 只把"worker 存活与 durable 记录矛盾"折进 `aggregate`；单读 `runtime` 会看到记录值而可能骗人，且 synthesized snapshot 的 `confirmed_generation` 是 0（没有 "unknown" 编码可用）。GUI 的列与颜色**以 `aggregate` 为准**。
2. **`ManagedProfileSnapshot` 不得直接进 UI 层**（含 `record.bundle_path` 等路径）。GUI 只消费 wire 投影。

### 10.4 功能映射

| GUI 动作 | Command |
|---|---|
| 磁盘列表 | `DiskListCommand` |
| 详情 / 配置 | `DiskShowCommand` / `DiskConfigGetCommand` |
| 新建 | `DiskCreateCommand`（name/bundle/size/mount/owner_sid/no_start/memory_mode/checkpoint_interval/dirty_threshold） |
| 导入 | `DiskImportCommand` |
| 启动 / 停止 / checkpoint | `DiskStartCommand` / `DiskStopCommand` / `DiskCheckpointCommand` |
| 开机自启开关 | `DiskConfigSetCommand{key="auto_start"}` |
| 进度 / 取消 | `OperationListCommand` / `OperationWaitCommand` / `OperationCancelCommand` |
| 释放内存 | `disk release-memory`（§7.4 新增） |
| 删除（fail-closed） | `DiskForgetCommand{profile_id, confirmation_id}` |
| 环境体检 | `DriverStatusCommand` / `DiagnosticsCollectCommand` |

**不需要为 GUI 新增公共协议命令**（`release-memory` 除外）；数据面命令集已经够。

### 10.5 三条 GUI 专属要求

1. **不假死**：单一后台管道线程 + 慢轮询（1–2 s）+ `OperationWait` 阻塞等待；UI 线程永不碰 pipe；"服务忙"渲染成正常状态而不是错误弹窗。**依赖 §6.2 T1 的读/写分离**。
2. **权限**：查询对普通本机用户开放，变更需 administrator/owner（退出码 4 = 权限）。**GUI 不以提权启动**：受限权限下能看能列，变更按钮禁用并给出提升入口。注意 public pipe 的 SDDL 带 `(D;;GA;;;NU)`，拒绝 Network Logon（纯本机产品，无影响）。
3. **危险操作的确认流**：`force` / `accept_volatile_data_loss` / `confirmation_id` 必须分别设计，不能共用一个 MessageBox。这三个是 fail-closed 语义在 UI 上的投影。

### 10.6 架构边界的可测试性

`tests/integration/m5_gui_pipe_tests.cpp` 断言 GUI 只使用 `PublicPipeClient`、打不开 bundle/卷/驱动。**落地手法是 CMake 层面的**：GUI 只链 `myrd_product_contracts` + `myrd_product_ipc` + 一个纯 client 库，**物理上拿不到** persistence / AIM / driver 符号，再加一条负例测试。这与现有 `myrd_assert_execution_boundary` 是同一个手法，比"约定不要调用"强得多。

---

## 11. 资源所有权表

| 资源 | 创建/取得 | 验证 | 清理 |
|---|---|---|---|
| bundle writer lock | M3（persistence 层，权威） | worker 重验 bundle identity | worker（M3 stop 释放） |
| M3 provider | worker | worker（geometry / identity） | worker |
| AIM 设备 | M3 provider（worker 内） | worker（设备身份 / disk number） | worker；崩溃残余由服务 sweep 按持久化身份回收（§5.5） |
| 卷 | worker（provision / find） | worker（volume GUID + extents 唯一） | worker（dismount） |
| 盘符 | worker（assign） | worker（占用检查） | worker；崩溃残余同 §5.5 |
| pending checkpoint transaction | M3（prepare） | service（P4/P5） | M3（commit/abort） |
| worker 进程 | service（launch） | service（WorkerBinding） | service（协作 → 有界升级，§5.3） |
| 运行时租约 | —— | —— | **本期不存在**（§3.4） |

**单盘故障只清理"该盘已验证拥有"的资源** —— 绝不按进程名 / AIM 编号 / 显示名清理其它盘。

---

## 12. 非目标（附理由）

本期明确不做，且理由要留在文档里，否则一定会被重新提：

| 非目标 | 理由 |
|---|---|
| **跨服务重启不中断磁盘**（worker 生存 + 重挂接） | G1–G4 无一条需要；成本最大（§3.3）。缝留在 §3.4 |
| **驱动分发 / 安装** | `THIRD_PARTY.md`：本项目不 vendor 也不 redistribute 第三方源码与二进制，AIM 用户态二进制一律不链接、不加载、不分发。README 已记录决定：**不授权卸载 AIM Toolkit、删除 `deviosvc`、替换驱动或分发独立驱动安装包**。上游 `phdskmnt` 为"AGPL-3.0 或商业授权"双轨，分发需商业授权或自研驱动 |
| **页面文件支持** | 结构不可为：我们的盘由**用户态 worker** 创建，而 Windows 内存管理器在**内核初始化早期**创建 pagefile，早于任何用户态服务。要支持需一个 boot-start 内核组件 |
| **Direct-IO 呈现模式** | 盘的呈现方式完全由 `phdskmnt` 决定，而本项目不改驱动。加第二种模式 = 改驱动或换驱动 |
| **压缩镜像 / 改基准格式** | 压缩要改 bundle/journal/manifest 格式 → 撞不变量 5，M3 全部验收作废。用户感知也不明显 |
| VSS、远程管理、加密、卷扩容、混合盘（RAM+file） | 与目标无关；放大攻击面与维护面 |
| **快照回滚到更早 generation** | 恢复规则固定为"最后一个完整已提交 generation"；多 generation 保留 + 用户可选回滚是独立设计（涉及数据保留语义与 fail-closed 对账），属 M6 级 |
| 多语言 UI | 只在 GUI 存在后才有意义；v1 不做 |
| 新的 bundle / profile / checkpoint / journal 格式 | 不变量 5 |

---

## 13. 交付阶梯与 VM 验收

**每个增量都上虚拟机验证**（用户确认）。开发机允许执行**不涉及真实物理资源**的测试——判断标准是该测试会不会创建真实 worker 进程 / AIM 代理盘 / Windows 卷（`AGENTS.md` §4）；不确定时按"只能上虚拟机"处理。

### T1 —— 单盘可用（命中 G2 + G3）

| 增量 | 内容 | VM 验收 |
|---|---|---|
| T1-01 | **VM 测试回路**：guest runner + host 侧驱动，构建在开发机、执行在 VM | 在当前 HEAD 上拿到真实的全套绿/红读数，写进 `m5-current-state.md` |
| T1-02 | 组合根 + 服务骨架（SCM 注册、启动、停止、preshutdown） | 服务能装、能起、能停；无盘时干净退出 |
| T1-03 | job + 句柄保留 + 启动 sweep + 资源身份持久化（§5） | 强杀服务 → worker 自行有序拆卸；强杀 worker → 下次启动 sweep 回收，不留设备/盘符 |
| T1-04 | 读/写分离（§6.2 T1） | create 进行中 `disk list` 1 秒内返回 |
| T1-05 | public pipe + CLI 全命令面（§10.4 的 CLI 部分） | 单盘端到端：create → start → 读写 → checkpoint → stop → 重启恢复 → remove |
| T1-06 | **中断提交判定**（§4.3） | 在 P4/P5 之间制造服务崩溃 → 重启后盘能自行恢复或明确报 needs-attention，**不卡死** |
| T1-07 | 内存可见性（§7） | CLI 报出 resident / budget / dirty，与任务管理器观测同量级；删文件后常驻内存下降（**UNMAP 端到端首次验证**） |
| T1-08 | suspend / memory-pressure checkpoint（§9） | 休眠/恢复后 generation 前进且盘可用；内存压力下触发 checkpoint |
| T1-09 | **M5-A 单盘验收 + 断电恢复** | VMware 强制断电 → 开机自动恢复到各自最后完整 generation，无人工介入、无残余 |
| T1-10 | 默认策略收紧 | 默认 interval 60 s、dirty 阈值默认开启；文档写明"崩溃最多丢一个间隔" |

### T2 —— 多盘（命中 G4）

| 增量 | 内容 | VM 验收 |
|---|---|---|
| T2-01 | per-profile 串行（§6.2 T2） | A 正在 create 时 B 的 start 不被阻塞；并发 100 次无死锁 |
| T2-02 | 多盘隔离矩阵 | A 在 create/checkpoint/stop/移除/preshutdown 失败时，B 的 generation / 挂载 / worker / gate / writer lock 全部不变 |
| T2-03 | 双盘断电矩阵 | 断电后 A/B 各自恢复到最后完整 generation，不混 generation、无残留 |

### T3 —— GUI（命中 G1）

| 增量 | 内容 | VM 验收 |
|---|---|---|
| T3-01 | GUI client 库（只走 pipe，CMake 层物理隔离） | 负例测试：GUI 二进制不含 persistence/AIM/driver 符号 |
| T3-02 | 托盘 + 主窗口 + 磁盘列表 + 内存列 | 多盘正确显示；`aggregate` 为 verdict |
| T3-03 | 新建/导入/启动/停止/checkpoint + 进度 | 长操作期间界面不假死；进度与终态一致 |
| T3-04 | 权限与错误呈现 | 未提权只读可用、变更引导提权；错误分类可读 |
| T3-05 | GUI 验收 | 数据面资格通过后的独立薄客户端工作流验收 |

### T4 —— 产品化

| 增量 | 内容 |
|---|---|
| T4-01 | 安装器（打包 / SCM / ProgramData ACL / 前置检查 / 卸载必须安全下盘） |
| T4-02 | Load Only 模式、导出为单文件 VHDX、易失盘模式、卷标 / 簇大小 / 卷序列号 |
| T4-03 | 在线 compaction（有界 + 自动触发；journal 无界增长必须有界） |
| T4-04 | M5-E：干净 Debug/Release + 冻结哈希 + 证据工具 + release manifest |

---

## 14. 测试与防回归

### 14.1 层级

- 单元：profile codec/store、control IPC/鉴权、状态机、operation store、worker protocol。
- 模型（行为）：P4/P5 四类 disposition、§4.4 恢复表、两条 stop policy、并发锁序。
- 集成：worker → DiskRuntime → M3 全生命周期；两阶段边界；**中断提交判定的三态**；隔离矩阵。
- 真机（VMware）：T1/T2/T3 各自的验收矩阵 + 断电。
- 回归边界：**M4 单阶段 `checkpoint()` 的原回归必须持续通过**；两阶段由新增测试与真机覆盖，二者不互相替代。

### 14.2 `dirty_epoch` 跨会话防回归（必写行为测试）

1. 服务重启 → `checkpoint_required` stop 成功（提交新 generation 后删除）；
2. 服务重启 → 显式 `checkpoint` 成功（新 session 的较小 epoch 不被拒）；
3. `require_clean` + 重启后块级 dirty → **拒绝删除**、resume、不伪造成功；
4. P4 后 worker 被替换 → 不删除、进入 reconciliation、不误报成功。

### 14.3 中断提交判定的测试矩阵（T1-06 的验收面）

| 用例 | 期望 |
|---|---|
| prior generation 可证明 | 终态 `not_committed`，gate 释放 |
| candidate generation 可证明 | 走 committed-P5 路径 |
| 归属不可证明 | `resolving` / needs-attention，保留 gate，不删不移 |
| correlation 缺 manifest slot / digest / native id | `recovery_not_provable`，**拒绝回答**（不得当作匹配） |
| 零 digest | 视为**缺失**，拒绝 |
| 检查 worker launch 失败 | `resolving`，不伪报 |
| 只读性 | 检查前后 bundle 的 generation 与文件哈希不变 |

当前 `reconcile_checkpoint` 只有**两条拒绝用例**，正面路径一次都没执行过 —— 这是 T1-06 必须关掉的第一笔账。

---

## 15. 与 v2 / tasklist 遗留问题的对账

| 遗留 | v3 处置 |
|---|---|
| tasklist `M5-02` 标 `NOT_STARTED`，而 manifest §1.3 记录 Task 2 已验证 | 以 manifest 为准，tasklist 状态重写（v3 的 tasklist 已改） |
| tasklist `M5-05` 声称拥有 `windows_worker_recovery.{h,cpp}`，该文件在分支上不存在 | **划掉该文件名**。该能力的功能由 §4.3 的 launch-based inspection 覆盖，**不重建** `windows_worker_recovery` |
| v2 §3.6 "原 worker 可重连"两行 | **删除**，理由见 §4.4 |
| v2 §3.5 "worker 退出 → 只能据 durable 重判" | 保留，且现在它是**唯一**分支（因 §3 裁决） |
| M5-06 的 PASS 范围说明（其两个"无法关闭"的条目） | 第 1 条由 §4.4 删除该行关闭；第 2 条（新 worker 只读 recovery proof 的**执行**）归 T1-06 |
| `reconcile_checkpoint` 正面路径零测试 | T1-06 的验收面（§14.3） |
| execution gate 粗锁 | §6.2，T1-04 + T2-01 |
| `windows_worker_recovery` 曾建议"删掉" | 结论不变（不重建），但理由更新：不是"reattach 场景不存在"，而是"本期不做 worker 生存，故该组件无消费者" |
| 09-30 版静默丢失的两件事（自动重连、运行时租约） | 记录在 §0 与 §12：**本期仍不做**，但这次是**有记录的裁决**，不是丢失 |

---

## 16. 待确认的开放项

| # | 事项 | 状态 |
|---|---|---|
| 1 | ~~开发机测试规则是否收窄~~ | **已确认（2026-10-14）**：收窄为"开发机允许执行不涉及真实物理资源的测试"，判断标准是**该测试会不会创建真实 worker 进程 / AIM 代理盘 / Windows 卷**；不确定时按"只能上虚拟机"处理。已写入 `AGENTS.md` §4 |
| 2 | 虚拟机接入方式 | **已确认（2026-10-14）**：统一走 **OpenSSH**（`ssh.exe` / `scp.exe`）+ **密钥认证**，不用 `plink`/`pscp`、不用明文密码。客户机 IP 固化方案待定，在那之前所有脚本把主机地址作为参数传入，不得硬编码。已写入 `AGENTS.md` §5 |
| 3 | "小工具"（Primo 的 Mini Tools）具体指什么 | 待你定义，否则无法估。当前不列入范围 |
| 4 | T4-02 的"导出为单文件 VHDX"是否需要——它依赖把 journal 折进 base（复用 compaction 的 fold writer）；基底已是标准 VHDX | 建议做，价值高、成本中低。当前列入 T4 |
