# T1-11 证据：stop 路径补写冻结

日期：2026-10-15
分支：`feature/m5-lifecycle`，工作树 `.worktrees/m5-lifecycle`
基线：`main` = `cb3df08`（未改动）
方案：`docs/superpowers/plans/2026-10-14-t1-11-stop-write-freeze.md`
缺陷调查：`docs/validation/m5-intermittent-findings.md` §1

---

## 0. 结论摘要

| 项 | 结果 |
|---|---|
| 修复前（本轮实测） | `m5.production_execution_backend` **0 / 20**（全部失败；失败点与签名与调查文档一致） |
| **修复后（终局测量）** | **见 §4**，`-Repeat 20` |
| 关键副产物 | 冻结暴露出**第二个**同类缺陷：冻结持有独占句柄后，任何"另开句柄"的卷操作都无法打开该卷（实测 20/20 失败，`code=21001`）。修法与 M4 一致：**所有后续步骤都走持锁的那个句柄**。 |

**这不是一次改动，而是两层**：先把"提交前必须冻结写"补上，再把"冻结之后所有卷操作必须复用那把锁"补上。
第二层不做，第一层会让 `stop` 从 25% 失败变成 **100% 失败**——这个数字是实测的，见 §3。

---

## 1. 缺陷与修法（一句话各一条）

**缺陷**：stop 的计划是 `quiesce → flush → checkpoint(提交) → remove_mount`，而 `quiesce_mutations`
自述"holds no write freeze to take"，所以提交之后、拆卸检查之前，仍挂载的卷还能被写入 →
`remove_mount` 被 `runtime_dirty` 拒绝。实测 **5/20 = 25%**。

**修法**：按 M4 `ShutdownHooks` 的顺序，给 stop 计划插入一步**真正的写冻结**，
并让这次冻结**跨越** flush、checkpoint、拆卸三个独立命令：

```
quiesce_mutations → freeze_writes → flush_exact_volume → checkpoint_if_required
                  → remove_mount → remove_exact_device → stop_runtime_and_release_lock
```

- `freeze_writes` 是 **worker 协议的新动作**（不是把锁塞进 `quiesce`）：
  quiesce 是"证明要停的是哪个 runtime"，freeze 是"让它不能再改"；两者的失败含义相反
  （前者是操作瞄准了错的 runtime，后者是卷忙、可重试）。
- 锁**存在 runtime 里**（`DiskRuntime::held_volume_lock_`），不是作为返回值交给调用方：
  必须跨命令存活的位置只有这一处。

---

## 2. 第二层：冻结之后一切都必须走那把锁（实测得来）

`Win32VolumeAdapter::lock` 用 **`CreateFileW(..., GENERIC_READ|GENERIC_WRITE, 0 /* 无共享 */, ...)`**
打开卷，然后 `FSCTL_LOCK_VOLUME`。所以**一旦本进程持锁，同一进程再开这个卷就会失败**——
适配器把它报成 `busy`，它无法区分"别人持有"和"自己持有"，也不该去猜。

第一版实现把 `freeze_writes` 插在 `flush` 之前，结果在 VM 上：

```
myrd-test: stop step failed: op=flush msg=opening volume failed native=0 code=21001
FAIL .../m5_production_execution_backend_tests.cpp:665: result.ok()
```

**20/20 全部失败**（`C:\m5\control\run-97f8fd31360b4675a3b6fb2a4f1fd65f`）。
原因不是竞态，是构造性的：`flush` 自己开句柄，而卷已被自己锁住。

M4 早已给出答案（`src/run/run_loop.cpp:145-191`）：`lock → verify → flush → checkpoint → dismount`
——**锁之后的每一步都通过持锁的那个句柄执行**。

因此本增量把两处"另开句柄"改成"复用已持有的锁"：

| 位置 | 改动 |
|---|---|
| `flush_volume` | 签名加 `std::optional<LockedVolume>&`；有锁时走 `LockedVolume::flush()` |
| `dismount_volume` | 签名加 `std::optional<LockedVolume>&`；有锁时走 `LockedVolume::dismount()`，不再二次获取 |

**这是类型层面的保证，不是流程约定**：`dismount_volume` 拿不到"自己再去 lock 一次"的能力，
因为获取锁的唯一入口是 `lock_volume`，而它只在 `freeze_writes` 里被调用一次。

