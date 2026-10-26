# M5 交接：从这里开始

**最后更新：2026-10-26**（状态有变化先改这里，再改 tasklist）

---

## 1. 一分钟现状

| 项 | 值 |
|---|---|
| **唯一活动线** | 分支 `feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`（HEAD 以 `git log` 为准，本文不钉 SHA） |
| 基线 | `main` = `cb3df08`（干净 M4），**保持不动** |
| 单盘（G2/G3） | T1-01 / T1-02 / T1-03 / T1-04 / T1-11 **PASSED**；**T1-05 可判 PASSED**（`cli_e2e -Repeat 20 → 20/20`，本文 §3.14） |
| 多盘（G4） | T2-01…T2-03 **整章延后**（2026-10-17 用户裁定） |
| 产品面客机读数 | 完整 154 条 → **151/154**；两个是客机无 Visual Studio 的 structural，一个是批内间发（单跑 3/3 通过）。见 §3.14 ⑥ |
| **下一件事** | T1-06（中断提交判定可达）。stop/restart 的收尾已在 §3.14 完成 |

**顺序**：T1-05 收尾（**已完成，§3.14**）→ T1-06 → T1-07 → T1-08 → T1-09 → T1-10 → T3（GUI）→ T4（产品化）。
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

**stop 的重复读数（2026-10-26 晚）**：`m5.production_execution_backend`（含 stop 路径）`-Repeat 20` → **20/20 通过**，请求 `a21cf2191a9d4b10bab4727e43a7737a`。这是 stop 修复的稳定证据；`cli_e2e` 整体仍卡在 forget。

**下一步（按顺序）**：① 给 worker 协议加一个只读的 forget 证明命令（bundle 存在 + file identity、
writer lock 可取、匹配设备不存在）；② 服务侧实现 `prove_forget_safe` 并接到 tombstone 校验；
③ 客机跑 `-Repeat 20`。
---

### 3.6 2026-10-26 晚：forget 通了，现在挡在"重启"

**forget 已实现（`c1ac06f`）**：worker 在 `inspect_bundle` 里证明"writer lock 可取 + 没有同尺寸
代理设备"，服务侧 `prove_forget_safe` 用一次 inspection 拿这两项并等进程退出。
客机读数：`disk.forget: succeeded (phase terminal)`。

**e2e 现在过了整条数据面**：create → list/show → 读写 → checkpoint → stop → forget 全通过，
失败点移到 **installed service** 阶段：

```text
installed service state: STOPPED   WIN32_EXIT_CODE 1066
sc.exe stop MyRamDiskService failed (1062): 服务尚未启动
```

**根因（已复现）**：一次 forget 之后 `C:\ProgramData\MyRamDisk\profiles\<id>\` 留下**空目录**，
`profiles\workers\<id>.workers` 也留着；**下一次服务启动直接失败**：

```text
myrd-service: start failed: op=profile_registration.load
  msg=The requested profile is not registered native=0
