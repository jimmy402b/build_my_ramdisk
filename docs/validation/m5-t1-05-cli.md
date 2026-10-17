# T1-05 响应侧 + CLI，以及 T1-04 的验收读数

**状态**：
- **T1-04 → PASSED**（客机实测：create 进行中 20 次 `disk list`，最大 **111 ms**，限值 1000 ms）。
- **T1-05 → 仍 PARTIAL**：响应侧、CLI、协议、开发机测试**全部完成并验证**；
  **客机端到端在 `create` 的执行阶段失败**，且**失败原因当前无法取得**（见 §7，这本身是一个缺陷）。

本文记录：建了什么、怎么验的、途中发现并修掉的两个真缺陷、客机读数、以及下一步。

---

## 1. 这一步之前缺什么（已核实，不是复述）

`docs/validation/m5-t1-05-scope.md` 已把边界写清：命令面（类型、编解码、帧、传输、授权、
`disk list` 处理器）**齐全**，缺的是
**(a) 命令 → 请求帧**（已由 part 100 的 `build_command_request` 补上）、
**(b) 响应 → 结构化结果 → 输出 + 退出码**、
**(c) CLI 可执行**、
**(d) 服务端把 pipe 接到 core**。

本轮实测又发现两条**更硬**的缺口，它们都不是"任务清单里写了没做"，而是**没人写过的接缝**：

| 缺口 | 证据 |
|---|---|
| **`PublicPipeServer` 在生产里从未被实例化** | `git grep PublicPipeServer src` 只命中它自己的 `.h/.cpp`；没有任何 handler 被提供过 |
| **`ServiceCore::dispatch` 只被测试调用** | `git grep "\.dispatch(" src` 零命中；产品里没有任何请求能走到 core |
| **没有任何响应编解码** | `git grep "encode_response\|decode_response\|service_response_wire" src` 零命中 |

⇒ **`public pipe → ServiceCore` 这条"唯一执行链路"（不变量 1）此前只存在于文档里。**

---

## 2. 建了什么（按依赖顺序）

### 2.1 响应文档：`src/management/service_response_wire.{h,cpp}`

**这是"客户端可以知道什么"的唯一决定点**，所以它同时是投影与编解码：

- `ServiceResponseWire`：`request_id` / `command` / `disposition` / `operation_id` /
  `operation` / `profiles` / `warnings` / `error` / `mount_target`；
- `project_profile()`：`ManagedProfileSnapshot` → `ServiceProfileProjection`，
  **`aggregate` 用服务端同一条规则算**（`aggregate_status(reported_profile_state(...))`），
  所以客户端不可能算出一个与服务端不同的判决；
- `encode_service_response()` = 投影 + 编码；`decode_service_response()` 严格解码。

**投影是有意有损的**，且这一点写进了注释：不携带 bundle 路径、journal、operation gate、
checkpoint correlation。**`mount_target` 保留**（用户靠盘符认盘，且它正是 `disk list`
必须有的一列）——为此给 `ServiceProfileProjection` **追加**了 `mount_target`（带默认值，
不改变任何既有聚合初始化）。

**严格性**（与命令编解码同级）：未知 tag、重复 tag、字段截断、计数越界、枚举越界、
尾随字节、文档 major 不认识、minor 高于读取者 ⇒ **全部拒绝**，不是忽略。
测试与实现同时证明"拒绝"这件事（见 §4）。

### 2.2 端点：`src/service/public_command_endpoint.{h,cpp}`

`Frame` → `ServiceCommandRequest` → `dispatch` → 编码 → `Frame`。**每一处取舍都写在头文件里**：

| 决定 | 理由 |
|---|---|
| **caller 来自 pipe 连接，绝不来自帧** | 客户端不能自称身份 |
| 控制位只认两个 flag，**未知位拒绝** | 设了位却静默不做是最坏的三种答案之一 |
| **查询不取 gate** | 调度器**整台操作**持有 gate（create 可达分钟级）；查询若取（共享或独占）就必然等它，T1-04 的验收就**不可能成立** |
| **变更只在其"准入"步取 gate（有界等待）** | 准入写的是调度器随后要 claim 的记录；`dispatch` 准入完即返回，execution 由调度器持 gate 执行 |
| **失败也回一个响应帧，不断连接** | 客户端必须能区分"服务拒绝"（退出码 2/4）与"服务不在"（退出码 3） |

gate 由 `std::mutex` 改为 **`std::timed_mutex`**（`ServiceExecutionScheduler` 与
`ServiceRuntime` 同步修改）：一处"有界的等待"应该由类型表达，而不是靠轮询。

