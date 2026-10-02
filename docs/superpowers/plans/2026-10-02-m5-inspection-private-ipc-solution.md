# M5 Recovery Inspection 与 Private Worker IPC 解决方案

## 目标

补齐 M5 生命周期实现中三个明确缺口：

1. 新 worker 如何从恢复侧证明 checkpoint 归属；
2. service 与 worker 如何建立独立的私有管道和启动契约；
3. worker-control、recovery 和 ServiceCore 编排如何划分任务边界。

本方案不改变 M3/M4 的磁盘格式、M4 前台单阶段 checkpoint 语义或 `phdskmnt`
驱动边界。

## 1. 恢复侧只读 Inspection API

### 1.1 所属任务

由 Task 3 实现，位于 persistence/M3 层；Task 5 只调用该接口，不直接读取 bundle。

### 1.2 数据结构

```cpp
struct CheckpointRecoveryInspection final {
    ImageUuid image_uuid;
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t journal_operation_id{};
    std::uint8_t selected_manifest_slot{};
    Sha256Digest selected_manifest_digest{};
};

Result<CheckpointRecoveryInspection> inspect_checkpoint(
    const OperationContext& context,
    const CheckpointRecoveryCorrelation& expected);
```

### 1.3 实现要求

- 从恢复侧读取已提交 manifest、journal/index 和
  `JournalRecordCommon::operation_id`；
- 不使用写入侧 `JournalIndexBuildPlan` 作为恢复证据；
- 只读，不创建 runtime、不持有 writer transaction、不修改 bundle；
- 验证 image UUID、prior/candidate generation、native operation id、manifest slot；
- 重新读取选中的 manifest slot，并计算其完整 bytes 的 SHA-256；
- 将 digest 与 P4 durable correlation 比较；
- 任一字段不匹配返回“归属不可证明”，不得确认原 operation 成功。

### 1.4 接口位置

建议把底层恢复逻辑放在：

- `src/persistence/image_inspector.{h,cpp}`；
- `src/persistence/recovery_engine.{h,cpp}`。

`M3Provider` 只提供薄包装，使 worker 可以通过 M3 公开的只读接口调用它。

这样可以保证：ServiceCore 不直接接触 bundle，worker 也不从写入侧计划对象推断
恢复结果。

## 2. 私有 Worker 管道与启动契约

### 2.1 所属任务

由 Task 5 完整负责，包括管道、启动、握手、worker-control 和资源清理。

Task 6 只调用 Task 5 暴露的管理接口，不负责进程创建或协议实现。

### 2.2 启动流程

```text
ServiceWorkerControl
  -> 创建 per-profile 私有 named pipe server
  -> 创建一次性继承启动通道
  -> 生成 WorkerLaunchSpec
  -> 启动 worker
  -> worker 读取启动描述
  -> worker 连接私有 pipe
  -> authenticated handshake
  -> 通过后才打开 bundle
```

### 2.3 WorkerLaunchSpec

```cpp
struct WorkerLaunchSpec final {
    product::Uuid profile_id;
    std::uint64_t profile_revision{};
    std::wstring bundle_path;
    std::uint64_t logical_size_bytes{};
    std::wstring mount_target;
    std::uint64_t session_generation{};
    std::wstring private_pipe_name;
    StartupSecret startup_secret;
};
```

启动 secret 不得放入命令行。应通过继承的匿名管道或受保护的 inherited handle
传给 worker；命令行最多携带非敏感的 handle 值或启动标识。

### 2.4 私有管道要求

- 每个 profile、每次 worker session 使用唯一的 pipe 名称；
- pipe ACL 只允许 service 与目标 worker 使用；
- worker 未完成握手前不得打开 bundle、AIM 或 volume；
- handshake 绑定 profile id、image id、PID、process creation time、session generation；
- 所有请求携带 operation UUID 和 authenticated worker binding；
- 必须拒绝重放、序号回退、profile/image 不匹配和 session 不匹配。

### 2.5 私有消息类型

私有协议至少包含：

- `prepare_checkpoint`；
- `commit_checkpoint`；
- `abort_checkpoint`；
- `status`；
- `require_runtime_observation`；
- `recovery_proof`；
- `remove_and_stop`；
- `shutdown`。

`prepare_checkpoint` 回复必须携带：

```text
WorkerBinding
prior_generation
candidate_generation
native_operation_id
cut_dirty_epoch
manifest_slot
candidate_bytes_digest
```

### 2.6 Recovery proof

worker 已死时，新 worker 接收 P4 durable correlation，调用 M3 的只读 inspection，
只返回以下三种结果之一：

```text
prior generation proved
this M5 candidate proved
ownership unprovable
```

“this M5 candidate proved”必须同时满足：

- image UUID 匹配；
- prior/candidate generation 匹配；
- journal native operation id 匹配；
- manifest slot 匹配；
- manifest bytes digest 匹配。

否则进入 `resolving`，不得确认成功、删除资源或重放 start/checkpoint/stop。

只有原 worker 仍存活且 binding 匹配时，才允许向原 worker 发 abort。

## 3. Task 5 / Task 6 文件归属

### 3.1 Task 3：M3 与 persistence inspection

负责：

- `src/m3/m3_provider.{h,cpp}`；
- `src/persistence/image_inspector.{h,cpp}`；
- `src/persistence/recovery_engine.{h,cpp}`；
- `tests/integration/m5_checkpoint_inspection_tests.cpp`。

职责是回答：bundle 当前能证明什么。该层不负责 service operation、worker 进程或
公共/私有管道。

### 3.2 Task 5：Worker 执行与私有 IPC

负责：

- `src/host/m5_worker_protocol.{h,cpp}`；
- `src/host/m5_worker_private_pipe.{h,cpp}`；
- `src/host/m5_worker_main.cpp`；
- `src/management/service_worker_session.{h,cpp}`；
- `src/management/service_worker_control.{h,cpp}`；
- `src/management/windows_worker_observation.h`；
- `src/management/windows_worker_recovery.{h,cpp}`。

职责是：

- 创建并管理 worker 私有管道；
- 生成并传递 WorkerLaunchSpec；
- 完成握手和 worker identity 绑定；
- 执行 checkpoint/stop/status/recovery 请求；
- 使用 M3 只读 inspection 生成 recovery proof；
- 只清理该 worker 已验证拥有的资源。

### 3.3 Task 6：ServiceCore 编排

只负责：

- durable operation 和 profile 状态转换；
- P4/P5 durable publication；
- phase、disposition、worker-liveness 的恢复决策；
- stop policy 和 reconciliation 编排；
- 对 Task 5 接口返回值进行安全判断。

Task 6 不得拥有或重复实现：

- worker 进程启动；
- 私有 pipe framing/handshake；
- bundle/journal/manifest 读取；
- worker-control cleanup；
- M3 recovery inspection。

## 4. 验收门

- [ ] recovery proof 不读取 write-side build plan；
- [ ] ServiceCore 不直接打开 bundle 或解析 journal bytes；
- [ ] private worker pipe 不复用 public command protocol；
- [ ] worker 启动 secret 不出现在命令行；
- [ ] Task 5 负责 worker-control/session/recovery 的实现，Task 6 只负责编排；
- [ ] worker replacement 不能触发 synthetic checkpoint success 或错误 removal；
- [ ] M3/M4 磁盘格式和 M4 前台单阶段 checkpoint 行为不变。