```

空目录 `list_profiles` 会跳过，所以嫌疑在 worker binding / 启动协调路径。
**下一步**：① forget 连目录与 binding 一起清掉（治源头）；② 启动路径把"已 forget 的 profile"
当作可回收残留而不是致命错误（崩溃中途也安全）；③ 然后跑 `cli_e2e -Repeat 20`。

---


### 3.7 2026-10-26 晚：cli_e2e 首次整套通过；重复 20 次暴露"第 5 次开始卡在 reconciling"

**单次通过（第一次）**：`integration.m5.cli_e2e` → **1 of 1 passed**，请求
`25e8077d130d42fdbe8c12d8e6b40509`。覆盖 create → list/show → 读写 → checkpoint →
stop → 注册仍在 → forget → installed service（SCM 安装/应答/停止/删除）。

**`-Repeat 20`：4/20**（请求 `8446da51fcaf43c0a36c0d0e240f73cf`）。前 4 次通过，
从第 5 次起全部失败，失败读数一致：

```text
FAIL: the operation did not reach a terminal state within 300 s (last state: reconciling)
WARNING: 1 worker process(es) survived the run
```

即 **stop 进入 reconciling 后 300 s 没有出来**，并且留下一个 worker 进程；此后每次运行都被
这个残留状态挡住。这是**累积型缺陷**，单次通过并不能代表稳定。

**已经修掉的测试侧缺陷**（这些曾把产品缺陷伪装成测试失败，或反之）：
1. 安装服务前未停前台实例（管道冲突，SCM 7024）；
2. stop 用 `--wait`（走已知坏掉的 `operation wait`）；
3. 用 `disk list` 里**第一个 UUID** 当作本轮 profile（在有残留时打错盘）。

**已经修掉的产品缺陷**：孤儿 profile 拖死 create（`2e8cf32`）、启动期终态操作缺 profile 即致命
（`6d70940`，按 profile 豁免）、forget 完成时拒绝"bundle 已不存在"（`c11f6d2`）、
forget 本身未实现（`c1ac06f`）。

**下一步**：查第 5 次那份 `last-service.log`/`last-trace.log` 里 stop 为什么停在 reconciling、
那个 worker 为什么没退（很可能是设备退役窗口内的阻塞，或 reconciliation 的判定条件永远不满足）。

---

### 3.8 决定：stop 的"最终 checkpoint 为 dirty"要有界复检，不 park

**读数**：`cli_e2e -Repeat 20` = **18/20**（请求 `98e28d87d1ff46ea9a8905aa755cc935`），
两个失败（#2、#14）同因：live stop 停在 `reconciling` 300 s，留一个 worker。

**根因是两个守卫互相矛盾**：
1. stop 的最终 checkpoint 判 dirty ⇒ `resolving` + needs-attention；
2. `remove_and_stop` 拒绝 dirty runtime（怕拆卸时丢未提交的写）；
3. 拒绝后 `park_stop_reconciliation` ⇒ `reconciling`，而它只能靠下一次服务启动推进。

**决定（选项 2，有界复检）**：
最终 checkpoint 判 dirty 时，**再 flush + checkpoint 一次（最多 N=2）**；
干净了继续拆卸；仍 dirty 才以具名失败 + needs-attention **终态**收尾，**不 park**。
不拆掉任何守卫，也不改变"允许丢 checkpoint 之后的写"这条既有契约。

**实现落点（按顺序）**：
1. `service_core.cpp` 的 stop checkpoint 分支：把「dirty ⇒ resolving」改成
   「dirty ⇒ 再执行一次 `flush_exact_volume` + checkpoint；仍 dirty 才 resolving」；
2. `park_stop_reconciliation` 的调用点：改为终态失败（`OperationState::failed`）
   + 具名标记（如 `stop-final-checkpoint-dirty`），不再进入 `reconciling`；
3. 验收：先 `-Repeat 3`（快），再 `-Repeat 20`。20/20 才算目标达成。

**不要**做的事：不要为了通过而让 `remove_and_stop` 接受 dirty——那是唯一在防"拆盘即丢数据"的守卫。

---

### 3.9 2026-10-26：`cli_e2e -Repeat 20 → 20/20`（第一次）

```text
integration.m5.cli_e2e  -Repeat 20  →  20 of 20 passed
请求 6d8dd60cf5064f6d85a21a3ef7065112
```

覆盖：create → list/show → 读写 → checkpoint → stop → 注册仍在 → forget →
installed service（SCM 安装/应答/停止/删除）。

**第二次同样 20/20**（请求 `3c6697405728401fa19ab5d75e3bb93b`）⇒ **连续两次 20/20，
合计 40 次连续通过**。

**仍然诚实记录**：在这之前的两轮是 18/20（失败停在 `reconciling`），而且从 18/20 到
20/20 之间**只改了测试的诊断输出、没有改产品代码**。所以那个间发缺陷**没有被根治**，
只是在这 40 次里没再出现。它现在的判据已经就位：下次一旦复现，失败信息会直接带出
`service.log` 尾部与 `step=`。

**这轮没有新的产品改动**，只给 `Wait-ForTerminal` 加了"失败时打印服务日志尾部"
（`reconciling` 被三种失败共用，不打印就只能说"它挂了"）。也就是说：18/20 → 20/20
之间没有代码变化，**这个间发缺陷仍未根治**，只是这次没出现。

---

### 3.10 **已完成（见 §3.13）**：`disk start`（stop 之后重启）的接线记录——保留作原理参考，不要再按它施工

**现象**：`disk.start → create-runtime` 失败，`code=21000 step=select_volume_for_disk`，
`seen=[C?@0 -?@0 -?@1 -?@0]`——卷在磁盘 1 上、**没有盘符**，而该函数第一道过滤是盘符。

**根因**：`find_recovered_volume` 的 GUID 分支（`windows_volume.cpp:685`）是对的，
但**上游从来没喂过 GUID**，所以它永远是 skip。

**已经做完的一层**（`798b0d1`）：`WorkerLaunchSpec` 有 `volume_guid` 字段 + 编解码，
`unit.m5.worker_protocol` 通过。

**还剩五处（按顺序）**：

1. `src/host/m5_worker_runtime.h`：`WorkerRuntimeConfig` 加 `std::wstring volume_guid;`
2. `src/host/m5_worker_main.cpp`（约 175-180 行，`config.bundle = …` 那一段）：
   `config.volume_guid = spec.value().volume_guid;`
3. `src/host/m5_worker_runtime.cpp`（约 313-320 行的 `ProductionRuntimeAdapter::Config{…}`）：
   把 `volume_guid` 传到 `expected_volume_guid`。**注意这是聚合初始化，字段顺序敏感**。
4. `src/management/service_worker_control.cpp` 的 `make_launch_request(const ProfileRecord&)`：
   从 durable 记录填 `request.volume_guid`。记录里的字段是
   `std::optional<product::Uuid> volume_guid`（`profile_types.h:116`），**只有大类
   `{xxxxxxxx-…}`，没有前缀**。
5. **格式陷阱（会白跑一轮，务必注意）**：`find_recovered_volume` 里的比较是
   `_wcsicmp` + 只去掉尾部的 `\`，而枚举出来的 `volume.volume_guid` 形状是
   `\\?\Volume{guid}\`。所以第 4 步必须把它拼成 **`\\?\Volume{<uuid>}\`** 再上线，
   否则 GUID 分支会匹配到 0 个、退回按盘符找、症状与现在一模一样。

**第 13 轮补充：实际是六处，不是五处。** 服务侧的请求结构也要带上它，否则 spec 永远是空：

| # | 位置 | 内容 |
|---|---|---|
| 0 | `src/management/service_worker_control.h` | `WorkerLaunchRequest` 加 `std::wstring volume_guid` |
| 0b | `service_worker_control.cpp:118` / `:150` | 两个 `make_launch_request` 都填：durable 那条从 `profile.volume_guid` 取，create/import 那条留空 |
| 1 | `src/host/windows_worker_sessions.cpp:59` | `spec.volume_guid = request.volume_guid;` |

**已有先例可直接抄**：`service_worker_control.cpp:467` 就有
`expected_volume = profile.volume_guid.value().to_string();`——同一个 durable 字段、
同一种取法。别忘了第 5 条的 `\\?\Volume{…}\` 前缀。

**验收**：干净客机单次 `cli_e2e`（含 restart）→ `-Repeat 20`。

---

### 3.11 2026-10-26 收尾：restart 的现状（交接用，未完成）

**已做完并提交**
- `798b0d1` / `99ff704`：`volume_guid` 从 durable profile 一路传到 worker 的
  `expected_volume_guid`（6 处接线，格式 `\\?\Volume{...}\`）。
- 客机**正面证据**：`verify_volume` 打出的 `volume_guid` 与 `expected_guid` 一致、
  `actual_disk=1 expected_disk=1` —— 卷是**按身份**认出来的，不再靠盘符。
- `stp`：stop 在 checkpoint 未解决时改为**具名终态失败**，不 park（另一处 `resolving`
  早前已改）；`verify_volume` 的诊断改为逐条列出每个可拒绝条件。
- `8b8e8fa`：e2e 增加 restart 校验（start → 同盘符 → 读回 stop 前写的文件 → 再 stop）。

**未验证 / 未解决**
- restart **通过过一次**（运行 `efbf62fe`，其后第二次 stop 卡在 `reconciling`），
  但之后两次在 `create-runtime` 又失败；最近一次（干净客机 `2bbffea`）仍失败且耗时
  约 5 分钟，指向又一次 300 s 等待。**结论：restart 不稳定，未达到 T1-05 的验收线。**
- 300 s 的 `reconciling` 等待仍会出现，说明还有 `park` 落点没改成终态失败
  （`service_core.cpp:3950` 的回滚路径尚未处理）。

**下一步（建议新会话开头就做）**
1. 硬重启 + 清 `ProgramData\MyRamDisk\{profiles,operations}`；
2. 单次运行并**完整**读该 run 的 `stderr`（不是 tail），确认失败是 `create-runtime` 还是
   `reconciling`；
3. 若是 3950 的 park → 同样改为具名终态失败；若是 `create-runtime` → 读
   `Diagnostic verify_volume` 的逐条输出定位。

---

### 3.12 **已被 §3.13 取代**：stop 的 remove_mount 被"runtime 仍然 dirty"拒绝（`278d63e` 已处理）

**读数**（`run-fe0a8a2a` 的服务日志）：

```text
disk.stop → checkpoint completed → evidence-recorded profile state published
disk.stop → lifecycle-action failed
code=23559 step=service_worker_control.remove_mount
detail=The worker refused to remove: its runtime stands dirty
→ 操作停在 reconciling，300 s 超时，残留 1 个 worker
```

**含义**：冻结 → flush → checkpoint 提交 → 拆卸，前三步都成功，第四步被拆卸守卫拒绝。
守卫本身是对的（dirty 时拆盘等于丢数据）；问题是**没有任何路径能在"提交后仍有残余写"时安全收尾**。

**修法（两处选一，建议 A）**

- **A. 有界复检**：拆卸被拒且原因为 dirty 时，再做一次 `flush + checkpoint`，然后重试
  `remove_mount` **一次**；第二次仍 dirty 则以具名终态失败收尾
  （`stop-dirty-after-recheckpoint`），不再 park。
- **B. worker 侧自愈**：让 `remove_and_stop` 在判定 dirty 之前先 flush 一次并重新取 dirty；
  改动更局部，但要确认 flush 之后 dirty 计数确实会归零。

**落点**：`src/management/service_core.cpp` 的动作失败分支（`remove_mount` 的失败处理）
＋ `src/host/m5_worker_runtime.cpp` 的 removal 路径。改完先单次，再 `-Repeat 20`。

**已验证通过的部分（本次会话的净收益）**：`volume_guid` 六层接线、`verify_volume` 的
ownership 判据（attach-only 不再要求 owned）、恢复路径先分配盘符 —— restart 已能从
`create-runtime` 一路走到"重启后再次 stop"。

---

### 3.13 当前真实位置（2026-10-26 晚，接 §3.12）

**restart 已打通**：一次运行的服务日志显示 `disk.start` 的八个动作全部
`action-result: completed` 并到达 terminal：

```text
create-runtime → recover-bundle → start-proxy → create-device →
locate-physical-drive → verify-volume → assign-mount → readiness-io → terminal
```

**剩下的失败**：紧接着的**第二次 stop** 以终态失败结束（快速，不再挂 300 秒）：

```text
kind=7 command=disk.stop phase=terminal
  message=terminal failure without a worker-action error; marker=flush
