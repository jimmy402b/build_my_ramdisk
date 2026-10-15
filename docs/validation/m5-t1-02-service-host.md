# T1-02 进展：服务生命周期骨架

日期：2026-10-15
分支：`feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`
设计：`docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md` §9
增量：tasklist T1-02（组合根 + 服务骨架：SCM 注册/启动/停止/preshutdown；无盘时干净退出）

**这一轮只完成了 T1-02 的第一块。** 状态如实记在这里，不夸大。

---

## 1. 已交付：`ServiceHost`（服务自身生命周期，不含 SCM）

| 文件 | 内容 |
|---|---|
| `src/service/service_host.{h,cpp}` | 服务生命周期：`start` / `stop` / `preshutdown`、状态、诊断 |
| `src/CMakeLists.txt` | 新目标 `myrd_service`（`myrd_assert_execution_boundary` 已挂上） |
| `tests/model/m5_service_host_tests.cpp` | 8 个纯逻辑用例，开发机执行 |
| `tests/model/m4_checkpoint_operation_tests.cpp` | fixture 的 `Operations` 增加 `fail_reconcile`（现有用例默认 `false`，行为不变） |

### 1.1 为什么要拆出这一层

Windows 服务是三件容易缠在一起的事：**SCM 协议**（`StartServiceCtrlDispatcher`、控制处理器、
状态上报）、**进程生命周期**、**实际工作**。`ServiceHost` 是中间那件。

**拆开的理由不是整洁，是可验证性。** 一个"只能靠装服务才能行使生命周期"的 host，
它的 start/stop 路径一辈子就只在 VM 上手工量过一两次。拆开之后，start/stop 的顺序、
幂等、预算传递、拒答路径**每次构建都被量**；VM 那边剩下的是真正需要 SCM 的部分：
控制管理器能起它、能停它、关机时能在进程被杀掉之前找到它。

### 1.2 契约（每条都有对应用例）

| 行为 | 为什么 | 用例 |
|---|---|---|
| 先 `recover_after_restart()`，**再** `scheduler.start()` | 调度器在恢复还在改写 operation 记录时就开始轮询，会领走一条"归属还没决定"的操作 | `service_host_recovers_before_it_admits_work`、`..._does_not_reach_the_scheduler_when_recovery_fails` |
| 恢复失败 ⇒ 状态 `failed`，**不是** running | 在无法对账的存储之上报 RUNNING，等于宣称一个健康服务跑在未知的持久状态上 | 同上 |
| 调度器起不来 ⇒ `failed` | 能收命令、永不执行的服务不是 running | `service_host_reports_a_failed_scheduler_start_as_not_running` |
| `start()` 在 running 时幂等 | 重入不能把一个跑着的调度器停掉再起——那是把在途操作连根拔 | `service_host_start_is_idempotent_while_running` |
| `stop()` 幂等，且**未启动时也回答成功** | SCM 会向启动失败的服务发 stop；把这报成关停失败，会让"机器要关机"看起来像"服务坏了" | `service_host_stop_is_idempotent_and_safe_before_start` |
| 调度器停不下来 ⇒ **不宣称 stopped** | 调度器还持有 core、core 还持有存储；宣称停机会让进程去销毁线程仍在调用的对象 | `service_host_does_not_claim_stopped_when_the_scheduler_will_not_stop` |
| 预算以**绝对 deadline** 下传 | 接缝收的是绝对时间，而 deadline 必须由时钟推导，不能凭空造 | `service_host_stop_budget_becomes_the_scheduler_deadline` |
| `preshutdown` 与 `stop` **同一条实现** | 两份关停实现就是两个可能互相矛盾的关停，而机器关机是最不该发现这件事的时刻 | `service_host_preshutdown_is_a_stop_and_keeps_the_same_budget` |

