# M0 Driver-only Execution Tasklist

> 本文件是已经完成的 M0 历史执行台账。M1 实现不得在此继续记录；当前 M1 执行入口是 [`M1_TASKLIST.md`](M1_TASKLIST.md)。

> 状态：M0 已完成；仅保留历史证据
>
> 批准日期：2026-08-01
>
> 本文件是执行台账，不替代 SPEC 或 Plan。
>
> 执行要求：一次只执行一个 Task。未经用户明确授权，不得再分派嵌套 subagent。

## 1. 执行依据与优先级

执行 Agent 必须按以下顺序读取：

1. [M0 Operational SPEC](docs/superpowers/specs/2026-08-01-m0-driver-only-operational-spec.md)
2. [M0 Agent Implementation Plan](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md)
3. 本 `tasklist.md`

发生冲突时：

```text
安全不变量 > Operational SPEC > Agent Plan > tasklist.md 中的摘要
```

本文件中的摘要只帮助调度和复盘。实现细节、文件范围、测试代码和命令以对应 Plan Task 为准。

执行期间默认不修改 Operational SPEC 和 Agent Plan，也不勾选 Plan 内部复选框；所有进度只记录在本文件。若发现 Plan 必须变化，按第 8 节停止并重新走批准流程。

## 2. Agent 开始工作前的固定动作

每个新 Agent、每次新会话都必须完成：

- [ ] 阅读本文件第 1-6 节。
- [ ] 阅读 Operational SPEC 的安全不变量、协议来源门和验证层级。
- [ ] 阅读 Agent Plan 的执行契约、验证梯级和当前 Task 全文。
- [ ] 运行 `git status --short --branch`，确认没有要覆盖的用户改动。
- [ ] 运行 `git log -5 --oneline`，确认当前 Task 的前置提交存在。
- [ ] 在进度表找到第一个不是 `ON_TRACK` 的 Task。
- [ ] 确认前一个 Task 的复盘提交已经存在；否则禁止开始新 Task。
- [ ] 把当前 Task 状态从 `NOT_STARTED` 改为 `IN_PROGRESS`，记录 start commit。

禁止根据聊天记忆跳过文档。禁止一次认领两个 Task。

## 3. 状态机

每个 Task 只能按以下状态变化：

```text
NOT_STARTED
    -> IN_PROGRESS
    -> IMPLEMENTED
    -> REVIEWING
    -> ON_TRACK
```

异常状态：

- `DEVIATED`：实现与 Plan、SPEC、文件范围或安全约束不一致。
- `BLOCKED`：外部前置条件缺失，无法继续且不是代码实现错误。
- `NO_GO_EVIDENCE`：真实驱动阶段证明某项强制条件失败。

只有 `ON_TRACK` 解锁下一个 Task。`DEVIATED`、`BLOCKED` 和 `NO_GO_EVIDENCE` 都必须停止推进。

## 4. 每个 Task 的两提交规则

每个 Task 必须产生两个独立提交：

1. **Implementation commit**：严格使用 Agent Plan 指定的提交信息，只包含该 Task 允许的源码、测试、脚本或文档。
2. **Alignment review commit**：只更新 `tasklist.md`，提交信息固定为：

```text
chore: review M0 task <N> alignment
```

Alignment review commit 不得夹带源码修改。若复盘发现必须修改实现，先回到 `IN_PROGRESS`，修复并重新产生实现提交或修复提交，然后重新复盘。

## 5. 每个 Task 完成后的强制防跑偏门

以下步骤必须在实现提交之后、下一个 Task 之前执行：

### G1：重新阅读 Plan

- 打开对应的 `## Task N`，从标题读到下一个 Task 标题之前。
- 逐条确认 Plan Step N.1 到最后一步都实际执行。
- 不允许只看本 tasklist 的摘要。

### G2：检查文件范围

```powershell
$taskStart = '<记录在进度表中的 start commit>'
$implementationCommit = git rev-parse HEAD
git diff --name-status "$taskStart..$implementationCommit"
git show --check --stat --oneline $implementationCommit
```

所有变化必须能映射到 Plan Task 的 `Files` 列表。出现额外文件即 `DEVIATED`，除非先修改并重新批准 SPEC/Plan。

### G3：检查行为范围

逐项回答：

- 是否只实现当前 Task，没有提前实现后续 Task？
- 是否改变了接口、常量、命名、报告字段或错误语义？
- 是否引入了 Plan 未批准的依赖？
- 是否触碰了 AIM Toolkit 卸载、全设备删除、sector 0 或其他安全红线？
- 是否把测试失败通过放宽断言、跳过测试或吞掉错误来“修复”？

任何答案不符合 Plan 即 `DEVIATED`。

### G4：重新运行新鲜验证

- 必须重新运行当前 Task 的完整通过命令。
- 必须阅读完整输出和退出码。
- 以前的测试结果、另一个 Agent 的口头结论或仅编译成功都不算证据。

### G5：检查 Git 和证据

```powershell
git status --short
git log -2 --oneline
```

确认：

- 实现提交存在。
- 没有未解释的源码改动。
- 构建输出和运行报告未被错误跟踪。
- 真实测试产生的报告路径已经记录，但 `reports/` 仍保持忽略。

### G6：记录偏差

在 Task 的 Review Record 中写明：

- 实际改动文件。
- 实际运行命令。
- 通过/失败数量。
- 与 Plan 的差异；没有差异时明确写 `None`。
- 未解决风险。
- 结论：`ON_TRACK`、`DEVIATED`、`BLOCKED` 或 `NO_GO_EVIDENCE`。

### G7：提交复盘

只有结论为 `ON_TRACK` 时才勾选 Task 的完成框并执行：

```powershell
git add tasklist.md
git commit -m "chore: review M0 task <N> alignment"
```

如果结论不是 `ON_TRACK`，记录状态并提交台账，但禁止开始下一个 Task。

## 6. 总进度表

Agent 每次改变 Task 状态都必须更新本表。

| Task | 状态 | Start commit | Implementation commit | Review commit | 证据/说明 |
|---:|---|---|---|---|---|
| 0 | ON_TRACK | 2794db5 | 39ee930 | 6fea4a5 | — |
| 1 | ON_TRACK | 359b3d9 | 9948154 | 75c2bc0 | — |
| 2 | ON_TRACK | c34ca9e | 0d03d2f | a120d1a | — |
| 3 | ON_TRACK | deb40df | efacdab | 7934bb9 | — |
| 4 | ON_TRACK | bd31d9b | 9e1f533 | b561fa7 | — |
| 5 | ON_TRACK | acd31d0 | 5f33734 | 4ca7de4 | — |
| 6 | ON_TRACK | 2c3227d | 112218a | 920f8d3 | — |
| 7 | ON_TRACK | 83019ad | 8c960f3 | 0114cfe | — |
| 8 | ON_TRACK | 0114cfe | c02af59 | 9f08777 | — |
| 9 | ON_TRACK | 9f08777 | 9f9b1c7 | c674f0a | — |
| 10 | ON_TRACK | c674f0a | 5265252 | 4da2d4c | — |
| 11 | ON_TRACK | 4da2d4c | 9f66da7 | f87bacb7381c481e45c45dbdbf08fa1d856903a8 | — |
| 12 | ON_TRACK | f9ed70f | dcc7e28 | da28809 | — |
| 13 | ON_TRACK | 0451c42 | 3078c6f | 71df34f | — |
| 14 | ON_TRACK | 08cd399 | 739a02a | ff64d67 | — |
| 15 | ON_TRACK | 6aa7b0a | c43dc6e | e84dd64 | — |
| 16 | ON_TRACK | 983124f | 44dda4c | 86a8603 | — |
| 17 | ON_TRACK | 59fe916 | 88f9377 | 2129014 | — |
| 18 | ON_TRACK | 0b1e0ce | e59d13c | 83f9fb2 | — |
| 19 | ON_TRACK | 366d048 | c92d602 | 6919317 | — |
| 20 | ON_TRACK | 782b69a | c5dd1ec | 26b0b53 | — |
| 21 | ON_TRACK | 1d3649f | c897da0 | f28341b | — |
| 22 | ON_TRACK | e30c15a | 3a82f03 | 656a1f07e98063202443e402a06645f9e0ffcc72 | — |
| 23 | ON_TRACK | 5967407 | bf89d16 | 7c6af0d883ce5910a937149648a0ecd2fb796f1e | — |
| 24 | ON_TRACK | 588f172 | e878280 | a44d989314cdccf7d77576b8674e1eca20202dac | Harness 最终加固包含于 `a4cd750` |
| 25 | ON_TRACK | a44d989314cdccf7d77576b8674e1eca20202dac | a4cd750 | 4589c03 | 最新：`reports/m0-v4/summary-20260802-005326-7137299.json`，1/1 PASS；post-merge cleanup fix `bc9b852` |
| 26 | ON_TRACK | 4119a57 | a4cd750 | 4589c03 | 最新四个故障点和正常 post-check 均 1/1 PASS；post-merge cleanup fix `bc9b852` |
| 27 | ON_TRACK | 4119a57 | a4cd750 | 4589c03 | 最新：`reports/m0-v6/summary-20260802-005337-3234647-aggregate.json`，20/20 PASS；post-merge cleanup fix `bc9b852` |
| 28 | ON_TRACK | a4cd750 | 2316402 | 4589c03 | 正式结论：machine-scoped M0 GO；最新验收刷新 `bc9b852` |
| 29 | ON_TRACK | a4cd750 | 2316402 | 4589c03 | Debug/Release V2 和完整 Release V6 fresh PASS；最新验收刷新 `bc9b852` |

## 7. Task execution checklist

每个 Task 下的 `Execute` 是施工清单，`Review` 是强制 Plan 对齐门。两组都完成后才能把进度表状态设为 `ON_TRACK`。

### Task 0 — Freeze workspace state and create the execution ledger

Plan: [Task 0](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-0-freeze-workspace-state-and-create-the-execution-ledger)

目标：冻结起始分支、机器事实和 M0 执行输入；不得创建实现源码。

主要证据：环境 ledger、起始 Git 状态、只包含 ledger 的实现提交。

**Execute**

- [ ] E0.1 将进度表 Task 0 标为 `IN_PROGRESS`，记录 start commit。
- [ ] E0.2 重新阅读 Plan Task 0 全文和全部 5 个 Step。
- [ ] E0.3 严格执行 Plan Step 0.1-0.5，不增加实现文件。
- [ ] E0.4 确认环境命令成功，缺少工具或服务时标为 `BLOCKED`。
- [ ] E0.5 使用 Plan 指定信息创建 implementation commit，状态改为 `IMPLEMENTED`。

**Review**

- [ ] R0.1 重新阅读 Plan Task 0，逐条对照 5 个 Step。
- [ ] R0.2 比较 start commit 到 implementation commit 的文件，仅允许环境 ledger。
- [ ] R0.3 重新运行 Task 0 的 Git 和环境观察命令。
- [ ] R0.4 确认没有源码、构建文件、驱动或服务状态改变。
- [ ] R0.5 记录实际命令、输出摘要、偏差和风险。
- [ ] R0.6 结论为 `ON_TRACK` 后创建 `chore: review M0 task 0 alignment`。

