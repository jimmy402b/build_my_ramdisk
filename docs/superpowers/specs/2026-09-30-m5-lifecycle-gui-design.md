# MyRamDisk M5 生命周期管理设计（服务 + CLI + 后置 GUI）

状态：设计 v2，供评审
日期：2026-09-30
前置基线：`docs/m4-m5-scope.md`；`main` = 干净 M4（`cb3df08`）

---

## 1. 目标与原则

在 M4 已验证的 `DiskRuntime` 引擎之上，实现可投入使用的多盘生命周期管理：
profile 配置、`disk` CLI、Windows Service 开机恢复、正常关机协调、单盘故障隔离、
VMware 断电验收。GUI 作为**独立后置**的薄客户端，不与数据面资格绑定。

**原则**（沿用 `docs/m4-m5-scope.md` 的不变量）：

1. **复用 `DiskRuntime` 为唯一引擎**——worker 只驱动 M4 的
   `create/recover/start/flush/checkpoint/shutdown_clean`，不重走 `HostCore → M3`
   的重复实现。
2. **单一权威**——public pipe → ServiceCore → worker → `DiskRuntime` → M3 → phdskmnt
   是唯一执行链路；不新增第二套状态机、JSON IPC、profile 格式或 host 执行核心。
3. **fail-closed**——任何 profile 损坏、资源冲突、驱动不可用、worker 身份不符或
   checkpoint 未确定，都拒绝而非猜测，不自动格式化、修复为零或删除 bundle。

---

## 2. 架构与分层

```text
┌─ 客户端层 ──────────────────────────────────────────────┐
│  myrd disk …（CLI）      myrd-gui（后置，薄客户端）         │  不碰 bundle/卷/设备
└───────────────────────┬─────────────────────────────────┘
                        │ 公共 named pipe（typed command，鉴权）
┌───────────────────────▼─────────────────────────────────┐
│  ServiceCore（management）                              │  命令→operation、授权、状态机、
│    + OperationStore + WorkerSupervisor                  │  operation 记录、编排
└───────────────────────┬─────────────────────────────────┘
                        │ 私有 authenticated Host IPC（per-profile worker pipe）
┌───────────────────────▼─────────────────────────────────┐
│  worker（host，每 profile 一个进程）                      │  薄包装：驱动 DiskRuntime
└───────────────────────┬─────────────────────────────────┘
                        │ 进程内调用（共享 production adapter）
┌───────────────────────▼─────────────────────────────────┐
│  DiskRuntime（run，M4 引擎，复用）                        │  create/recover/start/
│    + 两阶段 checkpoint 边界（唯一执行侧新增）              │  flush/checkpoint/shutdown
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│  M3 provider + persistence（复用）                       │  journal/VHDX/checkpoint/
│                                                        │  compaction/recovery
└───────────────────────┬─────────────────────────────────┘
                        │
                  phdskmnt → R:/S:（Windows 卷）
```

每层的单一职责与接口边界：

| 层 | 职责 | 接口（冻结） | 来源 |
|---|---|---|---|
| 客户端 | 解析/渲染命令，发请求给 service | typed command + public pipe | 从 M5 分支移植 `control` |
| ServiceCore | 授权、operation 编排、状态机、持久化 operation 记录 | `execute_operation` 语义 | 重建实现，冻结契约 |
| worker | 驱动一个 `DiskRuntime`，报告身份/状态/结果 | authenticated Host IPC | 重建为 DiskRuntime 驱动 |
| 引擎 | 单盘 create/recover/checkpoint/cleanup | `DiskRuntime` 接口 + 两阶段 | 复用 + 两阶段新增 |
| 持久化 | bundle/checkpoint/recovery | `M3Provider` 接口 | 复用 |
| profile | 配置与持久化权威 | 二进制 `profile.myrd` | 从 M5 分支移植 `profile` |

---

## 3. 两阶段 checkpoint 边界（本设计的核心）

### 3.1 为什么需要

ServiceCore 的 commit 边界不能「在 worker 已提交后回填 pre-commit 证据」。否则连接
丢失 / worker 退出 / ACK 丢失时，service 无法区分「已提交但未确认」与「未提交」，
会把 `commit_indeterminate` 误判为成功。因此 checkpoint 必须拆成
**prepare → commit / abort**，证据由 worker/M4 事实在 commit 前生成。

