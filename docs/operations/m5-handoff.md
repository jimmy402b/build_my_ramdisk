# M5 交接：从这里开�?

**最后更新：2026-10-28**（状态有变化先改这里，再�?tasklist�?

---

## 1. 一分钟现状

| 项 | 值 |
|---|---|
| **唯一活动线** | 分支 `feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`（HEAD 以 `git log` 为准） |
| 基线 | `main` = `cb3df08`（干净 M4），**保持不动** |
| **T1（G2/G3，单盘 + 断电恢复）** | T1-01…T1-11 **PASSED**；T1-05 `-Repeat 20` 20/20；**T1-09 断电三段客机通过**（`PASS integration.m5.powerloss`，prepare 52 s / verify 5 s） |
| **T3（GUI）** | T3-01…T3-05 **完成**，客机 `PASS T3-05`（`serial=5A42-9D28`） |
| **T4 进度** | T4-01 **PASSED**；**T4-02 PASSED**（卷序列号 / 易失盘保护（含 dirty force-stop 的 `accept_data_loss` 授权）/ Load Only 的 create + start / 单文件 VHDX 导出，§3.191、§3.194）；**T4-03 PASSED**；**T4-04 PASSED**（G3 修复后重新冻结，§3.197） |
| **客机基线（安静客机）** | unit 96/96 275 s；model/m5 13/13 114 s；cli_e2e 87 s；install 9 s；gui 25 s；integration m1/m2/m3/m4 = 1/4/31/3 全绿；structural 1/3（两条 `vswhere` 环境） |
| **下一件事** | **PASSED**：R-01…R-10、R-12、R-14、R-15…R-18；STAB-03/06/07/08/10/12/13/14/15/16。**R-11 PARTIAL**：产品侧完成（`disk_show` caller_filtered + 服务端 owner 比较，`unit.m5.authorization` 5/5）；客机进程级非提权读数未取得（六种启动机制失败于启动前，见 §3.224 与 `m5-stab-09-nonadmin.md`）。**下一步**：① R-13 24h soak（小负载 + 频繁 checkpoint + 周期硬断电）；② R-19 演练 + manifest 重新冻结 + 两次全量 + 8 条 gate；③ R-11 客机读数换机制（交互式会话/`--print-caller` 诊断入口）。**manifest 仍无效** |

**顺序**：T1-05 收尾�?*已完成，§3.14**）→ T1-06 �?T1-07 �?T1-08 �?T1-09 �?T1-10 �?T3（GUI）→ T4（产品化）�?
T2 鍦?G4 鎭㈠鍓嶄笉鍔ㄣ€?

---

## 2. 文档地图（按"你想知道什�?�?

| 鎯崇煡閬?| 璇昏繖浠?|
|---|---|
| **现在在哪、下一步做什�?* | **本文** |
| 每个增量的状�?| `docs/superpowers/plans/2026-10-14-m5-v3-tasklist.md`�?*唯一状态来�?*�?|
| 顺序、依赖、验收口径、风险、何时停下问 | `docs/superpowers/plans/2026-10-15-m5-master-plan.md` |
| 设计与不变量 | `docs/superpowers/specs/2026-10-14-m5-service-cli-gui-design-v3.md` |
| 某个增量凭什么算通过 | `docs/validation/`（按增量名对号） |
| **已经付过学费的坑** | `docs/operations/m5-lessons.md`�?*动手前扫一�?*�?|
| 2026-10-26 那次评审的结论与修法 | `docs/operations/m5-review-2026-10-26.md`（时点快照，写时固定�?|
| VM 回路的三条通道与一次性配�?| `docs/operations/m5-vm-harness.md` |
| 引擎层（M3/M4）怎么�?| `docs/operations/m3-persistence.md`、`docs/testing/m3-e2e.md` |

**文档纪律**：状态只写在 tasklist；本文写"下一�?；master-plan 不再�?当前位置"�?
任何一处与前两处冲突，�?tasklist �?本文 �?master-plan 的顺序取舍�?

---

## 3. 当前增量 T1-05：`stop` 已修，验收被 `create` 的新失败挡住

### 3.1 已经通的那一段（客机读数�?026-10-23，请�?`32d4ba1c71b54d72a87c4d4e6cd33203`�?

```text
disk.create:     queued 鈫?running 鈫?鈥?鈫?publish-ready 鈫?assign-mount 鈫?readiness-io
disk.checkpoint: queued 鈫?running 鈫?succeeded (terminal)
```

`create` 全链、`list`、`show`、读写、`checkpoint` 都通过了—�?*这些此前从未跑通过**�?
详细过程与六个被推翻的假设见 `docs/validation/m5-t1-05-cli.md` §8–�?0�?

### 3.2 原来是这里卡住（2026-10-26 已修，`d5b9ad7`�?

```text
execute_operation failed: code=22022 step=service_core.checkpoint_epoch
  detail=The final checkpoint state is invalid
```

位置：`src/management/service_core.cpp:5753`，在 checkpoint 确认回调�?
�?`complete_command(pending_plan, succeeded_clean|succeeded_dirty)` �?*完成整个 stop 计划**�?

读代码确认的两个事实�?

1. **`stop` 不接�?`succeeded_dirty`�?*
   `src/management/state_machine.cpp:740` 鐨?`stop` 鍒嗘敮鍙
   `succeeded_clean` / `transient_failure` / `persistence_blocked` /
   `commit_indeterminate` / `committed_needs_reconciliation_*`�?
   `succeeded_dirty` 落到 `break` �?`invalid_completion` �?就是上面这条 22022�?
2. **这一步本来也不该完成整个命令�?*
   stop 璁″垝鍦?checkpoint 涔嬪悗杩樻湁 `remove_mount` / `remove_exact_device` /
   `stop_runtime_and_release_lock`；终点在 `finalize_operation`（`service_core.cpp:6795`）�?

### 3.3 下一步（按顺序，不要跳）

1. **根因已查明（2026-10-26）：dirty epoch 是一条两端都空着的管道，不是判据写错�?*
   - worker 渚х殑 `cut_dirty_epoch` 鍙栫殑鏄?`maintenance_stats().generation`
     （backing generation，`m5_worker_runtime.cpp:519`）；
   - service 侧的 `live_dirty_epoch` �?*写死�?0**（`service_worker_control.cpp` �?
     `publish_checkpoint_epoch`）；
   - `WorkerRuntimeStatus.dirty_epoch` 全仓**没有生产�?*，恒�?0�?
   �?`clean = (0 == generation)`：generation 0 时成立（create 的第一�?checkpoint），
   之后永不成立（stop）�?*这是确定性的，不是间发�?*
   完整证据链与五步修法�?[`m5-review-2026-10-26.md`](m5-review-2026-10-26.md) §1�?
   **不要再往"比较顺序"上调�?*——`dbb25a3` 已经证明那条路只能让 create 看起来好转�?
2. **`normal_stop` 时不要在这一�?`complete_command`**：只发布"checkpoint 已确�?�?
   durable 状态（`phase = stable`、`disposition = committed`�?
   `confirmed_generation`、`current_dirty_epoch`），�?remove 系列继续�?
   �?`finalize_operation` �?stop 的真�?outcome 收尾�?
3. **不要�?接受 dirty"当成修法�?026-10-26 更正�?*：写冻结排在 checkpoint 之前�?
   "提交后还有脏�?意味着**冻结漏了�?*——那是缺陷，报成功等于把数据丢失伪装成正常收尾�?
   已按此实现并加模型用例：clean 成功、dirty �?applied�?
4. **验收**（客机）：`disk stop` �?盘符消失 �?重启服务 �?`disk start` 恢复到最�?
   generation �?`disk forget`。修复类结论**至少 `-Repeat 20`**�?

### 3.4 顺带记下的两个可用性缺口（不阻�?T1-05�?

- `disk show/stop/checkpoint` �?`<disk>` 参数只认 display name �?uuid�?*不认盘符**�?
  而用户会自然想用盘符（本轮排查就撞上�?`profile_not_found`）；
- 失败原因的可达性已补（`m5-t1-05-cli.md` §5.2），�?`OperationRecord` 仍只存标记串�?

### 3.5 2026-10-26 晚：create 通了，stop 也通了；现在挡�?`forget`

**create 的注册失败已修（`2e8cf32`�?*：`ensure_profile_unique` 会打开**每一个其�?profile** �?
bundle 去比 file identity，并把打开失败原样上抛。客机上有一�?01:59 留下�?profile
（`c084bc42`，bundle 已被上一�?teardown 删掉），于是**之后每一�?create 都失�?*�?
而错误里报的�?*�?bundle** 的路径。四次运行的失败路径完全相同（`b6707f16…`），
而新 bundle 当时确实存在、用户进程用同样�?flags 能打开——这就是判据�?
现在改成：打不开的其�?bundle 不可能等于正在注册的那个（identity 相等要两边都打开），
前四项检查（名字 / image / 路径 / 挂载点）已经覆盖真实冲突，所�?`continue`�?

**读到的链路（同一次运行，`disk.list` 停后仍可见）**�?

```text
create 鈫?list 鈫?show 鈫?璇诲啓 73 瀛楄妭 鈫?checkpoint succeeded 鈫?stop succeeded
停后：V: ready stopped/configured runtime=Stopped persistence=Clean generation=4
```

**现在挡在 `disk forget`**�?

```text
kind=7 command=disk.forget phase=terminal
  message=terminal failure without a worker-action error; marker=unsafe
```

`ServiceWorkerControl` **没有实现** `prove_forget_safe`，走的是 `IWorkerControl` 的默认实�?
（`service_core.cpp:1390`，直接返�?"The worker control does not expose forget safety proof"）�?
�?worker 协议�?*没有任何 forget 相关的命�?*（`m5_worker_protocol.h` 全文�?forget）�?
�?**`disk forget` 是未实现，不是回�?*；fail-closed 的语义让它看起来"安全地拒�?�?
所以一直没被发现�?

**为什么不能简单在服务里补**：`myrd_management` 只链 profile/product_ipc/contracts/
worker_protocol/common（`src/CMakeLists.txt:372`），拿不�?writer lock（persistence）和
AIM 设备（driver）。proof 的五项里有两项必须由 worker 来答 �?要么加一�?worker 命令�?
要么先做 T1-06 的只读检�?worker�?

**stop 的重复读数（2026-10-26 晚）**：`m5.production_execution_backend`（含 stop 路径）`-Repeat 20` �?**20/20 通过**，请�?`a21cf2191a9d4b10bab4727e43a7737a`。这�?stop 修复的稳定证据；`cli_e2e` 整体仍卡�?forget�?

**下一步（按顺序）**：① �?worker 协议加一个只读的 forget 证明命令（bundle 存在 + file identity�?
writer lock 可取、匹配设备不存在）；�?服务侧实�?`prove_forget_safe` 并接�?tombstone 校验�?
�?客机�?`-Repeat 20`�?
---

### 3.6 2026-10-26 晚：forget 通了，现在挡�?重启"

**forget 已实现（`c1ac06f`�?*：worker �?`inspect_bundle` 里证�?writer lock 可取 + 没有同尺�?
代理设备"，服务侧 `prove_forget_safe` 用一�?inspection 拿这两项并等进程退出�?
客机读数：`disk.forget: succeeded (phase terminal)`�?

**e2e 现在过了整条数据�?*：create �?list/show �?读写 �?checkpoint �?stop �?forget 全通过�?
失败点移�?**installed service** 阶段�?

```text
installed service state: STOPPED   WIN32_EXIT_CODE 1066
sc.exe stop MyRamDiskService failed (1062): 服务尚未启动
```

**根因（已复现�?*：一�?forget 之后 `C:\ProgramData\MyRamDisk\profiles\<id>\` 留下**空目�?*�?
`profiles\workers\<id>.workers` 也留着�?*下一次服务启动直接失�?*�?

```text
myrd-service: start failed: op=profile_registration.load
  msg=The requested profile is not registered native=0
```

空目�?`list_profiles` 会跳过，所以嫌疑在 worker binding / 启动协调路径�?
**下一�?*：① forget 连目录与 binding 一起清掉（治源头）；② 启动路径�?�?forget �?profile"
当作可回收残留而不是致命错误（崩溃中途也安全）；�?然后�?`cli_e2e -Repeat 20`�?

---


### 3.7 2026-10-26 晚：cli_e2e 首次整套通过；重�?20 次暴�?�?5 次开始卡�?reconciling"

**单次通过（第一次）**：`integration.m5.cli_e2e` �?**1 of 1 passed**，请�?
`25e8077d130d42fdbe8c12d8e6b40509`。覆�?create �?list/show �?读写 �?checkpoint �?
stop �?注册仍在 �?forget �?installed service（SCM 安装/应答/停止/删除）�?

**`-Repeat 20`�?/20**（请�?`8446da51fcaf43c0a36c0d0e240f73cf`）。前 4 次通过�?
从第 5 次起全部失败，失败读数一致：

```text
FAIL: the operation did not reach a terminal state within 300 s (last state: reconciling)
WARNING: 1 worker process(es) survived the run
```

�?**stop 进入 reconciling �?300 s 没有出来**，并且留下一�?worker 进程；此后每次运行都�?
这个残留状态挡住。这�?*累积型缺�?*，单次通过并不能代表稳定�?

**已经修掉的测试侧缺陷**（这些曾把产品缺陷伪装成测试失败，或反之）：
1. 安装服务前未停前台实例（管道冲突，SCM 7024）；
2. stop �?`--wait`（走已知坏掉�?`operation wait`）；
3. �?`disk list` �?*第一�?UUID** 当作本轮 profile（在有残留时打错盘）�?

**已经修掉的产品缺�?*：孤�?profile 拖死 create（`2e8cf32`）、启动期终态操作缺 profile 即致�?
（`6d70940`，按 profile 豁免）、forget 完成时拒�?bundle 已不存在"（`c11f6d2`）�?
forget 本身未实现（`c1ac06f`）�?

**下一�?*：查�?5 次那�?`last-service.log`/`last-trace.log` �?stop 为什么停�?reconciling�?
那个 worker 为什么没退（很可能是设备退役窗口内的阻塞，�?reconciliation 的判定条件永远不满足）�?

---

### 3.8 决定：stop �?最�?checkpoint �?dirty"要有界复检，不 park

**读数**：`cli_e2e -Repeat 20` = **18/20**（请�?`98e28d87d1ff46ea9a8905aa755cc935`），
两个失败�?2�?14）同因：live stop 停在 `reconciling` 300 s，留一�?worker�?

**根因是两个守卫互相矛�?*�?
1. stop 的最�?checkpoint �?dirty �?`resolving` + needs-attention�?
2. `remove_and_stop` 拒绝 dirty runtime（怕拆卸时丢未提交的写）；
3. 拒绝�?`park_stop_reconciliation` �?`reconciling`，而它只能靠下一次服务启动推进�?

**决定（选项 2，有界复检�?*�?
最�?checkpoint �?dirty 时，**�?flush + checkpoint 一次（最�?N=2�?*�?
干净了继续拆卸；�?dirty 才以具名失败 + needs-attention **终�?*收尾�?*�?park**�?
不拆掉任何守卫，也不改变"允许�?checkpoint 之后的写"这条既有契约�?

**实现落点（按顺序�?*�?
1. `service_core.cpp` �?stop checkpoint 分支：把「dirty �?resolving」改�?
   「dirty �?再执行一�?`flush_exact_volume` + checkpoint；仍 dirty �?resolving」；
2. `park_stop_reconciliation` 的调用点：改为终态失败（`OperationState::failed`�?
   + 具名标记（如 `stop-final-checkpoint-dirty`），不再进入 `reconciling`�?
3. 验收：先 `-Repeat 3`（快），�?`-Repeat 20`�?0/20 才算目标达成�?

**不要**做的事：不要为了通过而让 `remove_and_stop` 接受 dirty——那是唯一在防"拆盘即丢数据"的守卫�?

---

### 3.9 2026-10-26：`cli_e2e -Repeat 20 �?20/20`（第一次）

```text
integration.m5.cli_e2e  -Repeat 20  鈫? 20 of 20 passed
璇锋眰 6d8dd60cf5064f6d85a21a3ef7065112
```

覆盖：create �?list/show �?读写 �?checkpoint �?stop �?注册仍在 �?forget �?
installed service（SCM 安装/应答/停止/删除）�?

**第二次同�?20/20**（请�?`3c6697405728401fa19ab5d75e3bb93b`）⇒ **连续两次 20/20�?
合计 40 次连续通过**�?

**仍然诚实记录**：在这之前的两轮�?18/20（失败停�?`reconciling`），而且�?18/20 �?
20/20 之间**只改了测试的诊断输出、没有改产品代码**。所以那个间发缺�?*没有被根�?*�?
只是在这 40 次里没再出现。它现在的判据已经就位：下次一旦复现，失败信息会直接带�?
`service.log` 尾部�?`step=`�?

**这轮没有新的产品改动**，只�?`Wait-ForTerminal` 加了"失败时打印服务日志尾�?
（`reconciling` 被三种失败共用，不打印就只能�?它挂�?）。也就是说：18/20 �?20/20
之间没有代码变化�?*这个间发缺陷仍未根治**，只是这次没出现�?

---

### 3.10 **已完成（�?§3.13�?*：`disk start`（stop 之后重启）的接线记录——保留作原理参考，不要再按它施�?

**现象**：`disk.start �?create-runtime` 失败，`code=21000 step=select_volume_for_disk`�?
`seen=[C?@0 -?@0 -?@1 -?@0]`——卷在磁�?1 上�?*没有盘符**，而该函数第一道过滤是盘符�?

**根因**：`find_recovered_volume` �?GUID 分支（`windows_volume.cpp:685`）是对的�?
�?*上游从来没喂�?GUID**，所以它永远�?skip�?

**已经做完的一�?*（`798b0d1`）：`WorkerLaunchSpec` �?`volume_guid` 字段 + 编解码，
`unit.m5.worker_protocol` 通过�?

**还剩五处（按顺序�?*�?

1. `src/host/m5_worker_runtime.h`：`WorkerRuntimeConfig` �?`std::wstring volume_guid;`
2. `src/host/m5_worker_main.cpp`（约 175-180 行，`config.bundle = …` 那一段）�?
   `config.volume_guid = spec.value().volume_guid;`
3. `src/host/m5_worker_runtime.cpp`（约 313-320 行的 `ProductionRuntimeAdapter::Config{…}`）：
   �?`volume_guid` 传到 `expected_volume_guid`�?*注意这是聚合初始化，字段顺序敏感**�?
4. `src/management/service_worker_control.cpp` �?`make_launch_request(const ProfileRecord&)`�?
   �?durable 记录�?`request.volume_guid`。记录里的字段是
   `std::optional<product::Uuid> volume_guid`（`profile_types.h:116`），**只有大类
   `{xxxxxxxx-…}`，没有前缀**�?
5. **格式陷阱（会白跑一轮，务必注意�?*：`find_recovered_volume` 里的比较�?
   `_wcsicmp` + 只去掉尾部的 `\`，而枚举出来的 `volume.volume_guid` 形状�?
   `\\?\Volume{guid}\`。所以第 4 步必须把它拼�?**`\\?\Volume{<uuid>}\`** 再上线，
   否则 GUID 分支会匹配到 0 个、退回按盘符找、症状与现在一模一样�?

**�?13 轮补充：实际是六处，不是五处�?* 服务侧的请求结构也要带上它，否则 spec 永远是空�?

| # | 浣嶇疆 | 鍐呭 |
|---|---|---|
| 0 | `src/management/service_worker_control.h` | `WorkerLaunchRequest` 鍔?`std::wstring volume_guid` |
| 0b | `service_worker_control.cpp:118` / `:150` | 两个 `make_launch_request` 都填：durable 那条�?`profile.volume_guid` 取，create/import 那条留空 |
| 1 | `src/host/windows_worker_sessions.cpp:59` | `spec.volume_guid = request.volume_guid;` |

**已有先例可直接抄**：`service_worker_control.cpp:467` 就有
`expected_volume = profile.volume_guid.value().to_string();`——同一�?durable 字段�?
同一种取法。别忘了�?5 条的 `\\?\Volume{…}\` 前缀�?

**验收**：干净客机单次 `cli_e2e`（含 restart）→ `-Repeat 20`�?

---

### 3.11 2026-10-26 收尾：restart 的现状（交接用，未完成）

**宸插仛瀹屽苟鎻愪氦**
- `798b0d1` / `99ff704`：`volume_guid` �?durable profile 一路传�?worker �?
  `expected_volume_guid`�? 处接线，格式 `\\?\Volume{...}\`）�?
- 客机**正面证据**：`verify_volume` 打出�?`volume_guid` �?`expected_guid` 一致�?
  `actual_disk=1 expected_disk=1` —�?卷是**按身�?*认出来的，不再靠盘符�?
- `stp`：stop �?checkpoint 未解决时改为**具名终态失�?*，不 park（另一�?`resolving`
  早前已改）；`verify_volume` 的诊断改为逐条列出每个可拒绝条件�?
- `8b8e8fa`：e2e 增加 restart 校验（start �?同盘�?�?读回 stop 前写的文�?�?�?stop）�?

**鏈獙璇?/ 鏈В鍐?*
- restart **通过过一�?*（运�?`efbf62fe`，其后第二次 stop 卡在 `reconciling`），
  但之后两次在 `create-runtime` 又失败；最近一次（干净客机 `2bbffea`）仍失败且耗时
  �?5 分钟，指向又一�?300 s 等待�?*结论：restart 不稳定，未达�?T1-05 的验收线�?*
- 300 s �?`reconciling` 等待仍会出现，说明还�?`park` 落点没改成终态失�?
  （`service_core.cpp:3950` 的回滚路径尚未处理）�?

**下一步（建议新会话开头就做）**
1. 硬重�?+ �?`ProgramData\MyRamDisk\{profiles,operations}`�?
2. 单次运行�?*完整**读该 run �?`stderr`（不�?tail），确认失败�?`create-runtime` 还是
   `reconciling`�?
3. 若是 3950 �?park �?同样改为具名终态失败；若是 `create-runtime` �?�?
   `Diagnostic verify_volume` 的逐条输出定位�?

---

### 3.12 **已被 §3.13 取代**：stop �?remove_mount �?runtime 仍然 dirty"拒绝（`278d63e` 已处理）

**读数**（`run-fe0a8a2a` 的服务日志）�?

```text
disk.stop 鈫?checkpoint completed 鈫?evidence-recorded profile state published
disk.stop 鈫?lifecycle-action failed
code=23559 step=service_worker_control.remove_mount
detail=The worker refused to remove: its runtime stands dirty
�?操作停在 reconciling�?00 s 超时，残�?1 �?worker
```

**含义**：冻�?�?flush �?checkpoint 提交 �?拆卸，前三步都成功，第四步被拆卸守卫拒绝�?
守卫本身是对的（dirty 时拆盘等于丢数据）；问题�?*没有任何路径能在"提交后仍有残余写"时安全收�?*�?

**修法（两处选一，建�?A�?*

- **A. 有界复检**：拆卸被拒且原因�?dirty 时，再做一�?`flush + checkpoint`，然后重�?
  `remove_mount` **一�?*；第二次�?dirty 则以具名终态失败收�?
  （`stop-dirty-after-recheckpoint`），不再 park�?
- **B. worker 侧自�?*：让 `remove_and_stop` 在判�?dirty 之前�?flush 一次并重新�?dirty�?
  鏀瑰姩鏇村眬閮紝浣嗚纭 flush 涔嬪悗 dirty 璁℃暟纭疄浼氬綊闆躲€?

**落点**：`src/management/service_core.cpp` 的动作失败分支（`remove_mount` 的失败处理）
�?`src/host/m5_worker_runtime.cpp` �?removal 路径。改完先单次，再 `-Repeat 20`�?

**已验证通过的部分（本次会话的净收益�?*：`volume_guid` 六层接线、`verify_volume` �?
ownership 判据（attach-only 不再要求 owned）、恢复路径先分配盘符 —�?restart 已能�?
`create-runtime` 一路走�?重启后再�?stop"�?

---

### 3.13 当前真实位置�?026-10-26 晚，�?§3.12�?

**restart 已打�?*：一次运行的服务日志显示 `disk.start` 的八个动作全�?
`action-result: completed` 并到�?terminal�?

```text
create-runtime 鈫?recover-bundle 鈫?start-proxy 鈫?create-device 鈫?
locate-physical-drive 鈫?verify-volume 鈫?assign-mount 鈫?readiness-io 鈫?terminal
```

**剩下的失�?*：紧接着�?*第二�?stop** 以终态失败结束（快速，不再�?300 秒）�?

```text
kind=7 command=disk.stop phase=terminal
  message=terminal failure without a worker-action error; marker=flush
```

`marker` �?`finalize_operation` 收到的终止标记，内容恰好�?*动作�?* `flush`——即这次
stop 失败�?`flush` 这个动作上，�?*不是 worker 动作返回的错�?*（`action-start: flush`
甚至没有写出来，说明失败发生在动作启动阶段，例如 runtime 未绑定）�?

**§3.12 的结论已被取�?*：不再是 `remove_mount` 拒绝 dirty（`278d63e` �?
flush-before-refusal 让那条不再触发），现在前移到 flush 本身�?

**下一�?*：查 `service_core.cpp` �?用动作名�?marker"的那�?finalize 调用点，
确认第二�?stop �?flush 阶段失败的具体原因（runtime 是否已绑定、卷锁是否在手）�?
修完再单�?�?`-Repeat 20`�?

---

### 3.14 2026-10-26 晚：restart + 两次 stop 全通，`cli_e2e -Repeat 20 �?20/20`

**验收读数（干净客机�?*：`integration.m5.cli_e2e` × 20 = **20/20**�?
请求 `ad796d29c8c04ef58b71766fdaa28a3b`�?0 次共 5 �?50 秒（12:07:48 �?12:13:38Z）�?
姣忔杩唬閮借蛋瀹?create 鈫?checkpoint 鈫?stop 鈫?**restart** 鈫?**绗簩娆?stop** 鈫?forget 鈫?
安装服务 install/answer/stop/delete。对照：修复前同一命令 **18/20**�?*17/20**，两次不同批次�?

#### �?先加诊断（只加日志，不改行为�?

`rollback_stop` 之前把失败原因写进服务日志；同一�?lambda 复用�?quiesce / freeze /
flush / resume，并在两�?park 点各写一行。首次单次运行立刻读出原因：

```text
kind=7 message=stop action failed: action=flush_exact_volume outcome=error
      domain=3 code=20999 native=0 step=flush detail=volume ownership contract is incomplete
```

`marker=flush` 只是�?*动作�?*当标记（`finalize_operation` 收到的终止标记）�?
错误本身�?`rollback_stop({...})` 里被丢弃——这就是上一�?日志只剩 marker"的全部原因�?

另外：e2e 每次迭代把自己的 `service.log` / `trace.log` 另存�?
`C:\m5\runs\<work-root>.{service,trace}.log`�?*这一条是必需�?*：`last-*` 会被下一�?
迭代覆盖�?/20 那次失败时只剩下"最后一轮通过"的日志，等于没有证据�?

#### �?根因一：按身份认回的卷"不属于自�?（已修，`windows_volume.cpp`�?

`Win32VolumeAdapter::find_recovered_volume` 鍦?durable GUID 鍛戒腑鍞竴鍊欓€?鐨勫揩璺緞
直接返回观测到的卷，�?`enumerate_native_volumes` 给每个卷都打
`operation_owned=false`（`windows_volume.cpp:490`）。同函数的另外两条选择路径
（`select_volume_for_disk`）都会补 `owner_token` / `drive_letter` / `operation_owned=true`�?

于是**只有"重启后按身份认回�?这条�?*拿到一个无主的卷：`verify_native_volume_identity`
的所有权前置（`windows_volume.cpp:302`）在 flush 时拒绝。�?`disk.create` 建出来的�?
一直是有主的，所�?*第一�?stop 永远正常、第二次 stop 必挂**——这正是 §3.13 的现象�?

修法：快路径补上同样的三个字段（不动任何守卫）。修完单次运行全套通过
（请�?`696c778500974694a68558a38b80c6cb`�?4 秒）�?

#### �?根因二：stop 的写冻结从未运行过（T1-11 的死分支，已修）

20× 随即暴露第二层：**18/20**，两次都是�?00 s 没到终态，last state=reconciling」�?
诊断�?**17/20** 拿到了原因，两个签名�?

```text
action=lifecycle-action code=23559 step=service_worker_control.remove_mount
  detail=The worker refused to remove: its runtime stands dirty
鈫?stop parked at the removal boundary: action=lifecycle-action stop_phase=5

checkpoint: disposition=3 �?           （committed_needs_reconciliation�?
鈫?checkpoint parked for reconciliation: phase=checkpoint-resolving
```

两者是同一件事�?*最终提交之后卷仍可�?*，于是提交后又有 dirty。而写冻结
（T1-11，`b3fae6f` 引入）本该是那道闸：

```text
state_machine：stop 计划 = quiesce_mutations, freeze_writes, flush_exact_volume, checkpoint_if_required, �?
service_core：quiesce 块把 phase 置为 quiesced 并发�?
              绱ф帴鐫€鐨?freeze 鍧楀畧鍗啓鐨勬槸 `stop_state.phase == admitted`  鈬?姘镐笉鎴愮珛
              之后 execution_actions.erase(begin, begin + 3) 仍按"前三个已执行"�?freeze 删掉
```

`git show b3fae6f` 里顺序已经如此，所�?*�?T1-11 起这条分支就是死代码**，计划里�?
`freeze_writes` 被当�?已执�?跳过。T1-11 当时那份 20/20 来自 worker �?
（`remove_and_stop` �?flush 再判 dirty、无共享独占打开有界重试），不是 service 侧这个动作�?

修法：守卫改�?`quiesced`（quiesce 之后、flush 之前）；worker �?
`DiskRuntime::freeze_writes` 幂等，`volume_flushed` 的续跑路径仍跳过�?

**客机正面证据**（`C:\m5\last-trace.log`�?0/20 那轮）：

```text
Win32VolumeAdapter::lock: volume locked
dismount: through the retained lock          鈫?鎷嗗嵏澶嶇敤鍐荤粨閭ｆ妸閿侊紙T1-11 鐨勭 鈶?鏉¤姹傦級
```

并且所�?stop 的最终提交都�?`checkpoint: disposition=2`（`committed`），
不再�?park，也不再出现 `23559`�?

#### �?根因三：重启后卷还没发布就被枚举（已修，`runtime_adapter.cpp`�?

开冻结�?20× 变成 **17/20**，但失败前移�?restart �?*变快**（整�?6 分钟，不�?300 s）：

```text
command=disk.start message=action=create-runtime code=21000 native=1168
  step=find_recovered_volume detail=no volume carries the recorded identity
```

新建设备后卷尚未被枚举到（`ERROR_NOT_FOUND`），下一次迭代同样能起来 �?是发布延迟�?
`ProductionRuntimeAdapter::find` 现在�?`not_found` �?*有界复检**�? s�?50 ms 一次）�?
**且只在有 durable GUID �?*才复检：没有记录身份时"找不�?就是答案（reclaim 的残留可�?
真的没了），不该等�?

#### �?顺带修掉：三处陈旧期望（`6e1bfcc` 遗留�?

`m4_runtime_adapter_tests`、`m4_disk_runtime_tests`（开发机）与
`integration.m4.multi_instance`（客机整套时才暴露）都是红的�?*与本次改动无�?*�?
`6e1bfcc` 把恢复路径改�?先分配盘符再校验身份"（所有权 token 在文件系统上，没挂载点读不到），
三处 fake 的期望顺序仍�?`verify �?assign`。已改成 `assign �?verify`（`multi_instance` 是按下标
断言�?`events[6]/[7]`，一并改），并把
`m4_disk_runtime_recovery_size_mismatch_does_not_mount` 改成它真正想说的事：
**失败的重启不许留下挂�?*（现在断言盘符被收回：`assign-letter �?verify �?remove-letter �?
dismount �?aim-stop �?unlock`）。七�?fake-only 套件开发机全绿�?

**下一�?*：T1-05 可判 PASSED（create/checkpoint/stop/restart/第二�?stop/forget + 安装服务�?
20/20）；T1-06 起继续�?*未验�?*：`stop` �?`require_clean` 策略分支（e2e 只走
`checkpoint_required`）；park 仍保留给"真正 dirty"的情形（现在不再触发，但路径还在）�?

#### �?整套回归（客机，硬重�?+ 清空后）

**151/154**（请�?`8ec8aa88c34740308c2098e9dc90a7f3`�? 分钟），三个失败都不是本次改动：

| 澶辫触 | 鍘熷洜 | 澶勭疆 |
|---|---|---|
| `structural.m2.storage_independent_build` | 客机�?Visual Studio：`vswhere.exe not found` | 环境受阻，历次基线同项，**不是产品失败** |
| `structural.m5.execution_boundary` | 同上 | 同上 |
| `m5.production_execution_backend` | 批内挂在 `WorkerBindingStore::over_protected_root`（`store.ok()`，`m5_production_execution_backend_tests.cpp:514`�?| 单跑 **3/3 通过** �?批内间发（`m5-vm-harness.md` 已列的已知间发之一），不是本次改动 |

同日更早那次整套也是 151/154，第三个失败�?`integration.m4.multi_instance`
（`events[7] == "assign-letter"`，与 §�?同源），已修，单�?
`multi_instance + cli_e2e` �?**2/2**�?*本分支整套的期望值是 152/154**
（两�?vswhere 永远红），外�?`m5.production_execution_backend` 在批内的已知间发�?

---

### 3.15 2026-10-26 夜：T1-06 完成（中断提交判定可达）

**完整体验文档**：`docs/validation/m5-t1-06-recovery-proof.md`。要点：

1. **�?P4 �?launch 起只读检�?worker**：`reconcile_checkpoint` 在没�?session 时从 durable
   profile **launch 但不 start**，问 `recovery_proof`，每个分支都关闭 inspection 并等进程退出；
   launch 失败作为失败上报。改前用 `require_session`，只�?adopt 活着�?runtime——而本问题
   恰恰�?提交已在 P4、worker 已不�?，所�?*正面路径一次都跑不�?*�?
2. **发现一个让确认提交永远不可能的缺陷**：候选判据六条里�?prior generation 比的�?manifest �?
   `base_generation`（基底镜像代次，提交 checkpoint 时不前进），�?correlation �?prior 是上一�?
   checkpoint。客机读�?`mask=0x2`，其余五项（镜像 uuid、代次、native id、slot、digest�?*全相�?*�?
   修法：候选判据回到规格五条；prior 只用于否定判据；`CheckpointRecoveryInspection::prior_generation`
   改名 `base_generation`。修�?`verdict=2 mask=0x0`�?
3. §14.3 七条全部有读数（含真 worker/�?bundle 的三条正�?否定路径、只读性指纹）�?

**下一�?*：T1-07（内存可见�?+ UNMAP 端到端）�?

### 3.16 2026-10-26 夜：T1-07 完成（内存可见�?+ UNMAP 端到端）

**验证文档**：`docs/validation/m5-t1-07-memory-visibility.md`。要点：

1. `resident_bytes` / `budget_bytes` 从存储层计数器一路到 CLI：`disk list` / `disk show` 行尾
   出现 `mem=<resident>/<budget>`�?*来源是存储自己的 committed_bytes，不是进程工作集**�?
2. **UNMAP 首次端到�?*：写 16 MiB �?resident 2 949 120 �?**19 660 800**（恰�?+16 MiB）→ 删除�?
   回到 **2 949 120**。budget 1 073 741 824，worker 工作�?22 573 056（同量级）�?
3. **第一次量不到，原因值得�?*：普通写入后 resident 完全不动，数据还�?Windows 文件缓存里；
   �?*写穿** + 有界轮询才量到存储。所�?写文件后内存立刻上升"�?Windows 上是缓存错觉�?
4. 每层都钉了用例（协议 round-trip、截断报 malformed、observation 只在 alive 时带值、wire、渲染）�?

**验收**：干净客机 `cli_e2e -Repeat 20 �?20/20`（式 `541c8b4a…`�? �?10 秒）�?

**延后**：`disk release-memory`（�?.4）——它需要一条从 storage 公开 `reclaim_to` �?CLI
动词的完整纵切，与内存压力同源�?*T1-08 之后仍未做，�?§3.17「未做」�?*

---

### 3.17 2026-10-26 深夜：T1-08 完成（检查点策略终于有消费者）

**验证文档**：`docs/validation/m5-t1-08-checkpoint-policy.md`。要点：

1. 四个策略字段（interval / dirty 阈�?/ 内存压力 / 休眠）从"存下来就�?变成真的会触发�?
   **客机读数**：`generation moved 2 -> 3 on its own after 30.4 s`，全�?*没有发过 checkpoint 命令**�?
2. 落点：纯函数 `checkpoint_due_reason()` + `ServiceMaintenance`（SYSTEM 身份 dispatch�?
   **只做持久化准�?*，执行仍归调度器�? 调度�?claim-once tick�?5 s�?
   `SERVICE_ACCEPT_POWEREVENT` / `PBT_APMSUSPEND �?ServiceHost::suspend()`�?
   suspend **不是 stop**：设备不拆、worker 不退，只做尽力而为的检查点�?
3. **接线时量到两个缺�?*：① interval 时钟没有起点（`since` �?0 �?永不到期）；
   �?命令寻址用了 mount target 而非 profile uuid（每�?`The requested profile is not registered`）�?
4. **一条范围边�?*：两块盘同时活着时，第二块的拆卸不会真正移除设备（restart 找不到卷�?
   forget `device_absent=0`）。M5 是单盘产品（G4 延后），所�?e2e 改成一次一块盘�?
   这条不是策略缺陷，但值得记住�?
5. `forget` 的九个合取项现在拒绝�?*逐项打印**（和 `verify_volume`、checkpoint boundary 同源）�?

**验收**：干净客机 `cli_e2e -Repeat 5 �?5/5`（请�?`7aa88f14…`�? �?9 秒）�?

**未做（记录，不当作已完成�?*�?
- `disk release-memory`（�?.4 手动释放）。它�?T1-07 �?T1-08 各被延后一次，理由不变�?
  闇€瑕?storage 鍏紑 `reclaim_to` 鈫?M3 鈫?M4 runtime 鈫?worker 鍛戒护 鈫?鏈嶅姟鍔ㄤ綔/璁″垝 鈫?CLI 鍔ㄨ瘝
  的完整纵切�?*它不在完成口径里**（口�?= G1+G2+G3 验收 + T4-04 manifest + master-plan §8
  的遗留清单，而该清单四项里没有它），但设�?§7.4 承诺过这个命令，所以在这里留名�?

**下一�?*：T1-09（M5-A 单盘验收 + 断电恢复）。它依赖 T1-06/07/08/11，现在四个都齐了�?

---

### 3.18 2026-10-26 深夜：T1-09 进行中（开机自动恢复已完成，断�?harness 待打通）

**已完成并提交（`51e164d`�?*：开机自动恢复的消费者�?
- `ServiceCore::admit_auto_starts()`：启动时�?durable 状�?= running �?`auto_start` = true"�?
  profile 逐个重新**准入** `disk start`（准入而非执行，执行仍归调度器）�?
- `ServiceHost::start()` �?recovery �?sweep �?scheduler 之后、公开面打开之前调用它；失败�?
  启动失败（与 recovery/sweep 同规），条数�?`ServiceHostDiagnostics::auto_started_operations`�?
- 没有这段，断电后字节会被恢复、盘却以 stopped 回来等人手启—�?最多丢一个间�?只兑现一半�?
- 两个模型用例：会回来的那张盘被且仅被准入一次（`disk.start`）；`auto_start=false` 的盘不碰�?
- 顺带两处 fixture 修正（都�?拒绝要能定位"逼出来）：`resolve` 现在和产品一样接�?uuid�?
  失败日志�?`step=`�?

**已写好、未打�?*：断电验�?harness
- `tests/e2e/test-m5-powerloss-prepare.ps1`（装 auto-start 服务、建 30 s 间隔的盘、置 auto_start�?
  写标记、等一次自动提交、把"必须活下来的事实"写进 `C:\m5\powerloss-state.txt`�?
- `tests/e2e/test-m5-powerloss-verify.ps1`（只看不动：服务是否自己起来、盘是否自己回来�?
  generation �?提交值、标记字节一致、worker 恰好一个）
- `tools/m5vm/Test-M5PowerLoss.ps1`（host 侧三段：prepare �?`vmrun stop hard` �?开�?�?verify�?
  判定�?harness 自己�?`N of M passed`，因�?**harness 在用例失败时仍退�?0**——这条已经踩过）
- CMake 已注�?`integration.m5.powerloss_prepare` / `..._verify`�?55 �?157 例）

**2026-10-27 00:52 第六次读数（干净三段跑完；剩下的问题�?服务 RUNNING 但没有公开管道"�?*

干净客机（硬重启 + �?ProgramData + 删服务）跑完整三段：
- `prepare` **1 of 1 passed**（请�?`b0cb01b9…`�?2 s�?
- 硬断�?+ 开�?+ `verify` �?**0 of 1**，请�?`1280adbd…`，耗时 187 s
- 187 s �?`Wait-ForService 180` + 7 s，即**第一步就失败**：CLI 连不�?

断电后直接手动确认（不再�?harness）：

```text
myrd-cli disk list 鈫?exit=3, 30016 ms
  "myrd-cli: the service is not available (is it installed and running?)"
sc query MyRamDiskPowerLoss 鈫?STATE: 4 RUNNING
```

**所以现在的未解问题是产品侧�?*：断电重启后 **SCM �?RUNNING，但公开管道不存�?*（或不可连接）�?
`ServiceHost::start()` 若打开公开面失败会把状态置 failed 并让服务�?STOPPED，所以要�?
start() 根本没走�?open，要�?open 成功而管道对 CLI 不可达�?

**下一次要查的顺序（都很便宜）**�?
**2026-10-27 00:55 追加（关键一步已经查了）**：断电后那台客机�?

```text
Get-Process myrd-service 鈫?娌℃湁杩涚▼
\\.\pipe\ 閲?myrd*     鈫?娌℃湁浠讳綍绠￠亾
C:\ProgramData\MyRamDisk\logs\service.log 最后写�?00:46:32（在断电之前�?
```

�?`sc query` 在几分钟前还�?RUNNING�?*结论：断电后的服务进程自己退出了（疑似崩溃）�?
公开管道因此从没建立起来**。所�?T1-09 剩下的不是一�?恢复逻辑"问题，而是
**"重启后服务进程会�?**——下一步必须看 **Windows 应用程序事件日志**（Event ID 1000 /
WER）拿崩溃模块与偏移，再决定是修代码还是修环境。`service.log` �?00:46:32 之后没有任何
新行，说明服�?*在这次开机里连一条日志都没写**——它很可能在 `ServiceHost::start()` 之前
（或刚开始）就死了，这一点与"崩溃"比与"逻辑卡住"更吻合�?

**2026-10-27 01:00 再追加（证据互相矛盾，必须按时间戳重测，不要再猜�?*

同一台客机、几分钟之内�?

| 鏃跺埢 | 瑙傚療 |
|---|---|
| 00:52 | `myrd-cli disk list` �?exit 3「service is not available」，30 s；`sc query` �?**RUNNING** |
| 00:55 | 没有 `myrd-service` 进程、没有任�?`myrd*` 管道；`service.log` 最后一�?**00:46:32**（断电前�?|
| 01:00 | `sc query` �?**1060 服务不存�?*（`service.log` 仍是 00:46:32�?|

三条观察不能同时为真：RUNNING 的服务不能既没有进程、又没有管道，更不能在没有人删它的情况下
变成"不存�?。可能的解释有三类，**都必须用带时间戳的重测来区分**�?
(a) 我读到的 RUNNING �?*上一次准备阶�?*的输出（命令输出串了）；
(b) 客机在两次检查之�?*重启�?*（例如我加的 boot 重试或客机自己复位），服务因此回�?
    "未安�?（`C:\ProgramData` 里的 profile 目录还在，说明不是快照回滚）�?
(c) 服务装在一�?*每次开机都会消失的位置**（例�?binPath 指向 staging 目录而被清理）�?

**下一次的做法（写成清单，照做�?*�?
1. 干净客机 �?`Test-M5PowerLoss.ps1` 只跑�?prepare（`-ResumeVerify` 之前手动停）�?
2. **立刻**记录：`sc qc`（binPath/start 类型�? `sc query` + `Get-Process myrd-service` +
   `\\.\pipe\` 过滤 + `service.log` 行数 + `Get-Date`，作�?*基线快照**�?
3. 断电 �?开�?�?**�?15 �?*采一次同样的快照，连�?3 分钟并带时间戳输出；
4. 用快照序列判断上�?(a)/(b)/(c) 哪一类，再动代码�?

**2026-10-27 01:05 结论：上一轮的"矛盾"是测量通道的问题，不是产品�?*

�?*带时间戳的快�?*（`C:\m5\snap.ps1`，客机本地跑）在断电重启�?31 秒采到：

```text
01:03:38 state=RUNNING pid=3164 start=01:03:07 pipes=0 loglen=8605
sc qc 鈫?START_TYPE: 2 AUTO_START
        BINARY_PATH: 鈥out\build\msvc\src\Release\myrd-service.exe
```

而同一时刻服务自己的日志已经写�?`disk.start �?terminal`—�?*断电后的那次 start 走完了全�?
动作**（`start-proxy �?create-device �?locate-physical-drive �?verify-volume �?assign-mount �?
readiness-io �?terminal`）�?

于是三条"矛盾"观察的解释是�?*普�?ssh 会话不是提权会话**（AGENTS §5：SSH 是网络登录、不提权），
鎵€浠?
- `\\.\pipe\` 枚举可能被拒 �?我看到的 `pipes=0` **不是**"没有管道"�?
- 非提权的 `myrd-cli disk list` 拿不到公开管道 �?我看到的 `service is not available` **不是**
  "服务没起�?�?

**教训（写�?lessons�?*�?*凡是要碰公开管道、进程、服务的测量，只能走 harness 的提权通道
（ctest 用例 + scheduled task），不能用普�?ssh 会话下结论�?* 我上一轮据此写下的
"服务进程死了""服务不存�?两条都要收回�?

**下一�?*：把快照做成一�?ctest 用例（提权通道），�?verify 之前打印一次；然后用它替代
我这几轮的手�?ssh 判断，重跑一次三段。verify �?`Wait-ForService` 若仍超时，那才是真问题�?


   `StartTime` 鏄笉鏄繖娆″紑鏈猴紱
2. `C:\ProgramData\MyRamDisk\logs\service.log` 里这次开机的最后几行—�?*有没�?* `auto-start`
   �?subsequent �?action 行（如果日志停在 `auto-start: …retired…` 之后，说�?start() 卡在
   `admit_auto_starts` 或调度器启动里）�?
3. `\\.\pipe\` 里有没有公开管道名（`Get-ChildItem \\.\pipe\ | ? Name -match 'myrd'`）；
4. 若服务日�?*完全没有**这次开机的行，说明服务进程不是这一版二进制（staging/服务 binPath 不对）�?

**T1-09 已完成的部分**（有读数）：断电后服务自起、盘自起、generation �?提交值、脏数据重放
提交成下一代（`0af35d5` 的提交信息里有完整读数）�?*未完�?*：verify 端到端一次绿�?
以及上面那条"RUNNING 但无管道"的原因�?

**2026-10-27 00:45 第五次读数（T1-09 的产品部分已全绿；剩下的全在 verify 脚本与客机侧�?*

**断电后连续两次读到的产品事实**（这才是 T1-09 要的）：

```text
service state: RUNNING（脚本从未启动它�?
5003a664-鈥?T: ready running runtime=Running activity=Idle
            persistence=Dirty health=Healthy generation=4  mem=1048576/1073741824
recovered generation: 4 (last committed before the cut: 3)
```

即：**服务自己起来、盘自己回来、generation �?断电前提交值、resident 内存 1 MiB**�?
`persistence=Dirty` �?generation �?3 走到 4，说明恢复把掉电时的脏数据重放并提交了一�?
——这正是"最多丢一个间�?的实测形态�?

**本轮修掉的三处脚�?工具缺陷**（都�?让产品看起来坏了"的那类）�?
1. 状态文�?*没落�?*：断电后回来�?0 字节 �?�?`FileStream.Flush($true)` 写；
2. `verify` �?`Join-Path $mount (powerloss-marker.txt)` 少引号（`CommandNotFoundException`）；
3. 编排器加 `-ResumeVerify`（只�?verify）与 boot 失败自动重试一次�?

**最后一个未解的现象**：`-ResumeVerify` 那次 `sc query` �?RUNNING，但 CLI 连不上公开管道
（`Wait-ForService 180` 超时）。两次读数之间我前一�?verify 失败�?*没有做清�?*，所以极可能�?
**上一个服务实例的管道残留**。下次：断电重启 �?完整跑一次三段（不要 -ResumeVerify），
若仍�?`Wait-ForService` 失败，先�?`sc query` �?PID �?`C:\ProgramData\MyRamDisk\logs\service.log`
是否来自同一进程�?

**下一步（新会话从这里开始）**：干净客机 �?三段一次跑�?�?�?
`docs/validation/m5-t1-09-power-loss.md`、tasklist T1-09 �?PASSED �?�?T1-10�?

**2026-10-27 00:35 第四次读数（prepare 稳定通过，断电后客机本身有概率起不来�?*

- `prepare` 在干净客机�?*连续两次通过**（请�?`d0667c91…` 50 s、`8eae57a7…` 52 s）：
  �?auto-start 服务 �?�?30 s 间隔的盘 �?`auto_start` �?写标�?�?等自动提�?�?落状态�?
- 状态文件改 JSON �?`verify` 仍报 `invalid JSON primitive`（写读两端对编码的处理仍不一致）�?
  于是**改成不依�?JSON 的容错读�?*（一条正则同时接�?JSON �?`key=value`，失败时打印全文）�?
- **硬断电之后客机有概率起不�?*：这�?600 s �?SSH 无响应（累计第二次）。主机侧
  `vmrun stop hard` + `start nogui` 之后 15 s 就好了，说明�?*客机启动偶发挂死**，不是网络配置�?
  �?编排器需要：boot 等待超时�?*自动断电重试一�?*，并且提�?`-ResumeVerify`
  （prepare 与断电都已完成、状态文件仍在时，只�?verify，不重跑前两段）�?
- 断电后的那次 start 已经走完整真实序列（`6177380` 的修复），所以下一轮要看的只剩
  **verify 鐨勪笁鏉℃柇瑷€**鑳藉惁璇诲埌姝ｇ‘鐨勭姸鎬佹枃浠躲€?

**下一步（新会话从这里开始）**�?
1. 给编排器�?`-ResumeVerify`（跳�?prepare 与断电，直接 verify）与 boot 失败自动重试一次；
2. 干净客机跑一次完整三段；verify 通过�?T1-09 完成（读数写�?
   `docs/validation/m5-t1-09-power-loss.md`，tasklist T1-09 �?PASSED）；
3. 然后�?T1-10�?

**2026-10-27 00:05 第三次读数（harness 已能判绿，客机会在两处卡死）**

- harness 的判定修好了，两处都�?*量出来的**�?
  �?harness �?`Write-Host` 报判定，走的�?information stream，`2>&1 | Out-String` **抓不�?*�?
  于是包装器对着"屏幕上明明印着 `0 of 1 passed`"�?run �?没有判定�?；改�?`6>&1`�?
  �?staging 阶段印的是数量不是判定，用同一条规则会把刚成功�?stage 判成失败；so stage 只看退出码�?
- **`-SkipStage` 会让客机跑旧�?e2e 脚本**（脚本随 `-Stage` �?git 归档下发）。这解释�?
  "uuid 解析已修却仍然报同一个错"——那两次客机跑的是旧文件。以后改 e2e 脚本必须�?`-Stage`�?
- **客机会卡�?*（两次，都在 prepare 阶段前后）：SSH 无响应，只有硬断电才能恢复；
  两次都是先跑过一轮断电实验之后�?*下一次尝试前先硬重启**，并�?跑前重启"当作这个
  harness 鐨勫浐瀹氬墠缃紝鑰屼笉鏄緥澶栧鐞嗐€?

**下一步（新会话从这里开始）**�?
1. 硬重启客�?�?�?`ProgramData\MyRamDisk\{profiles,operations,logs}`�?
2. `Test-M5PowerLoss.ps1`�?*不带** `-SkipStage`），一次跑完三段；
3. �?prepare 再次卡死：先�?`sc query`/`Get-Process` 确认�?*服务**还是**客机**卡，
   再把 prepare �?装服务→start"这一步拆成独�?ctest 用例（现在它�?create 在一个用例里�?
   一卡就分不清）�?
4. 断电�?`verify` 若仍�?`blocked/needs-attention + PersistenceBlocked`，读
   `C:\ProgramData\MyRamDisk\logs\service.log`（本轮新增的文件日志，`c97e9cc`）里
   `auto-start refused: step=鈥 閭ｄ竴琛屸€斺€斿師鍥犲氨鍦ㄩ偅閲屻€?

**2026-10-26 深夜 第二次读数（prepare 已通，卡在断电后的 start�?*

- `prepare` **通过**（请�?`a9fb722dd5854923b25e754637f74ed4`�?2 s）：�?auto-start 服务 �?
  寤?30 s 闂撮殧鐨勭洏 鈫?`auto_start=true` 鈫?鍐欐爣璁?鈫?**鑷姩鎻愪氦鍒?generation 3** 鈫?鐘舵€佽惤鐩樸€?
  第一次失败的原因不在脚本：客机上残留着上一次尝试的服务�?profile，`sc start` 与旧实例冲突�?
  清干净后一次通过（`sc start` 之后 stdout 断在 STATE 块是**旧实�?*的表现，不是脚本 bug）�?
- 硬断电（`vmrun stop hard`�? 开机后�?*服务自己起来�?*（`sc query` RUNNING，开�?23:04），
  说明 `start= auto` �?SCM 路径成立�?
- `verify`（请�?`05e2971d25474ee79b2feb2b4faeeca7`）读到：

```text
6b710f86-鈥? U:  ready  blocked/needs-attention  runtime=Running
                persistence=PersistenceBlocked health=NeedsAttention
                generation=3  mem=0/0
recovered generation: 3 (last committed before the cut: 3)
```

即：**盘被自动拉起了（`runtime=Running`，generation 等于断电前的提交值），但运行面是坏的**—�?
`PersistenceBlocked`、`mem=0/0`、盘�?U: 不存在、也没有 worker 进程。所�?T1-09 剩下的问题是
**断电后的那次 start 没真正接上卷**，而不�?没有自动恢复"�?

**下一步（第一件事�?*：查断电后那�?`disk start` 失败在哪一步。可用的入口�?
- `ServiceHostDiagnostics::auto_started_operations` 只给条数，不给原因；installed 服务�?stderr
  �?SCM 丢掉，所�?*必须**让这条原因落进服务自己的日志（`ServiceLogEvent`）而不�?stderr�?
- 可疑点与 §3.18 前面记的"两块�?现象同源�?*崩溃残留的设�?句柄**�?`create-runtime` 接不�?
  （`find_recovered_volume` 找不到卷，或 `start_aim` 拒绝）。T1-03 �?startup sweep 本该先回�?
  残留，sweep �?auto-start 之前运行——所以要确认 sweep 是否真的把残留当"可回�?处理了�?
- 再顺带修一处脚本脆弱点：`verify` 在盘符不存在时抛 `DriveNotFoundException`（现在被 `trap`
  捕获并打印，但不是一条可读的失败），应改�?读不到标�?的具名失败并附上 `disk list` 行�?

---

## 4. 构建与测量循�?

```powershell
# **每一个新会话、每一次构建之前：**
#   �?dot-source 就没�?LIB/INCLUDE，链接会�?LNK1181（bcrypt.lib）上失败—�?
#   看起来像�?SDK，其实是环境没进来。冷会话一定会撞上�?026-10-26 复现并记�?
#   `m5-review-2026-10-26.md` §3.3；`m5-lessons.md` 里当时没写，别以为写过）�?
Set-ExecutionPolicy -Scope Process Bypass -Force
. .\tools\Enter-MsvcEnvironment.ps1

# 构建（改过任何头文件 �?必须�?--clean-first，见 lessons §1�?
cmake --build out/build/msvc --config Release --clean-first

# VM 回路（先在开发机构建，再送进客机；执行只在客机）
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
tools/m5vm/Invoke-M5Vm.ps1 -Probe
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release
tools/m5vm/Invoke-M5Vm.ps1 -Run -MinTimeoutSeconds 180
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '<用例�?' -Repeat 20
tools/m5vm/Invoke-M5Vm.ps1 -Run -RequestId <id>
```

整套一遍约 9�?1 分钟�?*`-Stage` 不是可选的**：二进制在开发机构建，执行只在客机�?
**`-Stage` 失败会留下脏客机**（孤�?worker + 多余�?+ 残留盘符）⇒ 先硬重启再重试�?

---

## 5. 环境事实

| 项 | 值 |
|---|---|
| 客机 | VMware Workstation，Windows 11 x64，MAC `00:0C:29:02:90:C1` |
| 地址 | `192.168.180.129`，SSH 别名 **`myrd-vm`**（脚本一律传别名，不硬编码地址�?|
| 客机账户 | `usr` / `123`；密�?`~\.ssh\id_ed25519` |
| VMX | `C:\Users\JimmyLin\Documents\Virtual Machines\Windows 11 x64\Windows 11 x64.vmx` |
| vmrun | **`-T ws` 必须是第一个参�?*；VM 加密，每条都�?`-vp 12345678` |
| 驱动 | `phdskmnt` 必须 Running�?*不得安装/替换/升级/卸载** |

**一条与仓库无关、但会浪费时间的工具事实**：本会话�?agent shell �?
`workspace-write` 沙箱�?*每个子进程都�?`0xC0000142`（`STATUS_DLL_INIT_FAILED`）退�?*�?
�?`Get-Process` 都起不来；切�?`danger-full-access` 后正常�?
**看到这个错误码先怀疑沙箱模式，不要怀疑构建或仓库�?*

---

## 6. 待裁决（只有这些需要停下来问）

| # | 事项 | 状�?|
|---|---|---|
| 1 | Primo "Mini Tools" 的范围定�?| **未定�?* �?不列入范�?|
| 2 | T4-02 鍗曟枃浠?VHDX 瀵煎嚭 | 宸茶鍐筹細淇濈暀 |
| 3 | 完成口径 | **已裁决（2026-10-17�?*：G1 + G2 + G3 全部验收 + T4-04 manifest 冻结 + §8 遗留清空；G4 延后 |
| 4 | G4 何时恢复 | 未定；恢复前置条件见 master-plan §6 |

其余（含 T1-05 �?`stop` 语义）都在实现权限内�?*自主推进，不要反复问**�?

---

## 7. 硬性规则（摘四条，全文�?`m5-lessons.md`�?

1. 改任何头文件之后、测量之前必�?`--clean-first`；否则得�?`0xC0000005` 假崩溃�?
2. **单次绿是弱证�?*；修复类结论 �?`-Repeat 20`，任�?�?的声明必须写明跑了几次�?
3. 开发机只跑**不创建真�?worker / AIM �?/ Windows �?*的测试；不确定时�?只能�?VM"�?
4. 断言失败必须打印 `operation / message / native / code`——一次客机往�?5�?0 分钟�?
   盲猜的代价是整轮�?

---

## 8. 历史（已归档，不要再引用�?

`feature/m5-unified-integration`、`feature/m5-supervisor-service` �?2026-09 的两条实现线�?
已被 v3 取代；工作树与分支已�?2026-10-25 删除，仅�?tag
`archive/m5-unified-integration`、`archive/m5-supervisor-service` 保留历史�?
**当前代码里没有任何东西依赖它�?*；`docs/m4-m5-scope.md` 里对它们的描述属于历史背景�?

---

### 3.22 2026-10-27 01:15：T3 开始（T3-01 第一部分已落地）

**已提交**：`src/product/service_view.{h,cpp}` —— 客户端可见的**中立词汇**
（`ServiceLifecycle` / `Runtime` / `Activity` / `Persistence` / `Health` / `Aggregate` /
`OperationState` / `Disposition`，加 `ServiceDiskView` / `ServiceOperationView`）。
值就是线上的数字，并用 `static_assert` 钉住；`ServiceAggregate` **只有四个值**，与服务自己算的
判定一一对应（客户端不许发明第五个）。

**为什么先做这个**：T3-01 要求 GUI 只链 `myrd_product_contracts` + `myrd_product_ipc` + 纯客户端
库，而服务的响应投影说的是 `product::RuntimeState` / `AggregateStatus` / `OperationState`
（management 的概念）。边界只能建立在一套两边都能说的词汇上。

**T3-01 剩余（下一步，机械执行）**：
1. 新建库 `myrd_product_client`（`src/client/response_view.{h,cpp}`）：把
   `src/management/service_response_wire.cpp` 的解码半边搬过去（约 230 行：`Reader`、
   `read_uuid`、`decode_profile`、`decode_operation`、`decode_error_field`、
   `decode_service_response`），产出 `ServiceDiskView` / `ServiceOperationView`；
2. management 新增 `project_view()` 的**穷尽 switch**（新增状态忘映射就编译失败），编码仍留在 management；
3. `myrd_cli` 改链 `myrd_product_client`（不再链 `myrd_management`）；
4. **负例测试**：配置期用 `get_target_property` 把 GUI 目标的 `LINK_LIBRARIES` 闭包写文件，
   ctest 断言不含 `myrd_management` / `myrd_persistence` / `myrd_storage` / `myrd_proxy` / `myrd_driver`；
5. T3-02 起才是 Win32 窗口/托盘/列表（`CreateWindowEx` + `NOTIFYICONDATA` + `ListView`），
   以及 T3-03 的进度一致性、T3-04 的三处确认流（force / accept_volatile_data_loss / confirmation_id）。

**T3 验收口径**：GUI 在客机上能列出盘/内存/generation；创建/启动/停止/checkpoint 可用且长操作
期间界面不假死；三处确认流分别出现；未提权时只读。
---

### 3.23 2026-10-27 01:40：T3-01…T3-04 完成，T3-05 待客机重跑

- **T3-01 完成**：客户端边界（`myrd_product_client` + 闭包检查，已证明能失败）。
- **T3-02 完成**：`myrd-gui.exe`（列表 + 托盘 + 3 秒刷新）；`aggregate` 是唯一判定（有测试）。
- **T3-03/T3-04 完成**：命令在**独立线程**发出（界面不假死）、进度跟操作状态并在终态停止、
  三处确认流（stop 仅在检查点未确认时、forget 需回显 uuid、权限拒绝给指令）。
- **T3-05 进行中**：`integration.m5.gui_workflow`（客机）已注册（159 例）。首次运行：服务装好并
  应答，但**建盘失败**（`the create ended failed`）——同一台客机跑了一整晚断电实验后的残留。
  **下一步：硬重启客机再跑一次**；若仍失败，先看 `disk list` 与 `C:\ProgramData\MyRamDisk\logs\service.log`。

**剩余**：T3-05 客机通过 → T4（安装/升级/卸载 + T4-04 release manifest 冻结）。
---

### 3.24 2026-10-27 01:50：T3 全部完成（GUI 五段）

**T3-01**：客户端边界 —— `myrd_product_client`（线格式 + 编解码）只链 `myrd_product_ipc` +
`myrd_product_contracts`；`myrd_cli` 与 `myrd-gui` 的**链接闭包**已写入构建目录并断言
（`tools/check_client_link_closure.cmake`，**configure 期**，因为 `cmake -P` 没有 target 会空检查报绿），
并**已证明能失败**（拿 `myrd_service` 当被试会拒绝）。

**T3-02**：`myrd-gui.exe`（列表 + 托盘 + 3 秒刷新）。**`aggregate` 是唯一判定**，有测试钉住
（runtime=running + aggregate=blocked 的盘必须显示 blocked）。内存列 resident/budget，未运行时
显示 "not running" 而不是 0。

**T3-03/T3-04**：命令在独立线程发出（界面不假死）、进度只跟操作状态并在终态停止并刷新；
三处确认流（stop 仅在检查点未确认时、forget 需回显 uuid、`authorization_denied` 给指令）。

**T3-05（客机，请求 `96bf7e5f…`，20 s）**：
```text
self-check: 1 disk(s)
  T:  running  Running  generation 2  2.8 MiB / 1024.0 MiB
tray: MyRamDisk: 1 disk
window-check: created 0x370088, found 0x370088
  process 3512 is alive after 6 s
PASS T3-05
```

**顺带抓到的真实缺陷**：T1-10 的默认脏数据阈值 64 MiB **大于 32 MiB 盘的容量**，校验拒绝，
`disk create` 以 `terminal failure without a worker-action error` 结束。修法：未显式指定时
**钳到请求容量**（阈值大于容量没有意义）。

**T4 计划（下一步从这里开始）**：
- **T4-01** 安装器：打包 / SCM 注册 / ProgramData ACL / `phdskmnt` **只检查不安装**；卸载必须安全下盘；
- **T4-02** Load Only 模式、导出单文件 VHDX（复用 compaction 的 fold writer）、易失盘模式、卷标/簇大小/卷序列号；
- **T4-03** 在线 compaction（第二套 durable phase + reconciliation + 自动触发；journal 增长有界可观测）；
- **T4-04** M5-E：干净 Debug/Release、M4 回归、冻结哈希、证据组校验工具（负例必须非零退出且**永不打印 PASS**）、release manifest。
---

### 3.25 2026-10-27 01:55：T4-01 完成（安装/卸载走通），T4 剩下三段

**客机读数**（请求 `48750655…`，37 s）：`INSTALLED` → 服务 RUNNING 且 `disk list` 应答 → 目录 ACL
符合 → 卸载把盘安全下盘、删服务、删文件、**保留** durable 记录 → `-PurgeData` 再清 →
`PASS T4-01`。

**profile store 的三条 ACL 规则（都是被拒后才学到的，写在这里省下次的时间）**：
1. 记录目录里**不许出现任何非管理 SID**（`Users: RX` 也会被拒：`A management ACL entry lacks
   required access`）⇒ profiles/operations **仅 SYSTEM + Administrators，FILE_ALL_ACCESS**；
2. DACL 必须是 **protected**（`A profile directory ACL is not protected`）⇒
   `icacls /inheritance:r` **不够**，要用 `SetAccessRuleProtection($true,$false)` + `Set-Acl`；
3. **数据根目录本身也被检查**（三个子目录都保护后仍报同一句）⇒ 根也要同样保护。
   唯一例外是 `logs`：它可以给 Users 只读（store 不检查它），因为"为什么失败"必须能被读到。

**手法**：installed 服务的 stderr 被 SCM 丢弃，但 **`myrd-service.exe --console` 从安装目录跑**
就能把 store 的原话打出来——三次定位全靠这一招。

**T4 剩余**：T4-02（Load Only / 单文件 VHDX 导出 / 易失盘模式 / 卷标·簇大小·卷序列号）→
T4-03（在线 compaction）→ T4-04（M5-E + release manifest 冻结）。
---

### 3.26 2026-10-27 02:00：T4-02 的施工图（已勘察，未开工）

T4-02 一句话：**Load Only 模式、单文件 VHDX 导出、易失盘模式、卷标/簇大小/卷序列号**。
勘察结果（`grep` 过一遍代码，不是凭印象）：**四件里没有一件是"加个开关"**——每件都是一条
CLI → control → wire → intent → profile → worker/卷 的完整纵切，而 profile 字段还有版本与校验成本。

**已有可复用的东西**：
- VHDX / fold 机制在 `src/persistence/compaction_coordinator.{h,cpp}`（`CompactionArtifactBuildRequest`
  已经在造"单文件产物"）与 `src/m3/m3_provider.cpp`（15 处 VHDX 相关）；
- 卷序列号已经采到：`m5_worker_runtime.cpp:258` 的 `evidence.volume_serial =
  identity.value().volume_serial_number`，并且已经在 worker 协议里传输
  （`m5_worker_protocol.cpp:344/655`）——**这条只差在 `disk show` 里显示出来**，是全组里最便宜的一件。

**按成本从低到高排的施工顺序**（建议照这个顺序，每步都能单独绿）：
1. **卷序列号可见**：`disk show` 增加一行（视图字段已有 `volume_serial`），改 CLI renderer + 一个模型用例；
2. **卷标 / 簇大小**：`LifecycleOperationIntent` 加两个字段（`std::wstring volume_label`、
   `std::uint32_t cluster_size_bytes`）→ `control::DiskCreateCommand` + `ipc_codec` →
   CLI `--volume-label` / `--cluster-size` → `profile::Profile` 追加两个字段（**追加到末尾并更新
   codec/validation/`kFields`**，注意 `profile_types.h` 的字段 tag 已经用到 26）→
   `m3_provider.cpp` 的卷格式化调用带上它们；
3. **易失盘模式**：`Profile` 加 `bool volatile_mode`；语义写清楚（**不做检查点、停止即弃**），
   在 `profile_validation` 里与 checkpoint 策略互斥校验，`disk create --volatile` 传下来；
4. **Load Only**：`M3Provider::start` 增加只读选项（store 以只读打开，执行器拒绝 checkpoint/
   compaction/带写停止），`disk start --load-only`；`disk list` 的 `activity` 要能表示这个状态；
5. **VHDX 导出**：`disk export --to <file.vhdx>`，复用 compaction 的 fold writer 产出折叠镜像，
   再套上 `m3_provider.cpp` 既有的 VHDX 头；导出必须**在只读快照上做**，不能影响正在运行的盘。

**验收口径**（每件都要客机读数）：卷标/簇大小在 `disk show` 与 `fsutil fsinfo` 里一致；易失盘
断电后**没有** checkpoint 可恢复（这是它的定义）；Load Only 成功的盘 `disk list` 显示只读且
worker 只为读启动；导出的 VHDX 能被 `Mount-DiskImage` 挂上且根目录可读。
### 3.27 2026-10-27 02:05：T4-02 第 1 步的实测勘察（比 3.26 的估计多一层）

想把**卷序列号**显示出来，实际要动三层（`grep` 确认）：

1. `WorkerRuntimeObservation`（`service_core.h`）**没有** `volume_serial` —— worker 协议里
   **有**（`m5_worker_protocol.cpp:344/655` 的 `status.bundle->volume_serial`，来自
   `m5_worker_runtime.cpp:258` 的 `evidence.volume_serial`），但**观测结构本身没带上来**；
   ⇒ 追加字段并在 `windows_worker_observation` 里从 status 填；
2. `product::ServiceDiskView`（`service_view.h`）没有该字段 ⇒ 追加，并让
   `src/client/response_view.cpp` 的 encode/decode 各写/读一个 tag；
3. `project_profile`（`management/service_response_wire.cpp`）从 `snapshot.runtime_observation` 填，
   CLI renderer 打出来，配一个模型用例 + 一个编解码往返断言。

**关于协议版本**：客户端与服务**始终一起发布**（设计如此，没有版本协商常量），所以追加字段
**不需要** minor 号——这一点写在这里，免得下一个人以为要先加版本号而把它拆成两次发布。

**这一步之后**才是卷标/簇大小（真正要动 profile schema），再之后是易失盘、Load Only、VHDX 导出。
### 3.28 2026-10-27 02:15：卷序列号那次尝试被回退，原因记在这里

**做了什么**：`WorkerRuntimeObservation` 加 `volume_serial`、`windows_worker_observation` 从
`status.bundle->volume_serial` 填、`ServiceProfileProjection` / `ServiceDiskView` 各加一个字段、
`encode_profile` / `decode_profile` 各写读一个 u64、`project_profile` 填、CLI 打 `serial=1A2B-3C4D`。

**结果**：编译通过，但 `m5.cli` 与 `m5.service_host` 的**编解码往返用例**失败
（`decoded.ok()` 为假）。两端读写顺序我核对过是一致的（has_mount, resident, budget, **serial**,
mount_length），所以不是顺序问题；**需要先拿到 decode 的具体错误文本**（下一步：给
`decode_service_response` 的错误加一行 stderr，或用一个小 driver 直接调它）。

**处理**：`git checkout -- src/` **全部回退**，重建后三个套件恢复绿。理由：在拿不到 decode 错误
的情况下继续改，只会把"不知道哪里错"变成"不知道哪里错且树是红的"。

**下一步（照做即可）**：
1. 用一个小 driver 编码一个含 `volume_serial` 的响应再解码，把 `decode` 的 `error().operation` 与
   `message` 打印出来——一次就能定位；
2. 拿到原因后再重做这次改动（字段与顺序可以照 3.27 的清单，已经验证过编译）；
3. 该改动完成后 T4-02 的第 1 步就结束，再进卷标/簇大小。
### 3.29 2026-10-27 02:25：卷序列号第二次失败——**不是陈旧对象，是布局真的不对称**

这次用 `--clean-first` 全量重建（排除"增量构建留下旧编码器"的假设），失败**照旧**：
`decode failed: A profile projection is truncated`（`m5.cli` 与 `m5.service_host` 的往返用例）。

**已核对**：`encode_profile` 写 `6×u8, u64 generation, uuid, u8 has_mount, u64 resident,
u64 budget, u64 serial, u16 mount_length, text`；`decode_profile` 读完全相同的顺序。
两边逐字对称，却仍在"固定区"那组读取处判定截断 ⇒ 问题**不在我改的那 8 个字节**，而在
**响应层切给 profile 的 payload 长度**（`put_field` / 字段切片）或 `Reader` 的边界语义。

**下一步只有一条路（10 行代码，一次定位）**：在 `decode_service_response` 里**临时打印每个字段的
tag 与 length**，以及 `decode_profile` 收到 payload 的**首 8 字节十六进制**——立刻就能看出
`profiles` 字段的声明长度是否比实际写入少 8（或 `Reader::u64` 是否在末尾拒绝读）。

**已经回退两次了**，不要再盲改：这条支线在拿到字节级证据前不值得第三次尝试。
**替代路线**：把卷序列号放到**响应级字段**（像 `mount_target` 那样），完全避开 profile 的固定布局——
`disk show` 只需要一块盘，够用；`disk list` 的每行序列号可以后面再补。
### 3.30 2026-10-27 02:35：字节级读数到手——**编码端少了那 8 字节**

探针（临时打印，已随回退移除）在客机无关的本机测试里给出两个数：

- `probe: decoded profile bytes=51`、`probe: decode payload=51` —— profile 的 payload 声明长度是 **51**；
- `probe: encoded profile bytes=12` —— 从 `budget` 之前量到结尾，只有 **12** 字节
  （= budget 8 + mount_length 2 + mount 2）。

51 恰好等于 **不含 serial** 的布局：`6 + 8 + uuid(2+16) + 1 + 8 + 8 + 2 = 51`。
12 也恰好是 **不含 serial** 的尾部：`8 + 2 + 2`。**含 serial 时应是 59 与 20。**

⇒ **解码端在读 serial（所以判截断），编码端没写**。原因：那两次改动里，编码端的
`put_u64(out, profile.volume_serial);` 与解码端的 `reader.u64(volume_serial)` **不是同一次编辑
加进去的**，而我在中途回退过一次，于是出现"半套改动"——这正是两次失败的真正原因，不是布局。

**重做时的正确做法（一次编辑里同时加三处，然后用那两个数验收）**：
1. `encode_profile`：在 `put_u64(out, profile.budget_bytes);` 之后加
   `put_u64(out, profile.volume_serial);`（期望尾部 12 → **20**）；
2. `decode_profile`：在 `budget_bytes` 之后声明并读 `volume_serial`（期望 payload 51 → **59**）；
3. `ServiceDiskView`/`ServiceProfileProjection` 各加字段、`project_profile` 填、CLI 打 `serial=`。
### 3.31 2026-10-27 02:45：卷序列号这条线**通了**（本机绿），客机读数欠一次干净机

- `a0bd1e9`：序列号穿过观测/两个投影/线格式到 renderer，`disk list` 与 GUI 行都显示
  `serial=A1B2-C3D4`（没有卷时**不显示**，GUI 自检显示 `serial=-`）；新增模型用例钉住往返与渲染；
  `--clean-first` + 八套件全绿。
- **元凶**：`kProfileFixedBytes` 这个**最小 payload 长度**守卫数的是旧布局（`6+8+16+1+8+8+2`），
  常量注释里已写明它值两次失败。
- **客机读数没拿到**：`integration.m5.gui_workflow` 3.5 秒失败——客机上还留着 T4-01 装的
  `MyRamDiskService`，两个服务不能同时拥有公开管道。**先卸载/重启再跑**。
- **下一步**：干净客机跑一次 `gui_workflow`（顺带就是序列号的客机读数），然后进 T4-02 第 2 步
  （卷标 / 簇大小：intent → command → codec → CLI → profile 追加字段 → 卷格式化）。
### 3.32 2026-10-27 02:55：GUI 用例失败的真因找到（缺数据目录），改造半途回退

**真因**（客机 `--console` 探针一次拿到）：
```text
myrd-service: build_state failed: op=profile_store.open_directory
              msg=Failed to open a required profile directory native=2
```
`native=2` = 文件不存在。T4-01 的用例用 `-PurgeData` 把 `C:\ProgramData\MyRamDisk` **整个删掉**了，
而 `gui_workflow` 用的是**裸 `sc create`**，从不创建那些目录 ⇒ 服务启动即失败（exit 0、无日志、
`sc start` 还可能回 1053）。**不是产品缺陷**：store 的 fail-closed 行为是正确的，缺的是用例的前置条件。

**正确改法（已试，但本轮没收尾，已回退）**：让 `gui_workflow` **走发布的安装器**
（`tools/install/Install-MyRamDisk.ps1`，它建目录并给 protected DACL），二进制改从
`C:\Program Files\MyRamDisk` 取；清理也改走卸载器。**回退原因**：那串替换把一个 `}` 弄没了，
脚本语法错误——这类多块替换必须一次只改一处并用 `Parser::ParseFile` 逐步验证，
不要在一次命令里叠三处。

**下一步**（按顺序，每步都跑 Parser 验证）：
1. 在用例里加 `$installDir` 并让三个二进制路径指向它（改一处，验证）；
2. 用安装器替换"裸 sc create + 启动"整块（改一处，验证）；
3. 清理改走卸载器（改一处，验证）；
4. 客机跑一次——这一次同时给出**卷序列号的客机读数**（自检现在会打 `serial=`）。
### 3.33 2026-10-27 03:05：GUI 用例恢复绿，**卷序列号拿到客机读数**

```text
install through the shipped installer
self-check: 1 disk(s)
  T:  running  Running  generation 2  2.8 MiB / 1024.0 MiB  serial=5A42-9D28
UNINSTALLED
PASS T3-05
```
（请求 `1e26484b…`，22 s）

- 之前的失败真因已确认：用例用裸 `sc create`，**数据目录不存在** ⇒ 服务 fail-closed
  （`profile_store.open_directory`, native 2；exit 0 且无日志，所以像崩溃）。改走**发布安装器**后一次通过。
- **卷序列号 = T4-02 第 1 步完成**（客机读数 `serial=5A42-9D28`）。它来自 worker 自己的 bundle 序列号，
  经观测/两个投影/线格式到渲染，按 `vol` 的样式显示；`kProfileFixedBytes` 守卫已随之 +8（注释里写明它值两次失败）。

**下一步**：T4-02 第 2 步——**卷标 / 簇大小**（intent → command → ipc_codec → CLI
`--volume-label` / `--cluster-size` → profile 追加两字段 → `m3_provider` 卷格式化带上它们）。
### 3.34 2026-10-27 03:15：下一步的最小可执行清单（T4-02 第 2 步：卷标 / 簇大小）

已经勘察过，这里只列**按顺序要动的点**，每步都能单独编译通过：

1. `src/control/command.h`：`DiskCreateCommand` 加 `std::wstring volume_label;` 与
   `std::uint32_t cluster_size_bytes{};`（末尾追加）；
2. `src/control/ipc_codec.cpp`：选两个未用的 tag 写/读这两个字段（该文件里 create 命令的
   tag 表就在附近，按现有风格追加，严格解码：未知 tag 报错）；
3. `src/cli/cli_parser.cpp`：`--volume-label <名字>`、`--cluster-size <N|NK|NM>`（复用
   `product::quantity_parser` 解析字节数），写进 `DiskCreateCommand`；
4. `src/management/service_core.h` 的 `LifecycleOperationIntent` 加同名字段，
   `service_core.cpp` 的 `register_lifecycle` 路径把它们带进 profile；
5. `src/profile/profile_types.h` 的 `Profile` 追加两个字段（`ProfileFieldTag` 已用到 26，
   继续 27/28），同步 `profile_codec.cpp` 的写/读/字段计数与 `profile_validation.cpp` 的范围校验
   （簇大小只接受 4096/8192/…/65536；卷标 ≤ 32 字符且非空）；
6. `src/m3/m3_provider.cpp` 的卷格式化调用带上这两个值（AIM 的 format 请求里有卷标与簇大小）；
7. 测试：`m5.cli`（解析与渲染）、`m4.checkpoint_operation`（profile 往返）、
   以及一个**客机**用例断言 `fsutil fsinfo volumeinfo <盘符>` 的卷标与簇大小与配置一致。

**纪律提醒（这轮踩过的两个坑，别再踩）**：
- 改 `profile_types.h`（头文件）后**必须 `--clean-first`**，否则会出现"半套改动"的假失败；
- **一次命令只做一处块替换**，每步都跑 `Parser::ParseFile` / 编译验证——上一轮三处叠加丢了一个 `}`。
### 3.35 2026-10-27 03:25：3.34 清单的两处修正（勘察结果，别再走弯路）

1. **字段 tag 号已经有了确定值**：`ProfileFieldTag` 目前用到 35（compaction 那组），所以卷标与簇大小
   用 **36 / 37**，追加在 `automatic_compaction` 之后；`profile_codec` 的写/读与字段计数按现有风格同步。
2. **卷格式化不在 `m3_provider.cpp`**：该文件里只有 `journal_format.h` 与错误码，**没有** volume create /
   label / cluster 的调用点。卷的创建走 **AIM 代理层**（`src/proxy/`）与 worker 侧，
   所以 3.34 第 6 步应改成：**在 worker 的卷创建路径（AIM format 请求）里带上卷标与簇大小**，
   对应 `src/proxy/*` + `src/host/m5_worker_runtime.cpp` 的 create-device 动作。
   ⇒ 这一步比 3.34 估计的更靠下，**需要先确认 AIM 的 format 请求结构里是否有这两个字段**（如果没有，
   卷标只能在格式化后用 `SetVolumeLabel` 设置，簇大小则必须先确认再格式化——这决定了 CLI 选项是
   "创建时"还是"创建前"语义）。

**结论**：第 2 步真正的第一件事是**读 AIM 的 format 请求结构**，不是先改 profile——
否则会做出一个存得下、却改不了卷的字段。
### 3.36 2026-10-27 03:35：**T4-02 的"卷标 / 簇大小"需要先做一个范围裁定**

全树搜索的结果（不是猜测）：

```
SetVolumeLabel / volume_label / cluster_size / FMIFS / FormatEx / IOCTL_DISK_FORMAT
  → 0 处命中
```

**本产品没有任何格式化卷的代码路径**。而 M5 的盘确实有文件系统（e2e 能写标记文件、能挂盘符），
说明文件系统是**别处带来的**（AIM 驱动呈现的镜像本身带格式，或 M3 阶段生成的镜像带格式），
不是我们在运行时格式化的。

**由此产生一个真实的取舍**，T4-02 的这两个子项因此不是"加两个字段"：

- **卷标**：若卷已格式化，可在挂上之后用 `SetVolumeLabel` 设置——**不需要格式化**，成本小；
- **簇大小**：**只有格式化时才能定**。要在运行时支持它，必须新增一条**镜像格式化**能力
  （生成带指定簇大小的新镜像，或在 worker 侧调用 FormatEx）——这是一个**新功能**，量级远超
  一个 CLI 选项，而且会改动"创建一块盘"的语义（谁负责格式化、失败如何回滚）。

**建议的裁定（需要拍板，我按此执行）**：
1. **卷标**：按"创建参数 + 挂载后 `SetVolumeLabel`"实现，工作正常即有客机读数（`fsutil fsinfo volumeinfo`
   能看到），**这一项保留在 T4-02**；
2. **簇大小**：从 T4-02 移出，作为**独立增记为遗留项**（需要"镜像格式化"这一新能力）。
   理由：它不是选项，是能力；把它塞进 T4-02 会让"产品化"这一段变成一次未评估的架构改动。

在这条裁定之前，**不动 profile 字段**——先存字段再发现改不了卷，比不存更糟。
### 3.37 2026-10-27 02:55：整套客机普查**已发起，结果待收**

- 请求 **`29ada3b770e943a482878ba3ac64293d`**（`-Run` 无 filter，160 个用例），02:29 发起。
  主机侧等待 1800 s 超时**不会中断客机运行**，按提示用下面这条收：
  ```powershell
  .\tools\m5vm\Invoke-M5Vm.ps1 -Run -RequestId 29ada3b770e943a482878ba3ac64293d -MinTimeoutSeconds 600
  ```
- 同时一条普通 ssh 探测（读 `C:\m5\control` 与进程）也**挂住了**，符合本会话早前记录的
  "客机在长时间设备类运行中会变慢/挂住"的现象；**收结果前先确认客机能应答**，
  必要时硬重启（会话里多次用过 `vmrun stop hard` + `start nogui`，15 s 内可 SSH）。
- **这条普查是 T4-04 "M4 回归 + 干净构建" 的客机侧基线**：期望 158/160
  （`structural.*` 两例在客机上因 `vswhere.exe not found` 失败，属已知）。
### 3.38 2026-10-27 03:15：整套客机普查**跑不完**（42 分钟未结束）——记为待查项

- 请求 `29ada3b7…`（160 例，02:29 发起）到 03:11 **仍未结束**；T1-05 时代的整套读数约 10 分钟。
- 同时：**普通 ssh 探测在 120 s 内没返回**（`Get-Date` 这种最轻的命令也挂），而同一分钟一次
  `echo up` 曾成功——**客机在整套设备类运行期间会不可预测地变慢**，与本会话早前记录一致。
- **处置**：停止等待（不等于中断客机运行），把这条记为 **T4-04 的前置问题**：
  整套普查必须先能稳定跑完，否则"干净构建 + M4 回归"没有基线。
- **下一步（按成本）**：
  1. 硬重启客机，确认 `echo up` + `Get-Date` 都能秒回；
  2. **分组跑**而不是整套跑：`-Filter '^unit\.'`、`'^model\.'`/`'^m5\.'`、`'^integration\.'` 三组分别计时，
     找出是哪一组把时间拖到 40 分钟（e2e 三例本身就是分钟级，但不应是 40 分钟）；
  3. 若某条 e2e 卡住，用它的 `run-<id>/*.stdout.txt` 与 `C:\m5\runs` 下的日志定位。
- **注意**：`-Run -RequestId` 收结果这条路径本身是好的（本次它正确识别"已提交，不重复触发"），
  问题在客机。
### 3.39 2026-10-27 03:20：staging 现在**跳过载荷拷贝**（不是失败），已加一道诚实的守卫

- 客机硬重启后 `echo up` 15 s、`Get-Date` 1.3 s——**客机本身是好的**，之前的"变慢"是整套 160 例的
  长运行期间出现的，分组跑之前不再整跑。
- 但 `-Stage` 现在**不拷贝载荷**：本地打印 `staged 160 tests` 之后，**没有**出现以往那句
  `staged to usr@myrd-vm:…`，客机侧 `out\build\msvc\tests\Release` 里 **0 个 exe**。
  ⇒ 不是拷贝失败，是**拷贝那一步被跳过**了。
- 我已加一道守卫（`tools/m5vm/Invoke-M5Vm.ps1`）：**拷贝前先在客机建好 tests 目录**，
  这样"计数"这一步不会再抛 `DirectoryNotFound`（那会把问题伪装成编译失败），而是如实报
  "0 个落地"。本次它如实报了，这正是我们要的行为。
- **下一步**：读 `Invoke-M5Vm.ps1` 里"本地 staging → git metadata 拷贝"之间的那段，
  找出本该执行载荷拷贝的分支为什么没走（可能是某个条件/开关在上次改动后不再成立）。
  这条不修，任何客机测量都跑不起来。
### 3.40 2026-10-27 03:35：staging 仍未落地，且**客机上查树也没回话**

- 守卫生效后读数一致：本地 `staged 122 test binaries`、客机 `0`，报错原文就是
  `staging did not land: the guest has 0 test executables ... but 122 were staged`。
- 随后那条"Stage 后立刻查客机目录树"的 ssh 探测**没有任何输出**（不是报错，是空）——
  说明要么该会话没执行，要么客机又处在本会话记录过的"能 echo、不能跑多行脚本"状态。
- **下一步（按顺序，别跳）**：
  1. 用**最短**的编码命令确认客机可写：`New-Item -Force C:\m5\probe.txt` 再 `Test-Path`；
  2. 用**单行** `cmd /c dir /b "C:\...\m5-lifecycle"` 看客机 worktree 到底有什么（cmd 比多行 PowerShell 稳）；
  3. 若 `out` 根本不存在，先手工 `mkdir` 出 `out\build\msvc\tests\Release`，再跑 `-Stage`，
     看 scp 是否报错（目前 `Copy-ToRemote` 的 scp 退出码 0 但我们没有逐项打印，可疑）；
  4. 若仍 0，把 `Copy-ToRemote` 临时改成**每项打印目标路径与结果**，一次就能看出哪一项没到。
- **这条是当前唯一阻塞**：修好之前，任何客机测量（包括 T4-04 的 M4 回归）都无法进行。
### 3.41 2026-10-27 03:50：staging 的嵌套是 scp 语义问题，**建议改用 tar 直传**

三次实测（都在客机上用 `cmd /c dir` 验证过）：

1. `scp -r local\out remote\out` 在 `remote\out` **已存在**时会创建 `remote\out\out` ⇒ 载荷全落在
   `out\out\build\...`，而 runner 只看 `out\build\...`，于是"客机有 0 个"；
2. **`\.` 内容拷贝技巧在 Windows OpenSSH 的 scp 上不成立**：它对嵌套文件报
   `dest open ... Failure`（看起来像权限问题，其实不是）；
3. "先删目标再拷"也**没解决**（本次客机仍是 `out\out` + `out\build` 两棵树并存）。

**推荐做法（不要再用 scp 拷目录）**：
```powershell
# 本地
tar.exe -czf $payloadTar -C $stageDir .
# 传输一个文件
Copy-ToRemote -Local $payloadTar -Remote "$guestRepoScp/.m5-payload.tgz"
# 客机解包（tar 已在该机可用，git metadata  Extract 就是这么做的）
Invoke-Remote "& 'C:\Windows\system32\tar.exe' -xzf '$guestRepoScp/.m5-payload.tgz' -C '$guestRepoScp'; Remove-Item '$guestRepoScp/.m5-payload.tgz' -Force"
```
这与"git metadata 用 tar 传"的成功做法一致，且**把 scp 的目录语义从问题里彻底移除**。

**在当前状态**：staging 会被计数守卫拒绝（这是好事，不会让客机跑出误导性失败）；
`out\out` 残留需在客机上 `rmdir /s /q ...\out` 清一次。
### 3.42 2026-10-27 03:45：staging 修好了，但**agent 不接请求**

- `eb04118`：载荷改为**单个 tar**（本地打包 → 传一个文件 → 客机 `tar -xzf`），staging 立刻恢复：
  `staged to usr@myrd-vm:…` 这行只在客机计数与本地一致后才打印。三条 scp 语义坑的经验已写进代码注释。
- 但用 `-Run -Filter '^unit\.'`（请求 `b8c62798…`）后 **10 分钟没有结果**。客机侧用 cmd 查到：
  - `C:\m5\control` 里**有** `b8c62798….request.ps1`（请求已提交）；
  - `cmd /c echo probe-ok` **秒回**（客机、ssh、cmd 都正常）；
  ⇒ **计划任务里的 agent 没去取这个请求**。最可能是之前那次 42 分钟的整套运行留下的 agent 实例
    还占着（它的锁/互斥），或者任务的触发器坏了。
- **下一步**：
  1. 用**单行、不含空格参数**的 cmd 查任务：`schtasks /query /fo csv` 再本地过滤（不要在同一条 ssh 里塞
     `findstr "a b"`，Windows 会把引号吃掉）；
  2. 看 `C:\m5\agent` 下 agent 自己的日志（它在里面写执行记录）；
  3. 若确认卡住：`schtasks /end /tn <任务名>` 再 `/run`，或直接重启客机（本会话多次证明 15 s 可恢复）。
- **纪律**：Windows 的 ssh 命令里**引号与空格会被吞**，多参数一律改用 `-EncodedCommand`；
  这次连 `tasklist /fi "..."` 都被拆坏，浪费了两轮。
### 3.43 2026-10-27 04:15：**VM harness 的 agent 停摆**（当前唯一阻塞）

证据（都在客机上取到，客机本身健康：硬重启后 `echo up` 15 s、`Get-Date` 1.3 s）：

- `C:\m5\control\m5-agent.log` 尾部：
  ```text
  2026-10-26T18:17:05 request 1e26484b… finished exit=0     ← 最后一次成功
  2026-10-26T18:19:35 agent start elevated=True drain=True
  2026-10-26T19:19:36 agent start elevated=True drain=True
  2026-10-26T19:53:51 agent start elevated=True drain=True
  ```
  ⇒ 18:19 之后**每次 agent 都只是"start"，再没有任何 "finished"**；中间那次整套 160 例的运行
  （`run-29ada3b7…`）到 04:03 还在写目录。
- 我杀掉了那个卡住的 runner（`pid=4712`，`Invoke-M5Tests`），**请求仍然不被处理**：
  `67f5bf1c…`（单个 `m5.gui` 用例）10 分钟无响应。
- ⇒ 需要**重建计划任务/agent 状态**，不是等一等就好。

**下一步（重建，按顺序）**：
1. `schtasks /query /fo csv` 找到 agent 任务名（本轮那条命令被 ssh 引号吃掉，**必须用 `-EncodedCommand`**）；
2. `schtasks /end /tn <名>` → `schtasks /delete /tn <名> /f` → 用 `tools/m5vm/Invoke-M5Vm.ps1 -Probe`
   里记录的注册命令**重新注册**（该脚本 `-Probe` 会打印三条通道与任务名）；
3. 清 `C:\m5\control\*.request.ps1` 残留，再跑一个单例（`^m5\.gui$`）确认 <60 s 返回；
4. 确认后再分组跑套件，恢复"每项以客机实测为准"。

**注意**：整套 160 例在本机跑不完（42 分钟未完）+ agent 单实例 ⇒ **以后一律分组跑**，
并在跑之前清残留请求。
### 3.44 2026-10-27 04:20：agent 恢复，**分组基线到手**（42 分钟那次是 agent 卡住，不是套件慢）

**恢复手法（有效，记住）**：
```powershell
# 客机上，EncodedCommand（ssh 会吞引号）
schtasks /end /tn 'MyRamDisk-M5Agent'        # 任务名见 -Probe 输出，本机为 MyRamDisk-M5-Agent
Get-ChildItem C:\m5\control -Filter '*.request.ps1' | Remove-Item -Force
```
之后一个单例 `^m5\.gui$` **5 s** 返回 1/1。

**分组读数（客机，Release）**：

| 组 | 用例数 | 用时 | 结果 |
|---|---|---|---|
| `^unit\.` | 96 | **210 s** | **95/96**，失败：`unit.m4.profile_codec` |
| `^model\.|^m5\.` | 13 | **128 s** | **12/13**，失败：`m5.production_execution_backend`（**已知在批内间发**，单独跑通过） |
| `^integration\.` | 51 | 未跑 | 待跑 |

⇒ **整套 42 分钟"跑不完"的真相是 agent 单实例被上一次运行卡住**，不是套件本身慢。
以后**分组跑**（unit 3.5 分钟 + model/m5 2 分钟 + integration 另算）。

**两个待查**：
1. `unit.m4.profile_codec`（本轮新出现，unit 组里唯一红）——很可能与 T1-10 的默认值或我今天的
   profile 相关改动有关，**下一步先看它**（它的失败信息会指出哪个字段/字节）；
2. `m5.production_execution_backend`——已知间发项，单独复跑确认。
### 3.45 2026-10-27 04:35：`unit.m4.profile_codec` 的红是**默认值改动的正确后果**

- 本机复现：`encoded.value() == minimal_golden()` 失败（不是客机问题）。
- 原因：T1-10 改了 `CheckpointPolicy` 默认值（interval 300 → **60**、dirty 阈值关 → **开 64 MiB**），
  而 `minimal_golden()` 是**字节级黄金样本**——默认值一变，编码字节当然变。**测试是对的，样本要更新。**
- **新的黄金字节**（从失败运行里打印出来的，可直接粘贴）在下一行；替换
  `tests/unit/m4_profile_codec_tests.cpp` 里 `minimal_golden()` 的 `from_hex(...)` 参数即可：

```text
4d595244505246000100000048000000fa010000000000000700000000000000b20100002300000097587e8adf5343a0f8c597c13bc0faa38748e8b56c68c931834bccca8fe77c42010001011000000000112233445566778899aabbccddeeff0200020108000000532d312d352d313803000201010000004104000501080000000068e5cf8b0100000500030101000000000600020104000000443a5c4207000201080000006469736b2e72646d0800010110000000102132435465768798a9bacbdcedfe0f090005010800000000000000020000000a00040104000000000200000b00040104000000001000000c000701000000000d000701000000000e000701000000000f00030101000000001000020102000000523a1100060101000000011200030101000000001300050108000000000000400000000014000501080000000000003800000000150005010800000000000030000000001600050108000000000000080000000017000601010000000118000501080000003c000000000000001900060101000000011a0005010800000000000004000000001b00060101000000011c00030101000000011d00030101000000011e00030101000000001f000601010000000120000401040000004600000021000401040000003200000022000401040000004000000023000401040000000a000000
```

- **这一项对 T4-04 有直接影响**：T4-04 要求"干净 Debug/Release"，而当前 unit 组有 1 红；
  更新黄金样本后 unit 应为 **96/96**，那是 T4-04 的基线。
- 备选做法（若不想硬编码）：把 `minimal_golden()` 改成"首次运行生成并比对 sha256"，但那会失去
  "字节级契约"的意义——**建议仍然更新明文黄金样本**，并在提交信息里写明"因 T1-10 默认值变更"。
### 3.46 2026-10-27 05:10：客机基线两段到手，integration 组仍在跑

- **`^unit\.`：96/96 通过（227 s，请求 `b37eed00…`）** —— 这是 T4-04 干净构建的第一段基线。
  之前那次 `0/96` 是**假失败**：上一次 `-Stage` 其实失败（被 `Select-Object -Last 1` 吞掉），
  `-Run` 跑在无二进制的树上，全报 `MISSING`。**纪律：`-Stage` 输出必须看到 `staged to usr@` 才算成。**
- **`^model\.|^m5\.`：12/13（128 s）**，唯一红 `m5.production_execution_backend`（已知批内间发）。
- **`^integration\.`**：请求 **`6f8fe4298c9149aab96fef4998e763c4`**，04:52 发起，25 分钟仍未回。
  按之前的经验，很可能是某条 e2e 卡住（`integration.m5.powerloss_verify` 在没有断电前置时会等 180 s，
  但 25 分钟意味着别的）。收集方式同前：
  ```powershell
  .\tools\m5vm\Invoke-M5Vm.ps1 -Run -RequestId 6f8fe4298c9149aab96fef4998e763c4 -MinTimeoutSeconds 600
  ```
- **下一步**：收这条结果；若卡住，把 integration 组再细分
  （`^integration\.m5\.`、`^integration\.m3\.`、`^integration\.m4\.`、其余），逐组计时——
  这正是 T4-04 "M4 回归"要用的分组口径。
### 3.47 2026-10-27 05:20：integration 组**卡住**（30+ 分钟），按计划细分

- 请求 `6f8fe429…`（`^integration\.`，51 例）自 04:52 起 **30 分钟未完成**，主机侧两次收集都超时。
- 期望时间约 12 分钟（`cli_e2e` ≈6、`powerloss_prepare` ≈1、`powerloss_verify` ≤3、`install_uninstall` ≈1、
  `gui_workflow` ≈0.5），所以这是**卡住**而不是慢。
- **下一步（照 3.46 的计划细分，逐组计时）**：
  ```powershell
  -Filter '^integration\.m5\.'      # 5 例，含 cli_e2e / powerloss / install / gui
  -Filter '^integration\.m3\.'
  -Filter '^integration\.m4\.'
  -Filter '^integration\.m1\.|^integration\.m2\.'
  ```
  卡在哪一组就单独跑那一条，并用它的 `C:\m5\control\run-<id>\*.stdout.txt` 与 `C:\m5\runs` 的日志定位。
- **注意**：`integration.m5.powerloss_verify` **单独跑必然失败**（它需要主机先断电），
  所以 m5 组的"预期"是 4 过 1 失败，不是全绿；这一点要写进 T4-04 的基线口径，否则会被误判为红。
### 3.48 2026-10-27 05:50：integration 组卡住的真因是**客机残留**，不是产品

`cli_e2e` 本次只跑了 3 秒，它的 stdout 说明了一切：

```text
service pid 3584 is answering: disk.list: completed
b4e5bd61-d856-4e67-85bc-50f7fd8380fa  T:  Ready  stopped/configured
        runtime=Stopped  activity=Idle  persistence=Dirty  health=Healthy
        generation=31  mem=0/0
1 disk(s)
```

客机上**留着一块之前运行没清掉的盘**（`T:`、generation 31、`persistence=Dirty`），
并且还有一个 **03:54 启动的 `myrd-m5-worker` 孤儿进程**仍在。于是 e2e 在"已经有盘"的机器上建盘、
做预检、等待终态，行为和干净机上完全不同——`powerloss_prepare` 迟了 30 分钟才开始，也是同一台机器
被这些残留拖住的表现。

**⇒ integration 组（以及任何 e2e）的前置条件必须写成脚本，不能靠"上次跑完应该清了"**：
```powershell
# 客机上，EncodedCommand
Get-Process myrd* -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($d in 'C:\ProgramData\MyRamDisk\profiles','C:\ProgramData\MyRamDisk\operations') {
  if (Test-Path $d) { Get-ChildItem -Force $d | Remove-Item -Recurse -Force }
}
sc.exe query MyRamDiskService > $null 2>&1; if ($LASTEXITCODE -eq 0) { sc.exe stop MyRamDiskService; sc.exe delete MyRamDiskService }
```
**下一步**：把这段做成 `tools/m5vm/Reset-M5Guest.ps1`（或 `Invoke-M5Vm.ps1 -Reset`），
**每次跑 e2e 组之前自动执行**，然后再跑 `^integration\.m5\.`。这样 integration 组的读数才有意义。
### 3.49 2026-10-27 06:00：`Reset-M5Guest.ps1` 落地，客机已干净

- `tools/m5vm/Reset-M5Guest.ps1`：停已知服务 → 杀 worker/service 进程 → **若还有 worker 存活或记录目录非空就
  `exit 1` 拒绝**（"拒绝"比"继续测"重要：脏机上的读数测的是机器）→ 清 `profiles`/`operations` →
  报告挂载卷（保留 `logs`，因为下一次失败的诊断要对着它读）。
- **实测**：重启前它拒绝了（`1 worker(s) survived`——那个 03:54 的 worker 卡在驱动调用里，
  只能重启才能消失）；重启后一次通过：`profiles 0`、`operations 0`、`mounted volumes: C:`、`RESET`。
- **用法**：客机上
  `powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1`
  （脚本已 scp 到 `C:\m5\`；仓库里是 `tools/m5vm/Reset-M5Guest.ps1`）。
- **下一步**：干净机上跑 `^integration\.m5\.`（5 例，预期 4 过 + `powerloss_verify` 单独必失），
  拿到第一个可信的 integration 读数；然后依次 `^integration\.m3\.`、`^integration\.m4\.`、其余。
### 3.50 2026-10-27 06:25：**干净客机上 `^integration\.m5\.` 也卡住**（30 分钟）

- 客机：硬重启 + `Reset-M5Guest.ps1` → `profiles 0 / operations 0 / 只有 C:`，staging 也确认成功
  （`staged to usr@`），然后请求 `2e2450f5…`（5 例）**30 分钟未完成**。
- 期间一条普通的 EncodedCommand 探测（列 `run-2e2450f5…` 目录 + agent 日志 + 进程）**也挂住**
  ⇒ 客机在跑这条 e2e 时又变得不可交互（本会话记录过的现象）。
- **结论**：这不是脏机（已排除），是**某一例 e2e 在干净机上卡死**。必须逐例隔离：
  ```powershell
  -Filter '^integration\.m5\.cli_e2e$'        # 单独跑，它在 05:17 那次只要 3 s 就失败过
  -Filter '^integration\.m5\.install_uninstall$'
  -Filter '^integration\.m5\.gui_workflow$'
  -Filter '^integration\.m5\.powerloss_prepare$'
  ```
  每例跑完先 `Reset-M5Guest`。**`powerloss_verify` 永远单独看**（需要主机断电）。
- **注意**：`cli_e2e` 在 05:17 那次 3 秒失败时，客机上有残留盘（已解释）；现在干净机上它是"卡住"还是
  "失败"未知——**先单独跑它**，并用 `C:\m5\runs\<work-root>.*.log` 与服务日志判断卡在哪一步。
### 3.51 2026-10-27 06:45：**`cli_e2e` 单独跑也卡住 —— 这是回归，不是客机问题**

- 客机：硬重启 → `Reset-M5Guest` → `RESET`（`mounted volumes: C:`），staging 成功，然后
  请求 `b4c523cc…`（**只跑 `cli_e2e` 一例**）**15 分钟未完成**；它的基线是 **6 分钟**，
  最近一次通过是 04:20 前后的 `gui_workflow`/其它 e2e。
- **时间线**（今天最后几小时的改动，全部只有 unit/model 覆盖，没有 e2e 覆盖）：
  1. 卷序列号纵切（`WorkerRuntimeObservation` / 两个投影 / 线格式 / renderer）；
  2. `Uuid()` **默认构造改为 public**（为了让线格式结构可逐字段构造）；
  3. `kProfileFixedBytes` +8；
  4. 两个测试文件的修正（黄金样本、校验前提）——纯测试。
- **最可疑的是第 2 条**：把一个"构造必须有身份"的类型放宽成可默认构造，**nil uuid 现在可以流进
  任何逐字段构造的结构**（例如请求 id、profile id）。若某处现在发的是 nil，服务的等待/寻址行为会
  与之前不同——这与"卡住而不是报错"的表现相符。
- **下一步（按成本）**：
  1. 在 `cli_e2e` 的失败点看服务日志：`C:\m5\runs\<work-root>.service.log`（每次迭代都会归档）；
     若最后一行停在某个 `action-start` 或 `checkpoint` 前，就能直接定位；
  2. 若确认与 nil uuid 有关：给 `Uuid` 加一个 `bool is_nil()` 并在**发出请求前**断言非 nil
     （或在 `build_command_request` 里拒绝 nil request id），把"可默认构造"的放宽重新收窄；
  3. 若与序列号字段有关：把该字段临时改回不发送，重跑 `cli_e2e` 二分。
- **纪律重复一次**：**unit/model 全绿不能替代一次 e2e**——这次改动都有单元覆盖，但 e2e 卡住了。
### 3.52 2026-10-27 06:50：**卡住的不是 e2e，是 agent 在"排空请求"里挂住**

客机 `m5-agent.log` 尾部（本地时间 05:17 / 05:49 / 06:25 三次）：

```text
2026-10-26T21:17:48 agent start elevated=True drain=True once=False request=
2026-10-26T21:49:55 agent start elevated=True drain=True once=False request=
2026-10-26T22:25:00 agent start elevated=True drain=True once=False request=
```
**只有 "agent start"，没有任何 "request … finished" / "drain complete"**；而 `C:\m5\control` 里
**三个请求文件都还在**（05:17、05:49、06:24）。跑目录 `run-b4c523cc…` **根本没被创建**。

⇒ 这一小时所有"卡住"（含 `cli_e2e` 15 分钟）**不是 e2e 卡、也不是产品卡**，而是
**agent 每次启动后在排空请求的过程中挂住**，请求因此永远不被执行。之前那些"integration 卡 30 分钟"
很可能也是同一个原因（而不是我 3.48/3.51 里写的产品回归——**那两条结论要收回**）。

**下一步（按成本）**：
1. 手工在客机上跑一次 runner，看它第一步做什么、停在哪：
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\agent\Invoke-M5Tests.ps1 -Filter '^m5\.gui$'
   ```
   （它可能会先做 `ctest --show-only`、`git` 或枚举 `tests.json`；**git 是最可疑的**：
   客机 worktree 的 `.git` 是**指向主仓的指针**，而这次 staging 重解了 git metadata）
2. 若确认卡在 git：把 runner 里对 git 的调用去掉或加超时（测试运行不需要版本信息）；
3. 修好后 `Reset-M5Guest` → 清 `C:\m5\control\*.request.ps1` → 再跑单例验证 <60 s。
### 3.53 2026-10-27 07:00：**runner 本身没问题**；是队列卡住 + 我一个无效探测

- **无效探测（先纠正自己）**：我第一次手工跑 runner 时**没给 `-RequestId`**，而它是
  `[Parameter(Mandatory)]` ⇒ PowerShell 停在交互提示上，60 秒"无输出且存活"——
  **那不是挂住，是在等输入**。带 `-RequestId probe-0001` 重跑：**`1 of 1 passed`，45 秒内完成**，
  并生成了 `run-probe-0001` 目录。
- **队列证据**：`C:\m5\control` 里三个请求文件都在，内容也**合法**（例如
  `& 'C:/m5/agent/Invoke-M5Tests.ps1' … -RequestId 'b4c523cc…' -Filter '^integration\.m5\.cli_e2e$' …`），
  但 `run-b4c523cc…` **从未被创建** ⇒ **那条请求根本没开始跑**。
- ⇒ 真正的原因是**agent 的排空循环卡在最老的一条请求上**（`18489f0e…`，05:17 的 m5 组），
  后面的请求永远排队。

**下一步（一次就够）**：
1. 客机清队列：`Get-ChildItem C:\m5\control -Filter '*.request.ps1' | Remove-Item -Force`；
2. `schtasks /end /tn 'MyRamDisk-M5-Agent'` → `schtasks /run /tn 'MyRamDisk-M5-Agent'`；
3. `Reset-M5Guest`；
4. **只提一个短请求**（`-Filter '^m5\.gui$'`）验证 <60 s，再提 `cli_e2e` 单例；
5. 若 `cli_e2e` 单例仍超过 10 分钟，**那才**是产品侧要查的问题——届时用本次已验证可用的手工 runner
   （带 `-RequestId`、加 `-TraceTestCases`）直接看它停在哪一例。
### 3.54 2026-10-27 07:05：队列修好后 **`cli_e2e` 的真实失败暴露出来**（这是可查的产品症状）

- 清队列 + `schtasks /end` + `/run` + `Reset-M5Guest` 之后：短请求 `^m5\.gui$` **5 s 通过**；
  `cli_e2e` **126 s 跑完（不再卡）并失败**。⇒ 之前一小时所有"卡住"都是**队列**，与产品无关。
- 失败原因（客机 `last-service.log` 原文）：
  ```text
  action=create-bundle code=4107 native=0 step=WorkerPipeChannel::read
  detail=pipe transfer exceeded its bounded wait
  WARNING: 1 worker process(es) survived the run
  ```
  ⇒ **`create-bundle` 等 worker 的私有管道超时**：worker 要么没起来、要么没在预算内应答；
  并且**留下 1 个 worker 进程**。
- **下一步（按成本）**：
  1. 看 `C:\m5\last-trace.log`（服务与 worker 共写的步骤轨迹）——它会指出 worker 停在哪个动作之前的哪一步；
  2. 单独跑一次 `myrd-m5-worker.exe` 看它能否独立启动（它需要 job object + 私有管道名参数）；
  3. 这类"worker 不应答"在本会话早期（T1-04/T1-05）出现过，`m5-lessons.md` 里可能有同源记录，
     先对照再动手；
  4. 若与今天的改动相关：**最先怀疑 `Uuid()` 默认构造放宽**（nil uuid 流进 worker 命令 → worker 拒绝/挂），
     验证方法是把该放宽**临时收回**（给需要默认构造的三个线格式结构改用 `std::optional<Uuid>`）
     再跑 `cli_e2e`。
### 3.55 2026-10-27 07:10：`create-bundle` 超时的**根因在 worker 的候选盘枚举**

`C:\m5\last-trace.log` 尾部（时间戳是同一台客机上的连续事件）：

```text
22:48:17.249Z enumerate_disk_candidates: opening the candidate
22:50:17.137Z opstore.write: waiting for gate          ← 正好 120 s 之后
```

⇒ worker 在 **`enumerate_disk_candidates` 打开某个候选盘时阻塞了整整 120 秒**（正好是那个
`pipe transfer exceeded its bounded wait` 的预算），期间不响应 `create-bundle`，于是服务判超时、
留下 1 个 worker。

这不是新现象：`m5-lessons.md` 里记过"打开每一个候选盘去问盘号，在设备正在退役的窗口里会阻塞约
11 秒"（G4 延后的理由之一）。**这次是 120 秒**，因为客机上还有**上一次 `cli_e2e` 留下的 AIM 代理设备**
——一个已经半死、又没被移除的设备会让 `CreateFile` 长时间挂住。

**两条路，建议都做**：
1. **客机侧（立即）**：`Reset-M5Guest.ps1` 增加"移除残留 AIM 设备"的能力（或在 e2e 前重启客机）。
   目前它只清 profile/operation 记录与进程，**设备还在**。
2. **产品侧（T4 应有）**：`enumerate_disk_candidates` 对**每个候选**的开盘操作加**有界超时**
   （例如 2 s），并把超时的候选记为"跳过"而不是让整个枚举挂住——一个第三方的坏设备**不应该**
   让我们的启动停 120 秒。这条应记入 T4 的遗留清单（属于"产品在恶劣设备环境下的健壮性"）。

**验证方法（便宜）**：客机重启 → `Reset` → 单独跑 `cli_e2e`；若通过，则确认根因是残留设备而非代码。
### 3.56 2026-10-27 07:15：**`cli_e2e` 通过（72 s）——"回归"撤回，根因是残留 AIM 设备**

- 客机：硬重启 → `Reset-M5Guest` → 单独跑 `cli_e2e` ⇒ **1 of 1 passed，72 s**（请求 `2f749544…`）。
- ⇒ **3.51 里"产品回归"的怀疑正式撤回**：今天的改动没有破坏 e2e。
- 真正的因果链（现在完整）：
  1. 上一次 e2e 留下**半死的 AIM 代理设备**；
  2. 下一次 e2e 的 worker 在 `enumerate_disk_candidates: opening the candidate` 上**阻塞 120 秒**；
  3. 服务的 `create-bundle` 等到管道超时（`pipe transfer exceeded its bounded wait`），判失败并留下 worker；
  4. 该 worker 又让客机变慢/不可交互，**看起来像产品卡死**。
- **⇒ e2e 的硬前置：每次 e2e 之前必须"干净启动"**（硬重启 + `Reset-M5Guest`）。
  这也解释了本会话反复出现的"跑几轮之后就不行了"。
- **待办（T4 遗留清单）**：`enumerate_disk_candidates` 对每个候选盘的开盘加**有界超时**（如 2 s），
  超时记为"跳过"——第三方坏设备不该让启动停 120 秒。这条与 G4 延后的理由是同一个根。

**当前可信基线（客机，干净启动）**：`unit 96/96`、`model/m5 12/13`、`integration.m5.cli_e2e PASS 72 s`。
`integration` 其余 4 例仍待跑（每次跑前重启+Reset）。
### 3.57 2026-10-27 07:20：**候选盘枚举加界生效 —— 连续两次 e2e 不需要重启**

| 运行 | 用例 | 用时 | 结果 | 请求 |
|---|---|---|---|---|
| 第 1 次（刚重启+Reset 后） | `integration.m5.cli_e2e` | **72 s** | **PASS** | `d4713c4e…` |
| 第 2 次（**不重启**，同一台机） | `integration.m5.cli_e2e` | **85 s** | **PASS** | `d82c88a0…` |

改动（`b1faaa3`）：`physical_drive_locator.cpp` 里三处仍调用**无界** `enumerate_disk_candidates()` 的
地方（其中一处就在 worker 的启动路径上）改为调用已有的 `enumerate_disk_candidates_within(kEnumerateTimeout)`
——那个包装函数自己的注释写的就是"runs it with a bound, because it has none of its own"。

**意义**：之前"跑一次 e2e 之后必须重启客机"的限制**消失了**。一个半死的第三方设备最多让枚举
多等 10 秒（被跳过），而不是让 `create-bundle` 等 120 秒后失败、再留下一个 worker 把整台客机拖慢。
这同时消除了本会话反复出现的"跑几轮就不行了"的主要来源。

**当前可信基线（客机）**：
- `^unit\.` **96/96**（227 s）
- `^model\.|^m5\.` **12/13**（唯一红为已知批内间发 `m5.production_execution_backend`）
- `integration.m5.cli_e2e` **连续两次 PASS（72 s / 85 s）**
### 3.58 2026-10-27 07:25：`model/m5` 组 **13/13**（含此前间发的那一例）

- 请求 `b90d1a38…`，**114 s，13 of 13 passed**——`m5.production_execution_backend` 这次也过了
  （它是已知批内间发项，单独跑通过；本轮在批内也通过）。
- 结合 3.57：**候选盘枚举加界之后，客机测量链路稳定了**（`cli_e2e` 连续两次通过、`model/m5` 全绿、
  `unit` 96/96 早前已测）。

**当前三段客机基线（Release，干净启动）**：

| 组 | 用例 | 用时 | 结果 |
|---|---|---|---|
| `^unit\.` | 96 | 227 s | **96/96** |
| `^model\.|^m5\.` | 13 | 114 s | **13/13** |
| `integration.m5.cli_e2e` | 1 | 72 s / 85 s | **两次连续 PASS（不重启）** |

这三行就是 T4-04 "M4 回归"比较用的基线；`integration` 其余 4 例仍待逐例跑（`powerloss_verify` 单独必失，
需主机断电，属预期）。
### 3.59 2026-10-27 07:30：`install_uninstall` **9 s PASS**、`gui_workflow` **25 s PASS**

- `^integration\.m5\.install_uninstall$`：请求 `ef240fb9…`，**9 s，1 of 1 passed**（比早前 37 s 更快：
  干净客机 + 枚举加界）。
- `^integration\.m5\.gui_workflow$`：请求 `ae6eb78d…`，**25 s，1 of 1 passed**。
- ⇒ **`integration.m5` 组四例已各有通过读数**（`cli_e2e` 72/85 s、`install_uninstall` 9 s、
  `gui_workflow` 25 s、`powerloss_prepare` 早前 52 s），只剩 `powerloss_verify`——它**必须由主机先断电**，
  属于 T1-09 三段流程的一部分，单独跑必失，不计入红。

**客机基线汇总（Release，干净启动）**：

| 组/用例 | 用时 | 结果 |
|---|---|---|
| `^unit\.`（96） | 227 s | **96/96** |
| `^model\.|^m5\.`（13） | 114 s | **13/13** |
| `integration.m5.cli_e2e` | 72 s / 85 s | **PASS ×2（不重启）** |
| `integration.m5.install_uninstall` | 9 s | **PASS** |
| `integration.m5.gui_workflow` | 25 s | **PASS** |

**这五行就是 T4-04 的 M4 回归基线**。`integration.m3`/`m4`/`m1`/`m2` 各组的读数仍待补（逐组跑即可）。
### 3.60 2026-10-27 07:35：**客机全套分组普查完成（除 powerloss_verify）**

| 组 | 用例 | 用时 | 结果 | 请求 |
|---|---|---|---|---|
| `^unit\.` | 96 | 227 s | **96/96** | `b37eed00…` |
| `^model\.|^m5\.` | 13 | 114 s | **13/13** | `b90d1a38…` |
| `^integration\.m1\.` | 1 | 5 s | **1/1** | `834f2ac9…` |
| `^integration\.m2\.` | 4 | 19 s | **4/4** | `25dcb84c…` |
| `^integration\.m3\.` | 31 | 97 s | **31/31** | `4d3bc185…` |
| `^integration\.m4\.` | 3 | 9 s | **3/3** | `3dbee656…` |
| `^integration\.m5\.` | 5 | — | **4 PASS**（cli 72/85 s、install 9 s、gui 25 s、prepare 52 s）+ `powerloss_verify` 需主机断电 | 见 3.57–3.59 |
| `^structural\.` | 3 | 5 s | **1/3**（`m2.storage_independent_build`、`m5.execution_boundary` 失败＝**已知客机环境**：`vswhere.exe not found`） | `860d0b41…` |
| `^m5\.gui$` 单例 | 1 | 5 s | **1/1** | `b9cd6b04…` |

**合计：155 组内用例中 152 通过、3 未通过**——其中 2 条是**已知客机环境**（structural 的两条），
1 条是 `powerloss_verify`（**按设计**必须由主机先断电，单独跑必失）。

⇒ **T4-04 的"M4 回归 + 客机基线"这一半已经具备**：这就是可以拿来比较的读数，而且与早前
`152/154` 的预期一致（本表多算了两条 `m5.gui` 单例重复跑）。
**仍缺**：`integration.m5.cli_e2e/powerloss 三段`作为一个整体的一次完整跑（T1-09 已做过一次），
以及 Debug 配置的构建（T4-04 要求 Debug+Release 双干净）。
### 3.61 2026-10-27 07:40：**Debug 配置构建通过**（T4-04 "双配置干净"的一半）

- `cmake --build out/build/msvc --config Debug`：**451/451 目标，132 s，零 error / 零 warning**。
- 仍缺（T4-04 自身要做）：**Release 的 `--clean-first` 全量重建**（当前 Release 是增量构建，
  虽然八个套件绿，但"干净"要以一次 clean-first 为准）。
- 记一条今天学到的：**头文件改动后必须 `--clean-first`**，否则会出现"半套改动"的假失败
  （今天在卷序列号那次踩过：编码端没写、解码端在读）。

**T4-04 待办清单（现在很具体）**：
1. `--clean-first` 重建 Release + 全绿（客机分组读数已有，见 3.60）；
2. Debug 配置的客机读数（可选：至少本机构建通过）；
3. **冻结哈希**：对 Release 产物（`myrd-service.exe`、`myrd-cli.exe`、`myrd-gui.exe`、`myrd-m5-worker.exe`）
   计算 SHA-256 并写入 manifest；
4. **release manifest**：`frozen_at`、文件清单+哈希、构建配置、客机分组读数摘要、证据文件清单；
5. 用 `tools/Verify-M5Evidence.ps1` 校验整组证据（它**永不打印 PASS**，只会 accepted 或具名拒绝）。
### 3.62 2026-10-27 07:45：**Release `--clean-first` 全量重建通过** —— T4-04 "双配置干净"两半齐了

- `cmake --build out/build/msvc --config Release --clean-first`：**451/451 目标，170 s，零 error / 零 warning**。
- 加上 3.61 的 Debug（451/451，132 s，零 error/warning）⇒ **T4-04 的构建要求已满足**，
  而且这是"从零重建"的干净，不是增量。

**T4-04 剩余三项**（都不依赖客机）：
1. **冻结哈希**：对四个 Release 产物算 SHA-256；
2. **release manifest**：`frozen_at` + 文件/哈希 + 配置（Release/Debug 各 451 目标 132/170 s）
   + 客机分组读数摘要（3.60 那张表）+ 证据文件清单；
3. 用 `tools/Verify-M5Evidence.ps1 -EvidenceDirectory <目录>` 校验整组（**只会 accepted 或具名拒绝**）。

**注意**：冻结必须在**最后一次代码改动之后**做；T4-02/T4-03 的功能改动仍会改变产物，
所以冻结步骤放在这两项完成之后，而不是现在。
### 3.63 2026-10-27 07:50：manifest 生成器就绪（**工具**，不是冻结）

- `tools/New-M5ReleaseManifest.ps1`：对四个 Release 产物算 SHA-256、收集证据目录里的 results 摘要
  （只记 name/exit_code/request_id，**不复制内容**——结果文件本身才是证据）、缺文件或坏 JSON 就拒绝。
- 实测：`manifest: wrote …release-manifest.json (4 binaries, 0 readings)`，哈希例如
  `myrd-service.exe 1110016 B / 20b273df…`、`myrd-cli.exe 185344 B / 6fc50e44…`。
- 文件里写明了两条自我约束：① `$PSScriptRoot` 不能用在 param 默认值里（实测报"empty string"），
  已在 body 里解析；② manifest 明确列出**它不主张什么**——它不证明客机跑过，那是 results 里的 request id 的职责。
- **冻结仍未发生**：要等 T4-02/T4-03 完成后的最后一次构建，再跑这个脚本 + `Verify-M5Evidence.ps1`。
### 3.64 2026-10-27 08:00：证据链**端到端跑通**（工具侧），冻结仍未做

- 组了一个真实证据集（`out/m5-evidence`）：**8 个 results 文件**，来自今天客机上的 8 次真实运行
  （model/m5、integration m1/m2/m3/m4、install、gui、cli_e2e）。
- `New-M5ReleaseManifest.ps1` → **4 binaries + 8 readings**；`Verify-M5Evidence.ps1` →
  **`accepted`（exit 0）**；`-SelfTest` → 四个坏证据集**仍全部被拒**（exit 0）。
- **两处实测修正**（都写进代码注释）：
  ① runner 写的是**数组**（每用例一条），`[int]$parsed.exit_code` 会报
     `Cannot convert System.Object[] to System.Int32` ⇒ manifest 改为记 cases/failed/worst_exit 并列出失败用例名；
  ② validator 里**硬编码的用例文件名清单会漂移**：它对一个**有意不含** power-loss 两例的证据集仍在索要它们
     ⇒ 改为"manifest + 其中引用的 readings，至少 3 条"。
- **冻结仍未做**：等 T4-02/T4-03 完成后的最后一次构建。当前 `frozen_at` 只是"工具能跑"的时间戳，
  **不是发布冻结**。
### 3.65 2026-10-27 08:10：断电复跑：prepare 过，verify 在 `Wait-ForService` 失败（**服务日志显示它其实起来了**）

- 三段流程（`Test-M5PowerLoss.ps1`）：`prepare` **1 of 1 passed**（49 s，请求 `c519a95d…`）→ 硬断电 → 开机 →
  `verify` **0 of 1**（187 s = `Wait-ForService 180` 超时，请求 `4598f66e…`）。
- **但 verify 的 stderr 里带出的服务日志显示服务确实起来了**：`console mode, state=constructed` →
  `started, state=running recovered=0` → `serving; press Ctrl+C to stop`，并且此后有 `disk.create`
  的动作序列（`create-bundle → … → terminal`）。⇒ **服务在跑，是"CLI 没在 180 s 内连上那一次"**。
- **最可能的解释**：断电重启后这一轮的**公开管道可用时刻晚于 180 s**（开机慢 + 服务自身 recovery），
  而 verify 的等待窗口是固定的 180 s。这不是恢复逻辑的回归——但**在拿到证据前不这么断言**。
- **下一步**：
  1. 把 `verify` 的 `Wait-ForService` 从 180 s 提到 **300 s**（并打印每次探测的时刻），
     若通过即确认是窗口问题；
  2. 若 300 s 仍不通过而服务日志正常，则问题在**管道就绪**（服务绑定 vs CLI 连接），
     届时看 `\\.\pipe\` 与服务的 `surface open` 日志顺序；
  3. **T1-09 之前的通过读数（请求 `1eb2ca27…`）仍然有效**，但需要在改动后再确认一次，
     因为今天改过枚举/序列号/默认值。
### 3.66 2026-10-27 08:20：verify 等待提到 300 s；复跑改在 prepare 处失败（**必须先 Reset**）

- 已改：`test-m5-powerloss-verify.ps1` 的 `Wait-ForService 180` → **300 s**，理由写在注释里
  （3.65 量到：服务已 `serving`，而客户端 180 s 内没连上；窗口太短会把健康的服务判成失败验收）。
- 复跑：`prepare` **17 s 就失败**——因为**上一次失败的 verify 把盘留在了客机上**，
  而"同时两块盘"不在本产品支持范围内（3.17 已记）。⇒ **`Reset-M5Guest` 是 harness 的前置**，
  不只是 verify 的前置。
- **下次跑断电三段的标准顺序**（写死在这里）：
  ```powershell
  # 客机
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1
  # 主机
  .\tools\m5vm\Test-M5PowerLoss.ps1 -SkipStage
  ```
### 3.67 2026-10-27 08:25：断电复跑——verify 这次 2.8 s 就失败，读数是"盘回来了但 **runtime=Stopped**"

`prepare` 过（51 s，请求 `f201e022…`）→ 硬断电 → 开机 → `verify` 失败（2.8 s，请求 `ea867784…`）。读数：

```text
service state: STATE : 4  RUNNING            ← 服务自己起来了（这一步通过）
c6e4550b-…  U:  Ready  stopped/configured  runtime=Stopped
            activity=Idle  persistence=Dirty  health=Healthy
            generation=3  mem=0/0
```

⇒ **服务自起 ✓、盘回到提交的 generation ✓，但那一刻 `runtime=Stopped`**，所以 verify 的
"盘自己回来运行"断言失败。

**两种可能，必须分开**：
1. **时序**：verify 在服务刚启动后**立刻**查（2.8 s），自动恢复的 `disk start` 可能还没执行完；
   若是这样，verify 应该**轮询**（例如最多 60 s）而不是查一次——这是**用例**的缺陷；
2. **真实失败**：自动恢复的 start 失败了（`Dirty` 的盘需要 recovery），服务把它留在 stopped；
   若是这样，`service.log` 里会有 `auto-start refused/failed` 一行——**这是产品缺陷**。

**判据（下一步第一件事）**：看这一轮的服务日志里 `auto-start` 相关的行：
- 有 `auto-start: this profile was running…` 且随后 `terminal` 成功 ⇒ 是时序问题，改用例（轮询）；
- 有 `refused` / `kind=7` ⇒ 是产品问题，按 §3.54 的步骤查 worker 侧。
### 3.68 2026-10-27 08:30：判据到手——**自动恢复的 start 真的失败了**（不是时序）

服务日志（同一轮，`C:\ProgramData\MyRamDisk\logs\service.log` 尾部）：

```text
kind=6 disk.start phase=auto-start message=auto-start: the durable running state had no worker,
       so it was retired before the disk was started
kind=1 disk.start phase=auto-start message=auto-start: this profile was running when the service last had it
kind=7 disk.start phase=running  message=action=create-runtime code=21001 native=87
       step=assign_drive_letter detail=SetVolumeMountPointW failed
kind=7 disk.start phase=terminal  message=terminal failure without a worker-action error; marker=worker
```

⇒ **退休陈旧状态 ✓、自动 start 被准入 ✓，但 start 在 `assign_drive_letter` 处失败**：
`SetVolumeMountPointW` 返回 **native=87（ERROR_INVALID_PARAMETER）**。

**最可能的原因**（断电后特有）：断电容留了**旧的盘符映射**——`U:` 仍映射到一个已不存在的设备，
于是重新 `SetVolumeMountPointW` 到同一个盘符被拒绝。这解释了为什么"平时能起来、断电后起不来"。

**下一步（产品侧，具体）**：
1. 在 `assign_drive_letter` 里**先 `DeleteVolumeMountPointW(U:\)`**（忽略失败），再 `SetVolumeMountPointW`；
   Windows 自己也是这么清理陈旧映射的；
2. 若删除后仍失败，把 `GetLastError` 与当时的盘符占用情况一起记进日志（现在只有 native=87，
   不足以区分"盘符被占"与"卷 GUID 路径无效"）；
3. 客机验收：`Reset` → 断电三段；读数要求 `runtime=Running` 且 generation ≥ 提交值。
### 3.69 2026-10-27 08:35：删陈旧映射**没解决**，但错误信息把原因指出来了

修复后复跑（`prepare` 过 51 s，硬断电，`verify` 2.9 s 失败，请求 `05db028b…`）：

```text
2a2f1678-…  T:  Ready  stopped/configured  runtime=Stopped  persistence=Dirty  generation=3
kind=7 disk.start phase=running  action=create-runtime code=21001 native=87
       step=assign_drive_letter detail=SetVolumeMountPointW failed for T:\   ← 现在带了 GUID
```

**结论**：`DeleteVolumeMountPointW` + 重试**没有改变结果**，所以不是"陈旧映射占着盘符"。
`SetVolumeMountPointW` 对 `T:\` 与一个**卷 GUID 路径**返回 87（ERROR_INVALID_PARAMETER），
最可信的解释是**那个 GUID 路径此刻不存在**——即断电容留后、卷被重新创建，**GUID 变了**，
而 `assign_drive_letter` 用的是**profile 记录里的旧 GUID**。

**下一步（产品侧，具体）**：
1. 在 `create-device` 之后**从活动卷重新取 GUID**（`GetVolumeNameForVolumeDeviceW` 或
   `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` + 卷设备路径 → 卷 GUID），**用活动值**去
   `SetVolumeMountPointW`，并把新 GUID 写回记录（否则每次恢复都会重演）；
2. 如果取不到活动 GUID，就把"记录 GUID vs 活动卷设备路径"一起打进日志——现在日志已能区分，
   只差活动值；
3. 客机验收仍是断电三段：要求 `runtime=Running`、generation ≥ 提交值、标记字节一致。
---

## 3.70 当前状态一览（2026-10-27 08:40，**新会话从这里读起**）

### 已完成并有客机读数
- **T1-01…T1-10 + T1-11**：检查点策略（`generation 2→3` 自动推进）、内存可见性（20/20）、
  中断提交判定（`verdict=2 mask=0x0`）、默认值收紧（60 s / 64 MiB）、断电三段（**曾在 T1-09 通过，
  之后又因下述缺陷复现失败**）。
- **T3-01…T3-05**：客户端边界（闭包断言、已证明能失败）、窗口/托盘/列表（`aggregate` 唯一判定）、
  命令不冻结界面、三处确认流、客机 `PASS T3-05`。
- **T4-01**：安装/卸载客机通过；三条 ACL 规则已记档。
- **T4-02 部分**：卷序列号端到端 + 客机读数 `serial=5A42-9D28`。
- **T4-04 部分**：Debug/Release **双配置干净构建**（各 451 目标）、manifest 生成器与证据校验器
  （**永不打印 PASS**，自检拒绝四个坏证据集，端到端 accepted）。
- **客机基线**：unit 96/96、model/m5 13/13、integration m1 1/1 / m2 4/4 / m3 31/31 / m4 3/3、
  m5 四例 PASS（cli_e2e 72/85 s、install 9 s、gui 25 s、prepare 51 s）、structural 1/3（两条已知环境）。

### 当前**唯一**阻塞（产品缺陷，已定位到具体一行）
**断电恢复后的 `disk start` 在 `assign_drive_letter` 失败，`SetVolumeMountPointW` 返回 87**：
- 事实链：陈旧 `running` 被退休 ✓ → auto-start 被准入 ✓ → `create-runtime` 在挂盘符处失败；
- 已排除：陈旧盘符映射（先 `DeleteVolumeMountPointW` + 重试 100 ms，结果不变）；
- 最可信原因：**恢复后卷被重建、GUID 变了，而挂载用的是 profile 记录里的旧 GUID**；
- **下一步**：在 `create-device` 之后**从活动卷重新取 GUID**（`GetVolumeNameForVolumeDeviceW` 或
  卷设备路径），用活动值挂载，并**把新 GUID 写回记录**；取不到时把记录值与活动设备路径一起记日志。

### 之后剩余
1. **T4-02**：卷标（创建参数 + 挂载后 `SetVolumeLabelW`）、易失盘模式、Load Only、单文件 VHDX 导出；
2. **T4-03**：在线 compaction（第二套 durable phase + reconciliation + 自动触发；journal 增长有界可观测）；
3. **T4-04**：最后再跑一次双配置干净构建 → `New-M5ReleaseManifest.ps1` 冻结 → `Verify-M5Evidence.ps1` 校验。

### 每次跑客机测试的标准顺序
```
客机: powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1
主机: .\tools\m5vm\Invoke-M5Vm.ps1 -Stage -Configuration Release   # 必须看到 "staged to usr@"
      .\tools\m5vm\Invoke-M5Vm.ps1 -Run -Filter '^...$'
断电: .\tools\m5vm\Test-M5PowerLoss.ps1 -SkipStage
```
### 3.71 2026-10-27 08:45：**缺陷的确切位置找到了**（`service_worker_control.cpp` 的 GUID 比对）

```cpp
// src/management/service_worker_control.cpp 附近
expected_volume = profile.volume_guid.value().to_string();     // ← 记录里的旧 GUID
if (status.value().volume->volume_guid != expected_volume) break;   // ← 不一致就"放弃等待"
```

这解释了 3.69 的现象：断电容留后卷被**重建**，worker 报的是**新 GUID**；
这段比对发现"不一致"就 `break`，于是后续流程**用记录里的旧 GUID**去
`SetVolumeMountPointW` ⇒ **87（参数无效，因为那个卷 GUID 路径已不存在）**。

**所以修复不是"重试挂载"，而是在这里承认"活动卷才是真相"**：
1. 不一致时**不要 break**，把 `status.value().volume->volume_guid`（以及 disk_guid）**采纳**为活动身份；
2. 把它**写回 profile 记录**（`volume_guid`、如需要还有 `gpt_disk_guid`）——否则每次恢复都重演；
3. `disk_guid`（磁盘 GUID）在同一处比较里也有同样的问题，一并采纳；
4. 采纳动作要落一条日志（`volume identity adopted after recovery: recorded=… live=…`），
   这样下一次审计能看出"身份换过"。

**验收**：`Reset` → `Test-M5PowerLoss.ps1 -SkipStage`，要求 `runtime=Running`、generation ≥ 提交值、
标记字节一致、worker 恰好 1 个。
### 3.72 2026-10-27 08:50：3.71 的前提**可能不成立**，先查"GUID 文本形态"再动手

复查代码后发现 3.71 的推断缺一步证据：服务日志里**没有**"身份不一致"那条，说明比对大概率**通过了**。
于是真问题回到"87 到底是什么"，而最可能的是**传给 `SetVolumeMountPointW` 的卷名形态不对**：

- `fill_persistent_identity()`（`windows_volume.cpp`）把卷身份存成**裸 GUID 文本**
  （`\\?\Volume{xxxxxxxx-…}\` 里抽出中间那段，小写），用于**比较**；
- 而 `SetVolumeMountPointW` 要的是**完整卷名** `\\?\Volume{xxxxxxxx-….}\`；
- `assign_drive_letter()` 里那句 `GetVolumeNameForVolumeMountPointW` 拿到的是**完整名**，
  与裸 GUID 比较（去掉尾部反斜杠）**永远不会相等**——除非 `Win32VolumeAdapter` 自己的
  `ProductVolume::volume_guid`（`std::wstring`，与 identity 的那个 `std::string` 是**两个不同字段**）
  里存的是完整名。

**⇒ 第一件事是**：读 `Win32VolumeAdapter::ProductVolume` 的构造处，确认 `volume_guid` 到底是
裸 GUID 还是 `\\?\Volume{…}\`。若是裸的，**修复就是把裸 GUID 包成完整卷名再传给
`SetVolumeMountPointW`**（`L"\\\\?\\Volume{" + guid + L"}\\"`），而不是加超时或重试。
`ERROR_INVALID_PARAMETER (87)` 正是"卷名格式非法"的返回值，与本假设一致。
### 3.73 2026-10-27 08:55：**先怀疑"跑的是旧二进制"**，再谈产品缺陷

刚加的那行日志（`585f812`：`SetVolumeMountPointW failed for T:\ at <GUID> (first error N)`）
**在客机日志里没有出现**——最新那条仍是旧格式：

```text
action=create-runtime code=21001 native=87 step=assign_drive_letter detail=SetVolumeMountPointW failed for T:\
```

⇒ **那次失败跑的不是带新日志的二进制**。所以在继续推断"卷名形态/身份形态"之前，必须先确认：

1. **客机上实际被 SCM 启动的 `binPath` 是哪一个**（`sc qc`）；它可能是早前安装到
   `C:\Program Files\MyRamDisk` 的副本，而不是 staging 到 repo 目录的那份；
2. 那份二进制的 **SHA-256** 与本地 `out/build/msvc/src/Release/myrd-service.exe` 是否一致；
3. 只有确认跑的是**最新构建**，日志格式也对得上，再谈 87 的原因（此时 `(first error …)` 与 GUID
   会随行给出，3.71/3.72 的假设可当场判定）。

**这一条比任何代码修改都优先**：本会话已经两次因为"以为跑的是新二进制"而误判
（陈旧对象、`Select-Object -Last 1` 吞掉 staging 错误），第三次的代价是**改错地方**。
### 3.74 2026-10-27 09:00：**新诊断字符串在 worker 里，但日志在 `T:\` 处被截断**

- 已确认（`certutil` 对比）：客机 `myrd-m5-worker.exe` = 本地新构建（`91113590…`，07:24），
  **不是旧二进制**；`myrd-service.exe` 里没有该字符串（符合预期——动作在 worker 侧执行）。
- 但日志行仍是 `detail=SetVolumeMountPointW failed for T:\` 就结束，**没有 ` at <GUID> (first error N)`**。
  我的 `detail` 是 `std::wstring` 拼接，不会被 `\0` 截断 ⇒ **是日志行被截断**（写日志的缓冲/长度上限），
  恰好切在 `T:\` 之后。
- **下一步（一行改动）**：把诊断**缩短**到能在同一行放下——只放 GUID 与错误码，不放 `T:\` 路径，
  例如 `mount=T: guid=<短> first=<N>`；或让 logger 支持第二行。**然后在下次跑时读取 GUID 形态**，
  立刻可判定 3.71/3.72 的两种假设（裸 GUID vs 完整卷名）哪一个成立。
- **注意**：这次终于排除了"跑旧二进制"这个反复出现的干扰项（哈希对齐），
  所以下一次的读数就是可信的产品读数。
### 3.75 2026-10-27 09:05：诊断生效——**卷名是完整路径，87 不是格式问题**

新的短诊断在客机上打出来了（worker 哈希已对齐 `1e8c7aba…`）：

```text
kind=7 disk.start phase=running action=create-runtime code=21001 native=87
step=assign_drive_letter
detail=set-mount failed first=87 second=87 guid=\\?\Volume{87792e03-9150-4130-ad02-b0f8b5700…
```

⇒ **`owned.volume_guid` 是完整的 `\\?\Volume{GUID}\` 路径，不是裸 GUID**（3.72 的假设**被否定**）；
`SetVolumeMountPointW` 对该路径与 `T:\` 连续两次返回 **87**，即使之前已 `DeleteVolumeMountPointW`。

**于是回到 3.71 的方向**（但这次有证据支撑）：**该卷 GUID 路径此刻不存在**，即
**恢复后卷被重建、GUID 已变**，而我们用的是记录里的旧 GUID。`DeleteVolumeMountPointW` 无用，
因为它清的是盘符映射，不是"卷 GUID 是否有效"。

**下一步（两个读数即可定论，都很便宜）**：
1. 在服务日志里**同时打出 worker 报的活动卷 GUID**（`status.volume->volume_guid`）与记录 GUID——
   两者若不同，3.71 成立，修复就是"采纳活动身份并写回记录"；
2. 或直接在 worker 的 `create-runtime` 完成后用 `GetVolumeNameForVolumeDeviceW`（对设备路径）
   取活动卷名，若取到与记录不同 ⇒ 同上。

**注意**：`assign_drive_letter` 里那句 `GetVolumeNameForVolumeMountPointW(mount)` 的比较也不可靠——
它对 `T:\` 返回失败（因为 `T:\` 此刻是空的），于是走 Set，而不是走"已挂载同一卷"的快路径。
### 3.76 2026-10-27 09:10：修复的**施工单**（下一轮照此一次做完）

目标：断电恢复后 `disk start` 能挂上盘符——即**用活动卷的身份**，而不是记录里的旧身份。

**改一处即可，位置：`src/run/windows_volume.cpp` 的 `Win32VolumeAdapter::assign_drive_letter`**
（当前已在 `SetVolumeMountPointW` 失败时 `Delete` + 重试一次）：

1. 在**两次 Set 都失败**之后，调用同文件里已有的卷枚举（约 1296 行的 `find_volumes` / 或
   `enumerate` 路径），按**同一组 extents**匹配候选卷（`owned.extents` 已有）；
2. 若**恰好一个**候选卷与 `owned.extents` 相同，取它的 `volume_guid`（完整 `\\?\Volume{…}\`），
   **再 Set 一次**；成功则返回 success，并**把该卷名写回**调用方能写回的地方
   （`ProductVolume` 是 const 传入，所以写回要么改签名、要么由调用方在 success 后重新枚举一次记录；
   最小改法：新增一个 `assign_drive_letter(ProductVolume& owned, ...)` 重载或在成功时把
   adopted name 记入日志，**同时**让 `runtime_adapter` 在下一次 `find` 时自然更新记录）；
3. 无论成功失败，日志写：`mounted with adopted guid=… recorded=…` —— 这是"身份换过"的审计证据。

**验收（必须）**：`Reset-M5Guest` → `Test-M5PowerLoss.ps1 -SkipStage` ⇒ `verify` 通过，
读数要求 `runtime=Running`、generation ≥ 提交值、标记字节一致、worker 恰好 1 个。
**若一次不到位**：先只做"第 1-2 步 + 日志"，因为**挂上**是阻塞项，"写回记录"可以随后再做
（下次恢复会再采纳一次，那只影响日志噪声，不影响正确性）。
### 3.77 2026-10-27 09:15：**T1-09 恢复通过**——用活动卷身份挂载

```text
prepare 1 of 1 passed (52 s, request f176d457…)
hard cut
verify  1 of 1 passed ( 5 s, request 4458cf90…)
PASS integration.m5.powerloss: the disk came back by itself after a hard power cut
```

**修复**（`windows_volume.cpp` 的 `assign_drive_letter`）：两次 `SetVolumeMountPointW` 失败后，
按 **extents**（跨重建仍然不变的身份）在活动卷里找候选；若**恰好一个**匹配且其卷名与记录不同，
用**活动卷名**再 Set 一次 ⇒ 成功。

**三层证据链**（本会话最后三小时）：
1. 记录里的 `\\?\Volume{…}\` **格式合法**（3.75 的短诊断打出来了），所以 87 不是格式问题；
2. `DeleteVolumeMountPointW` + 重试**无效**（3.69），所以不是盘符被占；
3. 同一设备、同一 extents 上**存在一个不同 GUID 的活动卷** ⇒ 恢复时卷被重建，记录身份过期。

**未做（记录在案）**：把采纳到的活动身份**写回 profile 记录**——目前每次恢复都会再采纳一次，
只多一行日志，不影响正确性；写回是后续项。
### 3.78 2026-10-27 09:25：卷标纵切**第一层已落**（只加字段，未开任何入口）

- `src/control/command.h` 的 `DiskCreateCommand` 末尾新增
  `std::optional<std::wstring> volume_label;`，注释写明：**它是创建选项而不是 profile 字段**的理由
  （要打在 worker 刚挂载的那块卷上，而不是"可能已不存在的卷"的记录上）。
- **故意只做这一层**：解析器还不接受 `--volume-label`，所以**没有任何路径能传一个"悄悄不生效"的标签**。
- **剩余步骤（位置已定位，按序做）**：
  1. `src/cli/cli_parser.cpp` 的 `verb == L"create"` 分支（约 260 行，`--owner` 之后）加
     `--volume-label`：取值 → 长度 ≤ 32 → 非法字符拒绝；同时在约 21 行的用法里补一行；
  2. 命令编解码（`src/control/ipc_codec.cpp`）：新 tag + 写 + 读，两侧同时改（客户机与服务一起发版，
     **追加字段不需要协议次版本号**——这是已记录的决定）；
  3. `LifecycleOperationIntent` 携带它；`profile::Profile` 加 tag 36（**同时**要把
     `m4_profile_codec_tests.cpp` 的黄金样本更新——今天已付过这次学费）；
  4. worker 在 `readiness-io` 之后调 `SetVolumeLabelW`；失败**不**让 start 失败，但要记日志；
  5. 客机读数：`fsutil fsinfo volumeinfo <letter>:` 的 `Volume Label` 行。
### 3.79 2026-10-27 09:35：卷标纵切再进一层（intent + 相等性），CLI 入口仍关闭

- `src/control/command.h`：`DiskCreateCommand::volume_label`（3.78）。
- `src/control/ipc_codec.cpp`：字段 11 双向编解码 + 边界校验（33 字符在边界被拒），描述符字段数 10 → 11。
- `src/management/service_core.h`：`LifecycleOperationIntent::volume_label`，**声明在最后**，
  使既有的按位置初始化全部保持原义。
- `src/management/service_core.cpp`：`lifecycle_intent_equal` **比较该字段**——只差标签的两个 create
  不是同一个请求，若被判相等，一个请求可能被另一个的操作结果回答。
- **CLI 仍不接受 `--volume-label`**（3.78 的决定保持）：管线通了、门锁着，直到服务与 worker 能兑现。
- **下一步**：`Profile` tag 36（记得同时更新 `m4_profile_codec_tests.cpp` 的黄金样本）→ worker 在
  `readiness-io` 后 `SetVolumeLabelW` → 再把 CLI 选项加回 → 客机 `fsutil fsinfo volumeinfo` 读数。
### 3.80 2026-10-27 09:45：profile 编解码**回退**——`kLastKnownTag` 不是"最高可识别"，而是"必须存在的范围"

- 现象：把 `kLastKnownTag` 从 `index_headroom_percent`(35) 提到 `volume_label`(36) 后，
  **黄金样本解码失败**（`decoded.ok()` 为假）。⇒ 这个解码器把 `kLastKnownTag` 当作
  **必需字段上界**，不是"认得的最高 tag"。少了 36 的记录会被判非法。
- 于是回退 `profile_codec.cpp`（22 个编解码用例恢复全绿），**保留** `profile_types.h` 的
  tag 36 + `Profile::volume_label` 字段。
- **下次的正确入口**：先看这个文件怎么表达"可选 vs 必需"（例如是否存在按 tag 的
  `FieldRequirement`/位图，或 `decode_typed_payload` 的 required 列表），**再**决定
  36 是可选还是"v1 必需、缺省即视为无标签"。
  两条可行路线：
  1. **可选**：给 36 一个"缺失即空"的规则（最像 `volume_guid` 这类 optional 字段的处理）；
  2. **必需**：那就必须**同时**把黄金样本更新为含 36 的新字节（今天已走过一次这条路，
     且会更动所有既有 profile 的兼容性假设——**不推荐**）。
- **教训**：`kLastKnownTag` 这种常量的语义必须**从使用点反推**，不能从名字推。
### 3.81 2026-10-27 09:55：profile 编解码**再次回退**，但这次学到了格式的真实规则

**规则（从解码器读出来的，不是猜的）**：`decode_profile` 要求 **1..kLastKnownTag 的每一个 tag 都存在**
（`known[tag] == nullptr` 即"Profile is missing a required field"），且每个已知 tag 必须带
`kProfileMandatoryField` 标志。⇒ **这个 v1 profile 格式没有"省略字段"这回事**。

**因此 tag 36 只能是"必需字段 + 哨兵值"**：
- 写：`value.volume_label.value_or(L"\u0001")`——字段编码器**拒绝零长度值**（实测 `encoded.ok()` 为假），
  所以"无标签"必须用一个可编码的值；**控制字符是诚实的哨兵**（CLI 拒绝控制字符、Windows 也不允许，
  所以不会与用户能请求的标签冲突）；
- 读：解出等于 `L"\u0001"` ⇒ 视为无标签。
- 这条路**必然改变黄金字节**（每个记录都多一个字段），需要重算黄金样本。

**这次为何仍然回退**：改完之后 `encoded.value() == minimal_golden()` 如预期失败，我需要打印新的黄金字节，
但生成打印的那次构建把 MSVC 环境脚本名打错（`EnterMsvcEnvironment.ps1`），导致编译环境不完整、
`test_framework.h` 找不到头文件，打印没跑出来。**没有新字节就不能提交**（否则留一棵红的树），
所以整棵改动回退，构建与 22 个编解码用例恢复全绿、工作树干净。

**下次的正确做法（一次到位）**：
1. 在 `tests/unit/m4_profile_codec_tests.cpp` 的 `matches_exact_golden_bytes` 里**先放打印**
   （`std::fprintf(stderr, "golden hex …")` + `#include <cstdio>`），构建该目标并运行，**抄下十六进制**；
2. 再把十六进制**按 64 字符分行**替换 `minimal_golden()` 的 `from_hex(...)` 参数；
3. 最后删掉打印，重跑 —— 三个测试（golden / decode-reencode / validation）应全绿；
4. **注意**：这是"v1 记录不向后兼容"的决定——旧版本写下的 profile 将无法解码。
   产品未发布、profile 都是测试产物，可接受；但**必须写进 release manifest 的注意事项**。
### 3.82 2026-10-27 10:00：黄金样本再生**改用文件重定向**（管道会截断长行）

- 本轮拿到了新格式的十六进制（`…2400020101000000 01…`——tag 36、utf8、长度 1、哨兵 `0x01`），
  但**两次都因为工具链细节没落成**：
  1. 用 `& exe 2>&1 | Out-String` 取 stderr 时，**长行被截断**（只拿到 63 个字符，实际约 374），
     于是写进黄金的是**前缀**——必然是错的；
  2. 环境脚本名我连续几次打成 `EnterMsvcEnvironment.ps1`（正确是 `Enter-MsvcEn**v**ironment.ps1`），
     没有 MSVC 环境时编译连 `test_framework.h` 都找不到，而我误以为是"文件被删了"。
- 两次都已回退：构建干净、22 个编解码用例全绿、工作树 0 改动。
- **下次的正确做法（写死）**：
  ```powershell
  # 用文件重定向，不要经过管道
  & .\out\build\msvc\tests\Release\myrd_m4_profile_codec_tests.exe 2> golden.txt
  # 再从文件里取完整十六进制
  $hex = [regex]::Match((Get-Content golden.txt -Raw), 'golden hex ([0-9a-f]+)').Groups[1].Value
  ```
  然后按 64 字符分行替换 `from_hex(...)`，删打印、重跑三个用例。
- **另一条**：临时把打印塞进测试时，**不要**顺手 `#include "tests/support/test_framework.h"`——
  该文件里原本就是 `#include "test_framework.h"`，多写一行会引入**不存在的路径**，报 C1083。
### 3.83 2026-10-27 10:05：黄金样本再生——**让测试自己写文件**，不要再经控制台

三次尝试都在"从控制台拿那 374 个字符"上翻车，分别是：管道截断（63 字符）、`2> file` 未捕获（0 字符）、
`Start-Process -RedirectStandardError` 取到整段 stderr 后正则抓到 1030 字符（含后续行）。
**结论：不要再从控制台搬运。**

**下次的正确做法（唯一可靠）**：在测试的 golden 用例里**直接写文件**：
```cpp
{
    std::string hex;
    for (const auto byte : encoded.value()) { /* … 同上 … */ }
    if (std::FILE* out = std::fopen("golden-hex.txt", "wb")) {
        std::fwrite(hex.data(), 1, hex.size(), out);
        std::fclose(out);
    }
}
```
然后 `Get-Content golden-hex.txt -Raw` 就是**恰好 374 个字符**，再按 64 字符分行替换 `from_hex(...)`，
删掉写文件的那段，重跑三个用例。（`#include <cstdio>` 只在临时版本里加，落盘前删掉。）

**本轮状态**：codec 改动与测试改动再次回退；构建干净、22 个用例全绿、工作树 0 改动、临时文件已删。
已知的新格式十六进制形状：`4d59524450524600 01000000 48000000 03020000 …`（长度 `bb`=187 字节，
中间含 tag 36 的 `2400 0201 01000000 01`）。
### 3.84 2026-10-27 10:10：抓到 1030 个字符（515 字节），**但数字对不上，因此不采用**

- 用「测试自己写文件」的办法终于拿到了**完整**内容：`golden-hex.txt` = **1030** 个十六进制字符
  = **515 字节**，全部是合法十六进制、只含一个 magic（`4d595244505246`）。
- **但对不上预期**：原记录 187 字节（374 字符），加一个可选标签字段（1 字节值 + 4 字节头）应当 ~195 字节。
  **515 字节多出 328 字节，我无法解释。** 用无法解释的字节去做"冻结的字节级契约"，比没有契约更糟。
- 所以：**全部回退**（codec + 测试），删除临时文件，构建干净、22 个用例全绿、工作树 0 改动。
- **下次先解释这 328 字节，再谈黄金样本**。三个具体检查：
  1. 这个 codec 里是否有**两个**编码入口（例如 `encode_profile` 与按字段循环的通用写入），
     我的插入点被两处共用 ⇒ 字段被写两次（虽然解码会拒绝重复，但编码端体积会翻倍）；
  2. `kProfileSchemaMinor` 是否也被我无意改动，导致头部长度/字段数变化；
  3. 用 `decode_profile` 对 515 字节做一次**自解码**（若失败，说明这份输出本身不是合法记录）。
- **教训**：本轮三次都卡在同一件事上——**先解释现象，再落盘**。
### 3.85 2026-10-27 10:15：328 字节之谜——**排除"写两次"**

- 实测计数：`encode_profile` 在文件里**只有一个**（297 行），`append_bytes_field(...display_name...)`
  也**只有一处**（我的标签写入就插在它后面）⇒ **不存在"两处插入、字段写两次"**。
- ⇒ §3.84 的第 1 项检查**已排除**；剩下的可能：
  1. `kLastKnownTag` 的改动**改变了头部**（例如字段数/长度写进了 header，见 §3.84 第 2 项）；
  2. 那 1030 个字符**不是一次编码的结果**（例如测试里两段代码都往同一个文件写、或 `hex` 被复用）。
- **下一步（最省的两个读数）**：
  1. 对那 515 字节调 `decode_profile`：**能解就说明是一份合法记录**，再比对其长度与"187+8"的差距；
     不能解就说明这份输出本身有问题，一切推断作废；
  2. 在写文件的那段里同时输出 `encoded.value().size()`，与文件长度对照——**两者不等就说明写文件那段有问题**。
- **方法上的教训（值得记住）**：这一整段卡在"**测量工具本身不可靠**"上，而不是产品逻辑。
  遇到这种情况，先把测量工具做到**自证**（打印 size、比对、自解码），再谈结论。
### 3.86 2026-10-27 10:20：原来自证成功——**515 字节是编码器真的产出**（不是我测错）

在临时版本里同时写出 `encoded.value().size()` 与十六进制：

```text
size=515
4d5952445052460001000…     （filelen=1040 = "size=515\n" + 1030 + "\n"）
```

⇒ **buffer 就是 515 字节**，文件与 buffer 一致 ⇒ **测量工具没问题，是我对"应当约 195 字节"的预期错了**。
但**为什么多了 328 字节，仍然没有解释**，所以黄金样本**依旧不采用**（回退、构建干净、22 用例全绿、工作树 0 改动）。

**下一步是一个单一变量的实验（一次就能定性）**：
1. **只**把 `kLastKnownTag` 从 35 提到 36（**不加任何字段写入**），构建并量 `encode_profile(minimal_record()).size()`；
2. 若 size 从 187 跳到 ~515 ⇒ **是 `kLastKnownTag` 在编码端驱动了额外输出**（例如按 tag 写占位/对齐），
   那 tag 36 的正确做法要按那个机制来，而不是"加一个 append"；
3. 若 size 仍是 187 ⇒ 增长来自我加的 append，届时再看 `append_bytes_field` 是否写入了额外内容（例如长度前缀用了 4 字节 + 对齐）。
### 3.87 2026-10-27 10:25：只改常量，size 从 187 变 **506** —— 与源码证据**矛盾**，先记矛盾

实验：**只**把 `kLastKnownTag` 从 35 提到 36（**没加任何字段写入**），构建、量 `encode_profile(minimal_record()).size()`
⇒ **506**（原 187）。

**但源码里 `kLastKnownTag` 只出现在 4 处**（19 定义、490 解码的 `known` 数组、516/535 解码的必需性检查），
**编码路径一处也没用**（`encode_profile` 297 行起，逐字段 append，直到 437 行写 header 再拼 payload）。
`kProfileKnownFieldCount` 只进 header 的 4 字节。⇒ **按源码，size 不应变化。**

**两种可能，必须分开**：
1. **测量被污染**：`probe.txt` 可能来自**另一个用例/上一次运行**（我在同一目录反复写同名文件）；
   验证方法：把 probe 写成**带随机后缀的文件名**，并在同一行写出 `encoded.value().size()` 与 `payload.size()`
   （后者需要临时在编码器里打印），两者不一致就说明我取错了对象；
2. **`kProfileKnownFieldCount` 是从 `kLastKnownTag` 派生的**，而它被用作**payload 的初始占位长度**——
   需要直接看该常量的定义（`grep kProfileKnownFieldCount`）。

**本轮已回退**：构建干净、22 用例全绿、工作树 0 改动、临时文件已删。

**方法论（第三次同一个教训）**：**一个与源码矛盾的读数，先当作测量问题处理**。
### 3.88 2026-10-27 10:30：两个常量澄清了算术，但 506 仍然对不上

```text
profile_types.h:18  inline constexpr std::uint32_t kProfileHeaderBytes     = 72;
profile_types.h:20  inline constexpr std::uint32_t kProfileKnownFieldCount = 35;   ← 独立常量，**不派生**自 kLastKnownTag
```

- ⇒ 正常记录 = **72 字节头 + 115 字节 payload = 187**（与黄金样本一致）。
- ⇒ 但"只改 `kLastKnownTag` 就变 506"**仍然无法用源码解释**：`kProfileKnownFieldCount` 是独立的 35，
  编码器不读 `kLastKnownTag`，payload 里也不该出现 36 号字段。
- **唯一还没排除的解释**：`probe.txt` 被**别的调用**写入（例如同一测试二进制里另一条用例也会编码 profile，
  或我在多次实验间复用了同名文件）。**下一次用 `probe-<GUID>.txt` 并只写 `payload.size()`**（在编码器里临时打印），
  与 `encoded.value().size()` 对照；**两者必须满足 `encoded = 72 + payload`**，否则说明我读的不是同一个对象。
- **状态**：回退后构建干净、22 用例全绿、工作树 0 改动。**卷标编解码就此暂停**，
  直到这个算术自洽——把一个 506 字节的样本冻成契约，是这一整段最不该做的事。
### 3.89 2026-10-27 10:35：**506 的解释找到了——头文件改动必须 `--clean-first`**

`kLastKnownTag`（和 `kProfileKnownFieldCount`、`kProfileHeaderBytes`）都定义在 **`profile_types.h`**，
而项目根 `AGENTS.md` 与 transfer manifest §7 item 17 都写着：
**任何头文件改动之后、测量之前，必须 `cmake --build … --clean-first`**，
否则增量构建不会重编所有包含该头文件的 TU，得到的是**假读数**（历史上表现为 `0xC0000005` 假崩溃）。

**我这一整段（3.81–3.88）所有"506/515/187"的测量都是在增量构建下取的**，因此：
- 编码器源码里**根本没有**对 `kLastKnownTag` 的依赖（3.87 已逐行确认：297–442 行是平的 append 列表）；
- 那么"只改常量、size 从 187 变 506"**只可能是陈旧对象/陈旧的测试可执行文件**造成的，
  而不是编码逻辑——这正是 AGENTS.md 警告的那类事故，只是这次表现为"尺寸变了"而不是崩溃。

**⇒ 下次做卷标编解码的唯一正确顺序**：
```powershell
# 1) 改 codec + 测试（tag 36 为必需字段 + 控制字符哨兵）
# 2) 头文件改了 ⇒ 必须干净重建
cmake --build out/build/msvc --config Release --clean-first
# 3) 再量 size，并用测试自己写文件（fopen/ofstream）。预期：72 头 + ~123 payload ≈ 195
# 4) 若 size 不是 ~195，才谈"编码器多写了什么"；若是 195，直接把该字节串写进黄金样本
```
**教训（第 4 次、也是最贵的一次）**：**头文件一改，之前的读数全部作废**。
本会话在客机上用哈希对齐解决过同类问题（旧 worker），在开发机上却忘了同一条规则。
### 3.90 2026-10-27 10:40：干净重建后仍是 515 ⇒ 增量构建**不是**原因；卷标编解码暂停，交回给人定位

- 按 3.89 的顺序做了：改 codec + 测试探针 → `cmake --build --config Release --clean-first --target myrd_m4_profile_codec_tests`
  （6 s）→ 运行 ⇒ **size=515**（hexlen=1030），与增量构建时的读数一致。
- ⇒ **3.89 的"陈旧对象"解释不成立**。当前掌握的事实：
  1. 提交态（常量 35）：`encode_profile(minimal_record())` = **187** 字节，黄金样本与它逐字节相等；
  2. **只**把常量改成 36：**506**；
  3. 常量 36 + 写一个 1 字节标签字段：**515**（差值 9 = 字段头 + 值，合理）；
  4. 而 `kLastKnownTag` 在源码里**只出现 4 次**，编码路径**没有**引用（297–442 行逐行确认）。
- ⇒ 事实 1–4 **无法同时为真**，除非有我看不到的东西（例如 `profile_types.h` 里 tag 枚举的**其他**用途、
  或某个 `static_assert`/模板实例化在 36 时命中另一条编码分支）。**这需要单步调试，不是再 grep 几行能解决的。**
- **决定（诚实优先）**：**暂停**卷标编解码，不再用不可解释的读数去改契约。留下一份**可复现的最小现场**：
  `git show 3.90 之前的提交`→ 应用 §3.81 的四处改动 → `--clean-first` → 运行 → 观察 515。
- **给接手者的两个建议**：
  1. 直接在 `encode_profile` 里打 `payload.size()`（临时代码），确认 443 还是 115——这一步没做过，
     它能把"编码器多写"与"头部变长"直接分开；
  2. 用调试器在 `kLastKnownTag` 的 4 个引用点各下一个断点，看编码时是否有隐藏引用（模板/内联）。
### 3.91 2026-10-27 10:45：矛盾的根源是**我的取数方式**——`Select-Object -First 1` 取到了别的用例

- 在 `encode_profile` 里打印 `payload.size()` 之后，我取 `Select-String payloadsize | Select-Object -First 1`
  ⇒ 得到 `payloadsize=434`。但**这个测试二进制里有多个用例都会调用 `encode_profile`**
  （golden、decode-reencode、validation、以及若干构造大记录的用例），**第一条不是 golden 那条**。
- 同理，`payload-probe.txt` 里的 `encoded=506` 是**上一次运行留下的**（我没有在每次运行前删除它），
  于是"baseline 也是 506"这个结论是**把陈旧文件当成了新读数**。
- ⇒ **3.87–3.90 里所有"506/515/187 互相矛盾"的观察，全部可以用"取错行 + 未删陈旧文件"解释**；
  **产品代码很可能一直是自洽的**。这一点必须先被证实，再谈 tag 36 的编码是否真的变了。
- **下次唯一可信的取数方式**：
  1. 每次运行前 `Remove-Item` 所有探针文件；
  2. 探针写入**唯一文件名**（带 GUID）或**追加一行带用例名的记录**（`TEST_CASE` 名 → `payload`/`encoded`）；
  3. 只读**那一行**（按用例名过滤），不要 `-First 1`。
- **状态**：回退后构建干净、22 用例全绿、工作树 0 改动、临时文件已删。
### 3.92 2026-10-27 10:55：取数方式修正后的**干净读数**——以及它证明我这一刀的写法是错的

**用干净方法取数**（每次运行前删探针、写到 exe 工作目录、`ofstream` 直接落盘、读整行）：

| 状态 | `encode_profile(minimal_record()).size()` | 能否解码 |
|---|---|---|
| 提交态（常量 35） | **187** | 是（22/22 绿） |
| **只**把常量改成 36 | **187**（编码未变） | **否**（`decoded.ok()` 失败——缺 tag 36，符合预期） |
| 常量 36 + 我在编码器末尾加一个标签字段 | **515** | **否**（新黄金 `decoded.ok()` 失败） |

**三条结论**：
1. **§3.87–3.91 的"506"全部是测量假象**——`kLastKnownTag` 对编码**没有任何影响**（源码与实测一致）；
2. 真正的变化是：**我加的字段让 payload 从 115 涨到 443（+328）**，而且**产出的字节无法解码**；
3. ⇒ **我这一刀的写法本身是错的**：+328 字节不是"一个字段"，解码失败说明字段重复/乱序/长度非法。
   最可能的原因：`append_bytes_field` 的用法与文件里其它 utf8 字段**不同**（例如它要求传入 span/长度，
   或 `ProfileFieldKind::utf8` 在**可选**字段上有额外约定）。

**下一步（具体到一个函数）**：**照抄** `manifest_name` 的写法（它是同文件里最接近的字段：`std::string` +
`std::as_bytes(std::span(...))`），而不是自己拼 `encode_utf8` 的结果；然后先只验证**解码通过**，
再谈黄金样本。

**状态**：全部回退——构建干净、22 用例全绿、工作树 0 改动、探针文件已删。
**这轮的教训**：读数错的时候，代码对也是错的；读数对的时候，才知道代码错在哪。§3.87–3.91 的六轮都花在前半句上。
### 3.93 2026-10-27 11:00：**这一轮没有产生新读数**——我的替换又没落地

- 想在编码器里打 `LABELPROBE`（标签字节数 + payload 增长），但在 `profile_codec.cpp` 里
  **`LABELPROBE` 一次都没出现**（`Select-String` 确认）⇒ 那段 `.Replace` **没有匹配上**，
  于是这一轮跑的是"只改常量"的版本，只得到 `decoded.ok()` 失败（这是**已知**结论）。
- ⇒ **又一次"以为改了、其实没改"**。本会话第 5 次同类事故（stale object、旧 worker、stale probe、
  `Select-Object -First 1`、这次是 Replace 落空）。
- **新纪律（从这一轮起强制）**：**每次修改后、构建前，必须 `Select-String` 确认标记存在**；
  没有标记就**不构建、不测量**。
  ```powershell
  if (-not (Select-String -Path $file -Pattern 'LABELPROBE' -Quiet)) { throw 'edit did not land' }
  ```
- **状态**：回退后构建干净、22 用例全绿、工作树 0 改动、探针文件已删。
- **卷标下一刀**：用 `edit` 工具（而不是 `.Replace`）落地，写完立刻 `Select-String` 验证，
  再构建——本轮之前那次 515 就是用 `edit` 打出来的，说明**工具可靠、方法差别在于是否验证**。
### 3.94 2026-10-27 11:05：`payload_before=434` ——**与 187 字节的黄金无法共存**，怀疑链接了陈旧的 profile 库

本轮探针（用 `edit` 工具落地、并用 `Select-String` 验证过）在**同一个**记录上给出：

```text
LABELPROBE label_bytes=1 payload_before=434
LABELPROBE payload_after=443        ← 只发生了一次编码（枚举后只有一组值）
```

⇒ 我的标签字段只加了 **9 字节**（443-434），**编码器本身是正常的**；
但 `payload_before=434` 意味着**不含标签时 payload 就是 434**，加 72 头 = **506**，
而**提交态的黄金是 187 字节且测试 22/22 绿**。**两者不可能同时为真**。

⇒ 最可信的解释：**测试可执行文件链接的 `myrd_profile` 库与我在改的源码不是同一份**
（陈旧静态库/另一份构建树）。这与本会话反复出现的"陈旧产物"同源，但这次发生在**链接层**。

**下一步（一条命令即可证实）**：
```powershell
# 显式重建库，再确认时间戳/哈希，然后才重建测试
cmake --build out/build/msvc --config Release --target myrd_profile
Get-ChildItem out/build/msvc/src/**/myrd_profile*  | Select-Object FullName,LastWriteTime
```
若库的时间戳早于 `profile_codec.cpp`，就是链接了旧库；`--clean-first` 全量构建后再测。

**状态**：全部回退——构建干净、22 用例全绿、工作树 0 改动、探针文件已删。
### 3.95 2026-10-27 11:10：陈旧库假设**被否定**——剩下的矛盾需要调试器，不是再读几行

时间戳实测：

```text
src/profile/profile_codec.cpp            2026/10/27 7:58:20
out/.../src/Release/myrd_profile.lib     2026/10/27 7:58:23   ← 晚于源码，是新的
out/.../tests/Release/myrd_m4_profile_codec_tests.exe  7:58:24
```

⇒ **库不是陈旧的**，§3.94 的假设**被否定**。

**当前仍然成立、但互相矛盾的两个事实**：
1. 提交态下 `myrd_m4_profile_codec_tests` **22/22 绿**，即 `encode_profile(minimal_record())`
   与 **187 字节**的 `minimal_golden()` **逐字节相等**；
2. 我在编码器末尾加探针后，同一条用例里 `payload_before=434`（⇒ 总长 506）。

**这两条只能由"探针测的不是同一个对象"来解释**，而唯一还没排除的可能：
`minimal_record()` 在**加探针的那次构建**里与提交态不同（例如我改了测试文件里的辅助函数，
或探针被插进了**另一个** `TEST_CASE`）。

**接手的正确工具是调试器**（不是 grep）：
1. 在 `encode_profile` 入口下断点，看 `record.profile.display_name` 与 `payload` 的最终大小；
2. 在 `profile_v1_minimal_profile_matches_exact_golden_bytes` 里看 `encoded.value().size()`
   —— **必须是 187**；若为 506，则说明这条用例拿到的 `minimal_record()` 与黄金样本不匹配，
   问题在**测试夹具**而不是产品代码。

**状态**：构建干净、22 用例全绿、工作树 0 改动。
**本段（3.81–3.95）共 15 轮**，全部消耗在"测量工具不可靠 + 我不断相信自己的读数"上；
产品代码**没有任何净改动**。这个结论本身值得记下来：**当读数互相矛盾时，先证明夹具，再改产品。**
### 3.96 2026-10-27 11:15：**矛盾消失了——黄金样本本来就是 506 字节，"187"是我一开始算错的**

直接数提交态 `minimal_golden()` 的十六进制：**1012 个字符 = 506 字节**，头部字段也印证：
`…48000000fa010000…`（`0x48`=72 头、`0x1fa`=506 总长）。

⇒ **一切都对上了**：
| 状态 | payload | 总长 | 结果 |
|---|---|---|---|
| 提交态 | **434** | **506** | 与黄金逐字节相等，22/22 绿 ✓ |
| 加标签字段（+9） | 443 | **515** | 与旧黄金不等（预期）✗ |
| 我的探针实测 | `payload_before=434`、`payload_after=443` | — | **完全一致** ✓ |

⇒ §3.81–3.95 那 15 轮里"互相矛盾的事实"**只有一条是错的：我坚持了"黄金=187"这个错误前提**
（来自早期把头部 `bb` 当成总长、又把 72+115 当成常态）。**产品代码一直是自洽的**，我的探针也一直是准的。

**⇒ 卷标编解码只差最后一步**：把黄金样本更新为 515 字节那条（§3.92 已经抓到过它：
`size=515 hexlen=1030`），并**同时**把 `kProfileKnownFieldCount` 与 `kLastKnownTag` 一起提到 36
（因为头部写的是"已知字段数"，现在有 36 个；只改一个很可能就是 §3.92 里 `decoded.ok()` 失败的原因）。
**`golden-hex-out.txt` 我在 3.92 里存过、后来删了**——下次重新生成即可（探针写法已验证可用）。
### 3.97 2026-10-27 11:25：卷标编解码**已打通 10/22**，卡在两条"字节手术"用例——配方完整记录

**本轮真实进展**（这些改动已回退，因为整树没绿；但配方是验证过的）：

1. `profile_types.h`：`kProfileKnownFieldCount` **35 → 36**（**必须**与 `kLastKnownTag` 同步，
   否则头部声明的已知字段数与解码要求不一致，`decoded.ok()` 失败）；
2. `profile_codec.cpp`：`kLastKnownTag` → `ProfileFieldTag::volume_label`（36）；
3. 编码器末尾（`index_headroom_percent` 之后）追加：
   ```cpp
   auto label = encode_utf8(value.volume_label.value_or(std::wstring(1, L'\u0001')), 64);
   if (!label.ok()) return Result<std::vector<std::byte>>::failure(label.error());
   append_bytes_field(payload, ProfileFieldTag::volume_label, ProfileFieldKind::utf8, label.value());
   ```
4. 解码聚合初始化末尾加一个 lambda，把 `L"\u0001"` 视为"无标签"；
5. `expected_kind_and_length()` 的 switch 里把 `volume_label` **并入 utf8 组**
   （**漏了这一步会得到 `decoded.ok()` 失败**——这是本轮踩的坑）；
6. 黄金样本换成 **515 字节**（`size=515 hexlen=1030`，本轮抓到并存过）；
7. 布局常量测试：`kProfileKnownFieldCount` 断言 35 → **36**。

**结果**：**22 例中 10 例通过**，`decoded.ok()` 在新黄金上成功 ⇒ **编解码本体已通**。
剩下的 2 条是**字节手术用例**，它们把"未知字段"用 tag **36** 表示，而 36 现在已是已知字段：
- `profile_v1_rejects_unknown_mandatory_field`：`put_u16(…, kLastFieldOffset, 36)` → 改 **37**；
- `profile_v1_accepts_bounded_unknown_optional_field`：
  - 追加字段的 tag 字节 `{36}` → **{37}**；
  - **payload 长度 445 → 454**（新 payload 443 + 11）；
  - 头部已知字段数 `put_u32(bytes, 36, 36)` **保持不变**（36 是对的）。
  改完仍失败 ⇒ 再核对这条用例追加的 11 字节布局（tag 2 + kind 1 + flags 1 + len 4 + value 3）。

**状态**：全部回退——构建干净、22 用例全绿、工作树 0 改动。
**这一段（3.81–3.97）的净产出**：一份**可复现的配方** + 一条教训（**先核对基准，再怀疑实现**）。
### 3.98 2026-10-27 11:35：配方**机械执行成功**（10/22），只剩一条"未知可选字段"用例

- 用两份脚本（`apply-label-recipe2.ps1` 已删、逻辑记在本条）**逐条带落地校验**地把 §3.97 的配方执行完：
  编译零错误；**22 例中 10 例通过**；新黄金（**515 字节**）**能被 `decode_profile` 接受**
  （golden 用例的 `encoded.value() == minimal_golden()` 通过）。
- **唯一剩下的失败**：`profile_v1_accepts_bounded_unknown_optional_field`（`decoded.ok()`）。
  这条用例手工拼一个"未知可选字段"追加到黄金末尾，涉及**三处硬编码数字**：
  总长（`put_u64(bytes,16,size+11)`，自动）、**payload 长度**（已从 445 改 454）、
  **追加字段的 tag**（已从 36 改 37）。仍然失败 ⇒ **说明我对头部偏移的理解有误**：
  offset 32 也许不是 payload 长度，或 offset 36 不是已知字段数。
- **下一步（必须先读、不要猜）**：把 `decode_profile` 里 440–470 行的头部解析（每个 `read_u16/read_u32/read_u64`
  对应的偏移）抄成一张表，再改这条用例的常量。**这条用例是唯一剩下的拦路者**，
  它挡住的不是产品功能，而是"格式演进时如何表达未知字段"的契约。
- **状态**：全部回退（脚本、探针、源码、测试）——构建干净、22 用例全绿、工作树 0 改动。
### 3.99 2026-10-27 11:45：**卷标的 profile 编解码完成，22/22 全绿**（`c04876a`）

```text
green=True pass=22
```

**最终落地内容**（配方 = §3.97 + §3.98 的补充）：
- `ProfileFieldTag::volume_label = 36`；`Profile::volume_label`（`std::optional<std::wstring>`）；
- `kLastKnownTag` 与 **`kProfileKnownFieldCount` 一起**提到 36（**必须同步**，否则头部声明的字段数与解码要求不一致）；
- 编码器**末尾**写该字段（字段按 tag 升序），无标签时写**控制字符哨兵** `\x01`（字段编码器拒绝零长度）；
- 解码器把哨兵映射回 `nullopt`；`expected_kind_and_length()` 把它并入 utf8 组（**漏了会 decode 失败**）；
- 黄金样本由**编码器自己产出**（515 字节）而非手推；探针随后移除；
- 两条"未知字段"契约用例：tag 36 → **37**、payload 长度 445 → **454**、
  **头部字段数 `put_u32(bytes,36,…)` 必须 36 → 37**（解析器按这个数走，少一个就报 trailing bytes）。

**下一步（卷标剩余部分）**：
1. 服务把 `DiskCreateCommand::volume_label` 放进 `LifecycleOperationIntent`（**字段已在**，要确认赋值处）；
2. profile 记录写入该字段（`profile_registration_store` / 记录构造处）；
3. worker 在卷挂载完成后调 `SetVolumeLabelW`（**失败不让 start 失败**，但要记日志）；
4. **然后把 `--volume-label` 加回 CLI 解析器**（`--owner` 之后）+ 用法行；
5. 客机读数：`fsutil fsinfo volumeinfo <letter>:` 的 `Volume Label` 行。
### 3.100 2026-10-27 11:55：卷标**已从创建命令贯通到 profile 记录**（两层就位）

- `service_core.cpp`：`LifecycleOperationIntent{…}` 的位置初始化末尾新增
  `create != nullptr ? create->volume_label : std::nullopt`；
- `materialize_*_profile` 里构造 `Profile` 的位置初始化末尾新增 `intent.volume_label`；
- 构建零错误；`m5.service_orchestration` **25/25**、`m5.service_host` **25/25**、
  `m4.profile_codec` **22/22**。
- **仍对用户不可达**：CLI 还不接受 `--volume-label`（守纪律：宁可没有入口，也不要静默空转）。
- **剩余两步**：
  1. **worker 在卷挂载完成后 `SetVolumeLabelW`**：标签已经在 profile 记录里，
     worker 拿到记录后可在 `assign-mount` 成功后调用；**失败不让 start 失败**，但要写日志
     （`volume label not applied: code=… label=…`）；
  2. **把 `--volume-label` 加回 `cli_parser.cpp`**（`--owner` 之后）+ 用法行；
     然后客机读数：`fsutil fsinfo volumeinfo <letter>:` 的 `Volume Label` 行。
### 3.101 2026-10-27 12:00：worker 侧落点的勘察结果（下一步照着改，不用再找）

**动作序列在 `src/management/state_machine.h:143` 的 `enum class ActionKind`**，与卷标相关的相邻成员：

```cpp
    verify_existing_volume_identity,   // 161
    assign_exact_mount,                // 162  → 字符串 "assign-mount"（service_core.cpp:330）
    readiness_io,                      // 163  → "readiness-io"
    remove_mount,                      // 164
```

**因此 worker 应用卷标有两条路，推荐第二条**：
1. **新增 `ActionKind::apply_volume_label`**：要在 5 处同步（枚举、`to_string`、动作执行分发、
   start 序列插入点、`validate_*` 对动作合法性的检查）——面广、易漏，且本会话已证明"面广的改动"在这种
   上下文里很难一次做对；
2. **并入 `assign_exact_mount` 的实现**（推荐）：挂载成功后，若 `profile.volume_label` 有值，
   直接 `SetVolumeLabelW(<盘符>:\ 或挂载路径, label)`；失败**只记日志**、不影响 start 结果。
   这样**只改一个函数**，用户可见行为就是"创建时给了标签 ⇒ 启动后卷上有标签"。

**验收（客机）**：`fsutil fsinfo volumeinfo <letter>:` 的 `Volume Label` 行必须等于请求值；
再断电重启一次，确认恢复后标签仍在（它就是卷上的元数据，应随之保留）。

**当前状态**：构建干净、工作树 0 改动、`m4.profile_codec` 22/22。
**卷标还剩**：① `assign_exact_mount` 里应用（上面第 2 条）；② CLI `--volume-label`；③ 客机读数。
### 3.102 2026-10-27 12:05：worker 应用的**真实接线位置**（勘察完，供下一次一次做完）

`SetVolumeMountPointW` 全树**只有一处调用方**：`src/run/windows_volume.cpp` 的
`Win32VolumeAdapter::assign_drive_letter(const ProductVolume&, wchar_t)`（1178/1186/1214 行）。
⇒ **worker 侧应用卷标的正确做法（不改签名、不扩散）**：
1. 在同一个类里**新增**一个方法（纯增量，不影响任何现有调用）：
   ```cpp
   Result<void> apply_volume_label(wchar_t drive_letter, const std::wstring& label) const;
   // 实现：std::wstring root{drive_letter, L':', L'\\', L'\0'};
   //       SetVolumeLabelW(root.c_str(), label.c_str()) 失败 ⇒ failure(platform_error, L"apply_volume_label", …)
   ```
2. 在**调用 `assign_drive_letter` 的地方**（worker 侧 runtime adapter 的 start 路径）紧接着调用它，
   条件：`profile.volume_label.has_value()`；**失败只记日志**，不影响 start 结果；
3. CLI `--volume-label` 最后加（此时链路才完整）；
4. 客机读数：`fsutil fsinfo volumeinfo <letter>:` 的 `Volume Label`；断电重启后仍在。

**当前状态（本轮结束时，全部有读数为证）**：

| 套件 | 结果 |
|---|---|
| `m4.profile_codec` | **22/22** |
| `m5.service_orchestration` | **25/25** |
| `m5.service_host` | **25/25** |
| `m5.cli` | **17/17** |

构建零错误、工作树 0 改动。
### 3.103 2026-10-27 12:15：`apply_volume_label` 就位（无调用方），**剩下的是它前面那条数据通路**

- `src/run/windows_volume.{h,cpp}`：新增 `Win32VolumeAdapter::apply_volume_label(wchar_t, const std::wstring&)`
  —— 与 `assign_drive_letter` **并列**，不改任何既有签名；标签长度 1–32 在这里（Windows 之前的最后一道）**再校验一次**；
  `m4.windows_volume` **19/19**、`m4.profile_codec` **22/22**。
- **为什么还没被调用**：worker 侧的挂载入口是
  `ProductionRuntimeAdapter::assign(const ProductVolume&, const wchar_t letter)`（`runtime_adapter.cpp:569`），
  经 `disk_runtime.cpp:199` 的 `options_.drive_letter` 调用——**这条路径上没有标签**。
  要把标签送到这里，必须先让 `DiskRuntimeOptions`/adapter 的 `assign` 带上它（**这是接口改动，会扩散**），
  而我不能在半懂的状态下动一个 M4 的公共接口。
- **T4-02 卷标剩余（剩下的都是这一条通路）**：
  1. `DiskRuntimeOptions` 增 `std::optional<std::wstring> volume_label`；worker 构造 options 时从 profile 记录填入；
  2. `ProductionRuntimeAdapter::assign` 在 `assign_drive_letter` 成功后，若 `volume_label` 有值则调
     `apply_volume_label(letter, *label)`，**失败只记日志**（`volume label not applied: …`）；
  3. CLI `--volume-label`（`cli_parser.cpp`，`--owner` 之后）+ 用法行；
  4. 客机读数：`fsutil fsinfo volumeinfo <letter>:` 的 `Volume Label`；断电重启后仍在。
- **本轮的现场**：构建零错误、工作树干净、四套件（windows_volume 19、profile_codec 22、
  service_orchestration 25、service_host 25）全绿。
### 3.104 2026-10-27 12:25：卷标**执行链路已通到适配器**（只差"谁把标签填进 options"）

- `IRuntimeAdapter::apply_label(wchar_t, const std::wstring&)`：**非纯虚 + 默认成功**（这样测试里的 fake 不用改，
  不能标记的平台也只是"不标记"）；`ProductionRuntimeAdapter` 覆盖它，转调
  `Win32VolumeAdapter::apply_volume_label`。
- `DiskRuntimeOperations::apply_volume_label` 在 `runtime_adapter.cpp` 里接到 `volumes.apply_label(...)`。
- `DiskRuntimeOptions::volume_label`（末尾追加）+ `disk_runtime.cpp` 在**挂载成功后**调用，
  **失败即丢标签、不影响 start**。
- 干净重建（`--clean-first`）+ 三个套件：`runtime_adapter` **6/6**、`disk_runtime` **33/33**、`windows_volume` **19/19**。
- **T4-02 卷标只剩两步**：
  1. **worker 把 profile 记录的标签填进 `DiskRuntimeOptions`**（构造 options 的地方）；
  2. **CLI `--volume-label`**（`cli_parser.cpp`，`--owner` 之后）+ 用法行；
  3. 客机读数：`fsutil fsinfo volumeinfo <letter>:`；断电重启后标签仍在。
### 3.105 2026-10-27 12:30：卷标**最后两步的确切位置**（worker 启动规格是第三层线格式）

**数据通路（三层，前两层已完成）**：
1. **命令层**（✔ 完成）：`DiskCreateCommand::volume_label` → IPC 字段 11 → `LifecycleOperationIntent` → `Profile::volume_label`；
2. **运行时层**（✔ 完成）：`DiskRuntimeOptions::volume_label` → `DiskRuntimeOperations::apply_volume_label`
   → `ProductionRuntimeAdapter::apply_label` → `Win32VolumeAdapter::apply_volume_label`
   （`disk_runtime.cpp` 在挂载成功后调用，失败即丢标签）；
3. **worker 启动层（✘ 未做）**：
   - `src/host/m5_worker_main.cpp:175-185` 从 `spec.value()`（`WorkerSpec`，即 worker 启动命令的**解码结果**）
     填 `config.*`；`config.drive_letter` 就是这么来的；
   - ⇒ 需要：`WorkerSpec` 加字段 + worker 命令编解码（与 IPC 字段同样做法：**追加 tag，不需要协议次版本号**）
     + 服务端启动 worker 时填值（从 profile 记录）+ `config.volume_label` + `options.volume_label = impl.config.volume_label`；
4. **CLI**（✘ 未做）：`cli_parser.cpp` 的 `--volume-label`（`--owner` 之后）+ 用法行。

**为什么 3 和 4 必须一起做**：只加 CLI 而没有 3，就会"接受参数、写进 profile、卷上什么都没有"——
正是本会话一直拒绝的静默空转。

**状态**：构建干净、工作树干净；`runtime_adapter 6/6`、`disk_runtime 33/33`、`windows_volume 19/19`、
`profile_codec 22/22`、`service_orchestration 25/25`、`service_host 25/25`、`cli 17/17`。
### 3.106 2026-10-27 12:40：worker 启动规格与 worker 配置**都能带标签**（尚无人读）

- `src/host/m5_worker_private_pipe.h`：`WorkerLaunchSpec` 末尾新增 `std::optional<std::wstring> volume_label;`
  （声明在最后 ⇒ 既有字段的编码顺序不变）。
- `src/host/m5_worker_runtime.h`：`DiskWorkerConfig` 同样末尾新增 `volume_label`。
- **干净重建 451/451**；`m5.worker_protocol` **39/39**、`m5.worker_session` **25/25**、`m5.service_host` **25/25**。
- **剩余三步（都在同一条链上）**：
  1. **worker 命令编解码**：`WorkerLaunchSpec` 的写入/读取各加一个**追加 tag**（客户机与 worker 一起发版，
     **不需要协议次版本号**）；对应套件 `m5.worker_protocol` 加一条往返用例；
  2. **服务端填值**：启动 worker 时从 profile 记录取 `volume_label` 放进 spec；
  3. **worker 侧转交**：`m5_worker_main.cpp` 把 `spec.volume_label` 放进 `config.volume_label`，
     `m5_worker_runtime.cpp` 再放进 `options.volume_label`（这一行就是 §3.105 第 2 层已在等的东西）；
  4. 最后 **CLI `--volume-label`** + 客机 `fsutil fsinfo volumeinfo` 读数。
### 3.107 2026-10-27 12:50：**卷标纵切完成**——`--volume-label` 已是真实选项

**三条路径实测（用构建出来的 `myrd-cli.exe`）**：
```text
--volume-label MyDisk        → 走到了服务（本机无服务 ⇒ "the service is not available"，正确的下一个错误）
--volume-label <33 个字符>    → myrd-cli: --volume-label must be 1 to 32 characters
--volume-label "a<TAB>b"     → myrd-cli: --volume-label must not contain control characters
```

**踩到的坑（记下来）**：`create` 分支里已经在解析这个选项了，但客户端仍报 `unknown option`——
因为**还有一张"合法选项白名单"**（`cli_parser.cpp` 的 `value_options`）。**解析代码与白名单是两处，必须同时改。**
这类"两处必须同步"的坑在这个项目里已出现多次（`kProfileKnownFieldCount`/`kLastKnownTag` 也是）。

**卷标纵切最终形态（全部有读数）**：
命令字段 → IPC 字段 11（边界校验）→ `LifecycleOperationIntent`（含相等性）→ `Profile` tag 36（22/22）→
worker 启动请求 → 启动规格 → **私有管道追加字段**（旧写入方解码为"无标签"）→ worker 配置 →
`DiskRuntimeOptions` → 挂载成功后 `SetVolumeLabelW`（失败只记日志）→ **CLI `--volume-label`**

**唯一未做的验证**：客机读数 `fsutil fsinfo volumeinfo <letter>:` + 断电重启后标签仍在。
`m5.cli` **17/17**、`m1.cli` **5/5**、`worker_protocol` **39/39**、`service_*` 25/25 各。
### 3.108 2026-10-27 13:00：卷标**已验证到"服务接受"**，但 `cli_e2e` 在策略盘上卡住 300 s（已知失败）

**本轮修掉的第三个"两处必须同步"**：`kCreateFields` 是**定长数组**（`std::array<FieldDescriptor, 10>`），
我先把 schema 的**计数**改成 11、又补了第 11 行，但**数组长度**没改 ⇒ `error C2078: 初始值设定项太多`。
已修（`510c164`）；**前面那次带红提交是我的错**，在下一条提交里修正并写明。

**客机读数（请求 `94dbab6a…`）**：
- 服务**接受了**带 `--volume-label` 的 create（`policy create: disk.create: operation queued`）；
- 但该操作**300 s 未达终态**（`last state: running`）⇒ `cli_e2e` 失败。

**首要嫌疑：卷标写入本身**。它就跑在挂载成功之后（`SetVolumeLabelW`），而此刻卷可能仍被我们自己的
flush/lock 持有 ⇒ 可能阻塞。**次要可能**：策略盘这一段本来就要 30 s 检查点 + 64 MiB 初始化，叠加后超时。

**下一步（二分，两次运行就能定性）**：
1. **去掉** `cli_e2e` 里的 `--volume-label`（只留断言前的创建），重跑：
   - 通过 ⇒ 问题在**卷标写入**（回到第 2 步）；
   - 仍卡 ⇒ 与卷标无关（是策略盘这一段本身变慢/卡住）。
2. 若确认是卷标：在 `apply_volume_label` 前后各打一行日志（`before/after SetVolumeLabelW`），
   看它是否真的阻塞；若是，考虑把标签写入放到 **readiness-io 之后**、或对已挂载路径用
   `SetVolumeLabelW` 前先确认卷不在 lock 中。

**状态**：`m1.cli 5/5`、`m5.cli 17/17`、`m5.service_orchestration 25/25`；
**`integration.m5.cli_e2e` 当前为已知失败**（不得读成"全绿"）。
### 3.109 2026-10-27 13:10：**二分定性完成——卷标写入就是那个 300 s 卡住的原因**

| 运行 | create 是否完成 | 用时 | 结论 |
|---|---|---|---|
| 带 `--volume-label` | **否**（300 s 未达终态） | 336 s | 卡在卷标路径 |
| **去掉** `--volume-label` | **是**（随后被我新加的 `fsutil` 断言判失败） | 33 s | 创建本身很快 |

⇒ **不是"这一段本来就慢"**，而是**卷标写入把创建卡住了**。最可能的原因：`SetVolumeLabelW` 在
**卷仍被我们自己独占持有**（lock/flush 句柄）时阻塞——这是 Windows 的常见行为，标签写入需要卷可写且不被独占。

**修复方向（按成本）**：
1. **把标签写入挪到"卷不再被独占"的时刻**：不是紧跟挂载，而是放到 `readiness-io` 之后、
   或在释放独占句柄之后的步骤里；
2. 若必须紧跟挂载，则**先在写入前释放/降级独占**（例如用共享读写打开卷根再 `SetVolumeLabelW`）；
3. **兜底**：给标签写入加**有界等待**（例如 5 s 不返回就放弃并记日志）——用户可见语义是"标签没写上去"，
   而不是"创建永远不结束"。**这一条无论 1/2 是否成功都应该做**，因为一个第三方阻塞不该让创建无限挂起。

**当前状态**：`cli_e2e` 仍为**已知失败**（`fsutil` 断言在，卷标未落盘）；构建与本机套件绿。
### 3.110 2026-10-27 13:25：有界等待**没有解决**——说明卡的不是"标签调用本身"，而是"卷被卡住"

- 改动：`apply_volume_label` 把 `SetVolumeLabelW` 放到**独立线程 + 5 秒上限**（线程 detach、自己持有参数副本，
  避免返回后读栈），超时返回失败。`windows_volume` **19/19**、`runtime_adapter` **6/6**。
- 复跑（请求 `5a256272…`）：**仍然 332 s 超时**。
- **推论**：如果 `SetVolumeLabelW` 自己不返回，5 秒上限会触发、create 会"丢标签"但**完成**；
  现在它没完成 ⇒ **卷在整个后续流程里被阻塞**（写标签把卷/过滤器卡住，后续 flush/unmount 全排队）。
- ⇒ **正确的修复是"什么时候写"，不是"等多久"**：
  1. **把标签写入移出设备流水线**：例如 start 操作**终态之后**再写（此时设备/卷已就绪、独占已释放），
     或作为一个**独立的后续动作**（`readiness-io` 之后、`remove_mount` 之前，或干脆下一次 start 时补写）；
  2. **不要在 `assign_drive_letter` 之后立即写**——这正是当前实现，也是被二分证明有问题的那一步；
  3. 保留 5 秒上限（它是对的：让"写不上去"变成日志而不是挂起），但**不要指望它拯救时序问题**。
- **当前状态**：`cli_e2e` 仍为**已知失败**；本机套件绿；工作树干净。
### 3.111 2026-10-27 13:35：**撤回 `--volume-label` 入口，基线恢复全绿**（卷标代码保留但休眠）

- 理由（一句话）：**提供"会让创建挂起的选项"比不提供更糟**。卷标链路已全部建好（命令字段 → IPC → intent →
  profile → worker spec → 编解码 → 运行时 → 适配器 → 有界写入），**但写入时机有问题**，所以**先关掉用户入口**。
- 实测恢复：**`cli_e2e` 1 of 1 passed，87 s**（请求 `77c53791…`）；`m1.cli` 5/5、`m5.cli` 17/17。
- **保留休眠的部分**：`DiskCreateCommand::volume_label`、IPC 字段 11、intent 字段、`Profile` tag 36 与其编解码、
  worker spec/config 字段、`DiskRuntimeOptions::volume_label`、`apply_volume_label`（含 5 s 上限）。
  **全部不参与用户可见行为**，所以不会造成静默空转。
- **卷标剩余的唯一工作**：**把写入移到设备流水线之外**（start 终态之后，或下一次启动时补写），
  然后把 CLI 选项与 e2e 断言一起加回（e2e 已有现成写法，见 §3.108）。
### 3.112 2026-10-27 13:50：**结论：卷标在当前 AIM 卷上做不到——`SetVolumeLabelW` 永不返回**

**三次实验，把可能性逐一排除**：

| 实验 | 结果 | 排除掉什么 |
|---|---|---|
| 写标签紧跟挂载 | create 300 s 未达终态（336 s） | —— |
| **去掉** `--volume-label` | create **33 s** 完成 | "这一段本来就慢" |
| 写标签 + **5 s 有界等待** | 仍 332 s 未完成 | "是调用卡住、加超时就好" |
| 写标签**移到 start() 之后** | 仍 342 s 未完成 | "是写早了挡住流水线" |

⇒ **`SetVolumeLabelW` 对这个 AIM 代理卷永不返回**，而卡住的线程**继续占着卷**，
因此**后续流水线（flush/unmount）也跟着挂住**——这解释了为什么"挪位置"和"加超时"都无效。
这是**驱动层行为**（与本会话记录的"候选盘开盘阻塞 120 s""设备退役窗口阻塞 11 s"同族）。

**产品决定（已执行，`ac55de0`）**：**撤回 `--volume-label` 入口**，e2e 恢复原状，基线全绿。
理由：**宁可"卷标暂不支持"，也不要"创建会挂五分钟"**。所有卷标代码保留休眠（命令字段/IPC/intent/profile 编解码/
worker spec/运行时/适配器），未来若驱动支持或改用别的接口，只需重新接上入口。

**若要把卷标做完，需要换一条路（记录，未做）**：
1. **不在 worker 进程里写**：由服务或 CLI **另起一个短命进程**调用 `SetVolumeLabelW`（卡住也只卡那个进程）；
2. **改用 `FSCTL_SET_VOLUME_LABEL` 直接下发**（绕过 `SetVolumeLabelW` 对卷的额外要求）；
3. **向 AIM 驱动确认是否支持卷标**——若驱动本身不支持，这一项应当在 T4-02 里**记为不支持**，
   而不是继续在应用层兜圈子。

**读数**：`m1.cli 5/5`、`m5.cli 17/17`、`disk_runtime 33/33`、`worker_session 25/25`；
`cli_e2e` 上次在撤回状态下 **1 of 1 passed，87 s**（请求 `77c53791…`）。
### 3.113 2026-10-27 14:05：**客机复跑出现两个异常，未解释——下一轮必须先查这个**

| 组 | 上次（卷标工作前） | 本次 | 差异 |
|---|---|---|---|
| `^unit\.` | 96/96，**227 s** | 96/96，**517 s** | 全对但**慢 2.3 倍** |
| `^model\.|^m5\.` | 13/13（曾 12/13 间发） | **11/13** | `m5.production_execution_backend`（已知间发）+ **`m5.startup_sweep`（新失败）** |

- **`m5.startup_sweep` 是本轮新出现的失败**，而这一轮我改过：`DiskWorkerConfig`/`WorkerLaunchSpec` 加字段、
  worker 私有管道编解码追加字段、`disk_runtime` 移除流水线内写标签、worker 在 `start()` 之后应用标签。
  **其中最可疑的是 worker 启动规格的字段追加**（`startup_sweep` 正是启动/接管相关的用例）。
- 本地跑 `myrd_m5_startup_sweep_tests.exe` **没有任何输出**（`fail=0 pass=0`）——说明这个可执行文件
  在本机要么需要参数、要么直接退出。**不能据此判断绿。**
- **采集失败**：那次运行没有留下 `integration.m5.startup_sweep.stdout.txt`（scp 报文件不存在），
  所以**失败原因目前未知**。
- **下一轮第一件事**（按成本）：
  1. 单跑 `-Filter '^integration\.m5\.startup_sweep$'`（现在能稳定复现），拿到它的 stdout/stderr；
  2. 若与启动规格字段有关：**临时把 `volume_label` 字段从 `WorkerLaunchSpec` 与编解码中移除**，
     重跑该用例——若恢复，则确认是"追加字段改变了 worker 启动语义"（这比性能问题严重得多）；
  3 **性能回归（227 → 517 s）单独查**：最可能是我的有界写入线程或 worker 启动路径引入了等待，
     但也不排除客机自身状态（本轮之前刚跑过多次 e2e）。
### 3.114 2026-10-27 14:15：`m5.startup_sweep` 的真相——**它自己拒绝在它认为的"非客机"上运行**

- 单独跑（`-Filter '^m5\.startup_sweep$'`）：**1.9 s，exit 1，stdout/stderr 都是 0 字节**。
- 直接在客机上跑该 exe（普通 ssh，非 agent）：
  ```text
  m5.startup_sweep creates real worker processes and may only run on the guest (AGENTS.md section 4).
  Use tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^m5\.startup_sweep$' rather than ctest on the development machine.
  exit=2
  ```
- ⇒ 这个用例**有一道"只在客机运行"的自检**，而**我的普通 ssh 会话不满足它的判定**（exit 2 + 明确提示）；
  但**通过 agent 跑时 exit 1、无任何输出**——**两种行为不同**，说明判定逻辑对"agent 会话"和"普通 ssh 会话"
  （以及可能的开发机）给出三种不同结果，而其中一种**默默失败且不留痕迹**。
- **下一步（两件小事）**：
  1. 找到这道自检的实现（grep `may only run on the guest`），看它依据什么判定
     （环境变量？标记文件？是否存在 worker 二进制？），并把"agent 会话"这一路补上——
     否则这条用例在客机上**永远静默失败**；
  2. 让它在拒绝时**把原因写进 stdout/stderr**（现在 agent 路径上是 0 字节）。
- **这条与我的改动无关**：它是**既有缺陷**（本会话更早的 `model/m5 12/13` 只出现过
  `m5.production_execution_backend`，说明 `startup_sweep` 那时是过的，或当时也静默失败过而未被注意）。
  **下次要先确认"它曾经绿过吗"**，再决定是否算回归。
### 3.115 2026-10-27 14:25：`m5.startup_sweep` 失败的真因——**数据目录不存在**（环境前提，不是回归）

用 `MYRD_M5VM_GUEST=1` 手动跑（这正好复现了 agent 那条路径）：

```text
DIAG store open failed op=profile_store.open_directory native=2
     msg=Failed to open a required profile directory
exit=1
```

- ⇒ **不是断言失败、也不是我的改动**：这个用例**要求 `C:\ProgramData\MyRamDisk\profiles` 存在**，
  而 `Reset-M5Guest.ps1` 清空内容时**不会重建被删掉的目录**（它只删文件）。
  本会话更早 `model/m5` 记 12/13 时之所以没这条红，是因为那时客机上**装着服务、目录在**。
- **为什么不带标记时 exit 2**：用例开头有 `MYRD_M5VM_GUEST` 自检（`m5_startup_sweep_tests.cpp:96`），
  拒绝时**会打印原因并返回 2**；带标记后才真正跑，然后因目录缺失失败（1）。
  ⇒ **"静默失败"是我的误读**：agent 路径下输出写进了 run 目录的 stdout，但我那次 scp 拿的是**别的文件名**，
  所以看起来是 0 字节。
- **正确的环境前提（写进流程）**：跑这类"需要已安装状态"的用例之前，要么**先跑安装脚本**，
  要么让 `Reset-M5Guest` 在清空后**重建目录**（`profiles`/`operations`，DACL 由安装脚本负责）。
  建议后者：`Reset-M5Guest.ps1` 加 `New-Item -ItemType Directory -Force` 两步，
  这样"重置后立刻跑测试"才成立。
- **修正 3.113 的结论**：`m5.startup_sweep` 的红**不是新回归**，是**重置脚本与用例前提不匹配**；
  `unit` 从 227 s 变 517 s 的那条**仍未解释**（单独跑 `^unit\.` 时也慢，可能与客机当时状态有关）。
### 3.116 2026-10-27 14:35：**`m5.startup_sweep` 仍未通过，且目录已重建、安装脚本也跑过——下一轮从这里接**

- 改动：`Reset-M5Guest.ps1` 现在**清空后重建** `profiles`/`operations`（`3.x` 的原因），实测 `operations: 0 entries left`。
- 重跑：仍 **exit 1，1.9 s，无捕获输出**；随后**在客机上跑了 `Install-MyRamDisk.ps1`**（`profiles` 存在），
  **仍然 exit 1**。
- **已知**：手动带 `MYRD_M5VM_GUEST=1` 跑时，**唯一的诊断行**是
  `DIAG store open failed op=profile_store.open_directory native=2`（目录不存在）。
  现在目录存在却仍失败 ⇒ **下一层原因未取到**（可能是 **DACL**：profile store 要求"受保护、仅管理组"的 ACL，
  而重建/安装后的 ACL 与记录期不同；T4-01 已记过 store 会拒绝非管理组 ACE 与非受保护 DACL）。
- **下一轮第一步（很便宜）**：带标记手动跑一次并**保留完整输出**（不要 `Select-Object -First`）：
  ```powershell
  $env:MYRD_M5VM_GUEST='1'
  & '...\myrd_m5_startup_sweep_tests.exe' 2>&1 | Tee-Object C:\m5\ss.log
  ```
  然后看 `DIAG` 行的 `op=` 是否变成 `profile_store.*` 的其它检查（例如 ACL/所有权），据此定位。
- **不影响产品判断**：这是**测试前置条件**问题，与 M5 功能无关；但它挡住 `model/m5` 组的全绿，
  所以 T4-04 基线必须先把这条解决或**明确标注为例外**。
### 3.117 2026-10-27 14:45：`m5.startup_sweep` 的根因**确认是 DACL**（`A profile directory is not protected`）

补上 `profiles\workers`（`WorkerBindingStore::over_protected_root` 要 `MyRamDisk\profiles\workers`）之后，
诊断行**变了**，这正好证明前一层已解决、露出下一层：

```text
DIAG store open failed op=profile_store.directory_acl native=0
     msg=A profile directory is not protected
```

⇒ **profile store 要求"受保护（protected）的 DACL"**——正是 T4-01 记录过的那三条规则之一。
`Reset-M5Guest` 用 `New-Item` 建的目录只有**继承 ACL**，所以被拒。

**正确的修法（下次一次做完）**：让 `Reset-M5Guest.ps1` 在重建目录后**应用与安装脚本相同的 ACL**
（`tools/install/Install-MyRamDisk.ps1` 里的 `Set-ManagementAcl`：`SetAccessRuleProtection($true,$false)`
+ 仅管理组/系统 ACE）。两种实现都可以：
1. **复用**：把安装脚本里的 ACL 函数抽到 `tools/install/M5Acl.ps1`，两个脚本都 dot-source 它（推荐，单一事实来源）；
2. **内联**：在重置脚本里重复那几行（快，但会漂移——本会话已因"两处必须同步"踩过三次）。

**为什么值得修**：这条挡住 `model/m5` 组全绿，而 T4-04 的基线需要它；而且"重置后立刻跑测试"是**这个 harness
最重要的一条纪律**，它必须真的成立。

**当前状态**：`profiles`/`operations`/`profiles\workers` 都会在重置后重建；**ACL 未设**，所以 `m5.startup_sweep` 仍红。
其余套件与 `cli_e2e`（87 s）不受影响。
### 3.118 2026-10-27 14:55：**`m5.startup_sweep` 通过**——重置脚本现在会保护它重建的目录

```text
[1/1] m5.startup_sweep
1 of 1 passed            （请求 a874a9ca…）
```

**三层原因，逐层量出来**：
1. `profiles\workers` 目录不存在 ⇒ `profile_store.open_directory native=2`；
2. 目录建了但 **ACL 是继承的** ⇒ `profile_store.directory_acl`：`A profile directory is not protected`；
3. 修法正是 T4-01 记的第三条规则：**去掉继承 + 只给 Administrators/SYSTEM 完全控制**（`SetAccessRuleProtection($true,$false)`）。

`Reset-M5Guest.ps1` 现在会 `protected` 四个目录（数据根、`profiles`、`operations`、`profiles\workers`），实测输出四行 `protected …`。

**意义**：这条不只是修一个用例——"**重置客机 → 立刻跑测试**"是这个 harness 最重要的一条纪律，
而一个留下"产品会拒绝的目录"的重置脚本，会让测试**因为不是产品的原因而失败**。

**`model/m5` 组现在应为 13/13**（需一次整组复跑确认，见下）。
### 3.119 2026-10-27 15:00：**`model/m5` 回到 13/13**（含 `m5.startup_sweep` 与 `production_execution_backend`）

```text
13 of 13 passed     请求 b37c8262…，114 s
```

- 这一组在两个半小时内出现过 **13/13 → 12/13 → 11/13 → 13/13** 的波动，**每一次的归因现在都能对上**：
  1. 12/13：`m5.production_execution_backend` 是**已知批内间发**（单独跑通过）；
  2. 11/13：额外的 `m5.startup_sweep` 是**环境假信号**——重置脚本没重建目录、建了也没保护 ACL；
  3. 13/13：`Reset-M5Guest.ps1` 现在建目录 + 申请保护 DACL（§3.118）。
- **⇒ 一个可复用的经验**：客机上的红，**先问"这台机器的前提成立吗"**（目录/ACL/服务状态/残留设备），
  再问产品。本会话在这条上花了很长时间，但最终每次都是前提问题。
- **客机基线（当前）**：
  | 组 | 结果 |
  |---|---|
  | `^unit\.` | 96/96（用时长波动 227→517 s，**仍未解释**） |
  | `^model\.|^m5\.` | **13/13**（114 s） |
  | `integration.m5.cli_e2e` | **PASS**（87 s，撤回卷标入口后） |
  | `integration.m5.install_uninstall` / `gui_workflow` | 上次各 PASS（9 s / 25 s） |
### 3.120 2026-10-27 15:10：`unit` 的 517 s **不复现**——安静客机上 275 s（结论：先前那次是瞬时负载）

```text
96 of 96 passed      请求 8c285128…，275 s
```

- 三次读数：**227 s**（最早，安静客机）→ **517 s**（§3.113，刚跑过多次 e2e 与断电流程）→ **275 s**（现在）。
- ⇒ **不是回归**：517 s 那次客机刚经历连续 e2e + 断电 + 多次安装，磁盘与设备栈处于"脏"状态。
  **可用的口径：`unit` 在安静客机上约 3–5 分钟**，超过 6 分钟再怀疑。
- **至此 §3.113 的两个异常都收口**：`m5.startup_sweep` = 环境假信号（已修，§3.118）；
  `unit` 变慢 = 瞬时负载（不复现）。

**客机基线（最终版，全部为安静客机读数）**：

| 组 | 结果 | 用时 |
|---|---|---|
| `^unit\.` | **96/96** | 275 s |
| `^model\.|^m5\.` | **13/13** | 114 s |
| `integration.m5.cli_e2e` | **PASS** | 87 s |
| `integration.m5.install_uninstall` | PASS | 9 s |
| `integration.m5.gui_workflow` | PASS | 25 s |
| `integration.m1/m2/m3/m4` | 1/1、4/4、31/31、3/3 | 5/19/97/9 s |
| `structural` | 1/3（两条 `vswhere` 环境） | 5 s |
### 3.121 2026-10-27 15:25：**Load Only 与易失盘的语义裁定**（设计文档只列了名字，没定义——这一条必须由实现方定）

**勘察结果**：`docs/.../m5-service-cli-gui-design-v3.md` 里这两项**只出现在任务表**（第 501 行），**没有定义**。
`disk import` 的解析器**不接受 `--no-start`**（`create` 接受）。因此裁定如下，按最小改动实现：

1. **Load Only = `disk import --no-start`**
   - `DiskImportCommand` 加 `bool no_start{}`；解析器加 `--no-start`；
   - 服务侧：`import` 的 intent `auto_start = !no_start`（`create` 已是这个形状，照抄）；
   - 语义：**只登记/校验 bundle 并发布 profile，不创建运行时、不碰设备**；
   - 验收：`disk import --no-start` 后 `disk list` 显示 `Ready stopped/configured`，且**没有 worker 进程**。

2. **易失盘模式 = 创建时禁用检查点（"数据只在内存/仅靠最终落盘"）**
   - 现状：`create` 已有 `--checkpoint-interval`（可禁用？需确认 `CheckpointInterval{enabled=false}` 的解析入口）
     与 `--accept-volatile-data-loss`（停止时的确认）；
   - 语义：**不做周期性 checkpoint**，断电即丢到最后一次手工 `disk checkpoint`；
   - 若 `--checkpoint-interval` 已能表达"禁用"，则这一项**只差一个显式别名**（如 `--volatile`）
     与文档/测试；**不要新增一套持久化机制**。
   - 验收：创建后写入数据、硬断电、恢复应回到**上一次手工 checkpoint** 的 generation（而不是最新数据），
     且 `disk stop` 必须要求 `--accept-volatile-data-loss`。

3. **VHDX 导出 = 新 `disk export` 动词**（writer 已有，见 §3.120 之后的 T4-02 行）：把当前 bundle 通过
   `persistence/image_creator` 落成一个单文件 VHDX；验收：导出文件能被 `VhdxReader` 读回且 extent 一致。
### 3.122 2026-10-27 15:40：**"Load Only" 其实已经存在**——`disk import` 从不自动启动；我加的两层已回退

- 读 `LifecycleOperationIntent` 的构造处：`auto_start` 的表达式是
  `create != nullptr && !create->no_start` ⇒ **对 `import` 恒为 `false`**。
  也就是说 **`disk import` 已经只登记/校验并发布 profile、不启动运行时、不碰设备**——这正是 §3.121 给
  "Load Only" 定的语义。
- ⇒ 我本轮加的两层（`DiskImportCommand::no_start` 字段 + 线格式字段 5）**是多余的**，
  已用 `git revert` 回退（`5d3bc31`、`54c903f`），构建零错误、三套件绿。
- **这条对 T4-02 的意义**：**"Load Only" 不需要新代码**，需要的是**确认与记录**：
  1. 客机实测：`disk import` 一个 bundle → `disk list` 显示 `Ready stopped/configured`、
     **无 `myrd-m5-worker` 进程**（一次读数即可，尚未做）；
  2. 文档/用法里把这个行为写清楚（现在 `disk import` 的帮助行没有提到"不会启动"）。
- **教训（写给自己）**：**先读实现，再定"要做什么"**。我上一轮把语义定对了（§3.121），
  却没有再往前读一步去确认它是否已经实现——于是白写两层、再回退两层。
  今天已经因为"没先读"付出过黄金样本、`kLastKnownTag`、schema 计数三次代价。
### 3.123 2026-10-27 15:55：Load Only 的**第二重证据**——import 走的是 `import_profile` 计划，而不是"auto_start=false 的 create"

`service_core.cpp` 的命令分派（1810 行附近）**无条件**为 import 选计划：

```cpp
const auto management_command = create != nullptr
    ? (create->no_start ? ManagementCommand::create_stopped
                        : ManagementCommand::create_and_start)
    : ManagementCommand::import_profile;      // ← import 永远走这条
```

⇒ **"import 不会启动"不是靠某个布尔值为假的副作用**，而是它**根本不进入 create 的计划分支**。
接线处（`plan_lifecycle_reservation`，275 行）也是同一个形状：`LifecycleOperationKind::import` 直接映射到
`import_profile`，`auto_start` 只影响 create 走 `create_and_start` 还是 `create_stopped`。

**结论（可以写进验收材料）**：
- **Load Only = `disk import`**：语义由**计划选择**保证，不依赖任何运行时开关；
- **create 侧的同族能力 = `disk create --no-start`**（`create_stopped` 计划），同样是一等公民；
- 两者都**已实现**，都**缺一次客机读数**（`disk list` 的 `stopped/configured` + 无 worker 进程）。
### 3.124 2026-10-27 16:10：**VHDX 导出不是"薄薄一层命令"**——fold writer 的输出位置目前写死在 bundle 里

读到 `persistence/compaction_coordinator.h` 的 `CompactionArtifactBuildRequest`，字段是：

```cpp
BundleWriterLock* writer_lock;  ManifestSlot active_slot;
std::string base_temporary_name;  std::string base_relative_name;
VhdxWriterIdentity base_identity;  …  CompactionArtifactBuildMetrics
```

⇒ 它把合并后的 base **按"相对名字写进 bundle"**，并要求持有 **`BundleWriterLock`**。
而 `image_creator.h` 的 `create_generation_zero_image` 是**创建空镜像**，不是导出已有内容。

**因此 `disk export` 的真实工作量**（修正 T4-02 行里"只缺命令"的乐观说法）：
1. **把 fold writer 的输出位置参数化**（bundle 内相对名 → 调用方给的绝对路径），或
2. **在 bundle 内构建 + 复制出去 + 丢弃**——但那样会**改动 bundle**（等于做了一次 compaction），
   **导出不该有副作用**，所以这条不可取；
3. 加上命令层：`disk export <disk> --output <file.vhdx>` → 命令结构 + 线格式（**四处一致**：
   数组长度/字段行/schema 计数/编解码）→ 解析器 → 服务 → worker。

**验收（定稿）**：导出文件能被 `VhdxReader` 打开、`VhdxWritePlan` 的几何与源一致、
**导出过程不改动 bundle**（导出前后 `disk show` 的 generation 不变），且目标文件已存在时**拒绝覆盖**。

**这一条的价值**：它把"差一个命令"改成"**差一次 fold writer 的参数化 + 一条命令链**"——
下一个会话按这个开始，就不会像我今天这样先写两层再回退。
### 3.125 2026-10-27 16:20：T4-03 的"可观测"具体缺什么——CLI 现在**不显示** journal/compaction 指标

- `cli_renderer.cpp` 的 profile 行只渲染：状态、`health`、`generation`、`serial` 等（62–63 行附近），
  **没有任何 journal 占用或 compaction 指标**。
- 而数据来源并不缺：`WorkerRuntimeObservation`（我在卷序列号那一刀扩过）与 `CompactionDurableState` 都已有指标字段；
  缺的是**从观测到渲染**的那条链。
- ⇒ **T4-03 的第二项（journal 增长有界且可见）可以照抄卷序列号那一刀的形状**（四层，已走过一次）：
  1. `WorkerRuntimeObservation` 增 `journal_bytes`（或占用百分比）；
  2. `product::ServiceDiskView` 增字段 + `to_string`；
  3. 线格式追加字段（**注意 `kProfileFixedBytes` 这类"固定长度护栏"要一起长大**——卷序列号那次前两次失败就是它）；
  4. `cli_renderer` 渲染，`m5.cli` 加一条断言；客机读数：`disk show` 里出现该字段且随 workload 增长。
- **T4-03 剩余因此是两件清楚的事**：① 上述可观测（机械，有先例）；② **自动触发策略**（服务侧策略，
  需要定"何时自己发起"，建议：`checkpoints_since_compaction ≥ compaction.checkpoint_count_threshold`
  或 `journal 占用 ≥ journal_occupancy_percent`，两者都已在 `CompactionPolicy` 里）。
### 3.126 2026-10-27 16:35：**T4-03 的两半卡在同一处**——journal/compaction 指标从来没有到达服务

- `ProfileState`（`state_machine.h:102`）只有 `lifecycle` + `disk{runtime, activity, persistence, health}`，
  **没有任何 journal 或 compaction 指标**；`ServiceMaintenance::tick()` 的输入正是这个列表。
- ⇒ 我实现的 `compaction_due_reason()`（`78a02ab`，11/11）**无法被 tick 用上**：
  策略函数需要 `journal_occupancy_percent` / `reclaimable_percent` / `checkpoints_since_compaction`，
  而 tick **没有这些数**。抄 checkpoint 那段 dispatch 也救不了——**参数根本不存在**。
- ⇒ **T4-03 的两半（自动触发 + 可观测）其实卡在同一处**：**指标必须先从权威来源进入服务**。
  这个来源是**耐久状态**（`CompactionDurableState` 里已有 `successful_compactions_total`、
  `checkpoints_since_compaction`）与 **worker 的运行时观测**（journal 占用）。
- **因此正确的顺序是**（我此前把顺序写错了）：
  1. **先把指标加进服务侧的 profile 视图**（就是 §3.125 那条四层链：observation → view → wire → renderer，
     顺带在 `ProfileState`/`dashboard` 一侧暴露给 maintenance）；
  2. **再**让 tick 用 `compaction_due_reason()` 决定；
  3. **最后**才是 dispatch（那时才有 `DiskCompactCommand` + `no_wait` + 日志，形状与 checkpoint 那段一致）。
- **教训**：我把"策略函数"当成第一步，其实**数据通路**才是第一步。这与今天反复出现的那条同源：
  **先确认东西在哪，再写用它的代码。**
### 3.127 2026-10-27 16:45：**修正 §3.126 的"全堵在数据通路"**——`checkpoint-count` 触发器**今天就能接**

- `ServiceMaintenance::tick()` 的数据源不是 `ProfileState`，而是 `listed.value().profiles` 的**快照**
  （`ManagedProfileSnapshot`），它**同时带** `record.profile.compaction`（策略）与
  `compaction`（`CompactionDurableState`，含 **`checkpoints_since_compaction`** 与
  `successful_compactions_total`）。
- ⇒ **`checkpoint-count` 这一个触发器不需要任何协议改动**：tick 里已经有它要的两个数。
  真正缺 worker 侧数据的只有 **`journal-occupancy` 与 `reclaimable`**（worker 未上报占用），
  而我的策略函数把 `0` 当作"该触发器关闭"，所以这两条**不接也不会误导**。
- ⇒ **T4-03 自动触发的正确下一步**（比 §3.126 说的更小）：
  1. tick 里填充 `input.compaction = profile.compaction`、
     `input.checkpoints_since_compaction = snapshot.compaction.checkpoints_since_compaction(截断到 uint32)`；
  2. 仅在 `snapshot.compaction.phase == CompactionDurablePhase::stable` 时评估（**不许在压缩进行中再要求一次**）；
  3. 理由非空则 `DiskCompactCommand{target}` + `caller` + `no_wait` 派发并记日志（形状与 checkpoint 那段完全一致）。
- **本轮状态**：尝试按上写下这三步时，**替换锚点没匹配上**（`candidate anchor missing`），
  **文件未被修改**（写入在锚点校验之后才发生），构建与工作树均干净。下次用 `edit` 工具按行写入即可。
### 3.128 2026-10-27 17:00：自动压缩**接线完成但今天无法端到端验证**——阈值不能从 CLI 调

- 自动触发已端到端接线（`bd32f41`：策略 → 候选 → `DiskCompactCommand` + `no_wait` 受理）。
- **但验收需要的"磁盘真的自己压缩"读数今天取不到**：
  - `CompactionPolicy` 默认 `checkpoint_count_threshold = 64`、`journal_occupancy_percent = 70`、`reclaimable_percent = 50`；
  - `disk create` **没有**任何设置这三项的选项；`disk config set` 只支持 `auto_start`（`cli_parser.cpp` 断言
    `words[3] == "auto_start"`）；
  - ⇒ 要用默认阈值触发一次自动压缩，需要**64 次 checkpoint**（`cli_e2e` 用 30 s 间隔 ⇒ 32 分钟），不可行。
- **因此 T4-03 的验收需要一个很小的前置**（建议二选一）：
  1. `disk create` 加 `--compaction-threshold <count>` / `--compaction-occupancy <percent>`（形状与
     `--checkpoint-interval` 完全一致，走 `DiskCreateCommand` → intent → `CompactionPolicy`）；
  2. 或 `disk config set` 增加对应两个键（需要一个新的配置命令面，比 1 大）。
- **读数脚本（有了 1 之后）**：`create --compaction-threshold 2 --checkpoint-interval 30s` → 等两次 checkpoint →
  `disk show` 的 `successful_compactions_total` 增加；日志里出现 `maintenance: compaction dispatched (checkpoint-count)`。
- **这条也解释了为什么 T4-03 之前一直标 PARTIAL**：机制全在，但**没有可操作的旋钮**去触发它，
  所以既没人测过、也没人用过。
### 3.129 2026-10-27 17:05：**新接线的一个已知风险**（我引入的，未修）

- `tick()` 每 15 s 一次。自动压缩派发**没有"刚刚派过"的记忆**（checkpoint 有 `record_last_checkpoint`，
  compaction 没有对等物）。因此在"已受理、尚未进入非 stable 阶段"的窗口里，下一次 tick 会**再派一次**
  `disk compact`；core 大概率以状态拒绝并记一条 `operation_failed` 日志。
- **后果**：不会损坏数据（受理是耐久的，状态机会拒绝），但会**产生误导性的失败日志**，
  且在 `--compaction-threshold` 很小的配置下会连续刷屏。
- **修法（下次，很小的改动）**：镜像 checkpoint 的做法——记 `last_compaction_dispatch`（profile → 时间/理由），
  在派发前跳过"最近 N 秒内已派发过同一 profile"的候选；或更简单：**只在一个稳定窗口的第一个 tick 派发**
  （例如把 `successful_compactions_total` 也纳入键，派发后要求它变化才允许再派）。
- **我没有实现它的原因**：需要新增成员 + 与 `state_mutex_` 的交互，而在会话末尾盲改状态机周边的锁与簿记，
  风险高于收益。**记下来，比改坏强。**
### 3.130 2026-10-27 17:30：**阈值到 profile 的那一改把 create 弄坏了**——已回退，留下精确线索

- **客机读数 1**（`d3408e1a…`，修 `ed7b302` 之前）：checkpoint 到 **generation 8**，服务日志 307 行里
  **`compaction` 出现 0 次** ⇒ 自动压缩从未触发。原因：**选项走到了 intent 就停了**
  （`service_core.cpp` 里记录仍构造默认 `CompactionPolicy{}`，阈值 64）。
- **修法 `ed7b302`**：在记录构造前用 intent 构建 `compaction_policy` 并交给记录。
  本地套件全绿（orchestration 25 / host 25 / maintenance 11 / cli 17）。
- **客机读数 2**（`d3408e1a…` 之后，带修重跑）：**create 直接失败**（38 s，`the policy disk create ended failed`，
  且 `1 worker process(es) survived`）。
  ⇒ 我的改动**破坏了创建路径**：最可能是我用来删 no-op 的那条 regex
  （`// A create may ask for …` 到下一个 `}`）**删多了或删错了位置**，
  把 `materialize_*_profile` 里的一段真正逻辑一起吃掉——它仍能编译，因为删掉的是一段赋值。
- **已回退**（`2181e5b`）：构建零错误、orchestration 25/25、host 25/25。**工作树干净**。
- **下一轮该做的（顺序不能变）**：
  1. **先**读 `service_core.cpp` 的 `materialize_*_profile`（约 540–600 行）**当前完整内容**，
     确认 `checkpoint` 策略是怎么被逐字段赋值的，**照它的样子**给 `compaction` 赋值
     （不要用"构建一个 CompactionPolicy 再塞进聚合"的方式，那是我的失误）；
  2. 本地 `orchestration` + `host` 绿；
  3. 客机跑 `cli_e2e`（它已带 `--compaction-threshold 2` 与"180 s 内必须在日志里看到
     `maintenance: compaction dispatched (checkpoint-count)`"的断言）。
- **教训（今天的第 6 次）**：**用正则删一段带注释的代码，是这个会话里最危险的动作**。
  前五次是"改了没落地"，这一次是"删了不该删的"。
### 3.131 2026-10-27 17:45：**那次 `cli_e2e` 通过不算证据**——断言被我自己的回退带走了

- 事实链：
  1. `ed7b302` 那次提交把 `service_core.cpp` **和** `test-m5-cli-e2e.ps1` 一起提交了（我用了 `git add -A`）；
  2. `git revert` 回退它时，**e2e 里的自动压缩断言也一起被撤销**；
  3. 于是随后那次 `cli_e2e` **PASS 78 s** 是**没有断言**的通过——**不能**解读为"自动压缩发生了"；
  4. 我重新加回断言后再跑：**12 s 就失败**，说明我插入的那段 PowerShell 本身有问题
     （位置或变量作用域），**不是产品问题**。
- **已处理**：`git checkout -- tests/e2e/test-m5-cli-e2e.ps1`（回到无断言的干净版本），工作树 0 改动。
- **留给下一轮的**：把断言写进 e2e 时**先只加一行 `Write-Host`**，跑一次确认它真的执行到那里，
  **再**加等待与 `Fail`。这与"先验证编辑落地、再构建"是同一条纪律，我今天在 PowerShell 上还没用过它。
- **产品的现状**（不受影响）：`--compaction-threshold` 已能到达 profile（`ed7b302` 的修法已被
  "字段逐项赋值"版取代并通过本地套件），自动压缩**已实现但尚未有客机证据**。
### 3.132 2026-10-27 17:55：**阈值到 profile 的两次尝试都让 create 失败**——已全部回退，这次留下可查的方向

| 尝试 | 本地 | 客机 | 结论 |
|---|---|---|---|
| `ed7b302`（构建 `CompactionPolicy` 塞进聚合） | 全绿 | **create 失败**（38 s） | 坏 |
| `a0a4dbe`（逐字段赋值，照 checkpoint 的形状） | 全绿 | **create 失败**（24 s） | **也坏** |
| 全部回退（`2181e5b`、`e562e9e`） | 全绿 | 之前那次无断言 `cli_e2e` **PASS 78 s** | 回到可用状态 |

- ⇒ **不是"写法"问题**：两种写法都让 create 在客机上失败，而**本地 `orchestration`/`host`/`cli` 全绿**
  ⇒ 说明**本地测试没有覆盖"带 compaction 阈值创建"这条路径的真实副作用**，而客机在
  `make_registration_record` 之后某处失败（stderr 只有 `the create did not succeed: failed`）。
- **下次的正确诊断顺序**（不要再盲改写法）：
  1. 客机 `last-service.log` 里找该次 create 的 `kind=7` 行 —— 它会给出 `step=` 与 `code=`
     （上一次我只看了 e2e 的 stderr，没看服务日志，这是信息浪费）；
  2. 若是 `validate_profile` 拒绝，日志会是 `invalid_request` + "materialized profile violates schema"；
     那就要看 `compaction.checkpoint_count_threshold = 2` 是否与其它策略字段**互相矛盾**
     （例如 `index_headroom_percent` 或 `journal_occupancy_percent` 的下界）；
  3. 若日志里根本没有该 create 的服务端行，则问题在**命令解码/受理**，回到 `ipc_codec` 字段 12。
- **当前 HEAD 状态**：`e562e9e` —— 构建零错误、本地套件绿、`cli_e2e`（无 compaction 断言）通过。
  **`--compaction-threshold` 仍在 CLI/线格式/intent 中可用，但不会影响 profile**（策略层未接）。
**这一条的教训**：**"本地全绿"对"带设备的 create 路径"没有证明力**。两次都是本地绿、客机红。
### 3.133 2026-10-27 18:00：把范围再缩小一格——失败的很可能是**策略盘**那次 create，而且**不是校验拒绝**

- 时间线对得上：主盘 create + 挂载 + checkpoint 大约 20 s，**失败发生在 24 s** ⇒ 很可能就是
  **第二个（策略）盘**的 create，也就是**唯一带 `--compaction-threshold 2` 的那次**。
- 而 `profile_validation.cpp:327-331` 的边界是
  `journal_occupancy_percent ∈ [50,95]`、`reclaimable_percent ∈ [10,90]`、
  **`checkpoint_count_threshold ∈ [1,4096]`** ⇒ **2 是合法值**，所以**不是校验拒绝**（至少不是这条规则）。
- ⇒ **下一步只需要一个读数**（不用改代码）：在**当前 HEAD**（阈值不落 profile，create 正常）基础上，
  **只加回 `service_core.cpp` 里那两行**（`profile::CompactionPolicy compaction{}` + 一次赋值 + 聚合换名），
  然后**跑一次 `cli_e2e` 并读 `C:\m5\last-service.log` 里该次 create 的 `kind=7` 行**——
  那条会写明 `step=` 与 `code=`，而这是我从头到尾**唯一没读**的一手信息。
- **如果日志里该 create 没有任何 `kind=7`**：说明失败在**命令解码**（字段 12 的 `FieldRequirement`
  我设的是 `optional`，但**服务端 schema 的 required 集合**可能与客户端不一致——两端的 `kCreateFields`
  必须逐项相同，这是今天已经踩过的"两端一致"问题）。
- **给下一任的提醒**：**不要**再靠"换一种写法试试"推进这一项；**先读服务日志的 `kind=7` 行**。
  我今天为此多花了两轮客机运行（各 4 分钟），而那条日志一直都在 `C:\m5\last-service.log`。
### 3.134 2026-10-27 18:10：追踪日志显示**枚举本身只要 12 毫秒**——所以那次 `code=258` 来自别处或来自误判

**证据（`C:\m5\last-trace.log`，1413 行，最后一段）**：
```text
01:51:09.316Z enumerate_disk_candidates: opening the candidate
01:51:09.320Z enumerate_disk_candidates: CreateFileW took 0 ms
01:51:09.325Z enumerate_disk_candidates: IOCTL_SCSI_GET_ADDRESS took 0 ms
01:51:09.328Z enumerate_disk_candidates: end, 2 candidates, all steps returned
```
⇒ 枚举**12 毫秒内成功**，而服务日志仍报 `code=258 ... enumerate_disk_candidates`。

**三种可能，下一步用一个 `diagnostic_trace` 就能分辨**：
1. **上界值没生效**：某个调用点仍传旧值（我已把四处都改成 `kEnumerateTimeout`，但**staged 二进制**是否真的重编过需要哈希对齐一次）；
2. **`wait_until` 误判**：包装器代码看起来正确（有谓词、`notify_one` 在解锁后），但 `complete` 与 `outcome`
   的可见性依赖同一把锁——这条本身没问题，**不能排除是我读漏了别处的类似包装**；
3. **258 来自另一条路径**：例如 worker 侧对 `create-bundle` 动作有自己的等待预算，而 `detail` 文本恰好相似。

**下一步（很小）**：在 `enumerate_disk_candidates_within` 的**超时返回前**与**成功返回前**各打一行
`diagnostic_trace("enum_within: timeout/completed at …")`，跑一次 e2e，看是哪一行出现。
**不要**再调上界值——10 s、30 s、120 s 三个值都试过，失败时间几乎不变（29/35/38 s），这本身说明**上界不是变量**。

**当前 HEAD（`1504b5f`）**：`kEnumerateTimeout = 120 s`；`cli_e2e` **失败**（38 s，同一个 `code=258`）；
`--compaction-threshold` 已在 CLI/线格式/intent/**profile** 全链就位（本地套件全绿），**但客机读数被这个 create 失败挡住**。
### 3.135 2026-10-27 18:20：**决定性证据——包装器从不超时**，那条 `258` 是**误归因**

一次运行里 6 次 `enum_within: completed normally`，**一次 `timeout fired` 都没有**，而服务日志仍写
`code=258 step=enumerate_disk_candidates detail=enumerating … did not finish within its bound`。

⇒ **这条错误不是包装器产生的**：`step`/`detail` 是**上一次动作留下的陈旧字段**，被套在了这次 create 失败上。
`code=258` 也很可能不是 `WAIT_TIMEOUT` 的真身，而是**别处**（例如 worker 管道读超时）被映射出来的。

**下一轮的唯一任务（不要再调上界、不要再改 profile）**：
1. 找到 **`create-bundle` 失败时谁在写 `step`/`detail`**（`WorkerActionResult` → 服务日志的那一段），
   让它们**描述真正发生的失败**；
2. 顺带把 `code` 的映射也查一遍（258 是否被人为赋值）；
3. 修好诊断后**重跑**，那时才会看到真实的失败原因——**在此之前，任何关于 create 的推断都不可信**。

**这条也解释了今天后半段的两次白改**：我被一条**假线索**（`step=enumerate_disk_candidates`）牵着走了两轮，
包括把上界从 10 s 提到 30 s 再到 120 s，**三个值都无效**。教训：**当一条诊断自相矛盾（12 ms 成功 vs "超时"），
先怀疑诊断本身。**
### 3.136 2026-10-27 18:30：那条消息**只有一个产出点**，而它的 trace 在服务进程里——**worker 那条没被记录**

- 全树 grep：`"did not finish within its bound"` **只有一处**（`physical_drive_locator.cpp:425`，就是我加的包装器）。
  所以 worker 报的那条**必然来自同一个函数**。
- 而那 6 行 `enum_within: completed normally` 来自**服务进程**的枚举（服务在 create 前自己也会枚举）。
  ⇒ **worker 进程里的那次调用没有被记录**，最可能的原因是：
  **`diagnostic_trace` 依赖 `MYRD_TRACE_FILE`，而 worker 若是用自定义环境块启动的，就写不出 trace**。
- **下一次的最小实验（不用改逻辑）**：
  1. 在包装器**超时分支**额外写一条**进服务日志**（`logger_` 不可用，那就用 `OutputDebugString` 或
     把 trace 路径显式传给 worker）；
  2. 或者最简单：**让 worker 启动时显式继承/接收 `MYRD_TRACE_FILE`**，然后重跑——若 trace 里出现
     `enum_within: timeout fired`，则**上界确实在 worker 里生效**，那时再谈"为什么 worker 的枚举要 >120 s"。
- **当前判断（谨慎）**：`code=258` **可能就是** WAIT_TIMEOUT，只是它发生在 **worker**，
  而我把服务的 trace 当成了全部证据。这解释了"三个上界值都不起作用"吗？**不能**——若上界在 worker 生效，
  改 10→30→120 应当改变失败时间，而实测几乎不变（29/35/38 s）。
  ⇒ **仍然自相矛盾**，所以下一步必须是"把 worker 的 trace 拿到"，而不是再推理。
### 3.137 2026-10-27 18:40：trace 的**结尾是干净的**——失败不是本次运行的包装器产生的

`trace3.log`（1435 行）最后 12 行：

```text
01:53:34.192Z worker: command received
01:53:34.194Z opstore.write: waiting for gate
… atomic_publish: current published
01:53:34.233Z opstore.write: done
01:53:34.234Z scheduler: execute returned
```

⇒ **调度器正常返回**，**没有任何 `enum_within: timeout fired`**，却仍然向客户端报了
`code=258 step=enumerate_disk_candidates detail=…did not finish within its bound`。

⇒ 这条错误**不是本次运行里那个包装器产生的**。最可信的两个来源：
1. **持久操作记录里残留的失败证据**被复用/回放（`opstore` 是本会话多次提到的原子发布存储）；
2. 某个**陈旧 `WorkerActionResult`** 被当成这次的结果。

**下一轮的唯一动作**：把这次 `disk.create` 的**操作记录**（`operation.show` 的输出，或
`C:\ProgramData\MyRamDisk\operations` 里的那个文件）读出来，看 `failure` 字段的**时间戳与步骤**
是否属于本次运行。这能一次分辨 1 与 2，而且**不需要改代码、不需要再跑 e2e**。
### 3.138 2026-10-27 18:45：操作记录是**二进制 TLV**，读不出失败详情——需要 `operation.show`，而那要求服务在跑

- `C:\ProgramData\MyRamDisk\operations\*.myop`（372–542 字节）确实是**本次失败运行留下的**（时间 9:53），
  但它们是 `MYRDOPR` 开头的 **TLV 二进制**（本机按 ASCII 转储只见字段边界与 SID 文本），
  **无法直接读出 `failure` 字段**。
- ⇒ 要读失败详情，唯一途径是 **`myrd-cli operation show <id>`**，而那需要**服务在运行**。
  e2e 在失败后立刻清理并停服，所以那条路径在现有 e2e 里取不到。
- **下一轮的具体做法（二选一，都很小）**：
  1. 在 `test-m5-cli-e2e.ps1` 的 create 失败分支里**先 `operation show` 再 Fail**
     （服务此刻还在跑，一条 CLI 调用即可）；
  2. 或写一个 5 行的独立脚本：起服务 → create（带 `--compaction-threshold 2`）→ `operation show` → 停服务。
- **为什么值得**：现在的症状是"**报错内容与 trace 自相矛盾**"（§3.134–§3.137）。
  只有 `operation.show` 的 `failure` 字段能判断"这条错误是本次的还是回放的"。**在此之前，T4-03 的客机证据无法取得。**
### 3.139 2026-10-27 18:55：**失败的是"策略盘"那次 create**（带阈值的那个），主盘创建成功

同一次运行（`d243ad9b…`）的 stdout 把两件事分开了：

```text
operation bd2a3b13-…: succeeded (phase terminal)          ← 主盘 create 成功
--- T1-08: an automatic checkpoint fires on the interval policy
  policy create: disk.create: operation queued
operation 127d80ff-…: running (phase admitting)            ← 策略盘 create 卡在这里
--- cleanup
```

- ⇒ **带上 `--compaction-threshold 2` 的那次 create 就是失败的那次**，而**主盘（默认阈值）成功**。
- ⇒ 服务日志里那条 `code=258 step=enumerate_disk_candidates` 属于 `127d80ff`；
  而 trace 显示**枚举从未超时** ⇒ **这条错误的内容与它所描述的失败不是一回事**。
- **我插入的 `operation show` 没触发**，因为策略盘的失败走的是另一条 `Fail`（"policy disk create ended failed"），
  不是主盘那条。**下一轮把它也加上**（同样一行），就能读到 `127d80ff` 的失败字段。
- **最值得先试的一个假设**：**创建期间 tick 就把这个 profile 当成可压缩对象**
  （`checkpoint_due_reason` 空 → 走到 compaction 分支 → `phase == stable` → 计数已达 2 ⇒ 立刻派发 compact），
  于是 **create 过程中并发了一次 compress**，把 create 弄失败。
  验证方法：把 tick 的 compaction 分支加上 `input.state.disk.runtime == running` **且**
  `lifecycle == ready` 的双重条件（或直接用 `active_operation_id` 已经覆盖的那条），再看是否还失败。
### 3.140 2026-10-27 19:10：**真因找到了——store 侧有一份"重新构造的 profile"用于比对，它没带上阈值**

**完整错误文本**（服务日志原文，此前被我截断）：
```text
publish_registration refused: code=22024 native=0 step=profile_registration.publish
detail=The proposed registration does not exactly match the lifecycle intent and bundle evidence
```

`profile_registration_store.cpp:1979` 附近的检查是：
```cpp
auto materialized = materialized_registration_profile(intent, bundle, volume);
if (!materialized.ok() || proposed != materialized.value() || !validate_profile(proposed).ok())
    return store_error(… "does not exactly match …");
```

而 `materialized_registration_profile()`（同文件 1074 行起）里那一行仍然是：
```cpp
    profile::CompactionPolicy{}}};      // ← 默认策略（阈值 64）
```

⇒ **`disk create --compaction-threshold 2` 必然在这里失败**：`make_registration_record()` 造出的
profile 带阈值 2，store 重造的比较对象是 64，两者**不相等** ⇒ 22024。
这解释了今天全部症状：主盘（默认阈值）成功、带阈值的那次失败、枚举报错是**陈旧字段的噪声**。

**修法（两处，形状已定）**：在 `materialized_registration_profile()` 里照
`make_registration_record()` 的写法加同样的两行：
```cpp
profile::CompactionPolicy compaction{};
if (intent.compaction_checkpoint_threshold.has_value()) {
    compaction.checkpoint_count_threshold = *intent.compaction_checkpoint_threshold;
}
```
并把聚合里的 `profile::CompactionPolicy{}` 换成 `compaction`。

**我这轮尝试直接改它，锚点因文件是 LF 结尾没匹配上，未做任何修改**（工作树干净）。
**这是今天最有价值的一条结论**：它把"神秘 create 失败"变成了"**两处必须一致**"——
而这正是今天第 5 次同族问题（profile 字段数、CLI 白名单、schema 计数、字段表、现在是这个）。
### 3.141 2026-10-27 19:25：**T4-03 自动触发：已证明，并且现在由 e2e 断言守住**

**根因（本轮修复）**：`profile_registration_store.cpp` 的 `materialized_registration_profile()`
用**默认 `CompactionPolicy`** 重建比对对象，而提议里带着 `--compaction-threshold 2` ⇒
`publish_registration` 以 **22024 "does not exactly match…"** 拒绝创建。**日志里的 `step=` 指向更早的枚举步骤，是误导。**

**修复后（两次连续通过）**：
| 运行 | 结果 | 用时 |
|---|---|---|
| `5d383052…`（探针版） | **PASS**，`probe: compaction observed = True` | 130 s |
| `fa6b1c05…`（**断言版**） | **PASS** | 103 s |

服务日志（`5d383052…` 那次）：
```text
kind=0 command=disk.compact phase=authorized
kind=1 command=disk.compact phase=queued
kind=1 command=disk.compact phase=maintenance message=maintenance: compaction dispatched (checkpoint-count)
```

**e2e 现在会守这条**：`--compaction-threshold 2` + 30 s 检查点 ⇒ 轮询 ≤180 s 等
`maintenance: compaction dispatched (checkpoint-count)`，等不到就 `Fail`。**探针已删除。**

**T4-03 剩余**：`journal`/`compaction` **指标的可观测性**（`disk show` 不显示，§3.125 给了四层形状）。
自动触发这一半**已完成并有客机证据**。
### 3.142 2026-10-27 19:40：压缩完成时**日志会报计数**（可观测性的第一步）

```text
compaction: completed total=<N> checkpoints_since=0
```
写在该次操作终态前（`ServiceLogKind::operation_terminal`）。**为什么有用**：T4-03 要求"journal 增长有界且可见"，
而此前**服务日志一个字都没有**——一个不断长大的盘，运维无从读起。

**仍缺（客户端可见的那一半）**：`disk show` 不显示这两个数。**形状已定（§3.125）**：`ServiceDiskView` 加字段 →
线格式加字段（**`kProfileFixedBytes` 护栏一起长**）→ 投影填充 → 渲染 + `m5.cli` 断言。

**读数**：`orchestration 25/25`、`host 25/25`、`maintenance 11/11`，构建零错误。
### 3.143 2026-10-27 19:55：**T4-03 完成**（自动触发 + 可观测，均有客机证据）

**证据链**：
| 环节 | 读数 |
|---|---|
| 策略 | `compaction_due_reason()` **11/11**（停止盘不压、`automatic=false` 静默、三理由按序、零值关闭） |
| 派发 | tick 仅在 `phase == stable`、`lifecycle == ready`、无 `active_operation_id` 时派发 `DiskCompactCommand`（`no_wait`，与 checkpoint 同形状） |
| 客机 | **两次连续 PASS**：`5d383052…`（130 s，探针版）、`fa6b1c05…`（103 s，**断言版**） |
| 日志 | `maintenance: compaction dispatched (checkpoint-count)`、`compaction: completed total=N checkpoints_since=0` |
| 客户端 | `disk show` 输出 `compactions=N`；线格式四处一次性提交后 **客机 `cli_e2e` 跨进程通过**（`f3a09779…`，104 s） |
| 测试 | `m5.cli` 17/17、`m1.cli` 5/5、`orchestration` 25/25、`host` 25/25、`maintenance` 11/11 |

**今天在这项上学到的两条**（都写进了代码注释）：
1. **`step=` 可能是上一次动作的陈旧字段**——我因此把枚举上界从 10 调到 30 再到 120，三次都无效；
2. **两处必须一致**——`make_registration_record()` 与 `materialized_registration_profile()` 是同一份 profile 的两次构造，
   只有前者带阈值时，带阈值的 create 就必然被 22024 拒绝。
### 3.144 2026-10-27 20:05：**两条缺的客机读数，给一段可直接粘的脚本**

下一任要取的两条读数（T4-02 的 Load Only 与易失盘）**不需要新功能**，只需要一个场景。
把下面这段加进 `tests/e2e/test-m5-cli-e2e.ps1`（放在现有 `disk forget` 之前，`Invoke-Cli`/`Fail` 已在该文件里）：

```powershell
    # --- T4-02: a stopped create starts nothing, and a volatile disk refuses a plain stop ---
    $quietName = 'm5-e2e-quiet-' + [guid]::NewGuid().ToString('N')
    $quietBundle = Join-Path 'C:\m5' $quietName
    $quiet = Invoke-Cli @('disk', 'create', '--name', $quietName, '--bundle', $quietBundle,
        '--size', '64MiB', '--mount', 'Z:', '--no-start', '--checkpoint-interval', 'off')
    if ($quiet.exit -ne 0) { Fail "the quiet create failed: $($quiet.output)" }
    $quietId = Get-UuidFromText $quiet.output
    if ($null -eq $quietId) { Fail 'the quiet create carried no operation id' }
    if ((Wait-ForTerminal $quietId 300) -ne 'succeeded') { Fail 'the quiet create did not succeed' }
    $quietList = (Invoke-Cli @('disk', 'list')).output
    if ($quietList -notmatch 'Ready\s+stopped/configured') { Fail "a --no-start create is not stopped: $quietList" }
    if ((@(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue)).Count -ne 0) {
        Fail 'a --no-start create left a worker running'
    }
    $stopPlain = Invoke-Cli @('disk', 'stop', $quietName)
    if ($stopPlain.exit -eq 0) { Fail 'stopping a volatile disk did not require --accept-volatile-data-loss' }
    if ($stopPlain.output -notmatch 'volatile') { Fail "the refusal does not mention volatile data: $($stopPlain.output)" }
    Write-Host '  quiet create left no worker; volatile stop was refused as it should be'
```

**为什么这样写**：
- `--no-start` 走的是 `ManagementCommand::create_stopped`（§3.123 已证明分派无条件），
  所以"没有 worker"是**硬断言**，不是观察；
- `--checkpoint-interval off` 就是易失盘（§3.121 的裁定），`disk stop` 必须要求
  `--accept-volatile-data-loss`——**拒绝**才是正确行为，脚本断言的是"拒绝发生"。
- 若 `Z:` 被占用，换一个 `disk list` 里没出现的盘符。
### 3.145 2026-10-27 20:15：**我插入的 T4-02 场景两次失败，已回退**——但失败信息本身有价值

| 尝试 | 结果 |
|---|---|
| 直接断言"`--no-start` 后无 worker" | **Fail**：`a --no-start create left a worker running` |
| 加 30 s 静置窗口后再断言 | **仍 Fail**（41 s 内结束） |

- ⇒ **`--no-start` 的 create 之后，客机上仍有 `myrd-m5-worker` 进程**（至少持续到 30 s 之后）。
  这可能意味着：① `create_stopped` 仍会拉起 worker 再停（与"不启动"的语义不符）；
  ② 或 e2e 前一步刚停下的盘留下的 worker 迟迟不退。
- **无法在本次会话内分辨**（那需要读那次运行的 worker StartTime 与服务日志）。
- **已回退**：`git checkout -- tests/e2e/test-m5-cli-e2e.ps1`，工作树 0 改动。
  **e2e 回到上次通过的状态**（`f3a09779…` 104 s 通过，含 T4-03 断言）。
- **下一任的第一步（很便宜）**：跑一次带该场景的 e2e，**失败时立刻记录 `Get-Process myrd-m5-worker | Select Id,StartTime`**
  与服务日志里该盘的 `disk.stop`/`disk.create` 行——**StartTime 能立刻区分①②**。
- **这条也修正了我先前的一个说法**：§3.123 说"import 走 `import_profile`"是**代码层**证据，
  但**"`--no-start` 的 create 实际不拉 worker"是运行时行为**，两者不能互相替代。这正是今天反复出现的教训：
  **代码说它不该启动，不等于运行时它没启动。**
### 3.146 2026-10-27 20:20：**worker 不是泄漏，但退出超过 30 秒**——这解释了那次断言为何失败

- 现在（失败运行之后数分钟）客机上 **`myrd-m5-worker` 数量 = 0** ⇒ **不是泄漏**，进程最终退出了。
- 结合 §3.145：那次断言在"主盘 stop 之后 30 秒"仍看到 1 个 worker ⇒ **停止后的 worker 退出耗时 > 30 s**。
- **合理怀疑**：`disk stop` 返回时 worker 只是"被要求退出"，真正的退出由**调度/清扫**在后续 tick（15 s）里完成，
  期间可能还要等设备退役（本会话记录过"设备退役窗口"）。**这本身可能不是缺陷**——但它让"停完立刻查进程"不可靠。
- **下一任的正确断言**（替换 §3.144 里那条）：**轮询到 120 s**并**打印实际耗时**：
  ```powershell
  $t0 = Get-Date
  while (((Get-Date) - $t0).TotalSeconds -lt 120 -and @(Get-Process -Name 'myrd-m5-worker' -ErrorAction SilentlyContinue).Count -ne 0) { Start-Sleep -Seconds 2 }
  Write-Host ("  workers settled after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
  ```
  若 > 60 s，那**本身值得作为一条产品读数记下来**（用户按 stop 之后，worker 还活着一分钟）。
- **顺带修正 §3.144 的措辞**：那里说"没有 worker 是硬断言"——**只有等它退出才是**；否则断言的是"退出够快"，那不是我原本想测的东西。
### 3.147 2026-10-27 20:30：**`--no-start` 的 create 在客机上直接失败** —— Load Only **并不是"已经能用"**

- 更正后的场景（120 s 轮询 + 易失盘 stop 拒绝）**在第一条断言就失败**：
  `the quiet create did not succeed`，且 `1 worker process(es) survived the run`。
- 也就是说 **`disk create --no-start --checkpoint-interval off --mount Z: ...` 这次创建没有成功**。
- **因此 §3.122 的结论需要收紧**：那里说"Load Only = `disk import`，已存在"——那是**代码层**证据（分派表）。
  而 **`--no-start` 的 *create*** 是另一条路径，**它在客机上失败**。**两条路径不能互相替代**（§3.146 已记同一类错误）。
- **已回退**：`git checkout -- tests/e2e/test-m5-cli-e2e.ps1`，工作树 0 改动；e2e 回到含 T4-03 断言的通过版本。
- **下一任的第一步**：把这条命令手跑一次并读**服务日志的 `kind=7` 行**（就像修 T4-03 那次一样）——
  大概率是某个具体步骤（挂载/发布/设备）失败，而不是"`--no-start` 不支持"。
  可能的嫌疑：`--checkpoint-interval off` 与 `--no-start` 同时给出时，profile 的某个字段组合触发校验；
  或 `Z:` 在客机上不可用（e2e 里应改用 `disk list` 里没出现的盘符）。
- **结论（对本轮）**：**T4-02 的 Load Only 与易失盘都还不能算完成**。我先前说"只差读数"是错的。
### 3.148 2026-10-27 20:35：**那次 quiet create 失败是"重复注册"，不是 `--no-start` 的问题**

服务日志（那次运行的 `last-service.log`，232 行）给出了确切的失败点：
```text
kind=7 disk.create phase=publish-provisioning
message=publish_registration refused: code=22024 native=0 step=profile_registration.publish
detail=The profile name, image, or bundle is already registered
```

⇒ **客机上留着上一次尝试的注册**（我那次运行**没有先 `Reset-M5Guest`**，而第一次尝试的 quiet 盘其实**创建成功了**，
只是随后的 worker 断言失败）。所以第二次用新名字、新 bundle 也被判为"已注册"——
**不是 `--no-start` 或 `--checkpoint-interval off` 的缺陷**。

**因此 §3.147 的结论要再更正一次**：`--no-start` 的 create **没有证据表明它会失败**；
我那次"失败"是**环境残留**。**Load Only 仍可能是"只差读数"**——但必须**先重置客机**再跑。

**这正是今天第 N 次的同一类错误**：客机上的红，先问前提（这次是"有没有先 Reset"）。我把 §3.147 写成"Load Only 不算完成"，
**那一步下结论太快了**。

**下一任的第一步（一条命令）**：
```powershell
# 客机
powershell -NoProfile -ExecutionPolicy Bypass -File C:\m5\Reset-M5Guest.ps1
# 然后跑带 §3.144 场景的 e2e（用 §3.146 的 120 s 轮询断言）
```
**如果那次通过**，T4-02 的 Load Only 与易失盘两条读数就都有了。
### 3.149 2026-10-27 20:45：**Load Only 的读数拿到了，而且它同时暴露了一个真缺陷**

重置客机后，同一个场景给出了两条读数：

```text
4bad2f44-925a-4e41-962f-82788e95c5b8  U:  Ready  stopped/configured  runtime=Stopped  activity=Idle
  persistence=Clean  health=Healthy  generation=4  mem=0/0
workers settled after 121 s (1 left)
```

1. ✅ **`disk create --no-start` 成功**，且 `disk list` 显示 **`Ready  stopped/configured  runtime=Stopped  mem=0/0`**
   ⇒ **Load Only 的语义成立**（§3.148 的更正方向对了；§3.147 的"Load Only 不算完成"是错的）。
2. ❌ **但 `--no-start` 的盘留下了一个 `myrd-m5-worker`，121 秒后仍在** ⇒
   **"stopped" 的盘实际占着一个 worker 进程**。这与 `runtime=Stopped`、`mem=0/0` 自相矛盾，
   也解释了本会话多次出现的 `WARNING: 1 worker process(es) survived the run`。

**这是一个应该修的产品缺陷**（不是测试问题）：worker 在 `--no-start` 的 create 之后没有被回收。
**下一任的第一步**：读那次运行的 `last-service.log` 里该盘 create 的终态行，看 **worker 是在哪一步被拉起、
又是谁应当在第几步回收它**（`create_stopped` 的意图是"只注册、不启动"）。
- 已回退 e2e（`git checkout`），工作树 0 改动；套件与 e2e 回到通过状态。
- **T4-02 状态更新**：**Load Only 语义已验证**；**易失盘的 stop 拒绝断言未执行到**（worker 断言先失败），
  所以易失盘那条读数**仍缺**。
### 3.150 2026-10-27 20:55：**`--no-start` 的计划里没有启动动作**——所以那个 worker 属于**被 stop 的盘**

`state_machine.cpp:321` 附近，`create_stopped` 的计划只有两个动作：
```cpp
    case ManagementCommand::create_stopped:
        append_action(plan, ActionKind::create_and_verify_bundle);
        append_action(plan, ActionKind::publish_provisioning_profile);
        return;
```
（对照 `create_and_start`：它多一行 `append_start_actions(plan, true);`。）

⇒ **服务不会为 `--no-start` 的 create 拉起 worker。** 因此 §3.149 里那个"121 秒后仍在"的 worker
**属于那一刻刚被 `disk stop` 停掉的主盘**（e2e 在插入点之前刚执行过 "stop it again before removing it"）。

**所以缺陷重新定位**：**`disk stop` 之后 worker 在 121 秒内没有退出** —— 这比"create 留下 worker"更严重，
因为 `disk stop` 是用户每天都在用的动作，而 `disk list` 会显示 `runtime=Stopped`。
**它是否等于"泄漏"取决于它最终会不会退出**：§3.146 观察过一次运行结束数分钟后 worker=0，
那可能是**服务整体退出时**带走的，而不是 worker 自己退出的。

**下一任的第一步（一条 grep 就能定性）**：读失败运行的 `last-trace.log`，找该盘 stop 之后的
`worker:` 行（例如 `worker: shutdown`/`worker: exiting`）——
- 若**没有**这类行 ⇒ **worker 不响应 stop，只在服务退出时被带走**（真缺陷）；
- 若**有**但之后仍在 ⇒ **退出路径卡在设备退役**（另一类缺陷，已有 §4.15.57 的先例）。

**本次两轮的价值**：`--no-start` 的清白由一个**计划表**证明；剩下的问题被**重新指向 `disk stop`**，
而它是一个用户可见的日常动作。
### 3.151 2026-10-27 21:05：**`disk stop` 之后 worker 仍在被"每 15 秒唤醒"**——这是要修的缺陷

失败运行（`44ee3644…`）的 `last-trace.log`（1381 行）里：
```text
worker: command received   02:20:55.398
worker: command received   02:21:10.398
worker: command received   02:21:25.423
…（每 15 秒一次，持续到最后）
```
- **`shutdown` / `cleanup` / `exiting` / `released` 关键字：0 条**。
- ⇒ 那个 worker **既没被要求退出，也一直在收命令**；而 `disk list` 同时显示 `runtime=Stopped`。
- **15 秒**正是维护 tick 的周期 ⇒ **tick 每轮都在和一个"已停止"的运行时通信**，这既让 worker 活着，
  也说明 **stop 没有真正终止运行时**（尽管 `stop` 的计划最后一步是
  `stop_runtime_and_release_lock`，见 `state_machine.cpp:341`）。

**下一任的第一步（按顺序）**：
1. 找到 `stop_runtime_and_release_lock` 的实现：它**是否真的**向 worker 发 shutdown / 关管道？
   若只是"标记状态 + 释放锁"，那 **worker 是设计上被遗弃的**（只在服务退出时被带走）——
   这与 `worker: command received` 每 15 秒一条**矛盾**（被遗弃的 worker 不该再收命令），
   所以**更可能是"标记了停止，但还在轮询"**；
2. 找到 tick 里**向每个 profile 发命令**的那一处（`worker: command received` 的来源），
   看它是否检查 `runtime == running`；
3. 两处都看过后，再决定是**停掉轮询**还是**补上 shutdown**。

**这条对验收的意义**：`disk stop` 是用户日常动作；"显示已停止但 worker 还活着并每 15 秒被唤醒"
**在验收时一定会被问到**（内存是否真的释放、设备是否真的退役）。**它不是测试问题，是产品行为问题。**
### 3.152 2026-10-27 21:15：**每 15 秒的"唤醒"来自 tick 自己发的 `disk list`**

`service_maintenance.cpp:170` 起，tick 每轮做的第一件事就是：
```cpp
    ServiceCommandRequest list_request{};
    list_request.command = control::DiskListCommand{};
    auto listed = core_.dispatch(list_request);
```
而 `disk list` 的投影需要 `runtime_observation`（`mem=`、`runtime=`、`serial=` 都来自它，
见 `service_response_wire.cpp` 的 `snapshot.runtime_observation->…`）——
**对运行中的盘，这要问 worker**。于是：
- 维护 tick（15 s）→ `disk list` → 投影 → 问 worker → **worker 收到一条命令**（正是 trace 里的
  `worker: command received` 每 15 秒一次）；
- 而那块盘的 `runtime` 已经是 `Stopped`（`disk show` 都这么显示）。

⇒ **两条修法（下一任按顺序判断）**：
1. **让 `disk list` 在 `runtime != running` 时不查 worker**（快、局部、可测）；
2. 或者**让 stop 真正终结 worker**（治本；但要看 §3.151 第 1 步的结论：
   `stop_runtime_and_release_lock` 到底做了什么）。

**判断依据（下一轮就能拿到）**：若 (1) 修好后 worker 仍不退 ⇒ 说明 stop 没终结它，必须做 (2)；
若 (1) 修好后 worker 在下一轮 tick 内退出（管道断开触发 worker 主循环的 `break`）⇒ **(1) 就够了**，
因为 worker 的退出条件之一正是"通道关闭/写失败"（`m5_worker_main.cpp:198-201`）。

**这条同时解释了一个本会话反复出现的现象**：`WARNING: 1 worker process(es) survived the run`——
e2e 结束时服务停了，但**在那之前** worker 一直被 `disk list` 喂着，来不及自己退出。
### 3.153 2026-10-27 21:30：**判定实验的结果：(1) 不够，必须做 (2)**

修好"不问已停止的盘"（`cf79fce`）之后，在**重置过的客机**上跑同一场景：
```text
workers settled after 120 s (1 left)
```
⇒ **worker 依然不退**。按 §3.152 事先定好的判定规则：
**光让 `disk list` 停止提问不足以让 worker 退出，`disk stop` 本身没有终结它。**

**因此下一任要做 (2)：让 stop 真正关闭与 worker 的连接。**
- 位置：`stop_runtime_and_release_lock`（计划最后一步，`state_machine.cpp:341` 之后那个 action 的实现）。
- 机制（worker 侧已具备）：worker 主循环 `m5_worker_main.cpp:198-201` 在**读失败/写失败**时 `break`，
  然后 `runtime->cleanup()` 并退出。所以**关闭管道就是最轻的终止手段**——
  不需要新的 shutdown 协议，只需要**在 stop 结束时释放/关闭 worker 的连接对象**。
- **判定标准（同样可事先定好）**：修好后再跑同一场景，`workers settled after N s` 应 **N < 30 s**；
  若仍 120 s，说明连接对象被别的持有者引用着（例如观察缓存或 job object 的句柄），
  那就顺藤摸瓜找持有者，而不是再加超时。

**已回退 e2e**（工作树 0 改动）。**本会话到此为止，我没有在代码上做 (2)**——
它属于"改产品生命周期"的一刀，需要先把连接对象的所有持有者列清楚；
以当前余量做它是赌，不是推进。
### 3.154 2026-10-27 21:40：**修法有现成先例：`import` 就是靠"不持有 session"让 worker 退出的**

`service_worker_control.h:94-96` 的原文：
> `import` … `inspect_bundle` … **The session is not held afterwards either: an inspection leaves nothing to
> command, and the worker exits when the channel closes.**

⇒ **机制不需要新协议**：**释放 session（关闭通道）⇒ worker 主循环读失败 ⇒ `break` ⇒ `cleanup()` ⇒ 退出**
（`m5_worker_main.cpp:198-201`）。

**所以 stop 的修法就是照 import 的做法**：在 `stop_runtime_and_release_lock` 完成之后，
**不要继续持有该 profile 的 `ServiceWorkerSession`**——把它释放掉（与 import 相同的处置）。
- `service_core.cpp:6412` 那段 `case ActionKind::stop_runtime_and_release_lock:` 目前**只做耐久阶段推进**
  （`StopDurablePhase::runtime_stopped`），**没有任何释放 session 的动作**——这就是缺口。
- **判定标准（不变）**：修好后跑 §3.153 的场景，`workers settled after N s` 应 **N < 30 s**。

**这条让"难修"变成"照抄"**：同一文件里已有正确处置（import 不持有 session），stop 只是**没有照做**。
### 3.155 2026-10-27 21:50：**`shutdown()` 方法存在、文档写明、但全树零调用者**——缺陷就是这个

`service_worker_session.h` 里的声明与注释：
```cpp
// Asks the worker to terminate.  Dismantles nothing, so `cleanup` is still
// available afterwards -- the two are not synonyms.
[[nodiscard]] Result<void> shutdown(const OperationContext& context, …);
```
实现（`service_worker_session.cpp:307`）发的是 `m5::WorkerCommand::shutdown`。

**全树 grep 结果**：`.shutdown(` / `->shutdown(` / `held_sessions_.erase` / `erase_session` —— **0 个匹配**。

⇒ **服务从来没有请 worker 终止过**：worker 只在**通道关闭**时自己退出，
而持有 session 的容器（`service_worker_control.h:486` 的 `std::vector<std::pair<Uuid, unique_ptr<ServiceWorkerSession>>>`）
在 stop 之后**没有释放那一项**。两件事合起来就是"盘显示 Stopped、worker 一直活着"。

**修法（三行，位置明确）**：
1. 在 stop 的 `ActionKind::stop_runtime_and_release_lock` 处理处（`service_core.cpp:6412` 一带）：
   取到该 profile 的 session → `session->shutdown(context, operation_id)`（**这正是那个从未被调用的方法**）；
2. 随后**释放/擦除该 session**（`held_sessions_` 里那一项）——通道关闭是 worker 的第二重保险；
3. `cleanup()` 与 `shutdown()` **不是同义词**（文档明说），所以顺序上先 `cleanup`/设备退役、再 `shutdown`。

**判定标准不变**：`workers settled after N s` 应 **N < 30 s**。

**为什么这一条对验收是硬要求**：验收时一定会问"`disk stop` 之后内存释放了吗、设备退役了吗"。
现在的答案在客户端看是"释放了"（`mem=0/0`、`runtime=Stopped`），**在进程列表里是"没有"**。
### 3.156 2026-10-27 22:00：**补上 `shutdown()` 调用后仍然 121 s / 1 left**——所以问题在 worker 的退出路径，不在"没人请它走"

重置客机、同一场景，读数 **`workers settled after 121 s (1 left)`**（与修复前一致）。

⇒ 两种可能，**必须用 worker 的 StartTime 分辨**（下一轮一件小事）：
1. **存活的是主盘的 worker**（它在我的场景之前被 stop）：那说明 `confirm_removal` 里的 `shutdown` **没成功**
   （best-effort 被吞掉）——要读 `asked` 的错误码；
2. **存活的是别的 worker**：那说明我一直在看错的对象。

**我的判断失误（记录在案）**：`shutdown` 是 best-effort，**我把错误吞掉了却没有记录**——
这正是本会话反复出现的毛病（"吞掉证据"）。正确做法是**至少写一条服务日志**说明 shutdown 失败与否。

**下一轮的两步**：
1. 在 `confirm_removal` 里把 `asked` 的结果写成服务日志（成功/失败+code），
   并把 **worker 的进程 StartTime** 打进 e2e 的输出（分辨是哪个盘）；
2. 若 `asked` 失败且 code 表示"管道已关" ⇒ worker 其实已经走了，**存活的是另一个**；
   若 code 表示"超时" ⇒ worker 卡在 `remove_and_stop` 之后的处理里 ⇒ 去读 worker 侧 `cleanup()` 的路径。

**已回退 e2e**（工作树 0 改动）。**代码修复 `ec75b6d` 保留**（它是必要不充分的一步，且不劣化行为）。
### 3.157 2026-10-27 22:10：**回归确认：`shutdown` 那一刀没有破坏 stop**

重置客机后跑未插入任何场景的 `cli_e2e`：
```text
request 2cad3213ad794c65a9f0bd1a8c57604c
1 of 1 passed            （115 s）
```
⇒ `ec75b6d`（在 `confirm_removal` 里补 `shutdown` 调用）**没有劣化 stop 路径**，
`T1-05` 的 stop/restart/forget 全链仍然通过。

**当前状态（准确）**：
- **T4-03 完成**（三次客机读数 + 回归用例）；
- **`disk stop` 的 worker 存活问题未解决**：两刀（观察守卫 `cf79fce`、shutdown `ec75b6d`）都保留，
  均未使 `workers settled` 降到 30 s 以内，但**都没有引入回归**；
- **下一步的诊断已经写死**（§3.156）：把 `shutdown` 的返回值写进服务日志 + 在场景里打印 worker 的 `StartTime`。
### 3.158 2026-10-27 22:25：**两条新读数，两个真缺陷**（都有进程 id 与退出码）

场景（90 s 轮询 + 打印 worker 身份）的完整输出：
```text
  workers before the scenario: 0
  workers settled after 90 s (1 left)
    id=6196 started=10:37:33
  volatile stop exit=0
--- trace ---
02:37:32.269Z stop: worker asked to shut down and answered
02:37:33.701Z stop: worker asked to shut down and answered
```

**缺陷 A —— `--no-start` 的 create 会拉起 worker，而且没人让它退出。**
- 场景开始前 **0 个** worker；场景结束后剩下的那个 **`started=10:37:33`**，正是**这次 `--no-start` create 的时刻**。
- 而 `create_stopped` 的计划里**没有**启动动作（§3.150）⇒ **worker 是由 `create_and_verify_bundle`
  或 `publish_provisioning_profile` 顺带拉起的**，之后**没有任何人**请它退出（只有 stop 才调 `shutdown`）。
- ⇒ **§3.150 的推断（"worker 属于被 stop 的盘"）是错的**，现在有进程启动时间作证。

**缺陷 B —— 易失盘的 `disk stop` 竟然成功（`exit=0`）。**
- `--checkpoint-interval off` 的盘，`disk stop` **没有**要求 `--accept-volatile-data-loss`，**直接成功**。
- ⇒ **易失盘的数据丢失保护没有生效**（或该保护只在别的命令上生效）。这是**验收时一定会被问到**的一条。

**已回退 e2e**（工作树 0 改动）。两处修复都保留（`cf79fce`、`ec75b6d` + `8da16ed` 的 trace），它们**不劣化**任何行为。

**下一任的第一件事**：为**缺陷 B** 找到"需要 `--accept-volatile-data-loss`"的那处判断（grep `accept_volatile`），
确认它覆盖哪些命令；`disk stop` 显然不在其中。**B 比 A 更容易修，也更接近用户价值（数据丢失保护）。**
### 3.159 2026-10-27 22:35：**缺陷 B 确认到代码级：`--accept-volatile-data-loss` 从来没有被"要求"过**

全 `src/management` 里 `volatile` 的出现只有三类用途：
1. **解析与传输**：`cli_parser.cpp:461` 读标志、`ipc_codec.cpp:835/844` 校验并编码；
2. **持久化**：`service_core.cpp:960-961` 写进 durable stop 记录；
3. **一致性校验**：`profile_registration_codec.cpp:835-851` 检查"标志与 force 是否自相矛盾"。

**没有一处**是"**易失盘且未给标志 ⇒ 拒绝 stop**"。也没有任何 `volatile_data_loss_possible`
被**计算**出来的地方（它只被持久化与校验自洽性）。

⇒ **`disk stop` 对易失盘畅通无阻**（客机实测 `exit=0`），**而 CLI 的用法文本写着要这个标志**。
**这是一条"承诺了但没实现"的保护**——比缺功能更糟，因为用户会以为数据被保护了。

**修法（形状已定，下一任可做）**：
1. 在 stop 的**受理**处计算 `volatile_data_loss_possible =
   !profile.checkpoint.interval_enabled && !profile.checkpoint.last_confirmed_generation`
   （即"没有定时检查点，也从未成功检查点过"）；
2. 若 `possible && !accept_volatile_data_loss && !force` ⇒ **拒绝**，错误文本要说明"这个盘没有检查点"；
3. 补一条 `m5.cli` 或 orchestration 用例：**易失盘 stop 不带标志必须失败**。

**判定标准**：客机重跑 §3.158 的场景，`volatile stop exit` 必须 **≠ 0**，且输出里含 `volatile`。
### 3.160 2026-10-27 22:50：**易失盘保护生效了（exit=1），但"确认"这条路走不通**

客机读数（`8263d645…`）：
```text
  volatile stop without the flag: exit=1
```
✅ **缺陷 B 的保护生效**：不给 `--accept-volatile-data-loss` 的 stop **被拒绝**（这是我加的那段判断）。

❌ **但给了标志也停不掉**，客户端自己就失败了：
```text
the acknowledged stop failed: myrd-cli: the command could not be encoded for the service
```
原因在 `ipc_codec.cpp:835`：
```cpp
            if (value.force != value.accept_volatile_data_loss) {
```
⇒ **`force` 与 `accept_volatile_data_loss` 必须同为真或同为假**。而 `cli_parser.cpp:461` 只设 ack、不设 force
⇒ **只给 ack 的命令无法编码**，用户会看到"could not be encoded"而不是"参数不合法"。

**缺陷 C（新）**：**文档给出的确认方式不可用**。修法二选一：
1. 解析器在 `--accept-volatile-data-loss` 时**同时置 `force`**（一行，且语义上 force 是"明知有损仍停"）；
2. 或在 codec 放宽（`accept` 为真即可），但那会改变线格式的既有约束，**优先选 1**。

**验收判据（更新）**：`volatile stop without the flag: exit=1`（已达成）**且**
`volatile stop with the flag: exit=0`（**未达成**，卡在缺陷 C）。

**已回退 e2e**（工作树 0 改动）。代码修复 `ba9f3aa` 保留——它让保护**真的存在**了。
### 3.161 2026-10-27 23:00：**缺陷 C 已修**（确认方式现在可用）

`cli_parser.cpp`：`stop.force = stop.force || stop.accept_volatile_data_loss;`
——理由写进代码注释：**线格式要求两者一致**，而"承认易失丢数据"**本身就是强制**（该盘没有检查点）。
`--force` 仍保有自己的语义与标志。

**读数**：构建零错误、`cli 18/18`、`m1.cli 5/5`、`orchestration 25/25`。

**待客机验证（同一场景两条读数）**：
- `volatile stop without the flag: exit=1`（**已达成**，§3.160）；
- `volatile stop with the flag: exit=0`（**本次待验**）。
### 3.162 2026-10-27 23:15：**缺陷 B + C 关闭**（两条读数都达成，且已进 e2e）

客机（`9b6bd93f…`，**整个 `cli_e2e` 通过，115 s**）：
```text
  volatile stop without the flag: exit=1
  volatile stop with the flag: exit=0
```

⇒ **易失盘的数据丢失保护：存在、可被合法确认、并被 e2e 永久守住。**

**这一项走过的路（值得记住）**：
1. 早上我以为它"只差读数"；
2. 读数显示 `exit=0`（**保护根本没实现**）；
3. 补上保护（`ba9f3aa`）⇒ 无标志时 `exit=1` ✅；
4. 但**带标志也停不掉**（`could not be encoded`）⇒ 解析器补 `force`（`97b547d`）⇒ 两条都过 ✅。

**教训**：**一条"只差读数"的项，往往藏着两个缺陷**——一个在行为里，一个在**配套路径**里。
### 3.163 2026-10-27 23:25：**缺陷 A 的持有者确认：`register_bundle()`**

`service_worker_control.cpp:1133` 起：
```cpp
    case ActionKind::create_and_verify_bundle:
        return register_bundle(request, BundleEvidenceSource::created_and_verified);
    case ActionKind::inspect_bundle_read_only:
        return register_bundle(request, BundleEvidenceSource::read_only_inspection);
```
**同一个函数、两种证据来源**：
- `read_only_inspection`（import 走这条）**不持有 session**（§3.154 的文档原话）；
- `created_and_verified`（create 走这条）**持有**——于是在 `create_stopped` 之后**没有任何人释放**，
  worker 就此活下去（§3.158 用进程 id/StartTime 证明）。

**所以修法就在 `register_bundle()` 内部按 `BundleEvidenceSource` 分支**（或按 plan 是否还有 start 动作），
**与 import 的既有处置对齐**——这是本项第 3 次出现同一个形状：**把已有的正确处置复制到缺它的那条路径上**。

**判定标准（不变）**：跑 §3.158 的场景，`workers settled after N s` 应 **N < 30 s**。
### 3.164 2026-10-27 23:35：**回归确认：加线格式字段后 GUI 仍然通过**

今天给 `ServiceDiskView` 加了一个 u64（压缩计数）并改了客户端编解码，**GUI 走同一条线**——
所以跑了一次 GUI 工作流（客机，`67e272f6…`）：
```text
1 of 1 passed        27 s
```
⇒ **加字段没有破坏 GUI 客户端**（它是 T3 的验收对象，不能被"顺手改坏"）。

**顺带记一条命令事实**：GUI 用例名是 **`integration.m5.gui_workflow`**（不是 `integration.m5.gui`，
后者不存在；`m5.gui` 是单元例）。我第一次用错过滤词，5 秒就失败了——**过滤词写错时的输出是
"the filter matched no case"，不会误判为通过**，这点值得记住。
### 3.165 2026-10-27 23:45：缺陷 A 的**正确**修法（比我先前写的"改 `register_bundle` 分支"更准）

先前 §3.163 说在 `register_bundle()` 里按证据来源分支——**那样不对**：`create_and_start` 的计划是
`create_and_verify_bundle` → `publish_provisioning_profile` → **start 动作**，
两条路径都会经过 `publish_provisioning_profile`，而 start 需要那个 session。**在 `register_bundle` 里释放会打断 start。**

**正确的修法是"计划结束时按运行时状态决定留不留"**，而且判据**文件里已经有了**：
```cpp
bool asserts_live_worker(product::RuntimeState runtime) noexcept {
    return runtime == product::RuntimeState::running;
}
```
——这正是我在观察守卫（`cf79fce`）里用的那个谓词。

**做法（约 5 行）**：在**计划执行完毕**处（`execute_operation` 的成功路径之后），
对本次操作涉及的 profile 调用一个新增的
```cpp
void ServiceWorkerControl::release_session(const product::Uuid& profile_id);
```
（内部：从 `held_sessions_` 擦除该项；析构即关通道，worker 主循环读失败后自行退出），
**仅当 `!asserts_live_worker(profile.runtime)`**。

**为什么这个位置对**：
- start 完成后 `runtime == running` ⇒ **不释放**（worker 应当继续活着，供后续命令使用）；
- `create_stopped` 完成后 `runtime != running` ⇒ **释放** ⇒ worker 退出（缺陷 A 关闭）；
- `stop` 完成后同样成立（那一刀已经在 `confirm_removal` 里单独做过了，两处不冲突）。

**判定标准（不变）**：§3.158 场景的 `workers settled after N s` 应 **N < 30 s**。

### 3.166 2026-10-27 23:50（记录时间已不可考）：**宽过滤器 `m5` 把客机跑死了——不要再用它**

**经过**：
1. 我想做一次"M5 全体回归"，用了 `-Filter 'm5'`（**宽匹配**）；
2. 第一次 30 分钟内没结束（`-DeadlineSeconds` 默认 1800），提示"guest run 未被中断，可稍后用 `-RequestId` 收取"；
3. 用 `-RequestId e11ef7e7…` 收，**又等 30 分钟**仍未出现 `response.txt`；
4. 此时 **SSH 本身开始挂起**（一条 `Get-Process` 命令 600 秒不返回）⇒ **客机已经无响应**；
5. **断电重启**（`vmrun stop/start`）⇒ 15 秒后 SSH 恢复。

**结论与纪律（请写进习惯）**：
- ❌ **不要用 `-Filter 'm5'`**。它会选中**设备类重用例**（例如 `m5.production_execution_backend`），
  这类用例**一次就可能把客机的存储栈拖垮**——本会话早先已记录过一次同类退化。
- ✅ 只用**精确过滤器**：`^integration\.m5\.cli_e2e$`、`^integration\.m5\.gui_workflow$`、
  `^unit\.m5\.` 等（**逐个跑，别求"一次跑完"**）。
- ✅ 判据仍然是"**跑了哪几个、各多少秒**"，不是"跑了一大类"。
- **本次没有任何读数产出**（结果文件从未生成），因此**不能**据此说"M5 回归通过"或"回归失败"。

**代价**：约一小时客机时间与一次断电重启。**这是我今天最不该做的一次"图省事"。**
### 3.167 2026-10-27 23:55：**断电重启后客机健康，`cli_e2e` 再次通过（118 s）**

断电重启（§3.166）后：`Reset-M5Guest` → `^integration\.m5\.cli_e2e$`：
```text
request 3713c53b33cb4efc8afcb40dc712e164
1 of 1 passed        118 s
```
⇒ ① **客机从"存储栈被拖垮"状态恢复**（重启即可，无需重装）；
② **今天全部改动在新启动上仍然通过**（T4-03 断言、易失盘两条断言都在这个用例里）。

**这条同时是一次"断电恢复"的旁证**：重启前那次宽过滤器运行是**被硬断电终止**的，
而重启后 `Reset-M5Guest` + 首例通过 —— 与本会话 T1-09 的结论一致（**断电后能回到最近检查点**）。

**读数表（今天最后的可比数据）**：
| 用例 | 用时 | 结果 |
|---|---|---|
| `integration.m5.cli_e2e`（含易失盘 + 自动压缩断言） | 118 s | **PASS** |
| `integration.m5.gui_workflow` | 27 s | **PASS** |
| 本机十一套件 | — | **228/228** |
### 3.168 2026-10-28 00:05：**更正 + 缺陷 A 的修法已缩到"补一次调用"**

**先更正我之前写错的名字**：持有 session 的容器**不是** `held_sessions_`（我在 §3.163/§3.165 写错了，
按那个名字 grep 永远搜不到），实际是：

```cpp
// service_worker_control.h:486
std::vector<std::pair<product::Uuid, std::unique_ptr<ServiceWorkerSession>>> sessions_by_profile_;
```

**而且"释放"的方法早就有了**（`service_worker_control.cpp:240`）：
```cpp
void ServiceWorkerControl::forget_session(const product::Uuid& profile_id) noexcept {
    … if (found != sessions_by_profile_.end()) sessions_by_profile_.erase(found);
}
```
**它今天有两处调用**：
- `:394` —— `start()` 失败时（会话已不可用）；
- `:932` —— 移除动作之后，注释原文：*"The worker's runtime is gone, so the session no longer describes
  anything this adapter may command."*

⇒ **所以 stop 其实会释放 session**（§3.158 里那两个 `shutdown … answered` 也证明 stop 是通的）。
**缺陷 A 的缺口精确到一句话**：`register_bundle()` 在 `:383` 把 session 放进 `sessions_by_profile_`，
而**`create_stopped` 的计划在那里结束后再没有第二步**（`create_and_start` 才有后续 start 动作），
**于是没有任何人调用 `forget_session`**。

**修法（下一任 10 分钟可完成）**：
1. 把 `forget_session` 从私有提为可调用（或加一个公开的 `release_stopped(profile_id)` 转调它）；
2. **在 `create_stopped` 的计划结束处调用一次**——计划结束处是那个"`publish_provisioning_profile`
   之后若无后续动作"的位置；**不要**放在 `register_bundle` 里（§3.165 已说明那会打断 `create_and_start`）。

**判定标准（不变）**：§3.158 场景 `workers settled after N s` 应 **N < 30 s**。
### 3.169 2026-10-28 00:35：**修了"留住 session"，但那个 worker 30 分钟后仍在，而且杀不掉**

**读数（客机，SSH 非提权会话）**：
```text
myrd-m5-worker  Id=6764  StartTime=2026/10/27 11:53:37  CPU=0.078
taskkill /F /IM myrd-m5-worker.exe   →  拒绝访问（无法终止，PID 6764）
```
- **`11:53:37` 正是那次 `--no-start` create 的时刻**（§3.158 的记录方式一致）；
- **30 分钟后仍在**，CPU 几乎为 0（**不是忙等，是卡住**）；
- **非提权会话杀不掉**（worker 由提权服务拉起）—— 这是本会话早先记过的客机特性。

**因此缺陷 A 的性质要更新**：它**不只是"session 被留住了"**（那个我已在 `4278144` 里修掉：
`create_stopped` 不再保留 session），而是 **worker 在某一步卡住、不能自行退出**。
`diagnostic_trace` 的存在理由写的就是这件事：**卡在驱动调用里的进程不会回答、不会退出**。

**下一任的第一步（顺序不能变）**：
1. **重启客机**（那个 worker 只能这样清掉）；
2. 用**带 StartTime 输出**的场景跑一次（§3.158 的写法），**同时**保留 `last-trace.log`；
3. 读 trace 里**那个 worker 自己的最后一组行**（`worker:` 系列），确认它停在哪个命令之后——
   若最后一行是 `worker: command received` 且再没有后续 ⇒ **卡在 `create_bundle` 里**；
4. 只有拿到这一步，才谈得上修。

**本次运行的 e2e 也未完成**（挂起 30 分钟，被中止），**无读数产出**。
**不要把这次算作"缺陷 A 已修未验"**——它是"修了一处、又暴露下一处"。
### 3.170 2026-10-28 00:45：**孤儿 worker 的后果：它占着设备，导致**别的盘**`forget` 被拒**

那次挂起运行的 `service.log`（194 行，`C:\m5\cli-e2e-d121dac7…`）结尾：

```text
kind=2 command=disk.forget phase=lifecycle-action message=action-start: lifecycle-action
kind=7 command=disk.forget phase=noncancellable
  message=forget safety refused: profile_match=1 image_match=1 revision=3 expected_revision=3
           worker_absent=1 writer_lock_absent=1 device_absent=0 bundle_present=1 identity_match=1
kind=7 command=disk.forget phase=terminal message=terminal failure without a worker-action error; marker=unsafe
```

- **`device_absent=0`** ⇒ 那块盘的**设备还在**，于是 `forget` 按安全规则**拒绝**（`marker=unsafe`）。
- 而同一时刻客机上有一个 **30 分钟不退、CPU≈0、杀不掉**的 `myrd-m5-worker`（§3.169）——
  **它占着设备**，所以设备无法消失。

⇒ **缺陷 A 的真实后果不是"多一个进程"**：**它会阻止后续的磁盘清理**（`disk forget` 因 `device_absent=0` 被拒），
进而让整个 e2e 流程卡在这一步。**这是"一个泄漏的 worker 会污染无关操作"的实证**，也是验收时会被踩到的那类问题。

**下一任的入口（现在很具体）**：
1. 重启客机清掉孤儿 worker；
2. **只跑一次性场景**（不进 e2e）：`create --no-start --checkpoint-interval off` → 立刻看
   `worker: command received` 之后是否有后续 trace（`last-trace.log`），**定位它卡在 `create_bundle` 的哪一步**；
3. 修好后，`disk forget` 应当恢复（`device_absent=1`）——这可以作为**副判据**。
### 3.171 2026-10-28 01:00：**重置过的客机上，`--no-start` 的 create 以"已注册"失败——而名字是全新的**

**探针**（重启客机 + `Reset-M5Guest` + 名字与 bundle 都是新 GUID）：
```text
  probe create exit=0            ← 命令受理
  probe terminal=failed          ← 操作最终失败
```
服务日志（260 行）：
```text
kind=7 disk.create phase=publish-provisioning message=publish_registration refused:
  code=22024 step=profile_registration.publish
  detail=The profile name, image, or bundle is already registered
kind=7 disk.create phase=terminal message=terminal failure without a worker-action error; marker=register
```

⇒ **这是"干净客机 + 全新名字 + 全新 bundle"下的重复注册拒绝**。
**上一轮（§3.148）我把它归因于"上次尝试的残留"，现在这个归因被推翻了**——
至少在这条路径上，**没有任何残留可归**。

**最可疑的地方**：那条检查同时看 **name / image / bundle** 三者之一即可拒绝。
名字与 bundle 都唯一时，**只能是 `image` 撞了**。而 image id 若由**尺寸/几何**派生（而不是路径），
那么"**第二个同样大小的盘**"就会被判为重复——这能解释本节的一系列现象，
也能解释为什么同样的命令有时成功（当时没有同尺寸的盘）有时失败。

**下一任的第一步（一条命令）**：读**生成 image_id 的那个函数**（`profile::` 或 `persistence::` 里，
关键字 `image_uuid`/`image_id`），确认它是否**只依赖容量/几何**。
- 若是 ⇒ **这是产品缺陷**（第二个同尺寸盘永远建不出来），修法是让 image id 参与 bundle 路径或 UUID；
- 若否 ⇒ 回到 §3.169 的 worker 卡住那条线。

**本次探针已从 e2e 移除**（工作树 0 改动），**两条读数保留在本文档**。
### 3.172 2026-10-28 01:15：**"already registered" 的第四条判据是**挂载点**，而错误文本从不提它**

`profile_registration_store.cpp:1410-1419`（publish 的重复检查）：
```cpp
        if (equal_ignore_case(record.value().profile.display_name,
                              proposed.profile.display_name)
            || record.value().profile.image_uuid == proposed.profile.image_uuid
            || equal_ignore_case(record.value().profile.bundle_path,
                                 proposed.profile.bundle_path)
            || mount_targets_equal(record.value().profile, proposed.profile)) {
            return store_error<void>(L"profile_registration.publish",
                L"The profile name, image, or bundle is already registered");
        }
```

⇒ **判据有四条**：名字、image、bundle、**挂载点**；而错误文本只列了**三条**（名字/image/bundle）。
**挂载点冲突时，读者会被指向三个都不冲突的字段**——我今天就因此改了两轮错误的方向（§3.148、§3.171）。

**这解释了 §3.171 的探针失败**：探针用了 `--mount Z:`，而那次运行里**已经有一个 profile 占着 `Z:`**
（e2e 早前的某个磁盘，或上一次尝试留下的）。**"干净客机"是真的，但"Z: 空闲"是假的。**

**修法（很小，两处）**：
1. **错误文本补上挂载点**，例如
   `L"The profile name, image, bundle or mount target is already registered"`；
2. 更好的做法是**分别给出冲突的是哪一项**（四条判据分别返回不同 detail）——这让下一次诊断从"猜"变成"读"。

**下一任的第一步**：把 §3.171 的探针**换一个空闲盘符**（e2e 应用 `disk list` 里没出现的字母）再跑一次；
若那次 `terminal=succeeded`，则 **§3.171 的"重复注册"完全解释为挂载点冲突**，缺陷 A 的真实形态就只剩
"worker 不退出"（§3.169）这一条。
### 3.173 2026-10-28 01:30：**新错误文本生效了，但这次失败**不是**"已注册"——是 create 卡在 `running`**

客机（`382b3c0b…`，327 s）：
```text
FAIL: the operation did not reach a terminal state within 300 s (last state: running)
```
服务日志只留下一行：
```text
kind=7 disk.create phase=publish-provisioning message=publish_registration disposition=0 (the store reported no cause)
```

⇒ 两个新事实：
1. **把盘符换成 `Y:` 后，重复注册的拒绝没有再出现** ⇒ §3.172 的判断成立：**之前那次是挂载点冲突**；
2. **但 create 现在卡在 `running`，300 s 内没有进入终态**，而且日志里只有那条"store reported no cause"。
   - `disposition=0 (the store reported no cause)` 是**新的措辞**（来自我改的那条路径附近），
     **它说明这次不是重复注册**，而是**发布阶段返回了一个没有 message 的结果**；
   - 卡在 `running` 与 §3.169 的"worker 不退出"很可能是**同一件事的两面**：create 的发布步骤在等一个不会回来的 worker 回复。

**已回退探针**（工作树 0 改动）。

**下一任的第一步**：把 §3.173 与 §3.169 合成一条线——**create 的某一步在等 worker，而 worker 卡住了**。
用 `last-trace.log` 里**该 worker 的最后 20 行**定位（`worker: command received` 之后有无后续），
这与我 §3.169 写的第 3 步是同一条命令。

**注意**：`publish_registration disposition=0 (the store reported no cause)` 这个措辞本身**也应改**：
"store reported no cause" 对用户毫无意义——它应当说**是哪一步没有给出原因**（这是今天第三次遇到"错误文本不告诉人任何事"）。
### 3.174 2026-10-28 01:45：**缺陷 A 的根因：worker 卡死在"打开候选磁盘设备"——`enumerate_disk_candidates` 的最后一行就是它**

`last-trace.log`（1347 行）里那个 worker 自己的最后几行：
```text
04:31:29.187Z worker: command received
04:31:29.194Z M4Adapter::create_generation_zero: entered
04:31:29.303Z M4Adapter::create_generation_zero: wrote generation zero
04:31:29.303Z M3AimRuntime::start: provider open
04:31:29.313Z M3AimRuntime::start: provider start
04:31:29.313Z M3AimRuntime::start: discover adapter
04:31:29.320Z M3AimRuntime::start: look for an existing device
04:31:29.322Z enumerate_disk_candidates: begin
04:31:29.324Z SetupDiEnumDeviceInterfaces[0] took 0 ms
04:31:29.324Z SetupDiGetDeviceInterfaceDetailW took 0 ms
04:31:29.324Z enumerate_disk_candidates: opening the candidate     ← **到此为止，之后再无任何行**
```
⇒ **worker 卡在 `CreateFileW`（打开某个候选磁盘设备）里，永不返回**。于是：
- create 永不进入终态（§3.173 的 `running`）；
- worker 永不退出、CPU≈0、非提权杀不掉（§3.169）；
- 它占着设备，导致别的盘 `forget` 被拒（§3.170，`device_absent=0`）。

**这三条症状是同一个根因**：**打开候选设备会在"设备退役窗口"里永久阻塞**——
这正是 `docs/validation/m5-t1-03-worker-job.md` §4.15.57 记录过的 11 秒阻塞的**更坏形态**（这里是永不返回）。

**为什么我加的 120 s 上界没救回来**：`enumerate_disk_candidates_within` 把调用放到**分离线程**里并超时返回，
**但那个线程永远卡在内核里**；worker 之后的 `cleanup()` 需要释放设备，于是**进程也无法干净退出**。
⇒ **上界只救了"调用者"，没救"被卡住的线程"。**

**修法方向（AGENTS.md §2.1 已写）**：**不要靠打开磁盘设备来问盘号**。
卷侧 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` 能给出盘号而不碰磁盘设备；
`M3AimRuntime::start` 这一步的 "look for an existing device" 应当改成**按卷 extents 匹配**（T1-09 已经用过同样的思路）。

**判定标准（合成一条）**：`create --no-start` 后 30 s 内 ① 操作进入终态、② 无残留 worker、③ 该盘 `forget` 的 `device_absent=1`。
### 3.175 2026-10-28 01:55：**更正 §3.174 的一半：create 路径并不"找已存在设备"，卡住的是**预创建基线**那次枚举**

`m3_windows_acceptance.cpp:312` 起：
```cpp
    auto existing = config_.attach_only
        ? find_existing_device(spec)
        : Result<std::optional<aim::DeviceId>>::success(std::nullopt);
```
⇒ **创建型运行时本来就不做 adopt**（那段长注释解释了原因，且已被实测验证）。所以 trace 里
`look for an existing device` 那一行只是**无条件打印的 trace**，紧随其后的枚举**不是它**。

**真正卡住的调用**（`:334`）：
```cpp
    std::vector<std::uint32_t> baseline;
    if (!adopting_existing && !config_.attach_only) {
        auto snapshot = aim::snapshot_physical_drive_numbers();   // ← 无上界
```
这是"**创建设备之前取一次全机盘号基线**"，用来让 `match_created_disk` 跳过 `existed_before`。
注释里已经记着：**这段枚举曾在设备退役窗口里被测量到阻塞 11 秒**，当时把它从"每次 start"移到"仅创建前一次"。
**但它仍然没有上界**，于是这次**永久阻塞**在 `CreateFileW`。

**修法（小，且与今天的上界经验一致）**：**给这次基线枚举加上界**；
**超时的语义应当是"基线为空"而不是"创建失败"**——因为基线的唯一用途是"跳过创建前就存在的盘"，
而没有基线时 `match_created_disk` 仍会用几何 + backing file 身份核对（那是更强的判据）。
代码里已经有"empty baseline 也在客机上测过可用"的注释（`:274`），**这条路径是被认可的**。

**判定标准**（同 §3.174）：`create --no-start` 后 30 s 内 ① 终态 ② 无残留 worker ③ `forget` 的 `device_absent=1`。
### 3.176 2026-10-28 02:05：**基线调用本来就有上界**——所以卡的是"被放弃的线程**持有锁**"

`physical_drive_locator.cpp:439`：
```cpp
Result<std::vector<std::uint32_t>> snapshot_physical_drive_numbers() {
    auto cand = enumerate_disk_candidates_within(kEnumerateTimeout);   // 120 s 上界
```
⇒ **它会在 120 s 内返回**（超时返回 error）。所以 §3.175 说"没有上界"是错的；`kEnumerateTimeout` 现在就是 120 s。

**那为什么 create 到 300 s 还没终态？** 只剩一个解释：**包装器返回后，调用者卡在了别的同步点上**——
最可能是**那个被放弃的分离线程仍然持有某把锁**（枚举若在持有 provider/device 互斥量的情况下进入 `CreateFileW`，
那么"放弃线程"就等于"放弃了锁"）。

**这正是"以分离线程加界"这种做法的固有风险**，而我今天早些时候在 §3.174 的注释里还把它当作解决方案的一部分。

**下一任的第一步（一条 grep 就能定性）**：在 `enumerate_disk_candidates` 里找**加锁的位置**（`scoped_lock`/`mutex`），
确认它是否**跨越 `CreateFileW`**：
- 是 ⇒ 修法是把锁挪到打开之后（或每次打开只用局部状态），**让被放弃的线程不再持有任何锁**；
- 否 ⇒ 卡在 `wait_for_created_physical_drive` 或 `stop_provider()` 的等待里，去读那条路径。

**判定标准（不变）**：`create --no-start` 后 30 s 内 ① 终态 ② 无残留 worker ③ `device_absent=1`。
### 3.177 2026-10-28 02:15：**"被放弃线程持锁"这一条也被排除**——剩下的三个候选

`physical_drive_locator.cpp` 里的锁只有**包装器自己那三处**（`:394` 的 `std::mutex`、`:406` 的 `lock_guard`、
`:413` 的 `unique_lock`），**没有任何锁跨越 `CreateFileW`**。⇒ §3.176 的假设不成立。

**还有一个更硬的事实**：我加的 `enum_within: timeout fired` 那行**在那次 trace 里没有出现**，
而时间线是够的（trace 最后一行 04:31:29，e2e 失败在 04:36:30 ≈ +5 分钟，上界 120 s 早该到了）。
⇒ **包装器根本没有返回**，连"超时"这条路径都没走到。

**因此剩下的候选只有三个，按可能性排序**：
1. **`M3AimRuntime::start` 在调用基线之前已持有自己的锁**，而 `stop_provider()`（超时后的清理路径）
   或后续步骤需要同一把锁 ⇒ 表现为死锁（**最能解释"连 timeout 分支都没走完"**）；
2. **`wait_for_created_physical_drive` 在等一个永远不出现的盘**（它的 timeout 若 > 300 s 则与症状一致）；
3. **驱动内部全局锁**：`CreateFileW` 在驱动里阻塞时，进程内**其它触碰同一驱动器的线程**也一并阻塞
   ⇒ 整个 worker "冻结"，这与"CPU≈0、杀不掉"完全吻合。

**下一任的第一步（按 1 → 2 → 3 顺序，每一步都只要读一段代码）**：
1. 在 `M3AimRuntime::start` 里找**基线调用之前/之后**的锁作用域（关键字 `scoped_lock`/`mutex`）；
2. 找 `wait_for_created_physical_drive` 的 timeout 值（`kM0*` 常量）并与 300 s 比较；
3. 若 1、2 都不成立，则按候机 3 处理：**这是驱动层问题**，只能靠"**不要在 create 前枚举全机盘号**"
   来回避（AGENTS.md §2.1 的卷 extents 方案），而不是继续在这里找锁。

**判定标准（不变）**：`create --no-start` 后 30 s 内 ① 终态 ② 无残留 worker ③ `device_absent=1`。
### 3.178 2026-10-28 02:25：**候选 2 也排除**（设备等待只有 30 s）——现在需要一个 2 行的定位实验

- `M3AimRuntime::create(config, device_timeout = 30 s)`（`m3_windows_acceptance.h:81`），
  `runtime_adapter.cpp:380` 用**默认值**，且 `provider.request_timeout = 30 s`。
  ⇒ 基线之后的"等盘出现"最多 30 s，**不可能撑到 300 s**。
- `snapshot_physical_drive_numbers()` 有 120 s 上界。
- ⇒ **两者相加 ≤ 150 s**，而 e2e 等到 **300 s** 仍在 `running`；且 trace 里**没有** `enum_within: timeout fired`。
  **结论：worker 卡在"基线调用返回之前"的某处，而连超时分支都没走到。**

**下一轮的唯一实验（2 行 trace，不用改逻辑）**——在 `m3_windows_acceptance.cpp:334` 附近：
```cpp
    std::vector<std::uint32_t> baseline;
    if (!adopting_existing && !config_.attach_only) {
        diagnostic_trace("M3AimRuntime::start: baseline about to be taken");
        auto snapshot = aim::snapshot_physical_drive_numbers();
        diagnostic_trace("M3AimRuntime::start: baseline taken");
```
- 若只出现 **about to be taken** ⇒ 卡在 `snapshot_physical_drive_numbers()`（或它内部），
  那就去 `enumerate_disk_candidates_within` 的**入口**再加一行，逐层夹逼；
- 若两行都出现 ⇒ 卡在**基线之后**（`wait_for_created_physical_drive`，30 s 上界仍不成立 ⇒ 那里的实现有问题）。

**为什么这个实验值得做**：今天我已经排除了三个假设（上界太紧、没有上界、被放弃线程持锁），
**每一次排除都把范围缩小一半**，而这次只需两行 `diagnostic_trace` 就能再对半分。
**不要在这个状态下改逻辑**——`create` 是 T1/T4 全都经过的路径。
### 3.179 2026-10-28 02:45：**又一次运行：没有异常，但 trace 停在 `remove_owned_device: PhysicalDrive retired`**

同一条探针（`c25f5fc9…`，5.5 分钟，失败），但**这次没有 EXCEPTION 行** ⇒ **异常是间歇的**，不是每次都有。
这次 trace 的最后几行是：
```text
04:48:04.802Z enumerate_disk_candidates: end, 1 candidates, all steps returned
04:48:04.802Z enum_within: completed normally
04:48:04.805Z remove_owned_device: PhysicalDrive retired         ← 之后再无任何行
```
而进程一直活到 04:52:59（约 5 分钟）⇒ **卡在"设备已退役"之后**。

**这解释了两条旧读数**：
- `device_absent=0`（§3.170）：**设备退役动作执行了，但"消失"没有发生**——退役是请求，不是完成；
- worker 不退、CPU≈0（§3.169）：它在**等设备真正消失**。

**因此缺陷 A 的最可能形态**（现在三处证据一致）：**`create` 在创建镜像后回滚/退役设备，然后等待"设备已消失"，
而该等待没有上界或上界很大**；同时间歇性地会有一个异常（§3.178 抓到过一次）。

**下一任的第一步（按顺序，每步都小）**：
1. 读 `remove_owned_device` 之后那段代码：**它等的是什么条件、用什么上界**（关键字 `PhysicalDrive retired` 的下文，
   以及 `wait_for_...` / `device_absent` / `retire`）；
2. 那个等待**必须有上界**，且超时的语义应是"设备仍未消失"的**明确失败**（不是继续等）；
3. 再复跑同一条探针，看是"超时失败"还是"继续卡住"。

**本轮的两条 trace（`enum_within`、`baseline about to be taken/taken`）已经把范围压到"退役之后"这一段**——
它们本身**不是**修复，但每一次运行都会把位置钉得更准。

**已回退探针**（工作树 0 改动）。
### 3.180 2026-10-28 02:55：**"退役之后"的等待找到了：是 `provider_->stop()`，不是等设备消失**

`m3_windows_acceptance.cpp:836-843`：
```cpp
            *owned_device_, info_.virtual_bytes, remaining);
        if (!retired) return retired;
        diagnostic_trace("remove_owned_device: PhysicalDrive retired");   // ← 打印了，说明**等待已成功**
        physical_drive_restored_ = true;
    }
    return Result<void>::success();
}

Result<void> M3AimRuntime::stop_provider() {
    if (!provider_) return Result<void>::success();
    const auto stopped = provider_->stop(OperationContext{}, {});        // ← 卡在这里
```
⇒ **"PhysicalDrive retired" 打印 = 设备退役的等待成功了**（我上一轮把它读成"卡在这行之后"，方向对、对象错）。
真正的阻塞点是下一步：**`stop()` → `stop_provider()` → `provider_->stop(...)`**。

**这与"CPU≈0、杀不掉"完全吻合**：`provider_->stop` 是向 AIM 驱动/传输层发停止请求，**若它不返回，线程就永久停在内核等待里**。

**下一任的第一步（一条 grep）**：`runtime_adapter.cpp:379` 设的是
```cpp
    factory.provider.request_timeout = std::chrono::seconds(30);
```
⇒ **检查 `provider_->stop()` 是否真的用这个 `request_timeout`**。
- 若**不用** ⇒ 让它用（或给它自己的上界）——**这是唯一缺的东西**；
- 若**用了**却仍不返回 ⇒ 那是 transport 层不遵守超时（`worker_pipe`/`aim` 传输），继续往下追一层。

**判定标准（不变）**：`create --no-start` 后 30 s 内 ① 终态 ② 无残留 worker ③ `forget` 的 `device_absent=1`。

**注**：这已经是**第 6 次收窄**（`step=` 陈旧 → 挂载点判据 → 上界 → 锁 → 基线 → 异常 → 退役等待 → **provider stop**），
每一次都由一条读数或一段代码推动，**没有一次是猜对的**。
### 3.181 2026-10-28 03:05：**`stop_provider()` 全程完成**——所以卡点在它**之后**

客机（`d7897e53…`）的 `last-trace.log` 尾部：
```text
04:55:13.500Z remove_owned_device: PhysicalDrive retired
04:55:13.500Z stop_provider: asking the provider to stop
04:55:13.502Z stop_provider: provider stopped
04:55:13.502Z stop_provider: handles released
```
- **三行全出现** ⇒ `provider_->stop` 与四个 `reset()` **都在 2 毫秒内完成**；
- **没有 EXCEPTION 行**（本次运行没有抛异常）；
- trace 到此结束，而运行直到 **05:00:08** 才失败（**又过了 4.5 分钟，无任何 trace**）。

⇒ **卡点在 `stop_provider()` 返回之后**：即 `M3AimRuntime::stop()` 的剩余两行、或**调用者的错误处理**、
或**worker 把失败结果回给服务的那一步**、或**服务把操作推进终态的那一步**。

**下一任的第一步（三行 trace，逐个夹逼，位置已明确）**：
1. `M3AimRuntime::stop()` 末尾（`started_ = false;` 之后）加一行 `stop: returned`；
2. worker 的 create-bundle 动作在**返回错误结果之前**加一行（`m5_worker_runtime` 的 create 分支）；
3. 服务侧 `execute_operation` 的失败路径入口（`service_core.cpp:2916` 附近那段）加一行。

**为什么这样排**：trace 已经证明"停止是干净的"，所以剩下的三段（stop 之后 / worker 回包 / 服务推进终态）
**必须**用同样的三段式夹逼，而不是再猜。

**已回退探针**（工作树 0 改动）。

**注**：这是第 8 次收窄，且**每一次都把"不可能"的那一段排除掉**——今天的 trace 支出很小，收益是
"缺陷 A 从'worker 莫名不退'变成'三行已知代码' "。
### 3.182 2026-10-28 03:15：**m3 出口已到，服务入口未到**——缺口就是"worker 的答复"

客机（`48bb0b4d…`）的 `last-trace.log`：
```text
05:02:15.126Z stop_provider: provider stopped
05:02:15.128Z stop_provider: handles released
05:02:15.128Z M3AimRuntime::stop: returning      ← **m3 出口到了**
（之后无任何行；`service: execute_operation returned a failure` **从未出现**；也没有 EXCEPTION）
```
运行一直持续到 ~05:07。

⇒ **缺口被夹在两条 trace 之间，长度为一段**：**worker 在 m3 停止返回之后、把失败结果回给服务之前**卡住。
这一段的内容是 `m5_worker_runtime` 的 create-bundle 收尾（很可能是 **`runtime->cleanup()`**）

**下一任的第一步（两行 trace，位置精确）**——在 `m5_worker_main.cpp` 的主循环里：
```cpp
        diagnostic_trace("worker: command handling");            // 已有 "command received"
        auto reply = session.handle(request.value());
        diagnostic_trace("worker: command handled");            // ← 新增
        auto written = client.value()->write(...);
        diagnostic_trace("worker: reply written");               // ← 新增
```
- 只见 `command received`（无 `command handled`）⇒ **卡在 `session.handle()` 内部**，
  而 m3 stop 已返回 ⇒ 那就是 create-bundle 收尾里的 **`cleanup()`**（下一步把 trace 放进 `cleanup`）；
- 见 `command handled` 无 `reply written` ⇒ **卡在写管道**（那是另一类问题）。

**已回退探针**（工作树 0 改动）。

**现状**：今天 9 次收窄，缺陷 A 的定位区间从"整条 create 路径"缩到"**m3 stop 返回 → 服务收到失败**"这一段，
**下一次两行 trace 就能再缩到函数级**。
### 3.183 2026-10-28 03:25：**worker 对每一条命令都答复了（87/87/87）**——卡点在**服务内部**

客机（`8b48a1c4…`）统计：
```text
worker: command received = 87
worker: command handled  = 87
worker: reply written    = 87
```
⇒ **worker 收到、处理、回复三样完全对齐**；trace 末尾是两次 `M3AimRuntime::stop: returning`（05:09:24），
而运行继续到 **05:14**（5 分钟）——**期间 worker 什么都不缺**。

再看服务侧那条 bracket：**`service: execute_operation returned a failure` 从未出现**。

⇒ **结论（本次收窄的终点）**：**worker 已答复，服务没有从 `execute_operation` 返回**。
卡点**在服务进程内部**，位置是"拿到 worker 答复之后、`execute_operation` 返回之前"。

**下一任的第一步（两行 trace，位置在服务侧）**：
1. 在 `service_core.cpp` 调 worker 动作的地方**之前**：`service: asking the worker for an action`；
2. 在其**之后**：`service: worker action returned`。
- 前有后无 ⇒ **卡在发命令或读答复**（管道/超时）；
- 两者都有 ⇒ 卡在**动作结果处理**（发布/状态机），那里已有 `execute_operation failed` 与 durable 阶段可供比对。

**已回退探针**（工作树 0 改动）。

**缺陷 A 的定位区间（现在）**：**服务进程内，"worker 答复之后 → execute_operation 返回之前"**——
从"整条 create 路径"缩到这里，用了 **10 次收窄**，全部由 trace 或代码推动。
### 3.184 2026-10-28 03:35：**定位完成：卡在 `ServiceWorkerControl::execute()` 的一次调用上——而它的第一行是拿锁**

客机（`1c5e2ecf…`）读数：
```text
service: asking the worker for an action = 1
service: worker action returned            = 0        ← **从未返回**
```
而 worker 侧此前已证明"收/处理/回"三样齐全（87/87/87，§3.183）。

**关键结构事实**（`service_worker_control.cpp:1128` 起）：
```cpp
Result<WorkerActionResult> ServiceWorkerControl::execute(
    const WorkerActionRequest& request) {
    std::unique_lock lock(command_gate_);      // ← **第一行就是拿锁**
```
⇒ **`execute()` 卡在拿 `command_gate_` 上**，而不是卡在 worker 通信上。
而今天早些时候（§3.152）已确认：**维护 tick 每 15 秒会走 `observe_runtime`**，`find_session`/`observe_runtime`
也在这把锁的作用域内。**若某条路径持锁后等 worker 答复，而 worker 正忙在另一条命令上，
`execute()` 就会无限期等这把锁。**

**这是缺陷 A 的最终定位**（11 → 12 次收窄，全部由读数推动）：
**服务内 `command_gate_` 的争用/死锁**，不是驱动、不是枚举、不是管道。

**下一任的第一步（一次读 + 一次改）**：
1. 找出**所有**拿 `command_gate_` 的地方（`grep command_gate_ service_worker_control.cpp`），
   确认哪一处**在持锁期间等待 worker**（那处就是死锁的一半）；
2. 把那处的等待**移到锁外**（或改成"取快照 → 放锁 → 通信"），这与 §3.154 里 import 的做法同型；
3. 判据：`create --no-start` 后 30 s 内 ① 终态 ② 无残留 worker ③ `device_absent=1`。

**已回退探针**（工作树 0 改动）。**这条结论足以让下一个人直接开工，而不必重复今天的 12 次收窄。**
### 3.185 2026-10-28 03:45：`command_gate_` 的九个持有者，**其中一个已经在用 `try_to_lock`**——这是"重入"的痕迹

```
1068 reclaim_leftover           1137 execute              1387 execute_checkpoint
1394 reconcile_checkpoint       1556 （continue of 1387?） 1626 prove_forget_safe
1706 current_dirty_epoch        1733 try_to_lock（**有人已知重入风险**）  1766 （另一处）
```
⇒ **九个函数在持锁的情况下与 worker 通信**（设计上就该串行化，这一点没问题）。
**但 `execute()`（1137）的锁覆盖了整个长函数体**，而它的调用链里只要有一处**再次**拿 `command_gate_`
（`std::mutex` 不可重入），就是**自死锁**——表现正好是：
- `service: asking the worker for an action` 出现一次、`returned` 永不出现；
- worker 侧一切正常（§3.183 的 87/87/87）；
- 进程 CPU≈0、杀不掉、操作永不终态。

**下一任的第一步（一条 grep 就能确认/否证）**：
```powershell
# execute() 的函数体范围（1137 到下一个顶层函数），在里面找对下列函数的调用：
grep -n "reclaim_leftover\|execute_checkpoint\|reconcile_checkpoint\|prove_forget_safe\|current_dirty_epoch" src/management/service_worker_control.cpp
```
若有任何一处被 `execute()` 间接调用 ⇒ **自死锁成立**，修法是把那把锁改成 `std::recursive_mutex`
（最省事但掩盖设计问题），**或**把内层调用改成"已持锁"版本（更好，与 `reclaim_leftover_locked` 的既有命名一致）。

**今天的 12 次收窄到此结束**：从"worker 莫名不退"到"**一把不可重入的锁**"。**每一次都由读数或代码推动。**
### 3.186 2026-10-28 03:52：**不是自死锁**——那五个函数在本文件里只被"定义"，没有被"调用"

```text
1066 reclaim_leftover(...)          ← 定义
1121 session->reclaim_leftover(...) ← 这是 **会话对象**的方法，不是控制器的
1384 execute_checkpoint(...)        ← 定义
1391 reconcile_checkpoint(...)      ← 定义
1624 prove_forget_safe(...)         ← 定义
1704 current_dirty_epoch(...)       ← 定义
```
⇒ **本文件内没有任何一处"在持锁时再次拿 `command_gate_`"**（`1121` 调的是 `ServiceWorkerSession::reclaim_leftover`，
不是控制器同名函数）。**所以 §3.185 的自死锁假设不成立。**

**结论收窄为：跨线程争用**。
- **线程 A**（维护 tick / checkpoint 策略）持有 `command_gate_`，并**阻塞在某次与 worker 的通信**里；
- **线程 B**（`execute`，来自 create）**在等这把锁**（`asking=1, returned=0`）；
- worker 本身健康（§3.183 的 87/87/87），所以 A 那次通信**不是"worker 不回"，而是"这条路径没有上界"**。

**下一任的第一步（一条 grep + 一次读）**：
1. `grep -n "current_dirty_epoch\|prove_forget_safe" src/` —— 看 **tick/策略**那条线程是否调用其中之一；
2. 读那次调用的 **`OperationContext` 超时**：`require_session(profile_id, true)` 之后的
   `session->status(...)` / `exchange(...)` 是否都带 `request_timeout`。
   **若有一处用默认空 context ⇒ 那就是无上界的等待**，也是缺陷 A 的最后一块。

**这一条把"锁"从"可能有自死锁"改成了"确定是跨线程争用 + 一处可能无上界的等待"**——
**范围再次减半，仍然是靠一段代码。**
### 3.187 2026-10-28 03:58：**最后一块拼图的位置：`current_dirty_epoch` 会阻塞等锁，而 `observe_runtime` 不会**

`service_worker_control.cpp` 里两个"读"的对比（同一把锁）：

```cpp
Result<std::uint64_t> ServiceWorkerControl::current_dirty_epoch(
    const product::Uuid& profile_id) {
    std::unique_lock lock(command_gate_);                       // ← **阻塞等锁**
    auto session = require_session(profile_id, true);
    auto status = session.value()->require_runtime_observation(context());   // ← 之后的通信
```

```cpp
Result<std::optional<WorkerRuntimeObservation>>
ServiceWorkerControl::observe_runtime(const product::Uuid& profile_id) {
    // **Never waits for the channel.** … the gate is *tried*: if this adapter is mid-command,
    // nothing is known about the runtime right now, and `nullopt` says so.
    std::unique_lock lock(command_gate_, std::try_to_lock);     // ← **不等锁**
```

⇒ **设计意图很清楚**：读取路径**不该等**（T1-04 要求 create 期间 `disk list` 一秒内答复）。
`observe_runtime` 遵守了；**`current_dirty_epoch` 没有**——它阻塞地拿锁，然后再做一次 worker 通信。

**它被谁调用**：`service_core.cpp:1167`、`:2255`、`:3944`（读取/观测路径）。
⇒ **一次 `disk list` / 一次 tick 就可能阻塞在 `command_gate_` 上**；
而只要它持锁期间那次 `require_runtime_observation(context())` **没有上界**（或上界很大），
`execute()`（create）就会**一直等锁**——这正是客机上的 `asking=1, returned=0`。

**下一任的第一件事（两行代码级别）**：
1. 读 `context()` 的实现（`ServiceWorkerControl` 的成员），确认它**带 `request_timeout`**；
2. 若不带 ⇒ 给它一个（30 s）：**这是缺陷 A 的修复**；
3. 若带 ⇒ 把 `current_dirty_epoch` 也改成 `try_to_lock` + "未知"语义
   （`std::optional<uint64_t>`，与 `observe_runtime` 完全同型——**注释已经把理由写好了**）。

**两种修法都在同一个文件里、都有现成的先例**，而判据只有一条：
`create --no-start` 后 30 s 内 ① 终态 ② 无残留 worker ③ `device_absent=1`。
### 3.188 2026-10-28 04:10：**缺陷 A 修好了，三判据在客机上通过——但"stopped create 的 start"仍未完成**

**两个根因，不是一个。**

1. **`publish-provisioning` 仍在向一个已经答复并离开的 worker 要 live observation。**
   `create_stopped` 的 `register_bundle` 按设计丢弃 session，worker 回复后开始 cleanup；服务随后调
   `require_runtime_observation`。更硬的一处：失败分支写的是 `observed.error()`（作用域里另一个
   `Result`），不是 `runtime_observed.error()`——对成功 `Result` 调 `.error()` 抛
   `bad_optional_access`，异常被吞，操作永远停在 `running`。这解释了"worker 已退出、服务日志只有
   action-start、没有 kind=7"的全部读数。
2. **`current_dirty_epoch` 阻塞拿 `command_gate_` 再通信**（§3.187 的定位仍成立）。读路径因此会和
   正在执行的操作抢同一把锁。

**修法：**
- `create_stopped` / `import_profile` 的发布**直接用 worker 答复里的 `bundle_generation`**
  （新增 `WorkerLifecycleEvidence::bundle_generation`），不再二次观测一个已退出的进程。
- worker 的 `observe()` 报告**从 bundle 恢复出的 generation**（`observed_bundle_generation`），
  stopped runtime 也有值；运行中取 `max(runtime stats, bundle)`。没有它，stopped create 会发布 0，
  下一次 start 的 checkpoint boundary 对不上。
- `current_dirty_epoch` 与 `observe_runtime` 同型：`try_to_lock`、`find_session`（不 attach）、
  `Result<std::optional<uint64_t>>` 的"未知"语义；checkpoint 路径不把未知读成 clean，也不重写
  durable epoch——由 checkpoint 自己的 boundary 证据负责。
- 失败分支的 `observed.error()` → `runtime_observed.error()`。
- **成功 forget 也记录证据**：`forget safety proved: worker_absent=… device_absent=…`。否则
  "device_absent=1" 只在**拒绝**时出现在日志里，e2e 只能拿 exit code 当证据。

**客机读数（`integration.m5.cli_e2e`，Release，`Reset-M5Guest` 后，两次独立运行通过）：**
```text
probe create exit=0
probe terminal=succeeded                  ← Wait-ForTerminal 30（判据 ①）
no worker survived the stopped create     ← 判据 ②
stopped disk forgotten; device_absent=1 proved  ← 判据 ③
1 of 1 passed                              ← 120 s / 117 s
```
`cli_e2e` 里新增的断言就是这三行；`disk list` 不显示 profile 名，所以探针按挂载点 `Y:` 取 profile id。
易失盘用例改成"正常运行 create + 一次手工 checkpoint + 拒绝无标志 stop"：不这样它会在**脏 runtime 的
remove_mount** 上被拒（那是另一条守卫），而不是测到 volatile 策略。

**仍未完成（明确记下，别当成已修）：从 `--no-start` 创建的盘执行 `disk start` 仍失败。**
`DiskRuntimeOptions::resume_existing_create` 已把 create 模式放宽到"续跑已有 generation-zero
bundle"（本地 `unit.m4.disk_runtime` 35/35，含两个新用例），客机上它越过了 `classify_bundle`，
但停在 `M3AimRuntime::start` 的 `match_created_disk`：

```text
located a disk that is not empty, so it is not the disk this runtime just created
```

stopped create 的 bundle 里已经有一个已配额的卷，start 却按 create 路径去找"空盘"。这是 Load Only
的 **start 半边**，留待后续；`--no-start` 的 create 本身满足三判据。

**纪律复盘：** 先按给定根因修 `current_dirty_epoch` **并没有让判据变绿**——真正卡住的是 publish 的
二次观测。两轮 trace 夹逼（"observing / returned"、"publishing registration"）把它钉在两行之间；
失败分支的变量名错误是读代码时发现的。**不要把"读起来像根因"当成"已经验证的根因"。**
### 3.189 2026-10-28 05:20：**T4-02 的单文件 VHDX 导出完成——fold writer 的输出位置真的参数化了**

**这不是"加一个命令"**（§3.124 的裁定）：`CompactionArtifactBuildRequest` 原来只会把 base
按 bundle 相对名写作并 rename 进 manifest，所以先给 writer 加了**绝对路径**的导出面：

1. `persistence/vhdx_writer`：`write_sparse_vhdx_to_path(context, destination, plan, source)`
   - **目标已存在 ⇒ 在任何字节写入前拒绝**（`GetFileAttributesW` + 最终 `MoveFileExW` 不覆盖）；
   - 写同目录的 `.partial-<pid>-<n>`，关闭后**重新以只读打开**，用
     `VhdxReader::create_from_standalone_file` 逐 payload 读回并比对 digest；
   - `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 发布；任何失败删除 partial。
   `VhdxReader` 新增 `create_from_standalone_file`：非 bundle capability 的独立文件也能读，
   调用方提供它刚观察到的 `WindowsFileIdentity`。
2. `persistence/compaction_coordinator`：`export_sparse_vhdx(...)` 复用
   `CompactionPayloadSource` + `plan_sparse_vhdx`，但**不取 `BundleWriterLock`**、不写 bundle 内
   任何名字——导出对 bundle 无副作用。
3. worker：`WorkerCommand::export_bundle = 17`；`WorkerMessage::export_path`（UTF-16，
   presence bit 13）；`DiskWorkerRuntime::export_bundle` 用 `recover_complete_image` 读 durable
   view，**running runtime 直接拒绝**；导出后 service 关闭会话，worker 退出。
4. service：`ServiceWorkerControl::export_bundle` 以 `adopt_existing` 启动 inspection worker；
   `disk export` 是**同步只读命令**（不建 operation），running 盘在 service 和 worker 两处都被拒。
5. CLI/IPC：`disk export <disk> --output <file.vhdx>`；`CommandId::disk_export = 21`；
   schema 2 字段（target / output）；授权 `owner_or_administrator`。

**客机读数（`integration.m5.cli_e2e`，Release，clean build 后，1 次；整例 133 s）：**
```text
--- T4-02: export the stopped disk to a standalone VHDX
  export exit=0
  exported 14680064 bytes
  generation unchanged at 5
  second export exit=1
1 of 1 passed
```
- **"VhdxReader 能读回"是导出成功的发布前条件**：写句柄关闭后重新只读打开，逐 payload 用
  `VhdxReader` 读回并比对 digest；任何一步失败都不会 rename 成目标名。所以 `export exit=0`
  本身已经包含"reader 读回了这个文件"（64 MiB 虚拟盘，输出 14,680,064 字节）。
- **generation 不变**：导出前后 `disk list` 的 `generation=5` 相同。
- **拒绝覆盖**：第二次 export `exit=1`，错误文本含 "already exists / refusing to overwrite"。

**边界（明确写下）：** 只支持**停止的盘**。running 盘有未 checkpoint 的 RAM 内容，导出的只是
durable view；把它当作"当前盘"会是一个安静的谎。

**仍未完成：** Load Only 的 start 半边（§3.188）。T4-04 的 release manifest 冻结尚未开始。
### 3.190 2026-10-28 05:40：**T4-04 release manifest 冻结——`accepted`**

- **干净构建**：Debug `--clean-first` 451/451、Release `--clean-first` 451/451；两者都在
  `/WX`（warnings-as-errors）下成功，所以"零 error/warning"是构建本身的结论，不是看日志得出的。
- **证据组**：`out/m5-evidence/m5-cli-e2e.results.json` 已换成 T4-02 导出那次客机通过
  （request `9da7393b…`，130.7 s，`exit=0`）；其余 readings 保留 2026-10-27 的全套客机结果。
- `tools/New-M5ReleaseManifest.ps1` → `out/m5-evidence/release-manifest.json`
  （4 binaries + 8 readings，`frozen_at 2026-10-27T15:37:45Z`）。
- `tools/Verify-M5Evidence.ps1 -EvidenceDirectory out/m5-evidence` → **`accepted`**（exit 0）；
  `-SelfTest` 的四个坏证据集全部被拒。
- 四个哈希（SHA-256，前 8 位）：`myrd-service 9ba40554`、`myrd-cli 17099c80`、
  `myrd-gui ec4cad28`、`myrd-m5-worker 1fdefed7`。

**这不是"T4-02 已全部完成"的声明。** Load Only 的 **start** 半边仍未完成（§3.188）；冻结的是
**当前构建**，一旦修 start，二进制哈希会变，必须重新冻结。T4-04 自身的判据（干净构建 + manifest +
validator `accepted`）已经满足。
### 3.191 2026-10-28 12:50：**Load Only 的 start 半边完成——stopped create 现在发布 ready + 卷身份**

**问题有两层：**
1. `--no-start` 的 create 只发布 `provisioning`，并把 `create_bundle` 答复里的卷身份丢掉了；之后的
   `disk start` 走 create 路径找"空盘"，M3 的 `match_created_disk` 拒绝非空的续跑盘。
2. 即使绕过第一层，profile 的 `confirmed_generation` 若发布 0，第一次 checkpoint 会报
   `prior=1 confirmed=0 candidate=2`——bundle 的第一代是 1，不是 0。

**修法（全部在 service 侧，worker 不需要常驻）：**
- `register_bundle` 从 create 答复捕获两样东西：卷身份（`status.volume` →
  `VolumeRegistrationEvidence`）和**有效的 durable generation**（stopped create 的答复是 0，但
  它的 bundle 第一代是 1，所以发布 1）。
- `create_stopped` 计划追加两个 service-owned action：
  `record_created_volume_identity`（把卷身份写进 profile 与 reservation）→
  `publish_ready_profile`（lifecycle ready）。
- `publish_ready_profile` 对 `create_stopped` 允许"尚无 checkpoint"：第一次 checkpoint 属于之后的
  start。
- 修一处 reservation 约束：`bundle_verified` 阶段**不允许**带 volume（validator 明确拒绝），卷只在
  `volume_identity_recorded` 阶段挂上；原来那行会让 create 的 reservation 变成非法。
- `bundle_generation` 用外层 `created_bundle_generation` 跨 action 传递：worker 结果只在它自己那个
  action 的作用域里，service-owned 的 publish 动作读不到。

**客机读数（`integration.m5.cli_e2e`，连续两次通过，120 s / 122 s）：**
```text
probe create exit=0
probe terminal=succeeded
no worker survived the stopped create
--- T4-02c: start the disk that was created without starting
start terminal=succeeded
<uuid>  Y:  Ready  running  runtime=Running  activity=Idle  persistence=Clean
        health=Healthy  generation=1  mem=1048576/1073741824  serial=5A42-9D28
the started Load Only volume accepted a write
stopped disk forgotten; device_absent=1 proved
```
- 起点 `generation=1`；随后一次手工 checkpoint 推到 2，再 stop（volatile 盘用
  `--accept-volatile-data-loss`），最后 forget 的 `device_absent=1` 成立。
- 同一用例里的 VHDX 导出仍通过：`export exit=0`、14,680,064 B、generation 不变、二次导出
  `exit=1`。

**已知边界（记下，不冒充已闭合）：** service 在 create 与 publish 之间重启时，
`created_bundle_generation` 不跨重启持久化；代码里有 `create_stopped && 0 → 1` 的 fallback，
正常路径因此仍能发布正确 generation，但 reservation 里没有这个数。要彻底闭合需要把它加进
lifecycle reservation（durable schema 变更），留作后续。

**重新冻结：** 二进制哈希变了，manifest 必须重新生成；见 §3.192。
### 3.192 2026-10-28 12:55：**Load Only 之后重新冻结——仍然 `accepted`**

- 干净 Debug 451/451、Release 451/451（`/WX`，零 error/warning）；随后只改了
  `service_core.cpp` 的 generation fallback，`cmake --build` 为 `no work to do` 说明
  Release 已经在干净构建里包含它。
- `tools/New-M5ReleaseManifest.ps1` → `out/m5-evidence/release-manifest.json`
  （4 binaries + 8 readings，`frozen_at 2026-10-28T05:05:09Z`）。
- `tools/Verify-M5Evidence.ps1 -EvidenceDirectory out/m5-evidence` → **`accepted`**（exit 0）。
- 证据里的 `m5-cli-e2e.results.json` 已换成 Load Only start 的最后一次通过
  （request `5914d1d2…`，109 s，`exit=0`）。
- 新哈希（SHA-256，前 8 位）：`myrd-service ca8fd2ff`、`myrd-cli 5bb73c44`、
  `myrd-gui 6b740bd8`、`myrd-m5-worker 27783c91`。

**§3.190 的哈希已作废，以本节为准；§3.190 的冻结判据过程仍然有效。**
### 3.193 2026-10-28 13:10：**发现但未修：脏 runtime 的 force stop 仍会被 worker 拒绝**

- 一次 Load Only 运行里，探针盘写入一个文件、手工 checkpoint、再
  `disk stop --accept-volatile-data-loss`；worker 的 `remove_and_stop` 先 flush，仍看到
  dirty，返回 `runtime_dirty`，服务在 `remove_mount` 处报 23559 并 park 到 `reconciling`。
- **含义**：`--accept-volatile-data-loss` 在 service 的 stop 策略里生效（无标志会被拒），但
  **没有传到 worker 的 removal authorization**；worker 的注释写的是"dirty runtime 必须对
  durable truth 解决，不能被覆盖"，这与用户明确接受数据丢失的语义冲突。
- T4-02b 的易失盘 e2e 是在**干净 runtime**（没有写）上测的，所以没有踩到。两次带写的
  Load Only 运行也成功 stop 过，说明这是**时序相关**的：checkpoint 之后挂载的文件系统元数据
  可能再次写脏。
- **修法方向（未做）**：给 `WorkerRemovalAuthorization` 加 `accept_data_loss`；
  service 把 stop 的 `--accept-volatile-data-loss` 传进 action；worker 仅在授权时跳过 dirty 拒绝，
  未授权时保持拒绝。这是 durable protocol 的多副本改动，留作后续。
- **状态**：已记录，未修；不属于 Load Only start 的三判据。M5 虚拟机验收前需要决定是否先修。
- **2026-10-28 13:20：已修，见 §3.194。**
### 3.194 2026-10-28 13:20：**脏 runtime 的 force stop 已修——`accept_data_loss` 走完整条链**

**根因**：`--accept-volatile-data-loss` 只到 service 的 stop 策略和 `force_stop` 计划选择，
**没有进 worker 的 removal authorization**；worker 的 `remove_and_stop` 先 flush，仍 dirty 就返回
`runtime_dirty`，service 在 `remove_mount` 处报 23559 并 park 到 `reconciling`。

**修法（一处新增字段，四处消费）：**
- `WorkerRemovalAuthorization` 增加 `bool accept_data_loss{}`（worker 私有协议，第四个 bool）；
- 编解码：`write_body`/`parse_body` 的 removal 块各加一个显式 bool（不是打包位）；
- `ServiceWorkerControl::remove_mount`：
  `authorization.accept_data_loss = (request.command == ManagementCommand::force_stop);`
  ——`force_stop` 只有在 `force` 与 acknowledgement 一致时才会被 service 选中；
- `DiskWorkerRuntime::remove_and_stop`：flush 后仍 dirty 时，**未授权**才返回
  `runtime_dirty`；授权时写 trace
  `remove_and_stop: dirty writes discarded under the caller's volatile-data-loss acknowledgement`
  并继续 cleanup。正常 stop 的 fail-closed 行为不变。
- 单测：`unit.m5.worker_protocol` 的 removal roundtrip 断言第四个 bool 独立往返；
  `unit.m5.service_worker_session` 通过。

**客机读数（`integration.m5.cli_e2e`，连续两次通过，138 s / 125 s）：**
```text
wrote data the forced stop must be allowed to discard
volatile stop without the flag: exit=1
volatile stop with the flag: exit=0
...
remove_and_stop: dirty writes discarded under the caller's volatile-data-loss acknowledgement
...
probe terminal=succeeded
start terminal=succeeded
stopped disk forgotten; device_absent=1 proved
```
易失盘现在**先 checkpoint、再写文件**制造真正的 dirty 状态；forced stop 的 trace 证明走的是
授权丢弃路径，而不是"碰巧干净"。
### 3.195 2026-10-28 13:25：**数据丢失授权之后重新冻结——仍然 `accepted`**

- 干净 Debug 451/451、Release 451/451（`/WX`，零 error/warning）。
- `tools/New-M5ReleaseManifest.ps1` → `out/m5-evidence/release-manifest.json`
  （4 binaries + 8 readings，`frozen_at 2026-10-28T05:24:56Z`）。
- `tools/Verify-M5Evidence.ps1 -EvidenceDirectory out/m5-evidence` → **`accepted`**（exit 0）。
- 证据里的 `m5-cli-e2e.results.json` 是数据丢失授权修复后的最后一次通过
  （request `83f33dcd…`，125 s，`exit=0`）。
- 新哈希（SHA-256，前 8 位）：`myrd-service 8f8f855d`、`myrd-cli ebdedbf6`、
  `myrd-gui 744a68f5`、`myrd-m5-worker 0f46b8a9`。

**§3.190 / §3.192 的哈希均已作废，以本节为准。**
### 3.196 2026-10-28 15:50：**M5 验收首跑：G2/G1/T4-01 通过，G3 失败在自动启动的挂载**

**全量 160 的首跑**（14:07–14:48）跑到尾部卡在 `unit.m2.harness_preflight`（0 字节、无进程推进）；
host 40 分钟 deadline 到期。根因不是产品：客机 agent 的请求队列里积压了旧请求，每次启动会把
`*.request.ps1` 依次重放，后面的请求（包括 M5 组）排在旧请求后面。清空队列后单例可跑。

**单例读数（当前冻结构建）：**

| 用例 | 结果 | 读数 |
|---|---|---|
| `integration.m5.cli_e2e` | **1/1 通过** | 请求 `9da83ac2…`，120 s |
| `integration.m5.gui_workflow` | **1/1 通过** | 请求 `3d533bf5…`，29 s |
| `integration.m5.install_uninstall` | **1/1 通过** | 请求 `7f22aba1…`，7.5 s |
| `integration.m5.powerloss_prepare` | **1/1 通过** | 请求 `bb79da1c…`，50 s |
| `integration.m5.powerloss_verify` | **0/1 失败** | 请求 `5c2ff5a0…` |

**G3 失败读数：**
```text
service state: RUNNING
1660bdb6-767d-4f4f-81d1-f3c27d5662b9  T:  Ready  stopped/configured
    runtime=Stopped  activity=Idle  persistence=Dirty  health=Healthy
    generation=3  mem=0/0
```
`service.log`：
```text
kind=6 command=disk.start phase=auto-start message=auto-start: this profile was running when the service last had it
kind=2 command=disk.start phase=running message=action-result: create-runtime failed
kind=7 ... action=create-runtime code=21001 native=18 step=assign_drive_letter
    detail=set-mount failed first=87 second=18 guid=\\?\Volume{...
```
⇒ 服务自动起来了，恢复也到了 generation 3（dirty），但**自动启动没有把盘挂上**；盘停在
`Ready stopped/configured`。手动 `disk start` 未能复测——verify 清理后服务已不在。

**这次修的两处基础设施：**
1. `Reset-M5Guest` 原来只在 `operations` **已存在**时清空/重建；卸载把它删掉后，服务启动
   `ServiceOperationStore::open_existing` 报 `native=2`，SCM 表现为 `sc start` 1053。现在两个
   目录无论是否存在都保证建出来。
2. `test-m5-powerloss-{prepare,verify}.ps1` 的服务名从 `MyRamDiskPowerLoss` 改为
   `MyRamDiskService`：`service_main.cpp` 的 `kServiceName` 是硬编码的，SCM 用别的名字启动
   时 dispatcher 匹配不上。

**下一步：** 修自动启动的挂载。优先复用 §3.77 的"按 extents 采纳活动卷"；本次
`second=18`（`ERROR_NO_MORE_FILES`）说明候选卷可能尚不可挂载，或者枚举没找到它。修完重跑
`Test-M5PowerLoss.ps1 -SkipStage`，读数要求 `runtime=Running`、generation ≥ 提交值、标记字节
一致、worker 恰好 1 个。
### 3.197 2026-10-29 17:55：**G3 修复——自动启动把已被 Windows 挂到 `D:` 的卷重新挂回 `T:`**

**诊断读数**（在 `assign_drive_letter` 的失败行里逐层加，最后一条是决定性的）：
```text
enum=4 match=1 same=1 own=1@0+63 qerr=2 cand=1@0+63 state=fs=NTFS paths=D:\ dos=none
```
- `same=1`：记录 GUID 就是活动卷，extents 完全相同 ⇒ 不是身份过期，也不是"找错卷"；
- `state=fs=NTFS`：卷是活的 NTFS，不是 phantom/RAW；
- `paths=D:\`：**开机时 Windows 把这块卷自动挂到了 `D:`**（原来的 `T:` 映射随旧设备一起死了）。

**根因**：一个卷只能有一个盘符。`SetVolumeMountPointW(T:, 同一个卷)` 在卷已挂 `D:` 时返回
**87**（`ERROR_INVALID_PARAMETER`），所以"按 extents 采纳活动卷"也救不了——它找的就是对的卷。

**修法**（`Win32VolumeAdapter::assign_drive_letter`）：`GetVolumeNameForVolumeMountPointW(T:)`
失败后、Set 之前，用 `GetVolumePathNamesForVolumeNameW(owned.volume_guid)` 列出这个卷已有的
路径；对**盘符形式**且不是请求盘符的路径调用 `DeleteVolumeMountPointW` 释放，`Sleep(100)`，
再走原有的 Set/采纳逻辑。目录挂载点不动（可能是用户自己建的），`C:` 不可能命中（卷是匹配出
来的那块）。失败行同时保留 `enum/match/same/own/cand/state/paths/dos/released`，下一次同类
问题不需要再猜。

**客机读数（`Test-M5PowerLoss.ps1 -SkipStage` 连续两次通过）：**
```text
pass 1: T:  Ready running runtime=Running generation=4 (cut before: 3)
        marker 65 chars match; workers: 1; aim-backed disks: 0
pass 2: T:  Ready running runtime=Running generation=3 (cut before: 3)
        marker 65 chars match; workers: 1; aim-backed disks: 0
PASS integration.m5.powerloss
```
两次都是服务由 SCM 自起、verify 全程不启动任何东西；断电瞬间的脏写重放并提交成下一代。

**回归**：`integration.m5.cli_e2e` 1/1（118 s，请求 `ed66e94e…`）；本机 12 个纯逻辑套件全绿。

**重新冻结**：`frozen_at 2026-10-29T10:05:04Z`，4 binaries + 8 readings；只有 worker 变了
（`7fb9032b`），service `8f8f855d`、cli `ebdedbf6`、gui `744a68f5` 不变；validator **`accepted`**。### 3.198 2026-10-29 18:26：**全量回归按组补跑——158/160，两个失败都是已知的 `vswhere` 环境**

按组跑（每组前清空 `C:\m5\control\*.request.ps1`、结束 agent、`Reset-M5Guest`）：

| 组 | 结果 | 墙钟 |
|---|---|---|
| `unit.*`（96） | **96/96** | 271 s |
| `m4.*` + `m5.*`（14） | **14/14** | 107 s |
| `integration.m1` + `integration.m4`（4） | **4/4** | 5 s |
| `integration.m2`（4） | **4/4** | 16 s |
| `integration.m3`（31） | **31/31** | 95 s |
| `integration.m5` 非电源损失（4） | **4/4** | 137 s |
| `structural.*`（3） | **1/3** | 3 s |
| 电源损失三段（prepare + verify，2） | **2/2** | 52 s + 5 s |

- **158/160 通过**。两个失败是 `structural.m2.storage_independent_build` 和
  `structural.m5.execution_boundary`，stderr 都是
  `vswhere.exe not found`（`tools/Enter-MsvcEnvironment.ps1:6`）——**已知环境缺失**，不是产品
  回归；基线同样是 structural 1/3。
- 首跑（一个请求跑 160 个）卡在 `unit.m2.harness_preflight` 的原因也确认了：agent 每次启动会把
  `*.request.ps1` 队列全部重放，旧的全量请求排在前面。按组跑、每组清队列后全部正常。
- 电源损失三段这次是第 4 次通过：`prepare 1/1`、硬断电、`verify 1/1`、`PASS T1-09`。

**证据**：`out/m5-evidence` 现在有 **14 个 readings**（原 8 个 + unit/model/integration 分组/
structural），manifest 重新生成（二进制未变）：`frozen_at 2026-10-29T10:26:18Z`，
service `8f8f855d`、cli `ebdedbf6`、gui `744a68f5`、worker `7fb9032b`，validator **`accepted`**。
`m5-powerloss-prepare` 的 `results.json` 在客机上被写成了 357 个空格（harness 的这次写入坏了），
所以没有进证据集；prepare 的通过由 harness 的 `1 of 1 passed` 输出记录在这里和 §3.197，
verify 的 reading 是有效的。

**结论**：G1 + G2 + G3 全部通过，全量回归的产品用例全部通过；只剩两条 `vswhere` 环境失败。

### 3.199 2026-10-29 21:35：**STAB-01 单盘硬保护完成——第二块盘在准入阶段被拒**

**问题**：M5 是单盘产品，但此前没有任何执行点。用户可以创建第二块盘；两块同时 running 时，第二块的
拆卸不会真正移除设备（restart `no volume carries the recorded identity`、forget `device_absent=0`）。

**修法**（完整落点表见 `docs/validation/m5-stab-01-single-disk.md`）：
- 新错误码 `ServiceCoreError::single_disk_violation = 0x5609`（**追加**，旧编号不变）；
- `ServiceCore::refuse_if_another_disk_is_up()` 是唯一执行点：pending 队列里的 create/import、pending
  start，以及任何 `runtime != stopped` 的另一个 profile 都算“已有盘 up”；
- `disk create` 在派生 profile id 之后、reservation/admission 之前拒绝（step `single_disk_create`）；
  `disk start` 在 plan 为 allowed 时拒绝（step `single_disk_start`），幂等 `already_running` 不受影响；
- 拒绝写 `ServiceLogKind::admission_refused`：被拒命令没有 durable operation 记录，原因必须进日志；
- CLI 把 `0x5609` 映射为退出码 1（产品拒绝），不是 2（用法）或 3（不可达）。

**本机**：`m5.service_orchestration` 新增 4 例（create/已有盘运行、create/第一个仍 pending、start/已有盘
运行、start/无他盘的反向对照）；12 个受影响套件全绿（`m5.startup_sweep` 按设计拒绝在开发机跑）。

**客机**（`Reset-M5Guest` 后，Release，`cli_e2e` 连续两次 109 s / 108 s）：

```text
--- STAB-01: a second disk is refused while the first one is running
  second create on T: exit=1 in 13 ms
  error: M5 is a single-disk product: only one disk may be up at a time. ...
  command: disk.create
  step: service_core.single_disk_create
  code: 0x5609 domain 8
PASS STAB-01: second create refused (code 0x5609), first disk unchanged (generation=2 mount=U: workers=1)
  first disk forgotten; device_absent=1 proved
PASS integration.m5.cli_e2e
```

第二次运行 16 ms，同一组 PASS 行。期间有一次**假失败**：断言用了 `-match`（PowerShell 默认不区分
大小写），`disk.list: completed` 里的 `t:` 命中了 `T:`；拒绝本身当时就是正确的（16 ms、0x5609），
断言改成 `-cmatch` 后通过。教训写进 validation 文档，不重复。

**未做**：G3 未重跑——自动启动走 `admit_auto_starts`（先发布 stopped、再 dispatch start），guard 逻辑上
通过；按“G3 已验证、不重做”的指示保留。历史遗留的两块 running 盘不会被自动拆除，guard 只阻止新的
create/start。

**发布影响**：产品二进制已变（service / cli 至少变化，worker 不变），`out/m5-evidence` 的 manifest 与
二进制不再对应；H1（STAB-01 / 02 / 04）全部完成后再重新冻结并跑 validator。

### 3.200 2026-10-29 23:15：**STAB-02 完成——预算耗尽可预期，顺带修掉“>64 MiB 盘无法创建”的四层缺陷**

**目标读数（客机，1 GiB 盘 / 256 MiB `--memory-hard-limit`，`tests/e2e/test-m5-memory-exhaustion.ps1 -Mode budget`）**：

```text
baseline:  generation=2  mem=6291456/268435456
write 320 MiB in 4 MiB chunks -> written_mib=256  write_failed=yes
           "由于 I/O 设备错误，无法运行此项请求。"（ERROR_IO_DEVICE，不是 ERROR_NOT_ENOUGH_MEMORY）
after:     generation=2  mem=268435456/268435456  runtime=Running persistence=Clean health=Healthy  workers=1
small write (23 B, 只改已提交块): succeeded
checkpoint -> succeeded; generation 2 -> 4; mem 回落 67108864/268435456
stop -> succeeded; service restart + disk start -> succeeded
after restart: generation=5  file_length=268435456  committed_prefix 4 MiB 逐字节一致
delete -> UNMAP -> mem=5505024/268435456 -> write_after_unmap=ok
cleanup forget=ok; PASS
```

**策略**（完整表在 `docs/validation/m5-stab-02-memory-exhaustion.md`）：耗尽点是**预算**而不是物理
RAM；automatic 预算 = min(物理/4, 容量)，所以正常使用不会把机器压到换页。需要新提交块的写失败，
只改已提交块的小写可能成功；checkpoint 仍完成；最多丢“最后一次提交之后”的写入（默认 60 s / 64 MiB）。

**途中发现的四个产品缺陷（全部阻塞 STAB-02，且都影响正常创建）**：

1. **MSR 分区号写死**：`provision_volume.ps1` 用 `-PartitionNumber 1` 给盘符；GPT 盘上分区 1 是
   16 MiB MSR，数据分区是 2。盘符挂到 MSR，数据卷永远没有盘符，创建在 `select_volume_for_disk`
   失败。客机时间线（`1:1@V:\`、`1:2@\\?\Volume{…}` 无盘符）留档。修法：用 `New-Partition`
   返回对象的 `PartitionNumber`；单测改为模拟 MSR 布局并钉住分区号。
2. **`verify_volume` 的 2 MiB 上限**：MSR 使数据分区比虚拟盘小约 16 MiB，`extent_bytes=520028160
   expected=536870912` 被拒。修法：上限 18 MiB（MSR + 对齐），GUID/所有权/长度检查不变。
3. **worker 的 `max_virtual_bytes` 写死 64 MiB**：`default_recovery_options()` 让任何更大的 bundle
   在 `record_bundle_identity` 被判 `slot_semantics;status=unsupported`。修法：用 worker 自己的
   `virtual_bytes`。
4. **内存预算从未到达 worker**：launch spec 不带 profile 的 memory policy，worker 永远开 1 GiB 默认
   store，`--memory-mode fixed --memory-hard-limit` 是惰性的。修法：launch spec 追加可选
   `ram_limit_bytes`（0 表示“无预算”，import 用），`make_launch_request` 从 profile / create intent
   物化，worker → adapter → `store_options_for`。同时 `record_bundle_identity` 现在把拒绝的 code/step
   和两个 slot 的 `safe_context` 写进错误，下次不必改代码取证。

**回归**：本机 17 个受影响套件全绿（provision script、worker private pipe、service worker control、
`m5.*` 模型、m4 checkpoint/recovery）；客机 `integration.m5.cli_e2e` **1/1 通过**（64 MiB 全流程 +
STAB-01 断言）。**注意**：修 #3/#4 时对 worker 的恢复上限按“2×脏集 + 64 MiB”推导——一次 256 MiB
脏集的 checkpoint 实测 journal 344 MiB；journal 随脏集线性增长这件事留给 STAB-03/STAB-11。

**未测**：物理 RAM 真的不够（fixed 预算大于机器提交上限）。automatic 预算 ≤ 1/4 RAM，正常路径到不了；
故意设 6–8 GiB 预算需要数分钟写入和巨量 journal，本轮没有做。不要把它说成已测。

**发布影响**：产品二进制（service/worker）与内嵌的 provisioning 资源都变了，`out/m5-evidence` 的
manifest 继续失效；H1（STAB-01/02/04）收尾统一重新冻结。

### 3.201 2026-10-30 00:10：**STAB-04 一键全量落地，但只到 PARTIAL——160 例全覆盖，155/2/3**

**一条命令**：`tools/m5vm/Invoke-M5FullSuite.ps1`（`-SkipStage` 复用已 stage 的树；`-Only` 调试）。
组：unit / model / integration.m1+m4 / m2 / m3 / m5 / structural / powerloss（prepare → vmrun 硬断电
→ 启动 → verify）。每组前经 agent 执行 `Reset-M5Guest` 并确认 `RESET`。

**第二次运行读数（客机，Release）**：

```text
M5 full suite: 160 cases, 155 passed, 2 skipped, 3 failed in 1065.9 s
  unit 96/0/0 231 s | model 14/0/1 103.4 s | m1m4 5/0/0 6.9 s | m2 4/0/0 17.8 s
  m3 31/0/0 85 s | m5 4/0/0 138 s | structural 1/2/0 5.2 s | powerloss 0/0/2 438.2 s
results: C:\tmp\m5-vm\full-suite\20261029-235529\results.json
```

**已落地**：`pending.id` 请求隔离（agent 只跑本次请求，旧队列不再重放）；`results.json` 临时文件 →
rename → 回读校验（宿主拒绝 0 字节/非 JSON/空结果）；`vswhere` 缺失 → `exit 77` → SKIP（ctest
`SKIP_RETURN_CODE 77`，guest runner 与宿主摘要单列）；powerloss 三段自动串起；总时长与逐用例
group/status/exit_code/seconds 都在 JSON 里。

**未达成验收（PARTIAL 的原因）**：

1. **`m5.production_execution_backend` 稳定失败**（model 组）。失败点是 reconcile 的完成态；探针显示
   同一 correlation 的直接 inspection 是 `verdict=2 mask=0`（可证明），而 worker 的 proof 是
   `outcome=0`（不可证明）。**不是 STAB-02 回归**：切到 `f933def`、`--clean-first` 重建 worker 与
   测试、同一客机重跑，同样失败。需要干净参考机再判；不要回滚已提交的 STAB-02 改动去“修”它。
2. **powerloss verify**：prepare 通过（服务安装为 auto、64 MiB 盘 `T:`、committed generation 3、
   `powerloss-state.txt` 已写、`PREPARED`）；硬断电重启后 `sc query` 返回 **1060（服务未安装）**，
   `Wait-ForService 300` 超时。状态文件跨断电还在，服务注册却没了。两次一键运行同形态。这是下一次
   接手的**第一件事**：先拿到“boot 后服务注册仍在”的读数（怀疑 SCM 注册在硬断电时未落盘，但未证实），
   在此之前 powerloss 不能算通过。
3. 还没有“连续两次结果一致”的比较。

**本轮踩到的三个工具坑（都修了）**：PS 5.1 `ConvertFrom-Json` 把顶层数组当**一个对象**——guest 的
回读校验把 96 条数成 1 条；宿主分组分配因此把 160 例看成 1 例，完整性检查形同虚设，两个用例
（`integration.shm`、`model.m3.checkpoint_crash`）第一次没跑；powerloss 抛错时驱动器直接终止，
前 7 组的 `results.json` 不会写出。修法分别是 `foreach` 枚举、显式补组、捕获并记录该组失败。

**发布影响**：只改 harness，产品二进制未变；manifest 仍待 H1（STAB-01/02 的产品改动）收尾时重新冻结。

### 3.202 2026-10-30 02:25：**STAB-04 收口——连续两次一键全绿，两个失败源都是真因，证据集重新冻结**

**连续两次（干净客机，一条命令，`-SkipStage`）**：

| | Run A | Run B |
|---|---|---|
| 用例 | **160** | **160** |
| passed / skipped / failed | **158 / 2 / 0** | **158 / 2 / 0** |
| 总时长 | **837.5 s** | **829.4 s** |
| 逐用例 name+status 差异 | — | **0** |

`structural.*` 两次都是 `SKIP exit=77`（无 VS / 无 `vswhere`）；powerloss 三段两次都是 2/2。

**失败源 1：`m5.production_execution_backend`——64 MiB 是恢复上限的下限，不是上限。**
该用例的盘只有 **8 MiB**；上一轮把 worker 的 `max_virtual_bytes` 改成“worker 自己的 `virtual_bytes`”，
把 8 MiB 盘的上限降到了 8 MiB，于是直接 inspection（硬编码 64 MiB）证明得了的 checkpoint，worker 的
`prove_recovery` 报 `ownership_unprovable`。修法：`max(64 MiB, virtual_bytes)`——旧常量本来就是下限，
小盘保持旧行为，大盘才提高（`src/host/m5_worker_runtime.cpp` `default_recovery_options()`）。单例与
两次全量的 model 组（15/15）都通过。

**失败源 2：powerloss verify——SCM 注册在硬断电时丢了。**
prepare 通过并安装了 auto 服务，断电重启后 `sc query` 返回 1060；同一时刻写的文件还在。对照实验：
空闲客机上同一 `myrd-service.exe` 建 auto 服务、60 s 后硬断电，服务**存活**。结论是全量套件负载下
Windows 注册表 lazy writer 在约 55 s 内没有把 SCM 数据库刷盘。修法：
`test-m5-powerloss-prepare.ps1` 在 `PREPARED` 之前对服务键调 `RegFlushKey`，强制 SYSTEM hive 落盘。
断电实验测产品恢复，不测 Windows 刷盘时机。修后 `Test-M5PowerLoss.ps1 -SkipStage` 三段 PASS，两次
全量的 powerloss 组 2/2。

**发布证据（本轮重新冻结）**：`tools/New-M5ReleaseManifest.ps1` → `tools/Verify-M5Evidence.ps1` =
**`accepted`**：

```text
frozen_at = 2026-10-29T18:23:10Z
service eba14b75   cli 2dd2a28d   gui 57b599e4   worker b3226f5e
4 binaries + 9 readings（unit 96 / model 15 / m1m4 5 / m2 4 / m3 31 / m5 4 /
structural 3[skip 2] / powerloss prepare 1 + verify 1），全部 failed=0
```

旧的分组/单例 readings 移到 `out/m5-evidence/archive-*`，没有删除。manifest 生成器同时修了一处：
`status=skip` 不再计入 `failed`（此前 structural 会显示 2 failed）。

**H1 完成**：STAB-01 / STAB-02 / STAB-04 全部 PASSED。下一步进入 **H2**（STAB-03 故障矩阵、
STAB-05 平台/驱动、STAB-06 升级/回滚、STAB-07 安全审计、STAB-08 发布工程）。

### 3.203 2026-10-30：**六项范围裁定落盘（只改文档，未改产品代码）**

用户在 2026-10-30 对发布前六个待决点作出裁定，已写进
`docs/superpowers/plans/2026-10-30-m5-release-tasklist.md` §0.1 / §7，tasklist 的 STAB 行同步：

| # | 裁定 | 对任务的影响 |
|---|---|---|
| 1 | **不允许** fixed 预算超过机器提交上限 | R-01 改为“创建时拒绝 + 可观测 + 实验入套件”；不做物理 RAM 耗尽实验 |
| 2 | **没有签名证书** | R-10 明确“未签名发布”，保留 SHA-256/可重复构建，发布说明写 SmartScreen 后果 |
| 3 | 卷标**直接不支持** | R-14 缩小为“入口拒绝并说明 + 文档 + 死代码隔离”，不做换路实现 |
| 4 | **只支持 Win11** | R-06 取消第二平台测试；R-18 移出 Win10；矩阵把 Win10/Server 标“不支持” |
| 5 | 回滚：**代码里没有** ⇒ 只支持重装 | R-08 用“重装”替代“回滚”，安装器/文档统一写“不支持回滚” |
| 6 | 簇大小**做** | R-17 拆成 R-17a（跨层实现，create-only）+ R-17b（测试路径） |

**裁定 5 的代码依据**：全树 `rollback|downgrade|previous version` 只命中事务性回滚
（`release_commit_reservation(rollback)`、stop 失败回滚、`rollback_metadata_preimage`）；
profile/bundle 没有格式版本字段，安装器也没有版本回退动作。所以可承诺的恢复路径只有“重装”。

**裁定 6 的代码依据**：旧文档“全树无格式化代码”已过时——`provision_volume.ps1` 在 create 的
`provision_new` 路径调用 `Format-Volume -FileSystem NTFS`。簇大小可通过 `Format-Volume -ClusterSize`
在 create 时生效，属于跨层加字段（CLI → command/IPC → intent/durable → profile → worker launch →
adapter → 脚本），不是从零写格式化引擎。

**本轮状态**：只改了文档；`src/` 与 `tests/` 已恢复到 `d8c2a87` 的内容。**下一次会话先做
`cmake --build out/build/msvc --config Release --clean-first`**——本轮收尾时有一次 clean build 被中止，
`out/build` 里可能留着与源码不一致的中间产物。第一条任务：**R-01**。

---

### 3.204 2026-10-30：**R-01 完成 —— M5 不 over-commit，且拒绝可自证**

**结论**：`R-01 / STAB-02R` **PASSED**。按裁定 1，fixed 预算超过机器提交上限的创建请求在**准入阶段**
被拒绝；预算拒绝在 `disk show` 里可见；耗尽实验成为一条可重跑的命令。证据：
`docs/validation/m5-stab-02b-memory-limits.md`。

**产品改动（4 层 + 1 条可观测链）**

- `PhysicalMemorySnapshot` 追加 `commit_limit_bytes`；`SystemPhysicalMemoryQuery` 用
  `MEMORYSTATUSEX::ullTotalPageFile`（= 物理内存 + 页面文件）填充。**不持久化**：这是准入时刻的事实，
  每次 create 重新读；冻进 durable record 会把“机器改配置”变成“请求变了”。
- 新稳定错误码 `ServiceCoreError::memory_budget_exceeds_commit_limit = 0x560A`（追加，旧编号不动）；
  CLI 映射到退出码 1（产品拒绝）。
- `ServiceCore::refuse_over_commit_budget()`：只在 `fixed` 且请求 > 上限时拒绝；`commit_limit == 0`
  **fail-closed**（拒绝而不是放行）。调用点在 create 的 `memory_.query()` 之后、intent/reservation/
  admission 之前；日志 `kind=admission_refused, phase=memory-budget`，消息带
  `requested=` / `commit_limit=`。
- `hard_limit_rejections` 全链路：store stats → `DiskMaintenanceStats` → worker 线格式（追加 u64）→
  `WorkerRuntimeObservation` → `ServiceProfileProjection` → 客户端线格式（追加 u64）→
  `disk show` 的 `budget_rejections=`（0 不显示）。
- 实验入套件：`tests/CMakeLists.txt` 注册 `integration.m5.memory_exhaustion`（guest-only）；全量脚本
  新增 `-IncludeMemoryExhaustion` / `-Only memory` 组。**默认全量仍是 160 例**，`-Only memory` 是 1 例。
  `expected_cases` 改成按“本次实际选择的组”计数。

**本机读数**：`m5.service_orchestration` 新增 2 例（拒绝 + 等于上限放行）；`unit.m5.worker_protocol`、
`unit.m5.service_worker_control`、`m5.cli` 各加计数断言；13 个受影响套件 **13/13 绿**（`--clean-first` 后）。

**客机读数**（`myrd-vm`，Release，`Reset-M5Guest` 后）：

- `integration.m5.cli_e2e` 的 R-01 段：64 GiB/64 GiB 请求被拒，`code 0x560A`，消息含
  “M5 does not over-commit” 与服务自报提交上限 **9,125,646,336 B**（与独立读取
  `TotalVirtualMemorySize = 8,911,764 KB` 逐字节一致）；无 profile / operation / bundle / worker。
- `integration.m5.memory_exhaustion`（1 GiB 盘 / 256 MiB 预算 / 320 MiB 写）：
  写失败于 256 MiB（13.4 s，`0x80131501`）；`budget_rejections` 基线 **0** → 失败后 **43** →
  checkpoint 后 **228**；checkpoint 2→3；重启 generation **4**，文件 268,435,456 B，前 4 MiB 一致；
  重启后计数 **0**（runtime 计数，不 durable）；UNMAP 后写恢复；forget ok。
- 一条命令：`tools/m5vm/Invoke-M5FullSuite.ps1 -SkipStage -Only memory` → **1/1 passed，118 s，exit 0**。
- 同组回归：`-Only integration.m5` → `cli_e2e` / `checkpoint_inspection` / `gui_workflow` /
  `install_uninstall` **4/4，152.6 s**。

**踩到的坑（已修）**：`response_view.cpp` 的 `kProfileFixedBytes` 是手工维护的定长前缀，新增 u64 后
没更新，`m5.cli` / `m5.service_host` 双红（mount 长度从错误偏移读）；`decode_profile` 的位置初始化
也把新字段放到了 `checkpoints_since_compaction` 的位置。两处都修好后复跑全绿。

**manifest 状态**：产品二进制已改，`2026-10-29T18:23:10Z` 的冻结**已失效**；R-19 收尾时重新生成并
`Verify-M5Evidence.ps1` 接受。

**边界**：物理 RAM 真耗尽按裁定 1 不做、不支持；运行中改页面文件不在范围；`budget_rejections` 是
per-runtime 计数，重启归零（有意）。

---

### 3.205 2026-10-30：**R-04 BLOCKED —— P4 之后的重启恢复撞 `execution_not_bound`，operation 永久 running**

**结论**：`R-04 / STAB-03` **BLOCKED（产品缺陷）**。证据：`docs/validation/m5-stab-03c-process-kills.md`。

**做出来的东西（保留，下一轮直接用）**

- 产品里的确定性 P4 窗口：`m5_worker_runtime.cpp` 的 `pause_at_p4_for_testing()`，
  `MYRD_M5_TEST_PAUSE_AT=p4-commit` + `_FILE` / `_SECONDS` / `_ARM`。ARM 文件是必须的——
  `disk create` 有自己的第一次 checkpoint，服务启动即武装会把 create 冻住（实测 create 在
  `reconciling` 停满整个暂停）。
- `tests/e2e/test-m5-process-kills.ps1`（`integration.m5.process_kills`）：A 杀 worker、B 杀服务、
  在 P4 窗口内；断言 operation settle、`disk list` 延迟、worker 残留、重启后恢复代与数据。
- R-02 装置：`test-m5-p4p5-powercut-prepare.ps1`、`-verify.ps1`、
  `tools/m5vm/Test-M5P4P5PowerCut.ps1`（prepare → `vmrun stop hard` → boot → verify，支持
  `-Cycles N`）。**已写、未跑**。
- 全量脚本：`-IncludeFaultMatrix`（R-04，opt-in，默认不跑，避免已知红污染门禁）、
  `-IncludeMemoryExhaustion`（R-01）；`p4p5_*` 永不进默认选择（需要主机断电）。

**读数（R-04.A 前半成立）**：kill 后 `disk list` 14 ms、operation `reconciling`、worker 0、
`persistence=CommitIndeterminate`；服务日志 `scheduler: execute returned` 立刻出现。

**失败（后半）**：重启后的恢复重新执行 checkpoint，发布 `evidence_recorded` 后调用
`publish_checkpoint_epoch`，命中 `0x5C02 execution_not_bound`
（`step=service_worker_control.require_session`，`detail=No runtime is bound to this profile to adopt`）；
`execute_claimed_operation` 只写 `operation_failed` 日志，**不把 operation 终态化**，于是它永远
`running`（实测 ≥300 s），每次重启重复同一序列（手动重启复现）。

**根因**：P5 需要“产生 P4 证据的同一个已绑定 session”，而恢复路径临时拉起/采纳的 worker 不在
绑定表里。**修法方向**：P5 无绑定 session 时走 T1-06 的 `reconcile_checkpoint`（只读 inspection 证明
candidate）；或让恢复 session 在本次动作链路内可被 P5 复用；并检查 P4 之后失败时 operation 必须
终态化或 profile 标 needs-attention。

**因此未测**：R-04.B（杀服务）、R-04.C（连续 3 次硬断电）、R-02（P4/P5 定点断电，落在同一恢复路径）。
**下一步**：先修恢复路径，再把 R-04.A/B/C 与 R-02（`-Cycles 3`）跑成读数；R-03 可以并行做（独立于
这条路径）。

---

### 3.206 2026-10-30：**R-04.A PASSED —— P5 恢复缺陷已修；B/C 与 R-02/R-03 未测**

**结论**：`R-04.A`（P4 窗口内杀 worker）**三次独立客机运行 PASSED**；`STAB-03` 记为 **PARTIAL**。
证据：`docs/validation/m5-stab-03c-process-kills.md`。

**缺陷（R-04 自己暴露的）**：恢复一个已过 P4 的 checkpoint 时，`execute_operation` 用
`worker_.reconcile_checkpoint()` 做只读 inspection；inspection 回答完即关闭（没有绑定 session）。
随后 P5 的 `publish_checkpoint_epoch` 在 `require_session(..., true)` 上返回
**`0x5C02 execution_not_bound`**，失败只在 `execute_claimed_operation` 记一条日志，**operation 永远
`running`**（实测 ≥300 s，每次服务重启复现）。

**修复（`src/management/service_core.cpp`）**：P5 的发布体抽成局部 `publish_epoch`；当
`recovering_checkpoint_from_m3`（或 in-flight `reconcile_checkpoint` 的 indeterminate 路径）为真时，
用 inspection 已证明的 `evidence.correlation` / `observed_dirty_epoch` 直接发布，
`post_commit_dirty=false`。正常路径（有绑定 session）不变，仍走 `publish_checkpoint_epoch`。

**客机读数（三次同形态）**：kill 后 `disk list` **11–25 ms**、operation `reconciling`、
`workers_left=0`、`persistence=CommitIndeterminate`；重启后 `operation_state_after_restart=succeeded`、
`start_exit=0`、`generation_after_recovery=3`（= P4 候选）、`Running`、`data_intact=yes`。

**未测及卡点**：

- **R-04.B**（杀服务）：用例在 A 之后 `disk stop` 第一块盘，operation 被准入但终态 **`failed`**
  （`b.part_a_stop_exit=0`、`b.part_a_stop_state=failed`）。这是另一个待查产品问题：一次成功的恢复
  之后，盘应当可以被正常停止。下一步读该次 `C:\m5\last-service.log` 的 `kind=7` 拿 code/step。
- **kill 实验后客机退化**：每次 kill 类用例跑完，PowerShell over SSH / `cmd` 会挂起，必须
  `vmrun stop … hard` + 启动才能跑下一次。所以 B/C 不能靠组间 `Reset-M5Guest` 串跑，需要“每组前
  硬重启”的驱动（或像 powerloss 那样放独立 host 驱动）。
- **R-04.C**：`tools/m5vm/Test-M5RepeatedPowerLoss.ps1` 已写（3 次 `Test-M5PowerLoss.ps1`，每轮
  reset），**未跑**。
- **R-02**：`test-m5-p4p5-powercut-prepare/verify.ps1` + `Test-M5P4P5PowerCut.ps1`（`-Cycles N`）
  已写，**未跑**；它落在同一条已修好的恢复路径上。
- **R-03**：`tests/e2e/test-m5-bundle-corruption.ps1`（manifest 截断/翻位、base 截断、journal 翻位，
  每次要求显式失败、bundle 字节不变、恢复后同盘可启动）已写，**未跑**；已注册为 opt-in `fault` 组。

**本机**：修复后 `m4.checkpoint_operation`、`m5.service_orchestration`、`m5.service_host`、
`m5.execution_scheduler`、`m5.service_maintenance`、`unit.m5.service_worker_control`、
`unit.m5.worker_protocol` **7/7 绿**。

**全量状态**：默认仍是 160 例（`-IncludeFaultMatrix` 才加 fault 组；`p4p5_*` 永不进默认）。本轮没有
重跑两次全量；上一次带 `-IncludeFaultMatrix` 的全量在 unit/model/m1m4/m2/m3/integration.m5/structural
全绿，fault 组按当时的旧代码红，powerloss 组在客机退化后未完成——不要把它当作发布级全量证据。

---

### 3.207 2026-10-30：**R-04.C PASSED（3 次连续硬断电）+ R-07 PASSED（版本/changelog）**

**R-04.C（连续 3 次硬断电）PASSED。** `tools/m5vm/Test-M5RepeatedPowerLoss.ps1 -SkipStage
-Cycles 3` → **exit 0、1037.7 s**。每轮 `Reset-M5Guest` → `Test-M5PowerLoss.ps1`（prepare →
`vmrun stop … hard` → 启动 → verify）：

| 轮 | prepare | 断电 | verify | verify 后 |
|---|---|---|---|---|
| 1 | 1/1 | hard | 1/1 | workers=0 services=0 disks=1 |
| 2 | 1/1 | hard（客机首次未答 SSH，按驱动约定再 power-cycle） | 1/1 | 同上 |
| 3 | 1/1 | hard | 1/1 | 同上 |

每轮判定来自 `PASS integration.m5.powerloss`（盘由 SCM 自动拉起、恢复到最后一提交代、marker 一致）。
`disks=1` 是宿主 NVMe。驱动的 “committed generation” 汇总字段打印 `?`（正则没看嵌套输出），不影响
判定——generation 由 verify 内部断言。

**R-07（语义化版本 + `--version` + changelog）PASSED。** `myrd-cli` / `myrd-service` / `myrd-gui`
都支持 `--version`，统一打印 `myrd::product::kProductVersion`；客机安装用例
（`integration.m5.install_uninstall`，1/1，11 s）读数：

```text
--- the service, the CLI and the GUI report the same version (R-07)
  cli: 1.0.0
  service: 1.0.0
  gui: 1.0.0
  consistent version: 1.0.0 (matches product_version.h)
```

实现在 `service_main.cpp` / `gui_main.cpp`（GUI 的 `--version` 在 `--self-check` 之前，不需要桌面）；
一致性检查加在安装用例里（只有那里拿得到“已安装的三个二进制”），不改变全量 160 例的构成。
`CHANGELOG.md` 已建立：1.0.0 条目 + 提交映射（单盘准入 `2a1f125`、预算 `fcd7da1`/`1dbc476`、
全量套件 `46308ec`…`3a13308`、裁定 `ae915d0`、R-01 `c8b3d63`、P4 装置 `b12b449`、P5 修复 `6393a89`）。
证据：`docs/validation/m5-stab-08-version.md`、`CHANGELOG.md`。

**R-04 现在只差 B**：A 与 C 已 PASSED；B 仍卡在“恢复后 `disk stop` 终态 failed”（需读 `kind=7`）
与“kill 实验后客机必须硬重启”。R-02/R-03 未跑。

---

### 3.208 2026-10-30：**R-04.B 独立成例后暴露产品缺陷 —— 杀服务后 worker 仍存活**

**做了什么**：把 `test-m5-process-kills.ps1` 参数化为 `-Part A|B`，并拆成两个 ctest：
`integration.m5.process_kills`（A）与 `integration.m5.service_kill`（B），各自一个全量组
（`fault` / `fault-service`）——一次 kill 实验之后客机存储栈需要硬重启，A/B 不能串在一次运行里。

**R-04.B 的实测读数**（`integration.m5.service_kill`，请求 `c131cb7c…`，81 s，FAIL）：

```text
READING: pause_marker=p4-commit generation=3 pid=1572
READING: candidate_generation=3
READING: killed_service_pid=3944
FAIL: b left 1 worker(s) behind after 60 s
```

**结论**：服务被强杀、worker 正停在 P4 的暂停里；60 秒后 **worker 仍然存活**。它继续持有 bundle
的独占写锁与 AIM 设备，会挡住下一次 start/forget——这正是 R-04 要防的“残留”。原因是服务给 worker
的 **job object 没有在服务进程死亡时把它带走**（kill-on-close 语义没有生效，或 worker 逃逸了 job）。
**这是 R-04.B 的阻塞点，必须修产品，不是改测试。**

**早先串跑版本里的另一个观察**（现在不阻塞）：A 恢复后对同一块盘 `disk stop`，operation 准入成功但
终态 `failed`。等 4.1 的 job-object 问题修好后，用独立用例读 `kind=7` 的 code/step 再定性。

**状态**：R-04.A PASSED、R-04.C PASSED、R-04.B BLOCKED（产品缺陷）。R-02/R-03 未跑。
证据：`docs/validation/m5-stab-03c-process-kills.md` §4。

---

### 3.209 2026-10-30：**R-04.B PASSED —— 修掉 job 生命周期与 sweep 升级策略**

**结论**：`R-04` **A/B/C 全部 PASSED**。B 的证据：`integration.m5.service_kill` 1/1，23 s。

**先纠正上一节的判断**：杀服务后 worker 存活**不是缺陷**，是设计（`windows_worker_job.h`：故意不设
`KILL_ON_JOB_CLOSE`，让 worker 能完成有序收尾；命名 job 就是给重启后的服务找它用的）。真正的缺陷在
下一层：

1. **job 随服务一起消失**：worker 通过 `PROC_THREAD_ATTRIBUTE_JOB_LIST` 进入 job，但不持有句柄；
   服务一死，最后一个句柄关闭、job 销毁；重启后的服务用同名新建一个空 job，`observe_orphans`
   按 `IsProcessInJob` 过滤 → 看不见任何孤儿 → sweep 无事可做，worker 继续持有 bundle 锁/设备。
   修复：launch 时把 job 句柄 `DuplicateHandle` 成可继承并加进 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`，
   worker 拿一个 **0 权限**句柄只为续命 job；父进程在 `CreateProcessW` 后立即关掉自己的副本。
2. **生产 sweep 不升级**：`WorkerSweepOptions::force_when_unreachable` 默认 false，服务死亡留下的
   孤儿按构造不可达（私有通道/启动秘密随服务消失），于是永远 left-alone。修复：
   `service_runtime.cpp` 显式设 `true`；sweep 先结束进程，再由**资源半边**（reclaimer）用 durable
   身份清盘符/设备——死掉的 worker 做不了的有序收尾由为它而设计的组件接走。

**客机读数**（`integration.m5.service_kill`，请求 `dda564f6…`，1/1 PASS）：

```text
READING: pause_marker=p4-commit generation=3 pid=6012
READING: killed_service_pid=5312
READING: workers_after_service_kill=1
READING: recovery.workers_left=0
READING: recovery.operation_state=succeeded
READING: recovery.start_exit=0
READING: recovery.generation=3
READING: recovery.data_intact=yes
PASS integration.m5.process_kills: the service was killed inside P4 and recovery reached the candidate generation
```

**顺带观察（不阻塞）**：`cleanup_forget=1`——恢复后的盘 stop 能过、随后的 forget 被状态机拒绝。
R-04.B 的验收（重启 reconcile 到确定状态、盘可启动、数据完整）三条都成立。该观察归入“恢复后 stop
终态”那条旧账，R-08 的升级/重装路径开始前应读 `kind=7` 定性。

**本机验证**：`service_core`/pipe/job 改动后全量构建通过；受影响的本机套件在改动前 7/7，绿；
kill 类用例只在客机跑。

**R-04 完成后的剩余**：R-02（P4/P5 定点断电，装置已写）、R-03（bundle 损坏/卷满/ACL，损坏用例
已写）、R-05、R-06、R-08、R-09、R-10、R-11…R-19。

---

### 3.210 2026-10-30：**R-03 第 1 部分半程 —— bundle 损坏矩阵被“恢复后 stop 失败”挡住**

**结论**：`integration.m5.bundle_corruption`（opt-in `fault` 组）**PARTIAL**。证据：
`docs/validation/m5-stab-03b-hostile-environment.md`。

**A（manifest 截断到 32 B）行为正确**：

```text
a.operation_state=failed    a.workers_left=0    a.bundle_unchanged=yes
a.restored_runtime=Running  a.marker_intact=yes a.stop_state=succeeded
```

明确失败、不挂死、不重写 bundle（逐文件长度+SHA-256 对比）；恢复原文件后同一块盘可启动、marker
完整、能正常 stop。这一条已经满足 R-03 对“截断”的验收口径。

**B（manifest 双槽各翻一字节）拒绝正确，但恢复后停不下来**：双槽损坏时 start 的 operation
`failed`（正确——单槽损坏时产品用完好槽启动也是正确回退，所以用例必须两个槽都翻）。恢复文件、
start 成功、marker 完整，然后：

```text
b.stop_state=failed
b.stop_output=... operation ...: running (phase admitting)
b.forced_stop_state=refused
```

`--force` 被状态机拒绝，盘保持 Running，客机随后退化（SSH/`cmd` 挂起），只能硬重启；强杀服务前
`service.log` 没有 `kind=7`，原因**未定性**。C（base 截断）/D（journal 头尾翻位）因此没跑。

**下一轮**：干净复现 B（stop 前不要杀服务进程），读 `kind=7` 与 `last-trace.log`，重点看 stop 的
最终 checkpoint 在“恢复后的持久化/gate”上是怎么失败的；定性后再跑 C/D。**R-03 的卷满与 ACL
两段还没写脚本**（计划见 validation 文档 §3）。

---

### 3.211 2026-10-30：**R-03 损坏矩阵 PASSED —— 上一轮的“stop 失败”是测试用旧备份**

**结论**：`integration.m5.bundle_corruption` **1/1，48 s**。证据：
`docs/validation/m5-stab-03b-hostile-environment.md` §1。

**四个用例全绿**（每个都是同一套判据：start 的 operation 终态 `failed`、无 worker、盘不 Running、
bundle 逐文件长度+SHA-256 不变、恢复后同盘可启动且 marker 完整、stop 成功）：

| 用例 | 损坏 | start 终态 | stop |
|---|---|---|---|
| A | `disk.rdm` 截断到 32 B | failed（112 ms） | succeeded |
| B | `disk.rdm` 双槽翻位（2048/6144） | failed（185 ms） | succeeded |
| C | `base-0000.vhdx` 4 MiB → 3 MiB | failed（214 ms） | succeeded |
| D | journal 头+尾翻位（64 / 10117120） | failed（88 ms） | succeeded |

`cleanup_forget=0`。

**上一轮“修复后 stop 失败”的真相**：用例只在开头做了一次 bundle 快照，之后每案恢复同一份旧快照。
每次成功 stop 都会推进 generation，第二次恢复把 bundle 倒回旧代，durable profile 却已在
`confirmed=5`，于是 stop 的 checkpoint boundary 拒绝：

```text
kind=7 command=disk.stop ... message=checkpoint boundary refused: ... prior=4 confirmed=5 candidate=5
kind=7 command=disk.stop ... code=22022 step=service_core.checkpoint_boundary
  detail=The worker checkpoint correlation is invalid
```

修法：**每案损坏前单独备份那个文件、只恢复它**（A/B 各备份 `disk.rdm`，C 备份 base，D 备份 journal）。

**观测缺口（已修）**：该用例漏了 `New-Item $workRoot`，而 `Start-Process` 的重定向不会创建目录，
所以 `service.log` 从未生成——`kind=7` 就是这样丢的。现在服务启动前显式建目录，失败时把
`kind=7` 打进 capture。这条同样适用于以后新写的 guest 用例。

**R-03 剩余**：bundle 卷满、ProgramData ACL 两段，测试脚本还没写。

---

### 3.212 2026-10-30：**R-03 ACL 段 PASSED —— 保护位一松，打开 profile 就明确拒绝**

**结论**：`integration.m5.hostile_acl` **1/1，87 s**。证据：
`docs/validation/m5-stab-03b-hostile-environment.md` §3。

**用例**：建盘 → 写 marker → checkpoint → stop；把
`C:\ProgramData\MyRamDisk\profiles\<id>` 的 DACL 保护位清掉；跑 `disk list` / `disk start`；按原
SDDL 恢复；同一块盘重新 start、读 marker、stop。

**读数**：

```text
acl.sddl_before   = ...D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)
acl.sddl_damaged  = ...D:AI(...SY)(...BA)(A;OICIID;FA;;;SY)(A;OICIID;FA;;;BA)
damaged.list_exit = 1   error: A profile directory ACL is not protected
                        step: profile_store.directory_acl   code: 0x4905 domain 2
damaged.start_exit= 1   （同上，command: disk.start）
damaged.workers_left = 0
acl.protected_after_restore = True
restored.runtime = Running   restored.marker_intact = yes   restored.stop_state = succeeded
cleanup_forget = 0
```

**测试细节**：恢复 SDDL 后 `Get-Acl .Sddl` 字符串与原始不同（Windows 补了 `AI`），所以断言用
`AreAccessRulesProtected` + “盘能起来”，不用字符串相等——否则会误报。

**R-03 剩余**：bundle 卷满一段（64 MiB VHD 挂 V:，填满后 create/checkpoint 明确失败、腾空间后恢复）。

---

### 3.213 2026-10-31：**R-03 卷满段 PASSED —— R-03 三段全部完成**

**结论**：`integration.m5.volume_full` **1/1，97 s**。证据：
`docs/validation/m5-stab-03b-hostile-environment.md` §3。

用例：diskpart 建 64 MiB 固定 VHD → 挂 V: → NTFS quick → 填充到只剩约 5 MiB → console service →
`disk create --bundle V:\bundle-first` → 必须明确失败 → 删填充文件 → 新路径 `V:\bundle-second`
重新 create → 写 marker → checkpoint → stop → forget → detach VHD。

**读数**：

```text
volume.free_after_fill = 5,394,432        （约 5 MiB）
full.create_state      = failed          （operation 终态明确失败）
full.workers_left      = 0
full.profiles_left     = 0               （没有注册 profile 残留）
full.bundle_dir_exists = True            （失败前建的目录仍在：残留观察）
volume.free_after_delete = 48,234,496
second.profile_id      = 444ee9b4-…
second.checkpoint_state= succeeded
second.marker_intact   = yes
second.stop_state      = succeeded
cleanup_forget         = 0
```

**判定**：卷满时 create 明确失败、不挂死、不静默成功、不注册 profile、不留 worker；腾出空间后同一
请求成功并完整走完 checkpoint/stop/forget。R-03 的三段（损坏、ACL、卷满）至此全部有客机读数。

**残留观察（不阻塞）**：失败的 create 在卷上留下 `V:\bundle-first` 目录。是否在失败路径清理，留给
R-19 前的清理项评估——它不影响磁盘状态，只影响卷上的垃圾文件。

**测试坑（已修）**：填充文件最初写在 C: 的 workRoot，V: 的可用空间不下降；必须写在 V: 上，且可用
空间用 `[System.IO.DriveInfo]::AvailableFreeSpace`（PSDrive 对象不保证刷新）。

**下一步**：R-02（`Test-M5P4P5PowerCut.ps1`，先 1 cycle 再 3 cycles）。

---

### 3.214 2026-11-01：**R-02 PASSED —— 三次独立定点断电，全部落到合法代**

**结论**：`R-02` 完成。证据：`docs/validation/m5-stab-03a-p4-p5-power-cut.md`。**STAB-03 至此全部
PASSED**（R-02 + R-03 三段 + R-04 A/B/C）。

**三次独立断电**（1 次早期驱动 + 2 次加固后），每次同一形态：

```text
survivors_after_cut=0
recovered_generation=2      (last committed; the P4 candidate 3 was not durable)
recovery_outcome=last_committed
checkpoint_operation_state=succeeded
start_exit=0  start_state=succeeded  disk_running=True
marker_present=False        (the write was inside the uncommitted P4 candidate)
residue=[]
```

**为什么不是“恢复到候选代”**：断电点在 P4 之后、P5 之前；P4 的提交没有 durable，所以恢复的正确结果
是丢弃候选、回到最后已提交代 2，并把 operation 收成 `succeeded`。三次都没有出现第三态。

**驱动加固（都是装置问题，不是产品问题）**：
1. 在 verdict 文本里找 `P4_REACHED` 找不到 → 从 `captures:` 目录读 `<case>.stdout.txt`；
2. verify 用例行首 `-and` 在 PS 5.1 解析失败；
3. `Join-Path 'T:\'` 在盘未出现时抛 `DriveNotFoundException`；
4. reset 的 scp 在 `$ErrorActionPreference='Stop'` 下抛 `NativeCommandError`，重试分支不执行；
5. cycle 之间只 reset 不够（P4 断电让 guest 存储栈/agent 退化）→ **每 cycle 前真断电重启**；
6. ssh 能答 ≠ scp 能答 → verify 前用 1 字节文件探测 scp 通道（≤180 s）。

**没计入的尝试**：一次 `-Cycles 3` 在 cycle 2 的 reset 卡住、一次在 reset 的 scp 上失败，都没有进入
实验；因此读数是三次**独立的单 cycle 调用**，不是一次三连。

**R-06 顺带**：安装器现在检查 `phdskmnt` 的**存在、运行状态与版本下限 1.2.28.93**（低于下限/未运行
/缺失都是明确拒绝）；缺/旧/停三个负分支在本客机不可复现（不允许卸载或降级驱动），已在
`docs/operations/m5-support-matrix.md` 注明；每次安装读数会打出 `phdskmnt x.y.z present and running
(floor 1.2.28.93)`。












---

### 3.215 2026-11-01：**STAB-06 PASSED —— R-07 + R-08 全部路径有客机读数**

**结论**：`STAB-06` 完成。证据：`docs/validation/m5-stab-06-upgrade.md`。

**R-08（`integration.m5.upgrade` 1/1，33 s）**：覆盖安装后同盘仍注册、可启动、marker 完整；保留数据
卸载（`install_dir_exists=False`、`data_dir_exists=True`）后重装同盘可启动；**运行中卸载**先停盘
（`processes_left=0`、`volume_gone=True`、记录保留），重装后数据完整。

**R-08 数据格式版本（`integration.m5.state_format` 1/1，30 s）**：把 durable 记录的 schema major 从 1
改成 32767，已安装服务拒绝启动（`refusal.service_running=False`），并在
`C:\ProgramData\MyRamDisk\logs\service.log` 写下：

```text
2026-11-01T05:56:22Z myrd-service: start failed:
  op=profile-registration.state.decode msg=Record schema major is unsupported native=0
```

恢复原始字节后服务恢复、`disk list` 正常。

**途中修掉的两个产品缺陷**：
1. **覆盖安装被旧服务占用文件**：`Copy-Item` 对运行中的 `myrd-service.exe` 报“文件正被使用”。安装器
   现在在复制**之前**停掉并删除旧服务（等 STOPPED，最多 120 s），复制后再注册启动。
2. **服务启动失败没有用户可读的消息**：服务模式没有 stderr，`sc query` 只有 1066。`failure_exit_code`
   现在把同一行追加到 `logs\service.log`（安装器已把该文件设为 Users 可读）。

**R-07 保持 PASSED**：`install_uninstall` 1/1，三处 `--version` 一致 1.0.0；驱动检查正路径
`phdskmnt 1.2.28.93 present and running (floor 1.2.28.93)` 也由这一次安装读数证明（R-06 正路径）。

**测试计数**：ctest 171 例；默认选择新增 `integration.m5.upgrade` 与 `integration.m5.state_format`
（默认门禁 160 → 162），两者都是 guest-only、`RUN_SERIAL`。

---

### 3.216 2026-11-01：安全审计完成（R-09 部分）、性能基线 PASSED（R-12）

**R-12 PASSED**（64 MiB 口径，`integration.m5.performance` 1/1）：create 107 ms、start 1453 ms、
worker RSS 20.6 MiB、写 32 MiB 40 ms（802 MiB/s）、checkpoint（32 MiB dirty）215 ms、stop 1130 ms、
forget 0。**256 MiB 口径的一次尝试把客机卡死**（写+checkpoint 后 SSH/cmd 无响应，只能硬重启），
已记录为待定性现象，未计入通过；R-13 soak 因此用小负载 + 频繁 checkpoint。
证据：`docs/validation/m5-stab-10-performance.md`。

**R-09 审计完成、7 个阻断项待修**（`docs/validation/m5-stab-07-security-audit.md`）：
- B1 worker 不认证服务端（服务死亡窗口内管道可被本地用户劫持，拿到秘密后驱动 SYSTEM worker）；
- B2 owner 未校验 + 安装器不设 owner（标准用户可预创建 ProgramData 目录并保持 owner）；
- B3 export 目的地可由 SYSTEM 在任意目录创建文件（含 junction 父目录）；
- B4 `logs\service.log` Users 可读且失败消息含完整 bundle 路径；
- B5 job 句柄用 `DUPLICATE_SAME_ACCESS`（全权，与“0 权限”注释不符）；
- B6 launch spec 读取上限 4096 字节（编码允许 128 KiB）；
- B7 公有管道可被抢注且客户端不校验服务端。

另有 8 条接受风险（worker/service 同信任域、breakaway、sweep 绑定比较不可达、proof 布尔等）与
11 条待执行运行期检查，均已写入审计文档。**下一步先修 B1–B7，再跑 §4 检查，R-09 才能 PASSED。**

**R-10 已 PASSED**：`/Brepro` 后同一 commit 的两次 `--clean-first` 构建四个二进制 SHA-256 完全一致；
发布流程文档 `docs/operations/m5-release.md` 已写（未签名 + SmartScreen 后果、manifest/verify、发布
说明要求）。

---

### 3.217 2026-11-01：**R-09 PASSED（7 个阻断项全修 + smoke 验证）、R-10 PASSED**

**R-09**：审计的 7 个阻断项全部落地并验证。关键点：B1 的 worker 侧服务端校验最初让 console 模式的
`disk create` 在 `create-bundle` 断管（`native=109`，worker 无日志）。A/B 关闭校验后 create 恢复；
把校验里的“两次 `GetTokenInformation` + 可变堆缓冲”改成**固定 256 字节缓冲 + 一次调用**后，校验
开启时 create 也恢复——问题在检查自身的实现细节，不在“worker 不该校验服务端”。修复后四支 smoke
全绿：`performance`、`install_uninstall`、`upgrade`、`bundle_corruption`（各 1/1）。

运行期检查已执行 3 条：安装后 `data_root.owner=BUILTIN\Administrators` 且 DACL 为
`O:BAG:...:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)`；`logs` 同一形状（Users 不再可读）；`C:\Program Files\MyRamDisk`
继承的 ACL 只给 `BUILTIN\Users` 读/执行（`0x1200a9`/`GXGR`），标准用户不能替换二进制。其余 8 条
（管道劫持、job DACL、worker token dump、junction export、超长路径、控制通道 DoS 等）需要标准用户
会话或攻击载荷，**明确标注未执行**。8 条接受风险已文档化。

**R-10**：`/Brepro` 后，同一 commit 两次 `--clean-first` 构建的四个二进制 SHA-256 完全一致
（cli `08D3837A…`、service `7E79BDEF…`、worker `55EEC4E6…`、gui `05D15424…`）；发布流程文档
`docs/operations/m5-release.md` 写完（未签名 + SmartScreen 后果、manifest/verify、发布说明六项、
发布前检查单）。

**客机 smoke 读数**（修复后）：
- `integration.m5.performance` 1/1（create/start/checkpoint/stop 全成功）；
- `integration.m5.install_uninstall` 1/1（含新加的 ACL 读回）；
- `integration.m5.upgrade` 1/1（覆盖/保留数据卸载/重装/脏卸载）；
- `integration.m5.bundle_corruption` 1/1（四种损坏的拒绝与恢复）。

---

### 3.218 2026-11-01：**R-14 收口 —— 选项名对齐 `--volume-label`**

发布任务清单写的是 `--volume-label`，而上一版只拒了短写 `--label`。现在两种拼写都在 CLI 入口被拒，
消息统一为 `--volume-label is not supported: this release does not set volume labels`，exit 2。
客机读数：`integration.m5.cli_e2e` 1/1，`label.exit=2`、`label.result=refused-by-name`；`m5.cli` 单测
覆盖两种拼写。wire/record 里的 `volume_label` 字段按“死代码隔离”保留，CLI 不可达，不做换路实现。
`docs/operations/m5-support-matrix.md` 的卷标一行已同步。

---

### 3.219 2026-11-01：**R-17a PASSED（可选簇大小，create-only）**

`disk create --cluster-size` 落地：4/8/16/32/64 KiB（2 的幂；`64KiB` 或裸字节 `65536`），缺省为
Windows 默认 4 KiB。簇大小是“格式化”的属性，因此**只走 create 的 operation intent，不写 profile
记录**；重启/恢复是 adopt 已有卷，没有需要重新施加的东西。四道闸门拒绝非法值：CLI → wire 字段 13
→ helper 参数检查 → provisioning 脚本。

客机读数（`integration.m5.cluster_size` 1/1、83 s）：

```text
READING: invalid.exit=2
READING: invalid.output=myrd-cli: --cluster-size must be a power of two between 4KiB and 64KiB
READING: create.state=succeeded
READING: volume.filesystem=NTFS
READING: volume.allocation_unit_bytes=65536
READING: volume.cluster_size_match=yes
READING: cleanup_forget=0
```

8 KiB 也单独跑过（`8192`），证明不是“恰好默认值”。另用临时 VHD 直接
`Format-Volume -AllocationUnitSize 8192` 读回 `au=8192`，确认 Windows 侧行为正常。

**这条“看起来只是加参数”的工作暴露了四个真实断路，全部已修**：

1. `write_lifecycle_intent` 不写 `volume_label`/`compaction_checkpoint_threshold`/
   `cluster_size_bytes` → 执行时“内存 intent vs 持久 intent”比较失败（`code=22022`）。
   现在三字段追加写入，读取端按剩余字节做版本兼容。
2. 嵌入的 `provision_volume.ps1` 不随源文件重建（CMake 只跟踪 `.rc`）→ helper 报
   `NamedParameterNotFound`。现在 `src/CMakeLists.txt` 给 `.rc` 声明
   `OBJECT_DEPENDS .../provision_volume.ps1`。
3. create 先注册 profile 再绑 worker，绑定用 record 版请求，record 里没有簇大小 → 值丢失。
   现在 `bind_worker` 从 `request.lifecycle_intent->cluster_size_bytes` 补上。
4. `m5_worker_main.cpp` 没把 `spec.cluster_size_bytes` 交给 `config` → 值到 worker 就停了
   （实测 spec=8192、卷=4096）。已补。

另修：`Format-Volume` 的参数名是 `-AllocationUnitSize`（不是 `-ClusterSize`）；三个 e2e 的
`Wait-ForTerminal` 会把 `disposition: completed` 误当成功状态，现在只匹配 `^operation <id>: <state>`。

**回归**：provisioning 改动影响所有 create，故重跑 `performance`（1/1、81 s）、
`install_uninstall`（1/1、9 s）、`cli_e2e`（1/1、118 s）。局部单测 `m5.cli` 与
`unit.m4.provision_script` 通过。文档：`docs/validation/m5-stab-15-cluster-size.md`；
支持矩阵与发布说明的“簇大小”行已改。

**仍剩**：R-17b、R-18、R-11、R-13、R-15、R-16、R-19。

---

### 3.220 2026-11-01：**R-17b PASSED（测试路径改为运行时根，非镜像布局可跑）**

任务清单 §8 的最后一项：测试在配置期把开发机绝对路径编进二进制
（`MYRD_M3_*_FIXTURE_PATH`、`MYRD_PRODUCT_EXE_PATH`），所以客机必须镜像开发机布局。

**做法**：`tests/support/test_paths.h` 在运行时用 `MYRD_TEST_ROOT` 把路径里
`tests\fixtures\` 或 `out\build\` 之后的部分重定位（未设置时原样返回，开发机行为不变）；
6 个文件 23 处宏引用改为 `MYRD_TEST_PATH(...)`/`MYRD_TEST_PATH_W(...)`；
`tools/m5vm/Invoke-M5Vm.ps1` 支持 `M5VM_GUEST_REPO`，并把 `tests.json` 的命令与
`WORKING_DIRECTORY` 从开发机路径改写到客机路径（**两种斜杠写法都要替换**——ctest 用正斜杠，
只替反斜杠会留下开发机路径，实测表现为 `MISSING ... C:/Users/...`）。

**客机读数**：

```text
staged to usr@myrd-vm:C:\m5repo\m5-lifecycle
filter '^(unit\.m3\.journal_format|unit\.m3\.persistence_types)$': 2 of 173 cases selected
2 of 2 passed
```

这两个用例必须读 `${CMAKE_SOURCE_DIR}/tests/fixtures/m3/*`；测试前已把旧镜像树删掉，
所以通过不是“旧树还在”。非镜像布局下的 e2e `integration.m5.install_uninstall` 也 1/1。
默认布局（不设 `M5VM_GUEST_REPO`）同样 2/2，历史行为不变。

**同时把 R-17a 的验收补齐**（同一轮）：`integration.m5.cluster_size` 现在测
4 件事：64 KiB create → `Get-Volume=65536` 与 `Win32_Volume.BlockSize=65536`；
stop/start 后仍 65536（adopt 不重格式化）；不传选项的 create → 4096；非法值 exit 2。
`fsutil` 输出是本地化文本（zh-CN），解析改用与语言无关的 `Win32_Volume.BlockSize`。
`disk show` 不显示簇大小，是“create-only、不进记录”的直接结果，已在验证文档里写成明确偏离。

**本地已知失败**（与本项无关，改动前后相同，`git stash` 对照）：
`integration.m4.product_composition`、`integration.m3.recovery_oracle`、
`model.m3.checkpoint_crash` 各一例未捕获异常。

**仍剩**：R-18、R-11、R-13、R-15、R-16、R-19。

---

### 3.221 2026-11-01：**R-18 PASSED（压缩/EFS/Defender/非系统盘）**

`integration.m5.compatibility` 在客机跑完整轮（1/1、100 s）。三件“用户实际会遇到的环境”都做了
create → 卷出现 → 写 1 MiB → checkpoint → stop 的整轮动作：

```text
READING: defender.realtime=True
READING: compressed.attribute=Directory, Compressed
READING: compressed.create=succeeded
READING: compressed.checkpoint=succeeded
READING: compressed.stop=succeeded
READING: efs.cipher_exit=0
READING: efs.attribute=Directory, Archive, Encrypted
READING: encrypted.create=succeeded
READING: encrypted.checkpoint=succeeded
READING: encrypted.stop=succeeded
READING: aim.disk_number=1
READING: aim.is_system=False
READING: aim.is_boot=False
READING: aim.logical_sector_bytes=512
READING: aim.physical_sector_bytes=512
READING: aim.bus_type=Fibre Channel
READING: system.disk_number=0
```

**结论**：压缩目录与 EFS 加密目录上的 bundle **都支持**；Defender 实时保护下整轮正常；AIM 盘是
非系统盘（系统盘 0、AIM 1）。没有证书的环境走 `efs.result=platform-unavailable` 分支（不挂死），
每个子动作都有 300 s 上限，`timeout` 即判失败，所以“不支持”只会是明确状态。

**未测**（矩阵不承诺）：4Kn 介质、BitLocker、SMB 路径。矩阵 §4 新增五行并引用本页读数；
验证文档 `docs/validation/m5-stab-16-compatibility.md`。

**仍剩**：R-11（非提权 GUI）、R-13（24h soak）、R-15（支持包）、R-16（一页文档）、R-19（演练）。

---

### 3.222 2026-11-01：**R-16 PASSED（一页支持说明 + 错误入口指向它）**

`docs/operations/m5-support.md`：六节——支持范围、已知限制（逐条给测试或“未测”）、数据丢失上限、
备份与导出、安装/卸载注意、出问题先看哪里。README 顶部加 “M5 support” 入口。

**错误指向它**：安装器 5 条驱动相关拒绝（未安装/安装不完整/版本读不出/过旧/未运行）都追加
`docs/operations/m5-support.md`（`support_pointer_count=5`）；产品 `driver_unavailable` 的消息改为
`phdskmnt is disabled or absent; the driver requirement and the other known limits are on docs/operations/m5-support.md`。

**读数**：消息改动后 `integration.m5.install_uninstall` 仍 1/1（16 s）。**未测**（写进验证文档）：
驱动缺失/过旧两个拒绝分支（客机不允许停用驱动）；`driver_unavailable` 的产品文本（唯一覆盖它的
`integration.m4.product_composition` 用例有既有未捕获异常，`git stash` 对照与本轮无关）。

逐条依据与文档评审表：`docs/validation/m5-stab-14-support-page.md`。

**仍剩**：R-15（支持包）、R-11（非提权 GUI）、R-13（24h soak）、R-19（演练）。

---

### 3.223 2026-11-01：**R-15 PASSED（支持包一条命令，默认脱敏）**

`tools/New-M5SupportBundle.ps1`：一条命令生成 zip，内容 environment/versions/driver/service/
disk list+show/operations/`failure.json`/profiles+`disk.rdm`/日志尾/`manifest.json`（每文件 SHA-256、
`failure`、`redacted`）。默认脱敏：计算机名 → `%COMPUTERNAME%`、用户 profile → `%USERPROFILE%`、
其余绝对路径 → `<path>`；`-IncludePaths` 才保留。脱敏在写盘前完成，哈希对已脱敏内容计算。

客机 `integration.m5.support_bundle` 1/1、74 s：先制造真实失败（bundle 目录非空 → create failed），
再让“陌生用户”跑工具：

```text
READING: failure.operation_state=failed
READING: tool.exit=0
READING: archive.bytes=3870
READING: manifest.files=10
READING: manifest.redacted=True
READING: manifest.failure.operation=3e9a9b8a-…   (等于失败操作)
READING: logs.service_bytes=3337
READING: redaction.offenders=0
```

**调试记录**：① CLI 没有 `status`/`driver status`/`operation list`（第一版工具按直觉调用，客机报
`unknown command 'status'`）；改为 `disk list`/`disk show`/`operation show`/`--version`，
操作 id 从 `operations/*.myop` 文件名枚举。② 原生 stderr 在 `$ErrorActionPreference='Stop'` 下是
终止错误，而 CLI 的拒绝正是要记录的内容；调用期间降为 `Continue` 并显式取 `$LASTEXITCODE`。

**未测**：非管理员运行（日志目录 management-only，包会标 `(absent: …)`）、`-IncludePaths` 路径。

**仍剩**：R-11（非提权 GUI）、R-13（24h soak）、R-19（演练）。

---

### 3.224 2026-11-01：**R-11 产品侧完成（caller_filtered show + owner 比较）；客机进程级读数未取得**

`disk_show` 原是 `administrator_only`，非提权连自己的盘都读不到。改为 **caller_filtered**，
并在 `service_core` 里比较 `profile.owner_sid` 与 `caller.primary_sid`：规则看不到资源，所以
“allowed, filtered”必须在加载 profile 的地方变成拒绝，否则按 id 就能读别人的盘。

证据：`unit.m5.authorization`（本机 5/5）——show/list caller_filtered；create/import/forget/
export/repair/rebind administrator_only（标准用户与“过滤后的管理员”都拒）；start/stop/
checkpoint/compact owner_or_administrator（owner 过、外人拒）；未认证与网络登录直接拒。

**客机进程级 e2e 没做成**，六种机制全部失败在“被测进程启动之前”：

| 机制 | 实际结果 |
|---|---|
| `Start-Process -Credential` | `0xC0000142`（STATUS_DLL_INIT_FAILED） |
| 计划任务 `-RunLevel Limited` | 从不运行，`LastTaskResult=267011`、`LastRunTime=1999` |
| `net user … /add` | 挂起 20 min（`net`+`net1` 无输出），触发 host 1800 s 截止 |
| `runas` + `.cmd` wrapper | `echo %ERRORLEVEL%>` 写出空值；重定向失败即中止批处理 |
| `runas` + `.ps1` wrapper | 手工探针可用；e2e 捕获目录里写探针不产 exit 文件 |
| `CreateProcessAsUser` + 受限令牌 | 子进程 `0xC0000005`（disable/restrict 两种写法都一样） |

失败表、判断与下一步都写进 `docs/validation/m5-stab-09-nonadmin.md`。tasklist STAB-09 记
**PARTIAL**，不写成通过。

**仍剩**：R-13（24h soak）、R-19（演练）；R-11 待换机制补客机读数。

---

### 3.225 2026-11-02：**R-13 PASSED（19 h 14 min soak，526 轮 / 13 次硬断电）**

24h soak 从 2026-11-01 10:56:57Z 跑到 2026-11-02 06:11Z；用户裁定提前收口，实际 **19 h 14 min**。

```text
526 rounds passed, 0 failures
13 power cuts, 13 verifies passed, 13 × post-cut workers=0
every round data_verified=True (committed-data SHA-256 matches)
service handles 166 -> 166, worker handles 119 -> 119
worker working set 33.7 MB -> 33.7 MB (max 33.8)
service working set 9.1..12.9 MB: ~10.2 at each service lifetime start,
  drifting to ~11-12 MB within 40-80 rounds, back to ~10 MB after a cut (bounded, reset by restart)
```

判定：恢复正确、句柄与 worker 内存无泄漏；service 内存是**有界漂移**（每个 service 生命周期
+1–2 MB，重启归零），记为**接受风险**，不写成“完全无增长”。时长偏离 24h 已写进
`docs/validation/m5-stab-11-soak.md` 与 tasklist STAB-11。

**仍剩**：R-11（客机读数，机制已找到：batch logon + SeBatchLogonRight）与 R-19 发布演练。
