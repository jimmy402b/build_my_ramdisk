# M0 Driver-only Execution Tasklist

> 状态：可交付其他 Agent 执行
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
| 11 | ON_TRACK | 4da2d4c | 9f66da7 | (pending) | — |
| 12 | IN_PROGRESS | (pending) | — | — | — |
| 13 | NOT_STARTED | — | — | — | — |
| 14 | NOT_STARTED | — | — | — | — |
| 15 | NOT_STARTED | — | — | — | — |
| 16 | NOT_STARTED | — | — | — | — |
| 17 | NOT_STARTED | — | — | — | — |
| 18 | NOT_STARTED | — | — | — | — |
| 19 | NOT_STARTED | — | — | — | — |
| 20 | NOT_STARTED | — | — | — | — |
| 21 | NOT_STARTED | — | — | — | — |
| 22 | NOT_STARTED | — | — | — | — |
| 23 | NOT_STARTED | — | — | — | — |
| 24 | NOT_STARTED | — | — | — | — |
| 25 | NOT_STARTED | — | — | — | — |
| 26 | NOT_STARTED | — | — | — | — |
| 27 | NOT_STARTED | — | — | — | — |
| 28 | NOT_STARTED | — | — | — | — |
| 29 | NOT_STARTED | — | — | — | — |

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

- [ ] E24.1 确认 Task 23 为 `ON_TRACK`，记录 start commit。
- [ ] E24.2 阅读 Plan Task 24 的 11 个 Step 和完整 try/finally 形状。
- [ ] E24.3 实现参数、serial mutex、所有 preflight 和 service baseline。
- [ ] E24.4 实现正常/故障报告断言、query baselines、summary JSON 和恢复逻辑。
- [ ] E24.5 只做 script parse 与 V6 `-ListCommands`，不运行 E2E。
- [ ] E24.6 在首次真实 CREATE 前按 Plan 提交 harness implementation commit。

**Review**

- [ ] R24.1 重新阅读 Plan Task 24，逐条核对 11 个 Step。
- [ ] R24.2 核对所有状态改变都在 try/finally，且没有 `Set-Service`/`sc config`。
- [ ] R24.3 fresh 运行 syntax parse 和 `-ListCommands`。
- [ ] R24.4 核对异常报告也要求 exact cleanup，不能因预期失败忽略清理失败。
- [ ] R24.5 记录列出的 V0-V6 命令和偏差。
- [ ] R24.6 结论 `ON_TRACK` 后提交 Task 24 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 25 — Pass one complete V4 lifecycle

Plan: [Task 25](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-25-pass-one-complete-v4-lifecycle)

目标：首次完成一个受保护的 CREATE、QUERY、定位、raw round trip、精确 REMOVE 和全清理周期。

主要证据：1 attempted/1 passed，id、地址、容量、offset、length、hash 和 before/after 全部匹配。

**Execute**

- [ ] E25.1 确认 Task 24 为 `ON_TRACK`，记录 start commit，确认 elevated shell。
- [ ] E25.2 阅读 Plan Task 25 的 6 个 Step 和首次失败停止规则。
- [ ] E25.3 fresh 重跑 V0-V3 后，只运行 1 次 Debug E2E。
- [ ] E25.4 独立检查 report 的 id/address/length/hash/list invariants。
- [ ] E25.5 检查服务和 forbidden processes，写 V4 evidence。
- [ ] E25.6 按 Plan 提交 evidence commit；失败时标记 `NO_GO_EVIDENCE` 并停止重复尝试。

**Review**

- [ ] R25.1 重新阅读 Plan Task 25 和对应 failure decision tree。
- [ ] R25.2 核对只删除本轮 CREATE 返回的 id，且 removed id 完全相同。
- [ ] R25.3 fresh 重跑完整 V4 作为复盘证据；若有风险，先保留现场并停止。
- [ ] R25.4 核对 `deviosvc` state/start mode 和设备列表恢复。
- [ ] R25.5 记录 report/summary 路径、所有关键值和偏差。
- [ ] R25.6 结论 `ON_TRACK` 后提交 Task 25 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 26 — Pass V5 post-create fault-injection cleanup

Plan: [Task 26](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-26-pass-v5-post-create-fault-injection-cleanup)

目标：证明 CREATE 后四个阶段发生主流程故障时仍精确删除、释放命名对象并恢复 baseline。

主要证据：四个预期故障 harness 均通过 cleanup contract，随后正常 run 成功。

**Execute**

- [ ] E26.1 确认 Task 25 为 `ON_TRACK`，记录 start commit。
- [ ] E26.2 阅读 Plan Task 26 的 6 个 Step 和四个 FailurePoint。
- [ ] E26.3 分别运行 create_returned、device_confirmed、physical_drive_confirmed、round_trip_verified。
- [ ] E26.4 每次都核对 primary error 是注入错误且 cleanup 全部为 true。
- [ ] E26.5 所有故障后再运行一次正常 Debug lifecycle，写 V5 evidence。
- [ ] E26.6 按 Plan 提交 evidence commit。

**Review**