### 2.3 服务侧接线：`src/service/public_pipe_surface.{h,cpp}` + `ServiceHost`

- `IPublicCommandSurface`（`open()` / `close()`）进 `ServiceHost`；
- **`open()` 在 `start()` 的最后**（recovery → sweep → scheduler → **pipe**）：
  在调度器起来之前接受命令，会准入一个永远不会被执行的 operation；
- **`close()` 在 `stop_locked()` 的最前**（设计 §9：关闭准入 → 摘 pipe → 停 scheduler）；
- 开发机测试用记录器断言这个顺序（与 FakeScheduler/FakeSweep 同一手法）。

`ServiceRuntime` 组合根部把 `PublicPipeSurface` 按依赖顺序 emplace，`host()` 传进去。

### 2.4 CLI：`src/cli/*` + `myrd-cli`

按设计 §10.2 的分层：`cli_parser`（文本 → `control::Command`）、`cli_renderer`
（typed response → 文本 + 退出码），CLI 专属，GUI 将来不消费。

**退出码 0–5**（v2 §10 原表，v3 G2 引用）：`0` 成功 / `1` 产品失败 / `2` 参数或 schema /
`3` 服务不可用 / `4` 权限 / `5` 超时或结果未确定。映射是**一个函数** `exit_code_for()`，
带一张测试表，而不是散在各调用点的条件。

命令面：`disk list|show|create|import|start|stop|checkpoint|compact|config set auto_start|
forget|repair-profile|rebind`、`operation show|wait|cancel`；选项 `--wait` / `--no-wait` /
`--dry-run` / `--timeout` / `--pipe`。

**`--wait` 是客户端行为**：准入后循环 `operation wait`，**不长期占住一个请求**——
服务端规则是"客户端超时永不取消一个已安全运行的 operation"（不变量 9），
所以放弃等待放弃的是等待，不是操作。

`--force` 与 `--accept-volatile-data-loss` **保持两个独立开关**：它们是给用户的两个不同承诺，
客户端不得让一个隐含另一个（设计 §10.5.3）。

`myrd-cli` 与 `myrd_cli` 都挂 `myrd_assert_execution_boundary`（不变量 2 的"新目标也要挂"）。

---

## 3. 途中发现并修掉的两个真缺陷

### 3.1 调度器在退役途中仍会 claim 并执行一个 operation（**修复前后 10/10 → 0/10**）

**现象**：`m5.execution_scheduler` 的
`m5_scheduler_stop_reports_a_deadline_it_could_not_meet` **干净重编后连跑 10 次全红**，
失败点是 `m5_execution_scheduler_tests.cpp:455`（"stop 不得把它中断的 operation 标成成功"）。

**这一点很重要**：master-plan §8 第 1 项曾把同一处判为"已关闭"，理由是"陈旧二进制"。
**实测推翻了这个理由**：`--clean-first` 之后仍 10/10 失败。**它不是陈旧产物。**

**根因（读代码即可确定）**：`run()` 的 `while (!token.stop_requested())` 在**取 gate 之前**判断；
一个 worker 若正停在 gate 上，stop 之后它取到 gate 就**不再判断 stop**，直接 claim 并执行。
于是"停在一个什么都没中断的 stop"仍会执行一个已准入的操作——**超出它刚刚报告的 deadline**，
并把 operation 标成 succeeded，而执行者正在退出。

**修复**：取到 gate 之后**再判一次 stop**，命中就不 claim。
**没有丢失**：记录是 durable 的、仍是 queued，下次启动 reconcile 之后会执行。

**读数**：`--clean-first` 干净构建后 **连续 10 次 → 10/10 通过**（修前 10/10 失败）。

### 3.2 `profiles` 根目录是共享根，但注册读取器只认自己的条目（**服务从此永远起不来**）

**现象**：客机上服务反复启动失败：

```
myrd-service: start failed: op=profile_registration.list
              msg=The protected profiles directory contains an unknown or invalid entry
```

**根因**：`WorkerBindingStore` 按自己的理由把指纹放在
`%ProgramData%\MyRamDisk\profiles\workers`（与 profile 记录同处一个受保护根，省掉第二套 ACL），
而 `ProfileRegistrationStore::list_profiles()` 把该目录里任何非 UUID、非
`lifecycle-*`/`recovery-*`/`forget-*` 的条目判为**不可接受的条目**。

