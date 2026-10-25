# M4 能力 与 M5 需求（Scope）

状态：已整理（v2：锁定「worker 复用 `DiskRuntime`」决策；第 4 节为保留/替换清单）
日期：2026-09-30
> **历史文档（2026-09-30 的范围决策）。** 文中对 `feature/m5-unified-integration` 的引用已过时：
> 该线已被 v3 取代，工作树与分支于 2026-10-25 删除，仅以 tag `archive/m5-unified-integration`
> 保留历史。**当前唯一活动线是 `feature/m5-lifecycle`**，状态见 tasklist。

前置：`main` = 干净 M4 基线（`cb3df08`）；当时的 M5 工作完整保留在
`feature/m5-unified-integration`（含 F10/F11/F12 修复与 Blocker A–D 处置）。

---

## 1. M4 —— 前台单盘「引擎」

M4 是 `myrd` 前台运行时。**单进程、单盘**：一个 `myrd run` 进程独占一个
bundle + 一个 AIM 设备 + 一个卷 + 一个盘符。无服务、无守护、无进程间 IPC。

### 1.1 命令

```text
myrd run    --bundle <path> --size <N> --drive-letter <R:>
            [--checkpoint-seconds <N>] [--checkpoint-dirty-mb <N>] [--ram-limit <N>]
myrd compact --bundle <path>
myrd --help
```

### 1.2 核心特征（源自 `src/run/*` 实际代码）

- **两种模式**（`DiskRuntime`）：
  - `create`：空 bundle → 建 generation-0 镜像 → provision GPT/NTFS 卷 → 校验 →
    flush → 提交 generation 1 → `Ready`。
  - `recover`：manifest bundle → 恢复最后完整 generation → 找卷 → 校验 geometry →
    分配盘符。**绝不重新 provision / 格式化。**
- **前置门**（在碰 bundle/AIM/卷之前）：提权管理员 token + `phdskmnt` 驱动可用。
  驱动缺失时不碰 bundle（包括未创建的 bundle 目录）。
- **自动 checkpoint**：按 `checkpoint_seconds` / `checkpoint_dirty_mb` 阈值触发；
  flush → checkpoint 顺序由 run loop 保证（先 flush 卷再持久化 checkpoint）。
- **干净关闭**（Ctrl+C）：flush → 最终 checkpoint → 卸载卷 → 删盘符 → 停 AIM →
  释放锁。反向、幂等、只清「本实例实际取得」的资源，不删其它实例的卷/设备/bundle。
- **恢复语义**：只恢复到「最后一次完整已提交 generation」；之后写入允许丢失。
  `commit_indeterminate` / `not_committed` 原样返回，调用方不得把 API 成功等同于已提交。
- **多盘**：靠**并行跑多个独立 `myrd run` 进程**，互不共享状态；同 bundle 第二个
  实例被 writer lock 拒绝。

### 1.3 M4 明确没有（= M5 的范围）

`disk list/start/stop/checkpoint`、Windows Service、开机自动恢复、关机协调、
profile 数据库（`profile.myrd`）、集中式状态、进程间 IPC、GUI。

---

## 2. M5 —— 多盘「管理层」

把 M4 的 `DiskRuntime` 包装成本机 Windows 服务管理的多盘系统，补上配置、编排、
服务与运维接口。GUI 是**独立后置**的薄客户端（见 §2.2.5 与 §5）。

### 2.1 数据流

```text
CLI (myrd disk …) / GUI   ← 都是 service 的客户端，走同一根 named pipe
        ↓
MyRamDiskService (Windows Service)
        ↓  每个 profile 一个 worker 进程
host worker（薄包装：驱动 DiskRuntime）
        ↓
DiskRuntime（M4 引擎）→ M3 provider → phdskmnt → R:/S:
        ↑
  两阶段 checkpoint 边界（prepare/commit/abort）—— 唯一的执行侧薄扩展
```

**决策（A）**：worker **真正复用 M4 的 `DiskRuntime`** 作为每盘引擎，不采用
`feature/m5-unified-integration` 里实际存在的 `HostCore → M3HostProviderAdapter`
独立执行核心。设计文档曾声明"复用 DiskRuntime"，实现却分叉成 HostCore 直连 M3；
本次重实现回到 A，消除那套重复引擎（正是 dirty_epoch / 块级 dirty 那批 bug 的温床）。

### 2.2 要补的能力

1. **Profile**（二进制 `profile.myrd`）——声明「哪些盘开机恢复」+ 配置（bundle /
   size / 盘符 / `auto_start` / checkpoint 策略）。存放于受保护的 `ProgramData`，
   原子更新、严格校验；不新增 JSON profile。
2. **`disk` CLI**：`create` / `import` / `list` / `start` / `stop` / `checkpoint`，
   外加 `auto_start` 开关（enable/disable）与 `remove/forget`（**fail-closed**：
   无法证明安全时拒绝，绝不伪报成功、不强制删除 bundle）。保留 `myrd run` 作 M4
   兼容/诊断前台入口。
3. **Windows Service**（`MyRamDiskService`）：
   - 开机自动恢复 `auto_start=true` 的 profile，逐盘启动；
   - **单盘故障隔离**：一盘失败不阻塞、不卸载、不清理其它盘；
   - preshutdown 协调：停止接收新变更 → 逐盘 flush/final checkpoint/dismount/删自有
     盘符/停 runtime，有界预算。
