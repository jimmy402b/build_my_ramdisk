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

### 5.2 T1-05：CLI 能连上真实服务；`create` 的失败原因已可达，并已定位

**已达成**：
- `myrd-cli` 经**计划任务（非网络登录）**连上真实服务的 public pipe；
- `disk list` 正常返回既有两条 profile（含盘符 `S:` / `R:`、`aggregate`、generation）；
- `disk show <不存在>` 正确返回 `error: The requested profile is not registered` /
  `code: 0x5604` / **退出码 1**，即"服务拒绝"与"服务不可达"被正确区分。

**让失败原因可达（本轮完成的第一半）**：
`ServiceLogEvent` 增加 `message` 字段、新增 `ServiceLogKind::operation_failed`、
在 `execute_operation` 的**动作失败分支**（`service_core.cpp` 的 `if (!result.ok())`，
即所有失败分支的上游、错误还在作用域内的唯一位置）记录
`action / code / native / step / detail`；`StderrServiceLogger` 打印该字段。

**第一次拿到原因（请求 `0edd233fabfc4518b639e68069fab1c8`）**：

```
myrd-service: kind=7 command=disk.create phase=create-bundle
  message=action=create-bundle code=20994 native=0
          step=provision_new_volume detail=requested drive letter is occupied
```

⇒ **失败点定位到 `provision_new_volume`：请求的盘符已被占用**（code `20994`）。
即 create 走到建卷那一步、被盘符冲突拒掉；**不是** bundle、不是 worker 启动、不是权限。
e2e 脚本选盘符时用的是 `Test-Path` + `disk list` 两条判据，**与产品自己的"盘符被占用"判据不一致**——
产品看到的是卷/设备命名空间里的占用（此前几次失败运行留下的残留），脚本看不到。
**⇒ 下一步：先让两边的判据一致**（用产品自己的判据选盘符，或加一条显式的"清理/校验盘符"前置），
再重跑端到端；这属于**测试夹具的修正**，不是产品缺陷的证据。

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
---

## 8. 2026-10-18：把 create 一层层打通到的位置（每一步都是读数）

**方法**：把"失败原因不可达"逐个补上（`ServiceLogEvent::message`、worker 动作失败分支、
`finalize_operation` 的终态标记、`publish_registration` 的拒绝原因），再用客机 e2e 逐层推进。
**没有一次是靠猜**；每次失败都带着 `action / code / step / detail`。

| # | 读数 | 结论 / 处置 |
|---|---|---|
| 1 | `code=20994 step=provision_new_volume detail=requested drive letter is occupied` | 盘符占用；e2e 改为按**产品同一命名空间**选字母（`Win32_Volume`/`LogicalDisk`），产品拒绝即硬失败 |
| 2 | 残留 `R:`/`U:`：`QueryDosDevice` 有映射、`Get-Volume` 无卷；`mountvol /D` 与 `DefineDosDevice` **都删不掉** | 属挂载管理器残留，**只有重启能清**（实测：重启后全部 `rc=0`）。e2e 现在把"任何 DOS 名有应答"的字母一律排除，并打印其目标 |
| 3 | `code=21000 step=select_volume_for_disk detail=no volume matches ...` 且 helper 报成功 | 加了 10 s 有界重试（供给后卷的发布是异步的），并让拒绝**带计数**：`observed/foreign/other_letter/not_mounted/no_guid/extents/other_disk` |
| 4 | `code=22024 step=profile_registration.publish detail=... unknown or invalid entry` | **`workers` 是目录共位者**，而 publish 的枚举对**目录**不查文件白名单 ⇒ 与 `disk list` 判据不一致。已按名字放行（`shared_root_directory_leaf`），并写明"不写成忽略目录"的理由 |
| 5 | **`code=22022 step=service_core.checkpoint_boundary detail=The worker checkpoint correlation is invalid`**，终态标记 `worker` | **当前阻塞点**：create 的供给流程会做第一次 checkpoint，P4 correlation 校验失败。这是**产品缺陷候选**（不是环境），下一步从这里入手 |

**e2e 同时修正了两个夹具缺陷**（都会造成假失败）：
- `operation wait` 的一次调用是**有界观察**，不是"已完成"的承诺；脚本现在循环到终态（`Wait-ForTerminal`）；
- create 的时长超过一次等待预算，之前的脚本把"仍在 running"当成失败。