⇒ **第一次启动创建 `workers`；此后每一次启动都失败。**
这台客机上 `workers` 的时间戳是 2026-10-17，**T1-02 的 `recovered=8` 读数因此是最后一次成功的启动**。

**这条为什么严重**：T1-09 的断电恢复、以及任何"重启后恢复"的验收，前提都是**服务能再次启动**。

**修复**：在 `allowed_root_metadata_leaf()` 里**按名字**承认 `workers`，并写明
**为什么不写成"忽略所有目录"**——受保护根里出现意外条目正是要报的错，
放行目录等于放弃这条保护；新的共位者必须**具名加入**，那是应当被要求论证的时刻。
`worker_binding_store.h` 的注释（原写 `<root>\MyRamDisk\workers`，与代码不一致）一并改正为实际路径，
并把"共享根的代价"写在付代价的地方。

**读数**：修复后客机服务 `STATE : 4 RUNNING`，`disk list` 正常返回两条既有 profile（见 §5）。

---

## 4. 开发机测试（纯逻辑，不建真实设备）

| 目标 | 结果 |
|---|---|
| `m5.cli`（新增，15 例） | **15/15 通过** |
| `m5.service_host`（新增 5 例端点测试，共 21 例） | **21/21 通过** |
| `m5.execution_scheduler` | **10/10 连续通过**（修 §3.1 前 10/10 失败） |
| `structural.m5.read_write_separation` | 通过 |
| `structural.m5.execution_boundary` | 通过 |
| 干净构建（`--clean-first`，全树） | **0 error / 0 warning** |

`m5.cli` 断言的核心性质：

1. **往返一致**：`disk list` 响应（含 2 个 profile + 挂载点）编码→解码后逐字段相等；
   含**非 ASCII 的 error 消息**（宽字符转换只有一处实现，见 `ipc::utf16_to_utf8`）。
2. **严格拒绝**：未知 tag / 重复 tag / 枚举越界 / 截断 / 尾随字节 / major 不认识 / minor 更新，
   全部失败（用按偏移改字节构造负例，不靠"相信实现"）。
3. **退出码映射表**：6 个错误码 + failed/reconciling/queued/wait_timeout 各一条。
4. **解析**：`disk create` 变成**类型化命令**（含 size/interval 由产品自己的 parser 解释）；
   未知开关/缺值/非法 uuid/非法 size 全部 usage 错误。
5. **渲染**：输出**判决**（`aggregate`）而不是 durable 声称；错误行带 step 与 code。

`m5.service_host` 的端点测试：

| 用例 | 断言 |
|---|---|
| 查询在 gate 被独占持有时仍作答 | 这**就是 T1-04 的纯逻辑形态**：若它不成立，VM 上无论如何都测不出来 |
| 变更在 gate 被持有时**在 deadline 内被拒** | 回的是响应帧（code `0x2203`），不是断链；且**没有编造 operation id** |
| 编码后的载荷**不含 bundle 路径** | 载荷字节里搜 `Images` / `work.myrd` 均为空，`R:` 存在 |
| 失败命令带回**服务自己的错误码与消息** | 与直接 `dispatch` 的错误逐字段相等，不是端点另编一个 |
| 未知控制位被拒 | code `0x2202` |

---

## 5. 客机读数

### 5.1 T1-04：create 进行中 `disk list` 1 秒内返回 ⇒ **PASSED**

方法（`tests/e2e/test-m5-cli-e2e.ps1`，客机受限、`MYRD_M5VM_GUEST` 守卫）：

1. `myrd-cli disk create ... --no-wait` 拿到 operation id；
2. 循环：先 `operation show` 读**当前状态**，再计时一次 `disk list`，把 `(状态, 毫秒)` 记下来；
3. 直到 operation 终态或采满 20 次；
4. 断言：**每条样本 < 1000 ms**、退出码全 0、且**至少 3 条样本是在 create 仍在跑时取的**。

**三次独立运行的读数**（同一套脚本，`create` 大小 256 MiB）：

| 运行 | 样本数 | 其中 create 进行中 | 最大时延 | 备注 |
|---|---|---|---|---|
| 第 1 次 | 20 | 20 | **35 ms** | 全部 `state=running` |
| 第 2 次 | 20 | 20 | **26 ms** | 全部 `state=running` |
| 第 3 次 | 20 | 20 | **111 ms** | 全部 `state=running` |

⇒ **60 次采样、全部发生在 create 进行中、最大 111 ms，限值 1000 ms。**

**这条读数为什么可信**：样本的"during create"不是假设，而是**每次采样前读到的状态**；
最小 13 ms / 最大 111 ms 的分布说明它量的是命令本身（含进程启动），不是缓存。