```

`marker` 是 `finalize_operation` 收到的终止标记，内容恰好是**动作名** `flush`——即这次
stop 失败在 `flush` 这个动作上，且**不是 worker 动作返回的错误**（`action-start: flush`
甚至没有写出来，说明失败发生在动作启动阶段，例如 runtime 未绑定）。

**§3.12 的结论已被取代**：不再是 `remove_mount` 拒绝 dirty（`278d63e` 的
flush-before-refusal 让那条不再触发），现在前移到 flush 本身。

**下一步**：查 `service_core.cpp` 里"用动作名当 marker"的那个 finalize 调用点，
确认第二次 stop 在 flush 阶段失败的具体原因（runtime 是否已绑定、卷锁是否在手），
修完再单次 → `-Repeat 20`。

---

### 3.14 2026-10-26 晚：restart + 两次 stop 全通，`cli_e2e -Repeat 20 → 20/20`

**验收读数（干净客机）**：`integration.m5.cli_e2e` × 20 = **20/20**，
请求 `ad796d29c8c04ef58b71766fdaa28a3b`，20 次共 5 分 50 秒（12:07:48 → 12:13:38Z）。
每次迭代都走完 create → checkpoint → stop → **restart** → **第二次 stop** → forget →
安装服务 install/answer/stop/delete。对照：修复前同一命令 **18/20**、**17/20**，两次不同批次。

#### ① 先加诊断（只加日志，不改行为）

`rollback_stop` 之前把失败原因写进服务日志；同一个 lambda 复用到 quiesce / freeze /
flush / resume，并在两个 park 点各写一行。首次单次运行立刻读出原因：

```text
kind=7 message=stop action failed: action=flush_exact_volume outcome=error
      domain=3 code=20999 native=0 step=flush detail=volume ownership contract is incomplete
