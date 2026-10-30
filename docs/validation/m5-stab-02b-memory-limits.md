# STAB-02R / R-01：M5 不 over-commit —— 超机器提交上限的预算在创建时拒绝

**结论（PASSED）**：

1. `disk create --memory-mode fixed --memory-hard-limit X` 在 `X > 机器提交上限` 时，于**创建准入阶段**
   被拒绝，稳定错误码 **`0x560A`**（`ServiceCoreError::memory_budget_exceeds_commit_limit`），
   文案写明 **“M5 does not over-commit”** 并给出请求值与本机提交上限两个数字；
   拒绝发生在任何 operation / registration reservation / bundle / worker / 设备之前。
2. 预算耗尽现在**可自证**：`disk show` 在预算拒绝发生后显示 `budget_rejections=N`
   （正常盘不显示该字段）。客机实测：基线 `0` → 写失败后 `43` → checkpoint 后 `228`；
   服务重启后的新 runtime 回到 `0`，符合“这是 runtime 的计数”的语义。
3. 耗尽实验变成**一条命令**：
   `tools/m5vm/Invoke-M5FullSuite.ps1 -SkipStage -Only memory`
   → 客机 **1/1 passed，118 s，exit 0**。该组在 `tests/CMakeLists.txt` 里注册为
   `integration.m5.memory_exhaustion`，默认 160 例全量**不包含**它（破坏性实验只按需跑）。

**客机**：`myrd-vm`，Windows 11 x64 26200，Release，`Reset-M5Guest` 后空机。
**本机**：13 个受影响套件全绿（见 §3）。

---

## 1. 为什么做这一刀（裁定 1）

H1 的 STAB-02 只证明了“预算内”的行为：1 GiB 盘、256 MiB 预算，写到 256 MiB 失败，checkpoint/重启/UNMAP
都正确。用户真正可能撞的是“把预算设得比机器能提交的还大”，而当时的失败形态是**泛化 I/O 错误**，
`disk show` 里也没有任何东西能说明“这次失败是预算拒绝”。2026-10-30 的裁定 1 把范围定死：

> **不允许** fixed 内存预算超过机器提交上限；创建时拒绝；文档写“M5 不 over-commit”；不做物理 RAM 耗尽实验。

所以这一刀要同时关三件事：**准入拒绝**、**拒绝可观测**、**实验可重跑**。

## 2. 实现落点

| 层 | 落点 | 作用 |
|---|---|---|
| 观察 | `src/management/service_core.h` `PhysicalMemorySnapshot::commit_limit_bytes`；`src/service/service_system.cpp` `SystemPhysicalMemoryQuery` 用 `MEMORYSTATUSEX::ullTotalPageFile` 填充 | 提交上限 = 物理内存 + 页面文件能承诺的量，**不是**物理内存本身 |
| 契约 | `src/product/service_status.h` `ServiceCoreError::memory_budget_exceeds_commit_limit` = **`0x560A`** | **追加**在枚举末尾，`0x5601…0x5609` 的编号全部不变 |
| 准入 | `ServiceCore::refuse_over_commit_budget()`（`service_core.cpp`，紧邻单盘 guard） | 只在 `fixed` 且请求值 `> commit_limit` 时拒绝；`commit_limit == 0` 视为预检失败并拒绝（fail-closed），而不是“无限制” |
| 调用点 | `ServiceCore::dispatch()` 的 create 分支，`memory_.query()` 之后、`LifecycleOperationIntent` / reservation / admission 之前 | 拒绝时没有 durable 记录、没有 pending 条目、没有 provisioning 请求 |
| 日志 | `ServiceLogKind::admission_refused`，`phase=memory-budget`，消息含 `M5 does not over-commit (requested=… commit_limit=…)` | 被拒命令没有 operation 记录，原因只能落在服务日志 |
| CLI | `src/cli/cli_renderer.cpp`：`0x560A` → 退出码 **1**（产品拒绝），不是 2（用法）或 3（不可达） | 脚本可分支 |
| 可观测链路 | store `hard_limit_rejections` → `DiskMaintenanceStats` → `WorkerRuntimeStatus`（线格式追加 u64）→ `WorkerRuntimeObservation` → `ServiceProfileProjection` → `ServiceDiskView`（客户端线格式追加 u64）→ `disk show` 的 `budget_rejections=` | 复用 resident/budget 这条已有的“worker 自述”通道；零值不显示 |

