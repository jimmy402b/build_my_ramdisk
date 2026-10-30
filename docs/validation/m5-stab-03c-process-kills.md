# STAB-03c / R-04：worker / service 在 P4 窗口内被杀 —— **A、C PASSED；B 被产品缺陷阻塞**

**结论分三段，不要合并读：**

1. **R-04.A（P4 窗口内杀 worker）PASSED。** 修复后的产品在三次独立客机运行中给出同一读数：
   kill 后服务立刻应答（`disk list` 11–25 ms）、operation 停在设计上的 `reconciling`、worker 残留 0、
   `persistence=CommitIndeterminate`；服务重启后恢复把 operation 推到 **`succeeded`**，磁盘回到
   **候选代 generation=3** 并 `Running`，P4 前提交的 marker 字节完整。
2. **R-04.C（连续 3 次硬断电）PASSED。** `Test-M5RepeatedPowerLoss.ps1 -Cycles 3` → exit 0、
   1037.7 s，三轮 prepare/cut/verify 全 PASS，每轮后无残留（§6）。
3. **R-04.B（P4 窗口内杀服务）被产品缺陷阻塞。** 服务被强杀后，停在 P4 的 **worker 仍然活着**
   （`FAIL: b left 1 worker(s) behind after 60 s`），继续持有 bundle 锁与 AIM 设备——job object
   没有把 worker 带走。这是需要修产品的缺陷，不是测试问题（§4.1）。
4. **R-02（P4/P5 定点断电）未测。** 装置（P4 暂停 + prepare/verify + 主机驱动）已落地；它落在这条
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

## 4. R-04.B 为什么还没测（具体条件）

**B 现在是独立用例**：`integration.m5.service_kill`（`test-m5-process-kills.ps1 -Part B`），
不再和 A 串在一次运行里——A 或 B 任一 kill 之后，客机存储栈都需要硬重启，且恢复过的卷不能当
下一次实验的对象。

### 4.1 B 的实测阻塞：杀掉服务后 **worker 还活着**

请求 `c131cb7c…`（`--Part B`，1/1 FAIL，81 s）：

```text
READING: pause_marker=p4-commit generation=3 pid=1572
READING: candidate_generation=3
READING: killed_service_pid=3944
FAIL: b left 1 worker(s) behind after 60 s
```

也就是说：服务进程被强杀、worker 正停在 P4 的暂停里，60 秒后 worker **仍然存在**。这说明
服务给 worker 的 **job object 没有在服务进程死亡时把它带走**（或者 worker 从 job 里逃逸了）。
后果是真实的：残留 worker 继续持有 bundle 的独占写锁与 AIM 设备，下一次 start/forget 会被它挡住，
这正是 R-04 要防的“残留”。**这是 R-04.B 的阻塞点，需要修产品（job object 的 kill-on-close /
breakaway 语义），不是改测试。**

### 4.2 另一个观察：A 的恢复之后 `disk stop` 终态 failed

早先 A+B 串跑的版本里，A 恢复后对同一块盘 `disk stop`，operation 被准入
（`b.part_a_stop_exit=0`）但终态 `failed`。这与 4.1 不同：那次 worker 已被杀干净，stop 仍然失败，
说明恢复后的 durable persistence 可能仍停在 indeterminate 一类。现在 B 已拆成独立用例，这个
观察暂时不阻塞 R-04；等 4.1 修好后，用独立用例把它查清（读 `kind=7` 的 code/step）。

### 4.3 客机在 kill 实验后退化

每次跑完 kill 类用例，客机的存储栈/SSH 会退化（PowerShell over SSH 挂起、`cmd` 也超时），
必须 `vmrun stop … hard` + 启动才能跑下一次。这是已知代价（lessons §5.2），所以 kill 类用例
**不能靠 `Reset-M5Guest` 串在同一组里跑**：A 与 B 各自一个组，且组前应硬重启（当前驱动还是
reset；把 B 放进门禁前，先给 fault 组加“组前硬重启”）。

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

## 6. R-04.C：连续 3 次硬断电 —— **PASSED**

`tools/m5vm/Test-M5RepeatedPowerLoss.ps1 -SkipStage -Cycles 3` → **exit 0，1037.7 s**。每轮先
`Reset-M5Guest`，再跑 `Test-M5PowerLoss.ps1`（prepare → `vmrun stop … hard` → 启动 → verify）：

| 轮 | prepare | 断电 | verify | verify 后的残留 |
|---|---|---|---|---|
| 1 | **1/1 passed** | hard | **1/1 passed** | `workers=0 services=0 disks=1` |
| 2 | **1/1 passed** | hard（客机第一次没答 SSH，按驱动约定再 power-cycle 一次） | **1/1 passed** | `workers=0 services=0 disks=1` |
| 3 | **1/1 passed** | hard | **1/1 passed** | `workers=0 services=0 disks=1` |

每轮的判定来自 `PASS integration.m5.powerloss: the disk came back by itself after a hard power
cut`，其内部断言是：盘由 SCM 自动拉起、恢复到最后一提交代、committed marker 内容一致。
`disks=1` 是宿主 NVMe；AIM 盘与 worker 都没有残留。三轮 `PASS` 说明“连续 3 次”没有累积残留。

**驱动的已知瑕疵（不影响判定）**：汇总表里的 `committed generation` 打印 `?`——正则只看了
`Test-M5PowerLoss.ps1` 的顶层输出，而 “committed generation: N” 在嵌套的 prepare 记录里。
generation 本身由 powerloss_verify 断言，下一轮把正则修到嵌套输出即可。