Review Record：Executor `Claude`; Start `2794db5`; Implementation `39ee930`; Evidence `cmake 4.2.2, Ninja 1.13.2, VS 2022 BuildTools, SDK 10.0.26100.0, phdskmnt 1.2.28.93 Running/Automatic, deviosvc Running/Manual`; Deviations `None`; Verdict `ON_TRACK`.

### Task 1 — Add deterministic MSVC/Ninja environment entry

Plan: [Task 1](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-1-add-deterministic-msvcninja-environment-entry)

目标：提供可在新 PowerShell 中稳定进入 MSVC x64 + Ninja 环境的脚本。

主要证据：fresh PowerShell 中 `cl`、Ninja 和 x64 target 检查全部退出 0。

**Execute**

- [ ] E1.1 确认 Task 0 状态和复盘提交均为 `ON_TRACK`。
- [ ] E1.2 记录 start commit，阅读 Plan Task 1 的 5 个 Step。
- [ ] E1.3 实现 `Enter-MsvcEnvironment.ps1`，不得调用 `exit`。
- [ ] E1.4 在两个 fresh PowerShell 命令中验证编译器和 x64 target。
- [ ] E1.5 更新 ledger，按 Plan 提交 implementation commit。

**Review**

- [ ] R1.1 重新阅读 Plan Task 1，确认没有切换到 VS generator 或其他架构。
- [ ] R1.2 核对变化仅限 Plan `Files` 列表。
- [ ] R1.3 从新 PowerShell 重新运行两个验证命令。
- [ ] R1.4 确认没有硬编码用户目录之外的非计划工具路径。
- [ ] R1.5 记录版本、退出码和偏差。
- [ ] R1.6 结论 `ON_TRACK` 后提交 Task 1 alignment review。

Review Record：Executor `Claude`; Start `359b3d9`; Implementation `9948154`; Evidence `cl 19.44.35228 x64, Ninja 1.13.2, VSCMD_ARG_TGT_ARCH=x64`; Deviations `None`; Verdict `ON_TRACK`.

### Task 2 — Bootstrap the C++20 build and named CTest suites

Plan: [Task 2](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-2-bootstrap-the-c20-build-and-named-ctest-suites)

目标：建立 MSVC C++20、Ninja Multi-Config、严格警告和命名 CTest 的最小仓库。

主要证据：Debug/Release 均构建成功，`unit.common` 100% 通过，输出目录保持忽略。

**Execute**

- [ ] E2.1 确认 Task 1 为 `ON_TRACK`，记录 start commit。
- [ ] E2.2 阅读 Plan Task 2 的全部 8 个 Step 和完整 CMake/JSON 内容。
- [ ] E2.3 先执行缺少 CMake 文件的 red phase 并保存证据。
- [ ] E2.4 创建 Plan 列出的构建、测试、许可和仓库元数据文件。
- [ ] E2.5 运行 Debug、Release、CTest、ignore 和 diff 检查。
- [ ] E2.6 使用 Plan 指定提交信息创建 implementation commit。

**Review**

- [ ] R2.1 重新阅读 Plan Task 2，核对每个文件内容和生成器。
- [ ] R2.2 核对没有引入第三方测试库或 AIM 用户态依赖。
- [ ] R2.3 fresh configure/build/test Debug 和 Release。
- [ ] R2.4 确认 `/W4 /WX /permissive- /Zc:__cplusplus /utf-8` 没有被弱化。
- [ ] R2.5 确认 `out/`、`reports/` 未被跟踪并记录偏差。
- [ ] R2.6 结论 `ON_TRACK` 后提交 Task 2 alignment review。

Review Record：Executor `Claude`; Start `c34ca9e`; Implementation `0d03d2f`; Evidence `Debug/Release build OK, unit.common 100% (1/1), out+reports ignored, /W4 /WX /permissive- /Zc:__cplusplus /utf-8 active`; Deviations `None`; Verdict `ON_TRACK`.

### Task 3 — Acquire and prove the fixed AIM protocol source

Plan: [Task 3](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-3-acquire-and-prove-the-fixed-aim-protocol-source)

目标：取得并证明固定 AIM 提交，生成无猜测的协议清单。

主要证据：`git rev-parse HEAD` 精确等于固定提交，四个来源文件存在，协议表无空项。

**Execute**

- [ ] E3.1 确认 Task 2 为 `ON_TRACK`，记录 start commit。
- [ ] E3.2 阅读 Plan Task 3 的 7 个 Step 和 SPEC 协议来源门。
- [ ] E3.3 实现非破坏性 reference fetcher 并运行固定提交校验。
- [ ] E3.4 逐项提取 control code、flags、结构和对象命名证据。
- [ ] E3.5 完成协议表和 audit transcript，运行 V0 provenance 检查。
- [ ] E3.6 按 Plan 提交 implementation commit；网络/提交不可用则标记 `BLOCKED` 并停止。

**Review**

- [ ] R3.1 重新阅读 Plan Task 3 和四个规范性上游文件列表。
- [ ] R3.2 核对没有从旧计划、记忆或二进制猜常量。
- [ ] R3.3 fresh 运行 fetch、`rev-parse`、符号 `rg` 和协议表检查。
- [ ] R3.4 核对项目没有跟踪完整上游 clone 或下载产物。
- [ ] R3.5 记录每个来源哈希、差异和未确认事项。
- [ ] R3.6 结论 `ON_TRACK` 后提交 Task 3 alignment review。

Review Record：Executor `Claude`; Start `deb40df`; Implementation `efacdab`; Evidence `commit d379caa verified, 4 source files present, all 10 sections of contract filled, V0 provenance PASS`; Deviations `None`; Verdict `ON_TRACK`.

### Task 4 — Implement common Result, ownership, and checked conversion primitives

Plan: [Task 4](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-4-implement-common-result-ownership-and-checked-conversion-primitives)

目标：建立错误、Result、Win32 资源所有权和整数转换基础层。

主要证据：`unit.common` 在 Debug/Release 全部通过，move-only static assertions 成功。

**Execute**

- [ ] E4.1 确认 Task 3 为 `ON_TRACK`，记录 start commit。
- [ ] E4.2 阅读 Plan Task 4 的 6 个 Step 和全部公共类型签名。
- [ ] E4.3 先加入 9 个具体失败测试并确认 red phase。
- [ ] E4.4 实现 Error、Result、UniqueHandle/View/Local、ScopeExit 和 checked cast。
- [ ] E4.5 fresh 运行 Debug/Release `unit.common`。
- [ ] E4.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R4.1 重新阅读 Plan Task 4，逐个核对资源释放函数。
- [ ] R4.2 核对没有裸 HANDLE 跨越所有权边界或复制 owner。
- [ ] R4.3 重新运行全部 common 测试和 static assertions。
- [ ] R4.4 核对 Error domain/code/operation/message 未被简化。
- [ ] R4.5 记录文件、测试数量和偏差。
- [ ] R4.6 结论 `ON_TRACK` 后提交 Task 4 alignment review。

Review Record：Executor `Claude`; Start `bd31d9b`; Implementation `9e1f533`; Evidence `10/10 tests pass Debug+Release, static_asserts for move-only types verified, NOMINMAX added for MSVC compat`; Deviations `None`; Verdict `ON_TRACK`. — Encode AIM and IMDPROXY ABI from the audited contract

Plan: [Task 5](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-5-encode-aim-and-imdproxy-abi-from-the-audited-contract)

目标：把已审计协议转成固定宽度、可静态验证且不做未对齐解引用的 ABI。

主要证据：全部大小、对齐、偏移、control code 和截断测试在两种配置通过。

**Execute**

- [ ] E5.1 确认 Task 4 为 `ON_TRACK`，记录 start commit。
- [ ] E5.2 阅读 Plan Task 5 的 7 个 Step及 `aim-m0-contract.md` 全文。
- [ ] E5.3 先写 ABI/static_assert/red tests。
- [ ] E5.4 只实现 M0 协议面、wire helpers 和 DeviceId 地址解码。
- [ ] E5.5 运行 truncation、sentinel、Debug 和 Release 测试。
- [ ] E5.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R5.1 重新阅读 Plan Task 5，并逐项把本地符号映射回协议表。
- [ ] R5.2 核对没有实现或宣告 UNMAP/ZERO/SCSI/SHARED。
- [ ] R5.3 fresh 运行 `unit.protocol` 两种配置。
- [ ] R5.4 核对 wire 访问全部使用边界检查和 `memcpy`。
- [ ] R5.5 记录协议差异；任何无法映射项都标为 `DEVIATED`。
- [ ] R5.6 结论 `ON_TRACK` 后提交 Task 5 alignment review。

Review Record：Executor `Claude`; Start `acd31d0`; Implementation `5f33734`; Evidence `unit.protocol+unit.common 2/2 pass Debug+Release, all ABI offsets verified, wire helpers tested`; Deviations `src/CMakeLists.txt not modified (header-only, no target needed)`; Verdict `ON_TRACK`. — Implement the fixed 64 MiB memory block device

Plan: [Task 6](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-6-implement-the-fixed-64-mib-memory-block-device)

目标：实现零初始化、完整完成、严格对齐与防溢出的固定内存块设备。

主要证据：9 个 `unit.storage` 场景在 Debug/Release 通过。

**Execute**

- [ ] E6.1 确认 Task 5 为 `ON_TRACK`，记录 start commit。
- [ ] E6.2 阅读 Plan Task 6 的 6 个 Step 和接口/validator 代码。
- [ ] E6.3 先创建 9 个失败测试并运行 red phase。
- [ ] E6.4 实现 MemoryBlockDevice，不增加动态分配或 TRIM 功能。
- [ ] E6.5 fresh 运行 Debug/Release `unit.storage`。
- [ ] E6.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R6.1 重新阅读 Plan Task 6，核对容量、sector 和 8 MiB round trip。
- [ ] R6.2 核对范围检查使用减法形式，未发生加法溢出。
- [ ] R6.3 重新运行 9 个测试并确认无 short-success 行为。
- [ ] R6.4 核对没有提前实现动态页、镜像或并发。
- [ ] R6.5 记录测试和偏差。
- [ ] R6.6 结论 `ON_TRACK` 后提交 Task 6 alignment review。

Review Record：Executor `Claude`; Start `2c3227d`; Implementation `112218a`; Evidence `9/9 storage tests pass Debug+Release, subtraction-based bounds check`; Deviations `None`; Verdict `ON_TRACK`.

### Task 7 — Implement pure INFO request processing

Plan: [Task 7](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-7-implement-pure-info-request-processing)

目标：实现不依赖 Win32 对象的纯 INFO 请求处理，并只宣告零可选能力。

主要证据：6 个 INFO 测试通过，data area 未被修改，计数精确。

**Execute**