### 5.2 T1-05：CLI 能连上真实服务，但 `create` 在执行阶段失败

**已达成**：
- `myrd-cli` 经**计划任务（非网络登录）**连上真实服务的 public pipe；
- `disk list` 正常返回既有两条 profile（含盘符 `S:` / `R:`、`aggregate`、generation）；
- `disk show <不存在>` 正确返回 `error: The requested profile is not registered` /
  `code: 0x5604` / **退出码 1**，即"服务拒绝"与"服务不可达"被正确区分。

**阻塞读数**：`disk create` 被**准入**（`operation queued`），随后**执行失败**：

```
myrd-cli: disk.create: operation queued
          operation <id>: failed (phase terminal)     ← operation.wait 的读数
myrd-service 日志：kind=1 command=disk.create phase=queued
                  kind=2 command=disk.create phase=running
                  kind=3 command=disk.create phase=terminal
```

**已知的边界事实**（用于排除假设，均为实测）：
- 服务 `STATE : 4 RUNNING`，`disk list` 可用 ⇒ 不是"起不来"；
- 失败后 `myrd-m5-worker` 进程数 **0** ⇒ worker 已退出（不是挂住）；
- 工作目录里**出现了 `bundle`** ⇒ worker 确实跑过、且成功建了 bundle；
- 服务日志里**没有 provisioning helper 的输出** ⇒ 失败点在 helper 之**前**。

**失败原因当前无法取得，这是本轮的第二个发现**：
- `OperationRecord` 只存 `terminal_result`（几百字节的**标记串**，如 `evidence`/`reversed`），
  **不存错误文本**；
- `ServiceLogEvent` 没有 message 字段 ⇒ `StderrServiceLogger` 打不出原因；
- 响应投影只带 `operation.state` ⇒ **CLI 也没法告诉用户为什么失败**。

⇒ **"一次 create 失败，服务、日志、CLI 三处都说不清原因"本身是一个必须修的产品缺口**，
且它是下一步排查的前置（不是可选的体验改进）。

### 5.3 整套基线（客机，154 条）

`tools/m5vm/Invoke-M5Vm.ps1 -Run`（请求 `f79804f56e234d9191638e1eb4ee0fc8`）：

```
151 of 154 passed
  FAIL structural.m2.storage_independent_build (exit 1)   ← 客机无 Visual Studio（已知）
  FAIL structural.m5.execution_boundary        (exit 1)   ← 同一原因（已知）
  FAIL integration.m5.cli_e2e                  (exit 1)   ← §5.2 的 create 失败
```

**测试矩阵 152 → 154**（新增 `m5.cli` 与 `integration.m5.cli_e2e`）。**产品面全绿**：
`m5.production_execution_backend`（T1-03 的修复保持有效）、`m5.execution_scheduler`（§3.1 的修复在客机成立）、
`m5.service_host`、`m5.cli`、`structural.m5.read_write_separation` 全部通过。
**量测范围**：本轮全量只跑了一次，不宣称"稳定"；有独立重复读数的是 §3.1（10 次）与 §5.1（3 次 × 20 采样）。

---

## 6. 变更清单

| 文件 | 性质 |
|---|---|
| `src/management/service_response_wire.{h,cpp}` | **新增**：响应投影 + 严格编解码 |
| `src/service/public_command_endpoint.{h,cpp}` | **新增**：帧 → core → 帧 |
| `src/service/public_pipe_surface.{h,cpp}` | **新增**：`PublicPipeServer` + endpoint 的宿主实现 |
| `src/cli/cli_parser.{h,cpp}`, `cli_renderer.{h,cpp}`, `cli_main.cpp` | **新增**：CLI |
| `src/service/service_host.{h,cpp}` | `IPublicCommandSurface`；open 最后、close 最前 |
| `src/service/service_runtime.{h,cpp}` | 组合 `PublicPipeSurface`；gate 改 `timed_mutex`；`pipe_name` |
| `src/service/service_main.cpp` | `--console` **改为持续服务到 Ctrl+C**（可诊断、可作 e2e 夹具） |
| `src/management/execution_scheduler.{h,cpp}` | gate 改 `timed_mutex`；**§3.1 的 stop 复检** |
| `src/management/service_worker_control.{h,cpp}` | **命令门**：所有命令串行；`observe_runtime` **try_lock → `nullopt`** |
| `src/management/profile_registration_store.cpp` | **§3.2**：承认 `workers` 共位者 |
| `src/management/worker_binding_store.h` | 注释与代码一致（`profiles\workers`），写明共享根的代价 |
| `src/control/ipc_codec.{h,cpp}` | `utf16_to_utf8` / `utf8_to_utf16` 导出（唯一实现，供响应编解码复用） |
| `src/management/service_core.h` | `ServiceProfileProjection` 追加 `mount_target`（带默认值） |
| `tests/model/m5_cli_tests.cpp` | **新增** `m5.cli`（15 例） |
| `tests/model/m5_service_host_tests.cpp` | **新增** 5 例端点测试（含"gate 被持有仍可查询"） |
| `tests/e2e/test-m5-cli-e2e.ps1` | **新增** `integration.m5.cli_e2e`（客机，守卫 exit 2） |