**为什么用 `ullTotalPageFile`**：Windows 拒绝的是“提交”，不是“物理页”。物理内存 8.59 GB、提交上限
9.13 GB 的客机上，用物理内存做界会把合法的 8.5 GiB 预算也拒掉，而用提交上限才是“能不能兑现”的真问题。

**为什么 `commit_limit == 0` 要拒绝而不是放行**：这是 guard 唯一可能悄悄失效的方式。观测不到上限时
声称“可以提交”正是裁定 1 禁止的 over-commit。

**为什么持久化里没有 `commit_limit_bytes`**：它是准入时刻的事实，每次 create 重新读取。把某台机器的
页面文件大小冻进 durable reservation，会把“机器改了配置”变成“请求变了”。`LifecycleOperationIntent`
的相等性也只比较 `total/available`，所以重放不受影响。

## 3. 本机纯逻辑读数（开发机，Release `--clean-first` 后）

新增 2 个模型用例（`m5.service_orchestration`）：

| 用例 | 断言 |
|---|---|
| `m5_over_commit_fixed_budget_is_refused_at_admission` | 20 GiB 提交上限/24 GiB 请求 → `0x560A`、step=`service_core.memory_commit_limit`、消息含 `M5 does not over-commit` 与两个十进制数字；**没有** operation、**没有** reservation、profile 快照逐字段不变；日志 kind=`admission_refused`、phase=`memory-budget` |
| `m5_a_fixed_budget_at_the_commit_limit_is_admitted` | 反向对照：请求 == 提交上限时照常准入（guard 不能变成“fixed 一律拒绝”） |

新增/扩充的读数通道用例：

| 套件 | 用例/断言 |
|---|---|
| `unit.m5.worker_protocol` | status 的 `hard_limit_rejections=7` 经线格式往返后仍为 7 |
| `unit.m5.service_worker_control` | `observe_worker_runtime` 在 worker alive 时携带 `hard_limit_rejections=7`，torn-down 时为 0 |
| `m5.cli` | 客户端线格式往返保留 `hard_limit_rejections=11`；渲染 `budget_rejections=11`，为 0 时不渲染 |

受影响套件（ctest，13/13 passed）：

```text
m5.service_orchestration  m5.service_host  m5.cli  m5.gui  m5.service_maintenance
m5.execution_scheduler    m4.checkpoint_operation
unit.m5.worker_protocol   unit.m5.service_worker_control  unit.m5.worker_session
unit.m5.worker_private_pipe  unit.m4.runtime_adapter  unit.m4.provision_script
```

**回归证据（记录一次真实缺陷）**：第一次跑 `m5.cli` / `m5.service_host` 双红，原因不是编解码本身，
而是 `response_view.cpp` 里手工维护的 `kProfileFixedBytes`（profile 记录的定长前缀）没有跟着新增的
u64 更新，导致 mount 长度从错误偏移读取。修法是把这个常量从“字段列表”重新推导（并注释写清），
同时修正 `decode_profile` 的位置初始化顺序（`checkpoints_since_compaction` 不上线，保持 0）。
两处都修好后复跑 13/13。

## 4. 客机读数

### 4.1 超提交预算在准入被拒（`integration.m5.cli_e2e`，R-01 段）

请求：`disk create --size 64GiB --memory-mode fixed --memory-hard-limit 64GiB`（空服务）。

```text
--- R-01: a fixed budget above the machine commit limit is refused at admission
  commit limit reported by the service: 9125646336 bytes
PASS R-01: over-commit budget refused at admission (code 0x560A, commit limit named,
     no profile/operation/bundle/worker)
```

CLI 输出（断言的一部分）：

```text
error: M5 does not over-commit: a fixed memory budget of 68719476736 bytes exceeds
       this machine's commit limit of 9125646336 bytes. Use a smaller
       --memory-hard-limit, or --memory-mode automatic.
  command: disk.create
  step: service_core.memory_commit_limit
  code: 0x560A domain 8
```

同一段断言还验证了**无残留**：`disk list` 没有 profile、输出里没有 operation id、
`C:\m5\<name>-over-commit` 目录不存在、`myrd-m5-worker` 进程数为 0。
退出码是 **1**（产品拒绝）。