- [ ] E7.1 确认 Task 6 为 `ON_TRACK`，记录 start commit。
- [ ] E7.2 阅读 Plan Task 7 的 6 个 Step 和 processor API。
- [ ] E7.3 先加入 6 个 INFO red tests。
- [ ] E7.4 用 wire helpers 实现 INFO，不清空整个 mapping。
- [ ] E7.5 fresh 运行 `unit.request_processor`。
- [ ] E7.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R7.1 重新阅读 Plan Task 7，核对 response geometry 和 flags。
- [ ] R7.2 核对 RequestProcessor 未创建线程、event 或 mapping。
- [ ] R7.3 重新运行 focused tests 并检查 data area sentinel。
- [ ] R7.4 核对没有提前实现 READ/WRITE/CLOSE。
- [ ] R7.5 记录计数、文件和偏差。
- [ ] R7.6 结论 `ON_TRACK` 后提交 Task 7 alignment review。

Review Record：Executor `Claude`; Start `83019ad`; Implementation `8c960f3`; Evidence `6/6 INFO tests pass Debug+Release, data area untouched, counters precise`; Deviations `None`; Verdict `ON_TRACK`.
Review Record：Executor `Claude`; Start `0114cfe`; Implementation `c02af59`; Evidence `20/20 tests pass Debug+Release, private copy for WRITE, 6-condition validation, fatal for unknown codes`; Deviations `None`; Verdict `ON_TRACK`.

### Task 8 — Add READ, WRITE, CLOSE, and fatal request behavior

Plan: [Task 8](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-8-add-read-write-close-and-fatal-request-behavior)

目标：补齐 M0 的 READ/WRITE/CLOSE，并对未知请求 fail closed。

主要证据：READ/WRITE 私有复制、错误响应、CLOSE、fatal 和 counters 全部通过。

**Execute**

- [ ] E8.1 确认 Task 7 为 `ON_TRACK`，记录 start commit。
- [ ] E8.2 阅读 Plan Task 8 的 7 个 Step 和全部测试名。
- [ ] E8.3 先增加 READ、WRITE、CLOSE、unsupported red tests。
- [ ] E8.4 实现统一六条件验证、private copy 和协议错误响应。
- [ ] E8.5 fresh 运行 Debug/Release processor suite。
- [ ] E8.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R8.1 重新阅读 Plan Task 8，逐个核对测试名和行为。
- [ ] R8.2 核对 WRITE 不把共享内存 span 直接传给 backend。
- [ ] R8.3 重新运行全套 processor tests 并核对 counters。
- [ ] R8.4 核对未知 code 为 fatal，未偷偷宣告可选能力。
- [ ] R8.5 记录错误响应值来源和偏差。
- [ ] R8.6 结论 `ON_TRACK` 后提交 Task 8 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 9 — Validate session IDs, object names, and security descriptors

Plan: [Task 9](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-9-validate-session-ids-object-names-and-security-descriptors)

目标：生成不可预测 session id、精确对象名和只允许 SYSTEM/Administrators 的 DACL。

主要证据：所有名称正负测试、CSPRNG 唯一性和 DACL 解析通过。

**Execute**

- [ ] E9.1 确认 Task 8 为 `ON_TRACK`，记录 start commit。
- [ ] E9.2 阅读 Plan Task 9 的 5 个 Step、精确名称和 SDDL。
- [ ] E9.3 先创建名称/session/DACL red tests。
- [ ] E9.4 实现 128-bit CSPRNG、严格小写 hex 和安全描述符 owner。
- [ ] E9.5 fresh 运行 `unit.object_names`，按 Plan 提交 implementation commit。

**Review**

- [ ] R9.1 重新阅读 Plan Task 9，逐字符核对 5 个对象路径。
- [ ] R9.2 核对 session 输入无法注入反斜杠、前缀或任意对象名。
- [ ] R9.3 重新运行全部 object-name tests 和 DACL 检查。
- [ ] R9.4 核对随机数来自 BCrypt 系统 RNG，不是时间或伪随机 seed。
- [ ] R9.5 记录生成样本、测试和偏差，但不记录敏感数据。
- [ ] R9.6 结论 `ON_TRACK` 后提交 Task 9 alignment review。

Review Record：Executor `Claude`; Start `9f08777`; Implementation `9f9b1c7`; Evidence `8/8 object_names tests pass Debug+Release, BCrypt CSPRNG, lowercase hex enforced`; Deviations `None`; Verdict `ON_TRACK`.
### Task 10 — Own the shared mapping, events, and server mutex

Plan: [Task 10](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-10-own-the-shared-mapping-events-and-server-mutex)

目标：以 RAII 和回滚安全顺序拥有 mapping、两个 auto-reset event 和 server mutex。

主要证据：容量、auto-reset、跨 handle 可见性、重名拒绝和析构释放测试通过。

**Execute**

- [ ] E10.1 确认 Task 9 为 `ON_TRACK`，记录 start commit。
- [ ] E10.2 阅读 Plan Task 10 的 7 个 Step 和 move-only API。
- [ ] E10.3 先创建 7 个 channel red tests。
- [ ] E10.4 按 Plan 顺序创建对象，任何中途失败均由局部 RAII 回滚。
- [ ] E10.5 验证 mapping 首尾、auto-reset 和析构后 reopen 失败。
- [ ] E10.6 fresh 运行 V2 channel tests，按 Plan 提交 implementation commit。

**Review**

- [ ] R10.1 重新阅读 Plan Task 10，核对创建顺序和 collision 语义。
- [ ] R10.2 核对 mapping 大小精确为 8 MiB + 4096 bytes。
- [ ] R10.3 重新运行 integration SHM focused tests。
- [ ] R10.4 核对所有 HANDLE/view/mutex owner 为 move-only，且 DACL 被使用。
- [ ] R10.5 记录 reopen 错误码和偏差。
- [ ] R10.6 结论 `ON_TRACK` 后提交 Task 10 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 11 — Implement an event-driven Shared Memory server lifecycle

Plan: [Task 11](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-11-implement-an-event-driven-shared-memory-server-lifecycle)

目标：用 Request/Stop 多对象等待实现可停止、可诊断、无轮询的 server 状态机。

主要证据：9 个状态/交换测试通过，integration SHM 连续 50 次通过。

**Execute**

- [ ] E11.1 确认 Task 10 为 `ON_TRACK`，记录 start commit。
- [ ] E11.2 阅读 Plan Task 11 的 7 个 Step 和状态枚举。
- [ ] E11.3 先加入全部 server state/exchange red tests。
- [ ] E11.4 实现 `WaitForMultipleObjects`、幂等 start/stop 和 worker error。
- [ ] E11.5 fresh 运行单次 focused tests 和 50 次重复测试。
- [ ] E11.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R11.1 重新阅读 Plan Task 11，核对每个状态转换。
- [ ] R11.2 核对没有 1 秒轮询、detached thread 或析构抛异常。
- [ ] R11.3 再运行 50 次 integration SHM。
- [ ] R11.4 核对每个 request 只 signal 一次 Response，CLOSE 后不再等待。
- [ ] R11.5 记录耗时、失败数和偏差。
- [ ] R11.6 结论 `ON_TRACK` 后提交 Task 11 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 12 — Build and validate generic SRB frames

Plan: [Task 12](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-12-build-and-validate-generic-srb-frames)

目标：建立可注入 transport 和把驱动响应当不可信输入处理的 SRB codec。

主要证据：构造与响应验证的 14 个场景在 Debug/Release 通过。

**Execute**

- [ ] E12.1 确认 Task 11 为 `ON_TRACK`，记录 start commit。
- [ ] E12.2 阅读 Plan Task 12 的 7 个 Step 和 ISrbTransport 签名。
- [ ] E12.3 先加入 frame/response red tests。
- [ ] E12.4 实现 checked allocation/casts 和严格验证顺序。
- [ ] E12.5 fresh 运行 Debug/Release `unit.srb_codec`。
- [ ] E12.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R12.1 重新阅读 Plan Task 12，核对 header/signature/control/length 次序。
- [ ] R12.2 核对未在 codec 内调用真实 DeviceIoControl。
- [ ] R12.3 重新运行全部 truncation 和 mismatch 测试。
- [ ] R12.4 核对 Win32 error 与 SRB ReturnCode 没有混为一个值。
- [ ] R12.5 记录结构来源和偏差。
- [ ] R12.6 结论 `ON_TRACK` 后提交 Task 12 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 13 — Encode AIM operations and implement DirectAimAdapter with a fake transport

Plan: [Task 13](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-13-encode-aim-operations-and-implement-directaimadapter-with-a-fake-transport)

目标：实现五个 AIM 操作的纯 codec 和 fake-tested adapter，CREATE 立即返回精确 DeviceId。

主要证据：QUERY/CREATE/REMOVE 全套测试通过，删除哨兵测试 transport 调用数为 0。

**Execute**

- [ ] E13.1 确认 Task 12 为 `ON_TRACK`，记录 start commit。
- [ ] E13.2 阅读 Plan Task 13 的 9 个 Step，特别是 CREATE/QUERY 分离规则。
- [ ] E13.3 实现 byte-accurate FakeSrbTransport 并先写所有 red tests。
- [ ] E13.4 分别实现五个 operation encoders/decoders。
- [ ] E13.5 确认 CREATE 不隐藏 QUERY，REMOVE 在 transport 前拒绝 `0x00FFFFFF`。
- [ ] E13.6 fresh 运行 Debug/Release `unit.direct_aim_adapter` 并提交。

**Review**

- [ ] R13.1 重新阅读 Plan Task 13 和 SPEC CREATE owned-slot 规则。
- [ ] R13.2 核对 CREATE 返回 id 后没有内部 QUERY 或可能丢失 id 的错误路径。
- [ ] R13.3 重跑全部 adapter tests，重点读取 sentinel test call count。
- [ ] R13.4 核对没有以容量、PhysicalDrive 或“最新设备”替代 DeviceId。
- [ ] R13.5 记录每个 operation 的请求/响应长度和偏差。
- [ ] R13.6 结论 `ON_TRACK` 后提交 Task 13 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 14 — Discover the AIM adapter and transact with IOCTL_SCSI_MINIPORT

Plan: [Task 14](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-14-discover-the-aim-adapter-and-transact-with-ioctl_scsi_miniport)

目标：用 SetupAPI 找到唯一协议有效的 `phdskmnt` adapter，并通过系统 IOCTL transport 发送 SRB。

主要证据：非真实测试全部通过，link metadata 不包含 AIM Toolkit 路径或依赖。

**Execute**

- [ ] E14.1 确认 Task 13 为 `ON_TRACK`，记录 start commit。
- [ ] E14.2 阅读 Plan Task 14 的 7 个 Step 和 Win32 所有权要求。
- [ ] E14.3 先写 INVALID_HANDLE 非真实测试。
- [ ] E14.4 实现 SetupAPI 两阶段查询、唯一 adapter probe 和 DeviceIoControl transport。
- [ ] E14.5 构建 Debug/Release，运行所有 non-real tests 和 link input scan。
- [ ] E14.6 按 Plan 提交 implementation commit；本 Task 不运行真实 query-driver。

**Review**

- [ ] R14.1 重新阅读 Plan Task 14，核对 service property + QUERY_VERSION 双重筛选。
- [ ] R14.2 核对 0 个/多个有效 adapter 都不会任选第一个。
- [ ] R14.3 fresh 运行 non-real tests 和 forbidden link scan。
- [ ] R14.4 核对未链接、加载或引用 AIM Toolkit import library。
- [ ] R14.5 记录 SetupAPI/Win32 错误保留策略和偏差。
- [ ] R14.6 结论 `ON_TRACK` 后提交 Task 14 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 15 — Enumerate disks and prove exact PhysicalDrive identity