```

`marker=flush` 只是把**动作名**当标记（`finalize_operation` 收到的终止标记），
错误本身在 `rollback_stop({...})` 里被丢弃——这就是上一轮"日志只剩 marker"的全部原因。

另外：e2e 每次迭代把自己的 `service.log` / `trace.log` 另存到
`C:\m5\runs\<work-root>.{service,trace}.log`。**这一条是必需的**：`last-*` 会被下一次
迭代覆盖，2/20 那次失败时只剩下"最后一轮通过"的日志，等于没有证据。

#### ② 根因一：按身份认回的卷"不属于自己"（已修，`windows_volume.cpp`）

`Win32VolumeAdapter::find_recovered_volume` 在"durable GUID 命中唯一候选"的快路径
直接返回观测到的卷，而 `enumerate_native_volumes` 给每个卷都打
`operation_owned=false`（`windows_volume.cpp:490`）。同函数的另外两条选择路径
（`select_volume_for_disk`）都会补 `owner_token` / `drive_letter` / `operation_owned=true`。

于是**只有"重启后按身份认回卷"这条路**拿到一个无主的卷：`verify_native_volume_identity`
的所有权前置（`windows_volume.cpp:302`）在 flush 时拒绝。而 `disk.create` 建出来的卷
一直是有主的，所以**第一次 stop 永远正常、第二次 stop 必挂**——这正是 §3.13 的现象。

修法：快路径补上同样的三个字段（不动任何守卫）。修完单次运行全套通过
（请求 `696c778500974694a68558a38b80c6cb`，24 秒）。

#### ③ 根因二：stop 的写冻结从未运行过（T1-11 的死分支，已修）

20× 随即暴露第二层：**18/20**，两次都是「300 s 没到终态，last state=reconciling」。
诊断轮 **17/20** 拿到了原因，两个签名：

```text
action=lifecycle-action code=23559 step=service_worker_control.remove_mount
  detail=The worker refused to remove: its runtime stands dirty
→ stop parked at the removal boundary: action=lifecycle-action stop_phase=5

checkpoint: disposition=3 …            （committed_needs_reconciliation）
→ checkpoint parked for reconciliation: phase=checkpoint-resolving
```

两者是同一件事：**最终提交之后卷仍可写**，于是提交后又有 dirty。而写冻结
（T1-11，`b3fae6f` 引入）本该是那道闸：

```text
state_machine：stop 计划 = quiesce_mutations, freeze_writes, flush_exact_volume, checkpoint_if_required, …
service_core：quiesce 块把 phase 置为 quiesced 并发布
              紧接着的 freeze 块守卫写的是 `stop_state.phase == admitted`  ⇒ 永不成立
              之后 execution_actions.erase(begin, begin + 3) 仍按"前三个已执行"把 freeze 删掉
```

`git show b3fae6f` 里顺序已经如此，所以**自 T1-11 起这条分支就是死代码**，计划里的
`freeze_writes` 被当成"已执行"跳过。T1-11 当时那份 20/20 来自 worker 侧
（`remove_and_stop` 先 flush 再判 dirty、无共享独占打开有界重试），不是 service 侧这个动作。

修法：守卫改成 `quiesced`（quiesce 之后、flush 之前）；worker 侧
`DiskRuntime::freeze_writes` 幂等，`volume_flushed` 的续跑路径仍跳过。

**客机正面证据**（`C:\m5\last-trace.log`，20/20 那轮）：

```text
Win32VolumeAdapter::lock: volume locked
dismount: through the retained lock          ← 拆卸复用冻结那把锁（T1-11 的第 ① 条要求）
```

并且所有 stop 的最终提交都是 `checkpoint: disposition=2`（`committed`），
不再有 park，也不再出现 `23559`。

#### ④ 根因三：重启后卷还没发布就被枚举（已修，`runtime_adapter.cpp`）

开冻结后 20× 变成 **17/20**，但失败前移到 restart 且**变快**（整批 6 分钟，不再 300 s）：

```text
command=disk.start message=action=create-runtime code=21000 native=1168
  step=find_recovered_volume detail=no volume carries the recorded identity
