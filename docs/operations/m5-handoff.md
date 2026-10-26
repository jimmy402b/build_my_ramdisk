# M5 交接：从这里开始

**最后更新：2026-10-26**（状态有变化先改这里，再改 tasklist）

---

## 1. 一分钟现状

| 项 | 值 |
|---|---|
| **唯一活动线** | 分支 `feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`（HEAD 以 `git log` 为准，本文不钉 SHA） |
| 基线 | `main` = `cb3df08`（干净 M4），**保持不动** |
| 单盘（G2/G3） | T1-01 / T1-02 / T1-03 / T1-04 / T1-11 **PASSED**；**T1-05 PARTIAL** |
| 多盘（G4） | T2-01…T2-03 **整章延后**（2026-10-17 用户裁定） |
| 产品面客机读数 | 完整 152 条 → **150 通过**，唯二失败是客机无 Visual Studio 的两条 structural |
| **下一件事** | `create` 在注册步骤失败（本文 §3.3）——stop 已修但还没验收 |

**顺序**：T1-05 收尾 → T1-06 → T1-07 → T1-08 → T1-09 → T1-10 → T3（GUI）→ T4（产品化）。
T2 在 G4 恢复前不动。

---

## 2. 文档地图（按"你想知道什么"）

| 想知道 | 读这份 |
|---|---|
| **现在在哪、下一步做什么** | **本文** |
| 每个增量的状态 | `docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`（**唯一状态来源**） |
| 顺序、依赖、验收口径、风险、何时停下问 | `docs/superpowers/plans/2026-10-15-m5-master-plan.md` |
| 设计与不变量 | `docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md` |
| 某个增量凭什么算通过 | `docs/validation/`（按增量名对号） |
| **已经付过学费的坑** | `docs/operations/m5-lessons.md`（**动手前扫一遍**） |
| 2026-10-26 那次评审的结论与修法 | `docs/operations/m5-review-2026-10-26.md`（时点快照，写时固定） |
| VM 回路的三条通道与一次性配置 | `docs/operations/m5-vm-harness.md` |
| 引擎层（M3/M4）怎么跑 | `docs/operations/m3-persistence.md`、`docs/testing/m3-e2e.md` |

**文档纪律**：状态只写在 tasklist；本文写"下一步"；master-plan 不再写"当前位置"。
任何一处与前两处冲突，以 tasklist → 本文 → master-plan 的顺序取舍。

---

## 3. 当前增量 T1-05：`stop` 已修，验收被 `create` 的新失败挡住

### 3.1 已经通的那一段（客机读数，2026-10-23，请求 `32d4ba1c71b54d72a87c4d4e6cd33203`）

```text
disk.create:     queued → running → … → publish-ready → assign-mount → readiness-io
disk.checkpoint: queued → running → succeeded (terminal)
```

`create` 全链、`list`、`show`、读写、`checkpoint` 都通过了——**这些此前从未跑通过**。
详细过程与六个被推翻的假设见 `docs/validation/m5-t1-05-cli.md` §8–§10。

### 3.2 原来是这里卡住（2026-10-26 已修，`d5b9ad7`）

```text
execute_operation failed: code=22022 step=service_core.checkpoint_epoch
  detail=The final checkpoint state is invalid
```

位置：`src/management/service_core.cpp:5753`，在 checkpoint 确认回调里
用 `complete_command(pending_plan, succeeded_clean|succeeded_dirty)` 去**完成整个 stop 计划**。

读代码确认的两个事实：

1. **`stop` 不接受 `succeeded_dirty`。**
   `src/management/state_machine.cpp:740` 的 `stop` 分支只认
   `succeeded_clean` / `transient_failure` / `persistence_blocked` /
   `commit_indeterminate` / `committed_needs_reconciliation_*`；
   `succeeded_dirty` 落到 `break` ⇒ `invalid_completion` ⇒ 就是上面这条 22022。
2. **这一步本来也不该完成整个命令。**
   stop 计划在 checkpoint 之后还有 `remove_mount` / `remove_exact_device` /
   `stop_runtime_and_release_lock`；终点在 `finalize_operation`（`service_core.cpp:6795`）。

