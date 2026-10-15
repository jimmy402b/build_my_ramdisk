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
| **修复后（终局测量）** | **20 / 20 通过**（含重入用例，13/13 用例全过；见 §4.2） |
| 关键副产物 ① | 冻结暴露出**第二个**同类缺陷：冻结持有独占句柄后，任何"另开句柄"的卷操作都无法打开该卷（实测 20/20 失败，`code=21001`）。修法与 M4 一致：**所有后续步骤都走持锁的那个句柄**。 |
| 关键副产物 ② | **第三个缺陷**：flush 刚 `CloseHandle`，紧接着的**无共享独占打开**会拿 `ERROR_SHARING_VIOLATION`（实测冲突句柄 250 ms 内即释放 ⇒ **不是泄漏，是瞬时竞态**）。修法是 `lock` 在既有预算内做**有界重试**。见 §4.4。 |
| 重入锁 | 按要求在 VM 真实设备上端到端测过并**留在套件里**：第二次获取**被拒**，方式是 `ERROR_NOT_READY`(21)，**不是** `busy`。用例现在是 T1-11 的回归保护，`-Repeat 20` 稳定。 |

**这不是一次改动，而是两层**：先把"提交前必须冻结写"补上，再把"冻结之后所有卷操作必须复用那把锁"补上。
第二层不做，第一层会让 `stop` 从 25% 失败变成 **100% 失败**——这个数字是实测的，见 §2。

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
实测的失败方式是 `ERROR_NOT_READY`(21)，见 §4.4；适配器只把 sharing / lock / busy 违规映射成
可重试的 `busy`，这条路径报的是原生失败。无论哪种，**第二次获取根本不会成功**，所以它不能用来
"再锁一次"，也不能靠重试绕过。

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
（修复前同一命令为 5 / 20 失败。）

**终局测量带着 §4.4 的重入用例一起跑**（也就是最终会留在套件里的那个配置），
对干净重建（403/403，0 error 0 warning）的二进制做的，请求
`33d32636b7e54593bf96969130593dbe`：

```
MYRD_VM_STARTED_UTC=2026-10-15T13:54:19.8323631Z
MYRD_VM_FINISHED_UTC=2026-10-15T14:17:01.6466812Z
MYRD_VM_EXIT=0     MYRD_VM_ELEVATED=True
[1/20] … [20/20]
20 of 20 passed
```

逐用例读数（`-Repeat 1` 的输出）：**13/13 通过**，13 个用例里包含
`m5_backend_holding_the_volume_lock_refuses_a_second_lock`；跑完客机状态
`workers: 0 / disks: 0 / drives: C`（干净）。

**修 §4.4 那个竞态之前的对照**（同一个套件、同一台客机）：

| 配置 | 结果 |
|---|---|
| 带重入用例、**未修**竞态 | 9/20、2/6、5/6（收尾间歇失败） |
| 带重入用例、**已修**竞态 | **20/20** |
| 不带重入用例（只验 T1-11 本身） | 20/20（请求 `2cfe65d6c544426ba7f525cea182f81a`） |

### 4.3 整套回归（VM，真实设备）

命令：`tools/m5vm/Invoke-M5Vm.ps1 -Run -Repeat 1 -MinTimeoutSeconds 180`（146 用例）

| 遍 | 结果 | 失败项 |
|---|---|---|
| 第 1 遍（仅 T1-11 两层修复，带重入用例前） | **143 / 146** | `structural.m2.storage_independent_build`、`structural.m5.execution_boundary`（两个已知环境失败，客机无 VS）+ **`integration.m1.shm`** |
| 第 2 遍（同上） | **144 / 146** | 只有上述两个已知环境失败 |
| **第 3 遍（三层修复 + 重入用例，终局配置）** | **144 / 146** | 只有上述两个已知环境失败（请求 `1228b5ed95834b19ada4b1bfdc653862`） |

基线（T1-01 记录）是 **144/146**；第 2、3 遍与基线**逐项一致**。

**`integration.m1.shm` 是新发现的第三个间发项，不是本次改动引起的介绍：**

- 它只在**整套连跑**中出现一次，失败点是
  `m1_shm_session_tests.cpp:190: session.snapshot().state == SessionState::serving`
  ——一个时序断言，不是本增量碰过的代码；