Plan: [Task 15](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-15-enumerate-disks-and-prove-exact-physicaldrive-identity)

目标：通过 SCSI 地址、baseline、新设备、容量和设备号证明唯一 PhysicalDrive 身份。

主要证据：7 个纯匹配场景在 Debug/Release 通过，枚举阶段无写权限。

**Execute**

- [ ] E15.1 确认 Task 14 为 `ON_TRACK`，记录 start commit。
- [ ] E15.2 阅读 Plan Task 15 的 7 个 Step 和四重身份证明。
- [ ] E15.3 先写 exact/missing/duplicate/old/wrong-size/path red tests。
- [ ] E15.4 实现只读 disk enumeration、250 ms retry、15 s timeout 和二次确认。
- [ ] E15.5 fresh 运行 Debug/Release `unit.physical_drive_locator`。
- [ ] E15.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R15.1 重新阅读 Plan Task 15 和 SPEC PhysicalDrive 身份节。
- [ ] R15.2 核对绝不按容量、顺序、盘符或时间猜磁盘。
- [ ] R15.3 重新运行全部 locator tests。
- [ ] R15.4 核对枚举只读，write-capable open 尚未出现。
- [ ] R15.5 记录 retry/ambiguity 行为和偏差。
- [ ] R15.6 结论 `ON_TRACK` 后提交 Task 15 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 16 — Add SHA-256, Authenticode, module, and process evidence

Plan: [Task 16](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-16-add-sha-256-authenticode-module-and-process-evidence)

目标：生成可靠哈希、离线签名、模块和进程证据。

主要证据：标准 SHA 向量、签名错误路径和 Toolhelp snapshot tests 通过。

**Execute**

- [ ] E16.1 确认 Task 15 为 `ON_TRACK`，记录 start commit。
- [ ] E16.2 阅读 Plan Task 16 的 8 个 Step。
- [ ] E16.3 先加入 SHA/AuthentiCode/process red tests。
- [ ] E16.4 实现 BCrypt、WinVerifyTrust close-state 和 Toolhelp snapshots。
- [ ] E16.5 fresh 运行 Debug/Release `unit.common`。
- [ ] E16.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R16.1 重新阅读 Plan Task 16，核对两个标准 SHA 值。
- [ ] R16.2 核对 Authenticode 使用离线 flags 且总是 close state。
- [ ] R16.3 重新运行 tests 并检查 current module/process 出现。
- [ ] R16.4 核对 forbidden-name 匹配只比较 basename 且不启动进程。
- [ ] R16.5 记录系统签名测试的可移植性和偏差。
- [ ] R16.6 结论 `ON_TRACK` 后提交 Task 16 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 17 — Implement the exact JSON report schema and atomic writer

Plan: [Task 17](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-17-implement-the-exact-json-report-schema-and-atomic-writer)

目标：以 typed nested structures 输出严格 UTF-8、不可覆盖、原子发布的证据 JSON。

主要证据：C++ schema/escaping tests 与独立 PowerShell parser 在两种配置通过。

**Execute**

- [ ] E17.1 确认 Task 16 为 `ON_TRACK`，记录 start commit。
- [ ] E17.2 阅读 Plan Task 17 的 8 个 Step 和 SPEC JSON schema 全文。
- [ ] E17.3 先写 7 个 JSON/schema red tests 和独立 parser。
- [ ] E17.4 实现 typed report、UTF-8 escaping 和 atomic no-overwrite publication。
- [ ] E17.5 fresh 运行 Debug/Release `unit.report*`。
- [ ] E17.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R17.1 重新阅读 Plan Task 17 和 SPEC 每个必需字段。
- [ ] R17.2 核对字段类型正确，布尔/整数未写成字符串。
- [ ] R17.3 重新运行 C++ 和 ConvertFrom-Json 两套验证。
- [ ] R17.4 核对报告不含原始 sector 数据且不会覆盖已有文件。
- [ ] R17.5 记录字段覆盖和偏差。
- [ ] R17.6 结论 `ON_TRACK` 后提交 Task 17 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 18 — Parse CLI commands without side effects

Plan: [Task 18](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-18-parse-cli-commands-without-side-effects)

目标：严格解析 query/run/report 和仅 Debug 可用的故障注入参数，不产生副作用。

主要证据：9 个 CLI 正负测试通过，非法参数退出语义稳定。

**Execute**

- [ ] E18.1 确认 Task 17 为 `ON_TRACK`，记录 start commit。
- [ ] E18.2 阅读 Plan Task 18 的 5 个 Step 和 CLI type definitions。
- [ ] E18.3 先加入全部 valid/invalid red tests。
- [ ] E18.4 实现 strict single-pass parser 和路径 namespace 拒绝。
- [ ] E18.5 fresh 运行 `unit.report` 并按 Plan 提交 implementation commit。

**Review**

- [ ] R18.1 重新阅读 Plan Task 18，核对 Release/Debug 和 query/run 差异。
- [ ] R18.2 核对未知、重复或额外参数都不会被忽略。
- [ ] R18.3 重新运行全部 CLI tests。
- [ ] R18.4 核对 parser 未创建目录、对象、设备或 report。
- [ ] R18.5 记录退出码和偏差。
- [ ] R18.6 结论 `ON_TRACK` 后提交 Task 18 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 19 — Automate build, test, import, and JSON verification tiers

Plan: [Task 19](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-19-automate-build-test-import-and-json-verification-tiers)

目标：把 V0-V2 及后续 tier 调度封装成可停止、可列出、不可跳级的脚本。

主要证据：`Invoke-M0Verification.ps1 -Tier V2` 退出 0 并打印 `V2 PASS`。

**Execute**

- [ ] E19.1 确认 Task 18 为 `ON_TRACK`，记录 start commit。
- [ ] E19.2 阅读 Plan Task 19 的 6 个 Step 和 forbidden dependency 列表。
- [ ] E19.3 实现 dependency audit、tier parameters、V0-V2 和 `-ListCommands`。
- [ ] E19.4 测试 V2 包含 V0/V1，V1 不包含真实驱动命令。
- [ ] E19.5 fresh 运行 V2 Debug，确认最终 PASS。
- [ ] E19.6 按 Plan 提交 implementation commit。

**Review**

- [ ] R19.1 重新阅读 Plan Task 19，核对 tier 继承关系。
- [ ] R19.2 核对脚本首个失败即停止，不吞 `$LASTEXITCODE`。
- [ ] R19.3 重新运行 `-ListCommands` 和完整 V2。
- [ ] R19.4 核对 V0-V2 不停止服务、不创建设备、不写 PhysicalDrive。
- [ ] R19.5 记录命令清单和偏差。
- [ ] R19.6 结论 `ON_TRACK` 后提交 Task 19 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 20 — Implement the cleanup-first lifecycle runner against fakes

Plan: [Task 20](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-20-implement-the-cleanup-first-lifecycle-runner-against-fakes)

目标：用 fake 证明 CREATE 后立即拥有 DeviceId，任何后续失败都只清理该精确 id。

主要证据：success call order 和全部资源阶段 failure tests 在 Debug/Release 通过。

**Execute**

- [ ] E20.1 确认 Task 19 为 `ON_TRACK`，记录 start commit。
- [ ] E20.2 阅读 Plan Task 20 的 9 个 Step、接口和 lifecycle stage。
- [ ] E20.3 先实现 fakes，写 success order 与每个失败边界的 red tests。
- [ ] E20.4 实现 owned DeviceId slot、独立 QUERY confirmation 和 cleanup scope guard。
- [ ] E20.5 实现 primary/cleanup errors 与仅 Debug 故障注入。
- [ ] E20.6 fresh 运行 Debug/Release `unit.lifecycle_runner` 并提交。

**Review**

- [ ] R20.1 重新阅读 Plan Task 20 和 SPEC 生命周期状态机。
- [ ] R20.2 核对 DeviceId 在 CREATE 返回后、QUERY 前就进入 owned slot。
- [ ] R20.3 重跑 success order 和所有 failure cleanup tests。
- [ ] R20.4 核对 cleanup 只读 owned slot，永不删除哨兵或猜测设备。
- [ ] R20.5 记录 primary/cleanup error 组合和偏差。
- [ ] R20.6 结论 `ON_TRACK` 后提交 Task 20 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 21 — Implement raw round trip behind a fakeable I/O boundary

Plan: [Task 21](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-21-implement-raw-round-trip-behind-a-fakeable-io-boundary)

目标：在写权限前再次确认磁盘身份，只在 8 MiB 写 4096 bytes 并验证 SHA-256。

主要证据：操作顺序、偏移/长度、flush/reseek、short I/O 和 hash mismatch tests 通过。

**Execute**

- [ ] E21.1 确认 Task 20 为 `ON_TRACK`，记录 start commit。
- [ ] E21.2 阅读 Plan Task 21 的 6 个 Step 和 IRawDiskIo API。
- [ ] E21.3 先写 8 个 exact-order/safety red tests。
- [ ] E21.4 实现 4096-byte CSPRNG buffer、hash 和 fakeable sequence。
- [ ] E21.5 实现 Windows read-only + write-handle 两次 identity recheck。
- [ ] E21.6 fresh 运行 lifecycle tests，按 Plan 提交 implementation commit。

**Review**

- [ ] R21.1 重新阅读 Plan Task 21 和 SPEC raw round trip。
- [ ] R21.2 核对 offset 精确 8388608、length 精确 4096，绝不写 sector 0。
- [ ] R21.3 重跑全部 order/short/hash tests。
- [ ] R21.4 核对 raw bytes 没有进入日志或 JSON。
- [ ] R21.5 记录 identity query、I/O 顺序和偏差。
- [ ] R21.6 结论 `ON_TRACK` 后提交 Task 21 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 22 — Assemble query-driver and run modes in the executable

Plan: [Task 22](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-22-assemble-query-driver-and-run-modes-in-the-executable)

目标：组装唯一顶层异常边界、只读 query mode、真实 run composition 和 M0Provider 资源释放。

主要证据：Debug/Release executable 构建成功，usage/invalid command 都返回 2 且无设备变化。

**Execute**

- [ ] E22.1 确认 Task 21 为 `ON_TRACK`，记录 start commit。
- [ ] E22.2 阅读 Plan Task 22 的 8 个 Step 和 main dispatch 顺序。
- [ ] E22.3 创建 executable 和 M0Provider，保持 query mode 无对象/设备副作用。
- [ ] E22.4 组合 run mode、global serial mutex、mode-aware process classification。
- [ ] E22.5 构建两种配置并运行 usage/invalid path 检查。
- [ ] E22.6 按 Plan 提交 implementation commit；不得在本 Task 运行 CREATE。

**Review**