```

新建设备后卷尚未被枚举到（`ERROR_NOT_FOUND`），下一次迭代同样能起来 ⇒ 是发布延迟。
`ProductionRuntimeAdapter::find` 现在对 `not_found` 做**有界复检**（5 s，250 ms 一次），
**且只在有 durable GUID 时**才复检：没有记录身份时"找不到"就是答案（reclaim 的残留可能
真的没了），不该等。

#### ⑤ 顺带修掉：三处陈旧期望（`6e1bfcc` 遗留）

`m4_runtime_adapter_tests`、`m4_disk_runtime_tests`（开发机）与
`integration.m4.multi_instance`（客机整套时才暴露）都是红的，**与本次改动无关**：
`6e1bfcc` 把恢复路径改成"先分配盘符再校验身份"（所有权 token 在文件系统上，没挂载点读不到），
三处 fake 的期望顺序仍是 `verify → assign`。已改成 `assign → verify`（`multi_instance` 是按下标
断言的 `events[6]/[7]`，一并改），并把
`m4_disk_runtime_recovery_size_mismatch_does_not_mount` 改成它真正想说的事：
**失败的重启不许留下挂载**（现在断言盘符被收回：`assign-letter → verify → remove-letter →
dismount → aim-stop → unlock`）。七个 fake-only 套件开发机全绿。

**下一步**：T1-05 可判 PASSED（create/checkpoint/stop/restart/第二次 stop/forget + 安装服务，
20/20）；T1-06 起继续。**未验证**：`stop` 的 `require_clean` 策略分支（e2e 只走
`checkpoint_required`）；park 仍保留给"真正 dirty"的情形（现在不再触发，但路径还在）。

#### ⑥ 整套回归（客机，硬重启 + 清空后）

**151/154**（请求 `8ec8aa88c34740308c2098e9dc90a7f3`，9 分钟），三个失败都不是本次改动：

| 失败 | 原因 | 处置 |
|---|---|---|
| `structural.m2.storage_independent_build` | 客机无 Visual Studio：`vswhere.exe not found` | 环境受阻，历次基线同项，**不是产品失败** |
| `structural.m5.execution_boundary` | 同上 | 同上 |
| `m5.production_execution_backend` | 批内挂在 `WorkerBindingStore::over_protected_root`（`store.ok()`，`m5_production_execution_backend_tests.cpp:514`） | 单跑 **3/3 通过** ⇒ 批内间发（`m5-vm-harness.md` 已列的已知间发之一），不是本次改动 |

同日更早那次整套也是 151/154，第三个失败是 `integration.m4.multi_instance`
（`events[7] == "assign-letter"`，与 §⑤ 同源），已修，单跑
`multi_instance + cli_e2e` → **2/2**。**本分支整套的期望值是 152/154**
（两条 vswhere 永远红），外加 `m5.production_execution_backend` 在批内的已知间发。

---

### 3.15 2026-10-26 夜：T1-06 完成（中断提交判定可达）

**完整体验文档**：`docs/validation/m5-t1-06-recovery-proof.md`。要点：

1. **从 P4 造 launch 起只读检查 worker**：`reconcile_checkpoint` 在没有 session 时从 durable
   profile **launch 但不 start**，问 `recovery_proof`，每个分支都关闭 inspection 并等进程退出；
   launch 失败作为失败上报。改前用 `require_session`，只能 adopt 活着的 runtime——而本问题
   恰恰是"提交已在 P4、worker 已不在"，所以**正面路径一次都跑不到**。
2. **发现一个让确认提交永远不可能的缺陷**：候选判据六条里的 prior generation 比的是 manifest 的
   `base_generation`（基底镜像代次，提交 checkpoint 时不前进），而 correlation 的 prior 是上一代
   checkpoint。客机读数 `mask=0x2`，其余五项（镜像 uuid、代次、native id、slot、digest）**全相同**。
   修法：候选判据回到规格五条；prior 只用于否定判据；`CheckpointRecoveryInspection::prior_generation`
   改名 `base_generation`。修后 `verdict=2 mask=0x0`。
3. §14.3 七条全部有读数（含真 worker/真 bundle 的三条正面/否定路径、只读性指纹）。

**下一步**：T1-07（内存可见性 + UNMAP 端到端）。

### 3.16 2026-10-26 夜：T1-07 完成（内存可见性 + UNMAP 端到端）

**验证文档**：`docs/validation/m5-t1-07-memory-visibility.md`。要点：

1. `resident_bytes` / `budget_bytes` 从存储层计数器一路到 CLI：`disk list` / `disk show` 行尾
   出现 `mem=<resident>/<budget>`。**来源是存储自己的 committed_bytes，不是进程工作集**。
2. **UNMAP 首次端到端**：写 16 MiB → resident 2 949 120 → **19 660 800**（恰好 +16 MiB）→ 删除后
   回到 **2 949 120**。budget 1 073 741 824，worker 工作集 22 573 056（同量级）。
3. **第一次量不到，原因值得记**：普通写入后 resident 完全不动，数据还在 Windows 文件缓存里；
   改**写穿** + 有界轮询才量到存储。所谓"写文件后内存立刻上升"在 Windows 上是缓存错觉。
4. 每层都钉了用例（协议 round-trip、截断报 malformed、observation 只在 alive 时带值、wire、渲染）。

**验收**：干净客机 `cli_e2e -Repeat 20 → 20/20`（式 `541c8b4a…`，7 分 10 秒）。

**延后**：`disk release-memory`（§7.4）——它需要一条从 storage 公开 `reclaim_to` 到 CLI
动词的完整纵切，与内存压力同源。**T1-08 之后仍未做，见 §3.17「未做」。**

---

### 3.17 2026-10-26 深夜：T1-08 完成（检查点策略终于有消费者）

**验证文档**：`docs/validation/m5-t1-08-checkpoint-policy.md`。要点：

1. 四个策略字段（interval / dirty 阈值 / 内存压力 / 休眠）从"存下来就丢"变成真的会触发。
   **客机读数**：`generation moved 2 -> 3 on its own after 30.4 s`，全程**没有发过 checkpoint 命令**。
2. 落点：纯函数 `checkpoint_due_reason()` + `ServiceMaintenance`（SYSTEM 身份 dispatch，
   **只做持久化准入**，执行仍归调度器）+ 调度器 claim-once tick（15 s）+
   `SERVICE_ACCEPT_POWEREVENT` / `PBT_APMSUSPEND → ServiceHost::suspend()`。
   suspend **不是 stop**：设备不拆、worker 不退，只做尽力而为的检查点。
3. **接线时量到两个缺陷**：① interval 时钟没有起点（`since` 恒 0 ⇒ 永不到期）；
   ② 命令寻址用了 mount target 而非 profile uuid（每次 `The requested profile is not registered`）。
4. **一条范围边界**：两块盘同时活着时，第二块的拆卸不会真正移除设备（restart 找不到卷、
   forget `device_absent=0`）。M5 是单盘产品（G4 延后），所以 e2e 改成一次一块盘；
   这条不是策略缺陷，但值得记住。
5. `forget` 的九个合取项现在拒绝时**逐项打印**（和 `verify_volume`、checkpoint boundary 同源）。

**验收**：干净客机 `cli_e2e -Repeat 5 → 5/5`（请求 `7aa88f14…`，6 分 9 秒）。

**未做（记录，不当作已完成）**：
- `disk release-memory`（§7.4 手动释放）。它在 T1-07 与 T1-08 各被延后一次，理由不变：
  需要 storage 公开 `reclaim_to` → M3 → M4 runtime → worker 命令 → 服务动作/计划 → CLI 动词
  的完整纵切。**它不在完成口径里**（口径 = G1+G2+G3 验收 + T4-04 manifest + master-plan §8
  的遗留清单，而该清单四项里没有它），但设计 §7.4 承诺过这个命令，所以在这里留名。

**下一步**：T1-09（M5-A 单盘验收 + 断电恢复）。它依赖 T1-06/07/08/11，现在四个都齐了。

---

### 3.18 2026-10-26 深夜：T1-09 进行中（开机自动恢复已完成，断电 harness 待打通）

**已完成并提交（`51e164d`）**：开机自动恢复的消费者。
- `ServiceCore::admit_auto_starts()`：启动时把"durable 状态 = running 且 `auto_start` = true"的
  profile 逐个重新**准入** `disk start`（准入而非执行，执行仍归调度器）。
- `ServiceHost::start()` 在 recovery → sweep → scheduler 之后、公开面打开之前调用它；失败即
  启动失败（与 recovery/sweep 同规），条数进 `ServiceHostDiagnostics::auto_started_operations`。
- 没有这段，断电后字节会被恢复、盘却以 stopped 回来等人手启——"最多丢一个间隔"只兑现一半。
- 两个模型用例：会回来的那张盘被且仅被准入一次（`disk.start`）；`auto_start=false` 的盘不碰。
- 顺带两处 fixture 修正（都由"拒绝要能定位"逼出来）：`resolve` 现在和产品一样接受 uuid；
  失败日志带 `step=`。

**已写好、未打通**：断电验收 harness
- `tests/e2e/test-m5-powerloss-prepare.ps1`（装 auto-start 服务、建 30 s 间隔的盘、置 auto_start、
  写标记、等一次自动提交、把"必须活下来的事实"写进 `C:\m5\powerloss-state.txt`）
- `tests/e2e/test-m5-powerloss-verify.ps1`（只看不动：服务是否自己起来、盘是否自己回来、
  generation ≥ 提交值、标记字节一致、worker 恰好一个）
- `tools/m5vm/Test-M5PowerLoss.ps1`（host 侧三段：prepare → `vmrun stop hard` → 开机 → verify；
  判定读 harness 自己的 `N of M passed`，因为 **harness 在用例失败时仍退出 0**——这条已经踩过）
- CMake 已注册 `integration.m5.powerloss_prepare` / `..._verify`（155 → 157 例）

**2026-10-27 00:52 第六次读数（干净三段跑完；剩下的问题是"服务 RUNNING 但没有公开管道"）**

干净客机（硬重启 + 清 ProgramData + 删服务）跑完整三段：
- `prepare` **1 of 1 passed**（请求 `b0cb01b9…`，52 s）
- 硬断电 + 开机 + `verify` → **0 of 1**，请求 `1280adbd…`，耗时 187 s
- 187 s ≈ `Wait-ForService 180` + 7 s，即**第一步就失败**：CLI 连不上

断电后直接手动确认（不再靠 harness）：

```text
myrd-cli disk list → exit=3, 30016 ms
  "myrd-cli: the service is not available (is it installed and running?)"
