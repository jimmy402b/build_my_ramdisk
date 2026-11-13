# MyRamDisk 1.0.4 发布说明（2026-11-13，候选）

**产品**：Windows 11 x64 单盘 RAM disk。
**版本**：`1.0.4`（四个二进制自报 `1.0.4`）。
**状态**：**已发布 2026-11-13（tag `v1.0.4`）；未签名。**
**范围**：只修 1.0.3 的 auto-start 缺陷。按用户裁定，本版门禁只覆盖**自动拉起 harness**，
不包含 12h/24h 无崩溃 soak 或完整 A1/A2/B/fault 矩阵。

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

请求 `reqc9ef501dc62f`（安装/准备）、`reqe07403f07ca0`（重启后验证）：

```text
安装：myrd-cli 1.0.4 / MyRamDiskService 1.0.4
盘：  af3da520-39d1-4e9d-b252-7f61c7d514fc  T:  1 GiB fixed
写：  16,777,216 B
hash：2C08064F0DA3B4D5E8EF949E188F69F0D9875DC57815CEB3D62D8C694F1F04CC
```

重启客机后：

```text
af3da520-…  T:  Ready  running  runtime=Running  activity=Idle
persistence=Dirty  health=Healthy  generation=2
file size=16,777,216
sha256=2C08064F0DA3B4D5E8EF949E188F69F0D9875DC57815CEB3D62D8C694F1F04CC
AUTO_START_OK
```

说明 1.0.4 在正常重启路径上能自动拉起并保留已 checkpoint 的文件。

## 4. 发布门禁（按用户裁定的范围）

**1.0.4 只验证自动拉起 harness，不包含 12h/24h 无崩溃 soak，也不重跑完整 A1/A2/B/fault 矩阵。**
这是用户对补丁发布范围的明确裁定。

- 模型测试 `m5.service_host` 5/5，包括
  `m5_host_keeps_the_running_intent_when_auto_start_runs_during_shutdown`；
- 客机 1.0.4 自动拉起：1 GiB fixed `T:`，`auto_start=true`，16 MiB 文件
  SHA-256 `2C08064F…F04CC`，重启后 running 且哈希不变；
- 客机 1.0.4 的 unit/model/m1m4/m2/m3 分组连同 smoke 作为辅助读数；
- `--clean-first` 构建 453/453，无 warning/error。

**未覆盖**：B performance/cluster/support 的逐例重跑、fault 的 hostile_acl/volume_full、
powerloss、完整 A1/A2 suite、12h/24h soak。1.0.3 的这些证据仍是历史基线，但不属于本补丁
的门禁范围。

## 5. 冻结与打包

```text
manifest: out/m5-evidence-1.0.4-final/release-manifest.json
frozen_at: 2026-11-13T11:54:56Z
readings: 9
validator: accepted

package: out/release/MyRamDisk-1.0.4-win11-x64（11 files）
SHA256SUMS: 10 条，复算 0 mismatch

binary hashes:
  myrd-service.exe    672065cfbdad8e4c89e31b2a8b6bd004af705716d12f5992d056a530126c4a4c
  myrd-cli.exe        2e8b8ad53e75e3e7959315abb46ddc29a31db14f9e699aacc3532d9b37300964
  myrd-gui.exe        b2378c5e3c9d4e8bc534e1b0603c06bf56eb7657982582dcc0babfe5e067b60a
  myrd-m5-worker.exe  022e105a212f484e17c77d595ccb714997a3c94d416aadca04823d0a02738bc4
```

**已发布 2026-11-13，tag `v1.0.4`；分支和 tag 已 push，GitHub release 使用同一 tag/包。**
未签名。