**量测范围**：T1-04 的读数在每一轮都复现（20 次采样全部落在 create 进行中，最大 26–135 ms）；
**T1-05 端到端仍未绿**，因此 **T1-05 保持 PARTIAL**。

### 8.1 当前阻塞点已精确到数值：create 的供给 checkpoint 的 P4 correlation

**读数（请求 `fc977fd68ca743fdb312cf03d138c402`，客机）**：

```
checkpoint boundary refused: profile=1 op_match=1 profile_match=1 image_match=1
  pid=5928 created=134367921189870409 session_gen=1
  prior=1 confirmed=0 candidate=2
```

`valid_checkpoint_boundary` 的七个合取项里，**唯一不成立的是**
`evidence.prior_generation == profile.checkpoint.confirmed_generation`：
**worker 说它要把 generation 1 推进到 2，而 durable 记录里 confirmed 是 0。**

**这说明什么（不是猜测，是等式）**：新建 bundle 在**供给完成之后**已经带有一个已落盘的 generation
（worker 打开时看到 prior=1），而服务侧这条 profile 的 `checkpoint.confirmed_generation`
还停在 0（本服务从未提交过 checkpoint）。**两侧对"当前是第几代"的认识差 1**，
而这条校验正是为捕捉这种跨层不一致而存在的——所以它拒绝得对，**问题在别处**。

**下一步的两条候选（必须先判定哪条对，不能猜）**：
1. **创建路径应在供给之后采纳 bundle 的当前 generation**（把它写进 durable profile），
   使首次 checkpoint 的 `prior` 与 `confirmed` 相等；或
2. **首次供给 checkpoint 的校验应允许 `prior == confirmed + 1`**，并写明理由（供给本身落了一代）。
   注意这**不是**放松校验：pid/creation/session/前几项仍然必须相等。

**判定依据应当来自 M3 的 generation 规则**（供给写没写第 0/1 代、谁拥有第一次提交），
不能用"让它过去"来选。**这也是它没有在本轮被改掉的原因**：
改动会落在 M3/M4 的既有证据上（T1-11 的 `-Repeat 20` 就在这条链上）。

**其余读数**：T1-04 在每一轮都复现（20/20 采样落在 create 进行中，最大 76 ms）；
e2e 本轮又修掉两个夹具缺陷（唯一 profile 名，避免历次失败留下的注册冲突；
"任何 DOS 名有应答"的盘符一律排除）。

### 8.2 判定：候选 1 成立（供给后的 bundle 已经站在 generation 1），并说明为什么不能靠"带个字段下来"

**依据（全部来自现有代码与读数，不是推断链）**：

1. `prior_generation` 来自 worker 的 `prepared`，即 **M3 自己**对 bundle 当前代的判断
   （`src/host/m5_worker_runtime.cpp:516-517`：`candidate.prior = prepared.prior`、
   `candidate.candidate = prepared.candidate`）——**不是服务侧推算**。
2. 客机读数 `prior=1 candidate=2` ⇒ **创建完成后 bundle 已经站在第 1 代**。
3. `m5.production_execution_backend` 之所以绿：它只断言
   `candidate == prior + 1`（`tests/integration/...:1342-1343`），
   **从不与 durable 的 `confirmed_generation` 比较**；那条比较只存在于服务核心，
   所以"测试绿 + 服务红"不矛盾，**它只是没测这一条**。
4. `BundleRegistrationEvidence` **没有 generation 字段**（`service_core.h`），
   所以服务不可能从既有证据里"顺手带下来"这一代。

**结论**：创建路径必须在**发布 profile 之前**确定 bundle 的当代，并把它写进
`CheckpointDurableState.confirmed_generation`（= 1），使首次 checkpoint 的
`prior == confirmed` 成立。这不能靠猜，也不能靠放宽校验：
`valid_checkpoint_boundary` 拒绝得对，**错的是 durable 侧起点写成了 0**。

**因此需要的一次改动（下一步，未做）**：
- 在 `create_and_verify_bundle` 之后，用**已存在的只读检查**（M3 `image_inspector`）
  读出该 bundle 的当代；
- 把它作为初始 durable checkpoint 状态的一部分发布（`confirmed_generation`）；
- 加一条**负例**：bundle 的当代与 durable 起点不一致时，发布必须拒绝（fail-closed），
  而不是照抄或清零。