- 单独跑 `-Filter '^integration\.m1\.shm$' -Repeat 10` → **10 / 10 通过**；
- 本次改动完全不涉及 M1 共享内存路径。

**结论：这是跨用例干扰型的间发项，应作为独立缺陷登记。** T1-01 的普查（每用例连跑 5 次 =
Consecutive 顺序）按构造抓不到这个形状：它只在**整套顺序**里、被前一个用例干扰时才出现。
这正是调查文档 §2.8 结尾提到的"Sweep 顺序对照普查"该做的事。

### 4.4 重入锁 + 第三个缺陷：独占打开与"刚关闭的句柄"的瞬时冲突

**结论先行**：重入用例做出来了、留在套件里、`-Repeat 20` 稳定通过（13/13 用例全过）。
过程中它暴露了**第三个真实缺陷**——不是我先以为的"句柄泄漏"，而是
**一个刚被关闭的句柄会让"请求无共享的独占打开"失败**，且这个窗口在成品路径上真实存在。

#### (1) 重入的测量结论（修正我先前的猜测）

用真实 worker → 真实 AIM 盘 → 真实 GPT/NTFS 卷，驱动**生产的** `Win32VolumeAdapter::lock`：

```
first : acquired=1 held=1
second: 被拒   code=21001 native=21 (ERROR_NOT_READY)
third : acquired=1 held=1        ← 释放后同一个卷仍可被取锁
```

第二次获取**被拒**，但**不是** `busy`：`CreateFileW(设备路径, RW, 0 /* 无共享 */)` 返回
`ERROR_NOT_READY`(21)，而 `lock` 只把 sharing/lock/busy 违规映射成 `busy`，所以走原生失败。
**无论哪种，第二次获取根本不会成功** ⇒ 冻结必须**保留并传递**（这就是
`flush_volume` / `dismount_volume` 收 `std::optional<LockedVolume>&` 的原因）。
第三次成功 ⇒ 拒绝是拒绝，不是"卷坏了"。

用例只断言属于我们的那半条契约（**没有锁被发放出去**），原生码**记录**而不钉死。

#### (2) 它暴露的第三个缺陷（我先前的定性是错的）

带这个用例跑 `-Repeat N` 时，收尾稳定间歇失败，我最初定性为 **worker 句柄泄漏**：

```
PROBE-REENTRANT cleanup failed: op=dismount_volume ... native=32 (ERROR_SHARING_VIOLATION)
```

**这个定性是错的。** 给 worker 的 `lock` 加了一条诊断（冲突后睡 250 ms 再试一次）之后，
worker 自己打印出：

```
MYRD-LEAK probe=1 volume=Y: first=SHARING_VIOLATION after-250ms=OPENED (err=0)
```

**冲突的句柄在 250 ms 内就被释放了——没有任何句柄泄漏。** 真正的机制是：

1. `shutdown_clean_locked` 先 **flush**：`Win32VolumeAdapter::flush` 打开卷（`FILE_SHARE_READ|WRITE`）
   然后 `CloseHandle`；
2. 紧接着 **dismount**：`lock` 用 **无共享模式** 打开同一个卷，要求"此刻没有任何其他句柄"；
3. 第 1 步刚关闭的句柄，内核/文件系统侧的引用还没拆完 ⇒ 第 2 步拿到
   `ERROR_SHARING_VIOLATION`。

`freeze_writes` 走同一条路（"打开失败→busy→拒绝"），所以 worker 会报
`the volume is busy; the write freeze could not be taken`。

**这个窗口在成品路径上真实存在**，不是探针造出来的：
`create → stop`（创建后立即停）就是"flush 完立刻独占打开"。
**它之所以一直没被发现，是因为失败被报成 `busy`**——调用方看到的是"这个盘停不下来"，
一个看起来确定性的结果，而不是一个时序竞态。这正是 §4.5 那条改进的价值。

#### (3) 修法：在调用方已经给的预算内做有界重试

`Win32VolumeAdapter::lock` 现在在 `timeout_ms` 预算内重试冲突的独占打开
（50 ms 固定间隔；窗口实测是毫秒级，退避只会拖慢答案）。

- **有界**：预算耗尽仍然返回 `busy` 并带上原生码——**不软化任何东西**。
  卷真的被别人占着时，每次尝试都被拒，调用方拿到的还是 `busy`。