sc query MyRamDiskPowerLoss → STATE: 4 RUNNING
```

**所以现在的未解问题是产品侧的**：断电重启后 **SCM 说 RUNNING，但公开管道不存在**（或不可连接）。
`ServiceHost::start()` 若打开公开面失败会把状态置 failed 并让服务报 STOPPED，所以要么
start() 根本没走到 open，要么 open 成功而管道对 CLI 不可达。

**下一次要查的顺序（都很便宜）**：
**2026-10-27 00:55 追加（关键一步已经查了）**：断电后那台客机上

```text
Get-Process myrd-service → 没有进程
\\.\pipe\ 里 myrd*     → 没有任何管道
C:\ProgramData\MyRamDisk\logs\service.log 最后写入 00:46:32（在断电之前）
```

而 `sc query` 在几分钟前还报 RUNNING。**结论：断电后的服务进程自己退出了（疑似崩溃），
公开管道因此从没建立起来**。所以 T1-09 剩下的不是一个"恢复逻辑"问题，而是
**"重启后服务进程会死"**——下一步必须看 **Windows 应用程序事件日志**（Event ID 1000 /
WER）拿崩溃模块与偏移，再决定是修代码还是修环境。`service.log` 在 00:46:32 之后没有任何
新行，说明服务**在这次开机里连一条日志都没写**——它很可能在 `ServiceHost::start()` 之前
（或刚开始）就死了，这一点与"崩溃"比与"逻辑卡住"更吻合。

**2026-10-27 01:00 再追加（证据互相矛盾，必须按时间戳重测，不要再猜）**

同一台客机、几分钟之内：

| 时刻 | 观察 |
|---|---|
| 00:52 | `myrd-cli disk list` → exit 3「service is not available」，30 s；`sc query` → **RUNNING** |
| 00:55 | 没有 `myrd-service` 进程、没有任何 `myrd*` 管道；`service.log` 最后一行 **00:46:32**（断电前） |
| 01:00 | `sc query` → **1060 服务不存在**（`service.log` 仍是 00:46:32） |

三条观察不能同时为真：RUNNING 的服务不能既没有进程、又没有管道，更不能在没有人删它的情况下
变成"不存在"。可能的解释有三类，**都必须用带时间戳的重测来区分**：
(a) 我读到的 RUNNING 是**上一次准备阶段**的输出（命令输出串了）；
(b) 客机在两次检查之间**重启过**（例如我加的 boot 重试或客机自己复位），服务因此回到
    "未安装"（`C:\ProgramData` 里的 profile 目录还在，说明不是快照回滚）；
(c) 服务装在一个**每次开机都会消失的位置**（例如 binPath 指向 staging 目录而被清理）。

**下一次的做法（写成清单，照做）**：
1. 干净客机 → `Test-M5PowerLoss.ps1` 只跑到 prepare（`-ResumeVerify` 之前手动停）；
2. **立刻**记录：`sc qc`（binPath/start 类型）+ `sc query` + `Get-Process myrd-service` +
   `\\.\pipe\` 过滤 + `service.log` 行数 + `Get-Date`，作为**基线快照**；
3. 断电 → 开机 → **每 15 秒**采一次同样的快照，连采 3 分钟并带时间戳输出；
4. 用快照序列判断上面 (a)/(b)/(c) 哪一类，再动代码。

**2026-10-27 01:05 结论：上一轮的"矛盾"是测量通道的问题，不是产品的**

用**带时间戳的快照**（`C:\m5\snap.ps1`，客机本地跑）在断电重启后 31 秒采到：

```text
01:03:38 state=RUNNING pid=3164 start=01:03:07 pipes=0 loglen=8605
sc qc → START_TYPE: 2 AUTO_START
        BINARY_PATH: …\out\build\msvc\src\Release\myrd-service.exe
