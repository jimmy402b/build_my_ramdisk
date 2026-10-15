# T1-11 实施方案：stop 路径补写冻结

状态：**方案已定，尚未实现**（tasklist T1-11）
依据：`docs/validation/m5-intermittent-findings.md` §1
前置结论：**必须在 T1-09（单盘验收）之前完成**，否则 preshutdown 的逐盘 stop 会以同一概率踩到

---

## 1. 缺陷与证据（一句话）

`remove_mount` 实测 **25%（5/20）** 被拒，原因是
`op=service_worker_control.remove_mount msg=The worker refused to remove: its runtime stands dirty (native=0 code=23559)`。

链条：

1. worker 判 dirty 的谓词是 `m5_worker_runtime.cpp:290` 的 `stats.dirty_bytes != 0`；
2. `dirty_bytes` = `(resident_dirty_blocks + zero_dirty_blocks) * 2 MiB`
   （`runtime_adapter.cpp:472`）——**这正是设计不变量 #6 要求的 M3 块级 dirty 真相，谓词没错**；
3. stop 的计划是 `quiesce → flush → checkpoint(提交) → remove_mount`；
4. **`quiesce_mutations` 不冻结写**——它的实现自己写着
   （`service_worker_control.cpp:492-495`）：
   > This adapter holds no write freeze to take, so quiescing is a proof and not a mutation.
5. 所以提交之后、拆卸检查之前，仍然**挂载着的卷还能被写入** → dirty 重新为真 → 拒绝。

对照 M4（`src/run/run_loop.h` 的 `ShutdownHooks`，实现在 `run_loop.cpp:145`）：

```
lock_volume → verify_volume → flush_volume → final_checkpoint → dismount_volume
→ remove_drive_letter → stop_runtime
```

**M4 先取 `FSCTL_LOCK_VOLUME` 独占锁再做最终 checkpoint**，卷被锁住时文件系统写不进去，
所以 checkpoint 就是最后一句话。**M5 缺的就是这一步。**

### 一个关键细节：锁已经存在，但太晚太短

`ProductionRuntimeAdapter::dismount()`（`runtime_adapter.cpp:443-452`）**已经**调
`impl_->volume_adapter.lock(volume, 30000)`，但：

- 它在 **checkpoint 提交之后**才加锁——提交窗口没被覆盖；
- 锁是 `dismount()` 的局部对象，**函数返回即释放**——根本不跨越拆卸。

所以 T1-11 不是"从零造锁"，而是**把已有的锁提到正确的位置并延长持有期**。

---

## 2. 改动清单

### 2.1 `src/run/disk_runtime.h` / `.cpp`：新增 lock 钩子

`DiskRuntimeOperations` 目前**没有** `lock_volume`（只有 `dismount_volume`、
`flush_volume`、`assign_drive_letter`）。需要：

```cpp
// 取得并【保留】卷独占锁。返回的 LockedVolume 由调用方持有，
// 其生命周期就是"写被冻结"的窗口。
std::function<Result<VolumeLockAttempt>(const ProductVolume&, std::uint64_t)>
    lock_volume;
```

以及 `DiskRuntime` 上的一个方法把锁**存进 runtime**（而不是作为返回值让调用方管）：

```cpp
// 持有期从 quiesce 直到 remove_and_stop；重复调用要么幂等，
// 要么明确失败（不得静默替换已持有的锁）。
Result<void> DiskRuntime::freeze_writes();
Result<void> DiskRuntime::release_write_freeze() noexcept;
```

成员：`std::optional<LockedVolume> held_volume_lock_;`

**为什么存在 runtime 里而不是逐条 RPC 传**：锁必须跨越"flush → checkpoint → 拆卸"
这几个独立的动作，而它们当前是分开的命令。把锁放进 worker 的 runtime 是唯一能
让它跨命令存活的位置。

### 2.2 `src/run/runtime_adapter.cpp`：接上钩子

`operations_.lock_volume = [this](const ProductVolume& v, std::uint64_t timeout) {
     return impl_->volume_adapter.lock(v, static_cast<std::uint32_t>(timeout)); };`