- **不是"用重试掩盖失败"**：重试的是**获取锁**这个可重入的准备工作，
  被重试的操作不做任何持久化改动，失败仍然上报。项目反对的是
  "让已提交的操作靠重试自我修复"，这不是那一类。
- **与既有文档一致**：`windows_volume.h` 早就写着
  "A busy volume is an expected observation, not an API failure: callers may keep
  the runtime alive and retry"——这里只是把它落实到**调用方自己重试不了**的那一处
  （冲突发生在它正在执行的操作内部）。

#### (4) 对照实验（同一个套件、同一台客机）

| 配置 | 结果 |
|---|---|
| 探针在套件内，**未修** | **9/20**、2/6、5/6（收尾间歇失败） |
| 探针在套件内，**已修** | **20/20**，13/13 用例全过 |
| 探针移除（只验 T1-11 本身） | 20/20 |

修完之后重入用例可以留在套件里：它现在是 T1-11 的回归保护，而不是污染源。

#### (5) 自我纠正（两次，都记下来）

1. 我先把这个探针的失败归因于**"测试形态不可行"**——错的。原因是**我自己 kill 掉
   host 侧等待进程留下孤儿 worker 污染了 VM**。
2. 我又把它归因于**"worker 句柄泄漏"**——也是错的。它是一条**瞬时竞态**，
   只是被 `busy` 这个措辞伪装成了确定性失败。

教训：设备类用例间歇失败时，**先排除环境污染，再给现象加一条能区分假设的诊断，
最后才下结论**——两次我都是直接下结论，两次都错。

---

## 5. 端到端行为（`-TraceTestCases` 观测）

诊断期的一次追踪跑（当时该套件含 12 个用例、外加一个后来被删掉的重入探针）。
**12 个产品用例全部通过，包括 T1-11 的目标用例**；第 13 个是被删掉的探针，
它失败在自己伪造卷的那一步（打不开卷），与产品路径无关：

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

（探针删除后该套件为 **12 个用例**；§4.3 的两次整套 = 146 个用例，
其中 `m5.production_execution_backend` 计为 1 项，两次都通过。）

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

测量过程中三次把 VM 拖进"存储栈退化"状态，都需要 `vmrun stop ... hard` + `start` 才恢复：

1. **中断测试会留下孤儿 worker 并占住 AIM 代理盘。** 我 kill 掉 host 侧等待的
   `Invoke-M5Vm.ps1` 进程，**客机上的运行不会中断**（harness 自己的提示写着这一点，
   但只有真的踩过才知道后果）。结果：`Get-Disk` 多出一个 `PhysicalDrive1`，
   `diskpart rescan` 直接挂住，`Stop-Process` / `taskkill /F` 都报"拒绝访问"。
   **规则：不要 kill host 侧进程；要停就停客机上的测试进程。**
2. **`REQUIRE` 失败也会留下孤儿 worker**：它调 `std::_Exit(1)`，**不展开栈**，
   所以 `TempBundle` / session 的析构都不会跑，worker 进程留在世上占着设备。
   一个用例失败之后就**不要**在原状态上继续测，先重启客机。
3. **孤儿 worker 会让后续用例以"看起来像产品缺陷"的方式失败。**
   在污染状态下 `m5_backend_creates_a_bundle_and_mounts_it_end_to_end` 失败在
   `created.ok()`——一个与冻结毫无关系的断言，而且**换行号**（524 / 525），
   因为它取决于哪一次运行先被污染。干净重启后同一二进制 **13/13 通过**。
   **规则：设备类用例失败前先确认 `Get-Process myrd-m5-worker` 与 `Get-Disk` 是干净的。**
4. **`-Stage` 偶发首次失败**（`scp ... failed to upload directory .../cmake`），
   重跑一次即成功：它在客机上**先传文件、后建目录**，所以父目录不存在时第一次必失败。
   **规则：`-Stage` 报这个错就重跑，不用怀疑本地构建。**
   （判断成功不要看 `$LASTEXITCODE`——管道里它取的是最后一个 cmdlet 的，会误报失败；
   看输出里有没有 `staged to ...`。）
5. **一个自我纠正**：我先前把重入探针的失败归因于"这个测试形态不可行"，
   **那是错的**——真正原因是事故 1/2 造成的污染。VM 干净后同一个形态一次通过，
   且跑完状态完全干净。教训：设备类用例失败时，**先排除环境污染，再质疑测试或产品。**