### 6.1 `ServiceWorkerControl` 的命令门（为什么必须加）

worker 的命令通道是**严格一次请求一次应答、没有重同步**的序列。在 public pipe 出现之前，
唯一调用者是调度器（在 execution gate 之后），所以"不会有两个线程"是**外部**给的保证，
类自己没写下来。T1-04 要求**一个读路径在长命令进行中也能跑**，这条保证必须落到类里：

- 所有命令入口取 `command_gate_`；
- **`observe_runtime` 用 `try_lock`，拿不到就返回 `nullopt`（"未观测"）**，
  绝不排队等 create —— 那是 `disk list` 会跟着等分钟级的原因；
- `nullopt` 是**更弱**的答案，不是错的答案：读路径据此报告 durable 状态（transition 中即
  `provisioning`/`starting`），并且**不会**谎报 `worker_alive == false`（那会把一块正在跑的盘 fail-closed）；
- `require_runtime_observation`（变更路径）**仍然排队**：决定"是否安全"的调用者不能被
  "通道忙"当成"不知道"。

**代价写在成员声明处**：一把锁是**整对象**粒度；per-profile 串行是 T2-01 的工作，
在此发明第二档粒度会在那里被推翻。后果是"一个 profile 的命令在跑时，另一个 profile 的观测答
'未观测'"——在 G4 延后的前提下可接受，恢复 G4 时必须改。

---

## 7. 下一步（按顺序）

1. **把 operation 的失败原因做成可达的**（三处，缺一不可）。**已经定位到要改的位置**：

   | 位置 | 现状 | 要做什么 |
   |---|---|---|
   | `src/management/service_core.h` `ServiceLogEvent` | 只有 `kind/request_id/operation_id/profile_id/command/phase`，**没有 message** | 加一个 `std::string message`；`src/service/service_system.cpp:50` 的 `StderrServiceLogger::write` 一并打印 |
   | `src/management/service_core.cpp:6423` `finalize_operation(...)` 调用处 | 传的是 `completion` + `worker_succeeded` + 标记串，**失败动作的 `Error` 在这一层之前就被丢掉了** | 把失败动作的 `Error`（`code` / `operation` / `message` / `native_code`）一路带到 `finalize_operation`，并写进（a）日志事件、（b）`terminal_result` 的结构化失败证据 |
   | `src/management/service_core.h` `ServiceOperationProjection` + `service_response_wire` | 只有 `state/phase` | 追加失败原因（可选字段，带默认值，无需破坏既有初始化），CLI 才能说"为什么失败" |

   **注意**：`terminal_result` 现在是**标记串**（`evidence` / `reversed` / `MYRD…`），把它换成或追加结构化失败证据属于 **durable 记录格式变更**，必须同时处理旧记录的读取（现有读者对旧记录应"照旧解读为无原因"，而不是拒读）。

2. 用 (1) 的读数定位 `create` 在服务里的失败点，修掉，然后跑完 T1-05 的端到端
   （create → list → show → 读写 → checkpoint → stop → 恢复 → forget），并 `-Repeat`。
3. T1-05 收尾后回到 T1-06（依赖 T1-03 + T1-05）。

**同时记下两条待办**（不是阻塞项）：
- `integration.m5.cli_e2e` 目前**包含 T1-04 的验收**（已通过）；T1-05 收尾后应把两段拆开，
  让 T1-04 的读数独立可重复运行；
- 客机上两条 2026-09/10 的既有 profile（`9e5a6b53` 在 `S:` blocked、`ea53d5dd` 在 `R:`
  `runtime=Stopping`）来自更早的会话，不是本轮产生。脚本已改为**动态选空闲盘符**以不受其影响；
  它们的处置需要一次人工判断（属于"外部遗留状态"，不是本轮可以自行清理的）。