这是 **M5 引入的新需求（跨进程提交协调）**。M4 前台 `myrd run` 的单阶段
`checkpoint()` 对前台是完整且正确的——它无需在提交前让外层介入。M3/M4 现有实现
**无需修复或重做**；两阶段接口是 M5 为满足 P4/P5 提交边界而新增的适配能力。

### 3.2 契约

```text
prepare_checkpoint
  → 返回（不提交）：
      prior_generation        （durable 已确认 generation）
      candidate_generation    （= prior + 1）
      dirty_epoch             （本次切点的 worker session mutation 计数）
      worker session identity （profile_id/image_id/pid/creation_time/session_generation）

commit_checkpoint
  → worker 执行 M4 commit，返回**四类** disposition：
      committed / not_committed / commit_indeterminate /
      committed_needs_reconciliation

abort_checkpoint
  → 在 commit 前取消 candidate
```

> `block_dirty` **不进入 prepare 契约**。「是否 dirty」由 `require_clean` 路径经**独立的
> 严格 runtime 观察**（`require_runtime_observation`，读 M3 块级 dirty）获取，不作为 P4/P5
> 决策字段——prepare 到 commit 之间 dirty 可能变化，除非 mutations 已严格 quiesce。

> 四类 disposition（`committed` / `not_committed` / `commit_indeterminate` /
> `committed_needs_reconciliation`）必须在**私有 IPC、worker 返回值、durable evidence、
> 测试矩阵**中一致出现；`committed_needs_reconciliation` **不得**折叠成成功或普通
> indeterminate。

### 3.3 真实拆分点（依据 `m3_provider.cpp` 现状）

当前 `M3Provider::checkpoint()` 的实际路径（已验证）：

```text
checkpoint_impl
  → coordinator.prepare                        (:1397)
  → transaction.publish_manifest               (:1439)
  → pending_checkpoint_.emplace                (:1454)
  → resume_pending_checkpoint                  (:1455)
```

即 prepare → publish → resume 在**同一同步调用**内完成，`transaction.abort()` 只在失败
路径被调。要把「准备后暂停、等待上层决定」接出来，拆分点是：

```text
M3Provider::prepare_checkpoint()
  → coordinator.prepare → 保留 transaction（不 publish_manifest、不 resume）
M3Provider::commit_checkpoint()
  → transaction.publish_manifest → resume_pending_checkpoint
M3Provider::abort_checkpoint()
  → transaction.abort()
```

并需定义（冻结契约的一部分）：

- **transaction 持有者**：prepare 后由 M3Provider 持有（`pending_checkpoint_`），
  直到 commit/abort 或重启。
- **超时**：prepare 后 transaction 有界存活，超时未 commit/abort → 走 abort/reconcile。
- **worker 退出**：prepare 后 worker 退出 → transaction 随进程丢失（见 §3.5），service
  不得把它当 committed 或 aborted，据 durable manifest/journal 重判 → reconciliation。
- **服务重启**：`resume_pending_checkpoint` 是**进程内**接口，无法跨 worker 重启恢复
  transaction；重启后只能据 durable manifest/journal 重判最终持久化结果，绝不猜测。
- **native operation id 的映射规则（不改格式、不声称 journal 绑定 UUID）**：

  ```text
  constexpr std::uint64_t kM5CheckpointOperationOffset = 3902;
  M5 native_id = (candidate_generation + kM5CheckpointOperationOffset) mod 2^64
  ```

  - 基准：M4 源码 `operation_id = checkpoint_generation + 3902`，而
    `candidate_generation = checkpoint_generation + 1`，故按 candidate 表示
    `m4_native_id = candidate_generation + 3901`。
  - **只区分「本 M5 管理操作的本次 candidate」与 M4**：同一 candidate generation G 下，
    M4 为 `G + 3901`，M5 为 `G + kM5CheckpointOperationOffset`，offset 不同 → **必不相等**。
  - **若 `native_id == 0`（模 2^64 回绕），在 prepare 前 fail-closed**。
  - **journal 不直接绑定 service UUID**——完整 UUID 仅保存在 P4 durable record；由
    operation gate 保证同一 image/generation 只有一个 M5 operation，从而 UUID 无歧义。
  - P4 durable record 持久化 `service UUID`、`candidate_generation`、`native_id`；
  - recovery 验证 image + prior/candidate generation + journal native id（应等于
    `candidate_generation + kM5CheckpointOperationOffset`）全部匹配；UUID 归属由 P4
    record + operation gate 提供，不由 journal native id 提供。
  - 测试证明：同一 candidate generation 下 `m5_native_id != m4_native_id`。