```

而同一时刻服务自己的日志已经写到 `disk.start … terminal`——**断电后的那次 start 走完了全部
动作**（`start-proxy → create-device → locate-physical-drive → verify-volume → assign-mount →
readiness-io → terminal`）。

于是三条"矛盾"观察的解释是：**普通 ssh 会话不是提权会话**（AGENTS §5：SSH 是网络登录、不提权），
所以
- `\\.\pipe\` 枚举可能被拒 → 我看到的 `pipes=0` **不是**"没有管道"；
- 非提权的 `myrd-cli disk list` 拿不到公开管道 → 我看到的 `service is not available` **不是**
  "服务没起来"。

**教训（写进 lessons）**：**凡是要碰公开管道、进程、服务的测量，只能走 harness 的提权通道
（ctest 用例 + scheduled task），不能用普通 ssh 会话下结论。** 我上一轮据此写下的
"服务进程死了""服务不存在"两条都要收回。

**下一步**：把快照做成一个 ctest 用例（提权通道），在 verify 之前打印一次；然后用它替代
我这几轮的手工 ssh 判断，重跑一次三段。verify 的 `Wait-ForService` 若仍超时，那才是真问题。


   `StartTime` 是不是这次开机；
2. `C:\ProgramData\MyRamDisk\logs\service.log` 里这次开机的最后几行——**有没有** `auto-start`
   与 subsequent 的 action 行（如果日志停在 `auto-start: …retired…` 之后，说明 start() 卡在
   `admit_auto_starts` 或调度器启动里）；
3. `\\.\pipe\` 里有没有公开管道名（`Get-ChildItem \\.\pipe\ | ? Name -match 'myrd'`）；
4. 若服务日志**完全没有**这次开机的行，说明服务进程不是这一版二进制（staging/服务 binPath 不对）。

**T1-09 已完成的部分**（有读数）：断电后服务自起、盘自起、generation ≥ 提交值、脏数据重放
提交成下一代（`0af35d5` 的提交信息里有完整读数）。**未完成**：verify 端到端一次绿，
以及上面那条"RUNNING 但无管道"的原因。

**2026-10-27 00:45 第五次读数（T1-09 的产品部分已全绿；剩下的全在 verify 脚本与客机侧）**

**断电后连续两次读到的产品事实**（这才是 T1-09 要的）：

```text
service state: RUNNING（脚本从未启动它）
5003a664-… T: ready running runtime=Running activity=Idle
            persistence=Dirty health=Healthy generation=4  mem=1048576/1073741824