4. **IPC + 鉴权**：本机命名管道；查询对普通本机用户开放，变更需管理员/owner；
   错误分类稳定；任何 profile 损坏、资源冲突、驱动不可用、worker 身份不符或
   checkpoint 未确定都 fail-closed。
5. **GUI（独立后置，不与数据面资格绑定）**：Win32 托盘图标 + 主窗口，薄客户端走
   同一 pipe；列出磁盘（名称/盘符/状态/generation）、创建/启动/停止/checkpoint。
   **在 service/pipe/鉴权/恢复/stop 通过真机验收之后，作为独立薄客户端工作流实施。**

### 2.3 不改变 M4/M3 语义

M5 不改变 M3/M4 的 bundle 格式、checkpoint 算法、恢复选择规则、`commit_indeterminate`
/ reconciliation 语义，也不安装/替换/升级/卸载 `phdskmnt`。

---

## 3. 核心关系：引擎 vs 管理器

> **M4 是「引擎」——怎么正确地跑一个 RAM 盘；M5 是「管理器」——怎么在多盘、开机、
> 关机、故障的尺度上管理它们，并复用 M4 的 `DiskRuntime` 作为每个盘的引擎，而不是
> 重写它。**

M5 的核心工作不是重写磁盘逻辑，而是：把 M4 的 `DiskRuntime` 包进 worker 进程
（host），加 profile 配置、ServiceCore 编排、Windows Service 生命周期、IPC，再叠
一层后置 GUI。

---

## 4. 实现边界：保留/替换清单 + 验收证据

基于「剥离后移植可用部分 + 复用 DiskRuntime」的决策。每一项都给出处置与验收证据；
「替换」不意味着推倒重写，而是**冻结下述契约后按行为测试重建实现**。

| 组件 | 处置 | 冻结/保持的契约 | 验收证据 |
|---|---|---|---|
| `src/profile/*` | **保留（移植）** | 二进制 `profile.myrd` 格式 / 严格校验 / 原子更新 | profile codec/store 单测；非法输入 fail-closed |
| `src/control/*` | **保留（移植）** | typed command / IPC framing / pipe transport / 鉴权 | IPC + authorization 单测 |
| `src/product/*` | **保留（移植）** | uuid / error / state / quantity / json 契约 | 单测 |
| `ServiceCore` + `state_machine` + `OperationStore` + `WorkerSupervisor` | **替换实现**（冻结契约） | durable state 格式（`.myop/.prev`、`service_state.myrs`）、operation 阶段/终态/gate、P4/P5 checkpoint 边界、reconciliation 与 `commit_indeterminate` 语义、worker identity 绑定（PID/creation time/session generation）、既有 profile 迁移兼容 | 行为测试覆盖 F10/F11/F12 + Blocker A–D；见 §5 防回归条件 |
| **host 执行核心** | **替换为 DiskRuntime 驱动** | M4 已验收的 create/recover/checkpoint/cleanup 语义（不重复实现 M3 层逻辑） | worker → `DiskRuntime` → M3 生命周期真机往返；M4 回归仍通过 |
| **两阶段 checkpoint 边界** | **新增（薄层）** | prepare/commit/abort；prepare 返回真实 prior/candidate generation + dirty_epoch + session identity，commit 前可阻止，绝不伪造 pre-commit 证据 | 四类 disposition（committed/not-committed/indeterminate/reconcile）真机往返 |
| `windows_service` 接线 | **替换** | 服务/worker 绑定、auto_start、preshutdown、有界清理 | 服务生命周期 + VMware 断电矩阵 |
| GUI | **后置（独立）** | 薄客户端走同一 pipe，不直接碰 bundle/卷/驱动 | 数据面资格通过后，单独薄客户端工作流验收 |

不变量：public pipe → ServiceCore → host worker → **M4 DiskRuntime** → M3 →
phdskmnt 保持单一权威（不新增第二套状态机、JSON IPC、profile 格式、或第二套
host 执行核心）。

---

## 5. 关键防回归条件：`dirty_epoch` 跨会话

Blocker D（D1/D2/D3）的根因是把 worker 的 per-process 计数器当成了跨会话的 durable
单调时钟。防回归条件写死如下：

- `dirty_epoch` 是 **per-worker-session** 的 mutation 计数器，**绝不**作为跨进程/
  跨重启的 durable 单调时钟。
- 任何正常 stop / 显式 checkpoint 决策**不得**比较 `live_epoch`（worker session）与
  durable `current_dirty_epoch`（跨会话）。
- "是否 dirty"以 worker 报告的 **M3 块级 dirty**（`resident_dirty_blocks` /
  `zero_dirty_blocks`）为执行真相；worker 无法回答时 **fail-closed**（绝不把"没答案"
  当 clean）。
- P4/P5 只验证**同一 authenticated worker session**（profile id、image id、PID、
  process creation time、session generation），不比较 epoch 数值。
- 防回归行为测试至少覆盖：
  1. 服务重启 → `checkpoint_required` stop 成功（提交新 generation 后删除）；
  2. 服务重启 → 显式 `checkpoint` 成功（新 session 的较小 epoch 不被拒）；
  3. `require_clean` + 重启后块级 dirty → **拒绝删除**、resume、不伪造成功；
  4. P4 后 worker 替换 → 不删除、进入 reconciliation、不误报成功。