- **manifest candidate 在 prepare 阶段确定**：`prepare_checkpoint` 打开并验证 active
  manifest，以**固定 committed time**（`image_.checkpoint_generation + 2`，与 M4 现有
  publish 一致）构造 `PreparedManifestCandidate`，返回并 durable 记录 `candidate slot` +
  `candidate bytes digest = SHA-256(PreparedManifestCandidate.bytes)`；之后
  `commit_checkpoint` 只能发布**这一个已记录的 candidate**——不得在 P4 后重新选 slot、
  重算 digest 或改 candidate（否则 P4 记录的不是实际提交对象）。

### 3.4 fail-closed 语义（修正 abort 表述）

- `continue_commit=false` 必须在真实 commit 前阻止提交。
- 连接丢失 / worker 退出 / ACK 丢失 → `commit_indeterminate` 或 reconciliation，
  **绝不猜成功/失败**。
- **abort 语义**（不保证连接丢失时可证明）：
  - 收到并验证 abort 响应 → 证明未提交；
  - abort 无响应 → 结果未知 → 进入 reconciliation，**不得宣称未提交**。
- 不改变 M4 已有的 generation 选择与「checkpoint 后写入可丢/可留」语义。

### 3.5 pending transaction 生命周期（跨 worker 存活边界）

核心事实（已验证）：`PreparedCheckpointTransaction` 持有 `ImageSession&`、
`DynamicBlockStore&`、`ConsistentSnapshotToken`、`PublishOperationLease`、
`shared_ptr<IPersistenceFile>` 等**进程内引用**（`checkpoint_coordinator.h`，注释明确
「referenced DynamicBlockStore and ImageSession must outlive this object」）。它
**没有 durable 序列化**，无法被新 worker 重建。

```text
worker 存活：
  prepared ──commit──▶ resolved(committed)
  prepared ──abort───▶ resolved(aborted)
  prepared ──超时────▶ abort → resolved(aborted)

worker 退出（崩溃/被杀）：
  prepared transaction 随进程丢失
  → 只能据 durable manifest/journal 重判最终持久化结果
  → 不得声称 commit 或 abort → reconciliation

M3 stop（与 pending transaction 的交互，必须新增规则）：
  prepared 且未 publish manifest → abort（释放 snapshot/lease），不得 resume
  已 publish、待 reconcile       → reconcile（manifest 已在盘上）
```

由此三条硬规则：

1. `DiskRuntime::shutdown_clean()` 在 pending transaction 未决时**不得**直接执行 M3 stop。
2. worker shutdown 前必须完成三者之一：已确认 abort / 已确认 commit / 标记 indeterminate
   → 进入专门 reconciliation。
3. M3 stop 必须区分「prepared 未 publish」与「已 publish 待 reconcile」两种 transaction
   ——当前 M3 stop（`m3_provider.cpp:1630`）无条件 `resume_pending_checkpoint`，需为 M5
   增加 gate（这是新增适配能力，不是判定 M3 有缺陷）。

**私有 worker reconciliation 命令**（worker 已死时的有权执行者）：

worker 已死时，原 `PreparedCheckpointTransaction` 已消失，无法 abort；ServiceCore 也不得
直接操作 bundle/M3（架构上 worker 才碰 bundle）。因此需要一个**私有 reconciliation 命令**：
新 worker 以 **P4 durable correlation** 为输入，打开并恢复 bundle，**验证归属后**返回三类
证据——「可证实仍在 prior generation」/「可证实为本 operation 的 candidate generation」/
「generation 已变但归属不可证明」。