**无盘**（T1-02 的验收措辞）在这层是：`recovered_operations == 0` 且服务照常 up/down。
`service_host_starts_and_stops_cleanly_with_no_disks` 直接断言这一点——
"没有要对账的工作"是健康的启动，不是退化的启动。

### 1.3 明确**没有**做的部分（写下来而不是等人发现）

设计 §9 的逐盘关停——"关闭变更准入 → 停 scheduler → 按预算逐盘 checkpoint + 拆卸"——
**这一轮没做**。只做了 scheduler 的停止，因为那是"任何被它调用的东西被销毁之前"必须先发生的一步。
逐盘那部分属于拥有 worker 拆卸的那层（T1-03），在这里凭空造一个还不存在的接缝没有意义。

---

## 2. 尚未完成（T1-02 剩下的两块）

1. **SCM 包装**：`myrd-service.exe` 入口、`StartServiceCtrlDispatcherW`、
   `RegisterServiceCtrlHandlerExW`、`SERVICE_CONTROL_STOP` / `_PRESHUTDOWN`、状态上报。
2. **组合根**：把 `WindowsWorkerSessionProvider` + `ServiceWorkerControl` +
   `ProfileRegistrationStore` + `ServiceOperationStore` + `ServiceExecutionScheduler` +
   时钟/日志/ID/物理内存查询装起来，并准备 ProgramData 的 durable 根。

**为什么组合根没有在这一轮硬凑出来**：它需要 ProgramData 的 ACL/目录布局机制、
`IProfileRepairBindingVerifier` 与 `IOperationWaitPace` 的实现，以及"存储不存在时怎么办"
这个 fail-closed 决策。那是一个自成一体的块，硬凑出一个"能起但什么都没接"的服务
只会给下一轮留下一个更难查的起点。

### 2.1 一个需要你裁决的点（组合根落地时会碰到）

**ProgramData 根不存在时**（全新机器）服务该怎么办？两个方向：

- **fail-closed**：启动失败并给出明确状态（安装器/首启负责建根）。诚实，但"无盘时干净退出"
  这条验收在全新机器上就跑不出绿。
- **按需创建**：服务首启时以 SYSTEM 身份建根并套 ACL。方便，但**把一次提权写操作放进了服务启动路径**。

我的倾向是 **fail-closed + 由 T4-01 的安装器负责建根**（那正是安装器的职责，
而且设计要求它管 ProgramData ACL）。这意味着 VM 验收时会先手工建好根来验"无盘干净退出"。
这属于设计未决项，按约定在这里说明。

---

## 3. 附带发现

**`m5.execution_scheduler` 在开发机上本来就是失败的。** 我用 `git stash` 回到未改动的
HEAD 复核过，**同一个用例在基线代码上同样失败**（`m5_execution_scheduler_tests.cpp:455`，
`!record_is_succeeded(fixture, operation_id)`），与本轮改动无关。

它落在 T1-05/T1-06 的验收面上，**建议在做到那两个增量时一并处理**。

对照：`m5.service_operation_store` 在开发机失败是已知的环境项
（`m5_service_operation_store_tests.cpp:92: created || error == ERROR_ALREADY_EXISTS`，
和 `unit.m4.profile_store` 同一类：SYSTEM/Administrators-only 的 DACL 目录在 UAC 过滤
token 下不可用），VM 上以管理员身份运行时通过。

---

## 4. 本轮读数（开发机，纯逻辑）

```
m5.service_host  ....  8/8 用例通过（0.04 s）
```

**声明范围**：这是**开发机纯逻辑**读数，1 次。它不建真实 worker、不开 AIM 盘、不建卷，
符合 `AGENTS.md` §4。

T1-02 的 **VM 侧验收（`sc create` / `sc start` / `sc query` / `sc stop`、无盘干净退出）
尚未执行**，因为可安装的服务可执行文件还没写出来。这一点必须说清楚：
**T1-02 还不是 PASSED，它是 IN_PROGRESS。**
