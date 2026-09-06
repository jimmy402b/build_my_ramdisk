# MyRamDisk M5 统一集成设计

状态：已批准，作为 M5 实施唯一设计依据。

## 目标

在 M4 已验证的 `profile.myrd`、checkpoint/reconciliation、资源归属和 AIM 边界之上，补齐可投入使用的多盘管理：命令行注册/启动/停止/检查点、Windows Service 开机恢复、正常关机协调、单盘故障隔离和 VMware 断电验收。

## 统一原则

1. 现有 `src/profile` 的二进制 `profile.myrd` 是唯一配置和持久化权威；不新增 JSON profile。
2. 现有 `src/management` 的 `ServiceCore`、状态模型、`OperationStore`、`ProfileRegistrationStore` 和 `WorkerSupervisor` 是唯一管理权威；不新增平行状态机。
3. 现有 `src/control` 的 typed command、framed IPC、named pipe 和授权模型是唯一 CLI-Service 通道；不新增 JSON IPC。
4. 现有 `src/host` 的 `MyRamDiskHost`、Host IPC 和资源清理是唯一 worker 执行路径；不新增第二种 worker 主进程协议。
5. M4 的最新完整 checkpoint 选择、`commit_indeterminate`、`committed_needs_reconciliation` 和 operation gate 语义不变。
6. `phdskmnt` 仅作为外部前置依赖检查，M5 不安装、替换、升级或卸载它。

## M5 增量范围

- 将现有 command/ServiceCore/host 链路接成可用的 `disk` CLI。
- 支持现有 profile 的 create/import、list、start、stop、checkpoint 和 `auto_start` 配置。
- 服务启动时按 profile 发现并恢复 `auto_start=true`；一盘失败不阻塞其他盘。
- 复用现有 worker 身份认证、独立资源归属和失败恢复；补齐缺失的状态聚合/查询。
- 复用 preshutdown 和有界 best-effort checkpoint；未确定结果不得伪报成功。
- 记录服务、请求、操作、profile、worker identity、generation 和清理结果。

## 非目标

GUI、VSS、逐写持久化、远程管理、加密、驱动替换、压缩/复制、复杂安装器、无限重试和新的 bundle/profile/checkpoint 格式。

## 数据流

```text
myrd disk ...
  -> control::PublicPipeClient / typed Command
  -> management::MyRamDiskService / ServiceCore
  -> existing ProfileStore + ProfileRegistrationStore + OperationStore
  -> existing WorkerSupervisor
  -> MyRamDiskHost per profile
  -> HostCore -> M4 DiskRuntime -> M3 provider -> phdskmnt -> Windows volume
```

CLI 不直接打开 bundle、卷或设备。所有路径、profile、操作和资源身份由 service 从受保护的现有 profile/registration truth 解析。

## 可验收阶段

### M5-A 单盘服务化

一盘可通过现有 profile 格式注册、`disk start`、读写、`disk checkpoint`、`disk stop`；只有 confirmed committed 才报告成功；未确定状态可诊断显示；M4 回归测试全通过。

### M5-B 双盘并行与隔离

A/B 使用独立 bundle、profile、盘符并可同时运行。A 的 checkpoint/stop/worker 失败不得影响 B；不得删除、格式化或卸载另一盘资源。

### M5-C 开机恢复

服务启动后自动恢复 `auto_start=true` profile 到各自最新完整 generation；一个 profile 失败时其他 profile 继续；状态能区分 Running、Blocked/NeedsAttention、Failed。

### M5-D 正常关机与强制断电

正常关机停止接收新变更，按有界预算执行 best-effort checkpoint 和逐盘清理。VMware `Power Off -> Power On` 后，各盘独立恢复到自身最后完整 generation，不混合 generation，不留下无法解释的 myrd/盘符/AIM/worker 残留。

### M5-E 发布

Debug/Release、M4 回归、M5 单元/集成测试、上述 VMware 矩阵、artifact hash、驱动前置条件、安装使用文档和 release manifest 全部具备证据。

## 错误与安全

沿用现有 `Error`、授权规则和命令错误映射。查询命令按现有 caller filtering 提供；变更命令按现有 administrator/owner 规则授权。任何 profile 损坏、资源冲突、驱动不可用、worker 身份不符或 checkpoint 未确定都 fail-closed，不自动格式化、修复为零或删除 bundle。

## 兼容性

`myrd run` 保留为 M4 前台兼容/诊断入口，不读写 M5 管理集合。所有 M5 管理操作必须通过现有 typed IPC 和 ServiceCore。