旁证：独立读取客机 `Win32_OperatingSystem.TotalVirtualMemorySize = 8,911,764 KB`
= **9,125,646,336 B**，与服务自报的提交上限逐字节一致。

### 4.2 预算耗尽可观测（`integration.m5.memory_exhaustion`，1 GiB / 256 MiB / 320 MiB）

```text
READING: baseline.mem=6291456/268435456
READING: baseline.budget_rejections=0
--- write 320 MiB in 4 MiB chunks
READING: write_seconds=13.4
READING: written_mib=256
READING: write_failed=yes
READING: failure_hresult=0x80131501        # “由于 I/O 设备错误，无法运行此项请求”
READING: after_failure.mem=268435456/268435456
READING: after_failure.budget_rejections=43
READING: budget_rejections_after_failure=43
READING: workers_after_failure=1
READING: checkpoint_state=succeeded
READING: generation_after_checkpoint=3
READING: after_checkpoint.budget_rejections=228
READING: generation_after_restart=4
READING: after_restart.budget_rejections=0
READING: file_length_after_restart=268435456
READING: committed_prefix_verified_bytes=4194304
READING: write_after_unmap=ok
READING: cleanup_forget=ok
PASS integration.m5.memory_exhaustion: budget exhaustion behaved as documented
```

| 断言 | 读数 |
|---|---|
| 写失败在预算处、不挂死 | 13.4 s 写满 256 MiB，第 65 个 4 MiB 块失败，`0x80131501` |
| 失败能被 `disk show` 解释 | `budget_rejections=43`（基线 0；checkpoint 后 228） |
| worker 仍然活着 | `workers_after_failure=1` |
| checkpoint 仍完成并推进代 | 2 → 3 |
| 重启回到最后提交代、字节完整 | generation 4、文件 268,435,456 B、前 4 MiB 逐字节一致 |
| 重启后计数语义正确 | 新 runtime `budget_rejections=0`（不是 durable 数字） |
| UNMAP 后恢复 | `write_after_unmap=ok` |

### 4.3 一条命令重跑（R-01 的第三半）

```text
tools/m5vm/Invoke-M5FullSuite.ps1 -SkipStage -Only memory
...
=== group memory (filter ^integration\.m5\.memory_exhaustion$)
1 of 1 passed
--- memory: 1 passed, 0 skipped, 0 failed in 118 s
M5 full suite: 1 cases, 1 passed, 0 skipped, 0 failed in 128.4 s
exit=0
```

组前由 harness 经 agent 执行 `Reset-M5Guest`（与全量同一套重置），所以它是可重复的门禁步骤，
不是一次性脚本。**默认全量仍是 160 例**：`-IncludeMemoryExhaustion` 才把这一例加进 161；
`-Only memory` 只跑它 1 例。`expected_cases` 按“本次实际选择的组”计数（修过一次：按 staged 列表
计数会让 `-Only memory` 通过却报 `1 cases reported, 161 expected` 并 exit 1）。

### 4.4 同组回归（`integration.m5`，4/4）

同一次会话里 `-Only integration.m5`：`cli_e2e` / `checkpoint_inspection` / `gui_workflow` /
`install_uninstall` **4/4 passed，152.6 s**。`cli_e2e` 里既有 R-01 段，也复跑了 STAB-01 的第二块盘拒绝
（`0x5609`）与 T1-04/T1-05/T1-07/T1-08/T4-02 各段。

## 5. 明确没有覆盖的（不要读成“已验证”）

- **物理 RAM 真耗尽**：按裁定 1 **不做**、也**不支持**。产品只承诺“预算内”，预算超过机器提交上限的
  请求在创建时就被拒。
- **页面文件在运行中被改小**：准入时读到的上限成为当时的承诺；运行中系统配置变化不在测试范围。
- **`hard_limit_rejections` 的持久性**：它属于 runtime，服务重启/盘重启后归零。这是有意的：
  它回答“这次运行的失败是不是预算拒绝”，不是寿命计数。
- **超过 `0xFFFFFFFF` 次拒绝**：计数是 u64，未做溢出测试（量级不可达）。
- **GUI 显示**：`budget_rejections` 只进了 CLI 的 `disk show` 行；GUI 的磁盘行尚未展示（R-14/R-16
  的 UX 收口会一并处理文案，不阻塞 R-01 的验收口径“`disk show`/日志能解释”）。