`dismount()` 里那次临时的 `lock()` 保留（拆卸仍需要独占），但要改成**复用已持有的锁**
（否则会自我死锁——`FSCTL_LOCK_VOLUME` 对同一句柄非重入）。

> **这是本改动最大的风险点**：重入锁。必须先写一个针对性测试证明
> "已持锁时再 lock 不会死锁/不会失败"，再动 `dismount()`。

### 2.3 worker 协议：新增一条动作

`src/host/m5_worker_protocol.h` 的动作用例里加 `freeze_writes`（或把
`quiesce` 变成"证明 + 取锁"的单一动作）。

**倾向前者**：单独一条 `freeze_writes`，理由是
- 服务侧的停止计划已经是分步的，多一步不改动既有语义；
- `quiesce_mutations` 的"只证明不冻结"这条注释是**有意**的（它记录了一个真实的设计约束），
  把它改成会取锁会让读代码的人以为 quiesce 能冻结——不如让它保持"证明"，由计划显式加一步。

相应地：`service_worker_control.cpp` 的 `quiesce_mutations` 之后、`flush_exact_volume`
之前插入 `freeze_writes`；`state_machine.cpp` 的停机计划同步更新。

### 2.4 worker 运行时：释放

`DiskWorkerRuntime::remove_and_stop()` 在拆卸完成后释放锁（`release_write_freeze()`），
**并且**在失败路径上也释放（否则一次失败的 stop 会永久占着卷锁——那比原缺陷更糟）。
`cleanup()` / 析构同样必须释放。

### 2.5 证据面

锁是否被取到、在什么超时下取到，应进入 `WorkerRuntimeStatus`
（`observe()` 已有 `dirty` / `committed_generation` / `volume`）：加一个
`bool writes_frozen` 或把 `freeze` 的结果记进 durable correlation，
这样"为什么这次 stop 成功了"以后也可查（manifest §1.31/§1.34 的一贯做法）。

---

## 3. 验证方案（不得用单次绿收尾）

本项目的教训是**单次绿是弱证据**（读数文档 §2.8）。所以：

1. **开发机**：锁序/策略/协议的纯逻辑测试（fake 适配器，不碰真实设备）。
2. **VM**：
   - `-Run -Filter '^m5\.production_execution_backend$' -Repeat 20` → **要求 20/20**
     （修复前实测 5/20 失败 = 25%）；
   - 重入锁的针对性用例同样 `-Repeat 20`；
   - 一次整套（144/146 的基线，只允许那两个无 VS 的环境失败）。
3. 任何"绿"的声明必须写明跑了几次。

> 注意：**头文件改动之后、测量之前必须 `--clean-first`**（读数文档 §2.7，
> 这是 manifest §7.17 第二次现身）。本改动会动 `disk_runtime.h` 与
> `m5_worker_protocol.h`，所以必然要全量重编。

---

## 4. 需要判断的点（可能升级给用户）

| 点 | 说明 | 倾向 |
|---|---|---|
| 取锁失败时的行为 | 卷被其他进程占用 → `VolumeLockAttempt::busy`。是**让 stop 失败并保留运行时**（安全、可重试），还是继续不锁地走下去（回到原缺陷）？ | **失败并保留**。`VolumeObservation` 的注释也写着 busy 是"可重试的预期观察"，不是 API 失败 |
| 超时值 | 复用 `DiskRuntimeOptions::volume_timeout`（默认 30000 ms），还是 stop 路径单独给一个更长的？ | 复用，避免第二处可调超时 |
| 是否顺带把 `dismount()` 的临时锁去掉 | 见 2.2 的重入风险 | 先测重入，再决定 |

---

## 5. 这份方案是怎么来的

- 复现率：`-Repeat 20` 实测 5/20 失败；
- 谓词来源：`m5_worker_runtime.cpp:290` + `runtime_adapter.cpp:472`；
- "不冻结"的自述：`service_worker_control.cpp:492-495`；
- M4 的正确顺序：`run_loop.h` 的 `ShutdownHooks` + `run_loop.cpp:145`；
- 已有的锁：`runtime_adapter.cpp:443-452`；
- 缺失的钩子：`disk_runtime.h` 的 `DiskRuntimeOperations` 全表（无 `lock_volume`）。
