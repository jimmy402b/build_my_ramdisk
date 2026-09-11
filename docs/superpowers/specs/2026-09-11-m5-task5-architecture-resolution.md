# MyRamDisk M5 Task 5 前置架构修正设计

状态：已记录，作为继续实现 Task 5 前必须解决的架构前置任务。

## 1. 问题结论

当前仓库存在两条彼此不通的产品栈：

```text
栈 A：PublicPipeServer -> ServiceCore -> IServiceProfileStore/
      IServiceRegistrationStore/IServiceOperationStore/IWorkerControl

栈 B：Windows Service -> ProductServiceActions -> MyRamDiskHost.exe
      -> DiskRuntime/windows_volume -> M3 provider -> phdskmnt
```

栈 A 的 `IWorkerControl` 没有生产实现，只有测试 fake；栈 B 是已经通过 M4 验收的真实执行路径，但没有使用 `ServiceCore`、`PublicPipeServer` 或 `ProfileRegistrationStore`。因此当前不能安全地把 public pipe 直接接到 ServiceCore，也不能把它直接接到 ProductServiceActions。

## 2. 禁止的做法

- 不创建新的 profile 格式、JSON IPC、第二套状态机或第二套 worker 协议。
- 不用 fake 或进程内内存对象冒充生产 ServiceCore 后端。
- 不把 host 已经完成的 checkpoint 结果事后包装成 pre-commit evidence。
- 不只适配部分 `ActionKind` 后开放 `disk stop`；stop 必须是完整且可清理的动作序列。
- 不改变 M4 的 checkpoint、reconciliation、`commit_indeterminate` 或资源清理语义。
- 不让 CLI 绕过 service 直接操作 profile、bundle、设备或卷。

## 3. 推荐架构

最终唯一执行链路为：

```text
CLI
  -> existing typed PublicPipeClient
  -> MyRamDiskService / PublicPipeServer
  -> ServiceCore
  -> production execution backend
  -> MyRamDiskHost.exe / ProductServiceActions
  -> M4 DiskRuntime + M3 provider + phdskmnt
```

`ServiceCore` 继续是命令、授权、操作记录、状态和 reconciliation 的唯一管理权威；M4 host/runtime 继续是设备、卷和持久化事实的唯一执行权威。两者之间只能通过一个经过认证、版本化且能表达真实提交边界的生产 adapter 连接。

## 4. 前置任务 A：统一持久化权威

必须先确定并实现一个权威映射：

- `profile.myrd` 继续保存用户 profile 和 M4 已定义的 durable profile 字段；
- `OperationStore` 保存操作请求、阶段、终态和重启恢复信息；
- runtime lease、worker identity、checkpoint/reconciliation state 只能由现有管理/host 事实映射产生；
- `ProfileRegistrationStore` 不得对真实服务返回空的 `service_state.myrs` 快照，也不得与 `ProfileStore` 各自独立解释同一个磁盘。

服务启动、`disk list` 和 `service status` 必须从同一套真实持久化事实生成结果。若某个旧 profile 没有可安全映射的 registration state，必须返回 `Blocked/NeedsAttention`，不能静默显示为未配置或创建空状态。

通过条件：

1. 真实 M4 profile 可被 service 枚举；
2. profile ID、image ID、bundle、mount、generation、worker lease 能一致解析；
3. current/previous、CAS、indeterminate 和 reconciliation 语义保持不变；
4. 任何缺失或冲突事实都 fail-closed。

## 5. 前置任务 B：生产执行后端

实现一个生产 `IWorkerControl` adapter，或等价的单一生产执行接口，但必须覆盖 ServiceCore 允许的完整动作集合，至少包括：

- start/recover/provision；
- checkpoint；
- stop 的 quiesce、flush、checkpoint、mount removal、device removal、runtime stop；
- 必要的 reconciliation、residual cleanup 和 identity proof。

该 adapter 必须复用现有 `ProductServiceActions`、`MyRamDiskHost` 和 Host IPC，不得自行创建第二个设备/卷生命周期。

每个 profile 的 adapter 必须独占并验证：profile ID、image ID、bundle writer lock、worker process identity、AIM/device identity、volume identity、mount target 和 runtime lease。A 失败时只能清理 A 已验证拥有的资源，不能触碰 B。