**P4 durable correlation 必填字段**（service 侧持久化；journal 不承载 UUID）：

```text
service operation UUID + candidate_generation + native_id（= candidate_generation + kM5CheckpointOperationOffset）
image UUID + prior generation + candidate generation + manifest slot/digest
```

journal 的 native operation id 只区分 M5 与 M4（§3.3），不承载 UUID。新 worker 的 recovery
验证：journal native id = `candidate_generation + kM5CheckpointOperationOffset`、image、prior/candidate
generation 全部匹配；UUID 归属由 P4 record + operation gate 提供。任一不符 → 「归属不可
证明」→ `resolving`，**不得**确认原 operation 成功。只有**原 worker 仍存活且绑定匹配**
时，才允许向它发 abort。二者不得混用。

**超时与析构**：

- transaction 超时与 commit/abort 使用**同一个 runtime mutex**；超时 abort 与 service 的
  commit 请求不得同时推进同一 transaction。
- 新增的 M3 stop gate 必须覆盖**析构路径**：`M3Provider` 析构会调用 `stop()`，
  `DiskRuntime` 析构会调用 `shutdown_clean()`——二者都不得在未决 transaction 上盲 resume。

### 3.6 durable phase 恢复转换表（用 M5 分支真实 enum）

M5 分支的 durable enum 是（**冻结格式，不得改**）：

```text
stable / admitted / commit_boundary / evidence_recorded /
committed_pending_epoch_confirmation / resolving
```

P4 = `commit_boundary`（prepare 证据已 durable、commit 前）；P5 =
`evidence_recorded`（commit 证据已 durable）+ `committed_pending_epoch_confirmation`
（epoch 确认未落）。

恢复动作**不能只按 phase 分支**，必须按 `durable phase × 原 worker 存活 × disposition`
联合决定：

| durable phase | 原 worker | 重启后动作 |
|---|---|---|
| `stable` | — | 无 |
| `admitted` | 可重连 | 查询其 pending correlation → abort 或继续已定义 reconcile |
| `admitted` | 不存在 | 新 worker 只读 recovery proof；未拿到 proof → 保持 `resolving` |
| `commit_boundary` | 可重连 | 查询 pending correlation → abort 或继续 reconcile |
| `commit_boundary` | 不存在 | 新 worker 只读 recovery proof（判断 manifest 是否 publish）；未拿到 proof → `resolving` |
| `evidence_recorded` | — | 见 disposition 表（下） |
| `committed_pending_epoch_confirmation` | — | 同 session 确认 epoch，或 reconcile |
| `resolving` | — | 恢复 reconciliation，阻止 stop/remove |

`evidence_recorded` 与 `CheckpointCommitDisposition` **联合解释**：

| disposition | 重启后动作 |
|---|---|
| `committed` | 推进 confirmed generation → `committed_pending_epoch_confirmation` |
| `not_committed` | rollback（回退 prior generation） |
| `commit_indeterminate` / `committed_needs_reconciliation` | `resolving` |

**硬规则**：`admitted` / `commit_boundary` 阶段，**未拿到 abort proof 或 recovery proof 时，
保持 `resolving`，禁止重放 start / checkpoint / stop**——`admitted` 不保证「尚未 prepare」：
service 可能已向旧 worker 发出 prepare、旧 worker 仍持有 snapshot/lease。关键序列
`prepare 已发 → 旧 worker 持有 transaction → service 在收到 prepare 回复、写 `commit_boundary`
前崩溃`，必须按此 worker-liveness 分支处理，不得盲目 re-plan/start。

recovery proof **拿到时的确定终态**：

- 证明仍在 prior generation → terminal `not_committed`，恢复 `stable` / 释放 operation gate；
- 证明为本 operation 的 candidate generation → 进入 committed-P5 路径；
- 证明 generation 已变但归属不符 → `resolving` / needs-attention。

---

## 4. DiskRuntime 生产适配层（从 `run/main.cpp` 提取）

`DiskRuntime` 只接收 `DiskRuntimeOperations`（callback 结构）；其 production 组装当前
在 `run/main.cpp` 的 `ForegroundRuntime::make_operations()` 内，含前台专属逻辑（控制台
输出、Ctrl+C、owner token 等），不能直接被 worker 复用。