> 附带发现（本轮踩到并记录）：`writes_frozen` 不能由"是否还握着描述符"推导。
> 拆卸会把描述符交给下层，那一刻描述符为空但卷仍然是冻结的；用描述符判空会让 worker
> 在拆卸途中报告"未冻结"，也就是在锁唯一发挥作用的时刻说它不存在。
> 因此冻结状态由独立的 `writes_frozen_` 承载。

---

## 3. 实现清单

| 层 | 文件 | 内容 |
|---|---|---|
| 引擎 | `src/run/disk_runtime.{h,cpp}` | `lock_volume` 钩子；`freeze_writes` / `release_write_freeze` / `writes_frozen`；`flush_volume`、`dismount_volume` 增加"已持有锁"参数；拆卸完成前不释放冻结 |
| 适配器 | `src/run/runtime_adapter.{h,cpp}` | 接上 `lock_volume`；`flush` / `dismount` 在已持锁时复用该锁 |
| 卷边界 | `src/run/windows_volume.h` | `LockedVolume` 的"接管已持锁句柄"构造改为 public（引擎层要跨命令持有它，fake provider 必须能造一个） |
| worker 协议 | `src/host/m5_worker_protocol.{h,cpp}` | 新命令 `freeze_writes` / `release_write_freeze`；`WorkerRuntimeStatus` 加 `writes_frozen`；协议 minor 0 → 1（状态载荷变了，须让陈旧 worker 握手失败而不是差一个字节地解码） |
| worker | `src/host/m5_worker_runtime.{h,cpp}`、`m5_worker_session.{h,cpp}` | 实现两个命令；`observe()` 报告 `writes_frozen`；release 的回包必须报告"已不冻结" |
| 服务侧 | `src/management/service_worker_session.{h,cpp}` | 两个命令的客户端；freeze 的回包**必须**说 `writes_frozen`，否则拒绝 |
| 服务侧 | `src/management/service_worker_control.{h,cpp}` | `freeze_writes` 动作（先验身份、再冻结）；`resume_mutations` 现在也**释放冻结**（回滚路径） |
| 计划 | `src/management/state_machine.{h,cpp}` | `ActionKind::freeze_writes` 插入 stop 计划 |
| 服务核心 | `src/management/service_core.cpp` | 驱动 freeze 步骤；从持久 stop phase 续跑时的动作数 3/4/5/6 → 4/5/6/7；已执行动作偏移 2 → 3 |

**回滚安全**：`freeze_writes` 失败 → 没拿到锁，直接失败（可重试）。
`flush`/`checkpoint` 之后失败 → `rollback_stop` → `resume_mutations` → 释放冻结。
不释放就等于"为了不让卷被写坏，把它永久锁死"，比原缺陷更糟。

---

## 4. 测量

规则：**单次绿是弱证据**，修复类结论至少 `-Repeat 20`，且必须写明跑了几次。

### 4.1 开发机（纯逻辑，不建真实设备）

| 用例 | 次数 | 结果 |
|---|---|---|
| `unit.m4.disk_runtime`（含 3 个新冻结用例） | 1 | 通过 |
| `unit.m4.runtime_adapter` | 1 | 通过 |
| `unit.m5.worker_protocol`（含 3 个新协议用例） | 1 | 通过 |
| `unit.m5.worker_session` | 1 | 通过 |
| `unit.m5.service_worker_control` | 1 | 通过 |
| `unit.m5.service_worker_session`（含 2 个新冻结用例） | 1 | 通过（40 s） |
| `unit.m*` 全套 | 96 个中 4 个失败 → 见 §6 | — |

### 4.2 虚拟机（真实设备，`myrd-vm` 别名，不提地址）

干净重建：`cmake --build out/build/msvc --config Release --clean-first` → **403/403，0 error 0 warning**。

`tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^m5\.production_execution_backend$' -Repeat 20 -MinTimeoutSeconds 180`

| 轮次 | 结果 | 备注 |
|---|---|---|
| 修复前基线（调查文档 §1） | **5 / 20 失败** | 25% |
| 第一版实现（冻结在 flush 之前） | **0 / 20 通过**（20/20 失败） | 构造性失败，`code=21001`，见 §2 |
| **终局测量** | **20 / 20 通过** | 冻结 + 全部复用持锁句柄 |

