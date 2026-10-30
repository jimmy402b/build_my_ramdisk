# STAB-03c / R-04：worker / service 在 P4 窗口内被杀 —— **A PASSED（缺陷已修）；B/C 未测**

**结论分两段，不要合并读：**

1. **R-04.A（P4 窗口内杀 worker）PASSED。** 修复后的产品在三次独立客机运行中给出同一读数：
   kill 后服务立刻应答（`disk list` 11–25 ms）、operation 停在设计上的 `reconciling`、worker 残留 0、
   `persistence=CommitIndeterminate`；服务重启后恢复把 operation 推到 **`succeeded`**，磁盘回到
   **候选代 generation=3** 并 `Running`，P4 前提交的 marker 字节完整。
2. **R-04.B（P4 窗口内杀服务）与 R-04.C（连续 3 次硬断电）未测。** B 当前卡在两个具体条件上：
   ① 恢复后的盘 `disk stop` 返回 `failed`（原因尚未提取，见 §4.1）；
   ② 每次 kill 类实验之后客机存储栈/SSH 会退化，必须硬重启才能跑下一次（§4.2）。
   C 的驱动 `tools/m5vm/Test-M5RepeatedPowerLoss.ps1` 已写，**没有跑过**。
3. **R-02（P4/P5 定点断电）未测。** 装置（P4 暂停 + prepare/verify + 主机驱动）已落地；它落在这条
   已修好的恢复路径上，但**没有读数，不写成通过**。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每次运行前 reset（kill 类实验前硬重启）。

---

## 1. 做了什么

### 1.1 可重复的 P4 窗口（产品里的测试 seam）

`src/host/m5_worker_runtime.cpp` 在 `commit_checkpoint` 拿到“commit 已落地”之后、写回复之前：

| 变量 | 作用 |
|---|---|
| `MYRD_M5_TEST_PAUSE_AT=p4-commit` | 打开暂停 |
| `MYRD_M5_TEST_PAUSE_FILE=<path>` | 写 P4 marker（含 `generation=`、pid），然后暂停 |
| `MYRD_M5_TEST_PAUSE_SECONDS=<n>` | 有界暂停（默认 180，上限 3600） |
| `MYRD_M5_TEST_PAUSE_ARM=<path>` | 可选：该文件存在时才暂停 |

**为什么要 ARM**：`disk create` 自己会做第一次 checkpoint。服务启动时就武装暂停，会把 **create**
冻结在 P4（实测：create 在 `reconciling` 停满整个暂停时长，直到测试侧 300 s 超时）。ARM 让测试在
create 完成后再打开窗口，并在恢复前解除（`Stop-ServiceProcess` 删除 arm 文件），这样恢复的提交
不会再次停住。

暂停是**有界 sleep 循环**，marker 写失败则不暂停（“没有证据就不制造窗口”）。它编进产品，但只由
安装器不会设置的环境变量触发；R-09 审计时应记为“管理员可用的测试 seam”。

### 1.2 R-04 的客机用例

`tests/e2e/test-m5-process-kills.ps1`（`integration.m5.process_kills`，opt-in 组 `fault`）：

- **A**：create 64 MiB 盘 → 写 marker → `disk checkpoint --no-wait` → 等 P4 marker → 杀 worker →
  记录 operation 状态 / `disk list` 延迟 / 残留 worker / persistence → 重启服务 → 期望恢复把
  checkpoint 解到终态、盘回到候选代 → 校验数据。
- **B**：停掉 A 的盘 → 在第二个盘上重复同一窗口，这次杀**服务** → 验证 worker 不残留 → 重启后恢复。

### 1.3 其它已落地但未测的装置

- `tests/e2e/test-m5-p4p5-powercut-prepare.ps1` / `-verify.ps1` +
  `tools/m5vm/Test-M5P4P5PowerCut.ps1`（R-02，`-Cycles N`）。
- `tools/m5vm/Test-M5RepeatedPowerLoss.ps1`（R-04.C，三次 `Test-M5PowerLoss.ps1`，每轮 reset）。
- `tests/e2e/test-m5-bundle-corruption.ps1`（R-03 第一部分：manifest 截断/翻位、base 截断、journal
  翻位；每次损坏后要求显式失败且 bundle 字节不变，再恢复文件证明同一块盘能重新启动）。
- 全量脚本：`-IncludeFaultMatrix`（R-04/R-03 的 opt-in 组）、`-IncludeMemoryExhaustion`（R-01）；
  `p4p5_*` 永不进默认选择（需要主机断电）。

## 2. 根因与修复（R-04.A）

**原症状**：kill 后重启，恢复执行把 checkpoint 重跑到 P5，`publish_checkpoint_epoch` 返回
`0x5C02 execution_not_bound`（`step=service_worker_control.require_session`，
`detail=No runtime is bound to this profile to adopt`），`execute_claimed_operation` 只写一条
`operation_failed` 日志，**不把 operation 终态化**，于是它永远 `running`（实测 ≥300 s，每次重启复现）。