需先做一次**运行时适配层提取**，让前台 CLI 与 worker 共用同一份 production 操作：

```text
src/run/runtime_adapter.{h,cpp}   （新增，可复用）
  → 构造 DiskRuntimeOperations：
      WindowsVolumeOperations   （provision/find/verify/flush/assign/dismount/remove）
      M3RuntimeOperations       （create/recover/start/stop/checkpoint/maintenance）
      lifecycle context         （bundle、size、letter、owner token）

前台 CLI（run/main.cpp）  ── 复用 runtime_adapter
worker（host）            ── 复用 runtime_adapter + 两阶段 checkpoint 命令
```

提取后：前台 `myrd run` 与 worker 走**同一份** create/recover/checkpoint/cleanup 逻辑，
不再有第二套执行实现。两阶段 `prepare/commit/abort` 是 adapter 之上的唯一新增。

现有 `DiskRuntimeOperations` 只有单阶段（`checkpoint`/`flush_volume`/`shutdown_clean`），
不足以承载两阶段。需新增一个 runtime 状态对象，所有操作由同一 runtime mutex 串行化：

```text
RuntimeCheckpointSession（新增）
  - correlation           （operation_id / image_uuid / pid / creation_time /
                            session_generation）
  - prepared transaction state（pending / committed / aborted / indeterminate）
  - worker session identity
  - commit / abort / reconcile 状态
```

`prepare_checkpoint` 创建 session 并持有 transaction；`commit`/`abort`/`reconcile` 在同一
session 上推进；worker shutdown 依据 §3.5 解决未决 session。

---

## 5. 资源所有权表

每个资源「谁创建、谁验证、谁清理」必须明确，且所有权身份跨 IPC 表达：

| 资源 | 创建/取得 | 验证 | 清理 |
|---|---|---|---|
| bundle writer lock | M3（persistence 层，权威） | worker 重验 bundle identity | worker（M3 stop 释放） |
| M3 provider | worker（open） | worker（geometry/identity） | worker（stop） |
| AIM 设备 | M3 provider（start） | worker（device identity/disk number） | worker（stop AIM） |
| 卷 | worker（provision/find） | worker（volume GUID + extents 唯一） | worker（dismount） |
| 盘符 | worker（assign） | worker（占用检查） | worker（remove letter） |
| pending checkpoint transaction | M3（prepare） | service（P4/P5） | M3（commit/abort） |
| worker 进程 | service（launch） | service（WorkerBinding） | service（supervise/stop） |

**单盘故障只清理「该盘已验证拥有」的资源**——绝不按进程名/AIM 编号/显示名清理其它盘。

---

## 6. 身份绑定与 durable 状态（冻结契约）

### 6.1 完整 runtime identity

`WorkerBinding` 的 profile/image/PID/session 字段**不足以单独证明**清理的是哪一个设备
和卷。认证并持续验证的完整身份链至少包括：

```text
profile_id / profile_revision
image_uuid / bundle identity（writer lock + manifest + journal identity）
worker_pid / process_creation_time / session_generation
AIM/device identity（disk number + 设备唯一 ID）
physical drive
GPT disk GUID / partition GUID / volume GUID
mount target（盘符或目录）
current generation（观测值，非恢复权威）
```

这些身份在 `provision/find/verify` 时采集、在每次操作时重验。P4/P5 的 epoch 规则：

- **禁止**跨 session 比较（新 worker 的 epoch 与 durable epoch 比较）；
- **保留**同一 authenticated worker session 内 `P5.live_epoch >= P4.cut_epoch`；
  `live_epoch == cut_epoch` 表示 checkpoint 后无新写，`>` 表示已提交但又变 dirty；
- 仅校验身份不能证明 P5 的状态读取不早于切点。

worker 进程替换 / 绑定失配 → 不删除、进入 reconciliation，不误报成功。

### 6.2 durable 状态（service 侧）

- `operation`（`.myop` / `.prev`）：请求、阶段、终态、重启恢复信息。
- `service_state`（`.myrs`）：运行态快照。
- `profile.myrd`（`ProfileRecord.revision`）：配置权威，`revision` 严格递增。

