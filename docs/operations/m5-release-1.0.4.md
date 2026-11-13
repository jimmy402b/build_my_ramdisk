# MyRamDisk 1.0.4 发布说明（2026-11-13，候选）

**产品**：Windows 11 x64 单盘 RAM disk。
**版本**：`1.0.4`（四个二进制自报 `1.0.4`）。
**状态**：**已通过定向验收的候选；未冻结、未打包、未发布。**
**范围**：只修 1.0.3 的 auto-start 缺陷。

## 1. 问题

2026-11-13 的重启后，`Z:` 没有自动拉起；服务日志显示：

```text
disk.start auto-start: durable running state had no worker, so it was retired
disk.start auto-start: this profile was running when the service last had it
disk.start create-runtime failed
action=create-runtime code=1115 native=0 step=OpenSCManagerW
detail=service control manager could not be opened
```

`1115` 的 Windows 含义是“系统正在关机”。Windows Update 的计划重启在关机窗口里启动了服务；auto-start 先把 durable `running` 发布成 `stopped`，随后 worker 的驱动预检 `OpenSCManagerW` 返回 1115，启动失败。下一次开机时 `admit_auto_starts()` 只考虑 `running/starting`，看到 `stopped` 就跳过，于是磁盘**永久停在 stopped**，直到人工 `disk start`。

数据侧没有损坏：最后 committed checkpoint 仍在，manual start 后 `Z:` 可读；当时数据的最新文件时间早于最后 checkpoint，没有看到 checkpoint 之后的写入。

## 2. 修复

**不在关机窗口消费 durable running 意图。**

- `ServiceHostOptions` 新增 `system_shutting_down` 探针；生产 `service_main` 传入
  `GetSystemMetrics(SM_SHUTTINGDOWN) != 0`。
- `ServiceHost::start()` 在 `admit_auto_starts()` 之前读取该探针。
- `ServiceCore::admit_auto_starts(bool system_shutting_down)` 在系统正在关机时：
  - 不改写任何 profile 的 durable 状态；
  - 不派发 auto-start 操作；
  - 写一条 `restart-reconcile` 日志；
  - 下一次正常开机仍看到 `running`，于是重新执行 auto-start。

这避免了“把 running 改成 stopped 后再失败，导致下次不再重试”的状态机问题。驱动预检的 1115 不再有机会在 auto-start 路径上消费持久意图。

## 3. 验收

### 3.1 本机模型测试

```text
ctest -R "^m5\.service_host$|^m5\.service_orchestration$|^m5\.service_maintenance$|^m5\.execution_scheduler$|^unit\.m5\.worker_protocol$"
5/5 passed
```

新增用例：

```text
m5_host_keeps_the_running_intent_when_auto_start_runs_during_shutdown
```

断言：shutdown 探针为 true 时 `host.start()` 成功、`auto_started_operations=0`、没有派发
operation，且 profile 的 durable runtime 仍是 `running`。

`cmake --build ... --clean-first`：**453/453 成功，无编译 warning/error**。

### 3.2 客机（myrd-vm）1.0.4 自动拉起

请求 `req31339f48bad0`（安装/准备）、`req879d70d25acf`（重启后验证）：

```text
安装：myrd-cli 1.0.4 / MyRamDiskService 1.0.4
盘：  6f685e69-ae79-48bd-9bc1-fc57274fc58c  T:  1 GiB fixed
写：  16,777,216 B
hash：8FC208BE964FC1AD2DAA2BCFAD283DFF85A7E57ACF2C4898C86B5B7DAAD5FE0A
```

重启客机后：

```text
6f685e69-…  T:  Ready  running  runtime=Running  activity=Idle
persistence=Dirty  health=Healthy  generation=2
file size=16,777,216
sha256=8FC208BE964FC1AD2DAA2BCFAD283DFF85A7E57ACF2C4898C86B5B7DAAD5FE0A
AUTO_START_OK
```

说明 1.0.4 在正常重启路径上能自动拉起并保留已 checkpoint 的文件。

## 4. 未覆盖与边界

- 本次是**定向验收**，没有重跑完整的 A1/A2/B/fault 矩阵；1.0.3 的发布证据仍是那些门禁的
  基线。若要把 1.0.4 作为正式发布，仍需完整门禁、manifest 冻结和打包。
- 该修复针对**关机窗口内的 auto-start**。如果 auto-start 在其他非关机原因下失败（例如磁盘
  bundle 损坏、驱动缺失），当前产品仍可能停在 stopped/needs-attention；这类失败是否需要
  跨启动重试，是下一步设计问题。
- 未修改 `phdskmnt`；客机/宿主驱动均为 1.2.28.93。
- 1.0.4 候选未 tag、未 push、未发布。
