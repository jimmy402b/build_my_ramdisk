# T1-08 检查点策略终于有消费者（interval / dirty / memory / suspend）

**结论**：`profile::CheckpointPolicy` 的四个字段从"序列化、校验、读取、丢弃"变成真的会触发
checkpoint；客机实测**没有发任何命令**的情况下 generation 自己前进。

**客机**：`myrd-vm`，Release，硬重启 + 清空 `C:\ProgramData\MyRamDisk\{profiles,operations}`。

---

## 1. 为什么这是一个增量

`interval_seconds` / `dirty_threshold_bytes` / `on_memory_pressure` / `on_suspend` 四个字段
自 profile schema 起就存在，写盘、读回、校验都覆盖，**但没有任何消费者**。所以
`--checkpoint-interval 30s` 只是一个被存下来的数字；T1-10 想写的"崩溃最多丢一个间隔"
在本次之前是不成立的。

## 2. 落点

| 层 | 落点 |
|---|---|
| 策略（纯函数） | `management::checkpoint_due_reason(MaintenanceInput)`：内存压力 → dirty 阈值 → interval，顺序即优先级；非 running 一律不触发 |
| 时钟 | `ServiceExecutionScheduler` 的 poll loop 按 `maintenance_interval_ms`（默认 15 s）**claim-once** 调用 `IMaintenanceTick::tick()`——N 个 worker 每周期只评估一次 |
| 执行 | `ServiceMaintenance` 以 SYSTEM 身份（`S-1-5-18`）`dispatch(DiskListCommand)` 取全部 profile + 观察值，再对每个到期的 profile `dispatch(DiskCheckpointCommand)`，`no_wait`，**只做持久化准入**，执行仍归调度器 |
| 休眠 | service 状态加 `SERVICE_ACCEPT_POWEREVENT`；`PBT_APMSUSPEND` → `ServiceHost::suspend()` → `checkpoint_for_suspend()`（`on_suspend == best_effort` 的 running profile 全部准入） |
| 观察值 | worker 状态新增 `dirty_bytes`（阈值按字节问）；`resident_bytes`/`budget_bytes` 来自 T1-07 |

**为什么 suspend 不是 stop**：机器去睡觉不是关机。停掉服务会拆掉它要保护的那些盘，
所以 suspend 只做"尽力而为的检查点准入"，worker 与设备原地不动；真在提交落盘前断电，
由恢复路径兜底（T1-09）。

## 3. 客机读数

```text
--- T1-08: an automatic checkpoint fires on the interval policy
  policy disk mounted at V: with a 30 s checkpoint interval
  generation moved 2 -> 3 on its own after 30.4 s
  policy disk stopped and forgotten
PASS integration.m5.cli_e2e
```

请求 `dbc9db2e59bf43df9bc0145627f863b7`（单次）；验收 **`-Repeat 5` → 5/5**
（请求 `7aa88f14b33741358f8689fd3aeff5e3`，6 分 9 秒，干净客机）。

重复 5 次而不是 20 次，理由写明：这不是竞态而是**定时器**，每次迭代已经包含一次
"30 s 定时 + 90 s 观察窗"，5 次连续通过即 5 次独立的定时命中；再多的重复增加的是运行时间，
不是证据强度。

这一轮**没有发过 `disk checkpoint`**：profile 建的时候就带了 `--checkpoint-interval 30s`，
此后只是轮询 `disk list`。

## 3.1 一个被量出来的范围边界：**两块盘同时活着不是 M5 的形态**

第一版把策略盘放在第一块盘还在运行时创建，于是客机上同时有两个 AIM 代理设备。结果：

```text
disk.stop（第二块盘）→ 三步 lifecycle-action 均 completed
disk.start           → find_recovered_volume: no volume carries the recorded identity
disk.forget          → forget safety refused: device_absent=0（其余八项全绿）
```

即第二块盘的**拆卸没有真正移除设备**。把这套操作搬到"第一块盘已经 forget 之后"（同一块盘
独占整机），整条链路立刻全通。M5 是**单盘**产品（G4 延后，见 AGENTS §2.1），所以这不是
"多了个 bug"，而是**同时两块盘本来就不在支持范围内**——e2e 因此改成一次只有一块盘，
并把这段读数记在这里，避免下一个会话再把它当成策略的缺陷。

顺带得到一条可复用的诊断：`forget` 的九个合取项现在会在拒绝时**逐项打印**
（`profile_match / image_match / revision / worker_absent / writer_lock_absent /
device_absent / bundle_present / identity_match`）。这条是在"十次重试都只看到 `unsafe`"
之后加的，和 `verify_volume`、checkpoint boundary 的那两处同源。

## 4. 接线时量到的两个缺陷（都记在这里，因为都是"只有真跑才会露出来"）

1. **interval 时钟没有起点**：`since_last_checkpoint_ms` 对没见过的 profile 返回 0，
   而只有 checkpoint 才会推进它——于是永远不会到期。客机表现是
   `generation stayed 2`（30 s 间隔等了 90 s）。修法：第一次看到 running profile 时把
   "现在"写进内存里的时钟表，并在注释里写明为什么。
2. **命令寻址用错了句柄**：第一版用 mount target（`"Z:"`）寻址，每次都收到
   `maintenance: checkpoint refused (interval): The requested profile is not registered`
   ——存储按**显示名或 uuid** 解析 target，盘符两者都不是。改为 profile uuid。
   （服务日志现在会写这行，所以这类失败不再是静默的。）

## 5. 未做

- `disk release-memory`（§7.4）仍未实现。它在 T1-07 被延后、在 T1-08 被再次延后，
  理由一致：需要一条从 storage 公开 `reclaim_to` 到 CLI 动词的完整纵切。**记录在案，
  不当作已完成**（`m5-handoff.md` §3.17「未做」）。它不在完成口径里（master-plan §8 的
  遗留清单四项中没有它），但设计 §7.4 承诺过，所以不许以"没提过"的方式消失。
- `require_clean` 停止策略、以及真实断电路径：前者仍未测，后者是 T1-09。