**根因（定位到函数）**：恢复一个已过 P4 的 checkpoint 时，`execute_operation` 走
`recovering_checkpoint_from_m3` 分支，用 `worker_.reconcile_checkpoint(...)` 做**只读 inspection**
（`ServiceWorkerControl::reconcile_checkpoint`）。inspection 回答完即关闭（代码里的注释写明“关闭通道
就是结束 inspection”），所以**没有绑定 session**。随后 P5 的 `publish_checkpoint_epoch` 在
`require_session(..., true)` 上找不到可采纳的 runtime，返回 `execution_not_bound`；失败沿
`execute_operation` 冒泡，operation 停在 `running`。

**修复**（`src/management/service_core.cpp`）：P5 的发布体抽成局部 `publish_epoch`；
当 `recovering_checkpoint_from_m3`（或走 in-flight `reconcile_checkpoint` 的那条 indeterminate 路径）
为真时，直接用**inspection 已经证明的** `evidence.correlation` / `observed_dirty_epoch` 调它，而不
再向 worker 要一次 epoch 观测；`post_commit_dirty=false`（只读 inspection 不可能持有未提交写）。
这不是放宽校验：证明来自同一 bundle、同一 correlation，且 `valid_checkpoint_evidence(..., true)` 已在
inspection 返回处校验过。

**为什么不是“忽略错误”**：只对“证据来自 inspection”这一种形态走直发；正常路径（有绑定 session）
仍是原来的 `publish_checkpoint_epoch`，包括 correlation 比对与 cleanliness 判定。

## 3. 客机读数（R-04.A，修复后三次独立运行）

三次运行的形态完全一致，摘录最后一次（请求 `9c67b095…`）：

```text
READING: a.pause_marker=p4-commit generation=3 pid=3320
READING: a.candidate_generation=3
READING: a.killed_worker_pid=3320
READING: a.operation_state_after_kill=reconciling
READING: a.list_exit=0
READING: a.list_ms=13
READING: a.runtime_after_kill=Running
READING: a.persistence_after_kill=CommitIndeterminate
READING: a.workers_left=0
--- A: restart the service and let its recovery resolve the checkpoint
READING: a.operation_state_after_restart=succeeded
READING: a.start_exit=0
READING: a.generation_after_recovery=3
READING: a.data_intact=yes
```

| 判据 | 读数 |
|---|---|
| kill 不挂死服务 | `disk list` 11–25 ms，exit 0 |
| operation 有确定答案 | kill 后 `reconciling`（设计上的“不知道”），重启后 `succeeded` |
| 无残留 | `workers_left=0` |
| 重启可恢复 | `start_exit=0`、`generation=3`（= P4 候选）、`Running` |
| 已提交数据不丢 | `data_intact=yes` |

同一形态在请求 `6215a0f3…`、`4ea930fd…` 的两次运行里重复出现（`a.operation_state_after_restart=succeeded`）。

## 4. B/C 为什么还没测（具体条件）

### 4.1 B：恢复后的盘 `disk stop` 返回 failed

用例在 A 之后先 `disk stop` 第一块盘，operation 被准入（`b.part_a_stop_exit=0`）但终态
**`b.part_a_stop_state=failed`**，用例据此失败。这说明：

- A 的恢复把盘带到了 `Running`/候选代，但该盘的持久化状态可能仍是 `CommitIndeterminate` 一类，
  于是紧随其后的 stop 在内部检查上失败；
- 这是**另一个待查的产品问题**（不是测试装置问题）：一次成功的恢复之后，盘应当可以被正常停止。
  下一步要读 `C:\m5\last-service.log` 的 `kind=7` 行拿 stop 的 code/step（本轮客机在测试收尾后
  SSH 退化，没能取到）。

### 4.2 客机在 kill 实验后退化

每次跑完 kill 类用例，客机的存储栈/SSH 会退化（PowerShell over SSH 挂起、`cmd` 也超时），
必须 `vmrun stop … hard` + 启动才能跑下一次。这是已知代价（lessons §5.2），但它意味着：
**kill 类用例不能靠 `Reset-M5Guest` 串在同一组里跑**，每组前必须硬重启。全量脚本目前只在组间
做 reset，不做重启；在把 R-04.B/C 放进任何门禁之前，先要有一个“每组前硬重启”的驱动（或把 B/C
放在独立的 host 驱动里，像 powerloss 那样）。

## 5. 复现

```powershell
# A（需要一块刚硬重启并 reset 过的客机）
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^integration\.m5\.process_kills$' -MinTimeoutSeconds 900

# C（已写未跑）
tools/m5vm/Test-M5RepeatedPowerLoss.ps1 -SkipStage -Cycles 3

# R-02（已写未跑）
tools/m5vm/Test-M5P4P5PowerCut.ps1 -SkipStage -Cycles 3
```

失败时读 `C:\m5\last-service.log` 的 `kind=7` 行与 `C:\m5\process-kill-*\trace-a2.log`。
