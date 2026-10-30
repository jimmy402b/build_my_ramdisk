# STAB-03c / R-04：worker / service 在 P4 窗口内被杀 —— **BLOCKED（产品恢复路径缺陷）**

**结论**：R-04 **未通过**，且阻塞点是产品缺陷，不是测试装置：

- 在 P4 窗口内杀掉 worker 后，**服务本身没有挂死**（`disk list` 14 ms 应答、operation 停在设计上的
  `reconciling`、worker 残留 0、`persistence=CommitIndeterminate`）——这部分读数成立。
- **服务重启后的恢复路径失败**：恢复执行重新跑到 P5，`publish_checkpoint_epoch` 返回
  `0x5C02`（`ServiceWorkerControlError::execution_not_bound`，"No runtime is bound to this profile
  to adopt"），operation 此后**永远停在 `running`**（实测 ≥300 s），磁盘无法自动回到可用状态。
  同一状态在每次服务重启时重复出现（诊断重启复现两次）。

因此 R-04 的验收句“operation 到终态；重启可恢复”**不成立**。R-02 依赖同一条恢复路径，**未测**，
在缺陷修复前不应声称通过。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release；每次运行前 `Reset-M5Guest`。

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
冻结在 P4（实测：create 在 `reconciling` 停满整个暂停时长）。ARM 让测试在 create 完成后再打开窗口。

暂停是**有界 sleep 循环**，marker 写失败则不暂停（“没有证据就不制造窗口”）。它编进产品，但只由
安装器不会设置的环境变量触发；R-09 审计时应把它记为“管理员可用的测试 seam”。

### 1.2 R-04 的客机用例

`tests/e2e/test-m5-process-kills.ps1`（`integration.m5.process_kills`）：

- **A**：create 64 MiB 盘 → 写 marker → `disk checkpoint --no-wait` → 等 P4 marker → 杀 worker →
  记录 operation 状态 / `disk list` 延迟 / 残留 worker / persistence → 重启服务 → 期望恢复把
  checkpoint 解到终态并把盘带回可启动 → 校验 generation 与 marker 字节。
- **B**：同样在 P4 杀**服务**，验证 worker 不残留、重启后的恢复可用。
- **C**（连续 3 次硬断电）不在这里：客机不能给自己断电。

### 1.3 R-02 的装置（已写，未取得读数）

- `tests/e2e/test-m5-p4p5-powercut-prepare.ps1`：在 P4 暂停、写状态文件、**退出时故意不清理**
  （服务和被暂停的 worker 留给主机断电）。
- `tests/e2e/test-m5-p4p5-powercut-verify.ps1`：断电重启后启动服务，断言恢复代 ∈
  {最后提交代, P4 候选代}，并校验候选代的 marker 字节。
- `tools/m5vm/Test-M5P4P5PowerCut.ps1`：prepare → `vmrun stop hard` → 启动 → 等 SSH → verify；
  每轮之间 `Reset-M5Guest`，支持 `-Cycles N`。

这三个文件与 `p4p5_prepare`/`p4p5_verify` 两个 ctest 项已经落地，但**没有跑过**（先后被同一个
恢复缺陷挡住）；任务状态保持 NOT_STARTED/BLOCKED，不写 PASSED。

## 2. 客机读数

### 2.1 杀 worker 之后（R-04.A 的前半，成立）

```text
READING: a.pause_marker=p4-commit generation=3 pid=5796
READING: a.candidate_generation=3
READING: a.killed_worker_pid=5796
READING: a.operation_state_after_kill=reconciling
READING: a.list_exit=0
READING: a.list_ms=14
READING: a.runtime_after_kill=Running
READING: a.persistence_after_kill=CommitIndeterminate
READING: a.workers_left=0
```

对着服务日志：`scheduler: execute returned` 在 kill 后立刻发生，说明执行线程没有被 worker 的死拖住；
operation 进入设计上的 `reconciling`（“不知道提交有没有落地，只有恢复路径能回答”），**不是挂死**。

### 2.2 服务重启之后（R-04.A 的后半，失败）