- [ ] R22.1 重新阅读 Plan Task 22，核对 main 只做 orchestration。
- [ ] R22.2 核对 query mode 不构造 provider、mapping、memory device 或调用 CREATE/REMOVE。
- [ ] R22.3 重建 Debug/Release 并 fresh 运行两个 exit-code 测试。
- [ ] R22.4 核对 M0Provider `stop_and_release()` 释放命名对象 owner 后再检查 reopen。
- [ ] R22.5 记录 executable 路径、依赖和偏差。
- [ ] R22.6 结论 `ON_TRACK` 后提交 Task 22 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 23 — Pass the read-only V3 real-driver gate

Plan: [Task 23](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-23-pass-the-read-only-v3-real-driver-gate)

目标：在提升权限下证明 QUERY_VERSION/list/signature/import 工作且完全不改变设备状态。

主要证据：Release import audit、query JSON、before/after services/disks/device ids 全部一致。

**Execute**

- [ ] E23.1 确认 Task 22 为 `ON_TRACK`，记录 start commit，并明确这是首次真实驱动访问但只查询。
- [ ] E23.2 阅读 Plan Task 23 的 8 个 Step 和 V3 禁止状态变化规则。
- [ ] E23.3 在 elevated x64 PowerShell fresh 运行 Release non-real tests 和 import audit。
- [ ] E23.4 捕获独立 baseline，运行一次 query-driver 并验证 JSON。
- [ ] E23.5 比较 query report 与独立 post-query 状态，更新 V3 orchestrator 和证据文档。
- [ ] E23.6 按 Plan 提交 implementation/evidence commit；任何变化即停止，禁止进入 V4。

**Review**

- [ ] R23.1 重新阅读 Plan Task 23 和 SPEC V3 定义。
- [ ] R23.2 核对 created_device/raw_round_trip 为 null，before/after 集合完全一致。
- [ ] R23.3 fresh 重跑 V3，而不是复用旧报告。
- [ ] R23.4 核对 query mode 没有停止 `deviosvc`、创建命名对象或写磁盘。
- [ ] R23.5 记录 executable/driver hash、adapter path 和偏差。
- [ ] R23.6 结论 `ON_TRACK` 后提交 Task 23 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 24 — Implement the guarded real-driver E2E harness

Plan: [Task 24](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-24-implement-the-guarded-real-driver-e2e-harness)

目标：实现管理员 preflight、串行锁、`deviosvc` try/finally 恢复、报告断言和 V4-V6 调度。

主要证据：PowerShell 语法有效，`-ListCommands` 显示 V0-V6 严格顺序；本 Task 不执行真实 CREATE。

**Execute**

- [x] E24.1 确认 Task 23 为 `ON_TRACK`，记录 start commit。
- [x] E24.2 阅读 Plan Task 24 的 11 个 Step 和完整 try/finally 形状。
- [x] E24.3 实现参数、serial mutex、所有 preflight 和 service baseline。
- [x] E24.4 实现正常/故障报告断言、query baselines、summary JSON 和恢复逻辑。
- [x] E24.5 只做 script parse 与 V6 `-ListCommands`，不运行 E2E。
- [x] E24.6 在首次真实 CREATE 前按 Plan 提交 harness implementation commit。

**Review**

- [x] R24.1 重新阅读 Plan Task 24，逐条核对 11 个 Step。
- [x] R24.2 核对所有状态改变都在 try/finally，且没有 `Set-Service`/`sc config`。
- [x] R24.3 fresh 运行 syntax parse 和 `-ListCommands`。
- [x] R24.4 核对异常报告也要求 exact cleanup，不能因预期失败忽略清理失败。
- [x] R24.5 记录列出的 V0-V6 命令和偏差。
- [x] R24.6 结论 `ON_TRACK` 后提交 Task 24 alignment review。

Review Record：Executor `Claude/Codex`; Start `588f172`; Implementation `e878280`，最终安全加固 `a4cd750`; Evidence `PowerShell syntax PASS；V0-V6 顺序由 Invoke-M0Verification.ps1 固化；真实运行由全局 mutex、管理员 preflight、deviosvc try/finally、生命周期及 query 不可变 hash 和严格断言保护；reparse pre-create rejection 与 abandoned mutex recovery 测试 PASS`; Deviations `用户后续授权批量接管 Task24-29，安全加固随真实故障修复集中提交`; Verdict `ON_TRACK`.

### Task 25 — Pass one complete V4 lifecycle

Plan: [Task 25](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-25-pass-one-complete-v4-lifecycle)

目标：首次完成一个受保护的 CREATE、QUERY、定位、raw round trip、精确 REMOVE 和全清理周期。

主要证据：1 attempted/1 passed，id、地址、容量、offset、length、hash 和 before/after 全部匹配。

**Execute**

- [x] E25.1 确认 Task 24 为 `ON_TRACK`，记录 start commit，确认 elevated shell。
- [x] E25.2 阅读 Plan Task 25 的 6 个 Step 和首次失败停止规则。
- [x] E25.3 fresh 重跑 V0-V3 后，只运行 1 次 Debug E2E。
- [x] E25.4 独立检查 report 的 id/address/length/hash/list invariants。
- [x] E25.5 检查服务和 forbidden processes，写 V4 evidence。
- [x] E25.6 按 Plan 提交 evidence commit；失败时标记 `NO_GO_EVIDENCE` 并停止重复尝试。

**Review**

- [x] R25.1 重新阅读 Plan Task 25 和对应 failure decision tree。
- [x] R25.2 核对只删除本轮 CREATE 返回的 id，且 removed id 完全相同。
- [x] R25.3 fresh 重跑完整 V4 作为复盘证据；若有风险，先保留现场并停止。
- [x] R25.4 核对 `deviosvc` state/start mode 和设备列表恢复。
- [x] R25.5 记录 report/summary 路径、所有关键值和偏差。
- [x] R25.6 结论 `ON_TRACK` 后提交 Task 25 alignment review。

Review Record：Executor `Codex`; Start `a44d989314cdccf7d77576b8674e1eca20202dac`; Implementation `a4cd750`（核心生命周期实现见 `ff380e1`，post-merge cleanup fix `bc9b852`）; Evidence `reports/m0-v4/summary-20260802-005326-7137299.json；run 1867955068cee93c03e05118a8086947；DeviceId 0 精确删除；PhysicalDrive1；64 MiB；8 MiB/4096 raw round trip；flush/hash/列表/命名对象/服务恢复全部通过`; Deviations `按用户明确要求批量完成 Task24-29，未为每个 Task 单独停顿提交，但首次失败均先定位、补测试再重跑`; Verdict `ON_TRACK`.

### Task 26 — Pass V5 post-create fault-injection cleanup

Plan: [Task 26](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-26-pass-v5-post-create-fault-injection-cleanup)

目标：证明 CREATE 后四个阶段发生主流程故障时仍精确删除、释放命名对象并恢复 baseline。

主要证据：四个预期故障 harness 均通过 cleanup contract，随后正常 run 成功。

**Execute**

- [x] E26.1 确认 Task 25 为 `ON_TRACK`，记录 start commit。
- [x] E26.2 阅读 Plan Task 26 的 6 个 Step 和四个 FailurePoint。
- [x] E26.3 分别运行 create_returned、device_confirmed、physical_drive_confirmed、round_trip_verified。
- [x] E26.4 每次都核对 primary error 是注入错误且 cleanup 全部为 true。
- [x] E26.5 所有故障后再运行一次正常 Debug lifecycle，写 V5 evidence。
- [x] E26.6 按 Plan 提交 evidence commit。

**Review**

- [x] R26.1 重新阅读 Plan Task 26 和 Task 20 cleanup 设计。
- [x] R26.2 核对每个故障点的 created id 与 removed id 完全相同。
- [x] R26.3 fresh 重跑 V5 整组，而不是只检查文档。
- [x] R26.4 核对预期主失败没有掩盖任何 cleanup failure。
- [x] R26.5 记录四份报告、postcheck 和偏差。
- [x] R26.6 结论 `ON_TRACK` 后提交 Task 26 alignment review。

Review Record：Executor `Codex`; Start `4119a57`; Implementation `a4cd750`（核心生命周期实现见 `ff380e1`，post-merge cleanup fix `bc9b852`）; Evidence `create_returned 005328-5759637、device_confirmed 005329-6335771、physical_drive_confirmed 005331-0747670、round_trip_verified 005332-6421580、normal 005334-1843324；五组均 1/1 PASS，注入报告 exit 12、primary_error=injected_failure、cleanup_errors=0`; Deviations `批量执行偏差同 Task25；所有故障点仍独立运行并由严格 validator 验证`; Verdict `ON_TRACK`.

### Task 27 — Execute the 20-cycle Release V6 acceptance

Plan: [Task 27](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-27-execute-the-20-cycle-release-v6-acceptance)

目标：用固定 Release executable 连续完成 20 个受保护周期并聚合全部不变量。

主要证据：20 attempted/20 passed、20 个唯一 run/session、零 fatal、零残留、无 forbidden dependency。

**Execute**

- [x] E27.1 确认 Task 26 为 `ON_TRACK`，记录 start commit。
- [x] E27.2 阅读 Plan Task 27 的 7 个 Step 和 V6 aggregate 列表。
- [x] E27.3 fresh 构建/测试/audit Release，记录 executable SHA-256。
- [x] E27.4 运行恰好 20 次 Release E2E，不在中途更换 executable。
- [x] E27.5 聚合 20 份报告并验证最终独立系统状态。
- [x] E27.6 写 V6 evidence，按 Plan 提交 evidence commit。

**Review**

- [x] R27.1 重新阅读 Plan Task 27 和 SPEC 20 次 GO 条件。
- [x] R27.2 核对 20 份报告使用同一 executable hash 且每份 invariants 全部 true。
- [x] R27.3 fresh 重跑完整 V6；资源/时间不足时不能沿用旧结果宣称 fresh pass。
- [x] R27.4 核对最后 query/service/disk/named-object 状态与 baseline 相同。
- [x] R27.5 记录总计数、所有失败数和偏差。
- [x] R27.6 结论 `ON_TRACK` 后提交 Task 27 alignment review。

Review Record：Executor `Codex`; Start `4119a57`; Implementation `a4cd750`（核心生命周期实现见 `ff380e1`，post-merge cleanup fix `bc9b852`）; Evidence `reports/m0-v6/summary-20260802-005337-3234647-aggregate.json；20/20；20 unique run/session/object path；query before/after 与 20 份 lifecycle report 的 immutable hash 全部复核；Release SHA-256 CBC84333F1E64504EDC156A5FEA69C1CF90BC8BCE5C8C232EB642C113BD1AB85；driver SHA-256 3C23DF3D22D2BD9ED34F4BBD12713EF9ADB131B6178F88C0CD3DBFCBDA9DF6F9；post-restore query/service/object checks PASS`; Deviations `批量执行偏差同 Task25；post-merge 首轮在 8/9 安全停止，确认异步 handle-release 竞态后以条件等待修复并从头 fresh 重跑 V6`; Verdict `ON_TRACK`.

### Task 28 — Write the formal M0 Go/No-go report

Plan: [Task 28](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-28-write-the-formal-m0-gono-go-report)

目标：把所有 MUST 和 V0-V6 证据映射成机械化 GO、NO-GO 或 BLOCKED 结论。

