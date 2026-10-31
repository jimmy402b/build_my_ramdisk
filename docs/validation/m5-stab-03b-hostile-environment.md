# STAB-03b / R-03：敌意环境（bundle 损坏 / 卷满 / ACL）—— **PARTIAL（损坏矩阵卡在 stop 失败）**

**结论分段：**

1. **bundle 损坏矩阵（R-03 第 1 部分）测到一半，A/B 的“损坏必须被拒绝”成立，但被一个新的
   产品行为挡住**：恢复损坏文件、把盘重新启动后，`disk stop` 的 operation 终态 **`failed`**，
   随后 `--force` 也被拒，盘保持 Running、worker 持有设备，客机退化到只能硬重启。C（base 截断）
   与 D（journal 翻位）因此没跑。
2. **卷满（bundle 所在卷写满）与 ProgramData ACL 被改坏**：**未测**，连测试脚本都还没写。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每次运行前 `Reset-M5Guest`（损坏实验前硬重启）。

---

## 1. 已有读数（`integration.m5.bundle_corruption`，请求 `b0d6d8fa…`）

### A. manifest 截断到 32 字节 —— **行为正确**

```text
READING: bundle.disk.rdm=8192
READING: a.manifest_bytes=32
READING: a.start_exit=0            <- start 是异步准入：operation queued
READING: a.operation_state=failed  <- 操作终态是明确失败
READING: a.workers_left=0
READING: a.runtime=Stopped
READING: a.bundle_unchanged=yes    <- 失败没有改写/重建 bundle（逐文件 SHA-256 不变）
READING: a.restored_runtime=Running
READING: a.marker_intact=yes       <- 恢复原文件后同一块盘可启动，数据完整
READING: a.stop_state=succeeded
```

判定：截断的 manifest 让 start **明确失败**，不挂死、不留 worker、不静默重建；恢复文件后盘可正常
启动并读回已提交的 marker；stop 也成功。

### B. manifest 两个槽各翻一个字节 —— **拒绝正确，之后的 stop 失败**

`disk.rdm` 是双槽（每 4096 字节一代）；只翻一个槽时产品会用另一个完好槽，start 成功（这是正确的
回退，不是缺陷）。所以本用例同时翻两个槽（偏移 2048、6144）：

```text
READING: b.manifest_flip_offsets=2048,6144
READING: b.operation_state=failed
READING: b.workers_left=0
READING: b.bundle_unchanged=yes
READING: b.restored_runtime=Running
READING: b.marker_intact=yes
READING: b.stop_state=failed
READING: b.stop_output=... operation ...: running (phase admitting)
READING: b.forced_stop_state=refused
FAIL: b: the disk could not be stopped (plain: failed, forced: refused)
```

**这是新的产品阻塞点**：同一块盘在 A 里 stop 成功、在 B 的“恢复并重新启动”之后 stop 失败，且
`--force` 被状态机拒绝。客机随后退化（PowerShell over SSH / `cmd` 挂起），只能硬重启；服务日志
在强制结束前没留下 `kind=7`（`service.log` 为空），因此**原因尚未定性**。

**第二次复现（请求 `beac3cfd…`，带诊断 dump）把范围缩小了**：

```text
b.stop_state=failed
b.runtime_after_failed_stop=Running
--- operation show
  operation c0e89e06-…: failed (phase terminal)
--- disk list line
  1768a597-…  T:  Ready  running  runtime=Running activity=Idle
              persistence=Clean health=Healthy generation=5 mem=1048576/67108864
--- trace tail
  （只有每秒一次的 worker status 轮询，来自测试的 disk list；
   没有任何 quiesce/freeze/flush/remove/stop 的 worker 动作）
```

两个新事实：

1. **stop 在碰 worker 之前就失败了**：盘仍 `Running`、`generation=5` 不变、`persistence=Clean`，
   trace 里没有任何 stop 计划的 worker 动作（quiesce/freeze/flush/remove/stop 一个都没有）。
   失败点在 `execute_operation` 的前置/第一个动作（`quiesce_mutations` 或绑定校验），不是
   checkpoint 写失败。
2. **之后 `--force` 被拒**：很可能是失败的那次 stop 仍持着 profile 的 operation gate，于是
   force_stop 在准入时被判 `state_rejected`。也就是说“失败的操作没有把 gate 放掉”本身是第二个问题。

**服务日志为什么读不到**：该用例把 console service 的 stderr 重定向到 `workRoot\service.log`，
但失败后用 `Get-ChildItem -File` 只看得到 `trace.log`（`service.log`、`service.out.log` 都不存在），
所以 `kind=7` 无从取。下一轮先修这个观测缺口（例如让 service 用 `cmd /c ... > log 2>&1` 启动，
或确认 Start-Process 的重定向在 harness 会话里确实生效），再做一次干净复现。

**下一轮必须做的**：用一次干净复现拿到 `kind=7` 的 code/step；同时看失败 stop 的 gate 是否被释放
（`disk show`/日志里的 operation_gate）。在定性之前，C/D 与“卷满”用例都不应继续在这条状态上叠。

## 2. 已经写好的东西

- `tests/e2e/test-m5-bundle-corruption.ps1`（`integration.m5.bundle_corruption`，opt-in `fault` 组）：
  A manifest 截断；B 双槽翻位；C base 截断；D journal 头+尾翻位。每个 case 的判据：
  ① start 的 operation 终态 `failed`（或准入前 exit 1）；
  ② 无 worker 残留、盘不显示 Running；
  ③ bundle 逐文件长度+SHA-256 不变（不静默重建/重格式化）；
  ④ 恢复文件后同一块盘可启动、marker 字节一致、能正常 stop。
- 用例里已加 `Stop-DiskForNextCase`：普通 stop 失败时尝试 `--force`、再按可观测量（runtime）判定，
  并在仍然 Running 时**先把 operation show / disk list / trace tail dump 出来再失败**——之前两次
  失败都被 harness 先杀掉服务，导致没有证据可读。
- 另外新增 `integration.m5.stop_after_restore`（`tests/e2e/test-m5-stop-after-restore.ps1`）：
  单次“损坏→拒→修复→启动→stop”闭环。它单独跑 **1/1 PASS**，说明单次修复后 stop 是好的；
  R-03 的 B 是**第二轮**修复后才失败，所以触发条件与“重复修复/启动”有关，而不是修复本身。

## 3. 未测的部分（明确写出来）

- **C/D**：等 B 的 stop 失败定性/修复后再跑。
- **bundle 卷满**：未写脚本。计划是一个 64 MiB 的 VHD 挂成 V:，bundle 放上去，填满到只剩几 MiB，
  触发 checkpoint/写入失败；期望明确失败、最后提交代仍可恢复，腾出空间后恢复。需要 diskpart 建
  VHD，属于会创建真实卷的操作，只能在客机上做。
- **ProgramData ACL 被改坏**：未写脚本。计划是去掉 `C:\ProgramData\MyRamDisk` 上 SYSTEM 的访问，
  重启服务，期望服务启动明确报错（不是静默降级）；恢复 ACL 后服务恢复。