**为什么本轮停在这里**：这条改动落在 **M3 generation 语义 + durable 发布顺序**上，
M4/T1-11 的既有证据整条压在这上面；在没有把 `image_inspector` 的只读契约读完之前动手，
风险远大于收益。**T1-05 保持 PARTIAL，阻塞点已从"不知道"变成"一个已判定的等式"**。

### 8.3 具体改动清单（下一轮照此执行，不必重新定位）

**约束**：服务侧**不能**自己读 bundle（执行边界：`myrd_management` 不得链接 M3/persistence）。
所以这一代必须**由 worker 报上来**，而 worker 已经有这个数：`WorkerRuntimeStatus.committed_generation`
（`src/host/m5_worker_protocol.h:345`，写在协议 310/612）。**不需要新增 IPC 消息。**

| # | 文件 | 改动 |
|---|---|---|
| 1 | `src/management/service_core.h` | `BundleRegistrationEvidence` **追加** `std::uint64_t committed_generation{}`（带默认值，不改变既有聚合初始化） |
| 2 | `src/management/service_worker_control.cpp` `register_bundle` | 建 `BundleRegistrationEvidence` 时填入该代：优先取 create 回复里的字段；回复没有就调 `session.require_runtime_observation(context())`（它已返回 `committed_generation`） |
| 3 | `src/management/service_core.cpp`（create 发布初始 durable 状态处） | 初始 `CheckpointDurableState.confirmed_generation` 由 `evidence.bundle->committed_generation` 决定，**不再是 0** |
| 4 | `tests/model/…`（`m5.service_orchestration` / `m5.cli` 所在模型层） | 正例：新 profile 的初始 `confirmed_generation` 等于证据里的一代；**负例**：证据代与既有 durable 记录不一致时发布被拒（fail-closed），既不清零也不照抄 |
| 5 | 客机 | `integration.m5.cli_e2e` 跑通后 `-Repeat 20`；`m5.production_execution_backend` 与 `integration.m5.cli_e2e` 都要在读数里 |

**验收判据**：create 的首次 checkpoint 走到 `valid_checkpoint_boundary` 时
`prior == confirmed`（不再差 1），并且 T1-05 的整链（create → list → show → 读写 →
checkpoint → stop → **重启恢复** → forget）全绿；断电恢复属 T1-09，本轮不冒充。

**风险**：第 3 步改变的是新建 profile 的 durable 起点，落在 M4/T1-11 证据链上。
因此第 4 步的**负例**是必须的——它证明"不一致就拒绝"，而不是把起点写成一个方便的值。

### 8.4 §8.3 已实施（步骤 1–3），create 前进到"durable 状态发布"

**改动**：`IServiceRegistrationStore::publish_registration` 两个重载各追加
`std::uint64_t bundle_generation = 0`；`ProfileRegistrationStore` 用它填
`CheckpointDurableState.confirmed_generation`（原来是 `CheckpointDurableState{}` 即 0）；
`ServiceCore` 在发布前用 `worker_.require_runtime_observation(...)` **严格**取该代
（取不到就不发布、记 `register` 标记与原因），并记一行
`publishing with bundle_generation=N`。

**读数（请求 `f7ea08f6284b4b04a84ee892a6ab6a27`）**：

```
kind=2 command=disk.create phase=publish-provisioning message=publishing with bundle_generation=1
kind=7 ... phase=terminal-indeterminate message=terminal failure without a worker-action error; marker=register
```

⇒ **取代正确（1）**，`valid_checkpoint_boundary` 那一条不再被触发；
create 现在停在**下一个**位置：`publish_registration` 返回
`RegistrationCommitDisposition::indeterminate`（不是 error，因此没有拒绝日志），
即 store 内部 **`publish_state_bytes` 失败**（`profile_registration_store.cpp` 里那条
`return {indeterminate, nullopt}`）。

**下一步（已定位，未做）**：查 `publish_state_bytes` 对**新建 profile** 的失败原因
（怀疑顺序：`write_next(0, ...)` 之后状态文件尚不存在，而 `publish_state_bytes` 需要既有记录做 revision 握手），
拿到它的具体错误后再改；同时补 §8.3 步骤 4 的**正例与负例**——目前
`ProfileRegistrationStore` 在测试里**零覆盖**，这正是这条缺陷能一路潜伏的原因。