主要证据：每个规范要求都有 evidence row，报告完整性命令退出 0。

**Execute**

- [x] E28.1 确认 Task 27 状态与实际结论一致，记录 start commit。
- [x] E28.2 阅读 Plan Task 28 的 6 个 Step 和 Operational SPEC 全文。
- [x] E28.3 创建逐项 requirement/evidence/result 表，区分 observed/derived/conclusion。
- [x] E28.4 按机械规则选择 verdict，不用多数通过替代全部通过。
- [x] E28.5 运行报告完整性检查，明确 Toolkit removal 仍未授权。
- [x] E28.6 按 Plan 提交 verdict commit。

**Review**

- [x] R28.1 重新阅读 Plan Task 28 和 SPEC GO/BLOCKED/NO-GO 定义。
- [x] R28.2 逐行抽查 evidence path/command 是否真实存在且支持结论。
- [x] R28.3 fresh 运行完整性命令并核对没有遗漏失败。
- [x] R28.4 核对结论没有把推断写成观测事实。
- [x] R28.5 记录任何证据空洞或偏差；有空洞时不得 `ON_TRACK`。
- [x] R28.6 结论 `ON_TRACK` 后提交 Task 28 alignment review。

Review Record：Executor `Codex`; Start `a4cd750`; Implementation `2316402`，post-merge acceptance refresh `bc9b852`; Evidence `docs/validation/m0-driver-only-acceptance.md requirement table 完整性检查 PASS，V0-V6、aimapi.dll、20/20、exact DeviceId、named objects 均有对应 evidence row`; Deviations `machine-scoped GO，不扩大为生产就绪或 Toolkit 卸载授权`; Verdict `ON_TRACK`.

### Task 29 — Final repository and reproducibility audit

Plan: [Task 29](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-29-final-repository-and-reproducibility-audit)

目标：fresh 重跑允许的最高 tier，检查 forbidden references、仓库卫生和复现文档。

主要证据：Debug/Release V2 fresh pass，最高授权真实 tier fresh 结果，Git clean，无 out/reports tracked。

**Execute**

- [x] E29.1 确认 Task 28 为 `ON_TRACK`，记录 start commit。
- [x] E29.2 阅读 Plan Task 29 的 6 个 Step 和最终 completion criteria。
- [x] E29.3 fresh 运行 Debug/Release V2 和结论允许的最高真实 tier。
- [x] E29.4 扫描 forbidden references、diff、status 和 tracked artifacts。
- [x] E29.5 更新 README/ledger 的精确复现命令。
- [x] E29.6 按 Plan 提交 reproducibility handoff implementation commit。

**Review**

- [x] R29.1 重新阅读 Plan Task 29、failure tree 和 completion criteria。
- [x] R29.2 核对最终 Git diff 仅包含批准内容，没有 build/report artifacts。
- [x] R29.3 fresh 重跑最终声明所依赖的全部验证命令。
- [x] R29.4 核对 README 未暗示 M0 是产品或已授权卸载 Toolkit。
- [x] R29.5 记录最终 branch、commit、verdict、测试计数和偏差。
- [x] R29.6 结论 `ON_TRACK` 后提交 Task 29 alignment review，并把总进度表全部终结。

Review Record：Executor `Codex`; Start `a4cd750`; Implementation `2316402`，post-merge acceptance refresh `bc9b852`; Evidence `Debug V2：10/10 unit + SHM 50x；Release V2：10/10 unit + SHM 50x；Release V6：V3 query PASS、Debug full CTest 11/11、preflight safety 2/2、V4 1/1、V5 五组 1/1、V6 20/20；PowerShell syntax/forbidden refs/git diff/tracked artifacts/acceptance completeness/service/process audits PASS`; Deviations `现有未跟踪 _diag/_test/_v* 诊断文件属于此前现场，按保护用户文件规则保留且未提交；post-merge 竞态修复保持零残留门槛不变`; Verdict `ON_TRACK`.

## 8. 偏差处理规则

### 实现缺陷但未改变 Plan

```text
状态回到 IN_PROGRESS
为缺陷补失败测试
修复最早边界
fresh 运行当前 Task 全套验证
重新执行 G1-G7
```

### Plan 本身不正确或无法实现

```text
状态设为 DEVIATED
停止后续 Task
记录实际证据和建议修改
先修改并重新批准 Operational SPEC / Agent Plan
更新 tasklist 对应摘要
再恢复执行
```

Agent 无权只修改实现来绕过已批准 Plan。

### 外部条件缺失

```text
状态设为 BLOCKED
记录命令、错误、机器状态和最后通过 tier
不得把 BLOCKED 写成 NO-GO 或 GO
不得跳过前置条件继续真实驱动操作
```

### 真实驱动强制条件失败

```text
状态设为 NO_GO_EVIDENCE
执行精确清理
保留 primary error、cleanup errors 和报告路径
停止进入更高 tier
按 failure decision tree 定位最早边界
```

## 9. Agent 每次回复用户的固定格式

执行 Agent 完成一个 Task 后，回复必须包含：

```markdown
Task N 状态：ON_TRACK | DEVIATED | BLOCKED | NO_GO_EVIDENCE

实现提交：<commit>
复盘提交：<commit 或尚未创建>

实际改动：
- <files>

新鲜验证：
- <command> -> <exit code / pass count>

Plan 对齐复盘：
- 已重新阅读 Plan Task N：是/否
- 文件范围一致：是/否
- 行为范围一致：是/否
- 提前实现后续 Task：是/否
- 偏差：None 或具体内容

下一步：只有 ON_TRACK 时才写 Task N+1；其他状态说明停止原因。
```

禁止只回复“已完成”“测试通过”或“可以继续”。

## 10. 整体完成条件

本 tasklist 完成必须同时满足：

- Task 0-29 每项都有 implementation commit 和 alignment review commit。
- Task 0-29 状态全部为 `ON_TRACK`，或者最终证据明确停在 `BLOCKED` / `NO_GO_EVIDENCE`。
- 每个 Review Record 都填写实际 commit、验证和偏差。
- 最终 verdict 与 Operational SPEC 一致。
- 工作区无未解释改动。
- M0 结论没有被描述成 Toolkit 卸载授权或完整产品完成。

---

# M4 驱动不可用与断电恢复资格验收执行台账

> 本节是当前活跃的 M4 执行台账；上文 M0 内容仅为历史记录，禁止回写或重新执行。实现依据是 [M4 驱动不可用与断电恢复资格验收实施计划](docs/superpowers/plans/2026-08-21-m4-driver-unavailable-qualification.md) 和 [对应 Spec](docs/superpowers/specs/2026-08-20-m4-driver-unavailable-qualification-design.md)。

> 状态：待实施。旧 kit、旧资格根目录和旧协议只可用于诊断，不得迁移、兼容、覆盖或作为通过证据。

## M4-0. 执行规则

1. 每次只将一个任务标记为 `IN_PROGRESS`；其前置任务必须是 `ON_TRACK`。
2. 每个任务先完成其 RED 项，再实现最小变更至 GREEN；未经记录的 GREEN 不得作为证据。
3. 每个实现任务完成后必须运行该任务列出的验证，执行 `git diff --check`，并在单独提交中仅记录本台账复盘。
4. `Blocked` 是资格根目录的产品终态；任何测试都必须断言同一资格根不自动重试、不改写状态、不再次触发子进程或 VMware 电源边界。
5. VMware 只能由用户人工操作。VM 正式验收总共恰好三次边界：Formal、Live Power-Cut、Missing-driver；每段选择 Reset 或一次 Power Off→Power On，绝不正常关机。
6. `pnputil.exe /restart-device` 不得进入实现。只有在干净 VM 前置实验已证明 `sc start phdskmnt` 不足时，才可停止本任务表、补充决策与 Spec 后重新规划。
7. 物理盘号、分区号、PnP 实例号、序列号和显示文本仅能写入诊断；通过条件只能使用当前轮 AIM 所拥有的 `S:` 与 Formal 来源 bundle 最后 checkpoint 的 `A.txt`/`D.txt` 内容哈希。

### M4 状态定义

```text
NOT_STARTED -> IN_PROGRESS -> IMPLEMENTED -> REVIEWING -> ON_TRACK
                                  |                 |
                                  +-> DEVIATED      +-> BLOCKED
                                                    +-> NO_GO_EVIDENCE
```

`DEVIATED`、`BLOCKED`、`NO_GO_EVIDENCE` 均停止后续任务；必须先更新并批准 Spec/Plan，或由用户解除外部阻断。

## M4-1. 总进度表

| Task | 状态 | 前置 | 实现提交 | 复盘提交 | 证据/阻断 |
|---|---|---|---|---|---|
| M4-00 | NOT_STARTED | — | — | — | 冻结起点与范围 |
| M4-01 | NOT_STARTED | M4-00 | — | — | 预检纯分类 RED |
| M4-02 | NOT_STARTED | M4-01 | — | — | SCM 包装与错误映射 |
| M4-03 | NOT_STARTED | M4-02 | — | — | `main.cpp` 精确插入点 |
| M4-04 | NOT_STARTED | M4-03 | — | — | discovery/query 协议回归 |
| M4-05 | NOT_STARTED | M4-04 | — | — | 九负例 manifest 与 fixture |
| M4-06 | NOT_STARTED | M4-05 | — | — | 两种驱动缺失 runner evidence |
| M4-07 | NOT_STARTED | M4-06 | — | — | Formal clone 与控制器状态/事件 schema |
| M4-08 | NOT_STARTED | M4-07 | — | — | Reset3 启动门与零副作用 |
| M4-09 | NOT_STARTED | M4-08 | — | — | `missing_driver_create` 控制器路径 |
| M4-10 | NOT_STARTED | M4-09 | — | — | 服务恢复、query 与真实副本恢复 |
| M4-11 | NOT_STARTED | M4-10 | — | — | 七个其余负例、失败矩阵、Blocked |
| M4-12 | NOT_STARTED | M4-11 | — | — | 纯离线 verifier |
| M4-13 | NOT_STARTED | M4-12 | — | — | 新协议 kit 冻结 |
| M4-14 | NOT_STARTED | M4-13 | — | — | 本地全量验证与发布说明 |
| M4-15 | NOT_STARTED | M4-14 | — | — | VM 前置实验与三边界人工验收 |

## M4-2. 细颗粒度执行清单

### M4-00 — 冻结起点、读取约束并建立 M4 证据台账

**范围：** `tasklist.md`、`docs/validation/m4-driver-unavailable-*`（新增）