### 3.3 下一步（按顺序，不要跳）

1. **根因已查明（2026-10-26）：dirty epoch 是一条两端都空着的管道，不是判据写错。**
   - worker 侧的 `cut_dirty_epoch` 取的是 `maintenance_stats().generation`
     （backing generation，`m5_worker_runtime.cpp:519`）；
   - service 侧的 `live_dirty_epoch` 是**写死的 0**（`service_worker_control.cpp` 的
     `publish_checkpoint_epoch`）；
   - `WorkerRuntimeStatus.dirty_epoch` 全仓**没有生产者**，恒为 0。
   ⇒ `clean = (0 == generation)`：generation 0 时成立（create 的第一处 checkpoint），
   之后永不成立（stop）。**这是确定性的，不是间发。**
   完整证据链与五步修法见 [`m5-review-2026-10-26.md`](m5-review-2026-10-26.md) §1。
   **不要再往"比较顺序"上调参**——`dbb25a3` 已经证明那条路只能让 create 看起来好转。
2. **`normal_stop` 时不要在这一步 `complete_command`**：只发布"checkpoint 已确认"的
   durable 状态（`phase = stable`、`disposition = committed`、
   `confirmed_generation`、`current_dirty_epoch`），让 remove 系列继续，
   由 `finalize_operation` 用 stop 的真实 outcome 收尾。
3. **不要把"接受 dirty"当成修法（2026-10-26 更正）**：写冻结排在 checkpoint 之前，
   "提交后还有脏块"意味着**冻结漏了写**——那是缺陷，报成功等于把数据丢失伪装成正常收尾。
   已按此实现并加模型用例：clean 成功、dirty 不 applied。
4. **验收**（客机）：`disk stop` → 盘符消失 → 重启服务 → `disk start` 恢复到最后
   generation → `disk forget`。修复类结论**至少 `-Repeat 20`**。

### 3.4 顺带记下的两个可用性缺口（不阻塞 T1-05）

- `disk show/stop/checkpoint` 的 `<disk>` 参数只认 display name 或 uuid，**不认盘符**，
  而用户会自然想用盘符（本轮排查就撞上过 `profile_not_found`）；
- 失败原因的可达性已补（`m5-t1-05-cli.md` §5.2），但 `OperationRecord` 仍只存标记串。

### 3.5 2026-10-26 晚：create 通了，stop 也通了；现在挡在 `forget`

**create 的注册失败已修（`2e8cf32`）**：`ensure_profile_unique` 会打开**每一个其它 profile** 的
bundle 去比 file identity，并把打开失败原样上抛。客机上有一个 01:59 留下的 profile
（`c084bc42`，bundle 已被上一轮 teardown 删掉），于是**之后每一次 create 都失败**，
而错误里报的是**新 bundle** 的路径。四次运行的失败路径完全相同（`b6707f16…`），
而新 bundle 当时确实存在、用户进程用同样的 flags 能打开——这就是判据。
现在改成：打不开的其它 bundle 不可能等于正在注册的那个（identity 相等要两边都打开），
前四项检查（名字 / image / 路径 / 挂载点）已经覆盖真实冲突，所以 `continue`。

**读到的链路（同一次运行，`disk.list` 停后仍可见）**：

```text
create → list → show → 读写 73 字节 → checkpoint succeeded → stop succeeded
停后：V: ready stopped/configured runtime=Stopped persistence=Clean generation=4
```

**现在挡在 `disk forget`**：

```text
kind=7 command=disk.forget phase=terminal
  message=terminal failure without a worker-action error; marker=unsafe
```

`ServiceWorkerControl` **没有实现** `prove_forget_safe`，走的是 `IWorkerControl` 的默认实现
（`service_core.cpp:1390`，直接返回 "The worker control does not expose forget safety proof"）。
而 worker 协议里**没有任何 forget 相关的命令**（`m5_worker_protocol.h` 全文无 forget）。
⇒ **`disk forget` 是未实现，不是回归**；fail-closed 的语义让它看起来"安全地拒绝"，
所以一直没被发现。