终局测量的原始读数（客机 `C:\m5\control\f08cd5bf227d40799fdb87060bc2ff56`）：

```
MYRD_VM_REQUEST_ID=f08cd5bf227d40799fdb87060bc2ff56
MYRD_VM_STARTED_UTC=2026-10-15T10:00:48.2051507Z
MYRD_VM_FINISHED_UTC=2026-10-15T10:21:31.3455320Z
MYRD_VM_EXIT=0
MYRD_VM_ELEVATED=True

filter '^m5\.production_execution_backend$': 1 of 146 cases selected
[1/20] … [20/20] m5.production_execution_backend#20
20 of 20 passed
```

**验收标准（≥ 20/20）达成：`m5.production_execution_backend` 20 / 20 通过，零失败。**
（修复前同一命令为 5 / 20 失败。）### 4.3 整套回归（VM，真实设备）

命令：`tools/m5vm/Invoke-M5Vm.ps1 -Run -Repeat 1 -MinTimeoutSeconds 180`（146 用例）

| 遍 | 结果 | 失败项 |
|---|---|---|
| 第 1 遍 | **143 / 146** | `structural.m2.storage_independent_build`、`structural.m5.execution_boundary`（两个已知环境失败，客机无 VS）+ **`integration.m1.shm`** |
| 第 2 遍 | **144 / 146** | 只有上述两个已知环境失败 |

基线（T1-01 记录）是 **144/146**，第 2 遍与基线**逐项一致**。

**`integration.m1.shm` 是新发现的第三个间发项，不是本次改动引起的介绍：**

- 它只在**整套连跑**中出现一次，失败点是
  `m1_shm_session_tests.cpp:190: session.snapshot().state == SessionState::serving`
  ——一个时序断言，不是本增量碰过的代码；
- 单独跑 `-Filter '^integration\.m1\.shm$' -Repeat 10` → **10 / 10 通过**；
- 本次改动完全不涉及 M1 共享内存路径。

**结论：这是跨用例干扰型的间发项，应作为独立缺陷登记。** T1-01 的普查（每用例连跑 5 次 =
Consecutive 顺序）按构造抓不到这个形状：它只在**整套顺序**里、被前一个用例干扰时才出现。
这正是调查文档 §2.8 结尾提到的"Sweep 顺序对照普查"该做的事。

### 4.4 重入锁的问题怎么处理的（诚实交代）

方案 §2.2 要求"先写'已持锁时再 lock'的重入测试，通过之后才动 `dismount()`"。

**写了一个，跑了一次，然后把它删掉了。** 原因：要造第二个锁，测试必须自己伪造一个
`ProductVolume`（含 extents）并直接调用 `Win32VolumeAdapter::lock`。这个测试在 VM 上
把卷卡住（`CreateFileW` 挂在独占打开上），把 VM 拖进了挂起状态（需要硬重启才恢复），
而它测到的其实是"`CreateFileW` + 伪造 `ProductVolume` 的行为"，不是产品路径。

删掉之后，同一个问题由**构造性保证**回答，并且有真实测量兜底：

1. **类型层面**：`dismount_volume` 收的是"调用方已持有的锁"（`std::optional<LockedVolume>&`），
   它在结构上无法"再 lock 一次"——获取锁的唯一入口 `lock_volume` 只在 `freeze_writes` 里出现。
   这条由 `unit.m4.disk_runtime_write_freeze_is_taken_once_and_covers_the_commit` 断言：
   **锁恰好被取一次**（`volume_lock_attempts == 1`），且拆卸发生在冻结之下。
2. **真实设备层面**：`m5_backend_stop_walks_quiesce_flush_commit_remove` 里的 `freeze_writes`
   步骤只有在**真的拿到了锁**时才能成功，而它之后的 flush / checkpoint / remove_mount
   只有在**全部复用那把锁**时才能成功。这正好覆盖"已持锁时不能再 lock"这条约束——
   任何一个环节退回去自己开句柄，这个用例就会像 §2 那样 20/20 失败。

所以：**重入那条规则是被测量的，只是测量点在端到端用例里，而不是在一个伪造卷的探针里。**

---

## 5. 端到端行为（终局测量的 `-TraceTestCases` 观测）

选中 13 个用例，全部通过（`-Repeat 1` 的追踪跑，用于确认失败点位置）：