**量测范围**：T1-04 本轮仍复现（20/20 采样，最大 44 ms）。**T1-05 保持 PARTIAL。**

### 8.5 generation 起点已修好并验证；create 现在卡在注册之后的下一步

**已修**：`publish_registration` 传入 `bundle_generation`；store 用它种
`CheckpointDurableState.confirmed_generation`；`ServiceCore` 里 `intended` 快照
**用同一个种子重建**（否则 store 组合出的快照与 `intended` 不等，一次**成功的提交**会被判成
`ambiguous`——这一条已由日志确认：`publish_registration disposition=0`（committed）
紧跟 `registration applied; continuing the plan`）。

**客机读数（请求 `c52f516882c6410585d126bfb9b137ee`，日志保留在 `C:\m5\last-service.log`）**：

```
kind=1 command=disk.create phase=queued
kind=2 command=disk.create phase=running
kind=2 ... phase=publish-provisioning message=publishing with bundle_generation=1
kind=7 ... phase=publish-provisioning message=publish_registration disposition=0 (the store reported no cause)
kind=2 ... phase=publish-provisioning message=registration applied; continuing the plan
（此后 600 s 内没有任何 lifecycle 事件，只有 operation.show 查询）
```

⇒ **注册这一步通了**（此前两步缺陷都已消除：起点 0 与 intended 快照不一致），
create 现在**卡在注册之后的计划步骤**（`create_runtime_and_worker` / checkpoint），
**不是失败，是挂住**。

**旁证**：同一台机器的旧日志里有一条
`action=create-runtime code=1 step=classify_bundle detail=bundle kind does not match runtime mode`
—— 说明"注册之后再起 runtime"这一步本就会因为**bundle 已经存在**而被模式判定拒绝；
现在表现成挂住（很可能是 worker 启动/等待的有界超时在重试）。

**下一步（已定位到函数）**：
1. `bind_worker` 的 launch 模式判定：`classify_bundle` 认为 bundle kind 与 runtime mode 不符；
   创建路径在注册之后应该用**采纳**模式而不是"新建"，或按 bundle 已存在的事实选模式；
2. 若是挂住而非拒绝，先把该处的等待预算与重试次数记进日志，再决定是"拒绝得更早"还是"允许采纳"。

**量测范围**：T1-04 每轮复现（20/20 采样，最大 86 ms）。**T1-05 仍 PARTIAL。**

### 8.6 根因：create 的计划与 runtime 模式**自相矛盾**（这是本轮最有价值的读数）

`append_start_actions(plan, provisioning=true)` 给出的创建序列是：

```
1  create_and_verify_bundle      ← 这一步就把 bundle 落成"非空"（有 manifest）
2  publish_provisioning_profile
3  prove_new_uninitialized
4  create_runtime_and_worker     ← 这一步要求 bundle 是**空的**
...
```

而 `DiskRuntime::start`（`src/run/disk_runtime.cpp:132-138`）的准入是：

```cpp
if ((mode_ == DiskRuntimeMode::create && kind != DiskBundleKind::empty) ||
    (mode_ != DiskRuntimeMode::create && kind != DiskBundleKind::manifest)) { 拒绝 }
```

`make_launch_request`（`service_worker_control.cpp`）在 lifecycle=provisioning 时选
`WorkerVolumeMode::provision_new`，它映射到 **`DiskRuntimeMode::create`** ⇒ **要求空 bundle**。
但第 1 步已经写了 manifest ⇒ **第 4 步必然被拒**。

**这与本轮所有读数一致**：先前那次 `action=create-runtime code=1 step=classify_bundle
detail=bundle kind does not match runtime mode`；现在因为注册已通、流程更靠后，同一处表现为挂住
（等待/重试），而不是立刻拒绝。

**⇒ 结论：这不是环境问题，也不是 generation 问题，而是"创建计划的第 1 步与第 4 步的契约冲突"。**
两条可能的修法（需要一次设计判断，不是随手改）：
1. **第 1 步只创建目录、不写镜像**，让 runtime 在 `create` 模式下写 generation-zero image
   （与 `disk_runtime.cpp:141-143` 的 `create_generation_zero_image` 一致）；
2. **第 4 步改用"采纳已存在 bundle"的模式**（`adopt_existing`/provision-existing），
   即承认"bundle 先建、runtime 后起"是产品意图，并让 `classify_bundle` 允许
   `create`-之后的 provisioning 用 `manifest` 类型。