重启后的服务 trace（`trace-a2.log`）：

```text
worker: runtime open, serving
worker: command received / command handled / reply written
p5: evidence profile state published
p5: operations_.write begin / done
service: execute_operation returned a failure
scheduler: execute FAILED code=23554
```

`23554 = 0x5C02 = execution_not_bound`；服务日志给出完整原因：

```text
kind=7 ... execute_operation failed: code=23554 native=0
  step=service_worker_control.require_session
  detail=No runtime is bound to this profile to adopt
```

测试侧读数：

```text
READING: a.operation_state_after_restart  -> (never left running)
FAIL: the checkpoint after the service restart did not reach a terminal state within 300 s
      (last state: running)
```

**第二次诊断**（手动再启动服务）复现同一序列：`started, state=running recovered=2` →
`disk.checkpoint ... evidence-recorded profile state published` → 同一 `0x5C02`。说明不是一次性时序，
而是每次启动都重复的循环。

### 2.3 失败形态为什么重要

- operation 既不是终态、也不再前进：客户端 `operation wait` 会一直等到超时，`disk start` 也没有
  可靠入口（profile 停在 post-P4 的中间相）。
- 这正是 R-04 要防的“挂死/残留”：盘没有丢已提交数据（P4 已落地），但**产品不能自己把它带回可用**。

## 3. 根因定位（到函数，不到行号结论）

1. 恢复路径（`ServiceCore::recover_after_restart`）把非终态 checkpoint 操作重新放进 pending，
   scheduler 重新执行它；这条路径本身是设计的“重启恢复”。
2. 重新执行时，P4 的 durable correlation 已存在，动作重跑到 P5：先把
   `checkpoint.phase = evidence_recorded` 发布成功，然后调用
   `worker_.publish_checkpoint_epoch(worker_request, evidence.correlation, ...)`。
3. `publish_checkpoint_epoch` 内部走 `require_session(profile)`，要求“这个 profile 现在有一个
   **已绑定**的 worker session”。恢复执行用的 worker 是这次执行临时拉起/采纳的，**没有进绑定表**，
   于是返回 `execution_not_bound`。
4. 该失败从 `execute_operation` 直接冒泡到 scheduler；`execute_claimed_operation` 只写一条
   `operation_failed` 日志并返回错误，**没有把 operation 变成终态**，也没有把 profile 标成
   needs-attention，于是 operation 永远 `running`。

**修复方向（下一轮从这里接）**：P5 在无绑定 session 时应当走 T1-06 已有的
`reconcile_checkpoint`（只读 inspection worker 证明 candidate），用它证明 P4 之后发布 P5；或者让
恢复执行产生的 session 在本次动作链路内可被 P5 复用。两条都要补一条客机用例：
“杀 worker 于 P4 → 服务重启 → operation 终态 + 盘可 start + 数据在候选代”。
同时检查 `execute_claimed_operation` 在“P4 之后失败”时是否必须终态化/标 needs-attention，
避免任何未来错误重复出现“永久 running”。

## 4. 复现命令

```powershell
# 客机上（或经 harness）
tools/m5vm/Invoke-M5FullSuite.ps1 -SkipStage -IncludeFaultMatrix
# 或单例（先 Reset 客机）：
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^integration\.m5\.process_kills$' -MinTimeoutSeconds 900
```

失败时读 `C:\m5\last-service.log` 的 `kind=7` 行与 `C:\m5\process-kill-*\trace-a2.log`。

## 5. 明确没有覆盖的

- **R-04.B**（杀服务）与 **R-04.C**（连续 3 次硬断电）：脚本/驱动已写，但 A 失败后没有继续跑；
  B 是否同样卡在 `execution_not_bound` **未测**。
- **R-02**（P4/P5 定点断电）：装置已写，**未跑**；它落在同一条恢复路径上，修复前不应期待通过。
- 杀 worker 后**同一进程内**的即时恢复（不重启服务）：设计上唯一出口是 `resolve_commit`，本轮没有
  测量它的 CLI 入口（`disk repair-profile` 在服务起不来时也没能验证）。