recovered generation: 4 (last committed before the cut: 3)
```

即：**服务自己起来、盘自己回来、generation ≥ 断电前提交值、resident 内存 1 MiB**。
`persistence=Dirty` 且 generation 从 3 走到 4，说明恢复把掉电时的脏数据重放并提交了一代
——这正是"最多丢一个间隔"的实测形态。

**本轮修掉的三处脚本/工具缺陷**（都是"让产品看起来坏了"的那类）：
1. 状态文件**没落盘**：断电后回来是 0 字节 → 用 `FileStream.Flush($true)` 写；
2. `verify` 里 `Join-Path $mount (powerloss-marker.txt)` 少引号（`CommandNotFoundException`）；
3. 编排器加 `-ResumeVerify`（只跑 verify）与 boot 失败自动重试一次。

**最后一个未解的现象**：`-ResumeVerify` 那次 `sc query` 是 RUNNING，但 CLI 连不上公开管道
（`Wait-ForService 180` 超时）。两次读数之间我前一次 verify 失败后**没有做清理**，所以极可能是
**上一个服务实例的管道残留**。下次：断电重启 → 完整跑一次三段（不要 -ResumeVerify），
若仍在 `Wait-ForService` 失败，先看 `sc query` 的 PID 与 `C:\ProgramData\MyRamDisk\logs\service.log`
是否来自同一进程。

**下一步（新会话从这里开始）**：干净客机 → 三段一次跑通 → 写
`docs/validation/m5-t1-09-power-loss.md`、tasklist T1-09 → PASSED → 进 T1-10。

**2026-10-27 00:35 第四次读数（prepare 稳定通过，断电后客机本身有概率起不来）**

- `prepare` 在干净客机上**连续两次通过**（请求 `d0667c91…` 50 s、`8eae57a7…` 52 s）：
  装 auto-start 服务 → 建 30 s 间隔的盘 → `auto_start` → 写标记 → 等自动提交 → 落状态。
- 状态文件改 JSON 后 `verify` 仍报 `invalid JSON primitive`（写读两端对编码的处理仍不一致），
  于是**改成不依赖 JSON 的容错读取**（一条正则同时接受 JSON 与 `key=value`，失败时打印全文）。
- **硬断电之后客机有概率起不来**：这次 600 s 内 SSH 无响应（累计第二次）。主机侧
  `vmrun stop hard` + `start nogui` 之后 15 s 就好了，说明是**客机启动偶发挂死**，不是网络配置。
  ⇒ 编排器需要：boot 等待超时后**自动断电重试一次**，并且提供 `-ResumeVerify`
  （prepare 与断电都已完成、状态文件仍在时，只跑 verify，不重跑前两段）。
- 断电后的那次 start 已经走完整真实序列（`6177380` 的修复），所以下一轮要看的只剩
  **verify 的三条断言**能否读到正确的状态文件。

**下一步（新会话从这里开始）**：
1. 给编排器加 `-ResumeVerify`（跳过 prepare 与断电，直接 verify）与 boot 失败自动重试一次；
2. 干净客机跑一次完整三段；verify 通过即 T1-09 完成（读数写进
   `docs/validation/m5-t1-09-power-loss.md`，tasklist T1-09 → PASSED）；
3. 然后进 T1-10。

**2026-10-27 00:05 第三次读数（harness 已能判绿，客机会在两处卡死）**

- harness 的判定修好了，两处都是**量出来的**：
  ① harness 用 `Write-Host` 报判定，走的是 information stream，`2>&1 | Out-String` **抓不到**，
  于是包装器对着"屏幕上明明印着 `0 of 1 passed`"的 run 说"没有判定行"；改用 `6>&1`。
  ② staging 阶段印的是数量不是判定，用同一条规则会把刚成功的 stage 判成失败；so stage 只看退出码。
- **`-SkipStage` 会让客机跑旧的 e2e 脚本**（脚本随 `-Stage` 的 git 归档下发）。这解释了
  "uuid 解析已修却仍然报同一个错"——那两次客机跑的是旧文件。以后改 e2e 脚本必须带 `-Stage`。
- **客机会卡死**（两次，都在 prepare 阶段前后）：SSH 无响应，只有硬断电才能恢复；
  两次都是先跑过一轮断电实验之后。**下一次尝试前先硬重启**，并把"跑前重启"当作这个
  harness 的固定前置，而不是例外处理。

**下一步（新会话从这里开始）**：
1. 硬重启客机 → 清 `ProgramData\MyRamDisk\{profiles,operations,logs}`；
2. `Test-M5PowerLoss.ps1`（**不带** `-SkipStage`），一次跑完三段；
3. 若 prepare 再次卡死：先用 `sc query`/`Get-Process` 确认是**服务**还是**客机**卡，
   再把 prepare 里"装服务→start"这一步拆成独立 ctest 用例（现在它和 create 在一个用例里，
   一卡就分不清）；
4. 断电后 `verify` 若仍报 `blocked/needs-attention + PersistenceBlocked`，读
   `C:\ProgramData\MyRamDisk\logs\service.log`（本轮新增的文件日志，`c97e9cc`）里
   `auto-start refused: step=…` 那一行——原因就在那里。

**2026-10-26 深夜 第二次读数（prepare 已通，卡在断电后的 start）**

- `prepare` **通过**（请求 `a9fb722dd5854923b25e754637f74ed4`，52 s）：装 auto-start 服务 →
  建 30 s 间隔的盘 → `auto_start=true` → 写标记 → **自动提交到 generation 3** → 状态落盘。
  第一次失败的原因不在脚本：客机上残留着上一次尝试的服务与 profile，`sc start` 与旧实例冲突；
  清干净后一次通过（`sc start` 之后 stdout 断在 STATE 块是**旧实例**的表现，不是脚本 bug）。
- 硬断电（`vmrun stop hard`）+ 开机后：**服务自己起来了**（`sc query` RUNNING，开机 23:04），
  说明 `start= auto` 与 SCM 路径成立。
- `verify`（请求 `05e2971d25474ee79b2feb2b4faeeca7`）读到：

```text
6b710f86-…  U:  ready  blocked/needs-attention  runtime=Running
                persistence=PersistenceBlocked health=NeedsAttention
                generation=3  mem=0/0
recovered generation: 3 (last committed before the cut: 3)
```

即：**盘被自动拉起了（`runtime=Running`，generation 等于断电前的提交值），但运行面是坏的**——
`PersistenceBlocked`、`mem=0/0`、盘符 U: 不存在、也没有 worker 进程。所以 T1-09 剩下的问题是
**断电后的那次 start 没真正接上卷**，而不是"没有自动恢复"。

**下一步（第一件事）**：查断电后那次 `disk start` 失败在哪一步。可用的入口：
- `ServiceHostDiagnostics::auto_started_operations` 只给条数，不给原因；installed 服务的 stderr
  被 SCM 丢掉，所以**必须**让这条原因落进服务自己的日志（`ServiceLogEvent`）而不是 stderr；
- 可疑点与 §3.18 前面记的"两块盘"现象同源：**崩溃残留的设备/句柄**让 `create-runtime` 接不上
  （`find_recovered_volume` 找不到卷，或 `start_aim` 拒绝）。T1-03 的 startup sweep 本该先回收
  残留，sweep 在 auto-start 之前运行——所以要确认 sweep 是否真的把残留当"可回收"处理了。
- 再顺带修一处脚本脆弱点：`verify` 在盘符不存在时抛 `DriveNotFoundException`（现在被 `trap`
  捕获并打印，但不是一条可读的失败），应改成"读不到标记"的具名失败并附上 `disk list` 行。

---

## 4. 构建与测量循环

```powershell
# **每一个新会话、每一次构建之前：**
#   不 dot-source 就没有 LIB/INCLUDE，链接会在 LNK1181（bcrypt.lib）上失败——
#   看起来像缺 SDK，其实是环境没进来。冷会话一定会撞上（2026-10-26 复现并记在
#   `m5-review-2026-10-26.md` §3.3；`m5-lessons.md` 里当时没写，别以为写过）。
Set-ExecutionPolicy -Scope Process Bypass -Force
. .\tools\Enter-MsvcEnvironment.ps1

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