```
PASS m5_backend_launches_and_authenticates_a_real_worker
PASS m5_backend_two_profiles_never_share_a_worker
PASS m5_backend_refuses_a_launch_it_cannot_complete
PASS m5_backend_attach_finds_nothing_and_says_so
PASS m5_backend_adapter_reaches_the_worker_it_launched
PASS m5_backend_creates_a_bundle_and_mounts_it_end_to_end
PASS m5_backend_adapter_registers_the_bundle_it_creates
PASS m5_backend_checkpoint_crosses_the_boundary_and_commits
PASS m5_backend_checkpoint_boundary_withdrawal_publishes_nothing
PASS m5_backend_stop_walks_quiesce_flush_commit_remove   ← T1-11 的目标用例
PASS m5_backend_confirmed_removal_without_a_removal_is_refused
PASS m5_backend_stopping_one_profile_does_not_require_the_other_gone
```

---

## 6. 开发机上 4 个失败的定性（不是本次改动引起的）

`ctest -R '^unit\.'`（96 个）在开发机上 4 个失败；**隔离运行仍然失败**，且与本次改动的源码无关：

| 用例 | 失败点 | 定性 |
|---|---|---|
| `unit.lifecycle_runner` | `CreateFileMappingW` → `code=5`（拒绝访问） | 测试用 `\BaseNamedObjects\Global\...`，创建 Global 命名对象需要 `SeCreateGlobalPrivilege`；本机 token 是 **UAC 过滤后的管理员**（`whoami /groups` 显示 `BUILTIN\Administrators` = `Group used for deny only`） |
| `unit.m4.profile_store` | `CreateDirectoryW` with SDDL → 非 `ERROR_ALREADY_EXISTS` | 同上，SYSTEM/Administrators-only 的 DACL 目录在过滤 token 下不可用 |
| `unit.m2.windows_acceptance` | — | 同上（Windows 设备/命名对象） |
| `unit.m3.soak_verifier` | TIMEOUT | 开发机预算（60 s）不足；同类用例在客机上按 `-MinTimeoutSeconds` 抬预算后通过 |

对照：客机上计划任务以 **`elevated True`, `session_id 0`** 运行，同样的用例在那里执行。
**这 4 个用例都不落在本次改动的路径上**，且失败签名是权限/预算，不是逻辑。

---

## 7. 复现命令

```powershell
# 开发机（纯逻辑）
ctest --test-dir out/build/msvc -C Release -R "unit.m4.disk_runtime|unit.m5.worker_protocol" --output-on-failure

# 干净重建（改过头文件之后必须）
cmake --build out/build/msvc --config Release --clean-first

# 虚拟机（真实设备）
$env:M5VM_HOST='myrd-vm'; $env:M5VM_USER='usr'
$env:M5VM_KEY="$env:USERPROFILE\.ssh\id_ed25519"
tools/m5vm/Invoke-M5Vm.ps1 -Stage -Configuration Release
tools/m5vm/Invoke-M5Vm.ps1 -Run -Filter '^m5\.production_execution_backend$' -Repeat 20 -MinTimeoutSeconds 180
```

---

## 8. 环境事故（一并记录，供下一个会话避坑）

测量过程中两次把 VM 拖进"存储栈退化"状态，都需要 `vmrun stop ... hard` + `start` 才恢复：

1. **中断测试会留下孤儿 worker 并占住 AIM 代理盘。** 我 kill 掉 host 侧等待的
   `Invoke-M5Vm.ps1` 进程，**客机上的运行不会中断**（harness 自己的提示写着这一点，
   但只有真的踩过才知道后果）。结果：`Get-Disk` 多出一个 `PhysicalDrive1`，
   `diskpart rescan` 直接挂住，`Stop-Process` 报"拒绝访问"。
   **规则：kill host 侧之前先想清楚客机侧还在跑；要停就停客机上的测试进程。**
2. **孤儿 worker 会让后续用例以"看起来像产品缺陷"的方式失败。**
   在污染状态下 `m5_backend_creates_a_bundle_and_mounts_it_end_to_end` 失败在
   `created.ok()`（第 524 行）——一个与冻结毫无关系的断言。干净重启后同一二进制
   13/13 通过。**规则：设备类用例失败前先确认没有孤儿 worker / 多余物理盘。**