判据：M4 的 `myrd create` 走的是哪条——`src/run/main.cpp:96` 先 `classify_bundle` 再决定模式，
那里的行为就是"创建"的正确定义。

**为什么本轮不动它**：这会改变创建的产品语义（谁写第一张镜像），
并且必须在客机上反复验证（单轮 10 分钟以上）；在把 M4 的 create 契约读清楚之前动手，
风险是把一条已经绿的 M4 路径改坏。

**T1-05 仍 PARTIAL**；阻塞点现在是一个**契约冲突**，不是未知。

### 8.7 判据取到了：M4 的规则是"**模式跟着 `classify_bundle` 走**"，不是跟着 lifecycle 走

`src/run/main.cpp:154-156`：

```cpp
auto runtime = bundle_kind.value() == DiskBundleKind::empty
    ? DiskRuntime::create(...)     // 空 bundle ⇒ create：写 generation-zero image 并供给
    : DiskRuntime::recover(...);   // 非空 ⇒ recover：采纳已有卷
```

即：**空 bundle 才允许 create**；bundle 一旦有 manifest，正确模式就是 recover（采纳）。
而 M5 的 `make_launch_request` 是按 **lifecycle** 选的（provisioning ⇒ `provision_new` ⇒ create 模式），
**这正是与 M4 规则的偏离点**。

**因此 §8.6 的两条候选里，候选 2 与 M4 语义冲突**（它会给"非空 bundle"发明一个供给模式）。
真正一致的做法只有一条路：**让 bundle 在 create 模式下由 runtime 自己写出来**——
也就是 `create_and_verify_bundle` 只能建目录/校验，不能写镜像。

**但这又碰到第二个结**：计划第 2 步 `publish_provisioning_profile` 在 runtime 之前，
而注册需要 bundle evidence（image id / manifest / 容量）——镜像还不存在时它无从得来。

⇒ **这是 creation bootstrap 的顺序问题，必须做一次设计裁定**（三条可能的顺序，各有代价）：
1. 先注册"占位 profile"（无 image id）→ runtime 创建 → 用 worker 回报的 evidence 更新注册；
2. 先由 worker 在**未注册**的情况下创建镜像与卷 → 再注册（注册即事实，但失败回滚面更大）；
3. 保留现状，但把 `create` 模式放宽为"空或仅 generation-zero image"，并为 `provision_new`
   增加一个明确的"已有镜像、尚无卷"状态（等于改 M4 的模式定义）。

**这三条都会改动 M4/M5 的既有语义与证据链，属于需要人裁定的范围**（也正是 AGENTS/mater-plan
规定应当停下来问的那类事项）。**T1-05 保持 PARTIAL**，阻塞点已从"未知"收敛为"一个需要裁定的顺序问题"。

---

## 9. 2026-10-21：create 全链推进到 checkpoint 之后，根因已命名

**方法**：`diagnostic_trace`（`MYRD_TRACE_FILE`，本来就存在、默认关闭）逐层打点，
加上两处"失败不再被吞"的改动。**每一次都拿到读数**，没有一次靠猜。

### 9.1 走到的位置

创建计划现在**完整跑到 checkpoint 并完成**（客机读数）：

```
action-start: create-and-verify-bundle / publish-provisioning / prove-uninitialized
              create-runtime / recover-bundle / start-proxy / create-device
              locate-physical-drive / provision-gpt-ntfs / record-volume / flush-volume
              checkpoint  → action-result: checkpoint completed
```

即 generation 起点、注册、供给、flush 全部通过；**卡的是 checkpoint 之后的提交收尾**。

### 9.2 根因（有名字，不是"挂住"）

调度器把 `execute_operation` 的失败**丢掉了**（`static_cast<void>(...)`），
于是 operation 既不是成功也不是失败，客户端永远看到 `running`。
本轮把这条失败**说出来**之后，两次运行分别给出：

```
execute_operation failed: code=22022 step=service_core.checkpoint_epoch
  detail=The worker dirty epoch regressed or changed identity
execute_operation failed: code=22022 step=service_core.stop_removal_boundary
  detail=The pre-existing noncancellable stop boundary is contradictory
```

**当前主阻塞是第一条**：commit 之后服务做 epoch 发布/校验，worker 报的 dirty epoch
**比 durable 记录里的更小**，被判为"regression"。