这些格式在「重建实现」时**冻结**：重建不得改格式，旧 profile 必须能被新 service 读取
（迁移兼容）。

---

## 7. 数据流

### 7.1 `disk start <profile>`

启动模式由 **durable lifecycle** 决定，不猜 bundle 状态：

- `lifecycle == ready` → 只能 `recover`；
- service 自己刚创建、`lifecycle == provisioning` → 才允许 `create`；
- bundle 缺失/空、identity 不符、或状态与 lifecycle 不符 → fail-closed（不把路径错误/
  数据损坏/外部替换误当新盘初始化）。

```text
ServiceCore：授权 → 建 operation → Starting
  → worker 启动（校验完整 identity / profile_revision / lifecycle）
  → 按 lifecycle 选 create 或 recover（绝不因 bundle 看似为空而自动 create）
  → verify volume identity → assign drive letter
  → Running（volume/AIM/letter/lock/heartbeat 全验证后才发布）
```

### 7.2 `disk checkpoint <profile>`

```text
ServiceCore：Checkpointing
  → worker：flush volume → prepare_checkpoint（返回边界证据，不提交）
  → ServiceCore：验证 P4（identity + prior_generation == durable confirmed）
  → commit_checkpoint → P5（同 session 确认）
  → 发布 checkpoint durable state → Running
```

### 7.3 `disk stop <profile>`

```text
ServiceCore：Stopping → quiesce → flush
  → checkpoint_required：走 prepare/commit 边界后删除
  → require_clean：读 worker 块级 dirty；dirty → 拒绝并 resume；clean → 删除
  → remove drive letter → dismount → stop AIM → Stopped
```

### 7.4 开机恢复 / 关机

- 开机：service 枚举 `auto_start=true` profile，逐盘启动；一盘失败不阻塞其它盘。
- preshutdown：停止接收新变更 → 逐盘 flush/final checkpoint/dismount/删自有盘符/停
  runtime。**预算**：全局 deadline（不超过 SCM preshutdown 期限），逐盘按剩余预算计算
  （如 全局剩余 ÷ 未处理盘数）；超时后：不宣称 stop/checkpoint 成功；durable operation
  标为 indeterminate / needs-attention；保留 worker 让系统终止或等待其自行退出（不盲
  kill）。

---

## 8. 组件处置（从 M5 分支移植 + 审计，非从 M4 基线保留）

`main`（`cb3df08`）是干净 M4，**不含** profile/control/management/host/cli/product。
「保留」的准确含义是**从 `feature/m5-unified-integration` 移植并审计**。

| 组件 | 处置 | 具体含义 |
|---|---|---|
| `src/profile/*` | 移植 + 审计 | 二进制 `MYRDPRF`（35 字段）从 M5 分支移植，审计 schema/校验/原子更新 |
| `src/control/*` + `src/product/*` | 移植 + 审计 | typed command、IPC framing、pipe、鉴权、契约移植并审计 |
| `ServiceCore` + `state_machine` + `OperationStore` + `WorkerSupervisor` | 重建实现（分阶段，见 §9） | 冻结 §6 的 durable 格式与 operation 语义，按行为测试重建 |
| worker 执行核心 | 重建为 DiskRuntime 驱动 | 删 `HostCore → M3HostProviderAdapter`，worker 只调 `DiskRuntime` + 两阶段 |
| 两阶段 checkpoint 边界 | 新增薄层 | §3 的真实拆分 |
| `windows_service` 接线 | 重建 | 服务/worker 绑定、auto_start、preshutdown |
| GUI | 后置独立 | 数据面资格通过后单独实施 |

### 8.1 M4 → M5 profile 迁移（首次创建）

- M4 前台 bundle **没有** profile。首次 `disk create/import` 才生成 `profile.myrd`。
- `disk import`：import worker 持有**独占 writer lease** 覆盖全程——acquire lease →
  verify bundle/geometry/identity → service 原子发布 registration → hand off 给 managed
  worker 或 release lease。若 IPC 边界不能安全持有该 lease，则必须用**可验证的 import
  reservation**（而非「曾经可取得」）。无法证明独占 → 拒绝注册；不迁移、不改写 bundle
  内容。
