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