通过条件：

- `ServiceCore::execute_operation()` 可在真实后端上执行 start/checkpoint/stop；
- action 失败后可诊断、可重试或进入 NeedsAttention；
- stop 失败不会留下未解释的盘符、卷、AIM 设备或 worker；
- M4 原有 host、worker、checkpoint 和 stop/forget 回归测试全部通过。

## 6. 前置任务 C：checkpoint 提交边界闭合

现有 host checkpoint 响应是在 worker 已提交后返回 `confirmed/generation`，不能满足 ServiceCore 的 pre-commit boundary callback。因此必须在现有私有 Host IPC 中增加同一语义的两阶段控制，而不是伪造证据：

```text
prepare_checkpoint
  -> 返回真实 prior_generation、candidate_generation、dirty_epoch、worker session identity

commit_checkpoint
  -> worker 执行 M4 commit，并返回 confirmed/not-committed/indeterminate

abort_checkpoint
  -> 在 commit 前取消 candidate，返回未提交证明
```

要求：

- `continue_commit=false` 必须能在真实 commit 前阻止提交；
- commit boundary evidence 必须由 worker/M4 事实生成，服务只能验证和持久化；
- 连接丢失、worker 退出或 ACK 丢失必须进入 `commit_indeterminate` 或 reconciliation，不能猜测成功/失败；
- 不得改变 M4 已有 generation 选择和后续写入可丢失/保留语义。

通过条件：

1. 四类 disposition 都能从真实 host 往返得到；
2. `commit_indeterminate` 不会变成 succeeded；
3. restart/reconciliation 可使用 durable evidence 继续处理；
4. VMware 强制断电后恢复到最后完整 generation。

## 7. 前置任务 D：接入 Windows Service

只有 A/B/C 完成后，才允许修改 `windows_service.cpp`：

1. `ProductServiceActions` 初始化并长期持有统一的 Profile/Registration/Operation/Execution 对象；
2. 服务启动完成 driver preflight、profile discovery、worker reattach 后，启动 `PublicPipeServer`；
3. pipe handler 调用同一个生产 ServiceCore；
4. preshutdown 先停止接收新变更，再按现有有界预算执行 checkpoint/stop/cleanup；
5. 一个 profile 失败不得阻塞其它 profile；
6. public pipe 在 service stop 前有界停止，客户端超时只表示结果未知，不取消服务端正在执行的安全操作。

Task 5 的首个可验收目标不是“所有命令都立即开放”，而是：

- `disk list`/`service status` 读取真实统一事实；
- start/checkpoint/stop 仅在生产后端和提交边界通过后开放；
- 未实现或无法安全证明的动作返回明确的 `Unsupported/NeedsAttention`，不改变资源。

## 8. 验收顺序

### T5-A 统一事实

真实 M4 profile 可枚举，状态、generation、worker identity 和 mount 正确。

### T5-B 生产后端

ServiceCore 通过真实 host 执行单盘 start/checkpoint/stop，四类 checkpoint disposition 和 stop 清理均可验证。

### T5-C 双盘隔离

A/B 同时运行；A 失败时 B 仍运行、generation 不变、清理调用不交叉。

### T5-D public pipe

CLI 通过 PublicPipeClient 访问 service；query/mutation 授权、请求关联、超时和错误分类正确。

### T5-E 开机与断电

auto_start、preshutdown、正常重启和 VMware `Power Off -> Power On` 均恢复到各自最后完整 generation。

## 9. 当前计划调整

原统一计划中的 Task 5“直接把 public pipe 接入 Windows Service”暂停，改为以下顺序：

1. Task 5-A：统一持久化事实；
2. Task 5-B：生产 `IWorkerControl`/execution adapter；
3. Task 5-C：Host IPC checkpoint 两阶段边界；
4. Task 5-D：Windows Service + PublicPipeServer 接入；
5. Task 5-E：Task 4/5 端到端和 VMware 验收。

在 5-A 至 5-C 未通过前，不打开虚拟机做 M5 service 验收，也不声称 Task 5 已完成。