- 不自动扫描/猜测挂载无 profile 的旧 bundle。
- profile schema 只接受 service 明确支持的版本；未知版本 → `Blocked`，保留原文件。

---

## 9. ServiceCore 分阶段实施（不整体推倒）

「重建 ServiceCore + state machine + OperationStore + WorkerSupervisor」拆成可验证阶段，
每阶段都有模型测试 + 真实 worker 集成测试，**不做一次性重写后再 VM 验收**：

1. **durable profile/operation 读 + CAS**：ProfileStore/OperationStore 读取、原子更新、
   revision 校验。
2. **单 worker start/recover**：一个 profile、一个 worker，DiskRuntime recover/create
   真机往返。
3. **checkpoint 两阶段**：prepare/commit/abort 四类 disposition 真机往返。
4. **stop / reconciliation / preshutdown**：checkpoint_required / require_clean 两策略 +
   `dirty_epoch` 防回归。
5. **双盘隔离 + auto_start**：A 失败不影响 B、开机逐盘恢复。
6. **GUI**（独立后置）：数据面资格通过后单独薄客户端工作流。

---

## 10. 错误处理与 fail-closed

- 查询命令按 caller filtering 提供；变更命令按 administrator/owner 授权。
- 退出码：`0` 成功 / `1` 产品失败 / `2` 参数/schema / `3` service 不可用 / `4` 权限 /
  `5` 超时或结果未确定。
- `disk remove/forget`：无法证明安全（writer lock / 设备 / bundle identity）时
  **fail-closed**，保留注册与 bundle，不强制删除、不伪报成功。
- 单盘失败只清理「该盘已验证拥有」的资源（见 §5），绝不跨盘清理。

---

## 11. GUI（独立后置薄客户端）

- `myrd-gui`：Win32 托盘图标 + 主窗口。
- 只走 public pipe（`public_pipe_client`），不直接碰 bundle/卷/驱动。
- 功能：磁盘列表（名称/盘符/状态/generation）、创建、启动、停止、checkpoint、刷新。
- **实施时机**：service/pipe/鉴权/恢复/stop 通过真机验收**之后**，作为独立薄客户端
  工作流，单独验收。

---

## 12. 测试与防回归

### 12.1 层级

- 单元：profile codec/store、control IPC/鉴权、状态机、operation store。
- 模型（行为测试）：F10/F11/F12、Blocker A–D 的回归用例。
- 集成：worker → DiskRuntime → M3 生命周期；两阶段 checkpoint 四类 disposition。
- 真机（VMware）：M5-A…M5-E 数据面矩阵 + 断电。
- **回归边界**：M4 单阶段 `checkpoint()` 原回归必须持续通过（不因新增两阶段而改变
  行为）；两阶段 `prepare/commit/abort` 由**新增**测试与真机验收覆盖，二者不互相替代。

### 12.2 `dirty_epoch` 跨会话防回归（必写行为测试）

1. 服务重启 → `checkpoint_required` stop 成功（提交新 generation 后删除）；
2. 服务重启 → 显式 `checkpoint` 成功（新 session 较小 epoch 不被拒）；
3. `require_clean` + 重启后块级 dirty → 拒绝删除、resume、不伪造成功；
4. P4 后 worker 替换 → 不删除、进入 reconciliation、不误报成功。

---

## 13. 验收阶段

- **M5-A** 单盘注册/start/checkpoint/stop（confirmed committed 才报成功）。
- **M5-B** 双盘并行与隔离（A 失败不影响 B）。
- **M5-C** 开机自动恢复（auto_start，逐盘、单盘失败隔离）。
- **M5-D** 正常关机 + VMware 断电后各盘独立恢复最后完整 generation。
- **M5-E** 发布（Debug/Release、回归、VM 矩阵、artifact hash、release manifest）。
- **GUI**：数据面资格通过后，独立薄客户端工作流验收。

---

## 14. 非目标

VSS、逐写持久化、远程管理、加密、驱动替换/安装、压缩/复制、复杂安装器、无限重试、
新的 bundle/profile/checkpoint 格式。GUI 之外的任何 UI 框架也不在范围。