**为什么不能简单在服务里补**：`myrd_management` 只链 profile/product_ipc/contracts/
worker_protocol/common（`src/CMakeLists.txt:372`），拿不到 writer lock（persistence）和
AIM 设备（driver）。proof 的五项里有两项必须由 worker 来答 ⇒ 要么加一个 worker 命令，
要么先做 T1-06 的只读检查 worker。

**下一步（按顺序）**：① 给 worker 协议加一个只读的 forget 证明命令（bundle 存在 + file identity、
writer lock 可取、匹配设备不存在）；② 服务侧实现 `prove_forget_safe` 并接到 tombstone 校验；
③ 客机跑 `-Repeat 20`。
---

## 4. 构建与测量循环

```powershell
# 构建（改过任何头文件 ⇒ 必须加 --clean-first，见 lessons §1）
cmake --build out/build/msvc --config Release --clean-first

# VM 回路（先在开发机构建，再送进客机；执行只在客机）
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
tools/m5vm/Invoke-M5Vm.ps1 -Probe
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release
tools/m5vm/Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 180
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '<用例名>' -Repeat 20
tools/m5vm/Invoke-M5Vm.ps1 -Run -RequestId <id>
```

整套一遍约 9–11 分钟。**`-Stage` 不是可选的**：二进制在开发机构建，执行只在客机。
**`-Stage` 失败会留下脏客机**（孤儿 worker + 多余盘 + 残留盘符）⇒ 先硬重启再重试。

---

## 5. 环境事实

| 项 | 值 |
|---|---|
| 客机 | VMware Workstation，Windows 11 x64，MAC `00:0C:29:02:90:C1` |
| 地址 | `192.168.180.129`，SSH 别名 **`myrd-vm`**（脚本一律传别名，不硬编码地址） |
| 客机账户 | `usr` / `123`；密钥 `~\.ssh\id_ed25519` |
| VMX | `C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx` |
| vmrun | **`-T ws` 必须是第一个参数**；VM 加密，每条都要 `-vp 12345678` |
| 驱动 | `phdskmnt` 必须 Running；**不得安装/替换/升级/卸载** |

**一条与仓库无关、但会浪费时间的工具事实**：本会话的 agent shell 在
`workspace-write` 沙箱下**每个子进程都以 `0xC0000142`（`STATUS_DLL_INIT_FAILED`）退出**，
连 `Get-Process` 都起不来；切到 `danger-full-access` 后正常。
**看到这个错误码先怀疑沙箱模式，不要怀疑构建或仓库。**

---

## 6. 待裁决（只有这些需要停下来问）

| # | 事项 | 状态 |
|---|---|---|
| 1 | Primo "Mini Tools" 的范围定义 | **未定义** ⇒ 不列入范围 |
| 2 | T4-02 单文件 VHDX 导出 | 已裁决：保留 |
| 3 | 完成口径 | **已裁决（2026-10-17）**：G1 + G2 + G3 全部验收 + T4-04 manifest 冻结 + §8 遗留清空；G4 延后 |
| 4 | G4 何时恢复 | 未定；恢复前置条件见 master-plan §6 |

其余（含 T1-05 的 `stop` 语义）都在实现权限内，**自主推进，不要反复问**。

---

## 7. 硬性规则（摘四条，全文见 `m5-lessons.md`）

1. 改任何头文件之后、测量之前必须 `--clean-first`；否则得到 `0xC0000005` 假崩溃。
2. **单次绿是弱证据**；修复类结论 ≥ `-Repeat 20`，任何"绿"的声明必须写明跑了几次。
3. 开发机只跑**不创建真实 worker / AIM 盘 / Windows 卷**的测试；不确定时按"只能上 VM"。
4. 断言失败必须打印 `operation / message / native / code`——一次客机往返 5–30 分钟，
   盲猜的代价是整轮。

---

## 8. 历史（已归档，不要再引用）

`feature/m5-unified-integration`、`feature/m5-supervisor-service` 是 2026-09 的两条实现线，
已被 v3 取代；工作树与分支已于 2026-10-25 删除，仅以 tag
`archive/m5-unified-integration`、`archive/m5-supervisor-service` 保留历史。
**当前代码里没有任何东西依赖它们**；`docs/m4-m5-scope.md` 里对它们的描述属于历史背景。