- [ ] R26.1 重新阅读 Plan Task 26 和 Task 20 cleanup 设计。
- [ ] R26.2 核对每个故障点的 created id 与 removed id 完全相同。
- [ ] R26.3 fresh 重跑 V5 整组，而不是只检查文档。
- [ ] R26.4 核对预期主失败没有掩盖任何 cleanup failure。
- [ ] R26.5 记录四份报告、postcheck 和偏差。
- [ ] R26.6 结论 `ON_TRACK` 后提交 Task 26 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 27 — Execute the 20-cycle Release V6 acceptance

Plan: [Task 27](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-27-execute-the-20-cycle-release-v6-acceptance)

目标：用固定 Release executable 连续完成 20 个受保护周期并聚合全部不变量。

主要证据：20 attempted/20 passed、20 个唯一 run/session、零 fatal、零残留、无 forbidden dependency。

**Execute**

- [ ] E27.1 确认 Task 26 为 `ON_TRACK`，记录 start commit。
- [ ] E27.2 阅读 Plan Task 27 的 7 个 Step 和 V6 aggregate 列表。
- [ ] E27.3 fresh 构建/测试/audit Release，记录 executable SHA-256。
- [ ] E27.4 运行恰好 20 次 Release E2E，不在中途更换 executable。
- [ ] E27.5 聚合 20 份报告并验证最终独立系统状态。
- [ ] E27.6 写 V6 evidence，按 Plan 提交 evidence commit。

**Review**

- [ ] R27.1 重新阅读 Plan Task 27 和 SPEC 20 次 GO 条件。
- [ ] R27.2 核对 20 份报告使用同一 executable hash 且每份 invariants 全部 true。
- [ ] R27.3 fresh 重跑完整 V6；资源/时间不足时不能沿用旧结果宣称 fresh pass。
- [ ] R27.4 核对最后 query/service/disk/named-object 状态与 baseline 相同。
- [ ] R27.5 记录总计数、所有失败数和偏差。
- [ ] R27.6 结论 `ON_TRACK` 后提交 Task 27 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 28 — Write the formal M0 Go/No-go report

Plan: [Task 28](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-28-write-the-formal-m0-gono-go-report)

目标：把所有 MUST 和 V0-V6 证据映射成机械化 GO、NO-GO 或 BLOCKED 结论。

主要证据：每个规范要求都有 evidence row，报告完整性命令退出 0。

**Execute**

- [ ] E28.1 确认 Task 27 状态与实际结论一致，记录 start commit。
- [ ] E28.2 阅读 Plan Task 28 的 6 个 Step 和 Operational SPEC 全文。
- [ ] E28.3 创建逐项 requirement/evidence/result 表，区分 observed/derived/conclusion。
- [ ] E28.4 按机械规则选择 verdict，不用多数通过替代全部通过。
- [ ] E28.5 运行报告完整性检查，明确 Toolkit removal 仍未授权。
- [ ] E28.6 按 Plan 提交 verdict commit。

**Review**

- [ ] R28.1 重新阅读 Plan Task 28 和 SPEC GO/BLOCKED/NO-GO 定义。
- [ ] R28.2 逐行抽查 evidence path/command 是否真实存在且支持结论。
- [ ] R28.3 fresh 运行完整性命令并核对没有遗漏失败。
- [ ] R28.4 核对结论没有把推断写成观测事实。
- [ ] R28.5 记录任何证据空洞或偏差；有空洞时不得 `ON_TRACK`。
- [ ] R28.6 结论 `ON_TRACK` 后提交 Task 28 alignment review。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

### Task 29 — Final repository and reproducibility audit

Plan: [Task 29](docs/superpowers/plans/2026-08-01-m0-driver-only-agent-plan.md#task-29-final-repository-and-reproducibility-audit)

目标：fresh 重跑允许的最高 tier，检查 forbidden references、仓库卫生和复现文档。

主要证据：Debug/Release V2 fresh pass，最高授权真实 tier fresh 结果，Git clean，无 out/reports tracked。

**Execute**

- [ ] E29.1 确认 Task 28 为 `ON_TRACK`，记录 start commit。
- [ ] E29.2 阅读 Plan Task 29 的 6 个 Step 和最终 completion criteria。
- [ ] E29.3 fresh 运行 Debug/Release V2 和结论允许的最高真实 tier。
- [ ] E29.4 扫描 forbidden references、diff、status 和 tracked artifacts。
- [ ] E29.5 更新 README/ledger 的精确复现命令。
- [ ] E29.6 按 Plan 提交 reproducibility handoff implementation commit。

**Review**

- [ ] R29.1 重新阅读 Plan Task 29、failure tree 和 completion criteria。
- [ ] R29.2 核对最终 Git diff 仅包含批准内容，没有 build/report artifacts。
- [ ] R29.3 fresh 重跑最终声明所依赖的全部验证命令。
- [ ] R29.4 核对 README 未暗示 M0 是产品或已授权卸载 Toolkit。
- [ ] R29.5 记录最终 branch、commit、verdict、测试计数和偏差。
- [ ] R29.6 结论 `ON_TRACK` 后提交 Task 29 alignment review，并把总进度表全部终结。

Review Record：Executor `Not recorded`; Start `Not recorded`; Implementation `Not recorded`; Evidence `Not recorded`; Deviations `Not recorded`; Verdict `Not recorded`.

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