### 9.3 为什么这条判据在 create 上必然失败（与既有注释自相矛盾）

`valid_checkpoint_boundary` 里已经写明：

> the cut epoch is deliberately NOT compared against the durable `current_dirty_epoch` …
> a re-baseline, not a regression

而 P5 之后这一步**正是**一次 re-baseline：**提交成功会把 dirty epoch 归零**。
durable 记录此时保存的是**提交前**的 cut epoch（P5 从 evidence 写入），
于是"提交后 epoch 更小"是**必然且正确**的，却被判成回归。

⇒ 修法与 P4 处那条规则一致：**提交边界上的 epoch 回退不是 regression**；
真正必须严格的是 worker 身份（pid / creation time / session generation）与 commit disposition。
这条判据应当与 `valid_checkpoint_boundary` 的注释同源，而不是各写一套。

### 9.4 顺带修掉的两个"失败被吞"（都已实测）

1. **调度器丢弃 `execute_claimed_operation` 的返回值**：现在失败会写入服务日志
   （`execute_operation failed: code/step/detail`），并且 `diagnostic_trace` 记一行。
   **这是"挂住"这个说法的真正来源**——不是死锁，是失败没人说。
2. **`ServiceLogEvent` 的 `message` 在无命令上下文时为空**：补齐 uuid/命令字段的初始化。

### 9.5 下一步（已收敛到一条判据）

在 checkpoint epoch 校验处采纳"提交即 re-baseline"：
- worker 身份（pid/creation/session）不一致 **仍然拒绝**；
- epoch 仅在**同一提交边界内**要求单调，跨提交允许回到 0；
- 加一条负例：**identity 变了但 epoch 也变了**时必须拒绝（证明不是把校验整体放宽）。

**T1-05 仍 PARTIAL**：链路已推进到 checkpoint 之后，卡点是一条可命名的判据错误。

---

## 10. 2026-10-23：create 全链通了；stop 停在 checkpoint epoch 的完成判定

### 10.1 本轮修掉并实测（请求 `32d4ba1c71b54d72a87c4d4e6cd33203`）

**epoch 判据改为只认身份**（`service_core.checkpoint_epoch`）：提交会重置 dirty epoch，
所以"提交后 epoch 更小"是 re-baseline；仅当 **correlation 不同**（换 worker / 换 checkpoint）才拒绝。

客机读数（这次 create **成功**）：

```
disk.create: operation queued → running → … → publish-ready → assign-mount → readiness-io
disk.checkpoint: operation queued → running → succeeded (phase terminal)
```

⇒ **T1-05 的 create 段与 checkpoint 段都通了**（此前从未有过）。

**夹具两处修正**：
- `disk show/checkpoint/stop` 的 `<disk>` 参数按 `resolve()` 的语义是 **display name 或 uuid**，
  **不是盘符** —— 之前用 `U:` 才得到 `profile_not_found`。已改为用本轮唯一的 profile 名。
  （**产品侧待办**：CLI 用户会自然想用盘符，`resolve` 不支持，属 T1-05 的可用性缺口，已记入 §11。）
- 失败时保留 `trace.log` 与 `service.log`。

### 10.2 当前阻塞：stop 的 checkpoint epoch 完成判定

```
execute_operation failed: code=22022 step=service_core.checkpoint_epoch
  detail=The final checkpoint state is invalid
```

位置：`service_core.cpp` 的 `complete_command(pending_plan, clean ? succeeded_clean : succeeded_dirty)`
（`if (!completed.applied())`）。**create 的第一处 checkpoint 用同一调用是通的**，
差别在于 `normal_stop`：stop 计划在 checkpoint 之后还有 `remove_mount` /
`remove_exact_device` / `stop_runtime_and_release_lock`，而这里用"checkpoint 完成"的
outcome 去完成整个 stop 计划，状态机判定不成立。

**下一步（已定位到行）**：`normal_stop` 时不要在这一步完成整个命令——
只发布"checkpoint 已确认"的 durable 状态（`completed.after` 需要换成 stop 计划在该阶段的合法状态），
让 remove 系列动作继续，由循环末尾正常完成 stop。需要先读 `complete_command` 对
`ManagementCommand::stop` 的合法 outcome，避免再猜。

**T1-05 仍 PARTIAL**，但链路已覆盖 create → list → show → 读写 → checkpoint，
只剩 stop（及其后的重启恢复与 forget）。