- [ ] M4-00.1 阅读 Spec §1–§10、Plan 全文及本节 M4-0；记录当前 `git status --short --branch` 与 `git log -5 --oneline`。
- [ ] M4-00.2 记录当前 kit schema、kit 固定文件清单、`myrd.exe`/query SHA-256、控制器/runner/verifier 文件 SHA-256；不生成新 kit。
- [ ] M4-00.3 运行只读基线：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-guest-qualification.ps1 -SourceRoot .`、`powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\unit\test-m4-qualification-kit.ps1 -SourceRoot .`、PowerShell AST 解析三份生产脚本。
- [ ] M4-00.4 在 `docs/validation/` 写入仅含起始 commit、命令、退出码、文件哈希和“不触碰 VM/驱动/设备”的 M4 基线记录。
- [ ] M4-00.5 提交基线记录，复盘文件范围只含该记录和本台账，状态改为 `ON_TRACK`。

### M4-01 — 为服务预检纯分类写出 RED 测试

**范围：** `tests/unit/m4_driver_preflight_tests.cpp`（新增）、`tests/CMakeLists.txt`、`src/run/driver_preflight.h`（仅声明）

- [ ] M4-01.1 创建 `m4_driver_preflight_tests.cpp`，先编写 `classify_phdskmnt_service` 的失败测试：服务不存在与 `SERVICE_DISABLED` 返回 `unavailable`。
- [ ] M4-01.2 在同一测试添加四种可用启动类型 RED：`SERVICE_BOOT_START`、`SERVICE_SYSTEM_START`、`SERVICE_DEMAND_START`、`SERVICE_AUTO_START` 均返回 `available`。
- [ ] M4-01.3 为 SCM 包装定义可替换 I/O seam 的测试输入，加入 `OpenSCManagerW` 失败、`OpenServiceW(ERROR_ACCESS_DENIED)`、`QueryServiceConfigW` 失败三条 RED；每条断言 `ErrorDomain::win32`、具体操作名且渲染文本不含 `driver_unavailable`。
- [ ] M4-01.4 在 `tests/CMakeLists.txt` 注册 `unit.m4.driver_preflight`、标签 `m4-v3`、超时 30 秒。
- [ ] M4-01.5 构建并运行：`cmake --preset msvc`、`cmake --build --preset debug --parallel 1 --target myrd_m4_driver_preflight_tests`、`ctest --preset debug -R '^unit\.m4\.driver_preflight$' --output-on-failure`；预期失败为缺声明/缺实现。
- [ ] M4-01.6 提交仅含 RED 测试与必要声明的变更；复盘 RED 输出和文件范围。

### M4-02 — 实现 SCM 包装及三类预检结果映射

**范围：** `src/run/driver_preflight.h`、`src/run/driver_preflight.cpp`、`src/CMakeLists.txt`、`tests/unit/m4_driver_preflight_tests.cpp`

- [ ] M4-02.1 实现纯函数：服务缺失或 `SERVICE_DISABLED` 归为 `unavailable`；四种列出的有效启动类型归为 `available`。
- [ ] M4-02.2 实现只含 SCM 访问的包装：`OpenSCManagerW`、`OpenServiceW`、`QueryServiceConfigW`、精确 close handle；不得读取 bundle、枚举 StoragePort、打开 AIM、检查盘符或创建设备。
- [ ] M4-02.3 仅将 `ERROR_SERVICE_DOES_NOT_EXIST` 和 `SERVICE_DISABLED` 映射为：`ErrorDomain::usage`、code `1`、`operation=L"driver_unavailable"`、`message=L"phdskmnt is disabled or absent"`。
- [ ] M4-02.4 将所有其他 SCM 失败映射为 `ErrorDomain::win32`、原始 `GetLastError()` 和各自操作名 `OpenSCManagerW`/`OpenServiceW`/`QueryServiceConfigW`；禁止借用 `driver_unavailable` operation/message。
- [ ] M4-02.5 重跑 `unit.m4.driver_preflight` 到 GREEN，并添加 handle-close/错误优先级断言。
- [ ] M4-02.6 提交实现；复盘确认没有 adapter handle、StoragePort、SRB QUERY_VERSION、设备、盘符或 bundle 依赖进入新模块。

### M4-03 — 将预检插入 `myrd run` 的唯一安全位置

**范围：** `src/run/main.cpp`、`src/CMakeLists.txt`、`tests/unit/m4_product_boundary_tests.ps1`

- [ ] M4-03.1 在产品边界 PowerShell 合同中写 RED：`require_phdskmnt_service_available()` 必须在 `require_elevation()` 成功分支之后、`classify_bundle()` 之前；Create 与 Recover 不得各自有第二个预检入口。
- [ ] M4-03.2 在该合同中写 RED 字节协议断言：已知不可用时 `print_run_error` 的字段必须是 `driver_unavailable` 和 `phdskmnt is disabled or absent`，stdout 空、exit 1、stderr CRLF；SCM I/O 失败 stderr 不得含该 operation。
- [ ] M4-03.3 在 `main.cpp` 仅在提权成功后插入预检；失败时调用既有 `print_run_error(options.bundle, error)` 并 `return 1`，然后才调用 `classify_bundle()`。
- [ ] M4-03.4 确认预检之前不读取/创建/修改/挂载 bundle；在合同中用源码位置检查覆盖 `classify_bundle`、`create_image`、recover、provider start。
- [ ] M4-03.5 运行 `ctest --preset debug -R '^unit\.m4\.(driver_preflight|product_boundary)$' --output-on-failure`，预期全绿。
- [ ] M4-03.6 提交实现并复盘错误优先级：未提权时仍优先报告 `require_elevation`，不会报告服务状态。

### M4-04 — 冻结 discovery 唯一权威与 query 字节协议

**范围：** `src/driver/adapter_discovery.cpp`、`src/m3/m3_windows_acceptance.cpp`、`tests/e2e/m4_aim_query.cpp`、`tests/unit/m4_product_boundary_tests.ps1`、对应 C++ 单元测试

- [ ] M4-04.1 写 RED 源码合同：`discover_aim_adapter` 继续使用 `GUID_DEVICEINTERFACE_STORAGEPORT`、`SPDRP_SERVICE == phdskmnt`、既有 QUERY_VERSION 与唯一候选拒绝规则。
- [ ] M4-04.2 写 RED 合同，禁止新预检或控制器使用 `FriendlyName`、Class、PnP 实例号、序列号或 PhysicalDrive 作为 adapter 匹配权威。
- [ ] M4-04.3 在 query 测试冻结失败协议：exit 1、stdout 空、stderr 精确 `discover_aim_adapter failed\r\n`。
- [ ] M4-04.4 在 query 测试冻结成功协议：exit 0、stderr 空、单一 JSON、`success` 为布尔 true、版本为数值、`ids` 为数值数组；BOM、多 JSON、错误 CRLF、额外/错误类型均失败。
- [ ] M4-04.5 验证服务可用但 provider discovery 失败仍采用既有 `discover_aim_adapter: no AIM adapter found`，测试不得要求此场景零 bundle 写入。
- [ ] M4-04.6 运行 query/selftest 与相关 unit 到 GREEN，提交并复盘 provider 中 discovery 的位置未前移。

### M4-05 — 将 negative fixture manifest 升级为精确九案

**范围：** `tests/e2e/Prepare-M4NegativeFixtures.ps1`、`tests/e2e/Invoke-M4RunLifecycle.ps1`、`tests/unit/test-m4-guest-qualification.ps1`、`tests/CMakeLists.txt`

- [ ] M4-05.1 写 RED：旧八案或旧单一 `missing_driver` manifest 必须被拒绝。
- [ ] M4-05.2 将 manifest 固定为恰好九项：`unknown_nonempty`、`size_mismatch`、`occupied_letter`、`gen0`、`writer_lock`、`missing_driver_create`、`driver_lost_recover`、`busy_volume`、`compact_lock`。
- [ ] M4-05.3 为每项写入并验证绝对 case 路径、相对路径/长度/SHA-256 清单、输入常量和确定性 schema；拒绝缺项、额外项、重排、路径逃逸、数组非字符串、BOM 与非 CRLF JSON。
- [ ] M4-05.4 更新 runner 自测的 producer-shaped 九案正例，运行 `MYRD_M4_NEGATIVE_SELFTEST=1` 得到 GREEN。
- [ ] M4-05.5 运行 guest qualification 合同与标签 `m4-v3` 中 runner 测试；若完整矩阵超时，只增加显式 CTest timeout，禁止删减案例。
- [ ] M4-05.6 提交 runner/fixture manifest 改动并复盘固定文件名、StateFile 与 Prepare/Negative 生命周期文件路径一致。

### M4-06 — 实现两种驱动缺失 runner evidence 与不变量

**范围：** `tests/e2e/Invoke-M4RunLifecycle.ps1`、`tests/e2e/Prepare-M4NegativeFixtures.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-06.1 写 `missing_driver_create` RED：预检拒绝后 case 根仍不存在，且没有 bundle、journal、AIM 或目标盘符证据。
- [ ] M4-06.2 写 `driver_lost_recover` RED：拒绝前后完整相对路径/长度/SHA-256 树不同（含 journal）时必须失败。
- [ ] M4-06.3 为两种拒绝固定 `driver_unavailable` run stderr，并把 query 的 exit 1/空 stdout/固定 stderr 作为独立协议；禁止比较二者 stderr 文本。
- [ ] M4-06.4 为真实 `driver_lost_recover` 成功 evidence 加入本轮 owned AIM/`S:`、`A.txt`/`D.txt`、Formal 来源最后 checkpoint、exit/stderr、精确清理和仅诊断性设备字段。
- [ ] M4-06.5 写 mutation RED/GREEN：截断 JSON、错误 CRLF/BOM、来源 hash、树成对伪造、owned `S:`、A/D hash、exit/stderr、数组元素/PID 类型、清理失败与重复运行。
- [ ] M4-06.6 运行 runner selftest、guest contract 和 `git diff --check`；提交后确认不存在旧 `missing_driver` 成功证据兼容分支。

### M4-07 — 创建 Formal 副本并扩展控制器状态/事件 schema

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-07.1 写 RED 状态转换测试：新增 `MissingDriverCreateCaptured`、`DriverLostRecoverCaptured`、`DriverRestored`、`DriverLostRecoverRecovered` 及其事件必须 forward-only；非法回退、跳跃、sequence 复用和 Blocked 恢复全部 typed 拒绝。
- [ ] M4-07.2 在 Formal 成功后复制已验证的 Formal bundle 到专用 `driver_lost_recover` 根；复制前后生成相对路径/长度/SHA-256 inventory 并写 durable event。
- [ ] M4-07.3 写 RED：控制器若把 Formal 原 bundle 作为第三阶段写入目标、复制树不相等、source commit/kit hash 不一致或 clone 路径逃逸，必须 Blocked。
- [ ] M4-07.4 实现严格事件 schema：clone source、clone inventory、路径、hash、时间、状态/sequence 与证据 SHA 全部明确类型并写后重读。
- [ ] M4-07.5 运行 controller SelfTest 和 guest unit；提交并复盘每个新状态都已接入 `WriteState → FlushEvent → ReReadState → VerifyEvidenceHash`。

### M4-08 — 实现 Reset3 的启动门与 same-boot 零副作用

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-08.1 写 RED：`AwaitingMissingDriverReset` 同 boot 重调必须以规定 exit 返回，状态/事件字节完全不变。
- [ ] M4-08.2 在 same-boot 测试中记录所有 seam；断言不调用 Facts、query、runner、Driver、Prepare、fixture、observer 写入或 VMware 相关动作。
- [ ] M4-08.3 写 RED：new boot 缺 41、缺 6008、boot 时间未前进、服务并非缺失/禁用、query 意外成功、AIM 盘存在或 `S:` 存在均 Blocked。
- [ ] M4-08.4 实现 Reset3 新 boot gate，并为每个失败先写独立阶段 Blocked evidence 后封存资格根。
- [ ] M4-08.5 运行 production dispatcher 的 fake same-boot/new-boot 集成 selftest 到 GREEN；提交并复盘调用向量精确匹配。

### M4-09 — 编排 `missing_driver_create` 并证明零 bundle 写入

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-09.1 写 RED：控制器必须在新 boot gate 后同时执行 run 与 query；任何一方 exit 0、超时、其他 exit、非空 stdout 或不匹配 stderr 均 Blocked。
- [ ] M4-09.2 将 run 拒绝 evidence 和 query evidence 分别严格解析并写入 `MissingDriverCreateCaptured` 的独立哈希锚点。
- [ ] M4-09.3 写 RED：case 根存在、树清单不为空、出现 bundle/journal/AIM/`S:` 任何一项时拒绝；控制器自身 evidence/临时文件不进入零写入比较。
- [ ] M4-09.4 实现 zero-write validator、路径 containment 与完整 typed schema；每个 validator 失败都写子步骤名、主错误和清理错误。
- [ ] M4-09.5 运行生产 dispatcher fake 流：Reset3 new boot → `missing_driver_create` 成功捕获；随后 mutation matrix 必须在启动下一个 runner 前阻止。
- [ ] M4-09.6 提交并复盘 query/run “一致”只按两者均 exit 1 判断，不比较两条 stderr 的字面文本。

### M4-10 — 恢复服务、探测 AIM，并执行 `driver_lost_recover` 的真实恢复

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`

- [ ] M4-10.1 写 RED：恢复只允许按顺序调用 `sc.exe config phdskmnt start= demand`、`sc.exe start phdskmnt`；`pnputil.exe /restart-device` 任何引用均被静态安全合同拒绝。
- [ ] M4-10.2 写 RED：`sc qc` 必须严格证明 `START_TYPE : 3 DEMAND_START`，`sc query` 必须严格证明 `STATE : 4 RUNNING`；非零 exit、stderr、重复/缺失字段、Disabled/Manual/Stopped 均失败。
- [ ] M4-10.3 写 RED：恢复后的 `myrd_m4_aim_query.exe` 必须 exit 0、stderr 空、单一严格成功 JSON；否则在真实恢复前 Blocked。
- [ ] M4-10.4 写 RED：`driver_lost_recover` 拒绝后副本树变化（包括 journal）或恢复后 `S:` 不属于本轮 owned AIM、A/D 不等于 Formal 来源最后 checkpoint 时失败。
- [ ] M4-10.5 实现服务恢复、query 探针、真实 recovery runner、来源内容/owned `S:` 验证和独立事件；保留 PhysicalDrive/分区/PnP 为诊断，不参与条件。
- [ ] M4-10.6 运行完整 fake production dispatcher：两个拒绝 → service restore → query success → real recovery success；提交并复盘每个失败均在后续 runner 前停止。

### M4-11 — 完成其余七负例、全失败矩阵与永久 Blocked 行为

**范围：** `tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-guest-qualification.ps1`、`tests/e2e/Invoke-M4RunLifecycle.ps1`

- [ ] M4-11.1 写 RED：真实恢复未成功时，`unknown_nonempty`、`size_mismatch`、`occupied_letter`、`gen0`、`writer_lock`、`busy_volume`、`compact_lock` 任一项不得启动。
- [ ] M4-11.2 为七个 case 的每一项写 production-shaped evidence、严格 raw/schema/hash/路径/argv/process/oracle/inventory 验证，并将最终集合固定为完整九案。
- [ ] M4-11.3 为每一个 Reset3 子步骤和七个 case 建立 fresh fake root 的失败矩阵；每行断言精确 Blocked reason、事件 reason、调用全序列、后续 seam 为零、owned cleanup 完成。
- [ ] M4-11.4 在每行失败后重调同一资格根；断言没有 runner、service、query、observer 或状态字节改变，且无自动重试。
- [ ] M4-11.5 写 RED/GREEN：最终 `NegativeComplete` evidence 缺 case、额外 case、弱 schema、错误 final lifecycle、缺 missing-driver capture hash 或来源/kit/commit 不绑定时拒绝。
- [ ] M4-11.6 运行 controller SelfTest、guest contract、runner native/protocol/negative selftest，提交并复盘所有矩阵 reason 不是“非空字符串”而是每行精确期望。

### M4-12 — 实现纯离线最终 verifier

**范围：** `tools/Invoke-M4Verification.ps1`、`tests/unit/test-m4-guest-qualification.ps1` 或新增 `tests/unit/test-m4-verification.ps1`、`tests/CMakeLists.txt`

- [ ] M4-12.1 写 RED：verifier 缺任一 driver-unavailable evidence、缺九案、篡改 clone tree/来源 checkpoint/A-D/owned `S:`/event hash 时必须失败。
- [ ] M4-12.2 实现只读 `Verify-DriverUnavailable`：严格重读 kit/source commit、Formal clone、两个缺驱动 evidence、目录清单、恢复内容、九案、final lifecycle 和所有状态事件哈希。
- [ ] M4-12.3 写 AST/命令 allowlist RED：verifier 不得调用 `myrd.exe`、query、SCM、PnP、Disk、WinEvent、VMware、Reset 或任何设备/电源命令。
- [ ] M4-12.4 添加 raw bytes、BOM、非 CRLF、额外字段、错误类型/数组元素、资格根逃逸、重复/缺失事件、hash 不符、旧 protocol 的离线 mutation matrix。
- [ ] M4-12.5 运行 verifier SelfTest 和对应 CTest 到 GREEN；提交并复盘 verifier 可在资格根目录副本上运行，不依赖当前 VM。

### M4-13 — 冻结新的 qualification kit 协议

**范围：** `tools/New-M4GuestQualificationKit.ps1`、`tools/Invoke-M4GuestQualification.ps1`、`tests/unit/test-m4-qualification-kit.ps1`、`tests/CMakeLists.txt`

- [ ] M4-13.1 写 RED：旧 manifest schema、旧文件数/顺序、旧 source commit、旧 artifact/query 或旧脚本 hash 不能被新 controller 装载。
- [ ] M4-13.2 升级 manifest schema，固定所有 payload 的相对路径、顺序、长度、SHA-256、source commit、artifact/query SHA 与确定性 UTF-8 无 BOM JSON。
- [ ] M4-13.3 写 packager mutation matrix：同名输出目录、缺文件、额外文件、重排、BOM、长度/hash/commit 改写、artifact/query 不匹配均 fail closed。
- [ ] M4-13.4 写 controller startup 复验：VM 内 kit manifest 与每个文件 hash/length 必须相等，运行中覆盖 kit 或混用旧 kit/root 必须 Blocked。
- [ ] M4-13.5 将所有新增原生/runner/controller/verifier/kit 测试纳入 `m4-v3`，调整超时但不得缩减测试。
- [ ] M4-13.6 运行 kit 合同与 Debug/Release focused CTest，提交并复盘 exact 文件清单没有遗漏或额外测试工具。

### M4-14 — 完成发布前本地验证和人工验收说明

**范围：** `README.md`、`docs/validation/*`、`tasklist.md`

- [ ] M4-14.1 更新 README：给出 host kit 生成四参数命令、Release artifact/query 路径、hash 检查、完整 kit 复制、新 qualification root、控制器命令（不写不存在的 `-Run` 参数）。
- [ ] M4-14.2 更新人工步骤：恰好三次电源边界、每段 Reset 或 Power Off→Power On 二选一、不得正常关机、不得手动修改资格根、Windows 修复屏的判定规则。
- [ ] M4-14.3 运行 PowerShell AST、guest/kit/verifier selftest 和以下全量命令：

  ```powershell
  cmake --preset msvc
  cmake --build --preset debug --parallel 1
  cmake --build --preset release --parallel 1
  ctest --preset debug -L m4-v3 --output-on-failure
  ctest --preset release -L m4-v3 --output-on-failure
  git diff --check
  ```

- [ ] M4-14.4 记录命令、退出码、测试总数、commit、没有真实 VM 资格结论的原因；任何失败回到最早相关任务。
- [ ] M4-14.5 在宿主机生成 commit+GUID 的新 kit，逐文件验证 manifest/hash/length，并记录发布候选证据；禁止覆盖旧 kit。
- [ ] M4-14.6 提交文档/本地验证记录，复盘后才允许开始 VM 前置实验。

### M4-15 — 执行 VM 前置实验与三次人工电源边界验收

**范围：** 仅新 kit、新 VM、新 qualification root 与 `docs/validation/m4-driver-unavailable-vm-*`（新增）

- [ ] M4-15.1 在干净 VM 上记录服务、query、AIM/盘符、事件和 kit hash 基线；新建从未存在过的 qualification root。
- [ ] M4-15.2 先单独验证服务恢复前置实验：将 `phdskmnt` 从 Disabled 恢复为 demand，执行 `sc start phdskmnt` 与 query；记录 query 是否严格成功。失败则标记 `BLOCKED`，不得调用 `pnputil` 或继续正式资格流程。
- [ ] M4-15.3 仅在 M4-15.2 成功后，执行 Formal：运行控制器至 armed，用户做一次 Reset 或一次 Power Off→Power On，再运行同一命令并保存全部 evidence hash。
- [ ] M4-15.4 执行 Live Power-Cut：运行控制器至 armed，用户做一次且仅一次允许电源边界，再运行同一命令并保存 evidence hash。
- [ ] M4-15.5 执行 Missing-driver：运行控制器至 armed，用户做一次且仅一次允许电源边界；重新运行命令，让控制器完成两个缺驱动 case、服务恢复、真实副本恢复与余下七例。
- [ ] M4-15.6 在不改变 VM 的条件下运行最终离线 verifier；保存 stdout、退出码、资格根哈希与三段电源边界记录。
- [ ] M4-15.7 仅当 M4-14 全绿、M4-15.2 前置实验成功、三段都通过且 verifier 通过时记录“VM 已资格验证”；否则只记录“VM 验收未完成”，禁止夸大为 M4 已通过。

## M4-3. 每个任务的复盘模板

每个任务的 implementation commit 后，必须先将状态标为 `REVIEWING`，填完以下字段，再单独提交本 `tasklist.md`：

```markdown
### Review Record — M4-XX

- Start commit: `<sha>`
- Implementation commit: `<sha>`
- 允许文件与实际文件：`一致 | 偏差说明`
- RED 证据：`命令、退出码、预期失败文本`
- GREEN 证据：`命令、退出码、通过数量`
- Debug/Release 证据：`命令、通过数量；未运行时写原因`
- Spec/Plan 对齐：`逐条确认或列出偏差`
- 安全检查：`无 VMware/驱动/设备动作，或已授权的精确动作与证据`
- 结论：`ON_TRACK | DEVIATED | BLOCKED | NO_GO_EVIDENCE`
```

只有 `ON_TRACK` 才能解锁下一项。任何偏差先更新并批准 Spec/Plan，再更新本台账。
